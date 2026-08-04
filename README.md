```markdown
# eth_wifi_fallback

ESPHome external component: **Ethernet primary + WiFi Client fallback**.

Repository: https://github.com/kemak92/eth_wifi_fallback

by **@kemak92** — heungelectric, 2026

Tested on **HEUNGELECTRIC** ESP32 + LAN8720 board.

---

## Features

- Ethernet as primary connection
- Automatic WiFi Client (STA) when Ethernet is lost
- Optional static IP for WiFi (can be the same as Ethernet)
- Stops WiFi when Ethernet recovers

---

## Usage

```yaml
esphome:
  name: "esp32-eth-wifi-fallback"
  friendly_name: ESP32 ETH WiFi Fallback

esp32:
  board: esp32dev
  framework:
    type: arduino

external_components:
  - source: github://kemak92/eth_wifi_fallback@main
    components: [eth_wifi_fallback]
    refresh: 0s

# ====================== ETHERNET (Primary) ======================
ethernet:
  type: LAN8720
  mdc_pin: GPIO23
  mdio_pin: GPIO18
  clk:
    pin: GPIO17
    mode: CLK_OUT
  phy_addr: 0
  # power_pin: GPIO16

  manual_ip:
    static_ip: 192.168.1.150
    gateway: 192.168.1.1
    subnet: 255.255.255.0
    dns1: 192.168.1.1
    dns2: 8.8.8.8

# ====================== FALLBACK: STA → AP ======================
eth_wifi_fallback:
  id: my_fallback

  # WiFi nhà (STA) — ưu tiên khi mất Ethernet
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  check_interval: 15s
  sta_timeout: 45s              # hết 45s không vào STA → bật AP

  # IP cố định khi chạy STA (cùng IP Ethernet cũng được)
  manual_ip:
    static_ip: 192.168.1.150
    gateway: 192.168.1.1
    subnet: 255.255.255.0
    dns1: 192.168.1.1

  # AP cứu hộ — để trống = tự sinh theo MAC + in PASS ra log
  # ap_ssid: "HEUNG-Rescue"
  # ap_password: "matkhaumanh123"   # nếu set phải ≥ 8 ký tự

# ====================== REST ======================
logger:
  level: INFO

api:
  encryption:
    key: !secret api_key
  reboot_timeout: 0s

ota:
  - platform: esphome
    password: !secret ota_password

web_server:
  port: 80

text_sensor:
  - platform: ethernet_info
    ip_address:
      name: "IP Address (Ethernet)"
    mac_address:
      name: "MAC Address"

sensor:
  - platform: uptime
    name: "Uptime"
    update_interval: 60s

# Nút trên HA / Web: xóa WiFi đã lưu NVS → về YAML default
button:
  - platform: template
    name: "Reset WiFi to YAML default"
    on_press:
      - eth_wifi_fallback.clear_wifi:
          id: my_fallback

# Ví dụ đổi WiFi động (script / automation HA)
# - eth_wifi_fallback.save_wifi:
#     id: my_fallback
#     ssid: "NewSSID"
#     password: "NewPassword"
```

---
## Configuration Options

| Option | Required | Default | Description |
| :--- | :---: | :---: | :--- |
| `ssid` | **yes** | — | Tên Wi-Fi mặc định trong file YAML[cite: 10] |
| `password` | **yes** | — | Mật khẩu Wi-Fi mặc định trong file YAML[cite: 10] |
| `sta_timeout` | no | `45s` | Thời gian chờ nối Wi-Fi trước khi tự bật Rescue AP[cite: 10] |
| `ap_ssid` | no | Auto MAC | Tên Hotspot AP cứu hộ (Ví dụ: `HEUNG-AP-A1B2`)[cite: 8, 10] |
| `ap_password` | no | Auto MAC | Mật khẩu Hotspot AP cứu hộ[cite: 8, 10] |
| `check_interval` | no | `15s` | Chu kỳ kiểm tra tín hiệu dây cáp LAN[cite: 10] |
| `manual_ip.static_ip` | conditional | — | IP cố định cho Wi-Fi (bắt buộc nếu dùng `manual_ip`)[cite: 10] |
| `manual_ip.gateway` | conditional | — | Địa chỉ IP Gateway (bắt buộc nếu dùng `manual_ip`)[cite: 10] |
| `manual_ip.subnet` | conditional | — | Subnet Mask (bắt buộc nếu dùng `manual_ip`)[cite: 10] |
| `manual_ip.dns1` | no | — | Máy chủ DNS thứ nhất[cite: 10] |



## Example logs (HEUNGELECTRIC board)

Ethernet up → lost → WiFi fallback (same IP) → Ethernet recovered:

Khi vào AP (nhìn serial log)
```text
[W] SSID: HEUNG-AP-CC6C
[W] PASS: heung_efe7cc6c (auto from MAC)
[W] IP:   usually 192.168.4.1
Điện thoại nối WiFi đó → mở http://192.168.4.1 (nếu có web_server).
```

---

## Notes

- Do **not** declare the official `wifi:` component together with `ethernet:`.
- This component starts WiFi at low level only when Ethernet is down.
- Using the same IP for Ethernet and WiFi is supported; brief ping loss can occur during switch (ARP update).
- Warning `took a long time for an operation` on WiFi start is normal (blocking init ~100 ms).
- Tested with ESPHome 2026.7.x / ESP32 + LAN8720 (HEUNGELECTRIC board).

---

## License

Use freely. Attribution appreciated.