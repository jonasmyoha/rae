# lib/physics — a Rae port of Box3D

A port of [Box3D](https://github.com/erincatto/box3d) (commit `9f998c8`, MIT —
`LICENSE-box3d.md`) to Rae, phase by phase (`docs/physics-rae-port-design.md`).
Every ported function is checked BIT-EXACT against Box3D's own C build through
the oracle (`tools/box3d-oracle/`).

## Porting rules (§3)

- Rae names: camelCase functions and parameters, PascalCase types, no `b3`
  prefix. Arguments are named at the call site, so overloads share one name
  where the parameter types tell them apart (`multiply(left: Quat, right: Quat)`,
  `multiply(matrix: Matrix3, vector: Vec3)`).
- `Float` is f32, as in Box3D. Operation order is preserved exactly — a
  reordered sum is a different result. Every build uses `-ffp-contract=off`.
- The deterministic `atan2` / `cos` / `sin` are ported verbatim.
- No globals: Box3D's `b3_lengthUnitsPerMeter`-scaled constants are functions
  of `lengthUnitsPerMeter` in `Constants.rae`.
- The vector and quaternion types are the stdlib's `Vec3`, `Quat` (`x y z w`)
  and `Vec2`. Box3D's large-world `b3Pos` / `b3WorldTransform` are the
  single-precision aliases `Pos` / `WorldTransform`.

## P1 — math (`math/`)

