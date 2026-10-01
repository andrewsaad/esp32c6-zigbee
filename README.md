# esp32c6-zigbee - zb_router, a Zigbee router component for ESP-IDF

`zb_router` is a small ESP-IDF component that does everything a mains-powered
Zigbee **router** needs except the application itself: NVS and radio/platform
setup, an optional RF antenna switch, the Zigbee task, BDB initialization,
network steering with retry, network selection (channel + extended PAN ID
from Kconfig) and join/leave logging. Your app only creates its endpoints and
clusters.

It targets Espressif's esp-zigbee-sdk **1.6.x** (classic `esp_zb_*` API) on
the ESP32-C6 (also builds for the ESP32-H2), and ships with an example On/Off
Light (`examples/on_off_light/`).

```
components/zb_router/      the component (Kconfig, idf_component.yml, include/, src/)
examples/on_off_light/     minimal HA On/Off Light router driving a GPIO/LED
test/host/                 host unit test for the EPID parser (gcc, no IDF)
```

## API

`#include "zb_router.h"`

```c
typedef void (*zb_router_register_eps_cb_t)(void);
typedef void (*zb_router_signal_hook_t)(esp_zb_app_signal_t *signal);
typedef void (*zb_router_joined_cb_t)(void);

void      zb_router_set_signal_hook(zb_router_signal_hook_t hook);
void      zb_router_set_joined_cb(zb_router_joined_cb_t cb);
esp_err_t zb_router_start(zb_router_register_eps_cb_t register_eps);
bool      zb_router_is_joined(void);
```

- **`zb_router_start(register_eps)`** validates the Kconfig network options,
  inits the antenna switch (if configured), NVS and the Zigbee platform, then
  spawns the Zigbee task and returns. The task calls `register_eps`, applies
  the channel mask and EPID filter, starts the stack and steers, retrying
  until it joins. Returns `ESP_ERR_INVALID_ARG` for a NULL callback or a
  malformed EPID option, `ESP_ERR_INVALID_STATE` if called twice.
- **`register_eps`** (required) builds your `esp_zb_ep_list_t`, calls
  `esp_zb_device_register()`, and usually registers a ZCL action handler with
  `esp_zb_core_action_handler_register()`. It runs once on the Zigbee task,
  between `esp_zb_init()` and `esp_zb_start()`.
- **`zb_router_set_joined_cb()`** (optional) fires each time the device is on
  a network: after a successful join, and on boot when already commissioned.
  Good for pushing initial attribute values.
- **`zb_router_set_signal_hook()`** (optional) sees every Zigbee app signal
  *after* the built-in handling, for app-specific signals.
- Hooks run on the Zigbee task, so `esp_zb_*` calls are safe there without
  `esp_zb_lock_acquire()`. From any other task, wrap stack calls in
  `esp_zb_lock_acquire(portMAX_DELAY)` / `esp_zb_lock_release()`.
- The component **defines `esp_zb_app_signal_handler()`**. Do not define it
  in your app (duplicate symbol); use the signal hook instead.

Minimal app (see `examples/on_off_light/main/on_off_light.c`):

```c
static void my_register_eps(void)
{
    esp_zb_on_off_light_cfg_t cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    esp_zb_device_register(esp_zb_on_off_light_ep_create(10, &cfg));
    esp_zb_core_action_handler_register(my_action_handler);
}

void app_main(void)
{
    ESP_ERROR_CHECK(zb_router_start(my_register_eps));
}
```

## Kconfig options

`idf.py menuconfig` -> **Zigbee Router (zb_router)**:

| Option | Default | Meaning |
|--------|---------|---------|
| `CONFIG_ZB_ROUTER_CHANNEL` | `0` | `0` = steer on all channels 11-26. `11`-`26` = steer only on that channel. 1-10 fail the build. |
| `CONFIG_ZB_ROUTER_EXT_PAN_ID` | `0000000000000000` | Extended PAN ID to join: 16 hex chars, big-endian as ZHA displays it, colons allowed. All zeros = any network. Malformed = `zb_router_start()` fails. |
| `CONFIG_ZB_ROUTER_MAX_CHILDREN` | `10` | Max child devices this router accepts. |
| `CONFIG_ZB_ROUTER_RETRY_DELAY_MS` | `1000` | Delay before retrying failed initialization/steering. |
| `CONFIG_ZB_ROUTER_TASK_STACK_SIZE` | `4096` | Zigbee task stack; your callbacks run on it. |
| `CONFIG_ZB_ROUTER_TASK_PRIORITY` | `5` | Zigbee task priority. |
| `CONFIG_ZB_ROUTER_ANTENNA_*` | `NONE` | RF antenna switch: `NONE`, `XIAO_ONBOARD` (built-in ceramic) or `XIAO_EXTERNAL` (u.FL). The XIAO options drive GPIO3 LOW (RF switch enable) and GPIO14 (antenna select) before the radio starts; use them only on a Seeed XIAO ESP32-C6. |

Channel and EPID are shown in ZHA under Settings -> Devices & Services ->
Zigbee -> Configure -> network settings. You can set them in an
`sdkconfig.defaults` instead of menuconfig:

```
CONFIG_ZB_ROUTER_CHANNEL=15
CONFIG_ZB_ROUTER_EXT_PAN_ID="01:23:45:67:89:ab:cd:ef"
CONFIG_ZB_ROUTER_ANTENNA_XIAO_ONBOARD=y
```

