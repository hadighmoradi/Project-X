// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/semantic.hpp"
#include "xfrontend/control_flow.hpp"
#include "xfrontend/lifetime.hpp"
#include "xfrontend/source_manager.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>
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

bool is_numeric(const std::string& type) {
    const std::string value = fold(type);
    return value == "int" || value == "integer" || value == "float" || value == "double";
}

bool is_integral(const std::string& type) {
    const std::string value = fold(type);
    return value == "int" || value == "integer";
}

bool is_boolean(const std::string& type) {
    return fold(type) == "bool" || fold(type) == "boolean";
}

std::string canonical_type(const std::string& type) {
    const std::string value = fold(type);
    if (value == "integer") return "int";
    if (value == "boolean") return "bool";
    return value;
}

bool is_unknown(const std::string& type) {
    return type.empty() || type == "<unknown>" || type == "<error>";
}

std::string signature_of_function(const FunctionDeclaration& function) {
    std::ostringstream signature;
    signature << fold(function.name) << '(';
    for (std::size_t index = 0; index < function.parameters.size(); ++index) {
        if (index != 0U) signature << ',';
        signature << canonical_type(function.parameters[index].type_name);
    }
    signature << ')';
    return signature.str();
}

bool flatten_member_path(const Expression& expression, std::vector<std::string>& parts) {
    if (expression.kind == ExprKind::Identifier) {
        parts.push_back(expression.text);
        return true;
    }
    if (expression.kind != ExprKind::Member || expression.children.empty() ||
        !expression.children[0]) return false;
    if (!flatten_member_path(*expression.children[0], parts)) return false;
    parts.push_back(expression.text);
    return true;
}

std::string join_member_path(const std::vector<std::string>& parts, std::size_t count) {
    std::string result;
    for (std::size_t index = 0; index < count; ++index) {
        if (index != 0U) result += '.';
        result += parts[index];
    }
    return result;
}

std::string substitute_generic_type(const std::string& type,
                                    const std::unordered_map<std::string, std::string>& substitutions) {
    std::string result;
    for (std::size_t index = 0; index < type.size();) {
        const unsigned char byte = static_cast<unsigned char>(type[index]);
        if (std::isalnum(byte) || type[index] == '_') {
            const std::size_t start = index;
            while (index < type.size()) {
                const unsigned char next = static_cast<unsigned char>(type[index]);
                if (!std::isalnum(next) && type[index] != '_') break;
                ++index;
            }
            const std::string token = type.substr(start, index - start);
            const auto found = substitutions.find(fold(token));
            result += found == substitutions.end() ? token : found->second;
        } else {
            result.push_back(type[index]);
            ++index;
        }
    }
    return result;
}

bool compatible(const std::string& destination, const std::string& source) {
    if (is_unknown(destination) || is_unknown(source)) return true;
    const std::string dst = canonical_type(destination);
    const std::string src = canonical_type(source);
    if (dst == src) return true;
    if ((dst == "float" || dst == "double") && is_integral(src)) return true;
    if (dst == "double" && src == "float") return true;
    return false;
}

std::string numeric_result(const std::string& left, const std::string& right) {
    const std::string lhs = canonical_type(left);
    const std::string rhs = canonical_type(right);
    if (lhs == "double" || rhs == "double") return "double";
    if (lhs == "float" || rhs == "float") return "float";
    return "int";
}

