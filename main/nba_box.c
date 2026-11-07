// main/nba_box.c —— summary 响应 -> nba_box_t 的字段收集器。
//
// 只认三个位置:
//   boxscore.players[]            每个元素是一支球队;
//   …players[].statistics[].keys[] 字段顺序(不同比赛会变,不能写死下标);
//   …statistics[].athletes[]       每个元素是一名球员(athlete.shortName + stats[])。
//   shortName 原样落盘:它是"名缩写在前、姓在后"(T. Camara),不重排语序。
// 其余(play-by-play、胜率曲线、伤病、赔率)一律丢弃,并且读完 players[] 就通知
// 调用方断开——那部分占了整份响应的 95%。
//
// 数组里的"裸值"(keys[]/stats[] 的字符串)没有成员名,所以 on_value 先判
// kind[d]=='[':此时 ckey[d] 才是它所属数组的名字;对象成员才看 st->key。
#include "nba_box.h"

#include <string.h>

// 只保留 ASCII:球员名里的重音字母(ć/š/ü)不在 LVGL 的 montserrat 字库里,
// 留着只会在屏幕上变成方框。丢掉高位字节后剩下的部分仍然可读。
static void copy_name(char *dst, size_t cap, const char *src, size_t len)
{
    if (cap == 0) return;
    size_t n = 0;
    for (size_t i = 0; i < len && n + 1 < cap; i++) {
        const unsigned char c = (unsigned char)src[i];
        if (c < 0x20 || c >= 0x7F) continue;
        dst[n++] = (char)c;
    }
    while (n > 0 && dst[n - 1] == ' ') n--;
    dst[n] = '\0';
}

static void copy_key(char *dst, size_t cap, const char *src, size_t len)
{
    if (cap == 0) return;
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int parse_int(const char *text, size_t len)
{
    if (!text || len == 0) return -1;
    int value = 0;
    bool any = false;
    for (size_t i = 0; i < len; i++) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            any = true;
            if (value > 10000) return -1;
        } else if (c == '-' && i == 0) {
            continue;          // "+26" / "-17" 里的符号
        } else if (c == '+' || c == '.') {
            if (c == '.') break;   // "31.0"
        } else {
            return -1;         // 复合字段交给 parse_pair
        }
    }
    return any ? value : -1;
}

// "+26" / "-17" / "0":带符号的正负值。parse_int 会把 "-17" 读成 17,所以单独一条。
static int parse_signed(const char *text, size_t len)
{
    if (!text || len == 0) return 0;
    int sign = 0;
    size_t i = 0;
    if (text[0] == '+') {
        i = 1;
    } else if (text[0] == '-') {
        sign = -1;
        i = 1;
    }
    int value = parse_int(text + i, len - i);
    if (value < 0) return 0;
    return sign < 0 ? -value : value;
}

// "4-9" -> 命中 4、出手 9。投篮/三分/罚球都是这种"命中-出手"的复合字段。
static bool parse_pair(const char *text, size_t len, int *made, int *attempted)
{
    if (!text || len == 0) return false;
    size_t dash = len;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '-') {
            dash = i;
            break;
        }
    }
    if (dash == len) return false;
    const int a = parse_int(text, dash);
    const int b = parse_int(text + dash + 1, len - dash - 1);
    if (a < 0 || b < 0) return false;
    *made = a;
    *attempted = b;
    return true;
}

static uint8_t clamp_u8(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}


void nba_box_reset(nba_box_t *box)
{
    if (!box) return;
    memset(box, 0, sizeof(*box));
}

static void begin_team(nba_box_parse_t *st)
{
    nba_box_t *box = st->box;
    if (box->team_count >= NBA_BOX_TEAMS) {
        st->team = -1;         // 超过两队:继续解析但不再记录
        return;
    }
    st->team = (int8_t)box->team_count++;
    memset(&box->teams[st->team], 0, sizeof(box->teams[st->team]));
    st->player = -1;
}

static void begin_statistics(nba_box_parse_t *st)
{
    st->in_statistics = true;
    for (int i = 0; i < NBA_STAT_COUNT; i++) st->idx[i] = -1;
    st->key_pos = 0;
}

