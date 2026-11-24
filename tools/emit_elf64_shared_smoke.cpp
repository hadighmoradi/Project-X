// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/elf64_shared_object.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: emit_elf64_shared_smoke <output-path>\n";
        return 2;
    }

    xfrontend::Elf64SharedObjectSpec spec;
    spec.soname = "libx_n26_smoke.so";
    spec.text = {0xb8U, 42U, 0U, 0U, 0U, 0xc3U}; // int x_answer(void) { return 42; }
    spec.data.assign(8U, 0U);
    spec.exports.push_back(xfrontend::Elf64ExportedSymbol{
        "x_answer", xfrontend::Elf64SharedSection::Text, 0U,
        spec.text.size(), xfrontend::Elf64DynamicSymbolKind::Function});
    spec.exports.push_back(xfrontend::Elf64ExportedSymbol{
        "x_answer_pointer", xfrontend::Elf64SharedSection::Data, 0U,
        sizeof(std::uint64_t), xfrontend::Elf64DynamicSymbolKind::Object});
    spec.relative_relocations.push_back(xfrontend::Elf64RelativeRelocation{
        0U, xfrontend::Elf64SharedSection::Text, 0U});

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
              << ", R_X86_64_RELATIVE=" << result.image.relative_relocation_count << '\n';
    return 0;
}
