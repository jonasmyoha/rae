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
