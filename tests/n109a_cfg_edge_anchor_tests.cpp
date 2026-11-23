#include "xfrontend/control_flow.hpp"
#include "xfrontend/native_cfg_bridge.hpp"
#include "xfrontend/x64_codegen.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace {
using namespace xfrontend;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void run_case(const std::string& id, const std::function<void()>& body,
              int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(id)) return;
    const auto started = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        xfrontend_test_report::record(id, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    } catch (const std::exception& error) {
        ++failed;
        xfrontend_test_report::record(id, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    }
}

std::string cfg_source(unsigned int variant) {
    const unsigned int split = 1U + variant % 5U;
    return "module N109A; public function Main(int limit, int flag) returns int { "
           "int i = 0; int total = 0; "
           "while (i < limit) { "
           "if (flag == " + std::to_string(split) + ") { total = total + i; } "
           "else { if (i == 2) { break; } else { total = total + 1; } } "
           "if (i == 3) { i = i + 1; continue; } "
           "i = i + 1; "
           "} "
           "if (total > 0) { return total; } else { return 42; } }";
}

ParseResult parse_valid(const std::string& source) {
    auto parsed = parse_source(source);
    require(parsed.succeeded(), parsed.diagnostics.empty()
        ? "N109A source parse failed without a diagnostic"
        : "N109A source parse failed: " + parsed.diagnostics.front().code + " " +
          parsed.diagnostics.front().message);
    return parsed;
}

const FlowControlStatement* find_control(const ControlFlowGraph& graph,
                                         FlowControlStatementId id) {
    const auto found = std::find_if(graph.control_statements.begin(), graph.control_statements.end(),
        [id](const FlowControlStatement& item) { return item.id == id; });
    return found == graph.control_statements.end() ? nullptr : &*found;
}

bool is_branch_anchor_edge(FlowEdgeKind kind) {
    switch (kind) {
    case FlowEdgeKind::IfTrue:
    case FlowEdgeKind::IfFalse:
    case FlowEdgeKind::IfThenMerge:
    case FlowEdgeKind::IfElseMerge:
    case FlowEdgeKind::LoopEntry:
    case FlowEdgeKind::LoopBody:
    case FlowEdgeKind::LoopExit:
    case FlowEdgeKind::LoopBack:
    case FlowEdgeKind::LoopBreak:
    case FlowEdgeKind::LoopContinue:
        return true;
    case FlowEdgeKind::Sequential:
    case FlowEdgeKind::Return:
    case FlowEdgeKind::ForEachEntry:
    case FlowEdgeKind::ForEachBody:
    case FlowEdgeKind::ForEachExit:
    case FlowEdgeKind::ForEachBack:
        return false;
    }
    return false;
}

void test_control_statement_ids_are_stable(unsigned int variant) {
    const auto parsed = parse_valid(cfg_source(variant));
    require(parsed.unit.functions.size() == 1U && parsed.unit.functions[0].body,
        "N109A fixture did not produce one function body");
    const auto first = enumerate_flow_control_statements(*parsed.unit.functions[0].body);
    const auto second = enumerate_flow_control_statements(*parsed.unit.functions[0].body);
    require(first.size() == second.size() && first.size() >= 6U,
        "N109A control statement traversal lost a control node");
    std::set<FlowControlStatementId> ids;
    for (std::size_t index = 0U; index < first.size(); ++index) {
        require(first[index].statement == second[index].statement &&
                first[index].id == second[index].id &&
                first[index].parent_construct_id == second[index].parent_construct_id,
            "N109A pre-order control identity is not deterministic");
        require(first[index].id != InvalidFlowControlStatementId && ids.insert(first[index].id).second,
            "N109A control statement ID is invalid or duplicated");
    }
}

