#!/usr/bin/env bash
#
# gen_ca_roots.sh: regenerate include/ca_roots.h, the built-in TLS trust store.
#
# Run this when a relay endpoint moves to a CA that is not already trusted (the
# symptom is a handshake failing with MBEDTLS_ERR_X509_CERT_VERIFY_FAILED and a
# "NOT_TRUSTED" flag in the log), or when one of these roots is due to expire.
#
# To find out what a host actually chains to:
#
#   openssl s_client -connect relay.tesserae.ink:443 \
#       -servername relay.tesserae.ink </dev/null 2>&1 | grep -E '^ *[0-9] [si]:'
#
# The last issuer in that chain is the root to add below.
#
# Each root's SHA-256 fingerprint is checked against the value recorded here
# before it is written out. That check is the point of the script: a root
# fetched over a compromised connection and baked into firmware is a trust
# anchor for whoever supplied it, and nothing downstream would ever notice.
#
set -euo pipefail

here="$(cd "$(dirname "$0")/.." && pwd)"
out="$here/include/ca_roots.h"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# name|url|der|expected SHA-256 fingerprint (colon-separated, uppercase)
roots=(
"GTS Root R4|https://i.pki.goog/r4.crt|yes|34:9D:FA:40:58:C5:E2:63:12:3B:39:8A:E7:95:57:3C:4E:13:13:C8:3F:E6:8F:93:55:6C:D5:E8:03:1B:3C:7D"
"GTS Root R1|https://i.pki.goog/r1.crt|yes|D9:47:43:2A:BD:E7:B7:FA:90:FC:2E:6B:59:10:1B:12:80:E0:E1:C7:E4:E4:0F:A3:C6:88:7F:FF:57:A7:F4:CF"
"ISRG Root X1|https://letsencrypt.org/certs/isrgrootx1.pem|no|96:BC:EC:06:26:49:76:F3:74:60:77:9A:CF:28:C5:A7:CF:E8:A3:C0:AA:E1:1A:8F:FC:EE:05:C0:BD:DF:08:C6"
)

i=0
for entry in "${roots[@]}"; do
    IFS='|' read -r name url der want <<<"$entry"
    raw="$work/$i.raw"; pem="$work/$i.pem"
    curl -fsSL -o "$raw" "$url"
    if [ "$der" = yes ]; then
        openssl x509 -inform DER -in "$raw" -outform PEM -out "$pem"
    else
        cp "$raw" "$pem"
    fi
    got="$(openssl x509 -in "$pem" -noout -fingerprint -sha256 | cut -d= -f2)"
    if [ "$got" != "$want" ]; then
        echo "FINGERPRINT MISMATCH for $name" >&2
        echo "  expected $want" >&2
        echo "  got      $got" >&2
        echo "Refusing to write a root we cannot vouch for." >&2
        exit 1
    fi
    echo "ok: $name"
    printf '%s\n' "$name" > "$work/$i.name"
    printf '%s\n' "$want" > "$work/$i.fp"
    i=$((i + 1))
done

{
cat <<'EOF'
/*
 * ca_roots.h: the built-in TLS trust store.
 *
 * GENERATED -- see the fingerprints below and scripts/gen_ca_roots.sh.
 *
 * This device makes outbound HTTPS connections to exactly one class of host:
 * a Tesserae cloud relay. It therefore ships a handful of roots rather than a
 * general web CA bundle, which would cost ~250 KB of flash and ~150 KB of RAM
 * to parse for no benefit -- and would mean any of ~150 CAs could impersonate
 * the relay.
 *
 * Which roots, and why each one:
 *
 *   - GTS Root R4 is what actually signs relay.tesserae.ink today
 *     (leaf -> "GTS WE1" -> GTS Root R4, verified against the live endpoint).
 *   - GTS Root R1 is Google's RSA root; GTS issues from both hierarchies and
 *     moves certificates between them without notice, so pinning only the EC
 *     root would turn a routine reissue into a dead fleet.
 *   - ISRG Root X1 is Let's Encrypt, which is what a self-hosted relay Worker
 *     on a custom domain will almost always present.
 *
 * A relay behind some other CA needs its root added here and a reflash. That is
 * a real constraint, and the deliberate trade: the alternative is trusting
 * everyone. Note that the frames themselves do not depend on this -- they are
 * AES-256-GCM sealed under a key the relay never sees (relay_crypto.h) -- so
 * TLS here protects the pairing exchange and the device token, not the pixels.
 */
#pragma once

/* One NUL-terminated buffer of concatenated PEM. mbedtls_x509_crt_parse() wants
 * the length INCLUDING the terminator when parsing PEM, which is why callers
 * use sizeof rather than strlen. */
static const char k_ca_roots_pem[] =
EOF

for n in $(seq 0 $((i - 1))); do
    printf '    /* %s\n     * SHA-256 %s */\n' "$(cat "$work/$n.name")" "$(cat "$work/$n.fp")"
    sed 's/.*/    "&\\n"/' "$work/$n.pem"
    printf '\n'
done
printf ';\n'
} > "$out"

echo "wrote $out"
