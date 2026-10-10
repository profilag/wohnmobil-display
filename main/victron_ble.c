#include "victron_ble.h"

#include <stdlib.h>
#include <string.h>

#include "ble_scan.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mbedtls/aes.h"

static const char *TAG = "victron";

/** Versätze in den Manufacturer Data, gezählt ab der Hersteller-ID. */
#define OFF_PREFIX     2
#define OFF_MODEL      4
#define OFF_RECORD     6
#define OFF_IV         7
#define OFF_CHECK      9
#define OFF_PAYLOAD    10
/** Der Solarladeregler-Satz ist zwölf Byte lang. */
#define PAYLOAD_LEN    12

static SemaphoreHandle_t s_lock;
static uint8_t s_key[16];
static victron_data_t s_data;
static int64_t s_last_us = -1;

/* -------------------------------------------------------------------------- */

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

const char *victron_state_text(uint8_t state)
{
    switch (state) {
    case 0:   return "aus";
    case 1:   return "Sparbetrieb";
    case 2:   return "Störung";
    case 3:   return "Konstantstrom";  /* Bulk       */
    case 4:   return "Konstantspannung"; /* Absorption */
    case 5:   return "Erhaltung";      /* Float      */
    case 6:   return "Lagerung";
    case 7:   return "Ausgleich (Hand)";
    case 9:   return "Wechselrichten";
    case 11:  return "Netzteil";
    case 245: return "startet";
    case 246: return "Nachladung";
    case 247: return "Rekonditionierung";
    case 248: return "Batterie-Schutz";
    case 249: return "aktiv";
    case 252: return "Fremdsteuerung";
    case 255: return "unbekannt";
    default:  return "?";
    }
}

const char *victron_error_text(uint8_t error)
{
    /* Nur die Fehler, die an einem Solarladeregler im Wohnmobil realistisch
     * auftreten. Alles andere wird als Nummer angezeigt — besser eine Nummer
     * zum Nachschlagen als ein falscher Klartext. */
    static char buf[24];

    switch (error) {
    case 0:   return "kein Fehler";
    case 1:   return "Batterie zu warm";
    case 2:   return "Batteriespannung zu hoch";
    case 14:  return "Batterie zu kalt";
    case 17:  return "Regler zu warm";
    case 18:  return "Überstrom";
    case 20:  return "Konstantstrom-Zeit überschritten";
    case 26:  return "Klemmen überhitzt";
    case 33:  return "PV-Spannung zu hoch";
    case 34:  return "PV-Strom zu hoch";
    case 38:  return "PV-Abschaltung (Batteriespannung)";
    case 116: return "Kalibrierung verloren";
    case 117: return "Firmware unpassend";
    case 119: return "Einstellungen ungültig";
    case 255: return "unbekannt";
    default:
        snprintf(buf, sizeof(buf), "Fehler %u", error);
        return buf;
    }
}

/* -------------------------------------------------------------------------- */

/**
 * Läuft im NimBLE-Host-Task für jedes Advertising-Paket mit Manufacturer Data.
 * Alles, was nicht von Victron kommt, fällt hier sofort heraus.
 */
