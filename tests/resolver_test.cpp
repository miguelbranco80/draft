// Transactional resolver tests with a deterministic in-process provider.

#include "compile/resolver.h"

#include "backend/toolchain.h"
#include "base/timing.h"
#include "compile/compiler.h"
#include "elaborator/provider.h"
#include "elaborator/resolution.h"
#include "elaborator/resolution_store.h"
#include "source/diagnostic.h"
#include "source/source.h"
#include "target/profile.h"

#include "test_directory.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

struct TestState {
  int failures = 0;

  void expect(bool condition, std::string_view expression, int line) {
    if (!condition) {
      ++failures;
      std::cerr << "resolver_test.cpp:" << line
                << ": expectation failed: " << expression << '\n';
    }
  }
};

#define EXPECT(state, expression) (state).expect((expression), #expression, __LINE__)

struct TemporaryWorkspace {
  draft::test::TemporaryDirectory directory{"draft-resolver-test"};
  std::filesystem::path root;
  std::filesystem::path package;

  TemporaryWorkspace() {
    root = directory.path();
    std::error_code error;
    package = root / "app";
    std::filesystem::create_directories(package, error);
    if (error) std::exit(EXIT_FAILURE);
    std::ofstream attachment(package / "PROMPT.txt", std::ios::binary);
    attachment << "exact attachment bytes\n";
  }

