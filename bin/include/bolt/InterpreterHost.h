#ifndef BOLT_INTERPRETER_HOST_H
#define BOLT_INTERPRETER_HOST_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) && defined(BOLT_INTERPRETER_SHARED)
# if defined(BOLT_INTERPRETER_BUILD)
#  define BOLT_INTERPRETER_API __declspec(dllexport)
# else
#  define BOLT_INTERPRETER_API __declspec(dllimport)
# endif
#elif defined(__GNUC__)
# define BOLT_INTERPRETER_API __attribute__((visibility("default")))
#else
# define BOLT_INTERPRETER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define BOLT_INTERPRETER_ABI 1u
typedef struct BoltInterpretedProgram BoltInterpretedProgram;
typedef struct BoltInterpreterCompiler BoltInterpreterCompiler;
enum BoltInterpreterStatus {
    BOLT_INTERPRETER_OK = 0,
    BOLT_INTERPRETER_ERROR = 1
};

typedef struct BoltInterpreterLimits {
    size_t max_steps;         /* Zero disables the instruction budget. */
    size_t max_call_depth;
    size_t max_runtime_depth;
} BoltInterpreterLimits;

/* Output bytes are valid for the duration of the callback. The callback must
   return normally and must not recursively use or release the same program. */
typedef void (*BoltInterpreterWrite)(void *context, const char *bytes, size_t length);
typedef struct BoltInterpreterIO {
    const char *input;
    size_t input_length;
    const char *const *arguments;  /* UTF-8 Main arguments, excluding argv[0]. */
    size_t argument_count;
    BoltInterpreterWrite write;
    void *context;
} BoltInterpreterIO;

BOLT_INTERPRETER_API uint32_t bolt_interpreter_abi(void);

/* Parses, checks and initializes one in-memory Main program using Bolt's
   interpreter. A null limits pointer selects Bolt's standard budgets. Input
   source is copied during this call. On failure *out is null. Diagnostics
   are optional, UTF-8 and NUL-terminated if capacity is positive. */
BOLT_INTERPRETER_API int bolt_interpreter_compile(
    uint32_t abi, const char *name, const char *source, size_t length,
    const BoltInterpreterLimits *limits, BoltInterpretedProgram **out,
    char *error, size_t capacity);

/* Reuse one compiler across source edits. Successful programs are independent
   handles. A failed edit leaves previously returned programs usable. Compiles
   on a session are serialized. Releasing a compiler frees its syntax cache. */
BOLT_INTERPRETER_API int bolt_interpreter_compiler_create(
    uint32_t abi, BoltInterpreterCompiler **out, char *error, size_t capacity);
BOLT_INTERPRETER_API int bolt_interpreter_compile_cached(
    BoltInterpreterCompiler *compiler, const char *name, const char *source, size_t length,
    const BoltInterpreterLimits *limits, BoltInterpretedProgram **out, char *error, size_t capacity);
BOLT_INTERPRETER_API void bolt_interpreter_compiler_release(BoltInterpreterCompiler *compiler);

/* Runs the compiled program again without reparsing. Static state persists
   between invocations. Null io means empty stdin/argv and discarded stdout.
   Run calls on the same handle are serialized. On failure exit_code is zero. */
BOLT_INTERPRETER_API int bolt_interpreter_run(
    BoltInterpretedProgram *program, const BoltInterpreterIO *io,
    int32_t *exit_code, char *error, size_t capacity);

/* Wait for all uses of a handle to finish before releasing it. Null is valid. */
BOLT_INTERPRETER_API void bolt_interpreter_release(BoltInterpretedProgram *program);

#ifdef __cplusplus
}
#endif
#endif
