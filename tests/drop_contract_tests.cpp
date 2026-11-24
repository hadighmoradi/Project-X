// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/control_flow.hpp"
#include "xfrontend/drop_contract.hpp"
#include "xfrontend/ownership_lowering.hpp"
#include "xfrontend/parser.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using xfrontend::ControlFlowResult;
using xfrontend::DropContractResolutionResult;
using xfrontend::DropFailureMode;
using xfrontend::DropFailurePolicy;
using xfrontend::DropInvocationKind;
using xfrontend::DropRoutineContract;
using xfrontend::DropRoutineKind;
using xfrontend::OwnershipActionKind;
using xfrontend::OwnershipLoweringResult;

void drop_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string lower_ascii(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= static_cast<unsigned char>('A') && byte <= static_cast<unsigned char>('Z'))
            ch = static_cast<char>(byte - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
    }
    return value;
}

std::string upper_ascii(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= static_cast<unsigned char>('a') && byte <= static_cast<unsigned char>('z'))
            ch = static_cast<char>(byte - static_cast<unsigned char>('a') + static_cast<unsigned char>('A'));
    }
    return value;
}

std::string payload_type(const std::string& owner_type) {
    const auto open = owner_type.find('[');
    if (open == std::string::npos || owner_type.empty() || owner_type.back() != ']') return {};
    return owner_type.substr(open + 1U, owner_type.size() - open - 2U);
}

std::string drop_type(int index) {
    static const std::vector<std::string> values{
        "Unique[Device]", "UniqueArray[Device]", "Unique[Widget]", "UniqueArray[Widget]",
        "Unique[Buffer]", "UniqueArray[Buffer]", "Unique[Node]", "UniqueArray[Node]",
        "Unique[Resource]", "UniqueArray[Resource]"
    };
    return values[static_cast<std::size_t>(index % static_cast<int>(values.size()))];
}

std::string drop_name(int index) {
    static const std::vector<std::string> values{
        "owner", "Owner", "OWNER", "resource", "Resource", "unit", "Unit", "handle",
        "Handle", "payload"
    };
    return values[static_cast<std::size_t>(index % static_cast<int>(values.size()))];
}

void drop_case(const std::string& name, const std::function<void()>& body,
               int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    const auto start = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

ControlFlowResult drop_flow(const std::string& source) {
    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded())
        throw std::runtime_error("N18 parse failed: " + parsed.diagnostics.front().code + " " +
            parsed.diagnostics.front().message + " | " + source);
    auto flow = xfrontend::analyze_control_flow(parsed.unit);
    if (!flow.succeeded())
        throw std::runtime_error("N18 CFG failed: " + flow.diagnostics.front().code + " " +
            flow.diagnostics.front().message + " | " + source);
    return flow;
}

OwnershipLoweringResult drop_lower(const std::string& source) {
    auto flow = drop_flow(source);
    auto lowered = xfrontend::lower_ownership_cleanup(flow);
    if (!lowered.succeeded())
        throw std::runtime_error("N18 lowering failed: " + lowered.diagnostics.front().code + " " +
            lowered.diagnostics.front().message + " | " + source);
    return lowered;
}

std::vector<DropRoutineContract> contracts_for(const OwnershipLoweringResult& lowered,
                                                DropRoutineKind kind = DropRoutineKind::Destructor) {
    std::map<std::string, std::string> types;
    for (const auto& function : lowered.functions)
        for (const auto& variable : function.variables)
            if (variable.is_unique_owner) {
                const std::string payload = payload_type(variable.type_name);
                types.emplace(lower_ascii(payload), payload);
            }
    std::vector<DropRoutineContract> contracts;
    for (const auto& entry : types) {
        DropRoutineContract contract;
        contract.resource_type = entry.second;
        contract.kind = kind;
        if (kind == DropRoutineKind::Destructor) contract.routine_name = "Destroy_" + entry.second;
        if (kind == DropRoutineKind::CustomDeleter) contract.routine_name = "Release_" + entry.second;
        contracts.push_back(std::move(contract));
    }
    return contracts;
}

