#include "aw9523b.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "aw9523b";

#define AW9523B_I2C_TIMEOUT_MS 100
#define AW9523B_GCR_GPOMD      (1 << 4) /* 1 = Port 0 Push-Pull */

struct aw9523b_t {
    i2c_master_dev_handle_t dev;
    gpio_num_t int_gpio;
    gpio_num_t reset_gpio;
    uint8_t out_shadow[2];      /* letzte Werte OUT_P0 / OUT_P1   */
    uint8_t dir_shadow[2];      /* CONFIG_P0 / CONFIG_P1          */
    uint8_t led_mode_shadow[2]; /* LED_MODE_P0 / LED_MODE_P1      */
    uint8_t int_shadow[2];      /* INT_P0 / INT_P1                */
    uint8_t gcr_shadow;
    bool isr_service_installed;
};

/* Zuordnung Pin -> DIM-Register. Die Reihenfolge im Chip ist nicht linear:
 * 0x20..0x23 = P1_0..P1_3, 0x24..0x2B = P0_0..P0_7, 0x2C..0x2F = P1_4..P1_7. */
static uint8_t dim_reg_for_pin(uint8_t pin)
{
    if (pin < 8) {
        return 0x24 + pin;
    }
    const uint8_t p1 = pin - 8;
    return (p1 < 4) ? (0x20 + p1) : (0x2C + (p1 - 4));
}

esp_err_t aw9523b_write_reg(aw9523b_handle_t dev, uint8_t reg, uint8_t value)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "kein Handle");
    const uint8_t buf[2] = {reg, value};
    return i2c_master_transmit(dev->dev, buf, sizeof(buf), AW9523B_I2C_TIMEOUT_MS);
}

esp_err_t aw9523b_read_reg(aw9523b_handle_t dev, uint8_t reg, uint8_t *out_value)
{
    ESP_RETURN_ON_FALSE(dev && out_value, ESP_ERR_INVALID_ARG, TAG, "ungültige Argumente");
    return i2c_master_transmit_receive(dev->dev, &reg, 1, out_value, 1, AW9523B_I2C_TIMEOUT_MS);
}

esp_err_t aw9523b_new(i2c_master_bus_handle_t bus, const aw9523b_config_t *config,
                      aw9523b_handle_t *out_dev)
{
    ESP_RETURN_ON_FALSE(bus && config && out_dev, ESP_ERR_INVALID_ARG, TAG, "ungültige Argumente");

    struct aw9523b_t *dev = calloc(1, sizeof(struct aw9523b_t));
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_NO_MEM, TAG, "kein Speicher");
    dev->int_gpio = config->int_gpio;
    dev->reset_gpio = config->reset_gpio;

    esp_err_t ret = ESP_OK;

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = config->i2c_address,
        .scl_speed_hz = 400000,
    };
    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &dev->dev), err, TAG,
                      "I2C-Gerät 0x%02X konnte nicht angelegt werden", config->i2c_address);

    if (config->reset_gpio != GPIO_NUM_NC) {
        const gpio_config_t rst_cfg = {
            .pin_bit_mask = 1ULL << config->reset_gpio,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&rst_cfg), err, TAG, "RESET-GPIO");
        gpio_set_level(config->reset_gpio, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(config->reset_gpio, 1);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    uint8_t id = 0;
    ESP_GOTO_ON_ERROR(aw9523b_read_reg(dev, AW9523B_REG_ID, &id), err, TAG,
                      "AW9523B auf Adresse 0x%02X antwortet nicht", config->i2c_address);
    ESP_GOTO_ON_FALSE(id == AW9523B_CHIP_ID, ESP_ERR_NOT_FOUND, err, TAG,
                      "unerwartete Chip-ID 0x%02X (erwartet 0x%02X)", id, AW9523B_CHIP_ID);

    /* Software-Reset: alle Register zurück auf Werkszustand. */
    ESP_GOTO_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_SOFT_RESET, 0x00), err, TAG, "Soft-Reset");
    vTaskDelay(pdMS_TO_TICKS(2));

    dev->gcr_shadow = (config->imax & 0x03) | (config->p0_push_pull ? AW9523B_GCR_GPOMD : 0);
    ESP_GOTO_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_GCR, dev->gcr_shadow), err, TAG, "GCR");

    /* Reset-Zustand explizit spiegeln: alles GPIO-Modus, alles Eingang,
     * alle Interrupts maskiert, Ausgangsregister low. */
    dev->led_mode_shadow[0] = 0xFF;
    dev->led_mode_shadow[1] = 0xFF;
    dev->dir_shadow[0] = 0xFF;
    dev->dir_shadow[1] = 0xFF;
    dev->int_shadow[0] = 0xFF;
    dev->int_shadow[1] = 0xFF;
    dev->out_shadow[0] = 0x00;
    dev->out_shadow[1] = 0x00;

    ESP_GOTO_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_LED_MODE_P0, 0xFF), err, TAG, "LED_MODE_P0");
    ESP_GOTO_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_LED_MODE_P1, 0xFF), err, TAG, "LED_MODE_P1");
    ESP_GOTO_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_INT_P0, 0xFF), err, TAG, "INT_P0");
    ESP_GOTO_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_INT_P1, 0xFF), err, TAG, "INT_P1");

    if (config->int_gpio != GPIO_NUM_NC) {
        const gpio_config_t int_cfg = {
            .pin_bit_mask = 1ULL << config->int_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .intr_type = GPIO_INTR_NEGEDGE,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&int_cfg), err, TAG, "INT-GPIO");
    }

    *out_dev = dev;
    ESP_LOGI(TAG, "AW9523B auf 0x%02X erkannt (ID 0x%02X, P0 %s, IMAX %d)",
             config->i2c_address, id, config->p0_push_pull ? "Push-Pull" : "Open-Drain",
             config->imax);
    return ESP_OK;

