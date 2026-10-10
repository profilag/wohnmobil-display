#include "bsp_ws_lcd7.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bsp";

static i2c_master_bus_handle_t s_i2c_bus;
static ch422g_handle_t s_expander;
static aw9523b_handle_t s_aw9523b;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static lv_display_t *s_disp;
static lv_indev_t *s_indev;

static bool s_backlight_on;
/* Zeitpunkt, ab dem das Aufwecken durch Berührung wieder erlaubt ist. */
static int64_t s_wake_allowed_after_us;
static bool s_output_state[BSP_OUTPUT_MAX];

/* -------------------------------------------------------------------------- */
/*  I2C                                                                       */
/* -------------------------------------------------------------------------- */

static esp_err_t bsp_i2c_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = BSP_I2C_NUM,
        .sda_io_num = BSP_I2C_PIN_SDA,
        .scl_io_num = BSP_I2C_PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &s_i2c_bus), TAG, "I2C-Bus");
    ESP_LOGI(TAG, "I2C%d bereit (SCL=%d, SDA=%d, %d kHz)", BSP_I2C_NUM, BSP_I2C_PIN_SCL,
             BSP_I2C_PIN_SDA, BSP_I2C_CLK_HZ / 1000);
    return ESP_OK;
}

void bsp_i2c_scan(void)
{
    if (!s_i2c_bus) {
        ESP_LOGW(TAG, "I2C-Bus nicht initialisiert");
        return;
    }
    ESP_LOGI(TAG, "I2C-Scan …");

    /* Der CH422G dekodiert seine Adresse nur unvollständig und quittiert
     * deshalb im gesamten Bereich 0x20…0x3F, nicht nur auf seinen vier
     * dokumentierten Adressen. Diese Treffer einzeln aufzulisten wäre nur Lärm —
     * und ein eigener Baustein in diesem Bereich (etwa ein SSD1306 auf 0x3C)
     * wäre vom CH422G ohnehin nicht zu unterscheiden. Darum den Bereich
     * zusammengefasst melden und darauf hinweisen. */
    int ch422g_aliases = 0;
    for (uint8_t addr = 0x20; addr <= 0x3F; addr++) {
        if (i2c_master_probe(s_i2c_bus, addr, 50) == ESP_OK) {
            ch422g_aliases++;
        }
    }
    if (ch422g_aliases > 0) {
        ESP_LOGI(TAG, "  0x20…0x3F: CH422G (%d Adressen, unvollständige Dekodierung — "
                      "eigene Bausteine in diesem Bereich sind hier nicht unterscheidbar)",
                 ch422g_aliases);
    }

    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        if (addr >= 0x20 && addr <= 0x3F) {
            continue; /* oben schon behandelt */
        }
        if (i2c_master_probe(s_i2c_bus, addr, 50) == ESP_OK) {
            const char *hint = "";
            switch (addr) {
            case 0x14: case 0x5D: hint = " — GT911 Touch"; break;
            case 0x58: case 0x59: case 0x5A: case 0x5B: hint = " — AW9523B"; break;
            default: hint = " — unbekannt"; break;
            }
            ESP_LOGI(TAG, "  0x%02X%s", addr, hint);
        }
    }
    ESP_LOGI(TAG, "I2C-Scan fertig");
}

/* -------------------------------------------------------------------------- */
/*  Reset-Sequenzen über den CH422G                                           */
/* -------------------------------------------------------------------------- */