  // Rewrites only surface source; .draft remains untouched so tests can observe
  // stale pin behavior and atomic preservation of the previous manifest.
  void write_source(std::string_view prompt) const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "answer :: proc() -> i64 {\n"
           << "    return ... \"" << prompt << "\" file \"PROMPT.txt\"\n"
           << "}\n\n"
           << "main :: proc() {\n"
           << "}\n";
  }

  // Two independent body sites make selective regeneration observable: one
  // accepted fragment can change while the unrelated fresh pin remains exact.
  void write_two_expression_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "first :: proc() -> i64 {\n"
           << "    return ... \"first expression\"\n"
           << "}\n\n"
           << "second :: proc() -> i64 {\n"
           << "    return ... \"second expression\"\n"
           << "}\n\n"
           << "main :: proc() {\n"
           << "}\n";
  }

  // The body names both an entire declaration and one aggregate field supplied
  // by early synthesis. A one-pass body checker would reject these names before
  // the provider could make the program complete. The package judgment forces
  // each intermediate round to build its review obligation too: incomplete
  // generated declarations must wait for a complete typed context row rather
  // than aborting the source transaction.
  void write_staged_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "judge \"The resolved package preserves generated values.\"\n\n"
           << "... \"declare answer\"\n\n"
           << "Packet :: struct {\n"
           << "    ... \"add value field\"\n"
           << "}\n\n"
           << "main :: proc() {\n"
           << "    packet: Packet\n"
           << "    packet.value = answer\n"
           << "    expected: i64 = ... \"compute expected value\"\n"
           << "    ... \"verify generated values\"\n"
           << "}\n";
  }

  // The consumer cannot even resolve its body until a dependency has published
  // the generated public declaration. This forces more than one interface
  // discovery round across the package graph.
  void write_dependency_staged_source() const {
    std::error_code error;
    const std::filesystem::path dependency = root / "dep";
    std::filesystem::create_directories(dependency, error);
    if (error) std::exit(EXIT_FAILURE);
    std::ofstream dependency_source(
        dependency / "package.draft", std::ios::binary | std::ios::trunc);
    dependency_source << "package dep\n\n"
                      << "... \"declare public answer\"\n";
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "import dep\n\n"
           << "main :: proc() {\n"
           << "    answer: i64 = dep.answer\n"
           << "    assert(answer == 42)\n"
           << "}\n";
  }

  // These two sites are deliberately in the same package-level interface
  // completeness set. The provider returns declarations that the final body
  // needs, but neither request is allowed to observe the other response.
  void write_opaque_interface_set_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "... \"declare first\"\n"
           << "... \"declare second\"\n\n"
           << "main :: proc() {\n"
           << "    assert(first + second == 42)\n"
           << "}\n";
  }

  // The procedure body is a compile-time dependency of Selected, which in
  // turn selects the declaration synthesis site. The expression site must run
  // in an earlier interface round even though expression sites normally belong
  // to the later runtime-body stage.
  void write_compile_time_dependency_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "compile_value :: proc() -> i64 {\n"
           << "    return ... \"compute compile-time selector\"\n"
           << "}\n\n"
           << "Selected :: compile_value()\n\n"
           << "when Selected == 42 {\n"
           << "    ... \"declare public answer\"\n"
           << "}\n\n"
           << "main :: proc() {\n"
           << "    assert(answer == 42)\n"
           << "}\n";
  }

  // A synthesis expression can itself be the package `when` condition. This
  // exercises the evaluator-owned site path: there is no enclosing procedure
  // body for BodyChecker to discover, but the condition still has exact bool
  // context and must precede the selected declaration set.
  void write_direct_condition_dependency_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "when ... \"select declaration\" {\n"
           << "    ... \"declare public answer\"\n"
           << "}\n\n"
           << "main :: proc() {\n"
           << "    assert(answer == 42)\n"
           << "}\n";
  }

  // The compile-time procedure cannot be checked until the current package
  // declaration completeness set supplies generated_value. Resolution must
  // therefore take three interface rounds: declaration, expression, then the
  // declaration selected by the resulting constant.
  void write_structural_then_compile_time_dependency_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "... \"declare compile input\"\n\n"
           << "compile_value :: proc() -> i64 {\n"
           << "    increment: i64 = "
              "... \"compute compile-time increment\"\n"
           << "    return generated_value + increment\n"
           << "}\n\n"
           << "Selected :: compile_value()\n\n"
           << "when Selected == 42 {\n"
           << "    ... \"declare public answer\"\n"
           << "}\n\n"
           << "main :: proc() {\n"
           << "    assert(answer == 42)\n"
           << "}\n";
  }

  // A direct synthesis expression can be the integer recipe that determines a
  // type layout. The type resolver supplies usize context before the provider
  // runs; the complete type is rebuilt only after the expansion is installed.
  void write_direct_layout_dependency_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "pub Buffer :: [... \"choose array length\"]u8\n\n"
           << "main :: proc() {\n"
           << "    value: Buffer\n"
           << "}\n";
  }

  // The array length is produced by a full compile-time procedure. Its body is
  // therefore an early type-layout dependency even though it would ordinarily
  // be checked only after package interfaces were complete.
  void write_procedure_layout_dependency_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "compile_length :: proc() -> usize {\n"
           << "    return ... \"compute array length\"\n"
           << "}\n\n"
           << "Buffer :: [compile_length()]u8\n\n"
           << "main :: proc() {\n"
           << "    value: Buffer\n"
           << "}\n";
  }

  // The other fixed integer-recipe boundaries share the same discovery path:
  // a generic value argument, aggregate alignment, and SIMD lane count each
  // require an exact usize expansion before their types are complete.
  void write_integer_recipe_boundaries_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "Box[N: usize] :: struct {\n"
           << "    values: [N]u8,\n"
           << "}\n\n"
           << "Applied :: Box[... \"choose value argument\"]\n\n"
           << "Aligned :: @align(... \"choose alignment\") struct {\n"
           << "    value: u8,\n"
           << "}\n\n"
           << "Vector :: #simd[... \"choose SIMD lanes\"]u32\n\n"
           << "Mode :: enum u8 {\n"
           << "    Zero = ... \"choose enum value\",\n"
           << "}\n\n"
           << "main :: proc() {\n"
           << "    applied: Applied\n"
           << "    aligned: Aligned\n"
           << "    vector: Vector\n"
           << "    mode: Mode\n"
           << "}\n";
  }

  // A consumer must wait for a dependency whose public type layout is still a
  // synthesis recipe. The dependency publishes no partial invalid interface;
  // its complete Buffer type appears only on the next clean graph round.
  void write_dependency_layout_source() const {
    std::error_code error;
    const std::filesystem::path dependency = root / "dep";
    std::filesystem::create_directories(dependency, error);
    if (error) std::exit(EXIT_FAILURE);
    std::ofstream dependency_source(
        dependency / "package.draft", std::ios::binary | std::ios::trunc);
    dependency_source << "package dep\n\n"
                      << "pub Buffer :: ["
                         "... \"choose dependency array length\"]u8\n";
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "import dep\n\n"
           << "main :: proc() {\n"
           << "    value: dep.Buffer\n"
           << "}\n";
  }

  // Packet's member completion cannot affect the independent compile_value
  // procedure. Interface discovery should therefore publish both obligations
  // in one opaque round, then expose the declaration selected by the result.
  void write_independent_member_and_compile_time_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "Packet :: struct {\n"
           << "    ... \"add value field\"\n"
           << "}\n\n"
           << "compile_value :: proc() -> i64 {\n"
           << "    return ... \"compute independent selector\"\n"
           << "}\n\n"
           << "Selected :: compile_value()\n\n"
           << "when Selected == 42 {\n"
           << "    ... \"declare public answer\"\n"
           << "}\n\n"
           << "main :: proc() {\n"
           << "    packet: Packet\n"
           << "    packet.value = answer\n"
           << "}\n";
  }

  // The evaluator skips the false branch and reaches the synthesis return, but
  // ordinary body checking still validates that branch. Its missing Packet
  // member makes the procedure depend on the current member completeness set,
  // so only the member obligation may appear in the first round.
  void write_dependent_member_and_compile_time_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "Packet :: struct {\n"
           << "    ... \"add value field\"\n"
           << "}\n\n"
           << "compile_value :: proc() -> i64 {\n"
           << "    if false {\n"
           << "        packet: Packet\n"
           << "        packet.value = 0\n"
           << "    }\n"
           << "    return ... \"compute dependent selector\"\n"
           << "}\n\n"
           << "Selected :: compile_value()\n\n"
           << "when Selected == 42 {\n"
           << "    ... \"declare public answer\"\n"
           << "}\n\n"
           << "main :: proc() {\n"
           << "    assert(answer == 42)\n"
           << "}\n";
  }

  void write_complete_source() const {
    std::ofstream source(
        package / "package.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "main :: proc() {\n"
           << "}\n";
  }

  // This is the original retained-body replay failure in its smallest complete
  // resolver form. The surface body and accepted replacement both request the
  // same cross-package static-pack specialization. Proposal checking copies
  // the current compiler graph, applies only the body overlay, and must reuse
  // the dependency's equal body-work key instead of rechecking its already
  // enriched semantic tables.
  void write_body_site_with_static_pack_dependency() const {
    std::error_code error;
    const std::filesystem::path formatting = root / "formatting";
    std::filesystem::create_directories(formatting, error);
    if (error)
      std::exit(EXIT_FAILURE);
    std::ofstream formatting_source(formatting / "package.draft",
                                    std::ios::binary | std::ios::trunc);
    formatting_source
        << "package formatting\n\n"
           "pub consume :: proc(values: ..type) {\n"
           "    for value in values {\n"
           "        when type_of(value) == string {\n"
           "        } else when type_kind(type_of(value)) == .signed_integer "
           "{\n"
           "        } else {\n"
           "            static_assert(false, \"unsupported value\")\n"
           "        }\n"
           "    }\n"
           "}\n";
    std::ofstream source(package / "package.draft",
                         std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
              "import formatting\n\n"
              "main :: proc() {\n"
              "    expected: i64 = ... \"produce expected value\"\n"
              "    formatting.consume(\"value\", expected)\n"
              "}\n";
  }

  void write_test_source(std::string_view extra_statement = {}) const {
    std::ofstream source(
        package / "candidate_test.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "import core/testing\n\n"
           << "test_generated_answer :: proc(test: ^testing.Test) {\n"
           << "    // Validation comments are not semantic agent context.\n"
           << "    testing.expect(test, answer() == 42)\n";
    if (!extra_statement.empty()) source << "    " << extra_statement << "\n";
    source << "}\n";
  }

  void write_invalid_test_source() const {
    std::ofstream source(
        package / "candidate_test.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "import core/testing\n\n"
           << "test_invalid :: proc(test: ^testing.Test) -> i64 {\n"
           << "    return 0\n"
           << "}\n";
  }

  void write_benchmark_source() const {
    std::ofstream source(
        package / "candidate_bench.draft", std::ios::binary | std::ios::trunc);
    source << "package app\n\n"
           << "import core/benchmark\n\n"
           << "bench_generated_answer :: proc(state: ^benchmark.Benchmark) {\n"
           << "}\n";
  }
};

struct FakeProviderState {
  std::size_t calls = 0;
  std::size_t preparation_calls = 0;
  std::string response = "42";
  std::string last_prompt;
  std::string last_attachment;
  // This mode deliberately returns a well-framed but ill-typed expression on
  // the first call, then uses the compiler-owned correction transcript to
  // return a valid expression. It proves retries occur above the provider.
  bool correct_after_rejection = false;
  bool report_error_and_succeed = false;
  bool staged_responses = false;
  bool opaque_interface_responses = false;
  std::vector<draft::AgentConstructKind> kinds;
  std::vector<std::string> prompts;
  std::vector<std::uint64_t> occurrences;
  std::vector<std::string> expected_type_texts;
  std::vector<std::string> anchor_names;
  std::vector<std::string> site_identities;
  std::vector<std::vector<std::string>> visible_binding_names;
  std::vector<draft::AgentValidationContext> last_validation_context;
  std::vector<std::vector<draft::SynthesisRejection>> rejection_histories;
};

// Preparation is observable but performs no setup. A multi-stage resolver test
// uses it to prove the callback belongs to the command, not to a site or stage.
bool prepare_fake_provider(
    void *opaque,
    draft::DiagnosticSink &diagnostics) {
  (void)diagnostics;
  auto *state = static_cast<FakeProviderState *>(opaque);
  ++state->preparation_calls;
  return true;
}
[[nodiscard]] bool boolean_cancellation_requested(void *opaque) {
  return *static_cast<bool *>(opaque);
}

[[nodiscard]] bool atomic_boolean_cancellation_requested(void *opaque) {
  return static_cast<std::atomic_bool *>(opaque)->load(
      std::memory_order_relaxed);
}

// The fake intentionally performs no language validation. This proves the
// resolver, rather than the provider, is responsible for accepting a proposal.
bool synthesize(
    void *opaque,
    const draft::SynthesisRequest &request,
    draft::SynthesisResponse &response,
    draft::DiagnosticSink &diagnostics) {
  (void)diagnostics;
  auto *state = static_cast<FakeProviderState *>(opaque);
  ++state->calls;
  state->kinds.push_back(request.obligation.kind);
  state->prompts.push_back(request.prompt);
  state->occurrences.push_back(request.obligation.occurrence);
  state->expected_type_texts.push_back(
      request.obligation.expected_type_text);
  state->anchor_names.push_back(request.obligation.anchor_name);
  state->site_identities.push_back(request.obligation.site_identity);
  state->last_prompt = request.prompt;
  state->last_attachment = request.attachments.empty()
      ? std::string()
      : request.attachments[0].contents;
  state->last_validation_context = request.obligation.validation_context;
  state->rejection_histories.push_back(request.prior_rejections);
  std::vector<std::string> visible_names;
  for (const draft::AgentVisibleBinding &binding :
       request.obligation.visible_bindings) {
    visible_names.push_back(binding.name);
  }
  state->visible_binding_names.push_back(std::move(visible_names));
  if (state->report_error_and_succeed) {
    diagnostics.error(
        draft::SourceRange::invalid(),
        "provider reported an error with a successful return");
    response.source = "42";
  } else if (state->correct_after_rejection) {
    response.source = request.prior_rejections.empty()
        ? "\"not an i64\""
        : "42";
  } else if (state->opaque_interface_responses) {
    if (request.prompt == "declare first") {
      response.source = "first :: 20;";
    } else if (request.prompt == "declare second") {
      response.source = "second :: 22;";
    } else {
      diagnostics.error(
          draft::SourceRange::invalid(),
          "opaque-set fixture received an unexpected prompt");
      return false;
    }
  } else if (state->staged_responses) {
    switch (request.obligation.kind) {
    case draft::AgentConstructKind::SynthesisDeclaration:
      response.source = request.prompt == "declare compile input"
          ? "generated_value :: cast[i64](40);"
          : "pub answer :: 42;";
      break;
    case draft::AgentConstructKind::SynthesisMember:
      response.source = "value: i64,";
      break;
    case draft::AgentConstructKind::SynthesisStatement:
      response.source =
          "assert(packet.value == answer && expected == 42)";
      break;
    case draft::AgentConstructKind::SynthesisExpression:
      if (request.prompt == "select declaration") {
        response.source = "true";
      } else if (request.prompt == "compute compile-time increment") {
        response.source = "2";
      } else if (request.prompt == "choose value argument" ||
                 request.prompt == "choose alignment" ||
                 request.prompt == "choose SIMD lanes") {
        response.source = "4";
      } else if (request.prompt == "choose enum value") {
        response.source = "0";
      } else {
        response.source = "42";
      }
      break;
    default:
      diagnostics.error(
          draft::SourceRange::invalid(),
          "fixture received an unexpected synthesis category");
      return false;
    }
  } else {
    response.source = state->response;
  }
  return true;
}

// This provider is intentionally safe for concurrent calls and exposes its
// overlap through atomics only. Two-site body and interface waves rendezvous;
// the mixed correction test deliberately permits its one remaining site to
// proceed alone. A sequential scheduler times out the rendezvous and makes the
// focused test fail instead of deadlocking indefinitely.
struct ParallelProviderState {
  bool fail = false;
  bool one_site_accepts_first = false;
  bool interface_mode = false;
  std::atomic_size_t first_attempt_calls = 0;
  std::atomic_size_t correction_attempt_calls = 0;
  std::atomic_size_t first_attempt_active = 0;
  std::atomic_size_t correction_attempt_active = 0;
  std::atomic_size_t first_attempt_maximum = 0;
  std::atomic_size_t correction_attempt_maximum = 0;
  std::mutex ready_mutex;
  std::condition_variable ready_changed;
};

// Records the largest simultaneous-callback count. Relaxed ordering is enough:
// the value is test telemetry only, no provider data depends on it, and joining
// the resolver's worker threads happens before the main test reads the result.
void observe_maximum(std::atomic_size_t &maximum, std::size_t value) {
  std::size_t observed = maximum.load(std::memory_order_relaxed);
  while (observed < value && !maximum.compare_exchange_weak(
             observed,
             value,
             std::memory_order_relaxed,
             std::memory_order_relaxed)) {
  }
}

// Waits without consuming a CPU until the expected wave has entered the fake.
// calls is atomic because increments occur before taking ready_mutex; the mutex
// and notification prevent a missed wakeup, while the five-second deadline
// converts a sequential or broken scheduler into a deterministic test failure.
[[nodiscard]] bool wait_for_ready_calls(
    ParallelProviderState &state,
    std::atomic_size_t &calls,
    std::size_t expected_calls) {
  std::unique_lock lock(state.ready_mutex);
  state.ready_changed.notify_all();
  return state.ready_changed.wait_for(
      lock,
      std::chrono::seconds(5),
      [&calls, expected_calls]() {
        return calls.load(std::memory_order_relaxed) >= expected_calls;
      });
}

bool synthesize_in_parallel(
    void *opaque,
    const draft::SynthesisRequest &request,
    draft::SynthesisResponse &response,
    draft::DiagnosticSink &diagnostics) {
  auto *state = static_cast<ParallelProviderState *>(opaque);
  const bool correction = !request.prior_rejections.empty();
  if (request.prior_rejections.size() > 1) {
    diagnostics.error(
        draft::SourceRange::invalid(),
        "parallel provider fixture received too many correction rows");
    return false;
  }
  std::atomic_size_t &calls = correction
      ? state->correction_attempt_calls
      : state->first_attempt_calls;
  std::atomic_size_t &active = correction
      ? state->correction_attempt_active
      : state->first_attempt_active;
  std::atomic_size_t &maximum = correction
      ? state->correction_attempt_maximum
      : state->first_attempt_maximum;
  calls.fetch_add(1, std::memory_order_relaxed);
  const std::size_t active_now =
      active.fetch_add(1, std::memory_order_relaxed) + 1;
  observe_maximum(maximum, active_now);
  const std::size_t expected_calls =
      state->one_site_accepts_first && correction ? 1 : 2;
  if (!wait_for_ready_calls(*state, calls, expected_calls)) {
    active.fetch_sub(1, std::memory_order_relaxed);
    diagnostics.error(
        draft::SourceRange::invalid(),
        "parallel provider fixture did not observe its complete ready wave");
    return false;
  }

  if (state->fail) {
    // Finish the second site first. The resolver must nevertheless publish the
    // lower package/obligation-order site's diagnostic after joining the wave.
    if (request.prompt == "first expression") {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      diagnostics.error(
          draft::SourceRange::invalid(), "first provider failure");
    } else {
      diagnostics.error(
          draft::SourceRange::invalid(), "second provider failure");
    }
    active.fetch_sub(1, std::memory_order_relaxed);
    return false;
  }

  if (state->interface_mode) {
    // Both requests were frozen before either generated declaration existed.
    // Observing a sibling name here would violate opaque completeness even if
    // final overlay publication remained deterministic.
    for (const draft::AgentVisibleBinding &binding :
         request.obligation.visible_bindings) {
      if (binding.name == "first" || binding.name == "second") {
        active.fetch_sub(1, std::memory_order_relaxed);
        diagnostics.error(
            draft::SourceRange::invalid(),
            "concurrent interface request observed a sibling expansion");
        return false;
      }
    }
    if (request.prompt == "declare first") {
      response.source = "first :: 20;";
    } else if (request.prompt == "declare second") {
      response.source = "second :: 22;";
    } else {
      active.fetch_sub(1, std::memory_order_relaxed);
      diagnostics.error(
          draft::SourceRange::invalid(),
          "concurrent interface fixture received an unexpected prompt");
      return false;
    }
  } else if (correction ||
             (state->one_site_accepts_first &&
              request.prompt == "first expression")) {
    response.source = "42";
  } else {
    response.source = "\"not an i64\"";
  }
  active.fetch_sub(1, std::memory_order_relaxed);
  return true;
}

// The first call completes normally but requests command cancellation. With a
// one-worker provider bound, the resolver must observe that flag in the next
// queued task before calling this function again.
struct CancellingProviderState {
  std::atomic_size_t calls = 0;
  std::atomic_bool *cancelled = nullptr;
};

bool synthesize_then_cancel(
    void *opaque,
    const draft::SynthesisRequest &request,
    draft::SynthesisResponse &response,
    draft::DiagnosticSink &diagnostics) {
  (void)request;
  (void)diagnostics;
  auto *state = static_cast<CancellingProviderState *>(opaque);
  state->calls.fetch_add(1, std::memory_order_relaxed);
  response.source = "42";
  state->cancelled->store(true, std::memory_order_relaxed);
  return true;
}

#if defined(__APPLE__)
[[nodiscard]] bool run_executable(
    const std::filesystem::path &executable,
    const std::filesystem::path &working_directory,
    int &status) {
  const pid_t child = ::fork();
  if (child < 0) return false;
  if (child == 0) {
    if (::chdir(working_directory.c_str()) != 0) _exit(126);
    ::execl(executable.c_str(), executable.c_str(), nullptr);
    _exit(127);
  }
  while (::waitpid(child, &status, 0) < 0) {
    if (errno != EINTR) return false;
  }
  return true;
}

[[nodiscard]] std::string read_binary_file(
    const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  input.seekg(0, std::ios::end);
  const std::streamoff end = input.tellg();
  if (!input || end < 0) return {};
  std::string contents(static_cast<std::size_t>(end), '\0');
  input.seekg(0, std::ios::beg);
  if (!contents.empty()) {
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  }
  return input ? contents : std::string();
}
#endif

draft::CompileWorkspaceOptions compile_options(
    const TemporaryWorkspace &workspace) {
  draft::CompileWorkspaceOptions options;
  options.target = draft::make_aarch64_macos_profile();
  options.workspace.workspace_directory = workspace.root.string();
  options.workspace.core_directory =
      std::string(DRAFT_SOURCE_DIRECTORY) + "/core";
  options.workspace.core_content_identity = "draft-core-bootstrap-v4";
  return options;
}

// Every resolver fixture selects the `app` package below its workspace root.
// Store assertions name that package explicitly so they cannot accidentally
// observe another executable's manifest.
draft::ResolutionStoreKey app_store_key() {
  return {
      draft::make_aarch64_macos_profile().facts.identity,
      {"workspace", "app"},
  };
}

draft::ResolveWorkspaceOptions resolve_options(
    const TemporaryWorkspace &workspace,
    FakeProviderState &provider_state) {
  draft::ResolveWorkspaceOptions options;
  options.compile = compile_options(workspace);
  options.provider.provider_identity = "deterministic-fake-provider-v1";
  options.provider.model_identity = "fixture-model-v1";
  options.provider.configuration_identity = "temperature-0-schema-v1";
  options.provider.state = &provider_state;
  options.provider.synthesize = synthesize;
  return options;
}

draft::ResolveWorkspaceOptions parallel_resolve_options(
    const TemporaryWorkspace &workspace,
    ParallelProviderState &provider_state) {
  draft::ResolveWorkspaceOptions options;
  options.compile = compile_options(workspace);
  options.provider.provider_identity = "parallel-fake-provider-v1";
  options.provider.model_identity = "fixture-model-v1";
  options.provider.configuration_identity = "parallel-fixture-v1";
  options.provider.state = &provider_state;
  options.provider.synthesize = synthesize_in_parallel;
  options.provider.maximum_parallel_calls = 2;
  return options;
}

draft::ExternalInputPin fake_runtime_asset_pin() {
  draft::ExternalInputPin pin;
  pin.kind = draft::ExternalInputKind::RuntimeAsset;
  pin.name = "fixture-runtime-data";
  pin.content_digest = draft::sha256("exact fixture runtime data tree");
  pin.entry_point = "tables.bin";
  return pin;
}

void test_resolution_reuse_revalidation_and_failure(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_source("first prompt");
  FakeProviderState provider;

  draft::SourceManager first_sources;
  draft::DiagnosticSink first_diagnostics;
  draft::ResolveWorkspaceOptions first_options =
      resolve_options(workspace, provider);
  first_options.compile.lower_mir = true;
  first_options.compile.emit_llvm = true;
  const draft::CompileWorkspaceOptions first_build_options =
      first_options.compile;
  first_options.external_inputs_configured = true;
  first_options.external_inputs.push_back(fake_runtime_asset_pin());
  draft::ResolveWorkspaceResult first = draft::resolve_workspace(
      first_sources,
      workspace.package.string(),
      std::move(first_options),
      first_diagnostics);
  if (!first.ok) {
    std::cerr << draft::render_diagnostics(first_sources, first_diagnostics);
  }
  EXPECT(state, first.ok);
  EXPECT(state, first.committed);
  EXPECT(state, first.synthesized_sites == 1);
  EXPECT(state, first.manifest.external_inputs.size() == 1);
  EXPECT(state, first.compiled_program.has_value());
  if (first.compiled_program.has_value()) {
    EXPECT(state,
        first.compiled_program->progress ==
            draft::CompileWorkspaceProgress::SemanticClosure);
    EXPECT(state,
        first.compiled_program->resolved_program_digest ==
            std::optional<draft::Sha256Digest>(
                first.manifest.resolved_program_digest));
    EXPECT(state, first.compiled_program->resolution_manifest.has_value());

    // Resolution commits before a build continuation. Advancing the returned
    // graph here proves the caller can lower the exact committed declarations,
    // types, and bodies without asking the resolver to rebuild its front end.
    EXPECT(state,
        draft::continue_compiled_workspace(
            first_sources,
            first_build_options,
            *first.compiled_program,
            first_diagnostics));
    EXPECT(state,
        first.compiled_program->progress ==
            draft::CompileWorkspaceProgress::TargetLowering);
    const std::size_t root_index = static_cast<std::size_t>(
        first.compiled_program->graph.root_package.value);
    EXPECT(state, root_index < first.compiled_program->packages.size());
    if (root_index < first.compiled_program->packages.size() &&
        first.compiled_program->packages[root_index].has_value()) {
      EXPECT(state, first.compiled_program->packages[root_index]->llvm.ok);
    }
  }
  EXPECT(state, provider.calls == 1);
  EXPECT(state, provider.last_prompt == "first prompt");
  EXPECT(state, provider.last_attachment == "exact attachment bytes\n");

  // A fresh pin builds with no provider boundary in scope.
  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  draft::CompileWorkspaceOptions offline_options = compile_options(workspace);
  draft::TimingRecorder offline_timings(draft::TimingOutput::Summary);
  offline_options.timings = &offline_timings;
  offline_options.lower_mir = true;
  offline_options.emit_llvm = true;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          offline_options,
          offline_diagnostics);
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());
  const std::string offline_report = offline_timings.render();
  EXPECT(state,
      offline_report.find("compiler passes: 1") != std::string::npos);
  EXPECT(state,
      offline_report.find("workspace loads: 1") != std::string::npos);
  EXPECT(state, offline.resolution_manifest.has_value());
  if (offline.resolution_manifest.has_value()) {
    EXPECT(state,
        offline.resolution_manifest->external_inputs.size() == 1);
  }

  draft::SourceManager reuse_sources;
  draft::DiagnosticSink reuse_diagnostics;
  const draft::ResolveWorkspaceResult reuse = draft::resolve_workspace(
      reuse_sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      reuse_diagnostics);
  EXPECT(state, reuse.ok);
  EXPECT(state, reuse.reused_sites == 1);
  EXPECT(state, reuse.synthesized_sites == 0);
  EXPECT(state, provider.calls == 1);
  EXPECT(state, reuse.manifest.external_inputs.size() == 1);
  if (reuse.manifest.external_inputs.size() == 1) {
    EXPECT(state,
        reuse.manifest.external_inputs.front().content_digest ==
            fake_runtime_asset_pin().content_digest);
  }

  // Provider configuration is provenance, not source freshness. Selecting a
  // different model or adapter policy must reuse accepted source while the
  // typed obligation remains unchanged.
  draft::ResolveWorkspaceOptions changed_provider =
      resolve_options(workspace, provider);
  changed_provider.provider.model_identity = "fixture-model-v2";
  changed_provider.provider.configuration_identity =
      "temperature-0-schema-v2";
  draft::SourceManager changed_provider_sources;
  draft::DiagnosticSink changed_provider_diagnostics;
  const draft::ResolveWorkspaceResult changed_provenance =
      draft::resolve_workspace(
          changed_provider_sources,
          workspace.package.string(),
          std::move(changed_provider),
          changed_provider_diagnostics);
  EXPECT(state, changed_provenance.ok);
  EXPECT(state, changed_provenance.synthesized_sites == 0);
  EXPECT(state, changed_provenance.reused_sites == 1);
  EXPECT(state, provider.calls == 1);
  EXPECT(state,
      changed_provenance.manifest.resolved_program_digest ==
          reuse.manifest.resolved_program_digest);
  EXPECT(state, changed_provenance.manifest.pins.size() == 1);
  if (changed_provenance.manifest.pins.size() == 1) {
    EXPECT(state,
        changed_provenance.manifest.pins[0].model_identity ==
            "fixture-model-v1");
    EXPECT(state,
        changed_provenance.manifest.pins[0].configuration_identity ==
            "temperature-0-schema-v1");
  }

  // Revalidation accepts the same generated bytes under a changed obligation
  // only after a complete new compile; it never invokes the provider.
  workspace.write_source("changed for revalidation");
  draft::ResolveWorkspaceOptions revalidate =
      resolve_options(workspace, provider);
  revalidate.revalidate = true;
  draft::SourceManager revalidate_sources;
  draft::DiagnosticSink revalidate_diagnostics;
  const draft::ResolveWorkspaceResult revalidated = draft::resolve_workspace(
      revalidate_sources,
      workspace.package.string(),
      std::move(revalidate),
      revalidate_diagnostics);
  EXPECT(state, revalidated.ok);
  EXPECT(state, revalidated.reused_sites == 1);
  EXPECT(state, provider.calls == 1);
  EXPECT(state, revalidated.manifest.external_inputs.size() == 1);

  draft::DiagnosticSink before_failure_diagnostics;
  const draft::ResolutionManifestLoadResult before_failure =
      draft::load_resolution_manifest(
          workspace.root, app_store_key(), before_failure_diagnostics);
  EXPECT(state,
      before_failure.state == draft::ResolutionManifestLoadState::Loaded);
  const std::string committed_manifest =
      draft::serialize_resolution_manifest(before_failure.manifest);

  // Repeatedly invalid provider proposals exhaust the compiler-check budget.
  // The previously committed manifest remains byte-for-byte authoritative.
  workspace.write_source("changed for invalid proposal");
  provider.response = "judge \"not an expression\";";
  draft::SourceManager failure_sources;
  draft::DiagnosticSink failure_diagnostics;
  const draft::ResolveWorkspaceResult failure = draft::resolve_workspace(
      failure_sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      failure_diagnostics);
  EXPECT(state, !failure.ok);
  EXPECT(state, !failure.committed);
  EXPECT(state, failure_diagnostics.has_errors());
  EXPECT(state, provider.calls == 3);
  const std::string failure_rendering =
      draft::render_diagnostics(failure_sources, failure_diagnostics);
  EXPECT(state,
      failure_rendering.find("exhausted 2 compiler-checked proposal") !=
          std::string::npos);
  EXPECT(state, provider.rejection_histories.size() == 3);
  if (provider.rejection_histories.size() == 3) {
    EXPECT(state, provider.rejection_histories[1].empty());
    EXPECT(state, provider.rejection_histories[2].size() == 1);
  }

  draft::DiagnosticSink after_failure_diagnostics;
  const draft::ResolutionManifestLoadResult after_failure =
      draft::load_resolution_manifest(
          workspace.root, app_store_key(), after_failure_diagnostics);
  EXPECT(state,
      after_failure.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state,
      draft::serialize_resolution_manifest(after_failure.manifest) ==
      committed_manifest);
}

void test_body_proposal_reuses_static_pack_dependency(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_body_site_with_static_pack_dependency();
  FakeProviderState provider;
  provider.response = "42";

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources, workspace.package.string(), resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 1);
  EXPECT(state, provider.calls == 1);
  EXPECT(state, resolved.compiled_program.has_value());
  if (!resolved.compiled_program.has_value())
    return;

  const draft::CompiledPackage *formatting = nullptr;
  for (const std::optional<draft::CompiledPackage> &package :
       resolved.compiled_program->packages) {
    if (package.has_value() &&
        package->identity.root_relative_path == "formatting") {
      formatting = &*package;
      break;
    }
  }
  EXPECT(state, formatting != nullptr);
  if (formatting == nullptr)
    return;
  EXPECT(state, formatting->declarations.package.parametric_instances.empty());
  EXPECT(state, formatting->bodies.package.parametric_instances.size() == 1);
  EXPECT(state, formatting->semantic_progress ==
                    draft::PackageSemanticProgress::ClosureReady);
}

void test_selective_regeneration_changes_only_selected_source(
    TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_two_expression_source();
  FakeProviderState provider;

  draft::SourceManager first_sources;
  draft::DiagnosticSink first_diagnostics;
  const draft::ResolveWorkspaceResult first = draft::resolve_workspace(
      first_sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      first_diagnostics);
  EXPECT(state, first.ok);
  EXPECT(state, first.synthesized_sites == 2);
  EXPECT(state, provider.calls == 2);
  EXPECT(state, provider.site_identities.size() == 2);
  if (!first.ok || provider.site_identities.size() != 2) return;

  const std::string selected_site = provider.site_identities[0];
  const std::string untouched_site = provider.site_identities[1];
  draft::Sha256Digest untouched_expansion;
  for (const draft::ResolutionPin &pin : first.manifest.pins) {
    if (pin.site_identity == untouched_site) {
      untouched_expansion = pin.expansion_digest;
    }
  }

  provider.response = "41";
  draft::ResolveWorkspaceOptions regenerate =
      resolve_options(workspace, provider);
  regenerate.regenerate = true;
  regenerate.regeneration_site_identities.push_back(selected_site);
  draft::SourceManager regenerated_sources;
  draft::DiagnosticSink regenerated_diagnostics;
  const draft::ResolveWorkspaceResult regenerated = draft::resolve_workspace(
      regenerated_sources,
      workspace.package.string(),
      std::move(regenerate),
      regenerated_diagnostics);
  EXPECT(state, regenerated.ok);
  EXPECT(state, regenerated.committed);
  EXPECT(state, regenerated.synthesized_sites == 1);
  EXPECT(state, regenerated.regenerated_sites == 1);
  EXPECT(state, regenerated.reused_sites == 1);
  EXPECT(state, provider.calls == 3);
  EXPECT(state,
      regenerated.manifest.resolved_program_digest !=
          first.manifest.resolved_program_digest);
  for (const draft::ResolutionPin &pin : regenerated.manifest.pins) {
    if (pin.site_identity == selected_site) {
      EXPECT(state, pin.expansion_digest == draft::sha256("41"));
    } else if (pin.site_identity == untouched_site) {
      EXPECT(state, pin.expansion_digest == untouched_expansion);
    }
  }

  // A selector typo is a failed source transaction, not an all-fresh no-op.
  // It must not call the provider or disturb the last committed manifest.
  draft::ResolveWorkspaceOptions unmatched =
      resolve_options(workspace, provider);
  unmatched.regenerate = true;
  unmatched.regeneration_site_identities.push_back("site-does-not-exist");
  draft::SourceManager unmatched_sources;
  draft::DiagnosticSink unmatched_diagnostics;
  const draft::ResolveWorkspaceResult unmatched_result =
      draft::resolve_workspace(
          unmatched_sources,
          workspace.package.string(),
          std::move(unmatched),
          unmatched_diagnostics);
  EXPECT(state, !unmatched_result.ok);
  EXPECT(state, !unmatched_result.committed);
  EXPECT(state, provider.calls == 3);
  EXPECT(state,
      draft::render_diagnostics(unmatched_sources, unmatched_diagnostics).find(
          "selector did not match") != std::string::npos);
  draft::DiagnosticSink persisted_diagnostics;
  const draft::ResolutionManifestLoadResult persisted =
      draft::load_resolution_manifest(
          workspace.root, app_store_key(), persisted_diagnostics);
  EXPECT(state,
      persisted.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state,
      persisted.manifest.resolved_program_digest ==
          regenerated.manifest.resolved_program_digest);
}

void test_compiler_rejection_retries_with_feedback(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_source("correct the typed expression");
  FakeProviderState provider;
  provider.correct_after_rejection = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 1);
  EXPECT(state, provider.calls == 2);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, provider.rejection_histories.size() == 2);
  if (provider.rejection_histories.size() == 2) {
    EXPECT(state, provider.rejection_histories[0].empty());
    EXPECT(state, provider.rejection_histories[1].size() == 1);
    if (provider.rejection_histories[1].size() == 1) {
      const draft::SynthesisRejection &rejection =
          provider.rejection_histories[1][0];
      EXPECT(state, rejection.attempt == 1);
      EXPECT(state, rejection.source == "\"not an i64\"");
      EXPECT(state,
          rejection.diagnostics.find("error") != std::string::npos);
      EXPECT(state,
          rejection.diagnostics.find("generated from synthesis site") !=
              std::string::npos);
    }
  }

  EXPECT(state, resolved.manifest.pins.size() == 1);
  if (resolved.manifest.pins.size() == 1) {
    std::string accepted_source;
    draft::DiagnosticSink load_diagnostics;
    EXPECT(state,
        draft::load_generated_expansion(
            workspace.root,
            resolved.manifest.pins[0].expansion_digest,
            accepted_source,
            load_diagnostics));
    EXPECT(state, accepted_source == "42");
    EXPECT(state, !load_diagnostics.has_errors());
  }
}

