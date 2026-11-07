// main/nba_ui.c —— NBA 看板的页面实现。
//
// 设计:深色"球馆记分牌"风格。左侧球队色条 + 缩写 + 大号比分,右侧状态;
// 详情页给出两队的节次分栏。所有控件由本文件创建,与基线 demo 的 ui_pixel
// 主题无共用代码(仅复用 LVGL 原生控件)。
//
// 线程约定:全部函数都由持有 bsp_lvgl_lock() 的调用方进入。
#include "nba_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "lvgl.h"

static const char *TAG = "nba_ui";

// 中文字形来自 assets/fonts/app_font_cjk_16.c:用 lv_font_conv 从 Noto Sans SC 生成的
// 16px 子集,只含本应用真正显示的 83 个非 ASCII 字符。不用 LVGL 内置的 CJK 子集——
// 实测它缺"赛/板/详/情/确/选/择"等本应用必用的字,会直接显示成方框。
// 生成命令与字符清单见 assets/README.md;tools/check_font_coverage.py 负责守住覆盖。
LV_FONT_DECLARE(app_font_cjk_16);
#define FONT_CJK (&app_font_cjk_16)

#define COLOR_BG          0x070A10
#define COLOR_CARD        0x121A25
#define COLOR_CARD_SEL    0x1B2C46
#define COLOR_STROKE      0x243244
#define COLOR_TEXT        0xF2F6FB
#define COLOR_MUTED       0x8FA0B5
#define COLOR_LIVE        0xFF5A4D
#define COLOR_BATTERY     0x6BE07A
#define COLOR_DAY_AWAY    0xF2A03B   // 翻到非"今天"时,日期用琥珀色区分

#define LIST_ROWS 4
#define ROW_H 54
#define ROW_GAP 4
#define ROW_TOP 54

typedef struct {
    lv_obj_t *card;
    lv_obj_t *accent;
    lv_obj_t *away_abbr;
    lv_obj_t *away_score;
    lv_obj_t *home_abbr;
    lv_obj_t *home_score;
    lv_obj_t *status;
    lv_obj_t *status_sub;
} row_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *net;
    lv_obj_t *day;
    lv_obj_t *battery_label;
    lv_obj_t *battery_body;
    lv_obj_t *battery_fill;
    lv_obj_t *empty;
    lv_obj_t *hint;        // 底部按键提示;文案随"有没有凭据"变,所以是成员
    row_t rows[LIST_ROWS];
} overview_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *status;
    lv_obj_t *net;
    lv_obj_t *day;
    lv_obj_t *battery_label;
    lv_obj_t *battery_fill;
    lv_obj_t *away_tag;
    lv_obj_t *home_tag;
    lv_obj_t *away_abbr;
    lv_obj_t *away_score;
    lv_obj_t *away_record;
    lv_obj_t *home_abbr;
    lv_obj_t *home_score;
    lv_obj_t *home_record;
    lv_obj_t *away_quarters[NBA_QUARTERS + 1];
    lv_obj_t *home_quarters[NBA_QUARTERS + 1];
    lv_obj_t *quarter_head[NBA_QUARTERS + 1];
} detail_t;

static overview_t s_overview;
static detail_t s_detail;
static lv_obj_t *s_loaded;   // 当前已加载的屏幕,避免重复 lv_screen_load

// ---- 小工具 -------------------------------------------------------------

// LVGL 内置堆不会自动扩容(expand=0)。对象建到一半堆耗尽时 lv_obj_create /
// lv_label_create 返回 NULL,再往下调任何 lv_* 都会踩到 LVGL 断言里的 while(1),
// 外部只能看到看门狗超时、完全看不出原因。这里统一拦截并打印一次日志。
static bool s_alloc_warned;

static bool alloc_failed(lv_obj_t *obj, const char *what)
{
    if (obj) return false;
    if (!s_alloc_warned) {
        s_alloc_warned = true;
        ESP_LOGE(TAG, "%s 创建失败:LVGL 堆不足,请调大 CONFIG_LV_MEM_SIZE_KILOBYTES", what);
    }
    return true;
}

static lv_obj_t *make_box(lv_obj_t *parent, int x, int y, int w, int h,
                          uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (alloc_failed(obj, "面板")) return NULL;
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (alloc_failed(label, "文本")) return NULL;
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    return label;
}

static void place(lv_obj_t *label, int x, int y, int w, lv_text_align_t align)
{
    if (!label) return;
    lv_obj_set_pos(label, x, y);
    if (w > 0) {
        lv_obj_set_width(label, w);
        lv_obj_set_style_text_align(label, align, 0);
    }
}

// 30 支球队的主色。未知缩写(季前赛/全明星/数据异常)统一灰色,不做猜测。
static uint32_t team_color(const char *abbr)
{
    static const struct { const char *abbr; uint32_t color; } kColors[] = {
        {"ATL", 0xE03A3E}, {"BOS", 0x00A758}, {"BKN", 0xC9CDD4}, {"CHA", 0x2C5697},
        {"CHI", 0xCE1141}, {"CLE", 0xFDBB30}, {"DAL", 0x3B7BC4}, {"DEN", 0xEDB53A},
        {"DET", 0xC8102E}, {"GS",  0x2F7DE1}, {"GSW", 0x2F7DE1}, {"HOU", 0xCE1141},
        {"IND", 0xFDBB30}, {"LAC", 0xC8102E}, {"LAL", 0xFDB927}, {"MEM", 0x5D76A9},
        {"MIA", 0xE12A43}, {"MIL", 0x3C8C6E}, {"MIN", 0x7EC7E8}, {"NO",  0x8C734B},
        {"NOP", 0x8C734B}, {"NY",  0xF58427}, {"NYK", 0xF58427}, {"OKC", 0x3A8FD4},
        {"ORL", 0x3C77C7}, {"PHI", 0x2C6BB3}, {"PHX", 0xE56020}, {"POR", 0xD6393F},
        {"SAC", 0x8A6BD1}, {"SA",  0xB8C4CC}, {"SAS", 0xB8C4CC}, {"TOR", 0xD7263D},
        {"UTA", 0xF7A22B}, {"UTAH", 0xF7A22B}, {"WSH", 0xE31837}, {"WAS", 0xE31837},
    };
    for (size_t i = 0; i < sizeof(kColors) / sizeof(kColors[0]); i++) {
        if (strcmp(abbr, kColors[i].abbr) == 0) return kColors[i].color;
    }
    return 0x5A6B80;
}