static esp_err_t bsp_lcd_reset(void)
{
    ESP_RETURN_ON_ERROR(ch422g_set_level(s_expander, CH422G_PIN_LCD_RST, false), TAG, "LCD_RST low");
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_RETURN_ON_ERROR(ch422g_set_level(s_expander, CH422G_PIN_LCD_RST, true), TAG, "LCD_RST high");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

/**
 * Der GT911 wählt seine I2C-Adresse anhand des INT-Pegels beim Verlassen des
 * Resets: INT low -> 0x5D. Deshalb muss INT während des Reset-Pulses vom ESP32
 * getrieben und danach wieder freigegeben werden (der Touch-Treiber
 * konfiguriert ihn anschließend als Interrupt-Eingang).
 */
static esp_err_t bsp_touch_reset(void)
{
    const gpio_config_t int_out = {
        .pin_bit_mask = 1ULL << BSP_TOUCH_PIN_INT,
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&int_out), TAG, "INT als Ausgang");
    gpio_set_level(BSP_TOUCH_PIN_INT, 0);
    vTaskDelay(pdMS_TO_TICKS(10));

    ESP_RETURN_ON_ERROR(ch422g_set_level(s_expander, CH422G_PIN_TP_RST, false), TAG, "TP_RST low");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(ch422g_set_level(s_expander, CH422G_PIN_TP_RST, true), TAG, "TP_RST high");
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_RETURN_ON_ERROR(gpio_reset_pin(BSP_TOUCH_PIN_INT), TAG, "INT freigeben");
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  RGB-Panel                                                                 */
/* -------------------------------------------------------------------------- */

static esp_err_t bsp_display_init(bool avoid_tearing)
{
    const esp_lcd_rgb_panel_config_t panel_cfg = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .timings = {
            .pclk_hz = BSP_LCD_PIXEL_CLOCK_HZ,
            .h_res = BSP_LCD_H_RES,
            .v_res = BSP_LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 8,
            .vsync_front_porch = 8,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .bits_per_pixel = 16,
        .num_fbs = avoid_tearing ? 2 : 1,
        /* Bounce Buffer in internem RAM: die DMA liest nicht direkt aus dem
         * PSRAM-Framebuffer, was das Verrutschen des Bildes ("screen drift")
         * auf dem ESP32-S3 zuverlässig verhindert.
         *
         * 24 Zeilen: Der Puffer ist der zeitliche Vorlauf, den das Nachfüllen
         * hat, bevor die DMA leer läuft. Kommt ein anderer Interrupt
         * dazwischen und der Nachschub verspätet sich, gibt die DMA alte Daten
         * aus — auf dem Schirm sind das die "Geisterlinien": ein kurzes Stück
         * Bild, das um ein paar Zeilen verrutscht ist und zappelt.
         *
         * **16 Zeilen — und nicht mehr.** Größer klingt naheliegend, ist es
         * aber nicht, und der Versuch ist teuer bezahlt worden:
         *
         *   - 24 Zeilen: zunächst weniger Geisterlinien, dann ein dauerhaft
         *     um rund 20 Zeilen vorgelaufenes Bild — der Kopf erschien unten.
         *   - 32 Zeilen: das Bild sprang schnell auf und ab.
         *
         * Der Grund ist, dass Vergrößern das Verhältnis von Nachfüllzeit zu
         * Leerlaufzeit gar nicht verbessert — es verlängert nur beides. Jede
         * Nachfüllung wird zu einem längeren Block, in dem der Interrupt am
         * Stück aus dem PSRAM kopiert, und die Korrektur, die
         * `LCD_RGB_RESTART_IN_VSYNC` bei jedem Bildwechsel vornimmt, greift
         * erst bei einem Versatz von mehr als zwei Puffern. Je größer der
         * Puffer, desto größer der Versatz, den sie stehen lässt.
         *
         * 16 Zeilen sind der Stand, der über Tage ruhig lief. Kostet internen
         * RAM: zwei Puffer à 800 × 16 × 2 Byte, gut 51 kB. */
        .bounce_buffer_size_px = BSP_LCD_H_RES * 16,
        .dma_burst_size = 64,
        .hsync_gpio_num = BSP_LCD_PIN_HSYNC,
        .vsync_gpio_num = BSP_LCD_PIN_VSYNC,
        .de_gpio_num = BSP_LCD_PIN_DE,
        .pclk_gpio_num = BSP_LCD_PIN_PCLK,
        .disp_gpio_num = BSP_LCD_PIN_DISP,
        .data_gpio_nums = {
            BSP_LCD_PIN_DATA0,  BSP_LCD_PIN_DATA1,  BSP_LCD_PIN_DATA2,  BSP_LCD_PIN_DATA3,
            BSP_LCD_PIN_DATA4,  BSP_LCD_PIN_DATA5,  BSP_LCD_PIN_DATA6,  BSP_LCD_PIN_DATA7,
            BSP_LCD_PIN_DATA8,  BSP_LCD_PIN_DATA9,  BSP_LCD_PIN_DATA10, BSP_LCD_PIN_DATA11,
            BSP_LCD_PIN_DATA12, BSP_LCD_PIN_DATA13, BSP_LCD_PIN_DATA14, BSP_LCD_PIN_DATA15,
        },
        .flags = {
            .fb_in_psram = true,
        },
    };

    ESP_RETURN_ON_ERROR(esp_lcd_new_rgb_panel(&panel_cfg, &s_panel), TAG, "RGB-Panel anlegen");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "Panel-Reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "Panel-Init");
    ESP_LOGI(TAG, "RGB-Panel %dx%d @ %d MHz aktiv", BSP_LCD_H_RES, BSP_LCD_V_RES,
             BSP_LCD_PIXEL_CLOCK_HZ / 1000000);
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  Touch                                                                     */
/* -------------------------------------------------------------------------- */

static esp_err_t bsp_touch_init(void)
{
    ESP_RETURN_ON_ERROR(bsp_touch_reset(), TAG, "Touch-Reset");

    esp_lcd_panel_io_handle_t tp_io = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &tp_io), TAG, "Touch-IO");

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = BSP_LCD_H_RES,
        .y_max = BSP_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC, /* liegt am CH422G, oben schon behandelt */
        .int_gpio_num = BSP_TOUCH_PIN_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_gt911(tp_io, &tp_cfg, &s_touch), TAG, "GT911");
    ESP_LOGI(TAG, "GT911-Touch bereit");
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  Hintergrundbeleuchtung und Display-Schlaf                                 */
/* -------------------------------------------------------------------------- */

