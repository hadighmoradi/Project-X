#include "xfrontend/pe64_dll.hpp"

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: emit_pe64_dll_import_smoke <output.dll>\n";
        return 2;
    }

    xfrontend::Pe64DllSpec spec;
    spec.dll_name = "n36_import_native.dll";
    spec.image_base = 0x180000000ULL;
    // A leaf tail-jump thunk: the original caller's return address is preserved,
    // so this tiny exported bridge needs no stack frame or unwind record.
    spec.text = {0xffU, 0x25U, 0U, 0U, 0U, 0U};
    spec.data.assign(8U, 0U);
    spec.absolute_relocations = {
        {0U, xfrontend::Pe64SectionKind::Text, 0U, 0}
    };
    spec.exports = {
        {"N36CurrentProcessId", 1U, xfrontend::Pe64SectionKind::Text, 0U, false, {}},
        {"N36FunctionPointer", 2U, xfrontend::Pe64SectionKind::Data, 0U, false, {}}
    };
    spec.imports = {
        {"KERNEL32.dll", "GetCurrentProcessId", 0U, xfrontend::Pe64ImportKind::ByName, 0U}
    };
    spec.iat_call_sites = {
        {2U, "KERNEL32.DLL", "GetCurrentProcessId", xfrontend::Pe64IatInstructionKind::JumpIndirect,
         xfrontend::Pe64ImportKind::ByName, 0U}
    };

    const auto dll = xfrontend::write_pe64_dll_image(spec);
    if (!dll.succeeded()) {
        for (const auto& item : dll.diagnostics)
            std::cerr << item.code << ": " << item.message << '\n';
        return 3;
    }
    if (!dll.image.has_dynamic_base || dll.image.base_relocation_count != 1U ||
        dll.image.relocation_directory_rva == 0U || dll.image.import_directory_rva == 0U ||
        dll.image.iat_rva == 0U || dll.image.imported_symbol_count != 1U ||
        dll.image.imported_library_count != 1U) {
        std::cerr << "generated DLL is missing its ASLR relocation or Import/IAT directories\n";
        return 4;
    }
    if (!write_file(argv[1], dll.image.bytes)) {
        std::cerr << "cannot write generated DLL\n";
        return 5;
    }

    std::cout << "Generated PE32+ DLL with Import Directory and IAT: " << argv[1] << '\n';
    std::cout << "DLL bytes: " << dll.image.bytes.size() << '\n';
    std::cout << "Import Directory RVA: 0x" << std::hex << dll.image.import_directory_rva << std::dec << '\n';
    std::cout << "Import Directory bytes: " << dll.image.import_directory_size << '\n';
    std::cout << "IAT RVA: 0x" << std::hex << dll.image.iat_rva << std::dec << '\n';
    std::cout << "IAT bytes: " << dll.image.iat_size << '\n';
    std::cout << "Imported libraries: " << dll.image.imported_library_count << '\n';
    std::cout << "Imported symbols: " << dll.image.imported_symbol_count << '\n';
    std::cout << "DIR64 relocations: " << dll.image.base_relocation_count << '\n';
    std::cout << "DYNAMIC_BASE: " << (dll.image.has_dynamic_base ? "true" : "false") << '\n';
    return 0;
}
