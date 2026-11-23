#include "xfrontend/pe64_image.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace xfrontend;

void n29_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void n29_case(const std::string& name, const std::function<void()>& body,
              int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    const auto start = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

std::uint16_t n29_u16(const std::vector<std::uint8_t>& b, std::size_t o) {
    n29_require(o <= b.size() && b.size() - o >= 2U, "u16 read outside PE image");
    return static_cast<std::uint16_t>(b[o]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[o + 1U]) << 8U);
}

std::uint32_t n29_u32(const std::vector<std::uint8_t>& b, std::size_t o) {
    n29_require(o <= b.size() && b.size() - o >= 4U, "u32 read outside PE image");
    return static_cast<std::uint32_t>(b[o]) |
        (static_cast<std::uint32_t>(b[o + 1U]) << 8U) |
        (static_cast<std::uint32_t>(b[o + 2U]) << 16U) |
        (static_cast<std::uint32_t>(b[o + 3U]) << 24U);
}

std::uint64_t n29_u64(const std::vector<std::uint8_t>& b, std::size_t o) {
    n29_require(o <= b.size() && b.size() - o >= 8U, "u64 read outside PE image");
    std::uint64_t value = 0U;
    for (unsigned i = 0U; i < 8U; ++i) value |= static_cast<std::uint64_t>(b[o + i]) << (i * 8U);
    return value;
}

std::size_t n29_rva_offset(const Pe64ExecutableImage& image, std::uint32_t rva) {
    for (const auto& section : image.sections) {
        if (rva >= section.virtual_address &&
            static_cast<std::uint64_t>(rva - section.virtual_address) < section.raw_size)
            return static_cast<std::size_t>(section.raw_offset) + (rva - section.virtual_address);
    }
    throw std::runtime_error("RVA does not map to raw section bytes");
}

Pe64SectionInfo n29_section(const Pe64ExecutableImage& image, const std::string& name) {
    const auto found = std::find_if(image.sections.begin(), image.sections.end(), [&name](const Pe64SectionInfo& s) {
        return s.name == name;
    });
    if (found == image.sections.end()) throw std::runtime_error("required PE section not found: " + name);
    return *found;
}

bool n29_has_diag(const std::vector<Diagnostic>& ds, const std::string& code) {
    return std::any_of(ds.begin(), ds.end(), [&code](const Diagnostic& d) { return d.code == code; });
}

std::string n30_ascii_lower(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'))
            ch = static_cast<char>(byte - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
    }
    return value;
}

std::string n30_cstr(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n29_require(offset < bytes.size(), "string RVA points outside image bytes");
    std::string value;
    while (offset < bytes.size() && bytes[offset] != 0U) {
        value.push_back(static_cast<char>(bytes[offset]));
        ++offset;
    }
    n29_require(offset < bytes.size(), "unterminated PE import string");
    return value;
}

std::uint32_t n30_find_iat_rva(const Pe64ExecutableImage& image,
                              const std::string& dll,
                              Pe64ImportKind kind,
                              const std::string& symbol,
                              std::uint16_t ordinal) {
    const auto& bytes = image.bytes;
    std::size_t descriptor_index = 0U;
    while (true) {
        const std::size_t descriptor = n29_rva_offset(image, image.import_directory_rva) + descriptor_index * 20U;
        const std::uint32_t oft = n29_u32(bytes, descriptor);
        if (oft == 0U) break;
        const std::uint32_t name_rva = n29_u32(bytes, descriptor + 12U);
        const std::uint32_t ft = n29_u32(bytes, descriptor + 16U);
        if (n30_ascii_lower(n30_cstr(bytes, n29_rva_offset(image, name_rva))) == n30_ascii_lower(dll)) {
            for (std::size_t thunk_index = 0U; thunk_index < 4096U; ++thunk_index) {
                const std::uint64_t thunk = n29_u64(bytes,
                    n29_rva_offset(image, oft) + thunk_index * 8U);
                if (thunk == 0U) break;
                bool match = false;
                if ((thunk & 0x8000000000000000ULL) != 0U) {
                    match = kind == Pe64ImportKind::ByOrdinal &&
                        static_cast<std::uint16_t>(thunk & 0xffffU) == ordinal &&
                        (thunk & 0x7fffffffffff0000ULL) == 0U;
                } else if (kind == Pe64ImportKind::ByName) {
                    const std::size_t ibn = n29_rva_offset(image, static_cast<std::uint32_t>(thunk));
                    match = n30_cstr(bytes, ibn + 2U) == symbol;
                }
                if (match) return ft + static_cast<std::uint32_t>(thunk_index * 8U);
            }
        }
        ++descriptor_index;
        n29_require(descriptor_index < 64U, "Import Directory lacks a null descriptor");
    }
    throw std::runtime_error("requested IAT slot was not found");
}

void n30_append_indirect(Pe64ExecutableSpec& spec, Pe64IatInstructionKind instruction,
                         const std::string& dll, const std::string& symbol,
                         Pe64ImportKind import_kind = Pe64ImportKind::ByName,
                         std::uint16_t ordinal = 0U) {
    const std::size_t start = spec.text.size();
    spec.text.push_back(0xffU);
    spec.text.push_back(instruction == Pe64IatInstructionKind::CallIndirect ? 0x15U : 0x25U);
    spec.text.insert(spec.text.end(), 4U, 0U);
    spec.iat_call_sites.push_back(Pe64IatCallSite{
        start + 2U, dll, symbol, instruction, import_kind, ordinal});
}

void n29_expect_failure(const Pe64ExecutableSpec& spec, const std::string& code) {
    const auto result = write_pe64_executable_image(spec);
    n29_require(!result.succeeded(), "invalid PE specification unexpectedly succeeded");
    n29_require(n29_has_diag(result.diagnostics, code), "expected diagnostic missing: " + code);
    n29_require(result.image.bytes.empty(), "failed PE writer must not expose partial image bytes");
}

Pe64ExecutableSpec n29_basic_spec() {
    Pe64ExecutableSpec spec;
    spec.text = {0x31U, 0xc0U, 0xc3U};
    spec.entry_point_offset = 0U;
    return spec;
}

Pe64ExecutableSpec n29_smoke_spec() {
    Pe64ExecutableSpec spec;
    spec.text = make_pe64_exit_process_stub();
    spec.entry_point_offset = 0U;
    spec.imports.push_back(Pe64Import{"KERNEL32.dll", "ExitProcess", 0U});
    spec.iat_call_sites.push_back(Pe64IatCallSite{11U, "kernel32.DLL", "ExitProcess"});
    spec.data.assign(8U, 0U);
    spec.absolute_relocations.push_back(Pe64AbsoluteRelocation{0U, Pe64SectionKind::Text, 0U, 0});
    return spec;
}

} // namespace

