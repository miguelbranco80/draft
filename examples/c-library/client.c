// Native smoke-test client for the generated c-library header and dylib.

#include "draft-c-library.h"

static int32_t add_one(int32_t value) {
    return value + 1;
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
    return 0;
}
