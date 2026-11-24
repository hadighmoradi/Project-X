// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/control_flow.hpp"
#include "xfrontend/source_manager.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <memory>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace xfrontend {
namespace {

std::string fold_name(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= 'A' && byte <= 'Z') {
            ch = static_cast<char>(byte - 'A' + 'a');
        }
    }
    return value;
}

bool is_unique_type(const std::string& type_name) {
    std::string type = fold_name(type_name);
    type.erase(std::remove_if(type.begin(), type.end(), [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    }), type.end());
    return type.rfind("unique[", 0U) == 0U || type.rfind("uniquearray[", 0U) == 0U;
}

bool is_method_named(const Expression& expression, const char* name,
                     const Expression** receiver_out = nullptr) {
    if (expression.kind != ExprKind::Call || expression.children.empty() || !expression.children[0]) return false;
    const Expression& callee = *expression.children[0];
    if (callee.kind != ExprKind::Member || fold_name(callee.text) != fold_name(name) ||
        callee.children.empty() || !callee.children[0]) return false;
    if (receiver_out) *receiver_out = callee.children[0].get();
    return true;
}

std::optional<bool> constant_boolean(const Expression* expression) {
    if (expression == nullptr || expression->kind != ExprKind::Literal) return std::nullopt;
    const std::string value = fold_name(expression->text);
    if (value == "true") return true;
    if (value == "false") return false;
    return std::nullopt;
}

bool contains_value_return(const Statement& statement) {
    if (statement.kind == StatementKind::Return && !statement.expressions.empty() &&
        statement.expressions[0]) {
        return true;
    }
    for (const auto& child : statement.statements) {
        if (child && contains_value_return(*child)) return true;
    }
    return statement.else_branch && contains_value_return(*statement.else_branch);
}

bool function_requires_value_return(const FunctionDeclaration& function) {
    if (!function.return_type.empty()) {
        return fold_name(function.return_type) != "void";
    }
    return function.body && contains_value_return(*function.body);
}

bool is_builtin_name(const std::string& name) {
    const std::string key = fold_name(name);
    return key == "print" || key == "printline" || key == "input" ||
           key == "exit" || key == "assert";
}

struct VariableBinding {
    FlowVariableId id = 0;
};

using LexicalScope = std::unordered_map<std::string, VariableBinding>;

struct LoopTargets {
    FlowBlockId break_target = InvalidFlowBlockId;
    FlowBlockId continue_target = InvalidFlowBlockId;
    std::size_t owner_scope_depth = 0;
    FlowControlConstructId control_construct_id = InvalidFlowControlConstructId;
};

class FunctionFlowBuilder {
public:
    FunctionFlowBuilder(const FunctionDeclaration& function,
                        const std::vector<std::string>& imports,
                        const std::string& module_name)
        : function_(function) {
        graph_.function_name = function.name;
        if (function_.body) {
            for (const auto& identity : enumerate_flow_control_statements(*function_.body)) {
                if (identity.statement == nullptr || identity.id == InvalidFlowControlStatementId) continue;
                control_statement_ids_.emplace(identity.statement, identity.id);
                graph_.control_statements.push_back(FlowControlStatement{
                    identity.id, identity.statement->kind, identity.parent_construct_id,
                    identity.statement->span});
            }
        }
        for (const auto& imported : imports) {
            const std::size_t dot = imported.find('.');
            import_roots_.insert(fold_name(imported.substr(0, dot)));
        }
        if (!module_name.empty()) {
            const std::size_t dot = module_name.find('.');
            import_roots_.insert(fold_name(module_name.substr(0, dot)));
        }
        scopes_.emplace_back();
        owner_scopes_.emplace_back();
        std::vector<std::pair<FlowVariableId, std::string>> unique_take_parameters;
        for (const auto& parameter : function_.parameters) {
            const FlowVariableId id = add_variable(parameter.name, parameter.span, true,
                                                   parameter.type_name, parameter.passing_mode);
            scopes_.back()[fold_name(parameter.name)] = VariableBinding{id};
            parameter_ids_.push_back(id);
            if (is_unique_type(parameter.type_name) && fold_name(parameter.passing_mode) == "take") {
                unique_take_parameters.emplace_back(id, parameter.name);
                owner_scopes_.back().push_back(id);
            }
        }
        graph_.entry_block = add_block(function_.span);
        graph_.exit_block = add_block();
        for (const auto& parameter : unique_take_parameters) {
            emit(graph_.entry_block, FlowOperationKind::OwnerCreate, parameter.first,
                 parameter.second, function_.span);
        }
    }

    ControlFlowGraph build() {
        if (function_.body && function_.has_body) {
            const FlowBlockId tail = build_statement(*function_.body, graph_.entry_block, false);
            if (tail != InvalidFlowBlockId) {
                emit_owner_drops_from(tail, 0U);
                add_edge(tail, graph_.exit_block);
            }
        } else {
            add_edge(graph_.entry_block, graph_.exit_block);
        }
        return std::move(graph_);
    }

    const std::vector<FlowVariableId>& parameter_ids() const noexcept {
        return parameter_ids_;
    }

private:
    const FunctionDeclaration& function_;
    ControlFlowGraph graph_;
    std::vector<LexicalScope> scopes_;
    std::vector<std::vector<FlowVariableId>> owner_scopes_;
    std::vector<LoopTargets> loops_;
    std::vector<FlowVariableId> parameter_ids_;
    std::unordered_set<std::string> import_roots_;
    std::unordered_map<const Statement*, FlowControlStatementId> control_statement_ids_;
    FlowEdgeId next_edge_id_ = 1U;

