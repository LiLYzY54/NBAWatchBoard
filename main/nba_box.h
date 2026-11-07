// main/nba_box.h —— 单场球员数据(box score)的模型与流式收集器。
//
// 数据来自 ESPN 的 summary 接口,该响应 ~450KB,但 boxscore 是整个文档的第一个键、
// 只占前 ~25KB。收集器在"两队球员都读完"时置 complete,调用方据此立刻断开连接,
// 剩下的 plays / winprobability 等大块内容根本不会下载。
//
// 与 nba_parse 一样:不依赖 ESP-IDF / LVGL,可在主机上单测。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nba_json.h"
#include "nba_model.h"

#define NBA_BOX_TEAMS 2
#define NBA_BOX_PLAYERS 12     // 每队解析容量(一轮名单最多 13 人,留点余量)
#define NBA_BOX_SHOW 10        // 排序后每队最多显示的球员数
#define NBA_NAME_LEN 24        // "S. Gilgeous-Alexander" 级别;超长由 UI 单行省略
// 容器栈与分词器同深:summary 响应的 boxscore 嵌套比记分接口更深,不能比它浅。
#define NBA_BOX_DEPTH NBA_JSON_STACK_MAX
#define NBA_BOX_KEY_LEN 16

typedef struct {
    char name[NBA_NAME_LEN];   // 名在前、姓在后:"T. Camara"(shortName 原序)
    int16_t pts;
    int16_t reb;
    int16_t ast;
    int16_t minutes;           // -1 = 未给出
    int16_t plus_minus;        // 正负值,可为负
    uint8_t fgm, fga;          // 投篮命中/出手
    uint8_t tpm, tpa;          // 三分命中/出手
    uint8_t ftm, fta;          // 罚球命中/出手
    uint8_t stl, blk, tov, pf; // 抢断/盖帽/失误/犯规
    bool played;               // didNotPlay 为真时置 false
} nba_player_t;

typedef struct {
    char abbr[NBA_ABBR_LEN];
    nba_player_t players[NBA_BOX_PLAYERS];
    uint8_t count;
} nba_team_box_t;

typedef struct {
    char game_id[16];                 // 这份数据属于哪场比赛(空串 = 无数据)
    nba_team_box_t teams[NBA_BOX_TEAMS];   // 解析时按接口顺序;nba_box_fix_order 后 0=客 1=主
    uint8_t team_count;
    bool ready;                       // 至少一队读到了球员
} nba_box_t;

// 要留下的列。接口的 statistics.keys[] 是这张表的"表头",顺序每场都可能变
// (有的场次会多出 teamId / reason 之类),所以只能按名字去对下标。
typedef enum {
    NBA_STAT_MIN = 0,
    NBA_STAT_PTS,
    NBA_STAT_FG,      // "4-9"
    NBA_STAT_TP,      // "2-11"
    NBA_STAT_FT,      // "5-6"
    NBA_STAT_REB,
    NBA_STAT_AST,
    NBA_STAT_TOV,
    NBA_STAT_STL,
    NBA_STAT_BLK,
    NBA_STAT_PF,
    NBA_STAT_PM,      // "+26" / "-17"
    NBA_STAT_COUNT,
} nba_stat_field_t;

typedef struct {
    nba_box_t *box;
    nba_json_t json;

    uint8_t depth;
    uint8_t kind[NBA_BOX_DEPTH];                 // '{' / '['
    char ckey[NBA_BOX_DEPTH][NBA_BOX_KEY_LEN];   // 打开该容器时的成员名
    char key[NBA_BOX_KEY_LEN];
    bool has_key;

    int8_t team;             // 当前正在读的 team 下标, -1 = 不在 players[] 元素内
    int8_t player;           // 当前正在读的球员下标, -1 = 不在 athletes[] 元素内
    bool in_statistics;      // 位于 statistics[] 元素对象内(其 keys[] 决定字段顺序)

    // keys[] 里各列的下标,-1 = 这场比赛没有这一列。
    int8_t idx[NBA_STAT_COUNT];
    int8_t key_pos;          // keys[] 已读到的位置
    int8_t stat_pos;         // 当前球员 stats[] 已读到的位置

    bool complete;           // players[] 已读完,可以断开连接
    bool ok;                 // 语法/深度未出错
} nba_box_parse_t;

// 清空一份球员数据(game_id 置空,ready=false)。
void nba_box_reset(nba_box_t *box);

// 把解析出来的两队按"客队在前"排好,并整理每队名单(去掉未上场、按得分降序)。
// away_abbr 为空或不匹配时保持接口顺序。返回是否至少一队有球员。
bool nba_box_fix_order(nba_box_t *box, const char *away_abbr);

// 开始一次新的解析。
void nba_box_parse_begin(nba_box_parse_t *state, nba_box_t *box);

// 喂入 HTTP 分片。
void nba_box_parse_feed(nba_box_parse_t *state, const char *data, size_t len);

// 两队球员都读完 → 调用方可以提前结束下载。
bool nba_box_parse_complete(const nba_box_parse_t *state);

// 语法是否正常且读到了至少一支球队(此时还没排序/去重,ready 也还没置位)。
bool nba_box_parse_ok(const nba_box_parse_t *state);

// 球员页的行数:队头行 + 该队球员。空队不算行,与 nba_ui.c 的摊平规则一致。
uint8_t nba_box_row_count(const nba_box_t *box);

// 拉取单场球员数据(HTTP)。成功时 box 已排好序、ready=true 且 game_id 已写入。
bool nba_box_fetch(nba_box_t *box, const char *event_id, int timeout_ms);
