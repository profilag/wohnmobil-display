#include "led_ble.h"

#include <stdio.h>
#include <string.h>

#include "ble_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"

static const char *TAG = "led";

/** Dienst und Schreib-Charakteristik des ELK-BLEDOM. */
static const ble_uuid16_t UUID_SVC = BLE_UUID16_INIT(0xFFF0);
static const ble_uuid16_t UUID_CHR = BLE_UUID16_INIT(0xFFF3);

#define LED_CMD_LEN 9

static SemaphoreHandle_t s_lock;
static volatile led_ble_state_t s_state = LED_BLE_IDLE;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_val_handle;
static uint16_t s_svc_start, s_svc_end;
static ble_addr_t s_addr;
static bool s_have_addr;
static int8_t s_rssi;
static led_ble_shadow_t s_shadow = {
    .r = 255, .g = 255, .b = 255, .brightness = 100, .speed = 50, .warm = 50,
};

/* -------------------------------------------------------------------------- */
/*  Befehle                                                                   */
/* -------------------------------------------------------------------------- */

/**
 * Neun Byte an die Charakteristik, ohne Antwort. Der Controller quittiert
 * nichts — ein Fehler hier heißt also "nicht abgeschickt", nicht "nicht
 * ausgeführt".
 */
static esp_err_t led_send(uint8_t b2, uint8_t b3, uint8_t b4, uint8_t b5, uint8_t b6)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint16_t conn = s_conn;
    const uint16_t handle = s_val_handle;
    const bool ready = (s_state == LED_BLE_READY);
    xSemaphoreGive(s_lock);

    ESP_RETURN_ON_FALSE(ready && handle != 0, ESP_ERR_INVALID_STATE, TAG,
                        "nicht verbunden");

    const uint8_t cmd[LED_CMD_LEN] = {0x7E, 0x00, b2, b3, b4, b5, b6, 0x00, 0xEF};
    const int rc = ble_gattc_write_no_rsp_flat(conn, handle, cmd, sizeof(cmd));
    if (rc != 0) {
        ESP_LOGW(TAG, "Schreiben fehlgeschlagen (rc=%d)", rc);
        return ESP_FAIL;
    }
    ESP_LOGD(TAG, "gesendet: 7E 00 %02X %02X %02X %02X %02X 00 EF", b2, b3, b4, b5, b6);
    return ESP_OK;
}

esp_err_t led_ble_power(bool on)
{
    /* Die überzähligen Bytes sind laut Mitschnitten belanglos; übernommen sind
     * die Werte, die in der Praxis am häufigsten funktionieren. */
    esp_err_t ret = on ? led_send(0x04, 0xF0, 0x00, 0x01, 0xFF)
                       : led_send(0x04, 0x00, 0x00, 0x00, 0xFF);
    if (ret == ESP_OK) {
        s_shadow.power = on;
    }
    return ret;
}

esp_err_t led_ble_color(uint8_t r, uint8_t g, uint8_t b)
{
    esp_err_t ret = led_send(0x05, 0x03, r, g, b);
    if (ret == ESP_OK) {
        s_shadow.r = r;
        s_shadow.g = g;
        s_shadow.b = b;
        /* Eine feste Farbe beendet den laufenden Effekt. */
        s_shadow.effect_active = false;
    }
    return ret;
}

/** 0…100 begrenzen — der Controller reagiert auf größere Werte unvorhersehbar. */
static uint8_t clamp100(uint8_t v)
{
    return v > 100 ? 100 : v;
}

esp_err_t led_ble_brightness(uint8_t percent)
{
    percent = clamp100(percent);
    esp_err_t ret = led_send(0x01, percent, 0x00, 0x00, 0x00);
    if (ret == ESP_OK) {
        s_shadow.brightness = percent;
    }
    return ret;
}

esp_err_t led_ble_speed(uint8_t percent)
{
    percent = clamp100(percent);
    esp_err_t ret = led_send(0x02, percent, 0x00, 0x00, 0x00);
    if (ret == ESP_OK) {
        s_shadow.speed = percent;
    }
    return ret;
}

esp_err_t led_ble_effect(uint8_t id)
{
    if (id < LED_BLE_EFFECT_FIRST || id > LED_BLE_EFFECT_LAST) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = led_send(0x03, id, 0x03, 0x00, 0x00);
    if (ret == ESP_OK) {
        s_shadow.effect = id;
        s_shadow.effect_active = true;
    }
    return ret;
}

