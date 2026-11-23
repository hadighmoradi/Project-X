#include "xfrontend/drop_abi.hpp"
#include "xfrontend/parser.hpp"
#include "xfrontend/control_flow.hpp"
#include "xfrontend/ownership_lowering.hpp"
#include "xfrontend/drop_contract.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace xfrontend;

void abi_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string fold(std::string value) {
    for (char& ch : value) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte >= 'A' && byte <= 'Z') ch = static_cast<char>(byte - 'A' + 'a');
    }
    return value;
}

std::string abi_type(int index) {
    static const std::vector<std::string> values{
        "Device", "Widget", "Buffer", "Node", "Resource",
        "Socket", "Handle", "Stream", "Window", "Context"
    };
    return values[static_cast<std::size_t>(index % 10)];
}

std::string abi_owner(int index) {
    static const std::vector<std::string> values{
        "owner", "Owner", "OWNER", "resource", "Resource",
        "handle", "Handle", "unit", "Unit", "payload"
    };
    return values[static_cast<std::size_t>(index % 10)];
}

void abi_case(const std::string& name, const std::function<void()>& body,
              int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    const auto start = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

struct AbiFixture {
    OwnershipLoweringResult lowered;
    DropContractResolutionResult resolved;
};

AbiFixture abi_fixture(const std::string& type, const std::string& owner,
                       DropRoutineKind kind, const std::string& routine_name) {
    const std::string source = "function Probe(bool flag) { Unique[" + type + "] " + owner +
        " = Unique[" + type + "].Create(); PrintLine " + owner + ".Get(); }";
    const auto parsed = parse_source(source);
    if (!parsed.succeeded())
        throw std::runtime_error("N19 parser failure: " + parsed.diagnostics.front().code + " | " + source);
    const auto flow = analyze_control_flow(parsed.unit);
    if (!flow.succeeded())
        throw std::runtime_error("N19 CFG failure: " + flow.diagnostics.front().code + " | " + source);
    AbiFixture fixture;
    fixture.lowered = lower_ownership_cleanup(flow);
    if (!fixture.lowered.succeeded())
        throw std::runtime_error("N19 lowering failure: " + fixture.lowered.diagnostics.front().code);
    DropRoutineContract contract;
    contract.resource_type = type;
    contract.kind = kind;
    contract.routine_name = kind == DropRoutineKind::Trivial ? std::string{} : routine_name;
    fixture.resolved = resolve_ownership_drop_contracts(fixture.lowered, {contract});
    if (!fixture.resolved.succeeded())
        throw std::runtime_error("N19 contract resolution failure: " + fixture.resolved.diagnostics.front().code);
    return fixture;
}

const DropInvocation& first_invocation(const DropContractResolutionResult& result) {
    for (const auto& function : result.functions)
        for (const auto& block : function.blocks)
            if (!block.invocations.empty()) return block.invocations.front();
    throw std::runtime_error("N19 fixture did not produce a drop invocation");
}

DropCallableSymbol make_symbol(const DropInvocation& invocation, DropCallableKind kind,
                               const std::string& module = "Resources") {
    DropCallableSymbol symbol;
    symbol.symbol_id = "sym:" + fold(module) + ":" + fold(invocation.routine_name) + ":" + fold(invocation.resource_type);
    symbol.module_name = module;
    symbol.name = invocation.routine_name;
    const auto dot = symbol.name.rfind('.');
    if (dot != std::string::npos) symbol.name = symbol.name.substr(dot + 1U);
    symbol.qualified_name = module + "." + symbol.name;
    symbol.linkage_name = "_X$" + fold(module) + "$" + fold(symbol.name);
    symbol.resource_type = invocation.resource_type;
    symbol.kind = kind;
    symbol.calling_convention = DropCallingConvention::XNative;
    symbol.visibility = DropSymbolVisibility::Public;
    symbol.linkage = DropSymbolLinkage::Internal;
    symbol.return_type = "void";
    symbol.failure_mode = invocation.failure_mode;
    if (kind == DropCallableKind::DestructorMethod) {
        symbol.receiver_type = invocation.resource_type;
        symbol.receiver_passing = DropParameterPassing::BorrowMut;
    } else {
        symbol.parameters.push_back(DropCallableParameter{invocation.resource_type, DropParameterPassing::Take, false});
    }
    return symbol;
}

bool abi_has_code(const DropAbiBindingResult& result, const std::string& code) {
    return std::any_of(result.diagnostics.begin(), result.diagnostics.end(), [&](const auto& diagnostic) {
        return diagnostic.code == code;
    });
}

int binding_count(const DropAbiBindingResult& result) {
    int count = 0;
    for (const auto& function : result.functions)
        for (const auto& block : function.blocks)
            count += static_cast<int>(block.bindings.size());
    return count;
}

} // namespace

