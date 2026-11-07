// main/nba_console.c —— USB 串口控制台:第一版配网入口。
//
// 用法(刷机后 `idf.py monitor`,或任意串口工具 115200 8N1 连接 USB-JTAG):
//   nba> wifi                 # 查看当前状态与语法
//   nba> wifi MyHome 12345678 # 保存凭据并立即重连(密码不会再被打印)
//   nba> wifi -l              # 列出已保存的网络
//   nba> wifi -d MyHome       # 忘掉一个已保存的网络
//   nba> wifi -c              # 清除全部已保存凭据
//   nba> key ok               # 注入一次按键,用于不开盖走界面(ok/up/down/long-*)
//
// 为什么先用串口而不是手机配网:开发机本来就要用 USB 刷机,串口是零额外依赖的
// 通道;BLE/BLUFI 手机配网要占用 NimBLE 与协议栈内存,留到下一版再做。
#include "nba_console.h"

#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "nba_app.h"
#include "wifi_mgr.h"

static const char *TAG = "nba_console";

static const char *state_text(wifi_mgr_state_t state)
{
    switch (state) {
    case WIFI_MGR_CONNECTED: return "已连接";
    case WIFI_MGR_CONNECTING: return "连接中";
    case WIFI_MGR_FAILED: return "连接失败";
    default: return "未配置";
    }
}

static int cmd_wifi(int argc, char **argv)
{
    if (argc == 1) {
        printf("状态: %s\n", state_text(wifi_mgr_state()));
        printf("用法: wifi <SSID> <密码>   保存并连接(最多 %d 组,同名只更新密码)\n",
               WIFI_MGR_MAX_NETWORKS);
        printf("      wifi -l              列出已保存的网络\n");
        printf("      wifi -d <SSID>       忘掉一个已保存的网络\n");
        printf("      wifi -p              开启手机配网热点\n");
        printf("      wifi -c              清除全部已保存的 Wi-Fi(并转去配网)\n");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "-l") == 0) {
        const int n = wifi_mgr_network_count();
        printf("已保存 %d/%d 个网络:\n", n, WIFI_MGR_MAX_NETWORKS);
        for (int i = 0; i < n; i++) {
            printf("  [%d] %s\n", i, wifi_mgr_network_ssid(i));
        }
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "-d") == 0) {
        const esp_err_t err = wifi_mgr_forget_network(argv[2]);
        if (err == ESP_OK) {
            printf("已忘掉 %s\n", argv[2]);
        } else if (err == ESP_ERR_NOT_FOUND) {
            printf("没有保存过这个网络: %s\n", argv[2]);
        } else {
            printf("忘掉 %s 失败\n", argv[2]);
        }
        return err == ESP_OK ? 0 : 1;
    }
    if (argc == 2 && strcmp(argv[1], "-p") == 0) {
        nba_app_request_prov();
        printf("正在开启配网热点,请用手机搜索 %s\n", "NBA-Board");
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "-c") == 0) {
        const esp_err_t err = wifi_mgr_clear_credentials();
        printf("%s\n", err == ESP_OK ? "已清除保存的 Wi-Fi" : "清除失败");
        if (err == ESP_OK) {
            // 清完凭据设备就彻底没网了,顺手开热点,免得用户以为它坏了。
            nba_app_request_prov();
        }
        return err == ESP_OK ? 0 : 1;
    }
    if (argc < 3) {
        printf("缺少密码。用法: wifi <SSID> <密码>;开放网络请传空串 \"\"\n");
        return 1;
    }
    const esp_err_t err = wifi_mgr_save_credentials(argv[1], argv[2]);
    ESP_LOGI(TAG, "已保存 Wi-Fi 配置,SSID=%s", argv[1]);
    printf("%s\n", err == ESP_OK ? "已保存,正在连接…" : "连接请求失败");
    return err == ESP_OK ? 0 : 1;
}

// 注入按键。板上的三个键在调试时很难按准(而且要拔线才能看日志),串口直接发
// 一次事件就能把三个页面走一遍。名称与 bsp_btn_t / bsp_btn_ev_t 一一对应。
static int cmd_key(int argc, char **argv)
{
    static const struct {
        const char *name;
        bsp_btn_t btn;
        bsp_btn_ev_t ev;
    } kKeys[] = {
        {"ok", BSP_BTN_OK, BSP_BTN_CLICK},
        {"up", BSP_BTN_UP, BSP_BTN_CLICK},
        {"down", BSP_BTN_DOWN, BSP_BTN_CLICK},
        {"long-ok", BSP_BTN_OK, BSP_BTN_LONG},
        {"long-up", BSP_BTN_UP, BSP_BTN_LONG},
        {"long-down", BSP_BTN_DOWN, BSP_BTN_LONG},
    };
    if (argc != 2) {
        printf("用法: key ok|up|down|long-ok|long-up|long-down\n");
        return 1;
    }
    for (size_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); i++) {
        if (strcmp(argv[1], kKeys[i].name) == 0) {
            nba_app_inject_key(kKeys[i].btn, kKeys[i].ev);
            printf("已注入按键 %s\n", kKeys[i].name);
            return 0;
        }
    }
    printf("未知按键: %s\n", argv[1]);
    return 1;
}

esp_err_t nba_console_init(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "nba>";
    repl_config.max_cmdline_length = 128;

    esp_console_dev_usb_serial_jtag_config_t dev_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    esp_err_t err = esp_console_new_repl_usb_serial_jtag(&dev_config, &repl_config, &repl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "控制台不可用(%s),仍可通过已保存的凭据联网",
                 esp_err_to_name(err));
        return err;
    }

    esp_console_cmd_t wifi_cmd = {
        .command = "wifi",
        .help = "配置/清除 Wi-Fi: wifi <SSID> <密码> | wifi -c",
        .func = cmd_wifi,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_cmd));
    esp_console_cmd_t key_cmd = {
        .command = "key",
        .help = "注入按键: key ok|up|down|long-ok|long-up|long-down",
        .func = cmd_key,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&key_cmd));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "串口控制台已启动(输入 wifi / key 查看用法)");
    return ESP_OK;
}
