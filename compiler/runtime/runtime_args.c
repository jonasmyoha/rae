/* Program arguments (#995): everything after the executable, as `main`
 * received them. The emitted `main` stores them here before the Rae entry runs;
 * lib/Sys.rae reads them through argCount() / argAt(). Included by rae_runtime.c
 * into the one runtime translation unit, like every other runtime_*.c.
 */

static int g_rae_arg_count = 0;
static char** g_rae_arg_values = NULL;

#ifndef __wasm__
/* An app started by `rae run` ends when the driver does. The driver forwards
 * a timeout / kill / hang-up to it, but a driver killed with SIGKILL cannot,
 * and macOS has no "signal me when my parent dies". So when the driver named
 * itself in RAE_RUN_PARENT_PID, a detached thread checks once a second that
 * it is still this process's parent (an orphan is re-parented to launchd /
 * init) and ends the app otherwise. Platform reason: getppid and a pthread;
 * it touches no shared state. A binary started any other way never starts it. */
static pid_t g_rae_run_parent = 0;

static void* rae_parent_watch(void* unused) {
  (void)unused;
  for (;;) {
    sleep(1);
    if (getppid() != g_rae_run_parent) _exit(143);
  }
  return NULL;
}

static void rae_start_parent_watch(void) {
  const char* text = getenv("RAE_RUN_PARENT_PID");
  if (!text || !text[0]) return;
  g_rae_run_parent = (pid_t)atoi(text);
  if (g_rae_run_parent <= 1 || getppid() != g_rae_run_parent) return;
  pthread_t thread;
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
  pthread_create(&thread, &attributes, rae_parent_watch, NULL);
  pthread_attr_destroy(&attributes);
}
#endif

void rae_runtime_set_args(int argc, char** argv) {
#ifndef __wasm__
  rae_start_parent_watch();
  /* The crash handler's "in <program>": argv[0]'s basename, a pointer into
   * argv that stays valid for the whole run. */
  if (argc > 0 && argv && argv[0] && argv[0][0] && strcmp(g_rae_program_name, "program") == 0) {
    const char* base = argv[0];
    for (const char* p = argv[0]; *p; p++) if (*p == '/') base = p + 1;
    g_rae_program_name = base;
  }
#endif
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
