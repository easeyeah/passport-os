// 相册在线客户端:纯在线模式,设备不落盘。
// 清单(manifest.json)拉取后常驻堆中作为目录(名字就地 NUL 化当索引用),
// 看图时按 index 现场开 HTTP 流,由应用层把 TJpgDec 解码器接在流上。
// 内存纪律:清单池按档位堆分配(16K/12K/8K/4K,堆紧张自动降档,池满按
// 前缀截断),首次刷新后收紧到实际用量长期持有(即相册工作集)。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_http_client.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ALBUM_SYNC_IDLE = 0,
    ALBUM_SYNC_RUNNING,
    ALBUM_SYNC_OK,          // 清单已拉取,可浏览
    ALBUM_SYNC_NO_URL,      // 尚未配置服务器地址
    ALBUM_SYNC_NO_WIFI,     // 设备未联网
    ALBUM_SYNC_NET_FAIL,    // 清单拉取失败
    ALBUM_SYNC_BAD_MANIFEST,// manifest 缺字段或超限
} album_sync_status_t;

void album_sync_init(void); // 读 NVS 里的 url,幂等

const char *album_sync_url(void); // 空串 = 未配置
esp_err_t album_sync_set_url(const char *url); // http(s):// 开头或空,≤127 字节

album_sync_status_t album_sync_status(void);
const char *album_sync_detail(void); // 最近一次动作的逐阶段诊断(状态屏显示)

// 拉取/刷新清单(独立任务)。运行中重复调用返回 false。
bool album_sync_refresh_start(void);
// 在线看图全程需要网络;离开应用时调用以打断刷新。
void album_sync_cancel(const char *who);

// 清除取消标志:应用内层级切换(网格↔状态页↔看图)后重用。取消标志是
// 粘滞的(只在退出路径设置),不清零会让后续所有拉图秒失败(黑屏元凶)。
void album_sync_cancel_reset(void);

// 浏览接口(清单 OK 后可用;零拷贝,LVGL 任务安全)
int album_sync_count(void);

// 拉取第 index 张原图到堆缓冲(上限 18KB;调用方负责 free)。
// 失败返回 false 并更新 detail。设备不落盘,仅驻留内存。
bool album_sync_fetch_photo(int index, uint8_t **out_buf, size_t *out_len);

// 拉取第 index 张的服务端缩略图(thumbs/ 目录,60×70 JPEG,约 2KB;调用方
// 负责 free)。旧相册源没有缩略图时返回 false(不写 detail,探测性请求),
// 调用方回退拉原图。
bool album_sync_fetch_thumb(int index, uint8_t **out_buf, size_t *out_len);

// 网格总览由应用层逐格拉原图解码生成;服务端只需静态提供原图与清单。


#ifdef __cplusplus
}
#endif
