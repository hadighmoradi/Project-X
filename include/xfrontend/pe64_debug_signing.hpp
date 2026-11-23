#pragma once

#include "xfrontend/pe64_dll.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xfrontend {

// Builds the standard PE IMAGE_DEBUG_DIRECTORY + CodeView RSDS record.
// The referenced PDB is external; this writer does not synthesize Microsoft's
// native PDB database format.
struct Pe64CodeViewSpec {
    std::array<std::uint8_t, 16U> guid{}; // on-disk GUID byte order
    std::uint32_t age = 1U;
    std::uint32_t timestamp = 0U;
    std::string pdb_path;
};

struct Pe64DebugInfo {
    std::uint32_t directory_rva = 0U;
    std::uint32_t directory_size = 0U;
    std::uint32_t codeview_rva = 0U;
    std::uint32_t codeview_size = 0U;
    std::uint32_t codeview_file_offset = 0U;
    std::uint32_t timestamp = 0U;
};

struct Pe64DebugResult {
    Pe64DllImage image;
    Pe64DebugInfo debug;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept {
        return diagnostics.empty() && image.bytes.size() >= 0x200U &&
            debug.directory_rva != 0U && debug.codeview_rva != 0U;
    }
};

Pe64DebugResult write_pe64_codeview_debug_directory(
    const Pe64DllImage& input, const Pe64CodeViewSpec& spec);

struct Pe64CodeViewInspection {
    bool present = false;
    bool valid = false;
    std::uint32_t directory_rva = 0U;
    std::uint32_t directory_size = 0U;
    std::uint32_t codeview_rva = 0U;
    std::uint32_t codeview_file_offset = 0U;
    std::uint32_t codeview_size = 0U;
    std::uint32_t timestamp = 0U;
    std::array<std::uint8_t, 16U> guid{};
    std::uint32_t age = 0U;
    std::string pdb_path;
    std::string error;
};

Pe64CodeViewInspection inspect_pe64_codeview_debug_directory(
    const std::vector<std::uint8_t>& image);

struct Pe64CertificateTableInfo {
    bool present = false;
    bool valid = false;
    std::uint32_t file_offset = 0U;
    std::uint32_t size = 0U;
    std::size_t entry_count = 0U;
    std::size_t pkcs7_entry_count = 0U;
    std::string error;
};

// Structural inspection only. Cryptographic/trust verification is delegated
// to SignTool / WinVerifyTrust; this routine does not claim a valid signature.
Pe64CertificateTableInfo inspect_pe64_certificate_table(
    const std::vector<std::uint8_t>& image);

} // namespace xfrontend
