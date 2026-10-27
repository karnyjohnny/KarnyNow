/* kn_mbedtls_user_config.h — the ADDITIVE user configuration for the
 * vendored mbedTLS 3.6.0 (the MBEDTLS_USER_CONFIG_FILE mechanism — the
 * pinned vendor tree stays untouched, the sha1.c discipline).
 *
 * ENABLED HERE:
 *   MBEDTLS_SSL_DTLS_SRTP — the RFC 5764 use_srtp extension (round
 *   XXXV, deviation D-t9). The reference client's ClientHello carries
 *   it: str0m's DTLS emits KeyingMaterial + SrtpProfile after the
 *   handshake (vendor str0m src/lib.rs:1648-1658 → session.set_keying_
 *   material) and its offer list is [AEAD_AES_256_GCM (0x0008),
 *   AEAD_AES_128_GCM (0x0007), AES128_CM_SHA1_80 (0x0001)]
 *   (crypto/srtp.rs SrtpProfile::ALL) — the bundle's audio
 *   (rtcAudioOnNativeBundle:1, which our own ANNOUNCE mirrors) takes
 *   its SRTP keys from the DTLS negotiation. mbedTLS 3.6 implements
 *   only the CM profiles, so kn_dtls offers 0x0001 — the INTERSECTION
 *   with the reference list and the mandatory WebRTC profile. Per RFC
 *   5764 a server without a common profile simply omits the extension
 *   from its answer — the handshake still completes (pinned by the
 *   test_kndtls negative leg), so this can never regress runs 13-15.
 *
 * ABI NOTE: this define changes the layout of mbedtls_ssl_context /
 * mbedtls_ssl_config — it MUST be set for EVERY translation unit that
 * includes mbedtls/ssl.h (CMake: PUBLIC on the mbedtls target, so all
 * consumers inherit; shell-gate manual legs: the -D flag is part of
 * each leg; ad-hoc sanitizer builds: same flag — see INTEGRATION.md
 * rev 25 notes).
 */
#ifndef KN_MBEDTLS_USER_CONFIG_H
#define KN_MBEDTLS_USER_CONFIG_H

#define MBEDTLS_SSL_DTLS_SRTP

#endif /* KN_MBEDTLS_USER_CONFIG_H */
