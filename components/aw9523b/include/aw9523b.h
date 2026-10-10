/**
 * @file aw9523b.h
 * @brief Treiber für den AW9523B I2C-GPIO-Expander / LED-Treiber (Awinic).
 *
 * 16 IOs, aufgeteilt in Port 0 (P0_0..P0_7) und Port 1 (P1_0..P1_7).
 * Jedes IO kann entweder als GPIO (Ein-/Ausgang) oder im LED-Modus als
 * 256-stufige Konstantstromsenke arbeiten.
 *
 * Besonderheiten der Hardware, die man kennen muss:
 *  - Port 0 ist per Reset Open-Drain. Für High-Pegel als Ausgang entweder
 *    Push-Pull global aktivieren (aw9523b_set_p0_push_pull) oder externe
 *    Pull-ups verwenden. Port 1 ist immer Push-Pull.
 *  - Die Adresse wird über AD0/AD1 gewählt: 0x58 (Standard) bis 0x5B.
 *  - Der INT-Ausgang ist Open-Drain, active-low, und wird erst durch Lesen
 *    der Input-Register (0x00/0x01) zurückgesetzt.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Mögliche I2C-Adressen (AD1/AD0). */
#define AW9523B_ADDR_00     0x58  /*!< AD1=0 AD0=0 — Werkseinstellung */
#define AW9523B_ADDR_01     0x59
#define AW9523B_ADDR_10     0x5A
#define AW9523B_ADDR_11     0x5B

/** Register des AW9523B. */
#define AW9523B_REG_IN_P0       0x00
#define AW9523B_REG_IN_P1       0x01
#define AW9523B_REG_OUT_P0      0x02
#define AW9523B_REG_OUT_P1      0x03
#define AW9523B_REG_CONFIG_P0   0x04  /*!< 1 = Eingang, 0 = Ausgang */
#define AW9523B_REG_CONFIG_P1   0x05
#define AW9523B_REG_INT_P0      0x06  /*!< 1 = Interrupt maskiert (aus)  */
#define AW9523B_REG_INT_P1      0x07
#define AW9523B_REG_ID          0x10  /*!< liest 0x23 */
#define AW9523B_REG_GCR         0x11  /*!< Bit4 = P0 Push-Pull, Bit0..1 = IMAX */
#define AW9523B_REG_LED_MODE_P0 0x12  /*!< 0 = LED-Modus, 1 = GPIO-Modus */
#define AW9523B_REG_LED_MODE_P1 0x13
#define AW9523B_REG_SOFT_RESET  0x7F

#define AW9523B_CHIP_ID         0x23

/** Maximalstrom im LED-Modus (Register GCR, Bit1..0). */
typedef enum {
    AW9523B_IMAX_37MA = 0,  /*!< volle Skala, ca. 37 mA */
    AW9523B_IMAX_27MA = 1,  /*!< 3/4 */
    AW9523B_IMAX_18MA = 2,  /*!< 2/4 */
    AW9523B_IMAX_9MA  = 3,  /*!< 1/4 */
} aw9523b_imax_t;

/** Pin-Nummerierung: 0..7 = P0_0..P0_7, 8..15 = P1_0..P1_7. */
#define AW9523B_P0(n)  (n)
#define AW9523B_P1(n)  (8 + (n))
#define AW9523B_PIN_MAX 15

typedef struct aw9523b_t *aw9523b_handle_t;

typedef struct {
    uint16_t i2c_address;   /*!< z. B. AW9523B_ADDR_00 */
    gpio_num_t int_gpio;    /*!< INT-Eingang am ESP32, GPIO_NUM_NC wenn ungenutzt */
    gpio_num_t reset_gpio;  /*!< optionaler Hardware-Reset, GPIO_NUM_NC wenn ungenutzt */
    bool p0_push_pull;      /*!< true = Port 0 Push-Pull statt Open-Drain */
    aw9523b_imax_t imax;    /*!< Stromskala für den LED-Modus */
} aw9523b_config_t;

