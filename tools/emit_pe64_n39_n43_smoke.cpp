#include "xfrontend/archive.hpp"
#include "xfrontend/binary_object.hpp"
#include "xfrontend/pe64_advanced.hpp"
#include "xfrontend/pe64_debug_signing.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

bool write_file(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(output);
}

xfrontend::ObjectFile make_main_object() {
    xfrontend::ObjectFile object;
    object.object_id = "n39_n43_main";
    object.descriptor = xfrontend::make_coff_x86_64_object_target();

    xfrontend::ObjectSection text;
    text.id = 1U;
    text.name = ".text";
    text.kind = xfrontend::ObjectSectionKind::Text;
    text.alignment = 16U;
    text.size = 32U;
    text.contents.assign(32U, 0x90U);
    text.contents[0U] = 0xe8U; // call rel32 Helper
    text.contents[5U] = 0xc3U; // ret
    text.contents[16U] = 0xb8U; // DllMain: mov eax, TRUE; ret
    text.contents[17U] = 1U;
    text.contents[18U] = 0U;
    text.contents[19U] = 0U;
    text.contents[20U] = 0U;
    text.contents[21U] = 0xc3U;
    object.sections.push_back(text);

    xfrontend::ObjectSection data;
    data.id = 2U;
    data.name = ".data";
    data.kind = xfrontend::ObjectSectionKind::Data;
    data.alignment = 8U;
    data.size = 32U;
    data.contents.assign(32U, 0U);
    object.sections.push_back(data);

    xfrontend::ObjectSymbol entry;
    entry.name = "EntryPoint";
    entry.signature_key = "fn()->i32";
    entry.binding = xfrontend::ObjectSymbolBinding::Global;
    entry.kind = xfrontend::ObjectSymbolKind::Function;
    entry.defined = true;
    entry.section_id = 1U;
    entry.value = 0U;
    entry.size = 6U;

    xfrontend::ObjectSymbol dll_main;
    dll_main.name = "DllMain";
    dll_main.signature_key = "fn(ptr,i32,ptr)->i32";
    dll_main.binding = xfrontend::ObjectSymbolBinding::Global;
    dll_main.kind = xfrontend::ObjectSymbolKind::Function;
    dll_main.defined = true;
    dll_main.section_id = 1U;
    dll_main.value = 16U;
    dll_main.size = 6U;

    xfrontend::ObjectSymbol helper;
    helper.name = "Helper";
    helper.signature_key = "fn()->i32";
    helper.binding = xfrontend::ObjectSymbolBinding::Global;
    helper.kind = xfrontend::ObjectSymbolKind::Function;
    helper.defined = false;

    object.symbols = {entry, dll_main, helper};
    object.relocations.push_back(xfrontend::ObjectRelocation{
        1U, 1U, xfrontend::ObjectRelocationKind::CoffRelative32, "Helper", 0, 4U});
    return object;
}

xfrontend::ObjectFile make_helper_object() {
    xfrontend::ObjectFile object;
    object.object_id = "n39_n43_helper";
    object.descriptor = xfrontend::make_coff_x86_64_object_target();
    xfrontend::ObjectSection text;
    text.id = 1U;
    text.name = ".text";
    text.kind = xfrontend::ObjectSectionKind::Text;
    text.alignment = 16U;
    text.size = 8U;
    text.contents = {0xb8U, 42U, 0U, 0U, 0U, 0xc3U, 0x90U, 0x90U};
    object.sections.push_back(text);

    xfrontend::ObjectSymbol helper;
    helper.name = "Helper";
    helper.signature_key = "fn()->i32";
    helper.binding = xfrontend::ObjectSymbolBinding::Global;
    helper.kind = xfrontend::ObjectSymbolKind::Function;
    helper.defined = true;
    helper.section_id = 1U;
    helper.value = 0U;
    helper.size = 6U;
    object.symbols.push_back(helper);
    return object;
}

std::vector<std::uint8_t> encode_object(const xfrontend::ObjectFile& object) {
    const auto encoded = xfrontend::write_binary_object(object);
    if (!encoded.succeeded()) {
        for (const auto& item : encoded.diagnostics)
            std::cerr << item.code << ": " << item.message << '\n';
        return {};
    }
    return encoded.bytes;
}

} // namespace

