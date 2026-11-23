#include "xfrontend/pe64_advanced.hpp"
#include "xfrontend/binary_object.hpp"
#include "xfrontend/archive.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xfrontend;
namespace {

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
void test_case(const std::string& id, const std::function<void()>& body, int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(id)) return;
    const auto started = std::chrono::steady_clock::now();
    try {
        body(); ++passed;
        xfrontend_test_report::record(id, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    } catch (const std::exception& error) {
        ++failed;
        xfrontend_test_report::record(id, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    }
}
std::uint16_t u16(const std::vector<std::uint8_t>& b, std::size_t o) {
    require(o <= b.size() && b.size() - o >= 2U, "u16 read outside PE image");
    return static_cast<std::uint16_t>(b[o]) | static_cast<std::uint16_t>(static_cast<std::uint16_t>(b[o + 1U]) << 8U);
}
std::uint32_t u32(const std::vector<std::uint8_t>& b, std::size_t o) {
    require(o <= b.size() && b.size() - o >= 4U, "u32 read outside PE image");
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1U]) << 8U) |
        (static_cast<std::uint32_t>(b[o + 2U]) << 16U) | (static_cast<std::uint32_t>(b[o + 3U]) << 24U);
}
std::uint64_t u64(const std::vector<std::uint8_t>& b, std::size_t o) {
    return static_cast<std::uint64_t>(u32(b, o)) | (static_cast<std::uint64_t>(u32(b, o + 4U)) << 32U);
}
const Pe64DllSectionInfo& section(const Pe64DllImage& image, const std::string& name) {
    const auto found = std::find_if(image.sections.begin(), image.sections.end(), [&name](const auto& s) { return s.name == name; });
    require(found != image.sections.end(), "missing PE section " + name); return *found;
}
std::size_t file_offset(const Pe64DllImage& image, std::uint32_t rva, std::size_t size) {
    for (const auto& s : image.sections) {
        if (rva < s.virtual_address) continue;
        const std::uint64_t delta = static_cast<std::uint64_t>(rva) - s.virtual_address;
        if (delta <= s.raw_size && size <= static_cast<std::size_t>(s.raw_size - delta))
            return static_cast<std::size_t>(s.raw_offset) + static_cast<std::size_t>(delta);
    }
    throw std::runtime_error("RVA does not map to initialized section data");
}
std::size_t optional_header(const Pe64DllImage& image) { return static_cast<std::size_t>(u32(image.bytes, 0x3cU)) + 24U; }
Pe64DllSpec base_dll(std::size_t index) {
    Pe64DllSpec s; s.dll_name = "n39_n43_fixture_" + std::to_string(index) + ".dll";
    s.image_base = 0x180000000ULL + static_cast<std::uint64_t>(index) * 0x10000ULL;
    s.text.assign(512U, 0x90U); s.data.assign(32U, 0U);
    s.exports = {{"N39N43Entry", 1U, Pe64SectionKind::Text, 0U, false, {}}};
    return s;
}
Pe64DllRuntimeFunction unwind_function(std::size_t begin, std::size_t end) {
    Pe64DllRuntimeFunction f; f.begin_offset = begin; f.end_offset = end; f.prolog_size = 4U;
    f.unwind_codes = {{4U, Pe64DllUnwindOperation::AllocSmall, 0U, 0U}}; return f;
}
Pe64ResourceItem resource(std::uint16_t type, std::uint16_t name, std::uint16_t lang, std::size_t size) {
    Pe64ResourceItem r; r.type_id = type; r.name_id = name; r.language_id = lang; r.code_page = 1200U;
    r.data.resize(size); for (std::size_t i = 0; i < size; ++i) r.data[i] = static_cast<std::uint8_t>((i + type + name + lang) & 0xffU);
    return r;
}
Pe64DllAdvancedSpec resource_spec(std::size_t index) {
    Pe64DllAdvancedSpec a; a.resources = {
        resource(3U, static_cast<std::uint16_t>(1U + index % 50U), 1033U, 24U + index % 13U),
        resource(24U, 1U, 1033U, 48U), resource(16U, 1U, 1033U, 32U),
        resource(6U, 1U, 1033U, 20U)};
    return a;
}
Pe64DllAdvancedSpec load_config_spec(std::size_t index) {
    Pe64DllAdvancedSpec a; a.load_config.enabled = true; a.load_config.has_security_cookie = true;
    a.load_config.security_cookie_data_offset = 0U; a.load_config.guard_cf_enabled = true;
    a.load_config.guard_cf_check_pointer_data_offset = 8U;
    a.load_config.guard_cf_check_target_text_offset = 8U;
    a.load_config.guard_cf_dispatch_pointer_data_offset = 16U;
    a.load_config.guard_cf_dispatch_target_text_offset = 16U;
    a.load_config.guard_cf_function_offsets = {32U + (index % 8U), 64U, 96U}; return a;
}
Pe64DllAdvancedSpec delay_spec(std::size_t index) {
    Pe64DllAdvancedSpec a;
    a.delay_imports = {
        {Pe64Import{"KERNEL32.dll", "GetCurrentProcessId", static_cast<std::uint16_t>(index % 32U), Pe64ImportKind::ByName, 0U}, 0U},
        {Pe64Import{"KERNEL32.dll", "ExitProcess", 0U, Pe64ImportKind::ByName, 0U}, 0U},
        {Pe64Import{"USER32.dll", "", 0U, Pe64ImportKind::ByOrdinal, static_cast<std::uint16_t>(1U + index % 300U)}, 0U}};
    a.bound_imports = {{"KERNEL32.dll", 0U}, {"USER32.dll", 0U}};
    return a;
}

