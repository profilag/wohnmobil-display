/**
 * @file batt_history.h
 * @brief Kurzzeitgedächtnis für Strom und Restkapazität des Akkus.
 *
 * ## Wozu
 *
 * Das BMS liefert immer nur den Augenblickswert. Für die Frage „wie lange
 * reicht das noch?" braucht man aber einen Mittelwert über einen längeren
 * Zeitraum — der Kühlschrank taktet, die Pumpe läuft kurz, die Sonne kommt und
 * geht. Dieses Modul sammelt die Werte und beantwortet zwei Fragen:
 *
 *   - Wie hoch war die mittlere Entnahme über ein Fenster?
 *   - Wie sah der Verlauf aus? (für die Grafiken im Detaildialog)
 *
 * ## Nur im RAM
 *
 * Bewusst nichts im NVS: Die Daten wären beim nächsten Start ohnehin
 * lückenhaft, und jeder Schreibzugriff hält den Flash-Cache an — was am
 * RGB-Panel als Bildstörung sichtbar wird. Nach einem Neustart beginnt die
 * Sammlung von vorn; `batt_history_mean()` sagt über `covered_ms`, wie weit sie
 * schon zurückreicht, damit die Anzeige nicht mehr behauptet als sie weiß.
 *
 * ## Aufbau
 *
 * Ein Ringpuffer aus Ein-Minuten-Fächern. Jedes Fach hält den Mittelwert des
 * Stroms und die zuletzt gesehene Restkapazität. Zehn Tage sind 14400 Fächer
 * zu 6 Byte, also gut 84 kB — dafür der **PSRAM**: Im internen RAM wäre das
 * ein spürbarer Batzen, im PSRAM ist es nichts, und angefasst wird der Puffer
 * nur zweimal je Minute und beim Zeichnen.
 *
 * Die Minute ist nicht wegen der Genauigkeit so fein gewählt, sondern wegen
 * der Wartezeit: Eine Linie braucht zwei Stützstellen. Mit Fünf-Minuten-Fächern
 * bliebe die Grafik nach dem Einschalten zehn Minuten lang leer.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Dauer eines Fachs. */
#define BATT_HIST_SLOT_MS (60 * 1000)
/** Wie weit der Speicher zurückreicht. */
#define BATT_HIST_DAYS    10
/** Anzahl der Fächer im Ring. */
#define BATT_HIST_SLOTS   (BATT_HIST_DAYS * 24 * (60 * 60 * 1000 / BATT_HIST_SLOT_MS))

/** Platzhalter für „keine Daten" in den Reihen von batt_history_series(). */
#define BATT_HIST_NONE    INT32_MAX

/**
 * @brief Ringpuffer anlegen. Einmal beim Start aufrufen.
 *
 * Schlägt die Zuteilung fehl, bleibt das Modul stumm: `batt_history_add()` tut
 * nichts, die Abfragen melden „keine Daten". Ein fehlender Verlauf ist
 * ärgerlich, aber kein Grund, den Rest der Anlage stehenzulassen.
 */
esp_err_t batt_history_init(void);

/**
 * @brief Einen frisch gelesenen Datensatz einsortieren.
 *
 * Darf so oft aufgerufen werden, wie Werte eintreffen; das Fach mittelt.
 *
 * @param current_ma    Packstrom in mA, negativ beim Entladen.
 * @param remaining_mah Restkapazität in mAh.
 */
void batt_history_add(int32_t current_ma, uint32_t remaining_mah);

/**
 * @brief Mittlere **Entnahme** über das jüngste Fenster.
 *
 * Gemittelt wird nur, was aus dem Akku herausgeflossen ist; Zeiten, in denen
 * geladen wurde, gehen mit null ein. Das ist bewusst nicht die Bilanz:
 *
 * Die Bilanz beantwortet die Frage „wächst oder schrumpft der Ladestand" und
 * steht an einem sonnigen Tag bei null — dann gibt es keine Restlaufzeit,
 * sondern nur ein Achselzucken. Die Entnahme beantwortet die Frage, auf die es
 * ankommt: „Wie lange reicht der Akku bei diesem Verbrauch, wenn die Sonne
 * ausbleibt." Darauf gibt es immer eine Zahl.
 *
 * Gewichtet nach der Zahl der Messwerte je Fach, nicht nach Fächern — ein Fach
 * mit einem einzigen Wert zählt also weniger als ein volles.
 *
 * @param window_ms   Länge des Fensters, z. B. 86400000 für einen Tag.
 * @param drain_ma    [out] mittlere Entnahme in mA, positiv oder null.
 * @param covered_ms  [out, optional] tatsächlich belegte Zeit im Fenster.
 *                    Kleiner als `window_ms`, solange der Speicher noch füllt.
 * @return false, wenn im Fenster überhaupt keine Daten liegen.
 */
bool batt_history_drain(uint32_t window_ms, uint32_t *drain_ma, uint32_t *covered_ms);

/**
 * @brief Wie weit der Speicher tatsächlich zurückreicht.
 *
 * Für die Grafiken: Ein Fenster von fünf Tagen über zwanzig Minuten Daten
 * ergibt eine leere Fläche mit einem Punkt am Rand. Wer zeichnet, fragt hier
 * nach und nimmt das kleinere von beidem.
 *
 * @return Zeitspanne in Millisekunden, 0 solange nichts gesammelt wurde.
 */
uint32_t batt_history_span_ms(void);

/**
 * @brief Verlauf auf eine feste Zahl von Stützstellen herunterrechnen.
 *
 * Für die Grafiken: `points` gleich lange Abschnitte über `window_ms`, ältester
 * zuerst. Abschnitte ohne Daten bekommen BATT_HIST_NONE — LVGL zeichnet dort
 * eine Lücke statt einer erfundenen Linie.
 *
 * @param current_ma [out, optional] Feld mit `points` Einträgen, Strom in mA.
 * @param cap_mah    [out, optional] Feld mit `points` Einträgen, Rest in mAh.
 * @return Anzahl der Stützstellen mit Daten.
 */
size_t batt_history_series(uint32_t window_ms, size_t points, int32_t *current_ma,
                           int32_t *cap_mah);

/**
 * @brief Restlaufzeit in Stunden aus der mittleren Entnahme.
 *
 * Die eine Stelle, an der diese Division steht — Anzeige und Webseite holen
 * sie beide hier ab, damit sie nie auseinanderlaufen können.
 *
 * @param window_ms     Fenster der Mittelung.
 * @param remaining_mah Restkapazität laut BMS.
 * @param minutes       [out] Minuten, bis der Akku leer wäre.
 * @param covered_ms    [out, optional] tatsächlich belegte Zeit im Fenster.
 * @return false, wenn keine Daten vorliegen oder nichts entnommen wurde —
 *         dann gibt es keine Restlaufzeit zu nennen.
 */
bool batt_history_runtime(uint32_t window_ms, uint32_t remaining_mah,
                          uint32_t *minutes, uint32_t *covered_ms);

#ifdef __cplusplus
}
#endif
