# Generates the byte table for Draft's internal Codex reference bundle.
#
# The focused references under the repository coding skill remain the single
# editable copies. This script embeds an explicit, sorted subset: factual Draft
# language and library guidance only. In particular it cannot silently include
# repository workflow instructions, skill metadata, editor backups, credentials,
# or the implementation source of core packages.

if(NOT DEFINED DRAFT_REFERENCE_ROOT OR NOT DEFINED DRAFT_REFERENCE_OUTPUT)
  message(FATAL_ERROR
    "DRAFT_REFERENCE_ROOT and DRAFT_REFERENCE_OUTPUT are required")
endif()

set(
  draft_reference_files
  "agent-features.md"
  "core-library.md"
  "interop-and-targets.md"
  "language.md"
  "memory-and-ownership.md"
)

get_filename_component(output_directory "${DRAFT_REFERENCE_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${DRAFT_REFERENCE_OUTPUT}"
  "// Generated from the factual write-draft-code references; do not edit.\n"
  "// The checked-in reference files are the source of truth for these bytes.\n\n"
  "#include \"elaborator/draft_reference.h\"\n\n"
  "#include <span>\n"
  "#include <string_view>\n\n"
  "namespace draft {\n"
  "namespace {\n\n"
)

set(file_index 0)
foreach(relative_path IN LISTS draft_reference_files)
  set(input_path "${DRAFT_REFERENCE_ROOT}/${relative_path}")
  if(NOT EXISTS "${input_path}")
    message(FATAL_ERROR "Draft reference input is missing: ${input_path}")
  endif()
  file(READ "${input_path}" file_hex HEX)
  string(LENGTH "${file_hex}" hex_length)
  file(APPEND "${DRAFT_REFERENCE_OUTPUT}"
    "const unsigned char kDraftReferenceFile${file_index}[] = {\n  "
  )
  if(hex_length GREATER 0)
    math(EXPR last_byte "(${hex_length} / 2) - 1")
    foreach(byte_index RANGE 0 ${last_byte})
      math(EXPR hex_offset "${byte_index} * 2")
      string(SUBSTRING "${file_hex}" ${hex_offset} 2 byte_hex)
      file(APPEND "${DRAFT_REFERENCE_OUTPUT}" "0x${byte_hex},")
      math(EXPR line_column "(${byte_index} + 1) % 16")
      if(line_column EQUAL 0)
        file(APPEND "${DRAFT_REFERENCE_OUTPUT}" "\n  ")
      endif()
    endforeach()
  endif()
  file(APPEND "${DRAFT_REFERENCE_OUTPUT}" "\n};\n\n")
  math(EXPR file_index "${file_index} + 1")
endforeach()

file(APPEND "${DRAFT_REFERENCE_OUTPUT}"
  "const EmbeddedDraftReferenceFile kDraftReferenceFiles[] = {\n"
)
set(file_index 0)
foreach(relative_path IN LISTS draft_reference_files)
  file(APPEND "${DRAFT_REFERENCE_OUTPUT}"
    "    {\"${relative_path}\", std::string_view(\n"
    "         reinterpret_cast<const char *>(kDraftReferenceFile${file_index}),\n"
    "         sizeof(kDraftReferenceFile${file_index}))},\n"
  )
  math(EXPR file_index "${file_index} + 1")
endforeach()
file(APPEND "${DRAFT_REFERENCE_OUTPUT}"
  "};\n\n"
  "} // namespace\n\n"
  "std::span<const EmbeddedDraftReferenceFile> embedded_draft_reference_files() {\n"
  "  return kDraftReferenceFiles;\n"
  "}\n\n"
  "} // namespace draft\n"
)