ObjectFile main_coff_object(std::size_t index) {
    ObjectFile o; o.object_id = "main_" + std::to_string(index); o.descriptor = make_coff_x86_64_object_target();
    ObjectSection s; s.id = 1U; s.name = ".text"; s.kind = ObjectSectionKind::Text; s.alignment = 16U;
    s.size = 8U; s.contents = {0xe8U, 0U, 0U, 0U, 0U, 0xc3U, 0x90U, 0x90U}; o.sections.push_back(s);
    ObjectSymbol entry; entry.name = "EntryPoint"; entry.signature_key = "fn()->i32"; entry.binding = ObjectSymbolBinding::Global;
    entry.kind = ObjectSymbolKind::Function; entry.defined = true; entry.section_id = 1U; entry.value = 0U; entry.size = 6U;
    ObjectSymbol helper; helper.name = "Helper"; helper.signature_key = "fn()->i32"; helper.binding = ObjectSymbolBinding::Global;
    helper.kind = ObjectSymbolKind::Function; helper.defined = false;
    o.symbols = {entry, helper};
    ObjectRelocation r; r.section_id = 1U; r.offset = 1U; r.kind = ObjectRelocationKind::CoffRelative32;
    r.target_symbol = "Helper"; r.addend = 0; r.width = 4U; o.relocations.push_back(r);
    return o;
}
ObjectFile helper_coff_object(std::size_t index) {
    ObjectFile o; o.object_id = "helper_" + std::to_string(index); o.descriptor = make_coff_x86_64_object_target();
    ObjectSection s; s.id = 1U; s.name = ".text"; s.kind = ObjectSectionKind::Text; s.alignment = 16U;
    s.size = 8U; s.contents = {0xb8U, 42U, 0U, 0U, 0U, 0xc3U, 0x90U, 0x90U}; o.sections.push_back(s);
    ObjectSymbol helper; helper.name = "Helper"; helper.signature_key = "fn()->i32"; helper.binding = ObjectSymbolBinding::Global;
    helper.kind = ObjectSymbolKind::Function; helper.defined = true; helper.section_id = 1U; helper.value = 0U; helper.size = 6U;
    o.symbols.push_back(helper); return o;
}
std::vector<std::uint8_t> coff_object_bytes(const ObjectFile& object) {
    const auto encoded = write_binary_object(object);
    require(encoded.succeeded(), "AMD64 COFF object writer rejected fixture");
    return encoded.bytes;
}

Pe64CoffDllLinkSpec coff_link_spec(std::size_t index) {
    Pe64CoffDllLinkSpec s; s.dll_name = "n43_coff_link_" + std::to_string(index) + ".dll";
    s.image_base = 0x180000000ULL + static_cast<std::uint64_t>(index) * 0x10000ULL;
    s.coff_object_files = {coff_object_bytes(main_coff_object(index)), coff_object_bytes(helper_coff_object(index))};
    s.exports = {{"EntryPoint", 1U}}; return s;
}

