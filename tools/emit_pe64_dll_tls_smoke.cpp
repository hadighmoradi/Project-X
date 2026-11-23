#include "xfrontend/pe64_dll.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

void patch_rel32(std::vector<std::uint8_t>& bytes, std::size_t offset,
                 std::uint32_t instruction_end_rva, std::uint32_t target_rva) {
    const std::int64_t displacement = static_cast<std::int64_t>(target_rva) - instruction_end_rva;
    if (displacement < std::numeric_limits<std::int32_t>::min() ||
        displacement > std::numeric_limits<std::int32_t>::max()) {
        throw std::runtime_error("TLS smoke RIP-relative displacement is out of range");
    }
    const std::uint32_t value = static_cast<std::uint32_t>(static_cast<std::int32_t>(displacement));
    for (unsigned index = 0U; index < 4U; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
}

const xfrontend::Pe64DllSectionInfo& find_section(const xfrontend::Pe64DllImage& image, const char* name) {
    for (const auto& section : image.sections) {
        if (section.name == name) return section;
    }
    throw std::runtime_error(std::string("missing generated section ") + name);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: emit_pe64_dll_tls_smoke <output.dll>\n";
        return 2;
    }

    xfrontend::Pe64DllSpec spec;
    spec.dll_name = "n37_tls_native.dll";
    spec.image_base = 0x180000000ULL;
    spec.text.assign(96U, 0x90U);

    // TLS callback at .text+0: lock inc dword ptr [rip+marker]; ret.
    // Windows invokes it for process attach and worker thread attach/detach.
    const std::uint8_t callback_code[] = {0xf0U, 0xffU, 0x05U, 0U, 0U, 0U, 0U, 0xc3U};
    for (std::size_t index = 0U; index < sizeof(callback_code); ++index) spec.text[index] = callback_code[index];

    // DWORD __stdcall N37GetTlsValue(): read TlsIndex, then TEB.ThreadLocalStoragePointer[ index ].
    const std::uint8_t getter_code[] = {
        0x8bU, 0x05U, 0U, 0U, 0U, 0U,
        0x65U, 0x48U, 0x8bU, 0x0cU, 0x25U, 0x58U, 0U, 0U, 0U,
        0x48U, 0x8bU, 0x04U, 0xc1U,
        0x8bU, 0x00U, 0xc3U
    };
    for (std::size_t index = 0U; index < sizeof(getter_code); ++index) spec.text[16U + index] = getter_code[index];

    // void __stdcall N37SetTlsValue(DWORD): preserve ECX in EDX, then write the current thread's TLS slot.
    const std::uint8_t setter_code[] = {
        0x89U, 0xcaU,
        0x8bU, 0x05U, 0U, 0U, 0U, 0U,
        0x65U, 0x48U, 0x8bU, 0x0cU, 0x25U, 0x58U, 0U, 0U, 0U,
        0x48U, 0x8bU, 0x04U, 0xc1U,
        0x89U, 0x10U, 0xc3U
    };
    for (std::size_t index = 0U; index < sizeof(setter_code); ++index) spec.text[48U + index] = setter_code[index];

    // Minimal x64 DllMain: mov eax, 1; ret. Its non-null AddressOfEntryPoint
    // ensures the Windows loader tracks normal DLL thread notifications.
    const std::uint8_t dll_main_code[] = {0xb8U, 1U, 0U, 0U, 0U, 0xc3U};
    for (std::size_t index = 0U; index < sizeof(dll_main_code); ++index) spec.text[80U + index] = dll_main_code[index];
    spec.has_entry_point = true;
    spec.entry_point_offset = 80U;

    spec.data.assign(8U, 0U); // callback counter at +0; loader-owned TLS index at +4
    spec.tls.enabled = true;
    spec.tls.template_data = {0x78U, 0x56U, 0x34U, 0x12U}; // 0x12345678 per thread
    spec.tls.zero_fill_size = 4U;
    spec.tls.index_data_offset = 4U;
    spec.tls.callbacks = {{xfrontend::Pe64SectionKind::Text, 0U}};
    spec.imports = {{"KERNEL32.dll", "GetCurrentProcessId", 0U, xfrontend::Pe64ImportKind::ByName, 0U}};
    spec.exports = {
        {"N37GetTlsValue", 1U, xfrontend::Pe64SectionKind::Text, 16U, false, {}},
        {"N37SetTlsValue", 2U, xfrontend::Pe64SectionKind::Text, 48U, false, {}},
        {"N37TlsCallbackCount", 3U, xfrontend::Pe64SectionKind::Data, 0U, false, {}},
        {"N37TlsIndex", 4U, xfrontend::Pe64SectionKind::Data, 4U, false, {}}
    };

    const auto layout = xfrontend::write_pe64_dll_image(spec);
    if (!layout.succeeded()) {
        for (const auto& item : layout.diagnostics) std::cerr << item.code << ": " << item.message << '\n';
        return 3;
    }
    const auto& text = find_section(layout.image, ".text");
    const auto& data = find_section(layout.image, ".data");
    patch_rel32(spec.text, 3U, text.virtual_address + 7U, data.virtual_address);
    patch_rel32(spec.text, 18U, text.virtual_address + 22U, data.virtual_address + 4U);
    patch_rel32(spec.text, 52U, text.virtual_address + 56U, data.virtual_address + 4U);

    const auto dll = xfrontend::write_pe64_dll_image(spec);
    if (!dll.succeeded()) {
        for (const auto& item : dll.diagnostics) std::cerr << item.code << ": " << item.message << '\n';
        return 4;
    }
    if (!dll.image.has_dynamic_base || dll.image.entry_point_rva == 0U || dll.image.tls_directory_rva == 0U ||
        dll.image.tls_directory_size != 40U || dll.image.tls_template_size != 4U ||
        dll.image.tls_zero_fill_size != 4U || dll.image.tls_callback_count != 1U ||
        dll.image.import_directory_rva == 0U || dll.image.iat_rva == 0U ||
        dll.image.imported_symbol_count != 1U || dll.image.base_relocation_count != 5U) {
        std::cerr << "generated DLL is missing TLS Directory, callback or base-relocation metadata\n";
        return 5;
    }
    if (!write_file(argv[1], dll.image.bytes)) {
        std::cerr << "cannot write generated TLS DLL\n";
        return 6;
    }

    std::cout << "Generated PE32+ DLL with TLS Directory and DllMain: " << argv[1] << '\n';
    std::cout << "DllMain EntryPoint RVA: 0x" << std::hex << dll.image.entry_point_rva << std::dec << '\n';
    std::cout << "DLL bytes: " << dll.image.bytes.size() << '\n';
    std::cout << "TLS Directory RVA: 0x" << std::hex << dll.image.tls_directory_rva << std::dec << '\n';
    std::cout << "TLS Directory bytes: " << dll.image.tls_directory_size << '\n';
    std::cout << "TLS template RVA: 0x" << std::hex << dll.image.tls_template_rva << std::dec << '\n';
    std::cout << "TLS template bytes: " << dll.image.tls_template_size << '\n';
    std::cout << "TLS zero-fill bytes: " << dll.image.tls_zero_fill_size << '\n';
    std::cout << "TLS index RVA: 0x" << std::hex << dll.image.tls_index_rva << std::dec << '\n';
    std::cout << "TLS callbacks: " << dll.image.tls_callback_count << '\n';
    std::cout << "Imported libraries: " << dll.image.imported_library_count << '\n';
    std::cout << "Imported symbols: " << dll.image.imported_symbol_count << '\n';
    std::cout << "Import Directory RVA: 0x" << std::hex << dll.image.import_directory_rva << std::dec << '\n';
    std::cout << "IAT RVA: 0x" << std::hex << dll.image.iat_rva << std::dec << '\n';
    std::cout << "DIR64 relocations: " << dll.image.base_relocation_count << '\n';
    std::cout << "DYNAMIC_BASE: " << (dll.image.has_dynamic_base ? "true" : "false") << '\n';
    return 0;
}
