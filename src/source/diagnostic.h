// Structured diagnostics attached to exact source ranges.
//
// Compiler stages report facts into DiagnosticSink and do not print directly.
// This keeps semantic decisions independent from terminal formatting and later
// permits JSON, editor, and generated-source-aware renderers. Diagnostics retain
// insertion order because that order follows the deterministic compiler walk.
// Notes are ordinary entries; a future diagnostic group can add parent/child
// relationships without changing the source and syntax layers.

#pragma once

#include "source/source.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace draft {

enum class DiagnosticSeverity {
  Error,
  Warning,
  Note,
};

// Diagnostic owns its message so it remains valid after temporary parser or
// semantic state is released. The source range may be invalid only when the
// failure occurred before a source buffer existed, such as an open failure.
struct Diagnostic {
  DiagnosticSeverity severity = DiagnosticSeverity::Error;
  SourceRange range;
  std::string message;
};

// DiagnosticSink is append-only during one operation. A stage may continue
// after reporting an error to improve feedback, but later stages consult
// has_errors before assuming a complete valid program.
class DiagnosticSink {
public:
  void report(DiagnosticSeverity severity, SourceRange range, std::string message);
  void error(SourceRange range, std::string message);
  void warning(SourceRange range, std::string message);
  void note(SourceRange range, std::string message);

  [[nodiscard]] bool has_errors() const;
  [[nodiscard]] std::size_t error_count() const;
  [[nodiscard]] const std::vector<Diagnostic> &diagnostics() const;

private:
  std::vector<Diagnostic> diagnostics_;
  std::size_t error_count_ = 0;
};

[[nodiscard]] std::string_view diagnostic_severity_name(DiagnosticSeverity severity);

// Produces deterministic, human-readable diagnostics with one source line and
// a caret. Rendering deliberately avoids terminal color and locale behavior so
// snapshot tests and build logs are stable.
[[nodiscard]] std::string render_diagnostics(
    const SourceManager &sources, const DiagnosticSink &diagnostics);

} // namespace draft
