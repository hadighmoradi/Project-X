// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/lifetime.hpp"
#include "test_report.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void matrix_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

xfrontend::LifetimeResult matrix_lifetime_result(const std::string& source) {
    const auto parsed = xfrontend::parse_source(source);
    if (!parsed.succeeded()) {
        throw std::runtime_error("matrix source failed to parse: " +
            parsed.diagnostics.front().code + " " + parsed.diagnostics.front().message +
            " | source=" + source);
    }
    return xfrontend::analyze_lifetimes(parsed.unit);
}

bool matrix_has_code(const xfrontend::LifetimeResult& result, const std::string& code) {
    for (const auto& diagnostic : result.diagnostics) {
        if (diagnostic.code == code) return true;
    }
    return false;
}

void matrix_case(const std::string& name, const std::function<void()>& body,
                 int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    const auto start = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        std::cout << "PASS " << name << '\n';
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        std::cout << "FAIL " << name << ": " << error.what() << '\n';
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

void expect_invalid(const std::string& source, const std::string& code) {
    const auto result = matrix_lifetime_result(source);
    matrix_require(matrix_has_code(result, code), "expected diagnostic " + code);
}

void expect_valid(const std::string& source) {
    const auto result = matrix_lifetime_result(source);
    matrix_require(result.succeeded(), "unexpected diagnostic: " +
        (result.diagnostics.empty() ? std::string() : result.diagnostics.front().code + " " +
         result.diagnostics.front().message));
}

std::string alias_chain(const std::string& root, int depth,
                        const std::string& prefix = "alias") {
    std::string previous = root;
    std::string statements;
    for (int index = 0; index <= depth; ++index) {
        const std::string current = prefix + std::to_string(index);
        statements += current + " = " + previous + "; ";
        previous = current;
    }
    return statements;
}

std::string final_alias(int depth, const std::string& prefix = "alias") {
    return prefix + std::to_string(depth);
}

std::string owner_expression(const std::string& type, const std::string& name) {
    return type + " " + name + " = " + type + ".Create(); ";
}

std::string numbered_name(int value) {
    return value < 10 ? "00" + std::to_string(value) :
           value < 100 ? "0" + std::to_string(value) : std::to_string(value);
}

} // namespace

