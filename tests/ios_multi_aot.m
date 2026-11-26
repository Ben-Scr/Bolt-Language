@import BoltInterpreter;
@import BoltScript;
@import BoltScript_second_scene;

int main(void) {
    const char *args[] = {"Funke 🌍"};
    if (bolt_interpreter_abi() != BOLT_INTERPRETER_ABI) return 1;
    if (bolt_mobile_run(1, args) != 42 || second_scene(1, args) != 42) return 2;
    if (bolt_mobile_run(1, args) != 43 || second_scene(1, args) != 43) return 3;
    return 0;
}
