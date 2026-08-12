/*
 * Copyright (c) 2026 SeriousPassenger
 * SPDX-License-Identifier: MIT
 */

#include "monero_stratum_pow_verifier.h"

#include <randomx.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef MSPV_ENABLE_TRACE_LOGGING
#define MSPV_ENABLE_TRACE_LOGGING 0
#endif

namespace {

thread_local mspv_context *ownedThreadContext = nullptr;

struct OwnedThreadMarker {
    explicit OwnedThreadMarker(mspv_context *context) noexcept
    {
        ownedThreadContext = context;
    }

    ~OwnedThreadMarker() { ownedThreadContext = nullptr; }

    OwnedThreadMarker(const OwnedThreadMarker &) = delete;
    OwnedThreadMarker &operator=(const OwnedThreadMarker &) = delete;
};

using Clock = std::chrono::steady_clock;

constexpr uint32_t kMaxWorkers = 256;
constexpr uint32_t kMaxInitThreads = 256;
constexpr uint32_t kMaxPendingJobs = 1'000'000;
constexpr uint32_t kMaxOutstandingJobs = 1'000'000;
constexpr uint32_t kHardMaxInputSize = 64u * 1024u * 1024u;
constexpr uint32_t kHardMaxSeedKeySize = 60;
constexpr uint32_t kHardMaxSeeds = 64;
constexpr uint64_t kHardMaxBufferedInputBytes = 16ull * 1024ull * 1024ull * 1024ull;
constexpr unsigned long kDatasetChunkItems = 65'536ul;

enum class Lifecycle {
    Created,
    Starting,
    Running,
    Fatal,
    Stopping,
    Stopped
};

uint64_t elapsedNs(const Clock::time_point &begin,
                   const Clock::time_point &end) noexcept
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

randomx_flags addFlag(randomx_flags flags, randomx_flags flag) noexcept
{
    return static_cast<randomx_flags>(static_cast<int>(flags) |
                                      static_cast<int>(flag));
}

randomx_flags removeFlag(randomx_flags flags, randomx_flags flag) noexcept
{
    return static_cast<randomx_flags>(static_cast<int>(flags) &
                                      ~static_cast<int>(flag));
}

bool hasFlag(randomx_flags flags, randomx_flags flag) noexcept
{
    return (static_cast<int>(flags) & static_cast<int>(flag)) != 0;
}

const char *memoryModeName(mspv_memory_mode mode) noexcept
{
    return mode == MSPV_MEMORY_FAST ? "fast" : "light";
}

const char *largePageModeName(mspv_large_page_mode mode) noexcept
{
    if (mode == MSPV_LARGE_PAGES_REQUIRE) {
        return "required";
    }
    return mode == MSPV_LARGE_PAGES_TRY ? "try" : "off";
}

const char *jitModeName(uint32_t options) noexcept
{
    if ((options & MSPV_OPTION_DISABLE_JIT) != 0) {
        return "off";
    }
    return (options & MSPV_OPTION_SECURE_JIT) != 0 ? "secure" : "on";
}

struct SeedResources {
    randomx_cache *cache = nullptr;
    randomx_dataset *dataset = nullptr;
    std::vector<randomx_vm *> vms;
    bool memoryUsesLargePages = false;
    bool allVmsUseLargePages = false;

    ~SeedResources()
    {
        for (randomx_vm *vm : vms) {
            if (vm != nullptr) {
                randomx_destroy_vm(vm);
            }
        }
        if (dataset != nullptr) {
            randomx_release_dataset(dataset);
        }
        if (cache != nullptr) {
            randomx_release_cache(cache);
        }
    }

    SeedResources() = default;
    SeedResources(const SeedResources &) = delete;
    SeedResources &operator=(const SeedResources &) = delete;
};

struct SeedRecord {
    mspv_seed_id id = 0;
    std::string key;
    mspv_seed_state state = MSPV_SEED_PREPARING;
    mspv_status lastError = MSPV_OK;
    uint64_t prepareNs = 0;
    uint32_t queuedJobs = 0;
    uint32_t runningJobs = 0;
    bool preparationActive = true;
    std::atomic<bool> cancelPreparation{false};
    bool resourcesDetached = false;
    std::unique_ptr<SeedResources> resources;
};

struct VerifyJob {
    uint64_t ticket = 0;
    uint64_t userTag = 0;
    std::shared_ptr<SeedRecord> seed;
    std::vector<uint8_t> input;
    bool compare = false;
    uint8_t claimedHash[MSPV_HASH_SIZE]{};
    Clock::time_point submitted;
};

struct ApiCallGuard;

} // namespace

struct mspv_context {
    explicit mspv_context(const mspv_config &value)
        : config(value), completionRing(value.max_outstanding)
    {
    }

    mspv_config config{};
    std::mutex mutex;
    std::condition_variable preparationCv;
    std::condition_variable workCv;
    std::condition_variable completionCv;
    std::condition_variable seedCv;
    std::condition_variable stateCv;
    Lifecycle lifecycle = Lifecycle::Created;
    bool shutdownFailed = false;
    bool shutdownComplete = false;
    uint32_t apiCalls = 0;

    std::thread preparationThread;
    std::vector<std::thread> workers;
    std::deque<std::shared_ptr<SeedRecord>> preparationQueue;
    std::deque<std::shared_ptr<VerifyJob>> pending;

    std::vector<mspv_completion> completionRing;
    size_t completionHead = 0;
    size_t completionTail = 0;
    size_t completionCount = 0;

    std::unordered_map<mspv_seed_id, std::shared_ptr<SeedRecord>> seeds;
    std::unordered_map<std::string, mspv_seed_id> seedsByKey;
    mspv_seed_id activeSeed = 0;
    mspv_seed_id nextSeedId = 1;
    uint64_t nextTicket = 1;

    uint32_t runningJobs = 0;
    uint32_t submissionsPreparing = 0;
    uint32_t outstanding = 0;
    uint64_t bufferedInputBytes = 0;
    uint64_t submitted = 0;
    uint64_t completed = 0;
    uint64_t cancelled = 0;
    uint64_t failed = 0;
};

namespace {

struct ApiCallGuard {
    explicit ApiCallGuard(mspv_context *value) noexcept : context(value)
    {
        if (context == nullptr) {
            return;
        }
        try {
            std::lock_guard<std::mutex> lock(context->mutex);
            ++context->apiCalls;
            active = true;
        }
        catch (...) {
            /* The called operation will report any subsequent lock failure. */
        }
    }

    ~ApiCallGuard()
    {
        if (!active) {
            return;
        }
        try {
            std::lock_guard<std::mutex> lock(context->mutex);
            if (context->apiCalls > 0) {
                --context->apiCalls;
            }
            context->stateCv.notify_all();
        }
        catch (...) {
            /* A synchronization failure is already unrecoverable. */
        }
    }

    ApiCallGuard(const ApiCallGuard &) = delete;
    ApiCallGuard &operator=(const ApiCallGuard &) = delete;

    mspv_context *context = nullptr;
    bool active = false;
};

[[gnu::format(printf, 3, 4)]] void logMessage(mspv_context *context,
                mspv_log_level level,
                const char *format,
                ...) noexcept
{
    if (context == nullptr || context->config.log == nullptr ||
        level > context->config.log_level) {
        return;
    }
#if !MSPV_ENABLE_TRACE_LOGGING
    if (level == MSPV_LOG_TRACE) {
        return;
    }
#endif

    char message[768];
    va_list arguments;
    va_start(arguments, format);
    const int written = std::vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);
    if (written < 0) {
        return;
    }
    message[sizeof(message) - 1] = '\0';

    try {
        context->config.log(context->config.log_user_data, level, message);
    }
    catch (...) {
        /* A diagnostic callback must never break verifier state. */
    }
}

void notifyCaller(mspv_context *context) noexcept
{
    if (context == nullptr || context->config.notify == nullptr) {
        return;
    }
    try {
        context->config.notify(context->config.notify_user_data);
    }
    catch (...) {
        /* C++ callbacks are not allowed to leak exceptions through the C ABI. */
    }
}