| Box3D | Rae |
|---|---|
| `b3MinInt` `b3MaxInt` `b3ClampInt` | `minInt` `maxInt` `clampInt` |
| `b3AbsFloat` `b3MinFloat` `b3MaxFloat` `b3ClampFloat` `b3LerpFloat` | `absFloat` `minFloat` `maxFloat` `clampFloat` `lerpFloat` |
| `b3Atan2` `b3ComputeCosSin` `b3Sin` `b3Cos` `b3UnwindAngle` | `deterministicAtan2` `computeCosSin` `deterministicSin` `deterministicCos` `unwindAngle` |
| `b3CeilingInt` `b3CeilingPow2` `b3IsValidFloat` | `ceilingInt` `ceilingPow2` `isValidFloat` |
| `b3Add` `b3Sub` `b3Mul` `b3Neg` `b3MulSV` | `add` `subtract` `multiply` `negate` `scale` |
| `b3MulAdd` `b3MulSub` `b3Blend2` `b3Blend3` | `mulAdd` `mulSub` `blend2` `blend3` |
| `b3Dot` `b3Cross` `b3Length` `b3LengthSquared` | `dot` `cross` `length` `lengthSquared` |
| `b3Distance` `b3DistanceSquared` `b3Lerp` | `distance` `distanceSquared` `lerp` |
| `b3Normalize` `b3GetLengthAndNormalize` `b3IsNormalized` `b3ClampLength` | `normalize` `getLengthAndNormalize` `isNormalized` `clampLength` |
| `b3Perp` `b3ArbitraryPerp` `b3ModifiedCross` `b3ScalarTripleProduct` | `perp` `arbitraryPerp` `modifiedCross` `scalarTripleProduct` |
| `b3Abs` `b3Min` `b3Max` `b3Clamp` `b3Sign` `b3SafeScale` | `abs` `min` `max` `clamp` `sign` `safeScale` |
| `b3MajorAxis` `b3MinElement` `b3MaxElement` | `majorAxis` `minElement` `maxElement` |
| `b3MakeQuatFromAxisAngle` `b3GetAxisAngle` `b3GetQuatAngle` | `quatFromAxisAngle` `axisAngle` `quatAngle` |
| `b3GetTwistAngle` `b3GetSwingAngle` | `twistAngle` `swingAngle` |
| `b3RotateVector` `b3InvRotateVector` | `rotateVector` `inverseRotateVector` |
| `b3DotQuat` `b3MulQuat` `b3InvMulQuat` `b3NegateQuat` `b3NormalizeQuat` | `dot` `multiply` `inverseMultiply` `negate` `normalize` |
| `b3Conjugate` | `conjugate` (lib/Quat) |
| `b3IsNormalizedQuat` `b3NLerp` `b3ComputeQuatBetweenUnitVectors` | `isNormalized` `nlerp` `quatBetweenUnitVectors` |
| `b3QuatFromExponentialMap` `b3IntegrateRotation` `b3DeltaQuatToRotation` | `quatFromExponentialMap` `integrateRotation` `deltaQuatToRotation` |
| `b3MakeQuatFromMatrix` `b3MakeMatrixFromQuat` | `quatFromMatrix` `matrixFromQuat` |
| `b3MulMV` `b3MulMM` `b3MulSM` `b3AddMM` `b3SubMM` `b3NegateMat3` | `multiply` `multiply` `scale` `add` `subtract` `negate` |
| `b3Transpose` `b3Det` `b3InvertMatrix` `b3InvertT` `b3Solve3` `b3AbsMatrix3` | `transpose` `determinant` `invert` `invertTransposed` `solve3` `abs` |
| `b3MakeDiagonalMatrix` `b3Skew` `b3Steiner` | `diagonalMatrix` `skew` `steiner` |
| `b3SphereInertia` `b3CylinderInertia` `b3BoxInertia` `b3RotateInertia` `b3TransformInertia` | `sphereInertia` `cylinderInertia` `boxInertia` `rotateInertia` `transformInertia` |
| `b3MulTransforms` `b3InvMulTransforms` `b3InvertTransform` | `multiply` `inverseMultiply` `invert` |
| `b3TransformPoint` `b3InvTransformPoint` | `transformPoint` `inverseTransformPoint` |
| `b3MulWorldTransforms` `b3InvMulWorldTransforms` `b3ToRelativeTransform` | `multiplyWorld` `inverseMultiplyWorld` `toRelativeTransform` |
| `b3TransformWorldPoint` `b3InvTransformWorldPoint` | `transformWorldPoint` `inverseTransformWorldPoint` |
| `b3SubPos` `b3OffsetPos` `b3LerpPosition` | `subtractPositions` `offsetPosition` `lerpPosition` |
| `b3MakeAABB` `b3OffsetAABB` `b3AABB_AddPoint` | `makeAabb` `offsetAabb` `addPoint` |
| `b3AABB_Contains` `b3AABB_Overlaps` `b3AABB_Union` `b3AABB_Inflate` | `contains` `overlaps` `union` `inflate` |
| `b3AABB_Area` `b3AABB_Center` `b3AABB_Extents` `b3AABB_Transform` | `area` `center` `extents` `transformAabb` |
| `b3ClosestPointToAABB` | `closestPointToAabb` |
| `b3PointToSegmentDistance` `b3SegmentDistance` `b3LineDistance` `b3IsWithinSegments` | `pointToSegmentDistance` `segmentDistance` `lineDistance` `isWithinSegments` |
| `b3NormalizePlane` `b3MakePlaneFromNormalAndPoint` `b3MakePlaneFromPoints` | `normalizePlane` `planeFromNormalAndPoint` `planeFromPoints` |
| `b3MakeNormalFromPoints` `b3TransformPlane` `b3PlaneSeparation` `b3SignedVolume` | `normalFromPoints` `transformPlane` `planeSeparation` `signedVolume` |
| 2D: `b3Dot2` `b3Cross2` `b3Length2` `b3Add2` `b3MulMV2` `b3Solve2` … | the same names over `Vec2` / `Matrix2` (`Vector2Math.rae`) |
| `b3IsValidVec3` `b3IsValidQuat` `b3IsValidTransform` `b3IsValidMatrix3` | `isValidVec3` `isValidQuat` `isValidTransform` `isValidMatrix3` |
| `b3IsValidAABB` `b3IsBoundedAABB` `b3IsSaneAABB` `b3IsValidPlane` | `isValidAabb` `isBoundedAabb` `isSaneAabb` `isValidPlane` |
| `b3IsValidPosition` `b3IsValidWorldTransform` | `isValidPosition` `isValidWorldTransform` |

The check: `compiler/tests/cases/948_physics_math` runs every line of
`tools/box3d-oracle/goldens/math.golden` (4 944 calls of 154 functions).

## P2 — geometry and narrow phase (`geometry/`, `collision/`), in progress

Box3D reads the length scale from a global (`b3GetLengthUnitsPerMeter`);
the port has no globals, so every function that needs a tolerance takes
`lengthUnitsPerMeter` (1 for meters). A `ShapeProxy` owns a copy of its
points (a Rae struct cannot hold a view); `proxyDistance` / `proxyCast`
take the proxies as views so the loops that call them never copy.

