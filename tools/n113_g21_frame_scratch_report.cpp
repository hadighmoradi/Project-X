#include "xfrontend/native_program_pipeline.hpp"
#include "xfrontend/parser.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace xfrontend;

struct InputModule {
    std::filesystem::path path;
    std::shared_ptr<const CompilationUnit> unit;
};

std::string json_string(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (const unsigned char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20U) out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                                << static_cast<unsigned int>(ch) << std::dec << std::setfill(' ');
            else out << static_cast<char>(ch);
            break;
        }
    }
    out << '"';
    return out.str();
}

const char* register_class_name(const NativeRegisterClass value) {
    return value == NativeRegisterClass::GeneralPurpose ? "GeneralPurpose" : "FloatingPoint";
}

std::string owner_json(const X64FlowFrameSpillSlotOwnerDiagnosticReport& owner) {
    std::ostringstream out;
    out << "{\"segment_id\":" << owner.segment_id
        << ",\"value_id\":" << owner.value_id
        << ",\"block_id\":" << owner.block_id
        << ",\"start_position\":" << owner.start_position
        << ",\"end_position\":" << owner.end_position
        << ",\"register_class\":" << json_string(register_class_name(owner.register_class)) << '}';
    return out.str();
}

std::string build_json(const NativeProgramBuildResult& build) {
    std::ostringstream out;
    out << "{\n  \"schema\":\"project-x.n113-g21-frame-scratch-feasibility.v1\",\n"
        << "  \"scope\":\"read-only active/hypothetical frame spill-slot inventory and exact G4 endpoint occupancy\",\n"
        << "  \"target\":\"AMD64 COFF\",\n"
        << "  \"frame_mutation_enabled\":false,\n"
        << "  \"unwind_mutation_enabled\":false,\n"
        << "  \"machine_code_insertion_enabled\":false,\n"
        << "  \"insertion_ready\":false,\n"
        << "  \"modules\":[";
    bool first_module = true;
    for (const auto& module : build.modules) {
        const auto& functions = module.codegen.stats.native_n113_frame_scratch_diagnostic_function_reports;
        if (functions.empty()) continue;
        if (!first_module) out << ',';
        first_module = false;
        out << "\n    {\"module_name\":" << json_string(module.module_name) << ",\"functions\":[";
        for (std::size_t f = 0U; f < functions.size(); ++f) {
            if (f != 0U) out << ',';
            const auto& function = functions[f];
            out << "\n      {\"function_index\":" << function.function_index
                << ",\"function_name\":" << json_string(function.function_name)
                << ",\"active_frame_plan_succeeded\":" << (function.active_frame_plan_succeeded ? "true" : "false")
                << ",\"active_frame_size_bytes\":" << function.active_frame_size_bytes
                << ",\"active_emitter_spill_slot_count\":" << function.active_emitter_spill_slot_count
                << ",\"active_emitter_home_slot_count\":" << function.active_emitter_home_slot_count
                << ",\"flow_frame_slots_reserved_in_active_frame\":" << (function.flow_frame_slots_reserved_in_active_frame ? "true" : "false")
                << ",\"flow_spill_slots_planned\":" << function.flow_spill_slots_planned
                << ",\"flow_home_slots_planned\":" << function.flow_home_slots_planned
                << ",\"hypothetical_flow_spill_slots_mapped\":" << function.hypothetical_flow_spill_slots_mapped
                << ",\"emitter_spill_slots\":[";
            for (std::size_t s = 0U; s < function.emitter_spill_slots.size(); ++s) {
                if (s != 0U) out << ',';
                const auto& slot = function.emitter_spill_slots[s];
                out << "{\"slot_index\":" << slot.slot_index
                    << ",\"active_frame_displacement\":" << slot.active_frame_displacement
                    << ",\"edge_liveness_crosswalk_available\":" << (slot.edge_liveness_crosswalk_available ? "true" : "false")
                    << ",\"proven_free_for_edge_scratch\":" << (slot.proven_free_for_edge_scratch ? "true" : "false")
                    << ",\"owners\":[";
                for (std::size_t o = 0U; o < slot.owners.size(); ++o) {
                    if (o != 0U) out << ',';
                    out << owner_json(slot.owners[o]);
                }
                out << "]}";
            }
            out << "],\"edges\":[";
            for (std::size_t e = 0U; e < function.edges.size(); ++e) {
                if (e != 0U) out << ',';
                const auto& edge = function.edges[e];
                out << "{\"edge_id\":" << edge.edge_id
                    << ",\"predecessor_block_id\":" << edge.predecessor_block_id
                    << ",\"successor_block_id\":" << edge.successor_block_id
                    << ",\"g4_live_values\":" << edge.g4_live_values
                    << ",\"flow_spill_slots\":[";
                for (std::size_t s = 0U; s < edge.flow_spill_slots.size(); ++s) {
                    if (s != 0U) out << ',';
                    const auto& slot = edge.flow_spill_slots[s];
                    out << "{\"slot_index\":" << slot.slot_index
                        << ",\"hypothetical_frame_displacement\":" << slot.hypothetical_frame_displacement
                        << ",\"frame_mapping_available\":" << (slot.frame_mapping_available ? "true" : "false")
                        << ",\"active_frame_reserved\":" << (slot.active_frame_reserved ? "true" : "false")
                        << ",\"live_endpoint_uses\":" << slot.live_endpoint_uses
                        << ",\"live_endpoint_value_ids\":[";
                    for (std::size_t v = 0U; v < slot.live_endpoint_value_ids.size(); ++v) {
                        if (v != 0U) out << ',';
                        out << slot.live_endpoint_value_ids[v];
                    }
                    out << "],\"no_g4_live_endpoint_alias\":" << (slot.no_g4_live_endpoint_alias ? "true" : "false")
                        << ",\"candidate_after_flow_frame_reservation\":" << (slot.candidate_after_flow_frame_reservation ? "true" : "false")
                        << ",\"usable_in_current_active_frame\":" << (slot.usable_in_current_active_frame ? "true" : "false")
                        << ",\"current_g7_accepts_spill_slot_temporary\":" << (slot.current_g7_accepts_spill_slot_temporary ? "true" : "false")
                        << ",\"owners\":[";
                    for (std::size_t o = 0U; o < slot.owners.size(); ++o) {
                        if (o != 0U) out << ',';
                        out << owner_json(slot.owners[o]);
                    }
                    out << "]}";
                }
                out << "]}";
            }
            out << "]}";
        }
        out << "]}";
    }
    out << "\n  ]\n}\n";
    return out.str();
}

