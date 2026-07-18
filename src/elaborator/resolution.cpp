// Deterministic JSON encoding and strict parsing for resolution manifests.

#include "elaborator/resolution.h"

#include "elaborator/obligation.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
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
        !key("sites") || !pins(parsed.pins) || !punctuation('}')) {
      return false;
    }
    whitespace();
    if (position_ != input_.size()) return fail("trailing bytes after manifest");
    if (parsed.format != "draft-resolution-v1") {
      return fail("unsupported resolution manifest format");
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

  [[nodiscard]] bool pin(ResolutionPin &pin) {
    std::string kind;
    if (!punctuation('{') || !key("site") || !string(pin.site_identity) ||
        !comma() || !key("kind") || !string(kind) || !comma() ||
        !key("input") || !digest(pin.input_digest) || !comma() ||
        !key("expansion") || !digest(pin.expansion_digest) || !comma() ||
        !key("provider") || !string(pin.provider_identity) || !comma() ||
        !key("model") || !string(pin.model_identity) || !punctuation('}')) {
      return false;
    }
    const std::optional<AgentConstructKind> parsed_kind = parse_kind(kind);
    if (!parsed_kind.has_value() ||
        *parsed_kind == AgentConstructKind::Judgment) {
      return fail("resolution pin has an invalid synthesis kind");
    }
    pin.kind = *parsed_kind;
    if (!pin.site_identity.starts_with("site-") || pin.site_identity.size() != 69) {
      return fail("resolution pin has an invalid site identity");
    }
    return true;
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

std::string serialize_resolution_manifest(const ResolutionManifest &manifest) {
  std::vector<ResolutionPin> pins = manifest.pins;
  std::sort(
      pins.begin(), pins.end(),
      [](const ResolutionPin &left, const ResolutionPin &right) {
        return left.site_identity < right.site_identity;
      });
  std::string output = "{\n  \"format\": ";
  append_json_string(manifest.format, output);
  output += ",\n  \"target\": ";
  append_json_string(manifest.target_identity, output);
  output += ",\n  \"resolved_program\": ";
  append_json_string(manifest.resolved_program_digest.hex(), output);
  output += ",\n  \"sites\": [";
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
    output += ", \"provider\": ";
    append_json_string(pin.provider_identity, output);
    output += ", \"model\": ";
    append_json_string(pin.model_identity, output);
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
