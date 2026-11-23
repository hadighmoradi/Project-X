#include "xfrontend/native_object_emitter.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <limits>
#include <set>
#include <string>
#include <utility>

namespace xfrontend {
namespace {

void add_diag(std::vector<Diagnostic>& diagnostics, const char* code, const std::string& message) {
    diagnostics.push_back(Diagnostic{code, message, {}});
}

DropSymbolVisibility to_native_visibility(Visibility visibility) {
    switch (visibility) {
    case Visibility::Public: return DropSymbolVisibility::Public;
    case Visibility::Internal: return DropSymbolVisibility::Internal;
    case Visibility::Private: return DropSymbolVisibility::Private;
    }
    return DropSymbolVisibility::Private;
}

DropParameterPassing to_parameter_passing(std::string value) {
    for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }), value.end());
    if (value == "take") return DropParameterPassing::Take;
    if (value == "borrow") return DropParameterPassing::Borrow;
    if (value == "borrowmut" || value == "borrow_mut") return DropParameterPassing::BorrowMut;
    return DropParameterPassing::Value;
}

std::string make_function_symbol_id(const CompilationUnit& unit, std::size_t index) {
    return canonicalize_native_symbol_name(unit.module_name) + "#function#" + std::to_string(index);
}

ObjectSymbol make_object_symbol(const NativeSymbolRecord& record, ObjectSymbolKind kind,
                                bool defined, ObjectSectionId section_id, std::size_t size) {
    ObjectSymbol symbol;
    symbol.name = record.mangled_name;
    symbol.signature_key = record.signature_key;
    symbol.binding = record.linkage == DropSymbolLinkage::Internal
        ? ObjectSymbolBinding::Local : ObjectSymbolBinding::Global;
    symbol.kind = kind;
    symbol.defined = defined;
    symbol.section_id = defined ? section_id : InvalidObjectSectionId;
    symbol.value = 0U;
    symbol.size = defined ? size : 0U;
    return symbol;
}

std::string section_name(const ObjectFileDescriptor& target, ObjectSectionKind kind,
                         ObjectSectionId id) {
    std::string prefix;
    if (kind == ObjectSectionKind::Text) prefix = target.format == ObjectFileFormat::COFF ? ".text$X" : ".text.X";
    else if (kind == ObjectSectionKind::ReadOnlyData) prefix = target.format == ObjectFileFormat::COFF ? ".rdata$X" : ".rodata.X";
    else if (kind == ObjectSectionKind::Bss) prefix = target.format == ObjectFileFormat::COFF ? ".bss$X" : ".bss.X";
    else prefix = target.format == ObjectFileFormat::COFF ? ".data$X" : ".data.X";
    return prefix + std::to_string(id);
}

