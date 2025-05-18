#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_idf_version.h>
#include <esp_netif.h>
#include <wifi_provisioning/manager.h>
#include <wifi_provisioning/scheme_ble.h>
#include <nvs.h>
#include <nvs_flash.h>
#include "hap.h"
#include "esp_mac.h"
#include "app_wifi.h"

esp_err_t targetmac_endpoint_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen, uint8_t **outbuf, ssize_t *outlen, void *priv_data);

void get_setup_code(char out_str[9])
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    // 用 DJB hash 算法处理 MAC
    uint32_t hash = 5381;
    for (int i = 0; i < 6; i++)
    {
        hash = ((hash << 5) + hash) + mac[i]; // hash * 33 + mac[i]
    }

    // 限制在 8 位十进制以内
    uint32_t id = (hash % 90000000) + 10000000;

    // 转为数字字符串
    sprintf(out_str, "%lu", (unsigned long)id);
}

static const char *TAG = "app_wifi";
static const int WIFI_CONNECTED_EVENT = BIT0;
static EventGroupHandle_t wifi_event_group;

#define PROV_TRANSPORT_BLE "ble"

static void get_device_service_name(char *service_name, size_t max)
{
    uint8_t eth_mac[6];
    const char *ssid_prefix = "PROV_";
    esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    snprintf(service_name, max, "%s%02X%02X%02X",
             ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
}

static esp_err_t get_device_pop(char *pop, size_t max)
{
    if (!pop || !max)
        return ESP_ERR_INVALID_ARG;
    uint8_t eth_mac[6];
    esp_err_t err = esp_wifi_get_mac(WIFI_IF_STA, eth_mac);
    if (err == ESP_OK)
    {
        snprintf(pop, max, "%02x%02x%02x%02x", eth_mac[2], eth_mac[3], eth_mac[4], eth_mac[5]);
        return ESP_OK;
    }
    else
    {
        return err;
    }
}

static bool s_prov_active = false;

static void stop_ble_provisioning()
{
    if (s_prov_active)
    {
        ESP_LOGI(TAG, "Stopping BLE provisioning");
        wifi_prov_mgr_stop_provisioning();
        wifi_prov_mgr_deinit();
        s_prov_active = false;
        ESP_LOGI(TAG, "BLE provisioning stopped");
    }
}

static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED)
    {
        esp_netif_create_ip6_linklocal((esp_netif_t *)arg);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_GOT_IP6)
    {
        ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
        ESP_LOGI(TAG, "Connected with IPv6 Address:" IPV6STR, IPV62STR(event->ip6_info.ip));
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
        esp_wifi_connect();
    }
    else if (event_base == WIFI_PROV_EVENT)
    {
        switch (event_id)
        {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "Provisioning started");
            break;
        case WIFI_PROV_CRED_RECV:
        {
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG, "Received Wi-Fi credentials\n\tSSID     : %s\n\tPassword : %s",
                     (const char *)wifi_sta_cfg->ssid,
                     (const char *)wifi_sta_cfg->password);
            // 判断SSID和密码都不为空再重启
            if (wifi_sta_cfg->ssid[0] != '\0' && wifi_sta_cfg->password[0] != '\0')
            {
                esp_restart(); // 获取到 SSID 和密码后直接重启
            }
            else
            {
                ESP_LOGW(TAG, "SSID或密码为空，不重启");
            }
            break;
        }
        case WIFI_PROV_CRED_FAIL:
        {
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
            ESP_LOGE(TAG, "Provisioning failed!\n\tReason : %s\n\tPlease reset to factory and retry provisioning",
                     (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning successful");
            break;
        case WIFI_PROV_END:
            stop_ble_provisioning(); // 不在这里deinit，交由 stop_ble_provisioning 控制
            break;
        default:
            break;
        }
    }
}

void app_wifi_init(void)
{
    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_event_group = xEventGroupCreate();
    esp_netif_t *wifi_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, wifi_netif));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &event_handler, NULL));
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
}

