// Strict line-format parsing for explicit foreign denial summaries.
//
// Summaries describe semantic effects which Draft cannot infer from a foreign
// body. They are read and validated on every command that elects to use them.
// They deliberately do not contain or verify an artifact digest: one library
// file cannot identify the loader and transitive native environment which gives
// it meaning, and the compiler should not claim otherwise.

#include "backend/foreign_summaries.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace draft {
namespace {

constexpr std::size_t maximum_summary_bytes = 16U * 1024U * 1024U;

[[nodiscard]] bool valid_field(std::string_view field, bool allow_empty) {
  if (field.empty()) return allow_empty;
  for (char character : field) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (byte < 0x21U || byte == 0x7fU) return false;
  }
  return true;
}

[[nodiscard]] std::vector<std::string_view> split_tabs(
    std::string_view line) {
  std::vector<std::string_view> result;
  std::size_t begin = 0;
  while (true) {
    const std::size_t tab = line.find('\t', begin);
    if (tab == std::string_view::npos) {
      result.push_back(line.substr(begin));
      return result;
    }
    result.push_back(line.substr(begin, tab - begin));
    begin = tab + 1;
  }
}

[[nodiscard]] std::optional<std::uint32_t> parse_index(
    std::string_view text) {
  if (text.empty()) return std::nullopt;
  std::uint64_t value = 0;
  for (char byte : text) {
    if (byte < '0' || byte > '9') return std::nullopt;
    value = value * 10U + static_cast<unsigned>(byte - '0');
    if (value > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
  }
  if (text.size() > 1 && text.front() == '0') return std::nullopt;
  return static_cast<std::uint32_t>(value);
}

[[nodiscard]] bool parse_effect(
    std::string_view line,
    ForeignAuditEffect &effect,
    std::string &reason) {
  const std::vector<std::string_view> fields = split_tabs(line);
  if (fields.empty()) {
    reason = "empty summary record";
    return false;
  }
  if (fields[0] == "callback") {
    if (fields.size() < 2) {
      reason = "callback record must contain a parameter index";
      return false;
    }
    const std::optional<std::uint32_t> index = parse_index(fields[1]);
    if (!index.has_value()) {
      reason = "callback parameter index is not canonical u32";
      return false;
    }
    effect.kind = EffectKind::FlowCall;
    effect.detail = "audited foreign callback";
    effect.flow_parameter = *index;
    for (std::size_t field = 2; field < fields.size(); ++field) {
      if (!valid_field(fields[field], false)) {
        reason = "callback record contains an invalid typed field path";
        return false;
      }
      effect.flow_path.push_back(std::string(fields[field]));
    }
    return true;
  }
  if (fields[0] != "effect" || fields.size() < 2) {
    reason = "symbol body must contain effect or callback records";
    return false;
  }
  if (fields[1] == "assert" && fields.size() == 2) {
    effect.kind = EffectKind::RuntimeAssert;
    effect.detail = "audited foreign assert";
    return true;
  }
  if (fields[1] == "assembly" && fields.size() == 2) {
    effect.kind = EffectKind::Assembly;
    effect.detail = "audited foreign assembly";
    return true;
  }
  if (fields[1] == "unchecked" && fields.size() == 2) {
    effect.kind = EffectKind::Unchecked;
    effect.detail = "audited foreign unchecked operation";
    return true;
  }
  if (fields[1] == "context" && fields.size() == 3 &&
      valid_field(fields[2], false)) {
    effect.kind = EffectKind::ContextField;
    effect.detail = std::string(fields[2]);
    return true;
  }
  if (fields[1] == "declaration" && fields.size() == 5 &&
      valid_field(fields[2], false) && valid_field(fields[3], true) &&
      valid_field(fields[4], false)) {
    effect.kind = EffectKind::Declaration;
    effect.root_identity = std::string(fields[2]);
    effect.root_relative_path = std::string(fields[3]);
    effect.declaration = std::string(fields[4]);
    effect.detail = "audited reachable Draft declaration";
    return true;
  }
  reason = "effect record has an unsupported kind or field shape";
  return false;
}

[[nodiscard]] bool parse_summary(
    std::string_view bytes,
    ForeignProviderAudit &audit,
    std::string &reason) {
  if (bytes.empty() || bytes.back() != '\n' ||
      bytes.find('\r') != std::string_view::npos) {
    reason = "summary must use LF lines and end with one LF";
    return false;
  }
  std::vector<std::string_view> lines;
  std::size_t begin = 0;
  while (begin < bytes.size()) {
    const std::size_t newline = bytes.find('\n', begin);
    lines.push_back(bytes.substr(begin, newline - begin));
    begin = newline + 1;
  }
  if (lines.size() < 3 ||
      lines[0] != "draft-provider-denial-summary-v2") {
    reason = "summary has an unsupported format header";
    return false;
  }
  const std::vector<std::string_view> provider = split_tabs(lines[1]);
  if (provider.size() != 2 || provider[0] != "provider" ||
      !valid_field(provider[1], false)) {
    reason = "summary provider row is invalid";
    return false;
  }
  audit.provider = std::string(provider[1]);

  std::size_t line_index = 2;
  std::string previous_symbol;
  while (line_index < lines.size()) {
    const std::vector<std::string_view> symbol_row =
        split_tabs(lines[line_index++]);
    if (symbol_row.size() != 2 || symbol_row[0] != "symbol" ||
        !valid_field(symbol_row[1], false)) {
      reason = "summary symbol row is invalid";
      return false;
    }
    if (!previous_symbol.empty() && previous_symbol >= symbol_row[1]) {
      reason = "summary symbols must be bytewise sorted and unique";
      return false;
    }
    ForeignAuditSymbol symbol;
    symbol.linker_name = std::string(symbol_row[1]);
    previous_symbol = symbol.linker_name;
    std::string previous_record;
    bool saw_end = false;
    while (line_index < lines.size()) {
      const std::string_view line = lines[line_index++];
      if (line == "end") {
        saw_end = true;
        break;
      }
      if (!previous_record.empty() && previous_record >= line) {
        reason = "summary symbol records must be sorted and unique";
        return false;
      }
      ForeignAuditEffect effect;
      if (!parse_effect(line, effect, reason)) return false;
      previous_record = std::string(line);
      symbol.effects.push_back(std::move(effect));
    }
    if (!saw_end) {
      reason = "summary symbol block has no end row";
      return false;
    }
    audit.symbols.push_back(std::move(symbol));
  }
  if (audit.symbols.empty()) {
    reason = "summary must contain at least one symbol block";
    return false;
  }
  return true;
}

[[nodiscard]] bool read_regular_file(
    const ForeignProviderSummaryInput &input,
    std::string &bytes,
    DiagnosticSink &diagnostics) {
  if (input.provider.empty() || input.path.empty() ||
      !input.path.is_absolute()) {
    diagnostics.error(
        SourceRange::invalid(),
        "foreign provider summary requires a provider and absolute path");
    return false;
  }
  std::error_code error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(input.path, error);
  if (error || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    diagnostics.error(
        SourceRange::invalid(),
        "foreign provider '" + input.provider +
            "' summary must be a real regular file, not a symlink");
    return false;
  }
  const std::filesystem::path canonical =
      std::filesystem::canonical(input.path, error);
  if (error) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot canonicalize foreign provider summary: " + error.message());
    return false;
  }
  std::ifstream stream(canonical, std::ios::binary);
  if (!stream) {
    diagnostics.error(
        SourceRange::invalid(), "cannot open foreign provider summary");
    return false;
  }
  bytes.assign(
      std::istreambuf_iterator<char>(stream),
      std::istreambuf_iterator<char>());
  if (stream.bad() || bytes.size() > maximum_summary_bytes) {
    diagnostics.error(
        SourceRange::invalid(),
        "cannot read foreign provider summary or summary exceeds 16 MiB");
    return false;
  }
  return true;
}

