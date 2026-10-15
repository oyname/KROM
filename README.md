![KROM Engine](https://github.com/oyname/oyname.github.io/blob/main/oynameengine_logo.png)

## in development

Main page: https://oyname.github.io/index.html

KROM Engine is a modern C++20 engine focused on render architecture and extensibility, built around proven game development patterns such as ECS, RenderGraph, and a plugin-based backend system. Its architecture is designed to keep the core clean and backend-agnostic, while platform-, renderer-, and feature-specific logic is moved into dedicated AddOns.

This AddOn model makes it possible to integrate and evolve backends such as DX11, DX12, OpenGL, Vulkan, or Metal without polluting the engine core with API-specific code. In addition, rendering features such as Forward, PBR, or Post-Processing can also be implemented as modular AddOns. Post-Processing is treated not as API-specific logic, but as a rendering feature built on top of the engine’s abstractions, while the actual execution is still handled by the respective graphics backends.

This keeps the engine more flexible, easier to maintain, and significantly better suited for long-term growth.

## Note: physics (Jolt) is downloaded on first configure

The physics AddOn (`addons/physics`) uses [Jolt Physics](https://github.com/jrouwe/JoltPhysics) (v5.5.0). Jolt is not part of this repository; CMake fetches it via `FetchContent` the first time the project is configured, so an internet connection is required once. The sources end up in the build folder (`out/build/<config>/_deps/`), not in the repository. Deleting the build folder triggers a new download.

To build without physics (no download, no Jolt), configure with `-DKROM_WITH_PHYSICS=OFF`. Scenes and prefabs that contain physics components still load; they are just not simulated.

## Note: textures are not included

The folder `projects/zweitesProjekt/Assets/Textures/` (about 200 MB) is excluded from this repository via `.gitignore`.
The project still builds and runs, but materials that reference these textures fall back to default materials, and the HDR environments (`*.hdr`) for skybox/IBL are missing.
To use textures, place your own files with the same names in that folder or assign other textures to the materials in the editor.
