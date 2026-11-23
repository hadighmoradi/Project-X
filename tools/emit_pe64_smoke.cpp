#include "xfrontend/pe64_image.hpp"

#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    const std::string output_path = argc > 1 ? argv[1] : "n30_pe64_multi_import_smoke.exe";
    xfrontend::Pe64ExecutableSpec spec;
    spec.text = xfrontend::make_pe64_multi_import_stub();
    spec.entry_point_offset = 0U;
    spec.imports.push_back(xfrontend::Pe64Import{"NTDLL.dll", "NtYieldExecution", 0U});
    spec.imports.push_back(xfrontend::Pe64Import{"KERNEL32.dll", "RtlLookupFunctionEntry", 0U});
    spec.imports.push_back(xfrontend::Pe64Import{"KERNEL32.dll", "GetCurrentProcessId", 0U});
    spec.imports.push_back(xfrontend::Pe64Import{"KERNEL32.dll", "ExitProcess", 0U});
    spec.iat_call_sites.push_back(xfrontend::Pe64IatCallSite{6U, "NTDLL.dll", "NtYieldExecution"});
    spec.iat_call_sites.push_back(xfrontend::Pe64IatCallSite{12U, "KERNEL32.dll", "GetCurrentProcessId"});
    spec.iat_call_sites.push_back(xfrontend::Pe64IatCallSite{33U, "KERNEL32.dll", "RtlLookupFunctionEntry"});
    spec.iat_call_sites.push_back(xfrontend::Pe64IatCallSite{53U, "KERNEL32.dll", "ExitProcess",
        xfrontend::Pe64IatInstructionKind::JumpIndirect});
    spec.iat_call_sites.push_back(xfrontend::Pe64IatCallSite{68U, "KERNEL32.dll", "ExitProcess",
        xfrontend::Pe64IatInstructionKind::JumpIndirect});
    spec.function_unwind.push_back(xfrontend::Pe64FunctionUnwind{0U, spec.text.size(), 4U,
        {xfrontend::Pe64UnwindCode{4U, xfrontend::Pe64UnwindOperation::AllocSmall, 0x28U}}});
    spec.data.assign(8U, 0U);
    spec.absolute_relocations.push_back(
        xfrontend::Pe64AbsoluteRelocation{0U, xfrontend::Pe64SectionKind::Text, 0U, 0});

    const auto result = xfrontend::write_pe64_executable_image(spec);
    if (!result.succeeded()) {
        for (const auto& item : result.diagnostics)
            std::cerr << item.code << ": " << item.message << '\n';
        return 2;
    }

    std::ofstream output(output_path, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "Cannot create PE32+ output: " << output_path << '\n';
        return 3;
    }
    output.write(reinterpret_cast<const char*>(result.image.bytes.data()),
                 static_cast<std::streamsize>(result.image.bytes.size()));
    if (!output) {
        std::cerr << "Failed writing PE32+ output: " << output_path << '\n';
        return 4;
    }
    std::cout << "PE32+ AMD64 executable written: " << output_path << '\n'
              << "Image size: " << result.image.bytes.size() << '\n'
              << "Entry RVA: 0x" << std::hex << result.image.entry_point_rva << std::dec << '\n'
              << "Imports: " << result.image.imported_symbol_count << '\n'
              << "DLLs: " << result.image.imported_library_count << " (name imports: " << result.image.imported_name_count
              << ", ordinal imports: " << result.image.imported_ordinal_count << ")\n"
              << "DIR64 relocations: " << result.image.base_relocation_count << '\n'
              << "Unwind functions: " << result.image.unwind_function_count << '\n';
    return 0;
}
