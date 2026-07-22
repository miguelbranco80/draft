// Append-only package declaration discovery and semantic finalization.
//
// This module owns the direct compositions around the lower-level declaration,
// constant, conditional, and type-product operations. Its inputs are parsed
// package files, completed dependency interfaces, target facts, and published
// product tables. Its outputs are either a retained PackageDeclarationDiscovery
// payload for the command-level scheduler or one final SemanticAnalysisResult
// for body checking. It owns no source buffers and performs no provider or
// backend work.
//
// Workspace compilation calls begin once, publishes graph results into that
// append-only payload, and calls finish once. Ordinary direct clients use the
// sequential package-local product coordinator in this file. Interface
// synthesis is intentionally absent: command-graph product tasks retain their
// own stopped semantic packets, and this module has no aggregate discovery or
// replay entry point. Package declaration order, diagnostic order, and imported
// constant publication are deterministic. Relevant specification: compiler
// dependency ordering and package `when` rules in sections 6 and 1.

#include "sema/semantic.h"

#include "sema/body_checker.h"
#include "sema/global_initializer.h"
#include "sema/runtime_context.h"
#include "sema/target_validation.h"
#include "sema/type_product.h"
#include "sema/type_resolver.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

namespace draft {
namespace {

// Finds the immutable parsed tree named by a cross-probe SyntaxReference.
// LoadedPackage owns every returned tree for the complete semantic operation;
// a missing/non-Draft file is a recoverable failed lookup rather than an
// assertion because malformed package inputs may still reach diagnostics.
[[nodiscard]] const SyntaxTree *find_tree(
    const LoadedPackage &loaded, FileId file) {
  for (const LoadedPackageFile &entry : loaded.files) {
    if (entry.source == file && entry.syntax.has_value()) {
      return &*entry.syntax;
    }
  }
  return nullptr;
}

// Publishes one terminal task-local diagnostic packet in its deterministic
// insertion order. Discovery packets from earlier blocked attempts are never
// passed here: only the attempt whose SemanticPackage payload is retained may
// contribute diagnostics to the command.
void publish_diagnostics(
    const DiagnosticSink &source, DiagnosticSink &destination) {
  for (const Diagnostic &diagnostic : source.diagnostics()) {
    destination.report(
        diagnostic.severity, diagnostic.range, diagnostic.message);
  }
}

// Installs ready dependency-interface constants under their consumer-local
// proxy SymbolIds. Local declarations are graph-owned ConstantValue products;
// imported constants are already immutable PackageInterface inputs and must
// not acquire duplicate consumer products merely to enter the package's
// constant lookup table. Interface binding has already translated nested type
// values into this package's TypeStore, so publication is a sorted value copy,
// not evaluation or recursive dependency work.
void append_imported_constant_bindings(
    const SemanticPackage &package, ConstantTable &constants) {
  for (const ImportedSymbol &imported : package.imported_symbols_for_read()) {
    if (!imported.has_constant) continue;
    const auto position = std::lower_bound(
        constants.bindings.begin(),
        constants.bindings.end(),
        imported.proxy.value,
        [](const ConstantBinding &binding, std::uint32_t value) {
          return binding.symbol.value < value;
        });
    if (position != constants.bindings.end() &&
        position->symbol == imported.proxy) {
      continue;
    }
    constants.bindings.insert(
        position,
        {imported.proxy,
         imported.constant,
         package.symbols.symbol(imported.proxy).type});
  }
}

// Returns true only for a package-level conditional branch recorded by the
// append-only declaration collector and not yet merged into the authoritative
// declaration tables. Conditional member and statement selections share
// ConditionalSelections but are consumed by their later owning phases.
[[nodiscard]] bool conditional_declaration_needs_materialization(
    const SemanticPackage &package, SyntaxReference site) {
  for (const ConditionalDeclarationRegion &region :
       package.conditional_declarations) {
    if (region.syntax == site) return !region.materialized;
  }
  return false;
}

// DirectProductKind names the independently publishable fact owned by one local
// row. Condition and Constant deliberately remain distinct here even though the
// command graph represents both as ConstantValue: the local coordinator has no
// command-wide side tables and direct dispatch is clearer than an invalid
// SymbolId sentinel. DeclarationType is the member-type packet for a nominal
// and the complete declared type for every other authored declaration.
enum class DirectProductKind {
  Condition,
  TypeMembers,
  DeclarationType,
  NaturalLayout,
  Constant,
};

// DirectProductState is monotonic for one analyze_package_semantics call.
// Waiting rows may gain sorted dependency indices but retain their identity;
// Complete and Error are terminal. Error is retained so independent products
// can still publish useful diagnostics without making failed dependants ready.
enum class DirectProductState {
  Waiting,
  Complete,
  Error,
};

// DirectSemanticProduct is one stable row in DirectSemanticCoordinator's
// append-only products_ vector. root is valid for declaration, layout, and
// constant facts; condition is valid only for a condition fact. type is the
// nominal TypeId captured when a layout row is created and belongs to the
// coordinator's canonical SemanticPackage tables. Dependency indices name
// earlier or later rows in the same vector and are sorted after dynamic edges
// are added; they never escape the call or enter a content hash.
struct DirectSemanticProduct {
  DirectProductKind kind = DirectProductKind::DeclarationType;
  DirectProductState state = DirectProductState::Waiting;
  SymbolId root;
  TypeId type;
  SyntaxReference condition;
  std::vector<std::size_t> dependencies;
};

// DirectSemanticCoordinator exists for lower-level tests and subsystem adapters
// which already own parsed LoadedPackage data. Production workspace compilation
// uses SemanticProductId rows in compile/compiler.cpp. Keeping this coordinator
// sequential and package-local makes its limitation explicit: a cross-package
// owner-evaluated generic request requires the workspace coordinator and is
// diagnosed rather than recursively manufactured here.
class DirectSemanticCoordinator {
public:
  DirectSemanticCoordinator(
      const SourceManager &sources, const LoadedPackage &loaded,
      const TargetFacts &target, const AvailablePackageImports &imports,
      DiagnosticSink &diagnostics)
      : sources_(sources), loaded_(loaded), target_(target), imports_(imports),
        diagnostics_(diagnostics) {}

