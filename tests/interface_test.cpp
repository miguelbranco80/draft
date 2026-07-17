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

pub C_Point :: @repr(C) @align(16) struct {
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
      target.facts,
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
  EXPECT(state, dependency_interface.declarations.size() == 4);
  EXPECT(state, consumer_semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, bodies.checked_procedures == 2);
  EXPECT(state, consumer_semantics.package.imported_symbols.size() == 4);
  EXPECT(state, consumer_semantics.package.imported_types.size() == 2);
  EXPECT(state, consumer_interface.declarations.size() == 1);
  EXPECT(state, !diagnostics.has_errors());

  bool saw_c_point = false;
  for (const draft::ImportedSymbol &imported :
       consumer_semantics.package.imported_symbols) {
    if (imported.public_name != "C_Point") continue;
    saw_c_point = true;
    const draft::Type &type = consumer_semantics.package.types.type(
        consumer_semantics.package.symbols.symbol(imported.proxy).type);
    EXPECT(state, type.c_representation);
    EXPECT(state, type.requested_alignment == 16);
    EXPECT(state, type.layout == draft::TypeLayout({true, 16, 16}));
  }
  EXPECT(state, saw_c_point);

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

void test_imported_parametric_type(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();

  draft::LoadedPackage dependency = parse_package(
      sources,
      diagnostics,
      "option/package.draft",
      "option",
      R"draft(package option

pub Maybe[T: type] :: union {
    none,
    some: T,
}
)draft");
  draft::SemanticAnalysisResult dependency_semantics =
      draft::analyze_package_semantics(
          sources, dependency, target.facts, diagnostics);
  draft::PackageInterface dependency_interface = draft::build_package_interface(
      {"workspace", "lib/option"},
      dependency_semantics.package,
      dependency_semantics.constants,
      diagnostics);

  // The consumer sees no dependency syntax. Resolving the public field must
  // rebuild Maybe's parameter scope from the interface and instantiate its
  // concrete tagged-union member layout locally.
  draft::LoadedPackage consumer = parse_package(
      sources,
      diagnostics,
      "middle/package.draft",
      "middle",
      R"draft(package middle

import lib/option as option

pub Holder :: struct {
    value: option.Maybe[i64],
}

pub unwrap :: proc(value: option.Maybe[i64]) -> i64 {
    switch value {
    case .some(payload):
        return payload
    case .none:
        return 0
    }
}
)draft");
  const std::optional<draft::SyntaxReference> import = first_import(consumer);
  EXPECT(state, import.has_value());
  if (!import.has_value()) return;
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
      target.facts,
      diagnostics);
  draft::PackageInterface consumer_interface = draft::build_package_interface(
      {"workspace", "middle"},
      consumer_semantics.package,
      consumer_semantics.constants,
      diagnostics);

  draft::LoadedPackage final_consumer = parse_package(
      sources,
      diagnostics,
      "app/package.draft",
      "app",
      R"draft(package app

import middle

pub relay :: proc(holder: middle.Holder) -> i64 {
    return middle.unwrap(holder.value)
}
)draft");
  const std::optional<draft::SyntaxReference> final_import =
      first_import(final_consumer);
  EXPECT(state, final_import.has_value());
  if (!final_import.has_value()) return;
  draft::AvailablePackageImports final_available;
  final_available.entries.push_back({*final_import, &consumer_interface});
  draft::SemanticAnalysisResult final_semantics =
      draft::analyze_package_semantics(
          sources, final_consumer, target.facts, final_available, diagnostics);
  draft::BodyCheckResult final_bodies = draft::check_package_bodies(
      sources,
      final_consumer,
      final_semantics.selections,
      final_semantics.package,
      final_semantics.constants,
      target.facts,
      diagnostics);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, dependency_semantics.ok);
  EXPECT(state, dependency_interface.declarations.size() == 1);
  if (!dependency_interface.declarations.empty()) {
    const draft::InterfaceDeclaration &maybe =
        dependency_interface.declarations.front();
    EXPECT(state, maybe.flags.parametric);
    EXPECT(state, maybe.parameters.size() == 1);
    if (!maybe.parameters.empty()) {
      EXPECT(state, maybe.parameters.front().name == "T");
      EXPECT(state, maybe.parameters.front().kind == draft::SymbolKind::TypeParameter);
      EXPECT(state, maybe.parameters.front().constraint ==
                        draft::TypeConstraintKind::AnyType);
    }
  }
  EXPECT(state, consumer_semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, consumer_semantics.package.parametric_parameters.size() == 1);
  EXPECT(state, consumer_semantics.package.parametric_type_instances.size() == 1);
  EXPECT(state, final_semantics.ok);
  EXPECT(state, final_bodies.ok);
  EXPECT(state, !diagnostics.has_errors());

  // A concrete specialization exported by the consumer must retain the
  // original template identity and its arguments. Rebaptizing it as a middle
  // package type would break equality in the next importing package.
  bool saw_maybe_specialization = false;
  for (const draft::InterfaceType &type : consumer_interface.types) {
    if (type.kind != draft::TypeKind::TaggedUnion ||
        type.nominal_public_name != "Maybe" ||
        type.nominal_arguments.size() != 1) {
      continue;
    }
    saw_maybe_specialization = true;
    EXPECT(state, type.nominal_root_identity == "workspace");
    EXPECT(state, type.nominal_root_relative_path == "lib/option");
    const draft::InterfaceType &argument =
        consumer_interface.types[type.nominal_arguments.front().value];
    EXPECT(state, argument.name == "i64");
  }
  EXPECT(state, saw_maybe_specialization);
}

void test_imported_parametric_constraint(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::LoadedPackage dependency = parse_package(
      sources,
      diagnostics,
      "number/package.draft",
      "number",
      R"draft(package number

pub Box[T: number] :: struct {
    value: T,
}
)draft");
  draft::SemanticAnalysisResult dependency_semantics =
      draft::analyze_package_semantics(
          sources, dependency, target.facts, diagnostics);
  draft::PackageInterface dependency_interface = draft::build_package_interface(
      {"workspace", "number"},
      dependency_semantics.package,
      dependency_semantics.constants,
      diagnostics);
  draft::LoadedPackage consumer = parse_package(
      sources,
      diagnostics,
      "bad/package.draft",
      "bad",
      R"draft(package bad

import number

Bad :: struct {
    value: number.Box[bool],
}
)draft");
  const std::optional<draft::SyntaxReference> import = first_import(consumer);
  EXPECT(state, import.has_value());
  if (!import.has_value()) return;
  draft::AvailablePackageImports available;
  available.entries.push_back({*import, &dependency_interface});
  const draft::SemanticAnalysisResult semantics =
      draft::analyze_package_semantics(
          sources, consumer, target.facts, available, diagnostics);

  EXPECT(state, dependency_semantics.ok);
  EXPECT(state, !semantics.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  EXPECT(state, rendered.find("does not satisfy its parametric constraint") !=
                    std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_imported_public_semantics(state);
  test_private_name_is_not_imported(state);
  test_imported_parametric_type(state);
  test_imported_parametric_constraint(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " interface test expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all interface tests passed\n";
  return EXIT_SUCCESS;
}
