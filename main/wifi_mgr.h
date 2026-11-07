// main/wifi_mgr.h —— Wi-Fi STA 连接与凭据管理。
//
// 凭据存放在 NVS(命名空间 nba_cfg),最多 WIFI_MGR_MAX_NETWORKS 组,不写进固件、
// 不打日志。同一 SSID 再次保存只更新密码。连接失败时按"上次连通的优先"轮换
// 已存网络(由 net_task 在 FAILED 后反复调 wifi_mgr_start 驱动)。两种配置入口:
//   * 手机配网:设备开临时热点 + 网页(见 nba_prov.c),这是日常用法;
//   * USB 串口控制台 `wifi <SSID> <密码>`,留给开发机上刷机时顺手配。
// 两条路共用同一份 NVS 记录,不会出现"网页里配的和串口配的不是一套"。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define WIFI_MGR_MAX_NETWORKS 4

typedef enum {
    WIFI_MGR_NO_CREDS = 0,   // 没存过 SSID
    WIFI_MGR_CONNECTING,
    WIFI_MGR_CONNECTED,
    WIFI_MGR_FAILED,         // 当前网络重试次数用尽(密码错/无此网络),会轮换下一个
} wifi_mgr_state_t;

// 初始化 NVS / netif / 事件循环 / Wi-Fi 驱动并读取已保存的凭据。
// 不会自动连接;调用方决定何时 wifi_mgr_start()。
esp_err_t wifi_mgr_init(void);

// 有凭据时发起连接(非阻塞,由事件回调推进状态)。
void wifi_mgr_start(void);

wifi_mgr_state_t wifi_mgr_state(void);
bool wifi_mgr_connected(void);

// 是否存着凭据(不判断当前有没有连上)。
bool wifi_mgr_has_credentials(void);

// 已存的所有网络是否都试过且失败(供"持续连不上就开配网"的看门狗判断,
// 避免把多网络轮换中途的短暂失败当成彻底没网)。
bool wifi_mgr_all_failed(void);

// 系统时间是否已通过 SNTP 同步(未同步时详情页不显示开赛时间)。
bool wifi_mgr_time_synced(void);

// 保存凭据到 NVS 并立即用新凭据重连。参数仅用于本次调用,内部复制。
// 不验证连通性;需要"先试后存"请用 wifi_mgr_try_credentials()。
esp_err_t wifi_mgr_save_credentials(const char *ssid, const char *password);

// 试连一组凭据,连通后才写入 NVS(失败时旧凭据原样保留)。
//
// 这是配网页用的:用户手输 SSID/密码难免打错,若先存后试,一次错字就会把设备
// 锁在"连不上且没有旧配置可回退"的状态,只能再配一次。所以顺序必须是
// 设配置 → 等拿到 IP → 才落盘。
//
// timeout_ms 是等待"拿到 IP"的总时长;密码错时事件回调会在 6 次重试后提前
// 置位 FAILED,不会白等到超时。返回 ESP_OK 表示已连通且凭据已保存。
esp_err_t wifi_mgr_try_credentials(const char *ssid, const char *password,
                                   uint32_t timeout_ms);

// 配网期间抑制本模块的自动重连。
//
// SoftAP 配网时 Wi-Fi 处于 APSTA:STA 一取不到目标网络就会不停断开重连,
// 把射频和事件循环都占住,还会反复改写状态机。配网期间关掉自动重连,
// 由配网页自己决定什么时候试连。
void wifi_mgr_set_provisioning(bool on);

// 抓取期间临时关掉 modem 省电(true = WIFI_PS_NONE 全速,false = WIFI_PS_MIN_MODEM)。
// 默认省电,只有 HTTP 传输那十几秒开全速;配网(APSTA)期间调用无效。
void wifi_mgr_set_fast_mode(bool fast);

// 清除已保存凭据(仍在连接中的会话不断开,下次启动不再自动连接)。
esp_err_t wifi_mgr_clear_credentials(void);

// ---- 多网络管理(串口控制台用)--------------------------------------------

// 已保存的网络数量(0 ~ WIFI_MGR_MAX_NETWORKS)。
int wifi_mgr_network_count(void);

// 第 index 个网络的 SSID(按保存顺序);index 越界返回 NULL。
const char *wifi_mgr_network_ssid(int index);

// 忘掉一个已保存的网络。忘掉的正好是当前连接的那个时,会断开并改连其它
// 已存网络;一个不剩时状态回到 NO_CREDS。找不到该 SSID 返回 ESP_ERR_NOT_FOUND。
esp_err_t wifi_mgr_forget_network(const char *ssid);