    FlowBlockId add_block(const SourceSpan& span = {}) {
        const FlowBlockId id = graph_.blocks.size();
        BasicBlock block;
        block.id = id;
        block.span = span;
        graph_.blocks.push_back(std::move(block));
        return id;
    }

    FlowControlStatementId control_statement_id(const Statement& statement) const {
        const auto found = control_statement_ids_.find(&statement);
        return found == control_statement_ids_.end()
            ? InvalidFlowControlStatementId : found->second;
    }

    void add_edge(FlowBlockId from, FlowBlockId to,
                  FlowEdgeKind kind = FlowEdgeKind::Sequential,
                  FlowControlStatementId control_id = InvalidFlowControlStatementId,
                  FlowControlConstructId construct_id = InvalidFlowControlConstructId,
                  const SourceSpan& span = {}) {
        if (from == InvalidFlowBlockId || to == InvalidFlowBlockId ||
            from >= graph_.blocks.size() || to >= graph_.blocks.size()) {
            return;
        }
        auto& successors = graph_.blocks[from].successors;
        if (std::find(successors.begin(), successors.end(), to) != successors.end()) {
            return;
        }
        if (next_edge_id_ == InvalidFlowEdgeId) return;
        const FlowEdgeId edge_id = next_edge_id_++;
        successors.push_back(to);
        graph_.blocks[to].predecessors.push_back(from);
        graph_.edges.push_back(FlowEdge{edge_id, from, to, kind, control_id,
                                        construct_id, span});
    }

