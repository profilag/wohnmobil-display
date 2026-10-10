#include "ble_probe.h"

#include <stdio.h>
#include <string.h>

#include "ble_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"

static const char *TAG = "probe";

/** Platz für den Mitschnitt. Volles Puffer verdrängt die ältesten Zeilen. */
#define PROBE_LOG_SIZE 3072

static SemaphoreHandle_t s_lock;
static volatile ble_probe_state_t s_state = BLE_PROBE_IDLE;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;

/** Gesuchte Adresse in Anzeigereihenfolge, also MSB zuerst. */
static uint8_t s_want[6];
/** Dieselbe Adresse als Text, für den Wiederverbindungsversuch. */
static char s_mac[20];
/** true zwischen ble_probe_connect() und ble_probe_disconnect(). */
static bool s_want_connected;
static int64_t s_reconnect_at_us;

/** Abstand zwischen zwei Wiederverbindungsversuchen. */
#define PROBE_RECONNECT_MS 15000

/**
 * Die Abfragefolge nach `patman15/aiobmsble`, Datei `aiobmsble/bms/humsienk_bms.py`.
 *
 * **`0x00` zuerst ist zwingend.** Der Referenztreiber sendet ihn als
 * Initialisierung, bevor er irgendetwas abfragt (`_cmd(b"\x00")  # init`), und
 * ohne ihn beantwortet das BMS die Statusbefehle nicht — es quittiert sie auf
 * ATT-Ebene und ignoriert sie darüber. Genau dieser Schritt fehlte hier, und
 * genau deshalb kam auf `0x20/0x21/0x22` nie eine Antwort.
 *
 * `0x11` und `0xF5` liefern Modell und Firmwarestand. Sie sind für die
 * Messwerte entbehrlich, kosten aber nichts und sagen uns, welche
 * HumsiENK-Variante hier verbaut ist.
 */
static const char *const s_queries[] = {
    "AA 00 00 00 00", /* Init           */
    "AA F5 00 F5 00", /* Version        */
    "AA 20 00 20 00", /* Status         */
    "AA 22 00 22 00", /* Zellspannungen */
    "AA 21 00 21 00", /* Batteriedaten  */
    "AA 23 00 23 00", /* Stromdetails   */
};
#define QUERY_COUNT (sizeof(s_queries) / sizeof(s_queries[0]))
/**
 * Höchstwartezeit auf eine Antwort. Die ESP32-Referenz gibt einer Abfrage
 * 2,5 s — danach richtet sich dieser Wert.
 */
#define QUERY_GAP_MS 2500
/** Nach einer eingetroffenen Antwort geht es schneller weiter. */
#define QUERY_NEXT_MS 250
/** Abstand zweier vollständiger Abfrageläufe im Betrieb. */
#define POLL_INTERVAL_MS 30000

static int64_t s_poll_at_us;

/**
 * Zweiter Anlauf mit umgekehrtem Schreibmodus.
 *
 * `aiobmsble` probiert in `_await_msg` beide Modi durch — erst den, der zu den
 * Properties passt, dann den anderen. Dieser Rückfall steht dort nicht ohne
 * Grund: Manche Baureihen verarbeiten nur Write Command, obwohl sie nur
 * Write Request bewerben. Bleibt der erste Durchgang stumm, wird der zweite
 * mit dem anderen Modus gefahren.
 */
static bool s_write_inverted;

/** `== QUERY_COUNT` heißt: die Folge läuft gerade nicht. */
static size_t s_query_idx = QUERY_COUNT;
static int64_t s_query_next_us;
/** Gedrückt, während noch verbunden wurde — wird nachgeholt. */
static bool s_queries_pending;

static struct {
    uint16_t start, end;
    char uuid[BLE_PROBE_UUID_LEN];
} s_svcs[BLE_PROBE_MAX_SVCS];
static uint8_t s_svc_count;
static uint8_t s_svc_idx;

static ble_probe_entry_t s_entries[BLE_PROBE_MAX_ENTRIES];
static size_t s_entry_count;
static size_t s_sub_idx;
static size_t s_read_idx;
/** Erstes Merkmal mit Schreibrecht — dorthin gehen die Anfragen. */
static uint16_t s_write_handle;
static uint8_t s_write_props;
static uint32_t s_generation;

static char s_log[PROBE_LOG_SIZE];
static size_t s_log_len;
static uint32_t s_log_generation;
static uint32_t s_notify_count;

/** Dekodierte Messwerte und ihr Alter. */
static ble_probe_battery_t s_batt;
static int64_t s_batt_us = -1;

/* -------------------------------------------------------------------------- */
/*  Hilfsmittel                                                               */
/* -------------------------------------------------------------------------- */

static void set_state(ble_probe_state_t state)
{
    s_state = state;
}

const char *ble_probe_state_text(ble_probe_state_t state)
{
    switch (state) {
    case BLE_PROBE_IDLE:         return "nicht verbunden";
    case BLE_PROBE_SEARCHING:    return "sucht Gerät …";
    case BLE_PROBE_CONNECTING:   return "verbindet …";
    case BLE_PROBE_DISCOVERING:  return "liest Dienste …";
    case BLE_PROBE_SUBSCRIBING:  return "meldet sich an …";
    case BLE_PROBE_READY:        return "verbunden";
    case BLE_PROBE_FAILED:       return "fehlgeschlagen";
    default:                     return "?";
    }
}

ble_probe_state_t ble_probe_get_state(void)
{
    return s_state;
}

void ble_probe_props_text(uint8_t props, char *buf, size_t len)
{
    int n = 0;
    buf[0] = '\0';
    if (props & BLE_GATT_CHR_PROP_READ) {
        n += snprintf(buf + n, len - n, "R ");
    }
    if (props & BLE_GATT_CHR_PROP_WRITE) {
        n += snprintf(buf + n, len - n, "W ");
    }
    if (props & BLE_GATT_CHR_PROP_WRITE_NO_RSP) {
        n += snprintf(buf + n, len - n, "Wn ");
    }
    if (props & BLE_GATT_CHR_PROP_NOTIFY) {
        n += snprintf(buf + n, len - n, "N ");
    }
    if (props & BLE_GATT_CHR_PROP_INDICATE) {
        n += snprintf(buf + n, len - n, "I ");
    }
    if (n == 0) {
        snprintf(buf, len, "—");
    }
}

/** Eine Zeile anhängen; ist der Puffer voll, fallen vorne ganze Zeilen weg. */
static void log_line(const char *line)
{
    const size_t need = strlen(line) + 1; /* mit Zeilenumbruch */
    if (need >= PROBE_LOG_SIZE) {
        return;
    }

    while (s_log_len + need >= PROBE_LOG_SIZE) {
        char *nl = memchr(s_log, '\n', s_log_len);
        if (!nl) {
            s_log_len = 0;
            break;
        }
        const size_t drop = (size_t)(nl - s_log) + 1;
        memmove(s_log, s_log + drop, s_log_len - drop);
        s_log_len -= drop;
    }

    memcpy(s_log + s_log_len, line, need - 1);
    s_log_len += need - 1;
    s_log[s_log_len++] = '\n';
    s_log_generation++;
}

uint32_t ble_probe_notify_count(void)
{
    return s_notify_count;
}

uint32_t ble_probe_log_generation(void)
{
    return s_log_generation;
}

size_t ble_probe_log_text(char *buf, size_t len)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = s_log_len < (len - 1) ? s_log_len : (len - 1);
    memcpy(buf, s_log, n);
    buf[n] = '\0';
    xSemaphoreGive(s_lock);
    return n;
}