bool is_comparison(const std::string& op) {
    return op == "==" || op == "!=" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

struct VariableSymbol {
    std::string original_name;
    std::string type;
    SourceSpan declaration_span;
    bool is_parameter = false;
};

struct Scope {
    explicit Scope(Scope* outer = nullptr) : parent(outer) {}
    Scope* parent = nullptr;
    std::unordered_map<std::string, VariableSymbol> variables;
};

struct FunctionSymbol {
    const FunctionDeclaration* declaration = nullptr;
    SemanticFunctionInfo info;
    std::vector<std::string> parameter_types;
    std::vector<bool> generic_parameter_positions;
};

class AnalyzerImpl {
public:
    explicit AnalyzerImpl(const CompilationUnit& unit,
                          const std::vector<const FunctionDeclaration*>* module_functions = nullptr,
                          const ImportedModuleFunctionMap* imported_functions = nullptr)
        : unit_(unit), module_functions_(module_functions),
          imported_functions_(imported_functions) {
        collect_import_roots();
        collect_functions();
    }

    SemanticResult run() {
        for (const auto& function : unit_.functions) {
            analyze_function(function);
        }
        return std::move(result_);
    }

private:
    const CompilationUnit& unit_;
    const std::vector<const FunctionDeclaration*>* module_functions_ = nullptr;
    const ImportedModuleFunctionMap* imported_functions_ = nullptr;
    SemanticResult result_;
    std::unordered_map<std::string, std::vector<FunctionSymbol>> functions_;
    std::unordered_set<std::string> import_roots_;
    std::unordered_map<std::string, std::string> inferred_returns_;
    std::unordered_set<std::string> reported_unresolved_;

    void error(const char* code, const std::string& message, const SourceSpan& span) {
        result_.diagnostics.push_back(Diagnostic{code, message, span});
    }

    void collect_import_roots() {
        for (const auto& imported : unit_.imports) {
            const std::size_t dot = imported.find('.');
            import_roots_.insert(fold(imported.substr(0, dot)));
        }
        if (!unit_.module_name.empty()) {
            const std::size_t dot = unit_.module_name.find('.');
            import_roots_.insert(fold(unit_.module_name.substr(0, dot)));
        }
    }

    static bool is_generic_type(const std::string& type,
                                const std::vector<std::string>& generic_names) {
        const std::string normalized = fold(type);
        return std::any_of(generic_names.begin(), generic_names.end(), [&](const std::string& item) {
            const std::size_t constraint = item.find(':');
            const std::string parameter = item.substr(0U, constraint);
            return normalized == fold(parameter);
        });
    }

    void collect_functions() {
        std::vector<const FunctionDeclaration*> local_functions;
        if (module_functions_ != nullptr) {
            local_functions = *module_functions_;
        } else {
            local_functions.reserve(unit_.functions.size());
            for (const auto& function : unit_.functions) local_functions.push_back(&function);
        }

        for (const FunctionDeclaration* declaration_ptr : local_functions) {
            if (declaration_ptr == nullptr) continue;
            const FunctionDeclaration& declaration = *declaration_ptr;
            FunctionSymbol symbol;
            symbol.declaration = &declaration;
            symbol.info.name = declaration.name;
            symbol.info.visibility = declaration.visibility;
            symbol.info.total_parameter_count = declaration.parameters.size();
            symbol.info.required_parameter_count = declaration.parameters.size();
            symbol.info.inferred_return_type = declaration.return_type;
            for (const auto& parameter : declaration.parameters) {
                symbol.parameter_types.push_back(parameter.type_name);
                symbol.generic_parameter_positions.push_back(
                    is_generic_type(parameter.type_name, declaration.generic_parameters));
                if (parameter.default_value && symbol.info.required_parameter_count > 0) {
                    --symbol.info.required_parameter_count;
                }
            }
            std::ostringstream signature;
            signature << fold(declaration.name) << '(';
            for (std::size_t index = 0; index < declaration.parameters.size(); ++index) {
                if (index != 0U) signature << ',';
                signature << canonical_type(declaration.parameters[index].type_name);
            }
            signature << ')';
            symbol.info.signature = signature.str();

            auto& overloads = functions_[fold(declaration.name)];
            const auto duplicate = std::find_if(overloads.begin(), overloads.end(), [&](const FunctionSymbol& old) {
                return old.info.signature == symbol.info.signature;
            });
            if (duplicate == overloads.end()) {
                overloads.push_back(symbol);
                const bool belongs_to_current_file = std::any_of(
                    unit_.functions.begin(), unit_.functions.end(),
                    [&](const FunctionDeclaration& item) { return &item == declaration_ptr; });
                if (belongs_to_current_file) result_.functions.push_back(symbol.info);
            } else if (module_functions_ == nullptr) {
                error("XSEM0001", "Duplicate function signature '" + symbol.info.signature + "'",
                      declaration.span);
            }
        }
    }

    VariableSymbol* lookup(Scope* scope, const std::string& name) const {
        const std::string key = fold(name);
        for (Scope* current = scope; current != nullptr; current = current->parent) {
            const auto found = current->variables.find(key);
            if (found != current->variables.end()) {
                return const_cast<VariableSymbol*>(&found->second);
            }
        }
        return nullptr;
    }

    bool is_import_root(const std::string& name) const {
        return import_roots_.find(fold(name)) != import_roots_.end();
    }

    void declare_variable(Scope& scope, const std::string& name, const std::string& type,
                          const SourceSpan& span, bool parameter, bool explicit_declaration,
                          bool check_shadowing) {
        if (name.empty()) return;
        const std::string key = fold(name);
        const auto existing = scope.variables.find(key);
        if (existing != scope.variables.end()) {
            error("XSEM0002", "Duplicate declaration of '" + name + "' in the same scope", span);
            return;
        }
        if (check_shadowing && scope.parent != nullptr && lookup(scope.parent, name) != nullptr) {
            error("XSEM0003", "Shadowing an outer declaration with '" + name + "' is deferred", span);
            return;
        }
        (void)explicit_declaration;
        scope.variables.emplace(key, VariableSymbol{name, type.empty() ? "<unknown>" : type,
                                                       span, parameter});
    }

    void analyze_function(const FunctionDeclaration& function) {
        if (!function.has_body || !function.body) return;
        Scope root;
        std::unordered_set<std::string> parameter_names;
        for (const auto& parameter : function.parameters) {
            const std::string key = fold(parameter.name);
            if (!parameter_names.insert(key).second) {
                error("XSEM0004", "Duplicate parameter name '" + parameter.name + "'", parameter.span);
                continue;
            }
            declare_variable(root, parameter.name, parameter.type_name, parameter.span,
                             true, true, false);
        }
        std::vector<std::string> return_types;
        analyze_statement(*function.body, root, 0, return_types, true);

        std::string inferred;
        bool saw_void = false;
        bool saw_value = false;
        for (const auto& type : return_types) {
            if (type == "void") {
                saw_void = true;
            } else {
                saw_value = true;
                if (is_unknown(inferred)) {
                    inferred = type;
                } else if (!compatible(inferred, type) && !compatible(type, inferred)) {
                    error("XSEM0010", "Function has incompatible return types '" + inferred +
                          "' and '" + type + "'", function.span);
                } else if (is_numeric(inferred) && is_numeric(type)) {
                    inferred = numeric_result(inferred, type);
                }
            }
        }
        if (saw_void && saw_value) {
            error("XSEM0011", "Function mixes value returns with bare returns", function.span);
        }
        if (function.return_type.empty()) {
            inferred = saw_value ? inferred : "void";
        } else {
            inferred = function.return_type;
            for (const auto& actual : return_types) {
                if (!compatible(function.return_type, actual)) {
                    error("XSEM0012", "Return type '" + actual + "' is not compatible with declared '" +
                          function.return_type + "'", function.span);
                }
            }
        }
        inferred_returns_[fold(function.name) + "#" + std::to_string(function.parameters.size())] = inferred;
        for (auto& info : result_.functions) {
            if (info.signature == signature_of_function(function)) {
                info.inferred_return_type = inferred;
            }
        }
        for (auto& overload : functions_[fold(function.name)]) {
            if (overload.info.signature == signature_of_function(function)) {
                overload.info.inferred_return_type = inferred;
            }
        }
    }

    static std::string make_signature(const FunctionDeclaration& function) {
        std::ostringstream signature;
        signature << fold(function.name) << '(';
        for (std::size_t index = 0; index < function.parameters.size(); ++index) {
            if (index != 0U) signature << ',';
            signature << canonical_type(function.parameters[index].type_name);
        }
        signature << ')';
        return signature.str();
    }

    void analyze_statement(const Statement& statement, Scope& scope, int loop_depth,
                           std::vector<std::string>& return_types, bool is_function_body = false) {
        switch (statement.kind) {
        case StatementKind::Block: {
            if (is_function_body) {
                for (const auto& child : statement.statements) {
                    if (child) analyze_statement(*child, scope, loop_depth, return_types);
                }
            } else {
                Scope nested(&scope);
                for (const auto& child : statement.statements) {
                    if (child) analyze_statement(*child, nested, loop_depth, return_types);
                }
            }
            break;
        }
        case StatementKind::VariableDeclaration: {
            std::string initializer_type = "<unknown>";
            if (!statement.expressions.empty() && statement.expressions[0]) {
                initializer_type = analyze_expression(*statement.expressions[0], scope);
            }
            if (!statement.type_name.empty() && !compatible(statement.type_name, initializer_type)) {
                error("XSEM0013", "Cannot initialize '" + statement.type_name +
                      "' variable with '" + initializer_type + "'", statement.span);
            }
            declare_variable(scope, statement.name, statement.type_name, statement.span,
                             false, true, true);
            break;
        }
        case StatementKind::Assignment: {
            if (statement.expressions.size() < 2U || !statement.expressions[0] || !statement.expressions[1]) break;
            const Expression& target = *statement.expressions[0];
            const std::string value_type = analyze_expression(*statement.expressions[1], scope);
            if (target.kind == ExprKind::Identifier) {
                VariableSymbol* symbol = lookup(&scope, target.text);
                if (symbol != nullptr) {
                    if (!compatible(symbol->type, value_type)) {
                        error("XSEM0014", "Cannot assign '" + value_type + "' to '" + symbol->type +
                              "' variable '" + target.text + "'", statement.span);
                    }
                } else if (functions_.find(fold(target.text)) != functions_.end()) {
                    error("XSEM0015", "Cannot assign to function name '" + target.text + "'", target.span);
                } else if (is_import_root(target.text)) {
                    error("XSEM0016", "Cannot assign to module namespace '" + target.text + "'", target.span);
                } else {
                    declare_variable(scope, target.text, value_type, target.span,
                                     false, false, false);
                }
            } else {
                (void)analyze_expression(target, scope);
            }
            break;
        }
        case StatementKind::Expression:
            for (const auto& expression : statement.expressions) {
                if (expression) (void)analyze_expression(*expression, scope);
            }
            break;
        case StatementKind::Return:
            if (statement.expressions.empty() || !statement.expressions[0]) {
                return_types.push_back("void");
            } else {
                return_types.push_back(analyze_expression(*statement.expressions[0], scope));
            }
            break;
        case StatementKind::If: {
            if (!statement.expressions.empty() && statement.expressions[0]) {
                const std::string condition = analyze_expression(*statement.expressions[0], scope);
                require_boolean_condition(condition, statement.expressions[0]->span, "if");
            }
            for (const auto& child : statement.statements) {
                if (child) analyze_statement(*child, scope, loop_depth, return_types);
            }
            if (statement.else_branch) {
                analyze_statement(*statement.else_branch, scope, loop_depth, return_types);
            }
            break;
        }
        case StatementKind::While: {
            if (!statement.expressions.empty() && statement.expressions[0]) {
                const std::string condition = analyze_expression(*statement.expressions[0], scope);
                require_boolean_condition(condition, statement.expressions[0]->span, "while");
            }
            for (const auto& child : statement.statements) {
                if (child) analyze_statement(*child, scope, loop_depth + 1, return_types);
            }
            break;
        }
        case StatementKind::ForEach: {
            std::string element_type = "<unknown>";
            if (!statement.expressions.empty() && statement.expressions[0]) {
                const Expression& sequence = *statement.expressions[0];
                const std::string sequence_type = analyze_expression(sequence, scope);
                if (sequence.kind == ExprKind::Binary && sequence.text == "..") {
                    element_type = sequence_type.substr(6U, sequence_type.size() > 7U ? sequence_type.size() - 7U : 0U);
                } else if (sequence.kind == ExprKind::List && !sequence.children.empty()) {
                    element_type = analyze_expression(*sequence.children[0], scope);
                }
            }
            Scope loop_scope(&scope);
            declare_variable(loop_scope, statement.name, element_type, statement.span,
                             false, false, false);
            for (const auto& child : statement.statements) {
                if (child) analyze_statement(*child, loop_scope, loop_depth + 1, return_types);
            }
            break;
        }
        case StatementKind::Break:
        case StatementKind::Continue:
            if (loop_depth == 0) {
                error("XSEM0017", statement.kind == StatementKind::Break
                      ? "break is only valid inside a loop" : "continue is only valid inside a loop",
                      statement.span);
            }
            break;
        }
    }

    void require_boolean_condition(const std::string& type, const SourceSpan& span,
                                   const char* construct) {
        if (!is_unknown(type) && !is_boolean(type)) {
            error("XSEM0018", std::string(construct) + " condition must be bool, not '" + type + "'", span);
        }
    }

    std::string analyze_expression(const Expression& expression, Scope& scope) {
        switch (expression.kind) {
        case ExprKind::Literal:
            return literal_type(expression.text);
        case ExprKind::Identifier:
            return resolve_identifier(expression, scope);
        case ExprKind::Unary: {
            if (expression.children.empty() || !expression.children[0]) return "<unknown>";
            const std::string operand = analyze_expression(*expression.children[0], scope);
            if (expression.text == "!") {
                if (!is_unknown(operand) && !is_boolean(operand)) {
                    error("XSEM0019", "Logical negation requires bool, not '" + operand + "'", expression.span);
                }
                return "bool";
            }
            if (expression.text == "~") {
                if (!is_unknown(operand) && !is_integral(operand)) {
                    error("XSEM0020", "Bitwise complement requires an integer operand", expression.span);
                }
                return "int";
            }
            if ((expression.text == "+" || expression.text == "-") &&
                !is_unknown(operand) && !is_numeric(operand)) {
                error("XSEM0021", "Unary arithmetic requires a numeric operand", expression.span);
            }
            if (expression.text == "&") return operand + "*";
            if (expression.text == "*") {
                if (!is_unknown(operand) && !operand.empty() && operand.back() == '*') {
                    return operand.substr(0U, operand.size() - 1U);
                }
                return "<unknown>";
            }
            return operand;
        }
        case ExprKind::Binary:
            return analyze_binary(expression, scope);
        case ExprKind::Call:
            return analyze_call(expression, scope);
        case ExprKind::Member:
            if (!expression.children.empty() && expression.children[0]) {
                (void)analyze_expression(*expression.children[0], scope);
            }
            return "<unknown>";
        case ExprKind::Index:
            if (expression.children.size() >= 2U) {
                const std::string base = analyze_expression(*expression.children[0], scope);
                const std::string index = analyze_expression(*expression.children[1], scope);
                if (!is_unknown(index) && !is_integral(index)) {
                    error("XSEM0022", "Array/list index must be an integer", expression.children[1]->span);
                }
                if (base.size() > 2U && base.back() == ']' && base.find('[') != std::string::npos) {
                    return base.substr(base.find('[') + 1U, base.size() - base.find('[') - 2U);
                }
            }
            return "<unknown>";
        case ExprKind::List: {
            std::string element_type;
            for (const auto& child : expression.children) {
                if (!child) continue;
                const std::string next = analyze_expression(*child, scope);
                if (is_unknown(element_type)) {
                    element_type = next;
                } else if (!compatible(element_type, next) || !compatible(next, element_type)) {
                    error("XSEM0023", "List literal elements must have compatible types", child->span);
                    element_type = "<unknown>";
                } else if (is_numeric(element_type) && is_numeric(next)) {
                    element_type = numeric_result(element_type, next);
                }
            }
            return "List[" + (is_unknown(element_type) ? std::string("<unknown>") : element_type) + "]";
        }
        }
        return "<unknown>";
    }

    std::string literal_type(const std::string& text) const {
        const std::string value = fold(text);
        if (value == "true" || value == "false") return "bool";
        if (value == "null") return "null";
        if (!text.empty() && text.front() == '"') return "string";
        if (text.find_first_of(".eE") != std::string::npos) return "float";
        return "int";
    }

    std::string resolve_identifier(const Expression& expression, Scope& scope) {
        if (VariableSymbol* variable = lookup(&scope, expression.text)) {
            return variable->type;
        }
        if (functions_.find(fold(expression.text)) != functions_.end()) return "function";
        if (is_import_root(expression.text)) return "namespace";
        const std::string key = fold(expression.text) + "@" +
            std::to_string(expression.span.start.byte_offset) + ":" +
            std::to_string(expression.span.source_id);
        if (reported_unresolved_.insert(key).second) {
            error("XSEM0008", "Unresolved identifier '" + expression.text + "'", expression.span);
        }
        return "<error>";
    }

    std::string analyze_binary(const Expression& expression, Scope& scope) {
        if (expression.children.size() < 2U || !expression.children[0] || !expression.children[1]) {
            return "<unknown>";
        }
        const std::string left = analyze_expression(*expression.children[0], scope);
        const std::string right = analyze_expression(*expression.children[1], scope);
        const std::string& op = expression.text;
        if (op == "..") {
            if ((!is_unknown(left) && !is_numeric(left)) || (!is_unknown(right) && !is_numeric(right))) {
                error("XSEM0024", "Range endpoints must be numeric", expression.span);
            }
            if (!is_unknown(left) && !is_unknown(right) && !compatible(left, right) && !compatible(right, left)) {
                error("XSEM0025", "Range endpoints must have compatible types", expression.span);
            }
            return "Range<" + (is_numeric(left) && is_numeric(right) ? numeric_result(left, right) : "<unknown>") + ">";
        }
        if (op == "&&" || op == "||") {
            if ((!is_unknown(left) && !is_boolean(left)) || (!is_unknown(right) && !is_boolean(right))) {
                error("XSEM0026", "Logical operators require bool operands", expression.span);
            }
            return "bool";
        }
        if (is_comparison(op)) {
            if (!is_unknown(left) && !is_unknown(right) &&
                !compatible(left, right) && !compatible(right, left) &&
                !(is_numeric(left) && is_numeric(right))) {
                error("XSEM0027", "Comparison operands have incompatible types '" + left + "' and '" + right + "'", expression.span);
            }
            return "bool";
        }
        if (op == "+" && canonical_type(left) == "string" && canonical_type(right) == "string") {
            return "string";
        }
        if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>") {
            if ((!is_unknown(left) && !is_integral(left)) || (!is_unknown(right) && !is_integral(right))) {
                error("XSEM0028", "Bitwise operators require integer operands", expression.span);
            }
            return "int";
        }
        if ((!is_unknown(left) && !is_numeric(left)) || (!is_unknown(right) && !is_numeric(right))) {
            error("XSEM0029", "Arithmetic operators require numeric operands", expression.span);
            return "<unknown>";
        }
        if (!is_unknown(left) && !is_unknown(right) && !compatible(left, right) && !compatible(right, left)) {
            error("XSEM0030", "Arithmetic operands have incompatible types", expression.span);
            return "<unknown>";
        }
        if (is_unknown(left) || is_unknown(right)) return "<unknown>";
        return numeric_result(left, right);
    }

    bool is_imported_qualified_callee(const Expression& callee) const {
        const Expression* current = &callee;
        while (current->kind == ExprKind::Member && !current->children.empty() && current->children[0]) {
            current = current->children[0].get();
        }
        return current->kind == ExprKind::Identifier && is_import_root(current->text);
    }

    static std::string generic_parameter_name(const ModuleFunctionInterface& function,
                                              const std::string& type_name) {
        const std::string key = fold(type_name);
        for (const auto& parameter : function.generic_parameters) {
            const std::size_t colon = parameter.find(':');
            const std::string name = parameter.substr(0U, colon);
            if (fold(name) == key) return name;
        }
        return {};
    }

    std::string analyze_imported_call(const Expression& expression,
                                      const Expression& callee,
                                      const std::vector<std::string>& argument_types) {
        if (imported_functions_ == nullptr) return "<unknown>";
        std::vector<std::string> parts;
        if (!flatten_member_path(callee, parts) || parts.size() < 2U) return "<unknown>";
        const std::string module_name = join_member_path(parts, parts.size() - 1U);
        const auto module = imported_functions_->find(fold(module_name));
        if (module == imported_functions_->end()) return "<unknown>";

        std::vector<const ModuleFunctionInterface*> overloads;
        for (const auto& function : module->second) {
            if (fold(function.name) == fold(parts.back()) &&
                function.visibility == Visibility::Public) {
                overloads.push_back(&function);
            }
        }
        if (overloads.empty()) return "<unknown>"; // Resolver owns visibility/name diagnostics.

        int best_score = -1;
        const ModuleFunctionInterface* best = nullptr;
        std::unordered_map<std::string, std::string> best_substitutions;
        bool ambiguous = false;
        bool had_arity_candidate = false;

        for (const ModuleFunctionInterface* candidate : overloads) {
            if (argument_types.size() < candidate->required_parameter_count ||
                argument_types.size() > candidate->parameters.size()) continue;
            had_arity_candidate = true;
            int score = 0;
            bool viable = true;
            std::unordered_map<std::string, std::string> substitutions;
            for (std::size_t index = 0; index < argument_types.size(); ++index) {
                const std::string& actual = argument_types[index];
                const std::string& expected = candidate->parameters[index].type_name;
                if (is_unknown(actual)) continue;
                const std::string generic = generic_parameter_name(*candidate, expected);
                if (!generic.empty()) {
                    const std::string generic_key = fold(generic);
                    const auto prior = substitutions.find(generic_key);
                    if (prior == substitutions.end()) {
                        substitutions.emplace(generic_key, actual);
                        score += 1;
                    } else if (canonical_type(prior->second) == canonical_type(actual)) {
                        score += 1;
                    } else {
                        viable = false;
                        break;
                    }
                } else if (canonical_type(expected) == canonical_type(actual)) {
                    score += 2;
                } else if (compatible(expected, actual)) {
                    score += 1;
                } else {
                    viable = false;
                    break;
                }
            }
            if (!viable) continue;
            if (score > best_score) {
                best_score = score;
                best = candidate;
                best_substitutions = std::move(substitutions);
                ambiguous = false;
            } else if (score == best_score) {
                ambiguous = true;
            }
        }

        if (best == nullptr) {
            if (had_arity_candidate) {
                error("XSEM0035", "No exported overload of '" + parts.back() +
                      "' in module '" + module_name + "' matches the argument types", expression.span);
            }
            return "<error>";
        }
        if (ambiguous) {
            error("XSEM0036", "Imported call to '" + parts.back() +
                  "' is ambiguous for the known argument types", expression.span);
            return "<unknown>";
        }
        if (best->return_type.empty() || is_unknown(best->return_type)) return "<unknown>";
        return substitute_generic_type(best->return_type, best_substitutions);
    }

    std::string analyze_call(const Expression& expression, Scope& scope) {
        if (expression.children.empty() || !expression.children[0]) return "<unknown>";
        const Expression& callee = *expression.children[0];
        std::vector<std::string> argument_types;
        for (std::size_t index = 1; index < expression.children.size(); ++index) {
            if (expression.children[index]) {
                argument_types.push_back(analyze_expression(*expression.children[index], scope));
            }
        }

        if (callee.kind == ExprKind::Identifier) {
            const std::string name = fold(callee.text);
            const auto found = functions_.find(name);
            if (found == functions_.end()) {
                const auto builtin = builtin_return(name);
                if (!builtin.empty()) return builtin;
                if (lookup(&scope, callee.text) != nullptr) {
                    error("XSEM0031", "Variable '" + callee.text + "' is not callable", callee.span);
                } else {
                    error("XSEM0008", "Unresolved function '" + callee.text + "'", callee.span);
                }
                return "<error>";
            }

            std::vector<const FunctionSymbol*> arity_candidates;
            for (const auto& candidate : found->second) {
                if (argument_types.size() >= candidate.info.required_parameter_count &&
                    argument_types.size() <= candidate.info.total_parameter_count) {
                    arity_candidates.push_back(&candidate);
                }
            }
            if (arity_candidates.empty()) {
                error("XSEM0032", "No overload of '" + callee.text + "' accepts " +
                      std::to_string(argument_types.size()) + " argument(s)", expression.span);
                return "<error>";
            }
            int best_score = -1;
            const FunctionSymbol* best = nullptr;
            bool ambiguous = false;
            for (const FunctionSymbol* candidate : arity_candidates) {
                int score = 0;
                bool viable = true;
                for (std::size_t index = 0; index < argument_types.size(); ++index) {
                    const std::string& parameter_type = candidate->parameter_types[index];
                    if (candidate->generic_parameter_positions[index] || is_unknown(argument_types[index])) {
                        continue;
                    }
                    if (canonical_type(parameter_type) == canonical_type(argument_types[index])) {
                        score += 2;
                    } else if (compatible(parameter_type, argument_types[index])) {
                        score += 1;
                    } else {
                        viable = false;
                        break;
                    }
                }
                if (viable && score > best_score) {
                    best_score = score;
                    best = candidate;
                    ambiguous = false;
                } else if (viable && score == best_score) {
                    ambiguous = true;
                }
            }
            if (best == nullptr) {
                error("XSEM0033", "No overload of '" + callee.text + "' matches argument types", expression.span);
                return "<error>";
            }
            if (ambiguous) {
                error("XSEM0034", "Call to overloaded function '" + callee.text + "' is ambiguous", expression.span);
                return "<unknown>";
            }
            return best->info.inferred_return_type.empty() ? "<unknown>" : best->info.inferred_return_type;
        }

        if (callee.kind == ExprKind::Member && is_imported_qualified_callee(callee)) {
            return analyze_imported_call(expression, callee, argument_types);
        }
        const std::string callee_type = analyze_expression(callee, scope);
        if (callee_type == "function") return "<unknown>";
        if (!is_unknown(callee_type) && callee_type != "<error>") {
            error("XSEM0031", "Expression is not callable", callee.span);
        }
        return "<unknown>";
    }

    static std::string builtin_return(const std::string& name) {
        if (name == "print" || name == "printline" || name == "assert" || name == "exit") return "void";
        if (name == "input") return "string";
        return {};
    }
};

} // namespace

