/* rae_storekit.h — the C face of the storeKit package's Swift glue (RaeStoreKit.swift) over
 * StoreKit 2 (docs/store-kit.md). StoreKit's calls are async, so each one is a REQUEST: it starts
 * in the background and returns its id (> 0; 0 when it could not start), the caller polls it, then
 * copies its result (UTF-8 JSON in the shape of the package's Rae types) and releases it. No policy
 * lives here: what to sell, what an entitlement unlocks and how the server checks a JWS are the
 * app's. On a platform without StoreKit 2 (rae_storekit_stub.c) nothing starts and
 * rae_storekit_available is 0. */
#ifndef RAE_STOREKIT_H
#define RAE_STOREKIT_H
#include <stdint.h>

/* 1 where StoreKit 2 is there, else 0 */
int64_t rae_storekit_available(void);

/* Product.products(for:): `ids` are product ids separated by '\n' (`length` bytes, no NUL) */
int64_t rae_storekit_products(const uint8_t* ids, int64_t length);
/* product.purchase() for one product id */
int64_t rae_storekit_purchase(const uint8_t* productId, int64_t length);
/* Transaction.currentEntitlements */
int64_t rae_storekit_entitlements(void);
/* transaction.finish() for a transaction id this process has seen (or an unfinished one) */
int64_t rae_storekit_finish(int64_t transactionId);

/* 0 running, 1 done, -1 no such request */
int64_t rae_storekit_poll(int64_t request);
/* a done request's JSON: its length in bytes, then a copy of it into `into` */
int64_t rae_storekit_result_length(int64_t request);
void rae_storekit_result_copy(int64_t request, uint8_t* into);
/* forget a request and its result */
void rae_storekit_release(int64_t request);

/* Start listening to Transaction.updates (once; later calls do nothing); 1 listening, 0 not */
int64_t rae_storekit_listen(void);
/* The oldest transaction update not yet taken: its JSON length (0: none), then take it */
int64_t rae_storekit_update_length(void);
void rae_storekit_update_take(uint8_t* into);

#endif