err:
    aw9523b_del(dev);
    return ret;
}

esp_err_t aw9523b_del(aw9523b_handle_t dev)
{
    if (!dev) {
        return ESP_OK;
    }
    if (dev->int_gpio != GPIO_NUM_NC) {
        gpio_isr_handler_remove(dev->int_gpio);
    }
    if (dev->dev) {
        i2c_master_bus_rm_device(dev->dev);
    }
    free(dev);
    return ESP_OK;
}

esp_err_t aw9523b_set_direction(aw9523b_handle_t dev, uint8_t pin, gpio_mode_t mode)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "kein Handle");
    ESP_RETURN_ON_FALSE(pin <= AW9523B_PIN_MAX, ESP_ERR_INVALID_ARG, TAG, "Pin %u ungültig", pin);
    ESP_RETURN_ON_FALSE(mode == GPIO_MODE_INPUT || mode == GPIO_MODE_OUTPUT, ESP_ERR_INVALID_ARG,
                        TAG, "nur GPIO_MODE_INPUT oder GPIO_MODE_OUTPUT");

    const uint8_t port = pin / 8;
    const uint8_t bit = pin % 8;
    uint8_t next = dev->dir_shadow[port];
    next = (mode == GPIO_MODE_INPUT) ? (next | (1u << bit)) : (next & ~(1u << bit));

    ESP_RETURN_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_CONFIG_P0 + port, next), TAG, "CONFIG");
    dev->dir_shadow[port] = next;
    return ESP_OK;
}

esp_err_t aw9523b_set_direction_mask(aw9523b_handle_t dev, uint16_t input_mask)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "kein Handle");
    const uint8_t p0 = input_mask & 0xFF;
    const uint8_t p1 = input_mask >> 8;
    ESP_RETURN_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_CONFIG_P0, p0), TAG, "CONFIG_P0");
    ESP_RETURN_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_CONFIG_P1, p1), TAG, "CONFIG_P1");
    dev->dir_shadow[0] = p0;
    dev->dir_shadow[1] = p1;
    return ESP_OK;
}

esp_err_t aw9523b_set_level(aw9523b_handle_t dev, uint8_t pin, bool level)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "kein Handle");
    ESP_RETURN_ON_FALSE(pin <= AW9523B_PIN_MAX, ESP_ERR_INVALID_ARG, TAG, "Pin %u ungültig", pin);

    const uint8_t port = pin / 8;
    const uint8_t bit = pin % 8;
    uint8_t next = dev->out_shadow[port];
    next = level ? (next | (1u << bit)) : (next & ~(1u << bit));

    ESP_RETURN_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_OUT_P0 + port, next), TAG, "OUT");
    dev->out_shadow[port] = next;
    return ESP_OK;
}

esp_err_t aw9523b_write_all(aw9523b_handle_t dev, uint16_t value)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "kein Handle");
    const uint8_t p0 = value & 0xFF;
    const uint8_t p1 = value >> 8;
    ESP_RETURN_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_OUT_P0, p0), TAG, "OUT_P0");
    ESP_RETURN_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_OUT_P1, p1), TAG, "OUT_P1");
    dev->out_shadow[0] = p0;
    dev->out_shadow[1] = p1;
    return ESP_OK;
}

esp_err_t aw9523b_read_all(aw9523b_handle_t dev, uint16_t *out_value)
{
    ESP_RETURN_ON_FALSE(dev && out_value, ESP_ERR_INVALID_ARG, TAG, "ungültige Argumente");
    uint8_t p0 = 0, p1 = 0;
    ESP_RETURN_ON_ERROR(aw9523b_read_reg(dev, AW9523B_REG_IN_P0, &p0), TAG, "IN_P0");
    ESP_RETURN_ON_ERROR(aw9523b_read_reg(dev, AW9523B_REG_IN_P1, &p1), TAG, "IN_P1");
    *out_value = ((uint16_t)p1 << 8) | p0;
    return ESP_OK;
}

