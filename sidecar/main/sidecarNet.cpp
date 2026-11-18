// The Sidecar's network: see sidecarNet.h. WiFi station, the web server
// and mDNS (moved here from main's wifi.cpp); the telnet server is in
// sidecarTelnet.cpp.

#include "sidecarNet.h"
#include "sidecarTelnet.h"

#include "a8log.h"
#include "sidecar.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "mdns.h"
#include <esp_http_server.h>

namespace {


EventGroupHandle_t netEvents;
constexpr EventBits_t kConnectedBit = BIT0;
constexpr EventBits_t kStartBit = BIT1;   // connect now (the delay is over, or an SMB mount asked)

SidecarNetSettings settings;
SidecarStream *telnetStream = nullptr;
bool configured = false;
bool wifiStarted = false;
bool serversStarted = false;
uint32_t disconnects = 0;
esp_ip4_addr_t address = {};
httpd_handle_t httpServer = nullptr;
uint32_t httpRequests = 0;

esp_err_t helloHandler(httpd_req_t *req)
{
    char page[160];
    snprintf(page, sizeof(page), "Hello World! Atari8 Sidecar, up %lld s, %u requests\n",
             (long long)(esp_timer_get_time() / 1000000), (unsigned)++httpRequests);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

void startWebServer()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    if (httpd_start(&httpServer, &config) != ESP_OK) {
        LOG("Sidecar net: web server failed to start\n");
        return;
    }
    static const httpd_uri_t hello = {
        .uri = "/hello",
        .method = HTTP_GET,
        .handler = helloHandler,
        .user_ctx = nullptr,
    };
    httpd_register_uri_handler(httpServer, &hello);
    LOG("Sidecar net: web server on port %d\n", config.server_port);
}

void startMdns()
{
    if (settings.hostname.empty() || settings.hostname.size() >= MDNS_NAME_BUF_LEN) {
        LOG("Sidecar net: mDNS: bad hostname '%s'\n", settings.hostname.c_str());
        return;
    }
    if (mdns_init() != ESP_OK) {
        LOG("Sidecar net: mDNS failed to start\n");
        return;
    }
    mdns_hostname_set(settings.hostname.c_str());
    mdns_instance_name_set("Atari Bus ESP32");
    LOG("Sidecar net: mDNS hostname %s.local\n", settings.hostname.c_str());
}

void onEvent(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(netEvents, kConnectedBit);
        ++disconnects;
        vTaskDelay(pdMS_TO_TICKS(1000));   // keep trying, once a second
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        address = static_cast<ip_event_got_ip_t *>(data)->ip_info.ip;
        LOG("Sidecar net: WiFi up, IP address " IPSTR "\n", IP2STR(&address));
        xEventGroupSetBits(netEvents, kConnectedBit);
    }
}

void startWifi()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();
    if (!settings.hostname.empty()) {
        esp_netif_set_hostname(netif, settings.hostname.c_str());   // the DHCP hostname
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &onEvent, nullptr, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &onEvent, nullptr, nullptr));
    wifi_config_t config = {};
    strlcpy(reinterpret_cast<char *>(config.sta.ssid), settings.ssid.c_str(), sizeof(config.sta.ssid));
    strlcpy(reinterpret_cast<char *>(config.sta.password), settings.password.c_str(), sizeof(config.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
    ESP_ERROR_CHECK(esp_wifi_start());
    LOG("Sidecar net: WiFi connecting to '%s'\n", settings.ssid.c_str());
}

// The network task: WiFi when asked (after main's delay, or at once for an
// SMB mount), then the servers once it has an address.
void netTask(void *)
{
    if (settings.connectDelaySec > 0) {
        xEventGroupWaitBits(netEvents, kStartBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(settings.connectDelaySec * 1000));
    }
    wifiStarted = true;
    startWifi();
    xEventGroupWaitBits(netEvents, kConnectedBit, pdFALSE, pdFALSE, portMAX_DELAY);
    startWebServer();
    if (settings.telnetPort != 0 && telnetStream) {
        sidecarTelnetStart(*telnetStream, settings.telnetPort);
    }
    if (settings.mdns) {
        startMdns();
    }
    serversStarted = true;
    vTaskDelete(nullptr);
}

}  // namespace

void sidecarNetConfigure(const SidecarNetSettings &s, SidecarStream &telnet)
{
    if (configured)
        return;   // once per Sidecar boot: WiFi keeps running across main's reboots
    configured = true;
    settings = s;
    telnetStream = &telnet;
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char macString[13];
    snprintf(macString, sizeof(macString), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
    for (size_t at = 0; (at = settings.hostname.find("$MAC", at)) != std::string::npos; at += 12)
        settings.hostname.replace(at, 4, macString);
    if (!settings.wifi) {
        LOG("Sidecar net: WiFi off in the boot profile\n");
        return;
    }
    netEvents = xEventGroupCreate();
    // Core 0 with the bus sampler (core 1 is the service loop's).
    xTaskCreatePinnedToCore(netTask, "net", 4096, nullptr, tskIDLE_PRIORITY + 5, nullptr, 0);
}

void sidecarNetUp()
{
    if (!configured || !settings.wifi || !netEvents) {
        LOG("Sidecar net: WiFi off in the boot profile\n");
        return;
    }
    xEventGroupSetBits(netEvents, kStartBit);   // skip the connect delay
    if (!(xEventGroupWaitBits(netEvents, kConnectedBit, pdFALSE, pdFALSE, pdMS_TO_TICKS(8000)) & kConnectedBit)) {
        LOG("Sidecar net: WiFi not connected yet\n");
    }
}

void sidecarNetPoll(SidecarStream &telnet)
{
    if (sidecarTelnetRunning())
        return;   // its task drains the stream
    // No telnet server (WiFi off or not up yet): a session opened from the
    // console ("telnet open") shows main's text here.
    uint8_t buf[128];
    while (const uint32_t n = telnet.read(buf, sizeof(buf))) {
        fwrite(buf, 1, n, stdout);
    }
    fflush(stdout);
}

void sidecarNetReport()
{
    const bool up = netEvents && (xEventGroupGetBits(netEvents) & kConnectedBit);
    printf("Sidecar net: settings %s, WiFi %s%s, ssid '%s', hostname '%s', IP " IPSTR
           ", disconnects %u, telnet port %u, mDNS %s, http requests %u\n",
           configured ? "the boot profile's" : "not yet", !settings.wifi ? "off" : wifiStarted ? "started" : "waiting",
           up ? " (connected)" : "", settings.ssid.c_str(), settings.hostname.c_str(), IP2STR(&address),
           (unsigned)disconnects, (unsigned)settings.telnetPort, settings.mdns ? "on" : "off",
           (unsigned)httpRequests);
    sidecarTelnetReport();
}
