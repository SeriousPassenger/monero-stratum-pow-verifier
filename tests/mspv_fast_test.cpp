/* SPDX-License-Identifier: MIT */

#include "monero_stratum_pow_verifier.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> hex(const std::string &text)
{
    const auto digit = [](char value) -> uint8_t {
        if (value >= '0' && value <= '9') {
            return static_cast<uint8_t>(value - '0');
        }
        if (value >= 'a' && value <= 'f') {
            return static_cast<uint8_t>(value - 'a' + 10);
        }
        throw std::runtime_error("invalid hex fixture");
    };
    if (text.empty() || (text.size() % 2) != 0) {
        throw std::runtime_error("invalid hex fixture length");
    }
    std::vector<uint8_t> bytes(text.size() / 2);
    for (size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<uint8_t>(
            (digit(text[index * 2]) << 4) | digit(text[index * 2 + 1]));
    }
    return bytes;
}

void check(mspv_status status, const char *operation)
{
    if (status != MSPV_OK) {
        throw std::runtime_error(std::string(operation) + ": " +
                                 mspv_status_string(status));
    }
}

} // namespace

int main()
{
    mspv_context *context = nullptr;
    try {
        const std::vector<uint8_t> seed{
            't', 'e', 's', 't', ' ', 'k', 'e', 'y', ' ', '0', '0', '0'};
        const std::vector<uint8_t> input{
            'T', 'h', 'i', 's', ' ', 'i', 's', ' ', 'a', ' ', 't', 'e', 's', 't'};
        const std::vector<uint8_t> expected =
            hex("639183aae1bf4c9a35884cb46b09cad9175f04efd7684e7262a0ac1c2f0b4e3f");

        mspv_config config{};
        check(mspv_config_init(&config), "config");
        config.worker_count = 2;
        config.seed_init_threads = 4;
        config.pending_capacity = 8;
        config.max_outstanding = 8;
        config.max_seeds = 1;
        config.memory_mode = MSPV_MEMORY_FAST;
        config.large_pages = MSPV_LARGE_PAGES_TRY;

        check(mspv_create(&config, &context), "create");
        check(mspv_start(context), "start");
        mspv_seed_id seedId = 0;
        check(mspv_seed_prepare(context,
                                seed.data(),
                                seed.size(),
                                &seedId),
              "prepare");
        check(mspv_seed_wait_ready(context, seedId, MSPV_WAIT_FOREVER),
              "wait ready");
        check(mspv_seed_activate(context, seedId), "activate");

        for (uint64_t index = 0; index < 4; ++index) {
            uint64_t ticket = 0;
            check(mspv_verify_submit(context,
                                     seedId,
                                     input.data(),
                                     input.size(),
                                     expected.data(),
                                     index,
                                     &ticket),
                  "verify submit");
        }
        for (int index = 0; index < 4; ++index) {
            mspv_completion completion{};
            check(mspv_completion_poll(context,
                                       &completion,
                                       MSPV_WAIT_FOREVER),
                  "completion");
            if (completion.result != MSPV_RESULT_OK ||
                completion.comparison != MSPV_COMPARISON_MATCH ||
                std::memcmp(completion.hash,
                            expected.data(),
                            MSPV_HASH_SIZE) != 0) {
                throw std::runtime_error("fast-mode known-answer mismatch");
            }
        }

        check(mspv_shutdown(context, MSPV_SHUTDOWN_DRAIN), "shutdown");
        mspv_destroy(context);
        std::cout << "fast-mode verifier test passed\n";
        return 0;
    }
    catch (const std::exception &error) {
        std::cerr << "fast-mode verifier test failed: " << error.what() << '\n';
        mspv_destroy(context);
        return 1;
    }
}