esp_err_t bsp_backlight_set(bool on)
{
    ESP_RETURN_ON_FALSE(s_expander, ESP_ERR_INVALID_STATE, TAG, "CH422G nicht initialisiert");
    ESP_RETURN_ON_ERROR(ch422g_set_level(s_expander, CH422G_PIN_LCD_BL, on), TAG, "Backlight");
    s_backlight_on = on;
    return ESP_OK;
}

bool bsp_backlight_is_on(void)
{
    return s_backlight_on;
}

esp_err_t bsp_display_sleep(uint32_t lockout_ms)
{
    if (lockout_ms == 0) {
        lockout_ms = BSP_DISPLAY_WAKE_LOCKOUT_MS;
    }
    ESP_RETURN_ON_ERROR(bsp_backlight_set(false), TAG, "Display abschalten");
    s_wake_allowed_after_us = esp_timer_get_time() + (int64_t)lockout_ms * 1000;
    ESP_LOGI(TAG, "Display aus, Aufwecken gesperrt für %" PRIu32 " ms", lockout_ms);
    return ESP_OK;
}

esp_err_t bsp_display_wake(void)
{
    s_wake_allowed_after_us = 0;
    if (s_backlight_on) {
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Display an");
    return bsp_backlight_set(true);
}

/**
 * Läuft im LVGL-Task, sobald der Touch gedrückt wird. Der Mutex ist hier schon
 * gehalten — deshalb kein bsp_display_lock().
 */
static void bsp_touch_wake_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_backlight_on) {
        return;
    }

    /* Bei dunklem Display gehört die gesamte Touchfläche der Beleuchtung: die
     * Berührung wird hier verbraucht und erreicht kein Bedienelement mehr.
     * Sonst schaltet der blinde Griff nebenbei eine Last oder den Pegel-Test.
     * stop_processing unterdrückt das PRESSED am Widget, wait_release den Rest
     * der Geste bis zum Loslassen — also auch CLICKED und VALUE_CHANGED. */
    lv_indev_stop_processing(s_indev);
    lv_indev_wait_release(s_indev);

    if (esp_timer_get_time() < s_wake_allowed_after_us) {
        /* Noch in der Sperrzeit: das ist das Nachprellen derselben Berührung,
         * mit der das Display abgeschaltet wurde. */
        return;
    }
    bsp_display_wake();
}

/* -------------------------------------------------------------------------- */
/*  Lastausgänge auf dem AW9523B                                              */
/* -------------------------------------------------------------------------- */

esp_err_t bsp_output_init(void)
{
    ESP_RETURN_ON_FALSE(s_aw9523b, ESP_ERR_INVALID_STATE, TAG, "AW9523B nicht vorhanden");

    for (bsp_output_t out = BSP_OUTPUT_PA0; out < BSP_OUTPUT_MAX; out++) {
        const uint8_t pin = AW9523B_P0((uint8_t)out);
        /* Der AW9523B startet jedes Pin im GPIO-Modus; explizit setzen, damit ein
         * vorher konfigurierter LED-Modus nicht durchschlägt. */
        ESP_RETURN_ON_ERROR(aw9523b_set_led_mode(s_aw9523b, pin, false), TAG, "PA%d GPIO-Modus", out);
        /* Erst den inaktiven Pegel in das Ausgangsregister, dann auf Ausgang
         * schalten — sonst zappelt die Last beim Hochlauf kurz an. */
        ESP_RETURN_ON_ERROR(aw9523b_set_level(s_aw9523b, pin, BSP_OUTPUT_ACTIVE_LOW ? true : false),
                            TAG, "PA%d Startpegel", out);
        ESP_RETURN_ON_ERROR(aw9523b_set_direction(s_aw9523b, pin, GPIO_MODE_OUTPUT), TAG,
                            "PA%d als Ausgang", out);
        s_output_state[out] = false;
    }
    ESP_LOGI(TAG, "Ausgänge PA0..PA3 bereit (low-aktiv, Open Drain), alle aus");
    return ESP_OK;
}

