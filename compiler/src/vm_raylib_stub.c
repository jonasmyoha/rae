#include "vm_raylib.h"

/* Live is deprecated; keep default native registration structurally intact
 * without making the compiler binary depend on the legacy Raylib SDK. */
bool vm_registry_register_raylib(VmRegistry* registry) {
  (void)registry;
  return true;
}
