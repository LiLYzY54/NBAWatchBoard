// main/nba_api.h —— 拉取 NBA 记分数据(HTTP + 流式解析)。
#pragma once

#include <stdbool.h>

#include "nba_model.h"

typedef enum {
    NBA_FETCH_OK = 0,
    NBA_FETCH_HTTP_FAIL,    // 连不上 / 非 200 / 读超时
    NBA_FETCH_PARSE_FAIL,   // 响应不是预期的 JSON 结构
    NBA_FETCH_NO_MEM,       // 客户端或缓冲区申请失败
    NBA_FETCH_PORTAL,       // 被 Captive Portal 302 到网页认证页(Wi-Fi 连着但外网不通)
} nba_fetch_result_t;

// 拉取指定比赛日的比分并写入 board。day 为 NULL 时请求接口的"当天",由服务端
// 决定是哪一天(首次抓取用它来校准日期)。阻塞调用,耗时可达数秒,必须在工作
// 任务里跑。无论成败,board 都会先被清空(失败时调用方应保留自己那份旧数据)。
nba_fetch_result_t nba_api_fetch(nba_board_t *board, const nba_date_t *day,
                                 int timeout_ms);

// 本轮抓取是否撞上了 Captive Portal(读取即清零,由网络任务每轮消费一次)。
bool nba_api_take_portal_flag(void);
