#include "xfrontend/lifetime.hpp"
#include "xfrontend/source_manager.hpp"

#include <algorithm>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace xfrontend {
namespace {

std::string fold_name(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= 'A' && byte <= 'Z') ch = static_cast<char>(byte - 'A' + 'a');
    }
    return value;
}

std::string compact_type(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
    }), value.end());
    return fold_name(std::move(value));
}

bool is_unique_type(const std::string& type_name) {
    const std::string type = compact_type(type_name);
    return type.rfind("unique[", 0U) == 0U || type.rfind("uniquearray[", 0U) == 0U;
}

bool is_pointer_type(const std::string& type_name) {
    const std::string type = compact_type(type_name);
    return !type.empty() && type.front() == '*';
}

bool has_prefix(const std::string& value, const char* prefix) {
    const std::string p(prefix);
    return value.compare(0U, p.size(), p) == 0;
}

using RootSet = std::unordered_set<std::string>;

struct FlowState {
    // Values are abstract origins, not runtime values. Empty means the variable
    // is currently known not to carry a tracked borrowed pointer.
    std::unordered_map<std::string, RootSet> roots;
    // Each live Unique owner has one or more possible resource-generation IDs.
    std::unordered_map<std::string, RootSet> owner_tokens;
    // A token enters this set after Reset/Take/Release or owner-scope exit.
    RootSet invalid_owner_tokens;
};

void union_into(RootSet& destination, const RootSet& source) {
    destination.insert(source.begin(), source.end());
}

FlowState merge_states(const FlowState& left, const FlowState& right) {
    FlowState merged;
    for (const auto& item : left.roots) union_into(merged.roots[item.first], item.second);
    for (const auto& item : right.roots) union_into(merged.roots[item.first], item.second);
    for (const auto& item : left.owner_tokens) union_into(merged.owner_tokens[item.first], item.second);
    for (const auto& item : right.owner_tokens) union_into(merged.owner_tokens[item.first], item.second);
    merged.invalid_owner_tokens = left.invalid_owner_tokens;
    union_into(merged.invalid_owner_tokens, right.invalid_owner_tokens);
    return merged;
}

const Expression* member_receiver(const Expression& callee) {
    if (callee.kind != ExprKind::Member || callee.children.empty() || !callee.children[0]) return nullptr;
    return callee.children[0].get();
}

bool is_method_call(const Expression& expression, const char* method,
                    const Expression** receiver_out = nullptr) {
    if (expression.kind != ExprKind::Call || expression.children.empty() || !expression.children[0]) return false;
    const Expression& callee = *expression.children[0];
    if (callee.kind != ExprKind::Member || fold_name(callee.text) != fold_name(method)) return false;
    const Expression* receiver = member_receiver(callee);
    if (receiver_out) *receiver_out = receiver;
    return receiver != nullptr;
}

bool is_identifier(const Expression* expression, std::string* name = nullptr) {
    if (expression == nullptr || expression->kind != ExprKind::Identifier) return false;
    if (name) *name = fold_name(expression->text);
    return true;
}

class FunctionLifetimeAnalyzer {
public:
    FunctionLifetimeAnalyzer(const CompilationUnit& unit,
                             const FunctionDeclaration& function,
                             LifetimeResult& result)
        : unit_(unit), function_(function), result_(result) {
        collect_callables();
        if (function_.body) collect_last_uses_statement(*function_.body);
        initialize_parameters();
    }

    void run() {
        if (!function_.has_body || !function_.body) return;
        analyze_statement(*function_.body, state_);
    }

private:
    struct Callable {
        const FunctionDeclaration* function = nullptr;
        std::string signature_key;
        std::size_t required_count = 0U;
    };

    const CompilationUnit& unit_;
    const FunctionDeclaration& function_;
    LifetimeResult& result_;
    FlowState state_;
    std::unordered_set<std::string> owner_names_;
    std::unordered_map<std::string, std::vector<Callable>> callables_;
    std::unordered_map<std::string, std::size_t> last_use_;
    std::unordered_set<std::string> emitted_;

