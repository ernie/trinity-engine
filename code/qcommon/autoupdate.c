// autoupdate.c -- self-update via GitHub Releases

#ifdef USE_CURL

#include "../client/client.h"
#include "autoupdate.h"
#include "unzip.h"

#define JSON_IMPLEMENTATION
#include "json.h"

#include <stdio.h>
#include <ctype.h>

#ifdef __APPLE__
#include <sys/wait.h>
#include <unistd.h>
#include <copyfile.h>
#include <errno.h>
#endif

// compile-time defaults (overridden by Makefile defines)
#ifndef UPDATE_GITHUB_OWNER
#define UPDATE_GITHUB_OWNER "ernie"
#endif

#ifndef UPDATE_GITHUB_REPO
#define UPDATE_GITHUB_REPO "trinity-engine"
#endif

#ifndef UPDATE_ASSET_PREFIX
#define UPDATE_ASSET_PREFIX "trinity"
#endif

#define UPDATE_API_BUFSIZE		(256 * 1024)
#define UPDATE_EXTRACT_BUFSIZE	(64 * 1024)

#define UPDATES_DIR				".updates"
#define PENDING_DIR				UPDATES_DIR "/pending"
#define MANIFEST_NAME			"update_manifest.dat"

// base game paks to exclude from extraction
static const char *updateExcludeList[] = {
	"baseq3/pak0.pk3",
	"baseq3/pak1.pk3",
	"baseq3/pak2.pk3",
	"baseq3/pak3.pk3",
	"baseq3/pak4.pk3",
	"baseq3/pak5.pk3",
	"baseq3/pak6.pk3",
	"baseq3/pak7.pk3",
	"baseq3/pak8.pk3",
	"missionpack/pak0.pk3",
	"missionpack/pak1.pk3",
	"missionpack/pak2.pk3",
	"missionpack/pak3.pk3",
	NULL
};

// state
static updateState_t	updateState = UPDATE_IDLE;
static download_t		updateDownload;
static unsigned char	*apiResponseBuf;
static int				apiResponseLen;

// cvars (set by engine, read by UI)
static cvar_t	*update_available;
static cvar_t	*update_version;
static cvar_t	*update_current;
static cvar_t	*update_size;
static cvar_t	*update_state;
static cvar_t	*update_progress;
static cvar_t	*update_error;
static cvar_t	*update_check;
static cvar_t	*update_force;

// parsed release info
static char		releaseVersion[64];
static char		releaseAssetURL[MAX_OSPATH];
static int		releaseAssetSize;


// Pick the update-state root: the user-data dir on macOS (so update
// staging lives under ~/Library/Application Support/Trinity like
// configs and demos), the install directory via Sys_DefaultBasePath()
// on Windows/Linux (preserves the existing install-dir-is-state model).
// Uses Sys_DefaultHomePath() rather than the fs_homepath cvar to stay
// symmetric with Sys_ApplyPendingUpdate in unix_main.c, which also calls
// Sys_DefaultHomePath() directly (the cvar is empty before Com_Init).
static const char *Update_StageRoot( void )
{
#ifdef __APPLE__
	return Sys_DefaultHomePath();
#else
	return Sys_DefaultBasePath();
#endif
}


/*
==================
Update_ParseVersion

Parse "vX.Y.Z" or "X.Y.Z" into major/minor/patch.
Returns 0 on success, -1 on failure.
==================
*/
static int Update_ParseVersion( const char *str, int *major, int *minor, int *patch )
{
	if ( !str || !major || !minor || !patch )
		return -1;

	*major = *minor = *patch = 0;

	if ( *str == 'v' || *str == 'V' )
		str++;

	if ( !isdigit( (unsigned char)*str ) )
		return -1;

	while ( isdigit( (unsigned char)*str ) ) {
		*major = ( *major * 10 ) + ( *str - '0' );
		str++;
	}

	if ( *str == '.' ) {
		str++;
		while ( isdigit( (unsigned char)*str ) ) {
			*minor = ( *minor * 10 ) + ( *str - '0' );
			str++;
		}
	}

	if ( *str == '.' ) {
		str++;
		while ( isdigit( (unsigned char)*str ) ) {
			*patch = ( *patch * 10 ) + ( *str - '0' );
			str++;
		}
	}

	return 0;
}


/*
==================
Update_GetCurrentVersion

Extract version string from com_engine cvar ("trinity-engine/vX.Y.Z")
==================
*/
static const char *Update_GetCurrentVersion( void )
{
	const char *engine = Cvar_VariableString( "com_engine" );
	const char *slash = strrchr( engine, '/' );
	return slash ? slash + 1 : engine;
}


/*
==================
Update_IsExcluded

Check if a filename (relative, after stripping ZIP prefix) matches the exclude list.
==================
*/
static qboolean Update_IsExcluded( const char *filename )
{
	int i;
	for ( i = 0; updateExcludeList[i]; i++ ) {
		if ( Q_stricmp( filename, updateExcludeList[i] ) == 0 )
			return qtrue;
	}
	return qfalse;
}


