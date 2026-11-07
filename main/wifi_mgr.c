// main/wifi_mgr.c —— Wi-Fi STA 连接、凭据持久化(多网络)、SNTP 对时。
#include "wifi_mgr.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_mgr";

#define NVS_NAMESPACE "nba_cfg"
#define KEY_SSID "ssid"     // 旧版单网络遗留键:读到就迁移进槽位,迁移完即删
#define KEY_PASS "pass"
#define KEY_LAST "last"     // 上次连通的 SSID,启动时优先试它
#define SSID_MAX 33
#define PASS_MAX 65
#define MAX_RETRY 6

typedef struct {
    char ssid[SSID_MAX];
    char pass[PASS_MAX];
} net_cred_t;

// 凭据表:启动时从 NVS 整体载入,之后 RAM 是唯一事实源,写操作同步落盘。
// 槽位保持与保存顺序一致(forget 时整体前移),NVS 里也按同样布局重写。
static net_cred_t s_nets[WIFI_MGR_MAX_NETWORKS];
static int s_net_count;
static int s_active = -1;    // 当前正在连/已连上的槽位;-1 = 无
static int s_rot;            // wifi_mgr_start 的轮换计数:net_task 在 FAILED 后
                             // 反复调起,每次轮到下一个网络,连上后归零
static char s_last_good[SSID_MAX];   // 上次连通的网络;不在表里时忽略

static EventGroupHandle_t s_events;
#define BIT_CONNECTED (1u << 0)
#define BIT_FAILED    (1u << 1)

static wifi_mgr_state_t s_state = WIFI_MGR_NO_CREDS;
static int s_retries;
static SemaphoreHandle_t s_mutex;   // 保护 s_state/s_retries 与凭据表,跨任务访问
static volatile bool s_provisioning;   // 配网期间不自动重连,见 wifi_mgr_set_provisioning()
static volatile bool s_switching;      // apply_credentials 主动断开重连期间为真:
                                       // 这段窗口里的 DISCONNECTED 是我们自己造的,
                                       // 必须忽略,否则自动重连会把旧网络抢先连回去

static esp_netif_t *s_netif;

static void save_last_good(void);   // on_ip_event 在定义前就要用

static void lock(void)
{
    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
}

static void unlock(void)
{
    if (s_mutex) xSemaphoreGive(s_mutex);
}

static void set_state(wifi_mgr_state_t state)
{
    lock();
    s_state = state;
    unlock();
}

// SNTP 只配一台国内可达的服务器:sntp_config.servers[] 的长度由
// CONFIG_LWIP_SNTP_MAX_SERVERS(默认 1)决定,列多台会直接编译不过。这里只启动、
// 不等待——本函数运行在系统事件循环任务里,同步等待会拖住其它网络事件。
// 是否已对时由 wifi_mgr_time_synced() 按系统时间判断。
static void start_sntp(void)
{
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    if (esp_netif_sntp_init(&config) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP 启动失败,开赛时间将不可用");
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "已接入 AP,等待 DHCP");
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        // 配网或主动切换网络时,断开是预期行为,不走自动重连。
        if (s_provisioning || s_switching) return;
        const wifi_event_sta_disconnected_t *info =
            (const wifi_event_sta_disconnected_t *)data;
        lock();
        const int retries = ++s_retries;
        unlock();
        if (retries <= MAX_RETRY) {
            ESP_LOGW(TAG, "连接断开(reason=%d),第 %d/%d 次重试",
                     info ? info->reason : -1, retries, MAX_RETRY);
            esp_wifi_connect();
        } else {
            set_state(WIFI_MGR_FAILED);
            xEventGroupSetBits(s_events, BIT_FAILED);
            // 换下一个网络的轮换由 net_task 驱动:它看到 FAILED 会再次调
            // wifi_mgr_start(),那里 s_rot 已前移,自然轮到下一个候选。
        }
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != IP_EVENT_STA_GOT_IP) return;
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
    ESP_LOGI(TAG, "获取到 IP: " IPSTR, IP2STR(&event->ip_info.ip));
    lock();
    s_retries = 0;
    s_rot = 0;
    if (s_active >= 0 && s_active < s_net_count) {
        snprintf(s_last_good, sizeof(s_last_good), "%s", s_nets[s_active].ssid);
    }
    unlock();
    save_last_good();
    set_state(WIFI_MGR_CONNECTED);
    xEventGroupSetBits(s_events, BIT_CONNECTED);
    start_sntp();
}

