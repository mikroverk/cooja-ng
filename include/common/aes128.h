/*
 * aes128 — AES-128 block encryption (FIPS-197), for SoC models whose crypto
 * peripheral firmware relies on (nRF52840 ECB: 802.15.4 frame security).
 * Encryption only: ECB and CCM on these parts never run the inverse cipher.
 */
#ifndef CSIM_AES128_H
#define CSIM_AES128_H

#include <stdint.h>

/* Encrypt one 16-byte block: out = AES-128(key, in).  in and out may alias. */
void aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);

#endif
