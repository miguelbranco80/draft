# Generates the immutable source table for the compiler-distributed core.
#
# Every recognized core source is an intentional distribution input. Paths are
# sorted relative to the core root, then both path and bytes feed one build-time
# SHA-256 identity. The compiler performs no runtime filesystem probe or hash.

if(NOT DEFINED DRAFT_CORE_ROOT OR NOT DEFINED DRAFT_CORE_OUTPUT)
  message(FATAL_ERROR "DRAFT_CORE_ROOT and DRAFT_CORE_OUTPUT are required")
endif()

file(GLOB_RECURSE draft_core_files
  RELATIVE "${DRAFT_CORE_ROOT}"
  "${DRAFT_CORE_ROOT}/*.draft"
  "${DRAFT_CORE_ROOT}/*.s"
  "${DRAFT_CORE_ROOT}/*.S"
  "${DRAFT_CORE_ROOT}/*.asm"
)
list(SORT draft_core_files)
if(NOT draft_core_files)
  message(FATAL_ERROR "Draft core bundle contains no source files")
endif()

set(identity_input "draft.embedded-core.v1;")
get_filename_component(output_directory "${DRAFT_CORE_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${DRAFT_CORE_OUTPUT}"
  "// Generated from core/; do not edit.\n\n"
  "#include \"workspace/embedded_core.h\"\n\n"
  "#include <span>\n"
  "#include <string_view>\n\n"
  "namespace draft {\n"
  "namespace {\n\n"
)

set(file_index 0)
set(draft_core_lengths)
foreach(relative_path IN LISTS draft_core_files)
  set(input_path "${DRAFT_CORE_ROOT}/${relative_path}")
  file(READ "${input_path}" file_hex HEX)
  string(LENGTH "${file_hex}" hex_length)
  math(EXPR byte_length "${hex_length} / 2")
  string(LENGTH "${relative_path}" path_length)
  string(APPEND identity_input
    "${path_length}:${relative_path}:${byte_length}:${file_hex};"
  )
  list(APPEND draft_core_lengths "${byte_length}")
  file(APPEND "${DRAFT_CORE_OUTPUT}"
    "const unsigned char kDraftCoreFile${file_index}[] = {\n  "
  )
  if(hex_length GREATER 0)
    math(EXPR last_byte "(${hex_length} / 2) - 1")
    foreach(byte_index RANGE 0 ${last_byte})
      math(EXPR hex_offset "${byte_index} * 2")
      string(SUBSTRING "${file_hex}" ${hex_offset} 2 byte_hex)
      file(APPEND "${DRAFT_CORE_OUTPUT}" "0x${byte_hex},")
      math(EXPR line_column "(${byte_index} + 1) % 16")
      if(line_column EQUAL 0)
        file(APPEND "${DRAFT_CORE_OUTPUT}" "\n  ")
      endif()
    endforeach()
  else()
    # C++ has no standard zero-length array. The sentinel is excluded from the
    # string_view below, so an empty source remains exactly zero bytes.
    file(APPEND "${DRAFT_CORE_OUTPUT}" "0x00,")
  endif()
  file(APPEND "${DRAFT_CORE_OUTPUT}" "\n};\n\n")
  math(EXPR file_index "${file_index} + 1")
endforeach()

string(SHA256 core_identity "${identity_input}")
file(APPEND "${DRAFT_CORE_OUTPUT}"
  "const EmbeddedPackageFile kDraftCoreFiles[] = {\n"
)
set(file_index 0)
foreach(relative_path IN LISTS draft_core_files)
  list(GET draft_core_lengths ${file_index} byte_length)
  file(APPEND "${DRAFT_CORE_OUTPUT}"
    "    {\"${relative_path}\", std::string_view(\n"
    "         reinterpret_cast<const char *>(kDraftCoreFile${file_index}),\n"
    "         ${byte_length})},\n"
  )
  math(EXPR file_index "${file_index} + 1")
endforeach()
file(APPEND "${DRAFT_CORE_OUTPUT}"
  "};\n\n"
  "} // namespace\n\n"
  "std::span<const EmbeddedPackageFile> embedded_core_files() {\n"
  "  return kDraftCoreFiles;\n"
  "}\n\n"
  "std::string_view embedded_core_content_identity() {\n"
  "  return \"draft-core:${core_identity}\";\n"
  "}\n\n"
  "} // namespace draft\n"
)
