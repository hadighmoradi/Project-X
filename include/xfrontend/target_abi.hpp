#pragma once

#include "xfrontend/drop_abi.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace xfrontend {

enum class TargetAbiKind { SysVAMD64, Win64, AAPCS64 };
enum class TargetArchitecture { X86_64, AArch64 };
enum class TargetOperatingSystem { Windows, Linux, MacOS };
enum class AbiTypeKind { Integer, Floating, Pointer, Aggregate };
enum class AbiEightbyteClass { Integer, Floating, Memory };
enum class TargetLocationKind { GeneralPurposeRegister, FloatingPointRegister, Stack };

struct TargetAbiDescriptor {
    TargetAbiKind kind = TargetAbiKind::SysVAMD64;
    TargetArchitecture architecture = TargetArchitecture::X86_64;
    TargetOperatingSystem operating_system = TargetOperatingSystem::Linux;
    std::string target_triple = "x86_64-unknown-linux-gnu";
    std::size_t pointer_size = 8U;
    std::size_t stack_alignment = 16U;
    std::size_t general_purpose_argument_registers = 6U;
    std::size_t floating_point_argument_registers = 8U;
    std::size_t shadow_space_size = 0U;
    std::size_t max_direct_aggregate_size = 16U;
};

struct AbiTypeLayout {
    std::string type_name;
    AbiTypeKind kind = AbiTypeKind::Integer;
    std::size_t size = 8U;
    std::size_t alignment = 8U;
    std::vector<AbiEightbyteClass> eightbyte_classes;
    bool contains_unaligned_fields = false;
    bool homogeneous_float_aggregate = false;
    std::size_t homogeneous_member_count = 0U;
};

struct TargetAbiArgumentInput {
    std::string type_name;
    DropParameterPassing passing = DropParameterPassing::Value;
    DropAbiArgumentRole role = DropAbiArgumentRole::ExplicitParameter;
    bool hidden_sret = false;
};

struct TargetAbiCallRequest {
    std::string symbol_id;
    std::string linkage_name;
    DropCallingConvention calling_convention = DropCallingConvention::XNative;
    std::vector<TargetAbiArgumentInput> arguments;
    bool variadic = false;
};

struct TargetAbiLocation {
    TargetLocationKind kind = TargetLocationKind::GeneralPurposeRegister;
    std::size_t register_index = 0U;
    std::size_t stack_offset = 0U;
    std::size_t size = 0U;
    std::size_t alignment = 1U;
};

struct TargetAbiArgumentPlacement {
    std::size_t source_argument_index = 0U;
    std::string source_type;
    DropParameterPassing passing = DropParameterPassing::Value;
    AbiTypeKind physical_type_kind = AbiTypeKind::Integer;
    bool passed_indirectly = false;
    std::vector<TargetAbiLocation> locations;
};

struct TargetAbiCallPlan {
    std::string symbol_id;
    std::string linkage_name;
    std::string target_triple;
    TargetAbiKind abi_kind = TargetAbiKind::SysVAMD64;
    DropCallingConvention source_calling_convention = DropCallingConvention::XNative;
    DropInvocationKind invocation_kind = DropInvocationKind::NoOp;
    OwnershipActionGuard guard = OwnershipActionGuard::Never;
    DropFailureMode failure_mode = DropFailureMode::NoThrow;
    bool terminate_if_failed = false;
    bool consumes_resource = false;
    bool is_noop = false;
    std::string owner_type;
    std::string resource_type;
    std::string routine_name;
    std::size_t action_index = 0U;
    FlowVariableId variable_id = InvalidFlowVariableId;
    SourceSpan span;
    std::size_t shadow_space_size = 0U;
    std::size_t stack_argument_size = 0U;
    std::vector<TargetAbiArgumentPlacement> arguments;
};

struct TargetAbiBlockPlan {
    FlowBlockId id = InvalidFlowBlockId;
    bool reachable = false;
    std::vector<TargetAbiCallPlan> calls;
};

struct TargetAbiFunctionPlan {
    std::string function_name;
    std::vector<TargetAbiBlockPlan> blocks;
};

struct TargetAbiLoweringResult {
    std::vector<TargetAbiFunctionPlan> functions;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty(); }
};

TargetAbiDescriptor make_sysv_amd64_target();
TargetAbiDescriptor make_win64_target();
TargetAbiDescriptor make_aapcs64_target();
bool validate_target_abi_descriptor(const TargetAbiDescriptor& descriptor,
                                   std::string* reason = nullptr);

// Classifies a general function-call argument list, including hidden sret pointers.
// This is a physical argument-placement model, not a machine-code emitter.
TargetAbiCallPlan classify_target_abi_call(
    const TargetAbiDescriptor& descriptor,
    const std::vector<AbiTypeLayout>& layouts,
    const TargetAbiCallRequest& request,
    std::vector<Diagnostic>* diagnostics = nullptr);

// Converts N19's target-independent Drop call bindings into concrete target argument
// locations. ResourceAddress/Borrow arguments become pointers; Take/Value arguments
// use their payload layout. No call is emitted and no user Destructor is executed.
TargetAbiLoweringResult lower_drop_bindings_to_target_abi(
    const DropAbiBindingResult& bindings,
    const TargetAbiDescriptor& descriptor,
    const std::vector<AbiTypeLayout>& layouts);

} // namespace xfrontend
