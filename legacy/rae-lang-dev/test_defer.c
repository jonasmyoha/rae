#include "rae_runtime.h"

int main(void) {
  rae_ext_rae_log_cstr("Opening database...");
  rae_ext_rae_log_cstr("Begin transaction...");
  int8_t success = 1;
  if (success) {
  rae_ext_rae_log_cstr("Processing data...");
  /* is_main: 1 */
  rae_ext_rae_log_cstr("End transaction (cleanup).");
  rae_ext_rae_log_cstr("Closing database connection.");
  return 0;
  }
  rae_ext_rae_log_cstr("This part is skipped because of 'ret' above.");
  rae_ext_rae_log_cstr("End transaction (cleanup).");
  rae_ext_rae_log_cstr("Closing database connection.");
  return 0;
}

