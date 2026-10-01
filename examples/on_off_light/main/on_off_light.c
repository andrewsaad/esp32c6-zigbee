/*
 * zb_router example: HA On/Off Light router.
 *
 * SPDX-License-Identifier: MIT
 *
 * One HA-standard On/Off Light endpoint whose OnOff attribute drives a GPIO
 * (an LED by default). Everything Zigbee-generic - radio, antenna switch,
 * NVS, router config, commissioning, steering retry, network selection - is
 * done by the zb_router component; this file only creates the endpoint and
 * reacts to attribute writes.
 *
 * Endpoint/action handling follows the esp-zigbee-sdk HA_on_off_light example
 * (release/v1.0, esp-zigbee-lib ~1.6.x).
 */

#include "esp_check.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "zb_router.h"

static const char *TAG = "ON_OFF_LIGHT";

#define LIGHT_ENDPOINT          10
#define LIGHT_GPIO              CONFIG_EXAMPLE_LIGHT_GPIO
#if CONFIG_EXAMPLE_LIGHT_ACTIVE_LOW
#define LIGHT_ACTIVE_LOW        1
#else
#define LIGHT_ACTIVE_LOW        0
#endif
#define LIGHT_LEVEL(on)         ((on) ? !LIGHT_ACTIVE_LOW : LIGHT_ACTIVE_LOW)

/* Basic cluster identity. ZCL character strings are length-prefixed:
 *   first byte = string length, then the characters.
 *   "esp32c6-zigbee" = 14 chars -> \x0e
 *   "OnOffLight"     = 10 chars -> \x0a */
#define MANUFACTURER_NAME       "\x0e""esp32c6-zigbee"
#define MODEL_IDENTIFIER        "\x0a""OnOffLight"

/* ================================================================== *
 *  Light output
 * ================================================================== */
static void light_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LIGHT_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(LIGHT_GPIO, LIGHT_LEVEL(false));    /* start OFF */
    ESP_LOGI(TAG, "Light on GPIO%d (%s)", LIGHT_GPIO,
             LIGHT_ACTIVE_LOW ? "active-low" : "active-high");
}

static void light_set(bool on)
{
    gpio_set_level(LIGHT_GPIO, LIGHT_LEVEL(on));
    ESP_LOGI(TAG, "Light %s", on ? "ON" : "OFF");
}

/* ================================================================== *
 *  Zigbee: endpoint + action handler (run on the Zigbee task)
 * ================================================================== */
static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *msg)
{
    ESP_RETURN_ON_FALSE(msg, ESP_FAIL, TAG, "Empty message");
    ESP_RETURN_ON_FALSE(msg->info.status == ESP_ZB_ZCL_STATUS_SUCCESS, ESP_ERR_INVALID_ARG, TAG,
                        "Received message: error status(%d)", msg->info.status);

    if (msg->info.dst_endpoint == LIGHT_ENDPOINT &&
        msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
        msg->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
        msg->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL &&
        msg->attribute.data.value) {
        light_set(*(bool *)msg->attribute.data.value);
    }
    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    switch (callback_id) {
    case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
        return zb_attribute_handler((const esp_zb_zcl_set_attr_value_message_t *)message);
    default:
        ESP_LOGI(TAG, "Unhandled Zigbee action callback 0x%x", callback_id);
        return ESP_OK;
    }
}

/* zb_router_register_eps_cb_t: build the endpoint list and register it. */
static void light_register_eps(void)
{
    esp_zb_on_off_light_cfg_t light_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    esp_zb_ep_list_t *ep_list = esp_zb_on_off_light_ep_create(LIGHT_ENDPOINT, &light_cfg);

    /* Set Basic ManufacturerName / ModelIdentifier.
     * Pattern lifted from the SDK's zcl_utility helper. */
    esp_zb_cluster_list_t *cluster_list = esp_zb_ep_list_get_ep(ep_list, LIGHT_ENDPOINT);
    esp_zb_attribute_list_t *basic_cluster = esp_zb_cluster_list_get_cluster(
            cluster_list, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(
            basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)MANUFACTURER_NAME));
    ESP_ERROR_CHECK(esp_zb_basic_cluster_add_attr(
            basic_cluster, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)MODEL_IDENTIFIER));

    esp_zb_device_register(ep_list);
    esp_zb_core_action_handler_register(zb_action_handler);
}

/* zb_router_joined_cb_t: on the network (fresh join or reboot). */
static void light_on_joined(void)
{
    ESP_LOGI(TAG, "On the network, ready for On/Off commands");
}

void app_main(void)
{
    light_init();

    zb_router_set_joined_cb(light_on_joined);
    ESP_ERROR_CHECK(zb_router_start(light_register_eps));
}
