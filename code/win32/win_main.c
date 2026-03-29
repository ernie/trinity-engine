/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.

Quake III Arena source code is free software; you can redistribute it
and/or modify it under the terms of the GNU General Public License as
published by the Free Software Foundation; either version 2 of the License,
or (at your option) any later version.

Quake III Arena source code is distributed in the hope that it will be
useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with Quake III Arena source code; if not, write to the Free Software
Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
===========================================================================
*/
// win_main.c

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#ifndef DEDICATED
#include "../client/client.h"
#endif
#include "win_local.h"
#include "resource.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <direct.h>
#include <io.h>


#define MEM_THRESHOLD (96*1024*1024)

WinVars_t	g_wv;

#ifndef DEDICATED

/*
==================
Sys_LowPhysicalMemory
==================
*/
qboolean Sys_LowPhysicalMemory( void ) {
#if	_MSC_VER < 1600 // MSVC 2008 and lower, assume win9x compatibility builds
	MEMORYSTATUS stat;
	GlobalMemoryStatus( &stat );
	return (stat.dwTotalPhys <= MEM_THRESHOLD) ? qtrue : qfalse;
#else
	MEMORYSTATUSEX stat;
	stat.dwLength = sizeof(stat);

	if ( !GlobalMemoryStatusEx( &stat ) ) {
		return qfalse;
	}

	return (stat.ullAvailPhys <= MEM_THRESHOLD) ? qtrue : qfalse;
#endif
}


/*
==================
Sys_BeginProfiling
==================
*/
void Sys_BeginProfiling( void ) {
	// this is just used on the mac build
}

#endif // !DEDICATED

/*
=============
Sys_Error

Show the early console as an error dialog
=============
*/
void NORETURN FORMAT_PRINTF(1, 2) QDECL Sys_Error( const char *error, ... ) {
	va_list	argptr;
	char	text[4096];
	MSG		msg;

	va_start( argptr, error );
	Q_vsnprintf( text, sizeof( text ), error, argptr );
	va_end( argptr );

#ifndef DEDICATED
	CL_Shutdown( text, qtrue );
#endif

	Conbuf_AppendText( text );
	Conbuf_AppendText( "\n" );

	Sys_SetErrorText( text );
	Sys_ShowConsole( 1, qtrue );

	timeEndPeriod( 1 );

	// wait for the user to quit
	while ( 1 ) {
		if ( GetMessage( &msg, NULL, 0, 0 ) <= 0 ) {
			Cmd_Clear();
			Com_Quit_f();
		}
		TranslateMessage( &msg );
		DispatchMessage( &msg );
	}

	SetUnhandledExceptionFilter( NULL );

	Sys_DestroyConsole();

	exit( 1 );
}


/*
==============
Sys_Quit
==============
*/
void NORETURN Sys_Quit( void ) {

	timeEndPeriod( 1 );

	SetUnhandledExceptionFilter( NULL );

	Sys_DestroyConsole();

	exit( 0 );
}


/*
==============
Sys_Print
==============
*/
void Sys_Print( const char *msg )
{
	Conbuf_AppendText( msg );
}


/*
=============
Sys_Sleep
=============
*/
void Sys_Sleep( int msec ) {

	if ( msec < 0 ) {
		// special case: wait for event or network packet
		DWORD dwResult;
		msec = 300;
		do {
			dwResult = MsgWaitForMultipleObjects( 0, NULL, FALSE, msec, QS_ALLEVENTS );
		}
		while ( dwResult == WAIT_TIMEOUT && NET_Sleep( 10 * 1000 ) );
		//WaitMessage();
		return;
	}

	// busy wait there because Sleep(0) will relinquish CPU - which is not what we want
	//if ( msec == 0 )
	//	return;

	Sleep( msec );
}


/*
==============
Sys_Mkdir
==============
*/
qboolean Sys_Mkdir( const char *path )
{
	if ( _mkdir( path ) == 0 ) {
		return qtrue;
	} else {
		if ( errno == EEXIST ) {
			return qtrue;
		} else {
			return qfalse;
		}
	}
}


/*
==============
Sys_FOpen
==============
*/
FILE *Sys_FOpen( const char *ospath, const char *mode )
{
	size_t length;

	// Windows API ignores all trailing spaces and periods which can get around Quake 3 file system restrictions.
	length = strlen( ospath );
	if ( length == 0 || ospath[length-1] == ' ' || ospath[length-1] == '.' ) {
		return NULL;
	}

	return fopen( ospath, mode );
}


