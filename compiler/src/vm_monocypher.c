#include "vm_registry.h"
#include "vm_value.h"
#include "../../third_party/monocypher/monocypher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper to get buffer data safely
static bool get_buffer_view(const Value* val, uint8_t** data, size_t* size) {
    if (val->type != VAL_BUFFER || !val->as.buffer_value) return false;
    *size = val->as.buffer_value->count;
    *data = calloc(1, *size);
    if (!*data) return false;
    for (size_t i = 0; i < *size; i++) {
        Value item = val->as.buffer_value->items[i];
        if (item.type == VAL_INT) (*data)[i] = (uint8_t)item.as.int_value;
        else (*data)[i] = 0;
    }
    return true;
}

static void fill_buffer_from_bytes(Value* val_buf, const uint8_t* data, size_t size) {
    if (!val_buf || val_buf->type != VAL_BUFFER || !val_buf->as.buffer_value) return;
    size_t count = val_buf->as.buffer_value->count;
    if (size < count) count = size;
    for (size_t i = 0; i < count; i++) {
        value_free(&val_buf->as.buffer_value->items[i]);
        val_buf->as.buffer_value->items[i] = value_int(data[i]);
    }
}

bool native_crypto_argon2i(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 6) return false; // password, salt, nb_blocks, nb_iterations, hash_buf, hash_len
    
    if (args[0].type != VAL_STRING) return false;
    const char* password = args[0].as.string_value.chars;
    size_t password_len = args[0].as.string_value.length;
    
    if (args[1].type != VAL_STRING) return false;
    const char* salt = args[1].as.string_value.chars;
    size_t salt_len = args[1].as.string_value.length;
    
    if (args[2].type != VAL_INT) return false;
    uint32_t nb_blocks = (uint32_t)args[2].as.int_value;
    
    if (args[3].type != VAL_INT) return false;
    uint32_t nb_iterations = (uint32_t)args[3].as.int_value;
    
    Value hash_buf = args[4];
    while (hash_buf.type == VAL_REF) hash_buf = *hash_buf.as.ref_value.target;
    if (hash_buf.type != VAL_BUFFER) return false;
    
    if (args[5].type != VAL_INT) return false;
    size_t hash_len = (size_t)args[5].as.int_value;
    
    uint8_t* hash_out = malloc(hash_len ? hash_len : 1);
    void* work_area = malloc(nb_blocks * 1024);
    if (!work_area) { free(hash_out); return false; }
    
    crypto_argon2(hash_out, (uint32_t)hash_len, 
                   work_area,
                   (crypto_argon2_config){CRYPTO_ARGON2_I, nb_blocks, nb_iterations, 1},
                   (crypto_argon2_inputs){(const uint8_t*)password, (const uint8_t*)salt, (uint32_t)password_len, (uint32_t)salt_len},
                   crypto_argon2_no_extras);
                   
    fill_buffer_from_bytes(&hash_buf, hash_out, hash_len);
    
    free(work_area);
    free(hash_out);
    
    out->has_value = false;
    return true;
}

bool native_crypto_lock(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    // key(32), nonce(24), plain(buf), plain_len, mac(16), cipher(buf)
    if (count != 6) return false;
    
    if (args[0].type != VAL_BUFFER || args[0].as.buffer_value->count != 32) return false;
    uint8_t* key = NULL; size_t key_len;
    if (!get_buffer_view(&args[0], &key, &key_len)) return false;
    
    if (args[1].type != VAL_BUFFER || args[1].as.buffer_value->count != 24) { free(key); return false; }
    uint8_t* nonce = NULL; size_t nonce_len;
    if (!get_buffer_view(&args[1], &nonce, &nonce_len)) { free(key); return false; }
    
    Value plain_val = args[2];
    while (plain_val.type == VAL_REF) plain_val = *plain_val.as.ref_value.target;
    uint8_t* plain = NULL; size_t plain_view_len;
    if (!get_buffer_view(&plain_val, &plain, &plain_view_len)) { free(key); free(nonce); return false; }
    
    if (args[3].type != VAL_INT) { free(key); free(nonce); free(plain); return false; }
    size_t plain_len = (size_t)args[3].as.int_value;

    Value mac_val = args[4];
    while (mac_val.type == VAL_REF) mac_val = *mac_val.as.ref_value.target;
    if (mac_val.type != VAL_BUFFER || mac_val.as.buffer_value->count != 16) {
        free(key); free(nonce); free(plain); return false;
    }
    
    Value cipher_val = args[5];
    while (cipher_val.type == VAL_REF) cipher_val = *cipher_val.as.ref_value.target;
    if (cipher_val.type != VAL_BUFFER || cipher_val.as.buffer_value->count < plain_len) {
        free(key); free(nonce); free(plain); return false;
    }
    
    uint8_t mac[16];
    uint8_t* cipher = malloc(plain_len ? plain_len : 1);
    
    crypto_aead_lock(cipher, mac, key, nonce, NULL, 0, plain, plain_len);
    
    fill_buffer_from_bytes(&mac_val, mac, 16);
    fill_buffer_from_bytes(&cipher_val, cipher, plain_len);
    
    free(key); free(nonce); free(plain); free(cipher);
    out->has_value = false;
    return true;
}

bool native_crypto_unlock(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    // key(32), nonce(24), mac(16), cipher(buf), cipher_len, plain(buf)
    if (count != 6) return false;
    
    uint8_t* key = NULL; size_t key_len;
    if (!get_buffer_view(&args[0], &key, &key_len)) return false;
    
    uint8_t* nonce = NULL; size_t nonce_len;
    if (!get_buffer_view(&args[1], &nonce, &nonce_len)) { free(key); return false; }
    
    uint8_t* mac = NULL; size_t mac_len;
    if (!get_buffer_view(&args[2], &mac, &mac_len)) { free(key); free(nonce); return false; }
    
    Value cipher_val = args[3];
    while (cipher_val.type == VAL_REF) cipher_val = *cipher_val.as.ref_value.target;
    uint8_t* cipher = NULL; size_t cipher_view_len;
    if (!get_buffer_view(&cipher_val, &cipher, &cipher_view_len)) { free(key); free(nonce); free(mac); return false; }
    
    if (args[4].type != VAL_INT) { free(key); free(nonce); free(mac); free(cipher); return false; }
    size_t cipher_len = (size_t)args[4].as.int_value;

    Value plain_val = args[5];
    while (plain_val.type == VAL_REF) plain_val = *plain_val.as.ref_value.target;
    
    uint8_t* plain = malloc(cipher_len ? cipher_len : 1);
    
    if (crypto_aead_unlock(plain, mac, key, nonce, NULL, 0, cipher, cipher_len) != 0) {
        free(key); free(nonce); free(mac); free(cipher); free(plain);
        out->has_value = true;
        out->value = value_int(-1);
        return true;
    }
    
    fill_buffer_from_bytes(&plain_val, plain, cipher_len);
    
    free(key); free(nonce); free(mac); free(cipher); free(plain);
    out->has_value = true;
    out->value = value_int(0);
    return true;
}

bool vm_registry_register_monocypher(VmRegistry* registry) {
    bool ok = true;
    ok &= vm_registry_register_native(registry, "rae_crypto_argon2i", native_crypto_argon2i, NULL);
    ok &= vm_registry_register_native(registry, "rae_crypto_lock", native_crypto_lock, NULL);
    ok &= vm_registry_register_native(registry, "rae_crypto_unlock", native_crypto_unlock, NULL);
    return ok;
}
