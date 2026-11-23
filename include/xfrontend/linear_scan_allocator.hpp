#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xfrontend {

enum class NativeRegisterClass { GeneralPurpose, FloatingPoint };
struct NativeLiveInterval {
    std::uint32_t value_id = 0U;
    std::uint32_t start = 0U;
    std::uint32_t end = 0U; // inclusive
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
    bool crosses_call = false;
};
struct NativeRegisterAssignment {
    std::uint32_t value_id = 0U;
    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose;
    bool spilled = false;
    std::uint8_t register_index = 0U;
    std::uint32_t spill_slot = 0U;
    bool requires_callee_save = false;
};
struct NativeRegisterInterferenceEdge {
    std::uint32_t first_value_id = 0U;
    std::uint32_t second_value_id = 0U;
};

struct NativeRegisterAllocationOptions {
    // When enabled, allocate from Win64 caller-saved scratch registers. Intervals
    // live across a call are spilled instead of being placed in volatile regs.
    bool prefer_caller_saved = false;
    bool spill_call_crossing = true;
    // Reserve XMM0 for Win64 floating-point results and expression return values.
    bool reserve_xmm0 = false;
    // After exhausting caller-saved scratch registers, permit Win64
    // nonvolatile registers. The code generator must emit matching saves and
    // restores for every selected nonvolatile register. Call-crossing values
    // still spill under spill_call_crossing=true.
    bool use_callee_saved_fallback = false;
    // Registers reserved by an earlier allocation domain (for example,
    // expression temporaries) and unavailable to this allocation pass.
    std::vector<std::uint8_t> reserved_gp_registers;
    std::vector<std::uint8_t> reserved_xmm_registers;

    NativeRegisterAllocationOptions() = default;
    NativeRegisterAllocationOptions(bool prefer_caller, bool spill_crossing,
                                    bool reserve_return_xmm, bool callee_fallback = false)
        : prefer_caller_saved(prefer_caller), spill_call_crossing(spill_crossing),
          reserve_xmm0(reserve_return_xmm), use_callee_saved_fallback(callee_fallback) {}
};

struct NativeRegisterAllocationStats {
    std::uint32_t general_purpose_register_assignments = 0U;
    std::uint32_t floating_point_register_assignments = 0U;
    std::uint32_t spilled_intervals = 0U;
    std::uint32_t register_evictions = 0U;
    std::uint32_t call_crossing_spills = 0U;
    // Number of spilled intervals assigned to a slot whose prior live range expired.
    std::uint32_t reused_spill_slots = 0U;
};

struct NativeRegisterAllocationResult {
    std::vector<NativeRegisterAssignment> assignments;
    std::uint32_t spill_slot_count = 0U;
    std::vector<std::uint8_t> used_callee_saved_gp;
    std::vector<std::uint8_t> used_callee_saved_xmm;
    NativeRegisterAllocationStats stats;
    bool succeeded = false;
};

// Deterministic inclusive-interval linear-scan allocator. The default policy
// uses nonvolatile Win64 GP/XMM registers. A caller-saved policy can instead
// use R8-R11 and XMM0-XMM5 (or XMM1-XMM5 when XMM0 is reserved), spilling
// call-crossing intervals when requested. Spill slots are globally colored
// across GP and XMM values and reused only when their inclusive live ranges do
// not overlap. The allocator reports callee-save requirements; callers remain
// responsible for emitting the corresponding ABI save/restore sequences.
NativeRegisterAllocationResult allocate_native_registers(
    const std::vector<NativeLiveInterval>& intervals,
    NativeRegisterAllocationOptions options = {});

// Graph-coloring allocator for CFG-derived interference. Unlike conservative
// linear intervals, values in disjoint branches can share a physical register.
// Spill slots are colored against the same graph so mutually exclusive spills
// can share storage safely.
NativeRegisterAllocationResult allocate_native_registers_with_interference(
    const std::vector<NativeLiveInterval>& intervals,
    const std::vector<NativeRegisterInterferenceEdge>& edges,
    NativeRegisterAllocationOptions options = {});

} // namespace xfrontend
