#include "xfrontend/pe64_dll.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xfrontend;
namespace {

constexpr std::uint64_t kOrdinalFlag64 = 0x8000000000000000ULL;

void n36_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void n36_case(const std::string& name, const std::function<void()>& body,
              int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    std::cout << "RUN " << name << '\n' << std::flush;
    const auto start = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        std::cout << "PASS " << name << '\n' << std::flush;
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        std::cout << "FAIL " << name << ": " << error.what() << '\n' << std::flush;
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

std::uint16_t get16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n36_require(offset <= bytes.size() && bytes.size() - offset >= 2U, "u16 read outside PE image");
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t get32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n36_require(offset <= bytes.size() && bytes.size() - offset >= 4U, "u32 read outside PE image");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t get64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint64_t>(get32(bytes, offset)) |
        (static_cast<std::uint64_t>(get32(bytes, offset + 4U)) << 32U);
}

std::string read_c_string(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n36_require(offset < bytes.size(), "string offset outside PE image");
    std::string result;
    while (offset < bytes.size() && bytes[offset] != 0U) {
        result.push_back(static_cast<char>(bytes[offset]));
        ++offset;
    }
    n36_require(offset < bytes.size(), "unterminated string in PE image");
    return result;
}

const Pe64DllSectionInfo& section(const Pe64DllImage& image, const char* name) {
    const auto found = std::find_if(image.sections.begin(), image.sections.end(), [name](const auto& item) {
        return item.name == name;
    });
    if (found == image.sections.end()) throw std::runtime_error(std::string("missing PE section ") + name);
    return *found;
}

std::size_t rva_offset(const Pe64DllImage& image, std::uint32_t rva, std::size_t length) {
    for (const auto& item : image.sections) {
        if (rva < item.virtual_address) continue;
        const std::uint64_t delta = static_cast<std::uint64_t>(rva) - item.virtual_address;
        if (delta <= item.raw_size && length <= static_cast<std::size_t>(item.raw_size - delta))
            return static_cast<std::size_t>(item.raw_offset) + static_cast<std::size_t>(delta);
    }
    throw std::runtime_error("RVA does not map to raw PE section");
}

std::size_t optional_header_offset(const Pe64DllImage& image) {
    (void)image;
    return 0x80U + 4U + 20U;
}

Pe64DllSpec make_spec(std::size_t index) {
    Pe64DllSpec spec;
    spec.dll_name = "n36_imports_" + std::to_string(index) + ".dll";
    spec.text = {0xc3U};
    spec.exports = {{"N36Entry", 1U, Pe64SectionKind::Text, 0U, false, {}}};
    return spec;
}

std::size_t count_descriptors(const Pe64DllImage& image) {
    const std::size_t descriptor = rva_offset(image, image.import_directory_rva, 20U);
    const std::size_t max_count = image.import_directory_size / 20U;
    std::size_t count = 0U;
    for (; count < max_count; ++count) {
        const std::size_t at = descriptor + count * 20U;
        bool all_zero = true;
        for (std::size_t byte = 0U; byte < 20U; ++byte) all_zero = all_zero && image.bytes[at + byte] == 0U;
        if (all_zero) return count;
    }
    throw std::runtime_error("Import Directory has no terminating null descriptor");
}

void test_single_named_import(std::size_t index) {
    auto spec = make_spec(index);
    const std::string symbol = "N36NamedImport" + std::to_string(index);
    const std::uint16_t hint = static_cast<std::uint16_t>((index * 37U) & 0xffffU);
    spec.imports.push_back(Pe64Import{"KERNEL32.dll", symbol, hint, Pe64ImportKind::ByName, 0U});
    const auto result = write_pe64_dll_image(spec);
    n36_require(result.succeeded(), "valid named import DLL was rejected");
    const auto& image = result.image;
    n36_require(image.import_directory_rva != 0U && image.import_directory_size == 40U,
                "single import must publish one descriptor plus null descriptor");
    n36_require(image.imported_library_count == 1U && image.imported_symbol_count == 1U &&
                image.imported_name_count == 1U && image.imported_ordinal_count == 0U,
                "single named import counters mismatch");
    const auto& idata = section(image, ".idata");
    n36_require(idata.characteristics == 0xc0000040U, ".idata must be read/write for the Windows Loader IAT fixup");
    const std::size_t optional = optional_header_offset(image);
    n36_require(get32(image.bytes, optional + 112U + 8U) == image.import_directory_rva &&
                get32(image.bytes, optional + 112U + 12U) == image.import_directory_size,
                "Optional Header Import Directory does not match image metadata");
    n36_require(get32(image.bytes, optional + 112U + 12U * 8U) == image.iat_rva &&
                get32(image.bytes, optional + 112U + 12U * 8U + 4U) == image.iat_size,
                "Optional Header IAT Directory does not match image metadata");
    n36_require(count_descriptors(image) == 1U, "single named import must create exactly one DLL descriptor");
    const std::size_t desc = rva_offset(image, image.import_directory_rva, 20U);
    const std::uint32_t original_first_thunk = get32(image.bytes, desc);
    const std::uint32_t name_rva = get32(image.bytes, desc + 12U);
    const std::uint32_t first_thunk = get32(image.bytes, desc + 16U);
    n36_require(read_c_string(image.bytes, rva_offset(image, name_rva, 1U)) == "KERNEL32.dll",
                "Import Descriptor DLL name mismatch");
    n36_require(first_thunk == image.iat_rva && image.iat_size == 16U,
                "single import IAT must contain one thunk and a zero terminator");
    const std::uint64_t lookup = get64(image.bytes, rva_offset(image, original_first_thunk, 8U));
    const std::uint64_t address = get64(image.bytes, rva_offset(image, first_thunk, 8U));
    n36_require(lookup == address && (lookup & kOrdinalFlag64) == 0U,
                "named import ILT and initial IAT entries must reference the same IMAGE_IMPORT_BY_NAME");
    const std::uint32_t import_name_rva = static_cast<std::uint32_t>(lookup);
    const std::size_t name_offset = rva_offset(image, import_name_rva, 2U + symbol.size() + 1U);
    n36_require(get16(image.bytes, name_offset) == hint, "IMAGE_IMPORT_BY_NAME hint mismatch");
    n36_require(read_c_string(image.bytes, name_offset + 2U) == symbol, "IMAGE_IMPORT_BY_NAME symbol mismatch");
    n36_require(get64(image.bytes, rva_offset(image, original_first_thunk + 8U, 8U)) == 0U &&
                get64(image.bytes, rva_offset(image, first_thunk + 8U, 8U)) == 0U,
                "ILT and IAT must have zero terminators");
}

void test_multi_library_and_ordinal_imports(std::size_t index) {
    auto spec = make_spec(index + 500U);
    const std::string first_name = "N36KernelName" + std::to_string(index);
    const std::string second_name = "N36UserName" + std::to_string(index);
    const std::uint16_t kernel_ordinal = static_cast<std::uint16_t>(1U + (index % 60000U));
    const std::uint16_t user_ordinal = static_cast<std::uint16_t>(1U + ((index * 3U) % 60000U));
    spec.imports = {
        {"KERNEL32.dll", first_name, static_cast<std::uint16_t>(index & 0xffffU), Pe64ImportKind::ByName, 0U},
        {"kernel32.DLL", "", 0U, Pe64ImportKind::ByOrdinal, kernel_ordinal},
        {"USER32.dll", second_name, static_cast<std::uint16_t>((index + 9U) & 0xffffU), Pe64ImportKind::ByName, 0U},
        {"user32.DLL", "", 0U, Pe64ImportKind::ByOrdinal, user_ordinal}
    };
    spec.text = {0xffU, 0x25U, 0U, 0U, 0U, 0U, 0xc3U};
    spec.iat_call_sites = {
        {2U, "KERNEL32.dll", "", Pe64IatInstructionKind::JumpIndirect, Pe64ImportKind::ByOrdinal, kernel_ordinal}
    };
    const auto result = write_pe64_dll_image(spec);
    n36_require(result.succeeded(), "valid multi-library imports were rejected");
    const auto& image = result.image;
    n36_require(count_descriptors(image) == 2U && image.imported_library_count == 2U,
                "case-insensitive DLL grouping did not produce two descriptors");
    n36_require(image.imported_symbol_count == 4U && image.imported_name_count == 2U &&
                image.imported_ordinal_count == 2U, "name/ordinal import counters mismatch");
    n36_require(image.import_directory_size == 60U, "two DLL descriptors plus null descriptor must occupy 60 bytes");
    n36_require(image.iat_size == 48U, "two libraries with two imports each need six IAT qwords including terminators");

    const std::size_t first_desc = rva_offset(image, image.import_directory_rva, 20U);
    const std::size_t second_desc = first_desc + 20U;
    n36_require(read_c_string(image.bytes, rva_offset(image, get32(image.bytes, first_desc + 12U), 1U)) == "KERNEL32.dll",
                "first-seen DLL spelling was not preserved");
    n36_require(read_c_string(image.bytes, rva_offset(image, get32(image.bytes, second_desc + 12U), 1U)) == "USER32.dll",
                "second DLL group order or spelling mismatch");
    const std::uint32_t first_ilt = get32(image.bytes, first_desc);
    const std::uint32_t first_iat = get32(image.bytes, first_desc + 16U);
    const std::uint32_t second_ilt = get32(image.bytes, second_desc);
    const std::uint32_t second_iat = get32(image.bytes, second_desc + 16U);
    n36_require(first_iat == image.iat_rva && second_iat == first_iat + 24U,
                "IAT tables must be contiguous and preserve per-library group order");
    n36_require((get64(image.bytes, rva_offset(image, first_ilt + 8U, 8U)) & kOrdinalFlag64) != 0U &&
                (get64(image.bytes, rva_offset(image, first_iat + 8U, 8U)) & kOrdinalFlag64) != 0U,
                "ordinal import must use the PE32+ high-bit ordinal encoding");
    n36_require((get64(image.bytes, rva_offset(image, second_ilt + 8U, 8U)) & kOrdinalFlag64) != 0U &&
                (get64(image.bytes, rva_offset(image, second_iat + 8U, 8U)) & 0xffffU) == user_ordinal,
                "second library ordinal import encoding mismatch");
    n36_require((get64(image.bytes, rva_offset(image, first_ilt, 8U)) & kOrdinalFlag64) == 0U &&
                (get64(image.bytes, rva_offset(image, second_ilt, 8U)) & kOrdinalFlag64) == 0U,
                "named imports must not use ordinal flag encoding");
    const auto& text = section(image, ".text");
    const std::int32_t ordinal_jump_disp = static_cast<std::int32_t>(
        get32(image.bytes, static_cast<std::size_t>(text.raw_offset) + 2U));
    const std::int64_t ordinal_jump_expected = static_cast<std::int64_t>(first_iat + 8U) -
        (static_cast<std::int64_t>(text.virtual_address) + 6);
    n36_require(ordinal_jump_disp == ordinal_jump_expected,
                "FF 25 ordinal import call site must target the ordinal IAT slot");
}

void test_iat_call_jump_patching_and_determinism(std::size_t index) {
    auto spec = make_spec(index + 1000U);
    spec.text = {0xffU, 0x15U, 0U, 0U, 0U, 0U,
                 0xffU, 0x25U, 0U, 0U, 0U, 0U, 0xc3U};
    const std::string first_name = "N36Call" + std::to_string(index);
    const std::string second_name = "N36Jump" + std::to_string(index);
    spec.imports = {
        {"KERNEL32.dll", first_name, 0U, Pe64ImportKind::ByName, 0U},
        {"KERNEL32.dll", second_name, 0U, Pe64ImportKind::ByName, 0U}
    };
    spec.iat_call_sites = {
        {2U, "kernel32.DLL", first_name, Pe64IatInstructionKind::CallIndirect, Pe64ImportKind::ByName, 0U},
        {8U, "KERNEL32.dll", second_name, Pe64IatInstructionKind::JumpIndirect, Pe64ImportKind::ByName, 0U}
    };
    const auto first = write_pe64_dll_image(spec);
    const auto second = write_pe64_dll_image(spec);
    n36_require(first.succeeded() && second.succeeded(), "valid FF 15/FF 25 IAT patch spec was rejected");
    n36_require(first.image.bytes == second.image.bytes, "identical import DLL specs must produce deterministic bytes");
    const auto& image = first.image;
    const auto& text = section(image, ".text");
    const std::size_t first_disp_offset = static_cast<std::size_t>(text.raw_offset) + 2U;
    const std::size_t second_disp_offset = static_cast<std::size_t>(text.raw_offset) + 8U;
    const std::int32_t first_disp = static_cast<std::int32_t>(get32(image.bytes, first_disp_offset));
    const std::int32_t second_disp = static_cast<std::int32_t>(get32(image.bytes, second_disp_offset));
    const std::int64_t first_expected = static_cast<std::int64_t>(image.iat_rva) -
        (static_cast<std::int64_t>(text.virtual_address) + 6);
    const std::int64_t second_expected = static_cast<std::int64_t>(image.iat_rva + 8U) -
        (static_cast<std::int64_t>(text.virtual_address) + 12);
    n36_require(first_disp == first_expected && second_disp == second_expected,
                "RIP-relative FF 15/FF 25 displacements do not target the corresponding IAT slots");
    n36_require(image.imported_symbol_count == 2U && image.iat_size == 24U,
                "two named imports in one DLL must include one shared table terminator per ILT/IAT");
}

void test_invalid_import_specs(std::size_t index) {
    auto spec = make_spec(index + 1500U);
    spec.imports = {{"KERNEL32.dll", "GetCurrentProcessId", 0U, Pe64ImportKind::ByName, 0U}};
    switch (index % 18U) {
    case 0U: spec.imports[0U].dll_name.clear(); break;
    case 1U: spec.imports[0U].dll_name.assign(261U, 'D'); break;
    case 2U: spec.imports[0U].dll_name = std::string("KERNEL") + static_cast<char>(1) + ".dll"; break;
    case 3U: spec.imports[0U].symbol_name.clear(); break;
    case 4U: spec.imports[0U].symbol_name.assign(4097U, 'S'); break;
    case 5U: spec.imports[0U].kind = Pe64ImportKind::ByOrdinal; spec.imports[0U].symbol_name = "NotEmpty"; break;
    case 6U: spec.imports[0U].kind = Pe64ImportKind::ByOrdinal; spec.imports[0U].hint = 1U; break;
    case 7U: spec.imports[0U].kind = Pe64ImportKind::ByOrdinal; spec.imports[0U].ordinal = 0U; break;
    case 8U: spec.imports[0U].kind = static_cast<Pe64ImportKind>(99U); break;
    case 9U: spec.imports.push_back(spec.imports[0U]); break;
    case 10U:
        spec.text = {0xffU, 0x15U, 0U, 0U, 0U, 0U, 0xc3U};
        spec.iat_call_sites = {{2U, "KERNEL32.dll", "MissingSymbol", Pe64IatInstructionKind::CallIndirect, Pe64ImportKind::ByName, 0U}};
        break;
    case 11U:
        spec.text = {0x90U, 0x90U, 0U, 0U, 0U, 0U, 0xc3U};
        spec.iat_call_sites = {{2U, "KERNEL32.dll", "GetCurrentProcessId", Pe64IatInstructionKind::CallIndirect, Pe64ImportKind::ByName, 0U}};
        break;
    case 12U:
        spec.text = {0xffU, 0x15U, 0U, 0U, 0U};
        spec.iat_call_sites = {{2U, "KERNEL32.dll", "GetCurrentProcessId", Pe64IatInstructionKind::CallIndirect, Pe64ImportKind::ByName, 0U}};
        break;
    case 13U:
        spec.text = {0xffU, 0x15U, 0xffU, 0x25U, 0U, 0U, 0U, 0U, 0xc3U};
        spec.iat_call_sites = {
            {2U, "KERNEL32.dll", "GetCurrentProcessId", Pe64IatInstructionKind::CallIndirect, Pe64ImportKind::ByName, 0U},
            {4U, "KERNEL32.dll", "GetCurrentProcessId", Pe64IatInstructionKind::JumpIndirect, Pe64ImportKind::ByName, 0U}
        };
        break;
    case 14U:
        spec.imports.assign(4097U, Pe64Import{"KERNEL32.dll", "N36TooMany", 0U, Pe64ImportKind::ByName, 0U});
        break;
    case 15U:
        spec.imports[0U].ordinal = 7U; // Ordinal field is unused for name imports.
        break;
    case 16U:
        spec.imports.push_back(Pe64Import{"kernel32.DLL", "GetCurrentProcessId", 7U,
                                          Pe64ImportKind::ByName, 0U}); // Duplicate despite different hint.
        break;
    case 17U:
        spec.text = {0xffU, 0x15U, 0U, 0U, 0U, 0U, 0xc3U};
        spec.iat_call_sites = {{2U, "KERNEL32.dll", "GetCurrentProcessId",
            Pe64IatInstructionKind::CallIndirect, Pe64ImportKind::ByName, 7U}};
        break;
    }
    const auto rejected = write_pe64_dll_image(spec);
    n36_require(!rejected.succeeded() && !rejected.diagnostics.empty(), "invalid import spec unexpectedly succeeded");
    n36_require(rejected.image.bytes.empty() && rejected.image.sections.empty(),
                "failed DLL import writer must not return a partial image or section list");
    n36_require(rejected.diagnostics.front().code.rfind("XPE64DLL", 0U) == 0U,
                "invalid import spec must return a stable XPE64DLL diagnostic");
}

} // namespace

void run_pe64_dll_import_tests(int& passed, int& failed) {
    for (std::size_t index = 0U; index < 500U; ++index) {
        n36_case("N36A" + std::to_string(index + 1U) + "_pe64_dll_named_import_directory", [index] {
            test_single_named_import(index);
        }, passed, failed);
    }
    for (std::size_t index = 0U; index < 500U; ++index) {
        n36_case("N36B" + std::to_string(index + 1U) + "_pe64_dll_multi_library_ordinal_imports", [index] {
            test_multi_library_and_ordinal_imports(index);
        }, passed, failed);
    }
    for (std::size_t index = 0U; index < 500U; ++index) {
        n36_case("N36C" + std::to_string(index + 1U) + "_pe64_dll_iat_call_jump_patching", [index] {
            test_iat_call_jump_patching_and_determinism(index);
        }, passed, failed);
    }
    for (std::size_t index = 0U; index < 500U; ++index) {
        n36_case("N36D" + std::to_string(index + 1U) + "_reject_invalid_pe64_dll_imports", [index] {
            test_invalid_import_specs(index);
        }, passed, failed);
    }
}