  // Runs eager collection once, then repeatedly selects the lowest ready
  // product index. Successful task packages replace the canonical append-only
  // payload; blocked task packages and their provisional diagnostics are
  // discarded after their exact dependencies are attached. With one worker,
  // this is also the simplest correctness oracle for workspace scheduling.
  [[nodiscard]] SemanticAnalysisResult run() {
    discovery_ = begin_package_declaration_discovery(
        sources_, loaded_, imports_, diagnostics_);
    if (!discovery_.discovery_ok) return failed_result();

    while (true) {
      if (products_need_indexing_) {
        if (!append_visible_products()) return failed_result();
        products_need_indexing_ = false;
      }
      const std::optional<std::size_t> ready = first_ready_product();
      if (!ready.has_value()) {
        bool complete = true;
        bool error = false;
        for (const DirectSemanticProduct &product : products_) {
          complete = complete && product.state == DirectProductState::Complete;
          error = error || product.state == DirectProductState::Error;
        }
        if (complete)
          break;
        // A failed product may leave its dependants waiting permanently. The
        // source diagnostics have already been published, and every other
        // independent ready product has been exhausted, so this is a normal
        // failed analysis rather than a second scheduler-cycle diagnostic.
        if (error) {
          validate_failed_semantics();
          return failed_result();
        }
        diagnose_stalled_products();
        return failed_result();
      }
      if (!run_product(*ready)) return failed_result();
    }

    if (!finish_package_declaration_discovery(discovery_, diagnostics_)) {
      return failed_result();
    }
    return finish_package_semantics_from_products(
        sources_, loaded_, target_, std::move(discovery_), diagnostics_);
  }

private:
  // Returns the published prefix for diagnostic inspection after a source or
  // coordinator failure. ok remains false, global initializers remain absent,
  // and no waiting product is represented as a completed value.
  [[nodiscard]] SemanticAnalysisResult failed_result() const {
    SemanticAnalysisResult result;
    result.package = discovery_.package;
    result.selections = discovery_.selections;
    result.constants = discovery_.published_constants;
    return result;
  }

  // Runs the diagnostic-only closure which normally follows successful
  // product publication. Package-expression validation deliberately inspects
  // operands suppressed by short-circuiting and non-evaluating type_of, so it
  // remains valuable after an unrelated constant or condition has failed.
  // The product scheduler first exhausts all independent ready work; this
  // closure then supplies the same secondary diagnostics as aggregate semantic
  // analysis without evaluating or publishing another copy of any named
  // constant. No result from this operation is used as semantic state.
  void validate_failed_semantics() {
    const ConstantTable constants = package_product_constant_inputs(
        discovery_.package, discovery_.published_constants);
    ConstantTable ignored_global_initializers;
    (void)check_global_initializers(
        sources_, loaded_, discovery_.package, target_, constants,
        ignored_global_initializers, diagnostics_);
    (void)validate_package_compile_time_expression_types(
        sources_, loaded_, discovery_.selections, discovery_.package,
        constants, target_, diagnostics_);
    (void)validate_target_types(
        discovery_.package.types, target_, diagnostics_);
  }

  // Concrete local generic type instances reuse their template syntax but are
  // discoveries of the declaration task which needs them. They must not be
  // mistaken for a second authored product when a successful task publishes
  // the enlarged symbol table.
  [[nodiscard]] bool is_authored_instance(SymbolId symbol) const {
    for (const ParametricTypeInstanceRecord &instance :
         discovery_.package.parametric_type_instances_for_read()) {
      if (instance.instance == symbol) return true;
    }
    return false;
  }

