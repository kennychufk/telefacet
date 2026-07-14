#version 330 core

// Fullscreen triangle covering the viewport, rendering in the raw YUV buffer's
// native orientation, matching telefacet-web/src/webgl/Debayer.js.
out vec2 v_texCoord;

void main() {
    // Triangle that covers (-1,-1)..(3,3) in clip space, so its rasterized
    // region exactly fills the viewport. No vertex buffer required.
    vec2 pos = vec2((gl_VertexID & 1) * 4 - 1,
                    (gl_VertexID & 2) * 2 - 1);
    gl_Position = vec4(pos, 0.0, 1.0);

    // Untransformed UV in [0,2]x[0,2]; clip restricts to [0,1]x[0,1].
    vec2 uv = vec2((gl_VertexID & 1) * 2, (gl_VertexID & 2));
    // Render in the raw buffer's native orientation (no rotation).
    v_texCoord = uv;
}
