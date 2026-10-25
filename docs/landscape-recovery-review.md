# Landscape recovery review — 25 October 2026

The first landscape series did not meet the supplied art references. The birch
regressed visibly. The task routing was wrong: all landscape art work belongs at
**difficulty 5 (6 Astra)**, as requested. The queue header had even
claimed all tasks should be difficulty 4. That instruction is removed. The 26
unstarted landscape tasks already had local difficulty-5 corrections when this
review began; those corrections are preserved, not credited as newly discovered.

## Preserve the work; recover selectively

`recovery/landscape-before-review-20261025` preserves `b37f95bd`, including all ten
landscape commits. `e86992db` is the pre-implementation reference/queue baseline.
The branch `recovery/landscape-original-plan-20261025` preserves that baseline.
It was extracted outside the working checkout to render the original appearance.
No branch history was reset, no queue file was stashed, and no work was pushed.

Keep the inventory, stable species numbers, capture controls, shared resource
owners, useful geometry checks, biome settings and exclusions. Their existence
does not justify retaining failed art. Reopen the eight visual implementation
outcomes and replace their acceptance claims with explicit recovery work. Do not
propagate the rejected birch or flower treatment into the remaining species.

## Birch: original, regression, first repair

All three frames below were newly rendered for this review at the same camera,
1280 × 800 output, Preetham 10:00, exposure 1, fixed 60 frames at 1/60 second and
wind disabled. Deferred toon rendering, cascaded shadows, SSAO, SMAA and bloom
remain on. The original scene uses its original shared seed 1437 and instance
scales; the rejected and repaired scenes use the exact same newer five seeds,
heights, transforms, scaffold and materials. Therefore the latter pair isolates
the foliage repair. The original is a historical appearance comparison, not a
controlled geometry experiment. Screenshots are actual app output, not mockups.

### Before the landscape implementation

![Original birch before the queue implementation](images/landscape-recovery/baseline-front.png)

Full crowns and fine edges are substantially better than the rejected series,
although rounded clumps and dark interiors were already unresolved.

### Rejected implementation

![Rejected birch after the landscape implementation](images/landscape-recovery/rejected-front.png)

Two to three smaller cards per spray, a heavily shaped four-cell atlas and a mix
of branch-plane/camera-facing orientations fragmented the crown and exposed too
much white scaffold. Changed tree sizes and seeds obscured that loss in the
previous comparisons.

### First recovery candidate, not a finished species

![Birch recovery candidate](images/landscape-recovery/repair1-front.png)

Recovered the original fine-edge `birchLeaves.png` from `e86992db`. Each spray
now uses four or five unequal overlapping camera-facing cards with full-image
UVs. The centres remain attached to the same twigs; crown normals and fixed wind
pivots remain independent of camera orientation. No emission, global exposure,
shadow disable or new renderer path was added. The newer wood and bark are left
unchanged so the foliage repair can be judged separately.

This restores much of the lost crown coverage. It still has visible clumps,
dark pockets, conspicuous branch fragments and repetitive leaf marks. It is a
recovery control for further work, not the desired reference quality. More cards
also mean more alpha overdraw; this is a deliberate quality recovery, not a
performance improvement. Do not copy this candidate unchanged to other species.

## Reference comparison

The private image presentation is at
`/Users/jonas/Documents/my_own_stuff/peli_ideoita/environmentReferenceStudy/recoveryReview/index.html`.
It contains front/side/under-crown views for all three versions and the actual
T04/T05/T07/T10 crops alongside them. Private source images remain outside git.

T04/T05 establish connected large crown shapes with small irregular leaf edges;
lighting describes the crown, rather than each leaf or round card group. T07
shows purposeful open sectors and sculptural exposed wood. T10 adds irregular
branch-supported depth. They are art references, not botanical evidence that
those trees are silver birch. Match their visual organization without changing
the birch into another species. G02 establishes ground-cover grouping and a
soft relationship between small plants and the ground, not isolated angular
blades scattered over one flat colour.

