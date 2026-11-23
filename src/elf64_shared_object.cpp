#include "xfrontend/elf64_shared_object.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace xfrontend {
namespace {

constexpr std::size_t HeaderSize = 64U;
constexpr std::size_t ProgramHeaderSize = 56U;
constexpr std::size_t ProgramHeaderCount = 5U;
constexpr std::size_t DynamicEntrySize = 16U;
constexpr std::size_t SymbolEntrySize = 24U;
constexpr std::size_t RelaEntrySize = 24U;
constexpr std::uint16_t ElfTypeShared = 3U;
constexpr std::uint16_t ElfMachineX86_64 = 62U;
constexpr std::uint32_t ElfVersionCurrent = 1U;
constexpr std::uint32_t PtLoad = 1U;
constexpr std::uint32_t PtDynamic = 2U;
constexpr std::uint32_t PtGnuStack = 0x6474e551U;
constexpr std::uint32_t PfExecute = 1U;
constexpr std::uint32_t PfWrite = 2U;
constexpr std::uint32_t PfRead = 4U;
constexpr std::uint32_t RelativeRelocationType = 8U; // R_X86_64_RELATIVE
constexpr std::uint32_t GlobalDataRelocationType = 6U; // R_X86_64_GLOB_DAT
constexpr std::uint32_t JumpSlotRelocationType = 7U; // R_X86_64_JUMP_SLOT
constexpr std::int64_t DtNull = 0;
constexpr std::int64_t DtNeeded = 1;
constexpr std::int64_t DtPltRelSize = 2;
constexpr std::int64_t DtPltGot = 3;
constexpr std::int64_t DtHash = 4;
constexpr std::int64_t DtStrtab = 5;
constexpr std::int64_t DtSymtab = 6;
constexpr std::int64_t DtRela = 7;
constexpr std::int64_t DtRelaSize = 8;
constexpr std::int64_t DtRelaEntrySize = 9;
constexpr std::int64_t DtStrSize = 10;
constexpr std::int64_t DtSymEntrySize = 11;
constexpr std::int64_t DtSoname = 14;
constexpr std::int64_t DtPltRel = 20;
constexpr std::int64_t DtJmpRel = 23;

struct SymbolLayout {
    Elf64ExportedSymbol symbol;
    std::uint32_t name_offset = 0U;
    std::uint64_t address = 0U;
};

struct ImportLayout {
    Elf64ImportedSymbol symbol;
    std::uint32_t name_offset = 0U;
    std::uint32_t symbol_index = 0U;
    std::size_t got_offset = 0U;
    std::size_t plt_index = 0U;
};

struct SerializedRelocation {
    std::uint64_t target = 0U;
    std::uint64_t info = 0U;
    std::int64_t addend = 0;
};

void diagnostic(std::vector<Diagnostic>& out, const char* code, const std::string& message) {
    out.push_back(Diagnostic{code, message, {}});
}

bool power_of_two(std::size_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) {
    if (a > std::numeric_limits<std::uint64_t>::max() - b) return false;
    out = a + b;
    return true;
}

bool align_up(std::uint64_t value, std::size_t alignment, std::uint64_t& out) {
    if (!power_of_two(alignment)) return false;
    const std::uint64_t mask = static_cast<std::uint64_t>(alignment - 1U);
    if (value > std::numeric_limits<std::uint64_t>::max() - mask) return false;
    out = (value + mask) & ~mask;
    return true;
}

void write_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void write_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0U; i < 4U; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
}

void write_u64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned i = 0U; i < 8U; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
}

void write_i64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::int64_t value) {
    write_u64(bytes, offset, static_cast<std::uint64_t>(value));
}

void write_i32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::int32_t value) {
    write_u32(bytes, offset, static_cast<std::uint32_t>(value));
}



std::uint32_t sysv_hash(const std::string& name) {
    std::uint32_t hash = 0U;
    for (unsigned char ch : name) {
        hash = (hash << 4U) + ch;
        const std::uint32_t high = hash & 0xf0000000U;
        if (high != 0U) hash ^= high >> 24U;
        hash &= ~high;
    }
    return hash;
}

bool is_prime(std::size_t value) {
    if (value < 2U) return false;
    if ((value % 2U) == 0U) return value == 2U;
    for (std::size_t divisor = 3U; divisor <= value / divisor; divisor += 2U) {
        if ((value % divisor) == 0U) return false;
    }
    return true;
}

std::size_t choose_bucket_count(std::size_t symbol_count) {
    std::size_t candidate = symbol_count < 2U ? 1U : symbol_count;
    while (candidate > 1U && !is_prime(candidate)) ++candidate;
    return candidate;
}

bool valid_name(const std::string& name) {
    return !name.empty() && name.find('\0') == std::string::npos;
}

bool valid_soname(const std::string& name) {
    return valid_name(name) && name.find('/') == std::string::npos &&
        name.find('\\') == std::string::npos;
}

bool address_for_section(Elf64SharedSection section, std::size_t value,
                         std::uint64_t text_address, std::size_t text_size,
                         std::uint64_t ro_address, std::size_t ro_size,
                         std::uint64_t data_address, std::size_t data_mem_size,
                         std::uint64_t& address) {
    std::uint64_t base = 0U;
    std::size_t size = 0U;
    switch (section) {
    case Elf64SharedSection::Text: base = text_address; size = text_size; break;
    case Elf64SharedSection::ReadOnlyData: base = ro_address; size = ro_size; break;
    case Elf64SharedSection::Data: base = data_address; size = data_mem_size; break;
    default: return false;
    }
    if (value >= size || !checked_add(base, static_cast<std::uint64_t>(value), address)) return false;
    return true;
}

