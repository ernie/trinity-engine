#ifndef __AUTOUPDATE_H__
#define __AUTOUPDATE_H__

#ifdef USE_CURL

typedef enum {
	UPDATE_IDLE,        // no update activity
	UPDATE_CHECKING,    // querying GitHub API
	UPDATE_AVAILABLE,   // newer version found, waiting for user
	UPDATE_DOWNLOADING, // downloading release ZIP
	UPDATE_EXTRACTING,  // extracting files to pendingupdate/
	UPDATE_STAGED,      // files ready, restart to apply
	UPDATE_ERROR        // something went wrong
} updateState_t;

void Update_Init( void );
void Update_Frame( void );
void Update_Shutdown( void );

// console commands
void Update_Check_f( void );
void Update_Download_f( void );
void Update_Cancel_f( void );
void Update_Restart_f( void );

#else

// stubs when curl is disabled
#define Update_Init()
#define Update_Frame()
#define Update_Shutdown()

#endif // USE_CURL

#endif // __AUTOUPDATE_H__