void test_provider_error_cannot_hide_behind_success(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_source("provider contradiction");
  FakeProviderState provider;
  provider.report_error_and_succeed = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  EXPECT(state, !resolved.ok);
  EXPECT(state, !resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 0);
  EXPECT(state, provider.calls == 1);
  EXPECT(state,
      draft::render_diagnostics(sources, diagnostics).find(
          "provider reported an error with a successful return") !=
          std::string::npos);
}

void test_external_inputs_commit_without_synthesis(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_complete_source();
  FakeProviderState provider;
  draft::ResolveWorkspaceOptions options = resolve_options(workspace, provider);
  options.provider = {};
  options.external_inputs_configured = true;
  options.external_inputs.push_back(fake_runtime_asset_pin());

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      std::move(options),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.manifest.pins.empty());
  EXPECT(state, resolved.manifest.external_inputs.size() == 1);
  EXPECT(state, provider.calls == 0);

  draft::DiagnosticSink loaded_diagnostics;
  const draft::ResolutionManifestLoadResult loaded =
      draft::load_resolution_manifest(
          workspace.root, app_store_key(), loaded_diagnostics);
  EXPECT(state,
      loaded.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state, loaded.manifest.external_inputs.size() == 1);

  // The provider-free compiler still verifies the coherent program identity
  // when a manifest exists solely to lock external build inputs.
  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          compile_options(workspace),
          offline_diagnostics);
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());
}

