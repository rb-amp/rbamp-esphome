# 05 · Quickstart — from zero to Home Assistant

This chapter walks you, in about 5 minutes of active work (plus compile and flash time), from a blank ESP32 and an rbAmp module to live sensors in Home Assistant.

## What you need

### Hardware

- **ESP32 board** — any board with two free I²C pins will do. The examples use `GPIO21` (SDA) and `GPIO22` (SCL), the DevKitC defaults. If your board differs, adjust the pin numbers in YAML.
- **rbAmp module** — any UI* or I* SKU. For a first run UI1 is the easiest: one voltage channel plus one current channel, minimum wiring.
- **5 V power** for rbAmp — the module runs on 4.5..5.5 V. USB power from the ESP32 is fine if the board exposes the 5V rail.
- **Wiring**: VCC → +5 V, GND → GND, SDA → GPIO21, SCL → GPIO22. External pull-up resistors are not needed for a single module (built-in 4.7 kΩ pull-ups are already on the PCB). Full schematic in [04_hardware.md](04_hardware.md).
- **USB cable** from the ESP32 to your computer for flashing.

### Software

| Requirement | Note |
|---|---|
| **ESPHome >= 2024.6** | The component uses the modern `ota:` syntax (list of platforms) introduced in 2024.6. Address changes use `i2c::I2CDevice::set_i2c_address()` (available since 2023.6). Tested through ESPHome 2026.5 and later. |
| **Python 3.10..3.13** for the CLI | The PlatformIO layer ESPHome uses internally (`penv`) does not accept Python 3.14. If you are using the HA Add-on, this item does not apply. |
| **Home Assistant** | Any recent version. Auto-discovery works via mDNS (Avahi / Bonjour). |

Two compile paths:

1. **HA ESPHome Add-on** (recommended for most users) — drop the YAML into the add-on's config directory and click Compile. No local Python environment required.
2. **Local CLI venv** — for development and CI. Install with `pip install esphome` inside a Python 3.11–3.13 venv.

### venv setup (CLI path only)

```sh
# Windows PowerShell — adjust the Python path to your own 3.11 / 3.13 install
& "C:\Python311\python.exe" -m venv .venv-esphome
.\.venv-esphome\Scripts\Activate.ps1
pip install esphome
esphome version          # should print 2024.6.x or newer
```

On Linux / macOS:

```sh
python3.11 -m venv .venv-esphome
source .venv-esphome/bin/activate
pip install esphome
esphome version
```

## Step 1 — Locate the component

There are two forms for the `external_components` block — pick one based on your environment.

### Form A — github (default, works in the HA Add-on)

```yaml
external_components:
  - source: github://rb-amp/rbamp-esphome@main
    components: [rbamp]
    refresh: 1d   # 1 day for production. During active development on
                  # @main switch to `0s`, otherwise ESPHome will cache a
                  # copy and ignore new commits.
```

This form is the only one that works inside the HA ESPHome Add-on (the
Add-on has no shell access to sibling directories). It is also convenient
for the CLI: nothing needs to be cloned locally.

### Form B — local path (for development inside the rbAmp monorepo)

If you have cloned the repository and are working directly against the
component sources, point ESPHome at a local path:

```yaml
external_components:
  - source:
      type: local
      path: /absolute/path/to/rbamp-repo/tools/esphome-rbamp/components
    components: [rbamp]
```

The `example/ui1.yaml` block is already wired to a relative path (`../components`)
that works when running `esphome compile` from the `example/` directory:

```sh
cd tools/esphome-rbamp/example
esphome compile ui1.yaml
```

> ⚠ **Local paths do not work inside the HA Add-on** — the Add-on keeps
> YAML in `/config/esphome/` with no shell access to the rest of the file
> system. Add-on users pick form A.

## Step 2 — Create the YAML

Copy `example/ui1.yaml` as a starting point. Open it in any editor and fill in your WiFi credentials and device name. The full file is shown below with line-by-line comments:

```yaml
esphome:
  name: rbamp-ui1          # node name — sets the mDNS hostname (rbamp-ui1.local)

esp32:
  board: esp32dev           # your board (for example nodemcu-32s, lolin_d32)
  framework:
    type: arduino           # arduino or esp-idf — both work with the component

logger:
  level: DEBUG              # DEBUG prints I²C reads; after the first boot switch to INFO

wifi:
  ssid: !secret wifi_ssid   # see secrets.yaml below
  password: !secret wifi_password
  ap:
    ssid: "rbAmp UI1 Fallback"    # fallback AP if the join fails
    password: !secret ap_password

captive_portal:             # config page on the fallback AP

api:                        # Home Assistant native API on port 6053
  # encryption:
  #   key: "BASE64_KEY"     # optional — see 08_has_integrations.md

ota:
  - platform: esphome       # over-the-air updates

i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz          # IMPORTANT: 100 kHz causes periodic NACKs from the module;
                            # 50 kHz reduces them by about 5-10x (see SPEC §B.5)
  scan: true                # logs every I²C address found at boot

external_components:
  - source:
      type: local
      path: ../components   # relative to this YAML file
    components: [rbamp]

rbamp:
  id: meter1
  address: 0x50             # factory default address
  update_interval: 60s      # how often to close a period and publish sensors
  ct_model: SCT_013_030     # CT clamp model — change to match your install
                            # (SCT_013_005 / _010 / _030 / _050 / _100)

sensor:
  - platform: rbamp
    rbamp_id: meter1
    voltage:
      name: "Mains Voltage"  # HA entity ID: sensor.rbamp_ui1_mains_voltage
    current:
      name: "Mains Current"
    power:
      name: "Mains Power"
    energy:
      name: "Mains Energy"   # state_class: total_increasing — ready for the Energy dashboard
    frequency:
      name: "Mains Frequency"
    power_factor:
      name: "Mains Power Factor"
```

> The `ct_model:` key loads the factory coefficients for the selected clamp model. If you are not using SCT-013-030, change it to the right one. For selection guidance see [03_sensor_selection.md](03_sensor_selection.md).

### secrets.yaml

Create `secrets.yaml` in the same directory as the YAML (it is `.gitignore`d by default via `example/.gitignore`, so credentials do not leak into a commit):

```yaml
wifi_ssid: "YourNetworkName"
wifi_password: "YourPassword"
ap_password: "rbamprbamp"
```

## Step 3 — Compile

```sh
# from the example/ directory with the venv active
esphome compile ui1.yaml
```

What happens:

1. ESPHome expands `external_components` and loads the Python package `rbamp` from `../components`.
2. PlatformIO downloads the ESP32 platform on the first run (~1–2 GB, cached afterwards). This is the slowest step — up to 15 minutes on a slow connection.
3. The C++ component compiles and links. A successful finish looks like:

```
SUCCESS Took 58.71 seconds
Wrote .esphome/build/rbamp-ui1/.pioenvs/rbamp-ui1/firmware.factory.bin
```

If the build fails with `Error: Failed to install Python dependencies`, the host is most likely running Python 3.14. Use the HA Add-on or a venv with 3.11 / 3.13 (see "What you need"). This is a PlatformIO `uv`-resolver incompatibility with some CPU models, not a component bug.

## Step 4 — First flash

`esphome upload` writes the binary over USB. First find the right port:

- **Windows**: Device Manager → Ports (COM & LPT). Usually `COM3`–`COM9`.
- **Linux / macOS**: `ls /dev/tty*` — usually `/dev/ttyUSB0` or `/dev/ttyACM0`.

```sh
esphome upload --device COM6 ui1.yaml             # Windows
esphome upload --device /dev/ttyUSB0 ui1.yaml     # Linux
```

For the HA Add-on path:

1. Open Home Assistant → Settings → Add-ons → ESPHome.
2. Open the ESPHome Add-on web UI.
3. Find your device (or click "New Device") → Install → Plug in to this computer.

The flashing tool (`esptool.py` under the hood) restarts the ESP32 automatically after the write. Total flash time is typically 30–60 seconds.

## Step 5 — What to expect in the boot log

Attach `esphome logs` to watch the boot in real time:

```sh
esphome logs ui1.yaml --device COM6
```

A healthy first boot produces lines like these (values and timings are approximate):

```
[D][i2c:136]: Found i2c device at address 0x50
[C][rbamp:442]: Setting up rbAmp at 0x50 ...
[C][rbamp:448]:   Firmware version: 0x01
[C][rbamp:598]: rbAmp:
[C][rbamp:599]:   Address: 0x50
[C][rbamp:600]:   Firmware version: 0x01
[C][rbamp:607]:   Topology: SINGLE, channels: 1, voltage: yes
[C][rbamp:609]:   Bidirectional: NO
[C][rbamp:610]:   Broadcast LATCH: NO
[C][rbamp:611]:   Wh persistence: NVS every 300s
[C][component:246]: Setup rbamp took 67ms
```

