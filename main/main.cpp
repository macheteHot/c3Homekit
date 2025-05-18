#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <cstring>
#include <iostream>
extern "C" {
#include "app_wifi.h"
#include "driver/gpio.h"
#include "esp_err.h"
// #include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hap.h"
#include "hap_apple_chars.h"
#include "hap_apple_servs.h"
#include "iot_button.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "wifi_provisioning/manager.h"
}

#define LED_GPIO 8    // 请根据你的开发板实际LED GPIO号修改
#define BUTTON_GPIO 3 // 按钮接在GPIO3
#define LAUNCHER_TASK_NAME "hap_launcher"
#define LAUNCHER_TASK_STACKSIZE 4 * 1024
#define LAUNCHER_TASK_PRIORITY 1

static const char *TAG = "HAP Launcher";

// LED 闪烁函数
static void blink_led(int times) {
  for (int i = 0; i < times; ++i) {
    int wait_time = 300 / portTICK_PERIOD_MS;
    gpio_set_level((gpio_num_t)LED_GPIO, 0);
    vTaskDelay(wait_time);
    gpio_set_level((gpio_num_t)LED_GPIO, 1);
    vTaskDelay(wait_time);
  }
}

// Identify 回调
static int launcher_identify(hap_acc_t *ha) {
  printf("Accessory identified\n");
  blink_led(3); // 维持原有行为
  return HAP_SUCCESS;
}

// Switch状态变量
static bool switch_on = false;
static hap_char_t *switch_on_char = NULL;

// 发送WOL魔术包
static void send_wol_from_nvs(void *arg) {
  uint8_t mac[6];
  nvs_handle_t nvs_handle;
  esp_err_t err = nvs_open("prov", NVS_READONLY, &nvs_handle);
  if (err != ESP_OK) {
    blink_led(8);
    return;
  }
  size_t len = 6;
  err = nvs_get_blob(nvs_handle, "targetMAC", mac, &len);
  nvs_close(nvs_handle);
  if (err != ESP_OK || len != 6) {
    blink_led(1);
    return;
  }
  uint8_t packet[102];
  memset(packet, 0xFF, 6);
  for (int i = 1; i <= 16; ++i) {
    memcpy(&packet[i * 6], mac, 6);
  }
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9);
  addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    blink_led(4);
    return;
  }
  int broadcast = 1;
  int so_ret =
      setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
  int ret = sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&addr,
                   sizeof(addr));
  close(sock);
  if (ret < 0) {
    blink_led(4);
  } else {
    blink_led(2);
  }
}

// Switch写回调
static int launcher_switch_write(hap_write_data_t write_data[], int count,
                                 void *serv_priv, void *write_priv) {
  for (int i = 0; i < count; i++) {
    hap_write_data_t *write = &write_data[i];
    const char *uuid = hap_char_get_type_uuid(write->hc);
    if (!strcmp(uuid, HAP_CHAR_UUID_ON)) {
      bool new_state = write->val.b;
      if (new_state) {
        send_wol_from_nvs(NULL);
        printf("Switch ON: trigger action (WOL)\n");
        // 1秒后自动复位为关
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        switch_on = false;
        hap_val_t val = {.b = false};
        hap_char_update_val(switch_on_char, &val);
      }
      switch_on = new_state;
      *(write->status) = HAP_STATUS_SUCCESS;
    } else {
      *(write->status) = HAP_STATUS_RES_ABSENT;
    }
  }
  return HAP_SUCCESS;
}

// HomeKit事件回调（可选，调试用）
static void launcher_hap_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event, void *data) {
  switch (event) {
  case HAP_EVENT_PAIRING_STARTED:
    printf("Pairing Started\n");
    break;
  case HAP_EVENT_PAIRING_ABORTED:
    printf("Pairing Aborted\n");
    break;
  case HAP_EVENT_CTRL_PAIRED:
    printf("Controller PAIRED.\n");
    break;
  case HAP_EVENT_CTRL_UNPAIRED:
    printf("Controller UNPAIRED.\n");
    break;
  case HAP_EVENT_CTRL_CONNECTED:
    printf("Controller CONNECTED.\n");
    break;
  case HAP_EVENT_CTRL_DISCONNECTED:
    printf("Controller DISCONNECTED.\n");
    break;
  default:
    break;
  }
}

// 全局标志
volatile bool g_short_press_event = false;
volatile bool g_long_press_event = false;

