#include "client.h"
#include "cl_vr_bind.h"
#include "../vrcommon/vr_cvars.h"

typedef char vrKeyOrderCheck[(K_VR_DPAD_RIGHT - K_VR_WPN_TRIGGER + 1 == VRK_COUNT) ? 1 : -1];

static char *tables[2][VRC_COUNT][VRK_COUNT]; /* [alt]; plain gameplay lives in the ordinary bind table */
static int activeProfile = -1;
/* the key that is Escape while no button this controller has is bound to it, -1 otherwise */
static int escapeFallback = -1;

static int VRBind_KeyFromName( const char *name ) {
	int keynum = Key_StringToKeynum( name );
	return keynum >= K_VR_WPN_TRIGGER && keynum <= K_VR_DPAD_RIGHT ? keynum - K_VR_WPN_TRIGGER : -1;
}

static const char *VRBind_Stored( vrContext_t context, int alt, vrKey_t key, void *user ) {
	(void)user;
	if ( context == VRC_GAMEPLAY && !alt )
		return Key_GetBinding( K_VR_WPN_TRIGGER + key );
	return tables[alt][context][key];
}

const char *CL_VRBind_Lookup( vrContext_t context, int alt, vrKey_t key, void *user ) {
	/* the way back to the menu can't be lost, whatever the saved bindings hold */
	if ( context == VRC_GLOBAL && !alt && (int)key == escapeFallback )
		return "+key ESCAPE";
	return VRBind_Stored( context, alt, key, user );
}

static void VRBind_UpdateFallback( void ) {
	escapeFallback = activeProfile < 0 ? -1 : VR_EscapeFallback( activeProfile, VRBind_Stored, NULL );
}

static void VRBind_Set( vrContext_t context, int alt, vrKey_t key, const char *binding ) {
	if ( context == VRC_GAMEPLAY && !alt ) {
		Key_SetBinding( K_VR_WPN_TRIGGER + key, binding ? binding : "" );
		return;
	}
	if ( tables[alt][context][key] ) {
		Z_Free( tables[alt][context][key] );
		tables[alt][context][key] = NULL;
	}
	if ( binding && binding[0] )
		tables[alt][context][key] = CopyString( binding );
	cvar_modifiedFlags |= CVAR_ARCHIVE;
	if ( context == VRC_GLOBAL && !alt )
		VRBind_UpdateFallback();
}

/* A set's console name: "follow", "follow+alt". */
static const char *VRBind_SetName( int context, int alt ) {
	return va( "%s%s", VR_ContextName( (vrContext_t)context ), alt ? "+alt" : "" );
}

static qboolean VRBind_Tables( void ) {
	int alt, c, k;
	for ( alt = 0; alt < 2; alt++ )
		for ( c = 0; c < VRC_COUNT; c++ )
			for ( k = 0; k < VRK_COUNT; k++ )
				if ( tables[alt][c][k] )
					return qtrue;
	return qfalse;
}

static qboolean VRBind_Any( void ) {
	int k;
	for ( k = 0; k < VRK_COUNT; k++ ) {
		const char *b = Key_GetBinding( K_VR_WPN_TRIGGER + k );
		if ( b && b[0] )
			return qtrue;
	}
	return VRBind_Tables();
}

static void VRBind_Default( vrContext_t context, int alt, vrKey_t key, const char *binding, void *user ) {
	(void)user;
	VRBind_Set( context, alt, key, binding );
}

static void VRBind_ResetTo( int profile ) {
	int alt, c, k;
	for ( alt = 0; alt < 2; alt++ )
		for ( c = 0; c < VRC_COUNT; c++ )
			for ( k = 0; k < VRK_COUNT; k++ )
				VRBind_Set( (vrContext_t)c, alt, (vrKey_t)k, NULL );
	VR_ForEachDefault( profile, VRBind_Default, NULL );
}

static const char *VRBind_FamilyName( int profile ) {
	switch ( VR_ProfileFamily( profile ) ) {
		case VRF_INDEX:
			return "Valve Index";
		case VRF_FRAME:
			return "Steam Frame";
		case VRF_SIMPLE:
			return "simple controller";
		default:
			return "Touch";
	}
}

void CL_VRBind_SetProfile( int profile ) {
	if ( profile == activeProfile )
		return;
	activeProfile = profile;
	VRBind_UpdateFallback();
	if ( profile < 0 )
		return;
	if ( !VRBind_Any() ) {
		VRBind_ResetTo( profile );
		Com_Printf( "VR bindings set to the %s defaults\n", VRBind_FamilyName( profile ) );
	}
	if ( VR_ProfileFamily( profile ) == VRF_SIMPLE )
		Com_Printf( S_COLOR_YELLOW "This controller isn't supported for play. Use a supported controller or switch to "
								   "flatscreen.\n" );
}

