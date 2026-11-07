// main/nba_api.c —— ESPN 记分接口的 HTTP 拉取。
//
// 两个刻意的选择:
//   * 走 http:// 而不是 https://:本板无 PSRAM,mbedTLS 握手要一次性拿出 30~40KB
//     连续堆,和 LVGL、Wi-Fi 抢同一块内部 RAM。该接口在 80 端口同样返回完整
//     JSON,因此第一版不做 TLS;换用需要鉴权的接口时必须改回 https 并重新评估内存。
//   * 边收边解析:nba_parse 是流式收集器,响应本身 70~240KB,不需要整包缓存。
#include "nba_api.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "nba_box.h"
#include "nba_parse.h"

static const char *TAG = "nba_api";

// 校园网/酒店网常见 Captive Portal:Wi-Fi 显示"已连接",但所有 HTTP 请求被网关
// 302 到网页认证页。这类重定向跟过去也拿不到 JSON,反而要花一次 TLS 握手,
// 所以只记标志、不跟随;界面据此提示"网络未连"而不是一直转圈。
static volatile bool s_portal_seen;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER &&
        evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Location") == 0 &&
        strstr(evt->header_value, "espn.com") == NULL) {
        s_portal_seen = true;
        ESP_LOGW(TAG, "请求被重定向到非 ESPN 页面(需要网页认证): %s", evt->header_value);
    }
    return ESP_OK;
}

bool nba_api_take_portal_flag(void)
{
    const bool seen = s_portal_seen;
    s_portal_seen = false;
    return seen;
}

// limit 与看板容量一致:响应约 200~350KB,流式解析不需要整包缓存。
// 带 dates=YYYYMMDD 取那一天;不带时接口返回的是"最近一个有比赛的日子",不是今天。
// 注意:接口只接受单日,不接受区间(dates=A-B 直接返回 HTTP 400),所以翻日
// 只能一天一发。日期口径是美东,换算成北京日期在 nba_app.c 里做。
#define NBA_SCOREBOARD_URL \
    "http://site.api.espn.com/apis/site/v2/sports/basketball/nba/scoreboard"
// 读缓冲 2KB:实测 512B 时 276KB 响应要爬 20 秒(约 14KB/s),加大后显著变快。
// 缓冲是静态的:抓取串行(单网络任务),复用同一份还能省下任务栈。
#define NBA_READ_CHUNK 2048
static char s_read_buf[NBA_READ_CHUNK];
// 兜底上限:响应异常膨胀时主动断开,避免无限占用网络与 CPU。
#define NBA_MAX_RESPONSE_BYTES (512 * 1024)

// 单场球员数据。整份响应 ~450KB,但 boxscore 是文档的第一个键、只占前 ~25KB,
// 解析器读完两队名单就会置 complete,这里随即断开——剩下的 plays/胜率曲线不下。
#define NBA_SUMMARY_URL \
    "http://site.api.espn.com/apis/site/v2/sports/basketball/nba/summary"
#define NBA_BOX_MAX_RESPONSE_BYTES (256 * 1024)

static nba_parse_t s_parse;   // 抓取是串行的(单网络任务),复用同一份状态省内存
static nba_box_parse_t s_box_parse;

static void build_url(const nba_date_t *day, char *buf, size_t cap)
{
    char query[16];
    if (nba_format_day_query(day, query, sizeof(query)) == 0) {
        snprintf(buf, cap, "%s?limit=%d", NBA_SCOREBOARD_URL, NBA_MAX_GAMES);
        return;
    }
    snprintf(buf, cap, "%s?limit=%d&dates=%s", NBA_SCOREBOARD_URL, NBA_MAX_GAMES, query);
}

