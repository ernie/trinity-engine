// cl_trinity.c -- Trinity tracker integration (login command)

#include "client.h"

#ifdef USE_CURL
#include "cl_curl.h"
#endif

#define JSON_IMPLEMENTATION
#include "../qcommon/json.h"

cvar_t *cl_trinityToken;
cvar_t *cl_trinityUser;
cvar_t *cl_trinityTracker;
cvar_t *cl_trinityLoginStatus;

static cvar_t *cl_trinityLoginUser;
static cvar_t *cl_trinityLoginPass;

#ifdef USE_CURL

// Buffer for curl response
typedef struct {
	char	data[4096];
	int		len;
} trinityResponse_t;

// Async login state
static struct {
	CURLM				*multi;
	CURL				*curl;
	struct curl_slist	*headers;
	trinityResponse_t	response;
	char				username[128];	// saved for fallback if JSON has no username
	char				postData[512];	// must persist for async POST
	qboolean			active;
} trinityLogin;

static size_t CL_TrinityWriteCallback( void *ptr, size_t size, size_t nmemb, void *userdata ) {
	trinityResponse_t *resp = (trinityResponse_t *)userdata;
	int bytes = (int)(size * nmemb);
	int space = (int)sizeof( resp->data ) - resp->len - 1;

	if ( bytes > space )
		bytes = space;

	if ( bytes > 0 ) {
		memcpy( resp->data + resp->len, ptr, bytes );
		resp->len += bytes;
		resp->data[resp->len] = '\0';
	}
	return size * nmemb;
}

/*
===============
CL_TrinityJsonEscape

Escape a string for use in a JSON string value.
Handles ", \, and control characters per RFC 8259.
===============
*/
static void CL_TrinityJsonEscape( const char *src, char *dest, int destSize ) {
	int i = 0, j = 0;

	while ( src[i] && j < destSize - 6 ) {
		unsigned char c = (unsigned char)src[i];
		if ( c == '"' ) { dest[j++] = '\\'; dest[j++] = '"'; }
		else if ( c == '\\' ) { dest[j++] = '\\'; dest[j++] = '\\'; }
		else if ( c == '\n' ) { dest[j++] = '\\'; dest[j++] = 'n'; }
		else if ( c == '\r' ) { dest[j++] = '\\'; dest[j++] = 'r'; }
		else if ( c == '\t' ) { dest[j++] = '\\'; dest[j++] = 't'; }
		else if ( c == '\b' ) { dest[j++] = '\\'; dest[j++] = 'b'; }
		else if ( c == '\f' ) { dest[j++] = '\\'; dest[j++] = 'f'; }
		else if ( c < 0x20 ) { i++; continue; }
		else { dest[j++] = c; }
		i++;
	}
	dest[j] = '\0';
}

/*
===============
CL_TrinityLoginCleanup

Clean up async login curl handles.
===============
*/
static void CL_TrinityLoginCleanup( void ) {
	if ( trinityLogin.curl ) {
		if ( trinityLogin.multi ) {
			qcurl_multi_remove_handle( trinityLogin.multi, trinityLogin.curl );
		}
		qcurl_easy_cleanup( trinityLogin.curl );
		trinityLogin.curl = NULL;
	}
	if ( trinityLogin.multi ) {
		qcurl_multi_cleanup( trinityLogin.multi );
		trinityLogin.multi = NULL;
	}
	if ( trinityLogin.headers ) {
		qcurl_slist_free_all( trinityLogin.headers );
		trinityLogin.headers = NULL;
	}
	memset( trinityLogin.postData, 0, sizeof( trinityLogin.postData ) );
	trinityLogin.active = qfalse;
}