/*
==============
Sys_ResetReadOnlyAttribute
==============
*/
qboolean Sys_ResetReadOnlyAttribute( const char *ospath ) {
	DWORD dwAttr;

	dwAttr = GetFileAttributesA( ospath );
	if ( dwAttr & FILE_ATTRIBUTE_READONLY ) {
		dwAttr &= ~FILE_ATTRIBUTE_READONLY;
		if ( SetFileAttributesA( ospath, dwAttr ) ) {
			return qtrue;
		} else {
			return qfalse;
		}
	} else {
		return qfalse;
	}
}


/*
==============
Sys_Pwd
==============
*/
const char *Sys_Pwd( void )
{
	static char pwd[ MAX_OSPATH ];
	TCHAR	buffer[ MAX_OSPATH ];
	char *s;

	if ( pwd[0] )
		return pwd;

	GetModuleFileName( NULL, buffer, ARRAY_LEN( buffer ) );
	buffer[ ARRAY_LEN( buffer ) - 1 ] = '\0';

	Q_strncpyz( pwd, WtoA( buffer ), sizeof( pwd ) );

	s = strrchr( pwd, PATH_SEP );
	if ( s ) 
		*s = '\0';
	else // bogus case?
	{
		_getcwd( pwd, sizeof( pwd ) - 1 );
		pwd[ sizeof( pwd ) - 1 ] = '\0';
	}

	return pwd;
}


/*
==============
Sys_DefaultBasePath
==============
*/
const char *Sys_DefaultBasePath( void )
{
	return Sys_Pwd();
}


/*
==============================================================

DIRECTORY SCANNING

==============================================================
*/


/*
=============
Sys_ListExtFiles
=============
*/
static int Sys_ListExtFiles( const char *directory, const char *subdir, const char *extension, const char *filter, char **list, int maxfiles, int subdirs ) {
	char		search[MAX_OSPATH*2+MAX_QPATH+1];
	char		filename[MAX_OSPATH * 2];
	int			nfiles;
	struct _finddata_t findinfo;
	intptr_t	findhandle;
	int			flag;
	int			extLen;
	const char	*x;
	qboolean	hasPatterns;

	// passing a slash as extension will find directories
	if ( extension[0] == '/' && extension[1] == '\0' ) {
		extension = "";
		flag = 0;
	} else {
		flag = _A_SUBDIR;
	}

	extLen = (int)strlen( extension );
	hasPatterns = Com_HasPatterns( extension ); // contains either '?' or '*'
	if ( hasPatterns && extension[0] == '.' && extension[1] != '\0' ) {
		extension++;
	}

	nfiles = 0;

	if ( *subdir != '\0' ) {
		Com_sprintf( search, sizeof( search ), "%s\\%s\\*", directory, subdir );
	} else {
		Com_sprintf( search, sizeof( search ), "%s\\*", directory );
	}

	if ( subdirs > 0 ) {
		// handle recursion
		findhandle = _findfirst( search, &findinfo );
		if ( findhandle != -1 ) {
			do {
				if ( findinfo.attrib & _A_SUBDIR ) {
					if ( !Q_streq( findinfo.name, "." ) && !Q_streq( findinfo.name, ".." ) ) {
						char subdir2[MAX_OSPATH * 2 + MAX_QPATH + 1];
						if ( *subdir != '\0' ) {
							Com_sprintf( subdir2, sizeof( subdir2 ), "%s\\%s", subdir, findinfo.name );
						} else {
							Q_strncpyz( subdir2, findinfo.name, sizeof( subdir2 ) );
						}
						if ( nfiles >= maxfiles ) {
							break;
						}
						nfiles += Sys_ListExtFiles( directory, subdir2, extension, filter, list + nfiles, maxfiles - nfiles, subdirs - 1);
					}
				}
			} while ( _findnext( findhandle, &findinfo ) == 0 );
		}
		_findclose( findhandle );
	}

	Q_strcat( search, sizeof( search ), extension );

	findhandle = _findfirst( search, &findinfo );
	if ( findhandle == -1 ) {
		return nfiles;
	}

	do {
		if ( flag ^ ( findinfo.attrib & _A_SUBDIR ) ) {
			if ( *subdir != '\0' ) {
				Com_sprintf( filename, sizeof( filename ), "%s\\%s", subdir, findinfo.name );
			} else {
				Q_strncpyz( filename, findinfo.name, sizeof( filename ) );
			}
			if ( filter != NULL && *filter != '\0' ) {
				if ( !Com_FilterPath( filter, filename ) ) {
					continue;
				}
			} else if ( *extension != '\0' ) {
				if ( hasPatterns ) {
					x = strrchr( findinfo.name, '.' );
					if ( x == NULL || !Com_FilterExt( extension, x + 1 ) ) {
						continue;
					}
				} else {
					// check for exact extension
					const int length = strlen( findinfo.name );
					if ( length < extLen || Q_stricmp( findinfo.name + length - extLen, extension ) ) {
						continue;
					}
				}
			}
			if ( nfiles >= maxfiles ) {
				break;
			}
			list[ nfiles++ ] = FS_CopyString( filename );
		}
	} while ( _findnext( findhandle, &findinfo ) == 0 );

	_findclose( findhandle );

	return nfiles;

}

