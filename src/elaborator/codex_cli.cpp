// Process, temporary-file, prompt, and response handling for Codex CLI.
//
// The implementation is intentionally POSIX and matches Draft's first
// AArch64-macOS host. It uses fork/exec directly rather than a shell, so prompt,
// model, and path bytes can never become command syntax. A private RAII
// directory owns schema, prompt, attachments, final output, and logs; it is
// removed on every return path. Provider failure is diagnostic-only and cannot
// write the resolution store.

#include "elaborator/codex_cli.h"

#include "base/sha256.h"
#include "sema/symbol.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#if defined(__APPLE__) || defined(__unix__)
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace draft {
namespace {

constexpr std::uintmax_t kMaximumCodexOutputBytes = 64U * 1024U * 1024U;
constexpr std::uintmax_t kMaximumCodexLogBytes = 4U * 1024U * 1024U;
constexpr std::string_view kPromptContractIdentity =
    "draft-codex-synthesis-prompt-v12";
constexpr std::string_view kOutputSchema =
    "{\n"
    "  \"type\": \"object\",\n"
    "  \"properties\": {\n"
    "    \"source\": {\"type\": \"string\"}\n"
    "  },\n"
    "  \"required\": [\"source\"],\n"
    "  \"additionalProperties\": false\n"
    "}\n";

class TemporaryDirectory {
public:
  TemporaryDirectory() = default;
  TemporaryDirectory(const TemporaryDirectory &) = delete;
  TemporaryDirectory &operator=(const TemporaryDirectory &) = delete;

  ~TemporaryDirectory() {
    if (path_.empty()) return;
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] bool create(DiagnosticSink &diagnostics) {
#if defined(__APPLE__) || defined(__unix__)
    std::error_code error;
    const std::filesystem::path temporary_root =
        std::filesystem::temp_directory_path(error);
    if (error) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot locate a temporary directory for Codex: " + error.message());
      return false;
    }
    std::string pattern =
        (temporary_root / "draft-codex-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char *created = ::mkdtemp(writable.data());
    if (created == nullptr) {
      diagnostics.error(
          SourceRange::invalid(),
          "cannot create Codex provider directory: " +
              std::string(std::strerror(errno)));
      return false;
    }
    path_ = created;
    return true;
#else
    diagnostics.error(
        SourceRange::invalid(),
        "Codex CLI provider is unavailable on this host");
    return false;
#endif
  }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

void provider_error(DiagnosticSink &diagnostics, std::string message) {
  diagnostics.error(SourceRange::invalid(), std::move(message));
}

[[nodiscard]] bool write_file(
    const std::filesystem::path &path,
    std::string_view contents,
    DiagnosticSink &diagnostics) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    provider_error(
        diagnostics, "cannot create Codex input file '" + path.string() + "'");
    return false;
  }
  output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  output.close();
  if (!output) {
    provider_error(
        diagnostics, "cannot write Codex input file '" + path.string() + "'");
    return false;
  }
  return true;
}

// Reads a regular bounded adapter output. Codex controls these bytes, so size
// and file type are validated before allocation.
[[nodiscard]] bool read_file(
    const std::filesystem::path &path,
    std::uintmax_t maximum_size,
    std::string &contents,
    DiagnosticSink &diagnostics) {
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, error);
  if (error || std::filesystem::is_symlink(status) ||
      !std::filesystem::is_regular_file(status)) {
    provider_error(
        diagnostics, "Codex did not produce a regular output file");
    return false;
  }
  const std::uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size > maximum_size ||
      size > static_cast<std::uintmax_t>(
          std::numeric_limits<std::size_t>::max())) {
    provider_error(diagnostics, "Codex output file is too large or unreadable");
    return false;
  }
  contents.resize(static_cast<std::size_t>(size));
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    provider_error(diagnostics, "cannot open Codex output file");
    return false;
  }
  if (!contents.empty()) {
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  }
  if (!input || input.peek() != std::ifstream::traits_type::eof()) {
    provider_error(diagnostics, "cannot read exact Codex output bytes");
    return false;
  }
  return true;
}

void append_u64(std::uint64_t value, std::string &output) {
  output += std::to_string(value);
  output.push_back('\n');
}

// Length-prefixed fields allow prompts and source-authored strings to contain
// arbitrary newlines without creating an ambiguous request transcript.
void append_field(
    std::string_view name,
    std::string_view value,
    std::string &output) {
  output += name;
  output.push_back(' ');
  append_u64(static_cast<std::uint64_t>(value.size()), output);
  output.append(value);
  output.push_back('\n');
}