  // Finds one symbol-owned row by its semantic kind. Product counts are small
  // for direct subsystem calls, and the append order is deterministic, so a
  // linear scan keeps the representation inspectable without another index.
  [[nodiscard]] std::optional<std::size_t> find_symbol_product(
      DirectProductKind kind, SymbolId root) const {
    for (std::size_t index = 0; index < products_.size(); ++index) {
      if (products_[index].kind == kind && products_[index].root == root) {
        return index;
      }
    }
    return std::nullopt;
  }

  // Finds the row for one exact source condition. SyntaxReference is stable
  // across private package attempts and distinguishes nested `else when` sites
  // which may share the same owning nominal.
  [[nodiscard]] std::optional<std::size_t> find_condition_product(
      SyntaxReference syntax) const {
    for (std::size_t index = 0; index < products_.size(); ++index) {
      if (products_[index].kind == DirectProductKind::Condition &&
          products_[index].condition == syntax) {
        return index;
      }
    }
    return std::nullopt;
  }

  // Appends one row and returns its stable products_ index. The vector may
  // reallocate, so callers retain indices rather than references across this
  // operation.
  [[nodiscard]] std::size_t append_product(DirectSemanticProduct product) {
    products_.push_back(std::move(product));
    return products_.size() - 1;
  }

  // Appends every newly visible condition, authored declaration facet, and
  // named constant in source SymbolId order. Selected `when` branches extend the
  // same canonical tables and re-enter this operation; existing rows are found
  // by stable SymbolId or SyntaxReference and never duplicated.
  [[nodiscard]] bool append_visible_products() {
    for (const SemanticSite &site : discovery_.package.sites_for_read()) {
      if (site.kind != SemanticSiteKind::ConditionalDeclaration &&
          site.kind != SemanticSiteKind::ConditionalMember) {
        continue;
      }
      if (!find_condition_product(site.syntax).has_value()) {
        DirectSemanticProduct product;
        product.kind = DirectProductKind::Condition;
        product.condition = site.syntax;
        (void)append_product(std::move(product));
      }
    }

    const std::vector<SymbolId> package_symbols =
        discovery_.package.symbols.scope(discovery_.package.package_scope).symbols;
    for (SymbolId symbol : package_symbols) {
      const Symbol &declaration = discovery_.package.symbols.symbol(symbol);
      if (!declaration.syntax.file.is_valid() ||
          !declaration.syntax.node.is_valid() || is_authored_instance(symbol)) {
        continue;
      }
      const bool nominal = declaration.kind == SymbolKind::Type &&
          declaration.type.is_valid() &&
          (discovery_.package.types.type(declaration.type).kind == TypeKind::Struct ||
           discovery_.package.types.type(declaration.type).kind == TypeKind::Enum ||
           discovery_.package.types.type(declaration.type).kind ==
               TypeKind::TaggedUnion ||
           discovery_.package.types.type(declaration.type).kind ==
               TypeKind::RawUnion);
      const bool needs_type = nominal || declaration.kind == SymbolKind::Type ||
          declaration.kind == SymbolKind::Procedure ||
          declaration.kind == SymbolKind::Variable ||
          declaration.kind == SymbolKind::UnresolvedDeclaration;
      if (needs_type &&
          !find_symbol_product(
               DirectProductKind::DeclarationType, symbol).has_value()) {
        DirectSemanticProduct declaration_product;
        declaration_product.kind = DirectProductKind::DeclarationType;
        declaration_product.root = symbol;
        declaration_product.type = declaration.type;
        if (nominal) {
          DirectSemanticProduct members;
          members.kind = DirectProductKind::TypeMembers;
          members.root = symbol;
          members.type = declaration.type;
          for (const SemanticSite &site :
               discovery_.package.sites_for_read()) {
            if (site.kind != SemanticSiteKind::ConditionalMember ||
                site.anchor != symbol) {
              continue;
            }
            const std::optional<std::size_t> condition =
                find_condition_product(site.syntax);
            if (!condition.has_value()) {
              diagnostics_.error(
                  SourceRange::invalid(),
                  "direct type-member product has no condition product");
              return false;
            }
            members.dependencies.push_back(*condition);
          }
          const std::size_t members_index = append_product(std::move(members));
          declaration_product.dependencies.push_back(members_index);
        }
        const std::size_t declaration_index =
            append_product(std::move(declaration_product));
        if (nominal && !declaration.flags.parametric) {
          DirectSemanticProduct layout;
          layout.kind = DirectProductKind::NaturalLayout;
          layout.root = symbol;
          layout.type = declaration.type;
          layout.dependencies.push_back(declaration_index);
          (void)append_product(std::move(layout));
        }
      }

      const Symbol &classified = discovery_.package.symbols.symbol(symbol);
      const bool needs_constant = classified.kind == SymbolKind::Constant ||
          classified.kind == SymbolKind::UnresolvedDeclaration;
      if (!needs_constant ||
          find_symbol_product(DirectProductKind::Constant, symbol).has_value()) {
        continue;
      }
      DirectSemanticProduct constant;
      constant.kind = DirectProductKind::Constant;
      constant.root = symbol;
      if (const std::optional<std::size_t> declaration_product =
              find_symbol_product(DirectProductKind::DeclarationType, symbol)) {
        constant.dependencies.push_back(*declaration_product);
      }
      (void)append_product(std::move(constant));
    }
    return true;
  }

