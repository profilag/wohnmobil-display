/**
 * @file bsp_ws_lcd7.h
 * @brief Board Support Package für das Waveshare ESP32-S3-Touch-LCD-7.
 *
 * Bringt in einem Aufruf (`bsp_init`) folgendes hoch:
 *   - I2C-Master-Bus 0 (SCL=GPIO9, SDA=GPIO8, 400 kHz) — gemeinsam genutzt von
 *     CH422G (Board), GT911 (Touch) und dem externen AW9523B.
 *   - CH422G-Expander, danach LCD-Reset und Touch-Reset in korrekter Reihenfolge
 *   - RGB-Panel ST7262, 800x480, RGB565, 16 Bit parallel, doppelter Framebuffer
 *     in PSRAM mit Bounce Buffer (verhindert das bekannte "Screen Drift")
 *   - GT911-Touch inklusive Adress-Auswahl über den INT-Pin
 *   - LVGL 9 über esp_lvgl_port
 *
 * Die Pin-Belegung entspricht dem Schaltplan des Boards und ist gegen die
 * funktionierende Arduino-Konfiguration (ESP32_Display_Panel) verifiziert.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "aw9523b.h"
#include "ch422g.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 *  Panel
 * ------------------------------------------------------------------------- */
#define BSP_LCD_H_RES               800
#define BSP_LCD_V_RES               480
/**
 * Pixeltakt. 12 statt der 16 MHz aus der Waveshare-Vorlage.
 *
 * Der Takt bestimmt, wie schnell die DMA Daten liefern **muss**. Bei 16 MHz
 * bleibt zwischen zwei Nachfüllungen des Bounce-Buffers so wenig Zeit, dass
 * ein dazwischenkommender Interrupt — seit BLE dauerhaft mitläuft, keine
 * Seltenheit — den Puffer leerlaufen lässt. Sichtbar wird das als kurzes,
 * zappelndes Stück am linken Rand, das ein paar Zeilen zu tief steht: Die DMA
 * hing zurück, und `LCD_RGB_RESTART_IN_VSYNC` setzt sie im nächsten Bild
 * wieder auf.
 *
 * **14 MHz ist die Untergrenze — 12 MHz waren zu wenig.** Der Versuch, den
 * Takt weiter zu senken, ergab rund 29 Hz Bildwiederholrate, und damit
 * verliert der ST7262 den vertikalen Gleichlauf: Das Bild hüpft schnell auf
 * und ab. Das Panel braucht seine 30 Hz, egal wie entspannt die DMA damit
 * wäre. Wer hier weiter drehen will, muss nach oben drehen, nicht nach unten.
 *
 * 14 MHz geben gegenüber der Vorlage rund 12 % mehr Zeitbudget je Nachfüllung
 * und entlasten die PSRAM-Bandbreite, bei etwa 34 Hz Bildwiederholrate.
 */
#define BSP_LCD_PIXEL_CLOCK_HZ      (14 * 1000 * 1000)

#define BSP_LCD_PIN_HSYNC           46
#define BSP_LCD_PIN_VSYNC           3
#define BSP_LCD_PIN_DE              5
#define BSP_LCD_PIN_PCLK            7
#define BSP_LCD_PIN_DISP            (-1)

/* 16-Bit-RGB565: D0..D4 = Blau, D5..D10 = Grün, D11..D15 = Rot */
#define BSP_LCD_PIN_DATA0           14
#define BSP_LCD_PIN_DATA1           38
#define BSP_LCD_PIN_DATA2           18
#define BSP_LCD_PIN_DATA3           17
#define BSP_LCD_PIN_DATA4           10
#define BSP_LCD_PIN_DATA5           39
#define BSP_LCD_PIN_DATA6           0
#define BSP_LCD_PIN_DATA7           45
#define BSP_LCD_PIN_DATA8           48
#define BSP_LCD_PIN_DATA9           47
#define BSP_LCD_PIN_DATA10          21
#define BSP_LCD_PIN_DATA11          1
#define BSP_LCD_PIN_DATA12          2
#define BSP_LCD_PIN_DATA13          42
#define BSP_LCD_PIN_DATA14          41
#define BSP_LCD_PIN_DATA15          40

