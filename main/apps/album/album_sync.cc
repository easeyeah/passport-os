#include "apps/album/album_sync.h"

#include "wifi_manager.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_spiffs.h"
#include <dirent.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <new>
#include <stdarg.h>

static const char *TAG = "album_sync";

static std::atomic<album_sync_status_t> s_status{ALBUM_SYNC_IDLE};
static std::atomic<int> s_done{0};
static std::atomic<int> s_total{0};
static std::atomic<bool> s_cancel{false};
static char s_url[128];
static char s_detail[160]; // 最近一次动作的逐阶段诊断,状态屏直接展示

// 会话数据:清单缓冲即名字目录(名字就地 NUL 化),偏移表指向每个名字。
// 首次刷新成功后长期持有(即相册的工作集);放堆不放 BSS 是为了守住
// 固件的 BSS 硬预算(verify_firmware.py 会拒绝超额)。
static char *s_names_buf;
static uint16_t *s_name_off;
static int s_count;
// 清单池按档位取堆(名字均长 ~25B):堆富余时 16K≈收录 600 名,紧张时
// 依次降档 12K/8K,最差 4K 也不低于旧行为(≈150 名)。池满截断不再整单
// 失败——名字流式解析本就耐受截断,收录已完整的前缀即可。
static const size_t kPoolCaps[] = {16 * 1024, 12 * 1024, 8 * 1024, 4 * 1024};
static const int kMaxNames = 768;
static const int kNameMaxLen = 63; // 单个名字字节上限(不含收尾引号)
static const int kHttpTimeoutMs = 8000;

static void diag(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_detail, sizeof(s_detail), fmt, args);
    va_end(args);
}

