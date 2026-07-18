// Canonical resolution-manifest encoding and strict-input rejection tests.

#include "elaborator/resolution.h"

#include "base/sha256.h"
#include "source/diagnostic.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

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
  pin.provider_identity = std::move(provider);
  pin.model_identity = std::move(model);
  return pin;
}

void test_canonical_round_trip(TestState &state) {
  draft::ResolutionManifest manifest;
  manifest.target_identity = "aarch64-apple-macos";
  manifest.resolved_program_digest = draft::sha256("resolved program");

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
      "model-two"));

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
  EXPECT(state, parsed.format == "draft-resolution-v1");
  EXPECT(state, parsed.target_identity == manifest.target_identity);
  EXPECT(state,
      parsed.resolved_program_digest == manifest.resolved_program_digest);
  EXPECT(state, parsed.pins.size() == 2);
  if (parsed.pins.size() == 2) {
    EXPECT(state, parsed.pins[0].site_identity == site('a'));
    EXPECT(state,
        parsed.pins[0].kind ==
            draft::AgentConstructKind::SynthesisExpression);
    EXPECT(state, parsed.pins[1].provider_identity == "codex\nlocal");
    EXPECT(state, parsed.pins[1].model_identity == "model-\"one\"");
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
}

} // namespace

int main() {
  TestState state;
  test_canonical_round_trip(state);
  test_invalid_inputs(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " resolution expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all resolution tests passed\n";
  return EXIT_SUCCESS;
}
