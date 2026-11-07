// main/nba_prov.c —— 手机配网:临时热点 + 一个单页配置界面。
//
// 设计取舍见 nba_prov.h 的头注释(为什么不用 BLE)。这里补两条实现上的决定:
//
// 1) 先试连、后落盘。配网页收到的 SSID/密码不直接写 NVS,而是切到 APSTA 用
//    STA 接口真连一次,拿到 IP 才保存。用户手输密码打错一个字符是常态,若先
//    存后试,一次错字就把设备锁死在"新配置连不上、旧配置也没了"的状态。
// 2) 配网期间关掉 wifi_mgr 的自动重连。APSTA 下 STA 取不到目标网络会不停
//    断开重连,把射频和事件循环占住,还会和这里的试连抢同一个 STA 接口。
#include "nba_prov.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "wifi_mgr.h"

static const char *TAG = "nba_prov";

#define AP_SSID      "NBA-Board"
#define AP_PASSWORD  ""          // 开放热点:配网窗口只有几分钟,密码反而挡自己
#define AP_CHANNEL   6
#define AP_MAX_STA   2
#define AP_URL       "192.168.4.1"

#define PROV_CONNECT_TIMEOUT_MS 20000
#define FORM_MAX_BYTES          256
#define SCAN_MAX_APS            8
#define SCAN_ITEM_BYTES         100     // 32 字节 SSID + 逗号引号 + 转义余量

static httpd_handle_t s_server;
static esp_netif_t *s_ap_netif;
static bool s_running;
static bool s_last_ok;
static nba_prov_done_cb_t s_done_cb;

// ---- 页面 ---------------------------------------------------------------
//
// 内联在固件里(约 1.8 KB,放 flash 的 .rodata,不占 DRAM)。不引外部资源:
// 手机连着的是没有上行网络的热点,任何 CDN 链接都只会转圈。
//
// 属性一律用单引号,JS 里用反引号,这样 C 字符串里几乎不需要转义双引号。

static const char kIndexHtml[] =
"<!DOCTYPE html><html lang='zh-CN'><head>"
"<meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>NBA 看板 · 配网</title><style>"
":root{color-scheme:dark}"
"*{box-sizing:border-box}"
"body{margin:0;padding:26px 18px;background:#070a10;color:#f2f6fb;"
"font:15px/1.6 -apple-system,BlinkMacSystemFont,'PingFang SC','Helvetica Neue',sans-serif}"
".card{max-width:380px;margin:0 auto}"
"h1{margin:0 0 6px;font-size:19px;font-weight:600}"
".sub{margin:0 0 24px;color:#8fa0b5;font-size:13px}"
"label{display:block;margin:16px 0 6px;color:#8fa0b5;font-size:13px}"
"input{width:100%;padding:12px 14px;border:1px solid #243244;border-radius:10px;"
"background:#121a25;color:#f2f6fb;font-size:16px}"
"input:focus{outline:none;border-color:#3c6fb0}"
"button{width:100%;margin-top:22px;padding:13px;border:0;border-radius:10px;"
"background:#e8462f;color:#fff;font-size:16px;font-weight:600}"
"button:active{opacity:.85}"
".hint{margin-top:16px;color:#6b7c91;font-size:12px;line-height:1.5}"
"</style></head><body><div class='card'>"
"<h1>NBA 看板</h1>"
"<p class='sub'>配置 Wi-Fi 后即可显示实时比分</p>"
"<form method='POST' action='/save'>"
"<label for='s'>Wi-Fi 名称</label>"
"<input id='s' name='ssid' list='nets' required maxlength='32' "
"autocomplete='off' autocapitalize='off' spellcheck='false'>"
"<datalist id='nets'></datalist>"
"<label for='p'>密码</label>"
"<input id='p' name='pass' type='password' maxlength='63' autocomplete='off'>"
"<button type='submit'>连接</button></form>"
"<p class='hint' id='h'>设备会先用这组密码试连,成功才保存;连不上则保留原配置。</p>"
"</div><script>"
"fetch('/scan').then(r=>r.json()).then(a=>{"
"var d=document.getElementById('nets');"
"a.forEach(n=>{var o=document.createElement('option');o.value=n;d.appendChild(o)})"
"}).catch(()=>{});"
"document.querySelector('form').addEventListener('submit',()=>{"
"document.getElementById('h').textContent=`正在连接,最长约 20 秒…`})"
"</script></body></html>";

static const char kOkHtml[] =
"<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>已连接</title><style>"
":root{color-scheme:dark}"
"body{margin:0;padding:60px 24px;background:#070a10;color:#f2f6fb;text-align:center;"
"font:16px/1.7 -apple-system,BlinkMacSystemFont,'PingFang SC',sans-serif}"
".ok{font-size:44px;color:#6be07a;line-height:1}"
"p{margin:14px 0 0;color:#8fa0b5;font-size:14px}"
"</style></head><body><div class='ok'>&#10003;</div>"
"<p style='color:#f2f6fb;font-size:17px;margin-top:18px'>已连接,凭据已保存</p>"
"<p>设备正在切换到新的 Wi-Fi,这个热点即将关闭。</p>"
"</body></html>";