/* ---------------------------------------------------------------------------
 *  I2C (gemeinsamer Bus für CH422G, GT911 und AW9523B)
 * ------------------------------------------------------------------------- */
#define BSP_I2C_NUM                 I2C_NUM_0
#define BSP_I2C_PIN_SCL             9
#define BSP_I2C_PIN_SDA             8
#define BSP_I2C_CLK_HZ              400000

/** Touch-Interrupt. Wird beim Reset zur Adress-Auswahl des GT911 benutzt. */
#define BSP_TOUCH_PIN_INT           4

/* ---------------------------------------------------------------------------
 *  Weitere freie Pins des Boards (Stiftleiste) — nützlich für den AW9523B
 * ------------------------------------------------------------------------- */
#define BSP_PIN_FREE_GPIO_15        15  /*!< am Board als GPIO/UART verfügbar */
#define BSP_PIN_FREE_GPIO_16        16
#define BSP_PIN_FREE_GPIO_43        43  /*!< UART0 TX  */
#define BSP_PIN_FREE_GPIO_44        44  /*!< UART0 RX  */

typedef struct {
    /** Konfiguration für den externen AW9523B. `enable = false` überspringt ihn. */
    struct {
        bool enable;
        aw9523b_config_t config;
    } aw9523b;
    /**
     * LVGL-Task: Priorität und Stack. 0 = Vorgabewerte von esp_lvgl_port.
     *
     * @note Der Stack ist **nicht** großzügig bemessen, sondern nötig. LVGLs
     *       Software-Renderer legt seine Zwischenwerte auf dem Stack ab, und
     *       Verläufe, Schatten und abgerundete Kanten brauchen dort ein
     *       Vielfaches dessen, was eine einfarbige Fläche kostet. Mit 8 kB —
     *       dem Wert für flächige Darstellung — stürzt der Task beim Aufbau
     *       einer Seite mit Verläufen ab: *„A stack overflow in task taskLVGL
     *       has been detected"*, gefolgt von einem Neustart. Das sieht am
     *       Display wie ein Flackern aus und ist keins.
     */
    int lvgl_task_priority;
    int lvgl_task_stack;
    /**
     * true  = zwei Framebuffer im PSRAM (je 768 kB), LVGL zeichnet direkt hinein.
     *         Kein Tearing, dafür etwas weniger Framerate. Empfohlene Vorgabe.
     * false = Teilbild-Modus mit eigenen Zeichenpuffern, sparsamer im PSRAM.
     *
     * Die Option steuert in esp_lvgl_port zwangsläufig auch `direct_mode` mit —
     * die beiden sind dort nicht unabhängig voneinander benutzbar.
     */
    bool avoid_tearing;
} bsp_config_t;

#define BSP_CONFIG_DEFAULT()                            \
    {                                                   \
        .aw9523b = {                                    \
            .enable = true,                             \
            .config = {                                 \
                .i2c_address  = AW9523B_ADDR_00,        \
                .int_gpio     = GPIO_NUM_NC,            \
                .reset_gpio   = GPIO_NUM_NC,            \
                /* Port A bleibt Open Drain — die Lastausgänge PA0/PA1 sind    \
                 * low-aktiv verdrahtet, siehe BSP_OUTPUT_ACTIVE_LOW. Das ist  \
                 * auch die Reset-Vorgabe des AW9523B.                         \
                 * Für LED-Betrieb mit Konstantstrom ist das ebenfalls richtig. \
                 * Nur bei echten Push-Pull-Signalen auf true stellen.         \
                 */                                     \
                .p0_push_pull = false,                  \
                .imax         = AW9523B_IMAX_37MA,      \
            },                                          \
        },                                              \
        .lvgl_task_priority = 4,                        \
        .lvgl_task_stack    = 24576,                    \
        .avoid_tearing      = false,                    \
    }

