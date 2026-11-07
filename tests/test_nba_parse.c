// tests/test_nba_parse.c —— 比分 JSON 解析 + 文本格式化的主机测试。
//
// 关键行为:
//   1. 同一份响应按任意分片切分喂入,结果必须一致(流式解析不能依赖缓冲边界);
//   2. 只收集看板字段,links 等无关对象里的 abbreviation 不能污染结果;
//   3. 截断的响应必须报错而不是留下半场比赛。
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nba_model.h"
#include "nba_parse.h"

static char *load_fixture(size_t *out_len)
{
    const char *path = "tests/fixtures/nba_scoreboard.json";
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "cannot open %s (run from the repository root)\n", path);
        exit(2);
    }
    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    assert(size > 0);
    char *buf = malloc((size_t)size + 1);
    assert(buf);
    const size_t got = fread(buf, 1, (size_t)size, fp);
    fclose(fp);
    assert(got == (size_t)size);
    buf[size] = '\0';
    *out_len = (size_t)size;
    return buf;
}

static void parse_all(nba_board_t *board, const char *data, size_t len, size_t chunk)
{
    nba_parse_t state;
    nba_parse_begin(&state, board);
    for (size_t off = 0; off < len; off += chunk) {
        const size_t n = (len - off < chunk) ? (len - off) : chunk;
        nba_parse_feed(&state, data + off, n);
    }
    assert(nba_parse_ok(&state));
}

static void check_board(const nba_board_t *board)
{
    assert(board->count == 3);

    const nba_game_t *g = &board->games[0];
    assert(strcmp(g->id, "401810055") == 0 || g->id[0] != '\0');
    assert(strcmp(g->away.abbr, "CHA") == 0);
    assert(strcmp(g->home.abbr, "WSH") == 0);
    assert(g->away.score == 79);
    assert(g->home.score == 95);
    assert(strcmp(g->away.record, "3-3") == 0);
    assert(g->state == NBA_STATE_POST);
    assert(g->period == 4);
    assert(g->away.quarters[0] == 26);
    assert(g->away.quarters[1] == 20);
    assert(g->away.quarters[2] == 13);
    assert(g->away.quarters[3] == 20);
    assert(g->home.quarters[0] == 22);
    assert(g->home.quarters[3] == 16);
    assert(strcmp(g->tipoff, "2026-10-30T23:00Z") == 0);

    // 第二场的比分方向必须和第一场独立:不能因为 home/away 顺序变了就串味。
    assert(strcmp(board->games[1].away.abbr, "CHI") == 0);
    assert(strcmp(board->games[1].home.abbr, "BOS") == 0);
    assert(board->games[1].away.score == 111);
    assert(board->games[1].home.score == 125);
    assert(strcmp(board->games[2].home.record, "0-5") == 0);

    // links[].team.abbreviation 是干扰项,必须是 CHA/WSH 而不是 XXX。
    for (uint8_t i = 0; i < board->count; i++) {
        assert(strcmp(board->games[i].away.abbr, "XXX") != 0);
        assert(strcmp(board->games[i].home.abbr, "XXX") != 0);
    }
}

static void test_parser_chunking(void)
{
    size_t len = 0;
    char *data = load_fixture(&len);

    nba_board_t reference;
    parse_all(&reference, data, len, len);
    check_board(&reference);

    const size_t chunks[] = {1, 2, 3, 5, 7, 11, 64, 511, 4096};
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        nba_board_t board;
        parse_all(&board, data, len, chunks[i]);
        assert(board.count == reference.count);
        assert(memcmp(&board, &reference, sizeof(board)) == 0);
    }

    // 截断:响应在比赛中途断掉,必须判失败,不能当成功。
    nba_parse_t state;
    nba_board_t truncated;
    nba_parse_begin(&state, &truncated);
    nba_parse_feed(&state, data, len / 2);
    assert(!nba_parse_ok(&state));

    free(data);
}

static void test_empty_events(void)
{
    const char json[] = "{\"leagues\":[],\"events\":[]}";
    nba_board_t board;
    nba_parse_t state;
    nba_parse_begin(&state, &board);
    nba_parse_feed(&state, json, sizeof(json) - 1);
    assert(nba_parse_ok(&state));
    assert(board.count == 0);
}

