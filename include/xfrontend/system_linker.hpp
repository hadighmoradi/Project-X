// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/archive.hpp"

#include <string>
#include <vector>

namespace xfrontend {

enum class SystemLinkerKind : std::uint8_t { GnuLd = 1U, LldLink = 2U, MsvcLink = 3U };

struct SystemLinkerRequest {
    SystemLinkerKind kind = SystemLinkerKind::GnuLd;
    std::string executable;
    std::string output_path;
    std::string entry_symbol;
    std::vector<std::string> object_paths;
    std::vector<std::string> archive_paths;
    std::vector<std::string> library_directories;
    std::vector<std::string> libraries;
    bool static_link = false;
    bool console_subsystem = true;
    bool disable_default_libraries = false;
};

struct SystemLinkerPlan {
    std::string executable;
    std::vector<std::string> arguments;
    std::string response_file_contents;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty() && !executable.empty(); }
};

// Builds deterministic argument vectors and response-file text for the host
// linker. It does not launch a process; the build driver owns process execution.
SystemLinkerPlan create_system_linker_plan(const SystemLinkerRequest& request);
std::string quote_system_linker_argument(SystemLinkerKind kind, const std::string& argument);

} // namespace xfrontend