/*
===============
CL_TrinityLogin_f

Initiate an async login request to the Trinity tracker.
Credentials are read from cl_trinityLoginUser/cl_trinityLoginPass cvars.
===============
*/
static void CL_TrinityLogin_f( void ) {
	char url[512];
	char password[128];
	char escapedUser[256];
	char escapedPass[256];

	if ( trinityLogin.active ) {
		Com_Printf( "Trinity login already in progress\n" );
		return;
	}

	// Read credentials from cvars (set by UI before invoking this command)
	Q_strncpyz( trinityLogin.username, cl_trinityLoginUser->string, sizeof( trinityLogin.username ) );
	Q_strncpyz( password, cl_trinityLoginPass->string, sizeof( password ) );

	// Clear password cvar immediately
	Cvar_Set( "cl_trinityLoginPass", "" );

	if ( !trinityLogin.username[0] || !password[0] ) {
		Com_Printf( "Usage: set cl_trinityLoginUser and cl_trinityLoginPass, then trinity_login\n" );
		return;
	}

	if ( !cl_trinityTracker->string[0] ) {
		Com_Printf( "cl_trinityTracker is not set\n" );
		return;
	}

	if ( !CL_cURL_Init() ) {
		Com_Printf( "Failed to initialize curl for Trinity login\n" );
		return;
	}

	// Build JSON POST body with escaped strings
	CL_TrinityJsonEscape( trinityLogin.username, escapedUser, sizeof( escapedUser ) );
	CL_TrinityJsonEscape( password, escapedPass, sizeof( escapedPass ) );
	Com_sprintf( trinityLogin.postData, sizeof( trinityLogin.postData ),
		"{\"username\":\"%s\",\"password\":\"%s\"}",
		escapedUser, escapedPass );

	// Clear password from stack
	memset( password, 0, sizeof( password ) );

	// Build URL
	Com_sprintf( url, sizeof( url ), "%s/api/auth/game-login",
		cl_trinityTracker->string );

	// Init response buffer
	memset( &trinityLogin.response, 0, sizeof( trinityLogin.response ) );

	trinityLogin.curl = qcurl_easy_init();
	if ( !trinityLogin.curl ) {
		Com_Printf( "Failed to create curl handle\n" );
		Cvar_Set( "cl_trinityLoginStatus", "failed" );
		return;
	}

	trinityLogin.headers = qcurl_slist_append( NULL, "Content-Type: application/json" );

	qcurl_easy_setopt( trinityLogin.curl, CURLOPT_URL, url );
	qcurl_easy_setopt( trinityLogin.curl, CURLOPT_POSTFIELDS, trinityLogin.postData );
	qcurl_easy_setopt( trinityLogin.curl, CURLOPT_HTTPHEADER, trinityLogin.headers );
	qcurl_easy_setopt( trinityLogin.curl, CURLOPT_WRITEFUNCTION, CL_TrinityWriteCallback );
	qcurl_easy_setopt( trinityLogin.curl, CURLOPT_WRITEDATA, &trinityLogin.response );
	qcurl_easy_setopt( trinityLogin.curl, CURLOPT_TIMEOUT, 10L );
	qcurl_easy_setopt( trinityLogin.curl, CURLOPT_FOLLOWLOCATION, 0L );

	trinityLogin.multi = qcurl_multi_init();
	if ( !trinityLogin.multi ) {
		Com_Printf( "Failed to create curl multi handle\n" );
		CL_TrinityLoginCleanup();
		Cvar_Set( "cl_trinityLoginStatus", "failed" );
		return;
	}

	qcurl_multi_add_handle( trinityLogin.multi, trinityLogin.curl );
	trinityLogin.active = qtrue;

	Cvar_Set( "cl_trinityLoginStatus", "pending" );
	Com_Printf( "Trinity login request sent...\n" );
}

