// Type syntax, signature, member-scope, and natural-layout resolution tests.

#include "sema/analyzer.h"
#include "sema/type_resolver.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/parser.h"
#include "workspace/package.h"

#include <array>
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

// ProductSemanticSource stops after eager declaration collection. Individual
// tests can then publish declaration-type attempts explicitly without the
// legacy package resolver having already supplied those facts.
struct ProductSemanticSource {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  draft::SemanticPackage semantic;

  explicit ProductSemanticSource(std::string text) {
    loaded.short_name = "type_products";
    loaded.physical_directory = "/virtual/type_products";
    draft::LoadedPackageFile file;
    file.kind = draft::PackageFileKind::DraftSource;
    file.relative_name = "package.draft";
    file.source = sources.add_source("package.draft", std::move(text));
    file.syntax.emplace(
        draft::parse_source_file(sources, file.source, diagnostics));
    loaded.files.push_back(std::move(file));
    semantic =
        draft::collect_package_declarations(sources, loaded, diagnostics);
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

[[nodiscard]] std::optional<draft::SymbolId> find_symbol_id(
    const draft::SemanticPackage &package, std::string_view name) {
  return package.symbols.lookup_direct(package.package_scope, name);
}

void test_declaration_type_products(TestState &state) {
  ProductSemanticSource source(R"draft(
package type_products

First :: Second
Second :: u32
Count :: 4
Buffer :: [Count]u8
Record :: struct {
    small: u8,
    large: u64,
}
Generated_Record :: struct {
    ... "add generated fields"
}
Cycle_A :: Cycle_B
Cycle_B :: Cycle_A
)draft");
  EXPECT(state, !source.diagnostics.has_errors());

  const std::optional<draft::SymbolId> first =
      find_symbol_id(source.semantic, "First");
  const std::optional<draft::SymbolId> second =
      find_symbol_id(source.semantic, "Second");
  const std::optional<draft::SymbolId> cycle_a =
      find_symbol_id(source.semantic, "Cycle_A");
  const std::optional<draft::SymbolId> cycle_b =
      find_symbol_id(source.semantic, "Cycle_B");
  const std::optional<draft::SymbolId> count =
      find_symbol_id(source.semantic, "Count");
  const std::optional<draft::SymbolId> buffer =
      find_symbol_id(source.semantic, "Buffer");
  const std::optional<draft::SymbolId> record =
      find_symbol_id(source.semantic, "Record");
  const std::optional<draft::SymbolId> generated_record =
      find_symbol_id(source.semantic, "Generated_Record");
  EXPECT(state, first.has_value());
  EXPECT(state, second.has_value());
  EXPECT(state, cycle_a.has_value());
  EXPECT(state, cycle_b.has_value());
  EXPECT(state, count.has_value());
  EXPECT(state, buffer.has_value());
  EXPECT(state, record.has_value());
  if (!first.has_value() || !second.has_value() || !cycle_a.has_value() ||
      !cycle_b.has_value() || !count.has_value() || !buffer.has_value() ||
      !record.has_value() || !generated_record.has_value()) {
    return;
  }

  const draft::ConditionalSelections selections;
  const draft::ConstantTable constants;
  const draft::TargetFacts target;

  draft::SemanticPackage blocked_package = source.semantic;
  draft::DiagnosticSink blocked_diagnostics;
  const draft::DeclarationTypeProductAttempt blocked =
      draft::resolve_package_declaration_type_product(
          source.sources,
          source.loaded,
          blocked_package,
          selections,
          *first,
          {},
          constants,
          target,
          draft::CompileTimeSynthesisMode::Reject,
          blocked_diagnostics);
  EXPECT(state, blocked.status == draft::TypeProductStatus::Blocked);
  EXPECT(
      state,
      blocked.declaration_dependencies ==
          std::vector<draft::SymbolId>({*second}));
  EXPECT(state, !blocked_diagnostics.has_errors());

  draft::SemanticPackage buffer_blocked_package = source.semantic;
  draft::DiagnosticSink buffer_blocked_diagnostics;
  const draft::DeclarationTypeProductAttempt buffer_blocked =
      draft::resolve_package_declaration_type_product(
          source.sources,
          source.loaded,
          buffer_blocked_package,
          selections,
          *buffer,
          {},
          constants,
          target,
          draft::CompileTimeSynthesisMode::Reject,
          buffer_blocked_diagnostics);
  EXPECT(
      state,
      buffer_blocked.status == draft::TypeProductStatus::Blocked);
  EXPECT(
      state,
      buffer_blocked.constant_dependencies ==
          std::vector<draft::SymbolId>({*count}));
  EXPECT(state, !buffer_blocked_diagnostics.has_errors());

  draft::ConstantTable published_count;
  published_count.bindings.push_back(
      {*count, draft::ConstantValue::make_integer(4)});
  draft::SemanticPackage buffer_package = source.semantic;
  draft::DiagnosticSink buffer_diagnostics;
  const draft::DeclarationTypeProductAttempt buffer_complete =
      draft::resolve_package_declaration_type_product(
          source.sources,
          source.loaded,
          buffer_package,
          selections,
          *buffer,
          {},
          published_count,
          target,
          draft::CompileTimeSynthesisMode::Reject,
          buffer_diagnostics);
  EXPECT(
      state,
      buffer_complete.status == draft::TypeProductStatus::Complete);
  EXPECT(state, !buffer_diagnostics.has_errors());
  if (buffer_complete.status == draft::TypeProductStatus::Complete) {
    const draft::Type &buffer_type =
        buffer_package.types.type(buffer_package.symbols.symbol(*buffer).type);
    EXPECT(state, buffer_type.kind == draft::TypeKind::Array);
    EXPECT(state, buffer_type.element_count == 4);
  }

  draft::SemanticPackage record_package = source.semantic;
  draft::DiagnosticSink record_diagnostics;
  const draft::DeclarationTypeProductAttempt record_members =
      draft::resolve_package_type_members_product(
          source.sources,
          source.loaded,
          record_package,
          selections,
          *record,
          draft::CompileTimeSynthesisMode::Reject,
          record_diagnostics);
  EXPECT(
      state,
      record_members.status == draft::TypeProductStatus::Complete);
  const draft::TypeId record_type =
      record_package.symbols.symbol(*record).type;
  EXPECT(
      state,
      record_package.types.facet_state(record_type, draft::TypeFacet::Members) ==
          draft::TypeFacetState::Complete);
  EXPECT(
      state,
      record_package.types.facet_state(
          record_type, draft::TypeFacet::MemberTypes) ==
          draft::TypeFacetState::Waiting);
  std::vector<draft::SymbolId> record_member_ids;
  std::vector<std::string> record_member_names;
  for (const draft::AggregateMember &member :
       record_package.aggregate_members) {
    if (member.owner == *record) {
      record_member_ids.push_back(member.member);
      record_member_names.push_back(
          record_package.symbols.symbol(member.member).name);
    }
  }
  EXPECT(
      state,
      record_member_names == std::vector<std::string>({"small", "large"}));

  const draft::DeclarationTypeProductAttempt record_complete =
      draft::resolve_package_declaration_type_product(
          source.sources,
          source.loaded,
          record_package,
          selections,
          *record,
          {},
          constants,
          target,
          draft::CompileTimeSynthesisMode::Reject,
          record_diagnostics);
  EXPECT(
      state,
      record_complete.status == draft::TypeProductStatus::Complete);
  EXPECT(
      state,
      record_package.types.facet_state(record_type, draft::TypeFacet::Members) ==
          draft::TypeFacetState::Complete);
  EXPECT(
      state,
      record_package.types.facet_state(
          record_type, draft::TypeFacet::MemberTypes) ==
          draft::TypeFacetState::Complete);
  std::vector<draft::SymbolId> typed_record_member_ids;
  for (const draft::AggregateMember &member :
       record_package.aggregate_members) {
    if (member.owner == *record) {
      typed_record_member_ids.push_back(member.member);
    }
  }
  EXPECT(state, typed_record_member_ids == record_member_ids);
  EXPECT(
      state,
      record_package.types.facet_state(
          record_type, draft::TypeFacet::NaturalLayout) ==
          draft::TypeFacetState::Waiting);
  draft::NaturalLayoutProductAttempt record_layout =
      draft::evaluate_natural_layout_product(
          record_package.types, record_type, record_diagnostics);
  EXPECT(
      state,
      record_layout.status == draft::TypeProductStatus::Complete);
  EXPECT(
      state,
      draft::publish_natural_layout_product(
          record_package,
          *record,
          record_type,
          std::move(record_layout),
          record_diagnostics));
  EXPECT(
      state,
      record_package.types.type(record_type).layout ==
          draft::TypeLayout({true, 16, 8}));
  EXPECT(state, !record_diagnostics.has_errors());

  // A member product may discover the package's opaque synthesis frontier, but
  // its incomplete member namespace is provider context rather than canonical
  // semantic state. The explicit status lets the workspace graph block this
  // exact product while retaining the source site needed to form an obligation.
  draft::SemanticPackage generated_package = source.semantic;
  draft::DiagnosticSink generated_diagnostics;
  const draft::DeclarationTypeProductAttempt generated_wait =
      draft::resolve_package_type_members_product(
          source.sources,
          source.loaded,
          generated_package,
          selections,
          *generated_record,
          draft::CompileTimeSynthesisMode::Discover,
          generated_diagnostics);
  EXPECT(
      state,
      generated_wait.status ==
          draft::TypeProductStatus::WaitingForSynthesis);
  EXPECT(state, !generated_diagnostics.has_errors());
  bool found_synthesis_member = false;
  for (const draft::SemanticSite &site : generated_package.sites) {
    if (site.kind == draft::SemanticSiteKind::SynthesisMember &&
        site.anchor == *generated_record) {
      found_synthesis_member = true;
    }
  }
  EXPECT(state, found_synthesis_member);

  draft::SemanticPackage second_package = source.semantic;
  draft::DiagnosticSink second_diagnostics;
  const draft::DeclarationTypeProductAttempt second_attempt =
      draft::resolve_package_declaration_type_product(
          source.sources,
          source.loaded,
          second_package,
          selections,
          *second,
          {},
          constants,
          target,
          draft::CompileTimeSynthesisMode::Reject,
          second_diagnostics);
  EXPECT(state, second_attempt.status == draft::TypeProductStatus::Complete);
  EXPECT(state, !second_diagnostics.has_errors());
  if (second_attempt.status != draft::TypeProductStatus::Complete) return;
  source.semantic = std::move(second_package);

  draft::SemanticPackage first_package = source.semantic;
  draft::DiagnosticSink first_diagnostics;
  const std::array completed{*second};
  const draft::DeclarationTypeProductAttempt first_attempt =
      draft::resolve_package_declaration_type_product(
          source.sources,
          source.loaded,
          first_package,
          selections,
          *first,
          completed,
          constants,
          target,
          draft::CompileTimeSynthesisMode::Reject,
          first_diagnostics);
  EXPECT(state, first_attempt.status == draft::TypeProductStatus::Complete);
  EXPECT(state, !first_diagnostics.has_errors());
  const std::optional<draft::TypeId> u32 =
      first_package.types.find_builtin("u32");
  EXPECT(state, u32.has_value());
  if (u32.has_value()) {
    EXPECT(state, first_package.symbols.symbol(*first).type == *u32);
    EXPECT(state, first_package.symbols.symbol(*second).type == *u32);
  }

  draft::SemanticPackage cycle_a_package = source.semantic;
  draft::DiagnosticSink cycle_a_diagnostics;
  const draft::DeclarationTypeProductAttempt cycle_a_attempt =
      draft::resolve_package_declaration_type_product(
          source.sources,
          source.loaded,
          cycle_a_package,
          selections,
          *cycle_a,
          {},
          constants,
          target,
          draft::CompileTimeSynthesisMode::Reject,
          cycle_a_diagnostics);
  draft::SemanticPackage cycle_b_package = source.semantic;
  draft::DiagnosticSink cycle_b_diagnostics;
  const draft::DeclarationTypeProductAttempt cycle_b_attempt =
      draft::resolve_package_declaration_type_product(
          source.sources,
          source.loaded,
          cycle_b_package,
          selections,
          *cycle_b,
          {},
          constants,
          target,
          draft::CompileTimeSynthesisMode::Reject,
          cycle_b_diagnostics);
  EXPECT(
      state,
      cycle_a_attempt.declaration_dependencies ==
          std::vector<draft::SymbolId>({*cycle_b}));
  EXPECT(
      state,
      cycle_b_attempt.declaration_dependencies ==
          std::vector<draft::SymbolId>({*cycle_a}));
  EXPECT(state, !cycle_a_diagnostics.has_errors());
  EXPECT(state, !cycle_b_diagnostics.has_errors());
}

void test_types_signatures_and_layouts(TestState &state) {
  SemanticSource source(R"draft(
package types

Base_Count :: 2
Member_Count :: Base_Count + 1
Cache_Alignment :: 1 << 6

Alias :: u32
Duration :: distinct i64
Grouped_Alias :: (u32)
Tuple_Alias :: (u32, u64)
Box_Alias :: Box[u32]
Tuple_Constant :: (1, 2)

Number_Box[T: number] :: struct {
    value: T,
}

box_pointer[T: type] :: proc(value: ^Box[T]) -> ^Box[T]
number_pointer[T: integer] :: proc(value: ^Number_Box[T]) -> ^Number_Box[T]

Header :: struct {
    tag: u8,
    value: u64,
    tail: [Member_Count]u16,
}

Overlay :: raw union {
    byte: u8,
    word: u64,
}

C_Header :: @repr(C) struct {
    tag: u8,
    value: u64,
}

C_Overlay :: @repr(C) @align(16) raw union {
    byte: u8,
    word: u64,
}

Aligned :: @align(Cache_Alignment) struct {
    bytes: [3]u8,
}

Kind :: enum u16 {
    First,
    Second = 7,
    Third,
}

Signed :: enum {
    Below = -1,
    Zero,
    High = 130,
}

C_Kind :: @repr(C) enum {
    First,
    Second,
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

Concrete_Pair :: struct {
    value: Pair[u32, u64],
}

Pair[T: type, U: type] :: struct {
    first: T,
    second: U,
}

Aligned_Box[T: type] :: @align(32) struct {
    value: T,
}

Concrete_Aligned :: struct {
    value: Aligned_Box[u8],
}

Buffer[T: type, N: usize] :: struct {
    data: [N]T,
}

Concrete_Buffer :: struct {
    value: Buffer[u16, Member_Count],
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
  const draft::Symbol *grouped_alias =
      find_symbol(source.semantic, "Grouped_Alias");
  const draft::Symbol *tuple_alias =
      find_symbol(source.semantic, "Tuple_Alias");
  const draft::Symbol *box_alias =
      find_symbol(source.semantic, "Box_Alias");
  const draft::Symbol *tuple_constant =
      find_symbol(source.semantic, "Tuple_Constant");
  const draft::Symbol *header = find_symbol(source.semantic, "Header");
  const draft::Symbol *overlay = find_symbol(source.semantic, "Overlay");
  const draft::Symbol *c_header = find_symbol(source.semantic, "C_Header");
  const draft::Symbol *c_overlay = find_symbol(source.semantic, "C_Overlay");
  const draft::Symbol *aligned = find_symbol(source.semantic, "Aligned");
  const draft::Symbol *kind = find_symbol(source.semantic, "Kind");
  const draft::Symbol *c_kind = find_symbol(source.semantic, "C_Kind");
  const draft::Symbol *choice = find_symbol(source.semantic, "Choice");
  const draft::Symbol *signed_kind = find_symbol(source.semantic, "Signed");
  const draft::Symbol *callback = find_symbol(source.semantic, "Callback");
  const draft::Symbol *transform = find_symbol(source.semantic, "transform");
  const draft::Symbol *box = find_symbol(source.semantic, "Box");
  const draft::Symbol *concrete_pair = find_symbol(source.semantic, "Concrete_Pair");
  const draft::Symbol *concrete_aligned =
      find_symbol(source.semantic, "Concrete_Aligned");
  const draft::Symbol *concrete_buffer =
      find_symbol(source.semantic, "Concrete_Buffer");
  const draft::Symbol *take = find_symbol(source.semantic, "take");
  EXPECT(state, alias != nullptr);
  EXPECT(state, duration != nullptr);
  EXPECT(state, grouped_alias != nullptr);
  EXPECT(state, tuple_alias != nullptr);
  EXPECT(state, box_alias != nullptr);
  EXPECT(state, tuple_constant != nullptr);
  EXPECT(state, header != nullptr);
  EXPECT(state, overlay != nullptr);
  EXPECT(state, c_header != nullptr);
  EXPECT(state, c_overlay != nullptr);
  EXPECT(state, aligned != nullptr);
  EXPECT(state, kind != nullptr);
  EXPECT(state, c_kind != nullptr);
  EXPECT(state, choice != nullptr);
  EXPECT(state, signed_kind != nullptr);
  EXPECT(state, callback != nullptr);
  EXPECT(state, transform != nullptr);
  EXPECT(state, box != nullptr);
  EXPECT(state, concrete_pair != nullptr);
  EXPECT(state, concrete_aligned != nullptr);
  EXPECT(state, concrete_buffer != nullptr);
  EXPECT(state, take != nullptr);
  if (alias == nullptr || duration == nullptr || grouped_alias == nullptr ||
      tuple_alias == nullptr || box_alias == nullptr ||
      tuple_constant == nullptr || header == nullptr ||
      overlay == nullptr || c_header == nullptr || c_overlay == nullptr ||
      aligned == nullptr || kind == nullptr || c_kind == nullptr || choice == nullptr ||
      signed_kind == nullptr || callback == nullptr || transform == nullptr ||
      box == nullptr || concrete_pair == nullptr ||
      concrete_aligned == nullptr || concrete_buffer == nullptr ||
      take == nullptr) {
    return;
  }

  const std::optional<draft::TypeId> u32 = source.semantic.types.find_builtin("u32");
  EXPECT(state, u32.has_value());
  if (u32.has_value()) {
    EXPECT(state, alias->kind == draft::SymbolKind::Type);
    EXPECT(state, alias->type == *u32);
    EXPECT(state, grouped_alias->kind == draft::SymbolKind::Type);
    EXPECT(state, grouped_alias->type == *u32);
  }
  EXPECT(state, tuple_alias->kind == draft::SymbolKind::Type);
  EXPECT(
      state,
      source.semantic.types.type(tuple_alias->type).kind ==
          draft::TypeKind::Tuple);
  EXPECT(
      state,
      source.semantic.types.type(tuple_alias->type).layout ==
          draft::TypeLayout({true, 16, 8}));
  EXPECT(state, box_alias->kind == draft::SymbolKind::Type);
  EXPECT(
      state,
      source.semantic.types.type(box_alias->type).kind ==
          draft::TypeKind::Struct);
  EXPECT(state, tuple_constant->kind == draft::SymbolKind::Constant);
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

  const draft::Type &c_header_type = source.semantic.types.type(c_header->type);
  EXPECT(state, c_header_type.c_representation);
  EXPECT(state, c_header_type.layout == draft::TypeLayout({true, 16, 8}));
  const draft::Type &c_overlay_type = source.semantic.types.type(c_overlay->type);
  EXPECT(state, c_overlay_type.c_representation);
  EXPECT(state, c_overlay_type.requested_alignment == 16);
  EXPECT(state, c_overlay_type.layout == draft::TypeLayout({true, 16, 16}));
  const draft::Type &aligned_type = source.semantic.types.type(aligned->type);
  EXPECT(state, !aligned_type.c_representation);
  EXPECT(state, aligned_type.requested_alignment == 64);
  EXPECT(state, aligned_type.layout == draft::TypeLayout({true, 64, 64}));

  const draft::Type &kind_type = source.semantic.types.type(kind->type);
  EXPECT(state, kind_type.layout == draft::TypeLayout({true, 2, 2}));
  EXPECT(state, source.semantic.types.type(kind_type.element).name == "u16");
  const draft::Type &signed_type = source.semantic.types.type(signed_kind->type);
  EXPECT(state, signed_type.layout == draft::TypeLayout({true, 2, 2}));
  EXPECT(state, source.semantic.types.type(signed_type.element).name == "i16");
  const draft::Type &c_kind_type = source.semantic.types.type(c_kind->type);
  EXPECT(state, c_kind_type.c_representation);
  EXPECT(state, c_kind_type.layout == draft::TypeLayout({true, 4, 4}));
  EXPECT(state, source.semantic.types.type(c_kind_type.element).name == "u32");
  bool saw_second = false;
  bool saw_third = false;
  bool saw_below = false;
  for (const draft::EnumMemberValue &value :
       source.semantic.enum_member_values) {
    const std::string &name = source.semantic.symbols.symbol(value.member).name;
    saw_second = saw_second ||
        (name == "Second" && value.value.to_decimal() == "7");
    saw_third = saw_third ||
        (name == "Third" && value.value.to_decimal() == "8");
    saw_below = saw_below ||
        (name == "Below" && value.value.to_decimal() == "-1");
  }
  EXPECT(state, saw_second);
  EXPECT(state, saw_third);
  EXPECT(state, saw_below);

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
  const draft::Type &concrete_pair_type =
      source.semantic.types.type(concrete_pair->type);
  EXPECT(state, concrete_pair_type.layout == draft::TypeLayout({true, 16, 8}));
  const draft::Type &concrete_aligned_type =
      source.semantic.types.type(concrete_aligned->type);
  EXPECT(state, concrete_aligned_type.layout == draft::TypeLayout({true, 32, 32}));
  EXPECT(state, concrete_aligned_type.members.size() == 1);
  if (!concrete_aligned_type.members.empty()) {
    const draft::Type &aligned_box =
        source.semantic.types.type(concrete_aligned_type.members.front());
    EXPECT(state, aligned_box.c_representation == false);
    EXPECT(state, aligned_box.requested_alignment == 32);
    EXPECT(state, aligned_box.layout == draft::TypeLayout({true, 32, 32}));
  }
  const draft::Type &concrete_buffer_type =
      source.semantic.types.type(concrete_buffer->type);
  EXPECT(state, concrete_buffer_type.layout == draft::TypeLayout({true, 6, 2}));
  EXPECT(state, concrete_buffer_type.members.size() == 1);
  if (!concrete_buffer_type.members.empty()) {
    const draft::Type &buffer =
        source.semantic.types.type(concrete_buffer_type.members.front());
    EXPECT(state, buffer.layout == draft::TypeLayout({true, 6, 2}));
    EXPECT(state, buffer.members.size() == 1);
    if (!buffer.members.empty()) {
      const draft::Type &data = source.semantic.types.type(buffer.members.front());
      EXPECT(state, data.kind == draft::TypeKind::Array);
      EXPECT(state, data.element_count == 3);
      EXPECT(state, source.semantic.types.type(data.element).name == "u16");
    }
  }
  EXPECT(state, source.semantic.parametric_type_instances.size() == 6);
  EXPECT(state, source.semantic.parametric_parameters.size() == 10);
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

void test_dependent_integer_expressions(TestState &state) {
  SemanticSource source(R"draft(
package types

Buffer[N: usize] :: struct {
    values: [N + 1]u16,
}

Narrow_Buffer[N: u8] :: struct {
    values: [cast[usize](N) + cast[usize](cast[u8](256)) + 1]u8,
}

Envelope[N: usize, M: usize] :: struct {
    buffer: Buffer[N + M],
    mask: [1 << M]u8,
}

Concrete :: struct {
    value: Envelope[1, 2],
}

Narrow_Concrete :: struct {
    value: Narrow_Buffer[3],
}

Explicit_Wrap :: struct {
    values: [cast[usize](cast[u8](256) + 1)]u8,
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, !source.diagnostics.has_errors());

  const draft::Symbol *concrete = find_symbol(source.semantic, "Concrete");
  const draft::Symbol *narrow =
      find_symbol(source.semantic, "Narrow_Concrete");
  const draft::Symbol *explicit_wrap =
      find_symbol(source.semantic, "Explicit_Wrap");
  EXPECT(state, concrete != nullptr);
  EXPECT(state, narrow != nullptr);
  EXPECT(state, explicit_wrap != nullptr);
  if (concrete == nullptr || narrow == nullptr || explicit_wrap == nullptr) {
    return;
  }
  const draft::Type &concrete_type =
      source.semantic.types.type(concrete->type);
  EXPECT(state, concrete_type.layout == draft::TypeLayout({true, 12, 2}));
  EXPECT(state, concrete_type.members.size() == 1);
  if (concrete_type.members.size() != 1) return;

  const draft::Type &envelope =
      source.semantic.types.type(concrete_type.members.front());
  EXPECT(state, envelope.layout == draft::TypeLayout({true, 12, 2}));
  EXPECT(state, envelope.members.size() == 2);
  if (envelope.members.size() != 2) return;
  const draft::Type &buffer =
      source.semantic.types.type(envelope.members.front());
  EXPECT(state, buffer.layout == draft::TypeLayout({true, 8, 2}));
  EXPECT(state, buffer.members.size() == 1);
  if (buffer.members.size() == 1) {
    const draft::Type &values =
        source.semantic.types.type(buffer.members.front());
    EXPECT(state, values.kind == draft::TypeKind::Array);
    EXPECT(state, values.element_count == 4);
  }
  const draft::Type &mask = source.semantic.types.type(envelope.members.back());
  EXPECT(state, mask.kind == draft::TypeKind::Array);
  EXPECT(state, mask.element_count == 4);

  const draft::Type &narrow_concrete =
      source.semantic.types.type(narrow->type);
  EXPECT(state, narrow_concrete.layout == draft::TypeLayout({true, 4, 1}));
  EXPECT(
      state,
      source.semantic.types.type(explicit_wrap->type).layout ==
          draft::TypeLayout({true, 1, 1}));
}

void test_dependent_integer_expression_diagnostics(TestState &state) {
  SemanticSource source(R"draft(
package types

Bad_Shift[N: usize] :: struct {
    values: [(1 << N) / 2]u8,
}

Concrete :: struct {
    value: Bad_Shift[64],
}
)draft");

  EXPECT(state, source.diagnostics.has_errors());
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("out-of-range shift") != std::string::npos);
}

void test_layout_integer_context_diagnostics(TestState &state) {
  SemanticSource source(R"draft(
package types

Bad_Array :: struct {
    values: [cast[u64](4)]u8,
}

Bad_Vector :: #simd[cast[u64](4)]u32

Bad_Aligned :: @align(cast[u64](4)) struct {
    value: u8,
}
)draft");

  EXPECT(state, source.diagnostics.error_count() == 3);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("package.draft:5:14") != std::string::npos);
  EXPECT(state, rendered.find("array length must have type 'usize'") !=
                    std::string::npos);
  EXPECT(state, rendered.find("SIMD lane count must have type 'usize'") !=
                    std::string::npos);
  EXPECT(state, rendered.find("'@align' argument must have type 'usize'") !=
                    std::string::npos);
}

void test_parametric_type_diagnostics(TestState &state) {
  SemanticSource source(R"draft(
package types

Number_Box[T: number] :: struct {
    value: T,
}

Counter :: distinct u32

Missing_Argument :: struct {
    value: Number_Box,
}

Wrong_Constraint :: struct {
    value: Number_Box[bool],
}

Wrong_Distinct_Constraint :: struct {
    value: Number_Box[Counter],
}
)draft");

  EXPECT(state, source.diagnostics.error_count() == 3);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("requires explicit type arguments") !=
                    std::string::npos);
  EXPECT(state, rendered.find("does not satisfy its parametric constraint") !=
                    std::string::npos);
}

void test_static_argument_pack_signature_metadata(TestState &state) {
  SemanticSource source(R"draft(
package types

visit :: proc(prefix: string, values: ..type) {}
empty :: proc(values: ..type) {}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, !source.diagnostics.has_errors());
  EXPECT(state, source.semantic.static_argument_packs.size() == 2);

  const draft::Symbol *visit = find_symbol(source.semantic, "visit");
  const draft::Symbol *empty = find_symbol(source.semantic, "empty");
  EXPECT(state, visit != nullptr);
  EXPECT(state, empty != nullptr);
  if (visit == nullptr || empty == nullptr ||
      source.semantic.static_argument_packs.size() != 2) {
    return;
  }

  EXPECT(state, visit->flags.parametric);
  EXPECT(state, empty->flags.parametric);
  const draft::Type &visit_type = source.semantic.types.type(visit->type);
  const draft::Type &empty_type = source.semantic.types.type(empty->type);
  // A static pack is compile-time signature structure. Only the fixed prefix
  // and result inhabit the source procedure type; concrete instances append
  // their ordinary tail parameters later during body checking.
  EXPECT(state, visit_type.members.size() == 2);
  EXPECT(state, empty_type.members.size() == 1);

  const draft::StaticArgumentPack &visit_pack =
      source.semantic.static_argument_packs.front();
  const draft::StaticArgumentPack &empty_pack =
      source.semantic.static_argument_packs.back();
  EXPECT(state, visit_pack.fixed_parameter_count == 1);
  EXPECT(state,
      source.semantic.symbols.symbol(visit_pack.binding).name == "values");
  EXPECT(state, visit_pack.symbolic_element_type.is_valid());
  EXPECT(state, empty_pack.fixed_parameter_count == 0);
}

void test_static_argument_pack_declaration_diagnostics(TestState &state) {
  SemanticSource source(R"draft(
package types

not_final :: proc(values: ..type, suffix: u8) {}
two :: proc(left: ..type, right: ..type) {}
grouped :: proc(left, right: ..type) {}
discarded :: proc(_: ..type) {}
wrong_marker :: proc(values: ..u32) {}
Callback :: proc(values: ..type)

c_variadic :: c proc(values: ..type) {}

foreign libc {
    foreign_variadic :: proc(values: ..type)
}

export exported_variadic :: c "exported_variadic" proc(values: ..type) {}
)draft");

  EXPECT(state, source.diagnostics.has_errors());
  for (const draft::Diagnostic &diagnostic :
       source.diagnostics.diagnostics()) {
    if (diagnostic.severity == draft::DiagnosticSeverity::Error) {
      EXPECT(state, diagnostic.range.is_valid());
    }
  }
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("static argument pack must be the final") !=
                    std::string::npos);
  EXPECT(state, rendered.find("only one static argument pack") !=
                    std::string::npos);
  EXPECT(state, rendered.find("requires one named binding") !=
                    std::string::npos);
  EXPECT(state, rendered.find("element marker must be '..type'") !=
                    std::string::npos);
  EXPECT(state, rendered.find("require a named procedure body") !=
                    std::string::npos);
  EXPECT(state, rendered.find(
      "C, foreign, and exported procedures require fixed signatures") !=
                    std::string::npos);
}

void test_value_parameter_diagnostics(TestState &state) {
  SemanticSource source(R"draft(
package types

Buffer[T: type, N: u8] :: struct {
    data: [N]T,
}

Mismatched[N: usize] :: struct {
    value: Buffer[u8, N + 1],
}

Word_Buffer[N: usize] :: struct {
    data: [N]u8,
}

Same_Width_Mismatch[N: u64] :: struct {
    value: Word_Buffer[N],
}

Zero :: struct { value: Buffer[u8, 0], }
Too_Wide :: struct { value: Buffer[u8, 256], }
)draft");

  EXPECT(state, source.diagnostics.error_count() == 4);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("must instantiate to a nonzero u64") !=
                    std::string::npos);
  EXPECT(state, rendered.find("not representable in its parameter type") !=
                    std::string::npos);
  EXPECT(state, rendered.find("symbolic type value argument has the wrong result type") !=
                    std::string::npos);
}

void test_invalid_enum_values(TestState &state) {
  SemanticSource source(R"draft(
package types

Duplicate :: enum {
    Zero,
    First = 4,
    Second = 4,
}

Too_Wide :: enum u8 {
    Zero,
    Value = 256,
}

Missing_Zero :: enum {
    Only = 1,
}

Too_Wide_C :: @repr(C) enum {
    Zero,
    Value = 18446744073709551616,
}
)draft");

  EXPECT(state, source.diagnostics.error_count() == 4);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("duplicate enum value") != std::string::npos);
  EXPECT(state, rendered.find("not representable") != std::string::npos);
  EXPECT(state, rendered.find("must declare a zero-valued member") !=
                    std::string::npos);
  EXPECT(state, rendered.find("do not fit the target C ABI's default backing") !=
                    std::string::npos);
}

void test_c_enum_default_backing(TestState &state) {
  SemanticSource source(R"draft(
package types

Positive :: @repr(C) enum {
    Zero,
    Largest_Int = 2147483647,
}

Negative :: @repr(C) enum {
    Minimum_Int = -2147483648,
    Zero = 0,
}

Wide_Positive :: @repr(C) enum {
    Zero,
    Beyond_U32 = 4294967296,
}

Wide_Negative :: @repr(C) enum {
    Beyond_I32 = -2147483649,
    Zero = 0,
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  const auto backing_name = [&](std::string_view name) -> std::string {
    const draft::Symbol *symbol = find_symbol(source.semantic, name);
    if (symbol == nullptr) return {};
    const draft::Type &enumeration =
        source.semantic.types.type(symbol->type);
    if (!enumeration.element.is_valid()) return {};
    return source.semantic.types.type(enumeration.element).name;
  };
  EXPECT(state, !source.diagnostics.has_errors());
  EXPECT(state, backing_name("Positive") == "u32");
  EXPECT(state, backing_name("Negative") == "i32");
  EXPECT(state, backing_name("Wide_Positive") == "u64");
  EXPECT(state, backing_name("Wide_Negative") == "i64");
}

void test_tagged_union_discriminator_capacity(TestState &state) {
  std::string text = "package types\n\nToo_Many :: union u8 {\n";
  // u8 can represent discriminators 0 through 255. The 257th source-order
  // alternative therefore proves that the complete range is validated.
  for (std::size_t index = 0; index < 257; ++index) {
    text += "    Case_" + std::to_string(index) + ",\n";
  }
  text += "}\n";
  SemanticSource source(std::move(text));

  EXPECT(state, source.diagnostics.error_count() == 1);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("do not fit its discriminator type") !=
                    std::string::npos);
}

void test_invalid_representation_attributes(TestState &state) {
  SemanticSource source(R"draft(
package types

Bad_Alignment :: @align(3) struct { value: u8, }
Reduced_Alignment :: @align(2) struct { value: u64, }
Aligned_Enum :: @align(8) enum { Value, }
C_Union :: @repr(C) union { none, }
Wrong_Repr :: @repr(Rust) struct { value: u8, }
Unknown :: @packed struct { value: u8, }
Attributed_Scalar :: @align(8) u64
)draft");

  EXPECT(state, source.diagnostics.has_errors());
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("positive power-of-two") != std::string::npos);
  EXPECT(state, rendered.find("cannot reduce") != std::string::npos);
  EXPECT(state, rendered.find("valid only on structs and raw unions") !=
                    std::string::npos);
  EXPECT(state, rendered.find("valid only on structs, raw unions, and enums") !=
                    std::string::npos);
  EXPECT(state, rendered.find("supports only '@repr(C)'") != std::string::npos);
  EXPECT(state, rendered.find("unknown type representation attribute") !=
                    std::string::npos);
  EXPECT(state, rendered.find("valid only on aggregate type constructors") !=
                    std::string::npos);
}

void test_cyclic_layout_constant(TestState &state) {
  SemanticSource source(R"draft(
package types

First :: Second + 0
Second :: First + 0

Bad :: struct {
    values: [First]u8,
}
)draft");

  EXPECT(state, source.diagnostics.has_errors());
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("cyclic integer constant required by type layout") !=
                    std::string::npos);
  EXPECT(state, rendered.find("array length") != std::string::npos);
}

