/*
===========================================================================
macOS HDR (Extended Dynamic Range) for the Vulkan/MoltenVK layer.

MoltenVK sets the swapchain layer's colorspace but not wantsExtendedDynamicRangeContent,
so scRGB values above 1.0 clamp to SDR until we set that flag on the presentation layer.
macOS reports the live EDR headroom (1.0 = SDR white); the renderer scales into it.

Avoids SDL_syswm.h, whose bundled (Windows) config drags in <windows.h>; the render
window is found via AppKit instead.
===========================================================================
*/

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

// SDL adds its Metal view as a subview of the content view; search the tree.
static CAMetalLayer *FindMetalLayer( NSView *view )
{
	if ( !view )
		return nil;
	if ( [view.layer isKindOfClass:[CAMetalLayer class]] )
		return (CAMetalLayer *)view.layer;
	for ( NSView *sub in view.subviews ) {
		CAMetalLayer *found = FindMetalLayer( sub );
		if ( found )
			return found;
	}
	return nil;
}

// The render window is the one whose view tree owns the presentation layer.
static CAMetalLayer *FindRenderLayer( NSWindow **outWindow )
{
	for ( NSWindow *w in [NSApp windows] ) {
		CAMetalLayer *layer = FindMetalLayer( [w contentView] );
		if ( layer ) {
			if ( outWindow )
				*outWindow = w;
			return layer;
		}
	}
	if ( outWindow )
		*outWindow = nil;
	return nil;
}

// Toggle extended-dynamic-range on the presentation layer; returns the display's
// potential EDR headroom (>1 when the display is HDR-capable), 1.0 otherwise.
float Sys_MacOS_ConfigureHDRLayer( int enable )
{
	NSWindow *win = nil;
	CAMetalLayer *layer = FindRenderLayer( &win );
	NSScreen *screen;

	if ( !layer || !win )
		return 1.0f;

	if ( enable ) {
		layer.wantsExtendedDynamicRangeContent = YES;
		CGColorSpaceRef cs = CGColorSpaceCreateWithName( kCGColorSpaceExtendedLinearSRGB );
		if ( cs ) {
			layer.colorspace = cs;
			CGColorSpaceRelease( cs );
		}
	} else {
		layer.wantsExtendedDynamicRangeContent = NO;
	}

	screen = [win screen];
	if ( !screen )
		return 1.0f;
	return (float)screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
}

// Current (live) EDR headroom of the render window's screen: 1.0 = SDR white,
// higher once macOS grants EDR budget. Drives the HDR highlight ceiling.
float Sys_MacOS_CurrentEDRHeadroom( void )
{
	NSWindow *win = nil;
	NSScreen *screen;

	if ( !FindRenderLayer( &win ) || !win )
		return 1.0f;

	screen = [win screen];
	if ( !screen )
		return 1.0f;
	return (float)screen.maximumExtendedDynamicRangeColorComponentValue;
}

#endif // __APPLE__
