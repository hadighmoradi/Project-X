// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/module_resolver.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace xfrontend {
namespace {

std::string fold(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= 'A' && byte <= 'Z') {
            ch = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return value;
}

std::string canonical_type(const std::string& type) {
    const std::string value = fold(type);
    if (value == "integer") return "int";
    if (value == "boolean") return "bool";
    return value;
}

bool is_numeric_type(const std::string& type) {
    const std::string value = canonical_type(type);
    return value == "int" || value == "float" || value == "double";
}

bool external_type_compatible(const std::string& destination, const std::string& source) {
    if (destination.empty() || source.empty() || destination == "<unknown>" || source == "<unknown>") {
        return true;
    }
    const std::string dst = canonical_type(destination);
    const std::string src = canonical_type(source);
    if (dst == src) return true;
    if ((dst == "float" || dst == "double") && src == "int") return true;
    if (dst == "double" && src == "float") return true;
    return false;
}

bool is_generic_parameter(const FunctionDeclaration& function, const std::string& type_name) {
    const std::string key = fold(type_name);
    return std::any_of(function.generic_parameters.begin(), function.generic_parameters.end(),
        [&](const std::string& parameter) {
            const std::size_t colon = parameter.find(':');
            return fold(parameter.substr(0U, colon)) == key;
        });
}

std::string infer_static_expression_type(const Expression& expression) {
    if (expression.kind == ExprKind::Literal) {
        const std::string value = fold(expression.text);
        if (value == "true" || value == "false") return "bool";
        if (value == "null") return "null";
        if (!expression.text.empty() && expression.text.front() == '\"') return "string";
        if (expression.text.find_first_of(".eE") != std::string::npos) return "float";
        return "int";
    }
    if (expression.kind == ExprKind::Unary && !expression.children.empty() && expression.children[0]) {
        return infer_static_expression_type(*expression.children[0]);
    }
    if (expression.kind == ExprKind::Binary && expression.children.size() >= 2U &&
        expression.children[0] && expression.children[1]) {
        const std::string left = infer_static_expression_type(*expression.children[0]);
        const std::string right = infer_static_expression_type(*expression.children[1]);
        if (expression.text == "==" || expression.text == "!=" || expression.text == "<" ||
            expression.text == "<=" || expression.text == ">" || expression.text == ">=" ||
            expression.text == "&&" || expression.text == "||") return "bool";
        if (expression.text == "+" && left == "string" && right == "string") return "string";
        if (is_numeric_type(left) && is_numeric_type(right)) {
            if (left == "double" || right == "double") return "double";
            if (left == "float" || right == "float") return "float";
            return "int";
        }
    }
    if (expression.kind == ExprKind::List && !expression.children.empty() && expression.children[0]) {
        const std::string element = infer_static_expression_type(*expression.children[0]);
        return "List[" + element + "]";
    }
    return "<unknown>";
}

std::string function_signature(const FunctionDeclaration& function) {
    std::ostringstream stream;
    stream << fold(function.name) << '(';
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
        if (index != 0U) stream << ',';
        stream << canonical_type(function.parameters[index].type_name);
    }
    stream << ')';
    return stream.str();
}

SourceSpan fallback_span(const ProgramFile& file) {
    if (file.unit) for (const auto& function : file.unit->functions) {
        if (function.span.source_id != InvalidSourceFileId ||
            function.span.start.byte_offset != 0U || function.span.end.byte_offset != 0U) {
            return function.span;
        }
    }
    SourceSpan span;
    span.source_id = file.source_id;
    return span;
}

void add_diagnostic(ModuleResolutionResult& result, const char* code,
                    const std::string& message, const SourceSpan& span) {
    result.diagnostics.push_back(Diagnostic{code, message, span});
}

struct FunctionRecord {
    const FunctionDeclaration* declaration = nullptr; // definition preferred
    const FunctionDeclaration* prototype = nullptr;
    const FunctionDeclaration* definition = nullptr;
    const ProgramFile* file = nullptr;
    std::string signature;
};

struct ImportEdge {
    std::string target;
    SourceSpan span;
};

struct ModuleRecord {
    std::string name;
    std::vector<const ProgramFile*> files;
    std::vector<FunctionRecord> functions;
    std::vector<ImportEdge> edges;
};

