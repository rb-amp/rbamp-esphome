# 08 · Home Assistant — deep dive

This chapter covers the HA-specific details of working with rbAmp: discovery, the native API, Energy dashboard, long-term statistics, Lovelace cards, automations, and operation in environments where HA runs in Docker or WSL.

Cross-references:

- Quickstart: [`05_quickstart.md`](05_quickstart.md)
- YAML examples (Lovelace, automations): [`06_examples.md`](06_examples.md)
- DIY integrations (MQTT, InfluxDB, Lambda): [`07_diy_integrations.md`](07_diy_integrations.md)
- HA documentation: <https://www.home-assistant.io/>

---

## 1 · Discovery: mDNS or manual IP

### mDNS auto-discovery (recommended)

ESPHome nodes announce themselves over mDNS (multicast DNS) when an `api:` block is present in the YAML. The HA ESPHome integration listens for these announcements and automatically suggests new devices.

What it takes for auto-discovery to work:

- The ESP32 and HA must be on the **same Layer 2 network segment** (one VLAN, no router between them). mDNS is multicast and does not cross routers without additional configuration (mDNS reflector / Avahi proxy).
- UDP multicast must not be blocked by the router or by client-isolation features on the access point. Consumer APs often have "AP isolation" or "wireless isolation" — these block device-to-device multicast. Disable it for the IoT VLAN or specific devices.
- HA must resolve mDNS correctly. In Docker / WSL deployments this sometimes requires extra network setup (see §10).

Discovery flow:

1. Flash the ESP32 and let it boot. It joins Wi-Fi and starts announcing itself over mDNS as `<node_name>.local`.
2. In HA: Settings → Devices & Services → Integrations. A "New devices found" banner appears, or look for ESPHome in the integration list with a discovery badge.
3. Click "Configure" on the new entry. HA will ask for the encryption key if one is configured (§3).
4. All sensors are added to the device automatically.

### Manual add (when mDNS is blocked)

Settings → Devices & Services → Add Integration → ESPHome.

