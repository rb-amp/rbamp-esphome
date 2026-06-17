# 08 · Home Assistant — deep dive

This chapter covers the HA-specific details of working with rbAmp: discovery, the native API, the Energy dashboard, long-term statistics, Lovelace cards, automations, and operation in environments where HA runs in Docker or WSL.

Cross-references:

- Quickstart: [`05_quickstart.md`](05_quickstart.md)
- YAML examples (Lovelace, automations): [`06_examples.md`](06_examples.md)
- DIY integrations (MQTT, InfluxDB, Lambda): [`07_diy_integrations.md`](07_diy_integrations.md)
- HA documentation: <https://www.home-assistant.io/>

---

## 1 · Discovery: mDNS or manual IP

### mDNS auto-discovery (recommended)

ESPHome nodes announce themselves over mDNS (multicast DNS) when the YAML contains an `api:` block. The HA ESPHome integration listens for these announcements and automatically offers new devices.

What auto-discovery needs in order to work:

- The ESP32 and HA must be on the **same Layer 2 network segment** (one VLAN, no router between them). mDNS is multicast and does not cross a router without extra configuration (mDNS reflector / Avahi proxy).
- UDP multicast must not be blocked by the router or by a client-isolation feature on the access point. Consumer APs often have an "AP isolation" or "wireless isolation" feature — it blocks device-to-device multicast. Disable it for the IoT VLAN or for the specific devices.
- HA must resolve mDNS correctly. In Docker / WSL deployments this sometimes requires extra network configuration (see §10).

Discovery flow:

1. Flash the ESP32 and let it boot. It connects to WiFi and starts announcing itself over mDNS as `<node_name>.local`.
2. In HA: Settings → Devices & Services → Integrations. A "New devices found" banner appears, or look for ESPHome in the integrations list with a discovery badge.
3. Click "Configure" on the new entry. HA asks for the encryption key if one is configured (§3).
4. All sensors are added to the device automatically.

### Manual add (if mDNS is blocked)

Settings → Devices & Services → Add Integration → ESPHome.

Enter the ESP32's IP (check the router's DHCP table or the ESPHome boot log — the line `[I][wifi:189]: IP: 192.168.x.y`). The default port is `6053`.

Assign a static DHCP lease for the ESP32's MAC address in the router so the IP does not change after a router reboot.

---

## 2 · Native ESPHome API (port 6053) vs MQTT

ESPHome supports two transports. Both work with rbAmp.

| Property | Native API (port 6053) | MQTT |
|---|---|---|
| HA integration | First-class: a dedicated ESPHome integration with auto-discovery | Generic: requires an MQTT broker, manual entity setup or MQTT discovery |
| Encryption | Built-in (noise protocol, optional) | Depends on the broker's TLS setup |
| Latency | Low — direct TCP with no broker | Depends on the broker |
| Reliability | Auto-reconnect | Depends on broker availability |
| Without HA | Useless (the API is HA-specific) | Any MQTT subscriber can consume it |
| Setup complexity | No extra infrastructure | Requires a running MQTT broker |

**Recommendation**: use the native API (`api:` block) when HA is the primary consumer. Add `mqtt:` only if you need a parallel non-HA consumer (Grafana, Node-RED, custom scripts). Both can coexist in one YAML.

The native API is the path described in this document. MQTT details are in [`07_diy_integrations.md §2`](07_diy_integrations.md).

---

## 3 · Encryption key

By default the native API is unencrypted — any host on the local network can connect to port 6053. That is acceptable for a trusted home network. For more security, add a key:

```yaml
api:
  encryption:
    key: "BASE64_ENCODED_32_BYTE_KEY"
```

Generating a key:

```sh
python3 -c "import base64, os; print(base64.b64encode(os.urandom(32)).decode())"
```

When you add the device to HA (Settings → Devices & Services → ESPHome → Add), enter the same key. HA stores it in the integration's configuration and uses it automatically for all subsequent connections to this node.

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

If you change the key, you must remove and re-add the device in HA.

---

## 4 · Energy dashboard configuration

The rbAmp `energy` sensors (and `energy_1`, `energy_2`, `energy_exported`, etc.) arrive already configured with:

```
device_class: energy
state_class: total_increasing
unit_of_measurement: Wh
```

