// Provider-independent agent metadata, attachment, and expected-type tests.

#include "sema/agent_metadata.h"
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

docs "Public operation."
    file "DESIGN.md"
pub work :: proc() -> i64 {
    judge "The implementation preserves the invariant."
        folder "notes"
    return ... "produce the answer" file "PROMPT.txt"
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
      diagnostics);
  const draft::AttachmentPolicy policy;
  const draft::AgentMetadataResult metadata = draft::collect_agent_metadata(
      sources, loaded.package, semantics.package, policy, diagnostics);
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
  EXPECT(state, metadata.records.size() == 4);
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
}

} // namespace

int main() {
  TestState state;
  test_agent_records(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " agent metadata expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all agent metadata tests passed\n";
  return EXIT_SUCCESS;
}