std::string canonical_passing_mode(const std::string& mode) {
    return fold(mode.empty() ? std::string("copy") : mode);
}

bool same_generic_contract(const FunctionDeclaration& left,
                           const FunctionDeclaration& right) {
    if (left.generic_parameters.size() != right.generic_parameters.size()) return false;
    for (std::size_t index = 0; index < left.generic_parameters.size(); ++index) {
        const auto constraint = [](const std::string& item) {
            const std::size_t colon = item.find(':');
            return colon == std::string::npos ? std::string{} : fold(item.substr(colon + 1U));
        };
        if (constraint(left.generic_parameters[index]) != constraint(right.generic_parameters[index])) {
            return false;
        }
    }
    return true;
}

bool compatible_declaration_contract(const FunctionDeclaration& prototype,
                                     const FunctionDeclaration& definition,
                                     std::string& reason) {
    if (prototype.parameters.size() != definition.parameters.size()) {
        reason = "parameter count differs";
        return false;
    }
    for (std::size_t index = 0; index < prototype.parameters.size(); ++index) {
        const auto& declared = prototype.parameters[index];
        const auto& defined = definition.parameters[index];
        if (canonical_passing_mode(declared.passing_mode) !=
            canonical_passing_mode(defined.passing_mode)) {
            reason = "parameter passing mode differs at position " + std::to_string(index + 1U);
            return false;
        }
        if (declared.default_value && defined.default_value) {
            // The full expression is intentionally not compared here. The declaration
            // supplies the call-site default and the definition supplies the body.
        }
    }
    if (!same_generic_contract(prototype, definition)) {
        reason = "generic parameter constraints differ";
        return false;
    }
    if (!prototype.return_type.empty() && !definition.return_type.empty() &&
        canonical_type(prototype.return_type) != canonical_type(definition.return_type)) {
        reason = "declared return type differs";
        return false;
    }
    return true;
}

ModuleFunctionInterface make_public_interface(const FunctionRecord& record) {
    ModuleFunctionInterface api;
    const FunctionDeclaration* primary = record.definition != nullptr
        ? record.definition : record.declaration;
    if (primary == nullptr) return api;
    const FunctionDeclaration* defaults = record.prototype != nullptr
        ? record.prototype : primary;
    api.name = primary->name;
    api.signature = record.signature;
    api.return_type = !primary->return_type.empty()
        ? primary->return_type
        : (record.prototype != nullptr ? record.prototype->return_type : std::string{});
    api.generic_parameters = primary->generic_parameters;
    api.visibility = primary->visibility;
    api.has_definition = record.definition != nullptr;
    api.declaration_span = primary->span;
    api.required_parameter_count = primary->parameters.size();
    for (std::size_t index = 0; index < primary->parameters.size(); ++index) {
        const Parameter& parameter = primary->parameters[index];
        const bool has_default = index < defaults->parameters.size() &&
            defaults->parameters[index].default_value != nullptr;
        api.parameters.push_back(ModuleParameterInterface{
            parameter.name, parameter.type_name, parameter.passing_mode, has_default});
        if (has_default && api.required_parameter_count > 0U) {
            --api.required_parameter_count;
        }
    }
    return api;
}

bool flatten_member_path(const Expression& expression, std::vector<std::string>& parts) {
    if (expression.kind == ExprKind::Identifier) {
        parts.push_back(expression.text);
        return true;
    }
    if (expression.kind != ExprKind::Member || expression.children.empty() ||
        !expression.children[0]) {
        return false;
    }
    if (!flatten_member_path(*expression.children[0], parts)) return false;
    parts.push_back(expression.text);
    return true;
}

std::string join_parts(const std::vector<std::string>& parts, std::size_t count) {
    std::string value;
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0U) value += '.';
        value += parts[index];
    }
    return value;
}

bool is_import_root(const std::string& root, const std::vector<std::string>& imports) {
    const std::string key = fold(root);
    for (const auto& imported : imports) {
        const std::size_t dot = imported.find('.');
        if (fold(imported.substr(0U, dot)) == key) return true;
    }
    return false;
}

struct CallValidationContext {
    ModuleResolutionResult& result;
    const ProgramFile& file;
    const std::map<std::string, ModuleRecord>& modules;
    std::unordered_set<std::string> imported_modules;
    std::unordered_set<std::string> diagnosed_calls;
};

