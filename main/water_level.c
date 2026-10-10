#include "bsp_ws_lcd7.h"
#include "water_level.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "water";

/** Maske der drei Elektrodeneingänge auf Port B. */
#define WATER_PB_MASK ((1u << WATER_PB_BIT_LOW) | \
                       (1u << WATER_PB_BIT_MID) | \
                       (1u << WATER_PB_BIT_HIGH))

static const uint8_t s_pb_bits[WATER_ELECTRODE_COUNT] = {
    WATER_PB_BIT_LOW,
    WATER_PB_BIT_MID,
    WATER_PB_BIT_HIGH,
};

static water_measurement_t s_last;
static bool s_excited;
/** Summierte Bestromungszeit — Maß dafür, wie viel Elektrolyse zugelassen wurde. */
static int64_t s_excite_total_us;
static int64_t s_excite_started_us;

/* -------------------------------------------------------------------------- */

esp_err_t water_level_init(void)
{
    ESP_RETURN_ON_FALSE(bsp_aw9523b_get_handle(), ESP_ERR_INVALID_STATE, TAG,
                        "AW9523B nicht vorhanden — Wasserstandsmessung nicht möglich");

    /* Anregung sicher aus, bevor die Eingänge überhaupt scharf werden. */
    ESP_RETURN_ON_ERROR(bsp_output_pb_configure(1u << WATER_EXCITE_PB_BIT), TAG,
                        "Anregungsausgang");
    ESP_RETURN_ON_ERROR(bsp_output_pb_set(WATER_EXCITE_PB_BIT, false), TAG, "Anregung aus");
    ESP_RETURN_ON_ERROR(bsp_input_pb_configure(WATER_PB_MASK), TAG, "Elektrodeneingänge");

    memset(&s_last, 0, sizeof(s_last));
    s_excited = false;
    s_excite_total_us = 0;

    ESP_LOGI(TAG, "Wasserstandsmessung bereit: Anregung PB%d, Elektroden PB%d/PB%d/PB%d",
             WATER_EXCITE_PB_BIT, WATER_PB_BIT_LOW, WATER_PB_BIT_MID, WATER_PB_BIT_HIGH);
    return ESP_OK;
}

/** Rechnet ein rohes Port-B-Byte in Elektrodenzustände und Füllstand um. */
static void water_level_decode(uint8_t raw_pb, water_measurement_t *out)
{
    out->raw_pb = raw_pb;

    for (int i = 0; i < WATER_ELECTRODE_COUNT; i++) {
        const uint8_t bit = (raw_pb >> s_pb_bits[i]) & 0x01;
        out->wet[i] = (bit == WATER_ELECTRODE_WET_LEVEL);
    }

    /* Von unten nach oben zählen und beim ersten trockenen Kontakt aufhören.
     * Eine nasse Elektrode darüber wäre physikalisch unmöglich — wir melden das
     * als unplausibel, statt einen zu hohen Stand anzuzeigen. Ein zu niedrig
     * angezeigter Tank ist die harmlosere Fehlerrichtung. */
    int contiguous = 0;
    while (contiguous < WATER_ELECTRODE_COUNT && out->wet[contiguous]) {
        contiguous++;
    }

    bool wet_above = false;
    for (int i = contiguous; i < WATER_ELECTRODE_COUNT; i++) {
        if (out->wet[i]) {
            wet_above = true;
        }
    }

    out->implausible = wet_above;
    out->level = (water_level_t)contiguous;
}

