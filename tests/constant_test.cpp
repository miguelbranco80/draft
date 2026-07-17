// Scalar compile-time evaluation and fixed-point declaration `when` tests.

#include "sema/constant.h"
#include "sema/semantic.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/parser.h"
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
      std::cerr << "constant_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct AnalyzedSource {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::LoadedPackage loaded;
  draft::SemanticAnalysisResult analysis;

  explicit AnalyzedSource(std::string text) {
    loaded.short_name = "conditions";
    draft::LoadedPackageFile file;
    file.kind = draft::PackageFileKind::DraftSource;
    file.relative_name = "package.draft";
    file.source = sources.add_source("package.draft", std::move(text));
    file.syntax.emplace(draft::parse_source_file(sources, file.source, diagnostics));
    loaded.files.push_back(std::move(file));

    draft::TargetFacts target;
    target.identity = "draft-aarch64-macos-v1";
    target.arch = "aarch64";
    target.os = "macos";
    target.abi = "darwin";
    target.byte_order = "little";
    target.object_format = "macho";
    target.file_tag = "aarch64-macos";
    target.pointer_bits = 64;
    target.page_size = 16384;
    target.known_features = {"crc", "neon"};
    target.features = {"neon"};
    analysis = draft::analyze_package_semantics(
        sources, loaded, target, diagnostics);
  }
};

[[nodiscard]] std::optional<draft::SymbolId> find_symbol(
    const draft::SemanticPackage &package, std::string_view name) {
  return package.symbols.lookup_direct(package.package_scope, name);
}

