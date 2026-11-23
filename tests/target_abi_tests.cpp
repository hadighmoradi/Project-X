#include "xfrontend/target_abi.hpp"
#include "test_report.hpp"

#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace xfrontend;

void target_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void target_case(const std::string& name, const std::function<void()>& body,
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

TargetAbiDescriptor profile(int index) {
    switch (index % 3) {
    case 0: return make_sysv_amd64_target();
    case 1: return make_win64_target();
    default: return make_aapcs64_target();
    }
}

std::vector<AbiTypeLayout> layouts_for(const TargetAbiDescriptor& target) {
    std::vector<AbiTypeLayout> values;
    values.push_back({"U8", AbiTypeKind::Integer, 1U, 1U, {}, false, false, 0U});
    values.push_back({"U16", AbiTypeKind::Integer, 2U, 2U, {}, false, false, 0U});
    values.push_back({"U32", AbiTypeKind::Integer, 4U, 4U, {}, false, false, 0U});
    values.push_back({"U64", AbiTypeKind::Integer, 8U, 8U, {}, false, false, 0U});
    values.push_back({"F32", AbiTypeKind::Floating, 4U, 4U, {}, false, false, 0U});
    values.push_back({"F64", AbiTypeKind::Floating, 8U, 8U, {}, false, false, 0U});
    values.push_back({"Ptr", AbiTypeKind::Pointer, target.pointer_size, target.pointer_size, {}, false, false, 0U});
    values.push_back({"PairI", AbiTypeKind::Aggregate, 16U, 8U,
                      {AbiEightbyteClass::Integer, AbiEightbyteClass::Integer}, false, false, 0U});
    values.push_back({"PairMix", AbiTypeKind::Aggregate, 16U, 8U,
                      {AbiEightbyteClass::Integer, AbiEightbyteClass::Floating}, false, false, 0U});
    values.push_back({"Large24", AbiTypeKind::Aggregate, 24U, 8U, {}, false, false, 0U});
    values.push_back({"Hfa3", AbiTypeKind::Aggregate, 12U, 4U,
                      {AbiEightbyteClass::Floating, AbiEightbyteClass::Floating}, false, true, 3U});
    values.push_back({"Unaligned16", AbiTypeKind::Aggregate, 16U, 1U, {}, true, false, 0U});
    for (int i = 0; i < 10; ++i) {
        values.push_back({"Payload" + std::to_string(i), AbiTypeKind::Aggregate,
                          static_cast<std::size_t>(8 + (i % 3) * 8), 8U,
                          i % 2 == 0 ? std::vector<AbiEightbyteClass>{AbiEightbyteClass::Integer,
                              AbiEightbyteClass::Integer} : std::vector<AbiEightbyteClass>{},
                          false, false, 0U});
    }
    return values;
}

std::string layout_name(int index) {
    static const std::vector<std::string> names{
        "U8", "U16", "U32", "U64", "F32", "F64", "Ptr",
        "PairI", "PairMix", "Large24", "Hfa3", "Unaligned16"
    };
    return names[static_cast<std::size_t>(index % static_cast<int>(names.size()))];
}

bool has_location(const TargetAbiArgumentPlacement& arg, TargetLocationKind kind) {
    for (const auto& location : arg.locations) if (location.kind == kind) return true;
    return false;
}

} // namespace

