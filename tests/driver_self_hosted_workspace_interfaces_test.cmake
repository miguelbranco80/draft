# Exact differential test for the first self-hosted typed package interface.
#
# Both executables emit the established graph, declaration, target-selection,
# and public-name prefix before the canonical scalar/structural/nominal type
# graph, including exact integer casts/comparisons, natural-layout struct
# fields/offsets, enum values, variant discriminator/payload packets, and
# zero-offset union overlays.
# Imported rows are compared through structural/nominal consumer-local type
# shapes so unrelated production-only private interning cannot affect the
# result. Scratch is process-unique below the caller-provided binary directory.

foreach(required ORACLE DRAFTC_NEXT SOURCE_ROOT TEST_ROOT HOST_TARGET)
  if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
    message(FATAL_ERROR "missing required -D${required}=...")
  endif()
endforeach()
if(NOT EXISTS "${ORACLE}")
  message(FATAL_ERROR "typed-interface oracle does not exist: ${ORACLE}")
endif()
if(NOT EXISTS "${DRAFTC_NEXT}")
  message(FATAL_ERROR "Draft-written draftc-next does not exist: ${DRAFTC_NEXT}")
endif()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_identity)
set(run_root "${TEST_ROOT}/${run_identity}")
file(MAKE_DIRECTORY "${run_root}")

function(compare_interfaces label root workspace core target)
  set(common_arguments
    workspace-interfaces "${root}"
    --workspace "${workspace}"
    --core "${core}"
    --core-identity draft-core-fixture
    --target "${target}"
  )
  execute_process(
    COMMAND "${ORACLE}" ${common_arguments}
    WORKING_DIRECTORY "${SOURCE_ROOT}"
    RESULT_VARIABLE oracle_result
    OUTPUT_FILE "${run_root}/oracle.stdout"
    ERROR_FILE "${run_root}/oracle.stderr"
  )
  execute_process(
    COMMAND "${DRAFTC_NEXT}" ${common_arguments}
    WORKING_DIRECTORY "${SOURCE_ROOT}"
    RESULT_VARIABLE next_result
    OUTPUT_FILE "${run_root}/next.stdout"
    ERROR_FILE "${run_root}/next.stderr"
  )
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
      "${run_root}/oracle.stdout" "${run_root}/next.stdout"
    RESULT_VARIABLE stdout_result
  )
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
      "${run_root}/oracle.stderr" "${run_root}/next.stderr"
    RESULT_VARIABLE stderr_result
  )
  if(NOT oracle_result EQUAL 0 OR NOT next_result EQUAL 0 OR
     NOT stdout_result EQUAL 0 OR NOT stderr_result EQUAL 0)
    file(READ "${run_root}/oracle.stdout" oracle_stdout)
    file(READ "${run_root}/next.stdout" next_stdout)
    file(READ "${run_root}/oracle.stderr" oracle_stderr)
    file(READ "${run_root}/next.stderr" next_stderr)
    message(FATAL_ERROR
      "workspace-interfaces implementations differ for ${label}\n"
      "oracle result: ${oracle_result}\n"
      "next result: ${next_result}\n"
      "--- oracle stdout ---\n${oracle_stdout}"
      "--- next stdout ---\n${next_stdout}"
      "--- oracle stderr ---\n${oracle_stderr}"
      "--- next stderr ---\n${next_stderr}"
    )
  endif()
endfunction()

function(expect_next_interface_failure label source expected_message)
  set(failure_root "${run_root}/${label}")
  file(MAKE_DIRECTORY
    "${failure_root}/workspace/app"
    "${failure_root}/core"
  )
  file(WRITE "${failure_root}/workspace/app/package.draft" "${source}")
  execute_process(
    COMMAND "${DRAFTC_NEXT}"
      workspace-interfaces "${failure_root}/workspace/app"
      --workspace "${failure_root}/workspace"
      --core "${failure_root}/core"
      --core-identity draft-core-fixture
      --target "${HOST_TARGET}"
    WORKING_DIRECTORY "${SOURCE_ROOT}"
    RESULT_VARIABLE next_result
    OUTPUT_VARIABLE next_stdout
    ERROR_VARIABLE next_stderr
  )
  if(next_result EQUAL 0 OR NOT next_stderr MATCHES "${expected_message}")
    message(FATAL_ERROR
      "${label} did not fail at the typed-interface boundary (${next_result})\n"
      "stdout:\n${next_stdout}stderr:\n${next_stderr}")
  endif()
