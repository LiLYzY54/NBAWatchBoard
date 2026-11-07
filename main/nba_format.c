// main/nba_format.c —— 看板文本格式化(纯逻辑,主机可测)。
//
// 主机单测在 Linux/glibc 下按 -std=c11 编译:localtime_r/gmtime_r 只有打开
// POSIX 特性宏才会声明(macOS 默认就能看见,本地不报、CI 报隐式声明)。
// 必须在任何系统头文件之前定义,所以放在所有 include 前面。
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "nba_model.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

void nba_board_reset(nba_board_t *board)
{
    if (!board) return;
    memset(board, 0, sizeof(*board));
}

static size_t write_str(char *buf, size_t cap, const char *src)
{
    if (!buf || cap == 0) return 0;
    const size_t len = strlen(src);
    const size_t n = (len < cap - 1) ? len : cap - 1;
    memcpy(buf, src, n);
    buf[n] = '\0';
    return n;
}

size_t nba_format_period(int period, char *buf, size_t cap)
{
    char tmp[32];   // 留足余量:格式化函数的 -Werror=format-truncation 不接受"刚好够"
    if (period < 1) return write_str(buf, cap, "");
    if (period <= NBA_QUARTERS) {
        snprintf(tmp, sizeof(tmp), "第%d节", period);
    } else if (period == NBA_QUARTERS + 1) {
        snprintf(tmp, sizeof(tmp), "加时");
    } else {
        snprintf(tmp, sizeof(tmp), "加时%d", period - NBA_QUARTERS);
    }
    return write_str(buf, cap, tmp);
}

size_t nba_format_score(int score, char *buf, size_t cap)
{
    char tmp[16];
    if (score < 0) return write_str(buf, cap, "--");
    snprintf(tmp, sizeof(tmp), "%d", score);
    return write_str(buf, cap, tmp);
}

size_t nba_format_status(const nba_game_t *game, const char *tipoff_local,
                         char *buf, size_t cap)
{
    if (!game) return write_str(buf, cap, "--");
    char tmp[48];

    switch (game->state) {
    case NBA_STATE_PRE:
        // 已对时才有意义;没对上升级失败时退回"未开始",不让列表出现 "--:--"。
        if (tipoff_local && strcmp(tipoff_local, "--:--") != 0) {
            return write_str(buf, cap, tipoff_local);
        }
        return write_str(buf, cap, "未开始");
    case NBA_STATE_LIVE: {
        char period[32];
        nba_format_period(game->period, period, sizeof(period));
        if (game->clock[0] != '\0' && strcmp(game->clock, "0.0") != 0) {
            snprintf(tmp, sizeof(tmp), "%s %s", period, game->clock);
        } else {
            snprintf(tmp, sizeof(tmp), "%s", period);
        }
        return write_str(buf, cap, tmp);
    }
    case NBA_STATE_POST:
        if (game->period > NBA_QUARTERS) return write_str(buf, cap, "已结束 加时");
        return write_str(buf, cap, "已结束");
    default:
        return write_str(buf, cap, "--");
    }
}

// Howard Hinnant 的 days_from_civil:把公历日期换成 1970-01-01 起的天数。
// 自己算是为了让 ISO8601 -> epoch 的转换不依赖 timegm(它在部分 libc 上不可用),
// 且能在主机测试里得到与时区无关的确定值。
static int64_t days_from_civil(int64_t y, int64_t m, int64_t d)
{
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;                                  // [0, 399]
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;          // [0, 146096]
    return era * 146097 + doe - 719468;
}

static bool parse_iso_utc(const char *iso, struct tm *tm_utc)
{
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (!iso) return false;
    // 只接受 "YYYY-MM-DDTHH:MM(:SS)?Z" 这一种 ESPN 实际返回的形状。
    if (sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2d", &year, &month, &day,
               &hour, &minute, &second) < 5) {
        return false;
    }
    if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return false;
    }
    memset(tm_utc, 0, sizeof(*tm_utc));
    tm_utc->tm_year = year - 1900;
    tm_utc->tm_mon = month - 1;
    tm_utc->tm_mday = day;
    tm_utc->tm_hour = hour;
    tm_utc->tm_min = minute;
    tm_utc->tm_sec = second;
    tm_utc->tm_isdst = 0;
    return true;
}

bool nba_tipoff_epoch(const char *iso, time_t *out)
{
    struct tm tm_utc;
    if (!out || !parse_iso_utc(iso, &tm_utc)) return false;
    const int64_t days = days_from_civil(tm_utc.tm_year + 1900, tm_utc.tm_mon + 1,
                                         tm_utc.tm_mday);
    *out = (time_t)(days * 86400 + tm_utc.tm_hour * 3600 +
                    tm_utc.tm_min * 60 + tm_utc.tm_sec);
    return true;
}

