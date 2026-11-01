# renderer_dx12

Direct3D 12 backend (Windows 10 and later).

Status: **stub** — not implemented yet.

Planned:
- Explicit multi-queue, low-level resource management
- Future path for DXR (ray tracing) if desired alongside Vulkan

The client currently falls back to the built-in OpenGL renderer when
`r_renderer dx12` is selected.
