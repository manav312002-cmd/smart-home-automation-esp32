#include <esp_log.h>
#include <esp_rmaker_core.h>
#include <esp_rmaker_standard_types.h>
#include <esp_rmaker_standard_devices.h>
#include <esp_rmaker_standard_params.h>
#include <app_network.h>
#include "app_devices.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "esp_event.h"
#include "esp_rmaker_ota.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

/*********** Hardware Pins *************/
#define RELAY_1_PIN     39      // Switch A
#define RELAY_2_PIN     38      // Switch B
#define RELAY_3_PIN     40      // Switch C
#define FAN_TRIAC_PIN   37      // Fan TRIAC gate
#define ZCD_PIN         35      // Zero Cross Detect input
#define STATUS_LED_PIN  13      // Status LED (Active HIGH)

#define NODE_NAME "Light Controller Node"
#define NODE_TYPE "Light Module"

/*********** A *************/
#define A_DEVICE_NAME "Light1"
#define A_DEVICE_TYPE "esp.device.switch"

#define A_COB_A_PARAM_NAME "A"
#define A_COB_A_PARAM_TYPE "esp.param.name"
#define A_POWER_PARAM_NAME "Power"
#define A_POWER_PARAM_TYPE "esp.param.power"

/*********** B *************/
#define B_DEVICE_NAME "Light2"
#define B_DEVICE_TYPE "esp.device.switch"

#define B_COB_B_PARAM_NAME "B"
#define B_COB_B_PARAM_TYPE "esp.param.name"
#define B_POWER_PARAM_NAME "Power"
#define B_POWER_PARAM_TYPE "esp.param.power"

/*********** C *************/
#define C_DEVICE_NAME "Light3"
#define C_DEVICE_TYPE "esp.device.switch"

#define C_COB_C_PARAM_NAME "C"
#define C_COB_C_PARAM_TYPE "esp.param.name"
#define C_POWER_PARAM_NAME "Power"
#define C_POWER_PARAM_TYPE "esp.param.power"

/*********** Fan *************/
#define FAN_DEVICE_NAME "Fan"
#define FAN_DEVICE_TYPE "esp.device.fan"

#define FAN_CELLING_PARAM_NAME "Celling"
#define FAN_CELLING_PARAM_TYPE "esp.param.name"
#define FAN_POWER_PARAM_NAME "Power"
#define FAN_POWER_PARAM_TYPE "esp.param.power"
#define FAN_FAN_SPEED_PARAM_NAME "Fan Speed"
#define FAN_FAN_SPEED_PARAM_TYPE "esp.param.speed"

static const char *TAG = "app_devices";

esp_rmaker_device_t *a_device;
esp_rmaker_device_t *b_device;
esp_rmaker_device_t *c_device;
esp_rmaker_device_t *fan_device;

/*********** Fan Control *************/
static int fan_speed = 0;

static const int fan_delay_table[6] = {
    0,      // 0 = off
    8000,   // speed 1 - slowest
    6000,   // speed 2
    4000,   // speed 3
    2000,   // speed 4
    500     // speed 5 - fastest
};

static esp_timer_handle_t triac_timer;

/*****************************************************************************
 * LED State Machine
 *
 *  LED_STATE_BOOTING   → solid ON for 3 sec (board power-on indication)
 *  LED_STATE_WIFI_WAIT → 500ms blink  (waiting for WiFi / credentials)
 *  LED_STATE_CONNECTED → OFF          (all good)
 *  LED_STATE_OTA       → 100ms blink  (OTA download/flash in progress)
 *  LED_STATE_OTA_FAIL  → 3 rapid flashes then OFF
 *****************************************************************************/
typedef enum {
    LED_STATE_BOOTING = 0,
    LED_STATE_WIFI_WAIT,
    LED_STATE_CONNECTED,
    LED_STATE_OTA,
    LED_STATE_OTA_FAIL,
} led_state_t;

static volatile led_state_t led_state = LED_STATE_BOOTING;
static TaskHandle_t led_task_handle = NULL;