[[nodiscard]] std::string attachment_name(std::size_t index) {
  std::string digits = std::to_string(index);
  const std::size_t zero_count = digits.size() < 8 ? 8 - digits.size() : 0;
  return "attachment-" + std::string(zero_count, '0') + digits + ".bin";
}

[[nodiscard]] std::string documentation_attachment_name(
    std::size_t documentation_index, std::size_t attachment_index) {
  std::string documentation = std::to_string(documentation_index);
  std::string attachment = std::to_string(attachment_index);
  const std::size_t documentation_zeroes =
      documentation.size() < 8 ? 8 - documentation.size() : 0;
  const std::size_t attachment_zeroes =
      attachment.size() < 8 ? 8 - attachment.size() : 0;
  return "documentation-" + std::string(documentation_zeroes, '0') +
      documentation + "-attachment-" +
      std::string(attachment_zeroes, '0') + attachment + ".bin";
}

[[nodiscard]] std::string judgment_attachment_name(
    std::size_t judgment_index, std::size_t attachment_index) {
  std::string judgment = std::to_string(judgment_index);
  std::string attachment = std::to_string(attachment_index);
  const std::size_t judgment_zeroes =
      judgment.size() < 8 ? 8 - judgment.size() : 0;
  const std::size_t attachment_zeroes =
      attachment.size() < 8 ? 8 - attachment.size() : 0;
  return "judgment-" + std::string(judgment_zeroes, '0') + judgment +
      "-attachment-" + std::string(attachment_zeroes, '0') + attachment +
      ".bin";
}

[[nodiscard]] std::string imported_documentation_attachment_name(
    std::size_t package_index,
    std::size_t documentation_index,
    std::size_t attachment_index) {
  const auto padded = [](std::size_t value) {
    std::string digits = std::to_string(value);
    const std::size_t zeroes = digits.size() < 8 ? 8 - digits.size() : 0;
    return std::string(zeroes, '0') + digits;
  };
  return "import-" + padded(package_index) + "-documentation-" +
      padded(documentation_index) + "-attachment-" +
      padded(attachment_index) + ".bin";
}