void run_drop_abi_tests(int& passed, int& failed) {
    // N19A (500): canonical symbol identity, module qualification, conventions and linkage.
    int id = 1;
    for (int type_case = 0; type_case < 10; ++type_case) {
        for (int owner_case = 0; owner_case < 10; ++owner_case) {
            for (int mode = 0; mode < 5; ++mode) {
                const std::string type = abi_type(type_case);
                const std::string owner = abi_owner(owner_case);
                const bool custom = mode >= 3;
                const std::string routine = (mode == 1 ? "Resources." : "") +
                    std::string(custom ? "Release_" : "Destroy_") + type;
                const std::string test = "N19A" + std::to_string(id++) + "_symbol_identity_type" +
                    std::to_string(type_case) + "_owner" + std::to_string(owner_case) + "_mode" +
                    std::to_string(mode);
                abi_case(test, [type, owner, mode, custom, routine] {
                    const auto fixture = abi_fixture(type, owner,
                        custom ? DropRoutineKind::CustomDeleter : DropRoutineKind::Destructor, routine);
                    const auto& invocation = first_invocation(fixture.resolved);
                    auto symbol = make_symbol(invocation,
                        custom ? DropCallableKind::FreeFunction : DropCallableKind::DestructorMethod,
                        "Resources");
                    if (mode == 0) symbol.name = routine.substr(routine.find_last_of('.') == std::string::npos
                        ? 0U : routine.find_last_of('.') + 1U);
                    if (mode == 1) symbol.qualified_name = "Resources." + symbol.name;
                    if (mode == 2) symbol.name = fold(symbol.name);
                    if (mode == 3) symbol.calling_convention = DropCallingConvention::C;
                    if (mode == 4) symbol.calling_convention = DropCallingConvention::System;
                    if (mode == 3 || mode == 4) symbol.linkage = DropSymbolLinkage::External;
                    if (mode == 0) symbol.visibility = DropSymbolVisibility::Private;
                    if (mode == 1) symbol.visibility = DropSymbolVisibility::Internal;
                    if (mode == 4) symbol.kind = DropCallableKind::StaticFunction;
                    DropAbiResolutionOptions options;
                    options.caller_module = (mode <= 1) ? "Resources" : "Application";
                    const auto bound = bind_drop_invocations_to_callables(fixture.resolved, {symbol}, options);
                    abi_require(bound.succeeded(), "valid callable symbol must bind successfully");
                    abi_require(binding_count(bound) == 1, "one lowered drop invocation must produce one ABI binding");
                    const auto& binding = bound.functions.front().blocks.front().bindings.front();
                    abi_require(binding.symbol_id == symbol.symbol_id, "stable symbol identity must be retained");
                    if (symbol.linkage == DropSymbolLinkage::External)
                        abi_require(binding.linkage_name == symbol.linkage_name, "external linker name must be retained");
                    if (custom) {
                        abi_require(binding.calling_convention == symbol.calling_convention,
                                    "custom-deleter calling convention must be retained");
                        abi_require(binding.consumes_resource, "custom deleter must consume the resource value");
                        abi_require(binding.arguments.size() == 1U, "custom deleter must bind one explicit argument");
                    } else {
                        abi_require(binding.calling_convention == DropCallingConvention::XNative,
                                    "destructor method must use XNative convention");
                        abi_require(!binding.consumes_resource, "destructor receiver borrows the resource in place");
                    }
                }, passed, failed);
            }
        }
    }

    // N19B (500): symbol visibility, discovery status, receiver contract and destructor ABI.
    id = 1;
    for (int type_case = 0; type_case < 10; ++type_case) {
        for (int owner_case = 0; owner_case < 10; ++owner_case) {
            for (int invalid = 0; invalid < 5; ++invalid) {
                const std::string type = abi_type(type_case);
                const std::string owner = abi_owner(owner_case);
                const std::string routine = "Destroy_" + type;
                const std::string test = "N19B" + std::to_string(id++) + "_destructor_validation_type" +
                    std::to_string(type_case) + "_owner" + std::to_string(owner_case) + "_invalid" +
                    std::to_string(invalid);
                abi_case(test, [type, owner, routine, invalid, type_case] {
                    const auto fixture = abi_fixture(type, owner, DropRoutineKind::Destructor, routine);
                    const auto& invocation = first_invocation(fixture.resolved);
                    auto symbol = make_symbol(invocation, DropCallableKind::DestructorMethod);
                    std::string expected;
                    switch (invalid) {
                    case 0: symbol.resolved = false; expected = "XABI0002"; break;
                    case 1: symbol.visibility = DropSymbolVisibility::Private; expected = "XABI0003"; break;
                    case 2: symbol.accessible = false; expected = "XABI0003"; break;
                    case 3: symbol.receiver_passing = DropParameterPassing::Borrow; expected = "XABI0009"; break;
                    default:
                        if ((invalid + type_case) % 2 == 0) {
                            symbol.calling_convention = DropCallingConvention::C;
                            expected = "XABI0006";
                        } else {
                            symbol.linkage = DropSymbolLinkage::External;
                            symbol.linkage_name.clear();
                            expected = "XABI0007";
                        }
                        break;
                    }
                    DropAbiResolutionOptions options;
                    options.caller_module = "App";
                    const auto bound = bind_drop_invocations_to_callables(fixture.resolved, {symbol}, options);
                    abi_require(!bound.succeeded(), "invalid destructor symbol must be rejected");
                    abi_require(abi_has_code(bound, expected), "expected destructor ABI diagnostic was not reported");
                    abi_require(bound.functions.empty(), "failed ABI binding must not return partial call plans");
                }, passed, failed);
            }
        }
    }

    // N19C (500): custom-deleter parameter mode, payload type, arity, defaults and return type.
    id = 1;
    for (int type_case = 0; type_case < 10; ++type_case) {
        for (int owner_case = 0; owner_case < 10; ++owner_case) {
            for (int invalid = 0; invalid < 5; ++invalid) {
                const std::string type = abi_type(type_case);
                const std::string owner = abi_owner(owner_case);
                const std::string routine = "Release_" + type;
                const std::string test = "N19C" + std::to_string(id++) + "_deleter_signature_type" +
                    std::to_string(type_case) + "_owner" + std::to_string(owner_case) + "_invalid" +
                    std::to_string(invalid);
                abi_case(test, [type, owner, routine, invalid, type_case, owner_case] {
                    const auto fixture = abi_fixture(type, owner, DropRoutineKind::CustomDeleter, routine);
                    const auto& invocation = first_invocation(fixture.resolved);
                    auto symbol = make_symbol(invocation, DropCallableKind::FreeFunction);
                    switch (invalid) {
                    case 0: symbol.parameters[0].passing = DropParameterPassing::Value; break;
                    case 1: symbol.parameters[0].passing = DropParameterPassing::BorrowMut; break;
                    case 2: symbol.parameters[0].type_name = "Other" + type; break;
                    case 3: symbol.parameters.clear(); break;
                    default:
                        if ((type_case + owner_case) % 2 == 0) symbol.parameters[0].has_default = true;
                        else symbol.return_type = "int";
                        break;
                    }
                    const auto bound = bind_drop_invocations_to_callables(fixture.resolved, {symbol});
                    abi_require(!bound.succeeded(), "invalid custom deleter signature must be rejected");
                    abi_require(abi_has_code(bound, "XABI0009"), "invalid deleter signature diagnostic is required");
                    abi_require(bound.functions.empty(), "invalid deleter must not produce partial ABI bindings");
                }, passed, failed);
            }
        }
    }

    // N19D (500): deterministic argument layout, no-op handling, ambiguity and fail-closed output.
    id = 1;
    for (int type_case = 0; type_case < 10; ++type_case) {
        for (int owner_case = 0; owner_case < 10; ++owner_case) {
            for (int layout = 0; layout < 5; ++layout) {
                const std::string type_a = abi_type(type_case);
                const std::string type_b = abi_type((type_case + 3) % 10);
                const std::string owner_a = "left_" + abi_owner(owner_case);
                const std::string owner_b = "right_" + abi_owner((owner_case + 4) % 10);
                const std::string source = "function Probe(bool flag) { Unique[" + type_a + "] " + owner_a +
                    " = Unique[" + type_a + "].Create(); Unique[" + type_b + "] " + owner_b +
                    " = Unique[" + type_b + "].Create(); if (flag) { " + owner_a +
                    ".Reset(); } else { PrintLine " + owner_b + ".Get(); } }";
                const std::string test = "N19D" + std::to_string(id++) + "_abi_layout_type" +
                    std::to_string(type_case) + "_owner" + std::to_string(owner_case) + "_layout" +
                    std::to_string(layout);
                abi_case(test, [source, type_a, type_b, layout, type_case, owner_case] {
                    const auto parsed = parse_source(source);
                    abi_require(parsed.succeeded(), "N19 multi-owner source must parse");
                    const auto flow = analyze_control_flow(parsed.unit);
                    abi_require(flow.succeeded(), "N19 multi-owner source must produce CFG");
                    const auto lowered = lower_ownership_cleanup(flow);
                    abi_require(lowered.succeeded(), "N19 multi-owner source must lower ownership");
                    std::vector<DropRoutineContract> contracts;
                    for (const auto& function : lowered.functions) {
                        for (const auto& variable : function.variables) {
                            if (!variable.is_unique_owner) continue;
                            const std::string payload = variable.type_name.substr(
                                variable.type_name.find('[') + 1U,
                                variable.type_name.size() - variable.type_name.find('[') - 2U);
                            DropRoutineContract contract;
                            contract.resource_type = payload;
                            if (layout == 3) {
                                contract.kind = DropRoutineKind::Trivial;
                            } else {
                                const bool custom = layout == 1 || (layout == 4 && fold(payload) == fold(type_b));
                                contract.kind = custom ? DropRoutineKind::CustomDeleter : DropRoutineKind::Destructor;
                                contract.routine_name = (custom ? "Release_" : "Destroy_") + payload;
                            }
                            contracts.push_back(std::move(contract));
                        }
                    }
                    const auto resolved = resolve_ownership_drop_contracts(lowered, contracts);
                    abi_require(resolved.succeeded(), "N19 drop contracts must resolve before ABI binding");
                    std::vector<DropCallableSymbol> symbols;
                    for (const auto& function : resolved.functions) {
                        for (const auto& block : function.blocks) {
                            for (const auto& invocation : block.invocations) {
                                if (invocation.kind == DropInvocationKind::NoOp) continue;
                                const bool custom = invocation.kind == DropInvocationKind::CustomDeleterCall;
                                auto symbol = make_symbol(invocation, custom ? DropCallableKind::FreeFunction
                                                                             : DropCallableKind::DestructorMethod);
                                const bool already_registered = std::any_of(symbols.begin(), symbols.end(),
                                    [&](const DropCallableSymbol& existing) {
                                        return existing.symbol_id == symbol.symbol_id;
                                    });
                                if (!already_registered) symbols.push_back(std::move(symbol));
                            }
                        }
                    }
                    if (layout == 2 && !symbols.empty()) {
                        if ((type_case + owner_case) % 2 == 0) {
                            auto overload = symbols.front();
                            overload.symbol_id += ":overload";
                            overload.linkage_name += "_overload";
                            symbols.push_back(std::move(overload));
                        } else {
                            symbols.front().failure_mode = DropFailureMode::MayFail;
                        }
                    }
                    const auto bound = bind_drop_invocations_to_callables(resolved, symbols);
                    if (layout == 2) {
                        abi_require(!bound.succeeded(), "invalid callable binding must fail closed");
                        const std::string expected = ((type_case + owner_case) % 2 == 0) ? "XABI0004" : "XABI0005";
                        abi_require(abi_has_code(bound, expected), "ambiguity/failure metadata diagnostic is required");
                        abi_require(bound.functions.empty(), "failed binding must not expose partial plans");
                        return;
                    }
                    abi_require(bound.succeeded(), "valid N19 layouts must bind successfully");
                    int expected = 0;
                    for (const auto& function : resolved.functions)
                        for (const auto& block : function.blocks) expected += static_cast<int>(block.invocations.size());
                    abi_require(binding_count(bound) == expected, "binding count must equal resolved invocation count");
                    for (const auto& function : bound.functions) {
                        for (const auto& block : function.blocks) {
                            std::size_t prior_index = 0;
                            bool first = true;
                            for (const auto& binding : block.bindings) {
                                if (!first) abi_require(binding.action_index > prior_index,
                                    "binding order must preserve action order within each block");
                                prior_index = binding.action_index;
                                first = false;
                                if (binding.invocation_kind == DropInvocationKind::NoOp) {
                                    abi_require(binding.symbol_id.empty() && binding.arguments.empty() &&
                                                !binding.consumes_resource,
                                                "trivial cleanup must create a symbol-free no-op binding");
                                } else if (binding.invocation_kind == DropInvocationKind::DestructorCall) {
                                    abi_require(binding.arguments.size() == 1U &&
                                                binding.arguments[0].role == DropAbiArgumentRole::Receiver &&
                                                binding.arguments[0].operand == DropAbiOperandKind::ResourceAddress &&
                                                binding.arguments[0].passing == DropParameterPassing::BorrowMut &&
                                                !binding.consumes_resource,
                                                "destructor binding must pass a mutable resource receiver");
                                } else {
                                    abi_require(binding.arguments.size() == 1U &&
                                                binding.arguments[0].role == DropAbiArgumentRole::ExplicitParameter &&
                                                binding.arguments[0].operand == DropAbiOperandKind::OwnedResourceValue &&
                                                binding.arguments[0].passing == DropParameterPassing::Take &&
                                                binding.consumes_resource,
                                                "custom deleter binding must transfer the owned resource value");
                                }
                            }
                            if (!block.reachable)
                                abi_require(block.bindings.empty(), "unreachable blocks cannot contain ABI bindings");
                        }
                    }
                }, passed, failed);
            }
        }
    }
}
