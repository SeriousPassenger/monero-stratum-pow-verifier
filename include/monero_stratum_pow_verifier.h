/*
 * Copyright (c) 2026 SeriousPassenger
 * SPDX-License-Identifier: MIT
 */

#ifndef MONERO_STRATUM_POW_VERIFIER_H
#define MONERO_STRATUM_POW_VERIFIER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MSPV_ABI_VERSION 1u
#define MSPV_HASH_SIZE 32u
#define MSPV_WAIT_FOREVER UINT32_MAX

typedef struct mspv_context mspv_context;
typedef uint64_t mspv_seed_id;

/*
 * Thread safety: all operations taking an mspv_context are synchronized and
 * may be called concurrently except mspv_destroy(), which requires exclusive
 * ownership after all other calls have returned. Callback storage must remain
 * valid until mspv_shutdown() or mspv_destroy() returns. Shutdown waits for
 * concurrently executing context calls before returning. A callback must not
 * call any MSPV function that takes the same context.
 *
 * This 0.x static library requires callers to rebuild against the matching
 * header. struct_size and abi_version reject mismatches; they do not promise
 * forward-compatible mixing of independently compiled headers and libraries.
 */

typedef int32_t mspv_status;
enum {
    MSPV_OK = 0,
    MSPV_INVALID_ARGUMENT = 1,
    MSPV_INVALID_CONFIG = 2,
    MSPV_NO_MEMORY = 3,
    MSPV_NOT_RUNNING = 4,
    MSPV_ALREADY_RUNNING = 5,
    MSPV_CLOSED = 6,
    MSPV_SEED_NOT_FOUND = 7,
    MSPV_SEED_NOT_READY = 8,
    MSPV_SEED_RELEASING = 9,
    MSPV_SEED_ACTIVE = 10,
    MSPV_SEED_CAPACITY = 11,
    MSPV_QUEUE_FULL = 12,
    MSPV_TIMEOUT = 13,
    MSPV_CANCELLED = 14,
    MSPV_UNSUPPORTED = 15,
    MSPV_RANDOMX_ERROR = 16,
    MSPV_INTERNAL_ERROR = 17
};

typedef uint32_t mspv_memory_mode;
enum {
    /* About 256 MiB + 2 MiB per worker/seed; lower memory, slower. */
    MSPV_MEMORY_LIGHT = 0,
    /*
     * About 2080 MiB + 2 MiB per worker/seed; higher memory, faster.
     * Preparing a FAST seed temporarily also needs a roughly 256 MiB cache.
     */
    MSPV_MEMORY_FAST = 1
};

typedef uint32_t mspv_large_page_mode;
enum {
    MSPV_LARGE_PAGES_DISABLED = 0,
    MSPV_LARGE_PAGES_TRY = 1,
    MSPV_LARGE_PAGES_REQUIRE = 2
};

typedef uint32_t mspv_option;
enum {
    MSPV_OPTION_NONE = 0,
    /* Do not use RandomX JIT even when the platform recommends it. */
    MSPV_OPTION_DISABLE_JIT = 1u << 0,
    /* Apply W^X protection to JIT pages. Invalid with DISABLE_JIT. */
    MSPV_OPTION_SECURE_JIT = 1u << 1,
    /* Use portable software AES instead of detected hardware AES. */
    MSPV_OPTION_DISABLE_HARD_AES = 1u << 2
};

typedef uint32_t mspv_seed_state;
enum {
    MSPV_SEED_PREPARING = 0,
    MSPV_SEED_READY = 1,
    MSPV_SEED_CURRENT = 2,
    MSPV_SEED_RELEASING_STATE = 3,
    MSPV_SEED_FAILED = 4
};

typedef uint32_t mspv_result;
enum {
    MSPV_RESULT_OK = 0,
    MSPV_RESULT_CANCELLED = 1,
    MSPV_RESULT_FAILED = 2
};

typedef uint32_t mspv_comparison;
enum {
    MSPV_COMPARISON_NOT_REQUESTED = 0,
    MSPV_COMPARISON_MATCH = 1,
    MSPV_COMPARISON_MISMATCH = 2
};

typedef uint32_t mspv_shutdown_mode;
enum {
    MSPV_SHUTDOWN_DRAIN = 0,
    MSPV_SHUTDOWN_CANCEL_PENDING = 1
};

typedef uint32_t mspv_log_level;
enum {
    MSPV_LOG_ERROR = 0,
    MSPV_LOG_WARNING = 1,
    MSPV_LOG_INFO = 2,
    MSPV_LOG_DEBUG = 3,
    MSPV_LOG_TRACE = 4
};

/*
 * Optional wake-up hint for event-loop integrations. It may run on any
 * verifier-owned thread, may be coalesced by the caller, and must return
 * promptly and must not call an MSPV function taking the same context. Hints
 * are emitted when the completion queue changes from empty to nonempty, seed
 * preparation resolves, or an owned thread fails. On a hint, drain completions
 * and query tracked seed state. No notification occurs after shutdown returns.
 */
typedef void (*mspv_notify_fn)(void *user_data);

/*
 * Optional diagnostic sink. message is valid only for the duration of the
 * call. The sink can be called concurrently and must not throw, block for a
 * long time, or call an MSPV function taking the same context. TRACE calls are
 * compiled in only when the library is built with MSPV_ENABLE_TRACE_LOGGING.
 */
typedef void (*mspv_log_fn)(void *user_data,
                            mspv_log_level level,
                            const char *message);

