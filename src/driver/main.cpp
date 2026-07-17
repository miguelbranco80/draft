// Bootstrap compiler command-line entry point.
//
// The driver is intentionally thin: it owns process-facing argument and stream
// behavior, while source loading, diagnostics, syntax, semantics, and codegen
// remain reusable modules. The initial `lex` command is an inspectable front-end
// probes and will remain useful after `check`, `build`, `resolve`, and `judge`
// are added. They print exact token spellings, grammar structure, or the complete
// versioned target profile without embedding phase logic in this file.

#include "backend/toolchain.h"
#include "compile/compiler.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "syntax/lexer.h"
#include "syntax/parser.h"
#include "syntax/syntax_tree.h"
#include "syntax/token.h"
#include "target/profile.h"
#include "workspace/package.h"
#include "workspace/workspace.h"

#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

[[nodiscard]] std::string escaped(std::string_view text) {
  std::string result;
  for (const char byte : text) {
    switch (byte) {
    case '\n': result += "\\n"; break;
    case '\r': result += "\\r"; break;
    case '\t': result += "\\t"; break;
    case '\\': result += "\\\\"; break;
    default:
      // Token dumps must not change with the process locale. ASCII graphic and
      // space bytes are printed directly; all other bytes use a fixed marker.
      if (byte >= 0x20 && byte <= 0x7e) {
        result += byte;
      } else {
        result += '?';
      }
      break;
    }
  }
  return result;
}

int lex_file(const std::string &path) {
  draft::SourceManager sources;
  const draft::LoadFileResult load = sources.load_file(path);
  if (!load.ok) {
    std::cerr << "error: " << load.error << '\n';
    return 1;
  }

  draft::DiagnosticSink diagnostics;
  const std::vector<draft::Token> tokens = draft::lex_source(sources, load.file, diagnostics);
  for (const draft::Token &token : tokens) {
    const draft::LineColumn position = sources.line_column(token.range.begin);
    std::cout << position.line << ':' << position.column << ' '
              << draft::token_kind_name(token.kind);
    if (token.inserted) {
      std::cout << " [inserted]";
    } else if (token.kind != draft::TokenKind::EndOfFile) {
      std::cout << " \"" << escaped(sources.text(token.range)) << '"';
    }
    std::cout << '\n';
  }

  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  return diagnostics.has_errors() ? 1 : 0;
}

int parse_file(const std::string &path) {
  draft::SourceManager sources;
  const draft::LoadFileResult load = sources.load_file(path);
  if (!load.ok) {
    std::cerr << "error: " << load.error << '\n';
    return 1;
  }

  draft::DiagnosticSink diagnostics;
  const draft::SyntaxTree tree = draft::parse_source_file(sources, load.file, diagnostics);
  std::cout << draft::dump_syntax_tree(tree);
  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  return diagnostics.has_errors() ? 1 : 0;
}

// Prints the profile in a stable line-oriented form suitable for inspection and
// simple snapshot tests. This is not the eventual canonical serialized build
// input, whose format will be versioned independently.
int print_target() {
  const draft::TargetProfile profile = draft::make_aarch64_macos_profile();
  std::string reason;
  if (!draft::validate_target_profile(profile, reason)) {
    std::cerr << "error: invalid built-in target profile: " << reason << '\n';
    return 1;
  }
  std::cout << "identity " << profile.facts.identity << '\n'
            << "triple " << profile.llvm_triple << '\n'
            << "data-layout " << profile.llvm_data_layout << '\n'
            << "file-tag " << profile.facts.file_tag << '\n'
            << "pointer-bits " << profile.facts.pointer_bits << '\n'
            << "page-size " << profile.facts.page_size << '\n'
            << "object-format " << profile.facts.object_format << '\n'
            << "assembly-dialect " << profile.parsed_assembly_dialect << '\n'
            << "relocation-model "
            << draft::relocation_model_name(profile.relocation_model) << '\n'
            << "code-model " << draft::code_model_name(profile.code_model) << '\n'
            << "tls-model " << draft::tls_model_name(profile.tls_model) << '\n';
  return 0;
}

