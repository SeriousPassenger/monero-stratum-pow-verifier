/* SPDX-License-Identifier: MIT */

#include "monero_stratum_pow_verifier.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

class TestFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require(bool condition, const char *expression, int line)
{
    if (!condition) {
        throw TestFailure("line " + std::to_string(line) + ": " + expression);
    }
}

#define REQUIRE(expression) require((expression), #expression, __LINE__)

void requireStatus(mspv_status actual, mspv_status expected, int line)
{
    if (actual != expected) {
        throw TestFailure("line " + std::to_string(line) + ": expected " +
                          mspv_status_string(expected) + ", got " +
                          mspv_status_string(actual));
    }
}

#define REQUIRE_STATUS(expression, expected) \
    requireStatus((expression), (expected), __LINE__)

std::vector<uint8_t> hex(const std::string &text)
{
    REQUIRE(!text.empty());
    REQUIRE((text.size() % 2) == 0);
    const auto digit = [](char value) -> uint8_t {
        if (value >= '0' && value <= '9') {
            return static_cast<uint8_t>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<uint8_t>(value - 'a' + 10);
        }
        if (value >= 'A' && value <= 'F') {
            return static_cast<uint8_t>(value - 'A' + 10);
        }
        throw TestFailure("invalid hexadecimal fixture");
    };

    std::vector<uint8_t> bytes(text.size() / 2);
    for (size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>(
            (digit(text[index * 2]) << 4) | digit(text[index * 2 + 1]));
    }
    return bytes;
}

struct LogCapture {
    std::mutex mutex;
    std::vector<std::string> messages;
};

void captureLog(void *userData, mspv_log_level, const char *message)
{
    auto *capture = static_cast<LogCapture *>(userData);
    std::lock_guard<std::mutex> lock(capture->mutex);
    capture->messages.emplace_back(message);
}

void notify(void *userData)
{
    static_cast<std::atomic<uint64_t> *>(userData)->fetch_add(
        1, std::memory_order_relaxed);
}

mspv_config testConfig(LogCapture *logs,
                       std::atomic<uint64_t> *notifications,
                       uint32_t workers = 2,
                       uint32_t capacity = 32)
{
    mspv_config config{};
    REQUIRE_STATUS(mspv_config_init(&config), MSPV_OK);
    config.worker_count = workers;
    config.seed_init_threads = workers;
    config.pending_capacity = capacity;
    config.max_outstanding = capacity;
    config.max_input_size = 4096;
    config.max_seed_key_size = 60;
    config.max_buffered_input_bytes = 1024 * 1024;
    config.max_seeds = 2;
    config.memory_mode = MSPV_MEMORY_LIGHT;
    config.large_pages = MSPV_LARGE_PAGES_DISABLED;
    config.log = captureLog;
    config.log_user_data = logs;
    config.log_level = MSPV_LOG_DEBUG;
    config.notify = notify;
    config.notify_user_data = notifications;
    return config;
}

class Context {
public:
    explicit Context(const mspv_config &config)
    {
        REQUIRE_STATUS(mspv_create(&config, &value_), MSPV_OK);
        REQUIRE(value_ != nullptr);
    }

    ~Context() { mspv_destroy(value_); }

    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;

    mspv_context *get() const { return value_; }

private:
    mspv_context *value_ = nullptr;
};

