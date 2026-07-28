cmake_minimum_required(VERSION 3.24)

project(
  Draft
  VERSION 0.1.0
  DESCRIPTION "Bootstrap compiler for the Draft systems language"
  # LLVM's exported CMake package performs C feature checks even though Draft
  # calls its C API from the C++ bootstrap and compiles no repository C source.
  LANGUAGES C CXX
)

include(GNUInstallDirs)

# The numeric project version remains available to CMake package tooling, while
# the explicit suffix labels this intentionally unstable public experiment. A
# release workflow supplies the exact full commit. Source builds discover it at
# configure time when Git is available and otherwise report the honest `source`
# sentinel rather than manufacturing provenance.
set(DRAFT_RELEASE_SUFFIX "alpha.3")
set(DRAFT_RELEASE_VERSION "${PROJECT_VERSION}-${DRAFT_RELEASE_SUFFIX}")
set(
  DRAFT_BUILD_COMMIT "" CACHE STRING
  "Source commit recorded by Draft binaries"
)
if(DRAFT_BUILD_COMMIT STREQUAL "")
  execute_process(
    COMMAND git rev-parse --verify HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE DRAFT_GIT_RESULT
    OUTPUT_VARIABLE DRAFT_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(DRAFT_GIT_RESULT EQUAL 0 AND NOT DRAFT_GIT_COMMIT STREQUAL "")
    set(DRAFT_BUILD_COMMIT "${DRAFT_GIT_COMMIT}")
  else()
    set(DRAFT_BUILD_COMMIT "source")
  endif()
endif()
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/src/base/version.h.in"
  "${CMAKE_CURRENT_BINARY_DIR}/generated/draft/version.h"
  @ONLY
)

