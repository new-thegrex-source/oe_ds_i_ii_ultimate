# renderer_dx9

Direct3D 9 backend (Windows XP and later).

Status: **stub** — not implemented yet.

Planned:
- Full D3D9 device + swap chain
- Shader Model 2.0 / 3.0 path
- Good performance and compatibility on Windows XP SP3+

The client currently falls back to the built-in OpenGL renderer when
`r_renderer dx9` is selected.
