#include <string.h>
#include <stdlib.h>
#include "plan_text.h"

static size_t skip_ws(const char *p, size_t i, size_t len)
{
    while (i < len && (p[i] == ' ' || p[i] == '\t' || p[i] == '\r' || p[i] == '\n')) i++;
    return i;
}

static size_t string_len(const char *p, size_t len)
{
    // p[0] == '"'
    for (size_t i = 1; i < len; i++) {
        if (p[i] == '\\') { i++; continue; }
        if (p[i] == '"') return i + 1;
    }
    return 0;
}

size_t jtext_value_len(const char *p, size_t len)
{
    if (!len) return 0;
    if (p[0] == '"') return string_len(p, len);
    if (p[0] == '{' || p[0] == '[') {
        int depth = 0;
        for (size_t i = 0; i < len; i++) {
            char c = p[i];
            if (c == '"') {
                size_t sl = string_len(p + i, len - i);
                if (!sl) return 0;
                i += sl - 1;
            } else if (c == '{' || c == '[') {
                depth++;
            } else if (c == '}' || c == ']') {
                if (--depth == 0) return i + 1;
            }
        }
        return 0;
    }
    // Number or literal: up to the next delimiter.
    size_t i = 0;
    while (i < len && p[i] != ',' && p[i] != '}' && p[i] != ']' && p[i] != ' ' &&
           p[i] != '\r' && p[i] != '\n' && p[i] != '\t') i++;
    return i;
}

bool jtext_find(const char *obj, size_t len, const char *key, const char **val, size_t *vlen)
{
    if (!obj || len < 2 || obj[0] != '{') return false;
    size_t kl = strlen(key);
    size_t i = 1;
    for (;;) {
        i = skip_ws(obj, i, len);
        if (i >= len || obj[i] == '}') return false;
        if (obj[i] != '"') return false;
        size_t sl = string_len(obj + i, len - i);
        if (!sl) return false;
        bool match = (sl == kl + 2) && memcmp(obj + i + 1, key, kl) == 0;
        i = skip_ws(obj, i + sl, len);
        if (i >= len || obj[i] != ':') return false;
        i = skip_ws(obj, i + 1, len);
        size_t vl = jtext_value_len(obj + i, len - i);
        if (!vl) return false;
        if (match) {
            *val = obj + i;
            *vlen = vl;
            return true;
        }
        i = skip_ws(obj, i + vl, len);
        if (i < len && obj[i] == ',') i++;
    }
}

long jtext_int(const char *obj, size_t len, const char *key, long def)
{
    const char *v;
    size_t vl;
    if (!jtext_find(obj, len, key, &v, &vl) || !vl) return def;
    char buf[24];
    if (vl >= sizeof(buf)) vl = sizeof(buf) - 1;
    memcpy(buf, v, vl);
    buf[vl] = '\0';
    char *end;
    double d = strtod(buf, &end);
    return end == buf ? def : (long)d;
}

bool jtext_plan_in_frame(const char *frame, size_t len, const char **plan, size_t *plen)
{
    // Skip leading whitespace to the frame's own '{'.
    size_t i = skip_ws(frame, 0, len);
    if (i >= len || frame[i] != '{') return false;
    const char *root = frame + i;
    size_t rl = len - i;
    const char *holder;
    size_t hl;
    if (!jtext_find(root, rl, "data", &holder, &hl) &&
        !jtext_find(root, rl, "params", &holder, &hl)) return false;
    if (hl < 2 || holder[0] != '{') return false;
    if (!jtext_find(holder, hl, "plan", plan, plen)) return false;
    return *plen >= 2 && (*plan)[0] == '{';
}