esp_err_t aw9523b_get_level(aw9523b_handle_t dev, uint8_t pin, bool *out_level)
{
    ESP_RETURN_ON_FALSE(dev && out_level, ESP_ERR_INVALID_ARG, TAG, "ungültige Argumente");
    ESP_RETURN_ON_FALSE(pin <= AW9523B_PIN_MAX, ESP_ERR_INVALID_ARG, TAG, "Pin %u ungültig", pin);

    uint8_t value = 0;
    ESP_RETURN_ON_ERROR(aw9523b_read_reg(dev, AW9523B_REG_IN_P0 + (pin / 8), &value), TAG, "IN");
    *out_level = (value >> (pin % 8)) & 0x01;
    return ESP_OK;
}

esp_err_t aw9523b_set_led_mode(aw9523b_handle_t dev, uint8_t pin, bool led_mode)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "kein Handle");
    ESP_RETURN_ON_FALSE(pin <= AW9523B_PIN_MAX, ESP_ERR_INVALID_ARG, TAG, "Pin %u ungültig", pin);

    const uint8_t port = pin / 8;
    const uint8_t bit = pin % 8;
    /* 0 = LED-Modus, 1 = GPIO-Modus */
    uint8_t next = dev->led_mode_shadow[port];
    next = led_mode ? (next & ~(1u << bit)) : (next | (1u << bit));

    ESP_RETURN_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_LED_MODE_P0 + port, next), TAG, "LED_MODE");
    dev->led_mode_shadow[port] = next;
    return ESP_OK;
}

esp_err_t aw9523b_set_brightness(aw9523b_handle_t dev, uint8_t pin, uint8_t brightness)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "kein Handle");
    ESP_RETURN_ON_FALSE(pin <= AW9523B_PIN_MAX, ESP_ERR_INVALID_ARG, TAG, "Pin %u ungültig", pin);
    return aw9523b_write_reg(dev, dim_reg_for_pin(pin), brightness);
}

esp_err_t aw9523b_set_interrupt_enable(aw9523b_handle_t dev, uint8_t pin, bool enable)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "kein Handle");
    ESP_RETURN_ON_FALSE(pin <= AW9523B_PIN_MAX, ESP_ERR_INVALID_ARG, TAG, "Pin %u ungültig", pin);

    const uint8_t port = pin / 8;
    const uint8_t bit = pin % 8;
    /* 1 = maskiert, 0 = freigegeben */
    uint8_t next = dev->int_shadow[port];
    next = enable ? (next & ~(1u << bit)) : (next | (1u << bit));

    ESP_RETURN_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_INT_P0 + port, next), TAG, "INT");
    dev->int_shadow[port] = next;
    return ESP_OK;
}

esp_err_t aw9523b_set_interrupt_mask(aw9523b_handle_t dev, uint16_t enable_mask)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "kein Handle");
    const uint8_t p0 = ~(enable_mask & 0xFF);
    const uint8_t p1 = ~(enable_mask >> 8);
    ESP_RETURN_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_INT_P0, p0), TAG, "INT_P0");
    ESP_RETURN_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_INT_P1, p1), TAG, "INT_P1");
    dev->int_shadow[0] = p0;
    dev->int_shadow[1] = p1;
    return ESP_OK;
}

esp_err_t aw9523b_set_p0_push_pull(aw9523b_handle_t dev, bool push_pull)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "kein Handle");
    uint8_t next = push_pull ? (dev->gcr_shadow | AW9523B_GCR_GPOMD)
                             : (dev->gcr_shadow & ~AW9523B_GCR_GPOMD);
    ESP_RETURN_ON_ERROR(aw9523b_write_reg(dev, AW9523B_REG_GCR, next), TAG, "GCR");
    dev->gcr_shadow = next;
    return ESP_OK;
}

esp_err_t aw9523b_register_isr(aw9523b_handle_t dev, gpio_isr_t isr_handler, void *arg)
{
    ESP_RETURN_ON_FALSE(dev && isr_handler, ESP_ERR_INVALID_ARG, TAG, "ungültige Argumente");
    ESP_RETURN_ON_FALSE(dev->int_gpio != GPIO_NUM_NC, ESP_ERR_INVALID_STATE, TAG,
                        "kein INT-GPIO konfiguriert");

    esp_err_t ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_RETURN_ON_ERROR(ret, TAG, "ISR-Service");
    }
    return gpio_isr_handler_add(dev->int_gpio, isr_handler, arg);
}
