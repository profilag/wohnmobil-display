/**
 * @file secrets.example.h
 * @brief Vorlage für secrets.h mit Platzhaltern.
 *
 * Nach main/secrets.h kopieren und die echten Werte eintragen:
 *
 *     cp main/secrets.example.h main/secrets.h
 *
 * secrets.h steht in .gitignore. Fehlt sie, baut das Projekt mit diesen
 * Platzhaltern und warnt beim Übersetzen.
 */
#pragma once

/**
 * Victron-Schlüssel, 32 Hexzeichen (VictronConnect → Produktinfo →
 * Instant Readout via Bluetooth → Show). Er beginnt mit dem Prüfbyte der
 * Pakete des jeweiligen Geräts.
 */
#define VICTRON_KEY_HEX "00000000000000000000000000000000"

/** WLAN für den ersten Start, danach gilt der NVS-Inhalt. */
#define WEB_SSID_DEFAULT "mein-netz"
#define WEB_PASSWORD_DEFAULT "mein-passwort"

/** BLE-Adresse des Akkus, Anzeigereihenfolge wie in der App. */
#define BLE_PROBE_DEFAULT_MAC "AA:BB:CC:DD:EE:FF"
