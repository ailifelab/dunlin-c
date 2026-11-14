/* SHA-256 pins for the two supported Ubuntu GNU architectures. */
#if defined(__aarch64__)
#define ARCH "arm64"
#define TRIPLET "aarch64-linux-gnu"
#define ARCHIVE "libc6_2.39-0ubuntu8.12_arm64.deb"
#define BASE_URL "https://ports.ubuntu.com/ubuntu-ports/pool/main/g/glibc/"
#define ARCHIVE_HASH "2578bc27d4e077f5e93c283d2502bc693c516d9590a20a27840b548a4d436820"
#define NODE_BINARY_HASH "66f4a883c880ef336276e7730c01cbae2b92fb8e6a51f7371f920599cac824b7"
static const struct artifact objects[] = {
    {"ld-linux-aarch64.so.1", "124e3f166bdc0be365eaa6dc59e415b876b0841a40463edd1b0e084b518bb7cd"},
    {"libc.so.6", "2544e7902ec1bd3a68b526b6af427c34d2a98e56b9dbf8395b4b498dd2112cd6"},
    {"libdl.so.2", "f3b0b0b1e89e3d0e0c0f517ef841d91ae0db4c5996fc20a95c54edb6fc8bf325"},
    {"libm.so.6", "1bd8c762a2d70a2f6520469f5e014f3ac993bc0e924c7c6eb4cfdfd5e6acfc3b"},
    {"libpthread.so.0", "8a3c40319907930a7705ee88772d6d51f1dfe00ecf048ec48fedc1add09f93a0"},
    {"librt.so.1", "f6e1af56444f5dc657d341ded9b140cfa37c06f1c9f7d3e0197f83590162d5cb"},
    {"libresolv.so.2", "9efd7586fcbc32e349ef063395af66ed2385785ce7fa4d674b73e8b7e69a827f"},
    {"libutil.so.1", "c26133885322ff2c4792d1ce63db147da0d9c366c64a8dde1f675804c13bd310"},
    {"libnss_files.so.2", "d6962c92a27616faa479d3c81bab3118b687a6097af32cd9445bbce3e6e19d3f"},
    {"libnss_dns.so.2", "d8436a608c6a20692bb16a559168a0e4c3832bdbb7f09c3206b8de8ae0a4aa84"},
    {"COPYRIGHT", "d3c95b56fa33e28b57860580f0baf4e4f4de2a268a2b80f1d031a5191bade265"},
};
#else
#define ARCH "amd64"
#define TRIPLET "x86_64-linux-gnu"
#define ARCHIVE "libc6_2.39-0ubuntu8.12_amd64.deb"
#define BASE_URL "https://archive.ubuntu.com/ubuntu/pool/main/g/glibc/"
#define ARCHIVE_HASH "d821b41fc59cb4978ecd53bc83e5c964ac15d305c8d2ac4c8a35aebe55010fb9"
#define NODE_BINARY_HASH "0adde40d1b7dfa57c8bc83cbc319758c3ec6ebba21363bb2fa752ea6b267ad1b"
static const struct artifact objects[] = {
    {"ld-linux-x86-64.so.2", "5fc0940888e9d2e1a00e66d6b1a559c36364ee867683652e1074fa576952cc3f"},
    {"libc.so.6", "755a371e86cc9f564ab54f66140747700fb0a103f698f659f2060f707897e09a"},
    {"libdl.so.2", "4dbcca3c01ca84d092b5b7b2610f5835b55fc70a0288dd9d766b40ac9386e2d9"},
    {"libm.so.6", "5dcc4d25466e29ca390ed6fabb371c69b65dff6fa4e91c34561057a020d6994a"},
    {"libpthread.so.0", "cd34f895c9d2aa669d5d2654bed6b71210fc9003c3a1e96bb5311a40feb86039"},
    {"librt.so.1", "5a00236da5864e8d1123d74e616713110708d6e3e604188f4f256dab4c6941d4"},
    {"libresolv.so.2", "4dcb01645dbeea5d17f28fd96a730430a808d828b3b7d6a097cfbc311501f69f"},
    {"libutil.so.1", "0cf9051e58789348d681a984d8e6d96334610093289b783539764a3d611921d3"},
    {"libnss_files.so.2", "d8fce7c23c3b664871d4824950312f07e8a8f5203d638e61839e6d1ad20e45ab"},
    {"libnss_dns.so.2", "042b1959620a4974b344ba45a309f100bc6f22bffda957b276b46e8d51701f60"},
    {"COPYRIGHT", "d3c95b56fa33e28b57860580f0baf4e4f4de2a268a2b80f1d031a5191bade265"},
};
#endif