// Constructs the complete model instruction. The output contract is repeated
// in prose as well as enforced by the CLI JSON Schema. No surface or generated
// workspace path is exposed.
[[nodiscard]] bool prepare_request(
    const SynthesisRequest &request,
    const std::filesystem::path &directory,
    std::string &prompt,
    DiagnosticSink &diagnostics) {
  prompt =
      "You are the Draft language synthesis provider. Produce exactly one "
      "ordinary Draft source fragment for the supplied grammar category. "
      "Return only a JSON object with one string field named source. The "
      "source must contain no judge construct and no unresolved ... synthesis "
      "site. Do not edit files. Do not inspect paths outside this isolated "
      "request directory.\n";
  append_field("REQUEST_FORMAT", request.format, prompt);
  append_field("SITE", request.obligation.site_identity, prompt);
  append_field("ROOT_IDENTITY", request.obligation.root_identity, prompt);
  append_field(
      "ROOT_RELATIVE_PATH", request.obligation.root_relative_path, prompt);
  append_field(
      "SOURCE_RELATIVE_PATH", request.obligation.source_relative_path, prompt);
  append_field("ANCHOR_NAME", request.obligation.anchor_name, prompt);
  append_field(
      "SITE_OCCURRENCE",
      std::to_string(request.obligation.occurrence),
      prompt);
  append_field(
      "GRAMMAR",
      agent_construct_kind_name(request.obligation.kind),
      prompt);
  append_field("INPUT_SHA256", request.obligation.input_digest.hex(), prompt);
  append_field("RECORD_SHA256", request.obligation.record_digest.hex(), prompt);
  append_field(
      "EXPECTED_TYPE_SHA256",
      request.obligation.expected_type_digest.hex(),
      prompt);
  append_field(
      "EXPECTED_TYPE_TEXT",
      request.obligation.expected_type_text,
      prompt);
  append_field("TARGET_IDENTITY", request.obligation.target.identity, prompt);
  append_field("TARGET_ARCH", request.obligation.target.arch, prompt);
  append_field("TARGET_OS", request.obligation.target.os, prompt);
  append_field("TARGET_ABI", request.obligation.target.abi, prompt);
  append_field(
      "TARGET_BYTE_ORDER", request.obligation.target.byte_order, prompt);
  append_field(
      "TARGET_OBJECT_FORMAT", request.obligation.target.object_format, prompt);
  append_field("TARGET_FILE_TAG", request.obligation.target.file_tag, prompt);
  append_field(
      "TARGET_POINTER_BITS",
      std::to_string(request.obligation.target.pointer_bits),
      prompt);
  append_field(
      "TARGET_PAGE_SIZE",
      std::to_string(request.obligation.target.page_size),
      prompt);
  prompt += "TARGET_FEATURES ";
  append_u64(
      static_cast<std::uint64_t>(request.obligation.target.features.size()),
      prompt);
  for (const std::string &feature : request.obligation.target.features) {
    append_field("TARGET_FEATURE", feature, prompt);
  }
  prompt += "TARGET_SIMD_SHAPES ";
  append_u64(
      static_cast<std::uint64_t>(request.obligation.target.simd_shapes.size()),
      prompt);
  for (const TargetSimdShape &shape : request.obligation.target.simd_shapes) {
    append_field("TARGET_SIMD_ELEMENT", shape.element, prompt);
    append_field("TARGET_SIMD_LANES", std::to_string(shape.lanes), prompt);
  }
  append_field(
      "ASSEMBLY_ARCHITECTURE",
      request.obligation.target.assembly_architecture,
      prompt);
  append_field(
      "ASSEMBLY_DIALECT", request.obligation.target.assembly_dialect, prompt);
  prompt += "ASSEMBLY_INSTRUCTIONS ";
  append_u64(
      static_cast<std::uint64_t>(
          request.obligation.target.assembly_instructions.size()),
      prompt);
  for (const std::string &instruction :
       request.obligation.target.assembly_instructions) {
    append_field("ASSEMBLY_INSTRUCTION", instruction, prompt);
  }
  append_field(
      "ENCLOSING_DECLARATION_PRESENT",
      request.obligation.enclosing_declaration.present ? "true" : "false",
      prompt);
  if (request.obligation.enclosing_declaration.present) {
    const AgentEnclosingDeclarationContext &enclosing =
        request.obligation.enclosing_declaration;
    if (sha256(enclosing.source) != enclosing.source_digest) {
      provider_error(
          diagnostics,
          "Codex enclosing declaration identity is inconsistent");
      return false;
    }
    append_field("ENCLOSING_DECLARATION_NAME", enclosing.name, prompt);
    append_field(
        "ENCLOSING_DECLARATION_KIND",
        symbol_kind_name(enclosing.kind),
        prompt);
    append_field(
        "ENCLOSING_DECLARATION_SHA256",
        enclosing.source_digest.hex(),
        prompt);
    append_field("ENCLOSING_DECLARATION_SOURCE", enclosing.source, prompt);
  }
  prompt += "ACTIVE_DENIALS ";
  append_u64(
      static_cast<std::uint64_t>(request.obligation.active_denials.size()),
      prompt);
  for (const AgentActiveDenial &denial :
       request.obligation.active_denials) {
    if (sha256(denial.selector) != denial.selector_digest) {
      provider_error(
          diagnostics, "Codex active denial identity is inconsistent");
      return false;
    }
    append_field("DENIAL_SELECTOR", denial.selector, prompt);
    append_field("DENIAL_SHA256", denial.selector_digest.hex(), prompt);
  }
  prompt += "PERMITTED_CONTEXT_FIELDS ";
  append_u64(
      static_cast<std::uint64_t>(request.obligation.context_fields.size()),
      prompt);
  for (const AgentContextField &field : request.obligation.context_fields) {
    append_field("CONTEXT_FIELD_NAME", field.name, prompt);
    append_field("CONTEXT_FIELD_OFFSET", std::to_string(field.offset), prompt);
    append_field("CONTEXT_FIELD_TYPE_SHA256", field.type_digest.hex(), prompt);
    append_field("CONTEXT_FIELD_TYPE_TEXT", field.type_text, prompt);
  }
  prompt += "PARAMETRIC_PARAMETERS ";
  append_u64(
      static_cast<std::uint64_t>(
          request.obligation.parametric_parameters.size()),
      prompt);
  for (const AgentParametricParameter &parameter :
       request.obligation.parametric_parameters) {
    append_field("PARAMETER_NAME", parameter.name, prompt);
    append_field(
        "PARAMETER_KIND", symbol_kind_name(parameter.kind), prompt);
    append_field("PARAMETER_CONSTRAINT", parameter.constraint, prompt);
    append_field("PARAMETER_TYPE_TEXT", parameter.type_text, prompt);
    append_field("PARAMETER_TYPE_SHA256", parameter.type_digest.hex(), prompt);
  }
  prompt += "TYPE_CONTEXTS ";
  append_u64(
      static_cast<std::uint64_t>(request.obligation.type_contexts.size()),
      prompt);
  for (const AgentTypeContext &type : request.obligation.type_contexts) {
    if (sha256(type.definition) != type.definition_digest) {
      provider_error(
          diagnostics, "Codex type context identity is inconsistent");
      return false;
    }
    append_field("TYPE_REFERENCE_SHA256", type.type_digest.hex(), prompt);
    append_field("TYPE_DEFINITION_SHA256", type.definition_digest.hex(), prompt);
    append_field("TYPE_DEFINITION", type.definition, prompt);
  }
  prompt += "IMPORTED_PACKAGES ";
  append_u64(
      static_cast<std::uint64_t>(request.obligation.imported_packages.size()),
      prompt);
  for (std::size_t package_index = 0;
       package_index < request.obligation.imported_packages.size();
       ++package_index) {
    const AgentImportedPackageContext &package =
        request.obligation.imported_packages[package_index];
    if (sha256(package.definition) != package.definition_digest) {
      provider_error(
          diagnostics, "Codex imported package identity is inconsistent");
      return false;
    }
    append_field("IMPORT_ALIAS", package.alias, prompt);
    append_field("IMPORT_ROOT_IDENTITY", package.root_identity, prompt);
    append_field(
        "IMPORT_ROOT_RELATIVE_PATH", package.root_relative_path, prompt);
    append_field(
        "IMPORT_DEFINITION_SHA256", package.definition_digest.hex(), prompt);
    append_field("IMPORT_DEFINITION", package.definition, prompt);
    prompt += "IMPORT_DOCUMENTATION ";
    append_u64(
        static_cast<std::uint64_t>(package.documentation.size()), prompt);
    for (std::size_t documentation_index = 0;
         documentation_index < package.documentation.size();
         ++documentation_index) {
      const AgentDocumentationContext &documentation =
          package.documentation[documentation_index];
      if (documentation.files.size() !=
          documentation.file_contents.size()) {
        provider_error(
            diagnostics,
            "Codex imported documentation identities are inconsistent");
        return false;
      }
      append_field("IMPORT_DOC_ANCHOR", documentation.anchor_name, prompt);
      append_field("IMPORT_DOC_TEXT", documentation.text, prompt);
      append_field(
          "IMPORT_DOC_SHA256", documentation.record_digest.hex(), prompt);
      prompt += "IMPORT_DOC_ATTACHMENTS ";
      append_u64(
          static_cast<std::uint64_t>(documentation.files.size()), prompt);
      for (std::size_t attachment_index = 0;
           attachment_index < documentation.files.size();
           ++attachment_index) {
        const AttachedFile &file = documentation.files[attachment_index];
        const std::string &contents =
            documentation.file_contents[attachment_index];
        if (contents.size() != file.size || sha256(contents) != file.digest) {
          provider_error(
              diagnostics,
              "Codex imported documentation attachment is inconsistent");
          return false;
        }
        const std::string name = imported_documentation_attachment_name(
            package_index, documentation_index, attachment_index);
        if (!write_file(directory / name, contents, diagnostics)) return false;
        append_field("IMPORT_DOC_ATTACHMENT_PATH", file.relative_path, prompt);
        append_field("IMPORT_DOC_ATTACHMENT_FILE", name, prompt);
        append_field(
            "IMPORT_DOC_ATTACHMENT_SHA256", file.digest.hex(), prompt);
      }
    }
  }
  prompt += "GUIDING_JUDGMENTS ";
  append_u64(
      static_cast<std::uint64_t>(
          request.obligation.guiding_judgments.size()),
      prompt);
  for (std::size_t judgment_index = 0;
       judgment_index < request.obligation.guiding_judgments.size();
       ++judgment_index) {
    const AgentJudgmentContext &judgment =
        request.obligation.guiding_judgments[judgment_index];
    if (judgment.files.size() != judgment.file_contents.size()) {
      provider_error(
          diagnostics, "Codex judgment attachment identities are inconsistent");
      return false;
    }
    append_field("JUDGMENT_ANCHOR", judgment.anchor_name, prompt);
    append_field("JUDGMENT_CLAIM", judgment.claim, prompt);
    append_field("JUDGMENT_SHA256", judgment.record_digest.hex(), prompt);
    prompt += "JUDGMENT_ATTACHMENTS ";
    append_u64(static_cast<std::uint64_t>(judgment.files.size()), prompt);
    for (std::size_t attachment_index = 0;
         attachment_index < judgment.files.size(); ++attachment_index) {
      const AttachedFile &file = judgment.files[attachment_index];
      const std::string &contents =
          judgment.file_contents[attachment_index];
      if (contents.size() != file.size || sha256(contents) != file.digest) {
        provider_error(
            diagnostics, "Codex judgment attachment identity is inconsistent");
        return false;
      }
      const std::string name = judgment_attachment_name(
          judgment_index, attachment_index);
      if (!write_file(directory / name, contents, diagnostics)) return false;
      append_field("JUDGMENT_ATTACHMENT_PATH", file.relative_path, prompt);
      append_field("JUDGMENT_ATTACHMENT_FILE", name, prompt);
      append_field("JUDGMENT_ATTACHMENT_SHA256", file.digest.hex(), prompt);
    }
  }
  prompt += "DOCUMENTATION ";
  append_u64(
      static_cast<std::uint64_t>(request.obligation.documentation.size()),
      prompt);
  for (std::size_t documentation_index = 0;
       documentation_index < request.obligation.documentation.size();
       ++documentation_index) {
    const AgentDocumentationContext &documentation =
        request.obligation.documentation[documentation_index];
    if (documentation.files.size() != documentation.file_contents.size()) {
      provider_error(
          diagnostics,
          "Codex documentation attachment identities are inconsistent");
      return false;
    }
    append_field("DOC_ANCHOR", documentation.anchor_name, prompt);
    append_field("DOC_TEXT", documentation.text, prompt);
    append_field("DOC_SHA256", documentation.record_digest.hex(), prompt);
    prompt += "DOC_ATTACHMENTS ";
    append_u64(
        static_cast<std::uint64_t>(documentation.files.size()), prompt);
    for (std::size_t attachment_index = 0;
         attachment_index < documentation.files.size();
         ++attachment_index) {
      const AttachedFile &file = documentation.files[attachment_index];
      const std::string &contents =
          documentation.file_contents[attachment_index];
      if (contents.size() != file.size || sha256(contents) != file.digest) {
        provider_error(
            diagnostics,
            "Codex documentation attachment identity is inconsistent");
        return false;
      }
      const std::string name = documentation_attachment_name(
          documentation_index, attachment_index);
      if (!write_file(directory / name, contents, diagnostics)) return false;
      append_field("DOC_ATTACHMENT_PATH", file.relative_path, prompt);
      append_field("DOC_ATTACHMENT_FILE", name, prompt);
      append_field("DOC_ATTACHMENT_SHA256", file.digest.hex(), prompt);
    }
  }
  append_field("AUTHOR_PROMPT", request.prompt, prompt);
  prompt += "VISIBLE_BINDINGS ";
  append_u64(
      static_cast<std::uint64_t>(request.obligation.visible_bindings.size()),
      prompt);
  for (const AgentVisibleBinding &binding :
       request.obligation.visible_bindings) {
    append_field("BINDING_NAME", binding.name, prompt);
    append_field("BINDING_KIND", symbol_kind_name(binding.kind), prompt);
    append_field("BINDING_TYPE_SHA256", binding.type_digest.hex(), prompt);
    append_field("BINDING_TYPE_TEXT", binding.type_text, prompt);
    append_field(
        "BINDING_HAS_CONSTANT", binding.has_constant ? "true" : "false", prompt);
    if (binding.has_constant) {
      if (sha256(binding.constant_definition) != binding.constant_digest) {
        provider_error(
            diagnostics, "Codex visible constant identity is inconsistent");
        return false;
      }
      append_field(
          "BINDING_CONSTANT_SHA256", binding.constant_digest.hex(), prompt);
      append_field(
          "BINDING_CONSTANT", binding.constant_definition, prompt);
    }
  }
  prompt += "ATTACHMENTS ";
  append_u64(static_cast<std::uint64_t>(request.attachments.size()), prompt);
  for (std::size_t index = 0; index < request.attachments.size(); ++index) {
    const SynthesisRequestFile &attachment = request.attachments[index];
    if (attachment.contents.size() != attachment.size ||
        sha256(attachment.contents) != attachment.digest) {
      provider_error(
          diagnostics, "Codex request attachment identity is inconsistent");
      return false;
    }
    const std::string name = attachment_name(index);
    if (!write_file(directory / name, attachment.contents, diagnostics)) {
      return false;
    }
    append_field("ATTACHMENT_PATH", attachment.relative_path, prompt);
    append_field("ATTACHMENT_FILE", name, prompt);
    append_field("ATTACHMENT_SHA256", attachment.digest.hex(), prompt);
  }
  return true;
}