esp_err_t led_ble_white(uint8_t warm_percent)
{
    warm_percent = clamp100(warm_percent);
    /* Warm und Kalt müssen sich zu 100 ergänzen. */
    esp_err_t ret = led_send(0x05, 0x02, warm_percent, (uint8_t)(100 - warm_percent), 0x00);
    if (ret == ESP_OK) {
        s_shadow.warm = warm_percent;
        s_shadow.effect_active = false;
    }
    return ret;
}

const led_ble_shadow_t *led_ble_shadow(void)
{
    return &s_shadow;
}

const char *led_ble_effect_name(uint8_t index)
{
    /* Die Namen stammen aus Community-Mitschnitten und sind nur für diese vier
     * IDs belegt. Alles andere bekommt eine Nummer — welcher Effekt sich
     * dahinter verbirgt, zeigt am schnellsten das Ausprobieren. */
    static char buf[24];

    if (index >= LED_BLE_EFFECT_COUNT) {
        return "—";
    }
    switch (LED_BLE_EFFECT_FIRST + index) {
    case 0x87: return "08  Sprung RGB";
    case 0x89: return "10  Verlauf RGB";
    case 0x8A: return "11  Verlauf 7 Farben";
    case 0x95: return "22  Blinken 7 Farben";
    default:
        snprintf(buf, sizeof(buf), "%02u", (unsigned)index + 1);
        return buf;
    }
}

/* -------------------------------------------------------------------------- */
/*  Verbindungsaufbau                                                         */
/* -------------------------------------------------------------------------- */

static void set_state(led_ble_state_t state)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = state;
    xSemaphoreGive(s_lock);
}

const char *led_ble_state_text(led_ble_state_t state)
{
    switch (state) {
    case LED_BLE_IDLE:        return "nicht verbunden";
    case LED_BLE_SEARCHING:   return "sucht Streifen …";
    case LED_BLE_CONNECTING:  return "verbindet …";
    case LED_BLE_DISCOVERING: return "sucht Dienst …";
    case LED_BLE_READY:       return "verbunden";
    case LED_BLE_FAILED:      return "nicht gefunden";
    default:                  return "?";
    }
}

led_ble_state_t led_ble_get_state(void)
{
    return s_state;
}

bool led_ble_is_ready(void)
{
    return s_state == LED_BLE_READY;
}

int8_t led_ble_rssi(void)
{
    return s_rssi;
}