void run_pe64_image_tests(int& passed, int& failed) {
    // N29A: DOS/PE/COFF/PE32+ headers, section layout, image sizing and flags.
    for (int index = 0; index < 500; ++index) {
        n29_case("N29A" + std::to_string(index + 1) + "_pe32plus_headers_and_sections", [index] {
            auto spec = n29_basic_spec();
            spec.image_base = 0x140000000ULL + static_cast<std::uint64_t>(index) * 0x10000ULL;
            spec.subsystem = (index % 2 == 0) ? 3U : 2U;
            spec.text.insert(spec.text.end(), static_cast<std::size_t>(index % 29), 0x90U);
            spec.read_only_data.assign(static_cast<std::size_t>(index % 23), 0x5aU);
            spec.data.assign(static_cast<std::size_t>(index % 17), 0x33U);
            spec.bss_size = static_cast<std::size_t>(index % 11);
            if (index % 3 == 0) {
                spec.data.resize(8U, 0U);
                spec.absolute_relocations.push_back(Pe64AbsoluteRelocation{0U, Pe64SectionKind::Text, 0U, 0});
            }
            const auto result = write_pe64_executable_image(spec);
            n29_require(result.succeeded(), "valid PE32+ specification must succeed");
            const auto& b = result.image.bytes;
            n29_require(b[0] == 'M' && b[1] == 'Z', "DOS MZ signature mismatch");
            n29_require(n29_u32(b, 0x3cU) == 0x80U, "e_lfanew mismatch");
            n29_require(b[0x80U] == 'P' && b[0x81U] == 'E' && b[0x82U] == 0U && b[0x83U] == 0U,
                        "PE signature mismatch");
            n29_require(n29_u16(b, 0x84U) == 0x8664U, "Machine must be IMAGE_FILE_MACHINE_AMD64");
            const std::uint16_t sections = n29_u16(b, 0x86U);
            n29_require(sections == result.image.sections.size(), "section count mismatch");
            const std::size_t opt = 0x98U;
            n29_require(n29_u16(b, opt) == 0x20bU, "optional header must be PE32+");
            n29_require(n29_u16(b, opt + 68U) == spec.subsystem, "subsystem mismatch");
            n29_require((n29_u16(b, opt + 70U) & 0x0100U) != 0U, "NX_COMPAT flag missing");
            n29_require(n29_u64(b, opt + 24U) == spec.image_base, "ImageBase mismatch");
            const auto file_align = n29_u32(b, opt + 36U);
            const auto section_align = n29_u32(b, opt + 32U);
            n29_require(file_align == spec.file_alignment && section_align == spec.section_alignment,
                        "PE alignment fields mismatch");
            n29_require(result.image.size_of_headers % file_align == 0U, "SizeOfHeaders must respect FileAlignment");
            n29_require(result.image.size_of_image % section_align == 0U, "SizeOfImage must respect SectionAlignment");
            n29_require(n29_u32(b, opt + 16U) == result.image.entry_point_rva, "AddressOfEntryPoint mismatch");
            n29_require(n29_u32(b, opt + 56U) == result.image.size_of_image, "SizeOfImage optional-header mismatch");
            n29_require(n29_u32(b, opt + 60U) == result.image.size_of_headers, "SizeOfHeaders optional-header mismatch");
            const auto text = n29_section(result.image, ".text");
            const auto rdata = n29_section(result.image, ".rdata");
            const auto data = n29_section(result.image, ".data");
            n29_require(std::none_of(result.image.sections.begin(), result.image.sections.end(), [](const Pe64SectionInfo& section) {
                return section.name == ".idata";
            }), "image without imports must not contain an empty .idata section");
            n29_require(text.virtual_address == 0x1000U && text.characteristics == 0x60000020U,
                        ".text must be RX at RVA 0x1000");
            n29_require(rdata.virtual_address == 0x2000U && rdata.characteristics == 0x40000040U,
                        ".rdata must be read-only at RVA 0x2000");
            n29_require(data.virtual_address == 0x3000U && data.characteristics == 0xc0000040U,
                        ".data must be RW at RVA 0x3000");
            n29_require((text.characteristics & 0x80000000U) == 0U, ".text must not be writable");
            n29_require((rdata.characteristics & 0x80000000U) == 0U, ".rdata must not be writable");
            for (const auto& section : result.image.sections) {
                if (section.raw_size != 0U) {
                    n29_require(section.raw_offset % file_align == 0U, "section raw offset alignment mismatch");
                    n29_require(section.raw_size % file_align == 0U, "section raw size alignment mismatch");
                    n29_require(section.raw_offset + section.raw_size <= b.size(), "section raw range outside file");
                }
            }
            n29_require(result.image.entry_point_rva >= text.virtual_address &&
                        result.image.entry_point_rva < text.virtual_address + text.virtual_size,
                        "entry point must lie inside .text");
        }, passed, failed);
    }

    // N29B: Import descriptors, ILT/IAT tables, imported names and RIP-relative FF 15 patching.
    for (int index = 0; index < 500; ++index) {
        n29_case("N29B" + std::to_string(index + 1) + "_pe_import_directory_and_iat", [index] {
            Pe64ExecutableSpec spec;
            const std::size_t library_count = static_cast<std::size_t>(1 + (index % 5));
            const std::size_t per_library = static_cast<std::size_t>(1 + ((index / 5) % 5));
            std::vector<std::pair<std::string, std::string>> imports;
            for (std::size_t lib = 0U; lib < library_count; ++lib) {
                for (std::size_t item = 0U; item < per_library; ++item) {
                    const std::string dll = (lib == 0U && index % 2 == 0) ? "KERNEL32.dll" : "api" + std::to_string(lib) + ".dll";
                    const std::string name = "Function_" + std::to_string(lib) + "_" + std::to_string(item);
                    spec.imports.push_back(Pe64Import{dll, name, static_cast<std::uint16_t>(item)});
                    imports.emplace_back(dll, name);
                    const std::size_t instruction = spec.text.size();
                    spec.text.insert(spec.text.end(), {0xffU, 0x15U, 0U, 0U, 0U, 0U, 0xc3U});
                    spec.iat_call_sites.push_back(Pe64IatCallSite{instruction + 2U, dll, name});
                }
            }
            if (spec.text.empty()) spec.text = {0xc3U};
            const auto result = write_pe64_executable_image(spec);
            n29_require(result.succeeded(), "valid import image must succeed");
            n29_require(result.image.imported_symbol_count == imports.size(), "import symbol count mismatch");
            n29_require(result.image.imported_library_count == library_count, "import library grouping mismatch");
            n29_require(result.image.import_directory_rva == 0x4000U, "Import Directory must point to .idata start");
            const auto idata = n29_section(result.image, ".idata");
            const auto rdata = n29_section(result.image, ".rdata");
            n29_require(idata.virtual_address == 0x4000U && idata.characteristics == 0xc0000040U,
                        ".idata must be a writable data section at RVA 0x4000 for loader IAT fixups");
            n29_require(rdata.virtual_address == 0x2000U && rdata.characteristics == 0x40000040U,
                        ".rdata must remain read-only and must not contain import thunks");
            n29_require(result.image.iat_rva >= idata.virtual_address &&
                        result.image.iat_rva + result.image.iat_size <= idata.virtual_address + idata.virtual_size,
                        "IAT must reside completely inside writable .idata");
            n29_require(result.image.import_directory_size == (library_count + 1U) * 20U,
                        "Import Directory size must include null descriptor");
            n29_require(result.image.iat_rva != 0U && result.image.iat_size == imports.size() * 8U + library_count * 8U,
                        "IAT directory size mismatch");
            const auto& b = result.image.bytes;
            const std::size_t descriptor = n29_rva_offset(result.image, result.image.import_directory_rva);
            std::size_t descriptor_count = 0U;
            while (descriptor_count < library_count) {
                const std::size_t d = descriptor + descriptor_count * 20U;
                const auto oft = n29_u32(b, d);
                const auto name_rva = n29_u32(b, d + 12U);
                const auto ft = n29_u32(b, d + 16U);
                n29_require(oft != 0U && name_rva != 0U && ft != 0U, "Import Descriptor fields must be non-zero");
                n29_require((oft & 7U) == 0U && (ft & 7U) == 0U, "thunk arrays must be qword aligned");
                const std::size_t name_offset = n29_rva_offset(result.image, name_rva);
                n29_require(name_offset < b.size() && b[name_offset] != 0U, "DLL name string missing");
                std::size_t thunk_index = 0U;
                while (true) {
                    const auto thunk = n29_u64(b, n29_rva_offset(result.image, oft) + thunk_index * 8U);
                    const auto iat = n29_u64(b, n29_rva_offset(result.image, ft) + thunk_index * 8U);
                    if (thunk == 0U) { n29_require(iat == 0U, "IAT terminator mismatch"); break; }
                    n29_require(thunk == iat && thunk < result.image.size_of_image,
                                "ILT/IAT entries must point to IMAGE_IMPORT_BY_NAME RVAs");
                    const std::size_t ibn = n29_rva_offset(result.image, static_cast<std::uint32_t>(thunk));
                    (void)n29_u16(b, ibn);
                    n29_require(b[ibn + 2U] != 0U, "IMAGE_IMPORT_BY_NAME hint/name payload missing");
                    ++thunk_index;
                }
                ++descriptor_count;
            }
            const std::size_t null_descriptor = descriptor + library_count * 20U;
            for (std::size_t byte = 0U; byte < 20U; ++byte)
                n29_require(b[null_descriptor + byte] == 0U, "Import Directory null descriptor must be zero");
            for (const auto& call : spec.iat_call_sites) {
                const auto disp = static_cast<std::int32_t>(n29_u32(b,
                    n29_rva_offset(result.image, 0x1000U) + call.displacement_offset));
                const std::int64_t call_end = static_cast<std::int64_t>(0x1000U + call.displacement_offset + 4U);
                const std::uint32_t target_rva = static_cast<std::uint32_t>(call_end + disp);
                n29_require(target_rva >= result.image.iat_rva &&
                            target_rva < result.image.iat_rva + result.image.iat_size,
                            "patched FF 15 must address an IAT entry");
            }
            n29_require(n29_u32(b, 0x98U + 112U + 8U) == result.image.import_directory_rva,
                        "Optional Header Import Directory RVA mismatch");
            n29_require(n29_u32(b, 0x98U + 112U + 8U + 4U) == result.image.import_directory_size,
                        "Optional Header Import Directory size mismatch");
        }, passed, failed);
    }

    // N29C: DIR64 base relocation blocks and preferred-base pointer values.
    for (int index = 0; index < 500; ++index) {
        n29_case("N29C" + std::to_string(index + 1) + "_pe_dir64_base_relocations", [index] {
            auto spec = n29_basic_spec();
            const std::size_t count = static_cast<std::size_t>(1 + (index % 48));
            spec.data.assign(count * 8U, 0U);
            for (std::size_t i = 0U; i < count; ++i) {
                const auto target_section = (i % 3U == 0U) ? Pe64SectionKind::Text :
                    ((i % 3U == 1U) ? Pe64SectionKind::ReadOnlyData : Pe64SectionKind::Data);
                if (target_section == Pe64SectionKind::ReadOnlyData && spec.read_only_data.empty())
                    spec.read_only_data = {0x11U, 0x22U, 0x33U};
                const std::size_t target_offset = (target_section == Pe64SectionKind::Text) ? 0U :
                    ((target_section == Pe64SectionKind::ReadOnlyData) ? 1U : 0U);
                const std::int64_t addend = static_cast<std::int64_t>((index % 7) - 3);
                spec.absolute_relocations.push_back(Pe64AbsoluteRelocation{i * 8U, target_section, target_offset, addend});
            }
            if (index % 4 == 0) {
                spec.imports.push_back(Pe64Import{"USER32.dll", "MessageBeep", 0U});
            }
            const auto result = write_pe64_executable_image(spec);
            n29_require(result.succeeded(), "valid DIR64 relocation spec must succeed");
            n29_require(result.image.has_dynamic_base, "DIR64 image must enable DYNAMIC_BASE");
            n29_require(result.image.base_relocation_count == count, "base relocation count mismatch");
            const std::uint32_t expected_reloc_rva = spec.imports.empty() ? 0x4000U : 0x5000U;
            n29_require(result.image.relocation_directory_rva == expected_reloc_rva &&
                        result.image.relocation_directory_size >= 12U,
                        "Base Relocation Directory must be present");
            const auto& b = result.image.bytes;
            const std::size_t reloc = n29_rva_offset(result.image, result.image.relocation_directory_rva);
            const auto page = n29_u32(b, reloc);
            const auto block_size = n29_u32(b, reloc + 4U);
            n29_require(page == 0x3000U, "relocation block page RVA mismatch");
            n29_require(block_size == result.image.relocation_directory_size &&
                        (block_size & 3U) == 0U, "relocation block size/alignment mismatch");
            std::size_t seen = 0U;
            for (std::size_t offset = 8U; offset < block_size; offset += 2U) {
                const std::uint16_t entry = n29_u16(b, reloc + offset);
                const std::uint16_t type = static_cast<std::uint16_t>(entry >> 12U);
                const std::uint16_t page_offset = static_cast<std::uint16_t>(entry & 0x0fffU);
                if (type == 0U) continue; // ABSOLUTE padding entry
                n29_require(type == 10U, "only IMAGE_REL_BASED_DIR64 is supported in N29");
                n29_require((page_offset & 7U) == 0U, "DIR64 relocation offset must be qword aligned");
                const std::size_t pointer_file_offset = n29_rva_offset(result.image, page + page_offset);
                const std::size_t ordinal = seen++;
                const auto& relocation = spec.absolute_relocations[ordinal];
                std::uint32_t target_rva = 0U;
                if (relocation.target_section == Pe64SectionKind::Text) target_rva = 0x1000U + static_cast<std::uint32_t>(relocation.target_offset);
                else if (relocation.target_section == Pe64SectionKind::ReadOnlyData) target_rva = result.image.read_only_data_rva + static_cast<std::uint32_t>(relocation.target_offset);
                else target_rva = 0x3000U + static_cast<std::uint32_t>(relocation.target_offset);
                const std::int64_t signed_value = static_cast<std::int64_t>(spec.image_base + target_rva) + relocation.addend;
                n29_require(n29_u64(b, pointer_file_offset) == static_cast<std::uint64_t>(signed_value),
                            "relocated absolute pointer does not contain ImageBase + RVA + addend");
            }
            n29_require(seen == count, "DIR64 relocation entry count mismatch");
            n29_require((n29_u16(b, 0x98U + 70U) & 0x0040U) != 0U,
                        "IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE missing");
            n29_require(n29_u32(b, 0x98U + 112U + 5U * 8U) == expected_reloc_rva,
                        "Base Relocation Directory optional-header RVA mismatch");
        }, passed, failed);
    }

    // N29D: malformed inputs fail closed and never return partial image bytes.
    for (int index = 0; index < 500; ++index) {
        n29_case("N29D" + std::to_string(index + 1) + "_pe32plus_rejection_matrix", [index] {
            auto spec = n29_smoke_spec();
            switch (index % 18) {
            case 0: spec.text.clear(); n29_expect_failure(spec, "XPE0001"); break;
            case 1: spec.entry_point_offset = spec.text.size(); n29_expect_failure(spec, "XPE0002"); break;
            case 2: spec.image_base += 1U; n29_expect_failure(spec, "XPE0003"); break;
            case 3: spec.file_alignment = 0x300U; n29_expect_failure(spec, "XPE0004"); break;
            case 4: spec.section_alignment = 0x100U; n29_expect_failure(spec, "XPE0004"); break;
            case 5: spec.subsystem = 1U; n29_expect_failure(spec, "XPE0005"); break;
            case 6: spec.maximum_file_size = 0x100U; n29_expect_failure(spec, "XPE0006"); break;
            case 7: spec.imports[0].dll_name.clear(); n29_expect_failure(spec, "XPE0008"); break;
            case 8: spec.imports.push_back(spec.imports[0]); n29_expect_failure(spec, "XPE0009"); break;
            case 9: spec.iat_call_sites[0].displacement_offset = 0U; n29_expect_failure(spec, "XPE0010"); break;
            case 10: spec.iat_call_sites[0].symbol_name = "Missing"; n29_expect_failure(spec, "XPE0011"); break;
            case 11: spec.absolute_relocations[0].destination_offset = 1U; n29_expect_failure(spec, "XPE0012"); break;
            case 12: spec.absolute_relocations.push_back(spec.absolute_relocations[0]); n29_expect_failure(spec, "XPE0013"); break;
            case 13: spec.absolute_relocations[0].target_offset = spec.text.size(); n29_expect_failure(spec, "XPE0014"); break;
            case 14: spec.iat_call_sites.push_back(spec.iat_call_sites[0]); n29_expect_failure(spec, "XPE0024"); break;
            case 15:
                spec.imports.clear();
                spec.iat_call_sites.clear();
                for (std::size_t item = 0U; item < 40U; ++item)
                    spec.imports.push_back(Pe64Import{"KERNEL32.dll", "Function_" + std::to_string(item) + std::string(128U, 'X'), 0U});
                n29_expect_failure(spec, "XPE0025");
                break;
            case 16: spec.imports[0].symbol_name.assign(5000U, 'X'); n29_expect_failure(spec, "XPE0025"); break;
            default: spec.text.resize(0x1001U, 0x90U); n29_expect_failure(spec, "XPE0015"); break;
            }
        }, passed, failed);
    }

    // N30A: stable multi-DLL descriptor grouping and independent ILT/IAT tables.
    for (int index = 0; index < 500; ++index) {
        n29_case("N30A" + std::to_string(index + 1) + "_pe_multi_dll_import_descriptor_matrix", [index] {
            Pe64ExecutableSpec spec;
            spec.text = {0xc3U};
            spec.function_unwind.push_back(Pe64FunctionUnwind{0U, 1U, 0U, {}});
            const std::size_t libraries = static_cast<std::size_t>(2 + (index % 5));
            const std::size_t per_library = static_cast<std::size_t>(1 + ((index / 5) % 6));
            std::vector<std::string> first_spellings;
            for (std::size_t lib = 0U; lib < libraries; ++lib) {
                const std::string original = "Library" + std::to_string(lib) + ".DLL";
                first_spellings.push_back(original);
                for (std::size_t item = 0U; item < per_library; ++item) {
                    const std::string dll = item % 2U == 0U ? original : n30_ascii_lower(original);
                    spec.imports.push_back(Pe64Import{dll,
                        "Export_" + std::to_string(lib) + "_" + std::to_string(item),
                        static_cast<std::uint16_t>(item)});
                }
            }
            const auto result = write_pe64_executable_image(spec);
            n29_require(result.succeeded(), "valid multi-DLL name imports must succeed");
            n29_require(result.image.imported_library_count == libraries, "case-insensitive DLL grouping count mismatch");
            n29_require(result.image.imported_symbol_count == libraries * per_library, "multi-DLL symbol count mismatch");
            n29_require(result.image.imported_name_count == result.image.imported_symbol_count &&
                        result.image.imported_ordinal_count == 0U, "name/ordinal statistics mismatch");
            n29_require(result.image.unwind_function_count == 1U && result.image.exception_directory_size == 12U,
                        "one runtime function must produce one .pdata entry and exception directory");
            const auto pdata = n29_section(result.image, ".pdata");
            const auto xdata = n29_section(result.image, ".xdata");
            n29_require(pdata.characteristics == 0x40000040U && xdata.characteristics == 0x40000040U,
                        ".pdata and .xdata must be read-only metadata sections");
            const auto pdata_off = n29_rva_offset(result.image, result.image.exception_directory_rva);
            n29_require(n29_u32(result.image.bytes, pdata_off) == 0x1000U &&
                        n29_u32(result.image.bytes, pdata_off + 4U) == 0x1001U &&
                        n29_u32(result.image.bytes, pdata_off + 8U) == result.image.xdata_rva,
                        "RUNTIME_FUNCTION begin/end/unwind RVA mismatch");
            const auto xdata_off = n29_rva_offset(result.image, result.image.xdata_rva);
            n29_require(result.image.bytes[xdata_off] == 1U && result.image.bytes[xdata_off + 1U] == 0U &&
                        result.image.bytes[xdata_off + 2U] == 0U && result.image.bytes[xdata_off + 3U] == 0U,
                        "empty-prologue UNWIND_INFO header mismatch");
            n29_require(n29_u32(result.image.bytes, 0x98U + 112U + 3U * 8U) == result.image.exception_directory_rva &&
                        n29_u32(result.image.bytes, 0x98U + 112U + 3U * 8U + 4U) == 12U,
                        "Exception Directory data-directory entry mismatch");
            const auto& bytes = result.image.bytes;
            const std::size_t import_base = n29_rva_offset(result.image, result.image.import_directory_rva);
            std::size_t total_iat_slots = 0U;
            for (std::size_t lib = 0U; lib < libraries; ++lib) {
                const std::size_t descriptor = import_base + lib * 20U;
                const std::uint32_t oft = n29_u32(bytes, descriptor);
                const std::uint32_t name_rva = n29_u32(bytes, descriptor + 12U);
                const std::uint32_t ft = n29_u32(bytes, descriptor + 16U);
                n29_require(oft != 0U && ft != 0U && oft != ft, "each DLL requires separate nonzero ILT and IAT");
                n29_require(n30_cstr(bytes, n29_rva_offset(result.image, name_rva)) == first_spellings[lib],
                            "first DLL spelling/order must remain stable after case-insensitive grouping");
                for (std::size_t item = 0U; item < per_library; ++item) {
                    const std::uint64_t lookup = n29_u64(bytes, n29_rva_offset(result.image, oft) + item * 8U);
                    const std::uint64_t address = n29_u64(bytes, n29_rva_offset(result.image, ft) + item * 8U);
                    n29_require(lookup != 0U && lookup == address &&
                                (lookup & 0x8000000000000000ULL) == 0U,
                                "name-based ILT/IAT entry must initially contain the same IMAGE_IMPORT_BY_NAME RVA");
                    const std::size_t ibn = n29_rva_offset(result.image, static_cast<std::uint32_t>(lookup));
                    n29_require(n29_u16(bytes, ibn) == item, "import hint mismatch");
                    n29_require(n30_cstr(bytes, ibn + 2U) ==
                                "Export_" + std::to_string(lib) + "_" + std::to_string(item),
                                "IMAGE_IMPORT_BY_NAME spelling mismatch");
                }
                n29_require(n29_u64(bytes, n29_rva_offset(result.image, oft) + per_library * 8U) == 0U &&
                            n29_u64(bytes, n29_rva_offset(result.image, ft) + per_library * 8U) == 0U,
                            "ILT and IAT must both be terminated per DLL");
                total_iat_slots += per_library + 1U;
            }
            const std::size_t null_descriptor = import_base + libraries * 20U;
            for (std::size_t byte = 0U; byte < 20U; ++byte)
                n29_require(bytes[null_descriptor + byte] == 0U, "multi-DLL Import Directory must end with a null descriptor");
            n29_require(result.image.iat_size == total_iat_slots * 8U,
                        "IAT directory must cover every DLL thunk terminator");
        }, passed, failed);
    }

    // N30B: IMAGE_ORDINAL_FLAG64 encoding, mixed name/ordinal thunk tables and ordinal call references.
    for (int index = 0; index < 500; ++index) {
        n29_case("N30B" + std::to_string(index + 1) + "_pe_import_by_ordinal_matrix", [index] {
            Pe64ExecutableSpec spec;
            spec.text = {0x48U, 0x83U, 0xecU, 0x28U, 0xc3U};
            spec.function_unwind.push_back(Pe64FunctionUnwind{0U, 5U, 4U,
                {Pe64UnwindCode{4U, Pe64UnwindOperation::AllocSmall, 0x28U}}});
            const std::size_t count = static_cast<std::size_t>(4 + (index % 24));
            for (std::size_t item = 0U; item < count; ++item) {
                if (item % 2U == 0U) {
                    Pe64Import imported{"ORDINALS.dll", "", 0U};
                    imported.kind = Pe64ImportKind::ByOrdinal;
                    imported.ordinal = static_cast<std::uint16_t>((static_cast<std::size_t>(index) * 127U + item * 257U) & 0xffffU);
                    spec.imports.push_back(imported);
                } else {
                    spec.imports.push_back(Pe64Import{"ordinals.DLL", "NamedExport_" + std::to_string(item),
                                                       static_cast<std::uint16_t>(item)});
                }
            }
            const auto result = write_pe64_executable_image(spec);
            n29_require(result.succeeded(), "mixed name/ordinal import directory must succeed");
            const auto expected_ordinals = (count + 1U) / 2U;
            n29_require(result.image.imported_ordinal_count == expected_ordinals &&
                        result.image.imported_name_count == count - expected_ordinals,
                        "import-by-name and import-by-ordinal counters mismatch");
            n29_require(result.image.imported_library_count == 1U, "DLL names must group case-insensitively");
            n29_require(result.image.unwind_function_count == 1U, "prologue descriptor must emit one unwind function");
            const std::size_t xdata_off = n29_rva_offset(result.image, result.image.xdata_rva);
            n29_require(result.image.bytes[xdata_off] == 1U && result.image.bytes[xdata_off + 1U] == 4U &&
                        result.image.bytes[xdata_off + 2U] == 1U && result.image.bytes[xdata_off + 3U] == 0U,
                        "UNWIND_INFO version/prologue/count/frame fields mismatch");
            n29_require(result.image.bytes[xdata_off + 4U] == 4U &&
                        result.image.bytes[xdata_off + 5U] == 0x42U,
                        "UWOP_ALLOC_SMALL(0x28) code slot mismatch");
            const std::size_t descriptor = n29_rva_offset(result.image, result.image.import_directory_rva);
            const std::uint32_t oft = n29_u32(result.image.bytes, descriptor);
            const std::uint32_t ft = n29_u32(result.image.bytes, descriptor + 16U);
            for (std::size_t item = 0U; item < count; ++item) {
                const std::uint64_t lookup = n29_u64(result.image.bytes, n29_rva_offset(result.image, oft) + item * 8U);
                const std::uint64_t address = n29_u64(result.image.bytes, n29_rva_offset(result.image, ft) + item * 8U);
                n29_require(lookup == address && lookup != 0U, "ordinal/name ILT and IAT initial values must match");
                if (item % 2U == 0U) {
                    const auto expected = static_cast<std::uint16_t>((static_cast<std::size_t>(index) * 127U + item * 257U) & 0xffffU);
                    n29_require((lookup & 0x8000000000000000ULL) != 0U &&
                                (lookup & 0x7fffffffffff0000ULL) == 0U &&
                                static_cast<std::uint16_t>(lookup & 0xffffU) == expected,
                                "ordinal thunk must set only IMAGE_ORDINAL_FLAG64 and the low 16-bit ordinal");
                } else {
                    n29_require((lookup & 0x8000000000000000ULL) == 0U,
                                "named thunk must contain an RVA, not the ordinal flag");
                    const std::size_t ibn = n29_rva_offset(result.image,
                        static_cast<std::uint32_t>(lookup));
                    n29_require(n30_cstr(result.image.bytes, ibn + 2U) == "NamedExport_" + std::to_string(item),
                                "mixed name thunk symbol mismatch");
                }
            }
            n29_require(n29_u64(result.image.bytes, n29_rva_offset(result.image, oft) + count * 8U) == 0U,
                        "mixed lookup thunk table terminator missing");
        }, passed, failed);
    }

    // N30C: FF 15 call, FF 25 tail-jump, repeated sites and ordinal IAT references.
    for (int index = 0; index < 500; ++index) {
        n29_case("N30C" + std::to_string(index + 1) + "_pe_iat_call_jump_patch_matrix", [index] {
            const auto startup_stub = make_pe64_multi_import_stub();
            n29_require(startup_stub.size() == 74U, "multi-import startup stub size/branch layout mismatch");
            n29_require(startup_stub[40U] == 0x74U && startup_stub[41U] == 0x0fU,
                        "RtlLookupFunctionEntry failure branch must reach the failure path");
            n29_require(startup_stub[47U] == 0x48U && startup_stub[48U] == 0x83U &&
                        startup_stub[49U] == 0xc4U && startup_stub[50U] == 0x28U &&
                        startup_stub[62U] == 0x48U && startup_stub[63U] == 0x83U &&
                        startup_stub[64U] == 0xc4U && startup_stub[65U] == 0x28U,
                        "both tail-jump paths must restore the 0x28-byte entry stack allocation");
            n29_require(startup_stub[51U] == 0xffU && startup_stub[52U] == 0x25U &&
                        startup_stub[66U] == 0xffU && startup_stub[67U] == 0x25U,
                        "both ExitProcess paths must use valid FF 25 indirect tail jumps");
            Pe64ExecutableSpec spec;
            spec.text = {0x48U, 0x83U, 0xecU, 0x28U};
            spec.imports.push_back(Pe64Import{"KERNEL32.dll", "GetCurrentProcessId", 0U});
            spec.imports.push_back(Pe64Import{"kernel32.DLL", "ExitProcess", 0U});
            spec.imports.push_back(Pe64Import{"NTDLL.dll", "NtYieldExecution", 0U});
            spec.function_unwind.push_back(Pe64FunctionUnwind{0U, 4U, 4U,
                {Pe64UnwindCode{4U, Pe64UnwindOperation::AllocSmall, 0x28U}}});
            Pe64Import ordinal{"KERNEL32.dll", "", 0U};
            ordinal.kind = Pe64ImportKind::ByOrdinal;
            ordinal.ordinal = static_cast<std::uint16_t>(100U + (index % 500));
            spec.imports.push_back(ordinal);
            const std::size_t repeats = static_cast<std::size_t>(1 + (index % 5));
            for (std::size_t repeat = 0U; repeat < repeats; ++repeat)
                n30_append_indirect(spec, Pe64IatInstructionKind::CallIndirect,
                                    "KERNEL32.dll", "GetCurrentProcessId");
            n30_append_indirect(spec, Pe64IatInstructionKind::CallIndirect,
                                "ntdll.DLL", "NtYieldExecution");
            n30_append_indirect(spec, Pe64IatInstructionKind::CallIndirect,
                                "kernel32.dll", "", Pe64ImportKind::ByOrdinal, ordinal.ordinal);
            n30_append_indirect(spec, Pe64IatInstructionKind::JumpIndirect,
                                "KERNEL32.dll", "ExitProcess");
            spec.function_unwind[0].end_offset = spec.text.size();
            const auto result = write_pe64_executable_image(spec);
            n29_require(result.succeeded(), "valid mixed FF 15/FF 25 IAT patch specification must succeed");
            n29_require(result.image.imported_library_count == 2U &&
                        result.image.imported_symbol_count == 4U,
                        "multi-DLL and mixed import count mismatch");
            for (const auto& site : spec.iat_call_sites) {
                const auto text_file = n29_rva_offset(result.image, 0x1000U);
                const std::uint8_t expected_opcode = site.instruction_kind == Pe64IatInstructionKind::CallIndirect ? 0x15U : 0x25U;
                n29_require(result.image.bytes[text_file + site.displacement_offset - 2U] == 0xffU &&
                            result.image.bytes[text_file + site.displacement_offset - 1U] == expected_opcode,
                            "indirect call/jump opcode must be preserved");
                const std::int32_t displacement = static_cast<std::int32_t>(n29_u32(
                    result.image.bytes, text_file + site.displacement_offset));
                const std::int64_t instruction_end = static_cast<std::int64_t>(0x1000U + site.displacement_offset + 4U);
                const auto target = static_cast<std::uint32_t>(instruction_end + displacement);
                const auto expected_iat = n30_find_iat_rva(result.image, site.dll_name,
                    site.import_kind, site.symbol_name, site.ordinal);
                n29_require(target == expected_iat,
                            "patched FF 15/FF 25 RIP-relative displacement must target the selected IAT qword");
            }
            std::uint32_t shared_getpid_slot = 0U;
            std::size_t getpid_site_count = 0U;
            for (const auto& site : spec.iat_call_sites) {
                if (site.import_kind == Pe64ImportKind::ByName && site.symbol_name == "GetCurrentProcessId") {
                    const std::uint32_t slot = n30_find_iat_rva(result.image, site.dll_name,
                        site.import_kind, site.symbol_name, site.ordinal);
                    if (getpid_site_count == 0U) shared_getpid_slot = slot;
                    else n29_require(slot == shared_getpid_slot,
                        "repeated references to the same import must share a single IAT slot");
                    ++getpid_site_count;
                }
            }
            n29_require(getpid_site_count == repeats,
                        "all repeated GetCurrentProcessId IAT references must be represented");
            n29_require(shared_getpid_slot != n30_find_iat_rva(result.image, "NTDLL.dll",
                        Pe64ImportKind::ByName, "NtYieldExecution", 0U),
                        "different imports must not alias a single IAT slot");
        }, passed, failed);
    }

    // N30D: ordinal/name validation, unknown selectors, table limits and valid edge values.
    for (int index = 0; index < 500; ++index) {
        n29_case("N30D" + std::to_string(index + 1) + "_pe_import_ordinal_validation_matrix", [index] {
            auto spec = n29_basic_spec();
            switch (index % 30) {
            case 0:
                spec.imports.push_back(Pe64Import{"KERNEL32.dll", "Name", 0U});
                spec.imports.back().kind = static_cast<Pe64ImportKind>(0xffU);
                n29_expect_failure(spec, "XPE0026");
                break;
            case 1: {
                Pe64Import item{"KERNEL32.dll", "NotAllowed", 0U};
                item.kind = Pe64ImportKind::ByOrdinal; item.ordinal = 7U;
                spec.imports.push_back(item); n29_expect_failure(spec, "XPE0027"); break;
            }
            case 2: {
                Pe64Import item{"KERNEL32.dll", "", 1U};
                item.kind = Pe64ImportKind::ByOrdinal; item.ordinal = 7U;
                spec.imports.push_back(item); n29_expect_failure(spec, "XPE0027"); break;
            }
            case 3: {
                Pe64Import item{"KERNEL32.dll", "", 0U}; item.kind = Pe64ImportKind::ByOrdinal; item.ordinal = 9U;
                spec.imports.push_back(item);
                spec.text = {0xffU, 0x15U, 0U, 0U, 0U, 0U};
                spec.iat_call_sites.push_back(Pe64IatCallSite{2U, "KERNEL32.dll", "BadName",
                    Pe64IatInstructionKind::CallIndirect, Pe64ImportKind::ByOrdinal, 9U});
                n29_expect_failure(spec, "XPE0027"); break;
            }
            case 4:
                spec.imports.push_back(Pe64Import{"KERNEL32.dll", "ExitProcess", 0U});
                spec.iat_call_sites.push_back(Pe64IatCallSite{2U, "KERNEL32.dll", "ExitProcess",
                    static_cast<Pe64IatInstructionKind>(0xffU)});
                spec.text = {0xffU, 0x15U, 0U, 0U, 0U, 0U};
                n29_expect_failure(spec, "XPE0028"); break;
            case 5:
                spec.imports.push_back(Pe64Import{"KERNEL32.dll", "ExitProcess", 0U});
                spec.iat_call_sites.push_back(Pe64IatCallSite{2U, "KERNEL32.dll", "ExitProcess",
                    Pe64IatInstructionKind::CallIndirect, static_cast<Pe64ImportKind>(0xffU)});
                spec.text = {0xffU, 0x15U, 0U, 0U, 0U, 0U};
                n29_expect_failure(spec, "XPE0028"); break;
            case 6: {
                Pe64Import item{"KERNEL32.dll", "", 0U}; item.kind = Pe64ImportKind::ByOrdinal; item.ordinal = 5U;
                spec.imports.push_back(item);
                spec.text = {0xffU, 0x15U, 0U, 0U, 0U, 0U};
                spec.iat_call_sites.push_back(Pe64IatCallSite{2U, "KERNEL32.dll", "", Pe64IatInstructionKind::CallIndirect,
                    Pe64ImportKind::ByOrdinal, 6U});
                n29_expect_failure(spec, "XPE0011"); break;
            }
            case 7: {
                Pe64Import item{"KERNEL32.dll", "", 0U}; item.kind = Pe64ImportKind::ByOrdinal; item.ordinal = 5U;
                spec.imports.push_back(item); spec.imports.push_back(item);
                n29_expect_failure(spec, "XPE0009"); break;
            }
            case 8:
                spec.imports.push_back(Pe64Import{"KERNEL32.dll", "Same", 0U});
                spec.imports.push_back(Pe64Import{"kernel32.DLL", "Same", 0U});
                n29_expect_failure(spec, "XPE0009"); break;
            case 9:
                spec.imports.push_back(Pe64Import{"KERNEL32.dll", "ExitProcess", 0U});
                spec.text = {0xffU, 0x15U, 0U, 0U, 0U, 0U};
                spec.iat_call_sites.push_back(Pe64IatCallSite{2U, "KERNEL32.dll", "ExitProcess",
                    Pe64IatInstructionKind::JumpIndirect});
                n29_expect_failure(spec, "XPE0010"); break;
            case 10:
                spec.imports.push_back(Pe64Import{"KERNEL32.dll", "ExitProcess", 0U});
                spec.text = {0xffU, 0x25U, 0U, 0U, 0U, 0U};
                spec.iat_call_sites.push_back(Pe64IatCallSite{2U, "KERNEL32.dll", "ExitProcess",
                    Pe64IatInstructionKind::CallIndirect});
                n29_expect_failure(spec, "XPE0010"); break;
            case 11:
                spec.imports.clear();
                for (std::size_t item = 0U; item < 4097U; ++item)
                    spec.imports.push_back(Pe64Import{"LIMIT.dll", "Symbol" + std::to_string(item), 0U});
                n29_expect_failure(spec, "XPE0007"); break;
            case 12:
                spec.imports.clear();
                for (std::size_t item = 0U; item < 40U; ++item)
                    spec.imports.push_back(Pe64Import{"BIG.dll", "Function_" + std::to_string(item) + std::string(128U, 'Z'), 0U});
                n29_expect_failure(spec, "XPE0025"); break;
            case 13: spec.imports.push_back(Pe64Import{std::string(261U, 'D'), "Name", 0U}); n29_expect_failure(spec, "XPE0025"); break;
            case 14: {
                std::string dll("Bad"); dll.push_back('\0'); dll += ".dll";
                spec.imports.push_back(Pe64Import{dll, "Name", 0U}); n29_expect_failure(spec, "XPE0008"); break;
            }
            case 15: {
                std::string name("Bad"); name.push_back('\0'); name += "Name";
                spec.imports.push_back(Pe64Import{"KERNEL32.dll", name, 0U}); n29_expect_failure(spec, "XPE0008"); break;
            }
            case 16: {
                Pe64Import a{"KERNEL32.dll", "", 0U}; a.kind = Pe64ImportKind::ByOrdinal; a.ordinal = 0U;
                Pe64Import b{"KERNEL32.dll", "", 0U}; b.kind = Pe64ImportKind::ByOrdinal; b.ordinal = 65535U;
                spec.imports = {a, b};
                const auto result = write_pe64_executable_image(spec);
                n29_require(result.succeeded() && result.image.imported_ordinal_count == 2U,
                            "ordinal values at both uint16 boundaries must be accepted");
                break;
            }
            case 17: {
                Pe64Import item{"KERNEL32.dll", "", 0U}; item.kind = Pe64ImportKind::ByOrdinal; item.ordinal = 0x1234U;
                spec.imports.push_back(item);
                spec.text = {0xffU, 0x25U, 0U, 0U, 0U, 0U};
                spec.iat_call_sites.push_back(Pe64IatCallSite{2U, "kernel32.DLL", "", Pe64IatInstructionKind::JumpIndirect,
                    Pe64ImportKind::ByOrdinal, 0x1234U});
                const auto result = write_pe64_executable_image(spec);
                n29_require(result.succeeded(), "ordinal-based FF 25 tail jump must be accepted");
                const auto displacement = static_cast<std::int32_t>(n29_u32(result.image.bytes,
                    n29_rva_offset(result.image, 0x1000U) + 2U));
                n29_require(static_cast<std::uint32_t>(0x1006 + displacement) ==
                    n30_find_iat_rva(result.image, "KERNEL32.dll", Pe64ImportKind::ByOrdinal, "", 0x1234U),
                    "ordinal FF 25 patch must target its IAT slot");
                break;
            }
            case 18: {
                spec.imports.push_back(Pe64Import{"KERNEL32.dll", "CaseSensitive", 0U});
                spec.imports.push_back(Pe64Import{"kernel32.dll", "casesensitive", 0U});
                const auto result = write_pe64_executable_image(spec);
                n29_require(result.succeeded() && result.image.imported_symbol_count == 2U &&
                            result.image.imported_library_count == 1U,
                            "exported symbol names must remain case-sensitive while DLL grouping is not");
                break;
            }
            case 19: {
                spec.imports.push_back(Pe64Import{"KERNEL32.dll", "ExitProcess", 0U});
                spec.imports.push_back(Pe64Import{"NTDLL.dll", "NtYieldExecution", 0U});
                const auto result = write_pe64_executable_image(spec);
                n29_require(result.succeeded() && result.image.imported_library_count == 2U,
                            "two independent imports from distinct DLLs must be accepted");
                break;
            }
            case 20:
                spec.function_unwind.push_back(Pe64FunctionUnwind{2U, 1U, 1U, {}});
                n29_expect_failure(spec, "XPE0031"); break;
            case 21:
                spec.text.resize(8U, 0x90U);
                spec.function_unwind.push_back(Pe64FunctionUnwind{0U, spec.text.size(), 4U,
                    {Pe64UnwindCode{4U, Pe64UnwindOperation::AllocSmall, 136U}}});
                n29_expect_failure(spec, "XPE0034"); break;
            case 22:
                spec.text.resize(8U, 0x90U);
                spec.function_unwind.push_back(Pe64FunctionUnwind{0U, spec.text.size(), 4U,
                    {Pe64UnwindCode{4U, Pe64UnwindOperation::PushNonVol, 4U}}});
                n29_expect_failure(spec, "XPE0034"); break;
            case 23:
                spec.text.resize(64U, 0x90U);
                spec.function_unwind.push_back(Pe64FunctionUnwind{0U, 32U, 0U, {}});
                spec.function_unwind.push_back(Pe64FunctionUnwind{16U, 48U, 0U, {}});
                n29_expect_failure(spec, "XPE0036"); break;
            case 24:
                spec.text.resize(8U, 0x90U);
                spec.function_unwind.push_back(Pe64FunctionUnwind{0U, spec.text.size(), 4U,
                    {Pe64UnwindCode{0U, Pe64UnwindOperation::AllocSmall, 40U}}});
                n29_expect_failure(spec, "XPE0033"); break;
            case 25:
                spec.text.resize(8U, 0x90U);
                spec.function_unwind.push_back(Pe64FunctionUnwind{0U, spec.text.size(), 4U,
                    {Pe64UnwindCode{4U, static_cast<Pe64UnwindOperation>(1U), 40U}}});
                n29_expect_failure(spec, "XPE0035"); break;
            case 26:
                spec.function_unwind.assign(4097U, Pe64FunctionUnwind{0U, 1U, 0U, {}});
                n29_expect_failure(spec, "XPE0030"); break;
            case 27: {
                spec.text.resize(64U, 0x90U);
                spec.function_unwind.push_back(Pe64FunctionUnwind{32U, 64U, 4U,
                    {Pe64UnwindCode{2U, Pe64UnwindOperation::PushNonVol, 3U},
                     Pe64UnwindCode{4U, Pe64UnwindOperation::AllocSmall, 40U}}});
                spec.function_unwind.push_back(Pe64FunctionUnwind{0U, 32U, 4U,
                    {Pe64UnwindCode{4U, Pe64UnwindOperation::AllocSmall, 32U}}});
                const auto result = write_pe64_executable_image(spec);
                n29_require(result.succeeded() && result.image.unwind_function_count == 2U,
                            "valid sorted multi-function unwind table must succeed");
                const auto pdata_off = n29_rva_offset(result.image, result.image.pdata_rva);
                n29_require(n29_u32(result.image.bytes, pdata_off) == 0x1000U &&
                            n29_u32(result.image.bytes, pdata_off + 12U) == 0x1020U,
                            "RUNTIME_FUNCTION entries must be sorted by BeginAddress");
                const auto xdata_off = n29_rva_offset(result.image, result.image.xdata_rva);
                n29_require(result.image.bytes[xdata_off + 12U] == 4U &&
                            result.image.bytes[xdata_off + 13U] == 0x42U &&
                            result.image.bytes[xdata_off + 14U] == 2U &&
                            result.image.bytes[xdata_off + 15U] == 0x30U,
                            "UNWIND_CODE slots must be encoded in descending CodeOffset order");
                break;
            }
            case 28: {
                spec.text.resize(5U, 0x90U);
                spec.function_unwind.push_back(Pe64FunctionUnwind{0U, 5U, 4U,
                    {Pe64UnwindCode{4U, Pe64UnwindOperation::PushNonVol, 12U}}});
                const auto result = write_pe64_executable_image(spec);
                n29_require(result.succeeded(), "R12 nonvolatile push unwind code must be accepted");
                const auto xdata_off = n29_rva_offset(result.image, result.image.xdata_rva);
                n29_require(result.image.bytes[xdata_off + 5U] == 0xc0U,
                            "UWOP_PUSH_NONVOL R12 OpInfo encoding mismatch");
                break;
            }
            default: {
                spec.text.resize(342U, 0x90U);
                for (std::size_t item = 0U; item < 342U; ++item)
                    spec.function_unwind.push_back(Pe64FunctionUnwind{item, item + 1U, 0U, {}});
                n29_expect_failure(spec, "XPE0037");
                break;
            }
            }
        }, passed, failed);
    }


}
