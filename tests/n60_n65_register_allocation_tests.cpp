#include "xfrontend/linear_scan_allocator.hpp"
#include "xfrontend/parser.hpp"
#include "xfrontend/x64_codegen.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace xfrontend;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void run_case(const std::string& id, const std::function<void()>& body, int& passed, int& failed) {
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

const NativeRegisterAssignment& assignment_for(
    const NativeRegisterAllocationResult& result, std::uint32_t value_id) {
    const auto found = std::find_if(result.assignments.begin(), result.assignments.end(),
        [value_id](const NativeRegisterAssignment& item) { return item.value_id == value_id; });
    if (found == result.assignments.end()) throw std::runtime_error("allocator omitted a value ID");
    return *found;
}

bool same_assignments(const NativeRegisterAllocationResult& left,
                      const NativeRegisterAllocationResult& right) {
    if (left.succeeded != right.succeeded || left.spill_slot_count != right.spill_slot_count ||
        left.assignments.size() != right.assignments.size()) return false;
    for (std::size_t index = 0U; index < left.assignments.size(); ++index) {
        const auto& a = left.assignments[index];
        const auto& b = right.assignments[index];
        if (a.value_id != b.value_id || a.register_class != b.register_class ||
            a.spilled != b.spilled || a.register_index != b.register_index ||
            a.spill_slot != b.spill_slot || a.requires_callee_save != b.requires_callee_save) return false;
    }
    return true;
}

std::vector<NativeLiveInterval> make_pressure_wave(std::uint32_t first_id,
                                                    std::uint32_t start,
                                                    std::uint32_t end,
                                                    std::size_t count,
                                                    NativeRegisterClass register_class = NativeRegisterClass::GeneralPurpose) {
    std::vector<NativeLiveInterval> intervals;
    intervals.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        intervals.push_back(NativeLiveInterval{first_id + static_cast<std::uint32_t>(index),
            start, end, register_class, false});
    }
    return intervals;
}

void test_n60_disjoint_spill_waves_reuse_slots(std::size_t index) {
    auto intervals = make_pressure_wave(100U, 0U, 10U, 6U);
    auto second = make_pressure_wave(200U, 11U, 20U, 6U);
    intervals.insert(intervals.end(), second.begin(), second.end());
    const auto result = allocate_native_registers(intervals, NativeRegisterAllocationOptions{true, true, true});
    require(result.succeeded && result.assignments.size() == 12U, "N60 spill-wave allocation failed");
    require(result.stats.spilled_intervals == 4U, "N60 expected two spills in each pressure wave");
    require(result.spill_slot_count == 2U, "N60 disjoint spill waves did not reuse the same two stack slots");
    require(result.stats.reused_spill_slots == 2U, "N60 spill-slot reuse counter is incorrect");
    require(assignment_for(result, 100U + static_cast<std::uint32_t>(index % 2U)).spilled == false,
            "N60 low-ID register candidate was unexpectedly spilled");
}

void test_n60_overlapping_spills_do_not_alias(std::size_t index) {
    const auto intervals = make_pressure_wave(300U + static_cast<std::uint32_t>(index * 8U),
        0U, 100U, 8U);
    const auto result = allocate_native_registers(intervals, NativeRegisterAllocationOptions{true, true, true});
    require(result.succeeded && result.stats.spilled_intervals == 4U,
            "N60 overlapping pressure did not spill four values with four volatile GP registers");
    require(result.spill_slot_count == 4U && result.stats.reused_spill_slots == 0U,
            "N60 overlapping spilled live ranges incorrectly shared stack storage");
    std::set<std::uint32_t> slots;
    for (const auto& assignment : result.assignments)
        if (assignment.spilled) slots.insert(assignment.spill_slot);
    require(slots.size() == 4U, "N60 overlapping spill assignments reused a stack slot");
}