static const char kFailHtml[] =
"<!DOCTYPE html><html lang='zh-CN'><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>连接失败</title><style>"
":root{color-scheme:dark}"
"body{margin:0;padding:56px 24px;background:#070a10;color:#f2f6fb;text-align:center;"
"font:16px/1.7 -apple-system,BlinkMacSystemFont,'PingFang SC',sans-serif}"
".bad{font-size:44px;color:#ff5a4d;line-height:1}"
"p{margin:14px 0 0;color:#8fa0b5;font-size:14px}"
"a{display:inline-block;margin-top:26px;padding:12px 28px;border-radius:10px;"
"background:#121a25;color:#f2f6fb;text-decoration:none;font-size:15px}"
"</style></head><body><div class='bad'>&#10005;</div>"
"<p style='color:#f2f6fb;font-size:17px;margin-top:18px'>连接失败</p>"
"<p>密码不对,或者这个网络当前不可用。原来的配置没有被改动。</p>"
"<a href='/'>重新填写</a></body></html>";

// ---- 小工具 -------------------------------------------------------------

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 表单解码:percent-encoding + '+' 代表空格。超长一律截断,不做半截解析。
static void url_decode(const char *src, size_t len, char *dst, size_t cap)
{
    size_t out = 0;
    for (size_t i = 0; i < len && out + 1 < cap; i++) {
        if (src[i] == '+') {
            dst[out++] = ' ';
        } else if (src[i] == '%' && i + 2 < len) {
            const int hi = hex_val(src[i + 1]);
            const int lo = hex_val(src[i + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[out++] = (char)((hi << 4) | lo);
                i += 2;
            } else {
                dst[out++] = src[i];
            }
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

// 从 a=1&b=2 里取 key 的值。找不到时 dst 置空串,返回 false。
static bool form_get(const char *body, const char *key, char *dst, size_t cap)
{
    dst[0] = '\0';
    const size_t key_len = strlen(key);
    const char *p = body;
    while (*p) {
        const char *amp = strchr(p, '&');
        const size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        if (seg > key_len && strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            url_decode(p + key_len + 1, seg - key_len - 1, dst, cap);
            return dst[0] != '\0';
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

// JSON 字符串转义:SSID 可能含引号/反斜杠/中文。中文按 UTF-8 原样输出即可。
static void json_escape(const char *src, char *dst, size_t cap)
{
    size_t out = 0;
    for (const char *p = src; *p && out + 8 < cap; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') {
            dst[out++] = '\\';
            dst[out++] = (char)c;
        } else if (c < 0x20) {
            out += (size_t)snprintf(dst + out, cap - out, "\\u%04x", c);
        } else {
            dst[out++] = (char)c;
        }
    }
    dst[out] = '\0';
}

// ---- HTTP 处理 ----------------------------------------------------------

static esp_err_t index_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t scan_get(httpd_req_t *req)
{
    char body[SCAN_MAX_APS * SCAN_ITEM_BYTES + 8];
    size_t used = 0;
    body[used++] = '[';

    // 阻塞扫描约 2~3 秒。APSTA 下扫描会让热点短暂停播,已连上的手机会卡一下,
    // 所以只在页面主动请求时才扫,不后台轮询。
    wifi_scan_config_t cfg = { .show_hidden = false };
    uint16_t count = 0;
    if (esp_wifi_scan_start(&cfg, true) == ESP_OK) {
        esp_wifi_scan_get_ap_num(&count);
        if (count > SCAN_MAX_APS) count = SCAN_MAX_APS;
    }

    if (count > 0) {
        wifi_ap_record_t *recs = calloc(count, sizeof(wifi_ap_record_t));
        if (recs) {
            uint16_t got = count;
            if (esp_wifi_scan_get_ap_records(&got, recs) == ESP_OK) {
                for (uint16_t i = 0; i < got; i++) {
                    if (recs[i].ssid[0] == '\0') continue;
                    // 先确认整项放得下再写:snprintf 空间不足时会写半截,
                    // 那样 break 出去得到的是残缺 JSON,前端 JSON.parse 直接抛错。
                    if (used + SCAN_ITEM_BYTES + 2 > sizeof(body)) break;
                    char name[33 * 6 + 1];
                    json_escape((const char *)recs[i].ssid, name, sizeof(name));
                    const int n = snprintf(body + used, sizeof(body) - used,
                                           "%s\"%s\"", used > 1 ? "," : "", name);
                    if (n < 0) break;
                    used += (size_t)n;
                }
            }
            free(recs);
        }
    } else {
        (void)esp_wifi_scan_get_ap_num(&count);   // 清掉扫描结果,避免下次误用
    }

    body[used++] = ']';
    body[used] = '\0';

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, used);
}

static esp_err_t save_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > FORM_MAX_BYTES) {
        // esp_http_server 的错误码表里没有 413,只能自己设状态行。
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "form too large");
    }

    char body[FORM_MAX_BYTES + 1];
    int received = 0;
    int timeouts = 0;
    while (received < req->content_len) {
        const int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) continue;
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "recv failed");
            return ESP_OK;
        }
        received += n;
    }
    body[received] = '\0';

    char ssid[33];
    char pass[65];
    const bool has_ssid = form_get(body, "ssid", ssid, sizeof(ssid));
    (void)form_get(body, "pass", pass, sizeof(pass));   // 开放网络可无密码

    if (!has_ssid) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "收到配网请求:ssid=\"%s\" 密码%s", ssid, pass[0] ? "已填" : "为空");
    const esp_err_t err =
        wifi_mgr_try_credentials(ssid, pass, PROV_CONNECT_TIMEOUT_MS);
    s_last_ok = (err == ESP_OK);

    if (s_last_ok) {
        ESP_LOGI(TAG, "配网成功,已保存并连通");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        (void)httpd_resp_send(req, kOkHtml, HTTPD_RESP_USE_STRLEN);
    } else {
        ESP_LOGW(TAG, "配网失败: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/html; charset=utf-8");
        (void)httpd_resp_send(req, kFailHtml, HTTPD_RESP_USE_STRLEN);
    }

    // 响应已经写回手机,再通知应用层收尾(关热点、切回看板)。
    if (s_done_cb) s_done_cb(s_last_ok);
    return ESP_OK;
}

