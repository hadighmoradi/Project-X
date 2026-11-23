#include "xfrontend/linker.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace xfrontend;

void link_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void link_case(const std::string& name, const std::function<void()>& body,
               int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    const auto start = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

ObjectFile make_object(const std::string& id, const ObjectFileDescriptor& target,
                       std::size_t section_size, std::size_t alignment, std::uint8_t fill) {
    ObjectFile object;
    object.object_id = id;
    object.descriptor = target;
    ObjectSection section;
    section.id = 1U;
    section.name = ".text." + id;
    section.kind = ObjectSectionKind::Text;
    section.alignment = alignment;
    section.size = section_size;
    section.contents.assign(section_size, fill);
    object.sections.push_back(std::move(section));
    return object;
}

void add_defined(ObjectFile& object, const std::string& name, std::size_t value,
                 std::size_t size = 8U, ObjectSymbolBinding binding = ObjectSymbolBinding::Global) {
    ObjectSymbol symbol;
    symbol.name = name;
    symbol.signature_key = "fn()->void";
    symbol.binding = binding;
    symbol.kind = ObjectSymbolKind::Function;
    symbol.defined = true;
    symbol.section_id = 1U;
    symbol.value = value;
    symbol.size = size;
    object.symbols.push_back(std::move(symbol));
}

void add_undefined(ObjectFile& object, const std::string& name,
                   ObjectSymbolBinding binding = ObjectSymbolBinding::Global) {
    ObjectSymbol symbol;
    symbol.name = name;
    symbol.signature_key = "fn()->void";
    symbol.binding = binding;
    symbol.kind = ObjectSymbolKind::Function;
    symbol.defined = false;
    object.symbols.push_back(std::move(symbol));
}

std::uint32_t read_u32_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    link_require(offset <= bytes.size() && bytes.size() - offset >= 4U, "32-bit read outside image");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t read_u64_at(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    link_require(offset <= bytes.size() && bytes.size() - offset >= 8U, "64-bit read outside image");
    std::uint64_t value = 0U;
    for (unsigned i = 0U; i < 8U; ++i)
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8U);
    return value;
}

bool has_link_diagnostic(const std::vector<Diagnostic>& diagnostics, const std::string& code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&code](const Diagnostic& item) {
        return item.code == code;
    });
}

LinkedImageSymbol linked_symbol(const LinkedImage& image, const std::string& name) {
    for (const auto& symbol : image.symbols) if (symbol.name == name) return symbol;
    throw std::runtime_error("linked symbol not found: " + name);
}

LinkedImageSection linked_section(const LinkedImage& image, const std::string& object_id) {
    for (const auto& section : image.sections) if (section.object_id == object_id) return section;
    throw std::runtime_error("linked section not found: " + object_id);
}

} // namespace

