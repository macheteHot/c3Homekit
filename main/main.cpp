#include "esp_netif.h"
#include "esp_wifi.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include <atomic>
#include <cstring>
#include <iostream>
#include <sys/time.h>
extern "C" {
#include "app_wifi.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
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
#include "freertos/timers.h"

#define TAG "computer_hap" // 定义日志TAG

#define LED_GPIO 8    // 请根据你的开发板实际LED GPIO号修改
#define BUTTON_GPIO 3 // 按钮接在GPIO3
#define LAUNCHER_TASK_NAME "hap_launcher"
#define LAUNCHER_TASK_STACKSIZE 4 * 1024
#define LAUNCHER_TASK_PRIORITY 1

static std::atomic<uint64_t> last_heartbeat_time_ms{0};
static std::atomic<bool> pc_online{false};

// 获取当前时间戳（毫秒）
static uint64_t get_time_ms() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

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
  ESP_LOGI(TAG, "Accessory identified");
  blink_led(3); // 维持原有行为
  return HAP_SUCCESS;
}

// Switch状态变量
static hap_char_t *switch_on_char = NULL;

// 全局保存NVS targetMAC
static uint8_t g_target_mac[6] = {0};
static bool g_target_mac_valid = false;

// WOL后等待心跳保护期（单位ms）
#define WOL_WAIT_HEARTBEAT_MS 30000
static uint64_t wol_sent_time_ms = 0;

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
  struct sockaddr_in addr = {{0}}; // 修正初始化
  addr.sin_family = AF_INET;
  addr.sin_port = htons(9);
  addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    blink_led(4);
    return;
  }
  int broadcast = 1;
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
  int ret = sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&addr, sizeof(addr));
  close(sock);
  if (ret < 0) {
    blink_led(4);
  } else {
    blink_led(2);
  }
}

// 发送关机指令
static void send_shutdown_cmd_from_nvs(void) {
  if (!g_target_mac_valid)
    return;
  char msg[64];
  snprintf(msg, sizeof(msg), "SHUTDOWN_ESP|%02X%02X%02X%02X%02X%02X", g_target_mac[0],
           g_target_mac[1], g_target_mac[2], g_target_mac[3], g_target_mac[4], g_target_mac[5]);
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0)
    return;
  int broadcast = 1;
  setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
  struct sockaddr_in addr = {{0}}; // 修正初始化
  addr.sin_family = AF_INET;
  addr.sin_port = htons(40000);
  addr.sin_addr.s_addr = inet_addr("255.255.255.255");
  sendto(sock, msg, strlen(msg), 0, (struct sockaddr *)&addr, sizeof(addr));
  close(sock);
}

// Switch写回调
static int launcher_switch_write(hap_write_data_t write_data[], int count, void *serv_priv,
                                 void *write_priv) {
  for (int i = 0; i < count; i++) {
    hap_write_data_t *write = &write_data[i];
    const char *uuid = hap_char_get_type_uuid(write->hc);
    if (!strcmp(uuid, HAP_CHAR_UUID_ON)) {
      bool new_state = write->val.b;
      if (new_state) {
        send_wol_from_nvs(NULL);
        ESP_LOGI(TAG, "Switch ON: trigger action (WOL)");
        wol_sent_time_ms = get_time_ms(); // 记录WOL发送时间
      } else {
        send_shutdown_cmd_from_nvs();
        ESP_LOGI(TAG, "Switch OFF: trigger shutdown command");
      }
      *(write->status) = HAP_STATUS_SUCCESS;
    } else {
      *(write->status) = HAP_STATUS_RES_ABSENT;
    }
  }
  return HAP_SUCCESS;
}

// HomeKit事件回调（可选，调试用）
static void launcher_hap_event_handler(void *arg, esp_event_base_t event_base, int32_t event,
                                       void *data) {
  switch (event) {
  case HAP_EVENT_PAIRING_STARTED:
    ESP_LOGI(TAG, "Pairing Started");
    break;
  case HAP_EVENT_PAIRING_ABORTED:
    ESP_LOGI(TAG, "Pairing Aborted");
    break;
  case HAP_EVENT_CTRL_PAIRED:
    ESP_LOGI(TAG, "Controller PAIRED.");
    break;
  case HAP_EVENT_CTRL_UNPAIRED:
    ESP_LOGI(TAG, "Controller UNPAIRED.");
    break;
  case HAP_EVENT_CTRL_CONNECTED:
    ESP_LOGI(TAG, "Controller CONNECTED.");
    break;
  case HAP_EVENT_CTRL_DISCONNECTED:
    ESP_LOGI(TAG, "Controller DISCONNECTED.");
    break;
  default:
    break;
  }
}

