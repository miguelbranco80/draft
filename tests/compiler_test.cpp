// Dependency-ordered full provider-free compiler pipeline tests.

#include "compile/compiler.h"
#include "base/timing.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include "test_directory.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "compiler_test.cpp:" << line << ": expectation failed: "
                << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

void test_procedure_demand_sets_are_canonical_and_exact(TestState &state) {
  draft::ProcedureInstantiationDemand first;
  first.public_template_name = "render";
  first.instance_name = "render$mono$first";
  first.digest = draft::sha256("first semantic packet");
  draft::ProcedureInstantiationDemand second;
  second.public_template_name = "render";
  second.instance_name = "render$mono$second";
  second.digest = draft::sha256("second semantic packet");

  draft::DiagnosticSink diagnostics;
  std::vector<draft::ProcedureInstantiationDemand> duplicated{second, first,
                                                              first};
  EXPECT(state, draft::canonicalize_procedure_demands(duplicated, diagnostics));
  EXPECT(state, duplicated.size() == 2);
  if (duplicated.size() == 2) {
    EXPECT(state, duplicated[0].instance_name == first.instance_name);
    EXPECT(state, duplicated[1].instance_name == second.instance_name);
  }
  EXPECT(state, !diagnostics.has_errors());

  std::vector<draft::ProcedureInstantiationDemand> previous{first};
  std::vector<draft::ProcedureInstantiationDemand> current{first, second};
  std::vector<draft::ProcedureInstantiationDemand> added;
  EXPECT(state, draft::added_procedure_demands(previous, current, added));
  EXPECT(state, added.size() == 1);
  if (added.size() == 1) {
    EXPECT(state, added.front().digest == second.digest);
  }
  added.clear();
  EXPECT(state, !draft::added_procedure_demands(current, previous, added));

  // Native names contain a shortened digest for inspectability. The complete
  // digest remains the semantic key, so two different packets with the same
  // shortened spelling must fail instead of sharing whichever request happened
  // to be visited first.
  draft::ProcedureInstantiationDemand collision = first;
  collision.digest = draft::sha256("different full semantic packet");
  std::vector<draft::ProcedureInstantiationDemand> colliding{first, collision};
  draft::DiagnosticSink collision_diagnostics;
  EXPECT(state, !draft::canonicalize_procedure_demands(colliding,
                                                       collision_diagnostics));
  EXPECT(state, collision_diagnostics.has_errors());
}

// Proves that local named constants have independent graph rows, exact dynamic
// edges, deterministic owner tables, and published values including a distinct
// result type.
void test_named_constants_are_semantic_products(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-constant-product-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  std::ofstream app(root / "app" / "package.draft", std::ios::binary);
  app << "package app\n"
         "Pair :: struct {\n"
         "    value: Duration,\n"
         "}\n"
         "Computed :: struct {\n"
         "    values: [computed_count(2)]i64,\n"
         "}\n"
         "computed_count :: proc(value: usize) -> usize {\n"
         "    return value + 1\n"
         "}\n"
         "Record :: struct {\n"
         "    value: i64,\n"
         "}\n"
         "Duration :: distinct i64\n"
         "Second :: cast[Duration](1000 * 1000 * 1000)\n"
         "Base :: 40\n"
         "Derived :: Base + 2\n"
         "Untyped_Tuple :: (1, 2.5)\n"
         "Compile_Record :: Record{value = 42}\n"
         "main :: proc() {\n"
         "    static_assert(Derived == 42)\n"
         "    static_assert(size_of(Computed) == 24)\n"
         "}\n";
  app.close();
  EXPECT(state, app.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources, (root / "app").string(), options, diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, compiled.ok);
  EXPECT(state, compiled.packages.size() == 1);
  if (!compiled.ok || compiled.packages.size() != 1 ||
      !compiled.packages.front().has_value()) {
    return;
  }

  const draft::CompiledPackage &package = *compiled.packages.front();
  const draft::SemanticPackage &semantic = package.declarations.package;
  const std::optional<draft::SymbolId> base =
      semantic.symbols.lookup_direct(semantic.package_scope, "Base");
  const std::optional<draft::SymbolId> derived =
      semantic.symbols.lookup_direct(semantic.package_scope, "Derived");
  const std::optional<draft::SymbolId> second =
      semantic.symbols.lookup_direct(semantic.package_scope, "Second");
  const std::optional<draft::SymbolId> duration =
      semantic.symbols.lookup_direct(semantic.package_scope, "Duration");
  const std::optional<draft::SymbolId> pair =
      semantic.symbols.lookup_direct(semantic.package_scope, "Pair");
  const std::optional<draft::SymbolId> untyped_tuple =
      semantic.symbols.lookup_direct(semantic.package_scope, "Untyped_Tuple");
  const std::optional<draft::SymbolId> computed =
      semantic.symbols.lookup_direct(semantic.package_scope, "Computed");
  const std::optional<draft::SymbolId> computed_count =
      semantic.symbols.lookup_direct(semantic.package_scope, "computed_count");
  const std::optional<draft::SymbolId> record =
      semantic.symbols.lookup_direct(semantic.package_scope, "Record");
  const std::optional<draft::SymbolId> compile_record =
      semantic.symbols.lookup_direct(semantic.package_scope, "Compile_Record");
  EXPECT(state, base.has_value());
  EXPECT(state, derived.has_value());
  EXPECT(state, second.has_value());
  EXPECT(state, duration.has_value());
  EXPECT(state, pair.has_value());
  EXPECT(state, untyped_tuple.has_value());
  EXPECT(state, computed.has_value());
  EXPECT(state, computed_count.has_value());
  EXPECT(state, record.has_value());
  EXPECT(state, compile_record.has_value());
  if (!base || !derived || !second || !duration || !pair || !untyped_tuple ||
      !computed || !computed_count || !record || !compile_record) {
    return;
  }

  const draft::PackageSemanticProducts &products =
      compiled.semantic_products.packages.front();
  EXPECT(state, compiled.semantic_products.package_by_product.size() ==
                    compiled.semantic_graph.products.size());
  EXPECT(state, compiled.semantic_products.constant_by_product.size() ==
                    compiled.semantic_graph.products.size());
  EXPECT(state, compiled.semantic_products.declaration_by_product.size() ==
                    compiled.semantic_graph.products.size());
  EXPECT(state, compiled.semantic_products.type_by_product.size() ==
                    compiled.semantic_graph.products.size());
  EXPECT(state, compiled.semantic_products.condition_by_product.size() ==
                    compiled.semantic_graph.products.size());
  EXPECT(state,
         compiled.semantic_products.generic_type_demand_by_product.size() ==
             compiled.semantic_graph.products.size());
  EXPECT(state, products.constants.size() == 5);
  std::optional<draft::SemanticProductId> base_product;
  std::optional<draft::SemanticProductId> derived_product;
  std::optional<draft::SemanticProductId> compile_record_product;
  for (draft::SemanticProductId product : products.constants) {
    EXPECT(state,
        compiled.semantic_graph.products[product.value].kind ==
            draft::SemanticProductKind::ConstantValue);
    EXPECT(state,
        compiled.semantic_graph.products[product.value].state ==
            draft::SemanticProductState::Complete);
    const draft::SymbolId root_symbol =
        compiled.semantic_products.constant_by_product[product.value];
    if (root_symbol == *base)
      base_product = product;
    if (root_symbol == *derived)
      derived_product = product;
    if (root_symbol == *compile_record)
      compile_record_product = product;
  }
  EXPECT(state, base_product.has_value());
  EXPECT(state, derived_product.has_value());
  if (base_product && derived_product) {
    const std::vector<draft::SemanticProductId> &dependencies =
        compiled.semantic_graph.products[derived_product->value].dependencies;
    EXPECT(state,
        std::find(dependencies.begin(), dependencies.end(), *base_product) !=
            dependencies.end());
  }

  // Pair is collected before Duration. Its MemberTypes product must acquire an
  // exact dynamic edge to Duration's TypeIdentity product instead of asking a
  // recursive resolver to complete the forward declaration. Natural layout is
  // a second product consuming the completed member packet.
  std::optional<draft::SemanticProductId> pair_type_product;
  std::optional<draft::SemanticProductId> duration_type_product;
  std::optional<draft::SemanticProductId> computed_type_product;
  std::optional<draft::SemanticProductId> computed_count_type_product;
  std::optional<draft::SemanticProductId> record_type_product;
  for (draft::SemanticProductId product : products.declaration_types) {
    const draft::SymbolId declaration =
        compiled.semantic_products.declaration_by_product[product.value];
    if (declaration == *pair)
      pair_type_product = product;
    if (declaration == *duration)
      duration_type_product = product;
    if (declaration == *computed)
      computed_type_product = product;
    if (declaration == *computed_count)
      computed_count_type_product = product;
    if (declaration == *record)
      record_type_product = product;
  }
  EXPECT(state, pair_type_product.has_value());
  EXPECT(state, duration_type_product.has_value());
  if (pair_type_product && duration_type_product) {
    EXPECT(state,
           compiled.semantic_graph.products[pair_type_product->value].kind ==
               draft::SemanticProductKind::TypeMemberTypes);
    EXPECT(
        state,
        compiled.semantic_graph.products[duration_type_product->value].kind ==
            draft::SemanticProductKind::TypeIdentity);
    const std::vector<draft::SemanticProductId> &dependencies =
        compiled.semantic_graph.products[pair_type_product->value].dependencies;
    EXPECT(state, std::find(dependencies.begin(), dependencies.end(),
                            *duration_type_product) != dependencies.end());
  }
  // The array count is a full Draft compile-time call, but the expression is
  // still part of Computed's coherent member-type product. Its callee signature
  // is an exact graph prerequisite; no semantic round or expression task is
  // involved.
  EXPECT(state, computed_type_product.has_value());
  EXPECT(state, computed_count_type_product.has_value());
  if (computed_type_product && computed_count_type_product) {
    const std::vector<draft::SemanticProductId> &dependencies =
        compiled.semantic_graph.products[computed_type_product->value]
            .dependencies;
    EXPECT(state,
           std::find(dependencies.begin(), dependencies.end(),
                     *computed_count_type_product) != dependencies.end());
  }
  // A composite constant may see Record's eager identity before its member
  // packet. The evaluator must block on that packet instead of treating the
  // temporarily empty field table as the completed aggregate definition.
  EXPECT(state, record_type_product.has_value());
  EXPECT(state, compile_record_product.has_value());
  if (record_type_product && compile_record_product) {
    const std::vector<draft::SemanticProductId> &dependencies =
        compiled.semantic_graph.products[compile_record_product->value]
            .dependencies;
    EXPECT(state, std::find(dependencies.begin(), dependencies.end(),
                            *record_type_product) != dependencies.end());
  }
  std::optional<draft::SemanticProductId> pair_layout_product;
  for (draft::SemanticProductId product : products.natural_layouts) {
    if (compiled.semantic_products.type_by_product[product.value] ==
        semantic.symbols.symbol(*pair).type) {
      pair_layout_product = product;
      break;
    }
  }
  EXPECT(state, pair_layout_product.has_value());
  if (pair_layout_product && pair_type_product) {
    EXPECT(state,
           compiled.semantic_graph.products[pair_layout_product->value].kind ==
               draft::SemanticProductKind::TypeNaturalLayout);
    const std::vector<draft::SemanticProductId> &dependencies =
        compiled.semantic_graph.products[pair_layout_product->value]
            .dependencies;
    EXPECT(state, std::find(dependencies.begin(), dependencies.end(),
                            *pair_type_product) != dependencies.end());
  }
  const draft::ConstantValue *value =
      package.declarations.constants.find(*derived);
  EXPECT(state, value != nullptr);
  if (value != nullptr) {
    EXPECT(state, value->kind == draft::ConstantKind::Integer);
    EXPECT(state, value->integer == draft::BigInteger::from_u64(42));
  }
  const draft::ConstantValue *duration_value =
      package.declarations.constants.find(*second);
  EXPECT(state, duration_value != nullptr);
  if (duration_value != nullptr) {
    EXPECT(state, duration_value->kind == draft::ConstantKind::Integer);
    EXPECT(state,
        duration_value->integer ==
            draft::BigInteger::from_u64(1000ULL * 1000ULL * 1000ULL));
  }
  const draft::Type &tuple_type =
      semantic.types.type(semantic.symbols.symbol(*untyped_tuple).type);
  EXPECT(state, tuple_type.kind == draft::TypeKind::Tuple);
  EXPECT(state, tuple_type.members.size() == 2);

  std::filesystem::remove_all(root, error);
}

