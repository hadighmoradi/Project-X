#include "xfrontend/control_flow.hpp"
#include "xfrontend/ownership_lowering.hpp"
#include "xfrontend/parser.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using xfrontend::ControlFlowResult;
using xfrontend::OwnershipActionGuard;
using xfrontend::OwnershipActionKind;
using xfrontend::OwnershipLoweredAction;
using xfrontend::OwnershipLoweringResult;
using xfrontend::OwnershipTransferTarget;

void lower_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

ControlFlowResult lower_flow(const std::string& source) {
    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        throw std::runtime_error("N17 source parse failed: " + parsed.diagnostics.front().code +
            " " + parsed.diagnostics.front().message + " | " + source);
    }
    auto flow = xfrontend::analyze_control_flow(parsed.unit);
    if (!flow.succeeded()) {
        throw std::runtime_error("N17 CFG failed: " + flow.diagnostics.front().code +
            " " + flow.diagnostics.front().message + " | " + source);
    }
    return flow;
}

std::string lower_type(int index) {
    static const std::vector<std::string> values{
        "Unique[Device]", "UniqueArray[Device]", "Unique[Widget]", "UniqueArray[Widget]",
        "Unique[Buffer]", "UniqueArray[Buffer]", "Unique[Node]", "UniqueArray[Node]",
        "Unique[Resource]", "UniqueArray[Resource]"
    };
    return values[static_cast<std::size_t>(index % static_cast<int>(values.size()))];
}

std::string lower_name(int index) {
    static const std::vector<std::string> values{
        "owner", "Owner", "OWNER", "resource", "Resource", "unit", "Unit", "handle",
        "Handle", "payload", "Payload", "asset"
    };
    return values[static_cast<std::size_t>(index % static_cast<int>(values.size()))];
}

std::string create_value(const std::string& type) { return type + ".Create()"; }

int count_action(const OwnershipLoweringResult& result, OwnershipActionKind kind) {
    int count = 0;
    for (const auto& function : result.functions)
        for (const auto& block : function.blocks)
            for (const auto& action : block.actions)
                if (action.kind == kind) ++count;
    return count;
}

int count_action_for(const OwnershipLoweringResult& result, OwnershipActionKind kind,
                     xfrontend::FlowVariableId variable_id) {
    int count = 0;
    for (const auto& function : result.functions)
        for (const auto& block : function.blocks)
            for (const auto& action : block.actions)
                if (action.kind == kind && action.variable_id == variable_id) ++count;
    return count;
}

std::vector<OwnershipLoweredAction> actions_of(const OwnershipLoweringResult& result,
                                                OwnershipActionKind kind) {
    std::vector<OwnershipLoweredAction> actions;
    for (const auto& function : result.functions)
        for (const auto& block : function.blocks)
            for (const auto& action : block.actions)
                if (action.kind == kind) actions.push_back(action);
    return actions;
}

xfrontend::FlowVariableId find_owner(const xfrontend::ControlFlowGraph& graph,
                                     const std::string& name) {
    auto fold = [](std::string value) {
        for (char& ch : value)
            if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        return value;
    };
    for (const auto& variable : graph.variables)
        if (variable.is_unique_owner && fold(variable.name) == fold(name)) return variable.id;
    throw std::runtime_error("owner not present in CFG: " + name);
}

void lower_case(const std::string& name, const std::function<void()>& body,
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

void check_basic_plan(const OwnershipLoweringResult& lowered) {
    lower_require(lowered.succeeded(), "cleanup lowering should succeed");
    lower_require(lowered.functions.size() == 1U, "one lowered function expected");
    const auto& function = lowered.functions.front();
    for (std::size_t index = 0; index < function.blocks.size(); ++index) {
        lower_require(function.blocks[index].id == index, "lowered block identity must be stable");
    }
    for (const auto& block : function.blocks) {
        if (!block.reachable) lower_require(block.actions.empty(), "unreachable block must not produce runtime actions");
        for (const auto& action : block.actions) {
            lower_require(action.block_id == block.id, "action must retain source CFG block id");
            lower_require(action.variable_id < function.variables.size(), "action owner id must be valid");
            lower_require(function.variables[action.variable_id].is_unique_owner,
                          "action must reference a Unique owner");
        }
    }
}

} // namespace

