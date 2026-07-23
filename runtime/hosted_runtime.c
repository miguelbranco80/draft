// Compiler-distributed hosted runtime for every Draft native artifact.
//
// This file owns the process-wide services which do not depend on one Draft
// program: the root Context, allocator providers, temporary-allocation epochs,
// assertion/bounds traps, process argument/environment views, and the bridges
// exposed by core/runtime and core/os. It is compiled once per supported target
// while building draftc, then embedded as an ordinary relocatable object in the
// compiler binary. A fresh Draft build therefore links these already-generated
// bytes instead of rebuilding and optimizing the same runtime LLVM module.
//
// The source intentionally includes no platform headers. Every selected target
// ABI is fixed by docs/targets and the small declarations below repeat only the
// exact libc/Kernel32 facts used here. This lets one LLVM Clang installation
// cross-compile all four objects without a target SDK; the final platform link
// still supplies the selected libc and operating-system providers. The C ABI is
// used only as a stable bootstrap implementation tool. Draft-facing symbols
// carry explicit linker names and their layouts are checked below.

#if !defined(DRAFT_RUNTIME_POSIX) && !defined(DRAFT_RUNTIME_WINDOWS)
#error "select exactly one hosted runtime platform"
#endif
#if defined(DRAFT_RUNTIME_POSIX) && defined(DRAFT_RUNTIME_WINDOWS)
#error "select exactly one hosted runtime platform"
#endif

typedef __SIZE_TYPE__ usize;
typedef __UINT64_TYPE__ u64;
typedef __INT64_TYPE__ i64;
typedef __UINT32_TYPE__ u32;
typedef __INT32_TYPE__ i32;
typedef unsigned char u8;

#if defined(DRAFT_RUNTIME_DARWIN)
// A C identifier normally receives Mach-O's leading underscore, but an asm
// label is already an exact object symbol. Add that platform prefix explicitly
// so LLVM IR `@__draft.*` references and this C object's definitions agree.
#define DRAFT_SYMBOL(name) __asm__("_" name)
#else
#define DRAFT_SYMBOL(name) __asm__(name)
#endif
#if defined(DRAFT_RUNTIME_WINDOWS)
#define DRAFT_HIDDEN
#else
#define DRAFT_HIDDEN __attribute__((visibility("hidden")))
#endif
#define DRAFT_NORETURN __attribute__((noreturn))

extern void *calloc(usize count, usize size);
extern void *malloc(usize size);
extern void *realloc(void *memory, usize size);
extern void free(void *memory);
extern usize strlen(const char *text);

// DraftString is the runtime representation of immutable UTF-8 source and
// process text. It is a borrowed view: runtime diagnostics never retain it and
// process views own only their parallel record arrays.
typedef struct DraftString {
  const u8 *data;
  usize length;
} DraftString;

typedef struct DraftContext DraftContext;

typedef void *(*DraftAllocatorProcedure)(
    DraftContext *context, void *user, u8 operation, void *old_memory,
    usize old_size, usize new_size, usize alignment);
typedef void (*DraftLoggerProcedure)(
    DraftContext *context, void *user, u8 level, DraftString message);
typedef _Bool (*DraftRandomProcedure)(
    DraftContext *context, void *user, void *output, usize count);
typedef void (*DraftAssertionFailureProcedure)(
    DraftContext *context, DraftString condition, DraftString message,
    DraftString file, usize line, usize column);

typedef struct DraftAllocator {
  DraftAllocatorProcedure procedure;
  void *user;
} DraftAllocator;

typedef struct DraftLogger {
  DraftLoggerProcedure procedure;
  void *user;
} DraftLogger;

typedef struct DraftRandomGenerator {
  DraftRandomProcedure procedure;
  void *user;
} DraftRandomGenerator;

// This record must remain byte-for-byte identical to core/runtime.Context.
// Provider state is intentionally represented beside each procedure pointer;
// the final three fields reserve the command, denial, and task-scheduler
// services already present in the public runtime shape.
struct DraftContext {
  DraftAllocator general_allocator;
  DraftAllocator temporary_allocator;
  DraftAssertionFailureProcedure assertion_failure;
  DraftLogger logger;
  DraftRandomGenerator random;
  void *command_executor;
  u64 denied_capabilities;
  void *task_scheduler;
};

