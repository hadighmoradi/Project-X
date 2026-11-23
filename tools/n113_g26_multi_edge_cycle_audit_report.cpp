#include "xfrontend/x64_location_transfer.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace xfrontend;

NativeRegisterLocation location(NativeRegisterLocationKind kind, std::uint8_t reg = 0U,
                                std::uint32_t slot = 0U) {
    NativeRegisterLocation value;
    value.kind = kind;
    value.register_index = reg;
    value.spill_slot = slot;
    return value;
}

void write_text(const std::filesystem::path& path, const std::string& value) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot write report: " + path.string());
    out << value;
    if (!out) throw std::runtime_error("report write failed: " + path.string());
}

std::vector<NativeRegisterParallelCopyOperation> one_cycle(NativeRegisterLocation slot) {
    const auto resolved = resolve_native_register_parallel_copies({
        NativeRegisterParallelCopy{1U, location(NativeRegisterLocationKind::Register, 8U),
                                   location(NativeRegisterLocationKind::Register, 9U)},
        NativeRegisterParallelCopy{2U, location(NativeRegisterLocationKind::Register, 9U),
                                   location(NativeRegisterLocationKind::Register, 8U)}
    }, slot);
    if (!resolved.succeeded || resolved.cycle_temporaries_used != 1U || resolved.operations.size() != 3U)
        throw std::runtime_error("G26 synthetic resolver did not produce a one-cycle save/restore schedule");
    return resolved.operations;
}
}