/*
==================
Update_BuildAssetName

Build the expected asset filename for this platform/arch.
e.g., "trinity-windows-mingw-x86_64.zip"
==================
*/
static void Update_BuildAssetName( char *buf, int bufSize )
{
#if defined(_WIN32)
  #if defined(_MSC_VER)
    #if defined(_M_ARM64)
	Com_sprintf( buf, bufSize, "%s-windows-msvc-arm64.zip", UPDATE_ASSET_PREFIX );
    #else
	Com_sprintf( buf, bufSize, "%s-windows-msvc-x86_64.zip", UPDATE_ASSET_PREFIX );
    #endif
  #else
    #if defined(__x86_64__) || defined(_M_X64)
	Com_sprintf( buf, bufSize, "%s-windows-mingw-x86_64.zip", UPDATE_ASSET_PREFIX );
    #else
	Com_sprintf( buf, bufSize, "%s-windows-mingw-x86.zip", UPDATE_ASSET_PREFIX );
    #endif
  #endif
#elif defined(__APPLE__)
	Com_sprintf( buf, bufSize, "%s-macos-universal2.dmg", UPDATE_ASSET_PREFIX );
#elif defined(__linux__)
  #if defined(__aarch64__)
	Com_sprintf( buf, bufSize, "%s-linux-arm64.zip", UPDATE_ASSET_PREFIX );
  #elif defined(__arm__)
	Com_sprintf( buf, bufSize, "%s-linux-armv7.zip", UPDATE_ASSET_PREFIX );
  #elif defined(__x86_64__)
	Com_sprintf( buf, bufSize, "%s-linux-x86_64.zip", UPDATE_ASSET_PREFIX );
  #else
	Com_sprintf( buf, bufSize, "%s-linux-x86.zip", UPDATE_ASSET_PREFIX );
  #endif
#else
	buf[0] = '\0';
#endif
}


/*
==================
Update_SetState
==================
*/
static void Update_SetState( updateState_t state )
{
	updateState = state;
	Cvar_SetIntegerValue( "update_state", (int)state );
}


/*
==================
Update_SetError
==================
*/
static void Update_SetError( const char *msg )
{
	Com_Printf( S_COLOR_RED "Update: %s\n", msg );
	Cvar_Set( "update_error", msg );
	Update_SetState( UPDATE_ERROR );
}


/*
==================
Update_APIWriteCallback

curl write callback for in-memory API response.
==================
*/
static size_t Update_APIWriteCallback( void *ptr, size_t size, size_t nmemb, void *userdata )
{
	int bytes = (int)( size * nmemb );
	download_t *dl = (download_t *)userdata;

	(void)dl;

	if ( apiResponseLen + bytes >= UPDATE_API_BUFSIZE - 1 ) {
		return 0; // too large, abort
	}

	memcpy( apiResponseBuf + apiResponseLen, ptr, bytes );
	apiResponseLen += bytes;
	apiResponseBuf[apiResponseLen] = '\0';

	return bytes;
}


