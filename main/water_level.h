/**
 * @file water_level.h
 * @brief Wasserstandsmessung mit drei Elektroden im Wohnmobil-Frischwassertank.
 *
 * Messprinzip: drei Elektroden reichen unterschiedlich tief in den Tank. Eine
 * vierte Elektrode (Bezug) wird über einen Ausgang bestromt. Steht das Wasser
 * über einer Elektrode, leitet es zur Bezugselektrode und der zugehörige
 * Eingang wechselt seinen Pegel.
 *
 * ## Elektrolyse
 *
 * Dauerhaft bestromte Elektroden zersetzen sich und das Wasser — daher wird die
 * Anregung **nur für die Dauer einer Messung** eingeschaltet, rund 70 ms.
 * Zwischen zwei Messungen liegt der Messkreis vollständig spannungsfrei.
 *
 * Gemessen wird beim Start einmal und danach alle WATER_CHECK_INTERVAL_MS,
 * zusätzlich jederzeit auf Knopfdruck. Das ist eine bewusste Abwägung: Früher
 * lief die Messung ausschließlich auf Knopfdruck, damit im Trinkwasser so
 * wenig Strom wie irgend möglich fließt. Der Preis war ein Tank, dessen Stand
 * man erst kennt, wenn man danach fragt — bei einer Anzeige, die im Vorbeigehen
 * gelesen wird, der falsche Kompromiss.
 *
 * Die Einschaltdauer bleibt dabei winzig: 70 ms alle fünf Minuten sind rund
 * **0,02 %**, über einen ganzen Tag etwa zwanzig Sekunden Bestromung.
 *
 * Der Testmodus (`water_level_excite`) durchbricht das absichtlich und ist
 * deshalb zeitlich begrenzt.
 *
 * ## Verdrahtung
 *
 * | Funktion                      | AW9523B | Richtung             |
 * |-------------------------------|---------|----------------------|
 * | Anregung / Bezugselektrode    | PB0     | Ausgang, low-aktiv   |
 * | Elektrode unten (tiefste)     | PB1     | Eingang              |
 * | Elektrode mitte               | PB2     | Eingang              |
 * | Elektrode oben (kürzeste)     | PB3     | Eingang              |
 *
 * Anregung und Sensoren liegen auf demselben Port — ein Kabelbaum statt
 * zweier. Port 1 des AW9523B ist immer Push-Pull; im Ruhezustand liegt die
 * Bezugselektrode damit auf demselben Pegel wie die Pull-ups der Sensoren, es
 * fließt kein Strom und es gibt keine Elektrolyse.
 *
 * Der AW9523B hat keine internen Pull-ups. Jede Elektrodenleitung braucht
 * deshalb einen externen Pull-up (typisch 10 kΩ nach 3V3): ohne Wasserkontakt
 * zieht er den Eingang auf High, mit Wasserkontakt zieht die bestromte
 * Bezugselektrode ihn auf Low. Genau diese Polarität steht in
 * WATER_ELECTRODE_WET_LEVEL — bei umgekehrter Beschaltung dort auf 1 ändern.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Verdrahtung ---------------------------------------------------------- */

#define WATER_EXCITE_PB_BIT         0  /*!< PB0 — Anregung Bezugselektrode */

#define WATER_ELECTRODE_COUNT       3
#define WATER_PB_BIT_LOW            1  /*!< PB1 — tiefste Elektrode  */
#define WATER_PB_BIT_MID            2  /*!< PB2 — mittlere Elektrode */
#define WATER_PB_BIT_HIGH           3  /*!< PB3 — oberste Elektrode  */

/** Pegel, den ein Eingang bei Wasserkontakt annimmt. 0 = low. */
#define WATER_ELECTRODE_WET_LEVEL   0

/* --- Zeiten -------------------------------------------------------------- */