static void battery_paint(lv_obj_t *fill, lv_obj_t *label, int soc)
{
    if (!fill) return;
    if (soc < 0) {
        // 读不到电量(电量计未应答):只画空壳,不显示数字,避免给出假读数。
        lv_obj_set_width(fill, 0);
        if (label) lv_label_set_text(label, "");
        return;
    }
    if (soc > 100) soc = 100;
    lv_obj_set_width(fill, (soc * 18) / 100);
    if (label) {
        char text[6];
        snprintf(text, sizeof(text), "%d", soc);
        lv_label_set_text(label, text);
    }
    lv_obj_set_style_bg_color(fill, lv_color_hex(soc <= 15 ? COLOR_LIVE : COLOR_BATTERY), 0);
}

static void battery_build(lv_obj_t *parent, lv_obj_t **label_out, lv_obj_t **fill_out)
{
    lv_obj_t *shell = make_box(parent, 196, 12, 26, 12, COLOR_CARD, 3);
    lv_obj_set_style_border_width(shell, 1, 0);
    lv_obj_set_style_border_color(shell, lv_color_hex(COLOR_STROKE), 0);
    *fill_out = make_box(shell, 1, 1, 18, 10, COLOR_BATTERY, 2);
    make_box(parent, 222, 15, 2, 6, COLOR_STROKE, 1);
    lv_obj_t *label = make_label(parent, "", &lv_font_montserrat_14, COLOR_MUTED);
    place(label, 168, 10, 26, LV_TEXT_ALIGN_RIGHT);
    *label_out = label;
}

// ---- 总览页 -------------------------------------------------------------

static void overview_build(void)
{
    s_overview.screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_overview.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_overview.screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_border_width(s_overview.screen, 0, 0);
    lv_obj_set_style_pad_all(s_overview.screen, 0, 0);
    lv_obj_t *scr = s_overview.screen;

    lv_obj_t *title = make_label(scr, "NBA 看板", FONT_CJK,
                                 COLOR_TEXT);
    place(title, 12, 10, 0, LV_TEXT_ALIGN_LEFT);
    // 比赛日贴在标题右侧(电量指示之前):翻到别的日子时换琥珀色,一眼可辨。
    s_overview.day = make_label(scr, "", &lv_font_montserrat_14, COLOR_MUTED);
    place(s_overview.day, 84, 12, 80, LV_TEXT_ALIGN_RIGHT);
    s_overview.net = make_label(scr, "", FONT_CJK, COLOR_MUTED);
    place(s_overview.net, 12, 32, 150, LV_TEXT_ALIGN_LEFT);

    battery_build(scr, &s_overview.battery_label, &s_overview.battery_fill);

    for (int i = 0; i < LIST_ROWS; i++) {
        row_t *row = &s_overview.rows[i];
        const int y = ROW_TOP + i * (ROW_H + ROW_GAP);
        row->card = make_box(scr, 8, y, 224, ROW_H, COLOR_CARD, 10);
        row->accent = make_box(row->card, 0, 8, 4, ROW_H - 16, COLOR_MUTED, 2);

        row->away_abbr = make_label(row->card, "", &lv_font_montserrat_20, COLOR_TEXT);
        place(row->away_abbr, 14, 2, 60, LV_TEXT_ALIGN_LEFT);
        row->away_score = make_label(row->card, "", &lv_font_montserrat_28, COLOR_TEXT);
        place(row->away_score, 76, 0, 60, LV_TEXT_ALIGN_RIGHT);

        row->home_abbr = make_label(row->card, "", &lv_font_montserrat_20, COLOR_TEXT);
        place(row->home_abbr, 14, 26, 60, LV_TEXT_ALIGN_LEFT);
        row->home_score = make_label(row->card, "", &lv_font_montserrat_28, COLOR_TEXT);
        place(row->home_score, 76, 24, 60, LV_TEXT_ALIGN_RIGHT);

        row->status = make_label(row->card, "", FONT_CJK,
                                 COLOR_MUTED);
        place(row->status, 142, 6, 74, LV_TEXT_ALIGN_RIGHT);
        row->status_sub = make_label(row->card, "", &lv_font_montserrat_14, COLOR_MUTED);
        place(row->status_sub, 142, 28, 74, LV_TEXT_ALIGN_RIGHT);
    }

    s_overview.empty = make_label(scr, "无比赛", FONT_CJK,
                                  COLOR_MUTED);
    lv_obj_set_pos(s_overview.empty, 0, 160);
    lv_obj_set_width(s_overview.empty, 240);
    lv_obj_set_style_text_align(s_overview.empty, LV_TEXT_ALIGN_CENTER, 0);

    // 提示只讲"看不见的功能":上下选择、确定进详情是列表的通行手势,不值得占掉
    // 这唯一一行 224px;长按两件事(刷新、换日)才是没人猜得到的。
    s_overview.hint = make_label(scr, "", FONT_CJK, COLOR_MUTED);
    place(s_overview.hint, 4, 296, 232, LV_TEXT_ALIGN_CENTER);
}