void ble_probe_log_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_log_len = 0;
    s_notify_count = 0;
    s_log_generation++;
    xSemaphoreGive(s_lock);
}

size_t ble_probe_snapshot(ble_probe_entry_t *out, size_t max)
{
    if (!out || max == 0) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t n = s_entry_count < max ? s_entry_count : max;
    memcpy(out, s_entries, n * sizeof(ble_probe_entry_t));
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t ble_probe_generation(void)
{
    return s_generation;
}

/**
 * In der App abgelesene Werte, nach denen jede Antwort durchsucht wird.
 *
 * Abgelesen am 2026-08-09 bei vollem Akku: 100 %, 13,5 V, 6 A, 22 °C. Für jeden
 * Messwert stehen hier die gängigen Skalierungen — welche stimmt, verrät der
 * Treffer. Die Kelvin-Variante ist die von JBD benutzte Darstellung; sie kommt
 * bei chinesischen BMS oft vor und kostet nichts, sie mitzusuchen.
 */
static const ble_probe_hint_t s_hints[] = {
    {"Ladung 100 %",     "%",        100},
    {"Spannung 13,5 V",  "0,1 V",    135},
    {"Spannung 13,5 V",  "0,01 V",   1350},
    {"Spannung 13,5 V",  "mV",       13500},
    {"Strom 6 A",        "0,1 A",    60},
    {"Strom 6 A",        "0,01 A",   600},
    {"Strom 6 A",        "mA",       6000},
    {"Temperatur 22 °C", "°C",       22},
    {"Temperatur 22 °C", "0,1 °C",   220},
    {"Temperatur 22 °C", "0,1 K",    2952}, /* (22 + 273,15) × 10 */
};

/** Nicht mehr Treffer je Antwort — ein Byte wie 0x64 kommt schnell mehrfach vor. */
#define PROBE_MAX_HITS 12

/**
 * Rohbytes nach den bekannten Werten absuchen und Treffer protokollieren.
 *
 * Einzelne Bytes treffen naturgemäß oft zufällig; erst ein Treffer, der über
 * mehrere Antworten hinweg an derselben Stelle steht und sich mit dem Messwert
 * ändert, ist ein echtes Feld. Deshalb wird der Versatz mitprotokolliert.
 *
 * @note Ruft der Aufrufer mit gehaltenem Mutex auf.
 */
static void analyze_payload(const uint8_t *data, size_t len)
{
    int hits = 0;

    for (size_t h = 0; h < sizeof(s_hints) / sizeof(s_hints[0]) && hits < PROBE_MAX_HITS; h++) {
        const ble_probe_hint_t *hint = &s_hints[h];
        char line[128];

        if (hint->value <= 0xFF) {
            for (size_t i = 0; i < len && hits < PROBE_MAX_HITS; i++) {
                if (data[i] == hint->value) {
                    snprintf(line, sizeof(line), "   @%u  %02X        %s  [%s]",
                             (unsigned)i, data[i], hint->name, hint->unit);
                    log_line(line);
                    hits++;
                }
            }
        }
        if (hint->value <= 0xFFFF && len >= 2) {
            for (size_t i = 0; i + 1 < len && hits < PROBE_MAX_HITS; i++) {
                const uint32_t be = ((uint32_t)data[i] << 8) | data[i + 1];
                const uint32_t le = (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8);
                if (be == hint->value) {
                    snprintf(line, sizeof(line), "   @%u  %02X %02X  BE  %s  [%s]",
                             (unsigned)i, data[i], data[i + 1], hint->name, hint->unit);
                    log_line(line);
                    hits++;
                }
                if (le == hint->value && le != be) {
                    snprintf(line, sizeof(line), "   @%u  %02X %02X  LE  %s  [%s]",
                             (unsigned)i, data[i], data[i + 1], hint->name, hint->unit);
                    log_line(line);
                    hits++;
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/*  Rahmen zusammensetzen                                                     */
/* -------------------------------------------------------------------------- */

/*
 * Eine BLE-Benachrichtigung ist **nicht** dasselbe wie ein BMS-Telegramm. Bei
 * der voreingestellten MTU passen nur 20 Nutzbytes in ein Paket; ein längeres
 * Telegramm kommt zerstückelt, und umgekehrt können zwei kurze Telegramme in
 * einem Paket stecken. Deshalb ein eigener Puffer, der auf 0xAA aufsetzt und
 * anhand von LEN abzählt.
 */
#define PROBE_RX_BUF 512

static uint8_t s_rx[PROBE_RX_BUF];
static size_t s_rx_len;

static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/** Markiert Werte, die außerhalb des Erwartbaren liegen. */
static const char *plausible(bool ok)
{
    return ok ? "" : "   << unplausibel";
}

/**
 * Antworten nach den Feldangaben von `aiobmsble` auswerten.
 *
 * Die Versätze sind **nutzdatenrelativ** — der Python-Treiber zählt sie ab
 * Rahmenanfang und liegt deshalb durchweg um drei höher (`AA CMD LEN`).
 *
 * Bewusst nur ins Log und mit Plausibilitätsvermerk: Solange kein echtes
 * Telegramm dieses Akkus vorliegt, sind auch belegte Offsets erst eine
 * Erwartung. Ein Wert wie 3276 V oder 65000 A entlarvt Endianness oder
 * Datentyp sofort — deshalb steht der Rohwert immer daneben.
 *
 * @note Mutex ist gehalten.
 */
static void decode_frame(uint8_t cmd, const uint8_t *d, uint8_t len)
{
    char line[200];

    if (cmd == 0x21 && len >= 26) {
        const uint32_t mv = rd_u32le(&d[0]);
        const int32_t ma = (int32_t)rd_u32le(&d[4]);
        const uint32_t remain = rd_u32le(&d[10]);
        const uint32_t design = rd_u32le(&d[14]);

        s_batt.voltage_mv = mv;
        s_batt.current_ma = ma;
        s_batt.soc = d[8];
        s_batt.soh = d[9];
        s_batt.remaining_mah = remain;
        s_batt.design_mah = design;
        s_batt.cycles = rd_u16le(&d[18]);
        for (int k = 0; k < 6; k++) {
            s_batt.temp[k] = (int8_t)d[20 + k];
        }
        s_batt.valid = true;
        s_batt_us = esp_timer_get_time();

        snprintf(line, sizeof(line), "    Spannung  %lu mV = %lu,%02lu V%s",
                 (unsigned long)mv, (unsigned long)(mv / 1000),
                 (unsigned long)((mv % 1000) / 10),
                 plausible(mv >= 10000 && mv <= 15000));
        log_line(line);
        ESP_LOGI(TAG, "%s", line);

        snprintf(line, sizeof(line), "    Strom     %ld mA = %ld,%02ld A  (%s)",
                 (long)ma, (long)(ma / 1000), (long)labs((ma % 1000) / 10),
                 ma >= 0 ? "Laden" : "Entladen");
        log_line(line);
        ESP_LOGI(TAG, "%s", line);

        snprintf(line, sizeof(line), "    SOC %u %%   SOH %u %%%s",
                 d[8], d[9], plausible(d[8] <= 100 && d[9] <= 100));
        log_line(line);
        ESP_LOGI(TAG, "%s", line);

        snprintf(line, sizeof(line), "    Restkapazität %lu mAh   Nennkapazität %lu mAh%s",
                 (unsigned long)remain, (unsigned long)design,
                 plausible(design > 250000 && design < 400000));
        log_line(line);
        ESP_LOGI(TAG, "%s", line);

        snprintf(line, sizeof(line), "    Zyklen %u   Temperaturen %d %d %d %d %d %d °C",
                 rd_u16le(&d[18]), (int8_t)d[20], (int8_t)d[21], (int8_t)d[22],
                 (int8_t)d[23], (int8_t)d[24], (int8_t)d[25]);
        log_line(line);
        ESP_LOGI(TAG, "%s", line);
        return;
    }

    if (cmd == 0x22 && len >= 8) {
        /* Nur die belegten Zellen übernehmen: Das Protokoll überträgt immer
         * 24 Plätze, ein 4S-Akku füllt davon vier und lässt den Rest auf null. */
        s_batt.cell_count = 0;
        for (uint8_t k = 0; k < BLE_PROBE_CELL_MAX && (k * 2 + 1) < len; k++) {
            const uint16_t mv = rd_u16le(&d[k * 2]);
            if (mv == 0) {
                break;
            }
            s_batt.cell_mv[k] = mv;
            s_batt.cell_count = k + 1;
        }
        s_batt.valid = true;
        s_batt_us = esp_timer_get_time();
        /* 4S — mehr Zellen liefert der Akku nicht, auch wenn das Protokoll
         * bis zu 24 vorsieht. */
        uint16_t lo = 0xFFFF, hi = 0;
        int n = snprintf(line, sizeof(line), "    Zellen:");
        for (int i = 0; i < 4; i++) {
            const uint16_t cell = rd_u16le(&d[i * 2]);
            n += snprintf(line + n, sizeof(line) - n, " %u", cell);
            if (cell < lo) lo = cell;
            if (cell > hi) hi = cell;
        }
        snprintf(line + n, sizeof(line) - n, " mV%s",
                 plausible(lo >= 2500 && hi <= 3700));
        log_line(line);
        ESP_LOGI(TAG, "%s", line);

        snprintf(line, sizeof(line), "    min %u  max %u  Differenz %u mV", lo, hi,
                 (unsigned)(hi - lo));
        log_line(line);
        ESP_LOGI(TAG, "%s", line);
        return;
    }

    if (cmd == 0x20 && len >= 8) {
        const uint32_t status = rd_u32le(&d[4]);
        s_batt.status = status;
        /* Die drei Zustände liegen im obersten Bit dreier aufeinanderfolgender
         * Bytes des Statusworts — im Mitschnitt: 0x00800008, Entladen an. */
        s_batt.charge_mos = (d[4] & 0x80) != 0;
        s_batt.balancing = (d[5] & 0x80) != 0;
        s_batt.discharge_mos = (d[6] & 0x80) != 0;
        s_batt.valid = true;
        s_batt_us = esp_timer_get_time();
        snprintf(line, sizeof(line),
                 "    Laufzeit %u d %u h %u min   Status 0x%08lX",
                 rd_u16le(&d[0]), d[2], d[3], (unsigned long)status);
        log_line(line);
        ESP_LOGI(TAG, "%s", line);

        /* Die Flags liegen laut Referenz in den Bytes 4, 5 und 6 des
         * Statusworts — jeweils das oberste Bit. */
        snprintf(line, sizeof(line), "    Lade-MOSFET %s   Entlade-MOSFET %s   Balancer %s",
                 (d[4] & 0x80) ? "an" : "aus",
                 (d[6] & 0x80) ? "an" : "aus",
                 (d[5] & 0x80) ? "aktiv" : "aus");
        log_line(line);
        ESP_LOGI(TAG, "%s", line);
        return;
    }

    if (cmd == 0xF5 && len >= 3) {
        size_t k = 0;
        while (k < len && k < sizeof(s_batt.version) - 1 && d[k] >= 0x20 && d[k] < 0x7F) {
            s_batt.version[k] = (char)d[k];
            k++;
        }
        s_batt.version[k] = '\0';
        ESP_LOGI(TAG, "    Firmware %s", s_batt.version);
        return;
    }

    if (cmd == 0x00) {
        log_line("    (Initialisierung bestätigt)");
        ESP_LOGI(TAG, "    Initialisierung bestätigt");
    }
}

/** Ein zusammengesetztes Telegramm protokollieren. Mutex ist gehalten. */
static void log_frame(const uint8_t *f, size_t total)
{
    const uint8_t cmd = f[1];
    const uint8_t plen = f[2];

    /* Summe über CMD, LEN und Nutzdaten — so steht es in der Hypothese. */
    uint16_t calc = 0;
    for (size_t i = 1; i < (size_t)(3 + plen); i++) {
        calc = (uint16_t)(calc + f[i]);
    }
    const uint16_t rx_ck = (uint16_t)f[3 + plen] | ((uint16_t)f[4 + plen] << 8);
    const bool ok = (calc == rx_ck);

    char line[220];
    int n = snprintf(line, sizeof(line), "  RAHMEN CMD 0x%02X  LEN %u  CK rx %04X "
                                         "calc %04X  %s",
                     cmd, plen, rx_ck, calc, ok ? "OK" : "FEHLER");
    log_line(line);
    ESP_LOGI(TAG, "%s", line);

    n = snprintf(line, sizeof(line), "  NUTZDATEN:");
    for (uint8_t i = 0; i < plen && n < (int)sizeof(line) - 5; i++) {
        n += snprintf(line + n, sizeof(line) - n, " %02X", f[3 + i]);
    }
    if (plen == 0) {
        snprintf(line + n, sizeof(line) - n, " (keine)");
    }
    log_line(line);
    ESP_LOGI(TAG, "%s", line);

    decode_frame(cmd, &f[3], plen);

    /* Zusätzlich gegen die in der App abgelesenen Werte prüfen — falls die
     * Referenz-Offsets für diese Baureihe doch abweichen, zeigt die Feldsuche,
     * wo die Zahlen wirklich stehen. */
    analyze_payload(&f[3], plen);

    /* Antwort da — die nächste Abfrage muss nicht die volle Wartezeit
     * abwarten. Der Referenztreiber wartet ebenso auf die Antwort. */
    if (s_query_idx < QUERY_COUNT) {
        s_query_next_us = esp_timer_get_time() + (int64_t)QUERY_NEXT_MS * 1000;
    }
}

/**
 * Empfangene Bytes einspeisen und alle vollständigen Telegramme herauslösen.
 *
 * Bewusst wegwerfend beim Aufsetzen: Alles vor dem ersten 0xAA ist entweder
 * Rest eines verpassten Telegramms oder gehört zu einem anderen Rahmenformat.
 * Beides wird gemeldet statt still geschluckt — bei einer unbestätigten
 * Hypothese ist gerade das der interessante Befund.
 *
 * @note Mutex ist gehalten.
 */
static void rx_feed(const uint8_t *data, size_t len)
{
    if (s_rx_len + len > sizeof(s_rx)) {
        /* Kann nur passieren, wenn nie ein gültiger Rahmen erkannt wird. */
        ESP_LOGW(TAG, "Empfangspuffer voll, wird verworfen");
        s_rx_len = 0;
    }
    memcpy(s_rx + s_rx_len, data, len);
    s_rx_len += len;

    for (;;) {
        /* Auf den Rahmenanfang aufsetzen. */
        size_t start = 0;
        while (start < s_rx_len && s_rx[start] != BLE_PROBE_FRAME_HEADER) {
            start++;
        }
        if (start > 0) {
            char line[64];
            snprintf(line, sizeof(line), "  (%u Byte vor 0xAA übersprungen)",
                     (unsigned)start);
            log_line(line);
            memmove(s_rx, s_rx + start, s_rx_len - start);
            s_rx_len -= start;
        }
        if (s_rx_len < 3) {
            return; /* CMD und LEN fehlen noch */
        }

        const size_t total = 5u + s_rx[2]; /* AA CMD LEN … CK_LO CK_HI */
        if (s_rx_len < total) {
            return; /* Rest folgt in einem weiteren Paket */
        }

        log_frame(s_rx, total);

        memmove(s_rx, s_rx + total, s_rx_len - total);
        s_rx_len -= total;
        /* Weiter — es können mehrere Telegramme im Puffer liegen. */
    }
}

/** Höchstens so viele Bytes einer Antwort werden ausgewertet. */
#define PROBE_PAYLOAD_MAX 64

/**
 * Eine empfangene Antwort protokollieren und nach bekannten Werten absuchen.
 *
 * @param kind "N" für Benachrichtigung, "R" für gelesen
 */
static void handle_payload(const char *kind, const char *what, uint16_t handle,
                           const struct os_mbuf *om)
{
    uint8_t data[PROBE_PAYLOAD_MAX];
    uint16_t total = OS_MBUF_PKTLEN(om);
    if (total > sizeof(data)) {
        total = sizeof(data);
    }
    if (total > 0 && os_mbuf_copydata(om, 0, total, data) != 0) {
        total = 0;
    }

    char line[220];
    int n = snprintf(line, sizeof(line), "[%lu ms] %s %s@%u  len=%u:",
                     (unsigned long)(esp_timer_get_time() / 1000), kind, what,
                     handle, total);
    for (uint16_t i = 0; i < total && n < (int)sizeof(line) - 4; i++) {
        n += snprintf(line + n, sizeof(line) - n, " %02X", data[i]);
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    log_line(line);
    /* Erst die Rohbytes wie empfangen, dann der Versuch, sie als Rahmen zu
     * lesen. Beides steht im Mitschnitt — verworfen wird nichts. */
    if (kind[0] == 'N') {
        rx_feed(data, total);
    } else {
        analyze_payload(data, total);
    }
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s", line);
}


/* -------------------------------------------------------------------------- */
/*  Vollständige Attributauflistung                                           */
/* -------------------------------------------------------------------------- */

/*
 * Die reguläre Erkundung fragt nach **primären** Diensten. Das ist der übliche
 * Weg und übersieht zweierlei: als sekundär deklarierte Dienste (`0x2801`) und
 * alles, was hinter dem letzten primären Dienst liegt. Bei diesem Akku ist
 * genau das der Verdacht — er bewirbt `0xFF00`, die Dienstsuche findet ihn
 * nicht.
 *
 * `Find Information` über den gesamten Handle-Bereich kennt diese Lücke nicht:
 * Es listet jedes Attribut mit Handle und Typ. Die Deklarationen werden danach
 * gelesen, um die dahinterliegenden UUIDs aufzulösen.
 */

#define PROBE_MAX_ATTRS 96

static struct {
    uint16_t handle;
    uint16_t type16; /*!< 0, wenn der Typ eine 128-Bit-UUID ist */
    char type[BLE_PROBE_UUID_LEN];
} s_attrs[PROBE_MAX_ATTRS];
static size_t s_attr_count;
static size_t s_decl_idx;
/** true = einmalige Erkundung, danach von allein trennen. */
static bool s_survey_mode;

static void decl_read_next(void);

/** Eine Dienst- oder Merkmalsdeklaration im Klartext ausgeben. */
static void log_declaration(uint16_t handle, uint16_t type16, const uint8_t *v, uint16_t len)
{
    char uuid[BLE_UUID_STR_LEN];
    ble_uuid_any_t any;

    if (type16 == 0x2800 || type16 == 0x2801) {
        const char *kind = (type16 == 0x2800) ? "Dienst (primär)  " : "Dienst (SEKUNDÄR)";
        if (len == 2) {
            ble_uuid_init_from_buf(&any, v, 2);
        } else if (len == 16) {
            ble_uuid_init_from_buf(&any, v, 16);
        } else {
            ESP_LOGI(TAG, "  @%3u  %s  (%u Byte: %s)", handle, kind, len, "unerwartet");
            return;
        }
        ble_uuid_to_str(&any.u, uuid);
        ESP_LOGI(TAG, "  @%3u  %s  %s", handle, kind, uuid);
        return;
    }

    if (type16 == 0x2803 && len >= 5) {
        char props[16];
        ble_probe_props_text(v[0], props, sizeof(props));
        const uint16_t val_handle = (uint16_t)v[1] | ((uint16_t)v[2] << 8);
        if (len == 5) {
            ble_uuid_init_from_buf(&any, &v[3], 2);
        } else if (len == 19) {
            ble_uuid_init_from_buf(&any, &v[3], 16);
        } else {
            return;
        }
        ble_uuid_to_str(&any.u, uuid);
        ESP_LOGI(TAG, "  @%3u  Merkmal          %s  Wert @%u  [%s]",
                 handle, uuid, val_handle, props);
    }
}

static int on_decl_read(uint16_t conn, const struct ble_gatt_error *error,
                        struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    const size_t idx = (size_t)(uintptr_t)arg;

    if (error->status == 0 && attr && attr->om && idx < s_attr_count) {
        uint8_t buf[24];
        uint16_t n = OS_MBUF_PKTLEN(attr->om);
        if (n > sizeof(buf)) {
            n = sizeof(buf);
        }
        if (os_mbuf_copydata(attr->om, 0, n, buf) == 0) {
            log_declaration(s_attrs[idx].handle, s_attrs[idx].type16, buf, n);
        }
    } else if (idx < s_attr_count) {
        ESP_LOGW(TAG, "  @%3u  Deklaration nicht lesbar (ATT 0x%04X)",
                 s_attrs[idx].handle, error->status);
    }

    decl_read_next();
    return 0;
}

/** Alle Deklarationen der Reihe nach lesen, um ihre UUIDs aufzulösen. */
static void decl_read_next(void)
{
    while (s_decl_idx < s_attr_count) {
        const size_t idx = s_decl_idx++;
        const uint16_t t = s_attrs[idx].type16;
        if (t != 0x2800 && t != 0x2801 && t != 0x2803) {
            continue;
        }
        if (ble_gattc_read(s_conn, s_attrs[idx].handle, on_decl_read,
                           (void *)(uintptr_t)idx) == 0) {
            return; /* weiter im Rückruf */
        }
    }

    ESP_LOGI(TAG, "=== Erkundung abgeschlossen: %u Attribute ===",
             (unsigned)s_attr_count);
    set_state(BLE_PROBE_READY);

    if (s_survey_mode) {
        /* Einmalig gemeint — der Akku gehört danach wieder der App. */
        s_survey_mode = false;
        ESP_LOGI(TAG, "Erkundung war einmalig, trenne wieder");
        ble_probe_disconnect();
    }
}

static int on_attr(uint16_t conn, const struct ble_gatt_error *error,
                   uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn;
    (void)chr_val_handle;
    (void)arg;

    if (error->status == 0 && dsc) {
        if (s_attr_count < PROBE_MAX_ATTRS) {
            s_attrs[s_attr_count].handle = dsc->handle;
            s_attrs[s_attr_count].type16 =
                (dsc->uuid.u.type == BLE_UUID_TYPE_16) ? ble_uuid_u16(&dsc->uuid.u) : 0;
            ble_uuid_to_str(&dsc->uuid.u, s_attrs[s_attr_count].type);
            ESP_LOGI(TAG, "  Attribut @%3u  Typ %s", dsc->handle,
                     s_attrs[s_attr_count].type);
            s_attr_count++;
        }
        return 0;
    }

    ESP_LOGI(TAG, "%u Attribute gefunden, löse Deklarationen auf",
             (unsigned)s_attr_count);
    s_decl_idx = 0;
    decl_read_next();
    return 0;
}

/** Den Sweep über den gesamten Handle-Bereich starten. */
static void survey_start(void)
{
    s_attr_count = 0;
    ESP_LOGI(TAG, "=== vollständige Attributauflistung (Handle 1 … 0xFFFF) ===");

    /* start_handle ist das Handle, **ab dem** gesucht wird (exklusiv) — NimBLE
     * setzt intern prev_handle darauf und beginnt bei prev_handle + 1. Eine 0
     * erfasst damit auch Handle 1. */
    if (ble_gattc_disc_all_dscs(s_conn, 0, 0xFFFF, on_attr, NULL) != 0) {
        ESP_LOGE(TAG, "Attributauflistung ließ sich nicht starten");
        set_state(BLE_PROBE_READY);
    }
}

/* -------------------------------------------------------------------------- */
/*  Lesbare Merkmale einmal auslesen                                          */
/* -------------------------------------------------------------------------- */

static void read_next(void);
static void survey_start(void);

static int on_read(uint16_t conn, const struct ble_gatt_error *error,
                   struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    const size_t idx = (size_t)(uintptr_t)arg;

    if (idx < s_entry_count) {
        if (error->status == 0 && attr && attr->om) {
            handle_payload("R", s_entries[idx].chr, s_entries[idx].val_handle, attr->om);
        } else {
            /* Auch die Ablehnung ist ein Befund: Der ATT-Status sagt, *warum*
             * nicht gelesen werden darf — „Read Not Permitted" ist etwas ganz
             * anderes als „Insufficient Authentication", und Letzteres würde
             * bedeuten, dass dem Gerät eine verschlüsselte Verbindung fehlt. */
            char line[96];
            snprintf(line, sizeof(line), "R %s @%u: abgelehnt, ATT-Status 0x%04X",
                     s_entries[idx].chr, s_entries[idx].val_handle, error->status);
            xSemaphoreTake(s_lock, portMAX_DELAY);
            log_line(line);
            xSemaphoreGive(s_lock);
            ESP_LOGI(TAG, "%s", line);
        }
    }

    read_next();
    return 0;
}

/**
 * Jedes Merkmal einmal lesen — auch die ohne READ-Property.
 *
 * Lesen ist gefahrlos, es verändert am BMS nichts. Zwei Gründe, es überall zu
 * versuchen: Manche Bausteine legen ihre Messwerte direkt in lesbare Merkmale,
 * statt sie zu senden. Und wo gelesen werden *darf*, obwohl die Properties es
 * nicht bewerben, ist das ein Befund — ebenso wie der Fehlercode, wenn nicht.
 */
static void read_next(void)
{
    while (s_read_idx < s_entry_count) {
        const size_t idx = s_read_idx++;
        if (ble_gattc_read(s_conn, s_entries[idx].val_handle, on_read,
                           (void *)(uintptr_t)idx) == 0) {
            return; /* weiter im Rückruf */
        }
    }

    ESP_LOGI(TAG, "fertig: %u Merkmale", (unsigned)s_entry_count);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    log_line("--- bereit, warte auf Benachrichtigungen ---");
    xSemaphoreGive(s_lock);

    /* Zum Schluss der Sweep — er deckt auf, was die Dienstsuche übersieht. */
    survey_start();
}

/* -------------------------------------------------------------------------- */
/*  Anmelden auf Benachrichtigungen                                           */
/* -------------------------------------------------------------------------- */

static void subscribe_next(void);

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *error,
                           struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    (void)attr;
    const size_t idx = (size_t)(uintptr_t)arg;

    if (error->status == 0 && idx < s_entry_count) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_entries[idx].subscribed = true;
        s_generation++;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "angemeldet auf %s", s_entries[idx].chr);
    } else if (error->status != 0) {
        /* Kein Beinbruch: nicht jedes Merkmal hat ein CCCD an der erwarteten
         * Stelle. Die übrigen werden trotzdem durchprobiert. */
        ESP_LOGD(TAG, "Anmeldung auf Eintrag %u abgelehnt (Status %d)",
                 (unsigned)idx, error->status);
    }

    subscribe_next();
    return 0;
}

/** CCCD des gefundenen Deskriptors beschreiben. */
static void write_cccd(size_t idx)
{
    const ble_probe_entry_t *e = &s_entries[idx];

    if (e->cccd_handle == 0) {
        ESP_LOGW(TAG, "kein CCCD (0x2902) bei %s — nicht anmeldbar", e->chr);
        subscribe_next();
        return;
    }

    static const uint8_t enable_notify[2] = {0x01, 0x00};
    static const uint8_t enable_indicate[2] = {0x02, 0x00};
    const uint8_t *value = (e->props & BLE_GATT_CHR_PROP_NOTIFY)
                               ? enable_notify : enable_indicate;

    if (ble_gattc_write_flat(s_conn, e->cccd_handle, value, 2, on_cccd_written,
                             (void *)(uintptr_t)idx) != 0) {
        subscribe_next();
    }
}

/**
 * Deskriptoren eines Merkmals durchgehen und das CCCD heraussuchen.
 *
 * Vorher wurde es auf `Wert-Handle + 1` geraten. Das stimmt fast immer, aber
 * „fast immer" ist bei einer Fehlersuche wertlos: Solange die Annahme nicht
 * geprüft ist, bleibt offen, ob wir uns überhaupt angemeldet haben. Die
 * Referenzimplementierungen suchen den Deskriptor ebenfalls, statt ihn zu
 * berechnen. Nebenbei landen alle Deskriptoren im Log.
 */
static int on_dsc(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
    (void)conn;
    (void)chr_val_handle;
    const size_t idx = (size_t)(uintptr_t)arg;

    if (error->status == 0 && dsc && idx < s_entry_count) {
        char uuid[BLE_UUID_STR_LEN];
        ble_uuid_to_str(&dsc->uuid.u, uuid);
        ESP_LOGI(TAG, "    Deskriptor %s  Handle %u", uuid, dsc->handle);

        if (ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(0x2902)) == 0 &&
            s_entries[idx].cccd_handle == 0) {
            s_entries[idx].cccd_handle = dsc->handle;
        }
        return 0;
    }

    write_cccd(idx);
    return 0;
}

/** Nächstes Notify-Merkmal: erst Deskriptoren suchen, dann anmelden. */
static void subscribe_next(void)
{
    while (s_sub_idx < s_entry_count) {
        const ble_probe_entry_t *e = &s_entries[s_sub_idx];
        const size_t idx = s_sub_idx++;

        if (!(e->props & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE))) {
            continue;
        }
        if (e->val_handle >= e->svc_end) {
            continue; /* kein Platz für einen Deskriptor dahinter */
        }
        /* `start_handle` ist das **Wert-Handle**, nicht das erste zu
         * durchsuchende. NimBLE setzt intern `prev_handle = start_handle` und
         * sucht ab `prev_handle + 1`. Übergibt man hier val_handle+1, beginnt
         * die Suche eins zu weit hinten — bei einem Dienst, der direkt hinter
         * dem CCCD endet, findet sie dann gar nichts.
         *
         * Als Ende dient das Dienstende. Bei einem Merkmal mitten im Dienst
         * würde das in die folgenden Merkmale hineinreichen; hier ist das
         * Notify-Merkmal jeweils das letzte, deshalb unkritisch. */
        if (ble_gattc_disc_all_dscs(s_conn, e->val_handle, e->svc_end, on_dsc,
                                    (void *)(uintptr_t)idx) == 0) {
            return; /* weiter im Rückruf */
        }
    }

    /* Ab hier **exakt** wie die Hersteller-App: Anmelden, dann ohne Umweg das
     * erste Kommando. Der Mitschnitt zeigt zwischen CCCD-Schreibzugriff und
     * `AA 00 00 00 00` keine einzige weitere ATT-Nachricht.
     *
     * Unser Board hatte dazwischen alle Merkmale gelesen — darunter zwei, die
     * das BMS mit „Read Not Permitted" ablehnte. Danach blieb es stumm. Zwei
     * abgelehnte Leseanfragen sind für einen ordentlichen Stapel folgenlos;
     * bei dieser Firmware sind sie der einzige erkennbare Unterschied zwischen
     * antworten und schweigen. Deshalb: keine Leseanfragen mehr.
     *
     * `read_next()` bleibt für die einmalige Erkundung erhalten. */
    if (s_survey_mode) {
        s_read_idx = 0;
        read_next();
        return;
    }

    set_state(BLE_PROBE_READY);
    ESP_LOGI(TAG, "angemeldet — Abfragen laufen los");
    ble_probe_start_queries();
}

/* -------------------------------------------------------------------------- */
/*  Dienste und Merkmale                                                      */
/* -------------------------------------------------------------------------- */

static void discover_next_service(void);

static int on_chr(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn;
    (void)arg;

    if (error->status == 0 && chr) {
        if (s_entry_count < BLE_PROBE_MAX_ENTRIES) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            ble_probe_entry_t *e = &s_entries[s_entry_count++];
            memset(e, 0, sizeof(*e));
            snprintf(e->svc, sizeof(e->svc), "%s", s_svcs[s_svc_idx].uuid);
            ble_uuid_to_str(&chr->uuid.u, e->chr);
            e->val_handle = chr->val_handle;
            e->svc_end = s_svcs[s_svc_idx].end;
            e->props = chr->properties;
            s_generation++;
            xSemaphoreGive(s_lock);

            if (s_write_handle == 0 &&
                (e->props & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP))) {
                s_write_handle = e->val_handle;
                s_write_props = e->props;
            }

            /* Auch ins Log: vom Schirm abzutippen ist fehleranfällig, und
             * 128-Bit-UUIDs passen dort ohnehin nur knapp in die Spalte. */
            char props[16];
            ble_probe_props_text(e->props, props, sizeof(props));
            ESP_LOGI(TAG, "  Merkmal %s  Handle %u  [%s]", e->chr, e->val_handle, props);
        }
        return 0;
    }

    /* EDONE oder Fehler: in beiden Fällen ist dieser Dienst abgearbeitet. */
    s_svc_idx++;
    discover_next_service();
    return 0;
}

