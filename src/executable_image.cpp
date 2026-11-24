// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/executable_image.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace xfrontend {
namespace {

constexpr std::size_t ElfHeaderSize = 64U;
constexpr std::size_t ElfProgramHeaderSize = 56U;
constexpr std::uint16_t ElfTypeExecutable = 2U;
constexpr std::uint16_t ElfMachineX86_64 = 62U;
constexpr std::uint32_t ElfVersionCurrent = 1U;
constexpr std::uint32_t ProgramTypeLoad = 1U;
constexpr std::uint32_t ProgramTypeGnuStack = 0x6474e551U;
constexpr std::uint32_t ProgramFlagExecute = 1U;
constexpr std::uint32_t ProgramFlagWrite = 2U;
constexpr std::uint32_t ProgramFlagRead = 4U;

struct SegmentDraft {
    std::size_t first_section = 0U;
    std::size_t last_section_exclusive = 0U;
    std::uint64_t virtual_address = 0U;
    std::size_t memory_size = 0U;
    std::size_t file_size = 0U;
    std::uint32_t flags = 0U;
    std::size_t file_offset = 0U;
};

void add_diag(std::vector<Diagnostic>& out, const char* code, const std::string& message) {
    out.push_back(Diagnostic{code, message, {}});
}

bool is_power_of_two(std::size_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

bool checked_add_u64(std::uint64_t left, std::uint64_t right, std::uint64_t& result) {
    if (left > std::numeric_limits<std::uint64_t>::max() - right) return false;
    result = left + right;
    return true;
}

bool checked_align_up(std::uint64_t value, std::size_t alignment, std::uint64_t& result) {
    if (!is_power_of_two(alignment)) return false;
    const std::uint64_t mask = static_cast<std::uint64_t>(alignment - 1U);
    if (value > std::numeric_limits<std::uint64_t>::max() - mask) return false;
    result = (value + mask) & ~mask;
    return true;
}

void write_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void write_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned index = 0U; index < 4U; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
}

void write_u64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned index = 0U; index < 8U; ++index)
        bytes[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU);
}

bool section_permissions(ObjectSectionKind kind, std::uint32_t& flags) {
    switch (kind) {
    case ObjectSectionKind::Text:
        flags = ProgramFlagRead | ProgramFlagExecute;
        return true;
    case ObjectSectionKind::ReadOnlyData:
    case ObjectSectionKind::Metadata:
        flags = ProgramFlagRead;
        return true;
    case ObjectSectionKind::Data:
    case ObjectSectionKind::Bss:
        flags = ProgramFlagRead | ProgramFlagWrite;
        return true;
    }
    return false;
}

ExecutableSegmentPermission to_permission(std::uint32_t flags) {
    if ((flags & ProgramFlagExecute) != 0U) return ExecutableSegmentPermission::ReadExecute;
    if ((flags & ProgramFlagWrite) != 0U) return ExecutableSegmentPermission::ReadWrite;
    return ExecutableSegmentPermission::Read;
}

bool symbol_is_entry(const LinkedImageSymbol& symbol, const std::string& entry_name) {
    return symbol.name == entry_name && symbol.binding != ObjectSymbolBinding::Local &&
           symbol.kind == ObjectSymbolKind::Function;
}

bool section_contains(const LinkedImageSection& section, std::uint64_t address) {
    if (address < section.address) return false;
    const std::uint64_t offset = address - section.address;
    return offset < section.memory_size;
}

bool validate_descriptor(const ObjectFileDescriptor& descriptor) {
    return descriptor.format == ObjectFileFormat::ELF &&
           descriptor.architecture == ObjectArchitecture::X86_64 &&
           descriptor.operating_system == ObjectOperatingSystem::Linux &&
           descriptor.byte_order == ObjectByteOrder::LittleEndian &&
           descriptor.pointer_size == 8U;
}

} // namespace