void n39_handler_record(std::size_t index) {
    auto spec = base_dll(index); auto f = unwind_function(0U, 40U);
    f.flags = static_cast<std::uint8_t>(1U + (index % 3U)); f.handler_offset = 160U;
    f.handler_data = {0xdeU, 0xadU, static_cast<std::uint8_t>(index & 0xffU), 0xefU};
    spec.runtime_functions = {f};
    const auto out = write_pe64_dll_image(spec);
    require(out.succeeded(), "valid EHANDLER/UHANDLER metadata was rejected");
    const auto xdata = section(out.image, ".xdata");
    const std::size_t record = file_offset(out.image, xdata.virtual_address, 4U);
    require((out.image.bytes[record] >> 3U) == f.flags, "UNWIND_INFO handler flags mismatch");
    const std::size_t tail = record + 4U + ((static_cast<std::size_t>(out.image.bytes[record + 2U]) + 1U) & ~std::size_t{1U}) * 2U;
    require(u32(out.image.bytes, tail) == section(out.image, ".text").virtual_address + f.handler_offset,
            "handler RVA does not point into executable .text");
    require(out.image.bytes[tail + 4U] == 0xdeU && out.image.bytes[tail + 5U] == 0xadU,
            "language-specific handler payload was not preserved");
}
void n39_chained_record(std::size_t index) {
    auto spec = base_dll(index); auto parent = unwind_function(64U, 96U); auto chained = unwind_function(0U, 32U);
    chained.flags = 4U; chained.chained_function_index = 0U; spec.runtime_functions = {parent, chained};
    const auto out = write_pe64_dll_image(spec); require(out.succeeded(), "valid chained unwind record was rejected");
    const auto xdata = section(out.image, ".xdata"); const std::size_t xoff = static_cast<std::size_t>(xdata.raw_offset);
    const std::size_t record = xoff; // sorted first function is the chained record at .text offset 0
    require((out.image.bytes[record] >> 3U) == 4U, "UNW_FLAG_CHAININFO was not encoded");
    const std::size_t tail = record + 4U + ((static_cast<std::size_t>(out.image.bytes[record + 2U]) + 1U) & ~std::size_t{1U}) * 2U;
    const auto text = section(out.image, ".text");
    require(u32(out.image.bytes, tail) == text.virtual_address + 64U &&
            u32(out.image.bytes, tail + 4U) == text.virtual_address + 96U,
            "CHAININFO RUNTIME_FUNCTION begin/end mismatch");
    require(u32(out.image.bytes, tail + 8U) == xdata.virtual_address + 20U,
            "CHAININFO unwind RVA does not reference chained target metadata");
}
void n39_extended_operations(std::size_t index) {
    auto spec = base_dll(index); auto f = unwind_function(0U, 48U); f.prolog_size = 12U;
    f.unwind_codes = {{10U, Pe64DllUnwindOperation::AllocLarge, 1U, 70000U},
                      {5U, Pe64DllUnwindOperation::PushMachineFrame, static_cast<std::uint8_t>(index % 2U), 0U}};
    spec.runtime_functions = {f}; const auto out = write_pe64_dll_image(spec);
    require(out.succeeded(), "valid ALLOC_LARGE OpInfo=1 / PUSH_MACHFRAME was rejected");
    const auto xdata = section(out.image, ".xdata"); const std::size_t off = file_offset(out.image, xdata.virtual_address, 16U);
    require(out.image.bytes[off + 4U] == 10U && out.image.bytes[off + 5U] == 0x11U, "ALLOC_LARGE OpInfo=1 opcode mismatch");
    require(u16(out.image.bytes, off + 6U) == static_cast<std::uint16_t>(70000U & 0xffffU) &&
            u16(out.image.bytes, off + 8U) == static_cast<std::uint16_t>(70000U >> 16U), "ALLOC_LARGE 32-bit operand mismatch");
    require(out.image.bytes[off + 10U] == 5U && (out.image.bytes[off + 11U] & 0x0fU) == 10U,
            "PUSH_MACHFRAME opcode encoding mismatch");
}
void n39_invalid_unwind(std::size_t index) {
    auto spec = base_dll(index); auto f = unwind_function(0U, 32U);
    switch (index % 7U) {
    case 0U: f.flags = 5U; break;
    case 1U: f.flags = 4U; f.chained_function_index = 99U; break;
    case 2U: f.flags = 4U; f.chained_function_index = 0U; break;
    case 3U: f.flags = 1U; f.handler_offset = spec.text.size(); break;
    case 4U: f.flags = 0U; f.handler_data = {1U}; break;
    case 5U: f.flags = 1U; f.chained_function_index = 0U; break;
    default: {
        auto second = unwind_function(64U, 96U);
        f.flags = 4U; f.chained_function_index = 1U;
        second.flags = 4U; second.chained_function_index = 0U;
        spec.runtime_functions = {f, second};
        const auto out = write_pe64_dll_image(spec);
        require(!out.succeeded() && out.image.bytes.empty() && !out.diagnostics.empty(),
                "cyclic CHAININFO graph was not rejected");
        return;
    }
    }
    spec.runtime_functions = {f}; const auto out = write_pe64_dll_image(spec);
    require(!out.succeeded() && out.image.bytes.empty() && !out.diagnostics.empty(), "invalid handler/chained metadata was not rejected atomically");
}

