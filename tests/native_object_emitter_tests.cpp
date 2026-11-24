// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/native_object_emitter.hpp"
#include "xfrontend/binary_object.hpp"
#include "xfrontend/linker.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace xfrontend;

void n47_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void n47_case(const std::string& id, const std::function<void()>& body, int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(id)) return;
    const auto started = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        xfrontend_test_report::record(id, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    } catch (const std::exception& error) {
        ++failed;
        xfrontend_test_report::record(id, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    }
}

CompilationUnit parse_unit(const std::string& source) {
    auto parsed = parse_source(source);
    if (!parsed.succeeded()) {
        throw std::runtime_error("source parse failed: " + parsed.diagnostics.front().code + " " +
            parsed.diagnostics.front().message);
    }
    return std::move(parsed.unit);
}

NativeObjectRelocationInput function_call(std::size_t offset, std::size_t target_index) {
    NativeObjectRelocationInput relocation;
    relocation.offset = offset;
    relocation.kind = ObjectRelocationKind::CoffRelative32;
    relocation.width = 4U;
    relocation.target_function_index = target_index;
    relocation.target_kind = ObjectSymbolKind::Function;
    return relocation;
}


bool image_has_symbol(const LinkedImage& image, const std::string& name) {
    return std::any_of(image.symbols.begin(), image.symbols.end(), [&name](const LinkedImageSymbol& symbol) {
        return symbol.name == name;
    });
}

void source_function_sections(std::size_t index) {
    const auto unit = parse_unit(
        "module App;\n"
        "public function Main() returns int { return 0; }\n"
        "private function Helper() returns int { return 1; }\n"
        "private function Unused() returns int { return 2; }\n");
    NativeFunctionCodeArtifact main_code;
    main_code.function_index = 0U;
    main_code.code = {0xE8U, 0U, 0U, 0U, 0U, 0xC3U};
    main_code.relocations.push_back(function_call(1U, 1U));
    NativeFunctionCodeArtifact helper_code;
    helper_code.function_index = 1U;
    helper_code.code = {0xB8U, static_cast<std::uint8_t>(index & 0xffU), 0U, 0U, 0U, 0xC3U};
    NativeFunctionCodeArtifact dead_code;
    dead_code.function_index = 2U;
    dead_code.code = {0xB8U, 0x7fU, 0U, 0U, 0U, 0xC3U};

    const auto emitted = emit_native_object_sections(unit, {main_code, helper_code, dead_code}, {},
        NativeObjectEmissionOptions{"n47_app_source_" + std::to_string(index), make_coff_x86_64_object_target()});
    n47_require(emitted.succeeded(), "source function section emission failed");
    n47_require(emitted.object_file.sections.size() == 3U, "each source function must have its own section");
    n47_require(emitted.object_file.symbols.size() == 3U, "each source function must have its own symbol");
    n47_require(emitted.object_file.relocations.size() == 1U, "source call must produce exactly one relocation");
    n47_require(std::all_of(emitted.object_file.sections.begin(), emitted.object_file.sections.end(),
        [](const ObjectSection& section) { return section.comdat; }),
        "COFF function sections must be independently collectible COMDAT sections");
    const auto binary = write_binary_object(emitted.object_file);
    n47_require(binary.succeeded(), "source-emitted COFF object serialization failed");
    const auto roundtrip = read_binary_object(binary.bytes);
    n47_require(roundtrip.succeeded(), "source-emitted COFF object roundtrip failed");
    n47_require(roundtrip.object_file.sections.size() == 3U &&
        std::all_of(roundtrip.object_file.sections.begin(), roundtrip.object_file.sections.end(),
            [](const ObjectSection& section) { return section.comdat; }),
        "serialized COFF sections must preserve COMDAT flags");

    LinkerOptions options;
    options.entry_symbol = emitted.object_file.symbols[0U].name;
    const auto linked = link_object_files(emitted.object_file.descriptor, {emitted.object_file}, {}, options);
    n47_require(linked.succeeded(), "source-emitted object failed to link");
    n47_require(linked.image.optimization.retained_section_count == 2U &&
                linked.image.optimization.discarded_section_count == 1U,
                "source-to-object DCE retained/discarded section count mismatch");
    n47_require(linked.image.symbols.size() == 2U, "unused source function symbol must be removed");
    n47_require(linked.image.applied_relocations.size() == 1U, "live source call relocation must be applied");
}

