// Provider-independent agent metadata, attachment, and expected-type tests.

#include "sema/agent_metadata.h"
#include "compile/compiler.h"
#include "elaborator/obligation.h"
#include "sema/body_checker.h"
#include "sema/interface.h"
#include "sema/semantic.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"
#include "workspace/package.h"

#include "test_directory.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

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

std::string read_file(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  input.seekg(0, std::ios::end);
  const std::streamoff end = input.tellg();
  if (!input || end < 0) std::exit(EXIT_FAILURE);
  std::string contents(static_cast<std::size_t>(end), '\0');
  input.seekg(0, std::ios::beg);
  if (!contents.empty()) {
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  }
  if (!input) std::exit(EXIT_FAILURE);
  return contents;
}

struct TemporaryPackage {
  draft::test::TemporaryDirectory directory{"draft-agent-metadata-test"};
  std::filesystem::path path;
  TemporaryPackage() {
    path = directory.path();
    write_file(
        path / "package.draft",
        R"draft(package context

docs "Package design\ncontext."
    file "DESIGN.md"
    folder "notes"

Package_Context_Version :: 1

Record :: struct {
    count: u32,
}

docs "Public operation."
    file "DESIGN.md"
pub work[T: integer, N: usize] :: proc(
    values: []u32,
    record: Record,
    callback: proc(value: ^i16, bytes: [4]u8) -> (bool, i64),
    blocked: u64,
    secret: i16,
) -> i64 {
    judge "The implementation preserves the invariant."
        folder "notes"
    deny assert, blocked, secret {
        deny context.user_index {
            // This shadows the denied parameter. Denials name semantic
            // entities, so this distinct inner binding remains usable.
            blocked := false
            Answer :: 42
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
  draft::PackageBodyWorkState bodies = draft::check_package_bodies(
      sources,
      loaded.package,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target.facts,
      diagnostics);
  const draft::AttachmentPolicy policy;
  const draft::AgentMetadataResult metadata = draft::collect_agent_metadata(
      sources, loaded.package, bodies.package, policy, diagnostics);
  const draft::AgentObligationResult obligations =
      draft::build_agent_obligations(
          {"workspace", "context"},
          sources,
          loaded.package,
          bodies.package,
          bodies.constants,
          metadata,
          draft::imported_procedure_contracts(bodies.package),
          target,
          diagnostics,
          {},
          bodies.procedures);
  const draft::PackageInterface package_interface = draft::build_package_interface(
      {"workspace", "context"},
      bodies.package,
      bodies.constants,
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
    EXPECT(state, bodies.package.types.type(synthesis.expected_type).name == "i64");
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
    EXPECT(state,
        draft::sha256(
            synthesis_obligation.enclosing_declaration.semantic_skeleton) ==
            synthesis_obligation.enclosing_declaration
                .semantic_skeleton_digest);
    EXPECT(state,
        synthesis_obligation.enclosing_declaration.semantic_skeleton.find(
            "PARAMETER_NAME 6\nvalues") != std::string::npos);
    EXPECT(state,
        synthesis_obligation.enclosing_declaration.semantic_skeleton.find(
            "DECLARATION_RESULT_TYPE 3\ni64") != std::string::npos);
    EXPECT(state,
        synthesis_obligation.enclosing_declaration.semantic_skeleton.find(
            "produce the answer") == std::string::npos);
    EXPECT(state, synthesis_obligation.active_denials.size() == 4);
    if (synthesis_obligation.active_denials.size() == 4) {
      EXPECT(state,
          synthesis_obligation.active_denials[0].selector == "assert");
      EXPECT(state,
          synthesis_obligation.active_denials[1].selector == "blocked");
      EXPECT(state,
          synthesis_obligation.active_denials[2].selector == "secret");
      EXPECT(state,
          synthesis_obligation.active_denials[3].selector ==
              "context.user_index");
      EXPECT(state,
          draft::sha256(
              synthesis_obligation.active_denials[3].selector) ==
              synthesis_obligation.active_denials[3].selector_digest);
    }
    EXPECT(state, synthesis_obligation.context_fields.size() == 7);
    bool saw_allocator_context = false;
    bool saw_denied_user_index_context = false;
    for (const draft::AgentContextField &field :
         synthesis_obligation.context_fields) {
      if (field.name == "allocator") {
        EXPECT(state, field.offset == 0);
        EXPECT(state, !field.type_text.empty());
        saw_allocator_context = true;
      }
      if (field.name == "user_index") {
        saw_denied_user_index_context = true;
      }
    }
    EXPECT(state, saw_allocator_context);
    EXPECT(state, !saw_denied_user_index_context);
    EXPECT(state, synthesis_obligation.parametric_parameters.size() == 2);
    if (synthesis_obligation.parametric_parameters.size() == 2) {
      const draft::AgentParametricParameter &type_parameter =
          synthesis_obligation.parametric_parameters[0];
      const draft::AgentParametricParameter &value_parameter =
          synthesis_obligation.parametric_parameters[1];
      EXPECT(state, type_parameter.name == "T");
      EXPECT(state,
          type_parameter.kind == draft::SymbolKind::TypeParameter);
      EXPECT(state, type_parameter.constraint == "integer");
      EXPECT(state, type_parameter.type_text == "T");
      EXPECT(state, value_parameter.name == "N");
      EXPECT(state,
          value_parameter.kind == draft::SymbolKind::ValueParameter);
      EXPECT(state, value_parameter.constraint == "value");
      EXPECT(state, value_parameter.type_text == "usize");
    }
    bool saw_record_definition = false;
    for (const draft::AgentTypeContext &type :
         synthesis_obligation.type_contexts) {
      EXPECT(state, draft::sha256(type.definition) == type.definition_digest);
      if (type.definition.find("TYPE_NOMINAL_PUBLIC_NAME 6\nRecord") !=
              std::string::npos &&
          type.definition.find("MEMBER_NAME 5\ncount") !=
              std::string::npos) {
        saw_record_definition = true;
      }
    }
    EXPECT(state, saw_record_definition);
    EXPECT(state, synthesis_obligation.guiding_judgments.size() == 1);
    if (synthesis_obligation.guiding_judgments.size() == 1) {
      const draft::AgentJudgmentContext &guidance =
          synthesis_obligation.guiding_judgments.front();
      EXPECT(state, guidance.anchor_name == "work");
      EXPECT(state,
          guidance.claim == "The implementation preserves the invariant.");
      EXPECT(state, guidance.files.size() == 2);
      EXPECT(state, guidance.file_contents.size() == 2);
      if (guidance.file_contents.size() == 2) {
        EXPECT(state, guidance.file_contents[0] == "first note\n");
        EXPECT(state, guidance.file_contents[1] == "second note\n");
      }
    }
    EXPECT(state, !synthesis_obligation.visible_bindings.empty());
    bool saw_values = false;
    bool saw_callback = false;
    bool saw_shadowed_blocked = false;
    bool saw_denied_secret = false;
    bool saw_package_version = false;
    bool saw_lexical_constant = false;
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
      if (binding.name == "blocked") {
        EXPECT(state, binding.type_text == "bool");
        saw_shadowed_blocked = true;
      }
      if (binding.name == "secret") saw_denied_secret = true;
      if (binding.name == "Package_Context_Version") {
        EXPECT(state, binding.has_constant);
        EXPECT(state,
            draft::sha256(binding.constant_definition) ==
                binding.constant_digest);
        EXPECT(state,
            binding.constant_definition.find("CONSTANT_INTEGER 1\n1\n") !=
                std::string::npos);
        saw_package_version = true;
      }
      if (binding.name == "Answer") {
        EXPECT(state, binding.has_constant);
        EXPECT(state,
            binding.constant_definition.find("CONSTANT_INTEGER 2\n42\n") !=
                std::string::npos);
        saw_lexical_constant = true;
      }
    }
    EXPECT(state, saw_values);
    EXPECT(state, saw_callback);
    EXPECT(state, saw_shadowed_blocked);
    EXPECT(state, !saw_denied_secret);
    EXPECT(state, saw_package_version);
    EXPECT(state, saw_lexical_constant);
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
            bodies.package,
            policy,
            diagnostics);
    const draft::AgentObligationResult changed_obligations =
        draft::build_agent_obligations(
            {"workspace", "context"},
            sources,
            loaded.package,
            bodies.package,
            bodies.constants,
            changed_metadata,
            draft::imported_procedure_contracts(bodies.package),
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

      // A nominal type keeps the same compact nominal identity when one of its
      // fields changes. The supplied complete definition must nevertheless
      // change and stale the site; otherwise `record: Record` would hide the
      // only information a provider needs to use the value correctly.
      std::string changed_source = read_file(temporary.path / "package.draft");
      const std::size_t member = changed_source.find("count: u32");
      EXPECT(state, member != std::string::npos);
      if (member != std::string::npos) {
        changed_source.replace(member, std::string_view("count").size(), "total");
        write_file(temporary.path / "package.draft", changed_source);

        draft::SourceManager member_sources;
        draft::DiagnosticSink member_diagnostics;
        const draft::PackageLoadResult member_loaded = draft::load_package(
            member_sources,
            temporary.path.string(),
            load_options,
            member_diagnostics);
        draft::SemanticAnalysisResult member_semantics =
            draft::analyze_package_semantics(
                member_sources,
                member_loaded.package,
                target.facts,
                member_diagnostics);
        const draft::PackageBodyWorkState member_bodies =
            draft::check_package_bodies(
                member_sources,
                member_loaded.package,
                member_semantics.selections,
                member_semantics.package,
                member_semantics.constants,
                target.facts,
                member_diagnostics);
        const draft::AgentMetadataResult member_metadata =
            draft::collect_agent_metadata(
                member_sources,
                member_loaded.package,
                member_bodies.package,
                policy,
                member_diagnostics);
        const draft::AgentObligationResult member_obligations =
            draft::build_agent_obligations(
                {"workspace", "context"},
                member_sources,
                member_loaded.package,
                member_bodies.package,
                member_bodies.constants,
                member_metadata,
                draft::imported_procedure_contracts(member_bodies.package),
                target,
                member_diagnostics);
        EXPECT(state, member_loaded.ok);
        EXPECT(state, member_semantics.ok);
        EXPECT(state, member_bodies.ok);
        EXPECT(state, member_metadata.ok);
        EXPECT(state, member_obligations.ok);
        EXPECT(state, !member_diagnostics.has_errors());
        EXPECT(state, member_obligations.obligations.size() == 2);
        if (member_obligations.obligations.size() == 2) {
          const draft::AgentObligation &member_synthesis =
              member_obligations.obligations[1];
          const draft::AgentObligation &before_member_change =
              changed_obligations.obligations[1];
          EXPECT(state,
              member_synthesis.site_identity ==
                  before_member_change.site_identity);
          EXPECT(state,
              member_synthesis.input_digest !=
                  before_member_change.input_digest);
          const draft::AgentVisibleBinding *before_record = nullptr;
          const draft::AgentVisibleBinding *after_record = nullptr;
          for (const draft::AgentVisibleBinding &binding :
               before_member_change.visible_bindings) {
            if (binding.name == "record") before_record = &binding;
          }
          for (const draft::AgentVisibleBinding &binding :
               member_synthesis.visible_bindings) {
            if (binding.name == "record") after_record = &binding;
          }
          EXPECT(state, before_record != nullptr);
          EXPECT(state, after_record != nullptr);
          if (before_record != nullptr && after_record != nullptr) {
            EXPECT(state,
                before_record->type_digest == after_record->type_digest);
          }
        }
      }
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

void test_judgment_guidance_respects_branch_dominance(TestState &state) {
  TemporaryPackage temporary;
  write_file(
      temporary.path / "package.draft",
      R"draft(package judgment_context

Package_Context_Version :: 1

judge "package-wide"

work :: proc() -> i64 {
    if true {
        judge "branch-only"
    }
    return ... "produce a value"
}

)draft");

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::PackageLoadOptions load_options;
  load_options.file_tag = target.facts.file_tag;
  const draft::PackageLoadResult loaded = draft::load_package(
      sources, temporary.path.string(), load_options, diagnostics);
  draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
      sources, loaded.package, target.facts, diagnostics);
  const draft::PackageBodyWorkState bodies = draft::check_package_bodies(
      sources,
      loaded.package,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target.facts,
      diagnostics);
  const draft::AgentMetadataResult metadata = draft::collect_agent_metadata(
      sources, loaded.package, bodies.package, {}, diagnostics);
  const draft::AgentObligationResult obligations =
      draft::build_agent_obligations(
          {"workspace", "judgment_context"},
          sources,
          loaded.package,
          bodies.package,
          bodies.constants,
          metadata,
          draft::imported_procedure_contracts(bodies.package),
          target,
          diagnostics);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, loaded.ok);
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, metadata.ok);
  EXPECT(state, obligations.ok);
  EXPECT(state, !diagnostics.has_errors());
  const draft::AgentObligation *synthesis = nullptr;
  for (const draft::AgentObligation &obligation : obligations.obligations) {
    if (obligation.kind == draft::AgentConstructKind::SynthesisExpression) {
      synthesis = &obligation;
      break;
    }
  }
  EXPECT(state, synthesis != nullptr);
  if (synthesis != nullptr) {
    // Package claims are universal. The claim inside the completed if branch
    // does not dominate the later return and must not leak into its request.
    EXPECT(state, synthesis->guiding_judgments.size() == 1);
    if (synthesis->guiding_judgments.size() == 1) {
      EXPECT(state,
          synthesis->guiding_judgments.front().claim == "package-wide");
    }
  }
}

// A package claim is universal within its package, while a type-member claim
// reaches only a later synthesis site in the same member list. This focused
// early-interface test keeps that positional rule independent of Codex and of
// any generated expansion. It also verifies that attachments on the package
// claim remain available to the member request.
void test_member_synthesis_receives_package_and_type_judgments(
    TestState &state) {
  TemporaryPackage temporary;
  write_file(
      temporary.path / "package.draft",
      R"draft(package member_guidance

judge "package-wide" file "CONTRACT.md"

Packet :: struct {
    prefix: u32,
    judge "member-local"
    ... "declare the remaining field"
}
)draft");
  write_file(temporary.path / "CONTRACT.md", "shared member contract\n");

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::CompileWorkspaceOptions options;
  options.target = target;
  options.workspace.workspace_directory = temporary.path.string();
  options.stage = draft::CompileWorkspaceStage::DiscoverInterfaceSynthesis;
  const draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources, temporary.path.string(), std::move(options), diagnostics);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, compiled.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, compiled.graph.root_package.is_valid());
  if (!compiled.ok || !compiled.graph.root_package.is_valid() ||
      compiled.graph.root_package.value >= compiled.packages.size() ||
      !compiled.packages[compiled.graph.root_package.value].has_value()) {
    return;
  }
  const draft::AgentObligationResult &obligations =
      compiled.packages[compiled.graph.root_package.value]->obligations;
  EXPECT(state, obligations.ok);

  const draft::AgentObligation *member_synthesis = nullptr;
  for (const draft::AgentObligation &obligation : obligations.obligations) {
    if (obligation.kind == draft::AgentConstructKind::SynthesisMember) {
      member_synthesis = &obligation;
      break;
    }
  }
  EXPECT(state, member_synthesis != nullptr);
  if (member_synthesis == nullptr) return;

  EXPECT(state, member_synthesis->anchor_name == "Packet");
  EXPECT(state, member_synthesis->guiding_judgments.size() == 2);
  if (member_synthesis->guiding_judgments.size() != 2) return;

  const draft::AgentJudgmentContext &package_guidance =
      member_synthesis->guiding_judgments[0];
  const draft::AgentJudgmentContext &member_guidance =
      member_synthesis->guiding_judgments[1];
  EXPECT(state, package_guidance.anchor_name.empty());
  EXPECT(state, package_guidance.claim == "package-wide");
  EXPECT(state, package_guidance.file_contents.size() == 1);
  if (package_guidance.file_contents.size() == 1) {
    EXPECT(state,
        package_guidance.file_contents.front() == "shared member contract\n");
  }
  EXPECT(state, member_guidance.anchor_name == "Packet");
  EXPECT(state, member_guidance.claim == "member-local");
  EXPECT(state, member_guidance.file_contents.empty());
}

// Package judgments are reviews of the completed package, not of the lexical
// declarations visible before their source offset. Keep a claim before the
// declarations here so the test catches any regression to ordinary
// source-position lookup. The obligation must carry each complete top-level
// definition, including procedure bodies, to a provider-independent request.
void test_package_judgment_receives_complete_package_source(
    TestState &state) {
  TemporaryPackage temporary;
  write_file(
      temporary.path / "package.draft",
      R"draft(package package_judgment_context

judge "The package implements and checks Counter addition."

Counter :: struct {
    value: i64,
}

counter_add :: proc(counter: ^Counter, amount: i64) {
    counter^.value += amount
}

main :: proc() -> int {
    counter := Counter{value = 40}
    counter_add(&counter, 2)
    assert(counter.value == 42)
    return 0
}
)draft");

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::PackageLoadOptions load_options;
  load_options.file_tag = target.facts.file_tag;
  const draft::PackageLoadResult loaded = draft::load_package(
      sources, temporary.path.string(), load_options, diagnostics);
  draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
      sources, loaded.package, target.facts, diagnostics);
  const draft::PackageBodyWorkState bodies = draft::check_package_bodies(
      sources,
      loaded.package,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target.facts,
      diagnostics);
  const draft::AgentMetadataResult metadata = draft::collect_agent_metadata(
      sources,
      loaded.package,
      bodies.package,
      {},
      diagnostics);
  const draft::AgentObligationResult obligations =
      draft::build_agent_obligations(
          {"workspace", "package_judgment_context"},
          sources,
          loaded.package,
          bodies.package,
          bodies.constants,
          metadata,
          draft::imported_procedure_contracts(bodies.package),
          target,
          diagnostics,
          {},
          bodies.procedures);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, loaded.ok);
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, metadata.ok);
  EXPECT(state, obligations.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, obligations.obligations.size() == 1);
  if (obligations.obligations.size() != 1) return;

  const draft::AgentObligation &judgment = obligations.obligations.front();
  EXPECT(state, judgment.kind == draft::AgentConstructKind::Judgment);
  EXPECT(state, judgment.anchor_name.empty());
  // The semantic package may also expose compiler-owned source-backed rows;
  // the contract here is that every authored top-level declaration is present,
  // not that those three declarations exhaust the internal package scope.
  EXPECT(state, judgment.relevant_declarations.size() >= 3);

  bool saw_counter = false;
  bool saw_counter_add = false;
  bool saw_main = false;
  for (const draft::AgentDeclarationContext &declaration :
       judgment.relevant_declarations) {
    if (declaration.name == "Counter") {
      EXPECT(state,
          declaration.source.find("value: i64") != std::string::npos);
      saw_counter = true;
    }
    if (declaration.name == "counter_add") {
      EXPECT(state,
          declaration.source.find("counter^.value += amount") !=
              std::string::npos);
      saw_counter_add = true;
    }
    if (declaration.name == "main") {
      EXPECT(state,
          declaration.source.find("assert(counter.value == 42)") !=
              std::string::npos);
      saw_main = true;
    }
  }
  EXPECT(state, saw_counter);
  EXPECT(state, saw_counter_add);
  EXPECT(state, saw_main);
}