bool validConfig(const mspv_config &config) noexcept
{
    constexpr uint32_t knownOptions = MSPV_OPTION_DISABLE_JIT |
                                      MSPV_OPTION_SECURE_JIT |
                                      MSPV_OPTION_DISABLE_HARD_AES;

    if (config.struct_size != sizeof(mspv_config) ||
        config.abi_version != MSPV_ABI_VERSION ||
        config.worker_count == 0 || config.worker_count > kMaxWorkers ||
        config.seed_init_threads == 0 ||
        config.seed_init_threads > kMaxInitThreads ||
        config.pending_capacity == 0 ||
        config.pending_capacity > kMaxPendingJobs ||
        config.max_outstanding == 0 ||
        config.max_outstanding > kMaxOutstandingJobs ||
        config.pending_capacity > config.max_outstanding ||
        config.max_input_size == 0 ||
        config.max_input_size > kHardMaxInputSize ||
        config.max_seed_key_size == 0 ||
        config.max_seed_key_size > kHardMaxSeedKeySize ||
        config.max_seeds == 0 || config.max_seeds > kHardMaxSeeds ||
        config.max_buffered_input_bytes < config.max_input_size ||
        config.max_buffered_input_bytes > kHardMaxBufferedInputBytes ||
        (config.memory_mode != MSPV_MEMORY_LIGHT &&
         config.memory_mode != MSPV_MEMORY_FAST) ||
        (config.large_pages != MSPV_LARGE_PAGES_DISABLED &&
         config.large_pages != MSPV_LARGE_PAGES_TRY &&
         config.large_pages != MSPV_LARGE_PAGES_REQUIRE) ||
        (config.options & ~knownOptions) != 0 ||
        config.log_level > MSPV_LOG_TRACE) {
        return false;
    }

    return !((config.options & MSPV_OPTION_DISABLE_JIT) != 0 &&
             (config.options & MSPV_OPTION_SECURE_JIT) != 0);
}

randomx_flags selectedFlags(const mspv_config &config) noexcept
{
    randomx_flags flags = randomx_get_flags();
    if ((config.options & MSPV_OPTION_DISABLE_JIT) != 0) {
        flags = removeFlag(flags, RANDOMX_FLAG_JIT);
        flags = removeFlag(flags, RANDOMX_FLAG_SECURE);
    }
    else if ((config.options & MSPV_OPTION_SECURE_JIT) != 0 &&
             hasFlag(flags, RANDOMX_FLAG_JIT)) {
        flags = addFlag(flags, RANDOMX_FLAG_SECURE);
    }
    if ((config.options & MSPV_OPTION_DISABLE_HARD_AES) != 0) {
        flags = removeFlag(flags, RANDOMX_FLAG_HARD_AES);
    }
    return flags;
}

randomx_cache *allocateCache(randomx_flags flags,
                             mspv_large_page_mode mode,
                             bool &usedLargePages) noexcept
{
    usedLargePages = false;
    if (mode != MSPV_LARGE_PAGES_DISABLED) {
        randomx_cache *cache =
            randomx_alloc_cache(addFlag(flags, RANDOMX_FLAG_LARGE_PAGES));
        if (cache != nullptr) {
            usedLargePages = true;
            return cache;
        }
        if (mode == MSPV_LARGE_PAGES_REQUIRE) {
            return nullptr;
        }
    }
    return randomx_alloc_cache(removeFlag(flags, RANDOMX_FLAG_LARGE_PAGES));
}

randomx_dataset *allocateDataset(mspv_large_page_mode mode,
                                 bool &usedLargePages) noexcept
{
    usedLargePages = false;
    if (mode != MSPV_LARGE_PAGES_DISABLED) {
        randomx_dataset *dataset =
            randomx_alloc_dataset(RANDOMX_FLAG_LARGE_PAGES);
        if (dataset != nullptr) {
            usedLargePages = true;
            return dataset;
        }
        if (mode == MSPV_LARGE_PAGES_REQUIRE) {
            return nullptr;
        }
    }
    return randomx_alloc_dataset(RANDOMX_FLAG_DEFAULT);
}

randomx_vm *allocateVm(randomx_flags flags,
                       randomx_cache *cache,
                       randomx_dataset *dataset,
                       mspv_large_page_mode mode,
                       bool &usedLargePages) noexcept
{
    usedLargePages = false;
    if (mode != MSPV_LARGE_PAGES_DISABLED) {
        randomx_vm *vm = randomx_create_vm(
            addFlag(flags, RANDOMX_FLAG_LARGE_PAGES), cache, dataset);
        if (vm != nullptr) {
            usedLargePages = true;
            return vm;
        }
        if (mode == MSPV_LARGE_PAGES_REQUIRE) {
            return nullptr;
        }
    }
    return randomx_create_vm(
        removeFlag(flags, RANDOMX_FLAG_LARGE_PAGES), cache, dataset);
}

mspv_status initializeDataset(randomx_dataset *dataset,
                              randomx_cache *cache,
                              uint32_t requestedThreads,
                              const std::atomic<bool> &cancelRequested)
{
    const unsigned long itemCount = randomx_dataset_item_count();
    if (itemCount == 0) {
        return MSPV_RANDOMX_ERROR;
    }

    const uint32_t threadCount = std::min<uint32_t>(
        requestedThreads,
        itemCount > static_cast<unsigned long>(kMaxInitThreads)
            ? kMaxInitThreads
            : static_cast<uint32_t>(itemCount));
    std::atomic<unsigned long> nextItem{0};
    std::atomic<bool> workerFailed{false};
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    try {
        for (uint32_t index = 0; index < threadCount; ++index) {
            threads.emplace_back([&]() noexcept {
                try {
                    for (;;) {
                        if (cancelRequested.load(std::memory_order_relaxed)) {
                            return;
                        }
                        const unsigned long start =
                            nextItem.fetch_add(kDatasetChunkItems,
                                               std::memory_order_relaxed);
                        if (start >= itemCount) {
                            return;
                        }
                        const unsigned long remaining = itemCount - start;
                        const unsigned long count =
                            std::min(kDatasetChunkItems, remaining);
                        randomx_init_dataset(dataset, cache, start, count);
                    }
                }
                catch (...) {
                    workerFailed.store(true, std::memory_order_relaxed);
                }
            });
        }
    }
    catch (...) {
        workerFailed.store(true, std::memory_order_relaxed);
    }

    for (std::thread &thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }

    if (cancelRequested.load(std::memory_order_relaxed)) {
        return MSPV_CANCELLED;
    }
    if (workerFailed.load(std::memory_order_relaxed) ||
        nextItem.load(std::memory_order_relaxed) < itemCount) {
        return MSPV_INTERNAL_ERROR;
    }
    return MSPV_OK;
}

