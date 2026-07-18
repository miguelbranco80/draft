// Native smoke-test client for the generated c-library header and dylib.

#include "draft-c-library.h"

#include <pthread.h>

static int32_t add_one(int32_t value) {
    return value + 1;
}

typedef struct bridge_observation {
    int32_t first;
    int32_t second;
} bridge_observation;

static void *run_bridge_on_foreign_thread(void *user) {
    bridge_observation *observation = (bridge_observation *)user;
    observation->first = draft_bridge_from_c(35);
    observation->second = draft_bridge_from_c(35);
    return NULL;
}

int main(void) {
    draft_c_library_Pair pair = {19, 23};
    pair = draft_pair_identity(pair);
    if (pair.left != 19 || pair.right != 23) return 1;
    if (draft_choice_identity(DRAFT_C_LIBRARY_CHOICE_RIGHT) !=
        DRAFT_C_LIBRARY_CHOICE_RIGHT) return 2;

    draft_c_library_Number number;
    number.integer = 42;
    number = draft_number_identity(number);
    if (number.integer != 42) return 3;
    if (draft_apply(add_one, 41) != 42) return 4;

    // Three-byte results use an exact i24 return container while arguments use
    // one full general-purpose register. This is an easy place for independent
    // front ends to agree on layout but disagree on call lowering.
    draft_c_library_Odd_Bytes odd = {{17, 33, 65}};
    odd = draft_odd_bytes_identity(odd);
    if (odd.bytes[0] != 17 || odd.bytes[1] != 33 || odd.bytes[2] != 65) return 5;

    // Darwin passes this 16-byte, 16-aligned record as one 128-bit container.
    draft_c_library_Aligned_Word aligned = {73};
    aligned = draft_aligned_word_identity(aligned);
    if (aligned.word != 73) return 6;

    // Homogeneous float aggregates travel through SIMD/FP registers rather
    // than the integer containers used by ordinary small structs.
    draft_c_library_Float_Pair floats = {1.25f, -2.5f};
    floats = draft_float_pair_identity(floats);
    if (floats.left != 1.25f || floats.right != -2.5f) return 7;

    // `_Float16` has a native Darwin arm64 ABI. This six-byte aggregate must
    // use three FP lanes on both the Clang caller and Draft callee sides.
    draft_c_library_Half_Triple halves = {{(_Float16)0.5, (_Float16)-1.5,
                                            (_Float16)3.25}};
    halves = draft_half_triple_identity(halves);
    if (halves.values[0] != (_Float16)0.5 ||
        halves.values[1] != (_Float16)-1.5 ||
        halves.values[2] != (_Float16)3.25) {
        return 17;
    }

    // The largest union alternative controls its HFA lane count. Nesting that
    // union beside another pair must preserve all four FP-register lanes.
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

    // The C process thread attaches lazily and retains its own Draft package
    // TLS between bridge calls.
    if (draft_bridge_from_c(35) != 42) return 12;
    if (draft_bridge_from_c(35) != 43) return 13;

    // This pthread is unknown to core/thread. The context-free runtime bridge
    // must attach it on first entry, initialize its package TLS to seven, and
    // retain that independent value until pthread exit.
    pthread_t foreign_thread;
    bridge_observation observation = {0, 0};
    if (pthread_create(
            &foreign_thread,
            NULL,
            run_bridge_on_foreign_thread,
            &observation) != 0) {
        return 14;
    }
    if (pthread_join(foreign_thread, NULL) != 0) return 15;
    if (observation.first != 42 || observation.second != 43) return 16;
    return 0;
}
