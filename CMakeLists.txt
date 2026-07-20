cmake_minimum_required(VERSION 3.24)

project(
  Draft
  VERSION 0.1.0
  DESCRIPTION "Bootstrap compiler for the Draft systems language"
  # LLVM's exported CMake package performs C feature checks even though Draft
  # calls its C API from the C++ bootstrap and compiles no repository C source.
  LANGUAGES C CXX
)

# The bootstrap compiler deliberately uses a small, uniform C++20 dialect. We
# disable implementation extensions so a construct accepted by one host compiler
# does not quietly become a portability dependency of the self-hosting path.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DRAFT_WARNINGS_AS_ERRORS "Treat bootstrap compiler warnings as errors" ON)
option(DRAFT_ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)

find_package(Threads REQUIRED)

# The checked-in Draft coding skill is also the Codex synthesis adapter's exact
# language guide. Generate an immutable byte table from an explicit file list so
# installed draftc binaries do not depend on a repository checkout at runtime.
set(DRAFT_SKILL_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.agents/skills/write-draft-code")
set(DRAFT_SKILL_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/generated/draft_skill_bundle.cpp")
set(
  DRAFT_SKILL_INPUTS
  "${DRAFT_SKILL_ROOT}/SKILL.md"
  "${DRAFT_SKILL_ROOT}/agents/openai.yaml"
  "${DRAFT_SKILL_ROOT}/references/agent-features.md"
  "${DRAFT_SKILL_ROOT}/references/core-library.md"
  "${DRAFT_SKILL_ROOT}/references/interop-and-targets.md"
  "${DRAFT_SKILL_ROOT}/references/language.md"
  "${DRAFT_SKILL_ROOT}/references/memory-and-ownership.md"
  "${DRAFT_SKILL_ROOT}/references/workflow-and-testing.md"
)
add_custom_command(
  OUTPUT "${DRAFT_SKILL_BUNDLE}"
  COMMAND
    "${CMAKE_COMMAND}"
    "-DDRAFT_SKILL_ROOT=${DRAFT_SKILL_ROOT}"
    "-DDRAFT_SKILL_OUTPUT=${DRAFT_SKILL_BUNDLE}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_draft_skill.cmake"
  DEPENDS
    ${DRAFT_SKILL_INPUTS}
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_draft_skill.cmake"
  VERBATIM
)

# Draft embeds one pinned LLVM major through its C API. The explicit common
# installation hints keep a normal Apple Silicon Homebrew or Ubuntu LLVM 22
# installation zero-configuration while LLVM_DIR remains the authoritative
# override for custom and distributed toolchains.
find_package(
  LLVM REQUIRED CONFIG
  HINTS
    "/opt/homebrew/opt/llvm/lib/cmake/llvm"
    "/usr/lib/llvm-22/lib/cmake/llvm"
)
if(NOT LLVM_PACKAGE_VERSION MATCHES "^22\\.")
  message(FATAL_ERROR
    "Draft requires LLVM 22.x; found ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}"
  )
endif()
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")
separate_arguments(LLVM_DEFINITIONS_LIST NATIVE_COMMAND "${LLVM_DEFINITIONS}")
if(NOT TARGET LLVM)
  message(FATAL_ERROR
    "LLVM 22 distribution does not export the required shared LLVM target"
  )
endif()

# An interface target keeps the host-language policy identical for the compiler,
# driver, and tests. Sanitizer link flags must reach final executables; placing
# them only on the static compiler archive would instrument objects without
# linking the sanitizer runtime.
add_library(draft_build_options INTERFACE)

# Keep LLVM headers, definitions, and link components behind one adapter target.
# The compiler proper includes only llvm_object_emitter.h and therefore cannot
# accidentally make an LLVM C++ representation part of semantic or MIR code.
add_library(
  draft_llvm_backend
  src/backend/llvm_object_emitter.cpp
)
target_include_directories(
  draft_llvm_backend
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
  SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS}
)
target_compile_options(draft_llvm_backend PRIVATE ${LLVM_DEFINITIONS_LIST})
target_compile_definitions(
  draft_llvm_backend
  PRIVATE
    DRAFT_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
    DRAFT_LLVM_TOOLS_DIRECTORY="${LLVM_TOOLS_BINARY_DIR}"
)
target_link_libraries(
  draft_llvm_backend
  PUBLIC draft_build_options
  PRIVATE LLVM
)

add_library(
  draft_compiler
  src/assembly/aarch64.cpp
  src/backend/foreign_inputs.cpp
  src/backend/foreign_summaries.cpp
  src/backend/llvm_ir.cpp
  src/backend/native_object_tasks.cpp
  src/backend/runtime_assets.cpp
  src/backend/source_correlation.cpp
  src/backend/toolchain.cpp
  src/base/content_tree.cpp
  src/base/sha256.cpp
  src/base/timing.cpp
  src/base/work_graph.cpp
  src/compile/compiler.cpp
  src/compile/expanded_source.cpp
  src/compile/resolver.cpp
  src/elaborator/codex_cli.cpp
  src/elaborator/draft_skill.cpp
  "${DRAFT_SKILL_BUNDLE}"
  src/elaborator/generated_source.cpp
  src/elaborator/obligation.cpp
  src/elaborator/resolution.cpp
  src/elaborator/resolution_overlay.cpp
  src/elaborator/resolved_program.cpp
  src/elaborator/resolution_store.cpp
  src/interop/aarch64_abi.cpp
  src/interop/c_header.cpp
  src/interop/native.cpp
  src/judgment/codex_cli.cpp
  src/judgment/cli_policy.cpp
  src/judgment/command.cpp
  src/judgment/evidence.cpp
  src/judgment/evidence_store.cpp
  src/judgment/selection.cpp
  src/mir/lower.cpp
  src/mir/mir.cpp
  src/mir/verify.cpp
  src/source/diagnostic.cpp
  src/source/source.cpp
  src/sema/analyzer.cpp
  src/sema/agent_flow.cpp
  src/sema/agent_metadata.cpp
  src/sema/big_integer.cpp
  src/sema/body_checker.cpp
  src/sema/constant.cpp
  src/sema/denial.cpp
  src/sema/effect.cpp
  src/sema/foreign_summary.cpp
  src/sema/global_initializer.cpp
  src/sema/hir.cpp
  src/sema/ieee_float.cpp
  src/sema/initialization.cpp
  src/sema/interface.cpp
  src/sema/integer_expression.cpp
  src/sema/runtime_context.cpp
  src/sema/semantic.cpp
  src/sema/symbol.cpp
  src/sema/target_validation.cpp
  src/sema/type.cpp
  src/sema/type_inspection.cpp
  src/sema/type_resolver.cpp
  src/syntax/lexer.cpp
  src/syntax/literal.cpp
  src/syntax/parser.cpp
  src/syntax/syntax_tree.cpp
  src/syntax/token.cpp
  src/target/profile.cpp
  src/validation/discovery.cpp
  src/validation/command.cpp
  src/validation/evidence.cpp
  src/validation/evidence_store.cpp
  src/validation/evidence_store_core.cpp
  src/validation/instrumentation.cpp
  src/validation/runner.cpp
  src/workspace/package.cpp
  src/workspace/selection.cpp
  src/workspace/workspace.cpp
)

target_include_directories(draft_compiler PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
  draft_compiler
  PUBLIC draft_build_options Threads::Threads
  PRIVATE draft_llvm_backend
)

if(MSVC)
  target_compile_options(draft_build_options INTERFACE /W4 /permissive- /GR- /EHs-c-)
  if(DRAFT_WARNINGS_AS_ERRORS)
    target_compile_options(draft_build_options INTERFACE /WX)
  endif()
else()
  target_compile_options(
    draft_build_options
    INTERFACE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
      -Wshadow
      -fno-exceptions
      -fno-rtti
  )
  if(DRAFT_WARNINGS_AS_ERRORS)
    target_compile_options(draft_build_options INTERFACE -Werror)
  endif()
  if(DRAFT_ENABLE_SANITIZERS)
    target_compile_options(draft_build_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(draft_build_options INTERFACE -fsanitize=address,undefined)
  endif()
endif()

add_executable(draftc src/driver/main.cpp)
target_link_libraries(draftc PRIVATE draft_compiler)
target_compile_definitions(
  draftc
  PRIVATE
    DRAFT_CORE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/core"
    DRAFT_CORE_CONTENT_IDENTITY="draft-core-bootstrap-v4"
)

include(CTest)
if(BUILD_TESTING)
  # The complete native matrix must execute the artifacts it just built. Keep
  # it enabled only when the host is one of Draft's implemented AArch64 target
  # pairs: Apple Silicon runs Mach-O and AArch64 Linux runs ELF. Other hosts
  # still build every target-independent compiler, ABI, and lowering test.
  set(DRAFT_NATIVE_AARCH64_HOST OFF)
  if(
    (APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
    AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$"
  )
    set(DRAFT_NATIVE_AARCH64_HOST ON)
  endif()

  add_executable(draft_sha256_tests tests/sha256_test.cpp)
  target_link_libraries(draft_sha256_tests PRIVATE draft_compiler)
  add_test(NAME draft_sha256_tests COMMAND draft_sha256_tests)

  add_executable(draft_timing_tests tests/timing_test.cpp)
  target_link_libraries(draft_timing_tests PRIVATE draft_compiler)
  add_test(NAME draft_timing_tests COMMAND draft_timing_tests)

  add_executable(draft_work_graph_tests tests/work_graph_test.cpp)
  target_link_libraries(draft_work_graph_tests PRIVATE draft_compiler)
  add_test(NAME draft_work_graph_tests COMMAND draft_work_graph_tests)

  add_executable(draft_content_tree_tests tests/content_tree_test.cpp)
  target_link_libraries(draft_content_tree_tests PRIVATE draft_compiler)
  add_test(NAME draft_content_tree_tests COMMAND draft_content_tree_tests)

  add_executable(draft_foreign_inputs_tests tests/foreign_inputs_test.cpp)
  target_link_libraries(draft_foreign_inputs_tests PRIVATE draft_compiler)
  add_test(NAME draft_foreign_inputs_tests COMMAND draft_foreign_inputs_tests)

  add_executable(draft_runtime_assets_tests tests/runtime_assets_test.cpp)
  target_link_libraries(draft_runtime_assets_tests PRIVATE draft_compiler)
  add_test(NAME draft_runtime_assets_tests COMMAND draft_runtime_assets_tests)

  add_executable(draft_lexer_tests tests/lexer_test.cpp)
  target_link_libraries(draft_lexer_tests PRIVATE draft_compiler)
  add_test(NAME draft_lexer_tests COMMAND draft_lexer_tests)

  add_executable(draft_parser_tests tests/parser_test.cpp)
  target_link_libraries(draft_parser_tests PRIVATE draft_compiler)
  add_test(NAME draft_parser_tests COMMAND draft_parser_tests)

  add_executable(draft_package_tests tests/package_test.cpp)
  target_link_libraries(draft_package_tests PRIVATE draft_compiler)
  add_test(NAME draft_package_tests COMMAND draft_package_tests)

  add_executable(draft_workspace_tests tests/workspace_test.cpp)
  target_link_libraries(draft_workspace_tests PRIVATE draft_compiler)
  add_test(NAME draft_workspace_tests COMMAND draft_workspace_tests)

  add_executable(
    draft_workspace_selection_tests
    tests/workspace_selection_test.cpp
  )
  target_link_libraries(draft_workspace_selection_tests PRIVATE draft_compiler)
  add_test(
    NAME draft_workspace_selection_tests
    COMMAND draft_workspace_selection_tests
  )

  add_executable(draft_sema_foundation_tests tests/sema_foundation_test.cpp)
  target_link_libraries(draft_sema_foundation_tests PRIVATE draft_compiler)
  add_test(NAME draft_sema_foundation_tests COMMAND draft_sema_foundation_tests)

  add_executable(draft_big_integer_tests tests/big_integer_test.cpp)
  target_link_libraries(draft_big_integer_tests PRIVATE draft_compiler)
  add_test(NAME draft_big_integer_tests COMMAND draft_big_integer_tests)

  add_executable(draft_integer_expression_tests tests/integer_expression_test.cpp)
  target_link_libraries(draft_integer_expression_tests PRIVATE draft_compiler)
  add_test(NAME draft_integer_expression_tests COMMAND draft_integer_expression_tests)

  add_executable(draft_declaration_tests tests/declaration_test.cpp)
  target_link_libraries(draft_declaration_tests PRIVATE draft_compiler)
  add_test(NAME draft_declaration_tests COMMAND draft_declaration_tests)

  add_executable(draft_type_resolver_tests tests/type_resolver_test.cpp)
  target_link_libraries(draft_type_resolver_tests PRIVATE draft_compiler)
  add_test(NAME draft_type_resolver_tests COMMAND draft_type_resolver_tests)

  add_executable(draft_interface_tests tests/interface_test.cpp)
  target_link_libraries(draft_interface_tests PRIVATE draft_compiler)
  add_test(NAME draft_interface_tests COMMAND draft_interface_tests)

  add_executable(draft_agent_metadata_tests tests/agent_metadata_test.cpp)
  target_link_libraries(draft_agent_metadata_tests PRIVATE draft_compiler)
  add_test(NAME draft_agent_metadata_tests COMMAND draft_agent_metadata_tests)

  add_executable(draft_resolution_tests tests/resolution_test.cpp)
  target_link_libraries(draft_resolution_tests PRIVATE draft_compiler)
  add_test(NAME draft_resolution_tests COMMAND draft_resolution_tests)

  add_executable(draft_resolution_store_tests tests/resolution_store_test.cpp)
  target_link_libraries(draft_resolution_store_tests PRIVATE draft_compiler)
  add_test(NAME draft_resolution_store_tests COMMAND draft_resolution_store_tests)

  add_executable(draft_resolution_overlay_tests tests/resolution_overlay_test.cpp)
  target_link_libraries(draft_resolution_overlay_tests PRIVATE draft_compiler)
  add_test(NAME draft_resolution_overlay_tests COMMAND draft_resolution_overlay_tests)

  add_executable(draft_resolver_tests tests/resolver_test.cpp)
  target_link_libraries(draft_resolver_tests PRIVATE draft_compiler)
  target_compile_definitions(
    draft_resolver_tests
    PRIVATE DRAFT_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}"
  )
  add_test(NAME draft_resolver_tests COMMAND draft_resolver_tests)

  add_executable(draft_expanded_source_tests tests/expanded_source_test.cpp)
  target_link_libraries(draft_expanded_source_tests PRIVATE draft_compiler)
  target_compile_definitions(
    draft_expanded_source_tests
    PRIVATE DRAFT_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}"
  )
  add_test(NAME draft_expanded_source_tests COMMAND draft_expanded_source_tests)

  add_executable(draft_codex_cli_tests tests/codex_cli_test.cpp)
  target_link_libraries(draft_codex_cli_tests PRIVATE draft_compiler)
  add_test(NAME draft_codex_cli_tests COMMAND draft_codex_cli_tests)

  add_executable(
    draft_judgment_codex_cli_tests
    tests/judgment_codex_cli_test.cpp
  )
  target_link_libraries(draft_judgment_codex_cli_tests PRIVATE draft_compiler)
  add_test(
    NAME draft_judgment_codex_cli_tests
    COMMAND draft_judgment_codex_cli_tests
  )

  add_executable(draft_effect_tests tests/effect_test.cpp)
  target_link_libraries(draft_effect_tests PRIVATE draft_compiler)
  add_test(NAME draft_effect_tests COMMAND draft_effect_tests)

  add_executable(draft_denial_tests tests/denial_test.cpp)
  target_link_libraries(draft_denial_tests PRIVATE draft_compiler)
  add_test(NAME draft_denial_tests COMMAND draft_denial_tests)

  add_executable(draft_constant_tests tests/constant_test.cpp)
  target_link_libraries(draft_constant_tests PRIVATE draft_compiler)
  add_test(NAME draft_constant_tests COMMAND draft_constant_tests)

  add_executable(draft_target_profile_tests tests/target_profile_test.cpp)
  target_link_libraries(draft_target_profile_tests PRIVATE draft_compiler)
  add_test(NAME draft_target_profile_tests COMMAND draft_target_profile_tests)

  add_executable(draft_body_checker_tests tests/body_checker_test.cpp)
  target_link_libraries(draft_body_checker_tests PRIVATE draft_compiler)
  add_test(NAME draft_body_checker_tests COMMAND draft_body_checker_tests)

  add_executable(draft_mir_tests tests/mir_test.cpp)
  target_link_libraries(draft_mir_tests PRIVATE draft_compiler)
  add_test(NAME draft_mir_tests COMMAND draft_mir_tests)

  add_executable(draft_llvm_ir_tests tests/llvm_ir_test.cpp)
  target_link_libraries(draft_llvm_ir_tests PRIVATE draft_compiler)
  add_test(NAME draft_llvm_ir_tests COMMAND draft_llvm_ir_tests)

  add_executable(
    draft_llvm_object_emitter_tests
    tests/llvm_object_emitter_test.cpp
  )
  target_link_libraries(draft_llvm_object_emitter_tests PRIVATE draft_compiler)
  add_test(
    NAME draft_llvm_object_emitter_tests
    COMMAND draft_llvm_object_emitter_tests
  )

  add_executable(
    draft_native_object_tasks_tests
    tests/native_object_tasks_test.cpp
  )
  target_link_libraries(draft_native_object_tasks_tests PRIVATE draft_compiler)
  add_test(
    NAME draft_native_object_tasks_tests
    COMMAND draft_native_object_tasks_tests
  )

  add_executable(draft_compiler_tests tests/compiler_test.cpp)
  target_link_libraries(draft_compiler_tests PRIVATE draft_compiler)
  target_compile_definitions(
    draft_compiler_tests
    PRIVATE DRAFT_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}"
  )
  add_test(NAME draft_compiler_tests COMMAND draft_compiler_tests)

  add_executable(draft_assembly_tests tests/assembly_test.cpp)
  target_link_libraries(draft_assembly_tests PRIVATE draft_compiler)
  add_test(NAME draft_assembly_tests COMMAND draft_assembly_tests)

  add_executable(draft_native_interop_tests tests/native_interop_test.cpp)
  target_link_libraries(draft_native_interop_tests PRIVATE draft_compiler)
  add_test(NAME draft_native_interop_tests COMMAND draft_native_interop_tests)

  add_executable(draft_aarch64_abi_tests tests/aarch64_abi_test.cpp)
  target_link_libraries(draft_aarch64_abi_tests PRIVATE draft_compiler)
  add_test(NAME draft_aarch64_abi_tests COMMAND draft_aarch64_abi_tests)

  add_executable(draft_c_header_tests tests/c_header_test.cpp)
  target_link_libraries(draft_c_header_tests PRIVATE draft_compiler)
  add_test(NAME draft_c_header_tests COMMAND draft_c_header_tests)

  add_executable(draft_toolchain_tests tests/toolchain_test.cpp)
  target_link_libraries(draft_toolchain_tests PRIVATE draft_compiler)
  target_compile_definitions(
    draft_toolchain_tests
    PRIVATE DRAFT_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}"
  )
  add_test(NAME draft_toolchain_tests COMMAND draft_toolchain_tests)

  # These are intentionally host integration tests rather than fake toolchain
  # tests. Each supported AArch64 host selects its matching Draft target and
  # proves that real links, launches, debug contracts, and C ABI calls work.
  if(DRAFT_NATIVE_AARCH64_HOST)
    add_executable(
      draft_native_determinism_tests
      tests/native_determinism_test.cpp
    )
    target_link_libraries(draft_native_determinism_tests PRIVATE draft_compiler)
    target_compile_definitions(
      draft_native_determinism_tests
      PRIVATE DRAFT_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(
      NAME draft_native_determinism_tests
      COMMAND draft_native_determinism_tests
    )

    add_executable(
      draft_native_backend_parity_tests
      tests/native_backend_parity_test.cpp
    )
    target_link_libraries(
      draft_native_backend_parity_tests PRIVATE draft_compiler
    )
    target_compile_definitions(
      draft_native_backend_parity_tests
      PRIVATE DRAFT_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(
      NAME draft_native_backend_parity_tests
      COMMAND draft_native_backend_parity_tests
    )

    add_executable(
      draft_native_conformance_tests
      tests/native_conformance_test.cpp
    )
    target_link_libraries(draft_native_conformance_tests PRIVATE draft_compiler)
    target_compile_definitions(
      draft_native_conformance_tests
      PRIVATE DRAFT_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(
      NAME draft_native_conformance_tests
      COMMAND draft_native_conformance_tests
    )
    set_tests_properties(
      draft_native_conformance_tests
      PROPERTIES LABELS "examples;integration"
    )

    # This closes the C ABI loop with two independent compilers: Draft emits a
    # dylib and header, then the host C compiler consumes both and the resulting
    # client process checks aggregate, enum, union, and callback behavior.
    add_executable(
      draft_c_client_integration_tests
      tests/c_client_integration_test.cpp
    )
    target_link_libraries(
      draft_c_client_integration_tests
      PRIVATE draft_compiler draft_llvm_backend
    )
    target_compile_definitions(
      draft_c_client_integration_tests
      PRIVATE DRAFT_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}"
    )
    add_test(
      NAME draft_c_client_integration_tests
      COMMAND draft_c_client_integration_tests
    )
    set_tests_properties(
      draft_c_client_integration_tests
      PROPERTIES LABELS "examples;integration"
    )
  endif()

  add_executable(
    draft_validation_discovery_tests
    tests/validation_discovery_test.cpp
  )
  target_link_libraries(draft_validation_discovery_tests PRIVATE draft_compiler)
  target_compile_definitions(
    draft_validation_discovery_tests
    PRIVATE DRAFT_SOURCE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}"
  )
  add_test(
    NAME draft_validation_discovery_tests
    COMMAND draft_validation_discovery_tests
  )

  add_executable(draft_validation_runner_tests tests/validation_runner_test.cpp)
  target_link_libraries(draft_validation_runner_tests PRIVATE draft_compiler)
  add_test(NAME draft_validation_runner_tests COMMAND draft_validation_runner_tests)

  add_executable(
    draft_validation_instrumentation_tests
    tests/validation_instrumentation_test.cpp
  )
  target_link_libraries(
    draft_validation_instrumentation_tests PRIVATE draft_compiler
  )
  add_test(
    NAME draft_validation_instrumentation_tests
    COMMAND draft_validation_instrumentation_tests
  )

  add_executable(draft_validation_evidence_tests tests/validation_evidence_test.cpp)
  target_link_libraries(draft_validation_evidence_tests PRIVATE draft_compiler)
  add_test(NAME draft_validation_evidence_tests COMMAND draft_validation_evidence_tests)

  add_executable(draft_judgment_evidence_tests tests/judgment_evidence_test.cpp)
  target_link_libraries(draft_judgment_evidence_tests PRIVATE draft_compiler)
  add_test(NAME draft_judgment_evidence_tests COMMAND draft_judgment_evidence_tests)

  add_executable(
    draft_validation_evidence_store_tests
    tests/validation_evidence_store_test.cpp
  )
  target_link_libraries(
    draft_validation_evidence_store_tests PRIVATE draft_compiler
  )
  add_test(
    NAME draft_validation_evidence_store_tests
    COMMAND draft_validation_evidence_store_tests
  )

  add_executable(
    draft_judgment_evidence_store_tests
    tests/judgment_evidence_store_test.cpp
  )
  target_link_libraries(
    draft_judgment_evidence_store_tests PRIVATE draft_compiler
  )
  add_test(
    NAME draft_judgment_evidence_store_tests
    COMMAND draft_judgment_evidence_store_tests
  )

  add_executable(draft_judgment_command_tests tests/judgment_command_test.cpp)
  target_link_libraries(draft_judgment_command_tests PRIVATE draft_compiler)
  add_test(NAME draft_judgment_command_tests COMMAND draft_judgment_command_tests)

  add_executable(draft_judgment_driver_tests tests/judgment_driver_test.cpp)
  target_link_libraries(draft_judgment_driver_tests PRIVATE draft_compiler)
  target_compile_definitions(
    draft_judgment_driver_tests
    PRIVATE
      DRAFT_DRIVER_PATH="$<TARGET_FILE:draftc>"
      DRAFT_CORE_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/core"
      DRAFT_CORE_CONTENT_IDENTITY="draft-core-bootstrap-v4"
  )
  add_dependencies(draft_judgment_driver_tests draftc)
  add_test(NAME draft_judgment_driver_tests COMMAND draft_judgment_driver_tests)

  add_executable(
    draft_workspace_build_driver_tests
    tests/workspace_build_driver_test.cpp
  )
  target_link_libraries(
    draft_workspace_build_driver_tests PRIVATE draft_compiler
  )
  target_compile_definitions(
    draft_workspace_build_driver_tests
    PRIVATE DRAFT_DRIVER_PATH="$<TARGET_FILE:draftc>"
  )
  add_dependencies(draft_workspace_build_driver_tests draftc)
  add_test(
    NAME draft_workspace_build_driver_tests
    COMMAND draft_workspace_build_driver_tests
  )

  # The TSV is the exhaustive repository-owned package inventory. This gate is
  # target-independent and provider-free: every tracked example package either
  # checks successfully on both targets or fails with its declared unresolved
  # synthesis diagnostic. Git is used only to prove that the data file has no
  # omissions; it works in both a primary checkout and a linked worktree.
  find_program(DRAFT_GIT_EXECUTABLE NAMES git)
  add_test(
    NAME draft_example_frontend_matrix
    COMMAND
      "${CMAKE_COMMAND}"
      "-DDRAFTC=$<TARGET_FILE:draftc>"
      "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DMATRIX=${CMAKE_CURRENT_SOURCE_DIR}/examples/qualification.tsv"
      "-DGIT_EXECUTABLE=${DRAFT_GIT_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_example_frontend_matrix_test.cmake"
  )
  set_tests_properties(
    draft_example_frontend_matrix
    PROPERTIES LABELS "examples;integration"
  )

  # The handwritten tour must stay usable without provider access. The first
  # command exercises its complete semantic graph; the second proves that the
  # public selector surface discovers a type-member and procedure judgment
  # without configuring or launching Codex.
  add_test(
    NAME draft_judgment_tour_check
    COMMAND draftc check
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/judgment-tour" --root app
  )
  add_test(
    NAME draft_judgment_tour_list
    COMMAND draftc judge
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/judgment-tour" --root app --list
  )
  set_tests_properties(
    draft_judgment_tour_list
    PROPERTIES PASS_REGULAR_EXPRESSION
      "workspace:app counter_add package.draft:0"
  )

  # The mixed example intentionally has no resolution manifest, so a normal
  # check must reject it until Codex supplies the four fragments. Parsing its
  # surface file still gives CI a provider-free guard for every synthesis and
  # judgment placement used by the live walkthrough.
  add_test(
    NAME draft_agent_judgment_mix_syntax
    COMMAND draftc syntax
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/agent-judgment-mix/app/package.draft"
  )
  set_tests_properties(
    draft_agent_judgment_mix_syntax
    PROPERTIES PASS_REGULAR_EXPRESSION "SynthesisAssembly"
  )

  # Target selection is a driver concern as well as a profile-library concern.
  # These process-level tests prove that the selector reaches both profile
  # reporting and a complete dependency-ordered semantic compilation.
  add_test(
    NAME draft_aarch64_linux_target_driver
    COMMAND draftc target --target aarch64-linux
  )
  set_tests_properties(
    draft_aarch64_linux_target_driver
    PROPERTIES PASS_REGULAR_EXPRESSION
      "identity draft-aarch64-linux-gnu-v1.*triple aarch64-unknown-linux-gnu"
  )
  add_test(
    NAME draft_aarch64_linux_check_driver
    COMMAND draftc check "${CMAKE_CURRENT_SOURCE_DIR}/examples/core-os"
      --target aarch64-linux
  )
  set_tests_properties(
    draft_aarch64_linux_check_driver
    PROPERTIES PASS_REGULAR_EXPRESSION
      "checked package graph rooted at core_os"
  )

  # Timing is a process-facing diagnostic contract. Exercise both report
  # densities through the real driver so option parsing, stderr routing, phase
  # propagation, filtering, and deterministic work counters stay connected.
  add_test(
    NAME draft_driver_timings
    COMMAND
      "${CMAKE_COMMAND}"
      "-DDRAFTC=$<TARGET_FILE:draftc>"
      "-DSOURCE_PACKAGE=${CMAKE_CURRENT_SOURCE_DIR}/examples/hello"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_timing_test.cmake"
  )

  add_test(
    NAME draft_driver_canonicalizes_symlinked_workspace
    COMMAND
      "${CMAKE_COMMAND}"
      "-DDRAFTC=$<TARGET_FILE:draftc>"
      "-DSOURCE_PACKAGE=${CMAKE_CURRENT_SOURCE_DIR}/examples/hello"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/driver-symlink-path-test"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_symlink_path_test.cmake"
  )

  # Complete-source projection is a public, provider-free inspection command.
  # Run it through the real driver while keeping its requested output entirely
  # inside the disposable build tree.
  add_test(
    NAME draft_driver_expand
    COMMAND
      "${CMAKE_COMMAND}"
      "-DDRAFTC=$<TARGET_FILE:draftc>"
      "-DSOURCE_WORKSPACE=${CMAKE_CURRENT_SOURCE_DIR}/examples/agent-acceptance"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/expanded-source-driver-test"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_expand_test.cmake"
  )

  if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    # Address instrumentation is an ordinary host-toolchain validation mode.
    # This native check proves the public command no longer depends on a
    # separate build mode or copied sanitizer distribution.
    add_test(
      NAME draft_validation_address_instrumentation
      COMMAND
        "${CMAKE_COMMAND}"
        "-DDRAFTC=$<TARGET_FILE:draftc>"
        "-DSOURCE_PACKAGE=${CMAKE_CURRENT_SOURCE_DIR}/examples/validation"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/address-instrumentation-test"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_address_instrumentation_test.cmake"
    )
  endif()

  # Agent commands discover Codex through PATH. These fixtures contain real
  # typed sites and must fail without committing source/evidence when the test
  # environment has no usable Codex process.
  add_test(
    NAME draft_resolve_reports_unavailable_codex
    COMMAND "${CMAKE_COMMAND}" -E env
      "PATH=${CMAKE_CURRENT_BINARY_DIR}/no-codex-path"
      $<TARGET_FILE:draftc> resolve
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/agent-pending" --root app
      --assertions=off
  )
  set_tests_properties(
    draft_resolve_reports_unavailable_codex
    PROPERTIES
      WILL_FAIL TRUE
  )
  add_test(
    NAME draft_judge_reports_unavailable_codex
    COMMAND "${CMAKE_COMMAND}" -E env
      "PATH=${CMAKE_CURRENT_BINARY_DIR}/no-codex-path"
      $<TARGET_FILE:draftc> judge
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/agent-pending" --root app
      --assertions=off
  )
  set_tests_properties(
    draft_judge_reports_unavailable_codex
    PROPERTIES
      WILL_FAIL TRUE
  )

  # Public optional-argument parsing reaches the resolver's exact-site
  # selection rule without starting Codex for any unrelated fresh pin.
  add_test(
    NAME draft_regeneration_rejects_unknown_site
    COMMAND draftc resolve
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/agent-acceptance" --root app
      --regenerate site-does-not-exist
  )
  set_tests_properties(
    draft_regeneration_rejects_unknown_site
    PROPERTIES
      WILL_FAIL TRUE
  )

  if(DRAFT_NATIVE_AARCH64_HOST)
    if(APPLE)
      set(DRAFT_NATIVE_TEST_TARGET aarch64-macos)
    else()
      set(DRAFT_NATIVE_TEST_TARGET aarch64-linux)
    endif()
    add_test(
      NAME draft_resolve_build_reuses_final_graph
      COMMAND
        "${CMAKE_COMMAND}"
        "-DDRAFTC=$<TARGET_FILE:draftc>"
        "-DSOURCE_WORKSPACE=${CMAKE_CURRENT_SOURCE_DIR}/examples/agent-acceptance"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/resolve-build-driver-test"
        "-DTARGET_SELECTOR=${DRAFT_NATIVE_TEST_TARGET}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_resolve_build_test.cmake"
    )
    set_tests_properties(
      draft_resolve_build_reuses_final_graph
      PROPERTIES LABELS "examples;integration"
    )

    # A target-qualified copied manifest must remain consumable when a failing
    # Codex sentinel shadows any real provider. This covers all four ordinary
    # consumers and verifies that repeated commands create no compiler cache.
    add_test(
      NAME draft_provider_free_resolved_program_consumers
      COMMAND
        "${CMAKE_COMMAND}"
        "-DDRAFTC=$<TARGET_FILE:draftc>"
        "-DSOURCE_WORKSPACE=${CMAKE_CURRENT_SOURCE_DIR}/examples/agent-acceptance"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/provider-free-consumers-test"
        "-DTARGET_SELECTOR=${DRAFT_NATIVE_TEST_TARGET}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_provider_free_consumers_test.cmake"
    )
    set_tests_properties(
      draft_provider_free_resolved_program_consumers
      PROPERTIES LABELS "examples;integration"
    )

    # Every example-owned Draft test and benchmark runs from a private copy of
    # its declared workspace. The same target-qualified acceptance pins used by
    # ordinary provider-free builds are therefore exercised on both native CI
    # hosts, alongside the handwritten validation packages.
    add_test(
      NAME draft_example_validation_matrix
      COMMAND
        "${CMAKE_COMMAND}"
        "-DDRAFTC=$<TARGET_FILE:draftc>"
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DMATRIX=${CMAKE_CURRENT_SOURCE_DIR}/examples/qualification.tsv"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/example-validation-matrix"
        "-DTARGET_SELECTOR=${DRAFT_NATIVE_TEST_TARGET}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_example_validation_matrix_test.cmake"
    )
    set_tests_properties(
      draft_example_validation_matrix
      PROPERTIES LABELS "examples;integration"
    )

    # Build the logical custom_math provider independently as C, then map its
    # exact object into the public Draft build and launch the checked example.
    add_library(
      draft_foreign_provider_fixture OBJECT
      tests/foreign_provider_fixture.c
    )
    add_test(
      NAME draft_foreign_provider_example
      COMMAND
        "${CMAKE_COMMAND}"
        "-DDRAFTC=$<TARGET_FILE:draftc>"
        "-DSOURCE_PACKAGE=${CMAKE_CURRENT_SOURCE_DIR}/examples/foreign-provider"
        "-DPROVIDER_OBJECT=$<TARGET_OBJECTS:draft_foreign_provider_fixture>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/foreign-provider-example"
        "-DTARGET_SELECTOR=${DRAFT_NATIVE_TEST_TARGET}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_foreign_provider_example_test.cmake"
    )
    set_tests_properties(
      draft_foreign_provider_example
      PROPERTIES LABELS "examples;integration"
    )
  endif()
endif()
