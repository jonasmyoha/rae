/* The storeKit package where StoreKit 2 is not there: nothing starts (docs/store-kit.md). */
#include "rae_storekit.h"

int64_t rae_storekit_available(void) { return 0; }
int64_t rae_storekit_products(const uint8_t* ids, int64_t length) { (void)ids; (void)length; return 0; }
int64_t rae_storekit_purchase(const uint8_t* productId, int64_t length) { (void)productId; (void)length; return 0; }
int64_t rae_storekit_entitlements(void) { return 0; }
int64_t rae_storekit_finish(int64_t transactionId) { (void)transactionId; return 0; }
int64_t rae_storekit_poll(int64_t request) { (void)request; return -1; }
int64_t rae_storekit_result_length(int64_t request) { (void)request; return 0; }
void rae_storekit_result_copy(int64_t request, uint8_t* into) { (void)request; (void)into; }
void rae_storekit_release(int64_t request) { (void)request; }
int64_t rae_storekit_listen(void) { return 0; }
int64_t rae_storekit_update_length(void) { return 0; }
void rae_storekit_update_take(uint8_t* into) { (void)into; }
