#pragma once
#include <signal.h>

static volatile sig_atomic_t bolt_console_interrupted;
static void bolt_console_signal(int signal_number) {
    (void)signal_number;
    bolt_console_interrupted = 1;
}
static int bolt_console_watch_signal(void) {
    struct sigaction action = {};
    action.sa_handler = bolt_console_signal;
    sigemptyset(&action.sa_mask);
    return sigaction(SIGUSR1, &action, NULL);
}
static int bolt_console_take_signal(void) {
    int received = bolt_console_interrupted;
    bolt_console_interrupted = 0;
    return received;
}