void source_math_archive_sections(std::size_t index) {
    const auto app = parse_unit("module App; public function Main() returns int { return 0; }");
    const auto math = parse_unit(
        "module Math;\n"
        "public function Sin() returns float { return 0.0; }\n"
        "public function Cos() returns float { return 0.0; }\n"
        "public function Tan() returns float { return 0.0; }\n");

    std::vector<NativeFunctionCodeArtifact> math_code(3U);
    for (std::size_t function_index = 0U; function_index < math_code.size(); ++function_index) {
        math_code[function_index].function_index = function_index;
        math_code[function_index].code = {0xC3U};
    }
    const auto emitted_math = emit_native_object_sections(math, math_code, {},
        NativeObjectEmissionOptions{"n47_math_obj_" + std::to_string(index), make_coff_x86_64_object_target()});
    n47_require(emitted_math.succeeded(), "Math source object emission failed");
    n47_require(emitted_math.object_file.sections.size() == 3U, "Math functions must occupy three independent sections");
    n47_require(emitted_math.object_file.symbols.size() == 3U, "Math functions must occupy three independent symbols");
    n47_require(std::all_of(emitted_math.object_file.sections.begin(), emitted_math.object_file.sections.end(),
        [](const ObjectSection& section) { return section.comdat; }),
        "Math archive functions must be COMDAT individually");
    const std::string math_sin_link_name = emitted_math.object_file.symbols[0U].name;

    NativeFunctionCodeArtifact main_code;
    main_code.function_index = 0U;
    main_code.code = {0xE8U, 0U, 0U, 0U, 0U, 0xC3U};
    NativeObjectRelocationInput sin_call;
    sin_call.offset = 1U;
    sin_call.kind = ObjectRelocationKind::CoffRelative32;
    sin_call.width = 4U;
    sin_call.target_link_name = math_sin_link_name;
    sin_call.target_signature_key = emitted_math.object_file.symbols[0U].signature_key;
    sin_call.target_kind = ObjectSymbolKind::Function;
    main_code.relocations.push_back(sin_call);
    const auto emitted_app = emit_native_object_sections(app, {main_code}, {},
        NativeObjectEmissionOptions{"n47_math_app_" + std::to_string(index), make_coff_x86_64_object_target()});
    n47_require(emitted_app.succeeded(), "Math caller source emission failed");

    StaticArchive archive;
    archive.archive_id = "Math.lib.N47." + std::to_string(index);
    archive.members.push_back(StaticArchiveMember{"math.obj", emitted_math.object_file});
    LinkerOptions options;
    options.entry_symbol = emitted_app.object_file.symbols[0U].name;
    const auto linked = link_object_files(emitted_app.object_file.descriptor,
        {emitted_app.object_file}, {archive}, options);
    if (!linked.succeeded()) {
        const std::string detail = linked.diagnostics.empty() ? "unknown linker diagnostic"
            : linked.diagnostics.front().code + ": " + linked.diagnostics.front().message;
        throw std::runtime_error("source-emitted Math archive failed to link: " + detail);
    }
    n47_require(linked.link_plan.extracted_archive_members.size() == 1U,
                "only the Math archive member needed by Main should be extracted");
    n47_require(linked.image.optimization.discarded_section_count == 2U,
                "unused source Math functions must be eliminated from extracted member");
    n47_require(linked.image.sections.size() == 2U && linked.image.symbols.size() == 2U,
                "final image must contain Main and Math.Sin only");
    n47_require(image_has_symbol(linked.image, math_sin_link_name), "used Math.Sin symbol must be retained");
}

