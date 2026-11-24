// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/target_abi.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace xfrontend {
namespace {

std::string fold(std::string value) {
    for (char& ch : value) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c >= static_cast<unsigned char>('A') && c <= static_cast<unsigned char>('Z'))
            ch = static_cast<char>(c - static_cast<unsigned char>('A') + static_cast<unsigned char>('a'));
    }
    return value;
}

std::string compact(std::string value) {
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }), value.end());
    return fold(std::move(value));
}

std::size_t round_up(std::size_t value, std::size_t alignment) {
    if (alignment <= 1U) return value;
    const std::size_t remainder = value % alignment;
    return remainder == 0U ? value : value + (alignment - remainder);
}

void diag(std::vector<Diagnostic>* diagnostics, const char* code, const std::string& message) {
    if (diagnostics != nullptr) diagnostics->push_back(Diagnostic{code, message, {}});
}

const AbiTypeLayout* find_layout(const std::vector<AbiTypeLayout>& layouts, const std::string& name) {
    const std::string key = compact(name);
    const AbiTypeLayout* found = nullptr;
    for (const auto& layout : layouts) {
        if (compact(layout.type_name) != key) continue;
        if (found != nullptr) return nullptr;
        found = &layout;
    }
    return found;
}

bool valid_layout(const AbiTypeLayout& layout, const TargetAbiDescriptor& descriptor) {
    if (layout.type_name.empty() || layout.size == 0U || layout.alignment == 0U) return false;
    if ((layout.alignment & (layout.alignment - 1U)) != 0U) return false;
    if (layout.alignment > 4096U || layout.size > (1U << 30U)) return false;
    if (layout.kind == AbiTypeKind::Pointer && layout.size != descriptor.pointer_size) return false;
    if (layout.kind == AbiTypeKind::Integer && layout.size > 8U) return false;
    if (layout.kind == AbiTypeKind::Floating && layout.size != 4U && layout.size != 8U && layout.size != 16U)
        return false;
    if (layout.kind != AbiTypeKind::Aggregate && !layout.eightbyte_classes.empty()) return false;
    if (layout.homogeneous_float_aggregate &&
        (layout.kind != AbiTypeKind::Aggregate || layout.homogeneous_member_count == 0U ||
         layout.homogeneous_member_count > 4U)) return false;
    return true;
}

bool is_reference_passing(DropParameterPassing passing) {
    return passing == DropParameterPassing::Borrow || passing == DropParameterPassing::BorrowMut;
}

void append_location(TargetAbiArgumentPlacement& placement, TargetLocationKind kind,
                     std::size_t index, std::size_t offset, std::size_t size, std::size_t alignment) {
    placement.locations.push_back(TargetAbiLocation{kind, index, offset, size, alignment});
}

struct ClassificationState {
    std::size_t gp_used = 0U;
    std::size_t fp_used = 0U;
    std::size_t stack_cursor = 0U;
    std::size_t win64_ordinal = 0U;
};

void place_stack(TargetAbiArgumentPlacement& placement, ClassificationState& state,
                 std::size_t size, std::size_t alignment, const TargetAbiDescriptor& descriptor) {
    const std::size_t effective_alignment = std::min<std::size_t>(
        std::max<std::size_t>(alignment, descriptor.kind == TargetAbiKind::AAPCS64 ? 8U : 8U),
        descriptor.stack_alignment);
    state.stack_cursor = round_up(state.stack_cursor, effective_alignment);
    append_location(placement, TargetLocationKind::Stack, 0U, state.stack_cursor,
                    size, effective_alignment);
    const std::size_t slot = descriptor.kind == TargetAbiKind::Win64
        ? 8U : round_up(size, 8U);
    state.stack_cursor += std::max<std::size_t>(slot, 8U);
}

