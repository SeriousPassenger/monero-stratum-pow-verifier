/*
 * Public-API benchmark for monero-stratum-pow-verifier.
 * SPDX-License-Identifier: MIT
 */

#include "monero_stratum_pow_verifier.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t kMaxLatencySamples = 1'000'000;
constexpr size_t kBenchmarkInputSize = 80;

struct Options {
    mspv_memory_mode mode = MSPV_MEMORY_LIGHT;
    mspv_large_page_mode largePages = MSPV_LARGE_PAGES_TRY;
    uint32_t workers = 0;
    uint32_t initThreads = 0;
    uint32_t inflight = 0;
    uint32_t warmupSeconds = 2;
    uint32_t seconds = 15;
    uint32_t timeoutSeconds = 300;
    uint32_t sampleEvery = 1;
    uint32_t options = MSPV_OPTION_SECURE_JIT;
    bool verbose = false;
};

struct Latencies {
    std::vector<uint64_t> queue;
    std::vector<uint64_t> hash;
    std::vector<uint64_t> total;
};

struct PhaseResult {
    uint64_t completed = 0;
    uint64_t failed = 0;
    uint64_t unexpectedComparison = 0;
    double elapsedSeconds = 0.0;
    Latencies latencies;
};

class Context {
public:
    ~Context() { mspv_destroy(value); }
    Context() = default;
    Context(const Context &) = delete;
    Context &operator=(const Context &) = delete;
    mspv_context *value = nullptr;
};

uint32_t parseU32(const std::string &text,
                  const char *name,
                  uint32_t minimum,
                  uint32_t maximum)
{
    try {
        size_t consumed = 0;
        const unsigned long long parsed = std::stoull(text, &consumed, 10);
        if (consumed != text.size() || parsed < minimum || parsed > maximum) {
            throw std::runtime_error("range");
        }
        return static_cast<uint32_t>(parsed);
    }
    catch (...) {
        throw std::runtime_error(std::string(name) + " must be an integer in " +
                                 std::to_string(minimum) + ".." +
                                 std::to_string(maximum));
    }
}

void usage(const char *program)
{
    std::cout
        << "usage: " << program << " [options]\n\n"
        << "  --mode light|fast       RandomX memory mode (default: light)\n"
        << "  --workers N             Hashing workers (default: library default)\n"
        << "  --init-threads N         FAST dataset builders (default: workers)\n"
        << "  --inflight N             Bounded jobs in flight (default: 4*workers)\n"
        << "  --seconds N              Timed duration, 1..600 (default: 15)\n"
        << "  --warmup N               Warm-up duration, 0..60 (default: 2)\n"
        << "  --timeout N              No-progress timeout, 1..3600 s (default: 300)\n"
        << "  --sample-every N         Record one latency per N completions\n"
        << "  --large-pages off|try|required (default: try)\n"
        << "  --jit off|on|secure      JIT policy (default: secure)\n"
        << "  --software-aes           Disable detected hardware AES\n"
        << "  --verbose                Show library INFO diagnostics\n"
        << "  --help                   Show this help\n\n"
        << "Memory per resident seed (W = workers):\n"
        << "  light: about 256 + 2*W MiB\n"
        << "  fast:  about 2080 + 2*W MiB; about 256 MiB more while preparing\n";
}