// A member-level `when` owns the selected member namespace and therefore must
// complete before the aggregate's member packet or layout. Nested `else when`
// sites are intentionally opaque until the preceding false selection publishes;
// this regression verifies that each newly reachable site becomes an exact
// dependency instead of letting the first incomplete member attempt complete.
void test_conditional_members_extend_the_semantic_graph(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-conditional-member-product-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  std::ofstream app(root / "app" / "package.draft", std::ios::binary);
  app << "package app\n"
         "Selected :: struct {\n"
         "    first: u8,\n"
         "    when false {\n"
         "        wrong: u64,\n"
         "    } else when true {\n"
         "        chosen: u32,\n"
         "    } else {\n"
         "        also_wrong: u64,\n"
         "    }\n"
         "    last: u16,\n"
         "}\n"
         "main :: proc() {\n"
         "    value: Selected\n"
         "    value.chosen = 42\n"
         "}\n";
  app.close();
  EXPECT(state, app.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  const draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources, (root / "app").string(), std::move(options), diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, compiled.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, compiled.packages.size() == 1);
  if (!compiled.ok || compiled.packages.size() != 1 ||
      !compiled.packages.front().has_value()) {
    std::filesystem::remove_all(root, error);
    return;
  }

  const draft::CompiledPackage &package = *compiled.packages.front();
  const draft::SemanticPackage &semantic = package.declarations.package;
  const std::optional<draft::SymbolId> selected =
      semantic.symbols.lookup_direct(semantic.package_scope, "Selected");
  EXPECT(state, selected.has_value());
  if (!selected.has_value()) {
    std::filesystem::remove_all(root, error);
    return;
  }
  const draft::TypeId selected_type =
      semantic.symbols.symbol(*selected).type;
  const draft::Type &type = semantic.types.type(selected_type);
  EXPECT(state, type.layout.known);
  EXPECT(state, type.layout.size == 12);
  EXPECT(state, type.layout.alignment == 4);

  std::vector<std::string> member_names;
  for (const draft::AggregateMember &member : semantic.aggregate_members) {
    if (member.owner == *selected) {
      member_names.push_back(semantic.symbols.symbol(member.member).name);
    }
  }
  const std::vector<std::string> expected_members{"first", "chosen", "last"};
  EXPECT(state, member_names == expected_members);

  const draft::PackageSemanticProducts &products =
      compiled.semantic_products.packages.front();
  EXPECT(state, products.conditions.size() == 2);
  std::optional<draft::SemanticProductId> member_product;
  for (draft::SemanticProductId product : products.declaration_types) {
    if (compiled.semantic_products.declaration_by_product[product.value] ==
        *selected) {
      member_product = product;
      break;
    }
  }
  EXPECT(state, member_product.has_value());
  if (member_product.has_value()) {
    const std::vector<draft::SemanticProductId> &dependencies =
        compiled.semantic_graph.products[member_product->value].dependencies;
    for (draft::SemanticProductId condition : products.conditions) {
      EXPECT(state,
             compiled.semantic_graph.products[condition.value].state ==
                 draft::SemanticProductState::Complete);
      EXPECT(state,
             std::find(dependencies.begin(), dependencies.end(), condition) !=
                 dependencies.end());
      // A nested condition receives only the immutable declaration inputs. If
      // it inherited PackageNameSet's dynamically enlarged dependency set, it
      // would point back to this member product and make the real graph cyclic.
      const std::vector<draft::SemanticProductId> &condition_dependencies =
          compiled.semantic_graph.products[condition.value].dependencies;
      EXPECT(state,
             std::find(condition_dependencies.begin(),
                       condition_dependencies.end(), *member_product) ==
                 condition_dependencies.end());
    }
  }

  std::filesystem::remove_all(root, error);
}

// Imported constants are completed dependency-interface inputs, not duplicate
// consumer products. They must nevertheless enter the consumer's local proxy
// table so body checking and typed validation context observe the same value.
void test_imported_constant_products_enter_consumer_table(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-imported-constant-product-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  std::filesystem::create_directories(root / "units", error);
  EXPECT(state, !error);
  if (error) return;

  std::ofstream units(root / "units" / "package.draft", std::ios::binary);
  units << "package units\n"
           "pub Duration :: distinct i64\n"
           "pub second :: cast[Duration](1000 * 1000 * 1000)\n";
  units.close();
  EXPECT(state, units.good());

  std::ofstream app(root / "app" / "package.draft", std::ios::binary);
  app << "package app\n"
         "import units\n"
         "main :: proc() {\n"
         "    static_assert(units.second == cast[units.Duration](1000000000))\n"
         "}\n";
  app.close();
  EXPECT(state, app.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources, (root / "app").string(), options, diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, compiled.ok);
  if (!compiled.ok) return;

  const draft::CompiledPackage *app_package = nullptr;
  const draft::CompiledPackage *units_package = nullptr;
  for (const std::optional<draft::CompiledPackage> &package :
       compiled.packages) {
    if (!package.has_value()) continue;
    if (package->identity.root_relative_path == "app") {
      app_package = &*package;
    } else if (package->identity.root_relative_path == "units") {
      units_package = &*package;
    }
  }
  EXPECT(state, app_package != nullptr);
  EXPECT(state, units_package != nullptr);
  if (app_package == nullptr || units_package == nullptr) return;

  std::optional<draft::SymbolId> imported_second;
  for (const draft::ImportedSymbol &imported :
       app_package->declarations.package.imported_symbols) {
    if (imported.root_relative_path == "units" &&
        imported.public_name == "second") {
      imported_second = imported.proxy;
      break;
    }
  }
  EXPECT(state, imported_second.has_value());
  if (!imported_second.has_value()) return;
  const draft::ConstantValue *value =
      app_package->declarations.constants.find(*imported_second);
  EXPECT(state, value != nullptr);
  if (value != nullptr) {
    EXPECT(state, value->kind == draft::ConstantKind::Integer);
    EXPECT(state,
        value->integer ==
            draft::BigInteger::from_u64(1000ULL * 1000ULL * 1000ULL));
  }

  const auto declaration = std::find_if(
      units_package->interface.declarations.begin(),
      units_package->interface.declarations.end(),
      [](const draft::InterfaceDeclaration &candidate) {
        return candidate.name == "second";
      });
  EXPECT(state, declaration != units_package->interface.declarations.end());
  if (declaration != units_package->interface.declarations.end()) {
    EXPECT(state, declaration->has_constant);
  }

  std::filesystem::remove_all(root, error);
}
[[nodiscard]] std::size_t occurrence_count(std::string_view text,
                                           std::string_view needle) {
  std::size_t count = 0;
  std::size_t cursor = 0;
  while (true) {
    cursor = text.find(needle, cursor);
    if (cursor == std::string_view::npos)
      return count;
    ++count;
    cursor += needle.size();
  }
}

void test_source_update_reuses_unaffected_semantics(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-compiler-source-update-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  std::filesystem::create_directories(root / "middle", error);
  std::filesystem::create_directories(root / "changed", error);
  std::filesystem::create_directories(root / "stable", error);
  EXPECT(state, !error);
  if (error)
    return;

  std::ofstream app(root / "app" / "package.draft", std::ios::binary);
  app << "package app\n"
         "import middle\n"
         "import stable\n"
         "main :: proc() {}\n";
  app.close();
  std::ofstream middle(root / "middle" / "package.draft", std::ios::binary);
  middle << "package middle\n"
            "import changed\n"
            "pub Value :: changed.Value\n";
  middle.close();
  std::ofstream changed(root / "changed" / "package.draft", std::ios::binary);
  changed << "package changed\npub Value :: 1\n";
  changed.close();
  std::ofstream stable(root / "stable" / "package.draft", std::ios::binary);
  stable << "package stable\npub Value :: 10\n";
  stable.close();
  EXPECT(state, app.good() && middle.good() && changed.good() && stable.good());

  draft::TimingRecorder timings(draft::TimingOutput::Summary);
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  options.stage = draft::CompileWorkspaceStage::DiscoverInterfaceSynthesis;
  options.timings = &timings;
  draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources, (root / "app").string(), options, diagnostics);
  EXPECT(state, compiled.ok);
  EXPECT(state, compiled.graph.packages.size() == 4);
  EXPECT(state, compiled.semantic_products.package_by_product.size() ==
                    compiled.semantic_graph.products.size());
  EXPECT(state,
         compiled.semantic_products.generic_type_demand_by_product.size() ==
             compiled.semantic_graph.products.size());
  EXPECT(state, compiled.semantic_graph
                        .products[compiled.semantic_products.target.value]
                        .state == draft::SemanticProductState::Complete);
  EXPECT(state,
         draft::freeze_semantic_ready_wave(compiled.semantic_graph).status ==
             draft::SemanticReadyWaveStatus::Complete);

  const draft::SemanticProductId initial_source_generation =
      compiled.semantic_products.source_generation;
  const std::vector<draft::PackageSemanticProducts> initial_products =
      compiled.semantic_products.packages;
  std::optional<std::size_t> changed_index;
  std::optional<std::size_t> stable_index;
  for (std::size_t index = 0; index < compiled.graph.packages.size(); ++index) {
    const std::string &path =
        compiled.graph.packages[index].identity.root_relative_path;
    if (path == "changed")
      changed_index = index;
    if (path == "stable")
      stable_index = index;
    EXPECT(state, compiled.semantic_graph
                          .products[initial_products[index].name_set.value]
                          .state == draft::SemanticProductState::Complete);
    EXPECT(state,
           compiled.semantic_graph
                   .products[initial_products[index].package_interface.value]
                   .state == draft::SemanticProductState::Complete);
  }
  EXPECT(state, changed_index.has_value());
  EXPECT(state, stable_index.has_value());

  draft::WorkspaceSourceOverride source_override;
  source_override.identity = {"workspace", "changed"};
  source_override.source.relative_name = "package.draft";
  source_override.source.contents = "package changed\npub Value :: 2\n";
  EXPECT(state, draft::apply_compiled_workspace_source_overrides(
                    sources, {source_override},
                    draft::WorkspaceSemanticChange::Interface, options,
                    compiled, diagnostics));
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, compiled.ok);
  EXPECT(state, compiled.progress ==
                    draft::CompileWorkspaceProgress::InterfaceDiscovery);

  EXPECT(state, compiled.semantic_products.source_generation !=
                    initial_source_generation);
  EXPECT(
      state,
      compiled.semantic_graph.products[initial_source_generation.value].state ==
          draft::SemanticProductState::Complete);
  if (changed_index.has_value()) {
    EXPECT(state,
           compiled.semantic_graph
                   .products[initial_products[*changed_index].name_set.value]
                   .state == draft::SemanticProductState::Superseded);
    EXPECT(state,
           compiled.semantic_products.packages[*changed_index].parsed_files !=
               initial_products[*changed_index].parsed_files);
    EXPECT(state,
           compiled.semantic_graph
                   .products[compiled.semantic_products.packages[*changed_index]
                                 .package_interface.value]
                   .state == draft::SemanticProductState::Complete);
  }
  if (stable_index.has_value()) {
    EXPECT(state, compiled.semantic_products.packages[*stable_index].name_set ==
                      initial_products[*stable_index].name_set);
    EXPECT(state,
           compiled.semantic_products.packages[*stable_index].parsed_files ==
               initial_products[*stable_index].parsed_files);
  }
  EXPECT(state,
         draft::freeze_semantic_ready_wave(compiled.semantic_graph).status ==
             draft::SemanticReadyWaveStatus::Complete);

  // Initial analysis visits all four packages. Replacing changed then revisits
  // changed, its direct middle consumer, and the transitive app consumer, but
  // not the independent stable dependency. The same recorder also proves that
  // the workspace was loaded once and transitioned in memory once.
  const std::string report = timings.render();
  EXPECT(state,
         report.find("package semantic analyses: 7") != std::string::npos);
  EXPECT(state, report.find("workspace loads: 1") != std::string::npos);
  EXPECT(state,
         report.find("workspace source transitions: 1") != std::string::npos);

  std::filesystem::remove_all(root, error);
}