bool resolve_calling_convention(const TargetAbiDescriptor& descriptor,
                                DropCallingConvention convention) {
    // On the currently modeled 64-bit targets, XNative uses the selected target
    // profile; C and System use the platform C/system ABI, whose argument placement
    // is the same profile for these three targets. Preserve the source label in plan.
    (void)descriptor;
    return convention == DropCallingConvention::XNative ||
           convention == DropCallingConvention::C || convention == DropCallingConvention::System;
}

TargetAbiArgumentPlacement classify_one(const TargetAbiDescriptor& descriptor,
                                        const AbiTypeLayout& layout,
                                        const TargetAbiArgumentInput& input,
                                        std::size_t source_index,
                                        ClassificationState& state) {
    TargetAbiArgumentPlacement result;
    result.source_argument_index = source_index;
    result.source_type = input.type_name;
    result.passing = input.passing;
    result.physical_type_kind = layout.kind;

    const bool by_reference = is_reference_passing(input.passing) || input.hidden_sret;
    result.passed_indirectly = by_reference;
    if (by_reference) {
        result.physical_type_kind = AbiTypeKind::Pointer;
        if (descriptor.kind == TargetAbiKind::Win64) {
            const std::size_t ordinal = state.win64_ordinal++;
            if (ordinal < 4U) {
                append_location(result, TargetLocationKind::GeneralPurposeRegister,
                                ordinal, 0U, descriptor.pointer_size, descriptor.pointer_size);
            } else {
                place_stack(result, state, 8U, 8U, descriptor);
            }
        } else if (state.gp_used < descriptor.general_purpose_argument_registers) {
            append_location(result, TargetLocationKind::GeneralPurposeRegister,
                            state.gp_used++, 0U, descriptor.pointer_size, descriptor.pointer_size);
        } else {
            place_stack(result, state, descriptor.pointer_size, descriptor.pointer_size, descriptor);
        }
        return result;
    }

    if (layout.kind == AbiTypeKind::Integer || layout.kind == AbiTypeKind::Pointer) {
        const std::size_t chunks = std::max<std::size_t>(1U, (layout.size + 7U) / 8U);
        if (descriptor.kind == TargetAbiKind::Win64) {
            const std::size_t ordinal = state.win64_ordinal++;
            if (ordinal < 4U) {
                append_location(result, TargetLocationKind::GeneralPurposeRegister,
                                ordinal, 0U, layout.size, layout.alignment);
            } else {
                place_stack(result, state, layout.size, layout.alignment, descriptor);
            }
        } else if (chunks == 1U && state.gp_used < descriptor.general_purpose_argument_registers) {
            append_location(result, TargetLocationKind::GeneralPurposeRegister,
                            state.gp_used++, 0U, layout.size, layout.alignment);
        } else {
            place_stack(result, state, layout.size, layout.alignment, descriptor);
        }
        return result;
    }

    if (layout.kind == AbiTypeKind::Floating) {
        if (descriptor.kind == TargetAbiKind::Win64) {
            const std::size_t ordinal = state.win64_ordinal++;
            if (ordinal < 4U) {
                append_location(result, TargetLocationKind::FloatingPointRegister,
                                ordinal, 0U, layout.size, layout.alignment);
            } else {
                place_stack(result, state, layout.size, layout.alignment, descriptor);
            }
        } else if (state.fp_used < descriptor.floating_point_argument_registers) {
            append_location(result, TargetLocationKind::FloatingPointRegister,
                            state.fp_used++, 0U, layout.size, layout.alignment);
        } else {
            place_stack(result, state, layout.size, layout.alignment, descriptor);
        }
        return result;
    }

    // Aggregates are handled according to the selected platform ABI.
    if (descriptor.kind == TargetAbiKind::Win64) {
        const bool direct = layout.size == 1U || layout.size == 2U ||
                            layout.size == 4U || layout.size == 8U;
        const std::size_t ordinal = state.win64_ordinal++;
        result.passed_indirectly = !direct;
        result.physical_type_kind = direct ? AbiTypeKind::Integer : AbiTypeKind::Pointer;
        if (ordinal < 4U) {
            append_location(result, TargetLocationKind::GeneralPurposeRegister, ordinal,
                            0U, direct ? layout.size : descriptor.pointer_size,
                            direct ? layout.alignment : descriptor.pointer_size);
        } else {
            place_stack(result, state, direct ? layout.size : 8U,
                        direct ? layout.alignment : 8U, descriptor);
        }
        return result;
    }

    if (descriptor.kind == TargetAbiKind::AAPCS64 && layout.homogeneous_float_aggregate &&
        layout.homogeneous_member_count <= 4U && !layout.contains_unaligned_fields) {
        if (state.fp_used + layout.homogeneous_member_count <= descriptor.floating_point_argument_registers) {
            for (std::size_t i = 0; i < layout.homogeneous_member_count; ++i)
                append_location(result, TargetLocationKind::FloatingPointRegister,
                                state.fp_used++, 0U, layout.size / layout.homogeneous_member_count,
                                layout.alignment);
        } else {
            place_stack(result, state, layout.size, layout.alignment, descriptor);
        }
        return result;
    }

    const bool indirect = layout.size > descriptor.max_direct_aggregate_size ||
                          layout.contains_unaligned_fields;
    if (indirect) {
        result.passed_indirectly = true;
        result.physical_type_kind = AbiTypeKind::Pointer;
        if (state.gp_used < descriptor.general_purpose_argument_registers) {
            append_location(result, TargetLocationKind::GeneralPurposeRegister,
                            state.gp_used++, 0U, descriptor.pointer_size, descriptor.pointer_size);
        } else {
            place_stack(result, state, descriptor.pointer_size, descriptor.pointer_size, descriptor);
        }
        return result;
    }

    std::vector<AbiEightbyteClass> classes = layout.eightbyte_classes;
    if (descriptor.kind == TargetAbiKind::SysVAMD64) {
        const std::size_t chunks = (layout.size + 7U) / 8U;
        if (layout.size > 16U || chunks > 2U || layout.contains_unaligned_fields) {
            place_stack(result, state, layout.size, layout.alignment, descriptor);
            return result;
        }
        if (classes.empty()) classes.assign(chunks, AbiEightbyteClass::Integer);
        if (classes.size() != chunks || std::find(classes.begin(), classes.end(), AbiEightbyteClass::Memory) != classes.end()) {
            place_stack(result, state, layout.size, layout.alignment, descriptor);
            return result;
        }
        const std::size_t need_gp = static_cast<std::size_t>(std::count(classes.begin(), classes.end(), AbiEightbyteClass::Integer));
        const std::size_t need_fp = classes.size() - need_gp;
        if (state.gp_used + need_gp > descriptor.general_purpose_argument_registers ||
            state.fp_used + need_fp > descriptor.floating_point_argument_registers) {
            place_stack(result, state, layout.size, layout.alignment, descriptor);
            return result;
        }
        for (const auto cls : classes) {
            if (cls == AbiEightbyteClass::Integer)
                append_location(result, TargetLocationKind::GeneralPurposeRegister, state.gp_used++, 0U, 8U, 8U);
            else
                append_location(result, TargetLocationKind::FloatingPointRegister, state.fp_used++, 0U, 8U, 8U);
        }
        return result;
    }

    // AAPCS64: non-HFA aggregates up to 16 bytes are split into one or two GP registers.
    const std::size_t chunks = (layout.size + 7U) / 8U;
    if (state.gp_used + chunks <= descriptor.general_purpose_argument_registers) {
        for (std::size_t i = 0; i < chunks; ++i) {
            const std::size_t part_size = std::min<std::size_t>(8U, layout.size - i * 8U);
            append_location(result, TargetLocationKind::GeneralPurposeRegister,
                            state.gp_used++, 0U, part_size, std::min<std::size_t>(8U, layout.alignment));
        }
    } else {
        place_stack(result, state, layout.size, layout.alignment, descriptor);
    }
    return result;
}

} // namespace

