/* SPDX-License-Identifier: GPL-3.0-or-later
 * Small JSON reader for the engine's API answers (one allocation per document). */
#ifndef E2A_APP_JSON_H
#define E2A_APP_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } jtype;

typedef struct jnode {
  jtype type;
  double num;               /* J_NUM, J_BOOL (0/1) */
  const char *str;          /* J_STR (decoded, NUL-terminated) */
  const char *key;          /* member name when inside an object */
  struct jnode *child;      /* first element / member */
  struct jnode *next;       /* next sibling */
  int count;
} jnode;

typedef struct { jnode *root; void *mem; } jdoc;

int json_parse(const char *text, size_t len, jdoc *doc);   /* 0 on success */
void json_free(jdoc *doc);
const jnode *json_get(const jnode *obj, const char *key);
const char *json_str(const jnode *obj, const char *key, const char *dflt);
double json_num(const jnode *obj, const char *key, double dflt);
bool json_bool(const jnode *obj, const char *key, bool dflt);

#endif
