#include "xfrontend/control_flow.hpp"
#include "xfrontend/lifetime.hpp"
#include "xfrontend/parser.hpp"
#include "test_report.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void own_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

xfrontend::ParseResult parse_ownership_source(const std::string& source) {
    auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        throw std::runtime_error("ownership source parse failed: " +
            parsed.diagnostics.front().code + " " + parsed.diagnostics.front().message +
            " | source=" + source);
    }
    return parsed;
}

xfrontend::LifetimeResult ownership_lifetime(const std::string& source) {
    const auto parsed = parse_ownership_source(source);
    return xfrontend::analyze_lifetimes(parsed.unit);
}

bool has_code(const xfrontend::LifetimeResult& result, const std::string& code) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

void expect_lifetime_error(const std::string& source, const std::string& code) {
    const auto result = ownership_lifetime(source);
    own_require(has_code(result, code), "expected lifetime diagnostic " + code);
}

void expect_lifetime_valid(const std::string& source) {
    const auto result = ownership_lifetime(source);
    own_require(result.succeeded(), "unexpected lifetime diagnostic: " +
        (result.diagnostics.empty() ? std::string() :
         result.diagnostics.front().code + " " + result.diagnostics.front().message));
}

void ownership_case(const std::string& name, const std::function<void()>& body,
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

std::string number3(int value) {
    return value < 10 ? "00" + std::to_string(value) :
           value < 100 ? "0" + std::to_string(value) : std::to_string(value);
}

std::string unique_type(int index) {
    static const std::vector<std::string> types{
        "Unique[Device]", "UniqueArray[Device]", "Unique[Widget]", "UniqueArray[Widget]",
        "Unique[Buffer]", "UniqueArray[Buffer]", "UNIQUE[Device]", "Unique[Node]",
        "UniqueArray[Node]", "Unique[Resource]"
    };
    return types[static_cast<std::size_t>(index % static_cast<int>(types.size()))];
}

std::string create_expr(const std::string& type) {
    return type + ".Create()";
}

std::string wrap_nested(const std::string& body, int depth) {
    std::string result = body;
    for (int i = 0; i < depth; ++i) result = "{ " + result + " }";
    return result;
}

bool graph_has_operation(const xfrontend::ControlFlowGraph& graph,
                         xfrontend::FlowOperationKind kind) {
    for (const auto& block : graph.blocks) {
        for (const auto& operation : block.operations) {
            if (operation.kind == kind) return true;
        }
    }
    return false;
}

const xfrontend::FlowOperation* graph_find_operation(const xfrontend::ControlFlowGraph& graph,
                                                      xfrontend::FlowOperationKind kind) {
    for (auto block = graph.blocks.rbegin(); block != graph.blocks.rend(); ++block) {
        for (auto operation = block->operations.rbegin(); operation != block->operations.rend(); ++operation) {
            if (operation->kind == kind) return &*operation;
        }
    }
    return nullptr;
}

} // namespace