/*
==================
Update_ParseAPIResponse

Parse GitHub releases/latest JSON response.
Extract tag_name, and find the matching asset URL + size.
==================
*/
static qboolean Update_ParseAPIResponse( void )
{
	const char *json, *jsonEnd;
	const char *tagValue, *assetsValue, *assetEntry;
	char tagName[64];
	char assetName[MAX_OSPATH];
	char expectedAsset[MAX_OSPATH];
	char assetURL[MAX_OSPATH];
	int curMajor, curMinor, curPatch;
	int newMajor, newMinor, newPatch;
	const char *curVer;

	json = (const char *)apiResponseBuf;
	jsonEnd = json + apiResponseLen;

	// extract tag_name
	tagValue = JSON_ObjectGetNamedValue( json, jsonEnd, "tag_name" );
	if ( !tagValue ) {
		Update_SetError( "No tag_name in release response" );
		return qfalse;
	}

	if ( !JSON_ValueGetString( tagValue, jsonEnd, tagName, sizeof( tagName ) ) ) {
		Update_SetError( "Failed to read tag_name" );
		return qfalse;
	}

	Q_strncpyz( releaseVersion, tagName, sizeof( releaseVersion ) );
	Cvar_Set( "update_version", releaseVersion );

	// compare versions
	curVer = Update_GetCurrentVersion();
	Cvar_Set( "update_current", curVer );

	if ( Update_ParseVersion( curVer, &curMajor, &curMinor, &curPatch ) != 0 ) {
		Update_SetError( "Cannot parse current version" );
		return qfalse;
	}
	if ( Update_ParseVersion( tagName, &newMajor, &newMinor, &newPatch ) != 0 ) {
		Update_SetError( "Cannot parse release version" );
		return qfalse;
	}

	if ( !update_force->integer &&
		( newMajor < curMajor ||
		( newMajor == curMajor && newMinor < curMinor ) ||
		( newMajor == curMajor && newMinor == curMinor && newPatch <= curPatch ) ) ) {
		Com_Printf( "Update: already up to date (%s)\n", curVer );
		Cvar_SetIntegerValue( "update_available", 0 );
		Update_SetState( UPDATE_IDLE );
		return qfalse;
	}

	// find the right asset
	Update_BuildAssetName( expectedAsset, sizeof( expectedAsset ) );
	if ( !expectedAsset[0] ) {
		Update_SetError( "Unsupported platform for auto-update" );
		return qfalse;
	}

	assetsValue = JSON_ObjectGetNamedValue( json, jsonEnd, "assets" );
	if ( !assetsValue ) {
		Update_SetError( "No assets in release" );
		return qfalse;
	}

	releaseAssetURL[0] = '\0';
	releaseAssetSize = 0;

	for ( assetEntry = JSON_ArrayGetFirstValue( assetsValue, jsonEnd );
		  assetEntry;
		  assetEntry = JSON_ArrayGetNextValue( assetEntry, jsonEnd ) )
	{
		const char *nameVal = JSON_ObjectGetNamedValue( assetEntry, jsonEnd, "name" );
		const char *urlVal = JSON_ObjectGetNamedValue( assetEntry, jsonEnd, "browser_download_url" );
		const char *sizeVal = JSON_ObjectGetNamedValue( assetEntry, jsonEnd, "size" );

		if ( !nameVal || !urlVal )
			continue;

		JSON_ValueGetString( nameVal, jsonEnd, assetName, sizeof( assetName ) );

		if ( Q_stricmp( assetName, expectedAsset ) == 0 ) {
			JSON_ValueGetString( urlVal, jsonEnd, assetURL, sizeof( assetURL ) );
			Q_strncpyz( releaseAssetURL, assetURL, sizeof( releaseAssetURL ) );
			if ( sizeVal )
				releaseAssetSize = JSON_ValueGetInt( sizeVal, jsonEnd );
			break;
		}
	}

	if ( !releaseAssetURL[0] ) {
		Update_SetError( va( "Asset '%s' not found in release %s", expectedAsset, tagName ) );
		return qfalse;
	}

	if ( releaseAssetSize >= 1024 * 1024 )
		Com_Printf( "Update: %s available (current: %s, size: %i.%iMB)\n",
			releaseVersion, curVer,
			releaseAssetSize / (1024*1024),
			(releaseAssetSize / (1024*1024/10)) % 10 );
	else
		Com_Printf( "Update: %s available (current: %s, size: %iKB)\n",
			releaseVersion, curVer, releaseAssetSize / 1024 );

	Cvar_SetIntegerValue( "update_available", 1 );
	Cvar_SetIntegerValue( "update_size", releaseAssetSize );

	return qtrue;
}


/*
==================
Update_BeginCheck

Start async HTTP GET to GitHub releases API.
==================
*/
static void Update_BeginCheck( void )
{
	download_t *dl = &updateDownload;
	char apiURL[MAX_OSPATH];

	if ( updateState != UPDATE_IDLE && updateState != UPDATE_ERROR ) {
		Com_Printf( "Update: operation already in progress\n" );
		return;
	}

	Com_DL_Cleanup( dl );

	if ( !Com_DL_Init( dl ) ) {
		Update_SetError( "Failed to initialize cURL" );
		return;
	}

	dl->cURL = dl->func.easy_init();
	if ( !dl->cURL ) {
		Update_SetError( "cURL easy_init failed" );
		Com_DL_Cleanup( dl );
		return;
	}

	// allocate response buffer
	if ( !apiResponseBuf ) {
		apiResponseBuf = Z_Malloc( UPDATE_API_BUFSIZE );
	}
	apiResponseLen = 0;
	apiResponseBuf[0] = '\0';

	Com_sprintf( apiURL, sizeof( apiURL ),
		"https://api.github.com/repos/%s/%s/releases/latest",
		UPDATE_GITHUB_OWNER, UPDATE_GITHUB_REPO );

	Com_Printf( "Update: checking %s/%s...\n", UPDATE_GITHUB_OWNER, UPDATE_GITHUB_REPO );

	if ( com_developer->integer )
		dl->func.easy_setopt( dl->cURL, CURLOPT_VERBOSE, 1 );

	dl->func.easy_setopt( dl->cURL, CURLOPT_URL, apiURL );
	dl->func.easy_setopt( dl->cURL, CURLOPT_USERAGENT, va( "%s/%s", UPDATE_GITHUB_REPO, Update_GetCurrentVersion() ) );
	dl->func.easy_setopt( dl->cURL, CURLOPT_WRITEFUNCTION, Update_APIWriteCallback );
	dl->func.easy_setopt( dl->cURL, CURLOPT_WRITEDATA, dl );
	dl->func.easy_setopt( dl->cURL, CURLOPT_NOPROGRESS, 1 );
	dl->func.easy_setopt( dl->cURL, CURLOPT_FAILONERROR, 1 );
	dl->func.easy_setopt( dl->cURL, CURLOPT_FOLLOWLOCATION, 1 );
	dl->func.easy_setopt( dl->cURL, CURLOPT_MAXREDIRS, 5 );
	dl->func.easy_setopt( dl->cURL, CURLOPT_TIMEOUT, 30 );
#if CURL_AT_LEAST_VERSION(7, 85, 0)
	dl->func.easy_setopt( dl->cURL, CURLOPT_PROTOCOLS_STR, "https" );
#else
	dl->func.easy_setopt( dl->cURL, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS );
#endif

	dl->cURLM = dl->func.multi_init();
	if ( !dl->cURLM ) {
		Update_SetError( "cURL multi_init failed" );
		Com_DL_Cleanup( dl );
		return;
	}

	if ( dl->func.multi_add_handle( dl->cURLM, dl->cURL ) != CURLM_OK ) {
		Update_SetError( "cURL multi_add_handle failed" );
		Com_DL_Cleanup( dl );
		return;
	}

	Update_SetState( UPDATE_CHECKING );
}