    void emit(const char* code, const char* message, const SourceSpan& span) {
        std::ostringstream key;
        key << code << '|' << span.source_id << '|' << span.start.byte_offset;
        if (!emitted_.insert(key.str()).second) return;
        result_.diagnostics.push_back(Diagnostic{code, message, span});
    }

    void initialize_parameters() {
        for (const auto& parameter : function_.parameters) {
            const std::string name = fold_name(parameter.name);
            const std::string mode = fold_name(parameter.passing_mode);
            if (mode == "borrow") {
                state_.roots[name].insert("borrow:" + name);
            } else if (mode == "borrow mut") {
                state_.roots[name].insert("borrow-mut:" + name);
            } else if (is_pointer_type(parameter.type_name)) {
                // Raw pointers carry no automatic escape contract, but two raw
                // pointer parameters may still alias when passed to a borrowed API.
                state_.roots[name].insert("param:" + name);
            }
            if (is_unique_type(parameter.type_name)) {
                owner_names_.insert(name);
                if (mode == "copy") {
                    emit("XLIFE0008",
                         "Unique values are move-only and cannot be declared with copy passing mode",
                         parameter.span);
                }
                if (mode == "take") {
                    state_.owner_tokens[name].insert(owner_token(name, parameter.span.start.byte_offset));
                }
            }
        }
    }

    static std::string owner_token(const std::string& name, std::size_t offset) {
        return "owner:" + name + "@" + std::to_string(offset);
    }

    void record_last_use(const Expression& expression) {
        const std::string name = fold_name(expression.text);
        const auto found = last_use_.find(name);
        if (found == last_use_.end() || expression.span.start.byte_offset > found->second) {
            last_use_[name] = expression.span.start.byte_offset;
        }
    }

    void collect_last_uses_expression(const Expression& expression) {
        if (expression.kind == ExprKind::Identifier) {
            record_last_use(expression);
            return;
        }
        if (expression.kind == ExprKind::Call) {
            if (!expression.children.empty() && expression.children[0]) {
                const Expression& callee = *expression.children[0];
                if (callee.kind == ExprKind::Member && !callee.children.empty() && callee.children[0]) {
                    collect_last_uses_expression(*callee.children[0]);
                }
            }
            for (std::size_t i = 1; i < expression.children.size(); ++i) {
                if (expression.children[i]) collect_last_uses_expression(*expression.children[i]);
            }
            return;
        }
        for (const auto& child : expression.children) {
            if (child) collect_last_uses_expression(*child);
        }
    }

    void collect_last_uses_statement(const Statement& statement) {
        if (statement.kind == StatementKind::Assignment && statement.expressions.size() >= 2U &&
            statement.expressions[0] && statement.expressions[0]->kind == ExprKind::Identifier) {
            collect_last_uses_expression(*statement.expressions[1]);
        } else {
            for (const auto& expression : statement.expressions) {
                if (expression) collect_last_uses_expression(*expression);
            }
        }
        for (const auto& child : statement.statements) {
            if (child) collect_last_uses_statement(*child);
        }
        if (statement.else_branch) collect_last_uses_statement(*statement.else_branch);
    }

    static void collect_identifier_names(const Expression& expression,
                                         std::unordered_set<std::string>& names) {
        if (expression.kind == ExprKind::Identifier) {
            names.insert(fold_name(expression.text));
            return;
        }
        if (expression.kind == ExprKind::Call) {
            if (!expression.children.empty() && expression.children[0]) {
                const Expression& callee = *expression.children[0];
                if (callee.kind == ExprKind::Member && !callee.children.empty() && callee.children[0]) {
                    collect_identifier_names(*callee.children[0], names);
                }
            }
            for (std::size_t i = 1; i < expression.children.size(); ++i) {
                if (expression.children[i]) collect_identifier_names(*expression.children[i], names);
            }
            return;
        }
        for (const auto& child : expression.children) {
            if (child) collect_identifier_names(*child, names);
        }
    }

