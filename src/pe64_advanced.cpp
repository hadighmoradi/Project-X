// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/pe64_advanced.hpp"
#include "xfrontend/archive.hpp"
#include "xfrontend/binary_object.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace xfrontend {
namespace {

constexpr std::size_t kCoffHeader = 20U;
constexpr std::size_t kOptionalHeader = 240U;
constexpr std::size_t kSectionHeader = 40U;
constexpr std::size_t kDataDirectoryOffset = 112U;
constexpr std::uint32_t kReadOnly = 0x40000040U;
constexpr std::uint32_t kReadWrite = 0xc0000040U;
constexpr std::uint32_t kRelocRead = 0x42000040U;
constexpr std::uint16_t kRelocDir64 = 10U;
constexpr std::size_t kLoadConfigSize64 = 0x94U;
constexpr std::size_t kMaximumAdvancedEntries = 4096U;
constexpr std::size_t kMaximumAdvancedBytes = 16U * 1024U * 1024U;

void put16(std::vector<std::uint8_t>& b, std::size_t o, std::uint16_t v) {
    b[o] = static_cast<std::uint8_t>(v & 0xffU);
    b[o + 1U] = static_cast<std::uint8_t>((v >> 8U) & 0xffU);
}
void put32(std::vector<std::uint8_t>& b, std::size_t o, std::uint32_t v) {
    for (unsigned i = 0U; i < 4U; ++i) b[o + i] = static_cast<std::uint8_t>((v >> (i * 8U)) & 0xffU);
}
void put64(std::vector<std::uint8_t>& b, std::size_t o, std::uint64_t v) {
    put32(b, o, static_cast<std::uint32_t>(v & 0xffffffffULL));
    put32(b, o + 4U, static_cast<std::uint32_t>(v >> 32U));
}
std::uint16_t get16(const std::vector<std::uint8_t>& b, std::size_t o) {
    return static_cast<std::uint16_t>(b[o]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[o + 1U]) << 8U);
}
std::uint32_t get32(const std::vector<std::uint8_t>& b, std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) |
        (static_cast<std::uint32_t>(b[o + 1U]) << 8U) |
        (static_cast<std::uint32_t>(b[o + 2U]) << 16U) |
        (static_cast<std::uint32_t>(b[o + 3U]) << 24U);
}
void append16(std::vector<std::uint8_t>& b, std::uint16_t v) {
    const auto o = b.size(); b.resize(o + 2U); put16(b, o, v);
}
void append32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    const auto o = b.size(); b.resize(o + 4U); put32(b, o, v);
}
void append_string(std::vector<std::uint8_t>& b, const std::string& s) {
    b.insert(b.end(), s.begin(), s.end()); b.push_back(0U);
}
bool align_up(std::uint64_t value, std::uint32_t alignment, std::uint64_t& out) {
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) return false;
    const std::uint64_t mask = static_cast<std::uint64_t>(alignment - 1U);
    if (value > std::numeric_limits<std::uint64_t>::max() - mask) return false;
    out = (value + mask) & ~mask; return true;
}
bool printable_ascii(const std::string& s, std::size_t max_len) {
    if (s.empty() || s.size() > max_len) return false;
    return std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= 0x20U && c <= 0x7eU; });
}
std::string lower_ascii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
void diagnostic(std::vector<Diagnostic>& out, const char* code, const std::string& message) {
    out.push_back(Diagnostic{code, message, SourceSpan{}});
}
bool read_pe_layout(const Pe64DllImage& image, std::size_t& pe, std::size_t& optional,
                    std::size_t& section_table, std::uint16_t& section_count,
                    std::uint32_t& size_headers) {
    if (image.bytes.size() < 0x100U || image.bytes[0] != 'M' || image.bytes[1] != 'Z') return false;
    pe = get32(image.bytes, 0x3cU);
    if (pe > image.bytes.size() || image.bytes.size() - pe < 24U) return false;
    if (image.bytes[pe] != 'P' || image.bytes[pe + 1U] != 'E' ||
        image.bytes[pe + 2U] != 0U || image.bytes[pe + 3U] != 0U) return false;
    section_count = get16(image.bytes, pe + 6U);
    optional = pe + 4U + kCoffHeader;
    if (optional > image.bytes.size() || image.bytes.size() - optional < kOptionalHeader) return false;
    if (get16(image.bytes, optional) != 0x20bU) return false;
    size_headers = get32(image.bytes, optional + 60U);
    section_table = optional + kOptionalHeader;
    if (section_table > size_headers || static_cast<std::uint64_t>(section_count) * kSectionHeader > size_headers - section_table)
        return false;
    if (size_headers > image.bytes.size()) return false;
    return true;
}
bool next_section_rva(const Pe64DllImage& image, std::uint32_t section_alignment, std::uint32_t& rva) {
    std::uint64_t end = 0U;
    for (const auto& s : image.sections)
        end = std::max(end, static_cast<std::uint64_t>(s.virtual_address) + s.virtual_size);
    std::uint64_t aligned = 0U;
    if (!align_up(end, section_alignment, aligned) || aligned > std::numeric_limits<std::uint32_t>::max()) return false;
    rva = static_cast<std::uint32_t>(aligned); return true;
}
bool append_section(Pe64DllImage& image, const std::string& name,
                    const std::vector<std::uint8_t>& payload, std::uint32_t characteristics,
                    std::uint32_t section_alignment, std::uint32_t file_alignment,
                    std::size_t maximum_file_size, std::uint32_t forced_rva,
                    std::uint32_t& section_rva, std::string& error) {
    if (name.empty() || name.size() > 8U || payload.empty() || payload.size() > kMaximumAdvancedBytes) {
        error = "Advanced PE section name or payload size is invalid."; return false;
    }
    std::size_t pe = 0U, optional = 0U, section_table = 0U;
    std::uint16_t section_count = 0U; std::uint32_t size_headers = 0U;
    if (!read_pe_layout(image, pe, optional, section_table, section_count, size_headers)) {
        error = "Base PE image has an invalid header or section table."; return false;
    }
    if (section_count == std::numeric_limits<std::uint16_t>::max()) {
        error = "PE section count exceeds the COFF header limit."; return false;
    }
    const std::uint64_t header_pos = static_cast<std::uint64_t>(section_table) +
        static_cast<std::uint64_t>(section_count) * kSectionHeader;
    if (header_pos > size_headers || size_headers - header_pos < kSectionHeader) {
        error = "Base PE header has no reserved room for another section header."; return false;
    }
    std::uint32_t rva = forced_rva;
    if (rva == 0U && !next_section_rva(image, section_alignment, rva)) {
        error = "Advanced section RVA overflows PE32 range."; return false;
    }
    std::uint64_t raw_offset64 = 0U, raw_size64 = 0U;
    if (!align_up(image.bytes.size(), file_alignment, raw_offset64) ||
        !align_up(payload.size(), file_alignment, raw_size64) ||
        raw_offset64 > std::numeric_limits<std::uint32_t>::max() ||
        raw_size64 > std::numeric_limits<std::uint32_t>::max() ||
        raw_offset64 + raw_size64 > maximum_file_size ||
        raw_offset64 + raw_size64 > std::numeric_limits<std::size_t>::max()) {
        error = "Advanced section raw layout exceeds the configured PE file limit."; return false;
    }
    std::uint64_t image_end = 0U, size_image = 0U;
    if (static_cast<std::uint64_t>(rva) + payload.size() > std::numeric_limits<std::uint32_t>::max() ||
        !align_up(static_cast<std::uint64_t>(rva) + payload.size(), section_alignment, size_image) ||
        size_image > std::numeric_limits<std::uint32_t>::max()) {
        error = "Advanced section virtual layout exceeds PE32 range."; return false;
    }
    image_end = static_cast<std::uint64_t>(rva) + payload.size();
    (void)image_end;
    image.bytes.resize(static_cast<std::size_t>(raw_offset64 + raw_size64), 0U);
    std::copy(payload.begin(), payload.end(), image.bytes.begin() + static_cast<std::ptrdiff_t>(raw_offset64));
    const std::size_t h = static_cast<std::size_t>(header_pos);
    std::fill(image.bytes.begin() + static_cast<std::ptrdiff_t>(h),
              image.bytes.begin() + static_cast<std::ptrdiff_t>(h + kSectionHeader),
              std::uint8_t{0U});
    std::copy(name.begin(), name.end(), image.bytes.begin() + static_cast<std::ptrdiff_t>(h));
    put32(image.bytes, h + 8U, static_cast<std::uint32_t>(payload.size()));
    put32(image.bytes, h + 12U, rva);
    put32(image.bytes, h + 16U, static_cast<std::uint32_t>(raw_size64));
    put32(image.bytes, h + 20U, static_cast<std::uint32_t>(raw_offset64));
    put32(image.bytes, h + 36U, characteristics);
    put16(image.bytes, pe + 6U, static_cast<std::uint16_t>(section_count + 1U));
    put32(image.bytes, optional + 56U, static_cast<std::uint32_t>(size_image));
    const std::uint64_t init_size = static_cast<std::uint64_t>(get32(image.bytes, optional + 8U)) + raw_size64;
    if (init_size > std::numeric_limits<std::uint32_t>::max()) {
        error = "PE SizeOfInitializedData overflows."; return false;
    }
    put32(image.bytes, optional + 8U, static_cast<std::uint32_t>(init_size));
    image.sections.push_back(Pe64DllSectionInfo{name, rva, static_cast<std::uint32_t>(payload.size()),
        static_cast<std::uint32_t>(raw_offset64), static_cast<std::uint32_t>(raw_size64), characteristics});
    image.size_of_image = static_cast<std::uint32_t>(size_image);
    section_rva = rva;
    return true;
}
std::size_t rva_to_file_offset(const Pe64DllImage& image, std::uint32_t rva, std::size_t length) {
    for (const auto& s : image.sections) {
        if (rva < s.virtual_address) continue;
        const std::uint64_t delta = static_cast<std::uint64_t>(rva) - s.virtual_address;
        if (delta <= s.raw_size && length <= static_cast<std::size_t>(s.raw_size - delta))
            return static_cast<std::size_t>(s.raw_offset) + static_cast<std::size_t>(delta);
    }
    return std::numeric_limits<std::size_t>::max();
}
void set_data_directory(Pe64DllImage& image, unsigned index, std::uint32_t rva, std::uint32_t size) {
    std::size_t pe = 0U, optional = 0U, table = 0U; std::uint16_t count = 0U; std::uint32_t headers = 0U;
    if (!read_pe_layout(image, pe, optional, table, count, headers)) return;
    const std::size_t off = optional + kDataDirectoryOffset + static_cast<std::size_t>(index) * 8U;
    put32(image.bytes, off, rva); put32(image.bytes, off + 4U, size);
}
std::vector<std::uint8_t> build_resource_tree(const std::vector<Pe64ResourceItem>& items,
                                               std::uint32_t rva, std::string& error) {
    using LangMap = std::map<std::uint16_t, const Pe64ResourceItem*>;
    using NameMap = std::map<std::uint16_t, LangMap>;
    std::map<std::uint16_t, NameMap> tree;
    std::size_t data_bytes = 0U;
    for (const auto& item : items) {
        if (item.type_id == 0U || item.name_id == 0U || item.data.empty() ||
            item.data.size() > std::numeric_limits<std::uint32_t>::max()) {
            error = "Resource type/name IDs must be non-zero and resource payloads must be non-empty."; return {};
        }
        auto& languages = tree[item.type_id][item.name_id];
        if (!languages.emplace(item.language_id, &item).second) {
            error = "Duplicate numeric type/name/language resource key."; return {};
        }
        if (data_bytes > kMaximumAdvancedBytes - std::min(item.data.size(), kMaximumAdvancedBytes)) {
            error = "Combined resource payload exceeds the 16-MiB safety limit."; return {};
        }
        data_bytes += item.data.size();
    }
    if (items.size() > kMaximumAdvancedEntries) { error = "Resource count exceeds 4096."; return {}; }
    std::vector<std::uint8_t> out(16U + tree.size() * 8U, 0U);
    const auto reserve_directory = [&out](std::size_t entries) -> std::size_t {
        const std::size_t offset = out.size(); out.resize(offset + 16U + entries * 8U, 0U);
        put16(out, offset + 12U, 0U); put16(out, offset + 14U, static_cast<std::uint16_t>(entries));
        return offset;
    };
    put16(out, 14U, static_cast<std::uint16_t>(tree.size()));
    std::size_t root_entry = 16U;
    for (const auto& type_pair : tree) {
        const std::size_t type_dir = reserve_directory(type_pair.second.size());
        put32(out, root_entry, type_pair.first); put32(out, root_entry + 4U, 0x80000000U | static_cast<std::uint32_t>(type_dir));
        root_entry += 8U;
        std::size_t name_entry = type_dir + 16U;
        for (const auto& name_pair : type_pair.second) {
            const std::size_t lang_dir = reserve_directory(name_pair.second.size());
            put32(out, name_entry, name_pair.first); put32(out, name_entry + 4U, 0x80000000U | static_cast<std::uint32_t>(lang_dir));
            name_entry += 8U;
            std::size_t lang_entry = lang_dir + 16U;
            for (const auto& lang_pair : name_pair.second) {
                const auto& item = *lang_pair.second;
                const std::size_t data_entry = out.size(); out.resize(data_entry + 16U, 0U);
                const std::size_t payload_offset = out.size();
                out.insert(out.end(), item.data.begin(), item.data.end());
                if (payload_offset > std::numeric_limits<std::uint32_t>::max() ||
                    rva > std::numeric_limits<std::uint32_t>::max() - payload_offset) {
                    error = "Resource data RVA exceeds PE32 range."; return {};
                }
                put32(out, data_entry, rva + static_cast<std::uint32_t>(payload_offset));
                put32(out, data_entry + 4U, static_cast<std::uint32_t>(item.data.size()));
                put32(out, data_entry + 8U, item.code_page);
                put32(out, data_entry + 12U, 0U);
                put32(out, lang_entry, lang_pair.first); put32(out, lang_entry + 4U, static_cast<std::uint32_t>(data_entry));
                lang_entry += 8U;
                while ((out.size() & 3U) != 0U) out.push_back(0U);
                if (out.size() > kMaximumAdvancedBytes) { error = "Resource directory exceeds 16 MiB."; return {}; }
            }
        }
    }
    if (out.size() > kMaximumAdvancedBytes || out.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "Resource directory exceeds the supported PE range."; return {};
    }
    return out;
}
struct DelayLibrary { std::string key; std::string name; std::uint32_t timestamp = 0U; std::vector<const Pe64DelayImport*> items; };
std::vector<std::uint8_t> build_delay_imports(const std::vector<Pe64DelayImport>& imports,
                                               std::uint32_t rva, std::string& error) {
    if (imports.empty()) return {};
    if (imports.size() > kMaximumAdvancedEntries) { error = "Delay import count exceeds 4096."; return {}; }
    std::vector<DelayLibrary> libs; std::map<std::string, std::size_t> by_name;
    std::set<std::string> keys;
    for (const auto& entry : imports) {
        const auto& item = entry.import;
        if (!printable_ascii(item.dll_name, 260U)) { error = "Delay-import DLL name is invalid."; return {}; }
        if (item.kind == Pe64ImportKind::ByName) {
            if (!printable_ascii(item.symbol_name, 4096U) || item.ordinal != 0U) { error = "Delay import-by-name record is invalid."; return {}; }
        } else if (item.kind == Pe64ImportKind::ByOrdinal) {
            if (!item.symbol_name.empty() || item.hint != 0U || item.ordinal == 0U) { error = "Delay import-by-ordinal record is invalid."; return {}; }
        } else { error = "Unknown delay import kind."; return {}; }
        const std::string lib_key = lower_ascii(item.dll_name);
        auto found = by_name.find(lib_key);
        if (found == by_name.end()) {
            const std::size_t idx = libs.size(); by_name.emplace(lib_key, idx);
            libs.push_back(DelayLibrary{lib_key, item.dll_name, entry.timestamp, {}}); found = by_name.find(lib_key);
        } else if (libs[found->second].timestamp != entry.timestamp) {
            error = "Delay imports for one DLL must use one deterministic timestamp."; return {};
        }
        std::string key = lib_key + "!" + std::to_string(static_cast<unsigned>(item.kind)) + "!" + item.symbol_name + "!" + std::to_string(item.ordinal);
        if (!keys.insert(key).second) { error = "Duplicate delay import."; return {}; }
        libs[found->second].items.push_back(&entry);
    }
    std::vector<std::uint8_t> out((libs.size() + 1U) * 32U, 0U);
    struct Layout { std::size_t module = 0U; std::size_t iat = 0U; std::size_t intab = 0U; std::vector<std::size_t> names; };
    std::vector<Layout> layouts(libs.size());
    for (std::size_t i = 0U; i < libs.size(); ++i) {
        while ((out.size() & 7U) != 0U) out.push_back(0U);
        layouts[i].module = out.size(); out.resize(out.size() + 8U, 0U);
        layouts[i].iat = out.size(); out.resize(out.size() + (libs[i].items.size() + 1U) * 8U, 0U);
        layouts[i].intab = out.size(); out.resize(out.size() + (libs[i].items.size() + 1U) * 8U, 0U);
    }
    for (std::size_t i = 0U; i < libs.size(); ++i) {
        const std::size_t desc = i * 32U;
        const auto rva32 = [&rva, &error](std::size_t off, std::uint32_t& value) {
            if (off > std::numeric_limits<std::uint32_t>::max() - rva) { error = "Delay-import RVA overflows."; return false; }
            value = rva + static_cast<std::uint32_t>(off); return true;
        };
        std::uint32_t dll_rva = 0U, module_rva = 0U, iat_rva = 0U, int_rva = 0U;
        const std::size_t dll_off = out.size(); append_string(out, libs[i].name);
        if (!rva32(dll_off, dll_rva) || !rva32(layouts[i].module, module_rva) ||
            !rva32(layouts[i].iat, iat_rva) || !rva32(layouts[i].intab, int_rva)) return {};
        put32(out, desc, 1U); put32(out, desc + 4U, dll_rva); put32(out, desc + 8U, module_rva);
        put32(out, desc + 12U, iat_rva); put32(out, desc + 16U, int_rva);
        put32(out, desc + 28U, libs[i].timestamp);
        for (std::size_t j = 0U; j < libs[i].items.size(); ++j) {
            const auto& item = libs[i].items[j]->import;
            std::uint64_t thunk = 0U;
            if (item.kind == Pe64ImportKind::ByOrdinal) thunk = 0x8000000000000000ULL | item.ordinal;
            else {
                while ((out.size() & 1U) != 0U) out.push_back(0U);
                const std::size_t name_off = out.size(); append16(out, item.hint); append_string(out, item.symbol_name);
                std::uint32_t name_rva = 0U; if (!rva32(name_off, name_rva)) return {};
                thunk = name_rva;
            }
            put64(out, layouts[i].iat + j * 8U, thunk);
            put64(out, layouts[i].intab + j * 8U, thunk);
        }
        if (out.size() > kMaximumAdvancedBytes) { error = "Delay-import directory exceeds 16 MiB."; return {}; }
    }
    if (out.size() > kMaximumAdvancedBytes || out.size() > std::numeric_limits<std::uint32_t>::max() - rva) {
        error = "Delay-import section exceeds the PE RVA range."; return {};
    }
    return out;
}
std::vector<std::uint8_t> build_bound_imports(const std::vector<Pe64BoundImport>& imports, std::string& error) {
    if (imports.empty()) return {};
    if (imports.size() > kMaximumAdvancedEntries) { error = "Bound import count exceeds 4096."; return {}; }
    std::set<std::string> unique;
    const std::size_t descriptor_bytes = (imports.size() + 1U) * 8U;
    std::vector<std::uint8_t> out(descriptor_bytes, 0U);
    for (std::size_t i = 0U; i < imports.size(); ++i) {
        if (!printable_ascii(imports[i].dll_name, 260U)) { error = "Bound-import DLL name is invalid."; return {}; }
        const std::string key = lower_ascii(imports[i].dll_name);
        if (!unique.insert(key).second) { error = "Duplicate bound-import DLL name."; return {}; }
        if (out.size() > std::numeric_limits<std::uint16_t>::max()) { error = "Bound-import name offset exceeds 16 bits."; return {}; }
        const auto name_offset = static_cast<std::uint16_t>(out.size());
        append_string(out, imports[i].dll_name);
        const std::size_t desc = i * 8U;
        put32(out, desc, imports[i].timestamp); put16(out, desc + 4U, name_offset); put16(out, desc + 6U, 0U);
    }
    if (out.size() > kMaximumAdvancedBytes) { error = "Bound-import directory exceeds 16 MiB."; return {}; }
    return out;
}
std::vector<std::uint8_t> build_relocations(const Pe64DllImage& image,
                                             const std::vector<std::uint32_t>& added_rvas,
                                             std::string& error) {
    std::vector<std::uint8_t> out;
    std::size_t pe = 0U, optional = 0U, table = 0U; std::uint16_t count = 0U; std::uint32_t headers = 0U;
    if (!read_pe_layout(image, pe, optional, table, count, headers)) { error = "Invalid PE while merging base relocations."; return {}; }
    const std::uint32_t old_rva = get32(image.bytes, optional + kDataDirectoryOffset + 5U * 8U);
    const std::uint32_t old_size = get32(image.bytes, optional + kDataDirectoryOffset + 5U * 8U + 4U);
    if (old_rva != 0U && old_size != 0U) {
        const auto off = rva_to_file_offset(image, old_rva, old_size);
        if (off == std::numeric_limits<std::size_t>::max()) { error = "Existing relocation directory is not file-backed."; return {}; }
        out.insert(out.end(), image.bytes.begin() + static_cast<std::ptrdiff_t>(off),
                   image.bytes.begin() + static_cast<std::ptrdiff_t>(off + old_size));
    }
    std::map<std::uint32_t, std::vector<std::uint16_t>> pages;
    for (const auto rva : added_rvas) {
        const std::uint32_t page = rva & 0xfffff000U;
        const auto offset = static_cast<std::uint16_t>(rva & 0x0fffU);
        pages[page].push_back(static_cast<std::uint16_t>((kRelocDir64 << 12U) | offset));
    }
    for (auto& page : pages) {
        auto& entries = page.second;
        std::sort(entries.begin(), entries.end());
        if (std::adjacent_find(entries.begin(), entries.end()) != entries.end()) {
            error = "Duplicate DIR64 relocation target in advanced load configuration."; return {};
        }
        if ((entries.size() & 1U) != 0U) entries.push_back(0U);
        append32(out, page.first); append32(out, static_cast<std::uint32_t>(8U + entries.size() * 2U));
        for (const auto entry : entries) append16(out, entry);
    }
    if (out.empty()) { error = "Advanced relocation directory unexpectedly empty."; return {}; }
    if (out.size() > kMaximumAdvancedBytes || out.size() > std::numeric_limits<std::uint32_t>::max()) {
        error = "Merged relocation directory exceeds the supported range."; return {};
    }
    return out;
}

