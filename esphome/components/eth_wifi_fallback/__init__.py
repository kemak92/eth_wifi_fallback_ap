# This ESPHome component wraps around the repo by @kemak92:
# https://github.com/kemak92/eth_wifi_fallback_ap
#
# Ethernet primary → WiFi STA → WiFi AP fallback with Dynamic NVS WiFi Config.
# by @kemak92 - heungelectric, 2026

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.const import (
    CONF_ID,
    CONF_SSID,
    CONF_PASSWORD,
    CONF_STATIC_IP,
    CONF_GATEWAY,
    CONF_SUBNET,
    CONF_DNS1,
)
from esphome.components import ethernet

DEPENDENCIES = ["ethernet"]
AUTO_LOAD = []

eth_wifi_fallback_ns = cg.esphome_ns.namespace("eth_wifi_fallback")
EthWifiFallback = eth_wifi_fallback_ns.class_("EthWifiFallback", cg.Component)
SaveWifiAction = eth_wifi_fallback_ns.class_("SaveWifiAction", automation.Action)
ClearWifiAction = eth_wifi_fallback_ns.class_("ClearWifiAction", automation.Action)

CONF_CHECK_INTERVAL = "check_interval"
CONF_MANUAL_IP = "manual_ip"
CONF_AP_SSID = "ap_ssid"
CONF_AP_PASSWORD = "ap_password"
CONF_STA_TIMEOUT = "sta_timeout"

MANUAL_IP_SCHEMA = cv.Schema({
    cv.Required(CONF_STATIC_IP): cv.ipv4address,
    cv.Required(CONF_GATEWAY): cv.ipv4address,
    cv.Required(CONF_SUBNET): cv.ipv4address,
    cv.Optional(CONF_DNS1): cv.ipv4address,
})

CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(EthWifiFallback),
    cv.Required(CONF_SSID): cv.string,
    cv.Required(CONF_PASSWORD): cv.string,
    cv.Optional(CONF_MANUAL_IP): MANUAL_IP_SCHEMA,
    cv.Optional(CONF_STA_TIMEOUT, default="45s"): cv.positive_time_period_milliseconds,
    cv.Optional(CONF_AP_SSID): cv.string,
    cv.Optional(CONF_AP_PASSWORD): cv.string,
    cv.Optional(CONF_CHECK_INTERVAL, default="15s"): cv.positive_time_period_milliseconds,
}).extend(cv.COMPONENT_SCHEMA)


def ip_to_uint32(ip):
    parts = str(ip).split(".")
    return (int(parts[0]) << 24) | (int(parts[1]) << 16) | (int(parts[2]) << 8) | int(parts[3])


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_ssid(config[CONF_SSID]))
    cg.add(var.set_password(config[CONF_PASSWORD]))
    cg.add(var.set_check_interval(config[CONF_CHECK_INTERVAL]))
    cg.add(var.set_sta_timeout(config[CONF_STA_TIMEOUT]))
    if CONF_AP_SSID in config:
        cg.add(var.set_ap_ssid(config[CONF_AP_SSID]))
    if CONF_AP_PASSWORD in config:
        cg.add(var.set_ap_password(config[CONF_AP_PASSWORD]))
    if CONF_MANUAL_IP in config:
        m = config[CONF_MANUAL_IP]
        dns = ip_to_uint32(m[CONF_DNS1]) if CONF_DNS1 in m else 0
        cg.add(var.set_manual_ip(
            ip_to_uint32(m[CONF_STATIC_IP]),
            ip_to_uint32(m[CONF_GATEWAY]),
            ip_to_uint32(m[CONF_SUBNET]),
            dns,
        ))


@automation.register_action(
    "eth_wifi_fallback.save_wifi",
    SaveWifiAction,
    cv.Schema({
        cv.GenerateID(): cv.use_id(EthWifiFallback),
        cv.Required(CONF_SSID): cv.templatable(cv.string),
        cv.Required(CONF_PASSWORD): cv.templatable(cv.string),
    }),
)
async def save_wifi_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    cg.add(var.set_ssid(await cg.templatable(config[CONF_SSID], args, cg.std_string)))
    cg.add(var.set_password(await cg.templatable(config[CONF_PASSWORD], args, cg.std_string)))
    return var


@automation.register_action(
    "eth_wifi_fallback.clear_wifi",
    ClearWifiAction,
    cv.Schema({cv.GenerateID(): cv.use_id(EthWifiFallback)}),
)
async def clear_wifi_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    return cg.new_Pvariable(action_id, template_arg, parent)