int CL_VRBind_Profile( void ) {
	return activeProfile;
}

/* The keys the router would run command with in context, as engine key codes: "<key>" or "<alt>+<key>". */
static qboolean VRBind_KeysFor( const char *context, const char *command, char *buf, int size ) {
	int suffix, c = VR_ContextFromName( context, &suffix ), alt, k;
	if ( c < 0 || !buf || size <= 0 )
		return qfalse;
	k = VR_CommandKey( (vrContext_t)c, command, activeProfile, CL_VRBind_Lookup, NULL, &alt );
	if ( k < 0 )
		return qfalse;
	if ( alt < 0 )
		Com_sprintf( buf, size, "%i", K_VR_WPN_TRIGGER + k );
	else
		Com_sprintf( buf, size, "%i+%i", K_VR_WPN_TRIGGER + alt, K_VR_WPN_TRIGGER + k );
	return qtrue;
}

static void VRBind_PrintUnknownContext( const char *name ) {
	Com_Printf( "Unknown VR context \"%s\"; use global, menu, adjust, scrub, scoreboard, vote, wheel, follow or "
				"gameplay, each also as <context>+alt for Alt held\n",
				name );
}

static qboolean VRBind_Parse( int *context, int *alt, int *key ) {
	*context = VR_ContextFromName( Cmd_Argv( 1 ), alt );
	*key = VRBind_KeyFromName( Cmd_Argv( 2 ) );
	if ( *context < 0 ) {
		VRBind_PrintUnknownContext( Cmd_Argv( 1 ) );
		return qfalse;
	}
	if ( *key < 0 ) {
		Com_Printf( "\"%s\" isn't a VR key\n", Cmd_Argv( 2 ) );
		return qfalse;
	}
	return qtrue;
}

static void VRBind_Bind_f( void ) {
	int context, alt, key;
	const char *b;
	if ( Cmd_Argc() < 3 ) {
		Com_Printf( "usage: vrbind <context>[+alt] <key> [command]\n" );
		return;
	}
	if ( !VRBind_Parse( &context, &alt, &key ) )
		return;
	if ( Cmd_Argc() > 3 ) {
		VRBind_Set( (vrContext_t)context, alt, (vrKey_t)key, Cmd_ArgsFrom( 3 ) );
		return;
	}
	b = CL_VRBind_Lookup( (vrContext_t)context, alt, (vrKey_t)key, NULL );
	if ( b && b[0] )
		Com_Printf( "%s %s = \"%s\"\n", Cmd_Argv( 1 ), Cmd_Argv( 2 ), b );
	else
		Com_Printf( "%s %s is not bound\n", Cmd_Argv( 1 ), Cmd_Argv( 2 ) );
}

static void VRBind_Unbind_f( void ) {
	int context, alt, key;
	if ( Cmd_Argc() != 3 ) {
		Com_Printf( "usage: vrunbind <context>[+alt] <key>\n" );
		return;
	}
	if ( VRBind_Parse( &context, &alt, &key ) )
		VRBind_Set( (vrContext_t)context, alt, (vrKey_t)key, NULL );
}

static void VRBind_UnbindAll_f( void ) {
	int alt, c, k;
	for ( alt = 0; alt < 2; alt++ )
		for ( c = 0; c < VRC_COUNT; c++ )
			if ( alt || c != VRC_GAMEPLAY )
				for ( k = 0; k < VRK_COUNT; k++ )
					VRBind_Set( (vrContext_t)c, alt, (vrKey_t)k, NULL );
}

static void VRBind_List_f( void ) {
	int onlyAlt = 0, only = Cmd_Argc() > 1 ? VR_ContextFromName( Cmd_Argv( 1 ), &onlyAlt ) : -1, alt, c, k;
	if ( Cmd_Argc() > 1 && only < 0 ) {
		VRBind_PrintUnknownContext( Cmd_Argv( 1 ) );
		return;
	}
	for ( alt = 0; alt < 2; alt++ )
		for ( c = 0; c < VRC_COUNT; c++ ) {
			if ( only >= 0 && ( c != only || alt != onlyAlt ) )
				continue;
			for ( k = 0; k < VRK_COUNT; k++ ) {
				const char *b = CL_VRBind_Lookup( (vrContext_t)c, alt, (vrKey_t)k, NULL );
				if ( b && b[0] )
					Com_Printf( "%s %s \"%s\"\n", VRBind_SetName( c, alt ), Key_KeynumToString( K_VR_WPN_TRIGGER + k ),
								b );
			}
		}
}

