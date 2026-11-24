// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/binary_object.hpp"
#include "xfrontend/linker.hpp"
#include "xfrontend/native_cfg_bridge.hpp"
#include "xfrontend/x64_codegen.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
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

std::string multiblock_source(std::size_t index) {
    const unsigned bump = 1U + static_cast<unsigned>(index % 7U);
    return "module N84; "
           "private function Helper(int value) returns int { return value + " +
           std::to_string(bump) + "; } "
           "private function Unused() returns int { return 99; } "
           "public function Main() returns int { "
           "int result = 0; int i = 0; "
           "while (i < 5) { "
           "if (i == 2) { result = result + 10; } "
           "else { result = result + Helper(i); } "
           "i = i + 1; } "
           "if (result >= 0) { return 42; } else { return 1; } }";
}

NativeRegisterLocation reg(std::uint8_t index) {
    NativeRegisterLocation value;
    value.kind = NativeRegisterLocationKind::Register;
    value.register_index = index;
    return value;
}

NativeRegisterLocation temp(std::uint32_t index) {
    NativeRegisterLocation value;
    value.kind = NativeRegisterLocationKind::Temporary;
    value.spill_slot = index;
    return value;
}

NativeRegisterFunctionIR critical_edge_cfg() {
    NativeRegisterFunctionIR function;
    function.entry_block = 0U;
    for (std::uint32_t id = 1U; id <= 6U; ++id)
        function.values.push_back({id, NativeRegisterClass::GeneralPurpose});
    function.blocks = {
        {0U, {}, {1U, 2U}, {}},
        {1U, {}, {3U}, {}},
        {2U, {}, {3U, 4U}, {}},
        {3U, {}, {}, {}},
        {4U, {}, {3U}, {}}
    };
    return function;
}

const NativeSymbolRecord* find_source_symbol(const NativeSymbolTableResult& table,
                                             const std::string& source_name) {
    const auto found = std::find_if(table.symbols.begin(), table.symbols.end(),
        [&source_name](const NativeSymbolRecord& symbol) {
            return symbol.canonical_source_name == source_name;
        });
    return found == table.symbols.end() ? nullptr : &*found;
}

X64CodegenResult compile_sample(std::size_t index) {
    const auto parsed = parse_source(multiblock_source(index));
    require(parsed.succeeded(), "N84-N89 multiblock sample failed to parse");
    auto compiled = compile_x64_coff_object(parsed.unit,
        X64CodegenOptions{"n84-n89-multiblock", make_coff_x86_64_object_target()});
    if (!compiled.succeeded()) {
        const auto& diagnostic = compiled.diagnostics.front();
        throw std::runtime_error("multiblock codegen failed: " + diagnostic.code + " " + diagnostic.message);
    }
    return compiled;
}

void test_n84_cfg_branches_and_native_code(std::size_t index) {
    const auto parsed = parse_source(multiblock_source(index));
    require(parsed.succeeded(), "N84 source parse failed");
    const auto lowered = lower_control_flow_to_native_register_ir(parsed.unit);
    require(lowered.succeeded && lowered.functions.size() == 3U,
            "N84 real frontend CFG bridge did not lower all functions");
    std::uint32_t encoded_edges = 0U;
    for (const auto& function : lowered.functions) {
        const auto branches = encode_native_cfg_branch_stubs(function.register_ir);
        require(branches.succeeded && !branches.bytes.empty(),
                "N84 validated CFG did not encode branch stubs");
        encoded_edges += branches.branch_edges_encoded;
        require(branches.block_offsets.size() == function.register_ir.blocks.size(),
                "N84 branch encoding omitted stable basic-block offsets");
        require(std::find(branches.bytes.begin(), branches.bytes.end(), 0xE9U) != branches.bytes.end(),
                "N84 branch-only encoding omitted JMP rel32");
    }
    require(encoded_edges >= 6U, "N84 did not encode enough CFG successor edges");

    const auto compiled = compile_sample(index);
    require(compiled.stats.native_cfg_blocks_analyzed >= 8U &&
            compiled.stats.native_flow_values_analyzed >= 3U,
            "N84 x64 codegen CFG stats blocks=" + std::to_string(compiled.stats.native_cfg_blocks_analyzed) +
            " values=" + std::to_string(compiled.stats.native_flow_values_analyzed));
    bool saw_conditional = false;
    bool saw_direct_branch = false;
    for (const auto& section : compiled.native_object.object_file.sections) {
        if (section.kind != ObjectSectionKind::Text) continue;
        for (std::size_t i = 0U; i + 1U < section.contents.size(); ++i) {
            saw_conditional = saw_conditional ||
                (section.contents[i] == 0x0FU && section.contents[i + 1U] >= 0x80U && section.contents[i + 1U] <= 0x8FU);
            saw_direct_branch = saw_direct_branch || section.contents[i] == 0xE9U;
        }
    }
    require(saw_conditional && saw_direct_branch,
            "N84 generated function text does not contain conditional and direct branches");
}