// 全局标志
volatile bool g_short_press_event = false;
volatile bool g_long_press_event = false;

// 单击/双击判定定时器
static TimerHandle_t tap_timer = NULL;

static void button_short_press_cb(void *arg) {
  const uint64_t DOUBLE_TAP_INTERVAL = 500; // ms
  static int tap_count = 0;
  tap_count++;
  ESP_LOGI(TAG, "Button tapped, tap_count=%d", tap_count); // 普通点击日志
  if (tap_timer == NULL) {
    tap_timer = xTimerCreate("tap_timer", pdMS_TO_TICKS(DOUBLE_TAP_INTERVAL), pdFALSE, &tap_count,
                             [](TimerHandle_t xTimer) {
                               ESP_LOGI(TAG, "Single tap detected, sending WOL");
                               send_wol_from_nvs(NULL);
                               int *tap_count_ptr = (int *)pvTimerGetTimerID(xTimer);
                               if (tap_count_ptr)
                                 *tap_count_ptr = 0;
                             });
  } else {
    vTimerSetTimerID(tap_timer, &tap_count);
  }
  xTimerStop(tap_timer, 0);
  if (tap_count == 1) {
    xTimerChangePeriod(tap_timer, pdMS_TO_TICKS(DOUBLE_TAP_INTERVAL), 0);
    xTimerStart(tap_timer, 0);
  } else if (tap_count == 2) {
    ESP_LOGI(TAG, "Double tap detected, sending shutdown command");
    send_shutdown_cmd_from_nvs();
    tap_count = 0;
  }
}
// 按钮长按回调
static void button_long_press_cb(void *arg) {
  ESP_LOGI(TAG, "Button long press: Erasing NVS and restarting...");
  nvs_flash_erase();
  esp_restart();
}

void loop() {
  static bool last_pc_online = false;
  uint64_t now = get_time_ms();
  bool online = false;
  // 判断是否处于WOL保护期
  bool in_wol_wait = (wol_sent_time_ms > 0) && (now - wol_sent_time_ms < WOL_WAIT_HEARTBEAT_MS);
  if (last_heartbeat_time_ms > 0 && now - last_heartbeat_time_ms < 2000) {
    online = true;
  } else if (in_wol_wait) {
    online = true; // 保护期内强制保持开
  }
  // 如果收到心跳，退出保护期
  if (last_heartbeat_time_ms > 0 && now - last_heartbeat_time_ms < 2000 && in_wol_wait) {
    wol_sent_time_ms = 0;
  }
  // 状态变化时同步HomeKit开关
  if (online != last_pc_online) {
    last_pc_online = online;
    if (switch_on_char) {
      hap_val_t val = {.b = online};
      hap_char_update_val(switch_on_char, &val);
    }
  }
  vTaskDelay(100 / portTICK_PERIOD_MS);
}

// UDP 监听任务，监听40000端口，收到HEARTBEAT|MAC包时刷新心跳
static void udp_heartbeat_task(void *arg) {
  int sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    vTaskDelete(NULL);
    return;
  }
  struct sockaddr_in addr = {{0}}; // 修正初始化
  addr.sin_family = AF_INET;
  addr.sin_port = htons(40000);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(sock);
    vTaskDelete(NULL);
    return;
  }
  char buf[128];
  while (1) {
    struct sockaddr_in src_addr;
    socklen_t addrlen = sizeof(src_addr);
    int len = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&src_addr, &addrlen);
    if (len > 0) {
      buf[len] = '\0';
      // 查找HEARTBEAT|前缀
      if (strncmp(buf, "HEARTBEAT|", 10) == 0 && g_target_mac_valid) {
        // 取出心跳包中的MAC字符串
        const char *mac_str = buf + 10;
        // 格式化NVS MAC为大写无冒号字符串
        char nvs_mac_str[13];
        snprintf(nvs_mac_str, sizeof(nvs_mac_str), "%02X%02X%02X%02X%02X%02X", g_target_mac[0],
                 g_target_mac[1], g_target_mac[2], g_target_mac[3], g_target_mac[4],
                 g_target_mac[5]);
        // 比较
        if (strncasecmp(mac_str, nvs_mac_str, 12) == 0) {
          last_heartbeat_time_ms = get_time_ms();
          pc_online = true;
        }
      }
    }
  }
  // never reached
  close(sock);
  vTaskDelete(NULL);
}