mspv_status buildSeedResources(mspv_context *context,
                               const std::string &key,
                               const std::atomic<bool> &cancelRequested,
                               std::unique_ptr<SeedResources> &out)
{
    const mspv_config &config = context->config;
    try {
        std::unique_ptr<SeedResources> resources(new SeedResources());
        logMessage(context,
                   MSPV_LOG_DEBUG,
                   "allocating %s seed: shared=%u MiB vm_scratchpads=%u MiB",
                   memoryModeName(config.memory_mode),
                   config.memory_mode == MSPV_MEMORY_FAST ? 2080u : 256u,
                   config.worker_count * 2u);
        randomx_flags selected = selectedFlags(config);
        randomx_flags cacheFlags = static_cast<randomx_flags>(
            static_cast<int>(selected) &
            (static_cast<int>(RANDOMX_FLAG_JIT) |
             static_cast<int>(RANDOMX_FLAG_ARGON2)));

        bool cacheLargePages = false;
        resources->cache = allocateCache(cacheFlags,
                                         config.large_pages,
                                         cacheLargePages);
        if (resources->cache == nullptr) {
            logMessage(context,
                       MSPV_LOG_ERROR,
                       "%sRandomX cache allocation failed",
                       config.large_pages == MSPV_LARGE_PAGES_REQUIRE
                           ? "required large-page "
                           : "");
            return MSPV_RANDOMX_ERROR;
        }
        if (config.large_pages == MSPV_LARGE_PAGES_TRY &&
            !cacheLargePages) {
            logMessage(context,
                       MSPV_LOG_WARNING,
                       "large-page RandomX cache unavailable; using regular pages");
        }

        randomx_init_cache(resources->cache, key.data(), key.size());
        if (cancelRequested.load(std::memory_order_relaxed)) {
            return MSPV_CANCELLED;
        }

        randomx_flags vmFlags = static_cast<randomx_flags>(
            static_cast<int>(selected) &
            (static_cast<int>(RANDOMX_FLAG_HARD_AES) |
             static_cast<int>(RANDOMX_FLAG_JIT) |
             static_cast<int>(RANDOMX_FLAG_SECURE)));

        if (config.memory_mode == MSPV_MEMORY_FAST) {
            resources->dataset = allocateDataset(
                config.large_pages, resources->memoryUsesLargePages);
            if (resources->dataset == nullptr) {
                logMessage(context,
                           MSPV_LOG_ERROR,
                           "%sRandomX dataset allocation failed",
                           config.large_pages == MSPV_LARGE_PAGES_REQUIRE
                               ? "required large-page "
                               : "");
                return MSPV_RANDOMX_ERROR;
            }
            if (config.large_pages == MSPV_LARGE_PAGES_TRY &&
                !resources->memoryUsesLargePages) {
                logMessage(context,
                           MSPV_LOG_WARNING,
                           "large-page RandomX dataset unavailable; using regular pages");
            }

            const mspv_status initialized = initializeDataset(
                resources->dataset,
                resources->cache,
                config.seed_init_threads,
                cancelRequested);
            if (initialized != MSPV_OK) {
                return initialized;
            }

            randomx_release_cache(resources->cache);
            resources->cache = nullptr;
            vmFlags = addFlag(vmFlags, RANDOMX_FLAG_FULL_MEM);
        }
        else {
            resources->memoryUsesLargePages = cacheLargePages;
        }

        resources->vms.reserve(config.worker_count);
        resources->allVmsUseLargePages =
            config.large_pages != MSPV_LARGE_PAGES_DISABLED;
        uint32_t largePageVms = 0;
        for (uint32_t index = 0; index < config.worker_count; ++index) {
            if (cancelRequested.load(std::memory_order_relaxed)) {
                return MSPV_CANCELLED;
            }
            bool vmLargePages = false;
            randomx_vm *vm = allocateVm(vmFlags,
                                        resources->cache,
                                        resources->dataset,
                                        config.large_pages,
                                        vmLargePages);
            if (vm == nullptr) {
                logMessage(context,
                           MSPV_LOG_ERROR,
                           "%sRandomX VM allocation failed at worker %u",
                           config.large_pages == MSPV_LARGE_PAGES_REQUIRE
                               ? "required large-page "
                               : "",
                           index);
                return MSPV_RANDOMX_ERROR;
            }
            resources->vms.push_back(vm);
            largePageVms += vmLargePages ? 1u : 0u;
            resources->allVmsUseLargePages =
                resources->allVmsUseLargePages && vmLargePages;
        }
        if (config.large_pages == MSPV_LARGE_PAGES_TRY &&
            largePageVms != config.worker_count) {
            logMessage(context,
                       MSPV_LOG_WARNING,
                       "large-page VM scratchpads unavailable for %u/%u workers; "
                       "using regular pages for those VMs",
                       config.worker_count - largePageVms,
                       config.worker_count);
        }

        out = std::move(resources);
        return MSPV_OK;
    }
    catch (const std::bad_alloc &) {
        return MSPV_NO_MEMORY;
    }
    catch (...) {
        return MSPV_RANDOMX_ERROR;
    }
}

bool detachReleasedSeedLocked(
    mspv_context *context,
    const std::shared_ptr<SeedRecord> &seed,
    std::unique_ptr<SeedResources> &outResources)
{
    if (seed->state != MSPV_SEED_RELEASING_STATE ||
        seed->preparationActive || seed->queuedJobs != 0 ||
        seed->runningJobs != 0 || seed->resourcesDetached) {
        return false;
    }

    const auto byKey = context->seedsByKey.find(seed->key);
    if (byKey != context->seedsByKey.end() && byKey->second == seed->id) {
        context->seedsByKey.erase(byKey);
    }
    seed->resourcesDetached = true;
    outResources = std::move(seed->resources);
    return true;
}

void finalizeReleasedSeed(mspv_context *context,
                          const std::shared_ptr<SeedRecord> &seed,
                          std::unique_ptr<SeedResources> resources) noexcept
{
    /* Destroy the potentially multi-gigabyte resources before reporting absent. */
    resources.reset();
    try {
        std::lock_guard<std::mutex> lock(context->mutex);
        const auto found = context->seeds.find(seed->id);
        if (found != context->seeds.end() && found->second == seed &&
            seed->state == MSPV_SEED_RELEASING_STATE &&
            seed->resourcesDetached && !seed->preparationActive &&
            seed->queuedJobs == 0 && seed->runningJobs == 0) {
            context->seeds.erase(found);
            context->seedCv.notify_all();
        }
    }
    catch (...) {
        logMessage(context,
                   MSPV_LOG_ERROR,
                   "failed to finalize released seed %llu",
                   static_cast<unsigned long long>(seed->id));
    }
}

bool pushCompletionLocked(mspv_context *context,
                          const mspv_completion &completion) noexcept
{
    if (context->completionCount >= context->completionRing.size()) {
        return false;
    }
    context->completionRing[context->completionTail] = completion;
    context->completionTail =
        (context->completionTail + 1) % context->completionRing.size();
    ++context->completionCount;
    return true;
}

bool markStopped(mspv_context *context, bool failed) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->shutdownFailed = context->shutdownFailed || failed;
        context->lifecycle = Lifecycle::Stopped;
        context->completionCv.notify_all();
        context->seedCv.notify_all();
        context->stateCv.notify_all();
        return context->shutdownFailed;
    }
    catch (...) {
        /* Nothing further can be done safely after a mutex failure. */
        return true;
    }
}

bool waitForApiCallsToFinish(mspv_context *context) noexcept
{
    try {
        std::unique_lock<std::mutex> lock(context->mutex);
        context->stateCv.wait(lock, [context]() {
            return context->apiCalls == 0;
        });
        return true;
    }
    catch (...) {
        return false;
    }
}

void markFatal(mspv_context *context) noexcept
{
    try {
        std::lock_guard<std::mutex> lock(context->mutex);
        context->shutdownFailed = true;
        if (context->lifecycle == Lifecycle::Running) {
            context->lifecycle = Lifecycle::Fatal;
        }
        for (const auto &entry : context->seeds) {
            if (entry.second->state == MSPV_SEED_PREPARING) {
                entry.second->cancelPreparation.store(
                    true, std::memory_order_relaxed);
            }
        }
        context->completionCv.notify_all();
        context->seedCv.notify_all();
        context->stateCv.notify_all();
    }
    catch (...) {
        /* A failed synchronization primitive is unrecoverable. */
    }
    context->preparationCv.notify_all();
    context->workCv.notify_all();
    notifyCaller(context);
}

mspv_completion cancelledCompletion(const VerifyJob &job) noexcept
{
    mspv_completion completion{};
    completion.result = MSPV_RESULT_CANCELLED;
    completion.error = MSPV_CANCELLED;
    completion.comparison = MSPV_COMPARISON_NOT_REQUESTED;
    completion.ticket = job.ticket;
    completion.user_tag = job.userTag;
    completion.seed_id = job.seed->id;
    completion.total_ns = elapsedNs(job.submitted, Clock::now());
    return completion;
}

uint64_t releaseJobInput(VerifyJob &job) noexcept
{
    const uint64_t inputBytes = static_cast<uint64_t>(job.input.size());
    std::vector<uint8_t> released;
    released.swap(job.input);
    return inputBytes;
}

void rollbackSubmissionLocked(mspv_context *context,
                              uint64_t inputBytes) noexcept
{
    if (context->submissionsPreparing > 0) {
        --context->submissionsPreparing;
    }
    if (context->outstanding > 0) {
        --context->outstanding;
    }
    context->bufferedInputBytes =
        inputBytes <= context->bufferedInputBytes
            ? context->bufferedInputBytes - inputBytes
            : 0;
    context->stateCv.notify_all();
}

uint64_t cancelPendingLocked(mspv_context *context,
                             bool &notifyCompletion) noexcept
{
    uint64_t cancelled = 0;
    while (!context->pending.empty()) {
        std::shared_ptr<VerifyJob> job = context->pending.front();
        context->pending.pop_front();
        if (job->seed->queuedJobs > 0) {
            --job->seed->queuedJobs;
        }
        const mspv_completion completion = cancelledCompletion(*job);
        const uint64_t inputBytes = releaseJobInput(*job);
        context->bufferedInputBytes =
            inputBytes <= context->bufferedInputBytes
                ? context->bufferedInputBytes - inputBytes
                : 0;
        const bool wasEmpty = context->completionCount == 0;
        if (pushCompletionLocked(context, completion)) {
            notifyCompletion = notifyCompletion || wasEmpty;
            ++context->completed;
            ++context->cancelled;
            ++cancelled;
        }
        else if (context->outstanding > 0) {
            --context->outstanding;
        }
    }
    return cancelled;
}