esp_err_t bsp_output_set(bsp_output_t out, bool on)
{
    ESP_RETURN_ON_FALSE(s_aw9523b, ESP_ERR_INVALID_STATE, TAG, "AW9523B nicht vorhanden");
    ESP_RETURN_ON_FALSE(out < BSP_OUTPUT_MAX, ESP_ERR_INVALID_ARG, TAG, "Ausgang %d ungültig", out);

    const bool level = BSP_OUTPUT_ACTIVE_LOW ? !on : on;
    ESP_RETURN_ON_ERROR(aw9523b_set_level(s_aw9523b, AW9523B_P0((uint8_t)out), level), TAG,
                        "PA%d schalten", out);
    s_output_state[out] = on;
    ESP_LOGI(TAG, "PA%d = %s (Pegel %s)", out, on ? "AN" : "AUS", level ? "high" : "low");
    return ESP_OK;
}

bool bsp_output_get(bsp_output_t out)
{
    return (out < BSP_OUTPUT_MAX) ? s_output_state[out] : false;
}

/* -------------------------------------------------------------------------- */
/*  Eingänge auf Port B des AW9523B                                           */
/* -------------------------------------------------------------------------- */

esp_err_t bsp_input_pb_configure(uint8_t input_mask)
{
    ESP_RETURN_ON_FALSE(s_aw9523b, ESP_ERR_INVALID_STATE, TAG, "AW9523B nicht vorhanden");

    for (uint8_t bit = 0; bit < 8; bit++) {
        if (!(input_mask & (1u << bit))) {
            continue;
        }
        const uint8_t pin = AW9523B_P1(bit);
        ESP_RETURN_ON_ERROR(aw9523b_set_led_mode(s_aw9523b, pin, false), TAG,
                            "PB%u GPIO-Modus", bit);
        ESP_RETURN_ON_ERROR(aw9523b_set_direction(s_aw9523b, pin, GPIO_MODE_INPUT), TAG,
                            "PB%u als Eingang", bit);
    }
    ESP_LOGI(TAG, "Eingänge Port B konfiguriert (Maske 0x%02X)", input_mask);
    return ESP_OK;
}

static bool s_output_pb_state[8];

esp_err_t bsp_output_pb_configure(uint8_t output_mask)
{
    ESP_RETURN_ON_FALSE(s_aw9523b, ESP_ERR_INVALID_STATE, TAG, "AW9523B nicht vorhanden");

    for (uint8_t bit = 0; bit < 8; bit++) {
        if (!(output_mask & (1u << bit))) {
            continue;
        }
        const uint8_t pin = AW9523B_P1(bit);
        ESP_RETURN_ON_ERROR(aw9523b_set_led_mode(s_aw9523b, pin, false), TAG,
                            "PB%u GPIO-Modus", bit);
        /* Erst den Ruhepegel ins Ausgangsregister, dann auf Ausgang schalten —
         * sonst zappelt die Anregung beim Hochlauf kurz an. */
        ESP_RETURN_ON_ERROR(aw9523b_set_level(s_aw9523b, pin, BSP_OUTPUT_ACTIVE_LOW ? true : false),
                            TAG, "PB%u Ruhepegel", bit);
        ESP_RETURN_ON_ERROR(aw9523b_set_direction(s_aw9523b, pin, GPIO_MODE_OUTPUT), TAG,
                            "PB%u als Ausgang", bit);
        s_output_pb_state[bit] = false;
    }
    ESP_LOGI(TAG, "Ausgänge Port B konfiguriert (Maske 0x%02X, low-aktiv, Push-Pull)",
             output_mask);
    return ESP_OK;
}