static void setup(void *p) {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  // 初始化 NVS，确保 Wi-Fi 能正常初始化
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  // 读取NVS targetMAC到全局变量
  nvs_handle_t nvs_handle;
  size_t mac_len = 6;
  esp_err_t err = nvs_open("prov", NVS_READONLY, &nvs_handle);
  if (err == ESP_OK) {
    err = nvs_get_blob(nvs_handle, "targetMAC", g_target_mac, &mac_len);
    nvs_close(nvs_handle);
    if (err == ESP_OK && mac_len == 6) {
      g_target_mac_valid = true;
    }
  }

  // 初始化 LED GPIO
  gpio_reset_pin((gpio_num_t)LED_GPIO);
  gpio_set_direction((gpio_num_t)LED_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level((gpio_num_t)LED_GPIO, 0); // 上电常亮

  // 初始化实体按钮
  static CButton button((gpio_num_t)BUTTON_GPIO, BUTTON_ACTIVE_LOW);
  button.set_evt_cb(BUTTON_CB_TAP, button_short_press_cb, NULL); // 点击回调
  button.add_on_press_cb(3, button_long_press_cb, NULL);         // 长按3秒

  // 初始化 Wi-Fi
  app_wifi_init();

  // 设置 Wi-Fi STA hostname，影响路由器显示名称
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (netif) {
    esp_netif_set_hostname(netif, "电脑启动器");
  }

  // 获取设备 MAC 地址并转为字符串（大写无冒号，仅用于 serial_num）
  char serial_num[18];
  snprintf(serial_num, sizeof(serial_num), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  ESP_LOGI(TAG, "HomeKit Accessory ID (serial_num): %s", serial_num);

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
  static const char *acc_name = "电脑启动器";
  static const char *acc_manufacturer = "虎哥科技";
  static const char *acc_model = "001";
  static const char *acc_fw_rev = "1.0";
  static const char *acc_pv = "1.1.0";
  cfg.name = (char *)acc_name;
  cfg.manufacturer = (char *)acc_manufacturer;
  cfg.model = (char *)acc_model;
  cfg.serial_num = serial_num;
  cfg.fw_rev = (char *)acc_fw_rev;
  cfg.hw_rev = NULL;
  cfg.pv = (char *)acc_pv;
  cfg.identify_routine = launcher_identify;
  cfg.cid = HAP_CID_SWITCH;
  hap_acc_t *accessory = hap_acc_create(&cfg);

  hap_acc_add_product_data(accessory, (uint8_t *)"ESP32HAP", 8);

  // 用 Switch 服务模拟按钮
  hap_serv_t *switch_service = hap_serv_switch_create(false);
  switch_on_char = hap_serv_get_char_by_uuid(switch_service, HAP_CHAR_UUID_ON);
  hap_serv_set_write_cb(switch_service, launcher_switch_write);
  hap_acc_add_serv(accessory, switch_service);
  hap_acc_add_wifi_transport_service(accessory, 0);
  hap_add_accessory(accessory);
  char setup_code[9];
  get_setup_code(setup_code);
  char formatted_code[12];
  snprintf(formatted_code, sizeof(formatted_code), "%c%c%c-%c%c-%c%c%c", setup_code[0],
           setup_code[1], setup_code[2], setup_code[3], setup_code[4], setup_code[5], setup_code[6],
           setup_code[7]);
  // 设置 HomeKit 配对码
  hap_set_setup_code(formatted_code);
  hap_set_setup_id("7G9X");

  // 注册 HomeKit 事件回调
  esp_event_handler_register(HAP_EVENT, ESP_EVENT_ANY_ID, &launcher_hap_event_handler, NULL);

  // 启动 HomeKit core
  hap_start();

  // 启动UDP心跳监听任务
  xTaskCreate(udp_heartbeat_task, "udp_heartbeat", 2 * 1024, NULL, 5, NULL);

  // 等待 Wi-Fi 连接（阻塞直到连接）
  app_wifi_start(portMAX_DELAY);
  gpio_set_level((gpio_num_t)LED_GPIO, 1); // 关闭LED

  // 主循环，处理长按事件
  while (1) {
    loop();
  }
}

extern "C" void app_main() {
  xTaskCreate(setup, LAUNCHER_TASK_NAME, LAUNCHER_TASK_STACKSIZE, NULL, LAUNCHER_TASK_PRIORITY,
              NULL);
}