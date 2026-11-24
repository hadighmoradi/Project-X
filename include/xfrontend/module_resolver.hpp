// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/parser.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace xfrontend {

// One already-parsed source file participating in a program-level module graph.
// Multiple files may declare the same module and become one compilation module.
struct ProgramFile {
    std::string display_name;
    SourceFileId source_id = InvalidSourceFileId;
    std::shared_ptr<const CompilationUnit> unit;
};

struct ModuleParameterInterface {
    std::string name;
    std::string type_name;
    std::string passing_mode;
    bool has_default = false;
};

// A stable, serializable view of one exported function. It contains no AST
// ownership and can be passed from module resolution into semantic analysis.
struct ModuleFunctionInterface {
    std::string name;
    std::string signature;
    std::string return_type;
    std::vector<std::string> generic_parameters;
    std::vector<ModuleParameterInterface> parameters;
    std::size_t required_parameter_count = 0;
    Visibility visibility = Visibility::Private;
    bool has_definition = false;
    SourceSpan declaration_span;
};

using ImportedModuleFunctionMap =
    std::unordered_map<std::string, std::vector<ModuleFunctionInterface>>;

struct ResolvedModuleInfo {
    std::string name;
    std::vector<SourceFileId> source_ids;
    std::vector<std::string> imports;
    std::vector<std::string> public_function_signatures;
    std::vector<ModuleFunctionInterface> public_functions;
};

struct ModuleResolutionResult {
    std::vector<ResolvedModuleInfo> modules;
    std::vector<Diagnostic> diagnostics;

    bool succeeded() const noexcept { return diagnostics.empty(); }
};

// Resolves the module graph for a set of parsed .src files and builds typed
// public function interfaces for downstream semantic analysis. Module and
// member identifiers are case-insensitive. Import aliases/selective imports
// are not supported; a qualified call must use the exact imported module path.
// The resolver validates declaration/definition contracts and preserves
// prototype defaults. Inferred return types are enriched by analyze_program.
ModuleResolutionResult resolve_modules(const std::vector<ProgramFile>& files);

} // namespace xfrontend
