/**
 * @file ble_scan.h
 * @brief BLE-Beobachter: zeigt, was am Einbauort in Funkreichweite sendet.
 *
 * Zweck ist die Vorarbeit für zwei mögliche Datenquellen im Wohnmobil:
 *
 *   - **Victron-Solarregler.** Victron-Geräte funken ihre Messwerte
 *     verschlüsselt im Advertising-Paket ("Instant Readout"), erkennbar an der
 *     Hersteller-ID 0x02E1. Auslesen ginge später rein passiv, ohne Verbindung
 *     und ohne der VictronConnect-App im Weg zu stehen.
 *   - **Akku mit Bluetooth-BMS.** Welches Protokoll ein Akku spricht, verrät
 *     meist schon sein Advertising-Name: `xiaoxiang BMS`/`JBD-…` für JBD,
 *     `JK_…` für JK, `DL-…` für Daly. Diese Erkennung ist eine **Vermutung**
 *     anhand des Namens, keine Protokollprüfung.
 *
 * ## Was dieses Modul tut — und was nicht
 *
 * Es **hört zu und verbindet sich mit nichts**. Es baut keine GATT-Verbindung
 * auf, liest keine Charakteristiken und paart sich nicht. Damit stört es auch
 * keine Handy-App, die gerade mit demselben Gerät spricht — anders als ein
 * BMS-Client, denn die meisten BMS lassen nur einen Client gleichzeitig zu.
 *
 * Gescannt wird allerdings **aktiv**: das Board sendet Scan-Requests, um die
 * Scan-Response zu bekommen. Ohne die bleiben viele Geräte namenlos, und
 * gerade der Name ist hier die gesuchte Information. Ein Scan-Request ist ein
 * kurzes Paket, auf das das Gegenüber ohnehin antwortet; eine Verbindung wird
 * dadurch nicht aufgebaut.
 *
 * ## Funk neben dem RGB-Panel
 *
 * Das Panel schaufelt dauernd per DMA aus dem PSRAM, und alles, was den Cache
 * anhält, ist sichtbar. Deshalb: NimBLE wird **erst beim ersten Scan**
 * hochgefahren (der Bootvorgang bleibt unverändert), Bonding ist abgeschaltet
 * (keine NVS-Schreibzugriffe) und das Scan-Tastverhältnis liegt bei rund 30 %
 * statt Dauerempfang. Zeigt das Bild trotzdem Störungen, sind
 * `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` und `CONFIG_SPIRAM_RODATA` die nächsten
 * Stellschrauben — sie verlagern Code und Konstanten ins PSRAM, sodass
 * Flash-Zugriffe den Bildaufbau nicht mehr anhalten.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** So viele verschiedene Geräte werden gemerkt; danach gewinnt der stärkere. */
#define BLE_SCAN_MAX_DEVICES 24
/** Advertising-Namen sind auf 31 Byte begrenzt. */
#define BLE_SCAN_NAME_MAX    32
/** So viele beworbene 16-Bit-Dienste werden gemerkt. */
#define BLE_SCAN_SVC16_MAX   4

/** Hersteller-ID von Victron Energy in den Manufacturer Data. */
#define BLE_SCAN_COMPANY_VICTRON 0x02E1

/** Vermutete Geräteart. Siehe Dateikopf: eine Heuristik, kein Beweis. */
typedef enum {
    BLE_SCAN_KIND_UNKNOWN = 0,
    BLE_SCAN_KIND_VICTRON,  /*!< sicher — über die Hersteller-ID erkannt */
    BLE_SCAN_KIND_JBD,      /*!< vermutet — Name spricht für JBD/Xiaoxiang */
    BLE_SCAN_KIND_JK,       /*!< vermutet — Name spricht für JK-BMS       */
    BLE_SCAN_KIND_DALY,     /*!< vermutet — Name spricht für Daly         */
} ble_scan_kind_t;