void run_linker_tests(int& passed, int& failed) {
    // N24A: absolute relocations, stable object order, alignment and image materialization.
    for (int i = 0; i < 500; ++i) {
        link_case("N24A" + std::to_string(i + 1) + "_absolute_relocation_layout", [i] {
            const bool coff = (i % 2) != 0;
            const auto target = coff ? make_coff_x86_64_object_target() : make_elf_x86_64_object_target();
            auto caller = make_object("caller-" + std::to_string(i), target, 40U, std::size_t{1} << (i % 6), 0xA5U);
            add_defined(caller, "main", 0U, 16U);
            add_defined(caller, "local_helper", 32U, 4U, ObjectSymbolBinding::Local);
            add_undefined(caller, "callee");
            add_undefined(caller, "optional_weak", ObjectSymbolBinding::Weak);
            ObjectRelocation relocation;
            relocation.section_id = 1U;
            relocation.offset = 8U;
            relocation.kind = coff ? ObjectRelocationKind::CoffAddress64 : ObjectRelocationKind::Absolute64;
            relocation.target_symbol = "callee";
            relocation.addend = static_cast<std::int64_t>((i % 61) - 30);
            relocation.width = 8U;
            caller.relocations.push_back(relocation);
            ObjectRelocation local_relocation = relocation;
            local_relocation.offset = 16U;
            local_relocation.target_symbol = "local_helper";
            local_relocation.addend = -2;
            caller.relocations.push_back(local_relocation);
            ObjectRelocation weak_relocation = relocation;
            weak_relocation.offset = 24U;
            weak_relocation.target_symbol = "optional_weak";
            weak_relocation.addend = 0;
            caller.relocations.push_back(weak_relocation);

            auto callee = make_object("callee-" + std::to_string(i), target, 48U,
                std::size_t{1} << ((i / 3) % 8), static_cast<std::uint8_t>(i & 0xff));
            const std::size_t value = static_cast<std::size_t>(i % 24);
            add_defined(callee, "callee", value, 8U);
            ObjectSection bss;
            bss.id = 2U; bss.name = ".bss"; bss.kind = ObjectSectionKind::Bss;
            bss.alignment = 16U; bss.size = static_cast<std::size_t>(16 + (i % 16));
            callee.sections.push_back(bss);

            LinkerOptions options;
            options.entry_symbol = "main";
            options.image_base = 0x400000ULL + static_cast<std::uint64_t>(i) * 0x1000ULL;
            const auto linked = link_object_files(target, {caller, callee}, {}, options);
            link_require(linked.succeeded(), "absolute relocation link failed");
            link_require(linked.image.applied_relocations.size() == 3U, "global, local and weak relocations must be applied");
            const auto symbol = linked_symbol(linked.image, "callee");
            const auto local_symbol = linked_symbol(linked.image, "local_helper");
            link_require(linked.image.has_entry && linked.image.entry_address == linked_symbol(linked.image, "main").address,
                         "requested entrypoint was not resolved");
            const auto weak_applied = std::find_if(linked.image.applied_relocations.begin(), linked.image.applied_relocations.end(),
                [](const AppliedRelocation& item) { return item.target_symbol == "optional_weak"; });
            link_require(weak_applied != linked.image.applied_relocations.end() && weak_applied->weak_zero_fallback,
                         "unresolved weak symbol did not use the configured zero fallback");
            const auto bss_it = std::find_if(linked.image.sections.begin(), linked.image.sections.end(),
                [&callee](const LinkedImageSection& section) {
                    return section.object_id == callee.object_id && section.kind == ObjectSectionKind::Bss;
                });
            link_require(bss_it != linked.image.sections.end(), "BSS section missing from linked image");
            link_require(std::all_of(bss_it->contents.begin(), bss_it->contents.end(),
                [](std::uint8_t item) { return item == 0U; }), "BSS image contents must be zero-filled");
            const auto source = linked_section(linked.image, caller.object_id);
            const auto field = static_cast<std::size_t>(source.address - linked.image.image_base) + 8U;
            const auto expected = static_cast<std::uint64_t>(static_cast<std::int64_t>(symbol.address) + relocation.addend);
            link_require(read_u64_at(linked.image.flat_bytes, field) == expected, "absolute relocation value mismatch");
            const auto local_field = static_cast<std::size_t>(source.address - linked.image.image_base) + 16U;
            const auto local_expected = static_cast<std::uint64_t>(static_cast<std::int64_t>(local_symbol.address) - 2);
            link_require(read_u64_at(linked.image.flat_bytes, local_field) == local_expected,
                         "local symbol relocation mismatch");
            link_require(linked.image.image_size <= options.maximum_image_size, "image exceeded configured size");
            link_require(source.address % source.alignment == 0U, "source section alignment was not preserved");
        }, passed, failed);
    }

    // N24B: ELF PC32/PLT32 and COFF REL32 formulas, including the COFF +4 bias.
    for (int i = 0; i < 500; ++i) {
        link_case("N24B" + std::to_string(i + 1) + "_pc_relative_formula", [i] {
            const int mode = i % 3;
            const bool coff = mode == 2;
            const auto target = coff ? make_coff_x86_64_object_target() : make_elf_x86_64_object_target();
            auto caller = make_object("pc-caller-" + std::to_string(i), target, 32U, 16U, 0x11U);
            add_defined(caller, "pc_main", 0U, 16U);
            add_undefined(caller, "pc_target");
            ObjectRelocation relocation;
            relocation.section_id = 1U;
            relocation.offset = 4U;
            relocation.kind = coff ? ObjectRelocationKind::CoffRelative32
                : (mode == 1 ? ObjectRelocationKind::PltRelative32 : ObjectRelocationKind::PcRelative32);
            relocation.target_symbol = "pc_target";
            relocation.addend = static_cast<std::int64_t>((i % 127) - 63);
            relocation.width = 4U;
            caller.relocations.push_back(relocation);

            auto callee = make_object("pc-callee-" + std::to_string(i), target, 64U,
                std::size_t{1} << ((i / 5) % 5), 0x22U);
            add_defined(callee, "pc_target", static_cast<std::size_t>(i % 32), 8U);
            LinkerOptions options;
            options.image_base = 0x140000ULL + static_cast<std::uint64_t>(i) * 0x400ULL;
            const auto linked = link_object_files(target, {caller, callee}, {}, options);
            link_require(linked.succeeded(), "PC-relative link failed");
            link_require(linked.image.applied_relocations.size() == 1U, "one PC-relative relocation expected");
            const auto& applied = linked.image.applied_relocations.front();
            const auto bias = coff ? 4LL : 0LL;
            const auto expected = static_cast<std::int64_t>(applied.symbol_address) + relocation.addend -
                                  static_cast<std::int64_t>(applied.place_address) - bias;
            link_require(applied.signed_result == expected, "PC-relative formula mismatch");
            const auto& source = linked_section(linked.image, caller.object_id);
            const auto field = static_cast<std::size_t>(source.address - linked.image.image_base) + 4U;
            link_require(static_cast<std::int32_t>(read_u32_at(linked.image.flat_bytes, field)) == expected,
                         "PC-relative bytes do not contain computed displacement");
        }, passed, failed);
    }

    // N24C: archive extraction reaches a fixed point across reverse-ordered members.
    for (int i = 0; i < 500; ++i) {
        link_case("N24C" + std::to_string(i + 1) + "_archive_dependency_chain", [i] {
            const bool coff = (i % 2) != 0;
            const auto target = coff ? make_coff_x86_64_object_target() : make_elf_x86_64_object_target();
            auto main_object = make_object("archive-main-" + std::to_string(i), target, 24U, 16U, 0x33U);
            add_defined(main_object, "archive_main", 0U, 8U);
            add_undefined(main_object, "archive_api");
            ObjectRelocation main_reloc;
            main_reloc.section_id = 1U; main_reloc.offset = 0U;
            main_reloc.kind = coff ? ObjectRelocationKind::CoffAddress64 : ObjectRelocationKind::Absolute64;
            main_reloc.target_symbol = "archive_api"; main_reloc.width = 8U;
            main_object.relocations.push_back(main_reloc);

            auto api = make_object("archive-api-object-" + std::to_string(i), target, 32U, 8U, 0x44U);
            add_defined(api, "archive_api", static_cast<std::size_t>(i % 8), 8U);
            add_undefined(api, "archive_helper");
            ObjectRelocation api_reloc;
            api_reloc.section_id = 1U; api_reloc.offset = 8U;
            api_reloc.kind = coff ? ObjectRelocationKind::CoffAddress64 : ObjectRelocationKind::Absolute64;
            api_reloc.target_symbol = "archive_helper"; api_reloc.width = 8U;
            api.relocations.push_back(api_reloc);

            auto helper = make_object("archive-helper-object-" + std::to_string(i), target, 40U, 16U, 0x55U);
            add_defined(helper, "archive_helper", static_cast<std::size_t>(i % 12), 8U);
            StaticArchive archive;
            archive.archive_id = "libN24-" + std::to_string(i);
            archive.members.push_back(StaticArchiveMember{"helper-member", helper});
            archive.members.push_back(StaticArchiveMember{"api-member", api});

            const auto linked = link_object_files(target, {main_object}, {archive});
            link_require(linked.succeeded(), "archive link failed");
            link_require(linked.link_plan.extracted_archive_members.size() == 2U,
                         "both archive members must be extracted through dependency closure");
            link_require(linked.link_plan.selected_object_ids.size() == 3U, "three objects must be selected");
            link_require(linked.image.applied_relocations.size() == 2U, "both cross-object relocations must be applied");
            link_require(linked_symbol(linked.image, "archive_api").address != 0U, "archive API symbol not laid out");
            link_require(linked_symbol(linked.image, "archive_helper").address != 0U, "archive helper symbol not laid out");
        }, passed, failed);
    }

    // N24D: linker failures are explicit and deterministic (duplicates, unresolved,
    // displacement overflow, image limit and invalid entrypoint).
    for (int i = 0; i < 500; ++i) {
        link_case("N24D" + std::to_string(i + 1) + "_linker_rejection_matrix", [i] {
            const auto target = (i % 2) == 0 ? make_elf_x86_64_object_target() : make_coff_x86_64_object_target();
            LinkerOptions options;
            options.image_base = 0x400000ULL;
            options.maximum_image_size = 1U << 20U;
            std::vector<ObjectFile> objects;
            const int scenario = i % 10;
            std::string expected_code;
            if (scenario == 0) {
                auto first = make_object("dup-a-" + std::to_string(i), target, 32U, 16U, 0U);
                auto second = make_object("dup-b-" + std::to_string(i), target, 32U, 16U, 0U);
                add_defined(first, "duplicate", 0U);
                add_defined(second, "duplicate", 0U);
                objects = {first, second};
                expected_code = "XOBJ0106";
            } else if (scenario == 1) {
                auto object = make_object("unresolved-" + std::to_string(i), target, 32U, 16U, 0U);
                add_defined(object, "unresolved_main", 0U);
                add_undefined(object, "missing_external");
                objects.push_back(object);
                expected_code = "XOBJ0107";
            } else if (scenario == 2) {
                auto caller = make_object("overflow-caller-" + std::to_string(i), target, 16U, 8U, 0U);
                add_defined(caller, "overflow_main", 0U);
                add_undefined(caller, "overflow_target");
                ObjectRelocation relocation;
                relocation.section_id = 1U; relocation.offset = 4U;
                relocation.kind = target.format == ObjectFileFormat::COFF
                    ? ObjectRelocationKind::CoffRelative32 : ObjectRelocationKind::PcRelative32;
                relocation.target_symbol = "overflow_target";
                relocation.addend = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max());
                relocation.width = 4U;
                caller.relocations.push_back(relocation);
                auto provider = make_object("overflow-provider-" + std::to_string(i), target, 32U, 8U, 0U);
                add_defined(provider, "overflow_target", 16U);
                objects = {caller, provider};
                expected_code = "XLINK0013";
            } else if (scenario == 3) {
                auto object = make_object("oversize-" + std::to_string(i), target, 128U, 16U, 0U);
                add_defined(object, "oversize_main", 0U);
                objects.push_back(object);
                options.maximum_image_size = 64U;
                expected_code = "XLINK0103";
            } else if (scenario == 4) {
                auto object = make_object("entry-" + std::to_string(i), target, 32U, 16U, 0U);
                add_defined(object, "not_entry", 0U);
                objects.push_back(object);
                options.entry_symbol = "missing_entry";
                expected_code = "XLINK0108";
            } else if (scenario == 5) {
                auto caller = make_object("abs-overflow-caller-" + std::to_string(i), target, 16U, 8U, 0U);
                add_defined(caller, "abs_overflow_main", 0U);
                add_undefined(caller, "abs_overflow_target");
                ObjectRelocation relocation;
                relocation.section_id = 1U; relocation.offset = 0U;
                relocation.kind = target.format == ObjectFileFormat::COFF
                    ? ObjectRelocationKind::CoffAddress64 : ObjectRelocationKind::Absolute64;
                relocation.target_symbol = "abs_overflow_target";
                relocation.addend = std::numeric_limits<std::int64_t>::max();
                relocation.width = 8U;
                caller.relocations.push_back(relocation);
                auto provider = make_object("abs-overflow-provider-" + std::to_string(i), target, 32U, 8U, 0U);
                add_defined(provider, "abs_overflow_target", 16U);
                objects = {caller, provider};
                options.image_base = std::numeric_limits<std::uint64_t>::max() - 128U;
                options.maximum_image_size = 1024U;
                expected_code = "XLINK0009";
            } else if (scenario == 6) {
                const auto unsupported_target = make_macho_aarch64_object_target();
                auto object = make_object("unsupported-" + std::to_string(i), unsupported_target, 32U, 16U, 0U);
                add_defined(object, "unsupported_entry", 0U);
                objects.push_back(object);
                const auto rejected = link_object_files(unsupported_target, objects, {}, options);
                link_require(!rejected.succeeded() && has_link_diagnostic(rejected.diagnostics, "XLINK0099"),
                             "N24 accepted an unimplemented Mach-O/AArch64 link target");
                return;
            } else if (scenario == 7) {
                auto object = make_object("weak-disabled-" + std::to_string(i), target, 16U, 8U, 0U);
                add_defined(object, "weak_disabled_main", 0U);
                add_undefined(object, "weak_disabled_symbol", ObjectSymbolBinding::Weak);
                ObjectRelocation relocation;
                relocation.section_id = 1U; relocation.offset = 0U;
                relocation.kind = target.format == ObjectFileFormat::COFF
                    ? ObjectRelocationKind::CoffAddress64 : ObjectRelocationKind::Absolute64;
                relocation.target_symbol = "weak_disabled_symbol"; relocation.width = 8U;
                object.relocations.push_back(relocation);
                objects.push_back(object);
                options.allow_unresolved_weak = false;
                expected_code = "XOBJ0109";
            } else if (scenario == 8) {
                options.maximum_image_size = 0U;
                auto object = make_object("zero-limit-" + std::to_string(i), target, 16U, 8U, 0U);
                add_defined(object, "zero_limit_main", 0U);
                objects.push_back(object);
                expected_code = "XLINK0100";
            } else {
                ObjectFile empty;
                empty.object_id = "empty-image-" + std::to_string(i);
                empty.descriptor = target;
                objects.push_back(empty);
                expected_code = "XLINK0109";
            }
            const auto linked = link_object_files(target, objects, {}, options);
            link_require(!linked.succeeded(), "invalid link scenario was accepted");
            link_require(!expected_code.empty() && has_link_diagnostic(linked.diagnostics, expected_code),
                         "linker rejected a case for an unexpected reason");
            link_require(linked.image.flat_bytes.empty() || linked.image.image_size != 0U,
                         "failed link returned inconsistent flat image state");
        }, passed, failed);
    }
}