## Using it from another project

Add the component to your app's `main/idf_component.yml`; it brings the
pinned Zigbee SDK with it:

```yaml
dependencies:
  zb_router:
    git: https://github.com/andrewsaad/esp32c6-zigbee.git   # or a local clone path
    path: components/zb_router
    version: "<tag or commit>"
  idf:
    version: ">=5.3.1"
```

For a local checkout use `zb_router: { path: <relative path>/components/zb_router }`
instead (the example does this), or add `components/` to `EXTRA_COMPONENT_DIRS`
in your top-level `CMakeLists.txt`.

Your project also needs what any esp-zigbee-sdk app needs (copy from
`examples/on_off_light/`):

- a partition table with **`zb_storage`** and **`zb_fct`** partitions
  (`partitions.csv`);
- `sdkconfig.defaults` with `CONFIG_ZB_ENABLED=y`, `CONFIG_ZB_ZCZR=y`,
  `CONFIG_ZB_RADIO_NATIVE=y`, the custom partition table and the mbedTLS
  settings.

## Build the example

Requires **ESP-IDF v5.3.1 or newer**. The Zigbee SDK is not in this repo: the
component manager downloads it into `managed_components/` on the first build
(Espressif's code, Apache-2.0).

```sh
idf.py -C examples/on_off_light set-target esp32c6
idf.py -C examples/on_off_light menuconfig   # channel, EPID, antenna, LED GPIO
idf.py -C examples/on_off_light build
idf.py -C examples/on_off_light -p <PORT> flash monitor
```

The example's `sdkconfig.defaults` selects the XIAO built-in antenna and its
user LED (GPIO15, active-low). On another board set
`CONFIG_ZB_ROUTER_ANTENNA_NONE=y` and pick your LED pin under
**Example: On/Off Light**. It identifies as ManufacturerName `esp32c6-zigbee`
/ ModelIdentifier `OnOffLight`.

On Windows, run ESP-IDF from PowerShell or the ESP-IDF command prompt, not
Git Bash / MSys (`idf_tools.py` rejects MSys environments).

## Host test

The EPID parser is plain C and tested on the host (on Windows, under WSL):

```sh
sh test/host/run.sh
```

## Gotchas (learned on the bench)

- **Why 1.6.x and not 2.x.** esp-zigbee-lib 2.0 is a full API rewrite
  (`ezb_*` namespace). This component uses the mature 1.6.x `esp_zb_*` API;
  `idf_component.yml` pins `esp-zigbee-lib` and `esp-zboss-lib` to `~1.6.0`.
  Keep the pins; on 2.x every API name changes. Built against
  esp-zigbee-lib 1.6.8 / esp-zboss-lib 1.6.4.
- **All-channels steering joins strangers.** With the default all-channels
  mask, a first bench boot joined a neighbor's network that happened to permit
  joining, within minutes, then stopped steering for good ("already on a
  network"). Pin the channel.
- **Channel pinning is not enough. Pin the EPID.** A neighbor's network on the
  same channel kept winning steering. Setting the extended PAN ID is what
  actually restricts association to your network. The EPID is public beacon
  data, not a secret. The boot log prints the network the device ends up on
  (`EPID .. PAN .. Ch .. Short ..`) so you can confirm it is yours.
- **Factory reset = full flash erase.** Erasing only the `zb_storage`
  partition did not clear the joined state on this SDK. Use
  `idf.py -p <PORT> erase-flash flash` to make the device factory-new so it
  steers again.
- **Pair near the coordinator.** A join made far from the coordinator
  completed at the network layer (right EPID/PAN/channel) but the coordinator
  never saw a Device_annce and no ZHA device was created; forwarding through
  an intermediate router was the suspect. Pairing close to the coordinator
  avoids it. If it still fails, enable zigpy debug logging during the join.
- **No explicit attribute reports right after joining.** Calling
  `esp_zb_zcl_report_attr_cmd_req()` with
  `ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT` while no binding exists
  (right after a join, before ZHA's interview binds the cluster, or on a
  foreign network) trips `Zigbee stack assertion failed
  zcl/zcl_general_commands.c:612` and the device boot-loops. Just set the
  attribute: ZHA configures reporting during pairing and the stack reports
  changes on its own.
- **Router vs end device.** This component is router-only: mains-powered,
  always listening, extends the mesh (the build needs `CONFIG_ZB_ZCZR=y`).
  A battery device must be a sleepy end device (`ESP_ZB_DEVICE_TYPE_ED`,
  `CONFIG_ZB_ZED=y`, keep-alive/sleep config), which zb_router does not cover.
- **After a leave:** a leave *with rejoin* (the coordinator asking the device
  to rejoin) starts steering again right away. A plain leave (removed from ZHA,
  or a local reset) does not rejoin by itself; it steers again on the next
  boot. Restart steering from your signal hook if you want otherwise.
- **ZCL strings are length-prefixed.** Basic-cluster identity strings start
  with a length byte (`"\x0a""OnOffLight"`); keep it accurate if you rename.
- **XIAO antenna pins.** On the XIAO ESP32-C6 keep GPIO3/GPIO14 for the
  antenna switch. GPIO8/9/15 are strapping pins: do not wire external
  circuits that pull them at reset (GPIO15 is the onboard user LED, which is
  fine to drive after boot).

## License

MIT, see [LICENSE](LICENSE). The Espressif Zigbee SDK fetched at build time is
licensed separately (Apache-2.0).