static void discover_next_service(void)
{
    while (s_svc_idx < s_svc_count) {
        const int rc = ble_gattc_disc_all_chrs(s_conn, s_svcs[s_svc_idx].start,
                                               s_svcs[s_svc_idx].end, on_chr, NULL);
        if (rc == 0) {
            return; /* weiter im Rückruf */
        }
        ESP_LOGW(TAG, "Merkmalssuche in %s fehlgeschlagen (rc=%d)",
                 s_svcs[s_svc_idx].uuid, rc);
        s_svc_idx++;
    }

    ESP_LOGI(TAG, "%u Dienste, %u Merkmale gefunden",
             (unsigned)s_svc_count, (unsigned)s_entry_count);
    set_state(BLE_PROBE_SUBSCRIBING);
    s_sub_idx = 0;
    subscribe_next();
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *service, void *arg);

/**
 * MTU aushandeln, bevor irgendetwas anderes läuft.
 *
 * NimBLE als Central tut das **nicht von allein** — es bleibt bei den 23 Byte
 * der Voreinstellung, also 20 Nutzbytes je Paket. Jede Handy-App handelt an
 * dieser Stelle mehr aus. Ein BMS, dessen Antwort länger ist, kann sie mit dem
 * kleinen MTU schlicht nicht schicken; dann wartet man ewig auf eine
 * Benachrichtigung, die nie kommen kann.
 *
 * Der Schritt ist Standard und verändert am Gerät nichts.
 */