size_t nba_format_tipoff_local(const char *iso, char *buf, size_t cap)
{
    time_t stamp;
    if (!nba_tipoff_epoch(iso, &stamp)) return write_str(buf, cap, "--:--");

    // 设备未对时(mktime/localtime 都能跑,但系统时间是 1970)时,本地时间毫无意义:
    // 用"当前时间是否早于 2020"作为已对时的判据,避免把 1970 的时区偏移当开赛时间。
    const time_t now = time(NULL);
    if (now < (time_t)1577836800) return write_str(buf, cap, "--:--");

    struct tm tm_local;
    if (!localtime_r(&stamp, &tm_local)) return write_str(buf, cap, "--:--");

    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%02d:%02d", tm_local.tm_hour, tm_local.tm_min);
    return write_str(buf, cap, tmp);
}

int nba_list_first(int selected, int count, int visible)
{
    if (count <= 0 || visible <= 0) return 0;
    if (count <= visible) return 0;
    int first = selected - visible + 1;
    if (first < 0) first = 0;
    if (first > count - visible) first = count - visible;
    return first;
}

// ---- 比赛日 -------------------------------------------------------------

// days_from_civil 的逆运算(Hinnant):天数 -> 公历年月日。
static void civil_from_days(int64_t z, nba_date_t *out)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;                                       // [0, 146096]
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;  // [0, 399]
    const int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);                // [0, 365]
    const int64_t mp = (5 * doy + 2) / 153;                                     // [0, 11]
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;                             // [1, 31]
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;                                // [1, 12]
    out->year = (int16_t)(y + (m <= 2 ? 1 : 0));
    out->month = (int8_t)m;
    out->day = (int8_t)d;
}

bool nba_date_valid(const nba_date_t *date)
{
    if (!date) return false;
    return date->year >= 2000 && date->year <= 2100 &&
           date->month >= 1 && date->month <= 12 &&
           date->day >= 1 && date->day <= 31;
}

// 把 UTC 的"年月日 + 当天第几分钟"按固定时区偏移折算成日历日。
// 换算全程用整数天/分钟走,不碰 mktime:设备未对时也能算,且主机测试与时区无关。
static void date_from_utc(int year, int month, int day, int utc_min, int offset_min,
                          nba_date_t *out)
{
    int64_t days = days_from_civil(year, month, day);
    int minutes = utc_min + offset_min;
    while (minutes < 0) {
        minutes += 24 * 60;
        days--;
    }
    while (minutes >= 24 * 60) {
        minutes -= 24 * 60;
        days++;
    }
    civil_from_days(days, out);
}

bool nba_date_from_iso_beijing(const char *iso, nba_date_t *out)
{
    struct tm tm_utc;
    if (!out || !parse_iso_utc(iso, &tm_utc)) return false;
    date_from_utc(tm_utc.tm_year + 1900, tm_utc.tm_mon + 1, tm_utc.tm_mday,
                  tm_utc.tm_hour * 60 + tm_utc.tm_min, 8 * 60, out);
    return true;
}

bool nba_date_today_beijing(nba_date_t *out)
{
    if (!out) return false;
    const time_t now = time(NULL);
    if (now < (time_t)1577836800) return false;   // 未对时:1970 的时间没有意义
    struct tm g;
    if (!gmtime_r(&now, &g)) return false;
    date_from_utc(g.tm_year + 1900, g.tm_mon + 1, g.tm_mday,
                  g.tm_hour * 60 + g.tm_min, 8 * 60, out);
    return true;
}

bool nba_date_add_days(nba_date_t *date, int delta)
{
    if (!nba_date_valid(date)) return false;
    const int64_t days = days_from_civil(date->year, date->month, date->day) + delta;
    civil_from_days(days, date);
    return true;
}

size_t nba_format_day(const nba_date_t *date, char *buf, size_t cap)
{
    if (!nba_date_valid(date)) return write_str(buf, cap, "");
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%02d-%02d", date->month, date->day);
    return write_str(buf, cap, tmp);
}

size_t nba_format_day_query(const nba_date_t *date, char *buf, size_t cap)
{
    if (!nba_date_valid(date)) return write_str(buf, cap, "");
    char tmp[16];
    snprintf(tmp, sizeof(tmp), "%04d%02d%02d", date->year, date->month, date->day);
    return write_str(buf, cap, tmp);
}

uint8_t nba_board_keep_day(nba_board_t *board, const nba_date_t *day)
{
    if (!board || !nba_date_valid(day)) {
        if (board) board->count = 0;
        return 0;
    }

    uint8_t kept = 0;
    for (uint8_t i = 0; i < board->count; i++) {
        nba_date_t game_day;
        if (nba_date_from_iso_beijing(board->games[i].tipoff, &game_day)) {
            // 解析成功才判断归属;解析失败属于"不知道哪天",下面照样保留。
            const bool same_day = game_day.year == day->year &&
                                  game_day.month == day->month && game_day.day == day->day;
            if (!same_day) continue;
        }
        if (kept != i) board->games[kept] = board->games[i];
        kept++;
    }
    board->count = kept;
    if (kept > 0) board->day = *day;
    return kept;
}

bool nba_board_day_from_games(nba_board_t *board)
{
    if (!board) return false;
    if (nba_date_valid(&board->day)) return true;
    for (uint8_t i = 0; i < board->count; i++) {
        nba_date_t day;
        if (nba_date_from_iso_beijing(board->games[i].tipoff, &day)) {
            board->day = day;
            return true;
        }
    }
    return false;
}