  // A row is ready only when every explicit prerequisite is terminal Complete.
  // An Error prerequisite intentionally leaves the dependant waiting; run()
  // distinguishes that failed closure from a cycle after exhausting unrelated
  // ready work.
  [[nodiscard]] bool dependencies_complete(
      const DirectSemanticProduct &product) const {
    for (std::size_t dependency : product.dependencies) {
      if (dependency >= products_.size() ||
          products_[dependency].state != DirectProductState::Complete) {
        return false;
      }
    }
    return true;
  }

  // Returns the lowest ready row index. This source/product-order choice is the
  // deterministic one-worker oracle; it does not claim that ready products
  // require this execution order in the command-level parallel scheduler.
  [[nodiscard]] std::optional<std::size_t> first_ready_product() const {
    for (std::size_t index = 0; index < products_.size(); ++index) {
      if (products_[index].state == DirectProductState::Waiting &&
          dependencies_complete(products_[index])) {
        return index;
      }
    }
    return std::nullopt;
  }

  // Adds one dynamic prerequisite exactly once. Sorting by local product index
  // makes later dependency traversal independent of expression discovery order
  // and keeps diagnostics deterministic.
  [[nodiscard]] bool add_dependency(std::size_t product,
                                    std::size_t dependency) {
    if (product >= products_.size() || dependency >= products_.size()) {
      diagnostics_.error(
          SourceRange::invalid(),
          "direct semantic product dependency is outside the product table");
      return false;
    }
    std::vector<std::size_t> &dependencies = products_[product].dependencies;
    if (std::find(dependencies.begin(), dependencies.end(), dependency) ==
        dependencies.end()) {
      dependencies.push_back(dependency);
      std::sort(dependencies.begin(), dependencies.end());
    }
    return true;
  }

  // Converts a semantic dependency returned by a product attempt into this
  // coordinator's local row index. A missing row is an integration error, not
  // a source-language blocker: every authored declaration, constant, type
  // facet, and reachable condition must have been indexed before an attempt
  // can name it. Keeping this check here prevents a failed lookup from turning
  // into an unexplained scheduler stall.
  [[nodiscard]] bool add_required_dependency(
      std::size_t product, std::optional<std::size_t> dependency,
      std::string missing_message) {
    if (!dependency.has_value()) {
      diagnostics_.error(SourceRange::invalid(), missing_message);
      return false;
    }
    return add_dependency(product, *dependency);
  }

  // Finds the authored symbol which currently owns one canonical nominal
  // TypeId. Type IDs alone do not identify products, and local concrete generic
  // instances intentionally have no authored row, so absence is a normal
  // lookup result which facet_product passes to the integration check.
  [[nodiscard]] std::optional<SymbolId> type_owner(TypeId type) const {
    const std::size_t symbol_count = discovery_.package.symbols.symbol_count();
    for (std::size_t index = 0; index < symbol_count; ++index) {
      const SymbolId symbol{static_cast<std::uint32_t>(index)};
      if (discovery_.package.symbols.symbol(symbol).type == type) return symbol;
    }
    return std::nullopt;
  }

  // Maps a type-facet wait back to the authored local row which can publish it.
  // Structural types do not own products; their evaluator already reduces a
  // wait to the nominal member/layout facet capable of making progress.
  [[nodiscard]] std::optional<std::size_t> facet_product(
      TypeFacetDependency dependency) const {
    const std::optional<SymbolId> owner = type_owner(dependency.type);
    if (!owner.has_value()) return std::nullopt;
    switch (dependency.facet) {
    case TypeFacet::Members:
      return find_symbol_product(DirectProductKind::TypeMembers, *owner);
    case TypeFacet::NaturalLayout:
      return find_symbol_product(DirectProductKind::NaturalLayout, *owner);
    case TypeFacet::Identity:
    case TypeFacet::MemberTypes:
      return find_symbol_product(DirectProductKind::DeclarationType, *owner);
    }
    return std::nullopt;
  }

