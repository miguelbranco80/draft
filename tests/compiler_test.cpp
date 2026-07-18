// Dependency-ordered full provider-free compiler pipeline tests.

#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "compiler_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void test_multi_package_native_pipeline(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/packages";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/packages/app",
      std::move(options),
      diagnostics);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, result.graph.packages.size() == 2);
  EXPECT(state, result.packages.size() == 2);
  EXPECT(state, result.packages[0].has_value());
  EXPECT(state, result.packages[1].has_value());
  if (result.packages[0].has_value() && result.packages[1].has_value()) {
    EXPECT(state, result.packages[0]->llvm.ok);
    EXPECT(state, result.packages[1]->llvm.ok);
    EXPECT(state, result.packages[0]->llvm.text.find("define i32 @main") !=
        std::string::npos);
    EXPECT(state, result.packages[1]->llvm.text.find("define i32 @main") ==
        std::string::npos);
    EXPECT(state, result.packages[0]->llvm.text.find(
        "draft.workspace.lib_2Fmath.translate") != std::string::npos);
    EXPECT(state, result.packages[1]->llvm.text.find(
        "draft.workspace.lib_2Fmath.translate") != std::string::npos);
    EXPECT(state, result.packages[0]->llvm.text.find(
        "define hidden void @__draft.assert") != std::string::npos);
    EXPECT(state, result.packages[1]->llvm.text.find(
        "declare hidden void @__draft.assert") != std::string::npos);
  }
}

