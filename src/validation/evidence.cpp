// Canonical validation evidence encoding and report decoding.

#include "validation/evidence.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

void hash_u64(Sha256 &hash, std::uint64_t value) {
  std::array<std::uint8_t, 8> bytes{};
  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[bytes.size() - 1 - index] =
        static_cast<std::uint8_t>(value >> (index * 8U));
  }
  hash.update(bytes);
}

void hash_field(Sha256 &hash, std::string_view value) {
  hash_u64(hash, static_cast<std::uint64_t>(value.size()));
  hash.update(value);
}

[[nodiscard]] bool entry_less(
    const ValidationEntry &left, const ValidationEntry &right) {
  if (left.package.root_identity != right.package.root_identity) {
    return left.package.root_identity < right.package.root_identity;
  }
  if (left.package.root_relative_path != right.package.root_relative_path) {
    return left.package.root_relative_path < right.package.root_relative_path;
  }
  return left.procedure < right.procedure;
}

void hash_entry(Sha256 &hash, const ValidationEntry &entry) {
  hash_field(hash, entry.package.root_identity);
  hash_field(hash, entry.package.root_relative_path);
  hash_field(hash, entry.procedure);
  hash_u64(hash, entry.state_size);
  hash_u64(hash, entry.state_alignment);
  hash_u64(hash, entry.failure_offset);
  hash_u64(hash, entry.report_size);
}

void append_json_string(std::string_view value, std::string &output) {
  constexpr char digits[] = "0123456789abcdef";
  output.push_back('"');
  for (char character : value) {
    const unsigned char byte = static_cast<unsigned char>(character);
    switch (byte) {
    case '"': output += "\\\""; break;
    case '\\': output += "\\\\"; break;
    case '\b': output += "\\b"; break;
    case '\f': output += "\\f"; break;
    case '\n': output += "\\n"; break;
    case '\r': output += "\\r"; break;
    case '\t': output += "\\t"; break;
    default:
      if (byte < 0x20U) {
        output += "\\u00";
        output.push_back(digits[byte >> 4U]);
        output.push_back(digits[byte & 0x0fU]);
      } else {
        output.push_back(static_cast<char>(byte));
      }
      break;
    }
  }
  output.push_back('"');
}

[[nodiscard]] std::optional<ValidationKind> parse_validation_kind(
    std::string_view spelling) {
  if (spelling == "test") return ValidationKind::Test;
  if (spelling == "benchmark") return ValidationKind::Benchmark;
  return std::nullopt;
}

class EvidenceParser {
public:
  EvidenceParser(std::string_view input, DiagnosticSink &diagnostics)
      : input_(input), diagnostics_(diagnostics) {}

  [[nodiscard]] bool parse(ValidationEvidence &evidence) {
    ValidationEvidence parsed;
    std::string kind;
    std::string verdict;
    std::uint64_t exit_code = 0;
    std::uint64_t signal = 0;
    if (!punctuation('{') ||
        !key("format") || !string(parsed.format) || !comma() ||
        !key("key") || !digest(parsed.key) || !comma() ||
        !key("attempt") || !unsigned_integer(parsed.attempt) || !comma() ||
        !key("resolved_program") || !digest(parsed.resolved_program) ||
        !comma() || !key("kind") || !string(kind) || !comma() ||
        !key("target") || !string(parsed.target_identity) || !comma() ||
        !key("compiler") || !string(parsed.compiler_identity) || !comma() ||
        !key("toolchain") || !string(parsed.toolchain_identity) || !comma() ||
        !key("environment") || !string(parsed.environment_identity) || !comma() ||
        !key("runner") || !string(parsed.runner_identity) || !comma() ||
        !key("policy") || !string(parsed.policy_identity) || !comma() ||
        !key("artifact") || !string(parsed.artifact_identity) || !comma() ||
        !key("warmup_runs") || !unsigned_integer(parsed.warmup_runs) ||
        !comma() || !key("sample_runs") ||
        !unsigned_integer(parsed.sample_runs) || !comma() ||
        !key("entries") || !entries(parsed.entries) || !comma() ||
        !key("observations") || !observations(parsed.observations) ||
        !comma() || !key("observations_complete") ||
        !boolean(parsed.observations_complete) || !comma() ||
        !key("verdict") || !string(verdict) || !comma() ||
        !key("exit_code") || !unsigned_integer(exit_code) || !comma() ||
        !key("signal") || !unsigned_integer(signal) || !punctuation('}')) {
      return false;
    }
    whitespace();
    if (position_ != input_.size()) return fail("trailing bytes after evidence");
    const std::optional<ValidationKind> parsed_kind =
        parse_validation_kind(kind);
    if (parsed.format != "draft-validation-evidence-v1") {
      return fail("unsupported validation evidence format");
    }
    if (!parsed_kind.has_value()) return fail("invalid validation kind");
    parsed.kind = *parsed_kind;
    if (verdict == "pass") {
      parsed.passed = true;
    } else if (verdict == "fail") {
      parsed.passed = false;
    } else {
      return fail("invalid validation verdict");
    }
    if (exit_code > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
        signal > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
      return fail("process outcome is outside the supported integer range");
    }
    parsed.exit_code = static_cast<int>(exit_code);
    parsed.signal = static_cast<int>(signal);
    if (!validate(parsed)) return false;
    evidence = std::move(parsed);
    return true;
  }

private:
  [[nodiscard]] bool fail(std::string message) {
    if (!failed_) {
      diagnostics_.error(
          SourceRange::invalid(),
          "invalid validation evidence at byte " +
              std::to_string(position_) + ": " + std::move(message));
      failed_ = true;
    }
    return false;
  }

