/*
 * zb_router - generic Zigbee ROUTER boilerplate for ESP-IDF
 * (esp-zigbee-lib 1.6.x classic esp_zb_* API).
 *
 * SPDX-License-Identifier: MIT
 *
 * The component owns everything that is the same for every mains-powered
 * Zigbee router: NVS + platform/radio config, optional RF antenna switch,
 * the Zigbee task, BDB initialization, network steering with retry, network
 * selection (channel + extended PAN ID from Kconfig) and join/leave logging.
 * The application only creates its endpoints/clusters.
 *
 * Network selection and the antenna switch are configured in menuconfig
 * ("Zigbee Router (zb_router)"), see components/zb_router/Kconfig.
 *
 * NOTE: this component defines esp_zb_app_signal_handler(). Applications
 * must NOT define it; use zb_router_set_signal_hook() instead.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "esp_zigbee_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Creates the application's endpoints/clusters and registers them, i.e.
 * builds an esp_zb_ep_list_t and calls esp_zb_device_register(). Register a
 * ZCL action handler here too (esp_zb_core_action_handler_register()).
 * Called once on the Zigbee task, after esp_zb_init() and before
 * esp_zb_start(). */
typedef void (*zb_router_register_eps_cb_t)(void);

/* Optional hook for every Zigbee app signal (ZDO/BDB/NWK). Called on the
 * Zigbee task AFTER the built-in handling (commissioning, retry, logging),
 * so it observes rather than replaces it. Read the type with
 * *signal->p_app_signal and the status with signal->esp_err_status. */
typedef void (*zb_router_signal_hook_t)(esp_zb_app_signal_t *signal);

/* Optional callback fired each time the device is on a network: after a
 * successful steering join, and on reboot when it is already commissioned.
 * Typical use: push initial attribute values. Runs on the Zigbee task, so
 * esp_zb_* calls are safe here without esp_zb_lock_acquire(). */
typedef void (*zb_router_joined_cb_t)(void);

/* Set the optional hooks. Call before zb_router_start(); NULL clears. */
void zb_router_set_signal_hook(zb_router_signal_hook_t hook);
void zb_router_set_joined_cb(zb_router_joined_cb_t cb);

/* Bring the router up: validate the Kconfig network options, init the RF
 * antenna switch (if configured), NVS and the Zigbee platform, then spawn
 * the Zigbee task. That task calls register_eps, applies the channel mask
 * and EPID filter, starts the stack and steers (retrying until joined).
 * Returns once the task is spawned; joining continues in the background.
 *
 * ESP_ERR_INVALID_ARG   register_eps is NULL or CONFIG_ZB_ROUTER_EXT_PAN_ID
 *                       is malformed
 * ESP_ERR_INVALID_STATE already started
 * ESP_ERR_NO_MEM        task creation failed
 * other                 from gpio / NVS / esp_zb_platform_config() */
esp_err_t zb_router_start(zb_router_register_eps_cb_t register_eps);

/* True once the device is on a network (joined or rebooted commissioned),
 * false again after it leaves. */
bool zb_router_is_joined(void);

#ifdef __cplusplus
}
#endif
