// main/nba_app.h —— 看板应用入口:页面状态、按键语义、后台刷新。
#pragma once

#include "bsp_button.h"
#include "esp_err.h"

// 在 LVGL 初始化完成之后调用一次:创建两个页面、启动 UI 任务与网络任务、
// 注册按键回调。返回非 ESP_OK 表示按键或任务创建失败(界面仍会显示)。
esp_err_t nba_app_start(void);

// 按键事件回调(签名与 bsp_btn_cb_t 一致)。运行在 button 组件共享的 esp_timer
// 任务里:只入队,不做重活。user 未使用。
void nba_app_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user);

// 把一次按键注入应用队列(串口控制台的 `key` 命令用):不经过 ADC,等价于用户
// 真按了一下。板上只有三个键,调试时能不开盖就把界面走一遍。
void nba_app_inject_key(bsp_btn_t btn, bsp_btn_ev_t ev);

// 请求进入手机配网(串口 `wifi -p` / 清凭据后用)。只置一个标志,开热点这件事
// 由 UI 任务去做——那里才有 LVGL 锁,也才不会和渲染抢 s_ui。
void nba_app_request_prov(void);