bool constantTimeEqual(const uint8_t *left,
                       const uint8_t *right,
                       size_t size) noexcept
{
    uint8_t difference = 0;
    for (size_t index = 0; index < size; ++index) {
        difference = static_cast<uint8_t>(difference |
                                          (left[index] ^ right[index]));
    }
    return difference == 0;
}

template<typename Predicate>
bool waitFor(std::condition_variable &condition,
             std::unique_lock<std::mutex> &lock,
             uint32_t timeoutMs,
             Predicate predicate)
{
    if (timeoutMs == MSPV_WAIT_FOREVER) {
        condition.wait(lock, predicate);
        return true;
    }
    return condition.wait_for(lock,
                              std::chrono::milliseconds(timeoutMs),
                              predicate);
}

mspv_seed_id nextSeedIdLocked(mspv_context *context) noexcept
{
    for (;;) {
        const mspv_seed_id candidate = context->nextSeedId++;
        if (candidate != 0 && context->seeds.find(candidate) == context->seeds.end()) {
            return candidate;
        }
    }
}

uint64_t nextTicketLocked(mspv_context *context) noexcept
{
    for (;;) {
        const uint64_t candidate = context->nextTicket++;
        if (candidate != 0) {
            return candidate;
        }
    }
}

void preparationLoop(mspv_context *context) noexcept
{
    const OwnedThreadMarker ownedThread(context);
    try {
        for (;;) {
            std::shared_ptr<SeedRecord> seed;
            std::unique_ptr<SeedResources> releasedResources;
            bool shouldFinalizeRelease = false;
            bool skipPreparation = false;
            {
                std::unique_lock<std::mutex> lock(context->mutex);
                context->preparationCv.wait(lock, [context]() {
                    return context->lifecycle == Lifecycle::Fatal ||
                           context->lifecycle == Lifecycle::Stopping ||
                           context->lifecycle == Lifecycle::Stopped ||
                           (context->lifecycle == Lifecycle::Running &&
                            !context->preparationQueue.empty());
                });

                if (context->lifecycle == Lifecycle::Fatal ||
                    context->lifecycle == Lifecycle::Stopped) {
                    return;
                }
                if (context->preparationQueue.empty()) {
                    if (context->lifecycle == Lifecycle::Stopping) {
                        return;
                    }
                    continue;
                }

                seed = context->preparationQueue.front();
                context->preparationQueue.pop_front();
                if (context->lifecycle != Lifecycle::Running ||
                    seed->state == MSPV_SEED_RELEASING_STATE) {
                    seed->cancelPreparation.store(true,
                                                  std::memory_order_relaxed);
                    seed->preparationActive = false;
                    if (seed->state != MSPV_SEED_RELEASING_STATE) {
                        seed->state = MSPV_SEED_RELEASING_STATE;
                        seed->lastError = MSPV_CLOSED;
                    }
                    shouldFinalizeRelease =
                        detachReleasedSeedLocked(context,
                                                 seed,
                                                 releasedResources);
                    skipPreparation = true;
                }
            }

            if (skipPreparation) {
                if (shouldFinalizeRelease) {
                    finalizeReleasedSeed(context,
                                         seed,
                                         std::move(releasedResources));
                }
                logMessage(context,
                           MSPV_LOG_DEBUG,
                           "seed %llu preparation skipped during release",
                           static_cast<unsigned long long>(seed->id));
                continue;
            }

            logMessage(context,
                       MSPV_LOG_INFO,
                       "seed %llu preparation started (%s mode)",
                       static_cast<unsigned long long>(seed->id),
                       memoryModeName(context->config.memory_mode));
            const Clock::time_point begin = Clock::now();
            std::unique_ptr<SeedResources> resources;
            const mspv_status status = buildSeedResources(
                context, seed->key, seed->cancelPreparation, resources);
            const uint64_t prepareNs = elapsedNs(begin, Clock::now());

            bool becameReady = false;
            bool becameFailed = false;
            {
                std::lock_guard<std::mutex> lock(context->mutex);
                seed->preparationActive = false;
                seed->prepareNs = prepareNs;

                if (seed->state == MSPV_SEED_RELEASING_STATE ||
                    context->lifecycle != Lifecycle::Running) {
                    seed->state = MSPV_SEED_RELEASING_STATE;
                    seed->lastError = context->lifecycle == Lifecycle::Running
                                          ? MSPV_CANCELLED
                                          : MSPV_CLOSED;
                    shouldFinalizeRelease =
                        detachReleasedSeedLocked(context,
                                                 seed,
                                                 releasedResources);
                }
                else if (status == MSPV_OK) {
                    seed->resources = std::move(resources);
                    seed->state = MSPV_SEED_READY;
                    seed->lastError = MSPV_OK;
                    becameReady = true;
                }
                else {
                    seed->state = MSPV_SEED_FAILED;
                    seed->lastError = status;
                    becameFailed = true;
                }
                context->seedCv.notify_all();
            }

            if (shouldFinalizeRelease) {
                finalizeReleasedSeed(context,
                                     seed,
                                     std::move(releasedResources));
            }

            if (becameReady) {
                logMessage(context,
                           MSPV_LOG_INFO,
                           "seed %llu ready after %.3f ms",
                           static_cast<unsigned long long>(seed->id),
                           static_cast<double>(prepareNs) / 1'000'000.0);
            }
            else if (becameFailed) {
                logMessage(context,
                           MSPV_LOG_ERROR,
                           "seed %llu preparation failed: %s",
                           static_cast<unsigned long long>(seed->id),
                           mspv_status_string(status));
            }
            else {
                logMessage(context,
                           MSPV_LOG_DEBUG,
                           "seed %llu preparation cancelled after %.3f ms",
                           static_cast<unsigned long long>(seed->id),
                           static_cast<double>(prepareNs) / 1'000'000.0);
            }
            notifyCaller(context);
        }
    }
    catch (...) {
        logMessage(context,
                   MSPV_LOG_ERROR,
                   "unhandled exception in seed preparation controller");
        markFatal(context);
    }
}