endfunction()

# expect_staged_interface_failure distinguishes a deliberate self-hosting seam
# from invalid Draft source. The production oracle must accept the source and
# build its complete interface, while draftc-next must stop at the named typed-
# interface boundary. This avoids treating a parser or shared semantic failure
# as evidence that a valid production feature is merely waiting to migrate.
function(expect_staged_interface_failure label source expected_message)
  set(failure_root "${run_root}/${label}")
  file(MAKE_DIRECTORY
    "${failure_root}/workspace/app"
    "${failure_root}/core"
  )
  file(WRITE "${failure_root}/workspace/app/package.draft" "${source}")
  set(common_arguments
    workspace-interfaces "${failure_root}/workspace/app"
    --workspace "${failure_root}/workspace"
    --core "${failure_root}/core"
    --core-identity draft-core-fixture
    --target "${HOST_TARGET}"
  )
  execute_process(
    COMMAND "${ORACLE}" ${common_arguments}
    WORKING_DIRECTORY "${SOURCE_ROOT}"
    RESULT_VARIABLE oracle_result
    OUTPUT_VARIABLE oracle_stdout
    ERROR_VARIABLE oracle_stderr
  )
  execute_process(
    COMMAND "${DRAFTC_NEXT}" ${common_arguments}
    WORKING_DIRECTORY "${SOURCE_ROOT}"
    RESULT_VARIABLE next_result
    OUTPUT_VARIABLE next_stdout
    ERROR_VARIABLE next_stderr
  )
  if(NOT oracle_result EQUAL 0 OR next_result EQUAL 0 OR
     NOT next_stderr MATCHES "${expected_message}")
    message(FATAL_ERROR
      "${label} did not preserve the production/staging boundary\n"
      "oracle result: ${oracle_result}\n"
      "next result: ${next_result}\n"
      "--- oracle stdout ---\n${oracle_stdout}"
      "--- next stdout ---\n${next_stdout}"
      "--- oracle stderr ---\n${oracle_stderr}"
      "--- next stderr ---\n${next_stderr}"
    )
  endif()
endfunction()

