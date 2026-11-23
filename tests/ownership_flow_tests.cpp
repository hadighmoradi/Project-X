#include "xfrontend/control_flow.hpp"
#include "xfrontend/parser.hpp"
#include "test_report.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void move_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

xfrontend::ControlFlowResult move_flow(const std::string& source) {
    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        throw std::runtime_error("ownership flow source parse failed: " +
            parsed.diagnostics.front().code + " " + parsed.diagnostics.front().message +
            " | source=" + source);
    }
    return xfrontend::analyze_control_flow(parsed.unit);
}

void move_case(const std::string& name, const std::function<void()>& body,
               int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    const auto start = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        std::cout << "PASS " << name << '\n';
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        std::cout << "FAIL " << name << ": " << error.what() << '\n';
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

std::string type_name(int index) {
    static const std::vector<std::string> names{
        "Unique[Device]", "UniqueArray[Device]", "Unique[Widget]", "UniqueArray[Widget]",
        "Unique[Buffer]", "UniqueArray[Buffer]", "UNIQUE[Device]", "Unique[Node]",
        "UniqueArray[Node]", "Unique[Resource]"
    };
    return names[static_cast<std::size_t>(index % static_cast<int>(names.size()))];
}

std::string create_value(const std::string& type) { return type + ".Create()"; }

xfrontend::FlowVariableId variable_id(const xfrontend::ControlFlowGraph& graph,
                                      const std::string& name) {
    for (const auto& variable : graph.variables) {
        std::string left = variable.name;
        std::string right = name;
        for (char& ch : left) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        for (char& ch : right) if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
        if (left == right && variable.is_unique_owner) return variable.id;
    }
    throw std::runtime_error("Unique variable not found: " + name);
}

const xfrontend::FlowOperation* owner_drop_for(const xfrontend::ControlFlowGraph& graph,
                                               xfrontend::FlowVariableId id) {
    for (auto block = graph.blocks.rbegin(); block != graph.blocks.rend(); ++block) {
        for (auto operation = block->operations.rbegin(); operation != block->operations.rend(); ++operation) {
            if (operation->kind == xfrontend::FlowOperationKind::OwnerDrop &&
                operation->variable_id == id) return &*operation;
        }
    }
    return nullptr;
}

const xfrontend::FlowOperation* transfer_to(const xfrontend::ControlFlowGraph& graph,
                                            xfrontend::FlowVariableId destination) {
    for (const auto& block : graph.blocks) {
        for (const auto& operation : block.operations) {
            if (operation.kind == xfrontend::FlowOperationKind::OwnerTransfer &&
                operation.related_variable_id == destination) return &operation;
        }
    }
    return nullptr;
}

int count_owner_drops(const xfrontend::ControlFlowGraph& graph) {
    int count = 0;
    for (const auto& block : graph.blocks) {
        for (const auto& operation : block.operations) {
            if (operation.kind == xfrontend::FlowOperationKind::OwnerDrop) ++count;
        }
    }
    return count;
}

bool returns_drop_before_terminator(const xfrontend::ControlFlowGraph& graph) {
    bool saw_return = false;
    for (const auto& block : graph.blocks) {
        std::size_t return_index = block.operations.size();
        for (std::size_t index = 0; index < block.operations.size(); ++index) {
            if (block.operations[index].kind == xfrontend::FlowOperationKind::Return) {
                return_index = index;
                saw_return = true;
                break;
            }
        }
        if (return_index == block.operations.size()) continue;
        for (std::size_t index = return_index + 1U; index < block.operations.size(); ++index) {
            if (block.operations[index].kind == xfrontend::FlowOperationKind::OwnerDrop) return false;
        }
        bool has_drop = false;
        for (std::size_t index = 0; index < return_index; ++index) {
            if (block.operations[index].kind == xfrontend::FlowOperationKind::OwnerDrop) has_drop = true;
        }
        if (!has_drop) return false;
    }
    return saw_return;
}

} // namespace