void n40_resource_tree(std::size_t index) {
    auto base = base_dll(index); auto advanced = resource_spec(index);
    const auto out = write_pe64_dll_image_advanced(base, advanced);
    require(out.succeeded(), "valid numeric resource tree was rejected");
    require(out.advanced.resource_count == 4U && out.advanced.resource_directory_rva != 0U,
            "resource directory counters or RVA mismatch");
    const auto rsrc = section(out.image, ".rsrc");
    require(out.advanced.resource_directory_size == rsrc.virtual_size, "resource directory size must cover .rsrc tree and payloads");
    const std::size_t root = file_offset(out.image, out.advanced.resource_directory_rva, 16U);
    require(u16(out.image.bytes, root + 12U) == 0U && u16(out.image.bytes, root + 14U) == 4U,
            "resource root directory must contain four numeric type IDs");
    const std::size_t opt = optional_header(out.image);
    require(u32(out.image.bytes, opt + 112U + 2U * 8U) == out.advanced.resource_directory_rva &&
            u32(out.image.bytes, opt + 112U + 2U * 8U + 4U) == out.advanced.resource_directory_size,
            "IMAGE_DIRECTORY_ENTRY_RESOURCE mismatch");
}
void n40_resource_stability(std::size_t index) {
    auto base = base_dll(index); auto advanced = resource_spec(index);
    advanced.resources.push_back(resource(5U, static_cast<std::uint16_t>(2U + index % 200U), 1033U, 64U));
    const auto first = write_pe64_dll_image_advanced(base, advanced);
    const auto second = write_pe64_dll_image_advanced(base, advanced);
    require(first.succeeded() && second.succeeded() && first.image.bytes == second.image.bytes,
            "identical resource inputs must produce byte-identical PE images");
    require(first.advanced.resource_count == 5U, "resource count did not include all leaves");
}
void n40_resource_reject(std::size_t index) {
    auto base = base_dll(index); auto advanced = resource_spec(index);
    switch (index % 4U) {
    case 0U: advanced.resources[1U] = advanced.resources[0U]; break;
    case 1U: advanced.resources[0U].type_id = 0U; break;
    case 2U: advanced.resources[0U].data.clear(); break;
    default: advanced.resources.resize(4097U, resource(3U, 1U, 1033U, 1U)); break;
    }
    const auto out = write_pe64_dll_image_advanced(base, advanced);
    require(!out.succeeded() && out.image.bytes.empty() && !out.diagnostics.empty(), "invalid resource tree was not rejected atomically");
}
void n40_resource_section_coexistence(std::size_t index) {
    auto base = base_dll(index); base.imports = {{"KERNEL32.dll", "GetCurrentProcessId", 0U, Pe64ImportKind::ByName, 0U}};
    base.runtime_functions = {unwind_function(0U, 32U)}; auto advanced = resource_spec(index);
    const auto out = write_pe64_dll_image_advanced(base, advanced);
    require(out.succeeded(), "resources did not coexist with import and unwind directories");
    require(out.image.import_directory_rva != 0U && out.image.exception_directory_rva != 0U &&
            out.advanced.resource_directory_rva != 0U, "one of the integrated PE data directories disappeared");
}

