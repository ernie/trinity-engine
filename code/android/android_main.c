/* Android entry: SDLActivity's native thread calls SDL_main, which runs the engine's own main loop. */
#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL.h>

#include "../vrcommon/xr_loader.h"

#define LOG_TAG "Trinity"

/* unix_main.c's main, renamed for this build */
int Sys_UnixMain( int argc, const char *argv[] );

static JavaVM *javaVM;
static jobject activity;
static pthread_mutex_t activityLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t activityReady = PTHREAD_COND_INITIALIZER;
static int logPipe[2];

JNIEXPORT jint JNICALL JNI_OnLoad( JavaVM *vm, void *reserved ) {
	(void)reserved;
	javaVM = vm;
	return JNI_VERSION_1_6;
}

/* MainActivity calls this once its game files and command line are in place. */
JNIEXPORT void JNICALL Java_io_ernie_trinity_MainActivity_nativeCreate( JNIEnv *env, jclass cls, jobject thiz ) {
	(void)cls;
	pthread_mutex_lock( &activityLock );
	if ( !activity )
		activity = (*env)->NewGlobalRef( env, thiz );
	pthread_cond_broadcast( &activityReady );
	pthread_mutex_unlock( &activityLock );
}

/* The engine's console goes to stdout and stderr, which an app process discards. */
static void *Android_LogThread( void *unused ) {
	char line[4096];
	size_t used = 0;
	ssize_t n;
	(void)unused;
	while ( ( n = read( logPipe[0], line + used, sizeof( line ) - 1 - used ) ) > 0 ) {
		char *start = line, *end;
		used += (size_t)n;
		line[used] = '\0';
		while ( ( end = strchr( start, '\n' ) ) != NULL ) {
			*end = '\0';
			__android_log_write( ANDROID_LOG_INFO, LOG_TAG, start );
			start = end + 1;
		}
		used -= (size_t)( start - line );
		memmove( line, start, used );
		if ( used == sizeof( line ) - 1 ) {
			line[used] = '\0';
			__android_log_write( ANDROID_LOG_INFO, LOG_TAG, line );
			used = 0;
		}
	}
	return NULL;
}

static void Android_RedirectOutput( void ) {
	pthread_t thread;
	setvbuf( stdout, NULL, _IOLBF, 0 );
	setvbuf( stderr, NULL, _IONBF, 0 );
	if ( pipe( logPipe ) != 0 )
		return;
	dup2( logPipe[1], STDOUT_FILENO );
	dup2( logPipe[1], STDERR_FILENO );
	if ( pthread_create( &thread, NULL, Android_LogThread, NULL ) == 0 )
		pthread_detach( thread );
}

void Sys_InstallApk( const char *path ) {
	JNIEnv *env;
	jclass cls;
	jmethodID install;
	jstring jpath;

	if ( !javaVM || !activity || (*javaVM)->AttachCurrentThread( javaVM, &env, NULL ) != JNI_OK )
		return;
	cls = (*env)->GetObjectClass( env, activity );
	install = cls ? (*env)->GetMethodID( env, cls, "installApk", "(Ljava/lang/String;)V" ) : NULL;
	if ( install ) {
		jpath = (*env)->NewStringUTF( env, path );
		(*env)->CallVoidMethod( env, activity, install, jpath );
		(*env)->DeleteLocalRef( env, jpath );
	}
	if ( (*env)->ExceptionCheck( env ) )
		(*env)->ExceptionClear( env );
	if ( cls )
		(*env)->DeleteLocalRef( env, cls );
}

void Sys_HapticEvent( const char *event, int position, int flags, int intensity, float angle, float height ) {
	static jmethodID hapticEvent;
	JNIEnv *env;
	jstring jevent;

	if ( !javaVM || !activity || (*javaVM)->GetEnv( javaVM, (void **)&env, JNI_VERSION_1_6 ) != JNI_OK )
		return;
	if ( !hapticEvent ) {
		jclass cls = (*env)->GetObjectClass( env, activity );
		hapticEvent = (*env)->GetMethodID( env, cls, "haptic_event", "(Ljava/lang/String;IIIFF)V" );
		(*env)->DeleteLocalRef( env, cls );
		if ( !hapticEvent ) {
			(*env)->ExceptionClear( env );
			return;
		}
	}
	jevent = (*env)->NewStringUTF( env, event );
	(*env)->CallVoidMethod( env, activity, hapticEvent, jevent, position, flags, intensity, angle, height );
	(*env)->DeleteLocalRef( env, jevent );
	if ( (*env)->ExceptionCheck( env ) )
		(*env)->ExceptionClear( env );
}

/* No signal handlers here: Android's own crash reporting keeps its tombstones. */
void Sys_CheckPendingSignal( void ) {
}

int SDL_main( int argc, char *argv[] ) {
	const char *args[3];
	const char *commandLine;
	(void)argc;
	(void)argv;

	Android_RedirectOutput();

	pthread_mutex_lock( &activityLock );
	while ( !activity )
		pthread_cond_wait( &activityReady, &activityLock );
	pthread_mutex_unlock( &activityLock );
	XRLoader_SetAndroidContext( javaVM, activity );

	/* the headset session pauses and resumes the game; blocking here would stall its event polling */
	SDL_SetHint( SDL_HINT_ANDROID_BLOCK_ON_PAUSE, "0" );
	SDL_SetHint( SDL_HINT_ENABLE_SCREEN_KEYBOARD, "0" );
	/* fs_homepath comes from the command line; an app's HOME is not a writable game directory */
	unsetenv( "HOME" );

	commandLine = getenv( "commandline" );
	args[0] = "trinity";
	args[1] = commandLine ? commandLine : "";
	args[2] = NULL;
	return Sys_UnixMain( 2, args );
}
