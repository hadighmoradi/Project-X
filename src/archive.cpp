// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/archive.hpp"

#include "xfrontend/binary_object.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace xfrontend {
namespace {
constexpr std::array<std::uint8_t, 8U> ArMagic{{'!', '<', 'a', 'r', 'c', 'h', '>', '\n'}};
constexpr std::size_t ArHeaderSize = 60U;

void add_diag(std::vector<Diagnostic>& diagnostics, const char* code, const std::string& message) {
    diagnostics.push_back(Diagnostic{code, message, {}});
}

void put_u16_le(std::vector<std::uint8_t>& b, std::size_t p, std::uint16_t v) {
    b[p] = static_cast<std::uint8_t>(v & 0xffU);
    b[p + 1U] = static_cast<std::uint8_t>((v >> 8U) & 0xffU);
}
void put_u32_le(std::vector<std::uint8_t>& b, std::size_t p, std::uint32_t v) {
    for (unsigned i = 0U; i < 4U; ++i) b[p + i] = static_cast<std::uint8_t>((v >> (i * 8U)) & 0xffU);
}
void put_u32_be(std::vector<std::uint8_t>& b, std::size_t p, std::uint32_t v) {
    for (unsigned i = 0U; i < 4U; ++i) b[p + i] = static_cast<std::uint8_t>((v >> ((3U - i) * 8U)) & 0xffU);
}
void append_u16_le(std::vector<std::uint8_t>& b, std::uint16_t v) {
    const auto p = b.size(); b.resize(p + 2U); put_u16_le(b, p, v);
}
void append_u32_le(std::vector<std::uint8_t>& b, std::uint32_t v) {
    const auto p = b.size(); b.resize(p + 4U); put_u32_le(b, p, v);
}
void append_u32_be(std::vector<std::uint8_t>& b, std::uint32_t v) {
    const auto p = b.size(); b.resize(p + 4U); put_u32_be(b, p, v);
}

bool get_u16_le(const std::vector<std::uint8_t>& b, std::size_t p, std::uint16_t& v) {
    if (p > b.size() || b.size() - p < 2U) return false;
    v = static_cast<std::uint16_t>(b[p]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[p + 1U]) << 8U);
    return true;
}
bool get_u32_le(const std::vector<std::uint8_t>& b, std::size_t p, std::uint32_t& v) {
    if (p > b.size() || b.size() - p < 4U) return false;
    v = static_cast<std::uint32_t>(b[p]) |
        (static_cast<std::uint32_t>(b[p + 1U]) << 8U) |
        (static_cast<std::uint32_t>(b[p + 2U]) << 16U) |
        (static_cast<std::uint32_t>(b[p + 3U]) << 24U);
    return true;
}
bool get_u32_be(const std::vector<std::uint8_t>& b, std::size_t p, std::uint32_t& v) {
    if (p > b.size() || b.size() - p < 4U) return false;
    v = (static_cast<std::uint32_t>(b[p]) << 24U) |
        (static_cast<std::uint32_t>(b[p + 1U]) << 16U) |
        (static_cast<std::uint32_t>(b[p + 2U]) << 8U) |
        static_cast<std::uint32_t>(b[p + 3U]);
    return true;
}

std::string trim_field(const std::vector<std::uint8_t>& bytes, std::size_t p, std::size_t n) {
    std::string value(reinterpret_cast<const char*>(bytes.data() + p), n);
    while (!value.empty() && value.back() == ' ') value.pop_back();
    return value;
}

bool parse_decimal(const std::vector<std::uint8_t>& b, std::size_t p, std::size_t n,
                   std::uint64_t& value) {
    value = 0U;
    std::size_t i = 0U;
    while (i < n && b[p + i] == ' ') ++i;
    if (i == n) return false;
    bool any = false;
    for (; i < n && b[p + i] != ' '; ++i) {
        const std::uint8_t ch = b[p + i];
        if (ch < '0' || ch > '9') return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) return false;
        value = value * 10U + digit;
        any = true;
    }
    return any;
}

bool write_decimal_field(std::vector<std::uint8_t>& out, std::size_t p, std::size_t n,
                         std::uint64_t value) {
    const std::string text = std::to_string(value);
    if (text.size() > n) return false;
    std::fill(out.begin() + static_cast<std::ptrdiff_t>(p),
              out.begin() + static_cast<std::ptrdiff_t>(p + n), static_cast<std::uint8_t>(' '));
    std::copy(text.begin(), text.end(), out.begin() + static_cast<std::ptrdiff_t>(p));
    return true;
}