/**
 * @brief Komplettes Board initialisieren: I2C, Expander, Display, Touch, LVGL.
 *
 * Danach ist LVGL lauffähig; UI-Aufrufe müssen mit bsp_display_lock() /
 * bsp_display_unlock() geklammert werden.
 */
esp_err_t bsp_init(const bsp_config_t *config);

/** @brief Handle des gemeinsamen I2C-Busses (für eigene Sensoren am Bus). */
i2c_master_bus_handle_t bsp_i2c_get_handle(void);

/** @brief Handle des On-Board-CH422G. */
ch422g_handle_t bsp_expander_get_handle(void);

/** @brief Handle des externen AW9523B, oder NULL wenn nicht initialisiert. */
aw9523b_handle_t bsp_aw9523b_get_handle(void);

/** @brief LVGL-Display-Handle. */
lv_display_t *bsp_display_get_handle(void);

/* ---------------------------------------------------------------------------
 *  Hintergrundbeleuchtung und Display-Schlaf
 * ------------------------------------------------------------------------- */

/** @brief Backlight über den CH422G ein-/ausschalten (kein Dimmen möglich). */
esp_err_t bsp_backlight_set(bool on);

/** @brief Aktueller Zustand der Hintergrundbeleuchtung. */
bool bsp_backlight_is_on(void);

/**
 * @brief Display schlafen legen: Backlight aus, Wiedereinschalten gesperrt.
 *
 * Der GT911 prellt beim Berühren nach, und ein "Display aus"-Button würde sonst
 * durch dieselbe Berührung sofort wieder aufwecken. Deshalb blockiert
 * `lockout_ms` das Aufwachen für diese Zeit. Danach weckt jede Berührung des
 * Bildschirms die Beleuchtung automatisch wieder auf.
 *
 * Solange das Display dunkel ist, dient die gesamte Touchfläche allein dem
 * Einschalten: die weckende Berührung wird verbraucht und löst kein
 * Bedienelement darunter aus.
 *
 * @param lockout_ms Sperrzeit in ms (0 nutzt BSP_DISPLAY_WAKE_LOCKOUT_MS)
 */
esp_err_t bsp_display_sleep(uint32_t lockout_ms);

/** @brief Display sofort aufwecken, unabhängig von der Sperrzeit. */
esp_err_t bsp_display_wake(void);

/** Vorgabe-Sperrzeit gegen das Nachprellen des Touch. */
#define BSP_DISPLAY_WAKE_LOCKOUT_MS 2000

/* ---------------------------------------------------------------------------
 *  Lastausgänge auf dem AW9523B-Erweiterungsboard
 * ------------------------------------------------------------------------- */

/**
 * Die benutzten Ausgänge liegen auf Port A des AW9523B — in der Registerwelt
 * des Bausteins ist das Port 0, deshalb hier die Umrechnung.
 */
typedef enum {
    BSP_OUTPUT_PA0 = 0, /*!< AW9523B PA0 — Relais Wasserversorgung     */
    BSP_OUTPUT_PA1 = 1, /*!< AW9523B PA1 — Relais Heizung Abwassertank */
    /* PA2 und PA3 trugen früher die Anregung der Messkreise. Die liegt
     * inzwischen auf Port B, bei ihren Sensoren — beide sind wieder frei. */
    BSP_OUTPUT_PA2 = 2,
    BSP_OUTPUT_PA3 = 3,
    BSP_OUTPUT_MAX
} bsp_output_t;

