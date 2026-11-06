#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 64

typedef struct {
  const char *p;
  int depth;
} parser;

static jval *parse_value(parser *ps);

static void skip_ws(parser *ps) {
  while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
    ps->p++;
}

static jval *new_val(jtype t) {
  jval *v = calloc(1, sizeof(*v));
  if (v)
    v->type = t;
  return v;
}

static int hex4(const char *s, unsigned *out) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    char c = s[i];
    v <<= 4;
    if (c >= '0' && c <= '9')
      v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f')
      v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
      v |= (unsigned)(c - 'A' + 10);
    else
      return -1;
  }
  *out = v;
  return 0;
}

static size_t put_utf8(char *o, unsigned cp) {
  if (cp < 0x80) {
    o[0] = (char)cp;
    return 1;
  } else if (cp < 0x800) {
    o[0] = (char)(0xC0 | (cp >> 6));
    o[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  } else if (cp < 0x10000) {
    o[0] = (char)(0xE0 | (cp >> 12));
    o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  o[0] = (char)(0xF0 | (cp >> 18));
  o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  o[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* Parses a string starting at the opening quote. Returns malloc'd UTF-8. */
static char *parse_string(parser *ps) {
  if (*ps->p != '"')
    return NULL;
  ps->p++;
  const char *start = ps->p;
  size_t maxlen = 0;
  for (const char *q = start; *q && *q != '"'; q++, maxlen++)
    if (*q == '\\' && q[1])
      q++, maxlen++;
  char *out = malloc(maxlen + 4);
  if (!out)
    return NULL;
  size_t n = 0;
  while (*ps->p && *ps->p != '"') {
    char c = *ps->p++;
    if (c != '\\') {
      out[n++] = c;
      continue;
    }
    char e = *ps->p++;
    switch (e) {
    case '"': out[n++] = '"'; break;
    case '\\': out[n++] = '\\'; break;
    case '/': out[n++] = '/'; break;
    case 'b': out[n++] = '\b'; break;
    case 'f': out[n++] = '\f'; break;
    case 'n': out[n++] = '\n'; break;
    case 'r': out[n++] = '\r'; break;
    case 't': out[n++] = '\t'; break;
    case 'u': {
      unsigned cp;
      if (hex4(ps->p, &cp) < 0)
        goto fail;
      ps->p += 4;
      if (cp >= 0xD800 && cp <= 0xDBFF && ps->p[0] == '\\' && ps->p[1] == 'u') {
        unsigned lo;
        if (hex4(ps->p + 2, &lo) == 0 && lo >= 0xDC00 && lo <= 0xDFFF) {
          cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          ps->p += 6;
        }
      }
      /* \uXXXX is 6 input bytes and at most 4 output bytes (4 for a surrogate pair of 12) */
      n += put_utf8(out + n, cp);
      break;
    }
    default:
      goto fail;
    }
  }
  if (*ps->p != '"')
    goto fail;
  ps->p++;
  out[n] = 0;
  return out;
fail:
  free(out);
  return NULL;
}

static jval *parse_container(parser *ps, int is_obj) {
  char close = is_obj ? '}' : ']';
  if (++ps->depth > JSON_MAX_DEPTH)
    return NULL;
  ps->p++; /* skip { or [ */
  jval *v = new_val(is_obj ? J_OBJ : J_ARR);
  if (!v)
    return NULL;
  jval **tail = &v->child;
  skip_ws(ps);
  if (*ps->p == close) {
    ps->p++;
    ps->depth--;
    return v;
  }
  for (;;) {
    char *key = NULL;
    skip_ws(ps);
    if (is_obj) {
      key = parse_string(ps);
      if (!key)
        goto fail;
      skip_ws(ps);
      if (*ps->p != ':') {
        free(key);
        goto fail;
      }
      ps->p++;
    }
    jval *item = parse_value(ps);
    if (!item) {
      free(key);
      goto fail;
    }
    item->key = key;
    *tail = item;
    tail = &item->next;
    skip_ws(ps);
    if (*ps->p == ',') {
      ps->p++;
      continue;
    }
    if (*ps->p == close) {
      ps->p++;
      ps->depth--;
      return v;
    }
    goto fail;
  }
fail:
  json_free(v);
  return NULL;
}

static jval *parse_value(parser *ps) {
  skip_ws(ps);
  const char *p = ps->p;
  if (*p == '{')
    return parse_container(ps, 1);
  if (*p == '[')
    return parse_container(ps, 0);
  if (*p == '"') {
    char *s = parse_string(ps);
    if (!s)
      return NULL;
    jval *v = new_val(J_STR);
    if (!v) {
      free(s);
      return NULL;
    }
    v->str = s;
    return v;
  }
  if (!strncmp(p, "true", 4) || !strncmp(p, "false", 5)) {
    jval *v = new_val(J_BOOL);
    if (!v)
      return NULL;
    v->num = (*p == 't');
    ps->p += (*p == 't') ? 4 : 5;
    return v;
  }
  if (!strncmp(p, "null", 4)) {
    ps->p += 4;
    return new_val(J_NULL);
  }
  if (*p == '-' || (*p >= '0' && *p <= '9')) {
    char *end;
    double d = strtod(p, &end);
    if (end == p)
      return NULL;
    jval *v = new_val(J_NUM);
    if (!v)
      return NULL;
    v->num = d;
    ps->p = end;
    return v;
  }
  return NULL;
}

jval *json_parse(const char *text) {
  if (!text)
    return NULL;
  parser ps = {text, 0};
  jval *v = parse_value(&ps);
  if (!v)
    return NULL;
  skip_ws(&ps);
  if (*ps.p) {
    json_free(v);
    return NULL;
  }
  return v;
}

void json_free(jval *v) {
  while (v) {
    jval *next = v->next;
    json_free(v->child);
    free(v->key);
    free(v->str);
    free(v);
    v = next;
  }
}

jval *json_get(const jval *obj, const char *key) {
  if (!obj || obj->type != J_OBJ)
    return NULL;
  for (jval *c = obj->child; c; c = c->next)
    if (c->key && !strcmp(c->key, key))
      return c;
  return NULL;
}

const char *json_str(const jval *obj, const char *key) {
  jval *v = json_get(obj, key);
  return (v && v->type == J_STR) ? v->str : NULL;
}

double json_num(const jval *obj, const char *key, double def) {
  jval *v = json_get(obj, key);
  return (v && (v->type == J_NUM || v->type == J_BOOL)) ? v->num : def;
}

int json_bool(const jval *obj, const char *key, int def) {
  jval *v = json_get(obj, key);
  return (v && v->type == J_BOOL) ? (int)v->num : def;
}

void json_escape(char *out, size_t outlen, const char *in) {
  size_t n = 0;
  if (!outlen)
    return;
  for (; in && *in; in++) {
    unsigned char c = (unsigned char)*in;
    char tmp[8];
    const char *rep = NULL;
    size_t rl;
    switch (c) {
    case '"': rep = "\\\""; break;
    case '\\': rep = "\\\\"; break;
    case '\n': rep = "\\n"; break;
    case '\r': rep = "\\r"; break;
    case '\t': rep = "\\t"; break;
    default:
      if (c < 0x20) {
        snprintf(tmp, sizeof(tmp), "\\u%04x", c);
        rep = tmp;
      }
    }
    if (rep) {
      rl = strlen(rep);
      if (n + rl >= outlen)
        break;
      memcpy(out + n, rep, rl);
      n += rl;
    } else {
      if (n + 1 >= outlen)
        break;
      out[n++] = (char)c;
    }
  }
  out[n] = 0;
}