mspv_seed_id prepare(mspv_context *context,
                     const void *key,
                     size_t keySize)
{
    mspv_seed_id seed = 0;
    REQUIRE_STATUS(mspv_seed_prepare(context, key, keySize, &seed), MSPV_OK);
    REQUIRE(seed != 0);
    REQUIRE_STATUS(mspv_seed_wait_ready(context, seed, 120'000), MSPV_OK);
    return seed;
}

mspv_completion poll(mspv_context *context)
{
    mspv_completion completion{};
    REQUIRE_STATUS(mspv_completion_poll(context, &completion, 120'000),
                   MSPV_OK);
    return completion;
}

uint64_t verify(mspv_context *context,
                mspv_seed_id seed,
                const std::vector<uint8_t> &input,
                const std::vector<uint8_t> &claimed,
                uint64_t tag)
{
    REQUIRE(claimed.size() == MSPV_HASH_SIZE);
    uint64_t ticket = 0;
    REQUIRE_STATUS(mspv_verify_submit(context,
                                      seed,
                                      input.data(),
                                      input.size(),
                                      claimed.data(),
                                      tag,
                                      &ticket),
                   MSPV_OK);
    REQUIRE(ticket != 0);
    return ticket;
}

const std::vector<uint8_t> kSimpleKey{
    't', 'e', 's', 't', ' ', 'k', 'e', 'y', ' ', '0', '0', '0'};
const std::vector<uint8_t> kSimpleInput{
    'T', 'h', 'i', 's', ' ', 'i', 's', ' ', 'a', ' ', 't', 'e', 's', 't'};

const std::vector<uint8_t> &simpleExpected()
{
    static const std::vector<uint8_t> value =
        hex("639183aae1bf4c9a35884cb46b09cad9175f04efd7684e7262a0ac1c2f0b4e3f");
    return value;
}

const std::vector<uint8_t> &moneroSeed()
{
    /*
     * Legacy Monero hashing-blob integration fixture from the predecessor
     * verifier's real-engine smoke test. Its result was independently
     * recomputed with the pinned official RandomX v1.2.2 in light and fast
     * modes; it is not presented as a block-height/chain-acceptance fixture.
     */
    static const std::vector<uint8_t> value =
        hex("d432f499205150873b2572b5f033c9c6e4b7c6f3394bd2dd93822cd7085e7307");
    return value;
}

const std::vector<uint8_t> &moneroBlob()
{
    static const std::vector<uint8_t> value = hex(
        "0e0ed286da8006ecdc1aab3033cf1716c52f13f9d8ae0051615a2453643de946"
        "43b550d543becd0000000002abc78b0101ffefc68b0101fcfcf0d4b422025014"
        "bb4a1eade6622fd781cb1063381cad396efa69719b41aa28b4fce8c7ad4b5f01"
        "9ce1dc670456b24a5e03c2d9058a2df10fec779e2579753b1847b74ee644f16b"
        "023c000000000000000000000000000000000000000000000000000000000000"
        "0000000000000000000000000000000000000000000000000000000000000005"
        "1399a1bc46a846474f5b33db24eae173a26393b976054ee14f9feefe999252338"
        "02867097564c9db7a36af5bb5ed33ab46e63092bd8d32cef121608c3258edd555"
        "62812e21cc7e3ac73045745a72f7d74581d9a0849d6f30e8b2923171253e864f"
        "4e9ddea3acb5bc755f1c4a878130a70c26297540bc0b7a57affb6b35c1f03d8d"
        "bd54ece8457531f8cba15bb74516779c01193e212050423020e45aa2c15dcb");
    return value;
}

const std::vector<uint8_t> &moneroExpected()
{
    static const std::vector<uint8_t> value =
        hex("d0402d6834e26fb94a9ce38c6424d27d2069896a9b8b1ce685d79936bca6e0a8");
    return value;
}

void testConfigurationValidation()
{
    REQUIRE_STATUS(mspv_config_init(nullptr), MSPV_INVALID_ARGUMENT);
    mspv_config config{};
    REQUIRE_STATUS(mspv_config_init(&config), MSPV_OK);
    REQUIRE(config.memory_mode == MSPV_MEMORY_LIGHT);
    REQUIRE(config.max_seeds == 2);

    mspv_context *invalid = nullptr;
    config.worker_count = 0;
    REQUIRE_STATUS(mspv_create(&config, &invalid), MSPV_INVALID_CONFIG);
    REQUIRE(invalid == nullptr);

    REQUIRE_STATUS(mspv_config_init(&config), MSPV_OK);
    config.options = MSPV_OPTION_DISABLE_JIT | MSPV_OPTION_SECURE_JIT;
    REQUIRE_STATUS(mspv_create(&config, &invalid), MSPV_INVALID_CONFIG);
    REQUIRE(invalid == nullptr);

    REQUIRE_STATUS(mspv_config_init(&config), MSPV_OK);
    config.max_seed_key_size = 61;
    REQUIRE_STATUS(mspv_create(&config, &invalid), MSPV_INVALID_CONFIG);
    REQUIRE(invalid == nullptr);

    REQUIRE_STATUS(mspv_config_init(&config), MSPV_OK);
    config.max_buffered_input_bytes = config.max_input_size - 1u;
    REQUIRE_STATUS(mspv_create(&config, &invalid), MSPV_INVALID_CONFIG);
    REQUIRE(invalid == nullptr);
}

void testKnownAnswersRotationConcurrencyAndBounds()
{
    LogCapture logs;
    std::atomic<uint64_t> notifications{0};
    const mspv_config config = testConfig(&logs, &notifications);
    Context owner(config);
    mspv_context *context = owner.get();

    mspv_seed_id beforeStart = 0;
    REQUIRE_STATUS(mspv_seed_prepare(context,
                                     kSimpleKey.data(),
                                     kSimpleKey.size(),
                                     &beforeStart),
                   MSPV_NOT_RUNNING);
    REQUIRE_STATUS(mspv_start(context), MSPV_OK);
    REQUIRE_STATUS(mspv_start(context), MSPV_ALREADY_RUNNING);

    const mspv_seed_id seedA =
        prepare(context, kSimpleKey.data(), kSimpleKey.size());
    mspv_seed_id duplicate = 0;
    REQUIRE_STATUS(mspv_seed_prepare(context,
                                     kSimpleKey.data(),
                                     kSimpleKey.size(),
                                     &duplicate),
                   MSPV_OK);
    REQUIRE(duplicate == seedA);
    REQUIRE_STATUS(mspv_seed_activate(context, seedA), MSPV_OK);
    REQUIRE_STATUS(mspv_seed_release(context, seedA), MSPV_SEED_ACTIVE);

    mspv_seed_info info{};
    REQUIRE_STATUS(mspv_seed_get_info(context, seedA, &info), MSPV_OK);
    REQUIRE(info.state == MSPV_SEED_CURRENT);
    REQUIRE(info.key_size == kSimpleKey.size());
    REQUIRE(info.prepare_ns > 0);

    std::vector<uint8_t> copiedInput = kSimpleInput;
    std::vector<uint8_t> copiedClaimed = simpleExpected();
    const uint64_t copiedTicket = verify(
        context, seedA, copiedInput, copiedClaimed, 1001);
    std::fill(copiedInput.begin(), copiedInput.end(), 0);
    std::fill(copiedClaimed.begin(), copiedClaimed.end(), 0);

    std::vector<uint8_t> wrong(MSPV_HASH_SIZE, 0);
    const uint64_t mismatchTicket = verify(
        context, seedA, kSimpleInput, wrong, 1002);
    uint64_t hashTicket = 0;
    REQUIRE_STATUS(mspv_hash_submit(context,
                                    seedA,
                                    kSimpleInput.data(),
                                    kSimpleInput.size(),
                                    1003,
                                    &hashTicket),
                   MSPV_OK);
    uint64_t zeroTicket = 0;
    REQUIRE_STATUS(mspv_hash_submit(context,
                                    seedA,
                                    kSimpleInput.data(),
                                    0,
                                    0,
                                    &zeroTicket),
                   MSPV_INVALID_ARGUMENT);

    std::unordered_map<uint64_t, mspv_completion> first;
    for (int index = 0; index < 3; ++index) {
        mspv_completion completion = poll(context);
        first.emplace(completion.ticket, completion);
    }
    REQUIRE(first.at(copiedTicket).comparison == MSPV_COMPARISON_MATCH);
    REQUIRE(first.at(mismatchTicket).comparison == MSPV_COMPARISON_MISMATCH);
    REQUIRE(first.at(hashTicket).comparison == MSPV_COMPARISON_NOT_REQUESTED);
    for (const auto &entry : first) {
        REQUIRE(entry.second.result == MSPV_RESULT_OK);
        REQUIRE(std::memcmp(entry.second.hash,
                            simpleExpected().data(),
                            MSPV_HASH_SIZE) == 0);
    }

    const mspv_seed_id seedB =
        prepare(context, moneroSeed().data(), moneroSeed().size());
    const std::vector<uint8_t> extraKey{'t', 'h', 'i', 'r', 'd'};
    mspv_seed_id extraSeed = 0;
    REQUIRE_STATUS(mspv_seed_prepare(context,
                                     extraKey.data(),
                                     extraKey.size(),
                                     &extraSeed),
                   MSPV_SEED_CAPACITY);
    REQUIRE_STATUS(mspv_seed_activate(context, seedB), MSPV_OK);

    verify(context, seedA, kSimpleInput, simpleExpected(), 2001);
    verify(context, seedB, moneroBlob(), moneroExpected(), 2002);

    constexpr uint32_t releaseJobs = 16;
    for (uint32_t index = 0; index < releaseJobs; ++index) {
        verify(context,
               seedA,
               kSimpleInput,
               simpleExpected(),
               3000 + index);
    }
    REQUIRE_STATUS(mspv_seed_release(context, seedA), MSPV_OK);
    const mspv_status rejected = mspv_verify_submit(context,
                                                    seedA,
                                                    kSimpleInput.data(),
                                                    kSimpleInput.size(),
                                                    simpleExpected().data(),
                                                    3999,
                                                    &zeroTicket);
    REQUIRE(rejected == MSPV_SEED_RELEASING ||
            rejected == MSPV_SEED_NOT_FOUND);

    for (uint32_t index = 0; index < releaseJobs + 2; ++index) {
        const mspv_completion completion = poll(context);
        REQUIRE(completion.result == MSPV_RESULT_OK);
        REQUIRE(completion.comparison == MSPV_COMPARISON_MATCH);
    }
    REQUIRE_STATUS(mspv_seed_wait_released(context, seedA, 120'000), MSPV_OK);

    std::mutex acceptedMutex;
    std::vector<uint64_t> accepted;
    std::atomic<uint32_t> producerFailures{0};
    std::vector<std::thread> producers;
    for (uint32_t producer = 0; producer < 4; ++producer) {
        producers.emplace_back([&, producer]() {
            for (uint32_t index = 0; index < 8; ++index) {
                uint64_t ticket = 0;
                const mspv_status status = mspv_verify_submit(
                    context,
                    seedB,
                    moneroBlob().data(),
                    moneroBlob().size(),
                    moneroExpected().data(),
                    4000 + producer * 8 + index,
                    &ticket);
                if (status != MSPV_OK) {
                    producerFailures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                std::lock_guard<std::mutex> lock(acceptedMutex);
                accepted.push_back(ticket);
            }
        });
    }
    for (std::thread &producer : producers) {
        producer.join();
    }
    REQUIRE(producerFailures.load(std::memory_order_relaxed) == 0);
    REQUIRE(accepted.size() == 32);
    REQUIRE(std::unordered_set<uint64_t>(accepted.begin(), accepted.end()).size()
            == accepted.size());

    uint64_t overflow = 0;
    REQUIRE_STATUS(mspv_verify_submit(context,
                                      seedB,
                                      moneroBlob().data(),
                                      moneroBlob().size(),
                                      moneroExpected().data(),
                                      5000,
                                      &overflow),
                   MSPV_QUEUE_FULL);
    REQUIRE(poll(context).comparison == MSPV_COMPARISON_MATCH);
    REQUIRE_STATUS(mspv_verify_submit(context,
                                      seedB,
                                      moneroBlob().data(),
                                      moneroBlob().size(),
                                      moneroExpected().data(),
                                      5001,
                                      &overflow),
                   MSPV_OK);
    for (uint32_t index = 0; index < 32; ++index) {
        REQUIRE(poll(context).comparison == MSPV_COMPARISON_MATCH);
    }

    mspv_stats stats{};
    REQUIRE_STATUS(mspv_get_stats(context, &stats), MSPV_OK);
    REQUIRE(stats.outstanding == 0);
    REQUIRE(stats.pending == 0);
    REQUIRE(stats.running == 0);
    REQUIRE(stats.buffered_input_bytes == 0);
    REQUIRE(stats.active_seed_id == seedB);
    REQUIRE(stats.failed == 0);
    REQUIRE_STATUS(mspv_shutdown(context, MSPV_SHUTDOWN_DRAIN), MSPV_OK);
    mspv_completion closed{};
    REQUIRE_STATUS(mspv_completion_poll(context, &closed, 0), MSPV_CLOSED);
    REQUIRE(notifications.load(std::memory_order_relaxed) > 0);

    std::lock_guard<std::mutex> lock(logs.mutex);
    const auto contains = [&](const char *needle) {
        return std::any_of(logs.messages.begin(),
                           logs.messages.end(),
                           [&](const std::string &message) {
                               return message.find(needle) != std::string::npos;
                           });
    };
    REQUIRE(contains("verifier started"));
    REQUIRE(contains("preparation started"));
    REQUIRE(contains("ready after"));
    REQUIRE(contains("activated"));
    REQUIRE(contains("release requested"));
    REQUIRE(contains("shutdown complete"));
}

void testCancelPendingAndReleaseDuringPreparation()
{
    LogCapture logs;
    std::atomic<uint64_t> notifications{0};
    mspv_config config = testConfig(&logs, &notifications, 1, 64);
    Context owner(config);
    mspv_context *context = owner.get();
    REQUIRE_STATUS(mspv_start(context), MSPV_OK);

    const std::vector<uint8_t> cancelledKey{'c', 'a', 'n', 'c', 'e', 'l'};
    mspv_seed_id cancelledSeed = 0;
    REQUIRE_STATUS(mspv_seed_prepare(context,
                                     cancelledKey.data(),
                                     cancelledKey.size(),
                                     &cancelledSeed),
                   MSPV_OK);
    REQUIRE_STATUS(mspv_seed_release(context, cancelledSeed), MSPV_OK);
    REQUIRE_STATUS(mspv_seed_wait_released(context, cancelledSeed, 120'000),
                   MSPV_OK);

    const mspv_seed_id seed =
        prepare(context, kSimpleKey.data(), kSimpleKey.size());
    for (uint32_t index = 0; index < 64; ++index) {
        verify(context,
               seed,
               kSimpleInput,
               simpleExpected(),
               6000 + index);
    }

    bool observedPending = false;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        mspv_stats stats{};
        REQUIRE_STATUS(mspv_get_stats(context, &stats), MSPV_OK);
        if (stats.running == 1 && stats.pending > 0) {
            observedPending = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(observedPending);
    REQUIRE_STATUS(mspv_shutdown(context, MSPV_SHUTDOWN_CANCEL_PENDING),
                   MSPV_OK);

    uint32_t successful = 0;
    uint32_t cancelled = 0;
    for (;;) {
        mspv_completion completion{};
        const mspv_status status =
            mspv_completion_poll(context, &completion, 0);
        if (status == MSPV_CLOSED) {
            break;
        }
        REQUIRE_STATUS(status, MSPV_OK);
        if (completion.result == MSPV_RESULT_OK) {
            ++successful;
        }
        else if (completion.result == MSPV_RESULT_CANCELLED) {
            ++cancelled;
        }
    }
    REQUIRE(successful >= 1);
    REQUIRE(cancelled >= 1);
    REQUIRE(successful + cancelled == 64);
}

void testConcurrentProducersAndPollers()
{
    LogCapture logs;
    std::atomic<uint64_t> notifications{0};
    mspv_config config = testConfig(&logs, &notifications, 4, 64);
    config.max_seeds = 1;
    Context owner(config);
    mspv_context *context = owner.get();
    REQUIRE_STATUS(mspv_start(context), MSPV_OK);
    const mspv_seed_id seed =
        prepare(context, kSimpleKey.data(), kSimpleKey.size());

    constexpr uint32_t producerCount = 4;
    constexpr uint32_t pollerCount = 4;
    constexpr uint32_t jobsPerProducer = 64;
    constexpr uint32_t totalJobs = producerCount * jobsPerProducer;
    std::atomic<bool> begin{false};
    std::atomic<uint32_t> completionsPolled{0};
    std::atomic<uint32_t> producersFinished{0};
    std::atomic<uint32_t> producerFailures{0};
    std::atomic<uint32_t> pollerFailures{0};
    std::mutex ticketsMutex;
    std::vector<uint64_t> tickets;
    tickets.reserve(totalJobs);

    std::vector<std::thread> pollers;
    for (uint32_t pollerIndex = 0; pollerIndex < pollerCount; ++pollerIndex) {
        pollers.emplace_back([&]() {
            while (!begin.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (;;) {
                if (completionsPolled.load(std::memory_order_acquire) >=
                    totalJobs) {
                    return;
                }
                mspv_completion completion{};
                const mspv_status status =
                    mspv_completion_poll(context, &completion, 10'000);
                if (status == MSPV_TIMEOUT) {
                    if (completionsPolled.load(std::memory_order_acquire) >=
                        totalJobs) {
                        return;
                    }
                    if (producersFinished.load(std::memory_order_acquire) ==
                        producerCount) {
                        pollerFailures.fetch_add(
                            1, std::memory_order_relaxed);
                        return;
                    }
                    continue;
                }
                if (status != MSPV_OK) {
                    pollerFailures.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                const bool valid =
                    completion.result == MSPV_RESULT_OK &&
                    completion.comparison == MSPV_COMPARISON_MATCH &&
                    std::memcmp(completion.hash,
                                simpleExpected().data(),
                                MSPV_HASH_SIZE) == 0;
                if (!valid) {
                    pollerFailures.fetch_add(1, std::memory_order_relaxed);
                }
                else {
                    std::lock_guard<std::mutex> lock(ticketsMutex);
                    tickets.push_back(completion.ticket);
                }
                completionsPolled.fetch_add(1, std::memory_order_release);
            }
        });
    }

    std::vector<std::thread> producers;
    for (uint32_t producerIndex = 0;
         producerIndex < producerCount;
         ++producerIndex) {
        producers.emplace_back([&, producerIndex]() {
            while (!begin.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            bool failed = false;
            for (uint32_t index = 0;
                 index < jobsPerProducer && !failed;
                 ++index) {
                const uint64_t tag =
                    static_cast<uint64_t>(producerIndex) * jobsPerProducer +
                    index;
                for (;;) {
                    uint64_t ticket = 0;
                    const mspv_status status = mspv_verify_submit(
                        context,
                        seed,
                        kSimpleInput.data(),
                        kSimpleInput.size(),
                        simpleExpected().data(),
                        tag,
                        &ticket);
                    if (status == MSPV_OK) {
                        break;
                    }
                    if (status != MSPV_QUEUE_FULL) {
                        producerFailures.fetch_add(
                            1, std::memory_order_relaxed);
                        failed = true;
                        break;
                    }
                    std::this_thread::yield();
                }
            }
            producersFinished.fetch_add(1, std::memory_order_release);
        });
    }

    begin.store(true, std::memory_order_release);
    for (std::thread &producer : producers) {
        producer.join();
    }
    for (std::thread &poller : pollers) {
        poller.join();
    }

    REQUIRE(producerFailures.load(std::memory_order_relaxed) == 0);
    REQUIRE(pollerFailures.load(std::memory_order_relaxed) == 0);
    REQUIRE(tickets.size() == totalJobs);
    REQUIRE(std::unordered_set<uint64_t>(tickets.begin(), tickets.end()).size()
            == tickets.size());
    mspv_stats stats{};
    REQUIRE_STATUS(mspv_get_stats(context, &stats), MSPV_OK);
    REQUIRE(stats.pending == 0);
    REQUIRE(stats.running == 0);
    REQUIRE(stats.completions == 0);
    REQUIRE(stats.outstanding == 0);
    REQUIRE(stats.buffered_input_bytes == 0);
    REQUIRE_STATUS(mspv_shutdown(context, MSPV_SHUTDOWN_DRAIN), MSPV_OK);
}

void testBufferedByteLimit()
{
    LogCapture logs;
    std::atomic<uint64_t> notifications{0};
    mspv_config config = testConfig(&logs, &notifications, 1, 4);
    constexpr size_t inputBytes = 8u * 1024u * 1024u;
    config.max_input_size = static_cast<uint32_t>(inputBytes);
    config.max_buffered_input_bytes = inputBytes;
    config.max_seeds = 1;
    Context owner(config);
    mspv_context *context = owner.get();
    REQUIRE_STATUS(mspv_start(context), MSPV_OK);
    const mspv_seed_id seed =
        prepare(context, kSimpleKey.data(), kSimpleKey.size());

    std::vector<uint8_t> largeInput(inputBytes, 0x5a);
    uint64_t firstTicket = 0;
    REQUIRE_STATUS(mspv_hash_submit(context,
                                    seed,
                                    largeInput.data(),
                                    largeInput.size(),
                                    9001,
                                    &firstTicket),
                   MSPV_OK);
    uint64_t rejectedTicket = 0;
    REQUIRE_STATUS(mspv_hash_submit(context,
                                    seed,
                                    largeInput.data(),
                                    largeInput.size(),
                                    9002,
                                    &rejectedTicket),
                   MSPV_QUEUE_FULL);
    REQUIRE(rejectedTicket == 0);
    const mspv_completion first = poll(context);
    REQUIRE(first.ticket == firstTicket);
    REQUIRE(first.result == MSPV_RESULT_OK);

    mspv_stats stats{};
    REQUIRE_STATUS(mspv_get_stats(context, &stats), MSPV_OK);
    REQUIRE(stats.buffered_input_bytes == 0);
    REQUIRE(stats.outstanding == 0);

    verify(context, seed, kSimpleInput, simpleExpected(), 9003);
    REQUIRE(poll(context).comparison == MSPV_COMPARISON_MATCH);
    REQUIRE_STATUS(mspv_shutdown(context, MSPV_SHUTDOWN_DRAIN), MSPV_OK);
}

void testDrainAndConcurrentShutdown()
{
    LogCapture logs;
    std::atomic<uint64_t> notifications{0};
    mspv_config config = testConfig(&logs, &notifications, 1, 64);
    config.max_seeds = 1;
    Context owner(config);
    mspv_context *context = owner.get();
    REQUIRE_STATUS(mspv_start(context), MSPV_OK);
    const mspv_seed_id seed =
        prepare(context, kSimpleKey.data(), kSimpleKey.size());

    constexpr uint32_t jobCount = 64;
    for (uint32_t index = 0; index < jobCount; ++index) {
        verify(context,
               seed,
               kSimpleInput,
               simpleExpected(),
               10'000 + index);
    }

    std::atomic<bool> begin{false};
    std::array<mspv_status, 2> statuses{MSPV_INTERNAL_ERROR,
                                        MSPV_INTERNAL_ERROR};
    std::thread first([&]() {
        while (!begin.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        statuses[0] = mspv_shutdown(context, MSPV_SHUTDOWN_DRAIN);
    });
    std::thread second([&]() {
        while (!begin.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        statuses[1] = mspv_shutdown(context, MSPV_SHUTDOWN_DRAIN);
    });
    begin.store(true, std::memory_order_release);
    first.join();
    second.join();
    REQUIRE(statuses[0] == MSPV_OK);
    REQUIRE(statuses[1] == MSPV_OK);

    for (uint32_t index = 0; index < jobCount; ++index) {
        const mspv_completion completion = poll(context);
        REQUIRE(completion.result == MSPV_RESULT_OK);
        REQUIRE(completion.comparison == MSPV_COMPARISON_MATCH);
    }
    mspv_completion closed{};
    REQUIRE_STATUS(mspv_completion_poll(context, &closed, 0), MSPV_CLOSED);
}

struct BlockingLog {
    std::mutex mutex;
    std::condition_variable condition;
    bool entered = false;
    bool release = false;
    std::atomic<bool> shutdownReturned{false};
    std::atomic<uint32_t> callsAfterShutdown{0};
};

void blockingLog(void *userData, mspv_log_level, const char *message)
{
    auto *state = static_cast<BlockingLog *>(userData);
    if (state->shutdownReturned.load(std::memory_order_acquire)) {
        state->callsAfterShutdown.fetch_add(1, std::memory_order_relaxed);
    }
    if (std::string(message).find("deactivated") == std::string::npos) {
        return;
    }
    std::unique_lock<std::mutex> lock(state->mutex);
    state->entered = true;
    state->condition.notify_all();
    state->condition.wait(lock, [state]() { return state->release; });
}

void testShutdownWaitsForConcurrentApiCallbacks()
{
    BlockingLog blocking;
    mspv_config config{};
    REQUIRE_STATUS(mspv_config_init(&config), MSPV_OK);
    config.worker_count = 1;
    config.seed_init_threads = 1;
    config.max_seeds = 1;
    config.large_pages = MSPV_LARGE_PAGES_DISABLED;
    config.log = blockingLog;
    config.log_user_data = &blocking;
    config.log_level = MSPV_LOG_INFO;
    Context owner(config);
    mspv_context *context = owner.get();
    REQUIRE_STATUS(mspv_start(context), MSPV_OK);
    const mspv_seed_id seed =
        prepare(context, kSimpleKey.data(), kSimpleKey.size());
    REQUIRE_STATUS(mspv_seed_activate(context, seed), MSPV_OK);

    mspv_status deactivateStatus = MSPV_INTERNAL_ERROR;
    std::thread deactivate([&]() {
        deactivateStatus = mspv_seed_deactivate(context);
    });
    {
        std::unique_lock<std::mutex> lock(blocking.mutex);
        REQUIRE(blocking.condition.wait_for(
            lock,
            std::chrono::seconds(10),
            [&blocking]() { return blocking.entered; }));
    }

    std::atomic<bool> shutdownDone{false};
    mspv_status shutdownStatus = MSPV_INTERNAL_ERROR;
    std::thread shutdown([&]() {
        shutdownStatus = mspv_shutdown(context, MSPV_SHUTDOWN_DRAIN);
        shutdownDone.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE(!shutdownDone.load(std::memory_order_acquire));
    {
        std::lock_guard<std::mutex> lock(blocking.mutex);
        blocking.release = true;
    }
    blocking.condition.notify_all();
    deactivate.join();
    shutdown.join();
    REQUIRE(deactivateStatus == MSPV_OK);
    REQUIRE(shutdownStatus == MSPV_OK);

    blocking.shutdownReturned.store(true, std::memory_order_release);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE(blocking.callsAfterShutdown.load(std::memory_order_relaxed) == 0);
}

} // namespace

int main()
{
    try {
        testConfigurationValidation();
        testKnownAnswersRotationConcurrencyAndBounds();
        testCancelPendingAndReleaseDuringPreparation();
        testConcurrentProducersAndPollers();
        testBufferedByteLimit();
        testDrainAndConcurrentShutdown();
        testShutdownWaitsForConcurrentApiCallbacks();
        std::cout << "all light-mode verifier tests passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "verifier test failed: " << error.what() << '\n';
        return 1;
    }
}