/** Beruhigungszeit nach dem Einschalten der Anregung, bevor gelesen wird. */
#define WATER_SETTLE_MS             40
/** Anzahl Einzelmessungen; das Ergebnis entscheidet die Mehrheit. */
#define WATER_SAMPLE_COUNT          3
/** Abstand zwischen den Einzelmessungen. */
#define WATER_SAMPLE_GAP_MS         8
/** Zwangsabschaltung des Testmodus, damit er nicht versehentlich anbleibt. */
#define WATER_TESTMODE_TIMEOUT_MS   30000
/** Abstand zweier selbsttätiger Messungen. Gleicher Takt wie beim Abwasser. */
#define WATER_CHECK_INTERVAL_MS     (5 * 60 * 1000)

/* --- Ergebnis ------------------------------------------------------------ */

/** Die vier unterscheidbaren Füllstände bei drei Elektroden. */
typedef enum {
    WATER_LEVEL_EMPTY = 0, /*!< keine Elektrode nass — Reserve/leer */
    WATER_LEVEL_THIRD,     /*!< nur untere Elektrode nass          */
    WATER_LEVEL_TWO_THIRD, /*!< untere und mittlere nass           */
    WATER_LEVEL_FULL,      /*!< alle drei nass                     */
} water_level_t;

typedef struct {
    water_level_t level;
    /** Rohzustand der Elektroden, Index 0 = unten. */
    bool wet[WATER_ELECTRODE_COUNT];
    /** Vollständiges Port-B-Byte, wie gelesen — für die Pegelanzeige. */
    uint8_t raw_pb;
    /**
     * true, wenn das Muster nicht von unten aufsteigend ist (z. B. obere
     * Elektrode nass, untere trocken). Das ist physikalisch unmöglich und
     * deutet auf Leitungsbruch, Korrosion oder eine vertauschte Leitung hin.
     * `level` enthält dann den Stand aus dem lückenlosen unteren Teil.
     */
    bool implausible;
} water_measurement_t;

/* --- API ----------------------------------------------------------------- */

/**
 * @brief Ausgang und Eingänge konfigurieren. Anregung bleibt aus.
 *
 * Setzt voraus, dass `bsp_init()` durchgelaufen ist und der AW9523B erkannt wurde.
 */
esp_err_t water_level_init(void);

/**
 * @brief Eine vollständige Messung ausführen.
 *
 * Schaltet die Anregung ein, wartet WATER_SETTLE_MS, nimmt WATER_SAMPLE_COUNT
 * Messungen und schaltet die Anregung in jedem Fall wieder aus — auch wenn ein
 * I2C-Zugriff fehlschlägt.
 *
 * @note Blockiert für rund WATER_SETTLE_MS + WATER_SAMPLE_COUNT *
 *       WATER_SAMPLE_GAP_MS Millisekunden. Nicht aus einem LVGL-Callback mit
 *       gehaltenem Mutex aufrufen, wenn die Oberfläche flüssig bleiben soll —
 *       bei ~70 ms ist das aber unkritisch.
 */
esp_err_t water_level_measure(water_measurement_t *out);

/** @brief Letztes Messergebnis (auch ohne neue Messung). */
const water_measurement_t *water_level_last(void);

/**
 * @brief Anregung dauerhaft schalten — nur für den Pegeltest.
 *
 * @warning Bestromt die Elektroden dauerhaft und fördert damit Elektrolyse.
 *          Nur kurz und bewusst benutzen.
 */
esp_err_t water_level_excite(bool on);

/** @brief true, wenn die Anregung gerade dauerhaft an ist. */
bool water_level_is_excited(void);

/** @brief Eingänge lesen, ohne die Anregung zu verändern (für den Pegeltest). */
esp_err_t water_level_read_raw(water_measurement_t *out);

/** @brief Füllstand als Text, z. B. "2/3 voll". */
const char *water_level_text(water_level_t level);

/** @brief Füllstand in Prozent für Balken/Anzeigen (0, 33, 66, 100). */
int water_level_percent(water_level_t level);

#ifdef __cplusplus
}
#endif
