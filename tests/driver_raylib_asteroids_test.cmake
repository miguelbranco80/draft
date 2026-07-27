# Native headless link-and-render gate for the vendored raylib example.
#
# The test configures the exact vendored raylib source as a shared library with
# its PLATFORM_MEMORY software renderer, builds the Draft application through
# the public `raylib` provider mapping, and launches `--smoke`. No display server
# or preinstalled raylib package participates. A successful process proves the
# selected C ABI for Color/Vector2, imported symbols, dynamic-library input,
# runtime loading, fixed-step simulation, zero-terminated text, and every draw
# call used by the application.
#
# All derived state lives below TEST_ROOT. The shared provider is intentional:
# desktop raylib owns platform libraries internally, and the same Draft foreign
# declaration therefore works with desktop dylib/so/DLL builds without adding
# target-specific link flags to the Draft package.

if(NOT DEFINED DRAFTC OR
   NOT DEFINED SOURCE_ROOT OR
   NOT DEFINED TEST_ROOT OR
   NOT DEFINED TARGET_SELECTOR)
  message(FATAL_ERROR
    "DRAFTC, SOURCE_ROOT, TEST_ROOT, and TARGET_SELECTOR are required")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
file(COPY "${SOURCE_ROOT}/examples/raylib-asteroids" DESTINATION "${TEST_ROOT}")
set(workspace "${TEST_ROOT}/raylib-asteroids")
set(raylib_source "${SOURCE_ROOT}/vendor/raylib")
set(raylib_build "${TEST_ROOT}/raylib-build")
set(configure_command
  "${CMAKE_COMMAND}"
  -S "${raylib_source}"
  -B "${raylib_build}"
  -DBUILD_EXAMPLES=OFF
  -DBUILD_SHARED_LIBS=ON
  -DPLATFORM=Memory
  -DCUSTOMIZE_BUILD=ON
  -DSUPPORT_MODULE_RAUDIO=OFF
  # This one-frame headless gate drives no platform event loop. Keep custom
  # frame control explicit because raylib 6.0's memory backend has no Apple
  # GetTime implementation; enabling its ordinary frame pacing on macOS would
  # wait forever. Desktop builds must disable this option as documented by the
  # example because their windows require EndDrawing to poll platform events.
  -DSUPPORT_CUSTOM_FRAME_CONTROL=ON
  -DSUPPORT_TRACELOG=OFF
  -DCMAKE_BUILD_TYPE=Release
)
if(UNIX AND NOT APPLE)
  # Raylib's normal Linux desktop configuration links libm publicly. Its
  # headless PLATFORM_MEMORY configuration omits that row even though the same
  # raymath and software-renderer objects use the C math API. Supply libm as a
  # standard C link input so the resulting shared provider records its own
  # dependency instead of asking Draft's final link to guess provider-private
  # libraries.
  list(APPEND configure_command -DCMAKE_C_STANDARD_LIBRARIES=-lm)
endif()
if(WIN32)
  list(APPEND configure_command -A x64)
endif()

execute_process(
  COMMAND ${configure_command}
  RESULT_VARIABLE configure_status
  OUTPUT_VARIABLE configure_stdout
  ERROR_VARIABLE configure_stderr
)
if(NOT configure_status EQUAL 0)
  message(FATAL_ERROR
    "headless raylib configure failed (${configure_status})\n"
    "stdout:\n${configure_stdout}\nstderr:\n${configure_stderr}")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${raylib_build}"
    --config Release --parallel 4
  RESULT_VARIABLE raylib_status
  OUTPUT_VARIABLE raylib_stdout
  ERROR_VARIABLE raylib_stderr
)
if(NOT raylib_status EQUAL 0)
  message(FATAL_ERROR
    "headless raylib build failed (${raylib_status})\n"
    "stdout:\n${raylib_stdout}\nstderr:\n${raylib_stderr}")
endif()

# CMake's shared-library publication differs only at the host artifact seam.
# Select one exact linker input and, on Windows, the sibling runtime DLL. The
# explicit Release/single-config candidates avoid filesystem enumeration and
# cover the two generator shapes used by repository development and CI.
set(provider_kind shared-library)
set(runtime_directory "${raylib_build}/raylib")
if(APPLE)
  file(REAL_PATH "${raylib_build}/raylib/libraylib.dylib" provider)
elseif(WIN32)
  set(provider_kind archive)
  set(provider_candidates
    "${raylib_build}/raylib/Release/raylib.lib"
    "${raylib_build}/raylib.lib"
  )
  set(runtime_candidates
    "${raylib_build}/raylib/Release/raylib.dll"
    "${raylib_build}/raylib.dll"
  )
  foreach(candidate IN LISTS provider_candidates)
    if(EXISTS "${candidate}")
      set(provider "${candidate}")
      break()
    endif()
  endforeach()
  foreach(candidate IN LISTS runtime_candidates)
    if(EXISTS "${candidate}")
      set(runtime_library "${candidate}")
      break()
    endif()
  endforeach()
  if(NOT provider OR NOT runtime_library)
    message(FATAL_ERROR
      "raylib Windows build did not publish raylib.lib and raylib.dll")
  endif()
  get_filename_component(runtime_directory "${runtime_library}" DIRECTORY)
else()
  file(REAL_PATH "${raylib_build}/raylib/libraylib.so" provider)
endif()
if(NOT EXISTS "${provider}")
  message(FATAL_ERROR "raylib build did not publish provider input ${provider}")
endif()

set(program "${TEST_ROOT}/draft-raylib-asteroids")
if(WIN32)
  string(APPEND program ".exe")
endif()
execute_process(
  COMMAND "${DRAFTC}" build
    "${workspace}/app"
    --target "${TARGET_SELECTOR}" -O2
    --provider "raylib=${provider_kind}:${provider}"
    -o "${program}"
  RESULT_VARIABLE draft_status
  OUTPUT_VARIABLE draft_stdout
  ERROR_VARIABLE draft_stderr
)
if(NOT draft_status EQUAL 0)
  message(FATAL_ERROR
    "Draft raylib Asteroids build failed (${draft_status})\n"
    "stdout:\n${draft_stdout}\nstderr:\n${draft_stderr}")
endif()

if(APPLE)
  set(run_command
    "${CMAKE_COMMAND}" -E env
    "DYLD_LIBRARY_PATH=${runtime_directory}"
    "${program}" --smoke
  )
elseif(WIN32)
  file(COPY "${runtime_library}" DESTINATION "${TEST_ROOT}")
  set(run_command "${program}" --smoke)
else()
  set(run_command
    "${CMAKE_COMMAND}" -E env
    "LD_LIBRARY_PATH=${runtime_directory}"
    "${program}" --smoke
  )
endif()

execute_process(
  COMMAND ${run_command}
  WORKING_DIRECTORY "${TEST_ROOT}"
  RESULT_VARIABLE program_status
  OUTPUT_VARIABLE program_stdout
  ERROR_VARIABLE program_stderr
)
if(NOT program_status EQUAL 0)
  message(FATAL_ERROR
    "Draft raylib Asteroids smoke failed (${program_status})\n"
    "stdout:\n${program_stdout}\nstderr:\n${program_stderr}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
message(STATUS "raylib Asteroids headless native integration passed")
