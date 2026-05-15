#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/gptimer.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

#define UART_PORT           UART_NUM_1
#define UART_TX_PIN         GPIO_NUM_2
#define UART_RX_PIN         GPIO_NUM_3
#define UART_BUF_SIZE       256
#define UART_BAUD_RATE      921600

#define ADC_UNIT_USED       ADC_UNIT_1
#define ADC_CHAN_USED       ADC_CHANNEL_0   // ESP32-C6: ADC1_CH0 = GPIO0
#define ADC_ATTEN_USED      ADC_ATTEN_DB_12
#define ADC_BITWIDTH_USED   ADC_BITWIDTH_12

// Cambia esto a 100 o 10
#define SAMPLE_PERIOD_US    200

static const char *TAG = "ADC_UART1";

static adc_oneshot_unit_handle_t adc_handle = NULL;
static adc_cali_handle_t cali_handle = NULL;
static bool cali_enabled = false;

static gptimer_handle_t sample_timer = NULL;
static TaskHandle_t sample_task_handle = NULL;

static void uart1_init(void)
{
    uart_config_t cfg = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_BUF_SIZE, UART_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT,
                                 UART_TX_PIN,
                                 UART_RX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
}

static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t init_cfg = {
        .unit_id = ADC_UNIT_USED,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_cfg, &adc_handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .bitwidth = ADC_BITWIDTH_USED,
        .atten = ADC_ATTEN_USED,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, ADC_CHAN_USED, &chan_cfg));

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_USED,
        .chan = ADC_CHAN_USED,
        .atten = ADC_ATTEN_USED,
        .bitwidth = ADC_BITWIDTH_USED,
    };

    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle) == ESP_OK) {
        cali_enabled = true;
    } else {
        ESP_LOGW(TAG, "Calibración no disponible; usaré aproximación");
    }
#endif
}

static bool IRAM_ATTR sample_timer_cb(gptimer_handle_t timer,
                                      const gptimer_alarm_event_data_t *edata,
                                      void *user_ctx)
{
    BaseType_t high_task_woken = pdFALSE;
    TaskHandle_t task = (TaskHandle_t)user_ctx;

    vTaskNotifyGiveFromISR(task, &high_task_woken);
    return (high_task_woken == pdTRUE);
}

static void timer_init(TaskHandle_t task_handle)
{
    gptimer_config_t timer_config = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,   // 1 tick = 1 us
    };
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &sample_timer));

    gptimer_event_callbacks_t cbs = {
        .on_alarm = sample_timer_cb,
    };
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(sample_timer, &cbs, task_handle));

    gptimer_alarm_config_t alarm_config = {
        .reload_count = 0,
        .alarm_count = SAMPLE_PERIOD_US,
        .flags.auto_reload_on_alarm = true,
    };
    ESP_ERROR_CHECK(gptimer_set_alarm_action(sample_timer, &alarm_config));

    ESP_ERROR_CHECK(gptimer_enable(sample_timer));
    ESP_ERROR_CHECK(gptimer_start(sample_timer));
}

static void adc_uart_task(void *arg)
{
    char line[32];

    while (1) {
        // Espera a que el timer dispare una muestra
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        int raw = 0;
        int mv = 0;

        esp_err_t err = adc_oneshot_read(adc_handle, ADC_CHAN_USED, &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ADC read error: %s", esp_err_to_name(err));
            continue;
        }

        if (cali_enabled && cali_handle) {
            if (adc_cali_raw_to_voltage(cali_handle, raw, &mv) != ESP_OK) {
                mv = (raw * 1100) / 4095;
            }
        } else {
            mv = (raw * 1100) / 4095;
        }

        int n = snprintf(line, sizeof(line), "{\"v\":%d}\n", mv);
        uart_write_bytes(UART_PORT, line, n);
    }
}

void app_main(void)
{
    uart1_init();
    adc_init();

    xTaskCreate(adc_uart_task, "adc_uart_task", 4096, NULL, 5, &sample_task_handle);
    timer_init(sample_task_handle);
}