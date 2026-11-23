#pragma once

#include "xfrontend/object_file.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xfrontend {

struct LinkerOptions {
    std::uint64_t image_base = 0x00400000ULL;
    std::size_t maximum_image_size = 256U * 1024U * 1024U;
    std::string entry_symbol;
    bool extract_archive_members = true;
    bool allow_unresolved_weak = true;
    // Internal linker roots used for DLL exports, runtime callbacks and externally
    // discoverable symbols. Normal X source code does not need to set these.
    std::vector<std::string> preserved_symbols;
};

struct LinkedImageSection {
    std::string object_id;
    ObjectSectionId source_section_id = InvalidObjectSectionId;
    std::string name;
    ObjectSectionKind kind = ObjectSectionKind::Text;
    std::uint64_t address = 0U;
    std::size_t alignment = 1U;
    std::size_t memory_size = 0U;
    std::vector<std::uint8_t> contents;
};

struct LinkedImageSymbol {
    std::string name;
    std::string object_id;
    ObjectSectionId section_id = InvalidObjectSectionId;
    std::uint64_t address = 0U;
    std::size_t size = 0U;
    ObjectSymbolBinding binding = ObjectSymbolBinding::Global;
    ObjectSymbolKind kind = ObjectSymbolKind::Function;
};

struct AppliedRelocation {
    std::string source_object_id;
    ObjectSectionId source_section_id = InvalidObjectSectionId;
    std::size_t offset = 0U;
    ObjectRelocationKind kind = ObjectRelocationKind::Absolute64;
    std::string target_symbol;
    std::uint64_t place_address = 0U;
    std::uint64_t symbol_address = 0U;
    std::int64_t addend = 0;
    std::int64_t signed_result = 0;
    std::uint64_t unsigned_result = 0U;
    std::size_t width = 0U;
    bool weak_zero_fallback = false;
};

struct LinkedImageOptimizationReport {
    bool section_elimination_applied = false;
    std::size_t input_section_count = 0U;
    std::size_t retained_section_count = 0U;
    std::size_t discarded_section_count = 0U;
    std::size_t input_section_bytes = 0U;
    std::size_t retained_section_bytes = 0U;
    std::size_t discarded_section_bytes = 0U;
};

struct LinkedImage {
    ObjectFileDescriptor descriptor;
    std::uint64_t image_base = 0U;
    std::size_t image_size = 0U;
    std::vector<std::uint8_t> flat_bytes;
    std::vector<LinkedImageSection> sections;
    std::vector<LinkedImageSymbol> symbols;
    std::vector<AppliedRelocation> applied_relocations;
    std::uint64_t entry_address = 0U;
    bool has_entry = false;
    LinkedImageOptimizationReport optimization;
};

struct LinkerResult {
    LinkedImage image;
    ObjectLinkPlanResult link_plan;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty() && image.image_size != 0U; }
};

// N24 performs deterministic section layout, static archive extraction,
// symbol-address resolution and supported x86-64 relocation application.
// The result is an in-memory linked image, not yet a PE/ELF executable file.
LinkerResult link_object_files(
    const ObjectFileDescriptor& target,
    const std::vector<ObjectFile>& input_objects,
    const std::vector<StaticArchive>& archives = {},
    LinkerOptions options = {});

} // namespace xfrontend