void test_constants_and_conditional_rounds(TestState &state) {
  AnalyzedSource source(R"draft(
package conditions

Bits :: target.pointer_bits
Base :: 40
Derived :: Base + 2
Has_Neon :: target.has_feature("neon")
Bit_Value :: (2 << 5) | 1
Huge :: 340282366920938463463374607431768211456 + 7
Fraction :: 1.25 + 0.75
Accent :: 'é'

Header :: struct {
    tag: u8,
    value: u64,
}

Header_Size :: size_of(Header)
Header_Alignment :: align_of(Header)

Conditional :: struct {
    when target.pointer_bits == 64 {
        wide: u64,
    } else {
        narrow: u32,
    }
}

when target.pointer_bits == 32 {
    Word :: u32
} else {
    Word :: u64
}

when target.os == .macos && Has_Neon {
    Platform_Value :: Derived
}

when false {
    Never :: 1
} else when Derived == 42 {
    Nested :: 9
}

when false {
    Broken :: struct { value: Does_Not_Exist, }
} else {
    Good :: u8
}

when Huge > 340282366920938463463374607431768211456 {
    Big_Selected :: bool
}

when Fraction == 2.0 {
    Float_Selected :: bool
}

when size_of(Header) == 16 && align_of(Header) == 8 {
    Layout_Selected :: bool
}

when Accent == '\u{e9}' {
    Rune_Selected :: bool
}
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.analysis.ok);
  EXPECT(state, !source.diagnostics.has_errors());
  EXPECT(state, source.analysis.selections.entries.size() == 10);
  EXPECT(state, find_symbol(source.analysis.package, "Word").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Platform_Value").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Nested").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Good").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Big_Selected").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Float_Selected").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Layout_Selected").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Rune_Selected").has_value());
  EXPECT(state, !find_symbol(source.analysis.package, "Never").has_value());
  EXPECT(state, !find_symbol(source.analysis.package, "Broken").has_value());

  const std::optional<draft::SymbolId> conditional =
      find_symbol(source.analysis.package, "Conditional");
  EXPECT(state, conditional.has_value());
  if (conditional.has_value()) {
    const draft::Symbol &symbol = source.analysis.package.symbols.symbol(*conditional);
    EXPECT(state, source.analysis.package.types.type(symbol.type).layout ==
                      draft::TypeLayout({true, 8, 8}));
    bool saw_wide = false;
    bool saw_narrow = false;
    for (const draft::OwnedSemanticScope &owned :
         source.analysis.package.owned_scopes) {
      if (owned.owner != *conditional ||
          source.analysis.package.symbols.scope(owned.scope).kind !=
              draft::ScopeKind::Type) {
        continue;
      }
      saw_wide = source.analysis.package.symbols.lookup_direct(
          owned.scope, "wide").has_value();
      saw_narrow = source.analysis.package.symbols.lookup_direct(
          owned.scope, "narrow").has_value();
    }
    EXPECT(state, saw_wide);
    EXPECT(state, !saw_narrow);
  }

  const std::optional<draft::SymbolId> bits =
      find_symbol(source.analysis.package, "Bits");
  const std::optional<draft::SymbolId> derived =
      find_symbol(source.analysis.package, "Derived");
  const std::optional<draft::SymbolId> feature =
      find_symbol(source.analysis.package, "Has_Neon");
  const std::optional<draft::SymbolId> bit_value =
      find_symbol(source.analysis.package, "Bit_Value");
  const std::optional<draft::SymbolId> huge =
      find_symbol(source.analysis.package, "Huge");
  const std::optional<draft::SymbolId> fraction =
      find_symbol(source.analysis.package, "Fraction");
  const std::optional<draft::SymbolId> header_size =
      find_symbol(source.analysis.package, "Header_Size");
  const std::optional<draft::SymbolId> header_alignment =
      find_symbol(source.analysis.package, "Header_Alignment");
  const std::optional<draft::SymbolId> accent =
      find_symbol(source.analysis.package, "Accent");
  EXPECT(state, bits.has_value());
  EXPECT(state, derived.has_value());
  EXPECT(state, feature.has_value());
  EXPECT(state, bit_value.has_value());
  EXPECT(state, huge.has_value());
  EXPECT(state, fraction.has_value());
  EXPECT(state, header_size.has_value());
  EXPECT(state, header_alignment.has_value());
  EXPECT(state, accent.has_value());
  if (bits && derived && feature && bit_value && huge && fraction &&
      header_size && header_alignment && accent) {
    const draft::ConstantValue *bits_value = source.analysis.constants.find(*bits);
    const draft::ConstantValue *derived_value = source.analysis.constants.find(*derived);
    const draft::ConstantValue *feature_value = source.analysis.constants.find(*feature);
    const draft::ConstantValue *bit_result = source.analysis.constants.find(*bit_value);
    const draft::ConstantValue *huge_result = source.analysis.constants.find(*huge);
    const draft::ConstantValue *fraction_result = source.analysis.constants.find(*fraction);
    const draft::ConstantValue *size_result =
        source.analysis.constants.find(*header_size);
    const draft::ConstantValue *alignment_result =
        source.analysis.constants.find(*header_alignment);
    const draft::ConstantValue *accent_result =
        source.analysis.constants.find(*accent);
    EXPECT(state, bits_value != nullptr);
    EXPECT(state, derived_value != nullptr);
    EXPECT(state, feature_value != nullptr);
    EXPECT(state, bit_result != nullptr);
    EXPECT(state, huge_result != nullptr);
    EXPECT(state, fraction_result != nullptr);
    EXPECT(state, size_result != nullptr);
    EXPECT(state, alignment_result != nullptr);
    EXPECT(state, accent_result != nullptr);
    if (bits_value) EXPECT(state, bits_value->integer.to_decimal() == "64");
    if (derived_value) EXPECT(state, derived_value->integer.to_decimal() == "42");
    if (feature_value) EXPECT(state, feature_value->boolean);
    if (bit_result) EXPECT(state, bit_result->integer.to_decimal() == "65");
    if (huge_result) {
      EXPECT(state, huge_result->integer.to_decimal() ==
                        "340282366920938463463374607431768211463");
    }
    if (fraction_result) {
      EXPECT(state, fraction_result->kind == draft::ConstantKind::Float);
      EXPECT(state, fraction_result->floating.to_fraction() == "2/1");
    }
    if (size_result) EXPECT(state, size_result->integer.to_decimal() == "16");
    if (alignment_result) {
      EXPECT(state, alignment_result->integer.to_decimal() == "8");
    }
    if (accent_result) {
      EXPECT(state, accent_result->integer.to_decimal() == "233");
      EXPECT(state, source.analysis.package.symbols.symbol(*accent).type ==
                        source.analysis.package.types.builtins().rune_type);
    }
  }
}

