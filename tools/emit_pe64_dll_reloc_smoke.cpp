// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

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
        std::cerr << "usage: emit_pe64_dll_reloc_smoke <output.dll>\n";
        return 2;
    }

    xfrontend::Pe64DllSpec spec;
    spec.dll_name = "n35_reloc_native.dll";
    spec.image_base = 0x180000000ULL;
    spec.text = {0xB8U, 0x2AU, 0x00U, 0x00U, 0x00U, 0xC3U}; // mov eax,42; ret
    spec.read_only_data = {'N','3','5','A','S','L','R',0U};
    spec.data.assign(8U, 0U);
    spec.absolute_relocations = {
        {0U, xfrontend::Pe64SectionKind::Text, 0U, 0}
    };
    spec.exports = {
        {"N35Return42", 1U, xfrontend::Pe64SectionKind::Text, 0U, false, {}},
        {"N35FunctionPointer", 2U, xfrontend::Pe64SectionKind::Data, 0U, false, {}}
    };

    const auto dll = xfrontend::write_pe64_dll_image(spec);
    if (!dll.succeeded()) {
        for (const auto& item : dll.diagnostics)
            std::cerr << item.code << ": " << item.message << '\n';
        return 3;
    }
    if (!dll.image.has_dynamic_base || dll.image.base_relocation_count != 1U ||
        dll.image.relocation_directory_rva == 0U || dll.image.relocation_directory_size == 0U) {
        std::cerr << "generated DLL is missing its ASLR relocation directory\n";
        return 4;
    }
    if (!write_file(argv[1], dll.image.bytes)) {
        std::cerr << "cannot write generated DLL\n";
        return 5;
    }

    std::cout << "Generated relocatable PE32+ DLL: " << argv[1] << '\n';
    std::cout << "DLL bytes: " << dll.image.bytes.size() << '\n';
    std::cout << "Preferred ImageBase: 0x" << std::hex << dll.image.image_base << std::dec << '\n';
    std::cout << "Relocation directory RVA: 0x" << std::hex << dll.image.relocation_directory_rva << std::dec << '\n';
    std::cout << "Relocation directory bytes: " << dll.image.relocation_directory_size << '\n';
    std::cout << "DIR64 relocation entries: " << dll.image.base_relocation_count << '\n';
    std::cout << "DYNAMIC_BASE: " << (dll.image.has_dynamic_base ? "true" : "false") << '\n';
    return 0;
}
