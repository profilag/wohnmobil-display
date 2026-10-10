#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "batt_history.h"
#include "ble_probe.h"
#include "victron_ble.h"
#include "waste_level.h"
#include "water_level.h"

static const char *TAG = "web";

/** Abstand zwischen zwei Verbindungsversuchen. */
#define WEB_RETRY_MS 30000

/** Fenster der Restlaufzeit — dasselbe wie auf dem Panel. */
#define WEB_RUN_WINDOW_MS (24u * 60 * 60 * 1000)

/*
 * Einstellungen liegen im NVS, nicht im Quelltext.
 *
 * Ein Fahrzeug wechselt den Hotspot — Telefon, Campingplatz, Zuhause. Wer
 * dafür neu übersetzen und flashen muss, hat kein Gerät, sondern einen
 * Prototyp. Deshalb Netzname, Passwort, Ein/Aus und Gerätename im Speicher,
 * änderbar über die Einstellungen am Gerät.
 */
#define WEB_NVS_NS "net"

static httpd_handle_t s_server;
static bool s_connected;
static bool s_enabled = true;
static bool s_started;
static char s_status[48] = "WLAN aus";
static char s_ssid[WEB_SSID_LEN] = WEB_SSID_DEFAULT;
static char s_pass[WEB_PASS_LEN] = WEB_PASSWORD_DEFAULT;
static char s_name[WEB_NAME_LEN] = WEB_NAME_DEFAULT;
static char s_ip[16];

/* -------------------------------------------------------------------------- */
/*  Seite                                                                     */
/* -------------------------------------------------------------------------- */

/*
 * Das Gerüst ist fest, die Zahlen holt die Seite sich selbst.
 *
 * Alternative wäre, bei jedem Aufruf fertiges HTML zu bauen — dann müsste das
 * Telefon aber die ganze Seite neu laden, um einen neuen Wert zu sehen, und
 * das Bild zuckt bei jedem Takt. So lädt es einmal und holt danach nur noch
 * ein paar hundert Byte JSON.
 *
 * Kein Nachladen von außen: keine Schriften, kein Stilblatt, kein Framework.
 * Das Fahrzeug steht oft genug dort, wo es kein Internet gibt, und eine Seite,
 * die ohne Netz nicht aussieht wie gedacht, hat ihren Zweck verfehlt.
 */
static const char PAGE[] =
    "<!DOCTYPE html><html lang=de><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Laterne</title><style>"
    "body{margin:0;padding:16px;background:#101418;color:#fff;"
    "font-family:system-ui,-apple-system,sans-serif}"
    "h1{font-size:18px;font-weight:600;color:#8b949e;margin:0 0 16px}"
    ".c{background:#1c2128;padding:14px 16px;margin-bottom:10px}"
    ".t{font-size:13px;color:#8b949e}"
    ".v{font-size:30px;margin-top:2px}"
    ".r{display:flex;justify-content:space-between;margin-top:8px;font-size:15px}"
    ".r span:first-child{color:#8b949e}"
    ".b{height:14px;background:#30363d;margin-top:8px;display:flex}"
    ".b i{display:block;height:100%}"
    "</style></head><body>"
    "<h1 id=hd>…</h1>"
    "<div class=c><div class=t>Solar</div><div class=v id=pv>–</div>"
    "<div class=b><i id=pvb style='width:0;background:#f5b301'></i></div></div>"
    "<div class=c><div class=t>Akku</div><div class=v id=soc>–</div>"
    "<div class=b><i id=socb style='width:0;background:#3fb950'></i></div>"
    "<div class=r><span>Leistung</span><span id=bw>–</span></div>"
    "<div class=r><span>Spannung</span><span id=bv>–</span></div>"
    "<div class=r><span id=rt>Restlaufzeit</span><span id=rv>–</span></div></div>"
    "<div class=c><div class=t>Tanks</div>"
    "<div class=r><span>Frischwasser</span><span id=fw>–</span></div>"
    "<div class=r><span>Abwasser</span><span id=ww>–</span></div></div>"
    "<script>"
    "async function u(){try{const d=await(await fetch('/data')).json();"
    "pv.textContent=d.pv+' W';pvb.style.width=Math.min(100,d.pv/2.2)+'%';"
    "soc.textContent=d.soc===null?'–':d.soc+' %';"
    "socb.style.width=(d.soc||0)+'%';"
    "socb.style.background=d.soc<=20?'#da3633':d.soc<=40?'#d29922':'#3fb950';"
    "bw.textContent=d.w===null?'–':(d.w>0?'+':'')+d.w+' W';"
    "bw.style.color=d.w>0?'#4ade80':d.w<0?'#d8b020':'#fff';"
    "bv.textContent=d.v===null?'–':d.v.toFixed(2)+' V';"
    "rt.textContent=d.rm===null?'Restlaufzeit':'Restlaufzeit ('+d.rc+')';"
    "rv.textContent=d.rm===null?'–':d.rm<90?d.rm+' Min':d.rm<2880?"
    "Math.round(d.rm/60)+' Std':Math.round(d.rm/1440)+' Tage';"
    "fw.textContent=d.fw;ww.textContent=d.ww;"
    "hd.textContent=d.n;document.title=d.n;"
    "}catch(e){}}"
    "u();setInterval(u,3000);"
    "</script></body></html>";

