#include <bolt/MobileAot.h>
#include <stdio.h>

int main(void) {
    const char *arguments[] = {"scene", "player"};
    if (bolt_mobile_run(0, NULL) != 17) return 1;
    if (bolt_mobile_run(2, arguments) != 19) return 2;
    puts("PASS AOT script from C, with zero and two arguments");
    return 0;
}
