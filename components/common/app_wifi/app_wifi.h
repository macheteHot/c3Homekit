#pragma once
#include <esp_err.h>

#ifdef __cplusplus
extern "C"
{
#endif

  void app_wifi_init(void);
  void get_setup_code(char out_str[9]);
  esp_err_t app_wifi_start(TickType_t ticks_to_wait);

#ifdef __cplusplus
}
#endif