static void test_unknown_and_missing_fields(void)
{
    // 只给最小字段:比分缺失时保持 -1,状态缺失时是 UNKNOWN,不能瞎猜成 0:0。
    const char json[] =
        "{\"events\":[{\"id\":\"1\",\"date\":\"2026-11-01T00:30Z\","
        "\"competitions\":[{\"competitors\":["
        "{\"homeAway\":\"home\",\"team\":{\"abbreviation\":\"LAL\"}},"
        "{\"homeAway\":\"away\",\"team\":{\"abbreviation\":\"BOS\"}}],"
        "\"status\":{\"displayClock\":\"0.0\",\"period\":0,"
        "\"type\":{\"state\":\"pre\"}}}]}]}";
    nba_board_t board;
    nba_parse_t state;
    nba_parse_begin(&state, &board);
    nba_parse_feed(&state, json, sizeof(json) - 1);
    assert(nba_parse_ok(&state));
    assert(board.count == 1);
    assert(strcmp(board.games[0].home.abbr, "LAL") == 0);
    assert(strcmp(board.games[0].away.abbr, "BOS") == 0);
    assert(board.games[0].home.score == -1);
    assert(board.games[0].state == NBA_STATE_PRE);
    assert(board.games[0].period == -1);

    char buf[32];
    assert(nba_format_status(&board.games[0], "--:--", buf, sizeof(buf)) > 0);
    assert(strcmp(buf, "未开始") == 0);
}

static void test_formatting(void)
{
    char buf[32];

    nba_format_period(1, buf, sizeof(buf));
    assert(strcmp(buf, "第1节") == 0);
    nba_format_period(4, buf, sizeof(buf));
    assert(strcmp(buf, "第4节") == 0);
    nba_format_period(5, buf, sizeof(buf));
    assert(strcmp(buf, "加时") == 0);
    nba_format_period(6, buf, sizeof(buf));
    assert(strcmp(buf, "加时2") == 0);
    nba_format_period(0, buf, sizeof(buf));
    assert(buf[0] == '\0');

    nba_format_score(-1, buf, sizeof(buf));
    assert(strcmp(buf, "--") == 0);
    nba_format_score(0, buf, sizeof(buf));
    assert(strcmp(buf, "0") == 0);
    nba_format_score(128, buf, sizeof(buf));
    assert(strcmp(buf, "128") == 0);

    nba_game_t game;
    memset(&game, 0, sizeof(game));
    game.state = NBA_STATE_LIVE;
    game.period = 3;
    strcpy(game.clock, "5:24");
    nba_format_status(&game, NULL, buf, sizeof(buf));
    assert(strcmp(buf, "第3节 5:24") == 0);

    game.clock[0] = '\0';
    nba_format_status(&game, NULL, buf, sizeof(buf));
    assert(strcmp(buf, "第3节") == 0);

    game.state = NBA_STATE_POST;
    game.period = 4;
    nba_format_status(&game, NULL, buf, sizeof(buf));
    assert(strcmp(buf, "已结束") == 0);

    game.period = 5;
    nba_format_status(&game, NULL, buf, sizeof(buf));
    assert(strcmp(buf, "已结束 加时") == 0);

    game.state = NBA_STATE_PRE;
    game.period = 0;
    nba_format_status(&game, "07:30", buf, sizeof(buf));
    assert(strcmp(buf, "07:30") == 0);

    game.state = NBA_STATE_UNKNOWN;
    nba_format_status(&game, NULL, buf, sizeof(buf));
    assert(strcmp(buf, "--") == 0);

    // 开赛时间:非法/缺字段时给 "--:--",不崩、不显示 1970。
    assert(nba_format_tipoff_local(NULL, buf, sizeof(buf)) > 0);
    assert(strcmp(buf, "--:--") == 0);
    assert(nba_format_tipoff_local("not-a-date", buf, sizeof(buf)) > 0);
    assert(strcmp(buf, "--:--") == 0);
    nba_format_tipoff_local("2026-10-30T23:00Z", buf, sizeof(buf));
    assert(strlen(buf) == 5 && buf[2] == ':');
}

