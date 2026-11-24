// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/diagnostic.hpp"
#include "xfrontend/parser.hpp"

#include <vector>

namespace xfrontend {

class SourceManager;

struct LifetimeResult {
    std::vector<Diagnostic> diagnostics;

    bool succeeded() const noexcept { return diagnostics.empty(); }
};

// Conservative lifetime/ownership contract pass for the currently supported AST subset.
// It tracks flow-sensitive local pointer origins, local-call borrow conflicts,
// and direct Unique.Get() provenance. It is not a complete borrow checker,
// NLL implementation, general pointer provenance solver, or Move-state checker.
LifetimeResult analyze_lifetimes(const CompilationUnit& unit);
LifetimeResult analyze_lifetimes(const SourceManager& sources, SourceFileId source_id);

} // namespace xfrontend
