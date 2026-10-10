#include "ble_scan.h"

#include <ctype.h>
#include <string.h>

#include "ble_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"

static const char *TAG = "ble";

/* Scan-Fenster 60 ms alle 200 ms — rund 30 % Tastverhältnis. Dauerempfang
 * fände kaum mehr Geräte, würde aber unnötig neben dem RGB-Panel funken. */
#define BLE_SCAN_ITVL_MS 200
#define BLE_SCAN_WIN_MS  60
/** NimBLE rechnet Scan-Zeiten in Einheiten von 0,625 ms. */
#define BLE_SCAN_UNITS(ms) (((ms) * 8) / 5)

typedef enum {
    STATE_OFF = 0,   /*!< NimBLE läuft nicht oder ist gestoppt */
    STATE_STARTING,  /*!< Stack fährt hoch, Scan beginnt nach dem Sync */
    STATE_RUNNING,
} scan_state_t;

static SemaphoreHandle_t s_lock;
static scan_state_t s_state;
static ble_scan_mfg_cb_t s_mfg_cb;

static ble_scan_dev_t s_devs[BLE_SCAN_MAX_DEVICES];
static size_t s_dev_count;
static uint32_t s_generation;

/* -------------------------------------------------------------------------- */
/*  Erkennung                                                                 */
/* -------------------------------------------------------------------------- */

static bool starts_with_ci(const char *s, const char *prefix)
{
    while (*prefix) {
        if (tolower((unsigned char)*s) != tolower((unsigned char)*prefix)) {
            return false;
        }
        s++;
        prefix++;
    }
    return true;
}

/**
 * Geräteart raten. Victron ist sicher (eingetragene Hersteller-ID), alles
 * andere ist ein Namensmuster — die BMS-Hersteller liefern ihre Module unter
 * wechselnden Handelsmarken aus, der Advertising-Name bleibt aber meist der
 * des OEM.
 */
static ble_scan_kind_t classify(const ble_scan_dev_t *dev)
{
    if (dev->has_company && dev->company == BLE_SCAN_COMPANY_VICTRON) {
        return BLE_SCAN_KIND_VICTRON;
    }
    if (dev->name[0] == '\0') {
        return BLE_SCAN_KIND_UNKNOWN;
    }
    if (starts_with_ci(dev->name, "xiaoxiang") || starts_with_ci(dev->name, "JBD")) {
        return BLE_SCAN_KIND_JBD;
    }
    if (starts_with_ci(dev->name, "JK_") || starts_with_ci(dev->name, "JK-")) {
        return BLE_SCAN_KIND_JK;
    }
    if (starts_with_ci(dev->name, "DL-") || starts_with_ci(dev->name, "DALY")) {
        return BLE_SCAN_KIND_DALY;
    }
    return BLE_SCAN_KIND_UNKNOWN;
}

const char *ble_scan_kind_text(ble_scan_kind_t kind)
{
    switch (kind) {
    case BLE_SCAN_KIND_VICTRON: return "Victron";
    case BLE_SCAN_KIND_JBD:     return "JBD/Xiaoxiang?";
    case BLE_SCAN_KIND_JK:      return "JK-BMS?";
    case BLE_SCAN_KIND_DALY:    return "Daly?";
    default:                    return "";
    }
}

void ble_scan_addr_text(const ble_scan_dev_t *dev, char *buf, size_t len)
{
    /* NimBLE liefert die Adresse little-endian, angezeigt wird sie andersherum. */
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
             dev->addr[5], dev->addr[4], dev->addr[3],
             dev->addr[2], dev->addr[1], dev->addr[0]);
}

void ble_scan_services_text(const ble_scan_dev_t *dev, char *buf, size_t len)
{
    int n = 0;
    buf[0] = '\0';

    for (uint8_t i = 0; i < dev->n_svc16 && n < (int)len; i++) {
        n += snprintf(buf + n, len - n, "%s%04X", n ? " " : "", dev->svc16[i]);
    }
    if (dev->has_svc128 && n < (int)len) {
        /* 128-Bit-UUIDs liegen little-endian im Paket; angezeigt wird der
         * Anfang der UUID, also die letzten Bytes des Puffers. */
        snprintf(buf + n, len - n, "%s%02X%02X%02X%02X…", n ? " " : "",
                 dev->svc128[15], dev->svc128[14], dev->svc128[13], dev->svc128[12]);
    }
}

/* -------------------------------------------------------------------------- */
/*  Geräteliste                                                               */
/* -------------------------------------------------------------------------- */

