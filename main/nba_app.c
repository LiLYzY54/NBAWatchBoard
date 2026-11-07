// main/nba_app.c —— 看板的主状态机与后台刷新。
//
// 任务划分:
//   ui_task  : 唯一接触 LVGL 的应用任务(自己加锁),消费按键事件与"数据已就绪"消息;
//   net_task : 只负责 Wi-Fi 重连与 HTTP 抓取,把结果放进 s_fetched,通过队列通知 UI。
// 这样抓取阻塞数秒时,按键依然能被立刻处理(只是渲染要等下一次 tick)。
//
// 按键语义(与屏幕底部提示一致):
//   总览:上/下 = 移动选择(循环),确定 = 进入详情,长按确定 = 立即刷新,
//         长按上/下 = 切到前一个/后一个比赛日
//   详情:上/下 = 切换比赛,确定 = 球员数据,长按确定 = 返回总览
//   球员:上/下 = 移动光标,确定 = 换下一组数据列(滑一格),
//         长按上 = 主队,长按下 = 客队,长按确定 = 返回详情
//   球员页的数据列不做自动轮播:什么时候翻、翻几组由这一下确定键决定。
//
// 球员数据按需抓取:只在进详情页按下确定时请求一次,同一场的结果缓存在 s_box,
// 再次进入不再重发。响应 ~450KB,但 boxscore 在文档开头,解析器读完两队名单就
// 断开,实际只下载 ~25KB。
//
// 比赛日按**北京时间**分组,与国内赛程表一致;ESPN 接口分组用的是美东日期。
// 北京比美东快 12~13 小时,美东傍晚开打的比赛在北京是第二天早上,所以:
//   北京 T 日的比赛  <->  ESPN 分组 T-1
// 实测 2026-10-31:ESPN 分组 10-31 的 7 场全部落在北京 11-01 03:00~08:30,而分组
// 11-01 的 4 场全在北京 11-02。所以取数时先减一天,再按开球时间过滤一次
// (nba_board_keep_day):分组里偶尔会混进前一天(北京)的场次。
#define ESPN_DAY_SHIFT (-1)
#include "nba_app.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bsp_battery.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nba_api.h"
#include "nba_model.h"
#include "nba_prov.h"
#include "nba_ui.h"
#include "wifi_mgr.h"

static const char *TAG = "nba_app";

// 刷新窗口不固定 60 秒,跟着赛程走:进行中勤刷,已结束/无比赛几乎不刷,
// 赛前一直睡到开赛前 5 分钟。手动刷新与翻日仍会立刻打断这个等待。
#define REFRESH_LIVE_MS     45000    // 有进行中比赛:45 秒跟上比分
#define REFRESH_PRE_NEAR_MS 60000    // 距开赛 <=10 分钟:盯住开赛瞬间
#define REFRESH_IDLE_MS     900000   // 15 分钟兜底:全是已结束/无比赛/赛前很久
#define REFRESH_DIM_MS      300000   // 背光已变暗(没人看):至少 5 分钟一次
#define REFRESH_UNKNOWN_MS  60000    // 未对时 / 上次抓取失败:保守起见一分钟
#define PRE_WAKE_LEAD_S     300      // 开赛前 5 分钟唤醒
#define HTTP_TIMEOUT_MS     10000
#define TICK_MS 1000                // UI 任务心跳:电量、时钟、自动变暗
#define BATTERY_PERIOD_MS 30000
#define DIM_AFTER_US (30LL * 1000 * 1000)
#define BACKLIGHT_ACTIVE 100
#define BACKLIGHT_DIM 22
#define DAY_OFFSET_MIN (-14)        // 最多往前翻两周,足够看完一轮客场之旅
#define DAY_OFFSET_MAX 7            // 往后只看一周(未来赛程意义有限)
#define PROBE_DAYS 3                // 当天没比赛时,自动前后各找几天(每次探一天,先往后再往前)
#define BOX_TIMEOUT_MS 15000        // 球员数据体积大,给比比分更宽的超时

typedef enum {
    MSG_KEY = 0,
    MSG_BOARD_READY,
    MSG_PROV_DONE,      // 配网页收到了手机提交的结果
} msg_type_t;

typedef struct {
    msg_type_t type;
    bsp_btn_t btn;
    bsp_btn_ev_t ev;
} app_msg_t;

