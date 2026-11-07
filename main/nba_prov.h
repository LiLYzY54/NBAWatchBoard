// main/nba_prov.h —— 手机配网(SoftAP + 网页)。
//
// 为什么是热点而不是蓝牙:这块板子是 ESP32-C3,没有 PSRAM,LVGL 已经静态占掉
// 64 KB 池,DRAM 只剩 ~114 KB。BLE 方案要额外背一套协议栈(NimBLE 约 40 KB,
// Bluedroid 约 80 KB),配网期间还要和 LVGL、Wi-Fi 同时在场,余量太小;而热点
// + HTTP 只多出二十几 KB,且任何手机都能用,不必装 App。
//
// 流程:设备开一个开放热点 → 手机连上 → 浏览器打开 192.168.4.1 → 选/填 Wi-Fi
// 名称与密码 → 设备以 APSTA 模式试连 → 拿到 IP 才写入 NVS → 关热点、切回 STA。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// 配网完成时的回调。运行在 HTTP 服务任务里,**只能做非阻塞操作**(入队/置位),
// 不要在里面调 LVGL 或等待网络。
typedef void (*nba_prov_done_cb_t)(bool ok);

// 启动热点与网页服务。重复调用是安全的(已在运行则直接返回)。
// 需要 wifi_mgr_init() 已完成。
esp_err_t nba_prov_start(void);

// 关闭网页服务与热点,恢复到纯 STA 模式。重复调用安全。
void nba_prov_stop(void);

bool nba_prov_running(void);

// 热点名(屏幕上要显示给用户)与访问地址。
const char *nba_prov_ap_ssid(void);
const char *nba_prov_ap_url(void);

void nba_prov_set_done_cb(nba_prov_done_cb_t cb);

// 最近一次配网结果:true = 已连通并保存,false = 还没成功或刚失败。
bool nba_prov_last_ok(void);
