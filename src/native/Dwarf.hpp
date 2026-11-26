#pragma once
#include "Object.hpp"
#include <map>

namespace bolt::native_backend::detail {

struct DebugPosition {
    std::size_t address=0, file=0;
    int line=0, column=0;
};
struct DebugFunction {
    std::string name, linkage;
    std::size_t begin=0, end=0;
    std::vector<DebugPosition> positions;
};
struct DwarfSections {
    enum class Section { Text, Abbrev, Line };
    struct Relocation {
        std::size_t offset;
        Section target;
        std::uint64_t addend;
        unsigned width;
    };
    Bytes abbrev, info, line;
    std::vector<Relocation> info_relocations, line_relocations;
};

// An encoder owns the recorder only for a debug ELF AOT build. Positions are
// copied while the IR is alive and serialized after final byte offsets are known.
class DebugRecorder {
    std::map<std::string,std::size_t> file_numbers;
    std::vector<std::string> files;
    std::vector<DebugFunction> functions;
public:
    void begin(const ir::Function& function,std::size_t address);
    void position(const Location& at,std::size_t address);
    void end(std::size_t address);
    DwarfSections encode() const;
};
}
