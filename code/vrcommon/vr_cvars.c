#include "vr_cvars.h"
#include "vr_defaults.h"

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../client/client.h"


cvar_t *vr_worldscale = NULL;
cvar_t *vr_worldscaleScaler = NULL;
cvar_t *vr_currentHudDepth = NULL;
cvar_t *vr_righthanded = NULL;
cvar_t *vr_switchThumbsticks = NULL;
cvar_t *vr_snapturn = NULL;
cvar_t *vr_heightAdjust = NULL;
cvar_t *vr_directionMode = NULL;
cvar_t *vr_weaponPitch = NULL;
cvar_t *vr_twoHandedWeapons = NULL;
cvar_t *vr_refreshrate = NULL;
cvar_t *vr_refreshrates = NULL;
cvar_t *vr_superSampling = NULL;
cvar_t *vr_weaponScope = NULL;
cvar_t *vr_6dof = NULL;
cvar_t *vr_hudYOffset = NULL;
cvar_t *vr_hudScale = NULL;
cvar_t *vr_sendRollToServer = NULL;
cvar_t *vr_hapticIntensity = NULL;
cvar_t *vr_bhaptics = NULL;
cvar_t *vr_weaponSelectorMode = NULL;
cvar_t *vr_currentHudDrawStatus = NULL;
cvar_t *vr_showConsoleMessages = NULL;
cvar_t *vr_desktopContentFit = NULL;
cvar_t *vr_desktopContentType = NULL;
cvar_t *vr_desktopMenuStyle = NULL;
cvar_t *vr_mirrorEnabled = NULL;
cvar_t *vr_mirrorFullscreen = NULL;
cvar_t *vr_mirrorWidth = NULL;
cvar_t *vr_mirrorHeight = NULL;
cvar_t *vr_virtualScreenMode = NULL;
cvar_t *vr_screenCurvature = NULL;
cvar_t *vr_weaponAdjust = NULL;
cvar_t *vr_thumbstickDeadzone = NULL;
cvar_t *vr_thumbstickFullDeflection = NULL;
cvar_t *vr_triggerSensitivity = NULL;
cvar_t *vr_analogWalk = NULL;
cvar_t *vr_foveation = NULL;
cvar_t *vr_foveationStrength = NULL;
cvar_t *vr_foveationCaps = NULL;

void VR_InitMirrorCvars( void ) {
	vr_mirrorEnabled = Cvar_Get( "vr_mirrorEnabled", VR_MIRROR_DEFAULT, VR_MODE_CVAR_FLAGS );
	vr_mirrorFullscreen = Cvar_Get( "vr_mirrorFullscreen", "0", CVAR_ARCHIVE | CVAR_LATCH | CVAR_NORESTART );
	vr_mirrorWidth = Cvar_Get( "vr_mirrorWidth", "1280", CVAR_ARCHIVE | CVAR_LATCH | CVAR_NORESTART );
	vr_mirrorHeight = Cvar_Get( "vr_mirrorHeight", "720", CVAR_ARCHIVE | CVAR_LATCH | CVAR_NORESTART );
	Cvar_CheckRange( vr_mirrorEnabled, "0", "1", CV_INTEGER );
	Cvar_CheckRange( vr_mirrorFullscreen, "0", "1", CV_INTEGER );
	Cvar_CheckRange( vr_mirrorWidth, "320", "16384", CV_INTEGER );
	Cvar_CheckRange( vr_mirrorHeight, "240", "16384", CV_INTEGER );
}