    static std::string callable_signature_key(const FunctionDeclaration& function) {
        std::ostringstream out;
        out << fold_name(function.name) << '(';
        for (std::size_t i = 0; i < function.parameters.size(); ++i) {
            if (i) out << ',';
            out << compact_type(function.parameters[i].type_name);
        }
        out << ')';
        return out.str();
    }

    void collect_callables() {
        std::unordered_map<std::string, std::size_t> by_signature;
        for (const auto& function : unit_.functions) {
            const std::string name = fold_name(function.name);
            const std::string signature = callable_signature_key(function);
            const auto found = by_signature.find(signature);
            std::size_t required = function.parameters.size();
            while (required > 0U && function.parameters[required - 1U].default_value) --required;
            Callable candidate{&function, signature, required};
            if (found == by_signature.end()) {
                by_signature.emplace(signature, callables_[name].size());
                callables_[name].push_back(candidate);
            } else {
                auto& existing = callables_[name][found->second];
                if (!existing.function->has_body && function.has_body) existing = candidate;
            }
        }
    }

    RootSet roots_of(const Expression& expression, const FlowState& state) const {
        switch (expression.kind) {
        case ExprKind::Identifier: {
            const std::string name = fold_name(expression.text);
            const auto found = state.roots.find(name);
            return found == state.roots.end() ? RootSet{} : found->second;
        }
        case ExprKind::Literal:
            return {};
        case ExprKind::Unary:
            if (expression.children.empty() || !expression.children[0]) return {};
            if (expression.text == "*") return {}; // pointee value, not pointer provenance
            if (expression.text == "&") {
                const Expression& operand = *expression.children[0];
                if (operand.kind == ExprKind::Unary && operand.text == "*" &&
                    !operand.children.empty() && operand.children[0]) {
                    return roots_of(*operand.children[0], state);
                }
                return {}; // address of a local value is outside this borrowed-pointer subset
            }
            return roots_of(*expression.children[0], state);
        case ExprKind::Binary:
        case ExprKind::List: {
            RootSet roots;
            for (const auto& child : expression.children) {
                if (child) union_into(roots, roots_of(*child, state));
            }
            return roots;
        }
        case ExprKind::Call: {
            const Expression* receiver = nullptr;
            if (is_method_call(expression, "Get", &receiver) && receiver != nullptr) {
                std::string owner_name;
                if (is_identifier(receiver, &owner_name)) {
                    const auto owner = state.owner_tokens.find(owner_name);
                    if (owner != state.owner_tokens.end()) return owner->second;
                }
            }
            return {};
        }
        case ExprKind::Member:
        case ExprKind::Index:
            return {}; // field/index provenance requires typed projection metadata
        }
        return {};
    }

    static bool contains_borrow_origin(const RootSet& roots) {
        for (const auto& root : roots) {
            if (has_prefix(root, "borrow:") || has_prefix(root, "borrow-mut:") ||
                has_prefix(root, "owner:")) return true;
        }
        return false;
    }

    static bool roots_may_alias(const RootSet& left, const RootSet& right) {
        for (const auto& root : left) {
            if (right.find(root) != right.end()) return true;
        }
        // Distinct formal pointer/borrow parameters can be aliases supplied by
        // the caller. Without a no-alias contract, treating them as disjoint is unsound.
        bool left_formal = false;
        bool right_formal = false;
        for (const auto& root : left) {
            if (has_prefix(root, "borrow:") || has_prefix(root, "borrow-mut:") ||
                has_prefix(root, "param:")) left_formal = true;
        }
        for (const auto& root : right) {
            if (has_prefix(root, "borrow:") || has_prefix(root, "borrow-mut:") ||
                has_prefix(root, "param:")) right_formal = true;
        }
        return left_formal && right_formal;
    }

    void check_stale_alias(const Expression& expression, const FlowState& state) {
        if (expression.kind != ExprKind::Identifier) return;
        const std::string name = fold_name(expression.text);
        const auto found = state.roots.find(name);
        if (found == state.roots.end()) return;
        for (const auto& root : found->second) {
            if (has_prefix(root, "owner:") &&
                state.invalid_owner_tokens.find(root) != state.invalid_owner_tokens.end()) {
                emit("XLIFE0005", "A borrowed value from Unique.Get() is used after its owner was reset, moved, released, or left scope",
                     expression.span);
                return;
            }
        }
    }

