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
    }
    EXPECT(state, saw_values);
    EXPECT(state, saw_callback);
    EXPECT(state, saw_shadowed_blocked);
    EXPECT(state, !saw_denied_secret);
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
        const draft::BodyCheckResult member_bodies =
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
                member_semantics.package,
                policy,
                member_diagnostics);
        const draft::AgentObligationResult member_obligations =
            draft::build_agent_obligations(
                {"workspace", "context"},
                member_sources,
                member_loaded.package,
                member_semantics.package,
                member_metadata,
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
  const draft::BodyCheckResult bodies = draft::check_package_bodies(
      sources,
      loaded.package,
      semantics.selections,
      semantics.package,
      semantics.constants,
      target.facts,
      diagnostics);
  const draft::AgentMetadataResult metadata = draft::collect_agent_metadata(
      sources, loaded.package, semantics.package, {}, diagnostics);
  const draft::AgentObligationResult obligations =
      draft::build_agent_obligations(
          {"workspace", "judgment_context"},
          sources,
          loaded.package,
          semantics.package,
          metadata,
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

void test_visible_import_interface_is_context(TestState &state) {
  TemporaryPackage temporary;
  const std::filesystem::path workspace = temporary.path / "workspace";
  write_file(
      workspace / "lib" / "package.draft",
      "package lib\n"
      "pub Answer :: 42\n"
      "pub Record :: struct {\n"
      "    count: u32,\n"
      "}\n");
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
  }

  // A dependency constant is part of its visible compact interface even when
  // the current expected type is unchanged. Editing it must stale the request
  // without manufacturing a new structural site identity.
  write_file(
      workspace / "lib" / "package.draft",
      "package lib\n"
      "pub Answer :: 43\n"
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
      "pub Answer :: 42\n"
      "pub Record :: struct { count: u32, }\n");
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
    }
  }
  if (whole_package != nullptr) {
    EXPECT(state, whole_package->imported_packages.empty());
  }
}

void test_early_synthesis_receives_permitted_context(TestState &state) {
  TemporaryPackage temporary;
  write_file(
      temporary.path / "package.draft",
      "package early_context\n"
      "deny context.user_index {\n"
      "    ... \"declare helpers\"\n"
      "}\n");

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::PackageLoadOptions load_options;
  load_options.file_tag = target.facts.file_tag;
  const draft::PackageLoadResult loaded = draft::load_package(
      sources, temporary.path.string(), load_options, diagnostics);
  draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
      sources, loaded.package, target.facts, diagnostics);
  const draft::AgentMetadataResult metadata = draft::collect_agent_metadata(
      sources,
      loaded.package,
      semantics.package,
      {},
      diagnostics);
  const draft::AgentObligationResult obligations =
      draft::build_agent_obligations(
          {"workspace", "early_context"},
          sources,
          loaded.package,
          semantics.package,
          metadata,
          target,
          diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, loaded.ok);
  EXPECT(state, semantics.ok);
  EXPECT(state, metadata.ok);
  EXPECT(state, obligations.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, obligations.obligations.size() == 1);
  if (obligations.obligations.size() == 1) {
    const draft::AgentObligation &obligation = obligations.obligations.front();
    EXPECT(state,
        obligation.kind ==
            draft::AgentConstructKind::SynthesisDeclaration);
    EXPECT(state, obligation.context_fields.size() == 7);
    for (const draft::AgentContextField &field : obligation.context_fields) {
      EXPECT(state, field.name != "user_index");
    }
  }
}

} // namespace

int main() {
  TestState state;
  test_agent_records(state);
  test_dangling_documentation_is_rejected(state);
  test_judgment_guidance_respects_branch_dominance(state);
  test_visible_import_interface_is_context(state);
  test_denied_imports_are_removed_from_usable_context(state);
  test_early_synthesis_receives_permitted_context(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " agent metadata expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all agent metadata tests passed\n";
  return EXIT_SUCCESS;
}
