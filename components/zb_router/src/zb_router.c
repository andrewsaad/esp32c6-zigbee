/*
 * zb_router - generic Zigbee ROUTER boilerplate for ESP-IDF.
 *
 * SPDX-License-Identifier: MIT
 *
 * -----------------------------------------------------------------------
 * API SOURCE / GROUNDING
 *   Adapted from esp-zigbee-sdk example HA_on_off_light at
 *   release/v1.0 @ commit c9e2c3e12642c704096dfefe25b62212b47229ba
 *   (esp-zigbee-lib ~1.6.x; 1.6.8 latest on that branch, 5-Nov-2025),
 *   plus the coordinator/router config from HA_on_off_switch
 *   (ESP_ZB_ZC_CONFIG), switched to the ROUTER role.
 *
 *   NOTE: esp-zigbee-lib 2.0.x (main branch) is a wholesale API rewrite to
 *   an ezb_* namespace. This component deliberately targets the mature 1.6.x
 *   classic esp_zb_* API. idf_component.yml pins ~1.6.0 to match. See README.
 * -----------------------------------------------------------------------
 */

#include "zb_router.h"
#include "zb_router_epid.h"

#include <stdbool.h>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ZB_ROUTER";

/* ------------------------------------------------------------------ *
 *  Network selection (Kconfig, see ../Kconfig)
 *
 * Channel: 0 = steer on all channels (11-26), else steer ONLY on that one
 * channel. With the all-channels mask a bench boot can silently join a
 * random foreign (neighbor's) network that happens to permit joining, then
 * stop steering for good. If your coordinator ever migrates channels,
 * update the option and rebuild.
 * ------------------------------------------------------------------ */
#if CONFIG_ZB_ROUTER_CHANNEL == 0
#define ZB_ROUTER_CHANNEL_MASK  ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK
#elif CONFIG_ZB_ROUTER_CHANNEL >= 11 && CONFIG_ZB_ROUTER_CHANNEL <= 26
#define ZB_ROUTER_CHANNEL_MASK  (1UL << CONFIG_ZB_ROUTER_CHANNEL)
#else
#error "CONFIG_ZB_ROUTER_CHANNEL must be 0 (all channels) or 11-26"
#endif

/* Router (ZC/ZR) device config. Mirrors the SDK ESP_ZB_ZC_CONFIG() macro
 * but with the ROUTER role: mains-powered, always on, extends the mesh. */
#define ZB_ROUTER_INSTALLCODE_POLICY    false
#define ZB_ROUTER_NWK_CONFIG()                                  \
    {                                                           \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,               \
        .install_code_policy = ZB_ROUTER_INSTALLCODE_POLICY,    \
        .nwk_cfg.zczr_cfg = {                                   \
            .max_children = CONFIG_ZB_ROUTER_MAX_CHILDREN,      \
        },                                                      \
    }

#define ZB_ROUTER_RADIO_CONFIG()                                \
    {                                                           \
        .radio_mode = ZB_RADIO_MODE_NATIVE,                     \
    }

#define ZB_ROUTER_HOST_CONFIG()                                 \
    {                                                           \
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,   \
    }

/* EPID bytes are little-endian (LSB first); print MSB first as ZHA does. */
#define EPID_FMT        "%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x"
#define EPID_ARGS(e)    (e)[7], (e)[6], (e)[5], (e)[4], (e)[3], (e)[2], (e)[1], (e)[0]

static zb_router_register_eps_cb_t s_register_eps;
static zb_router_signal_hook_t     s_signal_hook;
static zb_router_joined_cb_t       s_joined_cb;
static esp_zb_ieee_addr_t          s_epid;      /* parsed CONFIG_ZB_ROUTER_EXT_PAN_ID */
static volatile bool               s_started;
static volatile bool               s_joined;

/* ================================================================== *
 *  Antenna init  (must run BEFORE esp_zb_platform_config / radio)
 *
 *  Seeed XIAO ESP32-C6 shared RF front end (FM8625H RF switch):
 *    GPIO3  = RF_SWITCH_EN : drive LOW first to enable the RF switch.
 *    GPIO14 = ANT_SELECT   : LOW = built-in ceramic, HIGH = external u.FL.
 * ================================================================== */