set(interface_root "${run_root}/supported")
set(interface_workspace "${interface_root}/workspace")
set(interface_core "${interface_root}/core")
file(MAKE_DIRECTORY
  "${interface_workspace}/app"
  "${interface_workspace}/middle"
  "${interface_workspace}/values"
  "${interface_core}"
)
file(WRITE "${interface_workspace}/values/package.draft"
  "package values\n"
  "pub Count :: 4\n"
  "pub Arithmetic_Count :: (3 + 5) * 2 - 4 / 2\n"
  "pub Remainder_Count :: 19 % 5\n"
  "pub Forward_Arithmetic :: Later_Arithmetic * 3 + 1\n"
  "Later_Arithmetic :: 7\n"
  "pub Page_Half :: target.page_size / 2\n"
  "pub Wrapped_Page :: target.page_size - (target.page_size + 1)\n"
  "pub Arbitrary_Recovered :: (340282366920938463463374607431768211455 + 1) - 340282366920938463463374607431768211455\n"
  "pub Arbitrary_Divided :: ((340282366920938463463374607431768211455 + 1) * 3) / (340282366920938463463374607431768211455 + 1)\n"
  "pub Bitwise_Count :: (((10 | 5) & 14) ~ 2)\n"
  "pub Shifted_Count :: (1 << 6) >> 4\n"
  "pub Complement_Recovered :: ~(-5)\n"
  "pub Wide_Shift_Recovered :: (((340282366920938463463374607431768211455 + 1) << 65) >> 65) - 340282366920938463463374607431768211455\n"
  "pub Target_Shifted :: target.page_size >> 12\n"
  "pub Target_Complement :: ~target.page_size\n"
  "pub Target_Shift_Wrapped :: target.page_size << 60\n"
  "pub Concrete_Count_Shift :: (1 << target.pointer_bits) >> target.pointer_bits\n"
  "pub Index :: u32\n"
  "pub Cast_Byte :: cast[u8](257)\n"
  "pub Cast_Negative :: cast[u8](-1)\n"
  "pub Cast_Index :: cast[Index](4294967297)\n"
  "pub Cast_Page :: cast[usize](target.page_size)\n"
  "pub Cast_Wide :: cast[u64]((1 << 100) + 9)\n"
  "pub Cast_Arithmetic :: cast[u8](255) + 2\n"
  "pub Forward_Cast :: cast[Later_Cast_Index](65537)\n"
  "Later_Cast_Index :: u16\n"
  "pub Equal_Wide :: ((1 << 200) | 7) == ((1 << 200) + 7)\n"
  "pub Unequal_Wide :: (1 << 200) != (1 << 199)\n"
  "pub Less_Negative :: -(1 << 200) < -1\n"
  "pub Less_Equal_Wide :: (1 << 200) <= (1 << 200)\n"
  "pub Greater_Wide :: (1 << 200) > ((1 << 199) + 1)\n"
  "pub Greater_Equal_Cast :: cast[u8](-1) >= 255\n"
  "pub Target_Comparison :: target.page_size >= 4096\n"
  "pub Forward_Comparison :: Later_Comparison_Value > 7\n"
  "Later_Comparison_Value :: 8\n"
  "pub Boolean_Comparison :: true != false\n"
  "pub Type_Comparison :: u8 != u16\n"
  "pub Duration :: distinct i64\n"
  "pub Epoch :: distinct i64\n"
  "pub Pair :: struct { left: u8; right: u32; tail: u16; }\n"
  "pub Grouped :: struct { first, second: u16; marker: u8; }\n"
  "pub Node :: struct { next: ^Node; payload: Index; }\n"
  "Private_Count :: distinct u32\n"
  "pub Private_Count_View :: Private_Count\n"
  "Private_Record :: struct { count: Private_Count; pair: Pair; }\n"
  "pub Private_Record_View :: Private_Record\n"
  "pub Handle :: distinct ^Index\n"
  "pub Pair_Kind :: distinct Pair\n"
  "pub Limit :: 42\n"
  "pub Enabled :: true\n"
  "pub Label :: \"ready\"\n"
  "pub Mode :: enum { idle; ready = 4; later; }\n"
  "pub Signed_Code :: enum i16 { zero; minimum = -32768; minus_one = -1; maximum = 32767; }\n"
  "pub Wide_Code :: enum { zero; maximum = 340282366920938463463374607431768211455; }\n"
  "pub Forward_Code :: enum u16 { zero; selected = Later_Code; }\n"
  "Later_Code :: 257\n"
  "pub Arithmetic_Code :: enum i16 { zero; negative = -(2 + 3) * 2; quotient = 25 / 3; remainder = 17 % 5; negative_quotient = -17 / 5; negative_remainder = -17 % 5; combined = 7 * 6 - (5 + 4); }\n"
  "pub Product_Code :: enum { zero; square = 18446744073709551615 * 18446744073709551615; }\n"
  "pub Arbitrary_Code :: enum u128 { zero; recovered = (340282366920938463463374607431768211455 + 1) - 1; quotient = ((340282366920938463463374607431768211455 + 1) * 37) / (340282366920938463463374607431768211455 + 1); remainder = (((340282366920938463463374607431768211455 + 1) * 37) + 19) % (340282366920938463463374607431768211455 + 1); negative_recovered = -(340282366920938463463374607431768211455 + 1) + (340282366920938463463374607431768211455 + 2); }\n"
  "pub Bitwise_Code :: enum i16 { zero; complement = ~0; xor_value = -8 ~ 3; and_value = -1 & 255; or_value = 8 | 3; arithmetic_shift = -9 >> 2; left_shift = 3 << 4; wide_recovered = ((1 << 200) | 37) & 63; }\n"
  "pub Cast_Code :: enum u8 { zero; wrapped = cast[u8](257); maximum = cast[u8](-1); }\n"
  "Private_Status :: enum u8 { none; active = 9; }\n"
  "pub Private_Status_View :: Private_Status\n"
  "pub Choice :: variant { none; count: u32; pair: Pair; }\n"
  "pub Explicit_Choice :: variant u16 { empty; word: u64; }\n"
  "pub Variant_Tag_Base :: distinct i16\n"
  "pub Variant_Tag :: distinct Variant_Tag_Base\n"
  "pub Distinct_Choice :: variant Variant_Tag { none; word: u64; }\n"
  "pub Recursive_Choice :: variant { end; next: ^Recursive_Choice; }\n"
  "Private_Choice :: variant { none; record: Private_Record; }\n"
  "pub Private_Choice_View :: Private_Choice\n"
  "pub Overlay :: union { bytes: [13]u8; pair: Pair; }\n"
  "pub Grouped_Overlay :: union { low, high: u32; record: Pair; }\n"
  "pub Recursive_Overlay :: union { next: ^Recursive_Overlay; word: u64; }\n"
  "Private_Overlay :: union { count: Private_Count; record: Private_Record; }\n"
  "pub Private_Overlay_View :: Private_Overlay\n"
  "when target.arch == .aarch64 {\n"
  "  pub Target_Overlay :: union { word: u64; }\n"
  "} else {\n"
  "  pub Target_Overlay :: union { record: Pair; }\n"
  "}\n"
  "pub Forward :: Later\n"
  "Later :: u16\n"
  "pub Forward_View :: Later_View\n"
  "Later_View :: []Index\n"
  "pub Index_Pointer :: ^Index\n"
  "pub Index_Multi :: [^]Index\n"
  "pub Index_Slice :: []Index\n"
  "pub Index_Array :: [Count]Index\n"
  "pub Arithmetic_Array :: [Arithmetic_Count]Index\n"
  "pub Remainder_Array :: [Remainder_Count]u8\n"
  "pub Page_Half_Array :: [target.page_size / 2]u8\n"
  "pub Arbitrary_Array :: [((340282366920938463463374607431768211455 + 1) * 3) / (340282366920938463463374607431768211455 + 1)]u8\n"
  "pub Bitwise_Array :: [2 | 1]u8\n"
  "pub Shift_Array :: [(1 << 6) >> 4]u8\n"
  "pub Concrete_Count_Array :: [(1 << target.pointer_bits) >> target.pointer_bits]u8\n"
  "pub Cast_Count_Array :: [cast[usize]((1 << 80) + 3)]u8\n"
  "pub Duration_Array :: [Count]Duration\n"
  "pub Pair_Array :: [Count]Pair\n"
  "pub Pair_Pointer :: ^Pair\n"
  "pub Forward_Array :: [Later_Count]Index\n"
  "Later_Count :: Count\n"
  "pub Index_Tuple :: (Index_Pointer, Index_Array)\n"
  "pub Transformer :: proc(value: Index_Slice) -> Index_Pointer\n"
  "pub Page_Array :: [target.page_size]u8\n"
  "pub Storage: Index_Array\n"
  "pub Current: Duration\n"
  "pub Current_Pair: Pair\n"
  "pub Current_Mode: Mode\n"
  "pub Mode_Array :: [Count]Mode\n"
  "pub Choice_Array :: [Count]Choice\n"
  "pub Current_Choice: Choice\n"
  "pub Overlay_Array :: [Count]Overlay\n"
  "pub Current_Overlay: Overlay\n"
  "pub transform :: proc(values: Index_Slice, cursor: Index_Multi) -> Index_Pointer { return nil; }\n"
  "pub keep_duration :: proc(value: Duration) -> Duration { return value; }\n"
  "pub pair :: proc(pointer: Index_Pointer, values: Index_Array) -> (Index_Pointer, Index_Array) { return (pointer, values); }\n"
  "pub visit :: proc(node: ^Node, pair: Pair) -> ^Node { return node; }\n"
)
file(WRITE "${interface_workspace}/middle/package.draft"
  "package middle\n"
  "import values\n"
  "pub Duration :: values.Duration\n"
  "pub Private_Count :: values.Private_Count_View\n"
  "pub Pair :: values.Pair\n"
  "pub Private_Record :: values.Private_Record_View\n"
  "pub Mode :: values.Mode\n"
  "pub Scaled_Count :: values.Arithmetic_Count * 2\n"
  "pub Arithmetic_Code :: values.Arithmetic_Code\n"
  "pub Product_Code :: values.Product_Code\n"
  "pub Arbitrary_Code :: values.Arbitrary_Code\n"
  "pub Bitwise_Code :: values.Bitwise_Code\n"
  "pub Arbitrary_Count :: values.Arbitrary_Recovered + values.Arbitrary_Divided\n"
  "pub Bitwise_Count :: (values.Bitwise_Count << 1) >> 1\n"
  "pub Target_Bits :: values.Target_Shifted | 1\n"
  "pub Imported_Cast :: cast[values.Index](4294967297)\n"
  "pub Imported_Comparison :: values.Cast_Wide == 9\n"
  "pub Reexported_Comparison :: values.Greater_Wide\n"
  "pub Imported_Arithmetic_Array :: [values.Remainder_Count + 1]u8\n"
  "pub Private_Status :: values.Private_Status_View\n"
  "pub Middle_Code :: enum u16 { none; count = values.Count; }\n"
  "pub Choice :: values.Choice\n"
  "pub Distinct_Choice :: values.Distinct_Choice\n"
  "pub Private_Choice :: values.Private_Choice_View\n"
  "pub Middle_Choice :: variant u16 { empty; wrapper: Wrapper; direct: values.Choice; }\n"
  "pub Overlay :: values.Overlay\n"
  "pub Private_Overlay :: values.Private_Overlay_View\n"
  "pub Middle_Overlay :: union { wrapper: Wrapper; direct: values.Overlay; }\n"
  "pub Wrapped :: distinct values.Duration\n"
  "pub Wrapped_Array :: [2]Wrapped\n"
  "pub Wrapper :: struct { pair: values.Pair; duration: values.Duration; next: ^Wrapper; }\n"
)
file(WRITE "${interface_workspace}/app/package.draft"
  "package app\n"
  "import values\n"
  "import middle\n"
  "pub Local_Index :: values.Index\n"
  "pub Local_Limit :: values.Limit\n"
  "pub Imported_Arithmetic_Count :: values.Arithmetic_Count\n"
  "pub Transitive_Scaled_Count :: middle.Scaled_Count\n"
  "pub Mixed_Count :: values.Arithmetic_Count + middle.Scaled_Count\n"
  "pub Imported_Arithmetic_Code :: middle.Arithmetic_Code\n"
  "pub Imported_Product_Code :: middle.Product_Code\n"
  "pub Imported_Arbitrary_Code :: middle.Arbitrary_Code\n"
  "pub Imported_Bitwise_Code :: middle.Bitwise_Code\n"
  "pub Imported_Bitwise_Count :: middle.Bitwise_Count\n"
  "pub Imported_Cast :: middle.Imported_Cast\n"
  "pub Imported_Comparison :: middle.Imported_Comparison\n"
  "pub Transitive_Comparison :: middle.Reexported_Comparison\n"
  "pub Local_Enabled :: values.Enabled\n"
  "pub Local_Label :: values.Label\n"
  "pub Local_Pointer :: values.Index_Pointer\n"
  "pub Local_Multi :: values.Index_Multi\n"
  "pub Local_Slice :: values.Index_Slice\n"
  "pub Local_Array :: values.Index_Array\n"
  "pub Local_Tuple :: values.Index_Tuple\n"
  "pub Local_Transformer :: values.Transformer\n"
  "pub Direct_Duration :: values.Duration\n"
  "pub Via_Middle_Duration :: middle.Duration\n"
  "pub Direct_Pair :: values.Pair\n"
  "pub Via_Middle_Pair :: middle.Pair\n"
  "pub Direct_Mode :: values.Mode\n"
  "pub Via_Middle_Mode :: middle.Mode\n"
  "pub Reexported_Private_Status :: middle.Private_Status\n"
  "pub Imported_Middle_Code :: middle.Middle_Code\n"
  "pub Direct_Choice :: values.Choice\n"
  "pub Via_Middle_Choice :: middle.Choice\n"
  "pub Via_Middle_Distinct_Choice :: middle.Distinct_Choice\n"
  "pub Reexported_Private_Choice :: middle.Private_Choice\n"
  "pub Imported_Middle_Choice :: middle.Middle_Choice\n"
  "pub Direct_Overlay :: values.Overlay\n"
  "pub Via_Middle_Overlay :: middle.Overlay\n"
  "pub Reexported_Private_Overlay :: middle.Private_Overlay\n"
  "pub Imported_Middle_Overlay :: middle.Middle_Overlay\n"
  "pub Reexported_Private_Record :: middle.Private_Record\n"
  "pub Middle_Wrapper :: middle.Wrapper\n"
  "pub Reexported_Private_Count :: middle.Private_Count\n"
  "pub Middle_Wrapped :: middle.Wrapped\n"
  "pub Local_Duration :: distinct i64\n"
  "pub Nested_Distinct :: distinct values.Duration\n"
  "pub Local_Duration_Array :: [2]Local_Duration\n"
  "pub Imported_Duration_Array :: [2]middle.Duration\n"
  "pub Nested_Array :: [2]values.Index_Array\n"
  "pub Imported_Count_Array :: [values.Count]values.Index\n"
  "pub Mixed_Count_Array :: [Mixed_Count]u8\n"
  "pub Imported_Expression_Array :: [(values.Remainder_Count + 1) * 2]u16\n"
  "pub Imported_Arbitrary_Array :: [middle.Arbitrary_Count]u8\n"
  "pub Imported_Bitwise_Array :: [(middle.Bitwise_Count >> 1) | 2]u8\n"
  "pub Direct_Tuple :: ([]values.Index, ^values.Index_Tuple)\n"
  "pub Pair_Grid :: [2]values.Pair_Array\n"
  "pub Mode_Grid :: [2]values.Mode_Array\n"
  "pub Local_Mode :: enum u8 { none; imported_count = values.Count; }\n"
  "pub Local_Choice :: variant { none; pair: values.Pair; wrapper: middle.Wrapper; }\n"
  "pub Local_Overlay :: union { pair: values.Pair; wrapper: middle.Wrapper; }\n"
  "pub Envelope :: struct { pair: values.Pair; wrapper: middle.Wrapper; next: ^Envelope; grid: [2]values.Pair; }\n"
  "pub Enum_Record :: struct { direct: values.Mode; transitive: middle.Mode; local: Local_Mode; }\n"
  "pub Variant_Record :: struct { direct: values.Choice; transitive: middle.Choice; local: Local_Choice; }\n"
  "pub Union_Record :: struct { direct: values.Overlay; transitive: middle.Overlay; local: Local_Overlay; }\n"
  "pub Choice_Grid :: [2]values.Choice_Array\n"
  "pub Choice_Transformer :: proc(value: values.Choice) -> middle.Middle_Choice\n"
  "pub Overlay_Grid :: [2]values.Overlay_Array\n"
  "pub Overlay_Transformer :: proc(value: values.Overlay) -> middle.Middle_Overlay\n"
  "pub Storage: values.Index_Array\n"
  "pub Cursor: values.Index_Multi\n"
  "pub copy :: proc(value: []values.Index, cursor: [^]values.Index) -> ^values.Index { return nil; }\n"
  "main :: proc() {}\n"
)