static esp_err_t page_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, PAGE, HTTPD_RESP_USE_STRLEN);
}

/* -------------------------------------------------------------------------- */
/*  Daten                                                                     */
/* -------------------------------------------------------------------------- */

/** Füllstand als Wort — dieselben Begriffe wie auf dem Panel. */
static const char *fresh_text(void)
{
    /* `water_level.h` kennt kein Alter; der Frischwassertank wird beim Start
     * und danach alle fünf Minuten gemessen, der letzte Wert ist also nie
     * älter als das. */
    return water_level_text(water_level_last()->level);
}

static esp_err_t data_get(httpd_req_t *req)
{
    victron_data_t v;
    victron_get(&v);

    ble_probe_battery_t b;
    ble_probe_battery(&b);

    const waste_measurement_t *w = waste_level_last();
    const bool waste_known = (waste_level_age_ms() >= 0);

    /* Leistung wie auf dem Panel: aus Spannung und Strom des BMS. */
    int32_t watt = 0;
    if (b.valid) {
        watt = (int32_t)(((int64_t)b.current_ma * (int64_t)b.voltage_mv) / 1000000);
    }

    uint32_t minutes = 0, covered = 0;
    const bool have_run =
        b.valid && batt_history_runtime(WEB_RUN_WINDOW_MS, b.remaining_mah, &minutes,
                                        &covered);

    char cov[16] = "";
    if (have_run) {
        if (covered < 2u * 60 * 60 * 1000) {
            snprintf(cov, sizeof(cov), "%u min", (unsigned)(covered / 60000));
        } else {
            snprintf(cov, sizeof(cov), "%u h", (unsigned)(covered / 3600000));
        }
    }

    char json[384];
    int n = snprintf(json, sizeof(json),
                     "{\"pv\":%u,", v.valid && v.has_power ? v.solar_power : 0);
    if (b.valid) {
        n += snprintf(json + n, sizeof(json) - n,
                      "\"soc\":%u,\"w\":%ld,\"v\":%u.%02u,", b.soc, (long)watt,
                      (unsigned)(b.voltage_mv / 1000),
                      (unsigned)((b.voltage_mv % 1000) / 10));
    } else {
        n += snprintf(json + n, sizeof(json) - n, "\"soc\":null,\"w\":null,\"v\":null,");
    }
    if (have_run) {
        n += snprintf(json + n, sizeof(json) - n, "\"rm\":%u,\"rc\":\"%s\",",
                      (unsigned)minutes, cov);
    } else {
        n += snprintf(json + n, sizeof(json) - n, "\"rm\":null,\"rc\":\"\",");
    }
    snprintf(json + n, sizeof(json) - n,
             "\"fw\":\"%s\",\"ww\":\"%s\",\"n\":\"%s\"}", fresh_text(),
             !waste_known ? "unbekannt" : (w->full ? "voll" : "nicht voll"), s_name);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static void server_start(void)
{
    if (s_server) {
        return;
    }
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    /* Zwei Handler, kein Verzeichnisbaum — die Vorgabe von acht Steckplätzen
     * wäre verschenkter Speicher. */
    cfg.max_uri_handlers = 4;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Webserver ließ sich nicht starten");
        s_server = NULL;
        return;
    }

    static const httpd_uri_t page = {
        .uri = "/", .method = HTTP_GET, .handler = page_get};
    static const httpd_uri_t data = {
        .uri = "/data", .method = HTTP_GET, .handler = data_get};
    httpd_register_uri_handler(s_server, &page);
    httpd_register_uri_handler(s_server, &data);
    ESP_LOGI(TAG, "Webserver läuft");
}

/* -------------------------------------------------------------------------- */
/*  WLAN                                                                      */
/* -------------------------------------------------------------------------- */

/**
 * Einmal die Umgebung aufschreiben.
 *
 * Wenn die Verbindung scheitert, ist die erste Frage immer dieselbe: Ist das
 * Netz überhaupt da? Der ESP32-S3 funkt ausschließlich auf 2,4 GHz — ein
 * Telefon-Hotspot, der auf 5 GHz steht, ist für ihn unsichtbar, und der
 * Fehlercode dafür sieht genauso aus wie "zu weit weg". Diese Liste
 * beantwortet die Frage in einem Blick.
 *
 * Läuft nur beim Start: Ein Suchlauf belegt den Sender für ein paar Sekunden,
 * und den braucht Bluetooth.
 */