void n41_load_config_guard_cf(std::size_t index) {
    auto base = base_dll(index); auto advanced = load_config_spec(index);
    const auto out = write_pe64_dll_image_advanced(base, advanced);
    require(out.succeeded(), "valid Load Config / GuardCF metadata was rejected");
    const auto lcfg = section(out.image, ".lcfg"); const std::size_t off = file_offset(out.image, out.advanced.load_config_directory_rva, 0x94U);
    require(u32(out.image.bytes, off) == 0x94U, "IMAGE_LOAD_CONFIG_DIRECTORY64 Size mismatch");
    require(u64(out.image.bytes, off + 0x58U) == out.image.image_base + section(out.image, ".data").virtual_address,
            "SecurityCookie VA mismatch");
    const auto data = section(out.image, ".data");
    const auto text = section(out.image, ".text");
    require(u64(out.image.bytes, off + 0x70U) == out.image.image_base + data.virtual_address + 8U &&
            u64(out.image.bytes, off + 0x78U) == out.image.image_base + data.virtual_address + 16U,
            "GuardCF check/dispatch pointer-cell VA mismatch");
    require(u64(out.image.bytes, static_cast<std::size_t>(data.raw_offset) + 8U) == out.image.image_base + text.virtual_address + 8U &&
            u64(out.image.bytes, static_cast<std::size_t>(data.raw_offset) + 16U) == out.image.image_base + text.virtual_address + 16U,
            "GuardCF check/dispatch function-pointer cell contents mismatch");
    const std::uint64_t table_va = u64(out.image.bytes, off + 0x80U);
    require(table_va == out.image.image_base + lcfg.virtual_address + 0x94U, "GuardCFFunctionTable VA mismatch");
    require(u64(out.image.bytes, off + 0x88U) == 3U && u32(out.image.bytes, off + 0x90U) == 0x500U,
            "GuardCF function count or flags mismatch");
    const std::size_t table = file_offset(out.image, static_cast<std::uint32_t>(table_va - out.image.image_base), 12U);
    require(u32(out.image.bytes, table) < u32(out.image.bytes, table + 4U) &&
            u32(out.image.bytes, table + 4U) < u32(out.image.bytes, table + 8U), "GuardCF function table must be sorted");
    const std::size_t opt = optional_header(out.image);
    require(u32(out.image.bytes, opt + 112U + 10U * 8U) == lcfg.virtual_address &&
            u32(out.image.bytes, opt + 112U + 10U * 8U + 4U) == 0x94U, "Load Configuration data directory mismatch");
}
void n41_relocation_merge(std::size_t index) {
    auto base = base_dll(index); base.absolute_relocations = {{24U, Pe64SectionKind::Text, 24U, 0}};
    auto advanced = load_config_spec(index); const auto out = write_pe64_dll_image_advanced(base, advanced);
    require(out.succeeded() && out.image.has_dynamic_base, "Load Config VA pointers must retain ASLR support");
    const auto reloc = section(out.image, ".relx");
    const std::size_t opt = optional_header(out.image);
    const auto dir_rva = u32(out.image.bytes, opt + 112U + 5U * 8U);
    const auto dir_size = u32(out.image.bytes, opt + 112U + 5U * 8U + 4U);
    require(dir_rva == reloc.virtual_address && dir_size == reloc.virtual_size, "merged relocation directory does not reference .relx");
    std::size_t cursor = file_offset(out.image, dir_rva, dir_size), end = cursor + dir_size;
    bool found_cookie = false, found_guard = false; std::size_t entries = 0U;
    while (cursor < end) {
        const auto page = u32(out.image.bytes, cursor); const auto block = u32(out.image.bytes, cursor + 4U);
        require(block >= 8U && (block & 1U) == 0U && block <= end - cursor, "invalid relocation block in merged table");
        for (std::size_t at = cursor + 8U; at < cursor + block; at += 2U) {
            const auto entry = u16(out.image.bytes, at); const auto kind = entry >> 12U;
            if (kind == 10U) {
                ++entries; const auto target = page + (entry & 0x0fffU);
                if (target == out.advanced.load_config_directory_rva + 0x58U) found_cookie = true;
                if (target == out.advanced.load_config_directory_rva + 0x80U) found_guard = true;
            }
        }
        cursor += block;
    }
    const auto data = section(out.image, ".data");
    bool found_check_cell = false, found_dispatch_cell = false;
    std::size_t scan = file_offset(out.image, dir_rva, dir_size);
    while (scan < end) {
        const auto page = u32(out.image.bytes, scan); const auto block = u32(out.image.bytes, scan + 4U);
        for (std::size_t at = scan + 8U; at < scan + block; at += 2U) {
            const auto entry = u16(out.image.bytes, at);
            if ((entry >> 12U) == 10U) {
                const auto target = page + (entry & 0x0fffU);
                if (target == data.virtual_address + 8U) found_check_cell = true;
                if (target == data.virtual_address + 16U) found_dispatch_cell = true;
            }
        }
        scan += block;
    }
    require(cursor == end && entries >= 7U && found_cookie && found_guard && found_check_cell && found_dispatch_cell,
            "merged DIR64 table lost original, GuardCF pointer-cell or SecurityCookie relocations");
    require((u16(out.image.bytes, u32(out.image.bytes, 0x3cU) + 22U) & 1U) == 0U,
            "COFF RELOCS_STRIPPED flag must be cleared when .relx is present");
}
void n41_invalid_load_config(std::size_t index) {
    auto base = base_dll(index); auto advanced = load_config_spec(index);
    switch (index % 4U) {
    case 0U: advanced.load_config.security_cookie_data_offset = base.data.size() - 4U; break;
    case 1U: advanced.load_config.guard_cf_function_offsets.clear(); break;
    case 2U: advanced.load_config.guard_cf_function_offsets.push_back(64U); break;
    default: advanced.load_config.guard_cf_check_target_text_offset = base.text.size(); break;
    }
    const auto out = write_pe64_dll_image_advanced(base, advanced);
    require(!out.succeeded() && out.image.bytes.empty() && !out.diagnostics.empty(), "invalid Load Config / GuardCF data was not rejected");
}
void n41_plain_load_config(std::size_t index) {
    auto base = base_dll(index); Pe64DllAdvancedSpec advanced; advanced.load_config.enabled = true;
    const auto out = write_pe64_dll_image_advanced(base, advanced);
    require(out.succeeded() && out.advanced.load_config_directory_size == 0x94U,
            "minimal 64-bit Load Config directory should serialize without optional CFG pointers");
    require(out.advanced.advanced_relocation_directory_rva == 0U,
            "Load Config without VA fields must not manufacture an empty relocation directory");
}

