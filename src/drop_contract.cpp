#include "xfrontend/drop_contract.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <utility>

namespace xfrontend {
namespace {

std::string fold_identifier(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z')) {
            ch = static_cast<char>(byte - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
        }
    }
    return value;
}

void add_diagnostic(std::vector<Diagnostic>& diagnostics, const char* code,
                    const std::string& message, const SourceSpan& span = {}) {
    diagnostics.push_back(Diagnostic{code, message, span});
}

bool blank(const std::string& value) {
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
}

std::string resource_payload_type(const std::string& owner_type) {
    const std::size_t open = owner_type.find('[');
    if (open == std::string::npos || owner_type.empty() || owner_type.back() != ']') return {};
    std::string wrapper = owner_type.substr(0U, open);
    wrapper.erase(std::remove_if(wrapper.begin(), wrapper.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), wrapper.end());
    wrapper = fold_identifier(wrapper);
    if (wrapper != "unique" && wrapper != "uniquearray") return {};

    int depth = 0;
    std::size_t matching_close = std::string::npos;
    for (std::size_t index = open; index < owner_type.size(); ++index) {
        if (owner_type[index] == '[') ++depth;
        else if (owner_type[index] == ']') {
            --depth;
            if (depth == 0) {
                matching_close = index;
                break;
            }
            if (depth < 0) return {};
        }
    }
    if (matching_close != owner_type.size() - 1U || depth != 0) return {};
    std::string payload = owner_type.substr(open + 1U, matching_close - open - 1U);
    const auto first = std::find_if_not(payload.begin(), payload.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto last = std::find_if_not(payload.rbegin(), payload.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (first >= last) return {};
    return std::string(first, last);
}

struct IndexedContract {
    const DropRoutineContract* contract = nullptr;
    bool valid = false;
};

} // namespace

DropContractResolutionResult resolve_ownership_drop_contracts(
    const OwnershipLoweringResult& lowered,
    const std::vector<DropRoutineContract>& contracts,
    DropResolutionOptions options) {
    DropContractResolutionResult result;
    if (!lowered.diagnostics.empty()) {
        result.diagnostics = lowered.diagnostics;
        return result;
    }

    std::unordered_map<std::string, IndexedContract> by_type;
    by_type.reserve(contracts.size());

    for (const auto& contract : contracts) {
        const std::string key = fold_identifier(contract.resource_type);
        if (key.empty() || blank(key)) {
            add_diagnostic(result.diagnostics, "XDROP0001",
                           "Drop contract resource type must not be empty");
            continue;
        }
        if (by_type.find(key) != by_type.end()) {
            add_diagnostic(result.diagnostics, "XDROP0002",
                           "Duplicate drop contract for resource type '" + contract.resource_type + "'");
            continue;
        }

        bool valid = true;
        switch (contract.kind) {
        case DropRoutineKind::Trivial:
            if (!blank(contract.routine_name)) {
                add_diagnostic(result.diagnostics, "XDROP0003",
                               "Trivial cleanup contract must not name a callable", {});
                valid = false;
            }
            break;
        case DropRoutineKind::Destructor:
        case DropRoutineKind::CustomDeleter:
            if (blank(contract.routine_name)) {
                add_diagnostic(result.diagnostics, "XDROP0004",
                               "Non-trivial cleanup contract requires a routine name");
                valid = false;
            }
            if (!contract.resolved) {
                add_diagnostic(result.diagnostics, "XDROP0005",
                               "Drop routine is unresolved: '" + contract.routine_name + "'");
                valid = false;
            }
            if (!contract.accessible) {
                add_diagnostic(result.diagnostics, "XDROP0006",
                               "Drop routine is not accessible: '" + contract.routine_name + "'");
                valid = false;
            }
            if (!contract.signature_valid) {
                add_diagnostic(result.diagnostics, "XDROP0007",
                               "Drop routine signature does not satisfy the cleanup contract: '" +
                                   contract.routine_name + "'");
                valid = false;
            }
            if (!contract.receiver_compatible) {
                add_diagnostic(result.diagnostics, "XDROP0008",
                               "Drop routine receiver is incompatible with resource type '" +
                                   contract.resource_type + "'");
                valid = false;
            }
            break;
        }
        by_type.emplace(key, IndexedContract{&contract, valid});
    }

    for (const auto& function : lowered.functions) {
        DropInvocationFunction resolved_function;
        resolved_function.function_name = function.function_name;
        resolved_function.variables = function.variables;
        resolved_function.blocks.reserve(function.blocks.size());

        for (const auto& block : function.blocks) {
            DropInvocationBlock resolved_block;
            resolved_block.id = block.id;
            resolved_block.reachable = block.reachable;
            if (!block.reachable) {
                resolved_function.blocks.push_back(std::move(resolved_block));
                continue;
            }

            for (std::size_t action_index = 0; action_index < block.actions.size(); ++action_index) {
                const auto& action = block.actions[action_index];
                if (action.kind != OwnershipActionKind::DestroyResource) continue;
                if (action.variable_id >= function.variables.size() ||
                    !function.variables[action.variable_id].is_unique_owner) {
                    add_diagnostic(result.diagnostics, "XDROP0009",
                                   "Destroy action references an invalid resource owner", action.span);
                    continue;
                }

                const auto& variable = function.variables[action.variable_id];
                const std::string payload_type = resource_payload_type(variable.type_name);
                if (payload_type.empty()) {
                    add_diagnostic(result.diagnostics, "XDROP0009",
                                   "Unique owner type does not contain a valid resource payload type",
                                   action.span);
                    continue;
                }
                const std::string key = fold_identifier(payload_type);
                const auto found = by_type.find(key);
                if (found == by_type.end()) {
                    add_diagnostic(result.diagnostics, "XDROP0010",
                                   "No drop contract registered for resource type '" + payload_type + "'",
                                   action.span);
                    continue;
                }
                if (!found->second.valid || found->second.contract == nullptr) continue;

                const DropRoutineContract& contract = *found->second.contract;
                if (contract.failure_mode == DropFailureMode::MayFail &&
                    options.may_fail_policy == DropFailurePolicy::RejectMayFail) {
                    add_diagnostic(result.diagnostics, "XDROP0011",
                                   "MayFail cleanup routine requires an explicit failure policy: '" +
                                       contract.routine_name + "'", action.span);
                    continue;
                }

                DropInvocation invocation;
                invocation.owner_type = variable.type_name;
                invocation.resource_type = payload_type;
                invocation.routine_name = contract.routine_name;
                invocation.guard = action.guard;
                invocation.failure_mode = contract.failure_mode;
                invocation.terminate_if_failed = contract.failure_mode == DropFailureMode::MayFail &&
                    options.may_fail_policy == DropFailurePolicy::TerminateOnFailure;
                invocation.block_id = block.id;
                invocation.action_index = action_index;
                invocation.variable_id = action.variable_id;
                invocation.span = action.span;
                switch (contract.kind) {
                case DropRoutineKind::Trivial:
                    invocation.kind = DropInvocationKind::NoOp;
                    break;
                case DropRoutineKind::Destructor:
                    invocation.kind = DropInvocationKind::DestructorCall;
                    break;
                case DropRoutineKind::CustomDeleter:
                    invocation.kind = DropInvocationKind::CustomDeleterCall;
                    break;
                }
                resolved_block.invocations.push_back(std::move(invocation));
            }
            resolved_function.blocks.push_back(std::move(resolved_block));
        }
        result.functions.push_back(std::move(resolved_function));
    }

    if (!result.diagnostics.empty()) result.functions.clear();
    return result;
}

} // namespace xfrontend
