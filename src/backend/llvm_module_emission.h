// Native emission from one already-constructed package-owned LLVM unit.
//
// This is an internal seam between Draft's direct package-unit builder and
// the small target-machine adapter. The caller owns the LLVM context and module
// for the complete synchronous call. The adapter verifies and then mutates the
// module through the selected O2 and instrumentation pipelines before copying
// object or assembly bytes into an ordinary result value. No LLVM handle
// escapes the package-unit task.
//
// The textual LLVM adapter deliberately uses the same operation after parsing
// explicit oracle input. Ordinary Draft compilation must call this operation
// with a module constructed through LLVM's API and therefore pays no textual
// serialization or parsing boundary.

#pragma once

#include "backend/llvm_object_emitter.h"

#include <llvm-c/Types.h>

#include <string_view>

namespace draft {

// Verifies, optionally transforms, and emits one mutable LLVM module. module is
// borrowed and must be non-null. The caller retains disposal responsibility but
// must otherwise consider its IR consumed: optimization and instrumentation
// intentionally mutate it. module_name is a logical diagnostic label only.
[[nodiscard]] LlvmObjectEmissionResult emit_constructed_llvm_module_in_process(
    const TargetProfile &target, std::string_view module_name,
    LLVMModuleRef module, LlvmObjectEmissionOptions options);

} // namespace draft