void run_ownership_matrix_tests(int& passed, int& failed) {
    // Matrix A (100): stale Unique.Get aliases across mutation operations,
    // alias depths, use contexts, and case-folded identifiers.
    {
        const std::vector<std::string> operations{"Reset", "Take", "Release"};
        const std::vector<std::string> uses{
            "PrintLine %ALIAS%;",
            "%ALIAS%.Ping();",
            "sink = %ALIAS%;",
            "return %ALIAS%;"
        };
        int index = 109;
        for (const auto& operation : operations) {
            for (int depth = 0; depth < 5; ++depth) {
                for (const auto& use : uses) {
                    const std::string alias = final_alias(depth);
                    std::string use_statement = use;
                    const auto marker = use_statement.find("%ALIAS%");
                    use_statement.replace(marker, std::string("%ALIAS%").size(), alias);
                    const std::string head = use.find("return") != std::string::npos
                        ? "function Probe() returns *Device { " : "function Probe() { ";
                    const std::string source = head +
                        owner_expression("Unique[Device]", "owner") +
                        alias_chain("owner.Get()", depth) +
                        "owner." + operation + "(); " + use_statement + " }";
                    const std::string test_name = "T" + numbered_name(index++) +
                        "_unique_stale_" + operation + "_depth" + std::to_string(depth) +
                        "_use" + std::to_string(static_cast<int>(&use - &uses[0]));
                    matrix_case(test_name, [source] { expect_invalid(source, "XLIFE0005"); }, passed, failed);
                }
            }
        }
        const std::vector<std::string> owners{"Owner", "OWNER"};
        for (const auto& owner : owners) {
            for (int depth = 0; depth < 5; ++depth) {
                for (int use_index = 0; use_index < 4; ++use_index) {
                    const std::string alias = final_alias(depth, "Ref");
                    std::string use_statement = uses[static_cast<std::size_t>(use_index)];
                    const auto marker = use_statement.find("%ALIAS%");
                    use_statement.replace(marker, std::string("%ALIAS%").size(), alias);
                    const std::string head = use_index == 3
                        ? "function Probe() returns *Device { " : "function Probe() { ";
                    const std::string source = head +
                        owner_expression("Unique[Device]", owner) +
                        alias_chain(owner + ".Get()", depth, "Ref") +
                        owner + ".Release(); " + use_statement + " }";
                    const std::string test_name = "T" + numbered_name(index++) +
                        "_casefold_owner_" + owner + "_depth" + std::to_string(depth) +
                        "_use" + std::to_string(use_index);
                    matrix_case(test_name, [source] { expect_invalid(source, "XLIFE0005"); }, passed, failed);
                }
            }
        }
    }

    // Matrix B (100): valid recovery paths after owner mutation. These assert
    // that stale state is killed by overwrite and that resource generations
    // remain independent after reset/recreation.
    {
        int index = 209;
        for (int scenario = 0; scenario < 10; ++scenario) {
            for (int depth = 0; depth < 10; ++depth) {
                std::string source = "function Probe(*Device external) { ";
                source += owner_expression("Unique[Device]", "owner");
                source += owner_expression("Unique[Device]", "other");
                source += alias_chain("owner.Get()", depth);
                const std::string alias = final_alias(depth);
                switch (scenario) {
                case 0: // Clear the last alias with null after invalidation.
                    source += "owner.Reset(); " + alias + " = null; PrintLine " + alias + "; ";
                    break;
                case 1: // Replace with a non-borrow raw pointer parameter.
                    source += "owner.Take(); " + alias + " = external; PrintLine " + alias + "; ";
                    break;
                case 2: // Invalidate a different owner.
                    source += "other.Release(); PrintLine " + alias + "; ";
                    break;
                case 3: // Recreate owner and use only a fresh generation.
                    source += "owner.Reset(); owner = Unique[Device].Create(); fresh = owner.Get(); PrintLine fresh; ";
                    break;
                case 4: // Rebind the old alias directly to the new generation.
                    source += "owner.Release(); owner = Unique[Device].Create(); " + alias +
                              " = owner.Get(); PrintLine " + alias + "; ";
                    break;
                case 5: // Invalidate other owner, then use an alias from owner.
                    source += "other.Take(); PrintLine " + alias + "; ";
                    break;
                case 6: // Invalidate owner; replace final alias with a literal.
                    source += "owner.Release(); " + alias + " = null; sink = " + alias + "; ";
                    break;
                case 7: // Reinitialize owner multiple times before taking a new Borrow.
                    source += "owner.Reset(); owner = Unique[Device].Create(); owner.Reset(); "
                              "owner = Unique[Device].Create(); fresh = owner.Get(); PrintLine fresh; ";
                    break;
                case 8: // Use a fresh alias from an independent UniqueArray owner.
                    source += "UniqueArray[Device] arrayOwner = UniqueArray[Device].Create(); "
                              "owner.Reset(); arrayRef = arrayOwner.Get(); PrintLine arrayRef; ";
                    break;
                default: // Overwrite final alias from independent owner after reset.
                    source += "owner.Take(); " + alias + " = other.Get(); PrintLine " + alias + "; ";
                    break;
                }
                source += "}";
                const std::string test_name = "T" + numbered_name(index++) +
                    "_ownership_recovery_s" + std::to_string(scenario) +
                    "_depth" + std::to_string(depth);
                matrix_case(test_name, [source] { expect_valid(source); }, passed, failed);
            }
        }
    }

    // Matrix C (100): direct-call borrow conflict combinations. The 10 rows
    // cover immutable upgrade, mutable reborrow liveness, paired arguments,
    // formal-pointer alias assumptions, and disjoint Unique roots.
    {
        int index = 309;
        for (int depth = 0; depth < 10; ++depth) {
            for (int scenario = 0; scenario < 10; ++scenario) {
                const std::string alias_statements = alias_chain("source", depth, "link");
                const std::string alias = final_alias(depth, "link");
                std::string source;
                bool expected_conflict = false;
                switch (scenario) {
                case 0:
                    source = "function Target(borrow mut *Device value) {} "
                             "function Probe(borrow *Device source) { Target(source); }";
                    expected_conflict = true;
                    break;
                case 1:
                    source = "function Target(borrow mut *Device value) {} "
                             "function Probe(borrow *Device source) { " + alias_statements +
                             "Target(" + alias + "); }";
                    expected_conflict = true;
                    break;
                case 2:
                    source = "function Target(borrow mut *Device value) {} "
                             "function Probe(borrow mut *Device source) { Target(source); }";
                    expected_conflict = false;
                    break;
                case 3:
                    source = "function Target(borrow mut *Device value) {} "
                             "function Probe(borrow mut *Device source) { keeper = source; Target(source); PrintLine keeper; }";
                    expected_conflict = true;
                    break;
                case 4:
                    source = "function Target(borrow mut *Device value) {} "
                             "function Probe(borrow mut *Device source) { keeper = source; PrintLine keeper; Target(source); }";
                    expected_conflict = false;
                    break;
                case 5:
                    source = "function Target(borrow mut *Device value) {} "
                             "function Probe(borrow mut *Device source) { " + alias_statements +
                             "keeper = source; Target(" + alias + "); PrintLine keeper; }";
                    expected_conflict = true;
                    break;
                case 6:
                    source = "function Target(borrow mut *Device value) {} "
                             "function Probe(borrow mut *Device source) { " + alias_statements +
                             "keeper = source; PrintLine keeper; Target(" + alias + "); }";
                    expected_conflict = false;
                    break;
                case 7:
                    source = "function Target(borrow mut *Device left, borrow mut *Device right) {} "
                             "function Probe(borrow mut *Device source) { " + alias_statements +
                             "Target(source, " + alias + "); }";
                    expected_conflict = true;
                    break;
                case 8:
                    source = "function Target(borrow *Device left, borrow *Device right) {} "
                             "function Probe(borrow *Device source) { " + alias_statements +
                             "Target(source, " + alias + "); }";
                    expected_conflict = false;
                    break;
                default:
                    source = "function Target(borrow mut *Device left, borrow *Device right) {} "
                             "function Probe(*Device first, *Device second) { Target(first, second); }";
                    expected_conflict = true;
                    break;
                }
                // Add harmless independent statements at different source offsets
                // so last-use analysis sees the same intended ordering under stress.
                if (depth > 0 && scenario != 1 && scenario != 5 && scenario != 6 &&
                    scenario != 7 && scenario != 8) {
                    source.insert(source.rfind('}'), " padding" + std::to_string(depth) + " = " +
                                  std::to_string(depth) + "; ");
                }
                const std::string test_name = "T" + numbered_name(index++) +
                    "_borrow_matrix_s" + std::to_string(scenario) +
                    "_depth" + std::to_string(depth);
                matrix_case(test_name, [source, expected_conflict] {
                    if (expected_conflict) expect_invalid(source, "XLIFE0003");
                    else expect_valid(source);
                }, passed, failed);
            }
        }
    }

    // Matrix D (100): control-flow and scope joins. Each pattern is repeated
    // across ten Borrow alias depths to detect lost origins and false poisoning.
    {
        int index = 409;
        for (int scenario = 0; scenario < 10; ++scenario) {
            for (int depth = 0; depth < 10; ++depth) {
                const std::string aliases = alias_chain("owner.Get()", depth);
                const std::string alias = final_alias(depth);
                std::string source;
                bool expected_invalid = false;
                switch (scenario) {
                case 0: // A branch may invalidate the owner, and both paths continue.
                    source = "function Probe(bool flag) { " + owner_expression("Unique[Device]", "owner") +
                             aliases + "if (flag) { owner.Reset(); } PrintLine " + alias + "; }";
                    expected_invalid = true;
                    break;
                case 1: // The invalidated branch returns, so only the live path reaches use.
                    source = "function Probe(bool flag) { " + owner_expression("Unique[Device]", "owner") +
                             aliases + "if (flag) { owner.Reset(); return; } PrintLine " + alias + "; }";
                    expected_invalid = false;
                    break;
                case 2: // Both fallthrough branches invalidate the same generation.
                    source = "function Probe(bool flag) { " + owner_expression("Unique[Device]", "owner") +
                             aliases + "if (flag) { owner.Reset(); } else { owner.Release(); } PrintLine " + alias + "; }";
                    expected_invalid = true;
                    break;
                case 3: // Both paths terminate; unreachable tail must not be inspected.
                    source = "function Probe(bool flag) { " + owner_expression("Unique[Device]", "owner") +
                             aliases + "if (flag) { owner.Reset(); return; } else { owner.Take(); return; } PrintLine " + alias + "; }";
                    expected_invalid = false;
                    break;
                case 4: // Loop may execute and invalidate the owner.
                    source = "function Probe(bool flag) { " + owner_expression("Unique[Device]", "owner") +
                             aliases + "while (flag) { owner.Reset(); } PrintLine " + alias + "; }";
                    expected_invalid = true;
                    break;
                case 5: // Use occurs after reset inside loop body.
                    source = "function Probe(bool flag) { " + owner_expression("Unique[Device]", "owner") +
                             aliases + "while (flag) { owner.Take(); PrintLine " + alias + "; } }";
                    expected_invalid = true;
                    break;
                case 6: // Owner is created in a nested scope and its alias escapes.
                    source = "function Probe() { alias0 = null; { " +
                             owner_expression("Unique[Device]", "owner") + aliases +
                             "alias0 = " + alias + "; } PrintLine alias0; }";
                    expected_invalid = true;
                    break;
                case 7: // Outer owner survives the inner scope containing the alias.
                    source = "function Probe() { " + owner_expression("Unique[Device]", "owner") +
                             "{ " + aliases + "PrintLine " + alias + "; } PrintLine " + alias + "; }";
                    expected_invalid = false;
                    break;
                case 8: // Nested block returns after invalidation; outer continuation remains valid.
                    source = "function Probe(bool flag) { " + owner_expression("Unique[Device]", "owner") +
                             aliases + "if (flag) { { owner.Release(); return; } } PrintLine " + alias + "; }";
                    expected_invalid = false;
                    break;
                default: // A loop may be skipped, but body invalidation remains a possible state.
                    source = "function Probe(bool flag) { " + owner_expression("Unique[Device]", "owner") +
                             aliases + "while (flag) { if (flag) { owner.Reset(); } } PrintLine " + alias + "; }";
                    expected_invalid = true;
                    break;
                }
                const std::string test_name = "T" + numbered_name(index++) +
                    "_flow_scope_matrix_s" + std::to_string(scenario) +
                    "_depth" + std::to_string(depth);
                matrix_case(test_name, [source, expected_invalid] {
                    if (expected_invalid) expect_invalid(source, "XLIFE0005");
                    else expect_valid(source);
                }, passed, failed);
            }
        }
    }

    // Matrix E (100): resource generations, UniqueArray, parameter ownership,
    // mixed-case method names, and independent owners.
    {
        int index = 509;
        for (int scenario = 0; scenario < 10; ++scenario) {
            for (int depth = 0; depth < 10; ++depth) {
                const std::string alias = final_alias(depth, "ref");
                const std::string aliases = alias_chain("owner.Get()", depth, "ref");
                std::string source;
                bool expected_invalid = false;
                switch (scenario) {
                case 0:
                    source = "function Probe() { " + owner_expression("UniqueArray[Device]", "owner") +
                             alias_chain("owner.Get()", depth, "ref") + "owner.Reset(); PrintLine " + alias + "; }";
                    expected_invalid = true;
                    break;
                case 1:
                    source = "function Probe() { " + owner_expression("Unique[Device]", "owner") +
                             aliases + "OwNeR.tAkE(); PrintLine " + alias + "; }";
                    expected_invalid = true;
                    break;
                case 2:
                    source = "function Probe() { " + owner_expression("UniqueArray[Device]", "owner") +
                             alias_chain("owner.Get()", depth, "ref") + "owner.Release(); PrintLine " + alias + "; }";
                    expected_invalid = true;
                    break;
                case 3:
                    source = "function Probe(take Unique[Device] owner) { " +
                             alias_chain("owner.Get()", depth, "ref") + "owner.Reset(); PrintLine " + alias + "; }";
                    expected_invalid = true;
                    break;
                case 4:
                    source = "function Probe(take Unique[Device] owner) { " +
                             alias_chain("owner.Get()", depth, "ref") + "Consume(owner.Take()); PrintLine " + alias + "; }";
                    expected_invalid = true;
                    break;
                case 5:
                    source = "function Probe() { " + owner_expression("Unique[Device]", "owner") +
                             aliases + "owner.Reset(); owner = Unique[Device].Create(); fresh = owner.Get(); PrintLine fresh; }";
                    expected_invalid = false;
                    break;
                case 6:
                    source = "function Probe() { " + owner_expression("Unique[Device]", "owner") +
                             owner_expression("Unique[Device]", "other") + aliases +
                             "other.Reset(); PrintLine " + alias + "; }";
                    expected_invalid = false;
                    break;
                case 7:
                    source = "function Probe() { " + owner_expression("Unique[Device]", "owner") +
                             aliases + "owner.Take(); " + alias + " = null; PrintLine " + alias + "; }";
                    expected_invalid = false;
                    break;
                case 8:
                    source = "function Probe() { " + owner_expression("Unique[Device]", "owner") +
                             aliases + "owner.Release(); owner = Unique[Device].Create(); " +
                             alias + " = owner.Get(); PrintLine " + alias + "; }";
                    expected_invalid = false;
                    break;
                default:
                    source = "function Probe() { " + owner_expression("Unique[Device]", "owner") +
                             owner_expression("Unique[Device]", "other") + aliases +
                             "owner.Reset(); other.Take(); fresh = other.Get(); PrintLine fresh; }";
                    expected_invalid = false;
                    break;
                }
                const std::string test_name = "T" + numbered_name(index++) +
                    "_generation_matrix_s" + std::to_string(scenario) +
                    "_depth" + std::to_string(depth);
                matrix_case(test_name, [source, expected_invalid] {
                    if (expected_invalid) expect_invalid(source, "XLIFE0005");
                    else expect_valid(source);
                }, passed, failed);
            }
        }
    }
}