void VR_InitCvars( void )
{
	cvar_t *vr_hudDepth;
	int controlSchema;
	qboolean uturnEnabled;
	qboolean switchThumbsticks;
	Cvar_Get( "vr_platform", "pc", CVAR_ROM );	// advertise the VR platform to UI modules
	vr_worldscale = Cvar_Get( "vr_worldscale", "32.0", CVAR_ARCHIVE );
	vr_worldscaleScaler = Cvar_Get( "vr_worldscaleScaler", "1.0", CVAR_ARCHIVE );
	vr_hudDepth = Cvar_Get( "vr_hudDepth", "3", CVAR_ARCHIVE );
	Cvar_CheckRange( vr_hudDepth, "0", "5", CV_INTEGER );
	vr_righthanded = Cvar_Get( "vr_righthanded", "1", CVAR_ARCHIVE );
	vr_switchThumbsticks = Cvar_Get( "vr_switchThumbsticks", "0", CVAR_ARCHIVE );
	vr_snapturn = Cvar_Get( "vr_snapturn", "0", CVAR_ARCHIVE );
	vr_directionMode = Cvar_Get( "vr_directionMode", "1", CVAR_ARCHIVE ); // 0 = HMD, 1 = Off-hand
	// Degrees on top of the fixed VR_GRIP_TO_AIM_PITCH correction; zero is no personal adjustment
	vr_weaponPitch = Cvar_Get( "vr_weaponPitch", "0", CVAR_ARCHIVE );
	vr_heightAdjust = Cvar_Get( "vr_heightAdjust", "0.0", CVAR_ARCHIVE );
	vr_twoHandedWeapons = Cvar_Get( "vr_twoHandedWeapons", "0", CVAR_ARCHIVE );
	Cvar_Get( "vr_showItemInHand", "1", CVAR_ARCHIVE );
	vr_refreshrate = Cvar_Get( "vr_refreshrate", "90", CVAR_ARCHIVE );
	vr_refreshrates = Cvar_Get( "vr_refreshrates", "", CVAR_ROM );	// space-separated rates the runtime supports, for the UI
	vr_superSampling = Cvar_Get( "vr_superSampling", "1.0", CVAR_ARCHIVE );
	vr_weaponScope = Cvar_Get( "vr_weaponScope", "1", CVAR_ARCHIVE );
	vr_6dof = Cvar_Get( "vr_6dof", "0", CVAR_ARCHIVE ); // 0 - fake 6DoF in SP, 1 - true 6DoF in SP
	Cvar_Get( "vr_rollWhenHit", "0", CVAR_ARCHIVE );
	vr_hudYOffset = Cvar_Get( "vr_hudYOffset", "0", CVAR_ARCHIVE );
	vr_hudScale = Cvar_Get( "vr_hudScale", "1", CVAR_ARCHIVE );
	vr_sendRollToServer = Cvar_Get( "vr_sendRollToServer", "1", CVAR_ARCHIVE );
	Cvar_Get( "vr_lasersight", "0", CVAR_ARCHIVE );
	vr_hapticIntensity = Cvar_Get( "vr_hapticIntensity", "0.5", CVAR_ARCHIVE );
	vr_bhaptics = Cvar_Get( "vr_bhaptics", "0", CVAR_ARCHIVE );
	Cvar_Get( "vr_comfortVignette", "0.0", CVAR_ARCHIVE );
	vr_weaponSelectorMode = Cvar_Get( "vr_weaponSelectorMode", "0", CVAR_ARCHIVE );
	Cvar_Get( "vr_weaponSelectorWithHud", "0", CVAR_ARCHIVE );
	Cvar_Get( "vr_hudDrawStatus", "1", CVAR_ARCHIVE ); // 0 - no hud, 1 - in-world hud, 2 - performance (static HUD)
	vr_currentHudDrawStatus = Cvar_Get( "vr_currentHudDrawStatus", "1", 0 ); // 0 - no hud, 1 - in-world hud, 2 - performance (static HUD)
	vr_currentHudDepth = Cvar_Get( "vr_currentHudDepth", "3", 0 );  // Runtime copy, not archived
	vr_showConsoleMessages = Cvar_Get( "vr_showConsoleMessages", "1", CVAR_ARCHIVE );
	vr_desktopContentFit = Cvar_Get( "vr_desktopContentFit", "1", CVAR_ARCHIVE ); // 0 - fit/contain, 1 - fill/crop
	vr_desktopContentType = Cvar_Get( "vr_desktopContentType", "0", CVAR_ARCHIVE ); // 0 - left eye, 1 - right eye, 2 - both eyes
	vr_desktopMenuStyle = Cvar_Get( "vr_desktopMenuStyle", "0", CVAR_ARCHIVE ); // 0 - desktop view, 1 - VR view
	VR_InitMirrorCvars();
	vr_virtualScreenMode = Cvar_Get( "vr_virtualScreenMode", "0", CVAR_ARCHIVE ); // 0 - fixed, 1 - follow
	vr_screenCurvature = Cvar_Get( "vr_screenCurvature", "0.5", CVAR_ARCHIVE ); // 0 - flat, 1 - tightest curve
	Cvar_CheckRange( vr_screenCurvature, "0", "1", CV_FLOAT );
	vr_weaponAdjust = Cvar_Get( "vr_weaponAdjust", "0", CVAR_ARCHIVE ); // 0 - disabled, 1 - enabled
	vr_thumbstickDeadzone = Cvar_Get( "vr_thumbstickDeadzone", "0.15", CVAR_ARCHIVE );
	vr_thumbstickFullDeflection = Cvar_Get( "vr_thumbstickFullDeflection", "0.85", CVAR_ARCHIVE );
	vr_triggerSensitivity = Cvar_Get( "vr_triggerSensitivity", "0.25", CVAR_ARCHIVE );
	Cvar_CheckRange( vr_triggerSensitivity, "0.1", "0.9", CV_FLOAT );
	vr_analogWalk = Cvar_Get( "vr_analogWalk", "1", CVAR_ARCHIVE ); // 0 - classic always-run, 1 - silent walk below run speed

	// Prefer eye tracking, falling back to fixed foveation or disabled as supported.
	vr_foveation = Cvar_Get( "vr_foveation", "2", CVAR_ARCHIVE );
	Cvar_CheckRange( vr_foveation, "0", "2", CV_INTEGER );

	vr_foveationStrength = Cvar_Get( "vr_foveationStrength", "2", CVAR_ARCHIVE );
	Cvar_CheckRange( vr_foveationStrength, "1", "3", CV_INTEGER );

	// The Vulkan XR backend publishes capabilities after device initialization.
	vr_foveationCaps = Cvar_Get( "vr_foveationCaps", "none", CVAR_ROM );

	// Values are:  scale,right,up,forward,pitch,yaw,roll
	Cvar_Get( "vr_weapon_adjustment_1", "1,-4.0,7,-10,-20,-15,0", CVAR_ARCHIVE );
	Cvar_Get( "vr_weapon_adjustment_2", "0.8,-3.0,5.5,0,0,0,0", CVAR_ARCHIVE );
	Cvar_Get( "vr_weapon_adjustment_3", "0.8,-3.3,8,3.7,0,0,0", CVAR_ARCHIVE ); // shotgun
	Cvar_Get( "vr_weapon_adjustment_4", "0.75,-5.4,6.5,-4,0,0,0", CVAR_ARCHIVE );
	Cvar_Get( "vr_weapon_adjustment_5", "0.8,-5.2,6,7.5,0,0,0", CVAR_ARCHIVE );
	Cvar_Get( "vr_weapon_adjustment_6", "0.8,-3.3,6,7,0,0,0", CVAR_ARCHIVE );
	Cvar_Get( "vr_weapon_adjustment_7", "0.8,-5.5,6,0,0,0,0", CVAR_ARCHIVE );
	Cvar_Get( "vr_weapon_adjustment_8", "0.8,-4.5,6,1.5,0,0,0", CVAR_ARCHIVE );
	Cvar_Get( "vr_weapon_adjustment_9", "0.8,-5.5,6,0,0,0,0", CVAR_ARCHIVE );
	Cvar_Get( "vr_weapon_adjustment_10", "0.8,-2.75,6,-1.25,0,0,0", CVAR_ARCHIVE );

	//Team Arena Weapons
	Cvar_Get( "vr_weapon_adjustment_11", "0.8,-5.5,6,0,0,0,0", CVAR_ARCHIVE );
	Cvar_Get( "vr_weapon_adjustment_12", "0.8,-5.5,6,0,0,0,0", CVAR_ARCHIVE );
	Cvar_Get( "vr_weapon_adjustment_13", "0.8,-5.5,6,0,0,0,0", CVAR_ARCHIVE );

	Cvar_Get( "vr_uturn", "0", CVAR_ARCHIVE );
	Cvar_Get( "vr_controlSchema", "0", CVAR_ARCHIVE );
	uturnEnabled = Cvar_VariableValue( "vr_uturn" ) != 0;
	controlSchema = (int)Cvar_VariableValue( "vr_controlSchema" ) % 3;
	if ( controlSchema == 0 ) {
		// Default schema (weapon wheel on grip)
		Cvar_Get( "vr_button_map_RTHUMBLEFT", "turnleft", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBRIGHT", "turnright", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBFORWARD", "", CVAR_ARCHIVE );
		if ( uturnEnabled ) {
			Cvar_Get( "vr_button_map_RTHUMBBACK", "uturn", CVAR_ARCHIVE );
		} else {
			Cvar_Get( "vr_button_map_RTHUMBBACK", "", CVAR_ARCHIVE );
		}
		Cvar_Get( "vr_button_map_PRIMARYGRIP", "+weapon_select", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_PRIMARYTHUMBSTICK", "", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBFORWARD_ALT", "", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBRIGHT_ALT", "", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBBACK_ALT", "", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBLEFT_ALT", "", CVAR_ARCHIVE );
	} else if ( controlSchema == 1 ) {
		// Weapon wheel on thumbstick - all directions as weapon select (useful for HMD wheel)
		Cvar_Get( "vr_button_map_RTHUMBFORWARD", "+weapon_select", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBRIGHT", "+weapon_select", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBBACK", "+weapon_select", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBLEFT", "+weapon_select", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_PRIMARYTHUMBSTICK", "+weapon_select", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_PRIMARYGRIP", "+alt", CVAR_ARCHIVE ); // switch to alt layout
		Cvar_Get( "vr_button_map_RTHUMBLEFT_ALT", "turnleft", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBRIGHT_ALT", "turnright", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBFORWARD_ALT", "blank", CVAR_ARCHIVE );
		if ( uturnEnabled ) {
			Cvar_Get( "vr_button_map_RTHUMBBACK_ALT", "uturn", CVAR_ARCHIVE );
		} else {
			Cvar_Get( "vr_button_map_RTHUMBBACK_ALT", "blank", CVAR_ARCHIVE );
		}
	} else {
		// Weapon wheel disabled - only prev/next weapon switch is active
		Cvar_Get( "vr_button_map_RTHUMBLEFT", "turnleft", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBRIGHT", "turnright", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBFORWARD", "weapnext", CVAR_ARCHIVE );
		if ( uturnEnabled ) {
			Cvar_Get( "vr_button_map_RTHUMBBACK", "uturn", CVAR_ARCHIVE );
		} else {
			Cvar_Get( "vr_button_map_RTHUMBBACK", "weapprev", CVAR_ARCHIVE );
		}
		Cvar_Get( "vr_button_map_PRIMARYGRIP", "+alt", CVAR_ARCHIVE ); // switch to alt layout
		Cvar_Get( "vr_button_map_PRIMARYTHUMBSTICK", "", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBFORWARD_ALT", "", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBRIGHT_ALT", "", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBBACK_ALT", "", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_RTHUMBLEFT_ALT", "", CVAR_ARCHIVE );
	}

	// Face buttons follow the movement thumbstick: one thumb cannot move and jump at once
	switchThumbsticks = Cvar_VariableValue( "vr_switchThumbsticks" ) != 0;
	if ( switchThumbsticks ) {
		Cvar_Get( "vr_button_map_A", "+button2", CVAR_ARCHIVE ); // Use Item
		Cvar_Get( "vr_button_map_B", "togglemenu", CVAR_ARCHIVE );
		Cvar_Get( "vr_button_map_X", "+moveup", CVAR_ARCHIVE ); // Jump
		Cvar_Get( "vr_button_map_Y", "+movedown", CVAR_ARCHIVE ); // Crouch
	} else {
		Cvar_Get( "vr_button_map_A", "+moveup", CVAR_ARCHIVE ); // Jump
		Cvar_Get( "vr_button_map_B", "+movedown", CVAR_ARCHIVE ); // Crouch
		Cvar_Get( "vr_button_map_X", "+button2", CVAR_ARCHIVE ); // Use Item
		Cvar_Get( "vr_button_map_Y", "togglemenu", CVAR_ARCHIVE );
	}

	//Remaining button mapping (buttons not affected by schemas)
	Cvar_Get( "vr_button_map_A_ALT", "", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_B_ALT", "", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_X_ALT", "", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_Y_ALT", "", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_SECONDARYTHUMBSTICK", "+scores", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_SECONDARYTHUMBSTICK_ALT", "", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_PRIMARYTHUMBSTICK_ALT", "", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_SECONDARYTRIGGER", "+moveup", CVAR_ARCHIVE ); // Also Jump
	Cvar_Get( "vr_button_map_SECONDARYTRIGGER_ALT", "", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_PRIMARYTRIGGER", "+attack", CVAR_ARCHIVE ); // Fire
	Cvar_Get( "vr_button_map_PRIMARYTRIGGER_ALT", "", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_SECONDARYGRIP", "+weapon_stabilise", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_SECONDARYGRIP_ALT", "", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_PRIMARYGRIP_ALT", "", CVAR_ARCHIVE );

	Cvar_Get( "vr_button_map_PRIMARYTRACKPAD", "+scores", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_PRIMARYTRACKPAD_ALT", "", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_SECONDARYTRACKPAD", "togglemenu", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_SECONDARYTRACKPAD_ALT", "", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_PRIMARYTHUMBREST", "+alt", CVAR_ARCHIVE ); // Alt modifier
	Cvar_Get( "vr_button_map_PRIMARYTHUMBREST_ALT", "", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_SECONDARYTHUMBREST", "+alt", CVAR_ARCHIVE ); // Alt modifier
	Cvar_Get( "vr_button_map_SECONDARYTHUMBREST_ALT", "", CVAR_ARCHIVE );

	// Steam Frame bumpers; other controllers never press these slots
	Cvar_Get( "vr_button_map_LBUMPER", "+alt", CVAR_ARCHIVE );
	Cvar_Get( "vr_button_map_RBUMPER", "+alt", CVAR_ARCHIVE );
}