void workerLoop(mspv_context *context, uint32_t workerIndex) noexcept
{
    const OwnedThreadMarker ownedThread(context);
    try {
        for (;;) {
            std::shared_ptr<VerifyJob> job;
            SeedResources *resources = nullptr;
            {
                std::unique_lock<std::mutex> lock(context->mutex);
                context->workCv.wait(lock, [context]() {
                    return context->lifecycle == Lifecycle::Fatal ||
                           context->lifecycle == Lifecycle::Stopping ||
                           context->lifecycle == Lifecycle::Stopped ||
                           (context->lifecycle == Lifecycle::Running &&
                            !context->pending.empty());
                });

                if (context->lifecycle == Lifecycle::Fatal ||
                    context->lifecycle == Lifecycle::Stopped) {
                    return;
                }
                if (context->pending.empty()) {
                    if (context->lifecycle == Lifecycle::Stopping) {
                        return;
                    }
                    continue;
                }

                job = context->pending.front();
                context->pending.pop_front();
                --job->seed->queuedJobs;
                ++job->seed->runningJobs;
                ++context->runningJobs;
                resources = job->seed->resources.get();
            }

            mspv_completion completion{};
            completion.result = MSPV_RESULT_OK;
            completion.error = MSPV_OK;
            completion.comparison = MSPV_COMPARISON_NOT_REQUESTED;
            completion.ticket = job->ticket;
            completion.user_tag = job->userTag;
            completion.seed_id = job->seed->id;

            const Clock::time_point hashBegin = Clock::now();
            completion.queue_ns = elapsedNs(job->submitted, hashBegin);
            if (resources != nullptr &&
                workerIndex < resources->vms.size() &&
                resources->vms[workerIndex] != nullptr) {
                try {
                    randomx_calculate_hash(resources->vms[workerIndex],
                                           job->input.data(),
                                           job->input.size(),
                                           completion.hash);
                    if (job->compare) {
                        completion.comparison = constantTimeEqual(
                            completion.hash,
                            job->claimedHash,
                            MSPV_HASH_SIZE)
                                                    ? MSPV_COMPARISON_MATCH
                                                    : MSPV_COMPARISON_MISMATCH;
                    }
                }
                catch (...) {
                    completion.result = MSPV_RESULT_FAILED;
                    completion.error = MSPV_RANDOMX_ERROR;
                    completion.comparison = MSPV_COMPARISON_NOT_REQUESTED;
                    std::memset(completion.hash, 0, sizeof(completion.hash));
                }
            }
            else {
                completion.result = MSPV_RESULT_FAILED;
                completion.error = MSPV_INTERNAL_ERROR;
                completion.comparison = MSPV_COMPARISON_NOT_REQUESTED;
                std::memset(completion.hash, 0, sizeof(completion.hash));
            }
            const Clock::time_point hashEnd = Clock::now();
            completion.hash_ns = elapsedNs(hashBegin, hashEnd);
            completion.total_ns = elapsedNs(job->submitted, hashEnd);
            /* Free the copied payload before making its byte budget reusable. */
            const uint64_t inputBytes = releaseJobInput(*job);

            bool ringFailure = false;
            bool notifyCompletion = false;
            std::unique_ptr<SeedResources> releasedResources;
            bool shouldFinalizeRelease = false;
            {
                std::lock_guard<std::mutex> lock(context->mutex);
                --job->seed->runningJobs;
                --context->runningJobs;
                if (inputBytes <= context->bufferedInputBytes) {
                    context->bufferedInputBytes -= inputBytes;
                }
                else {
                    context->bufferedInputBytes = 0;
                    ringFailure = true;
                }

                const bool completionQueueWasEmpty =
                    context->completionCount == 0;
                if (!pushCompletionLocked(context, completion)) {
                    ringFailure = true;
                    if (context->outstanding > 0) {
                        --context->outstanding;
                    }
                }
                else {
                    notifyCompletion = completionQueueWasEmpty;
                    ++context->completed;
                    if (completion.result == MSPV_RESULT_FAILED) {
                        ++context->failed;
                    }
                }
                shouldFinalizeRelease =
                    detachReleasedSeedLocked(context,
                                             job->seed,
                                             releasedResources);
                context->completionCv.notify_one();
            }

            if (shouldFinalizeRelease) {
                finalizeReleasedSeed(context,
                                     job->seed,
                                     std::move(releasedResources));
            }

            if (ringFailure) {
                logMessage(context,
                           MSPV_LOG_ERROR,
                           "internal queue invariant failed for ticket %llu",
                           static_cast<unsigned long long>(job->ticket));
            }
            logMessage(context,
                       MSPV_LOG_TRACE,
                       "worker %u completed ticket %llu seed %llu in %.3f ms",
                       workerIndex,
                       static_cast<unsigned long long>(job->ticket),
                       static_cast<unsigned long long>(job->seed->id),
                       static_cast<double>(completion.hash_ns) / 1'000'000.0);
            if (notifyCompletion) {
                notifyCaller(context);
            }
        }
    }
    catch (...) {
        logMessage(context,
                   MSPV_LOG_ERROR,
                   "unhandled exception in verification worker %u",
                   workerIndex);
        markFatal(context);
    }
}

mspv_status startImpl(mspv_context *context)
{
    if (context == nullptr) {
        return MSPV_INVALID_ARGUMENT;
    }

    std::unique_lock<std::mutex> lock(context->mutex);
    if (context->lifecycle == Lifecycle::Running ||
        context->lifecycle == Lifecycle::Starting) {
        return MSPV_ALREADY_RUNNING;
    }
    if (context->lifecycle != Lifecycle::Created) {
        return MSPV_CLOSED;
    }

    context->lifecycle = Lifecycle::Starting;
    try {
        context->workers.reserve(context->config.worker_count);
        context->preparationThread = std::thread(preparationLoop, context);
        for (uint32_t index = 0; index < context->config.worker_count; ++index) {
            context->workers.emplace_back(workerLoop, context, index);
        }
        context->lifecycle = Lifecycle::Running;
    }
    catch (...) {
        context->lifecycle = Lifecycle::Stopping;
        lock.unlock();
        context->preparationCv.notify_all();
        context->workCv.notify_all();
        if (context->preparationThread.joinable()) {
            context->preparationThread.join();
        }
        for (std::thread &worker : context->workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        lock.lock();
        context->lifecycle = Lifecycle::Stopped;
        context->shutdownComplete = true;
        context->stateCv.notify_all();
        lock.unlock();
        logMessage(context,
                   MSPV_LOG_ERROR,
                   "failed to start verifier-owned threads");
        return MSPV_INTERNAL_ERROR;
    }

    lock.unlock();
    context->preparationCv.notify_all();
    context->workCv.notify_all();
    logMessage(context,
               MSPV_LOG_INFO,
               "verifier started: mode=%s workers=%u init_threads=%u "
               "max_seeds=%u pending=%u outstanding=%u pages=%s jit=%s aes=%s",
               memoryModeName(context->config.memory_mode),
               context->config.worker_count,
               context->config.seed_init_threads,
               context->config.max_seeds,
               context->config.pending_capacity,
               context->config.max_outstanding,
               largePageModeName(context->config.large_pages),
               jitModeName(context->config.options),
               (context->config.options & MSPV_OPTION_DISABLE_HARD_AES) != 0
                   ? "software"
                   : "detected");
    return MSPV_OK;
}

mspv_status submitImpl(mspv_context *context,
                       mspv_seed_id seedId,
                       const void *input,
                       size_t inputSize,
                       const uint8_t *claimedHash,
                       uint64_t userTag,
                       uint64_t *outTicket)
{
    if (context == nullptr || seedId == 0 || input == nullptr ||
        inputSize == 0 || outTicket == nullptr) {
        return MSPV_INVALID_ARGUMENT;
    }
    *outTicket = 0;
    if (inputSize > context->config.max_input_size) {
        return MSPV_INVALID_ARGUMENT;
    }

    std::shared_ptr<SeedRecord> admittedSeed;
    uint64_t ticket = 0;
    {
        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->lifecycle != Lifecycle::Running) {
            return context->lifecycle == Lifecycle::Created
                       ? MSPV_NOT_RUNNING
                       : MSPV_CLOSED;
        }

        const auto found = context->seeds.find(seedId);
        if (found == context->seeds.end()) {
            return MSPV_SEED_NOT_FOUND;
        }
        if (found->second->state == MSPV_SEED_RELEASING_STATE) {
            return MSPV_SEED_RELEASING;
        }
        if ((found->second->state != MSPV_SEED_READY &&
             found->second->state != MSPV_SEED_CURRENT) ||
            found->second->resources == nullptr) {
            return MSPV_SEED_NOT_READY;
        }
        if (context->pending.size() + context->submissionsPreparing >=
                context->config.pending_capacity ||
            context->outstanding >= context->config.max_outstanding) {
            return MSPV_QUEUE_FULL;
        }
        if (context->bufferedInputBytes >
                context->config.max_buffered_input_bytes ||
            static_cast<uint64_t>(inputSize) >
                context->config.max_buffered_input_bytes -
                    context->bufferedInputBytes) {
            return MSPV_QUEUE_FULL;
        }

        ticket = nextTicketLocked(context);
        admittedSeed = found->second;
        ++context->submissionsPreparing;
        ++context->outstanding;
        context->bufferedInputBytes += static_cast<uint64_t>(inputSize);
    }

    std::shared_ptr<VerifyJob> job;
    try {
        job.reset(new VerifyJob());
        const uint8_t *bytes = static_cast<const uint8_t *>(input);
        job->input.assign(bytes, bytes + inputSize);
        job->ticket = ticket;
        job->userTag = userTag;
        job->seed = admittedSeed;
        job->compare = claimedHash != nullptr;
        if (claimedHash != nullptr) {
            std::memcpy(job->claimedHash, claimedHash, MSPV_HASH_SIZE);
        }
        job->submitted = Clock::now();
    }
    catch (...) {
        job.reset();
        std::lock_guard<std::mutex> lock(context->mutex);
        rollbackSubmissionLocked(context,
                                 static_cast<uint64_t>(inputSize));
        throw;
    }

    {
        std::unique_lock<std::mutex> lock(context->mutex);
        if (context->lifecycle != Lifecycle::Running ||
            admittedSeed->state == MSPV_SEED_RELEASING_STATE) {
            const mspv_status rejection =
                context->lifecycle == Lifecycle::Running
                    ? MSPV_SEED_RELEASING
                    : MSPV_CLOSED;
            lock.unlock();
            job.reset();
            lock.lock();
            rollbackSubmissionLocked(context,
                                     static_cast<uint64_t>(inputSize));
            return rejection;
        }
        try {
            context->pending.push_back(job);
        }
        catch (...) {
            lock.unlock();
            job.reset();
            lock.lock();
            rollbackSubmissionLocked(context,
                                     static_cast<uint64_t>(inputSize));
            throw;
        }
        if (context->submissionsPreparing > 0) {
            --context->submissionsPreparing;
        }
        context->stateCv.notify_all();
        ++admittedSeed->queuedJobs;
        ++context->submitted;
        *outTicket = ticket;
    }

    context->workCv.notify_one();
    logMessage(context,
               MSPV_LOG_TRACE,
               "accepted ticket %llu seed %llu input=%zu tag=%llu",
               static_cast<unsigned long long>(ticket),
               static_cast<unsigned long long>(seedId),
               inputSize,
               static_cast<unsigned long long>(userTag));
    return MSPV_OK;
}