struct NameInfo {
    std::string header_name;
    std::size_t long_offset = 0U;
    bool is_long = false;
};

bool needs_long_name(const std::string& name) {
    return name.size() > 15U || name.find(' ') != std::string::npos ||
           name.find('/') != std::string::npos || name.find('\n') != std::string::npos;
}

bool build_names(ArchiveFormat format, const std::vector<ArchiveContainerMember>& members,
                 std::vector<std::uint8_t>& long_names,
                 std::vector<NameInfo>& names, std::vector<Diagnostic>& diagnostics) {
    names.clear(); names.reserve(members.size());
    std::set<std::string> unique_names;
    for (const auto& member : members) {
        if (member.name.empty() || member.name.find('\0') != std::string::npos ||
            member.name.find('\n') != std::string::npos || member.name.find('\r') != std::string::npos) {
            add_diag(diagnostics, "XAR0001", "archive member name is empty or contains a forbidden NUL/newline");
            return false;
        }
        // Microsoft COFF import libraries intentionally repeat the DLL filename
        // for every object associated with that DLL. The linker uses this shared
        // archive-member name to resolve IMAGE_SYM_CLASS_SECTION references across
        // members while merging the .idata$ sections. GNU ar keeps the stricter
        // unique-name rule used by the ordinary object-archive path.
        if (format == ArchiveFormat::GnuAr && !unique_names.insert(member.name).second) {
            add_diag(diagnostics, "XAR0002", "GNU ar member names must be unique within a container");
            return false;
        }
        NameInfo info;
        if (needs_long_name(member.name)) {
            info.is_long = true;
            info.long_offset = long_names.size();
            info.header_name = "/" + std::to_string(info.long_offset);
            long_names.insert(long_names.end(), member.name.begin(), member.name.end());
            long_names.push_back('/'); long_names.push_back('\n');
        } else {
            info.header_name = member.name + "/";
        }
        names.push_back(std::move(info));
    }
    return true;
}

std::vector<std::uint8_t> make_header(const std::string& name, std::size_t payload_size) {
    std::vector<std::uint8_t> header(ArHeaderSize, static_cast<std::uint8_t>(' '));
    const std::size_t copy_count = std::min<std::size_t>(16U, name.size());
    std::copy(name.begin(), name.begin() + static_cast<std::ptrdiff_t>(copy_count), header.begin());
    write_decimal_field(header, 16U, 12U, 0U);
    write_decimal_field(header, 28U, 6U, 0U);
    write_decimal_field(header, 34U, 6U, 0U);
    std::fill(header.begin() + 40, header.begin() + 48, static_cast<std::uint8_t>(' '));
    const std::string mode = "100644"; // ar stores this field as octal text.
    std::copy(mode.begin(), mode.end(), header.begin() + 40);
    write_decimal_field(header, 48U, 10U, payload_size);
    header[58U] = '`'; header[59U] = '\n';
    return header;
}

bool append_member(std::vector<std::uint8_t>& out, const std::string& header_name,
                   const std::vector<std::uint8_t>& payload, std::uint64_t* header_offset = nullptr) {
    if (payload.size() > 9999999999ULL) return false;
    if (header_offset != nullptr) *header_offset = static_cast<std::uint64_t>(out.size());
    auto header = make_header(header_name, payload.size());
    out.insert(out.end(), header.begin(), header.end());
    out.insert(out.end(), payload.begin(), payload.end());
    if ((payload.size() & 1U) != 0U) out.push_back(static_cast<std::uint8_t>('\n'));
    return true;
}

std::vector<ArchiveSymbolIndexEntry> gather_symbols(const std::vector<ArchiveContainerMember>& members) {
    std::vector<ArchiveSymbolIndexEntry> entries;
    for (std::size_t i = 0U; i < members.size(); ++i) {
        for (const auto& symbol : members[i].defined_symbols) {
            if (!symbol.empty() && symbol.find('\0') == std::string::npos) {
                entries.push_back(ArchiveSymbolIndexEntry{symbol, i});
            }
        }
    }
    std::stable_sort(entries.begin(), entries.end(), [](const ArchiveSymbolIndexEntry& left,
                                                         const ArchiveSymbolIndexEntry& right) {
        if (left.symbol_name != right.symbol_name) return left.symbol_name < right.symbol_name;
        return left.member_index < right.member_index;
    });
    return entries;
}