typedef struct mspv_config {
    uint32_t struct_size;
    uint32_t abi_version;

    uint32_t worker_count;
    uint32_t seed_init_threads;
    uint32_t pending_capacity;
    uint32_t max_outstanding;
    uint32_t max_input_size;
    uint32_t max_seed_key_size;
    uint32_t max_seeds;
    uint64_t max_buffered_input_bytes;

    mspv_memory_mode memory_mode;
    mspv_large_page_mode large_pages;
    uint32_t options;

    mspv_notify_fn notify;
    void *notify_user_data;
    mspv_log_fn log;
    void *log_user_data;
    mspv_log_level log_level;
} mspv_config;

typedef struct mspv_seed_info {
    mspv_seed_id seed_id;
    mspv_seed_state state;
    mspv_status last_error;
    uint32_t key_size;
    uint32_t queued_jobs;
    uint32_t running_jobs;
    uint64_t prepare_ns;
    /* LIGHT: persistent cache; FAST: persistent dataset. */
    uint8_t memory_uses_large_pages;
    /* True only when every worker VM's private scratchpad uses large pages. */
    uint8_t all_vms_use_large_pages;
    uint8_t reserved[6];
} mspv_seed_info;

typedef struct mspv_completion {
    mspv_result result;
    mspv_status error;
    mspv_comparison comparison;
    uint32_t reserved0;
    uint64_t ticket;
    uint64_t user_tag;
    mspv_seed_id seed_id;
    uint8_t hash[MSPV_HASH_SIZE];
    uint64_t queue_ns;
    uint64_t hash_ns;
    uint64_t total_ns;
} mspv_completion;

typedef struct mspv_stats {
    uint32_t workers;
    uint32_t seeds;
    uint32_t seeds_preparing;
    uint32_t seeds_ready;
    uint32_t pending;
    uint32_t running;
    uint32_t completions;
    uint32_t outstanding;
    uint64_t buffered_input_bytes;
    mspv_seed_id active_seed_id;
    uint64_t submitted;
    uint64_t completed;
    uint64_t cancelled;
    uint64_t failed;
} mspv_stats;

/* Initializes a configuration with safe, overridable defaults. */
mspv_status mspv_config_init(mspv_config *config);

/* Allocates an inert verifier. Call mspv_start() before preparing seeds. */
mspv_status mspv_create(const mspv_config *config,
                        mspv_context **out_context);

/* Starts the configured preparation controller and verification workers. */
mspv_status mspv_start(mspv_context *context);

/*
 * Asynchronously prepares an arbitrary RandomX key and returns a stable,
 * opaque seed ID. Portable RandomX keys are 1..60 bytes; Monero uses 32.
 * Repeating a resident key is idempotent. Seed builds are
 * serialized; seed_init_threads parallelizes FAST dataset construction.
 */
mspv_status mspv_seed_prepare(mspv_context *context,
                              const void *key,
                              size_t key_size,
                              mspv_seed_id *out_seed_id);

/* Waits until PREPARING resolves. OK means READY or CURRENT. */
mspv_status mspv_seed_wait_ready(mspv_context *context,
                                 mspv_seed_id seed_id,
                                 uint32_t timeout_ms);

mspv_status mspv_seed_get_info(mspv_context *context,
                               mspv_seed_id seed_id,
                               mspv_seed_info *out_info);

/*
 * Marks a ready seed current. The former current seed remains READY and is
 * not implicitly released. Every hash submission still names its exact seed.
 */
mspv_status mspv_seed_activate(mspv_context *context,
                               mspv_seed_id seed_id);

/* Clears the current designation without releasing any seed. */
mspv_status mspv_seed_deactivate(mspv_context *context);

/*
 * Immediately rejects new jobs for a non-current seed. Already accepted jobs
 * retain its resources and finish before those resources are destroyed.
 */
mspv_status mspv_seed_release(mspv_context *context,
                              mspv_seed_id seed_id);

mspv_status mspv_seed_wait_released(mspv_context *context,
                                    mspv_seed_id seed_id,
                                    uint32_t timeout_ms);

/* Calculates a raw 32-byte RandomX hash asynchronously. */
mspv_status mspv_hash_submit(mspv_context *context,
                             mspv_seed_id seed_id,
                             const void *input,
                             size_t input_size,
                             uint64_t user_tag,
                             uint64_t *out_ticket);

/*
 * Calculates a hash and compares it with claimed_hash. A mismatch is a
 * successful calculation reported as MSPV_COMPARISON_MISMATCH; the computed
 * hash remains authoritative and is always returned.
 */
mspv_status mspv_verify_submit(
    mspv_context *context,
    mspv_seed_id seed_id,
    const void *input,
    size_t input_size,
    const uint8_t claimed_hash[MSPV_HASH_SIZE],
    uint64_t user_tag,
    uint64_t *out_ticket);

/*
 * Retrieves one completion. Completion order is not guaranteed. Polling a
 * completion releases its bounded outstanding reservation.
 */
mspv_status mspv_completion_poll(mspv_context *context,
                                 mspv_completion *out_completion,
                                 uint32_t timeout_ms);

mspv_status mspv_get_stats(mspv_context *context, mspv_stats *out_stats);

/*
 * Stops admission and joins all owned threads. Running RandomX calls are not
 * interrupted. DRAIN finishes queued work; CANCEL_PENDING emits cancelled
 * completions for queued work while already-running calls finish normally.
 */
mspv_status mspv_shutdown(mspv_context *context, mspv_shutdown_mode mode);

/* Not safe to race with another API call on the same context. */
void mspv_destroy(mspv_context *context);

const char *mspv_status_string(mspv_status status);

#ifdef __cplusplus
}
#endif

#endif
