#include "xfrontend/linker.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace xfrontend;

ObjectSection make_section(ObjectSectionId id, const std::string& name, std::size_t size,
                           std::uint8_t fill) {
    ObjectSection section;
    section.id = id;
    section.name = name;
    section.kind = ObjectSectionKind::Text;
    section.alignment = 16U;
    section.size = size;
    section.contents.assign(size, fill);
    return section;
}

void add_function(ObjectFile& object, const std::string& name, ObjectSectionId section_id) {
    ObjectSymbol symbol;
    symbol.name = name;
    symbol.signature_key = "fn()->f64";
    symbol.binding = ObjectSymbolBinding::Global;
    symbol.kind = ObjectSymbolKind::Function;
    symbol.defined = true;
    symbol.section_id = section_id;
    symbol.value = 0U;
    const auto section = std::find_if(object.sections.begin(), object.sections.end(),
        [section_id](const ObjectSection& item) { return item.id == section_id; });
    if (section == object.sections.end()) throw std::runtime_error("section not found");
    symbol.size = section->size;
    object.symbols.push_back(std::move(symbol));
}

void add_undefined_function(ObjectFile& object, const std::string& name) {
    ObjectSymbol symbol;
    symbol.name = name;
    symbol.signature_key = "fn()->f64";
    symbol.binding = ObjectSymbolBinding::Global;
    symbol.kind = ObjectSymbolKind::Function;
    symbol.defined = false;
    object.symbols.push_back(std::move(symbol));
}

bool contains_symbol(const LinkedImage& image, const std::string& name) {
    return std::any_of(image.symbols.begin(), image.symbols.end(), [&name](const LinkedImageSymbol& symbol) {
        return symbol.name == name;
    });
}

} // namespace

int main() {
    using namespace xfrontend;
    const auto target = make_coff_x86_64_object_target();

    ObjectFile program;
    program.object_id = "n46_math_demo_program";
    program.descriptor = target;
    program.sections.push_back(make_section(1U, ".text$main", 8U, 0x11U));

    ObjectSymbol entry;
    entry.name = "N46DemoMain";
    entry.signature_key = "fn()->i32";
    entry.binding = ObjectSymbolBinding::Global;
    entry.kind = ObjectSymbolKind::Function;
    entry.defined = true;
    entry.section_id = 1U;
    entry.size = 8U;
    program.symbols.push_back(entry);
    add_undefined_function(program, "MathSin");

    ObjectRelocation call;
    call.section_id = 1U;
    call.offset = 0U;
    call.kind = ObjectRelocationKind::CoffAddress64;
    call.target_symbol = "MathSin";
    call.width = 8U;
    program.relocations.push_back(call);

    ObjectFile math;
    math.object_id = "n46_math_library_member";
    math.descriptor = target;
    math.sections.push_back(make_section(1U, ".text$sin", 8U, 0x21U));
    math.sections.push_back(make_section(2U, ".text$cos", 8U, 0x22U));
    math.sections.push_back(make_section(3U, ".text$tan", 8U, 0x23U));
    add_function(math, "MathSin", 1U);
    add_function(math, "MathCos", 2U);
    add_function(math, "MathTan", 3U);

    StaticArchive archive;
    archive.archive_id = "Math.lib";
    archive.members.push_back(StaticArchiveMember{"math-functions.obj", math});

    LinkerOptions options;
    options.entry_symbol = "N46DemoMain";
    const auto linked = link_object_files(target, {program}, {archive}, options);
    if (!linked.succeeded()) {
        std::cerr << "N46 DCE link failed";
        for (const auto& diagnostic : linked.diagnostics)
            std::cerr << "\n" << diagnostic.code << ": " << diagnostic.message;
        std::cerr << '\n';
        return 2;
    }

    const auto& report = linked.image.optimization;
    const bool valid = report.section_elimination_applied &&
        report.input_section_count == 4U && report.retained_section_count == 2U &&
        report.discarded_section_count == 2U && report.input_section_bytes == 32U &&
        report.retained_section_bytes == 16U && report.discarded_section_bytes == 16U &&
        contains_symbol(linked.image, "N46DemoMain") && contains_symbol(linked.image, "MathSin") &&
        !contains_symbol(linked.image, "MathCos") && !contains_symbol(linked.image, "MathTan") &&
        linked.link_plan.extracted_archive_members.size() == 1U;

    std::cout << "N46_DCE_SMOKE | RESULT=" << (valid ? "PASSED" : "FAILED")
              << " | INPUT_SECTIONS=" << report.input_section_count
              << " | RETAINED_SECTIONS=" << report.retained_section_count
              << " | DISCARDED_SECTIONS=" << report.discarded_section_count
              << " | INPUT_BYTES=" << report.input_section_bytes
              << " | RETAINED_BYTES=" << report.retained_section_bytes
              << " | DISCARDED_BYTES=" << report.discarded_section_bytes
              << " | MATH_SIN=" << (contains_symbol(linked.image, "MathSin") ? "RETAINED" : "MISSING")
              << " | MATH_COS=" << (contains_symbol(linked.image, "MathCos") ? "RETAINED" : "REMOVED")
              << " | MATH_TAN=" << (contains_symbol(linked.image, "MathTan") ? "RETAINED" : "REMOVED")
              << " | EXIT=" << (valid ? 0 : 1) << '\n';
    return valid ? 0 : 1;
}
