// Deterministic judgment-evidence hashing and strict JSON handling.

#include "judgment/evidence.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
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
        output.push_back(character);
      }
      break;
    }
  }
  output.push_back('"');
}

[[nodiscard]] bool artifact_less(
    const JudgmentArtifactIdentity &left,
    const JudgmentArtifactIdentity &right) {
  return left.kind < right.kind;
}

[[nodiscard]] bool valid_relative_path(
    std::string_view value,
    bool allow_root_dot) {
  if (allow_root_dot && value == ".") return true;
  if (value.empty() || value.front() == '/' || value.back() == '/' ||
      value.find('\\') != std::string_view::npos) {
    return false;
  }
  std::size_t begin = 0;
  while (begin < value.size()) {
    const std::size_t slash = value.find('/', begin);
    const std::size_t end =
        slash == std::string_view::npos ? value.size() : slash;
    const std::string_view component = value.substr(begin, end - begin);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    begin = end + 1;
  }
  return true;
}

[[nodiscard]] bool valid_site_identity(std::string_view value) {
  if (!value.starts_with("site-") || value.size() != 5 + 64) return false;
  return std::all_of(
      value.begin() + 5,
      value.end(),
      [](char character) {
        return (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f');
      });
}

// The parser is intentionally schema-specific. Compiler-owned evidence has one
// exact field order, so a small direct reader is easier to audit than a general
// JSON DOM plus a second validation pass. It accepts insignificant whitespace
// but rejects every alternate semantic shape.
class EvidenceParser {
public:
  EvidenceParser(std::string_view input, DiagnosticSink &diagnostics)
      : input_(input), diagnostics_(diagnostics) {}

  [[nodiscard]] bool parse(JudgmentEvidence &evidence) {
    JudgmentEvidence parsed;
    std::string verdict;
    if (!punctuation('{') ||
        !key("format") || !string(parsed.format) || !comma() ||
        !key("key") || !digest(parsed.key) || !comma() ||
        !key("attempt") || !unsigned_integer(parsed.attempt) || !comma() ||
        !key("resolved_program") || !digest(parsed.resolved_program) || !comma() ||
        !key("target") || !string(parsed.target_identity) || !comma() ||
        !key("compiler") || !string(parsed.compiler_identity) || !comma() ||
        !key("policy") || !string(parsed.policy_identity) || !comma() ||
        !key("claim") || !claim(parsed.claim) || !comma() ||
        !key("artifacts") || !artifacts(parsed.artifacts) || !comma() ||
        !key("validators") || !validators(parsed.validators) || !comma() ||
        !key("verdict") || !string(verdict) || !punctuation('}')) {
      return false;
    }
    whitespace();
    if (position_ != input_.size()) {
      return fail("trailing bytes after judgment evidence");
    }
    if (verdict == "pass") {
      parsed.passed = true;
    } else if (verdict == "fail") {
      parsed.passed = false;
    } else {
      return fail("aggregate verdict must be 'pass' or 'fail'");
    }
    if (!validate(parsed)) return false;
    evidence = std::move(parsed);
    return true;
  }

private:
  void whitespace() {
    while (position_ < input_.size() &&
           (input_[position_] == ' ' || input_[position_] == '\n' ||
            input_[position_] == '\r' || input_[position_] == '\t')) {
      ++position_;
    }
  }

  [[nodiscard]] bool fail(std::string message) {
    if (!failed_) {
      diagnostics_.error(
          SourceRange::invalid(),
          "invalid judgment evidence at byte " +
              std::to_string(position_) + ": " + std::move(message));
      failed_ = true;
    }
    return false;
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

  [[nodiscard]] bool continuation_byte(
      std::size_t offset,
      unsigned char minimum = 0x80U,
      unsigned char maximum = 0xbfU) const {
    if (position_ + offset >= input_.size()) return false;
    const unsigned char byte =
        static_cast<unsigned char>(input_[position_ + offset]);
    return byte >= minimum && byte <= maximum;
  }

  // JSON strings are Unicode. Canonical evidence writes non-ASCII code units
  // as their original UTF-8 bytes, so malformed, overlong, surrogate, and
  // out-of-range sequences must be rejected during parsing.
  [[nodiscard]] bool utf8_sequence(
      unsigned char lead,
      std::string &result) {
    std::size_t continuation_count = 0;
    bool valid = false;
    if (lead >= 0xc2U && lead <= 0xdfU) {
      continuation_count = 1;
      valid = continuation_byte(0);
    } else if (lead == 0xe0U) {
      continuation_count = 2;
      valid = continuation_byte(0, 0xa0U, 0xbfU) && continuation_byte(1);
    } else if ((lead >= 0xe1U && lead <= 0xecU) ||
               (lead >= 0xeeU && lead <= 0xefU)) {
      continuation_count = 2;
      valid = continuation_byte(0) && continuation_byte(1);
    } else if (lead == 0xedU) {
      continuation_count = 2;
      valid = continuation_byte(0, 0x80U, 0x9fU) && continuation_byte(1);
    } else if (lead == 0xf0U) {
      continuation_count = 3;
      valid = continuation_byte(0, 0x90U, 0xbfU) && continuation_byte(1) &&
          continuation_byte(2);
    } else if (lead >= 0xf1U && lead <= 0xf3U) {
      continuation_count = 3;
      valid = continuation_byte(0) && continuation_byte(1) &&
          continuation_byte(2);
    } else if (lead == 0xf4U) {
      continuation_count = 3;
      valid = continuation_byte(0, 0x80U, 0x8fU) && continuation_byte(1) &&
          continuation_byte(2);
    }
    if (!valid) return fail("invalid UTF-8 in JSON string");
    result.push_back(static_cast<char>(lead));
    result.append(input_.substr(position_, continuation_count));
    position_ += continuation_count;
    return true;
  }

  [[nodiscard]] bool string(std::string &result) {
    whitespace();
    if (position_ >= input_.size() || input_[position_] != '"') {
      return fail("expected JSON string");
    }
    ++position_;
    result.clear();
    while (position_ < input_.size()) {
      const unsigned char byte =
          static_cast<unsigned char>(input_[position_++]);
      if (byte == '"') return true;
      if (byte < 0x20U) return fail("unescaped control byte in string");
      if (byte >= 0x80U) {
        if (!utf8_sequence(byte, result)) return false;
        continue;
      }
      if (byte != '\\') {
        result.push_back(static_cast<char>(byte));
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
        const unsigned char value =
            static_cast<unsigned char>((*high << 4U) | *low);
        if (value >= 0x20U) {
          return fail("non-control byte uses noncanonical unicode escape");
        }
        result.push_back(static_cast<char>(value));
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
    if (converted.ec != std::errc{} || converted.ptr != last) {
      return fail("unsigned integer is out of range");
    }
    return true;
  }

  [[nodiscard]] bool claim(JudgmentClaimIdentity &result) {
    return punctuation('{') &&
        key("site") && string(result.site_identity) && comma() &&
        key("root") && string(result.root_identity) && comma() &&
        key("package") && string(result.root_relative_path) && comma() &&
        key("source") && string(result.source_relative_path) && comma() &&
        key("anchor") && string(result.anchor_name) && comma() &&
        key("occurrence") && unsigned_integer(result.occurrence) && comma() &&
        key("input") && digest(result.input_digest) && comma() &&
        key("record") && digest(result.record_digest) && punctuation('}');
  }

  [[nodiscard]] bool artifact(JudgmentArtifactIdentity &result) {
    return punctuation('{') &&
        key("kind") && string(result.kind) && comma() &&
        key("digest") && digest(result.content_digest) && punctuation('}');
  }

  [[nodiscard]] bool artifacts(
      std::vector<JudgmentArtifactIdentity> &result) {
    if (!punctuation('[')) return false;
    whitespace();
    if (position_ < input_.size() && input_[position_] == ']') {
      ++position_;
      return true;
    }
    while (true) {
      JudgmentArtifactIdentity value;
      if (!artifact(value)) return false;
      result.push_back(std::move(value));
      whitespace();
      if (position_ < input_.size() && input_[position_] == ']') {
        ++position_;
        return true;
      }
      if (!comma()) return false;
    }
  }

  [[nodiscard]] bool validator(JudgmentValidatorResult &result) {
    std::string verdict;
    if (!punctuation('{') ||
        !key("identity") || !string(result.validator_identity) || !comma() ||
        !key("provider") || !string(result.provider_identity) || !comma() ||
        !key("model") || !string(result.model_identity) || !comma() ||
        !key("configuration") || !string(result.configuration_identity) ||
        !comma() || !key("verdict") || !string(verdict) || !comma() ||
        !key("rationale") || !string(result.rationale) || !punctuation('}')) {
      return false;
    }
    if (verdict == "pass") {
      result.passed = true;
      return true;
    }
    if (verdict == "fail") {
      result.passed = false;
      return true;
    }
    return fail("validator verdict must be 'pass' or 'fail'");
  }

  [[nodiscard]] bool validators(
      std::vector<JudgmentValidatorResult> &result) {
    if (!punctuation('[')) return false;
    whitespace();
    if (position_ < input_.size() && input_[position_] == ']') {
      ++position_;
      return true;
    }
    while (true) {
      JudgmentValidatorResult value;
      if (!validator(value)) return false;
      result.push_back(std::move(value));
      whitespace();
      if (position_ < input_.size() && input_[position_] == ']') {
        ++position_;
        return true;
      }
      if (!comma()) return false;
    }
  }

  [[nodiscard]] bool validate(const JudgmentEvidence &evidence) {
    if (evidence.format != "draft-judgment-evidence-v1") {
      return fail("unsupported judgment evidence format");
    }
    if (evidence.attempt == 0 || evidence.target_identity.empty() ||
        evidence.compiler_identity.empty() || evidence.policy_identity.empty() ||
        evidence.claim.root_identity.empty() ||
        !valid_site_identity(evidence.claim.site_identity) ||
        !valid_relative_path(evidence.claim.root_relative_path, true) ||
        !valid_relative_path(evidence.claim.source_relative_path, false)) {
      return fail("required identity, path, site, or attempt is invalid");
    }
    for (std::size_t index = 0; index < evidence.artifacts.size(); ++index) {
      if (evidence.artifacts[index].kind.empty() ||
          (index != 0 && !artifact_less(
              evidence.artifacts[index - 1], evidence.artifacts[index]))) {
        return fail("artifact identities are not in unique canonical order");
      }
    }
    if (evidence.validators.empty()) {
      return fail("judgment evidence contains no validator result");
    }
    bool aggregate = true;
    std::vector<std::string> validator_identities;
    for (const JudgmentValidatorResult &validator : evidence.validators) {
      if (validator.validator_identity.empty() ||
          validator.provider_identity.empty() || validator.model_identity.empty() ||
          validator.configuration_identity.empty() || validator.rationale.empty()) {
        return fail("validator identity or rationale is empty");
      }
      if (std::find(
              validator_identities.begin(),
              validator_identities.end(),
              validator.validator_identity) != validator_identities.end()) {
        return fail("judgment evidence contains duplicate validator identity");
      }
      validator_identities.push_back(validator.validator_identity);
      aggregate = aggregate && validator.passed;
    }
    if (aggregate != evidence.passed) {
      return fail("aggregate verdict does not match validator verdicts");
    }
    if (hash_judgment_evidence_key(evidence) != evidence.key) {
      return fail("judgment evidence key does not match its inputs");
    }
    return true;
  }

  std::string_view input_;
  DiagnosticSink &diagnostics_;
  std::size_t position_ = 0;
  bool failed_ = false;
};

} // namespace

Sha256Digest hash_judgment_evidence_key(
    const JudgmentEvidence &evidence) {
  Sha256 hash;
  hash_field(hash, "draft.judgment-evidence-key.v1");
  hash.update(evidence.resolved_program.bytes);
  hash_field(hash, evidence.target_identity);
  hash_field(hash, evidence.compiler_identity);
  hash_field(hash, evidence.policy_identity);
  hash_field(hash, evidence.claim.site_identity);
  hash_field(hash, evidence.claim.root_identity);
  hash_field(hash, evidence.claim.root_relative_path);
  hash_field(hash, evidence.claim.source_relative_path);
  hash_field(hash, evidence.claim.anchor_name);
  hash_u64(hash, evidence.claim.occurrence);
  hash.update(evidence.claim.input_digest.bytes);
  hash.update(evidence.claim.record_digest.bytes);

  std::vector<JudgmentArtifactIdentity> artifacts = evidence.artifacts;
  std::sort(artifacts.begin(), artifacts.end(), artifact_less);
  hash_u64(hash, static_cast<std::uint64_t>(artifacts.size()));
  for (const JudgmentArtifactIdentity &artifact : artifacts) {
    hash_field(hash, artifact.kind);
    hash.update(artifact.content_digest.bytes);
  }

  hash_u64(hash, static_cast<std::uint64_t>(evidence.validators.size()));
  for (const JudgmentValidatorResult &validator : evidence.validators) {
    hash_field(hash, validator.validator_identity);
    hash_field(hash, validator.provider_identity);
    hash_field(hash, validator.model_identity);
    hash_field(hash, validator.configuration_identity);
  }
  return hash.finalize();
}

std::string serialize_judgment_evidence(
    const JudgmentEvidence &evidence) {
  std::vector<JudgmentArtifactIdentity> artifacts = evidence.artifacts;
  std::sort(artifacts.begin(), artifacts.end(), artifact_less);

  std::string output;
  output += "{\n  \"format\": ";
  append_json_string(evidence.format, output);
  output += ",\n  \"key\": ";
  append_json_string(evidence.key.hex(), output);
  output += ",\n  \"attempt\": " + std::to_string(evidence.attempt);
  output += ",\n  \"resolved_program\": ";
  append_json_string(evidence.resolved_program.hex(), output);
  output += ",\n  \"target\": ";
  append_json_string(evidence.target_identity, output);
  output += ",\n  \"compiler\": ";
  append_json_string(evidence.compiler_identity, output);
  output += ",\n  \"policy\": ";
  append_json_string(evidence.policy_identity, output);
  output += ",\n  \"claim\": {\"site\": ";
  append_json_string(evidence.claim.site_identity, output);
  output += ", \"root\": ";
  append_json_string(evidence.claim.root_identity, output);
  output += ", \"package\": ";
  append_json_string(evidence.claim.root_relative_path, output);
  output += ", \"source\": ";
  append_json_string(evidence.claim.source_relative_path, output);
  output += ", \"anchor\": ";
  append_json_string(evidence.claim.anchor_name, output);
  output += ", \"occurrence\": " + std::to_string(evidence.claim.occurrence);
  output += ", \"input\": ";
  append_json_string(evidence.claim.input_digest.hex(), output);
  output += ", \"record\": ";
  append_json_string(evidence.claim.record_digest.hex(), output);
  output += "},\n  \"artifacts\": [";
  for (std::size_t index = 0; index < artifacts.size(); ++index) {
    output += index == 0 ? "\n    {\"kind\": " : ",\n    {\"kind\": ";
    append_json_string(artifacts[index].kind, output);
    output += ", \"digest\": ";
    append_json_string(artifacts[index].content_digest.hex(), output);
    output += '}';
  }
  output += artifacts.empty() ? "]," : "\n  ],";
  output += "\n  \"validators\": [";
  for (std::size_t index = 0; index < evidence.validators.size(); ++index) {
    const JudgmentValidatorResult &validator = evidence.validators[index];
    output += index == 0 ? "\n    {\"identity\": " :
        ",\n    {\"identity\": ";
    append_json_string(validator.validator_identity, output);
    output += ", \"provider\": ";
    append_json_string(validator.provider_identity, output);
    output += ", \"model\": ";
    append_json_string(validator.model_identity, output);
    output += ", \"configuration\": ";
    append_json_string(validator.configuration_identity, output);
    output += ", \"verdict\": ";
    append_json_string(validator.passed ? "pass" : "fail", output);
    output += ", \"rationale\": ";
    append_json_string(validator.rationale, output);
    output += '}';
  }
  output += evidence.validators.empty() ? "]," : "\n  ],";
  output += "\n  \"verdict\": ";
  append_json_string(evidence.passed ? "pass" : "fail", output);
  output += "\n}\n";
  return output;
}

bool parse_judgment_evidence(
    std::string_view json,
    JudgmentEvidence &evidence,
    DiagnosticSink &diagnostics) {
  return EvidenceParser(json, diagnostics).parse(evidence);
}

} // namespace draft
