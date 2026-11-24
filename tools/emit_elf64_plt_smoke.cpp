// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/elf64_shared_object.hpp"

#include <fstream>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: emit_elf64_plt_smoke <output-path>\n";
        return 2;
    }

    xfrontend::Elf64SharedObjectSpec spec;
    spec.soname = "libx_n28_plt_smoke.so";
    // int x_n28_getpid(void) { return getpid(); }
    // call rel32 PLT(getpid); ret
    spec.text = {0xe8U, 0U, 0U, 0U, 0U, 0xc3U};
    spec.exports.push_back(xfrontend::Elf64ExportedSymbol{
        "x_n28_getpid", xfrontend::Elf64SharedSection::Text, 0U,
        spec.text.size(), xfrontend::Elf64DynamicSymbolKind::Function});
    spec.imports.push_back(xfrontend::Elf64ImportedSymbol{
        "getpid", xfrontend::Elf64DynamicSymbolKind::Function, xfrontend::Elf64ImportBinding::Plt});
    spec.needed_libraries.push_back("libc.so.6");
    spec.plt_pc_relative32.push_back(xfrontend::Elf64PltPcRelative32{1U, "getpid"});

    const auto result = xfrontend::write_elf64_shared_object(spec);
    if (!result.succeeded()) {
        for (const auto& item : result.diagnostics)
            std::cerr << item.code << ": " << item.message << '\n';
        return 3;
    }

    std::ofstream output(argv[1], std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "cannot open output file\n";
        return 4;
    }
    output.write(reinterpret_cast<const char*>(result.image.bytes.data()),
                 static_cast<std::streamsize>(result.image.bytes.size()));
    if (!output) {
        std::cerr << "failed writing shared-object bytes\n";
        return 5;
    }
    std::cout << "Wrote " << result.image.bytes.size()
              << " bytes, exports=" << result.image.exported_symbol_count
              << ", imports=" << result.image.imported_symbol_count
              << ", PLT=" << result.image.plt_size
              << ", GOTPLT=" << result.image.gotplt_size
              << ", JUMP_SLOT=" << result.image.plt_relocation_count << '\n';
    return 0;
}
