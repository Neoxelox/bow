#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_intr_alloc.h"
#include "gpio.hpp"

namespace gpio
{
    Gpio *Gpio::New(gpio_num_t pin, gpio_mode_t mode)
    {
        Gpio *gpio = new Gpio();

        ESP_ERROR_CHECK(!GPIO_IS_VALID_GPIO(pin));

        if (mode >= GPIO_MODE_OUTPUT)
            ESP_ERROR_CHECK(!GPIO_IS_VALID_OUTPUT_GPIO(pin));

        ESP_ERROR_CHECK(gpio_reset_pin(pin));

        gpio->Pin = pin;
        gpio->Mode = mode;

        const gpio_config_t cfg = {
            .pin_bit_mask = 1ull << gpio->Pin,
            .mode = gpio->Mode,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };

        ESP_ERROR_CHECK(gpio_config(&cfg));

        return gpio;
    }

    void Gpio::DigitalWrite(int level)
    {
        ESP_ERROR_CHECK(gpio_set_level(this->Pin, level));
    }

    int Gpio::DigitalRead()
    {
        return gpio_get_level(this->Pin);
    }

    void Gpio::AttachPullResistor(gpio_pull_mode_t pull)
    {
        ESP_ERROR_CHECK(gpio_set_pull_mode(this->Pin, pull));
    }

    void Gpio::DetachPullResistor()
    {
        ESP_ERROR_CHECK(gpio_set_pull_mode(this->Pin, GPIO_FLOATING));
    }

    void Gpio::AttachInterrupt(gpio_int_type_t type, gpio_isr_t handler, void *args)
    {
        esp_err_t ret = gpio_install_isr_service(ESP_INTR_FLAG_EDGE);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE)
            ESP_ERROR_CHECK(ret);

        ESP_ERROR_CHECK(gpio_set_intr_type(this->Pin, type));
        ESP_ERROR_CHECK(gpio_isr_handler_add(this->Pin, handler, args));
        ESP_ERROR_CHECK(gpio_intr_enable(this->Pin));
    }

    void Gpio::DetachInterrupt()
    {
        ESP_ERROR_CHECK(gpio_intr_disable(this->Pin));
        ESP_ERROR_CHECK(gpio_isr_handler_remove(this->Pin));
        ESP_ERROR_CHECK(gpio_set_intr_type(this->Pin, GPIO_INTR_DISABLE));
    }
}