void test_n60_call_crossing_spills_reuse_after_expiry(std::size_t index) {
    const auto id = static_cast<std::uint32_t>(5000U + index * 2U);
    const auto result = allocate_native_registers({
        NativeLiveInterval{id, 0U, 4U, NativeRegisterClass::GeneralPurpose, true},
        NativeLiveInterval{id + 1U, 5U, 9U, NativeRegisterClass::GeneralPurpose, true}},
        NativeRegisterAllocationOptions{true, true, true});
    require(result.succeeded && result.stats.call_crossing_spills == 2U,
            "N60 call-crossing values were not both identified as forced spills");
    require(result.spill_slot_count == 1U && result.stats.reused_spill_slots == 1U,
            "N60 non-overlapping call-crossing spills did not share one slot");
    require(assignment_for(result, id).spill_slot == assignment_for(result, id + 1U).spill_slot,
            "N60 sequential call-crossing spills have different slot IDs");
}

void test_n60_spill_statistics_match_assignments(std::size_t index) {
    const auto intervals = make_pressure_wave(6000U + static_cast<std::uint32_t>(index * 6U),
        0U, 10U, 6U, NativeRegisterClass::FloatingPoint);
    const auto result = allocate_native_registers(intervals, NativeRegisterAllocationOptions{true, true, true});
    const auto spilled = static_cast<std::uint32_t>(std::count_if(result.assignments.begin(), result.assignments.end(),
        [](const NativeRegisterAssignment& item) { return item.spilled; }));
    require(result.succeeded && spilled == result.stats.spilled_intervals,
            "N60 spilled-interval statistic differs from the returned assignments");
    require(result.stats.floating_point_register_assignments + spilled == 6U,
            "N60 FP register/spill counters do not sum to the input count");
}

void test_n61_farthest_end_interval_is_evicted(std::size_t index) {
    const std::uint32_t base = 10000U + static_cast<std::uint32_t>(index * 8U);
    const auto result = allocate_native_registers({
        NativeLiveInterval{base, 0U, 100U, NativeRegisterClass::GeneralPurpose, false},
        NativeLiveInterval{base + 1U, 1U, 80U, NativeRegisterClass::GeneralPurpose, false},
        NativeLiveInterval{base + 2U, 2U, 60U, NativeRegisterClass::GeneralPurpose, false},
        NativeLiveInterval{base + 3U, 3U, 40U, NativeRegisterClass::GeneralPurpose, false},
        NativeLiveInterval{base + 4U, 4U, 20U, NativeRegisterClass::GeneralPurpose, false}},
        NativeRegisterAllocationOptions{true, true, true});
    require(result.succeeded && result.stats.register_evictions == 1U,
            "N61 allocator did not record the expected register eviction");
    require(assignment_for(result, base).spilled,
            "N61 allocator did not spill the active value with the farthest live-range end");
    require(!assignment_for(result, base + 4U).spilled,
            "N61 shorter incoming interval was spilled instead of evicting the farthest interval");
}

void test_n61_eviction_tie_break_is_stable(std::size_t index) {
    const std::uint32_t base = 12000U + static_cast<std::uint32_t>(index * 8U);
    const std::vector<NativeLiveInterval> intervals{
        {base + 5U, 0U, 100U, NativeRegisterClass::GeneralPurpose, false},
        {base + 4U, 0U, 100U, NativeRegisterClass::GeneralPurpose, false},
        {base + 3U, 0U, 100U, NativeRegisterClass::GeneralPurpose, false},
        {base + 2U, 0U, 100U, NativeRegisterClass::GeneralPurpose, false},
        {base + 1U, 1U, 50U, NativeRegisterClass::GeneralPurpose, false}};
    const auto first = allocate_native_registers(intervals, NativeRegisterAllocationOptions{true, true, true});
    const auto second = allocate_native_registers(intervals, NativeRegisterAllocationOptions{true, true, true});
    require(first.succeeded && same_assignments(first, second),
            "N61 equal-end eviction is not deterministic");
    require(assignment_for(first, base + 5U).spilled,
            "N61 equal-end tie did not choose the highest value ID as deterministic victim");
}

