#include "xfrontend/pe64_pdb.hpp"
#include "xfrontend/pe64_dll.hpp"
#include "xfrontend/pe64_debug_signing.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xfrontend;
namespace {

void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void test_case(const std::string& id, const std::function<void()>& body, int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(id)) return;
    const auto started = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        xfrontend_test_report::record(id, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    } catch (const std::exception& error) {
        ++failed;
        xfrontend_test_report::record(id, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count());
    }
}

std::uint16_t get_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    require(offset + 2U <= bytes.size(), "16-bit read outside COFF object");
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

void put_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffU);
    bytes[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xffU);
}

void put_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (unsigned int i = 0U; i < 4U; ++i)
        bytes[offset + i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
}

ObjectFile make_object(const std::string& symbol_name = "DebugEntry") {
    ObjectFile object;
    object.object_id = "n45_codeview_fixture";
    object.descriptor = make_coff_x86_64_object_target();
    ObjectSection text;
    text.id = 1U;
    text.name = ".text";
    text.kind = ObjectSectionKind::Text;
    text.alignment = 16U;
    text.size = 6U;
    text.contents = {0xb8U, 42U, 0U, 0U, 0U, 0xc3U};
    object.sections.push_back(text);
    ObjectSymbol function;
    function.name = symbol_name;
    function.signature_key = "fn()->i32";
    function.binding = ObjectSymbolBinding::Global;
    function.kind = ObjectSymbolKind::Function;
    function.defined = true;
    function.section_id = 1U;
    function.value = 0U;
    function.size = 6U;
    object.symbols.push_back(function);
    return object;
}

Pe64CodeViewCoffSpec make_spec(std::size_t index) {
    Pe64CodeViewCoffSpec spec;
    spec.object_name = "n45_fixture_" + std::to_string(index) + ".obj";
    spec.public_symbols.push_back(Pe64CodeViewPublicSymbol{
        "DebugEntry_" + std::to_string(index), 1U, 0U, Pe64CodeViewPublicKind::Function});
    return spec;
}

Pe64DllImage make_pe_image(const std::array<std::uint8_t, 16U>& guid, std::uint32_t age) {
    Pe64DllSpec dll;
    dll.dll_name = "n45_pdb_pair.dll";
    dll.text = {0xc3U, 0x90U, 0x90U, 0x90U};
    dll.exports.push_back(Pe64DllExport{"N45Entry", 1U, Pe64SectionKind::Text, 0U, false, {}});
    const auto base = write_pe64_dll_image(dll);
    require(base.succeeded(), "N45 PE image fixture could not be created");
    Pe64CodeViewSpec codeview;
    codeview.guid = guid;
    codeview.age = age;
    codeview.timestamp = 0x65000001U;
    codeview.pdb_path = "n45_fixture.pdb";
    const auto with_debug = write_pe64_codeview_debug_directory(base.image, codeview);
    require(with_debug.succeeded(), "N45 PE CodeView fixture could not be created");
    return with_debug.image;
}

std::vector<std::uint8_t> make_minimal_msf_pdb(const std::array<std::uint8_t, 16U>& guid,
                                               std::uint32_t age) {
    constexpr std::size_t page_size = 512U;
    constexpr std::size_t page_count = 4U;
    const char magic[] = "Microsoft C/C++ MSF 7.00\r\n\032DS\0\0\0";
    std::vector<std::uint8_t> bytes(page_size * page_count, 0U);
    std::copy(std::begin(magic), std::end(magic) - 1, bytes.begin());
    put_u32(bytes, 32U, static_cast<std::uint32_t>(page_size));
    put_u32(bytes, 36U, 1U); // Free Block Map page
    put_u32(bytes, 40U, static_cast<std::uint32_t>(page_count));
    put_u32(bytes, 44U, 16U); // directory stream bytes
    put_u32(bytes, 48U, 0U);
    put_u32(bytes, 52U, 3U); // block-map page

    // Directory is stored at page 1: two streams, stream 0 absent, stream 1 is Info.
    put_u32(bytes, page_size, 2U);
    put_u32(bytes, page_size + 4U, 0xffffffffU);
    put_u32(bytes, page_size + 8U, 28U);
    put_u32(bytes, page_size + 12U, 2U); // Info stream is at page 2

    // PDB Info stream (stream 1): version, signature, age, GUID.
    put_u32(bytes, page_size * 2U, 20000404U);
    put_u32(bytes, page_size * 2U + 4U, 0x12345678U);
    put_u32(bytes, page_size * 2U + 8U, age);
    std::copy(guid.begin(), guid.end(), bytes.begin() + static_cast<std::ptrdiff_t>(page_size * 2U + 12U));

    // Directory block map at page 3 points to directory page 1.
    put_u32(bytes, page_size * 3U, 1U);
    return bytes;
}

