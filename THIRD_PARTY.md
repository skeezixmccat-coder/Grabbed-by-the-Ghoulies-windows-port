# Third-party sources and notices

The runtime is a modified native Windows integration. Upstream projects do not endorse this preview. Existing file-level copyright and licence notices are retained. Original project contributions are GPL-2.0-only; that declaration does not replace third-party grants.

| Component | Pinned source | Licence and use |
|---|---|---|
| Cxbx-Reloaded / XQEMU NV2A renderer | [Cxbx-Reloaded commit 585c49a50af1255ab155099e06f24505f9c5a800](https://github.com/Cxbx-Reloaded/Cxbx-Reloaded/tree/585c49a50af1255ab155099e06f24505f9c5a800), `src/devices/video/` | GPL version 2 selected where alternatives are offered; preserve each file's notices (including GPL-2-or-later, GPL-2-or-3 and swizzle's LGPL notice). [GPL text](docs/licenses/Cxbx-COPYING.txt), [LGPL v2 text](docs/licenses/LGPL-2.0.txt). |
| xemu renderer expressions and semantics | [xemu commit 36334d95b86f9c3c6ae34efb9af06785273f8e1a](https://github.com/xemu-project/xemu/tree/36334d95b86f9c3c6ae34efb9af06785273f8e1a), `hw/xbox/nv2a/pgraph/glsl/psh.c` and `vsh-ff.c` | Pixel shader code GPL-2-or-3 (version 2 selected); fixed-function viewport expression LGPL-2.0-or-later. [LGPL v2.1 text](docs/licenses/xemu-COPYING.LIB.txt) (existing source grants version 2 or later). |
| GLEW 2.0.0 headers | Same Cxbx checkout, `import/glew-2.0.0/include/GL/glew.h`, `wglew.h` | BSD-style GLEW and embedded Mesa/Khronos notices retained in headers and [licence text](docs/licenses/GLEW-LICENSE.txt). No prebuilt GLEW DLL is bundled. |

Cxbx/XQEMU port modifications include Windows threading/allocation/GL context glue, explicit unsupported-operation stops, native GPU-memory ownership and visibility integration, scalable rendering, state/texture/shader caching, asynchronous presentation and GPU depth-copy conversion. The original Xbox D3D code runs from the player's image; it is not included in source or binaries. The runtime maps and patches only the player's verified executable in process memory.

Interface/research references also include nxdk (CC0 kernel declarations; MIT pbkit/interface research), Nouveau/Envytools (MIT register/behaviour research), XbSymbolDatabase, and Khronos OpenGL specifications. The public [provenance index](docs/PORT-PROVENANCE.md) distinguishes consulted references from copied/adapted implementation. No proprietary SDK, game code translation, ISO, XBE, BIOS or game assets are included in the intended distribution.

See `docs/NV2A-SOURCE-MANIFEST.json` in the source tree for upstream paths and source hashes. The release manifest separately hashes each included current source file. Complete corresponding source and build scripts accompany binary candidates under `source/`.

For this GPL v2 combined runtime, the adapted LGPL portions use the conversion permission in LGPL section 3; original notices are retained. Modified upstream files include dated port notices.
