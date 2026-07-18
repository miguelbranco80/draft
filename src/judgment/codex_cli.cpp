// Judgment-specific Codex prompt construction and response interpretation.
//
// The compiler supplies a closed, typed request. Codex sees no workspace path
// and receives artifact bytes only under compiler-generated private filenames.
// A response can say pass or fail and explain why; it cannot change the claim,
// typed context, artifact identity, provider identity, or evidence state.

#include "judgment/codex_cli.h"

#include "base/sha256.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace draft {
namespace {

constexpr std::string_view kPromptContractIdentity =
    "draft-codex-judgment-prompt-v3";
constexpr std::string_view kOutputSchema =
    "{\n"
    "  \"type\": \"object\",\n"
    "  \"properties\": {\n"
    "    \"verdict\": {\"type\": \"string\", \"enum\": [\"pass\", \"fail\"]},\n"
    "    \"rationale\": {\"type\": \"string\", \"minLength\": 1}\n"
    "  },\n"
    "  \"required\": [\"verdict\", \"rationale\"],\n"
    "  \"additionalProperties\": false\n"
    "}\n";

void provider_error(DiagnosticSink &diagnostics, std::string message) {
  diagnostics.error(SourceRange::invalid(), std::move(message));
}

// The common request transcript uses byte-counted fields. Repeating this tiny
// encoder here keeps judgment policy out of the generic process runtime while
// preserving one unambiguous transcript format for arbitrary authored text.
void append_field(
    std::string_view name,
    std::string_view value,
    std::string &output) {
  output += name;
  output.push_back(' ');
  output += std::to_string(value.size());
  output.push_back('\n');
  output.append(value);
  output.push_back('\n');
}

[[nodiscard]] std::string padded_index(std::size_t index) {
  std::string digits = std::to_string(index);
  if (digits.size() < 8) digits.insert(0, 8 - digits.size(), '0');
  return digits;
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

// The CLI validates the JSON Schema, but the adapter still parses the exact
// bytes defensively. Object member order is deliberately irrelevant; duplicate
// or unknown members, invalid UTF-8, malformed escapes, and trailing bytes are
// rejected. This is the complete provider-controlled input surface.
class JudgmentResponseParser {
public:
  explicit JudgmentResponseParser(std::string_view input) : input_(input) {}

  [[nodiscard]] bool parse(JudgmentResponse &response) {
    if (!take('{')) return false;
    bool have_verdict = false;
    bool have_rationale = false;
    for (std::size_t member = 0; member < 2; ++member) {
      if (member != 0 && !take(',')) return false;
      std::string key;
      std::string value;
      if (!json_string(key) || !take(':') || !json_string(value)) return false;
      if (key == "verdict" && !have_verdict) {
        if (value == "pass") {
          response.passed = true;
        } else if (value == "fail") {
          response.passed = false;
        } else {
          return false;
        }
        have_verdict = true;
      } else if (key == "rationale" && !have_rationale) {
        if (value.empty()) return false;
        response.rationale = std::move(value);
        have_rationale = true;
      } else {
        return false;
      }
    }
    if (!take('}')) return false;
    whitespace();
    return have_verdict && have_rationale && position_ == input_.size();
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
    if (position_ >= input_.size() || input_[position_] != expected) {
      return false;
    }
    ++position_;
    return true;
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

  [[nodiscard]] bool utf8_sequence(
      unsigned char lead,
      std::string &output) {
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
      valid = continuation_byte(0, 0x80U, 0x8fU) &&
          continuation_byte(1) && continuation_byte(2);
    }
    if (!valid) return false;
    output.push_back(static_cast<char>(lead));
    output.append(input_.substr(position_, continuation_count));
    position_ += continuation_count;
    return true;
  }

  [[nodiscard]] bool unicode_escape(std::uint32_t &scalar) {
    if (position_ + 4 > input_.size()) return false;
    scalar = 0;
    for (std::size_t index = 0; index < 4; ++index) {
      const std::optional<unsigned char> digit =
          hex_digit(input_[position_++]);
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
      const unsigned char byte =
          static_cast<unsigned char>(input_[position_++]);
      if (byte == '"') return true;
      if (byte < 0x20U) return false;
      if (byte >= 0x80U) {
        if (!utf8_sequence(byte, output)) return false;
        continue;
      }
      if (byte != '\\') {
        output.push_back(static_cast<char>(byte));
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
};

[[nodiscard]] bool prepare_judgment_request(
    const JudgmentRequest &request,
    std::vector<CodexCliInputFile> &files,
    std::string &prompt,
    DiagnosticSink &diagnostics) {
  std::vector<CodexAgentRequestFile> attachments;
  attachments.reserve(request.attachments.size());
  for (const JudgmentRequestFile &attachment : request.attachments) {
    attachments.push_back({
        attachment.relative_path,
        attachment.size,
        attachment.digest,
        attachment.contents,
    });
  }

  constexpr std::string_view instruction =
      "You are the Draft language judgment provider. Evaluate exactly the "
      "authored claim against the supplied typed context and requested "
      "artifacts. Return only a JSON object with verdict (pass or fail) and a "
      "nonempty rationale. Do not synthesize source. Do not edit files. Do not "
      "inspect paths outside this isolated request directory.";
  if (!prepare_codex_agent_request(
          instruction,
          request.format,
          request.obligation,
          "JUDGMENT_CLAIM",
          request.claim,
          attachments,
          files,
          prompt,
          diagnostics)) {
    return false;
  }

  append_field(
      "RESOLVED_PROGRAM_SHA256", request.resolved_program.hex(), prompt);
  append_field("COMPILER_IDENTITY", request.compiler_identity, prompt);
  append_field("POLICY_IDENTITY", request.policy_identity, prompt);
  append_field("VALIDATOR_IDENTITY", request.validator_identity, prompt);
  prompt += "REQUESTED_ARTIFACTS " +
      std::to_string(request.artifacts.size()) + "\n";

  std::vector<std::string> artifact_kinds;
  artifact_kinds.reserve(request.artifacts.size());
  for (std::size_t index = 0; index < request.artifacts.size(); ++index) {
    const JudgmentRequestArtifact &artifact = request.artifacts[index];
    if (artifact.kind.empty() || artifact.digest != sha256(artifact.contents) ||
        std::find(
            artifact_kinds.begin(), artifact_kinds.end(), artifact.kind) !=
            artifact_kinds.end()) {
      provider_error(
          diagnostics,
          "Codex judgment artifacts require unique nonempty kinds and exact identities");
      return false;
    }
    artifact_kinds.push_back(artifact.kind);
    const std::string name =
        "requested-artifact-" + padded_index(index) + ".bin";
    files.push_back({name, artifact.contents});
    append_field("ARTIFACT_KIND", artifact.kind, prompt);
    append_field("ARTIFACT_FILE", name, prompt);
    append_field("ARTIFACT_SHA256", artifact.digest.hex(), prompt);
  }
  return true;
}

[[nodiscard]] bool judge_with_codex(
    void *opaque,
    const JudgmentRequest &request,
    JudgmentResponse &response,
    DiagnosticSink &diagnostics) {
  auto *state = static_cast<CodexCliProviderState *>(opaque);
  std::vector<CodexCliInputFile> files;
  std::string prompt;
  if (!prepare_judgment_request(
          request, files, prompt, diagnostics)) {
    return false;
  }

  std::string json;
  if (!invoke_codex_cli_runtime(
          *state, kOutputSchema, prompt, files, json, diagnostics)) {
    return false;
  }
  JudgmentResponse parsed;
  JudgmentResponseParser parser(json);
  if (!parser.parse(parsed)) {
    provider_error(
        diagnostics,
        "Codex final response does not match the judgment schema");
    return false;
  }
  response = std::move(parsed);
  return true;
}

} // namespace

JudgmentProvider configure_codex_cli_judgment_provider(
    const CodexCliProviderOptions &options,
    CodexCliProviderState &state,
    DiagnosticSink &diagnostics) {
  if (!configure_codex_cli_runtime(
          options,
          kPromptContractIdentity,
          kOutputSchema,
          state,
          diagnostics)) {
    return {};
  }

  JudgmentProvider provider;
  provider.provider_identity = "openai-codex-cli-v21";
  provider.model_identity = state.model;
  provider.configuration_identity = state.configuration_identity;
  provider.state = &state;
  provider.judge = judge_with_codex;
  return provider;
}

} // namespace draft
