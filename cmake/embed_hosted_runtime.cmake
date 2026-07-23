# Generates the immutable C++ byte table for Draft's hosted runtime objects.
#
# The four object/assembly pairs are explicit inputs produced by CMake custom
# commands.
# Keeping the order and target identities here fixed makes the compiler bundle
# deterministic and prevents host filesystem enumeration from becoming target
# selection policy.

if(NOT DEFINED DRAFT_RUNTIME_BUNDLE_OUTPUT OR
   NOT DEFINED DRAFT_RUNTIME_AARCH64_MACOS OR
   NOT DEFINED DRAFT_RUNTIME_AARCH64_MACOS_ASSEMBLY OR
   NOT DEFINED DRAFT_RUNTIME_AARCH64_LINUX OR
   NOT DEFINED DRAFT_RUNTIME_AARCH64_LINUX_ASSEMBLY OR
   NOT DEFINED DRAFT_RUNTIME_X86_64_LINUX OR
   NOT DEFINED DRAFT_RUNTIME_X86_64_LINUX_ASSEMBLY OR
   NOT DEFINED DRAFT_RUNTIME_X86_64_WINDOWS OR
   NOT DEFINED DRAFT_RUNTIME_X86_64_WINDOWS_ASSEMBLY)
  message(FATAL_ERROR "all hosted runtime object paths and output are required")
endif()

set(runtime_identities
  "draft-aarch64-macos-v5"
  "draft-aarch64-linux-gnu-v1"
  "draft-x86_64-linux-gnu-v1"
  "draft-x86_64-windows-msvc-v1"
)
set(runtime_paths
  "${DRAFT_RUNTIME_AARCH64_MACOS}"
  "${DRAFT_RUNTIME_AARCH64_MACOS_ASSEMBLY}"
  "${DRAFT_RUNTIME_AARCH64_LINUX}"
  "${DRAFT_RUNTIME_AARCH64_LINUX_ASSEMBLY}"
  "${DRAFT_RUNTIME_X86_64_LINUX}"
  "${DRAFT_RUNTIME_X86_64_LINUX_ASSEMBLY}"
  "${DRAFT_RUNTIME_X86_64_WINDOWS}"
  "${DRAFT_RUNTIME_X86_64_WINDOWS_ASSEMBLY}"
)

get_filename_component(output_directory "${DRAFT_RUNTIME_BUNDLE_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${output_directory}")
file(WRITE "${DRAFT_RUNTIME_BUNDLE_OUTPUT}"
  "// Generated from runtime/hosted_runtime.c; do not edit.\n\n"
  "#include \"backend/hosted_runtime_bundle.h\"\n\n"
  "namespace draft {\n"
  "namespace {\n\n"
)

set(runtime_index 0)
foreach(runtime_path IN LISTS runtime_paths)
  if(NOT EXISTS "${runtime_path}")
    message(FATAL_ERROR "hosted runtime object is missing: ${runtime_path}")
  endif()
  file(READ "${runtime_path}" runtime_hex HEX)
  string(LENGTH "${runtime_hex}" hex_length)
  if(hex_length EQUAL 0)
    message(FATAL_ERROR "hosted runtime object is empty: ${runtime_path}")
  endif()
  file(APPEND "${DRAFT_RUNTIME_BUNDLE_OUTPUT}"
    "const unsigned char kHostedRuntime${runtime_index}[] = {\n  "
  )
  math(EXPR last_byte "(${hex_length} / 2) - 1")
  foreach(byte_index RANGE 0 ${last_byte})
    math(EXPR hex_offset "${byte_index} * 2")
    string(SUBSTRING "${runtime_hex}" ${hex_offset} 2 byte_hex)
    file(APPEND "${DRAFT_RUNTIME_BUNDLE_OUTPUT}" "0x${byte_hex},")
    math(EXPR line_column "(${byte_index} + 1) % 16")
    if(line_column EQUAL 0)
      file(APPEND "${DRAFT_RUNTIME_BUNDLE_OUTPUT}" "\n  ")
    endif()
  endforeach()
  file(APPEND "${DRAFT_RUNTIME_BUNDLE_OUTPUT}" "\n};\n\n")
  math(EXPR runtime_index "${runtime_index} + 1")
endforeach()

file(APPEND "${DRAFT_RUNTIME_BUNDLE_OUTPUT}"
  "const EmbeddedHostedRuntimeObject kHostedRuntimes[] = {\n"
)
set(runtime_index 0)
foreach(runtime_identity IN LISTS runtime_identities)
  math(EXPR assembly_index "${runtime_index} + 1")
  file(APPEND "${DRAFT_RUNTIME_BUNDLE_OUTPUT}"
    "    {\"${runtime_identity}\", std::string_view(\n"
    "         reinterpret_cast<const char *>(kHostedRuntime${runtime_index}),\n"
    "         sizeof(kHostedRuntime${runtime_index})), std::string_view(\n"
    "         reinterpret_cast<const char *>(kHostedRuntime${assembly_index}),\n"
    "         sizeof(kHostedRuntime${assembly_index}))},\n"
  )
  math(EXPR runtime_index "${runtime_index} + 2")
endforeach()
file(APPEND "${DRAFT_RUNTIME_BUNDLE_OUTPUT}"
  "};\n\n"
  "} // namespace\n\n"
  "std::span<const EmbeddedHostedRuntimeObject>\n"
  "embedded_hosted_runtime_objects() {\n"
  "  return kHostedRuntimes;\n"
  "}\n\n"
  "const EmbeddedHostedRuntimeObject *embedded_hosted_runtime_object(\n"
  "    std::string_view target_identity) {\n"
  "  for (const EmbeddedHostedRuntimeObject &runtime : kHostedRuntimes) {\n"
  "    if (runtime.target_identity == target_identity) return &runtime;\n"
  "  }\n"
  "  return nullptr;\n"
  "}\n\n"
  "} // namespace draft\n"
)