    void emit(FlowBlockId block, FlowOperationKind kind, FlowVariableId id,
              const std::string& name, const SourceSpan& span, bool initialized = false,
              FlowVariableId related_variable_id = InvalidFlowVariableId,
              bool conditional = false) {
        if (block == InvalidFlowBlockId || block >= graph_.blocks.size()) return;
        auto& operations = graph_.blocks[block].operations;
        if (block >= static_cast<FlowBlockId>(std::numeric_limits<std::uint32_t>::max()) ||
            operations.size() >= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max() - 1U)) {
            // The native bridge reports an explicit capacity error if a CFG ever
            // exceeds the stable 32-bit block/operation coordinate space.
            operations.push_back(FlowOperation{kind, id, name, span, initialized,
                related_variable_id, conditional});
            return;
        }
        const auto operation_index = static_cast<std::uint32_t>(operations.size());
        FlowOperation operation{kind, id, name, span, initialized,
            related_variable_id, conditional};
        operation.operation_index = operation_index;
        operation.stable_id = (static_cast<FlowOperationId>(block + 1U) << 32U) |
            static_cast<FlowOperationId>(operation_index + 1U);
        operations.push_back(std::move(operation));
    }

    FlowVariableId add_variable(const std::string& name, const SourceSpan& span,
                                 bool parameter = false, const std::string& type_name = {},
                                 const std::string& passing_mode = {}) {
        const FlowVariableId id = graph_.variables.size();
        graph_.variables.push_back(FlowVariable{id, name, span, parameter, type_name,
                                                 passing_mode, is_unique_type(type_name)});
        return id;
    }

    std::optional<FlowVariableId> lookup_variable(const std::string& name) const {
        const std::string key = fold_name(name);
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            const auto found = it->find(key);
            if (found != it->end()) return found->second.id;
        }
        return std::nullopt;
    }

    FlowVariableId declare_variable(const std::string& name, const SourceSpan& span,
                                    bool parameter = false, const std::string& type_name = {},
                                    const std::string& passing_mode = {}) {
        const FlowVariableId id = add_variable(name, span, parameter, type_name, passing_mode);
        if (!scopes_.empty()) scopes_.back()[fold_name(name)] = VariableBinding{id};
        if (!parameter && is_unique_type(type_name) && !owner_scopes_.empty()) {
            owner_scopes_.back().push_back(id);
        }
        return id;
    }

    void push_scope() {
        scopes_.emplace_back();
        owner_scopes_.emplace_back();
    }

    void pop_scope() {
        if (!scopes_.empty()) scopes_.pop_back();
        if (!owner_scopes_.empty()) owner_scopes_.pop_back();
    }

    void emit_owner_drops_from(FlowBlockId block, std::size_t first_scope) {
        if (first_scope >= owner_scopes_.size()) return;
        for (std::size_t scope = owner_scopes_.size(); scope-- > first_scope;) {
            const auto& variables = owner_scopes_[scope];
            for (auto variable = variables.rbegin(); variable != variables.rend(); ++variable) {
                const auto& info = graph_.variables[*variable];
                emit(block, FlowOperationKind::OwnerDrop, *variable, info.name,
                     info.declaration_span, false, InvalidFlowVariableId, true);
            }
        }
    }

    bool is_import_root(const std::string& name) const {
        return import_roots_.find(fold_name(name)) != import_roots_.end();
    }

    bool is_import_qualified(const Expression& expression) const {
        const Expression* current = &expression;
        while (current->kind == ExprKind::Member && !current->children.empty() &&
               current->children[0]) {
            current = current->children[0].get();
        }
        return current->kind == ExprKind::Identifier && is_import_root(current->text);
    }

    void read_expression(const Expression& expression, FlowBlockId block, bool emit_owner_mutation = true) {
        switch (expression.kind) {
        case ExprKind::Literal:
            break;
        case ExprKind::Identifier: {
            const auto variable = lookup_variable(expression.text);
            if (variable) {
                emit(block, FlowOperationKind::Read, *variable, expression.text, expression.span);
            }
            break;
        }
        case ExprKind::Unary:
        case ExprKind::Binary:
        case ExprKind::List:
        case ExprKind::Index:
            for (const auto& child : expression.children) {
                if (child) read_expression(*child, block, emit_owner_mutation);
            }
            break;
        case ExprKind::Member:
            if (is_import_qualified(expression)) break;
            if (!expression.children.empty() && expression.children[0]) {
                read_expression(*expression.children[0], block, emit_owner_mutation);
            }
            break;
        case ExprKind::Call: {
            if (expression.children.empty() || !expression.children[0]) break;
            const Expression& callee = *expression.children[0];
            if (callee.kind == ExprKind::Identifier) {
                if (lookup_variable(callee.text)) {
                    read_expression(callee, block, emit_owner_mutation);
                } else if (is_builtin_name(callee.text)) {
                    // Prelude functions are not variable reads.
                } else {
                    // User functions (including unresolved ones) are diagnosed by semantic analysis.
                }
            } else if (callee.kind == ExprKind::Member && is_import_qualified(callee)) {
                // Imported module namespace is not a local variable.
            } else {
                read_expression(callee, block, emit_owner_mutation);
            }
            for (std::size_t index = 1; index < expression.children.size(); ++index) {
                if (expression.children[index]) read_expression(*expression.children[index], block, emit_owner_mutation);
            }
            // Keep the call as an explicit operation after evaluating its callee
            // and arguments so downstream native-register analysis sees the ABI
            // clobber boundary in source evaluation order.
            emit(block, FlowOperationKind::Call, InvalidFlowVariableId, {}, expression.span);
            const Expression* owner_receiver = nullptr;
            const bool take_event = is_method_named(expression, "Take", &owner_receiver);
            const bool reset_event = is_method_named(expression, "Reset", &owner_receiver) ||
                                     is_method_named(expression, "Release", &owner_receiver);
            if (emit_owner_mutation && (take_event || reset_event) && owner_receiver != nullptr &&
                owner_receiver->kind == ExprKind::Identifier) {
                const auto owner_id = lookup_variable(owner_receiver->text);
                if (owner_id && graph_.variables[*owner_id].is_unique_owner) {
                    emit(block, take_event ? FlowOperationKind::OwnerTransfer : FlowOperationKind::OwnerReset,
                         *owner_id, owner_receiver->text, expression.span);
                }
            }
            break;
        }
        }
    }

    void read_target_address(const Expression& target, FlowBlockId block) {
        if (target.kind == ExprKind::Identifier) return;
        read_expression(target, block);
    }

    FlowBlockId build_statement(const Statement& statement, FlowBlockId current,
                                bool create_block_scope = true) {
        if (current == InvalidFlowBlockId) return InvalidFlowBlockId;
        switch (statement.kind) {
        case StatementKind::Block: {
            if (create_block_scope) push_scope();
            FlowBlockId tail = current;
            for (const auto& child : statement.statements) {
                if (!child) continue;
                // Preserve source-order statements after return/break/continue
                // as detached blocks so reachability consumers can identify them.
                if (tail == InvalidFlowBlockId) tail = add_block(child->span);
                tail = build_statement(*child, tail, true);
            }
            if (create_block_scope) {
                if (tail != InvalidFlowBlockId) emit_owner_drops_from(tail, owner_scopes_.size() - 1U);
                pop_scope();
            }
            return tail;
        }
        case StatementKind::VariableDeclaration: {
            if (!statement.expressions.empty() && statement.expressions[0]) {
                const Expression* take_receiver = nullptr;
                const bool top_level_take = is_method_named(*statement.expressions[0], "Take", &take_receiver);
                read_expression(*statement.expressions[0], current, !top_level_take);
            }
            const FlowVariableId id = declare_variable(statement.name, statement.span, false,
                                                       statement.type_name);
            emit(current, FlowOperationKind::Declare, id, statement.name, statement.span,
                 !statement.expressions.empty());
            if (graph_.variables[id].is_unique_owner) {
                const Expression* initializer = statement.expressions.empty()
                    ? nullptr : statement.expressions[0].get();
                if (initializer == nullptr) {
                    emit(current, FlowOperationKind::OwnerReset, id, statement.name, statement.span);
                } else {
                    const Expression* take_receiver = nullptr;
                    if (is_method_named(*initializer, "Take", &take_receiver) &&
                        take_receiver != nullptr && take_receiver->kind == ExprKind::Identifier) {
                        const auto source_id = lookup_variable(take_receiver->text);
                        if (source_id && graph_.variables[*source_id].is_unique_owner) {
                            emit(current, FlowOperationKind::OwnerTransfer, *source_id,
                                 take_receiver->text, initializer->span, false, id);
                        } else {
                            emit(current, FlowOperationKind::OwnerCreate, id, statement.name, initializer->span);
                        }
                    } else if (initializer->kind == ExprKind::Identifier) {
                        const auto source_id = lookup_variable(initializer->text);
                        if (source_id && graph_.variables[*source_id].is_unique_owner) {
                            emit(current, FlowOperationKind::OwnerCopy, id, statement.name,
                                 initializer->span, false, *source_id);
                        } else {
                            emit(current, FlowOperationKind::OwnerCreate, id, statement.name, initializer->span);
                        }
                    } else {
                        emit(current, FlowOperationKind::OwnerCreate, id, statement.name, initializer->span);
                    }
                }
            }
            return current;
        }
        case StatementKind::Assignment: {
            if (statement.expressions.size() < 2U || !statement.expressions[0] ||
                !statement.expressions[1]) return current;
            const Expression& target = *statement.expressions[0];
            const Expression& value = *statement.expressions[1];
            const Expression* top_level_take_receiver = nullptr;
            const bool top_level_take = is_method_named(value, "Take", &top_level_take_receiver);
            read_expression(value, current, !top_level_take);
            if (target.kind == ExprKind::Identifier) {
                auto id = lookup_variable(target.text);
                if (!id) id = declare_variable(target.text, target.span);
                if (graph_.variables[*id].is_unique_owner) {
                    const Expression* self_take_receiver = nullptr;
                    const bool self_take = is_method_named(value, "Take", &self_take_receiver) &&
                        self_take_receiver != nullptr && self_take_receiver->kind == ExprKind::Identifier &&
                        lookup_variable(self_take_receiver->text).has_value() &&
                        *lookup_variable(self_take_receiver->text) == *id;
                    // Replacing an existing owner first clears its old resource.
                    // Self-Take restores the resource into the same wrapper.
                    if (!self_take) {
                        emit(current, FlowOperationKind::OwnerReset, *id, target.text, target.span);
                    }
                }
                emit(current, FlowOperationKind::Write, *id, target.text, target.span);
                if (graph_.variables[*id].is_unique_owner) {
                    const Expression* take_receiver = nullptr;
                    if (is_method_named(value, "Take", &take_receiver) &&
                        take_receiver != nullptr && take_receiver->kind == ExprKind::Identifier) {
                        const auto source_id = lookup_variable(take_receiver->text);
                        if (source_id && graph_.variables[*source_id].is_unique_owner) {
                            emit(current, FlowOperationKind::OwnerTransfer, *source_id,
                                 take_receiver->text, value.span, false, *id);
                        } else {
                            emit(current, FlowOperationKind::OwnerCreate, *id, target.text, value.span);
                        }
                    } else if (value.kind == ExprKind::Identifier) {
                        const auto source_id = lookup_variable(value.text);
                        if (source_id && graph_.variables[*source_id].is_unique_owner) {
                            emit(current, FlowOperationKind::OwnerCopy, *id, target.text,
                                 value.span, false, *source_id);
                        } else {
                            emit(current, FlowOperationKind::OwnerCreate, *id, target.text, value.span);
                        }
                    } else {
                        emit(current, FlowOperationKind::OwnerCreate, *id, target.text, value.span);
                    }
                }
            } else {
                read_target_address(target, current);
            }
            return current;
        }
        case StatementKind::Expression:
            for (const auto& expression : statement.expressions) {
                if (expression) read_expression(*expression, current);
            }
            return current;
        case StatementKind::Return:
            if (!statement.expressions.empty() && statement.expressions[0]) {
                const Expression& returned = *statement.expressions[0];
                read_expression(returned, current);
                if (returned.kind == ExprKind::Identifier) {
                    const auto returned_id = lookup_variable(returned.text);
                    if (returned_id && graph_.variables[*returned_id].is_unique_owner) {
                        emit(current, FlowOperationKind::OwnerTransfer, *returned_id,
                             returned.text, returned.span);
                    }
                }
            }
            emit_owner_drops_from(current, 0U);
            emit(current, FlowOperationKind::Return, InvalidFlowVariableId, {}, statement.span);
            add_edge(current, graph_.exit_block, FlowEdgeKind::Return,
                     InvalidFlowControlStatementId, InvalidFlowControlConstructId, statement.span);
            return InvalidFlowBlockId;
        case StatementKind::If: {
            const FlowControlStatementId if_id = control_statement_id(statement);
            if (if_id == InvalidFlowControlStatementId) return current;
            if (!statement.expressions.empty() && statement.expressions[0]) {
                read_expression(*statement.expressions[0], current);
            }
            const std::optional<bool> known = constant_boolean(
                statement.expressions.empty() ? nullptr : statement.expressions[0].get());
            const FlowBlockId then_entry = add_block(statement.span);
            const FlowBlockId join = add_block(statement.span);
            if (statement.else_branch) {
                const FlowBlockId else_entry = add_block(statement.else_branch->span);
                if (!known || *known) add_edge(current, then_entry, FlowEdgeKind::IfTrue,
                    if_id, if_id, statement.span);
                if (!known || !*known) add_edge(current, else_entry, FlowEdgeKind::IfFalse,
                    if_id, if_id, statement.span);
                push_scope();
                FlowBlockId then_tail = then_entry;
                for (const auto& child : statement.statements) {
                    if (!child) continue;
                    if (then_tail == InvalidFlowBlockId) then_tail = add_block(child->span);
                    then_tail = build_statement(*child, then_tail, true);
                }
                if (then_tail != InvalidFlowBlockId) {
                    emit_owner_drops_from(then_tail, owner_scopes_.size() - 1U);
                    add_edge(then_tail, join, FlowEdgeKind::IfThenMerge,
                        if_id, if_id, statement.span);
                }
                pop_scope();
                push_scope();
                const FlowBlockId else_tail = build_statement(*statement.else_branch, else_entry, true);
                if (else_tail != InvalidFlowBlockId) {
                    emit_owner_drops_from(else_tail, owner_scopes_.size() - 1U);
                    add_edge(else_tail, join, FlowEdgeKind::IfElseMerge,
                        if_id, if_id, statement.span);
                }
                pop_scope();
            } else {
                if (!known || !*known) add_edge(current, join, FlowEdgeKind::IfFalse,
                    if_id, if_id, statement.span); // False condition skips the body.
                if (!known || *known) add_edge(current, then_entry, FlowEdgeKind::IfTrue,
                    if_id, if_id, statement.span);
                push_scope();
                FlowBlockId then_tail = then_entry;
                for (const auto& child : statement.statements) {
                    if (!child) continue;
                    if (then_tail == InvalidFlowBlockId) then_tail = add_block(child->span);
                    then_tail = build_statement(*child, then_tail, true);
                }
                if (then_tail != InvalidFlowBlockId) {
                    emit_owner_drops_from(then_tail, owner_scopes_.size() - 1U);
                    add_edge(then_tail, join, FlowEdgeKind::IfThenMerge,
                        if_id, if_id, statement.span);
                }
                pop_scope();
            }
            return join;
        }
        case StatementKind::While: {
            const FlowControlStatementId while_id = control_statement_id(statement);
            if (while_id == InvalidFlowControlStatementId) return current;
            const FlowBlockId condition = add_block(statement.span);
            const FlowBlockId body_entry = add_block(statement.span);
            const FlowBlockId after = add_block(statement.span);
            add_edge(current, condition, FlowEdgeKind::LoopEntry,
                while_id, while_id, statement.span);
            if (!statement.expressions.empty() && statement.expressions[0]) {
                read_expression(*statement.expressions[0], condition);
            }
            const std::optional<bool> known = constant_boolean(
                statement.expressions.empty() ? nullptr : statement.expressions[0].get());
            if (!known || *known) add_edge(condition, body_entry, FlowEdgeKind::LoopBody,
                while_id, while_id, statement.span);
            if (!known || !*known) add_edge(condition, after, FlowEdgeKind::LoopExit,
                while_id, while_id, statement.span); // Unknown/false condition can exit.
            const std::size_t loop_scope_depth = owner_scopes_.size();
            loops_.push_back(LoopTargets{after, condition, loop_scope_depth, while_id});
            push_scope();
            FlowBlockId body_tail = body_entry;
            for (const auto& child : statement.statements) {
                if (!child) continue;
                if (body_tail == InvalidFlowBlockId) body_tail = add_block(child->span);
                body_tail = build_statement(*child, body_tail, true);
            }
            loops_.pop_back();
            if (body_tail != InvalidFlowBlockId) {
                emit_owner_drops_from(body_tail, owner_scopes_.size() - 1U);
                add_edge(body_tail, condition, FlowEdgeKind::LoopBack,
                    while_id, while_id, statement.span);
            }
            pop_scope();
            return after;
        }
        case StatementKind::ForEach: {
            const FlowControlStatementId for_id = control_statement_id(statement);
            if (for_id == InvalidFlowControlStatementId) return current;
            if (!statement.expressions.empty() && statement.expressions[0]) {
                read_expression(*statement.expressions[0], current);
            }
            const FlowBlockId condition = add_block();
            const FlowBlockId body_entry = add_block();
            const FlowBlockId after = add_block();
            add_edge(current, condition, FlowEdgeKind::ForEachEntry,
                for_id, for_id, statement.span);
            add_edge(condition, body_entry, FlowEdgeKind::ForEachBody,
                for_id, for_id, statement.span);
            add_edge(condition, after, FlowEdgeKind::ForEachExit,
                for_id, for_id, statement.span); // Iterable may be empty.
            push_scope(); // Loop binding scope.
            const FlowVariableId iterator = declare_variable(statement.name, statement.span);
            emit(body_entry, FlowOperationKind::Declare, iterator, statement.name,
                 statement.span, true);
            const std::size_t loop_scope_depth = owner_scopes_.size();
            loops_.push_back(LoopTargets{after, condition, loop_scope_depth, for_id});
            push_scope(); // Per-iteration body scope.
            FlowBlockId body_tail = body_entry;
            for (const auto& child : statement.statements) {
                if (!child) continue;
                if (body_tail == InvalidFlowBlockId) body_tail = add_block(child->span);
                body_tail = build_statement(*child, body_tail, true);
            }
            loops_.pop_back();
            if (body_tail != InvalidFlowBlockId) {
                emit_owner_drops_from(body_tail, owner_scopes_.size() - 1U);
                add_edge(body_tail, condition, FlowEdgeKind::ForEachBack,
                    for_id, for_id, statement.span);
            }
            pop_scope();
            pop_scope();
            return after;
        }
        case StatementKind::Break:
            if (!loops_.empty()) {
                emit_owner_drops_from(current, loops_.back().owner_scope_depth);
                add_edge(current, loops_.back().break_target, FlowEdgeKind::LoopBreak,
                    control_statement_id(statement), loops_.back().control_construct_id, statement.span);
            }
            return InvalidFlowBlockId;
        case StatementKind::Continue:
            if (!loops_.empty()) {
                emit_owner_drops_from(current, loops_.back().owner_scope_depth);
                add_edge(current, loops_.back().continue_target, FlowEdgeKind::LoopContinue,
                    control_statement_id(statement), loops_.back().control_construct_id, statement.span);
            }
            return InvalidFlowBlockId;
        }
        return current;
    }
};