void test_interface_synthesis_is_an_explicit_graph_wait(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-interface-product-wait-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error)
    return;

  std::ofstream app(root / "app" / "package.draft", std::ios::binary);
  app << "package app\n"
         "... \"Declare a package constant named Value with value 1\"\n";
  app.close();
  EXPECT(state, app.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  options.stage = draft::CompileWorkspaceStage::DiscoverInterfaceSynthesis;
  draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources, (root / "app").string(), options, diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, compiled.ok);
  EXPECT(state, compiled.packages.size() == 1);
  if (!compiled.ok || compiled.packages.size() != 1)
    return;

  const draft::PackageSemanticProducts &products =
      compiled.semantic_products.packages.front();
  EXPECT(state, products.opaque_synthesis_set.is_valid());
  EXPECT(state,
         compiled.semantic_graph.products[products.name_set.value].state ==
             draft::SemanticProductState::Waiting);
  EXPECT(state,
         compiled.semantic_graph.products[products.opaque_synthesis_set.value]
                 .state == draft::SemanticProductState::WaitingForSynthesis);
  EXPECT(state,
         compiled.semantic_graph.products[products.package_interface.value]
                 .state == draft::SemanticProductState::Waiting);
  const std::vector<draft::SemanticProductId> &name_dependencies =
      compiled.semantic_graph.products[products.name_set.value].dependencies;
  EXPECT(state,
         std::find(name_dependencies.begin(), name_dependencies.end(),
                   products.opaque_synthesis_set) != name_dependencies.end());
  EXPECT(state,
         draft::freeze_semantic_ready_wave(compiled.semantic_graph).status ==
             draft::SemanticReadyWaveStatus::WaitingForSynthesis);
  EXPECT(state, compiled.packages.front().has_value());
  if (compiled.packages.front().has_value()) {
    EXPECT(state, !compiled.packages.front()->obligations.obligations.empty());
  }

  std::filesystem::remove_all(root, error);
}

void test_body_source_update_reuses_closed_generic_dependency(
    TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-body-work-reuse-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  std::filesystem::create_directories(root / "formatting", error);
  EXPECT(state, !error);
  if (error)
    return;

  std::ofstream app(root / "app" / "package.draft", std::ios::binary);
  app << "package app\n"
         "import formatting\n"
         "main :: proc() {\n"
         "    expected: i64 = ... \"produce 42\"\n"
         "    formatting.consume(\"value\", expected)\n"
         "}\n";
  app.close();
  std::ofstream formatting(root / "formatting" / "package.draft",
                           std::ios::binary);
  formatting
      << "package formatting\n"
         "pub consume :: proc(values: ..type) {\n"
         "    for value in values {\n"
         "        when type_of(value) == string {\n"
         "        } else when type_kind(type_of(value)) == .signed_integer {\n"
         "        } else {\n"
         "            static_assert(false, \"unsupported value\")\n"
         "        }\n"
         "    }\n"
         "}\n";
  formatting.close();
  EXPECT(state, app.good() && formatting.good());

  draft::TimingRecorder timings(draft::TimingOutput::Summary);
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  options.timings = &timings;
  draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources, (root / "app").string(), options, diagnostics);
  EXPECT(state, compiled.ok);
  EXPECT(state,
         compiled.progress == draft::CompileWorkspaceProgress::SemanticClosure);
  if (!compiled.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return;
  }

  const std::size_t app_index = compiled.graph.root_package.value;
  std::optional<std::size_t> formatting_index;
  for (std::size_t index = 0; index < compiled.packages.size(); ++index) {
    if (compiled.packages[index].has_value() &&
        compiled.packages[index]->identity.root_relative_path == "formatting") {
      formatting_index = index;
      break;
    }
  }
  EXPECT(state, formatting_index.has_value());
  if (!formatting_index.has_value())
    return;

  const draft::CompiledPackage &initial_formatting =
      *compiled.packages[*formatting_index];
  const std::uint64_t formatting_declaration_generation =
      initial_formatting.declaration_generation;
  const std::uint64_t formatting_body_generation =
      initial_formatting.body_work_key.declaration_generation;
  const std::size_t formatting_symbol_count =
      initial_formatting.bodies.package.symbols.symbol_count();
  EXPECT(state,
         initial_formatting.declarations.package.parametric_instances.empty());
  EXPECT(state,
         initial_formatting.bodies.package.parametric_instances.size() == 1);

  draft::WorkspaceSourceOverride source_override;
  source_override.identity = {"workspace", "app"};
  source_override.source.relative_name = "package.draft";
  source_override.source.contents =
      "package app\n"
      "import formatting\n"
      "main :: proc() {\n"
      "    expected: i64 = 42\n"
      "    formatting.consume(\"value\", expected)\n"
      "}\n";
  EXPECT(state,
         draft::apply_compiled_workspace_source_overrides(
             sources, {source_override}, draft::WorkspaceSemanticChange::Body,
             options, compiled, diagnostics));
  EXPECT(state,
         draft::continue_compiled_workspace_semantics(
             sources, (root / "app").string(), options, compiled, diagnostics));
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, compiled.ok);
  EXPECT(state,
         compiled.progress == draft::CompileWorkspaceProgress::SemanticClosure);

  const draft::CompiledPackage &updated_app = *compiled.packages[app_index];
  const draft::CompiledPackage &updated_formatting =
      *compiled.packages[*formatting_index];
  EXPECT(state, updated_app.declaration_generation == 2);
  EXPECT(state, updated_formatting.declaration_generation ==
                    formatting_declaration_generation);
  EXPECT(state, updated_formatting.body_work_key.declaration_generation ==
                    formatting_body_generation);
  EXPECT(state, updated_formatting.bodies.package.symbols.symbol_count() ==
                    formatting_symbol_count);
  EXPECT(state,
         updated_formatting.bodies.package.parametric_instances.size() == 1);

  // Two packages are checked for the surface graph. The body replacement
  // checks only app; formatting's equal declaration+demand work key is reused.
  const std::string report = timings.render();
  EXPECT(state, report.find("package body checks: 3") != std::string::npos);
  EXPECT(state, report.find("package body reuses: 1") != std::string::npos);
}

void test_body_work_graph_extends_and_removes_generic_demand(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-body-demand-transition-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  std::filesystem::create_directories(root / "formatting", error);
  EXPECT(state, !error);
  if (error)
    return;

  std::ofstream app(root / "app" / "package.draft", std::ios::binary);
  app << "package app\n"
         "import formatting\n"
         "main :: proc() {\n"
         "    ... \"print one value\"\n"
         "}\n";
  app.close();
  std::ofstream formatting(root / "formatting" / "package.draft",
                           std::ios::binary);
  formatting << "package formatting\n"
                "pub consume :: proc(values: ..type) {\n"
                "    for value in values {\n"
                "        when type_kind(type_of(value)) == .signed_integer {\n"
                "        } else {\n"
                "            static_assert(false, \"unsupported value\")\n"
                "        }\n"
                "    }\n"
                "}\n";
  formatting.close();

  draft::TimingRecorder timings(draft::TimingOutput::Summary);
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  options.timings = &timings;
  draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources, (root / "app").string(), options, diagnostics);
  EXPECT(state, compiled.ok);
  if (!compiled.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return;
  }

  std::optional<std::size_t> formatting_index;
  for (std::size_t index = 0; index < compiled.packages.size(); ++index) {
    if (compiled.packages[index].has_value() &&
        compiled.packages[index]->identity.root_relative_path == "formatting") {
      formatting_index = index;
      break;
    }
  }
  EXPECT(state, formatting_index.has_value());
  if (!formatting_index.has_value())
    return;
  EXPECT(state, compiled.packages[*formatting_index]
                    ->bodies.package.parametric_instances.empty());
  const std::size_t declaration_symbol_count =
      compiled.packages[*formatting_index]
          ->declarations.package.symbols.symbol_count();
  const std::size_t initial_body_symbol_count =
      compiled.packages[*formatting_index]
          ->bodies.package.symbols.symbol_count();
  const std::size_t initial_checked =
      compiled.packages[*formatting_index]->bodies.checked_procedures;

  draft::WorkspaceSourceOverride add_demand;
  add_demand.identity = {"workspace", "app"};
  add_demand.source.relative_name = "package.draft";
  add_demand.source.contents = "package app\n"
                               "import formatting\n"
                               "main :: proc() {\n"
                               "    formatting.consume(42)\n"
                               "}\n";
  EXPECT(state, draft::apply_compiled_workspace_source_overrides(
                    sources, {add_demand}, draft::WorkspaceSemanticChange::Body,
                    options, compiled, diagnostics));
  EXPECT(state,
         draft::continue_compiled_workspace_semantics(
             sources, (root / "app").string(), options, compiled, diagnostics));
  const draft::CompiledPackage &extended =
      *compiled.packages[*formatting_index];
  EXPECT(state, extended.body_work_key.procedure_demands.size() == 1);
  EXPECT(state, extended.bodies.package.parametric_instances.size() == 1);
  EXPECT(state, extended.bodies.checked_procedures == initial_checked + 1);
  EXPECT(state, extended.declarations.package.symbols.symbol_count() ==
                    declaration_symbol_count);

  // Removing the only demand cannot leave the old executable specialization
  // in the final package. The scheduler falls back to the immutable
  // declaration baseline and reconstructs the exact empty-demand body result.
  draft::WorkspaceSourceOverride remove_demand;
  remove_demand.identity = {"workspace", "app"};
  remove_demand.source.relative_name = "package.draft";
  remove_demand.source.contents = "package app\n"
                                  "import formatting\n"
                                  "main :: proc() {\n"
                                  "}\n";
  EXPECT(state,
         draft::apply_compiled_workspace_source_overrides(
             sources, {remove_demand}, draft::WorkspaceSemanticChange::Body,
             options, compiled, diagnostics));
  EXPECT(state,
         draft::continue_compiled_workspace_semantics(
             sources, (root / "app").string(), options, compiled, diagnostics));
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  const draft::CompiledPackage &removed = *compiled.packages[*formatting_index];
  EXPECT(state, compiled.ok);
  EXPECT(state, removed.body_work_key.procedure_demands.empty());
  EXPECT(state, removed.bodies.package.parametric_instances.empty());
  EXPECT(state, removed.bodies.package.symbols.symbol_count() ==
                    initial_body_symbol_count);

  const std::string report = timings.render();
  EXPECT(state, report.find("package body extensions: 1") != std::string::npos);
  EXPECT(state, report.find("package body checks: 5") != std::string::npos);
}

