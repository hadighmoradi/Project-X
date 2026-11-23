#include "xfrontend/system_linker.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace xfrontend {
namespace {
void add_diag(std::vector<Diagnostic>& diagnostics, const char* code, const std::string& message) {
    diagnostics.push_back(Diagnostic{code, message, {}});
}

bool valid_argument(const std::string& value) {
    return !value.empty() && value.find('\0') == std::string::npos &&
           value.find('\n') == std::string::npos && value.find('\r') == std::string::npos;
}

bool valid_library_name(const std::string& value) {
    if (value.empty()) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '+' || ch == '.';
    });
}

std::string quote_windows(const std::string& argument) {
    const bool needs_quotes = argument.empty() || argument.find_first_of(" \t\"") != std::string::npos;
    if (!needs_quotes) return argument;
    std::string out = "\"";
    std::size_t slashes = 0U;
    for (const char ch : argument) {
        if (ch == '\\') { ++slashes; continue; }
        if (ch == '"') {
            out.append(slashes * 2U + 1U, '\\');
            out.push_back('"');
        } else {
            out.append(slashes, '\\');
            out.push_back(ch);
        }
        slashes = 0U;
    }
    out.append(slashes * 2U, '\\');
    out.push_back('"');
    return out;
}

std::string quote_gnu(const std::string& argument) {
    const bool needs_quotes = argument.empty() || argument.find_first_of(" \t\"") != std::string::npos;
    if (!needs_quotes) return argument;
    std::string out = "\"";
    for (const char ch : argument) {
        if (ch == '\\' || ch == '"') out.push_back('\\');
        out.push_back(ch);
    }
    out.push_back('"');
    return out;
}

} // namespace

std::string quote_system_linker_argument(SystemLinkerKind kind, const std::string& argument) {
    if (kind == SystemLinkerKind::MsvcLink || kind == SystemLinkerKind::LldLink) return quote_windows(argument);
    return quote_gnu(argument);
}

SystemLinkerPlan create_system_linker_plan(const SystemLinkerRequest& request) {
    SystemLinkerPlan plan;
    const bool windows = request.kind == SystemLinkerKind::MsvcLink || request.kind == SystemLinkerKind::LldLink;
    if (request.kind != SystemLinkerKind::GnuLd && !windows) {
        add_diag(plan.diagnostics, "XLDR0001", "unsupported system linker kind"); return plan;
    }
    plan.executable = request.executable;
    if (plan.executable.empty()) {
        plan.executable = request.kind == SystemLinkerKind::GnuLd ? "ld.lld" :
            (request.kind == SystemLinkerKind::LldLink ? "lld-link" : "link.exe");
    }
    if (!valid_argument(plan.executable) || !valid_argument(request.output_path)) {
        add_diag(plan.diagnostics, "XLDR0002", "linker executable and output path must be non-empty and contain no NUL or line breaks");
        return plan;
    }
    if (request.object_paths.empty() && request.archive_paths.empty()) {
        add_diag(plan.diagnostics, "XLDR0003", "at least one object or archive input is required"); return plan;
    }
    if (!request.entry_symbol.empty() && !valid_argument(request.entry_symbol)) {
        add_diag(plan.diagnostics, "XLDR0004", "entry symbol contains a forbidden NUL or line break"); return plan;
    }
    const auto validate_paths = [&plan](const std::vector<std::string>& values, const char* label) {
        for (const auto& value : values) {
            if (!valid_argument(value)) {
                add_diag(plan.diagnostics, "XLDR0005", std::string(label) + " contains an empty path or a path with NUL/line break");
                return false;
            }
        }
        return true;
    };
    if (!validate_paths(request.object_paths, "object inputs") ||
        !validate_paths(request.archive_paths, "archive inputs") ||
        !validate_paths(request.library_directories, "library directories")) return plan;
    for (const auto& library : request.libraries) {
        if (!valid_library_name(library)) {
            add_diag(plan.diagnostics, "XLDR0006", "library name contains unsupported characters or path separators"); return plan;
        }
    }

    auto& args = plan.arguments;
    if (request.kind == SystemLinkerKind::GnuLd) {
        args.push_back("-o"); args.push_back(request.output_path);
        if (!request.entry_symbol.empty()) { args.push_back("-e"); args.push_back(request.entry_symbol); }
        if (request.static_link) args.push_back("-static");
        for (const auto& directory : request.library_directories) {
            args.push_back("-L" + directory);
        }
        for (const auto& object : request.object_paths) args.push_back(object);
        for (const auto& archive : request.archive_paths) args.push_back(archive);
        for (const auto& library : request.libraries) args.push_back("-l" + library);
    } else {
        args.push_back("/MACHINE:X64");
        args.push_back("/OUT:" + request.output_path);
        args.push_back(request.console_subsystem ? "/SUBSYSTEM:CONSOLE" : "/SUBSYSTEM:WINDOWS");
        args.push_back("/INCREMENTAL:NO");
        if (!request.entry_symbol.empty()) args.push_back("/ENTRY:" + request.entry_symbol);
        if (request.disable_default_libraries) args.push_back("/NODEFAULTLIB");
        for (const auto& directory : request.library_directories) args.push_back("/LIBPATH:" + directory);
        for (const auto& object : request.object_paths) args.push_back(object);
        for (const auto& archive : request.archive_paths) args.push_back(archive);
        for (const auto& library : request.libraries) args.push_back(library + ".lib");
        // MSVC's /MT is a compiler runtime-selection flag, not a link.exe switch.
        // Static COFF libraries are supplied explicitly as .lib inputs instead.
    }

    std::ostringstream response;
    for (const auto& argument : args) {
        response << quote_system_linker_argument(request.kind, argument) << '\n';
    }
    plan.response_file_contents = response.str();
    return plan;
}

} // namespace xfrontend
