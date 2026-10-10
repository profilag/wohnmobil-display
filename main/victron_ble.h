/**
 * @file victron_ble.h
 * @brief Victron „Instant Readout" mitlesen — Solarladeregler ohne Verbindung.
 *
 * Victron-Geräte funken ihre Messwerte verschlüsselt im Advertising. Das
 * Auslesen ist deshalb **rein passiv**: keine Verbindung, kein Pairing, kein
 * Verbindungslimit. Die VictronConnect-App läuft ungestört weiter — anders als
 * beim LED-Streifen oder beim Akku, wo immer nur einer darf.
 *
 * ## Aufbau der Manufacturer Data
 *
 * Am Board mitgeschnitten und gegen die Referenzimplementierung geprüft:
 *
 * ```
 * E1 02 │ 10 02 │ 57 A0 │ 01 │ B9 0C │ 49 │ 06 8E A2 …
 *   │       │       │      │     │      │     └ AES-CTR-Nutzlast, 12 Byte
 *   │       │       │      │     │      └ Prüfbyte = erstes Byte des Schlüssels
 *   │       │       │      │     └ Zähler (LE) = CTR-Nonce, +1 je Paket
 *   │       │       │      └ Satzart 0x01 = Solarladeregler
 *   │       │       └ Modell-ID (LE)
 *   │       └ Satztyp „Product Advertisement"
 *   └ Hersteller 0x02E1 = Victron
 * ```
 *
 * Entschlüsselt wird AES-128 im CTR-Modus. Der Zählerblock ist der 16-Bit-Wert
 * aus dem Paket, little-endian in die ersten zwei Byte geschrieben, Rest null.
 * Weil die Nutzlast mit zwölf Byte kürzer als ein AES-Block ist, wird der
 * Zähler nie hochgezählt — die Zählrichtung ist hier also gleichgültig.
 *
 * Dass der Zähler bei jedem Paket steigt, ist auch der Grund, warum dieselben
 * Messwerte jedes Mal völlig anders verschlüsselt aussehen.
 *
 * ## Nutzlast des Solarladeregler-Satzes (12 Byte)
 *
 * | Versatz | Feld              | Typ       | Skalierung | „unbekannt" |
 * |---------|-------------------|-----------|------------|-------------|
 * | 0       | Ladezustand       | uint8     | Enum       | 0xFF        |
 * | 1       | Ladefehler        | uint8     | Enum       | 0xFF        |
 * | 2–3     | Batteriespannung  | int16 LE  | ÷100 → V   | 0x7FFF      |
 * | 4–5     | Batteriestrom     | int16 LE  | ÷10 → A    | 0x7FFF      |
 * | 6–7     | Ertrag heute      | uint16 LE | ×10 → Wh   | 0xFFFF      |
 * | 8–9     | PV-Leistung       | uint16 LE | W          | 0xFFFF      |
 * | 10–11   | Lastausgang       | 9 Bit LE  | ÷10 → A    | 0x1FF       |
 *
 * ## Der Schlüssel
 *
 * Gerätespezifisch, zu finden in VictronConnect unter Zahnrad → Produktinfo →
 * „Instant Readout via Bluetooth" → Anzeigen. Sein erstes Byte muss mit dem
 * Prüfbyte im Paket übereinstimmen, sonst gehört er zu einem anderen Gerät.
 *
 * Er gibt nur Lesezugriff auf Telemetrie, ist also kein Generalschlüssel zum
 * Regler — trotzdem gehört er behandelt wie ein Zugangsdatum. Beim Zurücksetzen
 * des Reglers ändert er sich.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Hersteller-ID von Victron Energy in den Manufacturer Data. */
#define VICTRON_COMPANY_ID 0x02E1
/** Satzart des Solarladereglers. */
#define VICTRON_RECORD_SOLAR_CHARGER 0x01

/*
 * VICTRON_KEY_HEX (32 Hexzeichen) steht in secrets.h. Er muss mit dem
 * Prüfbyte des jeweiligen Pakets beginnen.
 */
#include "secrets_select.h"

typedef struct {
    bool valid;         /*!< true, sobald ein Paket entschlüsselt wurde */
    uint16_t model_id;

    uint8_t device_state;
    uint8_t charger_error;

    /*
     * Die Werte bleiben ganzzahlig und in der Skalierung des Protokolls.
     * Zwei Gründe: das Umrechnen in float und zurück gewinnt nichts, und
     * LVGLs eingebautes `printf` beherrscht **kein `%f`** — eine Kachel mit
     * `"%.2f V"` zeigt dort nur ein einsames `f`.
     *
     * Jedes Feld kann als „unbekannt" gemeldet werden; dann ist das zugehörige
     * has_* false und der Wert bedeutungslos.
     */
    bool has_voltage;
    int16_t voltage_cv; /*!< Hundertstel Volt: 1350 = 13,50 V */
    bool has_current;
    int16_t current_da; /*!< Zehntel Ampere, negativ = Entladung */
    bool has_yield;
    /** Vielfache von 10 Wh — das sind zugleich Hundertstel kWh. */
    uint16_t yield_10wh;
    bool has_power;
    uint16_t solar_power; /*!< W */
    bool has_load;
    uint16_t load_da; /*!< Zehntel Ampere */

    int8_t rssi;
    uint32_t packets;    /*!< erfolgreich entschlüsselt */
    uint32_t key_errors; /*!< Prüfbyte passte nicht zum Schlüssel */
} victron_data_t;

/* --- API ----------------------------------------------------------------- */

/**
 * @brief Schlüssel einlesen und beim Scanner als Empfänger eintragen.
 *
 * Gelesen wird nur, solange der BLE-Scan läuft — dieses Modul startet ihn
 * nicht selbst.
 */
esp_err_t victron_init(void);

/** @brief Kopie des letzten Standes. */
void victron_get(victron_data_t *out);

/** @brief Alter des letzten Pakets in ms, -1 wenn noch keins kam. */
int64_t victron_age_ms(void);

/** @brief Ladezustand als Text, z. B. "Konstantstrom". */
const char *victron_state_text(uint8_t state);

/** @brief Ladefehler als Text, "kein Fehler" bei 0. */
const char *victron_error_text(uint8_t error);

#ifdef __cplusplus
}
#endif