void test_type_declaration_depth_is_bounded(TestState &state) {
  std::string text = "package types\n\n";
  constexpr std::size_t alias_count = 320;
  for (std::size_t index = 0; index < alias_count; ++index) {
    text += "Alias_" + std::to_string(index) + " :: Alias_" +
        std::to_string(index + 1) + "\n";
  }
  text += "Alias_" + std::to_string(alias_count) + " :: u64\n";
  SemanticSource source(std::move(text));

  EXPECT(state, source.diagnostics.has_errors());
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state,
      rendered.find(
          "declaration dependency depth exceeds the implementation "
          "limit of 256") != std::string::npos);

  // Name-valued declarations are intentionally ambiguous until this pass
  // discovers whether the final binding denotes a type or a constant. Exercise
  // the constant outcome as a separate chain so the shared guard cannot drift
  // into an alias-only implementation.
  std::string constants = "package types\n\n";
  for (std::size_t index = 0; index < alias_count; ++index) {
    constants += "Constant_" + std::to_string(index) + " :: Constant_" +
        std::to_string(index + 1) + "\n";
  }
  constants +=
      "Constant_" + std::to_string(alias_count) + " :: 42\n";
  SemanticSource constant_source(std::move(constants));
  EXPECT(state, constant_source.diagnostics.has_errors());
  const std::string constant_rendered = draft::render_diagnostics(
      constant_source.sources, constant_source.diagnostics);
  EXPECT(state,
      constant_rendered.find(
          "declaration dependency depth exceeds the implementation "
          "limit of 256") != std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_declaration_type_products(state);
  test_types_signatures_and_layouts(state);
  test_invalid_lengths_and_unknown_types(state);
  test_dependent_integer_expressions(state);
  test_dependent_integer_expression_diagnostics(state);
  test_layout_integer_context_diagnostics(state);
  test_parametric_type_diagnostics(state);
  test_static_argument_pack_signature_metadata(state);
  test_static_argument_pack_declaration_diagnostics(state);
  test_value_parameter_diagnostics(state);
  test_invalid_enum_values(state);
  test_c_enum_default_backing(state);
  test_tagged_union_discriminator_capacity(state);
  test_invalid_representation_attributes(state);
  test_cyclic_layout_constant(state);
  test_type_declaration_depth_is_bounded(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " type resolver expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all type resolver tests passed\n";
  return EXIT_SUCCESS;
}