_Static_assert(sizeof(void *) == 8, "Draft hosted runtime requires 64-bit pointers");
_Static_assert(sizeof(DraftString) == 16, "Draft string ABI changed");
_Static_assert(_Alignof(DraftString) == 8, "Draft string alignment changed");
_Static_assert(sizeof(DraftAllocator) == 16, "Draft allocator ABI changed");
_Static_assert(_Alignof(DraftAllocator) == 8, "Draft allocator alignment changed");
_Static_assert(sizeof(DraftLogger) == 16, "Draft logger ABI changed");
_Static_assert(_Alignof(DraftLogger) == 8, "Draft logger alignment changed");
_Static_assert(sizeof(DraftRandomGenerator) == 16, "Draft random ABI changed");
_Static_assert(
    _Alignof(DraftRandomGenerator) == 8,
    "Draft random alignment changed");
_Static_assert(sizeof(DraftContext) == 96, "core/runtime.Context ABI changed");
_Static_assert(_Alignof(DraftContext) == 8, "core/runtime.Context alignment changed");
_Static_assert(
    __builtin_offsetof(DraftContext, general_allocator) == 0,
    "core/runtime.Context allocator offset changed");
_Static_assert(
    __builtin_offsetof(DraftContext, temporary_allocator) == 16,
    "core/runtime.Context temporary allocator offset changed");
_Static_assert(
    __builtin_offsetof(DraftContext, assertion_failure) == 32,
    "core/runtime.Context assertion offset changed");
_Static_assert(
    __builtin_offsetof(DraftContext, logger) == 40,
    "core/runtime.Context logger offset changed");
_Static_assert(
    __builtin_offsetof(DraftContext, random) == 56,
    "core/runtime.Context random offset changed");
_Static_assert(
    __builtin_offsetof(DraftContext, command_executor) == 72,
    "core/runtime.Context command offset changed");
_Static_assert(
    __builtin_offsetof(DraftContext, denied_capabilities) == 80,
    "core/runtime.Context denial offset changed");
_Static_assert(
    __builtin_offsetof(DraftContext, task_scheduler) == 88,
    "core/runtime.Context scheduler offset changed");

typedef struct DraftTempNode {
  struct DraftTempNode *next;
  void *memory;
} DraftTempNode;

typedef struct DraftTempState {
  DraftTempNode *head;
} DraftTempState;

static DraftString *draft_process_arguments;
static usize draft_process_argument_count;
static DraftString *draft_process_environment;
static usize draft_process_environment_count;

#if defined(DRAFT_RUNTIME_WINDOWS)

extern i32 _write(i32 descriptor, const void *data, u32 count);
extern i32 _setmode(i32 descriptor, i32 mode);
extern i32 rand_s(u32 *value);
extern u32 FlsAlloc(void (*destructor)(void *));
extern void *FlsGetValue(u32 key);
extern i32 FlsSetValue(u32 key, void *value);
extern i32 FlsFree(u32 key);
extern i32 SwitchToThread(void);
extern i32 WideCharToMultiByte(
    u32 code_page, u32 flags, const unsigned short *wide, i32 wide_count,
    char *narrow, i32 narrow_count, const char *default_character,
    i32 *used_default_character);
extern usize wcslen(const unsigned short *text);

// -1 is uninitialized, -2 is owned by the initializing thread, and -3 records
// permanent FlsAlloc failure. Real Windows FLS indices are nonnegative i32s.
static _Atomic(i32) draft_temp_key = -1;
static char **draft_process_utf8_argv;
static char **draft_process_utf8_envp;

static i64 draft_host_write(i32 descriptor, const void *data, usize count) {
  const usize maximum = 0xffffffffu;
  const u32 bounded = (u32)(count > maximum ? maximum : count);
  return (i64)_write(descriptor, data, bounded);
}

static void draft_initialize_standard_streams(void) {
  // _O_BINARY prevents the UCRT from translating newlines or treating byte 26
  // as end-of-input. Draft's console and file APIs traffic in exact bytes.
  (void)_setmode(0, 0x8000);
  (void)_setmode(1, 0x8000);
  (void)_setmode(2, 0x8000);
}

