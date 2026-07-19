// Deterministic JSON encoding and strict parsing for resolution manifests.

#include "elaborator/resolution.h"

#include "elaborator/obligation.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace draft {
namespace {

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

[[nodiscard]] std::optional<AgentConstructKind> parse_kind(
    std::string_view value) {
  if (value == "judgment") return AgentConstructKind::Judgment;
  if (value == "declaration") return AgentConstructKind::SynthesisDeclaration;
  if (value == "member") return AgentConstructKind::SynthesisMember;
  if (value == "statement") return AgentConstructKind::SynthesisStatement;
  if (value == "expression") return AgentConstructKind::SynthesisExpression;
  if (value == "assembly") return AgentConstructKind::SynthesisAssembly;
  return std::nullopt;
}

[[nodiscard]] std::optional<ExternalInputKind> parse_external_kind(
    std::string_view value) {
  if (value == "foreign-artifact") return ExternalInputKind::ForeignArtifact;
  if (value == "object") return ExternalInputKind::Object;
  if (value == "archive") return ExternalInputKind::Archive;
  if (value == "shared-library") return ExternalInputKind::SharedLibrary;
  if (value == "runtime-asset") return ExternalInputKind::RuntimeAsset;
  if (value == "provider-summary") return ExternalInputKind::ProviderSummary;
  return std::nullopt;
}

[[nodiscard]] bool valid_evidence_kind(std::string_view value) {
  return value == "test" || value == "benchmark" || value == "judgment";
}

[[nodiscard]] std::uint32_t evidence_kind_rank(std::string_view value) {
  if (value == "test") return 0;
  if (value == "benchmark") return 1;
  if (value == "judgment") return 2;
  return 3;
}

[[nodiscard]] bool evidence_less(
    const ResolutionEvidencePin &left,
    const ResolutionEvidencePin &right) {
  if (left.kind != right.kind) {
    return evidence_kind_rank(left.kind) < evidence_kind_rank(right.kind);
  }
  if (left.root_identity != right.root_identity) {
    return left.root_identity < right.root_identity;
  }
  if (left.root_relative_path != right.root_relative_path) {
    return left.root_relative_path < right.root_relative_path;
  }
  return left.key.bytes < right.key.bytes;
}

// Manifest paths are semantic paths inside a content tree. Host separators,
// absolute spellings, empty components, and traversal would make the same row
// select different bytes on different machines and are therefore rejected.
[[nodiscard]] bool valid_relative_entry(std::string_view value) {
  if (value.empty()) return true;
  if (value.front() == '/' || value.back() == '/' ||
      value.find('\\') != std::string_view::npos) {
    return false;
  }
  std::size_t begin = 0;
  while (begin < value.size()) {
    const std::size_t slash = value.find('/', begin);
    const std::size_t end = slash == std::string_view::npos
        ? value.size()
        : slash;
    const std::string_view component = value.substr(begin, end - begin);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    begin = end + 1;
  }
  return true;
}

class ManifestParser {
public:
  ManifestParser(std::string_view input, DiagnosticSink &diagnostics)
      : input_(input), diagnostics_(diagnostics) {}

