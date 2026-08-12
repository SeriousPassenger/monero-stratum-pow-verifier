#include <monero_stratum_pow_verifier.h>

int main()
{
    mspv_config config{};
    mspv_context *context = nullptr;
    if (mspv_config_init(&config) != MSPV_OK ||
        mspv_create(&config, &context) != MSPV_OK) {
        return 1;
    }
    mspv_destroy(context);
    return 0;
}
