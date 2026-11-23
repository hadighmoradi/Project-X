#pragma once

#include "xfrontend/parser.hpp"
#include "xfrontend/module_resolver.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace xfrontend {

class SourceManager;

struct SemanticFunctionInfo {
    std::string name;
    std::string signature;
    std::string inferred_return_type;
    std::size_t required_parameter_count = 0;
    std::size_t total_parameter_count = 0;
    Visibility visibility = Visibility::Private;
};

struct SemanticResult {
    std::vector<Diagnostic> diagnostics;
    std::vector<SemanticFunctionInfo> functions;

    bool succeeded() const noexcept { return diagnostics.empty(); }
};

// Native semantic pass for the currently implemented .src AST subset.
// It resolves function/local names, validates lexical scopes and basic types,
// checks local and imported calls by overload arity/type, consumes typed public
// module interfaces, propagates inferred exported return types across an acyclic
// module graph, and infers consistent local return types. It does not claim
// complete generic constraint solving, user-defined type checking, borrow/
// lifetime analysis, or production-grade whole-program type checking.
class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(const CompilationUnit& unit);
    SemanticResult run();

private:
    const CompilationUnit& unit_;
    SemanticResult result_;
};

SemanticResult analyze_semantics(const CompilationUnit& unit);
SemanticResult analyze_source(const std::string& source);
SemanticResult analyze_source(const SourceManager& sources, SourceFileId source_id);
SemanticResult analyze_program(const std::vector<ProgramFile>& files);
SemanticResult analyze_program(const SourceManager& sources,
                               const std::vector<SourceFileId>& source_ids);

} // namespace xfrontend
