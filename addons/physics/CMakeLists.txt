# engine_physics_data: Komponenten + Serialisierung, ohne Jolt.
# Der Editor linkt nur diese Library und baut daher auch ohne Jolt.
add_library(engine_physics_data STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/PhysicsSerialization.cpp
)
target_include_directories(engine_physics_data
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/addons/physics>
)
target_link_libraries(engine_physics_data
    PUBLIC
        engine_core
)

# engine_physics: Simulation. Jolt wird PRIVATE gelinkt — kein oeffentlicher
# Header dieses Addons bindet Jolt ein.
if(TARGET Jolt)
    add_library(engine_physics STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/PhysicsWorld.cpp
    )
    target_include_directories(engine_physics
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
            $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/addons/physics>
    )
    target_link_libraries(engine_physics
        PUBLIC
            engine_physics_data
            engine_core
        PRIVATE
            Jolt
    )
endif()