The presentation also includes the previously committed flower and fork study
frames, clearly labelled as historical evidence. Their upright thin stalks,
small geometric heads, angular grass and long pointed wood explain why passing
geometry and nonblank-image checks did not meet the task.

## All 36 landscape tasks

These decisions concern every task in the landscape series. Unrelated completed
compiler/editor tasks are not reopened. Original IDs and technical completion
notes remain in QUEUE.md; rejected visual outcomes are retained as superseded `[postponed]` records
and have separate runnable `[ ]` difficulty-5 recovery tasks. The remaining 26 tasks retain
their IDs and receive the relevant recovery prerequisites.

| Task | Previous state | Decision | Required change |
|---|---|---|---|
| #57936867 Landscape species inventory | Previously marked done | Keep inventory | Stable numeric identities and the old-slot mapping remain useful. Botanical labels are targets, not a claim that the existing models resemble those species. |
| #72617739 Landscape comparison views | Previously marked done | Keep capture controls; correct evidence | A reproducible frame is useful, but it is not a visual acceptance test. The later birch change altered seeds, sizes and scaffold, preventing a controlled foliage comparison. Require an unchanged study specimen as well as the showcase. |
| #33428161 Shared sculptural tree forks | Previously marked done | Reopen visual work | Joined topology and frame tests are useful. The study still shows straight pointed limbs, thin stretched junctions and abrupt root forms. Prove a curved fork against T02/T06/T07/T09 before any further rollout. |
| #77839584 Birch foliage technique prototype | Previously marked done | Reject chosen visual treatment | The smaller hybrid cards and shaded four-cell atlas exposed the scaffold and broke the crown into small harsh masses. Restore the earlier full-image fine-edge texture as a control; compare one branch and one whole crown before choosing again. |
| #14470240 Finish silver birch | Previously marked done | Reject finished claim | The complete birch is visibly worse than its pre-queue control. This repair restores coverage only; clumps, dark pockets, branch visibility, bark and motion still need a full species pass. |
| #85557127 Grass close-up overhaul | Previously marked done | Reopen visual work | In the inspected frames the grass still reads as separate angular blades on flat ground. The textured alternative was dismissed in prose; a rendered comparison of both candidates is required. |
| #94765846 Grass patches and ground blending | Previously marked done | Retain tested placement; reopen appearance | Keep per-app biome settings, stable seeds and obstacle exclusions. Broad sparse gaps currently expose flat ground and make isolated spikes stronger. Reassess together with the new near grass treatment. |
| #40763640 Grass wind and forest density | Previously marked done | Reassess after grass recovery | Retain bounded storage and rooted interactions. No still image proves convincing motion; compare matched continuous calm/breeze walking clips after the silhouette is fixed. |
| #96591544 Meadow daisy flower prototype | Previously marked done | Reopen flower art | Shared batches and ownership are useful. Thin nearly straight stalks and sparse small discs do not make a convincing meadow. Rebuild one close plant and a compact patch before another species uses it as a reference. |
| #76414314 Bellflower family | Previously marked done | Reopen flower art | The bells are botanically distinguishable, but tiny geometric heads and upright sticks are not the soft patch composition in G02. Preserve batching; revisit scale, stem curves, leaves and head shading. |
| #87183597 Buttercup family | Unstarted | Hold behind flower recovery | Do not extend the rejected daisy/bellflower appearance to a third species. Start only after a reviewed plant-and-patch comparison. |
| #46350604 Flower meadows and species patches | Unstarted | Hold behind grass and flower recovery | Patch composition depends on the recovered assets. Require one small mixed patch first; do not populate the entire forest to judge it. |
| #80837875 Small-leaved lime refinement | Unstarted | Keep scope; replace prerequisite | Use a reviewed crown treatment and sculptural wood recovery, not the rejected birch recipe. Compare coherent oval silhouette and fine perimeter to T04/T07. |
| #48605089 Sugar maple autumn-orange refinement | Unstarted | Keep scope; replace prerequisite | Do one orange specimen before rollout; require broad crown colour groups and fine edges against T04/T05, not a recoloured birch. |
| #28246202 Red maple scarlet refinement | Unstarted | Keep after orange maple | Retain distinct scaffold and rounded crown. Compare the two maple species together at identical scale/light so differences are visible. |
| #21954987 Golden Globe ginkgo autumn-gold refinement | Unstarted | Keep distinct kind 6 | Keep the explicit showcase/forest mismatch correction. Add the named existing kind without deleting or recolouring the palm; use reviewed foliage. |
| #56427211 Bloodgood Japanese maple crimson refinement | Unstarted | Keep after maple recovery | Require readable burgundy masses in shade and backlight, not dark balls. Keep a distinct smaller spreading silhouette. |
| #55057148 Norway spruce layered foliage refinement | Unstarted | Keep species-specific prototype | A spruce does not inherit birch cards automatically. Compare layered branch-supported needle sprays to current skirts before changing six specimens. |
| #30757695 Mediterranean cypress refinement | Unstarted | Preserve successful slender control | This is a refinement, not permission to replace the narrow silhouette. Keep the pre-queue specimen in every comparison and reject thicker or lumpy results. |
| #43408695 Stone pine refinement | Unstarted | Keep; wait for wood and foliage review | Prove the exposed scaffold and parasol underside first. Branch-constrained pine sprays may differ from birch; choose by orbit evidence. |
| #73266889 Date palm frond refinement | Unstarted | Keep distinct frond approach | The named palm mismatch needs real pinnate fronds. Do not force the birch crown technique onto it; compare one lit frond then one apex crown. |
| #31529936 Camphor tree sculptural specimen refinement | Unstarted | Keep single specimen; wait for wood review | Keep height and placement. Prove a sheltered under-canopy space and sweeping primary branches before replacing its expansive crown. |
| #61690381 Common hazel shrubs | Unstarted | Keep after foliage recovery | Compare one multi-stem shrub next to a rock; avoid replacing leaf polygons with repeated card balls. |
| #83500006 Woodland fern ground cover | Unstarted | Delay new plant until ground cover works | A fern is not the repair for failed grass/flowers. Use a reviewed ground-cover material and one curved frond proof first. |
| #17691144 Forest foliage wind coherence | Unstarted | Keep final integration; require earlier local motion | Each species must already pass its own orbit/motion check. This task tunes spatial coherence across a stand; it must not defer basic wind quality until the end. |
| #53338503 Rock shape and material foundation | Unstarted | Keep independent prototype | One directional wedge, plain and mossed, with the actual R01/R02 crop beside it. Do not replace all rocks before seeing the first. |
| #81194269 Mossy wedge outcrops | Unstarted | Keep after rock review | Approve broad faces and a quiet upper moss cap before multiplying seeds; no uniformly outlined green stones. |
| #54599145 Layered grey rock shelves | Unstarted | Keep distinct shelves | Require recognizable long unequal strata and recesses from R03; do not count stretched wedges as another family. |
| #84622055 Fractured blocks and rubble | Unstarted | Keep distinct fracture family | Require a few readable major breaks and believable rubble scale; do not add noisy triangles as detail. |
| #22082117 Rock groups across streamed terrain | Unstarted | Keep after rock and ground review | Stream only the reviewed families; retain collision, grass exclusions and deterministic placement. |
| #58576313 Shared cloud layer foundation | Unstarted | Keep independent layer prototype | Compare one lit group against C01-C03 before general presets. No ray-marching expansion or global exposure workaround. |
| #45432443 Quiet rounded cloud composition | Unstarted | Keep after cloud review | Judge sky opening, large form and quiet bases against C03 at identical sun/exposure. |
| #57043632 Broken cloud-bank composition | Unstarted | Keep same implementation | Different composition on the reviewed layer; require a horizon view and moving-camera proof. |
| #13791278 Autumn Grove final landscape composition | Unstarted | Keep last art composition | Do not label finished while any earlier visual rejection remains. Use small successful groups to compose the whole scene. |
| #27142161 Huge Forest landscape integration and scale | Unstarted | Keep after grove review | Use the same assets, then test scale and transitions. Preserve near quality; performance is not permission for global density cuts. |
| #91339952 Landscape series final verification | Unstarted | Keep final verification; strengthen sign-off | Publish baseline/result/reference side by side and unresolved failures. Automated tests demonstrate correctness, not style or approval. |

