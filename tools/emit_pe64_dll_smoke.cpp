#include "xfrontend/pe64_dll.hpp"
#include "xfrontend/pe64_exports.hpp"

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
    if (argc != 3) {
        std::cerr << "usage: emit_pe64_dll_smoke <output.dll> <output.lib>\n";
        return 2;
    }

    xfrontend::Pe64DllSpec spec;
    spec.dll_name = "n34_native.dll";
    spec.image_base = 0x180000000ULL;
    spec.text = {0xB8U, 0x2AU, 0x00U, 0x00U, 0x00U, 0xC3U}; // mov eax,42; ret
    spec.read_only_data = {'N','3','4','\0'};
    spec.data = {0x2AU, 0x00U, 0x00U, 0x00U};
    spec.exports = {
        {"N34Return42", 1U, xfrontend::Pe64SectionKind::Text, 0U, false, {}},
        {"N34AliasReturn42", 1U, xfrontend::Pe64SectionKind::Text, 0U, false, {}},
        {"N34DataValue", 2U, xfrontend::Pe64SectionKind::Data, 0U, false, {}},
        {"N34ForwardedSleep", 3U, xfrontend::Pe64SectionKind::Text, 0U, true, "KERNELBASE.Sleep"},
        {"", 4U, xfrontend::Pe64SectionKind::Text, 0U, false, {}},
        {"N34ReadOnlyData", 5U, xfrontend::Pe64SectionKind::ReadOnlyData, 0U, false, {}},
        {"N34SparseOrdinal", 7U, xfrontend::Pe64SectionKind::Text, 0U, false, {}}
    };

    const auto dll = xfrontend::write_pe64_dll_image(spec);
    if (!dll.succeeded()) {
        for (const auto& item : dll.diagnostics)
            std::cerr << item.code << ": " << item.message << '\n';
        return 3;
    }
    if (!write_file(argv[1], dll.image.bytes)) {
        std::cerr << "cannot write generated DLL\n";
        return 4;
    }

    const auto import_library = xfrontend::write_coff_import_library_from_pe64_dll(
        dll.image.bytes,
        {{"N34DataValue", 0U, xfrontend::CoffImportType::Data},
         {"", 4U, xfrontend::CoffImportType::Const}});
    if (!import_library.succeeded()) {
        for (const auto& item : import_library.diagnostics)
            std::cerr << item.code << ": " << item.message << '\n';
        return 5;
    }
    if (!write_file(argv[2], import_library.bytes)) {
        std::cerr << "cannot write generated import library\n";
        return 6;
    }

    std::cout << "Generated PE32+ DLL: " << argv[1] << '\n';
    std::cout << "DLL bytes: " << dll.image.bytes.size() << '\n';
    std::cout << "Export names: " << dll.image.export_name_count << '\n';
    std::cout << "Export ordinals: " << dll.image.export_ordinal_count << '\n';
    std::cout << "Generated COFF Import Library: " << argv[2] << '\n';
    std::cout << "Import entries: " << import_library.import_count << '\n';
    std::cout << "Import library bytes: " << import_library.bytes.size() << '\n';
    return 0;
}
