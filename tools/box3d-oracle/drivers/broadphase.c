/* Box3D oracle driver: the broad-phase data structures (docs/physics-rae-port-design.md
 * §4, P3): the dynamic tree (dynamic_tree.c) driven through a scripted
 * insert / move / enlarge / destroy / mark / refit / rebuild sequence with
 * its full state dumped at checkpoints and queries in between, the pair hash
 * set (table.c), the bit set (bitset.c), the id pool (id_pool.c) and the
 * quicksort (qsort.h).
 *
 * One line per operation, `<op> <inputs> -> <outputs>`, replayed in order by
 * fixture 953: floats as C hex floats (`%a`), integers as decimals, a
 * uint64 as two decimals (high 32 bits, low 32 bits). Compound values:
 *
 *   box    lower(3) upper(3)
 *   state  nodeEnd pairFreeList proxyCount proxyCapacity proxyFreeList
 *          dfsOrdered height areaRatio rootBounds(box), then per node below
 *          nodeEnd: box flagIndex height, then per node below nodeEnd its
 *          parent, then per proxy slot: node next categoryBits userData
 *   stats  nodeVisits leafVisits
 *
 * The query callbacks are fixed and written out in the fixture:
 *   query    collect proxy ids, stop after `stopAfter` (0: never)
 *   raycast  b3RayCastAABB of the proxy box against the current segment:
 *            a miss returns -1, a hit its entry fraction (scaled to the
 *            input translation); records proxyId and the maxFraction passed
 *   boxcast  the same against the proxy box grown by the cast box's extents
 *            from the cast box center
 *   closest  the squared distance to the proxy box center; records proxyId
 *            and the minimum passed
 *
 * Compiled against the cached Box3D checkout by tools/box3d-oracle/oracle.sh,
 * never by a Rae build. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "aabb.h"
#include "bitset.h"
#include "dynamic_tree.h"
#include "id_pool.h"
#include "qsort.h"
#include "table.h"

#include "box3d/collision.h"
#include "box3d/math_functions.h"

static uint32_t g_state = 0xa54ff53au;

static uint32_t next_u32(void) {
  uint32_t x = g_state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  g_state = x;
  return x;
}

static float next_float(float range) {
  int steps = (int)(range * 256.0f);
  int k = (int)(next_u32() % (uint32_t)(2 * steps + 1)) - steps;
  return (float)k / 256.0f;
}

static int next_int(int count) { return (int)(next_u32() % (uint32_t)count); }

static b3Vec3 next_vec(float range) {
  b3Vec3 v = { next_float(range), next_float(range), next_float(range) };
  return v;
}

static uint64_t next_u64(void) { return ((uint64_t)next_u32() << 32) | next_u32(); }

static void f(float x) { printf(" %a", (double)x); }
static void i(long long x) { printf(" %lld", x); }
static void u64(uint64_t x) { printf(" %u %u", (unsigned)(x >> 32), (unsigned)(x & 0xffffffffu)); }
static void v3(b3Vec3 v) { f(v.x); f(v.y); f(v.z); }
static void box(b3AABB a) { v3(a.lowerBound); v3(a.upperBound); }
static void stats(b3TreeStats s) { i(s.nodeVisits); i(s.leafVisits); }

static void name(const char* n) { printf("%s", n); }
static void arrow(void) { printf(" ->"); }
static void end(void) { printf("\n"); }

static b3AABB next_box(float range, float size) {
  b3Vec3 c = next_vec(range);
  b3Vec3 h = { 0.125f + (float)next_int(16) * size, 0.125f + (float)next_int(16) * size, 0.125f + (float)next_int(16) * size };
  b3AABB b = { b3Sub(c, h), b3Add(c, h) };
  return b;
}

static uint64_t next_category(void) {
  switch (next_int(4)) {
    case 0: return 1ull;
    case 1: return 1ull << next_int(64);
    case 2: return next_u64();
    default: return 0x00000000ffffffffull;
  }
}

static void state(const b3DynamicTree* t) {
  name("tree.state"); arrow();
  i(t->nodeEnd); i(t->pairFreeList); i(t->proxyCount); i(t->proxyCapacity); i(t->proxyFreeList); i(t->dfsOrdered);
  i(b3DynamicTree_GetHeight(t)); f(b3DynamicTree_GetAreaRatio(t)); box(b3DynamicTree_GetRootBounds(t));
  for (int k = 0; k < t->nodeEnd; k++) { box(t->nodes[k].aabb); i(t->nodes[k].flagIndex); i(t->nodes[k].height); }
  for (int k = 0; k < t->nodeEnd; k++) i(t->parents[k]);
  for (int k = 0; k < t->proxyCapacity; k++) {
    i(t->proxies[k].node); i(t->proxies[k].next); u64(t->proxies[k].categoryBits); u64(t->proxies[k].userData);
  }
  end();
}

/* Query callbacks. */
typedef struct Recorder {
  const b3DynamicTree* tree;
  int stopAfter;
  int count;
  int ids[512];
  float passed[512];
  float values[512];
  b3Vec3 point;
  b3Vec3 extension;
} Recorder;

