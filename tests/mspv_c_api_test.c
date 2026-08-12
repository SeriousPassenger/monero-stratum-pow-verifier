/* SPDX-License-Identifier: MIT */

#include "monero_stratum_pow_verifier.h"

#include <stddef.h>
#include <string.h>

int main(void)
{
    static const uint8_t seed[] = {
        't', 'e', 's', 't', ' ', 'k', 'e', 'y', ' ', '0', '0', '0'};
    static const uint8_t input[] = {
        'T', 'h', 'i', 's', ' ', 'i', 's', ' ', 'a', ' ', 't', 'e', 's', 't'};
    static const uint8_t expected[MSPV_HASH_SIZE] = {
        0x63, 0x91, 0x83, 0xaa, 0xe1, 0xbf, 0x4c, 0x9a,
        0x35, 0x88, 0x4c, 0xb4, 0x6b, 0x09, 0xca, 0xd9,
        0x17, 0x5f, 0x04, 0xef, 0xd7, 0x68, 0x4e, 0x72,
        0x62, 0xa0, 0xac, 0x1c, 0x2f, 0x0b, 0x4e, 0x3f};
    mspv_config config;
    mspv_context *context = NULL;
    mspv_completion completion;
    mspv_seed_id seed_id = 0;
    uint64_t ticket = 0;
    uint64_t iteration;

    if (mspv_config_init(&config) != MSPV_OK ||
        config.struct_size != sizeof(config) ||
        config.abi_version != MSPV_ABI_VERSION ||
        config.memory_mode != MSPV_MEMORY_LIGHT ||
        config.max_seeds != 2 ||
        strcmp(mspv_status_string(MSPV_OK), "ok") != 0) {
        return 1;
    }
    config.worker_count = 1;
    config.seed_init_threads = 1;
    config.pending_capacity = 1;
    config.max_outstanding = 1;
    config.large_pages = MSPV_LARGE_PAGES_DISABLED;
    config.options = MSPV_OPTION_DISABLE_JIT |
                     MSPV_OPTION_DISABLE_HARD_AES;

    if (mspv_create(&config, &context) != MSPV_OK || context == NULL) {
        return 2;
    }
    if (mspv_start(context) != MSPV_OK ||
        mspv_seed_prepare(context, seed, sizeof(seed), &seed_id) != MSPV_OK ||
        mspv_seed_wait_ready(context, seed_id, 120000) != MSPV_OK ||
        mspv_seed_activate(context, seed_id) != MSPV_OK) {
        mspv_destroy(context);
        return 3;
    }

    /* Capacity one catches lost worker wakeups and proves the full C ABI. */
    for (iteration = 0; iteration < 8; ++iteration) {
        if (mspv_verify_submit(context,
                               seed_id,
                               input,
                               sizeof(input),
                               expected,
                               iteration,
                               &ticket) != MSPV_OK ||
            ticket == 0 ||
            mspv_completion_poll(context, &completion, 120000) != MSPV_OK ||
            completion.ticket != ticket ||
            completion.user_tag != iteration ||
            completion.result != MSPV_RESULT_OK ||
            completion.error != MSPV_OK ||
            completion.comparison != MSPV_COMPARISON_MATCH ||
            memcmp(completion.hash, expected, MSPV_HASH_SIZE) != 0) {
            mspv_destroy(context);
            return 4;
        }
    }

    if (mspv_shutdown(context, MSPV_SHUTDOWN_DRAIN) != MSPV_OK) {
        mspv_destroy(context);
        return 5;
    }
    if (mspv_completion_poll(context, &completion, 0) != MSPV_CLOSED) {
        mspv_destroy(context);
        return 6;
    }
    mspv_destroy(context);
    return 0;
}