  // Attaches every blocker returned by one declaration attempt. Imported-owner
  // generic demands require a WorkspaceGraph and are rejected at this direct
  // package-only boundary instead of being evaluated recursively.
  [[nodiscard]] bool add_declaration_dependencies(
      std::size_t product, const DeclarationTypeProductAttempt &attempt) {
    for (SymbolId dependency : attempt.declaration_dependencies) {
      const Symbol &symbol = discovery_.package.symbols.symbol(dependency);
      if (!add_required_dependency(
              product,
              find_symbol_product(
                  DirectProductKind::DeclarationType, dependency),
              "direct declaration dependency '" + symbol.name +
                  "' has no product")) {
        return false;
      }
    }
    for (SymbolId dependency : attempt.constant_dependencies) {
      const Symbol &symbol = discovery_.package.symbols.symbol(dependency);
      if (!add_required_dependency(
              product,
              find_symbol_product(DirectProductKind::Constant, dependency),
              "direct constant dependency '" + symbol.name +
                  "' has no product")) {
        return false;
      }
    }
    for (TypeFacetDependency dependency : attempt.type_dependencies) {
      if (!add_required_dependency(
              product, facet_product(dependency),
              "direct type-facet dependency has no product")) {
        return false;
      }
    }
    for (SyntaxReference dependency : attempt.condition_dependencies) {
      if (!add_required_dependency(
              product, find_condition_product(dependency),
              "direct condition dependency has no product")) {
        return false;
      }
    }
    if (!attempt.generic_type_dependencies.empty()) {
      diagnostics_.error(
          SourceRange::invalid(),
          "owner-evaluated imported type application requires workspace semantic scheduling");
      return false;
    }
    return true;
  }

  // ConstantProductAttempt and ConditionalProductAttempt intentionally expose
  // the same three dependency vectors. This small local template performs only
  // that shared direct loop; it introduces no type-level policy or dispatch.
  template <typename Attempt>
  [[nodiscard]] bool add_compile_time_dependencies(
      std::size_t product, const Attempt &attempt) {
    for (SymbolId dependency : attempt.declaration_dependencies) {
      const Symbol &symbol = discovery_.package.symbols.symbol(dependency);
      if (!add_required_dependency(
              product,
              find_symbol_product(
                  DirectProductKind::DeclarationType, dependency),
              "direct declaration dependency '" + symbol.name +
                  "' has no product")) {
        return false;
      }
    }
    for (SymbolId dependency : attempt.constant_dependencies) {
      const Symbol &symbol = discovery_.package.symbols.symbol(dependency);
      if (!add_required_dependency(
              product,
              find_symbol_product(DirectProductKind::Constant, dependency),
              "direct constant dependency '" + symbol.name +
                  "' has no product")) {
        return false;
      }
    }
    for (TypeFacetDependency dependency : attempt.type_dependencies) {
      if (!add_required_dependency(
              product, facet_product(dependency),
              "direct type-facet dependency has no product")) {
        return false;
      }
    }
    return true;
  }

  // Returns only completed declaration rows which are explicit prerequisites
  // of product. A signature present incidentally in the sequential canonical
  // package is not authority to consume it without an edge.
  [[nodiscard]] std::vector<SymbolId> completed_declarations(
      const DirectSemanticProduct &product) const {
    std::vector<SymbolId> result;
    for (std::size_t dependency : product.dependencies) {
      const DirectSemanticProduct &candidate = products_[dependency];
      if (candidate.kind == DirectProductKind::DeclarationType &&
          candidate.state == DirectProductState::Complete) {
        result.push_back(candidate.root);
      }
    }
    return result;
  }

  // Reacquires an append-only semantic site by stable syntax identity. No
  // pointer into package.sites survives branch materialization or member-site
  // discovery.
  [[nodiscard]] const SemanticSite *find_condition_site(
      SyntaxReference syntax) const {
    for (const SemanticSite &site : discovery_.package.sites_for_read()) {
      if (site.syntax == syntax) return &site;
    }
    return nullptr;
  }