void test_body_sites_receive_typed_branch_refinements(TestState &state) {
  TemporaryPackage temporary;
  write_file(
      temporary.path / "package.draft",
      R"draft(package branch_context

work :: proc(flag: bool, value: i64, values: []i64) {
    if flag {
        isolated :: proc() {
            judge "nested static"
        }
        isolated()
        if value > 0 {
            judge "positive"
        }
        for value > 0 {
            judge "conditional loop"
            break
        }
    } else {
        judge "negative flag"
    }
    for index: i64 = 0; index < value; index += 1 {
        judge "clause loop"
        break
    }
    for element, index in values {
        _ = element
        _ = index
        judge "iteration loop"
        break
    }
    for element, index in values {
        generated: i64 = ... "iteration expression"
        _ = element
        _ = index
        _ = generated
        break
    }
    for element, index in values {
        _ = element
        judge "iteration before direct mutation"
        index = 7
        judge "iteration after direct mutation"
        break
    }
    for element, index in values {
        pointer := &index
        _ = element
        _ = pointer
        judge "iteration escaped index"
        break
    }
    for element, index in values {
        _ = element
        deny index {
            judge "iteration denied index"
        }
        break
    }
    for element, outer_index in values {
        _ = element
        for flag {
            outer_index = 3
            break
        }
        judge "iteration after nested mutation"
        break
    }
    for mutable_index: i64 = 0;
        mutable_index < value;
        mutable_index += 1 {
        judge "mutable clause before mutation"
        mutable_index += 1
        break
    }
    switch value {
    case 1, 2:
        judge "selected values"
    case:
        judge "other values"
    }
}
)draft");

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::PackageLoadOptions load_options;
  load_options.file_tag = target.facts.file_tag;
  const draft::PackageLoadResult loaded = draft::load_package(
      sources, temporary.path.string(), load_options, diagnostics);
  draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
      sources, loaded.package, target.facts, diagnostics);
  const draft::PackageBodyWorkState bodies = draft::check_package_bodies(
      sources,
      loaded.package,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target.facts,
      diagnostics);
  const draft::AgentMetadataResult metadata = draft::collect_agent_metadata(
      sources, loaded.package, bodies.package, {}, diagnostics);
  const draft::AgentObligationResult obligations =
      draft::build_agent_obligations(
          {"workspace", "branch_context"},
          sources,
          loaded.package,
          bodies.package,
          bodies.constants,
          metadata,
          draft::imported_procedure_contracts(bodies.package),
          target,
          diagnostics);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, loaded.ok);
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, metadata.ok);
  EXPECT(state, obligations.ok);
  EXPECT(state, !diagnostics.has_errors());

  const auto obligation_for_claim = [&metadata, &obligations](
                                        std::string_view claim)
      -> const draft::AgentObligation * {
    for (const draft::AgentRecord &record : metadata.records) {
      if (record.kind != draft::AgentConstructKind::Judgment ||
          record.text != claim) {
        continue;
      }
      for (const draft::AgentObligation &obligation :
           obligations.obligations) {
        if (obligation.record_digest == record.record_digest) {
          return &obligation;
        }
      }
    }
    return nullptr;
  };
  const auto obligation_for_synthesis = [&metadata, &obligations](
                                            std::string_view prompt)
      -> const draft::AgentObligation * {
    for (const draft::AgentRecord &record : metadata.records) {
      if (record.kind != draft::AgentConstructKind::SynthesisExpression ||
          record.text != prompt) {
        continue;
      }
      for (const draft::AgentObligation &obligation :
           obligations.obligations) {
        if (obligation.record_digest == record.record_digest) {
          return &obligation;
        }
      }
    }
    return nullptr;
  };

  const draft::AgentObligation *positive =
      obligation_for_claim("positive");
  const draft::AgentObligation *negative =
      obligation_for_claim("negative flag");
  const draft::AgentObligation *nested =
      obligation_for_claim("nested static");
  const draft::AgentObligation *selected =
      obligation_for_claim("selected values");
  const draft::AgentObligation *other =
      obligation_for_claim("other values");
  const draft::AgentObligation *conditional_loop =
      obligation_for_claim("conditional loop");
  const draft::AgentObligation *clause_loop =
      obligation_for_claim("clause loop");
  const draft::AgentObligation *iteration_loop =
      obligation_for_claim("iteration loop");
  const draft::AgentObligation *iteration_expression =
      obligation_for_synthesis("iteration expression");
  const draft::AgentObligation *iteration_before_mutation =
      obligation_for_claim("iteration before direct mutation");
  const draft::AgentObligation *iteration_after_mutation =
      obligation_for_claim("iteration after direct mutation");
  const draft::AgentObligation *iteration_escaped =
      obligation_for_claim("iteration escaped index");
  const draft::AgentObligation *iteration_denied =
      obligation_for_claim("iteration denied index");
  const draft::AgentObligation *iteration_after_nested_mutation =
      obligation_for_claim("iteration after nested mutation");
  const draft::AgentObligation *mutable_clause =
      obligation_for_claim("mutable clause before mutation");
  EXPECT(state, positive != nullptr);
  EXPECT(state, negative != nullptr);
  EXPECT(state, nested != nullptr);
  EXPECT(state, selected != nullptr);
  EXPECT(state, other != nullptr);
  EXPECT(state, conditional_loop != nullptr);
  EXPECT(state, clause_loop != nullptr);
  EXPECT(state, iteration_loop != nullptr);
  EXPECT(state, iteration_expression != nullptr);
  EXPECT(state, iteration_before_mutation != nullptr);
  EXPECT(state, iteration_after_mutation != nullptr);
  EXPECT(state, iteration_escaped != nullptr);
  EXPECT(state, iteration_denied != nullptr);
  EXPECT(state, iteration_after_nested_mutation != nullptr);
  EXPECT(state, mutable_clause != nullptr);

  if (positive != nullptr) {
    EXPECT(state, positive->branch_refinements.size() == 2);
    if (positive->branch_refinements.size() == 2) {
      EXPECT(state,
          positive->branch_refinements[0].kind ==
              draft::AgentBranchRefinementKind::ConditionTrue);
      EXPECT(state, positive->branch_refinements[0].subject == "flag");
      EXPECT(state, positive->branch_refinements[0].type_text == "bool");
      EXPECT(state,
          positive->branch_refinements[1].kind ==
              draft::AgentBranchRefinementKind::ConditionTrue);
      EXPECT(state, positive->branch_refinements[1].subject == "value > 0");
      EXPECT(state, positive->branch_refinements[1].type_text == "bool");
    }
  }
  if (negative != nullptr) {
    EXPECT(state, negative->branch_refinements.size() == 1);
    if (negative->branch_refinements.size() == 1) {
      EXPECT(state,
          negative->branch_refinements[0].kind ==
              draft::AgentBranchRefinementKind::ConditionFalse);
      EXPECT(state, negative->branch_refinements[0].subject == "flag");
    }
  }
  if (nested != nullptr) {
    EXPECT(state, nested->branch_refinements.empty());
  }
  if (selected != nullptr) {
    EXPECT(state, selected->branch_refinements.size() == 1);
    if (selected->branch_refinements.size() == 1) {
      const draft::AgentBranchRefinement &refinement =
          selected->branch_refinements[0];
      EXPECT(state,
          refinement.kind ==
              draft::AgentBranchRefinementKind::SwitchCase);
      EXPECT(state, refinement.subject == "value");
      EXPECT(state, refinement.type_text == "i64");
      EXPECT(state,
          refinement.values == std::vector<std::string>({"1", "2"}));
      EXPECT(state,
          refinement.values.size() == refinement.value_digests.size());
    }
  }
  if (other != nullptr) {
    EXPECT(state, other->branch_refinements.size() == 1);
    if (other->branch_refinements.size() == 1) {
      const draft::AgentBranchRefinement &refinement =
          other->branch_refinements[0];
      EXPECT(state,
          refinement.kind ==
              draft::AgentBranchRefinementKind::SwitchDefault);
      EXPECT(state, refinement.subject == "value");
      EXPECT(state, refinement.type_text == "i64");
      EXPECT(state,
          refinement.values == std::vector<std::string>({"1", "2"}));
    }
  }
  if (conditional_loop != nullptr) {
    EXPECT(state, conditional_loop->branch_refinements.size() == 2);
    if (conditional_loop->branch_refinements.size() == 2) {
      EXPECT(state,
          conditional_loop->branch_refinements[0].kind ==
              draft::AgentBranchRefinementKind::ConditionTrue);
      EXPECT(state,
          conditional_loop->branch_refinements[0].subject == "flag");
      EXPECT(state,
          conditional_loop->branch_refinements[1].kind ==
              draft::AgentBranchRefinementKind::LoopConditionTrue);
      EXPECT(state,
          conditional_loop->branch_refinements[1].subject == "value > 0");
      EXPECT(state,
          conditional_loop->branch_refinements[1].type_text == "bool");
    }
  }
  if (clause_loop != nullptr) {
    EXPECT(state, clause_loop->branch_refinements.size() == 1);
    if (clause_loop->branch_refinements.size() == 1) {
      const draft::AgentBranchRefinement &refinement =
          clause_loop->branch_refinements.front();
      EXPECT(state,
          refinement.kind ==
              draft::AgentBranchRefinementKind::LoopConditionTrue);
      EXPECT(state, refinement.subject == "index < value");
      EXPECT(state, refinement.type_text == "bool");
    }
    EXPECT(state, clause_loop->loop_ranges.size() == 1);
    if (clause_loop->loop_ranges.size() == 1) {
      const draft::AgentLoopRange &range = clause_loop->loop_ranges.front();
      EXPECT(state,
          range.kind == draft::AgentLoopRangeKind::HeaderEntryValue);
      EXPECT(state, range.binding_name == "index");
      EXPECT(state, range.binding_type_text == "i64");
      EXPECT(state, range.lower_bound == "0");
      EXPECT(state, range.upper == "value");
      EXPECT(state, range.upper_type_text == "i64");
      EXPECT(state, range.upper_digest == draft::sha256(range.upper));
    }
  }
  if (iteration_loop != nullptr) {
    EXPECT(state, iteration_loop->branch_refinements.size() == 1);
    if (iteration_loop->branch_refinements.size() == 1) {
      const draft::AgentBranchRefinement &refinement =
          iteration_loop->branch_refinements.front();
      EXPECT(state,
          refinement.kind ==
              draft::AgentBranchRefinementKind::LoopIteration);
      EXPECT(state, refinement.subject == "values");
      EXPECT(state, refinement.type_text == "[]i64");
    }
    EXPECT(state, iteration_loop->loop_ranges.size() == 1);
    if (iteration_loop->loop_ranges.size() == 1) {
      const draft::AgentLoopRange &range =
          iteration_loop->loop_ranges.front();
      EXPECT(state,
          range.kind ==
              draft::AgentLoopRangeKind::CapturedIterationLength);
      EXPECT(state, range.binding_name == "index");
      EXPECT(state, range.binding_type_text == "usize");
      EXPECT(state, range.lower_bound == "0");
      EXPECT(state, range.upper == "values");
      EXPECT(state, range.upper_type_text == "[]i64");
      EXPECT(state, range.upper_digest == draft::sha256(range.upper));
    }
  }
  if (iteration_expression != nullptr) {
    EXPECT(state, iteration_expression->loop_ranges.size() == 1);
    if (iteration_expression->loop_ranges.size() == 1) {
      EXPECT(state,
          iteration_expression->loop_ranges.front().binding_name == "index");
    }
  }
  if (iteration_before_mutation != nullptr) {
    EXPECT(state, iteration_before_mutation->loop_ranges.size() == 1);
    if (iteration_before_mutation->loop_ranges.size() == 1) {
      EXPECT(state,
          iteration_before_mutation->loop_ranges.front().binding_name ==
              "index");
    }
  }
  if (iteration_after_mutation != nullptr) {
    EXPECT(state, iteration_after_mutation->loop_ranges.empty());
  }
  if (iteration_escaped != nullptr) {
    EXPECT(state, iteration_escaped->loop_ranges.empty());
  }
  if (iteration_denied != nullptr) {
    EXPECT(state, iteration_denied->loop_ranges.empty());
  }
  if (iteration_after_nested_mutation != nullptr) {
    EXPECT(state, iteration_after_nested_mutation->loop_ranges.empty());
  }
  if (mutable_clause != nullptr) {
    // A write in the body changes the next clause iteration's induction state,
    // so the static site cannot receive a fact valid on every execution.
    EXPECT(state, mutable_clause->loop_ranges.empty());
  }
}

