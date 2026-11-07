// main/nba_model.h —— NBA 比分看板的数据模型与纯格式化接口。
//
// 本文件只描述"看板要显示什么",不依赖 ESP-IDF / LVGL,便于在主机上做单元测试:
// 解析(nba_parse.c)与格式化(nba_format.c)都只操作这里的结构体。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

// 一次看板最多容纳的比赛数。240x320 屏一次只能看见 4 场,列表靠上下键滚动。
// 30 支球队单日最多 15 场,取 16 封顶;比美东日期多出来的北京凌晨场次还会被
// keep_day 过滤掉一部分,所以上限必须给足,否则"十几场比赛只显示一半"。
#define NBA_MAX_GAMES 16
#define NBA_ABBR_LEN 6      // 队名缩写最多 3~4 字符 + NUL
#define NBA_RECORD_LEN 8    // "3-3" / "12-4"
#define NBA_CLOCK_LEN 8     // "5:24"
#define NBA_TEXT_LEN 24     // 状态原文(英文短描述)
#define NBA_QUARTERS 4

typedef enum {
    NBA_STATE_UNKNOWN = 0,
    NBA_STATE_PRE,      // 未开始
    NBA_STATE_LIVE,     // 进行中
    NBA_STATE_POST,     // 已结束
} nba_state_t;

typedef struct {
    char abbr[NBA_ABBR_LEN];
    char record[NBA_RECORD_LEN];
    int16_t score;                  // -1 = 未知
    int8_t quarters[NBA_QUARTERS];  // 每节得分, -1 = 未知
} nba_team_t;

typedef struct {
    char id[16];                    // ESPN event id,详情页刷新时定位同一场
    char tipoff[24];                // 原始 ISO8601 UTC 时间(未开始场次显示开赛时间)
    nba_team_t away;
    nba_team_t home;
    nba_state_t state;
    int8_t period;                  // 1..4 常规, >4 加时
    char clock[NBA_CLOCK_LEN];      // 进行中的剩余时间
    bool detailed;                  // 已收到状态细节(区分"空"和"0")
} nba_game_t;

// 比赛日(公历年月日)。看板按**北京时间的日历日**分组,与国内赛程表一致:
// 北京时间 11-01 早上 08:00 开打的比赛算 11-01 的比赛。注意 ESPN 接口是按美东
// 日期分桶的,同一天在那里叫 10-31,换算在应用层做(nba_app.c 的 fetch_day)。
typedef struct {
    int16_t year;   // 完整年份,如 2026
    int8_t month;   // 1..12
    int8_t day;     // 1..31
} nba_date_t;

typedef struct {
    nba_game_t games[NBA_MAX_GAMES];
    uint8_t count;
    nba_date_t day;   // 这批比赛所属的北京日期;全 0 表示未知(解析不出来/未对时)
} nba_board_t;

// 清空看板(count=0,所有字段回到未知态)。
void nba_board_reset(nba_board_t *board);

// 节次中文名:"第 3 节" / "加时";返回写入长度(不含 NUL)。
// 仅在 period>=1 时有意义,period<1 返回 0 并写入空串。
size_t nba_format_period(int period, char *buf, size_t cap);

// 一场比赛的状态短句,用于总览列表右列:
//   未开始 -> "未开始" 或 "20:30" (tipoff_local 非空时)
//   进行中 -> "第 3 节 5:24"
//   已结束 -> "已结束" (period>4 时追加 "加时")
// 返回写入长度(不含 NUL)。
size_t nba_format_status(const nba_game_t *game, const char *tipoff_local,
                         char *buf, size_t cap);

// 比分文本:未知(-1)输出 "--",否则十进制。
size_t nba_format_score(int score, char *buf, size_t cap);

// 把 ISO8601 UTC("2026-10-30T23:00Z")按进程本地时间格式化为 "07:00"。
// 解析失败返回 0 并写入 "--:--"(设备未对时也是这个结果,调用方据此隐藏时间)。
size_t nba_format_tipoff_local(const char *iso, char *buf, size_t cap);

// 同一串时间换算成 Unix epoch 秒(与时区无关)。刷新策略用它算"距下一场开赛
// 还有多久";解析失败返回 false。主机测试可直接断言确定值。
bool nba_tipoff_epoch(const char *iso, time_t *out);

// 列表滚动窗口:屏幕一次只显示 visible 行,返回窗口首行下标(尽量让 selected
// 停在窗口底部,越过边界时夹紧)。count/visible 非法时返回 0。
int nba_list_first(int selected, int count, int visible);

// ---- 比赛日(北京时间)----------------------------------------------------

// 年月日是否落在合法范围(不校验当月天数上限,够用即可)。
bool nba_date_valid(const nba_date_t *date);

// ESPN 的 "YYYY-MM-DDTHH:MMZ"(UTC)换算成北京时间(UTC+8)的日历日。
// 用于判断一场比赛属于哪一天:北京时间 03:00 开打的比赛,UTC 还在前一天。
bool nba_date_from_iso_beijing(const char *iso, nba_date_t *out);

// 设备当前时间(需已 SNTP 对时)对应的北京日期。未对时返回 false,
// 调用方应退回"从比赛数据里反推"的方式。
bool nba_date_today_beijing(nba_date_t *out);

// 日期加减天数,自动跨月/跨年/闰年。原地修改,返回是否成功。
bool nba_date_add_days(nba_date_t *date, int delta);

// 顶栏日期文本 "10-31";date 非法时写入空串。返回写入长度。
size_t nba_format_day(const nba_date_t *date, char *buf, size_t cap);

// 接口查询参数 "20261031"。date 非法时写入空串。返回写入长度。
size_t nba_format_day_query(const nba_date_t *date, char *buf, size_t cap);

// 只保留开球时间落在 day 这个北京日期的场次,其余丢弃并把选中项前移。
// 开球时间解析不出来的场次一律保留(宁可多显示一场,也不要凭空丢数据)。
// 返回值 = 保留后的场次数量;有保留时顺手把 board->day 设为 day。
uint8_t nba_board_keep_day(nba_board_t *board, const nba_date_t *day);

// 用首场比赛的开球时间补出 board->day(接口不含"这属于哪天"的字段)。
// 已有合法 day 或没有可用比赛时保持原样,返回是否成功得到日期。
bool nba_board_day_from_games(nba_board_t *board);