void source_data_sections(std::size_t index) {
    const auto unit = parse_unit("module DataApp; public function Main() returns int { return 0; }");
    NativeSymbolDeclaration keep_decl;
    keep_decl.symbol_id = "dataapp#keep#" + std::to_string(index);
    keep_decl.module_name = "DataApp";
    keep_decl.source_name = "KeepData";
    keep_decl.return_type = "byte";
    keep_decl.visibility = DropSymbolVisibility::Private;
    keep_decl.linkage = DropSymbolLinkage::Internal;
    keep_decl.entity_kind = NativeSymbolEntityKind::Global;
    std::string mangle_reason;
    const std::string keep_name = mangle_native_symbol(keep_decl, &mangle_reason);
    n47_require(!keep_name.empty(), "unable to mangle test data symbol: " + mangle_reason);

    NativeFunctionCodeArtifact main_code;
    main_code.function_index = 0U;
    main_code.code = {0x48U, 0xB8U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0xC3U};
    NativeObjectRelocationInput data_reference;
    data_reference.offset = 2U;
    data_reference.kind = ObjectRelocationKind::CoffAddress64;
    data_reference.width = 8U;
    data_reference.target_link_name = keep_name;
    data_reference.target_kind = ObjectSymbolKind::Object;
    main_code.relocations.push_back(data_reference);

    NativeDataCodeArtifact keep_data;
    keep_data.declaration = keep_decl;
    keep_data.contents = {static_cast<std::uint8_t>(index & 0xffU), 2U, 3U, 4U};
    keep_data.logical_size = keep_data.contents.size();
    keep_data.read_only = true;
    NativeDataCodeArtifact dead_data;
    dead_data.declaration = keep_decl;
    dead_data.declaration.symbol_id += "#dead";
    dead_data.declaration.source_name = "DeadData";
    dead_data.contents.clear();
    dead_data.logical_size = 24U;
    dead_data.bss = true;

    NativeDataCodeArtifact registration_data;
    registration_data.declaration = keep_decl;
    registration_data.declaration.symbol_id += "#registration";
    registration_data.declaration.source_name = "RegistrationData";
    registration_data.contents = {9U, 8U, 7U, 6U};
    registration_data.logical_size = registration_data.contents.size();
    registration_data.linker_root = true;

    std::string registration_mangle_reason;
    const std::string registration_name = mangle_native_symbol(registration_data.declaration, &registration_mangle_reason);
    n47_require(!registration_name.empty(), "unable to mangle registration data symbol: " + registration_mangle_reason);
    const auto emitted = emit_native_object_sections(unit, {main_code}, {keep_data, dead_data, registration_data},
        NativeObjectEmissionOptions{"n47_data_obj_" + std::to_string(index), make_coff_x86_64_object_target()});
    n47_require(emitted.succeeded(), "source data section emission failed");
    n47_require(emitted.object_file.sections.size() == 4U, "Main and three globals must have separate sections");
    n47_require(std::all_of(emitted.object_file.sections.begin(), emitted.object_file.sections.end(),
        [](const ObjectSection& section) { return section.comdat; }),
        "COFF function and data sections must all be independently collectible COMDATs");
    const auto binary = write_binary_object(emitted.object_file);
    n47_require(binary.succeeded(), "source data COFF serialization failed");
    const auto roundtrip = read_binary_object(binary.bytes);
    n47_require(roundtrip.succeeded() && roundtrip.object_file.sections.size() == 4U &&
        std::all_of(roundtrip.object_file.sections.begin(), roundtrip.object_file.sections.end(),
            [](const ObjectSection& section) { return section.comdat; }),
        "COFF data COMDAT flags must survive binary round-trip");
    LinkerOptions options;
    options.entry_symbol = emitted.object_file.symbols[0U].name;
    const auto linked = link_object_files(emitted.object_file.descriptor, {emitted.object_file}, {}, options);
    n47_require(linked.succeeded(), "source data DCE link failed");
    n47_require(linked.image.optimization.retained_section_count == 3U &&
                linked.image.optimization.discarded_section_count == 1U,
                "referenced data and explicit registration root must remain; unreachable BSS must be eliminated");
    n47_require(linked.image.sections.size() == 3U && image_has_symbol(linked.image, registration_name),
        "Main, referenced global and explicit registration root must remain");
}

void reject_invalid_source_artifacts(std::size_t index) {
    const auto unit = parse_unit("module Bad; public function Main() returns int { return 0; }");
    NativeFunctionCodeArtifact invalid;
    invalid.function_index = 99U + index;
    invalid.code = {0xC3U};
    const auto emitted = emit_native_object_sections(unit, {invalid}, {},
        NativeObjectEmissionOptions{"n47_invalid_" + std::to_string(index), make_coff_x86_64_object_target()});
    n47_require(!emitted.succeeded(), "out-of-range source function artifact must be rejected");
    n47_require(!emitted.diagnostics.empty() && emitted.diagnostics[0U].code == "XNOBJ0007",
                "out-of-range source function diagnostic mismatch");

    const auto typed_unit = parse_unit("module Typed; public function Main() returns int { return 0; }");
    NativeFunctionCodeArtifact typed_main;
    typed_main.function_index = 0U;
    typed_main.code = {0xE8U, 0U, 0U, 0U, 0U, 0xE8U, 0U, 0U, 0U, 0U, 0xC3U};
    NativeObjectRelocationInput first_call;
    first_call.offset = 1U;
    first_call.kind = ObjectRelocationKind::CoffRelative32;
    first_call.width = 4U;
    first_call.target_link_name = "__X_MATH_SIN_IMPORT";
    first_call.target_signature_key = "float()";
    first_call.target_kind = ObjectSymbolKind::Function;
    NativeObjectRelocationInput conflicting_call = first_call;
    conflicting_call.offset = 6U;
    conflicting_call.target_signature_key = "int()";
    typed_main.relocations = {first_call, conflicting_call};
    const auto typed_result = emit_native_object_sections(typed_unit, {typed_main}, {},
        NativeObjectEmissionOptions{"n47_type_conflict_" + std::to_string(index), make_coff_x86_64_object_target()});
    n47_require(!typed_result.succeeded(), "same external symbol with conflicting signatures must be rejected");
    n47_require(std::any_of(typed_result.diagnostics.begin(), typed_result.diagnostics.end(),
        [](const Diagnostic& diagnostic) { return diagnostic.code == "XNOBJ0015"; }),
        "external symbol signature conflict diagnostic mismatch");
}

} // namespace

void run_native_object_emitter_tests(int& passed, int& failed) {
    for (std::size_t i = 0U; i < 500U; ++i) {
        n47_case("N47A" + std::to_string(i + 1U), [i] { source_function_sections(i); }, passed, failed);
        n47_case("N47B" + std::to_string(i + 1U), [i] { source_math_archive_sections(i); }, passed, failed);
        n47_case("N47C" + std::to_string(i + 1U), [i] { source_data_sections(i); }, passed, failed);
        n47_case("N47D" + std::to_string(i + 1U), [i] { reject_invalid_source_artifacts(i); }, passed, failed);
    }
}
