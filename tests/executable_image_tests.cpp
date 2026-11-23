#include "xfrontend/executable_image.hpp"
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

void exec_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void exec_case(const std::string& name, const std::function<void()>& body,
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

std::uint16_t read_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    exec_require(offset <= bytes.size() && bytes.size() - offset >= 2U, "16-bit ELF field outside output");
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t read_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    exec_require(offset <= bytes.size() && bytes.size() - offset >= 4U, "32-bit ELF field outside output");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

std::uint64_t read_u64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    exec_require(offset <= bytes.size() && bytes.size() - offset >= 8U, "64-bit ELF field outside output");
    std::uint64_t value = 0U;
    for (unsigned index = 0U; index < 8U; ++index)
        value |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    return value;
}

LinkedImage make_text_image(std::size_t text_size = 64U, std::size_t entry_offset = 0U,
                            std::uint8_t fill = 0x90U) {
    LinkedImage image;
    image.descriptor = make_elf_x86_64_object_target();
    image.image_base = 0x400000U;
    image.image_size = text_size;
    image.flat_bytes.assign(text_size, fill);
    LinkedImageSection text;
    text.object_id = "smoke-object";
    text.source_section_id = 1U;
    text.name = ".text";
    text.kind = ObjectSectionKind::Text;
    text.address = image.image_base;
    text.alignment = 4096U;
    text.memory_size = text_size;
    text.contents = image.flat_bytes;
    image.sections.push_back(text);
    LinkedImageSymbol entry;
    entry.name = "_start";
    entry.object_id = "smoke-object";
    entry.section_id = 1U;
    entry.address = image.image_base + entry_offset;
    entry.size = 1U;
    entry.binding = ObjectSymbolBinding::Global;
    entry.kind = ObjectSymbolKind::Function;
    image.symbols.push_back(entry);
    image.entry_address = entry.address;
    image.has_entry = true;
    return image;
}

LinkedImage make_multisegment_image(std::size_t ro_size, std::size_t data_size,
                                    std::size_t bss_size, std::uint8_t seed) {
    LinkedImage image;
    image.descriptor = make_elf_x86_64_object_target();
    image.image_base = 0x400000U;
    const std::uint64_t text_address = 0x400000U;
    const std::uint64_t ro_address = 0x401000U;
    const std::uint64_t data_address = 0x402000U;
    const std::uint64_t bss_address = data_address + data_size;
    const std::uint64_t image_end = bss_address + bss_size;
    image.image_size = static_cast<std::size_t>(image_end - image.image_base);
    image.flat_bytes.assign(image.image_size, 0U);

    LinkedImageSection text;
    text.object_id = "multi-text";
    text.source_section_id = 1U;
    text.name = ".text";
    text.kind = ObjectSectionKind::Text;
    text.address = text_address;
    text.alignment = 4096U;
    text.memory_size = 0x100U;
    text.contents.assign(text.memory_size, 0x90U);
    std::copy(text.contents.begin(), text.contents.end(), image.flat_bytes.begin());

    LinkedImageSection ro;
    ro.object_id = "multi-ro";
    ro.source_section_id = 2U;
    ro.name = ".rodata";
    ro.kind = ObjectSectionKind::ReadOnlyData;
    ro.address = ro_address;
    ro.alignment = 4096U;
    ro.memory_size = ro_size;
    ro.contents.resize(ro_size);
    for (std::size_t index = 0U; index < ro_size; ++index)
        ro.contents[index] = static_cast<std::uint8_t>(seed + index);
    std::copy(ro.contents.begin(), ro.contents.end(),
        image.flat_bytes.begin() + static_cast<std::ptrdiff_t>(ro_address - image.image_base));

    LinkedImageSection data;
    data.object_id = "multi-data";
    data.source_section_id = 3U;
    data.name = ".data";
    data.kind = ObjectSectionKind::Data;
    data.address = data_address;
    data.alignment = 4096U;
    data.memory_size = data_size;
    data.contents.resize(data_size);
    for (std::size_t index = 0U; index < data_size; ++index)
        data.contents[index] = static_cast<std::uint8_t>(seed ^ static_cast<std::uint8_t>(index * 3U));
    std::copy(data.contents.begin(), data.contents.end(),
        image.flat_bytes.begin() + static_cast<std::ptrdiff_t>(data_address - image.image_base));

    LinkedImageSection bss;
    bss.object_id = "multi-bss";
    bss.source_section_id = 4U;
    bss.name = ".bss";
    bss.kind = ObjectSectionKind::Bss;
    bss.address = bss_address;
    bss.alignment = 1U;
    bss.memory_size = bss_size;
    bss.contents.assign(bss_size, 0U);

    image.sections = {text, ro, data, bss};
    LinkedImageSymbol entry;
    entry.name = "_start";
    entry.object_id = "multi-text";
    entry.section_id = 1U;
    entry.address = text_address;
    entry.size = 8U;
    entry.binding = ObjectSymbolBinding::Global;
    entry.kind = ObjectSymbolKind::Function;
    image.symbols.push_back(entry);
    image.entry_address = entry.address;
    image.has_entry = true;
    return image;
}