void n42_delay_imports(std::size_t index) {
    auto base = base_dll(index); auto advanced = delay_spec(index);
    const auto out = write_pe64_dll_image_advanced(base, advanced);
    require(out.succeeded(), "valid Delay Import / Bound Import directories were rejected");
    const auto didat = section(out.image, ".didat"); const auto bnd = section(out.image, ".bnd");
    const std::size_t desc = file_offset(out.image, out.advanced.delay_import_directory_rva, 96U);
    require(u32(out.image.bytes, desc) == 1U && u32(out.image.bytes, desc + 32U) == 1U &&
            u32(out.image.bytes, desc + 64U) == 0U,
            "Delay Import descriptors must use RVA-based attributes and a null terminator");
    require(u32(out.image.bytes, desc + 4U) >= didat.virtual_address &&
            u32(out.image.bytes, desc + 8U) >= didat.virtual_address &&
            u32(out.image.bytes, desc + 12U) >= didat.virtual_address &&
            u32(out.image.bytes, desc + 16U) >= didat.virtual_address,
            "Delay Import descriptor name/module/IAT/INT RVA fields are invalid");
    const auto int_rva = u32(out.image.bytes, desc + 32U + 16U);
    const std::size_t int_off = file_offset(out.image, int_rva, 16U);
    require((u64(out.image.bytes, int_off) & 0x8000000000000000ULL) != 0U,
            "ordinal delay import must set the 64-bit ordinal flag");
    const std::size_t opt = optional_header(out.image);
    require(u32(out.image.bytes, opt + 112U + 13U * 8U) == didat.virtual_address &&
            u32(out.image.bytes, opt + 112U + 11U * 8U) == bnd.virtual_address,
            "Delay Import or Bound Import data directory RVA mismatch");
    const std::size_t bnd_off = file_offset(out.image, bnd.virtual_address, 24U);
    require(u16(out.image.bytes, bnd_off + 6U) == 0U && u16(out.image.bytes, bnd_off + 14U) == 0U,
            "Bound Import descriptors without forwarders must have zero forwarder counts");
}
void n42_delay_import_validation(std::size_t index) {
    auto base = base_dll(index); auto advanced = delay_spec(index);
    if ((index & 1U) == 0U) advanced.delay_imports[1U] = advanced.delay_imports[0U];
    else advanced.delay_imports[0U].import.dll_name.clear();
    const auto out = write_pe64_dll_image_advanced(base, advanced);
    require(!out.succeeded() && out.image.bytes.empty() && !out.diagnostics.empty(), "invalid or duplicate delay import was not rejected");
}
void n42_bound_import_validation(std::size_t index) {
    auto base = base_dll(index); Pe64DllAdvancedSpec advanced;
    advanced.bound_imports = {{"KERNEL32.dll", 0U}, {"kernel32.DLL", 0U}};
    const auto out = write_pe64_dll_image_advanced(base, advanced);
    require(!out.succeeded() && out.image.bytes.empty() && !out.diagnostics.empty(), "case-insensitive duplicate bound import was not rejected");
}
void n42_coexistence(std::size_t index) {
    auto base = base_dll(index); base.imports = {{"KERNEL32.dll", "GetCurrentProcessId", 0U, Pe64ImportKind::ByName, 0U}};
    auto advanced = delay_spec(index); advanced.resources = resource_spec(index).resources;
    const auto out = write_pe64_dll_image_advanced(base, advanced);
    require(out.succeeded() && out.image.import_directory_rva != 0U && out.advanced.delay_import_directory_rva != 0U &&
            out.advanced.resource_directory_rva != 0U && out.advanced.bound_import_directory_rva != 0U,
            "ordinary imports, delay imports, bound imports and resources must coexist");
}