[[nodiscard]] std::optional<unsigned char> hex_digit(char value) {
  if (value >= '0' && value <= '9') {
    return static_cast<unsigned char>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<unsigned char>(value - 'a' + 10);
  }
  if (value >= 'A' && value <= 'F') {
    return static_cast<unsigned char>(value - 'A' + 10);
  }
  return std::nullopt;
}

void append_utf8(std::uint32_t scalar, std::string &output) {
  if (scalar <= 0x7fU) {
    output.push_back(static_cast<char>(scalar));
  } else if (scalar <= 0x7ffU) {
    output.push_back(static_cast<char>(0xc0U | (scalar >> 6U)));
    output.push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
  } else if (scalar <= 0xffffU) {
    output.push_back(static_cast<char>(0xe0U | (scalar >> 12U)));
    output.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
  } else {
    output.push_back(static_cast<char>(0xf0U | (scalar >> 18U)));
    output.push_back(static_cast<char>(0x80U | ((scalar >> 12U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | ((scalar >> 6U) & 0x3fU)));
    output.push_back(static_cast<char>(0x80U | (scalar & 0x3fU)));
  }
}

class SourceResponseParser {
public:
  explicit SourceResponseParser(std::string_view input) : input_(input) {}

  [[nodiscard]] bool parse(std::string &source) {
    whitespace();
    if (!take('{') || !json_string(key_) || key_ != "source" || !take(':') ||
        !json_string(source) || !take('}')) {
      return false;
    }
    whitespace();
    return position_ == input_.size();
  }

private:
  void whitespace() {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\n' ||
            input_[position_] == '\r' || input_[position_] == '\t')) {
      ++position_;
    }
  }

  [[nodiscard]] bool take(char expected) {
    whitespace();
    if (position_ >= input_.size() || input_[position_] != expected) return false;
    ++position_;
    return true;
  }

  [[nodiscard]] bool unicode_escape(std::uint32_t &scalar) {
    if (position_ + 4 > input_.size()) return false;
    scalar = 0;
    for (std::size_t index = 0; index < 4; ++index) {
      const std::optional<unsigned char> digit = hex_digit(input_[position_++]);
      if (!digit.has_value()) return false;
      scalar = scalar * 16U + *digit;
    }
    return true;
  }

  [[nodiscard]] bool json_string(std::string &output) {
    whitespace();
    if (position_ >= input_.size() || input_[position_] != '"') return false;
    ++position_;
    output.clear();
    while (position_ < input_.size()) {
      const char value = input_[position_++];
      if (value == '"') return true;
      if (static_cast<unsigned char>(value) < 0x20U) return false;
      if (value != '\\') {
        output.push_back(value);
        continue;
      }
      if (position_ >= input_.size()) return false;
      const char escape = input_[position_++];
      switch (escape) {
      case '"': output.push_back('"'); break;
      case '\\': output.push_back('\\'); break;
      case '/': output.push_back('/'); break;
      case 'b': output.push_back('\b'); break;
      case 'f': output.push_back('\f'); break;
      case 'n': output.push_back('\n'); break;
      case 'r': output.push_back('\r'); break;
      case 't': output.push_back('\t'); break;
      case 'u': {
        std::uint32_t scalar = 0;
        if (!unicode_escape(scalar)) return false;
        if (scalar >= 0xd800U && scalar <= 0xdbffU) {
          if (position_ + 2 > input_.size() || input_[position_] != '\\' ||
              input_[position_ + 1] != 'u') {
            return false;
          }
          position_ += 2;
          std::uint32_t low = 0;
          if (!unicode_escape(low) || low < 0xdc00U || low > 0xdfffU) {
            return false;
          }
          scalar = 0x10000U + ((scalar - 0xd800U) << 10U) +
              (low - 0xdc00U);
        } else if (scalar >= 0xdc00U && scalar <= 0xdfffU) {
          return false;
        }
        append_utf8(scalar, output);
        break;
      }
      default: return false;
      }
    }
    return false;
  }

  std::string_view input_;
  std::size_t position_ = 0;
  std::string key_;
};