void test_body_work_graph_promotes_matching_local_instance(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-body-instance-promotion-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  std::filesystem::create_directories(root / "formatting", error);
  EXPECT(state, !error);
  if (error)
    return;

  std::ofstream app(root / "app" / "package.draft", std::ios::binary);
  app << "package app\n"
         "import formatting\n"
         "main :: proc() {\n"
         "    ... \"use formatting\"\n"
         "}\n";
  app.close();
  std::ofstream formatting(root / "formatting" / "package.draft",
                           std::ios::binary);
  formatting << "package formatting\n"
                "pub consume :: proc(values: ..type) {\n"
                "    for value in values {\n"
                "        when type_kind(type_of(value)) == .signed_integer {\n"
                "        } else {\n"
                "            static_assert(false, \"unsupported value\")\n"
                "        }\n"
                "    }\n"
                "}\n"
                "self_check :: proc() {\n"
                "    consume(42)\n"
                "}\n";
  formatting.close();

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources, (root / "app").string(), options, diagnostics);
  EXPECT(state, compiled.ok);
  if (!compiled.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
    return;
  }

  std::optional<std::size_t> formatting_index;
  for (std::size_t index = 0; index < compiled.packages.size(); ++index) {
    if (compiled.packages[index].has_value() &&
        compiled.packages[index]->identity.root_relative_path == "formatting") {
      formatting_index = index;
      break;
    }
  }
  EXPECT(state, formatting_index.has_value());
  if (!formatting_index.has_value())
    return;
  const draft::CompiledPackage &initial = *compiled.packages[*formatting_index];
  EXPECT(state, initial.bodies.package.parametric_instances.size() == 1);
  EXPECT(state, initial.interface.procedure_instances.empty());
  const std::size_t initial_symbols =
      initial.bodies.package.symbols.symbol_count();
  const std::size_t initial_procedures =
      initial.bodies.program.procedures().size();
  const std::size_t initial_checked = initial.bodies.checked_procedures;
  const draft::SymbolId initial_instance =
      initial.bodies.package.parametric_instances.front().instance;
  const std::string initial_lexical_name =
      initial.bodies.package.symbols.symbol(initial_instance).name;

  draft::WorkspaceSourceOverride add_external_demand;
  add_external_demand.identity = {"workspace", "app"};
  add_external_demand.source.relative_name = "package.draft";
  add_external_demand.source.contents = "package app\n"
                                        "import formatting\n"
                                        "main :: proc() {\n"
                                        "    formatting.consume(42)\n"
                                        "}\n";
  EXPECT(state, draft::apply_compiled_workspace_source_overrides(
                    sources, {add_external_demand},
                    draft::WorkspaceSemanticChange::Body, options, compiled,
                    diagnostics));
  EXPECT(state,
         draft::continue_compiled_workspace_semantics(
             sources, (root / "app").string(), options, compiled, diagnostics));
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  const draft::CompiledPackage &promoted =
      *compiled.packages[*formatting_index];
  EXPECT(state, promoted.bodies.package.parametric_instances.size() == 1);
  EXPECT(state,
         promoted.bodies.package.symbols.symbol_count() == initial_symbols);
  EXPECT(state,
         promoted.bodies.program.procedures().size() == initial_procedures);
  EXPECT(state, promoted.bodies.checked_procedures == initial_checked);
  EXPECT(state, promoted.interface.procedure_instances.size() == 1);
  const draft::Symbol &promoted_symbol =
      promoted.bodies.package.symbols.symbol(initial_instance);
  EXPECT(state, promoted_symbol.name == initial_lexical_name);
  EXPECT(state,
         promoted_symbol.linkage_name.find("$mono$") != std::string::npos);
  if (promoted.interface.procedure_instances.size() == 1) {
    EXPECT(state,
           promoted.interface.procedure_instances.front().instance_name.find(
               "$mono$") != std::string::npos);
  }
}

void test_target_lowering_continues_checked_graph(TestState &state) {
  const std::string workspace =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/packages";
  const std::string root = workspace + "/app";

  draft::CompileWorkspaceOptions check_options;
  check_options.target = draft::make_aarch64_macos_profile();
  check_options.workspace.workspace_directory = workspace;
  draft::CompileWorkspaceOptions interface_options = check_options;
  interface_options.stage =
      draft::CompileWorkspaceStage::DiscoverInterfaceSynthesis;
  draft::SourceManager continued_sources;
  draft::DiagnosticSink continued_diagnostics;
  draft::CompileWorkspaceResult continued = draft::compile_workspace(
      continued_sources, root, interface_options, continued_diagnostics);
  EXPECT(state, continued.ok);
  EXPECT(state,
      continued.progress ==
          draft::CompileWorkspaceProgress::InterfaceDiscovery);
  EXPECT(state,
      draft::continue_compiled_workspace_semantics(
          continued_sources,
          root,
          check_options,
          continued,
          continued_diagnostics));
  EXPECT(state,
      continued.progress == draft::CompileWorkspaceProgress::SemanticClosure);

  draft::CompileWorkspaceOptions lowering_options = check_options;
  lowering_options.lower_mir = true;
  lowering_options.emit_llvm = true;
  const bool lowered = draft::continue_compiled_workspace(
      continued_sources,
      lowering_options,
      continued,
      continued_diagnostics);
  EXPECT(state, lowered);
  EXPECT(state, continued.ok);
  EXPECT(state,
      continued.progress == draft::CompileWorkspaceProgress::TargetLowering);
  EXPECT(state, !continued_diagnostics.has_errors());

  // A direct lowering request is the behavioral oracle for the explicit
  // continuation route. Both public routes must emit byte-identical modules in
  // canonical package order.
  draft::SourceManager direct_sources;
  draft::DiagnosticSink direct_diagnostics;
  const draft::CompileWorkspaceResult direct = draft::compile_workspace(
      direct_sources, root, lowering_options, direct_diagnostics);
  if (direct_diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(direct_sources, direct_diagnostics);
  }
  EXPECT(state, direct.ok);
  EXPECT(state,
      direct.progress == draft::CompileWorkspaceProgress::TargetLowering);
  EXPECT(state, continued.packages.size() == direct.packages.size());
  if (continued.packages.size() == direct.packages.size()) {
    for (std::size_t index = 0; index < direct.packages.size(); ++index) {
      EXPECT(state,
          continued.packages[index].has_value() ==
              direct.packages[index].has_value());
      if (continued.packages[index].has_value() &&
          direct.packages[index].has_value()) {
        EXPECT(state,
            continued.packages[index]->llvm.text ==
                direct.packages[index]->llvm.text);
      }
    }
  }

  draft::DiagnosticSink repeated_diagnostics;
  EXPECT(state,
      !draft::continue_compiled_workspace(
          continued_sources,
          lowering_options,
          continued,
          repeated_diagnostics));
  EXPECT(state, repeated_diagnostics.error_count() == 1);
}

void test_multi_package_native_pipeline(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/packages";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/packages/app",
      std::move(options),
      diagnostics);

  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, result.graph.packages.size() == 2);
  EXPECT(state, result.packages.size() == 2);
  EXPECT(state, result.packages[0].has_value());
  EXPECT(state, result.packages[1].has_value());
  if (result.packages[0].has_value() && result.packages[1].has_value()) {
    EXPECT(state, result.packages[0]->llvm.ok);
    EXPECT(state, result.packages[1]->llvm.ok);
    EXPECT(state, result.packages[0]->llvm.text.find("define i32 @main") !=
        std::string::npos);
    EXPECT(state, result.packages[1]->llvm.text.find("define i32 @main") ==
        std::string::npos);
    EXPECT(state, result.packages[0]->llvm.text.find(
        "draft.workspace.lib_2Fmath.translate") != std::string::npos);
    EXPECT(state, result.packages[1]->llvm.text.find(
        "draft.workspace.lib_2Fmath.translate") != std::string::npos);
    EXPECT(state, result.packages[0]->llvm.text.find(
        "define hidden void @__draft.assert") != std::string::npos);
    EXPECT(state, result.packages[1]->llvm.text.find(
        "declare hidden void @__draft.assert") != std::string::npos);
  }
}