  void whitespace() {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\n' ||
            input_[position_] == '\r' || input_[position_] == '\t')) {
      ++position_;
    }
  }

  [[nodiscard]] bool punctuation(char expected) {
    whitespace();
    if (position_ >= input_.size() || input_[position_] != expected) {
      return fail(std::string("expected '") + expected + "'");
    }
    ++position_;
    return true;
  }

  [[nodiscard]] bool comma() { return punctuation(','); }

  [[nodiscard]] bool key(std::string_view expected) {
    std::string actual;
    if (!string(actual)) return false;
    if (actual != expected) {
      return fail("expected field '" + std::string(expected) + "'");
    }
    return punctuation(':');
  }

  [[nodiscard]] static std::optional<unsigned char> hex(char value) {
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

  [[nodiscard]] bool string(std::string &result) {
    whitespace();
    if (position_ >= input_.size() || input_[position_] != '"') {
      return fail("expected JSON string");
    }
    ++position_;
    result.clear();
    while (position_ < input_.size()) {
      const char value = input_[position_++];
      if (value == '"') return true;
      if (static_cast<unsigned char>(value) < 0x20U) {
        return fail("unescaped control byte in string");
      }
      if (value != '\\') {
        result.push_back(value);
        continue;
      }
      if (position_ >= input_.size()) return fail("unterminated string escape");
      const char escape = input_[position_++];
      switch (escape) {
      case '"': result.push_back('"'); break;
      case '\\': result.push_back('\\'); break;
      case '/': result.push_back('/'); break;
      case 'b': result.push_back('\b'); break;
      case 'f': result.push_back('\f'); break;
      case 'n': result.push_back('\n'); break;
      case 'r': result.push_back('\r'); break;
      case 't': result.push_back('\t'); break;
      case 'u': {
        if (position_ + 4 > input_.size() ||
            input_.substr(position_, 2) != "00") {
          return fail("only byte-sized JSON unicode escapes are canonical");
        }
        const std::optional<unsigned char> high = hex(input_[position_ + 2]);
        const std::optional<unsigned char> low = hex(input_[position_ + 3]);
        if (!high.has_value() || !low.has_value()) {
          return fail("invalid JSON unicode escape");
        }
        const unsigned char byte =
            static_cast<unsigned char>((*high << 4U) | *low);
        if (byte >= 0x20U) {
          return fail("non-control byte uses noncanonical unicode escape");
        }
        result.push_back(static_cast<char>(byte));
        position_ += 4;
        break;
      }
      default: return fail("invalid JSON string escape");
      }
    }
    return fail("unterminated JSON string");
  }

  [[nodiscard]] bool digest(Sha256Digest &result) {
    std::string encoded;
    if (!string(encoded)) return false;
    const std::optional<Sha256Digest> parsed =
        Sha256Digest::from_hex(encoded);
    if (!parsed.has_value()) {
      return fail("digest must contain 64 hexadecimal digits");
    }
    result = *parsed;
    return true;
  }

  [[nodiscard]] bool boolean(bool &result) {
    whitespace();
    if (input_.substr(position_, 4) == "true") {
      position_ += 4;
      result = true;
      return true;
    }
    if (input_.substr(position_, 5) == "false") {
      position_ += 5;
      result = false;
      return true;
    }
    return fail("expected JSON boolean");
  }

  [[nodiscard]] bool unsigned_integer(std::uint64_t &result) {
    whitespace();
    const std::size_t begin = position_;
    while (position_ < input_.size() &&
           input_[position_] >= '0' && input_[position_] <= '9') {
      ++position_;
    }
    if (begin == position_ ||
        (position_ - begin > 1 && input_[begin] == '0')) {
      return fail("expected canonical unsigned integer");
    }
    const char *first = input_.data() + begin;
    const char *last = input_.data() + position_;
    const std::from_chars_result converted =
        std::from_chars(first, last, result);
    return converted.ec == std::errc{} && converted.ptr == last
        ? true
        : fail("unsigned integer is out of range");
  }

  [[nodiscard]] bool signed_integer(std::int64_t &result) {
    whitespace();
    const std::size_t begin = position_;
    if (position_ < input_.size() && input_[position_] == '-') ++position_;
    const std::size_t digits = position_;
    while (position_ < input_.size() &&
           input_[position_] >= '0' && input_[position_] <= '9') {
      ++position_;
    }
    if (digits == position_ ||
        (position_ - digits > 1 && input_[digits] == '0') ||
        (input_[begin] == '-' && position_ - begin == 2 &&
         input_[digits] == '0')) {
      return fail("expected canonical signed integer");
    }
    const char *first = input_.data() + begin;
    const char *last = input_.data() + position_;
    const std::from_chars_result converted =
        std::from_chars(first, last, result);
    return converted.ec == std::errc{} && converted.ptr == last
        ? true
        : fail("signed integer is out of range");
  }

  [[nodiscard]] bool entry(ValidationEntry &entry) {
    std::string kind;
    if (!punctuation('{') ||
        !key("kind") || !string(kind) || !comma() ||
        !key("root") || !string(entry.package.root_identity) || !comma() ||
        !key("package") ||
        !string(entry.package.root_relative_path) || !comma() ||
        !key("procedure") || !string(entry.procedure) || !comma() ||
        !key("state_size") || !unsigned_integer(entry.state_size) || !comma() ||
        !key("state_alignment") ||
        !unsigned_integer(entry.state_alignment) || !comma() ||
        !key("failure_offset") ||
        !unsigned_integer(entry.failure_offset) || !comma() ||
        !key("report_size") || !unsigned_integer(entry.report_size) ||
        !punctuation('}')) {
      return false;
    }
    const std::optional<ValidationKind> parsed_kind =
        parse_validation_kind(kind);
    if (!parsed_kind.has_value()) return fail("entry has invalid kind");
    entry.kind = *parsed_kind;
    return true;
  }

  [[nodiscard]] bool entries(std::vector<ValidationEntry> &result) {
    if (!punctuation('[')) return false;
    whitespace();
    if (position_ < input_.size() && input_[position_] == ']') {
      ++position_;
      return true;
    }
    while (true) {
      ValidationEntry value;
      if (!entry(value)) return false;
      result.push_back(std::move(value));
      whitespace();
      if (position_ < input_.size() && input_[position_] == ']') {
        ++position_;
        return true;
      }
      if (!comma()) return false;
    }
  }

  [[nodiscard]] bool durations(std::vector<std::int64_t> &result) {
    if (!punctuation('[')) return false;
    whitespace();
    if (position_ < input_.size() && input_[position_] == ']') {
      ++position_;
      return true;
    }
    while (true) {
      std::int64_t value = 0;
      if (!signed_integer(value)) return false;
      result.push_back(value);
      whitespace();
      if (position_ < input_.size() && input_[position_] == ']') {
        ++position_;
        return true;
      }
      if (!comma()) return false;
    }
  }

  [[nodiscard]] bool observation(ValidationObservation &observation) {
    if (!punctuation('{') ||
        !key("root") || !string(observation.package.root_identity) ||
        !comma() || !key("package") ||
        !string(observation.package.root_relative_path) || !comma() ||
        !key("procedure") || !string(observation.procedure) || !comma() ||
        !key("checks") || !unsigned_integer(observation.checks) || !comma() ||
        !key("library_samples") ||
        !unsigned_integer(observation.library_samples) || !comma() ||
        !key("failures") || !unsigned_integer(observation.failures) ||
        !comma() || !key("maximum_time_ns") ||
        !signed_integer(observation.maximum_time_ns) || !comma() ||
        !key("durations_ns") || !durations(observation.durations_ns) ||
        !punctuation('}')) {
      return false;
    }
    return true;
  }

  [[nodiscard]] bool observations(
      std::vector<ValidationObservation> &result) {
    if (!punctuation('[')) return false;
    whitespace();
    if (position_ < input_.size() && input_[position_] == ']') {
      ++position_;
      return true;
    }
    while (true) {
      ValidationObservation value;
      if (!observation(value)) return false;
      result.push_back(std::move(value));
      whitespace();
      if (position_ < input_.size() && input_[position_] == ']') {
        ++position_;
        return true;
      }
      if (!comma()) return false;
    }
  }

  [[nodiscard]] bool validate(const ValidationEvidence &evidence) {
    if (evidence.attempt == 0 || evidence.target_identity.empty() ||
        evidence.compiler_identity.empty() ||
        evidence.toolchain_identity.empty() ||
        evidence.environment_identity.empty() ||
        evidence.runner_identity.empty() || evidence.policy_identity.empty() ||
        evidence.artifact_identity.empty()) {
      return fail("required identity or attempt field is empty");
    }
    if (evidence.entries.size() != evidence.observations.size()) {
      return fail("entry and observation counts differ");
    }
    if (evidence.passed &&
        (!evidence.observations_complete || evidence.exit_code != 0 ||
         evidence.signal != 0)) {
      return fail("passing evidence has an incomplete or failed outcome");
    }
    for (std::size_t index = 0; index < evidence.entries.size(); ++index) {
      const ValidationEntry &entry = evidence.entries[index];
      const ValidationObservation &observation = evidence.observations[index];
      if (entry.kind != evidence.kind || entry.state_size == 0 ||
          entry.state_alignment == 0 || entry.report_size == 0 ||
          entry.failure_offset + 8 != entry.report_size ||
          entry.report_size > entry.state_size ||
          entry.package != observation.package ||
          entry.procedure != observation.procedure) {
        return fail("entry and observation invariants do not match");
      }
      if (index != 0 && !entry_less(evidence.entries[index - 1], entry)) {
        return fail("validation entries are not in unique canonical order");
      }
    }
    if (hash_validation_evidence_key(evidence) != evidence.key) {
      return fail("validation evidence key does not match its inputs");
    }
    return true;
  }

  std::string_view input_;
  DiagnosticSink &diagnostics_;
  std::size_t position_ = 0;
  bool failed_ = false;
};

