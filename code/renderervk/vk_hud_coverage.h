/* Opaque replacement stages establish coverage after alpha testing. Shine
 * stages preserve it; separate translucent materials retain their own alpha. */
static ID_INLINE int VK_HudCoverage( int is2D, unsigned first, unsigned current ) {
	if ( is2D || (first & GLS_BLEND_BITS) ) {
		return 0;
	}
	return (current & GLS_BLEND_BITS) ? 2 : 1;
}

/* RGB blended over a transparent target is premultiplied. Accumulate coverage
 * independently: source-over must not square the source alpha. Additive light
 * and opaque-material detail affect color without adding coverage. */
static ID_INLINE void VK_HudAlphaBlend( unsigned stateBits, int coverage,
	VkPipelineColorBlendAttachmentState *blend ) {
	if ( coverage == 2 || (stateBits & GLS_DSTBLEND_BITS) == GLS_DSTBLEND_ONE ) {
		blend->srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		blend->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
	} else if ( (stateBits & GLS_BLEND_BITS) ==
		(GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA) ) {
		blend->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		blend->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
	}
}

/* The engine-owned panel samples premultiplied RGB. */
static ID_INLINE unsigned VK_HudCompositeBlend( unsigned stateBits ) {
	if ( (stateBits & GLS_BLEND_BITS) ==
		(GLS_SRCBLEND_SRC_ALPHA | GLS_DSTBLEND_ONE_MINUS_SRC_ALPHA) ) {
		stateBits = (stateBits & ~GLS_SRCBLEND_BITS) | GLS_SRCBLEND_ONE;
	}
	return stateBits;
}