void test_visible_import_interface_is_context(TestState &state) {
  TemporaryPackage temporary;
  const std::filesystem::path workspace = temporary.path / "workspace";
  write_file(
      workspace / "lib" / "package.draft",
      "package lib\n"
      "docs \"Library design.\" file \"GUIDE.md\"\n"
      "pub Answer :: 42\n"
      "docs \"Record design.\" file \"RECORD.md\"\n"
      "pub Record :: struct {\n"
      "    count: u32,\n"
      "}\n");
  write_file(workspace / "lib" / "GUIDE.md", "library design bytes\n");
  write_file(workspace / "lib" / "RECORD.md", "record design bytes\n");
  write_file(
      workspace / "app" / "package.draft",
      "package app\n"
      "import lib as lib\n"
      "main :: proc() {\n"
      "    value: lib.Record = ... \"make a record\"\n"
      "}\n");

  auto compile = [&](draft::SourceManager &sources,
                     draft::DiagnosticSink &diagnostics) {
    draft::CompileWorkspaceOptions options;
    options.target = draft::make_aarch64_macos_profile();
    options.workspace.workspace_directory = workspace.string();
    return draft::compile_workspace(
        sources,
        (workspace / "app").string(),
        std::move(options),
        diagnostics);
  };

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::CompileWorkspaceResult initial = compile(sources, diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, initial.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, initial.graph.root_package.is_valid());
  const draft::AgentObligation *initial_synthesis = nullptr;
  if (initial.graph.root_package.is_valid() &&
      initial.graph.root_package.value < initial.packages.size() &&
      initial.packages[initial.graph.root_package.value].has_value()) {
    for (const draft::AgentObligation &obligation :
         initial.packages[initial.graph.root_package.value]
             ->obligations.obligations) {
      if (obligation.kind == draft::AgentConstructKind::SynthesisExpression) {
        initial_synthesis = &obligation;
      }
    }
  }
  EXPECT(state, initial_synthesis != nullptr);
  if (initial_synthesis == nullptr) return;
  EXPECT(state, initial_synthesis->imported_packages.size() == 1);
  if (initial_synthesis->imported_packages.size() == 1) {
    const draft::AgentImportedPackageContext &context =
        initial_synthesis->imported_packages.front();
    EXPECT(state, context.alias == "lib");
    EXPECT(state, context.root_identity == "workspace");
    EXPECT(state, context.root_relative_path == "lib");
    EXPECT(state,
        context.definition.find("DECLARATION_NAME 6\nAnswer") !=
            std::string::npos);
    EXPECT(state,
        context.definition.find("DECLARATION_NAME 6\nRecord") !=
            std::string::npos);
    EXPECT(state,
        draft::sha256(context.definition) == context.definition_digest);
    EXPECT(state, context.documentation.size() == 2);
    bool saw_library_docs = false;
    bool saw_record_docs = false;
    for (const draft::AgentDocumentationContext &documentation :
         context.documentation) {
      if (documentation.text == "Library design.") {
        EXPECT(state, documentation.file_contents.size() == 1);
        if (documentation.file_contents.size() == 1) {
          EXPECT(state,
              documentation.file_contents.front() ==
                  "library design bytes\n");
        }
        saw_library_docs = true;
      }
      if (documentation.text == "Record design.") {
        EXPECT(state, documentation.anchor_name == "Record");
        EXPECT(state, documentation.file_contents.size() == 1);
        if (documentation.file_contents.size() == 1) {
          EXPECT(state,
              documentation.file_contents.front() ==
                  "record design bytes\n");
        }
        saw_record_docs = true;
      }
    }
    EXPECT(state, saw_library_docs);
    EXPECT(state, saw_record_docs);
  }

  // A dependency constant is part of its visible compact interface even when
  // the current expected type is unchanged. Editing it must stale the request
  // without manufacturing a new structural site identity.
  write_file(
      workspace / "lib" / "package.draft",
      "package lib\n"
      "docs \"Library design.\" file \"GUIDE.md\"\n"
      "pub Answer :: 43\n"
      "docs \"Record design.\" file \"RECORD.md\"\n"
      "pub Record :: struct {\n"
      "    count: u32,\n"
      "}\n");
  draft::SourceManager changed_sources;
  draft::DiagnosticSink changed_diagnostics;
  const draft::CompileWorkspaceResult changed =
      compile(changed_sources, changed_diagnostics);
  EXPECT(state, changed.ok);
  EXPECT(state, !changed_diagnostics.has_errors());
  const draft::AgentObligation *changed_synthesis = nullptr;
  if (changed.graph.root_package.is_valid() &&
      changed.graph.root_package.value < changed.packages.size() &&
      changed.packages[changed.graph.root_package.value].has_value()) {
    for (const draft::AgentObligation &obligation :
         changed.packages[changed.graph.root_package.value]
             ->obligations.obligations) {
      if (obligation.kind == draft::AgentConstructKind::SynthesisExpression) {
        changed_synthesis = &obligation;
      }
    }
  }
  EXPECT(state, changed_synthesis != nullptr);
  if (changed_synthesis != nullptr) {
    EXPECT(state,
        changed_synthesis->site_identity == initial_synthesis->site_identity);
    EXPECT(state,
        changed_synthesis->expected_type_digest ==
            initial_synthesis->expected_type_digest);
    EXPECT(state,
        changed_synthesis->input_digest != initial_synthesis->input_digest);
  }
}