[[nodiscard]] bool has_artifact(
    std::span<const ForeignProviderInput> artifacts,
    std::string_view provider) {
  for (const ForeignProviderInput &artifact : artifacts) {
    if (artifact.provider == provider) {
      return true;
    }
  }
  return false;
}

} // namespace

bool parse_foreign_provider_summary_input(
    std::string_view spelling,
    ForeignProviderSummaryInput &input,
    std::string &reason) {
  const std::size_t colon = spelling.find(':');
  if (colon == 0 || colon == std::string_view::npos ||
      colon + 1 >= spelling.size()) {
    reason = "provider summary mapping must be name:path";
    return false;
  }
  input.provider = std::string(spelling.substr(0, colon));
  std::error_code error;
  input.path = std::filesystem::absolute(
      std::filesystem::path(spelling.substr(colon + 1)), error).lexically_normal();
  if (error) {
    reason = "cannot make provider summary path absolute: " + error.message();
    return false;
  }
  return true;
}

bool load_foreign_provider_summaries(
    std::span<const ForeignProviderSummaryInput> inputs,
    std::span<const ForeignProviderInput> artifacts,
    std::vector<ForeignProviderAudit> &audits,
    DiagnosticSink &diagnostics) {
  const std::size_t initial_errors = diagnostics.error_count();
  std::vector<ForeignProviderAudit> parsed;
  for (std::size_t index = 0; index < inputs.size(); ++index) {
    for (std::size_t previous = 0; previous < index; ++previous) {
      if (inputs[previous].provider == inputs[index].provider) {
        diagnostics.error(
            SourceRange::invalid(),
            "foreign provider '" + inputs[index].provider +
                "' summary is configured more than once");
        return false;
      }
    }
    if (!has_artifact(artifacts, inputs[index].provider)) {
      diagnostics.error(
          SourceRange::invalid(),
          "foreign provider '" + inputs[index].provider +
              "' summary has no artifact mapping");
      return false;
    }
    std::string bytes;
    if (!read_regular_file(inputs[index], bytes, diagnostics)) {
      return false;
    }
    ForeignProviderAudit audit;
    std::string reason;
    if (!parse_summary(bytes, audit, reason)) {
      diagnostics.error(
          SourceRange::invalid(),
          "invalid foreign provider '" + inputs[index].provider +
              "' summary: " + reason);
      return false;
    }
    if (audit.provider != inputs[index].provider) {
      diagnostics.error(
          SourceRange::invalid(),
          "foreign provider summary identity does not match '" +
              inputs[index].provider + "'");
      return false;
    }
    parsed.push_back(std::move(audit));
  }
  audits = std::move(parsed);
  return diagnostics.error_count() == initial_errors;
}

} // namespace draft
