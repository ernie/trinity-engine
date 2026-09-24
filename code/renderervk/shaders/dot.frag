#version 450
#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif

layout(set = 0, binding = 0) buffer SSBO {
	#ifdef MULTIVIEW
	int sampled[2];
#else
	int sampled;
#endif
};

layout(location = 0) out vec4 out_color;
layout(early_fragment_tests) in; // force Early Fragment Tests

void main() {
	//atomicAdd( sampled, 1 );
#ifdef MULTIVIEW
	atomicExchange(sampled[gl_ViewIndex], 1);
#else
	sampled = 1;
#endif
	discard;
	//out_color = vec4( 0.0, 1.0, 0.0, 1.0 );
}
