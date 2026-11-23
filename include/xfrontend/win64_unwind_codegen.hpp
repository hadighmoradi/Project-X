#pragma once

#include "xfrontend/pe64_dll.hpp"

#include <cstdint>
#include <vector>

namespace xfrontend {

struct Win64UnwindPlanResult {
    Pe64DllRuntimeFunction runtime_function;
    std::vector<std::uint8_t> encoded_unwind_info;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty() && !encoded_unwind_info.empty(); }
};

// Describes the standard N48/N49 frame prologue:
// push rbp; mov rbp,rsp; sub rsp, frame_bytes.
// The returned record can be placed into the final PE .pdata/.xdata table once
// the linker has assigned the function's final .text range.
Win64UnwindPlanResult build_win64_frame_unwind_plan(
    std::size_t begin_offset, std::size_t end_offset,
    std::uint32_t frame_bytes, bool frame_pointer = true);

struct Win64StackProbePlan {
    bool succeeded = false;
    std::uint32_t frame_bytes = 0U;
    std::uint32_t page_bytes = 4096U;
    std::vector<std::uint32_t> probe_offsets;
    std::uint32_t final_remainder_bytes = 0U;
    std::vector<Diagnostic> diagnostics;
};

// Plans page-by-page stack touches for large Win64 frames. The plan is a
// backend contract; this revision validates the sequence but does not yet
// splice the probe instructions into every FunctionEmitter prologue.
Win64StackProbePlan build_win64_stack_probe_plan(
    std::uint32_t frame_bytes, std::uint32_t page_bytes = 4096U);


} // namespace xfrontend
