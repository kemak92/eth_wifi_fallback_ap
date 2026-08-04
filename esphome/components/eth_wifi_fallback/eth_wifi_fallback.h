// This ESPHome component wraps around the repo by @kemak92:
// https://github.com/kemak92/eth_wifi_fallback_ap
//
// Ethernet primary → WiFi STA → WiFi AP fallback with Dynamic NVS WiFi Config.
// by @kemak92 - heungelectric, 2026
//
// + /wifi web page (SSID + Password form, scan, save to NVS)

#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/ethernet/ethernet_component.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_http_server.h"

#include <vector>
#include <string>

namespace esphome {
namespace eth_wifi_fallback {

enum class FallbackState {
  IDLE,
  STARTING_STA,
  STA_ACTIVE,
  STARTING_AP,
  AP_ACTIVE,
  STOPPING,
};

struct WifiCredentials {
  char ssid[33];
  char password[65];
};

struct ScanResult {
  std::string ssid;
  int8_t rssi;
  uint8_t authmode;  // wifi_auth_mode_t
};

class EthWifiFallback : public Component {
 public:
  void set_ssid(const std::string &ssid) { this->yaml_ssid_ = ssid; }
  void set_password(const std::string &password) { this->yaml_password_ = password; }
  void set_ap_ssid(const std::string &ssid) { this->ap_ssid_ = ssid; }
  void set_ap_password(const std::string &password) { this->ap_password_ = password; }
  void set_check_interval(uint32_t ms) { this->check_interval_ = ms; }
  void set_sta_timeout(uint32_t ms) { this->sta_timeout_ = ms; }

  void set_manual_ip(uint32_t static_ip, uint32_t gateway, uint32_t subnet, uint32_t dns1 = 0) {
    this->use_manual_ip_ = true;
    this->static_ip_ = static_ip;
    this->gateway_ = gateway;
    this->subnet_ = subnet;
    this->dns1_ = dns1;
  }

  void save_wifi_credentials(const std::string &ssid, const std::string &password);
  void clear_saved_credentials();

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  std::string yaml_ssid_;
  std::string yaml_password_;
  std::string active_ssid_;
  std::string active_password_;
  std::string ap_ssid_;
  std::string ap_password_;

  uint32_t check_interval_{15000};
  uint32_t sta_timeout_{45000};
  uint32_t last_check_{0};
  uint32_t state_enter_time_{0};

  FallbackState state_{FallbackState::IDLE};
  bool wifi_initialized_{false};
  esp_netif_t *sta_netif_{nullptr};
  esp_netif_t *ap_netif_{nullptr};

  bool use_manual_ip_{false};
  uint32_t static_ip_{0};
  uint32_t gateway_{0};
  uint32_t subnet_{0};
  uint32_t dns1_{0};

  ESPPreferenceObject pref_creds_;

  // HTTP server for /wifi page
  httpd_handle_t http_server_{nullptr};
  bool scan_in_progress_{false};
  std::vector<ScanResult> scan_results_;

  void load_credentials_();
  void ensure_wifi_init_();
  void start_sta_();
  void start_ap_();
  void stop_wifi_();
  bool is_sta_connected_();
  void log_sta_ip_();
  std::string get_effective_ap_ssid_();
  std::string get_effective_ap_password_();

  // Web server
  void start_http_server_();
  void stop_http_server_();
  void start_scan_();
  void collect_scan_results_();
  static esp_err_t handle_root_(httpd_req_t *req);
  static esp_err_t handle_wifi_get_(httpd_req_t *req);
  static esp_err_t handle_wifi_post_(httpd_req_t *req);
  static esp_err_t handle_scan_(httpd_req_t *req);
  static esp_err_t handle_clear_(httpd_req_t *req);
  std::string build_wifi_page_html_();
  static EthWifiFallback *instance_;  // for static handlers
};

template<typename... Ts> class SaveWifiAction : public Action<Ts...> {
 public:
  explicit SaveWifiAction(EthWifiFallback *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(std::string, ssid)
  TEMPLATABLE_VALUE(std::string, password)
  void play(Ts... x) override {
    this->parent_->save_wifi_credentials(this->ssid_.value(x...), this->password_.value(x...));
  }
 protected:
  EthWifiFallback *parent_;
};

template<typename... Ts> class ClearWifiAction : public Action<Ts...> {
 public:
  explicit ClearWifiAction(EthWifiFallback *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->clear_saved_credentials(); }
 protected:
  EthWifiFallback *parent_;
};

}  // namespace eth_wifi_fallback
}  // namespace esphome