static bool queryCallback(int proxyId, uint64_t userData, void* context) {
  (void)userData;
  Recorder* r = context;
  r->ids[r->count++] = proxyId;
  return r->stopAfter == 0 || r->count < r->stopAfter;
}

static float rayCallback(const b3RayCastInput* input, int proxyId, uint64_t userData, void* context) {
  (void)userData;
  Recorder* r = context;
  b3AABB target = b3DynamicTree_GetAABB(r->tree, proxyId);
  target.lowerBound = b3Sub(target.lowerBound, r->extension);
  target.upperBound = b3Add(target.upperBound, r->extension);
  b3Vec3 p2 = b3MulAdd(input->origin, input->maxFraction, input->translation);
  float minFraction = -1.0f, maxFraction = -1.0f;
  float value = -1.0f;
  if (b3RayCastAABB(target, input->origin, p2, &minFraction, &maxFraction)) value = minFraction * input->maxFraction;
  r->ids[r->count] = proxyId; r->passed[r->count] = input->maxFraction; r->values[r->count] = value; r->count++;
  return value;
}

static float boxCallback(const b3BoxCastInput* input, int proxyId, uint64_t userData, void* context) {
  b3RayCastInput ray = { b3AABB_Center(input->box), input->translation, input->maxFraction };
  return rayCallback(&ray, proxyId, userData, context);
}

static float closestCallback(float minDistanceSqr, int proxyId, uint64_t userData, void* context) {
  (void)userData;
  Recorder* r = context;
  float dd = b3DistanceSquared(r->point, b3AABB_Center(b3DynamicTree_GetAABB(r->tree, proxyId)));
  r->ids[r->count] = proxyId; r->passed[r->count] = minDistanceSqr; r->values[r->count] = dd; r->count++;
  return dd;
}

static void queries(const b3DynamicTree* t, int n) {
  for (int k = 0; k < n; k++) {
    Recorder r; memset(&r, 0, sizeof r); r.tree = t;
    b3AABB q = next_box(20.0f, 0.5f);
    uint64_t mask = next_int(3) == 0 ? next_category() : ~0ull;
    bool requireAll = next_int(4) == 0;
    r.stopAfter = next_int(3) == 0 ? 1 + next_int(4) : 0;
    b3TreeStats s = b3DynamicTree_Query(t, q, mask, requireAll, queryCallback, &r);
    name("tree.query"); box(q); u64(mask); i(requireAll); i(r.stopAfter); arrow(); stats(s); i(r.count);
    for (int n2 = 0; n2 < r.count; n2++) i(r.ids[n2]);
    end();
  }
  for (int k = 0; k < n; k++) {
    Recorder r; memset(&r, 0, sizeof r); r.tree = t;
    b3RayCastInput ray = { next_vec(25.0f), b3Vec3_zero, next_int(4) == 0 ? 0.5f : 1.0f };
    ray.translation = b3MulSV(1.5f, b3Sub(next_vec(4.0f), ray.origin));
    uint64_t mask = next_int(3) == 0 ? next_category() : ~0ull;
    bool requireAll = next_int(4) == 0;
    b3TreeStats s = b3DynamicTree_RayCast(t, &ray, mask, requireAll, rayCallback, &r);
    name("tree.raycast"); v3(ray.origin); v3(ray.translation); f(ray.maxFraction); u64(mask); i(requireAll); arrow();
    stats(s); i(r.count);
    for (int n2 = 0; n2 < r.count; n2++) { i(r.ids[n2]); f(r.passed[n2]); f(r.values[n2]); }
    end();
  }
  for (int k = 0; k < n; k++) {
    Recorder r; memset(&r, 0, sizeof r); r.tree = t;
    b3BoxCastInput cast = { next_box(25.0f, 0.125f), b3Vec3_zero, next_int(4) == 0 ? 0.5f : 1.0f };
    cast.translation = b3MulSV(1.5f, b3Sub(next_vec(4.0f), b3AABB_Center(cast.box)));
    r.extension = b3AABB_Extents(cast.box);
    uint64_t mask = ~0ull;
    b3TreeStats s = b3DynamicTree_BoxCast(t, &cast, mask, false, boxCallback, &r);
    name("tree.boxcast"); box(cast.box); v3(cast.translation); f(cast.maxFraction); arrow(); stats(s); i(r.count);
    for (int n2 = 0; n2 < r.count; n2++) { i(r.ids[n2]); f(r.passed[n2]); f(r.values[n2]); }
    end();
  }
  for (int k = 0; k < n; k++) {
    Recorder r; memset(&r, 0, sizeof r); r.tree = t;
    r.point = next_vec(25.0f);
    uint64_t mask = next_int(3) == 0 ? next_category() : ~0ull;
    float minSqr = next_int(3) == 0 ? 25.0f : 3.4e38f;
    float start = minSqr;
    b3TreeStats s = b3DynamicTree_QueryClosest(t, r.point, mask, false, closestCallback, &r, &minSqr);
    name("tree.closest"); v3(r.point); u64(mask); f(start); arrow(); stats(s); f(minSqr); i(r.count);
    for (int n2 = 0; n2 < r.count; n2++) { i(r.ids[n2]); f(r.passed[n2]); f(r.values[n2]); }
    end();
  }
}