/*
==================
Update_PerformCheck

Poll the API check download. Called from Update_Frame.
==================
*/
static void Update_PerformCheck( void )
{
	download_t *dl = &updateDownload;
	CURLMcode res;
	CURLMsg *msg;
	int c, i;

	res = dl->func.multi_perform( dl->cURLM, &c );

	i = 0;
	while ( res == CURLM_CALL_MULTI_PERFORM && i < 128 ) {
		res = dl->func.multi_perform( dl->cURLM, &c );
		i++;
	}
	if ( res == CURLM_CALL_MULTI_PERFORM )
		return;

	msg = dl->func.multi_info_read( dl->cURLM, &c );
	if ( msg == NULL )
		return;

	// done
	if ( msg->msg == CURLMSG_DONE && msg->data.result == CURLE_OK ) {
		Com_DL_Cleanup( dl );
		if ( Update_ParseAPIResponse() ) {
			Update_SetState( UPDATE_AVAILABLE );
		}
		// else: state was set by ParseAPIResponse (IDLE or ERROR)
	} else {
		long code = 0;
		dl->func.easy_getinfo( msg->easy_handle, CURLINFO_RESPONSE_CODE, &code );
		Com_DL_Cleanup( dl );
		if ( code == 403 ) {
			Update_SetError( "GitHub API rate limited. Try again later." );
		} else {
			Update_SetError( va( "Update check failed (HTTP %ld)", code ) );
		}
	}
}


/*
==================
Update_FileWriteCallback

curl write callback for ZIP file download: writes directly to a raw file handle.
==================
*/
static FILE *updateZipFile;

static size_t Update_FileWriteCallback( void *ptr, size_t size, size_t nmemb, void *userdata )
{
	(void)userdata;
	if ( !updateZipFile )
		return 0;
	return fwrite( ptr, size, nmemb, updateZipFile );
}


/*
==================
Update_DownloadProgressCallback

curl progress callback for ZIP download.
==================
*/
#if CURL_AT_LEAST_VERSION(7, 32, 0)
static int Update_DownloadProgressCallback( void *data, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow )
#else
static int Update_DownloadProgressCallback( void *data, double dltotal, double dlnow, double ultotal, double ulnow )
#endif
{
	(void)data;
	(void)ultotal;
	(void)ulnow;

	if ( dltotal > 0 ) {
		Cvar_SetIntegerValue( "update_progress", (int)( ( dlnow * 100 ) / dltotal ) );
	}
	Cvar_SetIntegerValue( "update_size", (int)dltotal );

	return 0;
}


/*
==================
Update_BeginDownload

Start downloading the release asset to a temp file in the staging root.
==================
*/
static char updateDownloadPath[MAX_OSPATH];