| Box3D | Rae |
|---|---|
| `b3RayCastAABB` | `rayCastAabb` (math/AabbMath) |
| `b3ComputeSphereMass` `b3ComputeSphereAABB` `b3ComputeSweptSphereAABB` | `computeSphereMass` `computeSphereAabb` `computeSweptSphereAabb` |
| `b3OverlapSphere` `b3RayCastSphere` `b3RayCastHollowSphere` `b3ShapeCastSphere` | `overlapSphere` `rayCastSphere` `rayCastHollowSphere` `shapeCastSphere` |
| `b3CollideMoverAndSphere` | `collideMoverAndSphere` |
| `b3ComputeCapsuleMass` `b3ComputeCapsuleAABB` `b3ComputeSweptCapsuleAABB` | `computeCapsuleMass` `computeCapsuleAabb` `computeSweptCapsuleAabb` |
| `b3OverlapCapsule` `b3RayCastCapsule` `b3ShapeCastCapsule` `b3CollideMoverAndCapsule` | `overlapCapsule` `rayCastCapsule` `shapeCastCapsule` `collideMoverAndCapsule` |
| `b3IsValidRay` `b3MakeProxy` | `isValidRay` `makeProxy` (plus `sphereProxy`, `capsuleProxy`) |
| `b3GetProxySupport` `b3GetPointSupport` | `proxySupport` `pointSupport` |
| `b3ShapeDistance` | `shapeDistance` (`proxyDistance` over held proxies) |
| `b3ShapeCast` | `shapeCast` (`proxyCast` over held proxies) |
| `b3GetSweepTransform` `b3TimeOfImpact` | `sweepTransform` `timeOfImpact` |
| `b3MakeFeaturePair` `b3FlipPair` `b3MakeFeatureId` | `makeFeaturePair` `flipPair` `makeFeatureId` |
| `b3ClipPolygon` `b3ClipSegment` `b3ReduceManifoldPoints` | `clipPolygon` `clipSegment` `reduceManifoldPoints` |
| `b3CollideSpheres` `b3CollideCapsuleAndSphere` `b3CollideCapsules` | `collideSpheres` `collideCapsuleAndSphere` `collideCapsules` |

The checks: fixture 949 runs `goldens/geometry.golden` (2 345 calls of 22
functions) and fixture 950 runs `goldens/manifold.golden` (832 calls of 8).

### P2c — hulls (`geometry/Hull*`, `BoxHull`, `Hull2d`)

A hull (`Hull`) is a struct of Lists (vertices, points, half-edges, faces,
planes) instead of Box3D's one block with offset-addressed arrays; the hash,
version and SoA copies are not ported (the scalar support search needs no
SoA). The quickhull builder links by index: its records are parallel Lists
per field, `-1` is NULL, and the free lists reuse slots in Box3D's order.
Builders that can fail return `opt Hull`.

| Box3D | Rae |
|---|---|
| `b3CreateHull` `b3CreateCylinder` `b3CreateCone` `b3CreateRock` `b3CreateComplexHull` | `createHull` `createCylinder` `createCone` `createRock` `createComplexHull` |
| `b3CloneAndTransformHull` `b3IsValidHull` | `cloneAndTransformHull` `isValidHull` |
| `b3MakeBoxHull` `b3MakeCubeHull` `b3MakeOffsetBoxHull` `b3MakeTransformedBoxHull` | `makeBoxHull` `makeCubeHull` `makeOffsetBoxHull` `makeTransformedBoxHull` |
| `b3ScaleBox` `b3MakeScaledBoxHull` | `scaleBox` `makeScaledBoxHull` |
| `b3ComputeHullMass` `b3ComputeHullAABB` `b3ComputeSweptHullAABB` | `computeHullMass` `computeHullAabb` `computeSweptHullAabb` |
| `b3OverlapHull` `b3RayCastHull` `b3ShapeCastHull` `b3CollideMoverAndHull` | `overlapHull` `rayCastHull` `shapeCastHull` `collideMoverAndHull` |
| `b3FindHullSupportVertex` `b3FindHullSupportFace` `b3FindIncidentFace` | `findHullSupportVertex` `findHullSupportFace` `findIncidentFace` |
| `b3ComputeHullExtent` `b3ComputeHullProjectedArea` | `computeHullExtent` `computeHullProjectedArea` |
| `b3Hull2D` `b3SimplifyHull2D` (`b3Point2D`) | `hull2d` `simplifyHull2d` (`Point2d`) |