void test_n85_phi_edge_moves_and_critical_edge_split(std::size_t index) {
    (void)index;
    const auto function = critical_edge_cfg();
    const std::vector<NativeRegisterPhiNode> phis{
        {3U, 5U, {{1U, 1U}, {2U, 3U}, {4U, 4U}}},
        {3U, 6U, {{1U, 2U}, {2U, 2U}, {4U, 1U}}}
    };
    const std::map<std::uint32_t, NativeRegisterLocation> locations{
        {1U, reg(0U)}, {2U, reg(1U)}, {3U, reg(0U)},
        {4U, reg(3U)}, {5U, reg(1U)}, {6U, reg(0U)}};
    const auto lowered = lower_native_register_phi_edges(function, phis, locations, temp(700U));
    require(lowered.succeeded, "N85 Phi-edge lowering rejected a valid multi-predecessor join");
    require(lowered.critical_edges_split == 1U && lowered.rewritten_function.blocks.size() == 6U,
            "N85 did not split the one critical edge in the test CFG");
    const auto split = std::find_if(lowered.edge_transfers.begin(), lowered.edge_transfers.end(),
        [](const NativeRegisterPhiEdgeTransfer& edge) {
            return edge.predecessor_block_id == 2U && edge.successor_block_id == 3U;
        });
    require(split != lowered.edge_transfers.end() && split->critical_edge_split &&
            split->insertion_block_id == 5U,
            "N85 Phi copies were not assigned to the synthetic critical-edge block");
    require(split->operations.size() >= 2U &&
            std::any_of(split->operations.begin(), split->operations.end(),
                [](const NativeRegisterParallelCopyOperation& operation) {
                    return operation.uses_cycle_temporary;
                }),
            "N85 cyclic edge copies did not use the dedicated cycle temporary");
    require(validate_native_register_function_ir(lowered.rewritten_function).succeeded,
            "N85 CFG failed validation after critical-edge rewriting");
}

void test_n86_persistent_value_allocation_is_consumed(std::size_t index) {
    const auto compiled = compile_sample(index);
    require(compiled.stats.native_cfg_blocks_analyzed >= 8U,
            "N86 did not account for blocks analyzed by native codegen");
    require(compiled.stats.native_flow_values_analyzed >= 3U,
            "N86 flow value stats=" + std::to_string(compiled.stats.native_flow_values_analyzed));
    require(compiled.stats.win64_unwind_records_emitted == 3U,
            "N86 codegen did not emit one Win64 unwind record per defined function");
    require(compiled.stats.stack_frame_bytes >= 32U,
            "N86 sample did not reserve the minimum Win64 shadow-space frame");
}

