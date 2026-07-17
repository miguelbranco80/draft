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
)draft");

  if (source.diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(source.sources, source.diagnostics);
  }
  EXPECT(state, source.analysis.ok);
  EXPECT(state, !source.diagnostics.has_errors());
  EXPECT(state, source.analysis.selections.entries.size() == 6);
  EXPECT(state, find_symbol(source.analysis.package, "Word").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Platform_Value").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Nested").has_value());
  EXPECT(state, find_symbol(source.analysis.package, "Good").has_value());
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
  EXPECT(state, bits.has_value());
  EXPECT(state, derived.has_value());
  EXPECT(state, feature.has_value());
  EXPECT(state, bit_value.has_value());
  if (bits && derived && feature && bit_value) {
    const draft::ConstantValue *bits_value = source.analysis.constants.find(*bits);
    const draft::ConstantValue *derived_value = source.analysis.constants.find(*derived);
    const draft::ConstantValue *feature_value = source.analysis.constants.find(*feature);
    const draft::ConstantValue *bit_result = source.analysis.constants.find(*bit_value);
    EXPECT(state, bits_value != nullptr);
    EXPECT(state, derived_value != nullptr);
    EXPECT(state, feature_value != nullptr);
    EXPECT(state, bit_result != nullptr);
    if (bits_value) EXPECT(state, bits_value->integer == 64);
    if (derived_value) EXPECT(state, derived_value->integer == 42);
    if (feature_value) EXPECT(state, feature_value->boolean);
    if (bit_result) EXPECT(state, bit_result->integer == 65);
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

} // namespace

int main() {
  TestState state;
  test_constants_and_conditional_rounds(state);
  test_invalid_required_constants(state);

  if (state.failures != 0) {
    std::cerr << state.failures << " constant evaluation expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all constant evaluation tests passed\n";
  return EXIT_SUCCESS;
}