[[nodiscard]] std::uint64_t read_u64_little(
    std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint64_t result = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    result |= static_cast<std::uint64_t>(bytes[offset + index]) << (index * 8U);
  }
  return result;
}

} // namespace

Sha256Digest hash_validation_evidence_key(
    const ValidationEvidence &evidence) {
  Sha256 hash;
  hash_field(hash, "draft.validation-evidence-key.v1");
  hash.update(evidence.resolved_program.bytes);
  hash_field(hash, validation_kind_name(evidence.kind));
  hash_field(hash, evidence.target_identity);
  hash_field(hash, evidence.compiler_identity);
  hash_field(hash, evidence.toolchain_identity);
  hash_field(hash, evidence.environment_identity);
  hash_field(hash, evidence.runner_identity);
  hash_field(hash, evidence.policy_identity);
  hash_field(hash, evidence.artifact_identity);
  hash_u64(hash, evidence.warmup_runs);
  hash_u64(hash, evidence.sample_runs);
  std::vector<ValidationEntry> entries = evidence.entries;
  std::sort(entries.begin(), entries.end(), entry_less);
  hash_u64(hash, static_cast<std::uint64_t>(entries.size()));
  for (const ValidationEntry &entry : entries) hash_entry(hash, entry);
  return hash.finalize();
}