void test_cfg_edges_have_stable_identity_and_valid_topology(unsigned int variant) {
    const auto parsed = parse_valid(cfg_source(variant));
    const auto first = analyze_control_flow(parsed.unit);
    const auto second = analyze_control_flow(parsed.unit);
    require(first.succeeded() && second.succeeded() && first.functions.size() == 1U,
        "N109A control-flow analysis failed");
    const auto& graph = first.functions.front();
    const auto& repeated = second.functions.front();
    require(graph.edges.size() == repeated.edges.size() && !graph.edges.empty(),
        "N109A repeated CFG build changed the edge count");
    std::set<FlowEdgeId> ids;
    std::set<std::pair<FlowBlockId, FlowBlockId>> endpoint_pairs;
    for (std::size_t index = 0U; index < graph.edges.size(); ++index) {
        const auto& edge = graph.edges[index];
        const auto& other = repeated.edges[index];
        require(edge.id == other.id && edge.from_block_id == other.from_block_id &&
                edge.to_block_id == other.to_block_id && edge.kind == other.kind &&
                edge.control_statement_id == other.control_statement_id &&
                edge.control_construct_id == other.control_construct_id,
            "N109A repeated CFG build changed a stable edge identity");
        require(edge.id != InvalidFlowEdgeId && ids.insert(edge.id).second,
            "N109A CFG edge ID is invalid or duplicated");
        require(edge.from_block_id < graph.blocks.size() && edge.to_block_id < graph.blocks.size(),
            "N109A edge references an out-of-range block");
        require(endpoint_pairs.insert({edge.from_block_id, edge.to_block_id}).second,
            "N109A edge endpoint pair is duplicated");
        const auto& from = graph.blocks[edge.from_block_id];
        const auto& to = graph.blocks[edge.to_block_id];
        require(std::find(from.successors.begin(), from.successors.end(), edge.to_block_id) != from.successors.end(),
            "N109A edge is missing from its source successor list");
        require(std::find(to.predecessors.begin(), to.predecessors.end(), edge.from_block_id) != to.predecessors.end(),
            "N109A edge is missing from its target predecessor list");
        if (edge.control_statement_id != InvalidFlowControlStatementId) {
            require(find_control(graph, edge.control_statement_id) != nullptr,
                "N109A edge references an unknown control statement");
        }
        if (edge.control_construct_id != InvalidFlowControlConstructId) {
            const auto* construct = find_control(graph, edge.control_construct_id);
            require(construct != nullptr &&
                    (construct->kind == StatementKind::If || construct->kind == StatementKind::While ||
                     construct->kind == StatementKind::ForEach),
                "N109A edge owner is not a control construct");
        }
    }
    std::size_t successor_count = 0U;
    for (const auto& block : graph.blocks) successor_count += block.successors.size();
    require(successor_count == graph.edges.size(),
        "N109A edge table does not cover each successor relation exactly once");
}

void test_break_continue_edges_bind_to_enclosing_loop(unsigned int variant) {
    const auto parsed = parse_valid(cfg_source(variant));
    const auto flow = analyze_control_flow(parsed.unit);
    require(flow.succeeded() && flow.functions.size() == 1U,
        "N109A flow analysis failed for break/continue fixture");
    const auto& graph = flow.functions.front();
    std::size_t breaks = 0U;
    std::size_t continues = 0U;
    for (const auto& edge : graph.edges) {
        if (edge.kind != FlowEdgeKind::LoopBreak && edge.kind != FlowEdgeKind::LoopContinue) continue;
        const auto* statement = find_control(graph, edge.control_statement_id);
        const auto* loop = find_control(graph, edge.control_construct_id);
        require(statement != nullptr && loop != nullptr && loop->kind == StatementKind::While,
            "N109A break/continue edge does not reference its enclosing while construct");
        if (edge.kind == FlowEdgeKind::LoopBreak) {
            require(statement->kind == StatementKind::Break, "N109A break edge has a non-break source node");
            ++breaks;
        } else {
            require(statement->kind == StatementKind::Continue, "N109A continue edge has a non-continue source node");
            ++continues;
        }
    }
    require(breaks >= 1U && continues >= 1U,
        "N109A fixture did not produce both break and continue edges");
}

void test_native_bridge_preserves_control_and_edge_identity(unsigned int variant) {
    const auto parsed = parse_valid(cfg_source(variant));
    const auto flow = analyze_control_flow(parsed.unit);
    const auto lowered = lower_control_flow_to_native_register_ir(parsed.unit);
    require(flow.succeeded() && lowered.succeeded && lowered.functions.size() == 1U,
        "N109A CFG-to-register bridge failed");
    const auto& source = flow.functions.front();
    const auto& target = lowered.functions.front();
    require(target.control_statements.size() == source.control_statements.size() &&
            target.edges.size() == source.edges.size(),
        "N109A bridge dropped control statements or CFG edges");
    for (std::size_t index = 0U; index < source.edges.size(); ++index) {
        const auto& left = source.edges[index];
        const auto& right = target.edges[index];
        require(left.id == right.id && left.from_block_id == right.from_block_id &&
                left.to_block_id == right.to_block_id && left.kind == right.kind &&
                left.control_statement_id == right.control_statement_id &&
                left.control_construct_id == right.control_construct_id,
            "N109A bridge changed edge identity or ownership metadata");
    }
}