std::size_t debug_section_raw_offset(const std::vector<std::uint8_t>& object_bytes) {
    require(object_bytes.size() >= 60U, "COFF fixture too small");
    const std::uint16_t count = static_cast<std::uint16_t>(object_bytes[2U]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(object_bytes[3U]) << 8U);
    for (std::size_t i = 0U; i < count; ++i) {
        const std::size_t header = 20U + i * 40U;
        require(header + 40U <= object_bytes.size(), "COFF section header truncated");
        const std::string name(object_bytes.begin() + static_cast<std::ptrdiff_t>(header),
            object_bytes.begin() + static_cast<std::ptrdiff_t>(header + 8U));
        if (name.find(".debug$") == 0U) {
            const std::uint32_t offset = static_cast<std::uint32_t>(object_bytes[header + 20U]) |
                (static_cast<std::uint32_t>(object_bytes[header + 21U]) << 8U) |
                (static_cast<std::uint32_t>(object_bytes[header + 22U]) << 16U) |
                (static_cast<std::uint32_t>(object_bytes[header + 23U]) << 24U);
            return offset;
        }
    }
    throw std::runtime_error("COFF .debug$S section header not found");
}

void n45_codeview_coff_roundtrip(std::size_t index) {
    const std::string name = "DebugEntry_" + std::to_string(index);
    const auto built = write_pe64_codeview_coff_object(make_object(name), make_spec(index));
    require(built.succeeded(), "CodeView COFF object writer rejected valid input");
    require(built.public_symbol_count == 1U && built.debug_section_bytes >= 32U,
            "CodeView writer produced an unexpected public-symbol/debug-section count");
    const auto inspected = inspect_pe64_codeview_coff_object(built.bytes);
    require(inspected.valid && inspected.present, "CodeView COFF object inspector rejected emitted data: " + inspected.error);
    require(inspected.object_name == make_spec(index).object_name && inspected.public_symbols.size() == 1U,
            "CodeView COFF object identity/public symbol count mismatch");
    require(inspected.public_symbols[0U].name == name && inspected.public_symbols[0U].section_id == 1U &&
            inspected.public_symbols[0U].offset == 0U &&
            inspected.public_symbols[0U].kind == Pe64CodeViewPublicKind::Function,
            "S_PUB32 public symbol did not round-trip");
}

void n45_invalid_codeview_inputs(std::size_t index) {
    auto object = make_object();
    auto spec = make_spec(index);
    switch (index % 10U) {
    case 0U: spec.object_name.clear(); break;
    case 1U: spec.object_name = "bad\nobj"; break;
    case 2U: spec.public_symbols.clear(); break;
    case 3U: spec.public_symbols[0U].name = "not-in-object"; break;
    case 4U: spec.public_symbols[0U].section_id = 99U; break;
    case 5U: spec.public_symbols[0U].offset = 6U; break;
    case 6U: spec.public_symbols[0U].kind = Pe64CodeViewPublicKind::Data; break;
    case 7U: object.descriptor.operating_system = ObjectOperatingSystem::Linux; break;
    case 8U: {
        ObjectSection debug;
        debug.id = 2U; debug.name = ".debug$S"; debug.kind = ObjectSectionKind::Metadata;
        debug.alignment = 4U; debug.size = 4U; debug.contents = {4U, 0U, 0U, 0U};
        object.sections.push_back(debug);
        break;
    }
    default: spec.object_name.assign(1025U, 'x'); break;
    }
    const auto rejected = write_pe64_codeview_coff_object(object, spec);
    require(!rejected.succeeded() && !rejected.diagnostics.empty(), "invalid CodeView COFF request was not rejected");
}

