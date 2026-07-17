// Package declaration collection tests.
//
// The package is assembled in memory so the test can directly exercise the
// semantic boundary without duplicating filesystem-loader coverage. Two files
// prove that imports are file-local while declarations share one package scope.
// The source also checks nominal type allocation, documentation attachment,
// deferred `when`, transparent `deny`, and native linkage metadata.

#include "sema/analyzer.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/parser.h"
#include "workspace/package.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "declaration_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void add_file(
    draft::SourceManager &sources,
    draft::LoadedPackage &package,
    draft::DiagnosticSink &diagnostics,
    std::string name,
    std::string text) {
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = name;
  file.source = sources.add_source(name, std::move(text));
  file.syntax.emplace(draft::parse_source_file(sources, file.source, diagnostics));
  package.files.push_back(std::move(file));
}

[[nodiscard]] std::size_t count_sites(
    const draft::SemanticPackage &package, draft::SemanticSiteKind kind) {
  std::size_t count = 0;
  for (const draft::SemanticSite &site : package.sites) {
    if (site.kind == kind) {
      ++count;
    }
  }
  return count;
}

void test_package_collection(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "demo";
  loaded.physical_directory = "/virtual/demo";

  add_file(sources, loaded, diagnostics, "a.draft", R"draft(
package demo

docs "Package-level design."

import core/io as io

docs "Public generic type."
pub Pair[T: type] :: struct {
    value: T,
}

deny asm {
    Safe_Value :: 7
}

when target.pointer_bits == 64 {
    Word :: u64
} else {
    Word :: u32
}

foreign libc {
    puts :: c "puts" proc(message: cstring) -> int
}
)draft");

  add_file(sources, loaded, diagnostics, "b.draft", R"draft(
package demo

import core/c as c

judge "The exported entry has a stable C boundary."

export entry :: c "draft_entry" proc() -> int {
    return 0
}

... "generate another declaration"
)draft");

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, !diagnostics.has_errors());

  draft::SemanticPackage package =
      draft::collect_package_declarations(sources, loaded, diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, package.short_name == "demo");
  EXPECT(state, package.files.size() == 2);
  EXPECT(state, package.imports.size() == 2);
  EXPECT(state, package.symbols.scope_count() == 3);

  const std::optional<draft::SymbolId> pair =
      package.symbols.lookup_direct(package.package_scope, "Pair");
  const std::optional<draft::SymbolId> safe =
      package.symbols.lookup_direct(package.package_scope, "Safe_Value");
  const std::optional<draft::SymbolId> word =
      package.symbols.lookup_direct(package.package_scope, "Word");
  const std::optional<draft::SymbolId> puts =
      package.symbols.lookup_direct(package.package_scope, "puts");
  const std::optional<draft::SymbolId> entry =
      package.symbols.lookup_direct(package.package_scope, "entry");
  EXPECT(state, pair.has_value());
  EXPECT(state, safe.has_value());
  EXPECT(state, !word.has_value());
  EXPECT(state, puts.has_value());
  EXPECT(state, entry.has_value());

  if (pair.has_value()) {
    const draft::Symbol &symbol = package.symbols.symbol(*pair);
    EXPECT(state, symbol.kind == draft::SymbolKind::Type);
    EXPECT(state, symbol.visibility == draft::Visibility::Public);
    EXPECT(state, symbol.flags.parametric);
    EXPECT(state, symbol.type.is_valid());
    if (symbol.type.is_valid()) {
      EXPECT(state, package.types.type(symbol.type).kind == draft::TypeKind::Struct);
      EXPECT(state, !package.types.type(symbol.type).layout.known);
    }
  }
  if (puts.has_value()) {
    EXPECT(state, package.symbols.symbol(*puts).kind == draft::SymbolKind::Procedure);
    EXPECT(state, package.symbols.symbol(*puts).flags.foreign);
  }
  if (entry.has_value()) {
    EXPECT(state, package.symbols.symbol(*entry).kind == draft::SymbolKind::Procedure);
    EXPECT(state, package.symbols.symbol(*entry).flags.exported);
  }

  EXPECT(state, package.symbols.lookup(package.files[0].scope, "io").has_value());
  EXPECT(state, !package.symbols.lookup(package.files[0].scope, "c").has_value());
  EXPECT(state, package.symbols.lookup(package.files[1].scope, "c").has_value());
  EXPECT(state, !package.symbols.lookup(package.files[1].scope, "io").has_value());

  EXPECT(state, count_sites(package, draft::SemanticSiteKind::Documentation) == 2);
  EXPECT(state, count_sites(package, draft::SemanticSiteKind::Judgment) == 1);
  EXPECT(state, count_sites(package, draft::SemanticSiteKind::SynthesisDeclaration) == 1);
  EXPECT(state, count_sites(package, draft::SemanticSiteKind::ConditionalDeclaration) == 1);
  EXPECT(state, count_sites(package, draft::SemanticSiteKind::DenialDeclaration) == 1);
  EXPECT(state, package.native_bindings.size() == 2);
  if (package.native_bindings.size() == 2) {
    EXPECT(state, package.native_bindings[0].provider == "libc");
    EXPECT(state, package.native_bindings[0].linker_name_spelling == "\"puts\"");
    EXPECT(state, package.native_bindings[1].linker_name_spelling == "\"draft_entry\"");
  }

  bool saw_pair_documentation = false;
  for (const draft::SemanticSite &site : package.sites) {
    if (site.kind == draft::SemanticSiteKind::Documentation && pair.has_value() &&
        site.anchor == *pair) {
      saw_pair_documentation = true;
    }
  }
  EXPECT(state, saw_pair_documentation);
}

void test_cross_file_duplicate(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "duplicate";
  add_file(sources, loaded, diagnostics, "a.draft", "package duplicate\nValue :: 1\n");
  add_file(sources, loaded, diagnostics, "b.draft", "package duplicate\nValue :: 2\n");
  EXPECT(state, !diagnostics.has_errors());

  const draft::SemanticPackage package =
      draft::collect_package_declarations(sources, loaded, diagnostics);
  EXPECT(state, diagnostics.error_count() == 1);
  EXPECT(state, package.symbols.symbol_count() == 1);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  EXPECT(state, rendered.find("duplicate declaration") != std::string::npos);
  EXPECT(state, rendered.find("previous declaration") != std::string::npos);
}

void test_duplicate_nominal_does_not_allocate_type(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  loaded.short_name = "duplicate_type";
  add_file(
      sources,
      loaded,
      diagnostics,
      "a.draft",
      "package duplicate_type\nItem :: struct { value: u32, }\n");
  add_file(
      sources,
      loaded,
      diagnostics,
      "b.draft",
      "package duplicate_type\nItem :: struct { other: u64, }\n");
  EXPECT(state, !diagnostics.has_errors());

  const draft::SemanticPackage package =
      draft::collect_package_declarations(sources, loaded, diagnostics);
  EXPECT(state, diagnostics.error_count() == 1);
  std::size_t item_type_count = 0;
  for (std::size_t index = 0; index < package.types.size(); ++index) {
    const draft::Type &type = package.types.type(
        draft::TypeId{static_cast<std::uint32_t>(index)});
    if (type.kind == draft::TypeKind::Struct && type.name == "Item") {
      ++item_type_count;
    }
  }
  EXPECT(state, item_type_count == 1);
}

} // namespace

int main() {
  TestState state;
  test_package_collection(state);
  test_cross_file_duplicate(state);
  test_duplicate_nominal_does_not_allocate_type(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " declaration collection expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all declaration collection tests passed\n";
  return EXIT_SUCCESS;
}