void run_ownership_semantics_tests(int& passed, int& failed) {
    // Matrix M (100): a named Unique owner cannot be copied into another
    // Unique/UniqueArray local. Type spellings and identifier case vary.
    {
        int index = 109;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int name_case = 0; name_case < 10; ++name_case) {
                const std::string type = unique_type(type_case);
                const std::string source_name = name_case % 2 == 0 ? "source" : "SoUrCe";
                const std::string target_name = name_case % 3 == 0 ? "target" : "TaRgEt";
                const std::string source = "function Probe() { " + type + " " + source_name + " = " +
                    create_expr(type) + "; " + type + " " + target_name + " = " + source_name + "; }";
                const std::string test_name = "M" + number3(index++) + "_move_copy_matrix_type" +
                    std::to_string(type_case) + "_names" + std::to_string(name_case);
                ownership_case(test_name, [source] { expect_lifetime_error(source, "XLIFE0007"); },
                               passed, failed);
            }
        }
    }

    // Matrix N (100): ordinary assignment between two Unique owners is a copy,
    // while self-assignment and explicit .Take() transfer are handled separately.
    {
        int index = 209;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                const std::string type = unique_type(type_case);
                const std::string left = scenario % 2 == 0 ? "left" : "LeFt";
                const std::string right = scenario % 3 == 0 ? "right" : "RiGhT";
                const std::string prefix = "function Probe() { " + type + " " + left + " = " +
                    create_expr(type) + "; " + type + " " + right + " = " + create_expr(type) + "; ";
                std::string source;
                bool should_fail = true;
                if (scenario == 0) {
                    source = prefix + left + " = " + right + "; }";
                } else if (scenario == 1) {
                    source = prefix + right + " = " + left + "; }";
                } else if (scenario == 2) {
                    source = prefix + left + " = " + left + "; }";
                    should_fail = false;
                } else if (scenario == 3) {
                    source = prefix + right + " = " + left + ".Take(); PrintLine " + right + ".Get(); }";
                    should_fail = false;
                } else if (scenario == 4) {
                    source = prefix + left + " = " + right + ".Take(); PrintLine " + left + ".Get(); }";
                    should_fail = false;
                } else if (scenario == 5) {
                    source = prefix + right + " = " + create_expr(type) + "; }";
                    should_fail = false;
                } else if (scenario == 6) {
                    source = prefix + "if (true) { " + right + " = " + left + "; } }";
                } else if (scenario == 7) {
                    source = prefix + "while (false) { " + right + " = " + left + "; } }";
                } else if (scenario == 8) {
                    source = prefix + "{ " + right + " = " + left + "; } }";
                } else {
                    source = prefix + left + " = " + right + ".Take(); " + right + " = " + create_expr(type) + "; }";
                    should_fail = false;
                }
                const std::string test_name = "N" + number3(index++) + "_move_assignment_matrix_type" +
                    std::to_string(type_case) + "_scenario" + std::to_string(scenario);
                ownership_case(test_name, [source, should_fail] {
                    if (should_fail) expect_lifetime_error(source, "XLIFE0007");
                    else expect_lifetime_valid(source);
                }, passed, failed);
            }
        }
    }

    // Matrix O (100): copy passing mode is forbidden for Unique values, while
    // borrow, borrow mut, and take contracts remain admissible.
    {
        int index = 309;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                const std::string type = unique_type(type_case);
                const std::string parameter = scenario % 2 == 0 ? "value" : "VaLuE";
                std::string source;
                bool should_fail = true;
                if (scenario <= 5) {
                    source = "function Probe(copy " + type + " " + parameter + ") { PrintLine " + parameter + "; }";
                } else if (scenario == 6) {
                    source = "function Probe(borrow " + type + " " + parameter + ") { PrintLine " + parameter + "; }";
                    should_fail = false;
                } else if (scenario == 7) {
                    source = "function Probe(borrow mut " + type + " " + parameter + ") { PrintLine " + parameter + "; }";
                    should_fail = false;
                } else if (scenario == 8) {
                    source = "function Probe(take " + type + " " + parameter + ") { " + parameter + ".Take(); }";
                    should_fail = false;
                } else {
                    source = "function Probe(copy *Device " + parameter + ") { PrintLine " + parameter + "; }";
                    should_fail = false;
                }
                const std::string test_name = "O" + number3(index++) + "_copy_parameter_matrix_type" +
                    std::to_string(type_case) + "_mode" + std::to_string(scenario);
                ownership_case(test_name, [source, should_fail] {
                    if (should_fail) expect_lifetime_error(source, "XLIFE0008");
                    else expect_lifetime_valid(source);
                }, passed, failed);
            }
        }
    }

    // Matrix P (100): transfer through .Take() creates a fresh target resource,
    // while aliases of the transferred generation remain stale. Includes
    // destination initialization, assignment, nesting, and reinitialization.
    {
        int index = 409;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                const std::string type = unique_type(type_case);
                std::string source;
                bool should_fail = false;
                const std::string owner = scenario % 2 == 0 ? "owner" : "OwNeR";
                const std::string target = scenario % 3 == 0 ? "target" : "TaRgEt";
                if (scenario == 0) {
                    source = "function Probe() { " + type + " " + owner + " = " + create_expr(type) +
                             "; " + type + " " + target + " = " + owner + ".Take(); PrintLine " + target + ".Get(); }";
                } else if (scenario == 1) {
                    source = "function Probe() { " + type + " " + owner + " = " + create_expr(type) +
                             "; " + type + " " + target + " = " + create_expr(type) + "; " + target + " = " + owner + ".Take(); }";
                } else if (scenario == 2) {
                    source = "function Probe() { " + type + " " + owner + " = " + create_expr(type) +
                             "; alias = " + owner + ".Get(); " + type + " " + target + " = " + owner + ".Take(); PrintLine alias; }";
                    should_fail = true;
                } else if (scenario == 3) {
                    source = "function Probe() { " + type + " " + owner + " = " + create_expr(type) +
                             "; alias = " + owner + ".Get(); " + type + " " + target + " = " + create_expr(type) +
                             "; " + target + " = " + owner + ".Take(); PrintLine alias; }";
                    should_fail = true;
                } else if (scenario == 4) {
                    source = "function Probe() { " + type + " " + owner + " = " + create_expr(type) +
                             "; " + type + " " + target + " = " + owner + ".Take(); " + owner + " = " + create_expr(type) +
                             "; PrintLine " + owner + ".Get(); }";
                } else if (scenario == 5) {
                    source = "function Probe(bool flag) { " + type + " " + owner + " = " + create_expr(type) +
                             "; " + type + " " + target + " = " + create_expr(type) +
                             "; if (flag) { " + target + " = " + owner + ".Take(); } }";
                } else if (scenario == 6) {
                    source = "function Probe(bool flag) { " + type + " " + owner + " = " + create_expr(type) +
                             "; " + type + " " + target + " = " + create_expr(type) +
                             "; while (flag) { " + target + " = " + owner + ".Take(); break; } }";
                } else if (scenario == 7) {
                    source = "function Probe() { " + type + " " + owner + " = " + create_expr(type) +
                             "; " + type + " " + target + " = " + owner + ".Take(); " + target + " = " + target + ".Take(); }";
                } else if (scenario == 8) {
                    source = "function Probe() { " + type + " " + owner + " = " + create_expr(type) +
                             "; " + type + " " + target + " = " + create_expr(type) +
                             "; { " + target + " = " + owner + ".Take(); } PrintLine " + target + ".Get(); }";
                } else {
                    source = "function Probe() { " + type + " " + owner + " = " + create_expr(type) +
                             "; alias = " + owner + ".Get(); " + type + " " + target + " = " + owner + ".Take(); " +
                             "alias = null; PrintLine " + target + ".Get(); }";
                }
                const std::string test_name = "P" + number3(index++) + "_take_transfer_matrix_type" +
                    std::to_string(type_case) + "_scenario" + std::to_string(scenario);
                ownership_case(test_name, [source, should_fail] {
                    if (should_fail) expect_lifetime_error(source, "XLIFE0005");
                    else expect_lifetime_valid(source);
                }, passed, failed);
            }
        }
    }

    // Matrix Q (100): the CFG records ownership operations in source order,
    // preserves unique type and parameter-mode metadata, and connects transfer
    // events to their receiving owner variable when the syntax provides one.
    {
        int index = 509;
        for (int type_case = 0; type_case < 10; ++type_case) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                const std::string type = unique_type(type_case);
                std::string source;
                xfrontend::FlowOperationKind expected = xfrontend::FlowOperationKind::OwnerCreate;
                if (scenario == 0) {
                    source = "function Probe(take " + type + " owner) { PrintLine owner; }";
                    expected = xfrontend::FlowOperationKind::OwnerCreate;
                } else if (scenario == 1) {
                    source = "function Probe() { " + type + " owner = " + create_expr(type) + "; }";
                    expected = xfrontend::FlowOperationKind::OwnerCreate;
                } else if (scenario == 2) {
                    source = "function Probe() { " + type + " owner = " + create_expr(type) +
                             "; owner.Reset(); }";
                    expected = xfrontend::FlowOperationKind::OwnerReset;
                } else if (scenario == 3) {
                    source = "function Probe() { " + type + " owner = " + create_expr(type) +
                             "; owner.Release(); }";
                    expected = xfrontend::FlowOperationKind::OwnerReset;
                } else if (scenario == 4) {
                    source = "function Probe() { " + type + " owner = " + create_expr(type) +
                             "; owner.Take(); }";
                    expected = xfrontend::FlowOperationKind::OwnerTransfer;
                } else if (scenario == 5) {
                    source = "function Probe() { " + type + " owner = " + create_expr(type) +
                             "; " + type + " target = owner.Take(); }";
                    expected = xfrontend::FlowOperationKind::OwnerTransfer;
                } else if (scenario == 6) {
                    source = "function Probe() { " + type + " owner = " + create_expr(type) +
                             "; " + type + " target = " + create_expr(type) + "; target = owner.Take(); }";
                    expected = xfrontend::FlowOperationKind::OwnerTransfer;
                } else if (scenario == 7) {
                    source = "function Probe() { " + type + " owner = " + create_expr(type) +
                             "; " + type + " target = owner; }";
                    expected = xfrontend::FlowOperationKind::OwnerCopy;
                } else if (scenario == 8) {
                    source = "function Probe(bool flag) { " + type + " owner = " + create_expr(type) +
                             "; " + wrap_nested("owner.Reset();", type_case % 5) + " }";
                    expected = xfrontend::FlowOperationKind::OwnerReset;
                } else {
                    source = "function Probe(bool flag) { " + type + " owner = " + create_expr(type) +
                             "; while (flag) { owner.Take(); break; } }";
                    expected = xfrontend::FlowOperationKind::OwnerTransfer;
                }
                const std::string test_name = "Q" + number3(index++) + "_cfg_ownership_matrix_type" +
                    std::to_string(type_case) + "_event" + std::to_string(scenario);
                ownership_case(test_name, [source, expected, scenario, type] {
                    const auto parsed = parse_ownership_source(source);
                    const auto cfg = xfrontend::analyze_control_flow(parsed.unit);
                    own_require(!cfg.functions.empty(), "CFG function expected");
                    const auto& graph = cfg.functions.front();
                    own_require(graph_has_operation(graph, expected), "expected CFG ownership event missing");
                    bool has_unique_metadata = false;
                    for (const auto& variable : graph.variables) {
                        if (variable.is_unique_owner && variable.type_name == type) {
                            has_unique_metadata = true;
                            break;
                        }
                    }
                    own_require(has_unique_metadata, "Unique owner type metadata missing from CFG variable");
                    if (scenario == 5 || scenario == 6) {
                        const auto* transfer = graph_find_operation(graph, xfrontend::FlowOperationKind::OwnerTransfer);
                        own_require(transfer != nullptr &&
                                    transfer->related_variable_id != xfrontend::InvalidFlowVariableId,
                                    "transfer event must identify destination owner");
                    }
                    if (scenario == 7) {
                        const auto* copy = graph_find_operation(graph, xfrontend::FlowOperationKind::OwnerCopy);
                        own_require(copy != nullptr &&
                                    copy->related_variable_id != xfrontend::InvalidFlowVariableId,
                                    "copy event must identify source owner");
                    }
                }, passed, failed);
            }
        }
    }
}
