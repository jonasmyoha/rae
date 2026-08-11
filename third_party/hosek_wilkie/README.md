# Hosek-Wilkie sky model — coefficient dataset

`ArHosekSkyModelData_RGB.h` is vendored VERBATIM from the authors' sample
implementation, with its licence header intact:

    Source:  https://cgg.mff.cuni.cz/projects/SkylightModelling/
    Archive: HosekWilkie_SkylightModel_C_Source.1.4a.zip
    Version: 1.4a, February 22nd 2013
    Licence: 3-clause BSD, Copyright (c) 2012-2013 Lukas Hosek and Alexander Wilkie

Papers:
  * "An Analytic Model for Full Spectral Sky-Dome Radiance", SIGGRAPH 2012
  * "Adding a Solar Radiance Function to the Hosek Skylight Model", IEEE CG&A 2013

`rae_hosek.c` / `rae_hosek.h` are ours. They expose the dataset to Rae through a
single scalar accessor and transcribe the two "cook" routines from the reference
`ArHosekSkyModel.c` (the documented way to read this dataset). The per-pixel
radiance formula is NOT here — it lives in `lib/sky.rae` and in the WGSL
lighting pass, because it is nine multiply-adds and needs no table.

Deliberately NOT vendored from the same archive:
  * `ArHosekSkyModelData_Spectral.h` (33770 lines) — nothing here is spectral.
  * `ArHosekSkyModelData_CIEXYZ.h` — we want RGB directly.
  * `ArHosekSkyModel.c/.h` — we use only the two cook routines, transcribed with
    attribution above, rather than carrying the full solar-radiance/spectral API.

Verification: `compiler/tests/cases/591_hosek_wilkie_sky` checks our Rae
implementation against values produced by the UNMODIFIED reference
implementation. See that test's header for how the oracle was generated.
