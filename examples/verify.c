/*
 * Minimal C example for monero-stratum-pow-verifier.
 * SPDX-License-Identifier: MIT
 */

#include "monero_stratum_pow_verifier.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *log_level_name(mspv_log_level level)
{
    switch (level) {
    case MSPV_LOG_ERROR:
        return "error";
    case MSPV_LOG_WARNING:
        return "warning";
    case MSPV_LOG_INFO:
        return "info";
    case MSPV_LOG_DEBUG:
        return "debug";
    case MSPV_LOG_TRACE:
        return "trace";
    }
    return "unknown";
}

static void log_message(void *user_data,
                        mspv_log_level level,
                        const char *message)
{
    (void)user_data;
    fprintf(stderr, "[mspv:%s] %s\n", log_level_name(level), message);
}

static int hex_digit(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static int decode_hex(const char *text, uint8_t **out, size_t *out_size)
{
    const size_t length = strlen(text);
    size_t index;
    uint8_t *bytes;

    *out = NULL;
    *out_size = 0;
    if (length == 0 || (length % 2) != 0) {
        return 0;
    }
    bytes = (uint8_t *)malloc(length / 2);
    if (bytes == NULL) {
        return 0;
    }
    for (index = 0; index < length / 2; ++index) {
        const int high = hex_digit(text[index * 2]);
        const int low = hex_digit(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            free(bytes);
            return 0;
        }
        bytes[index] = (uint8_t)((high << 4) | low);
    }
    *out = bytes;
    *out_size = length / 2;
    return 1;
}

static void print_hex(const uint8_t *bytes, size_t size)
{
    static const char alphabet[] = "0123456789abcdef";
    size_t index;
    for (index = 0; index < size; ++index) {
        putchar(alphabet[bytes[index] >> 4]);
        putchar(alphabet[bytes[index] & 0x0f]);
    }
}

static int check(mspv_status status, const char *operation)
{
    if (status == MSPV_OK) {
        return 1;
    }
    fprintf(stderr, "%s failed: %s\n", operation, mspv_status_string(status));
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t *seed = NULL;
    uint8_t *blob = NULL;
    uint8_t *claimed = NULL;
    size_t seed_size = 0;
    size_t blob_size = 0;
    size_t claimed_size = 0;
    mspv_config config;
    mspv_context *context = NULL;
    mspv_seed_id seed_id = 0;
    uint64_t ticket = 0;
    mspv_completion completion;
    unsigned long workers = 0;
    int result = 1;

    if (argc < 4 || argc > 6 ||
        (strcmp(argv[1], "light") != 0 && strcmp(argv[1], "fast") != 0)) {
        fprintf(stderr,
                "usage: %s <light|fast> <seed-hex> <blob-hex> "
                "[claimed-hash-hex] [workers]\n",
                argv[0]);
        return 2;
    }
    if (!decode_hex(argv[2], &seed, &seed_size) ||
        !decode_hex(argv[3], &blob, &blob_size)) {
        fprintf(stderr, "seed and blob must be non-empty, even-length hex\n");
        goto cleanup;
    }
    if (argc >= 5) {
        if (!decode_hex(argv[4], &claimed, &claimed_size) ||
            claimed_size != MSPV_HASH_SIZE) {
            fprintf(stderr, "claimed hash must be exactly 32 bytes of hex\n");
            goto cleanup;
        }
    }
    if (argc == 6) {
        char *end = NULL;
        errno = 0;
        workers = strtoul(argv[5], &end, 10);
        if (errno != 0 || end == argv[5] || *end != '\0' ||
            workers == 0 || workers > UINT32_MAX) {
            fprintf(stderr, "workers must be a positive 32-bit integer\n");
            goto cleanup;
        }
    }

    if (!check(mspv_config_init(&config), "config initialization")) {
        goto cleanup;
    }
    config.memory_mode = strcmp(argv[1], "fast") == 0
                             ? MSPV_MEMORY_FAST
                             : MSPV_MEMORY_LIGHT;
    if (workers != 0) {
        config.worker_count = (uint32_t)workers;
        config.seed_init_threads = (uint32_t)workers;
    }
    if (seed_size > config.max_seed_key_size) {
        fprintf(stderr,
                "seed is too large: RandomX keys are limited to %u bytes\n",
                config.max_seed_key_size);
        goto cleanup;
    }
    if (blob_size > config.max_input_size) {
        if (blob_size > UINT32_MAX) {
            fprintf(stderr, "blob is too large\n");
            goto cleanup;
        }
        config.max_input_size = (uint32_t)blob_size;
        if (config.max_buffered_input_bytes < blob_size) {
            config.max_buffered_input_bytes = blob_size;
        }
    }
    config.log = log_message;
    config.log_level = MSPV_LOG_DEBUG;

    if (!check(mspv_create(&config, &context), "create") ||
        !check(mspv_start(context), "start") ||
        !check(mspv_seed_prepare(context, seed, seed_size, &seed_id),
               "prepare seed") ||
        !check(mspv_seed_wait_ready(context, seed_id, MSPV_WAIT_FOREVER),
               "wait for seed") ||
        !check(mspv_seed_activate(context, seed_id), "activate seed")) {
        goto cleanup;
    }

    if (claimed != NULL) {
        if (!check(mspv_verify_submit(context,
                                      seed_id,
                                      blob,
                                      blob_size,
                                      claimed,
                                      1,
                                      &ticket),
                   "submit verification")) {
            goto cleanup;
        }
    }
    else if (!check(mspv_hash_submit(context,
                                     seed_id,
                                     blob,
                                     blob_size,
                                     1,
                                     &ticket),
                    "submit hash")) {
        goto cleanup;
    }

    if (!check(mspv_completion_poll(context,
                                    &completion,
                                    MSPV_WAIT_FOREVER),
               "wait for completion") ||
        completion.result != MSPV_RESULT_OK || completion.ticket != ticket) {
        fprintf(stderr, "verification did not complete successfully\n");
        goto cleanup;
    }

    printf("hash: ");
    print_hex(completion.hash, MSPV_HASH_SIZE);
    putchar('\n');
    if (completion.comparison == MSPV_COMPARISON_MATCH) {
        puts("comparison: MATCH");
    }
    else if (completion.comparison == MSPV_COMPARISON_MISMATCH) {
        puts("comparison: MISMATCH");
    }
    else {
        puts("comparison: not requested");
    }
    printf("queue_ns: %llu\nhash_ns: %llu\ntotal_ns: %llu\n",
           (unsigned long long)completion.queue_ns,
           (unsigned long long)completion.hash_ns,
           (unsigned long long)completion.total_ns);
    result = completion.comparison == MSPV_COMPARISON_MISMATCH ? 3 : 0;

cleanup:
    if (context != NULL) {
        (void)mspv_shutdown(context, MSPV_SHUTDOWN_DRAIN);
        mspv_destroy(context);
    }
    free(claimed);
    free(blob);
    free(seed);
    return result;
}