static esp_err_t apply_credentials(const char *ssid, const char *password)
{
    // 已连接时绝不能直接 esp_wifi_connect():驱动走"内部先断开"的未定义路径,
    // 既不发 DISCONNECTED 事件、新连接也未必开始,状态机会永远卡在 CONNECTING
    // (真机复现过)。先显式断开,等事件走完,再下发新配置。
    s_switching = true;
    (void)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(120));   // 给驱动时间把断开事件分发完

    wifi_config_t config = {0};
    strncpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid) - 1);
    if (password && password[0]) {
        strncpy((char *)config.sta.password, password, sizeof(config.sta.password) - 1);
    }
    config.sta.threshold.authmode =
        (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    config.sta.pmf_cfg.capable = true;
    config.sta.pmf_cfg.required = false;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (err == ESP_OK) {
        lock();
        s_retries = 0;
        unlock();
        set_state(WIFI_MGR_CONNECTING);
        if (s_events) xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);
        s_switching = false;   // 必须在 connect 之前清零,之后的新断开才参与重试
        return esp_wifi_connect();
    }
    s_switching = false;
    return err;
}

// ---- 凭据表与 NVS ----------------------------------------------------------

static int find_slot(const char *ssid)
{
    for (int i = 0; i < s_net_count; i++) {
        if (strcmp(s_nets[i].ssid, ssid) == 0) return i;
    }
    return -1;
}

// 把第 i 个槽写进 NVS(调用方持有锁或处于单任务上下文)。
static esp_err_t write_slot(int i)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    char key[8];
    snprintf(key, sizeof(key), "n%dsid", i);
    err = nvs_set_str(h, key, s_nets[i].ssid);
    if (err == ESP_OK) {
        snprintf(key, sizeof(key), "n%dpass", i);
        err = nvs_set_str(h, key, s_nets[i].pass);
    }
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// 按当前 RAM 表全量重写 NVS 槽位(0..count-1 写入,其余擦除;顺带清掉旧版遗留键)。
static esp_err_t sync_nvs_slots(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    char key[8];
    for (int i = 0; i < WIFI_MGR_MAX_NETWORKS; i++) {
        snprintf(key, sizeof(key), "n%dsid", i);
        if (i < s_net_count) {
            err = nvs_set_str(h, key, s_nets[i].ssid);
            if (err == ESP_OK) {
                snprintf(key, sizeof(key), "n%dpass", i);
                err = nvs_set_str(h, key, s_nets[i].pass);
            }
        } else {
            err = nvs_erase_key(h, key);
            if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
            if (err == ESP_OK) {
                snprintf(key, sizeof(key), "n%dpass", i);
                err = nvs_erase_key(h, key);
                if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
            }
        }
        if (err != ESP_OK) break;
    }
    // 旧版单网络键:迁移完成后就没有存在价值了,顺手擦掉。
    if (err == ESP_OK) err = nvs_erase_key(h, KEY_SSID);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_erase_key(h, KEY_PASS);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void save_last_good(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = nvs_set_str(h, KEY_LAST, s_last_good);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) ESP_LOGW(TAG, "记录上次连通网络失败: %s", esp_err_to_name(err));
}

static void load_networks(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;

    for (int i = 0; i < WIFI_MGR_MAX_NETWORKS; i++) {
        char key[8];
        snprintf(key, sizeof(key), "n%dsid", i);
        size_t len = sizeof(s_nets[i].ssid);
        if (nvs_get_str(h, key, s_nets[i].ssid, &len) == ESP_OK && len > 1) {
            snprintf(key, sizeof(key), "n%dpass", i);
            len = sizeof(s_nets[i].pass);
            // 开放网络没有密码:读不到就当空串,不视为错误。
            if (nvs_get_str(h, key, s_nets[i].pass, &len) != ESP_OK) {
                s_nets[i].pass[0] = '\0';
            }
            s_net_count = i + 1;
        }
    }

    // 旧版只有一组 ssid/pass:迁移进槽位 0,下一次落盘时旧键会被擦掉。
    bool migrated = false;
    if (s_net_count == 0) {
        size_t len = sizeof(s_nets[0].ssid);
        if (nvs_get_str(h, KEY_SSID, s_nets[0].ssid, &len) == ESP_OK && len > 1) {
            len = sizeof(s_nets[0].pass);
            if (nvs_get_str(h, KEY_PASS, s_nets[0].pass, &len) != ESP_OK) {
                s_nets[0].pass[0] = '\0';
            }
            s_net_count = 1;
            migrated = true;
        }
    }

    size_t len = sizeof(s_last_good);
    if (nvs_get_str(h, KEY_LAST, s_last_good, &len) != ESP_OK) {
        s_last_good[0] = '\0';
    }
    nvs_close(h);

    if (migrated) {
        if (sync_nvs_slots() == ESP_OK) {
            ESP_LOGI(TAG, "已把旧版单 Wi-Fi 配置迁移到多网络槽位");
        }
    }
}