std::vector<bool> transfer_block(const BasicBlock& block, std::vector<bool> state) {
    for (const auto& operation : block.operations) {
        if (operation.variable_id >= state.size()) continue;
        if (operation.kind == FlowOperationKind::Declare) {
            state[operation.variable_id] = operation.initialized;
        } else if (operation.kind == FlowOperationKind::Write) {
            state[operation.variable_id] = true;
        }
    }
    return state;
}

std::vector<bool> intersect_predecessors(const ControlFlowGraph& graph,
                                        FlowBlockId block_id,
                                        const std::vector<std::vector<bool>>& out,
                                        const std::vector<bool>& reachable,
                                        std::size_t variable_count) {
    std::vector<bool> result(variable_count, true);
    bool saw_predecessor = false;
    for (const FlowBlockId predecessor : graph.blocks[block_id].predecessors) {
        if (predecessor >= reachable.size() || !reachable[predecessor]) continue;
        if (!saw_predecessor) {
            result = out[predecessor];
            saw_predecessor = true;
        } else {
            for (std::size_t index = 0; index < variable_count; ++index) {
                result[index] = result[index] && out[predecessor][index];
            }
        }
    }
    if (!saw_predecessor) std::fill(result.begin(), result.end(), false);
    return result;
}

FlowOwnershipState join_ownership_state(FlowOwnershipState left, FlowOwnershipState right) {
    if (left == right) return left;
    if (left == FlowOwnershipState::Unknown || right == FlowOwnershipState::Unknown) {
        return FlowOwnershipState::Unknown;
    }
    if (left == FlowOwnershipState::Borrowed || right == FlowOwnershipState::Borrowed) {
        return FlowOwnershipState::Unknown;
    }
    const auto is_dropped = [](FlowOwnershipState state) {
        return state == FlowOwnershipState::Dropped || state == FlowOwnershipState::MaybeDropped;
    };
    if (is_dropped(left) || is_dropped(right)) return FlowOwnershipState::MaybeDropped;
    const auto may_be_live = [](FlowOwnershipState state) {
        return state == FlowOwnershipState::Live || state == FlowOwnershipState::MaybeLive;
    };
    if (may_be_live(left) || may_be_live(right)) return FlowOwnershipState::MaybeLive;
    return FlowOwnershipState::MaybeEmpty;
}