Options parseOptions(int argc, char **argv)
{
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        const auto value = [&](const char *name) -> std::string {
            if (++index >= argc) {
                throw std::runtime_error(std::string(name) + " needs a value");
            }
            return argv[index];
        };

        if (argument == "--help") {
            usage(argv[0]);
            std::exit(0);
        }
        if (argument == "--mode") {
            const std::string mode = value("--mode");
            if (mode == "light") {
                options.mode = MSPV_MEMORY_LIGHT;
            }
            else if (mode == "fast") {
                options.mode = MSPV_MEMORY_FAST;
            }
            else {
                throw std::runtime_error("--mode must be light or fast");
            }
        }
        else if (argument == "--workers") {
            options.workers = parseU32(value("--workers"), "workers", 1, 256);
        }
        else if (argument == "--init-threads") {
            options.initThreads =
                parseU32(value("--init-threads"), "init threads", 1, 256);
        }
        else if (argument == "--inflight") {
            options.inflight =
                parseU32(value("--inflight"), "inflight", 1, 1'000'000);
        }
        else if (argument == "--seconds") {
            options.seconds =
                parseU32(value("--seconds"), "seconds", 1, 600);
        }
        else if (argument == "--warmup") {
            options.warmupSeconds =
                parseU32(value("--warmup"), "warmup", 0, 60);
        }
        else if (argument == "--timeout") {
            options.timeoutSeconds =
                parseU32(value("--timeout"), "timeout", 1, 3600);
        }
        else if (argument == "--sample-every") {
            options.sampleEvery = parseU32(
                value("--sample-every"), "sample interval", 1, 1'000'000);
        }
        else if (argument == "--large-pages") {
            const std::string mode = value("--large-pages");
            if (mode == "off") {
                options.largePages = MSPV_LARGE_PAGES_DISABLED;
            }
            else if (mode == "try") {
                options.largePages = MSPV_LARGE_PAGES_TRY;
            }
            else if (mode == "required") {
                options.largePages = MSPV_LARGE_PAGES_REQUIRE;
            }
            else {
                throw std::runtime_error(
                    "--large-pages must be off, try, or required");
            }
        }
        else if (argument == "--jit") {
            const std::string mode = value("--jit");
            const uint32_t jitMask = MSPV_OPTION_DISABLE_JIT |
                                     MSPV_OPTION_SECURE_JIT;
            options.options &= static_cast<uint32_t>(~jitMask);
            if (mode == "off") {
                options.options |= MSPV_OPTION_DISABLE_JIT;
            }
            else if (mode == "secure") {
                options.options |= MSPV_OPTION_SECURE_JIT;
            }
            else if (mode != "on") {
                throw std::runtime_error("--jit must be off, on, or secure");
            }
        }
        else if (argument == "--software-aes") {
            options.options |= MSPV_OPTION_DISABLE_HARD_AES;
        }
        else if (argument == "--verbose") {
            options.verbose = true;
        }
        else {
            throw std::runtime_error("unknown option: " + argument);
        }
    }
    return options;
}

void logMessage(void *, mspv_log_level level, const char *message)
{
    const char *name = level == MSPV_LOG_ERROR
                           ? "error"
                           : level == MSPV_LOG_WARNING ? "warning" : "info";
    std::cerr << "[mspv:" << name << "] " << message << '\n';
}

const char *jitPolicy(uint32_t options)
{
    if ((options & MSPV_OPTION_DISABLE_JIT) != 0) {
        return "off";
    }
    return (options & MSPV_OPTION_SECURE_JIT) != 0 ? "secure" : "on";
}

const char *largePagePolicy(mspv_large_page_mode mode)
{
    if (mode == MSPV_LARGE_PAGES_REQUIRE) {
        return "required";
    }
    return mode == MSPV_LARGE_PAGES_TRY ? "try" : "off";
}

void check(mspv_status status, const char *operation)
{
    if (status != MSPV_OK) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 mspv_status_string(status));
    }
}

std::array<uint8_t, kBenchmarkInputSize> benchmarkInput(uint64_t nonce)
{
    std::array<uint8_t, kBenchmarkInputSize> input{};
    constexpr char prefix[] = "mspv RandomX verifier benchmark";
    static_assert(sizeof(prefix) < kBenchmarkInputSize,
                  "benchmark prefix must leave nonce space");
    std::copy(prefix, prefix + sizeof(prefix) - 1, input.begin());
    for (size_t index = 0; index < sizeof(nonce); ++index) {
        input[input.size() - sizeof(nonce) + index] =
            static_cast<uint8_t>(nonce >> (index * 8u));
    }
    return input;
}