static void VRBind_Reset_f( void ) {
	if ( activeProfile < 0 ) {
		Com_Printf( "No VR controller detected; start VR first.\n" );
		return;
	}
	VRBind_ResetTo( activeProfile );
	Com_Printf( "VR bindings reset to the %s defaults\n", VRBind_FamilyName( activeProfile ) );
}

static void VRBind_WriteContexts( fileHandle_t f ) {
	int alt, c, k;
	for ( alt = 0; alt < 2; alt++ )
		for ( c = 0; c < VRC_COUNT; c++ )
			for ( k = 0; k < VRK_COUNT; k++ )
				if ( tables[alt][c][k] )
					FS_Printf( f, "vrbind %s %s \"%s\"" Q_NEWLINE, VRBind_SetName( c, alt ),
							   Key_KeynumToString( K_VR_WPN_TRIGGER + k ), tables[alt][c][k] );
}

void CL_VRBind_Write( fileHandle_t f ) {
	if ( !VRBind_Tables() )
		return;
	FS_Printf( f, "vrunbindall" Q_NEWLINE );
	VRBind_WriteContexts( f );
}

void CL_VRBind_InitCommands( void ) {
	Cmd_AddCommand( "vrbind", VRBind_Bind_f );
	Cmd_AddCommand( "vrunbind", VRBind_Unbind_f );
	Cmd_AddCommand( "vrunbindall", VRBind_UnbindAll_f );
	Cmd_AddCommand( "vrbindlist", VRBind_List_f );
	Cmd_AddCommand( "vr_bindreset", VRBind_Reset_f );
}

qboolean CL_VRBind_GetValue( const char *key, char *value, int size ) {
	char context[32];
	const char *command;
	/* "recW recH maxW maxH": what supersampling scales and the most the headset allows. */
	if ( !Q_stricmp( key, "vr_eyesize" ) ) {
		int recW, recH, maxW, maxH;
		if ( !re.XREyeSize || !re.XREyeSize( &recW, &recH, &maxW, &maxH ) )
			return qfalse;
		Com_sprintf( value, size, "%i %i %i %i", recW, recH, maxW, maxH );
		return qtrue;
	}
	/* VR key codes differ between engines, so modules ask where they start. */
	if ( !Q_stricmp( key, "vr_keyfirst" ) ) {
		Com_sprintf( value, size, "%i", K_VR_WPN_TRIGGER );
		return qtrue;
	}
	if ( !Q_stricmpn( key, "vr_keyglyph ", 12 ) ) {
		const int k = atoi( key + 12 ) - K_VR_WPN_TRIGGER;
		if ( k < 0 || k >= VRK_COUNT )
			return qfalse;
		VR_KeyGlyph( (vrKey_t)k, vr_righthanded ? vr_righthanded->integer : 1,
					 vr_switchThumbsticks ? vr_switchThumbsticks->integer : 0, value, size );
		return qtrue;
	}
	if ( !Q_stricmpn( key, "vr_keydefault ", 14 ) ) {
		const char *index = strchr( key + 14, ' ' ), *bound = index ? strchr( index + 1, ' ' ) : NULL;
		int alt = 0, c, k;
		if ( !bound || index - ( key + 14 ) >= (int)sizeof( context ) || activeProfile < 0 )
			return qfalse;
		Q_strncpyz( context, key + 14, index - ( key + 14 ) + 1 );
		c = VR_ContextFromName( context, &alt );
		k = c < 0 ? -1 : VR_DefaultKey( activeProfile, (vrContext_t)c, alt, bound + 1, atoi( index + 1 ) );
		if ( k < 0 )
			return qfalse;
		Com_sprintf( value, size, "%i", K_VR_WPN_TRIGGER + k );
		return qtrue;
	}
	if ( !Q_stricmpn( key, "vr_binding ", 11 ) ) {
		const char *keynum = strchr( key + 11, ' ' ), *b;
		int alt = 0, c, k;
		if ( !keynum || keynum - ( key + 11 ) >= (int)sizeof( context ) )
			return qfalse;
		Q_strncpyz( context, key + 11, keynum - ( key + 11 ) + 1 );
		c = VR_ContextFromName( context, &alt );
		k = atoi( keynum + 1 ) - K_VR_WPN_TRIGGER;
		if ( c < 0 || k < 0 || k >= VRK_COUNT )
			return qfalse;
		b = CL_VRBind_Lookup( (vrContext_t)c, alt, (vrKey_t)k, NULL );
		Q_strncpyz( value, b ? b : "", size );
		return qtrue;
	}
	if ( Q_stricmpn( key, "vr_bindkeys ", 12 ) )
		return qfalse;
	key += 12;
	command = strchr( key, ' ' );
	if ( !command || command - key >= (int)sizeof( context ) )
		return qfalse;
	Q_strncpyz( context, key, command - key + 1 );
	return VRBind_KeysFor( context, command + 1, value, size );
}
