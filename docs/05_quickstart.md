# 05 · Quickstart — from zero to Home Assistant

In about 5 minutes of hands-on work (plus compile and flash time), this chapter takes you from a bare ESP32 and an rbAmp module to live sensors in Home Assistant.

## What you'll need

### Hardware

- **ESP32 board** — any board with two free I²C pins works. The examples use `GPIO21` (SDA) and `GPIO22` (SCL) — the standard values for the DevKitC. If your board differs, adjust the pin numbers in the YAML.
- **rbAmp module** — any UI* or I* SKU. For a first run, UI1 is the easiest: one voltage + one current channel, the fewest wires.
- **5 V supply** for rbAmp — the module runs on 4.5..5.5 V. USB power from the ESP32 works if the board exposes the 5V rail.
- **Wiring**: VCC → +5 V, GND → GND, SDA → GPIO21, SCL → GPIO22. External pull-up resistors are not needed for a single module (the built-in 4.7 kΩ are already on the board). For the full schematic, see [04_hardware.md](04_hardware.md).
- **USB cable** from the ESP32 to your computer for flashing.

### Software

| Requirement | Note |
|---|---|
| **ESPHome ≥ 2024.6** | The component uses the modern `ota:` syntax (platform list), available since 2024.6. The I²C address change uses `i2c::I2CDevice::set_i2c_address()` (available since 2023.6). Verified up to ESPHome 2026.5 and newer. |
| **Python 3.10..3.13** for the CLI | ESPHome's internal PlatformIO layer (`penv`) does not accept Python 3.14. If you use the HA Add-on, this point does not apply. |
| **Home Assistant** | Any recent version. Auto-discovery works via mDNS (Avahi / Bonjour). |

Two compilation paths:

1. **HA ESPHome Add-on** (recommended for most users) — drop the YAML into the add-on's config directory and click Compile. No local Python environment needed.
2. **Local CLI venv** — for development and CI. Install with `pip install esphome` in a Python 3.11–3.13 venv.

### Setting up a venv (CLI path only)

```sh
# Windows PowerShell — adjust the Python path to your 3.11 / 3.13
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
    refresh: 1d   # 1 day for production. During active development
                  # on @main, change this to `0s`, otherwise ESPHome will
                  # cache the copy and ignore new commits.
```

This form is the only one that works in the HA ESPHome Add-on (the Add-on
has no shell access to sibling directories). It is also convenient for the
CLI: nothing needs to be cloned locally.

### Form B — local path (for development in the rbAmp monorepo)

If you cloned the repository and work directly with the component's
sources, point ESPHome at a local path:

```yaml
external_components:
  - source:
      type: local
      path: /absolute/path/to/rbamp-repo/tools/esphome-rbamp/components
    components: [rbamp]
```

In `example/ui1.yaml` the block is already configured for a relative path
(`../components`), which works when you run `esphome compile` from the
`example/` directory:

```sh
cd tools/esphome-rbamp/example
esphome compile ui1.yaml
```

> ⚠ **A local path does not work in the HA Add-on** — the Add-on keeps the
> YAML in `/config/esphome/` with no shell access to the rest of the file
> system. Add-on users choose form A.

## Step 2 — Create the YAML

Copy `example/ui1.yaml` as a starting point. Open it in any editor and fill in your WiFi credentials and device name. The full file is shown below with line-by-line comments:

```yaml
esphome:
  name: rbamp-ui1          # node name — sets the mDNS hostname (rbamp-ui1.local)

esp32:
  board: esp32dev           # your board (e.g. nodemcu-32s, lolin_d32)
  framework:
    type: arduino           # arduino or esp-idf — both work with the component

logger:
  level: DEBUG              # DEBUG shows I²C reads; after the first run you can use INFO

wifi:
  ssid: !secret wifi_ssid   # see secrets.yaml below
  password: !secret wifi_password
  ap:
    ssid: "rbAmp UI1 Fallback"    # fallback AP if the connection fails
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
  frequency: 50kHz          # IMPORTANT: 100 kHz causes intermittent NACKs from the module;
                            # 50 kHz reduces them ~5-10x (see 10 · troubleshooting → NACK-discipline)
  scan: true                # logs every I²C address found at boot

external_components:
  - source:
      type: local
      path: ../components   # relative to this YAML file
    components: [rbamp]

rbamp:
  id: meter1
  address: 0x50             # factory default address
  update_interval: 60s      # how often to close the period and publish sensors
                            # minimum 1 s (v1.3); below this → cv.Invalid
  ct_model: SCT_013_030     # CT clamp model — change it to match yours
                            # (SCT_013_005 / _010 / _020 / _030 / _050)
                            # SCT_013_100 removed in v1.3 (see 03_sensor_selection.md)

sensor:
  - platform: rbamp
    rbamp_id: meter1
    voltage:
      name: "Mains Voltage"  # entity ID in HA: sensor.rbamp_ui1_mains_voltage
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

> The `ct_model:` key loads the factory coefficients for the chosen clamp model. If you don't have an SCT-013-030, change it to the model you have. For more on the choice, see [03_sensor_selection.md](03_sensor_selection.md).

### secrets.yaml

Create `secrets.yaml` in the same directory as the YAML (it is `.gitignore`-d by default in `example/.gitignore`, so credentials don't leak into a commit):

```yaml
wifi_ssid: "YourNetworkName"
wifi_password: "YourPassword"
ap_password: "rbamprbamp"
```

## Step 3 — Compilation

```sh
# from the example/ directory with the venv active
esphome compile ui1.yaml
```

What happens:

1. ESPHome unpacks `external_components` and loads the `rbamp` Python package from `../components`.
2. PlatformIO downloads the ESP32 platform on the first run (~1–2 GB, cached). This is the slowest step — up to 15 minutes on a slow connection.
3. The C++ component compiles and links. A successful finish looks like:

```
SUCCESS Took 58.71 seconds
Wrote .esphome/build/rbamp-ui1/.pioenvs/rbamp-ui1/firmware.factory.bin
```

If the build fails with `Error: Failed to install Python dependencies`, the host is most likely running Python 3.14. Use the HA Add-on or a venv with 3.11 / 3.13 (see "What you'll need"). This is an incompatibility between PlatformIO's `uv` resolver and certain CPU models, not a bug in the component.

## Step 4 — First flash

`esphome upload` flashes the binary over USB. First, find the right port:

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

The flashing tool (`esptool.py` inside) reboots the ESP32 automatically after the write. The full flash time is usually 30–60 seconds.

## Step 5 — What to expect in the boot log

Attach `esphome logs` to watch the boot in real time:

```sh
esphome logs ui1.yaml --device COM6
```

A healthy first boot produces roughly these lines (values and timings are approximate):

```
[D][i2c:136]: Found i2c device at address 0x50
[C][rbamp:442]: Setting up rbAmp at 0x50 ...
[C][rbamp:448]:   Firmware version: 0x04                  ; v1.3 = 0x04
[C][rbamp:452]:   Capability:      0x0718                 ; CAP_GC_LATCH | CAP_TWO_PHASE_ADDR | …
[C][rbamp:454]:   Variant:         UI1                    ; from REG_HW_VARIANT 0x55
[C][rbamp:456]:   UID:             0x1A2B3C4D5E6F708192A3 ; 96-bit chip UID
[C][rbamp:460]:   sensor_class:    matches → SKIP flash erase   ; read-compare-write
[C][rbamp:461]:   ct_model[0]:     matches → SKIP flash erase   ; v1.3 boot ~0 ms
[C][rbamp:598]: rbAmp:
[C][rbamp:599]:   Address: 0x50
[C][rbamp:600]:   Firmware version: 1.3
[C][rbamp:607]:   Topology: SINGLE, channels: 1, voltage: yes
[C][rbamp:609]:   Bidirectional: NO
[C][rbamp:610]:   Broadcast LATCH: NO     (v0.4.0 legacy; use fleet_gc_enable)
[C][rbamp:611]:   Wh persistence: NVS every 300s
[C][component:246]: Setup rbamp took 67ms      ; v1.3: ~70 ms warm boot; v0.4.0: ~2.8 s cold install
```

The `i2c: scan: true` line appears before the component lines and confirms that the module is physically visible on the bus. If `Found i2c device at address 0x50` is missing, see the "What to do if" section below.

Within one `update_interval` (60 s by default), sensor publications appear:

```
[D][sensor:094]: 'Mains Voltage': Sending state 226.70 V
[D][sensor:094]: 'Mains Current': Sending state 0.755 A
[D][sensor:094]: 'Mains Power': Sending state 92.80 W
[D][sensor:094]: 'Mains Power Factor': Sending state 0.542
[D][sensor:094]: 'Mains Frequency': Sending state 50.0 Hz
[D][sensor:094]: 'Mains Energy': Sending state 0.026 Wh
```

Reference values on the bench with a UI1 SKU and an inductive load: U ≈ 225 V, I ≈ 0.78 A, P ≈ 110 W, PF ≈ 0.62. The exact values depend on the connected load.

## Step 6 — Discovery in Home Assistant

### mDNS (automatic)

HA finds ESPHome nodes with a configured `api:` block on its own within the same local network segment. Within 30–60 seconds after the ESP32's first boot:

1. HA shows a notification: **New devices discovered**.
2. Settings → Devices & Services → ESPHome (or click the notification).
3. Select the new node (e.g. `rbamp-ui1`).
4. Enter the device's local IP (visible in the logs as `[I][wifi:189]: IP: 192.168.0.xxx`) if HA didn't find it on its own.
5. Add.

All sensor entities register instantly. The entity ID follows the pattern `sensor.<node_name>_<sensor_name>` (spaces replaced with underscores, all lowercase). For the YAML above: `sensor.rbamp_ui1_mains_voltage`, `sensor.rbamp_ui1_mains_energy`, and so on.

### Manual addition (if mDNS is blocked)

Settings → Devices & Services → Add Integration → ESPHome → IP address and API port (6053). If a key is set in `api: encryption: key:`, enter it when prompted.

### Energy dashboard

The `energy` sensor is already configured for HA Energy:

- `device_class: energy`
- `state_class: total_increasing`
- `unit_of_measurement: Wh`

Settings → Dashboards → Energy → Add Consumption → select "Mains Energy". HA immediately starts tracking cumulative consumption.

## Step 7 — Scaling up to a fleet (canonical multi-module deployment)

A single UI1 on the mains gives whole-house totals. To get a breakdown
by sub-branches (boiler, air conditioner, EV), add I2/I3 modules on the
same I²C bus. This is the **80%-canonical deployment** (see [01_overview.md](01_overview.md)).

### Minimal YAML extension

```yaml
i2c:
  sda: GPIO21
  scl: GPIO22
  frequency: 50kHz
  scan: true