static void scan_once(void)
{
    static bool done;
    if (done) {
        return;
    }
    done = true;

    const wifi_scan_config_t cfg = {.show_hidden = true};
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
        ESP_LOGW(TAG, "Suchlauf ließ sich nicht starten");
        return;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count == 0) {
        ESP_LOGW(TAG, "kein einziges Netz in Reichweite");
        return;
    }
    if (count > 16) {
        count = 16;
    }

    wifi_ap_record_t *ap = calloc(count, sizeof(wifi_ap_record_t));
    if (!ap) {
        return;
    }
    if (esp_wifi_scan_get_ap_records(&count, ap) == ESP_OK) {
        ESP_LOGI(TAG, "%u Netze in Reichweite (nur 2,4 GHz sichtbar):", count);
        for (uint16_t i = 0; i < count; i++) {
            ESP_LOGI(TAG, "  \"%s\"  Kanal %u  %d dBm  Sicherheit %d%s",
                     (const char *)ap[i].ssid, ap[i].primary, ap[i].rssi,
                     (int)ap[i].authmode,
                     strcmp((const char *)ap[i].ssid, s_ssid) == 0 ? "   <-- gesucht"
                                                                    : "");
        }
    }
    free(ap);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        scan_once();
        esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_connected = false;
        s_ip[0] = '\0';
        /* Den Grund mitschreiben: "Netz nicht gefunden" und "Passwort falsch"
         * sehen von außen gleich aus, führen aber zu ganz verschiedenen
         * nächsten Schritten. */
        snprintf(s_status, sizeof(s_status), "nicht verbunden (Grund %u)", d->reason);
        ESP_LOGW(TAG, "Verbindung zu \"%s\" verloren oder abgelehnt, Grund %u",
                 s_ssid, d->reason);
        /* Weiter versuchen, aber mit Abstand. Das Netz ist ein
         * Fahrzeug-Hotspot — mal an, mal aus —, und dann soll sich das Board
         * von allein wieder einfinden. Ohne Pause liefe der Sender dauernd auf
         * Suche und nähme Bluetooth die Zeit weg. */
        if (!s_enabled) {
            strlcpy(s_status, "aus", sizeof(s_status));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(WEB_RETRY_MS));
        esp_wifi_connect();
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *e = data;
        s_connected = true;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        strlcpy(s_status, s_ip, sizeof(s_status));
        ESP_LOGI(TAG, "verbunden, Adresse " IPSTR, IP2STR(&e->ip_info.ip));
        server_start();
    }
}

/** Gespeicherte Einstellungen holen; fehlende bleiben auf der Voreinstellung. */
static void settings_load(void)
{
    nvs_handle_t h;
    if (nvs_open(WEB_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_ssid);
    nvs_get_str(h, "ssid", s_ssid, &len);
    len = sizeof(s_pass);
    nvs_get_str(h, "pass", s_pass, &len);
    len = sizeof(s_name);
    nvs_get_str(h, "name", s_name, &len);

    uint8_t on = 1;
    if (nvs_get_u8(h, "on", &on) == ESP_OK) {
        s_enabled = (on != 0);
    }
    nvs_close(h);
}

/** Einen Wert ablegen. Fehler werden gemeldet, nicht verschluckt. */
static esp_err_t settings_store_str(const char *key, const char *value)
{
    nvs_handle_t h;
    esp_err_t ret = nvs_open(WEB_NVS_NS, NVS_READWRITE, &h);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = nvs_set_str(h, key, value);
    if (ret == ESP_OK) {
        ret = nvs_commit(h);
    }
    nvs_close(h);
    return ret;
}

esp_err_t web_start(void)
{
    settings_load();

    if (!s_enabled) {
        strlcpy(s_status, "aus", sizeof(s_status));
        ESP_LOGI(TAG, "WLAN bleibt aus (so gespeichert)");
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    /* Die Ereignisschleife kann schon stehen — dann ist das kein Fehler. */
    const esp_err_t loop = esp_event_loop_create_default();
    if (loop != ESP_OK && loop != ESP_ERR_INVALID_STATE) {
        return loop;
    }
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "wifi_init");

    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL),
                        TAG, "wifi-Ereignisse");
    ESP_RETURN_ON_ERROR(esp_event_handler_instance_register(
                            IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL),
                        TAG, "ip-Ereignisse");

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password));
    /*
     * Keine Mindestanforderung an die Verschlüsselung.
     *
     * Erst stand hier WPA2. Der Treiber filtert damit beim Verbinden alle
     * Netze heraus, die weniger bieten — und meldet dann "kein Netz gefunden",
     * obwohl der Suchlauf es sieht. Genau dieses Bild hatten wir: "laterne"
     * in der Liste, trotzdem Grund 201. Welchen Modus der Hotspot wirklich
     * fährt, steht jetzt im Suchlauf-Protokoll.
     */
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Modus");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "Konfiguration");
    /* Stromsparen aus: Mit Modem-Sleep antwortet die Seite träge, und die
     * Koexistenz mit Bluetooth wird unberechenbarer. */
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "Energiesparen");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Start");

    s_started = true;
    snprintf(s_status, sizeof(s_status), "suche \"%s\" …", s_ssid);
    ESP_LOGI(TAG, "WLAN gestartet, suche \"%s\"", s_ssid);
    return ESP_OK;
}

