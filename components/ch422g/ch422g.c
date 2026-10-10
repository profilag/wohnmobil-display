#include "ch422g.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "ch422g";

#define CH422G_I2C_TIMEOUT_MS 100

struct ch422g_t {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev_set;  /* 0x24 */
    i2c_master_dev_handle_t dev_oc;   /* 0x23 */
    i2c_master_dev_handle_t dev_io;   /* 0x38 */
    i2c_master_dev_handle_t dev_rd;   /* 0x26 */
    uint8_t set_shadow;               /* letzter WR_SET-Wert   */
    uint8_t io_shadow;                /* letzter IO0..IO7-Wert */
    uint8_t oc_shadow;                /* letzter OC0..OC3-Wert */
};

static esp_err_t add_dev(i2c_master_bus_handle_t bus, uint16_t addr, i2c_master_dev_handle_t *out)
{
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    return i2c_master_bus_add_device(bus, &cfg, out);
}

static esp_err_t write_byte(i2c_master_dev_handle_t dev, uint8_t value)
{
    return i2c_master_transmit(dev, &value, 1, CH422G_I2C_TIMEOUT_MS);
}

esp_err_t ch422g_new(i2c_master_bus_handle_t bus, ch422g_handle_t *out_dev)
{
    ESP_RETURN_ON_FALSE(bus && out_dev, ESP_ERR_INVALID_ARG, TAG, "ungültige Argumente");

    struct ch422g_t *dev = calloc(1, sizeof(struct ch422g_t));
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_NO_MEM, TAG, "kein Speicher");
    dev->bus = bus;

    esp_err_t ret = ESP_OK;
    ESP_GOTO_ON_ERROR(add_dev(bus, CH422G_ADDR_WR_SET, &dev->dev_set), err, TAG, "WR_SET");
    ESP_GOTO_ON_ERROR(add_dev(bus, CH422G_ADDR_WR_OC, &dev->dev_oc), err, TAG, "WR_OC");
    ESP_GOTO_ON_ERROR(add_dev(bus, CH422G_ADDR_WR_IO, &dev->dev_io), err, TAG, "WR_IO");
    ESP_GOTO_ON_ERROR(add_dev(bus, CH422G_ADDR_RD_IO, &dev->dev_rd), err, TAG, "RD_IO");

    /* IO0..IO7 als Ausgang, OC-Ausgänge im Push-Pull-Betrieb. */
    dev->set_shadow = CH422G_WR_SET_IO_OE;
    ESP_GOTO_ON_ERROR(write_byte(dev->dev_set, dev->set_shadow), err, TAG,
                      "CH422G antwortet nicht (WR_SET)");

    /* Definierter Startzustand: alles low, Backlight aus, Resets aktiv. */
    dev->io_shadow = 0x00;
    dev->oc_shadow = 0x00;
    ESP_GOTO_ON_ERROR(write_byte(dev->dev_io, dev->io_shadow), err, TAG, "WR_IO");
    ESP_GOTO_ON_ERROR(write_byte(dev->dev_oc, dev->oc_shadow), err, TAG, "WR_OC");

    *out_dev = dev;
    ESP_LOGI(TAG, "CH422G initialisiert (IO0..IO7 = Ausgang)");
    return ESP_OK;

err:
    ch422g_del(dev);
    return ret;
}

esp_err_t ch422g_del(ch422g_handle_t dev)
{
    if (!dev) {
        return ESP_OK;
    }
    if (dev->dev_set) i2c_master_bus_rm_device(dev->dev_set);
    if (dev->dev_oc)  i2c_master_bus_rm_device(dev->dev_oc);
    if (dev->dev_io)  i2c_master_bus_rm_device(dev->dev_io);
    if (dev->dev_rd)  i2c_master_bus_rm_device(dev->dev_rd);
    free(dev);
    return ESP_OK;
}

esp_err_t ch422g_set_level(ch422g_handle_t dev, ch422g_pin_t pin, bool level)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "kein Handle");
    ESP_RETURN_ON_FALSE(pin <= CH422G_PIN_OC3, ESP_ERR_INVALID_ARG, TAG, "Pin %d ungültig", pin);

    if (pin <= CH422G_PIN_IO7) {
        uint8_t next = dev->io_shadow;
        next = level ? (next | (1u << pin)) : (next & ~(1u << pin));
        ESP_RETURN_ON_ERROR(write_byte(dev->dev_io, next), TAG, "WR_IO fehlgeschlagen");
        dev->io_shadow = next;
    } else {
        const uint8_t bit = pin - CH422G_PIN_OC0;
        uint8_t next = dev->oc_shadow;
        next = level ? (next | (1u << bit)) : (next & ~(1u << bit));
        ESP_RETURN_ON_ERROR(write_byte(dev->dev_oc, next), TAG, "WR_OC fehlgeschlagen");
        dev->oc_shadow = next;
    }
    return ESP_OK;
}

esp_err_t ch422g_write_io(ch422g_handle_t dev, uint8_t value)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "kein Handle");
    ESP_RETURN_ON_ERROR(write_byte(dev->dev_io, value), TAG, "WR_IO fehlgeschlagen");
    dev->io_shadow = value;
    return ESP_OK;
}

esp_err_t ch422g_write_oc(ch422g_handle_t dev, uint8_t value)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "kein Handle");
    value &= 0x0F;
    ESP_RETURN_ON_ERROR(write_byte(dev->dev_oc, value), TAG, "WR_OC fehlgeschlagen");
    dev->oc_shadow = value;
    return ESP_OK;
}

esp_err_t ch422g_write_set(ch422g_handle_t dev, uint8_t value)
{
    ESP_RETURN_ON_FALSE(dev, ESP_ERR_INVALID_ARG, TAG, "kein Handle");
    ESP_RETURN_ON_ERROR(write_byte(dev->dev_set, value), TAG, "WR_SET fehlgeschlagen");
    dev->set_shadow = value;
    return ESP_OK;
}

esp_err_t ch422g_read_io(ch422g_handle_t dev, uint8_t *out_value)
{
    ESP_RETURN_ON_FALSE(dev && out_value, ESP_ERR_INVALID_ARG, TAG, "ungültige Argumente");

    const uint8_t set_input = dev->set_shadow & ~CH422G_WR_SET_IO_OE;
    ESP_RETURN_ON_ERROR(write_byte(dev->dev_set, set_input), TAG, "WR_SET fehlgeschlagen");

    esp_err_t ret = i2c_master_receive(dev->dev_rd, out_value, 1, CH422G_I2C_TIMEOUT_MS);

    /* Ausgangsmodus und letzten Ausgangszustand wiederherstellen. */
    esp_err_t restore = write_byte(dev->dev_set, dev->set_shadow);
    if (restore == ESP_OK) {
        restore = write_byte(dev->dev_io, dev->io_shadow);
    }
    return (ret != ESP_OK) ? ret : restore;
}
