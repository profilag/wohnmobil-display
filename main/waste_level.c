#include "bsp_ws_lcd7.h"
#include "waste_level.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "waste";

static waste_measurement_t s_last;
/** Zeitpunkt der letzten erfolgreichen Prüfung, -1 = noch keine. */
static int64_t s_last_us = -1;
static bool s_excited;
/** Summierte Bestromungszeit — Maß dafür, wie viel Elektrolyse zugelassen wurde. */
static int64_t s_excite_total_us;
static int64_t s_excite_started_us;

/* -------------------------------------------------------------------------- */

esp_err_t waste_level_init(void)
{
    ESP_RETURN_ON_FALSE(bsp_aw9523b_get_handle(), ESP_ERR_INVALID_STATE, TAG,
                        "AW9523B nicht vorhanden — Abwasserprüfung nicht möglich");

    /* Anregung sicher aus, bevor der Eingang überhaupt scharf wird. */
    ESP_RETURN_ON_ERROR(bsp_output_pb_configure(1u << WASTE_EXCITE_PB_BIT), TAG,
                        "Anregungsausgang");
    ESP_RETURN_ON_ERROR(bsp_output_pb_set(WASTE_EXCITE_PB_BIT, false), TAG, "Anregung aus");
    ESP_RETURN_ON_ERROR(bsp_input_pb_configure(1u << WASTE_PB_BIT_FULL), TAG, "Elektrodeneingang");

    memset(&s_last, 0, sizeof(s_last));
    s_last_us = -1;
    s_excited = false;
    s_excite_total_us = 0;

    ESP_LOGI(TAG, "Abwasserprüfung bereit: Anregung PB%d, Elektrode PB%d, Takt %d s",
             WASTE_EXCITE_PB_BIT, WASTE_PB_BIT_FULL, WASTE_CHECK_INTERVAL_MS / 1000);
    return ESP_OK;
}

/** Rechnet ein rohes Port-B-Byte in den Elektrodenzustand um. */
static void waste_level_decode(uint8_t raw_pb, waste_measurement_t *out)
{
    const uint8_t bit = (raw_pb >> WASTE_PB_BIT_FULL) & 0x01;
    out->raw_pb = raw_pb;
    out->full = (bit == WASTE_ELECTRODE_WET_LEVEL);
}

esp_err_t waste_level_measure(waste_measurement_t *out)
{
    waste_measurement_t result = {0};

    /* Anregung an — ab hier zählt jede Millisekunde für die Elektrolyse. */
    esp_err_t ret = bsp_output_pb_set(WASTE_EXCITE_PB_BIT, true);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Anregung ließ sich nicht einschalten: %s", esp_err_to_name(ret));
        return ret;
    }
    const int64_t t_on = esp_timer_get_time();

    vTaskDelay(pdMS_TO_TICKS(WASTE_SETTLE_MS));

    /* Mehrheitsentscheid wie beim Frischwasser: Wasser im fahrenden Wohnmobil
     * schwappt, und ein einzelner Messwert kann die Elektrode zufällig treffen
     * oder verpassen. */
    int wet_votes = 0;
    int valid = 0;
    for (int s = 0; s < WASTE_SAMPLE_COUNT; s++) {
        uint8_t raw = 0;
        if (bsp_input_pb_read(&raw) == ESP_OK) {
            valid++;
            if (((raw >> WASTE_PB_BIT_FULL) & 0x01) == WASTE_ELECTRODE_WET_LEVEL) {
                wet_votes++;
            }
            result.raw_pb = raw;
        }
        if (s + 1 < WASTE_SAMPLE_COUNT) {
            vTaskDelay(pdMS_TO_TICKS(WASTE_SAMPLE_GAP_MS));
        }
    }

    /* Anregung in jedem Fall wieder aus, auch bei Lesefehlern. */
    esp_err_t off_ret = bsp_output_pb_set(WASTE_EXCITE_PB_BIT, false);
    const int64_t on_us = esp_timer_get_time() - t_on;
    s_excite_total_us += on_us;

    if (off_ret != ESP_OK) {
        ESP_LOGE(TAG, "Anregung ließ sich nicht abschalten: %s", esp_err_to_name(off_ret));
        return off_ret;
    }
    ESP_RETURN_ON_FALSE(valid > 0, ESP_ERR_TIMEOUT, TAG, "Elektrode nicht gelesen");

    result.full = (wet_votes * 2 > valid);

    const bool changed = (s_last_us < 0) || (s_last.full != result.full);
    s_last = result;
    s_last_us = esp_timer_get_time();
    if (out) {
        *out = result;
    }

    /* Nur beim Wechsel auf Info-Ebene protokollieren — sonst füllt der
     * 5-Minuten-Takt das Log mit immer derselben Zeile. */
    if (changed) {
        ESP_LOGI(TAG, "Abwassertank: %s  (PortB=0x%02X, Anregung %lld ms, gesamt %lld ms)",
                 result.full ? "VOLL" : "nicht voll", result.raw_pb,
                 on_us / 1000, s_excite_total_us / 1000);
    } else {
        ESP_LOGD(TAG, "Abwassertank unverändert: %s (PortB=0x%02X)",
                 result.full ? "VOLL" : "nicht voll", result.raw_pb);
    }
    return ESP_OK;
}

const waste_measurement_t *waste_level_last(void)
{
    return &s_last;
}

int64_t waste_level_age_ms(void)
{
    if (s_last_us < 0) {
        return -1;
    }
    return (esp_timer_get_time() - s_last_us) / 1000;
}

esp_err_t waste_level_excite(bool on)
{
    ESP_RETURN_ON_ERROR(bsp_output_pb_set(WASTE_EXCITE_PB_BIT, on), TAG, "Anregung schalten");

    if (on && !s_excited) {
        s_excite_started_us = esp_timer_get_time();
        ESP_LOGW(TAG, "Testmodus: Elektrode dauerhaft bestromt — Elektrolyse!");
    } else if (!on && s_excited) {
        const int64_t on_us = esp_timer_get_time() - s_excite_started_us;
        s_excite_total_us += on_us;
        ESP_LOGI(TAG, "Testmodus beendet nach %lld ms (Bestromung gesamt %lld ms)",
                 on_us / 1000, s_excite_total_us / 1000);
    }
    s_excited = on;
    return ESP_OK;
}

bool waste_level_is_excited(void)
{
    return s_excited;
}

esp_err_t waste_level_read_raw(waste_measurement_t *out)
{
    ESP_RETURN_ON_FALSE(out, ESP_ERR_INVALID_ARG, TAG, "kein Zielzeiger");

    uint8_t raw = 0;
    ESP_RETURN_ON_ERROR(bsp_input_pb_read(&raw), TAG, "Port B lesen");
    waste_level_decode(raw, out);
    return ESP_OK;
}
