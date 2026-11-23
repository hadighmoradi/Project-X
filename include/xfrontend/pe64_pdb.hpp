#pragma once

#include "xfrontend/binary_object.hpp"
#include "xfrontend/pe64_debug_signing.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xfrontend {

enum class Pe64CodeViewPublicKind {
    Function,
    Data
};

struct Pe64CodeViewPublicSymbol {
    std::string name;
    ObjectSectionId section_id = InvalidObjectSectionId;
    std::uint32_t offset = 0U;
    Pe64CodeViewPublicKind kind = Pe64CodeViewPublicKind::Function;
};

struct Pe64CodeViewCoffSpec {
    std::string object_name;
    std::vector<Pe64CodeViewPublicSymbol> public_symbols;
};

struct Pe64CodeViewCoffResult {
    std::vector<std::uint8_t> bytes;
    std::size_t public_symbol_count = 0U;
    std::size_t debug_section_bytes = 0U;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty() && !bytes.empty(); }
};

struct Pe64CodeViewCoffInspection {
    bool present = false;
    bool valid = false;
    std::string object_name;
    std::vector<Pe64CodeViewPublicSymbol> public_symbols;
    std::string error;
};

// Adds a CodeView C13 .debug$S section with S_OBJNAME and S_PUB32 records to
// an AMD64 COFF ObjectFile, then serializes it using the native COFF writer.
// The resulting .obj is consumed by link.exe /DEBUG to produce a real PDB.
Pe64CodeViewCoffResult write_pe64_codeview_coff_object(
    const ObjectFile& object, const Pe64CodeViewCoffSpec& spec);

Pe64CodeViewCoffInspection inspect_pe64_codeview_coff_object(
    const std::vector<std::uint8_t>& object_bytes);

struct Pe64PdbInspection {
    bool present = false;
    bool valid = false;
    std::uint32_t version = 0U;
    std::uint32_t signature = 0U;
    std::uint32_t age = 0U;
    std::array<std::uint8_t, 16U> guid{};
    std::uint32_t page_size = 0U;
    std::uint32_t page_count = 0U;
    std::uint32_t stream_count = 0U;
    std::string error;
};

// Reads the MSF 7.0 container and PDB Information stream (stream 1). This is
// structural validation, not certificate/trust verification.
Pe64PdbInspection inspect_pe64_msf_pdb(const std::vector<std::uint8_t>& pdb_bytes);

struct Pe64PdbPairInspection {
    bool valid = false;
    bool guid_matches = false;
    bool age_matches = false;
    Pe64CodeViewInspection codeview;
    Pe64PdbInspection pdb;
    std::string error;
};

// Confirms that PE CodeView RSDS identity matches the PDB Info stream GUID/Age.
Pe64PdbPairInspection verify_pe64_pdb_pair(
    const std::vector<std::uint8_t>& pe_bytes,
    const std::vector<std::uint8_t>& pdb_bytes);

} // namespace xfrontend