  // Executes one ready row against a private task package and diagnostic sink.
  // Complete publishes its owned fact, Blocked adds exact edges and discards
  // the attempt, Error publishes only terminal diagnostics, and an integration
  // failure returns false to abort the coordinator.
  [[nodiscard]] bool run_product(std::size_t index) {
    DirectSemanticProduct &product = products_[index];
    // Product routines receive a private sink for the same reason they receive
    // a private SemanticPackage: a blocked attempt may have observed an
    // incomplete prerequisite state. Only a terminal attempt publishes its
    // diagnostics. Coordinator integration failures continue to use the outer
    // sink directly because they are not speculative source diagnostics.
    DiagnosticSink task_diagnostics;
    if (product.kind == DirectProductKind::TypeMembers ||
        product.kind == DirectProductKind::DeclarationType) {
      SemanticPackage task_package = discovery_.package;
      DeclarationTypeProductAttempt attempt;
      if (product.kind == DirectProductKind::TypeMembers) {
        attempt = resolve_package_type_members_product(
            sources_, loaded_, task_package, discovery_.selections, product.root,
            CompileTimeSynthesisMode::Reject, task_diagnostics);
      } else {
        const std::vector<SymbolId> completed =
            completed_declarations(product);
        attempt = resolve_package_declaration_type_product(
            sources_, loaded_, task_package, discovery_.selections,
            product.root, completed, discovery_.published_constants, target_,
            CompileTimeSynthesisMode::Reject, task_diagnostics);
      }
      if (attempt.status == TypeProductStatus::Complete) {
        discovery_.package = std::move(task_package);
        publish_diagnostics(task_diagnostics, diagnostics_);
        product.state = DirectProductState::Complete;
        return true;
      }
      if (attempt.status == TypeProductStatus::Blocked) {
        return add_declaration_dependencies(index, attempt);
      }
      publish_diagnostics(task_diagnostics, diagnostics_);
      product.state = DirectProductState::Error;
      return true;
    }

    if (product.kind == DirectProductKind::NaturalLayout) {
      NaturalLayoutProductAttempt attempt = evaluate_natural_layout_product(
          discovery_.package.types, product.type, task_diagnostics);
      if (attempt.status == TypeProductStatus::Complete) {
        if (!publish_natural_layout_product(
                discovery_.package, product.root, product.type,
                std::move(attempt), task_diagnostics)) {
          publish_diagnostics(task_diagnostics, diagnostics_);
          product.state = DirectProductState::Error;
          return false;
        }
        publish_diagnostics(task_diagnostics, diagnostics_);
        product.state = DirectProductState::Complete;
        return true;
      }
      if (attempt.status == TypeProductStatus::Blocked) {
        for (TypeFacetDependency dependency : attempt.dependencies) {
          if (!add_required_dependency(
                  index, facet_product(dependency),
                  "direct natural-layout dependency has no product")) {
            return false;
          }
        }
        return true;
      }
      publish_diagnostics(task_diagnostics, diagnostics_);
      product.state = DirectProductState::Error;
      return true;
    }

    if (product.kind == DirectProductKind::Condition) {
      const SemanticSite *site = find_condition_site(product.condition);
      if (site == nullptr) {
        diagnostics_.error(
            SourceRange::invalid(),
            "direct conditional product lost its semantic site");
        product.state = DirectProductState::Error;
        return false;
      }
      SemanticPackage task_package = discovery_.package;
      const ConditionalProductAttempt attempt = evaluate_conditional_product(
          sources_, loaded_, task_package, target_, *site,
          discovery_.published_constants, CompileTimeSynthesisMode::Reject,
          task_diagnostics);
      if (attempt.status == CompileTimeProductStatus::Blocked) {
        return add_compile_time_dependencies(index, attempt);
      }
      if (attempt.status != CompileTimeProductStatus::Complete) {
        publish_diagnostics(task_diagnostics, diagnostics_);
        product.state = DirectProductState::Error;
        return true;
      }
      if (discovery_.selections.find(product.condition) == nullptr) {
        discovery_.selections.entries.push_back(
            {product.condition, attempt.selected_true});
      }
      if (conditional_declaration_needs_materialization(
              discovery_.package, product.condition) &&
          !materialize_conditional_declaration(
              sources_, loaded_, discovery_.selections, product.condition,
              discovery_.package, task_diagnostics)) {
        publish_diagnostics(task_diagnostics, diagnostics_);
        product.state = DirectProductState::Error;
        return false;
      }
      discover_package_member_condition_sites(
          sources_, loaded_, discovery_.selections, discovery_.package,
          task_diagnostics);
      if (task_diagnostics.has_errors()) {
        publish_diagnostics(task_diagnostics, diagnostics_);
        product.state = DirectProductState::Error;
        return false;
      }
      // A selected package branch may append declarations, while a selected
      // member condition may reveal the next reachable `else when`. Index that
      // new canonical frontier before selecting another ready product.
      products_need_indexing_ = true;
      publish_diagnostics(task_diagnostics, diagnostics_);
      product.state = DirectProductState::Complete;
      return true;
    }

    if (product.kind == DirectProductKind::Constant) {
      if (discovery_.package.symbols.symbol(product.root).kind ==
          SymbolKind::Type) {
        product.state = DirectProductState::Complete;
        return true;
      }
      SemanticPackage task_package = discovery_.package;
      ConstantProductAttempt attempt = evaluate_package_constant_product(
          sources_, loaded_, task_package, target_, product.root,
          discovery_.published_constants, CompileTimeSynthesisMode::Reject,
          task_diagnostics);
      if (attempt.status == CompileTimeProductStatus::Blocked) {
        return add_compile_time_dependencies(index, attempt);
      }
      if (attempt.status != CompileTimeProductStatus::Complete ||
          !attempt.result.has_value()) {
        publish_diagnostics(task_diagnostics, diagnostics_);
        product.state = DirectProductState::Error;
        return true;
      }
      discovery_.package = std::move(task_package);
      EvaluatedConstant value = std::move(*attempt.result);
      discovery_.package.symbols.symbol_mut(product.root).type = value.type;
      const auto position = std::lower_bound(
          discovery_.published_constants.bindings.begin(),
          discovery_.published_constants.bindings.end(), product.root.value,
          [](const ConstantBinding &binding, std::uint32_t value) {
            return binding.symbol.value < value;
          });
      discovery_.published_constants.bindings.insert(
          position, {product.root, std::move(value.value), value.type});
      publish_diagnostics(task_diagnostics, diagnostics_);
      product.state = DirectProductState::Complete;
      return true;
    }

    diagnostics_.error(
        SourceRange::invalid(), "unknown direct semantic product kind");
    product.state = DirectProductState::Error;
    return false;
  }