void test_interface_sites_precede_dependent_bodies(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_staged_source();
  FakeProviderState provider;
  provider.staged_responses = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  draft::ResolveWorkspaceOptions options = resolve_options(workspace, provider);
  options.provider.prepare = prepare_fake_provider;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      std::move(options),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 4);
  EXPECT(state, provider.calls == 4);
  EXPECT(state, provider.preparation_calls == 1);
  EXPECT(state, provider.kinds.size() == 4);
  if (provider.kinds.size() == 4) {
    EXPECT(state, provider.kinds[0] ==
        draft::AgentConstructKind::SynthesisDeclaration);
    EXPECT(state, provider.kinds[1] ==
        draft::AgentConstructKind::SynthesisMember);
    EXPECT(state, provider.kinds[2] ==
        draft::AgentConstructKind::SynthesisExpression);
    EXPECT(state, provider.kinds[3] ==
        draft::AgentConstructKind::SynthesisStatement);
  }

  // The committed result must be consumable by the provider-free compiler,
  // which has to reproduce the same interface/body staging from stored pins.
  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  draft::CompileWorkspaceOptions offline_options = compile_options(workspace);
  offline_options.lower_mir = true;
  offline_options.emit_llvm = true;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          offline_options,
          offline_diagnostics);
  if (!offline.ok) {
    std::cerr << draft::render_diagnostics(
        offline_sources, offline_diagnostics);
  }
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());
  EXPECT(state, resolved.manifest.format == "draft-resolution-v6");
  EXPECT(state, resolved.manifest.pins.size() == 4);
  std::size_t composed_maps = 0;
  for (const draft::WorkspacePackage &package : offline.graph.packages) {
    for (const draft::LoadedPackageFile &file : package.loaded.files) {
      const std::vector<draft::SourceExpansionMap> &maps =
          offline_sources.file(file.source).expansion_maps;
      composed_maps += maps.size();
      for (std::size_t index = 1; index < maps.size(); ++index) {
        EXPECT(state, maps[index - 1].generated_end <=
            maps[index].generated_begin);
      }
    }
  }
  EXPECT(state, composed_maps == 4);

