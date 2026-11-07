// main/nba_ui.h —— NBA 看板的界面层(纯视图,不联网、不读按键)。
//
// 四个页面:
//   * 总览:今日/最近比赛列表,4 行可见,上下键滚动选中;
//   * 详情:单场比分、节次分栏、战绩;
//   * 球员:一次看一支球队的球员名单,长按上下在客/主两队之间切换;
//   * 配网:设备还没有可用 Wi-Fi 时给出热点名和访问地址(见 nba_prov.c)。
// 界面全部由本文件自建,不复用基线 demo 的测试菜单与 ui_pixel 视觉外壳。
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "nba_box.h"
#include "nba_model.h"

typedef enum {
    NBA_VIEW_OVERVIEW = 0,
    NBA_VIEW_DETAIL,
    NBA_VIEW_BOX,
    NBA_VIEW_PROVISION,   // 手机配网提示页(还没有可用 Wi-Fi 时)
} nba_view_t;

// 球员页的数据列分组:一屏放不下 12 列,分成 分板助 / 投三罚 / 断帽失 / 时差犯
// 四组轮播,名字列固定不动,由确定键逐格推进(不自动翻页)。
#define NBA_BOX_GROUPS 4

typedef struct {
    nba_view_t view;
    uint8_t selected;      // 总览/详情共用的当前比赛下标
    const char *net_text;  // 顶栏副标题:更新时间/连接状态
    const char *day_text;  // 顶栏日期 "10-31";空串表示日期未知
    bool day_away;         // 当前不是"今天":日期用强调色,提示是翻过去的
    int battery;           // 0..100,-1 = 电量未知
    bool need_setup;       // 还没有 Wi-Fi 凭据:总览页的长按确定是"开配网"而不是"刷新"

    // 球员数据页。box 为 NULL 或 box->ready 为假时显示"刷新中"。
    const nba_box_t *box;
    uint8_t box_team;              // 0 = 客队,nba_box_fix_order 之后 teams[0] 就是客队
    uint8_t box_cursor[NBA_BOX_TEAMS];   // 每队各自记住光标,换回来不跳位
    uint8_t box_group;             // 当前显示第几组数据列

    // 配网页。仅 view == NBA_VIEW_PROVISION 时读取,指针由调用方持有。
    const char *prov_ssid;         // 热点名,如 "NBA-Board"
    const char *prov_url;          // 手机要打开的地址,如 "192.168.4.1"
    const char *prov_status;       // 状态词,如 "等待连接" / "连接失败"
} nba_ui_state_t;

// 建立三个页面。需在 LVGL 初始化之后调用,并由调用方持 LVGL 锁。
void nba_ui_init(void);

// 按当前状态刷新界面(自行选择加载哪个页面)。需在 LVGL 锁内调用。
void nba_ui_render(const nba_ui_state_t *state, const nba_board_t *board);
