# Public source provenance

This index accompanies the corresponding source and build scripts. It distinguishes adapted implementation from interface research. Upstream projects do not endorse this preview. Original project contributions are GPL-2.0-only; upstream notices and grants remain intact.

## Adapted implementation

- Cxbx-Reloaded commit `585c49a50af1255ab155099e06f24505f9c5a800`: the files listed in `NV2A-SOURCE-MANIFEST.json` derive from `src/devices/video/` and the listed GLEW headers. That manifest records upstream and current hashes. The DMA routines in `nv2a_gl.cpp` additionally adapt `src/devices/video/nv2a.cpp` from this revision; its original notice is retained.
- xemu commit `36334d95b86f9c3c6ae34efb9af06785273f8e1a`: `hw/xbox/nv2a/pgraph/glsl/psh.c` supplied HILO/linear-coordinate expressions in `nv2a_psh.cpp` (GPL version 2 selected). `vsh-ff.c` supplied the viewport expression in `nv2a_shaders.cpp` (LGPL-2.0-or-later portion distributed under GPL v2 using LGPL section 3). Additional upstream notices, paths and hashes accompany these adaptations.
- Swizzle retains its LGPL v2 and post-2012 GPL-2-or-later notices. For this combined runtime the LGPL portion is distributed under GPL v2 using LGPL section 3. This selection does not remove upstream recipients' original grants.
- GLEW headers are unchanged. Their complete embedded GLEW, Mesa and Khronos notices remain in the headers and bundled licence text; no GLEW binary is bundled.

Port changes dated 2026-10-14 through 2026-10-19 include Windows WGL/thread/allocation glue, explicit failure handling, GPU memory ownership and visibility, resolution scaling, shader/state/texture caching, GPU depth conversion, presentation and optional anisotropy. Modified upstream files carry a dated notice. Original upstream notices were not removed. See ../THIRD_PARTY.md and licences under licenses/.

## Locally authored code and interface research

The launcher, disc reader, native loader, kernel replacements, audio/input backends, timing policy and supporting glue are locally authored. They use documented interfaces and read-only observations of the supported retail executable. They do not contain translated game functions or bundled game assets. Revision hashes, addresses and small patch-site validation constants identify supported behavior; they are not a distributed game executable.

The kernel ordinal classification in `include/kernel_exports.hpp` uses XboxDev/nxdk's CC0 export metadata (`lib/xboxkrnl/xboxkrnl.exe.def`). Other consulted interfaces include nxdk kernel declarations (CC0), nxdk pbkit at `14d5ee97e73347c973f1f57b68b79ec08c9e77f2` (MIT), Envytools at `f102b82381f3f11cee113d16374c87091db039d9` (MIT), Nouveau register descriptions, Khronos OpenGL specifications and Microsoft Windows API documentation. These provided interface facts, not copied implementations. XbSymbolDatabase at `20eced544726f5558c5a408458f38a086cc4e543` (MIT) identified function addresses; neither its scanner nor signature database is shipped.

The original Xbox code and data are supplied by the player and read locally from their own image. ISO/XBE/BIOS files, saves, captures, private diagnostics and the historical research checkout are excluded from the release. Build tools and Windows system libraries are prerequisites, not bundled third-party downloads.
