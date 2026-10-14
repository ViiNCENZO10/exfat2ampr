/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 V-10 (exFAT2AMPR). GPL-3.0-or-later with additional terms: see NOTICE. */
#include "json.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef struct {
  const char *p, *end;
  jnode *nodes; size_t nn, ncap;
  char *str; size_t sn, scap;
  int depth;
} parser;

static void ws(parser *P) { while (P->p < P->end && (*P->p == ' ' || *P->p == '\t' || *P->p == '\n' || *P->p == '\r')) P->p++; }

static jnode *node(parser *P) {
  if (P->nn >= P->ncap) return NULL;
  jnode *n = &P->nodes[P->nn++];
  memset(n, 0, sizeof *n);
  return n;
}

static void put_utf8(parser *P, unsigned cp) {
  char b[4];
  int n;
  if (cp < 0x80) { b[0] = (char)cp; n = 1; }
  else if (cp < 0x800) { b[0] = (char)(0xc0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 63)); n = 2; }
  else if (cp < 0x10000) { b[0] = (char)(0xe0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 63)); b[2] = (char)(0x80 | (cp & 63)); n = 3; }
  else { b[0] = (char)(0xf0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 63)); b[2] = (char)(0x80 | ((cp >> 6) & 63)); b[3] = (char)(0x80 | (cp & 63)); n = 4; }
  for (int i = 0; i < n && P->sn < P->scap; i++) P->str[P->sn++] = b[i];
}

static int hex4(const char *p) {
  int v = 0;
  for (int i = 0; i < 4; i++) {
    char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= c - '0';
    else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
    else return -1;
  }
  return v;
}

static const char *string(parser *P) {
  if (P->p >= P->end || *P->p != '"') return NULL;
  P->p++;
  char *out = P->str + P->sn;
  while (P->p < P->end && *P->p != '"') {
    char c = *P->p++;
    if ((unsigned char)c < 0x20) return NULL;
    if (c == '\\' && P->p < P->end) {
      char e = *P->p++;
      switch (e) {
        case 'n': c = '\n'; break;
        case 't': c = '\t'; break;
        case 'r': c = '\r'; break;
        case 'b': c = '\b'; break;
        case 'f': c = '\f'; break;
        case 'u': {
          if (P->end - P->p < 4) return NULL;
          int v = hex4(P->p);
          if (v < 0) return NULL;
          P->p += 4;
          if (v >= 0xd800 && v <= 0xdbff) {
            if (P->end - P->p < 6 || P->p[0] != '\\' || P->p[1] != 'u') return NULL;
            int low = hex4(P->p + 2);
            if (low < 0xdc00 || low > 0xdfff) return NULL;
            P->p += 6;
            v = 0x10000 + ((v - 0xd800) << 10) + low - 0xdc00;
          } else if (v >= 0xdc00 && v <= 0xdfff) return NULL;
          if (v == 0) return NULL; /* C-string API cannot represent embedded NUL. */
          put_utf8(P, (unsigned)v);
          continue;
        }
        case '"': case '\\': case '/': c = e; break;
        default: return NULL;
      }
    }
    if (P->sn < P->scap) P->str[P->sn++] = c;
  }
  if (P->p >= P->end || P->sn >= P->scap) return NULL;
  P->p++;
  P->str[P->sn++] = 0;
  return out;
}

static jnode *value(parser *P);

static jnode *container(parser *P, bool obj) {
  jnode *n = node(P);
  if (!n || ++P->depth > 64) return NULL;
  n->type = obj ? J_OBJ : J_ARR;
  P->p++;
  jnode **tail = &n->child;
  ws(P);
  if (P->p < P->end && *P->p == (obj ? '}' : ']')) { P->p++; P->depth--; return n; }
  for (;;) {
    const char *key = NULL;
    ws(P);
    if (obj) {
      key = string(P);
      if (!key) return NULL;
      ws(P);
      if (P->p >= P->end || *P->p != ':') return NULL;
      P->p++;
    }
    jnode *v = value(P);
    if (!v) return NULL;
    v->key = key;
    *tail = v; tail = &v->next;
    n->count++;
    ws(P);
    if (P->p < P->end && *P->p == ',') { P->p++; continue; }
    if (P->p < P->end && *P->p == (obj ? '}' : ']')) { P->p++; P->depth--; return n; }
    return NULL;
  }
}