static nba_ui_state_t s_ui = {
    .view = NBA_VIEW_OVERVIEW,
    .selected = 0,
    .net_text = "",
    .day_text = "",
    .day_away = false,
    .battery = -1,
    .need_setup = true,       // 启动时按没有凭据处理,第一次刷新即纠正
    .box = NULL,
    .box_team = 0,
    .box_group = 0,
    .prov_ssid = NULL,
    .prov_url = NULL,
    .prov_status = NULL,
};
static nba_board_t s_board;      // UI 侧数据
static nba_board_t s_fetched;    // 网络任务写入的暂存
static nba_box_t s_box;          // 当前这场比赛的球员数据(game_id 空 = 没有)
static nba_box_t s_box_fetched;  // 网络任务写入的球员数据暂存
static SemaphoreHandle_t s_board_lock;   // 同时保护 s_board 与 s_box
static QueueHandle_t s_queue;
static TaskHandle_t s_net_task;
static TaskHandle_t s_ui_task;

// 球员数据的按需请求:UI 任务置位,网络任务消费。s_box_req_away 是发起时的客队缩写,
// 用来把接口给的两队顺序对齐到"客队在前"。
static bool s_box_req;
static char s_box_req_id[16];
static char s_box_req_away[NBA_ABBR_LEN];

static char s_net_text[32] = "连接中";
static char s_last_update[8];    // "21:30"
static char s_day_text[8];       // "10-31"
static bool s_last_fetch_ok;
static volatile bool s_need_portal;   // 网络任务写、UI 任务读:撞上了 Portal 认证页
static int64_t s_last_key_us;
static volatile bool s_dimmed;

// 抓取用的两块中转板:不放在任务栈上(board 结构体约 800 字节一块)。
// s_alt 给 fetch_day 的"补抓"用,s_probe 给"当天无比赛前后探测"用,互不覆盖。
static nba_board_t s_alt;
static nba_board_t s_probe;
// 补抓结果里全部场次共同的北京日期(仅当可整体采纳时有效,全 0 = 不可用)。
// 补抓拉的是美东"当天"分组,过滤后不属于北京 target 的场次几乎必然属于
// target+1(美东当晚 = 北京次日早上)。记下来,上层可直接采纳,免得重下 200KB+。
static nba_date_t s_alt_day;

// ---- 配网状态 -----------------------------------------------------------
//
// 三个进配网的入口,覆盖"这辈子只配一次"到"换个地方就得重配":
//   1. 开机没凭据            —— 首次上电;
//   2. 有凭据但连不上超过 90 秒 —— 换了 Wi-Fi 环境(最常见);
//   3. 看板页长按确定且无凭据   —— 用户中途退出了配网页,想再进去。
static volatile bool s_prov_active;
static volatile bool s_prov_wanted;      // 启动时判定"该进配网",交给 ui_task 执行
static int64_t s_prov_start_us;
static int64_t s_prov_close_at_us;       // 0 = 还没出结果;否则到点关热点
static int64_t s_offline_since_us;       // 有凭据却处于 FAILED 的起点

#define PROV_IDLE_TIMEOUT_US  (10LL * 60 * 1000 * 1000)  // 热点最多开 10 分钟
#define PROV_CLOSE_DELAY_US   (1500LL * 1000)            // 结果页留给手机收完
#define OFFLINE_PROV_AFTER_US (90LL * 1000 * 1000)       // 连不上多久后自动开热点

static void enter_prov(void);
static void trigger_refresh(void);

// 比赛日翻页状态:基准日(北京时间的"今天")+ 偏移。s_base_known 之前不允许翻页。
static nba_date_t s_base_day;
static bool s_base_known;
static int s_day_offset;
static bool s_probed;            // "当天无比赛就往前找"只做一次,避免反复空转

// ---- 顶栏文案 -----------------------------------------------------------

static void refresh_net_text(void)
{
    switch (wifi_mgr_state()) {
    case WIFI_MGR_NO_CREDS:
        snprintf(s_net_text, sizeof(s_net_text), "未配网");
        break;
    case WIFI_MGR_CONNECTING:
        snprintf(s_net_text, sizeof(s_net_text), "连接中");
        break;
    case WIFI_MGR_FAILED:
        snprintf(s_net_text, sizeof(s_net_text), "网络断开");
        break;
    case WIFI_MGR_CONNECTED:
    default:
        // 校园网/酒店网:Wi-Fi 明明连着,HTTP 却被网关 302 到认证页。这时
        // "刷新失败"太含糊,直接说外网没通,用户才知道要去网页登录。
        if (s_need_portal) {
            snprintf(s_net_text, sizeof(s_net_text), "网络未连");
        } else if (s_last_update[0] == '\0') {
            snprintf(s_net_text, sizeof(s_net_text), "刷新中");
        } else {
            snprintf(s_net_text, sizeof(s_net_text), "%s %s",
                     s_last_fetch_ok ? "更新" : "刷新失败", s_last_update);
        }
        break;
    }
    s_ui.net_text = s_net_text;
}

