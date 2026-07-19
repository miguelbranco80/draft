// Canonical resolution-manifest encoding and strict-input rejection tests.

#include "elaborator/resolution.h"

#include "base/sha256.h"
#include "source/diagnostic.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "resolution_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

std::string site(char digit) {
  return "site-" + std::string(64, digit);
}

draft::ResolutionPin make_pin(
    std::string site_identity,
    draft::AgentConstructKind kind,
    std::string_view expansion,
    std::string provider,
    std::string model) {
  draft::ResolutionPin pin;
  pin.site_identity = std::move(site_identity);
  pin.kind = kind;
  pin.input_digest = draft::sha256("typed obligation");
  pin.expansion_digest = draft::sha256(expansion);
  pin.source_map = {
      "workspace", ".", "package.draft", 10, 13,
      static_cast<std::uint64_t>(expansion.size()),
  };
  pin.provider_identity = std::move(provider);
  pin.model_identity = std::move(model);
  pin.configuration_identity = "resolver-config-v1";
  return pin;
}

draft::ExternalInputPin make_external(
    draft::ExternalInputKind kind,
    std::string name,
    std::string_view contents,
    std::string entry) {
  draft::ExternalInputPin input;
  input.kind = kind;
  input.name = std::move(name);
  input.content_digest = draft::sha256(contents);
  input.entry_point = std::move(entry);
  return input;
}

void test_canonical_round_trip(TestState &state) {
  draft::ResolutionManifest manifest;
  manifest.target_identity = "aarch64-apple-macos";
  manifest.resolved_program_digest = draft::sha256("resolved program");
  manifest.external_inputs.push_back(make_external(
      draft::ExternalInputKind::RuntimeAsset,
      "language-data",
      "runtime data tree",
      ""));
  manifest.external_inputs.push_back(make_external(
      draft::ExternalInputKind::Object,
      "sqlite-provider",
      "foreign object",
      ""));
  // Intentionally provide pins out of order. Ordering is part of the on-disk
  // contract, not a requirement imposed on every in-memory producer.
  manifest.pins.push_back(make_pin(
      site('b'),
      draft::AgentConstructKind::SynthesisAssembly,
      "add x0, x0, #1\n",
      "codex\nlocal",
      "model-\"one\""));
  manifest.pins.push_back(make_pin(
      site('a'),
      draft::AgentConstructKind::SynthesisExpression,
      "42",
      "codex",
      "model-\xc3\xb8ne"));

  const std::string encoded = draft::serialize_resolution_manifest(manifest);
  EXPECT(state, encoded.ends_with('\n'));
  EXPECT(state, encoded.find(site('a')) < encoded.find(site('b')));
  EXPECT(state, encoded.find("codex\\nlocal") != std::string::npos);
  EXPECT(state, encoded.find("model-\\\"one\\\"") != std::string::npos);

  draft::DiagnosticSink diagnostics;
  draft::ResolutionManifest parsed;
  EXPECT(state,
      draft::parse_resolution_manifest(encoded, parsed, diagnostics));
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, parsed.format == "draft-resolution-v5");
  EXPECT(state, parsed.target_identity == manifest.target_identity);
  EXPECT(state,
      parsed.resolved_program_digest == manifest.resolved_program_digest);
  EXPECT(state, parsed.pins.size() == 2);
  EXPECT(state, parsed.external_inputs.size() == 2);
  if (parsed.external_inputs.size() == 2) {
    EXPECT(state, parsed.external_inputs[0].kind ==
        draft::ExternalInputKind::Object);
    EXPECT(state, parsed.external_inputs[0].name == "sqlite-provider");
    EXPECT(state, parsed.external_inputs[0].entry_point.empty());
    EXPECT(state, parsed.external_inputs[1].kind ==
        draft::ExternalInputKind::RuntimeAsset);
  }
  if (parsed.pins.size() == 2) {
    EXPECT(state, parsed.pins[0].site_identity == site('a'));
    EXPECT(state,
        parsed.pins[0].kind ==
            draft::AgentConstructKind::SynthesisExpression);
    EXPECT(state, parsed.pins[1].provider_identity == "codex\nlocal");
    EXPECT(state, parsed.pins[1].model_identity == "model-\"one\"");
    EXPECT(state,
        parsed.pins[1].configuration_identity == "resolver-config-v1");
    EXPECT(state, parsed.pins[1].source_map.source_relative_path ==
        "package.draft");
    EXPECT(state, parsed.pins[1].source_map.expansion_bytes ==
        std::string_view("add x0, x0, #1\n").size());
  }
  EXPECT(state, draft::serialize_resolution_manifest(parsed) == encoded);
}