// 比赛日换算:看板按北京时间的日历日分组(与国内赛程表一致),所以 UTC 时间必须
// 换算到 UTC+8,跨日/跨月/跨年都要对。ESPN 接口分组用的是美东日期,两者差一天,
// 那一步在应用层做,不在这里。
static void test_day_math(void)
{
    nba_date_t date = {0};

    // 2026-10-31T19:00Z = 北京 11 月 1 日 03:00:这就是截图里"11-01 03:00 开拓者"那场
    assert(nba_date_from_iso_beijing("2026-10-31T19:00Z", &date));
    assert(date.year == 2026 && date.month == 11 && date.day == 1);

    // 2026-10-31T16:30Z = 北京 11 月 1 日 00:30:跨过 UTC 的日界,仍是同一天
    assert(nba_date_from_iso_beijing("2026-10-31T16:30Z", &date));
    assert(date.year == 2026 && date.month == 11 && date.day == 1);

    // 2026-10-31T15:30Z = 北京 10 月 31 日 23:30:这时还属于前一天
    assert(nba_date_from_iso_beijing("2026-10-31T15:30Z", &date));
    assert(date.month == 10 && date.day == 31);

    // 跨年:2026-12-31T16:30Z = 北京 2027-01-01 00:30
    assert(nba_date_from_iso_beijing("2026-12-31T16:30Z", &date));
    assert(date.year == 2027 && date.month == 1 && date.day == 1);

    // 跨月:2026-02-28T16:30Z = 北京 2026-03-01 00:30
    assert(nba_date_from_iso_beijing("2026-02-28T16:30Z", &date));
    assert(date.year == 2026 && date.month == 3 && date.day == 1);

    // 非法输入不编造日期
    assert(!nba_date_from_iso_beijing("", &date));
    assert(!nba_date_from_iso_beijing("2025-13-40T00:00Z", &date));
    assert(!nba_date_from_iso_beijing(NULL, &date));

    // 加减天数:跨月、跨年、闰年
    nba_date_t d = { .year = 2025, .month = 12, .day = 31 };
    assert(nba_date_add_days(&d, 1));
    assert(d.year == 2026 && d.month == 1 && d.day == 1);
    assert(nba_date_add_days(&d, -1));
    assert(d.year == 2025 && d.month == 12 && d.day == 31);
    d = (nba_date_t){ .year = 2024, .month = 2, .day = 28 };
    assert(nba_date_add_days(&d, 1));
    assert(d.month == 2 && d.day == 29);           // 2024 是闰年
    assert(nba_date_add_days(&d, 1));
    assert(d.month == 3 && d.day == 1);
    d = (nba_date_t){ .year = 2025, .month = 3, .day = 1 };
    assert(nba_date_add_days(&d, -1));
    assert(d.month == 2 && d.day == 28);           // 2025 不是闰年
    d = (nba_date_t){ .year = 0, .month = 0, .day = 0 };
    assert(!nba_date_add_days(&d, 1));             // 非法日期不改写

    // 文本格式
    char buf[16];
    const nba_date_t day = { .year = 2026, .month = 10, .day = 31 };
    assert(nba_format_day(&day, buf, sizeof(buf)) == 5);
    assert(strcmp(buf, "10-31") == 0);
    assert(nba_format_day_query(&day, buf, sizeof(buf)) == 8);
    assert(strcmp(buf, "20261031") == 0);
    const nba_date_t zero = {0};
    assert(nba_format_day(&zero, buf, sizeof(buf)) == 0);
    assert(strcmp(buf, "") == 0);
    assert(nba_format_day(NULL, buf, sizeof(buf)) == 0);

    // "设备今天"依赖真实时钟:主机上必然已对时,只断言结果落在合理范围。
    nba_date_t today;
    assert(nba_date_today_beijing(&today));
    assert(nba_date_valid(&today));
    assert(today.year >= 2025);
    assert(!nba_date_today_beijing(NULL));
}