## Recovery sequence

1. **Sculptural fork visual recovery:** retain the useful topology/frame work;
   replace the pointed straight visual study with one convincing young/mature fork.
2. **Silver birch crown recovery:** use the original and this recovery as
   controls, prove one branch and one complete crown, then review wood separately.
3. **Near-grass visual recovery:** actually render both textured tufts and curved
   ribbons under matched conditions before choosing the shared treatment.
4. **Grass patch and motion recovery:** integrate the selected near grass with
   ground colour, retained exclusions and continuous calm/breeze walking captures.
5. **Oxeye daisy visual recovery:** rebuild one plant and small patch before the
   shared flower framework is considered an art reference.
6. **Harebell visual recovery:** keep its different growth/head shape and batching;
   redo the plant art after the daisy/ground-cover comparison.

These are bounded replacements for the rejected outcomes, not extra species.
The remaining species, rock and cloud tasks keep their original IDs. Only tasks
that consume a rejected foundation gain the corresponding recovery prerequisite.
The initial `failed` markers unnecessarily stopped queue progression. They are
now superseded `[postponed]` records with links to the six runnable `[ ]`
recovery tasks. Formal dependencies point to those replacements. Start at
#64669660 (Sculptural fork visual recovery), followed by #87803692 (Silver birch
crown recovery). The comparison report remains the visual control for the work.