std::vector<FlowOwnershipState> initial_ownership_state(const ControlFlowGraph& graph) {
    std::vector<FlowOwnershipState> state(
        graph.variables.size(), FlowOwnershipState::Uninitialized);
    for (const auto& variable : graph.variables) {
        if (!variable.is_unique_owner || !variable.is_parameter) continue;
        const std::string mode = fold_name(variable.passing_mode);
        if (mode == "take") state[variable.id] = FlowOwnershipState::Live;
        else if (mode == "borrow" || mode == "borrow mut") {
            state[variable.id] = FlowOwnershipState::Borrowed;
        } else {
            state[variable.id] = FlowOwnershipState::Unknown;
        }
    }
    return state;
}

void apply_ownership_operation(const FlowOperation& operation,
                               const ControlFlowGraph& graph,
                               std::vector<FlowOwnershipState>& state) {
    if (operation.variable_id >= graph.variables.size() ||
        operation.variable_id >= state.size()) return;
    const FlowVariable& variable = graph.variables[operation.variable_id];
    if (!variable.is_unique_owner) return;

    switch (operation.kind) {
    case FlowOperationKind::OwnerCreate:
        state[operation.variable_id] = FlowOwnershipState::Live;
        break;
    case FlowOperationKind::OwnerTransfer: {
        const FlowOwnershipState source_state = state[operation.variable_id];
        FlowOwnershipState destination_state = FlowOwnershipState::Unknown;
        if (operation.related_variable_id != InvalidFlowVariableId &&
            operation.related_variable_id < state.size() &&
            graph.variables[operation.related_variable_id].is_unique_owner) {
            if (source_state == FlowOwnershipState::Live) {
                destination_state = FlowOwnershipState::Live;
            } else if (source_state == FlowOwnershipState::Empty) {
                destination_state = FlowOwnershipState::Empty;
            } else if (source_state == FlowOwnershipState::Uninitialized ||
                       source_state == FlowOwnershipState::MaybeEmpty ||
                       source_state == FlowOwnershipState::MaybeLive) {
                destination_state = FlowOwnershipState::MaybeLive;
            } else if (source_state == FlowOwnershipState::Borrowed) {
                destination_state = FlowOwnershipState::Unknown;
            }
            state[operation.related_variable_id] = destination_state;
        }
        // `owner = owner.Take()` restores the drained resource into the same
        // wrapper. Other transfers leave the source wrapper initialized-empty.
        if (operation.related_variable_id == operation.variable_id) {
            state[operation.variable_id] = destination_state;
        } else {
            state[operation.variable_id] = FlowOwnershipState::Empty;
        }
        break;
    }
    case FlowOperationKind::OwnerReset:
        state[operation.variable_id] = FlowOwnershipState::Empty;
        break;
    case FlowOperationKind::OwnerCopy:
        state[operation.variable_id] = FlowOwnershipState::Unknown;
        break;
    case FlowOperationKind::OwnerDrop: {
        const FlowOwnershipState before = state[operation.variable_id];
        if (before == FlowOwnershipState::MaybeLive ||
            before == FlowOwnershipState::MaybeEmpty ||
            before == FlowOwnershipState::Uninitialized) {
            state[operation.variable_id] = FlowOwnershipState::MaybeDropped;
        } else if (before == FlowOwnershipState::Live ||
                   before == FlowOwnershipState::Empty) {
            state[operation.variable_id] = FlowOwnershipState::Dropped;
        } else if (before != FlowOwnershipState::Borrowed) {
            state[operation.variable_id] = FlowOwnershipState::MaybeDropped;
        }
        break;
    }
    case FlowOperationKind::Read:
    case FlowOperationKind::Declare:
    case FlowOperationKind::Write:
    case FlowOperationKind::Return:
    case FlowOperationKind::Call:
        break;
    }
}

