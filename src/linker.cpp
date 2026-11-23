#include "xfrontend/linker.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace xfrontend {
namespace {

using SectionKey = std::pair<std::string, ObjectSectionId>;

void add_diag(std::vector<Diagnostic>& out, const char* code, const std::string& message) {
    out.push_back(Diagnostic{code, message, {}});
}

bool align_up_u64(std::uint64_t value, std::size_t alignment, std::uint64_t& result) {
    if (alignment == 0U || (alignment & (alignment - 1U)) != 0U) return false;
    const std::uint64_t mask = static_cast<std::uint64_t>(alignment - 1U);
    if (value > std::numeric_limits<std::uint64_t>::max() - mask) return false;
    result = (value + mask) & ~mask;
    return true;
}

bool checked_add_signed(std::uint64_t base, std::int64_t addend, std::uint64_t& result) {
    if (addend >= 0) {
        const auto amount = static_cast<std::uint64_t>(addend);
        if (base > std::numeric_limits<std::uint64_t>::max() - amount) return false;
        result = base + amount;
        return true;
    }
    const auto amount = static_cast<std::uint64_t>(-(addend + 1)) + 1U;
    if (base < amount) return false;
    result = base - amount;
    return true;
}

bool checked_add_i64(std::int64_t a, std::int64_t b, std::int64_t& result) {
    if ((b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) ||
        (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b)) return false;
    result = a + b;
    return true;
}

bool checked_sub_i64(std::int64_t a, std::int64_t b, std::int64_t& result) {
    if (b == std::numeric_limits<std::int64_t>::min()) {
        if (a >= 0) return false;
        result = a - b;
        return true;
    }
    return checked_add_i64(a, -b, result);
}

void write_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned i = 0U; i < 4U; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
}

void write_u64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    for (unsigned i = 0U; i < 8U; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
}

const ObjectFile* find_object(const std::string& id,
                              const std::vector<ObjectFile>& inputs,
                              const std::vector<StaticArchive>& archives) {
    const auto input = std::find_if(inputs.begin(), inputs.end(), [&id](const ObjectFile& object) {
        return object.object_id == id;
    });
    if (input != inputs.end()) return &*input;
    for (const auto& archive : archives) {
        for (const auto& member : archive.members) {
            if (member.object_file.object_id == id) return &member.object_file;
        }
    }
    return nullptr;
}

const LinkedImageSection* find_linked_section(const LinkedImage& image,
                                               const std::string& object_id,
                                               ObjectSectionId section_id) {
    const auto found = std::find_if(image.sections.begin(), image.sections.end(),
        [&object_id, section_id](const LinkedImageSection& section) {
            return section.object_id == object_id && section.source_section_id == section_id;
        });
    return found == image.sections.end() ? nullptr : &*found;
}

const ObjectSymbol* find_local_symbol(const ObjectFile& object, const std::string& name) {
    const auto found = std::find_if(object.symbols.begin(), object.symbols.end(),
        [&name](const ObjectSymbol& symbol) {
            return symbol.name == name && symbol.defined && symbol.binding == ObjectSymbolBinding::Local;
        });
    return found == object.symbols.end() ? nullptr : &*found;
}

const ObjectSymbolResolution* find_resolution(const ObjectLinkPlanResult& plan,
                                               const std::string& name) {
    const auto found = std::find_if(plan.resolved_symbols.begin(), plan.resolved_symbols.end(),
        [&name](const ObjectSymbolResolution& symbol) { return symbol.symbol_name == name; });
    return found == plan.resolved_symbols.end() ? nullptr : &*found;
}