foreach(target
    aarch64-macos
    aarch64-linux
    x86_64-linux
    x86_64-windows)
  compare_interfaces(
    "scalar-structural-nominal-${target}"
    "${interface_workspace}/app"
    "${interface_workspace}"
    "${interface_core}"
    "${target}"
  )
endforeach()

# C ABI enums remain outside this ordinary enum slice. Invalid arithmetic must
# fail explicitly rather than publishing a partial value table.
expect_next_interface_failure(
  unsupported-c-enum
  "package app\npub State :: c enum { ready; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  invalid-enum-value-division-by-zero
  "package app\npub State :: enum { none; ready = 2 / 0; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)

# The staged enum completion still enforces the semantic zero-value,
# uniqueness, and explicit-backing range invariants within its closed syntax.
expect_next_interface_failure(
  invalid-enum-missing-zero
  "package app\npub State :: enum { ready = 1; later; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  invalid-enum-duplicate-value
  "package app\npub State :: enum { none; ready = 1; later = 1; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  invalid-enum-backing-range
  "package app\npub State :: enum u8 { none; too_large = 256; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  invalid-enum-final-value-beyond-u128
  "package app\npub State :: enum { none; too_large = 340282366920938463463374607431768211455 + 1; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)

# Ordinary variant completion rejects invalid empty/member/discriminator forms
# inside the supported natural-layout syntax rather than publishing a partial
# tagged-sum packet.
expect_next_interface_failure(
  invalid-empty-variant
  "package app\npub Choice :: variant {}\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  invalid-variant-discriminator
  "package app\npub Choice :: variant bool { none; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  invalid-variant-duplicate-name
  "package app\npub Choice :: variant { same; same: u32; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  invalid-variant-discard-name
  "package app\npub Choice :: variant { _; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
set(overflow_variant_source "package app\npub Choice :: variant u8 {")
foreach(index RANGE 0 256)
  string(APPEND overflow_variant_source " alternative_${index};")
endforeach()
string(APPEND overflow_variant_source " }\nmain :: proc() {}\n")
expect_next_interface_failure(
  invalid-variant-discriminator-capacity
  "${overflow_variant_source}"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)

# Ordinary unions require one or more uniquely named fields and publish only
# the unmodified zero-offset overlay. Specialized aggregate modifiers and
# selected/synthesized member regions remain separate later slices.
expect_next_interface_failure(
  invalid-empty-union
  "package app\npub Overlay :: union {}\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  invalid-union-duplicate-name
  "package app\npub Overlay :: union { same: u8; same: u32; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  invalid-union-discard-name
  "package app\npub Overlay :: union { _: u8; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_staged_interface_failure(
  unsupported-c-union
  "package app\npub Overlay :: c union { word: u64; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_staged_interface_failure(
  unsupported-aligned-union
  "package app\npub Overlay :: align(16) union { word: u64; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_staged_interface_failure(
  unsupported-c-aligned-union
  "package app\npub Overlay :: c align(16) union { word: u64; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_staged_interface_failure(
  unsupported-selected-union-member
  "package app\npub Overlay :: union { when true { word: u64; } }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  unsupported-synthesized-union-member
  "package app\npub Overlay :: union { ... \"generate one field\"; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)

# The first aggregate slice is deliberately only ordinary natural layout.
# Packed fields and C/explicit-alignment aggregates must remain fail-closed
# until their distinct layout contracts move together.
expect_next_interface_failure(
  unsupported-packed-struct
  "package app\npub Record :: struct { packed value: u32; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  unsupported-bit-field-struct
  "package app\npub Record :: struct { bits(3) value: u8; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  unsupported-c-struct
  "package app\npub Record :: c struct { value: u32; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  unsupported-aligned-struct
  "package app\npub Record :: align(16) struct { value: u32; }\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)

# SIMD remains outside the closed type vocabulary even though its element and
# lane count would otherwise be supported.
expect_next_interface_failure(
  unsupported-simd
  "package app\npub Lanes :: simd[4]u32\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)

# Array counts reuse the shared exact integer evaluator. Trapping shifts and
# concrete type mismatches remain invalid even though bitwise, shift, and
# supported unsigned cast operators are now inside its closed vocabulary.
expect_next_interface_failure(
  invalid-array-count-division-by-zero
  "package app\npub Bytes :: [4 / 0]u8\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  invalid-array-count-negative-shift
  "package app\npub Bytes :: [4 << -1]u8\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  invalid-typed-shift-count
  "package app\npub Too_Far :: target.page_size << 64\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  invalid-shift-resource-limit
  "package app\npub Bytes :: [1 << 1000001]u8\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  invalid-bitwise-concrete-type-mismatch
  "package app\npub Mixed :: target.page_size & target.pointer_bits\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_next_interface_failure(
  invalid-comparison-concrete-type-mismatch
  "package app\npub Mixed :: cast[u8](1) == cast[u16](1)\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)

# The explicit-conversion slice currently publishes only unsigned integer
# destinations through 64 bits. Production accepts these additional cast
# families, so they remain deliberate self-hosting boundaries rather than
# being confused with invalid Draft source.
expect_staged_interface_failure(
  unsupported-signed-integer-cast
  "package app\npub Signed :: cast[i64](1)\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_staged_interface_failure(
  unsupported-boolean-cast
  "package app\npub Truth :: cast[bool](1)\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)

# The interface scalar packet currently publishes only nonnegative u64 values,
# although enums and arrays can consume wider or negative exact intermediates.
expect_staged_interface_failure(
  unsupported-negative-named-integer
  "package app\npub Negative :: -1\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)
expect_staged_interface_failure(
  unsupported-wide-named-integer
  "package app\npub Wider :: 18446744073709551616\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)

# Arbitrary-precision evaluation now accepts this expression, but its final
# value still cannot cross the existing nonnegative-u64 scalar interface packet.
# The supported fixture above proves that the same wide intermediate may narrow
# to a publishable scalar, array count, or enum value without wrapping.
expect_staged_interface_failure(
  unsupported-arbitrary-precision-final-scalar
  "package app\npub Too_Wide :: 340282366920938463463374607431768211455 + 1\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)

# A concrete uint is not an implicit usize merely because both current target
# widths are 64 bits. Only an untyped representable constant or exact usize may
# supply the fixed-array count.
expect_next_interface_failure(
  unsupported-array-count-type
  "package app\npub Bytes :: [target.pointer_bits]u8\nmain :: proc() {}\n"
  "self-hosted typed interface requires a supported scalar, structural, distinct, ordinary struct, ordinary enum, ordinary variant, or ordinary union declaration"
)

# Local declaration cycles remain visible graph edges and receive a distinct
# cycle diagnostic instead of recursing through source declarations.
expect_next_interface_failure(
  cyclic-types
  "package app\npub First :: Second\nSecond :: First\nmain :: proc() {}\n"
  "self-hosted typed interface contains a cyclic declaration dependency"
)

# A nominal shell permits pointer recursion but does not make direct by-value
# recursion layoutable. The latter remains an explicit scheduler cycle.
expect_next_interface_failure(
  cyclic-self-struct-layout
  "package app\npub Node :: struct { next: Node; }\nmain :: proc() {}\n"
  "self-hosted typed interface contains a cyclic declaration dependency"
)
expect_next_interface_failure(
  cyclic-self-variant-layout
  "package app\npub Loop :: variant { next: Loop; }\nmain :: proc() {}\n"
  "self-hosted typed interface contains a cyclic declaration dependency"
)
expect_next_interface_failure(
  cyclic-self-union-layout
  "package app\npub Loop :: union { next: Loop; }\nmain :: proc() {}\n"
  "self-hosted typed interface contains a cyclic declaration dependency"
)
expect_next_interface_failure(
  cyclic-struct-layout
  "package app\npub First :: struct { second: Second; }\nSecond :: struct { first: First; }\nmain :: proc() {}\n"
  "self-hosted typed interface contains a cyclic declaration dependency"
)

message(STATUS
  "draftc-next typed interfaces matched production scalar/structural/nominal graphs")
file(REMOVE_RECURSE "${run_root}")