static void stamp_update_time(void)
{
    // 未对时的时候显示本地时间毫无意义,直接留空,顶栏只显示状态词。
    if (!wifi_mgr_time_synced()) {
        s_last_update[0] = '\0';
        return;
    }
    const time_t now = time(NULL);
    struct tm local;
    if (localtime_r(&now, &local)) {
        snprintf(s_last_update, sizeof(s_last_update), "%02d:%02d",
                 local.tm_hour, local.tm_min);
    }
}

// 顶栏的日期取"屏幕上的数据属于哪一天",而不是"用户按到了第几天":
// 抓取失败时旧数据还在,日期标签必须跟着旧数据,不能骗人。
static void refresh_day_text(void)
{
    if (nba_format_day(&s_board.day, s_day_text, sizeof(s_day_text)) == 0) {
        s_day_text[0] = '\0';
    }
    s_ui.day_text = s_day_text;
    s_ui.day_away = nba_date_valid(&s_base_day) && nba_date_valid(&s_board.day) &&
                    (s_board.day.year != s_base_day.year ||
                     s_board.day.month != s_base_day.month ||
                     s_board.day.day != s_base_day.day);
}

// ---- 渲染 ---------------------------------------------------------------

static void render_locked(void)
{
    // 没有凭据时,总览页长按确定的行为是"重新开配网"而不是刷新,提示文案跟着变。
    s_ui.need_setup = !wifi_mgr_has_credentials();
    // 换日之后比赛数可能变少,选中行要夹回范围内,否则高亮会消失。
    if (s_board.count == 0) {
        s_ui.selected = 0;
    } else if (s_ui.selected >= s_board.count) {
        s_ui.selected = (uint8_t)(s_board.count - 1);
    }
    // 球员页的光标同理:名单可能比上一次短。两队各记一份,换队回来不跳位。
    for (int t = 0; t < NBA_BOX_TEAMS; t++) {
        const uint8_t rows = s_box.teams[t].count;
        if (rows == 0) s_ui.box_cursor[t] = 0;
        else if (s_ui.box_cursor[t] >= rows) s_ui.box_cursor[t] = (uint8_t)(rows - 1);
    }
    if (s_ui.box_team >= NBA_BOX_TEAMS || s_box.teams[s_ui.box_team].count == 0) {
        s_ui.box_team = (s_box.teams[0].count == 0 && s_box.teams[1].count > 0) ? 1 : 0;
    }
    // 缓存的是"某一场"的名单:换到别的场次时必须当作没有,否则会显示上一场的球员。
    const bool box_matches =
        s_board.count > 0 && s_box.game_id[0] &&
        strcmp(s_box.game_id, s_board.games[s_ui.selected].id) == 0;
    s_ui.box = (s_box.ready && box_matches) ? &s_box : NULL;
    refresh_net_text();
    refresh_day_text();
    nba_ui_render(&s_ui, &s_board);
}

static void request_render(void)
{
    if (!bsp_lvgl_lock(300)) return;
    render_locked();
    bsp_lvgl_unlock();
}

// ---- 网络任务 -----------------------------------------------------------

static void notify_ui(msg_type_t type)
{
    const app_msg_t msg = { .type = type };
    if (s_queue) (void)xQueueSend(s_queue, &msg, 0);
}

// 抓"北京时间 target 这一天的比赛"。返回取数结果;out->day 一定是 target。
//
// 先取 ESPN 的 target+ESPN_DAY_SHIFT 分组,再按开球时间过滤。过滤后一场不剩时才
// 补抓 ESPN 的 target 分组:美东中午前开打的比赛(伦敦赛、墨西哥城赛之类)在北京
// 是同一个日历日,会落在当天分组里。这种情况罕见,平时不会多发请求。
static nba_fetch_result_t fetch_day(const nba_date_t *target, nba_board_t *out)
{
    nba_date_t source = *target;
    if (!nba_date_add_days(&source, ESPN_DAY_SHIFT)) return NBA_FETCH_PARSE_FAIL;

    const nba_fetch_result_t result = nba_api_fetch(out, &source, HTTP_TIMEOUT_MS);
    if (result != NBA_FETCH_OK) return result;

    s_alt_day = (nba_date_t){0};
    if (nba_board_keep_day(out, target) == 0 &&
        nba_api_fetch(&s_alt, target, HTTP_TIMEOUT_MS) == NBA_FETCH_OK) {
        // keep_day 会就地删掉不属于 target 的场次,所以过滤在副本上做,原板
        // s_alt 留给"今天无比赛"的采纳路径(那里要的就是这些"不属于今天"的场次)。
        nba_date_t first;
        if (s_alt.count > 0 && nba_date_from_iso_beijing(s_alt.games[0].tipoff, &first)) {
            bool uniform = true;
            for (uint8_t i = 1; i < s_alt.count; i++) {
                nba_date_t d;
                if (!nba_date_from_iso_beijing(s_alt.games[i].tipoff, &d) ||
                    d.year != first.year || d.month != first.month ||
                    d.day != first.day) {
                    uniform = false;
                    break;
                }
            }
            if (uniform) s_alt_day = first;
        }
        s_probe = s_alt;
        if (nba_board_keep_day(&s_probe, target) > 0) {
            *out = s_probe;
        }
    }

    // 即使这天没有比赛,屏幕上的日期也要如实显示用户翻到的那一天。
    out->day = *target;
    return NBA_FETCH_OK;
}