std::string serialize_validation_evidence(
    const ValidationEvidence &evidence) {
  std::string output;
  output += "{\n  \"format\": ";
  append_json_string(evidence.format, output);
  output += ",\n  \"key\": ";
  append_json_string(evidence.key.hex(), output);
  output += ",\n  \"attempt\": " + std::to_string(evidence.attempt);
  output += ",\n  \"resolved_program\": ";
  append_json_string(evidence.resolved_program.hex(), output);
  output += ",\n  \"kind\": ";
  append_json_string(validation_kind_name(evidence.kind), output);
  output += ",\n  \"target\": ";
  append_json_string(evidence.target_identity, output);
  output += ",\n  \"compiler\": ";
  append_json_string(evidence.compiler_identity, output);
  output += ",\n  \"toolchain\": ";
  append_json_string(evidence.toolchain_identity, output);
  output += ",\n  \"environment\": ";
  append_json_string(evidence.environment_identity, output);
  output += ",\n  \"runner\": ";
  append_json_string(evidence.runner_identity, output);
  output += ",\n  \"policy\": ";
  append_json_string(evidence.policy_identity, output);
  output += ",\n  \"artifact\": ";
  append_json_string(evidence.artifact_identity, output);
  output += ",\n  \"warmup_runs\": " + std::to_string(evidence.warmup_runs);
  output += ",\n  \"sample_runs\": " + std::to_string(evidence.sample_runs);
  output += ",\n  \"entries\": [";
  for (std::size_t index = 0; index < evidence.entries.size(); ++index) {
    const ValidationEntry &entry = evidence.entries[index];
    output += index == 0 ? "\n    {\"kind\": " : ",\n    {\"kind\": ";
    append_json_string(validation_kind_name(entry.kind), output);
    output += ", \"root\": ";
    append_json_string(entry.package.root_identity, output);
    output += ", \"package\": ";
    append_json_string(entry.package.root_relative_path, output);
    output += ", \"procedure\": ";
    append_json_string(entry.procedure, output);
    output += ", \"state_size\": " + std::to_string(entry.state_size);
    output += ", \"state_alignment\": " +
        std::to_string(entry.state_alignment);
    output += ", \"failure_offset\": " +
        std::to_string(entry.failure_offset);
    output += ", \"report_size\": " + std::to_string(entry.report_size) + "}";
  }
  output += evidence.entries.empty() ? "]," : "\n  ],";
  output += "\n  \"observations\": [";
  for (std::size_t index = 0; index < evidence.observations.size(); ++index) {
    const ValidationObservation &observation = evidence.observations[index];
    output += index == 0 ? "\n    {\"root\": " : ",\n    {\"root\": ";
    append_json_string(observation.package.root_identity, output);
    output += ", \"package\": ";
    append_json_string(observation.package.root_relative_path, output);
    output += ", \"procedure\": ";
    append_json_string(observation.procedure, output);
    output += ", \"checks\": " + std::to_string(observation.checks);
    output += ", \"library_samples\": " +
        std::to_string(observation.library_samples);
    output += ", \"failures\": " + std::to_string(observation.failures);
    output += ", \"maximum_time_ns\": " +
        std::to_string(observation.maximum_time_ns);
    output += ", \"durations_ns\": [";
    for (std::size_t sample = 0;
         sample < observation.durations_ns.size(); ++sample) {
      if (sample != 0) output += ", ";
      output += std::to_string(observation.durations_ns[sample]);
    }
    output += "]}";
  }
  output += evidence.observations.empty() ? "]," : "\n  ],";
  output += "\n  \"observations_complete\": ";
  output += evidence.observations_complete ? "true," : "false,";
  output += "\n  \"verdict\": ";
  append_json_string(evidence.passed ? "pass" : "fail", output);
  output += ",\n  \"exit_code\": " + std::to_string(evidence.exit_code);
  output += ",\n  \"signal\": " + std::to_string(evidence.signal);
  output += "\n}\n";
  return output;
}