ExecutableImageResult write_elf64_executable_image(const LinkedImage& linked_image,
                                                    Elf64ExecutableOptions options) {
    ExecutableImageResult result;
    result.image.descriptor = linked_image.descriptor;
    result.image.image_base = linked_image.image_base;

    if (!validate_descriptor(linked_image.descriptor)) {
        add_diag(result.diagnostics, "XEIMG0001", "N25 ELF64 executable writer supports only little-endian Linux x86-64 targets");
        return result;
    }
    if (options.startup.use_c_runtime || options.startup.run_static_initializers ||
        options.startup.run_tls_initializers || options.startup.request_dynamic_interpreter) {
        add_diag(result.diagnostics, "XEIMG0017", "N25 startup pipeline supports direct static process entry only; CRT, constructors, TLS, and dynamic interpreter are not implemented");
        return result;
    }
    if (!is_power_of_two(options.page_size) || options.page_size < 4096U || options.page_size > 1024U * 1024U) {
        add_diag(result.diagnostics, "XEIMG0002", "ELF page size must be a power of two from 4096 through 1048576 bytes");
        return result;
    }
    if (options.maximum_file_size < ElfHeaderSize + ElfProgramHeaderSize) {
        add_diag(result.diagnostics, "XEIMG0013", "maximum executable file size is smaller than the minimum ELF headers");
        return result;
    }
    if (options.startup.entry_symbol.empty()) {
        add_diag(result.diagnostics, "XEIMG0004", "startup entry symbol name cannot be empty");
        return result;
    }
    if (!linked_image.has_entry) {
        add_diag(result.diagnostics, "XEIMG0003", "linked image does not contain an explicitly selected entry address");
        return result;
    }
    if (linked_image.image_size == 0U || linked_image.flat_bytes.size() != linked_image.image_size ||
        linked_image.sections.empty()) {
        add_diag(result.diagnostics, "XEIMG0005", "linked image is empty or its flat byte buffer does not match image_size");
        return result;
    }
    if (linked_image.sections.size() > 65534U) {
        add_diag(result.diagnostics, "XEIMG0014", "ELF program header count exceeds the 16-bit format limit");
        return result;
    }

    const auto entry_symbol = std::find_if(linked_image.symbols.begin(), linked_image.symbols.end(),
        [&options](const LinkedImageSymbol& symbol) { return symbol_is_entry(symbol, options.startup.entry_symbol); });
    if (entry_symbol == linked_image.symbols.end() || entry_symbol->address != linked_image.entry_address) {
        add_diag(result.diagnostics, "XEIMG0004", "requested startup entry must be a defined external Function matching the linker's entry address");
        return result;
    }

    std::uint64_t image_end = 0U;
    if (!checked_add_u64(linked_image.image_base, linked_image.image_size, image_end)) {
        add_diag(result.diagnostics, "XEIMG0006", "linked image address range overflows 64-bit virtual address space");
        return result;
    }

    std::vector<SegmentDraft> drafts;
    std::uint64_t previous_section_end = linked_image.image_base;
    std::uint64_t previous_group_page_end = linked_image.image_base;
    std::uint32_t current_flags = 0U;
    bool have_group = false;
    bool saw_bss_in_group = false;

    for (std::size_t index = 0U; index < linked_image.sections.size(); ++index) {
        const auto& section = linked_image.sections[index];
        std::uint32_t flags = 0U;
        if (!section_permissions(section.kind, flags)) {
            add_diag(result.diagnostics, "XEIMG0007", "linked image contains an unsupported section kind");
            return result;
        }
        if (section.memory_size == 0U || !is_power_of_two(section.alignment)) {
            add_diag(result.diagnostics, "XEIMG0020", "executable sections must be non-empty and use power-of-two alignment");
            return result;
        }
        if ((section.address & static_cast<std::uint64_t>(section.alignment - 1U)) != 0U) {
            add_diag(result.diagnostics, "XEIMG0009", "linked section address violates its declared alignment");
            return result;
        }
        std::uint64_t section_end = 0U;
        if (!checked_add_u64(section.address, section.memory_size, section_end) ||
            section.address < linked_image.image_base || section_end > image_end) {
            add_diag(result.diagnostics, "XEIMG0006", "linked section lies outside the validated image address range");
            return result;
        }
        if (section.address < previous_section_end) {
            add_diag(result.diagnostics, "XEIMG0008", "linked sections overlap or are not ordered by ascending virtual address");
            return result;
        }
        previous_section_end = section_end;

        const std::uint64_t flat_offset64 = section.address - linked_image.image_base;
        if (flat_offset64 > linked_image.flat_bytes.size() ||
            section.memory_size > linked_image.flat_bytes.size() - static_cast<std::size_t>(flat_offset64)) {
            add_diag(result.diagnostics, "XEIMG0018", "linked section is outside the flat byte buffer");
            return result;
        }
        if (section.kind == ObjectSectionKind::Bss) {
            if (!section.contents.empty() && section.contents.size() != section.memory_size) {
                add_diag(result.diagnostics, "XEIMG0012", "BSS contents must be empty or exactly memory_size bytes");
                return result;
            }
            if (std::any_of(section.contents.begin(), section.contents.end(),
                            [](std::uint8_t value) { return value != 0U; })) {
                add_diag(result.diagnostics, "XEIMG0012", "BSS contents must be zero-filled");
                return result;
            }
            saw_bss_in_group = true;
        } else {
            if (saw_bss_in_group) {
                add_diag(result.diagnostics, "XEIMG0012", "file-backed data cannot follow BSS inside the same load-permission run");
                return result;
            }
            if (section.contents.size() != section.memory_size) {
                add_diag(result.diagnostics, "XEIMG0012", "file-backed section contents must equal memory_size");
                return result;
            }
            const std::size_t flat_offset = static_cast<std::size_t>(flat_offset64);
            if (!std::equal(section.contents.begin(), section.contents.end(),
                            linked_image.flat_bytes.begin() + static_cast<std::ptrdiff_t>(flat_offset))) {
                add_diag(result.diagnostics, "XEIMG0018", "linked section contents disagree with the relocated flat image bytes");
                return result;
            }
        }

        if (!have_group || flags != current_flags) {
            if (have_group) {
                const auto& previous = drafts.back();
                std::uint64_t page_end = 0U;
                if (!checked_align_up(previous.virtual_address + previous.memory_size,
                                      options.page_size, page_end)) {
                    add_diag(result.diagnostics, "XEIMG0015", "segment page-range alignment overflows");
                    return result;
                }
                previous_group_page_end = page_end;
            }
            if ((section.address % options.page_size) != 0U) {
                add_diag(result.diagnostics, "XEIMG0010", "each new permission run must begin at a page-aligned virtual address");
                return result;
            }
            if (have_group && section.address < previous_group_page_end) {
                add_diag(result.diagnostics, "XEIMG0011", "different ELF permission runs share or overlap a memory page");
                return result;
            }
            SegmentDraft draft;
            draft.first_section = index;
            draft.last_section_exclusive = index + 1U;
            draft.virtual_address = section.address;
            draft.memory_size = section.memory_size;
            draft.file_size = section.kind == ObjectSectionKind::Bss ? 0U : section.memory_size;
            draft.flags = flags;
            drafts.push_back(draft);
            current_flags = flags;
            have_group = true;
            saw_bss_in_group = section.kind == ObjectSectionKind::Bss;
        } else {
            auto& draft = drafts.back();
            draft.last_section_exclusive = index + 1U;
            draft.memory_size = static_cast<std::size_t>(section_end - draft.virtual_address);
            if (section.kind != ObjectSectionKind::Bss) {
                draft.file_size = static_cast<std::size_t>(section_end - draft.virtual_address);
            }
        }
    }

    const auto entry_section = std::find_if(linked_image.sections.begin(), linked_image.sections.end(),
        [&linked_image](const LinkedImageSection& section) {
            return section.kind == ObjectSectionKind::Text && section_contains(section, linked_image.entry_address);
        });
    if (entry_section == linked_image.sections.end()) {
        add_diag(result.diagnostics, "XEIMG0019", "startup entry address must lie inside an executable Text section");
        return result;
    }

    const std::size_t program_header_count = drafts.size() + 1U; // PT_GNU_STACK.
    if (program_header_count > 65535U) {
        add_diag(result.diagnostics, "XEIMG0014", "ELF program header count exceeds the 16-bit format limit");
        return result;
    }

    std::uint64_t file_cursor = 0U;
    const std::uint64_t header_end = static_cast<std::uint64_t>(ElfHeaderSize) +
        static_cast<std::uint64_t>(program_header_count) * ElfProgramHeaderSize;
    if (!checked_align_up(header_end, options.page_size, file_cursor)) {
        add_diag(result.diagnostics, "XEIMG0015", "ELF header table alignment overflows");
        return result;
    }

    for (auto& draft : drafts) {
        if (!checked_align_up(file_cursor, options.page_size, file_cursor)) {
            add_diag(result.diagnostics, "XEIMG0015", "ELF segment file alignment overflows");
            return result;
        }
        draft.file_offset = static_cast<std::size_t>(file_cursor);
        std::uint64_t segment_file_end = 0U;
        if (!checked_add_u64(file_cursor, draft.file_size, segment_file_end) ||
            segment_file_end > options.maximum_file_size ||
            segment_file_end > std::numeric_limits<std::size_t>::max()) {
            add_diag(result.diagnostics, "XEIMG0013", "ELF executable exceeds configured maximum file size");
            return result;
        }
        file_cursor = segment_file_end;
    }
    if (file_cursor > options.maximum_file_size || file_cursor > std::numeric_limits<std::size_t>::max()) {
        add_diag(result.diagnostics, "XEIMG0013", "ELF executable exceeds configured maximum file size");
        return result;
    }

    result.image.bytes.assign(static_cast<std::size_t>(file_cursor), 0U);
    auto& bytes = result.image.bytes;

    // ELF64 file header.
    bytes[0U] = 0x7fU; bytes[1U] = 'E'; bytes[2U] = 'L'; bytes[3U] = 'F';
    bytes[4U] = 2U; // ELFCLASS64
    bytes[5U] = 1U; // ELFDATA2LSB
    bytes[6U] = 1U; // EV_CURRENT
    bytes[7U] = 0U; // ELFOSABI_SYSV
    write_u16(bytes, 16U, ElfTypeExecutable);
    write_u16(bytes, 18U, ElfMachineX86_64);
    write_u32(bytes, 20U, ElfVersionCurrent);
    write_u64(bytes, 24U, linked_image.entry_address);
    write_u64(bytes, 32U, ElfHeaderSize);
    write_u64(bytes, 40U, 0U); // No section header table in this minimal static image.
    write_u32(bytes, 48U, 0U);
    write_u16(bytes, 52U, static_cast<std::uint16_t>(ElfHeaderSize));
    write_u16(bytes, 54U, static_cast<std::uint16_t>(ElfProgramHeaderSize));
    write_u16(bytes, 56U, static_cast<std::uint16_t>(program_header_count));
    write_u16(bytes, 58U, 0U);
    write_u16(bytes, 60U, 0U);
    write_u16(bytes, 62U, 0U);

    for (std::size_t index = 0U; index < drafts.size(); ++index) {
        const auto& draft = drafts[index];
        const std::size_t ph = ElfHeaderSize + index * ElfProgramHeaderSize;
        write_u32(bytes, ph, ProgramTypeLoad);
        write_u32(bytes, ph + 4U, draft.flags);
        write_u64(bytes, ph + 8U, draft.file_offset);
        write_u64(bytes, ph + 16U, draft.virtual_address);
        write_u64(bytes, ph + 24U, draft.virtual_address);
        write_u64(bytes, ph + 32U, draft.file_size);
        write_u64(bytes, ph + 40U, draft.memory_size);
        write_u64(bytes, ph + 48U, options.page_size);

        if (draft.file_size != 0U) {
            const std::uint64_t source_offset64 = draft.virtual_address - linked_image.image_base;
            if (source_offset64 > linked_image.flat_bytes.size() ||
                draft.file_size > linked_image.flat_bytes.size() - static_cast<std::size_t>(source_offset64)) {
                add_diag(result.diagnostics, "XEIMG0018", "load segment file range exceeds linked flat image");
                result.image.bytes.clear();
                return result;
            }
            std::copy_n(linked_image.flat_bytes.begin() + static_cast<std::ptrdiff_t>(source_offset64),
                        draft.file_size,
                        bytes.begin() + static_cast<std::ptrdiff_t>(draft.file_offset));
        }
        result.image.load_segments.push_back(ExecutableLoadSegment{
            draft.virtual_address, static_cast<std::uint64_t>(draft.file_offset),
            draft.file_size, draft.memory_size, options.page_size, to_permission(draft.flags)});
    }

    // A non-executable process stack (PF_R | PF_W, no PF_X).
    const std::size_t stack_ph = ElfHeaderSize + drafts.size() * ElfProgramHeaderSize;
    write_u32(bytes, stack_ph, ProgramTypeGnuStack);
    write_u32(bytes, stack_ph + 4U, ProgramFlagRead | ProgramFlagWrite);
    write_u64(bytes, stack_ph + 8U, 0U);
    write_u64(bytes, stack_ph + 16U, 0U);
    write_u64(bytes, stack_ph + 24U, 0U);
    write_u64(bytes, stack_ph + 32U, 0U);
    write_u64(bytes, stack_ph + 40U, 0U);
    write_u64(bytes, stack_ph + 48U, 16U);

    result.image.entry_address = linked_image.entry_address;
    result.image.startup.entry_symbol = options.startup.entry_symbol;
    result.image.startup.entry_address = linked_image.entry_address;
    result.image.startup.entry_abi = ExecutableEntryAbi::SysVAMD64ProcessEntry;
    result.image.startup.stack_alignment = 16U;
    result.image.startup.initial_stack_contains_argc_argv_envp = true;
    result.image.startup.uses_c_runtime = false;
    result.image.startup.runs_static_initializers = false;
    result.image.startup.runs_tls_initializers = false;
    result.image.startup.uses_dynamic_interpreter = false;
    result.image.startup.direct_entry = true;
    result.image.has_non_executable_stack = true;
    result.image.has_dynamic_interpreter = false;
    result.image.has_c_runtime = false;
    return result;
}

} // namespace xfrontend