#if CONFIG_ZB_ROUTER_ANTENNA_XIAO_ONBOARD || CONFIG_ZB_ROUTER_ANTENNA_XIAO_EXTERNAL
#define XIAO_RF_SWITCH_EN_GPIO  3
#define XIAO_ANT_SELECT_GPIO    14
#define XIAO_ANT_SETTLE_MS      100
#if CONFIG_ZB_ROUTER_ANTENNA_XIAO_EXTERNAL
#define XIAO_ANT_EXTERNAL       1
#else
#define XIAO_ANT_EXTERNAL       0
#endif

static esp_err_t antenna_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << XIAO_RF_SWITCH_EN_GPIO) | (1ULL << XIAO_ANT_SELECT_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "antenna GPIO config failed");

    /* Enable the RF switch first, let it settle, then select the antenna. */
    gpio_set_level(XIAO_RF_SWITCH_EN_GPIO, 0);          /* LOW = enable RF switch */
    vTaskDelay(pdMS_TO_TICKS(XIAO_ANT_SETTLE_MS));
    gpio_set_level(XIAO_ANT_SELECT_GPIO, XIAO_ANT_EXTERNAL);
    ESP_LOGI(TAG, "Antenna: %s", XIAO_ANT_EXTERNAL ? "external u.FL" : "built-in ceramic");
    return ESP_OK;
}
#else
static esp_err_t antenna_init(void)
{
    return ESP_OK;  /* no RF switch configured */
}
#endif

/* ================================================================== *
 *  Commissioning / signal handling
 * ================================================================== */
static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    ESP_RETURN_ON_FALSE(esp_zb_bdb_start_top_level_commissioning(mode_mask) == ESP_OK, , TAG,
                        "Failed to start Zigbee commissioning");
}

static void retry_commissioning(uint8_t mode_mask)
{
    esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                           mode_mask, CONFIG_ZB_ROUTER_RETRY_DELAY_MS);
}

/* Log the network we are on, then notify the app. Check the EPID against
 * your coordinator's: without an EPID filter this may be a neighbor's. */
static void on_network(const char *what)
{
    esp_zb_ieee_addr_t epid;
    esp_zb_get_extended_pan_id(epid);
    ESP_LOGI(TAG, "%s (EPID " EPID_FMT ", PAN 0x%04hx, Ch %d, Short 0x%04hx)",
             what, EPID_ARGS(epid), esp_zb_get_pan_id(), esp_zb_get_current_channel(),
             esp_zb_get_short_address());

    s_joined = true;
    if (s_joined_cb) {
        s_joined_cb();
    }
}

/* The SDK requires the application to define this; the stack calls it for
 * every app signal. Apps hook in via zb_router_set_signal_hook(). */
void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t                 *p_sg_p     = signal_struct->p_app_signal;
    esp_err_t                 err_status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t  sig_type   = *p_sg_p;

    switch (sig_type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        ESP_LOGI(TAG, "Initialize Zigbee stack");
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (err_status == ESP_OK) {
            ESP_LOGI(TAG, "Device started up in%s factory-reset mode",
                     esp_zb_bdb_is_factory_new() ? "" : " non");
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Start network steering");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                on_network("Device rebooted, already on a network");
            }
        } else {
            ESP_LOGW(TAG, "%s failed with status: %s, retrying",
                     esp_zb_zdo_signal_to_string(sig_type), esp_err_to_name(err_status));
            retry_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (err_status == ESP_OK) {
            on_network("Joined network");
        } else {
            ESP_LOGI(TAG, "Network steering failed (status: %s), retrying",
                     esp_err_to_name(err_status));
            retry_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        }
        break;
    case ESP_ZB_ZDO_SIGNAL_LEAVE: {
        /* Removed by the coordinator or a local reset. A leave WITH rejoin
         * (e.g. the coordinator moving us) steers again right away; a plain
         * leave stays off the network until the next boot steers it back
         * (or the app restarts steering from its signal hook). */
        const esp_zb_zdo_signal_leave_params_t *lp =
            (const esp_zb_zdo_signal_leave_params_t *)esp_zb_app_signal_get_params(p_sg_p);
        bool rejoin = lp && lp->leave_type == ESP_ZB_NWK_LEAVE_TYPE_REJOIN;
        s_joined = false;
        ESP_LOGW(TAG, "Left the network (%s)", rejoin ? "rejoin requested, steering again" : "reset");
        if (rejoin) {
            retry_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
        }
        break;
    }
    default:
        ESP_LOGI(TAG, "ZDO signal: %s (0x%x), status: %s",
                 esp_zb_zdo_signal_to_string(sig_type), sig_type, esp_err_to_name(err_status));
        break;
    }

    if (s_signal_hook) {
        s_signal_hook(signal_struct);
    }
}