The `i2c: scan: true` line appears before the component lines and confirms that the module is physically visible on the bus. If `Found i2c device at address 0x50` is missing, see "What to do if" below.

Within one `update_interval` (60 s by default) the sensor publications begin:

```
[D][sensor:094]: 'Mains Voltage': Sending state 226.70 V
[D][sensor:094]: 'Mains Current': Sending state 0.755 A
[D][sensor:094]: 'Mains Power': Sending state 92.80 W
[D][sensor:094]: 'Mains Power Factor': Sending state 0.542
[D][sensor:094]: 'Mains Frequency': Sending state 50.0 Hz
[D][sensor:094]: 'Mains Energy': Sending state 0.026 Wh
```

Bench reference values for a UI1 SKU with an inductive load: U ≈ 225 V, I ≈ 0.78 A, P ≈ 110 W, PF ≈ 0.62. Actual values depend on the connected load.

## Step 6 — Discovery in Home Assistant

### mDNS (automatic)

HA discovers any ESPHome node with an `api:` block on the same local network segment. Within 30–60 seconds of the first boot of the ESP32:

1. HA shows a notification: **New devices discovered**.
2. Settings → Devices & Services → ESPHome (or click the notification).
3. Pick the new node (for example `rbamp-ui1`).
4. Enter the device's local IP (visible in the logs as `[I][wifi:189]: IP: 192.168.0.xxx`) if HA did not find it on its own.
5. Add.

Every sensor entity registers immediately. Entity IDs follow the pattern `sensor.<node_name>_<sensor_name>` (spaces replaced with underscores, all lowercase). For the YAML above: `sensor.rbamp_ui1_mains_voltage`, `sensor.rbamp_ui1_mains_energy`, and so on.

### Manual add (if mDNS is blocked)

Settings → Devices & Services → Add Integration → ESPHome → IP address and API port (6053). If `api: encryption: key:` is set, enter the key when prompted.

### Energy dashboard

The `energy` sensor is already configured for HA Energy:

- `device_class: energy`
- `state_class: total_increasing`
- `unit_of_measurement: Wh`

Settings → Dashboards → Energy → Add Consumption → pick "Mains Energy". HA starts tracking cumulative consumption right away.

## What to do if

| Symptom | Likely cause | What to do |
|---|---|---|
| `Found i2c device at address 0x50` is missing | Module is not powered or wired up, wrong GPIO pins | Check VCC (should be 4.5–5.0 V), GND, SDA / SCL; make sure the pull-up jumpers on the module are intact |
| `Probe failed at 0x50` in the logs | The module is visible in the scan but does not respond to register reads | Check `frequency: 50kHz` in the YAML; make sure the module is not in the middle of a flash write (~700 ms) |
| Sensors read 0 or NaN | The module is still booting; for the first ~250 ms after power-up the registers read 0 | Wait for the first `update_interval` cycle; the component skips the read until the status register is ready |
| HA does not find the device | mDNS blocked by the router / VLAN; `api:` block is missing | Add `api:`; try a manual add by IP |
| Warning `update() took 406ms` | Normal — the module has no I²C auto-increment, so 36 single-byte transactions per cycle at 50 kHz | Not a problem; the ESPHome 30 ms budget is a guideline, not a hard limit |
| Fallback to AP mode | Wrong SSID / password; AP out of range | Check `secrets.yaml`; move closer to the AP; 2.4 GHz vs 5 GHz |

For problems outside this list, see [10_troubleshooting.md](10_troubleshooting.md).

## What's next

- **Three-channel install**: [06_examples.md](06_examples.md) — UI3 YAML with three independent current channels and every derived quantity.
- **Several modules on one bus**: [06_examples.md](06_examples.md) — multi-module section.
- **Deep dive into HA Energy**: [08_has_integrations.md](08_has_integrations.md).
- **DIY integrations** (Grafana, MQTT, automations): [07_diy_integrations.md](07_diy_integrations.md).
- **Full configuration reference**: [09_api_reference.md](09_api_reference.md).
- **Troubleshooting** for first-time rbAmp use: [10_troubleshooting.md](10_troubleshooting.md).



---

← [Wiring](04_hardware.md) · [Docs index](README.md) · [Examples](06_examples.md) →
