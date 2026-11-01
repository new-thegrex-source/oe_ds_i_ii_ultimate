# renderer_dx11

Direct3D 11 backend (Windows 7 and later).

Status: **stub** — not implemented yet.

Planned:
- Feature level 10_0 / 11_0
- Compute shaders, better multi-threading
- Optional HDR / MSAA paths

The client currently falls back to the built-in OpenGL renderer when
`r_renderer dx11` is selected.
