#include "vm_tinyexpr.h"
#include "../../third_party/tinyexpr/rae_tinyexpr.h"
#include <stdio.h>

static bool native_tinyExprEval(struct VM* vm, VmNativeResult* out, const Value* args, size_t count, void* data) {
    (void)vm; (void)data;
    if (count != 1) return false;
    if (args[0].type != VAL_STRING) return false;
    
    int64_t result = tinyExprEval(args[0].as.string_value.chars);
    out->has_value = true;
    out->value = value_int(result);
    return true;
}

bool vm_registry_register_tinyexpr(VmRegistry* registry) {
    return vm_registry_register_native(registry, "tinyExprEval", native_tinyExprEval, NULL);
}