  [[nodiscard]] bool parse(ResolutionManifest &manifest) {
    ResolutionManifest parsed;
    if (!punctuation('{') || !key("format") ||
        !string(parsed.format) || !comma() ||
        !key("target") || !string(parsed.target_identity) || !comma() ||
        !key("resolved_program") ||
        !digest(parsed.resolved_program_digest) || !comma() ||
        !key("external_inputs") ||
        !external_inputs(parsed.external_inputs) || !comma() ||
        !key("evidence") || !evidence_pins(parsed.evidence) || !comma() ||
        !key("sites") || !pins(parsed.pins) || !punctuation('}')) {
      return false;
    }
    whitespace();
    if (position_ != input_.size()) return fail("trailing bytes after manifest");
    if (parsed.format != "draft-resolution-v4") {
      return fail("unsupported resolution manifest format");
    }
    if (parsed.target_identity.empty()) {
      return fail("resolution manifest target identity must not be empty");
    }
    std::sort(
        parsed.external_inputs.begin(), parsed.external_inputs.end(),
        [](const ExternalInputPin &left, const ExternalInputPin &right) {
          if (left.kind != right.kind) {
            return static_cast<std::uint32_t>(left.kind) <
                static_cast<std::uint32_t>(right.kind);
          }
          return left.name < right.name;
        });
    for (std::size_t index = 1; index < parsed.external_inputs.size(); ++index) {
      const ExternalInputPin &previous = parsed.external_inputs[index - 1];
      const ExternalInputPin &current = parsed.external_inputs[index];
      if (previous.kind == current.kind && previous.name == current.name) {
        return fail("resolution manifest contains a duplicate external input");
      }
    }
    std::sort(parsed.evidence.begin(), parsed.evidence.end(), evidence_less);
    for (std::size_t index = 1; index < parsed.evidence.size(); ++index) {
      const ResolutionEvidencePin &previous = parsed.evidence[index - 1];
      const ResolutionEvidencePin &current = parsed.evidence[index];
      if (previous.kind == current.kind &&
          previous.root_identity == current.root_identity &&
          previous.root_relative_path == current.root_relative_path &&
          previous.key == current.key) {
        return fail("resolution manifest contains duplicate validation evidence");
      }
    }
    std::sort(
        parsed.pins.begin(), parsed.pins.end(),
        [](const ResolutionPin &left, const ResolutionPin &right) {
          return left.site_identity < right.site_identity;
        });
    for (std::size_t index = 1; index < parsed.pins.size(); ++index) {
      if (parsed.pins[index - 1].site_identity ==
          parsed.pins[index].site_identity) {
        return fail("resolution manifest contains a duplicate site identity");
      }
    }
    manifest = std::move(parsed);
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
          "invalid resolution manifest at byte " +
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
    if (actual != expected) return fail("expected field '" + std::string(expected) + "'");
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
    const unsigned char byte = static_cast<unsigned char>(
        input_[position_ + offset]);
    return byte >= minimum && byte <= maximum;
  }

  // JSON text is Unicode. Canonical manifests write non-ASCII scalars as raw
  // UTF-8, so accepting arbitrary bytes here would let malformed identities
  // enter a manifest even though no conforming JSON reader could reproduce
  // them. These lead-byte ranges reject continuations, overlong encodings,
  // surrogate scalars, and values above U+10FFFF without a general decoder.
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
      const char value = input_[position_++];
      if (value == '"') return true;
      if (static_cast<unsigned char>(value) < 0x20U) {
        return fail("unescaped control byte in string");
      }
      if (static_cast<unsigned char>(value) >= 0x80U) {
        if (!utf8_sequence(static_cast<unsigned char>(value), result)) {
          return false;
        }
        continue;
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
        if (position_ + 4 > input_.size() || input_.substr(position_, 2) != "00") {
          return fail("only byte-sized JSON unicode escapes are canonical");
        }
        const std::optional<unsigned char> high = hex(input_[position_ + 2]);
        const std::optional<unsigned char> low = hex(input_[position_ + 3]);
        if (!high.has_value() || !low.has_value()) {
          return fail("invalid JSON unicode escape");
        }
        const unsigned char byte = static_cast<unsigned char>((*high << 4U) | *low);
        if (byte >= 0x20U) return fail("non-control byte uses noncanonical unicode escape");
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
    const std::optional<Sha256Digest> parsed = Sha256Digest::from_hex(encoded);
    if (!parsed.has_value()) return fail("digest must contain 64 hexadecimal digits");
    result = *parsed;
    return true;
  }

