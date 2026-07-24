// Process-wide initialization of the LLVM targets implemented by Draft.
//
// LLVM's target registries are process-global mutable state during setup and
// immutable afterward. Every object, assembly, and ThinLTO path calls this
// operation before lookup. One shared once-flag gives all later compiler and
// LLVM worker threads a happens-before edge and prevents two backend adapters
// from independently registering the same targets.

#pragma once

namespace draft {

void initialize_draft_llvm_targets();

} // namespace draft
