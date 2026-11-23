#include "xfrontend/archive.hpp"
#include "xfrontend/binary_object.hpp"
#include "xfrontend/system_linker.hpp"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace xfrontend;

namespace {
ObjectFile make_object(ObjectFileFormat format, const std::string& id, const std::string& symbol,
                       const std::vector<std::uint8_t>& code) {
    ObjectFile object;
    object.object_id = id;
    object.descriptor = format == ObjectFileFormat::ELF
        ? make_elf_x86_64_object_target() : make_coff_x86_64_object_target();
    ObjectSection text;
    text.id = 1U;
    text.name = ".text";
    text.kind = ObjectSectionKind::Text;
    text.alignment = 16U;
    text.size = code.size();
    text.contents = code;
    object.sections.push_back(text);
    ObjectSymbol fn;
    fn.name = symbol;
    fn.signature_key = "fn()->i32";
    fn.binding = ObjectSymbolBinding::Global;
    fn.kind = ObjectSymbolKind::Function;
    fn.defined = true;
    fn.section_id = 1U;
    fn.value = 0U;
    fn.size = code.size();
    object.symbols.push_back(fn);
    return object;
}

bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) return false;
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}
}

int main() {
    const std::vector<std::uint8_t> ret_code{0xC3U};
    std::vector<std::pair<std::string, ObjectFile>> elf_objects;
    elf_objects.emplace_back("alpha.o", make_object(ObjectFileFormat::ELF, "alpha", "n31_alpha", ret_code));
    elf_objects.emplace_back("beta_with_a_long_archive_member_name.o",
        make_object(ObjectFileFormat::ELF, "beta", "n31_beta", ret_code));
    const auto elf_archive = write_object_archive(ArchiveFormat::GnuAr, elf_objects, true);
    if (!elf_archive.succeeded() || !write_file("n31_demo.a", elf_archive.bytes)) {
        std::cerr << "failed to emit GNU ar demo: " << (elf_archive.diagnostics.empty() ? "file write" : elf_archive.diagnostics.front().message) << '\n';
        return 1;
    }

    std::vector<std::pair<std::string, ObjectFile>> coff_objects;
    coff_objects.emplace_back("alpha.obj", make_object(ObjectFileFormat::COFF, "alpha", "n31_alpha", ret_code));
    coff_objects.emplace_back("beta_with_a_long_archive_member_name.obj",
        make_object(ObjectFileFormat::COFF, "beta", "n31_beta", ret_code));
    const auto coff_archive = write_object_archive(ArchiveFormat::CoffLib, coff_objects, true);
    if (!coff_archive.succeeded() || !write_file("n31_demo.lib", coff_archive.bytes)) {
        std::cerr << "failed to emit COFF LIB demo: " << (coff_archive.diagnostics.empty() ? "file write" : coff_archive.diagnostics.front().message) << '\n';
        return 2;
    }
    ObjectFile root;
    root.object_id = "n31_root";
    root.descriptor = make_elf_x86_64_object_target();
    ObjectSection root_text;
    root_text.id = 1U;
    root_text.name = ".text";
    root_text.kind = ObjectSectionKind::Text;
    root_text.alignment = 16U;
    root_text.contents = {0xE8U, 0U, 0U, 0U, 0U, 0xB8U, 60U, 0U, 0U, 0U,
                          0xBFU, 42U, 0U, 0U, 0U, 0x0FU, 0x05U};
    root_text.size = root_text.contents.size();
    root.sections.push_back(root_text);
    ObjectSymbol entry;
    entry.name = "_start";
    entry.signature_key = "entry";
    entry.binding = ObjectSymbolBinding::Global;
    entry.kind = ObjectSymbolKind::Function;
    entry.defined = true;
    entry.section_id = 1U;
    entry.value = 0U;
    entry.size = root_text.size;
    root.symbols.push_back(entry);
    ObjectSymbol unresolved;
    unresolved.name = "n31_beta";
    unresolved.signature_key = "fn()->i32";
    unresolved.binding = ObjectSymbolBinding::Global;
    unresolved.kind = ObjectSymbolKind::Function;
    unresolved.defined = false;
    root.symbols.push_back(unresolved);
    root.relocations.push_back(ObjectRelocation{1U, 1U, ObjectRelocationKind::PltRelative32, "n31_beta", -4, 4U});
    const auto root_bytes = write_binary_object(root);
    if (!root_bytes.succeeded() || !write_file("n31_root.o", root_bytes.bytes)) {
        std::cerr << "failed to emit root ELF object\n";
        return 3;
    }

    SystemLinkerRequest link_request;
    link_request.kind = SystemLinkerKind::GnuLd;
    link_request.output_path = "n31_linked_app";
    link_request.entry_symbol = "_start";
    link_request.object_paths = {"n31_root.o"};
    link_request.archive_paths = {"n31_demo.a"};
    link_request.static_link = true;
    const auto link_plan = create_system_linker_plan(link_request);
    if (!link_plan.succeeded() || !write_file("n31_link.rsp",
            std::vector<std::uint8_t>(link_plan.response_file_contents.begin(), link_plan.response_file_contents.end()))) {
        std::cerr << "failed to create system linker response plan\n";
        return 4;
    }

    ObjectFile coff_validation_root;
    coff_validation_root.object_id = "n31_coff_validation_root";
    coff_validation_root.descriptor = make_coff_x86_64_object_target();
    ObjectSection validation_text;
    validation_text.id = 1U;
    validation_text.name = ".text";
    validation_text.kind = ObjectSectionKind::Text;
    validation_text.alignment = 16U;
    validation_text.contents = {0xE8U, 0U, 0U, 0U, 0U, 0xC3U};
    validation_text.size = validation_text.contents.size();
    coff_validation_root.sections.push_back(validation_text);
    ObjectSymbol validation_entry;
    validation_entry.name = "_start";
    validation_entry.signature_key = "entry";
    validation_entry.binding = ObjectSymbolBinding::Global;
    validation_entry.kind = ObjectSymbolKind::Function;
    validation_entry.defined = true;
    validation_entry.section_id = 1U;
    validation_entry.value = 0U;
    validation_entry.size = validation_text.size;
    coff_validation_root.symbols.push_back(validation_entry);
    ObjectSymbol validation_import;
    validation_import.name = "n31_beta";
    validation_import.signature_key = "fn()->i32";
    validation_import.binding = ObjectSymbolBinding::Global;
    validation_import.kind = ObjectSymbolKind::Function;
    validation_import.defined = false;
    coff_validation_root.symbols.push_back(validation_import);
    coff_validation_root.relocations.push_back(ObjectRelocation{1U, 1U, ObjectRelocationKind::CoffRelative32, "n31_beta", 0, 4U});
    const auto validation_object = write_binary_object(coff_validation_root);
    if (!validation_object.succeeded() || !write_file("n31_coff_validation_root.obj", validation_object.bytes)) {
        std::cerr << "failed to emit COFF archive validation object\n";
        return 7;
    }
    SystemLinkerRequest validation_request;
    validation_request.kind = SystemLinkerKind::LldLink;
    validation_request.output_path = "n31_coff_archive_validation.exe";
    validation_request.entry_symbol = "_start";
    validation_request.object_paths = {"n31_coff_validation_root.obj"};
    validation_request.archive_paths = {"n31_demo.lib"};
    validation_request.disable_default_libraries = true;
    const auto validation_plan = create_system_linker_plan(validation_request);
    if (!validation_plan.succeeded() || !write_file("n31_coff_validation.rsp",
            std::vector<std::uint8_t>(validation_plan.response_file_contents.begin(), validation_plan.response_file_contents.end()))) {
        std::cerr << "failed to create COFF validation response plan\n";
        return 8;
    }

    ObjectFile coff_root;
    coff_root.object_id = "n31_coff_root";
    coff_root.descriptor = make_coff_x86_64_object_target();
    ObjectSection coff_text;
    coff_text.id = 1U;
    coff_text.name = ".text";
    coff_text.kind = ObjectSectionKind::Text;
    coff_text.alignment = 16U;
    coff_text.contents = {0x48U, 0x83U, 0xECU, 0x28U,
                          0xE8U, 0U, 0U, 0U, 0U,
                          0xB9U, 42U, 0U, 0U, 0U,
                          0x48U, 0x83U, 0xC4U, 0x28U,
                          0xFFU, 0x25U, 0U, 0U, 0U, 0U};
    coff_text.size = coff_text.contents.size();
    coff_root.sections.push_back(coff_text);
    ObjectSymbol coff_entry;
    coff_entry.name = "_start";
    coff_entry.signature_key = "entry";
    coff_entry.binding = ObjectSymbolBinding::Global;
    coff_entry.kind = ObjectSymbolKind::Function;
    coff_entry.defined = true;
    coff_entry.section_id = 1U;
    coff_entry.value = 0U;
    coff_entry.size = coff_text.size;
    coff_root.symbols.push_back(coff_entry);
    ObjectSymbol coff_unresolved;
    coff_unresolved.name = "n31_beta";
    coff_unresolved.signature_key = "fn()->i32";
    coff_unresolved.binding = ObjectSymbolBinding::Global;
    coff_unresolved.kind = ObjectSymbolKind::Function;
    coff_unresolved.defined = false;
    coff_root.symbols.push_back(coff_unresolved);
    ObjectSymbol exit_iat;
    exit_iat.name = "__imp_ExitProcess";
    exit_iat.signature_key = "import:KERNEL32!ExitProcess";
    exit_iat.binding = ObjectSymbolBinding::Global;
    exit_iat.kind = ObjectSymbolKind::Object;
    exit_iat.defined = false;
    coff_root.symbols.push_back(exit_iat);
    coff_root.relocations.push_back(ObjectRelocation{1U, 5U, ObjectRelocationKind::CoffRelative32, "n31_beta", 0, 4U});
    coff_root.relocations.push_back(ObjectRelocation{1U, 20U, ObjectRelocationKind::CoffRelative32, "__imp_ExitProcess", 0, 4U});
    const auto coff_root_bytes = write_binary_object(coff_root);
    if (!coff_root_bytes.succeeded() || !write_file("n31_coff_root.obj", coff_root_bytes.bytes)) {
        std::cerr << "failed to emit root COFF object\n";
        return 5;
    }
    const auto coff_root_roundtrip = read_binary_object(coff_root_bytes.bytes);
    if (!coff_root_roundtrip.succeeded() || coff_root_roundtrip.object_file.relocations.size() != 2U) {
        std::cerr << "failed to validate COFF REL32 fields after binary round-trip\n";
        return 9;
    }
    for (const auto& relocation : coff_root_roundtrip.object_file.relocations) {
        if (relocation.kind != ObjectRelocationKind::CoffRelative32 || relocation.addend != 0) {
            std::cerr << "COFF REL32 call/IAT field must carry a zero in-place addend\n";
            return 10;
        }
    }
    SystemLinkerRequest coff_link_request;
    coff_link_request.kind = SystemLinkerKind::MsvcLink;
    coff_link_request.output_path = "n31_windows_smoke.exe";
    coff_link_request.entry_symbol = "_start";
    coff_link_request.object_paths = {"n31_coff_root.obj"};
    coff_link_request.archive_paths = {"n31_demo.lib"};
    coff_link_request.libraries = {"kernel32"};
    coff_link_request.disable_default_libraries = true;
    const auto coff_link_plan = create_system_linker_plan(coff_link_request);
    if (!coff_link_plan.succeeded() || !write_file("n31_coff_link.rsp",
            std::vector<std::uint8_t>(coff_link_plan.response_file_contents.begin(), coff_link_plan.response_file_contents.end()))) {
        std::cerr << "failed to create COFF system linker response plan\n";
        return 6;
    }

    std::cout << "GNU ar archive: n31_demo.a (" << elf_archive.bytes.size() << " bytes)\n";
    std::cout << "COFF LIB archive: n31_demo.lib (" << coff_archive.bytes.size() << " bytes)\n";
    std::cout << "GNU system linker response: n31_link.rsp\n";
    std::cout << "LLD COFF archive validation response: n31_coff_validation.rsp\n";
    std::cout << "MSVC system linker response: n31_coff_link.rsp\n";
    std::cout << "Windows process smoke target: n31_windows_smoke.exe (expected exit 42)\n";
    return 0;
}