esp_err_t bsp_output_pb_set(uint8_t bit, bool on)
{
    ESP_RETURN_ON_FALSE(s_aw9523b, ESP_ERR_INVALID_STATE, TAG, "AW9523B nicht vorhanden");
    ESP_RETURN_ON_FALSE(bit < 8, ESP_ERR_INVALID_ARG, TAG, "PB%u ungültig", bit);

    const bool level = BSP_OUTPUT_ACTIVE_LOW ? !on : on;
    ESP_RETURN_ON_ERROR(aw9523b_set_level(s_aw9523b, AW9523B_P1(bit), level), TAG,
                        "PB%u schalten", bit);
    s_output_pb_state[bit] = on;
    return ESP_OK;
}

bool bsp_output_pb_get(uint8_t bit)
{
    return (bit < 8) ? s_output_pb_state[bit] : false;
}

esp_err_t bsp_input_pb_read(uint8_t *out_value)
{
    ESP_RETURN_ON_FALSE(s_aw9523b, ESP_ERR_INVALID_STATE, TAG, "AW9523B nicht vorhanden");
    ESP_RETURN_ON_FALSE(out_value, ESP_ERR_INVALID_ARG, TAG, "kein Zielzeiger");
    return aw9523b_read_reg(s_aw9523b, AW9523B_REG_IN_P1, out_value);
}

/* -------------------------------------------------------------------------- */
/*  LVGL                                                                      */
/* -------------------------------------------------------------------------- */

static esp_err_t bsp_lvgl_init(const bsp_config_t *config)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    if (config->lvgl_task_priority > 0) {
        port_cfg.task_priority = config->lvgl_task_priority;
    }
    if (config->lvgl_task_stack > 0) {
        port_cfg.task_stack = config->lvgl_task_stack;
    }
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    /* Wichtige Kopplung in esp_lvgl_port: der Flush-Callback nimmt für ein
     * RGB-Panel im direct_mode das Semaphor `trans_sem` — angelegt wird das aber
     * ausschließlich im Zweig `avoid_tearing`. direct_mode ohne avoid_tearing
     * läuft deshalb in einen NULL-Zeiger (assert in xQueueSemaphoreTake).
     * Also beides gemeinsam schalten:
     *   avoid_tearing = true  -> direct_mode, LVGL zeichnet in die beiden
     *                            RGB-Framebuffer, kein Tearing
     *   avoid_tearing = false -> klassischer Teilbild-Modus mit eigenen
     *                            Zeichenpuffern (1/10 Bildschirm, doppelt) */
    const bool direct_mode = config->avoid_tearing;
    /*
     * Teilbildpuffer: 10 Zeilen, doppelt.
     *
     * Vorher waren es 48 Zeilen — zwei Puffer zu je 77 kB. Das passt nicht in
     * den internen RAM, und genau dorthin gehören sie (siehe unten).
     *
     * Auch 16 Zeilen waren noch zu viel: Mit 51 kB für die Puffer blieben nur
     * 52 kB internen RAMs übrig, und der Webserver bekam für seine Aufgabe
     * keinen Speicher mehr. 10 Zeilen sind 16 kB je Puffer. LVGL zeichnet dann
     * in mehr Streifen — das kostet ein paar zusätzliche Durchläufe, aber
     * keine Bandbreite, denn die Gesamtfläche bleibt dieselbe.
     */
    const uint32_t buffer_size = direct_mode ? (BSP_LCD_H_RES * BSP_LCD_V_RES)
                                             : (BSP_LCD_H_RES * 10);

    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = s_panel,
        .buffer_size = buffer_size,
        .double_buffer = !direct_mode,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            /*
             * Wohin LVGL zeichnet — und das ist die Stelle, an der sich
             * Bildruhe entscheidet.
             *
             * Im `direct_mode` zeichnet LVGL unmittelbar in die beiden
             * Framebuffer im PSRAM und überträgt jede geänderte Fläche
             * anschließend noch in den zweiten. Jeder Bildaufbau geht damit
             * zweimal über denselben Bus, aus dem das Panel ununterbrochen
             * sein Bild liest. Bei großen Änderungen — ein Dialog, eine
             * Bildschirmtastatur — reicht die Bandbreite nicht, der
             * Bounce-Puffer läuft leer, und es entstehen die verrutschten
             * Geisterzeilen am linken Rand.
             *
             * Ohne `direct_mode` zeichnet LVGL in kleine Streifen im
             * **internen** RAM und schreibt sie danach ein einziges Mal in den
             * Framebuffer. Das Zeichnen selbst — viele Lese-Änder-Schreib-
             * Zugriffe fürs Mischen — verlässt den PSRAM vollständig.
             *
             * Der Preis ist Tearing: Ohne zweiten Framebuffer kann ein
             * Bildaufbau mitten im Auslesen sichtbar werden. Bei einer
             * Anzeige, die überwiegend stillsteht, ist das der bessere Handel.
             */
            .buff_dma = !direct_mode,
            .buff_spiram = direct_mode,
            .swap_bytes = false,
            .direct_mode = direct_mode,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = {
            .bb_mode = true,
            .avoid_tearing = config->avoid_tearing,
        },
    };

    s_disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    ESP_RETURN_ON_FALSE(s_disp, ESP_FAIL, TAG, "lvgl_port_add_disp_rgb fehlgeschlagen");

    if (s_touch) {
        const lvgl_port_touch_cfg_t touch_cfg = {
            .disp = s_disp,
            .handle = s_touch,
        };
        s_indev = lvgl_port_add_touch(&touch_cfg);
        ESP_RETURN_ON_FALSE(s_indev, ESP_FAIL, TAG, "lvgl_port_add_touch fehlgeschlagen");

        /* Wake-on-Touch am Eingabegerät statt an einem Widget: so weckt jede
         * Berührung des Bildschirms auf, auch neben allen Bedienelementen —
         * bei dunklem Display sieht man ja nicht, wo man hindrückt. */
        lv_indev_add_event_cb(s_indev, bsp_touch_wake_event_cb, LV_EVENT_PRESSED, NULL);
    }

    ESP_LOGI(TAG, "LVGL %d.%d.%d bereit", lv_version_major(), lv_version_minor(),
             lv_version_patch());
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  Öffentliche API                                                           */
/* -------------------------------------------------------------------------- */