void run_target_abi_tests(int& passed, int& failed) {
    // N20A: target descriptor profiles and validation (500 distinct cases).
    for (int i = 0; i < 500; ++i) {
        target_case("N20A" + std::to_string(i + 1) + "_target_profile_case" + std::to_string(i), [=] {
            auto target = profile(i);
            target.target_triple += "-scenario" + std::to_string(i);
            std::string reason;
            target_require(validate_target_abi_descriptor(target, &reason), "valid built-in target profile rejected: " + reason);
            if (target.kind == TargetAbiKind::SysVAMD64) {
                target_require(target.general_purpose_argument_registers == 6U, "SysV integer register budget mismatch");
                target_require(target.floating_point_argument_registers == 8U, "SysV floating register budget mismatch");
                target_require(target.shadow_space_size == 0U, "SysV must not reserve Win64 shadow space");
            } else if (target.kind == TargetAbiKind::Win64) {
                target_require(target.general_purpose_argument_registers == 4U, "Win64 integer register budget mismatch");
                target_require(target.shadow_space_size == 32U, "Win64 shadow space must be 32 bytes");
            } else {
                target_require(target.architecture == TargetArchitecture::AArch64, "AAPCS64 architecture mismatch");
                target_require(target.general_purpose_argument_registers == 8U, "AAPCS64 GP budget mismatch");
            }
            if (i % 7 == 0) {
                auto invalid = target;
                invalid.stack_alignment = 3U;
                target_require(!validate_target_abi_descriptor(invalid), "non-power-of-two stack alignment must be rejected");
            }
            if (i % 11 == 0) {
                auto invalid = target;
                invalid.pointer_size = 16U;
                target_require(!validate_target_abi_descriptor(invalid), "unsupported pointer size must be rejected");
            }
            if (i % 13 == 0) {
                auto invalid = target;
                invalid.target_triple = target.architecture == TargetArchitecture::X86_64
                    ? "aarch64-unknown-linux-gnu" : "x86_64-unknown-linux-gnu";
                target_require(!validate_target_abi_descriptor(invalid), "target triple architecture mismatch must be rejected");
            }
            if (i % 17 == 0) {
                auto invalid = target;
                invalid.max_direct_aggregate_size = 32U;
                target_require(!validate_target_abi_descriptor(invalid), "unsupported aggregate direct limit must be rejected");
            }
            if (i % 19 == 0 && target.kind != TargetAbiKind::Win64) {
                auto invalid = target;
                invalid.operating_system = TargetOperatingSystem::MacOS;
                target_require(!validate_target_abi_descriptor(invalid), "OS and target triple mismatch must be rejected");
            }
        }, passed, failed);
    }

    // N20B: scalar/aggregate classification differences among SysV, Win64 and AAPCS64.
    for (int i = 0; i < 500; ++i) {
        target_case("N20B" + std::to_string(i + 1) + "_argument_classification_case" + std::to_string(i), [=] {
            const auto target = profile(i);
            const auto layouts = layouts_for(target);
            const std::string type = layout_name(i + (i / 3));
            TargetAbiCallRequest request;
            request.symbol_id = "n20.symbol." + std::to_string(i);
            request.linkage_name = "n20_link_" + std::to_string(i);
            request.calling_convention = static_cast<DropCallingConvention>(i % 3);
            request.arguments.push_back(TargetAbiArgumentInput{type, DropParameterPassing::Value,
                DropAbiArgumentRole::ExplicitParameter, false});
            std::vector<Diagnostic> diagnostics;
            if (i % 25 == 0) {
                request.arguments.front().type_name = "UnregisteredType";
                (void)classify_target_abi_call(target, layouts, request, &diagnostics);
                target_require(!diagnostics.empty() && diagnostics.front().code == "XTABI0004",
                               "unknown type layout must produce XTABI0004");
                return;
            }
            if (i % 29 == 0) {
                auto malformed = layouts;
                for (auto& entry : malformed) if (entry.type_name == "U64") entry.alignment = 3U;
                request.arguments.front().type_name = "U64";
                (void)classify_target_abi_call(target, malformed, request, &diagnostics);
                target_require(!diagnostics.empty() && diagnostics.front().code == "XTABI0005",
                               "invalid type alignment must produce XTABI0005");
                return;
            }
            if (i % 33 == 0 && target.kind == TargetAbiKind::SysVAMD64) {
                auto malformed = layouts;
                for (auto& entry : malformed) if (entry.type_name == "PairI")
                    entry.eightbyte_classes = {AbiEightbyteClass::Integer};
                request.arguments.front().type_name = "PairI";
                (void)classify_target_abi_call(target, malformed, request, &diagnostics);
                target_require(!diagnostics.empty() && diagnostics.front().code == "XTABI0006",
                               "malformed SysV aggregate classes must be rejected");
                return;
            }
            if (i % 40 == 0) request.arguments.front().hidden_sret = true;
            const auto plan = classify_target_abi_call(target, layouts, request, &diagnostics);
            target_require(diagnostics.empty(), "valid argument layout produced a diagnostic");
            target_require(plan.arguments.size() == 1U, "one source argument must produce one placement");
            target_require(!plan.arguments.front().locations.empty(), "argument has no physical location");
            if (i % 40 != 0 && (type == "F32" || type == "F64")) {
                target_require(has_location(plan.arguments.front(), TargetLocationKind::FloatingPointRegister) ||
                               has_location(plan.arguments.front(), TargetLocationKind::Stack),
                               "floating argument must use FP register or stack");
            }
            if (i % 40 != 0 && type == "Large24" && target.kind == TargetAbiKind::Win64)
                target_require(plan.arguments.front().passed_indirectly, "Win64 large aggregate must be passed indirectly");
            if (i % 40 != 0 && type == "Hfa3" && target.kind == TargetAbiKind::AAPCS64)
                target_require(plan.arguments.front().locations.size() == 3U,
                               "AAPCS64 HFA must be split across three FP locations");
            if (i % 40 != 0 && type == "PairMix" && target.kind == TargetAbiKind::SysVAMD64)
                target_require(plan.arguments.front().locations.size() == 2U,
                               "SysV mixed aggregate must have two eightbyte placements");
            if (i % 40 != 0 && type == "Hfa3" && target.kind == TargetAbiKind::SysVAMD64)
                target_require(plan.arguments.front().locations.size() == 2U &&
                               has_location(plan.arguments.front(), TargetLocationKind::FloatingPointRegister),
                               "SysV HFA-like aggregate must use SSE eightbyte classes");
            if (i % 40 == 0)
                target_require(plan.arguments.front().passed_indirectly &&
                               plan.arguments.front().physical_type_kind == AbiTypeKind::Pointer,
                               "hidden sret must be represented as an indirect pointer");
            target_require(plan.target_triple == target.target_triple, "target identity must be retained");
        }, passed, failed);
    }

    // N20C: register exhaustion, stack offsets, alignment, and Win64 positional slots.
    for (int i = 0; i < 500; ++i) {
        target_case("N20C" + std::to_string(i + 1) + "_register_stack_layout_case" + std::to_string(i), [=] {
            const auto target = profile(i);
            const auto layouts = layouts_for(target);
            TargetAbiCallRequest request;
            request.symbol_id = "n20.multi." + std::to_string(i);
            request.linkage_name = "n20_multi_" + std::to_string(i);
            const int argument_count = 17 + (i % 4);
            if (i % 50 == 0) {
                request.variadic = true;
                request.arguments.push_back(TargetAbiArgumentInput{"U64", DropParameterPassing::Value,
                    DropAbiArgumentRole::ExplicitParameter, false});
                std::vector<Diagnostic> diagnostics;
                (void)classify_target_abi_call(target, layouts, request, &diagnostics);
                target_require(!diagnostics.empty() && diagnostics.front().code == "XTABI0003",
                               "variadic request must be rejected explicitly");
                return;
            }
            for (int arg = 0; arg < argument_count; ++arg) {
                const int shape = (i + arg * 5) % 6;
                const std::string type = shape == 0 ? "F64" : shape == 1 ? "U64" :
                    shape == 2 ? "PairI" : shape == 3 ? "Large24" : shape == 4 ? "F32" : "U32";
                request.arguments.push_back(TargetAbiArgumentInput{type, DropParameterPassing::Value,
                    DropAbiArgumentRole::ExplicitParameter, false});
            }
            std::vector<Diagnostic> diagnostics;
            const auto plan = classify_target_abi_call(target, layouts, request, &diagnostics);
            target_require(diagnostics.empty(), "multi-argument call layout should succeed");
            target_require(plan.arguments.size() == request.arguments.size(), "argument count was not preserved");
            target_require(plan.stack_argument_size % target.stack_alignment == 0U,
                           "final stack argument area must respect target stack alignment");
            if (target.kind == TargetAbiKind::Win64)
                target_require(plan.shadow_space_size == 32U, "Win64 call plan must reserve shadow space");
            bool has_stack = false;
            for (std::size_t n = 0; n < plan.arguments.size(); ++n) {
                target_require(!plan.arguments[n].locations.empty(), "every argument must receive a location");
                has_stack = has_stack || has_location(plan.arguments[n], TargetLocationKind::Stack);
            }
            if (argument_count > 8)
                target_require(has_stack, "high-arity call should exercise stack placement");
        }, passed, failed);
    }

    // N20D: lowering N19 Drop bindings into target-specific physical argument plans.
    for (int i = 0; i < 500; ++i) {
        target_case("N20D" + std::to_string(i + 1) + "_drop_target_lowering_case" + std::to_string(i), [=] {
            const auto target = profile(i);
            const auto layouts = layouts_for(target);
            DropAbiBindingResult bindings;
            DropAbiBindingFunction function;
            function.function_name = "N20DropProbe";
            DropAbiBindingBlock reachable;
            reachable.id = static_cast<FlowBlockId>(1);
            reachable.reachable = true;
            DropCallBinding binding;
            binding.function_name = function.function_name;
            binding.block_id = reachable.id;
            binding.action_index = static_cast<std::size_t>(i);
            binding.variable_id = static_cast<FlowVariableId>(i + 1);
            binding.guard = i % 2 == 0 ? OwnershipActionGuard::Always : OwnershipActionGuard::IfLive;
            binding.failure_mode = i % 5 == 0 ? DropFailureMode::MayFail : DropFailureMode::NoThrow;
            binding.terminate_if_failed = binding.failure_mode == DropFailureMode::MayFail;
            binding.invocation_kind = (i % 9 == 0) ? DropInvocationKind::NoOp :
                ((i % 2 == 0) ? DropInvocationKind::DestructorCall : DropInvocationKind::CustomDeleterCall);
            binding.resource_type = (i % 12 == 9) ? "Large24" : layout_name(i);
            binding.owner_type = "Unique[" + binding.resource_type + "]";
            binding.symbol_id = "n20.drop.symbol." + std::to_string(i);
            binding.linkage_name = "n20_drop_link_" + std::to_string(i);
            binding.calling_convention = static_cast<DropCallingConvention>(i % 3);
            binding.consumes_resource = binding.invocation_kind == DropInvocationKind::CustomDeleterCall;
            if (binding.invocation_kind == DropInvocationKind::DestructorCall) {
                binding.arguments.push_back(DropAbiArgument{DropAbiArgumentRole::Receiver,
                    DropAbiOperandKind::ResourceAddress, 0U, binding.resource_type,
                    DropParameterPassing::BorrowMut});
            } else if (binding.invocation_kind == DropInvocationKind::CustomDeleterCall) {
                binding.arguments.push_back(DropAbiArgument{DropAbiArgumentRole::ExplicitParameter,
                    DropAbiOperandKind::OwnedResourceValue, 0U, binding.resource_type,
                    DropParameterPassing::Take});
            }
            reachable.bindings.push_back(binding);
            DropAbiBindingBlock unreachable;
            unreachable.id = static_cast<FlowBlockId>(2);
            unreachable.reachable = false;
            DropCallBinding ignored = binding;
            ignored.symbol_id = "unreachable_symbol";
            unreachable.bindings.push_back(ignored);
            function.blocks.push_back(reachable);
            function.blocks.push_back(unreachable);
            bindings.functions.push_back(function);

            const auto result = lower_drop_bindings_to_target_abi(bindings, target, layouts);
            target_require(result.succeeded(), "valid N19 Drop binding should lower to target ABI");
            target_require(result.functions.size() == 1U, "function count mismatch");
            target_require(result.functions.front().blocks.size() == 2U, "block count mismatch");
            const auto& lowered_block = result.functions.front().blocks.front();
            target_require(lowered_block.calls.size() == 1U, "reachable binding must produce one call plan");
            const auto& call = lowered_block.calls.front();
            target_require(call.target_triple == target.target_triple, "target triple must be copied to call plan");
            target_require(call.guard == binding.guard, "runtime guard must be preserved");
            target_require(call.failure_mode == binding.failure_mode, "failure mode must be preserved");
            target_require(call.terminate_if_failed == binding.terminate_if_failed, "terminate policy must be preserved");
            target_require(call.consumes_resource == binding.consumes_resource, "resource-consumption contract mismatch");
            target_require(call.owner_type == binding.owner_type && call.resource_type == binding.resource_type,
                           "owner and resource type metadata must be preserved");
            target_require(call.action_index == binding.action_index && call.variable_id == binding.variable_id,
                           "CFG action and owner variable identity must be preserved");
            target_require(result.functions.front().blocks.back().calls.empty(), "unreachable block must not emit a call plan");
            if (binding.invocation_kind == DropInvocationKind::NoOp) {
                target_require(call.is_noop && call.arguments.empty(), "NoOp binding must not create ABI arguments");
            } else {
                target_require(!call.arguments.empty(), "real Drop invocation must have a physical argument");
                if (binding.invocation_kind == DropInvocationKind::DestructorCall) {
                    target_require(call.arguments.front().passed_indirectly,
                                   "Destructor borrow receiver must lower as a pointer");
                    target_require(call.arguments.front().physical_type_kind == AbiTypeKind::Pointer,
                                   "Destructor receiver physical type must be a pointer");
                } else {
                    target_require(call.arguments.front().passing == DropParameterPassing::Take,
                                   "CustomDeleter must preserve Take passing mode");
                }
            }
        }, passed, failed);
    }
}
