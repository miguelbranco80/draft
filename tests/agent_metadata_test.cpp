// Provider-independent agent metadata, attachment, and expected-type tests.

#include "sema/agent_metadata.h"
#include "elaborator/obligation.h"
#include "sema/body_checker.h"
#include "sema/interface.h"
#include "sema/semantic.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"
#include "workspace/package.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>

namespace {

struct TestState {
  int failures = 0;
  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "agent_metadata_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void write_file(const std::filesystem::path &path, std::string_view contents) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    std::cerr << "cannot create metadata test directory: " << error.message() << '\n';
    std::exit(EXIT_FAILURE);
  }
  std::ofstream output(path, std::ios::binary);
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

struct TemporaryPackage {
  std::filesystem::path path;
  TemporaryPackage() {
    std::error_code error;
    path = std::filesystem::temp_directory_path(error) / "draft-agent-metadata-test";
    if (error) std::exit(EXIT_FAILURE);
    std::filesystem::remove_all(path, error);
    write_file(
        path / "package.draft",
        R"draft(package context

docs "Package design\ncontext."
    file "DESIGN.md"
    folder "notes"

Package_Context_Version :: 1

docs "Public operation."
    file "DESIGN.md"
pub work :: proc(
    values: []u32,
    callback: proc(value: ^i16, bytes: [4]u8) -> (bool, i64),
) -> i64 {
    judge "The implementation preserves the invariant."
        folder "notes"
    deny assert {
        deny context.user_index {
            // This local comment is deliberately not agent semantic context.
            return ... "produce the answer" file "PROMPT.txt"
        }
    }
}
)draft");
    write_file(path / "DESIGN.md", "stable design bytes\n");
    write_file(path / "PROMPT.txt", "answer constraints\n");
    write_file(path / "notes" / "a.md", "first note\n");
    write_file(path / "notes" / "nested" / "b.txt", "second note\n");
    write_file(path / "notes" / ".secret", "ignored\n");
  }
  ~TemporaryPackage() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

void test_agent_records(TestState &state) {
  TemporaryPackage temporary;
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::PackageLoadOptions load_options;
  load_options.file_tag = target.facts.file_tag;
  draft::PackageLoadResult loaded = draft::load_package(
      sources, temporary.path.string(), load_options, diagnostics);
  draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
      sources, loaded.package, target.facts, diagnostics);
  draft::BodyCheckResult bodies = draft::check_package_bodies(
      sources,
      loaded.package,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target.facts,
      diagnostics);
  const draft::AttachmentPolicy policy;
  const draft::AgentMetadataResult metadata = draft::collect_agent_metadata(
      sources, loaded.package, semantics.package, policy, diagnostics);
  const draft::AgentObligationResult obligations =
      draft::build_agent_obligations(
          {"workspace", "context"},
          sources,
          loaded.package,
          semantics.package,
          metadata,
          target,
          diagnostics);
  const draft::PackageInterface package_interface = draft::build_package_interface(
      {"workspace", "context"},
      semantics.package,
      semantics.constants,
      metadata,
      diagnostics);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, loaded.ok);
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, metadata.ok);
  EXPECT(state, obligations.ok);
  EXPECT(state, metadata.records.size() == 4);
  EXPECT(state, obligations.obligations.size() == 2);
  EXPECT(state, package_interface.documentation.size() == 2);
  EXPECT(state, !diagnostics.has_errors());
  if (metadata.records.size() != 4) return;

  const draft::AgentRecord &package_docs = metadata.records[0];
  const draft::AgentRecord &declaration_docs = metadata.records[1];
  const draft::AgentRecord &judgment = metadata.records[2];
  const draft::AgentRecord &synthesis = metadata.records[3];
  EXPECT(state, package_docs.kind == draft::AgentConstructKind::Documentation);
  EXPECT(state, package_docs.text == "Package design\ncontext.");
  EXPECT(state, package_docs.files.size() == 3);
  EXPECT(state, package_docs.public_interface);
  EXPECT(state, declaration_docs.public_interface);
  EXPECT(state, judgment.kind == draft::AgentConstructKind::Judgment);
  EXPECT(state, judgment.files.size() == 2);
  EXPECT(state, synthesis.kind == draft::AgentConstructKind::SynthesisExpression);
  EXPECT(state, synthesis.expected_type.is_valid());
  if (synthesis.expected_type.is_valid()) {
    EXPECT(state, semantics.package.types.type(synthesis.expected_type).name == "i64");
  }
  EXPECT(state, synthesis.files.size() == 1);
  EXPECT(state, synthesis.record_digest.hex().size() == 64);
  if (obligations.obligations.size() == 2) {
    const draft::AgentObligation &judgment_obligation =
        obligations.obligations[0];
    const draft::AgentObligation &synthesis_obligation =
        obligations.obligations[1];
    EXPECT(state,
        judgment_obligation.kind == draft::AgentConstructKind::Judgment);
    EXPECT(state,
        synthesis_obligation.kind ==
            draft::AgentConstructKind::SynthesisExpression);
    EXPECT(state, judgment_obligation.site_identity.starts_with("site-"));
    EXPECT(state, judgment_obligation.site_identity.size() == 69);
    EXPECT(state, synthesis_obligation.site_identity.size() == 69);
    EXPECT(state,
        synthesis_obligation.expected_type_digest.hex() !=
            std::string(64, '0'));
    EXPECT(state, synthesis_obligation.expected_type_text == "i64");
    EXPECT(state, synthesis_obligation.anchor_name == "work");
    EXPECT(state, synthesis_obligation.source_relative_path == "package.draft");
    EXPECT(state, synthesis_obligation.enclosing_declaration.present);
    EXPECT(state,
        synthesis_obligation.enclosing_declaration.name == "work");
    EXPECT(state,
        synthesis_obligation.enclosing_declaration.kind ==
            draft::SymbolKind::Procedure);
    EXPECT(state,
        synthesis_obligation.enclosing_declaration.source.find(
            "return ... \"produce the answer\"") != std::string::npos);
    EXPECT(state,
        synthesis_obligation.enclosing_declaration.source.find(
            "local comment") == std::string::npos);
    EXPECT(state,
        draft::sha256(
            synthesis_obligation.enclosing_declaration.source) ==
            synthesis_obligation.enclosing_declaration.source_digest);
    EXPECT(state, synthesis_obligation.active_denials.size() == 2);
    if (synthesis_obligation.active_denials.size() == 2) {
      EXPECT(state,
          synthesis_obligation.active_denials[0].selector == "assert");
      EXPECT(state,
          synthesis_obligation.active_denials[1].selector ==
              "context.user_index");
      EXPECT(state,
          draft::sha256(
              synthesis_obligation.active_denials[1].selector) ==
              synthesis_obligation.active_denials[1].selector_digest);
    }
    EXPECT(state, !synthesis_obligation.visible_bindings.empty());
    bool saw_values = false;
    bool saw_callback = false;
    for (const draft::AgentVisibleBinding &binding :
         synthesis_obligation.visible_bindings) {
      if (binding.name == "values") {
        EXPECT(state, binding.type_text == "[]u32");
        saw_values = true;
      }
      if (binding.name == "callback") {
        EXPECT(state,
            binding.type_text == "proc(^i16, [4]u8) -> (bool, i64)");
        saw_callback = true;
      }
    }
    EXPECT(state, saw_values);
    EXPECT(state, saw_callback);
    EXPECT(state,
        synthesis_obligation.target.identity == "draft-aarch64-macos-v5");
    EXPECT(state, synthesis_obligation.target.arch == "aarch64");
    EXPECT(state, synthesis_obligation.target.pointer_bits == 64);
    EXPECT(state,
        synthesis_obligation.target.assembly_dialect ==
            "draft-aarch64-apple-v2");
    EXPECT(state, synthesis_obligation.documentation.size() == 2);
    if (synthesis_obligation.documentation.size() == 2) {
      EXPECT(state,
          synthesis_obligation.documentation[0].anchor_name.empty());
      EXPECT(state,
          synthesis_obligation.documentation[0].text ==
              "Package design\ncontext.");
      EXPECT(state,
          synthesis_obligation.documentation[0].files.size() == 3);
      EXPECT(state,
          synthesis_obligation.documentation[1].anchor_name == "work");
      EXPECT(state,
          synthesis_obligation.documentation[1].text == "Public operation.");
      EXPECT(state,
          synthesis_obligation.documentation[1].file_contents.size() == 1);
      if (synthesis_obligation.documentation[1].file_contents.size() == 1) {
        EXPECT(state,
            synthesis_obligation.documentation[1].file_contents[0] ==
                "stable design bytes\n");
      }
    }
    EXPECT(state,
        synthesis_obligation.input_digest != judgment_obligation.input_digest);

    // Documentation is real synthesis input. Changing an attached design file
    // must stale the obligation even though the Draft source and site identity
    // are unchanged.
    write_file(temporary.path / "DESIGN.md", "changed design bytes\n");
    const draft::AgentMetadataResult changed_metadata =
        draft::collect_agent_metadata(
            sources,
            loaded.package,
            semantics.package,
            policy,
            diagnostics);
    const draft::AgentObligationResult changed_obligations =
        draft::build_agent_obligations(
            {"workspace", "context"},
            sources,
            loaded.package,
            semantics.package,
            changed_metadata,
            target,
            diagnostics);
    EXPECT(state, changed_metadata.ok);
    EXPECT(state, changed_obligations.ok);
    EXPECT(state, changed_obligations.obligations.size() == 2);
    if (changed_obligations.obligations.size() == 2) {
      EXPECT(state,
          changed_obligations.obligations[1].site_identity ==
              synthesis_obligation.site_identity);
      EXPECT(state,
          changed_obligations.obligations[1].input_digest !=
              synthesis_obligation.input_digest);
    }
  }
}