esp_err_t bsp_init(const bsp_config_t *config)
{
    ESP_RETURN_ON_FALSE(config, ESP_ERR_INVALID_ARG, TAG, "keine Konfiguration");

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C");
    ESP_RETURN_ON_ERROR(ch422g_new(s_i2c_bus, &s_expander), TAG, "CH422G");

    /* EXIO5 wählt zwischen USB- und CAN-Betrieb der Haupt-USB-C-Buchse:
     * low = USB (nativer USB-Serial/JTAG, also Flashen und Log), high = CAN.
     * Wer den CAN-Transceiver benutzen will, setzt das hier auf true. */
    ESP_RETURN_ON_ERROR(ch422g_set_level(s_expander, CH422G_PIN_USB_SEL, false), TAG, "USB_SEL");
    /* SD-Karte zunächst deselektieren. */
    ESP_RETURN_ON_ERROR(ch422g_set_level(s_expander, CH422G_PIN_SD_CS, true), TAG, "SD_CS");

    ESP_RETURN_ON_ERROR(bsp_lcd_reset(), TAG, "LCD-Reset");
    ESP_RETURN_ON_ERROR(bsp_display_init(config->avoid_tearing), TAG, "Display");
    ESP_RETURN_ON_ERROR(bsp_touch_init(), TAG, "Touch");
    ESP_RETURN_ON_ERROR(bsp_lvgl_init(config), TAG, "LVGL");

    if (config->aw9523b.enable) {
        esp_err_t ret = aw9523b_new(s_i2c_bus, &config->aw9523b.config, &s_aw9523b);
        if (ret != ESP_OK) {
            /* Der externe Expander ist optional — ein fehlendes Modul soll das
             * Display nicht mit in den Abgrund ziehen. */
            ESP_LOGW(TAG, "AW9523B nicht gefunden (%s) — läuft ohne externen Expander",
                     esp_err_to_name(ret));
            s_aw9523b = NULL;
        } else {
            ESP_RETURN_ON_ERROR(bsp_output_init(), TAG, "Lastausgänge");
        }
    }

    /* Backlight zuletzt an, damit kein halb initialisiertes Bild sichtbar wird. */
    ESP_RETURN_ON_ERROR(bsp_backlight_set(true), TAG, "Backlight");
    ESP_LOGI(TAG, "Board-Initialisierung abgeschlossen");
    return ESP_OK;
}

i2c_master_bus_handle_t bsp_i2c_get_handle(void)   { return s_i2c_bus; }
ch422g_handle_t         bsp_expander_get_handle(void) { return s_expander; }
aw9523b_handle_t        bsp_aw9523b_get_handle(void)  { return s_aw9523b; }
lv_display_t           *bsp_display_get_handle(void)  { return s_disp; }

bool bsp_display_lock(uint32_t timeout_ms) { return lvgl_port_lock(timeout_ms); }
void bsp_display_unlock(void)              { lvgl_port_unlock(); }