static int on_mtu(uint16_t conn, const struct ble_gatt_error *error,
                  uint16_t mtu, void *arg)
{
    (void)arg;

    if (error->status == 0) {
        ESP_LOGI(TAG, "MTU ausgehandelt: %u Byte (%u Nutzbytes je Paket)",
                 mtu, mtu - 3);
    } else {
        ESP_LOGW(TAG, "MTU-Aushandlung abgelehnt (Status %d) — bleibt bei 23",
                 error->status);
    }

    /* Erst danach die Dienste lesen: ein größerer MTU gilt für alles Folgende. */
    ble_gattc_disc_all_svcs(conn, on_svc, NULL);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *error,
                  const struct ble_gatt_svc *service, void *arg)
{
    (void)conn;
    (void)arg;

    if (error->status == 0 && service) {
        if (s_svc_count < BLE_PROBE_MAX_SVCS) {
            s_svcs[s_svc_count].start = service->start_handle;
            s_svcs[s_svc_count].end = service->end_handle;
            ble_uuid_to_str(&service->uuid.u, s_svcs[s_svc_count].uuid);
            ESP_LOGI(TAG, "Dienst %s (Handles %u…%u)", s_svcs[s_svc_count].uuid,
                     service->start_handle, service->end_handle);
            s_svc_count++;
        }
        return 0;
    }

    if (s_svc_count == 0) {
        ESP_LOGE(TAG, "keine Dienste gefunden (Status %d)", error->status);
        set_state(BLE_PROBE_FAILED);
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }

    s_svc_idx = 0;
    discover_next_service();
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  GAP                                                                       */
/* -------------------------------------------------------------------------- */

static int probe_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (s_state != BLE_PROBE_SEARCHING) {
            break;
        }
        /* NimBLE liefert die Adresse little-endian, gesucht wird in
         * Anzeigereihenfolge — deshalb rückwärts vergleichen. */
        bool match = true;
        for (int i = 0; i < 6; i++) {
            if (event->disc.addr.val[5 - i] != s_want[i]) {
                match = false;
                break;
            }
        }
        if (!match) {
            break;
        }

        ESP_LOGI(TAG, "Gerät gefunden (%d dBm, Adresstyp %u)",
                 event->disc.rssi, event->disc.addr.type);
        ble_gap_disc_cancel();
        set_state(BLE_PROBE_CONNECTING);
        /* Verbindungsparameter genau wie das iPhone im Mitschnitt: festes
         * Intervall von 30 ms statt einer Spanne 30…50 ms, Timeout 720 ms
         * statt 2,56 s, und eine zugesicherte Ereignislänge statt null.
         *
         * Das ist der letzte Unterschied, der zwischen unserer Verbindung und
         * der von iPhone und Mac übrig geblieben ist. Auf ATT-Ebene stimmen
         * wir Byte für Byte überein (siehe ble_probe.h), trotzdem sendet das
         * BMS uns nie eine Benachrichtigung. Mit max_ce_len = 0 darf der
         * Controller das Verbindungsereignis schließen, sobald die Antwort da
         * ist — ein Modul, das seine Benachrichtigung erst danach anhängt,
         * käme dann nie zum Zug. */
        static const struct ble_gap_conn_params conn_params = {
            .scan_itvl = 16,
            .scan_window = 16,
            .itvl_min = 24, /* 30 ms */
            .itvl_max = 24, /* 30 ms */
            .latency = 0,
            .supervision_timeout = 72, /* 720 ms */
            .min_ce_len = 16,
            .max_ce_len = 24,
        };
        if (ble_gap_connect(ble_host_own_addr_type(), &event->disc.addr, 10000,
                            &conn_params, probe_gap_event, NULL) != 0) {
            set_state(BLE_PROBE_FAILED);
        }
        break;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (s_state == BLE_PROBE_SEARCHING) {
            ESP_LOGW(TAG, "Gerät nicht gefunden");
            set_state(BLE_PROBE_FAILED);
        }
        break;

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "Verbindung fehlgeschlagen (Status %d)", event->connect.status);
            set_state(BLE_PROBE_FAILED);
            break;
        }
        s_conn = event->connect.conn_handle;
        s_svc_count = 0;
        s_entry_count = 0;
        s_write_handle = 0;
        s_write_props = 0;
        set_state(BLE_PROBE_DISCOVERING);
        ESP_LOGI(TAG, "verbunden, handle MTU aus");
        if (ble_gattc_exchange_mtu(s_conn, on_mtu, NULL) != 0) {
            /* Nicht schlimm — dann eben mit den voreingestellten 23 Byte. */
            ESP_LOGW(TAG, "MTU-Aushandlung ließ sich nicht starten");
            ble_gattc_disc_all_svcs(s_conn, on_svc, NULL);
        }
        break;

    case BLE_GAP_EVENT_ENC_CHANGE: {
        char line[80];
        snprintf(line, sizeof(line), "--- Verschlüsselung: Status %d ---",
                 event->enc_change.status);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        log_line(line);
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "%s", line);
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "getrennt (Grund %d)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        set_state(BLE_PROBE_IDLE);
        /* Halbe Telegramme aus der alten Verbindung dürfen nicht in die neue
         * hineinlaufen. */
        s_query_idx = QUERY_COUNT;
        s_reconnect_at_us = esp_timer_get_time() + (int64_t)PROBE_RECONNECT_MS * 1000;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_rx_len = 0;
        log_line("--- Verbindung beendet ---");
        xSemaphoreGive(s_lock);
        break;

    case BLE_GAP_EVENT_NOTIFY_RX:
        /* Das ist der eigentliche Ertrag: rohe Bytes vom BMS. */
        s_notify_count++;
        handle_payload("N", "", event->notify_rx.attr_handle, event->notify_rx.om);
        break;

    default:
        break;
    }
    return 0;
}