void test_hosted_entry_contract(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-entry-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  // i32 is a perfectly valid ordinary result type, but the hosted entry
  // contract deliberately accepts only void or the target-native `int`.
  std::ofstream source(root / "app" / "package.draft", std::ios::binary);
  source << "package app\n"
            "main :: proc() -> i32 {\n"
            "    return 0\n"
            "}\n";
  source.close();
  EXPECT(state, source.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources, (root / "app").string(), std::move(options), diagnostics);
  EXPECT(state, !result.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  if (rendered.find("main result must be void or int") == std::string::npos) {
    std::cerr << rendered;
  }
  EXPECT(state, rendered.find("main result must be void or int") !=
      std::string::npos);

  std::filesystem::remove_all(root, error);
}

void test_file_local_imports_share_one_llvm_declaration(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-duplicate-import-llvm-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  std::filesystem::create_directories(root / "lib", error);
  EXPECT(state, !error);
  if (error) return;

  std::ofstream dependency(root / "lib" / "package.draft", std::ios::binary);
  dependency <<
      "package lib\n"
      "pub base :: proc() -> i64 {\n"
      "    return 40\n"
      "}\n";
  dependency.close();
  EXPECT(state, dependency.good());

  // Imports are intentionally repeated in different files. They create two
  // file-local semantic proxies but refer to one package-qualified LLVM symbol.
  std::ofstream main_source(
      root / "app" / "package.draft", std::ios::binary);
  main_source <<
      "package app\n"
      "import lib\n"
      "main :: proc() {\n"
      "    assert(lib.base() == 40)\n"
      "}\n";
  main_source.close();
  EXPECT(state, main_source.good());
  std::ofstream other_source(
      root / "app" / "other.draft", std::ios::binary);
  other_source <<
      "package app\n"
      "import lib\n"
      "other :: proc() -> i64 {\n"
      "    return lib.base()\n"
      "}\n";
  other_source.close();
  EXPECT(state, other_source.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      (root / "app").string(),
      std::move(options),
      diagnostics);
  if (!result.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, result.graph.root_package.is_valid());
  if (result.graph.root_package.is_valid()) {
    const std::optional<draft::CompiledPackage> &package =
        result.packages[result.graph.root_package.value];
    EXPECT(state, package.has_value());
    if (package.has_value()) {
      EXPECT(state,
          occurrence_count(
              package->llvm.text,
              "declare i64 @\"draft.workspace.lib.base\"(ptr)\n") == 1);
      EXPECT(state,
          occurrence_count(
              package->llvm.text,
              "call i64 @\"draft.workspace.lib.base\"(ptr %context)") == 2);
    }
  }

  std::filesystem::remove_all(root, error);
}

void test_compiler_distributed_core(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/core-runtime",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, result.graph.packages.size() == 5);
  if (result.graph.root_package.is_valid()) {
    const std::optional<draft::CompiledPackage> &root_package =
        result.packages[result.graph.root_package.value];
    EXPECT(state, root_package.has_value());
    if (root_package.has_value()) {
      EXPECT(state, root_package->llvm.text.find(
          "observe_5Fcontext\"(ptr %l0)") != std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "observe_5Fcontext\"(ptr %l1)") != std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "define hidden void @\"__draft.runtime.default_context\"") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "define internal ptr @__draft.default_allocator") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "%draft.runtime.Allocator { ptr @__draft.default_allocator, "
          "ptr null }") != std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "%draft.runtime.Logger { ptr @__draft.default_logger, ptr null }") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "%draft.runtime.RandomGenerator { ptr @__draft.default_random, "
          "ptr null }") != std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "%draft.runtime.Allocator { ptr @__draft.temp_allocator, "
          "ptr null }") != std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "define hidden void "
          "@\"__draft.runtime.reset_temporary_allocator\"") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "call i32 @pthread_key_create") != std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "@__draft.thread_context = internal thread_local global") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "call void @\"__draft.runtime.attach_thread\"()") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "call void @\"__draft.runtime.default_context\"") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "call i64 @\"draft.workspace.core_2Druntime."
          "add_5Fcontext_5Findex\"(ptr %l1, i64 2)") !=
          std::string::npos);
      EXPECT(state, root_package->llvm.text.find(
          "call void @\"__draft.runtime.call_with_context\"") ==
          std::string::npos);

      const draft::SemanticPackage &package = root_package->bodies.package;
      const std::optional<draft::SymbolId> observe =
          package.symbols.lookup_direct(package.package_scope, "observe_context");
      const std::optional<draft::SymbolId> read_from_c =
          package.symbols.lookup_direct(package.package_scope, "read_context_from_c");
      const std::optional<draft::SymbolId> add_index =
          package.symbols.lookup_direct(package.package_scope, "add_context_index");
      const std::optional<draft::SymbolId> main =
          package.symbols.lookup_direct(package.package_scope, "main");
      EXPECT(state, observe.has_value());
      EXPECT(state, read_from_c.has_value());
      EXPECT(state, add_index.has_value());
      EXPECT(state, main.has_value());
      if (observe && read_from_c && add_index && main) {
        const draft::ProcedureEffectSummary *c_summary =
            root_package->effects.find(*read_from_c);
        const draft::ProcedureEffectSummary *main_summary =
            root_package->effects.find(*main);
        EXPECT(state, c_summary != nullptr);
        EXPECT(state, main_summary != nullptr);
        if (c_summary != nullptr) {
          EXPECT(state, std::find(
              c_summary->direct_calls.begin(),
              c_summary->direct_calls.end(),
              *observe) != c_summary->direct_calls.end());
          EXPECT(state, std::none_of(
              c_summary->effects.begin(),
              c_summary->effects.end(),
              [](const draft::SemanticEffect &effect) {
                return effect.kind == draft::EffectKind::UnknownCall;
              }));
        }
        if (main_summary != nullptr) {
          EXPECT(state, std::find(
              main_summary->direct_calls.begin(),
              main_summary->direct_calls.end(),
              *add_index) != main_summary->direct_calls.end());
        }
      }
    }
  }

  // The source-visible Context and the entry shim intentionally duplicate one
  // versioned physical contract.  This fixed layout check makes drift fail in
  // the ordinary compiler suite instead of appearing as a callback crash.
  const draft::CompiledPackage *runtime = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (package.has_value() &&
        package->identity.root_identity == "draft-core-test-v1" &&
        package->identity.root_relative_path == "runtime") {
      runtime = &*package;
      break;
    }
  }
  EXPECT(state, runtime != nullptr);
  if (runtime == nullptr) return;
  EXPECT(state, runtime->native_interop.providers.size() == 1);
  if (runtime->native_interop.providers.size() == 1) {
    EXPECT(state, runtime->native_interop.providers.front() == "draft_runtime");
  }
  const std::optional<draft::SymbolId> context =
      runtime->bodies.package.symbols.lookup_direct(
          runtime->bodies.package.package_scope, "Context");
  EXPECT(state, context.has_value());
  if (!context.has_value()) return;
  const draft::Type &type = runtime->bodies.package.types.type(
      runtime->bodies.package.symbols.symbol(*context).type);
  EXPECT(state, type.layout.known);
  EXPECT(state, type.layout.size == 96);
  EXPECT(state, type.layout.alignment == 8);
  EXPECT(state, type.member_offsets == std::vector<std::uint64_t>({
      0, 16, 32, 40, 56, 72, 80, 88}));
}

// `console.println` is ordinary cross-package Draft code, so an unsupported
// concrete pack type must select the library's dependent failure branch while
// compiling the owner specialization. This test guards both the public core
// policy and the diagnostic range propagated through the instance request.
void test_console_println_rejects_unsupported_pack_type(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-console-unsupported-pack-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  std::ofstream app(root / "app" / "package.draft", std::ios::binary);
  app << "package app\n"
         "import core/console\n"
         "main :: proc() {\n"
         "    console.println(1.5)\n"
         "}\n";
  app.close();
  EXPECT(state, app.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources, (root / "app").string(), std::move(options), diagnostics);

  EXPECT(state, !result.ok);
  bool found = false;
  for (const draft::Diagnostic &diagnostic : diagnostics.diagnostics()) {
    if (diagnostic.message !=
        "static assertion failed: console.println does not support this type") {
      continue;
    }
    found = true;
    EXPECT(state, diagnostic.range.is_valid());
    if (diagnostic.range.is_valid()) {
      EXPECT(
          state,
          sources.text(diagnostic.range).find("static_assert(false") !=
              std::string_view::npos);
    }
  }
  if (!found && diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, found);

  std::filesystem::remove_all(root, error);
}

void test_compiler_distributed_memory(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/core-memory",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, result.graph.packages.size() == 3);
  if (!result.ok || !result.graph.root_package.is_valid()) return;
  const std::optional<draft::CompiledPackage> &root =
      result.packages[result.graph.root_package.value];
  EXPECT(state, root.has_value());
  if (!root.has_value()) return;
  EXPECT(state, root->llvm.text.find(
      ".memory.allocate\"") != std::string::npos);
  EXPECT(state, root->llvm.text.find(
      ".memory.new_24mono_24") != std::string::npos);
  EXPECT(state,
      root->bodies.package.imported_procedure_instances.size() >= 9);
  EXPECT(state, root->llvm.text.find(
      "call ptr @realloc") != std::string::npos);
  EXPECT(state, root->llvm.text.find(
      "call i32 @posix_memalign") != std::string::npos);

  const draft::CompiledPackage *memory = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (package.has_value() &&
        package->identity.root_relative_path == "memory") {
      memory = &*package;
      break;
    }
  }
  EXPECT(state, memory != nullptr);
  if (memory != nullptr) {
    EXPECT(state,
        memory->bodies.package.parametric_instances.size() >= 11);
    EXPECT(state, memory->llvm.text.find(
        "@\"__draft.runtime.reset_temporary_allocator\"") !=
        std::string::npos);
    EXPECT(state, memory->llvm.text.find("arena_5Fprovider") !=
        std::string::npos);
    EXPECT(state, memory->llvm.text.find("@\"mmap\"") !=
        std::string::npos);
    EXPECT(state, memory->llvm.text.find("@\"mprotect\"") !=
        std::string::npos);
    EXPECT(state, memory->llvm.text.find("@\"munmap\"") !=
        std::string::npos);
  }
}

void test_compiler_distributed_array_and_support(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/core-array",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, result.graph.packages.size() == 9);
  if (!result.ok || !result.graph.root_package.is_valid()) return;

  const std::optional<draft::CompiledPackage> &root =
      result.packages[result.graph.root_package.value];
  EXPECT(state, root.has_value());
  if (!root.has_value()) return;
  EXPECT(state, root->llvm.ok);
  EXPECT(state, root->llvm.text.find(".array.append_24mono_24") !=
      std::string::npos);
  EXPECT(state, root->llvm.text.find(".heap.allocator\"") !=
      std::string::npos);
  EXPECT(state,
      root->bodies.package.imported_procedure_instances.size() == 5);

  const draft::CompiledPackage *array = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (package.has_value() &&
        package->identity.root_relative_path == "array") {
      array = &*package;
      break;
    }
  }
  EXPECT(state, array != nullptr);
  if (array != nullptr) {
    EXPECT(state, array->llvm.ok);
    EXPECT(state, array->bodies.package.parametric_instances.size() >= 5);
  }
}

void test_compiler_distributed_map(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/core-map",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, result.graph.packages.size() == 5);
  if (!result.ok || !result.graph.root_package.is_valid()) return;

  const std::optional<draft::CompiledPackage> &root =
      result.packages[result.graph.root_package.value];
  EXPECT(state, root.has_value());
  if (!root.has_value()) return;
  EXPECT(state, root->llvm.ok);
  EXPECT(state, root->llvm.text.find(".map.set_24mono_24") !=
      std::string::npos);
  EXPECT(state, root->llvm.text.find(".map.string_5Fkeys") !=
      std::string::npos);
  EXPECT(state,
      root->bodies.package.imported_procedure_instances.size() == 5);

  const draft::CompiledPackage *map = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (package.has_value() && package->identity.root_relative_path == "map") {
      map = &*package;
      break;
    }
  }
  EXPECT(state, map != nullptr);
  if (map != nullptr) {
    EXPECT(state, map->llvm.ok);
    EXPECT(state, map->llvm.text.find("hash_5Fstring") != std::string::npos);
    EXPECT(state, map->bodies.package.parametric_instances.size() >= 10);
  }
}