static void Update_BeginDownload( void )
{
	download_t *dl = &updateDownload;

	if ( !releaseAssetURL[0] ) {
		Update_SetError( "No asset URL" );
		return;
	}

	Com_DL_Cleanup( dl );

	if ( !Com_DL_Init( dl ) ) {
		Update_SetError( "Failed to initialize cURL" );
		return;
	}

	dl->cURL = dl->func.easy_init();
	if ( !dl->cURL ) {
		Update_SetError( "cURL easy_init failed" );
		Com_DL_Cleanup( dl );
		return;
	}

	// write to a temp file in the staging root (raw filesystem, not engine FS)
#ifdef __APPLE__
	Com_sprintf( updateDownloadPath, sizeof( updateDownloadPath ), "%s/update_download.dmg.tmp", Update_StageRoot() );
#else
	Com_sprintf( updateDownloadPath, sizeof( updateDownloadPath ), "%s/update_download.zip.tmp", Update_StageRoot() );
#endif

	updateZipFile = fopen( updateDownloadPath, "wb" );
	if ( !updateZipFile ) {
		Update_SetError( va( "Cannot write to update staging directory: %s", Update_StageRoot() ) );
		Com_DL_Cleanup( dl );
		return;
	}

	Com_Printf( "Update: downloading %s...\n", releaseVersion );

	Cvar_SetIntegerValue( "update_progress", 0 );

	if ( com_developer->integer )
		dl->func.easy_setopt( dl->cURL, CURLOPT_VERBOSE, 1 );

	dl->func.easy_setopt( dl->cURL, CURLOPT_URL, releaseAssetURL );
	dl->func.easy_setopt( dl->cURL, CURLOPT_USERAGENT, va( "%s/%s", UPDATE_GITHUB_REPO, Update_GetCurrentVersion() ) );
	dl->func.easy_setopt( dl->cURL, CURLOPT_WRITEFUNCTION, Update_FileWriteCallback );
	dl->func.easy_setopt( dl->cURL, CURLOPT_WRITEDATA, NULL );
	dl->func.easy_setopt( dl->cURL, CURLOPT_NOPROGRESS, 0 );
#if CURL_AT_LEAST_VERSION(7, 32, 0)
	dl->func.easy_setopt( dl->cURL, CURLOPT_XFERINFOFUNCTION, Update_DownloadProgressCallback );
	dl->func.easy_setopt( dl->cURL, CURLOPT_XFERINFODATA, NULL );
#else
	dl->func.easy_setopt( dl->cURL, CURLOPT_PROGRESSFUNCTION, Update_DownloadProgressCallback );
	dl->func.easy_setopt( dl->cURL, CURLOPT_PROGRESSDATA, NULL );
#endif
	dl->func.easy_setopt( dl->cURL, CURLOPT_FAILONERROR, 1 );
	dl->func.easy_setopt( dl->cURL, CURLOPT_FOLLOWLOCATION, 1 );
	dl->func.easy_setopt( dl->cURL, CURLOPT_MAXREDIRS, 10 );
#if CURL_AT_LEAST_VERSION(7, 85, 0)
	dl->func.easy_setopt( dl->cURL, CURLOPT_PROTOCOLS_STR, "https" );
#else
	dl->func.easy_setopt( dl->cURL, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS );
#endif

#ifdef CURL_MAX_READ_SIZE
	dl->func.easy_setopt( dl->cURL, CURLOPT_BUFFERSIZE, CURL_MAX_READ_SIZE );
#endif

	dl->cURLM = dl->func.multi_init();
	if ( !dl->cURLM ) {
		fclose( updateZipFile );
		updateZipFile = NULL;
		Update_SetError( "cURL multi_init failed" );
		Com_DL_Cleanup( dl );
		return;
	}

	if ( dl->func.multi_add_handle( dl->cURLM, dl->cURL ) != CURLM_OK ) {
		fclose( updateZipFile );
		updateZipFile = NULL;
		Update_SetError( "cURL multi_add_handle failed" );
		Com_DL_Cleanup( dl );
		return;
	}

	Update_SetState( UPDATE_DOWNLOADING );
}


/*
==================
Update_PerformDownload

Poll the ZIP download. Called from Update_Frame.
==================
*/
static void Update_PerformDownload( void )
{
	download_t *dl = &updateDownload;
	CURLMcode res;
	CURLMsg *msg;
	int c, i;

	res = dl->func.multi_perform( dl->cURLM, &c );

	i = 0;
	while ( res == CURLM_CALL_MULTI_PERFORM && i < 128 ) {
		res = dl->func.multi_perform( dl->cURLM, &c );
		i++;
	}
	if ( res == CURLM_CALL_MULTI_PERFORM )
		return;

	msg = dl->func.multi_info_read( dl->cURLM, &c );
	if ( msg == NULL )
		return;

	// close the file
	if ( updateZipFile ) {
		fclose( updateZipFile );
		updateZipFile = NULL;
	}

	if ( msg->msg == CURLMSG_DONE && msg->data.result == CURLE_OK ) {
		Com_DL_Cleanup( dl );
		Com_Printf( "Update: download complete, extracting...\n" );
		Update_SetState( UPDATE_EXTRACTING );
		// extraction happens in the next frame
	} else {
		long code = 0;
		dl->func.easy_getinfo( msg->easy_handle, CURLINFO_RESPONSE_CODE, &code );
		Com_DL_Cleanup( dl );
		remove( updateDownloadPath );
		Update_SetError( va( "Download failed (HTTP %ld)", code ) );
	}
}


/*
==================
Update_MakePath

Ensure all directories in a path exist (raw filesystem).
==================
*/
static void Update_MakePath( const char *path )
{
	char dir[MAX_OSPATH];
	char *p;

	Q_strncpyz( dir, path, sizeof( dir ) );

	for ( p = dir + 1; *p; p++ ) {
		if ( *p == '/' || *p == '\\' ) {
			*p = '\0';
			Sys_Mkdir( dir );
			*p = '/';
		}
	}
}


