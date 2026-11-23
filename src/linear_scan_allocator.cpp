#include "xfrontend/linear_scan_allocator.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <tuple>
#include <vector>

namespace xfrontend {
namespace {

bool valid_register_class(NativeRegisterClass value) {
    return value == NativeRegisterClass::GeneralPurpose ||
           value == NativeRegisterClass::FloatingPoint;
}

} // namespace

NativeRegisterAllocationResult allocate_native_registers(
    const std::vector<NativeLiveInterval>& intervals,
    NativeRegisterAllocationOptions options) {
    NativeRegisterAllocationResult result;
    for (const auto& item : intervals) {
        if (item.end < item.start || !valid_register_class(item.register_class)) return result;
    }

    std::vector<NativeLiveInterval> ordered = intervals;
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return std::tie(left.start, left.end, left.value_id) <
               std::tie(right.start, right.end, right.value_id);
    });
    std::set<std::uint32_t> ids;
    for (const auto& item : ordered) {
        if (!ids.insert(item.value_id).second) return result;
    }

    struct Active {
        NativeLiveInterval interval;
        std::uint8_t reg = 0U;
    };

    const std::array<std::uint8_t, 7U> nonvolatile_gp{{3U, 6U, 7U, 12U, 13U, 14U, 15U}};
    const std::array<std::uint8_t, 10U> nonvolatile_xmm{{6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U}};
    const std::array<std::uint8_t, 4U> volatile_gp{{8U, 9U, 10U, 11U}};
    const std::array<std::uint8_t, 6U> volatile_xmm{{0U, 1U, 2U, 3U, 4U, 5U}};
    const std::array<std::uint8_t, 5U> volatile_xmm_without_return{{1U, 2U, 3U, 4U, 5U}};

    std::vector<std::uint8_t> gp_register_pool;
    std::vector<std::uint8_t> xmm_register_pool;
    if (options.prefer_caller_saved) {
        gp_register_pool.assign(volatile_gp.begin(), volatile_gp.end());
        if (options.use_callee_saved_fallback)
            gp_register_pool.insert(gp_register_pool.end(), nonvolatile_gp.begin(), nonvolatile_gp.end());
        if (options.reserve_xmm0)
            xmm_register_pool.assign(volatile_xmm_without_return.begin(), volatile_xmm_without_return.end());
        else
            xmm_register_pool.assign(volatile_xmm.begin(), volatile_xmm.end());
        if (options.use_callee_saved_fallback)
            xmm_register_pool.insert(xmm_register_pool.end(), nonvolatile_xmm.begin(), nonvolatile_xmm.end());
    } else {
        gp_register_pool.assign(nonvolatile_gp.begin(), nonvolatile_gp.end());
        xmm_register_pool.assign(nonvolatile_xmm.begin(), nonvolatile_xmm.end());
    }

    const auto remove_reserved = [](std::vector<std::uint8_t>& pool,
                                    const std::vector<std::uint8_t>& reserved) {
        pool.erase(std::remove_if(pool.begin(), pool.end(), [&](std::uint8_t reg) {
            return std::find(reserved.begin(), reserved.end(), reg) != reserved.end();
        }), pool.end());
    };
    remove_reserved(gp_register_pool, options.reserved_gp_registers);
    remove_reserved(xmm_register_pool, options.reserved_xmm_registers);

    const auto requires_callee_save = [](NativeRegisterClass register_class, std::uint8_t reg) {
        if (register_class == NativeRegisterClass::GeneralPurpose)
            return reg == 3U || reg == 6U || reg == 7U || reg >= 12U;
        return reg >= 6U;
    };

    std::vector<Active> active_gp;
    std::vector<Active> active_xmm;
    std::map<std::uint32_t, NativeRegisterAssignment> assignments;
    std::vector<NativeLiveInterval> spilled_intervals;
    spilled_intervals.reserve(intervals.size());

    const auto spill_interval = [&](const NativeLiveInterval& interval, bool call_crossing) {
        NativeRegisterAssignment assignment;
        assignment.value_id = interval.value_id;
        assignment.register_class = interval.register_class;
        assignment.spilled = true;
        assignment.requires_callee_save = false;
        assignments[interval.value_id] = assignment;
        spilled_intervals.push_back(interval);
        if (call_crossing) ++result.stats.call_crossing_spills;
    };

    const auto process_class = [&](const NativeLiveInterval& current,
                                   const auto& registers, std::vector<Active>& active) {
        // Intervals are inclusive: an interval ending at position P remains
        // active for another interval starting at the same position P.
        active.erase(std::remove_if(active.begin(), active.end(), [&](const Active& entry) {
            return entry.interval.end < current.start;
        }), active.end());

        if (options.prefer_caller_saved && options.spill_call_crossing && current.crosses_call) {
            spill_interval(current, true);
            return;
        }

        std::set<std::uint8_t> occupied;
        for (const auto& entry : active) occupied.insert(entry.reg);

        std::uint8_t free_reg = 0U;
        bool has_free = false;
        for (const auto reg : registers) {
            if (occupied.find(reg) == occupied.end()) {
                free_reg = reg;
                has_free = true;
                break;
            }
        }

        NativeRegisterAssignment assignment;
        assignment.value_id = current.value_id;
        assignment.register_class = current.register_class;

        if (has_free) {
            assignment.register_index = free_reg;
            assignment.requires_callee_save = requires_callee_save(current.register_class, free_reg);
            active.push_back(Active{current, free_reg});
            assignments[current.value_id] = assignment;
            return;
        }

        const auto farthest = std::max_element(active.begin(), active.end(), [](const Active& left, const Active& right) {
            if (left.interval.end != right.interval.end)
                return left.interval.end < right.interval.end;
            return left.interval.value_id < right.interval.value_id;
        });

        if (farthest != active.end() && farthest->interval.end > current.end) {
            // Evict the interval with the farthest end. Its full interval is
            // conservatively assigned a spill slot; no split/reload transition
            // is implied by this allocator-only stage.
            spill_interval(farthest->interval, false);
            const std::uint8_t reused_register = farthest->reg;
            active.erase(farthest);
            assignment.register_index = reused_register;
            assignment.requires_callee_save = requires_callee_save(current.register_class, reused_register);
            active.push_back(Active{current, reused_register});
            assignments[current.value_id] = assignment;
            ++result.stats.register_evictions;
            return;
        }

        spill_interval(current, false);
    };

    for (const auto& current : ordered) {
        if (current.register_class == NativeRegisterClass::GeneralPurpose) {
            process_class(current, gp_register_pool, active_gp);
        } else {
            process_class(current, xmm_register_pool, active_xmm);
        }
    }

    // Color all spill intervals together because each native spill slot is an
    // eight-byte stack cell irrespective of whether its source was GP or XMM.
    // The first slot whose previous inclusive interval ended before this one
    // begins is safe to reuse. Sorting gives deterministic lowest-index reuse.
    std::sort(spilled_intervals.begin(), spilled_intervals.end(), [](const auto& left, const auto& right) {
        return std::tie(left.start, left.end, left.value_id) <
               std::tie(right.start, right.end, right.value_id);
    });
    std::vector<std::uint32_t> spill_slot_ends;
    for (const auto& interval : spilled_intervals) {
        std::size_t slot = 0U;
        while (slot < spill_slot_ends.size() && spill_slot_ends[slot] >= interval.start) ++slot;
        if (slot == spill_slot_ends.size()) {
            spill_slot_ends.push_back(interval.end);
        } else {
            spill_slot_ends[slot] = interval.end;
            ++result.stats.reused_spill_slots;
        }
        assignments[interval.value_id].spill_slot = static_cast<std::uint32_t>(slot);
    }
    result.spill_slot_count = static_cast<std::uint32_t>(spill_slot_ends.size());

    std::set<std::uint8_t> used_gp;
    std::set<std::uint8_t> used_xmm;
    for (const auto& entry : assignments) {
        const auto& assignment = entry.second;
        result.assignments.push_back(assignment);
        if (assignment.spilled) {
            ++result.stats.spilled_intervals;
        } else {
            if (assignment.register_class == NativeRegisterClass::GeneralPurpose) {
                ++result.stats.general_purpose_register_assignments;
                if (assignment.requires_callee_save) used_gp.insert(assignment.register_index);
            } else {
                ++result.stats.floating_point_register_assignments;
                if (assignment.requires_callee_save) used_xmm.insert(assignment.register_index);
            }
        }
    }
    result.used_callee_saved_gp.assign(used_gp.begin(), used_gp.end());
    result.used_callee_saved_xmm.assign(used_xmm.begin(), used_xmm.end());

    // Nonvolatile assignments are counted separately from volatile assignments
    // but remain visible through the corresponding used_callee_saved_* lists.
    result.succeeded = true;
    return result;
}