void test_compiler_distributed_os(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/core-os",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  // root, os, io, c, memory, and runtime form the native round-trip graph.
  EXPECT(state, result.graph.packages.size() == 6);
  if (!result.ok || !result.graph.root_package.is_valid()) return;

  const std::optional<draft::CompiledPackage> &root =
      result.packages[result.graph.root_package.value];
  EXPECT(state, root.has_value());
  if (!root.has_value()) return;
  EXPECT(state, root->llvm.ok);
  EXPECT(state, root->llvm.text.find(
      "define i32 @main(i32 %argc, ptr %argv, ptr %envp)") !=
      std::string::npos);
  EXPECT(state, root->llvm.text.find(
      "define hidden ptr @\"__draft.os.args_data\"") !=
      std::string::npos);
  EXPECT(state, root->llvm.text.find("@strlen") != std::string::npos);

  const draft::CompiledPackage *os = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (package.has_value() && package->identity.root_relative_path == "os") {
      os = &*package;
      break;
    }
  }
  EXPECT(state, os != nullptr);
  if (os != nullptr) {
    EXPECT(state, os->llvm.ok);
    EXPECT(state, os->llvm.text.find("@\"__draft.os.args_data\"") !=
        std::string::npos);
    EXPECT(state, os->llvm.text.find("@\"getpid\"") != std::string::npos);
    EXPECT(state, os->llvm.text.find("@\"draft_os_open_fixed\"") !=
        std::string::npos);
    EXPECT(state, os->assembly_sources.size() == 1);
    if (os->assembly_sources.size() == 1) {
    EXPECT(state, os->assembly_sources.front().relative_name ==
        "open@aarch64-macos.s");
      EXPECT(state, os->assembly_sources.front().contents.find(
          "_draft_os_open_fixed:") != std::string::npos);
    }
    EXPECT(state, os->native_interop.providers.size() == 3);
  }
}

void test_compiler_distributed_thread(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/core-thread",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, result.graph.packages.size() == 6);
  if (!result.ok || !result.graph.root_package.is_valid()) return;

  const std::optional<draft::CompiledPackage> &root =
      result.packages[result.graph.root_package.value];
  EXPECT(state, root.has_value());
  if (!root.has_value()) return;
  EXPECT(state, root->llvm.ok);
  EXPECT(state, root->llvm.text.find(
      "define hidden void @\"__draft.runtime.install_thread_context\"") !=
      std::string::npos);

  const draft::CompiledPackage *thread = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (package.has_value() &&
        package->identity.root_relative_path == "thread") {
      thread = &*package;
      break;
    }
  }
  EXPECT(state, thread != nullptr);
  if (thread != nullptr) {
    EXPECT(state, thread->llvm.ok);
    EXPECT(state, thread->llvm.text.find("@\"pthread_create\"") !=
        std::string::npos);
    EXPECT(state, thread->llvm.text.find(
        "@\"__draft.runtime.install_thread_context\"") !=
        std::string::npos);
    EXPECT(state, thread->llvm.text.find("@\"pthread_mutex_lock\"") !=
        std::string::npos);
    EXPECT(state, thread->llvm.text.find(
        "@\"__draft.runtime.default_context\"") != std::string::npos);
  }
}

// The Linux core gate runs the same public OS/thread examples through semantic,
// HIR, MIR, and LLVM construction. It checks the target-selected source seam
// before the later ELF linker test: no Darwin provider or Mach-O assembly file
// may survive merely because both hosts expose similarly named POSIX calls.
void test_aarch64_linux_core_selection(TestState &state) {
  for (const std::string_view example : {"core-os", "core-thread"}) {
    draft::SourceManager sources;
    draft::DiagnosticSink diagnostics;
    draft::CompileWorkspaceOptions options;
    options.target = draft::make_aarch64_linux_profile();
    options.workspace.workspace_directory =
        std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
    options.workspace.core_directory =
        std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
    options.workspace.core_content_identity = "draft-core-test-linux-v1";
    options.lower_mir = true;
    options.emit_llvm = true;
    const draft::CompileWorkspaceResult result = draft::compile_workspace(
        sources,
        std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/" +
            std::string(example),
        std::move(options),
        diagnostics);
    if (diagnostics.has_errors()) {
      std::cerr << draft::render_diagnostics(sources, diagnostics);
    }
    EXPECT(state, result.ok);
    EXPECT(state, !diagnostics.has_errors());
    if (!result.ok) continue;

    for (const std::optional<draft::CompiledPackage> &package : result.packages) {
      if (!package.has_value()) continue;
      EXPECT(state, package->llvm.text.find(
          "target triple = \"aarch64-unknown-linux-gnu\"") !=
          std::string::npos);
      EXPECT(state, std::find(
          package->native_interop.providers.begin(),
          package->native_interop.providers.end(),
          "darwin") == package->native_interop.providers.end());
      if (package->identity.root_relative_path != "os") continue;
      EXPECT(state, package->assembly_sources.size() == 1);
      if (package->assembly_sources.size() == 1) {
        EXPECT(state, package->assembly_sources.front().relative_name ==
            "open@aarch64-linux.s");
        EXPECT(state, package->assembly_sources.front().contents.find(
            "draft_os_open_fixed:") != std::string::npos);
        EXPECT(state, package->assembly_sources.front().contents.find(
            "_draft_os_open_fixed:") == std::string::npos);
      }
    }
  }
}

void test_compiler_distributed_atomic(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples";
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/core-atomic",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, result.graph.packages.size() == 2);
  if (!result.ok || !result.graph.root_package.is_valid()) return;

  const std::optional<draft::CompiledPackage> &root =
      result.packages[result.graph.root_package.value];
  EXPECT(state, root.has_value());
  if (!root.has_value()) return;
  const std::string &llvm = root->llvm.text;
  EXPECT(state, root->llvm.ok);
  EXPECT(state, llvm.find("store atomic i64 2") != std::string::npos);
  EXPECT(state, llvm.find("load atomic i64") != std::string::npos);
  EXPECT(state, llvm.find("atomicrmw add") != std::string::npos);
  EXPECT(state, llvm.find("atomicrmw sub") != std::string::npos);
  EXPECT(state, llvm.find("atomicrmw and") != std::string::npos);
  EXPECT(state, llvm.find("atomicrmw or") != std::string::npos);
  EXPECT(state, llvm.find("atomicrmw xor") != std::string::npos);
  EXPECT(state, llvm.find("atomicrmw xchg") != std::string::npos);
  EXPECT(state, llvm.find("cmpxchg ptr") != std::string::npos);
  EXPECT(state, llvm.find("fence seq_cst") != std::string::npos);
  EXPECT(state, llvm.find("relaxed atomic fence has no effect") !=
      std::string::npos);
  EXPECT(state, llvm.find("<type-parameter>") == std::string::npos);
}