static void probe_on_host_ready(void)
{
    if (s_state != BLE_PROBE_SEARCHING) {
        return;
    }

    const struct ble_gap_disc_params params = {
        .itvl = 160,  /* 100 ms */
        .window = 96, /*  60 ms */
        .filter_policy = BLE_HCI_SCAN_FILT_NO_WL,
        .limited = 0,
        .passive = 0,
        .filter_duplicates = 1,
    };
    if (ble_gap_disc(ble_host_own_addr_type(), BLE_PROBE_SEARCH_MS, &params,
                     probe_gap_event, NULL) != 0) {
        set_state(BLE_PROBE_FAILED);
    }
}

/* -------------------------------------------------------------------------- */
/*  Öffentliche API                                                           */
/* -------------------------------------------------------------------------- */

esp_err_t ble_probe_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "Mutex");
    }
    ESP_RETURN_ON_ERROR(ble_host_register(probe_on_host_ready), TAG, "Rückruf eintragen");
    ESP_LOGI(TAG, "GATT-Erkunder bereit (Vorgabe %s)", BLE_PROBE_DEFAULT_MAC);

    /* `ble_gap_security_initiate()` liefert BLE_HS_ENOTSUP, wenn der Security
     * Manager nicht einkompiliert ist — und das ist der einzige Weg zu diesem
     * Fehler. Statt weiter Header zu lesen: den Compiler fragen. */
