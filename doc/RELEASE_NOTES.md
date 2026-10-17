# KROM Engine — Release Notes

## 2026-10-18 Release Notes — Physics, Gameplay Scripting, Audio, and Project Workflow

Current version: 0.9 – The engine is still under development.

- Introduced a Physics AddOn built on Jolt Physics, kept fully out of the engine core
- Added rigid bodies (static, dynamic, kinematic) with box, sphere, and capsule colliders
- Added compound bodies: colliders on child objects combine into a single physical body
- Added a character controller for players and NPCs that walks over slopes and steps
- Added exact physics raycasts against rotated boxes, spheres, capsules, and characters
- Introduced an Audio AddOn built on miniaudio with 2D/3D sources, a listener, and one-shot sounds
- Introduced a C++ gameplay API for scripts covering components, transforms, input, spawning, and scene loading
- Scripts can apply forces, impulses, and velocities, and control characters and audio
- Added trigger events, physics collision callbacks, and events between scripts
- Prefabs now preserve scripts, collision shapes, physics, and audio components
- Unified the scene format so runtime scenes share the editor's serializer format
- Completed the project workflow: create a project, play it in the runtime, and export a standalone build
- Added rigidbody, collider, character, and audio editing to the editor, including collider wireframes
- Added undo for adding, removing, and editing components in the inspector
- Custom-shader materials now show and pass PBR factors only when the shader declares them
- Added grid snapping to the editor viewport and remembered gizmo mode across sessions
- Hardened the Vulkan backend by fixing validation errors in the shadow, sky, GTAO, and outline passes
- Restructured the editor UI code into focused modules for easier maintenance
- Added continuous integration that builds the engine and runs the tests on every push
- Expanded the documentation for gameplay systems, physics, audio, scripting, and the add-on structure

## 2026-06-14 Release Notes — Forward+ Lighting, Shadows, and Editor Tooling

Current version: 0.8 – The engine is still under development.

- Introduced a Forward+ rendering path with light culling for many dynamic lights
- Moved shadow rendering into a dedicated, optional Shadow AddOn
- Added Cascaded Shadow Maps (CSM) for higher-quality directional shadows
- Introduced an offline IBL bake with a cached format for fast load times
- Reduced IBL memory footprint by switching cached data to a more compact format
- Added screen-space ambient occlusion (GTAO) applied to the ambient term
- Reworked the material rendering pipeline with per-submesh material support
- Added a clearer fallback chain for missing or incomplete materials
- Introduced an ImGui-based Editor AddOn with a dedicated UI pass
- Added a scene system: author scenes in the editor and load them at runtime
- Hardened the Vulkan backend with corrected buffer and descriptor handling
- Improved Forward+ structured-buffer binding and push-constant usage on Vulkan
- Kept the core clean by delivering lighting, shadows, and tooling as AddOns
- Continued strengthening backend separation across DX11, DX12, Vulkan, and OpenGL

## 2026-05-04 Release Notes — Add-on Lifecycle and Shutdown Hardening

- Introduced a cleaner and more extensible add-on architecture
- Made engine features easier to integrate without large rewrites
- Improved separation between core systems and optional runtime modules
- Simplified add-on bootstrap and registration across example applications
- Strengthened lifecycle management for add-ons and runtime systems
- Added safer rollback behavior during initialization and shutdown
- Improved stability when reloading or restarting engine subsystems
- Hardened renderer cleanup and reduced shutdown-related edge cases
- Removed friction around normal example exit and window close behavior
- Continued refining the engine foundation for more modular future expansion

## 2026-04-19 Release Notes — Architecture and Rendering Overhaul

Current version: 0.7 – The engine is still under development.

- Expanded and better structured the addon architecture
- Cleaned up renderer core internals and clarified responsibilities
- Removed legacy material paths and unnecessary alias indirections
- Introduced a centralized core shader pipeline
- Simplified and refined frame, pipeline, and build contracts
- Improved backend separation across DX11, DX12, Vulkan, and OpenGL
- Established HLSL as the primary path for modern backends
- Kept OpenGL 4.1 as an isolated legacy path for macOS