char** Sys_ListFiles( const char *directory, const char *extension, const char *filter, int *numfiles, int subdirs )
{
	char** listCopy;
	char* list[MAX_FOUND_FILES];
	int		i, nfiles;

	if ( extension == NULL ) {
		extension = "";
	}

	nfiles = Sys_ListExtFiles( directory, "", extension, filter, list, ARRAY_LEN( list ), subdirs );

	// copy list from stack, reserve extra space for NULL
	listCopy = Z_Malloc( (nfiles + 1) * sizeof( listCopy[0] ) );
	for ( i = 0; i < nfiles; i++ ) {
		listCopy[i] = list[i];
	}
	listCopy[i] = NULL;

	if ( nfiles > 1 ) {
		Com_SortList( listCopy, nfiles - 1 );
		if ( nfiles > 2 ) {
			if ( Q_streq( listCopy[0], "." ) && Q_streq( listCopy[1], ".." ) ) {
				// emulate old strgtr() function sort behavior for special entries
				char* dot1 = listCopy[0];
				char* dot2 = listCopy[1];
				for ( i = 0; i < nfiles - 2; i++ ) {
					listCopy[i] = listCopy[i + 2];
				}
				listCopy[nfiles - 2] = dot1;
				listCopy[nfiles - 1] = dot2;
			}
		}
	}

	*numfiles = nfiles;
	return listCopy;
}


/*
=============
Sys_FreeFileList
=============
*/
void Sys_FreeFileList( char **list ) {
	int		i;

	if ( !list ) {
		return;
	}

	for ( i = 0 ; list[i] ; i++ ) {
		Z_Free( list[i] );
	}

	Z_Free( list );
}


/*
=============
Sys_GetFileStats
=============
*/
qboolean Sys_GetFileStats( const char *filename, fileOffset_t *size, fileTime_t *mtime, fileTime_t *ctime ) {
	struct _stat s;

	if ( _stat( filename, &s ) == 0 ) {
		*size = (fileOffset_t)s.st_size;
		*mtime = (fileTime_t)s.st_mtime;
		*ctime = (fileTime_t)s.st_ctime;
		return qtrue;
	} else {
		*size = 0;
		*mtime = *ctime = 0;
		return qfalse;
	}
}


//========================================================

/*
========================================================================

LOAD/UNLOAD DLL

========================================================================
*/

static int dll_err_count = 0;

/*
=================
Sys_LoadLibrary
=================
*/
void *Sys_LoadLibrary( const char *name )
{
	const char *ext;

	if ( !name || !*name )
		return NULL;

	if ( FS_AllowedExtension( name, qfalse, &ext ) )
	{
		Com_Error( ERR_FATAL, "Sys_LoadLibrary: Unable to load library with '%s' extension", ext );
	}

	return (void *)LoadLibrary( AtoW( name ) );
}


/*
=================
Sys_LoadFunction
=================
*/
void *Sys_LoadFunction( void *handle, const char *name )
{
	void *symbol;

	if ( handle == NULL || name == NULL || *name == '\0' ) 
	{
		dll_err_count++;
		return NULL;
	}

	symbol = GetProcAddress( handle, name );
	if ( !symbol )
		dll_err_count++;

	return symbol;
}


