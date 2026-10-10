/**
 * @file led_ble.h
 * @brief Steuerung eines ELK-BLEDOM-RGB-Streifens über BLE.
 *
 * Das ist der billige chinesische BLE-Controller, der unter den Namen
 * `ELK-BLEDOM`, `MELK-…`, `LEDBLE` oder `XROCKER` verkauft wird. Anders als der
 * Scanner ist dieses Modul ein echter **GATT-Client**: es verbindet sich.
 *
 * ## Wichtig: nur einer gleichzeitig
 *
 * Solange das Board verbunden ist, kommt die Hersteller-App nicht an den
 * Streifen — und umgekehrt. Deshalb ist die Verbindung ausdrücklich zu öffnen
 * und zu schließen und wird nicht von allein wiederhergestellt.
 *
 * ## Protokoll
 *
 * Dienst `0xFFF0`, Schreib-Charakteristik `0xFFF3`, ohne Antwort beschrieben.
 * Jeder Befehl ist neun Byte lang, beginnt mit `0x7E` und endet mit `0xEF`:
 *
 * | Funktion        | Bytes                                  |
 * |-----------------|----------------------------------------|
 * | Ein             | `7E 00 04 F0 00 01 FF 00 EF`           |
 * | Aus             | `7E 00 04 00 00 00 FF 00 EF`           |
 * | Farbe           | `7E 00 05 03 RR GG BB 00 EF`           |
 * | Helligkeit      | `7E 00 01 HH 00 00 00 00 EF`  HH=0…100 |
 * | Tempo           | `7E 00 02 TT 00 00 00 00 EF`  TT=0…100 |
 * | Effekt          | `7E 00 03 ID 03 00 00 00 EF`  ID=0x80…0x9D |
 * | Weiß / Farbton  | `7E 00 05 02 WW KK 00 00 EF`  WW+KK=100 |
 *
 * Der Controller **meldet nichts zurück**. Was die Oberfläche anzeigt, ist ein
 * Schattenzustand aus dem, was zuletzt gesendet wurde — nach einem Stromausfall
 * am Streifen kann er von der Wirklichkeit abweichen.
 *
 * Quellen: die frei zugänglichen Mitschnitte der Community, insbesondere
 * arduino12/ble_rgb_led_strip_controller und dave-code-ruiz/elkbledom. Die
 * **Zuordnung der Effekt-IDs zu Namen ist nicht verlässlich dokumentiert** und
 * unterscheidet sich zwischen Baureihen — deshalb sind hier alle 30 IDs
 * durchnummeriert erreichbar, mit Namen nur dort, wo sie belegt sind.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Nach diesem Namensanfang wird beim Verbinden gesucht. */
#define LED_BLE_NAME_PREFIX "ELK-BLEDOM"
/** Suchdauer, bevor aufgegeben wird. */
#define LED_BLE_SEARCH_MS   10000

/** Erste und letzte Effekt-ID des Controllers. */
#define LED_BLE_EFFECT_FIRST 0x80
#define LED_BLE_EFFECT_LAST  0x9D
#define LED_BLE_EFFECT_COUNT (LED_BLE_EFFECT_LAST - LED_BLE_EFFECT_FIRST + 1)

typedef enum {
    LED_BLE_IDLE = 0,   /*!< nicht verbunden          */
    LED_BLE_SEARCHING,  /*!< sucht den Streifen       */
    LED_BLE_CONNECTING,
    LED_BLE_DISCOVERING,/*!< sucht Dienst und Merkmal */
    LED_BLE_READY,      /*!< verbunden, Befehle gehen durch */
    LED_BLE_FAILED,     /*!< letzter Versuch fehlgeschlagen */
} led_ble_state_t;

/** Was zuletzt gesendet wurde. Kein Rücklesewert — der Controller schweigt. */
typedef struct {
    bool power;
    uint8_t r, g, b;
    uint8_t brightness; /*!< 0…100 */
    uint8_t speed;      /*!< 0…100 */
    uint8_t effect;     /*!< ID, gültig wenn `effect_active` */
    bool effect_active; /*!< true nach einem Effekt, false nach fester Farbe */
    uint8_t warm;       /*!< 0…100, Rest ist Kaltweiß */
} led_ble_shadow_t;

/* --- API ----------------------------------------------------------------- */

/** @brief Vorbereiten. Fährt den BLE-Stack noch nicht hoch. */
esp_err_t led_ble_init(void);

/**
 * @brief Streifen suchen und verbinden.
 *
 * Sucht bis zu LED_BLE_SEARCH_MS nach einem Gerät, dessen Name mit
 * LED_BLE_NAME_PREFIX beginnt, verbindet sich und sucht Dienst und Merkmal.
 * Der Fortschritt steht in `led_ble_get_state()`.
 *
 * @note Ein laufender Scan (ble_scan.h) muss vorher beendet werden — der
 *       Controller kann nicht gleichzeitig suchen und verbinden.
 */
esp_err_t led_ble_connect(void);

/** @brief Verbindung trennen und den Streifen der App zurückgeben. */
esp_err_t led_ble_disconnect(void);

led_ble_state_t led_ble_get_state(void);
const char *led_ble_state_text(led_ble_state_t state);
bool led_ble_is_ready(void);

/** @brief Feldstärke der Verbindung, 0 wenn unbekannt. */
int8_t led_ble_rssi(void);

/* --- Befehle. Alle liefern ESP_ERR_INVALID_STATE, wenn nicht verbunden. --- */

esp_err_t led_ble_power(bool on);
esp_err_t led_ble_color(uint8_t r, uint8_t g, uint8_t b);
/** @param percent 0…100 */
esp_err_t led_ble_brightness(uint8_t percent);
/** @param percent 0…100 */
esp_err_t led_ble_speed(uint8_t percent);
/** @param id LED_BLE_EFFECT_FIRST … LED_BLE_EFFECT_LAST */
esp_err_t led_ble_effect(uint8_t id);
/** @param warm_percent 0…100; der Rest wird als Kaltweiß gesendet. */
esp_err_t led_ble_white(uint8_t warm_percent);

/** @brief Zuletzt gesendeter Zustand. */
const led_ble_shadow_t *led_ble_shadow(void);

/**
 * @brief Name eines Effekts für die Auswahlliste.
 *
 * @param index 0 … LED_BLE_EFFECT_COUNT-1
 */
const char *led_ble_effect_name(uint8_t index);

#ifdef __cplusplus
}
#endif