static int proxies[1024];
static int proxyLive[1024];
static int proxyN;

static void insert(b3DynamicTree* t, int count) {
  for (int k = 0; k < count; k++) {
    b3AABB b = next_box(20.0f, 0.125f);
    uint64_t category = next_category();
    uint64_t userData = next_int(4) == 0 ? 0xfffffff0ull + (uint64_t)proxyN : (uint64_t)(3 * proxyN + 1);
    bool moved = next_int(3) == 0;
    int id = b3CreateTreeProxyInternal(t, b, category, userData, moved);
    name("tree.proxy"); box(b); u64(category); u64(userData); i(moved); arrow(); i(id); end();
    proxies[proxyN] = id; proxyLive[proxyN] = 1; proxyN++;
  }
}

static int pick_live(void) {
  for (;;) {
    int k = next_int(proxyN);
    if (proxyLive[k]) return k;
  }
}

static void script(int capacity, int count, int queryCount) {
  b3DynamicTree t = b3DynamicTree_Create(capacity);
  name("tree.create"); i(capacity); arrow(); end();
  proxyN = 0;

  insert(&t, count);
  state(&t);
  queries(&t, queryCount);

  for (int k = 0; k < count / 3; k++) {
    int p = pick_live();
    b3AABB b = next_box(20.0f, 0.125f);
    b3DynamicTree_MoveProxy(&t, proxies[p], b);
    name("tree.move"); i(proxies[p]); box(b); arrow(); end();
  }
  for (int k = 0; k < count / 6; k++) {
    int p = pick_live();
    b3AABB old = b3DynamicTree_GetAABB(&t, proxies[p]);
    b3AABB b = { b3Sub(old.lowerBound, (b3Vec3){ 0.5f, 0.25f, 0.125f }), b3Add(old.upperBound, next_vec(0.5f)) };
    b.upperBound = b3Max(b.upperBound, b3Add(old.upperBound, (b3Vec3){ 0.0625f, 0.0f, 0.0f }));
    b3DynamicTree_EnlargeProxy(&t, proxies[p], b);
    name("tree.enlarge"); i(proxies[p]); box(b); arrow(); end();
  }
  state(&t);

  for (int k = 0; k < count / 5; k++) {
    int p = pick_live();
    b3DynamicTree_DestroyProxy(&t, proxies[p]);
    proxyLive[p] = 0;
    name("tree.destroy"); i(proxies[p]); arrow(); end();
  }
  insert(&t, count / 4 + 1);
  state(&t);

  for (int k = 0; k < 3; k++) {
    int p = pick_live();
    b3DynamicTree_MarkProxyMovedSerial(&t, proxies[p]);
    name("tree.markMovedSerial"); i(proxies[p]); arrow(); end();
  }
  for (int k = 0; k < 3; k++) {
    int p = pick_live();
    b3AABB old = b3DynamicTree_GetAABB(&t, proxies[p]);
    b3AABB b = { b3Sub(old.lowerBound, (b3Vec3){ 0.25f, 0.25f, 0.25f }), old.upperBound };
    b3DynamicTree_MarkProxyMoved(&t, proxies[p], b);
    name("tree.markMoved"); i(proxies[p]); box(b); arrow(); end();
  }
  {
    int ids[1024];
    int n = b3DynamicTree_GatherMovedProxies(&t, ids);
    name("tree.gatherMoved"); arrow(); i(n);
    for (int k = 0; k < n; k++) i(ids[k]);
    end();
  }
  b3DynamicTree_Refit(&t);
  name("tree.refit"); arrow(); end();
  state(&t);

  name("tree.rebuild"); i(0); arrow(); i(b3DynamicTree_Rebuild(&t, false)); end();
  state(&t);
  queries(&t, queryCount);

  for (int k = 0; k < 4; k++) {
    int p = pick_live();
    b3AABB old = b3DynamicTree_GetAABB(&t, proxies[p]);
    b3AABB b = { old.lowerBound, b3Add(old.upperBound, (b3Vec3){ 0.375f, 0.0f, 0.25f }) };
    b3DynamicTree_MarkProxyMoved(&t, proxies[p], b);
    name("tree.markMoved"); i(proxies[p]); box(b); arrow(); end();
  }
  b3DynamicTree_Refit(&t);
  name("tree.refit"); arrow(); end();
  state(&t);
  name("tree.rebuild"); i(0); arrow(); i(b3DynamicTree_Rebuild(&t, false)); end();
  state(&t);
  name("tree.rebuild"); i(1); arrow(); i(b3DynamicTree_Rebuild(&t, true)); end();
  state(&t);
  queries(&t, queryCount);
  b3DynamicTree_ClearMoved(&t);
  name("tree.clearMoved"); arrow(); end();
  state(&t);
  b3DynamicTree_Destroy(&t);
}