static void *draft_allocate_zeroed(usize size, usize alignment) {
  if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0)
    return (void *)0;
  if (alignment > ((usize)-1) - 7)
    return (void *)0;
  const usize padding = alignment + 7;
  if (size > ((usize)-1) - padding)
    return (void *)0;
  void *base = malloc(size + padding);
  if (base == (void *)0)
    return (void *)0;
  const usize after_header = (usize)base + 8;
  const usize aligned_address =
      (after_header + alignment - 1) & ~(alignment - 1);
  void *aligned = (void *)aligned_address;
  *((void **)((u8 *)aligned - 8)) = base;
  __builtin_memset(aligned, 0, size);
  return aligned;
}

static void draft_release_allocation(void *memory) {
  if (memory != (void *)0)
    free(*((void **)((u8 *)memory - 8)));
}

static void *draft_resize_allocation(
    void *old_memory, usize old_size, usize new_size, usize alignment) {
  void *replacement = draft_allocate_zeroed(new_size, alignment);
  if (replacement == (void *)0)
    return (void *)0;
  if (old_memory != (void *)0) {
    const usize copied = old_size < new_size ? old_size : new_size;
    if (copied != 0)
      __builtin_memcpy(replacement, old_memory, copied);
  }
  draft_release_allocation(old_memory);
  return replacement;
}

#else

extern i64 write(i32 descriptor, const void *data, usize count);
extern i32 posix_memalign(void **result, usize alignment, usize size);
extern void arc4random_buf(void *output, usize count);

#if defined(DRAFT_RUNTIME_DARWIN)
typedef struct DraftPthreadOnce {
  i64 signature;
  u8 opaque[8];
} DraftPthreadOnce;
typedef u64 DraftPthreadKey;
_Static_assert(sizeof(DraftPthreadOnce) == 16, "Darwin pthread_once_t changed");
_Static_assert(
    _Alignof(DraftPthreadOnce) == 8,
    "Darwin pthread_once_t alignment changed");
_Static_assert(sizeof(DraftPthreadKey) == 8, "Darwin pthread_key_t changed");
static DraftPthreadOnce draft_temp_key_once = {816954554, {0}};
#else
typedef struct DraftPthreadOnce {
  i32 state;
} DraftPthreadOnce;
typedef u32 DraftPthreadKey;
_Static_assert(sizeof(DraftPthreadOnce) == 4, "glibc pthread_once_t changed");
_Static_assert(
    _Alignof(DraftPthreadOnce) == 4,
    "glibc pthread_once_t alignment changed");
_Static_assert(sizeof(DraftPthreadKey) == 4, "glibc pthread_key_t changed");
static DraftPthreadOnce draft_temp_key_once = {0};
#endif

extern i32 pthread_once(DraftPthreadOnce *once, void (*initialize)(void));
extern i32 pthread_key_create(
    DraftPthreadKey *key, void (*destructor)(void *));
extern void *pthread_getspecific(DraftPthreadKey key);
extern i32 pthread_setspecific(DraftPthreadKey key, const void *value);

static DraftPthreadKey draft_temp_key;
static _Bool draft_temp_key_ready;

static i64 draft_host_write(i32 descriptor, const void *data, usize count) {
  return write(descriptor, data, count);
}

static void draft_initialize_standard_streams(void) {}

static void *draft_allocate_zeroed(usize size, usize alignment) {
  if (size == 0)
    return (void *)0;
  if (alignment <= 16)
    return calloc(1, size);
  void *memory = (void *)0;
  if (posix_memalign(&memory, alignment, size) != 0)
    return (void *)0;
  __builtin_memset(memory, 0, size);
  return memory;
}

static void draft_release_allocation(void *memory) { free(memory); }

static void *draft_resize_allocation(
    void *old_memory, usize old_size, usize new_size, usize alignment) {
  if (alignment <= 16) {
    void *resized = realloc(old_memory, new_size);
    if (resized == (void *)0)
      return (void *)0;
    if (new_size > old_size)
      __builtin_memset((u8 *)resized + old_size, 0, new_size - old_size);
    return resized;
  }
  void *replacement = draft_allocate_zeroed(new_size, alignment);
  if (replacement == (void *)0)
    return (void *)0;
  if (old_memory != (void *)0) {
    const usize copied = old_size < new_size ? old_size : new_size;
    if (copied != 0)
      __builtin_memcpy(replacement, old_memory, copied);
  }
  free(old_memory);
  return replacement;
}

#endif

