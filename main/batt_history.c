#include "batt_history.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

/*
 * Alle Funktionen laufen aus der LVGL-Aufgabe heraus: `batt_history_add()` aus
 * demselben Zeitgeber, der auch die Anzeige nachführt, die Auswertungen beim
 * Zeichnen. Deshalb ohne Sperre — ein Mutex würde hier nur Laufzeit kosten und
 * eine Nebenläufigkeit vortäuschen, die es nicht gibt.
 */

typedef struct {
    int16_t mean_10ma;   /*!< Mittelwert des Fachs in 10-mA-Schritten  */
    uint16_t cap_100mah; /*!< Restkapazität in 0,1-Ah-Schritten        */
    uint16_t samples;    /*!< Zahl der Messwerte; 0 kennzeichnet Lücke */
} slot_t;

static const char *TAG = "batthist";

/* Im PSRAM: 14400 Fächer sind gut 84 kB, und der interne RAM ist auf diesem
 * Board mit Framebuffer-Bounce-Puffern und LVGL schon gut ausgelastet. */
static slot_t *s_slots;

/** Fortlaufender Index des offenen Fachs — nicht der Ringindex. */
static uint32_t s_open_index;
static bool s_started;

/* Laufende Summe des offenen Fachs, damit sein Mittelwert schon vor dem
 * Fachwechsel stimmt. Sonst stünde die Anzeige nach dem Start fünf Minuten
 * lang ohne Wert da. */
static int64_t s_open_sum_ma;
static uint32_t s_open_n;

esp_err_t batt_history_init(void)
{
    if (s_slots) {
        return ESP_OK;
    }
    s_slots = heap_caps_calloc(BATT_HIST_SLOTS, sizeof(slot_t), MALLOC_CAP_SPIRAM);
    if (!s_slots) {
        ESP_LOGE(TAG, "kein PSRAM für %d Fächer — Verlauf bleibt leer",
                 BATT_HIST_SLOTS);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Verlauf bereit: %d Fächer à %d s, %u kB im PSRAM", BATT_HIST_SLOTS,
             BATT_HIST_SLOT_MS / 1000,
             (unsigned)(BATT_HIST_SLOTS * sizeof(slot_t) / 1024));
    return ESP_OK;
}

static uint32_t slot_now(void)
{
    return (uint32_t)(esp_timer_get_time() / ((int64_t)BATT_HIST_SLOT_MS * 1000));
}

static void slot_clear(uint32_t index)
{
    memset(&s_slots[index % BATT_HIST_SLOTS], 0, sizeof(slot_t));
}

static int16_t clamp_10ma(int64_t ma)
{
    const int64_t v = ma / 10;
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return (int16_t)v;
}

void batt_history_add(int32_t current_ma, uint32_t remaining_mah)
{
    if (!s_slots) {
        return;
    }
    const uint32_t index = slot_now();

    if (!s_started) {
        s_started = true;
        s_open_index = index;
        slot_clear(index);
    } else if (index != s_open_index) {
        /* Zwischen altem und neuem Fach kann eine Lücke liegen — Funkabriss,
         * Akku außer Reichweite. Die betroffenen Fächer werden geleert, damit
         * sie später als Lücke erkennbar sind und nicht mit uralten Werten aus
         * dem Ring antworten. */
        uint32_t gap = index - s_open_index;
        if (gap > BATT_HIST_SLOTS) {
            gap = BATT_HIST_SLOTS;
        }
        for (uint32_t k = 0; k < gap; k++) {
            slot_clear(index - k);
        }
        s_open_index = index;
        s_open_sum_ma = 0;
        s_open_n = 0;
    }

    s_open_sum_ma += current_ma;
    s_open_n++;

    slot_t *slot = &s_slots[index % BATT_HIST_SLOTS];
    slot->mean_10ma = clamp_10ma(s_open_sum_ma / (int64_t)s_open_n);
    slot->cap_100mah = (uint16_t)((remaining_mah / 100 > UINT16_MAX) ? UINT16_MAX
                                                                    : remaining_mah / 100);
    slot->samples = (uint16_t)(s_open_n > UINT16_MAX ? UINT16_MAX : s_open_n);
}

uint32_t batt_history_span_ms(void)
{
    if (!s_started || !s_slots) {
        return 0;
    }

    uint32_t oldest = 0;
    for (uint32_t age = 0; age < BATT_HIST_SLOTS; age++) {
        if (age > s_open_index) {
            break;
        }
        if (s_slots[(s_open_index - age) % BATT_HIST_SLOTS].samples) {
            oldest = age;
        }
    }
    return (oldest + 1) * (uint32_t)BATT_HIST_SLOT_MS;
}

