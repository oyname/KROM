![KROM Engine](https://github.com/oyname/oyname.github.io/blob/main/oynameengine_logo.png)

## in development

Main page: https://oyname.github.io/index.html

KROM Engine is a modern C++20 engine focused on render architecture and extensibility, built around proven game development patterns such as ECS, RenderGraph, and a plugin-based backend system. Its architecture is designed to keep the core clean and backend-agnostic, while platform-, renderer-, and feature-specific logic is moved into dedicated AddOns.

This AddOn model makes it possible to integrate and evolve backends such as DX11, DX12, OpenGL, Vulkan, or Metal without polluting the engine core with API-specific code. In addition, rendering features such as Forward, PBR, or Post-Processing can also be implemented as modular AddOns. Post-Processing is treated not as API-specific logic, but as a rendering feature built on top of the engine’s abstractions, while the actual execution is still handled by the respective graphics backends.

This keeps the engine more flexible, easier to maintain, and significantly better suited for long-term growth.
