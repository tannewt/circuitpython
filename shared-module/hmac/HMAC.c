// This file is part of the CircuitPython project: https://circuitpython.org
//
// SPDX-FileCopyrightText: Copyright (c) 2026 Mike Mabey
//
// SPDX-License-Identifier: MIT

#include <string.h>

#include "py/runtime.h"

#include "shared-bindings/hmac/HMAC.h"
#include "shared-module/hmac/__init__.h"

#include "psa/crypto.h"

#define HMAC_ALG(self) (PSA_ALG_HMAC((self)->hash_alg))

// On failure, resets mac_op (per the PSA multipart contract: an operation
// that errors out must be aborted before it can be discarded) and raises.
static void check_psa(hmac_hmac_obj_t *self, psa_status_t status) {
    if (status != PSA_SUCCESS) {
        psa_mac_abort(&self->mac_op);
        mp_raise_RuntimeError(NULL);
    }
}

void common_hal_hmac_new(hmac_hmac_obj_t *self, const uint8_t *key, size_t key_len,
    psa_key_id_t borrowed_key_id, psa_algorithm_t hash_alg) {
    self->hash_alg = hash_alg;
    self->finished = false;
    self->digest_len = 0;
    self->mac_op = psa_mac_operation_init();
    self->buffered = (borrowed_key_id != 0);
    if (self->buffered) {
        vstr_init(&self->buf, 0);
    }

    if (psa_crypto_init() != PSA_SUCCESS) {
        mp_raise_RuntimeError(NULL);
    }

    if (borrowed_key_id != 0) {
        self->key_id = borrowed_key_id;
        self->owns_key = false;
    } else {
        psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attr, PSA_KEY_TYPE_HMAC);
        psa_set_key_bits(&attr, key_len * 8);
        psa_set_key_algorithm(&attr, HMAC_ALG(self));
        psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_MESSAGE);
        psa_set_key_lifetime(&attr, PSA_KEY_LIFETIME_VOLATILE);
        if (psa_import_key(&attr, key, key_len, &self->key_id) != PSA_SUCCESS) {
            mp_raise_RuntimeError(NULL);
        }
        self->owns_key = true;
    }

    psa_status_t status = psa_mac_sign_setup(&self->mac_op, self->key_id, HMAC_ALG(self));
    if (status != PSA_SUCCESS) {
        if (self->owns_key) {
            psa_destroy_key(self->key_id);
            self->owns_key = false;
        }
        if (status == PSA_ERROR_NOT_PERMITTED) {
            // A hardware-backed key can be locked to one digest algorithm --
            // a common restriction for this kind of peripheral.
            mp_raise_ValueError(MP_ERROR_TEXT("key does not support this digest"));
        }
        mp_raise_RuntimeError(NULL);
    }
}

void common_hal_hmac_update(hmac_hmac_obj_t *self, const uint8_t *data, size_t data_len) {
    if (self->finished) {
        mp_raise_RuntimeError(NULL);
    }
    if (self->buffered) {
        vstr_add_strn(&self->buf, (const char *)data, data_len);
        return;
    }
    check_psa(self, psa_mac_update(&self->mac_op, data, data_len));
}

void common_hal_hmac_digest(hmac_hmac_obj_t *self, uint8_t *out, size_t out_len) {
    if (!self->finished) {
        if (self->buffered) {
            if (self->buf.len == 0) {
                // Some hardware-backed keys' PSA drivers reject a
                // zero-length psa_mac_update() outright (and skipping the
                // call entirely would leave the result at all-zero bytes
                // from setup -- a wrong answer, not an error), so an empty
                // message can't be computed through this path.
                psa_mac_abort(&self->mac_op);
                mp_raise_ValueError(MP_ERROR_TEXT("HMAC of an empty message is not supported with a hardware key"));
            }
            check_psa(self, psa_mac_update(&self->mac_op, (const uint8_t *)self->buf.buf, self->buf.len));
        }
        psa_status_t status = psa_mac_sign_finish(&self->mac_op, self->digest, sizeof(self->digest),
            &self->digest_len);
        // The operation is spent either way -- successful finish or not, it
        // can't be resumed, so the owned key's job is done too.
        if (self->owns_key) {
            psa_destroy_key(self->key_id);
            self->owns_key = false;
        }
        check_psa(self, status);
        self->finished = true;
    }
    memcpy(out, self->digest, out_len);
}

size_t common_hal_hmac_get_digest_size(hmac_hmac_obj_t *self) {
    return PSA_HASH_LENGTH(self->hash_alg);
}

size_t common_hal_hmac_get_block_size(hmac_hmac_obj_t *self) {
    return PSA_HASH_BLOCK_LENGTH(self->hash_alg);
}

const char *common_hal_hmac_get_name(hmac_hmac_obj_t *self) {
    if (self->hash_alg == PSA_ALG_SHA_1) {
        return "hmac-sha1";
    }
    return "hmac-sha256";
}
