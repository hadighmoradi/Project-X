#include "xfrontend/x64_location_transfer.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {
using namespace xfrontend;

NativeRegisterLocation location(const NativeRegisterLocationKind kind,
                                 const std::uint8_t reg = 0U,
                                 const std::uint32_t slot = 0U) {
    NativeRegisterLocation value;
    value.kind = kind;
    value.register_index = reg;
    value.spill_slot = slot;
    return value;
}

NativeX64FrameLocationMap frame_map() {
    NativeX64FrameLocationMap frame;
    frame.spill_slot_offsets.emplace(0U, -8);
    frame.spill_slot_offsets.emplace(1U, -24);
    frame.spill_slot_offsets.emplace(2U, -40);
    return frame;
}

std::vector<NativeRegisterParallelCopy> memory_cycle_copies() {
    return {
        NativeRegisterParallelCopy{1U,
            location(NativeRegisterLocationKind::SpillSlot, 0U, 0U),
            location(NativeRegisterLocationKind::SpillSlot, 0U, 1U)},
        NativeRegisterParallelCopy{2U,
            location(NativeRegisterLocationKind::SpillSlot, 0U, 1U),
            location(NativeRegisterLocationKind::SpillSlot, 0U, 0U)}
    };
}

std::string hex_bytes(const std::vector<std::uint8_t>& bytes) {
    std::ostringstream out;
    out << std::hex << std::uppercase << std::setfill('0');
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        if (index != 0U) out << ' ';
        out << std::setw(2) << static_cast<unsigned int>(bytes[index]);
    }
    return out.str();
}