This is exactly the combination the HA Energy dashboard expects for cumulative consumption or generation sensors. No extra YAML is needed.

### Adding sensors to the Energy dashboard

Settings → Dashboards → Energy. The dashboard has four sections:

| Section | Which rbAmp sensor to use |
|---|---|
| Grid consumption | `energy` (import from the grid) |
| Return to grid | `energy_exported` (STANDARD / PRO tiers; reads 0 on BASIC and v1 firmware) |
| Solar panels | `energy` from a module measuring the solar inverter's output |
| Home battery | Not applicable (rbAmp does not measure DC) |
| Individual devices | `energy_1`, `energy_2`, etc. from a multi-channel module |

For a single whole-home install:

1. Click "Add consumption" under Grid consumption.
2. Select the `Mains Energy` entity.
3. Optionally set a tariff (EUR/kWh or your local currency) for cost tracking.

For a three-module install (grid + solar + EV from Example 11 in [`06_examples.md`](06_examples.md)):

1. Grid consumption: `House Energy` (from `meter_house`).
2. Return to grid: `Solar Energy Exported` (from `meter_solar`, STANDARD / PRO tiers; leave empty on BASIC).
3. Solar panels: `Solar Energy` (from `meter_solar`).
4. Individual device: `EV Charger Energy` (from `meter_evcharger`).

### NVS persistence and the Energy dashboard

The ESPHome component saves total energy to the ESP32's NVS every 5 minutes. On boot the values are restored from NVS and published immediately — **before** HA connects and **before** the first `update_interval` fires.

This keeps the Energy dashboard from interpreting a momentary `0 Wh` as a counter reset. If HA sees a `total_increasing` value drop to 0, it treats the difference as a billing-period boundary and discards the accumulated history.

The worst-case data loss on a sudden power failure is up to 5 minutes of accumulation (≈5 Wh at an average 60 W load — invisible in daily totals).

### `last_reset` after OTA or an ESP32 reboot

The HA Energy dashboard uses device_class `energy` + state_class
`total_increasing` with no explicit `last_reset` binding — this is the
recommended ESPHome configuration. On an ESP32 reboot the
component restores the value from NVS **before** the first `publish_state`,
so HA sees a continuous, monotonically increasing series and does **not** insert
a sentinel reset into the graph. This works by design.

Edge cases in which you may see a visible "reset":

- **NVS is corrupted** (the magic does not match or the CRC fails). The component
  starts at 0 Wh and HA records a discontinuity. Symptom: the dashboard shows
  a "morning spike" at the moment the node boots after a long idle period.
  *Fix*: check the ESP32 boot log for
  `[E][rbamp_nvs]` messages — they point to the cause (an incompatible
  NVS layout, low charge at the moment of the write, and so on). Restore from a backup
  of the HA history or scrub the phantom spike with the HA utilities.

- **Hardware replacement** (new ESP32, same module). The component starts
  at 0 Wh — the module does not pass its accumulated count over I²C (the module and
  the component keep separate accumulators; the module only provides
  instantaneous values to integrate). HA records a discontinuity.
  *Fix*: after flashing the new ESP32, seed the initial energy value with a manual `mqtt.publish` (for MQTT)
  or a temporary YAML edit with a lambda initialization.
  Or accept the discontinuity as a one-time event.

- **`update_interval:` changed between firmware builds by a noticeable amount**
  (for example 60 → 600 s). The change by itself resets nothing, but
  longer cycles produce coarser "steps" in the graph. Visually
  it looks like a change in accumulation mode; functionally it is OK.

- **The HA recorder was truncated or restored from an old backup**. This is not
  on the ESPHome side — it is the HA side: HA may treat some
  future values as "past" and redraw the graph. It has nothing to do
  with ESPHome.

During a routine OTA reflash, `last_reset` is not set, the NVS snapshot is
restored ~50 ms before HA connects, and the Energy dashboard does not
see a discontinuity. This is the baseline scenario.

---

## 5 · Utility meter

The HA `utility_meter` integration derives periodically-resetting sensors from any `total_increasing` sensor. Use it to get daily, weekly, and monthly kWh in addition to the raw accumulator.

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

