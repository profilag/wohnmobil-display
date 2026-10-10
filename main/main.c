/**
 * Waveshare ESP32-S3-Touch-LCD-7  —  Bedienoberfläche Wohnmobil
 *
 * Zwei Seiten:
 *
 *   "Betrieb" — alles, was im Alltag gebraucht wird:
 *      - Frischwasserstand über drei Elektroden, vierstufig. Der Messkreis wird
 *        nur für die Dauer einer Messung bestromt, ausgelöst per Knopfdruck
 *        (siehe water_level.h zur Elektrolyse).
 *      - Abwassertank über eine einzige Elektrode "voll", von allein geprüft
 *        alle WASTE_CHECK_INTERVAL_MS und zusätzlich auf Knopfdruck.
 *      - Die beiden Relais: PA0 Wasserversorgung aus dem Frischwassertank,
 *        PA1 Heizung des Abwassertanks.
 *
 *   "Test" — Rohpegel beider Messkreise mit dauerhafter Anregung. Bewusst auf
 *      einer eigenen Seite: der Testmodus bestromt die Elektroden dauerhaft und
 *      fördert Elektrolyse, das gehört nicht neben die täglichen Bedienelemente.
 *
 *   "Funk" — BLE-Scanner: was sendet am Einbauort, wie stark kommt es an, und
 *      ist ein Victron-Gerät oder ein Akku-BMS darunter. Vorarbeit für eine
 *      spätere Anbindung, siehe ble_scan.h.
 */

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>

#include "ble_probe.h"
#include "ble_scan.h"
#include "bsp_ws_lcd7.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "fonts_ui.h"
#include "freertos/FreeRTOS.h"
#include "led_ble.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "batt_history.h"
#include "victron_ble.h"
#include "web.h"
#include "waste_level.h"
#include "water_level.h"

static const char *TAG = "app";

/* Farben */
#define COL_BG          0x101418
#define COL_CARD        0x1c2128
#define COL_TEXT        0xffffff
#define COL_TEXT_DIM    0x8b949e
#define COL_WATER       0x2f81f7
#define COL_EMPTY       0x30363d
#define COL_WARN        0xd29922
#define COL_ALERT       0xda3633
/* Knopfgrund und Akzent für den eingerasteten Zustand. */
#define COL_BTN         0x2b333c
#define COL_ACCENT      0x2f81f7

/* Aufteilung der 800 px Breite: links schmaler, rechts etwas breiter, weil dort
 * die drei Bedienknöpfe nebeneinander passen müssen. */
#define COL_LEFT_W      370
#define COL_RIGHT_W     400

/* Frischwasser: drei Segmente, eines je Elektrode, unten beginnend. */
static lv_obj_t *s_tank_seg[WATER_ELECTRODE_COUNT];
static lv_obj_t *s_level_hint;

/* Solar-Kurzanzeige auf Home — die ausführliche Fassung steht auf der
 * Solar-Seite. */
static lv_obj_t *s_home_error;

/* Akku-Kurzanzeige unter den Solarwerten */
/** Restlaufzeit — sie steht jetzt im Ladebalken selbst. */
static lv_obj_t *s_batt_runtime;
static lv_obj_t *s_batt_alert;   /* steht im Störungsfall an deren Stelle  */

/*
 * Balken statt Zahlenkolonnen.
 *
 * Solar und Akku waren vier Wertzeilen und zwei große Zahlen. Im Vorbeigehen
 * liest man davon nichts ab — man sieht eine Zahl und muss sie erst gegen eine
 * Erwartung halten. Ein Balken zeigt das Verhältnis sofort: halb voll, fast
 * leer, am Anschlag. Deshalb dieselbe Sprache wie bei den Tanks; die genauen
 * Werte stehen weiterhin im Detaildialog.
 */
#define BAR_SEGMENTS 10

/** Vollausschlag des Solarbalkens. */
#define SOLAR_BAR_MAX_W 220

static lv_obj_t *s_solar_seg[BAR_SEGMENTS];
static lv_obj_t *s_batt_soc_seg[BAR_SEGMENTS];
/* Laden links vom Akku, Entladen rechts — die Richtung des Pfeils ist in
 * beiden Fällen dieselbe: dem Energiefluss nach. */
/** WLAN-Zeichen in der Kopfzeile; die Farbe trägt den Empfangspegel. */
static lv_obj_t *s_wifi_icon;

static lv_obj_t *s_batt_chg_w;
static lv_obj_t *s_batt_chg_arrow;
static lv_obj_t *s_batt_dis_w;
static lv_obj_t *s_batt_dis_arrow;

/* Farben der Leistungsanzeige. Die Entnahme läuft von Gelb nach Rot, die
 * Ladung von Dunkel- nach Hellgrün — je weiter vom Nullpunkt entfernt, desto
 * kräftiger. So liest man die Größenordnung schon an der Farbe ab, bevor man
 * die Segmente zählt. */
#define COL_DIS_LOW  0xd8b020
#define COL_DIS_HIGH 0xda3633
#define COL_CHG_LOW  0x15803d
#define COL_CHG_HIGH 0x4ade80
/** Die Null in der Mitte — sie muss immer zu sehen sein. */
#define COL_ZERO_MARK 0xe8edf2
/** Farbe der Sonnen, wenn Leistung anliegt. */
#define COL_SUN 0xf5b301
/** Füllfarbe der Batterie bei gutem Ladestand. */
#define COL_SOC_OK 0x3fb950

/**
 * So lange darf es still bleiben, bevor die Verbindung als tot gilt.
 *
 * Abgefragt wird alle 30 s. Vier ausgefallene Runden sind kein Zufall mehr,
 * sondern eine Verbindung, die nur noch auf dem Papier besteht.
 */
#define BATT_WATCHDOG_MS 120000
/** Neuer Versuch, wenn der Akku vorher schon einmal geantwortet hat. */
#define BATT_RETRY_FAST_MS 20000
/**
 * Neuer Versuch, wenn noch nie etwas kam.
 *
 * Bewusst lang: Solange das Board verbunden ist, wirbt das BMS nicht mehr und
 * die Hersteller-App kommt nicht an den Akku. Ein Gerät, das uns nichts
 * liefert, darf ihr nicht dauerhaft den Weg verstellen.
 */
#define BATT_RETRY_SLOW_MS 600000

/**
 * Fenster der Restlaufzeit: die letzten 24 Stunden.
 *
 * Ein voller Tag deckt einen kompletten Zyklus ab — Nacht mit Kühlschrank und
 * Heizung, Tag mit Sonne. Kürzer gerechnet schwankt die Zahl mit jedem
 * Pumpenlauf, länger gerechnet mittelt sie über Wetterlagen hinweg, die mit
 * heute nichts mehr zu tun haben.
 */
#define BATT_RUN_WINDOW_MS (24u * 60 * 60 * 1000)

/** Totbereich um die Null: darunter ist weder Laden noch Entladen gemeint. */
#define BATT_IDLE_MA 50

/* Abwasser */
static lv_obj_t *s_waste_label;
static lv_timer_t *s_waste_cycle;



/* -------------------------------------------------------------------------- */
/*  Optik: flächig                                                            */
/* -------------------------------------------------------------------------- */

/*
 * Einfarbige Flächen, rechte Winkel, keine Verläufe. Der einzige plastische
 * Rest ist ein leichter Schatten unter den Knöpfen — und der ist keine Zierde,
 * sondern Rückmeldung: Beim Drücken verschwindet er und die Fläche rutscht
 * zwei Pixel nach unten.
 *
 * Der Verzicht auf Verläufe hat neben dem Aussehen einen handfesten Grund.
 * LVGL rechnet sie im Software-Renderer auf dem Stack, und genau daran ist der
 * LVGL-Task abgestürzt, bis sein Stapelspeicher verdreifacht wurde. Flächen
 * kosten davon einen Bruchteil.
 */

/** Grundfarbe aufhellen (positiv) oder abdunkeln (negativ), in Prozent. */
static lv_color_t tint(uint32_t hex, int percent)
{
    const lv_color_t c = lv_color_hex(hex);
    if (percent >= 0) {
        return lv_color_lighten(c, (lv_opa_t)(percent * 255 / 100));
    }
    return lv_color_darken(c, (lv_opa_t)(-percent * 255 / 100));
}

/**
 * Einfarbige Fläche ohne Rundung und ohne Rahmen.
 *
 * @param shadow Schattenbreite in Pixeln; 0 lässt die Fläche ganz flach.
 */
static void style_flat(lv_obj_t *obj, uint32_t base, int shadow,
                       lv_style_selector_t sel)
{
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, sel);
    lv_obj_set_style_bg_color(obj, lv_color_hex(base), sel);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, sel);
    lv_obj_set_style_radius(obj, 0, sel);
    lv_obj_set_style_border_width(obj, 0, sel);

    if (shadow > 0) {
        lv_obj_set_style_shadow_color(obj, lv_color_black(), sel);
        lv_obj_set_style_shadow_width(obj, shadow, sel);
        lv_obj_set_style_shadow_offset_y(obj, shadow / 3, sel);
        lv_obj_set_style_shadow_opa(obj, LV_OPA_50, sel);
    } else {
        lv_obj_set_style_shadow_width(obj, 0, sel);
    }
}

/** Zustände eines Knopfes: gedrückt, eingerastet, gesperrt. */
static void style_button_states(lv_obj_t *btn, uint32_t base, uint32_t accent)
{
    /* Gedrückt: dunkler, Schatten weg, zwei Pixel tiefer. Der verschwindende
     * Schatten ist die eigentliche Rückmeldung — die Farbe allein sieht man
     * unter dem Finger nicht. */
    lv_obj_set_style_bg_color(btn, tint(base, -25), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(btn, 0, LV_STATE_PRESSED);
    lv_obj_set_style_translate_y(btn, 2, LV_STATE_PRESSED);

    /* Eingerastet: Akzentfarbe, flächig. */
    lv_obj_set_style_bg_color(btn, lv_color_hex(accent), LV_STATE_CHECKED);

    lv_obj_set_style_opa(btn, LV_OPA_40, LV_STATE_DISABLED);
}

/** Segment eines Füllbalkens einfärben. */
/*
 * ## Nichts anfassen, was sich nicht geändert hat
 *
 * LVGL vergleicht beim Setzen einer Stileigenschaft nicht mit dem alten Wert —
 * jede Zuweisung erklärt das Objekt für ungültig und zieht ein Neuzeichnen
 * nach sich. Die Anzeige frischt sich jede Sekunde auf; die meisten Werte
 * stehen dabei still.
 *
 * Auf einem RGB-Panel mit zwei Framebuffern im PSRAM ist das nicht bloß
 * verschwendete Rechenzeit: Jede ungültige Fläche muss gezeichnet **und** in
 * den zweiten Puffer übertragen werden. Kommt genug davon zusammen, sieht man
 * es als Flackern. Genau das trat auf, als aus einem Balken drei wurden —
 * 30 Segmente zu je sechs Stileigenschaften, sechzig Mal in der Minute.
 *
 * Deshalb prüfen die folgenden Helfer erst und setzen dann.
 */

static void segment_set(lv_obj_t *seg, uint32_t hex, bool filled)
{
    (void)filled;
    const lv_color_t want = lv_color_hex(hex);
    if (lv_color_eq(lv_obj_get_style_bg_color(seg, LV_PART_MAIN), want)) {
        return;
    }
    /* Nur die Farbe: Rand, Radius und Deckkraft setzt `bar_create` einmalig
     * beim Anlegen, und sie ändern sich nie. */
    lv_obj_set_style_bg_color(seg, want, LV_PART_MAIN);
}

/** Text nur setzen, wenn er sich geändert hat. */
static void label_set(lv_obj_t *label, const char *text)
{
    if (!label) {
        return;
    }
    const char *cur = lv_label_get_text(label);
    if (cur && strcmp(cur, text) == 0) {
        return;
    }
    lv_label_set_text(label, text);
}

/** Wie label_set(), mit Formatierung. */
static void label_set_fmt(lv_obj_t *label, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    label_set(label, buf);
}

/** Textfarbe nur setzen, wenn sie sich geändert hat. */
static void label_color(lv_obj_t *label, uint32_t hex)
{
    if (!label) {
        return;
    }
    const lv_color_t want = lv_color_hex(hex);
    if (lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), want)) {
        return;
    }
    lv_obj_set_style_text_color(label, want, LV_PART_MAIN);
}