void test_n61_inclusive_endpoint_remains_occupied(std::size_t index) {
    const auto first = static_cast<std::uint32_t>(14000U + index * 4U);
    const auto overlap = allocate_native_registers({
        {first, 0U, 2U, NativeRegisterClass::GeneralPurpose, false},
        {first + 1U, 2U, 5U, NativeRegisterClass::GeneralPurpose, false}},
        NativeRegisterAllocationOptions{true, true, true});
    require(overlap.succeeded && assignment_for(overlap, first).register_index !=
            assignment_for(overlap, first + 1U).register_index,
            "N61 inclusive endpoint overlap incorrectly reused a physical register");
}

void test_n61_register_reused_only_after_endpoint(std::size_t index) {
    const auto first = static_cast<std::uint32_t>(16000U + index * 4U);
    const auto disjoint = allocate_native_registers({
        {first, 0U, 2U, NativeRegisterClass::GeneralPurpose, false},
        {first + 1U, 3U, 5U, NativeRegisterClass::GeneralPurpose, false}},
        NativeRegisterAllocationOptions{true, true, true});
    require(disjoint.succeeded && assignment_for(disjoint, first).register_index ==
            assignment_for(disjoint, first + 1U).register_index,
            "N61 expired register was not reused at the next non-overlapping position");
}

void test_n62_caller_saved_register_pools(std::size_t index) {
    const auto base = static_cast<std::uint32_t>(18000U + index * 8U);
    const auto result = allocate_native_registers({
        {base, 0U, 20U, NativeRegisterClass::GeneralPurpose, false},
        {base + 1U, 0U, 20U, NativeRegisterClass::FloatingPoint, false}},
        NativeRegisterAllocationOptions{true, true, true});
    require(result.succeeded && !assignment_for(result, base).spilled &&
            !assignment_for(result, base + 1U).spilled, "N62 low-pressure caller-saved allocation spilled");
    require(assignment_for(result, base).register_index == 8U,
            "N62 GP caller-saved pool did not start at R8");
    require(assignment_for(result, base + 1U).register_index == 1U,
            "N62 reserved-XMM0 policy did not start at XMM1");
    require(!assignment_for(result, base).requires_callee_save &&
            !assignment_for(result, base + 1U).requires_callee_save,
            "N62 volatile allocation incorrectly requested callee-save operations");
}

void test_n62_nonvolatile_register_plan_flags_preservation(std::size_t index) {
    const auto base = static_cast<std::uint32_t>(20000U + index * 8U);
    const auto result = allocate_native_registers({
        {base, 0U, 5U, NativeRegisterClass::GeneralPurpose, false},
        {base + 1U, 0U, 5U, NativeRegisterClass::FloatingPoint, false}},
        NativeRegisterAllocationOptions{false, true, false});
    require(result.succeeded && assignment_for(result, base).register_index == 3U &&
            assignment_for(result, base + 1U).register_index == 6U,
            "N62 nonvolatile register pools do not match the Win64 allocation policy");
    require(assignment_for(result, base).requires_callee_save &&
            assignment_for(result, base + 1U).requires_callee_save,
            "N62 nonvolatile allocation failed to report callee-save requirements");
    require(result.used_callee_saved_gp.size() == 1U && result.used_callee_saved_gp.front() == 3U &&
            result.used_callee_saved_xmm.size() == 1U && result.used_callee_saved_xmm.front() == 6U,
            "N62 used-callee-saved register summary is incomplete");
}

void test_n62_gp_and_xmm_call_crossings_spill(std::size_t index) {
    const auto base = static_cast<std::uint32_t>(22000U + index * 8U);
    const auto result = allocate_native_registers({
        {base, 0U, 20U, NativeRegisterClass::GeneralPurpose, true},
        {base + 1U, 0U, 20U, NativeRegisterClass::FloatingPoint, true}},
        NativeRegisterAllocationOptions{true, true, true});
    require(result.succeeded && result.stats.call_crossing_spills == 2U &&
            assignment_for(result, base).spilled && assignment_for(result, base + 1U).spilled,
            "N62 caller-saved call-crossing GP/XMM values were not both spilled");
    require(result.spill_slot_count == 2U,
            "N62 simultaneously live GP and XMM call-crossing values aliased one spill slot");
}

