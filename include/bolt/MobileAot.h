#ifndef BOLT_MOBILE_AOT_H
#define BOLT_MOBILE_AOT_H

/* A precompiled executable Bolt project exports this C entry point. argc is
 * the number of script arguments. argv[0] is the first script argument; pass
 * 0, NULL for none. The result is Main's exit code, or 1 for a runtime error.
 * Call on the engine thread; script-owned static state belongs to the module. */
#ifndef BOLT_MOBILE_ENTRY
#define BOLT_MOBILE_ENTRY bolt_mobile_run
#endif

#ifdef __cplusplus
extern "C" {
#endif
#if defined(__clang__) || defined(__GNUC__)
__attribute__((visibility("default")))
#endif
int BOLT_MOBILE_ENTRY(int argc, const char* const* argv);
#ifdef __cplusplus
}
#endif
#endif
