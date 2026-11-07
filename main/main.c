// main/main.c —— NBA 比赛看板(FoloToy AI Passport / ESP32-C3)。
//
// 启动顺序:共享 I2C -> 显示与 LVGL -> 背光 -> 电量计 -> 看板应用 -> Wi-Fi -> 串口控制台。
// 显示是硬依赖(没有屏幕就没有看板);电量与 Wi-Fi 是软依赖,失败时界面降级显示,
// 其余功能照常。基线硬件自检 demo(demo_*.c)仍保留在源码树中作为参考,但不再被
// 编译进本应用:本固件开机直接进入 NBA 看板,不进入 demo 菜单。
#include "bsp_battery.h"
#include "bsp_display.h"
#include "bsp_i2c.h"
#include "bsp_pins.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "nba_app.h"
#include "nba_console.h"
#include "wifi_mgr.h"

#include <stdlib.h>
#include <time.h>

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "NBA 看板启动(ESP-IDF %s)", esp_get_idf_version());

    // 开赛时间是 UTC,按用户所在时区显示。国内用户默认东八区。
    setenv("TZ", "CST-8", 1);
    tzset();

    // 电量计与音频 codec 共用 I2C0;BSP 内部幂等,这里先探一次总线。
    bsp_i2c_init();
    bsp_i2c_scan();

    if (bsp_display_init() != ESP_OK || !bsp_lvgl_init()) {
        ESP_LOGE(TAG, "显示/LVGL 初始化失败,看板无法启动。"
                      "检查 SPI 接线(MOSI=%d SCLK=%d CS=%d DC=%d BL=%d)",
                 BSP_LCD_MOSI, BSP_LCD_SCLK, BSP_LCD_CS, BSP_LCD_DC, BSP_LCD_BL);
        return;
    }
    bsp_display_backlight(100);

    if (bsp_battery_init() != ESP_OK) {
        ESP_LOGW(TAG, "电量计未应答,电量角标将留空");
    }

    // Wi-Fi 驱动必须先于应用起来:无凭据时应用会立刻开配网热点,而开热点要动
    // esp_wifi_set_mode —— esp_wifi_init 还没跑就返回 ESP_ERR_WIFI_NOT_INIT。
    // wifi_mgr_init 自己只做驱动初始化和读 NVS,连接和抓数都在网络任务里,
    // 所以它不违反"先让用户看到界面"的初衷。
    if (wifi_mgr_init() != ESP_OK) {
        ESP_LOGW(TAG, "Wi-Fi 初始化失败,本次只能显示空看板");
    }

    // 再把界面立起来:UI 创建之后才会判定要不要进配网页。
    const esp_err_t app_err = nba_app_start();
    if (app_err != ESP_OK) {
        ESP_LOGE(TAG, "应用启动异常: %s", esp_err_to_name(app_err));
    }

    if (nba_console_init() != ESP_OK) {
        ESP_LOGW(TAG, "串口控制台不可用:配网请用已保存的凭据,或重新刷机");
    }

    ESP_LOGI(TAG, "就绪");
}