bool resolve_symbol_address(const ObjectFile& source_object, const ObjectRelocation& relocation,
                            const LinkedImage& image, const ObjectLinkPlanResult& plan,
                            std::uint64_t& address, bool& weak_fallback,
                            std::vector<Diagnostic>& diagnostics) {
    weak_fallback = false;
    const ObjectSymbol* local = find_local_symbol(source_object, relocation.target_symbol);
    if (local != nullptr) {
        const auto* section = find_linked_section(image, source_object.object_id, local->section_id);
        if (section == nullptr) {
            add_diag(diagnostics, "XLINK0001", "local relocation target section is absent from linked image");
            return false;
        }
        if (section->address > std::numeric_limits<std::uint64_t>::max() - local->value) {
            add_diag(diagnostics, "XLINK0002", "local symbol address overflows 64-bit address space");
            return false;
        }
        address = section->address + static_cast<std::uint64_t>(local->value);
        return true;
    }

    const auto* resolution = find_resolution(plan, relocation.target_symbol);
    if (resolution == nullptr) {
        const auto weak = std::find(plan.unresolved_weak_symbols.begin(), plan.unresolved_weak_symbols.end(),
                                    relocation.target_symbol);
        if (weak != plan.unresolved_weak_symbols.end()) {
            address = 0U;
            weak_fallback = true;
            return true;
        }
        add_diag(diagnostics, "XLINK0003", "relocation symbol has no resolved definition");
        return false;
    }
    const auto* section = find_linked_section(image, resolution->object_id,
                                               static_cast<ObjectSectionId>(resolution->section_id));
    if (section == nullptr) {
        add_diag(diagnostics, "XLINK0004", "resolved symbol section is absent from linked image");
        return false;
    }
    if (section->address > std::numeric_limits<std::uint64_t>::max() - resolution->value) {
        add_diag(diagnostics, "XLINK0005", "resolved symbol address overflows 64-bit address space");
        return false;
    }
    address = section->address + static_cast<std::uint64_t>(resolution->value);
    return true;
}

bool apply_one_relocation(const ObjectFile& object, const ObjectRelocation& relocation,
                          LinkedImage& image, const ObjectLinkPlanResult& plan,
                          std::vector<Diagnostic>& diagnostics) {
    auto section_it = std::find_if(image.sections.begin(), image.sections.end(),
        [&object, &relocation](const LinkedImageSection& section) {
            return section.object_id == object.object_id && section.source_section_id == relocation.section_id;
        });
    if (section_it == image.sections.end()) {
        add_diag(diagnostics, "XLINK0006", "relocation source section is absent from linked image");
        return false;
    }
    if (relocation.offset > section_it->contents.size() ||
        relocation.width > section_it->contents.size() - std::min(relocation.offset, section_it->contents.size())) {
        add_diag(diagnostics, "XLINK0007", "relocation field is not backed by file contents");
        return false;
    }
    if (section_it->address > std::numeric_limits<std::uint64_t>::max() - relocation.offset) {
        add_diag(diagnostics, "XLINK0008", "relocation place address overflows");
        return false;
    }

    AppliedRelocation applied;
    applied.source_object_id = object.object_id;
    applied.source_section_id = relocation.section_id;
    applied.offset = relocation.offset;
    applied.kind = relocation.kind;
    applied.target_symbol = relocation.target_symbol;
    applied.place_address = section_it->address + static_cast<std::uint64_t>(relocation.offset);
    applied.addend = relocation.addend;
    applied.width = relocation.width;

    if (!resolve_symbol_address(object, relocation, image, plan, applied.symbol_address,
                                applied.weak_zero_fallback, diagnostics)) return false;

    if (relocation.kind == ObjectRelocationKind::Absolute64 ||
        relocation.kind == ObjectRelocationKind::CoffAddress64) {
        std::uint64_t value = 0U;
        if (!checked_add_signed(applied.symbol_address, relocation.addend, value)) {
            add_diag(diagnostics, "XLINK0009", "absolute 64-bit relocation overflows or underflows");
            return false;
        }
        applied.unsigned_result = value;
        write_u64(section_it->contents, relocation.offset, value);
    } else if (relocation.kind == ObjectRelocationKind::CoffAddr32NB) {
        if (applied.symbol_address < image.image_base) {
            add_diag(diagnostics, "XLINK0015", "ADDR32NB target lies below the linked image base");
            return false;
        }
        const std::uint64_t target_rva = applied.symbol_address - image.image_base;
        std::uint64_t value = 0U;
        if (!checked_add_signed(target_rva, relocation.addend, value) ||
            value > std::numeric_limits<std::uint32_t>::max()) {
            add_diag(diagnostics, "XLINK0016", "ADDR32NB RVA overflows 32-bit range");
            return false;
        }
        applied.unsigned_result = value;
        write_u32(section_it->contents, relocation.offset, static_cast<std::uint32_t>(value));
    } else if (relocation.kind == ObjectRelocationKind::PcRelative32 ||
               relocation.kind == ObjectRelocationKind::PltRelative32 ||
               relocation.kind == ObjectRelocationKind::CoffRelative32) {
        if (applied.symbol_address > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            applied.place_address > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            add_diag(diagnostics, "XLINK0010", "relative relocation address exceeds signed arithmetic range");
            return false;
        }
        std::int64_t value = 0;
        if (!checked_add_i64(static_cast<std::int64_t>(applied.symbol_address), relocation.addend, value)) {
            add_diag(diagnostics, "XLINK0011", "relative relocation addition overflows");
            return false;
        }
        const std::int64_t place = static_cast<std::int64_t>(applied.place_address);
        const std::int64_t bias = relocation.kind == ObjectRelocationKind::CoffRelative32 ? 4 : 0;
        std::int64_t place_with_bias = 0;
        if (!checked_add_i64(place, bias, place_with_bias) || !checked_sub_i64(value, place_with_bias, value)) {
            add_diag(diagnostics, "XLINK0012", "relative relocation subtraction overflows");
            return false;
        }
        if (value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max()) {
            add_diag(diagnostics, "XLINK0013", "relative relocation result is outside signed 32-bit range");
            return false;
        }
        applied.signed_result = value;
        write_u32(section_it->contents, relocation.offset, static_cast<std::uint32_t>(static_cast<std::int32_t>(value)));
    } else {
        add_diag(diagnostics, "XLINK0014", "relocation kind is not implemented by N24 linker");
        return false;
    }
    image.applied_relocations.push_back(std::move(applied));
    return true;
}

