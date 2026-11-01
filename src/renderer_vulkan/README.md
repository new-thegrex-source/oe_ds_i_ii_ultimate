# renderer_vulkan

Vulkan backend with PBR and experimental RTX support.

Status: **stub** — not implemented yet.

This is the intended home for code adapted from:
https://github.com/timfox/idTech3

Planned features:
- Vulkan 1.1+ renderer
- Physically Based Rendering (metallic/roughness)
- Clustered / Forward+ lighting
- Optional KHR ray tracing (RTX)

The client currently falls back to the built-in OpenGL renderer when
`r_renderer vulkan` is selected.