int main(int argc, char** argv) {
    using namespace xfrontend;
    try {
        if (argc != 2) {
            std::cerr << "usage: xfrontend_n113_g26_multi_edge_cycle_audit <output-directory>\n";
            return 2;
        }
        const std::filesystem::path out_dir(argv[1]);
        std::filesystem::create_directories(out_dir);

        // This is deliberately a synthetic G25 proof-contract input. It does
        // not claim Product Source provenance or active-frame reservation.
        NativeX64GpXmmExpansionLivenessPlan g25;
        g25.succeeded = true;
        g25.g4_live_sets_reconciled = true;
        g25.candidate_edge_identities_exact = true;
        g25.candidate_edges_audited = 2U;
        g25.gp_memory_expansion_edges = 2U;
        g25.edges_with_dead_expansion_register = 2U;
        g25.all_required_edges_have_dead_xmm = true;
        for (const FlowEdgeId id : {3U, 4U}) {
            NativeX64FlowEdgeGpXmmExpansionEvidence edge;
            edge.edge_id = id;
            edge.g4_live_set_complete = true;
            edge.gp_only_candidates = true;
            edge.gp_memory_to_memory_required = true;
            edge.xmm_expansion_selection_proven = true;
            edge.selected_xmm_register = 4U;
            g25.edges.push_back(edge);
        }

        const auto cycle_slot = location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
        const auto schedule = one_cycle(cycle_slot);
        auto two_cycles = schedule;
        auto second_cycle = schedule;
        for (auto& operation : second_cycle) operation.value_id += 10U;
        two_cycles.insert(two_cycles.end(), second_cycle.begin(), second_cycle.end());

        NativeX64FrameLocationMap frame;
        frame.spill_slot_offsets = {{0U, -8}, {1U, -24}, {2U, -40}};
        frame.home_slot_offsets = {{0U, -16}};
        const std::vector<NativeX64GpMemoryCycleEdgeScheduleInput> edge_schedules{
            NativeX64GpMemoryCycleEdgeScheduleInput{3U, two_cycles},
            NativeX64GpMemoryCycleEdgeScheduleInput{4U, schedule}};

        const auto plan = audit_native_x64_multi_edge_gp_memory_cycle_protocol(
            g25, edge_schedules, cycle_slot, frame);
        if (!plan.succeeded || !plan.g25_edge_coverage_exact ||
            !plan.ordered_cycle_protocol_valid || !plan.cycle_slot_frame_mapping_valid ||
            plan.edges_audited != 2U || plan.total_cycle_transactions_audited != 3U ||
            !plan.cycle_slot_reused_across_edges || !plan.cycle_slot_reused_across_transactions ||
            plan.distinct_xmm_registers_selected != 1U || plan.machine_insertion_ready) {
            const std::string detail = plan.diagnostics.empty() ? "no diagnostic" :
                plan.diagnostics.front().code + ": " + plan.diagnostics.front().message;
            throw std::runtime_error("G26 synthetic multi-edge/multi-cycle audit failed: " + detail);
        }

        std::ostringstream json;
        json << "{\n"
             << "  \"schema\": \"project-x.n113-g26-multi-edge-memory-cycle-audit.v1\",\n"
             << "  \"fixture_kind\": \"synthetic_g25_proof_contract_and_resolver_schedules\",\n"
             << "  \"passed\": true,\n"
             << "  \"edges_audited\": " << plan.edges_audited << ",\n"
             << "  \"operations_audited\": " << plan.total_operations_audited << ",\n"
             << "  \"cycle_transactions_audited\": " << plan.total_cycle_transactions_audited << ",\n"
             << "  \"cycle_slot_reused_across_edges\": " << (plan.cycle_slot_reused_across_edges ? "true" : "false") << ",\n"
             << "  \"cycle_slot_reused_across_transactions\": " << (plan.cycle_slot_reused_across_transactions ? "true" : "false") << ",\n"
             << "  \"selected_xmm_registers\": " << plan.distinct_xmm_registers_selected << ",\n"
             << "  \"selected_xmm_per_edge\": [4, 4],\n"
             << "  \"ordered_save_restore_protocol\": true,\n"
             << "  \"overwrite_before_restore_rejected_by_tests\": true,\n"
             << "  \"restore_before_save_rejected_by_tests\": true,\n"
             << "  \"value_identity_mismatch_rejected_by_tests\": true,\n"
             << "  \"product_source_evidence\": false,\n"
             << "  \"active_frame_reservation\": false,\n"
             << "  \"frame_unwind_mutation\": false,\n"
             << "  \"edge_insertion\": false,\n"
             << "  \"machine_insertion_ready\": false\n"
             << "}\n";

        std::ostringstream text;
        text << "Schema: project-x.n113-g26-multi-edge-memory-cycle-audit.v1\n";
        text << "Fixture: SYNTHETIC_G25_PROOF_CONTRACT_AND_RESOLVER_SCHEDULES\n";
        text << "Edges audited: " << plan.edges_audited << "\n";
        text << "Operations audited: " << plan.total_operations_audited << "\n";
        text << "Ordered cycle transactions: " << plan.total_cycle_transactions_audited << "\n";
        text << "Edge 3 cycles: 2 | Edge 4 cycles: 1\n";
        text << "Shared SpillSlot reused across exact edges: TRUE (each edge transaction closes before exit)\n";
        text << "Shared SpillSlot reused across sequential cycles: TRUE\n";
        text << "Selected XMM per edge: XMM4, XMM4 | distinct XMM registers: 1\n";
        text << "Overwrite-before-restore guard: TRUE\n";
        text << "Restore-before-save guard: TRUE\n";
        text << "Cycle ValueId match guard: TRUE\n";
        text << "Product Source evidence: NOT_PROVEN\n";
        text << "Active frame reservation: NOT_PERFORMED\n";
        text << "Frame/Unwind mutation: NOT_PERFORMED\n";
        text << "Edge insertion: NOT_PERFORMED\n";
        text << "INSERTION_READY: FALSE\n";
        text << "RESULT: PASSED\n";

        write_text(out_dir / "N113_G26_Multi_Edge_Cycle_Audit.json", json.str());
        write_text(out_dir / "N113_G26_Multi_Edge_Cycle_Audit.txt", text.str());
        std::cout << text.str();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "N113-G26 diagnostic failed: " << error.what() << "\n";
        return 1;
    }
}
