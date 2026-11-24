// SPDX-FileCopyrightText: 2026 Hector Moradi
// SPDX-License-Identifier: GPL-3.0-or-later

#include "xfrontend/source_manager.hpp"
#include "xfrontend/lexer.hpp"
#include "xfrontend/parser.hpp"
#include "test_report.hpp"

#include <chrono>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require_source(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void source_case(const char* name, const std::function<void()>& body,
                 int& passed, int& failed) {
    if (!xfrontend_test_report::should_run(name)) return;
    const auto start = std::chrono::steady_clock::now();
    try {
        body();
        ++passed;
        std::cout << "PASS " << name << '\n';
        xfrontend_test_report::record(name, true, "", std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& error) {
        ++failed;
        std::cout << "FAIL " << name << ": " << error.what() << '\n';
        xfrontend_test_report::record(name, false, error.what(), std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count());
    }
}

} // namespace

void run_source_manager_tests(int& passed, int& failed) {
    using xfrontend::SourceManager;

    source_case("S001_unique_buffer_ids", [&] {
        SourceManager sources;
        const auto first = sources.add_buffer("one.src", "a;");
        const auto second = sources.add_buffer("two.src", "b;");
        require_source(first != xfrontend::InvalidSourceFileId, "first id must be valid");
        require_source(second > first, "source ids must increase monotonically");
    }, passed, failed);

    source_case("S002_empty_buffer_name_gets_identity", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("", "x;");
        const auto* file = sources.get(id);
        require_source(file != nullptr, "source record expected");
        require_source(file->display_name.find("<memory:") == 0, "memory name expected");
    }, passed, failed);

    source_case("S003_retrieves_display_name_and_contents", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("sample.src", "function A() {}");
        const auto* file = sources.get(id);
        require_source(file != nullptr, "file expected");
        require_source(file->display_name == "sample.src", "display name mismatch");
        require_source(file->contents == "function A() {}", "source contents mismatch");
    }, passed, failed);

    source_case("S004_unknown_id_returns_null", [&] {
        SourceManager sources;
        require_source(sources.get(0) == nullptr, "zero id must be invalid");
        require_source(sources.get(99) == nullptr, "unregistered id must be invalid");
    }, passed, failed);

    source_case("S005_lf_line_count", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("one.src", "a;\nb;");
        require_source(sources.line_count(id) == 2U, "two physical lines expected");
    }, passed, failed);

    source_case("S006_lf_line_mapping", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("lf.src", "a;\nb;");
        const auto position = sources.position_at(id, 3U);
        require_source(position && position->line == 2U && position->column == 1U,
                       "LF must begin a new line");
    }, passed, failed);

    source_case("S007_crlf_line_mapping", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("crlf.src", "a;\r\nb;");
        const auto position = sources.position_at(id, 4U);
        require_source(position && position->line == 2U && position->column == 1U,
                       "CRLF must count as one line break");
    }, passed, failed);

    source_case("S008_standalone_cr_line_mapping", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("cr.src", "a;\rb;");
        const auto position = sources.position_at(id, 3U);
        require_source(position && position->line == 2U && position->column == 1U,
                       "standalone CR must begin a new line");
    }, passed, failed);

    source_case("S009_unicode_line_separator_mapping", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("unicode-line.src", u8"a;\u2028b;\u2029c;");
        require_source(sources.line_count(id) == 3U, "Unicode separators must split lines");
        const std::string contents = sources.get(id)->contents;
        const auto second = contents.find("b;");
        const auto third = contents.find("c;");
        require_source(sources.position_at(id, second)->line == 2U, "U+2028 line mismatch");
        require_source(sources.position_at(id, third)->line == 3U, "U+2029 line mismatch");
    }, passed, failed);

    source_case("S010_unicode_scalar_columns", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("unicode.src", u8"😀x;");
        const auto offset = sources.get(id)->contents.find("x;");
        const auto position = sources.position_at(id, offset);
        require_source(position && position->column == 2U,
                       "four-byte UTF-8 scalar must occupy one column");
        require_source(position->byte_offset == 4U, "byte offset must remain byte-based");
    }, passed, failed);

    source_case("S011_leading_bom_is_not_a_visible_column", [&] {
        SourceManager sources;
        const std::string text = std::string("\xEF\xBB\xBF") + "abc";
        const auto id = sources.add_buffer("bom.src", text);
        const auto position = sources.position_at(id, 3U);
        require_source(position && position->column == 1U, "first character after BOM is column 1");
        require_source(position->byte_offset == 3U, "BOM bytes remain in byte offset");
        require_source(sources.line_text(id, 1U).value() == "abc", "line text must hide leading BOM");
    }, passed, failed);

    source_case("S012_offsets_are_clamped_to_source_length", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("clamp.src", "abc");
        const auto position = sources.position_at(id, 999U);
        require_source(position && position->byte_offset == 3U && position->column == 4U,
                       "offset must clamp to EOF");
    }, passed, failed);

    source_case("S013_line_text_strips_lf", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("line.src", "first\nsecond");
        require_source(sources.line_text(id, 1U).value() == "first", "LF must be excluded");
        require_source(sources.line_text(id, 2U).value() == "second", "second line mismatch");
    }, passed, failed);

    source_case("S014_line_text_strips_crlf", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("line.src", "first\r\nsecond");
        require_source(sources.line_text(id, 1U).value() == "first", "CRLF must be excluded");
    }, passed, failed);

    source_case("S015_line_text_strips_unicode_separator", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("line.src", u8"اول\u2028دوم");
        require_source(sources.line_text(id, 1U).value() == u8"اول", "U+2028 must be excluded");
        require_source(sources.line_text(id, 2U).value() == u8"دوم", "Persian second line mismatch");
    }, passed, failed);

    source_case("S016_trailing_newline_creates_empty_line", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("tail.src", "abc\n");
        require_source(sources.line_count(id) == 2U, "empty final line expected");
        require_source(sources.line_text(id, 2U).value().empty(), "final line must be empty");
    }, passed, failed);

    source_case("S017_invalid_line_queries_are_empty", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("query.src", "x");
        require_source(!sources.line_text(id, 0U), "line zero is invalid");
        require_source(!sources.line_text(id, 2U), "out-of-range line is invalid");
        require_source(!sources.line_text(99U, 1U), "unknown source id is invalid");
        require_source(sources.line_count(99U) == 0U, "unknown source has zero lines");
    }, passed, failed);

    source_case("S018_loads_disk_file_and_indexes_path", [&] {
        const auto path = std::filesystem::temp_directory_path() / "xfrontend_source_manager_test.src";
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            require_source(static_cast<bool>(output), "temporary file could not be created");
            output << "function Disk() {}";
        }
        SourceManager sources;
        const auto loaded = sources.load_file(path);
        require_source(loaded.succeeded(), "disk file should load");
        require_source(sources.get(loaded.source_id)->contents == "function Disk() {}",
                       "disk contents mismatch");
        require_source(sources.find_file(path).value() == loaded.source_id,
                       "canonical file lookup mismatch");
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }, passed, failed);

    source_case("S019_reload_same_path_keeps_source_id", [&] {
        const auto path = std::filesystem::temp_directory_path() / "xfrontend_source_manager_reload.src";
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "first";
        }
        SourceManager sources;
        const auto first = sources.load_file(path);
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "second\nline";
        }
        const auto second = sources.load_file(path);
        require_source(first.succeeded() && second.succeeded(), "both reads should succeed");
        require_source(first.source_id == second.source_id, "stable file id expected on reload");
        require_source(sources.get(first.source_id)->contents == "second\nline",
                       "reloaded content must replace old snapshot");
        require_source(sources.line_count(first.source_id) == 2U, "line index must refresh");
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }, passed, failed);

    source_case("S020_missing_file_returns_diagnostic", [&] {
        SourceManager sources;
        const auto path = std::filesystem::temp_directory_path() / "xfrontend_missing_source_92417.src";
        const auto loaded = sources.load_file(path);
        require_source(!loaded.succeeded(), "missing file must fail");
        require_source(!loaded.diagnostics.empty() && loaded.diagnostics[0].code == "XSRC0001",
                       "source open diagnostic expected");
    }, passed, failed);

    source_case("S021_repeated_path_spelling_resolves_same_id", [&] {
        const auto base = std::filesystem::temp_directory_path();
        const auto path = base / "xfrontend_source_manager_alias.src";
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            output << "x;";
        }
        SourceManager sources;
        const auto first = sources.load_file(path);
        const auto alias = base / "." / "xfrontend_source_manager_alias.src";
        const auto second = sources.load_file(alias);
        require_source(first.succeeded() && second.succeeded(), "both path spellings should load");
        require_source(first.source_id == second.source_id, "canonical path should be idempotent");
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }, passed, failed);

    source_case("S022_lexer_tokens_carry_source_id", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("token-source.src", "function Run() {}");
        const auto lexical = xfrontend::lex_source(sources, id);
        require_source(lexical.succeeded(), "source-backed lexing should succeed");
        require_source(!lexical.tokens.empty(), "tokens expected");
        for (const auto& token : lexical.tokens) {
            require_source(token.span.source_id == id, "every token span must identify its source");
        }
    }, passed, failed);

    source_case("S023_lexer_diagnostics_carry_source_id", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("bad-token.src", "name = \"unfinished");
        const auto lexical = xfrontend::lex_source(sources, id);
        require_source(!lexical.diagnostics.empty(), "lexical error expected");
        require_source(lexical.diagnostics[0].span.source_id == id,
                       "lexer diagnostic must identify source file");
    }, passed, failed);

    source_case("S024_parser_ast_spans_carry_source_id", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("ast-source.src",
            "function Add(int left, int right) { return left + right; }");
        const auto parsed = xfrontend::parse_source(sources, id);
        require_source(parsed.succeeded(), "source-backed parse should succeed");
        require_source(parsed.unit.functions.size() == 1U, "function expected");
        const auto& function = parsed.unit.functions[0];
        require_source(function.span.source_id == id, "function span must identify source");
        require_source(function.parameters[0].span.source_id == id,
                       "parameter span must identify source");
        require_source(function.body->span.source_id == id, "block span must identify source");
        require_source(function.body->statements[0]->expressions[0]->span.source_id == id,
                       "expression span must identify source");
    }, passed, failed);

    source_case("S025_parser_diagnostics_carry_source_id", [&] {
        SourceManager sources;
        const auto id = sources.add_buffer("bad-parse.src", "function Broken( {");
        const auto parsed = xfrontend::parse_source(sources, id);
        require_source(!parsed.diagnostics.empty(), "parse diagnostic expected");
        require_source(parsed.diagnostics[0].span.source_id == id,
                       "parser diagnostic must identify source file");
    }, passed, failed);

    source_case("S026_invalid_source_id_is_reported_by_lexer", [&] {
        SourceManager sources;
        const auto lexical = xfrontend::lex_source(sources, 77U);
        require_source(!lexical.succeeded(), "unknown source must fail");
        require_source(!lexical.diagnostics.empty() && lexical.diagnostics[0].code == "XSRC0004",
                       "invalid source id diagnostic expected");
    }, passed, failed);

    source_case("S027_source_positions_do_not_bleed_between_files", [&] {
        SourceManager sources;
        const auto first = sources.add_buffer("a.src", "a;\nb;");
        const auto second = sources.add_buffer("b.src", "longer;");
        const auto first_position = sources.position_at(first, 3U);
        const auto second_position = sources.position_at(second, 3U);
        require_source(first_position && second_position, "both locations expected");
        require_source(first_position->line == 2U && first_position->column == 1U,
                       "first source line mismatch");
        require_source(second_position->line == 1U && second_position->column == 4U,
                       "second source line mismatch");
    }, passed, failed);
}
