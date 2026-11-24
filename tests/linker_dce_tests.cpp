// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/linker.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace xfrontend;

void dce_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void dce_case(const std::string& id, const std::function<void()>& body, int& passed, int& failed) {
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

ObjectSection make_section(ObjectSectionId id, const std::string& name, std::size_t size,
                           std::uint8_t fill, ObjectSectionKind kind = ObjectSectionKind::Text,
                           bool root = false) {
    ObjectSection section;
    section.id = id;
    section.name = name;
    section.kind = kind;
    section.alignment = 16U;
    section.size = size;
    section.contents.assign(size, fill);
    section.linker_root = root;
    if (kind == ObjectSectionKind::Bss) section.contents.clear();
    return section;
}

void add_symbol(ObjectFile& object, const std::string& name, ObjectSectionId section_id,
                ObjectSymbolBinding binding = ObjectSymbolBinding::Global,
                ObjectSymbolKind kind = ObjectSymbolKind::Function) {
    ObjectSymbol symbol;
    symbol.name = name;
    symbol.signature_key = kind == ObjectSymbolKind::Function ? "fn()->i32" : "object";
    symbol.binding = binding;
    symbol.kind = kind;
    symbol.defined = true;
    symbol.section_id = section_id;
    symbol.value = 0U;
    const auto section = std::find_if(object.sections.begin(), object.sections.end(),
        [section_id](const ObjectSection& item) { return item.id == section_id; });
    if (section == object.sections.end()) throw std::runtime_error("test symbol section not found");
    symbol.size = section->size;
    object.symbols.push_back(std::move(symbol));
}

void add_undefined(ObjectFile& object, const std::string& name,
                   ObjectSymbolKind kind = ObjectSymbolKind::Function,
                   ObjectSymbolBinding binding = ObjectSymbolBinding::Global) {
    ObjectSymbol symbol;
    symbol.name = name;
    symbol.signature_key = kind == ObjectSymbolKind::Function ? "fn()->i32" : "object";
    symbol.binding = binding;
    symbol.kind = kind;
    symbol.defined = false;
    object.symbols.push_back(std::move(symbol));
}

void add_abs_relocation(ObjectFile& object, ObjectSectionId source_section, const std::string& target) {
    ObjectRelocation relocation;
    relocation.section_id = source_section;
    relocation.offset = 0U;
    relocation.kind = object.descriptor.format == ObjectFileFormat::COFF
        ? ObjectRelocationKind::CoffAddress64 : ObjectRelocationKind::Absolute64;
    relocation.target_symbol = target;
    relocation.addend = 0;
    relocation.width = 8U;
    object.relocations.push_back(std::move(relocation));
}

bool has_section(const LinkedImage& image, const std::string& object_id, ObjectSectionId section_id) {
    return std::any_of(image.sections.begin(), image.sections.end(),
        [&object_id, section_id](const LinkedImageSection& section) {
            return section.object_id == object_id && section.source_section_id == section_id;
        });
}

bool has_symbol(const LinkedImage& image, const std::string& name) {
    return std::any_of(image.symbols.begin(), image.symbols.end(), [&name](const LinkedImageSymbol& symbol) {
        return symbol.name == name;
    });
}

ObjectFile make_transitive_graph(std::size_t index) {
    ObjectFile object;
    object.object_id = "n46_graph_" + std::to_string(index);
    object.descriptor = make_coff_x86_64_object_target();
    object.sections.push_back(make_section(1U, ".text$main", 8U, 0x11U));
    object.sections.push_back(make_section(2U, ".text$helper", 8U, 0x22U));
    object.sections.push_back(make_section(3U, ".text$common", 8U, 0x33U));
    object.sections.push_back(make_section(4U, ".text$unused", 8U + (index % 64U), 0x44U));
    add_symbol(object, "N46Main", 1U);
    add_symbol(object, "N46Helper", 2U);
    add_symbol(object, "N46Common", 3U);
    add_symbol(object, "N46Unused", 4U);
    add_abs_relocation(object, 1U, "N46Helper");
    add_abs_relocation(object, 2U, "N46Common");
    add_undefined(object, "N46MissingDeadOnlyDependency");
    add_abs_relocation(object, 4U, "N46MissingDeadOnlyDependency");
    return object;
}

void transitive_reachability(std::size_t index) {
    auto object = make_transitive_graph(index);
    LinkerOptions options;
    options.entry_symbol = "N46Main";
    const auto linked = link_object_files(object.descriptor, {object}, {}, options);
    dce_require(linked.succeeded(), "transitive DCE link failed");
    dce_require(linked.image.optimization.section_elimination_applied,
                "entry-rooted section elimination was not applied");
    dce_require(linked.image.optimization.input_section_count == 4U &&
                linked.image.optimization.retained_section_count == 3U &&
                linked.image.optimization.discarded_section_count == 1U,
                "transitive reachability retained/discarded section counts mismatch");
    dce_require(has_section(linked.image, object.object_id, 1U) &&
                has_section(linked.image, object.object_id, 2U) &&
                has_section(linked.image, object.object_id, 3U),
                "entry, helper and transitive dependency sections must remain");
    dce_require(!has_section(linked.image, object.object_id, 4U) &&
                !has_symbol(linked.image, "N46Unused"),
                "unreferenced function section and symbol must be eliminated");
    dce_require(linked.image.applied_relocations.size() == 2U,
                "only relocations from retained sections must be applied");
    dce_require(linked.image.optimization.discarded_section_bytes == 8U + (index % 64U),
                "discarded byte accounting mismatch");
}

void archive_function_granularity(std::size_t index) {
    const auto target = make_coff_x86_64_object_target();
    ObjectFile caller;
    caller.object_id = "n46_archive_caller_" + std::to_string(index);
    caller.descriptor = target;
    caller.sections.push_back(make_section(1U, ".text$entry", 8U, 0x51U));
    add_symbol(caller, "N46ArchiveEntry", 1U);
    add_undefined(caller, "MathSin");
    add_abs_relocation(caller, 1U, "MathSin");

    ObjectFile math;
    math.object_id = "n46_math_archive_member_" + std::to_string(index);
    math.descriptor = target;
    math.sections.push_back(make_section(1U, ".text$sin", 8U + (index % 32U), 0x61U));
    math.sections.push_back(make_section(2U, ".text$cos", 8U + (index % 48U), 0x62U));
    math.sections.push_back(make_section(3U, ".text$tan", 8U + (index % 56U), 0x63U));
    add_symbol(math, "MathSin", 1U);
    add_symbol(math, "MathCos", 2U);
    add_symbol(math, "MathTan", 3U);

    StaticArchive archive;
    archive.archive_id = "n46_math_archive_" + std::to_string(index);
    archive.members.push_back(StaticArchiveMember{"math-functions.obj", math});

    LinkerOptions options;
    options.entry_symbol = "N46ArchiveEntry";
    const auto linked = link_object_files(target, {caller}, {archive}, options);
    dce_require(linked.succeeded(), "Math archive DCE link failed");
    dce_require(linked.link_plan.extracted_archive_members.size() == 1U,
                "exactly the needed archive member should be extracted");
    dce_require(has_symbol(linked.image, "MathSin"), "used Math.Sin must remain");
    dce_require(!has_symbol(linked.image, "MathCos") && !has_symbol(linked.image, "MathTan"),
                "unused functions from the extracted archive member must be removed");
    dce_require(linked.image.optimization.discarded_section_count == 2U,
                "unused Math function sections were not eliminated");
}

void explicit_and_side_effect_roots(std::size_t index) {
    ObjectFile object;
    object.object_id = "n46_roots_" + std::to_string(index);
    object.descriptor = make_elf_x86_64_object_target();
    object.sections.push_back(make_section(1U, ".text$main", 8U, 0x71U));
    object.sections.push_back(make_section(2U, ".text$export", 8U, 0x72U));
    object.sections.push_back(make_section(3U, ".init_array$registration", 8U, 0x73U,
        ObjectSectionKind::ReadOnlyData, false));
    object.sections.push_back(make_section(4U, ".debug$kept", 8U, 0x74U, ObjectSectionKind::Metadata, true));
    object.sections.push_back(make_section(5U, ".text$dead", 8U, 0x75U));
    object.sections.push_back(make_section(6U, ".debug$dead", 8U, 0x76U, ObjectSectionKind::Metadata, false));
    add_symbol(object, "N46RootMain", 1U);
    add_symbol(object, "N46ExportedCallback", 2U);
    add_symbol(object, "N46DeadPrivate", 5U);

    LinkerOptions options;
    options.entry_symbol = "N46RootMain";
    options.preserved_symbols.push_back("N46ExportedCallback");
    const auto linked = link_object_files(object.descriptor, {object}, {}, options);
    dce_require(linked.succeeded(), "explicit root link failed");
    dce_require(has_section(linked.image, object.object_id, 1U) &&
                has_section(linked.image, object.object_id, 2U) &&
                has_section(linked.image, object.object_id, 3U) &&
                has_section(linked.image, object.object_id, 4U),
                "entry, exported, side-effect and explicitly rooted debug sections must remain");
    dce_require(!has_section(linked.image, object.object_id, 5U) &&
                !has_section(linked.image, object.object_id, 6U) &&
                !has_symbol(linked.image, "N46DeadPrivate"),
                "unreachable private function and unrooted debug metadata must be removed");
    dce_require(linked.image.optimization.retained_section_count == 4U &&
                linked.image.optimization.discarded_section_count == 2U,
                "root accounting mismatch");
}

void conservative_fallback_and_accounting(std::size_t index) {
    const auto target = (index & 1U) == 0U ? make_coff_x86_64_object_target() : make_elf_x86_64_object_target();
    auto object = make_transitive_graph(index + 9000U);
    object.descriptor = target;
    object.sections.back().linker_root = true; // Still not enough without an explicit entry/export root.
    if (target.format == ObjectFileFormat::ELF) {
        for (auto& relocation : object.relocations)
            relocation.kind = ObjectRelocationKind::Absolute64;
    }
    // This no-root case deliberately retains the full input, so remove the
    // intentionally unresolved reference used by the rooted dead-section tests.
    object.relocations.pop_back();
    object.symbols.erase(std::remove_if(object.symbols.begin(), object.symbols.end(),
        [](const ObjectSymbol& symbol) { return symbol.name == "N46MissingDeadOnlyDependency"; }), object.symbols.end());
    // Without a proven entry/export/side-effect root, the linker must preserve
    // all sections rather than guessing and risking removal of dynamic roots.
    LinkerOptions options;
    const auto linked = link_object_files(target, {object}, {}, options);
    dce_require(linked.succeeded(), "conservative fallback link failed");
    dce_require(!linked.image.optimization.section_elimination_applied,
                "DCE must not guess when no root set is available");
    dce_require(linked.image.optimization.input_section_count == 4U &&
                linked.image.optimization.retained_section_count == 4U &&
                linked.image.optimization.discarded_section_count == 0U,
                "conservative fallback must retain the complete input");
    dce_require(linked.image.optimization.input_section_bytes == linked.image.optimization.retained_section_bytes,
                "conservative fallback byte accounting mismatch");
}

} // namespace

void run_linker_dce_tests(int& passed, int& failed) {
    for (std::size_t i = 0U; i < 500U; ++i)
        dce_case("N46A" + std::to_string(i + 1U) + "_transitive_function_section_reachability",
            [i] { transitive_reachability(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i)
        dce_case("N46B" + std::to_string(i + 1U) + "_static_archive_math_function_granularity",
            [i] { archive_function_granularity(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i)
        dce_case("N46C" + std::to_string(i + 1U) + "_explicit_exports_metadata_side_effect_roots",
            [i] { explicit_and_side_effect_roots(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i)
        dce_case("N46D" + std::to_string(i + 1U) + "_conservative_fallback_and_size_accounting",
            [i] { conservative_fallback_and_accounting(i); }, passed, failed);
}