void test_denied_imports_are_removed_from_usable_context(TestState &state) {
  TemporaryPackage temporary;
  const std::filesystem::path workspace = temporary.path / "denied-workspace";
  write_file(
      workspace / "lib" / "package.draft",
      "package lib\n"
      "docs \"Library design.\" file \"GUIDE.md\"\n"
      "docs \"Answer design.\" file \"ANSWER.md\"\n"
      "pub Answer :: 42\n"
      "docs \"Record design.\" file \"RECORD.md\"\n"
      "pub Record :: struct { count: u32, }\n");
  write_file(workspace / "lib" / "GUIDE.md", "library bytes\n");
  write_file(workspace / "lib" / "ANSWER.md", "answer bytes\n");
  write_file(workspace / "lib" / "RECORD.md", "record bytes\n");
  write_file(
      workspace / "app" / "package.draft",
      "package app\n"
      "import lib as lib\n"
      "member_denial :: proc() {\n"
      "    deny lib.Answer {\n"
      "        value: lib.Record = ... \"member denial\"\n"
      "    }\n"
      "}\n"
      "package_denial :: proc() {\n"
      "    deny lib {\n"
      "        value: lib.Record = ... \"package denial\"\n"
      "    }\n"
      "}\n");

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = workspace.string();
  const draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources,
      (workspace / "app").string(),
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, compiled.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, compiled.graph.root_package.is_valid());
  if (!compiled.graph.root_package.is_valid() ||
      compiled.graph.root_package.value >= compiled.packages.size() ||
      !compiled.packages[compiled.graph.root_package.value].has_value()) {
    return;
  }

  const draft::AgentObligation *member = nullptr;
  const draft::AgentObligation *whole_package = nullptr;
  for (const draft::AgentObligation &obligation :
       compiled.packages[compiled.graph.root_package.value]
           ->obligations.obligations) {
    if (obligation.kind !=
        draft::AgentConstructKind::SynthesisExpression) {
      continue;
    }
    if (obligation.anchor_name == "member_denial") member = &obligation;
    if (obligation.anchor_name == "package_denial") {
      whole_package = &obligation;
    }
  }
  EXPECT(state, member != nullptr);
  EXPECT(state, whole_package != nullptr);
  if (member != nullptr) {
    EXPECT(state, member->imported_packages.size() == 1);
    if (member->imported_packages.size() == 1) {
      const std::string &definition =
          member->imported_packages.front().definition;
      EXPECT(state,
          definition.find("DECLARATION_NAME 6\nAnswer") ==
              std::string::npos);
      EXPECT(state,
          definition.find("DECLARATION_NAME 6\nRecord") !=
              std::string::npos);
      bool saw_answer_docs = false;
      bool saw_record_docs = false;
      for (const draft::AgentDocumentationContext &documentation :
           member->imported_packages.front().documentation) {
        if (documentation.anchor_name == "Answer") saw_answer_docs = true;
        if (documentation.anchor_name == "Record") {
          EXPECT(state, documentation.file_contents.size() == 1);
          if (documentation.file_contents.size() == 1) {
            EXPECT(state,
                documentation.file_contents.front() == "record bytes\n");
          }
          saw_record_docs = true;
        }
      }
      EXPECT(state, !saw_answer_docs);
      EXPECT(state, saw_record_docs);
    }
  }
  if (whole_package != nullptr) {
    EXPECT(state, whole_package->imported_packages.empty());
  }
}

