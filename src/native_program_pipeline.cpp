// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/native_program_pipeline.hpp"

#include "xfrontend/module_resolver.hpp"
#include "xfrontend/semantic.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace xfrontend {
namespace {
void add_error(std::vector<Diagnostic>& diagnostics, const char* code, const std::string& message) {
    diagnostics.push_back(Diagnostic{code, message, {}});
}
std::string fold_ascii(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'))
            ch = static_cast<char>(byte - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
    }
    return value;
}
NativeSymbolDeclaration make_export_declaration(const CompilationUnit& unit,
                                                  std::size_t index) {
    const auto& function = unit.functions[index];
    NativeSymbolDeclaration declaration;
    declaration.symbol_id = fold_ascii(unit.module_name) + "#function#" + std::to_string(index);
    declaration.module_name = unit.module_name;
    declaration.source_name = function.name;
    declaration.return_type = function.return_type.empty() ? "void" : function.return_type;
    declaration.visibility = DropSymbolVisibility::Public;
    declaration.linkage = DropSymbolLinkage::External;
    declaration.entity_kind = NativeSymbolEntityKind::Function;
    declaration.defined = function.has_body;
    for (const auto& parameter : function.parameters)
        declaration.parameters.push_back(NativeSymbolParameter{parameter.type_name, DropParameterPassing::Value});
    return declaration;
}
}

NativeProgramBuildResult compile_x64_coff_program(
    const std::vector<NativeProgramModuleInput>& modules,
    X64CodegenOptions options) {
    NativeProgramBuildResult result;
    if (modules.empty()) {
        add_error(result.diagnostics, "XN53P0001", "native program requires at least one source module");
        return result;
    }
    std::set<std::string> module_names;
    std::vector<ProgramFile> semantic_files;
    semantic_files.reserve(modules.size());
    for (std::size_t index = 0U; index < modules.size(); ++index) {
        const auto& input = modules[index];
        if (!input.unit || input.unit->module_name.empty()) {
            add_error(result.diagnostics, "XN53P0002", "every native program input must contain a named module");
            continue;
        }
        const std::string key = fold_ascii(input.unit->module_name);
        if (!module_names.insert(key).second) {
            add_error(result.diagnostics, "XN53P0003", "this pipeline accepts one merged CompilationUnit per module");
            continue;
        }
        ProgramFile file;
        file.display_name = input.display_name.empty() ? input.unit->module_name : input.display_name;
        file.source_id = static_cast<SourceFileId>(index + 1U);
        file.unit = input.unit;
        semantic_files.push_back(std::move(file));
    }
    if (!result.diagnostics.empty()) return result;

    const SemanticResult semantic = analyze_program(semantic_files);
    if (!semantic.succeeded()) {
        result.diagnostics = semantic.diagnostics;
        return result;
    }

    std::vector<X64ExternalFunctionSignature> exports;
    for (const auto& input : modules) {
        std::vector<NativeSymbolDeclaration> declarations;
        std::vector<std::size_t> indices;
        for (std::size_t index = 0U; index < input.unit->functions.size(); ++index) {
            const auto& function = input.unit->functions[index];
            if (function.visibility != Visibility::Public || !function.has_body || !function.generic_parameters.empty()) continue;
            declarations.push_back(make_export_declaration(*input.unit, index));
            indices.push_back(index);
        }
        const auto symbols = build_native_symbol_table(declarations);
        if (!symbols.succeeded()) {
            result.diagnostics.insert(result.diagnostics.end(), symbols.diagnostics.begin(), symbols.diagnostics.end());
            continue;
        }
        for (std::size_t symbol_index = 0U; symbol_index < indices.size(); ++symbol_index) {
            const auto& function = input.unit->functions[indices[symbol_index]];
            const auto& symbol = symbols.symbols[symbol_index];
            X64ExternalFunctionSignature signature;
            signature.module_name = input.unit->module_name;
            signature.name = function.name;
            signature.return_type = function.return_type.empty() ? "void" : function.return_type;
            signature.mangled_name = symbol.mangled_name;
            signature.signature_key = symbol.signature_key;
            for (const auto& parameter : function.parameters) signature.parameter_types.push_back(parameter.type_name);
            exports.push_back(std::move(signature));
        }
    }
    if (!result.diagnostics.empty()) return result;

    std::vector<ObjectFile> objects;
    result.modules.reserve(modules.size());
    objects.reserve(modules.size());
    for (const auto& input : modules) {
        X64CodegenOptions module_options = options;
        module_options.object_id = input.unit->module_name + "-native-object";
        module_options.skip_semantic_validation = true;
        module_options.imported_functions.clear();
        for (const auto& import : input.unit->imports) {
            for (const auto& exported : exports) {
                if (fold_ascii(import) == fold_ascii(exported.module_name))
                    module_options.imported_functions.push_back(exported);
            }
        }
        X64CodegenResult compiled = compile_x64_coff_object(*input.unit, std::move(module_options));
        if (!compiled.succeeded()) {
            result.diagnostics.insert(result.diagnostics.end(), compiled.diagnostics.begin(), compiled.diagnostics.end());
            continue;
        }
        objects.push_back(compiled.native_object.object_file);
        NativeProgramModuleOutput output;
        output.module_name = input.unit->module_name;
        output.codegen = std::move(compiled);
        result.modules.push_back(std::move(output));
    }
    if (!result.diagnostics.empty()) return result;
    result.link_plan = build_object_link_plan(options.target, objects);
    if (!result.link_plan.succeeded()) result.diagnostics = result.link_plan.diagnostics;
    return result;
}

NativeDataCodeArtifact make_native_global_data_artifact(
    const std::string& module_name, const std::string& name,
    const std::string& type_name, const std::vector<std::uint8_t>& initial_bytes,
    std::size_t alignment, bool read_only, bool linker_root) {
    NativeDataCodeArtifact artifact;
    artifact.declaration.symbol_id = fold_ascii(module_name) + "#global#" + fold_ascii(name);
    artifact.declaration.module_name = module_name;
    artifact.declaration.source_name = name;
    artifact.declaration.return_type = type_name;
    artifact.declaration.visibility = DropSymbolVisibility::Public;
    artifact.declaration.linkage = DropSymbolLinkage::External;
    artifact.declaration.entity_kind = NativeSymbolEntityKind::Global;
    artifact.declaration.defined = true;
    artifact.contents = initial_bytes;
    artifact.logical_size = initial_bytes.size();
    artifact.alignment = alignment;
    artifact.read_only = read_only;
    artifact.linker_root = linker_root;
    return artifact;
}

} // namespace xfrontend