nba_fetch_result_t nba_api_fetch(nba_board_t *board, const nba_date_t *day,
                                 int timeout_ms)
{
    if (!board) return NBA_FETCH_HTTP_FAIL;

    char url[128];
    build_url(day, url, sizeof(url));

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = timeout_ms,
        .buffer_size = NBA_READ_CHUNK * 2,
        .buffer_size_tx = 256,
        .disable_auto_redirect = true,    // Portal 的 302 不跟随,见 http_event_cb
        .event_handler = http_event_cb,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "HTTP 客户端创建失败");
        return NBA_FETCH_NO_MEM;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "FoloToy-NBA-Board/1.0");

    nba_fetch_result_t result = NBA_FETCH_HTTP_FAIL;
    nba_parse_begin(&s_parse, board);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "连接比分接口失败: %s", esp_err_to_name(err));
        goto done;
    }

    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "比分接口返回 HTTP %d", status);
        result = s_portal_seen ? NBA_FETCH_PORTAL : NBA_FETCH_HTTP_FAIL;
        goto done;
    }

    char *buffer = s_read_buf;
    size_t total = 0;
    for (;;) {
        const int read = esp_http_client_read(client, buffer, sizeof(s_read_buf));
        if (read < 0) {
            ESP_LOGW(TAG, "读取响应失败");
            goto done;
        }
        if (read == 0) break;   // 传输结束
        total += (size_t)read;
        nba_parse_feed(&s_parse, buffer, (size_t)read);
        if (total > NBA_MAX_RESPONSE_BYTES) {
            ESP_LOGW(TAG, "响应超过 %d 字节,已中止", NBA_MAX_RESPONSE_BYTES);
            goto done;
        }
    }

    if (!nba_parse_ok(&s_parse)) {
        ESP_LOGW(TAG, "JSON 解析失败(%u 字节)", (unsigned)total);
        result = NBA_FETCH_PARSE_FAIL;
        goto done;
    }

    // 接口本身不带"这是哪一天"的字段:显式指定过日期就直接用,否则从首场比赛的
    // 开球时间反推(北京日期),让顶栏能显示当前看的是哪一天。
    if (nba_date_valid(day)) {
        board->day = *day;
    } else {
        (void)nba_board_day_from_games(board);
    }

    ESP_LOGI(TAG, "抓取成功: %u 字节, %u 场比赛", (unsigned)total, board->count);
    result = NBA_FETCH_OK;

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

bool nba_box_fetch(nba_box_t *box, const char *event_id, int timeout_ms)
{
    if (!box || !event_id || !event_id[0]) return false;

    char url[160];
    snprintf(url, sizeof(url), "%s?event=%s", NBA_SUMMARY_URL, event_id);

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = timeout_ms,
        .buffer_size = NBA_READ_CHUNK * 2,
        .buffer_size_tx = 256,
        .disable_auto_redirect = true,    // Portal 的 302 不跟随,见 http_event_cb
        .event_handler = http_event_cb,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "球员数据: HTTP 客户端创建失败");
        return false;
    }
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "User-Agent", "FoloToy-NBA-Board/1.0");

    bool ok = false;
    nba_box_parse_begin(&s_box_parse, box);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "连接球员接口失败: %s", esp_err_to_name(err));
        goto done;
    }

    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        ESP_LOGW(TAG, "球员接口返回 HTTP %d", status);
        goto done;
    }

    char *buffer = s_read_buf;
    size_t total = 0;
    for (;;) {
        const int read = esp_http_client_read(client, buffer, sizeof(s_read_buf));
        if (read < 0) {
            ESP_LOGW(TAG, "读取球员数据失败");
            goto done;
        }
        if (read == 0) break;   // 传输结束
        total += (size_t)read;
        nba_box_parse_feed(&s_box_parse, buffer, (size_t)read);
        if (nba_box_parse_complete(&s_box_parse)) break;   // 名单已到手,剩下的不用下
        if (total > NBA_BOX_MAX_RESPONSE_BYTES) {
            ESP_LOGW(TAG, "球员响应超过 %d 字节,已中止", NBA_BOX_MAX_RESPONSE_BYTES);
            goto done;
        }
    }

    ok = nba_box_parse_ok(&s_box_parse) && nba_box_fix_order(box, NULL);
    ESP_LOGI(TAG, "球员数据: %u 字节, %s", (unsigned)total,
             ok ? "已解析" : "未取到名单");

done:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}
