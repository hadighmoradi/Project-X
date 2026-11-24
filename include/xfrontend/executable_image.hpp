// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/linker.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xfrontend {

enum class ExecutableSegmentPermission : std::uint32_t {
    Read = 4U,
    ReadExecute = 5U,
    ReadWrite = 6U
};

enum class ExecutableEntryAbi {
    SysVAMD64ProcessEntry
};

struct ExecutableStartupOptions {
    std::string entry_symbol = "_start";
    // N25 deliberately supports direct process entry only. These options are
    // explicit so a future CRT/runtime pipeline cannot silently be implied.
    bool use_c_runtime = false;
    bool run_static_initializers = false;
    bool run_tls_initializers = false;
    bool request_dynamic_interpreter = false;
};

struct ExecutableStartupPlan {
    std::string entry_symbol;
    std::uint64_t entry_address = 0U;
    ExecutableEntryAbi entry_abi = ExecutableEntryAbi::SysVAMD64ProcessEntry;
    std::size_t stack_alignment = 16U;
    bool initial_stack_contains_argc_argv_envp = true;
    bool uses_c_runtime = false;
    bool runs_static_initializers = false;
    bool runs_tls_initializers = false;
    bool uses_dynamic_interpreter = false;
    bool direct_entry = true;
};

struct Elf64ExecutableOptions {
    std::size_t page_size = 4096U;
    std::size_t maximum_file_size = 512U * 1024U * 1024U;
    ExecutableStartupOptions startup;
};

struct ExecutableLoadSegment {
    std::uint64_t virtual_address = 0U;
    std::uint64_t file_offset = 0U;
    std::size_t file_size = 0U;
    std::size_t memory_size = 0U;
    std::size_t alignment = 4096U;
    ExecutableSegmentPermission permission = ExecutableSegmentPermission::Read;
};

struct ExecutableImage {
    ObjectFileDescriptor descriptor;
    std::vector<std::uint8_t> bytes;
    std::vector<ExecutableLoadSegment> load_segments;
    ExecutableStartupPlan startup;
    std::uint64_t entry_address = 0U;
    std::uint64_t image_base = 0U;
    bool has_non_executable_stack = true;
    bool has_dynamic_interpreter = false;
    bool has_c_runtime = false;
};

struct ExecutableImageResult {
    ExecutableImage image;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty() && image.bytes.size() >= 64U; }
};

// Writes a minimal static ELF64 ET_EXEC container for Linux/x86-64. The
// LinkedImage must already contain relocated machine-code bytes and an explicit
// external Function entry symbol (normally _start). No CRT, dynamic loader,
// language runtime, constructors, TLS setup, or machine-code generation occurs.
ExecutableImageResult write_elf64_executable_image(
    const LinkedImage& linked_image,
    Elf64ExecutableOptions options = {});

} // namespace xfrontend