// Runs one dependency-ordered provider-free pipeline. Until a build manifest
// supplies explicit roots, the requested package's parent is the workspace
// root. `check` stops after typed HIR/interfaces; `emit-llvm` additionally
// lowers MIR and prints each package module without invoking LLVM or a linker.
int compile_package(const std::string &directory, bool emit_llvm) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::error_code path_error;
  const std::filesystem::path absolute_directory =
      std::filesystem::absolute(directory, path_error);
  if (path_error) {
    std::cerr << "error: cannot make package path absolute: "
              << path_error.message() << '\n';
    return 1;
  }

  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory =
      absolute_directory.parent_path().string();
  options.lower_mir = emit_llvm;
  options.emit_llvm = emit_llvm;
  draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources, absolute_directory.string(), std::move(options), diagnostics);
  std::size_t symbol_count = 0;
  std::size_t type_count = 0;
  std::size_t procedure_count = 0;
  std::size_t agent_record_count = 0;
  if (compiled.ok) {
    for (std::size_t index = 0; index < compiled.packages.size(); ++index) {
      const draft::CompiledPackage &package = *compiled.packages[index];
      symbol_count += package.semantics.package.symbols.symbol_count();
      type_count += package.semantics.package.types.size();
      procedure_count += package.bodies.checked_procedures;
      agent_record_count += package.metadata.records.size();
      if (emit_llvm) {
        std::cout << "; ----- package "
                  << draft::display_package_identity(package.identity)
                  << " -----\n"
                  << package.llvm.text;
      }
    }
    if (!emit_llvm) {
      const draft::WorkspacePackage &root =
          compiled.graph.package(compiled.graph.root_package);
      std::cout << "checked package graph rooted at " << root.loaded.short_name << ": "
                << compiled.graph.packages.size() << " packages, "
                << symbol_count << " symbols, "
                << type_count << " types, "
                << procedure_count << " procedure bodies, "
                << agent_record_count << " agent records\n";
    }
  }

  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  return diagnostics.has_errors() ? 1 : 0;
}

int build_package(
    const std::string &directory,
    const std::optional<std::string> &requested_output,
    bool allow_host_toolchain) {
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  std::error_code path_error;
  const std::filesystem::path absolute_directory =
      std::filesystem::absolute(directory, path_error);
  if (path_error) {
    std::cerr << "error: cannot make package path absolute: "
              << path_error.message() << '\n';
    return 1;
  }
  const draft::TargetProfile target = draft::make_aarch64_macos_profile();
  draft::CompileWorkspaceOptions compile_options;
  compile_options.target = target;
  compile_options.workspace.workspace_directory =
      absolute_directory.parent_path().string();
  compile_options.lower_mir = true;
  compile_options.emit_llvm = true;
  const draft::CompileWorkspaceResult compiled = draft::compile_workspace(
      sources,
      absolute_directory.string(),
      std::move(compile_options),
      diagnostics);
  if (compiled.ok) {
    const std::filesystem::path build_directory =
        absolute_directory / ".draft" / "build";
    const std::filesystem::path output = requested_output.has_value()
        ? std::filesystem::path(*requested_output)
        : build_directory / absolute_directory.filename();
    draft::NativeBuildOptions native_options;
    native_options.build_directory = build_directory.string();
    native_options.output_path = output.string();
    native_options.allow_unpinned_toolchain = allow_host_toolchain;
    const draft::NativeBuildResult built = draft::build_native_executable(
        target, compiled, native_options, diagnostics);
    if (built.ok) {
      std::cout << "built " << built.output_path << '\n';
    }
  }
  if (!diagnostics.diagnostics().empty()) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  return diagnostics.has_errors() ? 1 : 0;
}

void print_usage() {
  std::cerr << "usage:\n"
            << "  draftc lex <file.draft>\n"
            << "  draftc syntax <file.draft>\n"
            << "  draftc check <package-directory>\n"
            << "  draftc emit-llvm <package-directory>\n"
            << "  draftc build <package-directory> [-o <output>] [--allow-host-toolchain]\n"
            << "  draftc target\n";
}

} // namespace

int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "lex") {
    return lex_file(argv[2]);
  }
  if (argc == 3 && std::string_view(argv[1]) == "syntax") {
    return parse_file(argv[2]);
  }
  if (argc == 3 && std::string_view(argv[1]) == "check") {
    return compile_package(argv[2], false);
  }
  if (argc == 3 && std::string_view(argv[1]) == "emit-llvm") {
    return compile_package(argv[2], true);
  }
  if (argc >= 3 && std::string_view(argv[1]) == "build") {
    std::optional<std::string> output;
    bool allow_host_toolchain = false;
    for (int index = 3; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--allow-host-toolchain") {
        allow_host_toolchain = true;
      } else if (argument == "-o" && index + 1 < argc) {
        ++index;
        output = argv[index];
      } else {
        print_usage();
        return 2;
      }
    }
    return build_package(argv[2], output, allow_host_toolchain);
  }
  if (argc == 2 && std::string_view(argv[1]) == "target") {
    return print_target();
  }
  print_usage();
  return 2;
}
