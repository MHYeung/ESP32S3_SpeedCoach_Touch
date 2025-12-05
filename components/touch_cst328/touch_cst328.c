#include "touch_cst328.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

static const char *TAG = "cst328";

#define CST328_I2C_ADDR_7BIT   0x1A      // 0x34/0x35 8-bit
#define CST328_BASE_REG        0xD000    // first finger data

// new-driver handles
static i2c_master_bus_handle_t  s_i2c_bus   = NULL;
static i2c_master_dev_handle_t  s_i2c_dev   = NULL;

static gpio_num_t s_rst_gpio = -1;
static gpio_num_t s_irq_gpio = -1;

static esp_err_t cst328_read_regs(uint16_t reg16, uint8_t *buf, size_t len)
{
    uint8_t reg[2] = { (uint8_t)(reg16 >> 8), (uint8_t)(reg16 & 0xFF) };
    return i2c_master_transmit_receive(s_i2c_dev,
                                       reg, sizeof(reg),
                                       buf, len,
                                       -1);
}

esp_err_t cst328_init(i2c_port_t port,
                      gpio_num_t sda,
                      gpio_num_t scl,
                      gpio_num_t rst,
                      gpio_num_t irq,
                      uint32_t i2c_clk_hz)
{
    s_rst_gpio = rst;
    s_irq_gpio = irq;

    // 1) Create I2C master bus (new API)
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = port,
        .scl_io_num = scl,
        .sda_io_num = sda,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    // 2) Add CST328 device
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = CST328_I2C_ADDR_7BIT,
        .scl_speed_hz    = i2c_clk_hz,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_i2c_dev));

    // 3) Reset pin (active low)
    if (rst >= 0) {
        gpio_config_t io = {
            .pin_bit_mask = 1ULL << rst,
            .mode = GPIO_MODE_OUTPUT,
        };
        ESP_ERROR_CHECK(gpio_config(&io));
        gpio_set_level(rst, 0);
        vTaskDelay(pdMS_TO_TICKS(5));
        gpio_set_level(rst, 1);
    }

    // 4) IRQ pin as input (if used)
    if (irq >= 0) {
        gpio_config_t io_irq = {
            .pin_bit_mask = 1ULL << irq,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&io_irq));
    }

    // Give controller time to boot (~200 ms)
    vTaskDelay(pdMS_TO_TICKS(200));

    ESP_LOGI(TAG, "CST328 init OK on port %d", port);
    return ESP_OK;
}

esp_err_t cst328_read_point(cst328_point_t *out_pt)
{
    if (!out_pt) return ESP_ERR_INVALID_ARG;

    uint8_t buf[7] = {0};
    esp_err_t err = cst328_read_regs(CST328_BASE_REG, buf, sizeof(buf));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "I2C read failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t id_status = buf[0];
    uint8_t xh        = buf[1];
    uint8_t yh        = buf[2];
    uint8_t xy_low    = buf[3];
    uint8_t pressure  = buf[4];
    // buf[5] flags, buf[6] fixed pattern, can be ignored here

    // Status 0x06 = “press” (from CST328 datasheet)
    bool pressed = ((id_status & 0x0F) == 0x06);

    uint16_t x = ((uint16_t)xh << 4) | (xy_low >> 4);
    uint16_t y = ((uint16_t)yh << 4) | (xy_low & 0x0F);

    out_pt->x = x;
    out_pt->y = y;
    out_pt->pressure = pressure;
    out_pt->pressed = pressed;

    return ESP_OK;
}