void album_sync_init(void) {
    static bool done;
    if (done) return;
    done = true;
    nvs_handle_t handle;
    if (nvs_open("album", NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(s_url);
        if (nvs_get_str(handle, "url", s_url, &len) != ESP_OK) s_url[0] = '\0';
        nvs_close(handle);
    }
}

const char *album_sync_url(void) { return s_url; }

esp_err_t album_sync_set_url(const char *url) {
    if (!url) return ESP_ERR_INVALID_ARG;
    const size_t len = strlen(url);
    if (len >= sizeof(s_url)) return ESP_ERR_INVALID_ARG;
    if (len && !(!strncasecmp(url, "http://", 7) || !strncasecmp(url, "https://", 8)))
        return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("album", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "url", url);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    if (err == ESP_OK) {
        snprintf(s_url, sizeof(s_url), "%s", url);
        ESP_LOGI(TAG, "album url updated (%d bytes)", (int)len);
    }
    return err;
}

album_sync_status_t album_sync_status(void) { return s_status.load(); }

const char *album_sync_detail(void) { return s_detail; }

int album_sync_count(void) { return s_count; }

void album_sync_progress(int *done, int *total) {
    if (done) *done = s_done.load();
    if (total) *total = s_total.load();
}

static char s_cancel_source[20];
void album_sync_cancel(const char *who) {
    snprintf(s_cancel_source, sizeof(s_cancel_source), "%s", who ? who : "?");
    s_cancel = true;
}

void album_sync_cancel_reset(void) {
    s_cancel = false;
}

// 拼接 base + path;base 末尾多余的 '/' 去重;path 按 RFC 3986 百分号转义
// (照片文件名常含空格/中文,不转义会让 URL 解析直接失败);base 与 path
// 之间自动补分隔斜杠。
static bool build_url(const char *base, const char *path, size_t path_len,
                      char *out, size_t cap) {
    size_t base_len = strlen(base);
    while (base_len && base[base_len - 1] == '/') --base_len;
    if (base_len >= cap) return false;
    memcpy(out, base, base_len);
    size_t pos = base_len;
    if (base_len > 0 && (path_len == 0 || path[0] != '/')) {
        if (pos + 1 >= cap) return false;
        out[pos++] = '/';
    }
    for (size_t i = 0; i < path_len; ++i) {
        const uint8_t c = static_cast<uint8_t>(path[i]);
        const bool safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
            c == '~' || c == '/';
        if (pos + 4 >= cap) return false;
        if (safe) {
            out[pos++] = static_cast<char>(c);
        } else {
            snprintf(out + pos, 4, "%%%02X", c);
            pos += 3;
        }
    }
    out[pos] = '\0';
    return true;
}

static bool status_2xx(esp_http_client_handle_t client) {
    return esp_http_client_get_status_code(client) >= 200 &&
           esp_http_client_get_status_code(client) < 300;
}

// GET 到内存(清单用)。完整收完且 2xx 才算成功;池满时停止接收但仍算
// 成功(truncated=true),已收前缀的名字流式解析有效——收录多少算多少。
static bool http_get_mem(const char *url, char *buf, size_t cap, size_t *used_out,
                         bool *truncated_out) {
    if (s_cancel.load()) return false; // 应用退出:立即放弃
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 1024;
    config.buffer_size_tx = 512;
    config.user_agent = "AI-Passport-Album/0.1";
    config.keep_alive_enable = false;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 2;
    if (!strncasecmp(url, "https://", 8)) config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { snprintf(s_detail, sizeof(s_detail), "清单 client创建失败"); return false; }
    size_t used = 0;
    bool ok = false;
    bool truncated = false;
    do {
        if (esp_http_client_open(client, 0) != ESP_OK) { snprintf(s_detail, sizeof(s_detail), "清单连接失败"); break; }
        esp_http_client_fetch_headers(client);
        if (!status_2xx(client)) {
            snprintf(s_detail, sizeof(s_detail), "清单 HTTP %d",
                     esp_http_client_get_status_code(client));
            break;
        }
        ok = true;
        while (true) {
            if (s_cancel.load()) { ok = false; break; } // 退出打断
            if (used + 1 >= cap) { truncated = true; break; } // 池满:停收,前缀有效
            const int received = esp_http_client_read(client, buf + used,
                static_cast<int>(cap - used - 1));
            if (received < 0) { ok = false; break; }
            if (received == 0) { ok = esp_http_client_is_complete_data_received(client); break; }
            used += static_cast<size_t>(received);
        }
    } while (false);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    buf[used] = '\0';
    if (used_out) *used_out = used;
    if (truncated_out) *truncated_out = truncated;
    return ok;
}

// 清理缓存时代的落盘文件(/storage/album/ 下的哈希图与 state.json)。
// 纯在线模式不再写 SPIFFS;每次开机至多尝试一次,首页未挂载就下次再说。
static int s_legacy_cleaned = -1; // -1 = 尚未执行
static void cleanup_legacy_cache(void) {
    static bool done;
    if (done) return;
    done = true;
    s_legacy_cleaned = 0;
    if (!esp_spiffs_mounted("storage")) return;
    DIR *dir = opendir("/storage/album");
    if (!dir) return; // 目录不存在 = 从未落盘或已清理
    char path[96];
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        snprintf(path, sizeof(path), "/storage/album/%.64s", entry->d_name);
        if (unlink(path) == 0) ++s_legacy_cleaned;
    }
    closedir(dir);
    if (s_legacy_cleaned > 0) {
        ESP_LOGI(TAG, "legacy cache cleaned: %d files", s_legacy_cleaned);
    }
}