void test_hosted_entry_contract(TestState &state) {
  std::error_code error;
  const std::filesystem::path root =
      std::filesystem::temp_directory_path(error) / "draft-bootstrap-entry-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(root, error);
  error.clear();
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  // i32 is a perfectly valid ordinary result type, but the hosted entry
  // contract deliberately accepts only void or the target-native `int`.
  std::ofstream source(root / "app" / "package.draft", std::ios::binary);
  source << "package app\n"
            "main :: proc() -> i32 {\n"
            "    return 0\n"
            "}\n";
  source.close();
  EXPECT(state, source.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources, (root / "app").string(), std::move(options), diagnostics);
  EXPECT(state, !result.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  if (rendered.find("main result must be void or int") == std::string::npos) {
    std::cerr << rendered;
  }
  EXPECT(state, rendered.find("main result must be void or int") !=
      std::string::npos);

  std::filesystem::remove_all(root, error);
}

void test_compiler_distributed_core(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/core-runtime",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, result.graph.packages.size() == 5);
  if (result.graph.root_package.is_valid()) {
    const std::optional<draft::CompiledPackage> &root_package =
        result.packages[result.graph.root_package.value];
    EXPECT(state, root_package.has_value());
    if (root_package.has_value()) {
      EXPECT(state, root_package->llvm.text.find(
          "observe_5Fcontext\"(ptr %l0)") != std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "observe_5Fcontext\"(ptr %l1)") != std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "define hidden void @\"__draft.runtime.default_context\"") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "define internal ptr @__draft.default_allocator") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "%draft.runtime.Allocator { ptr @__draft.default_allocator, "
          "ptr null }") != std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "%draft.runtime.Logger { ptr @__draft.default_logger, ptr null }") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "%draft.runtime.RandomGenerator { ptr @__draft.default_random, "
          "ptr null }") != std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "%draft.runtime.Allocator { ptr @__draft.temp_allocator, "
          "ptr null }") != std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "define hidden void "
          "@\"__draft.runtime.reset_temporary_allocator\"") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "call i32 @pthread_key_create") != std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "@__draft.thread_context = internal thread_local global") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "call void @\"__draft.runtime.attach_thread\"()") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "call void @\"__draft.runtime.default_context\"") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "call i64 @\"draft.workspace.core_2Druntime."
          "add_5Fcontext_5Findex\"(ptr %l1, i64 2)") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "call void @\"__draft.runtime.call_with_context\"") ==
          std::string::npos);

      const draft::SemanticPackage &package = root_package->semantics.package;
      const std::optional<draft::SymbolId> observe =
          package.symbols.lookup_direct(package.package_scope, "observe_context");
      const std::optional<draft::SymbolId> read_from_c =
          package.symbols.lookup_direct(package.package_scope, "read_context_from_c");
      const std::optional<draft::SymbolId> add_index =
          package.symbols.lookup_direct(package.package_scope, "add_context_index");
      const std::optional<draft::SymbolId> main =
          package.symbols.lookup_direct(package.package_scope, "main");
      EXPECT(state, observe.has_value());
      EXPECT(state, read_from_c.has_value());
      EXPECT(state, add_index.has_value());
      EXPECT(state, main.has_value());
      if (observe && read_from_c && add_index && main) {
        const draft::ProcedureEffectSummary *c_summary =
            root_package->effects.find(*read_from_c);
        const draft::ProcedureEffectSummary *main_summary =
            root_package->effects.find(*main);
        EXPECT(state, c_summary != nullptr);
        EXPECT(state, main_summary != nullptr);
        if (c_summary != nullptr) {
          EXPECT(state, std::find(
              c_summary->direct_calls.begin(),
              c_summary->direct_calls.end(),
              *observe) != c_summary->direct_calls.end());
          EXPECT(state, std::none_of(
              c_summary->effects.begin(),
              c_summary->effects.end(),
              [](const draft::SemanticEffect &effect) {
                return effect.kind == draft::EffectKind::UnknownCall;
              }));
        }
        if (main_summary != nullptr) {
          EXPECT(state, std::find(
              main_summary->direct_calls.begin(),
              main_summary->direct_calls.end(),
              *add_index) != main_summary->direct_calls.end());
        }
      }
    }
  }

  // The source-visible Context and the entry shim intentionally duplicate one
  // versioned physical contract.  This fixed layout check makes drift fail in
  // the ordinary compiler suite instead of appearing as a callback crash.
  const draft::CompiledPackage *runtime = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (package.has_value() &&
        package->identity.root_identity == "draft-core-test-v1" &&
        package->identity.root_relative_path == "runtime") {
      runtime = &*package;
      break;
    }
  }
  EXPECT(state, runtime != nullptr);
  if (runtime == nullptr) return;
  EXPECT(state, runtime->native_interop.providers.size() == 1);
  if (runtime->native_interop.providers.size() == 1) {
    EXPECT(state, runtime->native_interop.providers.front() == "draft_runtime");
  }
  const std::optional<draft::SymbolId> context =
      runtime->semantics.package.symbols.lookup_direct(
          runtime->semantics.package.package_scope, "Context");
  EXPECT(state, context.has_value());
  if (!context.has_value()) return;
  const draft::Type &type = runtime->semantics.package.types.type(
      runtime->semantics.package.symbols.symbol(*context).type);
  EXPECT(state, type.layout.known);
  EXPECT(state, type.layout.size == 96);
  EXPECT(state, type.layout.alignment == 8);
  EXPECT(state, type.member_offsets == std::vector<std::uint64_t>({
      0, 16, 32, 40, 56, 72, 80, 88}));
}

void test_compiler_distributed_memory(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/core-memory",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, result.graph.packages.size() == 3);
  if (!result.ok || !result.graph.root_package.is_valid()) return;
  const std::optional<draft::CompiledPackage> &root =
      result.packages[result.graph.root_package.value];
  EXPECT(state, root.has_value());
  if (!root.has_value()) return;
  EXPECT(state, root->llvm.text.find(
      ".memory.allocate\"") != std::string::npos);
  EXPECT(state, root->llvm.text.find(
      ".memory.new_24mono_24") != std::string::npos);
  EXPECT(state,
      root->semantics.package.imported_procedure_instances.size() >= 9);
  EXPECT(state, root->llvm.text.find(
      "call ptr @realloc") != std::string::npos);
  EXPECT(state, root->llvm.text.find(
      "call i32 @posix_memalign") != std::string::npos);

  const draft::CompiledPackage *memory = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (package.has_value() &&
        package->identity.root_relative_path == "memory") {
      memory = &*package;
      break;
    }
  }
  EXPECT(state, memory != nullptr);
  if (memory != nullptr) {
    EXPECT(state,
        memory->semantics.package.parametric_instances.size() >= 11);
    EXPECT(state, memory->llvm.text.find(
        "@\"__draft.runtime.reset_temporary_allocator\"") !=
        std::string::npos);
    EXPECT(state, memory->llvm.text.find("arena_5Fprovider") !=
        std::string::npos);
    EXPECT(state, memory->llvm.text.find("@\"mmap\"") !=
        std::string::npos);
    EXPECT(state, memory->llvm.text.find("@\"mprotect\"") !=
        std::string::npos);
    EXPECT(state, memory->llvm.text.find("@\"munmap\"") !=
        std::string::npos);
  }
}