static void overview_render(const nba_ui_state_t *state, const nba_board_t *board)
{
    lv_label_set_text(s_overview.net, state->net_text ? state->net_text : "");
    lv_label_set_text(s_overview.day, state->day_text ? state->day_text : "");
    lv_obj_set_style_text_color(s_overview.day,
        lv_color_hex(state->day_away ? COLOR_DAY_AWAY : COLOR_MUTED), 0);
    battery_paint(s_overview.battery_fill, s_overview.battery_label, state->battery);

    // 没凭据时长按确定不是刷新,而是重新打开热点:这两种情况下同一个键做两件
    // 完全不同的事,提示必须跟着换,否则"长按确定刷新"就是一句假话。
    lv_label_set_text(s_overview.hint, state->need_setup
                          ? "长按确定配网"
                          : "长按确定刷新·长按上下换日");

    const int first = nba_list_first(state->selected, board->count, LIST_ROWS);
    if (board->count == 0) {
        lv_obj_remove_flag(s_overview.empty, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_overview.empty, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < LIST_ROWS; i++) {
        row_t *row = &s_overview.rows[i];
        const int index = first + i;
        if (index >= board->count) {
            lv_obj_add_flag(row->card, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(row->card, LV_OBJ_FLAG_HIDDEN);

        const nba_game_t *game = &board->games[index];
        const bool selected = (index == state->selected);
        lv_obj_set_style_bg_color(row->card,
            lv_color_hex(selected ? COLOR_CARD_SEL : COLOR_CARD), 0);
        lv_obj_set_style_border_width(row->card, selected ? 1 : 0, 0);
        lv_obj_set_style_border_color(row->card, lv_color_hex(COLOR_LIVE), 0);

        char tipoff[8];
        nba_format_tipoff_local(game->tipoff, tipoff, sizeof(tipoff));

        lv_label_set_text(row->away_abbr, game->away.abbr);
        lv_label_set_text(row->home_abbr, game->home.abbr);
        lv_obj_set_style_text_color(row->away_abbr,
            lv_color_hex(team_color(game->away.abbr)), 0);
        lv_obj_set_style_text_color(row->home_abbr,
            lv_color_hex(team_color(game->home.abbr)), 0);

        char score[8];
        nba_format_score(game->away.score, score, sizeof(score));
        lv_label_set_text(row->away_score, score);
        nba_format_score(game->home.score, score, sizeof(score));
        lv_label_set_text(row->home_score, score);

        // 右列:主句放节次/已结束,副句放剩余时间或开赛时间。
        char period[16];
        const char *main_text = "";
        const char *sub_text = "";
        char sub_buf[16];
        sub_buf[0] = '\0';
        switch (game->state) {
        case NBA_STATE_LIVE:
            nba_format_period(game->period, period, sizeof(period));
            main_text = period;
            if (game->clock[0]) {
                snprintf(sub_buf, sizeof(sub_buf), "%s", game->clock);
                sub_text = sub_buf;
            }
            break;
        case NBA_STATE_POST:
            main_text = "已结束";
            if (game->period > NBA_QUARTERS) sub_text = "加时";
            break;
        case NBA_STATE_PRE:
            main_text = "未开始";
            if (strcmp(tipoff, "--:--") != 0) sub_text = tipoff;
            break;
        default:
            main_text = "--";
            break;
        }
        lv_label_set_text(row->status, main_text);
        lv_label_set_text(row->status_sub, sub_text);
        lv_obj_set_style_text_color(row->status,
            lv_color_hex(game->state == NBA_STATE_LIVE ? COLOR_LIVE : COLOR_MUTED), 0);

        lv_obj_set_style_bg_color(row->accent,
            lv_color_hex(game->state == NBA_STATE_LIVE ? COLOR_LIVE
                                                       : team_color(game->home.abbr)), 0);
    }
}

// ---- 详情页 -------------------------------------------------------------

// 前四列各 28px:单节得分最多两位数(montserrat_20 下 "88" 实测 25.8px)。
// 最后一列是三位数总分,"108" 实测 33.6px,原先统一 32px 会被 LVGL 按字符
// 折行,第二行溢出 20px 行高被裁掉 —— 真机表现就是"Q1~Q4 都在,总分不见了"。
static const int kQuarterX[NBA_QUARTERS + 1] = {62, 94, 126, 158, 193};
#define QUARTER_COL_W 28
#define TOTAL_COL_X 174
#define TOTAL_COL_W 40

static void detail_build(void)
{
    s_detail.screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_detail.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_detail.screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_border_width(s_detail.screen, 0, 0);
    lv_obj_set_style_pad_all(s_detail.screen, 0, 0);
    lv_obj_t *scr = s_detail.screen;

    s_detail.status = make_label(scr, "", FONT_CJK, COLOR_TEXT);
    place(s_detail.status, 10, 10, 150, LV_TEXT_ALIGN_LEFT);
    s_detail.net = make_label(scr, "", FONT_CJK, COLOR_MUTED);
    place(s_detail.net, 10, 32, 150, LV_TEXT_ALIGN_LEFT);
    s_detail.day = make_label(scr, "", FONT_CJK, COLOR_MUTED);
    place(s_detail.day, 10, 266, 220, LV_TEXT_ALIGN_LEFT);
    battery_build(scr, &s_detail.battery_label, &s_detail.battery_fill);

    // 客队(左)/主队(右)两栏
    s_detail.away_tag = make_label(scr, "客", FONT_CJK,
                                   COLOR_MUTED);
    place(s_detail.away_tag, 12, 58, 0, LV_TEXT_ALIGN_LEFT);
    s_detail.home_tag = make_label(scr, "主", FONT_CJK,
                                   COLOR_MUTED);
    place(s_detail.home_tag, 130, 58, 0, LV_TEXT_ALIGN_LEFT);

    s_detail.away_abbr = make_label(scr, "", &lv_font_montserrat_20, COLOR_TEXT);
    place(s_detail.away_abbr, 12, 78, 100, LV_TEXT_ALIGN_LEFT);
    s_detail.home_abbr = make_label(scr, "", &lv_font_montserrat_20, COLOR_TEXT);
    place(s_detail.home_abbr, 130, 78, 100, LV_TEXT_ALIGN_LEFT);

    s_detail.away_score = make_label(scr, "", &lv_font_montserrat_40, COLOR_TEXT);
    place(s_detail.away_score, 12, 100, 100, LV_TEXT_ALIGN_LEFT);
    s_detail.home_score = make_label(scr, "", &lv_font_montserrat_40, COLOR_TEXT);
    place(s_detail.home_score, 130, 100, 100, LV_TEXT_ALIGN_LEFT);

    s_detail.away_record = make_label(scr, "", &lv_font_montserrat_14, COLOR_MUTED);
    place(s_detail.away_record, 12, 146, 100, LV_TEXT_ALIGN_LEFT);
    s_detail.home_record = make_label(scr, "", &lv_font_montserrat_14, COLOR_MUTED);
    place(s_detail.home_record, 130, 146, 100, LV_TEXT_ALIGN_LEFT);

    lv_obj_t *divider = make_box(scr, 116, 100, 2, 32, COLOR_STROKE, 1);
    (void)divider;

    // 节次分栏
    static const char *kHead[NBA_QUARTERS + 1] = {"Q1", "Q2", "Q3", "Q4", "总"};
    for (int i = 0; i <= NBA_QUARTERS; i++) {
        // "总" 是中文:montserrat 里没有字形(占位符会画成方框),必须用中文字体。
        const bool total = (i == NBA_QUARTERS);
        s_detail.quarter_head[i] = make_label(scr, kHead[i],
                                              total ? FONT_CJK : &lv_font_montserrat_14,
                                              COLOR_MUTED);
        place(s_detail.quarter_head[i], total ? TOTAL_COL_X : kQuarterX[i] - 14, 178,
              total ? TOTAL_COL_W : QUARTER_COL_W,
              total ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER);
    }

    for (int i = 0; i <= NBA_QUARTERS; i++) {
        const bool total = (i == NBA_QUARTERS);
        const int x = total ? TOTAL_COL_X : kQuarterX[i] - 14;
        const int w = total ? TOTAL_COL_W : QUARTER_COL_W;
        const lv_text_align_t align = total ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER;
        s_detail.away_quarters[i] = make_label(scr, "", &lv_font_montserrat_20,
                                               COLOR_TEXT);
        place(s_detail.away_quarters[i], x, 198, w, align);
        // 数字一律不折行:意外超宽时宁可裁掉尾部,也不能把比分挪到第二行。
        lv_label_set_long_mode(s_detail.away_quarters[i], LV_LABEL_LONG_CLIP);
        s_detail.home_quarters[i] = make_label(scr, "", &lv_font_montserrat_20,
                                               COLOR_TEXT);
        place(s_detail.home_quarters[i], x, 228, w, align);
        lv_label_set_long_mode(s_detail.home_quarters[i], LV_LABEL_LONG_CLIP);
    }

    // 详情页的上下键不是"滚动",而是直接换一场比赛——不知道这一点的人会退回
    // 总览重新选,所以必须写出来。三段各 4 字,连间隔号共 14 字,正好占满 232px。
    lv_obj_t *hint = make_label(scr, "上下换场·确定球员·长按返回",
                                FONT_CJK, COLOR_MUTED);
    place(hint, 4, 296, 232, LV_TEXT_ALIGN_CENTER);
}

static void detail_render(const nba_ui_state_t *state, const nba_board_t *board)
{
    lv_label_set_text(s_detail.net, state->net_text ? state->net_text : "");
    battery_paint(s_detail.battery_fill, s_detail.battery_label, state->battery);

    // 详情页把比赛日写全("比赛日 10-31"):翻到历史比赛时,光看比分不知道是哪天。
    char day_line[24];
    if (state->day_text && state->day_text[0]) {
        snprintf(day_line, sizeof(day_line), "比赛日 %s", state->day_text);
    } else {
        day_line[0] = '\0';
    }
    lv_label_set_text(s_detail.day, day_line);
    lv_obj_set_style_text_color(s_detail.day,
        lv_color_hex(state->day_away ? COLOR_DAY_AWAY : COLOR_MUTED), 0);

    if (board->count == 0) {
        lv_label_set_text(s_detail.status, "无比赛");
        return;
    }
    const nba_game_t *game = &board->games[state->selected];

    char tipoff[8];
    nba_format_tipoff_local(game->tipoff, tipoff, sizeof(tipoff));
    char status_text[32];
    nba_format_status(game, tipoff, status_text, sizeof(status_text));
    lv_label_set_text(s_detail.status, status_text);
    lv_obj_set_style_text_color(s_detail.status,
        lv_color_hex(game->state == NBA_STATE_LIVE ? COLOR_LIVE : COLOR_TEXT), 0);

    lv_label_set_text(s_detail.away_abbr, game->away.abbr);
    lv_label_set_text(s_detail.home_abbr, game->home.abbr);
    lv_obj_set_style_text_color(s_detail.away_abbr,
        lv_color_hex(team_color(game->away.abbr)), 0);
    lv_obj_set_style_text_color(s_detail.home_abbr,
        lv_color_hex(team_color(game->home.abbr)), 0);

    char score[8];
    nba_format_score(game->away.score, score, sizeof(score));
    lv_label_set_text(s_detail.away_score, score);
    nba_format_score(game->home.score, score, sizeof(score));
    lv_label_set_text(s_detail.home_score, score);

    lv_label_set_text(s_detail.away_record, game->away.record);
    lv_label_set_text(s_detail.home_record, game->home.record);

    for (int i = 0; i <= NBA_QUARTERS; i++) {
        if (i == NBA_QUARTERS) {
            nba_format_score(game->away.score, score, sizeof(score));
            lv_label_set_text(s_detail.away_quarters[i], score);
            nba_format_score(game->home.score, score, sizeof(score));
            lv_label_set_text(s_detail.home_quarters[i], score);
            break;
        }
        // 有值就显示,没有就留空。不能拿 period 去猜"这一节打没打":进行中的比赛
        // period 已经是当前节,而该节可能还没得分;用 "-1 -> --" 又会把空节画成
        // 破折号(真机上 Q4 曾整列显示 "--")。
        char text[8];
        nba_format_score(game->away.quarters[i], text, sizeof(text));
        lv_label_set_text(s_detail.away_quarters[i],
                          game->away.quarters[i] >= 0 ? text : "");
        nba_format_score(game->home.quarters[i], text, sizeof(text));
        lv_label_set_text(s_detail.home_quarters[i],
                          game->home.quarters[i] >= 0 ? text : "");
    }
}

// ---- 球员数据页 ---------------------------------------------------------
//
// 一屏放不下 12 列数据,也放不下两队 20 个人,所以分两层:
//   * 左右分层:一次只看一支球队,长按上下在客/主之间切;
//   * 上下分层:名字列固定,右边 3 列数字整块滑走、下一组滑进来。换组由
//     确定键触发(一下滑一格),不自动轮播。
// 名字不参与动画——滑动的只有数据区,读的人不会丢掉"这行是谁"。

#define BOX_ROWS 8
// 行高 24 → 22、行卡 22 → 20:这一页是唯一放不下两行提示的页面(8 行名单本身
// 已经顶到 286),压 2px 才能在最底部腾出"换队/返回"那一行。行内文字跟着上移 1px。
#define BOX_ROW_H 22
#define BOX_TOP 96
#define BOX_COLS 3
// 球员名按 ESPN 原序显示(名缩写在前、姓在后),不能重排成"姓在前"。
// 名字必须是单行 + 省略号:行高只有 20px,一旦按单词折行,整块姓会被丢到第二行
// 裁掉,屏幕上只剩 "S."。
//
// 行卡 236px(屏幕 4..240)的分法:名字区 128 + 数据区 104 + 右侧 4px 边距。
// 投篮组会出现 "12-20" 这种"两位数-两位数"的复合值,14px 下实占 35px;原来
// 26px 的列宽会把它右对齐裁成 "2-20",等于报错数据。现在数值列给到 32px,
// 并让超宽文本自动降字号(见 fit_font):常规值 "+26"(约 25px)仍是 14px 大字,
// "10-18" 降到 12px(29px),极端值再降到 10px。列间距 4px,三列不挤成一团。
#define BOX_CARD_X 4
#define BOX_CARD_W 236
#define BOX_CLIP_X 128          // 数据区(相对行卡片)的起点
#define BOX_CLIP_W 104          // 数据区宽度,右侧留 4px
#define BOX_NAME_W 124          // 名字 label:BOX_CLIP_X - 4
#define BOX_COL_W 32            // 单个数值列宽
#define BOX_COL_PITCH 36        // 列间距 4px:BOX_COL_W + 2 * 36 = 104

// 四组数据列。表头用中文单字;投篮是 "4-9" 这种"命中-出手"的复合值。
static const char *const kBoxHeads[NBA_BOX_GROUPS][BOX_COLS] = {
    {"分", "板", "助"},
    {"投", "三", "罚"},
    {"断", "帽", "失"},
    {"时", "差", "犯"},
};

// 逐字量宽度,按 14 → 12 → 10 挑一个放得下的字号。姓名和数值共用这套:
// 长姓名要保住姓,投篮组的 "12-20" 要保住两头的两位数,裁掉哪边都是错数据。
// 不按字符数猜:连字符、后缀、宽窄字母的差别很大;lv_font_get_glyph_width
// 返回的正是 LVGL 渲染时用的整数像素宽,和屏幕上量到的结果一致。
static const lv_font_t *fit_font(const char *text, int limit)
{
    static const lv_font_t *const kFonts[] = {
        &lv_font_montserrat_14, &lv_font_montserrat_12, &lv_font_montserrat_10,
    };
    for (size_t f = 0; f < sizeof(kFonts) / sizeof(kFonts[0]); f++) {
        int width = 0;
        const unsigned char *p = (const unsigned char *)text;
        for (; *p; p++) {
            width += (int)lv_font_get_glyph_width(kFonts[f], *p, p[1]);
            if (width > limit) break;
        }
        if (!*p) return kFonts[f];
    }
    return &lv_font_montserrat_10;   // 极端值:交给 label 的省略/裁剪兜底
}

typedef struct {
    lv_obj_t *bar;           // 每行的底条:选中时高亮
    lv_obj_t *name;          // 球员名:固定在左边,不参与滑动
    lv_obj_t *clip;          // 数据区的裁剪窗口
    lv_obj_t *val[BOX_COLS]; // 分、板、助…
} box_row_t;

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *title;
    lv_obj_t *day;
    lv_obj_t *match;         // "POR 112 : 90 UTAH"
    lv_obj_t *battery_label;
    lv_obj_t *battery_fill;
    lv_obj_t *team_tag;      // "客队" / "主队"
    lv_obj_t *team_abbr;
    lv_obj_t *team_score;
    lv_obj_t *head_name;
    lv_obj_t *head_val[BOX_COLS];
    box_row_t rows[BOX_ROWS];
    lv_obj_t *empty;
} box_view_t;

static box_view_t s_box;

// 渲染时要用的数据快照。动画的完成回调没有 state 参数,只能从这里取。
static const nba_box_t *s_box_data;
static uint8_t s_box_team_drawn;
static uint8_t s_box_first;        // 当前首行在该队名单里的下标
static uint8_t s_box_group_drawn;  // 屏幕上这一组数字是哪一组
static int8_t s_box_pending = -1;  // 滑完之后要切到的组
static bool s_box_animating;

// 透明裁剪窗口:LVGL 默认把子对象裁在父对象内,所以滑出窗口的部分不会盖到名字上。
static lv_obj_t *make_clip(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (alloc_failed(obj, "数据块")) return NULL;
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static void box_build(void)
{
    s_box.screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_box.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_box.screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_border_width(s_box.screen, 0, 0);
    lv_obj_set_style_pad_all(s_box.screen, 0, 0);
    lv_obj_t *scr = s_box.screen;

    s_box.title = make_label(scr, "球员数据", FONT_CJK, COLOR_TEXT);
    place(s_box.title, 10, 8, 0, LV_TEXT_ALIGN_LEFT);
    // 比赛日贴在标题右边:球员数据同样属于某一天,翻到历史场次要能看出来。
    s_box.day = make_label(scr, "", &lv_font_montserrat_14, COLOR_MUTED);
    place(s_box.day, 76, 10, 88, LV_TEXT_ALIGN_RIGHT);
    battery_build(scr, &s_box.battery_label, &s_box.battery_fill);

    s_box.match = make_label(scr, "", &lv_font_montserrat_14, COLOR_MUTED);
    place(s_box.match, 10, 30, 220, LV_TEXT_ALIGN_LEFT);

    // 队头:客队/主队 + 缩写(球队色) + 全队总分。
    s_box.team_tag = make_label(scr, "", FONT_CJK, COLOR_MUTED);
    place(s_box.team_tag, 10, 56, 0, LV_TEXT_ALIGN_LEFT);
    s_box.team_abbr = make_label(scr, "", &lv_font_montserrat_20, COLOR_TEXT);
    place(s_box.team_abbr, 46, 50, 80, LV_TEXT_ALIGN_LEFT);
    s_box.team_score = make_label(scr, "", &lv_font_montserrat_20, COLOR_TEXT);
    place(s_box.team_score, 168, 50, 62, LV_TEXT_ALIGN_RIGHT);

    s_box.head_name = make_label(scr, "球员", FONT_CJK, COLOR_MUTED);
    place(s_box.head_name, 10, 78, 0, LV_TEXT_ALIGN_LEFT);
    for (int c = 0; c < BOX_COLS; c++) {
        s_box.head_val[c] = make_label(scr, "", FONT_CJK, COLOR_MUTED);
        place(s_box.head_val[c], BOX_CARD_X + BOX_CLIP_X + c * BOX_COL_PITCH, 78,
              BOX_COL_W, LV_TEXT_ALIGN_RIGHT);
    }

    for (int i = 0; i < BOX_ROWS; i++) {
        box_row_t *row = &s_box.rows[i];
        row->bar = make_box(scr, BOX_CARD_X, BOX_TOP + i * BOX_ROW_H, BOX_CARD_W,
                            BOX_ROW_H - 2, COLOR_CARD_SEL, 6);
        row->name = make_label(row->bar, "", &lv_font_montserrat_14, COLOR_TEXT);
        place(row->name, 4, 2, BOX_NAME_W, LV_TEXT_ALIGN_LEFT);
        // 超长名字用省略号收尾,绝不折行到第二行(否则姓会被整块裁掉)。
        lv_label_set_long_mode(row->name, LV_LABEL_LONG_DOT);
        row->clip = make_clip(row->bar, BOX_CLIP_X, 0, BOX_CLIP_W, BOX_ROW_H - 2);
        for (int c = 0; c < BOX_COLS; c++) {
            row->val[c] = make_label(row->clip, "", &lv_font_montserrat_14, COLOR_TEXT);
            place(row->val[c], c * BOX_COL_PITCH, 2, BOX_COL_W, LV_TEXT_ALIGN_RIGHT);
            // 数值绝不折行:行高只有 20px,第二行会被裁剪窗口吃掉。
            lv_label_set_long_mode(row->val[c], LV_LABEL_LONG_CLIP);
        }
    }

    s_box.empty = make_label(scr, "无数据", FONT_CJK, COLOR_MUTED);
    place(s_box.empty, 0, 176, 240, LV_TEXT_ALIGN_CENTER);

    // 这一页有三个"没人猜得到"的动作:确定不是进入而是换数据组、长按上下换队、
    // 长按确定才是返回。一行 224px 装不下(要 20 字),所以分两行:上行短按、
    // 下行长按,顺序固定,别的页面也用同一套读法。
    lv_obj_t *hint1 = make_label(scr, "上下选人·确定换组", FONT_CJK, COLOR_MUTED);
    place(hint1, 4, 278, 232, LV_TEXT_ALIGN_CENTER);
    lv_obj_t *hint2 = make_label(scr, "长按上下换队·长按确定返回", FONT_CJK, COLOR_MUTED);
    place(hint2, 4, 298, 232, LV_TEXT_ALIGN_CENTER);
}

// 一行 3 个数字(不含表头)。组换了以后靠这个函数重画,位置由动画负责。
static void box_stat_text(const nba_player_t *player, uint8_t group, uint8_t col,
                          char *buf, size_t cap)
{
    switch (group) {
    case 1: {
        const uint8_t made = (col == 0) ? player->fgm : (col == 1) ? player->tpm : player->ftm;
        const uint8_t att = (col == 0) ? player->fga : (col == 1) ? player->tpa : player->fta;
        snprintf(buf, cap, "%u-%u", (unsigned)made, (unsigned)att);
        break;
    }
    case 2:
        snprintf(buf, cap, "%d", (int)((col == 0) ? player->stl
                                     : (col == 1) ? player->blk : player->tov));
        break;
    case 3:
        if (col == 0) {
            if (player->minutes < 0) buf[0] = '\0';
            else snprintf(buf, cap, "%d", (int)player->minutes);
        } else if (col == 1) {
            // 正负值要带符号:"+26" 和 "26" 是两件事。
            if (player->plus_minus == 0) snprintf(buf, cap, "0");
            else snprintf(buf, cap, "%+d", (int)player->plus_minus);
        } else {
            snprintf(buf, cap, "%d", (int)player->pf);
        }
        break;
    case 0:
    default:
        snprintf(buf, cap, "%d", (int)((col == 0) ? player->pts
                                     : (col == 1) ? player->reb : player->ast));
        break;
    }
}

static void box_fill_numbers(void)
{
    for (int i = 0; i < BOX_ROWS; i++) {
        box_row_t *row = &s_box.rows[i];
        char buf[10];
        for (int c = 0; c < BOX_COLS; c++) {
            const int index = s_box_first + i;
            const nba_team_box_t *team = s_box_data ? &s_box_data->teams[s_box_team_drawn] : NULL;
            if (!team || index >= team->count) {
                if (row->val[c]) lv_label_set_text(row->val[c], "");
                continue;
            }
            box_stat_text(&team->players[index], s_box_group_drawn, (uint8_t)c,
                          buf, sizeof(buf));
            if (row->val[c]) {
                // 先定字号再换文字:复合值 "12-20" 在 14px 下会被列宽裁掉一截。
                lv_obj_set_style_text_font(row->val[c], fit_font(buf, BOX_COL_W), 0);
                lv_label_set_text(row->val[c], buf);
            }
        }
    }
}

static void box_shift(int dx)
{
    for (int i = 0; i < BOX_ROWS; i++) {
        for (int c = 0; c < BOX_COLS; c++) {
            if (s_box.rows[i].val[c]) {
                lv_obj_set_x(s_box.rows[i].val[c], c * BOX_COL_PITCH + dx);
            }
        }
    }
}

static void box_anim_exec(void *var, int32_t v)
{
    (void)var;
    box_shift((int)v);
}

static void box_anim_in_done(lv_anim_t *a)
{
    (void)a;
    s_box_animating = false;
    box_shift(0);
}

// 滑出 → 换文字 → 从右侧滑入。整块 3 列一起动,所以读起来是"翻页"而不是"跳字"。
static void box_anim_out_done(lv_anim_t *a);

static void box_anim_start(int from, int to, void (*done)(lv_anim_t *))
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, s_box.screen);
    lv_anim_set_exec_cb(&anim, box_anim_exec);
    lv_anim_set_values(&anim, from, to);
    lv_anim_set_duration(&anim, 170);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&anim, done);
    (void)lv_anim_start(&anim);
    s_box_animating = true;
}

