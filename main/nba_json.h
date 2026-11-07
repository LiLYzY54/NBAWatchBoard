// main/nba_json.h —— 增量 JSON 词法分析器(流式,无堆分配)。
//
// ESPN 的记分接口单次响应 70~240KB,而本板没有 PSRAM,不能整包缓存在内存里。
// 因此这里只做"分词":把 HTTP 分片喂进来,逐个吐出 token,由 nba_parse.c 决定
// 哪些字段要留下。解析器状态可跨分片保存,测试里用任意切分位置喂同一份 JSON
// 必须得到相同结果。
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    NBA_JSON_OBJECT_BEGIN,
    NBA_JSON_OBJECT_END,
    NBA_JSON_ARRAY_BEGIN,
    NBA_JSON_ARRAY_END,
    NBA_JSON_KEY,       // 对象成员名(已去引号、已解转义)
    NBA_JSON_STRING,    // 字符串值
    NBA_JSON_NUMBER,    // 数字,保留原始文本(整数/小数一致处理)
    NBA_JSON_TRUE,
    NBA_JSON_FALSE,
    NBA_JSON_NULL,
} nba_json_token_t;

// text/len 对该 token 是否有意义:结构 token 与 true/false/null 传 NULL/0。
typedef void (*nba_json_sink_t)(void *ctx, nba_json_token_t token,
                                const char *text, size_t len);

// 单个 token 文本上限。看板关心的字段(队名缩写、时间、状态)都远小于这个值;
// 超长文本(如 URL)会被截断,超出的部分不回传但也不影响后续解析。
#define NBA_JSON_TEXT_MAX 64
// JSON 容器栈深度。ESPN 记分响应最深约 10 层,取 24 留余量;
// 溢出视为语法错误而不是继续猜测层次。栈放在解析器实例里,便于并行/嵌套使用。
#define NBA_JSON_STACK_MAX 24

typedef struct {
    nba_json_sink_t sink;
    void *ctx;
    uint8_t stack[NBA_JSON_STACK_MAX];
    uint8_t depth;
    uint8_t state;
    uint8_t unicode_left;   // \uXXXX 还差几位十六进制
    uint16_t unicode_acc;
    char text[NBA_JSON_TEXT_MAX];
    size_t len;
    bool truncated;
    bool failed;
    bool string_is_key;     // 当前字符串是对象成员名
} nba_json_t;

void nba_json_init(nba_json_t *parser, nba_json_sink_t sink, void *ctx);

// 喂入任意长度分片;内部按字节推进状态机。
void nba_json_feed(nba_json_t *parser, const char *data, size_t len);

// 解析是否已因语法错误放弃(调用方应停止喂入并宣告本次抓取失败)。
bool nba_json_failed(const nba_json_t *parser);