#if defined(__APPLE__)
  // The provider is no longer in scope: link the stored resolved program twice,
  // require byte-identical executables, then launch one. This is the literal
  // native acceptance path for declaration/member/expression/body synthesis.
  const std::filesystem::path native_root = workspace.root / "native-acceptance";
  draft::NativeBuildOptions first_native;
  first_native.build_directory = (native_root / "first-build").string();
  first_native.output_path = (native_root / "first-program").string();
  const draft::NativeBuildResult first_built = draft::build_native_executable(
      draft::make_aarch64_macos_profile(),
      offline,
      first_native,
      offline_diagnostics);
  EXPECT(state, first_built.ok);
  const std::string first_bytes = first_built.ok
      ? read_binary_file(first_built.output_path)
      : std::string();
  if (first_built.ok) {
    // The final native gate must retain generated-source identity through both
    // public correlation surfaces. The JSON map is intended for profilers and
    // coverage ingestion; the linked DWARF labels let ordinary native tools
    // recover the same persistent synthesis site from the dSYM companion.
    const std::string correlation =
        read_binary_file(first_built.source_correlation_path);
    const std::filesystem::path dwarf_payload =
        std::filesystem::path(first_built.debug_symbols_path) /
        "Contents" / "Resources" / "DWARF" /
        std::filesystem::path(first_built.output_path).filename();
    const std::string linked_dwarf = read_binary_file(dwarf_payload);
    EXPECT(state, !correlation.empty());
    EXPECT(state, !linked_dwarf.empty());

    std::size_t executable_generated_sites = 0;
    for (const draft::ResolutionPin &pin : resolved.manifest.pins) {
      if (pin.kind != draft::AgentConstructKind::SynthesisExpression &&
          pin.kind != draft::AgentConstructKind::SynthesisStatement) {
        continue;
      }
      ++executable_generated_sites;
      EXPECT(state, correlation.find(pin.site_identity) != std::string::npos);
      EXPECT(state, linked_dwarf.find(pin.site_identity) != std::string::npos);
    }
    EXPECT(state, executable_generated_sites == 2);
  }
  // Output path is part of native artifact identity. Rebuild exactly the same
  // provider-free path so the comparison does not conflate program identity
  // with a changed install name or linker output name.
  draft::NativeBuildOptions second_native = first_native;
  const draft::NativeBuildResult second_built = draft::build_native_executable(
      draft::make_aarch64_macos_profile(),
      offline,
      second_native,
      offline_diagnostics);
  EXPECT(state, second_built.ok);
  if (first_built.ok && second_built.ok) {
    const std::string second_bytes = read_binary_file(second_built.output_path);
    EXPECT(state, !first_bytes.empty());
    EXPECT(state, first_bytes == second_bytes);
    int process_status = 0;
    EXPECT(state,
        run_executable(first_built.output_path, native_root, process_status));
    EXPECT(state, WIFEXITED(process_status));
    if (WIFEXITED(process_status)) {
      EXPECT(state, WEXITSTATUS(process_status) == 0);
    }
  }
#endif

  draft::SourceManager reuse_sources;
  draft::DiagnosticSink reuse_diagnostics;
  const draft::ResolveWorkspaceResult reused = draft::resolve_workspace(
      reuse_sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      reuse_diagnostics);
  EXPECT(state, reused.ok);
  EXPECT(state, reused.synthesized_sites == 0);
  EXPECT(state, reused.reused_sites == 4);
  EXPECT(state, provider.calls == 4);
}

void test_dependency_interface_rounds(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_dependency_staged_source();
  FakeProviderState provider;
  provider.staged_responses = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.synthesized_sites == 1);
  EXPECT(state, provider.calls == 1);
  EXPECT(state, provider.kinds.size() == 1);
  if (provider.kinds.size() == 1) {
    EXPECT(state, provider.kinds.front() ==
        draft::AgentConstructKind::SynthesisDeclaration);
  }

  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  draft::CompileWorkspaceOptions offline_options = compile_options(workspace);
  offline_options.lower_mir = true;
  offline_options.emit_llvm = true;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          offline_options,
          offline_diagnostics);
  if (!offline.ok) {
    std::cerr << draft::render_diagnostics(
        offline_sources, offline_diagnostics);
  }
  EXPECT(state, offline.ok);
  EXPECT(state, offline.packages.size() == 2);
  EXPECT(state, !offline_diagnostics.has_errors());
}

[[nodiscard]] bool contains_name(
    const std::vector<std::string> &names,
    std::string_view expected) {
  for (const std::string &name : names) {
    if (name == expected) return true;
  }
  return false;
}