// 抓一次"用户当前翻到的那一天"。锚点每次重算:设备跨过午夜后 offset=0 自动跟上。
static nba_fetch_result_t refresh_board(void)
{
    nba_date_t today;
    if (nba_date_today_beijing(&today)) {
        s_base_day = today;
        s_base_known = true;
    }

    if (!s_base_known) {
        // 未对时:不带 dates 抓一次(服务端给"最近一个有比赛的日子"),再从首场比赛的
        // 开球时间反推北京日期当锚点。接口本身不返回"这属于哪天"。
        const nba_fetch_result_t result = nba_api_fetch(&s_fetched, NULL, HTTP_TIMEOUT_MS);
        if (result != NBA_FETCH_OK) return result;
        if (nba_board_day_from_games(&s_fetched)) {
            s_base_day = s_fetched.day;
            s_base_known = true;
        }
        s_day_offset = 0;
        return NBA_FETCH_OK;
    }

    nba_date_t target = s_base_day;
    if (!nba_date_add_days(&target, s_day_offset)) return NBA_FETCH_PARSE_FAIL;

    const nba_fetch_result_t result = fetch_day(&target, &s_fetched);
    if (result != NBA_FETCH_OK) return result;

    // 今天一场没有(NBA 不是每天都有比赛,或者这一天的场次全在"北京明天早上"):
    // 优先采纳补抓结果——它就是接下来要打的那批,已经下载过了,不用重下;否则
    // 先往后找最近的比赛日(凌晨看板该显示今晚要打的,而不是昨天打完的),往后
    // 找不到再往前回退看历史。只做一次,之后完全由用户长按上下决定。
    if (s_day_offset == 0 && s_fetched.count == 0 && !s_probed) {
        s_probed = true;
        nba_date_t next = target;
        if (nba_date_valid(&s_alt_day) && nba_date_add_days(&next, 1) &&
            next.year == s_alt_day.year && next.month == s_alt_day.month &&
            next.day == s_alt_day.day) {
            s_day_offset = 1;
            s_fetched = s_alt;
            s_fetched.day = s_alt_day;   // 补抓把 day 标成了 target,采纳时改成真实日期
            ESP_LOGI(TAG, "今天无比赛,自动跳到 %02d-%02d 的赛程(+1 天)",
                     s_alt_day.month, s_alt_day.day);
            return NBA_FETCH_OK;
        }
        for (int fwd = 1; fwd <= PROBE_DAYS; fwd++) {
            nba_date_t probe = target;
            if (!nba_date_add_days(&probe, fwd)) break;
            if (fetch_day(&probe, &s_probe) != NBA_FETCH_OK) break;   // 网络出问题就别再追加请求
            if (s_probe.count > 0) {
                s_day_offset = fwd;
                s_fetched = s_probe;
                ESP_LOGI(TAG, "今天无比赛,已前进 %d 天", fwd);
                break;
            }
        }
        if (s_fetched.count == 0) {
            for (int back = 1; back <= PROBE_DAYS; back++) {
                nba_date_t probe = target;
                if (!nba_date_add_days(&probe, -back)) break;
                if (fetch_day(&probe, &s_probe) != NBA_FETCH_OK) break;   // 网络出问题就别再追加请求
                if (s_probe.count > 0) {
                    s_day_offset = -back;
                    s_fetched = s_probe;
                    ESP_LOGI(TAG, "后几天也没有比赛,已回退 %d 天", back);
                    break;
                }
            }
        }
    }
    return NBA_FETCH_OK;
}

static void commit_board(void)
{
    if (s_board_lock) xSemaphoreTake(s_board_lock, portMAX_DELAY);
    memcpy(&s_board, &s_fetched, sizeof(s_board));
    if (s_board_lock) xSemaphoreGive(s_board_lock);
    s_last_fetch_ok = true;
    stamp_update_time();
    ESP_LOGI(TAG, "看板已更新:%u 场(比赛日 %04d-%02d-%02d, offset %d)", s_board.count,
             s_board.day.year, s_board.day.month, s_board.day.day, s_day_offset);
}