void test_compiler_distributed_array_and_support(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/core-array",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, result.graph.packages.size() == 9);
  if (!result.ok || !result.graph.root_package.is_valid()) return;

  const std::optional<draft::CompiledPackage> &root =
      result.packages[result.graph.root_package.value];
  EXPECT(state, root.has_value());
  if (!root.has_value()) return;
  EXPECT(state, root->llvm.ok);
  EXPECT(state, root->llvm.text.find(".array.append_24mono_24") !=
      std::string::npos);
  EXPECT(state, root->llvm.text.find(".heap.allocator\"") !=
      std::string::npos);
  EXPECT(state,
      root->semantics.package.imported_procedure_instances.size() == 5);

  const draft::CompiledPackage *array = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (package.has_value() &&
        package->identity.root_relative_path == "array") {
      array = &*package;
      break;
    }
  }
  EXPECT(state, array != nullptr);
  if (array != nullptr) {
    EXPECT(state, array->llvm.ok);
    EXPECT(state, array->semantics.package.parametric_instances.size() >= 5);
  }
}

void test_compiler_distributed_map(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/core-map",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, result.graph.packages.size() == 5);
  if (!result.ok || !result.graph.root_package.is_valid()) return;

  const std::optional<draft::CompiledPackage> &root =
      result.packages[result.graph.root_package.value];
  EXPECT(state, root.has_value());
  if (!root.has_value()) return;
  EXPECT(state, root->llvm.ok);
  EXPECT(state, root->llvm.text.find(".map.set_24mono_24") !=
      std::string::npos);
  EXPECT(state, root->llvm.text.find(".map.string_5Fkeys") !=
      std::string::npos);
  EXPECT(state,
      root->semantics.package.imported_procedure_instances.size() == 5);

  const draft::CompiledPackage *map = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (package.has_value() && package->identity.root_relative_path == "map") {
      map = &*package;
      break;
    }
  }
  EXPECT(state, map != nullptr);
  if (map != nullptr) {
    EXPECT(state, map->llvm.ok);
    EXPECT(state, map->llvm.text.find("hash_5Fstring") != std::string::npos);
    EXPECT(state, map->semantics.package.parametric_instances.size() >= 10);
  }
}

void test_compiler_distributed_os(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/core-os",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, result.graph.packages.size() == 4);
  if (!result.ok || !result.graph.root_package.is_valid()) return;

  const std::optional<draft::CompiledPackage> &root =
      result.packages[result.graph.root_package.value];
  EXPECT(state, root.has_value());
  if (!root.has_value()) return;
  EXPECT(state, root->llvm.ok);
  EXPECT(state, root->llvm.text.find(
      "define i32 @main(i32 %argc, ptr %argv, ptr %envp)") !=
      std::string::npos);
  EXPECT(state, root->llvm.text.find(
      "define hidden ptr @\"__draft.os.args_data\"") !=
      std::string::npos);
  EXPECT(state, root->llvm.text.find("@strlen") != std::string::npos);

  const draft::CompiledPackage *os = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (package.has_value() && package->identity.root_relative_path == "os") {
      os = &*package;
      break;
    }
  }
  EXPECT(state, os != nullptr);
  if (os != nullptr) {
    EXPECT(state, os->llvm.ok);
    EXPECT(state, os->llvm.text.find("@\"__draft.os.args_data\"") !=
        std::string::npos);
    EXPECT(state, os->llvm.text.find("@\"getpid\"") != std::string::npos);
    EXPECT(state, os->native_interop.providers.size() == 2);
  }
}