/*
==================
Update_DetectZipPrefix

Check if all ZIP entries share a common top-level directory prefix.
Returns the prefix length (including trailing /) or 0 if no common prefix.
==================
*/
static int Update_DetectZipPrefix( unzFile uf )
{
	char filename[MAX_OSPATH];
	char prefix[MAX_OSPATH];
	int prefixLen = 0;
	int ret;

	ret = unzGoToFirstFile( uf );
	if ( ret != UNZ_OK )
		return 0;

	// get first entry to establish candidate prefix
	unzGetCurrentFileInfo( uf, NULL, filename, sizeof( filename ), NULL, 0, NULL, 0 );

	{
		char *slash = strchr( filename, '/' );
		if ( !slash )
			return 0; // first entry has no directory, no common prefix

		prefixLen = (int)( slash - filename + 1 ); // include the '/'
		Q_strncpyz( prefix, filename, prefixLen + 1 );
	}

	// verify all entries share this prefix
	while ( unzGoToNextFile( uf ) == UNZ_OK ) {
		unzGetCurrentFileInfo( uf, NULL, filename, sizeof( filename ), NULL, 0, NULL, 0 );
		if ( Q_stricmpn( filename, prefix, prefixLen ) != 0 )
			return 0;
	}

	// reset to beginning
	unzGoToFirstFile( uf );

	return prefixLen;
}


#ifdef __APPLE__
/*
==================
Update_RunCommand

fork+execvp+wait helper for shelling out to hdiutil and ditto. argv is
NULL-terminated; argv[0] is the program name (looked up on PATH).
Returns qtrue iff the child exited with status 0.
==================
*/
static qboolean Update_RunCommand( const char *argv[] )
{
	pid_t pid;
	int status;

	pid = fork();
	if ( pid < 0 ) {
		return qfalse;
	}
	if ( pid == 0 ) {
		execvp( argv[0], (char *const *)argv );
		_exit( 127 );
	}

	while ( waitpid( pid, &status, 0 ) < 0 ) {
		if ( errno != EINTR ) {
			return qfalse;
		}
	}
	return WIFEXITED( status ) && WEXITSTATUS( status ) == 0;
}


/*
==================
Update_ExtractDMG

Mount the downloaded .dmg, copy Trinity.app out into pending/Trinity.app
via copyfile(3) with COPYFILE_ALL (data + xattrs + ACLs + stat, so the
notarization staple xattr comes through), detach, then write a minimal
manifest. The apply path picks up pending/Trinity.app and does the
atomic bundle swap.
==================
*/
static qboolean Update_ExtractDMG( void )
{
	char updatesDir[MAX_OSPATH];
	char pendingDir[MAX_OSPATH];
	char manifestPath[MAX_OSPATH];
	char stagedBundle[MAX_OSPATH];
	char mountPoint[MAX_OSPATH];
	char bundleSource[MAX_OSPATH];
	FILE *manifest;
	int copyResult;

	Com_sprintf( updatesDir, sizeof( updatesDir ), "%s/%s", Update_StageRoot(), UPDATES_DIR );
	Com_sprintf( pendingDir, sizeof( pendingDir ), "%s/%s", Update_StageRoot(), PENDING_DIR );
	Com_sprintf( manifestPath, sizeof( manifestPath ), "%s/%s", pendingDir, MANIFEST_NAME );
	Com_sprintf( stagedBundle, sizeof( stagedBundle ), "%s/Trinity.app", pendingDir );

	Sys_Mkdir( updatesDir );

	// Clear any partial pending/ from a prior failed extract: copyfile
	// with COPYFILE_RECURSIVE merges into an existing destination dir
	// rather than replacing it.
	{
		const char *rmArgs[] = { "rm", "-rf", pendingDir, NULL };
		Update_RunCommand( rmArgs );
	}
	Sys_Mkdir( pendingDir );

	Com_sprintf( mountPoint, sizeof( mountPoint ), "%s/dmg-mount-XXXXXX", updatesDir );
	if ( !mkdtemp( mountPoint ) ) {
		Update_SetError( "Failed to create temp mount point for DMG" );
		remove( updateDownloadPath );
		return qfalse;
	}

	{
		const char *attachArgs[] = {
			"hdiutil", "attach", "-nobrowse", "-noverify", "-noautoopen",
			"-mountpoint", mountPoint, updateDownloadPath, NULL
		};
		if ( !Update_RunCommand( attachArgs ) ) {
			Update_SetError( "hdiutil attach failed for downloaded DMG" );
			rmdir( mountPoint );
			remove( updateDownloadPath );
			return qfalse;
		}
	}

	Com_sprintf( bundleSource, sizeof( bundleSource ), "%s/Trinity.app", mountPoint );
	copyResult = copyfile( bundleSource, stagedBundle, NULL,
		COPYFILE_ALL | COPYFILE_RECURSIVE );

	{
		const char *detachArgs[] = { "hdiutil", "detach", mountPoint, NULL };
		Update_RunCommand( detachArgs );
	}
	rmdir( mountPoint );

	if ( copyResult != 0 ) {
		Update_SetError( va( "copyfile failed to extract Trinity.app from DMG: %s",
			strerror( errno ) ) );
		remove( updateDownloadPath );
		return qfalse;
	}

	manifest = fopen( manifestPath, "w" );
	if ( !manifest ) {
		Update_SetError( "Failed to create update manifest" );
		remove( updateDownloadPath );
		return qfalse;
	}
	fprintf( manifest, "version=%s\n", releaseVersion );
	fclose( manifest );

	remove( updateDownloadPath );

	Com_Printf( "Update: extracted Trinity.app from DMG (version %s)\n", releaseVersion );
	return qtrue;
}
#endif