void test_n62_call_crossing_policy_can_be_overridden(std::size_t index) {
    const auto base = static_cast<std::uint32_t>(24000U + index * 4U);
    const auto result = allocate_native_registers({
        {base, 0U, 10U, NativeRegisterClass::GeneralPurpose, true}},
        NativeRegisterAllocationOptions{true, false, true});
    require(result.succeeded && !assignment_for(result, base).spilled,
            "N62 explicit spill_call_crossing=false override was ignored");
    require(result.stats.call_crossing_spills == 0U,
            "N62 override still counted a forced call-crossing spill");
}

void test_n63_reversed_interval_is_rejected(std::size_t index) {
    const auto result = allocate_native_registers({
        {static_cast<std::uint32_t>(26000U + index), 9U, 8U, NativeRegisterClass::GeneralPurpose, false}},
        NativeRegisterAllocationOptions{true, true, true});
    require(!result.succeeded && result.assignments.empty(),
            "N63 reversed live interval was not rejected cleanly");
}

void test_n63_duplicate_value_id_is_rejected(std::size_t index) {
    const auto id = static_cast<std::uint32_t>(28000U + index);
    const auto result = allocate_native_registers({
        {id, 0U, 4U, NativeRegisterClass::GeneralPurpose, false},
        {id, 5U, 8U, NativeRegisterClass::FloatingPoint, false}},
        NativeRegisterAllocationOptions{true, true, true});
    require(!result.succeeded && result.assignments.empty(),
            "N63 duplicate value IDs were not rejected before allocation");
}

void test_n63_invalid_register_class_and_empty_input(std::size_t index) {
    (void)index;
    const auto invalid = allocate_native_registers({
        {30000U, 0U, 1U, static_cast<NativeRegisterClass>(99), false}},
        NativeRegisterAllocationOptions{true, true, true});
    const auto empty = allocate_native_registers({});
    require(!invalid.succeeded && invalid.assignments.empty(),
            "N63 unknown register class was not rejected");
    require(empty.succeeded && empty.assignments.empty() && empty.spill_slot_count == 0U,
            "N63 empty allocation should be a successful empty result");
}

void test_n63_input_permutation_is_deterministic(std::size_t index) {
    const auto base = static_cast<std::uint32_t>(32000U + index * 8U);
    auto intervals = make_pressure_wave(base, 0U, 30U, 7U);
    intervals.push_back({base + 7U, 3U, 12U, NativeRegisterClass::GeneralPurpose, false});
    const auto forward = allocate_native_registers(intervals, NativeRegisterAllocationOptions{true, true, true});
    std::reverse(intervals.begin(), intervals.end());
    const auto backward = allocate_native_registers(intervals, NativeRegisterAllocationOptions{true, true, true});
    require(forward.succeeded && backward.succeeded && same_assignments(forward, backward),
            "N63 caller input order changed deterministic register or spill assignments");
}

void test_n64_gp_and_xmm_use_independent_register_pools(std::size_t index) {
    const auto base = static_cast<std::uint32_t>(34000U + index * 8U);
    std::vector<NativeLiveInterval> intervals{
        {base, 0U, 10U, NativeRegisterClass::GeneralPurpose, false},
        {base + 1U, 0U, 10U, NativeRegisterClass::FloatingPoint, false}};
    const auto result = allocate_native_registers(intervals, NativeRegisterAllocationOptions{true, true, true});
    require(result.succeeded && assignment_for(result, base).register_index == 8U &&
            assignment_for(result, base + 1U).register_index == 1U,
            "N64 GP and XMM pools were not allocated independently");
}

