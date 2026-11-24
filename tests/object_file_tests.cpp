// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/object_file.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace xfrontend;

void object_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void object_case(const std::string& name, const std::function<void()>& body,
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

ObjectFileDescriptor descriptor_for(int index) {
    switch (index % 3) {
    case 0: return make_elf_x86_64_object_target();
    case 1: return make_coff_x86_64_object_target();
    default: return make_macho_aarch64_object_target();
    }
}

ObjectFile basic_object(const std::string& id, ObjectFileDescriptor descriptor) {
    ObjectFile object;
    object.object_id = id;
    object.descriptor = std::move(descriptor);
    ObjectSection text;
    text.id = 1U;
    text.name = ".text";
    text.kind = ObjectSectionKind::Text;
    text.alignment = 16U;
    text.size = 16U;
    text.contents.assign(16U, 0U);
    object.sections.push_back(text);
    ObjectSymbol function;
    function.name = "entry_" + id;
    function.signature_key = "fn()->void";
    function.binding = ObjectSymbolBinding::Global;
    function.kind = ObjectSymbolKind::Function;
    function.defined = true;
    function.section_id = 1U;
    function.value = 0U;
    function.size = 8U;
    object.symbols.push_back(function);
    return object;
}

ObjectSymbol undefined_symbol(const std::string& name, ObjectSymbolBinding binding = ObjectSymbolBinding::Global,
                              const std::string& signature = "") {
    ObjectSymbol symbol;
    symbol.name = name;
    symbol.signature_key = signature;
    symbol.binding = binding;
    symbol.kind = ObjectSymbolKind::Function;
    symbol.defined = false;
    return symbol;
}

ObjectSymbol defined_symbol(const std::string& name, ObjectSymbolBinding binding = ObjectSymbolBinding::Global,
                            const std::string& signature = "fn()->void") {
    ObjectSymbol symbol;
    symbol.name = name;
    symbol.signature_key = signature;
    symbol.binding = binding;
    symbol.kind = ObjectSymbolKind::Function;
    symbol.defined = true;
    symbol.section_id = 1U;
    symbol.value = 0U;
    symbol.size = 8U;
    return symbol;
}

ObjectRelocation relocation(const std::string& target, ObjectRelocationKind kind = ObjectRelocationKind::PcRelative32,
                            std::size_t offset = 4U, std::size_t width = 4U) {
    ObjectRelocation item;
    item.section_id = 1U;
    item.offset = offset;
    item.kind = kind;
    item.target_symbol = target;
    item.addend = -4;
    item.width = width;
    return item;
}

} // namespace

