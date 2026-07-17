// Implementation of source ownership and byte-to-line mapping.
//
// The source manager intentionally does only two pieces of derived work: it
// records newline boundaries on insertion and counts UTF-8 continuation bytes
// when presenting a column. It does not validate tokens, normalize newlines, or
// canonicalize paths. Preserving original bytes is necessary for exact hashes,
// source maps, diagnostics, and generated expansion inspection.
//
// Relevant specification: 01-core-language.md, "Source text and literals".

#include "source/source.h"

#include <algorithm>
#include <cassert>
#include <fstream>
#include <sstream>
#include <utility>

namespace draft {

bool FileId::is_valid() const {
  return value != std::numeric_limits<std::uint32_t>::max();
}

bool SourceLocation::is_valid() const {
  return file.is_valid();
}

bool SourceRange::is_valid() const {
  return begin.is_valid() && end.is_valid() && begin.file == end.file && begin.offset <= end.offset;
}

SourceRange SourceRange::invalid() {
  return {};
}

SourceRange SourceRange::at(FileId file, std::uint32_t offset) {
  const SourceLocation location{file, offset};
  return {location, location};
}

FileId SourceManager::add_source(std::string display_path, std::string text) {
  assert(files_.size() < static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));

  SourceFile source;
  source.display_path = std::move(display_path);
  source.text = std::move(text);
  source.line_starts.push_back(0);

  // Line starts are byte offsets because every compiler source range is byte
  // addressed. UTF-8 is decoded only when converting a byte offset to a display
  // column, avoiding a second coordinate system in tokens and semantic nodes.
  for (std::size_t offset = 0; offset < source.text.size(); ++offset) {
    if (source.text[offset] == '\n') {
      const std::size_t next = offset + 1;
      assert(next <= static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max()));
      source.line_starts.push_back(static_cast<std::uint32_t>(next));
    }
  }

  const FileId id{static_cast<std::uint32_t>(files_.size())};
  files_.push_back(std::move(source));
  return id;
}

LoadFileResult SourceManager::load_file(const std::string &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {false, {}, "cannot open source file '" + path + "'"};
  }

  std::ostringstream bytes;
  bytes << input.rdbuf();
  if (input.bad()) {
    return {false, {}, "cannot read source file '" + path + "'"};
  }

  FileId id = add_source(path, bytes.str());
  return {true, id, {}};
}

const SourceFile &SourceManager::file(FileId id) const {
  assert(id.is_valid());
  assert(static_cast<std::size_t>(id.value) < files_.size());
  return files_[id.value];
}

std::string_view SourceManager::text(FileId id) const {
  return file(id).text;
}

std::string_view SourceManager::text(SourceRange range) const {
  assert(range.is_valid());
  const std::string_view bytes = text(range.begin.file);
  assert(static_cast<std::size_t>(range.end.offset) <= bytes.size());
  return bytes.substr(range.begin.offset, range.end.offset - range.begin.offset);
}

LineColumn SourceManager::line_column(SourceLocation location) const {
  assert(location.is_valid());
  const SourceFile &source = file(location.file);
  assert(static_cast<std::size_t>(location.offset) <= source.text.size());

  const auto after_line = std::upper_bound(
      source.line_starts.begin(), source.line_starts.end(), location.offset);
  const std::size_t line_index = static_cast<std::size_t>(after_line - source.line_starts.begin() - 1);
  const std::uint32_t line_start = source.line_starts[line_index];

  // A valid UTF-8 scalar has exactly one byte that is not a continuation byte.
  // Counting such bytes gives a stable scalar column without needing character
  // width or locale policy. For invalid UTF-8, each invalid leading byte still
  // occupies one diagnostic column, which keeps error locations monotonic.
  std::uint32_t column = 1;
  for (std::uint32_t offset = line_start; offset < location.offset; ++offset) {
    const unsigned char byte = static_cast<unsigned char>(source.text[offset]);
    if ((byte & 0xc0U) != 0x80U) {
      ++column;
    }
  }

  return {static_cast<std::uint32_t>(line_index + 1), column};
}

std::string_view SourceManager::line_text(FileId file_id, std::uint32_t one_based_line) const {
  const SourceFile &source = file(file_id);
  assert(one_based_line >= 1);
  assert(static_cast<std::size_t>(one_based_line) <= source.line_starts.size());

  const std::size_t line_index = static_cast<std::size_t>(one_based_line - 1);
  const std::uint32_t start = source.line_starts[line_index];
  std::uint32_t end = static_cast<std::uint32_t>(source.text.size());
  if (line_index + 1 < source.line_starts.size()) {
    end = source.line_starts[line_index + 1] - 1; // Exclude the terminating '\n'.
  }
  if (end > start && source.text[end - 1] == '\r') {
    --end;
  }
  return std::string_view(source.text).substr(start, end - start);
}

std::size_t SourceManager::file_count() const {
  return files_.size();
}

} // namespace draft
