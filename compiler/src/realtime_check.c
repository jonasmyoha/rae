/* The `realtime` and `noAlloc` checks (docs/realtime-language-design.md §2,
 * §3). The may-wait walk (may_wait.c) records, per function, the realtime
 * effects it has itself: allocates, frees, locks, waits, each with its first
 * site. Here they flow up the call graph to a fixed point, and every function
 * marked `realtime` (forbids all four) or `noAlloc` (forbids allocates and
 * frees) is checked as a root: an effect it reaches is an error that names
 * the chain from the marked function to the site, and the fix.
 *
 * An extern has the effects it promises: none with `realtime`, locks and
 * waits with `noAlloc`, all four unmarked (§3.4). lib/core's misuse reports
 * (an index out of range) are the crash path and add nothing (§3.6). A spawn's
 * target runs elsewhere and is not an edge; the spawn itself allocates and
 * waits. `rae build --report-realtime` prints every marked function, whether
 * it is clean, and the C it trusts. */
#include "realtime_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "wait_graph.h"

static const char* const effect_verbs[4] = { "allocates", "frees", "locks", "waits" };

static const char* const effect_fixes[4] = {
    "Real-time code may not allocate: make it before the real-time code runs (in a prepare step), or send a "
    "number on a ring instead of text.",
    "Real-time code may not free: move the value out (into a ring or a field), or hand it back on a reclaim ring "
    "for another thread to free.",
    "Real-time code may not lock: use a SpscRing or an Atomic instead of a Channel or a mutex.",
    "Real-time code may not wait: post a Signal instead of waiting, and never join a task here.",
};

static void solve_effects(WaitGraph* graph) {
  for (size_t i = 0; i < graph->count; i++) {
    WaitNode* node = &graph->nodes[i];
    node->reached = node->effects;
    for (int bit = 0; bit < 4; bit++) node->effect_via[bit] = -1;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (size_t i = 0; i < graph->count; i++) {
      WaitNode* node = &graph->nodes[i];
      size_t total = node->callee_count + node->effect_callee_count;
      for (size_t c = 0; c < total; c++) {
        int target = c < node->callee_count ? node->callees[c] : node->effect_callees[c - node->callee_count];
        const WaitNode* callee = &graph->nodes[target];
        unsigned char gained = (unsigned char)(callee->reached & ~node->reached);
        if (!gained) continue;
        for (int bit = 0; bit < 4; bit++) {
          if (gained & (1 << bit)) node->effect_via[bit] = target;
        }
        node->reached |= gained;
        changed = true;
      }
    }
  }
}

static const char* where(const WaitGraph* graph, const AstDecl* decl) {
  return wait_graph_is_stdlib(graph, decl) ? wait_graph_short_lib_path(decl->origin_file)
                                           : wait_graph_file_name(decl->origin_file);
}

static bool is_root(const WaitNode* node) {
  const AstFuncDecl* func = &node->decl->as.func_decl;
  return !func->is_extern && (func->is_realtime || func->is_no_alloc);
}

static int forbidden(const WaitNode* node) {
  return node->decl->as.func_decl.is_realtime ? EFFECT_ALL : (EFFECT_ALLOCATES | EFFECT_FREES);
}

static void append(char* text, size_t cap, size_t* length, const char* format, const char* a, int b) {
  if (*length >= cap) return;
  int wrote = snprintf(text + *length, cap - *length, format, a, b);
  if (wrote > 0) *length += (size_t)wrote;
}

/* The leaf of `bit`'s chain from `index`, and the chain as text */
static int chain_of(const WaitGraph* graph, int index, int bit, char* text, size_t cap) {
  size_t length = 0;
  text[0] = '\0';
  for (int hops = 0; hops < 24; hops++) {
    const WaitNode* node = &graph->nodes[index];
    const AstFuncDecl* func = &node->decl->as.func_decl;
    char name[160];
    snprintf(name, sizeof name, "%.*s", (int)func->name.len, func->name.data);
    if (hops) append(text, cap, &length, " -> %s", name, 0);
    else append(text, cap, &length, "%s", name, 0);
    char place[256];
    snprintf(place, sizeof place, "%s", where(graph, node->decl));
    append(text, cap, &length, " (%s:%d)", place, (int)node->decl->line);
    if (node->effect_via[bit] < 0) return index;
    index = node->effect_via[bit];
  }
  append(text, cap, &length, "%s", " -> ...", 0);
  return index;
}

typedef struct {
  const AstDecl* root;
  const AstDecl* leaf;
  int bit;
} Reported;