bool append_c_string(std::vector<std::uint8_t>& out, const std::string& value) {
    if (value.find('\0') != std::string::npos) return false;
    out.insert(out.end(), value.begin(), value.end()); out.push_back(0U); return true;
}

std::vector<std::uint8_t> make_gnu_index(const std::vector<ArchiveSymbolIndexEntry>& symbols,
                                         const std::vector<std::uint64_t>& member_offsets,
                                         std::vector<Diagnostic>& diagnostics) {
    std::vector<std::uint8_t> out;
    if (symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        add_diag(diagnostics, "XAR0003", "GNU ar symbol count exceeds 32-bit index capacity"); return {};
    }
    append_u32_be(out, static_cast<std::uint32_t>(symbols.size()));
    for (const auto& symbol : symbols) {
        if (symbol.member_index >= member_offsets.size() || member_offsets[symbol.member_index] > std::numeric_limits<std::uint32_t>::max()) {
            add_diag(diagnostics, "XAR0004", "GNU ar member offset exceeds 32-bit symbol index capacity"); return {};
        }
        append_u32_be(out, static_cast<std::uint32_t>(member_offsets[symbol.member_index]));
    }
    for (const auto& symbol : symbols) {
        if (!append_c_string(out, symbol.symbol_name)) {
            add_diag(diagnostics, "XAR0005", "archive symbol name contains NUL"); return {};
        }
    }
    return out;
}

std::vector<std::uint8_t> make_coff_first_index(const std::vector<ArchiveSymbolIndexEntry>& symbols,
                                                const std::vector<std::uint64_t>& member_offsets,
                                                std::vector<Diagnostic>& diagnostics) {
    std::vector<std::uint8_t> out;
    if (symbols.size() > std::numeric_limits<std::uint32_t>::max()) {
        add_diag(diagnostics, "XAR0006", "COFF archive symbol count exceeds 32-bit index capacity"); return {};
    }
    append_u32_be(out, static_cast<std::uint32_t>(symbols.size()));
    for (const auto& symbol : symbols) {
        if (symbol.member_index >= member_offsets.size() || member_offsets[symbol.member_index] > std::numeric_limits<std::uint32_t>::max()) {
            add_diag(diagnostics, "XAR0007", "COFF archive member offset exceeds 32-bit linker index capacity"); return {};
        }
        append_u32_be(out, static_cast<std::uint32_t>(member_offsets[symbol.member_index]));
    }
    for (const auto& symbol : symbols) {
        if (!append_c_string(out, symbol.symbol_name)) {
            add_diag(diagnostics, "XAR0008", "COFF archive symbol name contains NUL"); return {};
        }
    }
    return out;
}

std::vector<std::uint8_t> make_coff_second_index(const std::vector<ArchiveSymbolIndexEntry>& symbols,
                                                 const std::vector<std::uint64_t>& member_offsets,
                                                 std::vector<Diagnostic>& diagnostics) {
    std::vector<std::uint8_t> out;
    if (member_offsets.size() > std::numeric_limits<std::uint32_t>::max() ||
        symbols.size() > std::numeric_limits<std::uint32_t>::max() ||
        member_offsets.size() > std::numeric_limits<std::uint16_t>::max()) {
        add_diag(diagnostics, "XAR0009", "COFF second linker member exceeds supported member or symbol count"); return {};
    }
    append_u32_le(out, static_cast<std::uint32_t>(member_offsets.size()));
    for (const auto offset : member_offsets) {
        if (offset > std::numeric_limits<std::uint32_t>::max()) {
            add_diag(diagnostics, "XAR0010", "COFF second linker member offset exceeds 32-bit capacity"); return {};
        }
        append_u32_le(out, static_cast<std::uint32_t>(offset));
    }
    append_u32_le(out, static_cast<std::uint32_t>(symbols.size()));
    for (const auto& symbol : symbols) {
        if (symbol.member_index >= member_offsets.size()) {
            add_diag(diagnostics, "XAR0011", "COFF symbol index references an absent archive member"); return {};
        }
        append_u16_le(out, static_cast<std::uint16_t>(symbol.member_index + 1U));
    }
    for (const auto& symbol : symbols) {
        if (!append_c_string(out, symbol.symbol_name)) {
            add_diag(diagnostics, "XAR0012", "COFF archive symbol name contains NUL"); return {};
        }
    }
    return out;
}