/** Ruft der NimBLE-Host-Task auf. Der Mutex ist hier zu nehmen. */
static void device_seen(const ble_addr_t *addr, int8_t rssi,
                        const struct ble_hs_adv_fields *fields)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);

    ble_scan_dev_t *dev = NULL;
    for (size_t i = 0; i < s_dev_count; i++) {
        if (memcmp(s_devs[i].addr, addr->val, 6) == 0) {
            dev = &s_devs[i];
            break;
        }
    }

    if (!dev) {
        if (s_dev_count < BLE_SCAN_MAX_DEVICES) {
            dev = &s_devs[s_dev_count++];
        } else {
            /* Liste voll: den schwächsten Eintrag verdrängen, aber nur wenn der
             * neue tatsächlich besser ankommt — sonst rotiert die Anzeige
             * endlos durch fernes Rauschen. */
            ble_scan_dev_t *weakest = &s_devs[0];
            for (size_t i = 1; i < s_dev_count; i++) {
                if (s_devs[i].rssi_best < weakest->rssi_best) {
                    weakest = &s_devs[i];
                }
            }
            if (rssi <= weakest->rssi_best) {
                xSemaphoreGive(s_lock);
                return;
            }
            dev = weakest;
        }
        memset(dev, 0, sizeof(*dev));
        memcpy(dev->addr, addr->val, 6);
        dev->addr_type = addr->type;
        dev->rssi_best = rssi;
    }

    dev->rssi = rssi;
    if (rssi > dev->rssi_best) {
        dev->rssi_best = rssi;
    }
    dev->packets++;

    /* Name und Hersteller-ID kommen oft erst mit der Scan-Response, also in
     * einem späteren Paket als dem ersten — deshalb nur ergänzen, nie löschen. */
    const bool name_was_empty = (dev->name[0] == '\0');
    if (fields->name_len > 0 && fields->name) {
        size_t n = fields->name_len;
        if (n >= sizeof(dev->name)) {
            n = sizeof(dev->name) - 1;
        }
        memcpy(dev->name, fields->name, n);
        dev->name[n] = '\0';
    }
    bool mfg_changed = false;
    if (fields->mfg_data_len >= 2 && fields->mfg_data) {
        dev->company = (uint16_t)fields->mfg_data[0] | ((uint16_t)fields->mfg_data[1] << 8);
        dev->has_company = true;

        uint8_t len = fields->mfg_data_len;
        if (len > sizeof(dev->mfg)) {
            len = sizeof(dev->mfg);
        }
        /* Jede Änderung ist ein Hinweis: konstante Bytes sind eine Kennung,
         * wechselnde Bytes sind Nutzdaten. */
        if (len != dev->mfg_len || memcmp(dev->mfg, fields->mfg_data, len) != 0) {
            memcpy(dev->mfg, fields->mfg_data, len);
            dev->mfg_len = len;
            mfg_changed = true;
        }
    }

    for (uint8_t i = 0; i < fields->num_uuids16 && fields->uuids16; i++) {
        const uint16_t uuid = ble_uuid_u16(&fields->uuids16[i].u);
        bool known = false;
        for (uint8_t k = 0; k < dev->n_svc16; k++) {
            if (dev->svc16[k] == uuid) {
                known = true;
                break;
            }
        }
        if (!known && dev->n_svc16 < BLE_SCAN_SVC16_MAX) {
            dev->svc16[dev->n_svc16++] = uuid;
        }
    }
    if (!dev->has_svc128 && fields->num_uuids128 > 0 && fields->uuids128) {
        memcpy(dev->svc128, fields->uuids128[0].value, sizeof(dev->svc128));
        dev->has_svc128 = true;
    }

    dev->kind = classify(dev);
    s_generation++;

    /* Einmal ins Log, sobald der Name da ist — dann lässt sich ein Fund auch
     * ohne Blick auf den Schirm auswerten, und die Zeile ist kopierbar. */
    const bool announce = name_was_empty && dev->name[0] != '\0';
    ble_scan_dev_t copy;
    if (announce || mfg_changed) {
        copy = *dev;
    }

    xSemaphoreGive(s_lock);

    if (mfg_changed && copy.mfg_len > 0) {
        char hex[3 * sizeof(copy.mfg) + 1];
        int n = 0;
        for (uint8_t i = 0; i < copy.mfg_len; i++) {
            n += snprintf(hex + n, sizeof(hex) - n, "%02X ", copy.mfg[i]);
        }
        ESP_LOGI(TAG, "MFG %-16s %s",
                 copy.name[0] ? copy.name : "(kein Name)", hex);
    }

    if (announce) {
        char addr_buf[20];
        char svc_buf[64];
        char comp_buf[16];
        ble_scan_addr_text(&copy, addr_buf, sizeof(addr_buf));
        ble_scan_services_text(&copy, svc_buf, sizeof(svc_buf));
        if (copy.has_company) {
            snprintf(comp_buf, sizeof(comp_buf), "0x%04X", copy.company);
        } else {
            snprintf(comp_buf, sizeof(comp_buf), "—");
        }
        ESP_LOGI(TAG, "gefunden: \"%s\"  %s  %d dBm  Hersteller %s  Dienste %s  %s",
                 copy.name, addr_buf, copy.rssi, comp_buf,
                 svc_buf[0] ? svc_buf : "—",
                 ble_scan_kind_text(copy.kind));
    }
}

