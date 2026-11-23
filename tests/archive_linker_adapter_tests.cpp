#include "xfrontend/archive.hpp"
#include "xfrontend/binary_object.hpp"
#include "xfrontend/object_file.hpp"
#include "xfrontend/system_linker.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace xfrontend;

void n31_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void n31_case(const std::string& name, const std::function<void()>& body,
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

ObjectFile make_object(ObjectFileFormat format, const std::string& id,
                       const std::string& symbol, std::uint8_t fill) {
    ObjectFile object;
    object.object_id = id;
    object.descriptor = format == ObjectFileFormat::ELF
        ? make_elf_x86_64_object_target() : make_coff_x86_64_object_target();
    ObjectSection section;
    section.id = 1U;
    section.name = ".text";
    section.kind = ObjectSectionKind::Text;
    section.alignment = 16U;
    section.size = 8U;
    section.contents.assign(8U, fill);
    object.sections.push_back(section);
    ObjectSymbol function;
    function.name = symbol;
    function.signature_key = "fn()->i32";
    function.binding = ObjectSymbolBinding::Global;
    function.kind = ObjectSymbolKind::Function;
    function.defined = true;
    function.section_id = 1U;
    function.value = 0U;
    function.size = 8U;
    object.symbols.push_back(function);
    return object;
}

std::vector<ArchiveContainerMember> make_archive_members(std::size_t seed, std::size_t count) {
    std::vector<ArchiveContainerMember> members;
    for (std::size_t i = 0U; i < count; ++i) {
        ArchiveContainerMember member;
        member.name = i == 0U ? "m" + std::to_string(seed) + ".o" :
            "module_" + std::to_string(seed) + "_member_" + std::to_string(i) + "_with_long_name.obj";
        const std::size_t length = 1U + ((seed * 13U + i * 7U) % 97U);
        member.contents.resize(length);
        for (std::size_t j = 0U; j < length; ++j) {
            member.contents[j] = static_cast<std::uint8_t>((seed + i * 19U + j * 3U) & 0xffU);
        }
        member.defined_symbols.push_back("Export_" + std::to_string(seed) + "_" + std::to_string(i));
        if ((i & 1U) == 0U) member.defined_symbols.push_back("Secondary_" + std::to_string(seed) + "_" + std::to_string(i));
        members.push_back(std::move(member));
    }
    return members;
}

} // namespace