// 按钮短按回调
static void button_short_press_cb(void *arg) { g_short_press_event = true; }
// 按钮长按回调
static void button_long_press_cb(void *arg) { g_long_press_event = true; }

void loop() {
  if (g_short_press_event) {
    g_short_press_event = false;
    send_wol_from_nvs(NULL);
  }
  if (g_long_press_event) {
    printf("Button long press: Erasing NVS and restarting...\n");
    g_long_press_event = false;
    nvs_flash_erase();
    esp_restart();
  }
  vTaskDelay(100 / portTICK_PERIOD_MS);
}

static void setup(void *p) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  // 初始化 NVS，确保 Wi-Fi 能正常初始化
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // 初始化 LED GPIO
  gpio_reset_pin((gpio_num_t)LED_GPIO);
  gpio_set_direction((gpio_num_t)LED_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)LED_GPIO, 0); // 上电常亮

  // 初始化实体按钮
  static CButton button((gpio_num_t)BUTTON_GPIO, BUTTON_ACTIVE_LOW);
  button.set_evt_cb(BUTTON_CB_TAP, button_short_press_cb,
                    NULL);                               // 只在短按时触发
  button.add_on_press_cb(3, button_long_press_cb, NULL); // 长按3秒

  // 初始化 Wi-Fi
  app_wifi_init();

  // 设置 Wi-Fi STA hostname，影响路由器显示名称
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif) {
    esp_netif_set_hostname(netif, "电脑启动器");
  }

  // 获取设备 MAC 地址并转为字符串（大写无冒号，仅用于 serial_num）
  char serial_num[18];
  snprintf(serial_num, sizeof(serial_num), "%02X%02X%02X%02X%02X%02X", mac[0],
           mac[1], mac[2], mac[3], mac[4], mac[5]);
  printf("HomeKit Accessory ID (serial_num): %s\n", serial_num);

  // 设置 HomeKit 配置唯一参数，确保 id 字段正常
  hap_cfg_t hap_cfg;
  hap_get_config(&hap_cfg);
  // 设置唯一参数为 UNIQUE_NONE，确保 id 字段正常
  hap_cfg.unique_param = UNIQUE_NONE;
  hap_set_config(&hap_cfg);
  // 初始化 HAP core
  hap_init(HAP_TRANSPORT_WIFI);

  // HomeKit 配置
  hap_acc_cfg_t cfg;
  cfg.name = "ESP-Launcher";
  cfg.manufacturer = "Espressif";
  cfg.model = "Launcher01";
  cfg.serial_num = serial_num;
  cfg.fw_rev = "1.0";
  cfg.hw_rev = NULL;
  cfg.pv = "1.1.0";
  cfg.identify_routine = launcher_identify;
  cfg.cid = HAP_CID_PROGRAMMABLE_SWITCH;
  hap_acc_t *accessory = hap_acc_create(&cfg);

  hap_acc_add_product_data(accessory, (uint8_t *)"ESP32HAP", 8);

  // 用 Switch 服务模拟按钮
  hap_serv_t *switch_service = hap_serv_switch_create(false);
  switch_on_char = hap_serv_get_char_by_uuid(switch_service, HAP_CHAR_UUID_ON);
  hap_serv_set_write_cb(switch_service, launcher_switch_write);
  hap_acc_add_serv(accessory, switch_service);
  hap_acc_add_wifi_transport_service(accessory, 0);
  hap_add_accessory(accessory);

  // 设置 HomeKit 配对码、Setup ID
  hap_set_setup_code("111-22-333");
  hap_set_setup_id("ES32");

  // 注册 HomeKit 事件回调
  esp_event_handler_register(HAP_EVENT, ESP_EVENT_ANY_ID,
                             &launcher_hap_event_handler, NULL);

  // 启动 HomeKit core
  hap_start();

  // 等待 Wi-Fi 连接（阻塞直到连接）
  app_wifi_start(portMAX_DELAY);
  gpio_set_level((gpio_num_t)LED_GPIO, 1); // 关闭LED

  // 主循环，处理长按事件
  while (1) {
    loop();
  }
}

extern "C" void app_main() {
  xTaskCreate(setup, LAUNCHER_TASK_NAME, LAUNCHER_TASK_STACKSIZE, NULL,
              LAUNCHER_TASK_PRIORITY, NULL);
}