TargetAbiDescriptor make_sysv_amd64_target() {
    return TargetAbiDescriptor{TargetAbiKind::SysVAMD64, TargetArchitecture::X86_64,
        TargetOperatingSystem::Linux, "x86_64-unknown-linux-gnu", 8U, 16U, 6U, 8U, 0U, 16U};
}

TargetAbiDescriptor make_win64_target() {
    return TargetAbiDescriptor{TargetAbiKind::Win64, TargetArchitecture::X86_64,
        TargetOperatingSystem::Windows, "x86_64-pc-windows-msvc", 8U, 16U, 4U, 4U, 32U, 8U};
}

TargetAbiDescriptor make_aapcs64_target() {
    return TargetAbiDescriptor{TargetAbiKind::AAPCS64, TargetArchitecture::AArch64,
        TargetOperatingSystem::Linux, "aarch64-unknown-linux-gnu", 8U, 16U, 8U, 8U, 0U, 16U};
}

bool validate_target_abi_descriptor(const TargetAbiDescriptor& descriptor, std::string* reason) {
    const auto fail = [&](const char* text) {
        if (reason != nullptr) *reason = text;
        return false;
    };
    if (descriptor.target_triple.empty()) return fail("target triple is required");
    const std::string triple = fold(descriptor.target_triple);
    if (descriptor.architecture == TargetArchitecture::X86_64 && triple.find("x86_64-") != 0U)
        return fail("target triple architecture does not match x86_64 descriptor");
    if (descriptor.architecture == TargetArchitecture::AArch64 && triple.find("aarch64-") != 0U)
        return fail("target triple architecture does not match AArch64 descriptor");
    if (descriptor.pointer_size != 4U && descriptor.pointer_size != 8U) return fail("unsupported pointer size");
    if (descriptor.stack_alignment == 0U ||
        (descriptor.stack_alignment & (descriptor.stack_alignment - 1U)) != 0U)
        return fail("stack alignment must be a nonzero power of two");
    if (descriptor.stack_alignment != 16U)
        return fail("the selected 64-bit target profiles require 16-byte stack alignment");
    if (descriptor.stack_alignment < descriptor.pointer_size) return fail("stack alignment is smaller than pointer size");
    switch (descriptor.kind) {
    case TargetAbiKind::SysVAMD64:
        if (descriptor.architecture != TargetArchitecture::X86_64 || descriptor.pointer_size != 8U ||
            descriptor.general_purpose_argument_registers != 6U ||
            descriptor.floating_point_argument_registers != 8U || descriptor.shadow_space_size != 0U)
            return fail("invalid SysV AMD64 descriptor fields");
        if (descriptor.operating_system == TargetOperatingSystem::Windows)
            return fail("SysV AMD64 descriptor cannot target Windows");
        if (descriptor.operating_system == TargetOperatingSystem::Linux && triple.find("linux") == std::string::npos)
            return fail("SysV Linux descriptor requires a Linux target triple");
        if (descriptor.operating_system == TargetOperatingSystem::MacOS &&
            triple.find("apple") == std::string::npos && triple.find("darwin") == std::string::npos)
            return fail("SysV macOS descriptor requires an Apple/Darwin target triple");
        if (descriptor.max_direct_aggregate_size != 16U)
            return fail("SysV AMD64 direct aggregate limit must be 16 bytes");
        break;
    case TargetAbiKind::Win64:
        if (descriptor.architecture != TargetArchitecture::X86_64 || descriptor.pointer_size != 8U ||
            descriptor.general_purpose_argument_registers != 4U ||
            descriptor.floating_point_argument_registers != 4U || descriptor.shadow_space_size != 32U)
            return fail("invalid Win64 descriptor fields");
        if (descriptor.operating_system != TargetOperatingSystem::Windows ||
            triple.find("windows") == std::string::npos)
            return fail("Win64 descriptor requires a Windows target triple");
        if (descriptor.max_direct_aggregate_size != 8U)
            return fail("Win64 direct aggregate size limit must be 8 bytes");
        break;
    case TargetAbiKind::AAPCS64:
        if (descriptor.architecture != TargetArchitecture::AArch64 || descriptor.pointer_size != 8U ||
            descriptor.general_purpose_argument_registers != 8U ||
            descriptor.floating_point_argument_registers != 8U || descriptor.shadow_space_size != 0U)
            return fail("invalid AAPCS64 descriptor fields");
        if (descriptor.operating_system == TargetOperatingSystem::Windows)
            return fail("AAPCS64 profile does not model Windows ARM64 yet");
        if (descriptor.operating_system == TargetOperatingSystem::Linux && triple.find("linux") == std::string::npos)
            return fail("AAPCS64 Linux descriptor requires a Linux target triple");
        if (descriptor.operating_system == TargetOperatingSystem::MacOS &&
            triple.find("apple") == std::string::npos && triple.find("darwin") == std::string::npos)
            return fail("AAPCS64 macOS descriptor requires an Apple/Darwin target triple");
        if (descriptor.max_direct_aggregate_size != 16U)
            return fail("AAPCS64 direct aggregate limit must be 16 bytes");
        break;
    }
    if (descriptor.max_direct_aggregate_size == 0U || descriptor.max_direct_aggregate_size > 64U)
        return fail("invalid direct aggregate limit");
    return true;
}

