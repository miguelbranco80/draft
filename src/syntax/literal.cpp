// Shared Draft string-literal decoder.

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

} // namespace draft
