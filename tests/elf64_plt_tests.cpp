#include "xfrontend/elf64_shared_object.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace xfrontend;

void n28_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void n28_case(const std::string& name, const std::function<void()>& body,
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

std::uint32_t n28_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n28_require(offset <= bytes.size() && bytes.size() - offset >= 4U, "u32 outside ELF bytes");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t n28_u64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n28_require(offset <= bytes.size() && bytes.size() - offset >= 8U, "u64 outside ELF bytes");
    std::uint64_t value = 0U;
    for (unsigned i = 0U; i < 8U; ++i)
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8U);
    return value;
}

std::int32_t n28_i32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::int32_t>(n28_u32(bytes, offset));
}

std::uint64_t n28_tag(const Elf64SharedObjectImage& image, std::int64_t tag) {
    for (const auto& entry : image.dynamic_entries) if (entry.tag == tag) return entry.value;
    throw std::runtime_error("required dynamic tag is missing");
}

bool n28_has_diag(const std::vector<Diagnostic>& diagnostics, const std::string& code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&code](const Diagnostic& item) {
        return item.code == code;
    });
}

void n28_expect_failure(const Elf64SharedObjectSpec& spec, const std::string& code) {
    const auto result = write_elf64_shared_object(spec);
    n28_require(!result.succeeded(), "invalid PLT specification unexpectedly succeeded");
    n28_require(n28_has_diag(result.diagnostics, code), "expected diagnostic " + code + " missing");
    n28_require(result.image.bytes.empty(), "failed PLT writer must not expose a partial image");
}

Elf64SharedObjectSpec n28_spec() {
    Elf64SharedObjectSpec spec;
    spec.soname = "libx_n28_plt_smoke.so";
    spec.text = {0xb8U, 42U, 0U, 0U, 0U, 0xc3U};
    spec.exports.push_back(Elf64ExportedSymbol{
        "x_n28_answer", Elf64SharedSection::Text, 0U, spec.text.size(), Elf64DynamicSymbolKind::Function});
    return spec;
}

} // namespace