void test_n64_sequential_gp_and_xmm_spills_share_slot(std::size_t index) {
    const auto base = static_cast<std::uint32_t>(36000U + index * 16U);
    auto intervals = make_pressure_wave(base, 0U, 5U, 5U, NativeRegisterClass::GeneralPurpose);
    auto fp = make_pressure_wave(base + 8U, 6U, 10U, 6U, NativeRegisterClass::FloatingPoint);
    intervals.insert(intervals.end(), fp.begin(), fp.end());
    const auto result = allocate_native_registers(intervals, NativeRegisterAllocationOptions{true, true, true});
    require(result.succeeded && result.stats.spilled_intervals == 2U,
            "N64 expected one GP and one XMM spill in disjoint waves");
    require(result.spill_slot_count == 1U && result.stats.reused_spill_slots == 1U,
            "N64 GP and XMM spills with disjoint live ranges did not share one stack slot");
    require(assignment_for(result, base + 4U).spill_slot == assignment_for(result, base + 13U).spill_slot,
            "N64 cross-class spill slot identity was not reused");
}

void test_n64_overlapping_gp_and_xmm_spills_are_distinct(std::size_t index) {
    const auto base = static_cast<std::uint32_t>(38000U + index * 24U);
    auto intervals = make_pressure_wave(base, 0U, 10U, 5U, NativeRegisterClass::GeneralPurpose);
    auto fp = make_pressure_wave(base + 8U, 0U, 10U, 6U, NativeRegisterClass::FloatingPoint);
    intervals.insert(intervals.end(), fp.begin(), fp.end());
    const auto result = allocate_native_registers(intervals, NativeRegisterAllocationOptions{true, true, true});
    require(result.succeeded && result.stats.spilled_intervals == 2U,
            "N64 mixed class pressure did not produce one spill per class");
    require(result.spill_slot_count == 2U && result.stats.reused_spill_slots == 0U,
            "N64 simultaneously live GP/XMM spills shared an unsafe stack slot");
}

void test_n64_mixed_classes_preserve_assignment_class(std::size_t index) {
    const auto base = static_cast<std::uint32_t>(40000U + index * 8U);
    const auto result = allocate_native_registers({
        {base, 0U, 4U, NativeRegisterClass::GeneralPurpose, false},
        {base + 1U, 0U, 4U, NativeRegisterClass::FloatingPoint, false},
        {base + 2U, 5U, 9U, NativeRegisterClass::FloatingPoint, false},
        {base + 3U, 5U, 9U, NativeRegisterClass::GeneralPurpose, false}},
        NativeRegisterAllocationOptions{true, true, true});
    require(result.succeeded && assignment_for(result, base).register_class == NativeRegisterClass::GeneralPurpose &&
            assignment_for(result, base + 1U).register_class == NativeRegisterClass::FloatingPoint &&
            assignment_for(result, base + 2U).register_class == NativeRegisterClass::FloatingPoint &&
            assignment_for(result, base + 3U).register_class == NativeRegisterClass::GeneralPurpose,
            "N64 allocation changed the class identity of a value");
}

bool intervals_overlap(const NativeLiveInterval& left, const NativeLiveInterval& right) {
    return left.start <= right.end && right.start <= left.end;
}

void test_n65_stress_assignments_are_conflict_free(std::size_t index) {
    const std::uint32_t base = 50000U + static_cast<std::uint32_t>(index * 128U);
    std::vector<NativeLiveInterval> intervals;
    intervals.reserve(64U);
    for (std::uint32_t item = 0U; item < 64U; ++item) {
        const std::uint32_t start = static_cast<std::uint32_t>((item * 17U + index * 3U) % 48U);
        const std::uint32_t duration = static_cast<std::uint32_t>(1U + ((item * 11U + index) % 24U));
        const auto register_class = ((item + static_cast<std::uint32_t>(index % 3U)) % 2U) == 0U
            ? NativeRegisterClass::GeneralPurpose : NativeRegisterClass::FloatingPoint;
        const bool crosses_call = ((item + static_cast<std::uint32_t>(index)) % 7U) == 0U;
        intervals.push_back(NativeLiveInterval{base + item, start, start + duration,
            register_class, crosses_call});
    }
    const auto result = allocate_native_registers(intervals, NativeRegisterAllocationOptions{true, true, true});
    require(result.succeeded && result.assignments.size() == intervals.size(),
            "N65 allocator stress matrix returned an incomplete result");
    for (std::size_t left = 0U; left < intervals.size(); ++left) {
        const auto& left_assignment = assignment_for(result, intervals[left].value_id);
        for (std::size_t right = left + 1U; right < intervals.size(); ++right) {
            const auto& right_assignment = assignment_for(result, intervals[right].value_id);
            if (!intervals_overlap(intervals[left], intervals[right])) continue;
            if (!left_assignment.spilled && !right_assignment.spilled &&
                intervals[left].register_class == intervals[right].register_class) {
                require(left_assignment.register_index != right_assignment.register_index,
                        "N65 overlapping values share a physical register");
            }
            if (left_assignment.spilled && right_assignment.spilled) {
                require(left_assignment.spill_slot != right_assignment.spill_slot,
                        "N65 overlapping spills share a physical stack slot");
            }
        }
    }
}

