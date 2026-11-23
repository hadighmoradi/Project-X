#include "xfrontend/executable_image.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: emit_elf64_smoke <output-path>\n";
        return 2;
    }

    // Linux x86-64 process-entry program: exit(42).
    const std::uint8_t machine_code[] = {
        0xb8U, 0x3cU, 0x00U, 0x00U, 0x00U, // mov eax, 60 (SYS_exit)
        0xbfU, 0x2aU, 0x00U, 0x00U, 0x00U, // mov edi, 42
        0x0fU, 0x05U                      // syscall
    };

    xfrontend::LinkedImage linked;
    linked.descriptor = xfrontend::make_elf_x86_64_object_target();
    linked.image_base = 0x400000U;
    linked.image_size = sizeof(machine_code);
    linked.flat_bytes.assign(machine_code, machine_code + sizeof(machine_code));

    xfrontend::LinkedImageSection text;
    text.object_id = "n25-linux-smoke";
    text.source_section_id = 1U;
    text.name = ".text";
    text.kind = xfrontend::ObjectSectionKind::Text;
    text.address = linked.image_base;
    text.alignment = 4096U;
    text.memory_size = sizeof(machine_code);
    text.contents = linked.flat_bytes;
    linked.sections.push_back(text);

    xfrontend::LinkedImageSymbol entry;
    entry.name = "_start";
    entry.object_id = text.object_id;
    entry.section_id = 1U;
    entry.address = linked.image_base;
    entry.size = sizeof(machine_code);
    entry.binding = xfrontend::ObjectSymbolBinding::Global;
    entry.kind = xfrontend::ObjectSymbolKind::Function;
    linked.symbols.push_back(entry);
    linked.entry_address = entry.address;
    linked.has_entry = true;

    const auto result = xfrontend::write_elf64_executable_image(linked);
    if (!result.succeeded()) {
        for (const auto& diagnostic : result.diagnostics)
            std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
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
        std::cerr << "failed writing executable bytes\n";
        return 5;
    }
    std::cout << "Wrote " << result.image.bytes.size() << " bytes, entry=0x"
              << std::hex << result.image.entry_address << '\n';
    return 0;
}