void test_codegen_emits_one_anchor_for_every_control_edge(unsigned int variant) {
    const auto parsed = parse_valid(cfg_source(variant));
    const auto lowered = lower_control_flow_to_native_register_ir(parsed.unit);
    require(lowered.succeeded && lowered.functions.size() == 1U,
        "N109A source CFG lowering failed");
    const auto compiled = compile_x64_coff_object(parsed.unit,
        X64CodegenOptions{"n109a-edge-anchor", make_coff_x86_64_object_target()});
    require(compiled.succeeded(), compiled.diagnostics.empty()
        ? "N109A x64 code generation failed without a diagnostic"
        : "N109A x64 code generation failed: " + compiled.diagnostics.front().code + " " +
          compiled.diagnostics.front().message);
    const auto& function = lowered.functions.front();
    const std::size_t expected = static_cast<std::size_t>(std::count_if(
        function.edges.begin(), function.edges.end(), [](const FlowEdge& edge) {
            return is_branch_anchor_edge(edge.kind);
        }));
    require(compiled.flow_branch_anchors.size() == expected,
        "N109A emitter did not anchor every supported control edge exactly once");
    require(compiled.stats.native_cfg_branch_anchors_emitted == expected,
        "N109A branch-anchor statistics differ from the exported table");
    std::set<FlowEdgeId> seen;
    for (const auto& anchor : compiled.flow_branch_anchors) {
        require(anchor.function_index == 0U && anchor.edge_id != InvalidFlowEdgeId &&
                seen.insert(anchor.edge_id).second,
            "N109A branch anchor has an invalid function/edge identity or is duplicated");
        const auto edge = std::find_if(function.edges.begin(), function.edges.end(),
            [&](const FlowEdge& candidate) { return candidate.id == anchor.edge_id; });
        require(edge != function.edges.end(), "N109A branch anchor references an unknown CFG edge");
        require(anchor.from_block_id == edge->from_block_id && anchor.to_block_id == edge->to_block_id &&
                anchor.edge_kind == edge->kind &&
                anchor.control_statement_id == edge->control_statement_id &&
                anchor.control_construct_id == edge->control_construct_id,
            "N109A branch anchor metadata differs from its source CFG edge");
        require(anchor.function_code_size > 0U &&
                anchor.instruction_offset <= anchor.function_code_size &&
                anchor.target_offset <= anchor.function_code_size,
            "N109A branch anchor offset lies outside the final function code");
        if (anchor.encoding == X64FlowBranchEncoding::Fallthrough) {
            require(anchor.displacement_offset == InvalidNativeFlowOperationIndex &&
                    anchor.instruction_offset == anchor.target_offset,
                "N109A fallthrough anchor has invalid instruction/displacement metadata");
        } else if (anchor.encoding == X64FlowBranchEncoding::ConditionalRel32) {
            require(anchor.displacement_offset != InvalidNativeFlowOperationIndex &&
                    anchor.instruction_offset + 2U == anchor.displacement_offset &&
                    static_cast<std::uint64_t>(anchor.displacement_offset) + 4U <= anchor.function_code_size,
                "N109A conditional branch anchor has invalid rel32 boundaries");
        } else {
            require(anchor.displacement_offset != InvalidNativeFlowOperationIndex &&
                    anchor.instruction_offset + 1U == anchor.displacement_offset &&
                    static_cast<std::uint64_t>(anchor.displacement_offset) + 4U <= anchor.function_code_size,
                "N109A unconditional branch anchor has invalid rel32 boundaries");
        }
    }
    require(seen.size() == expected, "N109A branch-anchor edge coverage is incomplete");
}

void test_branch_anchors_are_deterministic() {
    const auto parsed = parse_valid(cfg_source(2U));
    const auto first = compile_x64_coff_object(parsed.unit,
        X64CodegenOptions{"n109a-deterministic", make_coff_x86_64_object_target()});
    const auto second = compile_x64_coff_object(parsed.unit,
        X64CodegenOptions{"n109a-deterministic", make_coff_x86_64_object_target()});
    require(first.succeeded() && second.succeeded(), "N109A repeated codegen failed");
    require(first.flow_branch_anchors.size() == second.flow_branch_anchors.size(),
        "N109A repeated codegen changed branch-anchor count");
    for (std::size_t index = 0U; index < first.flow_branch_anchors.size(); ++index) {
        const auto& left = first.flow_branch_anchors[index];
        const auto& right = second.flow_branch_anchors[index];
        require(left.edge_id == right.edge_id &&
                left.control_statement_id == right.control_statement_id &&
                left.control_construct_id == right.control_construct_id &&
                left.from_block_id == right.from_block_id && left.to_block_id == right.to_block_id &&
                left.edge_kind == right.edge_kind && left.encoding == right.encoding &&
                left.conditional_taken == right.conditional_taken &&
                left.instruction_offset == right.instruction_offset &&
                left.displacement_offset == right.displacement_offset &&
                left.target_offset == right.target_offset &&
                left.function_code_size == right.function_code_size,
            "N109A repeated codegen changed a deterministic branch anchor");
    }
}