void test_source_definitions_follow_semantic_references(TestState &state) {
  TemporaryPackage temporary;
  write_file(
      temporary.path / "package.draft",
      R"draft(package relevant_context

Prompt_Value :: 7

middle :: proc(value: i64) -> i64 {
    leaf :: proc(item: i64) -> i64 {
        return item + 1
    }
    // This comment must not enter provider context.
    return leaf(value)
}

Selected :: middle

unrelated :: proc(value: i64) -> i64 {
    return value - 1
}

work :: proc() -> i64 {
    prior := Selected(1)
    judge "Prompt_Value remains positive"
    _ = prior
    return ... "combine the result with Prompt_Value"
}
)draft");

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::PackageLoadOptions load_options;
  load_options.file_tag = target.facts.file_tag;
  const draft::PackageLoadResult loaded = draft::load_package(
      sources, temporary.path.string(), load_options, diagnostics);
  draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
      sources, loaded.package, target.facts, diagnostics);
  const draft::PackageBodyWorkState bodies = draft::check_package_bodies(
      sources,
      loaded.package,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target.facts,
      diagnostics);
  const draft::AgentMetadataResult metadata = draft::collect_agent_metadata(
      sources, loaded.package, bodies.package, {}, diagnostics);
  const draft::AgentObligationResult obligations =
      draft::build_agent_obligations(
          {"workspace", "relevant_context"},
          sources,
          loaded.package,
          bodies.package,
          bodies.constants,
          metadata,
          draft::imported_procedure_contracts(bodies.package),
          target,
          diagnostics,
          {},
          bodies.procedures);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, loaded.ok);
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, metadata.ok);
  EXPECT(state, obligations.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, obligations.obligations.size() == 2);
  const draft::AgentObligation *judgment = nullptr;
  const draft::AgentObligation *synthesis = nullptr;
  for (const draft::AgentObligation &obligation : obligations.obligations) {
    if (obligation.kind == draft::AgentConstructKind::Judgment) {
      judgment = &obligation;
    }
    if (obligation.kind == draft::AgentConstructKind::SynthesisExpression) {
      synthesis = &obligation;
    }
  }
  EXPECT(state, judgment != nullptr);
  EXPECT(state, synthesis != nullptr);
  if (judgment == nullptr || synthesis == nullptr) return;

  bool saw_leaf_binding = false;
  bool saw_unrelated_binding = false;
  for (const draft::AgentVisibleBinding &binding :
       synthesis->visible_bindings) {
    if (binding.name == "leaf") saw_leaf_binding = true;
    if (binding.name == "unrelated") saw_unrelated_binding = true;
  }
  EXPECT(state, !saw_leaf_binding);
  EXPECT(state, saw_unrelated_binding);

  bool saw_leaf = false;
  bool saw_middle = false;
  bool saw_prompt_value = false;
  bool saw_selected = false;
  bool saw_unrelated_definition = false;
  for (const draft::AgentDeclarationContext &declaration :
       synthesis->relevant_declarations) {
    EXPECT(state, declaration.source_relative_path == "package.draft");
    EXPECT(state,
        draft::sha256(declaration.source) == declaration.source_digest);
    if (declaration.name == "leaf") {
      EXPECT(state,
          declaration.source.find("return item + 1") !=
              std::string::npos);
      saw_leaf = true;
    }
    if (declaration.name == "middle") {
      EXPECT(state,
          declaration.source.find("return leaf(value)") !=
              std::string::npos);
      EXPECT(state,
          declaration.source.find("comment must not enter") ==
              std::string::npos);
      saw_middle = true;
    }
    if (declaration.name == "Prompt_Value") {
      EXPECT(state, declaration.has_constant);
      saw_prompt_value = true;
    }
    if (declaration.name == "Selected") {
      EXPECT(state, declaration.has_constant);
      saw_selected = true;
    }
    if (declaration.name == "unrelated") saw_unrelated_definition = true;
  }
  EXPECT(state, saw_leaf);
  EXPECT(state, saw_middle);
  EXPECT(state, saw_prompt_value);
  EXPECT(state, saw_selected);
  EXPECT(state, !saw_unrelated_definition);

  // Judgments use the same compiler-checked closure. Their claim may add
  // prompt-selected roots, but it cannot turn unrelated declarations into
  // context or expose a hidden helper as a visible binding.
  bool judgment_saw_leaf = false;
  bool judgment_saw_middle = false;
  bool judgment_saw_prompt_value = false;
  bool judgment_saw_unrelated = false;
  for (const draft::AgentDeclarationContext &declaration :
       judgment->relevant_declarations) {
    if (declaration.name == "leaf") judgment_saw_leaf = true;
    if (declaration.name == "middle") judgment_saw_middle = true;
    if (declaration.name == "Prompt_Value") {
      judgment_saw_prompt_value = true;
    }
    if (declaration.name == "unrelated") judgment_saw_unrelated = true;
  }
  EXPECT(state, judgment_saw_leaf);
  EXPECT(state, judgment_saw_middle);
  EXPECT(state, judgment_saw_prompt_value);
  EXPECT(state, !judgment_saw_unrelated);

  struct RebuiltIdentity {
    bool ok = false;
    std::string site_identity;
    draft::Sha256Digest input_digest;
  };
  const auto rebuild_synthesis = [&]() {
    RebuiltIdentity result;
    draft::SourceManager rebuilt_sources;
    draft::DiagnosticSink rebuilt_diagnostics;
    const draft::PackageLoadResult rebuilt_loaded = draft::load_package(
        rebuilt_sources,
        temporary.path.string(),
        load_options,
        rebuilt_diagnostics);
    draft::SemanticAnalysisResult rebuilt_semantics =
        draft::analyze_package_semantics(
            rebuilt_sources,
            rebuilt_loaded.package,
            target.facts,
            rebuilt_diagnostics);
    const draft::PackageBodyWorkState rebuilt_bodies =
        draft::check_package_bodies(
            rebuilt_sources,
            rebuilt_loaded.package,
            rebuilt_semantics.selections,
            rebuilt_semantics.package,
            rebuilt_semantics.constants,
            target.facts,
            rebuilt_diagnostics);
    const draft::AgentMetadataResult rebuilt_metadata =
        draft::collect_agent_metadata(
            rebuilt_sources,
            rebuilt_loaded.package,
            rebuilt_bodies.package,
            {},
            rebuilt_diagnostics);
    const draft::AgentObligationResult rebuilt_obligations =
        draft::build_agent_obligations(
            {"workspace", "relevant_context"},
            rebuilt_sources,
            rebuilt_loaded.package,
            rebuilt_bodies.package,
            rebuilt_bodies.constants,
            rebuilt_metadata,
            draft::imported_procedure_contracts(rebuilt_bodies.package),
            target,
            rebuilt_diagnostics,
            {},
            rebuilt_bodies.procedures);
    if (rebuilt_diagnostics.has_errors()) {
      std::cerr << draft::render_diagnostics(
          rebuilt_sources, rebuilt_diagnostics);
    }
    result.ok = rebuilt_loaded.ok && rebuilt_semantics.ok &&
        rebuilt_bodies.ok && rebuilt_metadata.ok && rebuilt_obligations.ok &&
        !rebuilt_diagnostics.has_errors();
    for (const draft::AgentObligation &obligation :
         rebuilt_obligations.obligations) {
      if (obligation.kind !=
          draft::AgentConstructKind::SynthesisExpression) {
        continue;
      }
      result.site_identity = obligation.site_identity;
      result.input_digest = obligation.input_digest;
    }
    return result;
  };

  const std::string original_source =
      read_file(temporary.path / "package.draft");
  std::string changed_helper_source = original_source;
  const std::size_t helper_body = changed_helper_source.find("item + 1");
  EXPECT(state, helper_body != std::string::npos);
  if (helper_body != std::string::npos) {
    changed_helper_source.replace(helper_body, 8, "item + 2");
    write_file(temporary.path / "package.draft", changed_helper_source);
    const RebuiltIdentity changed_helper = rebuild_synthesis();
    EXPECT(state, changed_helper.ok);
    EXPECT(state, changed_helper.site_identity == synthesis->site_identity);
    EXPECT(state, changed_helper.input_digest != synthesis->input_digest);
  }

  std::string changed_unrelated_source = original_source;
  const std::size_t unrelated_body =
      changed_unrelated_source.find("value - 1");
  EXPECT(state, unrelated_body != std::string::npos);
  if (unrelated_body != std::string::npos) {
    changed_unrelated_source.replace(unrelated_body, 9, "value - 2");
    write_file(temporary.path / "package.draft", changed_unrelated_source);
    const RebuiltIdentity changed_unrelated = rebuild_synthesis();
    EXPECT(state, changed_unrelated.ok);
    EXPECT(state, changed_unrelated.site_identity == synthesis->site_identity);
    EXPECT(state, changed_unrelated.input_digest == synthesis->input_digest);
  }
  write_file(temporary.path / "package.draft", original_source);
}