// ---- 生命周期 -----------------------------------------------------------

esp_err_t nba_prov_start(void)
{
    if (s_running) return ESP_OK;

    // 先抑制自动重连,再切模式:切换会引起 STA 断开,顺序反了会先触发一轮重连。
    wifi_mgr_set_provisioning(true);

    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "切换 APSTA 失败: %s", esp_err_to_name(err));
        wifi_mgr_set_provisioning(false);
        return err;
    }

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (!s_ap_netif) {
            ESP_LOGE(TAG, "创建 AP netif 失败");
            wifi_mgr_set_provisioning(false);
            return ESP_FAIL;
        }
    }

    wifi_config_t ap = {0};
    strncpy((char *)ap.ap.ssid, AP_SSID, sizeof(ap.ap.ssid) - 1);
    ap.ap.ssid_len = (uint8_t)strlen(AP_SSID);
    ap.ap.channel = AP_CHANNEL;
    ap.ap.max_connection = AP_MAX_STA;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AP 配置失败: %s", esp_err_to_name(err));
        wifi_mgr_set_provisioning(false);
        return err;
    }

    // 清空 STA 侧配置:否则会拿着上次的旧凭据对着一个根本不在的路由器反复重连。
    wifi_config_t sta_empty = {0};
    (void)esp_wifi_set_config(WIFI_IF_STA, &sta_empty);

    // 模式与配置都改完之后才真正打开发射频:APSTA 下只改配置不开机,手机搜不到
    // 这个热点。wifi_mgr_init 通常已经 start 过,这里是幂等兜底。
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi 启动失败: %s", esp_err_to_name(err));
        httpd_stop(s_server);
        s_server = NULL;
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        wifi_mgr_set_provisioning(false);
        return err;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 4;
    cfg.max_open_sockets = 2;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;          // 试连路径里有 NVS 与 20 秒等待,给足栈
    cfg.server_port = 80;
    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP 服务启动失败");
        s_server = NULL;
        wifi_mgr_set_provisioning(false);
        return ESP_FAIL;
    }

    const httpd_uri_t routes[] = {
        { .uri = "/",      .method = HTTP_GET,  .handler = index_get },
        { .uri = "/scan",  .method = HTTP_GET,  .handler = scan_get },
        { .uri = "/save",  .method = HTTP_POST, .handler = save_post },
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        if (httpd_register_uri_handler(s_server, &routes[i]) != ESP_OK) {
            ESP_LOGE(TAG, "注册路由失败: %s", routes[i].uri);
        }
    }

    s_running = true;
    ESP_LOGI(TAG, "配网热点已开启:SSID=%s 地址=http://%s", AP_SSID, AP_URL);
    return ESP_OK;
}

void nba_prov_stop(void)
{
    if (!s_running) return;

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
    (void)esp_wifi_set_mode(WIFI_MODE_STA);
    // 切回 STA 之后要保证射频还开着:net_task 靠 wifi_mgr_start() 重连,
    // 而那条路径假定 Wi-Fi 已经 start。重复调用是幂等的。
    (void)esp_wifi_start();
    if (s_ap_netif) {
        esp_netif_destroy_default_wifi(s_ap_netif);
        s_ap_netif = NULL;
    }
    s_running = false;
    // 最后才恢复自动重连:顺序反了,STA 会在 AP 还没关掉时就开始连。
    wifi_mgr_set_provisioning(false);
    ESP_LOGI(TAG, "配网热点已关闭");
}

bool nba_prov_running(void)
{
    return s_running;
}

const char *nba_prov_ap_ssid(void)
{
    return AP_SSID;
}

const char *nba_prov_ap_url(void)
{
    return AP_URL;
}

void nba_prov_set_done_cb(nba_prov_done_cb_t cb)
{
    s_done_cb = cb;
}

bool nba_prov_last_ok(void)
{
    return s_last_ok;
}