esp_err_t app_wifi_start(TickType_t ticks_to_wait)
{
    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM};
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
    s_prov_active = false;
    bool provisioned = false;
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));
    if (!provisioned)
    {
        ESP_LOGI(TAG, "Starting provisioning");
        esp_netif_create_default_wifi_ap();
        esp_event_handler_register(WIFI_PROV_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL);
        char service_name[12];
        get_device_service_name(service_name, sizeof(service_name));
        wifi_prov_security_t security = WIFI_PROV_SECURITY_0;
        char pop[9];
        esp_err_t err = get_device_pop(pop, sizeof(pop));
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Error: %d. Failed to get PoP from NVS, Please perform Claiming.", err);
            return err;
        }
        const char *service_key = NULL;
        uint8_t custom_service_uuid[] = {
            0xb4,
            0xdf,
            0x5a,
            0x1c,
            0x3f,
            0x6b,
            0xf4,
            0xbf,
            0xea,
            0x4a,
            0x82,
            0x03,
            0x04,
            0x90,
            0x1a,
            0x02,
        };
        err = wifi_prov_scheme_ble_set_service_uuid(custom_service_uuid);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "wifi_prov_scheme_ble_set_service_uuid failed %d", err);
            return err;
        }
        wifi_prov_mgr_endpoint_create("targetMAC");
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, pop, service_name, service_key));
        wifi_prov_mgr_endpoint_register("targetMAC", targetmac_endpoint_handler, NULL);
        ESP_LOGI(TAG, "Provisioning Started. Name : %s, POP : %s", service_name, pop);
        s_prov_active = true;
    }
    else
    {
        ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");
        wifi_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, false, true, ticks_to_wait);
    return ESP_OK;
}

esp_err_t targetmac_endpoint_handler(uint32_t session_id, const uint8_t *inbuf, ssize_t inlen, uint8_t **outbuf, ssize_t *outlen, void *priv_data)
{
    ESP_LOGI(TAG, "targetMAC endpoint handler called, inlen=%d", (int)inlen);
    esp_err_t nvs_result = ESP_OK;

    if (inbuf && inlen > 0)
    {
        ESP_LOGI(TAG, "targetMAC received: %.*s", (int)inlen, (const char *)inbuf);
        // 解析 JSON 格式 { "target_mac": "22:33:4D:06:43:ED" }
        const char *key = "target_mac";
        const char *json = (const char *)inbuf;
        char *mac_ptr = strstr(json, key);
        bool valid = false;
        uint8_t mac[6];
        if (mac_ptr)
        {
            mac_ptr = strchr(mac_ptr, ':');
            if (mac_ptr)
            {
                mac_ptr++;
                while (*mac_ptr == ' ' || *mac_ptr == '"')
                    mac_ptr++;
                // 支持 : 或 - 分割
                int n = sscanf(mac_ptr, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
                               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
                if (n != 6)
                {
                    n = sscanf(mac_ptr, "%hhx-%hhx-%hhx-%hhx-%hhx-%hhx",
                               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
                }
                valid = (n == 6);
            }
        }
        if (valid)
        {
            nvs_handle_t nvs_handle;
            nvs_result = nvs_open("prov", NVS_READWRITE, &nvs_handle);
            if (nvs_result == ESP_OK)
            {
                nvs_result = nvs_set_blob(nvs_handle, "targetMAC", mac, 6);
                if (nvs_result == ESP_OK)
                {
                    nvs_result = nvs_commit(nvs_handle);
                }
                nvs_close(nvs_handle);
            }
        }
        else
        {
            nvs_result = ESP_ERR_INVALID_ARG;
        }
    }
    // 构造响应
    char resp_buf[128];
    if (nvs_result == ESP_OK)
    {
        // 读取 setup_code
        char setup_code[9];
        get_setup_code(setup_code);
        /**
         * 此处
         * categoryId HAP_CID_SWITCH
         * flag 固定0
         * password get_setup_code
         * reserved 保留字段 固定0
         * setupId 随机4位字符 本固件固定 7G9X
         * version 当前固件固定 89.64
         */
        //  {
        //   categoryId: HAP_CID_SWITCH,
        //   password: get_setup_code,
        //   setupId: "7G9X",
        // }
        snprintf(resp_buf, sizeof(resp_buf), "{\"categoryId\":%d,\"password\":\"%s\",\"setupId\":\"%s\"}",
                 HAP_CID_SWITCH, setup_code, "7G9X");
    }
    else
    {
        snprintf(resp_buf, sizeof(resp_buf), "{\"status\":\"nvs_error\",\"err\":%d}", nvs_result);
    }
    *outbuf = (uint8_t *)strdup(resp_buf);
    *outlen = strlen(resp_buf);
    return ESP_OK;
}