void test_atomic_diagnostics(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-atomic-diagnostics-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  std::ofstream source(root / "app" / "package.draft", std::ios::binary);
  source << "package app\n"
            "import core/atomic\n"
            "word: atomic.Value[u64]\n"
            "pointer_word: atomic.Value[rawptr]\n"
            "main :: proc() {\n"
            "    order: atomic.Order = .relaxed\n"
            "    loader := atomic.load\n"
            "    atomic.load(&word, order)\n"
            "    atomic.load(&word, .release)\n"
            "    atomic.store(&word, 1, .acquire)\n"
            "    expected: u64 = 0\n"
            "    atomic.compare_exchange(\n"
            "        &word, &expected, 1, .relaxed, .acquire)\n"
            "    atomic.fetch_add(&pointer_word, nil, .relaxed)\n"
            "    atomic.fence(.relaxed)\n"
            "}\n";
  source.close();
  EXPECT(state, source.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources, (root / "app").string(), std::move(options), diagnostics);
  EXPECT(state, !result.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  EXPECT(state, rendered.find("must be a compile-time core/atomic.Order value") !=
      std::string::npos);
  EXPECT(state, rendered.find("core/atomic operations must be called directly") !=
      std::string::npos);
  EXPECT(state, rendered.find("atomic load cannot use a release order") !=
      std::string::npos);
  EXPECT(state, rendered.find("atomic store cannot use an acquire order") !=
      std::string::npos);
  EXPECT(state, rendered.find(
      "compare-exchange failure order is stronger than its success order") !=
      std::string::npos);
  EXPECT(state, rendered.find("atomic fetch operation requires an integer type") !=
      std::string::npos);

  std::filesystem::remove_all(root, error);
}

void test_cross_package_generic_procedures(TestState &state) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/packages-generic";
  options.lower_mir = true;
  options.emit_llvm = true;
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources,
      std::string(DRAFT_SOURCE_DIRECTORY) + "/examples/packages-generic/app",
      std::move(options),
      diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, result.graph.packages.size() == 5);

  const draft::CompiledPackage *app = nullptr;
  const draft::CompiledPackage *left = nullptr;
  const draft::CompiledPackage *right = nullptr;
  const draft::CompiledPackage *generic = nullptr;
  const draft::CompiledPackage *layout = nullptr;
  std::optional<draft::PackageId> app_id;
  for (std::size_t package_index = 0; package_index < result.packages.size();
       ++package_index) {
    const std::optional<draft::CompiledPackage> &package =
        result.packages[package_index];
    if (!package.has_value()) continue;
    if (package->identity.root_relative_path == "app") {
      app = &*package;
      app_id = draft::PackageId{static_cast<std::uint32_t>(package_index)};
    }
    if (package->identity.root_relative_path == "lib/left") left = &*package;
    if (package->identity.root_relative_path == "lib/right") right = &*package;
    if (package->identity.root_relative_path == "lib/generic") generic = &*package;
    if (package->identity.root_relative_path == "lib/layout") layout = &*package;
  }
  EXPECT(state, app != nullptr);
  EXPECT(state, left != nullptr);
  EXPECT(state, right != nullptr);
  EXPECT(state, generic != nullptr);
  EXPECT(state, layout != nullptr);
  EXPECT(state, app_id.has_value());
  if (app == nullptr || left == nullptr || right == nullptr ||
      generic == nullptr || layout == nullptr || !app_id.has_value()) {
    return;
  }

  // The app requests private-nominal, u64, byte-size, and inferred N + 1
  // specializations from generic, plus composed type/value work through left.
  // One concrete generic type explicitly depends on Buffer[2] from lib/layout,
  // then retries against that owner-produced graph. The concrete left bodies
  // publish transitive identity[Private_Value] and
  // exact_count[increment_count(4)] requests to generic. The latter keeps
  // left's private compile-time helper on its owner side while publishing only
  // the concrete value 5.
  // Both sibling packages also request identity[u64] and identity[Shared_Value].
  // The latter is local in left and imported in right, so both local display
  // spellings must hash to one canonical nominal identity. All sibling requests
  // must share owner symbols even though lib/generic was discovered before the
  // second sibling in the physical workspace traversal.
  EXPECT(state,
      app->bodies.package.imported_procedure_instances.size() == 6);
  EXPECT(state,
      left->bodies.package.imported_procedure_instances.size() == 4);
  EXPECT(state,
      right->bodies.package.imported_procedure_instances.size() == 2);
  EXPECT(state, generic->bodies.package.parametric_instances.size() == 6);
  EXPECT(state, left->bodies.package.parametric_instances.size() == 2);
  EXPECT(state,
      app->bodies.package.imported_type_instantiation_requests.empty());
  EXPECT(state,
      generic->bodies.package
          .imported_type_instantiation_requests.empty());
  EXPECT(state,
      layout->bodies.package
          .imported_type_instantiation_requests.empty());
  // Canonical owner products append concrete instances without rebuilding any
  // declaration package. The product side table, not the already-complete
  // public PackageInterface, owns each package-independent result graph. All
  // six owner-local nominal instances therefore remain in the append-only
  // package rather than being discarded by an owner retry.
  EXPECT(state,
      generic->bodies.package.parametric_type_instances.size() == 6);
  EXPECT(state,
      layout->bodies.package.parametric_type_instances.size() == 1);

  // Private_Procedural_Buffer cannot be exported to lib/generic until the
  // app's Private_Value natural layout is complete. Its declaration product
  // must therefore name both that exact requester-local layout product and the
  // canonical owner demand. This edge prevents an incomplete private nominal
  // graph from crossing the package boundary merely because its identity was
  // already known.
  const draft::SemanticPackage &app_declarations = app->declarations.package;
  const std::optional<draft::SymbolId> private_value =
      app_declarations.symbols.lookup_direct(
          app_declarations.package_scope, "Private_Value");
  const std::optional<draft::SymbolId> private_buffer =
      app_declarations.symbols.lookup_direct(
          app_declarations.package_scope, "Private_Procedural_Buffer");
  EXPECT(state, private_value.has_value());
  EXPECT(state, private_buffer.has_value());
  std::optional<draft::SemanticProductId> private_value_layout;
  std::optional<draft::SemanticProductId> private_buffer_declaration;
  const draft::PackageSemanticProducts &app_products =
      result.semantic_products.packages[app_id->value];
  if (private_value.has_value()) {
    const draft::TypeId private_value_type =
        app_declarations.symbols.symbol(*private_value).type;
    for (draft::SemanticProductId product : app_products.natural_layouts) {
      if (result.semantic_products.type_by_product[product.value] ==
          private_value_type) {
        private_value_layout = product;
        break;
      }
    }
  }
  if (private_buffer.has_value()) {
    for (draft::SemanticProductId product : app_products.declaration_types) {
      if (result.semantic_products.declaration_by_product[product.value] ==
          *private_buffer) {
        private_buffer_declaration = product;
        break;
      }
    }
  }
  EXPECT(state, private_value_layout.has_value());
  EXPECT(state, private_buffer_declaration.has_value());
  if (private_value_layout.has_value() &&
      private_buffer_declaration.has_value()) {
    const std::vector<draft::SemanticProductId> &dependencies =
        result.semantic_graph
            .products[private_buffer_declaration->value]
            .dependencies;
    EXPECT(state,
           std::find(dependencies.begin(), dependencies.end(),
                     *private_value_layout) != dependencies.end());
    bool has_generic_demand = false;
    for (draft::SemanticProductId dependency : dependencies) {
      if (result.semantic_products
              .generic_type_demand_by_product[dependency.value]
              .is_valid()) {
        has_generic_demand = true;
        break;
      }
    }
    EXPECT(state, has_generic_demand);
  }
  const auto has_published_type = [&](std::string_view public_name) {
    for (const draft::GenericTypeDemand &demand :
         result.semantic_products.generic_type_demands) {
      if (demand.owner.value >= result.packages.size() ||
          !result.packages[demand.owner.value].has_value() ||
          result.packages[demand.owner.value]->identity.root_relative_path !=
              "lib/generic" ||
          !demand.result.has_value()) {
        continue;
      }
      const draft::InterfaceTypeGraph &graph = *demand.result;
      if (graph.root.is_valid() && graph.root.value < graph.types.size() &&
          graph.types[graph.root.value].nominal_public_name == public_name) {
        EXPECT(state,
               result.semantic_graph.products[demand.product.value].state ==
                   draft::SemanticProductState::Complete);
        return true;
      }
    }
    return false;
  };
  EXPECT(state, result.semantic_products.generic_type_demands.size() == 7);
  for (std::size_t index = 0;
       index < result.semantic_products.generic_type_demands.size(); ++index) {
    const draft::GenericTypeDemand &demand =
        result.semantic_products.generic_type_demands[index];
    EXPECT(state, demand.product.is_valid());
    if (!demand.product.is_valid())
      continue;
    EXPECT(state,
           result.semantic_graph.products[demand.product.value].kind ==
               draft::SemanticProductKind::TypeNaturalLayout);
    EXPECT(state,
           result.semantic_graph.products[demand.product.value].state ==
               draft::SemanticProductState::Complete);
    EXPECT(state, demand.result.has_value());
    EXPECT(state,
           result.semantic_products
                   .generic_type_demand_by_product[demand.product.value] ==
               draft::GenericTypeDemandId{
                   static_cast<std::uint32_t>(index)});
  }
  const auto find_demand = [&](std::string_view owner_path,
                               std::string_view public_name)
      -> const draft::GenericTypeDemand * {
    for (const draft::GenericTypeDemand &demand :
         result.semantic_products.generic_type_demands) {
      if (demand.owner.value >= result.packages.size() ||
          !result.packages[demand.owner.value].has_value() ||
          result.packages[demand.owner.value]->identity.root_relative_path !=
              owner_path ||
          !demand.result.has_value()) {
        continue;
      }
      const draft::InterfaceTypeGraph &graph = *demand.result;
      if (graph.root.is_valid() && graph.root.value < graph.types.size() &&
          graph.types[graph.root.value].nominal_public_name == public_name) {
        return &demand;
      }
    }
    return nullptr;
  };
  const draft::GenericTypeDemand *transitive = find_demand(
      "lib/generic", "Transitive_Procedural_Wrapper");
  const draft::GenericTypeDemand *buffer =
      find_demand("lib/layout", "Buffer");
  EXPECT(state, transitive != nullptr);
  EXPECT(state, buffer != nullptr);
  if (transitive != nullptr && buffer != nullptr) {
    const std::vector<draft::SemanticProductId> &dependencies =
        result.semantic_graph.products[transitive->product.value].dependencies;
    EXPECT(state, std::find(dependencies.begin(), dependencies.end(),
                            buffer->product) != dependencies.end());
  }
  EXPECT(state, has_published_type("Transitive_Procedural_Wrapper"));
  EXPECT(state, has_published_type("Transitive_Procedural_Bytes"));
  EXPECT(state, app->llvm.text.find("_24mono_24") != std::string::npos);
  EXPECT(state, left->llvm.text.find("_24mono_24") != std::string::npos);
  EXPECT(state, right->llvm.text.find("_24mono_24") != std::string::npos);
  EXPECT(state, generic->llvm.text.find("define") != std::string::npos);
  EXPECT(state, generic->llvm.text.find("_24mono_24") != std::string::npos);
}

void test_runtime_context_bridge_diagnostics(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-context-bridge-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  EXPECT(state, !error);
  if (error) return;

  std::ofstream source(root / "app" / "package.draft", std::ios::binary);
  source << "package app\n"
            "import core/runtime\n"
            "callback :: proc() {}\n"
            "c_callback :: c proc() {}\n"
            "bad :: proc(value: ^runtime.Context) {\n"
            "    runtime.call_with_context(nil, callback)\n"
            "    runtime.call_with_context(value, c_callback)\n"
            "    runtime.call_with_context(value, callback, 1)\n"
            "}\n";
  source.close();
  EXPECT(state, source.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-test-v1";
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources, (root / "app").string(), std::move(options), diagnostics);
  EXPECT(state, !result.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  EXPECT(state, rendered.find("requires a non-nil Context pointer") !=
      std::string::npos);
  EXPECT(state, rendered.find("must use the Draft calling convention") !=
      std::string::npos);
  EXPECT(state, rendered.find("callback argument count does not match") !=
      std::string::npos);

  std::filesystem::remove_all(root, error);
}

void test_cross_package_higher_order_effect(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-higher-order-effect-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  std::filesystem::create_directories(root / "callbacks", error);
  EXPECT(state, !error);
  if (error) return;

  std::ofstream dependency(
      root / "callbacks" / "package.draft", std::ios::binary);
  dependency <<
      "package callbacks\n"
      "pub invoke :: proc(callback: proc()) {\n"
      "    callback()\n"
      "}\n"
      "pub apply :: proc(\n"
      "    higher: proc(callback: proc()),\n"
      "    callback: proc(),\n"
      ") {\n"
      "    higher(callback)\n"
      "}\n";
  dependency.close();
  EXPECT(state, dependency.good());

  std::ofstream app(root / "app" / "package.draft", std::ios::binary);
  app <<
      "package app\n"
      "import callbacks\n"
      "danger :: proc() {\n"
      "    assert(true)\n"
      "}\n"
      "deny assert {\n"
      "    main :: proc() {\n"
      "        callbacks.apply(callbacks.invoke, danger)\n"
      "    }\n"
      "}\n";
  app.close();
  EXPECT(state, app.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources, (root / "app").string(), std::move(options), diagnostics);
  EXPECT(state, !result.ok);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  if (rendered.find("denied assert") == std::string::npos ||
      rendered.find("unknown call") != std::string::npos) {
    std::cerr << rendered;
  }
  EXPECT(state, rendered.find("denied assert") != std::string::npos);
  EXPECT(state, rendered.find("unknown call") == std::string::npos);

  std::filesystem::remove_all(root, error);
}

// Public interface summaries must retain raw string-data extraction even when
// the importing package calls only a wrapper. This exercises the complete
// producer-interface-consumer path; a local-only denial test cannot prove that
// the dedicated effect survives package translation and proxy import.
void test_cross_package_raw_string_data_denial(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-raw-string-data-denial-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  std::filesystem::create_directories(root / "bytes", error);
  EXPECT(state, !error);
  if (error) return;

  std::ofstream dependency(
      root / "bytes" / "package.draft", std::ios::binary);
  dependency <<
      "package bytes\n"
      "pub expose :: proc(text: string) -> [^]u8 {\n"
      "    return raw_data(text)\n"
      "}\n"
      "pub forward :: proc(text: string) -> [^]u8 {\n"
      "    return expose(text)\n"
      "}\n";
  dependency.close();
  EXPECT(state, dependency.good());

  std::ofstream app(root / "app" / "package.draft", std::ios::binary);
  app <<
      "package app\n"
      "import bytes\n"
      "deny raw_data {\n"
      "    main :: proc(text: string) {\n"
      "        pointer := bytes.forward(text)\n"
      "    }\n"
      "}\n";
  app.close();
  EXPECT(state, app.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources, (root / "app").string(), std::move(options), diagnostics);
  EXPECT(state, !result.ok);
  EXPECT(state, diagnostics.error_count() == 1);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  if (rendered.find("denied raw_data") == std::string::npos ||
      rendered.find("unknown call") != std::string::npos) {
    std::cerr << rendered;
  }
  EXPECT(state, rendered.find("denied raw_data") != std::string::npos);
  EXPECT(state, rendered.find("unknown call") == std::string::npos);

  bool exact_imported_call_range = false;
  for (const draft::Diagnostic &diagnostic : diagnostics.diagnostics()) {
    if (diagnostic.message != "operation reaches denied raw_data") continue;
    exact_imported_call_range =
        sources.text(diagnostic.range) == "bytes.forward(text)";
  }
  EXPECT(state, exact_imported_call_range);

  std::filesystem::remove_all(root, error);
}

void test_cross_package_dependent_generic_effect(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-dependent-generic-effect-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  std::filesystem::create_directories(root / "generic", error);
  EXPECT(state, !error);
  if (error) return;

  std::ofstream dependency(
      root / "generic" / "package.draft", std::ios::binary);
  dependency <<
      "package generic\n"
      "pub inspect[T: type] :: proc(value: T) {\n"
      "    when type_of(value) == bool {\n"
      "        assert(value)\n"
      "    }\n"
      "}\n";
  dependency.close();

  std::ofstream app(root / "app" / "package.draft", std::ios::binary);
  app <<
      "package app\n"
      "import generic\n"
      "deny assert {\n"
      "    safe :: proc() {\n"
      "        generic.inspect(cast[i32](1))\n"
      "    }\n"
      "}\n"
      "unsafe :: proc() {\n"
      "    generic.inspect(true)\n"
      "}\n";
  app.close();
  EXPECT(state, dependency.good() && app.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources, (root / "app").string(), std::move(options), diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());

  const draft::CompiledPackage *app_package = nullptr;
  const draft::CompiledPackage *generic_package = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (!package.has_value()) continue;
    if (package->identity.root_relative_path == "app") {
      app_package = &*package;
    } else if (package->identity.root_relative_path == "generic") {
      generic_package = &*package;
    }
  }
  EXPECT(state, app_package != nullptr);
  EXPECT(state, generic_package != nullptr);
  if (app_package != nullptr && generic_package != nullptr) {
    EXPECT(state,
        app_package->bodies.package.imported_procedure_instances.size() == 2);
    EXPECT(state, generic_package->interface.procedure_instances.size() == 2);
    for (const draft::ImportedProcedureInstance &instance :
         app_package->bodies.package.imported_procedure_instances) {
      EXPECT(state, instance.arguments.size() == 1);
      if (instance.arguments.size() != 1) continue;
      const draft::TypeKind argument_kind =
          app_package->bodies.package.types.type(
              instance.arguments.front().type).kind;
      const bool has_assert = std::any_of(
          app_package->bodies.package.imported_effects.begin(),
          app_package->bodies.package.imported_effects.end(),
          [&](const draft::ImportedEffect &effect) {
            return effect.procedure_proxy == instance.instance_proxy &&
                effect.kind == draft::EffectKind::RuntimeAssert;
          });
      if (argument_kind == draft::TypeKind::Bool) {
        EXPECT(state, has_assert);
      } else if (argument_kind == draft::TypeKind::SignedInteger) {
        EXPECT(state, !has_assert);
      } else {
        EXPECT(state, false);
      }
    }
  }

  std::filesystem::remove_all(root, error);
}

