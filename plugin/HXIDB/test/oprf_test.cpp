// Checks the C++ identity derivation against the Go server's fixed vector (server/internal/oprf/oprf_test.go), and
// optionally against a running server:   oprf_test.exe [http://127.0.0.1:8081]
#include <cstdio>
#include <cstring>
#include <string>
#include "../core/oprf.h"

static std::string Hex(const uint8_t* p, size_t n)
{
    static const char* h = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += h[p[i] >> 4]; s += h[p[i] & 15]; }
    return s;
}

static void FromHex(const char* h, uint8_t* out)
{
    for (size_t i = 0; h[i * 2]; i++) { unsigned v; sscanf_s(h + i * 2, "%2x", &v); out[i] = (uint8_t)v; }
}

int main(int argc, char** argv)
{
    int bad = 0;
    auto check = [&](const char* what, bool ok) { printf("%-58s %s\n", what, ok ? "ok" : "FAIL"); if (!ok) bad++; };

    // Vector from the Go server test.
    const std::string input = "HorizonXI|player|123456";
    uint8_t r[32] = {9}, A[32], B[32], seed[32];
    FromHex("226655e354abe964ae0045d929a5f165af40529630144c48a5165de3da7a0f76", B);
    check("blinded element A equals the Go value", hxidb::OprfBlind(input, r, A) &&
          Hex(A, 32) == "5c5d1256d1681fd7cbb04287af698e7a7b15bf745798743a46b6100a76641f35");
    check("identity seed equals the Go value", hxidb::OprfFinish(input, r, B, seed) &&
          Hex(seed, 32) == "df6caae1e94e2d66f88fa038cc70d35b940c299a110a5f7bac330c32a3aa2890");
    uint8_t zero[32] = {};
    check("the identity element is refused", !hxidb::OprfFinish(input, r, zero, seed));

    if (argc > 1)
    {
        uint8_t s1[32], s2[32], s3[32];
        int st = 0;
        bool a = hxidb::OprfDerive(argv[1], "HorizonXI|player|42", s1, &st);
        printf("  derive #1 http %d\n", st);
        bool b = hxidb::OprfDerive(argv[1], "HorizonXI|player|42", s2, &st);
        bool c = hxidb::OprfDerive(argv[1], "HorizonXI|player|43", s3, &st);
        check("live server: same player, same identity (fresh blinding)", a && b && memcmp(s1, s2, 32) == 0);
        check("live server: another player, another identity", c && memcmp(s1, s3, 32) != 0);
        printf("  identity seed for player 42: %s\n", Hex(s1, 32).c_str());
    }
    printf(bad ? "%d FAILED\n" : "all passed\n", bad);
    return bad ? 1 : 0;
}