void run_ownership_flow_tests(int& passed, int& failed) {
    using namespace xfrontend;

    // U (100): precise ownership joins across branch and loop edges.
    {
        int id = 109;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                const std::string type = type_name(type_case);
                const std::string owner = scenario % 2 == 0 ? "owner" : "OwNeR";
                const std::string create = create_value(type);
                std::string body;
                FlowOwnershipState expected = FlowOwnershipState::Live;
                switch (scenario) {
                case 0:
                    body = "if (flag) { " + owner + ".Take(); } else { PrintLine " + owner + ".Get(); }";
                    expected = FlowOwnershipState::MaybeLive;
                    break;
                case 1:
                    body = "if (flag) { " + owner + ".Reset(); } else { " + owner + ".Take(); }";
                    expected = FlowOwnershipState::Empty;
                    break;
                case 2:
                    body = "if (flag) { PrintLine " + owner + ".Get(); } else { PrintLine " + owner + ".Get(); }";
                    expected = FlowOwnershipState::Live;
                    break;
                case 3:
                    body = "if (flag) { " + owner + ".Reset(); } else { " + owner + " = " + create + "; }";
                    expected = FlowOwnershipState::MaybeLive;
                    break;
                case 4:
                    body = "if (flag) { " + owner + ".Take(); } else { " + owner + ".Reset(); }";
                    expected = FlowOwnershipState::Empty;
                    break;
                case 5:
                    body = "if (flag) { " + owner + ".Reset(); " + owner + " = " + create + "; } else { " + owner + ".Take(); " + owner + " = " + create + "; }";
                    expected = FlowOwnershipState::Live;
                    break;
                case 6:
                    body = "if (false) { " + owner + ".Reset(); } else { PrintLine " + owner + ".Get(); }";
                    expected = FlowOwnershipState::Live;
                    break;
                case 7:
                    body = "if (true) { " + owner + ".Take(); } else { PrintLine " + owner + ".Get(); }";
                    expected = FlowOwnershipState::Empty;
                    break;
                case 8:
                    body = "if (flag) { if (flag2) { " + owner + ".Take(); } else { " + owner + ".Reset(); } } else { " + owner + " = " + create + "; }";
                    expected = FlowOwnershipState::MaybeLive;
                    break;
                default:
                    body = "while (flag) { " + owner + ".Take(); break; }";
                    expected = FlowOwnershipState::MaybeLive;
                    break;
                }
                const std::string source = "function Probe(bool flag, bool flag2) { " + type + " " + owner + " = " + create + "; " + body + " }";
                const std::string name = "U" + std::to_string(id++) + "_move_join_type" + std::to_string(type_case) + "_scenario" + std::to_string(scenario);
                move_case(name, [source, owner, expected] {
                    const auto result = move_flow(source);
                    move_require(result.succeeded(), "unexpected CFG diagnostic");
                    move_require(result.functions.size() == 1U, "single function graph expected");
                    const auto& graph = result.functions[0];
                    move_require(graph.ownership_analysis_converged, "ownership dataflow did not converge");
                    const auto id = variable_id(graph, owner);
                    const auto* drop = owner_drop_for(graph, id);
                    move_require(drop != nullptr, "function-scope owner must have a drop event");
                    move_require(drop->ownership_before == expected,
                                 "unexpected owner state before scope drop");
                    move_require(drop->conditional, "scope drop must be conditional");
                }, passed, failed);
            }
        }
    }

    // V (100): destination-aware transfers preserve source-empty and target-live
    // states for both declarations and assignments.
    {
        int id = 209;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                const std::string type = type_name(type_case);
                const std::string source_name = scenario % 2 == 0 ? "source" : "SoUrCe";
                const std::string target_name = scenario == 9 ? source_name :
                    (scenario % 3 == 0 ? "target" : "TaRgEt");
                std::string source;
                if (scenario == 9) {
                    source = "function Probe() { " + type + " " + source_name + " = " + create_value(type) + "; " +
                        source_name + " = " + source_name + ".Take(); PrintLine " + source_name + ".Get(); }";
                } else if (scenario % 2 == 0) {
                    source = "function Probe() { " + type + " " + source_name + " = " + create_value(type) + "; " +
                        type + " " + target_name + " = " + source_name + ".Take(); PrintLine " + target_name + ".Get(); }";
                } else {
                    source = "function Probe() { " + type + " " + source_name + " = " + create_value(type) + "; " +
                        type + " " + target_name + " = " + create_value(type) + "; " + target_name + " = " + source_name + ".Take(); PrintLine " + target_name + ".Get(); }";
                }
                const bool assignment_transfer = scenario % 2 == 1 && scenario != 9;
                const std::string name = "V" + std::to_string(id++) + "_transfer_state_type" + std::to_string(type_case) + "_scenario" + std::to_string(scenario);
                move_case(name, [source, source_name, target_name, assignment_transfer] {
                    const auto result = move_flow(source);
                    move_require(result.succeeded(), "unexpected CFG diagnostic");
                    const auto& graph = result.functions.at(0);
                    const auto source_id = variable_id(graph, source_name);
                    const auto target_id = variable_id(graph, target_name);
                    const auto* transfer = transfer_to(graph, target_id);
                    move_require(transfer != nullptr, "destination-aware transfer event missing");
                    move_require(transfer->ownership_before == FlowOwnershipState::Live,
                                 "transfer source must be live before Take");
                    const auto* source_drop = owner_drop_for(graph, source_id);
                    const auto* target_drop = owner_drop_for(graph, target_id);
                    if (source_id == target_id) {
                        move_require(transfer->ownership_after == FlowOwnershipState::Live,
                                     "self-Take assignment must restore the same resource");
                        move_require(source_drop && source_drop->ownership_before == FlowOwnershipState::Live,
                                     "self-Take must leave the wrapper live");
                    } else {
                        move_require(transfer->ownership_after == FlowOwnershipState::Empty,
                                     "transfer source must become empty");
                        move_require(source_drop && source_drop->ownership_before == FlowOwnershipState::Empty,
                                     "source should remain initialized but empty");
                        move_require(target_drop && target_drop->ownership_before == FlowOwnershipState::Live,
                                     "destination should receive the resource");
                        if (assignment_transfer) {
                            bool saw_replacement_reset = false;
                            bool saw_transfer = false;
                            for (const auto& block : graph.blocks) {
                                for (const auto& operation : block.operations) {
                                    if (operation.kind == FlowOperationKind::OwnerTransfer &&
                                        operation.related_variable_id == target_id) saw_transfer = true;
                                    if (!saw_transfer && operation.kind == FlowOperationKind::OwnerReset &&
                                        operation.variable_id == target_id &&
                                        operation.ownership_before == FlowOwnershipState::Live &&
                                        operation.ownership_after == FlowOwnershipState::Empty) {
                                        saw_replacement_reset = true;
                                    }
                                }
                            }
                            move_require(saw_replacement_reset,
                                         "assignment must clear the previous destination resource before transfer");
                        }
                    }
                }, passed, failed);
            }
        }
    }

    // W (100): nested lexical scopes drop Unique owners in reverse declaration
    // order and preserve one conditional cleanup event per local owner.
    {
        int id = 309;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                const int owner_count = 1 + (scenario % 5);
                std::string body = "function Probe() { { ";
                std::vector<std::string> names;
                for (int owner_index = 0; owner_index < owner_count; ++owner_index) {
                    const std::string name = "item" + std::to_string(owner_index);
                    names.push_back(name);
                    const std::string type = type_name(type_case + owner_index);
                    body += type + " " + name + " = " + create_value(type) + "; ";
                }
                body += "} }";
                const std::string name = "W" + std::to_string(id++) + "_scope_drop_type" + std::to_string(type_case) + "_count" + std::to_string(scenario);
                move_case(name, [body, owner_count, names] {
                    const auto result = move_flow(body);
                    move_require(result.succeeded(), "unexpected CFG diagnostic");
                    const auto& graph = result.functions.at(0);
                    move_require(count_owner_drops(graph) == owner_count,
                                 "nested scope must emit exactly one drop per Unique local");
                    std::vector<std::string> drop_names;
                    for (const auto& block : graph.blocks) {
                        for (const auto& operation : block.operations) {
                            if (operation.kind == FlowOperationKind::OwnerDrop) drop_names.push_back(operation.name);
                        }
                    }
                    move_require(drop_names.size() == names.size(), "drop list size mismatch");
                    for (std::size_t index = 0; index < names.size(); ++index) {
                        move_require(drop_names[index] == names[names.size() - index - 1U],
                                     "owners must drop in reverse declaration order");
                    }
                }, passed, failed);
            }
        }
    }

    // X (100): loop bodies, continue, break, and loop backedges carry scope
    // cleanup. Every graph must converge and record body owner drops.
    {
        int id = 409;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                const std::string type = type_name(type_case);
                const std::string owner = scenario % 2 == 0 ? "loopOwner" : "LoopOwner";
                std::string action;
                switch (scenario) {
                case 0: action = owner + ".Reset(); break;"; break;
                case 1: action = owner + ".Take(); continue;"; break;
                case 2: action = owner + " = " + create_value(type) + "; break;"; break;
                case 3: action = "if (flag) { continue; } " + owner + ".Reset(); break;"; break;
                case 4: action = "if (flag) { " + owner + ".Take(); break; } " + owner + ".Reset();"; break;
                case 5: action = owner + ".Take(); " + owner + " = " + create_value(type) + "; continue;"; break;
                case 6: action = "if (flag) { " + owner + ".Reset(); } else { " + owner + ".Take(); } break;"; break;
                case 7: action = owner + ".Reset(); if (flag) { break; } continue;"; break;
                case 8: action = owner + ".Take(); break;"; break;
                default: action = "if (flag) { continue; } break;"; break;
                }
                const std::string source = "function Probe(bool flag) { while (flag) { " + type + " " + owner + " = " + create_value(type) + "; " + action + " } }";
                const std::string name = "X" + std::to_string(id++) + "_loop_cleanup_type" + std::to_string(type_case) + "_scenario" + std::to_string(scenario);
                move_case(name, [source] {
                    const auto result = move_flow(source);
                    move_require(result.succeeded(), "unexpected CFG diagnostic");
                    const auto& graph = result.functions.at(0);
                    move_require(graph.ownership_analysis_converged, "loop ownership fixed point did not converge");
                    move_require(graph.ownership_analysis_iterations > 0U, "ownership analysis must execute");
                    move_require(count_owner_drops(graph) >= 1, "loop-local Unique needs cleanup on every exit path");
                    for (const auto& block : graph.blocks) {
                        if (!block.reachable) continue;
                        move_require(block.ownership_in.size() == graph.variables.size(),
                                     "reachable block must expose ownership input snapshot");
                        move_require(block.ownership_out.size() == graph.variables.size(),
                                     "reachable block must expose ownership output snapshot");
                    }
                }, passed, failed);
            }
        }
    }

    // Y (100): return paths drop local owners before the terminator; returning
    // a named Unique transfers its resource before conditional drop.
    {
        int id = 509;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                const std::string type = type_name(type_case);
                const std::string outer = scenario % 2 == 0 ? "outer" : "OuTeR";
                const std::string inner = scenario % 3 == 0 ? "inner" : "InNeR";
                std::string body;
                if (scenario % 2 == 0) {
                    body = "function Probe(bool flag) returns int { " + type + " " + outer + " = " + create_value(type) + "; if (flag) { " + type + " " + inner + " = " + create_value(type) + "; return 1; } else { return 2; } }";
                } else {
                    body = "function Probe() returns " + type + " { " + type + " " + outer + " = " + create_value(type) + "; return " + outer + "; }";
                }
                const std::string name = "Y" + std::to_string(id++) + "_return_drop_type" + std::to_string(type_case) + "_scenario" + std::to_string(scenario);
                move_case(name, [body] {
                    const auto result = move_flow(body);
                    move_require(result.succeeded(), "unexpected CFG diagnostic");
                    const auto& graph = result.functions.at(0);
                    move_require(graph.ownership_analysis_converged, "return ownership analysis must converge");
                    move_require(returns_drop_before_terminator(graph), "each reachable return path must drop locals before Return");
                    move_require(count_owner_drops(graph) >= 1, "at least one scope drop event expected");
                    if (body.find("return outer") != std::string::npos || body.find("return OuTeR") != std::string::npos) {
                        const auto owner_id = variable_id(graph, "outer");
                        bool saw_return_transfer = false;
                        for (const auto& block : graph.blocks) {
                            for (const auto& operation : block.operations) {
                                if (operation.kind == FlowOperationKind::OwnerTransfer &&
                                    operation.variable_id == owner_id &&
                                    operation.related_variable_id == InvalidFlowVariableId) {
                                    saw_return_transfer = true;
                                }
                            }
                        }
                        move_require(saw_return_transfer, "returning a named Unique must transfer its resource");
                    }
                }, passed, failed);
            }
        }
    }
}
