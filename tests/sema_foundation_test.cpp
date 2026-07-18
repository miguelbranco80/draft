// Semantic foundation tests for canonical types, natural layout, stable scopes,
// duplicate diagnostics, and lexical lookup.
//
// These tests operate below source declaration collection. They establish the
// invariants on which all later checking depends: structurally equal types share
// IDs, nominal types do not, package/file parent lookup is explicit, and duplicate
// declarations never replace the original binding.

#include "sema/symbol.h"
#include "sema/type.h"
#include "source/diagnostic.h"
#include "source/source.h"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "sema_foundation_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void test_builtin_and_structural_types(TestState &state) {
  draft::TypeStore types;
  const std::optional<draft::TypeId> u8 = types.find_builtin("u8");
  const std::optional<draft::TypeId> byte = types.find_builtin("byte");
  const std::optional<draft::TypeId> u32 = types.find_builtin("u32");
  const std::optional<draft::TypeId> u64 = types.find_builtin("u64");
  EXPECT(state, u8.has_value());
  EXPECT(state, byte == u8);
  EXPECT(state, u32.has_value());
  EXPECT(state, u64.has_value());
  if (!u8 || !u32 || !u64) return;

  const draft::TypeId pointer_a = types.pointer(*u32);
  const draft::TypeId pointer_b = types.pointer(*u32);
  EXPECT(state, pointer_a == pointer_b);
  EXPECT(state, types.type(pointer_a).layout.size == 8);
  EXPECT(state, types.type(pointer_a).layout.alignment == 8);

  const draft::TypeId slice = types.slice(*u32);
  EXPECT(state, types.type(slice).layout == draft::TypeLayout({true, 16, 8}));

  const draft::TypeId array = types.array(*u32, 5);
  EXPECT(state, types.type(array).layout == draft::TypeLayout({true, 20, 4}));

  // Procedure-dependent generic counts are not equal merely because they have
  // the same element type. Each source recipe owns a distinct side-table row
  // until its defining package returns a concrete count.
  const draft::TypeId deferred_array_a =
      types.owner_evaluated_array(*u32, 0);
  const draft::TypeId deferred_array_b =
      types.owner_evaluated_array(*u32, 1);
  EXPECT(state, deferred_array_a != deferred_array_b);
  EXPECT(state,
         types.type(deferred_array_a).owner_evaluated_element_count);
  EXPECT(state,
         types.type(deferred_array_a).deferred_element_count_index == 0);
  EXPECT(state, !types.type(deferred_array_a).layout.known);

  const draft::TypeId deferred_simd =
      types.owner_evaluated_simd(*u32, 2);
  EXPECT(state, types.type(deferred_simd).owner_evaluated_element_count);
  EXPECT(state,
         types.type(deferred_simd).deferred_element_count_index == 2);
  EXPECT(state, !types.type(deferred_simd).layout.known);

  const draft::TypeId tuple_a = types.tuple({*u8, *u64});
  const draft::TypeId tuple_b = types.tuple({*u8, *u64});
  EXPECT(state, tuple_a == tuple_b);
  EXPECT(state, types.type(tuple_a).layout == draft::TypeLayout({true, 16, 8}));

  const draft::TypeId ordinary =
      types.procedure({*u32}, types.builtins().bool_type, false);
  const draft::TypeId c_procedure =
      types.procedure({*u32}, types.builtins().bool_type, true);
  EXPECT(state, ordinary != c_procedure);
  EXPECT(state, types.type(ordinary).layout.size == 8);
}

void test_nominal_identity(TestState &state) {
  draft::TypeStore types;
  const std::optional<draft::TypeId> u32 = types.find_builtin("u32");
  EXPECT(state, u32.has_value());
  if (!u32) return;

  const draft::TypeId left = types.begin_nominal(
      draft::TypeKind::Struct, "Left", draft::SourceRange::invalid());
  const draft::TypeId right = types.begin_nominal(
      draft::TypeKind::Struct, "Right", draft::SourceRange::invalid());
  EXPECT(state, left != right);
  EXPECT(state, !types.type(left).layout.known);
  types.complete_nominal(left, {true, 4, 4}, {*u32});
  types.complete_nominal(right, {true, 4, 4}, {*u32});
  EXPECT(state, types.type(left).layout == types.type(right).layout);
  EXPECT(state, left != right);

  const draft::TypeId distinct_a =
      types.distinct("A", *u32, draft::SourceRange::invalid());
  const draft::TypeId distinct_b =
      types.distinct("B", *u32, draft::SourceRange::invalid());
  EXPECT(state, distinct_a != distinct_b);
  EXPECT(state, types.type(distinct_a).layout == types.type(*u32).layout);
}

void test_scopes_and_duplicates(TestState &state) {
  draft::SourceManager sources;
  const draft::FileId file = sources.add_source("symbols.draft", "Value Value local\n");
  draft::DiagnosticSink diagnostics;
  draft::SymbolTable symbols;
  const draft::ScopeId package = symbols.add_scope(
      draft::ScopeKind::Package, {}, draft::SourceRange::at(file, 0));
  const draft::ScopeId file_scope = symbols.add_scope(
      draft::ScopeKind::File, package, draft::SourceRange::at(file, 0));

  draft::Symbol first;
  first.name = "Value";
  first.kind = draft::SymbolKind::Constant;
  first.scope = package;
  first.name_range = {{file, 0}, {file, 5}};
  const draft::SymbolId first_id = symbols.declare(first, diagnostics);
  EXPECT(state, first_id.is_valid());

  draft::Symbol duplicate = first;
  duplicate.name_range = {{file, 6}, {file, 11}};
  const draft::SymbolId duplicate_id = symbols.declare(duplicate, diagnostics);
  EXPECT(state, !duplicate_id.is_valid());
  EXPECT(state, diagnostics.error_count() == 1);
  EXPECT(state, symbols.symbol_count() == 1);

  draft::Symbol local;
  local.name = "local";
  local.kind = draft::SymbolKind::Import;
  local.scope = file_scope;
  local.name_range = {{file, 12}, {file, 17}};
  const draft::SymbolId local_id = symbols.declare(local, diagnostics);
  EXPECT(state, local_id.is_valid());
  EXPECT(state, symbols.lookup(file_scope, "local") == local_id);
  EXPECT(state, symbols.lookup(file_scope, "Value") == first_id);
  EXPECT(state, !symbols.lookup_direct(package, "local").has_value());

  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  EXPECT(state, rendered.find("duplicate declaration") != std::string::npos);
  EXPECT(state, rendered.find("previous declaration") != std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_builtin_and_structural_types(state);
  test_nominal_identity(state);
  test_scopes_and_duplicates(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " semantic foundation expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all semantic foundation tests passed\n";
  return EXIT_SUCCESS;
}
