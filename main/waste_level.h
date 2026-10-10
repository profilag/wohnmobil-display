/**
 * @file waste_level.h
 * @brief Überwachung des Abwassertanks — eine einzige Elektrode für "voll".
 *
 * Der Abwassertank hat nur einen Kontakt. Erreicht das Wasser ihn, ist der Tank
 * voll; Zwischenstände lassen sich damit nicht unterscheiden. Entsprechend gibt
 * es hier kein Füllstands-Enum wie beim Frischwasser, sondern nur ja/nein.
 *
 * ## Zyklische Prüfung statt Knopfdruck
 *
 * Anders als der Frischwassertank wird der Abwassertank **von allein** geprüft,
 * alle WASTE_CHECK_INTERVAL_MS. Das ist die eigentliche Nutzung: man will
 * gewarnt werden, bevor der Tank überläuft, ohne daran zu denken. Zusätzlich
 * lässt sich jederzeit von Hand prüfen.
 *
 * Dauerhaft bestromt wird trotzdem nicht — auch hier zersetzt Gleichstrom die
 * Elektrode. Pro Prüfung liegt die Anregung rund 70 ms an; bei 5 Minuten Takt
 * sind das etwa 0,02 % Einschaltdauer.
 *
 * ## Eigener Anregungsausgang
 *
 * Die Anregung liegt auf **PB5**, nicht auf der PB0 des Frischwassers. Sonst
 * würde die zyklische Abwasserprüfung alle fünf Minuten auch die
 * Frischwasserelektroden bestromen — genau das, was water_level.h vermeidet.
 *
 * ## Verdrahtung
 *
 * | Funktion                   | AW9523B | Richtung           |
 * |----------------------------|---------|--------------------|
 * | Anregung / Bezugselektrode | PB5     | Ausgang, low-aktiv |
 * | Elektrode "voll"           | PB6     | Eingang            |
 *
 * PB6 braucht wie die Frischwassereingänge einen **externen Pull-up** (typisch
 * 10 kΩ nach 3V3) — der AW9523B hat keine internen. Ohne Wasserkontakt hält der
 * Pull-up den Eingang auf High = nicht voll.
 *
 * Misst die Elektrode statt gegen eine Bezugselektrode direkt gegen Tankwand
 * oder Fahrzeugmasse, bleibt PB5 unbeschaltet. Die Messung funktioniert dann
 * unverändert, nur ohne die schonende Taktung.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Verdrahtung ---------------------------------------------------------- */

#define WASTE_EXCITE_PB_BIT         5  /*!< PB5 — Anregung Bezugselektrode */
#define WASTE_PB_BIT_FULL           6  /*!< PB6 — Elektrode "voll"         */

/** Pegel, den der Eingang bei Wasserkontakt annimmt. 0 = low. */
#define WASTE_ELECTRODE_WET_LEVEL   0

/* --- Zeiten -------------------------------------------------------------- */

/** Abstand zweier selbsttätiger Prüfungen. */
#define WASTE_CHECK_INTERVAL_MS     (5 * 60 * 1000)
/** Beruhigungszeit nach dem Einschalten der Anregung, bevor gelesen wird. */
#define WASTE_SETTLE_MS             40
/** Anzahl Einzelmessungen; das Ergebnis entscheidet die Mehrheit. */
#define WASTE_SAMPLE_COUNT          3
/** Abstand zwischen den Einzelmessungen. */
#define WASTE_SAMPLE_GAP_MS         8
/** Zwangsabschaltung des Testmodus, damit er nicht versehentlich anbleibt. */
#define WASTE_TESTMODE_TIMEOUT_MS   30000

/* --- Ergebnis ------------------------------------------------------------ */

typedef struct {
    /** true = Wasser steht an der Elektrode, Tank ist voll. */
    bool full;
    /** Vollständiges Port-B-Byte, wie gelesen — für die Pegelanzeige. */
    uint8_t raw_pb;
} waste_measurement_t;

/* --- API ----------------------------------------------------------------- */

/**
 * @brief Anregungsausgang und Eingang konfigurieren. Anregung bleibt aus.
 *
 * Setzt voraus, dass `bsp_init()` durchgelaufen ist und der AW9523B erkannt
 * wurde.
 */
esp_err_t waste_level_init(void);

/**
 * @brief Eine vollständige Prüfung ausführen.
 *
 * Schaltet die Anregung ein, wartet WASTE_SETTLE_MS, nimmt WASTE_SAMPLE_COUNT
 * Messungen und schaltet die Anregung in jedem Fall wieder aus — auch wenn ein
 * I2C-Zugriff fehlschlägt.
 *
 * @note Blockiert für rund 70 ms.
 */
esp_err_t waste_level_measure(waste_measurement_t *out);

/** @brief Letztes Prüfergebnis (auch ohne neue Prüfung). */
const waste_measurement_t *waste_level_last(void);

/**
 * @brief Alter des letzten erfolgreichen Ergebnisses in Millisekunden.
 *
 * @return -1, solange noch nie erfolgreich geprüft wurde.
 */
int64_t waste_level_age_ms(void);

/**
 * @brief Anregung dauerhaft schalten — nur für den Pegeltest.
 *
 * @warning Bestromt die Elektrode dauerhaft und fördert damit Elektrolyse.
 *          Nur kurz und bewusst benutzen.
 */
esp_err_t waste_level_excite(bool on);

/** @brief true, wenn die Anregung gerade dauerhaft an ist. */
bool waste_level_is_excited(void);

/** @brief Eingang lesen, ohne die Anregung zu verändern (für den Pegeltest). */
esp_err_t waste_level_read_raw(waste_measurement_t *out);

#ifdef __cplusplus
}
#endif