# The bootstrap compiler deliberately uses a small, uniform C++20 dialect. We
# disable implementation extensions so a construct accepted by one host compiler
# does not quietly become a portability dependency of the self-hosting path.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# CMake's MSVC platform defaults add /EHsc before repository targets can apply
# Draft's no-exceptions policy. Leaving both switches on the command line makes
# cl diagnose the deliberate override on every target, and the MSVC standard
# library assumes exception unwinding unless its matching configuration macro
# says otherwise. Remove the ambient switch once here; draft_build_options below
# then establishes one exception policy for every repository-owned C++ target.
if(MSVC)
  string(REGEX REPLACE "[/-]EHsc( |$)" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
endif()

option(DRAFT_WARNINGS_AS_ERRORS "Treat bootstrap compiler warnings as errors" ON)
option(DRAFT_ENABLE_SANITIZERS "Enable address and undefined-behavior sanitizers" OFF)

# An instrumented ELF shared library cannot arrange for the address-sanitizer
# runtime to precede the uninstrumented executable which loads it. DraftIDE has
# exactly that shape in sanitizer builds: its Draft-owned executable is an
# ordinary target program, while the C++ compiler service is instrumented by
# the bootstrap build. Locate the host runtime once so the few tests which
# launch that mixed process can preload it explicitly. This does not instrument
# generated Draft code and does not affect installed release builds.
set(DRAFT_ASAN_PRELOAD "")
if(DRAFT_ENABLE_SANITIZERS AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(DRAFT_ASAN_RUNTIME_NAME "")
  set(DRAFT_ASAN_RUNTIME_RESULT 1)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(DRAFT_ASAN_RUNTIME_NAME "libasan.so")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
      set(DRAFT_ASAN_RUNTIME_NAME "libclang_rt.asan-x86_64.so")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
      set(DRAFT_ASAN_RUNTIME_NAME "libclang_rt.asan-aarch64.so")
    endif()
  endif()
  if(NOT DRAFT_ASAN_RUNTIME_NAME STREQUAL "")
    execute_process(
      COMMAND
        "${CMAKE_CXX_COMPILER}"
        "-print-file-name=${DRAFT_ASAN_RUNTIME_NAME}"
      RESULT_VARIABLE DRAFT_ASAN_RUNTIME_RESULT
      OUTPUT_VARIABLE DRAFT_ASAN_PRELOAD
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
    )
  endif()
  if(
    NOT DRAFT_ASAN_RUNTIME_RESULT EQUAL 0
    OR NOT IS_ABSOLUTE "${DRAFT_ASAN_PRELOAD}"
    OR NOT EXISTS "${DRAFT_ASAN_PRELOAD}"
  )
    message(FATAL_ERROR
      "The selected Linux compiler has no discoverable dynamic ASan runtime"
    )
  endif()
endif()

find_package(Threads REQUIRED)

# Internal Codex operations share the factual language references maintained by
# the repository coding skill, but not that skill's repository-editing workflow
# or agent metadata. Generate an immutable byte table from an explicit file list
# so installed draftc binaries do not depend on a checkout at runtime and an
# incidental file can never enter the provider context.
set(DRAFT_REFERENCE_ROOT
  "${CMAKE_CURRENT_SOURCE_DIR}/.agents/skills/write-draft-code/references")
set(DRAFT_REFERENCE_BUNDLE
  "${CMAKE_CURRENT_BINARY_DIR}/generated/draft_reference_bundle.cpp")
set(
  DRAFT_REFERENCE_INPUTS
  "${DRAFT_REFERENCE_ROOT}/agent-features.md"
  "${DRAFT_REFERENCE_ROOT}/core-library.md"
  "${DRAFT_REFERENCE_ROOT}/interop-and-targets.md"
  "${DRAFT_REFERENCE_ROOT}/language.md"
  "${DRAFT_REFERENCE_ROOT}/memory-and-ownership.md"
)
add_custom_command(
  OUTPUT "${DRAFT_REFERENCE_BUNDLE}"
  COMMAND
    "${CMAKE_COMMAND}"
    "-DDRAFT_REFERENCE_ROOT=${DRAFT_REFERENCE_ROOT}"
    "-DDRAFT_REFERENCE_OUTPUT=${DRAFT_REFERENCE_BUNDLE}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_draft_reference.cmake"
  DEPENDS
    ${DRAFT_REFERENCE_INPUTS}
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_draft_reference.cmake"
  VERBATIM
)

# Core source is part of the compiler distribution rather than an ambient
# checkout path. The generated table is consumed in memory by workspace loading
# and carries a build-time content identity.
set(DRAFT_CORE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/core")
file(GLOB_RECURSE DRAFT_CORE_INPUTS CONFIGURE_DEPENDS
  "${DRAFT_CORE_ROOT}/*.draft"
  "${DRAFT_CORE_ROOT}/*.s"
  "${DRAFT_CORE_ROOT}/*.S"
  "${DRAFT_CORE_ROOT}/*.asm"
)
list(SORT DRAFT_CORE_INPUTS)
set(DRAFT_CORE_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/generated/draft_core_bundle.cpp")
add_custom_command(
  OUTPUT "${DRAFT_CORE_BUNDLE}"
  COMMAND
    "${CMAKE_COMMAND}"
    "-DDRAFT_CORE_ROOT=${DRAFT_CORE_ROOT}"
    "-DDRAFT_CORE_OUTPUT=${DRAFT_CORE_BUNDLE}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_draft_core.cmake"
  DEPENDS
    ${DRAFT_CORE_INPUTS}
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_draft_core.cmake"
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
# Unix distributions normally publish the monolithic libLLVM target. LLVM
# cannot build that dylib on Windows, so the Windows bootstrap links the
# equivalent static LTO and AArch64/X86 component closure. ThinLTO requires
# LLVM's C++ LTO and module-summary APIs; LLVM-C.dll alone is no longer a
# sufficient implementation boundary even though ordinary module construction
# continues to use the stable C API.
if(TARGET LLVM)
  set(DRAFT_LLVM_LIBRARY_TARGETS LLVM)
elseif(WIN32)
  llvm_map_components_to_libnames(
    DRAFT_LLVM_LIBRARY_TARGETS
    LTO
    AArch64
    X86
  )
else()
  message(FATAL_ERROR
    "LLVM 22 distribution does not export the required LLVM LTO components"
  )
endif()

# The hosted runtime is invariant for one versioned target profile. Compile it
# once while building the compiler distribution and embed the resulting object
# bytes in draftc. Ordinary Draft builds can then link the runtime directly
# without reconstructing or optimizing a large target-only LLVM module and
# without depending on an installed sidecar directory.
find_program(
  DRAFT_RUNTIME_CLANG
  NAMES clang clang.exe
  HINTS "${LLVM_TOOLS_BINARY_DIR}"
  NO_DEFAULT_PATH
  REQUIRED
)
set(DRAFT_RUNTIME_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/hosted_runtime.c")
set(DRAFT_RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/generated/hosted-runtime")
set(DRAFT_RUNTIME_AARCH64_MACOS
    "${DRAFT_RUNTIME_OUTPUT_DIRECTORY}/aarch64-macos.o")
set(DRAFT_RUNTIME_AARCH64_MACOS_ASSEMBLY
    "${DRAFT_RUNTIME_OUTPUT_DIRECTORY}/aarch64-macos.s")
set(DRAFT_RUNTIME_AARCH64_LINUX
    "${DRAFT_RUNTIME_OUTPUT_DIRECTORY}/aarch64-linux.o")
set(DRAFT_RUNTIME_AARCH64_LINUX_ASSEMBLY
    "${DRAFT_RUNTIME_OUTPUT_DIRECTORY}/aarch64-linux.s")
set(DRAFT_RUNTIME_X86_64_LINUX
    "${DRAFT_RUNTIME_OUTPUT_DIRECTORY}/x86_64-linux.o")
set(DRAFT_RUNTIME_X86_64_LINUX_ASSEMBLY
    "${DRAFT_RUNTIME_OUTPUT_DIRECTORY}/x86_64-linux.s")
set(DRAFT_RUNTIME_X86_64_WINDOWS
    "${DRAFT_RUNTIME_OUTPUT_DIRECTORY}/x86_64-windows.obj")
set(DRAFT_RUNTIME_X86_64_WINDOWS_ASSEMBLY
    "${DRAFT_RUNTIME_OUTPUT_DIRECTORY}/x86_64-windows.s")

function(draft_compile_hosted_runtime output assembly triple)
  add_custom_command(
    OUTPUT "${output}" "${assembly}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${DRAFT_RUNTIME_OUTPUT_DIRECTORY}"
    COMMAND "${DRAFT_RUNTIME_CLANG}"
            "--target=${triple}"
            -std=c11 -O2 -ffreestanding -fno-stack-protector
            -Wall -Wextra -Werror
            ${ARGN}
            -c "${DRAFT_RUNTIME_SOURCE}" -o "${output}"
    COMMAND "${DRAFT_RUNTIME_CLANG}"
            "--target=${triple}"
            -std=c11 -O2 -ffreestanding -fno-stack-protector
            -Wall -Wextra -Werror
            ${ARGN}
            -S "${DRAFT_RUNTIME_SOURCE}" -o "${assembly}"
    DEPENDS "${DRAFT_RUNTIME_SOURCE}"
    VERBATIM
  )
endfunction()

draft_compile_hosted_runtime(
  "${DRAFT_RUNTIME_AARCH64_MACOS}"
  "${DRAFT_RUNTIME_AARCH64_MACOS_ASSEMBLY}"
  "arm64-apple-macosx14.0.0"
  -fPIC -DDRAFT_RUNTIME_POSIX=1 -DDRAFT_RUNTIME_DARWIN=1
)
draft_compile_hosted_runtime(
  "${DRAFT_RUNTIME_AARCH64_LINUX}"
  "${DRAFT_RUNTIME_AARCH64_LINUX_ASSEMBLY}"
  "aarch64-unknown-linux-gnu"
  -fPIC -DDRAFT_RUNTIME_POSIX=1 -DDRAFT_RUNTIME_LINUX=1
)
draft_compile_hosted_runtime(
  "${DRAFT_RUNTIME_X86_64_LINUX}"
  "${DRAFT_RUNTIME_X86_64_LINUX_ASSEMBLY}"
  "x86_64-unknown-linux-gnu"
  -fPIC -DDRAFT_RUNTIME_POSIX=1 -DDRAFT_RUNTIME_LINUX=1
)
draft_compile_hosted_runtime(
  "${DRAFT_RUNTIME_X86_64_WINDOWS}"
  "${DRAFT_RUNTIME_X86_64_WINDOWS_ASSEMBLY}"
  "x86_64-pc-windows-msvc"
  -DDRAFT_RUNTIME_WINDOWS=1
)

set(DRAFT_RUNTIME_BUNDLE
    "${CMAKE_CURRENT_BINARY_DIR}/generated/hosted_runtime_bundle.cpp")
add_custom_command(
  OUTPUT "${DRAFT_RUNTIME_BUNDLE}"
  COMMAND
    "${CMAKE_COMMAND}"
    "-DDRAFT_RUNTIME_BUNDLE_OUTPUT=${DRAFT_RUNTIME_BUNDLE}"
    "-DDRAFT_RUNTIME_AARCH64_MACOS=${DRAFT_RUNTIME_AARCH64_MACOS}"
    "-DDRAFT_RUNTIME_AARCH64_MACOS_ASSEMBLY=${DRAFT_RUNTIME_AARCH64_MACOS_ASSEMBLY}"
    "-DDRAFT_RUNTIME_AARCH64_LINUX=${DRAFT_RUNTIME_AARCH64_LINUX}"
    "-DDRAFT_RUNTIME_AARCH64_LINUX_ASSEMBLY=${DRAFT_RUNTIME_AARCH64_LINUX_ASSEMBLY}"
    "-DDRAFT_RUNTIME_X86_64_LINUX=${DRAFT_RUNTIME_X86_64_LINUX}"
    "-DDRAFT_RUNTIME_X86_64_LINUX_ASSEMBLY=${DRAFT_RUNTIME_X86_64_LINUX_ASSEMBLY}"
    "-DDRAFT_RUNTIME_X86_64_WINDOWS=${DRAFT_RUNTIME_X86_64_WINDOWS}"
    "-DDRAFT_RUNTIME_X86_64_WINDOWS_ASSEMBLY=${DRAFT_RUNTIME_X86_64_WINDOWS_ASSEMBLY}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_hosted_runtime.cmake"
  DEPENDS
    "${DRAFT_RUNTIME_AARCH64_MACOS}"
    "${DRAFT_RUNTIME_AARCH64_MACOS_ASSEMBLY}"
    "${DRAFT_RUNTIME_AARCH64_LINUX}"
    "${DRAFT_RUNTIME_AARCH64_LINUX_ASSEMBLY}"
    "${DRAFT_RUNTIME_X86_64_LINUX}"
    "${DRAFT_RUNTIME_X86_64_LINUX_ASSEMBLY}"
    "${DRAFT_RUNTIME_X86_64_WINDOWS}"
    "${DRAFT_RUNTIME_X86_64_WINDOWS_ASSEMBLY}"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_hosted_runtime.cmake"
  VERBATIM
)

# An interface target keeps the host-language policy identical for the compiler,
# driver, and tests. Sanitizer link flags must reach final executables; placing
# them only on the static compiler archive would instrument objects without
# linking the sanitizer runtime.
add_library(draft_build_options INTERFACE)
target_include_directories(
  draft_build_options
  INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

# Keep LLVM headers and definitions behind one object-library boundary. Its
# objects become part of draft_compiler so the direct package builder may read
# semantic/MIR products without creating a circular pair of static archives.
# LLVM handles remain private to backend implementation files; the final static
# compiler target alone carries the LLVM link dependency to its consumers.
add_library(
  draft_llvm_backend OBJECT
  src/backend/llvm_initialization.cpp
  src/backend/llvm_object_emitter.cpp
  src/backend/llvm_package_emitter.cpp
  src/backend/llvm_thinlto.cpp
)
target_include_directories(
  draft_llvm_backend
  PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_include_directories(
  draft_llvm_backend
  SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS}
)
target_compile_options(draft_llvm_backend PRIVATE ${LLVM_DEFINITIONS_LIST})
if(MSVC)
  # LLVM and the MSVC standard library are separately built dependencies with
  # their own warning policies. Draft headers use quoted includes, so treating
  # angle-bracket headers as external keeps /WX fully active for adapter code
  # and repository headers. llvm_thinlto.cpp separately brackets one narrowing
  # warning forced by LLVM 22.1.6's inline C++ API; that exception ends before
  # any Draft implementation is parsed.
  target_compile_options(
    draft_llvm_backend PRIVATE /external:anglebrackets /external:W0
  )
  # Optimized MSVC code generation diagnoses unreachable branches in LLVM's
  # DenseMap templates only after the header-level warning scope has ended.
  # There is no source-local pragma point for that deferred C4702. Disable it
  # for the one translation unit which consumes LLVM's C++ ThinLTO API; /WX and
  # every other warning remain active there, and C4702 remains an error for all
  # other Draft sources.
  set_source_files_properties(
    src/backend/llvm_thinlto.cpp PROPERTIES COMPILE_OPTIONS /wd4702
  )
endif()
target_compile_definitions(
  draft_llvm_backend
  PRIVATE
    DRAFT_LLVM_VERSION="${LLVM_PACKAGE_VERSION}"
    DRAFT_LLVM_TOOLS_DIRECTORY="${LLVM_TOOLS_BINARY_DIR}"
    DRAFT_DISTRIBUTION_LLVM_TOOLS_RELATIVE="../libexec/draft/bin"
)
target_link_libraries(
  draft_llvm_backend
  PUBLIC draft_build_options
)

add_library(
  draft_compiler
  src/assembly/aarch64.cpp
  src/assembly/analyze.cpp
  src/backend/build_policy.cpp
  src/backend/foreign_inputs.cpp
  src/backend/foreign_summaries.cpp
  "${DRAFT_RUNTIME_BUNDLE}"
  src/backend/native_object_tasks.cpp
  src/backend/runtime_assets.cpp
  src/backend/toolchain.cpp
  src/base/content_tree.cpp
  src/base/child_process.cpp
  src/base/executable_path.cpp
  src/base/sha256.cpp
  src/base/timing.cpp
  src/base/work_graph.cpp
  src/compile/body_work.cpp
  src/compile/compiler.cpp
  src/compile/expanded_source.cpp
  src/compile/resolver.cpp
  src/compile/semantic_work_graph.cpp
  src/elaborator/codex_cli.cpp
  src/elaborator/draft_reference.cpp
  "${DRAFT_REFERENCE_BUNDLE}"
  src/elaborator/generated_source.cpp
  src/elaborator/obligation.cpp
  src/elaborator/resolution.cpp
  src/elaborator/resolution_overlay.cpp
  src/elaborator/resolved_program.cpp
  src/elaborator/resolution_store.cpp
  src/interop/c_abi.cpp
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
  src/mir/native_reachability.cpp
  src/mir/verify.cpp
  src/source/diagnostic.cpp
  src/source/source.cpp
  src/sema/analyzer.cpp
  src/sema/agent_flow.cpp
  src/sema/agent_metadata.cpp
  src/sema/big_integer.cpp
  src/sema/body_checker.cpp
  src/sema/body_publication.cpp
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
  src/sema/task_semantics.cpp
  src/sema/semantic.cpp
  src/sema/symbol.cpp
  src/sema/target_validation.cpp
  src/sema/type.cpp
  src/sema/type_inspection.cpp
  src/sema/type_layout.cpp
  src/sema/type_product.cpp
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
  "${DRAFT_CORE_BUNDLE}"
  src/workspace/manifest.cpp
  src/workspace/selection.cpp
  src/workspace/workspace.cpp
  $<TARGET_OBJECTS:draft_llvm_backend>
)

target_include_directories(draft_compiler PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(
  draft_compiler
  PUBLIC draft_build_options Threads::Threads
  PRIVATE ${DRAFT_LLVM_LIBRARY_TARGETS}
)
# The bootstrap compiler is also consumed behind the draft_compiler_service
# shared-library boundary. Position-independent objects keep that embedding
# valid on ELF. Hidden C++ visibility ensures the shared object exposes only
# service.h's explicitly default-visible C ABI rather than the bootstrap's
# accidental implementation symbols.
set_target_properties(
  draft_compiler draft_llvm_backend
  PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

if(MSVC)
  target_compile_options(draft_build_options INTERFACE /W4 /permissive- /GR- /EHs-c-)
  target_compile_definitions(draft_build_options INTERFACE _HAS_EXCEPTIONS=0)
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

# The compiler service is an opaque C ABI over the C++ bootstrap. Draft owns the
# real draftide executable, main procedure, workspace selection, editor, and UI;
# this library owns only compiler-session products and native build operations.
add_library(
  draft_compiler_service SHARED
  src/ide/compiler_session.cpp
  src/ide/service.cpp
)
target_include_directories(
  draft_compiler_service PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_definitions(
  draft_compiler_service
  PRIVATE
    DRAFT_COMPILER_SERVICE_BUILD
)
target_link_libraries(draft_compiler_service PRIVATE draft_compiler)
set_target_properties(
  draft_compiler_service
  PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    OUTPUT_NAME draft_compiler_service
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
)
foreach(configuration DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
  set_target_properties(
    draft_compiler_service
    PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY_${configuration} "${CMAKE_CURRENT_BINARY_DIR}"
      RUNTIME_OUTPUT_DIRECTORY_${configuration} "${CMAKE_CURRENT_BINARY_DIR}"
      ARCHIVE_OUTPUT_DIRECTORY_${configuration} "${CMAKE_CURRENT_BINARY_DIR}"
  )
endforeach()
if(APPLE)
  # The Draft executable and service are installed as siblings. Recording a
  # loader-relative install name keeps the development and installed pair
  # relocatable without an ambient DYLD_LIBRARY_PATH.
  set_target_properties(
    draft_compiler_service
    PROPERTIES
      INSTALL_NAME_DIR "@loader_path"
      BUILD_WITH_INSTALL_NAME_DIR ON
  )
  # Hidden source visibility cannot suppress every default-visible libc++
  # template instantiation pulled from a static archive. The explicit export
  # list makes service.h the only dynamic boundary on Mach-O.
  target_link_options(
    draft_compiler_service
    PRIVATE
      "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/src/ide/service.exports"
  )
  set_property(
    TARGET draft_compiler_service APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ide/service.exports"
  )
elseif(UNIX)
  # The Draft linker adds $ORIGIN to executables that consume a shared foreign
  # provider; a stable soname lets the loader resolve this sibling library.
  set_target_properties(draft_compiler_service PROPERTIES SOVERSION 0)
  # The ELF version script performs the same final-symbol filtering as the
  # Mach-O export list and gives the first bootstrap ABI an explicit version.
  target_link_options(
    draft_compiler_service
    PRIVATE
      "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/ide/service.map"
  )
  set_property(
    TARGET draft_compiler_service APPEND PROPERTY LINK_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ide/service.map"
  )
endif()

# draftc builds DraftIDE as an ordinary Draft executable with a hosted Draft
# main. The service is merely an explicitly mapped foreign provider. The build
# graph remains acyclic: draftc does not consume the service, and the service
# does not call back into the Draft UI.
file(GLOB_RECURSE DRAFTIDE_DRAFT_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/core/*.draft"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/draft_compiler/*.draft"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/draft_compiler_api/*.draft"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/turbo_ui/*.draft"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/turbo_ui_terminal/*.draft"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/turbo_editor/*.draft"
  "${CMAKE_CURRENT_SOURCE_DIR}/lib/turbo_editor_app/*.draft"
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/draftide/*.draft"
)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(DRAFTIDE_HOST_TARGET aarch64-macos)
elseif(
  CMAKE_SYSTEM_NAME STREQUAL "Linux"
  AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$"
)
  set(DRAFTIDE_HOST_TARGET aarch64-linux)
elseif(
  CMAKE_SYSTEM_NAME STREQUAL "Linux"
  AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
)
  set(DRAFTIDE_HOST_TARGET x86_64-linux)
elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(DRAFTIDE_HOST_TARGET x86_64-windows)
endif()

if(DRAFTIDE_HOST_TARGET)
  if(WIN32)
    set(DRAFTIDE_PROVIDER_KIND archive)
    set(DRAFTIDE_PROVIDER_PATH $<TARGET_LINKER_FILE:draft_compiler_service>)
  else()
    set(DRAFTIDE_PROVIDER_KIND shared-library)
    set(DRAFTIDE_PROVIDER_PATH $<TARGET_FILE:draft_compiler_service>)
  endif()
  set(DRAFTIDE_EXECUTABLE
    "${CMAKE_CURRENT_BINARY_DIR}/draftide${CMAKE_EXECUTABLE_SUFFIX}")
  set(DRAFTIDE_BUILD_STAMP "${CMAKE_CURRENT_BINARY_DIR}/draftide-build.stamp")
  add_custom_command(
    OUTPUT "${DRAFTIDE_BUILD_STAMP}"
    BYPRODUCTS "${DRAFTIDE_EXECUTABLE}"
    COMMAND $<TARGET_FILE:draftc> build
      "${CMAKE_CURRENT_SOURCE_DIR}/tools/draftide"
      --target "${DRAFTIDE_HOST_TARGET}" -O2
      --provider
        "draft_compiler_service=${DRAFTIDE_PROVIDER_KIND}:${DRAFTIDE_PROVIDER_PATH}"
      -o "${DRAFTIDE_EXECUTABLE}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${DRAFTIDE_BUILD_STAMP}"
    DEPENDS
      draftc
      draft_compiler_service
      ${DRAFTIDE_DRAFT_SOURCES}
      "${CMAKE_CURRENT_SOURCE_DIR}/draft.workspace"
    COMMENT "Building the Draft-owned DraftIDE executable"
    VERBATIM
  )
  add_custom_target(draftide ALL DEPENDS "${DRAFTIDE_BUILD_STAMP}")
endif()

# A binary Draft distribution is a relocatable compiler, not a thin wrapper
# around the LLVM installation which happened to build it. Keep the public
# executables and compiler-service provider together in bin/, and keep the
# implementation toolchain under libexec/draft so it does not pollute PATH.
# draftc discovers that private directory from its loaded executable path.
set(DRAFT_DISTRIBUTION_RUNTIME_DIRECTORY "libexec/draft")
set(
  DRAFT_DISTRIBUTION_TOOL_DIRECTORY
  "${DRAFT_DISTRIBUTION_RUNTIME_DIRECTORY}/bin"
)
set(
  DRAFT_DISTRIBUTION_LIBRARY_DIRECTORY
  "${DRAFT_DISTRIBUTION_RUNTIME_DIRECTORY}/lib"
)

if(APPLE)
  set(DRAFT_INSTALL_LLVM_RPATH "@loader_path/../libexec/draft/lib")
elseif(UNIX)
  set(DRAFT_INSTALL_LLVM_RPATH "\$ORIGIN/../libexec/draft/lib")
endif()
if(DEFINED DRAFT_INSTALL_LLVM_RPATH)
  set_target_properties(
    draftc draft_compiler_service
    PROPERTIES INSTALL_RPATH "${DRAFT_INSTALL_LLVM_RPATH}"
  )
endif()

install(
  TARGETS draftc draft_compiler_service
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
if(DRAFTIDE_HOST_TARGET)
  # draftide is produced by draftc rather than by the host linker, so it is a
  # generated program rather than a CMake target. Its dependency on the service
  # library is already loader-relative on every supported target.
  install(
    PROGRAMS "${DRAFTIDE_EXECUTABLE}"
    DESTINATION "${CMAKE_INSTALL_BINDIR}"
  )
endif()

function(draft_install_llvm_program name)
  set(path "${LLVM_TOOLS_BINARY_DIR}/${name}${CMAKE_EXECUTABLE_SUFFIX}")
  if(NOT EXISTS "${path}")
    message(FATAL_ERROR
      "LLVM ${LLVM_PACKAGE_VERSION} does not provide required tool ${path}"
    )
  endif()
  # LLVM installations commonly expose an unversioned symlink such as
  # clang -> clang-22. An archive containing only that symlink is broken, so
  # install the resolved program bytes under the stable name draftc launches.
  file(REAL_PATH "${path}" real_path)
  install(
    PROGRAMS "${real_path}"
    DESTINATION "${DRAFT_DISTRIBUTION_TOOL_DIRECTORY}"
    RENAME "${name}${CMAKE_EXECUTABLE_SUFFIX}"
  )
endfunction()

draft_install_llvm_program(clang)
if(APPLE)
  draft_install_llvm_program(dsymutil)
  draft_install_llvm_program(llvm-ar)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  draft_install_llvm_program(ld.lld)
  draft_install_llvm_program(llvm-ar)
elseif(WIN32)
  draft_install_llvm_program(lld-link)
  draft_install_llvm_program(llvm-lib)
endif()

# Clang locates compiler-rt libraries relative to its own executable. Draft
# uses this private driver only for assembly, linking, and selected runtime
# instrumentation; it does not expose a general C compiler, so builtin headers
# would be dead distribution weight. Copy the exact runtime subtree into
# Clang's conventional resource layout.
execute_process(
  COMMAND "${DRAFT_RUNTIME_CLANG}" --print-resource-dir
  RESULT_VARIABLE DRAFT_CLANG_RESOURCE_RESULT
  OUTPUT_VARIABLE DRAFT_CLANG_RESOURCE_DIRECTORY
  OUTPUT_STRIP_TRAILING_WHITESPACE
)
# Clang prints native Windows separators. CMake later writes this configure-time
# value into cmake_install.cmake, where a path such as `D:\a\...` would be
# reparsed as a quoted CMake string and `\a` would become an invalid escape.
# Store the resource directory in CMake's portable slash form before generating
# the install program; Windows system calls accept that form unchanged.
file(
  TO_CMAKE_PATH
  "${DRAFT_CLANG_RESOURCE_DIRECTORY}"
  DRAFT_CLANG_RESOURCE_DIRECTORY
)
if(
  NOT DRAFT_CLANG_RESOURCE_RESULT EQUAL 0
  OR NOT IS_DIRECTORY "${DRAFT_CLANG_RESOURCE_DIRECTORY}"
)
  message(FATAL_ERROR
    "Could not locate the resource directory for ${DRAFT_RUNTIME_CLANG}"
  )
endif()
get_filename_component(
  DRAFT_CLANG_RESOURCE_VERSION
  "${DRAFT_CLANG_RESOURCE_DIRECTORY}"
  NAME
)
install(
  DIRECTORY "${DRAFT_CLANG_RESOURCE_DIRECTORY}/lib"
  DESTINATION
    "${DRAFT_DISTRIBUTION_LIBRARY_DIRECTORY}/clang/${DRAFT_CLANG_RESOURCE_VERSION}"
)

# Unix bootstrap binaries and LLVM tools share LLVM's dynamic libraries. Copy
# only that runtime closure; headers, CMake metadata, static archives, and
# unrelated LLVM executables are build inputs and do not belong in the release.
if(APPLE)
  install(
    FILES
      "${LLVM_LIBRARY_DIRS}/libLLVM.dylib"
      "${LLVM_LIBRARY_DIRS}/libclang-cpp.dylib"
    DESTINATION "${DRAFT_DISTRIBUTION_LIBRARY_DIRECTORY}"
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # The two ELF SONAMEs below are the complete non-system shared-library
  # closure of draftc and the private Clang driver in the supported LLVM build.
  # Install the terminal file behind each development-tree link under that
  # SONAME. This deliberately creates ordinary files in the distribution:
  # Ubuntu's links reach outside LLVM_LIBRARY_DIRS, so copying the links would
  # leave a relocatable archive dependent on the build host's /usr/lib tree.
  set(DRAFT_LLVM_RUNTIME_NAMES
    "libLLVM.so.${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}"
    "libclang-cpp.so.${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}"
  )
  foreach(DRAFT_LLVM_RUNTIME_NAME IN LISTS DRAFT_LLVM_RUNTIME_NAMES)
    set(DRAFT_LLVM_RUNTIME_SOURCE "")
    foreach(DRAFT_LLVM_LIBRARY_DIRECTORY IN LISTS LLVM_LIBRARY_DIRS)
      set(
        DRAFT_LLVM_RUNTIME_CANDIDATE
        "${DRAFT_LLVM_LIBRARY_DIRECTORY}/${DRAFT_LLVM_RUNTIME_NAME}"
      )
      if(EXISTS "${DRAFT_LLVM_RUNTIME_CANDIDATE}")
        set(DRAFT_LLVM_RUNTIME_SOURCE "${DRAFT_LLVM_RUNTIME_CANDIDATE}")
        break()
      endif()
    endforeach()
    if(DRAFT_LLVM_RUNTIME_SOURCE STREQUAL "")
      message(FATAL_ERROR
        "LLVM ${LLVM_PACKAGE_VERSION} has no ${DRAFT_LLVM_RUNTIME_NAME}"
      )
    endif()
    file(
      REAL_PATH "${DRAFT_LLVM_RUNTIME_SOURCE}"
      DRAFT_LLVM_RUNTIME_RESOLVED
    )
    install(
      FILES "${DRAFT_LLVM_RUNTIME_RESOLVED}"
      DESTINATION "${DRAFT_DISTRIBUTION_LIBRARY_DIRECTORY}"
      RENAME "${DRAFT_LLVM_RUNTIME_NAME}"
    )
  endforeach()
elseif(WIN32)
  file(GLOB DRAFT_LLVM_RUNTIME_LIBRARIES CONFIGURE_DEPENDS
    "${LLVM_TOOLS_BINARY_DIR}/*.dll"
  )
  if(DRAFT_LLVM_RUNTIME_LIBRARIES)
    list(SORT DRAFT_LLVM_RUNTIME_LIBRARIES)
    install(
      FILES ${DRAFT_LLVM_RUNTIME_LIBRARIES}
      DESTINATION "${DRAFT_DISTRIBUTION_TOOL_DIRECTORY}"
    )
  endif()
endif()

# The installed source material serves three separate purposes: the docs are
# the public contract, core is readable source for the code embedded in draftc,
# and the skill teaches an external coding agent how to write current Draft.
# The skill's relative links continue to resolve because it retains the same
# repository-shaped location below share/draft.
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md DESTINATION .)
install(FILES AGENTS.md DESTINATION "${CMAKE_INSTALL_DATADIR}/draft")
install(DIRECTORY docs core DESTINATION "${CMAKE_INSTALL_DATADIR}/draft")
install(
  DIRECTORY .agents/skills/write-draft-code
  DESTINATION "${CMAKE_INSTALL_DATADIR}/draft/.agents/skills"
)
install(FILES examples/README.md DESTINATION "${CMAKE_INSTALL_DATADIR}/draft/examples")
install(
  DIRECTORY examples/hello examples/language-tour
  DESTINATION "${CMAKE_INSTALL_DATADIR}/draft/examples"
  # A developer may have built either source example in place. Those ignored
  # .draft directories contain target-specific objects and manifests, not
  # source distribution inputs, so exclude them at the install boundary.
  PATTERN ".draft" EXCLUDE
)

get_filename_component(DRAFT_LLVM_ROOT "${LLVM_TOOLS_BINARY_DIR}" DIRECTORY)

# Homebrew and the Unix LLVM archives keep LICENSE.TXT at the distribution
# root. The official Windows development archive installs the same notice with
# LLVM's public Support headers. Debian-family packages instead place their
# package copyright notice under /usr/share/doc, outside LLVM's versioned lib
# directory. Each is an authoritative input for the exact LLVM distribution
# whose binaries we copy. Select the first existing candidate explicitly so a
# release archive can never silently omit the notice merely because the host
# packaged LLVM in a different conventional layout.
set(DRAFT_LLVM_LICENSE_CANDIDATES
  "${DRAFT_LLVM_ROOT}/LICENSE.TXT"
  "${DRAFT_LLVM_ROOT}/include/llvm/Support/LICENSE.TXT"
)
if(UNIX AND NOT APPLE)
  get_filename_component(DRAFT_LLVM_LIBRARY_PARENT "${DRAFT_LLVM_ROOT}" DIRECTORY)
  get_filename_component(DRAFT_LLVM_SYSTEM_PREFIX "${DRAFT_LLVM_LIBRARY_PARENT}" DIRECTORY)
  list(APPEND DRAFT_LLVM_LICENSE_CANDIDATES
    "${DRAFT_LLVM_SYSTEM_PREFIX}/share/doc/llvm-${LLVM_VERSION_MAJOR}/copyright"
    "${DRAFT_LLVM_SYSTEM_PREFIX}/share/doc/libllvm${LLVM_VERSION_MAJOR}/copyright"
  )
endif()

set(DRAFT_LLVM_LICENSE_FILE "")
foreach(DRAFT_LLVM_LICENSE_CANDIDATE IN LISTS DRAFT_LLVM_LICENSE_CANDIDATES)
  if(EXISTS "${DRAFT_LLVM_LICENSE_CANDIDATE}")
    set(DRAFT_LLVM_LICENSE_FILE "${DRAFT_LLVM_LICENSE_CANDIDATE}")
    break()
  endif()
endforeach()

if(DRAFT_LLVM_LICENSE_FILE)
  install(
    FILES "${DRAFT_LLVM_LICENSE_FILE}"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/draft/licenses/llvm"
    RENAME LLVM-LICENSE.txt
  )
else()
  list(JOIN DRAFT_LLVM_LICENSE_CANDIDATES "\n  " DRAFT_LLVM_LICENSE_CANDIDATE_LIST)
  message(FATAL_ERROR
    "LLVM distribution has no discoverable license notice. Checked:\n"
    "  ${DRAFT_LLVM_LICENSE_CANDIDATE_LIST}"
  )
endif()

if(APPLE)
  # Homebrew LLVM currently records an absolute install name in binaries which
  # link libLLVM directly. Rewrite only that known dependency after CMake has
  # installed its final RPATH, then restore an ad-hoc signature. Official
  # release CI may replace this with a Developer ID signature and notarization.
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/fixup_macos_distribution.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/fixup_macos_distribution.cmake"
    @ONLY
  )
  install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/fixup_macos_distribution.cmake")
endif()

# CPack is intentionally a thin archive over `cmake --install`: local installs,
# CI smoke tests, and published archives therefore exercise one file manifest.
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
  set(DRAFT_RELEASE_PLATFORM "aarch64-macos")
  set(CPACK_GENERATOR TXZ)
elseif(
  CMAKE_SYSTEM_NAME STREQUAL "Linux"
  AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$"
)
  set(DRAFT_RELEASE_PLATFORM "aarch64-linux")
  set(CPACK_GENERATOR TXZ)
elseif(
  CMAKE_SYSTEM_NAME STREQUAL "Linux"
  AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
)
  set(DRAFT_RELEASE_PLATFORM "x86_64-linux")
  set(CPACK_GENERATOR TXZ)
elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  set(DRAFT_RELEASE_PLATFORM "x86_64-windows")
  set(CPACK_GENERATOR ZIP)
endif()
if(DEFINED DRAFT_RELEASE_PLATFORM)
  set(CPACK_PACKAGE_NAME "draft")
  set(CPACK_PACKAGE_VENDOR "Draft")
  set(CPACK_PACKAGE_VERSION "${DRAFT_RELEASE_VERSION}")
  set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Draft experimental systems language")
  set(CPACK_PACKAGE_FILE_NAME
      "draft-${DRAFT_RELEASE_VERSION}-${DRAFT_RELEASE_PLATFORM}")
  set(CPACK_PACKAGE_CHECKSUM SHA256)
  set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
  include(CPack)
endif()

include(CTest)
if(BUILD_TESTING)
  if(DRAFTIDE_HOST_TARGET)
    # This smoke is deliberately provider-backed and noninteractive. It proves
    # that the executable produced by Draft can create the opaque C++ compiler
    # service, retrieve production lexer/semantic products, and render one UI
    # frame without making the test runner enter a terminal session.
    add_test(
      NAME draft_draftide_smoke
      COMMAND
        "${DRAFTIDE_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}" --smoke
    )
    set_tests_properties(
      draft_draftide_smoke PROPERTIES LABELS "ide;integration"
    )
    # examples/hello is itself a one-root workspace and intentionally contains
    # no draft.workspace. This companion smoke proves a standalone package is
    # itself a valid workspace rather than requiring a manifest.
    add_test(
      NAME draft_draftide_auto_root_smoke
      COMMAND
        "${DRAFTIDE_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/examples/hello" --smoke
    )
    set_tests_properties(
      draft_draftide_auto_root_smoke PROPERTIES LABELS "ide;integration"
    )
    add_test(
      NAME draft_draftide_version
      COMMAND "${DRAFTIDE_EXECUTABLE}" --version
    )
    set_tests_properties(
      draft_draftide_version
      PROPERTIES
        LABELS "ide;release"
        PASS_REGULAR_EXPRESSION "draftide ${DRAFT_RELEASE_VERSION}"
    )
    if(NOT DRAFT_ASAN_PRELOAD STREQUAL "")
      # Only the three mixed Draft/C++ service processes need preloading. The
      # remaining native tests intentionally exercise ordinary generated Draft
      # programs without silently turning bootstrap sanitizer policy into a
      # target-program option.
      set_tests_properties(
        draft_draftide_smoke
        draft_draftide_auto_root_smoke
        draft_draftide_version
        PROPERTIES
          ENVIRONMENT_MODIFICATION
            "LD_PRELOAD=path_list_prepend:${DRAFT_ASAN_PRELOAD}"
      )
    endif()
  endif()

  add_test(NAME draft_compiler_version COMMAND draftc --version)
  set_tests_properties(
    draft_compiler_version
    PROPERTIES
      LABELS "release"
      PASS_REGULAR_EXPRESSION "draftc ${DRAFT_RELEASE_VERSION}"
  )

  if(DRAFTIDE_HOST_TARGET)
    # Install into a clean prefix and exercise that prefix without a source-tree
    # core directory or build-tree service library. This is the release archive
    # contract, not merely a check that CMake copied some files.
    add_test(
      NAME draft_distribution_install_smoke
      COMMAND
        "${CMAKE_COMMAND}"
        "-DBUILD_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}"
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/distribution-smoke"
        "-DDRAFT_TARGET=${DRAFTIDE_HOST_TARGET}"
        "-DDRAFT_RELEASE_VERSION=${DRAFT_RELEASE_VERSION}"
        "-DDRAFT_ASAN_PRELOAD=${DRAFT_ASAN_PRELOAD}"
        "-DCMAKE_INSTALL_CONFIG_NAME=$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/install_distribution_smoke_test.cmake"
    )
    set_tests_properties(
      draft_distribution_install_smoke
      PROPERTIES LABELS "release;distribution;integration"
    )
  endif()

  # The complete native matrix must execute the artifacts it just built. Keep
  # it enabled only when the host exactly matches an implemented target pair.
  # Other hosts still build every target-independent compiler, ABI, and
  # lowering test; they simply cannot launch the produced artifact.
  set(DRAFT_NATIVE_HOST OFF)
  if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(DRAFT_NATIVE_HOST ON)
    set(DRAFT_NATIVE_TEST_TARGET aarch64-macos)
  elseif(
    CMAKE_SYSTEM_NAME STREQUAL "Linux"
    AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$"
  )
    set(DRAFT_NATIVE_HOST ON)
    set(DRAFT_NATIVE_TEST_TARGET aarch64-linux)
  elseif(
    CMAKE_SYSTEM_NAME STREQUAL "Linux"
    AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$"
  )
    set(DRAFT_NATIVE_HOST ON)
    set(DRAFT_NATIVE_TEST_TARGET x86_64-linux)
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

  add_executable(
    draft_semantic_work_graph_tests tests/semantic_work_graph_test.cpp)
  target_link_libraries(draft_semantic_work_graph_tests PRIVATE draft_compiler)
  add_test(
    NAME draft_semantic_work_graph_tests COMMAND draft_semantic_work_graph_tests)

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

  if(DRAFT_NATIVE_HOST)
    add_executable(
      draft_compiler_service_tests
      tests/compiler_service_test.cpp
    )
    target_link_libraries(
      draft_compiler_service_tests PRIVATE draft_compiler_service
    )
    add_test(
      NAME draft_compiler_service_tests
      COMMAND draft_compiler_service_tests
    )
    set_tests_properties(
      draft_compiler_service_tests PROPERTIES LABELS "ide;integration"
    )
    if(NOT DRAFT_ASAN_PRELOAD STREQUAL "")
      set_tests_properties(
        draft_compiler_service_tests
        PROPERTIES
          ENVIRONMENT_MODIFICATION
            "LD_PRELOAD=path_list_prepend:${DRAFT_ASAN_PRELOAD}"
      )
    endif()
  endif()

  add_executable(draft_parser_tests tests/parser_test.cpp)
  target_link_libraries(draft_parser_tests PRIVATE draft_compiler)
  add_test(NAME draft_parser_tests COMMAND draft_parser_tests)

  add_executable(draft_package_tests tests/package_test.cpp)
  target_link_libraries(draft_package_tests PRIVATE draft_compiler)
  add_test(NAME draft_package_tests COMMAND draft_package_tests)

  add_executable(draft_workspace_tests tests/workspace_test.cpp)
  target_link_libraries(draft_workspace_tests PRIVATE draft_compiler)
  add_test(NAME draft_workspace_tests COMMAND draft_workspace_tests)

  add_executable(draft_embedded_core_tests tests/embedded_core_test.cpp)
  target_link_libraries(draft_embedded_core_tests PRIVATE draft_compiler)
  add_test(NAME draft_embedded_core_tests COMMAND draft_embedded_core_tests)

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

  add_executable(
    draft_native_reachability_tests
    tests/native_reachability_test.cpp
  )
  target_link_libraries(draft_native_reachability_tests PRIVATE draft_compiler)
  add_test(
    NAME draft_native_reachability_tests
    COMMAND draft_native_reachability_tests
  )

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
    draft_llvm_package_emitter_tests
    tests/llvm_package_emitter_test.cpp
  )
  target_link_libraries(draft_llvm_package_emitter_tests PRIVATE draft_compiler)
  add_test(
    NAME draft_llvm_package_emitter_tests
    COMMAND draft_llvm_package_emitter_tests
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

  add_executable(
    draft_hosted_runtime_bundle_tests
    tests/hosted_runtime_bundle_test.cpp
  )
  target_link_libraries(draft_hosted_runtime_bundle_tests PRIVATE draft_compiler)
  add_test(
    NAME draft_hosted_runtime_bundle_tests
    COMMAND draft_hosted_runtime_bundle_tests
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

  add_executable(draft_x86_64_abi_tests tests/x86_64_abi_test.cpp)
  target_link_libraries(draft_x86_64_abi_tests PRIVATE draft_compiler)
  add_test(NAME draft_x86_64_abi_tests COMMAND draft_x86_64_abi_tests)

  add_executable(draft_win64_abi_tests tests/win64_abi_test.cpp)
  target_link_libraries(draft_win64_abi_tests PRIVATE draft_compiler)
  add_test(NAME draft_win64_abi_tests COMMAND draft_win64_abi_tests)

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
  # tests. Each supported host selects its matching Draft target and proves
  # that real links, launches, debug contracts, and C ABI calls work.
  if(DRAFT_NATIVE_HOST)
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
      draft_c_client_integration_tests PRIVATE draft_compiler
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
  # checks successfully on every target or fails with its declared unresolved
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
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/judgment-tour/app"
  )
  add_test(
    NAME draft_judgment_tour_list
    COMMAND draftc judge
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/judgment-tour/app" --list
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
      "identity draft-aarch64-linux-gnu-v2.*triple aarch64-unknown-linux-gnu"
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
  add_test(
    NAME draft_x86_64_linux_target_driver
    COMMAND draftc target --target x86_64-linux
  )
  set_tests_properties(
    draft_x86_64_linux_target_driver
    PROPERTIES PASS_REGULAR_EXPRESSION
      "identity draft-x86_64-linux-gnu-v2.*triple x86_64-unknown-linux-gnu"
  )
  add_test(
    NAME draft_x86_64_linux_check_driver
    COMMAND draftc check "${CMAKE_CURRENT_SOURCE_DIR}/examples/tetris"
      --target x86_64-linux
  )
  set_tests_properties(
    draft_x86_64_linux_check_driver
    PROPERTIES PASS_REGULAR_EXPRESSION
      "checked package graph rooted at tetris"
  )
  add_test(
    NAME draft_x86_64_windows_target_driver
    COMMAND draftc target --target x86_64-windows
  )
  set_tests_properties(
    draft_x86_64_windows_target_driver
    PROPERTIES PASS_REGULAR_EXPRESSION
      "identity draft-x86_64-windows-msvc-v2.*triple x86_64-pc-windows-msvc"
  )
  add_test(
    NAME draft_x86_64_windows_c_abi_check_driver
    COMMAND draftc check "${CMAKE_CURRENT_SOURCE_DIR}/core/c_abi"
      --target x86_64-windows
  )
  set_tests_properties(
    draft_x86_64_windows_c_abi_check_driver
    PROPERTIES PASS_REGULAR_EXPRESSION
      "checked package graph rooted at c_abi"
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

  # Optimization is a native artifact choice shared by ordinary, resolution-
  # continuation, test, and benchmark builds. Keep both fixtures below the
  # binary tree because these commands deliberately create workspace-owned
  # build and evidence state.
  if(DRAFT_NATIVE_HOST)
    add_test(
      NAME draft_driver_optimization
      COMMAND
        "${CMAKE_COMMAND}"
        "-DDRAFTC=$<TARGET_FILE:draftc>"
        "-DSOURCE_WORKSPACE=${CMAKE_CURRENT_SOURCE_DIR}/examples/packages"
        "-DSOURCE_VALIDATION=${CMAKE_CURRENT_SOURCE_DIR}/examples/validation"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/driver-optimization-test"
        "-DTARGET_SELECTOR=${DRAFT_NATIVE_TEST_TARGET}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_optimization_test.cmake"
    )
  endif()

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
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/agent-pending/app"
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
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/agent-pending/app"
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
      "${CMAKE_CURRENT_SOURCE_DIR}/examples/agent-acceptance/app"
      --regenerate site-does-not-exist
  )
  set_tests_properties(
    draft_regeneration_rejects_unknown_site
    PROPERTIES
      WILL_FAIL TRUE
  )

  if(DRAFT_NATIVE_HOST)
    # `run` is a foreground native-host operation. This process-level fixture
    # proves default-program selection, configured environment/cwd, and the
    # exact CLI separator contract through the real driver and hosted runtime.
    add_test(
      NAME draft_driver_run
      COMMAND
        "${CMAKE_COMMAND}"
        "-DDRAFTC=$<TARGET_FILE:draftc>"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/driver-run-test"
        "-DTARGET_SELECTOR=${DRAFT_NATIVE_TEST_TARGET}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_run_test.cmake"
    )
    set_tests_properties(
      draft_driver_run
      PROPERTIES LABELS "examples;integration"
    )

    # The exhaustive example inventory also owns the public command-shape
    # proof. Run every ordinary executable from a private copy, selecting
    # dedicated multi-package workspaces by their checked-in default program.
    add_test(
      NAME draft_example_direct_run
      COMMAND
        "${CMAKE_COMMAND}"
        "-DDRAFTC=$<TARGET_FILE:draftc>"
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DMATRIX=${CMAKE_CURRENT_SOURCE_DIR}/examples/qualification.tsv"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/example-direct-run"
        "-DTARGET_SELECTOR=${DRAFT_NATIVE_TEST_TARGET}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_example_direct_run_test.cmake"
    )
    set_tests_properties(
      draft_example_direct_run
      PROPERTIES LABELS "examples;integration"
    )

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
    # ordinary provider-free builds are therefore exercised on every native CI
    # host, alongside the handwritten validation packages.
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

    # core/terminal owns target-specific libc layouts but its unit tests avoid
    # changing the CTest process terminal. Run them from an isolated copied
    # workspace so evidence and native objects never enter the source checkout.
    add_test(
      NAME draft_core_terminal_tests
      COMMAND
        "${CMAKE_COMMAND}"
        "-DDRAFTC=$<TARGET_FILE:draftc>"
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/core-terminal-test"
        "-DTARGET_SELECTOR=${DRAFT_NATIVE_TEST_TARGET}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_core_terminal_test.cmake"
    )
    set_tests_properties(
      draft_core_terminal_tests
      PROPERTIES LABELS "integration"
    )

    # core/process owns the exact argv/environment/directory conversion on each
    # hosted target. Its native test launches fixed system executables from an
    # isolated copied workspace so no validation state reaches the checkout.
    add_test(
      NAME draft_core_process_tests
      COMMAND
        "${CMAKE_COMMAND}"
        "-DDRAFTC=$<TARGET_FILE:draftc>"
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/core-process-test"
        "-DTARGET_SELECTOR=${DRAFT_NATIVE_TEST_TARGET}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_core_process_test.cmake"
    )
    set_tests_properties(
      draft_core_process_tests
      PROPERTIES LABELS "integration"
    )

    # core/random combines a Context-provider boundary with deterministic
    # application streams. Native tests install fake providers so exact bytes,
    # failure, range, and sequence behavior remain reproducible.
    add_test(
      NAME draft_core_random_tests
      COMMAND
        "${CMAKE_COMMAND}"
        "-DDRAFTC=$<TARGET_FILE:draftc>"
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/core-random-test"
        "-DTARGET_SELECTOR=${DRAFT_NATIVE_TEST_TARGET}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_core_random_test.cmake"
    )
    set_tests_properties(
      draft_core_random_tests
      PROPERTIES LABELS "integration"
    )

    # core/unicode pins target-independent grapheme and terminal-width policy,
    # but its tests execute native Draft code. The copied workspace prevents
    # test evidence and object files from entering the source checkout.
    add_test(
      NAME draft_core_unicode_tests
      COMMAND
        "${CMAKE_COMMAND}"
        "-DDRAFTC=$<TARGET_FILE:draftc>"
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/core-unicode-test"
        "-DTARGET_SELECTOR=${DRAFT_NATIVE_TEST_TARGET}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_core_unicode_test.cmake"
    )
    set_tests_properties(
      draft_core_unicode_tests
      PROPERTIES LABELS "integration"
    )

    # core/tui is target-independent Draft policy over core/terminal, but its
    # tests execute the real allocator/runtime and therefore belong in the
    # native host matrix. The copied workspace keeps validation evidence out of
    # source while exact diff bytes are checked without terminal side effects.
    add_test(
      NAME draft_core_tui_tests
      COMMAND
        "${CMAKE_COMMAND}"
        "-DDRAFTC=$<TARGET_FILE:draftc>"
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/core-tui-test"
        "-DTARGET_SELECTOR=${DRAFT_NATIVE_TEST_TARGET}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_core_tui_test.cmake"
    )
    set_tests_properties(
      draft_core_tui_tests
      PROPERTIES LABELS "integration"
    )

    # The reusable Turbo libraries and project-manifest parser are ordinary
    # Draft packages. Run their same-package tests from a copied workspace so
    # editor buffer ownership, root synchronization, widgets, and manifest
    # grammar are exercised without publishing evidence in the source tree.
    add_test(
      NAME draft_turbo_ide_library_tests
      COMMAND
        "${CMAKE_COMMAND}"
        "-DDRAFTC=$<TARGET_FILE:draftc>"
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/turbo-ide-library-test"
        "-DTARGET_SELECTOR=${DRAFT_NATIVE_TEST_TARGET}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_turbo_ide_library_test.cmake"
    )
    set_tests_properties(
      draft_turbo_ide_library_tests
      PROPERTIES LABELS "ide;integration"
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

    # Build the exact vendored raylib source with its software framebuffer,
    # then map that shared artifact into the graphical Asteroids application.
    # The headless backend exercises the real public ABI and renderer on every
    # macOS/Linux native CI host without requiring a display server.
    add_test(
      NAME draft_raylib_asteroids_example
      COMMAND
        "${CMAKE_COMMAND}"
        "-DDRAFTC=$<TARGET_FILE:draftc>"
        "-DSOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/raylib-asteroids-example"
        "-DTARGET_SELECTOR=${DRAFT_NATIVE_TEST_TARGET}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/driver_raylib_asteroids_test.cmake"
    )
    set_tests_properties(
      draft_raylib_asteroids_example
      PROPERTIES LABELS "examples;integration"
    )
  endif()
endif()