void run_elf64_plt_tests(int& passed, int& failed) {
    // N28A: PLT0/PLTn machine-code layout and CALL rel32 patches.
    for (int index = 0; index < 500; ++index) {
        n28_case("N28A" + std::to_string(index + 1) + "_plt_stub_and_call_patch", [index] {
            auto spec = n28_spec();
            const std::size_t count = static_cast<std::size_t>(1 + index % 6);
            spec.text.clear();
            for (std::size_t i = 0U; i < count; ++i) {
                const std::size_t call_offset = spec.text.size();
                spec.text.insert(spec.text.end(), {0xe8U, 0U, 0U, 0U, 0U, 0xc3U});
                const std::string name = "plt_api_" + std::to_string(count - i);
                spec.imports.push_back(Elf64ImportedSymbol{name, Elf64DynamicSymbolKind::Function, Elf64ImportBinding::Plt});
                spec.plt_pc_relative32.push_back(Elf64PltPcRelative32{call_offset + 1U, name});
            }
            spec.needed_libraries = {"libc.so.6"};
            spec.exports.clear();
            spec.exports.push_back(Elf64ExportedSymbol{
                "x_n28_calls", Elf64SharedSection::Text, 0U, spec.text.size(), Elf64DynamicSymbolKind::Function});

            const auto result = write_elf64_shared_object(spec);
            n28_require(result.succeeded(), "valid PLT writer input must succeed");
            n28_require(result.image.plt_relocation_count == count, "PLT relocation count mismatch");
            n28_require(result.image.plt_size == (count + 1U) * 16U, "PLT code size must include PLT0 and one 16-byte entry per function");
            n28_require((result.image.plt_address & 15U) == 0U, "PLT must be 16-byte aligned");
            const auto& bytes = result.image.bytes;
            const std::size_t plt = static_cast<std::size_t>(result.image.plt_address);
            n28_require(bytes[plt] == 0xffU && bytes[plt + 1U] == 0x35U,
                        "PLT0 must push GOTPLT[1] using RIP-relative addressing");
            n28_require(bytes[plt + 6U] == 0xffU && bytes[plt + 7U] == 0x25U,
                        "PLT0 must indirect-jump through GOTPLT[2]");
            n28_require(bytes[plt + 12U] == 0x0fU && bytes[plt + 13U] == 0x1fU &&
                        bytes[plt + 14U] == 0x40U && bytes[plt + 15U] == 0U,
                        "PLT0 trailing NOP encoding mismatch");
            const std::uint64_t gotplt = result.image.gotplt_address;
            n28_require(static_cast<std::int64_t>(plt + 6U) + n28_i32(bytes, plt + 2U) ==
                        static_cast<std::int64_t>(gotplt + 8U), "PLT0 push must address GOTPLT[1]");
            n28_require(static_cast<std::int64_t>(plt + 12U) + n28_i32(bytes, plt + 8U) ==
                        static_cast<std::int64_t>(gotplt + 16U), "PLT0 jump must address GOTPLT[2]");

            std::vector<std::string> sorted_names;
            for (const auto& item : spec.imports) sorted_names.push_back(item.name);
            std::sort(sorted_names.begin(), sorted_names.end());
            for (std::size_t i = 0U; i < count; ++i) {
                const std::size_t entry = plt + (i + 1U) * 16U;
                const std::uint64_t slot = gotplt + 24U + i * 8U;
                n28_require(n28_u64(bytes, static_cast<std::size_t>(slot)) == entry + 6U,
                            "initial GOTPLT slot must point at PLTn+6 for lazy binding");
                n28_require(bytes[entry] == 0xffU && bytes[entry + 1U] == 0x25U,
                            "PLT entry must begin with an indirect GOTPLT jump");
                n28_require(static_cast<std::int64_t>(entry + 6U) + n28_i32(bytes, entry + 2U) ==
                            static_cast<std::int64_t>(slot), "PLT entry RIP displacement must point to its GOTPLT slot");
                n28_require(bytes[entry + 6U] == 0x68U && n28_u32(bytes, entry + 7U) == i,
                            "PLT entry must push its .rela.plt ordinal");
                n28_require(bytes[entry + 11U] == 0xe9U &&
                            static_cast<std::int64_t>(entry + 16U) + n28_i32(bytes, entry + 12U) ==
                            static_cast<std::int64_t>(plt), "PLT entry must branch back to PLT0");
            }
            for (const auto& reference : spec.plt_pc_relative32) {
                const auto found = std::find(sorted_names.begin(), sorted_names.end(), reference.symbol_name);
                n28_require(found != sorted_names.end(), "test call target should exist in sorted imports");
                const std::size_t stub_index = static_cast<std::size_t>(found - sorted_names.begin());
                const std::uint64_t stub = result.image.plt_address + (stub_index + 1U) * 16U;
                const std::uint64_t place = result.image.text_address + reference.text_displacement_offset;
                n28_require(static_cast<std::int64_t>(place + 4U) +
                            n28_i32(bytes, static_cast<std::size_t>(place)) == static_cast<std::int64_t>(stub),
                            "near CALL rel32 must point at the matching PLT entry");
            }
        }, passed, failed);
    }

    // N28B: DT_PLTGOT/DT_JMPREL/DT_PLTREL and R_X86_64_JUMP_SLOT records.
    for (int index = 0; index < 500; ++index) {
        n28_case("N28B" + std::to_string(index + 1) + "_plt_dynamic_tags_and_jump_slot", [index] {
            auto spec = n28_spec();
            const std::size_t plt_count = static_cast<std::size_t>(1 + index % 5);
            const std::size_t got_count = static_cast<std::size_t>(index % 4);
            spec.needed_libraries = {"libc.so.6"};
            for (std::size_t i = 0U; i < plt_count; ++i)
                spec.imports.push_back(Elf64ImportedSymbol{"z_plt_" + std::to_string(i),
                    Elf64DynamicSymbolKind::Function, Elf64ImportBinding::Plt});
            for (std::size_t i = 0U; i < got_count; ++i)
                spec.imports.push_back(Elf64ImportedSymbol{"a_got_" + std::to_string(i),
                    i % 2U == 0U ? Elf64DynamicSymbolKind::Object : Elf64DynamicSymbolKind::Function,
                    Elf64ImportBinding::Got});
            spec.text = {0xe8U, 0U, 0U, 0U, 0U, 0xc3U};
            spec.plt_pc_relative32 = {Elf64PltPcRelative32{1U, "z_plt_0"}};
            const auto result = write_elf64_shared_object(spec);
            n28_require(result.succeeded(), "mixed GOT and PLT imports should serialize");
            n28_require(n28_tag(result.image, 3) == result.image.gotplt_address, "DT_PLTGOT must point to GOTPLT");
            n28_require(n28_tag(result.image, 2) == plt_count * 24U, "DT_PLTRELSZ must match .rela.plt bytes");
            n28_require(n28_tag(result.image, 20) == 7U, "DT_PLTREL must identify RELA");
            n28_require(n28_tag(result.image, 23) == result.image.plt_relocation_address,
                        "DT_JMPREL must point to the PLT relocation table");
            n28_require(result.image.plt_relocation_count == plt_count, "PLT relocation count mismatch");
            n28_require(result.image.symbol_relocation_count == got_count,
                        "only GOT-bound imports may create GLOB_DAT relocations");
            const auto& bytes = result.image.bytes;
            const std::size_t jmprel = static_cast<std::size_t>(n28_tag(result.image, 23));
            std::vector<Elf64ImportedSymbol> sorted = spec.imports;
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
            std::size_t plt_index = 0U;
            std::size_t glob_count = 0U;
            const std::size_t rela = static_cast<std::size_t>(n28_tag(result.image, 7));
            const std::size_t rela_size = static_cast<std::size_t>(n28_tag(result.image, 8));
            for (std::size_t i = 0U; i < sorted.size(); ++i) {
                const std::size_t symbol_index = i + 2U; // dynsym[0] is reserved; one exported symbol precedes imports
                if (sorted[i].binding == Elf64ImportBinding::Plt) {
                    const std::size_t entry = jmprel + plt_index * 24U;
                    n28_require(n28_u64(bytes, entry) == result.image.gotplt_address + 24U + plt_index * 8U,
                                "JUMP_SLOT r_offset must target matching GOTPLT slot");
                    n28_require(n28_u64(bytes, entry + 8U) ==
                                ((static_cast<std::uint64_t>(symbol_index) << 32U) | 7U),
                                "JUMP_SLOT r_info must encode the imported dynsym index and type 7");
                    n28_require(n28_u64(bytes, entry + 16U) == 0U, "JUMP_SLOT addend must be zero");
                    ++plt_index;
                } else {
                    ++glob_count;
                }
            }
            n28_require(plt_index == plt_count && glob_count == got_count, "mixed binding enumeration mismatch");
            n28_require(rela_size == (got_count + 1U) * 24U,
                        "dynamic RELA must contain GOT GLOB_DAT entries and GOTPLT[0] RELATIVE");
            bool gotplt_zero_relocation = false;
            for (std::size_t offset = rela; offset < rela + rela_size; offset += 24U) {
                if (n28_u64(bytes, offset) == result.image.gotplt_address) {
                    gotplt_zero_relocation = n28_u64(bytes, offset + 8U) == 8U &&
                        n28_u64(bytes, offset + 16U) == result.image.dynamic_address;
                }
            }
            n28_require(gotplt_zero_relocation, "GOTPLT[0] must receive an image-relative PT_DYNAMIC pointer");
        }, passed, failed);
    }

    // N28C: mixed binding, GOTPLT reservation, file/memory sizes, BSS and W^X.
    for (int index = 0; index < 500; ++index) {
        n28_case("N28C" + std::to_string(index + 1) + "_mixed_gotplt_bss_layout", [index] {
            auto spec = n28_spec();
            spec.data.assign(static_cast<std::size_t>(1 + index % 31), 0xa5U);
            spec.bss_size = static_cast<std::size_t>(8 + (index % 9) * 8);
            const std::size_t got_count = static_cast<std::size_t>(index % 4);
            const std::size_t plt_count = static_cast<std::size_t>(1 + index % 4);
            spec.needed_libraries = {"libc.so.6"};
            for (std::size_t i = 0U; i < got_count; ++i)
                spec.imports.push_back(Elf64ImportedSymbol{"got_" + std::to_string(i),
                    Elf64DynamicSymbolKind::Object, Elf64ImportBinding::Got});
            for (std::size_t i = 0U; i < plt_count; ++i)
                spec.imports.push_back(Elf64ImportedSymbol{"plt_" + std::to_string(i),
                    Elf64DynamicSymbolKind::Function, Elf64ImportBinding::Plt});
            spec.relative_relocations.push_back(Elf64RelativeRelocation{
                spec.data.size(), Elf64SharedSection::Text, 0U});
            spec.text = {0xe8U, 0U, 0U, 0U, 0U, 0xc3U};
            spec.plt_pc_relative32 = {Elf64PltPcRelative32{1U, "plt_0"}};
            const auto result = write_elf64_shared_object(spec);
            n28_require(result.succeeded(), "mixed GOT/PLT with BSS should succeed");
            n28_require(result.image.got_size == got_count * 8U, "regular GOT must exclude PLT imports");
            n28_require(result.image.gotplt_size == 24U + plt_count * 8U, "GOTPLT must have three reserved entries and one slot per PLT import");
            n28_require((result.image.gotplt_address & 7U) == 0U, "GOTPLT alignment mismatch");
            n28_require(result.image.gotplt_address >= result.image.got_address + result.image.got_size,
                        "GOTPLT must follow the regular GOT");
            const std::size_t file_size = static_cast<std::size_t>(n28_u64(result.image.bytes, 64U + 2U * 56U + 32U));
            const std::size_t memory_size = static_cast<std::size_t>(n28_u64(result.image.bytes, 64U + 2U * 56U + 40U));
            n28_require(memory_size == file_size + spec.bss_size, "RW p_memsz must extend p_filesz by BSS size");
            n28_require(file_size >= spec.data.size() + result.image.got_size + result.image.gotplt_size,
                        "RW p_filesz must cover user data, GOT and GOTPLT");
            n28_require(result.image.memory_size == result.image.data_address + memory_size,
                        "image memory size endpoint mismatch");
            const std::size_t rela = static_cast<std::size_t>(n28_tag(result.image, 7));
            const std::size_t rela_size = static_cast<std::size_t>(n28_tag(result.image, 8));
            n28_require(rela_size == (1U + got_count + 1U) * 24U,
                        "RELATIVE, GLOB_DAT and GOTPLT initialization relocation count mismatch");
            bool user_bss_relocation_found = false;
            for (std::size_t offset = rela; offset < rela + rela_size; offset += 24U) {
                const std::uint64_t target = n28_u64(result.image.bytes, offset);
                const std::uint64_t type = n28_u64(result.image.bytes, offset + 8U) & 0xffffffffU;
                if (target >= result.image.data_address + file_size && type == 8U) user_bss_relocation_found = true;
            }
            n28_require(user_bss_relocation_found, "explicit BSS relocation must remain after GOTPLT memory reservation");
            const std::uint32_t rx_flags = n28_u32(result.image.bytes, 64U + 4U);
            const std::uint32_t rw_flags = n28_u32(result.image.bytes, 64U + 2U * 56U + 4U);
            n28_require((rx_flags & 3U) == 1U && (rw_flags & 3U) == 2U,
                        "executable PLT and writable GOTPLT must remain in distinct RX/RW segments");
        }, passed, failed);
    }

    // N28D: fail-closed binding, call-site and size validation.
    for (int index = 0; index < 500; ++index) {
        n28_case("N28D" + std::to_string(index + 1) + "_plt_rejection_matrix", [index] {
            auto spec = n28_spec();
            spec.text = {0xe8U, 0U, 0U, 0U, 0U, 0xc3U};
            spec.imports = {Elf64ImportedSymbol{"getpid", Elf64DynamicSymbolKind::Function, Elf64ImportBinding::Plt}};
            spec.needed_libraries = {"libc.so.6"};
            spec.plt_pc_relative32 = {Elf64PltPcRelative32{1U, "getpid"}};
            switch (index % 12) {
            case 0:
                spec.imports.front().kind = Elf64DynamicSymbolKind::Object;
                n28_expect_failure(spec, "XESHO0024"); break;
            case 1:
                spec.imports.front().binding = static_cast<Elf64ImportBinding>(99U);
                n28_expect_failure(spec, "XESHO0023"); break;
            case 2:
                spec.plt_pc_relative32.front().symbol_name = "not_imported";
                n28_expect_failure(spec, "XESHO0027"); break;
            case 3:
                spec.plt_pc_relative32.front().text_displacement_offset = 2U;
                n28_expect_failure(spec, "XESHO0028"); break;
            case 4:
                spec.plt_pc_relative32.push_back(Elf64PltPcRelative32{1U, "getpid"});
                n28_expect_failure(spec, "XESHO0029"); break;
            case 5:
                spec.plt_pc_relative32.front().text_displacement_offset = spec.text.size() - 2U;
                n28_expect_failure(spec, "XESHO0028"); break;
            case 6:
                spec.got_pc_relative32 = {Elf64GotPcRelative32{1U, "getpid"}};
                n28_expect_failure(spec, "XESHO0031"); break;
            case 7:
                spec.needed_libraries.clear();
                n28_expect_failure(spec, "XESHO0015"); break;
            case 8:
                spec.maximum_file_size = 64U + 5U * 56U + 32U;
                n28_expect_failure(spec, "XESHO0011"); break;
            case 9:
                spec.imports.push_back(spec.imports.front());
                n28_expect_failure(spec, "XESHO0016"); break;
            case 10:
                spec.page_size = 3072U;
                n28_expect_failure(spec, "XESHO0003"); break;
            default:
                spec.soname = std::string("bad\0soname", 10U);
                n28_expect_failure(spec, "XESHO0001"); break;
            }
        }, passed, failed);
    }
}
