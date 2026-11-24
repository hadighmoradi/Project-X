// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "xfrontend/native_symbol.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace xfrontend {

enum class ObjectFileFormat { ELF, COFF, MachO };
enum class ObjectArchitecture { X86_64, AArch64 };
enum class ObjectOperatingSystem { Linux, Windows, MacOS };
enum class ObjectByteOrder { LittleEndian, BigEndian };
enum class ObjectSectionKind { Text, ReadOnlyData, Data, Bss, Metadata };
enum class ObjectSymbolBinding { Local, Global, Weak };
enum class ObjectSymbolKind { Function, Object, Section };
enum class ObjectRelocationKind {
    Absolute64,
    PcRelative32,
    PltRelative32,
    CoffRelative32,
    CoffAddress64,
    CoffAddr32NB,
    AArch64Call26,
    AArch64Page21,
    AArch64PageOffset12
};

using ObjectSectionId = std::uint32_t;
constexpr ObjectSectionId InvalidObjectSectionId = 0U;

struct ObjectFileDescriptor {
    ObjectFileFormat format = ObjectFileFormat::ELF;
    ObjectArchitecture architecture = ObjectArchitecture::X86_64;
    ObjectOperatingSystem operating_system = ObjectOperatingSystem::Linux;
    ObjectByteOrder byte_order = ObjectByteOrder::LittleEndian;
    std::string target_triple = "x86_64-unknown-linux-gnu";
    std::size_t pointer_size = 8U;
};

struct ObjectSection {
    ObjectSectionId id = InvalidObjectSectionId;
    std::string name;
    ObjectSectionKind kind = ObjectSectionKind::Text;
    std::size_t alignment = 1U;
    std::size_t size = 0U;
    // BSS sections have a logical size but no file contents.
    std::vector<std::uint8_t> contents;
    // Sections with registration/initialization side effects are linker roots.
    bool linker_root = false;
    // COFF COMDAT sections can be discarded independently by link.exe /OPT:REF.
    bool comdat = false;
    // Non-zero for IMAGE_COMDAT_SELECT_ASSOCIATIVE sections. The associated
    // section is discarded or retained with this key section.
    ObjectSectionId associative_to_section_id = InvalidObjectSectionId;
};

struct ObjectSymbol {
    std::string name;
    std::string signature_key;
    ObjectSymbolBinding binding = ObjectSymbolBinding::Global;
    ObjectSymbolKind kind = ObjectSymbolKind::Function;
    bool defined = true;
    ObjectSectionId section_id = InvalidObjectSectionId;
    std::size_t value = 0U;
    std::size_t size = 0U;
};

// Adapts a validated N21 NativeSymbolRecord into an object-level symbol entry.
// Internal linkage becomes local binding; undefined references carry no section
// coordinates. External symbols may be marked weak by the object writer.
ObjectSymbol make_object_symbol_from_native_record(
    const NativeSymbolRecord& record, ObjectSymbolKind kind, bool defined,
    ObjectSectionId section_id = InvalidObjectSectionId, std::size_t value = 0U,
    std::size_t size = 0U, bool weak = false);

struct ObjectRelocation {
    ObjectSectionId section_id = InvalidObjectSectionId;
    std::size_t offset = 0U;
    ObjectRelocationKind kind = ObjectRelocationKind::Absolute64;
    std::string target_symbol;
    std::int64_t addend = 0;
    std::size_t width = 8U;
};

struct ObjectFile {
    std::string object_id;
    ObjectFileDescriptor descriptor;
    std::vector<ObjectSection> sections;
    std::vector<ObjectSymbol> symbols;
    std::vector<ObjectRelocation> relocations;
};

struct ObjectFileValidationResult {
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty(); }
};

ObjectFileDescriptor make_elf_x86_64_object_target();
ObjectFileDescriptor make_coff_x86_64_object_target();
ObjectFileDescriptor make_macho_aarch64_object_target();
bool validate_object_file_descriptor(const ObjectFileDescriptor& descriptor,
                                     std::string* reason = nullptr);
ObjectFileValidationResult validate_object_file(const ObjectFile& object_file);

struct StaticArchiveMember {
    std::string member_id;
    ObjectFile object_file;
};

struct StaticArchive {
    std::string archive_id;
    std::vector<StaticArchiveMember> members;
};

struct ObjectLinkOptions {
    bool extract_archive_members = true;
    bool allow_unresolved_weak = true;
};

struct ObjectSymbolResolution {
    std::string symbol_name;
    std::string object_id;
    std::string archive_id;
    std::string archive_member_id;
    std::string signature_key;
    ObjectSymbolBinding binding = ObjectSymbolBinding::Global;
    ObjectSymbolKind kind = ObjectSymbolKind::Function;
    std::size_t section_id = 0U;
    std::size_t value = 0U;
    bool weak_fallback = false;
};

struct ObjectRelocationPlan {
    std::string source_object_id;
    ObjectSectionId source_section_id = InvalidObjectSectionId;
    std::size_t offset = 0U;
    ObjectRelocationKind kind = ObjectRelocationKind::Absolute64;
    std::string target_symbol;
    std::string target_object_id;
    bool resolved = false;
    bool weak_zero_fallback = false;
};

struct ObjectLinkPlanResult {
    std::vector<std::string> selected_object_ids;
    std::vector<std::string> extracted_archive_members;
    std::vector<ObjectSymbolResolution> resolved_symbols;
    std::vector<std::string> unresolved_weak_symbols;
    std::vector<ObjectRelocationPlan> relocations;
    std::vector<Diagnostic> diagnostics;
    bool succeeded() const noexcept { return diagnostics.empty(); }
};

// Performs deterministic validation, static-archive member selection, symbol
// resolution and relocation planning. It does not serialize COFF/ELF/Mach-O bytes,
// apply relocation values to machine code, or invoke a platform linker.
ObjectLinkPlanResult build_object_link_plan(
    const ObjectFileDescriptor& target,
    const std::vector<ObjectFile>& input_objects,
    const std::vector<StaticArchive>& archives = {},
    ObjectLinkOptions options = {});

} // namespace xfrontend