SemanticAnalyzer::SemanticAnalyzer(const CompilationUnit& unit) : unit_(unit) {}

SemanticResult SemanticAnalyzer::run() {
    AnalyzerImpl analyzer(unit_);
    SemanticResult result = analyzer.run();
    ControlFlowResult flow = analyze_control_flow(unit_);
    result.diagnostics.insert(result.diagnostics.end(),
                              flow.diagnostics.begin(), flow.diagnostics.end());
    LifetimeResult lifetime = analyze_lifetimes(unit_);
    result.diagnostics.insert(result.diagnostics.end(),
                              lifetime.diagnostics.begin(), lifetime.diagnostics.end());
    return result;
}

SemanticResult analyze_semantics(const CompilationUnit& unit) {
    SemanticAnalyzer analyzer(unit);
    return analyzer.run();
}

SemanticResult analyze_source(const std::string& source) {
    ParseResult parsed = parse_source(source);
    if (!parsed.diagnostics.empty()) {
        SemanticResult result;
        result.diagnostics = std::move(parsed.diagnostics);
        return result;
    }
    return analyze_semantics(parsed.unit);
}

SemanticResult analyze_source(const SourceManager& sources, SourceFileId source_id) {
    ParseResult parsed = parse_source(sources, source_id);
    if (!parsed.diagnostics.empty()) {
        SemanticResult result;
        result.diagnostics = std::move(parsed.diagnostics);
        return result;
    }
    return analyze_semantics(parsed.unit);
}