bool is_automatic_linker_root(const ObjectSection& section) {
    if (section.associative_to_section_id != InvalidObjectSectionId) return section.linker_root;
    if (section.linker_root) return true;
    const std::string& name = section.name;
    const char* roots[] = {".init_array", ".fini_array", ".ctors", ".dtors", ".CRT$XCU",
        ".CRT$XCT", ".CRT$XLB", ".CRT$XLC", ".tls$", ".xinit", ".xdtor",
        ".pdata", ".xdata", ".eh_frame", ".eh_frame_hdr"};
    for (const char* prefix : roots) {
        const std::size_t length = std::char_traits<char>::length(prefix);
        if (name.compare(0U, length, prefix) == 0) return true;
    }
    return false;
}

bool is_automatic_linker_root(const ObjectFile& object, const ObjectSection& section) {
    if (is_automatic_linker_root(section)) return true;
    if (section.kind != ObjectSectionKind::Bss) return false;
    // Legacy/hand-authored BSS sections without a symbol map are opaque storage;
    // retain them conservatively. Symbolized data sections remain collectible.
    return std::none_of(object.symbols.begin(), object.symbols.end(),
        [&section](const ObjectSymbol& symbol) {
            return symbol.defined && symbol.section_id == section.id;
        });
}

struct SectionLiveness {
    std::set<SectionKey> retained;
    bool applied = false;
};

