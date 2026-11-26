#pragma once
#include <bolt/native/Layout.hpp>
#include <cstdint>

namespace bolt::host {
    struct Field {
        std::string name, type;
        uint32_t offset, size;
        bool visible;
    };
    struct Layout {
        uint32_t size = 0, alignment = 1;
        std::string error;
        std::vector<Field> fields;
    };

    inline std::unordered_map<std::string, Layout> layouts(const Program& program) {
        native_abi::ValueLayouts abi(program);
        std::unordered_map<std::string, Layout> result;
        auto describe = [&](auto&& self, Layout& out, const Class& type, const std::string& prefix, size_t base,
                            bool visible) -> void {
            if (type.is_native || !type.is_struct)
                throw std::runtime_error(type.name + " has owning or reference storage");
            for (const auto& method : type.methods) {
                if (method.is_destructor)
                    throw std::runtime_error(type.name + " has a destructor");
                for (const auto& attr : method.attributes)
                    if (attr.name == "CopyHook" || attr.name.ends_with(".CopyHook"))
                        throw std::runtime_error(type.name + " has a copy hook");
            }
            const auto& layout = abi.get(type.name);
            for (const auto& field : layout.fields) {
                const auto name = prefix + field.name;
                auto decl = std::find_if(type.fields.begin(), type.fields.end(),
                                         [&](const auto& f) { return f.name == field.name; });
                const bool exposed = visible && decl != type.fields.end() && decl->is_public;
                const auto* en = program.find_enum(field.type);
                const auto scalar = en ? en->underlying : field.type;
                const auto* nested = program.find_class(field.type);
                if (!(is_numeric(scalar) || scalar == "bool" || scalar == "char" || (nested && nested->is_struct)))
                    throw std::runtime_error(name + " is not a plain value field: " + field.type);
                auto bytes = abi.get(field.type).size;
                out.fields.push_back(
                    {name, scalar, static_cast<uint32_t>(base + field.offset), static_cast<uint32_t>(bytes), exposed});
                if (nested && nested->is_struct)
                    self(self, out, *nested, name + ".", base + field.offset, exposed);
            }
        };
        for (const auto& type : program.classes) {
            if (!type.is_struct || type.is_native || !type.generic_parameters.empty() || !type.pack_parameter.empty())
                continue;
            auto& record = result[type.name];
            try {
                const auto& layout = abi.get(type.name);
                if (layout.size > UINT32_MAX)
                    throw std::runtime_error("struct exceeds host layout size");
                record.size = static_cast<uint32_t>(layout.size);
                record.alignment = static_cast<uint32_t>(layout.alignment);
                describe(describe, record, type, "", 0, true);
            } catch (const std::exception& ex) {
                record.error = ex.what();
            }
        }
        return result;
    }
} // namespace bolt::host