/** Ein- oder ausblenden, aber nur bei echtem Wechsel. */
static void obj_show(lv_obj_t *obj, bool show)
{
    if (!obj) {
        return;
    }
    if (show == !lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (show) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

/* -------------------------------------------------------------------------- */
/*  Anzeige aktualisieren                                                     */
/* -------------------------------------------------------------------------- */

static void tank_update(const water_measurement_t *m)
{
    for (int i = 0; i < WATER_ELECTRODE_COUNT; i++) {
        segment_set(s_tank_seg[i], m->wet[i] ? COL_WATER : COL_EMPTY, m->wet[i]);
    }

    /* Der Füllstand steht nicht mehr als Wort im Tank — drei Segmente sagen
     * dasselbe, und ein Wort im Balken war bei dieser Größe ohnehin eng.
     * Leer wird stattdessen über die Farbe hervorgehoben: Rot statt Blau, wie
     * das rote Abwasserzeichen. Beides ist der Zustand, bei dem gehandelt
     * werden muss, und beide sollen von weitem gleich aussehen. */
    if (m->level == WATER_LEVEL_EMPTY) {
        segment_set(s_tank_seg[0], COL_ALERT, true);
    }

    /* Die Hinweiszeile bleibt im Normalfall leer und unsichtbar. Sie meldet
     * sich nur, wenn etwas nicht stimmt — dann fällt sie auf, statt in einer
     * Zeile unterzugehen, die immer dasteht. */
    if (m->implausible) {
        lv_label_set_text(s_level_hint, LV_SYMBOL_WARNING
                          " Unplausibel — Elektrode oder Leitung prüfen");
        lv_obj_remove_flag(s_level_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_level_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

/* -------------------------------------------------------------------------- */
/*  Frischwasser: Messung auf Knopfdruck                                      */
/* -------------------------------------------------------------------------- */

/**
 * Eine Frischwassermessung ausführen und anzeigen.
 *
 * Blockiert rund 70 ms im LVGL-Task — die Zeit, die Anregung, Beruhigung und
 * drei Abtastungen brauchen. Bei einem Takt von fünf Minuten fällt das nicht
 * ins Gewicht.
 */
static void water_check_now(void)
{
    if (water_level_is_excited()) {
        /* Im Testmodus liegt die Anregung dauerhaft an — eine getaktete Messung
         * würde sie am Ende abschalten und den Testmodus stillschweigend beenden. */
        return;
    }

    water_measurement_t m = {0};
    const esp_err_t ret = water_level_measure(&m);
    if (ret != ESP_OK) {
        for (int i = 0; i < WATER_ELECTRODE_COUNT; i++) {
            segment_set(s_tank_seg[i], COL_WARN, true);
        }
        lv_label_set_text_fmt(s_level_hint, LV_SYMBOL_WARNING " Messfehler: %s",
                              esp_err_to_name(ret));
        lv_obj_remove_flag(s_level_hint, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    tank_update(&m);
}

/*
 * Antippen misst nach — und sagt, dass es das tut.
 *
 * Die Messung dauert rund 70 ms und endet oft mit demselben Füllstand wie
 * vorher. Ohne Rückmeldung sieht es dann aus, als sei der Tipp ins Leere
 * gegangen. Deshalb blinkt der Rahmen kurz auf: beim Drücken über den
 * Zustandsstil sofort, danach noch einen Moment nach, bis der neue Wert steht.
 */
#define TANK_FLASH_MS 500

static lv_obj_t *s_tank_frame;

static void tank_flash_end(lv_timer_t *t)
{
    (void)t;
    lv_obj_set_style_border_color(s_tank_frame, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_tank_frame, 2, LV_PART_MAIN);
}

static void on_tank_clicked(lv_event_t *e)
{
    (void)e;

    /* Mitschreiben, weil sich von außen nicht unterscheiden lässt, ob die
     * Berührung nicht ankommt oder die Rückmeldung zu unscheinbar ist. */
    ESP_LOGI(TAG, "Tank angetippt — messe");

    /* Rahmen breiter **und** farbig: Die Messung dauert nur 70 ms, danach
     * steht meist derselbe Füllstand da. Ein schmaler Farbwechsel geht dabei
     * unter. */
    lv_obj_set_style_border_color(s_tank_frame, lv_color_hex(COL_ACCENT), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_tank_frame, 4, LV_PART_MAIN);
    water_check_now();

    lv_timer_t *t = lv_timer_create(tank_flash_end, TANK_FLASH_MS, NULL);
    lv_timer_set_repeat_count(t, 1);
}

/* -------------------------------------------------------------------------- */
/*  Abwasser: zyklische Prüfung                                               */
/* -------------------------------------------------------------------------- */

/*
 * Das Zeichen trägt die ganze Aussage: Grün heißt, es ist noch Platz, Rot
 * heißt voll. Mehr weiß der Tank nicht — er hat genau eine Elektrode.
 */
static void waste_update(const waste_measurement_t *m)
{
    label_color(s_waste_label, m->full ? COL_ALERT : COL_SOC_OK);
}

/**
 * Eine Prüfung ausführen und anzeigen. Läuft im LVGL-Task, blockiert ~70 ms.
 *
 * Auf Home steht nur noch das Ergebnis — kein Prüfknopf, keine Zeile mit dem
 * Alter der Messung. Der Fünf-Minuten-Takt läuft unverändert im Hintergrund;
 * wer den Rohpegel sehen will, findet ihn auf der Test-Seite.
 */
static void waste_check_now(void)
{
    if (waste_level_is_excited()) {
        /* Testmodus hält die Anregung dauerhaft an; die getaktete Prüfung würde
         * sie am Ende abschalten und den Test stillschweigend beenden. */
        ESP_LOGW(TAG, "Abwasserprüfung übersprungen — Pegel-Test läuft");
        return;
    }

    waste_measurement_t m = {0};
    if (waste_level_measure(&m) != ESP_OK) {
        /* Gelb statt Grün oder Rot: Der Zustand ist unbekannt, und das ist
         * etwas anderes als "noch Platz". */
        label_color(s_waste_label, COL_WARN);
        return;
    }
    waste_update(&m);
}

/**
 * Beide Tanks im selben Takt prüfen.
 *
 * Nacheinander, nicht gleichzeitig: Die Anregungen liegen zwar auf getrennten
 * Pins, aber jede Messung braucht ihre eigene Beruhigungszeit, und der
 * I2C-Bus wird ohnehin serialisiert.
 */
/*
 * Gemessen wird im Takt — aber nur, solange jemand hinsehen kann.
 *
 * Mit den Knöpfen ist auch die Möglichkeit entfallen, von Hand nachzumessen;
 * dafür muss der Takt zuverlässig laufen. Bei dunklem Bildschirm pausiert er:
 * Jede Messung bestromt die Elektroden, und Gleichstrom im Wasser zersetzt
 * sie. Nachts im Stand ist das vergeudeter Verschleiß an einer Zahl, die
 * niemand liest.
 *
 * Beim Aufwecken wird sofort gemessen, damit der erste Blick einen frischen
 * Wert trifft und nicht den von gestern Abend.
 */
static void tank_cycle_cb(lv_timer_t *t)
{
    (void)t;

    if (!bsp_backlight_is_on()) {
        return;
    }
    water_check_now();
    waste_check_now();
}

/**
 * Beide Framebuffer vollständig neu malen.
 *
 * Das Panel läuft im `direct_mode` mit zwei Bildspeichern: LVGL zeichnet immer
 * nur die geänderten Flächen und überträgt sie anschließend in den jeweils
 * anderen Puffer. Bleibt dabei eine Fläche aus — beim ersten Aufbau, nach
 * einem Umbau der Oberfläche, nach dem Aufwecken —, dann steht in einem der
 * beiden Puffer noch der alte Inhalt. Sichtbar wird das als einzelne Zeile,
 * die immer an derselben Stelle steht und Inhalt von woanders zeigt.
 *
 * Zweimal: Ein Durchgang je Puffer, danach sind beide vollständig.
 */
static void screen_repaint(void)
{
    for (int i = 0; i < 2; i++) {
        lv_obj_invalidate(lv_screen_active());
        lv_refr_now(NULL);
    }
}

/** Nach dem Aufwecken einmal nachmessen. */
static void tank_wake_check(void)
{
    static bool s_was_on = true;
    const bool on = bsp_backlight_is_on();

    if (on && !s_was_on) {
        water_check_now();
        waste_check_now();
        /* Nach dem Dunkelschalten kann in einem der beiden Bildspeicher
         * Veraltetes stehen. */
        screen_repaint();
    }
    s_was_on = on;
}

/* -------------------------------------------------------------------------- */
/*  Lasten und Beleuchtung                                                    */
/* -------------------------------------------------------------------------- */

static void on_output_toggled(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const bsp_output_t out = (bsp_output_t)(intptr_t)lv_event_get_user_data(e);
    const bool on = lv_obj_has_state(btn, LV_STATE_CHECKED);

    esp_err_t ret = bsp_output_set(out, on);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PA%d schalten fehlgeschlagen: %s", out, esp_err_to_name(ret));
        /* Der Knopf zeigt seinen Zustand selbst — bei einem Fehler darf er
         * nicht eingerastet bleiben und Betrieb vortäuschen. */
        if (on) {
            lv_obj_remove_state(btn, LV_STATE_CHECKED);
        } else {
            lv_obj_add_state(btn, LV_STATE_CHECKED);
        }
    }
}

static void on_display_off(lv_event_t *e)
{
    (void)e;
    /* Sperrzeit ist zwingend: dieselbe Berührung prellt beim GT911 nach und
     * würde sonst sofort wieder aufwecken. */
    ESP_ERROR_CHECK(bsp_display_sleep(BSP_DISPLAY_WAKE_LOCKOUT_MS));
}

/* -------------------------------------------------------------------------- */
/*  Bausteine der Oberfläche                                                  */
/* -------------------------------------------------------------------------- */

static lv_obj_t *card_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    style_flat(card, COL_CARD, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 14, LV_PART_MAIN);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *button_create(lv_obj_t *parent, const char *text, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    style_flat(btn, COL_BTN, 8, LV_PART_MAIN);
    style_button_states(btn, COL_BTN, COL_ACCENT);

    lv_obj_t *label = lv_label_create(btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, &font_ui_20, LV_PART_MAIN);
    lv_obj_center(label);
    return btn;
}

/**
 * Ein Segment eines Balkens.
 *
 * Ausdrücklich **nicht** anklickbar: LVGL gibt neuen Objekten die
 * Klickbarkeit mit, und ein Segment, das Berührungen schluckt, macht die
 * Fläche darunter unerreichbar. Genau daran scheiterte der Griff auf den Akku
 * — der Knopf lag darunter, die Segmente fingen jeden Tipp ab.
 */
static lv_obj_t *segment_create(lv_obj_t *parent)
{
    lv_obj_t *seg = lv_obj_create(parent);
    lv_obj_set_height(seg, LV_PCT(100));
    lv_obj_set_flex_grow(seg, 1);
    style_flat(seg, COL_EMPTY, 0, LV_PART_MAIN);
    lv_obj_remove_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(seg, LV_OBJ_FLAG_CLICKABLE);
    return seg;
}

/** Unsichtbarer Behälter — nur zum Gruppieren, ohne eigene Optik. */
static lv_obj_t *plain_create(lv_obj_t *parent, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(obj, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(obj, 0, LV_PART_MAIN);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    /* Ein reiner Gruppierbehälter darf keine Berührungen abfangen — sonst
     * liegt er wie eine Glasscheibe über dem, was darunter bedient werden
     * soll. Wer ihn doch anklickbar braucht, setzt das Merkmal selbst. */
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *title_create(lv_obj_t *parent, const char *text, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    return label;
}

/* -------------------------------------------------------------------------- */
/*  Seite "Betrieb"                                                           */
/* -------------------------------------------------------------------------- */
/*  Abschaltzeit: Einstellung und Dauerspeicherung                            */
/* -------------------------------------------------------------------------- */

#define SETTINGS_NAMESPACE "ui"
#define SETTINGS_KEY_IDLE  "idle_ms"

/** Ohne Berührung schaltet das Display nach dieser Zeit ab. 0 = niemals. */
static uint32_t s_idle_ms = 20000;

static const struct {
    const char *label;
    uint32_t ms;
} s_idle_options[] = {
    {"10 Sekunden", 10000},
    {"30 Sekunden", 30000},
    {"2 Minuten",   120000},
    {"10 Minuten",  600000},
    {"niemals",     0},
};
#define IDLE_OPTION_COUNT (sizeof(s_idle_options) / sizeof(s_idle_options[0]))

static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(SETTINGS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        /* Beim allerersten Start gibt es den Namensraum noch nicht — dann
         * bleibt es beim Vorgabewert, das ist kein Fehler. */
        return;
    }
    uint32_t value = 0;
    if (nvs_get_u32(h, SETTINGS_KEY_IDLE, &value) == ESP_OK) {
        s_idle_ms = value;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Abschaltzeit: %lu ms", (unsigned long)s_idle_ms);
}

/**
 * @note Schreibt ins NVS und damit in den Flash. Ein Flash-Zugriff hält den
 *       Cache an, was am RGB-Panel kurz sichtbar werden kann. Das passiert
 *       genau einmal je Auswahl und ist der Preis dafür, dass die Einstellung
 *       einen Neustart übersteht.
 */
static void settings_save_idle(uint32_t ms)
{
    s_idle_ms = ms;

    nvs_handle_t h;
    esp_err_t ret = nvs_open(SETTINGS_NAMESPACE, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Einstellung nicht speicherbar: %s", esp_err_to_name(ret));
        return;
    }
    if (nvs_set_u32(h, SETTINGS_KEY_IDLE, ms) == ESP_OK) {
        nvs_commit(h);
        ESP_LOGI(TAG, "Abschaltzeit gespeichert: %lu ms", (unsigned long)ms);
    }
    nvs_close(h);
}

static lv_obj_t *s_idle_dialog;

static void idle_dialog_close(void)
{
    if (s_idle_dialog) {
        lv_obj_delete(s_idle_dialog);
        s_idle_dialog = NULL;
    }
}

static void on_idle_choice(lv_event_t *e)
{
    settings_save_idle((uint32_t)(uintptr_t)lv_event_get_user_data(e));
    idle_dialog_close();
}

static void on_idle_backdrop(lv_event_t *e)
{
    (void)e;
    idle_dialog_close();
}

/**
 * Auswahl der Abschaltzeit. Liegt auf der obersten Ebene, damit sie unabhängig
 * von der gerade sichtbaren Seite erscheint.
 */
/*
 * Pulsierende Pfeile, aber sparsam.
 *
 * Zuerst lief das über `lv_anim`. Das sieht weich aus, erneuert die Deckkraft
 * aber bei **jedem** Bildaufbau — rund sechzig Mal je Sekunde —, und LVGL
 * erklärt das Objekt bei jeder Zuweisung für ungültig, auch wenn sich kaum
 * etwas ändert. Auf diesem Panel ist dauerndes Neuzeichnen der sichere Weg zu
 * Geisterzeilen: Jede ungültige Fläche muss gezeichnet und in den zweiten
 * Framebuffer kopiert werden, beides über denselben PSRAM-Bus, aus dem das
 * Panel gerade sein Bild liest.
 *
 * Deshalb eigener Takt: zwölf Stufen in knapp zwei Sekunden, also sechs
 * Änderungen je Sekunde statt sechzig. Für das Auge bleibt es ein weiches
 * Pulsieren, für den Bus ist es ein Zehntel der Last.
 */

#define ARROW_STEPS 12
#define ARROW_STEP_MS 160

static lv_obj_t *s_arrows[2];
static uint8_t s_arrow_count;

static void arrow_tick(lv_timer_t *t)
{
    (void)t;
    static uint8_t step;

    step = (uint8_t)((step + 1) % (2 * ARROW_STEPS));
    /* Hin und zurück: aus der Sägezahn- wird eine Dreieckskurve. */
    const uint8_t up = (step < ARROW_STEPS) ? step : (uint8_t)(2 * ARROW_STEPS - step);
    const lv_opa_t opa =
        (lv_opa_t)(LV_OPA_40 + ((LV_OPA_COVER - LV_OPA_40) * up) / ARROW_STEPS);

    for (uint8_t i = 0; i < s_arrow_count; i++) {
        /* Verdeckte Pfeile auslassen: LVGL zeichnet sie zwar nicht, die
         * Zuweisung erklärt sie aber trotzdem für ungültig. */
        if (!lv_obj_has_flag(s_arrows[i], LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_set_style_opa(s_arrows[i], opa, LV_PART_MAIN);
        }
    }
}

/** Einen Pfeil in den gemeinsamen Takt aufnehmen. */
static void arrow_blink(lv_obj_t *obj)
{
    if (s_arrow_count >= sizeof(s_arrows) / sizeof(s_arrows[0])) {
        return;
    }
    s_arrows[s_arrow_count++] = obj;
    if (s_arrow_count == 1) {
        lv_timer_create(arrow_tick, ARROW_STEP_MS, NULL);
    }
}

/** Kleine Wertzeile: Beschriftung links, Wert rechts. Gibt den Wert zurück. */
static lv_obj_t *row_value_create(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *row = plain_create(parent, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, caption);
    lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);

    lv_obj_t *value = lv_label_create(row);
    lv_obj_set_style_text_font(value, &font_ui_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(value, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_label_set_text(value, "—");
    return value;
}

static void light_page_open(void);

/** Aus dem Menü heraus die Lichtseite öffnen — und das Menü dabei schließen. */
static void on_open_light_page(lv_event_t *e)
{
    (void)e;
    idle_dialog_close();
    light_page_open();
}

/* -------------------------------------------------------------------------- */
/*  Texteingabe und Netzauswahl                                               */
/* -------------------------------------------------------------------------- */
/*
 * Eine Bildschirmtastatur für drei Felder: Netzname, Passwort, Gerätename.
 *
 * Sie ist der einzige Weg, das Gerät ohne Rechner umzuziehen — ein Fahrzeug
 * wechselt den Hotspot, und wer dafür neu übersetzen muss, hat kein Gerät,
 * sondern einen Prototyp.
 */

typedef void (*text_done_cb_t)(const char *text);

static lv_obj_t *s_kb_layer;
static lv_obj_t *s_kb_area;
static text_done_cb_t s_kb_done;

static void kb_close(lv_event_t *e)
{
    (void)e;
    if (s_kb_layer) {
        lv_obj_delete(s_kb_layer);
        s_kb_layer = NULL;
        s_kb_area = NULL;
    }
}

static void kb_accept(lv_event_t *e)
{
    (void)e;
    const text_done_cb_t done = s_kb_done;
    char text[WEB_PASS_LEN];
    strlcpy(text, lv_textarea_get_text(s_kb_area), sizeof(text));

    kb_close(NULL);
    if (done) {
        done(text);
    }
}

/**
 * Text abfragen.
 *
 * @param title    Was eingegeben werden soll.
 * @param preset   Vorbelegung, meist der aktuelle Wert.
 * @param password true blendet die Zeichen aus.
 * @param done     bekommt den Text, wenn bestätigt wurde.
 */
static void text_input_open(const char *title, const char *preset, bool password,
                            text_done_cb_t done)
{
    if (s_kb_layer) {
        return;
    }
    s_kb_done = done;

    s_kb_layer = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_kb_layer, BSP_LCD_H_RES, BSP_LCD_V_RES);
    style_flat(s_kb_layer, COL_BG, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_kb_layer, 10, LV_PART_MAIN);
    lv_obj_remove_flag(s_kb_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_kb_layer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_kb_layer, 8, LV_PART_MAIN);

    lv_obj_t *row = plain_create(s_kb_layer, LV_PCT(100), 48);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, LV_PART_MAIN);

    title_create(row, title, &font_ui_20);
    lv_obj_t *cancel = button_create(row, "Abbrechen", 150, 44);
    lv_obj_add_event_cb(cancel, kb_close, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ok = button_create(row, LV_SYMBOL_OK "  Übernehmen", 200, 44);
    lv_obj_add_event_cb(ok, kb_accept, LV_EVENT_CLICKED, NULL);

    s_kb_area = lv_textarea_create(s_kb_layer);
    lv_obj_set_width(s_kb_area, LV_PCT(100));
    lv_textarea_set_one_line(s_kb_area, true);
    lv_textarea_set_password_mode(s_kb_area, password);
    lv_textarea_set_max_length(s_kb_area, WEB_PASS_LEN - 1);
    lv_textarea_set_text(s_kb_area, preset ? preset : "");
    lv_obj_set_style_text_font(s_kb_area, &font_ui_20, LV_PART_MAIN);

    lv_obj_t *kb = lv_keyboard_create(s_kb_layer);
    lv_obj_set_width(kb, LV_PCT(100));
    lv_obj_set_flex_grow(kb, 1);
    lv_keyboard_set_textarea(kb, s_kb_area);
}

/* ---- Netzauswahl ---------------------------------------------------- */

static lv_obj_t *s_net_layer;

static void net_close(lv_event_t *e)
{
    (void)e;
    if (s_net_layer) {
        lv_obj_delete(s_net_layer);
        s_net_layer = NULL;
    }
}

/** Nach der Auswahl eines Netzes gleich nach dem Passwort fragen. */
static char s_pick_ssid[WEB_SSID_LEN];

static void on_password_entered(const char *text)
{
    if (web_set_credentials(s_pick_ssid, text) != ESP_OK) {
        ESP_LOGW(TAG, "Zugangsdaten ließen sich nicht übernehmen");
    }
}

static void on_net_picked(lv_event_t *e)
{
    strlcpy(s_pick_ssid, (const char *)lv_event_get_user_data(e), sizeof(s_pick_ssid));
    net_close(NULL);
    text_input_open("Passwort", "", true, on_password_entered);
}

static void on_ssid_entered(const char *text)
{
    strlcpy(s_pick_ssid, text, sizeof(s_pick_ssid));
    text_input_open("Passwort", "", true, on_password_entered);
}

static void on_net_manual(lv_event_t *e)
{
    (void)e;
    net_close(NULL);
    text_input_open("Netzname", web_ssid(), false, on_ssid_entered);
}

/**
 * Netze zur Auswahl stellen.
 *
 * Der Suchlauf blockiert rund zwei Sekunden und hält dabei den Sender besetzt.
 * Das ist hier vertretbar: Er läuft nur, wenn jemand ausdrücklich danach
 * fragt, und währenddessen sieht derjenige ohnehin auf den Bildschirm.
 */
static void net_list_open(lv_event_t *e)
{
    (void)e;

    if (s_net_layer) {
        return;
    }

    static char ssid[12][WEB_SSID_LEN];
    static int8_t rssi[12];
    const size_t count = web_scan(ssid, rssi, 12);

    s_net_layer = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_net_layer, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_net_layer, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_net_layer, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_net_layer, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_net_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_net_layer, net_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = card_create(s_net_layer, 520, 420);
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *row = plain_create(card, LV_PCT(100), 40);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    title_create(row, "Netz wählen", &font_ui_20);
    lv_obj_t *close = button_create(row, "Zurück", 140, 40);
    lv_obj_add_event_cb(close, net_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *list = plain_create(card, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, LV_PART_MAIN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_MOMENTUM);

    if (count == 0) {
        lv_obj_t *hint = lv_label_create(list);
        lv_obj_set_style_text_color(hint, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
        lv_label_set_text(hint, web_is_enabled()
                                    ? "Kein Netz gefunden. Nur 2,4 GHz ist sichtbar."
                                    : "Das WLAN ist abgeschaltet.");
    }

    for (size_t i = 0; i < count; i++) {
        /* Der Puffer muss den Namen in voller Länge fassen — der Übersetzer
         * rechnet sonst mit dem Schlimmsten und bricht ab. */
        char label[WEB_SSID_LEN + 16];
        snprintf(label, sizeof(label), "%.32s   %d dBm", ssid[i], rssi[i]);
        lv_obj_t *btn = button_create(list, label, LV_PCT(100), 48);
        lv_obj_add_event_cb(btn, on_net_picked, LV_EVENT_CLICKED, ssid[i]);
    }

    lv_obj_t *manual = button_create(card, "Von Hand eingeben", LV_PCT(100), 48);
    lv_obj_add_event_cb(manual, on_net_manual, LV_EVENT_CLICKED, NULL);
}

static void on_name_entered(const char *text)
{
    web_set_device_name(text);
}

static void on_name_clicked(lv_event_t *e)
{
    (void)e;
    idle_dialog_close();
    text_input_open("Gerätename", web_device_name(), false, on_name_entered);
}

static void on_net_clicked(lv_event_t *e)
{
    idle_dialog_close();
    net_list_open(e);
}

static void on_wifi_toggled(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    web_set_enabled(lv_obj_has_state(sw, LV_STATE_CHECKED));
}

/**
 * Einstellungen.
 *
 * Zwei Spalten statt einer Liste: Gerät und Netz links, Abschaltzeit rechts.
 * Untereinander lief die Karte unten aus dem Bild — elf Zeilen zu je 50
 * Bildpunkten sind mehr, als 480 Bildpunkte Höhe hergeben, und ein Dialog,
 * den man rollen muss, verbirgt genau das, was man sucht.
 */
static void idle_dialog_open(lv_event_t *e)
{
    (void)e;

    if (s_idle_dialog) {
        return;
    }

    /* Der lange Druck darf nicht zusätzlich als Klick durchgehen — sonst ginge
     * beim Loslassen das Display aus, während der Dialog aufgeht. */
    lv_indev_wait_release(lv_indev_active());

    s_idle_dialog = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_idle_dialog, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_idle_dialog, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_idle_dialog, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_idle_dialog, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_idle_dialog, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_idle_dialog, LV_OBJ_FLAG_SCROLLABLE);
    /* Daneben tippen schließt — der übliche Weg aus einem Dialog heraus. */
    lv_obj_add_event_cb(s_idle_dialog, on_idle_backdrop, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = card_create(s_idle_dialog, 720, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, LV_PART_MAIN);
    /* Klicks auf die Karte dürfen nicht als Klick auf den Hintergrund gelten. */
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *head = plain_create(card, LV_PCT(100), 44);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    title_create(head, "Einstellungen", &font_ui_28);
    lv_obj_t *close = button_create(head, "Schließen", 160, 44);
    lv_obj_add_event_cb(close, on_idle_backdrop, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cols = plain_create(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cols, 20, LV_PART_MAIN);

    /* ---- Links: Gerät und Netz ----------------------------------------- */
    /*
     * Alles, was man einmal einstellt und dann nie wieder anfasst. Die Adresse
     * braucht man genau einmal, nämlich wenn man das Telefon zum ersten Mal
     * darauf richtet.
     */
    lv_obj_t *left = plain_create(cols, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 8, LV_PART_MAIN);

    title_create(left, "Gerät und Netz", &font_ui_20);

    lv_obj_t *name_btn = button_create(left, "", LV_PCT(100), 48);
    lv_obj_set_style_pad_hor(name_btn, 12, LV_PART_MAIN);
    lv_obj_t *name_lbl = lv_obj_get_child(name_btn, 0);
    lv_label_set_text_fmt(name_lbl, "Name:  %s", web_device_name());
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(name_btn, on_name_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *net_btn = button_create(left, "", LV_PCT(100), 48);
    lv_obj_set_style_pad_hor(net_btn, 12, LV_PART_MAIN);
    lv_obj_t *net_lbl = lv_obj_get_child(net_btn, 0);
    lv_label_set_text_fmt(net_lbl, "Netz:  %s", web_ssid());
    lv_obj_align(net_lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(net_btn, on_net_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *wifi_row = plain_create(left, LV_PCT(100), 44);
    lv_obj_set_flex_flow(wifi_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *wifi_cap = lv_label_create(wifi_row);
    lv_obj_set_style_text_color(wifi_cap, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_label_set_text(wifi_cap, "WLAN einschalten");

    lv_obj_t *sw = lv_switch_create(wifi_row);
    lv_obj_set_size(sw, 70, 36);
    if (web_is_enabled()) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(sw, on_wifi_toggled, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *ip = row_value_create(left, "Adresse");
    lv_label_set_text(ip, web_is_connected() ? web_ip() : web_status_text());
    lv_obj_set_style_text_color(ip,
                                lv_color_hex(web_is_connected() ? COL_TEXT : COL_TEXT_DIM),
                                LV_PART_MAIN);

    lv_obj_t *to_light = button_create(left, LV_SYMBOL_IMAGE "  Lichtseite",
                                       LV_PCT(100), 48);
    lv_obj_add_event_cb(to_light, on_open_light_page, LV_EVENT_CLICKED, NULL);

    /* ---- Rechts: Abschaltzeit ------------------------------------------ */
    lv_obj_t *right = plain_create(cols, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 8, LV_PART_MAIN);

    title_create(right, "Bildschirm dunkel schalten", &font_ui_20);

    lv_obj_t *note = lv_label_create(right);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(note, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_label_set_text(note, "Eine Berührung weckt ihn wieder.");

    for (size_t i = 0; i < IDLE_OPTION_COUNT; i++) {
        lv_obj_t *btn = button_create(right, s_idle_options[i].label, LV_PCT(100), 44);
        lv_obj_add_event_cb(btn, on_idle_choice, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)s_idle_options[i].ms);
        /* Die gültige Einstellung ist eingerastet — so sieht man beim Öffnen
         * sofort, was gerade gilt. */
        if (s_idle_options[i].ms == s_idle_ms) {
            lv_obj_add_state(btn, LV_STATE_CHECKED);
        }
    }
}

/* -------------------------------------------------------------------------- */
/**
 * Sperrzeit nach dem selbsttätigen Abschalten — kurz gehalten. Beim Knopf
 * "Display aus" ist sie nötig, weil dieselbe Berührung nachprellt und sofort
 * wieder wecken würde. Hier war gar keine Berührung im Spiel; eine lange
 * Sperre würde nur die erste Weckberührung schlucken.
 */
#define DISPLAY_IDLE_LOCKOUT_MS 300

/**
 * Untätigkeit prüfen. LVGL vermerkt jede Berührung, **bevor** sie an ein
 * Bedienelement geht — auch die weckende, die der BSP verschluckt. Der Zähler
 * stimmt also unabhängig davon, wo hingetippt wurde.
 */
/* Beide Betriebsarten greifen in die Abschaltautomatik und in die
 * Bedienelemente ein, werden aber erst weiter unten gebaut. */
static bool torch_active(void);
static bool led_fade_active(void);

static void display_idle_tick(lv_timer_t *t)
{
    (void)t;

    if (!bsp_backlight_is_on()) {
        return;
    }
    /* Die Taschenlampe lebt davon, dass der Bildschirm anbleibt, auch wenn
     * ihn niemand berührt. */
    if (torch_active()) {
        return;
    }
    const uint32_t idle = lv_display_get_inactive_time(NULL);
    ESP_LOGD(TAG, "untätig seit %lu ms (Grenze %lu ms)",
             (unsigned long)idle, (unsigned long)s_idle_ms);

    if (s_idle_ms == 0 || idle < s_idle_ms) {
        return;
    }
    ESP_LOGI(TAG, "Display nach %lu s ohne Berührung abgeschaltet",
             (unsigned long)(s_idle_ms / 1000));
    bsp_display_sleep(DISPLAY_IDLE_LOCKOUT_MS);
}

/* -------------------------------------------------------------------------- */
/*  LED-Streifen: Bedienung auf Home                                          */
/* -------------------------------------------------------------------------- */

/*
 * Bewusst nur Farbe und Helligkeit. Der Treiber kann mehr — Effekte, Tempo,
 * Farbtemperatur —, aber am Wohnmobilpanel greift man nach Licht, nicht nach
 * einer Effektbibliothek. Die Fähigkeiten bleiben in led_ble.h erhalten und
 * sind jederzeit wieder anzuschließen.
 */

static lv_obj_t *s_led_power;
/** Die ganze Lichtkarte — die Lichtseite hängt sie zu sich herüber. */
static lv_obj_t *s_led_card;
/** Sammelbehälter für Farben, Helligkeit und das verzögerte Ausschalten. */
static lv_obj_t *s_led_controls;
static lv_obj_t *s_led_bright_label;
static lv_obj_t *s_led_fade_btn;

/** Helligkeit in Prozent. Der Streifen meldet nichts zurück, also merken wir. */
static uint8_t s_led_bright = 100;

/**
 * Wählbare Helligkeitsstufen.
 *
 * Unten fein, oben grob — nicht aus Willkür, sondern weil das Auge Helligkeit
 * logarithmisch wahrnimmt: Der Sprung von 2 auf 5 Prozent ist deutlich
 * sichtbar, der von 90 auf 100 kaum. Gleich große Schritte über den ganzen
 * Bereich wären unten zu grob und oben Verschwendung.
 *
 * Die kleinen Werte sind die eigentlich wichtigen: 2 Prozent sind nachts ein
 * Orientierungslicht, mit dem man niemanden weckt.
 */
static const uint8_t s_led_levels[] = {2, 5, 10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
#define LED_LEVEL_COUNT (sizeof(s_led_levels) / sizeof(s_led_levels[0]))

/**
 * Feste Farben statt eines Farbreglers.
 *
 * Ein Regler trifft jeden Farbton, aber keinen zweimal gleich — und im
 * fahrenden Fahrzeug trifft man ihn gar nicht. Sechs Knöpfe decken ab, was man
 * im Wohnmobil wirklich braucht: Orange für den Abend, Weiß für die Arbeit,
 * Rot fürs Nachtsehen, dazu drei Farben für Stimmung.
 */
static const struct {
    const char *name;
    uint32_t rgb;
} s_led_colors[] = {
    {"Orange", 0xFF5A00},   {"Weiß", 0xFFFFFF}, {"Rot", 0xFF0000},
    {"Grün", 0x00FF00},     {"Blau", 0x0064FF}, {"Violett", 0xC800FF},
};
#define LED_COLOR_COUNT (sizeof(s_led_colors) / sizeof(s_led_colors[0]))

/*
 * Die Drossel für Schieberegler ist entfallen: Regler feuerten bei jedem Pixel
 * und mussten gebremst werden, damit der Streifen mitkam. Ein Knopfdruck ist
 * ein Ereignis — da gibt es nichts zu bremsen.
 */

static void led_controls_update(void);
static void led_anim_cancel(void);
static void led_anim_stop(bool restore);
static void led_soft_on(void);
static void led_soft_off(void);

/** Streifen einschalten und den Knopf mitführen. */
static void led_switch_on(void)
{
    if (!lv_obj_has_state(s_led_power, LV_STATE_CHECKED)) {
        led_soft_on();
        lv_obj_add_state(s_led_power, LV_STATE_CHECKED);
        led_controls_update();
    }
}

/**
 * Farbe, Weißtöne und Helligkeit nur zeigen, wenn das Licht an ist.
 *
 * Bei ausgeschaltetem Streifen wären es Bedienelemente ohne Wirkung — man
 * zöge am Regler und nichts geschähe. Ausgeblendet statt gesperrt, weil ein
 * gesperrtes Element trotzdem Platz und Aufmerksamkeit beansprucht.
 */
static void led_controls_update(void)
{
    const bool ready = (led_ble_get_state() == LED_BLE_READY);
    const bool on = ready && lv_obj_has_state(s_led_power, LV_STATE_CHECKED);

    /* Reißt die Verbindung ab, während eine Blende läuft, kann niemand mehr
     * dimmen — die Automatik würde ins Leere zählen. Nachfassen mit der alten
     * Helligkeit wäre dann ebenfalls vergeblich, also ohne. */
    if (!ready) {
        led_anim_stop(false);
    }

    obj_show(s_led_controls, on);
}

static void on_led_power(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);

    /* Wer von Hand schaltet, hebt eine laufende Automatik auf — sonst ginge
     * das gerade eingeschaltete Licht Sekunden später von selbst aus. Ohne
     * Nachfassen der Helligkeit, die neue Blende setzt sie ohnehin gleich. */
    led_anim_stop(false);

    if (lv_obj_has_state(btn, LV_STATE_CHECKED)) {
        led_soft_on();
    } else {
        led_soft_off();
    }
    /* Sofort, nicht erst beim nächsten Auffrischen — sonst hinkt die Anzeige
     * dem Druck um eine halbe Sekunde hinterher. */
    led_controls_update();
}

/** Helligkeitsanzeige nachführen. */
static void led_bright_label_update(void)
{
    lv_label_set_text_fmt(s_led_bright_label, "%u %%", (unsigned)s_led_bright);
}

/** Helligkeit in Stufen — ein Druck, eine Stufe der Leiter. */
static void on_led_bright_step(lv_event_t *e)
{
    const int step = (int)(intptr_t)lv_event_get_user_data(e);

    /* Nächstgelegene Stufe suchen, statt einen Index mitzuführen: Der Wert
     * kann auch von woanders herkommen, und dann muss der Knopf trotzdem
     * sinnvoll weiterzählen. */
    size_t idx = 0;
    for (size_t i = 0; i < LED_LEVEL_COUNT; i++) {
        if (s_led_levels[i] <= s_led_bright) {
            idx = i;
        }
    }

    /* Unten ist bei der kleinsten Stufe Schluss, nicht bei null: Null wäre
     * dunkel, und dafür gibt es den Ausschalter. Eine Regelung, die bis ins
     * Schwarze läuft, lässt einen im Dunkeln nach dem Ausschalter suchen. */
    if (step > 0 && idx + 1 < LED_LEVEL_COUNT) {
        idx++;
    } else if (step < 0 && idx > 0) {
        idx--;
    }

    const int value = s_led_levels[idx];
    if ((uint8_t)value == s_led_bright) {
        return;
    }
    s_led_bright = (uint8_t)value;
    led_bright_label_update();
    /* Von Hand am Wert zu drehen beendet jede Automatik — sonst zöge sie den
     * gerade eingestellten Wert im nächsten Takt wieder weg. */
    led_anim_stop(false);
    led_ble_brightness(s_led_bright);
}

/** Eine der festen Farben wählen. */
static void on_led_color(lv_event_t *e)
{
    const uint32_t rgb = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    /* Ein Farbdruck bei ausgeschaltetem Licht schaltet ein — dann aber weich,
     * wie der Schalter auch. Läuft gerade eine Abschaltautomatik, ist der
     * Farbdruck die Absage daran. */
    if (lv_obj_has_state(s_led_power, LV_STATE_CHECKED)) {
        led_anim_cancel();
    }
    led_switch_on();
    led_ble_color((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
}

/* -------------------------------------------------------------------------- */
/*  Lichtseite                                                                */
/* -------------------------------------------------------------------------- */
/*
 * Eine zweite Ansicht, die nur eines gut können muss: das Licht bedienen. Am
 * Abend steht man davor und will die Farbe wechseln oder dimmen, nicht
 * Solarerträge lesen. Deshalb bekommt die Lichtkarte hier den größten Teil der
 * Fläche und alle sechs Farben in einer Reihe statt umgebrochen.
 *
 * Daneben stehen die beiden Zahlen, die abends wirklich zählen: wie viel
 * Frischwasser noch da ist und wie lange der Akku reicht.
 *
 * ## Umhängen statt verdoppeln
 *
 * Die Lichtkarte wird **nicht** ein zweites Mal gebaut, sondern mit
 * `lv_obj_set_parent()` herübergeholt und beim Schließen zurückgestellt. Zwei
 * Sätze Bedienelemente hätten zwei Zustände — der Schalter auf der einen Seite
 * wüsste nichts von der Farbe auf der anderen, und jede Rückrufkette müsste
 * beide nachführen. So gibt es weiterhin genau einen Satz.
 */

static lv_obj_t *s_light_page;
static lv_obj_t *s_light_slot;     /* Platz der Karte auf dieser Seite      */
static lv_obj_t *s_light_home;     /* wohin sie zurückgehört               */
static lv_obj_t *s_page_water;
static lv_obj_t *s_page_runtime;
static lv_obj_t *s_page_runtime_cap;
static lv_timer_t *s_light_timer;

static void on_torch_open(lv_event_t *e);

/** Text und Farbe einer Anzeige übernehmen. */
static void mirror_label(lv_obj_t *dst, lv_obj_t *src)
{
    if (!dst || !src) {
        return;
    }
    label_set(dst, lv_label_get_text(src));
    const lv_color_t c = lv_obj_get_style_text_color(src, LV_PART_MAIN);
    if (!lv_color_eq(lv_obj_get_style_text_color(dst, LV_PART_MAIN), c)) {
        lv_obj_set_style_text_color(dst, c, LV_PART_MAIN);
    }
}

/*
 * Die beiden Zahlen werden abgeschrieben, nicht neu berechnet.
 *
 * Wasserstand und Restlaufzeit entstehen an mehreren Stellen — bei der
 * Messung, im Fehlerfall, beim Auffrischen. Jede davon auch noch auf diese
 * Seite zeigen zu lassen, hieße, sie alle anzufassen und keine vergessen zu
 * dürfen. Abschreiben hat genau eine Wahrheitsquelle.
 */
static void light_page_tick(lv_timer_t *t)
{
    (void)t;
    /* Den Füllstand holt die Seite direkt beim Messwerk — auf dem
     * Startbildschirm steht er nur noch als Balken, es gibt also keine
     * Beschriftung mehr zum Abschreiben. */
    label_set(s_page_water, water_level_text(water_level_last()->level));
    mirror_label(s_page_runtime, s_batt_runtime);
    (void)s_page_runtime_cap;
}

static void light_page_close(lv_event_t *e)
{
    (void)e;

    if (!s_light_page) {
        return;
    }
    if (s_light_timer) {
        lv_timer_delete(s_light_timer);
        s_light_timer = NULL;
    }

    /* Die Karte zurück an ihren Platz — und zwar an den ersten, sonst säße
     * sie unter dem Knopfkasten statt darüber. */
    lv_obj_set_parent(s_led_card, s_light_home);
    lv_obj_move_to_index(s_led_card, 0);
    lv_obj_set_size(s_led_card, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(s_led_card, 1);

    lv_obj_delete(s_light_page);
    s_light_page = NULL;
    s_light_slot = NULL;
    s_page_water = NULL;
    s_page_runtime = NULL;
    s_page_runtime_cap = NULL;
}

/** Eine große Ablesestelle: kleine Beschriftung, großer Wert. */
static lv_obj_t *page_reading_create(lv_obj_t *parent, const char *caption,
                                     lv_obj_t **caption_out)
{
    lv_obj_t *box = card_create(parent, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 4, LV_PART_MAIN);

    lv_obj_t *cap = lv_label_create(box);
    lv_obj_set_style_text_color(cap, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_label_set_text(cap, caption);
    if (caption_out) {
        *caption_out = cap;
    }

    lv_obj_t *value = lv_label_create(box);
    lv_obj_set_style_text_font(value, &font_ui_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(value, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_label_set_text(value, "—");
    return value;
}

static void light_page_open(void)
{
    if (s_light_page) {
        return;
    }

    s_light_page = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_light_page, BSP_LCD_H_RES, BSP_LCD_V_RES);
    style_flat(s_light_page, COL_BG, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_light_page, 10, LV_PART_MAIN);
    lv_obj_remove_flag(s_light_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_light_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_light_page, 10, LV_PART_MAIN);

    /* Kopfzeile */
    lv_obj_t *head = plain_create(s_light_page, LV_PCT(100), 48);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    title_create(head, "Licht", &font_ui_28);
    lv_obj_t *back = button_create(head, LV_SYMBOL_LEFT "  Zurück", 180, 48);
    lv_obj_add_event_cb(back, light_page_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *body = plain_create(s_light_page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(body, 10, LV_PART_MAIN);

    /* Links die Ablesestellen, rechts das Licht — und rechts ist mehr Platz,
     * denn darum geht es auf dieser Seite. */
    lv_obj_t *side = plain_create(body, 250, LV_PCT(100));
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(side, 10, LV_PART_MAIN);

    s_page_water = page_reading_create(side, "Frischwasser", NULL);
    s_page_runtime = page_reading_create(side, "Restlaufzeit", &s_page_runtime_cap);

    lv_obj_t *spacer = plain_create(side, LV_PCT(100), 0);
    lv_obj_set_flex_grow(spacer, 1);

    lv_obj_t *lamp = button_create(side, LV_SYMBOL_IMAGE "  Lampe", LV_PCT(100), 56);
    lv_obj_add_event_cb(lamp, on_torch_open, LV_EVENT_CLICKED, NULL);

    s_light_slot = plain_create(body, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(s_light_slot, 1);

    /* Karte herüberholen. Die Breite hier ist großzügig, deshalb passen alle
     * sechs Farbknöpfe in eine Reihe statt in zwei. */
    s_light_home = lv_obj_get_parent(s_led_card);
    lv_obj_set_parent(s_led_card, s_light_slot);
    lv_obj_set_flex_grow(s_led_card, 0);
    lv_obj_set_size(s_led_card, LV_PCT(100), LV_PCT(100));

    s_light_timer = lv_timer_create(light_page_tick, 500, NULL);
    light_page_tick(NULL);
}

/* -------------------------------------------------------------------------- */
/*  Taschenlampe: der Bildschirm selbst als Lichtquelle                       */
/* -------------------------------------------------------------------------- */
/*
 * Eine weiße Fläche über den ganzen Schirm. 800 × 480 Bildpunkte auf Weiß bei
 * voller Hintergrundbeleuchtung sind hell genug, um im Stauraum etwas zu
 * finden — und immer da, auch wenn der LED-Streifen nicht antwortet.
 *
 * Sie bleibt an, bis sie ausgeschaltet wird — eine Lampe, die von selbst
 * erlischt, während man mit beiden Händen arbeitet, ist keine. Wer sie nur
 * kurz zum Aussteigen braucht, nimmt "Aus in 20 s": Dann zählt sie herunter
 * und geht von allein aus, und man muss im Dunkeln nichts mehr suchen.
 *
 * Immer weiß. Farben hatte diese Fläche einmal, sie bringen aber nur
 * Bedienelemente auf eine Fläche, deren einziger Zweck Helligkeit ist — und
 * jede Farbe außer Weiß macht sie dunkler.
 *
 * Solange sie leuchtet, ist die Abschaltautomatik des Displays ausgesetzt —
 * eine Lampe, die nach zwanzig Sekunden ohne Berührung ausgeht, wäre genau
 * falsch herum gedacht.
 */

/** Laufzeit der befristeten Betriebsart. */
#define TORCH_TIMED_MS 20000


static lv_obj_t *s_torch;
static lv_obj_t *s_torch_label;
static lv_timer_t *s_torch_timer;
static lv_obj_t *s_torch_delay_btn;
static uint32_t s_torch_until; /* 0 = dauerhaft, sonst Abschaltzeitpunkt */

/** true, solange die Taschenlampe leuchtet. */
static bool torch_active(void)
{
    return s_torch != NULL;
}

static void torch_close(lv_event_t *e)
{
    (void)e;

    if (s_torch_timer) {
        lv_timer_delete(s_torch_timer);
        s_torch_timer = NULL;
    }
    if (s_torch) {
        lv_obj_delete(s_torch);
        s_torch = NULL;
        s_torch_label = NULL;
        s_torch_delay_btn = NULL;
    }
}

static void torch_tick(lv_timer_t *t)
{
    (void)t;

    if (s_torch_until == 0) {
        lv_label_set_text(s_torch_label, "an");
        return;
    }

    const uint32_t now = lv_tick_get();
    if ((int32_t)(now - s_torch_until) >= 0) {
        torch_close(NULL);
        return;
    }
    lv_label_set_text_fmt(s_torch_label, "noch %u s",
                          (unsigned)((s_torch_until - now + 999) / 1000));
}

static void torch_tick(lv_timer_t *t);

/** Verzögertes Ausschalten scharf stellen oder wieder abbrechen. */
static void on_torch_delay(lv_event_t *e)
{
    (void)e;

    if (s_torch_until) {
        s_torch_until = 0;
        lv_obj_remove_state(s_torch_delay_btn, LV_STATE_CHECKED);
    } else {
        s_torch_until = lv_tick_get() + TORCH_TIMED_MS;
        lv_obj_add_state(s_torch_delay_btn, LV_STATE_CHECKED);
    }
    torch_tick(NULL);
}

static void on_torch_open(lv_event_t *e)
{
    (void)e;

    if (torch_active()) {
        return;
    }

    s_torch = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_torch, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_torch, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_torch, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_torch, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_torch, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_torch, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_torch, LV_OBJ_FLAG_SCROLLABLE);

    /* Die Bedienelemente sitzen unten und nehmen so wenig leuchtende Fläche
     * weg wie möglich — aber groß genug, um sie mit klammen Fingern im
     * Dunkeln zu treffen. Zwei Reihen: oben die Farben, unten das Schalten. */
    lv_obj_t *panel = plain_create(s_torch, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(panel, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(panel, 12, LV_PART_MAIN);

    lv_obj_t *bar = plain_create(panel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 12, LV_PART_MAIN);

    lv_obj_t *off = button_create(bar, LV_SYMBOL_POWER "  Aus", 180, 64);
    lv_obj_add_event_cb(off, torch_close, LV_EVENT_CLICKED, NULL);

    s_torch_label = lv_label_create(bar);
    lv_obj_set_style_text_font(s_torch_label, &font_ui_20, LV_PART_MAIN);
    /* Dunkle Schrift auf der weißen Fläche — die Grundfarbe der Oberfläche
     * wäre hier unlesbar. */
    lv_obj_set_style_text_color(s_torch_label, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_width(s_torch_label, 140);
    lv_obj_set_style_text_align(s_torch_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    s_torch_delay_btn = button_create(bar, "Aus in 20 s", 220, 64);
    lv_obj_add_flag(s_torch_delay_btn, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_event_cb(s_torch_delay_btn, on_torch_delay, LV_EVENT_CLICKED, NULL);

    /* Dauerhaft an, bis jemand etwas anderes sagt. */
    s_torch_until = 0;
    s_torch_timer = lv_timer_create(torch_tick, 200, NULL);
    torch_tick(NULL);
}

/* -------------------------------------------------------------------------- */
/*  Weiches Ein- und Ausschalten, Nachlauf                                    */
/* -------------------------------------------------------------------------- */
/*
 * Der Streifen kann hart schalten — das sieht man ihm aber an, und nachts tut
 * es weh. Deshalb geht hier nichts schlagartig an oder aus: Eingeschaltet wird
 * bei minimaler Helligkeit und dann hochgezogen, ausgeschaltet umgekehrt.
 *
 * Gedimmt wird über den Helligkeitsbefehl, nicht über die Farbe: So bleibt der
 * eingestellte Farbton bis zuletzt erhalten.
 *
 * Dasselbe Werk trägt zwei Betriebsarten:
 *
 *   - **weich** (LED_SOFT_MS): der normale Ein-/Ausschalter.
 *   - **Nachlauf**: erst LED_FADE_HOLD_MS unverändert stehen bleiben — Zeit,
 *     ins Bett zu kommen —, dann über LED_FADE_RAMP_MS herunter und aus.
 */

/** Dauer des weichen Ein- und Ausschaltens. */
#define LED_SOFT_MS 2000
/** Wie lange der Nachlauf das Licht unverändert stehen lässt. */
#define LED_FADE_HOLD_MS 20000
/** Wie lange das Herunterdimmen danach dauert. */
#define LED_FADE_RAMP_MS 10000
/**
 * Takt der Automatik.
 *
 * Mehr als acht Befehle je Sekunde verträgt der Streifen nicht — daran ist
 * früher schon der Farbregler gescheitert, der ungebremst Dutzende schickte
 * und die Verbindung abreißen ließ. 120 ms sind der erprobte Abstand.
 */
#define LED_ANIM_TICK_MS 120
/**
 * Unterste Stufe, die noch gesendet wird.
 *
 * Nicht null: Manche Streifen schalten bei null in eine eigene Abschaltung und
 * kommen beim Hochfahren verzögert zurück. Ein Prozent ist dunkel genug.
 */
#define LED_DIM_FLOOR 1

static lv_timer_t *s_led_anim;
static uint32_t s_led_anim_start;
static uint32_t s_led_anim_hold;  /* Haltezeit vor der Rampe               */
static uint32_t s_led_anim_ramp;  /* Dauer der Rampe                       */
static uint8_t s_led_anim_from;
static uint8_t s_led_anim_to;
static bool s_led_anim_off_at_end;

/** Läuft gerade eine Auf- oder Abblende? */
static bool led_fade_active(void)
{
    return s_led_anim != NULL;
}

/** Beschriftung des Nachlauf-Knopfes zurücksetzen. */
static void led_fade_btn_reset(void)
{
    if (s_led_fade_btn) {
        lv_obj_remove_state(s_led_fade_btn, LV_STATE_CHECKED);
        lv_label_set_text(lv_obj_get_child(s_led_fade_btn, 0), "Aus in 20 s");
    }
}

/**
 * Laufende Automatik abbrechen.
 *
 * @param restore true = die eingestellte Helligkeit wieder ins Gerät schreiben.
 *                Bei einem Verbindungsabriss sinnlos, also abschaltbar.
 */
static void led_anim_stop(bool restore)
{
    if (!s_led_anim) {
        return;
    }
    lv_timer_delete(s_led_anim);
    s_led_anim = NULL;
    if (restore) {
        led_ble_brightness(s_led_bright);
    }
    led_fade_btn_reset();
}

static void led_anim_cancel(void)
{
    led_anim_stop(true);
}

static void led_anim_tick(lv_timer_t *t)
{
    (void)t;
    const uint32_t elapsed = lv_tick_get() - s_led_anim_start;

    if (elapsed < s_led_anim_hold) {
        /* Haltephase. Der Knopf zählt mit, damit man sieht, dass etwas läuft
         * und wie viel Zeit noch bleibt. */
        if (s_led_fade_btn) {
            lv_label_set_text_fmt(lv_obj_get_child(s_led_fade_btn, 0), "Aus in %u s",
                                  (unsigned)((s_led_anim_hold - elapsed + 999) / 1000));
        }
        return;
    }

    const uint32_t ramp = elapsed - s_led_anim_hold;
    if (ramp < s_led_anim_ramp) {
        /* Geometrisch, nicht linear.
         *
         * Das Auge nimmt Helligkeit logarithmisch wahr. Eine lineare Rampe von
         * 100 auf 1 verbringt die meiste Zeit in einem Bereich, in dem man
         * kaum etwas sieht, und stürzt dann durch die letzten Prozente, wo
         * jeder Schritt auffällt — genau die groben Stufen am Ende. Bei
         * gleichem Verhältnis je Schritt sind die Stufen für das Auge gleich
         * groß, und die Blende läuft durchgehend ruhig. */
        const float t = (float)ramp / (float)s_led_anim_ramp;
        const float value = (float)s_led_anim_from *
                            powf((float)s_led_anim_to / (float)s_led_anim_from, t);
        const int step = (int)(value + 0.5f);
        led_ble_brightness((uint8_t)(step < LED_DIM_FLOOR ? LED_DIM_FLOOR : step));
        if (s_led_anim_off_at_end && s_led_anim_hold && s_led_fade_btn) {
            lv_label_set_text(lv_obj_get_child(s_led_fade_btn, 0), "dimmt …");
        }
        return;
    }

    /* Ziel erreicht. */
    lv_timer_delete(s_led_anim);
    s_led_anim = NULL;

    if (s_led_anim_off_at_end) {
        led_ble_power(false);
        /* Bewusst **kein** Zurückschreiben der Helligkeit hinterher: Ein
         * Helligkeitsbefehl an einen ausgeschalteten Streifen bringt manche
         * Modelle dazu, wieder anzugehen. Nötig ist es auch nicht — das weiche
         * Einschalten setzt den Wert ohnehin selbst. */
        lv_obj_remove_state(s_led_power, LV_STATE_CHECKED);
    } else {
        led_ble_brightness(s_led_anim_to);
    }
    led_fade_btn_reset();
    led_controls_update();
}

/** Rampe starten. `hold_ms` = 0 für sofortiges Dimmen. */
static void led_anim_start(uint8_t from, uint8_t to, uint32_t hold_ms, uint32_t ramp_ms,
                           bool off_at_end)
{
    led_anim_stop(false);
    s_led_anim_from = from;
    s_led_anim_to = to;
    s_led_anim_hold = hold_ms;
    s_led_anim_ramp = ramp_ms;
    s_led_anim_off_at_end = off_at_end;
    s_led_anim_start = lv_tick_get();
    s_led_anim = lv_timer_create(led_anim_tick, LED_ANIM_TICK_MS, NULL);
}

/** Weich einschalten: Strom an, ganz dunkel, dann hoch. */
static void led_soft_on(void)
{
    led_ble_power(true);
    led_ble_brightness(LED_DIM_FLOOR);
    led_anim_start(LED_DIM_FLOOR, s_led_bright, 0, LED_SOFT_MS, false);
}

/** Weich ausschalten: herunter, dann Strom aus. */
static void led_soft_off(void)
{
    led_anim_start(s_led_bright, LED_DIM_FLOOR, 0, LED_SOFT_MS, true);
}

static void on_led_fade(lv_event_t *e)
{
    (void)e;

    if (led_fade_active()) {
        led_anim_cancel();
        return;
    }
    led_anim_start(s_led_bright, LED_DIM_FLOOR, LED_FADE_HOLD_MS, LED_FADE_RAMP_MS, true);
    lv_obj_add_state(s_led_fade_btn, LV_STATE_CHECKED);
}

/** Sperrt die Bedienelemente, solange keine Verbindung steht. */
static void led_refresh(lv_timer_t *t)
{
    (void)t;

    const led_ble_state_t state = led_ble_get_state();
    const bool ready = (state == LED_BLE_READY);

    if (ready) {
        lv_obj_remove_state(s_led_power, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(s_led_power, LV_STATE_DISABLED);
        /* Verbindung weg heißt auch: Der Streifen ist nicht mehr unter
         * Kontrolle. Der Knopf darf dann nicht eingerastet stehen bleiben und
         * Licht behaupten, das niemand mehr schaltet. */
        lv_obj_remove_state(s_led_power, LV_STATE_CHECKED);
    }
    led_controls_update();
}


/* -------------------------------------------------------------------------- */
/*  Akku-Details                                                              */
/* -------------------------------------------------------------------------- */

static lv_obj_t *s_batt_dialog;

static void on_batt_dialog_close(lv_event_t *e)
{
    (void)e;
    if (s_batt_dialog) {
        lv_obj_delete(s_batt_dialog);
        s_batt_dialog = NULL;
    }
}

/** Wertzeile im Detail-Dialog: Beschriftung links, Wert rechts, gleich gesetzt. */
static void detail_row(lv_obj_t *parent, const char *caption, const char *value,
                       uint32_t color)
{
    lv_obj_t *v = row_value_create(parent, caption);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_color(v, lv_color_hex(color), LV_PART_MAIN);
}

/** Anzahl der Stützstellen und der Zeitraum der beiden Verlaufsgrafiken. */
#define BATT_CHART_POINTS    60
#define BATT_CHART_WINDOW_MS (5u * 24 * 60 * 60 * 1000)

/* Die Reihen liegen statisch, nicht auf dem Stapel: 120 Werte je Grafik sind
 * knapp ein Kilobyte, und der LVGL-Stapel ist schon einmal übergelaufen. */
static int32_t s_chart_current[BATT_CHART_POINTS];
static int32_t s_chart_capacity[BATT_CHART_POINTS];

/** Tausendstel-Wert mit einer Nachkommastelle, deutsches Komma. */
static void fmt_milli(char *buf, size_t n, int32_t milli, const char *unit)
{
    const int32_t a = (milli < 0) ? -milli : milli;
    snprintf(buf, n, "%s%ld,%ld %s", (milli < 0) ? "-" : "", (long)(a / 1000),
             (long)((a % 1000) / 100), unit);
}

/** Kleine, gedämpfte Achsenbeschriftung. */
static lv_obj_t *axis_label_create(lv_obj_t *parent, const char *text,
                                   lv_text_align_t align)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_color(label, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
    lv_label_set_text(label, text);
    return label;
}

/**
 * Eine Verlaufsgrafik mit beschrifteten Achsen anlegen.
 *
 * LVGL 9 hat für das Diagramm keine eingebauten Achsenbeschriftungen mehr —
 * die Teilstriche von früher sind entfallen. Deshalb hier von Hand: links die
 * Spannweite der Werte, unten der Zeitraum. Das ist ohnehin die passendere
 * Lösung, weil die X-Achse keine Zahlen tragen soll, sondern "vor 5 Tagen"
 * und "jetzt".
 *
 * `values` darf BATT_HIST_NONE enthalten — dort zeichnet LVGL eine Lücke,
 * statt eine Linie durch nicht vorhandene Messwerte zu ziehen.
 */
static void batt_chart_create(lv_obj_t *parent, const char *title,
                              const int32_t *values, size_t points, uint32_t color,
                              const char *unit, bool include_zero, const char *ago)
{
    lv_obj_t *box = plain_create(parent, LV_PCT(100), 150);
    lv_obj_set_flex_grow(box, 1);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 4, LV_PART_MAIN);

    lv_obj_t *head = lv_label_create(box);
    lv_obj_set_style_text_color(head, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_label_set_text(head, title);

    /* Spannweite aus den Daten. Beim Strom gehört die Null dazu, sonst sieht
     * ein Verlauf zwischen 0,1 und 0,2 A aus wie ein Gebirge. */
    int32_t lo = INT32_MAX, hi = INT32_MIN;
    size_t valid = 0;
    for (size_t i = 0; i < points; i++) {
        if (values[i] == BATT_HIST_NONE) {
            continue;
        }
        valid++;
        if (values[i] < lo) lo = values[i];
        if (values[i] > hi) hi = values[i];
    }

    /* Eine Linie braucht zwei Punkte. Mit weniger wäre die Fläche leer, und
     * eine leere Fläche sieht aus wie ein Fehler statt wie „noch zu früh". */
    if (valid < 2) {
        lv_obj_t *hint = lv_label_create(box);
        lv_obj_set_style_text_color(hint, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
        lv_label_set_text(hint, "noch keine Daten");
        return;
    }

    if (include_zero) {
        if (lo > 0) lo = 0;
        if (hi < 0) hi = 0;
    }
    if (lo == hi) {
        lo -= 100;
        hi += 100;
    }

    /* ---- Zeile: Y-Beschriftung, dann die Zeichenfläche ------------------ */
    lv_obj_t *row = plain_create(box, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);

    lv_obj_t *yaxis = plain_create(row, 64, LV_PCT(100));
    lv_obj_set_flex_flow(yaxis, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(yaxis, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_END);

    char buf[24];
    fmt_milli(buf, sizeof(buf), hi, unit);
    axis_label_create(yaxis, buf, LV_TEXT_ALIGN_RIGHT);
    /* Die Null nur dann eigens ausweisen, wenn sie zwischen den Enden liegt —
     * beim Strom trennt sie Laden von Entladen. */
    if (lo < 0 && hi > 0) {
        axis_label_create(yaxis, "0", LV_TEXT_ALIGN_RIGHT);
    }
    fmt_milli(buf, sizeof(buf), lo, unit);
    axis_label_create(yaxis, buf, LV_TEXT_ALIGN_RIGHT);

    lv_obj_t *chart = lv_chart_create(row);
    lv_obj_set_height(chart, LV_PCT(100));
    lv_obj_set_flex_grow(chart, 1);
    /* Das Diagramm ist von Haus aus selbst scrollbar. Wer im Dialog über eine
     * Grafik wischt, verschiebt dann deren Inhalt statt den Dialog — das war
     * das Zappeln. Ohne das Merkmal reicht LVGL die Geste an den Rollbereich
     * darüber weiter. */
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    style_flat(chart, COL_BG, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(chart, 2, LV_PART_MAIN);
    lv_obj_set_style_line_color(chart, lv_color_hex(COL_EMPTY), LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 1, LV_PART_MAIN);
    /* Keine Punktmarkierungen: bei bis zu 60 Stützstellen wäre die Linie
     * sonst nur noch eine Perlenkette. */
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart, (lo < 0 && hi > 0) ? 3 : 2, 5);
    lv_chart_set_point_count(chart, (uint32_t)points);
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, lo, hi);

    lv_chart_series_t *ser =
        lv_chart_add_series(chart, lv_color_hex(color), LV_CHART_AXIS_PRIMARY_Y);
    for (size_t i = 0; i < points; i++) {
        lv_chart_set_series_value_by_id(chart, ser, (uint32_t)i,
                                        (values[i] == BATT_HIST_NONE)
                                            ? LV_CHART_POINT_NONE
                                            : values[i]);
    }

    /* ---- Zeile: Zeitachse ---------------------------------------------- */
    lv_obj_t *xrow = plain_create(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(xrow, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_left(xrow, 70, LV_PART_MAIN); /* unter der Zeichenfläche */

    /* Beide Beschriftungen teilen sich die Breite; die Ausrichtung schiebt sie
     * an die jeweiligen Enden der Zeitachse. Ohne flex_grow wären beide je
     * 100 % breit und die Zeile liefe über. */
    lv_obj_set_flex_grow(axis_label_create(xrow, ago, LV_TEXT_ALIGN_LEFT), 1);
    lv_obj_set_flex_grow(axis_label_create(xrow, "jetzt", LV_TEXT_ALIGN_RIGHT), 1);
}

static lv_obj_t *s_solar_dialog;

static void on_solar_dialog_close(lv_event_t *e)
{
    (void)e;
    if (s_solar_dialog) {
        lv_obj_delete(s_solar_dialog);
        s_solar_dialog = NULL;
    }
}

/**
 * Einzelheiten zum Laderegler.
 *
 * Auf dem Startbildschirm steht nur noch die Sonnenreihe — bewusst, denn
 * unterwegs will man sehen, ob Ertrag da ist, nicht wie viel genau. Wer es
 * genau wissen will, tippt die Sonnen an und bekommt hier alles, was das
 * Gerät sendet.
 */
static void on_solar_details(lv_event_t *e)
{
    (void)e;

    if (s_solar_dialog) {
        return;
    }

    victron_data_t d;
    victron_get(&d);

    s_solar_dialog = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_solar_dialog, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_solar_dialog, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_solar_dialog, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_solar_dialog, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_solar_dialog, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_solar_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_solar_dialog, on_solar_dialog_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = card_create(s_solar_dialog, 520, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *head = plain_create(card, LV_PCT(100), 40);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    title_create(head, "Solar", &font_ui_28);
    lv_obj_t *close = button_create(head, "Schließen", 150, 40);
    lv_obj_add_event_cb(close, on_solar_dialog_close, LV_EVENT_CLICKED, NULL);

    if (!d.valid) {
        lv_obj_t *hint = lv_label_create(card);
        lv_obj_set_style_text_color(hint, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
        lv_label_set_text(hint, ble_scan_is_running()
                                    ? "Noch kein Telegramm empfangen."
                                    : "Der Empfang läuft gerade nicht.");
        return;
    }

    char buf[48];

    snprintf(buf, sizeof(buf), "%u W", d.has_power ? d.solar_power : 0);
    detail_row(card, "Leistung", d.has_power ? buf : "—", COL_TEXT);

    snprintf(buf, sizeof(buf), "%d,%02d V", d.voltage_cv / 100, abs(d.voltage_cv % 100));
    detail_row(card, "Batteriespannung", d.has_voltage ? buf : "—", COL_TEXT);

    snprintf(buf, sizeof(buf), "%d,%d A", d.current_da / 10, abs(d.current_da % 10));
    detail_row(card, "Ladestrom", d.has_current ? buf : "—", COL_TEXT);

    /* Der Ertrag kommt in Vielfachen von 10 Wh — das sind zugleich
     * Hundertstel Kilowattstunden. */
    snprintf(buf, sizeof(buf), "%u,%02u kWh", d.yield_10wh / 100, d.yield_10wh % 100);
    detail_row(card, "Ertrag heute", d.has_yield ? buf : "—", COL_TEXT);

    snprintf(buf, sizeof(buf), "%u,%u A", d.load_da / 10, d.load_da % 10);
    detail_row(card, "Laststrom", d.has_load ? buf : "—", COL_TEXT);

    detail_row(card, "Zustand", victron_state_text(d.device_state), COL_TEXT);
    detail_row(card, "Fehler", victron_error_text(d.charger_error),
               d.charger_error ? COL_ALERT : COL_TEXT_DIM);

    snprintf(buf, sizeof(buf), "%d dBm", d.rssi);
    detail_row(card, "Empfang", buf, COL_TEXT_DIM);

    /* Fehlgeschlagene Prüfbytes heißen fast immer: falscher Schlüssel. Das ist
     * die Zahl, an der man es merkt, bevor man lange sucht. */
    snprintf(buf, sizeof(buf), "%lu  (%lu verworfen)", (unsigned long)d.packets,
             (unsigned long)d.key_errors);
    detail_row(card, "Telegramme", buf, COL_TEXT_DIM);

    const int64_t age = victron_age_ms();
    snprintf(buf, sizeof(buf), "vor %d s", (int)(age / 1000));
    detail_row(card, "Stand", age < 0 ? "—" : buf, COL_TEXT_DIM);
}

static void on_batt_details(lv_event_t *e)
{
    (void)e;

    if (s_batt_dialog) {
        return;
    }

    ble_probe_battery_t b;
    ble_probe_battery(&b);
    if (!b.valid) {
        return;
    }

    s_batt_dialog = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_batt_dialog, BSP_LCD_H_RES, BSP_LCD_V_RES);
    lv_obj_set_style_bg_color(s_batt_dialog, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_batt_dialog, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_batt_dialog, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_batt_dialog, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_batt_dialog, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_batt_dialog, on_batt_dialog_close, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = card_create(s_batt_dialog, 780, 456);
    lv_obj_center(card);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, LV_PART_MAIN);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *head = plain_create(card, LV_PCT(100), 40);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    title_create(head, "Akku", &font_ui_28);
    lv_obj_t *close = button_create(head, "Schließen", 150, 40);
    lv_obj_add_event_cb(close, on_batt_dialog_close, LV_EVENT_CLICKED, NULL);

    /* Zwei Spalten: links die Kennzahlen des Packs, rechts die Zellen — was
     * man zusammen liest, steht zusammen. */
    /* Der Kopf bleibt stehen, nur der Inhalt darunter rollt. Sonst wäre der
     * Knopf "Schließen" nach dem ersten Wisch verschwunden. */
    lv_obj_t *body = plain_create(card, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 12, LV_PART_MAIN);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    /* Kein Nachlauf, kein Überziehen an den Enden. Beides ist auf einem
     * gemächlich zeichnenden RGB-Panel keine Eleganz, sondern Unruhe: Der
     * Inhalt läuft nach dem Loslassen weiter und federt zurück, während die
     * Grafiken noch neu gezeichnet werden. */
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLL_CHAIN);

    lv_obj_t *cols = plain_create(body, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(cols, 20, LV_PART_MAIN);

    lv_obj_t *left = plain_create(cols, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 6, LV_PART_MAIN);

    lv_obj_t *right = plain_create(cols, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 6, LV_PART_MAIN);

    char buf[48];

    snprintf(buf, sizeof(buf), "%u %%", b.soc);
    detail_row(left, "Ladung", buf, COL_TEXT);
    snprintf(buf, sizeof(buf), "%u %%", b.soh);
    detail_row(left, "Gesundheit", buf, COL_TEXT);
    snprintf(buf, sizeof(buf), "%lu,%02lu V", (unsigned long)(b.voltage_mv / 1000),
             (unsigned long)((b.voltage_mv % 1000) / 10));
    detail_row(left, "Spannung", buf, COL_TEXT);
    {
        const long ma = (long)b.current_ma;
        const long a = ma < 0 ? -ma : ma;
        snprintf(buf, sizeof(buf), "%s%ld,%02ld A", ma < 0 ? "-" : "", a / 1000,
                 (a % 1000) / 10);
        detail_row(left, b.current_ma < 0 ? "Strom (Entladen)" : "Strom (Laden)",
                   buf, COL_TEXT);
    }
    snprintf(buf, sizeof(buf), "%lu,%lu Ah", (unsigned long)(b.remaining_mah / 1000),
             (unsigned long)((b.remaining_mah % 1000) / 100));
    detail_row(left, "Restkapazität", buf, COL_TEXT);
    snprintf(buf, sizeof(buf), "%lu,%lu Ah", (unsigned long)(b.design_mah / 1000),
             (unsigned long)((b.design_mah % 1000) / 100));
    detail_row(left, "Nennkapazität", buf, COL_TEXT);
    snprintf(buf, sizeof(buf), "%u", b.cycles);
    detail_row(left, "Ladezyklen", buf, COL_TEXT);
    detail_row(left, "Firmware", b.version[0] ? b.version : "—", COL_TEXT_DIM);

    /* Zellen mit Kennzeichnung der höchsten und niedrigsten — die Spreizung
     * ist das, was man an einem LiFePO4-Pack im Auge behalten muss. */
    uint16_t lo = 0xFFFF, hi = 0;
    for (uint8_t i = 0; i < b.cell_count; i++) {
        if (b.cell_mv[i] < lo) lo = b.cell_mv[i];
        if (b.cell_mv[i] > hi) hi = b.cell_mv[i];
    }
    for (uint8_t i = 0; i < b.cell_count; i++) {
        char cap[16];
        snprintf(cap, sizeof(cap), "Zelle %u", i + 1);
        snprintf(buf, sizeof(buf), "%u,%03u V", b.cell_mv[i] / 1000, b.cell_mv[i] % 1000);
        const uint32_t col = (b.cell_count > 1 && (b.cell_mv[i] == lo || b.cell_mv[i] == hi))
                                 ? COL_WARN : COL_TEXT;
        detail_row(right, cap, buf, col);
    }
    if (b.cell_count > 1) {
        snprintf(buf, sizeof(buf), "%u mV", (unsigned)(hi - lo));
        detail_row(right, "Spreizung", buf, (hi - lo) > 100 ? COL_ALERT : COL_TEXT);
    }

    snprintf(buf, sizeof(buf), "%d %d %d %d %d %d °C", b.temp[0], b.temp[1], b.temp[2],
             b.temp[3], b.temp[4], b.temp[5]);
    detail_row(right, "Temperaturen", buf, COL_TEXT);

    snprintf(buf, sizeof(buf), "%s / %s", b.charge_mos ? "an" : "aus",
             b.discharge_mos ? "an" : "aus");
    detail_row(right, "MOSFET Laden/Entladen", buf, COL_TEXT);
    detail_row(right, "Balancer", b.balancing ? "aktiv" : "aus", COL_TEXT);

    const int64_t age = ble_probe_battery_age_ms();
    snprintf(buf, sizeof(buf), "vor %d s", (int)(age / 1000));
    detail_row(right, "Stand", age < 0 ? "—" : buf, COL_TEXT_DIM);

    /* ---- Verlauf: höchstens fünf Tage, sonst so weit wie vorhanden ----- */
    uint32_t window = batt_history_span_ms();
    if (window > BATT_CHART_WINDOW_MS) {
        window = BATT_CHART_WINDOW_MS;
    }
    /* Eine Stützstelle je Fach, gedeckelt — feiner als die Fächer geht nicht,
     * und mehr Punkte als Bildpunkte bringen nichts. */
    size_t points = window / BATT_HIST_SLOT_MS;
    if (points > BATT_CHART_POINTS) {
        points = BATT_CHART_POINTS;
    }
    batt_history_series(window, points, s_chart_current, s_chart_capacity);

    /* Zweimal dieselbe Dauer, einmal für die Überschrift und einmal für die
     * Zeitachse: "Strom · 5 Tage", aber "vor 5 Tagen". */
    char span[24], ago[32];
    if (window >= 48u * 60 * 60 * 1000) {
        const unsigned d = (unsigned)(window / (24u * 3600 * 1000));
        snprintf(span, sizeof(span), "%u Tage", d);
        snprintf(ago, sizeof(ago), "vor %u Tagen", d);
    } else if (window >= 2u * 60 * 60 * 1000) {
        const unsigned h = (unsigned)(window / (3600u * 1000));
        snprintf(span, sizeof(span), "%u h", h);
        snprintf(ago, sizeof(ago), "vor %u h", h);
    } else {
        const unsigned m = (unsigned)(window / (60u * 1000));
        snprintf(span, sizeof(span), "%u min", m);
        snprintf(ago, sizeof(ago), "vor %u min", m);
    }

    char t1[48], t2[48];
    snprintf(t1, sizeof(t1), "Strom · %s", span);
    snprintf(t2, sizeof(t2), "Kapazität · %s", span);

    /* Nebeneinander und flach: Je weniger Fläche sich beim Rollen neu zeichnen
     * muss, desto ruhiger läuft es. Zwei halbe Breiten reichen für den
     * Verlauf — es geht um den Trend, nicht um einzelne Messwerte. */
    lv_obj_t *charts = plain_create(body, LV_PCT(100), 150);
    lv_obj_set_flex_flow(charts, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(charts, 16, LV_PART_MAIN);

    batt_chart_create(charts, t1, s_chart_current, points, COL_ACCENT, "A", true, ago);
    batt_chart_create(charts, t2, s_chart_capacity, points, COL_WARN, "Ah", false, ago);
}

/* -------------------------------------------------------------------------- */
/*  Seite "Home"                                                              */
/* -------------------------------------------------------------------------- */
/*  Seite "Home"                                                              */
/* -------------------------------------------------------------------------- */

/*
 * Aufteilung: links die beiden Tanks übereinander, beide waagerecht und im
 * selben Aufbau — Balken, Zustand, und der zugehörige Verbraucher als
 * quadratischer Symbolknopf gleich daneben. Wer Wasser braucht, greift dort
 * hin, wo der Füllstand steht; das erspart den Weg über eine Bedienleiste.
 *
 * Mitte: Solar zum Ablesen. Rechts: Licht und der Display-Knopf.
 */
/*
 * Drei gleich breite Spalten: Wasser, Strom, Licht.
 *
 * 800 Bildpunkte abzüglich zweimal 10 Rand und zweimal 10 Spaltenabstand
 * lassen 760 übrig, macht 253 je Spalte. Vorher war die Lichtspalte schmaler
 * als die anderen beiden — das sah nach Rangordnung aus, die es nicht gibt.
 */
#define HOME_COL_W 253
/** Kantenlänge der quadratischen Symbolknöpfe. */





static void page_home_create(lv_obj_t *page)
{
    /*
     * ## Aufbau
     *
     * Oben eine Kopfzeile mit allem, was man **ablesen** will: Abwasser,
     * Frischwasser, Sonne, Akku — nebeneinander über die ganze Breite, jeweils
     * als Bild statt als Zahlenkolonne. Darunter der ganze Rest des Schirms
     * für das, was man **bedienen** will: das Licht.
     *
     * Die Trennung ist der Grund für den Umbau. Vorher lagen Anzeigen und
     * Schalter in drei Spalten nebeneinander, und jede Spalte hatte beides.
     * Wer nur nachsehen wollte, musste dreimal hinsehen; wer nur schalten
     * wollte, musste an Anzeigen vorbei zielen.
     */
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, 10, LV_PART_MAIN);

    /* ---- Kopfzeile ----------------------------------------------------- */
    lv_obj_t *head = card_create(page, LV_PCT(100), 76);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, 10, LV_PART_MAIN);

    /* -- Abwasser: ein Zeichen, das die Farbe wechselt -- */
    s_waste_label = lv_label_create(head);
    lv_obj_set_style_text_font(s_waste_label, &font_waste_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_waste_label, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_label_set_text(s_waste_label, SYMBOL_WASTE);

    /* -- Frischwasser: offener Behälter mit drei Segmenten -- */
    /*
     * Rundum geschlossen und mit Luft nach oben.
     *
     * Erst war der Tank oben offen — als Bild für einen Behälter, in den
     * etwas hineinläuft. Gelesen wurde das aber nicht als Gestaltung, sondern
     * als abgeschnittene Linie, zweimal hintereinander. Dazu kam, dass er mit
     * 46 Bildpunkten fast die ganze Innenhöhe der Kopfzeile ausfüllte und oben
     * tatsächlich beschnitten wurde. Beides ist jetzt behoben: ganzer Rahmen,
     * 38 statt 46 hoch, und reichlich Polster zwischen Rand und Segmenten.
     */
    lv_obj_t *tank = plain_create(head, 130, 38);
    lv_obj_set_style_border_width(tank, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(tank, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_pad_all(tank, 5, LV_PART_MAIN);
    lv_obj_set_flex_flow(tank, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tank, 3, LV_PART_MAIN);
    /* Antippen misst nach — der eigene Messknopf ist entfallen, der Wunsch
     * nach einem frischen Wert bleibt. */
    lv_obj_add_flag(tank, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tank, on_tank_clicked, LV_EVENT_CLICKED, NULL);
    /* Sofortige Rückmeldung unter dem Finger, noch bevor gemessen wird. */
    lv_obj_set_style_border_color(tank, lv_color_hex(COL_ACCENT),
                                  LV_PART_MAIN | LV_STATE_PRESSED);
    s_tank_frame = tank;

    for (int i = 0; i < WATER_ELECTRODE_COUNT; i++) {
        s_tank_seg[i] = segment_create(tank);
    }

    /* -- Solar: nur die Sonnenreihe --
     *
     * Die Wattzahl ist entfallen. Zehn Sonnen bis 220 W sagen dasselbe wie die
     * Zahl, nur schneller — genau die Überlegung, die vorher schon den
     * Prozentwert am Akku überflüssig gemacht hat. Wer den genauen Wert will,
     * findet ihn auf der Webseite. */
    lv_obj_t *suns = plain_create(head, 180, 36);
    lv_obj_add_flag(suns, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(suns, on_solar_details, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(suns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(suns, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < BAR_SEGMENTS; i++) {
        lv_obj_t *sun = lv_label_create(suns);
        lv_obj_set_style_text_font(sun, &font_sun_20, LV_PART_MAIN);
        lv_obj_set_style_text_color(sun, lv_color_hex(COL_EMPTY), LV_PART_MAIN);
        lv_label_set_text(sun, SYMBOL_SUN);
        s_solar_seg[i] = sun;
    }

    /* -- Akku: Laden links, Entladen rechts, Restlaufzeit im Balken -- */
    lv_obj_t *batt = plain_create(head, 250, 44);
    lv_obj_set_flex_flow(batt, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(batt, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(batt, 4, LV_PART_MAIN);
    /* Der ganze Akku ist der Knopf zu den Einzelheiten — ein eigener
     * "Details"-Knopf wäre ein zweites Ziel für dieselbe Absicht. */
    lv_obj_add_flag(batt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(batt, on_batt_details, LV_EVENT_CLICKED, NULL);

    /* 66 statt 54: "128 W" plus Pfeil passte vorher nicht hinein und wurde
     * abgeschnitten. */
    lv_obj_t *chg = plain_create(batt, 66, LV_PCT(100));
    lv_obj_set_flex_flow(chg, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chg, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chg, 3, LV_PART_MAIN);

    s_batt_chg_w = lv_label_create(chg);
    lv_obj_set_style_text_color(s_batt_chg_w, lv_color_hex(COL_CHG_HIGH), LV_PART_MAIN);
    lv_label_set_text(s_batt_chg_w, "");

    s_batt_chg_arrow = lv_label_create(chg);
    lv_obj_set_style_text_font(s_batt_chg_arrow, &font_ui_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_batt_chg_arrow, lv_color_hex(COL_CHG_HIGH), LV_PART_MAIN);
    lv_label_set_text(s_batt_chg_arrow, LV_SYMBOL_RIGHT);
    lv_obj_add_flag(s_batt_chg_arrow, LV_OBJ_FLAG_HIDDEN);
    arrow_blink(s_batt_chg_arrow);

    lv_obj_t *shell = plain_create(batt, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(shell, 1);
    lv_obj_set_style_border_width(shell, 2, LV_PART_MAIN);
    lv_obj_set_style_border_color(shell, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_obj_set_style_pad_all(shell, 4, LV_PART_MAIN);
    lv_obj_set_flex_flow(shell, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(shell, 2, LV_PART_MAIN);

    for (int i = 0; i < BAR_SEGMENTS; i++) {
        s_batt_soc_seg[i] = segment_create(shell);
    }

    /* Die Restlaufzeit steht im Gehäuse, auf einem dunklen Schild — ohne das
     * verschwindet Weiß auf der grünen Füllung. */
    s_batt_runtime = lv_label_create(shell);
    lv_obj_add_flag(s_batt_runtime, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_align(s_batt_runtime, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(s_batt_runtime, &font_ui_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_batt_runtime, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_batt_runtime, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_batt_runtime, LV_OPA_80, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(s_batt_runtime, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_ver(s_batt_runtime, 1, LV_PART_MAIN);
    lv_label_set_text(s_batt_runtime, "—");

    lv_obj_t *pole = lv_obj_create(batt);
    lv_obj_set_size(pole, 5, 18);
    style_flat(pole, COL_TEXT_DIM, 0, LV_PART_MAIN);
    lv_obj_remove_flag(pole, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *dis = plain_create(batt, 66, LV_PCT(100));
    lv_obj_set_flex_flow(dis, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dis, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dis, 3, LV_PART_MAIN);

    s_batt_dis_arrow = lv_label_create(dis);
    lv_obj_set_style_text_font(s_batt_dis_arrow, &font_ui_20, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_batt_dis_arrow, lv_color_hex(COL_DIS_LOW), LV_PART_MAIN);
    lv_label_set_text(s_batt_dis_arrow, LV_SYMBOL_RIGHT);
    lv_obj_add_flag(s_batt_dis_arrow, LV_OBJ_FLAG_HIDDEN);
    arrow_blink(s_batt_dis_arrow);

    s_batt_dis_w = lv_label_create(dis);
    lv_obj_set_style_text_color(s_batt_dis_w, lv_color_hex(COL_DIS_LOW), LV_PART_MAIN);
    lv_label_set_text(s_batt_dis_w, "");

    /* -- WLAN: ein Zeichen, dessen Farbe den Pegel trägt -- */
    s_wifi_icon = lv_label_create(head);
    lv_obj_set_style_text_font(s_wifi_icon, &font_ui_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_wifi_icon, lv_color_hex(COL_EMPTY), LV_PART_MAIN);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);

    /* -- Einstellungen -- */
    lv_obj_t *gear = button_create(head, LV_SYMBOL_SETTINGS, 56, 56);
    lv_obj_add_event_cb(gear, idle_dialog_open, LV_EVENT_CLICKED, NULL);

    /* ---- Meldezeilen: im Normalfall unsichtbar -------------------------- */
    s_level_hint = lv_label_create(page);
    lv_obj_set_width(s_level_hint, LV_PCT(100));
    lv_label_set_long_mode(s_level_hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(s_level_hint, lv_color_hex(COL_WARN), LV_PART_MAIN);
    lv_label_set_text(s_level_hint, "");
    lv_obj_add_flag(s_level_hint, LV_OBJ_FLAG_HIDDEN);

    s_batt_alert = lv_label_create(page);
    lv_obj_set_width(s_batt_alert, LV_PCT(100));
    lv_label_set_long_mode(s_batt_alert, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(s_batt_alert, &font_ui_20, LV_PART_MAIN);
    lv_label_set_text(s_batt_alert, "");
    lv_obj_add_flag(s_batt_alert, LV_OBJ_FLAG_HIDDEN);

    s_home_error = lv_label_create(page);
    lv_obj_set_width(s_home_error, LV_PCT(100));
    lv_label_set_long_mode(s_home_error, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_color(s_home_error, lv_color_hex(COL_TEXT_DIM), LV_PART_MAIN);
    lv_label_set_text(s_home_error, "");
    lv_obj_add_flag(s_home_error, LV_OBJ_FLAG_HIDDEN);

    /* ---- Licht: der ganze übrige Schirm --------------------------------- */
    lv_obj_t *light = card_create(page, LV_PCT(100), LV_PCT(100));
    s_led_card = light;
    lv_obj_set_flex_grow(light, 1);
    lv_obj_remove_flag(light, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(light, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(light, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(light, 12, LV_PART_MAIN);

    s_led_power = button_create(light, LV_SYMBOL_POWER "  Licht", LV_PCT(100), 56);
    lv_obj_add_flag(s_led_power, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_event_cb(s_led_power, on_led_power, LV_EVENT_VALUE_CHANGED, NULL);

    s_led_controls = plain_create(light, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_led_controls, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_led_controls, 12, LV_PART_MAIN);

    /* Über die volle Breite passen alle sechs Farben in eine Reihe. */
    lv_obj_t *colors = plain_create(s_led_controls, LV_PCT(100), 52);
    lv_obj_set_flex_flow(colors, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(colors, 10, LV_PART_MAIN);

    for (size_t i = 0; i < LED_COLOR_COUNT; i++) {
        lv_obj_t *btn = lv_button_create(colors);
        lv_obj_set_height(btn, LV_PCT(100));
        lv_obj_set_flex_grow(btn, 1);
        style_flat(btn, s_led_colors[i].rgb, 8, LV_PART_MAIN);
        style_button_states(btn, s_led_colors[i].rgb, s_led_colors[i].rgb);
        lv_obj_add_event_cb(btn, on_led_color, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)s_led_colors[i].rgb);
    }

    lv_obj_t *bright = plain_create(s_led_controls, LV_PCT(100), 52);
    lv_obj_set_flex_flow(bright, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bright, 10, LV_PART_MAIN);

    lv_obj_t *minus = button_create(bright, LV_SYMBOL_MINUS, 90, LV_PCT(100));
    lv_obj_add_event_cb(minus, on_led_bright_step, LV_EVENT_CLICKED,
                        (void *)(intptr_t)-1);

    lv_obj_t *box = plain_create(bright, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(box, 1);
    s_led_bright_label = lv_label_create(box);
    lv_obj_set_style_text_font(s_led_bright_label, &font_ui_28, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_led_bright_label, lv_color_hex(COL_TEXT), LV_PART_MAIN);
    lv_obj_center(s_led_bright_label);
    led_bright_label_update();

    lv_obj_t *plus = button_create(bright, LV_SYMBOL_PLUS, 90, LV_PCT(100));
    lv_obj_add_event_cb(plus, on_led_bright_step, LV_EVENT_CLICKED,
                        (void *)(intptr_t)1);

    s_led_fade_btn = button_create(s_led_controls, "Aus in 20 s", LV_PCT(100), 48);
    lv_obj_add_flag(s_led_fade_btn, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_event_cb(s_led_fade_btn, on_led_fade, LV_EVENT_CLICKED, NULL);

    /* ---- Unten: Lampe und Display aus ----------------------------------- */
    lv_obj_t *ctrl = plain_create(page, LV_PCT(100), 64);
    lv_obj_set_flex_flow(ctrl, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ctrl, 10, LV_PART_MAIN);

    /* Der Trinkwasserschalter ist eine Funktion, keine Anzeige — er bleibt,
     * auch wenn Heizung und Messknopf aus dem Bild genommen wurden. Hier
     * unten steht er bei den anderen Schaltern statt allein in der Kopfzeile
     * zwischen lauter Ablesestellen. */
    lv_obj_t *btn_water = button_create(ctrl, LV_SYMBOL_TINT "  Wasser",
                                        LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(btn_water, 1);
    lv_obj_add_flag(btn_water, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_event_cb(btn_water, on_output_toggled, LV_EVENT_VALUE_CHANGED,
                        (void *)(intptr_t)BSP_OUTPUT_PA0);

    lv_obj_t *btn_torch = button_create(ctrl, LV_SYMBOL_IMAGE "  Lampe",
                                        LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(btn_torch, 1);
    lv_obj_add_event_cb(btn_torch, on_torch_open, LV_EVENT_CLICKED, NULL);

    lv_obj_t *btn_off = button_create(ctrl, LV_SYMBOL_EYE_CLOSE "  Display aus",
                                      LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(btn_off, 1);
    lv_obj_add_event_cb(btn_off, on_display_off, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(btn_off, idle_dialog_open, LV_EVENT_LONG_PRESSED, NULL);
}

/* -------------------------------------------------------------------------- */
/*  Solar: Kurzanzeige auf Home                                               */
/* -------------------------------------------------------------------------- */





/** Einseitigen Balken auf einen Anteil setzen, von links nach rechts. */
static void fill_bar_set(lv_obj_t **seg, int32_t value, int32_t max, uint32_t color)
{
    int lit = 0;
    if (value > 0 && max > 0) {
        lit = (int)((value * BAR_SEGMENTS + max - 1) / max);
        if (lit > BAR_SEGMENTS) {
            lit = BAR_SEGMENTS;
        }
    }
    for (int i = 0; i < BAR_SEGMENTS; i++) {
        segment_set(seg[i], (i < lit) ? color : COL_EMPTY, i < lit);
    }
}

/**
 * Sonnenreihe setzen.
 *
 * Dieselbe Mechanik wie bei den Balken, nur färbt sie Schriftzeichen statt
 * Flächen — und auch hier wird nur angefasst, was sich geändert hat.
 */
static void sun_bar_set(lv_obj_t **sun, int32_t value, int32_t max)
{
    int lit = 0;
    if (value > 0 && max > 0) {
        lit = (int)((value * BAR_SEGMENTS + max - 1) / max);
        if (lit > BAR_SEGMENTS) {
            lit = BAR_SEGMENTS;
        }
    }
    for (int i = 0; i < BAR_SEGMENTS; i++) {
        label_color(sun[i], (i < lit) ? COL_SUN : COL_EMPTY);
    }
}


/**
 * Restlaufzeit in den Ladebalken schreiben.
 *
 * Gerechnet wird mit der mittleren **Entnahme**, nicht mit der Bilanz: Die
 * Bilanz steht an einem sonnigen Tag bei null, und dann gäbe es keine Zahl,
 * sondern nur ein Wort. Die Entnahme beantwortet dagegen immer dieselbe, für
 * ein Wohnmobil entscheidende Frage — wie lange der Akku bei diesem Verbrauch
 * reicht, wenn nichts mehr nachkommt.
 *
 * Die Einheit wächst mit dem Wert: Minuten, solange es um Minuten geht, dann
 * Stunden, dann Tage. "0 Std" wäre für jemanden, der noch zwanzig Minuten
 * Licht hat, die falsche Auskunft.
 */
static void runtime_set(uint32_t remaining_mah)
{
    uint32_t minutes = 0;
    uint32_t covered_ms = 0;

    if (!batt_history_runtime(BATT_RUN_WINDOW_MS, remaining_mah, &minutes, &covered_ms)) {
        label_set(s_batt_runtime, "—");
        return;
    }

    if (minutes < 90) {
        label_set_fmt(s_batt_runtime, "%u Min", (unsigned)minutes);
    } else if (minutes < 48u * 60) {
        label_set_fmt(s_batt_runtime, "%u Std", (unsigned)(minutes / 60));
    } else if (minutes <= 99u * 24 * 60) {
        label_set_fmt(s_batt_runtime, "%u Tage", (unsigned)(minutes / (24 * 60)));
    } else {
        /* Bei winziger Entnahme rechnet die Division Jahre aus. */
        label_set(s_batt_runtime, "> 99 T");
    }

    /* Solange das Fenster erst zu einem kleinen Teil belegt ist, ist die Zahl
     * eine Hochrechnung aus wenigen Minuten — das darf man ihr ansehen. */
    const bool thin = ((uint64_t)covered_ms * 4 < (uint64_t)BATT_RUN_WINDOW_MS);
    label_color(s_batt_runtime, thin ? COL_TEXT_DIM : COL_TEXT);
}

/** Akku-Kurzanzeige nachführen. */
static void battery_refresh(void)
{
    ble_probe_battery_t b;
    ble_probe_battery(&b);
    const int64_t age = ble_probe_battery_age_ms();

    if (!b.valid) {
        label_set(s_batt_runtime, ble_probe_get_state() == BLE_PROBE_READY
                                      ? "wartet" : "—");
        label_color(s_batt_runtime, COL_TEXT_DIM);
        fill_bar_set(s_batt_soc_seg, 0, 100, COL_SOC_OK);
        label_set(s_batt_chg_w, "");
        label_set(s_batt_dis_w, "");
        obj_show(s_batt_chg_arrow, false);
        obj_show(s_batt_dis_arrow, false);
        obj_show(s_batt_alert, false);
        return;
    }

    /* Jeden Datensatz genau einmal in den Verlauf geben. Das Alter taugt zum
     * Erkennen nicht — es wächst zwischen zwei Abfragen ständig. */
    static int64_t s_logged_us = -1;
    const int64_t stamp = ble_probe_battery_stamp_us();
    if (stamp != s_logged_us) {
        s_logged_us = stamp;
        batt_history_add(b.current_ma, b.remaining_mah);
    }


    /* Ladestand: Füllung von links, Farbe nach Lage. Der Prozentwert steht
     * nirgends mehr — er ist genau das, was der Balken ohnehin zeigt. */
    fill_bar_set(s_batt_soc_seg, b.soc, 100,
                 (b.soc <= 20) ? COL_ALERT : (b.soc <= 40) ? COL_WARN : COL_SOC_OK);


    /* Leistung statt Strom: Watt sind die Währung, in der man Verbraucher
     * kennt — der Kühlschrank zieht 45 W, nicht 3,4 A. Gerechnet aus den
     * beiden Werten, die das BMS ohnehin liefert. Unterhalb des Totbereichs
     * gilt glatt null, sonst zappelte die Anzeige am Messrauschen. */
    const int32_t ma = (b.current_ma > BATT_IDLE_MA || b.current_ma < -BATT_IDLE_MA)
                           ? b.current_ma
                           : 0;
    const int32_t watt = (int32_t)(((int64_t)ma * (int64_t)b.voltage_mv) / 1000000);
    const int32_t abs_w = (watt < 0) ? -watt : watt;

    /* Die Leistung steht auf der Seite, auf die sie gehört: Laden links,
     * Entladen rechts. Die jeweils andere Seite bleibt leer, behält aber
     * ihren Platz. */
    char wbuf[16];
    snprintf(wbuf, sizeof(wbuf), "%ld W", (long)abs_w);

    label_set(s_batt_chg_w, (watt > 0) ? wbuf : "");
    label_set(s_batt_dis_w, (watt < 0) ? wbuf : "");
    obj_show(s_batt_chg_arrow, watt > 0);
    obj_show(s_batt_dis_arrow, watt < 0);

    /* Störung? Dann steht sie an der Stelle der Restlaufzeiten.
     *
     * Bewusst nur das, was aus dem Mitschnitt gesichert ist: die beiden
     * MOSFET-Zustände und das Alter der Daten. Die übrigen Bits des
     * Statusworts sind nicht entschlüsselt — wir haben nur Aufzeichnungen aus
     * dem störungsfreien Betrieb. Etwas hineinzudeuten wäre schlimmer als es
     * wegzulassen: eine erfundene Entwarnung ist gefährlicher als gar keine
     * Anzeige. */
    const char *alert = NULL;
    uint32_t alert_color = COL_WARN;

    if (!b.discharge_mos && !b.charge_mos) {
        alert = LV_SYMBOL_WARNING "  Laden und Entladen gesperrt";
        alert_color = COL_ALERT;
    } else if (!b.discharge_mos) {
        alert = LV_SYMBOL_WARNING "  Entladen gesperrt";
        alert_color = COL_ALERT;
    } else if (!b.charge_mos) {
        /* Kein Alarm, sondern der Normalfall am vollen Akku: Das BMS trennt
         * den Ladepfad, wenn nichts mehr hineinpasst. Deshalb gelb. */
        alert = LV_SYMBOL_WARNING "  Laden gesperrt";
        alert_color = COL_WARN;
    } else if (ble_probe_get_state() != BLE_PROBE_READY) {
        /* Die Werte bleiben stehen, wenn die Verbindung abreißt — sie sehen
         * dann aus wie frische Messwerte. Genau das ist im Langzeittest
         * passiert: eine Anzeige, die stundenlang 87 % behauptete. Also
         * ansagen, dass hier nichts mehr nachkommt. */
        alert = LV_SYMBOL_WARNING "  Akku nicht verbunden";
        alert_color = COL_WARN;
    } else if (age > BATT_WATCHDOG_MS) {
        alert = LV_SYMBOL_WARNING "  Daten veraltet";
        alert_color = COL_WARN;
    }

    if (alert) {
        label_set(s_batt_alert, alert);
        label_color(s_batt_alert, alert_color);
        obj_show(s_batt_alert, true);
    } else {
        obj_show(s_batt_alert, false);
    }
    runtime_set(b.remaining_mah);

    /* Veraltete Werte sind gefährlicher als gar keine — nach dem doppelten
     * Abfragetakt wird die Anzeige gedimmt. */
    if ((age > BATT_WATCHDOG_MS) || (ble_probe_get_state() != BLE_PROBE_READY)) {
        label_color(s_batt_runtime, COL_TEXT_DIM);
    }
}

/**
 * WLAN-Zeichen nachführen.
 *
 * Die Farbe trägt den Pegel, weil ein einzelnes Zeichen keine Balken hat:
 * grün ab −60 dBm, gelb bis −75, rot darunter — das ist der Bereich, in dem
 * eine Verbindung zwar steht, aber bei jedem Handschlag wackelt. Grau heißt
 * "sucht noch", und ganz blass heißt "abgeschaltet".
 */
static void wifi_icon_refresh(void)
{
    if (!web_is_enabled()) {
        label_color(s_wifi_icon, COL_EMPTY);
        return;
    }
    if (!web_is_connected()) {
        label_color(s_wifi_icon, COL_TEXT_DIM);
        return;
    }

    const int8_t rssi = web_rssi();
    label_color(s_wifi_icon, (rssi >= -60)   ? COL_SOC_OK
                             : (rssi >= -75) ? COL_WARN
                                             : COL_ALERT);
}

static void solar_refresh(lv_timer_t *t)
{
    (void)t;

    battery_refresh();
    tank_wake_check();
    wifi_icon_refresh();

    victron_data_t d;
    victron_get(&d);

    if (!d.valid) {
        sun_bar_set(s_solar_seg, 0, SOLAR_BAR_MAX_W);
        label_set(s_home_error, ble_scan_is_running() ? "wartet …" : "Empfang aus");
        label_color(s_home_error, COL_TEXT_DIM);
        obj_show(s_home_error, true);
        return;
    }

    if (d.has_power) {
        sun_bar_set(s_solar_seg, (int32_t)d.solar_power, SOLAR_BAR_MAX_W);
    }

    /* Ein Ladefehler gehört auf den Startbildschirm, nicht in eine Unterseite —
     * sonst bemerkt man ihn erst, wenn der Akku leer ist. */
    if (d.charger_error != 0) {
        label_set(s_home_error, victron_error_text(d.charger_error));
        label_color(s_home_error, COL_ALERT);
        obj_show(s_home_error, true);
    } else {
        obj_show(s_home_error, false);
    }
}

/* -------------------------------------------------------------------------- */
/*  Funk von allein, solange das Display an ist                               */
/* -------------------------------------------------------------------------- */

/*
 * Beides soll ohne Zutun laufen: der LED-Streifen verbunden, der Victron-Empfang
 * aktiv. Zwei Dinge sind dabei zu beachten.
 *
 * Erstens kann der Controller **nicht gleichzeitig suchen und verbinden**.
 * Deshalb die feste Reihenfolge: erst verbinden, und erst wenn der Aufbau
 * abgeschlossen ist, den Scan für Victron starten. Eine *bestehende*
 * Verbindung stört den Scan dagegen nicht — der Controller teilt sich die
 * Zeit. Das ist der Unterschied, an dem der Victron-Empfang zwischenzeitlich
 * gescheitert ist: Der Akku bleibt dauerhaft verbunden, und solange das als
 * Scan-Sperre galt, kam nie wieder ein Victron-Telegramm an.
 *
 * Zweitens wird bei dunklem Display nichts gestartet — dann sieht ohnehin
 * niemand hin, und das Funkmodul neben dem RGB-Panel schweigt. Beendet wird
 * dagegen nichts: Ein Wiederverbinden dauert Sekunden, und wer das Display
 * weckt, will das Licht sofort schalten können, nicht erst nach dem nächsten
 * Suchlauf.
 */

/** Abstand zwischen zwei Verbindungsversuchen nach einem Fehlschlag. */
#define AUTO_RETRY_MS 120000

static void probe_tick_cb(lv_timer_t *t)
{
    (void)t;
    ble_probe_tick();
}

static void auto_ble_tick(lv_timer_t *t)
{
    (void)t;
    static uint32_t s_retry_at;

    const uint32_t now = lv_tick_get();
    const ble_probe_state_t probe = ble_probe_get_state();

    /* ---- Akku: Verbindung halten --------------------------------------- */
    /*
     * Der Akku ist die einzige Quelle, die dauerhaft laufen muss — er füllt
     * den Verlauf, aus dem die Restlaufzeit kommt. Deshalb steht dieser Block
     * **vor** der Display-Abfrage: Das Display geht nach 20 s aus, und alles
     * darunter läuft dann nicht mehr. Im Langzeittest hat genau das die
     * Verbindung über Nacht verwaisen lassen; die Anzeige stand danach auf
     * einem eingefrorenen Ladestand.
     *
     * Drei Fälle, drei Antworten:
     *
     *   - **Verbindung weg** (IDLE/FAILED): neu aufbauen. Hatte der Akku schon
     *     einmal geantwortet, zügig — dann ist er nur kurz außer Reichweite
     *     oder die Strecke ist abgerissen. Hat er noch nie geantwortet, mit
     *     langem Abstand, damit ein fremdes oder stummes Gerät nicht dauernd
     *     den Funk belegt und die Hersteller-App aussperrt.
     *   - **Verbindung steht, aber es kommt nichts** (Zombie): Auf dem Papier
     *     ist alles in Ordnung, die Anzeige altert still vor sich hin. Das ist
     *     der gefährlichere Fall, weil er nach Betrieb aussieht. Nach
     *     BATT_WATCHDOG_MS ohne Datensatz wird getrennt und neu aufgebaut.
     *   - **Aufbau läuft**: in Ruhe lassen.
     */
    static bool s_batt_seen;      /* Es kamen schon einmal Daten            */
    static uint32_t s_batt_since; /* Beginn des laufenden Versuchs          */
    static uint32_t s_batt_next;  /* Frühester nächster Versuch             */

    const int64_t batt_age = ble_probe_battery_age_ms();

    switch (probe) {
    case BLE_PROBE_READY:
        if (batt_age >= 0 && batt_age < BATT_WATCHDOG_MS) {
            s_batt_seen = true;
            break; /* alles in Ordnung */
        }
        /* Stumm. Der Verbindung eine Anlaufzeit zugestehen — nach dem
         * Aufbau dauert es ein paar Sekunden bis zur ersten Antwort. */
        if (now - s_batt_since > BATT_WATCHDOG_MS) {
            ESP_LOGW(TAG, "Akku antwortet seit %lld s nicht — Verbindung wird erneuert",
                     (batt_age < 0) ? -1LL : batt_age / 1000);
            ble_probe_disconnect();
            s_batt_next = now + (s_batt_seen ? BATT_RETRY_FAST_MS : BATT_RETRY_SLOW_MS);
            return;
        }
        break;

    case BLE_PROBE_IDLE:
    case BLE_PROBE_FAILED:
        if ((int32_t)(now - s_batt_next) >= 0) {
            s_batt_since = now;
            s_batt_next = now + (s_batt_seen ? BATT_RETRY_FAST_MS : BATT_RETRY_SLOW_MS);
            ESP_LOGI(TAG, "Akku: Verbindungsversuch");
            ble_probe_connect(BLE_PROBE_DEFAULT_MAC);
        }
        return; /* Aufbau läuft oder Wartezeit — Controller nicht stören */

    default:
        return; /* SEARCHING/CONNECTING/… — Aufbau nicht stören */
    }

    if (!bsp_backlight_is_on()) {
        return;
    }

    /* ---- LED-Streifen -------------------------------------------------- */
    const led_ble_state_t led = led_ble_get_state();

    if (led == LED_BLE_SEARCHING || led == LED_BLE_CONNECTING ||
        led == LED_BLE_DISCOVERING) {
        return; /* läuft gerade, nicht dazwischenfunken */
    }

    if (led == LED_BLE_IDLE || (led == LED_BLE_FAILED && now - s_retry_at >= AUTO_RETRY_MS)) {
        s_retry_at = now;
        if (ble_scan_is_running()) {
            ble_scan_stop();
        }
        led_ble_connect();
        return;
    }

    /* Streifen steht (oder ist vorerst aufgegeben) — jetzt Victron mitlesen. */
    if (!ble_scan_is_running()) {
        ble_scan_start();
    }
}

/* -------------------------------------------------------------------------- */
/*  Aufbau                                                                    */
/* -------------------------------------------------------------------------- */

static void ui_create(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), LV_PART_MAIN);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    /* Grundschrift für alles, was keine eigene setzt. Der eingebaute
     * Montserrat kann keine Umlaute — siehe fonts_ui.h.
     *
     * Beide Ebenen, nicht nur der Bildschirm: Dialoge liegen auf
     * `lv_layer_top()`, und die erbt nichts vom Bildschirm. Ohne die zweite
     * Zeile fielen dort alle Umlaute auf LV_FONT_DEFAULT zurück und wurden als
     * Rechteck gezeichnet — "Kapazität", "Schließen", "lädt". */
    lv_obj_set_style_text_font(scr, &font_ui_14, LV_PART_MAIN);
    lv_obj_set_style_text_font(lv_layer_top(), &font_ui_14, LV_PART_MAIN);

    /* Keine Laschen mehr: Es gibt nur diese eine Seite, und ein Tabview mit
     * einem einzigen Reiter wäre nur eine Leiste, die Platz kostet und nichts
     * tut. Home ist damit der Bildschirm. */
    lv_obj_set_style_pad_all(scr, 10, LV_PART_MAIN);
    page_home_create(scr);

    /* Beide Tanks: einmal jetzt, danach im Fünf-Minuten-Takt. */
    s_waste_cycle = lv_timer_create(tank_cycle_cb, WATER_CHECK_INTERVAL_MS, NULL);
    water_check_now();
    waste_check_now();

    lv_timer_create(led_refresh, 500, NULL);
    led_refresh(NULL);

    lv_timer_create(solar_refresh, 1000, NULL);
    solar_refresh(NULL);

    /* Funkverbindungen selbsttätig herstellen, solange das Display an ist. */
    lv_timer_create(auto_ble_tick, 2000, NULL);

    /* Display nach Untätigkeit abschalten. */
    lv_timer_create(display_idle_tick, 1000, NULL);

    /* Der Akku-Erkunder schaltet seine Abfragefolge im Tick weiter — dichter
     * getaktet als die übrige Automatik, weil zwischen zwei Abfragen nur
     * 250 ms liegen, sobald eine Antwort da ist. */
    lv_timer_create(probe_tick_cb, 250, NULL);
}

/* -------------------------------------------------------------------------- */

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    bsp_config_t cfg = BSP_CONFIG_DEFAULT();
    cfg.aw9523b.config.i2c_address = AW9523B_ADDR_00;
    ESP_ERROR_CHECK(bsp_init(&cfg));

    bsp_i2c_scan();

    if (bsp_aw9523b_get_handle()) {
        ESP_ERROR_CHECK(water_level_init());
        ESP_ERROR_CHECK(waste_level_init());
    } else {
        ESP_LOGE(TAG, "Ohne AW9523B keine Tankmessung und keine Ausgänge");
    }

    /* Beide BLE-Module tragen sich beim gemeinsamen Host ein, bevor ihn jemand
     * hochfährt — siehe ble_host.h zur Reihenfolge. */
    ESP_ERROR_CHECK(ble_scan_init());
    ESP_ERROR_CHECK(led_ble_init());
    ESP_ERROR_CHECK(ble_probe_init());
    ESP_ERROR_CHECK(victron_init());
    /* Kein ESP_ERROR_CHECK: Ohne Verlauf fehlen nur die Restlaufzeiten und die
     * Grafiken — das ist kein Grund, das Gerät nicht hochfahren zu lassen. */
    (void)batt_history_init();
    /* WLAN und Webseite. Auch hier kein ESP_ERROR_CHECK: Ohne Netz fehlt nur
     * die Fernanzeige, das Fahrzeug funktioniert vollständig weiter. */
    if (web_start() != ESP_OK) {
        ESP_LOGW(TAG, "WLAN ließ sich nicht starten — weiter ohne Webseite");
    }

    settings_load();

    if (bsp_display_lock(0)) {
        ui_create();
        /* Erst ganz am Ende, wenn die Oberfläche steht: Beide Bildspeicher
         * einmal vollständig füllen. */
        screen_repaint();
        bsp_display_unlock();
    }

    ESP_LOGI(TAG, "freier Heap: %u B intern, %u B PSRAM",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