    bool is_owner_variable(const std::string& name) const {
        return owner_names_.find(name) != owner_names_.end();
    }

    void invalidate_owner(const std::string& name, FlowState& state) {
        const auto found = state.owner_tokens.find(name);
        if (found != state.owner_tokens.end()) {
            union_into(state.invalid_owner_tokens, found->second);
            state.owner_tokens.erase(found);
        }
    }

    void initialize_owner(const std::string& name, std::size_t offset, FlowState& state) {
        invalidate_owner(name, state);
        state.owner_tokens[name].insert(owner_token(name, offset));
    }

    void check_direct_call(const Expression& expression, FlowState& state) {
        if (expression.children.empty() || !expression.children[0]) return;
        const Expression& callee = *expression.children[0];
        if (callee.kind != ExprKind::Identifier) return;
        const auto found = callables_.find(fold_name(callee.text));
        if (found == callables_.end()) return;

        const std::size_t argument_count = expression.children.size() - 1U;
        std::vector<const Callable*> viable;
        for (const auto& candidate : found->second) {
            const std::size_t parameter_count = candidate.function->parameters.size();
            if (argument_count >= candidate.required_count && argument_count <= parameter_count) {
                viable.push_back(&candidate);
            }
        }
        if (viable.empty()) return;

        std::set<std::pair<std::size_t, std::size_t>> common_conflicts;
        bool first_candidate = true;
        std::unordered_set<std::string> passed_names;
        for (std::size_t i = 0; i < argument_count; ++i) {
            collect_identifier_names(*expression.children[i + 1U], passed_names);
        }
        const std::pair<std::size_t, std::size_t> live_conflict_marker{
            argument_count + 1U, argument_count + 1U};
        for (const Callable* candidate : viable) {
            std::set<std::pair<std::size_t, std::size_t>> candidate_conflicts;
            for (std::size_t i = 0; i < argument_count; ++i) {
                const std::string left_mode = fold_name(candidate->function->parameters[i].passing_mode);
                if (left_mode != "borrow mut") continue;
                const RootSet left_roots = roots_of(*expression.children[i + 1U], state);
                if (left_roots.empty()) continue;

                // An immutable Borrow cannot be upgraded to an exclusive Borrow.
                for (const auto& root : left_roots) {
                    if (has_prefix(root, "borrow:")) {
                        candidate_conflicts.insert(live_conflict_marker);
                        break;
                    }
                }

                // Check other borrowed aliases that remain live after this call.
                // The exact source argument itself is excluded: the reborrow ends
                // with the call, after which the source may be used again.
                for (const auto& live : state.roots) {
                    if (passed_names.find(live.first) != passed_names.end()) continue;
                    const auto last = last_use_.find(live.first);
                    if (last == last_use_.end() || last->second <= expression.span.start.byte_offset) continue;
                    bool is_tracked_borrow = false;
                    for (const auto& root : live.second) {
                        if (has_prefix(root, "borrow:") || has_prefix(root, "borrow-mut:") ||
                            has_prefix(root, "owner:")) {
                            is_tracked_borrow = true;
                            break;
                        }
                    }
                    if (is_tracked_borrow && roots_may_alias(left_roots, live.second)) {
                        candidate_conflicts.insert(live_conflict_marker);
                        break;
                    }
                }

                for (std::size_t j = 0; j < argument_count; ++j) {
                    if (i == j) continue;
                    const std::string right_mode = fold_name(candidate->function->parameters[j].passing_mode);
                    if (right_mode != "borrow" && right_mode != "borrow mut") continue;
                    const RootSet right_roots = roots_of(*expression.children[j + 1U], state);
                    if (roots_may_alias(left_roots, right_roots)) {
                        candidate_conflicts.insert(std::minmax(i, j));
                    }
                }
            }
            if (first_candidate) {
                common_conflicts = std::move(candidate_conflicts);
                first_candidate = false;
            } else {
                std::set<std::pair<std::size_t, std::size_t>> intersection;
                std::set_intersection(common_conflicts.begin(), common_conflicts.end(),
                                      candidate_conflicts.begin(), candidate_conflicts.end(),
                                      std::inserter(intersection, intersection.begin()));
                common_conflicts = std::move(intersection);
            }
        }
        if (!common_conflicts.empty()) {
            emit("XLIFE0003", "The same or potentially aliasing value is passed to overlapping borrow mut and borrow parameters",
                 expression.span);
        }

        // Enforce the explicit transfer syntax only for statically known Unique owners.
        if (viable.size() == 1U) {
            const FunctionDeclaration& target = *viable.front()->function;
            for (std::size_t i = 0; i < argument_count; ++i) {
                const Parameter& parameter = target.parameters[i];
                if (fold_name(parameter.passing_mode) != "take" || !is_unique_type(parameter.type_name)) continue;
                const Expression& argument = *expression.children[i + 1U];
                std::string owner_name;
                if (is_identifier(&argument, &owner_name) && is_owner_variable(owner_name)) {
                    emit("XLIFE0006", "A named Unique owner must be transferred explicitly with .Take() to a take parameter",
                         argument.span);
                }
            }
        }
    }