// 清单拉取任务:GET manifest → 名字就地 NUL 化建目录 → 状态 OK。
// 成功后清单缓冲与偏移表长期持有,作为浏览用的名字目录。
static void refresh_task(void *) {
    cleanup_legacy_cache();
    const char *base = s_url;
    if (!base[0]) { s_status = ALBUM_SYNC_NO_URL; vTaskDelete(nullptr); return; }
    if (!WifiManager::GetInstance().IsConnected()) { s_status = ALBUM_SYNC_NO_WIFI; vTaskDelete(nullptr); return; }

    // 清单池按档位取堆:堆富余时多收录,紧张时自动降档,最差 4K=旧行为。
    char *pool = nullptr;
    size_t pool_cap = 0;
    for (size_t i = 0; i < sizeof(kPoolCaps) / sizeof(kPoolCaps[0]); ++i) {
        pool = static_cast<char *>(malloc(kPoolCaps[i]));
        if (pool) { pool_cap = kPoolCaps[i]; break; }
    }
    if (!pool) { diag("清单缓冲分配失败"); s_status = ALBUM_SYNC_NET_FAIL; vTaskDelete(nullptr); return; }
    // malloc/new[] 分配器不可混用(-Werror=mismatched-new-delete),用 free 配对
    struct PoolFree { char *p; ~PoolFree() { free(p); } } pool_hold{pool};
    char manifest_url[192];
    if (!build_url(base, "/manifest.json", strlen("/manifest.json"),
                   manifest_url, sizeof(manifest_url))) {
        s_status = ALBUM_SYNC_NET_FAIL;
        vTaskDelete(nullptr);
        return;
    }
    size_t used = 0;
    bool truncated = false;
    if (!http_get_mem(manifest_url, pool, pool_cap, &used, &truncated)) {
        s_status = ALBUM_SYNC_NET_FAIL;
        vTaskDelete(nullptr);
        return;
    }
    // 流式扫描自家定义的小 JSON:{"rev": N, "photos": ["a.jpg", ...]}
    // json.dump 默认输出 ": "/" " 带空格,键名定位必须容忍空白。
    const char *rev_pos = strstr(pool, "\"rev\"");
    const char *arr_pos = strstr(pool, "\"photos\"");
    if (!rev_pos || !arr_pos) { diag("清单缺 rev/photos 字段"); s_status = ALBUM_SYNC_BAD_MANIFEST; vTaskDelete(nullptr); return; }
    rev_pos = strchr(rev_pos, ':');
    if (!rev_pos) { s_status = ALBUM_SYNC_BAD_MANIFEST; vTaskDelete(nullptr); return; }
    const long rev = strtol(rev_pos + 1, nullptr, 10);
    (void)rev;
    arr_pos = strchr(arr_pos, ':');
    while (arr_pos && *arr_pos && *arr_pos != '[') ++arr_pos;
    if (!arr_pos || *arr_pos != '[') { diag("photos 不是数组"); s_status = ALBUM_SYNC_BAD_MANIFEST; vTaskDelete(nullptr); return; }

    // 名字就地 NUL 化(收尾引号改 '\0'),偏移表放堆上收集(768×2B),
    // 不吃任务栈。池满截断处名字不完整,解析到不完整名自然停住。
    std::unique_ptr<uint16_t[]> offs(new (std::nothrow) uint16_t[kMaxNames]);
    if (!offs) { diag("偏移表分配失败"); s_status = ALBUM_SYNC_NET_FAIL; vTaskDelete(nullptr); return; }
    int count = 0;
    const char *p = arr_pos + 1;
    while (*p && *p != ']' && count < kMaxNames) {
        while (*p == ' ' || *p == ',') ++p;
        if (*p != '"') break; // ']' 或清单损坏
        ++p;
        char *name_start = pool + (p - pool);
        while (*p && *p != '"') ++p;
        if (*p != '"') break; // 清单截断
        char *name_end = pool + (p - pool);
        const int len = static_cast<int>(name_end - name_start);
        if (*p) ++p;
        if (len <= 0 || len > kNameMaxLen) continue;
        *name_end = '\0';
        offs[count] = static_cast<uint16_t>(name_start - pool);
        ++count;
    }
    if (count <= 0) { diag("清单内没有有效名字"); s_status = ALBUM_SYNC_BAD_MANIFEST; vTaskDelete(nullptr); return; }

    // 提交会话:池收紧到实际用量(名字原样拷贝,偏移相对基址不变),
    // 16K 档的富余块还回堆,长期持有的工作集只有真实大小。
    char *final_pool = static_cast<char *>(malloc(used + 1));
    std::unique_ptr<uint16_t[]> new_offs(new (std::nothrow) uint16_t[count]);
    if (!final_pool || !new_offs) {
        free(final_pool);
        diag("目录缓冲分配失败");
        s_status = ALBUM_SYNC_NET_FAIL;
        vTaskDelete(nullptr);
        return;
    }
    memcpy(final_pool, pool, used + 1);
    memcpy(new_offs.get(), offs.get(), sizeof(uint16_t) * static_cast<size_t>(count));
    free(s_name_off);
    s_name_off = new_offs.release();
    free(s_names_buf);
    s_names_buf = final_pool;
    s_count = count;
    s_done = count;
    s_total = count;
    if (truncated) {
        diag("收录前%d张 · 清单超池已截断", count);
    } else if (s_legacy_cleaned > 0) {
        diag("完成 %d张 · 已清理旧缓存%d个文件", count, s_legacy_cleaned);
    } else {
        diag("完成 %d张 · 在线模式", count);
    }
    s_status = ALBUM_SYNC_OK;
    vTaskDelete(nullptr);
}