  // Manifest offsets are canonical unsigned JSON integers: zero is written as
  // `0`, other values have no leading zero, and overflow is rejected before a
  // host arithmetic operation can wrap.
  [[nodiscard]] bool unsigned_integer(std::uint64_t &result) {
    whitespace();
    if (position_ >= input_.size() || input_[position_] < '0' ||
        input_[position_] > '9') {
      return fail("expected unsigned integer");
    }
    if (input_[position_] == '0' && position_ + 1 < input_.size() &&
        input_[position_ + 1] >= '0' && input_[position_ + 1] <= '9') {
      return fail("unsigned integer has a noncanonical leading zero");
    }
    result = 0;
    while (position_ < input_.size() && input_[position_] >= '0' &&
           input_[position_] <= '9') {
      const std::uint64_t digit =
          static_cast<std::uint64_t>(input_[position_] - '0');
      if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
        return fail("unsigned integer is out of range");
      }
      result = result * 10U + digit;
      ++position_;
    }
    return true;
  }

  [[nodiscard]] bool pin(ResolutionPin &pin) {
    std::string kind;
    if (!punctuation('{') || !key("site") || !string(pin.site_identity) ||
        !comma() || !key("kind") || !string(kind) || !comma() ||
        !key("input") || !digest(pin.input_digest) || !comma() ||
        !key("expansion") || !digest(pin.expansion_digest) || !comma() ||
        !key("source_root") ||
        !string(pin.source_map.root_identity) || !comma() ||
        !key("source_package") ||
        !string(pin.source_map.root_relative_path) || !comma() ||
        !key("source_file") ||
        !string(pin.source_map.source_relative_path) || !comma() ||
        !key("surface_begin") ||
        !unsigned_integer(pin.source_map.surface_begin) || !comma() ||
        !key("surface_end") ||
        !unsigned_integer(pin.source_map.surface_end) || !comma() ||
        !key("expansion_bytes") ||
        !unsigned_integer(pin.source_map.expansion_bytes) || !comma() ||
        !key("provider") || !string(pin.provider_identity) || !comma() ||
        !key("model") || !string(pin.model_identity) || !comma() ||
        !key("configuration") || !string(pin.configuration_identity) ||
        !punctuation('}')) {
      return false;
    }
    const std::optional<AgentConstructKind> parsed_kind = parse_kind(kind);
    if (!parsed_kind.has_value() ||
        *parsed_kind == AgentConstructKind::Judgment) {
      return fail("resolution pin has an invalid synthesis kind");
    }
    pin.kind = *parsed_kind;
    if (!pin.site_identity.starts_with("site-") ||
        !Sha256Digest::from_hex(std::string_view(pin.site_identity).substr(5))
             .has_value()) {
      return fail("resolution pin has an invalid site identity");
    }
    if (pin.provider_identity.empty() || pin.model_identity.empty() ||
        pin.configuration_identity.empty()) {
      return fail(
          "resolution pin provider, model, and configuration identities "
          "must not be empty");
    }
    if (pin.source_map.root_identity.empty() ||
        pin.source_map.root_relative_path.empty() ||
        pin.source_map.source_relative_path.empty() ||
        pin.source_map.surface_begin > pin.source_map.surface_end ||
        !valid_relative_entry(pin.source_map.source_relative_path) ||
        (pin.source_map.root_relative_path != "." &&
         !valid_relative_entry(pin.source_map.root_relative_path))) {
      return fail("resolution pin has an invalid generated-source map");
    }
    return true;
  }

  [[nodiscard]] bool external_input(ExternalInputPin &input) {
    std::string kind;
    if (!punctuation('{') || !key("kind") || !string(kind) || !comma() ||
        !key("name") || !string(input.name) || !comma() ||
        !key("content") || !digest(input.content_digest) || !comma() ||
        !key("entry") || !string(input.entry_point) || !punctuation('}')) {
      return false;
    }
    const std::optional<ExternalInputKind> parsed_kind =
        parse_external_kind(kind);
    if (!parsed_kind.has_value()) {
      return fail("resolution manifest has an invalid external input kind");
    }
    input.kind = *parsed_kind;
    if (input.name.empty()) {
      return fail("resolution manifest external input name must not be empty");
    }
    if (!valid_relative_entry(input.entry_point)) {
      return fail("resolution manifest external entry path is not canonical");
    }
    return true;
  }