/** Zahl der Fächer, die ein Fenster abdeckt — mindestens eines, höchstens der Ring. */
static uint32_t window_slots(uint32_t window_ms)
{
    uint32_t n = window_ms / BATT_HIST_SLOT_MS;
    if (n == 0) {
        n = 1;
    }
    if (n > BATT_HIST_SLOTS) {
        n = BATT_HIST_SLOTS;
    }
    return n;
}

bool batt_history_drain(uint32_t window_ms, uint32_t *drain_ma, uint32_t *covered_ms)
{
    if (!s_started || !s_slots) {
        return false;
    }

    const uint32_t n = window_slots(window_ms);
    int64_t sum = 0;
    uint64_t weight = 0;
    uint32_t filled = 0;

    for (uint32_t age = 0; age < n; age++) {
        /* Weiter zurück als der Systemstart gibt es nichts — ohne diese
         * Schranke liefe die Schleife in nie beschriebene Ringfächer. */
        if (age > s_open_index) {
            break;
        }
        const slot_t *slot = &s_slots[(s_open_index - age) % BATT_HIST_SLOTS];
        if (slot->samples == 0) {
            continue;
        }
        /* Nur die Entnahme zählt; geladene Fächer gehen mit null ein. */
        if (slot->mean_10ma < 0) {
            sum += (int64_t)(-slot->mean_10ma) * 10 * slot->samples;
        }
        weight += slot->samples;
        filled++;
    }

    if (weight == 0) {
        return false;
    }
    if (drain_ma) {
        *drain_ma = (uint32_t)(sum / (int64_t)weight);
    }
    if (covered_ms) {
        *covered_ms = filled * (uint32_t)BATT_HIST_SLOT_MS;
    }
    return true;
}

size_t batt_history_series(uint32_t window_ms, size_t points, int32_t *current_ma,
                           int32_t *cap_mah)
{
    if (points == 0 || !s_slots) {
        return 0;
    }

    const uint32_t total = window_slots(window_ms);
    size_t got = 0;

    for (size_t p = 0; p < points; p++) {
        /* Stützstelle 0 ist die älteste. `age` zählt in Fächern rückwärts, der
         * Abschnitt [a0, a1) gehört zu dieser Stützstelle. */
        const uint32_t a0 = (uint32_t)((uint64_t)total * (points - 1 - p) / points);
        const uint32_t a1 = (uint32_t)((uint64_t)total * (points - p) / points);

        int64_t sum = 0;
        uint64_t weight = 0;
        int32_t cap = BATT_HIST_NONE;

        for (uint32_t age = a0; age < a1 && s_started; age++) {
            if (age > s_open_index) {
                break;
            }
            const slot_t *slot = &s_slots[(s_open_index - age) % BATT_HIST_SLOTS];
            if (slot->samples == 0) {
                continue;
            }
            sum += (int64_t)slot->mean_10ma * 10 * slot->samples;
            weight += slot->samples;
            /* a0 ist das jüngste Fach des Abschnitts: der erste Treffer ist
             * damit der aktuellste Kapazitätswert. */
            if (cap == BATT_HIST_NONE) {
                cap = (int32_t)slot->cap_100mah * 100;
            }
        }

        if (current_ma) {
            current_ma[p] = weight ? (int32_t)(sum / (int64_t)weight) : BATT_HIST_NONE;
        }
        if (cap_mah) {
            cap_mah[p] = cap;
        }
        if (weight) {
            got++;
        }
    }

    return got;
}

bool batt_history_runtime(uint32_t window_ms, uint32_t remaining_mah,
                          uint32_t *minutes, uint32_t *covered_ms)
{
    uint32_t drain_ma = 0;

    if (!batt_history_drain(window_ms, &drain_ma, covered_ms) || drain_ma == 0) {
        return false;
    }
    if (minutes) {
        /* In Minuten, nicht in Stunden: Ein fast leerer Akku soll "20 Min"
         * sagen können und nicht "0 h". 64 Bit, weil Kapazität mal 60 den
         * 32-Bit-Bereich streifen kann. */
        *minutes = (uint32_t)(((uint64_t)remaining_mah * 60) / drain_ma);
    }
    return true;
}