void test_compiler_distributed_thread(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/core-thread",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, result.graph.packages.size() == 6);
  if (!result.ok || !result.graph.root_package.is_valid()) return;

  const std::optional<draft::CompiledPackage> &root =
      result.packages[result.graph.root_package.value];
  EXPECT(state, root.has_value());
  if (!root.has_value()) return;
  EXPECT(state, root->llvm.ok);
  EXPECT(state, root->llvm.text.find(
      "define hidden void @\"__draft.runtime.install_thread_context\"") !=
      std::string::npos);

  const draft::CompiledPackage *thread = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (package.has_value() &&
        package->identity.root_relative_path == "thread") {
      thread = &*package;
      break;
    }
  }
  EXPECT(state, thread != nullptr);
  if (thread != nullptr) {
    EXPECT(state, thread->llvm.ok);
    EXPECT(state, thread->llvm.text.find("@\"pthread_create\"") !=
        std::string::npos);
    EXPECT(state, thread->llvm.text.find(
        "@\"__draft.runtime.install_thread_context\"") !=
        std::string::npos);
    EXPECT(state, thread->llvm.text.find("@\"pthread_mutex_lock\"") !=
        std::string::npos);
    EXPECT(state, thread->llvm.text.find(
        "@\"__draft.runtime.default_context\"") != std::string::npos);
  }
}

void test_compiler_distributed_atomic(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/core-atomic",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, result.graph.packages.size() == 2);
  if (!result.ok || !result.graph.root_package.is_valid()) return;

  const std::optional<draft::CompiledPackage> &root =
      result.packages[result.graph.root_package.value];
  EXPECT(state, root.has_value());
  if (!root.has_value()) return;
  const std::string &llvm = root->llvm.text;
  EXPECT(state, root->llvm.ok);
  EXPECT(state, llvm.find("store atomic i64 2") != std::string::npos);
  EXPECT(state, llvm.find("load atomic i64") != std::string::npos);
  EXPECT(state, llvm.find("atomicrmw add") != std::string::npos);
  EXPECT(state, llvm.find("atomicrmw sub") != std::string::npos);
  EXPECT(state, llvm.find("atomicrmw and") != std::string::npos);
  EXPECT(state, llvm.find("atomicrmw or") != std::string::npos);
  EXPECT(state, llvm.find("atomicrmw xor") != std::string::npos);
  EXPECT(state, llvm.find("atomicrmw xchg") != std::string::npos);
  EXPECT(state, llvm.find("cmpxchg ptr") != std::string::npos);
  EXPECT(state, llvm.find("fence seq_cst") != std::string::npos);
  EXPECT(state, llvm.find("relaxed atomic fence has no effect") !=
      std::string::npos);
  EXPECT(state, llvm.find("<type-parameter>") == std::string::npos);
}

