// Target-independent, executable middle representation for Draft.
//
// MIR sits below typed HIR and above every native backend. Structured source
// control flow has become explicit basic blocks and terminators; storage is
// represented by locals, addresses, loads, and stores. It deliberately keeps
// Draft TypeId and SymbolId identities instead of encoding an LLVM type system
// or an AArch64 register class. That separation lets the bootstrap compiler and
// the eventual self-hosted compiler agree on language behavior independently
// from one particular code generator.
//
// Tables are append-only and use small integer IDs. Public data rows are
// intentional: lowering, verification, diagnostics, and emission are sequential
// compiler passes over a compact representation, not an object hierarchy.

#pragma once

#include "sema/constant_value.h"
#include "sema/hir.h"
#include "sema/symbol.h"
#include "sema/type.h"
#include "source/source.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace draft {

struct MirValueId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();
  [[nodiscard]] bool is_valid() const;
  bool operator==(const MirValueId &) const = default;
};

struct MirInstructionId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();
  [[nodiscard]] bool is_valid() const;
  bool operator==(const MirInstructionId &) const = default;
};

struct MirBlockId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();
  [[nodiscard]] bool is_valid() const;
  bool operator==(const MirBlockId &) const = default;
};

struct MirLocalId {
  std::uint32_t value = std::numeric_limits<std::uint32_t>::max();
  [[nodiscard]] bool is_valid() const;
  bool operator==(const MirLocalId &) const = default;
};

enum class MirLocalKind {
  Parameter,
  Automatic,
  Temporary,
};

// Parameters and source locals retain their SymbolId. Compiler-created
// temporaries have an invalid symbol and a source range explaining why the
// storage exists (for example, a conditional-expression result or saved loop
// iterable). parameter_index is meaningful only for Parameter rows.
struct MirLocal {
  MirLocalKind kind = MirLocalKind::Automatic;
  SymbolId symbol;
  TypeId type;
  SourceRange range;
  std::uint32_t parameter_index = 0;
};

enum class MirInstructionKind {
  Invalid,
  Constant,
  Zero,
  Context,
  LocalAddress,
  GlobalAddress,
  ProcedureReference,
  Load,
  Store,
  AtomicLoad,
  AtomicStore,
  AtomicExchange,
  AtomicReadModifyWrite,
  AtomicCompareExchange,
  AtomicFence,
  Unary,
  Binary,
  Convert,
  PointerOffset,
  PointerSubtract,
  Call,
  Length,
  // Extracts the data pointer from Draft's immutable string view. Keeping the
  // operation explicit prevents a source-level raw-data escape hatch from
  // becoming an accidental aggregate-layout convention in later backends.
  RawData,
  Assert,
  MemberAddress,
  ExtractMember,
  IndexAddress,
  BoundsCheck,
  SliceBoundsCheck,
  Slice,
  Aggregate,
  Assembly,
  Trap,
};

// Instructions list operands in evaluation/use order. Store uses address then
// value; Call uses callee, optional hidden context, then arguments; BoundsCheck
// uses index then length; Slice uses base, low, then high after omitted bounds
// have been materialized by lowering. offset is a byte offset for member access
// and element_size for indexed access and pointer arithmetic. A result-less
// instruction has an invalid result and normally the canonical void TypeId.
struct MirInstruction {
  MirInstructionKind kind = MirInstructionKind::Invalid;
  SourceRange range;
  TypeId type;
  // Address-producing instructions may need a compiler-internal address even
  // when source checking never constructed `^T`. In that case type is the
  // canonical rawptr builtin and addressed_type records T directly. A real
  // source pointer keeps its ordinary Pointer TypeId here as `type` and records
  // the same element in addressed_type. Other instructions leave this invalid.
  // This MIR-local fact prevents lowering from appending synthetic pointer rows
  // to the immutable semantic TypeStore.
  TypeId addressed_type;
  MirValueId result;
  HirOperation operation = HirOperation::None;
  // Atomic instructions carry orders as semantic enum values. They never use a
  // runtime operand for an order, which makes invalid target spellings
  // impossible after MIR verification.
  AtomicMemoryOrder atomic_order = AtomicMemoryOrder::SequentiallyConsistent;
  AtomicMemoryOrder atomic_failure_order =
      AtomicMemoryOrder::SequentiallyConsistent;
  std::vector<MirValueId> operands;
  MirLocalId local;
  SymbolId symbol;
  ConstantValue constant;
  std::uint64_t offset = 0;
  // Aggregate construction may place each source operand at a different byte
  // offset. Other instructions leave this vector empty and use offset alone.
  std::vector<std::uint64_t> offsets;
  std::string assembly_text;
  std::string assembly_constraints;
  bool passes_context = false;
  // The explicit runtime.call_with_context bridge initializes Draft TLS before
  // entering its ordinary callback. Normal ordinary calls already run on an
  // attached Draft thread and leave this false.
  bool establishes_thread_context = false;
};

struct MirValue {
  TypeId type;
  MirInstructionId definition;
};

struct MirSwitchArm {
  MirValueId label;
  MirBlockId target;
};

enum class MirTerminatorKind {
  Invalid,
  Return,
  Branch,
  ConditionalBranch,
  Switch,
  Unreachable,
};

// Return optionally carries one value. Branch has one target, conditional
// branch has true then false targets, and switch stores its default in targets[0]
// plus one row per source label in switch_arms. Cases never fall through.
struct MirTerminator {
  MirTerminatorKind kind = MirTerminatorKind::Invalid;
  SourceRange range;
  MirValueId value;
  std::vector<MirBlockId> targets;
  std::vector<MirSwitchArm> switch_arms;
};

struct MirBlock {
  SourceRange range;
  std::vector<MirInstructionId> instructions;
  MirTerminator terminator;
};

struct MirProcedure {
  SymbolId symbol;
  TypeId type;
  SourceRange range;
  MirBlockId entry;
  std::vector<MirLocal> locals;
  std::vector<MirValue> values;
  std::vector<MirInstruction> instructions;
  std::vector<MirBlock> blocks;
  bool valid = false;

  [[nodiscard]] MirLocalId add_local(MirLocal local);
  [[nodiscard]] MirBlockId add_block(SourceRange block_range);
  [[nodiscard]] MirInstructionId add_instruction(
      MirBlockId block, MirInstruction instruction, bool produces_value);
  void set_terminator(MirBlockId block, MirTerminator terminator);

  [[nodiscard]] const MirLocal &local(MirLocalId id) const;
  [[nodiscard]] const MirValue &value(MirValueId id) const;
  [[nodiscard]] const MirInstruction &instruction(MirInstructionId id) const;
  [[nodiscard]] const MirBlock &block(MirBlockId id) const;
};

[[nodiscard]] const char *mir_instruction_kind_name(MirInstructionKind kind);
[[nodiscard]] const char *mir_terminator_kind_name(MirTerminatorKind kind);

} // namespace draft
