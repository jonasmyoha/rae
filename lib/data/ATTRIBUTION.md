# lib/data attribution

## hosek_wilkie_rgb.json

The numeric values are reproduced verbatim from the authors' published sample
implementation; only the container changed (C array initialisers -> JSON).

    Source:  https://cgg.mff.cuni.cz/projects/SkylightModelling/
    Archive: HosekWilkie_SkylightModel_C_Source.1.4a.zip
    File:    ArHosekSkyModelData_RGB.h
    Version: 1.4a, February 22nd 2013
    Licence: 3-clause BSD
    Copyright (c) 2012-2013, Lukas Hosek and Alexander Wilkie
    All rights reserved.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are met:

        * Redistributions of source code must retain the above copyright
          notice, this list of conditions and the following disclaimer.
        * Redistributions in binary form must reproduce the above copyright
          notice, this list of conditions and the following disclaimer in the
          documentation and/or other materials provided with the distribution.
        * None of the names of the contributors may be used to endorse or promote
          products derived from this software without specific prior written
          permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY
    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

Papers:
  * "An Analytic Model for Full Spectral Sky-Dome Radiance", SIGGRAPH 2012
  * "Adding a Solar Radiance Function to the Hosek Skylight Model", IEEE CG&A 2013

The model's arithmetic — the Bezier/albedo/turbidity interpolation and the
nine-term radiance formula — is implemented in Rae (`lib/sky_hosek.rae`), not
carried from the reference. Only the fitted table is foreign, and a table is
data. See docs/tech-stack-and-dependencies.md: own the small and controllable.
