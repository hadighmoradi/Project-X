#include "xfrontend/pe64_dll.hpp"

#include <algorithm>
#include <cstdint>
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
        std::cerr << "usage: emit_pe64_dll_unwind_smoke <output.dll>\n";
        return 2;
    }

    xfrontend::Pe64DllSpec spec;
    spec.dll_name = "n38_exception_native.dll";
    spec.image_base = 0x180000000ULL;
    spec.text.assign(64U, 0x90U);

    // int N38Return42(): sub rsp, 0x28; mov eax,42; add rsp,0x28; ret.
    const std::uint8_t function_code[] = {
        0x48U, 0x83U, 0xecU, 0x28U,
        0xb8U, 0x2aU, 0U, 0U, 0U,
        0x48U, 0x83U, 0xc4U, 0x28U,
        0xc3U
    };
    std::copy(std::begin(function_code), std::end(function_code), spec.text.begin());

    // Minimal DllMain: return TRUE.
    const std::uint8_t dll_main[] = {0xb8U, 1U, 0U, 0U, 0U, 0xc3U};
    std::copy(std::begin(dll_main), std::end(dll_main), spec.text.begin() + 32);
    spec.has_entry_point = true;
    spec.entry_point_offset = 32U;

    spec.data.assign(8U, 0U);
    spec.absolute_relocations.push_back({0U, xfrontend::Pe64SectionKind::Text, 0U, 0});
    spec.imports = {{"KERNEL32.dll", "GetCurrentProcessId", 0U, xfrontend::Pe64ImportKind::ByName, 0U}};
    spec.exports = {{"N38Return42", 1U, xfrontend::Pe64SectionKind::Text, 0U, false, {}}};

    xfrontend::Pe64DllRuntimeFunction runtime_function;
    runtime_function.begin_offset = 0U;
    runtime_function.end_offset = sizeof(function_code);
    runtime_function.prolog_size = 4U;
    runtime_function.unwind_codes.push_back({4U, xfrontend::Pe64DllUnwindOperation::AllocSmall, 4U, 0U});
    spec.runtime_functions.push_back(runtime_function);

    const auto result = xfrontend::write_pe64_dll_image(spec);
    if (!result.succeeded()) {
        for (const auto& item : result.diagnostics) std::cerr << item.code << ": " << item.message << '\n';
        return 3;
    }
    const auto& image = result.image;
    if (image.exception_directory_rva == 0U || image.exception_directory_size != 12U ||
        image.runtime_function_count != 1U || image.unwind_info_size != 8U ||
        image.import_directory_rva == 0U || image.iat_rva == 0U ||
        image.base_relocation_count != 1U || !image.has_dynamic_base || image.entry_point_rva == 0U) {
        std::cerr << "generated DLL is missing Exception Directory, UNWIND_INFO, Import/IAT, DllMain or DIR64 metadata\n";
        return 4;
    }
    if (!write_file(argv[1], image.bytes)) {
        std::cerr << "cannot write generated exception/unwind DLL\n";
        return 5;
    }

    std::cout << "Generated PE32+ DLL with Exception Directory: " << argv[1] << '\n';
    std::cout << "DLL bytes: " << image.bytes.size() << '\n';
    std::cout << "Exception Directory RVA: 0x" << std::hex << image.exception_directory_rva << std::dec << '\n';
    std::cout << "Exception Directory bytes: " << image.exception_directory_size << '\n';
    std::cout << "Runtime functions: " << image.runtime_function_count << '\n';
    std::cout << "UNWIND_INFO bytes: " << image.unwind_info_size << '\n';
    std::cout << "Import Directory RVA: 0x" << std::hex << image.import_directory_rva << std::dec << '\n';
    std::cout << "IAT RVA: 0x" << std::hex << image.iat_rva << std::dec << '\n';
    std::cout << "DIR64 relocations: " << image.base_relocation_count << '\n';
    std::cout << "DYNAMIC_BASE: " << (image.has_dynamic_base ? "true" : "false") << '\n';
    std::cout << "DllMain EntryPoint RVA: 0x" << std::hex << image.entry_point_rva << std::dec << '\n';
    return 0;
}