  [[nodiscard]] bool external_inputs(std::vector<ExternalInputPin> &result) {
    if (!punctuation('[')) return false;
    whitespace();
    if (position_ < input_.size() && input_[position_] == ']') {
      ++position_;
      return true;
    }
    while (true) {
      ExternalInputPin value;
      if (!external_input(value)) return false;
      result.push_back(std::move(value));
      whitespace();
      if (position_ < input_.size() && input_[position_] == ']') {
        ++position_;
        return true;
      }
      if (!comma()) return false;
    }
  }

  [[nodiscard]] bool evidence_pin(ResolutionEvidencePin &evidence) {
    if (!punctuation('{') || !key("kind") || !string(evidence.kind) ||
        !comma() || !key("root") || !string(evidence.root_identity) ||
        !comma() || !key("package") ||
        !string(evidence.root_relative_path) || !comma() ||
        !key("key") || !digest(evidence.key) || !comma() ||
        !key("content") || !digest(evidence.content_digest) ||
        !punctuation('}')) {
      return false;
    }
    if (!valid_evidence_kind(evidence.kind)) {
      return fail("resolution manifest has an invalid evidence kind");
    }
    if (evidence.root_identity.empty() ||
        evidence.root_relative_path.empty() ||
        (evidence.root_relative_path != "." &&
         !valid_relative_entry(evidence.root_relative_path))) {
      return fail("resolution manifest evidence package is not canonical");
    }
    return true;
  }

  [[nodiscard]] bool evidence_pins(
      std::vector<ResolutionEvidencePin> &result) {
    if (!punctuation('[')) return false;
    whitespace();
    if (position_ < input_.size() && input_[position_] == ']') {
      ++position_;
      return true;
    }
    while (true) {
      ResolutionEvidencePin value;
      if (!evidence_pin(value)) return false;
      result.push_back(std::move(value));
      whitespace();
      if (position_ < input_.size() && input_[position_] == ']') {
        ++position_;
        return true;
      }
      if (!comma()) return false;
    }
  }

  [[nodiscard]] bool pins(std::vector<ResolutionPin> &result) {
    if (!punctuation('[')) return false;
    whitespace();
    if (position_ < input_.size() && input_[position_] == ']') {
      ++position_;
      return true;
    }
    while (true) {
      ResolutionPin value;
      if (!pin(value)) return false;
      result.push_back(std::move(value));
      whitespace();
      if (position_ < input_.size() && input_[position_] == ']') {
        ++position_;
        return true;
      }
      if (!comma()) return false;
    }
  }

  std::string_view input_;
  DiagnosticSink &diagnostics_;
  std::size_t position_ = 0;
  bool failed_ = false;
};

} // namespace

std::string_view external_input_kind_name(ExternalInputKind kind) {
  switch (kind) {
  case ExternalInputKind::ForeignArtifact: return "foreign-artifact";
  case ExternalInputKind::Object: return "object";
  case ExternalInputKind::Archive: return "archive";
  case ExternalInputKind::SharedLibrary: return "shared-library";
  case ExternalInputKind::RuntimeAsset: return "runtime-asset";
  case ExternalInputKind::ProviderSummary: return "provider-summary";
  }
  return "invalid";
}

