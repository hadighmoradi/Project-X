#include "xfrontend/elf64_shared_object.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace xfrontend;

void sh_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void sh_case(const std::string& name, const std::function<void()>& body,
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

std::uint16_t sh_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    sh_require(offset <= bytes.size() && bytes.size() - offset >= 2U, "u16 outside ELF bytes");
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t sh_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    sh_require(offset <= bytes.size() && bytes.size() - offset >= 4U, "u32 outside ELF bytes");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t sh_u64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    sh_require(offset <= bytes.size() && bytes.size() - offset >= 8U, "u64 outside ELF bytes");
    std::uint64_t value = 0U;
    for (unsigned i = 0U; i < 8U; ++i)
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (i * 8U);
    return value;
}

std::int64_t sh_i64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::int64_t>(sh_u64(bytes, offset));
}

Elf64SharedObjectSpec basic_shared_spec() {
    Elf64SharedObjectSpec spec;
    spec.soname = "libx_n26.so";
    spec.text = {0xb8U, 42U, 0U, 0U, 0U, 0xc3U}; // int answer(void) { return 42; }
    spec.exports.push_back(Elf64ExportedSymbol{
        "x_answer", Elf64SharedSection::Text, 0U, spec.text.size(), Elf64DynamicSymbolKind::Function});
    return spec;
}

bool sh_has_diag(const std::vector<Diagnostic>& diagnostics, const std::string& code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&code](const Diagnostic& item) {
        return item.code == code;
    });
}

void sh_expect_failure(const Elf64SharedObjectSpec& spec, const std::string& code) {
    const auto result = write_elf64_shared_object(spec);
    sh_require(!result.succeeded(), "invalid shared-object spec unexpectedly succeeded");
    sh_require(sh_has_diag(result.diagnostics, code), "expected diagnostic " + code + " missing");
    sh_require(result.image.bytes.empty(), "failed writer must not expose partial ELF bytes");
}

std::size_t sh_bucket_count(std::size_t count) {
    if (count < 2U) return 1U;
    std::size_t candidate = count;
    for (;;) {
        bool prime = true;
        for (std::size_t divisor = 2U; divisor <= candidate / divisor; ++divisor) {
            if (candidate % divisor == 0U) { prime = false; break; }
        }
        if (prime) return candidate;
        ++candidate;
    }
}

std::uint32_t sh_sysv_hash(const std::string& name) {
    std::uint32_t hash = 0U;
    for (unsigned char ch : name) {
        hash = (hash << 4U) + ch;
        const std::uint32_t high = hash & 0xf0000000U;
        if (high != 0U) hash ^= high >> 24U;
        hash &= ~high;
    }
    return hash;
}

std::uint64_t dynamic_value(const Elf64SharedObjectImage& image, std::int64_t tag) {
    for (const auto& entry : image.dynamic_entries) {
        if (entry.tag == tag) return entry.value;
    }
    throw std::runtime_error("dynamic tag not found");
}

} // namespace