int invocation_count(const DropContractResolutionResult& result) {
    int count = 0;
    for (const auto& function : result.functions)
        for (const auto& block : function.blocks)
            count += static_cast<int>(block.invocations.size());
    return count;
}

int destroy_action_count(const OwnershipLoweringResult& lowered) {
    int count = 0;
    for (const auto& function : lowered.functions)
        for (const auto& block : function.blocks)
            for (const auto& action : block.actions)
                if (action.kind == OwnershipActionKind::DestroyResource) ++count;
    return count;
}

bool has_code(const DropContractResolutionResult& result, const std::string& code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.code == code;
    });
}

} // namespace

void run_drop_contract_tests(int& passed, int& failed) {
    // N18A (500): case-insensitive resource type identity and routine selection.
    int id = 1;
    for (int type_case = 0; type_case < 10; ++type_case) {
        for (int name_case = 0; name_case < 10; ++name_case) {
            for (int spelling = 0; spelling < 5; ++spelling) {
                const std::string type = drop_type(type_case);
                const std::string owner = drop_name(name_case);
                const std::string source = "function Probe(bool flag) { " + type + " " + owner +
                    " = " + type + ".Create(); PrintLine " + owner + ".Get(); }";
                const std::string test = "N18A" + std::to_string(id++) + "_contract_identity_type" +
                    std::to_string(type_case) + "_name" + std::to_string(name_case) + "_spelling" +
                    std::to_string(spelling);
                drop_case(test, [source, type, spelling] {
                    const auto lowered = drop_lower(source);
                    DropRoutineContract contract;
                    const std::string payload = payload_type(type);
                    contract.resource_type = spelling == 0 ? payload :
                        (spelling == 1 ? lower_ascii(payload) : (spelling == 2 ? upper_ascii(payload) :
                         (spelling == 3 ? payload : lower_ascii(payload))));
                    contract.kind = (spelling == 4) ? DropRoutineKind::CustomDeleter :
                        (spelling == 3 ? DropRoutineKind::Trivial : DropRoutineKind::Destructor);
                    contract.routine_name = contract.kind == DropRoutineKind::Destructor ?
                        "DestroyResource" : (contract.kind == DropRoutineKind::CustomDeleter ? "ReleaseResource" : "");
                    const auto resolved = xfrontend::resolve_ownership_drop_contracts(lowered, {contract});
                    drop_require(resolved.succeeded(), "case-insensitive resource type contract should resolve");
                    drop_require(invocation_count(resolved) == 1, "one cleanup invocation should be resolved");
                    const auto& invocation = resolved.functions.front().blocks.front().invocations.front();
                    drop_require(invocation.owner_type == type, "resolved invocation must retain declared owner wrapper type");
                    drop_require(invocation.resource_type == payload_type(type),
                                 "cleanup contract must resolve against the wrapped resource payload type");
                    drop_require(invocation.routine_name == contract.routine_name, "selected routine name must be preserved");
                    const auto expected = contract.kind == DropRoutineKind::Destructor ?
                        DropInvocationKind::DestructorCall : (contract.kind == DropRoutineKind::CustomDeleter ?
                        DropInvocationKind::CustomDeleterCall : DropInvocationKind::NoOp);
                    drop_require(invocation.kind == expected, "cleanup callable kind must map exactly");
                    drop_require(invocation.guard == xfrontend::OwnershipActionGuard::Always ||
                                  invocation.guard == xfrontend::OwnershipActionGuard::IfLive,
                                  "a live destruction action must retain its guard");
                }, passed, failed);
            }
        }
    }

    // N18B (500): fail-closed validation of malformed or duplicate contracts.
    id = 1;
    for (int type_case = 0; type_case < 10; ++type_case) {
        for (int name_case = 0; name_case < 10; ++name_case) {
            for (int invalid_case = 0; invalid_case < 5; ++invalid_case) {
                const std::string type = drop_type(type_case);
                const std::string owner = drop_name(name_case);
                const std::string source = "function Probe(bool flag) { " + type + " " + owner +
                    " = " + type + ".Create(); PrintLine " + owner + ".Get(); }";
                const std::string test = "N18B" + std::to_string(id++) + "_contract_validation_type" +
                    std::to_string(type_case) + "_name" + std::to_string(name_case) + "_invalid" +
                    std::to_string(invalid_case);
                drop_case(test, [source, type, invalid_case, type_case, name_case] {
                    const auto lowered = drop_lower(source);
                    DropRoutineContract contract;
                    contract.resource_type = payload_type(type);
                    contract.kind = DropRoutineKind::Destructor;
                    contract.routine_name = "DestroyResource";
                    std::vector<DropRoutineContract> list;
                    list.push_back(contract);
                    std::string expected;
                    switch (invalid_case) {
                    case 0: contract.resolved = false; expected = "XDROP0005"; break;
                    case 1: contract.accessible = false; expected = "XDROP0006"; break;
                    case 2: contract.signature_valid = false; expected = "XDROP0007"; break;
                    case 3: contract.receiver_compatible = false; expected = "XDROP0008"; break;
                    default:
                        if (((type_case + name_case) % 2) == 0) {
                            list.push_back(contract);
                            expected = "XDROP0002";
                        } else {
                            list.clear();
                            expected = "XDROP0010";
                        }
                        break;
                    }
                    if (invalid_case != 4) list[0] = contract;
                    const auto resolved = xfrontend::resolve_ownership_drop_contracts(lowered, list);
                    drop_require(!resolved.succeeded(), "invalid cleanup contract must be rejected");
                    drop_require(has_code(resolved, expected), "expected cleanup-contract diagnostic was not emitted");
                    drop_require(resolved.functions.empty(), "failed contract resolution must not return partial plans");
                }, passed, failed);
            }
        }
    }

    // N18C (500): explicit handling of potentially failing cleanup routines.
    id = 1;
    for (int type_case = 0; type_case < 10; ++type_case) {
        for (int name_case = 0; name_case < 10; ++name_case) {
            for (int mode_case = 0; mode_case < 5; ++mode_case) {
                const std::string type = drop_type(type_case);
                const std::string owner = drop_name(name_case);
                const std::string source = "function Probe(bool flag) { " + type + " " + owner +
                    " = " + type + ".Create(); PrintLine " + owner + ".Get(); }";
                const std::string test = "N18C" + std::to_string(id++) + "_failure_policy_type" +
                    std::to_string(type_case) + "_name" + std::to_string(name_case) + "_mode" +
                    std::to_string(mode_case);
                drop_case(test, [source, type, mode_case] {
                    const auto lowered = drop_lower(source);
                    DropRoutineContract contract;
                    contract.resource_type = payload_type(type);
                    contract.kind = (mode_case >= 3) ? DropRoutineKind::CustomDeleter : DropRoutineKind::Destructor;
                    contract.routine_name = "CleanupRoutine";
                    contract.failure_mode = (mode_case == 2) ? DropFailureMode::NoThrow : DropFailureMode::MayFail;
                    xfrontend::DropResolutionOptions options;
                    if (mode_case == 1 || mode_case == 4)
                        options.may_fail_policy = DropFailurePolicy::TerminateOnFailure;
                    const auto resolved = xfrontend::resolve_ownership_drop_contracts(lowered, {contract}, options);
                    if (mode_case == 0 || mode_case == 3) {
                        drop_require(!resolved.succeeded(), "MayFail routine must require explicit policy");
                        drop_require(has_code(resolved, "XDROP0011"), "missing explicit failure-policy diagnostic");
                    } else {
                        drop_require(resolved.succeeded(), "explicitly supported cleanup failure policy should resolve");
                        const auto& invocation = resolved.functions.front().blocks.front().invocations.front();
                        if (mode_case == 1 || mode_case == 4) {
                            drop_require(invocation.terminate_if_failed, "termination policy must be attached to invocation");
                            drop_require(invocation.failure_mode == DropFailureMode::MayFail,
                                          "MayFail metadata must be retained");
                        } else {
                            drop_require(!invocation.terminate_if_failed, "NoThrow routine must not gain termination wrapper");
                            drop_require(invocation.failure_mode == DropFailureMode::NoThrow,
                                          "NoThrow metadata must be retained");
                        }
                    }
                }, passed, failed);
            }
        }
    }

    // N18D (500): deterministic mapping, filtering, and invocation ordering.
    id = 1;
    for (int type_case = 0; type_case < 10; ++type_case) {
        for (int name_case = 0; name_case < 10; ++name_case) {
            for (int layout = 0; layout < 5; ++layout) {
                const std::string type_a = drop_type(type_case);
                const std::string type_b = drop_type(type_case + 3);
                const std::string owner_a = drop_name(name_case);
                const std::string owner_b = drop_name((name_case + 3) % 10);
                std::string body;
                switch (layout) {
                case 0: body = "PrintLine " + owner_a + ".Get(); PrintLine " + owner_b + ".Get();"; break;
                case 1: body = owner_a + ".Reset(); PrintLine " + owner_b + ".Get();"; break;
                case 2: body = "if (flag) { PrintLine " + owner_a + ".Get(); } else { " + owner_b + ".Reset(); }"; break;
                case 3: body = "while (false) { PrintLine " + owner_b + ".Get(); } PrintLine " + owner_a + ".Get();"; break;
                default: body = owner_a + ".Take(); PrintLine " + owner_b + ".Get();"; break;
                }
                const std::string source = "function Probe(bool flag) { " + type_a + " " + owner_a +
                    " = " + type_a + ".Create(); " + type_b + " " + owner_b + " = " + type_b +
                    ".Create(); " + body + " }";
                const std::string test = "N18D" + std::to_string(id++) + "_invocation_order_type" +
                    std::to_string(type_case) + "_name" + std::to_string(name_case) + "_layout" +
                    std::to_string(layout);
                drop_case(test, [source] {
                    const auto lowered = drop_lower(source);
                    const auto contracts = contracts_for(lowered, DropRoutineKind::CustomDeleter);
                    const auto resolved = xfrontend::resolve_ownership_drop_contracts(lowered, contracts);
                    drop_require(resolved.succeeded(), "all cleanup actions must resolve with matching contracts");
                    drop_require(invocation_count(resolved) == destroy_action_count(lowered),
                                  "exactly one invocation record is required per DestroyResource action");
                    for (const auto& function : resolved.functions) {
                        for (const auto& block : function.blocks) {
                            std::size_t previous = 0;
                            bool first = true;
                            for (const auto& invocation : block.invocations) {
                                drop_require(invocation.block_id == block.id,
                                              "invocation must retain original basic-block identity");
                                drop_require(invocation.variable_id < function.variables.size(),
                                              "invocation must retain valid owner variable identity");
                                drop_require(function.variables[invocation.variable_id].is_unique_owner,
                                              "invocation must reference a Unique owner");
                                if (!first) drop_require(invocation.action_index > previous,
                                                         "invocation order must follow lowered action order");
                                previous = invocation.action_index;
                                first = false;
                                drop_require(invocation.kind == DropInvocationKind::CustomDeleterCall,
                                              "custom deleter contract must produce custom-deleter invocation");
                            }
                            if (!block.reachable)
                                drop_require(block.invocations.empty(), "unreachable blocks must not invoke cleanup");
                        }
                    }
                }, passed, failed);
            }
        }
    }
}