static void draft_default_logger(
    DraftContext *context, void *user, u8 level, DraftString message) {
  (void)context;
  (void)user;
  (void)level;
  (void)draft_host_write(2, message.data, message.length);
  (void)draft_host_write(2, "\n", 1);
}

static _Bool draft_default_random(
    DraftContext *context, void *user, void *output, usize count) {
  (void)context;
  (void)user;
#if defined(DRAFT_RUNTIME_WINDOWS)
  usize offset = 0;
  while (offset < count) {
    u32 word;
    if (rand_s(&word) != 0)
      return 0;
    const usize remaining = count - offset;
    const usize chunk = remaining < sizeof(word) ? remaining : sizeof(word);
    __builtin_memcpy((u8 *)output + offset, &word, chunk);
    offset += chunk;
  }
#else
  arc4random_buf(output, count);
#endif
  return 1;
}

static void *draft_default_allocator(
    DraftContext *context, void *user, u8 operation, void *old_memory,
    usize old_size, usize new_size, usize alignment) {
  (void)context;
  (void)user;
  if (operation == 0)
    return draft_allocate_zeroed(new_size, alignment);
  if (operation == 1) {
    if (new_size != 0)
      return draft_resize_allocation(
          old_memory, old_size, new_size, alignment);
    draft_release_allocation(old_memory);
    return (void *)0;
  }
  if (operation == 2) {
    draft_release_allocation(old_memory);
    return (void *)0;
  }
  return (void *)0;
}

static void draft_reset_temp_state(DraftTempState *state) {
  if (state == (void *)0)
    return;
  DraftTempNode *node = state->head;
  state->head = (void *)0;
  while (node != (void *)0) {
    DraftTempNode *next = node->next;
    draft_release_allocation(node->memory);
    free(node);
    node = next;
  }
}

static void draft_destroy_temp_state(void *opaque_state) {
  DraftTempState *state = (DraftTempState *)opaque_state;
  draft_reset_temp_state(state);
  free(state);
}

#if defined(DRAFT_RUNTIME_WINDOWS)

static i32 draft_ensure_temp_key(void) {
  for (;;) {
    const i32 observed = __c11_atomic_load(&draft_temp_key, __ATOMIC_ACQUIRE);
    if (observed >= 0)
      return observed;
    if (observed == -1) {
      i32 expected = -1;
      if (__c11_atomic_compare_exchange_strong(
              &draft_temp_key, &expected, -2, __ATOMIC_ACQ_REL,
              __ATOMIC_ACQUIRE)) {
        const u32 allocated = FlsAlloc(draft_destroy_temp_state);
        const i32 published = allocated == 0xffffffffu ? -3 : (i32)allocated;
        __c11_atomic_store(&draft_temp_key, published, __ATOMIC_RELEASE);
        return published >= 0 ? published : -1;
      }
      continue;
    }
    if (observed != -2)
      return -1;
    (void)SwitchToThread();
  }
}

static DraftTempState *draft_ensure_temp_state(void) {
  const i32 key = draft_ensure_temp_key();
  if (key < 0)
    return (void *)0;
  DraftTempState *state = (DraftTempState *)FlsGetValue((u32)key);
  if (state != (void *)0)
    return state;
  state = (DraftTempState *)calloc(1, sizeof(DraftTempState));
  if (state == (void *)0)
    return (void *)0;
  if (FlsSetValue((u32)key, state) == 0) {
    free(state);
    return (void *)0;
  }
  return state;
}

static void draft_destroy_current_temp_state(void) {
  const i32 key = draft_ensure_temp_key();
  if (key < 0)
    return;
  DraftTempState *state = (DraftTempState *)FlsGetValue((u32)key);
  if (state == (void *)0 || FlsSetValue((u32)key, (void *)0) == 0)
    return;
  draft_destroy_temp_state(state);
}

static void draft_shutdown_temp_key(void) {
  const i32 key = __c11_atomic_load(&draft_temp_key, __ATOMIC_ACQUIRE);
  if (key >= 0) {
    (void)FlsFree((u32)key);
    __c11_atomic_store(&draft_temp_key, -3, __ATOMIC_RELEASE);
  }
}

#else

static void draft_initialize_temp_key(void) {
  draft_temp_key_ready =
      pthread_key_create(&draft_temp_key, draft_destroy_temp_state) == 0;
}