/*
===============
CL_TrinityPerformLogin

Called once per frame to poll async login progress.
===============
*/
void CL_TrinityPerformLogin( void ) {
	int running;
	CURLMsg *msg;
	int msgsLeft;
	long httpCode;

	if ( !trinityLogin.active ) {
		return;
	}

	qcurl_multi_perform( trinityLogin.multi, &running );

	msg = qcurl_multi_info_read( trinityLogin.multi, &msgsLeft );
	if ( !msg || msg->msg != CURLMSG_DONE ) {
		return;	// still in progress
	}

	// Request completed
	if ( msg->data.result != CURLE_OK ) {
		Com_Printf( "Trinity login failed: %s\n", qcurl_easy_strerror( msg->data.result ) );
		Cvar_Set( "cl_trinityLoginStatus", "failed" );
		CL_TrinityLoginCleanup();
		return;
	}

	qcurl_easy_getinfo( trinityLogin.curl, CURLINFO_RESPONSE_CODE, &httpCode );

	if ( httpCode != 200 ) {
		Com_Printf( "Trinity login failed: HTTP %ld\n", httpCode );
		Cvar_Set( "cl_trinityLoginStatus", "failed" );
		CL_TrinityLoginCleanup();
		return;
	}

	// Parse JSON response: {"token": "...", "username": "..."}
	{
		trinityResponse_t *resp = &trinityLogin.response;
		const char *jsonEnd = resp->data + resp->len;
		const char *tokenVal, *userVal;
		char token[256];

		tokenVal = JSON_ObjectGetNamedValue( resp->data, jsonEnd, "token" );
		userVal = JSON_ObjectGetNamedValue( resp->data, jsonEnd, "username" );

		if ( !tokenVal || !JSON_ValueGetString( tokenVal, jsonEnd, token, sizeof( token ) ) ) {
			Com_Printf( "Trinity login failed: no token in response\n" );
			Cvar_Set( "cl_trinityLoginStatus", "failed" );
			CL_TrinityLoginCleanup();
			return;
		}

		if ( userVal ) {
			JSON_ValueGetString( userVal, jsonEnd, trinityLogin.username, sizeof( trinityLogin.username ) );
		}

		Cvar_Set( "cl_trinityToken", token );
		Cvar_Set( "cl_trinityUser", trinityLogin.username );
		Cvar_Set( "cl_trinityLoginStatus", "success" );

		Com_Printf( "Trinity login successful: %s\n", trinityLogin.username );
	}

	CL_TrinityLoginCleanup();
}

#endif // USE_CURL

/*
===============
CL_TrinityLogout_f

Clear Trinity auth state. Needed as an engine command because
cl_trinityToken is CVAR_PROTECTED and cannot be set from QVMs.
===============
*/
static void CL_TrinityLogout_f( void ) {
	Cvar_Set( "cl_trinityToken", "" );
	Cvar_Set( "cl_trinityUser", "" );
	Cvar_Set( "cl_trinityLoginStatus", "none" );
	Com_Printf( "Trinity: logged out\n" );
}

void CL_TrinityInit( void ) {
	cl_trinityToken = Cvar_Get( "cl_trinityToken", "", CVAR_ARCHIVE_ND | CVAR_PROTECTED );
	Cvar_SetDescription( cl_trinityToken, "Trinity authentication token." );

	cl_trinityUser = Cvar_Get( "cl_trinityUser", "", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( cl_trinityUser, "Trinity logged-in username." );

	cl_trinityTracker = Cvar_Get( "cl_trinityTracker", "https://trinity.ernie.io", CVAR_ARCHIVE_ND );
	Cvar_SetDescription( cl_trinityTracker, "Trinity Tracker server URL." );

	cl_trinityLoginStatus = Cvar_Get( "cl_trinityLoginStatus", "none", CVAR_ROM );
	Cvar_SetDescription( cl_trinityLoginStatus, "Trinity login status: none, pending, success, failed." );

	cl_trinityLoginUser = Cvar_Get( "cl_trinityLoginUser", "", CVAR_TEMP );
	cl_trinityLoginPass = Cvar_Get( "cl_trinityLoginPass", "", CVAR_TEMP );

#ifdef USE_CURL
	memset( &trinityLogin, 0, sizeof( trinityLogin ) );
	Cmd_AddCommand( "trinity_login", CL_TrinityLogin_f );
#endif
	Cmd_AddCommand( "trinity_logout", CL_TrinityLogout_f );
}

void CL_TrinityShutdown( void ) {
#ifdef USE_CURL
	CL_TrinityLoginCleanup();
	Cmd_RemoveCommand( "trinity_login" );
#endif
	Cmd_RemoveCommand( "trinity_logout" );
}
