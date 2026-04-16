#version 410 core
#extension GL_ARB_shading_language_420pack : enable

out vec2 vTexCoord;

void main()
{
    const vec2 pos[3] = vec2[3](
        vec2(-1.0,  1.0),
        vec2(-1.0, -3.0),
        vec2( 3.0,  1.0)
    );
    const vec2 uv[3] = vec2[3](
        vec2(0.0, 0.0),
        vec2(0.0, 2.0),
        vec2(2.0, 0.0)
    );

    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
    vTexCoord = uv[gl_VertexID];
}