bool power_of_two(std::size_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

void add_undefined_symbol_if_missing(ObjectFile& object, const std::string& name,
                                     const std::string& signature_key, ObjectSymbolKind kind) {
    const auto found = std::find_if(object.symbols.begin(), object.symbols.end(), [&name](const ObjectSymbol& symbol) {
        return symbol.name == name;
    });
    if (found != object.symbols.end()) return;
    ObjectSymbol symbol;
    symbol.name = name;
    symbol.signature_key = signature_key;
    symbol.binding = ObjectSymbolBinding::Global;
    symbol.kind = kind;
    symbol.defined = false;
    object.symbols.push_back(std::move(symbol));
}

bool append_relocations(ObjectFile& object, ObjectSectionId section_id,
                        const std::vector<NativeObjectRelocationInput>& inputs,
                        const std::vector<NativeSymbolRecord>& function_records,
                        std::vector<Diagnostic>& diagnostics, ObjectSymbolKind default_target_kind) {
    for (const auto& input : inputs) {
        std::string target_name;
        if (input.target_function_index != InvalidNativeFunctionIndex) {
            if (input.target_function_index >= function_records.size()) {
                add_diag(diagnostics, "XNOBJ0010", "relocation target function index is outside the source unit");
                continue;
            }
            target_name = function_records[input.target_function_index].mangled_name;
            if (target_name.empty()) {
                add_diag(diagnostics, "XNOBJ0013", "relocation target function has no emitted native symbol");
                continue;
            }
        } else {
            target_name = input.target_link_name;
        }
        if (target_name.empty()) {
            add_diag(diagnostics, "XNOBJ0011", "relocation must identify a source function or external linker symbol");
            continue;
        }
        const ObjectSymbolKind expected_kind = input.target_function_index == InvalidNativeFunctionIndex
            ? (input.target_kind == ObjectSymbolKind::Function ? default_target_kind : input.target_kind)
            : ObjectSymbolKind::Function;
        const auto target_symbol = std::find_if(object.symbols.begin(), object.symbols.end(),
            [&target_name](const ObjectSymbol& symbol) { return symbol.name == target_name; });
        if (target_symbol == object.symbols.end()) {
            if (input.target_function_index == InvalidNativeFunctionIndex && input.target_signature_key.empty()) {
                add_diag(diagnostics, "XNOBJ0014", "external relocation requires the resolved target signature key");
                continue;
            }
            const std::string expected_signature = input.target_function_index == InvalidNativeFunctionIndex
                ? input.target_signature_key : function_records[input.target_function_index].signature_key;
            add_undefined_symbol_if_missing(object, target_name, expected_signature, expected_kind);
        } else {
            const std::string expected_signature = input.target_function_index != InvalidNativeFunctionIndex
                ? function_records[input.target_function_index].signature_key
                : (!input.target_signature_key.empty() ? input.target_signature_key : target_symbol->signature_key);
            if (target_symbol->signature_key != expected_signature || target_symbol->kind != expected_kind) {
                add_diag(diagnostics, "XNOBJ0015", "relocation target symbol has a conflicting type signature or entity kind");
                continue;
            }
        }

        ObjectRelocation relocation;
        relocation.section_id = section_id;
        relocation.offset = input.offset;
        relocation.kind = input.kind;
        relocation.target_symbol = std::move(target_name);
        relocation.addend = input.addend;
        relocation.width = input.width;
        object.relocations.push_back(std::move(relocation));
    }
    return diagnostics.empty();
}

} // namespace