typedef struct {
    /** Adresse, wie NimBLE sie liefert: `addr[0]` ist das niederwertigste Byte. */
    uint8_t addr[6];
    uint8_t addr_type;
    /** Leerer String, solange kein Name empfangen wurde. */
    char name[BLE_SCAN_NAME_MAX];
    int8_t rssi;      /*!< zuletzt empfangen */
    int8_t rssi_best; /*!< bester Wert seit dem Löschen — Maß für den Einbauort */
    uint16_t company; /*!< Hersteller-ID, nur gültig wenn `has_company` */
    bool has_company;
    /**
     * Die Manufacturer Data im Rohzustand, samt Hersteller-ID am Anfang.
     *
     * Der Grund, warum das hier steht: manche Geräte tragen ihre Messwerte im
     * Advertising spazieren, statt sie erst auf Anfrage herauszurücken. Ob das
     * so ist, sieht man nur daran, ob sich die Bytes im Betrieb **ändern** —
     * deshalb protokolliert das Modul jede Änderung.
     */
    uint8_t mfg[31];
    uint8_t mfg_len;
    /**
     * Beworbene 16-Bit-Dienste. Bei einem BMS steht hier meist genau der
     * Dienst, über den später die Daten laufen — 0xFF00 spricht für JBD,
     * 0xFFE0 für einen HM-10-artigen UART-Dienst und so weiter.
     */
    uint16_t svc16[BLE_SCAN_SVC16_MAX];
    uint8_t n_svc16;
    /** Erster beworbener 128-Bit-Dienst, für herstellereigene UUIDs. */
    uint8_t svc128[16];
    bool has_svc128;
    uint32_t packets;
    ble_scan_kind_t kind;
} ble_scan_dev_t;

/* --- API ----------------------------------------------------------------- */

/**
 * @brief Modul vorbereiten. Fährt den BLE-Stack noch **nicht** hoch.
 *
 * Legt nur die Geräteliste und ihren Mutex an. NimBLE startet erst beim ersten
 * `ble_scan_start()`, damit ein Board, das nie scannt, exakt so läuft wie ohne
 * dieses Modul.
 */
esp_err_t ble_scan_init(void);

/**
 * @brief Scan starten. Beim allerersten Aufruf kommt zuerst NimBLE hoch.
 *
 * @note Der erste Aufruf blockiert kurz (Stack-Init) und der Scan beginnt erst,
 *       wenn der Host synchronisiert ist — `ble_scan_is_running()` meldet also
 *       einen Moment lang noch false.
 */
esp_err_t ble_scan_start(void);

/** @brief Scan anhalten. Der Stack bleibt oben, ein Neustart geht sofort. */
esp_err_t ble_scan_stop(void);

/** @brief true, sobald der Controller tatsächlich empfängt. */
bool ble_scan_is_running(void);

/** @brief Geräteliste leeren, insbesondere die gemerkten Bestwerte. */
void ble_scan_clear(void);

/**
 * @brief Rückruf für **jedes** Advertising-Paket mit Manufacturer Data.
 *
 * Gedacht für Auswerter, die den Inhalt selbst deuten — victron_ble.h etwa.
 * Anders als die Geräteliste bekommt der Rückruf jedes Paket einzeln, denn bei
 * Victron trägt jedes einen neuen Zählerstand und damit neue Nutzdaten.
 *
 * @warning Läuft im NimBLE-Host-Task. Von hier aus keine `lv_*`-Funktionen,
 *          und nichts, was lange dauert.
 */
typedef void (*ble_scan_mfg_cb_t)(const uint8_t *mfg, uint8_t len, int8_t rssi);

esp_err_t ble_scan_set_mfg_cb(ble_scan_mfg_cb_t cb);

/**
 * @brief Zähler, der bei jeder Änderung der Liste hochgeht.
 *
 * Damit lässt sich eine Anzeige auffrischen, ohne sie im Sekundentakt
 * grundlos neu aufzubauen.
 */
uint32_t ble_scan_generation(void);

/**
 * @brief Kopie der Liste ziehen, nach Feldstärke absteigend sortiert.
 *
 * @return Anzahl der geschriebenen Einträge, höchstens `max`.
 */
size_t ble_scan_snapshot(ble_scan_dev_t *out, size_t max);

/** @brief Geräteart als Text, Vermutungen mit Fragezeichen. */
const char *ble_scan_kind_text(ble_scan_kind_t kind);

/** @brief Adresse als "AA:BB:CC:DD:EE:FF". `buf` braucht 18 Byte. */
void ble_scan_addr_text(const ble_scan_dev_t *dev, char *buf, size_t len);

/**
 * @brief Beworbene Dienste als Text, z. B. "FF00 FFE0" oder "6E400001…".
 *
 * 128-Bit-UUIDs werden auf die ersten vier Byte gekürzt — das ist der Teil, an
 * dem sich herstellereigene Dienste erkennen lassen. Leerer String, wenn das
 * Gerät gar keine Dienste bewirbt.
 */
void ble_scan_services_text(const ble_scan_dev_t *dev, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
