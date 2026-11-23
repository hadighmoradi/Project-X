#include "xfrontend/pe64_exports.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <utility>
#include <string>
#include <vector>

namespace {

bool parse_ordinal(const std::string& text, std::uint32_t& value) {
    if (text.empty()) return false;
    std::uint64_t parsed = 0U;
    for (const unsigned char ch : text) {
        if (ch < '0' || ch > '9') return false;
        parsed = parsed * 10U + static_cast<std::uint64_t>(ch - '0');
        if (parsed > std::numeric_limits<std::uint16_t>::max()) return false;
    }
    if (parsed == 0U) return false;
    value = static_cast<std::uint32_t>(parsed);
    return true;
}

void print_usage() {
    std::cerr << "usage: emit_pe64_export_import_smoke <input.dll> <output.lib>"
                 " [--data <name>|--const <name>|--data-ordinal <n>|--const-ordinal <n>]...\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3 || ((argc - 3) % 2) != 0) {
        print_usage();
        return 2;
    }
    const std::string input_path = argv[1];
    const std::string output_path = argv[2];
    std::vector<xfrontend::Pe64ExportKindOverride> overrides;
    for (int index = 3; index < argc; index += 2) {
        const std::string option = argv[index];
        const std::string value = argv[index + 1];
        xfrontend::Pe64ExportKindOverride item;
        if (option == "--data" || option == "--const") {
            item.export_name = value;
            item.type = option == "--data" ? xfrontend::CoffImportType::Data
                                             : xfrontend::CoffImportType::Const;
            if (value.empty()) {
                std::cerr << "export name override must not be empty\n";
                return 2;
            }
        } else if (option == "--data-ordinal" || option == "--const-ordinal") {
            if (!parse_ordinal(value, item.ordinal)) {
                std::cerr << "ordinal override must be a decimal value from 1 through 65535\n";
                return 2;
            }
            item.type = option == "--data-ordinal" ? xfrontend::CoffImportType::Data
                                                     : xfrontend::CoffImportType::Const;
        } else {
            std::cerr << "unknown override option: " << option << '\n';
            print_usage();
            return 2;
        }
        overrides.push_back(std::move(item));
    }

    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        std::cerr << "cannot open DLL: " << input_path << '\n';
        return 3;
    }
    const std::vector<std::uint8_t> image{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    const auto parsed = xfrontend::read_pe64_export_directory(image);
    if (!parsed.succeeded()) {
        for (const auto& item : parsed.diagnostics)
            std::cerr << item.code << ": " << item.message << '\n';
        return 4;
    }
    std::cout << "DLL: " << parsed.directory.dll_name << '\n';
    std::cout << "Export entries: " << parsed.directory.exports.size() << '\n';
    const auto written = xfrontend::write_coff_import_library_from_pe64_dll(image, overrides);
    if (!written.succeeded()) {
        for (const auto& item : written.diagnostics)
            std::cerr << item.code << ": " << item.message << '\n';
        return 5;
    }
    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "cannot create import library: " << output_path << '\n';
        return 6;
    }
    output.write(reinterpret_cast<const char*>(written.bytes.data()),
                 static_cast<std::streamsize>(written.bytes.size()));
    if (!output) {
        std::cerr << "failed while writing import library\n";
        return 7;
    }
    std::cout << "Generated COFF Import Library: " << output_path << '\n';
    std::cout << "Imported exports: " << written.import_count << '\n';
    std::cout << "Library bytes: " << written.bytes.size() << '\n';
    return 0;
}