void test_atomic_diagnostics(TestState &state) {
  std::error_code error;
  const std::filesystem::path root = std::filesystem::temp_directory_path(error) /
      "draft-bootstrap-atomic-diagnostics-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(root, error);
  error.clear();
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  std::ofstream source(root / "app" / "package.draft", std::ios::binary);
  source << "package app\n"
            "import core/atomic\n"
            "word: atomic.Value[u64]\n"
            "pointer_word: atomic.Value[rawptr]\n"
            "main :: proc() {\n"
            "    order: atomic.Order = .relaxed\n"
            "    loader := atomic.load\n"
            "    atomic.load(&word, order)\n"
            "    atomic.load(&word, .release)\n"
            "    atomic.store(&word, 1, .acquire)\n"
            "    expected: u64 = 0\n"
            "    atomic.compare_exchange(\n"
            "        &word, &expected, 1, .relaxed, .acquire)\n"
            "    atomic.fetch_add(&pointer_word, nil, .relaxed)\n"
            "    atomic.fence(.relaxed)\n"
            "}\n";
  source.close();
  EXPECT(state, source.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources, (root / "app").string(), std::move(options), diagnostics);
  EXPECT(state, !result.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  EXPECT(state, rendered.find("must be a compile-time core/atomic.Order value") !=
      std::string::npos);
  EXPECT(state, rendered.find("core/atomic operations must be called directly") !=
      std::string::npos);
  EXPECT(state, rendered.find("atomic load cannot use a release order") !=
      std::string::npos);
  EXPECT(state, rendered.find("atomic store cannot use an acquire order") !=
      std::string::npos);
  EXPECT(state, rendered.find(
      "compare-exchange failure order is stronger than its success order") !=
      std::string::npos);
  EXPECT(state, rendered.find("atomic fetch operation requires an integer type") !=
      std::string::npos);

  std::filesystem::remove_all(root, error);
}

void test_cross_package_generic_procedures(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/packages-generic";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/packages-generic/app",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, result.graph.packages.size() == 4);

  const draft::CompiledPackage *app = nullptr;
  const draft::CompiledPackage *left = nullptr;
  const draft::CompiledPackage *right = nullptr;
  const draft::CompiledPackage *generic = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (!package.has_value()) continue;
    if (package->identity.root_relative_path == "app") app = &*package;
    if (package->identity.root_relative_path == "lib/left") left = &*package;
    if (package->identity.root_relative_path == "lib/right") right = &*package;
    if (package->identity.root_relative_path == "lib/generic") generic = &*package;
  }
  EXPECT(state, app != nullptr);
  EXPECT(state, left != nullptr);
  EXPECT(state, right != nullptr);
  EXPECT(state, generic != nullptr);
  if (app == nullptr || left == nullptr || right == nullptr || generic == nullptr) {
    return;
  }

  // The app requests a private nominal, u64, byte-size, and composed type/value
  // template specializations in left. The concrete left bodies publish
  // transitive identity[Private_Value] and plus_one[4] requests to generic.
  // Both sibling packages also request identity[u64] and identity[Shared_Value].
  // The latter is local in left and imported in right, so both local display
  // spellings must hash to one canonical nominal identity. All sibling requests
  // must share owner symbols even though lib/generic was discovered before the
  // second sibling in the physical workspace traversal.
  EXPECT(state,
      app->semantics.package.imported_procedure_instances.size() == 5);
  EXPECT(state,
      left->semantics.package.imported_procedure_instances.size() == 4);
  EXPECT(state,
      right->semantics.package.imported_procedure_instances.size() == 2);
  EXPECT(state, generic->semantics.package.parametric_instances.size() == 5);
  EXPECT(state, left->semantics.package.parametric_instances.size() == 2);
  EXPECT(state, app->llvm.text.find("_24mono_24") != std::string::npos);
  EXPECT(state, left->llvm.text.find("_24mono_24") != std::string::npos);
  EXPECT(state, right->llvm.text.find("_24mono_24") != std::string::npos);
  EXPECT(state, generic->llvm.text.find("define") != std::string::npos);
  EXPECT(state, generic->llvm.text.find("_24mono_24") != std::string::npos);
}

void test_runtime_context_bridge_diagnostics(TestState &state) {
  std::error_code error;
  const std::filesystem::path root = std::filesystem::temp_directory_path(error) /
      "draft-bootstrap-context-bridge-test";
  EXPECT(state, !error);
  std::filesystem::remove_all(root, error);
  error.clear();
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  std::ofstream source(root / "app" / "package.draft", std::ios::binary);
  source << "package app\n"
            "import core/runtime\n"
            "callback :: proc() {}\n"
            "c_callback :: c proc() {}\n"
            "bad :: proc(value: ^runtime.Context) {\n"
            "    runtime.call_with_context(nil, callback)\n"
            "    runtime.call_with_context(value, c_callback)\n"
            "    runtime.call_with_context(value, callback, 1)\n"
            "}\n";
  source.close();
  EXPECT(state, source.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources, (root / "app").string(), std::move(options), diagnostics);
  EXPECT(state, !result.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  EXPECT(state, rendered.find("requires a non-nil Context pointer") !=
      std::string::npos);
  EXPECT(state, rendered.find("must use the Draft calling convention") !=
      std::string::npos);
  EXPECT(state, rendered.find("callback argument count does not match") !=
      std::string::npos);

  std::filesystem::remove_all(root, error);
}

} // namespace

int main() {
  TestState state;
  test_multi_package_native_pipeline(state);
  test_hosted_entry_contract(state);
  test_compiler_distributed_core(state);
  test_compiler_distributed_memory(state);
  test_compiler_distributed_array_and_support(state);
  test_compiler_distributed_map(state);
  test_compiler_distributed_os(state);
  test_compiler_distributed_thread(state);
  test_compiler_distributed_atomic(state);
  test_atomic_diagnostics(state);
  test_cross_package_generic_procedures(state);
  test_runtime_context_bridge_diagnostics(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " compiler pipeline expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all compiler pipeline tests passed\n";
  return EXIT_SUCCESS;
}
