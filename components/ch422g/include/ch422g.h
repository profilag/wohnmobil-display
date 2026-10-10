/**
 * @file ch422g.h
 * @brief Treiber für den CH422G I2C-IO-Expander (WCH), wie er auf dem
 *        Waveshare ESP32-S3-Touch-LCD-7 verbaut ist.
 *
 * Der CH422G ist ein reines "Adress-als-Register"-Bauteil: es gibt keinen
 * Registerzeiger, sondern vier feste I2C-Adressen, an die jeweils genau ein
 * Datenbyte geschrieben (bzw. von einer gelesen) wird.
 *
 * Pin-Belegung auf dem Waveshare 7"-Board (IO0..IO7):
 *   IO0 = frei          IO1 = TP_RST        IO2 = LCD_BL (Backlight)
 *   IO3 = LCD_RST       IO4 = SD_CS         IO5 = USB_SEL
 * OC0..OC3 sind die vier Open-Drain-Ausgänge (Index 8..11).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Feste I2C-Adressen des CH422G (7 Bit). */
#define CH422G_ADDR_WR_SET  0x24  /*!< Modus-/Konfigurationsregister      */
#define CH422G_ADDR_WR_OC   0x23  /*!< Ausgangswerte OC0..OC3 (Low-Nibble) */
#define CH422G_ADDR_WR_IO   0x38  /*!< Ausgangswerte IO0..IO7             */
#define CH422G_ADDR_RD_IO   0x26  /*!< Eingangswerte IO0..IO7 (nur lesen) */

/** Bits im WR_SET-Register. */
#define CH422G_WR_SET_IO_OE     (1 << 0) /*!< 1 = IO0..IO7 sind Ausgänge      */
#define CH422G_WR_SET_A_SCAN    (1 << 1) /*!< Tastatur-Scan aktiv             */
#define CH422G_WR_SET_OD_EN     (1 << 2) /*!< OC0..OC3 als Open Drain         */
#define CH422G_WR_SET_SLEEP     (1 << 3) /*!< Sleep-Modus                     */

/** Logische Pin-Namen des Waveshare-Boards. */
typedef enum {
    CH422G_PIN_IO0     = 0,
    CH422G_PIN_TP_RST  = 1,
    CH422G_PIN_LCD_BL  = 2,
    CH422G_PIN_LCD_RST = 3,
    CH422G_PIN_SD_CS   = 4,
    CH422G_PIN_USB_SEL = 5,
    CH422G_PIN_IO6     = 6,
    CH422G_PIN_IO7     = 7,
    CH422G_PIN_OC0     = 8,
    CH422G_PIN_OC1     = 9,
    CH422G_PIN_OC2     = 10,
    CH422G_PIN_OC3     = 11,
} ch422g_pin_t;

typedef struct ch422g_t *ch422g_handle_t;

/**
 * @brief CH422G an einem bereits erzeugten I2C-Master-Bus anmelden.
 *
 * Setzt IO0..IO7 auf Ausgang und OC0..OC3 auf Push-Pull (kein Open Drain),
 * damit LCD_RST/TP_RST/Backlight sauber getrieben werden können.
 */
esp_err_t ch422g_new(i2c_master_bus_handle_t bus, ch422g_handle_t *out_dev);

/** @brief Handle freigeben (I2C-Bus bleibt bestehen). */
esp_err_t ch422g_del(ch422g_handle_t dev);

/**
 * @brief Einen einzelnen Ausgang setzen.
 *
 * Der Treiber hält den letzten geschriebenen Zustand im RAM (der CH422G kann
 * seine Ausgangsregister nicht zurücklesen) und schreibt immer das komplette
 * Byte.
 */
esp_err_t ch422g_set_level(ch422g_handle_t dev, ch422g_pin_t pin, bool level);

/** @brief Alle acht IO-Ausgänge in einem Rutsch setzen. */
esp_err_t ch422g_write_io(ch422g_handle_t dev, uint8_t value);

/** @brief Alle vier OC-Ausgänge in einem Rutsch setzen (Low-Nibble). */
esp_err_t ch422g_write_oc(ch422g_handle_t dev, uint8_t value);

/**
 * @brief IO0..IO7 als Eingänge lesen.
 *
 * Achtung: Der CH422G kann IO0..IO7 nur gemeinsam auf Eingang schalten. Diese
 * Funktion deaktiviert dazu kurzzeitig IO_OE — auf dem Waveshare-Board würde
 * das LCD_RST/TP_RST/Backlight loslassen. Nur benutzen, wenn du weißt, was du
 * tust; für zusätzliche Eingänge ist der externe AW9523B die richtige Wahl.
 */
esp_err_t ch422g_read_io(ch422g_handle_t dev, uint8_t *out_value);

/** @brief WR_SET-Register direkt schreiben (Bit-Masken siehe oben). */
esp_err_t ch422g_write_set(ch422g_handle_t dev, uint8_t value);

#ifdef __cplusplus
}
#endif