mspv_status shutdownImpl(mspv_context *context, mspv_shutdown_mode mode)
{
    if (context == nullptr ||
        (mode != MSPV_SHUTDOWN_DRAIN &&
         mode != MSPV_SHUTDOWN_CANCEL_PENDING)) {
        return MSPV_INVALID_ARGUMENT;
    }
    if (ownedThreadContext == context) {
        logMessage(context,
                   MSPV_LOG_ERROR,
                   "shutdown cannot run on a verifier-owned thread");
        return MSPV_INVALID_ARGUMENT;
    }

    try {
    uint64_t cancelledNow = 0;
    bool closedBeforeStart = false;
    bool notifyCancelled = false;
    {
        std::unique_lock<std::mutex> lock(context->mutex);
        if (context->lifecycle == Lifecycle::Stopped) {
            context->stateCv.wait(lock, [context]() {
                return context->apiCalls == 0;
            });
            return context->shutdownFailed ? MSPV_INTERNAL_ERROR : MSPV_OK;
        }
        if (context->lifecycle == Lifecycle::Stopping) {
            context->stateCv.wait(lock, [context]() {
                return context->lifecycle == Lifecycle::Stopped &&
                       context->apiCalls == 0;
            });
            return context->shutdownFailed ? MSPV_INTERNAL_ERROR : MSPV_OK;
        }
        if (context->lifecycle == Lifecycle::Created) {
            context->lifecycle = Lifecycle::Stopping;
            closedBeforeStart = true;
        }
        else if (context->lifecycle != Lifecycle::Running &&
                 context->lifecycle != Lifecycle::Fatal) {
            return MSPV_CLOSED;
        }
        else {
            const bool fatalShutdown =
                context->lifecycle == Lifecycle::Fatal ||
                context->shutdownFailed;
            context->lifecycle = Lifecycle::Stopping;
            for (const auto &entry : context->seeds) {
                if (entry.second->state == MSPV_SEED_PREPARING) {
                    entry.second->state = MSPV_SEED_RELEASING_STATE;
                    entry.second->lastError = MSPV_CLOSED;
                    entry.second->cancelPreparation.store(
                        true, std::memory_order_relaxed);
                }
            }

            if (mode == MSPV_SHUTDOWN_CANCEL_PENDING || fatalShutdown) {
                cancelledNow +=
                    cancelPendingLocked(context, notifyCancelled);
            }
            context->stateCv.wait(lock, [context]() {
                return context->submissionsPreparing == 0;
            });
        }
    }

    if (closedBeforeStart) {
        try {
            logMessage(context,
                       MSPV_LOG_INFO,
                       "verifier closed before start");
            {
                std::lock_guard<std::mutex> lock(context->mutex);
                context->shutdownComplete = true;
            }
            const bool failed = markStopped(context, false);
            return !waitForApiCallsToFinish(context) || failed
                       ? MSPV_INTERNAL_ERROR
                       : MSPV_OK;
        }
        catch (...) {
            (void)markStopped(context, true);
            return MSPV_INTERNAL_ERROR;
        }
    }

    try {
        logMessage(context,
                   MSPV_LOG_INFO,
                   "shutdown started: mode=%s cancelled_pending=%llu",
                   mode == MSPV_SHUTDOWN_DRAIN ? "drain" : "cancel",
                   static_cast<unsigned long long>(cancelledNow));
        context->preparationCv.notify_all();
        context->workCv.notify_all();
        context->completionCv.notify_all();
        context->seedCv.notify_all();
        if (notifyCancelled) {
            notifyCaller(context);
        }

        if (context->preparationThread.joinable()) {
            context->preparationThread.join();
        }
        for (std::thread &worker : context->workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }

        bool notifyLateCancellation = false;
        uint64_t submitted = 0;
        uint64_t completed = 0;
        uint64_t cancelled = 0;
        uint64_t failed = 0;
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            cancelledNow +=
                cancelPendingLocked(context, notifyLateCancellation);
            context->preparationQueue.clear();
            context->seedsByKey.clear();
            /* Shutdown is exclusive; this cleanup performs no new allocation. */
            context->seeds.clear();
            context->activeSeed = 0;
            context->shutdownComplete = true;
            submitted = context->submitted;
            completed = context->completed;
            cancelled = context->cancelled;
            failed = context->failed;
        }
        if (notifyLateCancellation) {
            context->completionCv.notify_all();
            notifyCaller(context);
        }

        logMessage(context,
                   MSPV_LOG_INFO,
                   "shutdown complete: submitted=%llu completed=%llu "
                   "cancelled=%llu failed=%llu",
                   static_cast<unsigned long long>(submitted),
                   static_cast<unsigned long long>(completed),
                   static_cast<unsigned long long>(cancelled),
                   static_cast<unsigned long long>(failed));

        const bool stoppedWithFailure = markStopped(context, false);
        return !waitForApiCallsToFinish(context) || stoppedWithFailure
                   ? MSPV_INTERNAL_ERROR
                   : MSPV_OK;
    }
    catch (...) {
        (void)markStopped(context, true);
        (void)waitForApiCallsToFinish(context);
        return MSPV_INTERNAL_ERROR;
    }
    }
    catch (...) {
        (void)markStopped(context, true);
        (void)waitForApiCallsToFinish(context);
        return MSPV_INTERNAL_ERROR;
    }
}

template<typename Function>
mspv_status translateExceptions(Function function) noexcept
{
    try {
        return function();
    }
    catch (const std::bad_alloc &) {
        return MSPV_NO_MEMORY;
    }
    catch (...) {
        return MSPV_INTERNAL_ERROR;
    }
}

} // namespace