std::string serialize_resolution_manifest(const ResolutionManifest &manifest) {
  std::vector<ResolutionPin> pins = manifest.pins;
  std::sort(
      pins.begin(), pins.end(),
      [](const ResolutionPin &left, const ResolutionPin &right) {
        return left.site_identity < right.site_identity;
      });
  std::vector<ExternalInputPin> external_inputs = manifest.external_inputs;
  std::sort(
      external_inputs.begin(), external_inputs.end(),
      [](const ExternalInputPin &left, const ExternalInputPin &right) {
        if (left.kind != right.kind) {
          return static_cast<std::uint32_t>(left.kind) <
              static_cast<std::uint32_t>(right.kind);
        }
        return left.name < right.name;
      });
  std::vector<ResolutionEvidencePin> evidence = manifest.evidence;
  std::sort(evidence.begin(), evidence.end(), evidence_less);
  std::string output = "{\n  \"format\": ";
  append_json_string(manifest.format, output);
  output += ",\n  \"target\": ";
  append_json_string(manifest.target_identity, output);
  output += ",\n  \"resolved_program\": ";
  append_json_string(manifest.resolved_program_digest.hex(), output);
  output += ",\n  \"external_inputs\": [";
  for (std::size_t index = 0; index < external_inputs.size(); ++index) {
    const ExternalInputPin &input = external_inputs[index];
    output += index == 0 ? "\n" : ",\n";
    output += "    {\"kind\": ";
    append_json_string(external_input_kind_name(input.kind), output);
    output += ", \"name\": ";
    append_json_string(input.name, output);
    output += ", \"content\": ";
    append_json_string(input.content_digest.hex(), output);
    output += ", \"entry\": ";
    append_json_string(input.entry_point, output);
    output += '}';
  }
  if (!external_inputs.empty()) output += '\n';
  output += "  ],\n  \"evidence\": [";
  for (std::size_t index = 0; index < evidence.size(); ++index) {
    const ResolutionEvidencePin &pin = evidence[index];
    output += index == 0 ? "\n" : ",\n";
    output += "    {\"kind\": ";
    append_json_string(pin.kind, output);
    output += ", \"root\": ";
    append_json_string(pin.root_identity, output);
    output += ", \"package\": ";
    append_json_string(pin.root_relative_path, output);
    output += ", \"key\": ";
    append_json_string(pin.key.hex(), output);
    output += ", \"content\": ";
    append_json_string(pin.content_digest.hex(), output);
    output += '}';
  }
  if (!evidence.empty()) output += '\n';
  output += "  ],\n  \"sites\": [";
  for (std::size_t index = 0; index < pins.size(); ++index) {
    const ResolutionPin &pin = pins[index];
    output += index == 0 ? "\n" : ",\n";
    output += "    {\"site\": ";
    append_json_string(pin.site_identity, output);
    output += ", \"kind\": ";
    append_json_string(agent_construct_kind_name(pin.kind), output);
    output += ", \"input\": ";
    append_json_string(pin.input_digest.hex(), output);
    output += ", \"expansion\": ";
    append_json_string(pin.expansion_digest.hex(), output);
    output += ", \"source_root\": ";
    append_json_string(pin.source_map.root_identity, output);
    output += ", \"source_package\": ";
    append_json_string(pin.source_map.root_relative_path, output);
    output += ", \"source_file\": ";
    append_json_string(pin.source_map.source_relative_path, output);
    output += ", \"surface_begin\": " +
        std::to_string(pin.source_map.surface_begin);
    output += ", \"surface_end\": " +
        std::to_string(pin.source_map.surface_end);
    output += ", \"expansion_bytes\": " +
        std::to_string(pin.source_map.expansion_bytes);
    output += ", \"provider\": ";
    append_json_string(pin.provider_identity, output);
    output += ", \"model\": ";
    append_json_string(pin.model_identity, output);
    output += ", \"configuration\": ";
    append_json_string(pin.configuration_identity, output);
    output += '}';
  }
  if (!pins.empty()) output += '\n';
  output += "  ]\n}\n";
  return output;
}

bool parse_resolution_manifest(
    std::string_view json,
    ResolutionManifest &manifest,
    DiagnosticSink &diagnostics) {
  ManifestParser parser(json, diagnostics);
  return parser.parse(manifest);
}

} // namespace draft