static void led_task(void *arg)
{
    /* --- Phase 1: Boot → solid ON for 3 seconds --- */
    ESP_LOGI(TAG, "LED: Boot indication (3s solid ON)");
    gpio_set_level(STATUS_LED_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* --- Phase 2: WiFi wait → blink 500ms until connected --- */
    led_state = LED_STATE_WIFI_WAIT;
    ESP_LOGI(TAG, "LED: Waiting for WiFi (blinking)");

    while (led_state == LED_STATE_WIFI_WAIT) {
        gpio_set_level(STATUS_LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(STATUS_LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* --- Phase 3: Connected → LED OFF (all good) --- */
    gpio_set_level(STATUS_LED_PIN, 0);
    ESP_LOGI(TAG, "LED: Connected — OFF (all good)");

    /* --- Phase 4: Stay alive to handle OTA states --- */
    while (1) {
        if (led_state == LED_STATE_OTA) {
            /* Fast 100ms blink during OTA download/flash */
            gpio_set_level(STATUS_LED_PIN, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(STATUS_LED_PIN, 0);
            vTaskDelay(pdMS_TO_TICKS(100));

        } else if (led_state == LED_STATE_OTA_FAIL) {
            /* 3 rapid flashes then OFF */
            ESP_LOGW(TAG, "LED: OTA failed — 3 flash sequence");
            for (int i = 0; i < 3; i++) {
                gpio_set_level(STATUS_LED_PIN, 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                gpio_set_level(STATUS_LED_PIN, 0);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            gpio_set_level(STATUS_LED_PIN, 0);
            led_state = LED_STATE_CONNECTED;  // Back to idle

        } else {
            /* LED_STATE_CONNECTED — idle, low CPU usage */
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

/* Called from app_main after app_network_start() succeeds */
void app_led_set_connected(void)
{
    led_state = LED_STATE_CONNECTED;
    gpio_set_level(STATUS_LED_PIN, 0);  // OFF = all good
}

/*********** RainMaker Event Handler (OTA only) *************/
static void rmaker_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    if (event_base == RMAKER_OTA_EVENT) {
        if (event_id == RMAKER_OTA_EVENT_STARTING) {
            ESP_LOGI(TAG, "OTA starting → LED fast blink");
            led_state = LED_STATE_OTA;
        } else if (event_id == RMAKER_OTA_EVENT_IN_PROGRESS) {
            ESP_LOGI(TAG, "OTA in progress → LED fast blink");
            led_state = LED_STATE_OTA;
        } else if (event_id == RMAKER_OTA_EVENT_SUCCESSFUL) {
            ESP_LOGI(TAG, "OTA successful — rebooting");
            led_state = LED_STATE_OTA;  // stays blinking until reboot
        } else if (event_id == RMAKER_OTA_EVENT_FAILED) {
            ESP_LOGW(TAG, "OTA failed → 3 flash sequence");
            led_state = LED_STATE_OTA_FAIL;
        }
    }
}

/*********** TRIAC Pulse *************/
static void triac_trigger(void *arg)
{
    gpio_set_level(FAN_TRIAC_PIN, 1);
    esp_rom_delay_us(100);   // 100us gate pulse
    gpio_set_level(FAN_TRIAC_PIN, 0);
}

/*********** Zero Cross ISR *************/
static void IRAM_ATTR zcd_isr_handler(void *arg)
{
    if (fan_speed == 0)
        return;

    esp_timer_stop(triac_timer);
    esp_timer_start_once(triac_timer, fan_delay_table[fan_speed]);
}

/*********** Hardware Init *************/
void app_driver_init(void)
{
    gpio_config_t io_conf = {};

    // Output pins
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pin_bit_mask =
        (1ULL << RELAY_1_PIN)   |
        (1ULL << RELAY_2_PIN)   |
        (1ULL << RELAY_3_PIN)   |
        (1ULL << FAN_TRIAC_PIN) |
        (1ULL << STATUS_LED_PIN);
    gpio_config(&io_conf);

    // Input: ZCD
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.pin_bit_mask = (1ULL << ZCD_PIN);
    gpio_config(&io_conf);

    // Default states — everything OFF, LED task takes control
    gpio_set_level(RELAY_1_PIN, 0);
    gpio_set_level(RELAY_2_PIN, 0);
    gpio_set_level(RELAY_3_PIN, 0);
    gpio_set_level(FAN_TRIAC_PIN, 0);
    gpio_set_level(STATUS_LED_PIN, 0);

    // ZCD interrupt
    gpio_set_intr_type(ZCD_PIN, GPIO_INTR_POSEDGE);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(ZCD_PIN, zcd_isr_handler, NULL);

    // TRIAC timer
    esp_timer_create_args_t timer_args = {
        .callback = &triac_trigger,
        .name = "triac_timer"
    };
    esp_timer_create(&timer_args, &triac_timer);

    // Register for OTA events only
    esp_event_handler_register(RMAKER_OTA_EVENT, ESP_EVENT_ANY_ID,
                                rmaker_event_handler, NULL);

    // Start LED state machine task (priority 5, 2KB stack)
    xTaskCreate(led_task, "led_task", 2048, NULL, 5, &led_task_handle);

    ESP_LOGI(TAG, "Hardware drivers initialized");
    ESP_LOGI(TAG, "Pins → A:%d B:%d C:%d TRIAC:%d ZCD:%d LED:%d",
             RELAY_1_PIN, RELAY_2_PIN, RELAY_3_PIN,
             FAN_TRIAC_PIN, ZCD_PIN, STATUS_LED_PIN);
}

esp_rmaker_node_t *app_device_create_node(esp_rmaker_config_t *config)
{
    esp_rmaker_node_t *node = esp_rmaker_node_init(config, NODE_NAME, NODE_TYPE);
    if (!node) {
        ESP_LOGE(TAG, "Could not initialise node. Aborting!!!");
        return NULL;
    }
    return node;
}

/*********** RainMaker Write Callback *************/
static esp_err_t app_device_bulk_write_cb(const esp_rmaker_device_t *device,
                                           const esp_rmaker_param_write_req_t write_req[],
                                           uint8_t count,
                                           void *priv_data,
                                           esp_rmaker_write_ctx_t *ctx)
{
    if (ctx) {
        ESP_LOGI(TAG, "Received write request via : %s",
                 esp_rmaker_device_cb_src_to_str(ctx->src));
    }

    for (int i = 0; i < count; i++) {
        const esp_rmaker_param_t *param = write_req[i].param;
        esp_rmaker_param_val_t val = write_req[i].val;
        const char *device_name = esp_rmaker_device_get_name(device);
        const char *param_name  = esp_rmaker_param_get_name(param);

        ESP_LOGI(TAG, "Received value for %s - %s", device_name, param_name);

        // -------- Switch A → Relay 1 --------
        if (device == a_device) {
            if (strcmp(param_name, A_POWER_PARAM_NAME) == 0) {
                gpio_set_level(RELAY_1_PIN, val.val.b);
                ESP_LOGI(TAG, "Relay 1 (A) -> %s", val.val.b ? "ON" : "OFF");
            } else if (strcmp(param_name, A_COB_A_PARAM_NAME) == 0) {
                ESP_LOGI(TAG, "COB-A name: %s", val.val.s ? val.val.s : "null");
            } else {
                ESP_LOGW(TAG, "Invalid param: %s", param_name);
            }
        }

        // -------- Switch B → Relay 2 --------
        else if (device == b_device) {
            if (strcmp(param_name, B_POWER_PARAM_NAME) == 0) {
                gpio_set_level(RELAY_2_PIN, val.val.b);
                ESP_LOGI(TAG, "Relay 2 (B) -> %s", val.val.b ? "ON" : "OFF");
            } else if (strcmp(param_name, B_COB_B_PARAM_NAME) == 0) {
                ESP_LOGI(TAG, "COB-B name: %s", val.val.s ? val.val.s : "null");
            } else {
                ESP_LOGW(TAG, "Invalid param: %s", param_name);
            }
        }

        // -------- Switch C → Relay 3 --------
        else if (device == c_device) {
            if (strcmp(param_name, C_POWER_PARAM_NAME) == 0) {
                gpio_set_level(RELAY_3_PIN, val.val.b);
                ESP_LOGI(TAG, "Relay 3 (C) -> %s", val.val.b ? "ON" : "OFF");
            } else if (strcmp(param_name, C_COB_C_PARAM_NAME) == 0) {
                ESP_LOGI(TAG, "COB-C name: %s", val.val.s ? val.val.s : "null");
            } else {
                ESP_LOGW(TAG, "Invalid param: %s", param_name);
            }
        }

        // -------- Fan → TRIAC phase control --------
        else if (device == fan_device) {
            if (strcmp(param_name, FAN_POWER_PARAM_NAME) == 0) {
                if (val.val.b == false) {
                    fan_speed = 0;
                    esp_timer_stop(triac_timer);
                    gpio_set_level(FAN_TRIAC_PIN, 0);
                    ESP_LOGI(TAG, "Fan -> OFF");

                    esp_rmaker_param_t *sp = esp_rmaker_device_get_param_by_name(
                        fan_device, FAN_FAN_SPEED_PARAM_NAME);
                    if (sp) esp_rmaker_param_update_and_report(sp, esp_rmaker_int(0));

                } else {
                    if (fan_speed == 0) {
                        fan_speed = 5;
                        ESP_LOGI(TAG, "Fan -> ON (speed 5)");

                        esp_rmaker_param_t *sp = esp_rmaker_device_get_param_by_name(
                            fan_device, FAN_FAN_SPEED_PARAM_NAME);
                        if (sp) esp_rmaker_param_update_and_report(sp, esp_rmaker_int(5));
                    }
                }

            } else if (strcmp(param_name, FAN_FAN_SPEED_PARAM_NAME) == 0) {
                fan_speed = val.val.i;
                if (fan_speed < 0) fan_speed = 0;
                if (fan_speed > 5) fan_speed = 5;

                if (fan_speed == 0) {
                    esp_timer_stop(triac_timer);
                    gpio_set_level(FAN_TRIAC_PIN, 0);
                }
                ESP_LOGI(TAG, "Fan speed -> %d", fan_speed);

            } else if (strcmp(param_name, FAN_CELLING_PARAM_NAME) == 0) {
                ESP_LOGI(TAG, "Fan name: %s", val.val.s ? val.val.s : "null");
            } else {
                ESP_LOGW(TAG, "Invalid param: %s", param_name);
            }
        }

        else {
            ESP_LOGW(TAG, "Invalid device");
        }

        esp_rmaker_param_update(param, val);
    }

    return ESP_OK;
}

/*********** Create RainMaker Devices *************/
esp_rmaker_device_t *app_device_create(esp_rmaker_node_t *node)
{
    esp_rmaker_device_t *device = NULL;

    a_device = esp_rmaker_switch_device_create(A_DEVICE_NAME, NULL, false);
    if (!a_device) { ESP_LOGE(TAG, "Failed to create %s", A_DEVICE_NAME); return NULL; }
    esp_rmaker_device_add_bulk_cb(a_device, app_device_bulk_write_cb, NULL);
    esp_rmaker_node_add_device(node, a_device);

    b_device = esp_rmaker_switch_device_create(B_DEVICE_NAME, NULL, false);
    if (!b_device) { ESP_LOGE(TAG, "Failed to create %s", B_DEVICE_NAME); return NULL; }
    esp_rmaker_device_add_bulk_cb(b_device, app_device_bulk_write_cb, NULL);
    esp_rmaker_node_add_device(node, b_device);

    c_device = esp_rmaker_switch_device_create(C_DEVICE_NAME, NULL, false);
    if (!c_device) { ESP_LOGE(TAG, "Failed to create %s", C_DEVICE_NAME); return NULL; }
    esp_rmaker_device_add_bulk_cb(c_device, app_device_bulk_write_cb, NULL);
    esp_rmaker_node_add_device(node, c_device);

    fan_device = esp_rmaker_fan_device_create(FAN_DEVICE_NAME, NULL, false);
    if (!fan_device) { ESP_LOGE(TAG, "Failed to create %s", FAN_DEVICE_NAME); return NULL; }
    esp_rmaker_device_add_bulk_cb(fan_device, app_device_bulk_write_cb, NULL);
    esp_rmaker_device_add_param(fan_device,
        esp_rmaker_speed_param_create(FAN_FAN_SPEED_PARAM_NAME, 0));
    esp_rmaker_node_add_device(node, fan_device);

    ESP_LOGI(TAG, "All devices created successfully");
    return device;
}

esp_err_t app_device_set_mfg_data(void)
{
    ESP_LOGW(TAG, "Manufacturing data not implemented");
    return ESP_OK;
}