// Starts exactly one documented non-interactive Codex process. All arguments
// are separate execv entries and the prompt is an already-opened regular file.
[[nodiscard]] bool run_codex_once(
    const CodexCliProviderState &state,
    const std::filesystem::path &directory,
    DiagnosticSink &diagnostics) {
#if defined(__APPLE__) || defined(__unix__)
  const std::filesystem::path prompt = directory / "prompt.txt";
  const std::filesystem::path schema = directory / "schema.json";
  const std::filesystem::path output = directory / "response.json";
  const std::filesystem::path log = directory / "codex.log";
  const pid_t child = ::fork();
  if (child < 0) {
    provider_error(
        diagnostics, "cannot fork Codex CLI: " + std::string(std::strerror(errno)));
    return false;
  }
  if (child == 0) {
    const int input = ::open(prompt.c_str(), O_RDONLY);
    const int log_file = ::open(
        log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (input < 0 || log_file < 0 || ::dup2(input, STDIN_FILENO) < 0 ||
        ::dup2(log_file, STDOUT_FILENO) < 0 ||
        ::dup2(log_file, STDERR_FILENO) < 0) {
      ::_exit(126);
    }
    (void)::close(input);
    (void)::close(log_file);
    std::vector<std::string> arguments{
        state.executable.string(),
        "exec",
        "--ephemeral",
        "--sandbox", "read-only",
        "--skip-git-repo-check",
        "--ignore-user-config",
        "--ignore-rules",
        "--color", "never",
        "--model", state.model,
        "--cd", directory.string(),
        "--output-schema", schema.string(),
        "--output-last-message", output.string(),
        "-",
    };
    std::vector<char *> raw;
    raw.reserve(arguments.size() + 1);
    for (std::string &argument : arguments) raw.push_back(argument.data());
    raw.push_back(nullptr);
    ::execv(state.executable.c_str(), raw.data());
    ::_exit(127);
  }

  // Polling keeps the implementation simple and gives the parent an exact
  // deadline without installing process-global signal handlers. Ten
  // milliseconds is negligible relative to provider latency and keeps tests
  // responsive. A timeout always reaps the child before returning.
  const auto timeout = std::chrono::milliseconds(
      static_cast<std::chrono::milliseconds::rep>(
          state.timeout_milliseconds));
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (true) {
    const pid_t waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) break;
    if (waited < 0 && errno != EINTR) {
      // Reaping can overwrite errno, so preserve the wait failure that the
      // diagnostic is meant to report.
      const int wait_error = errno;
      (void)::kill(child, SIGKILL);
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      provider_error(
          diagnostics, "cannot wait for Codex CLI: " +
              std::string(std::strerror(wait_error)));
      return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      (void)::kill(child, SIGKILL);
      while (::waitpid(child, &status, 0) < 0 && errno == EINTR) {
      }
      provider_error(diagnostics, "Codex CLI attempt timed out");
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    std::string log_contents;
    DiagnosticSink ignored;
    if (read_file(log, kMaximumCodexLogBytes, log_contents, ignored) &&
        !log_contents.empty()) {
      provider_error(diagnostics, "Codex CLI failed: " + log_contents);
    } else {
      provider_error(diagnostics, "Codex CLI failed without readable output");
    }
    return false;
  }
  return true;
#else
  (void)state;
  (void)directory;
  provider_error(diagnostics, "Codex CLI provider is unavailable on this host");
  return false;
#endif
}

[[nodiscard]] bool synthesize_with_codex(
    void *opaque,
    const SynthesisRequest &request,
    SynthesisResponse &response,
    DiagnosticSink &diagnostics) {
  auto *state = static_cast<CodexCliProviderState *>(opaque);
  TemporaryDirectory temporary;
  if (!temporary.create(diagnostics)) return false;
  std::string prompt;
  if (!prepare_request(request, temporary.path(), prompt, diagnostics) ||
      !write_file(temporary.path() / "schema.json", kOutputSchema, diagnostics) ||
      !write_file(temporary.path() / "prompt.txt", prompt, diagnostics)) {
    return false;
  }
  bool completed = false;
  std::string last_failure;
  for (std::uint32_t attempt = 0; attempt < state->maximum_attempts; ++attempt) {
    // A failed process may have left a partial final message. Removing it makes
    // each attempt independent and prevents a later successful exit from
    // accidentally consuming stale bytes.
    std::error_code ignored;
    std::filesystem::remove(temporary.path() / "response.json", ignored);
    DiagnosticSink attempt_diagnostics;
    if (run_codex_once(*state, temporary.path(), attempt_diagnostics)) {
      completed = true;
      break;
    }
    if (!attempt_diagnostics.diagnostics().empty()) {
      last_failure = attempt_diagnostics.diagnostics().back().message;
    }
  }
  if (!completed) {
    provider_error(
        diagnostics,
        "Codex provider failed after " +
            std::to_string(state->maximum_attempts) + " attempt(s): " +
            (last_failure.empty() ? "no diagnostic" : last_failure));
    return false;
  }
  std::string json;
  if (!read_file(
          temporary.path() / "response.json",
          kMaximumCodexOutputBytes,
          json,
          diagnostics)) {
    return false;
  }
  SourceResponseParser parser(json);
  if (!parser.parse(response.source)) {
    provider_error(
        diagnostics, "Codex final response does not match the source schema");
    return false;
  }
  return true;
}

// Hashes one regular executable without inserting it into SourceManager. The
// executable may be a script or native binary; exact bytes, not file metadata,
// are the configuration input.
[[nodiscard]] std::optional<Sha256Digest> hash_executable(
    const std::filesystem::path &path,
    DiagnosticSink &diagnostics) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    provider_error(
        diagnostics, "cannot open Codex executable '" + path.string() + "'");
    return std::nullopt;
  }
  Sha256 hash;
  std::array<char, 64 * 1024> buffer{};
  while (input) {
    input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize count = input.gcount();
    if (count > 0) {
      hash.update(std::string_view(
          buffer.data(), static_cast<std::size_t>(count)));
    }
  }
  if (!input.eof()) {
    provider_error(diagnostics, "cannot read complete Codex executable");
    return std::nullopt;
  }
  return hash.finalize();
}

} // namespace

