// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/object_file.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>

namespace xfrontend {
namespace {

void add_diag(std::vector<Diagnostic>& out, const char* code, const std::string& message) {
    out.push_back(Diagnostic{code, message, {}});
}

bool is_power_of_two(std::size_t value) {
    return value != 0U && (value & (value - 1U)) == 0U;
}

bool triple_matches_os(const std::string& triple, ObjectOperatingSystem os) {
    const auto has = [&triple](const std::string& part) {
        return triple.find(part) != std::string::npos;
    };
    switch (os) {
    case ObjectOperatingSystem::Linux: return has("linux") || has("android");
    case ObjectOperatingSystem::Windows: return has("windows") || has("win32") || has("mingw");
    case ObjectOperatingSystem::MacOS: return has("darwin") || has("apple") || has("macos");
    }
    return false;
}

bool format_matches_os(ObjectFileFormat format, ObjectOperatingSystem os) {
    switch (format) {
    case ObjectFileFormat::ELF: return os == ObjectOperatingSystem::Linux;
    case ObjectFileFormat::COFF: return os == ObjectOperatingSystem::Windows;
    case ObjectFileFormat::MachO: return os == ObjectOperatingSystem::MacOS;
    }
    return false;
}

std::size_t relocation_width(ObjectRelocationKind kind) {
    switch (kind) {
    case ObjectRelocationKind::Absolute64:
    case ObjectRelocationKind::CoffAddress64: return 8U;
    case ObjectRelocationKind::PcRelative32:
    case ObjectRelocationKind::PltRelative32:
    case ObjectRelocationKind::CoffRelative32:
    case ObjectRelocationKind::CoffAddr32NB:
    case ObjectRelocationKind::AArch64Call26:
    case ObjectRelocationKind::AArch64Page21:
    case ObjectRelocationKind::AArch64PageOffset12: return 4U;
    }
    return 0U;
}

bool relocation_supported(const ObjectFileDescriptor& d, ObjectRelocationKind kind) {
    if (d.architecture == ObjectArchitecture::X86_64) {
        if (d.format == ObjectFileFormat::ELF || d.format == ObjectFileFormat::MachO)
            return kind == ObjectRelocationKind::Absolute64 || kind == ObjectRelocationKind::PcRelative32 ||
                   (d.format == ObjectFileFormat::ELF && kind == ObjectRelocationKind::PltRelative32);
        return kind == ObjectRelocationKind::CoffAddress64 || kind == ObjectRelocationKind::CoffRelative32 ||
               kind == ObjectRelocationKind::CoffAddr32NB;
    }
    if (d.architecture == ObjectArchitecture::AArch64) {
        if (kind == ObjectRelocationKind::Absolute64) return true;
        return kind == ObjectRelocationKind::AArch64Call26 ||
               kind == ObjectRelocationKind::AArch64Page21 ||
               kind == ObjectRelocationKind::AArch64PageOffset12;
    }
    return false;
}

const ObjectSymbol* find_local_definition(const ObjectFile& object, const std::string& name) {
    const auto found = std::find_if(object.symbols.begin(), object.symbols.end(),
        [&name](const ObjectSymbol& symbol) {
            return symbol.name == name && symbol.defined && symbol.binding == ObjectSymbolBinding::Local;
        });
    return found == object.symbols.end() ? nullptr : &*found;
}

struct ChosenDefinition {
    ObjectSymbolResolution resolution;
    bool strong = false;
};

bool same_target(const ObjectFileDescriptor& a, const ObjectFileDescriptor& b) {
    return a.format == b.format && a.architecture == b.architecture &&
           a.operating_system == b.operating_system && a.pointer_size == b.pointer_size &&
           a.byte_order == b.byte_order && a.target_triple == b.target_triple;
}

} // namespace

ObjectSymbol make_object_symbol_from_native_record(const NativeSymbolRecord& record,
    ObjectSymbolKind kind, bool defined, ObjectSectionId section_id, std::size_t value,
    std::size_t size, bool weak) {
    ObjectSymbol symbol;
    symbol.name = record.mangled_name;
    symbol.signature_key = record.signature_key;
    symbol.binding = record.linkage == DropSymbolLinkage::Internal
        ? ObjectSymbolBinding::Local
        : (weak ? ObjectSymbolBinding::Weak : ObjectSymbolBinding::Global);
    symbol.kind = kind;
    symbol.defined = defined;
    symbol.section_id = defined ? section_id : InvalidObjectSectionId;
    symbol.value = defined ? value : 0U;
    symbol.size = defined ? size : 0U;
    return symbol;
}

ObjectFileDescriptor make_elf_x86_64_object_target() {
    return ObjectFileDescriptor{ObjectFileFormat::ELF, ObjectArchitecture::X86_64,
        ObjectOperatingSystem::Linux, ObjectByteOrder::LittleEndian,
        "x86_64-unknown-linux-gnu", 8U};
}

ObjectFileDescriptor make_coff_x86_64_object_target() {
    return ObjectFileDescriptor{ObjectFileFormat::COFF, ObjectArchitecture::X86_64,
        ObjectOperatingSystem::Windows, ObjectByteOrder::LittleEndian,
        "x86_64-pc-windows-msvc", 8U};
}

ObjectFileDescriptor make_macho_aarch64_object_target() {
    return ObjectFileDescriptor{ObjectFileFormat::MachO, ObjectArchitecture::AArch64,
        ObjectOperatingSystem::MacOS, ObjectByteOrder::LittleEndian,
        "arm64-apple-darwin", 8U};
}

bool validate_object_file_descriptor(const ObjectFileDescriptor& descriptor, std::string* reason) {
    const auto reject = [reason](const char* message) {
        if (reason != nullptr) *reason = message;
        return false;
    };
    if (descriptor.target_triple.empty()) return reject("target triple must not be empty");
    if (descriptor.pointer_size != 8U) return reject("only 64-bit object targets are supported");
    if (descriptor.byte_order != ObjectByteOrder::LittleEndian)
        return reject("big-endian object targets are not supported by this model");
    if (!format_matches_os(descriptor.format, descriptor.operating_system))
        return reject("object format is incompatible with target operating system");
    if (!triple_matches_os(descriptor.target_triple, descriptor.operating_system))
        return reject("target triple does not describe the selected operating system");
    if (descriptor.architecture == ObjectArchitecture::X86_64 &&
        descriptor.target_triple.find("x86_64") == std::string::npos &&
        descriptor.target_triple.find("amd64") == std::string::npos)
        return reject("target triple does not describe x86_64 architecture");
    if (descriptor.architecture == ObjectArchitecture::AArch64 &&
        descriptor.target_triple.find("aarch64") == std::string::npos &&
        descriptor.target_triple.find("arm64") == std::string::npos)
        return reject("target triple does not describe AArch64 architecture");
    if (descriptor.format == ObjectFileFormat::COFF && descriptor.operating_system != ObjectOperatingSystem::Windows)
        return reject("COFF profile currently requires Windows");
    return true;
}

ObjectFileValidationResult validate_object_file(const ObjectFile& object_file) {
    ObjectFileValidationResult result;
    std::string reason;
    if (object_file.object_id.empty()) add_diag(result.diagnostics, "XOBJ0001", "object identifier must not be empty");
    if (!validate_object_file_descriptor(object_file.descriptor, &reason))
        add_diag(result.diagnostics, "XOBJ0002", reason);

    std::map<ObjectSectionId, const ObjectSection*> sections;
    std::set<std::string> section_names;
    for (const auto& section : object_file.sections) {
        if (section.id == InvalidObjectSectionId || !sections.emplace(section.id, &section).second)
            add_diag(result.diagnostics, "XOBJ0003", "section identifiers must be unique and non-zero");
        if (section.name.empty() || !section_names.insert(section.name).second)
            add_diag(result.diagnostics, "XOBJ0004", "section names must be non-empty and unique");
        if (!is_power_of_two(section.alignment) || section.alignment > (1U << 20U))
            add_diag(result.diagnostics, "XOBJ0005", "section alignment must be a supported power of two");
        if (section.kind == ObjectSectionKind::Bss) {
            if (!section.contents.empty()) add_diag(result.diagnostics, "XOBJ0006", "BSS sections must not contain file bytes");
        } else if (section.contents.size() != section.size) {
            add_diag(result.diagnostics, "XOBJ0007", "non-BSS section content size must equal logical section size");
        }
    }

    std::map<std::string, int> defined_external_counts;
    for (const auto& symbol : object_file.symbols) {
        if (symbol.name.empty()) {
            add_diag(result.diagnostics, "XOBJ0008", "symbol name must not be empty");
            continue;
        }
        if (symbol.defined) {
            if (symbol.section_id == InvalidObjectSectionId) {
                add_diag(result.diagnostics, "XOBJ0009", "defined symbol must reference a section");
            } else {
                const auto section = sections.find(symbol.section_id);
                if (section == sections.end()) {
                    add_diag(result.diagnostics, "XOBJ0010", "defined symbol references an unknown section");
                } else if (symbol.value > section->second->size ||
                           symbol.size > section->second->size - std::min(symbol.value, section->second->size)) {
                    add_diag(result.diagnostics, "XOBJ0011", "symbol range extends beyond its section");
                }
            }
            if (symbol.binding != ObjectSymbolBinding::Local && ++defined_external_counts[symbol.name] > 1)
                add_diag(result.diagnostics, "XOBJ0012", "object contains multiple external definitions for one symbol");
        } else {
            if (symbol.binding == ObjectSymbolBinding::Local)
                add_diag(result.diagnostics, "XOBJ0013", "undefined symbols cannot have local binding");
            if (symbol.section_id != InvalidObjectSectionId || symbol.value != 0U || symbol.size != 0U)
                add_diag(result.diagnostics, "XOBJ0014", "undefined symbol must not carry section or storage coordinates");
        }
    }

    std::set<std::tuple<ObjectSectionId, std::size_t, ObjectRelocationKind, std::string>> relocations;
    for (const auto& section : object_file.sections) {
        if (section.associative_to_section_id != InvalidObjectSectionId) {
            const auto associated = sections.find(section.associative_to_section_id);
            if (!section.comdat || associated == sections.end() ||
                section.associative_to_section_id == section.id) {
                add_diag(result.diagnostics, "XOBJ0023",
                    "associative COMDAT section must name a distinct existing key section");
            }
        }
    }

    for (const auto& relocation : object_file.relocations) {
        const auto section = sections.find(relocation.section_id);
        if (section == sections.end()) {
            add_diag(result.diagnostics, "XOBJ0015", "relocation references an unknown source section");
            continue;
        }
        if (relocation.target_symbol.empty())
            add_diag(result.diagnostics, "XOBJ0016", "relocation target symbol must not be empty");
        const bool symbol_exists = std::any_of(object_file.symbols.begin(), object_file.symbols.end(),
            [&relocation](const ObjectSymbol& symbol) { return symbol.name == relocation.target_symbol; });
        if (!symbol_exists) add_diag(result.diagnostics, "XOBJ0017", "relocation target is absent from the object's symbol table");
        if (!relocation_supported(object_file.descriptor, relocation.kind))
            add_diag(result.diagnostics, "XOBJ0018", "relocation kind is not supported by this object format and architecture");
        const std::size_t expected_width = relocation_width(relocation.kind);
        if (expected_width == 0U || relocation.width != expected_width)
            add_diag(result.diagnostics, "XOBJ0019", "relocation width does not match relocation kind");
        if (relocation.offset > section->second->size || relocation.width > section->second->size -
            std::min(relocation.offset, section->second->size))
            add_diag(result.diagnostics, "XOBJ0020", "relocation field extends beyond source section");
        if (object_file.descriptor.architecture == ObjectArchitecture::AArch64 &&
            (relocation.kind == ObjectRelocationKind::AArch64Call26 ||
             relocation.kind == ObjectRelocationKind::AArch64Page21 ||
             relocation.kind == ObjectRelocationKind::AArch64PageOffset12) && relocation.offset % 4U != 0U)
            add_diag(result.diagnostics, "XOBJ0021", "AArch64 instruction relocation must be four-byte aligned");
        const auto key = std::make_tuple(relocation.section_id, relocation.offset, relocation.kind, relocation.target_symbol);
        if (!relocations.insert(key).second)
            add_diag(result.diagnostics, "XOBJ0022", "duplicate relocation record at the same field");
    }
    return result;
}

ObjectLinkPlanResult build_object_link_plan(const ObjectFileDescriptor& target,
    const std::vector<ObjectFile>& input_objects, const std::vector<StaticArchive>& archives,
    ObjectLinkOptions options) {
    ObjectLinkPlanResult result;
    std::string reason;
    if (!validate_object_file_descriptor(target, &reason)) {
        add_diag(result.diagnostics, "XOBJ0100", "invalid link target: " + reason);
        return result;
    }

    struct SelectedObject { const ObjectFile* object; std::string archive_id; std::string member_id; };
    std::vector<SelectedObject> selected;
    std::set<std::string> selected_ids;
    for (const auto& object : input_objects) {
        const auto validation = validate_object_file(object);
        result.diagnostics.insert(result.diagnostics.end(), validation.diagnostics.begin(), validation.diagnostics.end());
        if (!same_target(object.descriptor, target)) add_diag(result.diagnostics, "XOBJ0101", "input object target does not match link target");
        if (!selected_ids.insert(object.object_id).second) add_diag(result.diagnostics, "XOBJ0102", "duplicate object identifier in link inputs");
        selected.push_back(SelectedObject{&object, {}, {}});
    }
    for (const auto& archive : archives) {
        if (archive.archive_id.empty()) add_diag(result.diagnostics, "XOBJ0103", "archive identifier must not be empty");
        std::set<std::string> member_ids;
        for (const auto& member : archive.members) {
            if (member.member_id.empty() || !member_ids.insert(member.member_id).second)
                add_diag(result.diagnostics, "XOBJ0104", "archive member identifiers must be non-empty and unique");
            const auto validation = validate_object_file(member.object_file);
            result.diagnostics.insert(result.diagnostics.end(), validation.diagnostics.begin(), validation.diagnostics.end());
            if (!same_target(member.object_file.descriptor, target))
                add_diag(result.diagnostics, "XOBJ0105", "archive member target does not match link target");
        }
    }
    if (!result.diagnostics.empty()) return result;

    const auto unresolved_names = [&selected]() {
        std::set<std::string> names;
        for (const auto& item : selected) {
            for (const auto& symbol : item.object->symbols)
                if (!symbol.defined && symbol.binding == ObjectSymbolBinding::Global) names.insert(symbol.name);
        }
        return names;
    };

    if (options.extract_archive_members) {
        bool changed = true;
        std::set<std::string> extracted_keys;
        while (changed) {
            changed = false;
            const auto unresolved = unresolved_names();
            if (unresolved.empty()) break;
            for (const auto& archive : archives) {
                for (const auto& member : archive.members) {
                    const std::string key = archive.archive_id + "::" + member.member_id;
                    if (extracted_keys.find(key) != extracted_keys.end()) continue;
                    bool satisfies = false;
                    for (const auto& symbol : member.object_file.symbols) {
                        if (symbol.defined && symbol.binding != ObjectSymbolBinding::Local &&
                            unresolved.find(symbol.name) != unresolved.end()) { satisfies = true; break; }
                    }
                    if (satisfies) {
                        if (selected_ids.insert(member.object_file.object_id).second) {
                            selected.push_back(SelectedObject{&member.object_file, archive.archive_id, member.member_id});
                            result.extracted_archive_members.push_back(key);
                            extracted_keys.insert(key);
                            changed = true;
                            break;
                        }
                        extracted_keys.insert(key);
                    }
                }
                if (changed) break;
            }
        }
    }

    std::map<std::string, ChosenDefinition> definitions;
    for (const auto& item : selected) {
        result.selected_object_ids.push_back(item.object->object_id);
        for (const auto& symbol : item.object->symbols) {
            if (!symbol.defined || symbol.binding == ObjectSymbolBinding::Local) continue;
            const bool strong = symbol.binding == ObjectSymbolBinding::Global;
            ObjectSymbolResolution resolution;
            resolution.symbol_name = symbol.name;
            resolution.object_id = item.object->object_id;
            resolution.archive_id = item.archive_id;
            resolution.archive_member_id = item.member_id;
            resolution.signature_key = symbol.signature_key;
            resolution.binding = symbol.binding;
            resolution.kind = symbol.kind;
            resolution.section_id = symbol.section_id;
            resolution.value = symbol.value;
            auto found = definitions.find(symbol.name);
            if (found == definitions.end()) {
                definitions.emplace(symbol.name, ChosenDefinition{resolution, strong});
            } else if (found->second.strong && strong) {
                add_diag(result.diagnostics, "XOBJ0106", "multiple strong definitions for symbol '" + symbol.name + "'");
            } else if (!found->second.strong && strong) {
                found->second = ChosenDefinition{resolution, true};
            }
        }
    }

    for (const auto& item : selected) {
        for (const auto& symbol : item.object->symbols) {
            if (symbol.defined) continue;
            const auto found = definitions.find(symbol.name);
            if (found == definitions.end()) {
                if (symbol.binding == ObjectSymbolBinding::Weak) {
                    result.unresolved_weak_symbols.push_back(symbol.name);
                } else {
                    add_diag(result.diagnostics, "XOBJ0107", "unresolved strong symbol '" + symbol.name + "'");
                }
            } else if (!symbol.signature_key.empty() && !found->second.resolution.signature_key.empty() &&
                       symbol.signature_key != found->second.resolution.signature_key) {
                add_diag(result.diagnostics, "XOBJ0108", "symbol signature mismatch for '" + symbol.name + "'");
            }
        }
    }

    for (const auto& entry : definitions) result.resolved_symbols.push_back(entry.second.resolution);

    for (const auto& item : selected) {
        for (const auto& relocation : item.object->relocations) {
            ObjectRelocationPlan plan;
            plan.source_object_id = item.object->object_id;
            plan.source_section_id = relocation.section_id;
            plan.offset = relocation.offset;
            plan.kind = relocation.kind;
            plan.target_symbol = relocation.target_symbol;
            const ObjectSymbol* local = find_local_definition(*item.object, relocation.target_symbol);
            if (local != nullptr) {
                plan.resolved = true;
                plan.target_object_id = item.object->object_id;
            } else {
                const auto found = definitions.find(relocation.target_symbol);
                if (found != definitions.end()) {
                    plan.resolved = true;
                    plan.target_object_id = found->second.resolution.object_id;
                } else {
                    const auto weak = std::find(result.unresolved_weak_symbols.begin(), result.unresolved_weak_symbols.end(),
                                                relocation.target_symbol);
                    if (weak != result.unresolved_weak_symbols.end() && options.allow_unresolved_weak) {
                        plan.resolved = true;
                        plan.weak_zero_fallback = true;
                    } else {
                        add_diag(result.diagnostics, "XOBJ0109", "relocation target is unresolved: '" + relocation.target_symbol + "'");
                    }
                }
            }
            result.relocations.push_back(std::move(plan));
        }
    }
    std::sort(result.unresolved_weak_symbols.begin(), result.unresolved_weak_symbols.end());
    result.unresolved_weak_symbols.erase(std::unique(result.unresolved_weak_symbols.begin(), result.unresolved_weak_symbols.end()),
                                         result.unresolved_weak_symbols.end());
    return result;
}

} // namespace xfrontend