SectionLiveness compute_section_liveness(
    const std::vector<const ObjectFile*>& objects,
    const ObjectLinkPlanResult& plan,
    const LinkerOptions& options) {
    SectionLiveness result;
    if (options.entry_symbol.empty() && options.preserved_symbols.empty()) {
        for (const auto* object : objects)
            for (const auto& section : object->sections)
                result.retained.insert(SectionKey{object->object_id, section.id});
        return result;
    }
    std::map<std::string, const ObjectFile*> by_id;
    for (const auto* object : objects) by_id.emplace(object->object_id, object);

    std::vector<SectionKey> worklist;
    const auto mark = [&result, &worklist](const ObjectFile& object, ObjectSectionId section_id) {
        const SectionKey key{object.object_id, section_id};
        if (result.retained.insert(key).second) worklist.push_back(key);
    };

    for (const auto* object : objects) {
        for (const auto& section : object->sections) {
            if (is_automatic_linker_root(*object, section)) mark(*object, section.id);
        }
        for (const auto& symbol : object->symbols) {
            if (!symbol.defined) continue;
            const bool entry_root = !options.entry_symbol.empty() && symbol.name == options.entry_symbol;
            const bool preserved_root = std::find(options.preserved_symbols.begin(), options.preserved_symbols.end(),
                symbol.name) != options.preserved_symbols.end();
            if (entry_root || preserved_root) mark(*object, symbol.section_id);
        }
    }

    // Without a reliable root set, retain everything. This is intentionally
    // conservative for object-only links, tests, and partially specified DLLs.
    if (worklist.empty()) {
        for (const auto* object : objects)
            for (const auto& section : object->sections) mark(*object, section.id);
        return result;
    }

    result.applied = true;
    while (!worklist.empty()) {
        const SectionKey current = worklist.back();
        worklist.pop_back();
        const auto source_it = by_id.find(current.first);
        if (source_it == by_id.end()) continue;
        const ObjectFile& source = *source_it->second;

        for (const auto& associated : source.sections) {
            if (associated.associative_to_section_id == current.second)
                mark(source, associated.id);
        }

        for (const auto& relocation : source.relocations) {
            if (relocation.section_id != current.second) continue;

            const ObjectSymbol* local = find_local_symbol(source, relocation.target_symbol);
            if (local != nullptr) {
                mark(source, local->section_id);
                continue;
            }
            const auto* resolution = find_resolution(plan, relocation.target_symbol);
            if (resolution == nullptr) continue; // unresolved weak references have no target.
            const auto target_it = by_id.find(resolution->object_id);
            if (target_it == by_id.end()) continue;
            mark(*target_it->second, static_cast<ObjectSectionId>(resolution->section_id));
        }
    }
    return result;
}

struct ReachableInputs {
    bool active = false;
    std::vector<ObjectFile> objects;
    std::vector<std::string> extracted_archive_members;
    std::map<std::string, std::pair<std::string, std::string>> archive_owners;
};

