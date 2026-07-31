// Standalone phase benchmark for the C++ bootstrap lexer and parser.
//
// The executable loads one caller-selected valid Draft source before measuring
// anything, then records raw scanning, semicolon insertion, complete lexing,
// token-to-tree parsing, and the combined frontend. Every phase runs repeatedly
// inside one process; one warmup precedes ten samples and the reported value is
// their median. A clock-only phase exposes the cost of the per-iteration timer.
//
// This file and its benchmark-only hooks are compiled into an EXCLUDE_FROM_ALL
// target from a private copy of the frontend sources. It does not link
// draft_compiler and cannot add code, counters, branches, or startup work to
// draftc. Source loading, vector preparation for the parser/semicolon phases,
// result checksums, destruction, sorting, and printing all occur outside timed
// regions. Relevant specification: core language section 4, "Source text and
// literals".

#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/frontend_benchmark.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"
#include "syntax/syntax_tree.h"
#include "syntax/token.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

constexpr std::size_t kSampleCount = 10;
constexpr std::uint64_t kDefaultIterations = 100;
constexpr std::uint64_t kMaximumIterations = 1'000'000;
constexpr std::uint64_t kChecksumSeed = 1'469'598'103'934'665'603ULL;
constexpr std::uint64_t kChecksumMultiplier = 1'099'511'628'211ULL;

enum class Phase {
  Clock,
  RawLexer,
  SemicolonInsertion,
  Lexer,
  Parser,
  Frontend,
};

// One Sample is the complete result of one batched phase invocation. elapsed_ns
// includes only production phase calls bracketed by the two clock reads. The
// remaining fields are computed afterward and make every result observable.
struct Sample {
  std::uint64_t elapsed_ns = 0;
  std::uint64_t checksum = 0;
  std::size_t tokens = 0;
  std::size_t nodes = 0;
  bool ok = true;
};

// Workload owns the immutable source and the two precomputed token forms used
// by isolated phases. SourceManager outlives every token range. raw_tokens and
// tokens are never mutated; individual samples copy them before transferring
// ownership to a production operation.
struct Workload {
  draft::SourceManager sources;
  draft::FileId file;
  std::vector<draft::Token> raw_tokens;
  std::vector<draft::Token> tokens;
  std::size_t expected_nodes = 0;
};

[[nodiscard]] std::uint64_t now_nanoseconds() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

[[nodiscard]] std::uint64_t mix_checksum(
    std::uint64_t checksum, std::uint64_t value) {
  return (checksum ^ value) * kChecksumMultiplier;
}

// token_checksum observes both ends of the append-only result plus its length.
// This is constant work outside the timer: a full dump/hash would perturb cache
// and thermal state between samples more than it would strengthen the benchmark.
[[nodiscard]] std::uint64_t token_checksum(
    const std::vector<draft::Token> &tokens) {
  std::uint64_t checksum = mix_checksum(kChecksumSeed, tokens.size());
  if (tokens.empty()) return checksum;

  const auto add_token = [&checksum](const draft::Token &token) {
    checksum = mix_checksum(
        checksum, static_cast<std::uint64_t>(token.kind));
    checksum = mix_checksum(checksum, token.range.begin.offset);
    checksum = mix_checksum(checksum, token.range.end.offset);
    checksum = mix_checksum(checksum, token.inserted ? 1U : 0U);
  };
  add_token(tokens.front());
  add_token(tokens.back());
  return checksum;
}

// tree_checksum adds the root identity and shape to the already normalized
// token summary. Node IDs and NodeKind ordinals intentionally mirror the Draft
// frontend, making the benchmark comparison reject a mismatched workload.
[[nodiscard]] std::uint64_t tree_checksum(const draft::SyntaxTree &tree) {
  std::uint64_t checksum = token_checksum(tree.tokens());
  checksum = mix_checksum(checksum, tree.nodes().size());
  checksum = mix_checksum(checksum, tree.root().value);
  const draft::SyntaxNode &root = tree.node(tree.root());
  checksum = mix_checksum(checksum, static_cast<std::uint64_t>(root.kind));
  checksum = mix_checksum(checksum, root.token_begin);
  checksum = mix_checksum(checksum, root.token_end);
  checksum = mix_checksum(checksum, root.children.size());
  return checksum;
}

[[nodiscard]] Sample measure_clock(std::uint64_t iterations) {
  Sample sample;
  sample.checksum = kChecksumSeed;
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    const std::uint64_t begin = now_nanoseconds();
    const std::uint64_t end = now_nanoseconds();
    sample.elapsed_ns += end - begin;
    sample.checksum = mix_checksum(sample.checksum, iteration);
  }
  return sample;
}