void n45_codeview_corruption(std::size_t index) {
    const auto built = write_pe64_codeview_coff_object(make_object("DebugEntry_1"), make_spec(1U));
    require(built.succeeded(), "CodeView object fixture generation failed");
    auto corrupted = built.bytes;
    const std::size_t debug_raw = debug_section_raw_offset(corrupted);
    switch (index % 8U) {
    case 0U: corrupted[debug_raw] = 0U; break; // C13 signature
    case 1U: put_u32(corrupted, debug_raw + 8U, 0xffffffffU); break; // subsection length
    case 2U: put_u32(corrupted, debug_raw + 4U, 0xF2U); break; // no symbol subsection
    case 3U: put_u16(corrupted, debug_raw + 12U, 0xffffU); break; // record length
    case 4U: put_u16(corrupted, debug_raw + 14U, 0xffffU); break; // record kind
    case 5U: {
        const std::size_t first_record = debug_raw + 12U;
        const std::size_t second_record = first_record + 2U + get_u16(corrupted, first_record);
        put_u16(corrupted, second_record + 12U, 0U); // S_PUB32 section
        break;
    }
    case 6U: {
        const std::size_t first_record = debug_raw + 12U;
        const std::size_t second_record = first_record + 2U + get_u16(corrupted, first_record);
        const std::size_t name_start = second_record + 14U;
        const std::size_t record_end = second_record + 2U + get_u16(corrupted, second_record);
        const auto zero = std::find(corrupted.begin() + static_cast<std::ptrdiff_t>(name_start),
            corrupted.begin() + static_cast<std::ptrdiff_t>(record_end), 0U);
        require(zero != corrupted.begin() + static_cast<std::ptrdiff_t>(record_end), "S_PUB32 terminator not found in fixture");
        *zero = 0x7fU;
        break;
    } // break the S_PUB32 name terminator
    default: corrupted.resize(debug_raw + 9U); break; // truncated subsection
    }
    const auto inspected = inspect_pe64_codeview_coff_object(corrupted);
    require(!inspected.valid && !inspected.error.empty(), "corrupted CodeView COFF data was accepted");
}

void n45_pdb_msf_identity(std::size_t index) {
    std::array<std::uint8_t, 16U> guid{};
    for (std::size_t i = 0U; i < guid.size(); ++i)
        guid[i] = static_cast<std::uint8_t>((index * 11U + i * 23U + 1U) & 0xffU);
    if (std::all_of(guid.begin(), guid.end(), [](std::uint8_t value) { return value == 0U; })) guid[0U] = 1U;
    const std::uint32_t age = static_cast<std::uint32_t>((index % 500U) + 1U);
    const auto pdb = make_minimal_msf_pdb(guid, age);
    const auto pdb_info = inspect_pe64_msf_pdb(pdb);
    require(pdb_info.valid && pdb_info.age == age && pdb_info.guid == guid &&
            pdb_info.page_size == 512U && pdb_info.page_count == 4U && pdb_info.stream_count == 2U,
            "MSF 7.0 PDB Info stream did not round-trip");
    auto pe = make_pe_image(guid, age);
    const auto pair = verify_pe64_pdb_pair(pe.bytes, pdb);
    require(pair.valid && pair.guid_matches && pair.age_matches,
            "matching PE RSDS and PDB Info identity was not accepted: " + pair.error);

    if ((index % 4U) == 0U) {
        auto mismatch_guid = guid;
        mismatch_guid[5U] ^= 0x80U;
        const auto mismatched_pdb = make_minimal_msf_pdb(mismatch_guid, age);
        const auto mismatch = verify_pe64_pdb_pair(pe.bytes, mismatched_pdb);
        require(!mismatch.valid && !mismatch.guid_matches, "mismatched PDB GUID was accepted");
    } else if ((index % 4U) == 1U) {
        const auto mismatched_pdb = make_minimal_msf_pdb(guid, age + 1U);
        const auto mismatch = verify_pe64_pdb_pair(pe.bytes, mismatched_pdb);
        require(!mismatch.valid && !mismatch.age_matches, "mismatched PDB age was accepted");
    } else if ((index % 4U) == 2U) {
        auto truncated = pdb;
        truncated.resize(truncated.size() - 1U);
        require(!inspect_pe64_msf_pdb(truncated).valid, "truncated MSF/PDB file was accepted");
    } else {
        auto invalid = pdb;
        invalid[0U] ^= 0xffU;
        require(!inspect_pe64_msf_pdb(invalid).valid, "invalid MSF signature was accepted");
    }
}

} // namespace

void run_pe64_n45_tests(int& passed, int& failed) {
    for (std::size_t i = 0U; i < 500U; ++i)
        test_case("N45A" + std::to_string(i + 1U) + "_codeview_c13_coff_public_symbol_roundtrip", [i] { n45_codeview_coff_roundtrip(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i)
        test_case("N45B" + std::to_string(i + 1U) + "_invalid_codeview_coff_debug_input_rejection", [i] { n45_invalid_codeview_inputs(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i)
        test_case("N45C" + std::to_string(i + 1U) + "_codeview_coff_malformed_record_detection", [i] { n45_codeview_corruption(i); }, passed, failed);
    for (std::size_t i = 0U; i < 500U; ++i)
        test_case("N45D" + std::to_string(i + 1U) + "_msf_pdb_guid_age_and_pe_pair_validation", [i] { n45_pdb_msf_identity(i); }, passed, failed);
}