void test_same_interface_set_is_opaque(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_opaque_interface_set_source();
  FakeProviderState provider;
  provider.opaque_interface_responses = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 2);
  EXPECT(state, provider.calls == 2);
  EXPECT(state, provider.kinds.size() == 2);
  EXPECT(state, provider.visible_binding_names.size() == 2);
  if (provider.kinds.size() == 2) {
    EXPECT(state, provider.kinds[0] ==
        draft::AgentConstructKind::SynthesisDeclaration);
    EXPECT(state, provider.kinds[1] ==
        draft::AgentConstructKind::SynthesisDeclaration);
  }
  if (provider.visible_binding_names.size() == 2) {
    // The strongest useful assertion is symmetric: request order must not make
    // the first proposal visible to the second request, and source order must
    // not invent the second declaration for the first request.
    EXPECT(state,
        !contains_name(provider.visible_binding_names[0], "first"));
    EXPECT(state,
        !contains_name(provider.visible_binding_names[0], "second"));
    EXPECT(state,
        !contains_name(provider.visible_binding_names[1], "first"));
    EXPECT(state,
        !contains_name(provider.visible_binding_names[1], "second"));
  }

  // The opaque proposals are merged only after both requests return. Their
  // combined package must then pass the normal provider-free complete compile.
  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          compile_options(workspace),
          offline_diagnostics);
  if (!offline.ok) {
    std::cerr << draft::render_diagnostics(
        offline_sources, offline_diagnostics);
  }
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());
}

void test_compile_time_body_dependency_precedes_selected_declaration(
    TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_compile_time_dependency_source();
  FakeProviderState provider;
  provider.staged_responses = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 2);
  EXPECT(state, provider.calls == 2);
  EXPECT(state, provider.kinds.size() == 2);
  if (provider.kinds.size() == 2) {
    EXPECT(state, provider.kinds[0] ==
        draft::AgentConstructKind::SynthesisExpression);
    EXPECT(state, provider.kinds[1] ==
        draft::AgentConstructKind::SynthesisDeclaration);
  }
  EXPECT(state, provider.expected_type_texts.size() == 2);
  EXPECT(state, provider.anchor_names.size() == 2);
  if (provider.expected_type_texts.size() == 2) {
    EXPECT(state, provider.expected_type_texts[0] == "i64");
  }
  if (provider.anchor_names.size() == 2) {
    EXPECT(state, provider.anchor_names[0] == "compile_value");
  }

  // Offline replay must reproduce both interface rounds using only committed
  // expansion bytes. If it classifies the expression as an ordinary late body
  // site, the `when` condition cannot select the generated declaration.
  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          compile_options(workspace),
          offline_diagnostics);
  if (!offline.ok) {
    std::cerr << draft::render_diagnostics(
        offline_sources, offline_diagnostics);
  }
  EXPECT(state, offline.ok);
  EXPECT(state, offline.resolution_manifest.has_value());
  EXPECT(state, !offline_diagnostics.has_errors());
}

void test_direct_when_synthesis_precedes_selected_declaration(
    TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_direct_condition_dependency_source();
  FakeProviderState provider;
  provider.staged_responses = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 2);
  EXPECT(state, provider.kinds.size() == 2);
  if (provider.kinds.size() == 2) {
    EXPECT(state, provider.kinds[0] ==
        draft::AgentConstructKind::SynthesisExpression);
    EXPECT(state, provider.kinds[1] ==
        draft::AgentConstructKind::SynthesisDeclaration);
  }
  EXPECT(state, provider.expected_type_texts.size() == 2);
  if (provider.expected_type_texts.size() == 2) {
    EXPECT(state, provider.expected_type_texts[0] == "bool");
  }
  EXPECT(state, provider.anchor_names.size() == 2);
  if (provider.anchor_names.size() == 2) {
    EXPECT(state, provider.anchor_names[0].empty());
  }

  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          compile_options(workspace),
          offline_diagnostics);
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());
}

void test_structural_set_precedes_compile_time_body_dependency(
    TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_structural_then_compile_time_dependency_source();
  FakeProviderState provider;
  provider.staged_responses = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 3);
  EXPECT(state, provider.kinds.size() == 3);
  if (provider.kinds.size() == 3) {
    EXPECT(state, provider.kinds[0] ==
        draft::AgentConstructKind::SynthesisDeclaration);
    EXPECT(state, provider.kinds[1] ==
        draft::AgentConstructKind::SynthesisExpression);
    EXPECT(state, provider.kinds[2] ==
        draft::AgentConstructKind::SynthesisDeclaration);
  }
  EXPECT(state, provider.prompts.size() == 3);
  if (provider.prompts.size() == 3) {
    EXPECT(state, provider.prompts[0] == "declare compile input");
    EXPECT(state, provider.prompts[1] == "compute compile-time increment");
    EXPECT(state, provider.prompts[2] == "declare public answer");
  }
  EXPECT(state, provider.occurrences.size() == 3);
  if (provider.occurrences.size() == 3) {
    EXPECT(state, provider.occurrences[0] == 0);
    EXPECT(state, provider.occurrences[1] == 0);
    EXPECT(state, provider.occurrences[2] == 1);
  }
  if (provider.visible_binding_names.size() == 3) {
    EXPECT(state,
        contains_name(provider.visible_binding_names[1], "generated_value"));
  }

  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          compile_options(workspace),
          offline_diagnostics);
  if (!offline.ok) {
    std::cerr << draft::render_diagnostics(
        offline_sources, offline_diagnostics);
  }
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());
}

void test_direct_synthesis_resolves_type_layout(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_direct_layout_dependency_source();
  FakeProviderState provider;
  provider.staged_responses = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 1);
  EXPECT(state, provider.kinds.size() == 1);
  EXPECT(state, provider.expected_type_texts.size() == 1);
  EXPECT(state, provider.anchor_names.size() == 1);
  if (provider.kinds.size() == 1) {
    EXPECT(state, provider.kinds[0] ==
        draft::AgentConstructKind::SynthesisExpression);
  }
  if (provider.expected_type_texts.size() == 1) {
    EXPECT(state, provider.expected_type_texts[0] == "usize");
  }
  if (provider.anchor_names.size() == 1) {
    EXPECT(state, provider.anchor_names[0] == "Buffer");
  }

  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          compile_options(workspace),
          offline_diagnostics);
  if (!offline.ok) {
    std::cerr << draft::render_diagnostics(
        offline_sources, offline_diagnostics);
  }
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());
}

void test_procedure_synthesis_resolves_type_layout(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_procedure_layout_dependency_source();
  FakeProviderState provider;
  provider.staged_responses = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 1);
  EXPECT(state, provider.kinds.size() == 1);
  EXPECT(state, provider.expected_type_texts.size() == 1);
  EXPECT(state, provider.anchor_names.size() == 1);
  if (provider.kinds.size() == 1) {
    EXPECT(state, provider.kinds[0] ==
        draft::AgentConstructKind::SynthesisExpression);
  }
  if (provider.expected_type_texts.size() == 1) {
    EXPECT(state, provider.expected_type_texts[0] == "usize");
  }
  if (provider.anchor_names.size() == 1) {
    EXPECT(state, provider.anchor_names[0] == "compile_length");
  }

  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          compile_options(workspace),
          offline_diagnostics);
  if (!offline.ok) {
    std::cerr << draft::render_diagnostics(
        offline_sources, offline_diagnostics);
  }
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());
}

void test_synthesis_resolves_integer_recipe_boundaries(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_integer_recipe_boundaries_source();
  FakeProviderState provider;
  provider.staged_responses = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 4);
  EXPECT(state, provider.kinds.size() == 4);
  EXPECT(state, provider.expected_type_texts.size() == 4);
  EXPECT(state, provider.anchor_names.size() == 4);
  if (provider.kinds.size() == 4) {
    for (draft::AgentConstructKind kind : provider.kinds) {
      EXPECT(state, kind == draft::AgentConstructKind::SynthesisExpression);
    }
  }
  if (provider.expected_type_texts.size() == 4) {
    EXPECT(state, provider.expected_type_texts[0] == "usize");
    EXPECT(state, provider.expected_type_texts[1] == "usize");
    EXPECT(state, provider.expected_type_texts[2] == "usize");
    EXPECT(state, provider.expected_type_texts[3] == "u8");
  }
  if (provider.anchor_names.size() == 4) {
    EXPECT(state, provider.anchor_names[0] == "Applied");
    EXPECT(state, provider.anchor_names[1] == "Aligned");
    EXPECT(state, provider.anchor_names[2] == "Vector");
    EXPECT(state, provider.anchor_names[3] == "Mode");
  }

  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          compile_options(workspace),
          offline_diagnostics);
  if (!offline.ok) {
    std::cerr << draft::render_diagnostics(
        offline_sources, offline_diagnostics);
  }
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());
}

void test_dependency_waits_for_synthesized_public_layout(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_dependency_layout_source();
  FakeProviderState provider;
  provider.staged_responses = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 1);
  EXPECT(state, provider.expected_type_texts.size() == 1);
  if (provider.expected_type_texts.size() == 1) {
    EXPECT(state, provider.expected_type_texts[0] == "usize");
  }

  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          compile_options(workspace),
          offline_diagnostics);
  if (!offline.ok) {
    std::cerr << draft::render_diagnostics(
        offline_sources, offline_diagnostics);
  }
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());
}