#if NIMBLE_BLE_SM
    ESP_LOGI(TAG, "Security Manager einkompiliert — Kopplung grundsätzlich möglich");
#else
    ESP_LOGW(TAG, "Security Manager NICHT einkompiliert — \"Koppeln\" kann nicht "
                  "funktionieren, unabhängig vom Akku");
#endif
    return ESP_OK;
}

esp_err_t ble_probe_connect(const char *mac)
{
    ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_INVALID_STATE, TAG, "ble_probe_init() fehlt");
    ESP_RETURN_ON_FALSE(mac, ESP_ERR_INVALID_ARG, TAG, "keine Adresse");

    if (s_state != BLE_PROBE_IDLE && s_state != BLE_PROBE_FAILED) {
        return ESP_OK;
    }

    unsigned v[6];
    ESP_RETURN_ON_FALSE(sscanf(mac, "%x:%x:%x:%x:%x:%x",
                               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6,
                        ESP_ERR_INVALID_ARG, TAG, "Adresse \"%s\" nicht lesbar", mac);
    for (int i = 0; i < 6; i++) {
        s_want[i] = (uint8_t)v[i];
    }
    snprintf(s_mac, sizeof(s_mac), "%s", mac);
    s_want_connected = true;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_entry_count = 0;
    s_svc_count = 0;
    s_rx_len = 0;
    s_generation++;
    xSemaphoreGive(s_lock);

    set_state(BLE_PROBE_SEARCHING);
    ESP_LOGI(TAG, "suche %s", mac);

    if (!ble_host_is_ready()) {
        return ble_host_start(); /* weiter im Rückruf */
    }
    probe_on_host_ready();
    return ESP_OK;
}