NativeObjectEmissionResult emit_native_object_sections(
    const CompilationUnit& unit,
    const std::vector<NativeFunctionCodeArtifact>& functions,
    const std::vector<NativeDataCodeArtifact>& data,
    NativeObjectEmissionOptions options) {
    NativeObjectEmissionResult result;
    result.object_file.object_id = std::move(options.object_id);
    result.object_file.descriptor = std::move(options.target);
    if (result.object_file.object_id.empty()) add_diag(result.diagnostics, "XNOBJ0001", "object identifier must not be empty");
    if (unit.module_name.empty()) add_diag(result.diagnostics, "XNOBJ0002", "source unit must declare a module name");
    std::string target_reason;
    if (!validate_object_file_descriptor(result.object_file.descriptor, &target_reason))
        add_diag(result.diagnostics, "XNOBJ0003", "invalid object target: " + target_reason);
    if (!result.diagnostics.empty()) return result;

    std::vector<NativeSymbolDeclaration> declarations;
    std::vector<std::size_t> declaration_indices;
    declarations.reserve(unit.functions.size() + data.size());
    declaration_indices.reserve(unit.functions.size());
    for (std::size_t index = 0U; index < unit.functions.size(); ++index) {
        const auto& function = unit.functions[index];
        if (!function.generic_parameters.empty()) {
            const bool has_artifact = std::any_of(functions.begin(), functions.end(), [index](const NativeFunctionCodeArtifact& artifact) {
                return artifact.function_index == index;
            });
            if (has_artifact) add_diag(result.diagnostics, "XNOBJ0004", "unspecialized generic function cannot be emitted; instantiate it first");
            continue;
        }
        NativeSymbolDeclaration declaration;
        declaration.symbol_id = make_function_symbol_id(unit, index);
        declaration.module_name = unit.module_name;
        declaration.source_name = function.name;
        declaration.return_type = function.return_type.empty() ? "void" : function.return_type;
        declaration.visibility = to_native_visibility(function.visibility);
        declaration.linkage = function.visibility == Visibility::Public
            ? DropSymbolLinkage::External : DropSymbolLinkage::Internal;
        declaration.entity_kind = NativeSymbolEntityKind::Function;
        declaration.defined = function.has_body;
        for (const auto& parameter : function.parameters) {
            declaration.parameters.push_back(NativeSymbolParameter{
                parameter.type_name.empty() ? "unknown" : parameter.type_name,
                to_parameter_passing(parameter.passing_mode)});
        }
        declarations.push_back(std::move(declaration));
        declaration_indices.push_back(index);
    }
    for (const auto& artifact : data) {
        if (artifact.declaration.entity_kind != NativeSymbolEntityKind::Global) {
            add_diag(result.diagnostics, "XNOBJ0005", "data artifact declaration must use Global entity kind");
            continue;
        }
        if (canonicalize_native_symbol_name(artifact.declaration.module_name) !=
            canonicalize_native_symbol_name(unit.module_name)) {
            add_diag(result.diagnostics, "XNOBJ0006", "data artifact module must match the source unit module");
            continue;
        }
        declarations.push_back(artifact.declaration);
    }
    if (!result.diagnostics.empty()) return result;

    result.symbol_table = build_native_symbol_table(declarations);
    if (!result.symbol_table.succeeded()) {
        result.diagnostics = result.symbol_table.diagnostics;
        return result;
    }

    std::map<std::size_t, const NativeFunctionCodeArtifact*> artifacts_by_index;
    for (const auto& artifact : functions) {
        if (artifact.function_index >= unit.functions.size()) {
            add_diag(result.diagnostics, "XNOBJ0007", "function artifact index is outside the source unit");
            continue;
        }
        if (!artifacts_by_index.emplace(artifact.function_index, &artifact).second)
            add_diag(result.diagnostics, "XNOBJ0008", "duplicate machine-code artifact for one source function");
        if (!unit.functions[artifact.function_index].has_body)
            add_diag(result.diagnostics, "XNOBJ0009", "machine-code artifact supplied for a declaration without a body");
        if (artifact.code.empty()) add_diag(result.diagnostics, "XNOBJ000A", "function machine code must not be empty");
        if (!power_of_two(artifact.alignment) || artifact.alignment > (1U << 20U))
            add_diag(result.diagnostics, "XNOBJ000B", "function code alignment must be a supported power of two");
    }
    for (const auto source_index : declaration_indices) {
        if (unit.functions[source_index].has_body && artifacts_by_index.find(source_index) == artifacts_by_index.end())
            add_diag(result.diagnostics, "XNOBJ000C", "defined source function has no machine-code artifact");
    }
    for (const auto& artifact : data) {
        const std::size_t logical_size = artifact.bss ? artifact.logical_size : artifact.contents.size();
        if (logical_size == 0U) add_diag(result.diagnostics, "XNOBJ000D", "global data artifact must have non-zero size");
        if (artifact.bss && !artifact.contents.empty()) add_diag(result.diagnostics, "XNOBJ000E", "BSS data artifact must not contain file bytes");
        if (!artifact.bss && artifact.logical_size != 0U && artifact.logical_size != artifact.contents.size())
            add_diag(result.diagnostics, "XNOBJ000F", "initialized global logical size must match content size");
        if (!power_of_two(artifact.alignment) || artifact.alignment > (1U << 20U))
            add_diag(result.diagnostics, "XNOBJ0012", "global data alignment must be a supported power of two");
    }
    if (!result.diagnostics.empty()) return result;

    std::vector<NativeSymbolRecord> function_records(unit.functions.size());
    for (std::size_t record_index = 0U; record_index < declaration_indices.size(); ++record_index)
        function_records[declaration_indices[record_index]] = result.symbol_table.symbols[record_index];

    ObjectSectionId next_section_id = 1U;
    std::vector<std::pair<ObjectSectionId, const NativeFunctionCodeArtifact*>> function_relocations;
    std::vector<std::pair<ObjectSectionId, const NativeDataCodeArtifact*>> data_relocations;
    for (const auto source_index : declaration_indices) {
        const auto& function = unit.functions[source_index];
        const auto artifact_it = artifacts_by_index.find(source_index);
        if (!function.has_body) {
            const auto& record = function_records[source_index];
            result.object_file.symbols.push_back(make_object_symbol(record, ObjectSymbolKind::Function,
                false, InvalidObjectSectionId, 0U));
            continue;
        }
        const auto& artifact = *artifact_it->second;
        ObjectSection section;
        section.id = next_section_id++;
        section.name = section_name(result.object_file.descriptor, ObjectSectionKind::Text, section.id);
        section.kind = ObjectSectionKind::Text;
        section.alignment = artifact.alignment;
        section.size = artifact.code.size();
        section.contents = artifact.code;
        section.linker_root = artifact.linker_root;
        section.comdat = result.object_file.descriptor.format == ObjectFileFormat::COFF;
        result.object_file.sections.push_back(section);
        const auto& record = function_records[source_index];
        result.object_file.symbols.push_back(make_object_symbol(record, ObjectSymbolKind::Function,
            true, section.id, section.size));
        function_relocations.emplace_back(section.id, &artifact);
    }

    std::size_t data_record_index = declaration_indices.size();
    for (const auto& artifact : data) {
        const auto& record = result.symbol_table.symbols[data_record_index++];
        const ObjectSectionKind kind = artifact.bss ? ObjectSectionKind::Bss
            : (artifact.read_only ? ObjectSectionKind::ReadOnlyData : ObjectSectionKind::Data);
        ObjectSection section;
        section.id = next_section_id++;
        section.name = section_name(result.object_file.descriptor, kind, section.id);
        section.kind = kind;
        section.alignment = artifact.alignment;
        section.size = artifact.bss ? artifact.logical_size : artifact.contents.size();
        section.contents = artifact.contents;
        section.linker_root = artifact.linker_root;
        section.comdat = result.object_file.descriptor.format == ObjectFileFormat::COFF;
        result.object_file.sections.push_back(section);
        result.object_file.symbols.push_back(make_object_symbol(record, ObjectSymbolKind::Object,
            true, section.id, section.size));
        data_relocations.emplace_back(section.id, &artifact);
    }

    // All definitions are installed before any undefined references are added,
    // so forward calls and mutually recursive functions resolve to one symbol.
    for (const auto& item : function_relocations) {
        append_relocations(result.object_file, item.first, item.second->relocations,
            function_records, result.diagnostics, ObjectSymbolKind::Function);
    }
    for (const auto& item : data_relocations) {
        append_relocations(result.object_file, item.first, item.second->relocations,
            function_records, result.diagnostics, ObjectSymbolKind::Object);
    }

    if (!result.diagnostics.empty()) return result;

    // Emit one RUNTIME_FUNCTION record and one UNWIND_INFO payload per function.
    // Both metadata sections are associative COMDATs keyed to the corresponding
    // text section, so /OPT:REF discards them together with an unused function.
    if (result.object_file.descriptor.format == ObjectFileFormat::COFF) {
        for (const auto& item : function_relocations) {
            const ObjectSectionId text_section_id = item.first;
            const NativeFunctionCodeArtifact& artifact = *item.second;
            if (artifact.win64_unwind_info.empty()) continue;
            if (artifact.function_index >= function_records.size()) {
                add_diag(result.diagnostics, "XNOBJ0016", "unwind metadata function index is outside declaration table");
                continue;
            }
            const std::string& function_symbol_name = function_records[artifact.function_index].mangled_name;
            const auto function_symbol = std::find_if(result.object_file.symbols.begin(), result.object_file.symbols.end(),
                [&function_symbol_name](const ObjectSymbol& symbol) {
                    return symbol.name == function_symbol_name && symbol.defined && symbol.kind == ObjectSymbolKind::Function;
                });
            if (function_symbol == result.object_file.symbols.end() || function_symbol->section_id != text_section_id) {
                add_diag(result.diagnostics, "XNOBJ0017", "unwind metadata has no matching emitted text function");
                continue;
            }
            if (artifact.code.empty() || artifact.code.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()) ||
                artifact.win64_unwind_info.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
                add_diag(result.diagnostics, "XNOBJ0018", "function code or unwind metadata exceeds COFF 32-bit section limits");
                continue;
            }

            const std::string suffix = std::to_string(artifact.function_index + 1U);
            ObjectSection xdata;
            xdata.id = next_section_id++;
            xdata.name = ".xdata$X" + suffix;
            xdata.kind = ObjectSectionKind::Metadata;
            xdata.alignment = 4U;
            xdata.size = artifact.win64_unwind_info.size();
            xdata.contents = artifact.win64_unwind_info;
            xdata.comdat = true;
            xdata.associative_to_section_id = text_section_id;
            const ObjectSectionId xdata_section_id = xdata.id;
            result.object_file.sections.push_back(std::move(xdata));

            const std::string xdata_symbol_name = "__xdata$" + function_symbol_name;
            ObjectSymbol xdata_symbol;
            xdata_symbol.name = xdata_symbol_name;
            xdata_symbol.signature_key = "win64-unwind-info-v1";
            xdata_symbol.binding = ObjectSymbolBinding::Local;
            xdata_symbol.kind = ObjectSymbolKind::Object;
            xdata_symbol.defined = true;
            xdata_symbol.section_id = xdata_section_id;
            xdata_symbol.value = 0U;
            xdata_symbol.size = artifact.win64_unwind_info.size();
            result.object_file.symbols.push_back(std::move(xdata_symbol));

            ObjectSection pdata;
            pdata.id = next_section_id++;
            pdata.name = ".pdata$X" + suffix;
            pdata.kind = ObjectSectionKind::Metadata;
            pdata.alignment = 4U;
            pdata.size = 12U;
            pdata.contents.assign(12U, 0U);
            pdata.comdat = true;
            pdata.associative_to_section_id = text_section_id;
            const ObjectSectionId pdata_section_id = pdata.id;
            result.object_file.sections.push_back(std::move(pdata));

            result.object_file.relocations.push_back(ObjectRelocation{
                pdata_section_id, 0U, ObjectRelocationKind::CoffAddr32NB,
                function_symbol_name, 0, 4U});
            result.object_file.relocations.push_back(ObjectRelocation{
                pdata_section_id, 4U, ObjectRelocationKind::CoffAddr32NB,
                function_symbol_name, static_cast<std::int64_t>(artifact.code.size()), 4U});
            result.object_file.relocations.push_back(ObjectRelocation{
                pdata_section_id, 8U, ObjectRelocationKind::CoffAddr32NB,
                xdata_symbol_name, 0, 4U});
        }
    } else {
        for (const auto& item : function_relocations) {
            if (!item.second->win64_unwind_info.empty())
                add_diag(result.diagnostics, "XNOBJ0019", "Win64 unwind metadata cannot be emitted for a non-COFF target");
        }
    }
    if (!result.diagnostics.empty()) return result;

    const auto validation = validate_object_file(result.object_file);
    result.diagnostics.insert(result.diagnostics.end(), validation.diagnostics.begin(), validation.diagnostics.end());
    return result;
}

} // namespace xfrontend