std::vector<FlowOwnershipState> transfer_ownership_block(
    const BasicBlock& block, const ControlFlowGraph& graph,
    std::vector<FlowOwnershipState> state) {
    for (const auto& operation : block.operations) {
        apply_ownership_operation(operation, graph, state);
    }
    return state;
}

void analyze_ownership_flow(ControlFlowGraph& graph) {
    const std::size_t block_count = graph.blocks.size();
    const std::size_t variable_count = graph.variables.size();
    graph.ownership_analysis_converged = true;
    graph.ownership_analysis_iterations = 0;
    if (block_count == 0U || graph.entry_block >= block_count) return;

    std::vector<bool> known(block_count, false);
    std::vector<std::vector<FlowOwnershipState>> in(
        block_count, std::vector<FlowOwnershipState>(variable_count, FlowOwnershipState::Uninitialized));
    std::vector<std::vector<FlowOwnershipState>> out = in;
    known[graph.entry_block] = true;
    in[graph.entry_block] = initial_ownership_state(graph);
    out[graph.entry_block] = transfer_ownership_block(
        graph.blocks[graph.entry_block], graph, in[graph.entry_block]);

    const std::size_t iteration_limit = (block_count + 1U) * (variable_count + 2U) * 8U;
    bool changed = true;
    while (changed && graph.ownership_analysis_iterations < iteration_limit) {
        changed = false;
        ++graph.ownership_analysis_iterations;
        for (FlowBlockId block = 0; block < block_count; ++block) {
            if (!graph.blocks[block].reachable || block == graph.entry_block) continue;
            std::vector<FlowOwnershipState> next_in(variable_count, FlowOwnershipState::Uninitialized);
            bool saw_predecessor = false;
            for (const FlowBlockId predecessor : graph.blocks[block].predecessors) {
                if (predecessor >= block_count || !graph.blocks[predecessor].reachable ||
                    !known[predecessor]) continue;
                if (!saw_predecessor) {
                    next_in = out[predecessor];
                    saw_predecessor = true;
                } else {
                    for (std::size_t variable = 0; variable < variable_count; ++variable) {
                        next_in[variable] = join_ownership_state(
                            next_in[variable], out[predecessor][variable]);
                    }
                }
            }
            if (!saw_predecessor) continue;
            const std::vector<FlowOwnershipState> next_out =
                transfer_ownership_block(graph.blocks[block], graph, next_in);
            if (!known[block] || next_in != in[block] || next_out != out[block]) {
                known[block] = true;
                in[block] = next_in;
                out[block] = next_out;
                changed = true;
            }
        }
    }
    if (changed) graph.ownership_analysis_converged = false;

    for (FlowBlockId block = 0; block < block_count; ++block) {
        graph.blocks[block].ownership_in = in[block];
        graph.blocks[block].ownership_out = out[block];
        if (!graph.blocks[block].reachable || !known[block]) continue;
        std::vector<FlowOwnershipState> state = in[block];
        for (auto& operation : graph.blocks[block].operations) {
            operation.ownership_before = operation.variable_id < state.size()
                ? state[operation.variable_id] : FlowOwnershipState::Unknown;
            apply_ownership_operation(operation, graph, state);
            operation.ownership_after = operation.variable_id < state.size()
                ? state[operation.variable_id] : FlowOwnershipState::Unknown;
        }
    }
}

