// Adapter for Funke's Scripting/NativeEngineAPI.hpp. Call tick on the engine thread.
#pragma once
#include "Scripting/NativeEngineAPI.hpp"
#include "bolt/CompilerHost.h"
#include <charconv>
#include <cmath>
#include <cstdint>
#include <stdexcept>

class BoltSceneScript {
    struct Frame {
        Funke::NativeEngineAPI& api;
        std::uint32_t entity;
        bool failed = false;
    };
    static inline thread_local Frame* frame = nullptr;
    struct Scope {
        Frame* previous;
        explicit Scope(Frame& current) : previous(frame) { frame = &current; }
        ~Scope() { frame = previous; }
    };
    BoltCompiler* compiler = nullptr;
    char message[2048]{};

    static std::uint64_t entity(std::uint64_t, std::uint64_t) noexcept {
        return frame ? frame->entity : 0;
    }
    static std::uint64_t deltaMicros(std::uint64_t, std::uint64_t) noexcept {
        if (!frame) return 0;
        try {
            const double micros = double(frame->api.GetDeltaTime()) * 1000000.;
            if (std::isfinite(micros) && micros >= 0 && micros <= UINT32_MAX)
                return static_cast<std::uint64_t>(std::round(micros));
        } catch (...) {}
        frame->failed = true;
        return 0;
    }
    static std::uint64_t moveRight(std::uint64_t id, std::uint64_t milliUnits) noexcept {
        if (!frame) return 0;
        if (id == frame->entity && milliUnits <= UINT32_MAX) {
            try {
                float x = 0, y = 0;
                frame->api.GetPosition(frame->entity, &x, &y);
                frame->api.SetPosition(frame->entity, x + float(milliUnits) / 1000.f, y);
                return 1;
            } catch (...) {}
        }
        frame->failed = true;
        return 0;
    }
    static std::uint64_t logValue(std::uint64_t, std::uint64_t value) noexcept {
        if (!frame) return 0;
        try {
            char digits[32]{};
            std::to_chars(digits, digits + sizeof(digits) - 1, value);
            frame->api.LogInfo(digits, "Bolt", 0);
            return 1;
        } catch (...) {
            frame->failed = true;
            return 0;
        }
    }
public:
    explicit BoltSceneScript(int profile = BOLT_HOST_DEBUG) {
        const BoltHostBinding bindings[] = {{"FunkeEntity", entity}, {"FunkeDeltaMicros", deltaMicros},
                                            {"FunkeMoveRight", moveRight}, {"FunkeLogValue", logValue}};
        if (bolt_compiler_create(BOLT_COMPILER_HOST_ABI, profile, bindings, 4,
                                 &compiler, message, sizeof(message)) != BOLT_HOST_OK)
            throw std::runtime_error(message);
    }
    ~BoltSceneScript() { bolt_compiler_destroy(compiler); }
    BoltSceneScript(const BoltSceneScript&) = delete;
    BoltSceneScript& operator=(const BoltSceneScript&) = delete;

    // Compile first. A syntax error leaves the active image intact.
    bool reload(const char* project) {
        BoltCompiledModule* next = nullptr;
        if (bolt_compiler_compile_project(compiler, project, &next, message, sizeof(message)))
            return false;
        const int status = bolt_compiler_load(compiler, next, message, sizeof(message));
        bolt_module_release(next);
        return status == BOLT_HOST_OK;
    }
    const char* diagnostic() const { return message; }

    int tick(Funke::NativeEngineAPI& api, std::uint32_t id) {
        if (!api.GetDeltaTime || !api.GetPosition || !api.SetPosition || !api.LogInfo)
            throw std::invalid_argument("Incomplete Funke engine API for this script");
        Frame current{api, id};
        Scope scope(current);
        std::int32_t code = 0;
        char error[2048]{};
        if (bolt_compiler_run(compiler, &code, error, sizeof(error))) throw std::runtime_error(error);
        if (current.failed) throw std::runtime_error("Funke callback failed");
        return code;
    }
};