void validate_expression(const Expression& expression, CallValidationContext& context);

void validate_statement(const Statement& statement, CallValidationContext& context) {
    for (const auto& expression : statement.expressions) {
        if (expression) validate_expression(*expression, context);
    }
    for (const auto& child : statement.statements) {
        if (child) validate_statement(*child, context);
    }
    if (statement.else_branch) validate_statement(*statement.else_branch, context);
}

void validate_qualified_call(const Expression& expression, const Expression& callee,
                             CallValidationContext& context,
                             const std::vector<std::string>& parts) {
    if (parts.size() < 2U) return;

    const std::string module_name = join_parts(parts, parts.size() - 1U);
    const std::string module_key = fold(module_name);
    const std::string function_name = fold(parts.back());
    const std::string call_key = module_key + "." + function_name + "@" +
        std::to_string(expression.span.source_id) + ":" +
        std::to_string(expression.span.start.byte_offset);
    if (!context.diagnosed_calls.insert(call_key).second) return;

    const bool exact_import = context.imported_modules.find(module_key) != context.imported_modules.end();
    const bool known_module = context.modules.find(module_key) != context.modules.end();
    if (!exact_import) {
        if (known_module) {
            add_diagnostic(context.result, "XMOD0011",
                "Module '" + module_name + "' must be imported before it can be referenced",
                callee.span);
        } else if (is_import_root(parts.front(), context.file.unit->imports)) {
            add_diagnostic(context.result, "XMOD0005",
                "Qualified call must use the exact imported module path ('" + module_name + "')",
                callee.span);
        }
        return;
    }

    const auto module_found = context.modules.find(module_key);
    if (module_found == context.modules.end()) {
        // The unknown import has already been diagnosed at its declaration.
        return;
    }

    std::vector<const FunctionRecord*> public_candidates;
    std::vector<const FunctionRecord*> private_candidates;
    for (const auto& function : module_found->second.functions) {
        if (function.declaration == nullptr || fold(function.declaration->name) != function_name) continue;
        if (function.declaration->visibility == Visibility::Public) {
            public_candidates.push_back(&function);
        } else {
            private_candidates.push_back(&function);
        }
    }

    if (public_candidates.empty()) {
        if (!private_candidates.empty()) {
            add_diagnostic(context.result, "XMOD0006",
                "Function '" + parts.back() + "' is not public in module '" + module_name + "'",
                callee.span);
        } else {
            add_diagnostic(context.result, "XMOD0007",
                "Module '" + module_name + "' does not export function '" + parts.back() + "'",
                callee.span);
        }
        return;
    }

    const std::size_t argument_count = expression.children.empty()
        ? 0U : expression.children.size() - 1U;
    std::vector<std::string> argument_types;
    for (std::size_t index = 1; index < expression.children.size(); ++index) {
        argument_types.push_back(expression.children[index]
            ? infer_static_expression_type(*expression.children[index]) : "<unknown>");
    }

    const auto accepts_arity = [&](const FunctionRecord* candidate) {
        const ModuleFunctionInterface interface = make_public_interface(*candidate);
        return argument_count >= interface.required_parameter_count &&
            argument_count <= interface.parameters.size();
    };
    const auto matches_types = [&](const FunctionRecord* record) {
        const FunctionDeclaration* candidate = record->declaration;
        int score = 0;
        for (std::size_t index = 0; index < argument_types.size(); ++index) {
            const std::string& actual = argument_types[index];
            const std::string& expected = candidate->parameters[index].type_name;
            if (actual == "<unknown>" || actual.empty() || is_generic_parameter(*candidate, expected)) continue;
            if (canonical_type(actual) == canonical_type(expected)) {
                score += 2;
            } else if (external_type_compatible(expected, actual)) {
                score += 1;
            } else {
                return std::pair<bool, int>{false, score};
            }
        }
        return std::pair<bool, int>{true, score};
    };

    std::vector<const FunctionRecord*> public_arity_candidates;
    std::vector<const FunctionRecord*> public_viable_candidates;
    int best_score = -1;
    for (const auto* candidate : public_candidates) {
        if (!accepts_arity(candidate)) continue;
        public_arity_candidates.push_back(candidate);
        const auto match = matches_types(candidate);
        if (match.first && match.second > best_score) {
            best_score = match.second;
            public_viable_candidates.clear();
            public_viable_candidates.push_back(candidate);
        } else if (match.first && match.second == best_score) {
            public_viable_candidates.push_back(candidate);
        }
    }

    if (public_viable_candidates.empty()) {
        const bool private_match = std::any_of(private_candidates.begin(), private_candidates.end(),
            [&](const FunctionRecord* candidate) {
                return accepts_arity(candidate) && matches_types(candidate).first;
            });
        if (private_match) {
            add_diagnostic(context.result, "XMOD0006",
                "Matching overload of '" + parts.back() + "' is private in module '" + module_name + "'",
                callee.span);
        } else if (public_arity_candidates.empty()) {
            add_diagnostic(context.result, "XMOD0008",
                "No public overload of '" + parts.back() + "' accepts " +
                std::to_string(argument_count) + " argument(s)", callee.span);
        } else {
            add_diagnostic(context.result, "XMOD0012",
                "No public overload of '" + parts.back() + "' matches the statically known argument types",
                callee.span);
        }
    } else if (public_viable_candidates.size() > 1U) {
        const bool has_unknown_argument = std::any_of(argument_types.begin(), argument_types.end(),
            [](const std::string& type) { return type.empty() || type == "<unknown>"; });
        // If the source expression is not statically recognizable here (for example,
        // a local variable), defer overload selection to SemanticAnalyzer where the
        // caller's lexical symbol table provides the actual argument type.
        if (!has_unknown_argument) {
            add_diagnostic(context.result, "XMOD0009",
                "Qualified call to '" + parts.back() +
                "' is ambiguous; argument expressions did not uniquely select an exported overload",
                callee.span);
        }
    }
}

