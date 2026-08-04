// This ESPHome component wraps around the repo by @kemak92:
// https://github.com/kemak92/eth_wifi_fallback_ap
//
// Ethernet primary → WiFi STA → WiFi AP fallback with Dynamic NVS WiFi Config.
// by @kemak92 - heungelectric, 2026
//
// + /wifi web page (SSID + Password form, scan, save to NVS)

#include "eth_wifi_fallback.h"
#include "esphome/core/log.h"
#include "esphome/components/ethernet/ethernet_component.h"

#include <cstring>
#include <algorithm>
#include <sstream>

namespace esphome {
namespace eth_wifi_fallback {

static const char *const TAG = "eth_wifi_fallback";
static const uint32_t NVS_CREDS_KEY = 0x8F23A1B2;

EthWifiFallback *EthWifiFallback::instance_ = nullptr;

void EthWifiFallback::setup() {
  instance_ = this;
  this->pref_creds_ = global_preferences->make_preference<WifiCredentials>(NVS_CREDS_KEY, true);
  this->load_credentials_();
  ESP_LOGI(TAG, "EthWifiFallback ready (STA SSID: %s)", this->active_ssid_.c_str());
}

void EthWifiFallback::load_credentials_() {
  WifiCredentials creds{};
  if (this->pref_creds_.load(&creds) && creds.ssid[0] != '\0') {
    this->active_ssid_ = std::string(creds.ssid);
    this->active_password_ = std::string(creds.password);
    ESP_LOGI(TAG, "Loaded WiFi from NVS: %s", this->active_ssid_.c_str());
  } else {
    this->active_ssid_ = this->yaml_ssid_;
    this->active_password_ = this->yaml_password_;
    ESP_LOGI(TAG, "Using YAML WiFi default: %s", this->active_ssid_.c_str());
  }
}

void EthWifiFallback::save_wifi_credentials(const std::string &ssid, const std::string &password) {
  WifiCredentials creds{};
  strncpy(creds.ssid, ssid.c_str(), sizeof(creds.ssid) - 1);
  strncpy(creds.password, password.c_str(), sizeof(creds.password) - 1);
  this->pref_creds_.save(&creds);
  global_preferences->sync();

  this->active_ssid_ = ssid;
  this->active_password_ = password;
  ESP_LOGI(TAG, "WiFi credentials saved to NVS: %s", ssid.c_str());

  // Switch to new STA credentials immediately (even if currently in AP)
  if (this->state_ != FallbackState::IDLE && this->state_ != FallbackState::STOPPING) {
    ESP_LOGI(TAG, "Switching to new STA credentials...");
    this->stop_http_server_();
    this->stop_wifi_();
    this->state_ = FallbackState::STARTING_STA;
    this->state_enter_time_ = millis();
    this->start_sta_();
  }
}

void EthWifiFallback::clear_saved_credentials() {
  WifiCredentials empty{};
  this->pref_creds_.save(&empty);
  global_preferences->sync();
  this->active_ssid_ = this->yaml_ssid_;
  this->active_password_ = this->yaml_password_;
  ESP_LOGI(TAG, "Cleared NVS → back to YAML default");
}

void EthWifiFallback::loop() {
  const uint32_t now = millis();
  if (now - this->last_check_ < this->check_interval_)
    return;
  this->last_check_ = now;

  bool eth_connected = false;
  if (ethernet::global_eth_component != nullptr)
    eth_connected = ethernet::global_eth_component->is_connected();

  if (eth_connected && this->state_ != FallbackState::IDLE && this->state_ != FallbackState::STOPPING) {
    ESP_LOGI(TAG, "Ethernet recovered → stopping WiFi");
    this->stop_http_server_();
    this->state_ = FallbackState::STOPPING;
    this->stop_wifi_();
    return;
  }

  switch (this->state_) {
    case FallbackState::IDLE:
      if (!eth_connected) {
        ESP_LOGW(TAG, "Ethernet lost → starting WiFi STA");
        this->state_ = FallbackState::STARTING_STA;
        this->state_enter_time_ = now;
        this->start_sta_();
      }
      break;

    case FallbackState::STARTING_STA:
      if (this->is_sta_connected_()) {
        ESP_LOGI(TAG, "WiFi STA connected");
        this->log_sta_ip_();
        this->state_ = FallbackState::STA_ACTIVE;
        // Keep HTTP server running so /wifi is still reachable over STA
        this->start_http_server_();
      } else if (now - this->state_enter_time_ > this->sta_timeout_) {
        ESP_LOGW(TAG, "STA timeout → starting AP");
        this->stop_wifi_();
        this->state_ = FallbackState::STARTING_AP;
        this->state_enter_time_ = now;
        this->start_ap_();
      } else {
        esp_wifi_connect();
      }
      break;

    case FallbackState::STA_ACTIVE:
      if (!this->is_sta_connected_()) {
        ESP_LOGW(TAG, "STA lost → restart STA timeout (then AP if still fail)");
        this->stop_http_server_();
        this->state_ = FallbackState::STARTING_STA;
        this->state_enter_time_ = now;
        esp_wifi_connect();
      }
      break;

    case FallbackState::STARTING_AP:
      this->state_ = FallbackState::AP_ACTIVE;
      this->start_http_server_();
      this->start_scan_();  // auto-scan when AP starts
      break;

    case FallbackState::AP_ACTIVE:
      break;

    case FallbackState::STOPPING:
      this->state_ = FallbackState::IDLE;
      break;
  }
}

void EthWifiFallback::ensure_wifi_init_() {
  if (this->wifi_initialized_)
    return;
  esp_netif_init();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_err_t err = esp_wifi_init(&cfg);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
    return;
  }
  this->wifi_initialized_ = true;
}

std::string EthWifiFallback::get_effective_ap_ssid_() {
  if (!this->ap_ssid_.empty())
    return this->ap_ssid_;
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[32];
  snprintf(buf, sizeof(buf), "HEUNG-AP-%02X%02X", mac[4], mac[5]);
  return std::string(buf);
}

std::string EthWifiFallback::get_effective_ap_password_() {
  if (!this->ap_password_.empty())
    return this->ap_password_;
  // Auto password from MAC (≥ 8 chars)
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[32];
  snprintf(buf, sizeof(buf), "heung_%02x%02x%02x%02x", mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}

void EthWifiFallback::start_sta_() {
  this->ensure_wifi_init_();
  if (this->sta_netif_ == nullptr)
    this->sta_netif_ = esp_netif_create_default_wifi_sta();

  if (this->sta_netif_ != nullptr) {
    if (this->use_manual_ip_) {
      esp_netif_dhcpc_stop(this->sta_netif_);
      esp_netif_ip_info_t ip_info = {};
      ip_info.ip.addr = htonl(this->static_ip_);
      ip_info.gw.addr = htonl(this->gateway_);
      ip_info.netmask.addr = htonl(this->subnet_);
      esp_netif_set_ip_info(this->sta_netif_, &ip_info);
      if (this->dns1_ != 0) {
        esp_netif_dns_info_t dns = {};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = htonl(this->dns1_);
        esp_netif_set_dns_info(this->sta_netif_, ESP_NETIF_DNS_MAIN, &dns);
      }
      ESP_LOGI(TAG, "STA using static IP");
    } else {
      esp_netif_dhcpc_start(this->sta_netif_);
      ESP_LOGI(TAG, "STA using DHCP");
    }
  }

  esp_wifi_set_mode(WIFI_MODE_STA);
  wifi_config_t cfg = {};
  strncpy((char *) cfg.sta.ssid, this->active_ssid_.c_str(), sizeof(cfg.sta.ssid) - 1);
  strncpy((char *) cfg.sta.password, this->active_password_.c_str(), sizeof(cfg.sta.password) - 1);
  cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
  esp_wifi_set_config(WIFI_IF_STA, &cfg);
  esp_wifi_start();
  esp_wifi_connect();
  ESP_LOGI(TAG, "STA start (SSID: %s)", this->active_ssid_.c_str());
}

void EthWifiFallback::start_ap_() {
  this->ensure_wifi_init_();
  if (this->ap_netif_ == nullptr)
    this->ap_netif_ = esp_netif_create_default_wifi_ap();

  const std::string ap_ssid = this->get_effective_ap_ssid_();
  const std::string ap_pass = this->get_effective_ap_password_();
  const bool auto_pass = this->ap_password_.empty();

  esp_wifi_set_mode(WIFI_MODE_AP);
  wifi_config_t cfg = {};
  strncpy((char *) cfg.ap.ssid, ap_ssid.c_str(), sizeof(cfg.ap.ssid) - 1);
  strncpy((char *) cfg.ap.password, ap_pass.c_str(), sizeof(cfg.ap.password) - 1);
  cfg.ap.ssid_len = ap_ssid.length();
  cfg.ap.channel = 1;
  cfg.ap.max_connection = 4;
  cfg.ap.authmode = (ap_pass.length() >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

  esp_wifi_set_config(WIFI_IF_AP, &cfg);
  esp_wifi_start();

  ESP_LOGW(TAG, "========== RESCUE AP ==========");
  ESP_LOGW(TAG, "SSID: %s", ap_ssid.c_str());
  ESP_LOGW(TAG, "PASS: %s%s", ap_pass.c_str(), auto_pass ? " (auto from MAC)" : "");
  ESP_LOGW(TAG, "IP:   192.168.4.1");
  ESP_LOGW(TAG, "URL:  http://192.168.4.1/wifi");
  ESP_LOGW(TAG, "================================");
}

void EthWifiFallback::stop_wifi_() {
  esp_wifi_disconnect();
  esp_wifi_stop();
  ESP_LOGI(TAG, "WiFi stopped");
}

bool EthWifiFallback::is_sta_connected_() {
  wifi_ap_record_t info;
  return esp_wifi_sta_get_ap_info(&info) == ESP_OK;
}

void EthWifiFallback::log_sta_ip_() {
  if (this->sta_netif_ == nullptr)
    return;
  esp_netif_ip_info_t ip;
  if (esp_netif_get_ip_info(this->sta_netif_, &ip) == ESP_OK) {
    ESP_LOGI(TAG, "STA IP: " IPSTR, IP2STR(&ip.ip));
    ESP_LOGI(TAG, "Gateway: " IPSTR, IP2STR(&ip.gw));
  }
}

// ───────────────────────── HTTP server ─────────────────────────

void EthWifiFallback::start_http_server_() {
  if (this->http_server_ != nullptr)
    return;

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.max_uri_handlers = 8;
  config.stack_size = 8192;

  if (httpd_start(&this->http_server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server");
    this->http_server_ = nullptr;
    return;
  }

  httpd_uri_t uri_root = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = handle_root_,
      .user_ctx = this,
  };
  httpd_uri_t uri_wifi_get = {
      .uri = "/wifi",
      .method = HTTP_GET,
      .handler = handle_wifi_get_,
      .user_ctx = this,
  };
  httpd_uri_t uri_wifi_post = {
      .uri = "/wifi",
      .method = HTTP_POST,
      .handler = handle_wifi_post_,
      .user_ctx = this,
  };
  httpd_uri_t uri_scan = {
      .uri = "/scan",
      .method = HTTP_GET,
      .handler = handle_scan_,
      .user_ctx = this,
  };
  httpd_uri_t uri_clear = {
      .uri = "/clear",
      .method = HTTP_POST,
      .handler = handle_clear_,
      .user_ctx = this,
  };

  httpd_register_uri_handler(this->http_server_, &uri_root);
  httpd_register_uri_handler(this->http_server_, &uri_wifi_get);
  httpd_register_uri_handler(this->http_server_, &uri_wifi_post);
  httpd_register_uri_handler(this->http_server_, &uri_scan);
  httpd_register_uri_handler(this->http_server_, &uri_clear);

  ESP_LOGI(TAG, "HTTP server started → http://<ip>/wifi");
}

void EthWifiFallback::stop_http_server_() {
  if (this->http_server_ == nullptr)
    return;
  httpd_stop(this->http_server_);
  this->http_server_ = nullptr;
  ESP_LOGI(TAG, "HTTP server stopped");
}

void EthWifiFallback::start_scan_() {
  if (this->scan_in_progress_)
    return;
  this->scan_in_progress_ = true;
  this->scan_results_.clear();

  wifi_scan_config_t scan_cfg = {};
  scan_cfg.ssid = nullptr;
  scan_cfg.bssid = nullptr;
  scan_cfg.channel = 0;
  scan_cfg.show_hidden = true;
  scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
  scan_cfg.scan_time.active.min = 100;
  scan_cfg.scan_time.active.max = 300;

  esp_err_t err = esp_wifi_scan_start(&scan_cfg, false);  // async
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Scan start failed: %s", esp_err_to_name(err));
    this->scan_in_progress_ = false;
    return;
  }
  ESP_LOGI(TAG, "WiFi scan started");
}

void EthWifiFallback::collect_scan_results_() {
  uint16_t ap_count = 0;
  esp_wifi_scan_get_ap_num(&ap_count);
  if (ap_count == 0) {
    this->scan_in_progress_ = false;
    return;
  }
  if (ap_count > 30)
    ap_count = 30;

  wifi_ap_record_t *ap_records = new wifi_ap_record_t[ap_count];
  if (ap_records == nullptr) {
    this->scan_in_progress_ = false;
    return;
  }

  if (esp_wifi_scan_get_ap_records(&ap_count, ap_records) == ESP_OK) {
    this->scan_results_.clear();
    for (uint16_t i = 0; i < ap_count; i++) {
      if (ap_records[i].ssid[0] == '\0')
        continue;  // skip hidden empty
      ScanResult r;
      r.ssid = std::string(reinterpret_cast<char *>(ap_records[i].ssid));
      r.rssi = ap_records[i].rssi;
      r.authmode = ap_records[i].authmode;
      // dedupe by SSID (keep strongest)
      bool found = false;
      for (auto &existing : this->scan_results_) {
        if (existing.ssid == r.ssid) {
          if (r.rssi > existing.rssi) {
            existing.rssi = r.rssi;
            existing.authmode = r.authmode;
          }
          found = true;
          break;
        }
      }
      if (!found)
        this->scan_results_.push_back(r);
    }
    // sort by RSSI desc
    std::sort(this->scan_results_.begin(), this->scan_results_.end(),
              [](const ScanResult &a, const ScanResult &b) { return a.rssi > b.rssi; });
  }
  delete[] ap_records;
  this->scan_in_progress_ = false;
  ESP_LOGI(TAG, "Scan done, %d unique networks", (int) this->scan_results_.size());
}

std::string EthWifiFallback::build_wifi_page_html_() {
  // Try to refresh scan results if a scan finished
  if (this->scan_in_progress_) {
    uint16_t ap_num = 0;
    if (esp_wifi_scan_get_ap_num(&ap_num) == ESP_OK && ap_num > 0) {
      this->collect_scan_results_();
    }
  }

  std::ostringstream html;
  html << R"html(<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>WiFi Config – HEUNG</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;
       background:#0f172a;color:#e2e8f0;min-height:100vh;padding:16px}
  .card{max-width:420px;margin:0 auto;background:#1e293b;border-radius:16px;
        padding:24px;box-shadow:0 8px 32px rgba(0,0,0,.4)}
  h1{font-size:1.4rem;margin-bottom:4px;color:#38bdf8}
  .sub{font-size:.85rem;color:#94a3b8;margin-bottom:20px}
  label{display:block;font-size:.8rem;color:#94a3b8;margin:12px 0 4px}
  input[type=text],input[type=password]{width:100%;padding:12px;border-radius:10px;
       border:1px solid #334155;background:#0f172a;color:#f1f5f9;font-size:1rem}
  input:focus{outline:none;border-color:#38bdf8}
  .btn{display:block;width:100%;padding:14px;margin-top:16px;border:none;
       border-radius:10px;font-size:1rem;font-weight:600;cursor:pointer}
  .btn-primary{background:#0ea5e9;color:#fff}
  .btn-primary:active{background:#0284c7}
  .btn-scan{background:#334155;color:#e2e8f0;margin-top:10px}
  .btn-danger{background:#7f1d1d;color:#fecaca;margin-top:10px;font-size:.9rem}
  .list{margin-top:20px;max-height:260px;overflow-y:auto}
  .item{display:flex;align-items:center;padding:10px 12px;border-radius:10px;
        background:#0f172a;margin-bottom:6px;cursor:pointer;border:1px solid transparent}
  .item:active,.item:hover{border-color:#38bdf8}
  .ssid{flex:1;font-weight:500;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
  .rssi{font-size:.75rem;color:#94a3b8;margin-left:8px;min-width:42px;text-align:right}
  .lock{font-size:.75rem;margin-left:4px}
  .status{margin-top:12px;padding:10px;border-radius:8px;font-size:.85rem;text-align:center}
  .ok{background:#064e3b;color:#6ee7b7}
  .err{background:#7f1d1d;color:#fecaca}
  .cur{font-size:.8rem;color:#64748b;margin-bottom:8px}
</style>
</head>
<body>
<div class="card">
  <h1>WiFi Config</h1>
  <p class="sub">Ethernet fallback · NVS storage</p>
)html";

  html << "<p class=\"cur\">Current SSID: <b>" << this->active_ssid_ << "</b></p>";

  html << R"html(
  <form method="POST" action="/wifi" id="f">
    <label for="ssid">SSID</label>
    <input type="text" id="ssid" name="ssid" maxlength="32" required
           placeholder="Tên mạng WiFi" value=")html";
  html << this->active_ssid_;
  html << R"html(">
    <label for="password">Password</label>
    <input type="password" id="password" name="password" maxlength="64"
           placeholder="Mật khẩu (để trống nếu open)">
    <button type="submit" class="btn btn-primary">Lưu & Kết nối</button>
  </form>

  <button class="btn btn-scan" onclick="location.href='/scan'">Quét mạng WiFi</button>
  <form method="POST" action="/clear" style="margin:0">
    <button type="submit" class="btn btn-danger">Xóa NVS → dùng YAML default</button>
  </form>

  <div class="list">
)html";

  if (this->scan_in_progress_) {
    html << "<p style=\"text-align:center;color:#94a3b8;padding:20px\">Đang quét…</p>";
  } else if (this->scan_results_.empty()) {
    html << "<p style=\"text-align:center;color:#64748b;padding:12px\">Chưa có kết quả quét.<br>Bấm \"Quét mạng WiFi\".</p>";
  } else {
    for (const auto &r : this->scan_results_) {
      const char *lock = (r.authmode == WIFI_AUTH_OPEN) ? "" : "🔒";
      html << "<div class=\"item\" onclick=\"document.getElementById('ssid').value='"
           << r.ssid << "';document.getElementById('password').focus()\">"
           << "<span class=\"ssid\">" << r.ssid << "</span>"
           << "<span class=\"rssi\">" << (int) r.rssi << " dBm</span>"
           << "<span class=\"lock\">" << lock << "</span>"
           << "</div>";
    }
  }

  html << R"html(
  </div>
</div>
<script>
  // simple auto-refresh while scanning
  if (document.querySelector('.list').textContent.includes('Đang quét')) {
    setTimeout(() => location.reload(), 2500);
  }
</script>
</body>
</html>
)html";

  return html.str();
}

esp_err_t EthWifiFallback::handle_root_(httpd_req_t *req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/wifi");
  httpd_resp_send(req, nullptr, 0);
  return ESP_OK;
}

esp_err_t EthWifiFallback::handle_wifi_get_(httpd_req_t *req) {
  auto *self = static_cast<EthWifiFallback *>(req->user_ctx);
  std::string page = self->build_wifi_page_html_();
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, page.c_str(), page.length());
  return ESP_OK;
}

esp_err_t EthWifiFallback::handle_wifi_post_(httpd_req_t *req) {
  auto *self = static_cast<EthWifiFallback *>(req->user_ctx);

  char buf[256] = {0};
  int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
  if (ret <= 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
    return ESP_FAIL;
  }

  // very simple form parse: ssid=...&password=...
  std::string body(buf);
  std::string ssid, password;

  auto extract = [](const std::string &src, const std::string &key) -> std::string {
    size_t pos = src.find(key + "=");
    if (pos == std::string::npos)
      return "";
    pos += key.length() + 1;
    size_t end = src.find('&', pos);
    std::string val = (end == std::string::npos) ? src.substr(pos) : src.substr(pos, end - pos);
    // url-decode minimal (+ and %20 → space, %xx)
    std::string out;
    for (size_t i = 0; i < val.size(); ++i) {
      if (val[i] == '+') {
        out += ' ';
      } else if (val[i] == '%' && i + 2 < val.size()) {
        char hex[3] = {val[i + 1], val[i + 2], 0};
        out += static_cast<char>(strtol(hex, nullptr, 16));
        i += 2;
      } else {
        out += val[i];
      }
    }
    return out;
  };

  ssid = extract(body, "ssid");
  password = extract(body, "password");

  if (ssid.empty()) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Web form save: SSID=%s", ssid.c_str());
  self->save_wifi_credentials(ssid, password);

  // success page
  const char *ok = R"html(<!DOCTYPE html><html><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Saved</title>
<style>body{font-family:sans-serif;background:#0f172a;color:#e2e8f0;display:flex;
align-items:center;justify-content:center;min-height:100vh;margin:0}
.box{background:#1e293b;padding:32px;border-radius:16px;text-align:center}
h2{color:#34d399}p{color:#94a3b8;margin-top:12px}</style></head>
<body><div class="box"><h2>✓ Đã lưu NVS</h2>
<p>Đang chuyển sang STA…<br>Nếu kết nối thành công thiết bị sẽ rời AP.<br>
Nếu thất bại sẽ quay lại AP sau ~45s.</p>
<p style="margin-top:20px"><a href="/wifi" style="color:#38bdf8">← Quay lại</a></p>
</div></body></html>)html";

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, ok, strlen(ok));
  return ESP_OK;
}

esp_err_t EthWifiFallback::handle_scan_(httpd_req_t *req) {
  auto *self = static_cast<EthWifiFallback *>(req->user_ctx);
  self->start_scan_();
  // redirect back to /wifi (page will show "Đang quét…")
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "/wifi");
  httpd_resp_send(req, nullptr, 0);
  return ESP_OK;
}

esp_err_t EthWifiFallback::handle_clear_(httpd_req_t *req) {
  auto *self = static_cast<EthWifiFallback *>(req->user_ctx);
  self->clear_saved_credentials();

  const char *ok = R"html(<!DOCTYPE html><html><head><meta charset="UTF-8">
<meta http-equiv="refresh" content="2;url=/wifi">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Cleared</title>
<style>body{font-family:sans-serif;background:#0f172a;color:#e2e8f0;display:flex;
align-items:center;justify-content:center;min-height:100vh;margin:0}
.box{background:#1e293b;padding:32px;border-radius:16px;text-align:center}
h2{color:#fbbf24}</style></head>
<body><div class="box"><h2>NVS đã xóa</h2>
<p style="color:#94a3b8;margin-top:12px">Quay về YAML default.<br>Đang chuyển hướng…</p>
</div></body></html>)html";

  httpd_resp_set_type(req, "text/html");
  httpd_resp_send(req, ok, strlen(ok));
  return ESP_OK;
}

}  // namespace eth_wifi_fallback
}  // namespace esphome
