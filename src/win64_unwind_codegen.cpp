// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/win64_unwind_codegen.hpp"

#include <algorithm>
#include <limits>

namespace xfrontend {
namespace {
void add_error(std::vector<Diagnostic>& diagnostics, const char* code, const char* message) {
    diagnostics.push_back(Diagnostic{code, message, {}});
}
void put16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
}
}

Win64UnwindPlanResult build_win64_frame_unwind_plan(
    std::size_t begin_offset, std::size_t end_offset,
    std::uint32_t frame_bytes, bool frame_pointer) {
    Win64UnwindPlanResult result;
    if (begin_offset >= end_offset) {
        add_error(result.diagnostics, "XN49U0001", "runtime function range must be non-empty");
        return result;
    }
    if ((frame_bytes & 7U) != 0U) {
        add_error(result.diagnostics, "XN49U0002", "Win64 stack allocation must be a multiple of eight bytes");
        return result;
    }
    if (frame_bytes > 0x7fffffffU) {
        add_error(result.diagnostics, "XN49U0003", "Win64 frame allocation exceeds the supported unwind range");
        return result;
    }
    auto& function = result.runtime_function;
    function.begin_offset = begin_offset;
    function.end_offset = end_offset;
    function.prolog_size = frame_pointer ? 11U : 7U;
    function.frame_register = frame_pointer ? 5U : 0U; // RBP
    function.frame_offset = 0U;
    if (frame_bytes != 0U) {
        if (frame_bytes <= 128U) {
            function.unwind_codes.push_back(Pe64DllUnwindCode{
                11U, Pe64DllUnwindOperation::AllocSmall,
                static_cast<std::uint8_t>((frame_bytes - 8U) / 8U), 0U});
        } else if (frame_bytes / 8U <= 0xffffU) {
            function.unwind_codes.push_back(Pe64DllUnwindCode{
                11U, Pe64DllUnwindOperation::AllocLarge, 0U, frame_bytes});
        } else {
            function.unwind_codes.push_back(Pe64DllUnwindCode{
                11U, Pe64DllUnwindOperation::AllocLarge, 1U, frame_bytes});
        }
    }
    if (frame_pointer) {
        function.unwind_codes.push_back(Pe64DllUnwindCode{
            4U, Pe64DllUnwindOperation::SetFramePointer, 0U, 0U});
        function.unwind_codes.push_back(Pe64DllUnwindCode{
            1U, Pe64DllUnwindOperation::PushNonVol, 5U, 0U});
    }

    // Encode UNWIND_INFO v1. Operations are already ordered by descending CodeOffset.
    std::vector<std::uint8_t> slots;
    std::uint8_t slot_count = 0U;
    for (const auto& code : function.unwind_codes) {
        slots.push_back(code.code_offset);
        slots.push_back(static_cast<std::uint8_t>((code.operation_info << 4U) |
            static_cast<std::uint8_t>(code.operation)));
        ++slot_count;
        if (code.operation == Pe64DllUnwindOperation::AllocLarge) {
            if (code.operation_info == 0U) {
                put16(slots, static_cast<std::uint16_t>(code.operand / 8U));
                ++slot_count;
            } else if (code.operation_info == 1U) {
                put16(slots, static_cast<std::uint16_t>(code.operand & 0xffffU));
                put16(slots, static_cast<std::uint16_t>((code.operand >> 16U) & 0xffffU));
                slot_count = static_cast<std::uint8_t>(slot_count + 2U);
            } else {
                add_error(result.diagnostics, "XN49U0004", "unsupported UWOP_ALLOC_LARGE operation info");
                return result;
            }
        }
    }
    result.encoded_unwind_info = {
        1U, function.prolog_size, slot_count,
        static_cast<std::uint8_t>(function.frame_register & 0x0fU)};
    result.encoded_unwind_info.insert(result.encoded_unwind_info.end(), slots.begin(), slots.end());
    if ((slot_count & 1U) != 0U) {
        result.encoded_unwind_info.push_back(0U);
        result.encoded_unwind_info.push_back(0U);
    }
    return result;
}


Win64StackProbePlan build_win64_stack_probe_plan(
    std::uint32_t frame_bytes, std::uint32_t page_bytes) {
    Win64StackProbePlan result;
    result.frame_bytes = frame_bytes;
    result.page_bytes = page_bytes;
    const auto add_error = [&result](const char* code, const char* message) {
        result.diagnostics.push_back(Diagnostic{code, message, {}});
    };
    if (page_bytes < 4096U || (page_bytes & (page_bytes - 1U)) != 0U) {
        add_error("XN106U0001", "stack probe page size must be a power of two of at least 4096 bytes");
        return result;
    }
    if ((frame_bytes & 7U) != 0U) {
        add_error("XN106U0002", "stack probe frame size must be eight-byte aligned");
        return result;
    }
    if (frame_bytes > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max())) {
        add_error("XN106U0003", "stack probe frame exceeds signed Win64 displacement capacity");
        return result;
    }
    std::uint32_t offset = 0U;
    while (frame_bytes - offset >= page_bytes) {
        offset += page_bytes;
        result.probe_offsets.push_back(offset);
    }
    result.final_remainder_bytes = frame_bytes - offset;
    result.succeeded = true;
    return result;
}

} // namespace xfrontend