ReachableInputs prepare_reachable_inputs(
    const std::vector<ObjectFile>& input_objects,
    const std::vector<StaticArchive>& archives,
    const LinkerOptions& options) {
    struct Candidate {
        const ObjectFile* object = nullptr;
        bool from_archive = false;
        std::string archive_id;
        std::string member_id;
    };
    ReachableInputs result;
    // A section attribute alone must not trigger global elimination. At least
    // one explicit program root (entry point or preserved export) is required.
    if (options.entry_symbol.empty() && options.preserved_symbols.empty()) return result;
    std::vector<Candidate> candidates;
    candidates.reserve(input_objects.size());
    for (const auto& object : input_objects) candidates.push_back(Candidate{&object, false, {}, {}});
    if (options.extract_archive_members) {
        for (const auto& archive : archives)
            for (const auto& member : archive.members)
                candidates.push_back(Candidate{&member.object_file, true, archive.archive_id, member.member_id});
    }

    std::map<std::string, std::size_t> by_id;
    for (std::size_t i = 0U; i < candidates.size(); ++i) {
        if (!by_id.emplace(candidates[i].object->object_id, i).second) {
            // Do not guess in the presence of ambiguous object identities.
            return result;
        }
    }

    std::set<SectionKey> live;
    std::deque<SectionKey> worklist;
    std::set<std::size_t> selected_archives;
    const auto mark = [&](std::size_t candidate_index, ObjectSectionId section_id) {
        const auto& object = *candidates[candidate_index].object;
        const SectionKey key{object.object_id, section_id};
        if (live.insert(key).second) {
            worklist.push_back(key);
            if (candidates[candidate_index].from_archive) selected_archives.insert(candidate_index);
        }
    };

    for (std::size_t i = 0U; i < input_objects.size(); ++i) {
        const auto& object = *candidates[i].object;
        for (const auto& section : object.sections)
            if (is_automatic_linker_root(object, section)) mark(i, section.id);
    }

    const auto root_symbol = [&](const std::string& name) {
        if (name.empty()) return;
        std::vector<std::size_t> direct_matches;
        std::vector<std::size_t> archive_matches;
        for (std::size_t i = 0U; i < candidates.size(); ++i) {
            const auto& candidate = candidates[i];
            for (const auto& symbol : candidate.object->symbols) {
                if (!symbol.defined || symbol.name != name || symbol.binding == ObjectSymbolBinding::Local) continue;
                (candidate.from_archive ? archive_matches : direct_matches).push_back(i);
                break;
            }
        }
        if (!direct_matches.empty()) {
            for (const auto index : direct_matches) {
                const auto& object = *candidates[index].object;
                const auto found = std::find_if(object.symbols.begin(), object.symbols.end(),
                    [&name](const ObjectSymbol& symbol) {
                        return symbol.defined && symbol.name == name && symbol.binding != ObjectSymbolBinding::Local;
                    });
                if (found != object.symbols.end()) mark(index, found->section_id);
            }
        } else if (!archive_matches.empty()) {
            auto chosen = archive_matches.front();
            for (const auto index : archive_matches) {
                const auto& object = *candidates[index].object;
                const auto found = std::find_if(object.symbols.begin(), object.symbols.end(),
                    [&name](const ObjectSymbol& symbol) {
                        return symbol.defined && symbol.name == name && symbol.binding == ObjectSymbolBinding::Global;
                    });
                if (found != object.symbols.end()) { chosen = index; break; }
            }
            const auto& object = *candidates[chosen].object;
            const auto found = std::find_if(object.symbols.begin(), object.symbols.end(),
                [&name](const ObjectSymbol& symbol) {
                    return symbol.defined && symbol.name == name && symbol.binding != ObjectSymbolBinding::Local;
                });
            if (found != object.symbols.end()) mark(chosen, found->section_id);
        }
    };
    root_symbol(options.entry_symbol);
    for (const auto& name : options.preserved_symbols) root_symbol(name);

    if (worklist.empty()) return result; // Conservative legacy path when no roots are known.
    result.active = true;

    while (!worklist.empty()) {
        const SectionKey current = worklist.front();
        worklist.pop_front();
        const auto owner = by_id.find(current.first);
        if (owner == by_id.end()) continue;
        const std::size_t source_index = owner->second;
        const ObjectFile& source = *candidates[source_index].object;

        if (candidates[source_index].from_archive) {
            // Archive members are selected only after a live symbol reference or
            // explicit root reaches them; preserve their registration/debug data.
            for (const auto& side_effect : source.sections)
                if (is_automatic_linker_root(source, side_effect)) mark(source_index, side_effect.id);
        }

        for (const auto& relocation : source.relocations) {
            if (relocation.section_id != current.second) continue;
            const ObjectSymbol* local = find_local_symbol(source, relocation.target_symbol);
            if (local != nullptr) {
                mark(source_index, local->section_id);
                continue;
            }

            std::vector<std::size_t> direct_matches;
            std::vector<std::size_t> archive_matches;
            for (std::size_t i = 0U; i < candidates.size(); ++i) {
                for (const auto& symbol : candidates[i].object->symbols) {
                    if (!symbol.defined || symbol.name != relocation.target_symbol ||
                        symbol.binding == ObjectSymbolBinding::Local) continue;
                    (candidates[i].from_archive ? archive_matches : direct_matches).push_back(i);
                    break;
                }
            }
            if (!direct_matches.empty()) {
                for (const auto index : direct_matches) {
                    const auto& target_object = *candidates[index].object;
                    const auto symbol = std::find_if(target_object.symbols.begin(), target_object.symbols.end(),
                        [&relocation](const ObjectSymbol& item) {
                            return item.defined && item.name == relocation.target_symbol &&
                                   item.binding != ObjectSymbolBinding::Local;
                        });
                    if (symbol != target_object.symbols.end()) mark(index, symbol->section_id);
                }
            } else if (!archive_matches.empty()) {
                auto chosen = archive_matches.front();
                for (const auto index : archive_matches) {
                    const auto& target_object = *candidates[index].object;
                    const auto strong = std::find_if(target_object.symbols.begin(), target_object.symbols.end(),
                        [&relocation](const ObjectSymbol& item) {
                            return item.defined && item.name == relocation.target_symbol &&
                                   item.binding == ObjectSymbolBinding::Global;
                        });
                    if (strong != target_object.symbols.end()) { chosen = index; break; }
                }
                const auto& target_object = *candidates[chosen].object;
                const auto symbol = std::find_if(target_object.symbols.begin(), target_object.symbols.end(),
                    [&relocation](const ObjectSymbol& item) {
                        return item.defined && item.name == relocation.target_symbol &&
                               item.binding != ObjectSymbolBinding::Local;
                    });
                if (symbol != target_object.symbols.end()) mark(chosen, symbol->section_id);
            }
        }
    }

    const auto filter_object = [&live](const ObjectFile& source) {
        ObjectFile filtered = source;
        filtered.sections.erase(std::remove_if(filtered.sections.begin(), filtered.sections.end(),
            [&live, &source](const ObjectSection& section) {
                return live.find(SectionKey{source.object_id, section.id}) == live.end();
            }), filtered.sections.end());
        filtered.relocations.erase(std::remove_if(filtered.relocations.begin(), filtered.relocations.end(),
            [&live, &source](const ObjectRelocation& relocation) {
                return live.find(SectionKey{source.object_id, relocation.section_id}) == live.end();
            }), filtered.relocations.end());
        std::set<std::string> referenced_undefined;
        for (const auto& relocation : filtered.relocations) referenced_undefined.insert(relocation.target_symbol);
        filtered.symbols.erase(std::remove_if(filtered.symbols.begin(), filtered.symbols.end(),
            [&live, &referenced_undefined, &source](const ObjectSymbol& symbol) {
                if (symbol.defined)
                    return live.find(SectionKey{source.object_id, symbol.section_id}) == live.end();
                return referenced_undefined.find(symbol.name) == referenced_undefined.end();
            }), filtered.symbols.end());
        return filtered;
    };

    for (std::size_t i = 0U; i < input_objects.size(); ++i) {
        const auto filtered = filter_object(input_objects[i]);
        if (!filtered.sections.empty()) result.objects.push_back(filtered);
    }
    for (const auto index : selected_archives) {
        const auto& candidate = candidates[index];
        const auto filtered = filter_object(*candidate.object);
        if (!filtered.sections.empty()) {
            result.objects.push_back(filtered);
            result.extracted_archive_members.push_back(candidate.archive_id + "::" + candidate.member_id);
            result.archive_owners.emplace(candidate.object->object_id,
                std::make_pair(candidate.archive_id, candidate.member_id));
        }
    }
    return result;
}

} // namespace