NativeRegisterAllocationResult allocate_native_registers_with_interference(
    const std::vector<NativeLiveInterval>& intervals,
    const std::vector<NativeRegisterInterferenceEdge>& edges,
    NativeRegisterAllocationOptions options) {
    NativeRegisterAllocationResult result;
    std::map<std::uint32_t, NativeLiveInterval> values;
    for (const auto& interval : intervals) {
        if (interval.end < interval.start || !valid_register_class(interval.register_class) ||
            !values.emplace(interval.value_id, interval).second) return result;
    }

    std::map<std::uint32_t, std::set<std::uint32_t>> adjacency;
    for (const auto& interval : intervals) adjacency.emplace(interval.value_id, std::set<std::uint32_t>{});
    for (const auto& edge : edges) {
        if (edge.first_value_id == edge.second_value_id) return result;
        if (values.find(edge.first_value_id) == values.end() ||
            values.find(edge.second_value_id) == values.end()) continue;
        const auto& first = values.at(edge.first_value_id);
        const auto& second = values.at(edge.second_value_id);
        if (first.register_class != second.register_class) continue;
        adjacency[edge.first_value_id].insert(edge.second_value_id);
        adjacency[edge.second_value_id].insert(edge.first_value_id);
    }

    const std::array<std::uint8_t, 7U> nonvolatile_gp{{3U, 6U, 7U, 12U, 13U, 14U, 15U}};
    const std::array<std::uint8_t, 10U> nonvolatile_xmm{{6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U}};
    const std::array<std::uint8_t, 4U> volatile_gp{{8U, 9U, 10U, 11U}};
    const std::array<std::uint8_t, 6U> volatile_xmm{{0U, 1U, 2U, 3U, 4U, 5U}};
    const std::array<std::uint8_t, 5U> volatile_xmm_without_return{{1U, 2U, 3U, 4U, 5U}};

    std::vector<std::uint8_t> gp_pool;
    std::vector<std::uint8_t> xmm_pool;
    if (options.prefer_caller_saved) {
        gp_pool.assign(volatile_gp.begin(), volatile_gp.end());
        xmm_pool = options.reserve_xmm0
            ? std::vector<std::uint8_t>(volatile_xmm_without_return.begin(), volatile_xmm_without_return.end())
            : std::vector<std::uint8_t>(volatile_xmm.begin(), volatile_xmm.end());
        if (options.use_callee_saved_fallback) {
            gp_pool.insert(gp_pool.end(), nonvolatile_gp.begin(), nonvolatile_gp.end());
            xmm_pool.insert(xmm_pool.end(), nonvolatile_xmm.begin(), nonvolatile_xmm.end());
        }
    } else {
        gp_pool.assign(nonvolatile_gp.begin(), nonvolatile_gp.end());
        xmm_pool.assign(nonvolatile_xmm.begin(), nonvolatile_xmm.end());
    }
    const auto remove_reserved = [](std::vector<std::uint8_t>& pool, const std::vector<std::uint8_t>& reserved) {
        pool.erase(std::remove_if(pool.begin(), pool.end(), [&](std::uint8_t reg) {
            return std::find(reserved.begin(), reserved.end(), reg) != reserved.end();
        }), pool.end());
    };
    remove_reserved(gp_pool, options.reserved_gp_registers);
    remove_reserved(xmm_pool, options.reserved_xmm_registers);

    const auto requires_callee_save = [](NativeRegisterClass cls, std::uint8_t reg) {
        return cls == NativeRegisterClass::GeneralPurpose
            ? (reg == 3U || reg == 6U || reg == 7U || reg >= 12U)
            : reg >= 6U;
    };

    std::vector<std::uint32_t> order;
    order.reserve(intervals.size());
    for (const auto& interval : intervals) order.push_back(interval.value_id);
    std::sort(order.begin(), order.end(), [&](std::uint32_t left, std::uint32_t right) {
        const auto& a = values.at(left);
        const auto& b = values.at(right);
        if (a.register_class != b.register_class) return a.register_class < b.register_class;
        if (adjacency[left].size() != adjacency[right].size())
            return adjacency[left].size() > adjacency[right].size();
        if (a.crosses_call != b.crosses_call) return a.crosses_call;
        if (a.start != b.start) return a.start < b.start;
        if (a.end != b.end) return a.end > b.end;
        return left < right;
    });

    std::map<std::uint32_t, NativeRegisterAssignment> assignments;
    std::vector<std::uint32_t> spilled;
    for (const auto value_id : order) {
        const auto& interval = values.at(value_id);
        NativeRegisterAssignment assignment;
        assignment.value_id = value_id;
        assignment.register_class = interval.register_class;
        const auto& pool = interval.register_class == NativeRegisterClass::GeneralPurpose ? gp_pool : xmm_pool;
        bool forced_spill = options.prefer_caller_saved && options.spill_call_crossing && interval.crosses_call;
        std::set<std::uint8_t> occupied;
        for (const auto neighbour : adjacency[value_id]) {
            const auto found = assignments.find(neighbour);
            if (found != assignments.end() && !found->second.spilled) occupied.insert(found->second.register_index);
        }
        bool found_register = false;
        if (!forced_spill) {
            for (const auto reg : pool) {
                if (occupied.find(reg) == occupied.end()) {
                    assignment.register_index = reg;
                    assignment.requires_callee_save = requires_callee_save(interval.register_class, reg);
                    found_register = true;
                    break;
                }
            }
        }
        if (!found_register) {
            assignment.spilled = true;
            spilled.push_back(value_id);
            if (interval.crosses_call) ++result.stats.call_crossing_spills;
        }
        assignments.emplace(value_id, assignment);
    }

    // Color spill homes with the same graph. This allows branch-exclusive
    // spilled values to reuse a stack slot while interfering spills stay apart.
    std::sort(spilled.begin(), spilled.end(), [&](std::uint32_t left, std::uint32_t right) {
        if (adjacency[left].size() != adjacency[right].size())
            return adjacency[left].size() > adjacency[right].size();
        return left < right;
    });
    std::uint32_t slot_count = 0U;
    for (const auto value_id : spilled) {
        std::set<std::uint32_t> occupied_slots;
        for (const auto neighbour : adjacency[value_id]) {
            const auto found = assignments.find(neighbour);
            if (found != assignments.end() && found->second.spilled) occupied_slots.insert(found->second.spill_slot);
        }
        std::uint32_t slot = 0U;
        while (occupied_slots.find(slot) != occupied_slots.end()) ++slot;
        assignments[value_id].spill_slot = slot;
        slot_count = std::max(slot_count, slot + 1U);
    }
    result.spill_slot_count = slot_count;

    std::set<std::uint8_t> used_gp;
    std::set<std::uint8_t> used_xmm;
    for (const auto& item : assignments) {
        const auto& assignment = item.second;
        result.assignments.push_back(assignment);
        if (assignment.spilled) {
            ++result.stats.spilled_intervals;
        } else if (assignment.register_class == NativeRegisterClass::GeneralPurpose) {
            ++result.stats.general_purpose_register_assignments;
            if (assignment.requires_callee_save) used_gp.insert(assignment.register_index);
        } else {
            ++result.stats.floating_point_register_assignments;
            if (assignment.requires_callee_save) used_xmm.insert(assignment.register_index);
        }
    }
    result.used_callee_saved_gp.assign(used_gp.begin(), used_gp.end());
    result.used_callee_saved_xmm.assign(used_xmm.begin(), used_xmm.end());
    result.succeeded = true;
    return result;
}

} // namespace xfrontend
