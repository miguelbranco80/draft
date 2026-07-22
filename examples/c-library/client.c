// Native smoke-test client for the generated header and shared library.

#include "draft-c-library.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <pthread.h>
#endif

static int32_t add_one(int32_t value) {
    return value + 1;
}

typedef struct bridge_observation {
    int32_t first;
    int32_t second;
} bridge_observation;

#if defined(_WIN32)
static DWORD WINAPI run_bridge_on_foreign_thread(LPVOID user) {
#else
static void *run_bridge_on_foreign_thread(void *user) {
#endif
    bridge_observation *observation = (bridge_observation *)user;
    observation->first = draft_bridge_from_c(35);
    observation->second = draft_bridge_from_c(35);
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

int main(void) {
    draft_c_library_Pair pair = {19, 23};
    pair = draft_pair_identity(pair);
    if (pair.left != 19 || pair.right != 23) return 1;
    if (draft_choice_identity(DRAFT_C_LIBRARY_CHOICE_RIGHT) !=
        DRAFT_C_LIBRARY_CHOICE_RIGHT) return 2;
    if (draft_unsigned_choice_identity(
            DRAFT_C_LIBRARY_UNSIGNED_CHOICE_LARGEST) !=
        DRAFT_C_LIBRARY_UNSIGNED_CHOICE_LARGEST) {
        return 20;
    }
    if (draft_wide_choice_identity(DRAFT_C_LIBRARY_WIDE_CHOICE_LARGE) !=
        DRAFT_C_LIBRARY_WIDE_CHOICE_LARGE) {
        return 21;
    }
    if (draft_maximum_choice_identity(
            DRAFT_C_LIBRARY_MAXIMUM_CHOICE_MAXIMUM) !=
        DRAFT_C_LIBRARY_MAXIMUM_CHOICE_MAXIMUM) {
        return 22;
    }
    if (draft_minimum_choice_identity(
            DRAFT_C_LIBRARY_MINIMUM_CHOICE_MINIMUM) !=
        DRAFT_C_LIBRARY_MINIMUM_CHOICE_MINIMUM) {
        return 23;
    }
    if (draft_huge_choice_identity(DRAFT_C_LIBRARY_HUGE_CHOICE_HUGE) !=
        DRAFT_C_LIBRARY_HUGE_CHOICE_HUGE) {
        return 24;
    }

    draft_c_library_Number number;
    number.integer = 42;
    number = draft_number_identity(number);
    if (number.integer != 42) return 3;
    if (draft_apply(add_one, 41) != 42) return 4;

    // Three-byte records expose a deliberately awkward ABI boundary. AArch64
    // and SysV use direct integer carriers, while Win64 passes and returns this
    // non-power-of-two record indirectly. This is an easy place for independent
    // front ends to agree on layout but disagree on call lowering.
    draft_c_library_Odd_Bytes odd = {{17, 33, 65}};
    odd = draft_odd_bytes_identity(odd);
    if (odd.bytes[0] != 17 || odd.bytes[1] != 33 || odd.bytes[2] != 65) return 5;

    // This 16-byte, 16-aligned record exercises the target's aligned small-
    // aggregate container rule.
    draft_c_library_Aligned_Word aligned = {73};
    aligned = draft_aligned_word_identity(aligned);
    if (aligned.word != 73) return 6;

    // Homogeneous float aggregates travel through SIMD/FP registers rather
    // than the integer containers used by ordinary small structs.
    draft_c_library_Float_Pair floats = {1.25f, -2.5f};
    floats = draft_float_pair_identity(floats);
    if (floats.left != 1.25f || floats.right != -2.5f) return 7;

    // `_Float16` has a native representation on every current target. This
    // six-byte aggregate uses three AArch64 FP lanes, one SysV SSE eightbyte,
    // or an indirect Win64 copy; the Clang caller and Draft callee must select
    // the same target rule.
    draft_c_library_Half_Triple halves = {{(_Float16)0.5, (_Float16)-1.5,
                                            (_Float16)3.25}};
    halves = draft_half_triple_identity(halves);
    if (halves.values[0] != (_Float16)0.5 ||
        halves.values[1] != (_Float16)-1.5 ||
        halves.values[2] != (_Float16)3.25) {
        return 17;
    }

    // The largest union member controls aggregate classification. Nesting that
    // union beside another pair must preserve all four values through AArch64
    // FP lanes, SysV SSE eightbytes, or Win64's indirect aggregate path.
    draft_c_library_Float_Overlay overlay;
    overlay.pair[0] = 4.5f;
    overlay.pair[1] = -6.25f;
    overlay = draft_float_overlay_identity(overlay);
    if (overlay.pair[0] != 4.5f || overlay.pair[1] != -6.25f) return 18;

    draft_c_library_Nested_Floats nested = {
        {.pair = {7.5f, 8.25f}},
        {-9.5f, 10.75f},
    };
    nested = draft_nested_floats_identity(nested);
    if (nested.overlay.pair[0] != 7.5f ||
        nested.overlay.pair[1] != 8.25f ||
        nested.tail[0] != -9.5f || nested.tail[1] != 10.75f) {
        return 19;
    }

    // Records larger than sixteen bytes use an indirect result and an indirect
    // by-value argument. The copies must remain value copies on both sides.
    draft_c_library_Large_Record large = {{11, 22, 33}};
    large = draft_large_record_identity(large);
    if (large.words[0] != 11 || large.words[1] != 22 || large.words[2] != 33) {
        return 8;
    }

    uint8_t byte = 91;
    uint8_t *byte_pointer = &byte;
    uint8_t **pointer_pointer = &byte_pointer;
    if (draft_pointer_pointer_identity(pointer_pointer) != pointer_pointer) return 9;

    uint8_t row[3] = {5, 8, 13};
    uint8_t (*row_pointer)[3] = &row;
    if (draft_row_pointer_identity(row_pointer) != row_pointer) return 10;

    void *opaque_pointer = &byte;
    void **opaque_pointer_pointer = &opaque_pointer;
    if (draft_opaque_pointer_pointer_identity(opaque_pointer_pointer) !=
        opaque_pointer_pointer) {
        return 11;
    }

    // The pointee contains a Draft-only slice and is therefore intentionally
    // erased to void * in the generated C11 header. Address identity still
    // crosses the ABI exactly like any other data pointer.
    if (draft_opaque_view_identity(&byte) != &byte) return 25;
    draft_c_library_Opaque_View_Box opaque_box = {&byte};
    opaque_box = draft_opaque_view_box_identity(opaque_box);
    if (opaque_box.value != &byte) return 26;

    // The C process thread attaches lazily and retains its own Draft package
    // TLS between bridge calls.
    if (draft_bridge_from_c(35) != 42) return 12;
    if (draft_bridge_from_c(35) != 43) return 13;

    // This native thread is unknown to core/thread. The context-free runtime
    // bridge must attach it on first entry, initialize its package TLS to
    // seven, and retain that independent value until native thread exit.
    bridge_observation observation = {0, 0};
#if defined(_WIN32)
    HANDLE foreign_thread = CreateThread(
        NULL, 0, run_bridge_on_foreign_thread, &observation, 0, NULL);
    if (foreign_thread == NULL) return 14;
    if (WaitForSingleObject(foreign_thread, INFINITE) != WAIT_OBJECT_0) {
        CloseHandle(foreign_thread);
        return 15;
    }
    if (!CloseHandle(foreign_thread)) return 15;
#else
    pthread_t foreign_thread;
    if (pthread_create(
            &foreign_thread,
            NULL,
            run_bridge_on_foreign_thread,
            &observation) != 0) {
        return 14;
    }
    if (pthread_join(foreign_thread, NULL) != 0) return 15;
#endif
    if (observation.first != 42 || observation.second != 43) return 16;
    return 0;
}