void analyze_graph(ControlFlowGraph& graph, const std::vector<FlowVariableId>& parameter_ids,
                   std::vector<Diagnostic>& diagnostics) {
    const std::size_t block_count = graph.blocks.size();
    const std::size_t variable_count = graph.variables.size();
    if (block_count == 0U || graph.entry_block >= block_count) return;

    std::vector<bool> reachable(block_count, false);
    std::deque<FlowBlockId> queue;
    queue.push_back(graph.entry_block);
    reachable[graph.entry_block] = true;
    while (!queue.empty()) {
        const FlowBlockId current = queue.front();
        queue.pop_front();
        for (const FlowBlockId next : graph.blocks[current].successors) {
            if (next < block_count && !reachable[next]) {
                reachable[next] = true;
                queue.push_back(next);
            }
        }
    }

    graph.has_reachable_return = false;
    graph.has_reachable_fallthrough = false;
    graph.unreachable_blocks.clear();
    for (FlowBlockId block = 0; block < block_count; ++block) {
        graph.blocks[block].reachable = reachable[block];
        if (!reachable[block]) graph.unreachable_blocks.push_back(block);
    }
    if (graph.exit_block < block_count) {
        for (const FlowBlockId predecessor : graph.blocks[graph.exit_block].predecessors) {
            if (predecessor >= block_count || !reachable[predecessor]) continue;
            const auto& operations = graph.blocks[predecessor].operations;
            const bool returns_here = !operations.empty() &&
                operations.back().kind == FlowOperationKind::Return;
            if (returns_here) graph.has_reachable_return = true;
            else graph.has_reachable_fallthrough = true;
        }
    }

    std::vector<std::vector<bool>> in(block_count, std::vector<bool>(variable_count, true));
    std::vector<std::vector<bool>> out(block_count, std::vector<bool>(variable_count, true));
    std::fill(in[graph.entry_block].begin(), in[graph.entry_block].end(), false);
    for (const FlowVariableId id : parameter_ids) {
        if (id < variable_count) in[graph.entry_block][id] = true;
    }
    out[graph.entry_block] = transfer_block(graph.blocks[graph.entry_block], in[graph.entry_block]);

    bool changed = true;
    std::size_t iterations = 0;
    const std::size_t iteration_limit = (block_count + 1U) * (variable_count + 2U) * 4U;
    while (changed && iterations < iteration_limit) {
        changed = false;
        ++iterations;
        for (FlowBlockId block = 0; block < block_count; ++block) {
            if (!reachable[block] || block == graph.entry_block) continue;
            const std::vector<bool> next_in = intersect_predecessors(
                graph, block, out, reachable, variable_count);
            const std::vector<bool> next_out = transfer_block(graph.blocks[block], next_in);
            if (next_in != in[block] || next_out != out[block]) {
                in[block] = next_in;
                out[block] = next_out;
                changed = true;
            }
        }
    }

    for (FlowBlockId block = 0; block < block_count; ++block) {
        if (!reachable[block]) continue;
        std::vector<bool> state = in[block];
        for (const auto& operation : graph.blocks[block].operations) {
            if (operation.variable_id >= variable_count) continue;
            if (operation.kind == FlowOperationKind::Read && !state[operation.variable_id]) {
                diagnostics.push_back(Diagnostic{
                    "XFLOW0001",
                    "Variable '" + operation.name + "' may be read before it is initialized",
                    operation.span});
            } else if (operation.kind == FlowOperationKind::Declare) {
                state[operation.variable_id] = operation.initialized;
            } else if (operation.kind == FlowOperationKind::Write) {
                state[operation.variable_id] = true;
            }
        }
    }
    analyze_ownership_flow(graph);
}


} // namespace