static DraftTempState *draft_ensure_temp_state(void) {
  if (pthread_once(&draft_temp_key_once, draft_initialize_temp_key) != 0 ||
      !draft_temp_key_ready)
    return (void *)0;
  DraftTempState *state =
      (DraftTempState *)pthread_getspecific(draft_temp_key);
  if (state != (void *)0)
    return state;
  state = (DraftTempState *)calloc(1, sizeof(DraftTempState));
  if (state == (void *)0)
    return (void *)0;
  if (pthread_setspecific(draft_temp_key, state) != 0) {
    free(state);
    return (void *)0;
  }
  return state;
}

static void draft_destroy_current_temp_state(void) {
  if (pthread_once(&draft_temp_key_once, draft_initialize_temp_key) != 0 ||
      !draft_temp_key_ready)
    return;
  DraftTempState *state =
      (DraftTempState *)pthread_getspecific(draft_temp_key);
  if (state == (void *)0 || pthread_setspecific(draft_temp_key, (void *)0) != 0)
    return;
  draft_destroy_temp_state(state);
}

static void draft_shutdown_temp_key(void) {}

#endif

static void *draft_temp_allocate(usize size, usize alignment) {
  if (size == 0)
    return (void *)0;
  DraftTempState *state = draft_ensure_temp_state();
  if (state == (void *)0)
    return (void *)0;
  void *memory = draft_allocate_zeroed(size, alignment);
  if (memory == (void *)0)
    return (void *)0;
  DraftTempNode *node = (DraftTempNode *)calloc(1, sizeof(DraftTempNode));
  if (node == (void *)0) {
    draft_release_allocation(memory);
    return (void *)0;
  }
  node->next = state->head;
  node->memory = memory;
  state->head = node;
  return memory;
}

static void *draft_temp_allocator(
    DraftContext *context, void *user, u8 operation, void *old_memory,
    usize old_size, usize new_size, usize alignment) {
  (void)context;
  (void)user;
  if (operation == 0)
    return draft_temp_allocate(new_size, alignment);
  if (operation == 1 && new_size != 0) {
    void *replacement = draft_temp_allocate(new_size, alignment);
    if (replacement == (void *)0)
      return (void *)0;
    if (old_memory != (void *)0) {
      const usize copied = old_size < new_size ? old_size : new_size;
      if (copied != 0)
        __builtin_memcpy(replacement, old_memory, copied);
    }
    return replacement;
  }
  // Individual releases are no-ops; reset owns the complete epoch list.
  return (void *)0;
}

static void draft_default_assertion_failure(
    DraftContext *context, DraftString condition, DraftString message,
    DraftString file, usize line, usize column) {
  (void)context;
  (void)message;
  (void)file;
  (void)line;
  (void)column;
  (void)draft_host_write(2, "Draft assertion failed: ", 24);
  (void)draft_host_write(2, condition.data, condition.length);
  (void)draft_host_write(2, "\n", 1);
}

static DraftContext draft_root_context = {
    {draft_default_allocator, (void *)0},
    {draft_temp_allocator, (void *)0},
    draft_default_assertion_failure,
    {draft_default_logger, (void *)0},
    {draft_default_random, (void *)0},
    (void *)0,
    0,
    (void *)0,
};

static _Thread_local DraftContext draft_thread_context;
static _Thread_local _Bool draft_thread_context_initialized;

static DraftContext *draft_ensure_thread_context(void) {
  if (!draft_thread_context_initialized) {
    draft_thread_context = draft_root_context;
    draft_thread_context_initialized = 1;
  }
  return &draft_thread_context;
}

DRAFT_HIDDEN void draft_runtime_attach_thread(void)
    DRAFT_SYMBOL("__draft.runtime.attach_thread");
DRAFT_HIDDEN void draft_runtime_attach_thread(void) {
  (void)draft_ensure_thread_context();
}

DRAFT_HIDDEN void draft_runtime_install_thread_context(
    const DraftContext *source)
    DRAFT_SYMBOL("__draft.runtime.install_thread_context");
DRAFT_HIDDEN void draft_runtime_install_thread_context(
    const DraftContext *source) {
  draft_thread_context = *source;
  draft_thread_context.temporary_allocator.procedure = draft_temp_allocator;
  draft_thread_context.temporary_allocator.user = (void *)0;
  draft_thread_context_initialized = 1;
}

