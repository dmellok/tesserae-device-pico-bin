#
# mbedtls.py: build the Pico SDK's bundled mbedTLS plus lwIP's altcp_tls layer.
#
# The picosdk framework puts pico_mbedtls's include dir on the path but never
# compiles mbedTLS itself, nor lwIP's altcp_tls glue (the SDK does that from
# CMake, which PlatformIO does not run). This post-script builds the three
# pieces the firmware needs:
#
#   1. mbedTLS library sources -- only the files our mbedtls_config.h can
#      actually use. The upstream CMake list includes the whole PSA stack and
#      every cipher; compiling those against a trimmed config is wasted build
#      time at best, so the list here tracks include/mbedtls_config.h. If you
#      turn a feature on there and the link fails, the missing .c belongs here.
#   2. pico_mbedtls.c -- the SDK's MBEDTLS_ENTROPY_HARDWARE_ALT hook, which
#      feeds mbedTLS from the RP2350 hardware RNG.
#   3. lwIP's altcp_tls_mbedtls{,_mem}.c -- the altcp-to-mbedTLS bridge. The SDK
#      ships its own copy of altcp_tls_mbedtls.c (patched for mbedTLS 3.x), so
#      that one is used in preference to lwIP's; the _mem half is unchanged and
#      comes from lwIP.
#
# Config selection: mbedTLS looks for MBEDTLS_CONFIG_FILE, and the SDK's
# pico_mbedtls_config.h just includes "mbedtls_config.h" -- which resolves to
# ours because lwip_prepend.py has already put the project include dir first.
#
Import("env")
from os.path import join

# A post-script runs after PlatformIO has already derived `projenv` (the
# environment that compiles src/) from `env`, so appending include paths to
# `env` alone reaches the libraries built here but NOT the firmware's own
# sources -- which is how you get "mbedtls/ssl.h: No such file" on net_tls.c
# while mbedTLS itself compiles perfectly.
try:
    Import("projenv")
except Exception:
    projenv = None

platform = env.PioPlatform()
fw = platform.get_package_dir("framework-picosdk")

MBEDTLS = join(fw, "lib", "mbedtls")
LWIP = join(fw, "lib", "lwip", "src")
PICO_MBEDTLS = join(fw, "src", "rp2_common", "pico_mbedtls")
PICO_LWIP = join(fw, "src", "rp2_common", "pico_lwip")

TLS_CPPPATH = [
    join(MBEDTLS, "include"),
    join(MBEDTLS, "library"),        # ssl_misc.h and friends, for the glue
    join(PICO_MBEDTLS, "include"),   # pico_mbedtls_config.h, sha256_alt.h
    join(LWIP, "apps", "altcp_tls"), # altcp_tls_mbedtls_structs.h / _mem.h
]
TLS_CPPDEFINES = [("MBEDTLS_CONFIG_FILE", '\\"pico_mbedtls_config.h\\"')]

for e in (env, projenv):
    if e is not None:
        e.Append(CPPPATH=TLS_CPPPATH, CPPDEFINES=TLS_CPPDEFINES)

# Keep this list in step with include/mbedtls_config.h. Ordered roughly the way
# that file is: platform, hashes, ciphers, public key, RNG, X.509, TLS.
MBEDTLS_SRC = [
    # platform / util
    "platform.c", "platform_util.c", "constant_time.c", "error.c",
    "version.c", "version_features.c", "threading.c",
    # hashes
    "md.c", "sha256.c", "sha512.c", "hkdf.c",
    # ciphers
    "aes.c", "gcm.c", "cipher.c", "cipher_wrap.c",
    # public key
    "bignum.c", "bignum_core.c", "asn1parse.c", "asn1write.c", "oid.c",
    "pk.c", "pk_wrap.c", "pk_ecc.c", "pkparse.c", "pkwrite.c",
    "rsa.c", "rsa_alt_helpers.c",
    "ecp.c", "ecp_curves.c", "ecdh.c", "ecdsa.c",
    # RNG
    "entropy.c", "entropy_poll.c", "ctr_drbg.c", "hmac_drbg.c",
    # X.509
    "base64.c", "pem.c", "x509.c", "x509_crt.c",
    # TLS
    "ssl_ciphersuites.c", "ssl_client.c", "ssl_msg.c", "ssl_tls.c",
    "ssl_tls12_client.c", "ssl_debug_helpers_generated.c", "debug.c",
]

# Vendored third-party code: build it in a cloned env so its warnings cannot
# drown out ours. mbedTLS trips -Wcast-qual all over (mbedtls_get_unaligned_*
# and friends) and upstream's own CMake suppresses exactly these for exactly
# these files, so a warning here is noise rather than a signal.
vendor_env = env.Clone()
vendor_env.Append(
    CCFLAGS=["-Wno-cast-qual", "-Wno-null-dereference", "-Wno-unused-parameter"]
)

# lwIP's altcp_tls glue reaches into mbedTLS internals (ssl->out_left,
# session->start) that 3.x made private. Upstream's own CMake grants this, and
# there is no public equivalent for what the glue needs.
#
# Scoped to the glue alone, for two reasons: our own sources must stay on public
# APIs, and mbedTLS's library/common.h defines this symbol itself, so setting it
# on the mbedTLS build produces a redefinition warning on every file.
altcp_env = vendor_env.Clone()
altcp_env.Append(CPPDEFINES=["MBEDTLS_ALLOW_PRIVATE_ACCESS"])

mbedtls_lib = vendor_env.BuildLibrary(
    join("$BUILD_DIR", "MbedTLS"),
    join(MBEDTLS, "library"),
    "-<*> " + " ".join("+<%s>" % f for f in MBEDTLS_SRC),
)

pico_mbedtls_lib = vendor_env.BuildLibrary(
    join("$BUILD_DIR", "PicoMbedTLS"), PICO_MBEDTLS, "-<*> +<pico_mbedtls.c>"
)

# The SDK's altcp_tls_mbedtls.c supersedes lwIP's (mbedTLS 3.x fixes); the _mem
# half is only in lwIP. Both are needed and they resolve each other's symbols.
altcp_sdk_lib = altcp_env.BuildLibrary(
    join("$BUILD_DIR", "AltcpTlsSdk"), PICO_LWIP, "-<*> +<altcp_tls_mbedtls.c>"
)
altcp_mem_lib = altcp_env.BuildLibrary(
    join("$BUILD_DIR", "AltcpTlsMem"),
    join(LWIP, "apps", "altcp_tls"),
    "-<*> +<altcp_tls_mbedtls_mem.c>",
)

env.Append(LIBS=[altcp_sdk_lib, altcp_mem_lib, pico_mbedtls_lib, mbedtls_lib])