// 下一次自动刷新前等多久。固定 60 秒在"全是已结束"时等于每小时白下 60 次
// 276KB,是整机功耗的大头,所以让间隔跟着赛程走:
//   * 有比赛进行中   -> 45 秒(比分要跟得上);
//   * 全是未开始     -> 睡到最近一场开赛前 5 分钟,临近开赛改成 60 秒;
//   * 全是已结束/无比赛 -> 15 分钟兜底(跨过午夜时自动跟上新的一天);
//   * 屏幕已变暗     -> 至少 5 分钟,反正没人看(唤醒的按键会立刻打断等待)。
static uint32_t refresh_interval_ms(void)
{
    if (!s_base_known || !s_last_fetch_ok) return REFRESH_UNKNOWN_MS;

    bool live = false;
    bool have_pre = false;
    time_t nearest = 0;
    if (s_board_lock) xSemaphoreTake(s_board_lock, portMAX_DELAY);
    for (uint8_t i = 0; i < s_board.count; i++) {
        const nba_game_t *g = &s_board.games[i];
        if (g->state == NBA_STATE_LIVE) { live = true; break; }
        if (g->state == NBA_STATE_PRE) {
            time_t t;
            if (nba_tipoff_epoch(g->tipoff, &t) && t > 0 &&
                (!have_pre || t < nearest)) {
                nearest = t;
                have_pre = true;
            }
        }
    }
    if (s_board_lock) xSemaphoreGive(s_board_lock);

    const time_t now = time(NULL);
    if (now < (time_t)1577836800) return REFRESH_UNKNOWN_MS;   // 设备未对时

    uint32_t interval;
    if (live) {
        interval = REFRESH_LIVE_MS;
    } else if (have_pre) {
        const int64_t sec = (int64_t)nearest - (int64_t)now;
        if (sec <= 10 * 60) {
            interval = REFRESH_PRE_NEAR_MS;
        } else {
            const int64_t wait_ms = (sec - PRE_WAKE_LEAD_S) * 1000;
            interval = (wait_ms >= REFRESH_IDLE_MS) ? REFRESH_IDLE_MS
                     : (wait_ms <= REFRESH_PRE_NEAR_MS) ? REFRESH_PRE_NEAR_MS
                     : (uint32_t)wait_ms;
        }
    } else {
        interval = REFRESH_IDLE_MS;
    }
    if (s_dimmed && interval < REFRESH_DIM_MS) interval = REFRESH_DIM_MS;
    return interval;
}

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        // 等"立即刷新"通知,超时即自动刷新。未联网时用短超时,以便连上 Wi-Fi 后
        // 立刻出第一屏数据,而不是干等一个完整周期。
        const bool online = wifi_mgr_connected();
        const uint32_t interval = online ? refresh_interval_ms() : 3000;
        if (online) ESP_LOGI(TAG, "下次自动刷新:%u 秒", (unsigned)(interval / 1000));
        const TickType_t wait = pdMS_TO_TICKS(interval);
        (void)ulTaskNotifyTake(pdTRUE, wait);

        if (!wifi_mgr_connected()) {
            // 断开或失败时顺手重连:密码错会再次失败,不会无限快速重试
            //(事件回调里有次数上限,这里只是下一次抓取周期的自然退避)。
            if (wifi_mgr_state() == WIFI_MGR_FAILED) wifi_mgr_start();
            notify_ui(MSG_BOARD_READY);
            continue;
        }

        // 抓取期间临时关掉 modem 省电换取全速,抓完立刻恢复(见 wifi_mgr.c)。
        // 其余时间 Wi-Fi 按 DTIM 休眠,这是常驻电流的大头。
        wifi_mgr_set_fast_mode(true);

        // 球员数据的请求优先:用户刚按下确定,等比分刷新完再看名单会很突兀。
        if (s_box_req) {
            s_box_req = false;
            if (nba_box_fetch(&s_box_fetched, s_box_req_id, BOX_TIMEOUT_MS) &&
                nba_box_fix_order(&s_box_fetched, s_box_req_away)) {
                snprintf(s_box_fetched.game_id, sizeof(s_box_fetched.game_id), "%s",
                         s_box_req_id);
                if (s_board_lock) xSemaphoreTake(s_board_lock, portMAX_DELAY);
                s_box = s_box_fetched;
                if (s_board_lock) xSemaphoreGive(s_board_lock);
                ESP_LOGI(TAG, "球员数据已更新:%s %u 人 / %s %u 人",
                         s_box.teams[0].abbr, s_box.teams[0].count,
                         s_box.teams[1].abbr, s_box.teams[1].count);
            } else {
                ESP_LOGW(TAG, "球员数据未取到(比赛 %s)", s_box_req_id);
            }
            notify_ui(MSG_BOARD_READY);
        }

        const nba_fetch_result_t result = refresh_board();
        const bool portal = nba_api_take_portal_flag();
        wifi_mgr_set_fast_mode(false);   // 唯一的恢复点:成功失败都要走
        s_need_portal = (result != NBA_FETCH_OK) && portal;
        if (result != NBA_FETCH_OK) {
            s_last_fetch_ok = false;
            stamp_update_time();
            ESP_LOGW(TAG, "刷新失败,保留上一份数据");
            notify_ui(MSG_BOARD_READY);
            continue;
        }

        commit_board();
        notify_ui(MSG_BOARD_READY);
    }
}