/** Merkmale des gefundenen Dienstes durchgehen. */
static int on_chr(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn;
    (void)arg;

    if (error->status == 0 && chr) {
        const bool writable = (chr->properties & (BLE_GATT_CHR_PROP_WRITE |
                                                  BLE_GATT_CHR_PROP_WRITE_NO_RSP)) != 0;
        const bool is_target = ble_uuid_cmp(&chr->uuid.u, &UUID_CHR.u) == 0;

        /* Bevorzugt 0xFFF3. Falls eine Baureihe eine andere UUID benutzt, tut
         * es auch das erste beschreibbare Merkmal — besser als aufgeben. */
        if (writable && (is_target || s_val_handle == 0)) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_val_handle = chr->val_handle;
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "Schreib-Merkmal gefunden: Handle %u%s",
                     chr->val_handle, is_target ? " (0xFFF3)" : " (Ersatzwahl)");
        }
        return 0;
    }

    if (error->status != BLE_HS_EDONE) {
        ESP_LOGE(TAG, "Merkmalssuche fehlgeschlagen (Status %d)", error->status);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    if (s_val_handle == 0) {
        ESP_LOGE(TAG, "kein beschreibbares Merkmal in Dienst 0xFFF0");
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    set_state(LED_BLE_READY);
    ESP_LOGI(TAG, "Streifen bereit");
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;

    if (error->status == 0 && service) {
        s_svc_start = service->start_handle;
        s_svc_end = service->end_handle;
        return 0;
    }

    if (error->status != BLE_HS_EDONE || s_svc_start == 0) {
        ESP_LOGE(TAG, "Dienst 0xFFF0 nicht gefunden (Status %d)", error->status);
        ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    ESP_LOGI(TAG, "Dienst 0xFFF0 gefunden (Handles %u…%u)", s_svc_start, s_svc_end);
    ble_gattc_disc_all_chrs(conn, s_svc_start, s_svc_end, on_chr, NULL);
    return 0;
}

static int led_gap_event(struct ble_gap_event *event, void *arg);

static void connect_to_addr(void)
{
    set_state(LED_BLE_CONNECTING);
    const int rc = ble_gap_connect(ble_host_own_addr_type(), &s_addr, 10000, NULL,
                                   led_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect fehlgeschlagen (rc=%d)", rc);
        set_state(LED_BLE_FAILED);
    }
}

static int led_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (s_state != LED_BLE_SEARCHING) {
            break;
        }
        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));
        ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);
        if (fields.name_len == 0 || !fields.name) {
            break;
        }
        const size_t plen = strlen(LED_BLE_NAME_PREFIX);
        if (fields.name_len < plen || memcmp(fields.name, LED_BLE_NAME_PREFIX, plen) != 0) {
            break;
        }

        s_addr = event->disc.addr;
        s_have_addr = true;
        s_rssi = event->disc.rssi;
        ESP_LOGI(TAG, "Streifen gefunden (%d dBm), Suche beenden", s_rssi);
        /* Erst den Scan abbrechen — verbinden geht währenddessen nicht. Beides
         * aus dem GAP-Rückruf heraus ist der von NimBLE vorgesehene Weg. */
        ble_gap_disc_cancel();
        connect_to_addr();
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_state == LED_BLE_SEARCHING) {
            ESP_LOGW(TAG, "Streifen nicht gefunden");
            set_state(LED_BLE_FAILED);
        }
        break;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "Verbindung fehlgeschlagen (Status %d)", event->connect.status);
            set_state(LED_BLE_FAILED);
            break;
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_conn = event->connect.conn_handle;
        s_val_handle = 0;
        s_svc_start = 0;
        s_svc_end = 0;
        s_state = LED_BLE_DISCOVERING;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "verbunden, suche Dienst 0xFFF0");
        ble_gattc_disc_svc_by_uuid(event->connect.conn_handle, &UUID_SVC.u, on_svc, NULL);
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "getrennt (Grund %d)", event->disconnect.reason);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_val_handle = 0;
        s_state = LED_BLE_IDLE;
        xSemaphoreGive(s_lock);
        break;

    default:
        break;
    }
    return 0;
}

/** Läuft im Host-Task, sobald der Controller bereit ist. */
static void led_ble_on_host_ready(void)
{
    if (s_state != LED_BLE_SEARCHING) {
        return;
    }
    if (s_have_addr) {
        /* Adresse von einem früheren Versuch — direkt verbinden spart die Suche. */
        connect_to_addr();
        return;
    }

    const struct ble_gap_disc_params params = {
        .itvl = 160,   /* 100 ms */
        .window = 96,  /*  60 ms */
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 0, /* aktiv — der Name steht oft erst in der Scan-Response */
        .filter_duplicates = 1,
    };
    const int rc = ble_gap_disc(ble_host_own_addr_type(), LED_BLE_SEARCH_MS, &params,
                                led_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Suche ließ sich nicht starten (rc=%d)", rc);
        set_state(LED_BLE_FAILED);
    }
}

/* -------------------------------------------------------------------------- */
/*  Öffentliche API                                                           */
/* -------------------------------------------------------------------------- */

esp_err_t led_ble_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "Mutex");
    }
    ESP_RETURN_ON_ERROR(ble_host_register(led_ble_on_host_ready), TAG, "Rückruf eintragen");
    ESP_LOGI(TAG, "LED-Client bereit — sucht \"%s\" auf Anforderung", LED_BLE_NAME_PREFIX);
    return ESP_OK;
}

esp_err_t led_ble_connect(void)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "led_ble_init() fehlt");

    if (s_state == LED_BLE_READY || s_state == LED_BLE_CONNECTING ||
        s_state == LED_BLE_DISCOVERING || s_state == LED_BLE_SEARCHING) {
        return ESP_OK;
    }

    set_state(LED_BLE_SEARCHING);

    if (!ble_host_is_ready()) {
        return ble_host_start(); /* weiter im Rückruf */
    }
    led_ble_on_host_ready();
    return ESP_OK;
}

esp_err_t led_ble_disconnect(void)
{
    if (s_state == LED_BLE_SEARCHING) {
        ble_gap_disc_cancel();
        set_state(LED_BLE_IDLE);
        return ESP_OK;
    }
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        set_state(LED_BLE_IDLE);
        return ESP_OK;
    }
    /* Der DISCONNECT-Rückruf räumt den Rest auf. */
    const int rc = ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}