extern "C" {

mspv_status mspv_config_init(mspv_config *config)
{
    if (config == nullptr) {
        return MSPV_INVALID_ARGUMENT;
    }

    const unsigned int hardware = std::thread::hardware_concurrency();
    const uint32_t threads = std::max<uint32_t>(
        1, std::min<uint32_t>(4, hardware == 0 ? 1 : hardware));

    mspv_config value{};
    value.struct_size = sizeof(value);
    value.abi_version = MSPV_ABI_VERSION;
    value.worker_count = threads;
    value.seed_init_threads = threads;
    value.pending_capacity = 256;
    value.max_outstanding = 512;
    value.max_input_size = 4096;
    value.max_seed_key_size = 60;
    value.max_seeds = 2;
    value.max_buffered_input_bytes = 16u * 1024u * 1024u;
    value.memory_mode = MSPV_MEMORY_LIGHT;
    value.large_pages = MSPV_LARGE_PAGES_TRY;
    value.options = MSPV_OPTION_SECURE_JIT;
    value.log_level = MSPV_LOG_INFO;
    *config = value;
    return MSPV_OK;
}

mspv_status mspv_create(const mspv_config *config,
                        mspv_context **outContext)
{
    if (outContext != nullptr) {
        *outContext = nullptr;
    }
    if (config == nullptr || outContext == nullptr) {
        return MSPV_INVALID_ARGUMENT;
    }
    if (!validConfig(*config)) {
        return MSPV_INVALID_CONFIG;
    }

    return translateExceptions([&]() {
        *outContext = new mspv_context(*config);
        return MSPV_OK;
    });
}

mspv_status mspv_start(mspv_context *context)
{
    ApiCallGuard call(context);
    return translateExceptions([&]() { return startImpl(context); });
}

mspv_status mspv_seed_prepare(mspv_context *context,
                              const void *key,
                              size_t keySize,
                              mspv_seed_id *outSeedId)
{
    ApiCallGuard call(context);
    if (context == nullptr || key == nullptr || keySize == 0 ||
        outSeedId == nullptr) {
        return MSPV_INVALID_ARGUMENT;
    }
    *outSeedId = 0;
    if (keySize > context->config.max_seed_key_size) {
        return MSPV_INVALID_ARGUMENT;
    }

    return translateExceptions([&]() {
        const std::string keyBytes(static_cast<const char *>(key), keySize);
        mspv_seed_id seedId = 0;
        bool created = false;
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            if (context->lifecycle != Lifecycle::Running) {
                return context->lifecycle == Lifecycle::Created
                           ? MSPV_NOT_RUNNING
                           : MSPV_CLOSED;
            }

            const auto existingKey = context->seedsByKey.find(keyBytes);
            if (existingKey != context->seedsByKey.end()) {
                const auto existingSeed =
                    context->seeds.find(existingKey->second);
                if (existingSeed != context->seeds.end() &&
                    existingSeed->second->state !=
                        MSPV_SEED_RELEASING_STATE) {
                    *outSeedId = existingSeed->second->id;
                    return MSPV_OK;
                }
                context->seedsByKey.erase(existingKey);
            }

            if (context->seeds.size() >= context->config.max_seeds) {
                return MSPV_SEED_CAPACITY;
            }

            std::shared_ptr<SeedRecord> seed(new SeedRecord());
            seed->id = nextSeedIdLocked(context);
            seed->key = keyBytes;
            context->seeds.emplace(seed->id, seed);
            try {
                context->seedsByKey.emplace(keyBytes, seed->id);
                context->preparationQueue.push_back(seed);
            }
            catch (...) {
                context->seedsByKey.erase(keyBytes);
                context->seeds.erase(seed->id);
                throw;
            }
            seedId = seed->id;
            *outSeedId = seedId;
            created = true;
        }

        if (created) {
            logMessage(context,
                       MSPV_LOG_DEBUG,
                       "seed %llu admitted for preparation (key_bytes=%zu)",
                       static_cast<unsigned long long>(seedId),
                       keySize);
            context->preparationCv.notify_one();
        }
        return MSPV_OK;
    });
}

mspv_status mspv_seed_wait_ready(mspv_context *context,
                                 mspv_seed_id seedId,
                                 uint32_t timeoutMs)
{
    ApiCallGuard call(context);
    if (context == nullptr || seedId == 0) {
        return MSPV_INVALID_ARGUMENT;
    }

    return translateExceptions([&]() -> mspv_status {
        std::unique_lock<std::mutex> lock(context->mutex);
        const bool resolved = waitFor(
            context->seedCv, lock, timeoutMs, [context, seedId]() {
                const auto found = context->seeds.find(seedId);
                return found == context->seeds.end() ||
                       found->second->state != MSPV_SEED_PREPARING ||
                       context->lifecycle == Lifecycle::Fatal ||
                       context->lifecycle == Lifecycle::Stopped;
            });
        if (!resolved) {
            return MSPV_TIMEOUT;
        }
        if (context->lifecycle == Lifecycle::Fatal) {
            return MSPV_INTERNAL_ERROR;
        }

        const auto found = context->seeds.find(seedId);
        if (found == context->seeds.end()) {
            return MSPV_SEED_NOT_FOUND;
        }
        if (found->second->state == MSPV_SEED_READY ||
            found->second->state == MSPV_SEED_CURRENT) {
            return MSPV_OK;
        }
        if (found->second->state == MSPV_SEED_FAILED) {
            return static_cast<mspv_status>(found->second->lastError);
        }
        if (found->second->state == MSPV_SEED_RELEASING_STATE) {
            return MSPV_SEED_RELEASING;
        }
        return MSPV_SEED_NOT_READY;
    });
}

mspv_status mspv_seed_get_info(mspv_context *context,
                               mspv_seed_id seedId,
                               mspv_seed_info *outInfo)
{
    ApiCallGuard call(context);
    if (context == nullptr || seedId == 0 || outInfo == nullptr) {
        return MSPV_INVALID_ARGUMENT;
    }

    return translateExceptions([&]() {
        std::lock_guard<std::mutex> lock(context->mutex);
        const auto found = context->seeds.find(seedId);
        if (found == context->seeds.end()) {
            return MSPV_SEED_NOT_FOUND;
        }

        const std::shared_ptr<SeedRecord> &seed = found->second;
        mspv_seed_info info{};
        info.seed_id = seed->id;
        info.state = seed->state;
        info.last_error = seed->lastError;
        info.key_size = static_cast<uint32_t>(seed->key.size());
        info.queued_jobs = seed->queuedJobs;
        info.running_jobs = seed->runningJobs;
        info.prepare_ns = seed->prepareNs;
        if (seed->resources != nullptr) {
            info.memory_uses_large_pages =
                seed->resources->memoryUsesLargePages ? 1u : 0u;
            info.all_vms_use_large_pages =
                seed->resources->allVmsUseLargePages ? 1u : 0u;
        }
        *outInfo = info;
        return MSPV_OK;
    });
}

mspv_status mspv_seed_activate(mspv_context *context,
                               mspv_seed_id seedId)
{
    ApiCallGuard call(context);
    if (context == nullptr || seedId == 0) {
        return MSPV_INVALID_ARGUMENT;
    }

    const mspv_status status = translateExceptions([&]() {
        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->lifecycle != Lifecycle::Running) {
            return context->lifecycle == Lifecycle::Created
                       ? MSPV_NOT_RUNNING
                       : MSPV_CLOSED;
        }
        const auto found = context->seeds.find(seedId);
        if (found == context->seeds.end()) {
            return MSPV_SEED_NOT_FOUND;
        }
        if (found->second->state == MSPV_SEED_RELEASING_STATE) {
            return MSPV_SEED_RELEASING;
        }
        if (found->second->state != MSPV_SEED_READY &&
            found->second->state != MSPV_SEED_CURRENT) {
            return MSPV_SEED_NOT_READY;
        }
        if (context->activeSeed == seedId) {
            return MSPV_OK;
        }

        const auto previous = context->seeds.find(context->activeSeed);
        if (previous != context->seeds.end() &&
            previous->second->state == MSPV_SEED_CURRENT) {
            previous->second->state = MSPV_SEED_READY;
        }
        context->activeSeed = seedId;
        found->second->state = MSPV_SEED_CURRENT;
        context->seedCv.notify_all();
        return MSPV_OK;
    });

    if (status == MSPV_OK) {
        logMessage(context,
                   MSPV_LOG_INFO,
                   "seed %llu activated",
                   static_cast<unsigned long long>(seedId));
    }
    return status;
}

mspv_status mspv_seed_deactivate(mspv_context *context)
{
    ApiCallGuard call(context);
    if (context == nullptr) {
        return MSPV_INVALID_ARGUMENT;
    }

    mspv_seed_id deactivated = 0;
    const mspv_status status = translateExceptions([&]() {
        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->lifecycle != Lifecycle::Running) {
            return context->lifecycle == Lifecycle::Created
                       ? MSPV_NOT_RUNNING
                       : MSPV_CLOSED;
        }
        deactivated = context->activeSeed;
        const auto current = context->seeds.find(context->activeSeed);
        if (current != context->seeds.end() &&
            current->second->state == MSPV_SEED_CURRENT) {
            current->second->state = MSPV_SEED_READY;
        }
        context->activeSeed = 0;
        context->seedCv.notify_all();
        return MSPV_OK;
    });

    if (status == MSPV_OK && deactivated != 0) {
        logMessage(context,
                   MSPV_LOG_INFO,
                   "seed %llu deactivated",
                   static_cast<unsigned long long>(deactivated));
    }
    return status;
}