// 已在表里 → 只更新密码;不在 → 追加到第一个空位;满了 → 挤掉一个非当前连接的
// 槽(优先最早保存的)。返回该网络所在槽位。调用方需持有锁。
static int upsert_network(const char *ssid, const char *pass)
{
    int i = find_slot(ssid);
    if (i < 0) {
        if (s_net_count < WIFI_MGR_MAX_NETWORKS) {
            i = s_net_count++;
        } else {
            i = (s_active == 0) ? 1 : 0;
        }
    }
    snprintf(s_nets[i].ssid, sizeof(s_nets[i].ssid), "%s", ssid);
    snprintf(s_nets[i].pass, sizeof(s_nets[i].pass), "%s", pass ? pass : "");
    return i;
}

// 从表里按当前连接偏好挑一个候选:第一轮(s_rot==0)优先上次连通的,之后轮换。
// 调用方需持有锁;没有网络时返回 -1。
static int pick_candidate(void)
{
    if (s_net_count == 0) return -1;
    int order[WIFI_MGR_MAX_NETWORKS];
    int n = 0;
    const int lg = (s_last_good[0] != '\0') ? find_slot(s_last_good) : -1;
    if (lg >= 0) order[n++] = lg;
    for (int i = 0; i < s_net_count; i++) {
        if (i != lg) order[n++] = i;
    }
    return order[s_rot % n];
}

// ---- 对外接口 ---------------------------------------------------------------

esp_err_t wifi_mgr_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // 分区是全新/升级过的:NVS 必须重新格式化,不做静默降级。
        ESP_LOGW(TAG, "NVS 需要重新格式化(%s)", esp_err_to_name(err));
        if (nvs_flash_erase() != ESP_OK) {
            ESP_LOGE(TAG, "NVS 擦除失败");
            return ESP_FAIL;
        }
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS 初始化失败: %s", esp_err_to_name(err));
        return err;
    }

    s_mutex = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    if (!s_mutex || !s_events) return ESP_ERR_NO_MEM;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    if (!s_netif) return ESP_ERR_NO_MEM;
    esp_netif_set_hostname(s_netif, "nba-board");

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               on_ip_event, NULL));
    // 凭据在 NVS 里由本模块自己管理,不让 Wi-Fi 驱动再存一份,避免两份配置打架。
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    // 默认开 modem 省电(DTIM 休眠):看板绝大多数时间只是显示,射频常驻全速是
    // 整机功耗的最大头。HTTP 抓取那十几秒由 wifi_mgr_set_fast_mode(true) 临时
    // 切回 WIFI_PS_NONE,下载速度不受影响。
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_MIN_MODEM));

    load_networks();
    if (s_net_count > 0) {
        wifi_mgr_start();   // 内部从"上次连通的网络"开始试
        return ESP_OK;
    }
    set_state(WIFI_MGR_NO_CREDS);
    ESP_LOGW(TAG, "尚未配置 Wi-Fi:请在串口控制台执行 wifi <SSID> <密码>");
    return ESP_OK;
}

void wifi_mgr_start(void)
{
    if (wifi_mgr_state() == WIFI_MGR_CONNECTING) return;
    lock();
    const int idx = pick_candidate();
    if (idx < 0) {
        unlock();
        set_state(WIFI_MGR_NO_CREDS);
        return;
    }
    s_active = idx;
    s_rot++;
    const char *ssid = s_nets[idx].ssid;
    const char *pass = s_nets[idx].pass;
    unlock();
    ESP_LOGI(TAG, "尝试连接 %s(%d/%d)", ssid, idx + 1, s_net_count);
    (void)apply_credentials(ssid, pass);
}

wifi_mgr_state_t wifi_mgr_state(void)
{
    lock();
    const wifi_mgr_state_t state = s_state;
    unlock();
    return state;
}

bool wifi_mgr_connected(void)
{
    return wifi_mgr_state() == WIFI_MGR_CONNECTED;
}

bool wifi_mgr_time_synced(void)
{
    // 2020-01-01 之前视为"未对时":芯片上电从 1970 起走,SNTP 生效后才会越过这个门槛。
    return time(NULL) >= (time_t)1577836800;
}

bool wifi_mgr_has_credentials(void)
{
    lock();
    const bool found = s_net_count > 0;
    unlock();
    return found;
}

// 所有已存网络是否都试过且失败(FAILED 且轮换计数已覆盖全部槽位)。
// 供离线看门狗判断"是真的没网可连了",而不是轮换中途的短暂失败。
bool wifi_mgr_all_failed(void)
{
    lock();
    const bool all = s_net_count > 0 && s_state == WIFI_MGR_FAILED &&
                     s_rot >= s_net_count;
    unlock();
    return all;
}

