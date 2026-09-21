#include "gb_string.h"
#include <stdio.h>
#include <stdlib.h>

static void require(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        exit(1);
    }
}
static void token(char *actual, const char *expected) {
    require(actual && strcmp(actual, expected) == 0, "token mismatch");
}
int main(void) {
    char first[] = "+ONE++two+THREE+";
    char second[] = "red,green;;blue";
    char empty[] = "+++";
    char *a = NULL, *b = NULL, *c = NULL;
    token(gb_strtok_r(first, "+", &a), "ONE");
    token(gb_strtok_r(second, ",;", &b), "red");
    token(gb_strtok_r(NULL, "+", &a), "two");
    token(gb_strtok_r(NULL, ",;", &b), "green");
    token(gb_strtok_r(NULL, "+", &a), "THREE");
    require(gb_strtok_r(NULL, "+", &a) == NULL, "trailing delimiter not skipped");
    token(gb_strtok_r(NULL, ",;", &b), "blue");
    require(gb_strtok_r(NULL, ",;", &b) == NULL, "second input not exhausted");
    require(gb_strtok_r(NULL, "+", &a) == NULL, "exhausted iterator restarted");
    require(gb_strtok_r(empty, "+", &c) == NULL, "empty input produced a token");
    require(gb_strcasecmp("MiXeD", "mixed") == 0, "case folding failed");
    require(gb_strcasecmp("alpha", "BETA") < 0, "ascending comparison failed");
    require(gb_strcasecmp("Zulu", "alpha") > 0, "descending comparison failed");
    require(gb_strcasecmp("", "") == 0, "empty comparison failed");
    puts("PASS: independent token streams, delimiters and case-insensitive host strings");
    return 0;
}
