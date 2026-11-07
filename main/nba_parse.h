// main/nba_parse.h —— 把 ESPN 记分 JSON 的 token 流整理成 nba_board_t。
//
// 只关心看板要显示的字段,其余(链接、赔率、高亮)一律丢弃。解析结果与"整包
// 缓存后一次解析"等价,但内存占用是常数,适合无 PSRAM 的 ESP32-C3。
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "nba_json.h"
#include "nba_model.h"

// 解析器状态。字段是公开的(测试需要按值检查),但只有 nba_parse_* 能改。
//
// NBA_PARSE_DEPTH 必须 >= NBA_JSON_STACK_MAX:分词器先按 24 层收容器,收集器若比它
// 先溢出,就会把一次合法响应判成语法错误。实测 ESPN 的单个比赛日响应最深 15 层
// (events[] > competitions[] > competitors[] > leaders[] > leaders[] > athlete),
// 早先取 12 导致真机上"解析失败(82027 字节)"。两层栈保持同深度,谁先满都能对上。
#define NBA_PARSE_DEPTH NBA_JSON_STACK_MAX
#define NBA_PARSE_KEY_LEN 20

_Static_assert(NBA_PARSE_DEPTH >= NBA_JSON_STACK_MAX,
               "解析器的容器栈不能比分词器浅");

typedef struct {
    bool is_home;
    int16_t score;
    int8_t quarters[NBA_QUARTERS];
    char abbr[NBA_ABBR_LEN];
    char record[NBA_RECORD_LEN];
} nba_parse_team_t;

typedef struct {
    nba_board_t *board;
    nba_json_t json;

    int8_t game;      // 当前比赛下标,-1 = 不在 events[] 元素内或已超上限
    uint8_t depth;    // 已打开容器数
    uint8_t kind[NBA_PARSE_DEPTH];            // '{' 或 '['
    char ckey[NBA_PARSE_DEPTH][NBA_PARSE_KEY_LEN];  // 打开该容器时的成员名
    uint16_t idx[NBA_PARSE_DEPTH];            // 所在数组的第几个元素(1 基)

    char key[NBA_PARSE_KEY_LEN];   // 最近读到的成员名
    bool has_key;

    nba_parse_team_t cur;          // 正在读的那个 competitor
    uint8_t quarter_pos;           // linescores 已写入的节数
    bool ok;                       // 语法未出错
} nba_parse_t;

// 开始一次新的解析:清空 board,重置状态。
void nba_parse_begin(nba_parse_t *state, nba_board_t *board);

// 喂入 HTTP 分片;可反复调用直到响应结束。
void nba_parse_feed(nba_parse_t *state, const char *data, size_t len);

// 本次解析是否成功(语法正确且 board 已填充,可能 0 场)。
bool nba_parse_ok(const nba_parse_t *state);