void wifi_mgr_set_provisioning(bool on)
{
    s_provisioning = on;
}

void wifi_mgr_set_fast_mode(bool fast)
{
    // APSTA 配网期间 STA 正在被网页反复试连,别去动射频省电模式。
    if (s_provisioning) return;
    const wifi_ps_type_t ps = fast ? WIFI_PS_NONE : WIFI_PS_MIN_MODEM;
    const esp_err_t err = esp_wifi_set_ps(ps);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "切换 Wi-Fi 省电模式失败: %s", esp_err_to_name(err));
    }
}

int wifi_mgr_network_count(void)
{
    lock();
    const int count = s_net_count;
    unlock();
    return count;
}

const char *wifi_mgr_network_ssid(int index)
{
    if (index < 0 || index >= WIFI_MGR_MAX_NETWORKS) return NULL;
    lock();
    const char *ssid = (index < s_net_count) ? s_nets[index].ssid : NULL;
    unlock();
    return ssid;
}

esp_err_t wifi_mgr_forget_network(const char *ssid)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    lock();
    const int i = find_slot(ssid);
    if (i < 0) {
        unlock();
        return ESP_ERR_NOT_FOUND;
    }
    const bool was_active = (i == s_active);
    const bool was_last = (strcmp(s_last_good, ssid) == 0);
    for (int j = i; j < s_net_count - 1; j++) {
        s_nets[j] = s_nets[j + 1];
    }
    s_net_count--;
    s_active = -1;
    if (was_last) s_last_good[0] = '\0';
    unlock();

    const esp_err_t err = sync_nvs_slots();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "忘掉 %s 后落盘失败: %s", ssid, esp_err_to_name(err));
    }

    if (was_active && s_net_count > 0) {
        wifi_mgr_start();   // 断开当前连接,换下一个已存网络
    } else if (was_active) {
        esp_wifi_disconnect();
        set_state(WIFI_MGR_NO_CREDS);
    }
    return err;
}

esp_err_t wifi_mgr_try_credentials(const char *ssid, const char *password,
                                   uint32_t timeout_ms)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!s_events) return ESP_ERR_INVALID_STATE;

    // 先记下现有配置:试连失败要原样还原,否则设备会停在"新配置连不上、
    // 旧配置也没了"的中间状态。
    wifi_config_t backup = {0};
    (void)esp_wifi_get_config(WIFI_IF_STA, &backup);

    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);
    esp_err_t err = apply_credentials(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "试连配置下发失败: %s", esp_err_to_name(err));
        return err;
    }

    // 密码错时事件回调会在 6 次重试后提前置位 FAILED,不必白等到超时。
    const EventBits_t bits = xEventGroupWaitBits(
        s_events, BIT_CONNECTED | BIT_FAILED, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    if ((bits & BIT_CONNECTED) && wifi_mgr_state() == WIFI_MGR_CONNECTED) {
        lock();
        s_active = upsert_network(ssid, password);
        s_rot = 0;
        snprintf(s_last_good, sizeof(s_last_good), "%s", ssid);
        unlock();
        save_last_good();
        return sync_nvs_slots();
    }

    ESP_LOGW(TAG, "试连失败,还原原有凭据");
    esp_wifi_disconnect();
    if (backup.sta.ssid[0] != '\0') {
        (void)esp_wifi_set_config(WIFI_IF_STA, &backup);
    }
    set_state(WIFI_MGR_FAILED);
    return ESP_FAIL;
}

esp_err_t wifi_mgr_save_credentials(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0') return ESP_ERR_INVALID_ARG;

    lock();
    s_active = upsert_network(ssid, password);
    s_rot = 0;
    // 注意:这里不写 s_last_good——它只记录"真正连通过"的网络,由 GOT_IP /
    // 试连成功时更新。否则一个手打错字的网络会抢占轮换的优先位,连累其它
    // 已存网络多等一整轮失败。
    const char *p = s_nets[s_active].pass;
    unlock();

    const esp_err_t err = sync_nvs_slots();
    if (err != ESP_OK) return err;
    return apply_credentials(ssid, p);
}

esp_err_t wifi_mgr_clear_credentials(void)
{
    lock();
    s_net_count = 0;
    s_active = -1;
    s_last_good[0] = '\0';
    s_rot = 0;
    unlock();
    const esp_err_t err = sync_nvs_slots();
    if (err == ESP_OK) set_state(WIFI_MGR_NO_CREDS);
    return err;
}