void expect_rejected(TestState &state, std::string_view input) {
  draft::DiagnosticSink diagnostics;
  draft::ResolutionManifest manifest;
  EXPECT(state,
      !draft::parse_resolution_manifest(input, manifest, diagnostics));
  EXPECT(state, diagnostics.error_count() == 1);
}

void test_invalid_inputs(TestState &state) {
  draft::ResolutionManifest manifest;
  manifest.target_identity = "aarch64-apple-macos";
  manifest.resolved_program_digest = draft::sha256("program");
  manifest.pins.push_back(make_pin(
      site('a'),
      draft::AgentConstructKind::SynthesisStatement,
      "return;",
      "codex",
      "model"));
  manifest.external_inputs.push_back(make_external(
      draft::ExternalInputKind::RuntimeAsset,
      "unicode-data",
      "runtime data",
      "tables.bin"));
  std::string encoded = draft::serialize_resolution_manifest(manifest);

  // Digests are fixed-width values. Shortening one must be diagnosed rather
  // than silently producing a different identity.
  std::string short_digest = encoded;
  const std::size_t digest = short_digest.find(manifest.pins[0].input_digest.hex());
  EXPECT(state, digest != std::string::npos);
  if (digest != std::string::npos) short_digest.erase(digest, 1);
  expect_rejected(state, short_digest);

  // A manifest is a map keyed by structural site identity. Duplicate entries
  // would make pin selection ambiguous, even when their values happen to agree.
  manifest.pins.push_back(manifest.pins[0]);
  expect_rejected(state, draft::serialize_resolution_manifest(manifest));

  manifest.pins.pop_back();
  manifest.external_inputs.push_back(manifest.external_inputs.front());
  expect_rejected(state, draft::serialize_resolution_manifest(manifest));
  manifest.external_inputs.pop_back();

  manifest.pins[0].source_map.surface_begin = 20;
  manifest.pins[0].source_map.surface_end = 10;
  expect_rejected(state, draft::serialize_resolution_manifest(manifest));
  manifest.pins[0].source_map.surface_begin = 10;
  manifest.pins[0].source_map.surface_end = 13;

  std::string escaping_entry = encoded;
  const std::size_t entry = escaping_entry.find("tables.bin");
  EXPECT(state, entry != std::string::npos);
  if (entry != std::string::npos) {
    escaping_entry.replace(
        entry, std::string_view("tables.bin").size(), "../tables.bin");
  }
  expect_rejected(state, escaping_entry);

  // The manifest schema deliberately does not accept judgment evidence as a
  // source expansion. Evidence has its own format and validation lifecycle.
  std::string judgment = encoded;
  const std::size_t statement = judgment.find("statement");
  EXPECT(state, statement != std::string::npos);
  if (statement != std::string::npos) {
    judgment.replace(statement, std::string_view("statement").size(), "judgment");
  }
  expect_rejected(state, judgment);

  std::string invalid_site = encoded;
  const std::size_t site_digit = invalid_site.find(site('a'));
  EXPECT(state, site_digit != std::string::npos);
  if (site_digit != std::string::npos) invalid_site[site_digit + 5] = 'z';
  expect_rejected(state, invalid_site);

  // Unknown fields are rejected. This prevents an older compiler from
  // accepting a newer manifest while ignoring semantics it does not know.
  std::string unknown_field = encoded;
  const std::size_t target = unknown_field.find("\"target\"");
  EXPECT(state, target != std::string::npos);
  if (target != std::string::npos) {
    unknown_field.replace(target, std::string_view("\"target\"").size(), "\"trg\"");
  }
  expect_rejected(state, unknown_field);

  std::string escaping_source = encoded;
  const std::size_t source_file = escaping_source.find("package.draft");
  EXPECT(state, source_file != std::string::npos);
  if (source_file != std::string::npos) {
    escaping_source.replace(
        source_file,
        std::string_view("package.draft").size(),
        "../package.draft");
  }
  expect_rejected(state, escaping_source);
}

