/* Program arguments (#995): everything after the executable, as `main`
 * received them. The emitted `main` stores them here before the Rae entry runs;
 * lib/Sys.rae reads them through argCount() / argAt(). Included by rae_runtime.c
 * into the one runtime translation unit, like every other runtime_*.c.
 */

static int g_rae_arg_count = 0;
static char** g_rae_arg_values = NULL;

void rae_runtime_set_args(int argc, char** argv) {
  if (argc > 0 && argv) {
    g_rae_arg_count = argc - 1;
    g_rae_arg_values = argv + 1;
  } else {
    g_rae_arg_count = 0;
    g_rae_arg_values = NULL;
  }
}

int64_t rae_ext_rae_sys_arg_count(void) {
  return (int64_t)g_rae_arg_count;
}

rae_String rae_ext_rae_sys_arg_at(int64_t index) {
  if (index < 0 || index >= (int64_t)g_rae_arg_count || !g_rae_arg_values) {
    return rae_str_from_cstr_impl("", RAE_SITE_FROM_CSTR);
  }
  return rae_str_from_cstr_impl(g_rae_arg_values[index], RAE_SITE_FROM_CSTR);
}
