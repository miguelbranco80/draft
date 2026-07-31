// Benchmark-only access to the bootstrap front-end's internal phase boundaries.
//
// The production lexer intentionally exposes only its complete normalized token
// stream, and the production parser normally owns the transition from those
// tokens into a SyntaxTree. The standalone bootstrap benchmark needs the raw
// scanner and semicolon pass separately and must parse an already prepared token
// stream so token copying is outside the timed region. These declarations exist
// only when the benchmark target compiles its private copy of the front-end
// sources. draft_compiler never defines DRAFT_FRONTEND_BENCHMARK and therefore
// contains neither these entry points nor any timing or benchmark branches.
//
// Every operation preserves the production algorithm and representation. Raw
// tokens include Newline and one EndOfFile. Semicolon insertion consumes that
// owner and returns the public stream. Parsing consumes the supplied public
// token owner and constructs the ordinary SyntaxTree. Diagnostics retain their
// normal deterministic order. Relevant specification: core language section 4,
// "Source text and literals".

#pragma once

#ifndef DRAFT_FRONTEND_BENCHMARK
#error "frontend_benchmark.h is available only to the standalone benchmark target"
#endif

#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/syntax_tree.h"
#include "syntax/token.h"

#include <vector>

namespace draft {

// Runs UTF-8 validation and the raw scanner without semicolon insertion. The
// returned vector owns staging tokens and always ends in EndOfFile.
[[nodiscard]] std::vector<Token> benchmark_raw_lex_source(
    const SourceManager &sources, FileId file, DiagnosticSink &diagnostics);

// Applies the production semicolon pass to one immutable raw stream. The input
// owner remains outside the timed call, matching the Draft benchmark seam.
[[nodiscard]] std::vector<Token> benchmark_insert_semicolons(
    const std::vector<Token> &raw_tokens);

// Parses one already normalized, owned token stream. This excludes lexing and
// token copying while retaining ordinary node and child allocation costs.
[[nodiscard]] SyntaxTree benchmark_parse_tokens(
    FileId file, std::vector<Token> tokens, DiagnosticSink &diagnostics);

} // namespace draft