  // Reports the first waiting row in product order when no source error and no
  // ready work remain. The row's source range makes an actual declaration or
  // inline-layout cycle actionable; table corruption retains an invalid range.
  void diagnose_stalled_products() {
    for (const DirectSemanticProduct &product : products_) {
      if (product.state != DirectProductState::Waiting) continue;
      SourceRange range = SourceRange::invalid();
      if (product.root.is_valid() &&
          product.root.value < discovery_.package.symbols.symbol_count()) {
        range = discovery_.package.symbols.symbol(product.root).name_range;
      } else if (product.condition.node.is_valid()) {
        const SyntaxTree *tree = find_tree(loaded_, product.condition.file);
        if (tree != nullptr) range = tree->node(product.condition.node).range;
      }
      diagnostics_.error(
          range, "semantic declaration products contain a dependency cycle");
      return;
    }
    diagnostics_.error(
        SourceRange::invalid(), "direct semantic product scheduling stalled");
  }

  const SourceManager &sources_;
  const LoadedPackage &loaded_;
  const TargetFacts &target_;
  const AvailablePackageImports &imports_;
  DiagnosticSink &diagnostics_;
  PackageDeclarationDiscovery discovery_;
  std::vector<DirectSemanticProduct> products_;
  // True initially and after a condition publishes new canonical source
  // structure. Ordinary type/constant/layout publication cannot reveal another
  // authored package product and leaves this false.
  bool products_need_indexing_ = true;
};

// Supplies the public direct-analysis overload with the product coordinator
// while keeping the coordinator type private to this translation unit.
[[nodiscard]] SemanticAnalysisResult analyze_package_semantics_with_products(
    const SourceManager &sources, const LoadedPackage &loaded,
    const TargetFacts &target, const AvailablePackageImports &imports,
    DiagnosticSink &diagnostics) {
  DirectSemanticCoordinator coordinator(
      sources, loaded, target, imports, diagnostics);
  return coordinator.run();
}

} // namespace

SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    DiagnosticSink &diagnostics) {
  const AvailablePackageImports imports;
  return analyze_package_semantics(
      sources,
      loaded,
      target,
      imports,
      diagnostics);
}

SemanticAnalysisResult analyze_package_semantics(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const TargetFacts &target,
    const AvailablePackageImports &imports,
    DiagnosticSink &diagnostics) {
  return analyze_package_semantics_with_products(
      sources, loaded, target, imports, diagnostics);
}

PackageDeclarationDiscovery begin_package_declaration_discovery(
    const SourceManager &sources,
    const LoadedPackage &loaded,
    const AvailablePackageImports &imports,
    DiagnosticSink &diagnostics) {
  PackageDeclarationDiscovery result;
  const std::size_t initial_error_count = diagnostics.error_count();
  result.package = collect_package_declarations(sources, loaded, diagnostics);
  result.package.identity = imports.consumer_identity;
  bind_package_interfaces(result.package, imports, diagnostics);
  // Member conditions are graph prerequisites of their owning nominal member
  // packet. Discover only the initial reachable frontier now; each published
  // selection asks the same scanner for newly reachable nested sites without
  // declaring provisional members or inspecting an unselected branch.
  discover_package_member_condition_sites(
      sources, loaded, result.selections, result.package, diagnostics);
  result.discovery_ok =
      diagnostics.error_count() == initial_error_count;
  return result;
}

