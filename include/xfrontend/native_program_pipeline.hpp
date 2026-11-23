#pragma once

#include "xfrontend/x64_codegen.hpp"

#include <memory>
#include <string>
#include <vector>

namespace xfrontend {

struct NativeProgramModuleInput {
    std::string display_name;
    std::shared_ptr<const CompilationUnit> unit;
};

struct NativeProgramModuleOutput {
    std::string module_name;
    X64CodegenResult codegen;
};

struct NativeProgramBuildResult {
    std::vector<NativeProgramModuleOutput> modules;
    ObjectLinkPlanResult link_plan;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty() && !modules.empty() && link_plan.succeeded(); }
};

// Performs whole-program semantic/module analysis, compiles each source module
// to a COFF object, resolves qualified imported calls against public module
// interfaces, then produces the existing deterministic object link plan.
// Final PE serialization/execution is delegated to the platform linker.
NativeProgramBuildResult compile_x64_coff_program(
    const std::vector<NativeProgramModuleInput>& modules,
    X64CodegenOptions options = {});

// Helper for N52 global data emission. An optional pointer relocation can be
// appended to the returned artifact by using its stable mangled symbol name.
NativeDataCodeArtifact make_native_global_data_artifact(
    const std::string& module_name, const std::string& name,
    const std::string& type_name, const std::vector<std::uint8_t>& initial_bytes,
    std::size_t alignment = 8U, bool read_only = false,
    bool linker_root = false);

} // namespace xfrontend
