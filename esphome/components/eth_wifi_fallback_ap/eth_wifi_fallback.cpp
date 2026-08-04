// This ESPHome component wraps around the repo by @kemak92:
// https://github.com/kemak92/eth_wifi_fallback_ap
//
// Ethernet primary → WiFi STA → WiFi AP fallback with Dynamic NVS WiFi Config.
// by @kemak92 - heungelectric, 2026

#include "eth_wifi_fallback.h"
#include "esphome/core/log.h"
#include "esphome/components/ethernet/ethernet_component.h"

namespace esphome {
namespace eth_wifi_fallback {

static const char *const TAG = "eth_wifi_fallback";
static const uint32_t NVS_CREDS_KEY = 0x8F23A1B2;

void EthWifiFallback::setup() {
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

  // FIX: đổi WiFi ngay dù đang STA hoặc AP
  if (this->state_ != FallbackState::IDLE && this->state_ != FallbackState::STOPPING) {
    ESP_LOGI(TAG, "Switching to new STA credentials...");
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
      // FIX: mất STA → đếm lại timeout → có thể về AP
      if (!this->is_sta_connected_()) {
        ESP_LOGW(TAG, "STA lost → restart STA timeout (then AP if still fail)");
        this->state_ = FallbackState::STARTING_STA;
        this->state_enter_time_ = now;
        esp_wifi_connect();
      }
      break;

    case FallbackState::STARTING_AP:
      this->state_ = FallbackState::AP_ACTIVE;
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
  // Auto password từ MAC (≥ 8 ký tự) — in ra log khi bật AP để chữa cháy
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

  // Chữa cháy: luôn in SSID; in PASS khi auto (user không set) hoặc khi user set cũng in để rescue
  ESP_LOGW(TAG, "========== RESCUE AP ==========");
  ESP_LOGW(TAG, "SSID: %s", ap_ssid.c_str());
  ESP_LOGW(TAG, "PASS: %s%s", ap_pass.c_str(), auto_pass ? " (auto from MAC)" : "");
  ESP_LOGW(TAG, "IP:   usually 192.168.4.1");
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

}  // namespace eth_wifi_fallback
}  // namespace esphome