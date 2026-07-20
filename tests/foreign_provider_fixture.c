// Native object provider for the checked-in foreign-provider example.
//
// The object owns no state and depends only on the C integer ABI. Its exported
// symbol must match the example's explicit `c "draft_triple"` declaration.
// The C compiler, Draft compiler, and platform linker therefore provide an
// independent end-to-end check of logical provider mapping and exact symbol
// resolution on each supported native AArch64 host.

#include <stdint.h>

// Returns the value expected by examples/foreign-provider. Signed overflow is
// irrelevant to the fixture because the sole checked input is 14.
int64_t draft_triple(int64_t value) {
  return value * 3;
}
