#include <bolt/MobileAot.h>
#include <stdio.h>

/* UTF-8 source, including the argument passed to Bolt. */
int main(void) {
    const char *args[] = {"Funke 🌍"};
    const char *invalid[] = {NULL};
    if (BOLT_MOBILE_ENTRY(1, NULL) != 1 || BOLT_MOBILE_ENTRY(-1, NULL) != 1 ||
        BOLT_MOBILE_ENTRY(1, invalid) != 1) return 2;
    if (BOLT_MOBILE_ENTRY(0, NULL) != 5) return 3;
    if (BOLT_MOBILE_ENTRY(1, args) != 42 || BOLT_MOBILE_ENTRY(1, args) != 43) return 4;
    puts("PASS native mobile AOT: UTF-8 arguments, array loop, repeated calls and static state");
    return 0;
}
