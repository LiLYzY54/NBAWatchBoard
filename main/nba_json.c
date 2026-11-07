// main/nba_json.c —— 增量 JSON 词法状态机实现。见 nba_json.h 的设计说明。
#include "nba_json.h"

#include <string.h>

enum {
    ST_VALUE = 0,     // 期待一个值,或空容器的结束符
    ST_OBJECT_KEY,    // 期待成员名或 '}'
    ST_COLON,         // 期待 ':'
    ST_STRING,        // 字符串内部
    ST_STRING_ESC,    // 字符串内刚读到反斜杠
    ST_UNICODE,       // \uXXXX 的十六进制位
    ST_NUMBER,        // 数字内部
    ST_LITERAL,       // true/false/null 内部
    ST_AFTER,         // 期待 ',' 或容器结束
    ST_ERROR,
};

static void emit(nba_json_t *p, nba_json_token_t token, const char *text, size_t len)
{
    if (p->sink) p->sink(p->ctx, token, text, len);
}

static void fail(nba_json_t *p)
{
    p->state = ST_ERROR;
    p->failed = true;
}

static void text_reset(nba_json_t *p)
{
    p->len = 0;
    p->truncated = false;
}

// 追加一个 UTF-8 码点。超出 NBA_JSON_TEXT_MAX 的部分丢弃,但保持 truncated 标记,
// 让上层知道"这个字段被截断了"而不是"它本来就短"。
static void text_push_cp(nba_json_t *p, uint32_t cp)
{
    char bytes[4];
    size_t n = 0;
    if (cp < 0x80) {
        bytes[n++] = (char)cp;
    } else if (cp < 0x800) {
        bytes[n++] = (char)(0xC0 | (cp >> 6));
        bytes[n++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        bytes[n++] = (char)(0xE0 | (cp >> 12));
        bytes[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        bytes[n++] = (char)(0xF0 | (cp >> 18));
        bytes[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        bytes[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        bytes[n++] = (char)(0x80 | (cp & 0x3F));
    }
    for (size_t i = 0; i < n; i++) {
        if (p->len + 1 >= NBA_JSON_TEXT_MAX) {
            p->truncated = true;
            return;
        }
        p->text[p->len++] = bytes[i];
    }
}

static void text_push_char(nba_json_t *p, char c)
{
    if (p->len + 1 >= NBA_JSON_TEXT_MAX) {
        p->truncated = true;
        return;
    }
    p->text[p->len++] = c;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static bool is_number_char(char c)
{
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

static void push_container(nba_json_t *p, uint8_t kind, nba_json_token_t token)
{
    if (p->depth >= NBA_JSON_STACK_MAX) {
        fail(p);
        return;
    }
    p->stack[p->depth++] = kind;
    emit(p, token, NULL, 0);
    p->state = (kind == '{') ? ST_OBJECT_KEY : ST_VALUE;
}

static void finish_scalar(nba_json_t *p, nba_json_token_t token)
{
    p->text[p->len] = '\0';
    emit(p, token, p->text, p->len);
    p->state = ST_AFTER;
}

void nba_json_init(nba_json_t *parser, nba_json_sink_t sink, void *ctx)
{
    memset(parser, 0, sizeof(*parser));
    parser->sink = sink;
    parser->ctx = ctx;
    parser->state = ST_VALUE;
    parser->string_is_key = false;
    parser->depth = 0;
}

bool nba_json_failed(const nba_json_t *parser)
{
    return parser->failed;
}

void nba_json_feed(nba_json_t *parser, const char *data, size_t len)
{
    nba_json_t *p = parser;
    size_t i = 0;

    while (i < len && p->state != ST_ERROR) {
        const char c = data[i];
        bool consume = true;

        switch (p->state) {
        case ST_VALUE:
            if (is_ws(c)) break;
            if (c == '{') {
                push_container(p, '{', NBA_JSON_OBJECT_BEGIN);
            } else if (c == '[') {
                push_container(p, '[', NBA_JSON_ARRAY_BEGIN);
            } else if (c == '}') {
                if (p->depth == 0 || p->stack[p->depth - 1] != '{') { fail(p); break; }
                p->depth--;
                emit(p, NBA_JSON_OBJECT_END, NULL, 0);
                p->state = ST_AFTER;
            } else if (c == ']') {
                if (p->depth == 0 || p->stack[p->depth - 1] != '[') { fail(p); break; }
                p->depth--;
                emit(p, NBA_JSON_ARRAY_END, NULL, 0);
                p->state = ST_AFTER;
            } else if (c == '"') {
                text_reset(p);
                p->string_is_key = false;
                p->state = ST_STRING;
            } else if (c == 't' || c == 'f' || c == 'n') {
                text_reset(p);
                text_push_char(p, c);
                p->state = ST_LITERAL;
            } else if (is_number_char(c)) {
                text_reset(p);
                text_push_char(p, c);
                p->state = ST_NUMBER;
            } else {
                fail(p);
            }
            break;

        case ST_OBJECT_KEY:
            if (is_ws(c)) break;
            if (c == '}') {
                p->depth--;
                emit(p, NBA_JSON_OBJECT_END, NULL, 0);
                p->state = ST_AFTER;
            } else if (c == '"') {
                text_reset(p);
                p->string_is_key = true;
                p->state = ST_STRING;
            } else {
                fail(p);
            }
            break;

        case ST_COLON:
            if (is_ws(c)) break;
            if (c != ':') { fail(p); break; }
            p->state = ST_VALUE;
            break;

        case ST_STRING:
            if (c == '"') {
                if (p->string_is_key) {
                    finish_scalar(p, NBA_JSON_KEY);
                    p->state = ST_COLON;
                } else {
                    finish_scalar(p, NBA_JSON_STRING);
                }
            } else if (c == '\\') {
                p->state = ST_STRING_ESC;
            } else {
                text_push_char(p, c);
            }
            break;

        case ST_STRING_ESC:
            switch (c) {
            case '"': text_push_char(p, '"'); p->state = ST_STRING; break;
            case '\\': text_push_char(p, '\\'); p->state = ST_STRING; break;
            case '/': text_push_char(p, '/'); p->state = ST_STRING; break;
            case 'b': text_push_char(p, '\b'); p->state = ST_STRING; break;
            case 'f': text_push_char(p, '\f'); p->state = ST_STRING; break;
            case 'n': text_push_char(p, '\n'); p->state = ST_STRING; break;
            case 'r': text_push_char(p, '\r'); p->state = ST_STRING; break;
            case 't': text_push_char(p, '\t'); p->state = ST_STRING; break;
            case 'u':
                p->unicode_acc = 0;
                p->unicode_left = 4;
                p->state = ST_UNICODE;
                break;
            default: fail(p); break;
            }
            break;

        case ST_UNICODE: {
            const int v = hex_value(c);
            if (v < 0) { fail(p); break; }
            p->unicode_acc = (uint16_t)((p->unicode_acc << 4) | (uint16_t)v);
            if (--p->unicode_left == 0) {
                // 代理对(D800-DFFF)在这里不做配对还原:看板读取的字段都是 ASCII,
                // 真出现代理对时用 U+FFFD 代替,避免把半个字符塞进 UTF-8。
                uint32_t cp = p->unicode_acc;
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;
                text_push_cp(p, cp);
                p->state = ST_STRING;
            }
            break;
        }

        case ST_NUMBER:
            if (is_number_char(c)) {
                text_push_char(p, c);
            } else {
                finish_scalar(p, NBA_JSON_NUMBER);
                consume = false;   // 让分隔符在本状态外重新处理
            }
            break;

        case ST_LITERAL:
            // 只接受 "true"/"false"/"null" 三种前缀,长度到齐后按内容判类型。
            if ((c >= 'a' && c <= 'z') && p->len < 5) {
                text_push_char(p, c);
            } else {
                p->text[p->len] = '\0';
                if (strcmp(p->text, "true") == 0) {
                    emit(p, NBA_JSON_TRUE, NULL, 0);
                } else if (strcmp(p->text, "false") == 0) {
                    emit(p, NBA_JSON_FALSE, NULL, 0);
                } else if (strcmp(p->text, "null") == 0) {
                    emit(p, NBA_JSON_NULL, NULL, 0);
                } else {
                    fail(p);
                    break;
                }
                p->state = ST_AFTER;
                consume = false;
            }
            break;

        case ST_AFTER:
            if (is_ws(c)) break;
            if (c == ',') {
                if (p->depth == 0) { fail(p); break; }
                p->state = (p->stack[p->depth - 1] == '{') ? ST_OBJECT_KEY : ST_VALUE;
            } else if (c == '}') {
                if (p->depth == 0 || p->stack[p->depth - 1] != '{') { fail(p); break; }
                p->depth--;
                emit(p, NBA_JSON_OBJECT_END, NULL, 0);
            } else if (c == ']') {
                if (p->depth == 0 || p->stack[p->depth - 1] != '[') { fail(p); break; }
                p->depth--;
                emit(p, NBA_JSON_ARRAY_END, NULL, 0);
            } else {
                fail(p);
            }
            break;

        default:
            fail(p);
            break;
        }

        if (consume) i++;
    }
}
