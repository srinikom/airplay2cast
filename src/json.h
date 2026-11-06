#ifndef AP2C_JSON_H
#define AP2C_JSON_H

#include <stddef.h>

/* Minimal JSON DOM, sufficient for Cast v2 control messages. */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jval {
  jtype type;
  char *key;           /* member name when inside an object */
  char *str;           /* J_STR */
  double num;          /* J_NUM, J_BOOL (0/1) */
  struct jval *child;  /* first element/member of J_ARR / J_OBJ */
  struct jval *next;   /* next sibling */
} jval;

jval *json_parse(const char *text);
void json_free(jval *v);

jval *json_get(const jval *obj, const char *key);     /* object member or NULL */
const char *json_str(const jval *obj, const char *key); /* string member or NULL */
double json_num(const jval *obj, const char *key, double def);
int json_bool(const jval *obj, const char *key, int def);

/* Escape `in` as JSON string contents (no surrounding quotes). Always NUL-terminates. */
void json_escape(char *out, size_t outlen, const char *in);

#endif