esp_err_t water_level_measure(water_measurement_t *out)
{
    water_measurement_t result = {0};

    /* Anregung an — ab hier zählt jede Millisekunde für die Elektrolyse. */
    esp_err_t ret = bsp_output_pb_set(WATER_EXCITE_PB_BIT, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Anregung ließ sich nicht einschalten: %s", esp_err_to_name(ret));
        return ret;
    }
    const int64_t t_on = esp_timer_get_time();

    vTaskDelay(pdMS_TO_TICKS(WATER_SETTLE_MS));

    /* Mehrheitsentscheid über mehrere Abtastungen: Wasser im fahrenden Wohnmobil
     * schwappt, und ein einzelner Messwert kann eine Elektrode zufällig treffen
     * oder verpassen. */
    uint8_t votes[8] = {0};
    int valid = 0;
    for (int s = 0; s < WATER_SAMPLE_COUNT; s++) {
        uint8_t raw = 0;
        if (bsp_input_pb_read(&raw) == ESP_OK) {
            valid++;
            for (int bit = 0; bit < 8; bit++) {
                if ((raw >> bit) & 0x01) {
                    votes[bit]++;
                }
            }
        }
        if (s + 1 < WATER_SAMPLE_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(WATER_SAMPLE_GAP_MS));
        }
    }

    /* Anregung in jedem Fall wieder aus, auch bei Lesefehlern. */
    esp_err_t off_ret = bsp_output_pb_set(WATER_EXCITE_PB_BIT, false);
    const int64_t on_us = esp_timer_get_time() - t_on;
    s_excite_total_us += on_us;

    if (off_ret != ESP_OK) {
        ESP_LOGE(TAG, "Anregung ließ sich nicht abschalten: %s", esp_err_to_name(off_ret));
        return off_ret;
    }
    ESP_RETURN_ON_FALSE(valid > 0, ESP_ERR_TIMEOUT, TAG, "keine Elektrode gelesen");

    uint8_t majority = 0;
    for (int bit = 0; bit < 8; bit++) {
        if (votes[bit] * 2 > valid) {
            majority |= (1u << bit);
        }
    }

    water_level_decode(majority, &result);
    s_last = result;
    if (out) {
        *out = result;
    }

    ESP_LOGI(TAG, "Messung: %s  (PortB=0x%02X, unten=%s mitte=%s oben=%s, "
                  "Anregung %lld ms, gesamt %lld ms)%s",
             water_level_text(result.level), result.raw_pb,
             result.wet[0] ? "nass" : "trocken",
             result.wet[1] ? "nass" : "trocken",
             result.wet[2] ? "nass" : "trocken",
             on_us / 1000, s_excite_total_us / 1000,
             result.implausible ? "  ACHTUNG: unplausibles Muster" : "");

    if (result.implausible) {
        ESP_LOGW(TAG, "Elektrode über einer trockenen Elektrode ist nass — "
                      "Leitungsbruch, Korrosion oder vertauschte Leitung prüfen");
    }
    return ESP_OK;
}

const water_measurement_t *water_level_last(void)
{
    return &s_last;
}

esp_err_t water_level_excite(bool on)
{
    ESP_RETURN_ON_ERROR(bsp_output_pb_set(WATER_EXCITE_PB_BIT, on), TAG, "Anregung schalten");

    if (on && !s_excited) {
        s_excite_started_us = esp_timer_get_time();
        ESP_LOGW(TAG, "Testmodus: Elektroden dauerhaft bestromt — Elektrolyse!");
    } else if (!on && s_excited) {
        const int64_t on_us = esp_timer_get_time() - s_excite_started_us;
        s_excite_total_us += on_us;
        ESP_LOGI(TAG, "Testmodus beendet nach %lld ms (Bestromung gesamt %lld ms)",
                 on_us / 1000, s_excite_total_us / 1000);
    }
    s_excited = on;
    return ESP_OK;
}

bool water_level_is_excited(void)
{
    return s_excited;
}

esp_err_t water_level_read_raw(water_measurement_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "kein Zielzeiger");

    uint8_t raw = 0;
    ESP_RETURN_ON_ERROR(bsp_input_pb_read(&raw), TAG, "Port B lesen");
    water_level_decode(raw, out);
    return ESP_OK;
}

const char *water_level_text(water_level_t level)
{
    switch (level) {
    case WATER_LEVEL_EMPTY:     return "leer";
    case WATER_LEVEL_THIRD:     return "1/3 voll";
    case WATER_LEVEL_TWO_THIRD: return "2/3 voll";
    case WATER_LEVEL_FULL:      return "voll";
    default:                    return "unbekannt";
    }
}

int water_level_percent(water_level_t level)
{
    static const int pct[] = {0, 33, 66, 100};
    return (level <= WATER_LEVEL_FULL) ? pct[level] : 0;
}