// ---- 按键与 UI 任务 -----------------------------------------------------

static void move_selection(int delta)
{
    if (s_board.count == 0) return;
    int next = (int)s_ui.selected + delta;
    if (next < 0) next = s_board.count - 1;
    if (next >= s_board.count) next = 0;
    s_ui.selected = (uint8_t)next;
}

static void move_box_cursor(int delta)
{
    const uint8_t team = s_ui.box_team;
    const int total = s_box.teams[team].count;
    if (total <= 0) return;
    int next = (int)s_ui.box_cursor[team] + delta;
    if (next < 0) next = total - 1;
    if (next >= total) next = 0;
    s_ui.box_cursor[team] = (uint8_t)next;
}

// 长按上下切客/主两队。0 = 客队(接口里 fix_order 之后的第一队),1 = 主队。
static void switch_box_team(uint8_t team)
{
    if (team >= NBA_BOX_TEAMS || s_box.teams[team].count == 0) return;
    s_ui.box_team = team;
}

// 进入球员页。同一场只抓一次(结果缓存在 s_box->game_id);失败不写 game_id,
// 下次进来会重试——比"失败就永久显示无数据"更符合直觉。
static void enter_box_view(void)
{
    if (s_board.count == 0) return;
    const nba_game_t *game = &s_board.games[s_ui.selected];
    s_ui.view = NBA_VIEW_BOX;
    s_ui.box_team = 0;          // 从客队开始,与详情页"客队在左"一致
    s_ui.box_group = 0;
    s_ui.box_cursor[0] = 0;
    s_ui.box_cursor[1] = 0;
    if (game->id[0] == '\0') return;
    if (s_box.game_id[0] && strcmp(s_box.game_id, game->id) == 0) return;
    if (s_box_req && strcmp(s_box_req_id, game->id) == 0) return;
    snprintf(s_box_req_id, sizeof(s_box_req_id), "%s", game->id);
    snprintf(s_box_req_away, sizeof(s_box_req_away), "%s", game->away.abbr);
    s_box_req = true;
    if (s_net_task) (void)xTaskNotifyGive(s_net_task);
}

// ---- 配网 ---------------------------------------------------------------

static void enter_prov(void)
{
    if (s_prov_active) return;
    const esp_err_t err = nba_prov_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "配网热点启动失败: %s", esp_err_to_name(err));
        return;
    }
    s_prov_active = true;
    s_prov_start_us = esp_timer_get_time();
    s_prov_close_at_us = 0;
    s_ui.view = NBA_VIEW_PROVISION;
    s_ui.prov_ssid = nba_prov_ap_ssid();
    s_ui.prov_url = nba_prov_ap_url();
    s_ui.prov_status = "等待连接";
    ESP_LOGI(TAG, "进入配网:热点 %s / 地址 %s", s_ui.prov_ssid, s_ui.prov_url);
}

static void leave_prov(bool ok)
{
    if (!s_prov_active) return;
    nba_prov_stop();
    s_prov_active = false;
    s_ui.prov_ssid = NULL;
    s_ui.prov_url = NULL;
    s_ui.prov_status = NULL;
    s_ui.view = NBA_VIEW_OVERVIEW;
    s_prov_close_at_us = 0;
    s_offline_since_us = 0;
    if (ok) {
        // 关热点时 Wi-Fi 重新进入了 STA 模式,STA 侧连接已被打断,必须重连一次。
        wifi_mgr_start();
        trigger_refresh();
    }
    ESP_LOGI(TAG, "退出配网(%s)", ok ? "已保存并连通" : "未完成");
}

// 由配网模块在 HTTP 任务里回调:只入队,不碰 LVGL,也不写跨任务的 64 位时间戳
// (32 位 RISC-V 上非原子,读端可能读到撕裂值)。计时全部回到 ui_task 里做。
static void on_prov_done(bool ok)
{
    if (!s_queue) return;
    const app_msg_t msg = { .type = MSG_PROV_DONE };
    (void)xQueueSend(s_queue, &msg, 0);
    (void)ok;   // 结果由 nba_prov_last_ok() 取,避免两处状态不一致
}

