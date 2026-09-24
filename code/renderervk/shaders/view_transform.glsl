// Shared mono/MV transform ABI.
#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
layout(set = 0, binding = 1, std140) uniform ViewTransform {
    mat4 eyeProj[2];
};
#endif
layout(push_constant) uniform Transform {
    mat4 mvp; // mono: complete MVP; multiview: center modelview times model
};
vec4 transformPosition(vec3 position) {
#ifdef MULTIVIEW
    return eyeProj[gl_ViewIndex] * (mvp * vec4(position, 1.0));
#else
    return mvp * vec4(position, 1.0);
#endif
}
