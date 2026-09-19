#include "shader_compose.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char* read_whole_file(Arena* arena, const char* path, size_t* out_len) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  struct stat st;
  if (fstat(fileno(f), &st) != 0 || !S_ISREG(st.st_mode)) { fclose(f); return NULL; }
  size_t len = (size_t)st.st_size;
  char* buf = arena_alloc(arena, len + 1);
  size_t got = fread(buf, 1, len, f);
  fclose(f);
  buf[got] = '\0';
  *out_len = got;
  return buf;
}

static bool file_is_regular(const char* path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

bool shader_compose(Arena* arena,
                    const char* declaring_file,
                    const char* project_root,
                    const char* stdlib_dir,
                    const char** paths,
                    size_t path_count,
                    ShaderComposition* out,
                    char* err,
                    size_t err_cap) {
  memset(out, 0, sizeof(*out));
  out->resolved = arena_alloc(arena, sizeof(const char*) * (path_count ? path_count : 1));
  out->part_start = arena_alloc(arena, sizeof(int) * (path_count ? path_count : 1));
  out->part_count = path_count;
  char** contents = arena_alloc(arena, sizeof(char*) * (path_count ? path_count : 1));
  size_t* lengths = arena_alloc(arena, sizeof(size_t) * (path_count ? path_count : 1));
  size_t total = 0;
  for (size_t i = 0; i < path_count; i++) {
    const char* p = paths[i];
    char tried[3][4096];
    int tried_count = 0;
    const char* found = NULL;
    if (p[0] == '/') {
      snprintf(tried[tried_count++], sizeof tried[0], "%s", p);
    } else {
      /* 1. Next to the file that declares the shader (an `assets/` part). */
      if (declaring_file && declaring_file[0]) {
        const char* slash = strrchr(declaring_file, '/');
        if (slash) snprintf(tried[tried_count++], sizeof tried[0], "%.*s/%s", (int)(slash - declaring_file), declaring_file, p);
        else snprintf(tried[tried_count++], sizeof tried[0], "%s", p);
      }
      /* 2. Against the project root (a project's own `lib/`). */
      if (project_root && project_root[0]) snprintf(tried[tried_count++], sizeof tried[0], "%s/%s", project_root, p);
      else if (tried_count == 0) snprintf(tried[tried_count++], sizeof tried[0], "%s", p);
      /* 3. A `lib/<rest>` part from the toolchain stdlib (that directory IS
       *    the stdlib `lib`, so the prefix is dropped when joining). */
      if (stdlib_dir && stdlib_dir[0] && strncmp(p, "lib/", 4) == 0)
        snprintf(tried[tried_count++], sizeof tried[0], "%s/%s", stdlib_dir, p + 4);
    }
    /* The declaring file's directory, the project root and the stdlib often
     * coincide (a project inside the toolchain tree): drop repeats. */
    for (int t = 1; t < tried_count; t++) {
      bool dup = false;
      for (int u = 0; u < t; u++) if (strcmp(tried[u], tried[t]) == 0) { dup = true; break; }
      if (dup) { for (int v = t; v + 1 < tried_count; v++) memcpy(tried[v], tried[v + 1], sizeof tried[0]); tried_count--; t--; }
    }
    for (int t = 0; t < tried_count && !found; t++)
      if (file_is_regular(tried[t])) found = tried[t];
    if (!found) {
      int n = snprintf(err, err_cap, "shader part '%s' not found (tried", p);
      for (int t = 0; t < tried_count && n < (int)err_cap; t++)
        n += snprintf(err + n, err_cap - (size_t)n, "%s '%s'", t ? (t + 1 == tried_count ? " and" : ",") : "", tried[t]);
      if (n < (int)err_cap) snprintf(err + n, err_cap - (size_t)n, ")");
      return false;
    }
    size_t len = 0;
    char* text = read_whole_file(arena, found, &len);
    if (!text) { snprintf(err, err_cap, "shader part '%s' could not be read ('%s')", p, found); return false; }
    size_t flen = strlen(found);
    char* keep = arena_alloc(arena, flen + 1);
    memcpy(keep, found, flen + 1);
    out->resolved[i] = keep;
    contents[i] = text;
    lengths[i] = len;
    total += strlen("// --- ") + strlen(p) + 1 + len + 1;
  }
  char* text = arena_alloc(arena, total + 1);
  size_t pos = 0;
  int line = 1;
  for (size_t i = 0; i < path_count; i++) {
    pos += (size_t)snprintf(text + pos, total + 1 - pos, "// --- %s\n", paths[i]);
    line++;  /* the marker line */
    out->part_start[i] = line;
    memcpy(text + pos, contents[i], lengths[i]);
    pos += lengths[i];
    for (size_t k = 0; k < lengths[i]; k++) if (contents[i][k] == '\n') line++;
    if (lengths[i] == 0 || contents[i][lengths[i] - 1] != '\n') { text[pos++] = '\n'; line++; }
  }
  text[pos] = '\0';
  out->text = text;
  out->text_len = pos;
  return true;
}

const char* shader_naga_path(void) {
  static char cached[4096];
  static int computed = 0;
  if (computed) return cached[0] ? cached : NULL;
  computed = 1;
  cached[0] = '\0';
  const char* env = getenv("RAE_NAGA");
  if (env && env[0] && access(env, X_OK) == 0) { snprintf(cached, sizeof cached, "%s", env); return cached; }
  const char* path = getenv("PATH");
  if (path) {
    const char* start = path;
    while (*start) {
      const char* end = strchr(start, ':');
      size_t n = end ? (size_t)(end - start) : strlen(start);
      if (n > 0 && n < 3900) {
        char probe[4096];
        snprintf(probe, sizeof probe, "%.*s/naga", (int)n, start);
        if (access(probe, X_OK) == 0) { snprintf(cached, sizeof cached, "%s", probe); return cached; }
      }
      if (!end) break;
      start = end + 1;
    }
  }
  const char* home = getenv("HOME");
  if (home && home[0]) {
    char probe[4096];
    snprintf(probe, sizeof probe, "%s/.cargo/bin/naga", home);
    if (access(probe, X_OK) == 0) { snprintf(cached, sizeof cached, "%s", probe); return cached; }
  }
  return NULL;
}

/* Translate a composed line to (part, line within the part). A hit on a
 * marker line is attributed to the part that follows it, line 1. */
static void map_line(const ShaderComposition* c, int composed_line, int* out_part, int* out_line) {
  *out_part = -1; *out_line = -1;
  for (size_t i = 0; i < c->part_count; i++) {
    int next_start = (i + 1 < c->part_count) ? c->part_start[i + 1] - 1 : 0x7fffffff;
    if (composed_line < next_start) {
      *out_part = (int)i;
      *out_line = composed_line - c->part_start[i] + 1;
      if (*out_line < 1) *out_line = 1;
      return;
    }
  }
}

ShaderValidation shader_validate(const ShaderComposition* composition,
                                 const char** part_names,
                                 char* err,
                                 size_t err_cap,
                                 int* out_part,
                                 int* out_line) {
  (void)part_names;
  *out_part = -1; *out_line = -1;
  err[0] = '\0';
  const char* mode = getenv("RAE_SHADER_VALIDATE");
  if (mode && strcmp(mode, "off") == 0) return SHADER_VALIDATE_OFF;
  const char* naga = shader_naga_path();
  if (!naga) return SHADER_NAGA_MISSING;

  char tmp_path[4096];
  const char* tmpdir = getenv("TMPDIR");
  if (!tmpdir || !tmpdir[0]) tmpdir = "/tmp";
  snprintf(tmp_path, sizeof tmp_path, "%s/rae_shader_%ld_XXXXXX", tmpdir, (long)getpid());
  int fd = mkstemp(tmp_path);
  if (fd < 0) { snprintf(err, err_cap, "could not create a temp file for naga in %s", tmpdir); return SHADER_INVALID; }
  FILE* tf = fdopen(fd, "wb");
  fwrite(composition->text, 1, composition->text_len, tf);
  fclose(tf);

  char cmd[8192];
  snprintf(cmd, sizeof cmd, "'%s' --input-kind wgsl '%s' 2>&1", naga, tmp_path);
  FILE* p = popen(cmd, "r");
  if (!p) { unlink(tmp_path); snprintf(err, err_cap, "could not run naga ('%s')", naga); return SHADER_INVALID; }
  char output[16384];
  size_t got = fread(output, 1, sizeof output - 1, p);
  output[got] = '\0';
  int status = pclose(p);
  unlink(tmp_path);
  if (status == 0) return SHADER_VALID;

  /* naga prints `error: <message>` then a codespan block whose first line is
   * `┌─ <file>:LINE:COL`. Keep the message, translate the location. */
  const char* msg = strstr(output, "error: ");
  if (msg) {
    msg += 7;
    const char* eol = strchr(msg, '\n');
    size_t n = eol ? (size_t)(eol - msg) : strlen(msg);
    if (n >= err_cap) n = err_cap - 1;
    memcpy(err, msg, n); err[n] = '\0';
  } else {
    size_t n = strlen(output);
    while (n > 0 && (output[n - 1] == '\n' || output[n - 1] == ' ')) n--;
    if (n >= err_cap) n = err_cap - 1;
    memcpy(err, output, n); err[n] = '\0';
  }
  const char* loc = strstr(output, tmp_path);
  if (loc) {
    loc += strlen(tmp_path);
    if (*loc == ':') {
      int composed_line = atoi(loc + 1);
      if (composed_line > 0) map_line(composition, composed_line, out_part, out_line);
    }
  }
  return SHADER_INVALID;
}