/*
=================
Sys_LoadFunctionErrors
=================
*/
int Sys_LoadFunctionErrors( void )
{
	int result = dll_err_count;
	dll_err_count = 0;
	return result;
}


/*
=================
Sys_UnloadLibrary
=================
*/
void Sys_UnloadLibrary( void *handle )
{
	if ( handle )
		FreeLibrary( handle );
}


/*
=================
Sys_SendKeyEvents

Platform-dependent event handling
=================
*/
void Sys_SendKeyEvents( void )
{
#ifndef DEDICATED
	if ( !com_dedicated->integer )
		HandleEvents();
	else
#endif
	HandleConsoleEvents();
}


//================================================================


/*
==================
SetTimerResolution

Try to set lower timer period
==================
*/
static void SetTimerResolution( void )
{
	typedef HRESULT (WINAPI *pfnNtQueryTimerResolution)( PULONG MinRes, PULONG MaxRes, PULONG CurRes );
	typedef HRESULT (WINAPI *pfnNtSetTimerResolution)( ULONG NewRes, BOOLEAN SetRes, PULONG CurRes );
	pfnNtQueryTimerResolution pNtQueryTimerResolution;
	pfnNtSetTimerResolution pNtSetTimerResolution;
	ULONG curr, minr, maxr;
	HMODULE dll;

	dll = LoadLibrary( T( "ntdll" ) );
	if ( dll )
	{
		pNtQueryTimerResolution = (pfnNtQueryTimerResolution) GetProcAddress( dll, "NtQueryTimerResolution" );
		pNtSetTimerResolution = (pfnNtSetTimerResolution) GetProcAddress( dll, "NtSetTimerResolution" );
		if ( pNtQueryTimerResolution && pNtSetTimerResolution )
		{
			pNtQueryTimerResolution( &minr, &maxr, &curr );
			if ( maxr < 5000 ) // well, we don't need less than 0.5ms periods for select()
				maxr = 5000;
			pNtSetTimerResolution( maxr, TRUE, &curr );
		}
		FreeLibrary( dll );
	}
}


/*
================
Sys_Init

Called after the common systems (cvars, files, etc)
are initialized
================
*/
void Sys_Init( void ) {

	// make sure the timer is high precision, otherwise
	// NT gets 18ms resolution
	timeBeginPeriod( 1 );

	SetTimerResolution();

	Cvar_Set( "arch", "winnt" );
}

//=======================================================================


/*
==================
SetDPIAwareness
==================
*/
#if 0
static void SetDPIAwareness( void ) 
{
	typedef HANDLE (WINAPI *pfnSetThreadDpiAwarenessContext)( HANDLE dpiContext );
	typedef HRESULT (WINAPI *pfnSetProcessDpiAwareness)( int value );

	pfnSetThreadDpiAwarenessContext pSetThreadDpiAwarenessContext;
	pfnSetProcessDpiAwareness pSetProcessDpiAwareness;
	HMODULE dll;

	dll = GetModuleHandle( T("user32") );
	if ( dll )
	{
		pSetThreadDpiAwarenessContext = (pfnSetThreadDpiAwarenessContext) GetProcAddress( dll, "SetThreadDpiAwarenessContext" );
		if ( pSetThreadDpiAwarenessContext )
		{
			pSetThreadDpiAwarenessContext( (HANDLE)(intptr_t)-2 ); // DPI_AWARENESS_CONTEXT_SYSTEM_AWARE
		}

	}

	dll = LoadLibrary( T("shcore") );
	if ( dll )
	{
		pSetProcessDpiAwareness = (pfnSetProcessDpiAwareness) GetProcAddress( dll, "SetProcessDpiAwareness" );
		if ( pSetProcessDpiAwareness )
		{
			pSetProcessDpiAwareness( 2 ); // PROCESS_PER_MONITOR_DPI_AWARE
		}
		FreeLibrary( dll );
	}
}
#endif


static const char *GetExceptionName( DWORD code )
{
	static char buf[ 32 ];

	switch ( code )
	{
		case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
		case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
		case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
		case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
		case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
		case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "NONCONTINUABLE_EXCEPTION";
		case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
		case EXCEPTION_INVALID_DISPOSITION: return "INVALID_DISPOSITION";
		case EXCEPTION_GUARD_PAGE: return "GUARD_PAGE";
		case EXCEPTION_INVALID_HANDLE: return "INVALID_HANDLE";
		default: break;
	}

	sprintf( buf, "0x%08X", (unsigned int)code );
	return buf;
}