void test_independent_member_and_compile_time_sites_share_round(
    TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_independent_member_and_compile_time_source();

  // Inspect the first provider-free discovery surface directly. Seeing both
  // rows here proves the expression was not serialized behind the unrelated
  // aggregate member completion set.
  draft::CompileWorkspaceOptions discovery_options = compile_options(workspace);
  discovery_options.stage =
      draft::CompileWorkspaceStage::DiscoverInterfaceSynthesis;
  draft::SourceManager discovery_sources;
  draft::DiagnosticSink discovery_diagnostics;
  const draft::CompileWorkspaceResult discovery = draft::compile_workspace(
      discovery_sources,
      workspace.package.string(),
      std::move(discovery_options),
      discovery_diagnostics);
  if (!discovery.ok) {
    std::cerr << draft::render_diagnostics(
        discovery_sources, discovery_diagnostics);
  }
  EXPECT(state, discovery.ok);
  if (discovery.graph.root_package.is_valid() &&
      discovery.graph.root_package.value < discovery.packages.size() &&
      discovery.packages[discovery.graph.root_package.value].has_value()) {
    const draft::CompiledPackage &root =
        *discovery.packages[discovery.graph.root_package.value];
    EXPECT(state, root.obligations.obligations.size() == 2);
    if (root.obligations.obligations.size() == 2) {
      EXPECT(state, root.obligations.obligations[0].kind ==
          draft::AgentConstructKind::SynthesisMember);
      EXPECT(state, root.obligations.obligations[1].kind ==
          draft::AgentConstructKind::SynthesisExpression);
    }
  } else {
    EXPECT(state, false);
  }

  FakeProviderState provider;
  provider.staged_responses = true;
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 3);
  EXPECT(state, provider.kinds.size() == 3);
  if (provider.kinds.size() == 3) {
    EXPECT(state, provider.kinds[0] ==
        draft::AgentConstructKind::SynthesisMember);
    EXPECT(state, provider.kinds[1] ==
        draft::AgentConstructKind::SynthesisExpression);
    EXPECT(state, provider.kinds[2] ==
        draft::AgentConstructKind::SynthesisDeclaration);
  }

  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          compile_options(workspace),
          offline_diagnostics);
  if (!offline.ok) {
    std::cerr << draft::render_diagnostics(
        offline_sources, offline_diagnostics);
  }
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());
}

void test_member_dependent_compile_time_site_waits_for_next_round(
    TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_dependent_member_and_compile_time_source();

  draft::CompileWorkspaceOptions discovery_options = compile_options(workspace);
  discovery_options.stage =
      draft::CompileWorkspaceStage::DiscoverInterfaceSynthesis;
  draft::SourceManager discovery_sources;
  draft::DiagnosticSink discovery_diagnostics;
  const draft::CompileWorkspaceResult discovery = draft::compile_workspace(
      discovery_sources,
      workspace.package.string(),
      std::move(discovery_options),
      discovery_diagnostics);
  if (!discovery.ok) {
    std::cerr << draft::render_diagnostics(
        discovery_sources, discovery_diagnostics);
  }
  EXPECT(state, discovery.ok);
  if (discovery.graph.root_package.is_valid() &&
      discovery.graph.root_package.value < discovery.packages.size() &&
      discovery.packages[discovery.graph.root_package.value].has_value()) {
    const draft::CompiledPackage &root =
        *discovery.packages[discovery.graph.root_package.value];
    EXPECT(state, root.obligations.obligations.size() == 1);
    if (root.obligations.obligations.size() == 1) {
      EXPECT(state, root.obligations.obligations[0].kind ==
          draft::AgentConstructKind::SynthesisMember);
    }
  } else {
    EXPECT(state, false);
  }

  FakeProviderState provider;
  provider.staged_responses = true;
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 3);
  EXPECT(state, provider.kinds.size() == 3);
  if (provider.kinds.size() == 3) {
    EXPECT(state, provider.kinds[0] ==
        draft::AgentConstructKind::SynthesisMember);
    EXPECT(state, provider.kinds[1] ==
        draft::AgentConstructKind::SynthesisExpression);
    EXPECT(state, provider.kinds[2] ==
        draft::AgentConstructKind::SynthesisDeclaration);
  }

  draft::SourceManager offline_sources;
  draft::DiagnosticSink offline_diagnostics;
  const draft::CompileWorkspaceResult offline =
      draft::compile_workspace_with_resolution(
          offline_sources,
          workspace.package.string(),
          compile_options(workspace),
          offline_diagnostics);
  if (!offline.ok) {
    std::cerr << draft::render_diagnostics(
        offline_sources, offline_diagnostics);
  }
  EXPECT(state, offline.ok);
  EXPECT(state, !offline_diagnostics.has_errors());
}

void test_resolution_keeps_validation_as_context_only(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_source("candidate with tests");
  workspace.write_test_source();
  workspace.write_benchmark_source();
  FakeProviderState provider;

  // Tests and benchmarks remain typed provider context, but resolution does
  // not execute them or make their evidence a source-commit prerequisite.
  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved =
      draft::resolve_workspace(
          sources,
          workspace.package.string(),
          resolve_options(workspace, provider),
          diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, !diagnostics.has_errors());
  EXPECT(state, provider.last_validation_context.size() == 2);
  if (provider.last_validation_context.size() == 2) {
    const draft::AgentValidationContext &benchmark =
        provider.last_validation_context[0];
    const draft::AgentValidationContext &test =
        provider.last_validation_context[1];
    EXPECT(state, benchmark.kind == "benchmark");
    EXPECT(state,
        benchmark.source_relative_path == "candidate_bench.draft");
    EXPECT(state,
        benchmark.source.find("bench_generated_answer") !=
            std::string::npos);
    EXPECT(state, draft::sha256(benchmark.source) == benchmark.source_digest);
    EXPECT(state, benchmark.typing_complete);
    EXPECT(state, benchmark.procedures.size() == 1);
    if (benchmark.procedures.size() == 1) {
      const draft::AgentValidationProcedureContext &procedure =
          benchmark.procedures.front();
      EXPECT(state, procedure.name == "bench_generated_answer");
      EXPECT(state, !procedure.type_text.empty());
      EXPECT(state,
          draft::sha256(procedure.type_definition) ==
              procedure.type_definition_digest);
      EXPECT(state, procedure.state_size > 0);
      EXPECT(state, procedure.state_alignment > 0);
      EXPECT(state, procedure.report_size >= procedure.failure_offset);
    }
    EXPECT(state, test.kind == "test");
    EXPECT(state, test.source_relative_path == "candidate_test.draft");
    EXPECT(state,
        test.source.find("test_generated_answer") != std::string::npos);
    EXPECT(state,
        test.source.find("Validation comments") == std::string::npos);
    EXPECT(state, draft::sha256(test.source) == test.source_digest);
    EXPECT(state, test.typing_complete);
    EXPECT(state, test.procedures.size() == 1);
    if (test.procedures.size() == 1) {
      const draft::AgentValidationProcedureContext &procedure =
          test.procedures.front();
      EXPECT(state, procedure.name == "test_generated_answer");
      EXPECT(state, !procedure.type_text.empty());
      EXPECT(state,
          draft::sha256(procedure.type_definition) ==
              procedure.type_definition_digest);
      EXPECT(state, procedure.state_size > 0);
      bool saw_answer = false;
      bool saw_expect = false;
      bool saw_test_parameter = false;
      for (const draft::AgentValidationReferenceContext &reference :
           procedure.references) {
        EXPECT(state,
            draft::sha256(reference.type_definition) ==
                reference.type_definition_digest);
        if (reference.name == "answer" &&
            reference.root_identity == "workspace" &&
            reference.root_relative_path == "app") {
          saw_answer = true;
        }
        if (reference.name == "expect" &&
            reference.root_identity == "draft-core-bootstrap-v4" &&
            reference.root_relative_path == "testing") {
          saw_expect = true;
        }
        if (reference.name == "test") saw_test_parameter = true;
      }
      EXPECT(state, saw_answer);
      EXPECT(state, saw_expect);
      EXPECT(state, saw_test_parameter);
    }
  }
  EXPECT(state, !provider.visible_binding_names.empty());
  if (!provider.visible_binding_names.empty()) {
    const std::vector<std::string> &ordinary_names =
        provider.visible_binding_names.back();
    EXPECT(state,
        std::find(
            ordinary_names.begin(), ordinary_names.end(), "testing") ==
            ordinary_names.end());
    EXPECT(state,
        std::find(
            ordinary_names.begin(), ordinary_names.end(), "expect") ==
            ordinary_names.end());
    EXPECT(state,
        std::find(
            ordinary_names.begin(),
            ordinary_names.end(),
            "test_generated_answer") == ordinary_names.end());
  }
  draft::DiagnosticSink manifest_diagnostics;
  const draft::ResolutionManifestLoadResult manifest =
      draft::load_resolution_manifest(
          workspace.root, app_store_key(), manifest_diagnostics);
  EXPECT(state,
      manifest.state == draft::ResolutionManifestLoadState::Loaded);
  EXPECT(state, !manifest_diagnostics.has_errors());
}

void test_invalid_validation_context_stops_before_provider(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_source("must not reach provider");
  workspace.write_invalid_test_source();
  FakeProviderState provider;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      diagnostics);
  EXPECT(state, !resolved.ok);
  EXPECT(state, !resolved.committed);
  EXPECT(state, provider.calls == 0);
  EXPECT(state,
      draft::render_diagnostics(sources, diagnostics).find(
          "must return void") != std::string::npos);
}

void test_validation_context_stales_synthesis(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_source("validation context freshness");
  workspace.write_test_source();
  FakeProviderState provider;

  draft::SourceManager first_sources;
  draft::DiagnosticSink first_diagnostics;
  const draft::ResolveWorkspaceResult first = draft::resolve_workspace(
      first_sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      first_diagnostics);
  if (!first.ok) {
    std::cerr << draft::render_diagnostics(first_sources, first_diagnostics);
  }
  EXPECT(state, first.ok);
  EXPECT(state, first.synthesized_sites == 1);
  EXPECT(state, provider.calls == 1);

  // The surface program and author prompt are unchanged. Altering a selected
  // test statement alone must change the obligation and force a new proposal;
  // otherwise a pin could survive after its authoritative acceptance context
  // changed.
  workspace.write_test_source("testing.expect(test, answer() >= 0)");
  draft::SourceManager changed_sources;
  draft::DiagnosticSink changed_diagnostics;
  const draft::ResolveWorkspaceResult changed = draft::resolve_workspace(
      changed_sources,
      workspace.package.string(),
      resolve_options(workspace, provider),
      changed_diagnostics);
  if (!changed.ok) {
    std::cerr << draft::render_diagnostics(
        changed_sources, changed_diagnostics);
  }
  EXPECT(state, changed.ok);
  EXPECT(state, changed.synthesized_sites == 1);
  EXPECT(state, changed.reused_sites == 0);
  EXPECT(state, provider.calls == 2);
}