void write_program_header(std::vector<std::uint8_t>& bytes, std::size_t offset,
                          std::uint32_t type, std::uint32_t flags,
                          std::uint64_t file_offset, std::uint64_t virtual_address,
                          std::uint64_t file_size, std::uint64_t memory_size,
                          std::uint64_t alignment) {
    write_u32(bytes, offset, type);
    write_u32(bytes, offset + 4U, flags);
    write_u64(bytes, offset + 8U, file_offset);
    write_u64(bytes, offset + 16U, virtual_address);
    write_u64(bytes, offset + 24U, virtual_address);
    write_u64(bytes, offset + 32U, file_size);
    write_u64(bytes, offset + 40U, memory_size);
    write_u64(bytes, offset + 48U, alignment);
}

} // namespace

Elf64SharedObjectResult write_elf64_shared_object(const Elf64SharedObjectSpec& spec) {
    Elf64SharedObjectResult result;
    result.image.descriptor = make_elf_x86_64_object_target();

    if (!valid_name(spec.soname)) {
        diagnostic(result.diagnostics, "XESHO0001", "shared-object SONAME must be non-empty and contain no embedded NUL");
        return result;
    }
    if (spec.text.empty()) {
        diagnostic(result.diagnostics, "XESHO0002", "shared object requires a non-empty executable text segment");
        return result;
    }
    if (!power_of_two(spec.page_size) || spec.page_size < 4096U || spec.page_size > 1024U * 1024U) {
        diagnostic(result.diagnostics, "XESHO0003", "ELF page size must be a power of two from 4096 through 1048576 bytes");
        return result;
    }
    if (spec.maximum_file_size < HeaderSize + ProgramHeaderCount * ProgramHeaderSize) {
        diagnostic(result.diagnostics, "XESHO0004", "maximum file size is smaller than ELF header and program-header table");
        return result;
    }
    if (spec.exports.size() + spec.imports.size() > 65534U ||
        spec.relative_relocations.size() + spec.imports.size() > 65535U ||
        spec.needed_libraries.size() > 4096U || spec.plt_pc_relative32.size() > 65535U) {
        diagnostic(result.diagnostics, "XESHO0005", "dynamic symbol, relocation or dependency count exceeds the supported ELF64 writer limit");
        return result;
    }

    if (spec.bss_size > std::numeric_limits<std::size_t>::max() - spec.data.size()) {
        diagnostic(result.diagnostics, "XESHO0010", "data plus BSS size overflows size_t");
        return result;
    }

    std::vector<Elf64ExportedSymbol> exports = spec.exports;
    std::sort(exports.begin(), exports.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    std::vector<Elf64ImportedSymbol> imports = spec.imports;
    std::sort(imports.begin(), imports.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    std::vector<std::string> needed_libraries = spec.needed_libraries;
    std::sort(needed_libraries.begin(), needed_libraries.end());
    std::set<std::string> dependency_names;
    for (const auto& dependency : needed_libraries) {
        if (!valid_soname(dependency) || !dependency_names.insert(dependency).second) {
            diagnostic(result.diagnostics, "XESHO0014", "DT_NEEDED entries must be unique valid SONAMEs without path separators");
            return result;
        }
    }
    if (!imports.empty() && needed_libraries.empty()) {
        diagnostic(result.diagnostics, "XESHO0015", "undefined imported symbols require at least one DT_NEEDED dependency");
        return result;
    }

    std::set<std::string> symbol_names;
    for (const auto& symbol : exports) {
        if (!valid_name(symbol.name) || !symbol_names.insert(symbol.name).second) {
            diagnostic(result.diagnostics, "XESHO0006", "dynamic export names must be non-empty, NUL-free, and unique");
            return result;
        }
        std::uint64_t ignored = 0U;
        if (!address_for_section(symbol.section, symbol.value,
                0U, spec.text.size(), 0U, spec.read_only_data.size(),
                0U, spec.data.size() + spec.bss_size, ignored)) {
            diagnostic(result.diagnostics, "XESHO0007", "dynamic export lies outside its declared section");
            return result;
        }
        std::size_t section_size = 0U;
        switch (symbol.section) {
        case Elf64SharedSection::Text: section_size = spec.text.size(); break;
        case Elf64SharedSection::ReadOnlyData: section_size = spec.read_only_data.size(); break;
        case Elf64SharedSection::Data: section_size = spec.data.size() + spec.bss_size; break;
        default: section_size = 0U; break;
        }
        if (symbol.size > section_size - symbol.value) {
            diagnostic(result.diagnostics, "XESHO0007", "dynamic export size exceeds its declared section");
            return result;
        }
        if ((symbol.kind == Elf64DynamicSymbolKind::Function && symbol.section != Elf64SharedSection::Text) ||
            (symbol.kind == Elf64DynamicSymbolKind::Object && symbol.section == Elf64SharedSection::Text)) {
            diagnostic(result.diagnostics, "XESHO0008", "dynamic symbol kind does not match its section permissions");
            return result;
        }
    }
    for (const auto& symbol : imports) {
        if (!valid_name(symbol.name) || !symbol_names.insert(symbol.name).second) {
            diagnostic(result.diagnostics, "XESHO0016", "dynamic import names must be non-empty, NUL-free, unique and distinct from exports");
            return result;
        }
        if (symbol.kind != Elf64DynamicSymbolKind::Function && symbol.kind != Elf64DynamicSymbolKind::Object) {
            diagnostic(result.diagnostics, "XESHO0017", "dynamic import kind must be Function or Object");
            return result;
        }
        if (symbol.binding != Elf64ImportBinding::Got && symbol.binding != Elf64ImportBinding::Plt) {
            diagnostic(result.diagnostics, "XESHO0023", "dynamic import binding must be GOT or PLT");
            return result;
        }
        if (symbol.binding == Elf64ImportBinding::Plt && symbol.kind != Elf64DynamicSymbolKind::Function) {
            diagnostic(result.diagnostics, "XESHO0024", "PLT binding is supported only for imported functions");
            return result;
        }
    }

    const std::size_t plt_import_count = static_cast<std::size_t>(std::count_if(
        imports.begin(), imports.end(), [](const Elf64ImportedSymbol& item) {
            return item.binding == Elf64ImportBinding::Plt;
        }));
    const std::size_t got_import_count = imports.size() - plt_import_count;
    const bool has_plt = plt_import_count != 0U;
    if (has_plt && spec.text.size() > std::numeric_limits<std::size_t>::max() - 15U) {
        diagnostic(result.diagnostics, "XESHO0025", "PLT text alignment overflows size_t");
        return result;
    }
    const std::size_t plt_payload_offset = has_plt ? ((spec.text.size() + 15U) & ~static_cast<std::size_t>(15U)) : spec.text.size();
    if (has_plt && plt_import_count > (std::numeric_limits<std::size_t>::max() / 16U) - 1U) {
        diagnostic(result.diagnostics, "XESHO0025", "PLT text size overflows size_t");
        return result;
    }
    const std::size_t plt_table_size = has_plt ? (plt_import_count + 1U) * 16U : 0U;
    if (has_plt && plt_payload_offset > std::numeric_limits<std::size_t>::max() - plt_table_size) {
        diagnostic(result.diagnostics, "XESHO0025", "PLT payload range overflows size_t");
        return result;
    }
    const std::size_t plt_payload_size = has_plt ? (plt_payload_offset + plt_table_size) : spec.text.size();
    if (plt_payload_size < spec.text.size()) {
        diagnostic(result.diagnostics, "XESHO0025", "PLT text payload size overflows size_t");
        return result;
    }

    std::uint64_t phdr_end = HeaderSize + ProgramHeaderCount * ProgramHeaderSize;
    std::uint64_t text_offset = 0U;
    if (!align_up(phdr_end, 16U, text_offset)) {
        diagnostic(result.diagnostics, "XESHO0009", "text file offset alignment overflow");
        return result;
    }
    std::uint64_t text_end = 0U;
    if (!checked_add(text_offset, plt_payload_size, text_end)) {
        diagnostic(result.diagnostics, "XESHO0009", "text and PLT file range overflow");
        return result;
    }
    std::uint64_t ro_offset = 0U;
    if (!align_up(text_end, spec.page_size, ro_offset)) {
        diagnostic(result.diagnostics, "XESHO0009", "read-only segment alignment overflow");
        return result;
    }

    // Construct the read-only dynamic metadata segment.
    std::vector<std::uint8_t> ro_bytes;
    auto append_aligned = [&ro_bytes](std::size_t alignment) {
        const std::size_t mask = alignment - 1U;
        const std::size_t padding = (alignment - (ro_bytes.size() & mask)) & mask;
        ro_bytes.insert(ro_bytes.end(), padding, 0U);
    };
    const std::size_t rodata_offset = 0U;
    ro_bytes.insert(ro_bytes.end(), spec.read_only_data.begin(), spec.read_only_data.end());
    append_aligned(8U);
    const std::size_t dynstr_offset = ro_bytes.size();
    std::vector<std::uint8_t> dynstr(1U, 0U);
    auto append_string = [&dynstr](const std::string& value) -> std::uint32_t {
        const std::uint32_t offset = static_cast<std::uint32_t>(dynstr.size());
        dynstr.insert(dynstr.end(), value.begin(), value.end());
        dynstr.push_back(0U);
        return offset;
    };
    const std::uint32_t soname_offset = append_string(spec.soname);
    std::vector<std::uint32_t> needed_offsets;
    needed_offsets.reserve(needed_libraries.size());
    for (const auto& dependency : needed_libraries) needed_offsets.push_back(append_string(dependency));
    std::vector<SymbolLayout> symbol_layouts;
    symbol_layouts.reserve(exports.size());
    for (const auto& symbol : exports) {
        const std::uint32_t name_offset = append_string(symbol.name);
        symbol_layouts.push_back(SymbolLayout{symbol, name_offset, 0U});
    }
    std::vector<ImportLayout> import_layouts;
    import_layouts.reserve(imports.size());
    for (std::size_t i = 0U; i < imports.size(); ++i) {
        import_layouts.push_back(ImportLayout{imports[i], append_string(imports[i].name),
            static_cast<std::uint32_t>(exports.size() + i + 1U), 0U});
    }
    ro_bytes.insert(ro_bytes.end(), dynstr.begin(), dynstr.end());

    append_aligned(8U);
    const std::size_t dynsym_offset = ro_bytes.size();
    const std::size_t dynamic_symbol_count = exports.size() + imports.size();
    const std::size_t dynsym_size = (dynamic_symbol_count + 1U) * SymbolEntrySize;
    ro_bytes.resize(ro_bytes.size() + dynsym_size, 0U);

    append_aligned(4U);
    const std::size_t hash_offset = ro_bytes.size();
    const std::size_t hash_bucket_count = choose_bucket_count(dynamic_symbol_count);
    const std::size_t hash_size = 8U + hash_bucket_count * 4U + (dynamic_symbol_count + 1U) * 4U;
    ro_bytes.resize(ro_bytes.size() + hash_size, 0U);

    append_aligned(8U);
    const std::size_t rela_offset = ro_bytes.size();
    const std::size_t relocation_count = spec.relative_relocations.size() + got_import_count + (has_plt ? 1U : 0U);
    const std::size_t rela_size = relocation_count * RelaEntrySize;
    ro_bytes.resize(ro_bytes.size() + rela_size, 0U);

    append_aligned(8U);
    const std::size_t jmprel_offset = ro_bytes.size();
    const std::size_t jmprel_size = plt_import_count * RelaEntrySize;
    ro_bytes.resize(ro_bytes.size() + jmprel_size, 0U);

    append_aligned(8U);
    const std::size_t dynamic_offset = ro_bytes.size();
    const std::size_t dynamic_count = 10U + needed_libraries.size() + (has_plt ? 4U : 0U);
    const std::size_t dynamic_size = dynamic_count * DynamicEntrySize;
    ro_bytes.resize(ro_bytes.size() + dynamic_size, 0U);

    const std::uint64_t ro_address = ro_offset;
    std::uint64_t data_offset = 0U;
    std::uint64_t ro_end = 0U;
    if (!checked_add(ro_offset, ro_bytes.size(), ro_end) || !align_up(ro_end, spec.page_size, data_offset)) {
        diagnostic(result.diagnostics, "XESHO0009", "read-only metadata segment range overflow");
        return result;
    }
    const std::uint64_t data_address = data_offset;
    const std::size_t got_offset = got_import_count == 0U
        ? spec.data.size()
        : ((spec.data.size() + 7U) & ~static_cast<std::size_t>(7U));
    if (got_import_count > (std::numeric_limits<std::size_t>::max() - got_offset) / 8U) {
        diagnostic(result.diagnostics, "XESHO0018", "GOT size overflows size_t");
        return result;
    }
    const std::size_t got_size = got_import_count * 8U;
    if (got_offset > std::numeric_limits<std::size_t>::max() - got_size) {
        diagnostic(result.diagnostics, "XESHO0026", "GOT range overflows size_t");
        return result;
    }
    const std::size_t got_end = got_offset + got_size;
    if (has_plt && got_end > std::numeric_limits<std::size_t>::max() - 7U) {
        diagnostic(result.diagnostics, "XESHO0026", "GOTPLT alignment overflows size_t");
        return result;
    }
    const std::size_t gotplt_offset = has_plt
        ? ((got_end + 7U) & ~static_cast<std::size_t>(7U))
        : got_end;
    if (has_plt && (gotplt_offset > std::numeric_limits<std::size_t>::max() - 24U ||
        plt_import_count > (std::numeric_limits<std::size_t>::max() - gotplt_offset - 24U) / 8U)) {
        diagnostic(result.diagnostics, "XESHO0026", "GOTPLT size overflows size_t");
        return result;
    }
    const std::size_t gotplt_size = has_plt ? 24U + plt_import_count * 8U : 0U;
    if (gotplt_offset > std::numeric_limits<std::size_t>::max() - gotplt_size) {
        diagnostic(result.diagnostics, "XESHO0026", "GOTPLT range overflows size_t");
        return result;
    }
    const std::size_t bss_offset = gotplt_offset + gotplt_size;
    if (spec.bss_size > std::numeric_limits<std::size_t>::max() - bss_offset) {
        diagnostic(result.diagnostics, "XESHO0010", "data, GOT, GOTPLT and BSS size overflows size_t");
        return result;
    }
    const std::size_t data_file_size = bss_offset;
    const std::size_t data_memory_size = data_file_size + spec.bss_size;
    std::uint64_t total_file_size = 0U;
    if (!checked_add(data_offset, data_file_size, total_file_size) ||
        total_file_size > spec.maximum_file_size || total_file_size > std::numeric_limits<std::size_t>::max()) {
        diagnostic(result.diagnostics, "XESHO0011", "shared-object file exceeds configured maximum size");
        return result;
    }
    std::uint64_t total_memory_size = 0U;
    if (!checked_add(data_address, data_memory_size, total_memory_size)) {
        diagnostic(result.diagnostics, "XESHO0010", "shared-object memory range overflows");
        return result;
    }

    // Validate and resolve dynamic symbols now that segment addresses are known.
    for (auto& layout : symbol_layouts) {
        if (layout.symbol.section == Elf64SharedSection::Data) {
            const std::size_t mapped = layout.symbol.value < spec.data.size()
                ? layout.symbol.value
                : bss_offset + (layout.symbol.value - spec.data.size());
            if (!checked_add(data_address, static_cast<std::uint64_t>(mapped), layout.address)) {
                diagnostic(result.diagnostics, "XESHO0007", "dynamic export address cannot be represented");
                return result;
            }
        } else if (!address_for_section(layout.symbol.section, layout.symbol.value,
                text_offset, spec.text.size(), ro_address + rodata_offset, spec.read_only_data.size(),
                data_address, data_memory_size, layout.address)) {
            diagnostic(result.diagnostics, "XESHO0007", "dynamic export address cannot be represented");
            return result;
        }
    }

    std::map<std::string, std::uint32_t> import_indices;
    std::map<std::string, std::size_t> plt_indices;
    std::size_t next_got_index = 0U;
    std::size_t next_plt_index = 0U;
    for (auto& item : import_layouts) {
        import_indices.emplace(item.symbol.name, item.symbol_index);
        if (item.symbol.binding == Elf64ImportBinding::Plt) {
            item.plt_index = next_plt_index++;
            plt_indices.emplace(item.symbol.name, item.plt_index);
        } else {
            item.got_offset = got_offset + next_got_index * 8U;
            ++next_got_index;
        }
    }

    // RIP-relative references in text are resolved statically to the GOT slot.
    std::vector<std::uint8_t> text_bytes = spec.text;
    std::vector<std::size_t> patch_offsets;
    patch_offsets.reserve(spec.got_pc_relative32.size());
    for (const auto& reference : spec.got_pc_relative32) {
        const auto found = import_indices.find(reference.symbol_name);
        if (found == import_indices.end()) {
            diagnostic(result.diagnostics, "XESHO0019", "RIP-relative GOT reference names no declared import");
            return result;
        }
        if (reference.text_displacement_offset > text_bytes.size() ||
            text_bytes.size() - reference.text_displacement_offset < 4U) {
            diagnostic(result.diagnostics, "XESHO0020", "RIP-relative GOT displacement field lies outside .text");
            return result;
        }
        patch_offsets.push_back(reference.text_displacement_offset);
    }
    std::sort(patch_offsets.begin(), patch_offsets.end());
    for (std::size_t i = 1U; i < patch_offsets.size(); ++i) {
        if (patch_offsets[i] < patch_offsets[i - 1U] + 4U) {
            diagnostic(result.diagnostics, "XESHO0021", "RIP-relative GOT patch fields must not overlap");
            return result;
        }
    }
    for (const auto& reference : spec.got_pc_relative32) {
        const auto found = std::find_if(import_layouts.begin(), import_layouts.end(), [&reference](const ImportLayout& item) {
            return item.symbol.name == reference.symbol_name;
        });
        if (found == import_layouts.end() || found->symbol.binding != Elf64ImportBinding::Got) {
            diagnostic(result.diagnostics, "XESHO0031", "RIP-relative GOT reference must target an import with GOT binding");
            return result;
        }
        const std::uint64_t got_address = data_address + found->got_offset;
        const std::uint64_t place = text_offset + reference.text_displacement_offset;
        const std::int64_t displacement = static_cast<std::int64_t>(got_address) -
            static_cast<std::int64_t>(place + 4U);
        if (displacement < std::numeric_limits<std::int32_t>::min() ||
            displacement > std::numeric_limits<std::int32_t>::max()) {
            diagnostic(result.diagnostics, "XESHO0022", "RIP-relative GOT displacement exceeds signed 32-bit range");
            return result;
        }
        write_i32(text_bytes, reference.text_displacement_offset, static_cast<std::int32_t>(displacement));
    }

    std::vector<std::size_t> plt_patch_offsets;
    plt_patch_offsets.reserve(spec.plt_pc_relative32.size());
    for (const auto& reference : spec.plt_pc_relative32) {
        const auto found = plt_indices.find(reference.symbol_name);
        if (found == plt_indices.end()) {
            diagnostic(result.diagnostics, "XESHO0027", "PLT call reference names no imported function with PLT binding");
            return result;
        }
        if (reference.text_displacement_offset > text_bytes.size() ||
            text_bytes.size() - reference.text_displacement_offset < 4U ||
            reference.text_displacement_offset < 1U ||
            text_bytes[reference.text_displacement_offset - 1U] != 0xe8U) {
            diagnostic(result.diagnostics, "XESHO0028", "PLT call reference must point to the rel32 field of a near CALL instruction in .text");
            return result;
        }
        plt_patch_offsets.push_back(reference.text_displacement_offset);
    }
    std::sort(plt_patch_offsets.begin(), plt_patch_offsets.end());
    for (std::size_t i = 1U; i < plt_patch_offsets.size(); ++i) {
        if (plt_patch_offsets[i] < plt_patch_offsets[i - 1U] + 4U) {
            diagnostic(result.diagnostics, "XESHO0029", "PLT call displacement fields must not overlap");
            return result;
        }
    }
    for (const auto got_patch : patch_offsets) {
        for (const auto plt_patch : plt_patch_offsets) {
            if (got_patch < plt_patch + 4U && plt_patch < got_patch + 4U) {
                diagnostic(result.diagnostics, "XESHO0031", "GOT and PLT displacement patches must not overlap");
                return result;
            }
        }
    }
    for (const auto& reference : spec.plt_pc_relative32) {
        const auto found = plt_indices.find(reference.symbol_name);
        const std::uint64_t stub = text_offset + plt_payload_offset + (found->second + 1U) * 16U;
        const std::uint64_t place = text_offset + reference.text_displacement_offset;
        const std::int64_t displacement = static_cast<std::int64_t>(stub) - static_cast<std::int64_t>(place + 4U);
        if (displacement < std::numeric_limits<std::int32_t>::min() ||
            displacement > std::numeric_limits<std::int32_t>::max()) {
            diagnostic(result.diagnostics, "XESHO0030", "PLT call displacement exceeds signed 32-bit range");
            return result;
        }
        write_i32(text_bytes, reference.text_displacement_offset, static_cast<std::int32_t>(displacement));
    }

    // Each base relocation writes a pointer-sized value in RW data/BSS. Imported
    // symbols receive dedicated GOT slots and R_X86_64_GLOB_DAT entries.
    std::vector<Elf64RelativeRelocation> relocations = spec.relative_relocations;
    std::sort(relocations.begin(), relocations.end(), [](const auto& left, const auto& right) {
        return left.target_offset < right.target_offset;
    });
    std::size_t previous_target_end = 0U;
    for (const auto& relocation : relocations) {
        const std::size_t logical_data_memory_size = spec.data.size() + spec.bss_size;
        if (relocation.target_offset < previous_target_end ||
            relocation.target_offset > logical_data_memory_size ||
            logical_data_memory_size - relocation.target_offset < 8U) {
            diagnostic(result.diagnostics, "XESHO0012", "relative relocation target must be non-overlapping and contained in RW data/BSS");
            return result;
        }
        const std::size_t physical_target_offset = relocation.target_offset < spec.data.size()
            ? relocation.target_offset
            : bss_offset + (relocation.target_offset - spec.data.size());
        if ((physical_target_offset & 7U) != 0U) {
            diagnostic(result.diagnostics, "XESHO0012", "relative relocation target must be pointer-aligned in the final RW image layout");
            return result;
        }
        previous_target_end = relocation.target_offset + 8U;
        std::uint64_t addend = 0U;
        bool addend_valid = false;
        if (relocation.addend_section == Elf64SharedSection::Data) {
            const std::size_t logical_size = spec.data.size() + spec.bss_size;
            if (relocation.addend_value < logical_size) {
                const std::size_t mapped = relocation.addend_value < spec.data.size()
                    ? relocation.addend_value
                    : bss_offset + (relocation.addend_value - spec.data.size());
                addend_valid = checked_add(data_address, static_cast<std::uint64_t>(mapped), addend);
            }
        } else {
            addend_valid = address_for_section(relocation.addend_section, relocation.addend_value,
                text_offset, spec.text.size(), ro_address + rodata_offset, spec.read_only_data.size(),
                data_address, data_memory_size, addend);
        }
        if (!addend_valid || addend > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            diagnostic(result.diagnostics, "XESHO0013", "R_X86_64_RELATIVE addend must point into a loadable image section and fit signed ELF64 RELA");
            return result;
        }
    }

    // Serialize defined exports and undefined imported symbols into dynsym.
    for (std::size_t i = 0U; i < symbol_layouts.size(); ++i) {
        const auto& symbol = symbol_layouts[i];
        const std::size_t offset = dynsym_offset + (i + 1U) * SymbolEntrySize;
        write_u32(ro_bytes, offset, symbol.name_offset);
        const std::uint8_t type = static_cast<std::uint8_t>(symbol.symbol.kind);
        ro_bytes[offset + 4U] = static_cast<std::uint8_t>(0x10U | type); // STB_GLOBAL | STT_*.
        ro_bytes[offset + 5U] = 0U;
        write_u16(ro_bytes, offset + 6U, static_cast<std::uint16_t>(symbol.symbol.section));
        write_u64(ro_bytes, offset + 8U, symbol.address);
        write_u64(ro_bytes, offset + 16U, symbol.symbol.size);
    }
    for (const auto& imported : import_layouts) {
        const std::size_t offset = dynsym_offset + static_cast<std::size_t>(imported.symbol_index) * SymbolEntrySize;
        write_u32(ro_bytes, offset, imported.name_offset);
        const std::uint8_t type = static_cast<std::uint8_t>(imported.symbol.kind);
        ro_bytes[offset + 4U] = static_cast<std::uint8_t>(0x10U | type); // STB_GLOBAL | STT_*.
        ro_bytes[offset + 5U] = 0U;
        write_u16(ro_bytes, offset + 6U, 0U); // SHN_UNDEF
        write_u64(ro_bytes, offset + 8U, 0U);
        write_u64(ro_bytes, offset + 16U, 0U);
    }

    write_u32(ro_bytes, hash_offset, static_cast<std::uint32_t>(hash_bucket_count));
    write_u32(ro_bytes, hash_offset + 4U, static_cast<std::uint32_t>(dynamic_symbol_count + 1U));
    std::vector<std::uint32_t> hash_buckets(hash_bucket_count, 0U);
    std::vector<std::uint32_t> hash_chains(dynamic_symbol_count + 1U, 0U);
    std::vector<std::uint32_t> hash_tails(hash_bucket_count, 0U);
    auto add_hash_name = [&](std::size_t name_index, const std::string& name) {
        const std::uint32_t symbol_index = static_cast<std::uint32_t>(name_index + 1U);
        const std::size_t bucket = sysv_hash(name) % hash_bucket_count;
        if (hash_buckets[bucket] == 0U) hash_buckets[bucket] = symbol_index;
        else hash_chains[hash_tails[bucket]] = symbol_index;
        hash_tails[bucket] = symbol_index;
    };
    for (std::size_t i = 0U; i < symbol_layouts.size(); ++i) add_hash_name(i, symbol_layouts[i].symbol.name);
    for (std::size_t i = 0U; i < import_layouts.size(); ++i)
        add_hash_name(symbol_layouts.size() + i, import_layouts[i].symbol.name);
    const std::size_t buckets_offset = hash_offset + 8U;
    const std::size_t chains_offset = buckets_offset + hash_bucket_count * 4U;
    for (std::size_t i = 0U; i < hash_buckets.size(); ++i)
        write_u32(ro_bytes, buckets_offset + i * 4U, hash_buckets[i]);
    for (std::size_t i = 0U; i < hash_chains.size(); ++i)
        write_u32(ro_bytes, chains_offset + i * 4U, hash_chains[i]);

    std::vector<SerializedRelocation> serialized_relocations;
    serialized_relocations.reserve(relocation_count);
    for (const auto& relocation : relocations) {
        std::uint64_t addend = 0U;
        if (relocation.addend_section == Elf64SharedSection::Data) {
            const std::size_t mapped = relocation.addend_value < spec.data.size()
                ? relocation.addend_value
                : bss_offset + (relocation.addend_value - spec.data.size());
            (void)checked_add(data_address, static_cast<std::uint64_t>(mapped), addend);
        } else {
            (void)address_for_section(relocation.addend_section, relocation.addend_value,
                text_offset, spec.text.size(), ro_address + rodata_offset, spec.read_only_data.size(),
                data_address, data_memory_size, addend);
        }
        const std::size_t mapped_target = relocation.target_offset < spec.data.size()
            ? relocation.target_offset
            : bss_offset + (relocation.target_offset - spec.data.size());
        serialized_relocations.push_back(SerializedRelocation{
            data_address + mapped_target, RelativeRelocationType,
            static_cast<std::int64_t>(addend)});
    }
    for (const auto& imported : import_layouts) {
        if (imported.symbol.binding == Elf64ImportBinding::Got) {
            serialized_relocations.push_back(SerializedRelocation{
                data_address + imported.got_offset,
                (static_cast<std::uint64_t>(imported.symbol_index) << 32U) | GlobalDataRelocationType,
                0});
        }
    }
    if (has_plt) {
        // GOTPLT[0] carries the relocated address of the PT_DYNAMIC array.
        serialized_relocations.push_back(SerializedRelocation{
            data_address + gotplt_offset, RelativeRelocationType,
            static_cast<std::int64_t>(ro_address + dynamic_offset)});
    }
    for (std::size_t i = 0U; i < serialized_relocations.size(); ++i) {
        const auto& relocation = serialized_relocations[i];
        const std::size_t offset = rela_offset + i * RelaEntrySize;
        write_u64(ro_bytes, offset, relocation.target);
        write_u64(ro_bytes, offset + 8U, relocation.info);
        write_i64(ro_bytes, offset + 16U, relocation.addend);
    }
    for (const auto& imported : import_layouts) {
        if (imported.symbol.binding != Elf64ImportBinding::Plt) continue;
        const std::size_t relocation_index = imported.plt_index;
        const std::size_t offset = jmprel_offset + relocation_index * RelaEntrySize;
        const std::uint64_t gotplt_slot = data_address + gotplt_offset + 24U + relocation_index * 8U;
        write_u64(ro_bytes, offset, gotplt_slot);
        write_u64(ro_bytes, offset + 8U,
            (static_cast<std::uint64_t>(imported.symbol_index) << 32U) | JumpSlotRelocationType);
        write_i64(ro_bytes, offset + 16U, 0);
    }

    const std::uint64_t dynstr_address = ro_address + dynstr_offset;
    const std::uint64_t dynsym_address = ro_address + dynsym_offset;
    const std::uint64_t hash_address = ro_address + hash_offset;
    const std::uint64_t rela_address = ro_address + rela_offset;
    const std::uint64_t dynamic_address = ro_address + dynamic_offset;
    std::vector<Elf64DynamicEntry> dynamic_entries;
    dynamic_entries.reserve(dynamic_count);
    for (const auto offset : needed_offsets) dynamic_entries.push_back({DtNeeded, offset});
    dynamic_entries.push_back({DtHash, hash_address});
    dynamic_entries.push_back({DtStrtab, dynstr_address});
    dynamic_entries.push_back({DtSymtab, dynsym_address});
    dynamic_entries.push_back({DtStrSize, dynstr.size()});
    dynamic_entries.push_back({DtSymEntrySize, SymbolEntrySize});
    dynamic_entries.push_back({DtSoname, soname_offset});
    if (has_plt) {
        dynamic_entries.push_back({DtPltGot, data_address + gotplt_offset});
        dynamic_entries.push_back({DtPltRelSize, jmprel_size});
        dynamic_entries.push_back({DtPltRel, 7U}); // DT_RELA
        dynamic_entries.push_back({DtJmpRel, ro_address + jmprel_offset});
    }
    dynamic_entries.push_back({DtRela, rela_address});
    dynamic_entries.push_back({DtRelaSize, rela_size});
    dynamic_entries.push_back({DtRelaEntrySize, RelaEntrySize});
    dynamic_entries.push_back({DtNull, 0U});
    for (std::size_t i = 0U; i < dynamic_entries.size(); ++i) {
        const std::size_t offset = dynamic_offset + i * DynamicEntrySize;
        write_i64(ro_bytes, offset, dynamic_entries[i].tag);
        write_u64(ro_bytes, offset + 8U, dynamic_entries[i].value);
    }

    std::vector<std::uint8_t> plt_bytes;
    if (has_plt) {
        plt_bytes.assign((plt_import_count + 1U) * 16U, 0U);
        const std::uint64_t plt_address = text_offset + plt_payload_offset;
        const std::uint64_t gotplt_address = data_address + gotplt_offset;
        // PLT0: push GOTPLT[1](%rip); jmp *GOTPLT[2](%rip); nop.
        plt_bytes[0U] = 0xffU; plt_bytes[1U] = 0x35U;
        const std::int64_t push_disp = static_cast<std::int64_t>(gotplt_address + 8U) -
            static_cast<std::int64_t>(plt_address + 6U);
        if (push_disp < std::numeric_limits<std::int32_t>::min() || push_disp > std::numeric_limits<std::int32_t>::max()) {
            diagnostic(result.diagnostics, "XESHO0030", "PLT0 GOTPLT push displacement exceeds signed 32-bit range");
            return result;
        }
        write_i32(plt_bytes, 2U, static_cast<std::int32_t>(push_disp));
        plt_bytes[6U] = 0xffU; plt_bytes[7U] = 0x25U;
        const std::int64_t jump_disp = static_cast<std::int64_t>(gotplt_address + 16U) -
            static_cast<std::int64_t>(plt_address + 12U);
        if (jump_disp < std::numeric_limits<std::int32_t>::min() || jump_disp > std::numeric_limits<std::int32_t>::max()) {
            diagnostic(result.diagnostics, "XESHO0030", "PLT0 GOTPLT resolver displacement exceeds signed 32-bit range");
            return result;
        }
        write_i32(plt_bytes, 8U, static_cast<std::int32_t>(jump_disp));
        plt_bytes[12U] = 0x0fU; plt_bytes[13U] = 0x1fU; plt_bytes[14U] = 0x40U; plt_bytes[15U] = 0x00U;
        for (const auto& imported : import_layouts) {
            if (imported.symbol.binding != Elf64ImportBinding::Plt) continue;
            const std::size_t index = imported.plt_index;
            const std::size_t entry = (index + 1U) * 16U;
            const std::uint64_t entry_address = plt_address + entry;
            const std::uint64_t slot_address = gotplt_address + 24U + index * 8U;
            plt_bytes[entry] = 0xffU; plt_bytes[entry + 1U] = 0x25U;
            const std::int64_t slot_disp = static_cast<std::int64_t>(slot_address) -
                static_cast<std::int64_t>(entry_address + 6U);
            if (slot_disp < std::numeric_limits<std::int32_t>::min() || slot_disp > std::numeric_limits<std::int32_t>::max()) {
                diagnostic(result.diagnostics, "XESHO0030", "PLT entry GOTPLT displacement exceeds signed 32-bit range");
                return result;
            }
            write_i32(plt_bytes, entry + 2U, static_cast<std::int32_t>(slot_disp));
            plt_bytes[entry + 6U] = 0x68U; // push imm32: .rela.plt ordinal
            write_u32(plt_bytes, entry + 7U, static_cast<std::uint32_t>(index));
            plt_bytes[entry + 11U] = 0xe9U; // jmp rel32 PLT0
            const std::int64_t back_disp = static_cast<std::int64_t>(plt_address) -
                static_cast<std::int64_t>(entry_address + 16U);
            if (back_disp < std::numeric_limits<std::int32_t>::min() || back_disp > std::numeric_limits<std::int32_t>::max()) {
                diagnostic(result.diagnostics, "XESHO0030", "PLT entry branch to PLT0 exceeds signed 32-bit range");
                return result;
            }
            write_i32(plt_bytes, entry + 12U, static_cast<std::int32_t>(back_disp));
        }
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(total_file_size), 0U);
    // ELF64 ET_DYN header; no section-header table is required by the dynamic loader.
    bytes[0U] = 0x7fU; bytes[1U] = 'E'; bytes[2U] = 'L'; bytes[3U] = 'F';
    bytes[4U] = 2U; bytes[5U] = 1U; bytes[6U] = 1U; bytes[7U] = 0U;
    write_u16(bytes, 16U, ElfTypeShared);
    write_u16(bytes, 18U, ElfMachineX86_64);
    write_u32(bytes, 20U, ElfVersionCurrent);
    write_u64(bytes, 24U, 0U); // Shared objects do not have a process entry point.
    write_u64(bytes, 32U, HeaderSize);
    write_u64(bytes, 40U, 0U);
    write_u32(bytes, 48U, 0U);
    write_u16(bytes, 52U, static_cast<std::uint16_t>(HeaderSize));
    write_u16(bytes, 54U, static_cast<std::uint16_t>(ProgramHeaderSize));
    write_u16(bytes, 56U, static_cast<std::uint16_t>(ProgramHeaderCount));
    write_u16(bytes, 58U, 0U);
    write_u16(bytes, 60U, 0U);
    write_u16(bytes, 62U, 0U);

    write_program_header(bytes, 64U, PtLoad, PfRead | PfExecute,
                         0U, 0U, text_end, text_end, spec.page_size);
    write_program_header(bytes, 64U + ProgramHeaderSize, PtLoad, PfRead,
                         ro_offset, ro_address, ro_bytes.size(), ro_bytes.size(), spec.page_size);
    write_program_header(bytes, 64U + 2U * ProgramHeaderSize, PtLoad, PfRead | PfWrite,
                         data_offset, data_address, data_file_size, data_memory_size, spec.page_size);
    write_program_header(bytes, 64U + 3U * ProgramHeaderSize, PtDynamic, PfRead,
                         ro_offset + dynamic_offset, dynamic_address, dynamic_size, dynamic_size, 8U);
    write_program_header(bytes, 64U + 4U * ProgramHeaderSize, PtGnuStack, PfRead | PfWrite,
                         0U, 0U, 0U, 0U, 16U);

    std::copy(text_bytes.begin(), text_bytes.end(), bytes.begin() + static_cast<std::ptrdiff_t>(text_offset));
    if (!plt_bytes.empty()) {
        std::copy(plt_bytes.begin(), plt_bytes.end(),
            bytes.begin() + static_cast<std::ptrdiff_t>(text_offset + plt_payload_offset));
    }
    std::copy(ro_bytes.begin(), ro_bytes.end(), bytes.begin() + static_cast<std::ptrdiff_t>(ro_offset));
    std::copy(spec.data.begin(), spec.data.end(), bytes.begin() + static_cast<std::ptrdiff_t>(data_offset));
    if (has_plt) {
        // The lazy resolver adds the object's load bias to the initial GOTPLT
        // slot value. Seed each slot with the link-time address of PLTn+6,
        // where the relocation ordinal is pushed before branching to PLT0.
        for (const auto& imported : import_layouts) {
            if (imported.symbol.binding != Elf64ImportBinding::Plt) continue;
            const std::uint64_t lazy_entry = text_offset + plt_payload_offset +
                (imported.plt_index + 1U) * 16U + 6U;
            const std::size_t slot_file_offset = static_cast<std::size_t>(data_offset) +
                gotplt_offset + 24U + imported.plt_index * 8U;
            write_u64(bytes, slot_file_offset, lazy_entry);
        }
    }

    result.image.bytes = std::move(bytes);
    result.image.dynamic_entries = dynamic_entries;
    result.image.text_address = text_offset;
    result.image.read_only_data_address = ro_address + rodata_offset;
    result.image.data_address = data_address;
    result.image.dynamic_address = dynamic_address;
    result.image.relocation_address = rela_address;
    result.image.exported_symbol_count = exports.size();
    result.image.imported_symbol_count = imports.size();
    result.image.needed_library_count = needed_libraries.size();
    result.image.symbol_relocation_count = got_import_count;
    result.image.relative_relocation_count = relocations.size() + (has_plt ? 1U : 0U);
    result.image.got_address = data_address + got_offset;
    result.image.got_size = got_size;
    result.image.gotplt_address = has_plt ? data_address + gotplt_offset : 0U;
    result.image.gotplt_size = gotplt_size;
    result.image.plt_address = has_plt ? text_offset + plt_payload_offset : 0U;
    result.image.plt_size = plt_bytes.size();
    result.image.plt_relocation_address = has_plt ? ro_address + jmprel_offset : 0U;
    result.image.plt_relocation_count = plt_import_count;
    result.image.memory_size = static_cast<std::size_t>(total_memory_size);
    return result;
}

} // namespace xfrontend