void run_object_file_tests(int& passed, int& failed) {
    // N22A: object format, target descriptor and section-layout validation.
    for (int i = 0; i < 500; ++i) {
        object_case("N22A" + std::to_string(i + 1) + "_object_descriptor_sections_case" + std::to_string(i), [=] {
            auto object = basic_object("descriptor" + std::to_string(i), descriptor_for(i));
            switch (i % 10) {
            case 0: {
                auto result = validate_object_file(object);
                object_require(result.succeeded(), "valid format/architecture profile was rejected");
                break;
            }
            case 1:
                object.descriptor.pointer_size = 4U;
                object_require(!validate_object_file(object).succeeded(), "32-bit pointer target was accepted");
                break;
            case 2:
                object.descriptor.byte_order = ObjectByteOrder::BigEndian;
                object_require(!validate_object_file(object).succeeded(), "unsupported big-endian target was accepted");
                break;
            case 3:
                object.descriptor.format = ObjectFileFormat::COFF;
                object.descriptor.operating_system = ObjectOperatingSystem::Linux;
                object.descriptor.target_triple = "x86_64-unknown-linux-gnu";
                object_require(!validate_object_file(object).succeeded(), "COFF/Linux descriptor mismatch was accepted");
                break;
            case 4:
                object.descriptor.architecture = object.descriptor.architecture == ObjectArchitecture::X86_64
                    ? ObjectArchitecture::AArch64 : ObjectArchitecture::X86_64;
                object_require(!validate_object_file(object).succeeded(), "architecture/triple mismatch was accepted");
                break;
            case 5: {
                ObjectSection duplicate = object.sections.front();
                duplicate.name = ".rodata";
                object.sections.push_back(duplicate);
                object_require(!validate_object_file(object).succeeded(), "duplicate section identifier was accepted");
                break;
            }
            case 6: {
                ObjectSection duplicate = object.sections.front();
                duplicate.id = 2U;
                object.sections.push_back(duplicate);
                object_require(!validate_object_file(object).succeeded(), "duplicate section name was accepted");
                break;
            }
            case 7:
                object.sections.front().alignment = 3U;
                object_require(!validate_object_file(object).succeeded(), "non-power-of-two section alignment was accepted");
                break;
            case 8: {
                ObjectSection bss;
                bss.id = 2U;
                bss.name = ".bss";
                bss.kind = ObjectSectionKind::Bss;
                bss.alignment = 8U;
                bss.size = 128U;
                bss.contents.push_back(0U);
                object.sections.push_back(bss);
                object_require(!validate_object_file(object).succeeded(), "BSS file contents were accepted");
                break;
            }
            default:
                object.sections.front().contents.pop_back();
                object_require(!validate_object_file(object).succeeded(), "section content-size mismatch was accepted");
                break;
            }
        }, passed, failed);
    }

    // N22B: object symbol, storage-range and relocation-record validation.
    for (int i = 0; i < 500; ++i) {
        object_case("N22B" + std::to_string(i + 1) + "_object_symbol_relocation_case" + std::to_string(i), [=] {
            auto object = basic_object("symbol" + std::to_string(i), make_elf_x86_64_object_target());
            object.sections.front().size = 32U;
            object.sections.front().contents.assign(32U, 0U);
            switch (i % 10) {
            case 0: {
                NativeSymbolRecord native;
                native.symbol_id = "native" + std::to_string(i);
                native.module_name = "Core.Object";
                native.canonical_source_name = "entry";
                native.mangled_name = "__Xv1_native_" + std::to_string(i);
                native.signature_key = "fn()->void";
                native.linkage = DropSymbolLinkage::External;
                auto adapted = make_object_symbol_from_native_record(native, ObjectSymbolKind::Function,
                    true, 1U, 0U, 8U);
                object.symbols.front() = adapted;
                object.relocations.push_back(relocation(adapted.name));
                object_require(validate_object_file(object).succeeded(), "N21 native symbol adapter produced an invalid object symbol");
                object_require(adapted.name == native.mangled_name && adapted.signature_key == native.signature_key,
                               "native symbol identity or signature was not preserved in object adapter");
                auto weak_undefined = make_object_symbol_from_native_record(native, ObjectSymbolKind::Function,
                    false, 1U, 9U, 9U, true);
                object_require(weak_undefined.binding == ObjectSymbolBinding::Weak && !weak_undefined.defined &&
                               weak_undefined.section_id == InvalidObjectSectionId && weak_undefined.value == 0U &&
                               weak_undefined.size == 0U,
                               "undefined weak native symbol retained storage coordinates");
                break;
            }
            case 1: {
                auto bad = defined_symbol("bad_section");
                bad.section_id = 99U;
                object.symbols.push_back(bad);
                object_require(!validate_object_file(object).succeeded(), "defined symbol with unknown section was accepted");
                break;
            }
            case 2: {
                auto bad = defined_symbol("bad_range");
                bad.value = 30U;
                bad.size = 8U;
                object.symbols.push_back(bad);
                object_require(!validate_object_file(object).succeeded(), "symbol extending past section was accepted");
                break;
            }
            case 3:
                object.symbols.push_back(defined_symbol(object.symbols.front().name));
                object_require(!validate_object_file(object).succeeded(), "duplicate external object definitions were accepted");
                break;
            case 4:
                object.symbols.push_back(undefined_symbol("local_undefined", ObjectSymbolBinding::Local));
                object_require(!validate_object_file(object).succeeded(), "undefined local symbol was accepted");
                break;
            case 5: {
                auto item = relocation(object.symbols.front().name);
                item.section_id = 7U;
                object.relocations.push_back(item);
                object_require(!validate_object_file(object).succeeded(), "relocation with unknown source section was accepted");
                break;
            }
            case 6:
                object.relocations.push_back(relocation("missing_symbol"));
                object_require(!validate_object_file(object).succeeded(), "relocation to absent symbol-table entry was accepted");
                break;
            case 7:
                object.descriptor = make_coff_x86_64_object_target();
                object.relocations.push_back(relocation(object.symbols.front().name, ObjectRelocationKind::PltRelative32));
                object_require(!validate_object_file(object).succeeded(), "ELF PLT relocation was accepted in COFF object");
                break;
            case 8:
                object.relocations.push_back(relocation(object.symbols.front().name, ObjectRelocationKind::PcRelative32, 4U, 8U));
                object_require(!validate_object_file(object).succeeded(), "relocation width mismatch was accepted");
                break;
            default: {
                auto item = relocation(object.symbols.front().name);
                object.relocations.push_back(item);
                object.relocations.push_back(item);
                object_require(!validate_object_file(object).succeeded(), "duplicate relocation record was accepted");
                break;
            }
            }
        }, passed, failed);
    }

    // N22C: global/weak resolution and fixed-point extraction of static archive members.
    for (int i = 0; i < 500; ++i) {
        object_case("N22C" + std::to_string(i + 1) + "_archive_extraction_resolution_case" + std::to_string(i), [=] {
            auto app = basic_object("app" + std::to_string(i), make_elf_x86_64_object_target());
            app.symbols.push_back(undefined_symbol("foo", ObjectSymbolBinding::Global, "fn()->void"));
            StaticArchive archive;
            archive.archive_id = "libCore";
            auto foo = basic_object("foo_member" + std::to_string(i), app.descriptor);
            foo.symbols.front().name = "foo";
            foo.symbols.push_back(undefined_symbol("bar", ObjectSymbolBinding::Global, "fn()->void"));
            archive.members.push_back(StaticArchiveMember{"foo.o", foo});
            auto bar = basic_object("bar_member" + std::to_string(i), app.descriptor);
            bar.symbols.front().name = "bar";
            archive.members.push_back(StaticArchiveMember{"bar.o", bar});

            switch (i % 10) {
            case 0: {
                auto result = build_object_link_plan(app.descriptor, {app}, {archive});
                object_require(result.succeeded(), "archive member for unresolved symbol was not extracted");
                object_require(result.extracted_archive_members.size() == 2U, "transitive archive dependency was not extracted");
                object_require(result.resolved_symbols.size() >= 2U, "archive definitions were not resolved");
                break;
            }
            case 1: {
                auto result = build_object_link_plan(app.descriptor, {app});
                object_require(!result.succeeded(), "unresolved strong symbol was accepted");
                break;
            }
            case 2: {
                auto top = basic_object("top" + std::to_string(i), app.descriptor);
                top.symbols.push_back(undefined_symbol("foo", ObjectSymbolBinding::Global, "fn()->void"));
                auto result = build_object_link_plan(app.descriptor, {top}, {archive});
                object_require(result.succeeded() && result.extracted_archive_members.size() == 2U,
                               "archive extraction did not reach a fixed point");
                break;
            }
            case 3: {
                auto weak = basic_object("weak" + std::to_string(i), app.descriptor);
                weak.symbols.push_back(undefined_symbol("optional", ObjectSymbolBinding::Weak));
                auto result = build_object_link_plan(app.descriptor, {weak});
                object_require(result.succeeded() && result.unresolved_weak_symbols.size() == 1U,
                               "unresolved weak symbol was not preserved as optional");
                break;
            }
            case 4: {
                auto second = basic_object("duplicate" + std::to_string(i), app.descriptor);
                second.symbols.front().name = app.symbols.front().name;
                auto first = basic_object("first" + std::to_string(i), app.descriptor);
                first.symbols.front().name = app.symbols.front().name;
                auto result = build_object_link_plan(app.descriptor, {first, second});
                object_require(!result.succeeded(), "duplicate strong definitions were accepted");
                break;
            }
            case 5: {
                auto weak = basic_object("weak_def" + std::to_string(i), app.descriptor);
                weak.symbols.front().name = "shared";
                weak.symbols.front().binding = ObjectSymbolBinding::Weak;
                auto strong = basic_object("strong_def" + std::to_string(i), app.descriptor);
                strong.symbols.front().name = "shared";
                auto caller = basic_object("caller" + std::to_string(i), app.descriptor);
                caller.symbols.push_back(undefined_symbol("shared", ObjectSymbolBinding::Global));
                auto result = build_object_link_plan(app.descriptor, {weak, strong, caller});
                object_require(result.succeeded(), "strong definition did not override weak definition");
                const auto found = std::find_if(result.resolved_symbols.begin(), result.resolved_symbols.end(),
                    [](const ObjectSymbolResolution& value) { return value.symbol_name == "shared"; });
                object_require(found != result.resolved_symbols.end() && found->object_id == strong.object_id,
                               "linker plan selected weak definition over strong definition");
                break;
            }
            case 6: {
                StaticArchive unused;
                unused.archive_id = "unused";
                unused.members.push_back(archive.members.back());
                auto self = basic_object("self" + std::to_string(i), app.descriptor);
                auto result = build_object_link_plan(app.descriptor, {self}, {unused});
                object_require(result.succeeded() && result.extracted_archive_members.empty(),
                               "unreferenced archive member was eagerly extracted");
                break;
            }
            case 7: {
                auto result = build_object_link_plan(app.descriptor, {app}, {archive}, ObjectLinkOptions{false, true});
                object_require(!result.succeeded() && result.extracted_archive_members.empty(),
                               "disabled archive extraction still selected a member");
                break;
            }
            case 8: {
                auto mismatch = basic_object("mismatch" + std::to_string(i), app.descriptor);
                mismatch.symbols.front().name = "foo";
                mismatch.symbols.front().signature_key = "fn(i32)->void";
                auto caller = basic_object("signature_caller" + std::to_string(i), app.descriptor);
                caller.symbols.push_back(undefined_symbol("foo", ObjectSymbolBinding::Global, "fn()->void"));
                auto result = build_object_link_plan(app.descriptor, {mismatch, caller});
                object_require(!result.succeeded(), "symbol signature mismatch was accepted");
                break;
            }
            default: {
                StaticArchive duplicate;
                duplicate.archive_id = "duplicate_archive";
                duplicate.members.push_back(archive.members.front());
                duplicate.members.push_back(archive.members.front());
                auto result = build_object_link_plan(app.descriptor, {app}, {duplicate});
                object_require(!result.succeeded(), "duplicate archive member identifiers were accepted");
                break;
            }
            }
        }, passed, failed);
    }

    // N22D: target compatibility and relocation resolution/fallback planning.
    for (int i = 0; i < 500; ++i) {
        object_case("N22D" + std::to_string(i + 1) + "_object_link_relocation_case" + std::to_string(i), [=] {
            auto caller = basic_object("caller" + std::to_string(i), make_elf_x86_64_object_target());
            caller.sections.front().size = 32U;
            caller.sections.front().contents.assign(32U, 0U);
            caller.symbols.push_back(undefined_symbol("callee", ObjectSymbolBinding::Global, "fn()->void"));
            caller.relocations.push_back(relocation("callee"));
            auto callee = basic_object("callee" + std::to_string(i), caller.descriptor);
            callee.symbols.front().name = "callee";

            switch (i % 10) {
            case 0: {
                auto result = build_object_link_plan(caller.descriptor, {caller, callee});
                object_require(result.succeeded() && result.relocations.size() == 1U &&
                               result.relocations.front().resolved &&
                               result.relocations.front().target_object_id == callee.object_id,
                               "cross-object relocation did not resolve");
                break;
            }
            case 1:
                callee.descriptor = make_coff_x86_64_object_target();
                object_require(!build_object_link_plan(caller.descriptor, {caller, callee}).succeeded(),
                               "mixed object format target was accepted");
                break;
            case 2:
                caller.relocations.front().kind = ObjectRelocationKind::CoffRelative32;
                object_require(!build_object_link_plan(caller.descriptor, {caller, callee}).succeeded(),
                               "COFF relocation kind was accepted by ELF object");
                break;
            case 3:
                caller.relocations.front().offset = 31U;
                object_require(!build_object_link_plan(caller.descriptor, {caller, callee}).succeeded(),
                               "out-of-range relocation field was accepted");
                break;
            case 4: {
                caller.symbols.back() = undefined_symbol("optional_callee", ObjectSymbolBinding::Weak);
                caller.relocations.front().target_symbol = "optional_callee";
                auto result = build_object_link_plan(caller.descriptor, {caller});
                object_require(result.succeeded() && result.relocations.front().weak_zero_fallback,
                               "weak unresolved relocation did not produce zero-fallback plan");
                break;
            }
            case 5: {
                caller.symbols.back() = undefined_symbol("optional_callee", ObjectSymbolBinding::Weak);
                caller.relocations.front().target_symbol = "optional_callee";
                auto result = build_object_link_plan(caller.descriptor, {caller}, {}, ObjectLinkOptions{true, false});
                object_require(!result.succeeded(), "disabled weak zero fallback was accepted");
                break;
            }
            case 6: {
                auto duplicate = callee;
                duplicate.object_id += "_duplicate";
                object_require(!build_object_link_plan(caller.descriptor, {caller, callee, duplicate}).succeeded(),
                               "multiple strong relocation targets were accepted");
                break;
            }
            case 7: {
                auto local_caller = basic_object("local" + std::to_string(i), caller.descriptor);
                auto local = defined_symbol("local_target", ObjectSymbolBinding::Local);
                local_caller.symbols.push_back(local);
                local_caller.relocations.push_back(relocation("local_target"));
                auto result = build_object_link_plan(local_caller.descriptor, {local_caller});
                object_require(result.succeeded() && result.relocations.front().resolved &&
                               result.relocations.front().target_object_id == local_caller.object_id,
                               "object-local symbol relocation did not resolve");
                break;
            }
            case 8: {
                auto arm = basic_object("arm" + std::to_string(i), make_macho_aarch64_object_target());
                arm.relocations.push_back(relocation(arm.symbols.front().name, ObjectRelocationKind::AArch64Call26, 2U, 4U));
                object_require(!validate_object_file(arm).succeeded(), "unaligned AArch64 instruction relocation was accepted");
                break;
            }
            default: {
                auto invalid_target = caller.descriptor;
                invalid_target.target_triple = "x86_64-unknown-windows-msvc";
                object_require(!build_object_link_plan(invalid_target, {caller}).succeeded(),
                               "target descriptor inconsistent with selected object was accepted");
                break;
            }
            }
        }, passed, failed);
    }
}