void n43_coff_to_dll(std::size_t index) {
    auto spec = coff_link_spec(index); const auto out = link_coff_objects_to_pe64_dll(spec);
    require(out.succeeded(), "COFF-to-PE DLL link failed" + (out.diagnostics.empty() ? std::string{} : ": " + out.diagnostics.front().code));
    require(out.selected_object_count == 2U && out.applied_relocation_count == 1U,
            "COFF object selection or relocation count mismatch");
    const auto text = section(out.output.image, ".text");
    const std::size_t call = static_cast<std::size_t>(text.raw_offset);
    require(out.output.image.bytes[call] == 0xe8U && static_cast<std::int32_t>(u32(out.output.image.bytes, call + 1U)) == 11,
            "final page-aligned PE REL32 displacement was not recomputed against Helper");
    require(out.output.image.export_directory_rva != 0U && out.output.image.export_name_count == 1U,
            "COFF global symbol did not become a PE export");
}
void n43_archive_extraction(std::size_t index) {
    auto main = main_coff_object(index); auto helper = helper_coff_object(index);
    Pe64CoffDllLinkSpec spec; spec.dll_name = "n43_archive_" + std::to_string(index) + ".dll";
    spec.coff_object_files = {coff_object_bytes(main)};
    const auto library = write_object_archive(ArchiveFormat::CoffLib, {{"helper.obj", helper}}, true);
    require(library.succeeded(), "AMD64 COFF LIB writer rejected helper member");
    spec.coff_library_files = {library.bytes};
    spec.exports = {{"EntryPoint", 1U}};
    const auto out = link_coff_objects_to_pe64_dll(spec);
    require(out.succeeded() && out.selected_object_count == 2U && out.extracted_archive_member_count == 1U,
            "COFF static archive extraction was not carried through to PE output");
}
void n43_binary_object_pipeline(std::size_t index) {
    auto main = main_coff_object(index); auto helper = helper_coff_object(index);
    const auto binary = write_binary_object(main); require(binary.succeeded(), "AMD64 COFF object writer rejected valid input");
    const auto decoded = read_binary_object(binary.bytes); require(decoded.succeeded(), "AMD64 COFF object reader rejected writer output");
    Pe64CoffDllLinkSpec spec; spec.dll_name = "n43_obj_roundtrip_" + std::to_string(index) + ".dll";
    spec.coff_object_files = {binary.bytes, coff_object_bytes(helper)}; spec.exports = {{"EntryPoint", 1U}};
    const auto out = link_coff_objects_to_pe64_dll(spec);
    require(out.succeeded(), "binary .obj read -> COFF linker -> PE DLL pipeline failed");
}
void n43_invalid_coff_link(std::size_t index) {
    auto spec = coff_link_spec(index);
    switch (index % 4U) {
    case 0U: spec.exports[0U].symbol_name = "MissingExport"; break;
    case 1U: spec.coff_object_files[1U] = spec.coff_object_files[0U]; break;
    case 2U: spec.coff_object_files.pop_back(); break;
    default: spec.dll_name.clear(); break;
    }
    const auto out = link_coff_objects_to_pe64_dll(spec);
    require(!out.succeeded() && !out.diagnostics.empty(), "invalid COFF-to-PE link graph was not rejected");
}

} // namespace

