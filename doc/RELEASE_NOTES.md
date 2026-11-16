# KROM Engine — Release Notes

## 2026-11-16 Release Notes — Editor Overhaul, Animation, Particles, and Image Quality

Current version: 0.9 – The engine is still under development.

- Rebuilt the editor on ImGui docking with a new toolbar, launcher, color theme, Lucide icons, and detachable multi-viewport windows
- Play now runs directly inside the editor, with Viewport and Game visible side by side
- Added C++ script hot reload: project scripts are built asynchronously and swapped without rebuilding scenes
- Added German and English localization to the editor
- Introduced skeletal animation with Blender clips, animation controllers, a graph editor with live preview, clip events, and a script API
- Introduced a particle system with CPU simulation, textures, flipbooks, ground collision, .particle assets, a particle editor, and script control
- Added a node-based material graph with UV, position, and vector nodes and editable UV transforms
- Added local reflection probes with box projection, blending, specular occlusion, and baked .kprobe assets
- Added screen-space reflections through an opaque MRT pass
- Added MSAA (off/2x/4x), filmic tonemap operators, and exposure control to the forward pipeline
- Added PCSS and rotated Poisson shadow filters, BRDF options, and per-view shadow caster culling
- Improved GTAO with history reprojection under camera motion and current-frame depth input
- Moved the worker pool to the engine host and build shader caches and missing variants in the background
- Added GPU timings per render-graph pass and a new profiler window in the editor
- Added hierarchy folders, drag and drop, type filters, prefab mode, and a component clipboard
- Added on-screen text for scripts and HUDs, fixed-update timing, time scale, and physics interpolation
- Made all paths UTF-8 across the engine, editor builds, and project export
- Export now builds a standalone release with prefab/material warm-up, bundled DXC, and third-party license notices
- The editor finds Visual Studio automatically and accepts custom build and shader tool paths
- Faster undo, with viewport drags and transform edits recorded as single steps
- OpenGL is frozen: it still builds and runs, but receives no new features

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
