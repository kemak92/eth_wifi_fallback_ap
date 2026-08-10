# eth_wifi_fallback

ESPHome external component: **Ethernet primary → WiFi STA fallback → WiFi AP rescue**  
with **NVS WiFi credentials** and a **config web page** (AP mode only).

by **@kemak92** — heungelectric, 2026

Tested on **HEUNGELECTRIC** ESP32 + LAN8720, ESPHome **2026.7.x** / ESP-IDF.

---

## Features

| Feature | Description |
|--------|-------------|
| Ethernet primary | Preferred link when cable is up |
| WiFi STA fallback | Connects to SSID from NVS (or YAML default) when Ethernet is lost |
| WiFi AP rescue | SoftAP + config portal if STA fails |
| NVS storage | Save / clear WiFi credentials without reflashing |
| Config page | `http://192.168.4.1/` (or `:8080` if port 80 is taken by `web_server`) |
| Security | Config HTTP **only** on rescue AP — not exposed on LAN when STA/ETH is up |
| Actions | `eth_wifi_fallback.save_wifi` / `clear_wifi` for automations |

**Flow**

```
Ethernet OK  →  use Ethernet, WiFi off
Ethernet lost → try WiFi STA (NVS → else YAML ssid/password)
STA OK       → use WiFi (API / OTA / web_server as usual)
STA fail     → start rescue AP + config portal
User saves   → credentials → NVS → switch to STA immediately
Ethernet back → stop WiFi + portal
```

---

## Install

```yaml
external_components:
  - source: github://kemak92/eth_wifi_fallback_ap@main
    components: [eth_wifi_fallback]
    refresh: 0s
```

**Requirements**

- ESP32 + ESP-IDF (`framework: type: esp-idf`)
- `ethernet:` component (this component does **not** use ESPHome `wifi:`)
- DHCP server enabled for SoftAP (see YAML below)

---

## Minimal configuration

```yaml
esphome:
  name: heung-eth-fallback

esp32:
  board: esp32dev
  framework:
    type: esp-idf
    advanced:
      enable_lwip_dhcp_server: true   # required for rescue AP clients

external_components:
  - source: github://kemak92/eth_wifi_fallback_ap@main
    components: [eth_wifi_fallback]
    refresh: 0s

ethernet:
  type: LAN8720
  mdc_pin: GPIO23
  mdio_pin: GPIO18
  clk:
    pin: GPIO17
    mode: CLK_OUT
  phy_addr: 0
  manual_ip:
    static_ip: 192.168.1.150
    gateway: 192.168.1.1
    subnet: 255.255.255.0
    dns1: 192.168.1.1

eth_wifi_fallback:
  ssid: "YourHomeWiFi"          # default when NVS empty
  password: "YourPassword"
  sta_timeout: 45s              # wait for STA before opening AP
  check_interval: 15s
  # ap_ssid: "HEUNG-AP-XXXX"    # optional; default from MAC
  # ap_password: "........"     # optional; ≥8 chars, else auto from MAC
  manual_ip:                    # optional static IP on WiFi STA
    static_ip: 192.168.1.150
    gateway: 192.168.1.1
    subnet: 255.255.255.0
    dns1: 192.168.1.1

logger:
  level: DEBUG

api:
  reboot_timeout: 0s            # avoid reboot while switching links

ota:
  - platform: esphome

# Optional: dashboard when on ETH / STA (uses port 80)
# Config portal then moves to :8080 automatically
web_server:
  port: 80
```

---

## Config portal (rescue AP)

When STA cannot connect, the device starts SoftAP:

| Item | Default |
|------|---------|
| SSID | `HEUNG-AP-XXXX` (last 2 bytes of MAC) |
| Password | `heung_` + 8 hex digits from MAC |
| IP | `192.168.4.1` |
| URL | `http://192.168.4.1/` or `http://192.168.4.1:8080/` |

Exact SSID / PASS / URL are printed in the log:

```text
========== RESCUE AP ==========
SSID: HEUNG-AP-CC6C
PASS: heung_efe7cc6c (auto from MAC)
IP:   192.168.4.1
URL:  http://192.168.4.1:8080/
================================
```

**Phone tips**

1. Turn **off mobile data** (or use Airplane mode + WiFi only).
2. Join the rescue AP.
3. Open the **URL from the log** (include port if not 80).
4. Scan networks → choose SSID → enter password → **Save**.
5. Device stores credentials in NVS and connects as STA.

---

## Options

