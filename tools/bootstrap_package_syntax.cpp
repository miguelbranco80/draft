// Bootstrap oracle for the self-hosted folder-package syntax boundary.
//
// This non-installed test executable runs the production C++ package loader,
// then prints the exact stable package/tree dump consumed by the Draft-written
// differential gate. It owns one SourceManager, diagnostic sink, and loaded
// package for the invocation. The target selector affects source filenames
// only; no semantic or native phase runs here.
//
// Pre-source directory enumeration failures are normalized to the portable
// message available through Draft's current core/filesystem API. Source-backed
// parser and package-name diagnostics retain the production renderer exactly.

#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/syntax_tree.h"
#include "workspace/package.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

// Returns true only for the four exact target.file_tag spellings understood by
// this compiler generation. The oracle rejects an unknown selector before any
// filesystem work, matching draftc-next's staging command boundary.
[[nodiscard]] bool valid_target_selector(std::string_view selector) {
  return selector == "aarch64-macos" || selector == "aarch64-linux" ||
         selector == "x86_64-linux" || selector == "x86_64-windows";
}

// Rebuilds the diagnostic sequence while replacing only a platform-detailed
// directory enumeration failure. core/filesystem deliberately exposes a closed
// portable error, so the staging oracle cannot make strerror text part of the
// self-hosted phase contract. Every other severity, range, and message remains
// byte-for-byte production output.
[[nodiscard]] draft::DiagnosticSink
normalized_diagnostics(const draft::DiagnosticSink &source,
                       std::string_view directory) {
  draft::DiagnosticSink result;
  constexpr std::string_view enumeration_prefix =
      "cannot enumerate package directory '";
  for (const draft::Diagnostic &diagnostic : source.diagnostics()) {
    std::string message = diagnostic.message;
    if (!diagnostic.range.is_valid() &&
        message.starts_with(enumeration_prefix)) {
      message = std::string(enumeration_prefix) + std::string(directory) + "'";
    }
    result.report(diagnostic.severity, diagnostic.range, std::move(message));
  }
  return result;
}

// Prints the same selected-name and concrete-tree representation as the Draft
// package layer. Failed source loads have no LoadedPackageFile row and
// therefore contribute diagnostics but no stdout entry.
void dump_package_syntax(const draft::LoadedPackage &package,
                         std::ostream &output) {
  if (!package.short_name.empty()) {
    output << "package " << package.short_name << '\n';
  }
  for (const draft::LoadedPackageFile &file : package.files) {
    output << (file.kind == draft::PackageFileKind::DraftSource ? "draft "
                                                                : "assembly ")
           << file.relative_name << '\n';
    if (file.syntax.has_value()) {
      output << draft::dump_syntax_tree(*file.syntax);
    }
  }
}

} // namespace

int main(int argc, char **argv) {
  if (argc != 5 || std::string_view(argv[1]) != "package-syntax" ||
      std::string_view(argv[3]) != "--target" ||
      !valid_target_selector(argv[4])) {
    std::cerr << "usage:\n"
                 "  draft-bootstrap-package-syntax package-syntax <package> "
                 "--target <selector>\n";
    return EXIT_FAILURE;
  }

  const std::string directory = argv[2];
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::PackageLoadOptions options;
  options.file_tag = argv[4];
  const draft::PackageLoadResult loaded =
      draft::load_package(sources, directory, options, diagnostics);

  dump_package_syntax(loaded.package, std::cout);
  const draft::DiagnosticSink normalized =
      normalized_diagnostics(diagnostics, directory);
  std::cerr << draft::render_diagnostics(sources, normalized);
  if (!std::cout || !std::cerr)
    return EXIT_FAILURE;
  return loaded.ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
