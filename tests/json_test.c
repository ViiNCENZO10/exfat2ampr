/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "json.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const char *bad[] = {"{}junk", "01", "1.", "1e", "1e+", "-.1", "1e999999999999999999999",
                       "[1,]", "{\"x\":\"\\q\"}", "\"\\uD800\"", "\"\\uDC00\"", "\"\\u0000\"", "\"a\nb\""};
  for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) {
    jdoc d;
    assert(json_parse(bad[i], strlen(bad[i]), &d) == -1);
    assert(!d.root && !d.mem);
  }
  const char *good = " {\"x\":-1.25e2,\"emoji\":\"\\uD83D\\uDE00\",\"array\":[true,null,0]} \n";
  jdoc d;
  assert(json_parse(good, strlen(good), &d) == 0);
  assert(json_num(d.root, "x", 0) == -125);
  assert(strcmp(json_str(d.root, "emoji", ""), "\xf0\x9f\x98\x80") == 0);
  json_free(&d);
  puts("strict JSON: OK");
  return 0;
}