void test_cross_package_static_argument_pack_effects(TestState &state) {
  draft::test::TemporaryDirectory temporary_directory{
      "draft-bootstrap-static-pack-effect-test"};
  const std::filesystem::path &root = temporary_directory.path();
  std::error_code error;
  std::filesystem::create_directories(root / "app", error);
  std::filesystem::create_directories(root / "packing", error);
  EXPECT(state, !error);
  if (error) return;

  std::ofstream dependency(
      root / "packing" / "package.draft", std::ios::binary);
  dependency <<
      "package packing\n"
      "pub inspect_all :: proc(values: ..type) {\n"
      "    for value in values {\n"
      "        when type_of(value) == bool {\n"
      "            assert(value)\n"
      "        }\n"
      "    }\n"
      "}\n"
      "pub inspect_after[T: type] :: proc(prefix: T, values: ..type) -> usize {\n"
      "    static_assert(type_of(prefix) == T)\n"
      "    for value, index in values {\n"
      "        when index == 0 {\n"
      "            static_assert(type_of(value) == string)\n"
      "        } else when index == 1 {\n"
      "            static_assert(type_of(value) == bool)\n"
      "        } else {\n"
      "            static_assert(false, \"unexpected tail element\")\n"
      "        }\n"
      "    }\n"
      "    return len(values)\n"
      "}\n";
  dependency.close();

  std::ofstream app(root / "app" / "package.draft", std::ios::binary);
  app <<
      "package app\n"
      "import packing\n"
      "deny assert {\n"
      "    safe :: proc() {\n"
      "        packing.inspect_all()\n"
      "        packing.inspect_all(cast[i32](1))\n"
      "        packing.inspect_all(cast[i32](2))\n"
      "        inferred := packing.inspect_after(cast[u8](7), \"draft\", true)\n"
      "        explicit := packing.inspect_after[u8](cast[u8](8), \"again\", false)\n"
      "        _ = inferred\n"
      "        _ = explicit\n"
      "    }\n"
      "}\n"
      "unsafe :: proc() {\n"
      "    packing.inspect_all(true)\n"
      "}\n";
  app.close();
  EXPECT(state, dependency.good() && app.good());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = root.string();
  const draft::CompileWorkspaceResult result = draft::compile_workspace(
      sources, (root / "app").string(), std::move(options), diagnostics);
  if (diagnostics.has_errors()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, result.ok);
  EXPECT(state, !diagnostics.has_errors());

  const draft::CompiledPackage *app_package = nullptr;
  const draft::CompiledPackage *packing_package = nullptr;
  for (const std::optional<draft::CompiledPackage> &package : result.packages) {
    if (!package.has_value()) continue;
    if (package->identity.root_relative_path == "app") {
      app_package = &*package;
    } else if (package->identity.root_relative_path == "packing") {
      packing_package = &*package;
    }
  }
  EXPECT(state, app_package != nullptr);
  EXPECT(state, packing_package != nullptr);
  if (app_package != nullptr && packing_package != nullptr) {
    const draft::InterfaceDeclaration *declaration = nullptr;
    const draft::InterfaceDeclaration *composed_declaration = nullptr;
    for (const draft::InterfaceDeclaration &candidate :
         packing_package->interface.declarations) {
      if (candidate.name == "inspect_all") declaration = &candidate;
      if (candidate.name == "inspect_after") {
        composed_declaration = &candidate;
      }
    }
    EXPECT(state, declaration != nullptr);
    if (declaration != nullptr) {
      EXPECT(state, declaration->has_static_argument_pack);
      EXPECT(state, declaration->static_argument_pack_name == "values");
      EXPECT(state,
          declaration->static_argument_pack_fixed_parameter_count == 0);
    }
    EXPECT(state, composed_declaration != nullptr);
    if (composed_declaration != nullptr) {
      EXPECT(state, composed_declaration->has_static_argument_pack);
      EXPECT(state,
          composed_declaration->static_argument_pack_name == "values");
      EXPECT(state,
          composed_declaration->static_argument_pack_fixed_parameter_count ==
              1);
    }

    EXPECT(state,
        app_package->bodies.package.imported_procedure_instances.size() == 4);
    EXPECT(state, packing_package->interface.procedure_instances.size() == 4);
    std::size_t composed_instances = 0;
    for (const draft::ImportedProcedureInstance &instance :
         app_package->bodies.package.imported_procedure_instances) {
      if (instance.public_template_name == "inspect_after") {
        ++composed_instances;
        EXPECT(state, instance.arguments.size() == 1);
        if (instance.arguments.size() == 1) {
          EXPECT(state, instance.arguments.front().is_type);
          EXPECT(state,
              app_package->bodies.package.types.type(
                  instance.arguments.front().type).kind ==
                  draft::TypeKind::UnsignedInteger);
        }
        EXPECT(state, instance.pack_types.size() == 2);
        if (instance.pack_types.size() == 2) {
          EXPECT(state,
              app_package->bodies.package.types.type(
                  instance.pack_types[0]).kind == draft::TypeKind::String);
          EXPECT(state,
              app_package->bodies.package.types.type(
                  instance.pack_types[1]).kind == draft::TypeKind::Bool);
        }
        continue;
      }
      EXPECT(state, instance.public_template_name == "inspect_all");
      EXPECT(state, instance.arguments.empty());
      const bool has_assert = std::any_of(
          app_package->bodies.package.imported_effects.begin(),
          app_package->bodies.package.imported_effects.end(),
          [&](const draft::ImportedEffect &effect) {
            return effect.procedure_proxy == instance.instance_proxy &&
                effect.kind == draft::EffectKind::RuntimeAssert;
          });
      if (instance.pack_types.empty()) {
        EXPECT(state, !has_assert);
      } else if (instance.pack_types.size() == 1) {
        const draft::TypeKind kind =
            app_package->bodies.package.types.type(
                instance.pack_types.front()).kind;
        EXPECT(state, has_assert == (kind == draft::TypeKind::Bool));
      } else {
        EXPECT(state, false);
      }
    }
    // Inferred and explicit `T = u8` calls with the same ordered pack tail
    // denote one specialization across the package interface.
    EXPECT(state, composed_instances == 1);
  }

  std::filesystem::remove_all(root, error);
}

} // namespace

int main() {
  TestState state;
  test_procedure_demand_sets_are_canonical_and_exact(state);
  test_named_constants_are_semantic_products(state);
  test_conditional_members_extend_the_semantic_graph(state);
  test_imported_constant_products_enter_consumer_table(state);
  test_source_update_reuses_unaffected_semantics(state);
  test_interface_synthesis_is_an_explicit_graph_wait(state);
  test_body_source_update_reuses_closed_generic_dependency(state);
  test_body_work_graph_extends_and_removes_generic_demand(state);
  test_body_work_graph_promotes_matching_local_instance(state);
  test_target_lowering_continues_checked_graph(state);
  test_multi_package_native_pipeline(state);
  test_hosted_entry_contract(state);
  test_file_local_imports_share_one_llvm_declaration(state);
  test_compiler_distributed_core(state);
  test_console_println_rejects_unsupported_pack_type(state);
  test_compiler_distributed_memory(state);
  test_compiler_distributed_array_and_support(state);
  test_compiler_distributed_map(state);
  test_compiler_distributed_os(state);
  test_compiler_distributed_thread(state);
  test_aarch64_linux_core_selection(state);
  test_compiler_distributed_atomic(state);
  test_atomic_diagnostics(state);
  test_cross_package_generic_procedures(state);
  test_runtime_context_bridge_diagnostics(state);
  test_cross_package_higher_order_effect(state);
  test_cross_package_raw_string_data_denial(state);
  test_cross_package_dependent_generic_effect(state);
  test_cross_package_static_argument_pack_effects(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " compiler pipeline expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all compiler pipeline tests passed\n";
  return EXIT_SUCCESS;
}
