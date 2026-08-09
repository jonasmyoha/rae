# Third-party asset: `character.glb`

The Khronos **Fox** glTF sample, downloaded unmodified from
<https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Fox>
and renamed to `character.glb`.

It is here because it is *clearly licensed*. An earlier version of this
example used a model copied from another local repository whose licensing
could not be established; that file was removed from this repository's
history rather than merely deleted. Anything added under `assets/` should
come with the paragraph below already written.

## Licence

| Part | Licence | Author |
| --- | --- | --- |
| Model | [CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/legalcode) | PixelMannen (2014) |
| Rigging & animation | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/legalcode) | tomkranis (2014) |
| glTF conversion | [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/legalcode) | @AsoboStudio and @scurest |

The CC-BY parts require attribution, which this file provides. No part
requires share-alike, so it imposes nothing on the rest of the repository.

## Why this model specifically

It exercises the loader harder than a typical export:

* **No `NORMAL` attribute.** glTF makes normals optional and requires the
  client to compute FLAT face normals instead. Most exports include them,
  so this is the asset that proves that path is implemented rather than
  merely written.
* **Non-indexed primitive.** Vertices are already in triangle order with
  no index accessor — the other branch most loaders get wrong once.
* **24 joints and three animation clips** (Survey, Walk, Run) in a single
  file, which is what the skinning and animation increments need next.