bool album_sync_refresh_start(void) {
    if (!s_url[0]) { s_status = ALBUM_SYNC_NO_URL; return false; }
    // 上一次会话可能残留 RUNNING:退出时的 cancel 要等旧清单任务走出阻塞
    // 网络调用(最长 ~8s)才生效,期间 status 一直是 RUNNING。若见 RUNNING
    // 就直接拒绝(旧防御),重新进入后没有任何任务在拉清单——界面永远
    // "正在获取 0s"、堆高挂。正确做法:先叫停、给旧任务 ≤1s 退场,再开
    // 新任务;它实在没退完(还堵在系统调用里)本次拒绝,它退场时会置
    // NET_FAIL,下次进入即可正常刷新。
    s_cancel = true;
    for (int i = 0; i < 20 && s_status.load() == ALBUM_SYNC_RUNNING; ++i)
        vTaskDelay(pdMS_TO_TICKS(50));
    if (s_status.load() == ALBUM_SYNC_RUNNING) {
        // 旧任务仍堵在阻塞调用里:本次放弃,但**必须清掉粘滞的取消**——
        // 不清的话旧任务退出后,后续所有网格/看图拉图都会被秒拒(黑屏、
        // "正在获取"永久卡死的元凶)。旧任务走出阻塞后会自行完成/失败
        // 并更新状态,下次进入相册即可正常刷新。
        s_cancel = false;
        return false;
    }
    s_cancel = false;
    s_status = ALBUM_SYNC_RUNNING;
    if (xTaskCreate(refresh_task, "album_sync", 6144, nullptr, 4, nullptr) != pdPASS) {
        s_status = ALBUM_SYNC_NET_FAIL;
        return false;
    }
    return true;
}

// 公共:打开一个 GET 流(2xx、响应头已消费)。失败返回 NULL 并更新 detail。
static esp_http_client_handle_t open_get(const char *url, const char *what) {
    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 1024;
    config.buffer_size_tx = 512;
    config.user_agent = "AI-Passport-Album/0.1";
    config.keep_alive_enable = false;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 2;
    if (!strncasecmp(url, "https://", 8)) config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { snprintf(s_detail, sizeof(s_detail), "%s client创建失败", what); return nullptr; }
    if (esp_http_client_open(client, 0) != ESP_OK) {
        snprintf(s_detail, sizeof(s_detail), "%s 连接失败", what);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return nullptr;
    }
    esp_http_client_fetch_headers(client);
    if (!status_2xx(client)) {
        snprintf(s_detail, sizeof(s_detail), "%s HTTP %d",
                 what, esp_http_client_get_status_code(client));
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return nullptr;
    }
    return client;
}