std::string build_text(const NativeProgramBuildResult& build) {
    std::ostringstream out;
    std::uint64_t functions = 0U;
    std::uint64_t active_emitter_spills = 0U;
    std::uint64_t flow_spills = 0U;
    std::uint64_t flow_homes = 0U;
    std::uint64_t emitter_homes = 0U;
    std::uint64_t candidate_edge_slots = 0U;
    std::uint64_t currently_usable_slots = 0U;
    std::uint64_t endpoint_aliases = 0U;
    for (const auto& module : build.modules) {
        for (const auto& function : module.codegen.stats.native_n113_frame_scratch_diagnostic_function_reports) {
            ++functions;
            active_emitter_spills += function.active_emitter_spill_slot_count;
            flow_spills += function.flow_spill_slots_planned;
            flow_homes += function.flow_home_slots_planned;
            emitter_homes += function.active_emitter_home_slot_count;
            for (const auto& edge : function.edges) {
                for (const auto& slot : edge.flow_spill_slots) {
                    candidate_edge_slots += slot.candidate_after_flow_frame_reservation ? 1U : 0U;
                    currently_usable_slots += slot.usable_in_current_active_frame ? 1U : 0U;
                    endpoint_aliases += slot.live_endpoint_uses;
                }
            }
        }
    }
    out << "Project X N113-G21 frame-backed cycle-scratch feasibility\n"
        << "Scope: read-only slot inventory, physical frame mapping and exact G4 endpoint occupancy\n"
        << "Functions: " << functions << "\n"
        << "Active Emitter SpillSlots: " << active_emitter_spills << "\n"
        << "N112 Flow SpillSlots planned: " << flow_spills << "\n"
        << "N112 Flow HomeSlots planned (canonical homes; not scratch): " << flow_homes << "\n"
        << "Active Emitter HomeSlots: " << emitter_homes << "\n"
        << "Per-edge Flow slot candidates after explicit Flow-frame reservation: " << candidate_edge_slots << "\n"
        << "Per-edge Flow slots usable in current active frame: " << currently_usable_slots << "\n"
        << "G4 endpoint uses of Flow SpillSlots: " << endpoint_aliases << "\n"
        << "CURRENT_G7_ACCEPTS_MEMORY_CYCLE_TEMPORARY=FALSE\n"
        << "FLOW_FRAME_RESERVATION_PERFORMED=FALSE\n"
        << "FRAME_MUTATION=NOT_PERFORMED\nUNWIND_MUTATION=NOT_PERFORMED\n"
        << "MACHINE_CODE_INSERTION=NOT_PERFORMED\nINSERTION_READY=FALSE\n\n";
    for (const auto& module : build.modules) {
        out << "MODULE " << module.module_name << "\n";
        for (const auto& function : module.codegen.stats.native_n113_frame_scratch_diagnostic_function_reports) {
            out << " FUNCTION " << function.function_index << ' ' << function.function_name
                << " active_frame=" << function.active_frame_plan_succeeded << ':' << function.active_frame_size_bytes
                << " emitter_spills=" << function.active_emitter_spill_slot_count
                << " emitter_homes=" << function.active_emitter_home_slot_count
                << " flow_spills_planned=" << function.flow_spill_slots_planned
                << " flow_homes_planned=" << function.flow_home_slots_planned
                << " flow_slots_active=" << function.flow_frame_slots_reserved_in_active_frame
                << " flow_slots_mapped=" << function.hypothetical_flow_spill_slots_mapped << "\n";
            for (const auto& slot : function.emitter_spill_slots) {
                out << "  EMITTER_SPILL_SLOT " << slot.slot_index
                    << " disp=" << slot.active_frame_displacement
                    << " owners=" << slot.owners.size()
                    << " edge_crosswalk=" << slot.edge_liveness_crosswalk_available
                    << " proven_free=" << slot.proven_free_for_edge_scratch << "\n";
                for (const auto& owner : slot.owners) {
                    out << "   OWNER value=" << owner.value_id << " segment=" << owner.segment_id
                        << " block=" << owner.block_id << " range=" << owner.start_position << '-' << owner.end_position
                        << " class=" << register_class_name(owner.register_class) << "\n";
                }
            }
            for (const auto& edge : function.edges) {
                std::uint32_t future_candidates = 0U;
                std::uint32_t current_candidates = 0U;
                for (const auto& slot : edge.flow_spill_slots) {
                    future_candidates += slot.candidate_after_flow_frame_reservation ? 1U : 0U;
                    current_candidates += slot.usable_in_current_active_frame ? 1U : 0U;
                }
                out << "  EDGE " << edge.edge_id << ' ' << edge.predecessor_block_id << "->" << edge.successor_block_id
                    << " G4_live=" << edge.g4_live_values
                    << " flow_slots=" << edge.flow_spill_slots.size()
                    << " future_candidates=" << future_candidates
                    << " current_usable=" << current_candidates << "\n";
                for (const auto& slot : edge.flow_spill_slots) {
                    out << "   FLOW_SPILL_SLOT " << slot.slot_index
                        << " disp=" << slot.hypothetical_frame_displacement
                        << " map=" << slot.frame_mapping_available
                        << " active=" << slot.active_frame_reserved
                        << " owners=" << slot.owners.size()
                        << " endpoint_uses=" << slot.live_endpoint_uses
                        << " endpoint_values=";
                    for (std::size_t v = 0U; v < slot.live_endpoint_value_ids.size(); ++v) {
                        if (v != 0U) out << ',';
                        out << slot.live_endpoint_value_ids[v];
                    }
                    out << " future_candidate=" << slot.candidate_after_flow_frame_reservation
                        << " current_usable=" << slot.usable_in_current_active_frame
                        << " G7_memory_temp=" << slot.current_g7_accepts_spill_slot_temporary << "\n";
                }
            }
        }
    }
    return out.str();
}

