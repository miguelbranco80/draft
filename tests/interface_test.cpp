// Public package-interface extraction and imported-name semantic tests.
//
// The test analyzes a dependency first, publishes only its `pub` declarations,
// then checks a consumer using qualified types, constants, procedures, aggregate
// members, and a dependency-selected `when`. It also verifies that an imported
// nominal retains its original package identity when exposed through another
// package's public procedure signature.

#include "sema/body_checker.h"
#include "sema/interface.h"
#include "sema/semantic.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/parser.h"
#include "target/profile.h"
#include "workspace/package.h"

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
      std::cerr << "interface_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

draft::LoadedPackage parse_package(
    draft::SourceManager &sources,
    draft::DiagnosticSink &diagnostics,
    std::string path,
    std::string short_name,
    std::string text) {
  draft::LoadedPackage package;
  package.short_name = std::move(short_name);
  package.physical_directory = "/virtual/" + package.short_name;
  draft::LoadedPackageFile file;
  file.kind = draft::PackageFileKind::DraftSource;
  file.relative_name = "package.draft";
  file.source = sources.add_source(std::move(path), std::move(text));
  file.syntax.emplace(draft::parse_source_file(sources, file.source, diagnostics));
  package.files.push_back(std::move(file));
  return package;
}

std::optional<draft::SyntaxReference> first_import(const draft::LoadedPackage &package) {
  if (package.files.empty() || !package.files.front().syntax.has_value()) {
    return std::nullopt;
  }
  const draft::SyntaxTree &tree = *package.files.front().syntax;
  const draft::SyntaxNode &root = tree.node(tree.root());
  for (draft::NodeId child : root.children) {
    if (tree.node(child).kind == draft::NodeKind::ImportClause) {
      return draft::SyntaxReference{tree.file(), child};
    }
  }
  return std::nullopt;
}

void test_imported_public_semantics(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();

  draft::LoadedPackage dependency = parse_package(
      sources,
      diagnostics,
      "math/package.draft",
      "math",
      R"draft(package math

pub Count :: 7

pub Point :: struct {
    x: i64,
    y: i64,
}

pub add :: proc(a, b: i64) -> i64 {
    return a + b
}

Hidden :: struct { value: u64, }
)draft");
  draft::SemanticAnalysisResult dependency_semantics =
      draft::analyze_package_semantics(
          sources, dependency, target.facts, diagnostics);
  const draft::PackageIdentity dependency_identity{"workspace", "lib/math"};
  draft::PackageInterface dependency_interface = draft::build_package_interface(
      dependency_identity,
      dependency_semantics.package,
      dependency_semantics.constants,
      diagnostics);

  draft::LoadedPackage consumer = parse_package(
      sources,
      diagnostics,
      "app/package.draft",
      "app",
      R"draft(package app

import lib/math as math

when math.Count == 7 {
    Selected :: i64
} else {
    Selected :: Does_Not_Exist
}

pub echo :: proc(point: math.Point) -> math.Point {
    copy := math.Point{x = point.x, y = math.Count}
    return copy
}

main :: proc() {
    value := math.add(math.Count, 1)
    assert(value == 8)
}
)draft");
  const std::optional<draft::SyntaxReference> import = first_import(consumer);
  EXPECT(state, import.has_value());
  if (!import.has_value()) {
    return;
  }
  draft::AvailablePackageImports available;
  available.entries.push_back({*import, &dependency_interface});
  draft::SemanticAnalysisResult consumer_semantics =
      draft::analyze_package_semantics(
          sources, consumer, target.facts, available, diagnostics);
  draft::BodyCheckResult bodies = draft::check_package_bodies(
      sources,
      consumer,
      consumer_semantics.selections,
      consumer_semantics.package,
      consumer_semantics.constants,
      diagnostics);
  const draft::PackageIdentity consumer_identity{"workspace", "app"};
  draft::PackageInterface consumer_interface = draft::build_package_interface(
      consumer_identity,
      consumer_semantics.package,
      consumer_semantics.constants,
      diagnostics);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, dependency_semantics.ok);
  EXPECT(state, dependency_interface.declarations.size() == 3);
  EXPECT(state, consumer_semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, bodies.checked_procedures == 2);
  EXPECT(state, consumer_semantics.package.imported_symbols.size() == 3);
  EXPECT(state, consumer_semantics.package.imported_types.size() == 1);
  EXPECT(state, consumer_interface.declarations.size() == 1);
  EXPECT(state, !diagnostics.has_errors());

  if (consumer_interface.declarations.empty()) {
    return;
  }
  const draft::InterfaceDeclaration &echo = consumer_interface.declarations.front();
  EXPECT(state, echo.name == "echo");
  EXPECT(state, echo.type.is_valid());
  if (!echo.type.is_valid()) {
    return;
  }
  const draft::InterfaceType &procedure = consumer_interface.types[echo.type.value];
  EXPECT(state, procedure.kind == draft::TypeKind::Procedure);
  EXPECT(state, procedure.members.size() == 2);
  if (procedure.members.size() == 2) {
    const draft::InterfaceType &point =
        consumer_interface.types[procedure.members.front().value];
    EXPECT(state, point.nominal_root_identity == "workspace");
    EXPECT(state, point.nominal_root_relative_path == "lib/math");
    EXPECT(state, point.nominal_public_name == "Point");
  }
}

void test_private_name_is_not_imported(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::LoadedPackage dependency = parse_package(
      sources,
      diagnostics,
      "dep/package.draft",
      "dep",
      "package dep\npub Visible :: u64\nHidden :: u64\n");
  draft::SemanticAnalysisResult dependency_semantics =
      draft::analyze_package_semantics(sources, dependency, target.facts, diagnostics);
  draft::PackageInterface dependency_interface = draft::build_package_interface(
      {"workspace", "dep"},
      dependency_semantics.package,
      dependency_semantics.constants,
      diagnostics);
  draft::LoadedPackage consumer = parse_package(
      sources,
      diagnostics,
      "use/package.draft",
      "use",
      "package use\nimport dep\nbad :: proc(value: dep.Hidden) {}\n");
  const std::optional<draft::SyntaxReference> import = first_import(consumer);
  EXPECT(state, import.has_value());
  if (!import.has_value()) {
    return;
  }
  draft::AvailablePackageImports available;
  available.entries.push_back({*import, &dependency_interface});
  const draft::SemanticAnalysisResult semantics = draft::analyze_package_semantics(
      sources, consumer, target.facts, available, diagnostics);

  EXPECT(state, !semantics.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  EXPECT(state, rendered.find("no public type named 'Hidden'") != std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_imported_public_semantics(state);
  test_private_name_is_not_imported(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " interface test expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all interface tests passed\n";
  return EXIT_SUCCESS;
}