void run_elf64_shared_object_tests(int& passed, int& failed) {
    // N26A: ELF64 ET_DYN header and program table, with page-congruent PT_LOADs.
    for (int index = 0; index < 500; ++index) {
        sh_case("N26A" + std::to_string(index + 1) + "_et_dyn_header_program_loads", [index] {
            auto spec = basic_shared_spec();
            const std::size_t extra = static_cast<std::size_t>(index % 127);
            spec.text.insert(spec.text.end(), extra, static_cast<std::uint8_t>(index & 0xff));
            spec.page_size = (index % 2 == 0) ? 4096U : 8192U;
            const auto result = write_elf64_shared_object(spec);
            sh_require(result.succeeded(), "valid ET_DYN shared object should be emitted");
            const auto& bytes = result.image.bytes;
            sh_require(bytes[0] == 0x7fU && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F',
                       "ELF magic mismatch");
            sh_require(bytes[4] == 2U && bytes[5] == 1U && bytes[6] == 1U, "ELF64 little-endian identity mismatch");
            sh_require(sh_u16(bytes, 16U) == 3U && sh_u16(bytes, 18U) == 62U, "ET_DYN / EM_X86_64 mismatch");
            sh_require(sh_u32(bytes, 20U) == 1U && sh_u64(bytes, 24U) == 0U, "ELF shared-object version/entry mismatch");
            sh_require(sh_u64(bytes, 32U) == 64U && sh_u64(bytes, 40U) == 0U, "ELF program/section offsets mismatch");
            sh_require(sh_u16(bytes, 52U) == 64U && sh_u16(bytes, 54U) == 56U && sh_u16(bytes, 56U) == 5U,
                       "ELF header/program-header sizes mismatch");
            for (std::size_t ph_index = 0U; ph_index < 3U; ++ph_index) {
                const std::size_t ph = 64U + ph_index * 56U;
                sh_require(sh_u32(bytes, ph) == 1U, "first three headers must be PT_LOAD");
                const auto file_offset = sh_u64(bytes, ph + 8U);
                const auto virtual_address = sh_u64(bytes, ph + 16U);
                const auto alignment = sh_u64(bytes, ph + 48U);
                sh_require((file_offset % alignment) == (virtual_address % alignment), "PT_LOAD offset/vaddr are incongruent");
                sh_require(alignment == spec.page_size, "PT_LOAD alignment mismatch");
            }
            sh_require(sh_u32(bytes, 64U + 4U) == 5U, "text PT_LOAD must be PF_R|PF_X");
            sh_require(sh_u32(bytes, 64U + 56U + 4U) == 4U, "metadata PT_LOAD must be PF_R");
            sh_require(sh_u32(bytes, 64U + 112U + 4U) == 6U, "data PT_LOAD must be PF_R|PF_W");
            sh_require(result.image.text_address < sh_u64(bytes, 64U + 32U), "text must lie inside the first executable PT_LOAD");
            sh_require(bytes[static_cast<std::size_t>(result.image.text_address)] == spec.text.front(), "text payload offset mismatch");
            sh_require(result.image.read_only_data_address == sh_u64(bytes, 64U + 56U + 16U), "RO VA metadata mismatch");
            sh_require(result.image.data_address == sh_u64(bytes, 64U + 112U + 16U), "RW VA metadata mismatch");
        }, passed, failed);
    }

    // N26B: dynamic table, SONAME, deterministic dynsym/dynstr and SysV hash.
    for (int index = 0; index < 500; ++index) {
        sh_case("N26B" + std::to_string(index + 1) + "_dynamic_symbols_hash_table", [index] {
            auto spec = basic_shared_spec();
            const std::size_t count = static_cast<std::size_t>(index % 8);
            spec.text.resize(512U, 0x90U);
            spec.exports.clear();
            for (std::size_t i = 0U; i < count; ++i) {
                const std::string name = "x_export_" + std::to_string((count - i) * 13U + i);
                spec.exports.push_back(Elf64ExportedSymbol{
                    name, Elf64SharedSection::Text, i, 1U, Elf64DynamicSymbolKind::Function});
            }
            const auto result = write_elf64_shared_object(spec);
            sh_require(result.succeeded(), "shared object with dynamic exports should be emitted");
            sh_require(result.image.exported_symbol_count == count, "export count mismatch");
            sh_require(result.image.dynamic_entries.size() == 10U, "dynamic tag count mismatch");
            sh_require(result.image.dynamic_entries.back().tag == 0 && result.image.dynamic_entries.back().value == 0U,
                       "DT_NULL must terminate the dynamic table");
            const auto& bytes = result.image.bytes;
            const auto dynamic_ph = 64U + 3U * 56U;
            sh_require(sh_u32(bytes, dynamic_ph) == 2U, "PT_DYNAMIC missing");
            sh_require(sh_u64(bytes, dynamic_ph + 16U) == result.image.dynamic_address, "PT_DYNAMIC address mismatch");
            const auto hash_address = dynamic_value(result.image, 4);
            const auto symtab_address = dynamic_value(result.image, 6);
            const auto strtab_address = dynamic_value(result.image, 5);
            const auto hash_file_offset = hash_address;
            const auto symtab_file_offset = symtab_address;
            const auto strtab_file_offset = strtab_address;
            const std::size_t bucket_count = sh_bucket_count(count);
            sh_require(sh_u32(bytes, static_cast<std::size_t>(hash_file_offset)) == bucket_count, "SysV hash bucket count mismatch");
            sh_require(sh_u32(bytes, static_cast<std::size_t>(hash_file_offset + 4U)) == count + 1U,
                       "SysV hash chain count mismatch");
            std::vector<bool> visited(count + 1U, false);
            for (std::size_t i = 0U; i < count; ++i) {
                const std::size_t symbol_offset = static_cast<std::size_t>(symtab_file_offset + (i + 1U) * 24U);
                const auto name_offset = sh_u32(bytes, symbol_offset);
                sh_require(bytes[symbol_offset + 4U] == 0x12U, "exported symbol must be STB_GLOBAL|STT_FUNC");
                sh_require(sh_u16(bytes, symbol_offset + 6U) == 1U, "function symbol section index mismatch");
                const auto name_pos = static_cast<std::size_t>(strtab_file_offset + name_offset);
                sh_require(name_pos < bytes.size(), "dynstr name offset outside ELF file");
                std::string name;
                for (std::size_t pos = name_pos; pos < bytes.size() && bytes[pos] != 0U; ++pos)
                    name.push_back(static_cast<char>(bytes[pos]));
                sh_require(!name.empty() && name.rfind("x_export_", 0U) == 0U, "dynamic string entry mismatch");
                const std::size_t bucket_index = sh_sysv_hash(name) % bucket_count;
                const auto bucket_head = sh_u32(bytes, static_cast<std::size_t>(hash_file_offset + 8U + bucket_index * 4U));
                sh_require(bucket_head != 0U, "SysV hash bucket must contain its exported symbol");
                std::uint32_t chain_index = bucket_head;
                bool found = false;
                for (std::size_t steps = 0U; chain_index != 0U && steps <= count; ++steps) {
                    sh_require(chain_index <= count, "SysV hash chain index outside dynsym");
                    if (chain_index == i + 1U) found = true;
                    const std::size_t chain_offset = static_cast<std::size_t>(hash_file_offset + 8U + bucket_count * 4U + chain_index * 4U);
                    chain_index = sh_u32(bytes, chain_offset);
                }
                sh_require(found, "SysV hash bucket chain does not reach exported symbol");
                visited[i + 1U] = true;
            }
            for (std::size_t i = 1U; i <= count; ++i) sh_require(visited[i], "every dynsym export must be visited");
            sh_require(dynamic_value(result.image, 10) > 0U, "DT_STRSZ must include NUL and SONAME");
            sh_require(dynamic_value(result.image, 11) == 24U, "DT_SYMENT must be 24 bytes");
            sh_require(dynamic_value(result.image, 14) > 0U, "DT_SONAME offset must be nonzero");
        }, passed, failed);
    }

    // N26C: R_X86_64_RELATIVE RELA entries, RW targets, BSS targets and addend mapping.
    for (int index = 0; index < 500; ++index) {
        sh_case("N26C" + std::to_string(index + 1) + "_relative_rela_serialization", [index] {
            auto spec = basic_shared_spec();
            spec.data.assign(64U, 0U);
            spec.bss_size = 64U;
            const std::size_t count = static_cast<std::size_t>((index % 4) + 1);
            for (std::size_t i = 0U; i < count; ++i) {
                const bool target_bss = ((index + static_cast<int>(i)) % 2) != 0;
                const std::size_t target = target_bss ? 64U + i * 8U : i * 8U;
                spec.relative_relocations.push_back(Elf64RelativeRelocation{
                    target, Elf64SharedSection::Text, static_cast<std::size_t>((index + static_cast<int>(i)) % 6)});
            }
            const auto result = write_elf64_shared_object(spec);
            sh_require(result.succeeded(), "valid relative relocations should be serialized");
            sh_require(result.image.relative_relocation_count == count, "relative relocation count mismatch");
            const auto rela = dynamic_value(result.image, 7);
            sh_require(dynamic_value(result.image, 8) == count * 24U, "DT_RELASZ mismatch");
            sh_require(dynamic_value(result.image, 9) == 24U, "DT_RELAENT mismatch");
            auto expected_relocations = spec.relative_relocations;
            std::sort(expected_relocations.begin(), expected_relocations.end(), [](const auto& left, const auto& right) {
                return left.target_offset < right.target_offset;
            });
            for (std::size_t i = 0U; i < count; ++i) {
                const std::size_t offset = static_cast<std::size_t>(rela + i * 24U);
                const auto target = sh_u64(result.image.bytes, offset);
                const auto info = sh_u64(result.image.bytes, offset + 8U);
                const auto addend = sh_i64(result.image.bytes, offset + 16U);
                sh_require((target & 7U) == 0U, "RELA target must be pointer aligned");
                sh_require(target >= result.image.data_address && target + 8U <= result.image.memory_size,
                           "RELA target must lie inside RW data/BSS");
                sh_require(info == 8U, "r_info must encode symbol zero and R_X86_64_RELATIVE");
                sh_require(addend == static_cast<std::int64_t>(result.image.text_address +
                    static_cast<std::uint64_t>(expected_relocations[i].addend_value)), "RELA addend mismatch");
            }
            const auto data_ph = 64U + 2U * 56U;
            sh_require(sh_u64(result.image.bytes, data_ph + 32U) == spec.data.size(), "RW p_filesz mismatch");
            sh_require(sh_u64(result.image.bytes, data_ph + 40U) == spec.data.size() + spec.bss_size,
                       "RW p_memsz must include BSS");
        }, passed, failed);
    }

    // N26D: malformed specs, unsupported features, overflow and fail-closed diagnostics.
    for (int index = 0; index < 500; ++index) {
        sh_case("N26D" + std::to_string(index + 1) + "_shared_object_rejection_matrix", [index] {
            auto spec = basic_shared_spec();
            switch (index % 12) {
            case 0: spec.soname.clear(); sh_expect_failure(spec, "XESHO0001"); break;
            case 1: spec.text.clear(); sh_expect_failure(spec, "XESHO0002"); break;
            case 2: spec.page_size = 3000U; sh_expect_failure(spec, "XESHO0003"); break;
            case 3: spec.maximum_file_size = 64U; sh_expect_failure(spec, "XESHO0004"); break;
            case 4: spec.exports.push_back(spec.exports.front()); sh_expect_failure(spec, "XESHO0006"); break;
            case 5: spec.exports.front().name.clear(); sh_expect_failure(spec, "XESHO0006"); break;
            case 6: spec.exports.front().value = spec.text.size(); sh_expect_failure(spec, "XESHO0007"); break;
            case 7: spec.exports.front().kind = Elf64DynamicSymbolKind::Object; sh_expect_failure(spec, "XESHO0008"); break;
            case 8: spec.data.assign(16U, 0U); spec.relative_relocations.push_back(
                        Elf64RelativeRelocation{1U, Elf64SharedSection::Text, 0U});
                    sh_expect_failure(spec, "XESHO0012"); break;
            case 9: spec.data.assign(8U, 0U); spec.relative_relocations.push_back(
                        Elf64RelativeRelocation{8U, Elf64SharedSection::Text, 0U});
                    sh_expect_failure(spec, "XESHO0012"); break;
            case 10: spec.data.assign(8U, 0U); spec.relative_relocations.push_back(
                         Elf64RelativeRelocation{0U, Elf64SharedSection::ReadOnlyData, 0U});
                     sh_expect_failure(spec, "XESHO0013"); break;
            default: spec.bss_size = static_cast<std::size_t>(-1);
                     sh_expect_failure(spec, "XESHO0010"); break;
            }
        }, passed, failed);
    }
}