void submitOne(mspv_context *context,
               mspv_seed_id seed,
               uint64_t tag)
{
    const std::array<uint8_t, kBenchmarkInputSize> input =
        benchmarkInput(tag);
    const std::array<uint8_t, MSPV_HASH_SIZE> deliberatelyWrongHash{};
    uint64_t ticket = 0;
    check(mspv_verify_submit(context,
                             seed,
                             input.data(),
                             input.size(),
                             deliberatelyWrongHash.data(),
                             tag,
                             &ticket),
          "verification submission");
}

PhaseResult runPhase(mspv_context *context,
                     mspv_seed_id seed,
                     uint32_t inflight,
                     uint32_t seconds,
                     uint32_t timeoutMs,
                     uint32_t sampleEvery,
                     uint64_t &nextTag,
                     bool collect)
{
    PhaseResult result;
    if (seconds == 0) {
        return result;
    }
    if (collect) {
        const size_t reserve = std::min<size_t>(
            kMaxLatencySamples,
            static_cast<size_t>(seconds) * 4096u / sampleEvery + 1u);
        result.latencies.queue.reserve(reserve);
        result.latencies.hash.reserve(reserve);
        result.latencies.total.reserve(reserve);
    }

    const Clock::time_point begin = Clock::now();
    const Clock::time_point deadline = begin + std::chrono::seconds(seconds);
    uint32_t active = 0;
    while (active < inflight && Clock::now() < deadline) {
        submitOne(context, seed, nextTag++);
        ++active;
    }

    while (active != 0) {
        mspv_completion completion{};
        const mspv_status pollStatus =
            mspv_completion_poll(context, &completion, timeoutMs);
        if (pollStatus == MSPV_TIMEOUT) {
            mspv_stats stats{};
            const mspv_status statsStatus = mspv_get_stats(context, &stats);
            if (statsStatus == MSPV_OK) {
                throw std::runtime_error(
                    "no completion before watchdog timeout (pending=" +
                    std::to_string(stats.pending) + ", running=" +
                    std::to_string(stats.running) + ", completed_queue=" +
                    std::to_string(stats.completions) + ")");
            }
        }
        check(pollStatus, "completion poll");
        --active;

        const Clock::time_point now = Clock::now();
        const bool insideTimedWindow = now <= deadline;
        if (completion.result != MSPV_RESULT_OK) {
            ++result.failed;
        }
        else if (completion.comparison != MSPV_COMPARISON_MISMATCH) {
            ++result.unexpectedComparison;
        }
        else if (insideTimedWindow) {
            ++result.completed;
            if (collect && (result.completed % sampleEvery) == 0 &&
                result.latencies.hash.size() < kMaxLatencySamples) {
                result.latencies.queue.push_back(completion.queue_ns);
                result.latencies.hash.push_back(completion.hash_ns);
                result.latencies.total.push_back(completion.total_ns);
            }
        }

        if (now < deadline) {
            submitOne(context, seed, nextTag++);
            ++active;
        }
    }
    result.elapsedSeconds = std::chrono::duration<double>(
        Clock::now() - begin).count();
    return result;
}

double percentile(const std::vector<uint64_t> &sorted, double fraction)
{
    if (sorted.empty()) {
        return 0.0;
    }
    const size_t index = static_cast<size_t>(std::floor(
        fraction * static_cast<double>(sorted.size() - 1)));
    return static_cast<double>(sorted[index]) / 1'000'000.0;
}

double averageMs(const std::vector<uint64_t> &values)
{
    long double total = 0.0;
    for (const uint64_t value : values) {
        total += static_cast<long double>(value);
    }
    return values.empty()
               ? 0.0
               : static_cast<double>(total /
                                     static_cast<long double>(values.size()) /
                                     1'000'000.0L);
}

void printLatency(const char *name, std::vector<uint64_t> values)
{
    if (values.empty()) {
        std::cout << std::left << std::setw(14) << name << " n/a\n";
        return;
    }
    const double average = averageMs(values);
    std::sort(values.begin(), values.end());
    std::cout << std::left << std::setw(14) << name
              << " avg=" << std::fixed << std::setprecision(3)
              << average
              << " ms  p50=" << percentile(values, 0.50)
              << " ms  p95=" << percentile(values, 0.95)
              << " ms  p99=" << percentile(values, 0.99) << " ms\n";
}

} // namespace

