// Type syntax, signature, member-scope, and natural-layout resolution tests.

#include "sema/analyzer.h"
#include "sema/type_resolver.h"
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
#include <vector>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "type_resolver_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct SemanticSource {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  draft::SemanticPackage semantic;

  explicit SemanticSource(std::string text) {
    loaded.short_name = "types";
    loaded.physical_directory = "/virtual/types";
    draft::LoadedPackageFile file;
    file.kind = draft::PackageFileKind::DraftSource;
    file.relative_name = "package.draft";
    file.source = sources.add_source("package.draft", std::move(text));
    file.syntax.emplace(draft::parse_source_file(sources, file.source, diagnostics));
    loaded.files.push_back(std::move(file));
    semantic = draft::collect_package_declarations(sources, loaded, diagnostics);
    draft::resolve_package_types(sources, loaded, semantic, diagnostics);
  }
};

[[nodiscard]] const draft::Symbol *find_symbol(
    const draft::SemanticPackage &package, std::string_view name) {
  const std::optional<draft::SymbolId> id =
      package.symbols.lookup_direct(package.package_scope, name);
  if (!id.has_value()) {
    return nullptr;
  }
  return &package.symbols.symbol(*id);
}

void test_types_signatures_and_layouts(TestState &state) {
  SemanticSource source(R"draft(
package types

Alias :: u32
Duration :: distinct i64

Header :: struct {
    tag: u8,
    value: u64,
    tail: [3]u16,
}

Overlay :: raw union {
    byte: u8,
    word: u64,
}

Kind :: enum u16 {
    First,
    Second = 7,
}

Choice :: union u16 {
    none,
    some: u32,
}

Callback :: c proc(value: u8) -> u32

transform :: proc(header: ^Header, count: usize) -> u64 {
    return count
}

Box[T: type] :: struct {
    value: ^T,
}

take[T: type] :: proc(value: ^T) -> ^T {
    return value
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, !source.diagnostics.has_errors());

  const draft::Symbol *alias = find_symbol(source.semantic, "Alias");
  const draft::Symbol *duration = find_symbol(source.semantic, "Duration");
  const draft::Symbol *header = find_symbol(source.semantic, "Header");
  const draft::Symbol *overlay = find_symbol(source.semantic, "Overlay");
  const draft::Symbol *kind = find_symbol(source.semantic, "Kind");
  const draft::Symbol *choice = find_symbol(source.semantic, "Choice");
  const draft::Symbol *callback = find_symbol(source.semantic, "Callback");
  const draft::Symbol *transform = find_symbol(source.semantic, "transform");
  const draft::Symbol *box = find_symbol(source.semantic, "Box");
  const draft::Symbol *take = find_symbol(source.semantic, "take");
  EXPECT(state, alias != nullptr);
  EXPECT(state, duration != nullptr);
  EXPECT(state, header != nullptr);
  EXPECT(state, overlay != nullptr);
  EXPECT(state, kind != nullptr);
  EXPECT(state, choice != nullptr);
  EXPECT(state, callback != nullptr);
  EXPECT(state, transform != nullptr);
  EXPECT(state, box != nullptr);
  EXPECT(state, take != nullptr);
  if (alias == nullptr || duration == nullptr || header == nullptr ||
      overlay == nullptr || kind == nullptr || choice == nullptr ||
      callback == nullptr || transform == nullptr || box == nullptr ||
      take == nullptr) {
    return;
  }

  const std::optional<draft::TypeId> u32 = source.semantic.types.find_builtin("u32");
  EXPECT(state, u32.has_value());
  if (u32.has_value()) {
    EXPECT(state, alias->kind == draft::SymbolKind::Type);
    EXPECT(state, alias->type == *u32);
  }
  EXPECT(state, source.semantic.types.type(duration->type).kind == draft::TypeKind::Distinct);
  EXPECT(state, source.semantic.types.type(duration->type).layout ==
                    draft::TypeLayout({true, 8, 8}));

  const draft::Type &header_type = source.semantic.types.type(header->type);
  EXPECT(state, header_type.layout == draft::TypeLayout({true, 24, 8}));
  EXPECT(state, header_type.member_offsets.size() == 3);
  if (header_type.member_offsets.size() == 3) {
    EXPECT(state, header_type.member_offsets[0] == 0);
    EXPECT(state, header_type.member_offsets[1] == 8);
    EXPECT(state, header_type.member_offsets[2] == 16);
  }

  const draft::Type &overlay_type = source.semantic.types.type(overlay->type);
  EXPECT(state, overlay_type.layout == draft::TypeLayout({true, 8, 8}));
  EXPECT(state, overlay_type.member_offsets == std::vector<std::uint64_t>({0, 0}));

  const draft::Type &kind_type = source.semantic.types.type(kind->type);
  EXPECT(state, kind_type.layout == draft::TypeLayout({true, 2, 2}));
  EXPECT(state, source.semantic.types.type(kind_type.element).name == "u16");

  const draft::Type &choice_type = source.semantic.types.type(choice->type);
  EXPECT(state, choice_type.layout == draft::TypeLayout({true, 8, 4}));
  EXPECT(state, choice_type.member_offsets == std::vector<std::uint64_t>({4, 4}));

  const draft::Type &callback_type = source.semantic.types.type(callback->type);
  const draft::Type &transform_type = source.semantic.types.type(transform->type);
  EXPECT(state, callback_type.kind == draft::TypeKind::Procedure);
  EXPECT(state, callback_type.c_calling_convention);
  EXPECT(state, transform_type.kind == draft::TypeKind::Procedure);
  EXPECT(state, !transform_type.c_calling_convention);
  EXPECT(state, transform_type.members.size() == 3);

  const draft::Type &box_type = source.semantic.types.type(box->type);
  EXPECT(state, box_type.layout == draft::TypeLayout({true, 8, 8}));
  EXPECT(state, source.semantic.parametric_parameters.size() == 2);
  EXPECT(state, take->flags.parametric);
}

void test_invalid_lengths_and_unknown_types(TestState &state) {
  SemanticSource source(R"draft(
package types

Bad_Array :: struct {
    empty: [0]u8,
    missing: Does_Not_Exist,
}
)draft");

  EXPECT(state, source.diagnostics.error_count() == 2);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("array length") != std::string::npos);
  EXPECT(state, rendered.find("unknown type name") != std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_types_signatures_and_layouts(state);
  test_invalid_lengths_and_unknown_types(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " type resolver expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all type resolver tests passed\n";
  return EXIT_SUCCESS;
}