int main(int argc, char** argv) {
    const bool include_n44_debug = argc == 3 && std::string(argv[2]) == "--n44";
    if (argc != 2 && !include_n44_debug) {
        std::cerr << "usage: emit_pe64_n39_n43_smoke <output.dll> [--n44]\n";
        return 2;
    }

    const auto main_object = encode_object(make_main_object());
    const auto helper_archive = xfrontend::write_object_archive(
        xfrontend::ArchiveFormat::CoffLib,
        {{"helper.obj", make_helper_object()}}, true);
    if (main_object.empty() || !helper_archive.succeeded()) {
        for (const auto& item : helper_archive.diagnostics)
            std::cerr << item.code << ": " << item.message << '\n';
        return 3;
    }

    xfrontend::Pe64CoffDllLinkSpec spec;
    spec.dll_name = "n39_n43_integrated.dll";
    spec.image_base = 0x180000000ULL;
    spec.coff_object_files = {main_object};
    spec.coff_library_files = {helper_archive.bytes};
    spec.exports = {{"EntryPoint", 1U}};
    spec.entry_symbol = "DllMain";
    spec.imports = {
        {"KERNEL32.dll", "GetCurrentProcessId", 0U, xfrontend::Pe64ImportKind::ByName, 0U}
    };
    // The fixture layout is intentionally deterministic: main .text occupies
    // bytes 0..31 and the 16-byte-aligned helper begins at byte 32.
    for (const auto& range : {std::pair<std::size_t, std::size_t>{0U, 6U}, {16U, 22U}, {32U, 38U}}) {
        xfrontend::Pe64DllRuntimeFunction function;
        function.begin_offset = range.first;
        function.end_offset = range.second;
        spec.runtime_functions.push_back(function);
    }

    xfrontend::Pe64ResourceItem resource;
    resource.type_id = 10U; // RT_RCDATA
    resource.name_id = 101U;
    resource.language_id = 1033U;
    resource.code_page = 1200U;
    resource.data = {'X', '3', '9', 'N', '4', '3'};
    spec.advanced.resources.push_back(resource);

    spec.advanced.load_config.enabled = true;
    spec.advanced.load_config.has_security_cookie = true;
    spec.advanced.load_config.security_cookie_data_offset = 0U;
    spec.advanced.delay_imports.push_back({
        {"KERNEL32.dll", "GetTickCount", 0U, xfrontend::Pe64ImportKind::ByName, 0U}, 0U});
    spec.advanced.bound_imports.push_back({"KERNEL32.dll", 0U});

    const auto linked = xfrontend::link_coff_objects_to_pe64_dll(spec);
    if (!linked.succeeded()) {
        for (const auto& item : linked.diagnostics)
            std::cerr << item.code << ": " << item.message << '\n';
        for (const auto& item : linked.output.diagnostics)
            std::cerr << item.code << ": " << item.message << '\n';
        return 4;
    }
    xfrontend::Pe64DllImage final_image = linked.output.image;
    if (include_n44_debug) {
        xfrontend::Pe64CodeViewSpec codeview;
        codeview.guid = {{0x58U, 0x4eU, 0x34U, 0x34U, 0x91U, 0x37U, 0x4aU, 0x62U,
                          0xa3U, 0x44U, 0x6bU, 0x19U, 0x77U, 0x20U, 0x11U, 0x44U}};
        codeview.age = 1U;
        codeview.timestamp = 0U; // deterministic/reproducible debug record
        codeview.pdb_path = "n44_integrated.pdb";
        const auto debugged = xfrontend::write_pe64_codeview_debug_directory(final_image, codeview);
        if (!debugged.succeeded()) {
            for (const auto& item : debugged.diagnostics)
                std::cerr << item.code << ": " << item.message << '\n';
            return 6;
        }
        const auto inspected = xfrontend::inspect_pe64_codeview_debug_directory(debugged.image.bytes);
        if (!inspected.valid || inspected.pdb_path != codeview.pdb_path || inspected.guid != codeview.guid) {
            std::cerr << "CodeView RSDS post-write inspection failed: " << inspected.error << '\n';
            return 7;
        }
        final_image = debugged.image;
        std::cout << "Debug Directory RVA: " << debugged.debug.directory_rva << '\n';
        std::cout << "CodeView RSDS RVA: " << debugged.debug.codeview_rva << '\n';
        std::cout << "CodeView PDB path: " << inspected.pdb_path << '\n';
        std::cout << "CodeView PDB age: " << inspected.age << '\n';
        std::cout << "CodeView RSDS verified: YES\n";
    }
    if (!write_file(argv[1], final_image.bytes)) {
        std::cerr << "cannot write generated PE32+ DLL\n";
        return 5;
    }

    std::cout << "Generated DLL: " << argv[1] << '\n';
    std::cout << "COFF objects selected: " << linked.selected_object_count << '\n';
    std::cout << "COFF archive members extracted: " << linked.extracted_archive_member_count << '\n';
    std::cout << "COFF relocations applied: " << linked.applied_relocation_count << '\n';
    std::cout << "PE bytes: " << final_image.bytes.size() << '\n';
    std::cout << "PE sections: " << final_image.sections.size() << '\n';
    std::cout << "Exception Directory RVA: " << linked.output.image.exception_directory_rva << '\n';
    std::cout << "Resource Directory RVA: " << linked.output.advanced.resource_directory_rva << '\n';
    std::cout << "Load Config Directory RVA: " << linked.output.advanced.load_config_directory_rva << '\n';
    std::cout << "Delay Import Directory RVA: " << linked.output.advanced.delay_import_directory_rva << '\n';
    std::cout << "Bound Import Directory RVA: " << linked.output.advanced.bound_import_directory_rva << '\n';
    std::cout << "Base Relocation Directory RVA: " << linked.output.image.relocation_directory_rva << '\n';
    std::cout << "DLL EntryPoint RVA: " << linked.output.image.entry_point_rva << '\n';
    return 0;
}
