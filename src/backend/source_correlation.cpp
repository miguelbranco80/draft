// Deterministic JSON encoding for native source-correlation sidecars.

#include "backend/source_correlation.h"

#include <algorithm>
#include <string_view>

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
        output.push_back(character);
      }
      break;
    }
  }
  output.push_back('"');
}

[[nodiscard]] bool entry_less(
    const SourceCorrelationEntry &left,
    const SourceCorrelationEntry &right) {
  if (left.package.root_identity != right.package.root_identity) {
    return left.package.root_identity < right.package.root_identity;
  }
  if (left.package.root_relative_path != right.package.root_relative_path) {
    return left.package.root_relative_path < right.package.root_relative_path;
  }
  if (left.procedure_ordinal != right.procedure_ordinal) {
    return left.procedure_ordinal < right.procedure_ordinal;
  }
  return left.ordinal < right.ordinal;
}

void append_coordinate(
    std::string_view file,
    LineColumn coordinate,
    std::string &output) {
  output += "{\"file\": ";
  append_json_string(file, output);
  output += ", \"line\": " + std::to_string(coordinate.line);
  output += ", \"column\": " + std::to_string(coordinate.column) + "}";
}

} // namespace

bool validate_source_correlation_map(
    const SourceCorrelationMap &map,
    std::string &reason) {
  reason.clear();
  if (map.format != "draft-source-correlation-v1") {
    reason = "unsupported source-correlation format";
    return false;
  }
  if (map.target_identity.empty() || map.compiler_identity.empty() ||
      map.program_identity.empty()) {
    reason = "source-correlation identity is incomplete";
    return false;
  }

  std::vector<SourceCorrelationEntry> entries = map.entries;
  std::sort(entries.begin(), entries.end(), entry_less);
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const SourceCorrelationEntry &entry = entries[index];
    if (entry.package.root_identity.empty() || entry.procedure.empty() ||
        entry.operation.empty() || entry.authored_file.empty() ||
        entry.generated_file.empty() || entry.authored.line == 0 ||
        entry.authored.column == 0 || entry.generated.line == 0 ||
        entry.generated.column == 0) {
      reason = "source-correlation entry is incomplete";
      return false;
    }
    if (index != 0) {
      const SourceCorrelationEntry &previous = entries[index - 1];
      if (previous.package == entry.package &&
          previous.procedure_ordinal == entry.procedure_ordinal &&
          previous.ordinal == entry.ordinal) {
        reason = "source-correlation operation identity is duplicated";
        return false;
      }
    }
  }
  return true;
}

std::string serialize_source_correlation_map(
    const SourceCorrelationMap &map) {
  std::vector<SourceCorrelationEntry> entries = map.entries;
  std::sort(entries.begin(), entries.end(), entry_less);

  std::string output;
  output += "{\n  \"format\": ";
  append_json_string(map.format, output);
  output += ",\n  \"target\": ";
  append_json_string(map.target_identity, output);
  output += ",\n  \"compiler\": ";
  append_json_string(map.compiler_identity, output);
  output += ",\n  \"program\": ";
  append_json_string(map.program_identity, output);
  output += ",\n  \"entries\": [";
  for (std::size_t index = 0; index < entries.size(); ++index) {
    const SourceCorrelationEntry &entry = entries[index];
    output += index == 0 ? "\n    {\"root\": " : ",\n    {\"root\": ";
    append_json_string(entry.package.root_identity, output);
    output += ", \"package\": ";
    append_json_string(entry.package.root_relative_path, output);
    output += ", \"procedure\": ";
    append_json_string(entry.procedure, output);
    output += ", \"procedure_ordinal\": " +
        std::to_string(entry.procedure_ordinal);
    output += ", \"ordinal\": " + std::to_string(entry.ordinal);
    output += ", \"operation\": ";
    append_json_string(entry.operation, output);
    output += ", \"authored\": ";
    append_coordinate(entry.authored_file, entry.authored, output);
    output += ", \"generated\": ";
    append_coordinate(entry.generated_file, entry.generated, output);
    output += ", \"synthesis_site\": ";
    append_json_string(entry.synthesis_site, output);
    output += '}';
  }
  output += entries.empty() ? "]\n" : "\n  ]\n";
  output += "}\n";
  return output;
}

} // namespace draft
