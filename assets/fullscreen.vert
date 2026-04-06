// =============================================================================
// KROM Engine — assets/fullscreen.vert
// Fullscreen-Dreieck aus gl_VertexID — kein Vertex-Buffer nötig.
// glDrawArrays(GL_TRIANGLES, 0, 3) genügt.
// CCW-Reihenfolge damit Standard-Backface-Culling den Postprocess-Pass
// nicht wegcullt.
// =============================================================================
#version 410 core
#extension GL_ARB_shading_language_420pack : enable

out vec2 vUV;

void main()
{
    if (gl_VertexID == 0)
    {
        gl_Position = vec4(-1.0,  1.0, 0.0, 1.0);
        vUV         = vec2(0.0, 0.0);
    }
    else if (gl_VertexID == 1)
    {
        gl_Position = vec4(-1.0, -3.0, 0.0, 1.0);
        vUV         = vec2(0.0, 2.0);
    }
    else
    {
        gl_Position = vec4( 3.0,  1.0, 0.0, 1.0);
        vUV         = vec2(2.0, 0.0);
    }
}