/**
 * Die Ausgänge schalten **low-aktiv**: der Ausgang zieht die Last nach Masse,
 * "aus" bedeutet also High-Pegel. Port A des AW9523B bleibt dafür bewusst im
 * Open-Drain-Betrieb (Reset-Vorgabe des Bausteins) — genau wie in der
 * bisherigen Firmware. Auf Push-Pull umzustellen würde gegen einen etwaigen
 * externen Pull-up arbeiten.
 */
#define BSP_OUTPUT_ACTIVE_LOW 1

/** @brief PA0..PA3 als Ausgänge konfigurieren, alle aus. */
esp_err_t bsp_output_init(void);

/** @brief Ausgang schalten. `on = true` heißt Last an (Pegel low). */
esp_err_t bsp_output_set(bsp_output_t out, bool on);

/** @brief Letzter gesetzter Zustand eines Ausgangs. */
bool bsp_output_get(bsp_output_t out);

/* ---------------------------------------------------------------------------
 *  Eingänge auf Port B des AW9523B
 * ------------------------------------------------------------------------- */

/**
 * @brief PB-Pins als Eingänge konfigurieren.
 *
 * @param input_mask Bit n = 1 -> PBn wird Eingang. Nicht gesetzte Bits bleiben
 *                   unverändert.
 *
 * @note Der AW9523B hat **keine internen Pull-up-Widerstände**. Ein
 *       unbeschalteter Eingang liefert deshalb keinen definierten Pegel —
 *       für Schalter, Taster oder Elektroden ist ein externer Pull-up
 *       (typisch 10 kΩ nach 3V3) Pflicht.
 */
esp_err_t bsp_input_pb_configure(uint8_t input_mask);

/** @brief Alle acht PB-Pins in einem Zugriff lesen. Bit n = PBn. */
esp_err_t bsp_input_pb_read(uint8_t *out_value);

/* ---------------------------------------------------------------------------
 *  Ausgänge auf Port B des AW9523B
 * ------------------------------------------------------------------------- */

/**
 * @brief PB-Pins als Ausgänge konfigurieren, alle im Ruhezustand.
 *
 * Gedacht für die Anregung der Messelektroden, die auf demselben Port liegt
 * wie ihre Sensoren — ein Kabelbaum statt zweier.
 *
 * @note **Port 1 des AW9523B ist immer Push-Pull**, anders als Port 0. Ein
 *       Ausgang treibt hier also aktiv auf High, statt nur nach Masse zu
 *       ziehen. Für die Anregung ist das richtig: Im Ruhezustand liegt die
 *       Bezugselektrode damit auf demselben Pegel wie die Pull-ups der
 *       Sensoren, es fließt kein Strom und es gibt keine Elektrolyse.
 *
 * @param output_mask Bit n = 1 -> PBn wird Ausgang. Nicht gesetzte Bits
 *                    bleiben unverändert.
 */
esp_err_t bsp_output_pb_configure(uint8_t output_mask);

/**
 * @brief PB-Ausgang schalten. `on = true` heißt aktiv (Pegel low).
 *
 * Gleiche Polarität wie die Lastausgänge auf Port A, damit im ganzen Projekt
 * dieselbe Vorstellung gilt: „an" bedeutet Low.
 */
esp_err_t bsp_output_pb_set(uint8_t bit, bool on);

/** @brief Letzter gesetzter Zustand eines PB-Ausgangs. */
bool bsp_output_pb_get(uint8_t bit);

/**
 * @brief LVGL-Mutex nehmen, bevor lv_*-Funktionen aufgerufen werden.
 *
 * @param timeout_ms 0 = unbegrenzt warten
 * @return true wenn der Mutex genommen wurde
 */
bool bsp_display_lock(uint32_t timeout_ms);

/** @brief LVGL-Mutex freigeben. */
void bsp_display_unlock(void);

/** @brief Alle Adressen am I2C-Bus scannen und ins Log schreiben. */
void bsp_i2c_scan(void);

#ifdef __cplusplus
}
#endif