static void box_anim_out_done(lv_anim_t *a)
{
    (void)a;
    if (s_box_pending >= 0) {
        s_box_group_drawn = (uint8_t)s_box_pending;
        s_box_pending = -1;
    }
    for (int c = 0; c < BOX_COLS; c++) {
        if (s_box.head_val[c]) {
            lv_label_set_text(s_box.head_val[c], kBoxHeads[s_box_group_drawn][c]);
        }
    }
    box_fill_numbers();
    box_shift(BOX_CLIP_W);
    box_anim_start(BOX_CLIP_W, 0, box_anim_in_done);
}

static void box_row_hide(box_row_t *row)
{
    if (row->bar) lv_obj_add_flag(row->bar, LV_OBJ_FLAG_HIDDEN);
}

static void box_set_label(lv_obj_t *label, const char *text)
{
    if (label) lv_label_set_text(label, text);
}


static void box_render(const nba_ui_state_t *state, const nba_board_t *board)
{
    battery_paint(s_box.battery_fill, s_box.battery_label, state->battery);

    const nba_game_t *game = (state->selected < board->count) ?
                             &board->games[state->selected] : NULL;

    // 顶部比分条:球员数据脱离比分就没有意义,始终把两队总分摆在上面。
    char line[48];
    if (game) {
        char away[8];
        char home[8];
        nba_format_score(game->away.score, away, sizeof(away));
        nba_format_score(game->home.score, home, sizeof(home));
        snprintf(line, sizeof(line), "%s  %s : %s  %s",
                 game->away.abbr, away, home, game->home.abbr);
    } else {
        line[0] = '\0';
    }
    lv_label_set_text(s_box.match, line);

    char day_line[16];
    if (state->day_text && state->day_text[0]) {
        snprintf(day_line, sizeof(day_line), "%s", state->day_text);
    } else {
        day_line[0] = '\0';
    }
    lv_label_set_text(s_box.day, day_line);
    lv_obj_set_style_text_color(s_box.day,
        lv_color_hex(state->day_away ? COLOR_DAY_AWAY : COLOR_MUTED), 0);

    const nba_box_t *box = state->box;
    const bool ready = (box && box->ready && box->team_count > 0);
    s_box_data = ready ? box : NULL;

    uint8_t team = state->box_team;
    if (ready && team >= box->team_count) team = 0;
    s_box_team_drawn = team;

    const char *abbr = "";
    int score = -1;
    if (ready) {
        abbr = box->teams[team].abbr;
        if (game) score = (team == 0) ? game->away.score : game->home.score;
    } else if (game) {
        abbr = (team == 0) ? game->away.abbr : game->home.abbr;
        score = (team == 0) ? game->away.score : game->home.score;
    }
    box_set_label(s_box.team_tag, team == 0 ? "客队" : "主队");
    box_set_label(s_box.team_abbr, abbr);
    lv_obj_set_style_text_color(s_box.team_abbr, lv_color_hex(team_color(abbr)), 0);
    char score_text[8];
    nba_format_score(score, score_text, sizeof(score_text));
    box_set_label(s_box.team_score, score == -1 ? "" : score_text);

    for (int i = 0; i < BOX_ROWS; i++) box_row_hide(&s_box.rows[i]);

    const uint8_t count = (ready && team < box->team_count) ? box->teams[team].count : 0;
    const bool have_rows = (count > 0);
    if (!have_rows) {
        lv_obj_add_flag(s_box.head_name, LV_OBJ_FLAG_HIDDEN);
        for (int c = 0; c < BOX_COLS; c++) {
            if (s_box.head_val[c]) lv_obj_add_flag(s_box.head_val[c], LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_flag(s_box.empty, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_box.empty, ready ? "无数据" : (game ? "刷新中" : "无数据"));
        return;
    }
    lv_obj_add_flag(s_box.empty, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_box.head_name, LV_OBJ_FLAG_HIDDEN);
    for (int c = 0; c < BOX_COLS; c++) {
        if (s_box.head_val[c]) lv_obj_remove_flag(s_box.head_val[c], LV_OBJ_FLAG_HIDDEN);
    }

    uint8_t cursor = state->box_cursor[team];
    if (cursor >= count) cursor = (uint8_t)(count - 1);
    s_box_first = (uint8_t)nba_list_first(cursor, count, BOX_ROWS);

    for (int i = 0; i < BOX_ROWS; i++) {
        const int index = s_box_first + i;
        if (index >= count) break;
        box_row_t *row = &s_box.rows[i];
        lv_obj_remove_flag(row->bar, LV_OBJ_FLAG_HIDDEN);
        const bool selected_row = (index == cursor);
        lv_obj_set_style_bg_opa(row->bar, selected_row ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row->bar, 0, 0);
        const char *player_name = box->teams[team].players[index].name;
        // 名字列 124px:普通名 14px,长姓降 12px / 10px 也要保住"名缩写在前后接姓"。
        lv_obj_set_style_text_font(row->name, fit_font(player_name, BOX_NAME_W), 0);
        box_set_label(row->name, player_name);
    }

    // 表头跟着当前组走:动画期间表头也得先停留在旧组,滑完再换。
    if (!s_box_animating) {
        for (int c = 0; c < BOX_COLS; c++) {
            box_set_label(s_box.head_val[c], kBoxHeads[s_box_group_drawn][c]);
        }
    }
    box_fill_numbers();

    // 组切换:不在动画中就起一段;在动画中就让这一帧过去,下一次渲染会重新判断。
    if (!s_box_animating && state->box_group != s_box_group_drawn) {
        s_box_pending = (int8_t)state->box_group;
        box_anim_start(0, -BOX_CLIP_W, box_anim_out_done);
    } else if (!s_box_animating) {
        box_shift(0);
    }
}

// ---- 配网页 -------------------------------------------------------------
//
// 只在"设备还没有可用 Wi-Fi"时出现,作用是让用户知道该连哪个热点、该开哪个
// 地址——这两个信息只存在于屏幕上,手机里查不到。所以字号给足、层次只有三步。

typedef struct {
    lv_obj_t *screen;
    lv_obj_t *ssid;
    lv_obj_t *url;
    lv_obj_t *status;
} prov_t;

static prov_t s_prov;

static void prov_build(void)
{
    s_prov.screen = lv_obj_create(NULL);
    lv_obj_remove_flag(s_prov.screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_prov.screen, lv_color_hex(COLOR_BG), 0);
    lv_obj_set_style_border_width(s_prov.screen, 0, 0);
    lv_obj_set_style_pad_all(s_prov.screen, 0, 0);
    lv_obj_t *scr = s_prov.screen;

    lv_obj_t *title = make_label(scr, "Wi-Fi 配网", FONT_CJK, COLOR_TEXT);
    place(title, 12, 12, 0, LV_TEXT_ALIGN_LEFT);

    lv_obj_t *card = make_box(scr, 8, 52, 224, 172, COLOR_CARD, 12);
    lv_obj_t *accent = make_box(card, 0, 14, 4, 144, COLOR_LIVE, 2);
    (void)accent;

    lv_obj_t *step1 = make_label(card, "手机连热点", FONT_CJK, COLOR_MUTED);
    place(step1, 18, 16, 190, LV_TEXT_ALIGN_LEFT);
    s_prov.ssid = make_label(card, "", &lv_font_montserrat_20, COLOR_TEXT);
    place(s_prov.ssid, 18, 40, 190, LV_TEXT_ALIGN_LEFT);

    lv_obj_t *step2 = make_label(card, "打开", FONT_CJK, COLOR_MUTED);
    place(step2, 18, 84, 190, LV_TEXT_ALIGN_LEFT);
    s_prov.url = make_label(card, "", &lv_font_montserrat_20, COLOR_DAY_AWAY);
    place(s_prov.url, 18, 108, 190, LV_TEXT_ALIGN_LEFT);

    s_prov.status = make_label(scr, "", FONT_CJK, COLOR_MUTED);
    place(s_prov.status, 12, 240, 216, LV_TEXT_ALIGN_LEFT);

    // 这一页屏幕不收任何输入(密码在手机上敲),只有一个出口。不写出来,用户会
    // 以为设备卡住了,只能等 10 分钟热点超时或者拔电池。
    lv_obj_t *hint = make_label(scr, "长按确定返回", FONT_CJK, COLOR_MUTED);
    place(hint, 4, 296, 232, LV_TEXT_ALIGN_CENTER);
}

static void prov_render(const nba_ui_state_t *state)
{
    if (state->prov_ssid) lv_label_set_text(s_prov.ssid, state->prov_ssid);
    if (state->prov_url) lv_label_set_text(s_prov.url, state->prov_url);
    if (state->prov_status) lv_label_set_text(s_prov.status, state->prov_status);
}

void nba_ui_init(void)
{
    overview_build();
    detail_build();
    box_build();
    prov_build();
    s_loaded = NULL;

    // 四个页面是一次性建好的常驻对象,堆占用直接决定还能加多少控件。
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    ESP_LOGI(TAG, "界面就绪:LVGL 堆已用 %u/%u 字节,最大空闲块 %u 字节",
             (unsigned)(mon.total_size - mon.free_size), (unsigned)mon.total_size,
             (unsigned)mon.free_biggest_size);
}

void nba_ui_render(const nba_ui_state_t *state, const nba_board_t *board)
{
    lv_obj_t *target = s_overview.screen;
    if (state->view == NBA_VIEW_DETAIL) target = s_detail.screen;
    if (state->view == NBA_VIEW_BOX) target = s_box.screen;
    if (state->view == NBA_VIEW_PROVISION) target = s_prov.screen;
    if (target != s_loaded) {
        lv_screen_load(target);
        s_loaded = target;
    }
    switch (state->view) {
    case NBA_VIEW_PROVISION:
        prov_render(state);
        break;
    case NBA_VIEW_DETAIL:
        detail_render(state, board);
        break;
    case NBA_VIEW_BOX:
        box_render(state, board);
        break;
    case NBA_VIEW_OVERVIEW:
    default:
        overview_render(state, board);
        break;
    }
}