The check: fixture 951 runs `goldens/hull.golden` (1 177 calls of 26
functions, hulls from random, spherical, grid, duplicate-laden, flat and
collinear point clouds) and checks every built hull with `isValidHull`.

### P2d — hull manifolds and the shape-pair dispatch (`collision/`)

The separating axis test is Box3D's scalar path (`B3_SIMD_NONE`), including
the scalar emulation of its four-lane support search, which packs the vertex
index into the low mantissa bits of the support value (`Math.floatBits` /
`Math.floatFromBits` give the exact reinterpretation). Box3D's function-
pointer registry of shape pairs is an exhaustive `match` on `ShapeType`.

| Box3D | Rae |
|---|---|
| `b3CollideHullAndSphere` `b3CollideHullAndCapsule` `b3CollideHulls` | `collideHullAndSphere` `collideHullAndCapsule` `collideHulls` |
| `b3ComputeSeparatingAxis` (`b3AxisQuery`, `b3SeparatingAxis`, `b3SeparatingFeature`) | `computeSeparatingAxis` (`AxisQuery`, `SeparatingAxis`, `SeparatingFeature`) |
| `b3SATCache` `b3ContactCache` | `SatCache` `ContactCache` |
| `b3BuildPolygon` `b3BuildFaceAContact` `b3BuildFaceBContact` `b3BuildEdgeContact` | `buildPolygon` `buildFaceAContact` `buildFaceBContact` `buildEdgeContact` |
| `b3ClipSegmentToHullFace` `b3GetSupportWide` `b3TestEdgeCandidate` | `clipSegmentToHullFace` `supportWide` `testEdgeCandidate` |
| `b3ShapeType`, `s_registers` (`b3AddType`) | `ShapeType`, `isPrimaryContactPair` / `isSupportedContactPair` |
| `b3ComputeConvexManifold`'s dispatch | `collideConvexShapes` (over `ConvexShape`) |

The check: fixture 952 runs `goldens/hullmanifold.golden` (768 calls: the
manifolds with their GJK and SAT caches carried across steps, and the
separating axis test) through `collideConvexShapes`, and prints the pair
registry.

## P3 — broad-phase data structures (`broadPhase/`, `container/`)

The dynamic tree's nodes, parents and proxies are Lists; the proxy pool
keeps Box3D's capacity, because it decides the order proxy ids are handed
out. Box3D's tree queries take a callback; Rae has no function values, so
each query is an explicit traversal state: `next…` returns the next
candidate proxy where Box3D would call back, and the casts and the closest
query take the callback's value back with `report…`.

| Box3D | Rae |
|---|---|
| `b3DynamicTree_Create` `_CreateProxy` (`b3CreateTreeProxyInternal`) `_DestroyProxy` | `createDynamicTree` `createTreeProxy` (`createTreeProxyMarked`) `destroyTreeProxy` |
| `_MoveProxy` `_EnlargeProxy` `_SetCategoryBits` `_GetCategoryBits` `_GetUserData` `_GetAABB` | `moveTreeProxy` `enlargeTreeProxy` `setTreeCategoryBits` `treeCategoryBits` `treeUserData` `treeProxyAabb` |
| `_GetHeight` `_GetAreaRatio` `_GetRootBounds` `_GetProxyCount` `_Validate` | `treeHeight` `treeAreaRatio` `treeRootBounds` `treeProxyCount` `isValidTree` |
| `_Query` (callback) | `beginTreeQuery` + `nextTreeQueryProxy` |
| `_RayCast` `_BoxCast` (callback) | `beginTreeRayCast` / `beginTreeBoxCast` + `nextTreeCastProxy` + `reportTreeCast` |
| `_QueryClosest` (callback) | `beginTreeClosestQuery` + `nextTreeClosestProxy` + `reportTreeClosest` |
| `_Rebuild` `_Refit` `_ClearMoved` `_GatherMovedProxies` | `rebuildTree` `refitTree` `clearMoved` `gatherMovedProxies` |
| `_MarkProxyMovedSerial` `_MarkProxyMoved` | `markProxyMovedSerial` `markProxyMoved` |
| `b3HashSet` `b3CreateSet` `b3AddKey` `b3RemoveKey` `b3ContainsKey` `b3ClearSet` | `PairSet` `createPairSet` `addPairKey` `removePairKey` `pairSetContains` `clearPairSet` |
| `b3ShapePairKey` `b3KeyHash` | `shapePairKey` `keyHash` |
| `b3BitSet` and its functions | `Bitset`, `createBitset` `setBitCountAndClear` `setBit` `setBitGrow` `clearBit` `getBit` `inPlaceUnion` `countSetBits` |
| `b3IdPool` `b3AllocId` `b3FreeId` | `IdPool` `allocId` `freeId` |
| `QSORT` (qsort.h) | `quickSort(T:)` over a List of an ordered type |

