#include "ble_host.h"

#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static const char *TAG = "blehost";

static ble_host_ready_cb_t s_cbs[BLE_HOST_MAX_CALLBACKS];
static uint8_t s_cb_count;
static bool s_started;
static volatile bool s_ready;
static uint8_t s_own_addr_type;

esp_err_t ble_host_register(ble_host_ready_cb_t cb)
{
    ESP_RETURN_ON_FALSE(cb, ESP_ERR_INVALID_ARG, TAG, "kein Rückruf");
    ESP_RETURN_ON_FALSE(!s_started, ESP_ERR_INVALID_STATE, TAG,
                        "Host läuft schon — zu spät zum Eintragen");
    ESP_RETURN_ON_FALSE(s_cb_count < BLE_HOST_MAX_CALLBACKS, ESP_ERR_NO_MEM, TAG,
                        "Rückrufliste voll");
    s_cbs[s_cb_count++] = cb;
    return ESP_OK;
}

static void ble_host_on_sync(void)
{
    /* Ohne eigene Adresse geht kein GAP-Aufruf; das ist kein Zustand, in dem
     * sich weiterarbeiten ließe. */
    int rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "keine eigene Adresse (rc=%d)", rc);
        return;
    }
    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Adresstyp nicht bestimmbar (rc=%d)", rc);
        return;
    }

    s_ready = true;
    ESP_LOGI(TAG, "NimBLE synchronisiert (Adresstyp %u)", s_own_addr_type);

    for (uint8_t i = 0; i < s_cb_count; i++) {
        s_cbs[i]();
    }
}

static void ble_host_on_reset(int reason)
{
    /* Der Controller setzt sich neu auf; alle Verbindungen und der Scan sind
     * damit weg. Die Module erfahren das über ihre eigenen GAP-Ereignisse. */
    ESP_LOGW(TAG, "NimBLE zurückgesetzt (Grund %d)", reason);
    s_ready = false;
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run(); /* kehrt erst bei nimble_port_stop() zurück */
    nimble_port_freertos_deinit();
}

esp_err_t ble_host_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "NimBLE wird hochgefahren …");
    ESP_RETURN_ON_ERROR(nimble_port_init(), TAG, "nimble_port_init");

    ble_hs_cfg.sync_cb = ble_host_on_sync;
    ble_hs_cfg.reset_cb = ble_host_on_reset;

    /* Kopplung ohne Bestätigung am Gerät („Just Works") — das Board hat weder
     * Tastatur noch Anzeige aus Sicht der Gegenstelle. Angestoßen wird sie
     * ausdrücklich, nie von allein; siehe ble_probe_pair(). */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    s_started = true;
    nimble_port_freertos_init(ble_host_task);
    return ESP_OK;
}

bool ble_host_is_ready(void)
{
    return s_ready;
}

uint8_t ble_host_own_addr_type(void)
{
    return s_own_addr_type;
}