void run_pe64_n39_n43_tests(int& passed, int& failed) {
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N39A" + std::to_string(i + 1U) + "_unwind_exception_termination_handlers", [i] { n39_handler_record(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N39B" + std::to_string(i + 1U) + "_unwind_chained_runtime_functions", [i] { n39_chained_record(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N39C" + std::to_string(i + 1U) + "_unwind_extended_opcode_encodings", [i] { n39_extended_operations(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N39D" + std::to_string(i + 1U) + "_unwind_invalid_metadata_rejection", [i] { n39_invalid_unwind(i); }, passed, failed);

    for (std::size_t i = 0U; i < 500U; ++i) test_case("N40A" + std::to_string(i + 1U) + "_pe_resource_tree_layout", [i] { n40_resource_tree(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N40B" + std::to_string(i + 1U) + "_pe_resource_determinism_stress", [i] { n40_resource_stability(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N40C" + std::to_string(i + 1U) + "_pe_resource_validation_rejections", [i] { n40_resource_reject(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N40D" + std::to_string(i + 1U) + "_pe_resource_directory_coexistence", [i] { n40_resource_section_coexistence(i); }, passed, failed);

    for (std::size_t i = 0U; i < 500U; ++i) test_case("N41A" + std::to_string(i + 1U) + "_pe64_load_config_guard_cf", [i] { n41_load_config_guard_cf(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N41B" + std::to_string(i + 1U) + "_pe64_guard_cf_base_relocation_merge", [i] { n41_relocation_merge(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N41C" + std::to_string(i + 1U) + "_pe64_load_config_invalid_metadata", [i] { n41_invalid_load_config(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N41D" + std::to_string(i + 1U) + "_pe64_minimal_load_config", [i] { n41_plain_load_config(i); }, passed, failed);

    for (std::size_t i = 0U; i < 500U; ++i) test_case("N42A" + std::to_string(i + 1U) + "_pe64_delay_and_bound_import_layout", [i] { n42_delay_imports(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N42B" + std::to_string(i + 1U) + "_pe64_delay_import_rejection_matrix", [i] { n42_delay_import_validation(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N42C" + std::to_string(i + 1U) + "_pe64_bound_import_rejection_matrix", [i] { n42_bound_import_validation(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N42D" + std::to_string(i + 1U) + "_pe64_delay_import_full_directory_coexistence", [i] { n42_coexistence(i); }, passed, failed);

    for (std::size_t i = 0U; i < 500U; ++i) test_case("N43A" + std::to_string(i + 1U) + "_coff_objects_to_native_pe_dll", [i] { n43_coff_to_dll(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N43B" + std::to_string(i + 1U) + "_coff_static_archive_to_pe_dll", [i] { n43_archive_extraction(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N43C" + std::to_string(i + 1U) + "_coff_binary_object_roundtrip_pipeline", [i] { n43_binary_object_pipeline(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i) test_case("N43D" + std::to_string(i + 1U) + "_coff_link_failure_validation", [i] { n43_invalid_coff_link(i); }, passed, failed);
}