/*
==================
Update_ExtractAndStage

Stage the downloaded release into pendingupdate/ and write the manifest.
On macOS the asset is a .dmg handled by Update_ExtractDMG; everywhere
else it's a .zip unpacked entry-by-entry.
==================
*/
static qboolean Update_ExtractAndStage( void )
{
#ifdef __APPLE__
	return Update_ExtractDMG();
#else
	unzFile uf;
	char filename[MAX_OSPATH];
	char destPath[MAX_OSPATH];
	char pendingDir[MAX_OSPATH];
	char manifestPath[MAX_OSPATH];
	unz_file_info fileInfo;
	unsigned char *extractBuf;
	FILE *outFile;
	FILE *manifest;
	int prefixLen;
	int extracted = 0;
	int ret;

	Com_sprintf( pendingDir, sizeof( pendingDir ), "%s/%s", Update_StageRoot(), PENDING_DIR );
	Com_sprintf( manifestPath, sizeof( manifestPath ), "%s/%s", pendingDir, MANIFEST_NAME );

	uf = unzOpen( updateDownloadPath );
	if ( !uf ) {
		Update_SetError( "Failed to open downloaded ZIP" );
		remove( updateDownloadPath );
		return qfalse;
	}

	prefixLen = Update_DetectZipPrefix( uf );

	// create .updates/pending/ directory
	{
		char updatesDir[MAX_OSPATH];
		Com_sprintf( updatesDir, sizeof( updatesDir ), "%s/%s", Update_StageRoot(), UPDATES_DIR );
		Sys_Mkdir( updatesDir );
	}
	Sys_Mkdir( pendingDir );

	// open manifest
	manifest = fopen( manifestPath, "w" );
	if ( !manifest ) {
		Update_SetError( "Failed to create update manifest" );
		unzClose( uf );
		remove( updateDownloadPath );
		return qfalse;
	}

	fprintf( manifest, "version=%s\n", releaseVersion );

	extractBuf = Z_Malloc( UPDATE_EXTRACT_BUFSIZE );

	ret = unzGoToFirstFile( uf );
	while ( ret == UNZ_OK ) {
		const char *relName;
		int bytesRead;
		int len;

		unzGetCurrentFileInfo( uf, &fileInfo, filename, sizeof( filename ), NULL, 0, NULL, 0 );

		// strip common prefix if detected
		relName = filename + prefixLen;

		// skip directories (entries ending with /)
		len = (int)strlen( relName );
		if ( len == 0 || relName[len - 1] == '/' ) {
			ret = unzGoToNextFile( uf );
			continue;
		}

		// skip excluded files
		if ( Update_IsExcluded( relName ) ) {
			ret = unzGoToNextFile( uf );
			continue;
		}

		// build destination path
		Com_sprintf( destPath, sizeof( destPath ), "%s/%s", pendingDir, relName );

		// ensure parent directories exist
		Update_MakePath( destPath );

		// extract
		if ( unzOpenCurrentFile( uf ) != UNZ_OK ) {
			Com_Printf( S_COLOR_YELLOW "Update: failed to open ZIP entry '%s'\n", relName );
			ret = unzGoToNextFile( uf );
			continue;
		}

		outFile = fopen( destPath, "wb" );
		if ( !outFile ) {
			Com_Printf( S_COLOR_YELLOW "Update: failed to create '%s'\n", destPath );
			unzCloseCurrentFile( uf );
			ret = unzGoToNextFile( uf );
			continue;
		}

		{
			qboolean writeError = qfalse;
			while ( ( bytesRead = unzReadCurrentFile( uf, extractBuf, UPDATE_EXTRACT_BUFSIZE ) ) > 0 ) {
				if ( fwrite( extractBuf, 1, bytesRead, outFile ) != (size_t)bytesRead ) {
					writeError = qtrue;
					break;
				}
			}

			fclose( outFile );
			unzCloseCurrentFile( uf );

			if ( writeError ) {
				Com_Printf( S_COLOR_RED "Update: write error extracting '%s' (disk full?)\n", relName );
				remove( destPath );
				Z_Free( extractBuf );
				fclose( manifest );
				unzClose( uf );
				remove( updateDownloadPath );
				Update_SetError( "Write error during extraction (disk full?)" );
				return qfalse;
			}
		}

		// write manifest entry: relative_path|CRC32
		fprintf( manifest, "%s|%08lX\n", relName, fileInfo.crc );
		extracted++;

		Com_DPrintf( "Update: extracted %s (%lu bytes)\n", relName, fileInfo.uncompressed_size );

		ret = unzGoToNextFile( uf );
	}

	Z_Free( extractBuf );
	fclose( manifest );
	unzClose( uf );

	// clean up the downloaded ZIP
	remove( updateDownloadPath );

	if ( extracted == 0 ) {
		Update_SetError( "No files extracted from update ZIP" );
		remove( manifestPath );
		return qfalse;
	}

	Com_Printf( "Update: %d files staged in %s\n", extracted, PENDING_DIR );
	return qtrue;
#endif
}