std::string large_frame_control_source() {
    std::string source = "module N109ALarge; public function Main(int limit, int flag) returns int { int seed = flag; ";
    for (unsigned index = 0U; index < 520U; ++index)
        source += "int slot" + std::to_string(index) + " = " + std::to_string(index) + "; ";
    source += "int i = 0; while (i < limit) { if (flag == 0) { seed = seed + i; } else { seed = seed + 1; } i = i + 1; } return seed; }";
    return source;
}

void test_branch_anchor_offsets_follow_stack_probe_insertion() {
    const auto parsed = parse_valid(large_frame_control_source());
    const auto compiled = compile_x64_coff_object(parsed.unit,
        X64CodegenOptions{"n109a-stack-probe-edge-anchors", make_coff_x86_64_object_target()});
    require(compiled.succeeded(), compiled.diagnostics.empty()
        ? "N109A large-frame codegen failed without a diagnostic"
        : "N109A large-frame codegen failed: " + compiled.diagnostics.front().code + " " +
          compiled.diagnostics.front().message);
    require(compiled.stats.win64_stack_probe_functions == 1U,
        "N109A large-frame edge-anchor fixture did not activate Stack Probe");
    require(!compiled.flow_branch_anchors.empty(), "N109A large-frame codegen emitted no branch anchors");
    for (const auto& anchor : compiled.flow_branch_anchors) {
        require(anchor.instruction_offset >= 14U && anchor.target_offset >= 14U,
            "N109A branch anchor was not shifted after Stack Probe insertion");
        require(anchor.function_code_size > anchor.target_offset,
            "N109A post-probe branch target is outside final function code");
    }
}

void test_constant_pruned_control_edges_are_not_fabricated() {
    const auto parsed = parse_valid(
        "module N109AConstant; public function Main() returns int { "
        "int value = 0; if (true) { value = 7; } "
        "while (true) { value = value + 1; break; } return value; }");
    const auto flow = analyze_control_flow(parsed.unit);
    require(flow.succeeded() && flow.functions.size() == 1U,
        "N109A constant-condition CFG failed");
    const auto lowered = lower_control_flow_to_native_register_ir(parsed.unit);
    require(lowered.succeeded, "N109A constant-condition bridge failed");
    const auto compiled = compile_x64_coff_object(parsed.unit,
        X64CodegenOptions{"n109a-constant-pruned", make_coff_x86_64_object_target()});
    require(compiled.succeeded(), compiled.diagnostics.empty()
        ? "N109A constant-condition codegen failed without a diagnostic"
        : "N109A constant-condition codegen failed: " + compiled.diagnostics.front().code + " " +
          compiled.diagnostics.front().message);
    const auto& edges = lowered.functions.front().edges;
    const std::size_t expected = static_cast<std::size_t>(std::count_if(
        edges.begin(), edges.end(), [](const FlowEdge& edge) { return is_branch_anchor_edge(edge.kind); }));
    require(compiled.flow_branch_anchors.size() == expected,
        "N109A fabricated a machine anchor for a constant-pruned CFG edge or omitted a surviving edge");
}

} // namespace

void run_n109a_cfg_edge_anchor_tests(int& passed, int& failed) {
    for (unsigned int index = 0U; index < 6U; ++index) {
        run_case("N109AA_control_statement_identity_stable_" + std::to_string(index),
            [index]() { test_control_statement_ids_are_stable(index); }, passed, failed);
        run_case("N109AB_edge_identity_and_topology_" + std::to_string(index),
            [index]() { test_cfg_edges_have_stable_identity_and_valid_topology(index); }, passed, failed);
        run_case("N109AC_break_continue_bind_to_loop_" + std::to_string(index),
            [index]() { test_break_continue_edges_bind_to_enclosing_loop(index); }, passed, failed);
        run_case("N109AD_bridge_preserves_edge_identity_" + std::to_string(index),
            [index]() { test_native_bridge_preserves_control_and_edge_identity(index); }, passed, failed);
    }
    for (unsigned int index = 0U; index < 4U; ++index) {
        run_case("N109AE_machine_branch_anchor_coverage_" + std::to_string(index),
            [index]() { test_codegen_emits_one_anchor_for_every_control_edge(index); }, passed, failed);
    }
    run_case("N109AF_deterministic_branch_anchors",
        test_branch_anchors_are_deterministic, passed, failed);
    run_case("N109AG_offsets_after_stack_probe_insertion",
        test_branch_anchor_offsets_follow_stack_probe_insertion, passed, failed);
    run_case("N109AH_constant_pruned_edges_not_fabricated",
        test_constant_pruned_control_edges_are_not_fabricated, passed, failed);
}