Enter the ESP32 IP (find it in the router's DHCP table or in the ESPHome boot log — the line `[I][wifi:189]: IP: 192.168.x.y`). The default port is `6053`.

Assign a static DHCP lease for the ESP32's MAC address in the router so the IP does not change after a router reboot.

---

## 2 · Native ESPHome API (port 6053) vs MQTT

ESPHome supports two transports. Both work with rbAmp.

| Property | Native API (port 6053) | MQTT |
|---|---|---|
| HA integration | First-class: dedicated ESPHome integration with auto-discovery | Generic: requires an MQTT broker, manual entity setup or MQTT discovery |
| Encryption | Built in (noise protocol, optional) | Depends on broker TLS setup |
| Latency | Low — direct TCP without a broker | Depends on the broker |
| Reliability | Auto-reconnect | Depends on broker availability |
| Without HA | Useless (the API is HA-specific) | Any MQTT subscriber can consume |
| Setup complexity | No additional infrastructure | Requires a running MQTT broker |

**Recommendation**: use the native API (the `api:` block) when HA is the primary consumer. Add `mqtt:` only if you also need a parallel non-HA consumer (Grafana, Node-RED, custom scripts). Both can coexist in one YAML.

The native API is the path described in this document. MQTT details are in [`07_diy_integrations.md §2`](07_diy_integrations.md).

---

## 3 · Encryption key

By default the native API is unencrypted — any host on the local network can connect to port 6053. This is acceptable on a trusted home network. For tighter security, add a key:

```yaml
api:
  encryption:
    key: "BASE64_ENCODED_32_BYTE_KEY"
```

Generate a key:

```sh
python3 -c "import base64, os; print(base64.b64encode(os.urandom(32)).decode())"
```

When adding the device to HA (Settings → Devices & Services → ESPHome → Add), enter the same key. HA will store it in the integration config and use it automatically for all subsequent connections to that node.

Put the key in `secrets.yaml`:

```yaml
# secrets.yaml (untracked, next to the device YAML)
api_key: "BASE64_ENCODED_32_BYTE_KEY"
```

```yaml
# device YAML
api:
  encryption:
    key: !secret api_key
```

When the key changes, the device must be removed from HA and re-added.

---

## 4 · Energy dashboard configuration

The rbAmp `energy` sensor (and `energy_1`, `energy_2`, `energy_exported`, etc.) ship with the following preset:

```
device_class: energy
state_class: total_increasing
unit_of_measurement: Wh
```

This is exactly the combination the HA Energy dashboard expects for cumulative consumption or generation sensors. No additional YAML is required.

### Adding sensors to the Energy dashboard

Settings → Dashboards → Energy. The dashboard has four sections:

| Section | Which rbAmp sensor to use |
|---|---|
| Grid consumption | `energy` (grid import) |
| Return to grid | `energy_exported` (STANDARD / PRO tiers; on BASIC and v1 firmware reads 0) |
| Solar panels | `energy` from a module that meters the solar inverter output |
| Home battery | N/A (rbAmp does not measure DC) |
| Individual devices | `energy_1`, `energy_2`, etc. from a multi-channel module |

For a single-component whole-house install:

1. Click "Add consumption" under Grid consumption.
2. Pick the `Mains Energy` entity.
3. Optionally set the tariff (EUR/kWh or your local currency) for cost tracking.

For a three-module install (grid + solar + EV from Example 11 in [`06_examples.md`](06_examples.md)):

1. Grid consumption: `House Energy` (from `meter_house`).
2. Return to grid: `Solar Energy Exported` (from `meter_solar`, STANDARD / PRO tiers; on BASIC leave empty).
3. Solar panels: `Solar Energy` (from `meter_solar`).
4. Individual device: `EV Charger Energy` (from `meter_evcharger`).

### NVS persistence and the Energy dashboard

The ESPHome component saves the running energy total to ESP32 NVS every 5 minutes. On boot the values are restored from NVS and published immediately — **before** HA connects and **before** the first `update_interval` fires.

This keeps the Energy dashboard from interpreting an instantaneous `0 Wh` as a counter reset. If HA sees `total_increasing` drop to 0, it treats the difference as a billing-period boundary and resets the accumulated history.

The worst-case data loss on sudden power failure is up to 5 minutes of accumulation (≈5 Wh at an average load of 60 W — invisible in daily totals).

### `last_reset` after OTA or ESP32 reboot

The HA Energy dashboard uses device_class `energy` + state_class
`total_increasing` without an explicit `last_reset` — this is the
recommended configuration for ESPHome. On ESP32 reboot the
component restores the value from NVS **before** the first
`publish_state`, so HA sees a continuous monotonically increasing
series and does **not** insert a sentinel reset into the chart.
This is by design.

Edge cases where you may see a visible "reset":

- **NVS is corrupted** (magic mismatch or CRC failure). The component
  starts at 0 Wh and HA records a discontinuity. Symptom: the
  dashboard shows a "morning spike" at boot after a long downtime.
  *Fix*: check the ESP32 boot log for `[E][rbamp_nvs]` messages —
  the cause will be there (incompatible NVS layout, low voltage at
  write time, etc.). Restore from a HA history backup or scrub the
  phantom spike with HA utilities.

- **Hardware swap** (new ESP32, same module). The component starts
  at 0 Wh — the module does not transfer its accumulated total over
  I²C (the module and the component each maintain their own
  accumulators; the module exposes only instantaneous values for
  integration). HA records a discontinuity.
  *Fix*: after flashing the new ESP32, seed the initial energy value
  via a manual `mqtt.publish` (for MQTT) or a temporary YAML edit
  with a lambda initialization. Or accept the gap as a one-off event.

- **`update_interval:` changed between flashes by a large margin**
  (e.g. 60 → 600 s). The change itself does not reset anything, but
  long cycles produce coarser "steps" in the chart. Visually it
  looks like a change of accumulation mode; functionally it is fine.

- **HA recorder truncated or restored from an old backup**. This is
  not on the ESPHome side — it is on the HA side: HA may treat some
  future values as "past" and redraw the chart. Unrelated to ESPHome.

On a normal OTA reflash, `last_reset` is not set, the NVS snapshot
is restored ~50 ms before HA connects, and the Energy dashboard sees
no discontinuity. This is the baseline scenario.

---

## 5 · Utility meter

The HA `utility_meter` integration derives sensors that reset periodically from any `total_increasing` sensor. Use it to get daily, weekly, and monthly kWh alongside the raw accumulator.

```yaml
# configuration.yaml
utility_meter:
  energy_daily:
    source: sensor.rbamp_ui1_mains_energy
    cycle: daily

  energy_monthly:
    source: sensor.rbamp_ui1_mains_energy
    cycle: monthly

  energy_yearly:
    source: sensor.rbamp_ui1_mains_energy
    cycle: yearly
```

HA creates three new entities (`sensor.energy_daily`, etc.) that reset to 0 at the start of each period. They are well suited to individual-device views in the Energy dashboard and as a basis for cost-tracking template sensors.

Cost and kWh/day examples are in [`06_examples.md §10`](06_examples.md).

---

## 6 · Statistics sensor for min / max / average

The HA `statistics` sensor computes rolling min, max, mean and standard deviation over a configurable time window from the history of any sensor:

```yaml
# configuration.yaml
sensor:
  - platform: statistics
    name: "Mains Power 1h Average"
    entity_id: sensor.rbamp_ui1_mains_power
    state_characteristic: mean
    sampling_size: 60          # last 60 points
    max_age:
      hours: 1

  - platform: statistics
    name: "Mains Power 24h Peak"
    entity_id: sensor.rbamp_ui1_mains_power
    state_characteristic: value_max
    max_age:
      hours: 24
```

The derived sensors are useful for demand management (peak-consumption window, monthly maximum for tariff billing) and anomaly detection (a 24-hour power maximum suddenly above historical norms).

---

## 7 · Long-term statistics

HA writes sensors with `state_class: total_increasing` and `state_class: measurement` into its long-term statistics database (LTS). LTS data is retained indefinitely (independent of the short-term recorder's retention).

Verifying that an rbAmp sensor lands in LTS:

1. Developer Tools → Statistics.
2. Find the entity (e.g. `sensor.rbamp_ui1_mains_energy`).
3. Make sure the row has a `statistic_id` and a recent `last_stats_ts`.

LTS is used by the Energy dashboard for historical views and is exported via HA's "Download statistics".

When the rbAmp module is replaced (new hardware, new entity ID), the old LTS history is not migrated automatically. To preserve history, keep the same `name:` for every sensor in the new YAML. ESPHome derives the entity ID from the node name plus the sensor name; matching names yield the same `statistic_id`.

---

## 8 · Lovelace cards

### Energy flow card (built-in)

The HA Energy dashboard already contains an energy flow card at the top of the Energy settings page. It is rendered once the Energy dashboard is configured (grid consumption, solar, battery). No extra setup needed.

### Gauge

```yaml
type: gauge
entity: sensor.rbamp_ui1_mains_power
min: 0
max: 5000
name: "Live Power"
unit: W
severity:
  green: 0
  yellow: 2000
  red: 3500
```

### Sensor list card

```yaml
type: entities
title: "rbAmp UI1"
entities:
  - entity: sensor.rbamp_ui1_mains_voltage
    name: Voltage
  - entity: sensor.rbamp_ui1_mains_current
    name: Current
  - entity: sensor.rbamp_ui1_mains_power
    name: Power
  - entity: sensor.rbamp_ui1_mains_power_factor
    name: Power Factor
  - entity: sensor.rbamp_ui1_mains_frequency
    name: Frequency
  - entity: sensor.rbamp_ui1_mains_energy
    name: Energy (total)
```

### Mini graph card (HACS)

Installed via HACS (§11):

```yaml
type: custom:mini-graph-card
entities:
  - entity: sensor.rbamp_ui1_mains_power
    name: Power
hours_to_show: 6
points_per_hour: 4
line_width: 2
show:
  extrema: true
  average: true
```

### Apex Charts (HACS)

```yaml
type: custom:apexcharts-card
header:
  title: "Power (W)"
  show: true
graph_span: 24h
span:
  end: now
series:
  - entity: sensor.rbamp_ui1_mains_power
    name: Power
    type: area
    stroke_width: 2
    fill_raw: last
  - entity: sensor.rbamp_ui1_mains_power
    name: 1h avg
    type: line
    stroke_width: 1
    color: orange
    group_by:
      func: avg
      duration: 1h
```

---

## 9 · HA automations vs ESPHome on_value

Both HA automations and ESPHome `on_value` callbacks react to changes in rbAmp sensors. Choosing the tool:

| Scenario | HA automation | ESPHome on_value |
|---|---|---|
| Push notification | Yes — access to all HA services (notify, TTS, etc.) | No — the ESP32 has no notification service |
| Driving a relay on the same ESP32 | Possible (HA → switch), but adds latency | Yes — runs on-device, no network dependency |
| Firing when HA is offline | No | Yes — the ESP32 acts autonomously |
| Complex conditions (entity combos, history, calendar) | Yes | Limited — C++ Lambda, no access to HA state |
| Reaction latency | ~1–5 s (Wi-Fi + API round-trip) | ~0 ms (same event loop) |
| Reaction to MQTT publish | Yes (MQTT trigger) | N/A |

**Rule of thumb**: use ESPHome `on_value` for low-latency local actions (relay toggling, LED indicator, buzzer), and HA automations for anything that needs HA services (notifications, scene activation, calendar schedules, combinations with other entities).

An example HA automation combining rbAmp and an HA switch:

```yaml
automation:
  - alias: "Cut dishwasher when peak tariff starts"
    trigger:
      - platform: time
        at: "16:00:00"              # peak tariff start
    condition:
      - condition: numeric_state
        entity_id: sensor.rbamp_ui1_mains_power
        above: 1500                 # only cut off when load is high
    action:
      - service: switch.turn_off
        target:
          entity_id: switch.dishwasher_socket
```

---

## 10 · WSL and Docker

HA often runs in Docker (via the `homeassistant/home-assistant` image or Home Assistant OS in a VM). ESPHome runs either as the HA Add-on (in the same container environment) or as a separate Docker container.

### mDNS in Docker / WSL

mDNS (multicast UDP 5353) does not cross Docker network boundaries by default. If HA is in Docker and the ESP32 is on the physical LAN:

- **Docker host networking** (`--network host` on Linux) gives the container multicast access and usually solves the problem.
- **mDNS reflector**: tools like `avahi-daemon` with a correct `allow-interfaces` or `mdns-repeater` bridge multicast between the Docker bridge (`docker0`) and the physical LAN interface.
- **Manual add**: bypass mDNS entirely — add the ESP32 to the HA ESPHome integration manually by IP.

On Windows with WSL2, the WSL2 VM has its own virtual network adapter. Docker containers inside can sit behind two NAT layers (Windows → WSL2 → Docker bridge). mDNS almost certainly will not work — add by IP.

### ESPHome Add-on vs standalone ESPHome

When using the HA ESPHome Add-on, the Add-on container shares its network namespace with HA (assuming `host` networking, which is the default for HA OS). mDNS and device discovery work without extra configuration.

When running ESPHome as a standalone Docker container, make sure the container has access to the physical LAN (`--network host` on Linux) and that `/dev/ttyUSB0` (or your COM port) is forwarded for USB flashing:

```sh
docker run -it --rm --network host \
  -v /path/to/configs:/config \
  --device /dev/ttyUSB0:/dev/ttyUSB0 \
  ghcr.io/esphome/esphome compile /config/ui1.yaml
```

### Static IP for reliable OTA

In Docker / VLAN environments, static DHCP leases for ESP32 devices with rbAmp are strongly recommended. They prevent:

- The IP address recorded in the HA ESPHome integration entry going stale.
- OTA flash failures (`esphome upload --device 192.168.x.y`).
- Any automation that uses the API by IP.

---

## 11 · HACS (Home Assistant Community Store)

HACS is a third-party integration manager for HA that gives access to community-developed cards, integrations and automations. It is not required for basic rbAmp operation, but it is useful for the richer Lovelace cards in §8.

Install from <https://hacs.xyz/>. Then install these community cards:

| Card | HACS category | Use with rbAmp |
|---|---|---|
| `mini-graph-card` | Frontend | Power / current chart over time |
| `apexcharts-card` | Frontend | Multi-series charts |
| `lovelace-card-mod` | Frontend | Custom CSS on any card |
| `energy-flow-card-plus` | Frontend | Enhanced energy flow diagram |

HACS installation and management is covered in its own documentation. Each card is documented in its own GitHub repository.

---

## 12 · Push notifications on threshold

The HA mobile app (`home-assistant.io/integrations/mobile_app`) provides push notifications to iOS and Android. Combined with rbAmp sensors, you get instant alerts for over-current, over-power, mains loss, or any other threshold:

```yaml
automation:
  - alias: "rbAmp over-current alert"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rbamp_ui1_mains_current
        above: 14.0           # alert at 14 A (below the typical 16 A breaker)
        for:
          seconds: 10
    action:
      - service: notify.mobile_app_your_phone_name
        data:
          title: "Over-current warning"
          message: >
            Current is {{ states('sensor.rbamp_ui1_mains_current') | round(2) }} A
            — approaching 16 A breaker limit.
          data:
            push:
              sound:
                name: default
                critical: 1    # critical alert bypasses Do Not Disturb (iOS)
              interruption-level: critical

  - alias: "rbAmp mains loss alert"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rbamp_ui1_mains_voltage
        below: 10
        for:
          seconds: 5
    action:
      - service: notify.mobile_app_your_phone_name
        data:
          title: "Mains power lost"
          message: "Voltage dropped to {{ states('sensor.rbamp_ui1_mains_voltage') }} V."
```

For the `notify.mobile_app_*` service to be available, the HA Companion app must be installed on the target device and the mobile-app integration configured in HA. Details at <https://companion.home-assistant.io/>.

---

## 13 · HA ESPHome Add-on setup

When using the HA ESPHome Add-on (recommended for most users):

1. The Add-on stores YAML configs in `/config/esphome/` (accessible through the HA file system or the Add-on editor UI).
2. `secrets.yaml` lives next to the device YAML files in `/config/esphome/`.
3. The `external_components` path must be accessible from inside the Add-on container. Options:
    - Copy the `components/rbamp/` directory to `/config/esphome/components/` and set `path: components/` (relative).
    - Once the component is published on GitHub, use the remote form: `source: github://rb-amp/rbamp-esphome@main`.
4. OTA: the Add-on runs the OTA flash itself — click "Install" on the device card and the Add-on will attempt OTA to the last known IP. USB flashing is also available through the Add-on's serial port if the ESP32 is plugged into the HA host.

The Add-on includes a built-in log viewer (the "Logs" button on the device card) — the equivalent of `esphome logs` on the CLI.

---

## 14 · Entity naming and entity ID stability

HA entity IDs are derived from the ESPHome node name and the `name:` field of the sensor:

```
sensor.<node_name>_<sensor_name_lowercased_spaces_to_underscores>
```

For node `rbamp-ui1` and sensor `name: "Mains Voltage"`:

```
sensor.rbamp_ui1_mains_voltage
```

Entity IDs appear in automations, template sensors, Lovelace dashboards and Energy dashboard config. Changing a sensor's `name:` in YAML creates a **new** entity ID and breaks every reference to the old one. Plan sensor names before the first deployment.

To rename a sensor without breaking its entity ID:

1. Rename the sensor in YAML.
2. Reflash the device.
3. In HA: Settings → Devices & Services → ESPHome → your device → click the sensor entity → click the gear icon → rename the entity ID back to the old value (or change it to the new one and update every reference).

An alternative is HA's "Entity ID rename" feature (Settings → Entities → search → click an entity → edit name / entity ID) — it lets you decouple the display name from the entity ID. You can rename the display name freely without affecting automations.


---

← [DIY Integrations](07_diy_integrations.md) · [Docs index](README.md) · [Schema Reference](09_api_reference.md) →