[[nodiscard]] Sample measure_raw_lexer(
    const Workload &workload, std::uint64_t iterations) {
  Sample sample;
  sample.checksum = kChecksumSeed;
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    draft::DiagnosticSink diagnostics;
    const std::uint64_t begin = now_nanoseconds();
    std::vector<draft::Token> tokens = draft::benchmark_raw_lex_source(
        workload.sources, workload.file, diagnostics);
    const std::uint64_t end = now_nanoseconds();
    sample.elapsed_ns += end - begin;
    sample.ok = sample.ok && !diagnostics.has_errors();
    sample.tokens = tokens.size();
    sample.checksum = mix_checksum(sample.checksum, token_checksum(tokens));
  }
  return sample;
}

[[nodiscard]] Sample measure_semicolon_insertion(
    const Workload &workload, std::uint64_t iterations) {
  Sample sample;
  sample.checksum = kChecksumSeed;
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    const std::uint64_t begin = now_nanoseconds();
    std::vector<draft::Token> tokens =
        draft::benchmark_insert_semicolons(workload.raw_tokens);
    const std::uint64_t end = now_nanoseconds();
    sample.elapsed_ns += end - begin;
    sample.tokens = tokens.size();
    sample.checksum = mix_checksum(sample.checksum, token_checksum(tokens));
  }
  return sample;
}

[[nodiscard]] Sample measure_lexer(
    const Workload &workload, std::uint64_t iterations) {
  Sample sample;
  sample.checksum = kChecksumSeed;
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    draft::DiagnosticSink diagnostics;
    const std::uint64_t begin = now_nanoseconds();
    std::vector<draft::Token> tokens = draft::lex_source(
        workload.sources, workload.file, diagnostics);
    const std::uint64_t end = now_nanoseconds();
    sample.elapsed_ns += end - begin;
    sample.ok = sample.ok && !diagnostics.has_errors();
    sample.tokens = tokens.size();
    sample.checksum = mix_checksum(sample.checksum, token_checksum(tokens));
  }
  return sample;
}

[[nodiscard]] Sample measure_parser(
    const Workload &workload, std::uint64_t iterations) {
  Sample sample;
  sample.checksum = kChecksumSeed;
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    std::vector<draft::Token> prepared = workload.tokens;
    draft::DiagnosticSink diagnostics;
    const std::uint64_t begin = now_nanoseconds();
    draft::SyntaxTree tree = draft::benchmark_parse_tokens(
        workload.file, std::move(prepared), diagnostics);
    const std::uint64_t end = now_nanoseconds();
    sample.elapsed_ns += end - begin;
    sample.ok = sample.ok && !diagnostics.has_errors();
    sample.tokens = tree.tokens().size();
    sample.nodes = tree.nodes().size();
    sample.checksum = mix_checksum(sample.checksum, tree_checksum(tree));
  }
  return sample;
}

[[nodiscard]] Sample measure_frontend(
    const Workload &workload, std::uint64_t iterations) {
  Sample sample;
  sample.checksum = kChecksumSeed;
  for (std::uint64_t iteration = 0; iteration < iterations; ++iteration) {
    draft::DiagnosticSink diagnostics;
    const std::uint64_t begin = now_nanoseconds();
    draft::SyntaxTree tree = draft::parse_source_file(
        workload.sources, workload.file, diagnostics);
    const std::uint64_t end = now_nanoseconds();
    sample.elapsed_ns += end - begin;
    sample.ok = sample.ok && !diagnostics.has_errors();
    sample.tokens = tree.tokens().size();
    sample.nodes = tree.nodes().size();
    sample.checksum = mix_checksum(sample.checksum, tree_checksum(tree));
  }
  return sample;
}

[[nodiscard]] Sample measure_phase(
    Phase phase, const Workload &workload, std::uint64_t iterations) {
  switch (phase) {
  case Phase::Clock: return measure_clock(iterations);
  case Phase::RawLexer: return measure_raw_lexer(workload, iterations);
  case Phase::SemicolonInsertion:
    return measure_semicolon_insertion(workload, iterations);
  case Phase::Lexer: return measure_lexer(workload, iterations);
  case Phase::Parser: return measure_parser(workload, iterations);
  case Phase::Frontend: return measure_frontend(workload, iterations);
  }
  return {.ok = false};
}

[[nodiscard]] std::string_view phase_name(Phase phase) {
  switch (phase) {
  case Phase::Clock: return "clock";
  case Phase::RawLexer: return "raw_lexer";
  case Phase::SemicolonInsertion: return "semicolon_insertion";
  case Phase::Lexer: return "lexer";
  case Phase::Parser: return "parser";
  case Phase::Frontend: return "frontend";
  }
  return "unknown";
}