static void victron_on_mfg(const uint8_t *mfg, uint8_t len, int8_t rssi)
{
    if (len < OFF_PAYLOAD + PAYLOAD_LEN) {
        return;
    }
    const uint16_t company = (uint16_t)mfg[0] | ((uint16_t)mfg[1] << 8);
    if (company != VICTRON_COMPANY_ID) {
        return;
    }
    if (mfg[OFF_RECORD] != VICTRON_RECORD_SOLAR_CHARGER) {
        return; /* anderes Victron-Gerät, z. B. ein Batteriewächter */
    }

    if (mfg[OFF_CHECK] != s_key[0]) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_data.key_errors++;
        xSemaphoreGive(s_lock);
        return;
    }

    const uint16_t iv = (uint16_t)mfg[OFF_IV] | ((uint16_t)mfg[OFF_IV + 1] << 8);

    /* Zählerblock: der 16-Bit-Wert little-endian vorn, Rest null. Die Nutzlast
     * ist kürzer als ein AES-Block, also bleibt es bei diesem einen Block. */
    uint8_t counter[16] = {0};
    counter[0] = (uint8_t)(iv & 0xFF);
    counter[1] = (uint8_t)(iv >> 8);

    uint8_t plain[PAYLOAD_LEN];
    uint8_t stream_block[16];
    size_t nc_off = 0;

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    if (mbedtls_aes_setkey_enc(&aes, s_key, 128) != 0 ||
        mbedtls_aes_crypt_ctr(&aes, PAYLOAD_LEN, &nc_off, counter, stream_block,
                              &mfg[OFF_PAYLOAD], plain) != 0) {
        mbedtls_aes_free(&aes);
        ESP_LOGW(TAG, "Entschlüsseln fehlgeschlagen");
        return;
    }
    mbedtls_aes_free(&aes);

    const int16_t raw_voltage = (int16_t)((uint16_t)plain[2] | ((uint16_t)plain[3] << 8));
    const int16_t raw_current = (int16_t)((uint16_t)plain[4] | ((uint16_t)plain[5] << 8));
    const uint16_t raw_yield = (uint16_t)plain[6] | ((uint16_t)plain[7] << 8);
    const uint16_t raw_power = (uint16_t)plain[8] | ((uint16_t)plain[9] << 8);
    const uint16_t raw_load = ((uint16_t)plain[10] | ((uint16_t)plain[11] << 8)) & 0x01FF;

    xSemaphoreTake(s_lock, portMAX_DELAY);

    s_data.valid = true;
    s_data.model_id = (uint16_t)mfg[OFF_MODEL] | ((uint16_t)mfg[OFF_MODEL + 1] << 8);
    s_data.device_state = plain[0];
    s_data.charger_error = plain[1];

    /* Jedes Feld hat einen eigenen Wert für "liegt gerade nicht vor". Den als
     * Zahl anzuzeigen wäre grob irreführend — 327,67 V etwa. */
    s_data.has_voltage = (raw_voltage != 0x7FFF);
    s_data.voltage_cv = raw_voltage;
    s_data.has_current = (raw_current != 0x7FFF);
    s_data.current_da = raw_current;
    s_data.has_yield = (raw_yield != 0xFFFF);
    s_data.yield_10wh = raw_yield;
    s_data.has_power = (raw_power != 0xFFFF);
    s_data.solar_power = raw_power;
    s_data.has_load = (raw_load != 0x01FF);
    s_data.load_da = raw_load;

    s_data.rssi = rssi;
    s_data.packets++;
    const uint32_t count = s_data.packets;
    s_last_us = esp_timer_get_time();

    xSemaphoreGive(s_lock);

    /* Nicht jedes Paket ins Log — bei einem Paket je Sekunde wäre das Log sonst
     * in Minuten unbrauchbar. */
    if (count == 1 || count % 60 == 0) {
        ESP_LOGI(TAG, "%s  %d,%02d V  %d,%d A  PV %u W  heute %u,%02u kWh  (%u Pakete)",
                 victron_state_text(plain[0]),
                 raw_voltage / 100, abs(raw_voltage % 100),
                 raw_current / 10, abs(raw_current % 10),
                 raw_power, raw_yield / 100, raw_yield % 100,
                 (unsigned)count);
    }
}

/* -------------------------------------------------------------------------- */

esp_err_t victron_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_lock, ESP_ERR_NO_MEM, TAG, "Mutex");
    }

    const char *hex = VICTRON_KEY_HEX;
    ESP_RETURN_ON_FALSE(strlen(hex) == 32, ESP_ERR_INVALID_ARG, TAG,
                        "Schlüssel muss 32 Hexzeichen haben");
    for (int i = 0; i < 16; i++) {
        const int hi = hex_digit(hex[2 * i]);
        const int lo = hex_digit(hex[2 * i + 1]);
        ESP_RETURN_ON_FALSE(hi >= 0 && lo >= 0, ESP_ERR_INVALID_ARG, TAG,
                            "Schlüssel enthält kein gültiges Hex");
        s_key[i] = (uint8_t)((hi << 4) | lo);
    }

    memset(&s_data, 0, sizeof(s_data));
    s_last_us = -1;

    ESP_RETURN_ON_ERROR(ble_scan_set_mfg_cb(victron_on_mfg), TAG, "Empfänger eintragen");
    ESP_LOGI(TAG, "Victron-Empfang bereit (Prüfbyte %02X), läuft mit dem Scan",
             s_key[0]);
    return ESP_OK;
}

void victron_get(victron_data_t *out)
{
    if (!out) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *out = s_data;
    xSemaphoreGive(s_lock);
}

int64_t victron_age_ms(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const int64_t last = s_last_us;
    xSemaphoreGive(s_lock);
    return (last < 0) ? -1 : (esp_timer_get_time() - last) / 1000;
}
