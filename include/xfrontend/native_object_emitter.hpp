// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/object_file.hpp"
#include "xfrontend/parser.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xfrontend {

constexpr std::size_t InvalidNativeFunctionIndex = static_cast<std::size_t>(-1);

// A relocation emitted by the native code generator. A target_function_index
// names a function declared in the same CompilationUnit; otherwise target_link_name
// must contain an already-resolved external linker symbol (for example, a symbol
// imported from a typed module interface or a standard-library archive).
struct NativeObjectRelocationInput {
    std::size_t offset = 0U;
    ObjectRelocationKind kind = ObjectRelocationKind::PcRelative32;
    std::size_t width = 4U;
    std::size_t target_function_index = InvalidNativeFunctionIndex;
    std::string target_link_name;
    std::string target_signature_key;
    ObjectSymbolKind target_kind = ObjectSymbolKind::Function;
    std::int64_t addend = 0;
};

// Machine code for one source function. The backend supplies the bytes; this
// emitter gives the function its own independently collectible object section.
struct NativeFunctionCodeArtifact {
    std::size_t function_index = InvalidNativeFunctionIndex;
    std::vector<std::uint8_t> code;
    std::size_t alignment = 16U;
    bool linker_root = false;
    std::vector<NativeObjectRelocationInput> relocations;
    // Encoded AMD64 UNWIND_INFO v1. When present, the object emitter creates
    // associative .pdata$ and .xdata$ COMDAT sections for this function.
    std::vector<std::uint8_t> win64_unwind_info;
};

// Storage emitted for one source-level global. BSS has a logical size and no
// file bytes. Registration/initializer storage can be explicitly marked root.
struct NativeDataCodeArtifact {
    NativeSymbolDeclaration declaration;
    std::vector<std::uint8_t> contents;
    std::size_t logical_size = 0U;
    std::size_t alignment = 8U;
    bool read_only = false;
    bool bss = false;
    bool linker_root = false;
    std::vector<NativeObjectRelocationInput> relocations;
};

struct NativeObjectEmissionOptions {
    std::string object_id;
    ObjectFileDescriptor target = make_coff_x86_64_object_target();
};

struct NativeObjectEmissionResult {
    ObjectFile object_file;
    NativeSymbolTableResult symbol_table;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty(); }
};

// Bridges the parsed X source declarations to the existing object/linker model.
// Every defined function and every global data artifact becomes its own section.
// This is the object-section emission boundary; instruction selection and machine
// code generation remain responsibilities of the target backend.
NativeObjectEmissionResult emit_native_object_sections(
    const CompilationUnit& unit,
    const std::vector<NativeFunctionCodeArtifact>& functions,
    const std::vector<NativeDataCodeArtifact>& data = {},
    NativeObjectEmissionOptions options = {});

} // namespace xfrontend
