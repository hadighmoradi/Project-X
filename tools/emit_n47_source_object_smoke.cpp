// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/native_object_emitter.hpp"
#include "xfrontend/linker.hpp"
#include "xfrontend/binary_object.hpp"
#include "xfrontend/archive.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
xfrontend::CompilationUnit parse_unit(const std::string& source) {
    auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) throw std::runtime_error(parsed.diagnostics.front().code + ": " + parsed.diagnostics.front().message);
    return std::move(parsed.unit);
}

void write_bytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot create " + path.string());
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("cannot write " + path.string());
}

void write_text(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("cannot create " + path.string());
    output << text << '\n';
    if (!output) throw std::runtime_error("cannot write " + path.string());
}
}

int main(int argc, char** argv) {
    using namespace xfrontend;
    try {
        const auto app_unit = parse_unit(
            "module App;\n"
            "public function Main() returns int { return 0; }\n"
            "public function DllMain() returns int { return 1; }\n");
        const auto math_unit = parse_unit(
            "module Math;\n"
            "public function Sin() returns float { return 0.0; }\n"
            "public function Cos() returns float { return 0.0; }\n"
            "public function Tan() returns float { return 0.0; }\n");

        std::vector<NativeFunctionCodeArtifact> math_code(3U);
        for (std::size_t index = 0U; index < math_code.size(); ++index) {
            math_code[index].function_index = index;
            math_code[index].code = {0xC3U};
        }
        // Fixture implementation returns 42 so the Windows loader consumer can
        // verify the complete call path without a C runtime dependency in DLL.
        math_code[0U].code = {0xB8U, 42U, 0U, 0U, 0U, 0xC3U};
        math_code[1U].code = {0xB8U, 17U, 0U, 0U, 0U, 0xC3U};
        math_code[2U].code = {0xB8U, 19U, 0U, 0U, 0U, 0xC3U};
        const auto math_object = emit_native_object_sections(math_unit, math_code, {},
            NativeObjectEmissionOptions{"n47_math_smoke", make_coff_x86_64_object_target()});
        if (!math_object.succeeded()) throw std::runtime_error(math_object.diagnostics.front().code + ": " + math_object.diagnostics.front().message);
        const std::string sin_link_name = math_object.object_file.symbols[0U].name;
        const std::string sin_signature = math_object.object_file.symbols[0U].signature_key;

        NativeFunctionCodeArtifact main_code;
        main_code.function_index = 0U;
        // x64 entry function reserves shadow space, calls Math.Sin, restores
        // the stack, and returns its EAX value to the consumer.
        main_code.code = {0x48U, 0x83U, 0xECU, 0x28U, 0xE8U, 0U, 0U, 0U, 0U,
                          0x48U, 0x83U, 0xC4U, 0x28U, 0xC3U};
        NativeObjectRelocationInput call;
        call.offset = 5U;
        call.kind = ObjectRelocationKind::CoffRelative32;
        call.width = 4U;
        call.target_link_name = sin_link_name;
        call.target_signature_key = sin_signature;
        call.target_kind = ObjectSymbolKind::Function;
        main_code.relocations.push_back(call);

        NativeFunctionCodeArtifact dll_main_code;
        dll_main_code.function_index = 1U;
        dll_main_code.code = {0xB8U, 1U, 0U, 0U, 0U, 0xC3U}; // BOOL TRUE
        const auto app_object = emit_native_object_sections(app_unit, {main_code, dll_main_code}, {},
            NativeObjectEmissionOptions{"n47_app_smoke", make_coff_x86_64_object_target()});
        if (!app_object.succeeded()) throw std::runtime_error(app_object.diagnostics.front().code + ": " + app_object.diagnostics.front().message);

        StaticArchive archive;
        archive.archive_id = "Math.lib.N47";
        archive.members.push_back(StaticArchiveMember{"math-functions.obj", math_object.object_file});
        LinkerOptions options;
        options.entry_symbol = app_object.object_file.symbols[1U].name; // DllMain
        options.preserved_symbols.push_back(app_object.object_file.symbols[0U].name); // exported XMain
        const auto linked = link_object_files(make_coff_x86_64_object_target(),
            {app_object.object_file}, {archive}, options);
        if (!linked.succeeded()) {
            const std::string message = linked.diagnostics.empty() ? "unknown linker failure"
                : linked.diagnostics.front().code + ": " + linked.diagnostics.front().message;
            throw std::runtime_error(message);
        }

        const auto has_symbol = [&linked](const std::string& name) {
            for (const auto& symbol : linked.image.symbols) if (symbol.name == name) return true;
            return false;
        };
        const std::string main_link_name = app_object.object_file.symbols[0U].name;
        const std::string entry_link_name = app_object.object_file.symbols[1U].name;
        const bool has_sin = has_symbol(sin_link_name);
        const bool correct = linked.image.optimization.section_elimination_applied &&
            linked.image.optimization.input_section_count == 5U &&
            linked.image.optimization.retained_section_count == 3U &&
            linked.image.optimization.discarded_section_count == 2U &&
            linked.image.optimization.discarded_section_bytes == 12U &&
            linked.image.sections.size() == 3U && has_sin && has_symbol(main_link_name) &&
            has_symbol(entry_link_name) && linked.link_plan.extracted_archive_members.size() == 1U &&
            linked.image.applied_relocations.size() == 1U &&
            math_object.object_file.sections.size() == 3U &&
            std::all_of(math_object.object_file.sections.begin(), math_object.object_file.sections.end(),
                [](const ObjectSection& section) { return section.comdat; });
        if (!correct) throw std::runtime_error("source-to-object COMDAT/DCE integration assertions failed");

        bool files_written = false;
        if (argc >= 2) {
            const std::filesystem::path output_dir(argv[1]);
            std::filesystem::create_directories(output_dir);
            const auto app_binary = write_binary_object(app_object.object_file);
            if (!app_binary.succeeded()) throw std::runtime_error(app_binary.diagnostics.front().message);
            const auto math_archive = write_object_archive(ArchiveFormat::CoffLib,
                {{"math-functions.obj", math_object.object_file}}, true);
            if (!math_archive.succeeded()) throw std::runtime_error(math_archive.diagnostics.front().message);
            const auto readback = read_binary_object(app_binary.bytes);
            if (!readback.succeeded() || readback.object_file.sections.size() != 2U ||
                !std::all_of(readback.object_file.sections.begin(), readback.object_file.sections.end(),
                    [](const ObjectSection& section) { return section.comdat; })) {
                throw std::runtime_error("serialized AMD64 COFF object failed COMDAT roundtrip validation");
            }
            const auto archive_readback = read_object_archive(math_archive.bytes, "n47-math-readback");
            if (!archive_readback.succeeded() || archive_readback.archive.members.size() != 1U ||
                archive_readback.archive.members.front().object_file.sections.size() != 3U) {
                throw std::runtime_error("COFF .lib archive failed object-aware roundtrip validation");
            }
            write_bytes(output_dir / "n47-app.obj", app_binary.bytes);
            write_bytes(output_dir / "Math.lib", math_archive.bytes);
            write_text(output_dir / "n47-entry-symbol.txt", entry_link_name);
            write_text(output_dir / "n47-main-symbol.txt", main_link_name);
            files_written = true;
        }

        std::cout << "N47_SOURCE_OBJECT_SMOKE | RESULT=PASSED"
                  << " | SOURCE_FUNCTIONS=5 | FUNCTION_SECTIONS=5"
                  << " | RETAINED=" << linked.image.optimization.retained_section_count
                  << " | REMOVED=" << linked.image.optimization.discarded_section_count
                  << " | MATH_SIN=RETAINED | MATH_COS=REMOVED | MATH_TAN=REMOVED"
                  << " | COMDAT=PASSED | COFF_ROUNDTRIP=PASSED | LIB_ROUNDTRIP=PASSED"
                  << " | MSVC_INPUTS=" << (files_written ? "WRITTEN" : "NOT_REQUESTED")
                  << " | BYTES_REMOVED=" << linked.image.optimization.discarded_section_bytes
                  << " | EXIT=0\n";
        if (files_written) {
            std::cout << "N47_APP_OBJECT=CREATED | N47_MATH_LIB=CREATED"
                      << " | ENTRY_SYMBOL=" << entry_link_name
                      << " | MAIN_SYMBOL=" << main_link_name << '\n';
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "N47_SOURCE_OBJECT_SMOKE | RESULT=FAILED | ERROR=" << error.what() << " | EXIT=2\n";
        return 2;
    }
}