static void trigger_refresh(void)
{
    // 没有凭据时"刷新"本身没有意义:把确定键当成重新打开配网页的入口。
    // 用户中途退出配网(或热点超时关了)之后,不用插电脑也能再进去。
    if (!wifi_mgr_has_credentials() && !s_prov_active) {
        enter_prov();
        return;
    }
    s_last_update[0] = '\0';   // 立刻显示"刷新中"
    if (s_net_task) (void)xTaskNotifyGive(s_net_task);
}

// 换比赛日。锚点未知(还没抓到过任何数据)时不动,避免请求一个凭空猜的日期。
static bool shift_day(int delta)
{
    if (!s_base_known) return false;
    int next = s_day_offset + delta;
    if (next < DAY_OFFSET_MIN) next = DAY_OFFSET_MIN;
    if (next > DAY_OFFSET_MAX) next = DAY_OFFSET_MAX;
    if (next == s_day_offset) return false;
    s_day_offset = next;
    s_ui.selected = 0;
    trigger_refresh();
    return true;
}

static void handle_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    if (ev == BSP_BTN_PRESS || ev == BSP_BTN_RELEASE || ev == BSP_BTN_DOUBLE) return;

    if (s_dimmed) {
        bsp_display_backlight(BACKLIGHT_ACTIVE);
        s_dimmed = false;
        // 暗屏时刷新间隔被拉到 5 分钟,唤醒这一下要立刻打断等待,别让人盯着旧比分。
        if (s_net_task) (void)xTaskNotifyGive(s_net_task);
    }

    // 配网页上屏幕不收输入:密码在手机上敲。只有"长按确定"能退出,退出后回到
    // 空白看板,再长按确定可以重新开热点(见 trigger_refresh)。
    if (s_ui.view == NBA_VIEW_PROVISION) {
        if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
            leave_prov(false);
            request_render();
        }
        return;
    }

    if (ev == BSP_BTN_CLICK) {
        if (btn == BSP_BTN_UP) {
            if (s_ui.view == NBA_VIEW_BOX) move_box_cursor(-1);
            else move_selection(-1);
        } else if (btn == BSP_BTN_DOWN) {
            if (s_ui.view == NBA_VIEW_BOX) move_box_cursor(1);
            else move_selection(1);
        } else if (btn == BSP_BTN_OK) {
            if (s_ui.view == NBA_VIEW_OVERVIEW) {
                if (s_board.count > 0) s_ui.view = NBA_VIEW_DETAIL;
            } else if (s_ui.view == NBA_VIEW_DETAIL) {
                enter_box_view();
            } else {
                // 球员页:确定 = 换下一组数据列。轮播完全由这一下驱动,没有自动计时——
                // 自动翻页会在人正盯着某一行时把数字换掉,读起来反而累。
                s_ui.box_group = (uint8_t)((s_ui.box_group + 1) % NBA_BOX_GROUPS);
            }
        }
    } else if (ev == BSP_BTN_LONG && btn == BSP_BTN_OK) {
        if (s_ui.view == NBA_VIEW_BOX) {
            s_ui.view = NBA_VIEW_DETAIL;
        } else if (s_ui.view == NBA_VIEW_DETAIL) {
            s_ui.view = NBA_VIEW_OVERVIEW;
        } else {
            trigger_refresh();
        }
    } else if (ev == BSP_BTN_LONG && (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN)) {
        if (s_ui.view == NBA_VIEW_BOX) {
            // 长按上 = 主队,长按下 = 客队。
            switch_box_team(btn == BSP_BTN_UP ? 1 : 0);
        } else if (s_ui.view == NBA_VIEW_OVERVIEW) {
            // 长按上下 = 前/后一个比赛日(只在总览页;详情页里上下是换场次)。
            (void)shift_day(btn == BSP_BTN_UP ? -1 : 1);
        }
    }
    request_render();
}

