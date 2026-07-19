# Generates the byte table for Draft's bundled Codex coding skill.
#
# The source skill remains the single editable copy under .agents/. This script
# reads an explicit, sorted file list and emits one C++ translation unit into the
# build tree. An explicit list prevents an editor backup, credential, or other
# incidental file from silently entering the compiler binary. CMake reruns this
# command whenever one listed input changes.

if(NOT DEFINED DRAFT_SKILL_ROOT OR NOT DEFINED DRAFT_SKILL_OUTPUT)
  message(FATAL_ERROR "DRAFT_SKILL_ROOT and DRAFT_SKILL_OUTPUT are required")
endif()

set(
  draft_skill_files
  "SKILL.md"
  "agents/openai.yaml"
  "references/agent-features.md"
  "references/core-library.md"
  "references/interop-and-targets.md"
  "references/language.md"
  "references/memory-and-ownership.md"
  "references/workflow-and-testing.md"
)

get_filename_component(output_directory "${DRAFT_SKILL_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${DRAFT_SKILL_OUTPUT}"
  "// Generated from .agents/skills/write-draft-code; do not edit.\n"
  "// The checked-in skill is the source of truth for these exact bytes.\n\n"
  "#include \"elaborator/draft_skill.h\"\n\n"
  "#include <span>\n"
  "#include <string_view>\n\n"
  "namespace draft {\n"
  "namespace {\n\n"
)

set(file_index 0)
foreach(relative_path IN LISTS draft_skill_files)
  set(input_path "${DRAFT_SKILL_ROOT}/${relative_path}")
  if(NOT EXISTS "${input_path}")
    message(FATAL_ERROR "Draft skill input is missing: ${input_path}")
  endif()
  file(READ "${input_path}" file_hex HEX)
  string(LENGTH "${file_hex}" hex_length)
  file(APPEND "${DRAFT_SKILL_OUTPUT}"
    "const unsigned char kDraftSkillFile${file_index}[] = {\n  "
  )
  if(hex_length GREATER 0)
    math(EXPR last_byte "(${hex_length} / 2) - 1")
    foreach(byte_index RANGE 0 ${last_byte})
      math(EXPR hex_offset "${byte_index} * 2")
      string(SUBSTRING "${file_hex}" ${hex_offset} 2 byte_hex)
      file(APPEND "${DRAFT_SKILL_OUTPUT}" "0x${byte_hex},")
      math(EXPR line_column "(${byte_index} + 1) % 16")
      if(line_column EQUAL 0)
        file(APPEND "${DRAFT_SKILL_OUTPUT}" "\n  ")
      endif()
    endforeach()
  endif()
  file(APPEND "${DRAFT_SKILL_OUTPUT}" "\n};\n\n")
  math(EXPR file_index "${file_index} + 1")
endforeach()

file(APPEND "${DRAFT_SKILL_OUTPUT}"
  "const EmbeddedDraftSkillFile kDraftSkillFiles[] = {\n"
)
set(file_index 0)
foreach(relative_path IN LISTS draft_skill_files)
  file(APPEND "${DRAFT_SKILL_OUTPUT}"
    "    {\"${relative_path}\", std::string_view(\n"
    "         reinterpret_cast<const char *>(kDraftSkillFile${file_index}),\n"
    "         sizeof(kDraftSkillFile${file_index}))},\n"
  )
  math(EXPR file_index "${file_index} + 1")
endforeach()
file(APPEND "${DRAFT_SKILL_OUTPUT}"
  "};\n\n"
  "} // namespace\n\n"
  "std::span<const EmbeddedDraftSkillFile> embedded_draft_skill_files() {\n"
  "  return kDraftSkillFiles;\n"
  "}\n\n"
  "} // namespace draft\n"
)
