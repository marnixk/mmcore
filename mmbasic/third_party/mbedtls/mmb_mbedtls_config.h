/*
 * mmbasic mbedTLS configuration for the bare-metal Circle TLS backend.
 *
 * Derived from mbedTLS 2.28 LTS. It keeps a TLS 1.2 client with X.509 (PEM
 * and DER) certificate parsing, hostname/usage verification, RSA and ECDSA
 * signatures, ECDHE/RSA key exchange, AES-GCM/CBC and SHA-1/256/512, and the
 * hardware-entropy + CTR-DRBG path. Everything that needs an operating system
 * (sockets, file I/O, POSIX timing, self-tests) is left off: the Circle
 * backend feeds entropy from the BCM2835 RNG and supplies its own socket
 * send/recv callbacks and RTC-derived wall clock.
 */
#ifndef MMB_MBEDTLS_CONFIG_H
#define MMB_MBEDTLS_CONFIG_H

/* Some 2.28 sources use INT_MAX/UINT_MAX without including limits.h. */
#include <limits.h>

/* ---- platform ------------------------------------------------------- */

#define MBEDTLS_PLATFORM_C
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE

/* No /dev/urandom on bare metal; the BCM2835/BCM2711 hardware RNG supplies
 * entropy through mbedtls_hardware_poll() in console/net_tls.cpp. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* ---- random and crypto ------------------------------------------------- */

#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C

#define MBEDTLS_BIGNUM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21

#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED

/* ---- ASN.1, OID, PEM/X.509 parsing ------------------------------------ */

#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PEM_PARSE_C

#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CHECK_KEY_USAGE
#define MBEDTLS_X509_CHECK_EXTENDED_KEY_USAGE
#define MBEDTLS_X509_RSASSA_PSS_SUPPORT

/* ---- TLS client ------------------------------------------------------- */

#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_ALL_ALERT_MESSAGES

#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

/* The X.509 time checks accept the RTC/NTP wall clock wired through time()
 * in the Circle libc shims; no timing/net/fs modules are pulled in. */

#endif /* MMB_MBEDTLS_CONFIG_H */