static void begin_player(nba_box_parse_t *st)
{
    if (st->team < 0) {
        st->player = -1;
        return;
    }
    nba_team_box_t *team = &st->box->teams[st->team];
    if (team->count >= NBA_BOX_PLAYERS) {
        st->player = -1;
        return;
    }
    st->player = (int8_t)team->count++;
    nba_player_t *player = &team->players[st->player];
    memset(player, 0, sizeof(*player));
    player->minutes = -1;
    player->played = true;
    st->stat_pos = 0;
}

static void on_open(nba_box_parse_t *st, uint8_t kind)
{
    if (st->depth + 1 >= NBA_BOX_DEPTH) {
        st->ok = false;
        return;
    }
    const uint8_t d = (uint8_t)(st->depth + 1);
    st->depth = d;
    st->kind[d] = kind;
    if (st->has_key) {
        copy_key(st->ckey[d], NBA_BOX_KEY_LEN, st->key, strlen(st->key));
    } else {
        st->ckey[d][0] = '\0';
    }
    st->has_key = false;

    if (kind != '{' || st->kind[d - 1] != '[') return;
    const char *array_key = st->ckey[d - 1];
    if (strcmp(array_key, "players") == 0) {
        begin_team(st);
    } else if (strcmp(array_key, "athletes") == 0) {
        begin_player(st);
    } else if (strcmp(array_key, "statistics") == 0) {
        begin_statistics(st);
    }
}

static void on_close(nba_box_parse_t *st, uint8_t kind)
{
    const uint8_t d = st->depth;
    if (d == 0) {
        st->ok = false;
        return;
    }
    // 关的是数组本身时,ckey[d] 是数组名;关的是数组元素时看 ckey[d-1]。
    if (kind == '[' && strcmp(st->ckey[d], "players") == 0 && !st->complete) {
        st->complete = true;   // 两队名单都在了,后面的 play-by-play 不必再下
    } else if (kind == '{' && st->kind[d - 1] == '[') {
        const char *array_key = st->ckey[d - 1];
        if (strcmp(array_key, "statistics") == 0) {
            st->in_statistics = false;
        } else if (strcmp(array_key, "athletes") == 0) {
            st->player = -1;
        } else if (strcmp(array_key, "players") == 0) {
            st->team = -1;
        }
    }
    st->depth = (uint8_t)(d - 1);
}

// 数组里的裸字符串:keys[](字段名列表)或 stats[](某个球员的数值列表)。
static bool key_is(const char *text, size_t len, const char *literal)
{
    const size_t n = strlen(literal);
    return len == n && memcmp(text, literal, n) == 0;
}

// keys[] 的一项 -> 我们关心的列。表里没有的名字(offensiveRebounds 等)直接忽略。
static void map_key(nba_box_parse_t *st, const char *text, size_t len, int8_t pos)
{
    static const struct { const char *name; uint8_t field; } kMap[] = {
        {"minutes", NBA_STAT_MIN},
        {"points", NBA_STAT_PTS},
        {"fieldGoalsMade-fieldGoalsAttempted", NBA_STAT_FG},
        {"threePointFieldGoalsMade-threePointFieldGoalsAttempted", NBA_STAT_TP},
        {"freeThrowsMade-freeThrowsAttempted", NBA_STAT_FT},
        {"rebounds", NBA_STAT_REB},
        {"assists", NBA_STAT_AST},
        {"turnovers", NBA_STAT_TOV},
        {"steals", NBA_STAT_STL},
        {"blocks", NBA_STAT_BLK},
        {"fouls", NBA_STAT_PF},
        {"plusMinus", NBA_STAT_PM},
    };
    for (size_t i = 0; i < sizeof(kMap) / sizeof(kMap[0]); i++) {
        if (key_is(text, len, kMap[i].name)) {
            st->idx[kMap[i].field] = pos;
            return;
        }
    }
}

