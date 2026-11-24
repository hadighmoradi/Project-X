// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/x64_location_transfer.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <stdexcept>
#include <vector>

namespace {
using namespace xfrontend;

NativeRegisterLocation location(NativeRegisterLocationKind kind, std::uint32_t reg,
                                std::uint32_t slot = 0U) {
    NativeRegisterLocation value;
    value.kind = kind;
    value.register_index = static_cast<std::uint8_t>(reg);
    value.spill_slot = slot;
    return value;
}

void write_text(const std::filesystem::path& path, const std::string& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write report: " + path.string());
    output << value;
    if (!output) throw std::runtime_error("report write failed: " + path.string());
}

std::string json_bool(bool value) { return value ? "true" : "false"; }

} // namespace

int main(int argc, char** argv) {
    using namespace xfrontend;
    try {
        if (argc != 2) {
            std::cerr << "usage: xfrontend_n113_g24_gp_xmm_expansion <output-directory>\n";
            return 2;
        }
        const std::filesystem::path output_dir(argv[1]);
        std::filesystem::create_directories(output_dir);

        NativeX64FrameLocationMap frame;
        frame.spill_slot_offsets.emplace(0U, -8);
        frame.spill_slot_offsets.emplace(1U, -24);
        frame.spill_slot_offsets.emplace(2U, -40);
        frame.spill_slot_offsets.emplace(3U, -48);

        const auto cycle_slot = location(NativeRegisterLocationKind::SpillSlot, 0U, 2U);
        const std::vector<NativeRegisterParallelCopy> copies{
            NativeRegisterParallelCopy{1U,
                location(NativeRegisterLocationKind::SpillSlot, 0U, 0U),
                location(NativeRegisterLocationKind::SpillSlot, 0U, 1U)},
            NativeRegisterParallelCopy{2U,
                location(NativeRegisterLocationKind::SpillSlot, 0U, 1U),
                location(NativeRegisterLocationKind::SpillSlot, 0U, 0U)}};
        const auto resolved = resolve_native_register_parallel_copies(copies, cycle_slot);
        if (!resolved.succeeded || resolved.operations.size() != 3U || resolved.cycle_temporaries_used != 1U)
            throw std::runtime_error("GP SpillSlot cycle resolver did not produce the expected 3-operation schedule");

        auto operations = resolved.operations;
        // RCX is a GP register; its numeric index does not alias XMM1.
        operations.push_back(NativeRegisterParallelCopyOperation{
            3U, location(NativeRegisterLocationKind::Register, 1U),
            location(NativeRegisterLocationKind::SpillSlot, 0U, 3U), false});
        const std::map<std::uint32_t, NativeRegisterClass> classes{
            {1U, NativeRegisterClass::GeneralPurpose},
            {2U, NativeRegisterClass::GeneralPurpose},
            {3U, NativeRegisterClass::GeneralPurpose}};

        const auto encoded = encode_native_x64_parallel_copy_schedule_with_gp_memory_cycle_xmm_expansion(
            operations, classes, cycle_slot,
            location(NativeRegisterLocationKind::Temporary, 1U), frame);
        const std::vector<std::uint8_t> expected{
            0xF3U, 0x0FU, 0x7EU, 0x8DU, 0xF8U, 0xFFU, 0xFFU, 0xFFU,
            0x66U, 0x0FU, 0xD6U, 0x8DU, 0xD8U, 0xFFU, 0xFFU, 0xFFU,
            0xF3U, 0x0FU, 0x7EU, 0x8DU, 0xE8U, 0xFFU, 0xFFU, 0xFFU,
            0x66U, 0x0FU, 0xD6U, 0x8DU, 0xF8U, 0xFFU, 0xFFU, 0xFFU,
            0xF3U, 0x0FU, 0x7EU, 0x8DU, 0xD8U, 0xFFU, 0xFFU, 0xFFU,
            0x66U, 0x0FU, 0xD6U, 0x8DU, 0xE8U, 0xFFU, 0xFFU, 0xFFU,
            0x48U, 0x89U, 0x8DU, 0xD0U, 0xFFU, 0xFFU, 0xFFU};
        const bool gp_pass = encoded.succeeded && encoded.bytes == expected &&
            encoded.machine_moves_emitted == 7U &&
            encoded.memory_to_memory_expansions == 3U &&
            encoded.temporary_register_moves == 6U;
        bool xmm_pool_pass = true;
        for (std::uint8_t xmm_index = 1U; xmm_index <= 5U; ++xmm_index) {
            const auto xmm_encoded = encode_native_x64_parallel_copy_schedule_with_gp_memory_cycle_xmm_expansion(
                operations, classes, cycle_slot,
                location(NativeRegisterLocationKind::Temporary, xmm_index), frame);
            auto expected_xmm = expected;
            const auto modrm = static_cast<std::uint8_t>(0x85U | (xmm_index << 3U));
            for (const auto index : {3U, 11U, 19U, 27U, 35U, 43U}) expected_xmm[index] = modrm;
            xmm_pool_pass = xmm_pool_pass && xmm_encoded.succeeded && xmm_encoded.bytes == expected_xmm &&
                xmm_encoded.machine_moves_emitted == 7U && xmm_encoded.memory_to_memory_expansions == 3U;
        }

        const auto reject_index = [&](std::uint8_t index) {
            return encode_native_x64_parallel_copy_schedule_with_gp_memory_cycle_xmm_expansion(
                resolved.operations,
                {{1U, NativeRegisterClass::GeneralPurpose}, {2U, NativeRegisterClass::GeneralPurpose}},
                cycle_slot, location(NativeRegisterLocationKind::Temporary, index), frame);
        };
        const auto xmm0 = reject_index(0U);
        const auto xmm6 = reject_index(6U);
        const auto xmm15 = reject_index(15U);
        const bool abi_pool_rejected = !xmm0.succeeded && !xmm6.succeeded && !xmm15.succeeded &&
            !xmm0.diagnostics.empty() && !xmm6.diagnostics.empty() && !xmm15.diagnostics.empty() &&
            xmm0.diagnostics.front().code == "N113G24E0001" &&
            xmm6.diagnostics.front().code == "N113G24E0001" &&
            xmm15.diagnostics.front().code == "N113G24E0001";

        const auto wrong_class = encode_native_x64_parallel_copy_schedule_with_gp_memory_cycle_xmm_expansion(
            resolved.operations,
            {{1U, NativeRegisterClass::FloatingPoint}, {2U, NativeRegisterClass::FloatingPoint}},
            cycle_slot, location(NativeRegisterLocationKind::Temporary, 1U), frame);
        const bool wrong_class_rejected = !wrong_class.succeeded && !wrong_class.diagnostics.empty() &&
            wrong_class.diagnostics.front().code == "N113G24E0002";

        const auto wrong_kind = encode_native_x64_parallel_copy_schedule_with_gp_memory_cycle_xmm_expansion(
            resolved.operations,
            {{1U, NativeRegisterClass::GeneralPurpose}, {2U, NativeRegisterClass::GeneralPurpose}},
            cycle_slot, location(NativeRegisterLocationKind::Register, 1U), frame);
        const bool wrong_kind_rejected = !wrong_kind.succeeded && !wrong_kind.diagnostics.empty() &&
            wrong_kind.diagnostics.front().code == "N113G24E0003";

        const bool passed = gp_pass && xmm_pool_pass && abi_pool_rejected && wrong_class_rejected && wrong_kind_rejected;
        const std::string json =
            "{\n"
            "  \"schema\": \"project-x.n113-g24-gp-xmm-expansion.v1\",\n"
            "  \"fixture_kind\": \"synthetic_isolated_parallel_copy_contract\",\n"
            "  \"gp\": {\"resolver_passed\": " + json_bool(resolved.succeeded) +
                ", \"operations\": " + std::to_string(resolved.operations.size()) +
                ", \"encoder_passed\": " + json_bool(gp_pass) +
                ", \"memory_to_memory_expansions\": " + std::to_string(encoded.memory_to_memory_expansions) +
                ", \"machine_moves\": " + std::to_string(encoded.machine_moves_emitted) +
                ", \"byte_count\": " + std::to_string(encoded.bytes.size()) +
                ", \"xmm_register\": 1, \"xmm1_to_xmm5_encoder_passed\": " + json_bool(xmm_pool_pass) +
                ", \"encoding\": \"MOVQ F3-0F-7E /r + 66-0F-D6 /r\"},\n"
            "  \"rejections\": {\"xmm0_fixed\": " + json_bool(!xmm0.succeeded) +
                ", \"xmm6_nonvolatile\": " + json_bool(!xmm6.succeeded) +
                ", \"xmm15_nonvolatile\": " + json_bool(!xmm15.succeeded) +
                ", \"wrong_register_class\": " + json_bool(wrong_class_rejected) +
                ", \"non_temporary_expansion\": " + json_bool(wrong_kind_rejected) + "},\n"
            "  \"product_source_edge_liveness_proven\": false,\n"
            "  \"g9_xmm_deadness_required\": true,\n"
            "  \"frame_reservation\": false,\n"
            "  \"frame_unwind_mutation\": false,\n"
            "  \"edge_insertion\": false,\n"
            "  \"insertion_ready\": false,\n"
            "  \"passed\": " + json_bool(passed) + "\n"
            "}\n";
        std::string report =
            "Project X N113-G24 — GP memory cycle with XMM MOVQ expansion\n"
            "Schema: project-x.n113-g24-gp-xmm-expansion.v1\n"
            "Fixture: SYNTHETIC_ISOLATED_PARALLEL_COPY_CONTRACT\n"
            "Resolver operations: " + std::to_string(resolved.operations.size()) + "\n"
            "GP XMM MOVQ expansion: " + std::string(gp_pass ? "PASS" : "FAIL") +
                " | expansions=" + std::to_string(encoded.memory_to_memory_expansions) +
                " | moves=" + std::to_string(encoded.machine_moves_emitted) +
                " | bytes=" + std::to_string(encoded.bytes.size()) + "\n"
            "Encoding: MOVQ xmm,m64 (F3 0F 7E /r); MOVQ m64,xmm (66 0F D6 /r)\n"
            "Cross-bank equal numeric register index: GP register index 1 (RCX) and XMM1 treated as distinct\n"
            "XMM1-XMM5 expansion encoding: " + std::string(xmm_pool_pass ? "PASS" : "FAIL") + "\n"
            "XMM0 fixed temporary rejected: " + std::string(!xmm0.succeeded ? "TRUE" : "FALSE") + "\n"
            "XMM6-XMM15 nonvolatile temporaries rejected: " + std::string((!xmm6.succeeded && !xmm15.succeeded) ? "TRUE" : "FALSE") + "\n"
            "Wrong register class rejected: " + std::string(wrong_class_rejected ? "TRUE" : "FALSE") + "\n"
            "Non-Temporary expansion operand rejected: " + std::string(wrong_kind_rejected ? "TRUE" : "FALSE") + "\n"
            "G9 exact-edge XMM deadness proof: REQUIRED, NOT_PROVEN_BY_THIS_FIXTURE\n"
            "Frame reservation: NOT_PERFORMED\n"
            "Frame/Unwind mutation: NOT_PERFORMED\n"
            "Edge insertion: NOT_PERFORMED\n"
            "INSERTION_READY: FALSE\n"
            "RESULT: " + std::string(passed ? "PASSED" : "FAILED") + "\n";
        write_text(output_dir / "N113_G24_GP_XMM_Expansion.json", json);
        write_text(output_dir / "N113_G24_GP_XMM_Expansion.txt", report);
        std::cout << report;
        return passed ? 0 : 1;
    } catch (const std::exception& ex) {
        std::cerr << "N113-G24 report failed: " << ex.what() << "\n";
        return 1;
    }
}
