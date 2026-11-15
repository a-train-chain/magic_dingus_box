// fullscreen.vert - the one vertex shader of every Tube program. The renderer
// draws a two-triangle quad already in clip space (attribute 0 = position,
// attribute 1 = texCoord, the renderer's VAO layout); the fragment programs work
// from gl_FragCoord, so texCoord is unused. Assembled with the version line only
// (common.glsl is fragment-only).
layout(location = 0) in vec2 position;
layout(location = 1) in vec2 texCoord;

void main()
{
    gl_Position = vec4(position, 0.0, 1.0);
}
