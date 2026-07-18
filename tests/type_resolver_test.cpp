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

} // namespace

int main() {
  TestState state;
  test_types_signatures_and_layouts(state);
  test_invalid_lengths_and_unknown_types(state);
  test_dependent_integer_expressions(state);
  test_dependent_integer_expression_diagnostics(state);
  test_layout_integer_context_diagnostics(state);
  test_parametric_type_diagnostics(state);
  test_value_parameter_diagnostics(state);
  test_invalid_enum_values(state);
  test_c_enum_default_backing(state);
  test_tagged_union_discriminator_capacity(state);
  test_invalid_representation_attributes(state);
  test_cyclic_layout_constant(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " type resolver expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all type resolver tests passed\n";
  return EXIT_SUCCESS;
}
