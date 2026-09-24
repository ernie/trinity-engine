#version 450
#extension GL_EXT_fragment_shading_rate : require
layout(location = 0) out vec4 out_color;
// Show the rate actually selected by the GPU.
void main() {
    int w = 1 << ((gl_ShadingRateEXT >> 2) & 3);
    int h = 1 << (gl_ShadingRateEXT & 3);
    int area = w * h;
    vec3 tint;
    if (area <= 1) tint = vec3(0.0);
    else if (area <= 2) tint = vec3(0.0, 1.0, 0.0);
    else if (area <= 4) tint = vec3(1.0, 1.0, 0.0);
    else if (area <= 8) tint = vec3(1.0, 0.5, 0.0);
    else tint = vec3(1.0, 0.0, 0.0);
    out_color = vec4(tint, area <= 1 ? 0.0 : 0.35);
}
