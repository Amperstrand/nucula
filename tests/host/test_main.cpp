// Host runner for nucula's on-device self-test suites. Same code, same
// vectors, no hardware: the suites the firmware can run at boot via
// the 'selftest' console command, plus Wallet::run_tests() which the
// console path also reaches.

#include <cstdio>

#include <secp256k1.h>

#include "esp_random.h"

#include "cashu_json.hpp"
#include "crypto_test.h"
#include "keyset.hpp"
#include "selftest.hpp"
#include "unit.hpp"
#include "wallet.hpp"

// ---------------------------------------------------------------------------
// Host fakes: flow methods that live in wallet_flows.cpp / wallet_keysets.cpp
// (http-backed, excluded from this build). The pure suites under test do not
// call them; these satisfy the linker for anything that does.
// ---------------------------------------------------------------------------

namespace cashu {

bool Wallet::load_keysets()
{
    return false;
}

bool Wallet::request_mint_quote(int, const std::string &, const std::string &, MintQuote &)
{
    return false;
}

bool Wallet::check_mint_quote(const std::string &, const std::string &, MintQuote &)
{
    return false;
}

bool Wallet::mint_tokens(const MintQuote &, int)
{
    return false;
}

bool Wallet::request_melt_quote(const std::string &, const std::string &, const std::string &, MeltQuote &, std::optional<int>)
{
    return false;
}

bool Wallet::check_melt_quote(const std::string &, const std::string &, MeltQuote &)
{
    return false;
}

bool Wallet::melt_tokens(const MeltQuote &, int &)
{
    return false;
}

bool Wallet::swap(std::vector<Proof> &, int, std::vector<Proof> &, std::vector<Proof> &)
{
    return false;
}

bool Wallet::receive(const Token &, std::vector<Proof> &)
{
    return false;
}

} // namespace cashu

// ---------------------------------------------------------------------------

static int g_failed = 0;

static void run(const char *name, bool ok)
{
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) g_failed++;
}

int main()
{
    secp256k1_context *ctx =
        secp256k1_context_create(SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    if (!ctx) {
        fprintf(stderr, "secp256k1 context creation failed\n");
        return 2;
    }
    {
        unsigned char rand32[32];
        esp_fill_random(rand32, sizeof(rand32));
        secp256k1_context_randomize(ctx, rand32);
    }

    run("crypto vectors (BDHKE/DLEQ/NUT-13)", crypto_run_tests(ctx) == 0);
    run("keyset id derivation", cashu::keyset_run_tests());
    run("unit formatting", cashu::unit_run_tests());
    run("quote/mint-info json", cashu::cashu_json_run_tests());
    run("pure codec/math selftests", nucula_pure_selftests());
    run("wallet logic (fees/selection/units)", cashu::Wallet::run_tests());

    secp256k1_context_destroy(ctx);
    printf("\n%s (%d suite(s) failed)\n", g_failed ? "FAILED" : "ALL PASS", g_failed);
    return g_failed ? 1 : 0;
}