void test_dangling_documentation_is_rejected(TestState &state) {
  TemporaryPackage temporary;
  write_file(
      temporary.path / "package.draft",
      R"draft(package invalid_docs

Package_Context_Version :: 1

docs "This does not immediately precede a declaration."
judge "The judgment interrupts documentation attachment."

work :: proc() {
}
)draft");

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::PackageLoadOptions load_options;
  load_options.file_tag = target.facts.file_tag;
  const draft::PackageLoadResult loaded = draft::load_package(
      sources, temporary.path.string(), load_options, diagnostics);
  const draft::SemanticAnalysisResult semantics =
      draft::analyze_package_semantics(
          sources, loaded.package, target.facts, diagnostics);
  const draft::AgentMetadataResult metadata = draft::collect_agent_metadata(
      sources,
      loaded.package,
      semantics.package,
      {},
      diagnostics);

  EXPECT(state, loaded.ok);
  EXPECT(state, semantics.ok);
  EXPECT(state, !metadata.ok);
  EXPECT(state,
      draft::render_diagnostics(sources, diagnostics).find(
          "documentation must be package documentation") !=
          std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_agent_records(state);
  test_dangling_documentation_is_rejected(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " agent metadata expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all agent metadata tests passed\n";
  return EXIT_SUCCESS;
}