| Option | Required | Default | Description |
|--------|----------|---------|-------------|
| `ssid` | yes | — | Default STA SSID if NVS empty |
| `password` | yes | — | Default STA password |
| `sta_timeout` | no | `45s` | Time to wait for STA before AP |
| `check_interval` | no | `15s` | How often to check Ethernet |
| `ap_ssid` | no | from MAC | Rescue AP SSID |
| `ap_password` | no | from MAC | Rescue AP password (≥ 8 chars) |
| `manual_ip.*` | no | DHCP | Static IP on WiFi STA |

---

## Automations: button & switch examples

### 1) Template button — clear NVS (back to YAML WiFi)

```yaml
button:
  - platform: template
    name: "Clear saved WiFi (NVS)"
    icon: mdi:wifi-remove
    on_press:
      - eth_wifi_fallback.clear_wifi:
```

### 2) Template button — save fixed credentials

```yaml
button:
  - platform: template
    name: "Save factory WiFi"
    icon: mdi:wifi-lock
    on_press:
      - eth_wifi_fallback.save_wifi:
          ssid: "FactorySSID"
          password: "FactoryPassword"
```

### 3) Physical button (GPIO) — clear NVS on long press

```yaml
binary_sensor:
  - platform: gpio
    pin:
      number: GPIO0          # change to your button pin
      mode: INPUT_PULLUP
      inverted: true
    name: "Config button"
    filters:
      - delayed_on: 50ms
    on_click:
      min_length: 3s
      max_length: 10s
      then:
        - logger.log: "Clearing NVS WiFi credentials"
        - eth_wifi_fallback.clear_wifi:
```

### 4) Switch — “use YAML WiFi” vs keep NVS

Useful as a Home Assistant control: turn **ON** = clear NVS (force YAML defaults on next STA attempt).

```yaml
switch:
  - platform: template
    name: "Force YAML WiFi (clear NVS)"
    id: force_yaml_wifi
    optimistic: true
    restore_mode: ALWAYS_OFF
    turn_on_action:
      - eth_wifi_fallback.clear_wifi:
      - logger.log: "NVS cleared — YAML SSID will be used"
    turn_off_action:
      - logger.log: "NVS not modified"
```

### 5) Save WiFi from Home Assistant text inputs

```yaml
input_text:   # only if using HA helpers via api — or use text sensors / globals

# Simpler pattern with globals + template button:
globals:
  - id: g_ssid
    type: std::string
    restore_value: no
    initial_value: '"MySSID"'
  - id: g_pass
    type: std::string
    restore_value: no
    initial_value: '""'

button:
  - platform: template
    name: "Apply WiFi from globals"
    on_press:
      - eth_wifi_fallback.save_wifi:
          ssid: !lambda return id(g_ssid);
          password: !lambda return id(g_pass);
```

### 6) Status binary sensor (optional helper)

ESPHome does not expose eth_wifi_fallback state as a built-in sensor; you can approximate with Ethernet status:

```yaml
binary_sensor:
  - platform: status
    name: "Node status"

# Ethernet link is already reflected by the ethernet component connectivity.
```

---

## Security notes

- The config portal is **not** started when Ethernet or WiFi STA is connected.
- Anyone who can join the rescue AP can change WiFi credentials — set a strong `ap_password` in production if needed.
- Prefer `api:` encryption and OTA password as usual.

---

## Troubleshooting

| Symptom | Cause / fix |
|---------|-------------|
| `DHCP server start failed` | Set `enable_lwip_dhcp_server: true` and **clean build** |
| `httpd error in listen (112)` | Port 80 used by `web_server` — portal uses **8080**; open URL from log |
| Phone connects AP but page timeout | Disable mobile data; try `http://192.168.4.1:8080/` |
| Blank page on `:80` | That is ESPHome `web_server`, not the portal |
| STA never connects | Check NVS SSID/password; use **Clear NVS** button and retry |
| Ethernet flapping in log | Cable/PHY issue; fallback still works on WiFi |

---

## Actions reference

```yaml
# Save credentials to NVS and switch to STA (if WiFi already active)
- eth_wifi_fallback.save_wifi:
    ssid: "NewSSID"
    password: "NewPassword"

# Erase NVS → next STA attempt uses YAML ssid/password
- eth_wifi_fallback.clear_wifi:
```

---

## License / credit

Based on ideas from [kemak92/eth_wifi_fallback](https://github.com/kemak92/eth_wifi_fallback).  
SoftAP + NVS + config portal extensions — heungelectric, 2026.