static jnode *value(parser *P) {
  ws(P);
  if (P->p >= P->end) return NULL;
  char c = *P->p;
  if (c == '{' || c == '[') return container(P, c == '{');
  jnode *n = node(P);
  if (!n) return NULL;
  if (c == '"') { n->type = J_STR; n->str = string(P); return n->str ? n : NULL; }
  if (P->end - P->p >= 4 && !strncmp(P->p, "true", 4)) { n->type = J_BOOL; n->num = 1; P->p += 4; return n; }
  if (P->end - P->p >= 5 && !strncmp(P->p, "false", 5)) { n->type = J_BOOL; P->p += 5; return n; }
  if (P->end - P->p >= 4 && !strncmp(P->p, "null", 4)) { n->type = J_NULL; P->p += 4; return n; }
  /* Strict JSON number, bounded work even for an enormous exponent. */
  const char *start = P->p;
  if (*P->p == '-') P->p++;
  if (P->p == P->end) return NULL;
  if (*P->p == '0') P->p++;
  else {
    if (*P->p < '1' || *P->p > '9') return NULL;
    while (P->p < P->end && *P->p >= '0' && *P->p <= '9') P->p++;
  }
  if (P->p < P->end && *P->p == '.') {
    P->p++;
    const char *digits = P->p;
    while (P->p < P->end && *P->p >= '0' && *P->p <= '9') P->p++;
    if (digits == P->p) return NULL;
  }
  if (P->p < P->end && (*P->p == 'e' || *P->p == 'E')) {
    P->p++;
    if (P->p < P->end && (*P->p == '-' || *P->p == '+')) P->p++;
    const char *digits = P->p;
    unsigned exponent = 0;
    while (P->p < P->end && *P->p >= '0' && *P->p <= '9') {
      exponent = exponent * 10 + (unsigned)(*P->p++ - '0');
      if (exponent > 308) return NULL;
    }
    if (digits == P->p) return NULL;
  }
  size_t count = (size_t)(P->p - start);
  char number[128];
  if (!count || count >= sizeof number) return NULL;
  memcpy(number, start, count); number[count] = 0;
  char *end;
  double v = strtod(number, &end);
  if (*end || !isfinite(v)) return NULL;
  n->type = J_NUM;
  n->num = v;
  return n;
}

int json_parse(const char *text, size_t len, jdoc *doc) {
  doc->root = NULL; doc->mem = NULL;
  /* every node needs at least one input byte, every string fits in its input */
  size_t ncap = len / 2 + 4, scap = len + 4;
  void *mem = malloc(ncap * sizeof(jnode) + scap);
  if (!mem) return -1;
  parser P = {text, text + len, mem, 0, ncap, (char *)mem + ncap * sizeof(jnode), 0, scap, 0};
  jnode *root = value(&P);
  ws(&P);
  if (!root || P.p != P.end) { free(mem); return -1; }
  doc->root = root; doc->mem = mem;
  return 0;
}

void json_free(jdoc *doc) { free(doc->mem); doc->mem = NULL; doc->root = NULL; }

const jnode *json_get(const jnode *obj, const char *key) {
  if (!obj || obj->type != J_OBJ) return NULL;
  for (const jnode *n = obj->child; n; n = n->next)
    if (n->key && !strcmp(n->key, key)) return n;
  return NULL;
}
const char *json_str(const jnode *obj, const char *key, const char *dflt) {
  const jnode *n = json_get(obj, key);
  return n && n->type == J_STR ? n->str : dflt;
}
double json_num(const jnode *obj, const char *key, double dflt) {
  const jnode *n = json_get(obj, key);
  return n && (n->type == J_NUM || n->type == J_BOOL) ? n->num : dflt;
}
bool json_bool(const jnode *obj, const char *key, bool dflt) {
  const jnode *n = json_get(obj, key);
  return n && n->type == J_BOOL ? n->num != 0 : dflt;
}
