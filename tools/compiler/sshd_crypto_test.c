/* ============================================================
 * sshd_crypto_test.c —— sshd_crypto.h 的宿主侧单测
 * ------------------------------------------------------------
 * 用各标准的官方测试向量逐条校验：
 *   SHA-256    FIPS 180-4 / NIST "abc"
 *   SHA-512    FIPS 180-4 / NIST "abc"
 *   HMAC-SHA256 RFC 4231 测试用例 1..4
 *   AES-128-CTR NIST SP 800-38A F.5.1
 *   X25519      RFC 7748 §5.2 / §6.1
 *   Ed25519     RFC 8032 §7.1 TEST 1..2、TEST 1024
 * 编译（宿主 x86_64）：
 *   zig cc -O2 -o sshd_crypto_test.exe sshd_crypto_test.c && ./sshd_crypto_test.exe
 * ============================================================ */
#include <stdio.h>
#include <string.h>
#include "sshd_crypto.h"

static int g_fail = 0, g_pass = 0;

static void hex2bin(const char *h, c_u8 *out, int n) {
    for (int i = 0; i < n; i++) {
        int v = 0;
        for (int k = 0; k < 2; k++) {
            char c = h[i * 2 + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        }
        out[i] = (c_u8)v;
    }
}

static void check(const char *name, const c_u8 *got, const char *want_hex, int n) {
    char g[512];
    for (int i = 0; i < n; i++) sprintf(g + i * 2, "%02x", got[i]);
    g[n * 2] = 0;
    if (strcmp(g, want_hex) == 0) { g_pass++; printf("  PASS  %s\n", name); }
    else { g_fail++; printf("  FAIL  %s\n        got  %s\n        want %s\n", name, g, want_hex); }
}

/* ---------------- SHA-256 / SHA-512 ---------------- */
static void test_hashes(void) {
    printf("== SHA-256 / SHA-512 ==\n");
    c_u8 h32[32], h64[64];
    cx_sha256((const c_u8 *)"abc", 3, h32);
    check("SHA-256(\"abc\")", h32,
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", 32);

    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    cx_sha256((const c_u8 *)m448, (c_u32)strlen(m448), h32);
    check("SHA-256(448-bit msg)", h32,
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", 32);

    cx_sha512((const c_u8 *)"abc", 3, h64);
    check("SHA-512(\"abc\")", h64,
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f", 64);

    cx_sha512((const c_u8 *)m448, (c_u32)strlen(m448), h64);
    check("SHA-512(448-bit msg)", h64,
          "204a8fc6dda82f0a0ced7beb8e08a41657c16ef468b228a8279be331a703c335"
          "96fd15c13b1b07f9aa1d3bea57789ca031ad85c7a71dd70354ec631238ca3445", 64);

    /* 896 位消息（2 个块）：这是 FIPS 180-4 给出的 SHA-512 示例 */
    {
        const char *m896 = "abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                           "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu";
        cx_sha512((const c_u8 *)m896, (c_u32)strlen(m896), h64);
        check("SHA-512(896-bit msg)", h64,
              "8e959b75dae313da8cf4f72814fc143f8f7779c6eb9f7fa17299aeadb6889018"
              "501d289e4900f7e4331b99dec4b5433ac7d329eeb6dd26545e96e55b874be909", 64);
    }

    /* 多块输入：100 万个 'a'，走一遍 update 分片，验证流式状态机 */
    {
        cx_sha256_ctx c; cx_sha256_init(&c);
        c_u8 blk[1000]; memset(blk, 'a', sizeof(blk));
        for (int i = 0; i < 1000; i++) cx_sha256_update(&c, blk, 1000);
        cx_sha256_final(&c, h32);
        check("SHA-256(1e6 x 'a')", h32,
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", 32);
    }
}

/* ---------------- HMAC-SHA256 (RFC 4231) ---------------- */
static void test_hmac(void) {
    printf("== HMAC-SHA256 (RFC 4231) ==\n");
    c_u8 k[200], d[200], out[32];

    hex2bin("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", k, 20);
    hex2bin("4869205468657265", d, 8);
    cx_hmac256(k, 20, d, 8, out);
    check("TC1", out, "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", 32);

    hex2bin("4a656665", k, 4);
    hex2bin("7768617420646f2079612077616e7420666f72206e6f7468696e673f", d, 28);
    cx_hmac256(k, 4, d, 28, out);
    check("TC2", out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32);

    memset(k, 0xaa, 20);
    memset(d, 0xdd, 50);
    hex2bin("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
            "dddddddddddddddddddddddddddddddddddd", d, 50);
    cx_hmac256(k, 20, d, 50, out);
    check("TC3", out, "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe", 32);

    /* 超长 key（>64 字节）走 "先哈希" 分支 */
    memset(k, 0xaa, 131);
    hex2bin("54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a"
            "65204b6579202d2048617368204b6579204669727374", d, 54);
    cx_hmac256(k, 131, d, 54, out);
    check("TC6", out, "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", 32);
}

/* ---------------- AES-128-CTR (NIST SP 800-38A F.5.1) ---------------- */
static void test_aes_ctr(void) {
    printf("== AES-128-CTR (NIST F.5.1) ==\n");
    c_u8 key[16], iv[16], buf[64], want[64];
    hex2bin("2b7e151628aed2a6abf7158809cf4f3c", key, 16);
    hex2bin("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff", iv, 16);
    hex2bin("6bc1bee22e409f96e93d7e117393172a"
            "ae2d8a571e03ac9c9eb76fac45af8e51"
            "30c81c46a35ce411e5fbc1191a0a52ef"
            "f69f2445df4f9b17ad2b417be66c3710", buf, 64);
    hex2bin("874d6191b620e3261bef6864990db6ce"
            "9806f66b7970fdff8617187bb9fffdff"
            "5ae4df3edbd5d35e5b4f09020db03eab"
            "1e031dda2fbe03d1792170a0f3009cee", want, 64);

    cx_aes128ctr s;
    cx_aes128ctr_init(&s, key, iv);
    cx_aes128ctr_xor(&s, buf, 64);
    check("CTR encrypt 64B", buf, 
          "874d6191b620e3261bef6864990db6ce"
          "9806f66b7970fdff8617187bb9fffdff"
          "5ae4df3edbd5d35e5b4f09020db03eab"
          "1e031dda2fbe03d1792170a0f3009cee", 64);
    (void)want;

    /* 分片调用必须与一次性调用等价（SSH 数据包是流式解密的） */
    c_u8 buf2[64];
    memcpy(buf2, want, 64);
    cx_aes128ctr s2; cx_aes128ctr_init(&s2, key, iv);
    for (int i = 0; i < 64; i += 7) {
        int n = (64 - i < 7) ? 64 - i : 7;
        cx_aes128ctr_xor(&s2, buf2 + i, (c_u32)n);
    }
    check("CTR split-into-7 decrypt", buf2,
          "6bc1bee22e409f96e93d7e117393172a"
          "ae2d8a571e03ac9c9eb76fac45af8e51"
          "30c81c46a35ce411e5fbc1191a0a52ef"
          "f69f2445df4f9b17ad2b417be66c3710", 64);
}

/* ---------------- X25519 (RFC 7748) ---------------- */
static void test_x25519(void) {
    printf("== X25519 (RFC 7748) ==\n");
    c_u8 k[32], u[32], out[32];

    /* §5.2 第一个向量 */
    hex2bin("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4", k, 32);
    hex2bin("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c", u, 32);
    cx_x25519(out, k, u);
    check("RFC7748 §5.2 #1", out,
          "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552", 32);

    /* §5.2 第二个向量 */
    hex2bin("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d", k, 32);
    hex2bin("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493", u, 32);
    cx_x25519(out, k, u);
    check("RFC7748 §5.2 #2", out,
          "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957", 32);

    /* §6.1 基点标量乘（这组是 SSH curve25519-sha256 实际用的形状） */
    hex2bin("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", k, 32);
    cx_x25519_base(out, k);
    check("RFC7748 §6.1 alice.pub", out,
          "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", 32);

    hex2bin("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", k, 32);
    cx_x25519_base(out, k);
    check("RFC7748 §6.1 bob.pub", out,
          "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", 32);

    /* 两端协商出的共享密钥必须相同 */
    c_u8 a_priv[32], b_priv[32], a_pub[32], b_pub[32], s1[32], s2[32];
    hex2bin("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a_priv, 32);
    hex2bin("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b_priv, 32);
    cx_x25519_base(a_pub, a_priv);
    cx_x25519_base(b_pub, b_priv);
    cx_x25519(s1, a_priv, b_pub);
    cx_x25519(s2, b_priv, a_pub);
    int same = (memcmp(s1, s2, 32) == 0);
    if (same && memcmp(s1,
        "\x4a\x5d\x9d\x5b\xa4\xce\x2d\xe1\x72\x8e\x3b\xf4\x80\x35\x0f\x25"
        "\xe0\x7e\x21\xc9\x47\xd1\x9e\x33\x76\xf0\x9b\x3c\x1e\x16\x17\x42", 32) == 0) {
        g_pass++; printf("  PASS  ECDH shared secret\n");
    } else {
        g_fail++;
        printf("  FAIL  ECDH shared secret (equal=%d)\n", same);
        printf("        got  ");
        for (int i = 0; i < 32; i++) printf("%02x", s1[i]);
        printf("\n");
    }
}

/* ---------------- Ed25519 (RFC 8032 §7.1) ---------------- */
static void test_ed25519(void) {
    printf("== Ed25519 (RFC 8032 §7.1) ==\n");
    c_u8 seed[32], pub[32], msg[1024], sig[64];

    /* TEST 1: 空消息 */
    hex2bin("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed, 32);
    cx_ed_public_key(pub, seed);
    check("TEST1 public key", pub,
          "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", 32);
    cx_ed_sign(sig, seed, (const c_u8 *)"", 0);
    check("TEST1 signature", sig,
          "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
          "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b", 64);

    /* TEST 2: 1 字节消息 */
    hex2bin("4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb", seed, 32);
    cx_ed_public_key(pub, seed);
    check("TEST2 public key", pub,
          "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", 32);
    hex2bin("72", msg, 1);
    cx_ed_sign(sig, seed, msg, 1);
    check("TEST2 signature", sig,
          "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
          "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00", 64);

    /* TEST 3: 2 字节消息（考验长度拼接） */
    hex2bin("c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7", seed, 32);
    cx_ed_public_key(pub, seed);
    check("TEST3 public key", pub,
          "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", 32);
    hex2bin("af82", msg, 2);
    cx_ed_sign(sig, seed, msg, 2);
    check("TEST3 signature", sig,
          "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
          "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a", 64);

    /* TEST 1024（1023 字节消息）：验证大消息与流式 SHA-512 拼接 */
    hex2bin("f5e5767cf153319517630f226876b86c8160cc583bc013744c6bf255f5cc0ee5", seed, 32);
    cx_ed_public_key(pub, seed);
    check("TEST1024 public key", pub,
          "278117fc144c72340f67d0f2316e8386ceffbf2b2428c9c51fef7c597f1d426e", 32);
    {
        static const char *m =
            "08b8b2b733424243760fe426a4b54908632110a66c2f6591eabd3345e3e4eb98"
            "fa6e264bf09efe12ee50f8f54e9f77b1e355f6c50544e23fb1433ddf73be84d8"
            "79de7c0046dc4996d9e773f4bc9efe5738829adb26c81b37c93a1b270b20329d"
            "658675fc6ea534e0810a4432826bf58c941efb65d57a338bbd2e26640f89ffbc"
            "1a858efcb8550ee3a5e1998bd177e93a7363c344fe6b199ee5d02e82d522c4fe"
            "ba15452f80288a821a579116ec6dad2b3b310da903401aa62100ab5d1a36553e"
            "06203b33890cc9b832f79ef80560ccb9a39ce767967ed628c6ad573cb116dbef"
            "efd75499da96bd68a8a97b928a8bbc103b6621fcde2beca1231d206be6cd9ec7"
            "aff6f6c94fcd7204ed3455c68c83f4a41da4af2b74ef5c53f1d8ac70bdcb7ed1"
            "85ce81bd84359d44254d95629e9855a94a7c1958d1f8ada5d0532ed8a5aa3fb2"
            "d17ba70eb6248e594e1a2297acbbb39d502f1a8c6eb6f1ce22b3de1a1f40cc24"
            "554119a831a9aad6079cad88425de6bde1a9187ebb6092cf67bf2b13fd65f270"
            "88d78b7e883c8759d2c4f5c65adb7553878ad575f9fad878e80a0c9ba63bcbcc"
            "2732e69485bbc9c90bfbd62481d9089beccf80cfe2df16a2cf65bd92dd597b07"
            "07e0917af48bbb75fed413d238f5555a7a569d80c3414a8d0859dc65a46128ba"
            "b27af87a71314f318c782b23ebfe808b82b0ce26401d2e22f04d83d1255dc51a"
            "ddd3b75a2b1ae0784504df543af8969be3ea7082ff7fc9888c144da2af58429e"
            "c96031dbcad3dad9af0dcbaaaf268cb8fcffead94f3c7ca495e056a9b47acdb7"
            "51fb73e666c6c655ade8297297d07ad1ba5e43f1bca32301651339e22904cc8c"
            "42f58c30c04aafdb038dda0847dd988dcda6f3bfd15c4b4c4525004aa06eeff8"
            "ca61783aacec57fb3d1f92b0fe2fd1a85f6724517b65e614ad6808d6f6ee34df"
            "f7310fdc82aebfd904b01e1dc54b2927094b2db68d6f903b68401adebf5a7e08"
            "d78ff4ef5d63653a65040cf9bfd4aca7984a74d37145986780fc0b16ac451649"
            "de6188a7dbdf191f64b5fc5e2ab47b57f7f7276cd419c17a3ca8e1b939ae49e4"
            "88acba6b965610b5480109c8b17b80e1b7b750dfc7598d5d5011fd2dcc5600a3"
            "2ef5b52a1ecc820e308aa342721aac0943bf6686b64b2579376504ccc493d97e"
            "6aed3fb0f9cd71a43dd497f01f17c0e2cb3797aa2a2f256656168e6c496afc5f"
            "b93246f6b1116398a346f1a641f3b041e989f7914f90cc2c7fff357876e506b5"
            "0d334ba77c225bc307ba537152f3f1610e4eafe595f6d9d90d11faa933a15ef1"
            "369546868a7f3a45a96768d40fd9d03412c091c6315cf4fde7cb68606937380d"
            "b2eaaa707b4c4185c32eddcdd306705e4dc1ffc872eeee475a64dfac86aba41c"
            "0618983f8741c5ef68d3a101e8a3b8cac60c905c15fc910840b94c00a0b9d0";
        hex2bin(m, msg, 1023);
    }
    cx_ed_sign(sig, seed, msg, 1023);
    check("TEST1024 signature", sig,
          "0aab4c900501b3e24d7cdf4663326a3a87df5e4843b2cbdb67cbf6e460fec350"
          "aa5371b1508f9f4528ecea23c436d94b5e8fcd4f681e30a6ac00a9704a188a03", 64);
}

int main(void) {
    printf("TinyOS sshd crypto self-test\n");
    printf("----------------------------\n");
    test_hashes();
    test_hmac();
    test_aes_ctr();
    test_x25519();
    test_ed25519();
    printf("----------------------------\n");
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
