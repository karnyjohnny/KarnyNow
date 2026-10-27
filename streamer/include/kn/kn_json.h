/* kn_json.h — minimal zero-allocation JSON parser (RFC 8259 subset).
 *
 * Why it exists: the CloudMatch payload (docs/RTSP.md §5) and the RTSP
 * handoff are JSON; the project links no JSON library (F1 minimalism),
 * and kn_diag/knlog only ever WRITE JSON. This is a READ-only parser for
 * trusted-but-foreign network text: a fixed node pool, a depth cap, no
 * allocation, no recursion past the cap, honest overflow errors. Strings
 * are stored as RAW spans of the input (zero-copy); kn_json_str_copy
 * unescapes on demand into a caller buffer.
 *
 * Bounds (documented limits, not bugs):
 *   KN_JSON_MAX_NODES  node pool (CloudMatch session payloads parse in
 *                      ~200-400 nodes; overflow = KN_JSON_ERR_NODES)
 *   KN_JSON_MAX_DEPTH  32
 *   numbers are parsed with strtod (double); int64-precision fields the
 *                      callers use (status, ports, usage) are read via
 *                      kn_json_int which re-parses the raw span with
 *                      strtoll — no precision loss for those.
 */
#ifndef KN_JSON_H
#define KN_JSON_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    KN_JSON_OK        =  0,
    KN_JSON_ERR_ARGS  = -1,
    KN_JSON_ERR_SYNTAX= -2,
    KN_JSON_ERR_DEPTH = -3,
    KN_JSON_ERR_NODES = -4,     /* pool exhausted */
    KN_JSON_ERR_TYPE  = -5,     /* accessor: wrong node type */
    KN_JSON_ERR_BIG   = -6      /* str_copy: out buffer too small */
};

#define KN_JSON_MAX_NODES 1024
#define KN_JSON_MAX_DEPTH 32

typedef enum {
    KN_J_NULL = 0, KN_J_BOOL, KN_J_NUM, KN_J_STR, KN_J_ARR, KN_J_OBJ
} kn_jtype;

typedef struct kn_jnode {
    int type;                       /* kn_jtype */
    int child;                      /* obj/arr: index+1 of first child */
    int next;                       /* index+1 of next sibling */
    const char *key; size_t key_len;    /* member name (raw span) */
    const char *str; size_t str_len;    /* string/number raw span */
    double num;                         /* KN_J_NUM value */
    int bval;                           /* KN_J_BOOL value */
} kn_jnode;

typedef struct kn_jdoc {
    kn_jnode nodes[KN_JSON_MAX_NODES];
    int count;
    const char *src;
    size_t src_len;
} kn_jdoc;

/* Parse `text` (len bytes, need not be NUL-terminated). The document
 * keeps RAW SPANS into text — text must outlive the doc. Trailing
 * whitespace after the top-level value is allowed; anything else is a
 * syntax error. Returns KN_JSON_OK / negative; *err_at (optional) gets
 * the byte offset of the failure. */
int kn_json_parse(kn_jdoc *d, const char *text, size_t len, size_t *err_at);

const kn_jnode *kn_json_root(const kn_jdoc *d);

/* Object member by EXACT name (case-sensitive, JSON semantics). NULL when
 * absent or `obj` is not an object. */
const kn_jnode *kn_json_get(const kn_jdoc *d, const kn_jnode *obj,
                            const char *key);

/* Array element by index / length. NULL / -1 on type errors. */
const kn_jnode *kn_json_at(const kn_jdoc *d, const kn_jnode *arr, int idx);
int kn_json_len(const kn_jdoc *d, const kn_jnode *arr);

/* Typed accessors (KN_JSON_ERR_TYPE when the node type differs): */
int kn_json_int(const kn_jnode *n, long long *out);     /* strict strtoll */
int kn_json_double(const kn_jnode *n, double *out);
int kn_json_bool(const kn_jnode *n, int *out);

/* Unescaped string copy (handles \" \\ \/ \b \f \n \r \t and \uXXXX with
 * UTF-8 encoding incl. surrogate pairs; lone surrogates -> U+FFFD).
 * Returns KN_JSON_OK / ERR_TYPE / ERR_BIG / ERR_SYNTAX (bad escape). */
int kn_json_str_copy(const kn_jnode *n, char *out, size_t outsz);

#ifdef __cplusplus
}
#endif

#endif /* KN_JSON_H */
