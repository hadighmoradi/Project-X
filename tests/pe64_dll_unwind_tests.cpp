#include "xfrontend/pe64_dll.hpp"
#include "test_report.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace xfrontend;
namespace {

void n38_require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void n38_case(const std::string& name, const std::function<void()>& body,
              int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    std::cout << "RUN " << name << '\n' << std::flush;
    const auto start = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        std::cout << "PASS " << name << '\n' << std::flush;
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        std::cout << "FAIL " << name << ": " << error.what() << '\n' << std::flush;
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

std::uint16_t get16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n38_require(offset <= bytes.size() && bytes.size() - offset >= 2U, "u16 read outside PE image");
    return static_cast<std::uint16_t>(bytes[offset]) |
        static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}

std::uint32_t get32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    n38_require(offset <= bytes.size() && bytes.size() - offset >= 4U, "u32 read outside PE image");
    return static_cast<std::uint32_t>(bytes[offset]) |
        (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
        (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}

const Pe64DllSectionInfo& find_section(const Pe64DllImage& image, const char* name) {
    const auto found = std::find_if(image.sections.begin(), image.sections.end(), [name](const auto& item) {
        return item.name == name;
    });
    if (found == image.sections.end()) throw std::runtime_error(std::string("missing section ") + name);
    return *found;
}

std::size_t rva_offset(const Pe64DllImage& image, std::uint32_t rva, std::size_t length) {
    for (const auto& section : image.sections) {
        if (rva < section.virtual_address) continue;
        const std::uint64_t delta = static_cast<std::uint64_t>(rva) - section.virtual_address;
        if (delta <= section.raw_size && length <= static_cast<std::size_t>(section.raw_size - delta))
            return static_cast<std::size_t>(section.raw_offset) + static_cast<std::size_t>(delta);
    }
    throw std::runtime_error("RVA does not map to initialized section bytes");
}

Pe64DllSpec base_spec(std::size_t index) {
    Pe64DllSpec spec;
    spec.dll_name = "n38_unwind_fixture_" + std::to_string(index) + ".dll";
    spec.image_base = 0x180000000ULL + static_cast<std::uint64_t>(index) * 0x10000ULL;
    spec.text.assign(512U, 0x90U);
    spec.exports = {{"N38Entry", 1U, Pe64SectionKind::Text, 0U, false, {}}};
    return spec;
}

Pe64DllRuntimeFunction basic_function(std::size_t begin, std::size_t end, std::uint8_t op_info) {
    Pe64DllRuntimeFunction function;
    function.begin_offset = begin;
    function.end_offset = end;
    function.prolog_size = 4U;
    function.unwind_codes.push_back(Pe64DllUnwindCode{
        4U, Pe64DllUnwindOperation::AllocSmall, op_info, 0U
    });
    return function;
}

void test_basic_unwind_directory(std::size_t index) {
    auto spec = base_spec(index);
    spec.runtime_functions.push_back(basic_function(0U, 64U, static_cast<std::uint8_t>(index % 16U)));
    const auto result = write_pe64_dll_image(spec);
    n38_require(result.succeeded(), "valid basic unwind DLL was rejected" +
        (result.diagnostics.empty() ? std::string{} : ": " + result.diagnostics.front().code + " " + result.diagnostics.front().message));
    const auto& image = result.image;
    const auto& pdata = find_section(image, ".pdata");
    const auto& xdata = find_section(image, ".xdata");
    const auto& text = find_section(image, ".text");
    const std::size_t pe = get32(image.bytes, 0x3cU);
    const std::size_t optional = pe + 24U;
    n38_require(image.exception_directory_rva == pdata.virtual_address &&
                image.exception_directory_size == 12U && image.runtime_function_count == 1U,
                "Exception Directory metadata counters mismatch");
    n38_require(get32(image.bytes, optional + 112U + 3U * 8U) == image.exception_directory_rva &&
                get32(image.bytes, optional + 112U + 3U * 8U + 4U) == 12U,
                "Optional Header Exception Directory entry mismatch");
    n38_require(get32(image.bytes, optional + 108U) == 16U, "PE32+ DataDirectory count must remain 16");

    const std::size_t pdata_offset = rva_offset(image, image.exception_directory_rva, 12U);
    const std::uint32_t begin_rva = get32(image.bytes, pdata_offset);
    const std::uint32_t end_rva = get32(image.bytes, pdata_offset + 4U);
    const std::uint32_t unwind_rva = get32(image.bytes, pdata_offset + 8U);
    n38_require(begin_rva == text.virtual_address && end_rva == text.virtual_address + 64U,
                "RUNTIME_FUNCTION BeginAddress/EndAddress mismatch");
    n38_require(unwind_rva >= xdata.virtual_address && unwind_rva < xdata.virtual_address + xdata.virtual_size,
                "RUNTIME_FUNCTION UnwindData does not point into .xdata");
    n38_require((unwind_rva & 3U) == 0U, "UNWIND_INFO must be 4-byte aligned");
    const std::size_t unwind_offset = rva_offset(image, unwind_rva, 6U);
    n38_require(image.bytes[unwind_offset] == 1U && image.bytes[unwind_offset + 1U] == 4U &&
                image.bytes[unwind_offset + 2U] == 1U && image.bytes[unwind_offset + 3U] == 0U,
                "UNWIND_INFO version/prolog/code-count/frame header mismatch");
    n38_require(image.bytes[unwind_offset + 4U] == 4U &&
                image.bytes[unwind_offset + 5U] == static_cast<std::uint8_t>(((index % 16U) << 4U) | 2U),
                "UWOP_ALLOC_SMALL encoding mismatch");
    n38_require(xdata.virtual_size == image.unwind_info_size && image.unwind_info_size == 8U,
                "single UNWIND_INFO record must be padded to 4 bytes");
}

void test_supported_unwind_operations(std::size_t index) {
    auto spec = base_spec(index);
    Pe64DllRuntimeFunction function;
    function.begin_offset = 0U;
    function.end_offset = 128U;
    function.prolog_size = 64U;
    function.frame_register = 5U;
    function.frame_offset = static_cast<std::uint8_t>(index % 16U);
    function.unwind_codes = {
        {56U, Pe64DllUnwindOperation::PushNonVol, 3U, 0U},
        {48U, Pe64DllUnwindOperation::AllocLarge, 0U, 256U},
        {40U, Pe64DllUnwindOperation::AllocSmall, 4U, 0U},
        {32U, Pe64DllUnwindOperation::SetFramePointer, 0U, 0U},
        {24U, Pe64DllUnwindOperation::SaveNonVol, 12U, 128U},
        {16U, Pe64DllUnwindOperation::SaveNonVolFar, 13U, 70000U},
        {8U, Pe64DllUnwindOperation::SaveXmm128, 6U, 160U},
        {4U, Pe64DllUnwindOperation::SaveXmm128Far, 15U, 80000U}
    };
    spec.runtime_functions.push_back(function);
    const auto result = write_pe64_dll_image(spec);
    n38_require(result.succeeded(), "valid multi-opcode unwind info was rejected" +
        (result.diagnostics.empty() ? std::string{} : ": " + result.diagnostics.front().code + " " + result.diagnostics.front().message));
    const auto& image = result.image;
    const std::size_t pdata_offset = rva_offset(image, image.exception_directory_rva, 12U);
    const std::uint32_t unwind_rva = get32(image.bytes, pdata_offset + 8U);
    const std::size_t unwind = rva_offset(image, unwind_rva, 4U + 30U);
    n38_require(image.bytes[unwind] == 1U && image.bytes[unwind + 1U] == 64U && image.bytes[unwind + 2U] == 15U,
                "full UNWIND_INFO header or code slot count mismatch");
    n38_require(image.bytes[unwind + 3U] == static_cast<std::uint8_t>(5U | ((index % 16U) << 4U)),
                "frame register/frame offset nibble mismatch");
    n38_require(image.bytes[unwind + 4U] == 56U && image.bytes[unwind + 5U] == 0x30U,
                "UWOP_PUSH_NONVOL encoding mismatch");
    n38_require(image.bytes[unwind + 6U] == 48U && image.bytes[unwind + 7U] == 0x01U &&
                get16(image.bytes, unwind + 8U) == 32U,
                "UWOP_ALLOC_LARGE scaled operand mismatch");
    n38_require(image.bytes[unwind + 10U] == 40U && image.bytes[unwind + 11U] == 0x42U,
                "UWOP_ALLOC_SMALL encoding mismatch");
    n38_require(image.bytes[unwind + 12U] == 32U && image.bytes[unwind + 13U] == 0x03U,
                "UWOP_SET_FPREG encoding mismatch");
    n38_require(get16(image.bytes, unwind + 16U) == 16U,
                "UWOP_SAVE_NONVOL scaled offset mismatch");
    n38_require(get16(image.bytes, unwind + 20U) == static_cast<std::uint16_t>(70000U & 0xffffU) &&
                get16(image.bytes, unwind + 22U) == static_cast<std::uint16_t>(70000U >> 16U),
                "UWOP_SAVE_NONVOL_FAR exact offset mismatch");
    n38_require(image.unwind_info_size == 36U, "multi-opcode unwind record aligned size mismatch");
}

void test_runtime_function_table_order_and_determinism(std::size_t index) {
    auto spec = base_spec(index);
    const std::size_t count = 1U + (index % 8U);
    for (std::size_t reverse = count; reverse > 0U; --reverse) {
        const std::size_t item = reverse - 1U;
        spec.runtime_functions.push_back(basic_function(item * 32U, item * 32U + 24U,
            static_cast<std::uint8_t>((index + item) % 16U)));
    }
    const auto first = write_pe64_dll_image(spec);
    const auto second = write_pe64_dll_image(spec);
    n38_require(first.succeeded() && second.succeeded(), "valid sorted runtime-function table was rejected");
    n38_require(first.image.bytes == second.image.bytes, "identical runtime functions must produce byte-identical PE images");
    n38_require(first.image.runtime_function_count == count && first.image.exception_directory_size == count * 12U,
                "runtime function count or exception directory byte size mismatch");
    std::size_t offset = rva_offset(first.image, first.image.exception_directory_rva, count * 12U);
    std::uint32_t previous_begin = 0U;
    for (std::size_t item = 0U; item < count; ++item) {
        const std::uint32_t begin = get32(first.image.bytes, offset + item * 12U);
        const std::uint32_t end = get32(first.image.bytes, offset + item * 12U + 4U);
        const std::uint32_t unwind = get32(first.image.bytes, offset + item * 12U + 8U);
        n38_require(begin < end && (item == 0U || begin >= previous_begin),
                    "RUNTIME_FUNCTION table is not sorted by BeginAddress");
        n38_require((unwind & 3U) == 0U && unwind >= find_section(first.image, ".xdata").virtual_address,
                    "RUNTIME_FUNCTION contains an invalid or unaligned UnwindData RVA");
        previous_begin = begin;
    }
}

void test_invalid_unwind_metadata_is_rejected(std::size_t index) {
    auto spec = base_spec(index);
    auto function = basic_function(0U, 64U, 1U);
    switch (index % 21U) {
    case 0U: function.end_offset = 0U; break;
    case 1U: function.end_offset = spec.text.size() + 1U; break;
    case 2U: function.prolog_size = 65U; break;
    case 3U: function.unwind_codes[0U].code_offset = 0U; break;
    case 4U: function.unwind_codes[0U].code_offset = 5U; break;
    case 5U: function.unwind_codes = {
        {2U, Pe64DllUnwindOperation::AllocSmall, 0U, 0U},
        {5U, Pe64DllUnwindOperation::AllocSmall, 0U, 0U}}; break;
    case 6U: function.unwind_codes[0U].operation = Pe64DllUnwindOperation::PushNonVol;
             function.unwind_codes[0U].operation_info = 1U; function.unwind_codes[0U].operand = 0U; break;
    case 7U: function.unwind_codes[0U].operation = Pe64DllUnwindOperation::AllocLarge;
             function.unwind_codes[0U].operation_info = 0U; function.unwind_codes[0U].operand = 129U; break;
    case 8U: function.unwind_codes[0U].operation = Pe64DllUnwindOperation::SetFramePointer;
             function.unwind_codes[0U].operation_info = 0U; function.unwind_codes[0U].operand = 0U; break;
    case 9U: function.frame_register = 1U; function.frame_offset = 0U; break;
    case 10U: function.frame_register = 0U; function.frame_offset = 1U; break;
    case 11U: function.unwind_codes[0U].operation = static_cast<Pe64DllUnwindOperation>(6U); break;
    case 12U: function.unwind_codes[0U].operation = Pe64DllUnwindOperation::SaveXmm128;
              function.unwind_codes[0U].operation_info = 5U; function.unwind_codes[0U].operand = 16U; break;
    case 13U: function.unwind_codes[0U].operation = Pe64DllUnwindOperation::SaveNonVol;
              function.unwind_codes[0U].operation_info = 3U; function.unwind_codes[0U].operand = 7U; break;
    case 14U: function.unwind_codes.clear(); function.frame_register = 5U; break;
    case 15U: function.unwind_codes.assign(256U, Pe64DllUnwindCode{1U, Pe64DllUnwindOperation::AllocSmall, 0U, 0U}); break;
    case 16U: spec.runtime_functions = {function, basic_function(32U, 80U, 0U)}; break;
    case 17U: spec.runtime_functions = {function}; spec.runtime_functions[0U].begin_offset = 64U; spec.runtime_functions[0U].end_offset = 32U; break;
    case 18U: function.unwind_codes[0U].operation = Pe64DllUnwindOperation::SaveXmm128Far;
              function.unwind_codes[0U].operation_info = 6U; function.unwind_codes[0U].operand = 17U; break;
    case 19U: function.unwind_codes[0U].operation = Pe64DllUnwindOperation::SaveNonVolFar;
              function.unwind_codes[0U].operation_info = 12U; function.unwind_codes[0U].operand = 3U; break;
    case 20U: spec.runtime_functions.resize(65537U); break;
    }
    if (spec.runtime_functions.empty()) spec.runtime_functions.push_back(function);
    const auto result = write_pe64_dll_image(spec);
    n38_require(!result.succeeded() && result.image.bytes.empty() && !result.diagnostics.empty(),
                "invalid UNWIND_INFO or RUNTIME_FUNCTION was not rejected atomically");
    n38_require(result.diagnostics.front().code.rfind("XPE64DLL006", 0U) == 0U,
                "invalid unwind metadata returned an unexpected diagnostic family");
}

} // namespace

void run_pe64_dll_unwind_tests(int& passed, int& failed) {
    for (std::size_t index = 0U; index < 500U; ++index)
        n38_case("N38A" + std::to_string(index + 1U) + "_pe64_exception_directory_basic_unwind", [index] {
            test_basic_unwind_directory(index);
        }, passed, failed);
    for (std::size_t index = 0U; index < 500U; ++index)
        n38_case("N38B" + std::to_string(index + 1U) + "_pe64_unwind_operation_encoding", [index] {
            test_supported_unwind_operations(index);
        }, passed, failed);
    for (std::size_t index = 0U; index < 500U; ++index)
        n38_case("N38C" + std::to_string(index + 1U) + "_pe64_runtime_function_order_determinism", [index] {
            test_runtime_function_table_order_and_determinism(index);
        }, passed, failed);
    for (std::size_t index = 0U; index < 500U; ++index)
        n38_case("N38D" + std::to_string(index + 1U) + "_reject_invalid_pe64_unwind_metadata", [index] {
            test_invalid_unwind_metadata_is_rejected(index);
        }, passed, failed);
}
