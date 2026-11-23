#pragma once

#include "xfrontend/object_file.hpp"

#include <cstdint>
#include <vector>

namespace xfrontend {

struct BinaryObjectWriteResult {
    std::vector<std::uint8_t> bytes;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty() && !bytes.empty(); }
};

struct BinaryObjectReadResult {
    ObjectFile object_file;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty(); }
};

// N23 binary object support is deliberately limited to ELF64 x86-64 and
// AMD64 COFF. Mach-O and AArch64 remain modeled by N22 but are not serialized
// by this version. The writer emits relocatable object files (not executables).
BinaryObjectWriteResult write_binary_object(const ObjectFile& object_file);
BinaryObjectReadResult read_binary_object(const std::vector<std::uint8_t>& bytes);

} // namespace xfrontend
