/* kn_json.c — minimal zero-allocation JSON parser (see kn_json.h).
 * Recursive descent with an explicit depth cap (KN_JSON_MAX_DEPTH=32 =>
 * bounded stack, ~32 small frames); node pool fixed at KN_JSON_MAX_NODES. */
#include "kn/kn_json.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *s;
    size_t n, i, err;
    kn_jdoc *d;
    int depth;
} jp;

static int jp_alloc(jp *p)
{
    if (p->d->count >= KN_JSON_MAX_NODES) {
        p->err = p->i;
        return -1;
    }
    p->d->nodes[p->d->count].child = 0;
    p->d->nodes[p->d->count].next = 0;
    p->d->nodes[p->d->count].key = NULL;
    p->d->nodes[p->d->count].key_len = 0;
    p->d->nodes[p->d->count].str = NULL;
    p->d->nodes[p->d->count].str_len = 0;
    p->d->nodes[p->d->count].num = 0;
    p->d->nodes[p->d->count].bval = 0;
    return p->d->count++;
}

static void jp_ws(jp *p)
{
    while (p->i < p->n) {
        char c = p->s[p->i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') p->i++;
        else break;
    }
}

static int jp_value(jp *p, int *out_node);

/* Parse a string token starting at p->s[p->i] == '"'. The raw span
 * (between the quotes) is returned; escapes are NOT resolved here (the
 * accessor does that). Rejects invalid escapes and unterminated strings
 * so a document with malformed strings never parses. */
static int jp_string(jp *p, const char **start, size_t *len)
{
    size_t i;
    if (p->i >= p->n || p->s[p->i] != '"') { p->err = p->i; return -1; }
    i = p->i + 1;
    for (;;) {
        unsigned char c;
        if (i >= p->n) { p->err = i; return -1; }
        c = (unsigned char)p->s[i];
        if (c == '"') break;
        if (c == '\\') {
            i++;
            if (i >= p->n) { p->err = i; return -1; }
            switch (p->s[i]) {
            case '"': case '\\': case '/': case 'b': case 'f':
            case 'n': case 'r': case 't':
                i++;
                break;
            case 'u': {
                int k;
                if (i + 4 >= p->n) { p->err = i; return -1; }
                for (k = 1; k <= 4; ++k) {
                    char h = p->s[i + k];
                    int ok = (h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') ||
                             (h >= 'A' && h <= 'F');
                    if (!ok) { p->err = i + (size_t)k; return -1; }
                }
                i += 5;
                break;
            }
            default:
                p->err = i;
                return -1;
            }
        } else if (c < 0x20) {              /* control char in string */
            p->err = i;
            return -1;
        } else {
            i++;
        }
    }
    *start = p->s + p->i + 1;
    *len = i - (p->i + 1);
    p->i = i + 1;
    return 0;
}

static int jp_number(jp *p, int node)
{
    const char *st = p->s + p->i;
    size_t i = p->i;
    char *endp = NULL;
    if (i < p->n && (p->s[i] == '-' || p->s[i] == '+')) i++;
    {
        size_t digits = 0;
        while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') { i++; digits++; }
        if (digits == 0) { p->err = p->i; return -1; }
        if (i < p->n && p->s[i] == '.') {
            i++;
            digits = 0;
            while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') { i++; digits++; }
            if (digits == 0) { p->err = i; return -1; }
        }
        if (i < p->n && (p->s[i] == 'e' || p->s[i] == 'E')) {
            i++;
            if (i < p->n && (p->s[i] == '-' || p->s[i] == '+')) i++;
            digits = 0;
            while (i < p->n && p->s[i] >= '0' && p->s[i] <= '9') { i++; digits++; }
            if (digits == 0) { p->err = i; return -1; }
        }
    }
    p->d->nodes[node].type = KN_J_NUM;
    p->d->nodes[node].str = st;
    p->d->nodes[node].str_len = i - p->i;
    p->d->nodes[node].num = strtod(st, &endp);
    if (!endp || (size_t)(endp - st) != i - p->i) { p->err = p->i; return -1; }
    p->i = i;
    return 0;
}

static int jp_literal(jp *p, const char *lit, size_t litlen, int type, int bv)
{
    int node;
    if (p->i + litlen > p->n || memcmp(p->s + p->i, lit, litlen) != 0) {
        p->err = p->i;
        return -1;
    }
    node = jp_alloc(p);
    if (node < 0) return -1;
    p->d->nodes[node].type = type;
    p->d->nodes[node].bval = bv;
    p->i += litlen;
    return node;
}

static int jp_value(jp *p, int *out_node)
{
    int node;
    if (p->depth >= KN_JSON_MAX_DEPTH) { p->err = p->i; return KN_JSON_ERR_DEPTH; }
    jp_ws(p);
    if (p->i >= p->n) { p->err = p->i; return KN_JSON_ERR_SYNTAX; }
    switch (p->s[p->i]) {
    case 'n': {
        int r = jp_literal(p, "null", 4, KN_J_NULL, 0);
        if (r < 0) return KN_JSON_ERR_SYNTAX;
        *out_node = r;
        return KN_JSON_OK;
    }
    case 't': {
        int r = jp_literal(p, "true", 4, KN_J_BOOL, 1);
        if (r < 0) return KN_JSON_ERR_SYNTAX;
        *out_node = r;
        return KN_JSON_OK;
    }
    case 'f': {
        int r = jp_literal(p, "false", 5, KN_J_BOOL, 0);
        if (r < 0) return KN_JSON_ERR_SYNTAX;
        *out_node = r;
        return KN_JSON_OK;
    }
    case '"': {
        const char *st;
        size_t sl;
        if (jp_string(p, &st, &sl) != 0) return KN_JSON_ERR_SYNTAX;
        node = jp_alloc(p);
        if (node < 0) return KN_JSON_ERR_NODES;
        p->d->nodes[node].type = KN_J_STR;
        p->d->nodes[node].str = st;
        p->d->nodes[node].str_len = sl;
        *out_node = node;
        return KN_JSON_OK;
    }
    case '[': {
        int first = -1, prev = -1;
        p->i++;
        node = jp_alloc(p);
        if (node < 0) return KN_JSON_ERR_NODES;
        p->d->nodes[node].type = KN_J_ARR;
        p->depth++;
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == ']') { p->i++; p->depth--; *out_node = node; return KN_JSON_OK; }
        for (;;) {
            int child;
            int rc = jp_value(p, &child);
            if (rc != KN_JSON_OK) { p->depth--; return rc; }
            if (first < 0) first = child;
            else p->d->nodes[prev].next = child + 1;
            prev = child;
            jp_ws(p);
            if (p->i >= p->n) { p->err = p->i; p->depth--; return KN_JSON_ERR_SYNTAX; }
            if (p->s[p->i] == ',') { p->i++; continue; }
            if (p->s[p->i] == ']') { p->i++; break; }
            p->err = p->i;
            p->depth--;
            return KN_JSON_ERR_SYNTAX;
        }
        p->depth--;
        p->d->nodes[node].child = first >= 0 ? first + 1 : 0;
        *out_node = node;
        return KN_JSON_OK;
    }
    case '{': {
        int first = -1, prev = -1;
        p->i++;
        node = jp_alloc(p);
        if (node < 0) return KN_JSON_ERR_NODES;
        p->d->nodes[node].type = KN_J_OBJ;
        p->depth++;
        jp_ws(p);
        if (p->i < p->n && p->s[p->i] == '}') { p->i++; p->depth--; *out_node = node; return KN_JSON_OK; }
        for (;;) {
            const char *kst;
            size_t klen;
            int child;
            int rc;
            jp_ws(p);
            if (jp_string(p, &kst, &klen) != 0) { p->depth--; return KN_JSON_ERR_SYNTAX; }
            jp_ws(p);
            if (p->i >= p->n || p->s[p->i] != ':') {
                p->err = p->i; p->depth--; return KN_JSON_ERR_SYNTAX;
            }
            p->i++;
            rc = jp_value(p, &child);
            if (rc != KN_JSON_OK) { p->depth--; return rc; }
            p->d->nodes[child].key = kst;
            p->d->nodes[child].key_len = klen;
            if (first < 0) first = child;
            else p->d->nodes[prev].next = child + 1;
            prev = child;
            jp_ws(p);
            if (p->i >= p->n) { p->err = p->i; p->depth--; return KN_JSON_ERR_SYNTAX; }
            if (p->s[p->i] == ',') { p->i++; continue; }
            if (p->s[p->i] == '}') { p->i++; break; }
            p->err = p->i;
            p->depth--;
            return KN_JSON_ERR_SYNTAX;
        }
        p->depth--;
        p->d->nodes[node].child = first >= 0 ? first + 1 : 0;
        *out_node = node;
        return KN_JSON_OK;
    }
    default:
        node = jp_alloc(p);
        if (node < 0) return KN_JSON_ERR_NODES;
        if (jp_number(p, node) != 0) return KN_JSON_ERR_SYNTAX;
        *out_node = node;
        return KN_JSON_OK;
    }
}

int kn_json_parse(kn_jdoc *d, const char *text, size_t len, size_t *err_at)
{
    jp p;
    int root = -1, rc;
    if (err_at) *err_at = 0;
    if (!d || !text) return KN_JSON_ERR_ARGS;
    memset(d, 0, sizeof *d);
    d->src = text;
    d->src_len = len;
    p.s = text; p.n = len; p.i = 0; p.err = 0; p.d = d; p.depth = 0;
    rc = jp_value(&p, &root);
    if (rc != KN_JSON_OK) {
        if (err_at) *err_at = p.err;
        return rc;
    }
    jp_ws(&p);
    if (p.i != p.n) {                     /* trailing garbage */
        if (err_at) *err_at = p.i;
        return KN_JSON_ERR_SYNTAX;
    }
    /* The root is ALWAYS node 0: jp_value allocates the value's node
     * before descending into children, and parsing starts at the root. */
    (void)root;
    return KN_JSON_OK;
}

const kn_jnode *kn_json_root(const kn_jdoc *d)
{
    /* the root is node 0: jp_value allocates the container/literal node
     * BEFORE parsing children, and parsing starts at the root. */
    if (!d || d->count == 0) return NULL;
    return &d->nodes[0];
}

const kn_jnode *kn_json_get(const kn_jdoc *d, const kn_jnode *obj,
                            const char *key)
{
    int idx;
    size_t klen;
    if (!d || !obj || obj->type != KN_J_OBJ || !key) return NULL;
    klen = strlen(key);
    idx = obj->child;
    while (idx) {
        const kn_jnode *n = &d->nodes[idx - 1];
        if (n->key_len == klen && n->key && memcmp(n->key, key, klen) == 0)
            return n;
        idx = n->next;
    }
    return NULL;
}

const kn_jnode *kn_json_at(const kn_jdoc *d, const kn_jnode *arr, int idx_w)
{
    int idx;
    if (!d || !arr || arr->type != KN_J_ARR || idx_w < 0) return NULL;
    idx = arr->child;
    while (idx && idx_w > 0) {
        idx = d->nodes[idx - 1].next;
        idx_w--;
    }
    return idx ? &d->nodes[idx - 1] : NULL;
}

int kn_json_len(const kn_jdoc *d, const kn_jnode *arr)
{
    int idx, n = 0;
    if (!d || !arr || arr->type != KN_J_ARR) return -1;
    idx = arr->child;
    while (idx) {
        n++;
        idx = d->nodes[idx - 1].next;
    }
    return n;
}

int kn_json_int(const kn_jnode *n, long long *out)
{
    char *endp = NULL;
    long long v;
    if (!n || !out) return KN_JSON_ERR_ARGS;
    if (n->type != KN_J_NUM) return KN_JSON_ERR_TYPE;
    if (!n->str || n->str_len == 0) return KN_JSON_ERR_TYPE;
    v = strtoll(n->str, &endp, 10);
    if (!endp || (size_t)(endp - n->str) != n->str_len)
        return KN_JSON_ERR_TYPE;          /* not an integer literal */
    *out = v;
    return KN_JSON_OK;
}

int kn_json_double(const kn_jnode *n, double *out)
{
    if (!n || !out) return KN_JSON_ERR_ARGS;
    if (n->type != KN_J_NUM) return KN_JSON_ERR_TYPE;
    *out = n->num;
    return KN_JSON_OK;
}

int kn_json_bool(const kn_jnode *n, int *out)
{
    if (!n || !out) return KN_JSON_ERR_ARGS;
    if (n->type != KN_J_BOOL) return KN_JSON_ERR_TYPE;
    *out = n->bval;
    return KN_JSON_OK;
}

/* UTF-8 emit helper for \u decoding */
static size_t utf8_put(char *o, size_t rem, unsigned cp)
{
    if (cp < 0x80u) {
        if (rem < 1) return 0;
        o[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        if (rem < 2) return 0;
        o[0] = (char)(0xC0u | (cp >> 6));
        o[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        if (rem < 3) return 0;
        o[0] = (char)(0xE0u | (cp >> 12));
        o[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        o[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (rem < 4) return 0;
    o[0] = (char)(0xF0u | (cp >> 18));
    o[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    o[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    o[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

static int hex4(const char *s, unsigned *out)
{
    int i;
    unsigned v = 0;
    for (i = 0; i < 4; ++i) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
        else return -1;
    }
    *out = v;
    return 0;
}

int kn_json_str_copy(const kn_jnode *n, char *out, size_t outsz)
{
    size_t i, o = 0;
    if (!n || !out || outsz == 0) return KN_JSON_ERR_ARGS;
    out[0] = '\0';
    if (n->type != KN_J_STR) return KN_JSON_ERR_TYPE;
    for (i = 0; i < n->str_len; ) {
        char c = n->str[i];
        if (c == '\\') {
            char e;
            unsigned cp;
            i++;
            if (i >= n->str_len) return KN_JSON_ERR_SYNTAX;
            e = n->str[i++];
            switch (e) {
            case '"':  if (o + 1 >= outsz) return KN_JSON_ERR_BIG; out[o++] = '"'; break;
            case '\\': if (o + 1 >= outsz) return KN_JSON_ERR_BIG; out[o++] = '\\'; break;
            case '/':  if (o + 1 >= outsz) return KN_JSON_ERR_BIG; out[o++] = '/'; break;
            case 'b':  if (o + 1 >= outsz) return KN_JSON_ERR_BIG; out[o++] = '\b'; break;
            case 'f':  if (o + 1 >= outsz) return KN_JSON_ERR_BIG; out[o++] = '\f'; break;
            case 'n':  if (o + 1 >= outsz) return KN_JSON_ERR_BIG; out[o++] = '\n'; break;
            case 'r':  if (o + 1 >= outsz) return KN_JSON_ERR_BIG; out[o++] = '\r'; break;
            case 't':  if (o + 1 >= outsz) return KN_JSON_ERR_BIG; out[o++] = '\t'; break;
            case 'u':
                if (i + 4 > n->str_len || hex4(n->str + i, &cp) != 0)
                    return KN_JSON_ERR_SYNTAX;
                i += 4;
                if (cp >= 0xD800u && cp <= 0xDBFFu &&
                    i + 6 <= n->str_len && n->str[i] == '\\' &&
                    n->str[i + 1] == 'u') {
                    unsigned lo;
                    if (hex4(n->str + i + 2, &lo) == 0 &&
                        lo >= 0xDC00u && lo <= 0xDFFFu) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) +
                             (lo - 0xDC00u);
                        i += 6;
                    } else {
                        cp = 0xFFFDu;      /* lone high surrogate */
                    }
                } else if (cp >= 0xD800u && cp <= 0xDFFFu) {
                    cp = 0xFFFDu;          /* lone surrogate */
                }
                {
                    size_t w = utf8_put(out + o, outsz - o - 1, cp);
                    if (w == 0) return KN_JSON_ERR_BIG;
                    o += w;
                }
                break;
            default:
                return KN_JSON_ERR_SYNTAX;
            }
        } else {
            if (o + 1 >= outsz) return KN_JSON_ERR_BIG;
            out[o++] = c;                  /* raw UTF-8 bytes pass through */
            i++;
        }
    }
    out[o] = '\0';
    return KN_JSON_OK;
}