void validate_expression(const Expression& expression, CallValidationContext& context) {
    if (expression.kind == ExprKind::Call && !expression.children.empty() && expression.children[0]) {
        const Expression& callee = *expression.children[0];
        if (callee.kind == ExprKind::Member) {
            std::vector<std::string> parts;
            if (flatten_member_path(callee, parts)) {
                validate_qualified_call(expression, callee, context, parts);
            }
        }
    }
    for (const auto& child : expression.children) {
        if (child) validate_expression(*child, context);
    }
}

} // namespace

ModuleResolutionResult resolve_modules(const std::vector<ProgramFile>& files) {
    ModuleResolutionResult result;
    std::map<std::string, ModuleRecord> modules;

    for (const auto& file : files) {
        if (!file.unit) {
            add_diagnostic(result, "XMOD0010", "Program file has no parsed compilation unit",
                           fallback_span(file));
            continue;
        }
        if (file.unit->module_name.empty()) {
            add_diagnostic(result, "XMOD0001", "Every program source file must declare a module",
                           fallback_span(file));
            continue;
        }
        const std::string key = fold(file.unit->module_name);
        auto& module = modules[key];
        if (module.name.empty()) module.name = file.unit->module_name;
        module.files.push_back(&file);
    }

    // Merge declarations from all source files that belong to the same module.
    for (auto& module_pair : modules) {
        ModuleRecord& module = module_pair.second;
        std::unordered_map<std::string, std::size_t> signature_positions;
        for (const ProgramFile* file : module.files) {
            for (const auto& function : file->unit->functions) {
                const std::string signature = function_signature(function);
                const auto found = signature_positions.find(signature);
                if (found == signature_positions.end()) {
                    signature_positions.emplace(signature, module.functions.size());
                    FunctionRecord record;
                    record.declaration = &function;
                    record.prototype = function.has_body ? nullptr : &function;
                    record.definition = function.has_body ? &function : nullptr;
                    record.file = file;
                    record.signature = signature;
                    module.functions.push_back(std::move(record));
                    continue;
                }

                FunctionRecord& old = module.functions[found->second];
                const FunctionDeclaration* previous = old.declaration;
                const bool same_visibility = previous != nullptr &&
                    previous->visibility == function.visibility;
                const bool prototype_definition_pair = same_visibility &&
                    (old.definition == nullptr) != (!function.has_body);
                if (prototype_definition_pair) {
                    const FunctionDeclaration& prototype = function.has_body
                        ? *previous : function;
                    const FunctionDeclaration& definition = function.has_body
                        ? function : *previous;
                    std::string reason;
                    if (!compatible_declaration_contract(prototype, definition, reason)) {
                        add_diagnostic(result, "XMOD0013",
                            "Prototype/definition interface mismatch for '" + signature + "': " + reason,
                            function.span);
                    }
                    if (function.has_body) {
                        old.definition = &function;
                        old.declaration = &function;
                        old.file = file;
                    } else {
                        old.prototype = &function;
                    }
                    continue;
                }
                add_diagnostic(result, "XMOD0002",
                    "Duplicate module function signature '" + signature + "' in module '" + module.name + "'",
                    function.span);
            }
        }
    }

    // Validate imports and build a case-insensitive dependency graph.
    for (auto& module_pair : modules) {
        ModuleRecord& module = module_pair.second;
        std::unordered_set<std::string> seen_imports;
        for (const ProgramFile* file : module.files) {
            for (std::size_t index = 0; index < file->unit->imports.size(); ++index) {
                const std::string& imported = file->unit->imports[index];
                const std::string target = fold(imported);
                if (!seen_imports.insert(target).second) continue;
                SourceSpan span = fallback_span(*file);
                if (index < file->unit->import_spans.size()) span = file->unit->import_spans[index];
                if (modules.find(target) == modules.end()) {
                    add_diagnostic(result, "XMOD0003",
                        "Imported module '" + imported + "' is not part of this program", span);
                } else {
                    module.edges.push_back(ImportEdge{target, span});
                }
            }
        }
    }

    // Reject cycles. This graph is required to be a DAG by the module design.
    std::map<std::string, int> state;
    std::set<std::pair<std::string, std::string>> reported_cycles;
    std::vector<std::string> stack;
    std::function<void(const std::string&)> visit = [&](const std::string& key) {
        state[key] = 1;
        stack.push_back(key);
        const auto module_it = modules.find(key);
        if (module_it != modules.end()) {
            for (const auto& edge : module_it->second.edges) {
                const int edge_state = state[edge.target];
                if (edge_state == 0) {
                    visit(edge.target);
                } else if (edge_state == 1 && reported_cycles.insert({key, edge.target}).second) {
                    std::ostringstream chain;
                    const auto start = std::find(stack.begin(), stack.end(), edge.target);
                    for (auto it = start; it != stack.end(); ++it) {
                        if (it != start) chain << " -> ";
                        chain << modules[*it].name;
                    }
                    chain << " -> " << modules[edge.target].name;
                    add_diagnostic(result, "XMOD0004", "Module dependency cycle: " + chain.str(), edge.span);
                }
            }
        }
        stack.pop_back();
        state[key] = 2;
    };
    for (const auto& module_pair : modules) {
        if (state[module_pair.first] == 0) visit(module_pair.first);
    }

    // Validate fully-qualified calls through imported module namespaces.
    for (const auto& module_pair : modules) {
        for (const ProgramFile* file : module_pair.second.files) {
            CallValidationContext context{result, *file, modules, {}, {}};
            for (const auto& imported : file->unit->imports) {
                context.imported_modules.insert(fold(imported));
            }
            for (const auto& function : file->unit->functions) {
                if (function.body) validate_statement(*function.body, context);
            }
        }
    }

    for (const auto& pair : modules) {
        const ModuleRecord& module = pair.second;
        ResolvedModuleInfo info;
        info.name = module.name;
        std::set<SourceFileId> source_ids;
        std::set<std::string> imports;
        for (const ProgramFile* file : module.files) {
            if (file->source_id != InvalidSourceFileId) source_ids.insert(file->source_id);
            for (const auto& imported : file->unit->imports) imports.insert(imported);
        }
        info.source_ids.assign(source_ids.begin(), source_ids.end());
        info.imports.assign(imports.begin(), imports.end());
        for (const auto& function : module.functions) {
            if (function.declaration != nullptr &&
                function.declaration->visibility == Visibility::Public) {
                info.public_function_signatures.push_back(function.signature);
                info.public_functions.push_back(make_public_interface(function));
            }
        }
        result.modules.push_back(std::move(info));
    }

    return result;
}

} // namespace xfrontend
