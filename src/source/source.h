// Source ownership and byte-addressed source locations.
//
// This module is the first persistent layer of the compiler. It owns the exact
// bytes loaded for each source file and maps stable FileId values to those bytes.
// Syntax, diagnostics, semantic nodes, generated-source maps, and later debug
// information refer to source exclusively through FileId plus byte offsets.
// Physical paths are retained for I/O and diagnostics but are deliberately not
// semantic identities; package loading will assign those separately.
//
// Ranges are half-open: begin identifies the first byte and end identifies the
// byte immediately after the construct. Line and column values presented to a
// user are one-based. Columns count UTF-8 scalar encodings rather than bytes for
// valid source, so a multibyte character occupies one displayed column. Invalid
// UTF-8 is still addressable by byte and is diagnosed by the lexer.
//
// Relevant specification: 01-core-language.md, "Source text and literals".

#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

// FileId is a stable index into one SourceManager. File zero is valid; the
// maximum uint32 value is reserved for locations that do not refer to source.
// IDs are process-local and must never enter a manifest or semantic hash.
struct FileId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();

  [[nodiscard]] bool is_valid() const;
  bool operator==(const FileId &) const = default;
};

// SourceLocation identifies a byte boundary, not necessarily a token start.
// The offset may equal the file size to represent EOF. It may not exceed it.
struct SourceLocation {
  FileId file;
  std::uint32_t offset = 0;

  [[nodiscard]] bool is_valid() const;
  bool operator==(const SourceLocation &) const = default;
};

// SourceRange represents [begin, end) within one file. Invalid ranges are used
// only for diagnostics such as a failure to open a file, where no bytes exist.
struct SourceRange {
  SourceLocation begin;
  SourceLocation end;

  [[nodiscard]] bool is_valid() const;
  [[nodiscard]] static SourceRange invalid();
  [[nodiscard]] static SourceRange at(FileId file, std::uint32_t offset);
};

// LineColumn is the user-facing coordinate corresponding to a byte location.
// Both fields are one-based. The source manager clamps an EOF location to the
// final logical line and returns a column one past that line's final scalar.
struct LineColumn {
  std::uint32_t line = 1;
  std::uint32_t column = 1;
};

// SourceFile owns one immutable source buffer after insertion. line_starts[0]
// is always zero; each later entry is the byte after a '\n'. A SourceFile value
// can move when another file is added, so callers retain FileId and string_view
// only for operations during which the manager is not mutated.
struct SourceFile {
  std::string display_path;
  std::string text;
  std::vector<std::uint32_t> line_starts;
};

// LoadFileResult distinguishes an I/O failure from a valid empty file without
// exceptions. On success, error is empty and file is valid. On failure, no file
// is inserted and error is a complete user-facing explanation.
struct LoadFileResult {
  bool ok = false;
  FileId file;
  std::string error;
};

// SourceManager is the sole owner of source bytes for one compiler invocation.
// Files are append-only, which keeps FileId stable. The class performs no path
// normalization or package identity work; that belongs to the workspace layer.
class SourceManager {
public:
  // Adds already available bytes under a diagnostic display path. This entry
  // point is also used by tests and, later, content-addressed generated source.
  // The bytes are not required to be valid UTF-8 here because the lexer must
  // produce a source-located diagnostic for invalid input.
  [[nodiscard]] FileId add_source(std::string display_path, std::string text);

  // Loads one file as binary bytes and inserts it only after a complete read.
  // The function never throws and does not mutate the manager on failure.
  [[nodiscard]] LoadFileResult load_file(const std::string &path);

  [[nodiscard]] const SourceFile &file(FileId id) const;
  [[nodiscard]] std::string_view text(FileId id) const;
  [[nodiscard]] std::string_view text(SourceRange range) const;
  [[nodiscard]] LineColumn line_column(SourceLocation location) const;
  [[nodiscard]] std::string_view line_text(FileId file, std::uint32_t one_based_line) const;
  [[nodiscard]] std::size_t file_count() const;

private:
  std::vector<SourceFile> files_;
};

} // namespace draft