TargetAbiCallPlan classify_target_abi_call(
    const TargetAbiDescriptor& descriptor,
    const std::vector<AbiTypeLayout>& layouts,
    const TargetAbiCallRequest& request,
    std::vector<Diagnostic>* diagnostics) {
    TargetAbiCallPlan plan;
    plan.symbol_id = request.symbol_id;
    plan.linkage_name = request.linkage_name;
    plan.target_triple = descriptor.target_triple;
    plan.abi_kind = descriptor.kind;
    plan.source_calling_convention = request.calling_convention;
    plan.shadow_space_size = descriptor.shadow_space_size;

    std::string descriptor_reason;
    if (!validate_target_abi_descriptor(descriptor, &descriptor_reason)) {
        diag(diagnostics, "XTABI0001", "Invalid target ABI descriptor: " + descriptor_reason);
        return plan;
    }
    if (!resolve_calling_convention(descriptor, request.calling_convention)) {
        diag(diagnostics, "XTABI0002", "Calling convention is not supported by the selected target ABI");
        return plan;
    }
    if (request.variadic) {
        diag(diagnostics, "XTABI0003", "Variadic calls are not supported by the current target ABI lowering contract");
        return plan;
    }

    ClassificationState state;
    state.stack_cursor = descriptor.shadow_space_size;
    std::size_t source_index = 0U;
    for (const auto& argument : request.arguments) {
        const AbiTypeLayout* layout = find_layout(layouts, argument.type_name);
        AbiTypeLayout pointer_layout;
        if (is_reference_passing(argument.passing) || argument.hidden_sret) {
            pointer_layout.type_name = argument.type_name + "*";
            pointer_layout.kind = AbiTypeKind::Pointer;
            pointer_layout.size = descriptor.pointer_size;
            pointer_layout.alignment = descriptor.pointer_size;
            layout = &pointer_layout;
        }
        if (layout == nullptr) {
            diag(diagnostics, "XTABI0004", "Missing or ambiguous ABI layout for type '" + argument.type_name + "'");
            ++source_index;
            continue;
        }
        if (!valid_layout(*layout, descriptor)) {
            diag(diagnostics, "XTABI0005", "Invalid ABI layout for type '" + argument.type_name + "'");
            ++source_index;
            continue;
        }
        if (layout->kind == AbiTypeKind::Aggregate && descriptor.kind == TargetAbiKind::SysVAMD64 &&
            layout->eightbyte_classes.size() != 0U &&
            layout->eightbyte_classes.size() != (layout->size + 7U) / 8U &&
            layout->size <= 16U && !layout->contains_unaligned_fields) {
            diag(diagnostics, "XTABI0006", "SysV aggregate eightbyte classification count does not match its size");
            ++source_index;
            continue;
        }
        plan.arguments.push_back(classify_one(descriptor, *layout, argument, source_index, state));
        ++source_index;
    }
    plan.stack_argument_size = round_up(state.stack_cursor, descriptor.stack_alignment);
    if (diagnostics != nullptr && !diagnostics->empty()) return plan;
    return plan;
}

