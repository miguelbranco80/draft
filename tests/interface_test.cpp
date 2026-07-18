// Public package-interface extraction and imported-name semantic tests.
//
// The test analyzes a dependency first, publishes only its `pub` declarations,
// then checks a consumer using qualified types, constants, procedures, aggregate
// members, and a dependency-selected `when`. It also verifies that an imported
// nominal retains its original package identity when exposed through another
// package's public procedure signature.

#include "sema/body_checker.h"
#include "sema/effect.h"
#include "sema/interface.h"
#include "sema/semantic.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/parser.h"
#include "target/profile.h"
#include "workspace/package.h"

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
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

pub Add_Procedure :: add

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

callback: proc(a, b: i64) -> i64 = math.Add_Procedure

Sized :: struct {
    bytes: [math.Count]u8,
}

main :: proc() {
    value := math.add(math.Count, 1)
    assert(value == 8)
    assert(callback(20, 22) == 42)
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
  EXPECT(state, dependency_interface.declarations.size() == 5);
  EXPECT(state, consumer_semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, bodies.checked_procedures == 2);
  EXPECT(state, consumer_semantics.package.imported_symbols.size() == 5);
  EXPECT(state, consumer_semantics.package.imported_types.size() == 2);
  EXPECT(state, consumer_interface.declarations.size() == 1);
  EXPECT(state, !diagnostics.has_errors());

  const draft::InterfaceDeclaration *procedure_constant = nullptr;
  for (const draft::InterfaceDeclaration &declaration :
       dependency_interface.declarations) {
    if (declaration.name == "Add_Procedure") {
      procedure_constant = &declaration;
      break;
    }
  }
  EXPECT(state, procedure_constant != nullptr);
  if (procedure_constant != nullptr) {
    EXPECT(state, procedure_constant->has_constant);
    EXPECT(state,
           procedure_constant->constant.kind == draft::ConstantKind::Procedure);
    EXPECT(state,
           procedure_constant->constant.root_identity == "workspace");
    EXPECT(state,
           procedure_constant->constant.root_relative_path == "lib/math");
    EXPECT(state, procedure_constant->constant.text == "add");
    EXPECT(state,
           procedure_constant->constant.symbol_index ==
               std::numeric_limits<std::uint32_t>::max());
  }

  const std::optional<draft::SymbolId> sized =
      consumer_semantics.package.symbols.lookup_direct(
          consumer_semantics.package.package_scope, "Sized");
  EXPECT(state, sized.has_value());
  if (sized.has_value()) {
    const draft::Type &type = consumer_semantics.package.types.type(
        consumer_semantics.package.symbols.symbol(*sized).type);
    EXPECT(state, type.layout == draft::TypeLayout({true, 7, 1}));
  }

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

pub Buffer[T: type, N: usize] :: struct {
    data: [N]T,
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
    bytes: option.Buffer[u16, 3],
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
  EXPECT(state, dependency_interface.declarations.size() == 2);
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
  if (dependency_interface.declarations.size() == 2) {
    const draft::InterfaceDeclaration &buffer =
        dependency_interface.declarations[1];
    EXPECT(state, buffer.name == "Buffer");
    EXPECT(state, buffer.flags.parametric);
    EXPECT(state, buffer.parameters.size() == 2);
    if (buffer.parameters.size() == 2) {
      EXPECT(state, buffer.parameters[1].name == "N");
      EXPECT(state, buffer.parameters[1].kind ==
                        draft::SymbolKind::ValueParameter);
      EXPECT(state, buffer.parameters[1].constraint ==
                        draft::TypeConstraintKind::CompileTimeValue);
      const draft::InterfaceType &value_type =
          dependency_interface.types[buffer.parameters[1].type.value];
      EXPECT(state, value_type.name == "usize");
    }
    const draft::InterfaceType &buffer_type =
        dependency_interface.types[buffer.type.value];
    EXPECT(state, buffer_type.members.size() == 1);
    if (!buffer_type.members.empty()) {
      const draft::InterfaceType &data =
          dependency_interface.types[buffer_type.members.front().value];
      EXPECT(state, data.kind == draft::TypeKind::Array);
      EXPECT(state, data.element_count_parameter == 1);
    }
  }
  EXPECT(state, consumer_semantics.ok);
  EXPECT(state, bodies.ok);
  EXPECT(state, consumer_semantics.package.parametric_parameters.size() == 3);
  EXPECT(state, consumer_semantics.package.parametric_type_instances.size() == 2);
  EXPECT(state, final_semantics.ok);
  EXPECT(state, final_bodies.ok);
  EXPECT(state, !diagnostics.has_errors());

  // A concrete specialization exported by the consumer must retain the
  // original template identity and its arguments. Rebaptizing it as a middle
  // package type would break equality in the next importing package.
  bool saw_maybe_specialization = false;
  bool saw_buffer_specialization = false;
  for (const draft::InterfaceType &type : consumer_interface.types) {
    if (type.kind == draft::TypeKind::TaggedUnion &&
        type.nominal_public_name == "Maybe" &&
        type.nominal_arguments.size() == 1) {
      saw_maybe_specialization = true;
      EXPECT(state, type.nominal_root_identity == "workspace");
      EXPECT(state, type.nominal_root_relative_path == "lib/option");
      const draft::InterfaceType &argument =
          consumer_interface.types[type.nominal_arguments.front().type.value];
      EXPECT(state, argument.name == "i64");
    }
    if (type.kind == draft::TypeKind::Struct &&
        type.nominal_public_name == "Buffer" &&
        type.nominal_arguments.size() == 2) {
      saw_buffer_specialization = true;
      EXPECT(state, type.nominal_root_identity == "workspace");
      EXPECT(state, type.nominal_root_relative_path == "lib/option");
      EXPECT(state, type.layout == draft::TypeLayout({true, 6, 2}));
      EXPECT(state, type.nominal_arguments[0].is_type);
      EXPECT(state, !type.nominal_arguments[1].is_type);
      EXPECT(state, type.nominal_arguments[1].value.kind ==
                        draft::ConstantKind::Integer);
      EXPECT(state,
             type.nominal_arguments[1].value.integer.to_decimal() == "3");
      const draft::InterfaceType &argument =
          consumer_interface.types[type.nominal_arguments[0].type.value];
      EXPECT(state, argument.name == "u16");
    }
  }
  EXPECT(state, saw_maybe_specialization);
  EXPECT(state, saw_buffer_specialization);
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

void test_imported_flow_effect(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();

  draft::LoadedPackage dependency = parse_package(
      sources,
      diagnostics,
      "callbacks/package.draft",
      "callbacks",
      R"draft(package callbacks

pub invoke :: proc(callback: proc()) {
    copy := callback
    copy()
}

pub apply :: proc(
    higher: proc(callback: proc()),
    callback: proc(),
) {
    higher(callback)
}

pub Callback_Box :: struct {
    callback: proc(),
}

pub invoke_box :: proc(box: Callback_Box) {
    box.callback()
}

pub install :: proc(destination: ^Callback_Box, callback: proc()) {
    destination^.callback = callback
}

pub identity_return :: proc(callback: proc()) -> proc() {
    return callback
}

hidden_assert :: proc() {
    assert(true)
}

pub make_assert :: proc() -> proc() {
    return hidden_assert
}
)draft");
  draft::SemanticAnalysisResult dependency_semantics =
      draft::analyze_package_semantics(
          sources, dependency, target.facts, diagnostics);
  draft::BodyCheckResult dependency_bodies = draft::check_package_bodies(
      sources,
      dependency,
      dependency_semantics.selections,
      dependency_semantics.package,
      dependency_semantics.constants,
      target.facts,
      diagnostics);
  const draft::EffectSummaryResult dependency_effects =
      draft::summarize_package_effects(
          dependency_semantics.package, dependency_bodies.program, &target);
  const draft::AgentMetadataResult empty_metadata;
  draft::PackageInterface dependency_interface =
      draft::build_package_interface(
          {"workspace", "callbacks"},
          dependency_semantics.package,
          dependency_semantics.constants,
          empty_metadata,
          dependency_effects,
          diagnostics);
  const auto invoke_box_interface = std::find_if(
      dependency_interface.declarations.begin(),
      dependency_interface.declarations.end(),
      [](const draft::InterfaceDeclaration &declaration) {
        return declaration.name == "invoke_box";
      });
  EXPECT(state,
      invoke_box_interface != dependency_interface.declarations.end());
  if (invoke_box_interface != dependency_interface.declarations.end()) {
    const auto path_effect = std::find_if(
        invoke_box_interface->effects.begin(),
        invoke_box_interface->effects.end(),
        [](const draft::InterfaceDeclaration::Effect &effect) {
          return effect.kind == draft::EffectKind::FlowCall;
        });
    EXPECT(state, path_effect != invoke_box_interface->effects.end());
    if (path_effect != invoke_box_interface->effects.end()) {
      EXPECT(state,
          path_effect->flow_path ==
              std::vector<std::string>{"callback"});
    }
  }
  const auto apply_interface = std::find_if(
      dependency_interface.declarations.begin(),
      dependency_interface.declarations.end(),
      [](const draft::InterfaceDeclaration &declaration) {
        return declaration.name == "apply";
      });
  EXPECT(state, apply_interface != dependency_interface.declarations.end());
  if (apply_interface != dependency_interface.declarations.end()) {
    const auto flow = std::find_if(
        apply_interface->effects.begin(),
        apply_interface->effects.end(),
        [](const draft::InterfaceDeclaration::Effect &effect) {
          return effect.kind == draft::EffectKind::FlowCall;
        });
    EXPECT(state, flow != apply_interface->effects.end());
    if (flow != apply_interface->effects.end()) {
      EXPECT(state, flow->flow_parameter == 0);
      EXPECT(state, flow->flow_arguments.size() == 1);
      if (flow->flow_arguments.size() == 1) {
        EXPECT(state, flow->flow_arguments.front().fields.size() == 1);
        if (flow->flow_arguments.front().fields.size() == 1) {
          const draft::InterfaceDeclaration::FlowValue &nested =
              flow->flow_arguments.front().fields.front().value;
          EXPECT(state, nested.flow_slots.size() == 1);
          if (nested.flow_slots.size() == 1) {
            EXPECT(state, nested.flow_slots.front().parameter == 1);
          }
        }
      }
    }
  }
  const auto identity_return_interface = std::find_if(
      dependency_interface.declarations.begin(),
      dependency_interface.declarations.end(),
      [](const draft::InterfaceDeclaration &declaration) {
        return declaration.name == "identity_return";
      });
  EXPECT(state,
      identity_return_interface != dependency_interface.declarations.end());
  if (identity_return_interface != dependency_interface.declarations.end()) {
    EXPECT(state, identity_return_interface->return_values.size() == 1);
    if (identity_return_interface->return_values.size() == 1) {
      EXPECT(state,
          identity_return_interface->return_values[0].flow_slots.size() == 1);
      if (identity_return_interface->return_values[0].flow_slots.size() == 1) {
        EXPECT(state,
            identity_return_interface->return_values[0]
                    .flow_slots[0].parameter == 0);
      }
    }
  }
  const auto install_interface = std::find_if(
      dependency_interface.declarations.begin(),
      dependency_interface.declarations.end(),
      [](const draft::InterfaceDeclaration &declaration) {
        return declaration.name == "install";
      });
  EXPECT(state, install_interface != dependency_interface.declarations.end());
  if (install_interface != dependency_interface.declarations.end()) {
    EXPECT(state, install_interface->field_writes.size() == 1);
    if (install_interface->field_writes.size() == 1) {
      const draft::InterfaceDeclaration::FieldWrite &write =
          install_interface->field_writes.front();
      EXPECT(state, write.parameter == 0);
      EXPECT(state, write.indirection == 1);
      EXPECT(state,
          write.path == std::vector<std::string>{"callback"});
      EXPECT(state, write.value_flow_slots.size() == 1);
      if (write.value_flow_slots.size() == 1) {
        EXPECT(state, write.value_flow_slots.front().parameter == 1);
        EXPECT(state, write.value_flow_slots.front().path.empty());
      }
    }
  }

  draft::LoadedPackage consumer = parse_package(
      sources,
      diagnostics,
      "consumer/package.draft",
      "consumer",
      R"draft(package consumer

import callbacks

danger :: proc() {
    assert(true)
}

caller :: proc() {
    callbacks.invoke(danger)
    callbacks.apply(callbacks.invoke, danger)
    box: callbacks.Callback_Box
    box.callback = danger
    callbacks.invoke_box(box)
    callbacks.install(&box, danger)
    box.callback()
    selected := callbacks.identity_return(danger)
    selected()
    provided := callbacks.make_assert()
    provided()
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
  draft::BodyCheckResult consumer_bodies = draft::check_package_bodies(
      sources,
      consumer,
      consumer_semantics.selections,
      consumer_semantics.package,
      consumer_semantics.constants,
      target.facts,
      diagnostics);
  const draft::EffectSummaryResult consumer_effects =
      draft::summarize_package_effects(
          consumer_semantics.package, consumer_bodies.program, &target);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, dependency_semantics.ok);
  EXPECT(state, dependency_bodies.ok);
  EXPECT(state, consumer_semantics.ok);
  EXPECT(state, consumer_bodies.ok);
  const std::optional<draft::SymbolId> caller =
      consumer_semantics.package.symbols.lookup_direct(
          consumer_semantics.package.package_scope, "caller");
  EXPECT(state, caller.has_value());
  if (!caller.has_value()) return;
  const draft::ProcedureEffectSummary *summary =
      consumer_effects.find(*caller);
  EXPECT(state, summary != nullptr);
  if (summary == nullptr) return;
  const bool has_assert = std::any_of(
      summary->effects.begin(),
      summary->effects.end(),
      [](const draft::SemanticEffect &effect) {
        return effect.kind == draft::EffectKind::RuntimeAssert;
      });
  const bool has_unknown = std::any_of(
      summary->effects.begin(),
      summary->effects.end(),
      [](const draft::SemanticEffect &effect) {
        return effect.kind == draft::EffectKind::UnknownCall;
      });
  EXPECT(state, has_assert);
  EXPECT(state, !has_unknown);
}

} // namespace

int main() {
  TestState state;
  test_imported_public_semantics(state);
  test_private_name_is_not_imported(state);
  test_imported_parametric_type(state);
  test_imported_parametric_constraint(state);
  test_imported_flow_effect(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " interface test expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all interface tests passed\n";
  return EXIT_SUCCESS;
}