/*
==================
ExceptionFilter

Restore gamma and hide fullscreen window in case of crash
==================
*/
static LONG WINAPI ExceptionFilter( struct _EXCEPTION_POINTERS *ExceptionInfo )
{
#ifndef DEDICATED
	if ( com_dedicated->integer == 0 ) {
		extern cvar_t *com_cl_running;
		if ( com_cl_running  && com_cl_running->integer ) {
			// assume we can restart client module
		} else {
			GLW_RestoreGamma();
			GLW_HideFullscreenWindow();
		}
	}
#endif

	if ( ExceptionInfo->ExceptionRecord->ExceptionCode != EXCEPTION_BREAKPOINT )
	{
		char msg[128], name[MAX_OSPATH];
		const char *basename;
		HMODULE hModule, hKernel32;
		byte *addr;

		hModule = NULL;
		name[0] = '\0';
		basename = name;
		addr = (byte*)ExceptionInfo->ExceptionRecord->ExceptionAddress;

		hKernel32 = GetModuleHandleA( "kernel32" );
		if ( hKernel32 != NULL ) {
			typedef BOOL (WINAPI *PFN_GetModuleHandleExA)( DWORD dwFlags, LPCSTR lpModuleName, HMODULE *phModule );
			PFN_GetModuleHandleExA pGetModuleHandleExA;

			pGetModuleHandleExA = (PFN_GetModuleHandleExA) GetProcAddress( hKernel32, "GetModuleHandleExA" );
			if ( pGetModuleHandleExA != NULL ) {
				if ( pGetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCTSTR)addr, &hModule ) ) {
					if (GetModuleFileNameA( hModule, name, ARRAY_LEN(name) - 1) != 0 ) {
						name[ARRAY_LEN(name) - 1] = '\0';
						basename = strrchr( name, '\\' );
						if ( basename ) {
							basename = basename + 1;
						}
						else {
							basename = strrchr( name, '/' );
							if ( basename ) {
								basename = basename + 1;
							}
						}
					}
				}
			}
		}

		if ( basename && *basename ) {
			Com_sprintf( msg, sizeof( msg ), "Exception Code: %s\nException Address: %s@%x",
				GetExceptionName( ExceptionInfo->ExceptionRecord->ExceptionCode ),
				basename, (uint32_t)(addr - (byte*)hModule) );
		} else {
			Com_sprintf( msg, sizeof( msg ), "Exception Code: %s\nException Address: %p",
				GetExceptionName( ExceptionInfo->ExceptionRecord->ExceptionCode ),
				addr );
		}

		Com_Error( ERR_DROP, "Unhandled exception caught\n%s", msg );
	}

	return EXCEPTION_EXECUTE_HANDLER;
}


/*
==================
Sys_RestartProcess

Relaunch the engine process.
==================
*/
void NORETURN Sys_RestartProcess( void )
{
	TCHAR exePath[MAX_OSPATH];
	STARTUPINFO si;
	PROCESS_INFORMATION pi;

	GetModuleFileName( NULL, exePath, MAX_OSPATH );
	memset( &si, 0, sizeof( si ) );
	si.cb = sizeof( si );

	if ( CreateProcess( exePath, GetCommandLine(), NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi ) ) {
		// allow the child process to set itself as foreground
		AllowSetForegroundWindow( pi.dwProcessId );
		CloseHandle( pi.hProcess );
		CloseHandle( pi.hThread );
	}
	ExitProcess( 0 );
}


/*
==================
Sys_RemoveDirectoryRecursive

Recursively delete a directory and all its contents (Windows).
==================
*/
static void Sys_RemoveDirectoryRecursive( const char *path )
{
	WIN32_FIND_DATA fd;
	char pattern[MAX_OSPATH];
	char childPath[MAX_OSPATH];
	HANDLE hFind;

	Com_sprintf( pattern, sizeof( pattern ), "%s\\*", path );
	hFind = FindFirstFile( pattern, &fd );
	if ( hFind == INVALID_HANDLE_VALUE )
		return;

	do {
		if ( strcmp( fd.cFileName, "." ) == 0 || strcmp( fd.cFileName, ".." ) == 0 )
			continue;

		Com_sprintf( childPath, sizeof( childPath ), "%s\\%s", path, fd.cFileName );

		if ( fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY )
			Sys_RemoveDirectoryRecursive( childPath );
		else
			DeleteFile( childPath );
	} while ( FindNextFile( hFind, &fd ) );

	FindClose( hFind );
	RemoveDirectory( path );
}