struct SectionMapEntry { Pe64SectionKind kind = Pe64SectionKind::Text; std::size_t offset = 0U; };
struct SectionMapKey { std::string object; ObjectSectionId section = InvalidObjectSectionId;
    bool operator<(const SectionMapKey& other) const { return std::tie(object, section) < std::tie(other.object, other.section); } };

} // namespace

Pe64DllAdvancedResult write_pe64_dll_image_advanced(
    const Pe64DllSpec& base, const Pe64DllAdvancedSpec& advanced) {
    Pe64DllAdvancedResult result;
    auto base_result = write_pe64_dll_image(base);
    if (!base_result.succeeded()) { result.diagnostics = std::move(base_result.diagnostics); return result; }
    result.image = std::move(base_result.image);
    result.advanced.resource_count = advanced.resources.size();
    result.advanced.delay_import_count = advanced.delay_imports.size();
    result.advanced.bound_import_count = advanced.bound_imports.size();

    std::size_t pe = 0U, optional = 0U, section_table = 0U; std::uint16_t count = 0U; std::uint32_t headers = 0U;
    if (!read_pe_layout(result.image, pe, optional, section_table, count, headers)) {
        diagnostic(result.diagnostics, "XPE64ADV0001", "Base PE image header validation failed.");
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
    }
    std::string error;
    const auto add_payload = [&](const std::string& name, const std::vector<std::uint8_t>& payload,
                                 std::uint32_t characteristics, std::uint32_t& out_rva) -> bool {
        if (payload.empty()) return true;
        return append_section(result.image, name, payload, characteristics, base.section_alignment,
                              base.file_alignment, base.maximum_file_size, 0U, out_rva, error);
    };

    if (!advanced.resources.empty()) {
        std::uint32_t rva = 0U;
        if (!next_section_rva(result.image, base.section_alignment, rva)) {
            diagnostic(result.diagnostics, "XPE64ADV0002", "Unable to allocate .rsrc RVA.");
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
        }
        auto payload = build_resource_tree(advanced.resources, rva, error);
        if (payload.empty() || !add_payload(".rsrc", payload, kReadOnly, result.advanced.resource_directory_rva)) {
            diagnostic(result.diagnostics, "XPE64ADV0003", error.empty() ? "Resource tree serialization failed." : error);
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
        }
        result.advanced.resource_directory_size = static_cast<std::uint32_t>(payload.size());
        set_data_directory(result.image, 2U, result.advanced.resource_directory_rva, result.advanced.resource_directory_size);
    }

    if (!advanced.bound_imports.empty()) {
        auto payload = build_bound_imports(advanced.bound_imports, error);
        if (payload.empty() || !add_payload(".bnd", payload, kReadOnly, result.advanced.bound_import_directory_rva)) {
            diagnostic(result.diagnostics, "XPE64ADV0004", error.empty() ? "Bound-import serialization failed." : error);
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
        }
        result.advanced.bound_import_directory_size = static_cast<std::uint32_t>(payload.size());
        set_data_directory(result.image, 11U, result.advanced.bound_import_directory_rva, result.advanced.bound_import_directory_size);
    }

    if (!advanced.delay_imports.empty()) {
        std::uint32_t rva = 0U;
        if (!next_section_rva(result.image, base.section_alignment, rva)) {
            diagnostic(result.diagnostics, "XPE64ADV0005", "Unable to allocate .didat RVA.");
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
        }
        auto payload = build_delay_imports(advanced.delay_imports, rva, error);
        if (payload.empty() || !add_payload(".didat", payload, kReadWrite, result.advanced.delay_import_directory_rva)) {
            diagnostic(result.diagnostics, "XPE64ADV0006", error.empty() ? "Delay-import serialization failed." : error);
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
        }
        result.advanced.delay_import_directory_size = static_cast<std::uint32_t>(payload.size());
        set_data_directory(result.image, 13U, result.advanced.delay_import_directory_rva, result.advanced.delay_import_directory_size);
    }

    if (advanced.load_config.enabled) {
        std::vector<std::uint8_t> payload(kLoadConfigSize64, 0U);
        put32(payload, 0U, static_cast<std::uint32_t>(kLoadConfigSize64));
        std::vector<std::uint32_t> relocation_rvas;
        const auto va = [&base](std::uint32_t rva, std::uint64_t& value) {
            if (base.image_base > std::numeric_limits<std::uint64_t>::max() - rva) return false;
            value = base.image_base + rva; return true;
        };
        std::uint32_t text_rva = 0U;
        const auto text_it = std::find_if(result.image.sections.begin(), result.image.sections.end(),
            [](const Pe64DllSectionInfo& section) { return section.name == ".text"; });
        if (text_it == result.image.sections.end()) {
            diagnostic(result.diagnostics, "XPE64ADV0007", "Load configuration requires a .text section.");
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
        }
        text_rva = text_it->virtual_address;
        if (advanced.load_config.has_security_cookie) {
            if ((advanced.load_config.security_cookie_data_offset & 7U) != 0U ||
                advanced.load_config.security_cookie_data_offset > base.data.size() ||
                base.data.size() - advanced.load_config.security_cookie_data_offset < 8U) {
                diagnostic(result.diagnostics, "XPE64ADV0008", "Security cookie pointer slot must be an aligned 8-byte cell in .data.");
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
            }
        }
        std::vector<std::size_t> guard_offsets = advanced.load_config.guard_cf_function_offsets;
        if (advanced.load_config.guard_cf_enabled) {
            if (guard_offsets.empty() || guard_offsets.size() > kMaximumAdvancedEntries) {
                diagnostic(result.diagnostics, "XPE64ADV0009", "GuardCF requires 1..4096 function table entries.");
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
            }
            const auto data_it = std::find_if(result.image.sections.begin(), result.image.sections.end(),
                [](const Pe64DllSectionInfo& section) { return section.name == ".data"; });
            const auto valid_pointer_cell = [&base](std::size_t offset) {
                return (offset & 7U) == 0U && offset <= base.data.size() && base.data.size() - offset >= 8U;
            };
            const std::size_t check_cell = advanced.load_config.guard_cf_check_pointer_data_offset;
            const std::size_t dispatch_cell = advanced.load_config.guard_cf_dispatch_pointer_data_offset;
            const std::size_t check_target = advanced.load_config.guard_cf_check_target_text_offset;
            const std::size_t dispatch_target = advanced.load_config.guard_cf_dispatch_target_text_offset;
            if (data_it == result.image.sections.end() || !valid_pointer_cell(check_cell) ||
                !valid_pointer_cell(dispatch_cell) || check_cell == dispatch_cell ||
                check_target >= base.text.size() || dispatch_target >= base.text.size() ||
                (advanced.load_config.has_security_cookie &&
                    (check_cell == advanced.load_config.security_cookie_data_offset ||
                     dispatch_cell == advanced.load_config.security_cookie_data_offset))) {
                diagnostic(result.diagnostics, "XPE64ADV0010", "GuardCF requires distinct aligned .data pointer cells, valid .text targets and no SecurityCookie overlap.");
                result.image = Pe64DllImage{};
                result.advanced = Pe64DllAdvancedInfo{}; return result;
            }
            for (const auto& relocation : base.absolute_relocations) {
                const std::size_t destination = relocation.destination_offset;
                const bool overlaps_check = destination < check_cell + 8U && check_cell < destination + 8U;
                const bool overlaps_dispatch = destination < dispatch_cell + 8U && dispatch_cell < destination + 8U;
                if (overlaps_check || overlaps_dispatch) {
                    diagnostic(result.diagnostics, "XPE64ADV0010", "GuardCF pointer cells must not overlap user DIR64 relocation destinations.");
                    result.image = Pe64DllImage{};
                    result.advanced = Pe64DllAdvancedInfo{}; return result;
                }
            }
            std::sort(guard_offsets.begin(), guard_offsets.end());
            if (std::adjacent_find(guard_offsets.begin(), guard_offsets.end()) != guard_offsets.end() ||
                guard_offsets.back() >= base.text.size()) {
                diagnostic(result.diagnostics, "XPE64ADV0011", "GuardCF function offsets must be unique and inside .text.");
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
            }
        } else if (!guard_offsets.empty()) {
            diagnostic(result.diagnostics, "XPE64ADV0012", "GuardCF function offsets require GuardCF to be enabled.");
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
        }
        while ((payload.size() & 3U) != 0U) payload.push_back(0U);
        const std::size_t table_offset = payload.size();
        for (const auto off : guard_offsets) append32(payload, text_rva + static_cast<std::uint32_t>(off));
        if (payload.size() > kMaximumAdvancedBytes) {
            diagnostic(result.diagnostics, "XPE64ADV0013", "Load configuration plus GuardCF table exceeds 16 MiB.");
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
        }
        std::uint32_t lcfg_rva = 0U;
        if (!next_section_rva(result.image, base.section_alignment, lcfg_rva)) {
            diagnostic(result.diagnostics, "XPE64ADV0014", "Unable to allocate .lcfg RVA.");
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
        }
        const auto write_va_field = [&](std::size_t field, std::uint32_t target_rva) -> bool {
            std::uint64_t value = 0U;
            if (!va(target_rva, value)) return false;
            put64(payload, field, value);
            relocation_rvas.push_back(lcfg_rva + static_cast<std::uint32_t>(field));
            return true;
        };
        if (advanced.load_config.has_security_cookie) {
            const auto data_it = std::find_if(result.image.sections.begin(), result.image.sections.end(),
                [](const Pe64DllSectionInfo& section) { return section.name == ".data"; });
            if (data_it == result.image.sections.end() ||
                !write_va_field(0x58U, data_it->virtual_address + static_cast<std::uint32_t>(advanced.load_config.security_cookie_data_offset))) {
                diagnostic(result.diagnostics, "XPE64ADV0015", "Security cookie VA could not be represented.");
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
            }
        }
        if (advanced.load_config.guard_cf_enabled) {
            const auto data_it = std::find_if(result.image.sections.begin(), result.image.sections.end(),
                [](const Pe64DllSectionInfo& section) { return section.name == ".data"; });
            const std::size_t check_cell = advanced.load_config.guard_cf_check_pointer_data_offset;
            const std::size_t dispatch_cell = advanced.load_config.guard_cf_dispatch_pointer_data_offset;
            std::uint64_t check_target_va = 0U, dispatch_target_va = 0U;
            if (data_it == result.image.sections.end() ||
                !write_va_field(0x70U, data_it->virtual_address + static_cast<std::uint32_t>(check_cell)) ||
                !write_va_field(0x78U, data_it->virtual_address + static_cast<std::uint32_t>(dispatch_cell)) ||
                !write_va_field(0x80U, lcfg_rva + static_cast<std::uint32_t>(table_offset)) ||
                !va(text_rva + static_cast<std::uint32_t>(advanced.load_config.guard_cf_check_target_text_offset), check_target_va) ||
                !va(text_rva + static_cast<std::uint32_t>(advanced.load_config.guard_cf_dispatch_target_text_offset), dispatch_target_va)) {
                diagnostic(result.diagnostics, "XPE64ADV0016", "GuardCF preferred-base VA calculation overflowed or a pointer cell was unavailable.");
                result.image = Pe64DllImage{};
                result.advanced = Pe64DllAdvancedInfo{}; return result;
            }
            if (static_cast<std::uint64_t>(data_it->raw_offset) + dispatch_cell + 8U > result.image.bytes.size()) {
                diagnostic(result.diagnostics, "XPE64ADV0016", "GuardCF pointer cells are not file-backed initialized .data.");
                result.image = Pe64DllImage{};
                result.advanced = Pe64DllAdvancedInfo{}; return result;
            }
            put64(result.image.bytes, static_cast<std::size_t>(data_it->raw_offset) + check_cell, check_target_va);
            put64(result.image.bytes, static_cast<std::size_t>(data_it->raw_offset) + dispatch_cell, dispatch_target_va);
            relocation_rvas.push_back(data_it->virtual_address + static_cast<std::uint32_t>(check_cell));
            relocation_rvas.push_back(data_it->virtual_address + static_cast<std::uint32_t>(dispatch_cell));
            put64(payload, 0x88U, guard_offsets.size());
            put32(payload, 0x90U, 0x00000500U); // CF_INSTRUMENTED | FUNCTION_TABLE_PRESENT
            const std::uint16_t dll_chars = get16(result.image.bytes, optional + 70U);
            put16(result.image.bytes, optional + 70U, static_cast<std::uint16_t>(dll_chars | 0x4000U)); // IMAGE_DLLCHARACTERISTICS_GUARD_CF
        }
        if (!append_section(result.image, ".lcfg", payload, kReadOnly, base.section_alignment,
                            base.file_alignment, base.maximum_file_size, lcfg_rva,
                            result.advanced.load_config_directory_rva, error)) {
            diagnostic(result.diagnostics, "XPE64ADV0017", error);
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
        }
        result.advanced.load_config_directory_size = static_cast<std::uint32_t>(kLoadConfigSize64);
        result.advanced.guard_cf_function_count = guard_offsets.size();
        set_data_directory(result.image, 10U, result.advanced.load_config_directory_rva,
                           result.advanced.load_config_directory_size);
        if (!relocation_rvas.empty()) {
            auto reloc_payload = build_relocations(result.image, relocation_rvas, error);
            std::uint32_t reloc_rva = 0U;
            if (reloc_payload.empty() || !add_payload(".relx", reloc_payload, kRelocRead, reloc_rva)) {
                diagnostic(result.diagnostics, "XPE64ADV0018", error.empty() ? "Merged DIR64 relocation generation failed." : error);
            result.image = Pe64DllImage{};
            result.advanced = Pe64DllAdvancedInfo{}; return result;
            }
            result.advanced.advanced_relocation_directory_rva = reloc_rva;
            result.advanced.advanced_relocation_directory_size = static_cast<std::uint32_t>(reloc_payload.size());
            result.image.relocation_directory_rva = reloc_rva;
            result.image.relocation_directory_size = result.advanced.advanced_relocation_directory_size;
            set_data_directory(result.image, 5U, reloc_rva, result.advanced.advanced_relocation_directory_size);
            const std::uint16_t coff_characteristics = get16(result.image.bytes, pe + 22U);
            put16(result.image.bytes, pe + 22U, static_cast<std::uint16_t>(coff_characteristics & ~0x0001U));
            const std::uint16_t dll_chars = get16(result.image.bytes, optional + 70U);
            put16(result.image.bytes, optional + 70U, static_cast<std::uint16_t>(dll_chars | 0x0040U));
            result.image.has_dynamic_base = true;
            result.image.base_relocation_count += relocation_rvas.size();
        }
    }
    return result;
}