static void apply_stat(nba_player_t *player, uint8_t field, const char *text, size_t len)
{
    int made = 0;
    int attempted = 0;
    switch (field) {
    case NBA_STAT_MIN: player->minutes = (int16_t)parse_int(text, len); break;
    case NBA_STAT_PTS: player->pts = (int16_t)parse_int(text, len); break;
    case NBA_STAT_REB: player->reb = (int16_t)parse_int(text, len); break;
    case NBA_STAT_AST: player->ast = (int16_t)parse_int(text, len); break;
    case NBA_STAT_STL: player->stl = clamp_u8(parse_int(text, len)); break;
    case NBA_STAT_BLK: player->blk = clamp_u8(parse_int(text, len)); break;
    case NBA_STAT_TOV: player->tov = clamp_u8(parse_int(text, len)); break;
    case NBA_STAT_PF:  player->pf = clamp_u8(parse_int(text, len)); break;
    case NBA_STAT_PM:  player->plus_minus = (int16_t)parse_signed(text, len); break;
    case NBA_STAT_FG:
        if (parse_pair(text, len, &made, &attempted)) {
            player->fgm = clamp_u8(made);
            player->fga = clamp_u8(attempted);
        }
        break;
    case NBA_STAT_TP:
        if (parse_pair(text, len, &made, &attempted)) {
            player->tpm = clamp_u8(made);
            player->tpa = clamp_u8(attempted);
        }
        break;
    case NBA_STAT_FT:
        if (parse_pair(text, len, &made, &attempted)) {
            player->ftm = clamp_u8(made);
            player->fta = clamp_u8(attempted);
        }
        break;
    default:
        break;
    }
}

static void on_element(nba_box_parse_t *st, const char *text, size_t len)
{
    const uint8_t d = st->depth;
    const char *array_key = st->ckey[d];

    if (strcmp(array_key, "keys") == 0 && st->in_statistics) {
        map_key(st, text, len, st->key_pos++);
        return;
    }

    if (strcmp(array_key, "stats") == 0 && st->team >= 0 && st->player >= 0) {
        const int8_t pos = st->stat_pos++;
        nba_player_t *player = &st->box->teams[st->team].players[st->player];
        for (int i = 0; i < NBA_STAT_COUNT; i++) {
            if (st->idx[i] == pos) {
                apply_stat(player, (uint8_t)i, text, len);
                break;
            }
        }
    }
}

static void on_value(nba_box_parse_t *st, const char *text, size_t len)
{
    const uint8_t d = st->depth;
    if (d == 0) {
        st->has_key = false;
        return;
    }
    if (st->kind[d] == '[') {          // 数组元素:没有成员名
        st->has_key = false;
        on_element(st, text, len);
        return;
    }

    const bool has_member = st->has_key;
    st->has_key = false;
    if (!has_member) return;

    if (st->team >= 0 && strcmp(st->ckey[d], "team") == 0 &&
        strcmp(st->key, "abbreviation") == 0) {
        copy_key(st->box->teams[st->team].abbr, NBA_ABBR_LEN, text, len);
        return;
    }
    if (st->player < 0) return;

    nba_player_t *player = &st->box->teams[st->team].players[st->player];
    if (strcmp(st->ckey[d], "athlete") == 0 && strcmp(st->key, "shortName") == 0) {
        copy_name(player->name, sizeof(player->name), text, len);
    }
}

// "didNotPlay": true 之类没有文本的值也要处理,单独走一条路径。
// 注意它在 athletes[] 元素对象上,不在 athlete{} 里面,所以判的是父数组名。
static void on_flag(nba_box_parse_t *st, nba_json_token_t token)
{
    const uint8_t d = st->depth;
    const bool has_member = st->has_key;
    st->has_key = false;
    if (!has_member || d == 0 || st->kind[d] != '{') return;
    if (st->team < 0 || st->player < 0) return;
    if (st->kind[d - 1] != '[' || strcmp(st->ckey[d - 1], "athletes") != 0) return;

    if (strcmp(st->key, "didNotPlay") == 0 && token == NBA_JSON_TRUE) {
        st->box->teams[st->team].players[st->player].played = false;
    }
}