# external_components: ... as in Step 1

rbamp:
  - id: mains_meter
    address: 0x50
    sensor_class: SCT_013
    ct_model: SCT_013_030
    fleet_gc_enable: true       # NEW v1.3 — capability-gated GC latch
    group_id: 1                  # cluster id

  - id: boiler_meter             # I2 sub-meter, address 0x51 after provisioning
    address: 0x51
    sensor_class: SCT_013
    ct_models: [SCT_013_020, SCT_013_010]   # 20A boiler + 10A washing machine
    fleet_gc_enable: true
    group_id: 1                  # same group → synchronized latching

  - id: panel_meter              # I3 sub-meter, address 0x52
    address: 0x52
    sensor_class: SCT_013
    ct_models: [SCT_013_005, SCT_013_030, SCT_013_020]   # mixed mixed-CT
    fleet_gc_enable: true
    group_id: 1

sensor:
  - platform: rbamp
    rbamp_id: mains_meter
    voltage: { name: "Mains Voltage" }
    current: { name: "Mains Current" }
    power:   { name: "Mains Power" }
    energy:  { name: "Mains Energy" }
  - platform: rbamp
    rbamp_id: boiler_meter
    current:   { name: "Boiler Current" }       # SCT_013_020 channel 0
    current_1: { name: "Washer Current" }       # SCT_013_010 channel 1
  - platform: rbamp
    rbamp_id: panel_meter
    current:   { name: "Lighting Current" }     # SCT_013_005 channel 0
    current_1: { name: "Appliances Current" }   # SCT_013_030 channel 1
    current_2: { name: "Sub-panel Current" }    # SCT_013_020 channel 2