Pe64CoffDllLinkResult link_coff_objects_to_pe64_dll(const Pe64CoffDllLinkSpec& input) {
    Pe64CoffDllLinkResult result;
    std::vector<ObjectFile> object_inputs = input.input_objects;
    std::vector<StaticArchive> archive_inputs = input.archives;
    for (std::size_t index = 0U; index < input.coff_object_files.size(); ++index) {
        const auto decoded = read_binary_object(input.coff_object_files[index]);
        if (!decoded.succeeded() || decoded.object_file.descriptor.format != ObjectFileFormat::COFF ||
            decoded.object_file.descriptor.architecture != ObjectArchitecture::X86_64 ||
            decoded.object_file.descriptor.operating_system != ObjectOperatingSystem::Windows) {
            result.diagnostics = decoded.diagnostics;
            if (result.diagnostics.empty()) diagnostic(result.diagnostics, "XPE64COFF0020", "Raw input is not a supported AMD64 Windows COFF object file.");
            return result;
        }
        ObjectFile object = decoded.object_file;
        object.object_id = "coff_obj_input_" + std::to_string(index);
        object_inputs.push_back(std::move(object));
    }
    for (std::size_t index = 0U; index < input.coff_library_files.size(); ++index) {
        const auto decoded = read_object_archive(input.coff_library_files[index], "coff_lib_input_" + std::to_string(index));
        if (!decoded.diagnostics.empty() || decoded.format != ArchiveFormat::CoffLib) {
            result.diagnostics = decoded.diagnostics;
            if (result.diagnostics.empty()) diagnostic(result.diagnostics, "XPE64COFF0021", "Raw archive is not a supported Microsoft COFF LIB container.");
            return result;
        }
        StaticArchive archive = decoded.archive;
        for (std::size_t member = 0U; member < archive.members.size(); ++member) {
            archive.members[member].object_file.object_id = archive.archive_id + "_member_" + std::to_string(member);
        }
        archive_inputs.push_back(std::move(archive));
    }
    LinkerOptions options;
    options.image_base = input.image_base;
    options.maximum_image_size = input.maximum_image_size;
    options.extract_archive_members = true;
    options.allow_unresolved_weak = false;
    options.entry_symbol = input.entry_symbol;
    for (const auto& exported : input.exports) options.preserved_symbols.push_back(exported.symbol_name);
    result.link = link_object_files(make_coff_x86_64_object_target(), object_inputs, archive_inputs, options);
    if (!result.link.succeeded()) {
        result.diagnostics = result.link.diagnostics; return result;
    }
    result.selected_object_count = result.link.link_plan.selected_object_ids.size();
    result.extracted_archive_member_count = result.link.link_plan.extracted_archive_members.size();
    result.applied_relocation_count = result.link.image.applied_relocations.size();

    std::vector<std::uint8_t> text, rodata, data;
    std::map<SectionMapKey, SectionMapEntry> map;
    const auto append_aligned = [](std::vector<std::uint8_t>& target, const LinkedImageSection& source,
                                   std::size_t& offset) -> bool {
        const auto alignment = source.alignment;
        if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) return false;
        const std::size_t mask = alignment - 1U;
        if (target.size() > std::numeric_limits<std::size_t>::max() - mask) return false;
        const std::size_t aligned = (target.size() + mask) & ~mask;
        target.resize(aligned, 0U); offset = aligned;
        if (source.memory_size > std::numeric_limits<std::size_t>::max() - target.size()) return false;
        target.resize(target.size() + source.memory_size, 0U);
        if (source.kind != ObjectSectionKind::Bss) {
            if (source.contents.size() != source.memory_size) return false;
            std::copy(source.contents.begin(), source.contents.end(), target.begin() + static_cast<std::ptrdiff_t>(offset));
        }
        return true;
    };
    for (const auto& section : result.link.image.sections) {
        std::vector<std::uint8_t>* target = nullptr;
        Pe64SectionKind pe_kind = Pe64SectionKind::Text;
        if (section.kind == ObjectSectionKind::Text) { target = &text; pe_kind = Pe64SectionKind::Text; }
        else if (section.kind == ObjectSectionKind::ReadOnlyData || section.kind == ObjectSectionKind::Metadata) {
            target = &rodata; pe_kind = Pe64SectionKind::ReadOnlyData;
        } else { target = &data; pe_kind = Pe64SectionKind::Data; }
        std::size_t offset = 0U;
        if (!append_aligned(*target, section, offset)) {
            diagnostic(result.diagnostics, "XPE64COFF0001", "COFF input section alignment, size or contents are invalid."); return result;
        }
        map.emplace(SectionMapKey{section.object_id, section.source_section_id}, SectionMapEntry{pe_kind, offset});
    }
    if (text.empty() || input.exports.empty()) {
        diagnostic(result.diagnostics, "XPE64COFF0002", "PE DLL link requires non-empty executable text and at least one export."); return result;
    }
    Pe64DllSpec base;
    base.dll_name = input.dll_name;
    base.image_base = input.image_base;
    base.text = std::move(text); base.read_only_data = std::move(rodata); base.data = std::move(data);
    base.imports = input.imports;
    base.runtime_functions = input.runtime_functions;
    for (const auto& requested : input.exports) {
        if (!printable_ascii(requested.symbol_name, 4096U) || requested.ordinal == 0U) {
            diagnostic(result.diagnostics, "XPE64COFF0003", "Requested COFF export name or ordinal is invalid."); return result;
        }
        const auto symbol = std::find_if(result.link.image.symbols.begin(), result.link.image.symbols.end(),
            [&requested](const LinkedImageSymbol& item) { return item.name == requested.symbol_name && item.binding != ObjectSymbolBinding::Local; });
        if (symbol == result.link.image.symbols.end()) {
            diagnostic(result.diagnostics, "XPE64COFF0004", "Requested PE export does not resolve to a global COFF symbol."); return result;
        }
        const auto section = std::find_if(result.link.image.sections.begin(), result.link.image.sections.end(),
            [&symbol](const LinkedImageSection& item) { return item.object_id == symbol->object_id && item.source_section_id == symbol->section_id; });
        if (section == result.link.image.sections.end()) {
            diagnostic(result.diagnostics, "XPE64COFF0005", "Exported COFF symbol section is absent after linking."); return result;
        }
        const auto mapped = map.find(SectionMapKey{symbol->object_id, symbol->section_id});
        if (mapped == map.end() || symbol->address < section->address) {
            diagnostic(result.diagnostics, "XPE64COFF0006", "Exported COFF symbol has no PE section mapping."); return result;
        }
        const std::uint64_t offset = static_cast<std::uint64_t>(mapped->second.offset) + (symbol->address - section->address);
        if (offset > std::numeric_limits<std::size_t>::max()) {
            diagnostic(result.diagnostics, "XPE64COFF0007", "Exported COFF symbol offset exceeds host range."); return result;
        }
        base.exports.push_back(Pe64DllExport{requested.symbol_name, requested.ordinal, mapped->second.kind,
            static_cast<std::size_t>(offset), false, {}});
    }
    if (!input.entry_symbol.empty()) {
        const auto symbol = std::find_if(result.link.image.symbols.begin(), result.link.image.symbols.end(),
            [&input](const LinkedImageSymbol& item) { return item.name == input.entry_symbol && item.binding != ObjectSymbolBinding::Local; });
        if (symbol == result.link.image.symbols.end() || symbol->kind != ObjectSymbolKind::Function) {
            diagnostic(result.diagnostics, "XPE64COFF0008", "Requested DLL entry symbol is not a resolved external function."); return result;
        }
        const auto mapped = map.find(SectionMapKey{symbol->object_id, symbol->section_id});
        const auto section = std::find_if(result.link.image.sections.begin(), result.link.image.sections.end(),
            [&symbol](const LinkedImageSection& item) { return item.object_id == symbol->object_id && item.source_section_id == symbol->section_id; });
        if (mapped == map.end() || section == result.link.image.sections.end() || mapped->second.kind != Pe64SectionKind::Text) {
            diagnostic(result.diagnostics, "XPE64COFF0009", "DLL entry symbol must be located in a mapped executable section."); return result;
        }
        base.has_entry_point = true;
        base.entry_point_offset = mapped->second.offset + static_cast<std::size_t>(symbol->address - section->address);
    }
    // PE DIR64 support currently models initialized .data destinations. Refuse
    // unsupported absolute-pointer placements instead of emitting wrong code.
    for (const auto& object_id : result.link.link_plan.selected_object_ids) {
        const auto object = std::find_if(object_inputs.begin(), object_inputs.end(), [object_id](const ObjectFile& item) { return item.object_id == object_id; });
        const ObjectFile* source = object == object_inputs.end() ? nullptr : &*object;
        if (source == nullptr) {
            for (const auto& archive : archive_inputs) for (const auto& member : archive.members)
                if (member.object_file.object_id == object_id) source = &member.object_file;
        }
        if (source == nullptr) { diagnostic(result.diagnostics, "XPE64COFF0010", "Selected COFF object is missing from link inputs."); return result; }
        for (const auto& relocation : source->relocations) {
            if (relocation.kind != ObjectRelocationKind::CoffAddress64) continue;
            const auto src_map = map.find(SectionMapKey{source->object_id, relocation.section_id});
            if (src_map == map.end() || src_map->second.kind != Pe64SectionKind::Data || relocation.width != 8U) {
                diagnostic(result.diagnostics, "XPE64COFF0011", "COFF ADDR64 relocation is supported only for 8-byte initialized .data destinations."); return result;
            }
            const auto resolved = std::find_if(result.link.image.symbols.begin(), result.link.image.symbols.end(),
                [&relocation](const LinkedImageSymbol& item) { return item.name == relocation.target_symbol; });
            if (resolved == result.link.image.symbols.end()) {
                diagnostic(result.diagnostics, "XPE64COFF0012", "COFF ADDR64 relocation target is unresolved."); return result;
            }
            const auto target_map = map.find(SectionMapKey{resolved->object_id, resolved->section_id});
            const auto target_section = std::find_if(result.link.image.sections.begin(), result.link.image.sections.end(),
                [&resolved](const LinkedImageSection& item) { return item.object_id == resolved->object_id && item.source_section_id == resolved->section_id; });
            if (target_map == map.end() || target_section == result.link.image.sections.end()) {
                diagnostic(result.diagnostics, "XPE64COFF0013", "COFF ADDR64 target has no PE mapping."); return result;
            }
            const std::size_t dst = src_map->second.offset + relocation.offset;
            const std::uint64_t target_offset = static_cast<std::uint64_t>(target_map->second.offset) + (resolved->address - target_section->address);
            base.absolute_relocations.push_back(Pe64AbsoluteRelocation{dst, target_map->second.kind,
                static_cast<std::size_t>(target_offset), relocation.addend});
        }
    }
    auto output = write_pe64_dll_image_advanced(base, input.advanced);
    if (!output.succeeded()) { result.diagnostics = std::move(output.diagnostics); return result; }

    // Reapply COFF/AMD64 REL32-family fixups against final PE section RVAs;
    // the in-memory linker layout differs from final page-aligned PE layout.
    const auto pe_section = [&output](Pe64SectionKind kind) -> const Pe64DllSectionInfo* {
        const char* name = kind == Pe64SectionKind::Text ? ".text" :
            (kind == Pe64SectionKind::ReadOnlyData ? ".rdata" : ".data");
        const auto found = std::find_if(output.image.sections.begin(), output.image.sections.end(),
            [name](const Pe64DllSectionInfo& item) { return item.name == name; });
        return found == output.image.sections.end() ? nullptr : &*found;
    };
    for (const auto& object_id : result.link.link_plan.selected_object_ids) {
        const ObjectFile* source = nullptr;
        for (const auto& item : object_inputs) if (item.object_id == object_id) source = &item;
        if (source == nullptr) for (const auto& archive : archive_inputs) for (const auto& member : archive.members)
            if (member.object_file.object_id == object_id) source = &member.object_file;
        if (source == nullptr) continue;
        for (const auto& relocation : source->relocations) {
            if (relocation.kind != ObjectRelocationKind::CoffRelative32 &&
                relocation.kind != ObjectRelocationKind::PcRelative32 &&
                relocation.kind != ObjectRelocationKind::PltRelative32) continue;
            const auto src_map = map.find(SectionMapKey{source->object_id, relocation.section_id});
            if (src_map == map.end() || relocation.width != 4U) {
                diagnostic(result.diagnostics, "XPE64COFF0014", "COFF REL32 source section or width is unsupported."); return result;
            }
            const auto target = std::find_if(result.link.image.symbols.begin(), result.link.image.symbols.end(),
                [&relocation](const LinkedImageSymbol& item) { return item.name == relocation.target_symbol; });
            if (target == result.link.image.symbols.end()) {
                diagnostic(result.diagnostics, "XPE64COFF0015", "COFF REL32 target symbol is unresolved."); return result;
            }
            const auto target_map = map.find(SectionMapKey{target->object_id, target->section_id});
            if (target_map == map.end()) { diagnostic(result.diagnostics, "XPE64COFF0016", "COFF REL32 target section is unmapped."); return result; }
            const auto* src_pe = pe_section(src_map->second.kind);
            const auto* dst_pe = pe_section(target_map->second.kind);
            const auto source_section = std::find_if(result.link.image.sections.begin(), result.link.image.sections.end(),
                [&src_map, &source](const LinkedImageSection& item) { return item.object_id == source->object_id && item.source_section_id == src_map->first.section; });
            const auto target_section = std::find_if(result.link.image.sections.begin(), result.link.image.sections.end(),
                [&target](const LinkedImageSection& item) { return item.object_id == target->object_id && item.source_section_id == target->section_id; });
            if (src_pe == nullptr || dst_pe == nullptr || source_section == result.link.image.sections.end() || target_section == result.link.image.sections.end()) {
                diagnostic(result.diagnostics, "XPE64COFF0017", "COFF REL32 PE section mapping is incomplete."); return result;
            }
            const std::uint64_t source_off = static_cast<std::uint64_t>(src_map->second.offset) + relocation.offset;
            const std::uint64_t target_off = static_cast<std::uint64_t>(target_map->second.offset) + (target->address - target_section->address);
            const std::int64_t place = static_cast<std::int64_t>(src_pe->virtual_address) + static_cast<std::int64_t>(source_off);
            const std::int64_t symbol = static_cast<std::int64_t>(dst_pe->virtual_address) + static_cast<std::int64_t>(target_off);
            const std::int64_t bias = relocation.kind == ObjectRelocationKind::CoffRelative32 ? 4 : 0;
            const std::int64_t value = symbol + relocation.addend - (place + bias);
            if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
                diagnostic(result.diagnostics, "XPE64COFF0018", "Final PE REL32 displacement exceeds signed 32-bit range."); return result;
            }
            const std::size_t raw = static_cast<std::size_t>(src_pe->raw_offset) + static_cast<std::size_t>(source_off);
            if (raw > output.image.bytes.size() || output.image.bytes.size() - raw < 4U) {
                diagnostic(result.diagnostics, "XPE64COFF0019", "Final PE REL32 patch lies outside file bytes."); return result;
            }
            put32(output.image.bytes, raw, static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
        }
    }
    result.output = std::move(output);
    return result;
}

} // namespace xfrontend
