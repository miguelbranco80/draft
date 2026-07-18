cmake_minimum_required(VERSION 3.24)

project(
  Draft
  VERSION 0.1.0
  DESCRIPTION "Bootstrap compiler for the Draft systems language"
  LANGUAGES CXX
)

# The bootstrap compiler deliberately uses a small, uniform C++20 dialect. We
# disable implementation extensions so a construct accepted by one host compiler
# does not quietly become a portability dependency of the self-hosting path.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(DRAFT_WARNINGS_AS_ERRORS "Treat bootstrap compiler warnings as errors" ON)
option(DRAFT_ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)

# An interface target keeps the host-language policy identical for the compiler,
# driver, and tests. Sanitizer link flags must reach final executables; placing
# them only on the static compiler archive would instrument objects without
# linking the sanitizer runtime.
add_library(draft_build_options INTERFACE)

add_library(
  draft_compiler
  src/assembly/aarch64.cpp
  src/backend/locked_inputs.cpp
  src/backend/llvm_ir.cpp
  src/backend/toolchain.cpp
  src/base/content_tree.cpp
  src/base/sha256.cpp
  src/compile/compiler.cpp
  src/compile/resolver.cpp
  src/elaborator/codex_cli.cpp
  src/elaborator/generated_source.cpp
  src/elaborator/obligation.cpp
  src/elaborator/resolution.cpp
  src/elaborator/resolution_overlay.cpp
  src/elaborator/resolved_program.cpp
  src/elaborator/resolution_store.cpp
  src/interop/aarch64_abi.cpp
  src/interop/c_header.cpp
  src/interop/native.cpp
  src/mir/lower.cpp
  src/mir/mir.cpp
  src/mir/verify.cpp
  src/source/diagnostic.cpp
  src/source/source.cpp
  src/sema/analyzer.cpp
  src/sema/agent_metadata.cpp
  src/sema/big_integer.cpp
  src/sema/body_checker.cpp
  src/sema/constant.cpp
  src/sema/denial.cpp
  src/sema/effect.cpp
  src/sema/global_initializer.cpp
  src/sema/hir.cpp
  src/sema/ieee_float.cpp
  src/sema/initialization.cpp
  src/sema/interface.cpp
  src/sema/semantic.cpp
  src/sema/symbol.cpp
  src/sema/type.cpp
  src/sema/type_resolver.cpp
  src/syntax/lexer.cpp
  src/syntax/literal.cpp
  src/syntax/parser.cpp
  src/syntax/syntax_tree.cpp
  src/syntax/token.cpp
  src/target/profile.cpp
  src/workspace/package.cpp
  src/workspace/workspace.cpp
)

target_include_directories(draft_compiler PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(draft_compiler PUBLIC draft_build_options)

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
    DRAFT_CORE_CONTENT_IDENTITY="draft-core-bootstrap-v1"
)

include(CTest)
if(BUILD_TESTING)
  add_executable(draft_sha256_tests tests/sha256_test.cpp)
  target_link_libraries(draft_sha256_tests PRIVATE draft_compiler)
  add_test(NAME draft_sha256_tests COMMAND draft_sha256_tests)

  add_executable(draft_content_tree_tests tests/content_tree_test.cpp)
  target_link_libraries(draft_content_tree_tests PRIVATE draft_compiler)
  add_test(NAME draft_content_tree_tests COMMAND draft_content_tree_tests)

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

  add_executable(draft_sema_foundation_tests tests/sema_foundation_test.cpp)
  target_link_libraries(draft_sema_foundation_tests PRIVATE draft_compiler)
  add_test(NAME draft_sema_foundation_tests COMMAND draft_sema_foundation_tests)

  add_executable(draft_big_integer_tests tests/big_integer_test.cpp)
  target_link_libraries(draft_big_integer_tests PRIVATE draft_compiler)
  add_test(NAME draft_big_integer_tests COMMAND draft_big_integer_tests)

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
  add_test(NAME draft_resolver_tests COMMAND draft_resolver_tests)

  add_executable(draft_codex_cli_tests tests/codex_cli_test.cpp)
  target_link_libraries(draft_codex_cli_tests PRIVATE draft_compiler)
  add_test(NAME draft_codex_cli_tests COMMAND draft_codex_cli_tests)

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

  # Provider-free agent commands must recognize real typed sites but fail
  # before writing anything when no provider adapter is configured.
  add_test(
    NAME draft_resolve_requires_provider
    COMMAND draftc resolve "${CMAKE_CURRENT_SOURCE_DIR}/examples/agent-pending"
  )
  set_tests_properties(
    draft_resolve_requires_provider
    PROPERTIES
      WILL_FAIL TRUE
  )
  add_test(
    NAME draft_judge_requires_provider
    COMMAND draftc judge "${CMAKE_CURRENT_SOURCE_DIR}/examples/agent-pending"
  )
  set_tests_properties(
    draft_judge_requires_provider
    PROPERTIES
      WILL_FAIL TRUE
  )
endif()
