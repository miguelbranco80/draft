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
    return 0;
}