// GET 到堆缓冲(照片/缩略图共用):按 caps 逐档尝试分配,完整收完且 2xx
// 才算成功。what 非空时失败原因写入 detail(状态屏可见);缩略图是探测性
// 请求,传 nullptr 不污染 detail。退出应用的 cancel 会打断长拉取。
static bool fetch_to_heap(const char *url, const size_t *caps, size_t n_caps,
                          const char *what, uint8_t **out_buf, size_t *out_len) {
    if (s_cancel.load()) return false; // 应用退出:立即放弃,别占着堆
    uint8_t *buf = nullptr;
    size_t cap = 0;
    for (size_t i = 0; i < n_caps; ++i) {
        buf = static_cast<uint8_t *>(malloc(caps[i]));
        if (buf) { cap = caps[i]; break; }
    }
    if (!buf) {
        if (what) snprintf(s_detail, sizeof(s_detail), "%s缓冲分配失败(堆不足)", what);
        return false;
    }

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = kHttpTimeoutMs;
    config.buffer_size = 1024;
    config.buffer_size_tx = 512;
    config.user_agent = "AI-Passport-Album/0.1";
    config.keep_alive_enable = false;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 2;
    if (!strncasecmp(url, "https://", 8)) config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    size_t used = 0;
    bool ok = false;
    do {
        if (!client) { if (what) snprintf(s_detail, sizeof(s_detail), "%s client创建失败", what); break; }
        if (esp_http_client_open(client, 0) != ESP_OK) { if (what) snprintf(s_detail, sizeof(s_detail), "%s 连接失败", what); break; }
        esp_http_client_fetch_headers(client);
        if (!status_2xx(client)) {
            if (what) snprintf(s_detail, sizeof(s_detail), "%s HTTP %d",
                               what, esp_http_client_get_status_code(client));
            break;
        }
        ok = true;
        while (true) {
            if (s_cancel.load()) { ok = false; break; } // 退出打断,不写 detail
            if (used >= cap) { if (what) snprintf(s_detail, sizeof(s_detail), "%s超过缓冲档位", what); ok = false; break; }
            const int n = esp_http_client_read(client, reinterpret_cast<char *>(buf + used),
                static_cast<int>(cap - used));
            if (n < 0) { if (what) snprintf(s_detail, sizeof(s_detail), "%s接收错误", what); ok = false; break; }
            if (n == 0) {
                ok = esp_http_client_is_complete_data_received(client);
                if (!ok && what) snprintf(s_detail, sizeof(s_detail), "%s连接提前断开", what);
                break;
            }
            used += static_cast<size_t>(n);
        }
    } while (false);
    if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
    if (!ok) { free(buf); return false; }
    *out_buf = buf;
    *out_len = used;
    return true;
}

bool album_sync_fetch_photo(int index, uint8_t **out_buf, size_t *out_len) {
    if (index < 0 || index >= s_count || !s_names_buf || !s_name_off) {
        snprintf(s_detail, sizeof(s_detail), "照片序号越界(%d/%d)", index, s_count);
        return false;
    }
    const char *name = s_names_buf + s_name_off[index];
    char url[224];
    if (!build_url(s_url, name, strlen(name), url, sizeof(url))) {
        snprintf(s_detail, sizeof(s_detail), "URL 过长:%.20s", name);
        return false;
    }
    static const size_t caps[] = {18 * 1024, 12 * 1024, 8 * 1024};
    return fetch_to_heap(url, caps, 3, "照片", out_buf, out_len);
}

bool album_sync_fetch_thumb(int index, uint8_t **out_buf, size_t *out_len) {
    if (index < 0 || index >= s_count || !s_names_buf || !s_name_off) return false;
    const char *name = s_names_buf + s_name_off[index];
    char path[96];
    snprintf(path, sizeof(path), "/thumbs/%s", name);
    char url[256];
    if (!build_url(s_url, path, strlen(path), url, sizeof(url))) return false;
    static const size_t caps[] = {8 * 1024, 4 * 1024};
    return fetch_to_heap(url, caps, 2, nullptr, out_buf, out_len);
}


