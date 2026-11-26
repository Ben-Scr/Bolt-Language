#include <bolt/InterpreterHost.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char output[256];
static size_t used;
static void collect(void *context, const char *bytes, size_t length) {
    if (context == output && length < sizeof(output) - used) {
        memcpy(output + used, bytes, length);
        used += length;
        output[used] = 0;
    }
}

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "Failure at line %d: %s\n", __LINE__, error); return 1; } } while (0)
static int run_fixture(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return 1;
    if (fseek(file, 0, SEEK_END)) { fclose(file); return 1; }
    long count = ftell(file);
    if (count < 0 || fseek(file, 0, SEEK_SET)) { fclose(file); return 1; }
    char *source = malloc((size_t)count + 1);
    if (!source) { fclose(file); return 1; }
    size_t length = fread(source, 1, (size_t)count, file);
    fclose(file);
    char message[2048] = "";
    BoltInterpretedProgram *program = NULL;
    int status = length != (size_t)count || bolt_interpreter_compile(
        BOLT_INTERPRETER_ABI, path, source, length, NULL, &program, message, sizeof(message));
    free(source);
    int32_t code = 0;
    if (!status) status = bolt_interpreter_run(program, NULL, &code, message, sizeof(message));
    bolt_interpreter_release(program);
    if (status) fprintf(stderr, "%s\n", message);
    return status ? 1 : (int)code;
}
int main(int argc, char **argv) {
    if (argc == 2) return run_fixture(argv[1]);
    char error[512] = "";
    const char source[] =
        "import System\n"
        "public static class Program { public: static int calls; "
        "int Main(string[] args) { calls++; Console.WriteLine(Console.ReadLine()); return calls + args.Length; } }";
    BoltInterpretedProgram *program = 0, *bad = 0;
    CHECK(bolt_interpreter_abi() == BOLT_INTERPRETER_ABI);
    CHECK(bolt_interpreter_compile(0, 0, source, sizeof(source)-1, 0, &bad, error, sizeof(error)) == BOLT_INTERPRETER_ERROR);
    CHECK(!bad && strstr(error,"ABI"));
    CHECK(bolt_interpreter_compile(BOLT_INTERPRETER_ABI, "Scene.bolt", source, sizeof(source)-1, 0, &program, error, sizeof(error)) == BOLT_INTERPRETER_OK);
    CHECK(program && !error[0]);
    const char *args[] = {"a", "b"};
    BoltInterpreterIO io = {"in\ntrailing", 3, args, 2, collect, output};
    int32_t code = -1;
    CHECK(bolt_interpreter_run(program, &io, &code, error, sizeof(error)) == BOLT_INTERPRETER_OK);
    CHECK(code == 3 && strcmp(output,"in\n") == 0);
    io.arguments = 0; io.argument_count = 0;
    CHECK(bolt_interpreter_run(program, &io, &code, error, sizeof(error)) == BOLT_INTERPRETER_OK);
    CHECK(code == 2 && strcmp(output,"in\nin\n") == 0);
    io.input = 0;
    CHECK(bolt_interpreter_run(program, &io, &code, error, sizeof(error)) == BOLT_INTERPRETER_ERROR);
    CHECK(code == 0 && error[0]);
    CHECK(bolt_interpreter_compile(BOLT_INTERPRETER_ABI, "Broken.bolt", "class", 5, 0, &bad, error, sizeof(error)) == BOLT_INTERPRETER_ERROR);
    CHECK(!bad && strstr(error,"Broken.bolt"));
    CHECK(bolt_interpreter_run(program, 0, &code, error, sizeof(error)) == BOLT_INTERPRETER_OK && code == 3);
    bolt_interpreter_release(program);

    const char loop[] = "public static class Program { public: int Main() { int n=0; while(n<100000)n++; return n; } }";
    BoltInterpreterLimits limits = {40, 128, 16};
    CHECK(bolt_interpreter_compile(BOLT_INTERPRETER_ABI, "Budget.bolt", loop, sizeof(loop)-1, &limits, &program, error, sizeof(error)) == BOLT_INTERPRETER_OK);
    CHECK(bolt_interpreter_run(program, 0, &code, error, sizeof(error)) == BOLT_INTERPRETER_ERROR);
    CHECK(code == 0 && strstr(error,"budget"));
    bolt_interpreter_release(program);
    BoltInterpreterCompiler *compiler = 0;
    CHECK(bolt_interpreter_compiler_create(0, &compiler, error, sizeof(error)) == BOLT_INTERPRETER_ERROR && !compiler);
    CHECK(bolt_interpreter_compiler_create(BOLT_INTERPRETER_ABI, &compiler, error, sizeof(error)) == BOLT_INTERPRETER_OK && compiler);
    const char first[] = "public static class Program { public: static int calls; int Main(){ return ++calls; } }";
    const char second[] = "public static class Program { public: int Main(){ return 42; } }";
    const char invalid[] = "public static class Program { public: int Main(){ return missing; } }";
    BoltInterpretedProgram *original = 0, *edited = 0;
    CHECK(bolt_interpreter_compile_cached(compiler, "Scene.bolt", first, sizeof(first)-1, 0, &original, error, sizeof(error)) == BOLT_INTERPRETER_OK);
    CHECK(bolt_interpreter_run(original, 0, &code, error, sizeof(error)) == BOLT_INTERPRETER_OK && code == 1);
    CHECK(bolt_interpreter_compile_cached(compiler, "Scene.bolt", "class", 5, 0, &bad, error, sizeof(error)) == BOLT_INTERPRETER_ERROR && !bad);
    CHECK(bolt_interpreter_compile_cached(compiler, "Scene.bolt", invalid, sizeof(invalid)-1, 0, &bad, error, sizeof(error)) == BOLT_INTERPRETER_ERROR && !bad);
    CHECK(strstr(error,"Scene.bolt"));
    CHECK(bolt_interpreter_run(original, 0, &code, error, sizeof(error)) == BOLT_INTERPRETER_OK && code == 2);
    CHECK(bolt_interpreter_compile_cached(compiler, "Scene.bolt", second, sizeof(second)-1, 0, &edited, error, sizeof(error)) == BOLT_INTERPRETER_OK);
    bolt_interpreter_compiler_release(compiler);
    CHECK(bolt_interpreter_run(edited, 0, &code, error, sizeof(error)) == BOLT_INTERPRETER_OK && code == 42);
    CHECK(bolt_interpreter_run(original, 0, &code, error, sizeof(error)) == BOLT_INTERPRETER_OK && code == 3);
    const char random[] =
        "import System\nimport System.Security.Cryptography\n"
        "public static class Program { public: int Main() {"
        "byte[] empty=RandomNumberGenerator.GetBytes(0); RandomNumberGenerator.Fill(empty);"
        "byte value=RandomNumberGenerator.GetByte(); byte[] a=RandomNumberGenerator.GetBytes(513);"
        "byte[] b=new byte[513]; RandomNumberGenerator.Fill(b);"
        "if(empty.Length!=0 || a.Length!=b.Length) return 1;"
        "for(int i=0;i<a.Length;i++) if(a[i]!=b[i]) return 0; return 2; } }";
    BoltInterpretedProgram *random_program = 0;
    CHECK(bolt_interpreter_compile(BOLT_INTERPRETER_ABI, "Random.bolt", random, sizeof(random)-1,
                                   0, &random_program, error, sizeof(error)) == BOLT_INTERPRETER_OK);
    CHECK(bolt_interpreter_run(random_program, 0, &code, error, sizeof(error)) == BOLT_INTERPRETER_OK && code == 0);
    bolt_interpreter_release(random_program);
    bolt_interpreter_release(original);
    bolt_interpreter_release(edited);
    bolt_interpreter_compiler_release(0);
    bolt_interpreter_release(0);
    puts("PASS C ABI: I/O, arguments, budget, repeated edits, rollback, independent program lifetime and OS random bytes");
    return 0;
}
