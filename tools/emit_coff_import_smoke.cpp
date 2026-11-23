#include "xfrontend/binary_object.hpp"
#include "xfrontend/coff_import_library.hpp"
#include "xfrontend/system_linker.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace xfrontend;

namespace {
bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) return false;
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(stream);
}

bool write_text_file(const std::string& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary);
    if (!stream) return false;
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return static_cast<bool>(stream);
}
} // namespace

int main() {
    CoffImportSymbol exit_process;
    exit_process.dll_name = "KERNEL32.dll";
    exit_process.symbol_name = "ExitProcess";
    exit_process.type = CoffImportType::Code;
    exit_process.name_type = CoffImportNameType::Name;
    exit_process.ordinal_or_hint = 0U;
    exit_process.timestamp = 0U;
    exit_process.member_name = "KERNEL32.dll";

    const auto import_library = write_coff_import_library({exit_process});
    if (!import_library.succeeded() || !write_file("n32_kernel32_import.lib", import_library.bytes)) {
        std::cerr << "failed to emit generated KERNEL32 import library";
        if (!import_library.diagnostics.empty()) std::cerr << ": " << import_library.diagnostics.front().message;
        std::cerr << '\n';
        return 1;
    }
    const auto import_library_check = read_coff_import_library(import_library.bytes);
    if (!import_library_check.succeeded() || import_library_check.imports.size() != 1U ||
        import_library_check.archive.members.size() != 4U ||
        import_library_check.archive.symbol_index.size() != 5U) {
        std::cerr << "generated KERNEL32 import library failed its member/index self-check\n";
        return 2;
    }

    ObjectFile root;
    root.object_id = "n32_import_library_root";
    root.descriptor = make_coff_x86_64_object_target();
    ObjectSection text;
    text.id = 1U;
    text.name = ".text";
    text.kind = ObjectSectionKind::Text;
    text.alignment = 16U;
    // Windows entry stack alignment: sub rsp,28h; ecx=42; call ExitProcess; ret.
    text.contents = {0x48U, 0x83U, 0xecU, 0x28U,
                     0xb9U, 42U, 0U, 0U, 0U,
                     0xe8U, 0U, 0U, 0U, 0U,
                     0xc3U};
    text.size = text.contents.size();
    root.sections.push_back(text);

    ObjectSymbol entry;
    entry.name = "_start";
    entry.signature_key = "entry";
    entry.binding = ObjectSymbolBinding::Global;
    entry.kind = ObjectSymbolKind::Function;
    entry.defined = true;
    entry.section_id = 1U;
    entry.value = 0U;
    entry.size = text.size;
    root.symbols.push_back(entry);

    ObjectSymbol imported;
    imported.name = "ExitProcess";
    imported.signature_key = "import:KERNEL32!ExitProcess";
    imported.binding = ObjectSymbolBinding::Global;
    imported.kind = ObjectSymbolKind::Function;
    imported.defined = false;
    root.symbols.push_back(imported);
    root.relocations.push_back(ObjectRelocation{1U, 10U,
        ObjectRelocationKind::CoffRelative32, "ExitProcess", 0, 4U});

    const auto object = write_binary_object(root);
    if (!object.succeeded() || !write_file("n32_import_library_root.obj", object.bytes)) {
        std::cerr << "failed to emit COFF import-library consumer object\n";
        return 3;
    }
    const auto object_check = read_binary_object(object.bytes);
    if (!object_check.succeeded() || object_check.object_file.relocations.size() != 1U ||
        object_check.object_file.relocations.front().addend != 0) {
        std::cerr << "COFF import call relocation failed object round-trip validation\n";
        return 4;
    }

    SystemLinkerRequest link_request;
    link_request.kind = SystemLinkerKind::MsvcLink;
    link_request.output_path = "n32_import_library_smoke.exe";
    link_request.entry_symbol = "_start";
    link_request.object_paths = {"n32_import_library_root.obj"};
    link_request.archive_paths = {"n32_kernel32_import.lib"};
    link_request.disable_default_libraries = true;
    const auto plan = create_system_linker_plan(link_request);
    if (!plan.succeeded() || !write_text_file("n32_import_library_link.rsp", plan.response_file_contents)) {
        std::cerr << "failed to create link.exe response file for generated import library\n";
        return 5;
    }

    std::cout << "Generated import library: n32_kernel32_import.lib (" << import_library.bytes.size() << " bytes)\n";
    std::cout << "Import member: KERNEL32.dll!ExitProcess (CODE/NAME)\n";
    std::cout << "Import descriptor: __IMPORT_DESCRIPTOR_KERNEL32\n";
    std::cout << "Terminator members: __NULL_IMPORT_DESCRIPTOR, KERNEL32 null thunk\n";
    std::cout << "Linker symbols: ExitProcess, __imp_ExitProcess, descriptor support\n";
    std::cout << "Consumer object: n32_import_library_root.obj\n";
    std::cout << "MSVC linker response: n32_import_library_link.rsp\n";
    std::cout << "Windows loader sample: n32_import_library_smoke.exe (expected exit 42)\n";
    return 0;
}