    void analyze_expression(const Expression& expression, FlowState& state) {
        if (expression.kind == ExprKind::Identifier) {
            check_stale_alias(expression, state);
            return;
        }
        if (expression.kind == ExprKind::Call) {
            if (expression.children.empty() || !expression.children[0]) return;
            const Expression& callee = *expression.children[0];
            if (callee.kind == ExprKind::Member && !callee.children.empty() && callee.children[0]) {
                analyze_expression(*callee.children[0], state);
            }
            for (std::size_t i = 1; i < expression.children.size(); ++i) {
                if (expression.children[i]) analyze_expression(*expression.children[i], state);
            }
            check_direct_call(expression, state);

            const Expression* receiver = nullptr;
            const bool mutating_owner_op =
                is_method_call(expression, "Reset", &receiver) ||
                is_method_call(expression, "Take", &receiver) ||
                is_method_call(expression, "Release", &receiver);
            if (mutating_owner_op && receiver != nullptr) {
                std::string owner_name;
                if (is_identifier(receiver, &owner_name) && is_owner_variable(owner_name)) {
                    invalidate_owner(owner_name, state);
                }
            }
            return;
        }
        for (const auto& child : expression.children) {
            if (child) analyze_expression(*child, state);
        }
    }

    bool analyze_statement(const Statement& statement, FlowState& state) {
        switch (statement.kind) {
        case StatementKind::Block: {
            std::vector<std::string> local_owners;
            bool falls_through = true;
            for (const auto& child : statement.statements) {
                if (!child || !falls_through) continue;
                if (child->kind == StatementKind::VariableDeclaration &&
                    is_unique_type(child->type_name)) {
                    const std::string name = fold_name(child->name);
                    owner_names_.insert(name);
                    local_owners.push_back(name);
                }
                falls_through = analyze_statement(*child, state);
            }
            for (const auto& owner : local_owners) invalidate_owner(owner, state);
            return falls_through;
        }
        case StatementKind::VariableDeclaration: {
            if (!statement.expressions.empty() && statement.expressions[0]) {
                analyze_expression(*statement.expressions[0], state);
            }
            const std::string name = fold_name(statement.name);
            if (is_unique_type(statement.type_name)) {
                owner_names_.insert(name);
                state.roots[name].clear();
                if (!statement.expressions.empty() && statement.expressions[0]) {
                    const Expression& initializer = *statement.expressions[0];
                    std::string source_owner;
                    if (is_identifier(&initializer, &source_owner) &&
                        is_owner_variable(source_owner) && source_owner != name) {
                        emit("XLIFE0007",
                             "Unique values are move-only; use .Take() to transfer a named owner",
                             initializer.span);
                    }
                    initialize_owner(name, statement.span.start.byte_offset, state);
                } else {
                    invalidate_owner(name, state);
                }
            } else if (!statement.expressions.empty() && statement.expressions[0]) {
                state.roots[name] = roots_of(*statement.expressions[0], state);
            } else {
                state.roots[name].clear();
            }
            return true;
        }
        case StatementKind::Assignment: {
            if (statement.expressions.size() < 2U || !statement.expressions[0] || !statement.expressions[1]) return true;
            const Expression& target = *statement.expressions[0];
            const Expression& value = *statement.expressions[1];
            if (target.kind != ExprKind::Identifier) analyze_expression(target, state);
            analyze_expression(value, state);
            const RootSet value_roots = roots_of(value, state);
            if ((target.kind == ExprKind::Member || target.kind == ExprKind::Index) &&
                contains_borrow_origin(value_roots)) {
                emit("XLIFE0002", "A borrowed value cannot be stored through a member or index target",
                     value.span);
            }
            if (target.kind == ExprKind::Identifier) {
                const std::string name = fold_name(target.text);
                if (is_owner_variable(name)) {
                    std::string source_owner;
                    if (is_identifier(&value, &source_owner) &&
                        is_owner_variable(source_owner) && source_owner != name) {
                        emit("XLIFE0007",
                             "Unique values are move-only; use .Take() to transfer a named owner",
                             value.span);
                    }
                    initialize_owner(name, statement.span.start.byte_offset, state);
                    state.roots[name].clear();
                } else {
                    state.roots[name] = value_roots;
                }
            }
            return true;
        }
        case StatementKind::Expression:
            if (!statement.expressions.empty() && statement.expressions[0]) {
                analyze_expression(*statement.expressions[0], state);
            }
            return true;
        case StatementKind::Return:
            if (!statement.expressions.empty() && statement.expressions[0]) {
                analyze_expression(*statement.expressions[0], state);
                if (contains_borrow_origin(roots_of(*statement.expressions[0], state))) {
                    emit("XLIFE0001", "A borrowed value cannot be returned without an explicit lifetime-bound return contract",
                         statement.expressions[0]->span);
                }
            }
            return false;
        case StatementKind::If: {
            if (!statement.expressions.empty() && statement.expressions[0]) {
                analyze_expression(*statement.expressions[0], state);
            }
            FlowState then_state = state;
            FlowState else_state = state;
            const bool then_falls = !statement.statements.empty() && statement.statements[0]
                ? analyze_statement(*statement.statements[0], then_state) : true;
            const bool else_falls = statement.else_branch
                ? analyze_statement(*statement.else_branch, else_state) : true;
            if (then_falls && else_falls) state = merge_states(then_state, else_state);
            else if (then_falls) state = std::move(then_state);
            else if (else_falls) state = std::move(else_state);
            return then_falls || else_falls;
        }
        case StatementKind::While:
        case StatementKind::ForEach: {
            if (!statement.expressions.empty() && statement.expressions[0]) {
                analyze_expression(*statement.expressions[0], state);
            }
            FlowState body_state = state;
            for (const auto& child : statement.statements) {
                if (child) analyze_statement(*child, body_state);
            }
            // A loop may execute zero times. Union with the body result captures
            // aliases and invalidations that may become observable after an iteration.
            state = merge_states(state, body_state);
            return true;
        }
        case StatementKind::Break:
        case StatementKind::Continue:
            return false;
        }
        return true;
    }
};

} // namespace

LifetimeResult analyze_lifetimes(const CompilationUnit& unit) {
    LifetimeResult result;
    for (const auto& function : unit.functions) {
        FunctionLifetimeAnalyzer analyzer(unit, function, result);
        analyzer.run();
    }
    return result;
}

LifetimeResult analyze_lifetimes(const SourceManager& sources, SourceFileId source_id) {
    ParseResult parsed = parse_source(sources, source_id);
    LifetimeResult result;
    if (!parsed.diagnostics.empty()) {
        result.diagnostics = std::move(parsed.diagnostics);
        return result;
    }
    return analyze_lifetimes(parsed.unit);
}

} // namespace xfrontend