```

### What to do before the first boot

1. **Address provisioning** — all modules ship at 0x50 from the factory. See
   [04_hardware.md Two-phase address commit](04_hardware.md#two-phase-address-commit-v13-production-ok)
   for the sequential procedure (one virgin on the bus at a time).
2. **External 4.7 kΩ pull-up** on SDA + SCL (see [04_hardware.md Multi-module
   bus](04_hardware.md)). The internal
   pull-ups are cut by jumpers on all modules except one.
3. **External 5 V supply** — 3 modules × ~15 mA = ~45 mA + ESP32 ~200 mA. USB
   VBUS is usually enough for bring-up; for a production deployment, use a
   separate 5 V SMPS.

### What to expect in the log after boot

```text
[D][i2c:136]: Found i2c device at address 0x50
[D][i2c:136]: Found i2c device at address 0x51
[D][i2c:136]: Found i2c device at address 0x52
[C][rbamp:450]: rbAmp 0x50: variant=UI1, capability=0x0718, fleet_gc=on, group=1
[C][rbamp:450]: rbAmp 0x51: variant=I2,  capability=0x0618, fleet_gc=on, group=1
[C][rbamp:450]: rbAmp 0x52: variant=I3,  capability=0x069E, fleet_gc=on, group=1
[I][rbamp:493]: GC latch enabled fleet-wide: 3 modules in group 1
```

### Bench-validated synchronization

On the bench (Fix-A fleet UI1@0x50 + I2@0x51 + I3@0x52) with `fleet_gc_enable: true`:

```
[D][rbamp:572]: GC frame emit: group=1, tick=42, all 3 modules ACK'd
[D][rbamp:580]: check_sync(tick=42) → 3/3 in_sync   ; billing-grade sync
```

See [09_api_reference.md Fleet section](09_api_reference.md) for programmatic
emit + sync verification via native API services.

## What to do if

| Symptom | Likely cause | What to do |
|---|---|---|
| `Found i2c device at address 0x50` is missing | Module not powered or wired incorrectly, wrong GPIO pins | Check VCC (should be 4.5–5.0 V), GND, SDA / SCL; make sure the pull-up jumpers on the module are intact |
| `Probe failed at 0x50` in the logs | Module is visible in the scan but does not respond to register reads | Check `frequency: 50kHz` in the YAML; make sure the module is not in the middle of a flash-write (~700 ms) |
| Sensors read 0 or NaN | Module is still booting; for the first ~250 ms after power-up the registers read 0 | Wait for the first `update_interval` cycle; the component skips the read until the status register is ready |
| HA does not find the device | mDNS blocked by the router / VLAN; `api:` block missing | Add `api:`; try manual addition by IP |
| Warning `update() took 406ms` | The component fell back from the v1.3 burst-read path to per-register reads (4 transactions × 9 floats at 50 kHz) | Not a problem; check firmware version is `0x04` and the bus pull-ups are 4.7 kΩ to clean 3.3 V — a healthy v1.3 cycle stays under 30 ms |
| Falls back to AP mode | Wrong SSID / password; AP out of range | Check `secrets.yaml`; move closer to the AP; 2.4 GHz vs 5 GHz |

For problems not on this list, see [10_troubleshooting.md](10_troubleshooting.md).

## What's next

- **Three-channel setup**: [06_examples.md](06_examples.md) — UI3 YAML with three independent current channels and all derived quantities.
- **Multiple modules on one bus**: [06_examples.md](06_examples.md) — the multi-module section.
- **A deep dive into HA Energy**: [08_has_integrations.md](08_has_integrations.md).
- **DIY integrations** (Grafana, MQTT, automations): [07_diy_integrations.md](07_diy_integrations.md).
- **Full configuration reference**: [09_api_reference.md](09_api_reference.md).
- **Diagnosing problems** during your first run with rbAmp: [10_troubleshooting.md](10_troubleshooting.md).