std::vector<FlowControlStatementIdentity> enumerate_flow_control_statements(
    const Statement& root) {
    std::vector<FlowControlStatementIdentity> result;
    FlowControlStatementId next_id = 1U;
    const auto is_control = [](StatementKind kind) {
        return kind == StatementKind::If || kind == StatementKind::While ||
               kind == StatementKind::ForEach || kind == StatementKind::Break ||
               kind == StatementKind::Continue;
    };
    const auto is_construct = [](StatementKind kind) {
        return kind == StatementKind::If || kind == StatementKind::While ||
               kind == StatementKind::ForEach;
    };
    const auto visit = [&](const auto& self, const Statement& statement,
                           FlowControlConstructId parent_construct_id) -> void {
        FlowControlConstructId child_parent = parent_construct_id;
        if (is_control(statement.kind)) {
            if (next_id == InvalidFlowControlStatementId) return;
            const FlowControlStatementId id = next_id++;
            result.push_back(FlowControlStatementIdentity{
                &statement, id, parent_construct_id});
            if (is_construct(statement.kind)) child_parent = id;
        }
        for (const auto& child : statement.statements) {
            if (child) self(self, *child, child_parent);
        }
        if (statement.else_branch) self(self, *statement.else_branch, child_parent);
    };
    visit(visit, root, InvalidFlowControlConstructId);
    return result;
}

ControlFlowResult analyze_control_flow(const CompilationUnit& unit) {
    ControlFlowResult result;
    for (const auto& function : unit.functions) {
        if (!function.has_body || !function.body) continue;
        FunctionFlowBuilder builder(function, unit.imports, unit.module_name);
        ControlFlowGraph graph = builder.build();
        analyze_graph(graph, builder.parameter_ids(), result.diagnostics);
        if (function_requires_value_return(function) && graph.has_reachable_fallthrough) {
            result.diagnostics.push_back(Diagnostic{
                "XFLOW0002",
                "Function '" + function.name + "' may reach the end without returning a value",
                function.span});
        }
        result.functions.push_back(std::move(graph));
    }
    return result;
}

ControlFlowResult analyze_control_flow(const SourceManager& sources, SourceFileId source_id) {
    ParseResult parsed = parse_source(sources, source_id);
    ControlFlowResult result;
    if (!parsed.diagnostics.empty()) {
        result.diagnostics = std::move(parsed.diagnostics);
        return result;
    }
    return analyze_control_flow(parsed.unit);
}

} // namespace xfrontend