SynthesisProvider configure_codex_cli_provider(
    const CodexCliProviderOptions &options,
    CodexCliProviderState &state,
    DiagnosticSink &diagnostics) {
  state = {};
  if (options.model.empty()) {
    provider_error(diagnostics, "Codex provider model identity must not be empty");
    return {};
  }
  if (options.timeout_milliseconds == 0 || options.maximum_attempts == 0 ||
      options.maximum_attempts > 8) {
    provider_error(
        diagnostics,
        "Codex timeout must be positive and attempts must be between 1 and 8");
    return {};
  }
  std::error_code error;
  const std::filesystem::path executable =
      std::filesystem::canonical(options.executable, error);
  if (error || !std::filesystem::is_regular_file(executable, error) || error) {
    provider_error(
        diagnostics,
        "Codex executable path is invalid: '" + options.executable.string() + "'");
    return {};
  }
  const std::optional<Sha256Digest> executable_digest =
      hash_executable(executable, diagnostics);
  if (!executable_digest.has_value()) return {};

  Sha256 configuration;
  configuration.update("draft.codex-cli-provider.v7");
  configuration.update(executable_digest->bytes);
  configuration.update(options.model);
  configuration.update(";timeout-ms=");
  configuration.update(std::to_string(options.timeout_milliseconds));
  configuration.update(";attempts=");
  configuration.update(std::to_string(options.maximum_attempts));
  configuration.update(kPromptContractIdentity);
  configuration.update(kOutputSchema);
  configuration.update(
      "exec;ephemeral;sandbox=read-only;skip-git;ignore-user-config;"
      "ignore-rules;color=never;stdin;output-schema;output-last-message");
  state.executable = executable;
  state.model = options.model;
  state.timeout_milliseconds = options.timeout_milliseconds;
  state.maximum_attempts = options.maximum_attempts;
  state.configuration_identity =
      "codex-config-" + configuration.finalize().hex();

  SynthesisProvider provider;
  provider.provider_identity = "openai-codex-cli-v13";
  provider.model_identity = state.model;
  provider.configuration_identity = state.configuration_identity;
  provider.state = &state;
  provider.synthesize = synthesize_with_codex;
  return provider;
}

} // namespace draft