void test_deterministic_malformed_byte_corpus(TestState &state) {
  draft::ResolutionManifest manifest;
  manifest.target_identity = "aarch64-apple-macos";
  manifest.resolved_program_digest = draft::sha256("mutation-program");
  manifest.external_inputs.push_back(make_external(
      draft::ExternalInputKind::RuntimeAsset,
      "unicode-data",
      "runtime data",
      "tables.bin"));
  manifest.pins.push_back(make_pin(
      site('c'),
      draft::AgentConstructKind::SynthesisExpression,
      "42",
      "codex",
      "model"));
  const std::string encoded = draft::serialize_resolution_manifest(manifest);
  const std::size_t closing = encoded.rfind('}');
  EXPECT(state, closing != std::string::npos);
  if (closing == std::string::npos) return;

  // Every prefix that omits the outer closing brace is incomplete. This walks
  // all parser states, including every string escape, digest, integer, array,
  // and nested object boundary, under both ordinary and sanitizer test runs.
  for (std::size_t length = 0; length <= closing; ++length) {
    expect_rejected(state, std::string_view(encoded).substr(0, length));
  }

  // NUL is illegal both as JSON syntax and as an unescaped string byte. 0xff
  // additionally proves that every possible placement of an invalid UTF-8 lead
  // is rejected instead of being retained in a semantic identity.
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    std::string nul = encoded;
    nul[index] = '\0';
    expect_rejected(state, nul);

    std::string invalid_utf8 = encoded;
    invalid_utf8[index] = static_cast<char>(0xffU);
    expect_rejected(state, invalid_utf8);
  }

  // After a complete object only the four JSON whitespace bytes are legal.
  // Exercise every byte value so signed-char behavior cannot create a platform
  // dependent acceptance path.
  const std::string complete = encoded.substr(0, closing + 1);
  for (std::uint32_t value = 0; value <= 0xffU; ++value) {
    std::string trailing = complete;
    trailing.push_back(static_cast<char>(value));
    draft::DiagnosticSink diagnostics;
    draft::ResolutionManifest parsed;
    const bool accepted =
        draft::parse_resolution_manifest(trailing, parsed, diagnostics);
    const bool whitespace = value == static_cast<std::uint32_t>(' ') ||
        value == static_cast<std::uint32_t>('\n') ||
        value == static_cast<std::uint32_t>('\r') ||
        value == static_cast<std::uint32_t>('\t');
    EXPECT(state, accepted == whitespace);
    EXPECT(state, diagnostics.has_errors() != whitespace);
  }
}

void test_deterministic_structural_mutation_corpus(TestState &state) {
  draft::ResolutionManifest manifest;
  manifest.target_identity = "aarch64-apple-macos";
  manifest.resolved_program_digest = draft::sha256("structure-program");
  manifest.external_inputs.push_back(make_external(
      draft::ExternalInputKind::RuntimeAsset,
      "unicode-data",
      "runtime data",
      "tables.bin"));
  manifest.pins.push_back(make_pin(
      site('d'),
      draft::AgentConstructKind::SynthesisStatement,
      "return",
      "codex",
      "model"));
  const std::string encoded = draft::serialize_resolution_manifest(manifest);

  // Walk only punctuation outside JSON strings. Deleting any canonical
  // delimiter or replacing it with another delimiter must fail either JSON
  // structure or the exact manifest schema. This is intentionally distinct
  // from byte fuzzing: quoted braces, commas, and escapes are not candidates.
  const std::string structural = "{}[]:,";
  bool in_string = false;
  bool escaped = false;
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const char byte = encoded[index];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (byte == '\\') {
        escaped = true;
      } else if (byte == '"') {
        in_string = false;
      }
      continue;
    }
    if (byte == '"') {
      in_string = true;
      continue;
    }
    if (structural.find(byte) == std::string::npos) continue;

    std::string deleted = encoded;
    deleted.erase(index, 1);
    expect_rejected(state, deleted);
    for (char replacement : structural) {
      if (replacement == byte) continue;
      std::string replaced = encoded;
      replaced[index] = replacement;
      expect_rejected(state, replaced);
    }
  }

  // Numeric fields are unsigned decimal byte offsets and lengths. Mutate each
  // complete token, rather than one byte, through signed, overflowing,
  // noncanonical, fractional, boolean, null, and string shapes.
  in_string = false;
  escaped = false;
  for (std::size_t index = 0; index < encoded.size();) {
    const char byte = encoded[index];
    if (in_string) {
      if (escaped) {
        escaped = false;
      } else if (byte == '\\') {
        escaped = true;
      } else if (byte == '"') {
        in_string = false;
      }
      ++index;
      continue;
    }
    if (byte == '"') {
      in_string = true;
      ++index;
      continue;
    }
    if (byte < '0' || byte > '9') {
      ++index;
      continue;
    }
    std::size_t end = index + 1;
    while (end < encoded.size() && encoded[end] >= '0' && encoded[end] <= '9') {
      ++end;
    }
    const std::string_view replacements[]{
        "-1",
        "18446744073709551616",
        "01",
        "1.0",
        "true",
        "null",
        "\"\"",
    };
    for (std::string_view replacement : replacements) {
      std::string mutated = encoded;
      mutated.replace(index, end - index, replacement);
      expect_rejected(state, mutated);
    }
    index = end;
  }
}

} // namespace

int main() {
  TestState state;
  test_canonical_round_trip(state);
  test_invalid_inputs(state);
  test_deterministic_malformed_byte_corpus(state);
  test_deterministic_structural_mutation_corpus(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " resolution expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all resolution tests passed\n";
  return EXIT_SUCCESS;
}