mspv_status mspv_seed_release(mspv_context *context,
                              mspv_seed_id seedId)
{
    ApiCallGuard call(context);
    if (context == nullptr || seedId == 0) {
        return MSPV_INVALID_ARGUMENT;
    }

    std::shared_ptr<SeedRecord> releasedSeed;
    std::unique_ptr<SeedResources> releasedResources;
    bool shouldFinalizeRelease = false;
    const mspv_status status = translateExceptions([&]() {
        std::lock_guard<std::mutex> lock(context->mutex);
        if (context->lifecycle != Lifecycle::Running) {
            return context->lifecycle == Lifecycle::Created
                       ? MSPV_NOT_RUNNING
                       : MSPV_CLOSED;
        }
        const auto found = context->seeds.find(seedId);
        if (found == context->seeds.end()) {
            return MSPV_SEED_NOT_FOUND;
        }
        const std::shared_ptr<SeedRecord> seed = found->second;
        if (context->activeSeed == seedId ||
            seed->state == MSPV_SEED_CURRENT) {
            return MSPV_SEED_ACTIVE;
        }
        if (seed->state == MSPV_SEED_RELEASING_STATE) {
            return MSPV_OK;
        }

        seed->state = MSPV_SEED_RELEASING_STATE;
        seed->cancelPreparation.store(true, std::memory_order_relaxed);
        const auto byKey = context->seedsByKey.find(seed->key);
        if (byKey != context->seedsByKey.end() &&
            byKey->second == seedId) {
            context->seedsByKey.erase(byKey);
        }
        releasedSeed = seed;
        shouldFinalizeRelease = detachReleasedSeedLocked(context,
                                                         seed,
                                                         releasedResources);
        context->preparationCv.notify_all();
        return MSPV_OK;
    });

    if (status == MSPV_OK && shouldFinalizeRelease) {
        finalizeReleasedSeed(context,
                             releasedSeed,
                             std::move(releasedResources));
    }

    if (status == MSPV_OK) {
        logMessage(context,
                   MSPV_LOG_INFO,
                   "seed %llu release requested",
                   static_cast<unsigned long long>(seedId));
    }
    return status;
}

mspv_status mspv_seed_wait_released(mspv_context *context,
                                    mspv_seed_id seedId,
                                    uint32_t timeoutMs)
{
    ApiCallGuard call(context);
    if (context == nullptr || seedId == 0) {
        return MSPV_INVALID_ARGUMENT;
    }

    return translateExceptions([&]() {
        std::unique_lock<std::mutex> lock(context->mutex);
        const bool released = waitFor(
            context->seedCv, lock, timeoutMs, [context, seedId]() {
                return context->seeds.find(seedId) == context->seeds.end() ||
                       context->lifecycle == Lifecycle::Fatal ||
                       context->lifecycle == Lifecycle::Stopped;
            });
        if (!released) {
            return MSPV_TIMEOUT;
        }
        if (context->lifecycle == Lifecycle::Fatal) {
            return MSPV_INTERNAL_ERROR;
        }
        return context->seeds.find(seedId) == context->seeds.end()
                   ? MSPV_OK
                   : MSPV_CLOSED;
    });
}

mspv_status mspv_hash_submit(mspv_context *context,
                             mspv_seed_id seedId,
                             const void *input,
                             size_t inputSize,
                             uint64_t userTag,
                             uint64_t *outTicket)
{
    ApiCallGuard call(context);
    return translateExceptions([&]() {
        return submitImpl(context,
                          seedId,
                          input,
                          inputSize,
                          nullptr,
                          userTag,
                          outTicket);
    });
}

mspv_status mspv_verify_submit(
    mspv_context *context,
    mspv_seed_id seedId,
    const void *input,
    size_t inputSize,
    const uint8_t claimedHash[MSPV_HASH_SIZE],
    uint64_t userTag,
    uint64_t *outTicket)
{
    ApiCallGuard call(context);
    if (claimedHash == nullptr) {
        return MSPV_INVALID_ARGUMENT;
    }
    return translateExceptions([&]() {
        return submitImpl(context,
                          seedId,
                          input,
                          inputSize,
                          claimedHash,
                          userTag,
                          outTicket);
    });
}

mspv_status mspv_completion_poll(mspv_context *context,
                                 mspv_completion *outCompletion,
                                 uint32_t timeoutMs)
{
    ApiCallGuard call(context);
    if (context == nullptr || outCompletion == nullptr) {
        return MSPV_INVALID_ARGUMENT;
    }

    return translateExceptions([&]() {
        std::unique_lock<std::mutex> lock(context->mutex);
        const bool available = waitFor(
            context->completionCv, lock, timeoutMs, [context]() {
                return context->completionCount != 0 ||
                       context->lifecycle == Lifecycle::Fatal ||
                       context->lifecycle == Lifecycle::Stopped;
            });
        if (!available) {
            return MSPV_TIMEOUT;
        }
        if (context->completionCount == 0) {
            return context->shutdownFailed ? MSPV_INTERNAL_ERROR
                                           : MSPV_CLOSED;
        }

        *outCompletion = context->completionRing[context->completionHead];
        context->completionHead =
            (context->completionHead + 1) % context->completionRing.size();
        --context->completionCount;
        if (context->outstanding == 0) {
            return MSPV_INTERNAL_ERROR;
        }
        --context->outstanding;
        return MSPV_OK;
    });
}

mspv_status mspv_get_stats(mspv_context *context, mspv_stats *outStats)
{
    ApiCallGuard call(context);
    if (context == nullptr || outStats == nullptr) {
        return MSPV_INVALID_ARGUMENT;
    }

    return translateExceptions([&]() {
        std::lock_guard<std::mutex> lock(context->mutex);
        mspv_stats stats{};
        stats.workers = static_cast<uint32_t>(context->workers.size());
        stats.seeds = static_cast<uint32_t>(context->seeds.size());
        for (const auto &entry : context->seeds) {
            if (entry.second->state == MSPV_SEED_PREPARING) {
                ++stats.seeds_preparing;
            }
            else if (entry.second->state == MSPV_SEED_READY ||
                     entry.second->state == MSPV_SEED_CURRENT) {
                ++stats.seeds_ready;
            }
        }
        stats.pending = static_cast<uint32_t>(context->pending.size());
        stats.running = context->runningJobs;
        stats.completions = static_cast<uint32_t>(context->completionCount);
        stats.outstanding = context->outstanding;
        stats.buffered_input_bytes = context->bufferedInputBytes;
        stats.active_seed_id = context->activeSeed;
        stats.submitted = context->submitted;
        stats.completed = context->completed;
        stats.cancelled = context->cancelled;
        stats.failed = context->failed;
        *outStats = stats;
        return MSPV_OK;
    });
}

mspv_status mspv_shutdown(mspv_context *context, mspv_shutdown_mode mode)
{
    return translateExceptions([&]() { return shutdownImpl(context, mode); });
}

void mspv_destroy(mspv_context *context)
{
    if (context == nullptr) {
        return;
    }
    if (ownedThreadContext == context) {
        logMessage(context,
                   MSPV_LOG_ERROR,
                   "destroy ignored on a verifier-owned thread");
        return;
    }
    try {
        (void)shutdownImpl(context, MSPV_SHUTDOWN_CANCEL_PENDING);
        bool safeToDelete = false;
        {
            std::lock_guard<std::mutex> lock(context->mutex);
            safeToDelete = context->lifecycle == Lifecycle::Stopped &&
                           context->shutdownComplete;
        }
        if (safeToDelete) {
            delete context;
        }
    }
    catch (...) {
        /* Destructors cannot report failures through the C ABI. */
    }
}

const char *mspv_status_string(mspv_status status)
{
    switch (status) {
    case MSPV_OK:
        return "ok";
    case MSPV_INVALID_ARGUMENT:
        return "invalid argument";
    case MSPV_INVALID_CONFIG:
        return "invalid configuration";
    case MSPV_NO_MEMORY:
        return "out of memory";
    case MSPV_NOT_RUNNING:
        return "not running";
    case MSPV_ALREADY_RUNNING:
        return "already running";
    case MSPV_CLOSED:
        return "closed";
    case MSPV_SEED_NOT_FOUND:
        return "seed not found";
    case MSPV_SEED_NOT_READY:
        return "seed not ready";
    case MSPV_SEED_RELEASING:
        return "seed releasing";
    case MSPV_SEED_ACTIVE:
        return "seed is current";
    case MSPV_SEED_CAPACITY:
        return "seed capacity reached";
    case MSPV_QUEUE_FULL:
        return "queue or input-byte capacity reached";
    case MSPV_TIMEOUT:
        return "timeout";
    case MSPV_CANCELLED:
        return "cancelled";
    case MSPV_UNSUPPORTED:
        return "unsupported";
    case MSPV_RANDOMX_ERROR:
        return "RandomX error";
    case MSPV_INTERNAL_ERROR:
        return "internal error";
    }
    return "unknown status";
}

} // extern "C"
