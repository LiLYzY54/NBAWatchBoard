// main/nba_parse.c —— token 流 -> nba_board_t 的字段收集器。
//
// 路径识别规则只有两条,其它判断都由它们拼出来:
//   * 普通成员:当前容器(kind 为 '{')的成员名 ckey[depth] 指明了它属于哪个对象;
//   * 数组元素成员的"外部键"是数组本身,即 ckey[depth-1](kind[depth-1] 为 '[')。
// 例如 competitors 数组里的 score 属于某个 competitor;team 对象里的 abbreviation
// 属于该队的缩写。这样就不需要给每种嵌套单独写一遍层级判断。
#include "nba_parse.h"

#include <stdlib.h>
#include <string.h>

// 拷贝并保证 NUL 结尾;超长时只在 UTF-8 字符边界截断,避免把半个汉字写进屏幕。
static void copy_text(char *dst, size_t cap, const char *src, size_t len)
{
    if (cap == 0) return;
    if (len >= cap) len = cap - 1;
    while (len > 0 && (((unsigned char)src[len]) & 0xC0) == 0x80) len--;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static int parse_int(const char *text, size_t len)
{
    if (!text || len == 0) return -1;
    long value = 0;
    bool any = false;
    for (size_t i = 0; i < len; i++) {
        const char c = text[i];
        if (c >= '0' && c <= '9') {
            value = value * 10 + (c - '0');
            any = true;
            if (value > 100000) return -1;
        } else if (c == '-' && i == 0) {
            continue;      // 负数不在看板字段里出现,顺手容忍
        } else if (c == '.') {
            break;         // "95.0" 之类按整数截断
        } else {
            return -1;
        }
    }
    return any ? (int)value : -1;
}

static void reset_team(nba_parse_team_t *team)
{
    memset(team, 0, sizeof(*team));
    team->score = -1;
    for (int i = 0; i < NBA_QUARTERS; i++) team->quarters[i] = -1;
}

static void start_game(nba_parse_t *st)
{
    if (st->board->count >= NBA_MAX_GAMES) {
        st->game = -1;     // 超出上限:继续解析但不再记录,免得越界
        return;
    }
    const uint8_t index = st->board->count++;
    nba_game_t *game = &st->board->games[index];
    memset(game, 0, sizeof(*game));
    game->away.score = -1;
    game->home.score = -1;
    game->period = -1;
    game->state = NBA_STATE_UNKNOWN;
    for (int i = 0; i < NBA_QUARTERS; i++) {
        game->away.quarters[i] = -1;
        game->home.quarters[i] = -1;
    }
    st->game = (int8_t)index;
    st->quarter_pos = 0;
}

static void commit_competitor(nba_parse_t *st)
{
    if (st->game < 0) return;
    nba_game_t *game = &st->board->games[st->game];
    nba_team_t *dst = st->cur.is_home ? &game->home : &game->away;
    copy_text(dst->abbr, sizeof(dst->abbr), st->cur.abbr, strlen(st->cur.abbr));
    copy_text(dst->record, sizeof(dst->record), st->cur.record, strlen(st->cur.record));
    dst->score = st->cur.score;
    for (int i = 0; i < NBA_QUARTERS; i++) dst->quarters[i] = st->cur.quarters[i];
}

static void on_open(nba_parse_t *st, uint8_t kind)
{
    if (st->depth + 1 >= NBA_PARSE_DEPTH) {
        st->ok = false;
        return;
    }
    const uint8_t d = (uint8_t)(st->depth + 1);
    st->depth = d;
    st->kind[d] = kind;
    if (st->has_key) {
        copy_text(st->ckey[d], NBA_PARSE_KEY_LEN, st->key, strlen(st->key));
    } else {
        st->ckey[d][0] = '\0';
    }
    st->idx[d] = 0;
    st->has_key = false;

    if (st->kind[d - 1] == '[') st->idx[d - 1]++;   // 本容器是父数组的第几个元素

    if (kind == '{' && st->kind[d - 1] == '[') {
        if (strcmp(st->ckey[d - 1], "events") == 0) {
            start_game(st);
        } else if (strcmp(st->ckey[d - 1], "competitors") == 0) {
            reset_team(&st->cur);
            st->quarter_pos = 0;
        }
    }
}

static void on_close(nba_parse_t *st, uint8_t kind)
{
    const uint8_t d = st->depth;
    if (d == 0) {
        st->ok = false;
        return;
    }
    if (kind == '{' && st->kind[d - 1] == '[' &&
        strcmp(st->ckey[d - 1], "competitors") == 0) {
        commit_competitor(st);
    }
    st->depth = (uint8_t)(d - 1);
}

static nba_state_t state_from_text(const char *text, size_t len)
{
    if (len == 3 && memcmp(text, "pre", 3) == 0) return NBA_STATE_PRE;
    if (len == 2 && memcmp(text, "in", 2) == 0) return NBA_STATE_LIVE;
    if (len == 4 && memcmp(text, "post", 4) == 0) return NBA_STATE_POST;
    return NBA_STATE_UNKNOWN;
}

static void on_value(nba_parse_t *st, nba_json_token_t token,
                     const char *text, size_t len)
{
    const uint8_t d = st->depth;
    const bool has_member = st->has_key;
    st->has_key = false;
    if (!has_member || d == 0) return;

    const bool in_object = st->kind[d] == '{';
    const char *container_key = st->ckey[d];
    const char *array_key = (st->kind[d - 1] == '[') ? st->ckey[d - 1] : NULL;
    nba_game_t *game = (st->game >= 0) ? &st->board->games[st->game] : NULL;

    // ---- 比赛级字段(events[] 元素的直接成员)
    if (array_key && strcmp(array_key, "events") == 0 && game) {
        if (strcmp(st->key, "id") == 0) {
            copy_text(game->id, sizeof(game->id), text, len);
        } else if (strcmp(st->key, "date") == 0) {
            copy_text(game->tipoff, sizeof(game->tipoff), text, len);
        }
        return;
    }

    // ---- 球队级字段(competitors[] / records[] 元素的成员)
    if (array_key && strcmp(array_key, "competitors") == 0 && st->game >= 0) {
        if (strcmp(st->key, "homeAway") == 0) {
            st->cur.is_home = (len == 4 && memcmp(text, "home", 4) == 0);
        } else if (strcmp(st->key, "score") == 0) {
            st->cur.score = (int16_t)parse_int(text, len);
        }
        return;
    }
    if (array_key && strcmp(array_key, "records") == 0 && st->game >= 0) {
        if (strcmp(st->key, "summary") == 0) {
            copy_text(st->cur.record, sizeof(st->cur.record), text, len);
        }
        return;
    }
    if (array_key && strcmp(array_key, "linescores") == 0 && st->game >= 0) {
        if (strcmp(st->key, "period") == 0) {
            const int period = parse_int(text, len);
            // 元素里 "value" 排在 "period" 之前(实测),所以这里只能把游标往后推、
            // 绝不能回退:回退会让下一节的值覆盖上一节,末节则永远读不到,整行左移一格。
            if (period >= 1 && period <= NBA_QUARTERS &&
                st->quarter_pos < (uint8_t)(period - 1)) {
                st->quarter_pos = (uint8_t)(period - 1);
            }
        } else if (strcmp(st->key, "value") == 0) {
            if (st->quarter_pos < NBA_QUARTERS) {
                st->cur.quarters[st->quarter_pos] = (int8_t)parse_int(text, len);
                st->quarter_pos++;
            }
        }
        return;
    }

    // ---- 状态字段(status{} / status.type{} 的成员)
    if (in_object && game) {
        if (strcmp(container_key, "status") == 0) {
            if (strcmp(st->key, "displayClock") == 0) {
                copy_text(game->clock, sizeof(game->clock), text, len);
            } else if (strcmp(st->key, "period") == 0) {
                const int period = parse_int(text, len);
                if (period >= 1) game->period = (int8_t)period;
            } else if (strcmp(st->key, "state") == 0) {
                game->state = state_from_text(text, len);
                game->detailed = true;
            }
            return;
        }
        if (strcmp(container_key, "type") == 0) {
            if (strcmp(st->key, "state") == 0) {
                game->state = state_from_text(text, len);
                game->detailed = true;
            }
            return;
        }
    }

    // ---- 队名缩写(team{} 的成员)
    if (in_object && st->game >= 0 && strcmp(container_key, "team") == 0 &&
        strcmp(st->key, "abbreviation") == 0) {
        copy_text(st->cur.abbr, sizeof(st->cur.abbr), text, len);
        return;
    }

    (void)token;
}

static void on_token(void *ctx, nba_json_token_t token, const char *text, size_t len)
{
    nba_parse_t *st = (nba_parse_t *)ctx;
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
        copy_text(st->key, NBA_PARSE_KEY_LEN, text, len);
        st->has_key = true;
        break;
    case NBA_JSON_STRING:
    case NBA_JSON_NUMBER:
        on_value(st, token, text, len);
        break;
    case NBA_JSON_TRUE:
    case NBA_JSON_FALSE:
    case NBA_JSON_NULL:
        st->has_key = false;
        break;
    }
}

void nba_parse_begin(nba_parse_t *state, nba_board_t *board)
{
    memset(state, 0, sizeof(*state));
    state->board = board;
    state->game = -1;
    state->ok = true;
    state->kind[0] = '{';      // 根容器按对象处理,方便统一走 ckey[d] 判断
    state->ckey[0][0] = '\0';
    nba_board_reset(board);
    nba_json_init(&state->json, on_token, state);
}

void nba_parse_feed(nba_parse_t *state, const char *data, size_t len)
{
    if (!state->ok) return;
    nba_json_feed(&state->json, data, len);
    if (nba_json_failed(&state->json)) state->ok = false;
}

bool nba_parse_ok(const nba_parse_t *state)
{
    return state->ok && state->depth == 0;
}