DRAFT_HIDDEN DraftContext draft_runtime_default_context(void)
    DRAFT_SYMBOL("__draft.runtime.default_context");
DRAFT_HIDDEN DraftContext draft_runtime_default_context(void) {
  return *draft_ensure_thread_context();
}

DRAFT_HIDDEN void draft_runtime_reset_temporary_allocator(void)
    DRAFT_SYMBOL("__draft.runtime.reset_temporary_allocator");
DRAFT_HIDDEN void draft_runtime_reset_temporary_allocator(void) {
  DraftTempState *state = draft_ensure_temp_state();
  draft_reset_temp_state(state);
}

DRAFT_HIDDEN void draft_runtime_assert(
    DraftContext *context, _Bool condition, DraftString condition_text,
    DraftString message, DraftString file, usize line, usize column)
    DRAFT_SYMBOL("__draft.assert");
DRAFT_HIDDEN void draft_runtime_assert(
    DraftContext *context, _Bool condition, DraftString condition_text,
    DraftString message, DraftString file, usize line, usize column) {
  if (condition)
    return;
  if (context != (void *)0 && context->assertion_failure != (void *)0) {
    context->assertion_failure(
        context, condition_text, message, file, line, column);
  }
  __builtin_trap();
}

static DRAFT_NORETURN void draft_bounds_failure(void) {
  (void)draft_host_write(2, "Draft bounds check failed\n", 26);
  __builtin_trap();
  __builtin_unreachable();
}

DRAFT_HIDDEN void draft_runtime_bounds(
    usize index, usize length, const u8 *file, usize line, usize column)
    DRAFT_SYMBOL("__draft.bounds");
DRAFT_HIDDEN void draft_runtime_bounds(
    usize index, usize length, const u8 *file, usize line, usize column) {
  (void)file;
  (void)line;
  (void)column;
  if (index >= length)
    draft_bounds_failure();
}

DRAFT_HIDDEN void draft_runtime_slice_bounds(
    usize low, usize high, usize length, const u8 *file, usize line,
    usize column) DRAFT_SYMBOL("__draft.slice_bounds");
DRAFT_HIDDEN void draft_runtime_slice_bounds(
    usize low, usize high, usize length, const u8 *file, usize line,
    usize column) {
  (void)file;
  (void)line;
  (void)column;
  if (low > high || high > length)
    draft_bounds_failure();
}

static void draft_initialize_process_views(i32 argc, char **argv, char **envp) {
  draft_process_argument_count = argc < 0 ? 0 : (usize)argc;
  draft_process_arguments = (DraftString *)calloc(
      draft_process_argument_count, sizeof(DraftString));
  if (draft_process_argument_count != 0 &&
      draft_process_arguments == (void *)0)
    __builtin_trap();
  for (usize index = 0; index < draft_process_argument_count; ++index) {
    draft_process_arguments[index].data = (const u8 *)argv[index];
    draft_process_arguments[index].length = strlen(argv[index]);
  }

  usize environment_count = 0;
  if (envp != (void *)0) {
    while (envp[environment_count] != (void *)0)
      ++environment_count;
  }
  draft_process_environment_count = environment_count;
  draft_process_environment = (DraftString *)calloc(
      environment_count, sizeof(DraftString));
  if (environment_count != 0 && draft_process_environment == (void *)0)
    __builtin_trap();
  for (usize index = 0; index < environment_count; ++index) {
    draft_process_environment[index].data = (const u8 *)envp[index];
    draft_process_environment[index].length = strlen(envp[index]);
  }
}

#if defined(DRAFT_RUNTIME_WINDOWS)

static char *draft_utf16_to_utf8(const unsigned short *wide) {
  const usize wide_length = wcslen(wide);
  if (wide_length > 0x7fffffff)
    return (void *)0;
  if (wide_length == 0)
    return (char *)calloc(1, 1);
  const i32 required = WideCharToMultiByte(
      65001, 0, wide, (i32)wide_length, (void *)0, 0, (void *)0,
      (void *)0);
  if (required <= 0)
    return (void *)0;
  char *storage = (char *)calloc((usize)required + 1, 1);
  if (storage == (void *)0)
    return (void *)0;
  const i32 converted = WideCharToMultiByte(
      65001, 0, wide, (i32)wide_length, storage, required, (void *)0,
      (void *)0);
  if (converted != required) {
    free(storage);
    return (void *)0;
  }
  return storage;
}

