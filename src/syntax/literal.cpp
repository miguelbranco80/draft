// Shared Draft string- and rune-literal decoding implementation.
//
// Input is one complete token spelling. Output owns decoded string bytes or is
// one scalar integer; malformed input returns nullopt and never diagnoses here,
// because the caller owns the appropriate source range and phase policy. UTF-8
// validation is intentionally repeated at this small boundary so non-lexer
// callers cannot manufacture surrogate or out-of-range rune values.
//
// Relevant specification: docs/specification/01-core-language.md, "Source text and literals".

#include "syntax/literal.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace draft {
namespace {

[[nodiscard]] std::uint8_t hex_digit(char character) {
  if (character >= '0' && character <= '9') {
    return static_cast<std::uint8_t>(character - '0');
  }
  if (character >= 'a' && character <= 'f') {
    return static_cast<std::uint8_t>(character - 'a') + 10U;
  }
  if (character >= 'A' && character <= 'F') {
    return static_cast<std::uint8_t>(character - 'A') + 10U;
  }
  return 0xffU;
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

// Decodes one source UTF-8 scalar and returns its byte width. The checks mirror
// the lexer instead of trusting it because this module's public functions are
// also useful to tests and tools that may pass standalone spellings.
[[nodiscard]] bool decode_utf8_scalar(
    std::string_view text,
    std::size_t offset,
    std::uint32_t &scalar,
    std::size_t &width) {
  if (offset >= text.size()) return false;
  const std::uint8_t first = static_cast<std::uint8_t>(text[offset]);
  if (first < 0x80U) {
    scalar = first;
    width = 1;
    return true;
  }

  std::uint32_t minimum = 0;
  if (first >= 0xc2U && first <= 0xdfU) {
    scalar = first & 0x1fU;
    width = 2;
    minimum = 0x80U;
  } else if (first >= 0xe0U && first <= 0xefU) {
    scalar = first & 0x0fU;
    width = 3;
    minimum = 0x800U;
  } else if (first >= 0xf0U && first <= 0xf4U) {
    scalar = first & 0x07U;
    width = 4;
    minimum = 0x10000U;
  } else {
    return false;
  }
  if (offset + width > text.size()) return false;
  for (std::size_t index = 1; index < width; ++index) {
    const std::uint8_t continuation =
        static_cast<std::uint8_t>(text[offset + index]);
    if ((continuation & 0xc0U) != 0x80U) return false;
    scalar = (scalar << 6U) | (continuation & 0x3fU);
  }
  return scalar >= minimum && scalar <= 0x10ffffU &&
      !(scalar >= 0xd800U && scalar <= 0xdfffU);
}

} // namespace

std::optional<std::string> decode_string_literal(
    std::string_view spelling, TokenKind kind) {
  if (spelling.size() < 2) return std::nullopt;
  if (kind == TokenKind::RawStringLiteral) {
    return std::string(spelling.substr(1, spelling.size() - 2));
  }
  if (kind != TokenKind::StringLiteral) return std::nullopt;
  std::string output;
  for (std::size_t index = 1; index + 1 < spelling.size(); ++index) {
    const char character = spelling[index];
    if (character != '\\') {
      output.push_back(character);
      continue;
    }
    ++index;
    if (index + 1 >= spelling.size()) return std::nullopt;
    const char escape = spelling[index];
    switch (escape) {
    case '\\': output.push_back('\\'); break;
    case '"': output.push_back('"'); break;
    case '\'': output.push_back('\''); break;
    case 'n': output.push_back('\n'); break;
    case 'r': output.push_back('\r'); break;
    case 't': output.push_back('\t'); break;
    case '0': output.push_back('\0'); break;
    case 'x': {
      if (index + 2 >= spelling.size() - 1) return std::nullopt;
      const std::uint8_t high = hex_digit(spelling[index + 1]);
      const std::uint8_t low = hex_digit(spelling[index + 2]);
      if (high == 0xffU || low == 0xffU) return std::nullopt;
      output.push_back(static_cast<char>((high << 4U) | low));
      index += 2;
      break;
    }
    case 'u': {
      if (index + 1 >= spelling.size() || spelling[index + 1] != '{') {
        return std::nullopt;
      }
      index += 2;
      std::uint32_t scalar = 0;
      std::size_t digits = 0;
      while (index < spelling.size() && spelling[index] != '}') {
        const std::uint8_t digit = hex_digit(spelling[index]);
        if (digit == 0xffU || scalar > (0x10ffffU - digit) / 16U) {
          return std::nullopt;
        }
        scalar = scalar * 16U + digit;
        ++digits;
        ++index;
      }
      if (digits == 0 || index >= spelling.size() || scalar > 0x10ffffU ||
          (scalar >= 0xd800U && scalar <= 0xdfffU)) {
        return std::nullopt;
      }
      append_utf8(scalar, output);
      break;
    }
    default: return std::nullopt;
    }
  }
  return output;
}

std::optional<std::uint32_t> decode_rune_literal(std::string_view spelling) {
  if (spelling.size() < 3 || spelling.front() != '\'' ||
      spelling.back() != '\'') {
    return std::nullopt;
  }
  const std::string_view content = spelling.substr(1, spelling.size() - 2);
  if (content.empty()) return std::nullopt;

  if (content.front() != '\\') {
    std::uint32_t scalar = 0;
    std::size_t width = 0;
    if (!decode_utf8_scalar(content, 0, scalar, width) || width != content.size()) {
      return std::nullopt;
    }
    return scalar;
  }
  if (content.size() < 2) return std::nullopt;
  const char escape = content[1];
  if (content.size() == 2) {
    switch (escape) {
    case '\\': return static_cast<std::uint32_t>('\\');
    case '"': return static_cast<std::uint32_t>('"');
    case '\'': return static_cast<std::uint32_t>('\'');
    case 'n': return static_cast<std::uint32_t>('\n');
    case 'r': return static_cast<std::uint32_t>('\r');
    case 't': return static_cast<std::uint32_t>('\t');
    case '0': return 0U;
    default: return std::nullopt;
    }
  }
  if (escape == 'x' && content.size() == 4) {
    const std::uint8_t high = hex_digit(content[2]);
    const std::uint8_t low = hex_digit(content[3]);
    if (high == 0xffU || low == 0xffU) return std::nullopt;
    return static_cast<std::uint32_t>((high << 4U) | low);
  }
  if (escape != 'u' || content.size() < 5 || content[2] != '{' ||
      content.back() != '}') {
    return std::nullopt;
  }
  std::uint32_t scalar = 0;
  for (std::size_t index = 3; index + 1 < content.size(); ++index) {
    const std::uint8_t digit = hex_digit(content[index]);
    if (digit == 0xffU || scalar > (0x10ffffU - digit) / 16U) {
      return std::nullopt;
    }
    scalar = scalar * 16U + digit;
  }
  if (scalar > 0x10ffffU || (scalar >= 0xd800U && scalar <= 0xdfffU)) {
    return std::nullopt;
  }
  return scalar;
}

} // namespace draft
