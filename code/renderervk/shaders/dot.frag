#version 450
#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
#endif

layout(set = 0, binding = 0) buffer SSBO {
	#ifdef MULTIVIEW
	uint counts[4]; // fragments passed per eye, then fragments drawn per eye
#else
	int sampled;
#endif
};

layout(location = 0) out vec4 out_color;
layout(early_fragment_tests) in; // force Early Fragment Tests

void main() {
	//atomicAdd( sampled, 1 );
#ifdef MULTIVIEW
#ifdef USE_TOTAL
	atomicAdd(counts[2 + gl_ViewIndex], 1u);
#else
	atomicAdd(counts[gl_ViewIndex], 1u);
#endif
#else
	sampled = 1;
#endif
	discard;
	//out_color = vec4( 0.0, 1.0, 0.0, 1.0 );
}