int main(int argc, char **argv)
{
    try {
        const Options options = parseOptions(argc, argv);
        const std::array<uint8_t, 12> seed{
            't', 'e', 's', 't', ' ', 'k', 'e', 'y', ' ', '0', '0', '0'};
        const std::array<uint8_t, 14> input{
            'T', 'h', 'i', 's', ' ', 'i', 's', ' ', 'a', ' ', 't', 'e', 's', 't'};
        const std::array<uint8_t, MSPV_HASH_SIZE> expected{
            0x63, 0x91, 0x83, 0xaa, 0xe1, 0xbf, 0x4c, 0x9a,
            0x35, 0x88, 0x4c, 0xb4, 0x6b, 0x09, 0xca, 0xd9,
            0x17, 0x5f, 0x04, 0xef, 0xd7, 0x68, 0x4e, 0x72,
            0x62, 0xa0, 0xac, 0x1c, 0x2f, 0x0b, 0x4e, 0x3f};

        mspv_config config{};
        check(mspv_config_init(&config), "configuration");
        config.memory_mode = options.mode;
        config.large_pages = options.largePages;
        config.options = options.options;
        if (options.workers != 0) {
            config.worker_count = options.workers;
        }
        config.seed_init_threads = options.initThreads != 0
                                       ? options.initThreads
                                       : config.worker_count;
        const uint64_t defaultInflight =
            static_cast<uint64_t>(config.worker_count) * 4u;
        const uint32_t inflight = options.inflight != 0
                                      ? options.inflight
                                      : static_cast<uint32_t>(
                                            std::min<uint64_t>(defaultInflight,
                                                               1'000'000u));
        config.pending_capacity = inflight;
        config.max_outstanding = inflight;
        config.max_input_size = static_cast<uint32_t>(kBenchmarkInputSize);
        config.max_buffered_input_bytes =
            static_cast<uint64_t>(inflight) * kBenchmarkInputSize;
        config.max_seeds = 1;
        if (options.verbose) {
            config.log = logMessage;
            config.log_level = MSPV_LOG_INFO;
        }

        const uint64_t residentMiB =
            (options.mode == MSPV_MEMORY_FAST ? 2080u : 256u) +
            static_cast<uint64_t>(config.worker_count) * 2u;
        const uint64_t preparationPeakMiB =
            options.mode == MSPV_MEMORY_FAST
                ? std::max<uint64_t>(2336u, residentMiB)
                : residentMiB;
        const uint32_t timeoutMs = options.timeoutSeconds * 1000u;

        std::cout << "benchmark configuration\n"
                  << "  mode: "
                  << (options.mode == MSPV_MEMORY_FAST ? "fast" : "light")
                  << ", workers: " << config.worker_count
                  << ", inflight limit: " << inflight << '\n'
                  << "  JIT: " << jitPolicy(options.options)
                  << ", AES: "
                  << ((options.options & MSPV_OPTION_DISABLE_HARD_AES) != 0
                          ? "software"
                          : "detected")
                  << ", large pages: " << largePagePolicy(options.largePages)
                  << '\n'
                  << "  estimated RandomX resident memory: " << residentMiB
                  << " MiB; RandomX preparation peak: " << preparationPeakMiB
                  << " MiB\n"
                  << "preparing RandomX seed...\n"
                  << std::flush;

        Context context;
        check(mspv_create(&config, &context.value), "create");
        check(mspv_start(context.value), "start");

        const Clock::time_point prepareBegin = Clock::now();
        mspv_seed_id seedId = 0;
        check(mspv_seed_prepare(context.value,
                                seed.data(),
                                seed.size(),
                                &seedId),
              "prepare seed");
        check(mspv_seed_wait_ready(context.value,
                                   seedId,
                                   timeoutMs),
              "wait for seed");
        const double prepareSeconds = std::chrono::duration<double>(
            Clock::now() - prepareBegin).count();
        check(mspv_seed_activate(context.value, seedId), "activate seed");

        uint64_t validationTicket = 0;
        check(mspv_verify_submit(context.value,
                                 seedId,
                                 input.data(),
                                 input.size(),
                                 expected.data(),
                                 0,
                                 &validationTicket),
              "known-answer submission");
        mspv_completion validation{};
        check(mspv_completion_poll(context.value,
                                   &validation,
                                   timeoutMs),
              "known-answer completion");
        if (validation.result != MSPV_RESULT_OK ||
            validation.error != MSPV_OK ||
            validation.ticket != validationTicket ||
            validation.comparison != MSPV_COMPARISON_MATCH ||
            std::memcmp(validation.hash,
                        expected.data(),
                        expected.size()) != 0) {
            throw std::runtime_error("known-answer validation failed");
        }

        uint64_t nextTag = 1;
        if (options.warmupSeconds != 0) {
            const PhaseResult warmup = runPhase(context.value,
                                                seedId,
                                                inflight,
                                                options.warmupSeconds,
                                                timeoutMs,
                                                options.sampleEvery,
                                                nextTag,
                                                false);
            if (warmup.failed != 0 || warmup.unexpectedComparison != 0) {
                throw std::runtime_error("warm-up produced an invalid result");
            }
        }

        const PhaseResult result = runPhase(context.value,
                                            seedId,
                                            inflight,
                                            options.seconds,
                                            timeoutMs,
                                            options.sampleEvery,
                                            nextTag,
                                            true);
        mspv_seed_info seedInfo{};
        check(mspv_seed_get_info(context.value, seedId, &seedInfo),
              "seed information");
        check(mspv_shutdown(context.value, MSPV_SHUTDOWN_DRAIN), "shutdown");

        const double timedSeconds = static_cast<double>(options.seconds);
        const double hashesPerSecond =
            static_cast<double>(result.completed) / timedSeconds;

        std::cout << "\nmonero-stratum-pow-verifier benchmark\n"
                  << "mode:             "
                  << (options.mode == MSPV_MEMORY_FAST ? "fast" : "light")
                  << '\n'
                  << "workers:          " << config.worker_count << '\n'
                  << "init threads:      " << config.seed_init_threads << '\n'
                  << "inflight limit:    " << inflight << '\n'
                  << "JIT policy:        " << jitPolicy(options.options) << '\n'
                  << "AES policy:        "
                  << ((options.options & MSPV_OPTION_DISABLE_HARD_AES) != 0
                          ? "software"
                          : "detected")
                  << '\n'
                  << "large-page policy: "
                  << largePagePolicy(options.largePages) << '\n'
                  << "large-page memory: "
                  << (seedInfo.memory_uses_large_pages ? "yes" : "no") << '\n'
                  << "all VM pages:      "
                  << (seedInfo.all_vms_use_large_pages ? "yes" : "no") << '\n'
                  << "prepare time:      " << std::fixed << std::setprecision(3)
                  << prepareSeconds << " s\n"
                  << "timed duration:    " << options.seconds << " s\n"
                  << "drain-inclusive:   " << result.elapsedSeconds << " s\n"
                  << "completed hashes:  " << result.completed << '\n'
                  << "hashes/second:     " << std::fixed << std::setprecision(2)
                  << hashesPerSecond << '\n'
                  << "failed hashes:     " << result.failed << '\n'
                  << "unexpected compare: "
                  << result.unexpectedComparison << '\n'
                  << "latency samples:   " << result.latencies.hash.size()
                  << " (every " << options.sampleEvery << ")\n";
        printLatency("queue latency", result.latencies.queue);
        printLatency("hash latency", result.latencies.hash);
        printLatency("total latency", result.latencies.total);
        return result.failed == 0 && result.unexpectedComparison == 0 ? 0 : 1;
    }
    catch (const std::exception &error) {
        std::cerr << "benchmark failed: " << error.what() << '\n';
        return 1;
    }
}