bool realtime_check(CompilerContext* ctx, const AstModule* merged, struct WaitGraph* graph) {
  (void)ctx;
  (void)merged;
  if (!graph) return true;
  bool any = false;
  for (size_t i = 0; i < graph->count && !any; i++) any = is_root(&graph->nodes[i]);
  if (!any) return true;
  solve_effects(graph);
  Reported* reported = NULL;
  size_t reported_count = 0, reported_cap = 0;
  bool clean = true;
  for (size_t i = 0; i < graph->count; i++) {
    const WaitNode* root = &graph->nodes[i];
    if (!is_root(root)) continue;
    int bad = root->reached & forbidden(root);
    int done = 0;
    for (int bit = 0; bit < 4; bit++) {
      if (!(bad & (1 << bit)) || (done & (1 << bit))) continue;
      char chain[2048];
      int leaf_index = chain_of(graph, (int)i, bit, chain, sizeof chain);
      const WaitNode* leaf = &graph->nodes[leaf_index];
      /* the other effects with the same cause (the same site, or the same
       * unmarked C) go into the same message */
      int together = 1 << bit;
      for (int other = bit + 1; other < 4; other++) {
        if (!(bad & (1 << other)) || (done & (1 << other))) continue;
        char other_chain[2048];
        int other_leaf = chain_of(graph, (int)i, other, other_chain, sizeof other_chain);
        if (other_leaf == leaf_index && leaf->effect_line[other] == leaf->effect_line[bit] &&
            strcmp(other_chain, chain) == 0) {
          together |= 1 << other;
        }
      }
      done |= together;
      /* one error per (marked function, site, effects), whatever the
       * specialization */
      const AstDecl* root_decl = root->decl->as.func_decl.generic_template ? root->decl->as.func_decl.generic_template
                                                                           : root->decl;
      bool seen = false;
      for (size_t r = 0; r < reported_count && !seen; r++) {
        seen = reported[r].root == root_decl && reported[r].leaf == leaf->decl && reported[r].bit == together;
      }
      if (seen) continue;
      if (reported_count == reported_cap) {
        reported_cap = reported_cap ? reported_cap * 2 : 16;
        reported = realloc(reported, reported_cap * sizeof(Reported));
      }
      reported[reported_count++] = (Reported){ root_decl, leaf->decl, together };
      const AstFuncDecl* root_func = &root->decl->as.func_decl;
      const AstFuncDecl* leaf_func = &leaf->decl->as.func_decl;
      char what[256];
      if (leaf_func->is_extern) {
        snprintf(what, sizeof what, "it calls `%.*s`, %s", (int)leaf_func->name.len, leaf_func->name.data,
                 leaf->effect_what[bit] ? leaf->effect_what[bit] : "C");
      } else {
        snprintf(what, sizeof what, "%s", leaf->effect_what[bit] ? leaf->effect_what[bit] : "this line");
      }
      /* "allocates", "allocates and frees", "allocates, frees, locks and waits" */
      char verbs[128] = "";
      char fixes[1024] = "";
      int count = 0, total = 0;
      for (int k = 0; k < 4; k++) total += (together >> k) & 1;
      for (int k = 0; k < 4; k++) {
        if (!(together & (1 << k))) continue;
        count++;
        size_t length = strlen(verbs);
        snprintf(verbs + length, sizeof verbs - length, "%s%s", count == 1 ? "" : count == total ? " and " : ", ",
                 effect_verbs[k]);
        length = strlen(fixes);
        snprintf(fixes + length, sizeof fixes - length, "\n  %s", effect_fixes[k]);
      }
      char message[4096];
      snprintf(message, sizeof message, "`%.*s` is %s, but this %s: %s.\n    %s%s",
               (int)root_func->name.len, root_func->name.data, root_func->is_realtime ? "realtime" : "noAlloc", verbs,
               what, chain, fixes);
      diag_error(leaf->decl->origin_file, leaf->effect_line[bit], leaf->effect_column[bit], message);
      clean = false;
    }
  }
  free(reported);
  return clean;
}

/* The C a root reaches and trusts: externs marked realtime or noAlloc */
static void print_trusted(FILE* out, const WaitGraph* graph, int start) {
  bool* seen = calloc(graph->count, sizeof(bool));
  int* stack = malloc((graph->count + 1) * sizeof(int));
  size_t depth = 0, printed = 0;
  stack[depth++] = start;
  seen[start] = true;
  while (depth > 0) {
    const WaitNode* node = &graph->nodes[stack[--depth]];
    const AstFuncDecl* func = &node->decl->as.func_decl;
    if (func->is_extern && (func->is_realtime || func->is_no_alloc)) {
      fprintf(out, "%s%.*s", printed++ ? ", " : "      trusts: ", (int)func->name.len, func->name.data);
    }
    size_t total = node->callee_count + node->effect_callee_count;
    for (size_t c = 0; c < total; c++) {
      int target = c < node->callee_count ? node->callees[c] : node->effect_callees[c - node->callee_count];
      if (!seen[target]) {
        seen[target] = true;
        stack[depth++] = target;
      }
    }
  }
  if (printed) fprintf(out, "\n");
  free(seen);
  free(stack);
}

bool realtime_report(CompilerContext* ctx, const AstModule* merged, const char* entry, FILE* out) {
  WaitGraph graph;
  wait_graph_build(&graph, ctx, merged);
  solve_effects(&graph);
  fprintf(out, "realtime report: %s\n", wait_graph_file_name(entry));
  size_t roots = 0;
  for (int scope = 0; scope < 2; scope++) {
    for (size_t i = 0; i < graph.count; i++) {
      const WaitNode* node = &graph.nodes[i];
      if (!is_root(node) || (wait_graph_is_stdlib(&graph, node->decl) ? 1 : 0) != scope) continue;
      roots++;
      const AstFuncDecl* func = &node->decl->as.func_decl;
      fprintf(out, "  %s %.*s", func->is_realtime ? "realtime" : "noAlloc", (int)func->name.len, func->name.data);
      if (node->key) fprintf(out, "(%s)", node->key);
      fprintf(out, "  %s:%zu\n", where(&graph, node->decl), node->decl->line);
      int bad = node->reached & forbidden(node);
      if (!bad) {
        fprintf(out, "      clean\n");
      } else {
        for (int bit = 0; bit < 4; bit++) {
          if (!(bad & (1 << bit))) continue;
          char chain[2048];
          chain_of(&graph, (int)i, bit, chain, sizeof chain);
          fprintf(out, "      %s: %s\n", effect_verbs[bit], chain);
        }
      }
      print_trusted(out, &graph, (int)i);
    }
  }
  if (roots == 0) fprintf(out, "  no function is marked realtime or noAlloc\n");
  wait_graph_release(&graph);
  return true;
}