// run_phase discards one complete warmup, then verifies that all ten samples
// produced the same result shape and checksum before reporting their median.
// A mismatch is an infrastructure failure rather than noisy performance data.
[[nodiscard]] bool run_phase(
    Phase phase,
    const Workload &workload,
    std::uint64_t iterations,
    Sample &median) {
  const Sample warmup = measure_phase(phase, workload, iterations);
  if (!warmup.ok) return false;

  std::array<std::uint64_t, kSampleCount> elapsed{};
  Sample reference;
  for (std::size_t index = 0; index < kSampleCount; ++index) {
    const Sample sample = measure_phase(phase, workload, iterations);
    if (!sample.ok) return false;
    if (index == 0) {
      reference = sample;
    } else if (
        sample.checksum != reference.checksum ||
        sample.tokens != reference.tokens ||
        sample.nodes != reference.nodes) {
      return false;
    }
    elapsed[index] = sample.elapsed_ns;
  }
  std::sort(elapsed.begin(), elapsed.end());
  reference.elapsed_ns =
      elapsed[kSampleCount / 2 - 1] +
      (elapsed[kSampleCount / 2] - elapsed[kSampleCount / 2 - 1]) / 2;
  median = reference;
  return true;
}

[[nodiscard]] std::uint64_t scaled_cost(
    std::uint64_t elapsed_ns,
    std::uint64_t units,
    std::uint64_t iterations) {
  if (units == 0 || iterations == 0) return 0;
  if (units > std::numeric_limits<std::uint64_t>::max() / iterations)
    return 0;
  const std::uint64_t total_units = units * iterations;
  if (elapsed_ns > std::numeric_limits<std::uint64_t>::max() / 1'000U)
    return 0;
  return elapsed_ns * 1'000U / total_units;
}

void print_row(
    Phase phase,
    const Workload &workload,
    std::uint64_t iterations,
    const Sample &sample) {
  const std::uint64_t bytes = phase == Phase::Clock
      ? 0
      : static_cast<std::uint64_t>(
            workload.sources.text(workload.file).size());
  const std::uint64_t tokens = sample.tokens;
  std::cout
      << "bootstrap-cpp\t" << phase_name(phase)
      << '\t' << bytes
      << '\t' << tokens
      << '\t' << sample.nodes
      << '\t' << iterations
      << '\t' << sample.elapsed_ns
      << '\t' << scaled_cost(sample.elapsed_ns, bytes, iterations)
      << '\t' << scaled_cost(sample.elapsed_ns, tokens, iterations)
      << '\t' << sample.checksum
      << '\n';
}

[[nodiscard]] bool parse_iterations(
    std::string_view spelling, std::uint64_t &iterations) {
  const char *begin = spelling.data();
  const char *end = begin + spelling.size();
  std::uint64_t parsed = 0;
  const auto result = std::from_chars(begin, end, parsed, 10);
  if (result.ec != std::errc{} || result.ptr != end || parsed == 0 ||
      parsed > kMaximumIterations) {
    return false;
  }
  iterations = parsed;
  return true;
}

[[nodiscard]] bool prepare_workload(
    const std::string &path, Workload &workload) {
  const draft::LoadFileResult loaded = workload.sources.load_file(path);
  if (!loaded.ok) {
    std::cerr << loaded.error << '\n';
    return false;
  }
  workload.file = loaded.file;

  draft::DiagnosticSink raw_diagnostics;
  workload.raw_tokens = draft::benchmark_raw_lex_source(
      workload.sources, workload.file, raw_diagnostics);
  if (raw_diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(workload.sources, raw_diagnostics);
    return false;
  }

  draft::DiagnosticSink lexer_diagnostics;
  workload.tokens = draft::lex_source(
      workload.sources, workload.file, lexer_diagnostics);
  if (lexer_diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(workload.sources, lexer_diagnostics);
    return false;
  }

  std::vector<draft::Token> prepared = workload.tokens;
  draft::DiagnosticSink parser_diagnostics;
  const draft::SyntaxTree tree = draft::benchmark_parse_tokens(
      workload.file, std::move(prepared), parser_diagnostics);
  if (parser_diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(workload.sources, parser_diagnostics);
    return false;
  }
  workload.expected_nodes = tree.nodes().size();
  return true;
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 2 && argc != 3) {
    std::cerr
        << "usage: draft-bootstrap-frontend-benchmark <file.draft> [iterations]\n";
    return 2;
  }

  std::uint64_t iterations = kDefaultIterations;
  if (argc == 3 && !parse_iterations(argv[2], iterations)) {
    std::cerr << "error: iterations must be in [1, 1000000]\n";
    return 2;
  }

  Workload workload;
  if (!prepare_workload(argv[1], workload)) return 1;

  std::cout
      << "implementation\tphase\tbytes\ttokens\tnodes\titerations"
         "\tmedian_ns\tns_per_byte_x1000\tns_per_token_x1000\tchecksum\n";
  constexpr std::array phases{
      Phase::Clock,
      Phase::RawLexer,
      Phase::SemicolonInsertion,
      Phase::Lexer,
      Phase::Parser,
      Phase::Frontend,
  };
  for (const Phase phase : phases) {
    Sample median;
    if (!run_phase(phase, workload, iterations, median)) {
      std::cerr << "error: benchmark phase failed or changed output: "
                << phase_name(phase) << '\n';
      return 1;
    }
    print_row(phase, workload, iterations, median);
  }
  return 0;
}
