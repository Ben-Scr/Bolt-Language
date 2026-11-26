#pragma once
#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace bolt {
// Public compiler locations use 1-based lines and UTF-8 byte columns.
// The language server converts them to LSP's 0-based UTF-16 positions.
struct Location {
    std::string file = "<source>";
    int line = 1;
    int column = 1;
    int length = 1;
    std::size_t offset = 0;
};
enum class Severity { Error, Warning, Information };
enum class DiagnosticPhase { Lexing, Parsing, Semantics, Runtime, Tooling };
enum class DiagnosticFormat { Text, MSBuild, Json };
struct DiagnosticNote { Location location; std::string message; };
struct Diagnostic {
    std::string code = "BLT2000";
    Severity severity = Severity::Error;
    DiagnosticPhase phase = DiagnosticPhase::Semantics;
    Location location;
    std::string message;
    std::string hint;
    std::string source_line;
    std::vector<DiagnosticNote> notes;
    std::vector<DiagnosticNote> frames;
};
class Error : public std::runtime_error {
public:
    Location location; // Retained for source compatibility with Bolt 0.2.
    Diagnostic diagnostic;
    Error(Location at, const std::string& message, std::string code = "BLT2000");
    explicit Error(Diagnostic value);
};
const char* severity_name(Severity value) noexcept;
const char* phase_name(DiagnosticPhase value) noexcept;
std::string source_line_at(std::string_view source, int line);
void attach_source(Diagnostic& diagnostic, std::string_view source);
std::string format_diagnostic(const Diagnostic& diagnostic, DiagnosticFormat format = DiagnosticFormat::Text);
std::string format_diagnostics_json(const std::vector<Diagnostic>& diagnostics);
Diagnostic runtime_diagnostic(Location at, std::string message);
Diagnostic tooling_diagnostic(std::string message);

// No allocations on the successful native path. Preserve both value and reference
// returns; the generated lambda specifies the Bolt expression's checked type.
struct NativeSourceLocation {
    const char* file;
    int line, column, length;
    Location location() const { return {file, line, column, length}; }
};
template<class Function>
decltype(auto) checked_at(NativeSourceLocation at, Function&& function) {
    try { return std::forward<Function>(function)(); }
    catch (const Error&) { throw; }
    catch (const std::exception& error) { throw Error(runtime_diagnostic(at.location(), error.what())); }
}
} // namespace bolt