bool web_is_connected(void)
{
    return s_connected;
}

bool web_is_enabled(void)
{
    return s_enabled;
}

const char *web_ssid(void)
{
    return s_ssid;
}

const char *web_ip(void)
{
    return s_ip;
}

const char *web_device_name(void)
{
    return s_name;
}

int8_t web_rssi(void)
{
    wifi_ap_record_t ap;
    if (!s_connected || esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return 0;
    }
    return ap.rssi;
}

esp_err_t web_set_device_name(const char *name)
{
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_name, name, sizeof(s_name));
    return settings_store_str("name", s_name);
}

/** Zugangsdaten in den Treiber schieben und neu verbinden. */
static esp_err_t apply_credentials(void)
{
    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, s_ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_pass, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "Konfiguration");
    esp_wifi_disconnect();
    snprintf(s_status, sizeof(s_status), "suche \"%s\" …", s_ssid);
    return esp_wifi_connect();
}

esp_err_t web_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || !ssid[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    strlcpy(s_ssid, ssid, sizeof(s_ssid));
    strlcpy(s_pass, password ? password : "", sizeof(s_pass));

    ESP_RETURN_ON_ERROR(settings_store_str("ssid", s_ssid), TAG, "ssid sichern");
    ESP_RETURN_ON_ERROR(settings_store_str("pass", s_pass), TAG, "Passwort sichern");

    ESP_LOGI(TAG, "neues Netz: \"%s\"", s_ssid);
    return s_started ? apply_credentials() : web_start();
}

esp_err_t web_set_enabled(bool on)
{
    nvs_handle_t h;
    if (nvs_open(WEB_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "on", on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }

    if (on == s_enabled) {
        return ESP_OK;
    }
    s_enabled = on;

    if (!on) {
        /*
         * Nur trennen und den Sender stilllegen, nicht abbauen.
         * `esp_wifi_deinit()` gäbe zwar den Speicher zurück, aber ein erneutes
         * Hochfahren müsste dann die ganze Kette aus netif, Ereignissen und
         * Treiber wiederholen — mehr Gelegenheiten für Fehler als der Nutzen
         * der paar Kilobyte wert ist.
         */
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_connected = false;
        s_ip[0] = '\0';
        strlcpy(s_status, "aus", sizeof(s_status));
        ESP_LOGI(TAG, "WLAN abgeschaltet");
        return ESP_OK;
    }

    if (!s_started) {
        return web_start();
    }
    ESP_LOGI(TAG, "WLAN wieder eingeschaltet");
    snprintf(s_status, sizeof(s_status), "suche \"%s\" …", s_ssid);
    return esp_wifi_start();
}

size_t web_scan(char (*ssid)[WEB_SSID_LEN], int8_t *rssi, size_t max)
{
    if (!s_started || !s_enabled || max == 0) {
        return 0;
    }

    const wifi_scan_config_t cfg = {.show_hidden = false};
    if (esp_wifi_scan_start(&cfg, true) != ESP_OK) {
        return 0;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return 0;
    }
    if (found > max) {
        found = (uint16_t)max;
    }

    wifi_ap_record_t *ap = calloc(found, sizeof(wifi_ap_record_t));
    if (!ap) {
        return 0;
    }

    size_t out = 0;
    if (esp_wifi_scan_get_ap_records(&found, ap) == ESP_OK) {
        /* Der Treiber liefert bereits nach Pegel sortiert, stärkstes zuerst. */
        for (uint16_t i = 0; i < found; i++) {
            if (ap[i].ssid[0] == '\0') {
                continue;
            }
            strlcpy(ssid[out], (const char *)ap[i].ssid, WEB_SSID_LEN);
            rssi[out] = ap[i].rssi;
            out++;
        }
    }
    free(ap);
    return out;
}

const char *web_status_text(void)
{
    return s_status;
}
