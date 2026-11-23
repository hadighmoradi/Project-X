#include "xfrontend/x64_location_transfer.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace xfrontend;

NativeRegisterLocation make_location(NativeRegisterLocationKind kind,
                                     std::uint8_t register_index = 0U,
                                     std::uint32_t slot = 0U) {
    NativeRegisterLocation value;
    value.kind = kind;
    value.register_index = register_index;
    value.spill_slot = slot;
    return value;
}

void write_text(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write report: " + path.string());
    output << value;
    if (!output) throw std::runtime_error("report write failed: " + path.string());
}

} // namespace

int main(int argc, char** argv) {
    using namespace xfrontend;
    try {
        if (argc != 2) {
            std::cerr << "usage: xfrontend_n113_g25_gp_xmm_edge_liveness <output-directory>\n";
            return 2;
        }
        const std::filesystem::path output_dir(argv[1]);
        std::filesystem::create_directories(output_dir);

        // A deliberately synthetic two-block contract. Value 1 is GP and moves
        // SpillSlot->HomeSlot; Value 2 is FP and remains live in XMM3 at both
        // Flow endpoints and the N111 active Emitter location.
        NativeRegisterFlowFunction function;
        function.function_name = "g25_synthetic_gp_xmm_edge";
        function.register_ir.entry_block = 0U;
        function.register_ir.values = {
            NativeRegisterValueInfo{1U, NativeRegisterClass::GeneralPurpose},
            NativeRegisterValueInfo{2U, NativeRegisterClass::FloatingPoint}};

        NativeRegisterBasicBlock predecessor;
        predecessor.id = 0U;
        predecessor.successors = {1U};
        NativeRegisterInstruction define;
        define.definitions = {1U, 2U};
        predecessor.instructions.push_back(define);

        NativeRegisterBasicBlock successor;
        successor.id = 1U;
        successor.predecessors = {0U};
        NativeRegisterInstruction use;
        use.uses = {1U, 2U};
        successor.instructions.push_back(use);

        function.register_ir.blocks = {predecessor, successor};
        function.edges = {FlowEdge{1U, 0U, 1U, FlowEdgeKind::IfTrue}};

        const auto liveness = compute_native_register_liveness(function.register_ir);
        if (!liveness.succeeded || !liveness.converged || liveness.blocks.size() != 2U ||
            liveness.blocks[0U].live_out.size() != 2U || liveness.blocks[1U].live_in.size() != 2U) {
            throw std::runtime_error("synthetic CFG liveness did not produce the exact two-value edge intersection");
        }

        NativeX64FlowEdgeLocationAuditPlan g4;
        g4.succeeded = true;
        g4.edges_audited = 1U;
        g4.live_values_audited = 2U;
        g4.flow_location_changes = 1U;

        NativeX64FlowEdgeValueAudit gp;
        gp.edge_id = 1U;
        gp.predecessor_block_id = 0U;
        gp.successor_block_id = 1U;
        gp.value_id = 1U;
        gp.register_class = NativeRegisterClass::GeneralPurpose;
        gp.predecessor_flow_location = make_location(NativeRegisterLocationKind::SpillSlot, 0U, 0U);
        gp.successor_flow_location = make_location(NativeRegisterLocationKind::HomeSlot, 0U, 0U);
        gp.emitter_active_location = make_location(NativeRegisterLocationKind::Register, 3U);
        gp.predecessor_uses_frame = true;
        gp.successor_uses_frame = true;
        gp.predecessor_frame_displacement = -8;
        gp.successor_frame_displacement = -16;
        gp.flow_location_changes = true;
        g4.values.push_back(gp);

        NativeX64FlowEdgeValueAudit fp;
        fp.edge_id = 1U;
        fp.predecessor_block_id = 0U;
        fp.successor_block_id = 1U;
        fp.value_id = 2U;
        fp.register_class = NativeRegisterClass::FloatingPoint;
        fp.predecessor_flow_location = make_location(NativeRegisterLocationKind::Register, 3U);
        fp.successor_flow_location = make_location(NativeRegisterLocationKind::Register, 3U);
        fp.emitter_active_location = make_location(NativeRegisterLocationKind::Register, 3U);
        fp.flow_location_changes = false;
        g4.values.push_back(fp);

        NativeX64FlowEdgeTransferCandidatePlan g6;
        g6.succeeded = true;
        g6.ready_for_machine_insertion = false;
        g6.flow_location_changes_considered = 1U;
        g6.candidate_copies = 1U;
        g6.candidate_edges = 1U;

        NativeX64FlowEdgeTransferCandidate candidate;
        candidate.edge_id = 1U;
        candidate.predecessor_block_id = 0U;
        candidate.successor_block_id = 1U;
        candidate.branch_anchor.function_index = 0U;
        candidate.branch_anchor.edge_id = 1U;
        candidate.branch_anchor.from_block_id = 0U;
        candidate.branch_anchor.to_block_id = 1U;
        candidate.branch_anchor.edge_kind = FlowEdgeKind::IfTrue;
        candidate.branch_anchor.encoding = X64FlowBranchEncoding::Fallthrough;
        candidate.branch_anchor.displacement_offset = InvalidNativeFlowOperationIndex;
        candidate.branch_anchor.instruction_offset = 4U;
        candidate.branch_anchor.target_offset = 12U;
        candidate.branch_anchor.function_code_size = 16U;
        candidate.copy = NativeRegisterParallelCopy{
            1U, gp.predecessor_flow_location, gp.successor_flow_location};
        candidate.emitter_active_location = gp.emitter_active_location;
        g6.candidates.push_back(candidate);

        const auto plan = audit_native_x64_gp_memory_cycle_xmm_expansion_liveness(
            function, liveness, g4, g6, {1U, 2U});
        if (!plan.succeeded || !plan.g4_live_sets_reconciled ||
            !plan.candidate_edge_identities_exact || plan.cfg_edges_reconciled != 1U ||
            plan.g4_live_values_reconciled != 2U || plan.candidate_edges_audited != 1U ||
            plan.gp_memory_expansion_edges != 1U || plan.edges.size() != 1U ||
            plan.edges.front().selected_xmm_register != 4U ||
            !plan.edges.front().xmm_expansion_selection_proven ||
            !plan.edges.front().xmm_candidates[0U].expression_reserved ||
            !plan.edges.front().xmm_candidates[1U].expression_reserved ||
            !plan.edges.front().xmm_candidates[2U].live_value_conflict ||
            plan.edges.front().xmm_candidates[2U].conflicting_value_ids != std::vector<std::uint32_t>{2U} ||
            !plan.edges.front().xmm_candidates[3U].available ||
            !plan.edges.front().xmm_candidates[4U].available ||
            plan.xmm_register_candidates_audited != 5U ||
            plan.xmm_register_candidates_dead != 2U ||
            plan.xmm_register_candidates_blocked != 3U ||
            plan.machine_insertion_ready || plan.all_required_edges_have_dead_xmm != true) {
            const std::string diagnostic = plan.diagnostics.empty()
                ? "none" : (plan.diagnostics.front().code + ": " + plan.diagnostics.front().message);
            throw std::runtime_error("G25 exact-edge XMM liveness probe failed: " + diagnostic);
        }

        const std::string json =
            "{\n"
            "  \"schema\": \"project-x.n113-g25-gp-xmm-edge-liveness.v1\",\n"
            "  \"fixture_kind\": \"synthetic_manual_g4_g6_endpoint_contract\",\n"
            "  \"passed\": true,\n"
            "  \"cfg_edges_reconciled\": 1,\n"
            "  \"g4_live_values_reconciled\": 2,\n"
            "  \"g6_candidate_edges_audited\": 1,\n"
            "  \"gp_memory_expansion_edges\": 1,\n"
            "  \"xmm_register_candidates_audited\": 5,\n"
            "  \"xmm_register_candidates_dead\": 2,\n"
            "  \"xmm_register_candidates_blocked\": 3,\n"
            "  \"selected_xmm_register\": 4,\n"
            "  \"xmm3_live_value_conflict\": true,\n"
            "  \"xmm3_conflicting_value_ids\": [2],\n"
            "  \"expression_reserved_xmm1_xmm2\": true,\n"
            "  \"gp_numeric_xmm_bank_distinct\": true,\n"
            "  \"product_source_evidence\": false,\n"
            "  \"cycle_spill_slot_reserved\": false,\n"
            "  \"frame_reservation\": false,\n"
            "  \"frame_unwind_mutation\": false,\n"
            "  \"edge_insertion\": false,\n"
            "  \"machine_insertion_ready\": false\n"
            "}\n";

        std::ostringstream text;
        text << "Schema: project-x.n113-g25-gp-xmm-edge-liveness.v1\n";
        text << "Fixture: SYNTHETIC_MANUAL_G4_G6_ENDPOINT_CONTRACT\n";
        text << "CFG edges exactly reconciled: 1\n";
        text << "G4 live values exactly reconciled: 2\n";
        text << "G6 candidate edges audited: 1\n";
        text << "GP memory-expansion edges: 1\n";
        text << "XMM1-XMM5 candidates: 5 | dead=2 | blocked=3\n";
        text << "XMM3 blocked by live FP Value 2 at predecessor/successor/N111 evidence: TRUE\n";
        text << "XMM1/XMM2 expression-reserved: TRUE\n";
        text << "Selected exact-edge expansion register: XMM4\n";
        text << "GP register index 3 and XMM3 are distinct physical banks: TRUE\n";
        text << "Product Source evidence: NOT_PROVEN\n";
        text << "Cycle SpillSlot reservation: NOT_PERFORMED\n";
        text << "Frame/Unwind mutation: NOT_PERFORMED\n";
        text << "Edge insertion: NOT_PERFORMED\n";
        text << "INSERTION_READY: FALSE\n";
        text << "RESULT: PASSED\n";

        write_text(output_dir / "N113_G25_GP_XMM_Edge_Liveness.json", json);
        write_text(output_dir / "N113_G25_GP_XMM_Edge_Liveness.txt", text.str());
        std::cout << text.str();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "N113-G25 diagnostic failed: " << error.what() << "\n";
        return 1;
    }
}