bool finish_package_declaration_discovery(
    PackageDeclarationDiscovery &discovery,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_error_count = diagnostics.error_count();
  if (discovery.terminal) {
    diagnostics.error(
        SourceRange::invalid(),
        "package declaration discovery was finalized more than once");
    return false;
  }
  for (const ConditionalDeclarationRegion &region :
       discovery.package.conditional_declarations) {
    if (region.materialized) continue;
    diagnostics.error(
        discovery.package.symbols.scope(region.scope).range,
        "package name set still contains an unselected conditional region");
    return false;
  }

  const std::vector<SymbolId> package_symbols =
      discovery.package.symbols.scope(discovery.package.package_scope).symbols;
  for (SymbolId symbol_id : package_symbols) {
    const Symbol &symbol = discovery.package.symbols.symbol(symbol_id);
    // Interface import may add a compiler-owned package-scope nominal solely
    // to own the member scope of a nested signature type. Its immutable facet
    // packet is an upstream input, not an authored declaration governed by
    // this package's terminal barrier.
    if (!symbol.syntax.file.is_valid() || !symbol.syntax.node.is_valid()) {
      continue;
    }
    bool is_type_instance = false;
    for (const ParametricTypeInstanceRecord &instance :
         discovery.package.parametric_type_instances_for_read()) {
      if (instance.instance == symbol_id) {
        is_type_instance = true;
        break;
      }
    }
    if (is_type_instance)
      continue;
    if ((symbol.kind == SymbolKind::Type ||
         symbol.kind == SymbolKind::Procedure) &&
        !symbol.type.is_valid()) {
      diagnostics.error(
          symbol.name_range,
          "package declaration type product is not complete for '" +
              symbol.name + "'");
      return false;
    }
    if (symbol.kind != SymbolKind::Type || !symbol.type.is_valid()) continue;
    const TypeKind kind = discovery.package.types.type(symbol.type).kind;
    if (kind != TypeKind::Struct && kind != TypeKind::Enum &&
        kind != TypeKind::TaggedUnion && kind != TypeKind::RawUnion) {
      continue;
    }
    const bool member_types_complete =
        discovery.package.types.facet_state(
            symbol.type, TypeFacet::MemberTypes) == TypeFacetState::Complete;
    // A parametric nominal is the checked symbolic pattern from which
    // canonical applications are formed. It has no one target-natural layout:
    // `Result[i32, string]` and `Result[i128, u8]` own different concrete
    // layouts. Closing the package name set therefore requires the template's
    // member-type pattern but leaves layout to each instance product.
    const bool natural_layout_complete =
        symbol.flags.parametric ||
        discovery.package.types.facet_state(
            symbol.type, TypeFacet::NaturalLayout) == TypeFacetState::Complete;
    if (!member_types_complete || !natural_layout_complete) {
      diagnostics.error(symbol.name_range,
                        "nominal type products are not complete for '" +
                            symbol.name + "'");
      return false;
    }
  }

  ensure_runtime_context_type(discovery.package, diagnostics);
  if (diagnostics.error_count() != initial_error_count)
    return false;
  discovery.terminal = true;
  return true;
}

ConstantTable package_product_constant_inputs(
    const SemanticPackage &package,
    const ConstantTable &published_constants) {
  ConstantTable result = published_constants;
  append_imported_constant_bindings(package, result);
  return result;
}

SemanticAnalysisResult finish_package_semantics_from_products(
    const SourceManager &sources, const LoadedPackage &loaded,
    const TargetFacts &target, PackageDeclarationDiscovery discovery,
    DiagnosticSink &diagnostics) {
  SemanticAnalysisResult result;
  result.package = std::move(discovery.package);
  result.selections = std::move(discovery.selections);
  if (!discovery.terminal) return result;

  const std::size_t initial_error_count = diagnostics.error_count();
  result.constants = package_product_constant_inputs(
      result.package, discovery.published_constants);
  // ConstantValue products own both the immutable value and its checked static
  // type. Private condition or constant task state may predate
  // that publication, so the package-interface barrier installs the type
  // payload explicitly before validation or body work consumes the declaration
  // publication into the canonical declaration tables. This is product
  // publication, not reevaluation of source.
  for (const ConstantBinding &binding : result.constants.bindings) {
    if (!binding.type.is_valid() ||
        static_cast<std::size_t>(binding.symbol.value) >=
            result.package.symbols.symbol_count()) {
      continue;
    }
    Symbol &symbol = result.package.symbols.symbol_mut(binding.symbol);
    if (symbol.kind != SymbolKind::Type)
      symbol.type = binding.type;
  }
  (void)finalize_package_procedure_parameter_defaults(
      sources,
      loaded,
      result.package,
      target,
      result.constants,
      diagnostics);
  const CompileTimeRoundResult final_round = validate_compile_time_products(
      sources, loaded, result.package, target, result.selections,
      result.constants, CompileTimeSynthesisMode::Reject, true, diagnostics);
  (void)check_global_initializers(sources, loaded, result.package, target,
                                  result.constants, result.global_initializers,
                                  diagnostics);
  (void)validate_package_compile_time_expression_types(
      sources,
      loaded,
      result.selections,
      result.package,
      result.constants,
      target,
      diagnostics);
  (void)validate_target_types(result.package.types, target, diagnostics);
  result.ok = discovery.discovery_ok &&
              diagnostics.error_count() == initial_error_count &&
              final_round.unresolved_conditionals == 0;
  return result;
}

} // namespace draft