void test_invalid_required_constants(TestState &state) {
  AnalyzedSource source(R"draft(
package conditions

Not_Bool :: 7
Bad :: 1 / 0

when Not_Bool {
    Value :: 1
}
)draft");

  EXPECT(state, !source.analysis.ok);
  EXPECT(state, source.diagnostics.error_count() == 2);
  const std::string rendered =
      draft::render_diagnostics(source.sources, source.diagnostics);
  EXPECT(state, rendered.find("must have type bool") != std::string::npos);
  EXPECT(state, rendered.find("division by zero") != std::string::npos);
}

void test_global_initializers(TestState &state) {
  AnalyzedSource source(R"draft(
package conditions

Mode :: enum i16 {
    Off,
    On = 7,
}

Answer :: 40 + 2
count: u64 = Answer
inferred := 21
ratio: f64 = 0.5
enabled: bool = true
message: string = "draft"
mode: Mode = .On
pointer: ^u64 = nil
thread_local scratch: i32 = -7
)draft");
  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.analysis.ok);
  EXPECT(state, !source.diagnostics.has_errors());
  const std::optional<draft::SymbolId> count =
      find_symbol(source.analysis.package, "count");
  const std::optional<draft::SymbolId> inferred =
      find_symbol(source.analysis.package, "inferred");
  const std::optional<draft::SymbolId> mode =
      find_symbol(source.analysis.package, "mode");
  const std::optional<draft::SymbolId> pointer =
      find_symbol(source.analysis.package, "pointer");
  EXPECT(state, count.has_value());
  EXPECT(state, inferred.has_value());
  EXPECT(state, mode.has_value());
  EXPECT(state, pointer.has_value());
  if (count.has_value()) {
    EXPECT(state, source.analysis.constants.find(*count) == nullptr);
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*count);
    EXPECT(state, value != nullptr);
    if (value != nullptr) EXPECT(state, value->integer.to_decimal() == "42");
  }
  if (inferred.has_value()) {
    EXPECT(state, source.analysis.package.symbols.symbol(*inferred).type ==
                      source.analysis.package.types.builtins().int_type);
  }
  if (mode.has_value()) {
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*mode);
    EXPECT(state, value != nullptr);
    if (value != nullptr) EXPECT(state, value->integer.to_decimal() == "7");
  }
  if (pointer.has_value()) {
    const draft::ConstantValue *value =
        source.analysis.global_initializers.find(*pointer);
    EXPECT(state, value != nullptr);
    if (value != nullptr) EXPECT(state, value->kind == draft::ConstantKind::Nil);
  }

  AnalyzedSource invalid(R"draft(
package conditions

Mode :: enum {
    Off,
}

runtime :: proc() -> i64 {
    return 1
}

too_large: u8 = 256
not_constant: i64 = runtime()
uninitialized: i64 = ---
wrong_number: i32 = 1.5
wrong_nil: i32 = nil
wrong_mode: Mode = .Missing
)draft");
  EXPECT(state, !invalid.analysis.ok);
  const std::string rendered =
      draft::render_diagnostics(invalid.sources, invalid.diagnostics);
  EXPECT(state, rendered.find("not representable") != std::string::npos);
  EXPECT(state, rendered.find("not compile-time evaluable") != std::string::npos);
  EXPECT(state, rendered.find("automatic local") != std::string::npos);
  EXPECT(state, rendered.find("incompatible with global type") != std::string::npos);
  EXPECT(state, rendered.find("names no member") != std::string::npos);
}

} // namespace

int main() {
  TestState state;
  test_constants_and_conditional_rounds(state);
  test_invalid_required_constants(state);
  test_global_initializers(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " constant evaluation expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all constant evaluation tests passed\n";
  return EXIT_SUCCESS;
}