void test_n65_stress_allocation_is_input_order_independent(std::size_t index) {
    const std::uint32_t base = 60000U + static_cast<std::uint32_t>(index * 96U);
    std::vector<NativeLiveInterval> intervals;
    intervals.reserve(48U);
    for (std::uint32_t item = 0U; item < 48U; ++item) {
        const std::uint32_t start = static_cast<std::uint32_t>((item * 13U + index) % 32U);
        const std::uint32_t end = start + static_cast<std::uint32_t>(1U + ((item * 5U + index) % 19U));
        const auto cls = (item % 2U) == 0U ? NativeRegisterClass::GeneralPurpose : NativeRegisterClass::FloatingPoint;
        intervals.push_back({base + item, start, end, cls, ((item + index) % 9U) == 0U});
    }
    const auto forward = allocate_native_registers(intervals, NativeRegisterAllocationOptions{true, true, true});
    std::reverse(intervals.begin(), intervals.end());
    const auto reverse = allocate_native_registers(intervals, NativeRegisterAllocationOptions{true, true, true});
    require(forward.succeeded && reverse.succeeded && same_assignments(forward, reverse),
            "N65 stress allocation changed when input intervals were reversed");
}

void test_n65_codegen_consumes_allocator_after_pressure(std::size_t index) {
    const std::string source =
        "module N65Stress; public function Main(double seed) returns double { "
        "double a = seed + " + std::to_string(1U + index % 9U) + ".0; "
        "double b = a * 2.0; double c = b + 3.0; double d = c * 4.0; "
        "return (a + b) * (c + d); }";
    const auto parsed = parse_source(source);
    require(parsed.succeeded(), "N65 pressure source did not parse");
    const auto compiled = compile_x64_coff_object(parsed.unit,
        X64CodegenOptions{"n65-pressure-codegen", make_coff_x86_64_object_target()});
    require(compiled.succeeded() && !compiled.object_bytes.empty(),
            "N65 register-allocation pressure codegen did not emit COFF");
    require(compiled.stats.xmm_register_temporaries > 0U,
            "N65 pressure codegen did not consume XMM register assignments");
}

void test_n65_statistics_account_for_all_values(std::size_t index) {
    const auto base = static_cast<std::uint32_t>(70000U + index * 8U);
    std::vector<NativeLiveInterval> intervals{
        {base, 0U, 10U, NativeRegisterClass::GeneralPurpose, false},
        {base + 1U, 0U, 10U, NativeRegisterClass::GeneralPurpose, false},
        {base + 2U, 0U, 10U, NativeRegisterClass::FloatingPoint, false},
        {base + 3U, 0U, 10U, NativeRegisterClass::FloatingPoint, true}};
    const auto result = allocate_native_registers(intervals, NativeRegisterAllocationOptions{true, true, true});
    require(result.succeeded, "N65 statistics scenario failed to allocate");
    const auto assigned = result.stats.general_purpose_register_assignments +
        result.stats.floating_point_register_assignments + result.stats.spilled_intervals;
    require(assigned == intervals.size(), "N65 allocation statistics do not account for every input interval");
    require(result.stats.call_crossing_spills == 1U,
            "N65 call-crossing spill counter does not match the constructed scenario");
}

} // namespace