static void set_state(const b3HashSet* s) {
  name("set.state"); arrow(); i(s->capacity); i(s->count);
  for (uint32_t k = 0; k < s->capacity; k++) { u64(s->items[k].key); i(s->items[k].hash); }
  end();
}

static void bits_state(int which, const b3BitSet* b) {
  name("bits.state"); i(which); arrow(); i(b->blockCapacity); i(b->blockCount);
  for (uint32_t k = 0; k < b->blockCount; k++) u64(b->bits[k]);
  end();
}

int main(void) {
  printf("# box3d oracle: broad-phase data structures, commit %s, scalar, -ffp-contract=off\n", BOX3D_ORACLE_COMMIT);

  script(16, 5, 4);
  script(8, 48, 12);
  script(64, 160, 12);

  /* The pair set: shape pair keys, hashes, adds, removes, growth. */
  {
    b3HashSet s = b3CreateSet(20);
    name("set.create"); i(20); arrow(); i(s.capacity); end();
    uint64_t keys[200];
    int keyCount = 0;
    for (int k = 0; k < 120; k++) {
      int s1 = next_int(64), s2 = next_int(64), c = next_int(4) == 0 ? next_int(1 << 20) : 0;
      if (s1 == s2) s2 = s1 + 1;
      uint64_t key = b3ShapePairKey(s1, s2, c);
      name("set.key"); i(s1); i(s2); i(c); arrow(); u64(key); end();
      name("set.hash"); u64(key); arrow(); i(b3KeyHash(key)); end();
      bool had = b3AddKey(&s, key);
      name("set.add"); u64(key); arrow(); i(had); end();
      keys[keyCount++] = key;
      if (k % 10 == 9) set_state(&s);
      if (k % 4 == 3) {
        uint64_t victim = keys[next_int(keyCount)];
        bool found = b3RemoveKey(&s, victim);
        name("set.remove"); u64(victim); arrow(); i(found); end();
      }
      if (k % 6 == 5) {
        uint64_t probe = next_int(2) ? keys[next_int(keyCount)] : b3ShapePairKey(next_int(64), 70 + next_int(10), 0);
        name("set.contains"); u64(probe); arrow(); i(b3ContainsKey(&s, probe)); end();
      }
    }
    set_state(&s);
    b3ClearSet(&s);
    name("set.clear"); arrow(); end();
    set_state(&s);
    b3DestroySet(&s);
  }

  /* Bit sets. */
  {
    b3BitSet sets[2] = { b3CreateBitSet(100), b3CreateBitSet(100) };
    name("bits.create"); i(0); i(100); arrow(); end();
    name("bits.create"); i(1); i(100); arrow(); end();
    for (int w = 0; w < 2; w++) {
      b3SetBitCountAndClear(&sets[w], 150);
      name("bits.setCountClear"); i(w); i(150); arrow(); end();
      bits_state(w, &sets[w]);
    }
    for (int k = 0; k < 120; k++) {
      int w = next_int(2);
      int op = next_int(5);
      int bit = next_int(300);
      if (op == 0 && bit < 150) { b3SetBit(&sets[w], bit); name("bits.set"); i(w); i(bit); arrow(); end(); }
      else if (op <= 1) { b3SetBitGrow(&sets[w], bit); name("bits.setGrow"); i(w); i(bit); arrow(); end(); }
      else if (op == 2) { b3ClearBit(&sets[w], bit); name("bits.clear"); i(w); i(bit); arrow(); end(); }
      else { name("bits.get"); i(w); i(bit); arrow(); i(b3GetBit(&sets[w], bit)); end(); }
      if (k % 20 == 19) {
        bits_state(w, &sets[w]);
        name("bits.count"); i(w); arrow(); i(b3CountSetBits(&sets[w])); end();
      }
    }
    while (sets[0].blockCount < sets[1].blockCount) {
      b3SetBitGrow(&sets[0], sets[0].blockCount * 64);
      name("bits.setGrow"); i(0); i((sets[0].blockCount - 1) * 64); arrow(); end();
    }
    while (sets[1].blockCount < sets[0].blockCount) {
      b3SetBitGrow(&sets[1], sets[1].blockCount * 64);
      name("bits.setGrow"); i(1); i((sets[1].blockCount - 1) * 64); arrow(); end();
    }
    b3InPlaceUnion(&sets[0], &sets[1]);
    name("bits.union"); arrow(); end();
    bits_state(0, &sets[0]);
    name("bits.count"); i(0); arrow(); i(b3CountSetBits(&sets[0])); end();
    b3SetBitCountAndClear(&sets[1], 1000);
    name("bits.setCountClear"); i(1); i(1000); arrow(); end();
    bits_state(1, &sets[1]);
    b3DestroyBitSet(&sets[0]);
    b3DestroyBitSet(&sets[1]);
  }

  /* The id pool. */
  {
    b3IdPool pool = b3CreateIdPool();
    int live[256];
    int liveCount = 0;
    for (int k = 0; k < 200; k++) {
      if (liveCount == 0 || next_int(3) != 0) {
        int id = b3AllocId(&pool);
        name("ids.alloc"); arrow(); i(id); end();
        live[liveCount++] = id;
      } else {
        int j = next_int(liveCount);
        int id = live[j];
        live[j] = live[--liveCount];
        b3FreeId(&pool, id);
        name("ids.free"); i(id); arrow(); end();
      }
      if (k % 25 == 24) { name("ids.state"); arrow(); i(b3GetIdCount(&pool)); i(b3GetIdCapacity(&pool)); end(); }
    }
    b3DestroyIdPool(&pool);
  }

  /* Quicksort of uint64 keys: sizes around the insertion threshold, duplicates. */
  for (int k = 0; k < 24; k++) {
    int n = k < 12 ? k * 3 : 16 + next_int(300);
    uint64_t keys[400];
    for (int j = 0; j < n; j++) keys[j] = next_int(4) == 0 ? (uint64_t)next_int(8) : next_u64();
    name("qsort"); i(n);
    for (int j = 0; j < n; j++) u64(keys[j]);
#define LESS( a, b ) ( keys[(int)( a )] < keys[(int)( b )] )
#define SWAP( a, b ) do { uint64_t tmp_ = keys[(int)( a )]; keys[(int)( a )] = keys[(int)( b )]; keys[(int)( b )] = tmp_; } while ( 0 )
    QSORT( n, LESS, SWAP );
#undef LESS
#undef SWAP
    arrow();
    for (int j = 0; j < n; j++) u64(keys[j]);
    end();
  }
  return 0;
}