bool decode_index_strings(const std::vector<std::uint8_t>& data, std::size_t offset,
                          std::size_t count, std::vector<std::string>& names) {
    names.clear(); names.reserve(count);
    for (std::size_t i = 0U; i < count; ++i) {
        if (offset >= data.size()) return false;
        auto end = offset;
        while (end < data.size() && data[end] != 0U) ++end;
        if (end == data.size()) return false;
        names.emplace_back(reinterpret_cast<const char*>(data.data() + offset), end - offset);
        offset = end + 1U;
    }
    return offset == data.size();
}

struct RawMember {
    std::string raw_name;
    std::string name;
    std::vector<std::uint8_t> contents;
    std::uint64_t header_offset = 0U;
    std::size_t long_offset = 0U;
    bool has_long_offset = false;
};

bool parse_index_offsets(const std::vector<std::uint8_t>& payload, bool big_endian,
                         std::size_t count, std::size_t offset,
                         std::vector<std::uint64_t>& member_offsets) {
    member_offsets.clear();
    if (count > (payload.size() - std::min(offset, payload.size())) / 4U) return false;
    for (std::size_t i = 0U; i < count; ++i) {
        std::uint32_t value = 0U;
        const bool ok = big_endian ? get_u32_be(payload, offset + i * 4U, value)
                                   : get_u32_le(payload, offset + i * 4U, value);
        if (!ok) return false;
        member_offsets.push_back(value);
    }
    return true;
}

} // namespace

ArchiveWriteResult write_archive_container(ArchiveFormat format,
    const std::vector<ArchiveContainerMember>& members, bool include_symbol_index) {
    ArchiveWriteResult result;
    if (format != ArchiveFormat::GnuAr && format != ArchiveFormat::CoffLib) {
        add_diag(result.diagnostics, "XAR0013", "unsupported archive container format"); return result;
    }
    if (format == ArchiveFormat::CoffLib && members.size() > std::numeric_limits<std::uint16_t>::max()) {
        add_diag(result.diagnostics, "XAR0014", "COFF LIB currently supports at most 65535 indexed members"); return result;
    }
    std::vector<std::uint8_t> long_names;
    std::vector<NameInfo> names;
    if (!build_names(format, members, long_names, names, result.diagnostics)) return result;
    const auto symbols = include_symbol_index ? gather_symbols(members) : std::vector<ArchiveSymbolIndexEntry>{};

    std::vector<std::uint8_t> index1;
    std::vector<std::uint8_t> index2;
    const std::size_t long_record_size = long_names.empty() ? 0U : ArHeaderSize + long_names.size() + (long_names.size() & 1U);
    std::size_t prefix = ArMagic.size();
    if (format == ArchiveFormat::GnuAr) {
        const std::size_t index_size = include_symbol_index ? 4U + symbols.size() * 4U : 0U;
        std::size_t string_bytes = 0U;
        for (const auto& symbol : symbols) string_bytes += symbol.symbol_name.size() + 1U;
        prefix += include_symbol_index ? ArHeaderSize + index_size + string_bytes + ((index_size + string_bytes) & 1U) : 0U;
    } else {
        std::size_t names_size = 0U;
        for (const auto& symbol : symbols) names_size += symbol.symbol_name.size() + 1U;
        const std::size_t first_size = include_symbol_index ? 4U + symbols.size() * 4U + names_size : 4U + names_size;
        const std::size_t second_size = include_symbol_index ? 4U + members.size() * 4U + 4U + symbols.size() * 2U + names_size : 8U + members.size() * 4U;
        prefix += ArHeaderSize + first_size + (first_size & 1U);
        prefix += ArHeaderSize + second_size + (second_size & 1U);
    }
    prefix += long_record_size;

    std::vector<std::uint64_t> member_offsets;
    member_offsets.reserve(members.size());
    std::size_t cursor = prefix;
    for (const auto& member : members) {
        if (member.contents.size() > 9999999999ULL || cursor > std::numeric_limits<std::uint32_t>::max()) {
            add_diag(result.diagnostics, "XAR0015", "archive size exceeds supported member header or index range"); return result;
        }
        member_offsets.push_back(cursor);
        const std::size_t record_size = ArHeaderSize + member.contents.size() + (member.contents.size() & 1U);
        if (cursor > std::numeric_limits<std::size_t>::max() - record_size) {
            add_diag(result.diagnostics, "XAR0016", "archive size arithmetic overflows"); return result;
        }
        cursor += record_size;
    }

    if (include_symbol_index || format == ArchiveFormat::CoffLib) {
        if (format == ArchiveFormat::GnuAr) index1 = make_gnu_index(symbols, member_offsets, result.diagnostics);
        else {
            index1 = make_coff_first_index(symbols, member_offsets, result.diagnostics);
            index2 = make_coff_second_index(symbols, member_offsets, result.diagnostics);
        }
        if (!result.diagnostics.empty()) return result;
    }

    auto& out = result.bytes;
    out.insert(out.end(), ArMagic.begin(), ArMagic.end());
    if (format == ArchiveFormat::GnuAr) {
        if (include_symbol_index && !append_member(out, "/", index1)) {
            add_diag(result.diagnostics, "XAR0017", "unable to append GNU ar symbol index"); return result;
        }
    } else {
        if (!append_member(out, "/", index1) || !append_member(out, "/", index2)) {
            add_diag(result.diagnostics, "XAR0018", "unable to append COFF LIB linker members"); return result;
        }
    }
    if (!long_names.empty() && !append_member(out, "//", long_names)) {
        add_diag(result.diagnostics, "XAR0019", "unable to append archive long-name table"); return result;
    }
    for (std::size_t i = 0U; i < members.size(); ++i) {
        std::uint64_t actual_offset = 0U;
        if (!append_member(out, names[i].header_name, members[i].contents, &actual_offset)) {
            add_diag(result.diagnostics, "XAR0020", "unable to append archive object member"); return result;
        }
        if (actual_offset != member_offsets[i]) {
            add_diag(result.diagnostics, "XAR0021", "archive member offset calculation mismatch"); return result;
        }
    }
    return result;
}