void run_n60_n65_register_allocation_tests(int& passed, int& failed) {
    for (std::size_t index = 0U; index < 500U; ++index) {
        run_case("N60A_disjoint_spill_slot_reuse_" + std::to_string(index),
            [index] { test_n60_disjoint_spill_waves_reuse_slots(index); }, passed, failed);
        run_case("N60B_overlapping_spills_no_alias_" + std::to_string(index),
            [index] { test_n60_overlapping_spills_do_not_alias(index); }, passed, failed);
        run_case("N60C_call_spill_slot_reuse_" + std::to_string(index),
            [index] { test_n60_call_crossing_spills_reuse_after_expiry(index); }, passed, failed);
        run_case("N60D_spill_statistics_" + std::to_string(index),
            [index] { test_n60_spill_statistics_match_assignments(index); }, passed, failed);

        run_case("N61A_farthest_end_eviction_" + std::to_string(index),
            [index] { test_n61_farthest_end_interval_is_evicted(index); }, passed, failed);
        run_case("N61B_eviction_tie_determinism_" + std::to_string(index),
            [index] { test_n61_eviction_tie_break_is_stable(index); }, passed, failed);
        run_case("N61C_inclusive_endpoint_" + std::to_string(index),
            [index] { test_n61_inclusive_endpoint_remains_occupied(index); }, passed, failed);
        run_case("N61D_expired_register_reuse_" + std::to_string(index),
            [index] { test_n61_register_reused_only_after_endpoint(index); }, passed, failed);

        run_case("N62A_caller_saved_pools_" + std::to_string(index),
            [index] { test_n62_caller_saved_register_pools(index); }, passed, failed);
        run_case("N62B_nonvolatile_save_plan_" + std::to_string(index),
            [index] { test_n62_nonvolatile_register_plan_flags_preservation(index); }, passed, failed);
        run_case("N62C_gp_xmm_call_spills_" + std::to_string(index),
            [index] { test_n62_gp_and_xmm_call_crossings_spill(index); }, passed, failed);
        run_case("N62D_call_spill_override_" + std::to_string(index),
            [index] { test_n62_call_crossing_policy_can_be_overridden(index); }, passed, failed);

        run_case("N63A_reversed_interval_rejection_" + std::to_string(index),
            [index] { test_n63_reversed_interval_is_rejected(index); }, passed, failed);
        run_case("N63B_duplicate_value_id_rejection_" + std::to_string(index),
            [index] { test_n63_duplicate_value_id_is_rejected(index); }, passed, failed);
        run_case("N63C_invalid_class_empty_input_" + std::to_string(index),
            [index] { test_n63_invalid_register_class_and_empty_input(index); }, passed, failed);
        run_case("N63D_input_permutation_determinism_" + std::to_string(index),
            [index] { test_n63_input_permutation_is_deterministic(index); }, passed, failed);

        run_case("N64A_gp_xmm_pool_isolation_" + std::to_string(index),
            [index] { test_n64_gp_and_xmm_use_independent_register_pools(index); }, passed, failed);
        run_case("N64B_cross_class_spill_reuse_" + std::to_string(index),
            [index] { test_n64_sequential_gp_and_xmm_spills_share_slot(index); }, passed, failed);
        run_case("N64C_cross_class_spill_separation_" + std::to_string(index),
            [index] { test_n64_overlapping_gp_and_xmm_spills_are_distinct(index); }, passed, failed);
        run_case("N64D_mixed_class_identity_" + std::to_string(index),
            [index] { test_n64_mixed_classes_preserve_assignment_class(index); }, passed, failed);

        run_case("N65A_stress_conflict_free_" + std::to_string(index),
            [index] { test_n65_stress_assignments_are_conflict_free(index); }, passed, failed);
        run_case("N65B_stress_order_determinism_" + std::to_string(index),
            [index] { test_n65_stress_allocation_is_input_order_independent(index); }, passed, failed);
        run_case("N65C_codegen_under_pressure_" + std::to_string(index),
            [index] { test_n65_codegen_consumes_allocator_after_pressure(index); }, passed, failed);
        run_case("N65D_allocation_statistics_" + std::to_string(index),
            [index] { test_n65_statistics_account_for_all_values(index); }, passed, failed);
    }
}
