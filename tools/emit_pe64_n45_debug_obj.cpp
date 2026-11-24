// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/pe64_pdb.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(file);
}

xfrontend::ObjectFile make_debug_object() {
    xfrontend::ObjectFile object;
    object.object_id = "n45_debug_probe";
    object.descriptor = xfrontend::make_coff_x86_64_object_target();

    xfrontend::ObjectSection text;
    text.id = 1U;
    text.name = ".text";
    text.kind = xfrontend::ObjectSectionKind::Text;
    text.alignment = 16U;
    text.size = 6U;
    // mov eax, 42; ret
    text.contents = {0xb8U, 42U, 0U, 0U, 0U, 0xc3U};
    object.sections.push_back(text);

    xfrontend::ObjectSymbol entry;
    entry.name = "DebugEntry";
    entry.signature_key = "fn()->i32";
    entry.binding = xfrontend::ObjectSymbolBinding::Global;
    entry.kind = xfrontend::ObjectSymbolKind::Function;
    entry.defined = true;
    entry.section_id = 1U;
    entry.value = 0U;
    entry.size = 6U;
    object.symbols.push_back(entry);
    return object;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: emit_pe64_n45_debug_obj <output.obj>\n";
        return 2;
    }
    xfrontend::Pe64CodeViewCoffSpec spec;
    spec.object_name = "n45_debug_probe.obj";
    spec.public_symbols.push_back(xfrontend::Pe64CodeViewPublicSymbol{
        "DebugEntry", 1U, 0U, xfrontend::Pe64CodeViewPublicKind::Function});
    const auto output = xfrontend::write_pe64_codeview_coff_object(make_debug_object(), spec);
    if (!output.succeeded()) {
        for (const auto& diagnostic : output.diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
        return 3;
    }
    const auto inspected = xfrontend::inspect_pe64_codeview_coff_object(output.bytes);
    if (!inspected.valid || inspected.public_symbols.size() != 1U ||
        inspected.public_symbols[0U].name != "DebugEntry") {
        std::cerr << "CodeView COFF self-inspection failed: " << inspected.error << '\n';
        return 4;
    }
    if (!write_file(argv[1], output.bytes)) {
        std::cerr << "cannot write CodeView COFF object file\n";
        return 5;
    }
    std::cout << "Generated CodeView AMD64 COFF object: " << argv[1] << '\n';
    std::cout << "COFF object bytes: " << output.bytes.size() << '\n';
    std::cout << "CodeView C13 .debug$S bytes: " << output.debug_section_bytes << '\n';
    std::cout << "S_PUB32 public symbols: " << output.public_symbol_count << '\n';
    std::cout << "CodeView COFF inspection: PASSED\n";
    return 0;
}
