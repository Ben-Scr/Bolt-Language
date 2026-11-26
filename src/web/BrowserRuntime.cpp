#include <bolt/API.hpp>
#include <bolt/Diagnostics.hpp>
#include <bolt/Frontend.hpp>
#include <bolt/Version.hpp>
#include <emscripten/bind.h>
#include <sstream>
#include "../InterpreterSourceCache.hpp"

namespace {
using emscripten::val;

template<class Action>
val response(Action action) {
    val result = val::object();
    try {
        action(result);
        result.set("ok", true);
    } catch (const bolt::Error& error) {
        result.set("ok", false);
        result.set("message", std::string(error.what()));
        result.set("code", error.diagnostic.code);
        result.set("file", error.location.file);
        result.set("line", error.location.line);
        result.set("column", error.location.column);
    } catch (const std::exception& error) {
        result.set("ok", false);
        result.set("message", std::string(error.what()));
    }
    return result;
}

bolt::ExecutionOptions limits(unsigned max_steps) {
    bolt::ExecutionOptions options;
    options.max_steps = max_steps;
    return options;
}

class BrowserProgram {
    std::istringstream input_;
    std::ostringstream output_;
    bolt::Runtime runtime_;
public:
    BrowserProgram(bolt::Program program, unsigned steps)
        : runtime_(std::move(program), limits(steps), &input_, &output_) {}

    val run(const std::string& input, const val& arguments) {
        input_.str(input);
        input_.clear();
        output_.str({});
        output_.clear();
        auto result = response([&](val& result) {
            if (!val::global("Array").call<bool>("isArray", arguments))
                throw std::invalid_argument("Main arguments must be an array of strings.");
            std::vector<std::string> args;
            const unsigned count = arguments["length"].as<unsigned>();
            args.reserve(count);
            for (unsigned i = 0; i < count; ++i) {
                if (arguments[i].typeOf().as<std::string>() != "string")
                    throw std::invalid_argument("Each Main argument must be a string.");
                args.push_back(arguments[i].as<std::string>());
            }
            result.set("exitCode", runtime_.run(args));
        });
        result.set("stdout", output_.str());
        return result;
    }
};

val prepare(const std::string& source, unsigned max_steps) {
    return response([&](val& result) {
        auto program = bolt::compile_source(source, "Main.bolt");
        result.set("program", std::make_shared<BrowserProgram>(std::move(program), max_steps));
    });
}

class BrowserCompiler {
    bolt::InterpreterSourceCache cache_;
public:
    val prepare(const std::string& source, unsigned max_steps) {
        return response([&](val& result) {
            auto program = cache_.compile(source, "Main.bolt");
            result.set("program", std::make_shared<BrowserProgram>(std::move(program), max_steps));
        });
    }
};

val run_source(const std::string& source, const std::string& input, unsigned max_steps, bool check_only) {
    std::istringstream in(input);
    std::ostringstream out;
    auto result = response([&](val& value) {
        auto program = bolt::compile_source(source, "Main.bolt");
        int exit_code = 0;
        if (!check_only) {
            bolt::Runtime runtime(std::move(program), limits(max_steps), &in, &out);
            exit_code = runtime.run();
        }
        value.set("exitCode", exit_code);
    });
    result.set("stdout", out.str());
    return result;
}
}

EMSCRIPTEN_BINDINGS(bolt_browser_runtime) {
    emscripten::class_<BrowserProgram>("BoltProgram")
        .smart_ptr<std::shared_ptr<BrowserProgram>>("BoltProgramHandle")
        .function("run", &BrowserProgram::run);
    emscripten::class_<BrowserCompiler>("Compiler")
        .constructor<>()
        .function("prepare", &BrowserCompiler::prepare);
    emscripten::function("prepare", &prepare);
    emscripten::function("evaluate", &run_source);
    emscripten::constant("version", std::string(BOLT_VERSION_STRING));
}