bool write_file(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream) return false;
    stream << contents;
    return stream.good();
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Usage: xfrontend_n113_g22_memory_cycle_contract <output-directory>\n";
        return 2;
    }
    const std::filesystem::path output_directory(argv[1]);
    std::error_code filesystem_error;
    std::filesystem::create_directories(output_directory, filesystem_error);
    if (filesystem_error) {
        std::cerr << "N113_G22_OUTPUT | FAILED | " << filesystem_error.message() << '\n';
        return 1;
    }

    const NativeRegisterLocation cycle_slot =
        location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
    const NativeRegisterLocation expansion =
        location(NativeRegisterLocationKind::Temporary, 11U);
    const auto gp_schedule = resolve_native_register_parallel_copies(memory_cycle_copies(), cycle_slot);
    const auto xmm_schedule = resolve_native_register_parallel_copies(memory_cycle_copies(), cycle_slot);
    const auto frame = frame_map();
    const auto gp_encoding = encode_native_x64_parallel_copy_schedule_with_memory_cycle_temporary(
        gp_schedule.operations,
        {{1U, NativeRegisterClass::GeneralPurpose}, {2U, NativeRegisterClass::GeneralPurpose}},
        cycle_slot, expansion, frame);
    const auto xmm_encoding = encode_native_x64_parallel_copy_schedule_with_memory_cycle_temporary(
        xmm_schedule.operations,
        {{1U, NativeRegisterClass::FloatingPoint}, {2U, NativeRegisterClass::FloatingPoint}},
        cycle_slot, expansion, frame);

    auto bad_register_schedule = gp_schedule.operations;
    bad_register_schedule.push_back(NativeRegisterParallelCopyOperation{
        3U, location(NativeRegisterLocationKind::Register, 11U),
        location(NativeRegisterLocationKind::Register, 12U), false});
    const auto register_alias = encode_native_x64_parallel_copy_schedule_with_memory_cycle_temporary(
        bad_register_schedule,
        {{1U, NativeRegisterClass::GeneralPurpose}, {2U, NativeRegisterClass::GeneralPurpose},
         {3U, NativeRegisterClass::GeneralPurpose}},
        cycle_slot, expansion, frame);

    auto alias_frame = frame;
    alias_frame.home_slot_offsets.emplace(0U, -40);
    const auto frame_alias = encode_native_x64_parallel_copy_schedule_with_memory_cycle_temporary(
        gp_schedule.operations,
        {{1U, NativeRegisterClass::GeneralPurpose}, {2U, NativeRegisterClass::GeneralPurpose}},
        cycle_slot, expansion, alias_frame);

    const auto home_rejected = resolve_native_register_parallel_copies(
        memory_cycle_copies(), location(NativeRegisterLocationKind::HomeSlot, 0U, 7U));
    auto marker_mismatch_schedule = gp_schedule.operations;
    if (!marker_mismatch_schedule.empty()) marker_mismatch_schedule.front().uses_cycle_temporary = false;
    const auto marker_mismatch = encode_native_x64_parallel_copy_schedule_with_memory_cycle_temporary(
        marker_mismatch_schedule,
        {{1U, NativeRegisterClass::GeneralPurpose}, {2U, NativeRegisterClass::GeneralPurpose}},
        cycle_slot, expansion, frame);

    const bool passed = gp_schedule.succeeded && xmm_schedule.succeeded &&
        gp_schedule.cycle_temporaries_used == 1U && xmm_schedule.cycle_temporaries_used == 1U &&
        gp_schedule.operations.size() == 3U && xmm_schedule.operations.size() == 3U &&
        gp_encoding.succeeded && xmm_encoding.succeeded &&
        gp_encoding.memory_to_memory_expansions == 3U &&
        xmm_encoding.memory_to_memory_expansions == 3U &&
        gp_encoding.machine_moves_emitted == 6U && xmm_encoding.machine_moves_emitted == 6U &&
        !register_alias.succeeded && !register_alias.diagnostics.empty() &&
        register_alias.diagnostics.front().code == "N113G22E0006" &&
        !frame_alias.succeeded && !frame_alias.diagnostics.empty() &&
        frame_alias.diagnostics.front().code == "N113G22E0005" &&
        !home_rejected.succeeded && !home_rejected.diagnostics.empty() &&
        home_rejected.diagnostics.front().code == "N80E0001" &&
        !marker_mismatch.succeeded && !marker_mismatch.diagnostics.empty() &&
        marker_mismatch.diagnostics.front().code == "N113G22E0011";

    std::ostringstream text;
    text << "Project X N113-G22 memory-backed cycle temporary contract\n"
         << "Schema: project-x.n113-g22-memory-cycle-contract.v1\n"
         << "Fixture: synthetic isolated parallel-copy contract; not a product-source liveness proof\n"
         << "GP resolver: " << (gp_schedule.succeeded ? "PASS" : "FAIL")
         << " | operations=" << gp_schedule.operations.size()
         << " | cycles=" << gp_schedule.cycle_temporaries_used << '\n'
         << "XMM resolver: " << (xmm_schedule.succeeded ? "PASS" : "FAIL")
         << " | operations=" << xmm_schedule.operations.size()
         << " | cycles=" << xmm_schedule.cycle_temporaries_used << '\n'
         << "GP encoder: " << (gp_encoding.succeeded ? "PASS" : "FAIL")
         << " | memory-to-memory-expansions=" << gp_encoding.memory_to_memory_expansions
         << " | machine-moves=" << gp_encoding.machine_moves_emitted
         << " | bytes=" << gp_encoding.bytes.size() << '\n'
         << "GP bytes: " << hex_bytes(gp_encoding.bytes) << '\n'
         << "XMM encoder: " << (xmm_encoding.succeeded ? "PASS" : "FAIL")
         << " | memory-to-memory-expansions=" << xmm_encoding.memory_to_memory_expansions
         << " | machine-moves=" << xmm_encoding.machine_moves_emitted
         << " | bytes=" << xmm_encoding.bytes.size() << '\n'
         << "XMM bytes: " << hex_bytes(xmm_encoding.bytes) << '\n'
         << "Expansion-register alias rejected: " << (!register_alias.succeeded ? "TRUE" : "FALSE") << '\n'
         << "Physical frame-offset alias rejected: " << (!frame_alias.succeeded ? "TRUE" : "FALSE") << '\n'
         << "Canonical HomeSlot rejected as cycle scratch: " << (!home_rejected.succeeded ? "TRUE" : "FALSE") << '\n'
         << "Cycle-marker mismatch rejected: " << (!marker_mismatch.succeeded ? "TRUE" : "FALSE") << '\n'
         << "Frame reservation: NOT_PERFORMED\n"
         << "Frame/Unwind mutation: NOT_PERFORMED\n"
         << "Edge insertion: NOT_PERFORMED\n"
         << "Product-source edge liveness: NOT_PROVEN\n"
         << "INSERTION_READY: FALSE\n"
         << "RESULT: " << (passed ? "PASSED" : "FAILED") << '\n';

    std::ostringstream json;
    json << "{\n"
         << "  \"schema\": \"project-x.n113-g22-memory-cycle-contract.v1\",\n"
         << "  \"fixture_kind\": \"synthetic_isolated_parallel_copy_contract\",\n"
         << "  \"gp\": {\"resolver_passed\": " << (gp_schedule.succeeded ? "true" : "false")
         << ", \"operations\": " << gp_schedule.operations.size()
         << ", \"cycles\": " << gp_schedule.cycle_temporaries_used
         << ", \"encoder_passed\": " << (gp_encoding.succeeded ? "true" : "false")
         << ", \"memory_to_memory_expansions\": " << gp_encoding.memory_to_memory_expansions
         << ", \"machine_moves\": " << gp_encoding.machine_moves_emitted
         << ", \"byte_count\": " << gp_encoding.bytes.size() << "},\n"
         << "  \"xmm\": {\"resolver_passed\": " << (xmm_schedule.succeeded ? "true" : "false")
         << ", \"operations\": " << xmm_schedule.operations.size()
         << ", \"cycles\": " << xmm_schedule.cycle_temporaries_used
         << ", \"encoder_passed\": " << (xmm_encoding.succeeded ? "true" : "false")
         << ", \"memory_to_memory_expansions\": " << xmm_encoding.memory_to_memory_expansions
         << ", \"machine_moves\": " << xmm_encoding.machine_moves_emitted
         << ", \"byte_count\": " << xmm_encoding.bytes.size() << "},\n"
         << "  \"rejections\": {\"expansion_register_alias\": " << (!register_alias.succeeded ? "true" : "false")
         << ", \"frame_offset_alias\": " << (!frame_alias.succeeded ? "true" : "false")
         << ", \"home_slot_as_cycle_scratch\": " << (!home_rejected.succeeded ? "true" : "false")
         << ", \"cycle_marker_mismatch\": " << (!marker_mismatch.succeeded ? "true" : "false") << "},\n"
         << "  \"frame_reservation\": false,\n"
         << "  \"frame_unwind_mutation\": false,\n"
         << "  \"edge_insertion\": false,\n"
         << "  \"product_source_edge_liveness_proven\": false,\n"
         << "  \"insertion_ready\": false,\n"
         << "  \"passed\": " << (passed ? "true" : "false") << "\n"
         << "}\n";

    if (!write_file(output_directory / "N113_G22_Memory_Cycle_Contract.txt", text.str()) ||
        !write_file(output_directory / "N113_G22_Memory_Cycle_Contract.json", json.str())) {
        std::cerr << "N113_G22_OUTPUT | FAILED | cannot write TXT/JSON report\n";
        return 1;
    }
    if (!passed) {
        std::cerr << "N113_G22_CONTRACT | RESULT=FAILED\n";
        return 1;
    }
    std::cout << "N113_G22_MEMORY_CYCLE_CONTRACT | RESULT=PASSED"
              << " | GP_EXPANSIONS=" << gp_encoding.memory_to_memory_expansions
              << " | XMM_EXPANSIONS=" << xmm_encoding.memory_to_memory_expansions
              << " | GP_BYTES=" << gp_encoding.bytes.size()
              << " | XMM_BYTES=" << xmm_encoding.bytes.size()
              << " | REJECTION_GUARDS=4"
              << " | FRAME_UNWIND_INSERTION=NOT_PERFORMED\n";
    return 0;
}