void test_early_synthesis_receives_permitted_context(TestState &state) {
  TemporaryPackage temporary;
  write_file(
      temporary.path / "package.draft",
      "package early_context\n"
      "deny context.user_index {\n"
      "    ... \"declare helpers\"\n"
      "}\n"
      "Packet :: struct {\n"
      "    prefix: u32,\n"
      "    deny context.user_index {\n"
      "        ... \"declare fields\"\n"
      "    }\n"
      "}\n");
  write_file(
      temporary.path / "context_test.draft",
      "package early_context\n"
      "test_context :: proc() {\n"
      "}\n");

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::CompileWorkspaceOptions options;
  options.target = target;
  options.workspace.workspace_directory = temporary.path.string();
  options.stage = draft::CompileWorkspaceStage::DiscoverInterfaceSynthesis;
  const draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources, temporary.path.string(), std::move(options), diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, compiled.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, compiled.graph.root_package.is_valid());
  if (!compiled.ok || !compiled.graph.root_package.is_valid() ||
      compiled.graph.root_package.value >= compiled.packages.size() ||
      !compiled.packages[compiled.graph.root_package.value].has_value()) {
    return;
  }
  const draft::AgentObligationResult &obligations =
      compiled.packages[compiled.graph.root_package.value]->obligations;
  EXPECT(state, obligations.ok);
  EXPECT(state, obligations.obligations.size() == 2);
  bool saw_declaration = false;
  bool saw_member = false;
  for (const draft::AgentObligation &obligation : obligations.obligations) {
    EXPECT(state, obligation.validation_context.size() == 1);
    if (obligation.validation_context.size() == 1) {
      EXPECT(state, !obligation.validation_context.front().typing_complete);
      EXPECT(state,
          obligation.validation_context.front().procedures.empty());
    }
    EXPECT(state, obligation.context_fields.size() == 7);
    for (const draft::AgentContextField &field : obligation.context_fields) {
      EXPECT(state, field.name != "user_index");
    }
    if (obligation.kind ==
        draft::AgentConstructKind::SynthesisDeclaration) {
      saw_declaration = true;
    }
    if (obligation.kind == draft::AgentConstructKind::SynthesisMember) {
      saw_member = true;
      EXPECT(state, obligation.enclosing_declaration.present);
      EXPECT(state, obligation.enclosing_declaration.name == "Packet");
      EXPECT(state,
          obligation.enclosing_declaration.semantic_skeleton.find(
              "MEMBER_NAME 6\nprefix") != std::string::npos);
      EXPECT(state,
          obligation.enclosing_declaration.semantic_skeleton.find(
              "MEMBER_TYPE 3\nu32") != std::string::npos);
    }
  }
  EXPECT(state, saw_declaration);
  EXPECT(state, saw_member);
}

