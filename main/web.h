/**
 * @file web.h
 * @brief WLAN-Anbindung und kleine Webseite mit den Betriebsdaten.
 *
 * ## Wozu
 *
 * Dieselben Zahlen, die auf dem Panel stehen, auch auf dem Telefon — ohne
 * aufzustehen, und von der Liege aus lesbar. Die Seite zeigt nur an, sie
 * schaltet nichts: Licht, Pumpe und Heizung bleiben dem Gerät vorbehalten, an
 * dem man steht. Ein offenes WLAN mit Schaltbefehlen wäre eine Einladung.
 *
 * ## Funk teilt sich die Antenne
 *
 * ESP32-S3 hat **einen** Sender für WLAN und Bluetooth. Beides gleichzeitig
 * geht nur über die Koexistenz-Schicht, die die Zeit zwischen den beiden
 * aufteilt. Das kostet: Der Akku wird etwas seltener abgefragt, der
 * Victron-Empfang verliert Pakete. Deshalb hält sich dieses Modul kurz —
 * verbinden, eine Seite ausliefern, sonst nichts.
 *
 * Verbindet sich das Netz nicht, läuft alles andere unverändert weiter. Ein
 * fehlendes WLAN ist kein Grund, ein Fahrzeug stehenzulassen.
 */

#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Voreinstellung für den ersten Start. Danach gilt, was im NVS steht und über
 * die Einstellungen geändert wurde.
 *
 * Kleingeschrieben, weil das Netz so heißt: SSIDs unterscheiden Groß- und
 * Kleinschreibung. Mit "Laterne" meldete der Treiber Grund 201 — "kein Netz
 * gefunden" —, und das sieht von außen genauso aus wie außer Reichweite oder
 * ein 5-GHz-Netz.
 */
#include "secrets_select.h" /* WEB_SSID_DEFAULT, WEB_PASSWORD_DEFAULT */
#define WEB_NAME_DEFAULT "Laterne"

/**
 * @brief WLAN starten und den Webserver vorbereiten.
 *
 * Kehrt sofort zurück; der Verbindungsaufbau läuft im Hintergrund. Der Server
 * startet erst, wenn eine Adresse zugeteilt wurde.
 */
esp_err_t web_start(void);

/** @brief true, sobald eine IP-Adresse vorliegt. */
bool web_is_connected(void);

/** @brief true, solange das Funkmodul überhaupt eingeschaltet ist. */
bool web_is_enabled(void);

/**
 * @brief WLAN ein- oder ausschalten; der Zustand überlebt den Neustart.
 *
 * Ausschalten gibt den Sender vollständig an Bluetooth zurück — wer die
 * Webseite nicht braucht, bekommt dafür einen zuverlässigeren Akku-Empfang.
 */
esp_err_t web_set_enabled(bool on);

/**
 * @brief Zugangsdaten setzen, speichern und neu verbinden.
 *
 * Ein leerer Name schaltet das WLAN wirkungsvoll ab — ohne Netz gibt es
 * nichts zu verbinden.
 */
esp_err_t web_set_credentials(const char *ssid, const char *password);

/** @brief Eingestellter Netzname. */
const char *web_ssid(void);

/** @brief Zugeteilte Adresse, oder "" solange keine vorliegt. */
const char *web_ip(void);

/**
 * @brief Empfangspegel in dBm.
 *
 * @return 0, wenn keine Verbindung steht. Werte sind negativ; −50 ist sehr
 *         gut, −80 ist die Grenze des Brauchbaren.
 */
int8_t web_rssi(void);

/** @brief Name des Geräts, wie er im Kopf der Webseite steht. */
const char *web_device_name(void);

/** @brief Gerätenamen setzen und speichern. */
esp_err_t web_set_device_name(const char *name);

/** Längen einschließlich Abschluss. */
#define WEB_SSID_LEN 33
#define WEB_PASS_LEN 65
#define WEB_NAME_LEN 25

/**
 * @brief Netze in der Umgebung suchen.
 *
 * Blockiert für die Dauer des Suchlaufs (rund zwei Sekunden) und hält dabei
 * den Sender besetzt. Nur auf ausdrücklichen Wunsch aufrufen, nicht im Takt.
 *
 * @param ssid  [out] Feld mit `max` Einträgen zu je WEB_SSID_LEN Zeichen.
 * @param rssi  [out] Feld mit `max` Pegeln.
 * @return Zahl der gefundenen Netze, stärkstes zuerst.
 */
size_t web_scan(char (*ssid)[WEB_SSID_LEN], int8_t *rssi, size_t max);

/**
 * @brief Zustand in einem Satz, für die Anzeige im Menü.
 *
 * Entweder die IP-Adresse oder der Grund, warum es noch keine gibt.
 */
const char *web_status_text(void);

#ifdef __cplusplus
}
#endif