/* ================================================================== *
 *  Zigbee task + public API
 * ================================================================== */

/* Restrict steering to the configured network (see CONFIG_ZB_ROUTER_EXT_PAN_ID).
 * All-zero leaves the filter unset = join any network. Channel pinning alone
 * proved insufficient on the bench: a neighbor's network on the same channel
 * kept winning steering; esp_zb_set_extended_pan_id() makes BDB steering
 * associate only with the matching network. */
static void ext_pan_id_apply(void)
{
    if (zb_router_epid_is_zero(s_epid)) {
        ESP_LOGW(TAG, "No extended PAN ID set: will join ANY network that permits joining");
        return;
    }
    esp_zb_set_extended_pan_id(s_epid);
    ESP_LOGI(TAG, "Steering restricted to EPID " EPID_FMT, EPID_ARGS(s_epid));
}

static void zb_router_task(void *pvParameters)
{
    esp_zb_cfg_t zb_nwk_cfg = ZB_ROUTER_NWK_CONFIG();  /* ROUTER: mains-powered, extends mesh */
    esp_zb_init(&zb_nwk_cfg);

    s_register_eps();

    esp_zb_set_primary_network_channel_set(ZB_ROUTER_CHANNEL_MASK);

    /* Associate only with your network, if configured. */
    ext_pan_id_apply();

    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

void zb_router_set_signal_hook(zb_router_signal_hook_t hook)
{
    s_signal_hook = hook;
}

void zb_router_set_joined_cb(zb_router_joined_cb_t cb)
{
    s_joined_cb = cb;
}

bool zb_router_is_joined(void)
{
    return s_joined;
}

esp_err_t zb_router_start(zb_router_register_eps_cb_t register_eps)
{
    ESP_RETURN_ON_FALSE(register_eps, ESP_ERR_INVALID_ARG, TAG, "register_eps is NULL");
    ESP_RETURN_ON_FALSE(!s_started, ESP_ERR_INVALID_STATE, TAG, "already started");

    /* Fail loudly on a malformed EPID instead of silently joining anything. */
    ESP_RETURN_ON_FALSE(zb_router_epid_parse(CONFIG_ZB_ROUTER_EXT_PAN_ID, s_epid),
                        ESP_ERR_INVALID_ARG, TAG,
                        "Invalid CONFIG_ZB_ROUTER_EXT_PAN_ID \"%s\" "
                        "(need 16 hex chars, e.g. 0123456789abcdef)",
                        CONFIG_ZB_ROUTER_EXT_PAN_ID);

    /* Order matters: antenna select before the radio powers up. */
    ESP_RETURN_ON_ERROR(antenna_init(), TAG, "antenna init failed");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Standard IDF recovery. Zigbee network data lives in the separate
         * zb_storage partition, so this does not drop the network. */
        ESP_LOGW(TAG, "NVS partition needs erase (%s), erasing", esp_err_to_name(err));
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "NVS erase failed");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "NVS init failed");

    esp_zb_platform_config_t config = {
        .radio_config = ZB_ROUTER_RADIO_CONFIG(),
        .host_config  = ZB_ROUTER_HOST_CONFIG(),
    };
    ESP_RETURN_ON_ERROR(esp_zb_platform_config(&config), TAG, "Zigbee platform config failed");

    s_register_eps = register_eps;
    ESP_LOGI(TAG, "Start Zigbee router (channel %d (0 = all), max children %d)",
             CONFIG_ZB_ROUTER_CHANNEL, CONFIG_ZB_ROUTER_MAX_CHILDREN);
    if (xTaskCreate(zb_router_task, "Zigbee_main", CONFIG_ZB_ROUTER_TASK_STACK_SIZE, NULL,
                    CONFIG_ZB_ROUTER_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}
