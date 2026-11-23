#include "xfrontend/pe64_dll.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xfrontend;
namespace {

void n37_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void n37_case(const std::string& name, const std::function<void()>& body,
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
    n37_require(offset <= bytes.size() && bytes.size() - offset >= 2U, "u16 read outside PE image");
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t get32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n37_require(offset <= bytes.size() && bytes.size() - offset >= 4U, "u32 read outside PE image");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t get64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint64_t>(get32(bytes, offset)) |
        (static_cast<std::uint64_t>(get32(bytes, offset + 4U)) << 32U);
}

const Pe64DllSectionInfo& find_section(const Pe64DllImage& image, const char* name) {
    const auto found = std::find_if(image.sections.begin(), image.sections.end(), [name](const auto& item) {
        return item.name == name;
    });
    if (found == image.sections.end()) throw std::runtime_error(std::string("missing section ") + name);
    return *found;
}

std::size_t rva_offset(const Pe64DllImage& image, std::uint32_t rva, std::size_t length) {
    for (const auto& item : image.sections) {
        if (rva < item.virtual_address) continue;
        const std::uint64_t delta = static_cast<std::uint64_t>(rva) - item.virtual_address;
        if (delta <= item.raw_size && length <= static_cast<std::size_t>(item.raw_size - delta))
            return static_cast<std::size_t>(item.raw_offset) + static_cast<std::size_t>(delta);
    }
    throw std::runtime_error("RVA does not map to raw section bytes");
}

Pe64DllSpec make_tls_spec(std::size_t index) {
    Pe64DllSpec spec;
    spec.dll_name = "n37_tls_fixture_" + std::to_string(index) + ".dll";
    spec.image_base = 0x180000000ULL + static_cast<std::uint64_t>(index) * 0x10000ULL;
    spec.text.assign(96U, 0x90U);
    // Minimal DllMain: return TRUE so the loader keeps per-thread DLL
    // notification state active for TLS callback dispatch.
    const std::uint8_t dll_main[] = {0xb8U, 1U, 0U, 0U, 0U, 0xc3U};
    std::copy(std::begin(dll_main), std::end(dll_main), spec.text.begin() + 80);
    spec.has_entry_point = true;
    spec.entry_point_offset = 80U;
    spec.text[95U] = 0xc3U;
    spec.read_only_data = {'N','3','7','T','L','S',0U};
    spec.data.assign(24U, 0U);
    spec.data[0U] = 0x37U;
    spec.tls.enabled = true;
    spec.tls.template_data = {
        static_cast<std::uint8_t>(index & 0xffU),
        static_cast<std::uint8_t>((index >> 8U) & 0xffU),
        0x5aU, 0xa5U
    };
    spec.tls.zero_fill_size = static_cast<std::uint32_t>(index % 257U);
    spec.tls.index_data_offset = 8U;
    spec.tls.callbacks = {
        {Pe64SectionKind::Text, 0U},
        {Pe64SectionKind::Text, 32U}
    };
    spec.imports = {{"KERNEL32.dll", "GetCurrentProcessId", 0U, Pe64ImportKind::ByName, 0U}};
    spec.exports = {
        {"N37TlsMarker", 1U, Pe64SectionKind::Data, 0U, false, {}},
        {"N37TlsIndex", 2U, Pe64SectionKind::Data, spec.tls.index_data_offset, false, {}},
        {"N37TlsCallbackOne", 3U, Pe64SectionKind::Text, 0U, false, {}},
        {"N37TlsCallbackTwo", 4U, Pe64SectionKind::Text, 32U, false, {}}
    };
    return spec;
}

std::set<std::uint32_t> read_dir64_relocations(const Pe64DllImage& image) {
    n37_require(image.relocation_directory_rva != 0U && image.relocation_directory_size != 0U,
                "TLS DLL must have a base-relocation directory");
    std::set<std::uint32_t> targets;
    std::size_t cursor = rva_offset(image, image.relocation_directory_rva, image.relocation_directory_size);
    const std::size_t end = cursor + image.relocation_directory_size;
    while (cursor < end) {
        n37_require(end - cursor >= 8U, "truncated relocation block header");
        const std::uint32_t page = get32(image.bytes, cursor);
        const std::uint32_t block_size = get32(image.bytes, cursor + 4U);
        n37_require(block_size >= 8U && (block_size & 1U) == 0U && block_size <= end - cursor,
                    "invalid relocation block size");
        for (std::size_t offset = cursor + 8U; offset < cursor + block_size; offset += 2U) {
            const std::uint16_t entry = get16(image.bytes, offset);
            const std::uint16_t type = static_cast<std::uint16_t>(entry >> 12U);
            if (type == 0U) continue;
            n37_require(type == 10U, "unexpected non-DIR64 relocation type");
            targets.insert(page + static_cast<std::uint32_t>(entry & 0x0fffU));
        }
        cursor += block_size;
    }
    n37_require(cursor == end, "relocation directory size did not end on a block boundary");
    return targets;
}

void test_tls_directory_layout(std::size_t index) {
    const auto spec = make_tls_spec(index);
    const auto result = write_pe64_dll_image(spec);
    n37_require(result.succeeded(), "valid TLS DLL was rejected" +
        (result.diagnostics.empty() ? std::string{} : ": " + result.diagnostics.front().code + " " + result.diagnostics.front().message));
    const auto& image = result.image;
    const std::size_t pe = get32(image.bytes, 0x3cU);
    const std::size_t optional = pe + 24U;
    n37_require(image.entry_point_rva != 0U && image.entry_point_rva ==
                find_section(image, ".text").virtual_address + spec.entry_point_offset,
                "Optional caller-provided DllMain entry point RVA mismatch");
    n37_require(get32(image.bytes, optional + 16U) == image.entry_point_rva,
                "Optional Header AddressOfEntryPoint mismatch");
    const std::size_t entry_offset = rva_offset(image, image.entry_point_rva, 6U);
    const std::uint8_t expected_dll_main[] = {0xb8U, 1U, 0U, 0U, 0U, 0xc3U};
    n37_require(std::equal(std::begin(expected_dll_main), std::end(expected_dll_main),
                image.bytes.begin() + static_cast<std::ptrdiff_t>(entry_offset)),
                "minimal DllMain fixture must return TRUE");
    n37_require(get32(image.bytes, optional + 112U + 9U * 8U) == image.tls_directory_rva,
                "Optional Header TLS Data Directory RVA mismatch");
    n37_require(get32(image.bytes, optional + 112U + 9U * 8U + 4U) == 40U && image.tls_directory_size == 40U,
                "IMAGE_TLS_DIRECTORY64 data directory size mismatch");
    n37_require(image.tls_directory_rva != 0U && image.tls_index_rva != 0U && image.tls_template_rva != 0U,
                "TLS metadata RVAs must be present");
    n37_require(image.import_directory_rva != 0U && image.iat_rva != 0U &&
                image.imported_library_count == 1U && image.imported_symbol_count == 1U,
                "N36 Import Directory/IAT must remain compatible with TLS metadata");
    n37_require(image.tls_template_size == spec.tls.template_data.size() &&
                image.tls_zero_fill_size == spec.tls.zero_fill_size && image.tls_callback_count == 2U,
                "TLS image metadata counters mismatch");

    const std::size_t directory = rva_offset(image, image.tls_directory_rva, 40U);
    const std::uint64_t start = get64(image.bytes, directory);
    const std::uint64_t finish = get64(image.bytes, directory + 8U);
    const std::uint64_t index_va = get64(image.bytes, directory + 16U);
    const std::uint64_t callbacks_va = get64(image.bytes, directory + 24U);
    n37_require(start == image.image_base + image.tls_template_rva,
                "TLS StartAddressOfRawData must point to the .tls template");
    n37_require(finish == start + spec.tls.template_data.size(),
                "TLS EndAddressOfRawData must exclude zero-fill bytes");
    n37_require(index_va == image.image_base + image.tls_index_rva,
                "TLS AddressOfIndex must point to the writable four-byte index cell");
    n37_require(callbacks_va != 0U, "non-empty TLS callback list must publish AddressOfCallbacks");
    n37_require(get32(image.bytes, directory + 32U) == spec.tls.zero_fill_size,
                "TLS SizeOfZeroFill mismatch");
    n37_require(get32(image.bytes, directory + 36U) == 0U, "TLS Characteristics must use the documented default");

    const auto& tls_section = find_section(image, ".tls");
    n37_require((tls_section.characteristics & 0xc0000000U) == 0xc0000000U,
                ".tls template section must be readable and writable");
    const std::size_t template_offset = rva_offset(image, image.tls_template_rva, spec.tls.template_data.size());
    n37_require(std::equal(spec.tls.template_data.begin(), spec.tls.template_data.end(), image.bytes.begin() +
                static_cast<std::ptrdiff_t>(template_offset)), "TLS initialized template bytes mismatch");
    const std::size_t index_offset = rva_offset(image, image.tls_index_rva, 4U);
    n37_require(get32(image.bytes, index_offset) == 0U, "TLS index cell must be zero before loader initialization");
}

void test_tls_callbacks_and_relocations(std::size_t index) {
    auto spec = make_tls_spec(index + 500U);
    spec.absolute_relocations = {{0U, Pe64SectionKind::Text, 48U, 0}};
    const auto first = write_pe64_dll_image(spec);
    const auto second = write_pe64_dll_image(spec);
    n37_require(first.succeeded() && second.succeeded(), "TLS callback DLL generation failed");
    const auto& image = first.image;
    n37_require(image.bytes == second.image.bytes, "identical TLS callback specs must produce deterministic PE bytes");
    n37_require(image.base_relocation_count == spec.absolute_relocations.size() + 3U + 1U + spec.tls.callbacks.size(),
                "TLS directory and callback pointer relocation count mismatch");
    n37_require(image.has_dynamic_base, "TLS VAs require DYNAMIC_BASE relocation metadata");

    const std::size_t directory = rva_offset(image, image.tls_directory_rva, 40U);
    const std::uint32_t callbacks_rva = static_cast<std::uint32_t>(get64(image.bytes, directory + 24U) - image.image_base);
    const auto relocations = read_dir64_relocations(image);
    std::set<std::uint32_t> expected = {
        image.tls_directory_rva,
        image.tls_directory_rva + 8U,
        image.tls_directory_rva + 16U,
        image.tls_directory_rva + 24U,
        callbacks_rva,
        callbacks_rva + 8U
    };
    const auto& data = find_section(image, ".data");
    expected.insert(data.virtual_address); // user-owned absolute pointer at data offset zero
    n37_require(relocations == expected, "TLS Directory, callback table and user pointer DIR64 targets mismatch");

    const std::uint32_t callback_one_rva = static_cast<std::uint32_t>(get64(image.bytes,
        rva_offset(image, callbacks_rva, 8U)) - image.image_base);
    const std::uint32_t callback_two_rva = static_cast<std::uint32_t>(get64(image.bytes,
        rva_offset(image, callbacks_rva + 8U, 8U)) - image.image_base);
    const auto& text = find_section(image, ".text");
    n37_require(callback_one_rva == text.virtual_address && callback_two_rva == text.virtual_address + 32U,
                "TLS callback VA array order/target mismatch");
    n37_require(image.import_directory_rva != 0U && image.iat_rva != 0U,
                "TLS and import metadata must coexist in the same PE image");
    n37_require(get64(image.bytes, rva_offset(image, callbacks_rva + 16U, 8U)) == 0U,
                "TLS callback array must be null terminated");
}

void test_invalid_tls_specs(std::size_t index) {
    auto spec = make_tls_spec(index + 1000U);
    switch (index % 13U) {
    case 0U: spec.tls.template_data.clear(); spec.tls.zero_fill_size = 0U; break;
    case 1U: spec.tls.template_data.assign(16U * 1024U * 1024U + 1U, 0x41U); break;
    case 2U: spec.tls.zero_fill_size = 16U * 1024U * 1024U + 1U; break;
    case 3U: spec.data.resize(3U); spec.tls.index_data_offset = 0U; break;
    case 4U: spec.tls.index_data_offset = 1U; break;
    case 5U: spec.data[8U] = 1U; break;
    case 6U: spec.tls.callbacks.assign(1025U, Pe64DllTlsCallback{Pe64SectionKind::Text, 0U}); break;
    case 7U: spec.tls.callbacks[0U].target_section = Pe64SectionKind::Data; break;
    case 8U: spec.tls.callbacks[0U].target_offset = spec.text.size(); break;
    case 9U: spec.tls.callbacks[1U].target_offset = spec.tls.callbacks[0U].target_offset; break;
    case 10U: spec.tls.index_data_offset = std::numeric_limits<std::size_t>::max(); break;
    case 11U:
        spec.data.assign(16U, 0U);
        spec.tls.index_data_offset = 4U;
        spec.absolute_relocations = {{0U, Pe64SectionKind::Text, 0U, 0}};
        break;
    case 12U: spec.entry_point_offset = spec.text.size(); break;
    }
    const auto rejected = write_pe64_dll_image(spec);
    n37_require(!rejected.succeeded() && !rejected.diagnostics.empty(),
                "invalid TLS spec unexpectedly succeeded");
    n37_require(rejected.image.bytes.empty() && rejected.image.sections.empty(),
                "invalid TLS spec must not return a partial image or section list");
    const bool tls_diagnostic = rejected.diagnostics.front().code.rfind("XPE64DLL005", 0U) == 0U;
    const bool entry_diagnostic = rejected.diagnostics.front().code.rfind("XPE64DLL006", 0U) == 0U;
    n37_require(tls_diagnostic || entry_diagnostic,
                "invalid TLS/entry-point spec must return a stable XPE64DLL00xx diagnostic");
}

void test_no_tls_compatibility(std::size_t index) {
    Pe64DllSpec spec;
    spec.dll_name = "n37_compat_" + std::to_string(index) + ".dll";
    spec.image_base = 0x180000000ULL + static_cast<std::uint64_t>(index) * 0x10000ULL;
    spec.text = {0xb8U, 0x2aU, 0U, 0U, 0U, 0xc3U, 0xffU, 0x25U, 0U, 0U, 0U, 0U};
    spec.read_only_data = {'C','O','M','P','A','T',0U};
    spec.data.assign(8U, 0U);
    spec.exports = {
        {"N37CompatReturn42", 1U, Pe64SectionKind::Text, 0U, false, {}},
        {"N37CompatData", 2U, Pe64SectionKind::Data, 0U, false, {}}
    };
    if ((index & 1U) != 0U) {
        spec.absolute_relocations = {{0U, Pe64SectionKind::Text, 0U, 0}};
    }
    if ((index & 2U) != 0U) {
        spec.imports = {{"KERNEL32.dll", "GetCurrentProcessId", 0U, Pe64ImportKind::ByName, 0U}};
        spec.iat_call_sites = {{8U, "KERNEL32.dll", "GetCurrentProcessId",
            Pe64IatInstructionKind::JumpIndirect, Pe64ImportKind::ByName, 0U}};
    }
    const auto first = write_pe64_dll_image(spec);
    const auto second = write_pe64_dll_image(spec);
    n37_require(first.succeeded() && second.succeeded(), "pre-N37 DLL compatibility fixture was rejected");
    n37_require(first.image.bytes == second.image.bytes, "pre-N37 DLL bytes must remain deterministic");
    n37_require(first.image.entry_point_rva == 0U,
                "default DLL entry point must remain zero when no entry point is requested");
    n37_require(first.image.tls_directory_rva == 0U && first.image.tls_directory_size == 0U &&
                first.image.tls_index_rva == 0U && first.image.tls_template_rva == 0U &&
                first.image.tls_callback_count == 0U,
                "TLS-disabled DLL must leave TLS image metadata zero");
    const std::size_t pe = get32(first.image.bytes, 0x3cU);
    const std::size_t optional = pe + 24U;
    n37_require(get32(first.image.bytes, optional + 112U + 9U * 8U) == 0U &&
                get32(first.image.bytes, optional + 112U + 9U * 8U + 4U) == 0U,
                "TLS-disabled Optional Header data-directory slot must remain zero");
    n37_require(std::none_of(first.image.sections.begin(), first.image.sections.end(), [](const auto& item) {
        return item.name == ".tls";
    }), "TLS-disabled DLL must not gain a .tls section");
    n37_require(first.image.base_relocation_count == spec.absolute_relocations.size(),
                "TLS-disabled user DIR64 count must remain unchanged");
}

} // namespace

void run_pe64_dll_tls_tests(int& passed, int& failed) {
    for (std::size_t index = 0U; index < 500U; ++index) {
        n37_case("N37A" + std::to_string(index + 1U) + "_pe64_dll_tls_directory_template", [index] {
            test_tls_directory_layout(index);
        }, passed, failed);
    }
    for (std::size_t index = 0U; index < 500U; ++index) {
        n37_case("N37B" + std::to_string(index + 1U) + "_pe64_dll_tls_callbacks_dir64", [index] {
            test_tls_callbacks_and_relocations(index);
        }, passed, failed);
    }
    for (std::size_t index = 0U; index < 500U; ++index) {
        n37_case("N37C" + std::to_string(index + 1U) + "_reject_invalid_pe64_dll_tls", [index] {
            test_invalid_tls_specs(index);
        }, passed, failed);
    }
    for (std::size_t index = 0U; index < 500U; ++index) {
        n37_case("N37D" + std::to_string(index + 1U) + "_pe64_dll_tls_disabled_compat", [index] {
            test_no_tls_compatibility(index);
        }, passed, failed);
    }
}