static void draft_initialize_windows_process_views(
    i32 argc, unsigned short **wide_argv, unsigned short **wide_envp) {
  const usize argument_count = argc < 0 ? 0 : (usize)argc;
  draft_process_utf8_argv =
      (char **)calloc(argument_count, sizeof(char *));
  if (argument_count != 0 && draft_process_utf8_argv == (void *)0)
    __builtin_trap();
  for (usize index = 0; index < argument_count; ++index) {
    draft_process_utf8_argv[index] = draft_utf16_to_utf8(wide_argv[index]);
    if (draft_process_utf8_argv[index] == (void *)0)
      __builtin_trap();
  }

  usize environment_count = 0;
  while (wide_envp[environment_count] != (void *)0)
    ++environment_count;
  draft_process_utf8_envp =
      (char **)calloc(environment_count + 1, sizeof(char *));
  if (draft_process_utf8_envp == (void *)0)
    __builtin_trap();
  for (usize index = 0; index < environment_count; ++index) {
    draft_process_utf8_envp[index] = draft_utf16_to_utf8(wide_envp[index]);
    if (draft_process_utf8_envp[index] == (void *)0)
      __builtin_trap();
  }
  draft_initialize_process_views(
      argc, draft_process_utf8_argv, draft_process_utf8_envp);
}

#endif

DRAFT_HIDDEN DraftContext *draft_runtime_initialize_process(
    i32 argc, void *argv, void *envp)
    DRAFT_SYMBOL("__draft.runtime.initialize_process");
DRAFT_HIDDEN DraftContext *draft_runtime_initialize_process(
    i32 argc, void *argv, void *envp) {
  draft_initialize_standard_streams();
#if defined(DRAFT_RUNTIME_WINDOWS)
  draft_initialize_windows_process_views(
      argc, (unsigned short **)argv, (unsigned short **)envp);
#else
  draft_initialize_process_views(argc, (char **)argv, (char **)envp);
#endif
  return &draft_root_context;
}

DRAFT_HIDDEN void draft_runtime_shutdown_process(void)
    DRAFT_SYMBOL("__draft.runtime.shutdown_process");
DRAFT_HIDDEN void draft_runtime_shutdown_process(void) {
  draft_destroy_current_temp_state();
  draft_shutdown_temp_key();
#if defined(DRAFT_RUNTIME_WINDOWS)
  for (usize index = 0; index < draft_process_argument_count; ++index)
    free(draft_process_utf8_argv[index]);
  for (usize index = 0; index < draft_process_environment_count; ++index)
    free(draft_process_utf8_envp[index]);
  free(draft_process_utf8_argv);
  free(draft_process_utf8_envp);
  draft_process_utf8_argv = (void *)0;
  draft_process_utf8_envp = (void *)0;
#endif
  free(draft_process_arguments);
  free(draft_process_environment);
  draft_process_arguments = (void *)0;
  draft_process_argument_count = 0;
  draft_process_environment = (void *)0;
  draft_process_environment_count = 0;
}

DRAFT_HIDDEN void draft_runtime_validation_report(
    const void *data, usize size)
    DRAFT_SYMBOL("__draft.runtime.validation_report");
DRAFT_HIDDEN void draft_runtime_validation_report(
    const void *data, usize size) {
  (void)draft_host_write(3, data, size);
}

DRAFT_HIDDEN void *draft_os_args_data(void)
    DRAFT_SYMBOL("__draft.os.args_data");
DRAFT_HIDDEN void *draft_os_args_data(void) {
  return draft_process_arguments;
}

DRAFT_HIDDEN usize draft_os_args_count(void)
    DRAFT_SYMBOL("__draft.os.args_count");
DRAFT_HIDDEN usize draft_os_args_count(void) {
  return draft_process_argument_count;
}

DRAFT_HIDDEN void *draft_os_environment_data(void)
    DRAFT_SYMBOL("__draft.os.environment_data");
DRAFT_HIDDEN void *draft_os_environment_data(void) {
  return draft_process_environment;
}

DRAFT_HIDDEN usize draft_os_environment_count(void)
    DRAFT_SYMBOL("__draft.os.environment_count");
DRAFT_HIDDEN usize draft_os_environment_count(void) {
  return draft_process_environment_count;
}