/*
==================
Sys_ApplyPendingUpdate

Called before Com_Init to apply a staged update from .updates/pending/.
Reads the manifest, backs up current files to .updates/previous/,
moves staged files into place. If the running executable was replaced,
re-launches itself. On next launch, cleans up the .updates/ tree.
==================
*/
void Sys_ApplyPendingUpdate( void )
{
	char pwd[MAX_OSPATH];
	char updatesDir[MAX_OSPATH];
	char pendingDir[MAX_OSPATH];
	char previousDir[MAX_OSPATH];
	char manifestPath[MAX_OSPATH];
	char line[MAX_OSPATH * 2];
	char srcPath[MAX_OSPATH];
	char dstPath[MAX_OSPATH];
	char bakPath[MAX_OSPATH];
	char version[64];
	FILE *f;
	qboolean exeSwapped = qfalse;
	int applied = 0;
	char *sep;

	#define MAX_UPDATE_FILES 256
	char movedDst[MAX_UPDATE_FILES][MAX_OSPATH];
	char movedBak[MAX_UPDATE_FILES][MAX_OSPATH];
	int movedCount = 0;

	// determine install directory
	{
		TCHAR buffer[MAX_OSPATH];
		char *s;
		GetModuleFileName( NULL, buffer, MAX_OSPATH );
		buffer[MAX_OSPATH - 1] = '\0';
		Q_strncpyz( pwd, buffer, sizeof( pwd ) );
		s = strrchr( pwd, '\\' );
		if ( !s ) s = strrchr( pwd, '/' );
		if ( s ) *s = '\0';
	}

	Com_sprintf( updatesDir, sizeof( updatesDir ), "%s\\.updates", pwd );
	Com_sprintf( pendingDir, sizeof( pendingDir ), "%s\\pending", updatesDir );
	Com_sprintf( previousDir, sizeof( previousDir ), "%s\\previous", updatesDir );
	Com_sprintf( manifestPath, sizeof( manifestPath ), "%s\\update_manifest.dat", pendingDir );

	f = fopen( manifestPath, "r" );
	if ( !f ) {
		// no pending update — clean up .updates/ tree from a previous update
		// Brief delay to let the previous process fully exit and release file locks
		// on the backed-up exe in .updates/previous/ (race with CreateProcess/ExitProcess)
		{
			DWORD attrs = GetFileAttributes( updatesDir );
			if ( attrs != INVALID_FILE_ATTRIBUTES && ( attrs & FILE_ATTRIBUTE_DIRECTORY ) ) {
				Sleep( 1000 );
			}
		}
		Sys_RemoveDirectoryRecursive( updatesDir );
		return;
	}

	// create .updates/previous/ for backups
	CreateDirectory( updatesDir, NULL );
	CreateDirectory( previousDir, NULL );

	version[0] = '\0';

	while ( fgets( line, sizeof( line ), f ) ) {
		char *nl, *pipe;
		int len;

		nl = strchr( line, '\n' );
		if ( nl ) *nl = '\0';
		nl = strchr( line, '\r' );
		if ( nl ) *nl = '\0';

		len = (int)strlen( line );
		if ( len == 0 )
			continue;

		if ( strncmp( line, "version=", 8 ) == 0 ) {
			Q_strncpyz( version, line + 8, sizeof( version ) );
			continue;
		}

		// parse file entry: relative_path|CRC32
		pipe = strchr( line, '|' );
		if ( pipe )
			*pipe = '\0';

		// convert forward slashes to backslashes for Windows
		{
			char *p;
			for ( p = line; *p; p++ ) {
				if ( *p == '/' ) *p = '\\';
			}
		}

		Com_sprintf( srcPath, sizeof( srcPath ), "%s\\%s", pendingDir, line );
		Com_sprintf( dstPath, sizeof( dstPath ), "%s\\%s", pwd, line );
		Com_sprintf( bakPath, sizeof( bakPath ), "%s\\%s", previousDir, line );

		// ensure destination directory exists
		sep = strrchr( dstPath, '\\' );
		if ( sep ) {
			char dir[MAX_OSPATH];
			int dirLen = (int)( sep - dstPath );
			Q_strncpyz( dir, dstPath, dirLen + 1 );
			CreateDirectory( dir, NULL );
		}

		// ensure backup directory exists
		sep = strrchr( bakPath, '\\' );
		if ( sep ) {
			char dir[MAX_OSPATH];
			int dirLen = (int)( sep - bakPath );
			Q_strncpyz( dir, bakPath, dirLen + 1 );
			CreateDirectory( dir, NULL );
		}

		// backup current file to .updates/previous/
		MoveFile( dstPath, bakPath );

		// move staged file into place
		if ( !MoveFile( srcPath, dstPath ) ) {
			int i;
			fprintf( stderr, "Update: failed to move %s -> %s (error %lu)\n",
				srcPath, dstPath, GetLastError() );
			MoveFile( bakPath, dstPath );
			for ( i = 0; i < movedCount; i++ ) {
				DeleteFile( movedDst[i] );
				MoveFile( movedBak[i], movedDst[i] );
			}
			fclose( f );
			DeleteFile( manifestPath );
			return;
		}

		if ( movedCount < MAX_UPDATE_FILES ) {
			Q_strncpyz( movedDst[movedCount], dstPath, MAX_OSPATH );
			Q_strncpyz( movedBak[movedCount], bakPath, MAX_OSPATH );
			movedCount++;
		}

		// check if we just replaced the running executable
		{
			char exePath[MAX_OSPATH];
			GetModuleFileName( NULL, exePath, MAX_OSPATH );
			if ( Q_stricmp( dstPath, exePath ) == 0 )
				exeSwapped = qtrue;
		}

		applied++;
	}

	fclose( f );

	// delete the manifest so next launch triggers cleanup
	DeleteFile( manifestPath );

	if ( applied > 0 ) {
		fprintf( stderr, "Update: applied %d files (version %s)\n", applied, version );
	}

	if ( exeSwapped ) {
		Sys_RestartProcess();
	}
}