bool has_code(const std::vector<Diagnostic>& diagnostics, const std::string& code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(), [&code](const Diagnostic& item) {
        return item.code == code;
    });
}

void require_failure(const LinkedImage& image, const Elf64ExecutableOptions& options,
                     const std::string& expected_code) {
    const auto result = write_elf64_executable_image(image, options);
    exec_require(!result.succeeded(), "invalid executable image unexpectedly succeeded");
    exec_require(has_code(result.diagnostics, expected_code),
                 "expected diagnostic " + expected_code + " was not produced");
    exec_require(result.image.bytes.empty(), "failed writer must not expose partial executable bytes");
}

} // namespace

void run_executable_image_tests(int& passed, int& failed) {
    // N25A: ELF64 ET_EXEC header, entrypoint, program-header table, and stable file layout.
    for (int index = 0; index < 500; ++index) {
        exec_case("N25A" + std::to_string(index + 1) + "_elf64_header_program_table", [index] {
            const std::size_t size = 32U + static_cast<std::size_t>(index % 193);
            const std::size_t entry_offset = static_cast<std::size_t>(index) % size;
            const std::uint8_t fill = static_cast<std::uint8_t>((index * 37) & 0xff);
            const auto input = make_text_image(size, entry_offset, fill);
            const auto result = write_elf64_executable_image(input);
            exec_require(result.succeeded(), "valid ELF64 image should be emitted");
            const auto& bytes = result.image.bytes;
            exec_require(bytes.size() >= 4096U + size, "ELF must reserve a page-aligned first segment");
            exec_require(bytes[0] == 0x7fU && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F',
                         "ELF magic mismatch");
            exec_require(bytes[4] == 2U && bytes[5] == 1U && bytes[6] == 1U,
                         "ELF64 little-endian identification mismatch");
            exec_require(read_u16(bytes, 16U) == 2U && read_u16(bytes, 18U) == 62U,
                         "ET_EXEC / EM_X86_64 mismatch");
            exec_require(read_u32(bytes, 20U) == 1U, "ELF header version mismatch");
            exec_require(read_u64(bytes, 24U) == input.entry_address, "ELF entry address mismatch");
            exec_require(read_u64(bytes, 32U) == 64U, "program header offset mismatch");
            exec_require(read_u64(bytes, 40U) == 0U, "minimal image must omit section header table");
            exec_require(read_u16(bytes, 52U) == 64U && read_u16(bytes, 54U) == 56U &&
                         read_u16(bytes, 56U) == 2U, "ELF header and program-header sizes/count mismatch");
            exec_require(read_u16(bytes, 58U) == 0U && read_u16(bytes, 60U) == 0U &&
                         read_u16(bytes, 62U) == 0U, "section-header metadata should be zeroed");
            exec_require(read_u32(bytes, 64U) == 1U && read_u32(bytes, 68U) == 5U,
                         "first PT_LOAD must be readable and executable");
            exec_require(read_u64(bytes, 72U) == 4096U && read_u64(bytes, 80U) == 0x400000U,
                         "first PT_LOAD file/virtual address mismatch");
            exec_require(read_u64(bytes, 96U) == size && read_u64(bytes, 104U) == size,
                         "PT_LOAD file and memory sizes mismatch");
            exec_require(read_u64(bytes, 112U) == 4096U, "PT_LOAD page alignment mismatch");
            exec_require(result.image.entry_address == input.entry_address &&
                         result.image.load_segments.size() == 1U, "result metadata mismatch");
            exec_require(bytes[4096U + (entry_offset % size)] == fill,
                         "first load segment payload mismatch");
        }, passed, failed);
    }

    // N25B: W^X segment separation, read-only data, writable data and trailing BSS.
    for (int index = 0; index < 500; ++index) {
        exec_case("N25B" + std::to_string(index + 1) + "_segment_permissions_bss", [index] {
            const std::size_t ro_size = 16U + static_cast<std::size_t>(index % 240);
            const std::size_t data_size = 16U + static_cast<std::size_t>((index * 7) % 96);
            const std::size_t bss_size = 32U + static_cast<std::size_t>((index * 11) % 256);
            const auto input = make_multisegment_image(ro_size, data_size, bss_size,
                static_cast<std::uint8_t>((index * 19) & 0xff));
            const auto result = write_elf64_executable_image(input);
            exec_require(result.succeeded(), "multi-segment ELF image should be emitted");
            exec_require(result.image.load_segments.size() == 3U,
                         "text, read-only and writable runs should produce three PT_LOAD entries");
            const auto& text = result.image.load_segments[0];
            const auto& ro = result.image.load_segments[1];
            const auto& writable = result.image.load_segments[2];
            exec_require(text.permission == ExecutableSegmentPermission::ReadExecute,
                         "text segment permissions must be RX");
            exec_require(ro.permission == ExecutableSegmentPermission::Read,
                         "read-only segment permissions must be R");
            exec_require(writable.permission == ExecutableSegmentPermission::ReadWrite,
                         "data/BSS segment permissions must be RW");
            exec_require((static_cast<std::uint32_t>(text.permission) & 2U) == 0U,
                         "text segment must not be writable");
            exec_require((static_cast<std::uint32_t>(writable.permission) & 1U) == 0U,
                         "writable segment must not be executable");
            exec_require(text.virtual_address == 0x400000U && ro.virtual_address == 0x401000U &&
                         writable.virtual_address == 0x402000U, "virtual segment order mismatch");
            exec_require(ro.file_size == ro_size && ro.memory_size == ro_size,
                         "read-only segment file/memory sizes mismatch");
            exec_require(writable.file_size == data_size &&
                         writable.memory_size == data_size + bss_size,
                         "BSS must extend memory size without extending file size");
            exec_require(text.alignment == 4096U && ro.alignment == 4096U &&
                         writable.alignment == 4096U, "segment page alignment mismatch");
            const std::size_t stack_header = 64U + 3U * 56U;
            exec_require(read_u32(result.image.bytes, stack_header) == 0x6474e551U,
                         "PT_GNU_STACK header is missing");
            exec_require(read_u32(result.image.bytes, stack_header + 4U) == 6U,
                         "GNU stack must be RW and not executable");
            exec_require(result.image.has_non_executable_stack, "non-executable stack metadata must be set");
            exec_require(result.image.bytes.size() >= writable.file_offset + writable.file_size,
                         "writable file-backed bytes must fit in the executable");
        }, passed, failed);
    }

    // N25C: direct process-entry startup contract, stack ABI and runtime exclusions.
    for (int index = 0; index < 500; ++index) {
        exec_case("N25C" + std::to_string(index + 1) + "_direct_startup_contract", [index] {
            const std::size_t size = 64U + static_cast<std::size_t>(index % 128);
            const std::size_t entry_offset = static_cast<std::size_t>(index * 13) % size;
            auto input = make_text_image(size, entry_offset, static_cast<std::uint8_t>(0x40U + (index % 32)));
            Elf64ExecutableOptions options;
            options.startup.entry_symbol = "_start";
            const auto result = write_elf64_executable_image(input, options);
            exec_require(result.succeeded(), "direct-entry startup plan should succeed");
            const auto& startup = result.image.startup;
            exec_require(startup.entry_symbol == "_start" && startup.entry_address == input.entry_address,
                         "startup entry metadata mismatch");
            exec_require(startup.entry_abi == ExecutableEntryAbi::SysVAMD64ProcessEntry,
                         "startup ABI must be the System V AMD64 process-entry ABI");
            exec_require(startup.stack_alignment == 16U && startup.initial_stack_contains_argc_argv_envp,
                         "startup stack contract mismatch");
            exec_require(startup.direct_entry && !startup.uses_c_runtime &&
                         !startup.runs_static_initializers && !startup.runs_tls_initializers &&
                         !startup.uses_dynamic_interpreter, "startup must be direct and runtime-free");
            exec_require(!result.image.has_dynamic_interpreter && !result.image.has_c_runtime,
                         "image metadata must not claim an unavailable runtime");
            exec_require(result.image.bytes[127U] != 0U || result.image.bytes.size() > 128U,
                         "serialized program header table was unexpectedly truncated");
            const std::size_t stack_header = 64U + 56U;
            exec_require(read_u32(result.image.bytes, stack_header) == 0x6474e551U,
                         "single-segment image must include PT_GNU_STACK");
        }, passed, failed);
    }

    // N25D: strict rejection of unsupported targets, invalid sections, layout and startup options.
    for (int index = 0; index < 500; ++index) {
        exec_case("N25D" + std::to_string(index + 1) + "_executable_rejection_matrix", [index] {
            LinkedImage input = make_text_image(96U, 0U, 0x90U);
            Elf64ExecutableOptions options;
            std::string expected;
            switch (index % 40) {
            case 0: input.descriptor.format = ObjectFileFormat::COFF; expected = "XEIMG0001"; break;
            case 1: input.descriptor.operating_system = ObjectOperatingSystem::Windows; expected = "XEIMG0001"; break;
            case 2: input.descriptor.byte_order = ObjectByteOrder::BigEndian; expected = "XEIMG0001"; break;
            case 3: input.descriptor.pointer_size = 4U; expected = "XEIMG0001"; break;
            case 4: options.page_size = 0U; expected = "XEIMG0002"; break;
            case 5: options.page_size = 2048U; expected = "XEIMG0002"; break;
            case 6: options.page_size = 3U; expected = "XEIMG0002"; break;
            case 7: options.maximum_file_size = 32U; expected = "XEIMG0013"; break;
            case 8: input.has_entry = false; expected = "XEIMG0003"; break;
            case 9: options.startup.entry_symbol.clear(); expected = "XEIMG0004"; break;
            case 10: options.startup.entry_symbol = "main"; expected = "XEIMG0004"; break;
            case 11: options.startup.use_c_runtime = true; expected = "XEIMG0017"; break;
            case 12: options.startup.run_static_initializers = true; expected = "XEIMG0017"; break;
            case 13: options.startup.run_tls_initializers = true; expected = "XEIMG0017"; break;
            case 14: options.startup.request_dynamic_interpreter = true; expected = "XEIMG0017"; break;
            case 15: input.sections[0].memory_size = 0U; expected = "XEIMG0020"; break;
            case 16: input.sections[0].alignment = 3U; expected = "XEIMG0020"; break;
            case 17: input.sections[0].address += 1U; expected = "XEIMG0009"; break;
            case 18: input.sections[0].memory_size += 8U; expected = "XEIMG0006"; break;
            case 19: input.sections[0].contents.pop_back(); expected = "XEIMG0012"; break;
            case 20: input.flat_bytes[4U] ^= 0xffU; expected = "XEIMG0018"; break;
            case 21: input.symbols[0].kind = ObjectSymbolKind::Object; expected = "XEIMG0004"; break;
            case 22: input.symbols[0].binding = ObjectSymbolBinding::Local; expected = "XEIMG0004"; break;
            case 23: input.entry_address += 200U; input.symbols[0].address += 200U; expected = "XEIMG0019"; break;
            case 24: {
                LinkedImageSection data;
                data.object_id = "bad-data"; data.source_section_id = 2U; data.name = ".data";
                data.kind = ObjectSectionKind::Data; data.address = 0x400060U; data.alignment = 16U;
                data.memory_size = 16U; data.contents.assign(16U, 0x22U);
                input.sections.push_back(data); input.image_size += 16U; input.flat_bytes.resize(input.image_size, 0U);
                std::copy(data.contents.begin(), data.contents.end(), input.flat_bytes.begin() + 96);
                expected = "XEIMG0010"; break;
            }
            case 25: {
                input.sections[0].kind = ObjectSectionKind::Bss;
                input.sections[0].contents.assign(input.sections[0].memory_size, 0x7fU);
                expected = "XEIMG0012"; break;
            }
            case 26: {
                input.sections.clear(); expected = "XEIMG0005"; break;
            }
            case 27: {
                input.image_size = 0U; input.flat_bytes.clear(); expected = "XEIMG0005"; break;
            }
            case 28: {
                input.sections[0].address = input.image_base + input.image_size + 1U;
                input.sections[0].alignment = 1U;
                expected = "XEIMG0006"; break;
            }
            case 29: {
                input.symbols[0].address += 1U; expected = "XEIMG0004"; break;
            }
            case 30: {
                options.maximum_file_size = 4096U; expected = "XEIMG0013"; break;
            }
            case 31: {
                input.sections[0].contents.clear(); expected = "XEIMG0012"; break;
            }
            case 32: {
                input.sections[0].kind = static_cast<ObjectSectionKind>(99);
                expected = "XEIMG0007"; break;
            }
            case 33: {
                input.image_base = std::numeric_limits<std::uint64_t>::max() - 8U;
                expected = "XEIMG0006"; break;
            }
            case 34: {
                options.page_size = 2U * 1024U * 1024U; expected = "XEIMG0002"; break;
            }
            case 35: {
                input.sections[0].address = 0x400000U;
                input.sections[0].memory_size = std::numeric_limits<std::size_t>::max();
                expected = "XEIMG0006"; break;
            }
            case 36: {
                input.sections[0].contents[0] ^= 1U; expected = "XEIMG0018"; break;
            }
            case 37: {
                LinkedImageSection bss;
                bss.object_id = "bss-first"; bss.source_section_id = 2U; bss.name = ".bss";
                bss.kind = ObjectSectionKind::Bss; bss.address = 0x401000U; bss.alignment = 4096U;
                bss.memory_size = 16U; bss.contents.assign(16U, 0U);
                LinkedImageSection data;
                data.object_id = "data-after-bss"; data.source_section_id = 3U; data.name = ".data";
                data.kind = ObjectSectionKind::Data; data.address = 0x401010U; data.alignment = 16U;
                data.memory_size = 16U; data.contents.assign(16U, 0x55U);
                input.sections.push_back(bss); input.sections.push_back(data);
                input.image_size = static_cast<std::size_t>(0x401020U - input.image_base);
                input.flat_bytes.resize(input.image_size, 0U);
                std::copy(data.contents.begin(), data.contents.end(), input.flat_bytes.begin() + 0x1010);
                expected = "XEIMG0012"; break;
            }
            case 38: {
                input.sections[0].address = input.image_base + 8U;
                input.sections[0].alignment = 1U;
                input.image_size = 128U; input.flat_bytes.resize(128U, 0U);
                input.sections[0].contents.resize(96U, 0x90U);
                std::copy(input.sections[0].contents.begin(), input.sections[0].contents.end(), input.flat_bytes.begin() + 8);
                input.entry_address = input.image_base + 8U; input.symbols[0].address = input.entry_address;
                expected = "XEIMG0010"; break;
            }
            case 39: {
                input.sections[0].address = 0x401000U;
                input.image_base = 0x400000U;
                input.image_size = 96U;
                expected = "XEIMG0006"; break;
            }
            }
            require_failure(input, options, expected);
        }, passed, failed);
    }
}