bool parse_validation_evidence(
    std::string_view json,
    ValidationEvidence &evidence,
    DiagnosticSink &diagnostics) {
  return EvidenceParser(json, diagnostics).parse(evidence);
}

bool decode_validation_report(
    std::span<const std::uint8_t> report,
    const std::vector<ValidationEntry> &entries,
    std::vector<ValidationObservation> &observations,
    DiagnosticSink &diagnostics) {
  std::uint64_t expected_size = 0;
  for (const ValidationEntry &entry : entries) {
    if (entry.report_size >
        std::numeric_limits<std::uint64_t>::max() - expected_size) {
      diagnostics.error(
          SourceRange::invalid(), "validation report size overflows");
      return false;
    }
    expected_size += entry.report_size;
  }
  if (expected_size != static_cast<std::uint64_t>(report.size())) {
    diagnostics.error(
        SourceRange::invalid(),
        "validation harness produced " + std::to_string(report.size()) +
            " report bytes; expected " + std::to_string(expected_size));
    return false;
  }

  observations.clear();
  std::size_t offset = 0;
  for (const ValidationEntry &entry : entries) {
    ValidationObservation observation;
    observation.package = entry.package;
    observation.procedure = entry.procedure;
    if (entry.kind == ValidationKind::Test && entry.report_size == 16) {
      observation.checks = read_u64_little(report, offset);
      observation.failures = read_u64_little(report, offset + 8);
    } else if (entry.kind == ValidationKind::Benchmark &&
               entry.report_size == 32) {
      observation.maximum_time_ns = std::bit_cast<std::int64_t>(
          read_u64_little(report, offset));
      observation.durations_ns.push_back(std::bit_cast<std::int64_t>(
          read_u64_little(report, offset + 8)));
      observation.library_samples = read_u64_little(report, offset + 16);
      observation.failures = read_u64_little(report, offset + 24);
    } else {
      diagnostics.error(
          SourceRange::invalid(),
          "validation entry has an unsupported report layout");
      return false;
    }
    observations.push_back(std::move(observation));
    offset += static_cast<std::size_t>(entry.report_size);
  }
  return true;
}

} // namespace draft