LinkerResult link_object_files(const ObjectFileDescriptor& target,
    const std::vector<ObjectFile>& input_objects, const std::vector<StaticArchive>& archives,
    LinkerOptions options) {
    LinkerResult result;
    result.image.descriptor = target;
    result.image.image_base = options.image_base;

    if (!((target.format == ObjectFileFormat::ELF || target.format == ObjectFileFormat::COFF) &&
          target.architecture == ObjectArchitecture::X86_64)) {
        add_diag(result.diagnostics, "XLINK0099", "N24 executable link core currently supports only ELF64 x86-64 and AMD64 COFF");
        return result;
    }
    if (options.maximum_image_size == 0U) {
        add_diag(result.diagnostics, "XLINK0100", "maximum image size must be greater than zero");
        return result;
    }
    for (const auto& object : input_objects) {
        const auto validation = validate_object_file(object);
        result.diagnostics.insert(result.diagnostics.end(), validation.diagnostics.begin(), validation.diagnostics.end());
        if (object.descriptor.format != target.format || object.descriptor.architecture != target.architecture ||
            object.descriptor.operating_system != target.operating_system || object.descriptor.target_triple != target.target_triple ||
            object.descriptor.pointer_size != target.pointer_size || object.descriptor.byte_order != target.byte_order)
            add_diag(result.diagnostics, "XLINK010A", "input object target does not match link target");
    }
    for (const auto& archive : archives) {
        if (archive.archive_id.empty()) add_diag(result.diagnostics, "XLINK010B", "archive identifier must not be empty");
        std::set<std::string> member_ids;
        for (const auto& member : archive.members) {
            if (member.member_id.empty() || !member_ids.insert(member.member_id).second)
                add_diag(result.diagnostics, "XLINK010C", "archive member identifiers must be non-empty and unique");
            const auto validation = validate_object_file(member.object_file);
            result.diagnostics.insert(result.diagnostics.end(), validation.diagnostics.begin(), validation.diagnostics.end());
            if (member.object_file.descriptor.format != target.format || member.object_file.descriptor.architecture != target.architecture ||
                member.object_file.descriptor.operating_system != target.operating_system ||
                member.object_file.descriptor.target_triple != target.target_triple ||
                member.object_file.descriptor.pointer_size != target.pointer_size ||
                member.object_file.descriptor.byte_order != target.byte_order)
                add_diag(result.diagnostics, "XLINK010D", "archive member target does not match link target");
        }
    }
    if (!result.diagnostics.empty()) return result;
    const ReachableInputs prepared = prepare_reachable_inputs(input_objects, archives, options);
    if (prepared.active) {
        result.link_plan = build_object_link_plan(target, prepared.objects, {},
            ObjectLinkOptions{false, options.allow_unresolved_weak});
        result.link_plan.extracted_archive_members = prepared.extracted_archive_members;
        for (auto& symbol : result.link_plan.resolved_symbols) {
            const auto owner = prepared.archive_owners.find(symbol.object_id);
            if (owner != prepared.archive_owners.end()) {
                symbol.archive_id = owner->second.first;
                symbol.archive_member_id = owner->second.second;
            }
        }
    } else {
        result.link_plan = build_object_link_plan(target, input_objects, archives,
            ObjectLinkOptions{options.extract_archive_members, options.allow_unresolved_weak});
    }
    if (!result.link_plan.succeeded()) {
        result.diagnostics = result.link_plan.diagnostics;
        return result;
    }

    std::vector<const ObjectFile*> selected_objects;
    selected_objects.reserve(result.link_plan.selected_object_ids.size());
    for (const auto& id : result.link_plan.selected_object_ids) {
        const auto* object = find_object(id, input_objects, archives);
        if (object == nullptr) {
            add_diag(result.diagnostics, "XLINK0101", "selected link object cannot be found");
            return result;
        }
        selected_objects.push_back(object);
    }

    const SectionLiveness liveness = compute_section_liveness(selected_objects, result.link_plan, options);
    result.image.optimization.section_elimination_applied = liveness.applied;
    for (const auto* object : selected_objects) {
        for (const auto& section : object->sections) {
            ++result.image.optimization.input_section_count;
            result.image.optimization.input_section_bytes += section.size;
            if (liveness.retained.find(SectionKey{object->object_id, section.id}) != liveness.retained.end()) {
                ++result.image.optimization.retained_section_count;
                result.image.optimization.retained_section_bytes += section.size;
            }
        }
    }
    result.image.optimization.discarded_section_count = result.image.optimization.input_section_count -
        result.image.optimization.retained_section_count;
    result.image.optimization.discarded_section_bytes = result.image.optimization.input_section_bytes -
        result.image.optimization.retained_section_bytes;

    std::uint64_t cursor = options.image_base;
    for (const auto* object : selected_objects) {
        for (const auto& source_section : object->sections) {
            if (liveness.retained.find(SectionKey{object->object_id, source_section.id}) == liveness.retained.end())
                continue;
            std::uint64_t address = 0U;
            if (!align_up_u64(cursor, source_section.alignment, address)) {
                add_diag(result.diagnostics, "XLINK0102", "section alignment overflows image address space");
                return result;
            }
            if (address < options.image_base || address - options.image_base > options.maximum_image_size ||
                source_section.size > options.maximum_image_size - static_cast<std::size_t>(address - options.image_base)) {
                add_diag(result.diagnostics, "XLINK0103", "linked image exceeds configured maximum size");
                return result;
            }
            if (address > std::numeric_limits<std::uint64_t>::max() - source_section.size) {
                add_diag(result.diagnostics, "XLINK0104", "section end address overflows");
                return result;
            }
            LinkedImageSection linked;
            linked.object_id = object->object_id;
            linked.source_section_id = source_section.id;
            linked.name = source_section.name;
            linked.kind = source_section.kind;
            linked.address = address;
            linked.alignment = source_section.alignment;
            linked.memory_size = source_section.size;
            if (source_section.kind == ObjectSectionKind::Bss) linked.contents.assign(source_section.size, 0U);
            else linked.contents = source_section.contents;
            result.image.sections.push_back(std::move(linked));
            cursor = address + static_cast<std::uint64_t>(source_section.size);
        }
    }

    if (result.image.sections.empty() || cursor == options.image_base) {
        add_diag(result.diagnostics, "XLINK0109", "link inputs contain no allocated sections; empty image cannot be produced");
        return result;
    }
    if (cursor < options.image_base || cursor - options.image_base > options.maximum_image_size) {
        add_diag(result.diagnostics, "XLINK0105", "linked image size is invalid");
        return result;
    }
    result.image.image_size = static_cast<std::size_t>(cursor - options.image_base);
    result.image.flat_bytes.assign(result.image.image_size, 0U);
    for (const auto& section : result.image.sections) {
        const auto offset64 = section.address - options.image_base;
        if (offset64 > result.image.flat_bytes.size() ||
            section.contents.size() > result.image.flat_bytes.size() - static_cast<std::size_t>(offset64)) {
            add_diag(result.diagnostics, "XLINK0106", "linked section lies outside flat image buffer");
            return result;
        }
        std::copy(section.contents.begin(), section.contents.end(),
                  result.image.flat_bytes.begin() + static_cast<std::ptrdiff_t>(offset64));
    }

    for (const auto* object : selected_objects) {
        for (const auto& symbol : object->symbols) {
            if (!symbol.defined || liveness.retained.find(SectionKey{object->object_id, symbol.section_id}) == liveness.retained.end())
                continue;
            const auto* section = find_linked_section(result.image, object->object_id, symbol.section_id);
            if (section == nullptr || section->address > std::numeric_limits<std::uint64_t>::max() - symbol.value) {
                add_diag(result.diagnostics, "XLINK0107", "defined symbol address is invalid");
                continue;
            }
            result.image.symbols.push_back(LinkedImageSymbol{symbol.name, object->object_id,
                symbol.section_id, section->address + static_cast<std::uint64_t>(symbol.value),
                symbol.size, symbol.binding, symbol.kind});
        }
    }
    if (!result.diagnostics.empty()) return result;

    for (const auto* object : selected_objects) {
        for (const auto& relocation : object->relocations) {
            if (liveness.retained.find(SectionKey{object->object_id, relocation.section_id}) == liveness.retained.end())
                continue;
            if (!apply_one_relocation(*object, relocation, result.image, result.link_plan, result.diagnostics))
                return result;
        }
    }

    for (const auto& section : result.image.sections) {
        const auto offset = static_cast<std::size_t>(section.address - options.image_base);
        std::copy(section.contents.begin(), section.contents.end(),
                  result.image.flat_bytes.begin() + static_cast<std::ptrdiff_t>(offset));
    }

    if (!options.entry_symbol.empty()) {
        const auto entry = std::find_if(result.image.symbols.begin(), result.image.symbols.end(),
            [&options](const LinkedImageSymbol& symbol) {
                return symbol.name == options.entry_symbol && symbol.binding != ObjectSymbolBinding::Local &&
                       symbol.kind == ObjectSymbolKind::Function;
            });
        if (entry == result.image.symbols.end()) {
            add_diag(result.diagnostics, "XLINK0108", "requested entry symbol is not a resolved external function");
            return result;
        }
        result.image.entry_address = entry->address;
        result.image.has_entry = true;
    }

    return result;
}

} // namespace xfrontend