/*
==================
WinMain
==================
*/
int WINAPI WinMain( HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow ) 
{
	static char	sys_cmdline[ MAX_STRING_CHARS ];
	char con_title[ MAX_CVAR_VALUE_STRING ];
	int xpos, ypos;
	qboolean useXYpos;
	HANDLE hProcess;
	DWORD dwPriority;

	// should never get a previous instance in Win32
	if ( hPrevInstance ) {
		return 0;
	}

	// slightly boost process priority if it set to default
	hProcess = GetCurrentProcess();
	dwPriority = GetPriorityClass( hProcess );
	if ( dwPriority == NORMAL_PRIORITY_CLASS || dwPriority == ABOVE_NORMAL_PRIORITY_CLASS ) {
		SetPriorityClass( hProcess, HIGH_PRIORITY_CLASS );
	}

	//SetDPIAwareness();

	g_wv.hInstance = hInstance;
	Q_strncpyz( sys_cmdline, lpCmdLine, sizeof( sys_cmdline ) );

	useXYpos = Com_EarlyParseCmdLine( sys_cmdline, con_title, sizeof( con_title ), &xpos, &ypos );

	// done before Com/Sys_Init since we need this for error output
	Sys_CreateConsole( con_title, xpos, ypos, useXYpos );

	// no abort/retry/fail errors
	SetErrorMode( SEM_FAILCRITICALERRORS );

	SetUnhandledExceptionFilter( ExceptionFilter );

	Sys_ApplyPendingUpdate();

	Com_Init( sys_cmdline );

	// hide the early console since we've reached the point where we
	// have a working graphics subsystems
	if ( !com_dedicated->integer && !com_viewlog->integer ) {
		Sys_ShowConsole( 0, qfalse );
	}

	// main game loop
	while ( 1 ) {
		// set low precision every frame, because some system calls
		// reset it arbitrarily
		// _controlfp( _PC_24, _MCW_PC );
		// _controlfp( -1, _MCW_EM  ); // no exceptions, even if some crappy syscall turns them back on!

#ifdef DEDICATED
		// run the game
		Com_Frame( qfalse );
#else
		// make sure mouse and joystick are only called once a frame
		IN_Frame();
		// run the game
		Com_Frame( CL_NoDelay() );
#endif
	}

	// never gets here
	return 0;
}
