#include <bolt/InterpreterHost.h>
#include <bolt/API.hpp>
#include <bolt/Diagnostics.hpp>
#include <bolt/Frontend.hpp>
#include "InterpreterSourceCache.hpp"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <sstream>

namespace {
void diagnostic(char* dest, std::size_t capacity, std::string_view message) noexcept {
    if (!dest || !capacity) return;
    auto size = std::min(capacity - 1, message.size());
    while (size && size < message.size() && (static_cast<unsigned char>(message[size]) & 0xc0) == 0x80) --size;
    if (size) std::memcpy(dest, message.data(), size);
    dest[size] = '\0';
}

template<class Operation>
int invoke(char* error, std::size_t capacity, Operation operation) noexcept {
    diagnostic(error, capacity, {});
    try {
        operation();
        return BOLT_INTERPRETER_OK;
    } catch (const bolt::Error& e) {
        try {
            diagnostic(error, capacity, bolt::format_diagnostic(e.diagnostic));
        } catch (...) {
            diagnostic(error, capacity, e.what());
        }
    } catch (const std::exception& e) {
        diagnostic(error, capacity, e.what());
    } catch (...) {
        diagnostic(error, capacity, "Unknown Bolt interpreter error");
    }
    return BOLT_INTERPRETER_ERROR;
}

class Output final : public std::streambuf {
public:
    BoltInterpreterWrite write = nullptr;
    void* context = nullptr;

protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        if (write && size > 0) write(context, data, static_cast<std::size_t>(size));
        return size;
    }
    int_type overflow(int_type ch) override {
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            const char c = traits_type::to_char_type(ch);
            xsputn(&c, 1);
        }
        return traits_type::not_eof(ch);
    }
};
}

struct BoltInterpretedProgram {
    std::mutex mutex;
    std::istringstream input;
    Output sink;
    std::ostream output{&sink};
    bolt::Runtime runtime;

    BoltInterpretedProgram(bolt::Program program, bolt::ExecutionOptions limits)
        : runtime(std::move(program), limits, &input, &output) {}
};

struct BoltInterpreterCompiler {
    bolt::InterpreterSourceCache cache;
};

namespace {
int compile_program(BoltInterpreterCompiler* compiler, const char* name, const char* source, size_t length,
                    const BoltInterpreterLimits* limits, BoltInterpretedProgram** out, char* error, size_t capacity) {
    if (out) *out = nullptr;
    return invoke(error, capacity, [&] {
        if (!out || (!source && length)) throw std::invalid_argument("Invalid Bolt interpreter compile arguments");
        bolt::ExecutionOptions options;
        if (limits) options = {limits->max_steps, limits->max_call_depth, limits->max_runtime_depth};
        const std::string_view text(source ? source : "", length);
        const std::string filename = name ? name : "<memory>";
        auto program = compiler ? compiler->cache.compile(text, filename) : bolt::compile_source(text, filename);
        *out = new BoltInterpretedProgram(std::move(program), options);
    });
}

int invalid_compile(const char* message, BoltInterpretedProgram** out, char* error, size_t capacity) {
    if (out) *out = nullptr;
    return invoke(error, capacity, [&] { throw std::invalid_argument(message); });
}
}

extern "C" {
uint32_t bolt_interpreter_abi(void) { return BOLT_INTERPRETER_ABI; }

int bolt_interpreter_compile(uint32_t abi, const char* name, const char* source, size_t length,
                             const BoltInterpreterLimits* limits, BoltInterpretedProgram** out,
                             char* error, size_t capacity) {
    if (abi != BOLT_INTERPRETER_ABI) return invalid_compile("Unsupported Bolt interpreter ABI",out,error,capacity);
    return compile_program(nullptr,name,source,length,limits,out,error,capacity);
}

int bolt_interpreter_compiler_create(uint32_t abi, BoltInterpreterCompiler** out, char* error, size_t capacity) {
    if (out) *out = nullptr;
    return invoke(error, capacity, [&] {
        if (abi != BOLT_INTERPRETER_ABI || !out) throw std::invalid_argument("Invalid Bolt interpreter compiler arguments");
        *out = new BoltInterpreterCompiler();
    });
}

int bolt_interpreter_compile_cached(BoltInterpreterCompiler* compiler, const char* name, const char* source, size_t length,
                                    const BoltInterpreterLimits* limits, BoltInterpretedProgram** out,
                                    char* error, size_t capacity) {
    if (!compiler) return invalid_compile("Missing Bolt interpreter compiler",out,error,capacity);
    return compile_program(compiler,name,source,length,limits,out,error,capacity);
}

void bolt_interpreter_compiler_release(BoltInterpreterCompiler* compiler) { delete compiler; }

int bolt_interpreter_run(BoltInterpretedProgram* program, const BoltInterpreterIO* io,
                         int32_t* exit_code, char* error, size_t capacity) {
    if (exit_code) *exit_code = 0;
    return invoke(error, capacity, [&] {
        if (!program || !exit_code) throw std::invalid_argument("Missing Bolt interpreter program or exit code");
        const BoltInterpreterIO empty{};
        const auto& in = io ? *io : empty;
        if ((!in.input && in.input_length) || (!in.arguments && in.argument_count))
            throw std::invalid_argument("Invalid Bolt interpreter input or argument array");
        std::vector<std::string> arguments;
        arguments.reserve(in.argument_count);
        for (std::size_t n = 0; n < in.argument_count; ++n) {
            if (!in.arguments[n]) throw std::invalid_argument("Null Bolt interpreter argument");
            arguments.emplace_back(in.arguments[n]);
        }
        std::lock_guard lock(program->mutex);
        program->input.clear();
        program->input.str(std::string(in.input ? in.input : "", in.input_length));
        program->output.clear();
        program->sink.write = in.write;
        program->sink.context = in.context;
        struct Disconnect {
            Output& sink;
            ~Disconnect() { sink.write = nullptr; sink.context = nullptr; }
        } disconnect{program->sink};
        *exit_code = program->runtime.run(arguments);
    });
}

void bolt_interpreter_release(BoltInterpretedProgram* program) { delete program; }
}