#define AW9523B_CONFIG_DEFAULT()          \
    {                                     \
        .i2c_address  = AW9523B_ADDR_00,  \
        .int_gpio     = GPIO_NUM_NC,      \
        .reset_gpio   = GPIO_NUM_NC,      \
        .p0_push_pull = true,             \
        .imax         = AW9523B_IMAX_37MA,\
    }

/**
 * @brief AW9523B an einem bestehenden I2C-Master-Bus anmelden und prüfen.
 *
 * Führt einen Software-Reset aus, verifiziert die Chip-ID (0x23) und setzt
 * alle 16 Pins in den GPIO-Modus als Eingang — also den Reset-Zustand, aber
 * mit definierter Push-Pull-/IMAX-Einstellung.
 */
esp_err_t aw9523b_new(i2c_master_bus_handle_t bus, const aw9523b_config_t *config,
                      aw9523b_handle_t *out_dev);

esp_err_t aw9523b_del(aw9523b_handle_t dev);

/** @brief Richtung eines Pins setzen (GPIO-Modus). */
esp_err_t aw9523b_set_direction(aw9523b_handle_t dev, uint8_t pin, gpio_mode_t mode);

/** @brief Richtung für alle 16 Pins auf einmal: Bit = 1 -> Eingang. */
esp_err_t aw9523b_set_direction_mask(aw9523b_handle_t dev, uint16_t input_mask);

/** @brief Ausgangspegel eines Pins setzen. */
esp_err_t aw9523b_set_level(aw9523b_handle_t dev, uint8_t pin, bool level);

/** @brief Eingangspegel eines Pins lesen. */
esp_err_t aw9523b_get_level(aw9523b_handle_t dev, uint8_t pin, bool *out_level);

/** @brief Alle 16 Ausgänge auf einmal schreiben (Bit0 = P0_0 ... Bit15 = P1_7). */
esp_err_t aw9523b_write_all(aw9523b_handle_t dev, uint16_t value);

/**
 * @brief Alle 16 Eingänge auf einmal lesen.
 *
 * Der Lesezugriff quittiert gleichzeitig einen anstehenden Interrupt.
 */
esp_err_t aw9523b_read_all(aw9523b_handle_t dev, uint16_t *out_value);

/** @brief Pin in den LED-Modus (Konstantstromsenke) schalten. */
esp_err_t aw9523b_set_led_mode(aw9523b_handle_t dev, uint8_t pin, bool led_mode);

/** @brief Helligkeit eines Pins im LED-Modus setzen (0..255). */
esp_err_t aw9523b_set_brightness(aw9523b_handle_t dev, uint8_t pin, uint8_t brightness);

/** @brief Interrupt für einen Pin freigeben oder maskieren. */
esp_err_t aw9523b_set_interrupt_enable(aw9523b_handle_t dev, uint8_t pin, bool enable);

/** @brief Interrupt-Maske für alle 16 Pins: Bit = 1 -> Interrupt freigegeben. */
esp_err_t aw9523b_set_interrupt_mask(aw9523b_handle_t dev, uint16_t enable_mask);

/** @brief Port 0 zwischen Push-Pull und Open-Drain umschalten. */
esp_err_t aw9523b_set_p0_push_pull(aw9523b_handle_t dev, bool push_pull);

/** @brief Rohes Register schreiben — für Sonderfälle. */
esp_err_t aw9523b_write_reg(aw9523b_handle_t dev, uint8_t reg, uint8_t value);

/** @brief Rohes Register lesen. */
esp_err_t aw9523b_read_reg(aw9523b_handle_t dev, uint8_t reg, uint8_t *out_value);

/**
 * @brief ISR-Callback für die fallende Flanke am INT-Pin registrieren.
 *
 * Voraussetzung: `int_gpio` wurde in der Konfiguration gesetzt. Der Callback
 * läuft im ISR-Kontext — dort nur eine Queue/Semaphore bedienen und die
 * I2C-Register in einem Task auslesen.
 */
esp_err_t aw9523b_register_isr(aw9523b_handle_t dev, gpio_isr_t isr_handler, void *arg);

#ifdef __cplusplus
}
#endif
