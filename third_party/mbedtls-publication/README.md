# Publication SHA-256 provider

The selected files are copied byte-for-byte from Mbed TLS 3.6.6 archive SHA-256
`8fb65fae8dcae5840f793c0a334860a411f884cc537ea290ce1c52bb64ca007a`.
`LICENSE` contains its complete Apache-2.0 OR GPL-2.0-or-later license text.
`provenance.json` records exact repository-relative source/header/license hashes
and the authored provider/configuration hashes. No archive or generated object
is included.

Compile only `library/sha256.c` and `../../src/pw_publication_sha256.c` with:

```sh
-DMBEDTLS_CONFIG_FILE='"pw_publication_config.h"'
-Ithird_party/mbedtls-publication
-Ithird_party/mbedtls-publication/include
```

These flags also apply to every C/C++ caller of the stack context header. Use a
separate translation unit from a differently configured Mbed TLS consumer;
the header refuses a missing publication configuration. Vendor function symbols
are namespaced through the local config to avoid colliding with a full Mbed TLS
library linked elsewhere. No TLS, X.509, PSA, SIMD, platform allocation, or
firmware crypto is enabled.

The authored `MBEDTLS_PLATFORM_ZEROIZE_ALT` hook uses volatile byte stores.
Upstream `platform_util.c` is deliberately excluded: it also exports an allocator
helper not needed by this provider. The selected platform/error/configuration
headers are dependencies of the unmodified SHA source, not extra compiled modules.

From the repository root, the focused portable test is:

```sh
cc -std=c11 -O2 -Wall -Wextra -Werror \
  -DMBEDTLS_CONFIG_FILE='"pw_publication_config.h"' \
  -Ithird_party/mbedtls-publication -Ithird_party/mbedtls-publication/include \
  tests/test_pw_publication_sha256.c src/pw_publication_sha256.c \
  third_party/mbedtls-publication/library/sha256.c -o /tmp/pw-publication-sha-test
/tmp/pw-publication-sha-test
```

The provider enforces SHA-256's 64-bit bit-length bound before reading input.
Native file callers must additionally enforce their own bounded file lengths,
managed-path/regular-file checks and publication lifetime locking. This provider
does not implement file admission or a publication transaction.