// 按目标日期过滤:ESPN 的一个分组里可能混进相邻两天的场次,只留目标日。
static void test_board_keep_day(void)
{
    nba_board_t board;
    nba_board_reset(&board);
    const nba_date_t target = { .year = 2026, .month = 11, .day = 1 };

    board.count = 4;
    snprintf(board.games[0].tipoff, sizeof(board.games[0].tipoff), "%s",
             "2026-10-31T15:30Z");   // 北京 10-31 23:30,不属于目标日 -> 丢
    snprintf(board.games[1].tipoff, sizeof(board.games[1].tipoff), "%s",
             "2026-10-31T19:00Z");   // 北京 11-01 03:00 -> 留
    snprintf(board.games[2].tipoff, sizeof(board.games[2].tipoff), "%s",
             "2026-11-01T02:30Z");   // 北京 11-01 10:30 -> 留
    board.games[3].tipoff[0] = '\0';  // 开球时间缺失:无从判断,保留
    for (int i = 0; i < 4; i++) snprintf(board.games[i].id, sizeof(board.games[i].id), "%d", i);

    assert(nba_board_keep_day(&board, &target) == 3);
    assert(board.count == 3);
    assert(strcmp(board.games[0].id, "1") == 0);   // 前移后顺序保持
    assert(strcmp(board.games[1].id, "2") == 0);
    assert(strcmp(board.games[2].id, "3") == 0);
    assert(board.day.month == 11 && board.day.day == 1);

    // 全都不属于目标日:清空,且不谎报日期
    nba_board_reset(&board);
    board.count = 1;
    snprintf(board.games[0].tipoff, sizeof(board.games[0].tipoff), "%s", "2026-10-31T15:30Z");
    assert(nba_board_keep_day(&board, &target) == 0);
    assert(board.count == 0);

    // 非法输入不炸
    assert(nba_board_keep_day(NULL, &target) == 0);
    const nba_date_t bad = {0};
    board.count = 1;
    assert(nba_board_keep_day(&board, &bad) == 0);
    assert(board.count == 0);
}

// 接口不返回"这是哪一天":board->day 要从首场比赛的开球时间反推(北京日期)。
static void test_board_day_from_games(void)
{
    nba_board_t board;
    nba_board_reset(&board);
    assert(!nba_board_day_from_games(&board));      // 没有比赛:仍未知

    board.count = 2;
    snprintf(board.games[0].tipoff, sizeof(board.games[0].tipoff), "%s",
             "2026-10-31T19:00Z");                  // 北京 11-01 03:00
    assert(nba_board_day_from_games(&board));
    assert(board.day.month == 11 && board.day.day == 1);

    // 已经有合法日期时不覆盖(显式请求过的日期更可信)
    board.day = (nba_date_t){ .year = 2026, .month = 10, .day = 31 };
    assert(nba_board_day_from_games(&board));
    assert(board.day.month == 10 && board.day.day == 31);
}

static void test_deep_nesting(void)
{
    // 真实响应里 competitors[] 下面还有 leaders[] > leaders[] > athlete > team ...
    // 单个比赛日实测最深 15 层。收集器的容器栈早先只留了 12 层,真机上会把一份
    // 合法响应当成语法错误("解析失败(82027 字节)")。这里用一个 17 层的无关子树
    // 守住这一点:深但合法的响应必须解析成功,且比分不受影响。
    const char json[] =
        "{\"events\":[{\"id\":\"9\",\"date\":\"2026-11-01T00:30Z\","
        "\"competitions\":[{\"competitors\":["
        "{\"homeAway\":\"home\",\"score\":\"99\",\"team\":{\"abbreviation\":\"LAL\"},"
        "\"records\":[{\"summary\":\"1-0\"}],"
        "\"leaders\":[{\"name\":\"pointsPerGame\",\"leaders\":[{\"displayValue\":\"20.8\","
        "\"athlete\":{\"team\":{\"x\":{\"y\":{\"z\":[{\"deep\":1}]}}}}}]}]},"
        "{\"homeAway\":\"away\",\"score\":\"88\",\"team\":{\"abbreviation\":\"BOS\"}}]}]}]}";

    nba_board_t board;
    nba_parse_t state;
    nba_parse_begin(&state, &board);
    nba_parse_feed(&state, json, sizeof(json) - 1);
    assert(nba_parse_ok(&state));
    assert(board.count == 1);
    assert(strcmp(board.games[0].home.abbr, "LAL") == 0);
    assert(board.games[0].home.score == 99);
    assert(board.games[0].away.score == 88);

    // 超出分词器栈深(24 层)必须干净地判失败,而不是继续往下猜或在设备上崩掉。
    char deep[512];
    size_t pos = 0;
    pos += (size_t)snprintf(deep + pos, sizeof(deep) - pos, "%s", "{\"events\":");
    for (int i = 0; i < 30; i++) deep[pos++] = '[';
    for (int i = 0; i < 30; i++) deep[pos++] = ']';
    pos += (size_t)snprintf(deep + pos, sizeof(deep) - pos, "%s", "}");
    deep[pos] = '\0';

    nba_parse_begin(&state, &board);
    nba_parse_feed(&state, deep, pos);
    assert(!nba_parse_ok(&state));
}