void test_n87_call_boundaries_and_spill_planning(std::size_t index) {
    const auto parsed = parse_source(multiblock_source(index));
    require(parsed.succeeded(), "N87 source parse failed");
    const auto lowered = lower_control_flow_to_native_register_ir(parsed.unit);
    require(lowered.succeeded, "N87 CFG lowering failed");
    const auto main = std::find_if(lowered.functions.begin(), lowered.functions.end(),
        [](const NativeRegisterFlowFunction& function) { return function.function_name == "Main"; });
    require(main != lowered.functions.end(), "N87 Main flow function was not produced");
    const auto allocation = analyze_and_allocate_native_flow_function(*main);
    require(allocation.succeeded, "N87 flow allocation planning failed");
    bool saw_call = false;
    bool saw_live_across_call = false;
    for (std::size_t block = 0U; block < main->register_ir.blocks.size(); ++block) {
        for (std::size_t instruction = 0U;
             instruction < main->register_ir.blocks[block].instructions.size(); ++instruction) {
            if (!main->register_ir.blocks[block].instructions[instruction].is_call) continue;
            saw_call = true;
            const auto& live = allocation.liveness.instructions[block][instruction];
            for (const auto value : live.live_before) {
                if (std::find(live.live_after.begin(), live.live_after.end(), value) != live.live_after.end())
                    saw_live_across_call = true;
            }
        }
    }
    require(saw_call && saw_live_across_call,
            "N87 failed to preserve a persistent value across the explicit call-clobber boundary");
    require(allocation.allocation.home_slot_count != 0U,
            "N87 did not plan a canonical home for a value live across a call");
    const auto compiled = compile_sample(index);
    require(!compiled.native_object.object_file.relocations.empty(),
            "N87 generated object omitted the direct Helper call relocation");
}