// A source-authored agent site in a static-pack loop is checked while the pack
// element and index are symbolic.  Building obligations must preserve those
// symbolic bindings and the enclosing dependent-when facts exactly once per
// site: later concrete procedure specializations may validate the branches,
// but they must not multiply provider work or change the provider's context.
void test_static_pack_sites_build_one_symbolic_obligation(TestState &state) {
  TemporaryPackage temporary;
  write_file(
      temporary.path / "package.draft",
      R"draft(package pack_agent

render :: proc(values: ..type) {
    for value, index in values {
        when type_of(value) == string {
            ... "render the string pack element"
        } else when type_kind(type_of(value)) == .signed_integer {
            judge "the signed pack element is handled"
        } else {
            static_assert(false, "unsupported pack element")
        }
    }
}

main :: proc() {
    render("draft", 1)
    // Equal ordered tail types reuse the specialization and must not create a
    // second agent record for either source-authored site.
    render("again", 2)
}
)draft");

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::PackageLoadOptions load_options;
  load_options.file_tag = target.facts.file_tag;
  const draft::PackageLoadResult loaded = draft::load_package(
      sources, temporary.path.string(), load_options, diagnostics);
  draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
      sources, loaded.package, target.facts, diagnostics);
  const draft::PackageBodyWorkState bodies = draft::check_package_bodies(
      sources,
      loaded.package,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target.facts,
      diagnostics);
  const draft::AgentMetadataResult metadata = draft::collect_agent_metadata(
      sources, loaded.package, bodies.package, {}, diagnostics);
  const draft::AgentObligationResult obligations =
      draft::build_agent_obligations(
          {"workspace", "pack_agent"},
          sources,
          loaded.package,
          bodies.package,
          bodies.constants,
          metadata,
          draft::imported_procedure_contracts(bodies.package),
          target,
          diagnostics,
          {},
          bodies.procedures);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, loaded.ok);
  EXPECT(state, semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, metadata.ok);
  EXPECT(state, obligations.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, metadata.records.size() == 2);
  EXPECT(state, obligations.obligations.size() == 2);

  const draft::AgentObligation *synthesis = nullptr;
  const draft::AgentObligation *judgment = nullptr;
  for (const draft::AgentObligation &obligation : obligations.obligations) {
    if (obligation.kind == draft::AgentConstructKind::SynthesisStatement) {
      synthesis = &obligation;
    } else if (obligation.kind == draft::AgentConstructKind::Judgment) {
      judgment = &obligation;
    }
  }
  EXPECT(state, synthesis != nullptr);
  EXPECT(state, judgment != nullptr);

  const auto check_symbolic_bindings = [&state](
                                           const draft::AgentObligation &item) {
    bool saw_value = false;
    bool saw_index = false;
    bool saw_pack = false;
    for (const draft::AgentVisibleBinding &binding : item.visible_bindings) {
      if (binding.name == "value") {
        EXPECT(state, binding.kind == draft::SymbolKind::Parameter);
        EXPECT(state,
            binding.type_text.find("render.values.element") !=
                std::string::npos);
        saw_value = true;
      } else if (binding.name == "index") {
        EXPECT(state, binding.kind == draft::SymbolKind::ValueParameter);
        EXPECT(state, binding.type_text == "usize");
        saw_index = true;
      } else if (binding.name == "values") {
        EXPECT(state, binding.kind == draft::SymbolKind::ValueParameter);
        EXPECT(state,
            binding.type_text.find("render.values.element") !=
                std::string::npos);
        saw_pack = true;
      }
    }
    EXPECT(state, saw_value);
    EXPECT(state, saw_index);
    EXPECT(state, saw_pack);
  };

  if (synthesis != nullptr) {
    check_symbolic_bindings(*synthesis);
    EXPECT(state, synthesis->branch_refinements.size() == 1);
    if (synthesis->branch_refinements.size() == 1) {
      const draft::AgentBranchRefinement &refinement =
          synthesis->branch_refinements.front();
      EXPECT(state,
          refinement.kind ==
              draft::AgentBranchRefinementKind::ConditionTrue);
      EXPECT(state,
          refinement.subject == "type_of(value) == string");
      EXPECT(state, refinement.type_text == "bool");
    }
  }
  if (judgment != nullptr) {
    check_symbolic_bindings(*judgment);
    EXPECT(state, judgment->branch_refinements.size() == 2);
    if (judgment->branch_refinements.size() == 2) {
      EXPECT(state,
          judgment->branch_refinements[0].kind ==
              draft::AgentBranchRefinementKind::ConditionFalse);
      EXPECT(state,
          judgment->branch_refinements[0].subject ==
              "type_of(value) == string");
      EXPECT(state,
          judgment->branch_refinements[1].kind ==
              draft::AgentBranchRefinementKind::ConditionTrue);
      EXPECT(state,
          judgment->branch_refinements[1].subject ==
              "type_kind(type_of(value)) == .signed_integer");
    }
  }
}

} // namespace

int main() {
  TestState state;
  test_agent_records(state);
  test_dangling_documentation_is_rejected(state);
  test_judgment_guidance_respects_branch_dominance(state);
  test_member_synthesis_receives_package_and_type_judgments(state);
  test_package_judgment_receives_complete_package_source(state);
  test_body_sites_receive_typed_branch_refinements(state);
  test_visible_import_interface_is_context(state);
  test_denied_imports_are_removed_from_usable_context(state);
  test_source_definitions_follow_semantic_references(state);
  test_early_synthesis_receives_permitted_context(state);
  test_static_pack_sites_build_one_symbolic_obligation(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " agent metadata expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all agent metadata tests passed\n";
  return EXIT_SUCCESS;
}
