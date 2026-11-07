// tests/test_nba_box.c —— 单场球员数据(summary.boxscore)解析的主机测试。
//
// 关键行为:
//   1. 任意分片切分喂入,结果一致(流式解析不能依赖缓冲边界);
//   2. 字段顺序由 statistics.keys[] 决定,不能写死下标;
//   3. 读完 players[] 就置 complete(真机上据此提前断开,省掉 95% 的下载);
//   4. 未上场的人被剔除,名单按得分降序,且能按客队缩写把两队顺序对齐。
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nba_box.h"

static char *load_fixture(size_t *out_len)
{
    const char *path = "tests/fixtures/nba_summary_box.json";
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

static void parse_all(nba_box_t *box, const char *data, size_t len, size_t chunk)
{
    static nba_box_parse_t state;
    nba_box_parse_begin(&state, box);
    for (size_t off = 0; off < len; off += chunk) {
        const size_t n = (len - off < chunk) ? (len - off) : chunk;
        nba_box_parse_feed(&state, data + off, n);
    }
    assert(nba_box_parse_ok(&state));
}

static const nba_player_t *find_player(const nba_team_box_t *team, const char *name)
{
    for (uint8_t i = 0; i < team->count; i++) {
        if (strcmp(team->players[i].name, name) == 0) return &team->players[i];
    }
    return NULL;
}

static void check_box(const nba_box_t *box)
{
    assert(box->team_count == 2);
    // 接口给的是"主队在前"(UTAH 是主队),fix_order 之前不能假定顺序。
    assert(strcmp(box->teams[0].abbr, "POR") == 0 || strcmp(box->teams[1].abbr, "POR") == 0);

    const nba_team_box_t *away = NULL;
    const nba_team_box_t *home = NULL;
    for (uint8_t i = 0; i < box->team_count; i++) {
        if (strcmp(box->teams[i].abbr, "POR") == 0) away = &box->teams[i];
        if (strcmp(box->teams[i].abbr, "UTAH") == 0) home = &box->teams[i];
    }
    assert(away && home);

    // 数值必须落在正确的列上(keys[] 里 points/rebounds/assists 的下标)。
    const nba_player_t *camara = find_player(away, "T. Camara");
    assert(camara);
    assert(camara->pts == 14);
    assert(camara->reb == 4);
    assert(camara->ast == 0);
    assert(camara->minutes == 31);
    // 复合列("4-9" = 命中 4 / 出手 9)与正负值都要落对。
    assert(camara->fgm == 4 && camara->fga == 9);
    assert(camara->tpm == 4 && camara->tpa == 9);
    assert(camara->ftm == 2 && camara->fta == 2);
    assert(camara->stl == 2 && camara->blk == 0 && camara->tov == 1);
    assert(camara->pf == 3);
    assert(camara->plus_minus == 26);

    const nba_player_t *markkanen = find_player(home, "L. Markkanen");
    assert(markkanen);
    assert(markkanen->pts == 13);
    assert(markkanen->reb == 6);
    assert(markkanen->ast == 1);
    // 负的正负值不能被读成 "+17"。
    assert(markkanen->plus_minus == -17);
    assert(markkanen->fgm == 3 && markkanen->fga == 14);
    assert(markkanen->tpm == 2 && markkanen->tpa == 11);
    assert(markkanen->ftm == 5 && markkanen->fta == 6);

    // 未上场的人不该出现(UTAH 的 T. Bradley 是 didNotPlay)。
    for (uint8_t t = 0; t < box->team_count; t++) {
        assert(find_player(&box->teams[t], "T. Bradley") == NULL);
        assert(box->teams[t].count > 0);
    }

    // 名字里不能留下非 ASCII:montserrat 没有重音字形,留着会显示成方框。
    for (uint8_t t = 0; t < box->team_count; t++) {
        for (uint8_t i = 0; i < box->teams[t].count; i++) {
            const char *name = box->teams[t].players[i].name;
            assert(name[0] != '\0');
            for (const char *p = name; *p; p++) {
                assert((unsigned char)*p < 0x7F);
            }
        }
    }
}

static void test_chunking(void)
{
    size_t len = 0;
    char *data = load_fixture(&len);

    nba_box_t reference;
    parse_all(&reference, data, len, len);
    check_box(&reference);

    const size_t chunks[] = {1, 2, 3, 5, 7, 11, 64, 511, 4096};
    for (size_t i = 0; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        nba_box_t box;
        parse_all(&box, data, len, chunks[i]);
        assert(box.team_count == reference.team_count);
        assert(box.team_count == reference.team_count);
        assert(memcmp(&box, &reference, sizeof(box)) == 0);
    }

    free(data);
}

// 真机上不会读完整个 450KB:两队名单到齐就该收工。这里拼一个"boxscore 在前、
// 后面跟一大段无关数组"的响应(真实响应的 plays / winprobability 就是这样),
// 验证读完名单就置 complete,用掉的字节数远小于整份响应。
static void test_early_complete(void)
{
    static char payload[128 * 1024];
    size_t pos = 0;
    const char head[] =
        "{\"boxscore\":{\"players\":[{\"team\":{\"abbreviation\":\"BOS\"},"
        "\"statistics\":[{\"keys\":[\"minutes\",\"points\",\"rebounds\",\"assists\"],"
        "\"athletes\":[{\"athlete\":{\"shortName\":\"J. Tatum\"},"
        "\"stats\":[\"38\",\"31\",\"9\",\"5\"]}]}]}]},\"plays\":[";
    memcpy(payload, head, sizeof(head) - 1);
    pos = sizeof(head) - 1;
    while (pos + 3 < sizeof(payload)) {
        payload[pos++] = '0';
        payload[pos++] = ',';
    }
    payload[pos - 1] = ']';
    payload[pos++] = '}';
    const size_t len = pos;

    nba_box_parse_t state;
    nba_box_t box;
    nba_box_parse_begin(&state, &box);
    assert(!nba_box_parse_complete(&state));

    size_t used = 0;
    for (size_t off = 0; off < len; off += 512) {
        const size_t n = (len - off < 512) ? (len - off) : 512;
        nba_box_parse_feed(&state, payload + off, n);
        used += n;
        if (nba_box_parse_complete(&state)) break;
    }
    assert(nba_box_parse_complete(&state));
    assert(used * 4 < len);            // 只用了不到 1/4 的响应
    assert(nba_box_parse_ok(&state));
    assert(nba_box_fix_order(&box, NULL));
    assert(box.teams[0].players[0].pts == 31);

    // complete 之后继续喂数据不再改变结果(真实链路里此时已经断开连接)。
    nba_box_parse_feed(&state, payload, len);
    assert(box.team_count == 1);
    assert(box.teams[0].count == 1);
}

static void test_fix_order_and_sort(void)
{
    size_t len = 0;
    char *data = load_fixture(&len);
    nba_box_t box;
    parse_all(&box, data, len, 777);

    assert(nba_box_fix_order(&box, "POR"));
    assert(box.ready);
    assert(strcmp(box.teams[0].abbr, "POR") == 0);    // 客队在前
    assert(strcmp(box.teams[1].abbr, "UTAH") == 0);

    for (uint8_t t = 0; t < box.team_count; t++) {
        const nba_team_box_t *team = &box.teams[t];
        assert(team->count > 0 && team->count <= NBA_BOX_SHOW);
        for (uint8_t i = 1; i < team->count; i++) {
            assert(team->players[i - 1].pts >= team->players[i].pts);   // 得分降序
        }
    }
    // POR 的头号得分手是 D. Avdija(24 分)。
    assert(strcmp(box.teams[0].players[0].name, "D. Avdija") == 0);
    assert(box.teams[0].players[0].pts == 24);

    // 客队缩写对不上时保持现有顺序,不能瞎换。
    assert(nba_box_fix_order(&box, "XXX"));
    assert(strcmp(box.teams[0].abbr, "POR") == 0);
    // 换成"客队是 UTAH"再对一次:顺序要跟着换,且重复调用是幂等的。
    assert(nba_box_fix_order(&box, "UTAH"));
    assert(strcmp(box.teams[0].abbr, "UTAH") == 0);
    assert(nba_box_fix_order(&box, "UTAH"));
    assert(strcmp(box.teams[0].abbr, "UTAH") == 0);

    // 行数 = 队头 + 球员,空队不算。
    const uint8_t rows = nba_box_row_count(&box);
    assert(rows == 2 + box.teams[0].count + box.teams[1].count);
    assert(nba_box_row_count(NULL) == 0);

    free(data);
}

static void test_keys_order(void)
{
    // keys[] 的顺序不固定(不同比赛会多出 teamId 之类的列),所以字段必须按名字定位。
    const char json[] =
        "{\"boxscore\":{\"players\":[{\"team\":{\"abbreviation\":\"LAL\"},"
        "\"statistics\":[{\"keys\":[\"assists\",\"minutes\",\"points\",\"rebounds\"],"
        "\"athletes\":[{\"athlete\":{\"shortName\":\"L. James\"},\"stats\":[\"9\",\"35\",\"28\",\"7\"]},"
        "{\"athlete\":{\"shortName\":\"Bench Guy\"},\"didNotPlay\":true,"
        "\"stats\":[\"0\",\"0\",\"0\",\"0\"]}]}]}]}}";
    nba_box_parse_t state;
    nba_box_t box;
    nba_box_parse_begin(&state, &box);
    nba_box_parse_feed(&state, json, sizeof(json) - 1);
    assert(nba_box_parse_ok(&state));
    assert(nba_box_fix_order(&box, NULL));
    assert(box.teams[0].count == 1);
    const nba_player_t *lebron = &box.teams[0].players[0];
    assert(strcmp(lebron->name, "L. James") == 0);
    assert(lebron->pts == 28);
    assert(lebron->reb == 7);
    assert(lebron->ast == 9);
    assert(lebron->minutes == 35);
}

static void test_extra_columns(void)
{
    // 列顺序和列集合都不固定:这里把复合列、正负值、失误放在最前面,并且故意
    // 混进本应用不显示的列(offensiveRebounds / teamId)验证不会串位。
    const char json[] =
        "{\"boxscore\":{\"players\":[{\"team\":{\"abbreviation\":\"BOS\"},"
        "\"statistics\":[{\"keys\":[\"plusMinus\","
        "\"freeThrowsMade-freeThrowsAttempted\",\"teamId\",\"blocks\","
        "\"threePointFieldGoalsMade-threePointFieldGoalsAttempted\",\"turnovers\","
        "\"points\",\"steals\",\"fouls\"],"
        "\"athletes\":[{\"athlete\":{\"shortName\":\"J. Tatum\"},"
        "\"stats\":[\"-9\",\"7-8\",\"2\",\"3\",\"4-11\",\"2\",\"33\",\"1\",\"4\"]}]}]}]}}";
    nba_box_parse_t state;
    nba_box_t box;
    nba_box_parse_begin(&state, &box);
    nba_box_parse_feed(&state, json, sizeof(json) - 1);
    assert(nba_box_parse_ok(&state));
    assert(nba_box_fix_order(&box, NULL));
    assert(box.teams[0].count == 1);
    const nba_player_t *p = &box.teams[0].players[0];
    assert(strcmp(p->name, "J. Tatum") == 0);
    assert(p->pts == 33);
    assert(p->plus_minus == -9);
    assert(p->ftm == 7 && p->fta == 8);
    assert(p->tpm == 4 && p->tpa == 11);
    assert(p->blk == 3);
    assert(p->tov == 2);
    assert(p->stl == 1);
    assert(p->pf == 4);
    // 这份响应没有 minutes / rebounds / assists:保持"未知",不能凭空变成 0。
    assert(p->minutes == -1);
    assert(p->reb == 0 && p->ast == 0);
}

static void test_garbage(void)
{
    // 被截断的响应:分词器只是"还没读完",不会报语法错;真正拦住它的是 complete。
    nba_box_parse_t state;
    nba_box_t box;
    nba_box_parse_begin(&state, &box);
    const char truncated[] = "{\"boxscore\":{\"players\":[{\"team\":{";
    nba_box_parse_feed(&state, truncated, sizeof(truncated) - 1);
    assert(!nba_box_parse_complete(&state));

    // 真正的语法错误(层级对不上)必须判失败,不能带着半份名单宣告成功。
    nba_box_parse_begin(&state, &box);
    const char broken[] = "}}}";
    nba_box_parse_feed(&state, broken, sizeof(broken) - 1);
    assert(!state.ok);
    assert(!nba_box_parse_ok(&state));

    // 没有 boxscore 的响应:不崩,ready 保持 false。
    nba_box_parse_begin(&state, &box);
    const char empty[] = "{\"plays\":[]}";
    nba_box_parse_feed(&state, empty, sizeof(empty) - 1);
    assert(!nba_box_parse_ok(&state));
    assert(!box.ready);
    assert(nba_box_row_count(&box) == 0);
}

static void test_name_order(void)
{
    // ESPN 的 shortName 本来就是"名缩写在前、姓在后",解析层必须原样保留:
    // 不许为了塞进窄列就把姓挪到前面("Gilgeous-Alexander S.")。带后缀的
    // "R. Williams III" 和普通双词名也不能被改动。
    const char json[] =
        "{\"boxscore\":{\"players\":[{\"team\":{\"abbreviation\":\"OKC\"},"
        "\"statistics\":[{\"keys\":[\"points\"],"
        "\"athletes\":["
        "{\"athlete\":{\"shortName\":\"S. Gilgeous-Alexander\"},\"stats\":[\"30\"]},"
        "{\"athlete\":{\"shortName\":\"R. Williams III\"},\"stats\":[\"8\"]},"
        "{\"athlete\":{\"shortName\":\"Bench Guy\"},\"stats\":[\"0\"]}"
        "]}]}]}}";
    nba_box_parse_t state;
    nba_box_t box;
    nba_box_parse_begin(&state, &box);
    nba_box_parse_feed(&state, json, sizeof(json) - 1);
    assert(nba_box_parse_ok(&state));

    // 在 team_finish 之前检查:它会把 0 分未上场的球员从名单里剔掉。
    bool long_ok = false, suffix_ok = false, plain_ok = false;
    for (uint8_t i = 0; i < box.teams[0].count; i++) {
        const char *n = box.teams[0].players[i].name;
        if (strcmp(n, "S. Gilgeous-Alexander") == 0) long_ok = true;
        if (strcmp(n, "R. Williams III") == 0) suffix_ok = true;
        if (strcmp(n, "Bench Guy") == 0) plain_ok = true;
    }
    assert(long_ok && suffix_ok && plain_ok);
    assert(nba_box_fix_order(&box, NULL));
    assert(strcmp(box.teams[0].players[0].name, "S. Gilgeous-Alexander") == 0);
}

int main(void)
{
    test_chunking();
    test_early_complete();
    test_fix_order_and_sort();
    test_keys_order();
    test_extra_columns();
    test_name_order();
    test_garbage();
    printf("test_nba_box: PASS\n");
    return 0;
}
