// main/nba_console.h —— USB 串口控制台(第一版 Wi-Fi 配网入口)。
#pragma once

#include "esp_err.h"

// 注册 wifi 命令并启动 REPL。控制台不可用时返回错误,不影响看板其余功能。
esp_err_t nba_console_init(void);
