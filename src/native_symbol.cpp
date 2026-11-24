// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/native_symbol.hpp"

#include <algorithm>
#include <cctype>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <utility>

namespace xfrontend {
namespace {

std::string trim_copy(const std::string& value) {
    const auto first = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto last = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

std::string fold_ascii(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'))
            ch = static_cast<char>(byte - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
    }
    return value;
}

std::string canonical_type(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }), value.end());
    return fold_ascii(std::move(value));
}

std::string encode_component(const std::string& input) {
    std::ostringstream out;
    out << input.size() << '_';
    out << std::hex << std::setfill('0');
    for (unsigned char byte : input) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}

bool valid_external_name(const std::string& value) {
    if (value.empty()) return false;
    const auto first = static_cast<unsigned char>(value.front());
    if (!((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z') || first == '_'))
        return false;
    for (unsigned char byte : value) {
        if (!((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
              (byte >= '0' && byte <= '9') || byte == '_')) return false;
    }
    return true;
}

const char* passing_code(DropParameterPassing passing) {
    switch (passing) {
    case DropParameterPassing::Value: return "v";
    case DropParameterPassing::Borrow: return "b";
    case DropParameterPassing::BorrowMut: return "m";
    case DropParameterPassing::Take: return "t";
    }
    return "?";
}

const char* convention_code(DropCallingConvention convention) {
    switch (convention) {
    case DropCallingConvention::XNative: return "x";
    case DropCallingConvention::C: return "c";
    case DropCallingConvention::System: return "s";
    }
    return "?";
}

const char* entity_code(NativeSymbolEntityKind kind) {
    switch (kind) {
    case NativeSymbolEntityKind::Function: return "f";
    case NativeSymbolEntityKind::Global: return "g";
    case NativeSymbolEntityKind::TypeDescriptor: return "t";
    case NativeSymbolEntityKind::DropThunk: return "d";
    }
    return "?";
}

std::string make_overload_key(const NativeSymbolDeclaration& declaration) {
    std::ostringstream out;
    out << canonical_type(declaration.module_name) << '|'
        << canonical_type(declaration.source_name) << '|'
        << entity_code(declaration.entity_kind) << '|'
        << convention_code(declaration.calling_convention) << '|'
        << declaration.parameters.size();
    for (const auto& parameter : declaration.parameters)
        out << '|' << passing_code(parameter.passing) << ':' << canonical_type(parameter.type_name);
    return out.str();
}

std::string make_signature_key(const NativeSymbolDeclaration& declaration) {
    std::ostringstream out;
    out << make_overload_key(declaration) << "->" << canonical_type(declaration.return_type)
        << (declaration.variadic ? ":var" : ":fixed");
    return out.str();
}

void add_diag(std::vector<Diagnostic>& diagnostics, const char* code, const std::string& message) {
    diagnostics.push_back(Diagnostic{code, message, {}});
}

bool is_public_external(const NativeSymbolDeclaration& declaration) {
    return declaration.visibility == DropSymbolVisibility::Public &&
           declaration.linkage == DropSymbolLinkage::External;
}

} // namespace

std::string canonicalize_native_symbol_name(const std::string& value) {
    return fold_ascii(trim_copy(value));
}

std::string mangle_native_symbol(const NativeSymbolDeclaration& declaration, std::string* reason) {
    const std::string module = canonicalize_native_symbol_name(declaration.module_name);
    const std::string name = canonicalize_native_symbol_name(declaration.source_name);
    if (module.empty() || name.empty()) {
        if (reason != nullptr) *reason = "module and source names must be non-empty";
        return {};
    }
    if (declaration.generic) {
        if (reason != nullptr) *reason = "unspecialized generic declarations cannot receive native symbols";
        return {};
    }
    if (declaration.visibility == DropSymbolVisibility::Public &&
        declaration.linkage != DropSymbolLinkage::External) {
        if (reason != nullptr) *reason = "public declarations require external linkage";
        return {};
    }
    if (declaration.visibility != DropSymbolVisibility::Public &&
        declaration.linkage == DropSymbolLinkage::External) {
        if (reason != nullptr) *reason = "non-public declarations cannot use external linkage";
        return {};
    }
    if (declaration.calling_convention == DropCallingConvention::XNative && declaration.variadic) {
        if (reason != nullptr) *reason = "XNative variadic declarations are not supported";
        return {};
    }

    if (declaration.calling_convention == DropCallingConvention::C ||
        declaration.calling_convention == DropCallingConvention::System) {
        if (!is_public_external(declaration)) {
            if (reason != nullptr) *reason = "C/System declarations require public external linkage";
            return {};
        }
        const std::string external = trim_copy(declaration.external_name).empty()
            ? trim_copy(declaration.source_name) : trim_copy(declaration.external_name);
        if (!valid_external_name(external)) {
            if (reason != nullptr) *reason = "external linker name is not a valid C identifier";
            return {};
        }
        return external;
    }

    std::ostringstream out;
    out << "__Xv1_M" << encode_component(module)
        << "_N" << encode_component(name)
        << "_K" << entity_code(declaration.entity_kind)
        << "_C" << convention_code(declaration.calling_convention)
        << "_P" << declaration.parameters.size();
    for (const auto& parameter : declaration.parameters) {
        const std::string type = canonical_type(parameter.type_name);
        if (type.empty()) {
            if (reason != nullptr) *reason = "parameter type names must be non-empty";
            return {};
        }
        out << '_' << passing_code(parameter.passing) << encode_component(type);
    }
    return out.str();
}

NativeSymbolTableResult build_native_symbol_table(
    const std::vector<NativeSymbolDeclaration>& declarations) {
    NativeSymbolTableResult result;
    std::set<std::string> symbol_ids;
    std::map<std::string, std::size_t> emitted_names;
    std::map<std::string, std::string> signatures_by_overload;

    for (const auto& declaration : declarations) {
        if (trim_copy(declaration.symbol_id).empty()) {
            add_diag(result.diagnostics, "XSYM0001", "symbol id must be non-empty");
            continue;
        }
        if (!symbol_ids.insert(declaration.symbol_id).second) {
            add_diag(result.diagnostics, "XSYM0005", "duplicate source symbol id '" + declaration.symbol_id + "'");
            continue;
        }
        std::string reason;
        const std::string mangled = mangle_native_symbol(declaration, &reason);
        if (mangled.empty()) {
            add_diag(result.diagnostics, "XSYM0002", "cannot mangle '" + declaration.source_name + "': " + reason);
            continue;
        }
        const std::string overload = make_overload_key(declaration);
        const std::string signature = make_signature_key(declaration);
        const auto existing_signature = signatures_by_overload.find(overload);
        if (existing_signature != signatures_by_overload.end()) {
            add_diag(result.diagnostics, "XSYM0006",
                     existing_signature->second == signature
                         ? "duplicate native overload identity for '" + declaration.source_name + "'"
                         : "return-type-only or ABI-only overload conflict for '" + declaration.source_name + "'");
            continue;
        }
        signatures_by_overload.emplace(overload, signature);

        const auto existing_name = emitted_names.find(mangled);
        if (existing_name != emitted_names.end()) {
            add_diag(result.diagnostics, "XSYM0006", "native linker name collision for '" + mangled + "'");
            continue;
        }
        emitted_names.emplace(mangled, result.symbols.size());

        NativeSymbolRecord record;
        record.symbol_id = declaration.symbol_id;
        record.module_name = canonicalize_native_symbol_name(declaration.module_name);
        record.canonical_source_name = canonicalize_native_symbol_name(declaration.source_name);
        record.mangled_name = mangled;
        record.overload_key = overload;
        record.signature_key = signature;
        record.calling_convention = declaration.calling_convention;
        record.visibility = declaration.visibility;
        record.linkage = declaration.linkage;
        record.entity_kind = declaration.entity_kind;
        record.defined = declaration.defined;
        result.symbols.push_back(std::move(record));
    }
    return result;
}

NativeLinkResolutionResult resolve_native_symbol_references(
    const std::vector<NativeSymbolRecord>& definitions,
    const std::vector<NativeSymbolReference>& references) {
    NativeLinkResolutionResult result;
    std::map<std::string, std::vector<const NativeSymbolRecord*>> definitions_by_name;
    for (const auto& definition : definitions) {
        if (definition.defined) definitions_by_name[definition.mangled_name].push_back(&definition);
    }

    for (const auto& reference : references) {
        if (trim_copy(reference.reference_id).empty() || trim_copy(reference.mangled_name).empty()) {
            add_diag(result.diagnostics, "XLINK0005", "link reference id and mangled name must be non-empty");
            continue;
        }
        const auto found = definitions_by_name.find(reference.mangled_name);
        if (found == definitions_by_name.end()) {
            if (!reference.weak)
                add_diag(result.diagnostics, "XLINK0001", "unresolved strong native symbol '" + reference.mangled_name + "'");
            result.resolutions.push_back(NativeSymbolResolution{reference.reference_id, {}, reference.mangled_name, false});
            continue;
        }
        std::vector<const NativeSymbolRecord*> visible;
        for (const NativeSymbolRecord* candidate : found->second) {
            if (candidate->linkage == DropSymbolLinkage::External ||
                canonicalize_native_symbol_name(candidate->module_name) ==
                    canonicalize_native_symbol_name(reference.referencing_module)) {
                visible.push_back(candidate);
            }
        }
        if (visible.empty()) {
            if (!reference.weak)
                add_diag(result.diagnostics, "XLINK0003", "internal native symbol is not visible from module '" +
                         reference.referencing_module + "'");
            result.resolutions.push_back(NativeSymbolResolution{reference.reference_id, {}, reference.mangled_name, false});
            continue;
        }
        if (visible.size() != 1U) {
            add_diag(result.diagnostics, "XLINK0004", "multiple native definitions provide '" + reference.mangled_name + "'");
            continue;
        }
        const NativeSymbolRecord& definition = *visible.front();
        if (!reference.expected_signature_key.empty() &&
            reference.expected_signature_key != definition.signature_key) {
            add_diag(result.diagnostics, "XLINK0002", "native symbol signature mismatch for '" + reference.mangled_name + "'");
            continue;
        }
        result.resolutions.push_back(NativeSymbolResolution{
            reference.reference_id, definition.symbol_id, reference.mangled_name, true});
    }
    return result;
}

NativeLinkPlanResult bind_target_abi_calls_to_native_symbols(
    const TargetAbiLoweringResult& target_plan,
    const NativeSymbolTableResult& symbol_table) {
    NativeLinkPlanResult result;
    if (!target_plan.diagnostics.empty()) {
        result.diagnostics = target_plan.diagnostics;
        return result;
    }
    if (!symbol_table.diagnostics.empty()) {
        result.diagnostics = symbol_table.diagnostics;
        return result;
    }
    std::map<std::string, const NativeSymbolRecord*> by_id;
    for (const auto& symbol : symbol_table.symbols) by_id.emplace(symbol.symbol_id, &symbol);

    for (const auto& function : target_plan.functions) {
        for (const auto& block : function.blocks) {
            if (!block.reachable) continue;
            for (const auto& call : block.calls) {
                if (call.is_noop) continue;
                const auto found = by_id.find(call.symbol_id);
                if (found == by_id.end()) {
                    add_diag(result.diagnostics, "XLINK0006", "target ABI call refers to unknown symbol id '" + call.symbol_id + "'");
                    continue;
                }
                const NativeSymbolRecord& symbol = *found->second;
                if (!call.linkage_name.empty() && call.linkage_name != symbol.mangled_name) {
                    add_diag(result.diagnostics, "XLINK0007", "target ABI linkage name disagrees with native symbol table");
                    continue;
                }
                NativeLinkedCall linked;
                linked.function_name = function.function_name;
                linked.block_id = block.id;
                linked.action_index = call.action_index;
                linked.variable_id = call.variable_id;
                linked.symbol_id = symbol.symbol_id;
                linked.mangled_name = symbol.mangled_name;
                linked.target_triple = call.target_triple;
                linked.arguments = call.arguments;
                result.calls.push_back(std::move(linked));
            }
        }
    }
    return result;
}

} // namespace xfrontend