static void ui_task(void *arg)
{
    (void)arg;
    if (bsp_lvgl_lock(1000)) {
        nba_ui_init();
        render_locked();
        bsp_lvgl_unlock();
    } else {
        ESP_LOGE(TAG, "无法取得 LVGL 锁,界面未建立");
    }

    int64_t last_battery_us = 0;
    wifi_mgr_state_t last_wifi = (wifi_mgr_state_t)-1;
    for (;;) {
        app_msg_t msg;
        if (xQueueReceive(s_queue, &msg, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
            if (msg.type == MSG_KEY) {
                s_last_key_us = esp_timer_get_time();   // 变暗计时从这里重新起算
                handle_key(msg.btn, msg.ev);
            } else if (msg.type == MSG_PROV_DONE) {
                // 结果页已经发回手机,再留 1.5 秒才关热点:立刻关的话,浏览器
                // 只会看到"连接被重置",看不到"已连接"这几个字。
                s_prov_close_at_us = esp_timer_get_time() + PROV_CLOSE_DELAY_US;
                request_render();
            } else {
                request_render();
            }
        }

        const int64_t now = esp_timer_get_time();

        // 配网:启动判定 → 收尾 → 空闲超时。启动判定要放在 ui_task 里而不是
        // nba_app_start 里,因为 enter_prov 会改 s_ui,必须和渲染串在同一个任务上。
        if (s_prov_wanted && !s_prov_active) {
            s_prov_wanted = false;
            enter_prov();
            request_render();
        }
        if (s_prov_active) {
            if (s_prov_close_at_us > 0 && now >= s_prov_close_at_us) {
                s_prov_close_at_us = 0;
                leave_prov(nba_prov_last_ok());
                request_render();
            } else if (s_prov_close_at_us == 0 &&
                       now - s_prov_start_us > PROV_IDLE_TIMEOUT_US) {
                ESP_LOGW(TAG, "配网超时,关闭热点");
                leave_prov(false);
                request_render();
            }
        } else if (s_offline_since_us > 0 &&
                   now - s_offline_since_us > OFFLINE_PROV_AFTER_US) {
            s_offline_since_us = 0;
            ESP_LOGW(TAG, "已保存的 Wi-Fi 持续连不上,自动进入配网");
            enter_prov();
            request_render();
        }

        if (now - last_battery_us > BATTERY_PERIOD_MS * 1000LL) {
            last_battery_us = now;
            const int soc = bsp_battery_soc();
            if (soc != s_ui.battery) {
                s_ui.battery = soc;
                request_render();
            }
        }
        // 连接状态变化要立刻反映到顶栏,不能等下一次抓取周期。
        const wifi_mgr_state_t wifi_state = wifi_mgr_state();
        if (wifi_state != last_wifi) {
            last_wifi = wifi_state;
            if (wifi_state == WIFI_MGR_CONNECTED) {
                s_offline_since_us = 0;
                // 自动刷新窗口可能长达 15 分钟,重连后不能干等到下一个周期。
                if (s_net_task) (void)xTaskNotifyGive(s_net_task);
            } else if (wifi_state == WIFI_MGR_FAILED && s_offline_since_us == 0 &&
                       wifi_mgr_all_failed()) {
                // 只在"所有已存网络都轮换过仍失败"上起算:多网络轮换途中单网
                // 失败属正常,不能抢跑开配网。CONNECTING 不清零:net_task 每
                // 3 秒会重新拉起连接,状态在 FAILED↔CONNECTING 间跳,清零会
                // 永远累计不到。
                s_offline_since_us = now;
            }
            request_render();
        }
        // 电池供电的随身设备:没人看的时候把背光压下去,按键立刻恢复。
        if (!s_dimmed && now - s_last_key_us > DIM_AFTER_US) {
            s_dimmed = true;
            bsp_display_backlight(BACKLIGHT_DIM);
        }
    }
}

// ---- 初始化 -------------------------------------------------------------

void nba_app_on_key(bsp_btn_t btn, bsp_btn_ev_t ev, void *user)
{
    (void)user;
    // 只做入队:本回调运行在共享 esp_timer 任务里,任何阻塞都会拖慢系统定时器。
    if (!s_queue) return;
    const app_msg_t msg = { .type = MSG_KEY, .btn = btn, .ev = ev };
    (void)xQueueSend(s_queue, &msg, 0);
}

void nba_app_inject_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    nba_app_on_key(btn, ev, NULL);
}

void nba_app_request_prov(void)
{
    s_prov_wanted = true;
}

esp_err_t nba_app_start(void)
{
    s_queue = xQueueCreate(10, sizeof(app_msg_t));
    s_board_lock = xSemaphoreCreateMutex();
    if (!s_queue || !s_board_lock) return ESP_ERR_NO_MEM;

    nba_board_reset(&s_board);
    nba_board_reset(&s_fetched);
    nba_box_reset(&s_box);
    nba_box_reset(&s_box_fetched);
    s_last_key_us = esp_timer_get_time();

    if (xTaskCreate(ui_task, "nba_ui", 5120, NULL, 5, &s_ui_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(net_task, "nba_net", 6144, NULL, 4, &s_net_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    const esp_err_t button_err = bsp_button_init(nba_app_on_key, NULL);
    if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "按键初始化失败: %s", esp_err_to_name(button_err));
        return button_err;
    }

    // 没有凭据 = 第一次上电(或被清过配置):直接进配网,别让用户对着一张空白
    // 看板猜该干什么。判定放在建任务之前,ui_task 第一轮循环就会切过去。
    if (!wifi_mgr_has_credentials()) {
        s_prov_wanted = true;
    }
    nba_prov_set_done_cb(on_prov_done);

    // 首次进场就给网络任务一个启动信号,不必等满一个刷新周期。
    (void)xTaskNotifyGive(s_net_task);
    return ESP_OK;
}