void test_cancelled_resolution_does_not_start_transaction(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_source("must not reach provider");
  FakeProviderState provider;
  bool cancelled = true;
  draft::ResolveWorkspaceOptions options = resolve_options(workspace, provider);
  options.cancellation_state = &cancelled;
  options.cancellation_requested = boolean_cancellation_requested;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      std::move(options),
      diagnostics);
  EXPECT(state, !resolved.ok);
  EXPECT(state, !resolved.committed);
  EXPECT(state, provider.calls == 0);
  EXPECT(state, diagnostics.error_count() == 1);
  if (!diagnostics.diagnostics().empty()) {
    EXPECT(state,
        diagnostics.diagnostics().front().message == "resolution cancelled");
  }
  draft::DiagnosticSink manifest_diagnostics;
  const draft::ResolutionManifestLoadResult manifest =
      draft::load_resolution_manifest(
          workspace.root, app_store_key(), manifest_diagnostics);
  EXPECT(state, manifest.state == draft::ResolutionManifestLoadState::Missing);
}

void test_cancellation_stops_queued_provider_calls(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_two_expression_source();
  std::atomic_bool cancelled = false;
  CancellingProviderState provider;
  provider.cancelled = &cancelled;
  draft::TimingRecorder timings(draft::TimingOutput::All);
  draft::ResolveWorkspaceOptions options;
  options.compile = compile_options(workspace);
  options.compile.timings = &timings;
  options.provider.provider_identity = "cancelling-fake-provider-v1";
  options.provider.model_identity = "fixture-model-v1";
  options.provider.configuration_identity = "cancelling-fixture-v1";
  options.provider.state = &provider;
  options.provider.synthesize = synthesize_then_cancel;
  options.provider.maximum_parallel_calls = 1;
  options.cancellation_state = &cancelled;
  options.cancellation_requested = atomic_boolean_cancellation_requested;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      std::move(options),
      diagnostics);
  EXPECT(state, !resolved.ok);
  EXPECT(state, !resolved.committed);
  EXPECT(state, provider.calls.load(std::memory_order_relaxed) == 1);
  EXPECT(state,
      draft::render_diagnostics(sources, diagnostics).find(
          "resolution cancelled") != std::string::npos);
  const std::string timing_report = timings.render();
  EXPECT(state,
      timing_report.find("synthesis provider ready waves: 1") !=
          std::string::npos);
  EXPECT(state,
      timing_report.find("synthesis provider calls: 1") != std::string::npos);
}

void test_ready_provider_calls_and_corrections_overlap(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_two_expression_source();
  ParallelProviderState provider;
  draft::TimingRecorder timings(draft::TimingOutput::All);
  draft::ResolveWorkspaceOptions options =
      parallel_resolve_options(workspace, provider);
  options.compile.timings = &timings;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      std::move(options),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 2);
  EXPECT(state,
      provider.first_attempt_calls.load(std::memory_order_relaxed) == 2);
  EXPECT(state,
      provider.correction_attempt_calls.load(std::memory_order_relaxed) == 2);
  EXPECT(state,
      provider.first_attempt_maximum.load(std::memory_order_relaxed) == 2);
  EXPECT(state,
      provider.correction_attempt_maximum.load(std::memory_order_relaxed) == 2);
  const std::string timing_report = timings.render();
  EXPECT(state,
      timing_report.find("synthesis provider ready waves: 2") !=
          std::string::npos);
  EXPECT(state,
      timing_report.find("synthesis provider calls: 4") != std::string::npos);
}

void test_accepted_site_leaves_correction_wave(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_two_expression_source();
  ParallelProviderState provider;
  provider.one_site_accepts_first = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      parallel_resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.synthesized_sites == 2);
  EXPECT(state,
      provider.first_attempt_calls.load(std::memory_order_relaxed) == 2);
  EXPECT(state,
      provider.first_attempt_maximum.load(std::memory_order_relaxed) == 2);
  EXPECT(state,
      provider.correction_attempt_calls.load(std::memory_order_relaxed) == 1);
  EXPECT(state,
      provider.correction_attempt_maximum.load(std::memory_order_relaxed) == 1);
}

void test_concurrent_interface_set_remains_opaque(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_opaque_interface_set_source();
  ParallelProviderState provider;
  provider.interface_mode = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      parallel_resolve_options(workspace, provider),
      diagnostics);
  if (!resolved.ok) {
    std::cerr << draft::render_diagnostics(sources, diagnostics);
  }
  EXPECT(state, resolved.ok);
  EXPECT(state, resolved.committed);
  EXPECT(state, resolved.synthesized_sites == 2);
  EXPECT(state,
      provider.first_attempt_calls.load(std::memory_order_relaxed) == 2);
  EXPECT(state,
      provider.first_attempt_maximum.load(std::memory_order_relaxed) == 2);
  EXPECT(state,
      provider.correction_attempt_calls.load(std::memory_order_relaxed) == 0);
}

void test_parallel_provider_failure_uses_canonical_order(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_two_expression_source();
  ParallelProviderState provider;
  provider.fail = true;

  draft::SourceManager sources;
  draft::DiagnosticSink diagnostics;
  const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
      sources,
      workspace.package.string(),
      parallel_resolve_options(workspace, provider),
      diagnostics);
  EXPECT(state, !resolved.ok);
  EXPECT(state, !resolved.committed);
  EXPECT(state,
      provider.first_attempt_calls.load(std::memory_order_relaxed) == 2);
  EXPECT(state,
      provider.first_attempt_maximum.load(std::memory_order_relaxed) == 2);
  const std::string rendered = draft::render_diagnostics(sources, diagnostics);
  EXPECT(state,
      rendered.find("first provider failure") != std::string::npos);
  EXPECT(state,
      rendered.find("second provider failure") == std::string::npos);
}

void test_invalid_proposal_budget_stops_before_provider(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_source("must not reach provider with invalid budget");
  FakeProviderState provider;

  for (const std::uint32_t invalid_budget : {0U, 9U}) {
    draft::ResolveWorkspaceOptions options =
        resolve_options(workspace, provider);
    options.maximum_proposal_attempts = invalid_budget;
    draft::SourceManager sources;
    draft::DiagnosticSink diagnostics;
    const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
        sources,
        workspace.package.string(),
        std::move(options),
        diagnostics);
    EXPECT(state, !resolved.ok);
    EXPECT(state, !resolved.committed);
    EXPECT(state, diagnostics.error_count() == 1);
    if (!diagnostics.diagnostics().empty()) {
      EXPECT(state,
          diagnostics.diagnostics().front().message.find("between 1 and 8") !=
              std::string::npos);
    }
  }
  EXPECT(state, provider.calls == 0);
}

void test_invalid_provider_parallel_bound_stops_before_call(TestState &state) {
  TemporaryWorkspace workspace;
  workspace.write_source("must not reach provider with invalid worker bound");
  FakeProviderState provider;

  for (const std::size_t invalid_bound : {0U, 65U}) {
    draft::ResolveWorkspaceOptions options =
        resolve_options(workspace, provider);
    options.provider.maximum_parallel_calls = invalid_bound;
    draft::SourceManager sources;
    draft::DiagnosticSink diagnostics;
    const draft::ResolveWorkspaceResult resolved = draft::resolve_workspace(
        sources,
        workspace.package.string(),
        std::move(options),
        diagnostics);
    EXPECT(state, !resolved.ok);
    EXPECT(state, !resolved.committed);
    EXPECT(state, diagnostics.error_count() == 1);
    if (!diagnostics.diagnostics().empty()) {
      EXPECT(state,
          diagnostics.diagnostics().front().message.find("between 1 and 64") !=
              std::string::npos);
    }
  }
  EXPECT(state, provider.calls == 0);
}

} // namespace

int main() {
  TestState state;
  test_resolution_reuse_revalidation_and_failure(state);
  test_body_proposal_reuses_static_pack_dependency(state);
  test_selective_regeneration_changes_only_selected_source(state);
  test_compiler_rejection_retries_with_feedback(state);
  test_provider_error_cannot_hide_behind_success(state);
  test_external_inputs_commit_without_synthesis(state);
  test_interface_sites_precede_dependent_bodies(state);
  test_dependency_interface_rounds(state);
  test_same_interface_set_is_opaque(state);
  test_compile_time_body_dependency_precedes_selected_declaration(state);
  test_direct_when_synthesis_precedes_selected_declaration(state);
  test_structural_set_precedes_compile_time_body_dependency(state);
  test_direct_synthesis_resolves_type_layout(state);
  test_procedure_synthesis_resolves_type_layout(state);
  test_synthesis_resolves_integer_recipe_boundaries(state);
  test_dependency_waits_for_synthesized_public_layout(state);
  test_independent_member_and_compile_time_sites_share_round(state);
  test_member_dependent_compile_time_site_waits_for_next_round(state);
  test_resolution_keeps_validation_as_context_only(state);
  test_invalid_validation_context_stops_before_provider(state);
  test_validation_context_stales_synthesis(state);
  test_cancelled_resolution_does_not_start_transaction(state);
  test_cancellation_stops_queued_provider_calls(state);
  test_ready_provider_calls_and_corrections_overlap(state);
  test_accepted_site_leaves_correction_wave(state);
  test_concurrent_interface_set_remains_opaque(state);
  test_parallel_provider_failure_uses_canonical_order(state);
  test_invalid_proposal_budget_stops_before_provider(state);
  test_invalid_provider_parallel_bound_stops_before_call(state);
  if (state.failures != 0) {
    std::cerr << state.failures << " resolver expectation(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "all resolver tests passed\n";
  return EXIT_SUCCESS;
}