static void on_token(void *ctx, nba_json_token_t token, const char *text, size_t len)
{
    nba_box_parse_t *st = (nba_box_parse_t *)ctx;
    if (!st->ok) return;

    switch (token) {
    case NBA_JSON_OBJECT_BEGIN:
        on_open(st, '{');
        break;
    case NBA_JSON_ARRAY_BEGIN:
        on_open(st, '[');
        break;
    case NBA_JSON_OBJECT_END:
        on_close(st, '{');
        break;
    case NBA_JSON_ARRAY_END:
        on_close(st, '[');
        break;
    case NBA_JSON_KEY:
        copy_key(st->key, NBA_BOX_KEY_LEN, text, len);
        st->has_key = true;
        break;
    case NBA_JSON_STRING:
    case NBA_JSON_NUMBER:
        on_value(st, text, len);
        break;
    case NBA_JSON_TRUE:
    case NBA_JSON_FALSE:
    case NBA_JSON_NULL:
        on_flag(st, token);
        break;
    }
}

void nba_box_parse_begin(nba_box_parse_t *state, nba_box_t *box)
{
    memset(state, 0, sizeof(*state));
    state->box = box;
    state->team = -1;
    state->player = -1;
    state->ok = true;
    state->kind[0] = '{';      // 根容器按对象处理,与 nba_parse 保持一致
    state->ckey[0][0] = '\0';
    nba_box_reset(box);
    nba_json_init(&state->json, on_token, state);
}

void nba_box_parse_feed(nba_box_parse_t *state, const char *data, size_t len)
{
    if (!state->ok || state->complete) return;
    nba_json_feed(&state->json, data, len);
    if (nba_json_failed(&state->json)) state->ok = false;
}

bool nba_box_parse_complete(const nba_box_parse_t *state)
{
    return state->complete && state->box->team_count > 0;
}

bool nba_box_parse_ok(const nba_box_parse_t *state)
{
    return state->ok && state->box->team_count > 0;
}

uint8_t nba_box_row_count(const nba_box_t *box)
{
    if (!box) return 0;
    uint8_t rows = 0;
    for (uint8_t i = 0; i < box->team_count; i++) {
        if (box->teams[i].count == 0) continue;
        rows = (uint8_t)(rows + 1 + box->teams[i].count);
    }
    return rows;
}

static bool player_counts(const nba_player_t *player)
{
    if (!player->played) return false;
    // DNP 球员在 ESPN 里是整行 0(分钟也是 0),留着只会挤掉真正上场的人。
    return player->minutes > 0 || player->pts > 0 || player->reb > 0 || player->ast > 0;
}

static void team_finish(nba_team_box_t *team)
{
    uint8_t kept = 0;
    for (uint8_t i = 0; i < team->count; i++) {
        if (player_counts(&team->players[i])) team->players[kept++] = team->players[i];
    }
    team->count = kept;

    // 按得分降序;同分看篮板+助攻,再看上场时间。名单只有十几人,插入排序足够。
    // 截断必须排在排序之后:接口给的是"首发在前"的顺序,直接在解析顺序上截会
    // 把末位的高分替补丢掉。
    for (uint8_t i = 1; i < team->count; i++) {
        nba_player_t key = team->players[i];
        int j = (int)i - 1;
        while (j >= 0) {
            const nba_player_t *cur = &team->players[j];
            const int cur_rank = cur->pts * 100 + (cur->reb + cur->ast) * 2 + cur->minutes;
            const int key_rank = key.pts * 100 + (key.reb + key.ast) * 2 + key.minutes;
            if (cur_rank >= key_rank) break;
            team->players[j + 1] = team->players[j];
            j--;
        }
        team->players[j + 1] = key;
    }
    if (team->count > NBA_BOX_SHOW) team->count = NBA_BOX_SHOW;
}

bool nba_box_fix_order(nba_box_t *box, const char *away_abbr)
{
    if (!box) return false;
    for (uint8_t i = 0; i < box->team_count; i++) team_finish(&box->teams[i]);

    // 接口按 displayOrder 给两队,不保证客队在前;用缩写对上总比分里的客队最稳。
    if (away_abbr && away_abbr[0] && box->team_count == 2 &&
        box->teams[1].abbr[0] && strcmp(box->teams[1].abbr, away_abbr) == 0) {
        const nba_team_box_t tmp = box->teams[0];
        box->teams[0] = box->teams[1];
        box->teams[1] = tmp;
    }

    bool any = false;
    for (uint8_t i = 0; i < box->team_count; i++) {
        if (box->teams[i].count > 0) any = true;
    }
    box->ready = any;
    return any;
}