TargetAbiLoweringResult lower_drop_bindings_to_target_abi(
    const DropAbiBindingResult& bindings,
    const TargetAbiDescriptor& descriptor,
    const std::vector<AbiTypeLayout>& layouts) {
    TargetAbiLoweringResult result;
    if (!bindings.diagnostics.empty()) {
        result.diagnostics = bindings.diagnostics;
        return result;
    }
    for (const auto& function : bindings.functions) {
        TargetAbiFunctionPlan lowered_function;
        lowered_function.function_name = function.function_name;
        lowered_function.blocks.reserve(function.blocks.size());
        for (const auto& block : function.blocks) {
            TargetAbiBlockPlan lowered_block;
            lowered_block.id = block.id;
            lowered_block.reachable = block.reachable;
            if (!block.reachable) {
                lowered_function.blocks.push_back(std::move(lowered_block));
                continue;
            }
            for (const auto& binding : block.bindings) {
                TargetAbiCallPlan call;
                call.symbol_id = binding.symbol_id;
                call.linkage_name = binding.linkage_name;
                call.target_triple = descriptor.target_triple;
                call.abi_kind = descriptor.kind;
                call.source_calling_convention = binding.calling_convention;
                call.invocation_kind = binding.invocation_kind;
                call.guard = binding.guard;
                call.failure_mode = binding.failure_mode;
                call.terminate_if_failed = binding.terminate_if_failed;
                call.consumes_resource = binding.consumes_resource;
                call.is_noop = binding.invocation_kind == DropInvocationKind::NoOp;
                call.owner_type = binding.owner_type;
                call.resource_type = binding.resource_type;
                call.routine_name = binding.routine_name;
                call.action_index = binding.action_index;
                call.variable_id = binding.variable_id;
                call.span = binding.span;
                if (!call.is_noop) {
                    TargetAbiCallRequest request;
                    request.symbol_id = binding.symbol_id;
                    request.linkage_name = binding.linkage_name;
                    request.calling_convention = binding.calling_convention;
                    for (const auto& source_argument : binding.arguments) {
                        request.arguments.push_back(TargetAbiArgumentInput{
                            source_argument.type_name, source_argument.passing,
                            source_argument.role, false});
                    }
                    std::vector<Diagnostic> local_diagnostics;
                    auto classified = classify_target_abi_call(descriptor, layouts, request, &local_diagnostics);
                    if (!local_diagnostics.empty()) {
                        result.diagnostics.insert(result.diagnostics.end(),
                            local_diagnostics.begin(), local_diagnostics.end());
                        continue;
                    }
                    call.arguments = std::move(classified.arguments);
                    call.shadow_space_size = classified.shadow_space_size;
                    call.stack_argument_size = classified.stack_argument_size;
                }
                lowered_block.calls.push_back(std::move(call));
            }
            lowered_function.blocks.push_back(std::move(lowered_block));
        }
        result.functions.push_back(std::move(lowered_function));
    }
    if (!result.diagnostics.empty()) result.functions.clear();
    return result;
}

} // namespace xfrontend