The check: fixture 953 replays `goldens/broadphase.golden` (1 633
operations: three scripted trees with full state dumps and every query
kind, the pair set's slot layout, the bit sets, the id pool, quicksort).
Pair finding (broad_phase.c) needs the world's shapes and filters and
comes with P4.

## P4a — the world, bodies and shapes (`dynamics/`, `broadPhase/BroadPhase`)

Everything a world owns is a field of one `PhysicsWorld` value, passed `mod`
to whatever changes it — Box3D's world registry, length scale and
allocators included; there are no globals. Pointers between objects are
ids into the world's Lists (-1 for none), exactly as Box3D's arrays are
indexed. A hull shape keeps a copy of its hull in `world.hulls` (Box3D
shares equal hulls through a database; that only saves memory).

| Box3D | Rae |
|---|---|
| `b3World` `b3CreateWorld` | `PhysicsWorld` `createWorld` (`dynamics/World`) |
| `b3Body` `b3BodySim` `b3BodyState` | `Body` `BodySim` `BodyState` (`dynamics/Body`) |
| `b3SolverSet` `b3Island` | `SolverSet` `Island` (`dynamics/SolverSet`, `dynamics/Island`) |
| `b3CreateIsland` `b3DestroyIsland` `b3WakeSolverSet` `b3WakeBody` | `createIsland` `destroyIsland` `wakeSolverSet` `wakeBody` (`dynamics/WorldIslands`) |
| `b3CreateBody` `b3DestroyBody` `b3UpdateBodyMassData` | `createBody` `destroyBody` `updateBodyMassData` (`dynamics/WorldBodies`, `dynamics/BodyMass`) |
| `b3Body_Get*` / `_Set*` / `_Apply*` | `getBody…` / `setBody…` / `applyBody…` |
| `b3Shape` `b3CreateSphereShape` `…Capsule…` `…Hull…` `…TransformedHull…` `b3DestroyShape` | `Shape` `createSphereShape` `createCapsuleShape` `createHullShape` `createTransformedHullShape` `destroyShape` (`dynamics/Shape`, `dynamics/WorldShapes`) |
| `b3CreateShapeProxy` `b3DestroyShapeProxy` | `createShapeProxy` `destroyShapeProxy` (`dynamics/ShapeProxy`) |
| `b3BroadPhase` `b3BroadPhase_CreateProxy` `_DestroyProxy` `_MoveProxy` | `BroadPhase` `createBroadPhaseProxy` `destroyBroadPhaseProxy` `moveBroadPhaseProxy` |

The check: fixture 954 replays `goldens/world.golden` (134 operations:
bodies of every type and state, sphere, capsule and hull shapes, destroys,
transforms, velocities, forces, impulses, mass data and damping, with
Box3D's whole internal world state — id pools, bodies, solver sets,
islands, shapes and the three broad-phase trees — dumped at checkpoints).
Contacts, the constraint graph, the solver and its stage dispatcher, sleep
and island splitting, sensors and events are P4b-P4d.

## P3b — broad-phase pair finding (`dynamics/BroadPhasePairs`)

Box3D's pair finding is ported with its traversal and task structure: the
moved sibling pairs of the dynamic tree collide their subtrees against each
other, breadth-first seeds collide the dynamic tree against the static and
kinematic trees, candidates are culled against the pair set and filtered in
batches of 32, and the sorted new keys go to contact creation. The tasks
run block by block as worker 0 (`dynamics/ParallelFor`, Box3D's block
split), each gathering into its task context's pair keys, so threading them
later changes only the block loops.

| Box3D | Rae |
|---|---|
| `b3UpdateBroadPhasePairs` | `updateBroadPhasePairs` (returns the keys it handed to contact creation) |
| `b3GatherMovedSiblings` `b3GatherCrossSeeds` | `gatherMovedSiblings` `gatherCrossSeeds` |
| `b3SelfPairsTask` `b3CrossPairsTask` | `selfPairsTask` `crossPairsTask` |
| `b3CollideCrossPairs` `b3VisitPair` `b3CollideProxyAndSubtree` `b3TestPair` | `collideCrossPairs` `visitPair` `collideProxyAndSubtree` `testNodePair` |
| `b3AddCandidatePair` `b3FlushCandidatePairs` `b3ShouldCreatePair` | `addCandidatePair` `flushCandidatePairs` `shouldCreatePair` |
| `b3ShouldBodiesCollide` | `shouldBodiesCollide` (`dynamics/WorldBodies`; the joint walk is P6) |
| `b3ParallelFor`'s block split | `parallelForBlockSize` `parallelForBlockCount` … (`dynamics/ParallelFor`) |
| `b3TaskContext` | `TaskContext` (`dynamics/World`; only the pair keys so far) |
| `b3CreateContact` | `createContact` (`dynamics/Contact`; the pair registry and pair set only, the rest is P4b) |

Not yet: compound pair emission (`b3EmitCompoundPairs`, with compounds in
P5) and the custom filter callback (Rae has no function values; P8).

The check: fixture 956 replays `goldens/pairs.golden` (245 operations: a
world of static, kinematic and dynamic bodies packed to overlap, bodies
moved between updates, the pair keys of every contact created in creation
order, and the pair set's slot layout and the three trees at checkpoints).

## P4b — contacts, the constraint graph, contact islands (`dynamics/`)

A contact is a value in the world's contact list, linked into both bodies'
contact lists by keys (contact id times two plus the edge), exactly as in
Box3D; a convex contact holds at most one manifold. The narrow phase runs
block by block as worker 0, then processes the touching-state changes
serially in contact id order.

| Box3D | Rae |
|---|---|
| `b3Contact` `b3ContactEdge` `b3Manifold` `b3ManifoldPoint` `b3ContactFlags` | `Contact` `ContactEdge` `Manifold` `ManifoldPoint` the flag consts (`dynamics/Contact`) |
| `b3CreateContact` `b3DestroyContact` | `createContact` `destroyContact` (`dynamics/WorldContacts`) |
| `b3ComputeConvexManifold` `b3UpdateConvexContact` `b3UpdateContact` | `collideShapes` + `computeConvexManifold` `updateConvexContact` `updateContact` |
| `b3ConstraintGraph` `b3GraphColor` `b3ContactSpec` `b3CreateGraph` | `ConstraintGraph` `GraphColor` `ContactSpec` `createGraph` (`dynamics/ConstraintGraph`) |
| `b3AddContactToGraph` `b3RemoveContactFromGraph` | `addContactToGraph` (`assignContactColor`) `removeContactFromGraph` (`dynamics/WorldGraph`) |
| `b3ContactLink` `b3MergeIslands` `b3LinkContact` `b3UnlinkContact` | `ContactLink` `mergeIslands` `linkContact` `unlinkContact` (`dynamics/WorldIslands`) |
| `b3WakeSolverSet` `b3TrySleepIsland` | `wakeSolverSet` `trySleepIsland` (with their contact moves) |
| `b3RefreshBodyContactIndices` | `refreshBodyContactIndices` (`dynamics/World`) |
| `b3CollideTask` (with contact recycling) `b3Collide` | `collideTask` (`recycleContact`) `collide` (`dynamics/Collide`) |
| `b3World_Step` with a zero time step | `collideWorld` |

Destroying a shape or a body now destroys its contacts, and a mass update
invalidates their cached recycling transforms. Not yet: contact events
(P4d), the pre-solve and custom friction/restitution callbacks (Box3D's
default mixing is used), compound and mesh contacts (P5), joints in the
graph and islands (P6), island splitting (P4d).

The check: fixture 957 replays `goldens/contacts.golden` (233 operations:
zero-time-step collides of a world of touching bodies, small moves that
recycle contacts, large moves that end them, wakes, islands put to sleep,
re-massing, shape and body destroys; every contact with its manifold, the
24 graph colours including overflow, the solver sets, the islands and the
bodies' contact fields dumped at checkpoints).