bool write_text_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return false;
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return static_cast<bool>(output);
}

void print_diagnostics(const char* label, const std::vector<Diagnostic>& diagnostics) {
    for (const auto& item : diagnostics)
        std::cerr << label << ' ' << item.code << " | " << item.message
                  << " | line=" << item.span.start.line << " | column=" << item.span.start.column << '\n';
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 4 || std::string(argv[1]) != "--out") {
        std::cerr << "usage: xfrontend_n113_g21_frame_scratch --out <output-directory> <source-file> [source-file ...]\n";
        return 2;
    }
    const std::filesystem::path output_directory(argv[2]);
    std::error_code ec;
    std::filesystem::create_directories(output_directory, ec);
    if (ec) {
        std::cerr << "N113_G21_OUTPUT | FAILED | " << ec.message() << '\n';
        return 3;
    }

    std::vector<NativeProgramModuleInput> modules;
    modules.reserve(static_cast<std::size_t>(argc - 3));
    for (int index = 3; index < argc; ++index) {
        const std::filesystem::path source_path(argv[index]);
        std::ifstream input(source_path, std::ios::binary);
        if (!input) {
            std::cerr << "N113_G21_INPUT | FAILED | Cannot open source: " << source_path.string() << '\n';
            return 4;
        }
        std::ostringstream source;
        source << input.rdbuf();
        if (!input.good() && !input.eof()) {
            std::cerr << "N113_G21_INPUT | FAILED | Cannot read source: " << source_path.string() << '\n';
            return 5;
        }
        std::string content = source.str();
        if (content.size() >= 3U && static_cast<unsigned char>(content[0]) == 0xEFU &&
            static_cast<unsigned char>(content[1]) == 0xBBU && static_cast<unsigned char>(content[2]) == 0xBFU)
            content.erase(0U, 3U);
        auto parsed = parse_source(content);
        if (!parsed.succeeded()) {
            std::cerr << "N113_G21_PARSE | FAILED | " << source_path.string() << '\n';
            print_diagnostics("PARSE", parsed.diagnostics);
            return 6;
        }
        auto unit = std::make_shared<CompilationUnit>(std::move(parsed.unit));
        if (unit->module_name.empty()) {
            std::cerr << "N113_G21_PARSE | FAILED | missing module declaration\n";
            return 7;
        }
        modules.push_back(NativeProgramModuleInput{source_path.string(), std::move(unit)});
    }

    X64CodegenOptions options{"n113-g21-frame-scratch", make_coff_x86_64_object_target()};
    options.audit_n113_flow_edge_bundle_eligibility = true;
    options.audit_n113_frame_spill_scratch = true;
    options.reserve_n113_flow_frame_slots = false;
    options.insert_n113_emitter_spill_reload_actions = false;
    const auto compiled = compile_x64_coff_program(modules, std::move(options));
    if (!compiled.succeeded()) {
        std::cerr << "N113_G21_CODEGEN | FAILED | compile pipeline failed\n";
        print_diagnostics("CODEGEN", compiled.diagnostics);
        return 8;
    }

    std::uint64_t functions = 0U;
    std::uint64_t edges = 0U;
    std::uint64_t planned_flow_spills = 0U;
    std::uint64_t hypothetical_candidates = 0U;
    std::uint64_t active_candidates = 0U;
    std::uint64_t endpoint_uses = 0U;
    for (const auto& module : compiled.modules) {
        const auto& stats = module.codegen.stats;
        if (stats.native_n113_emitter_actions_inserted != 0U ||
            stats.native_n113_emitter_machine_moves_inserted != 0U ||
            stats.native_n113_emitter_transfer_bytes_inserted != 0U ||
            stats.native_n113_flow_edge_machine_ready_plans != 0U) {
            std::cerr << "N113_G21_GUARD | FAILED | insertion or machine-ready counter changed\n";
            return 9;
        }
        for (const auto& function : stats.native_n113_frame_scratch_diagnostic_function_reports) {
            ++functions;
            planned_flow_spills += function.flow_spill_slots_planned;
            for (const auto& edge : function.edges) {
                ++edges;
                for (const auto& slot : edge.flow_spill_slots) {
                    hypothetical_candidates += slot.candidate_after_flow_frame_reservation ? 1U : 0U;
                    active_candidates += slot.usable_in_current_active_frame ? 1U : 0U;
                    endpoint_uses += slot.live_endpoint_uses;
                    if (slot.candidate_after_flow_frame_reservation !=
                        (slot.frame_mapping_available && slot.live_endpoint_uses == 0U)) {
                        std::cerr << "N113_G21_RECONCILIATION | FAILED | candidate predicate mismatch\n";
                        return 10;
                    }
                    if (slot.usable_in_current_active_frame && !slot.active_frame_reserved) {
                        std::cerr << "N113_G21_RECONCILIATION | FAILED | hypothetical slot marked active\n";
                        return 11;
                    }
                }
            }
        }
    }
    if (functions == 0U || edges == 0U) {
        std::cerr << "N113_G21_RECONCILIATION | FAILED | no function/edge frame diagnostics\n";
        return 12;
    }

    const auto json_path = output_directory / "N113_G21_Frame_Scratch.json";
    const auto text_path = output_directory / "N113_G21_Frame_Scratch.txt";
    const auto json = build_json(compiled);
    const auto text = build_text(compiled);
    if (!write_text_file(json_path, json) || !write_text_file(text_path, text)) {
        std::cerr << "N113_G21_OUTPUT | FAILED | could not write TXT/JSON report\n";
        return 13;
    }
    std::cout << "N113_G21_FRAME_SCRATCH | RESULT=PASSED"
              << " | FUNCTIONS=" << functions
              << " | EDGES=" << edges
              << " | FLOW_SPILL_SLOTS_PLANNED=" << planned_flow_spills
              << " | PER_EDGE_FUTURE_CANDIDATES=" << hypothetical_candidates
              << " | PER_EDGE_CURRENT_ACTIVE_CANDIDATES=" << active_candidates
              << " | G4_ENDPOINT_SLOT_USES=" << endpoint_uses
              << " | FRAME_MUTATION=NOT_PERFORMED"
              << " | UNWIND_MUTATION=NOT_PERFORMED"
              << " | MACHINE_CODE_INSERTION=NOT_PERFORMED"
              << " | INSERTION_READY=FALSE\n"
              << "JSON=" << json_path.string() << '\n'
              << "TXT=" << text_path.string() << '\n';
    return 0;
}