void run_archive_linker_adapter_tests(int& passed, int& failed) {
    for (std::size_t i = 0U; i < 500U; ++i) {
        n31_case("N31A" + std::to_string(i + 1U) + "_gnu_ar_container_roundtrip_" + std::to_string(i), [i] {
            const auto members = make_archive_members(i + 1U, 1U + (i % 5U));
            const auto written = write_archive_container(ArchiveFormat::GnuAr, members, true);
            n31_require(written.succeeded(), "GNU ar writer rejected a valid member set");
            n31_require(written.bytes.size() >= 8U && written.bytes[0] == '!', "GNU ar magic missing");
            const auto read = read_archive_container(written.bytes);
            n31_require(read.succeeded(), "GNU ar reader rejected writer output");
            n31_require(read.archive.format == ArchiveFormat::GnuAr, "GNU ar format was not retained");
            n31_require(read.archive.has_symbol_index, "GNU ar symbol index missing");
            n31_require(read.archive.members.size() == members.size(), "GNU ar member count changed");
            n31_require(read.archive.symbol_index.size() >= members.size(), "GNU ar exported symbols missing");
            for (std::size_t j = 0U; j < members.size(); ++j) {
                n31_require(read.archive.members[j].name == members[j].name, "GNU ar long/short member name mismatch");
                n31_require(read.archive.members[j].contents == members[j].contents, "GNU ar payload mismatch");
                n31_require(read.archive.members[j].header_offset > 0U, "GNU ar member offset was not captured");
            }
        }, passed, failed);
    }

    for (std::size_t i = 0U; i < 500U; ++i) {
        n31_case("N31B" + std::to_string(i + 1U) + "_coff_lib_linker_index_" + std::to_string(i), [i] {
            const auto members = make_archive_members(i + 101U, 1U + (i % 6U));
            const auto written = write_archive_container(ArchiveFormat::CoffLib, members, true);
            n31_require(written.succeeded(), "COFF LIB writer rejected a valid member set");
            const auto read = read_archive_container(written.bytes);
            n31_require(read.succeeded(), "COFF LIB reader rejected writer output");
            n31_require(read.archive.format == ArchiveFormat::CoffLib, "COFF LIB format was not detected");
            n31_require(read.archive.has_symbol_index, "COFF two-level linker index missing");
            n31_require(read.archive.members.size() == members.size(), "COFF LIB member count changed");
            std::size_t expected_symbols = 0U;
            for (std::size_t j = 0U; j < members.size(); ++j) {
                expected_symbols += members[j].defined_symbols.size();
                n31_require(read.archive.members[j].header_offset == members[j].header_offset ||
                    read.archive.members[j].header_offset > 0U, "COFF LIB member offset invalid");
                n31_require(read.archive.members[j].name == members[j].name, "COFF LIB long-name resolution mismatch");
                n31_require(read.archive.members[j].contents == members[j].contents, "COFF LIB payload mismatch");
            }
            n31_require(read.archive.symbol_index.size() == expected_symbols, "COFF LIB symbol index entry count mismatch");
            for (const auto& entry : read.archive.symbol_index) {
                n31_require(entry.member_index < members.size(), "COFF LIB symbol index member ordinal out of range");
                const auto& exports = members[entry.member_index].defined_symbols;
                n31_require(std::find(exports.begin(), exports.end(), entry.symbol_name) != exports.end(),
                            "COFF LIB index symbol/member mapping mismatch");
            }
        }, passed, failed);
    }

    for (std::size_t i = 0U; i < 500U; ++i) {
        n31_case("N31C" + std::to_string(i + 1U) + "_object_archive_binary_roundtrip_" + std::to_string(i), [i] {
            const bool coff = (i & 1U) != 0U;
            const auto format = coff ? ArchiveFormat::CoffLib : ArchiveFormat::GnuAr;
            const auto object_format = coff ? ObjectFileFormat::COFF : ObjectFileFormat::ELF;
            std::vector<std::pair<std::string, ObjectFile>> objects;
            const std::size_t count = 1U + (i % 4U);
            for (std::size_t j = 0U; j < count; ++j) {
                objects.emplace_back("n31_member_" + std::to_string(i) + "_" + std::to_string(j) + ".o",
                    make_object(object_format, "n31_obj_" + std::to_string(i) + "_" + std::to_string(j),
                                "n31_export_" + std::to_string(i) + "_" + std::to_string(j),
                                static_cast<std::uint8_t>((i + j) & 0xffU)));
            }
            const auto written = write_object_archive(format, objects, true);
            n31_require(written.succeeded(), "object-aware archive writer failed");
            const auto decoded = read_object_archive(written.bytes, "n31_archive_" + std::to_string(i));
            n31_require(decoded.succeeded(), "object-aware archive reader failed");
            n31_require(decoded.format == format, "object archive target format mismatch");
            n31_require(decoded.archive.members.size() == objects.size(), "object archive member count mismatch");
            for (std::size_t j = 0U; j < objects.size(); ++j) {
                const auto& original = objects[j].second;
                const auto& actual = decoded.archive.members[j].object_file;
                n31_require(decoded.archive.members[j].member_id == objects[j].first, "object archive member name mismatch");
                n31_require(actual.descriptor.format == original.descriptor.format, "object target format changed after archive roundtrip");
                n31_require(actual.sections.size() == original.sections.size(), "object section count mismatch after archive roundtrip");
                n31_require(actual.symbols.size() == original.symbols.size(), "object symbol count mismatch after archive roundtrip");
                n31_require(actual.symbols.front().name == original.symbols.front().name, "object symbol name mismatch after archive roundtrip");
            }
            n31_require(decoded.symbol_index.size() == objects.size(), "object archive symbol index count mismatch");

            ObjectFile root;
            root.object_id = "n31_archive_root_" + std::to_string(i);
            root.descriptor = objects.front().second.descriptor;
            ObjectSymbol reference;
            reference.name = objects.front().second.symbols.front().name;
            reference.signature_key = objects.front().second.symbols.front().signature_key;
            reference.binding = ObjectSymbolBinding::Global;
            reference.kind = ObjectSymbolKind::Function;
            reference.defined = false;
            root.symbols.push_back(reference);
            const auto plan = build_object_link_plan(root.descriptor, {root}, {decoded.archive});
            n31_require(plan.succeeded(), "archive-decoded members failed static-link extraction planning");
            n31_require(plan.extracted_archive_members.size() == 1U, "archive extraction did not select exactly the required member");

            auto corrupt = written.bytes;
            n31_require(corrupt.size() > 67U, "archive test image too small for corruption probe");
            corrupt[66U] = static_cast<std::uint8_t>('X');
            const auto malformed = read_archive_container(corrupt);
            n31_require(!malformed.succeeded(), "archive reader accepted a corrupted header trailer");
        }, passed, failed);
    }

    for (std::size_t i = 0U; i < 500U; ++i) {
        n31_case("N31D" + std::to_string(i + 1U) + "_system_linker_response_plan_" + std::to_string(i), [i] {
            const auto kind = static_cast<SystemLinkerKind>(1U + (i % 3U));
            SystemLinkerRequest request;
            request.kind = kind;
            request.output_path = i % 2U == 0U ? "build output/app image.exe" : "build/output/app image";
            request.entry_symbol = kind == SystemLinkerKind::GnuLd ? "_start" : "XStart";
            request.object_paths = {"obj/module " + std::to_string(i) + ".o"};
            request.archive_paths = {"lib/runtime package" + std::to_string(i) + (kind == SystemLinkerKind::GnuLd ? ".a" : ".lib")};
            request.library_directories = {"lib dir/one", "lib dir/two"};
            request.libraries = {"runtime", "m"};
            request.static_link = (i % 2U) == 0U;
            request.console_subsystem = (i % 4U) != 0U;
            request.disable_default_libraries = (i % 5U) == 0U;
            const auto plan = create_system_linker_plan(request);
            n31_require(plan.succeeded(), "system linker adapter rejected a valid request");
            n31_require(!plan.arguments.empty(), "system linker argument vector is empty");
            n31_require(!plan.response_file_contents.empty(), "system linker response file is empty");
            n31_require(plan.response_file_contents.back() == '\n', "response file does not end at an argument boundary");
            if (kind == SystemLinkerKind::GnuLd) {
                n31_require(plan.executable == "ld.lld", "GNU linker default executable mismatch");
                n31_require((plan.response_file_contents.find("-static") != std::string::npos) == request.static_link,
                            "GNU static-link switch mismatch");
                n31_require(plan.response_file_contents.find("-e") != std::string::npos, "GNU entry switch missing");
            } else {
                n31_require(plan.executable == (kind == SystemLinkerKind::LldLink ? "lld-link" : "link.exe"),
                            "Windows linker default executable mismatch");
                n31_require(plan.response_file_contents.find("/MACHINE:X64") != std::string::npos,
                            "Windows AMD64 machine switch missing");
                n31_require(plan.response_file_contents.find("/OUT:") != std::string::npos,
                            "Windows output switch missing");
                n31_require(plan.response_file_contents.find("/ENTRY:") != std::string::npos,
                            "Windows entry switch missing");
                n31_require(plan.response_file_contents.find("/MT") == std::string::npos,
                            "MSVC runtime compiler flag must not be passed to link.exe");
            }
            n31_require(plan.response_file_contents.find("\"obj/module ") != std::string::npos,
                        "path containing spaces was not quoted in response file");
            auto invalid = request;
            invalid.output_path = "bad\noutput.exe";
            const auto rejected = create_system_linker_plan(invalid);
            n31_require(!rejected.succeeded(), "system linker adapter accepted an output path with a newline");
            auto invalid_library = request;
            invalid_library.libraries = {"..\\escape"};
            n31_require(!create_system_linker_plan(invalid_library).succeeded(),
                        "system linker adapter accepted a library name containing a path separator");
            const std::string trailing_slash = "folder with space\\";
            const auto quoted = quote_system_linker_argument(kind, trailing_slash);
            if (kind == SystemLinkerKind::GnuLd) {
                n31_require(quoted.front() == '"' && quoted.back() == '"', "GNU response quote delimiters missing");
            } else {
                n31_require(quoted.front() == '"' && quoted.back() == '"' &&
                            quoted.size() >= 2U && quoted[quoted.size() - 2U] == '\\',
                            "Windows trailing backslash was not doubled before response quote");
            }
        }, passed, failed);
    }
}