uint16_t ble_probe_write_handle(void)
{
    return s_write_handle;
}

/** Erster Eintrag mit Notify — das ist bei diesem Gerät 0x0003. */
static const ble_probe_entry_t *notify_entry(void)
{
    for (size_t i = 0; i < s_entry_count; i++) {
        if (s_entries[i].props & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE)) {
            return &s_entries[i];
        }
    }
    return NULL;
}

uint16_t ble_probe_notify_cccd(void)
{
    const ble_probe_entry_t *e = notify_entry();
    return e ? e->cccd_handle : 0;
}

bool ble_probe_notify_subscribed(void)
{
    const ble_probe_entry_t *e = notify_entry();
    return e ? e->subscribed : false;
}

esp_err_t ble_probe_pair(void)
{
    ESP_RETURN_ON_FALSE(s_conn != BLE_HS_CONN_HANDLE_NONE, ESP_ERR_INVALID_STATE, TAG,
                        "nicht verbunden");
    const int rc = ble_gap_security_initiate(s_conn);
    if (rc != 0) {
        ESP_LOGW(TAG, "Kopplung ließ sich nicht anstoßen (rc=%d)", rc);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Kopplung angestoßen");
    return ESP_OK;
}

/**
 * Ergebnis eines Schreibvorgangs protokollieren.
 *
 * Ohne diesen Rückruf bliebe offen, ob das BMS die Bytes überhaupt angenommen
 * hat. Eine ATT-Ablehnung — „Request Not Supported", „Invalid Attribute Value
 * Length", „Write Not Permitted" — ist ein völlig anderer Befund als
 * schweigendes Verwerfen und führt zu einer anderen nächsten Frage.
 */
static int on_write_done(uint16_t conn, const struct ble_gatt_error *error,
                         struct ble_gatt_attr *attr, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;

    char line[80];
    if (error->status == 0) {
        snprintf(line, sizeof(line), "  Schreiben bestätigt (ATT ok)");
    } else {
        snprintf(line, sizeof(line), "  Schreiben abgelehnt, ATT-Status 0x%04X",
                 error->status);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    log_line(line);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s", line);
    return 0;
}

/** Ein Hexzeichen als Wert, oder -1. */
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

esp_err_t ble_probe_write_hex(const char *hex)
{
    ESP_RETURN_ON_FALSE(hex, ESP_ERR_INVALID_ARG, TAG, "keine Bytes");
    ESP_RETURN_ON_FALSE(s_state == BLE_PROBE_READY, ESP_ERR_INVALID_STATE, TAG,
                        "nicht verbunden");
    ESP_RETURN_ON_FALSE(s_write_handle != 0, ESP_ERR_NOT_FOUND, TAG,
                        "kein beschreibbares Merkmal gefunden");

    /* Alles außer Hexziffern überlesen — so gehen Leerzeichen, Doppelpunkte
     * und ein vorangestelltes "0x" gleichermaßen durch. */
    uint8_t bytes[32];
    size_t len = 0;
    int hi = -1;
    for (const char *p = hex; *p && len < sizeof(bytes); p++) {
        const int d = hex_digit(*p);
        if (d < 0) {
            continue;
        }
        if (hi < 0) {
            hi = d;
        } else {
            bytes[len++] = (uint8_t)((hi << 4) | d);
            hi = -1;
        }
    }
    ESP_RETURN_ON_FALSE(len > 0, ESP_ERR_INVALID_ARG, TAG, "keine Bytes erkannt");
    ESP_RETURN_ON_FALSE(hi < 0, ESP_ERR_INVALID_ARG, TAG, "ungerade Zahl von Hexziffern");

    /* Erst protokollieren, dann senden: steht die Anfrage schon im Mitschnitt,
     * lässt sich die Antwort eindeutig zuordnen. */
    char line[160];
    int n = snprintf(line, sizeof(line), "W @%u:", s_write_handle);
    for (size_t i = 0; i < len && n < (int)sizeof(line) - 4; i++) {
        n += snprintf(line + n, sizeof(line) - n, " %02X", bytes[i]);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    log_line(line);
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%s", line);

    /* Mit Rückruf, nicht ohne: „kein Fehler beim Absenden" heißt noch lange
     * nicht „vom Gerät angenommen". Lehnt das BMS den Schreibzugriff auf
     * ATT-Ebene ab, steht die Begründung sonst nirgends. */
    const bool with_response =
        ((s_write_props & BLE_GATT_CHR_PROP_WRITE) != 0) != s_write_inverted;
    const int rc = with_response
                       ? ble_gattc_write_flat(s_conn, s_write_handle, bytes, len,
                                              on_write_done, NULL)
                       : ble_gattc_write_no_rsp_flat(s_conn, s_write_handle, bytes, len);
    if (rc != 0) {
        ESP_LOGW(TAG, "Senden fehlgeschlagen (rc=%d)", rc);
        return ESP_FAIL;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------------- */
/*  Abfragefolge und Wiederverbinden                                          */
/* -------------------------------------------------------------------------- */

esp_err_t ble_probe_start_queries(void)
{
    ESP_RETURN_ON_FALSE(s_want_connected, ESP_ERR_INVALID_STATE, TAG,
                        "erst verbinden");

    if (s_state == BLE_PROBE_READY) {
        s_query_idx = 0;
        s_query_next_us = 0; /* erste Abfrage sofort */
        s_queries_pending = false;
        return ESP_OK;
    }

    /* Der Aufbau dauert mehrere Sekunden — Suchen, Verbinden, Discovery,
     * Anmelden. Einen Tastendruck in dieser Zeit zu verwerfen wäre nur
     * ärgerlich; er wird stattdessen vorgemerkt. */
    s_queries_pending = true;
    ESP_LOGI(TAG, "Abfragen vorgemerkt — werden nach dem Verbinden gesendet");
    return ESP_OK;
}

bool ble_probe_queries_running(void)
{
    return s_query_idx < QUERY_COUNT || s_queries_pending;
}

bool ble_probe_queries_pending(void)
{
    return s_queries_pending;
}

bool ble_probe_wants_connection(void)
{
    return s_want_connected;
}

void ble_probe_tick(void)
{
    const int64_t now = esp_timer_get_time();

    /* Vorgemerkte Abfragen starten, sobald die Verbindung steht. */
    if (s_queries_pending && s_state == BLE_PROBE_READY) {
        s_queries_pending = false;
        s_query_idx = 0;
        s_query_next_us = 0;
        ESP_LOGI(TAG, "vorgemerkte Abfragen werden gesendet");
    }

    /* Im Betrieb regelmäßig nachfragen — der Akku meldet sich nicht von allein.
     * Die Hersteller-App macht es genauso: derselbe Satz Abfragen, wiederholt. */
    if (s_query_idx >= QUERY_COUNT && s_state == BLE_PROBE_READY && now >= s_poll_at_us) {
        s_poll_at_us = now + (int64_t)POLL_INTERVAL_MS * 1000;
        s_query_idx = 0;
        s_query_next_us = 0;
    }

    /* Abfragefolge weiterschalten. */
    if (s_query_idx < QUERY_COUNT && s_state == BLE_PROBE_READY &&
        now >= s_query_next_us) {
        ble_probe_write_hex(s_queries[s_query_idx]);
        s_query_idx++;
        s_query_next_us = now + (int64_t)QUERY_GAP_MS * 1000;

        /* Ganze Folge durch und kein Ton zurück? Dann denselben Durchgang
         * einmal mit dem anderen Schreibmodus wiederholen. */
        if (s_query_idx == QUERY_COUNT && s_notify_count == 0 && !s_write_inverted) {
            s_write_inverted = true;
            s_query_idx = 0;
            ESP_LOGW(TAG, "keine Antwort — zweiter Durchgang mit %s",
                     (s_write_props & BLE_GATT_CHR_PROP_WRITE)
                         ? "Write Command (ohne Response)"
                         : "Write Request (mit Response)");
        }
    }

    /* Wiederverbinden nach einem Abbruch. Mit Abstand, damit ein Akku außer
     * Reichweite nicht dauerhaft den Controller beschäftigt. */
    if (s_want_connected && (s_state == BLE_PROBE_IDLE || s_state == BLE_PROBE_FAILED) &&
        now >= s_reconnect_at_us) {
        ESP_LOGI(TAG, "Wiederverbindungsversuch");
        s_reconnect_at_us = now + (int64_t)PROBE_RECONNECT_MS * 1000;
        ble_probe_connect(s_mac);
    }
}

void ble_probe_battery(ble_probe_battery_t *out)
{
    if (!out) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_batt;
    xSemaphoreGive(s_lock);
}

int64_t ble_probe_battery_age_ms(void)
{
    return (s_batt_us < 0) ? -1 : (esp_timer_get_time() - s_batt_us) / 1000;
}

int64_t ble_probe_battery_stamp_us(void)
{
    return s_batt_us;
}

esp_err_t ble_probe_survey_once(void)
{
    s_survey_mode = true;
    return ble_probe_connect(BLE_PROBE_DEFAULT_MAC);
}

esp_err_t ble_probe_disconnect(void)
{
    /* Ausdrücklich getrennt heißt: kein Wiederverbinden. */
    s_want_connected = false;
    s_query_idx = QUERY_COUNT;
    s_queries_pending = false;

    if (s_state == BLE_PROBE_SEARCHING) {
        ble_gap_disc_cancel();
        set_state(BLE_PROBE_IDLE);
        return ESP_OK;
    }
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        set_state(BLE_PROBE_IDLE);
        return ESP_OK;
    }
    return (ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM) == 0) ? ESP_OK : ESP_FAIL;
}