## Acceptance from now on

1. Keep an unchanged control specimen. Match seed, physical dimensions, camera,
   light, exposure, resolution and wind phase; show any intentional differences.
2. Render the candidate next to that control and the named reference crops.
   Reject the candidate when its mass, silhouette, edge scale or lighting is
   worse, even if every automated check passes.
3. Prove one branch/clump and one complete plant before shared rollout. Motion,
   underside, shadow and night checks are part of that prototype, not work left
   for the final series task.
4. Record an honest visual verdict and remaining defects. A model assignment,
   fixture count or regenerated reference is never a substitute for the images.

## Verification of this recovery

Passed with explicit timeouts:

- `run_cases.sh 1235_birch 1228_tree_levels_cover_alike 1232_wind`: 3/3.
- `surfaceImages`, `leafWind`, `impostorBake` hardware checks: 3/3.
- Example gates 136, 137, 138, 143: 4/4, including the fourteen grove inspection
  views and the 31,090-tree forest.
- `format-check-tree.sh`: 1,811 canonical files; example-copy and banned-terms
  gates passed.
- Repeated repaired front BMP: byte-identical. Additional rear/backlit and night
  captures inspected and included in the private presentation. Foreground
  cypresses partly obscure the rear view. A continuous orbit/walk review remains
  necessary before accepting the species; this repair does not claim it.

These verify compatibility and the specified technical properties. They do not
establish the desired appearance. No full compiler suite was needed for this
bounded generator/asset change under the repository's narrow-check rules.

The birch fixture previously used repository-relative
asset paths and failed when run from the official runner's compiler directory.
It now resolves those images through RAE_STDLIB. Geometry, pivots, conservative
bounds, alpha coverage and mip checks remain active; the old four-cell atlas
assertions are replaced by the recovered full-image UV/coverage contract.