ArchiveReadResult read_archive_container(const std::vector<std::uint8_t>& bytes) {
    ArchiveReadResult result;
    if (bytes.size() < ArMagic.size() || !std::equal(ArMagic.begin(), ArMagic.end(), bytes.begin())) {
        add_diag(result.diagnostics, "XAR0022", "archive magic is missing or invalid"); return result;
    }
    std::vector<RawMember> raw_members;
    std::vector<std::uint8_t> long_names;
    std::vector<std::vector<std::uint8_t>> linker_members;
    std::size_t cursor = ArMagic.size();
    while (cursor < bytes.size()) {
        if (bytes.size() - cursor < ArHeaderSize) {
            add_diag(result.diagnostics, "XAR0023", "truncated archive member header"); return result;
        }
        if (bytes[cursor + 58U] != '`' || bytes[cursor + 59U] != '\n') {
            add_diag(result.diagnostics, "XAR0024", "archive member header trailer is invalid"); return result;
        }
        const std::string raw_name = trim_field(bytes, cursor, 16U);
        std::uint64_t size64 = 0U;
        if (!parse_decimal(bytes, cursor + 48U, 10U, size64) || size64 > std::numeric_limits<std::size_t>::max()) {
            add_diag(result.diagnostics, "XAR0025", "archive member size field is invalid or overflows"); return result;
        }
        const std::size_t size = static_cast<std::size_t>(size64);
        const std::size_t payload_at = cursor + ArHeaderSize;
        if (size > bytes.size() - payload_at) {
            add_diag(result.diagnostics, "XAR0026", "archive member payload is truncated"); return result;
        }
        std::vector<std::uint8_t> payload(bytes.begin() + static_cast<std::ptrdiff_t>(payload_at),
            bytes.begin() + static_cast<std::ptrdiff_t>(payload_at + size));
        if (raw_name == "//") {
            if (!long_names.empty()) { add_diag(result.diagnostics, "XAR0027", "duplicate archive long-name table"); return result; }
            long_names = std::move(payload);
        } else if (raw_name == "/") {
            linker_members.push_back(std::move(payload));
        } else if (raw_name == "/SYM64/") {
            add_diag(result.diagnostics, "XAR0028", "64-bit GNU archive symbol tables are not supported in this version"); return result;
        } else if (raw_name == "__.SYMDEF" || raw_name == "__.SYMDEF SORTED") {
            add_diag(result.diagnostics, "XAR0029", "BSD ranlib archive indexes are not supported in this version"); return result;
        } else {
            RawMember member;
            member.raw_name = raw_name;
            member.header_offset = cursor;
            if (raw_name.rfind("#1/", 0U) == 0U) {
                std::uint64_t name_length = 0U;
                const std::string field = raw_name.substr(3U);
                try {
                    std::size_t used = 0U;
                    name_length = std::stoull(field, &used, 10);
                    if (used != field.size()) throw std::invalid_argument("bad");
                } catch (...) {
                    add_diag(result.diagnostics, "XAR0030", "BSD extended archive name length is malformed"); return result;
                }
                if (name_length > payload.size()) { add_diag(result.diagnostics, "XAR0031", "BSD extended archive name exceeds member payload"); return result; }
                member.name.assign(reinterpret_cast<const char*>(payload.data()), static_cast<std::size_t>(name_length));
                member.contents.assign(payload.begin() + static_cast<std::ptrdiff_t>(name_length), payload.end());
            } else if (raw_name.size() > 1U && raw_name[0] == '/' &&
                       std::all_of(raw_name.begin() + 1, raw_name.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
                try {
                    member.long_offset = static_cast<std::size_t>(std::stoull(raw_name.substr(1U)));
                    member.has_long_offset = true;
                } catch (...) { add_diag(result.diagnostics, "XAR0032", "GNU long-name offset overflows"); return result; }
                member.contents = std::move(payload);
            } else {
                member.name = raw_name;
                if (!member.name.empty() && member.name.back() == '/') member.name.pop_back();
                member.contents = std::move(payload);
            }
            raw_members.push_back(std::move(member));
        }
        const std::size_t record_size = ArHeaderSize + size + (size & 1U);
        if (record_size > bytes.size() - cursor) {
            add_diag(result.diagnostics, "XAR0033", "archive member padding is missing"); return result;
        }
        if ((size & 1U) != 0U && bytes[payload_at + size] != static_cast<std::uint8_t>('\n')) {
            add_diag(result.diagnostics, "XAR0056", "archive odd-size member padding byte is not newline"); return result;
        }
        cursor += record_size;
    }
    if (cursor != bytes.size()) { add_diag(result.diagnostics, "XAR0034", "archive has trailing bytes outside member records"); return result; }
    if (linker_members.size() > 2U) { add_diag(result.diagnostics, "XAR0035", "archive contains more than two linker index members"); return result; }

    std::map<std::uint64_t, std::size_t> offset_to_member;
    for (std::size_t i = 0U; i < raw_members.size(); ++i) offset_to_member.emplace(raw_members[i].header_offset, i);
    for (auto& member : raw_members) {
        if (member.has_long_offset) {
            if (member.long_offset >= long_names.size()) { add_diag(result.diagnostics, "XAR0036", "GNU long-name reference is outside long-name table"); return result; }
            std::size_t end = member.long_offset;
            while (end + 1U < long_names.size() && !(long_names[end] == '/' && long_names[end + 1U] == '\n')) ++end;
            if (end + 1U >= long_names.size()) { add_diag(result.diagnostics, "XAR0037", "GNU long-name entry is not terminated by slash-newline"); return result; }
            member.name.assign(reinterpret_cast<const char*>(long_names.data() + member.long_offset), end - member.long_offset);
        }
        if (member.name.empty() || member.name.find('\0') != std::string::npos) {
            add_diag(result.diagnostics, "XAR0038", "archive member resolved to an empty or NUL-containing name"); return result;
        }
        result.archive.members.push_back(ArchiveContainerMember{member.name, std::move(member.contents), {}, member.header_offset});
    }

    if (!linker_members.empty()) {
        std::vector<std::string> names;
        std::uint32_t count = 0U;
        if (!get_u32_be(linker_members[0], 0U, count)) {
            add_diag(result.diagnostics, "XAR0039", "archive symbol index is truncated"); return result;
        }
        if (linker_members.size() == 1U) {
            result.archive.format = ArchiveFormat::GnuAr;
            std::vector<std::uint64_t> member_offsets;
            if (!get_u32_be(linker_members[0], 0U, count) ||
                !parse_index_offsets(linker_members[0], true, count, 4U, member_offsets)) {
                add_diag(result.diagnostics, "XAR0040", "GNU ar symbol index offset table is malformed"); return result;
            }
            const std::size_t strings_at = 4U + static_cast<std::size_t>(count) * 4U;
            if (!decode_index_strings(linker_members[0], strings_at, count, names)) {
                add_diag(result.diagnostics, "XAR0041", "GNU ar symbol index string table is malformed"); return result;
            }
            for (std::size_t i = 0U; i < names.size(); ++i) {
                const auto found = offset_to_member.find(member_offsets[i]);
                if (found == offset_to_member.end()) { add_diag(result.diagnostics, "XAR0042", "GNU symbol index points outside ordinary archive members"); return result; }
                result.archive.symbol_index.push_back(ArchiveSymbolIndexEntry{names[i], found->second});
            }
        } else {
            result.archive.format = ArchiveFormat::CoffLib;
            std::vector<std::uint64_t> member_offsets;
            if (!get_u32_le(linker_members[1], 0U, count) ||
                !parse_index_offsets(linker_members[1], false, count, 4U, member_offsets)) {
                add_diag(result.diagnostics, "XAR0043", "COFF second linker member member-offset table is malformed"); return result;
            }
            if (member_offsets.size() != raw_members.size()) {
                add_diag(result.diagnostics, "XAR0058", "COFF second linker member does not enumerate every archive object member"); return result;
            }
            for (std::size_t i = 0U; i < member_offsets.size(); ++i) {
                const auto found_member = offset_to_member.find(member_offsets[i]);
                if (found_member == offset_to_member.end() || found_member->second != i) {
                    add_diag(result.diagnostics, "XAR0057", "COFF member-offset table is not a complete ordered list of object member headers"); return result;
                }
            }
            const std::size_t symbol_count_at = 4U + static_cast<std::size_t>(count) * 4U;
            std::uint32_t symbol_count = 0U;
            if (!get_u32_le(linker_members[1], symbol_count_at, symbol_count)) {
                add_diag(result.diagnostics, "XAR0044", "COFF second linker member symbol count is missing"); return result;
            }
            const std::size_t indices_at = symbol_count_at + 4U;
            if (symbol_count > (linker_members[1].size() - std::min(indices_at, linker_members[1].size())) / 2U) {
                add_diag(result.diagnostics, "XAR0045", "COFF second linker member symbol index array is truncated"); return result;
            }
            std::vector<std::uint16_t> member_indices;
            for (std::size_t i = 0U; i < symbol_count; ++i) {
                std::uint16_t index = 0U;
                if (!get_u16_le(linker_members[1], indices_at + i * 2U, index) || index == 0U || index > member_offsets.size()) {
                    add_diag(result.diagnostics, "XAR0046", "COFF symbol references an invalid 1-based member index"); return result;
                }
                member_indices.push_back(index);
            }
            const std::size_t strings_at = indices_at + static_cast<std::size_t>(symbol_count) * 2U;
            if (!decode_index_strings(linker_members[1], strings_at, symbol_count, names)) {
                add_diag(result.diagnostics, "XAR0047", "COFF second linker member string table is malformed"); return result;
            }
            for (std::size_t i = 0U; i < names.size(); ++i) {
                const std::size_t member_index = static_cast<std::size_t>(member_indices[i] - 1U);
                const auto found = offset_to_member.find(member_offsets[member_index]);
                if (found == offset_to_member.end()) { add_diag(result.diagnostics, "XAR0048", "COFF linker index points outside ordinary archive members"); return result; }
                result.archive.symbol_index.push_back(ArchiveSymbolIndexEntry{names[i], found->second});
            }
            // Validate the first linker member too: its offsets and symbol names must agree in count.
            std::uint32_t first_count = 0U;
            std::vector<std::uint64_t> first_offsets;
            std::vector<std::string> first_names;
            if (!get_u32_be(linker_members[0], 0U, first_count) ||
                !parse_index_offsets(linker_members[0], true, first_count, 4U, first_offsets) ||
                !decode_index_strings(linker_members[0], 4U + static_cast<std::size_t>(first_count) * 4U,
                                      first_count, first_names) || first_count != symbol_count) {
                add_diag(result.diagnostics, "XAR0049", "COFF first linker member is malformed or disagrees with second index"); return result;
            }
            for (std::size_t i = 0U; i < first_names.size(); ++i) {
                const std::size_t second_member_index = static_cast<std::size_t>(member_indices[i] - 1U);
                if (first_names[i] != names[i] || first_offsets[i] != member_offsets[second_member_index]) {
                    add_diag(result.diagnostics, "XAR0055", "COFF first and second linker member indexes disagree"); return result;
                }
            }
        }
        result.archive.has_symbol_index = true;
    } else {
        result.archive.format = ArchiveFormat::GnuAr;
    }

    if (result.archive.format == ArchiveFormat::GnuAr) {
        std::set<std::string> names_seen;
        for (const auto& member : result.archive.members) {
            if (!names_seen.insert(member.name).second) {
                add_diag(result.diagnostics, "XAR0050", "GNU ar archive contains duplicate resolved member names"); return result;
            }
        }
    }
    return result;
}

ObjectArchiveWriteResult write_object_archive(ArchiveFormat format,
    const std::vector<std::pair<std::string, ObjectFile>>& objects, bool include_symbol_index) {
    ObjectArchiveWriteResult result;
    std::vector<ArchiveContainerMember> members;
    members.reserve(objects.size());
    const ObjectFileFormat expected = format == ArchiveFormat::GnuAr ? ObjectFileFormat::ELF : ObjectFileFormat::COFF;
    for (const auto& item : objects) {
        if (item.second.descriptor.format != expected || item.second.descriptor.architecture != ObjectArchitecture::X86_64) {
            add_diag(result.diagnostics, "XAR0051", "object format or architecture does not match target archive container"); return result;
        }
        const auto encoded = write_binary_object(item.second);
        if (!encoded.succeeded()) { result.diagnostics = encoded.diagnostics; return result; }
        ArchiveContainerMember member;
        member.name = item.first;
        member.contents = encoded.bytes;
        for (const auto& symbol : item.second.symbols) {
            if (symbol.defined && symbol.binding != ObjectSymbolBinding::Local) member.defined_symbols.push_back(symbol.name);
        }
        members.push_back(std::move(member));
    }
    const auto archive = write_archive_container(format, members, include_symbol_index);
    result.bytes = archive.bytes;
    result.diagnostics = archive.diagnostics;
    return result;
}

ObjectArchiveReadResult read_object_archive(const std::vector<std::uint8_t>& bytes,
                                            const std::string& archive_id) {
    ObjectArchiveReadResult result;
    const auto container = read_archive_container(bytes);
    if (!container.succeeded()) { result.diagnostics = container.diagnostics; return result; }
    result.format = container.archive.format;
    result.symbol_index = container.archive.symbol_index;
    result.archive.archive_id = archive_id;
    const ObjectFileFormat expected = result.format == ArchiveFormat::GnuAr ? ObjectFileFormat::ELF : ObjectFileFormat::COFF;
    for (const auto& member : container.archive.members) {
        const auto decoded = read_binary_object(member.contents);
        if (!decoded.succeeded()) { result.diagnostics = decoded.diagnostics; return result; }
        if (decoded.object_file.descriptor.format != expected ||
            decoded.object_file.descriptor.architecture != ObjectArchitecture::X86_64) {
            add_diag(result.diagnostics, "XAR0052", "archive object payload does not match archive container format"); return result;
        }
        result.archive.members.push_back(StaticArchiveMember{member.name, decoded.object_file});
    }
    for (const auto& index_entry : result.symbol_index) {
        if (index_entry.member_index >= result.archive.members.size()) {
            add_diag(result.diagnostics, "XAR0053", "archive symbol index member mapping is outside decoded object members"); return result;
        }
        const auto& object = result.archive.members[index_entry.member_index].object_file;
        const auto found = std::find_if(object.symbols.begin(), object.symbols.end(), [&index_entry](const ObjectSymbol& symbol) {
            return symbol.name == index_entry.symbol_name && symbol.defined && symbol.binding != ObjectSymbolBinding::Local;
        });
        if (found == object.symbols.end()) {
            add_diag(result.diagnostics, "XAR0054", "archive symbol index references a symbol not defined by its member"); return result;
        }
    }
    return result;
}

} // namespace xfrontend