// 分节数组的字段顺序不固定:测试样本是 period 在前,真机响应是 value 在前。
// 收集器若按 period 直接给游标赋值,value-first 的数据会整行左移一格:第一节被
// 第二节覆盖,Q4 永远是 -1,详情页那一列显示 "--"。
static void test_linescores_field_order(void)
{
    const char json[] =
        "{\"events\":[{\"id\":\"9\","
        "\"competitions\":[{\"competitors\":["
        "{\"homeAway\":\"away\",\"score\":\"112\",\"team\":{\"abbreviation\":\"POR\"},"
        "\"linescores\":[{\"value\":27.0,\"period\":1},{\"value\":32.0,\"period\":2},"
        "{\"value\":29.0,\"period\":3},{\"value\":24.0,\"period\":4}]},"
        "{\"homeAway\":\"home\",\"score\":\"90\",\"team\":{\"abbreviation\":\"UTAH\"},"
        "\"linescores\":[{\"value\":18.0,\"period\":1},{\"value\":24.0,\"period\":2},"
        "{\"value\":22.0,\"period\":3},{\"value\":26.0,\"period\":4}]}"
        "],\"status\":{\"period\":4,\"type\":{\"state\":\"post\"}}}]}]}";

    nba_board_t board;
    nba_parse_t state;
    nba_parse_begin(&state, &board);
    nba_parse_feed(&state, json, sizeof(json) - 1);
    assert(nba_parse_ok(&state));
    assert(board.count == 1);
    const nba_game_t *game = &board.games[0];
    assert(game->away.quarters[0] == 27);
    assert(game->away.quarters[1] == 32);
    assert(game->away.quarters[2] == 29);
    assert(game->away.quarters[3] == 24);
    assert(game->home.quarters[0] == 18);
    assert(game->home.quarters[3] == 26);

    // 加时:第 5 节没有列可放,只能丢弃,不能污染 Q4 或越界写入。
    const char overtime[] =
        "{\"events\":[{\"id\":\"10\",\"competitions\":[{\"competitors\":["
        "{\"homeAway\":\"away\",\"team\":{\"abbreviation\":\"BOS\"},"
        "\"linescores\":[{\"value\":20.0,\"period\":1},{\"value\":21.0,\"period\":2},"
        "{\"value\":22.0,\"period\":3},{\"value\":23.0,\"period\":4},"
        "{\"value\":9.0,\"period\":5}]}]}]}]}";
    nba_parse_begin(&state, &board);
    nba_parse_feed(&state, overtime, sizeof(overtime) - 1);
    assert(nba_parse_ok(&state));
    assert(board.games[0].away.quarters[3] == 23);
}

static void test_tipoff_epoch(void)
{
    // 刷新窗口靠它算"距开赛还有多久":必须与时区无关、坏输入要报错。
    time_t a = 0, b = 0;
    assert(nba_tipoff_epoch("1970-01-01T00:00Z", &a) && a == 0);
    assert(nba_tipoff_epoch("2020-01-01T00:00Z", &a) && a == 1577836800);
    assert(nba_tipoff_epoch("2026-11-05T00:00Z", &a));
    assert(nba_tipoff_epoch("2026-11-05T01:30Z", &b));
    assert(b - a == 5400);
    assert(!nba_tipoff_epoch("not-a-date", &a));
    assert(!nba_tipoff_epoch(NULL, &a));
}

int main(void)
{
    test_parser_chunking();
    test_empty_events();
    test_unknown_and_missing_fields();
    test_deep_nesting();
    test_linescores_field_order();
    test_formatting();
    test_tipoff_epoch();
    test_day_math();
    test_board_keep_day();
    test_board_day_from_games();
    printf("test_nba_parse: PASS\n");
    return 0;
}