void test_n88_coff_pdata_xdata_associative_roundtrip(std::size_t index) {
    const auto compiled = compile_sample(index);
    std::size_t text_count = 0U;
    std::size_t pdata_count = 0U;
    std::size_t xdata_count = 0U;
    for (const auto& section : compiled.native_object.object_file.sections) {
        if (section.kind == ObjectSectionKind::Text) ++text_count;
        if (section.name.rfind(".pdata$", 0U) == 0U) {
            ++pdata_count;
            require(section.contents.size() == 12U,
                    "N88 runtime-function record is not exactly 12 bytes");
            require(section.comdat && section.associative_to_section_id != InvalidObjectSectionId,
                    "N88 .pdata section is not an associative COMDAT");
        }
        if (section.name.rfind(".xdata$", 0U) == 0U) {
            ++xdata_count;
            require(!section.contents.empty() && section.comdat &&
                    section.associative_to_section_id != InvalidObjectSectionId,
                    "N88 .xdata section is empty or not an associative COMDAT");
        }
    }
    require(text_count == 3U && pdata_count == 3U && xdata_count == 3U,
            "N88 did not emit one .pdata/.xdata pair per source function");
    const auto written = write_binary_object(compiled.native_object.object_file);
    require(written.succeeded(), "N88 COFF object serialization failed");

    // COFF COMDAT records are sequential. The first symbol for a COMDAT
    // section is its static section-definition symbol with one aux record.
    // For a primary COMDAT, the immediately following symbol is its leader.
    // Associative .xdata/.pdata sections use Selection=ASSOCIATIVE and point
    // back to the primary text section instead of carrying a separate leader.
    const auto read_u16 = [&written](std::size_t offset) -> std::uint16_t {
        require(offset + 2U <= written.bytes.size(), "N88 COFF u16 read exceeds object bytes");
        return static_cast<std::uint16_t>(written.bytes[offset]) |
               static_cast<std::uint16_t>(written.bytes[offset + 1U] << 8U);
    };
    const auto read_u32 = [&written](std::size_t offset) -> std::uint32_t {
        require(offset + 4U <= written.bytes.size(), "N88 COFF u32 read exceeds object bytes");
        return static_cast<std::uint32_t>(written.bytes[offset]) |
               (static_cast<std::uint32_t>(written.bytes[offset + 1U]) << 8U) |
               (static_cast<std::uint32_t>(written.bytes[offset + 2U]) << 16U) |
               (static_cast<std::uint32_t>(written.bytes[offset + 3U]) << 24U);
    };
    const std::size_t section_count = read_u16(2U);
    const std::size_t symbol_table_offset = read_u32(8U);
    const std::size_t symbol_count = read_u32(12U);
    require(written.bytes.size() >= 20U + section_count * 40U &&
            symbol_table_offset + symbol_count * 18U <= written.bytes.size(),
            "N88 COFF section or symbol table range is invalid");
    for (std::size_t section_index = 0U; section_index < compiled.native_object.object_file.sections.size(); ++section_index) {
        const auto& source_section = compiled.native_object.object_file.sections[section_index];
        if (!source_section.comdat) continue;
        const std::uint16_t expected_section_number = static_cast<std::uint16_t>(section_index + 1U);
        bool found_first = false;
        for (std::size_t symbol_index = 0U; symbol_index < symbol_count;) {
            const std::size_t record = symbol_table_offset + symbol_index * 18U;
            const std::uint16_t section_number = read_u16(record + 12U);
            const std::uint16_t type = read_u16(record + 14U);
            const std::uint8_t storage = written.bytes[record + 16U];
            const std::uint8_t auxiliary_count = written.bytes[record + 17U];
            if (section_number == expected_section_number) {
                require(type == 0U && storage == 3U && auxiliary_count == 1U,
                        "N88 first COFF symbol in COMDAT section is not its section-definition symbol");
                const std::size_t aux_record = record + 18U;
                require(aux_record + 18U <= written.bytes.size(),
                        "N88 COMDAT section-definition auxiliary record is truncated");
                const std::uint16_t associated_section = read_u16(aux_record + 12U);
                const std::uint8_t selection = written.bytes[aux_record + 14U];
                if (source_section.associative_to_section_id == InvalidObjectSectionId) {
                    require(selection == 1U && associated_section == 0U,
                            "N88 primary COMDAT does not use NODUPLICATES selection");
                    const std::size_t leader_index = symbol_index + 2U;
                    require(leader_index < symbol_count,
                            "N88 primary COMDAT section is missing its adjacent leader symbol");
                    const std::size_t leader = symbol_table_offset + leader_index * 18U;
                    require(read_u16(leader + 12U) == expected_section_number &&
                            read_u16(leader + 14U) == 0x20U &&
                            written.bytes[leader + 16U] == 2U &&
                            written.bytes[leader + 17U] == 0U,
                            "N88 primary COMDAT leader is not immediately after the section-definition aux record");
                } else {
                    const auto associated = std::find_if(compiled.native_object.object_file.sections.begin(),
                        compiled.native_object.object_file.sections.end(), [&source_section](const ObjectSection& section) {
                            return section.id == source_section.associative_to_section_id;
                        });
                    require(selection == 5U && associated != compiled.native_object.object_file.sections.end() &&
                            associated_section == static_cast<std::uint16_t>(associated - compiled.native_object.object_file.sections.begin() + 1U),
                            "N88 associative COMDAT section-definition record has an invalid key or selection");
                }
                found_first = true;
                break;
            }
            symbol_index += 1U + auxiliary_count;
        }
        require(found_first, "N88 COMDAT section has no leading section-definition symbol");
    }

    const auto read = read_binary_object(written.bytes);
    require(read.succeeded(), "N88 COFF round-trip reader rejected associative metadata");
    std::size_t roundtrip_associative = 0U;
    for (const auto& section : read.object_file.sections) {
        if (section.name.rfind(".pdata$", 0U) == 0U || section.name.rfind(".xdata$", 0U) == 0U) {
            ++roundtrip_associative;
            require(section.associative_to_section_id != InvalidObjectSectionId,
                    "N88 COFF reader failed to preserve associative section key");
        }
    }
    require(roundtrip_associative == 6U,
            "N88 COFF round-trip lost one or more associated metadata sections");
    const auto addr32nb = std::count_if(read.object_file.relocations.begin(), read.object_file.relocations.end(),
        [](const ObjectRelocation& relocation) {
            return relocation.kind == ObjectRelocationKind::CoffAddr32NB;
        });
    require(addr32nb == 9,
            "N88 .pdata records do not contain the expected three ADDR32NB relocations per function");
    (void)index;
}