void ble_scan_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_dev_count = 0;
    s_generation++;
    xSemaphoreGive(s_lock);
}

esp_err_t ble_scan_set_mfg_cb(ble_scan_mfg_cb_t cb)
{
    s_mfg_cb = cb;
    return ESP_OK;
}

uint32_t ble_scan_generation(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint32_t gen = s_generation;
    xSemaphoreGive(s_lock);
    return gen;
}

size_t ble_scan_snapshot(ble_scan_dev_t *out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_dev_count < max ? s_dev_count : max;
    memcpy(out, s_devs, n * sizeof(ble_scan_dev_t));
    xSemaphoreGive(s_lock);

    /* Nach Feldstärke sortieren: das Nächstliegende oben. Bei höchstens zwei
     * Dutzend Einträgen ist Einfügesortieren schnell genug und kurz. */
    for (size_t i = 1; i < n; i++) {
        ble_scan_dev_t tmp = out[i];
        size_t j = i;
        while (j > 0 && out[j - 1].rssi < tmp.rssi) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = tmp;
    }
    return n;
}

/* -------------------------------------------------------------------------- */
/*  NimBLE                                                                    */
/* -------------------------------------------------------------------------- */

static int ble_scan_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields fields;
        memset(&fields, 0, sizeof(fields));
        /* Fehlerhafte Pakete kommen vor; dann bleiben die Felder eben leer und
         * der Eintrag lebt von Adresse und Feldstärke. */
        ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data);

        /* Vor der Geräteliste und außerhalb ihres Mutex: der Auswerter bekommt
         * jedes Paket, und er soll nicht auf die Liste warten müssen. */
        if (s_mfg_cb && fields.mfg_data && fields.mfg_data_len >= 2) {
            s_mfg_cb(fields.mfg_data, fields.mfg_data_len, event->disc.rssi);
        }

        device_seen(&event->disc.addr, event->disc.rssi, &fields);
        break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        ESP_LOGI(TAG, "Scan beendet (Grund %d)", event->disc_complete.reason);
        s_state = STATE_OFF;
        break;
    default:
        break;
    }
    return 0;
}

static void scan_begin(void)
{
    const struct ble_gap_disc_params params = {
        .itvl = BLE_SCAN_UNITS(BLE_SCAN_ITVL_MS),
        .window = BLE_SCAN_UNITS(BLE_SCAN_WIN_MS),
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        /* Aktiv: fragt die Scan-Response ab. Ohne die bleiben viele Geräte
         * namenlos — und der Name ist hier die gesuchte Information. */
        .passive = 0,
        /* Duplikate durchlassen, damit die Feldstärke laufend nachgeführt wird. */
        .filter_duplicates = 0,
    };

    const int rc = ble_gap_disc(ble_host_own_addr_type(), BLE_HS_FOREVER, &params,
                                ble_scan_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc fehlgeschlagen (rc=%d)", rc);
        s_state = STATE_OFF;
        return;
    }

    s_state = STATE_RUNNING;
    ESP_LOGI(TAG, "Scan läuft: aktiv, %d ms Fenster alle %d ms",
             BLE_SCAN_WIN_MS, BLE_SCAN_ITVL_MS);
}

/** Läuft im Host-Task, sobald der Controller bereit ist. */
static void ble_scan_on_host_ready(void)
{
    if (s_state == STATE_STARTING) {
        scan_begin();
    }
}

/* -------------------------------------------------------------------------- */
/*  Öffentliche API                                                           */
/* -------------------------------------------------------------------------- */

esp_err_t ble_scan_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "Mutex");
    }
    s_dev_count = 0;
    s_generation = 0;
    ESP_RETURN_ON_ERROR(ble_host_register(ble_scan_on_host_ready), TAG, "Rückruf eintragen");
    ESP_LOGI(TAG, "BLE-Scanner bereit — Stack startet erst bei Bedarf");
    return ESP_OK;
}

esp_err_t ble_scan_start(void)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "ble_scan_init() fehlt");

    if (s_state != STATE_OFF) {
        return ESP_OK;
    }

    s_state = STATE_STARTING;

    if (!ble_host_is_ready()) {
        /* Der Stack fährt gerade erst hoch; scan_begin() kommt aus dem Rückruf. */
        return ble_host_start();
    }

    scan_begin();
    return (s_state == STATE_RUNNING) ? ESP_OK : ESP_FAIL;
}

esp_err_t ble_scan_stop(void)
{
    if (s_state == STATE_OFF) {
        return ESP_OK;
    }
    const int rc = ble_gap_disc_cancel();
    s_state = STATE_OFF;
    /* BLE_HS_EALREADY heißt nur: es lief ohnehin schon nichts mehr. */
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "ble_gap_disc_cancel (rc=%d)", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Scan gestoppt");
    return ESP_OK;
}

bool ble_scan_is_running(void)
{
    return s_state == STATE_RUNNING;
}