void run_ownership_lowering_tests(int& passed, int& failed) {
    // N17A (500): establish/leave-lifetime plan matrix.
    int id = 1;
    for (int type_case = 0; type_case < 10; ++type_case) {
        for (int name_case = 0; name_case < 10; ++name_case) {
            for (int layout = 0; layout < 5; ++layout) {
                const std::string type = lower_type(type_case);
                const std::string owner = lower_name(name_case);
                std::string body;
                switch (layout) {
                case 0: body = "PrintLine " + owner + ".Get();"; break;
                case 1: body = "if (flag) { PrintLine " + owner + ".Get(); }"; break;
                case 2: body = "{ PrintLine " + owner + ".Get(); }"; break;
                case 3: body = "while (false) { PrintLine " + owner + ".Get(); }"; break;
                default: body = "PrintLine \"resource-ready\";"; break;
                }
                const std::string source = "function Probe(bool flag) { " + type + " " + owner +
                    " = " + create_value(type) + "; " + body + " }";
                const std::string test_name = "N17A" + std::to_string(id++) +
                    "_init_plan_type" + std::to_string(type_case) + "_name" +
                    std::to_string(name_case) + "_layout" + std::to_string(layout);
                lower_case(test_name, [source, owner] {
                    const auto flow = lower_flow(source);
                    const auto lowered = xfrontend::lower_ownership_cleanup(flow);
                    check_basic_plan(lowered);
                    const auto owner_id = find_owner(flow.functions.front(), owner);
                    lower_require(count_action_for(lowered, OwnershipActionKind::EstablishResource, owner_id) == 1,
                                  "one resource establishment action expected");
                    lower_require(count_action(lowered, OwnershipActionKind::EndLifetime) == 1,
                                  "one owner lifetime-end action expected");
                    lower_require(count_action(lowered, OwnershipActionKind::DestroyResource) == 1,
                                  "live owner must be destroyed on scope exit");
                }, passed, failed);
            }
        }
    }

    // N17B (500): Reset lowering and live-guard selection matrix.
    id = 1;
    for (int type_case = 0; type_case < 10; ++type_case) {
        for (int name_case = 0; name_case < 10; ++name_case) {
            for (int sequence = 0; sequence < 5; ++sequence) {
                const std::string type = lower_type(type_case);
                const std::string owner = lower_name(name_case);
                std::string body;
                switch (sequence) {
                case 0: body = owner + ".Reset();"; break;
                case 1: body = owner + ".Reset(); " + owner + ".Reset();"; break;
                case 2: body = owner + ".Reset(); " + owner + " = " + create_value(type) + ";"; break;
                case 3: body = owner + ".Take(); " + owner + ".Reset();"; break;
                default: body = "if (flag) { " + owner + ".Reset(); } else { PrintLine " + owner + ".Get(); }"; break;
                }
                const std::string source = "function Probe(bool flag) { " + type + " " + owner +
                    " = " + create_value(type) + "; " + body + " }";
                const std::string test_name = "N17B" + std::to_string(id++) +
                    "_reset_plan_type" + std::to_string(type_case) + "_name" +
                    std::to_string(name_case) + "_sequence" + std::to_string(sequence);
                lower_case(test_name, [source, sequence] {
                    const auto flow = lower_flow(source);
                    const auto lowered = xfrontend::lower_ownership_cleanup(flow);
                    check_basic_plan(lowered);
                    lower_require(count_action(lowered, OwnershipActionKind::ClearOwner) >= 1,
                                  "Reset must produce an explicit empty-state action");
                    lower_require(count_action(lowered, OwnershipActionKind::EndLifetime) == 1,
                                  "scope end must be retained independently from Reset");
                    const auto destroys = actions_of(lowered, OwnershipActionKind::DestroyResource);
                    for (const auto& action : destroys)
                        lower_require(action.guard != OwnershipActionGuard::Never,
                                      "never-live destruction must not be emitted");
                    if (sequence == 0 || sequence == 1 || sequence == 2)
                        lower_require(!destroys.empty(), "Reset of a live owner must destroy its resource");
                    if (sequence == 3) {
                        lower_require(destroys.empty(), "Take followed by Reset must not destroy the transferred resource");
                        const auto transfers = actions_of(lowered, OwnershipActionKind::TransferResource);
                        lower_require(transfers.size() == 1U &&
                                      transfers.front().transfer_target == OwnershipTransferTarget::ExpressionValue,
                                      "discarded Take expression must retain its expression-value transfer target");
                    }
                    if (sequence == 4) {
                        lower_require(std::any_of(destroys.begin(), destroys.end(),
                            [](const OwnershipLoweredAction& action) {
                                return action.guard == OwnershipActionGuard::IfLive;
                            }), "branch-joined owner cleanup must retain an IfLive guard");
                    }
                }, passed, failed);
            }
        }
    }

    // N17C (500): destination-aware Take and return-value lowering matrix.
    id = 1;
    for (int type_case = 0; type_case < 10; ++type_case) {
        for (int name_case = 0; name_case < 10; ++name_case) {
            for (int form = 0; form < 5; ++form) {
                const std::string type = lower_type(type_case);
                const std::string source_name = lower_name(name_case);
                const std::string destination = "target" + std::to_string(name_case);
                std::string body;
                switch (form) {
                case 0:
                    body = type + " " + destination + " = " + source_name + ".Take();";
                    break;
                case 1:
                    body = type + " " + destination + "; " + destination + " = " + source_name + ".Take();";
                    break;
                case 2:
                    body = type + " middle" + std::to_string(name_case) + " = " + source_name +
                        ".Take(); " + type + " " + destination + " = middle" +
                        std::to_string(name_case) + ".Take();";
                    break;
                case 3:
                    body = source_name + " = " + source_name + ".Take();";
                    break;
                default:
                    body = "return " + source_name + ";";
                    break;
                }
                const std::string source = "function Probe() { " + type + " " + source_name +
                    " = " + create_value(type) + "; " + body + " }";
                const std::string test_name = "N17C" + std::to_string(id++) +
                    "_take_plan_type" + std::to_string(type_case) + "_name" +
                    std::to_string(name_case) + "_form" + std::to_string(form);
                lower_case(test_name, [source, form] {
                    const auto flow = lower_flow(source);
                    const auto lowered = xfrontend::lower_ownership_cleanup(flow);
                    check_basic_plan(lowered);
                    const auto transfers = actions_of(lowered, OwnershipActionKind::TransferResource);
                    const int expected = form == 2 ? 2 : 1;
                    lower_require(static_cast<int>(transfers.size()) == expected,
                                  "unexpected number of lowered Take transfers");
                    const auto expected_target = form == 3 ? OwnershipTransferTarget::Self :
                        (form == 4 ? OwnershipTransferTarget::ReturnValue : OwnershipTransferTarget::Variable);
                    for (const auto& action : transfers)
                        lower_require(action.transfer_target == expected_target,
                                      "Take transfer destination classification mismatch");
                }, passed, failed);
            }
        }
    }

    // N17D (500): lexical cleanup plus fail-closed lowering gates.
    id = 1;
    for (int type_case = 0; type_case < 10; ++type_case) {
        for (int name_case = 0; name_case < 12; ++name_case) {
            for (int layout = 0; layout < 4; ++layout) {
                const std::string type = lower_type(type_case);
                const std::string owner = lower_name(name_case);
                const std::string nested = "nested" + std::to_string(name_case);
                std::string body;
                switch (layout) {
                case 0:
                    body = "{ " + type + " " + nested + " = " + create_value(type) +
                        "; PrintLine " + nested + ".Get(); }";
                    break;
                case 1:
                    body = "if (flag) { " + type + " " + nested + " = " + create_value(type) +
                        "; PrintLine " + nested + ".Get(); }";
                    break;
                case 2:
                    body = "while (flag) { " + type + " " + nested + " = " + create_value(type) +
                        "; break; }";
                    break;
                default:
                    body = "while (flag) { " + type + " " + nested + " = " + create_value(type) +
                        "; continue; }";
                    break;
                }
                const std::string source = "function Probe(bool flag) { " + type + " " + owner +
                    " = " + create_value(type) + "; " + body + " }";
                const std::string test_name = "N17D" + std::to_string(id++) +
                    "_cleanup_plan_type" + std::to_string(type_case) + "_name" +
                    std::to_string(name_case) + "_layout" + std::to_string(layout);
                lower_case(test_name, [source, owner, nested, layout] {
                    const auto flow = lower_flow(source);
                    const auto lowered = xfrontend::lower_ownership_cleanup(flow);
                    check_basic_plan(lowered);
                    lower_require(count_action(lowered, OwnershipActionKind::EndLifetime) >= 2,
                                  "outer and nested owner cleanup must both be represented");
                    lower_require(count_action(lowered, OwnershipActionKind::EstablishResource) == 2,
                                  "both owners must be initialized in the plan");
                    if (layout == 0) {
                        const auto outer_id = find_owner(flow.functions.front(), owner);
                        const auto nested_id = find_owner(flow.functions.front(), nested);
                        const auto endings = actions_of(lowered, OwnershipActionKind::EndLifetime);
                        lower_require(endings.size() == 2U, "two lexical lifetime ends expected in direct nested block");
                        lower_require(endings[0].variable_id == nested_id && endings[1].variable_id == outer_id,
                                      "lowered cleanup must preserve reverse lexical declaration order");
                    }
                }, passed, failed);
            }
        }
    }

    // 20 fail-closed cases are part of N17D's 500-case allocation.
    for (int type_case = 0; type_case < 10; ++type_case) {
        const std::string type = lower_type(type_case);
        const std::string source = "function Probe() { " + type + " source = " +
            create_value(type) + "; " + type + " target = source; }";
        const std::string test_name = "N17D" + std::to_string(id++) +
            "_cleanup_plan_copy_gate_type" + std::to_string(type_case);
        lower_case(test_name, [source] {
            const auto flow = lower_flow(source);
            const auto lowered = xfrontend::lower_ownership_cleanup(flow);
            lower_require(!lowered.succeeded(), "move-only owner copy must fail lowering");
            lower_require(lowered.functions.empty(), "invalid function must not produce a partial lowered plan");
            lower_require(std::any_of(lowered.diagnostics.begin(), lowered.diagnostics.end(),
                [](const xfrontend::Diagnostic& diagnostic) { return diagnostic.code == "XLOWER0002"; }),
                "copy reaching lowering must produce XLOWER0002");
        }, passed, failed);
    }
    for (int type_case = 0; type_case < 10; ++type_case) {
        const std::string type = lower_type(type_case);
        const std::string source = "function Probe() { " + type + " owner = " + create_value(type) + "; }";
        const std::string test_name = "N17D" + std::to_string(id++) +
            "_cleanup_plan_convergence_gate_type" + std::to_string(type_case);
        lower_case(test_name, [source] {
            auto flow = lower_flow(source);
            lower_require(flow.functions.size() == 1U, "one flow graph expected");
            flow.functions.front().ownership_analysis_converged = false;
            const auto lowered = xfrontend::lower_ownership_cleanup(flow);
            lower_require(!lowered.succeeded(), "non-converged flow must fail lowering");
            lower_require(lowered.functions.empty(), "non-converged graph must not produce a plan");
            lower_require(std::any_of(lowered.diagnostics.begin(), lowered.diagnostics.end(),
                [](const xfrontend::Diagnostic& diagnostic) { return diagnostic.code == "XLOWER0001"; }),
                "convergence failure must produce XLOWER0001");
        }, passed, failed);
    }
}