SemanticResult analyze_program(const std::vector<ProgramFile>& files) {
    SemanticResult combined;
    ModuleResolutionResult module_result = resolve_modules(files);
    combined.diagnostics = std::move(module_result.diagnostics);

    std::unordered_map<std::string, std::vector<const FunctionDeclaration*>> module_functions;
    std::unordered_map<std::string, std::unordered_set<std::string>> module_signatures;
    for (const auto& file : files) {
        if (!file.unit || file.unit->module_name.empty()) continue;
        const std::string module_key = fold(file.unit->module_name);
        for (const auto& function : file.unit->functions) {
            const std::string signature = signature_of_function(function);
            if (module_signatures[module_key].insert(signature).second) {
                module_functions[module_key].push_back(&function);
            } else {
                auto& functions = module_functions[module_key];
                const auto existing = std::find_if(functions.begin(), functions.end(), [&](const auto* old) {
                    return signature_of_function(*old) == signature;
                });
                if (existing != functions.end() && !(*existing)->has_body && function.has_body &&
                    (*existing)->visibility == function.visibility) {
                    *existing = &function;
                }
            }
        }
    }

    ImportedModuleFunctionMap imported_functions;
    std::size_t exported_function_count = 0U;
    for (const auto& module : module_result.modules) {
        auto& functions = imported_functions[fold(module.name)];
        functions = module.public_functions;
        exported_function_count += functions.size();
    }

    // Return types inferred in one module become part of its exported interface.
    // Iterate to a fixed point so acyclic dependency chains can propagate types
    // regardless of source-file order. Intermediate diagnostics are discarded;
    // only the final semantic round is published.
    SemanticResult final_round;
    const std::size_t maximum_rounds = std::max<std::size_t>(1U, exported_function_count + 1U);
    for (std::size_t round = 0; round < maximum_rounds; ++round) {
        SemanticResult current_round;
        std::unordered_map<std::string,
            std::unordered_map<std::string, std::string>> inferred_by_module;
        for (const auto& file : files) {
            if (!file.unit) continue;
            const std::string module_key = fold(file.unit->module_name);
            const auto found = module_functions.find(module_key);
            const std::vector<const FunctionDeclaration*>* visible =
                found == module_functions.end() ? nullptr : &found->second;
            AnalyzerImpl analyzer(*file.unit, visible, &imported_functions);
            SemanticResult one = analyzer.run();
            for (const auto& info : one.functions) {
                if (!is_unknown(info.inferred_return_type)) {
                    inferred_by_module[module_key][info.signature] = info.inferred_return_type;
                }
            }
            current_round.functions.insert(current_round.functions.end(),
                one.functions.begin(), one.functions.end());
            current_round.diagnostics.insert(current_round.diagnostics.end(),
                one.diagnostics.begin(), one.diagnostics.end());
        }

        bool changed = false;
        for (auto& module : imported_functions) {
            const auto inferred_module = inferred_by_module.find(module.first);
            if (inferred_module == inferred_by_module.end()) continue;
            for (auto& function : module.second) {
                const auto inferred = inferred_module->second.find(function.signature);
                if (inferred != inferred_module->second.end() &&
                    (function.return_type.empty() || is_unknown(function.return_type))) {
                    function.return_type = inferred->second;
                    changed = true;
                }
            }
        }
        final_round = std::move(current_round);
        if (!changed) break;
    }

    combined.functions = std::move(final_round.functions);
    combined.diagnostics.insert(combined.diagnostics.end(),
        final_round.diagnostics.begin(), final_round.diagnostics.end());

    // Control-flow diagnostics are emitted once, after the interface fixed point.
    for (const auto& file : files) {
        if (!file.unit) continue;
        ControlFlowResult flow = analyze_control_flow(*file.unit);
        combined.diagnostics.insert(combined.diagnostics.end(),
            flow.diagnostics.begin(), flow.diagnostics.end());
        LifetimeResult lifetime = analyze_lifetimes(*file.unit);
        combined.diagnostics.insert(combined.diagnostics.end(),
            lifetime.diagnostics.begin(), lifetime.diagnostics.end());
    }

    std::unordered_set<std::string> seen;
    std::vector<Diagnostic> unique;
    unique.reserve(combined.diagnostics.size());
    for (auto& diagnostic : combined.diagnostics) {
        std::ostringstream key;
        key << diagnostic.code << '|' << diagnostic.span.source_id << '|'
            << diagnostic.span.start.byte_offset << '|' << diagnostic.message;
        if (seen.insert(key.str()).second) unique.push_back(std::move(diagnostic));
    }
    combined.diagnostics = std::move(unique);
    return combined;
}

SemanticResult analyze_program(const SourceManager& sources,
                               const std::vector<SourceFileId>& source_ids) {
    std::vector<ProgramFile> files;
    SemanticResult result;
    for (const SourceFileId id : source_ids) {
        const SourceFile* source = sources.get(id);
        if (source == nullptr) {
            SourceSpan span;
            span.source_id = id;
            result.diagnostics.push_back(Diagnostic{
                "XSRC0001", "Unknown source file id in program input", span});
            continue;
        }
        ParseResult parsed = parse_source(sources, id);
        if (!parsed.diagnostics.empty()) {
            result.diagnostics.insert(result.diagnostics.end(),
                                      parsed.diagnostics.begin(), parsed.diagnostics.end());
            continue;
        }
        ProgramFile file;
        file.display_name = source->display_name;
        file.source_id = id;
        file.unit = std::make_shared<CompilationUnit>(std::move(parsed.unit));
        files.push_back(std::move(file));
    }
    SemanticResult analyzed = analyze_program(files);
    result.functions = std::move(analyzed.functions);
    result.diagnostics.insert(result.diagnostics.end(),
                              analyzed.diagnostics.begin(), analyzed.diagnostics.end());
    return result;
}

} // namespace xfrontend