HA creates three new entities (`sensor.energy_daily`, etc.) that reset to 0 at the start of each period. They are great for individual device views in the Energy dashboard and as the basis for cost-tracking template sensors.

Cost and kWh/day examples are in [`06_examples.md §10`](06_examples.md).

---

## 6 · Statistics sensor for min / max / average

The HA `statistics` sensor computes rolling min, max, mean, and standard deviation over a configurable time window from the history of any sensor:

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

These derived sensors are useful for demand management (peak-consumption window, monthly maximum for tariff billing) and anomaly detection (if the 24-hour power maximum suddenly exceeds historical norms).

---

## 7 · Long-term statistics

HA writes sensors with `state_class: total_increasing` and `state_class: measurement` into its long-term statistics database (LTS). LTS data is kept indefinitely (independent of the short-term recorder's retention).

To verify that an rbAmp sensor lands in LTS:

1. Developer Tools → Statistics.
2. Find the entity (for example `sensor.rbamp_ui1_mains_energy`).
3. Confirm that its row has a `statistic_id` and a recent `last_stats_ts`.

LTS is used by the Energy dashboard for historical views and is exported through HA's "Download statistics".

When you replace an rbAmp module (new hardware, new entity ID), the old LTS history is not migrated automatically. To preserve the history, keep the same `name:` for each sensor in the new YAML. ESPHome generates the entity ID from the node name plus the sensor name; matching names yield the same `statistic_id`.

---

## 8 · Lovelace cards

### Energy flow card (built-in)

The built-in HA Energy dashboard already includes an energy flow card at the top of the Energy settings page. It is drawn once the Energy dashboard is configured (grid consumption, solar, battery). No extra configuration is needed.

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
| Driving a relay on the same ESP32 | Possible (HA → switch), but adds latency | Yes — runs on-device, with no network dependency |
| Triggering when HA is offline | No | Yes — the ESP32 acts autonomously |
| Complex conditions (combining entities, history, calendar) | Yes | Limited — Lambda in C++, with no access to HA state |
| Reaction latency | ~1–5 s (WiFi + API round-trip) | ~0 ms (the same event loop) |
| Reacting to an MQTT publish | Yes (MQTT trigger) | N/A |

**Practical advice**: use ESPHome `on_value` for low-latency local actions (toggling a relay, an LED indicator, a buzzer), and HA automations for anything that needs HA services (notifications, scene activation, calendar scheduling, combinations with other entities).

Example HA automation — combining rbAmp and an HA switch:

```yaml
automation:
  - alias: "Cut dishwasher when peak tariff starts"
    trigger:
      - platform: time
        at: "16:00:00"              # start of the peak tariff
    condition:
      - condition: numeric_state
        entity_id: sensor.rbamp_ui1_mains_power
        above: 1500                 # only switch off if the load is high
    action:
      - service: switch.turn_off
        target:
          entity_id: switch.dishwasher_socket
```

---

## 10 · WSL and Docker

HA often runs in Docker (via the `homeassistant/home-assistant` image or Home Assistant OS in a VM). ESPHome runs either as an HA Add-on (in the same container environment) or as a separate Docker container.

### mDNS in Docker / WSL

mDNS (multicast UDP 5353) does not cross Docker network boundaries by default. If HA is in Docker and the ESP32 is on the physical LAN:

- **Docker host networking** (`--network host` on Linux) gives the container access to multicast and usually solves the problem.
- **mDNS reflector**: tools like `avahi-daemon` with a correct `allow-interfaces` or `mdns-repeater` bridge multicast between the Docker bridge (`docker0`) and the physical LAN interface.
- **Manual add**: bypass mDNS entirely — add the ESP32 to the HA ESPHome integration manually by IP.

On Windows with WSL2, the WSL2 VM has its own virtual network adapter. Docker containers inside it can be behind two NAT layers (Windows → WSL2 → Docker bridge). mDNS almost certainly will not work — add by IP.

### ESPHome Add-on vs standalone ESPHome

When you use the HA ESPHome Add-on, the Add-on container shares the network namespace with HA (assuming `host` networking, which is the default for HA OS). mDNS and device discovery work with no extra configuration.

When you run ESPHome as a standalone Docker container, make sure the container has access to the physical LAN (`--network host` on Linux) and that `/dev/ttyUSB0` (or your COM port) is passed through for USB flashing:

```sh
docker run -it --rm --network host \
  -v /path/to/configs:/config \
  --device /dev/ttyUSB0:/dev/ttyUSB0 \
  ghcr.io/esphome/esphome compile /config/ui1.yaml
```

### Static IP for reliable OTA

In a Docker / VLAN environment, static DHCP leases for rbAmp ESP32 devices are strongly recommended. They prevent:

- A stale IP address recorded in HA for the ESPHome integration entry.
- A failed OTA flash (`esphome upload --device 192.168.x.y`).
- A failure of any automation that uses the API by IP.

---

## 11 · HACS (Home Assistant Community Store)

HACS is a third-party integration manager for HA that provides access to community-developed cards, integrations, and automations. It is not required for basic rbAmp operation, but it is useful for the richer Lovelace cards from §8.

Install it from <https://hacs.xyz/>. Then install these community cards:

| Card | HACS category | Use with rbAmp |
|---|---|---|
| `mini-graph-card` | Frontend | Power / current graph over time |
| `apexcharts-card` | Frontend | Multi-series charts |
| `lovelace-card-mod` | Frontend | Custom CSS on any card |
| `energy-flow-card-plus` | Frontend | Improved energy-flow diagram |

Installing and managing HACS is covered in its documentation. The cards are documented in their respective GitHub repositories.

---

## 12 · Threshold push notifications

The HA mobile app (`home-assistant.io/integrations/mobile_app`) provides push notifications on iOS and Android. Combined with rbAmp sensors, you get instant alerts on over-current, over-power, loss of mains, or any other threshold:

```yaml
automation:
  - alias: "rbAmp over-current alert"
    trigger:
      - platform: numeric_state
        entity_id: sensor.rbamp_ui1_mains_current
        above: 14.0           # alert at 14 A (below a typical 16 A breaker)
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
                critical: 1    # a critical alert bypasses Do Not Disturb (iOS)
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

For the `notify.mobile_app_*` service to be available, the HA Companion app must be installed on the target device and the mobile-app integration must be set up in HA. Details are at <https://companion.home-assistant.io/>.

---

## 13 · HA ESPHome Add-on setup

When you use the HA ESPHome Add-on (recommended for most users):

1. The Add-on stores the YAML configs in `/config/esphome/` (accessible through the HA file system or the Add-on's editor UI).
2. `secrets.yaml` lives next to the device YAML files in `/config/esphome/`.
3. The `external_components` path must be reachable from inside the Add-on container. Options:
    - Copy the `components/rbamp/` directory into `/config/esphome/components/` and specify `path: components/` (relative).
    - Once the component is published on GitHub, use the remote form: `source: github://rb-amp/rbamp-esphome@main`.
4. OTA: the Add-on performs the OTA flash itself — click "Install" on the device card and the Add-on attempts an OTA to the last known IP. USB flashing is also available through the Add-on's serial port if the ESP32 is connected to the HA host.

The Add-on includes a built-in log viewer (the "Logs" button on the device card) — the equivalent of `esphome logs` in the CLI.

---

## 14 · Entity naming and entity ID stability

Entity IDs in HA are derived from the ESPHome node name and the sensor's `name:` field:

```
sensor.<node_name>_<sensor_name_lowercased_spaces_to_underscores>
```

For the node `rbamp-ui1` and a sensor with `name: "Mains Voltage"`:

```
sensor.rbamp_ui1_mains_voltage
```

Entity IDs appear in automations, template sensors, Lovelace dashboards, and the Energy dashboard config. Changing a sensor's `name:` in the YAML creates a **new** entity ID and breaks every reference to the old one. Plan your sensor names before the first deployment.

To change a sensor's name without breaking the entity ID:

1. Rename the sensor in the YAML.
2. Reflash the device.
3. In HA: Settings → Devices & Services → ESPHome → your device → click the sensor entity → click the gear icon → rename the entity ID back to the old value (or change it to a new one and update all references).

An alternative is the HA "Entity ID rename" feature (Settings → Entities → search → click the entity → edit name / entity ID) — it lets you decouple the display name from the entity ID. You can freely rename the display name without affecting automations.