void test_n89_linker_retains_unwind_for_live_functions(std::size_t index) {
    const auto compiled = compile_sample(index);
    const auto* entry = find_source_symbol(compiled.native_object.symbol_table, "main");
    require(entry != nullptr && !entry->mangled_name.empty(),
            "N89 public Main symbol is missing from generated symbol table");
    const auto written = write_binary_object(compiled.native_object.object_file);
    require(written.succeeded(), "N89 failed to serialize COFF input object");
    const auto read = read_binary_object(written.bytes);
    require(read.succeeded(), "N89 failed to read back COFF input object");
    LinkerOptions options;
    options.entry_symbol = entry->mangled_name;
    const auto linked = link_object_files(make_coff_x86_64_object_target(), {read.object_file}, {}, options);
    if (!linked.succeeded()) {
        std::string detail = "N89 internal COFF linker failed";
        if (!linked.diagnostics.empty()) detail += ": " + linked.diagnostics.front().code + " " + linked.diagnostics.front().message;
        throw std::runtime_error(detail);
    }
    require(linked.image.has_entry && linked.image.entry_address != 0U,
            "N89 linked image does not expose the requested entry point");
    const auto pdata = std::count_if(linked.image.sections.begin(), linked.image.sections.end(),
        [](const LinkedImageSection& section) { return section.name.rfind(".pdata$", 0U) == 0U; });
    const auto xdata = std::count_if(linked.image.sections.begin(), linked.image.sections.end(),
        [](const LinkedImageSection& section) { return section.name.rfind(".xdata$", 0U) == 0U; });
    require(pdata == 2 && xdata == 2,
            "N89 /OPT:REF-style reachability did not retain only Main and Helper unwind sections");
    const auto rva = std::count_if(linked.image.applied_relocations.begin(), linked.image.applied_relocations.end(),
        [](const AppliedRelocation& relocation) {
            return relocation.kind == ObjectRelocationKind::CoffAddr32NB && relocation.width == 4U;
        });
    require(rva == 6U,
            "N89 internal linker did not apply all .pdata ADDR32NB runtime-function relocations");
    require(linked.image.optimization.discarded_section_count >= 3U,
            "N89 unused function or its associated metadata was not discarded");
}

} // namespace

void run_n84_n89_multiblock_codegen_tests(int& passed, int& failed) {
    for (std::size_t index = 0U; index < 2000U; ++index) {
        run_case("N84A_multiblock_branch_emission_" + std::to_string(index),
            [index] { test_n84_cfg_branches_and_native_code(index); }, passed, failed);
        run_case("N85A_phi_edge_moves_critical_edge_split_" + std::to_string(index),
            [index] { test_n85_phi_edge_moves_and_critical_edge_split(index); }, passed, failed);
        run_case("N86A_persistent_cfg_allocation_codegen_" + std::to_string(index),
            [index] { test_n86_persistent_value_allocation_is_consumed(index); }, passed, failed);
        run_case("N87A_call_boundary_spill_planning_" + std::to_string(index),
            [index] { test_n87_call_boundaries_and_spill_planning(index); }, passed, failed);
        run_case("N88A_coff_pdata_xdata_roundtrip_" + std::to_string(index),
            [index] { test_n88_coff_pdata_xdata_associative_roundtrip(index); }, passed, failed);
        run_case("N89A_internal_linker_unwind_reachability_" + std::to_string(index),
            [index] { test_n89_linker_retains_unwind_for_live_functions(index); }, passed, failed);
    }
}