/*
==================
Update_Init
==================
*/
void Update_Init( void )
{
	update_available = Cvar_Get( "update_available", "0", CVAR_ROM );
	update_version = Cvar_Get( "update_version", "", CVAR_ROM );
	update_current = Cvar_Get( "update_current", "", CVAR_ROM );
	update_size = Cvar_Get( "update_size", "0", CVAR_ROM );
	update_state = Cvar_Get( "update_state", "0", CVAR_ROM );
	update_progress = Cvar_Get( "update_progress", "0", CVAR_ROM );
	update_error = Cvar_Get( "update_error", "", CVAR_ROM );
	update_check = Cvar_Get( "update_check", "1", CVAR_ARCHIVE );
	Cvar_SetDescription( update_check, "Check for engine updates on startup." );
	update_force = Cvar_Get( "update_force", "0", CVAR_TEMP );
	Cvar_SetDescription( update_force, "Force update even if current version is equal or newer." );

	Cmd_AddCommand( "update", Update_Check_f );
	Cmd_AddCommand( "updatedownload", Update_Download_f );
	Cmd_AddCommand( "updatecancel", Update_Cancel_f );
	Cmd_AddCommand( "updaterestart", Update_Restart_f );

	// auto-check on startup if enabled
	if ( update_check->integer ) {
		Update_BeginCheck();
	}
}


/*
==================
Update_Frame

Called once per client frame to poll async operations.
==================
*/
void Update_Frame( void )
{
	switch ( updateState ) {
	case UPDATE_CHECKING:
		Update_PerformCheck();
		break;
	case UPDATE_DOWNLOADING:
		Update_PerformDownload();
		break;
	case UPDATE_EXTRACTING:
		if ( Update_ExtractAndStage() ) {
			Com_Printf( "Update: %s staged. Restart to apply.\n", releaseVersion );
			Update_SetState( UPDATE_STAGED );
		}
		// else: error already set
		break;
	default:
		break;
	}
}


/*
==================
Update_Shutdown
==================
*/
void Update_Shutdown( void )
{
	if ( updateZipFile ) {
		fclose( updateZipFile );
		updateZipFile = NULL;
	}

	Com_DL_Cleanup( &updateDownload );

	if ( apiResponseBuf ) {
		Z_Free( apiResponseBuf );
		apiResponseBuf = NULL;
	}

	Cmd_RemoveCommand( "update" );
	Cmd_RemoveCommand( "updatedownload" );
	Cmd_RemoveCommand( "updatecancel" );
	Cmd_RemoveCommand( "updaterestart" );
}


/*
==================
Update_Check_f

Console command: \update
==================
*/
void Update_Check_f( void )
{
	Update_BeginCheck();
}


/*
==================
Update_Download_f

Console command: \updatedownload
==================
*/
void Update_Download_f( void )
{
	if ( updateState != UPDATE_AVAILABLE ) {
		Com_Printf( "Update: no update available to download\n" );
		return;
	}

	Update_BeginDownload();
}


/*
==================
Update_Cancel_f

Console command: \updatecancel
==================
*/
void Update_Cancel_f( void )
{
	if ( updateState == UPDATE_DOWNLOADING ) {
		if ( updateZipFile ) {
			fclose( updateZipFile );
			updateZipFile = NULL;
		}
		Com_DL_Cleanup( &updateDownload );
		remove( updateDownloadPath );
		Com_Printf( "Update: download cancelled\n" );
	} else if ( updateState == UPDATE_CHECKING ) {
		Com_DL_Cleanup( &updateDownload );
		Com_Printf( "Update: check cancelled\n" );
	}

	Update_SetState( UPDATE_IDLE );
}


/*
==================
Update_Restart_f

Console command: \updaterestart
Does a clean shutdown then relaunches the process.
==================
*/
void Update_Restart_f( void )
{
	Com_Printf( "Update: restarting...\n" );

	VM_Forced_Unload_Start();
	SV_Shutdown( "Restarting for update" );
	CL_Shutdown( "Restarting for update", qtrue );
	VM_Forced_Unload_Done();
	FS_Shutdown( qtrue );

	Sys_RestartProcess();
}


#endif // USE_CURL
