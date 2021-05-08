#include "SDL.h"
#include "SDL_main.h"
#include "SDL_config.h"

#include "../../doomdef.h"
#include "../../d_main.h"
#include "../../m_argv.h"
#include "../../i_system.h"

#ifdef SPLASH_SCREEN
#include "../../i_video.h"
#include "../ogl_es_sdl.h"
#endif

#include <jni_android.h>

#ifdef SPLASH_SCREEN
static INT32 displayingSplash = 0;

static void ShowSplashScreen(void)
{
	displayingSplash = VID_LoadSplashScreen();

	if (displayingSplash)
	{
		// Present it for a single second.
		UINT32 delay = SDL_GetTicks() + 1000;

		VID_BlitSplashScreen();

		while (SDL_GetTicks() < delay)
			VID_PresentSplashScreen();
	}
}
#endif

#define REQUEST_STORAGE_PERMISSION

#define REQUEST_MESSAGE_TITLE "Permission required"
#define REQUEST_MESSAGE_TEXT "Sonic Robo Blast 2 needs storage permission.\nYour settings and game progress will not be saved if you decline."

static void PermissionRequestMessage(void)
{
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, REQUEST_MESSAGE_TITLE, REQUEST_MESSAGE_TEXT, NULL);
}

static boolean StorageInit(void)
{
	JNI_SharedStorage = I_SharedStorageLocation();
	return (JNI_SharedStorage != NULL);
}

static void StorageGrantedPermission(void)
{
	I_mkdir(JNI_SharedStorage, 0755);
	JNI_StoragePermission = true;
}

static boolean StorageCheckPermission(void)
{
	// Permission was already granted. Create the directory anyway.
	if (JNI_CheckStoragePermission())
	{
		StorageGrantedPermission();
		return true;
	}

	PermissionRequestMessage();

	// Permission granted. Create the directory.
	if (I_RequestSystemPermission(JNI_GetWriteExternalStoragePermission()))
	{
		StorageGrantedPermission();
		return true;
	}

	return false;
}

int main(int argc, char* argv[])
{
#ifdef LOGMESSAGES
	boolean logging = (!M_CheckParm("-nolog"));
#endif

	myargc = argc;
	myargv = argv;

	// Obtain the activity class before doing anything else.
	JNI_Startup();

	// Start up the main system.
	I_OutputMsg("I_StartupSystem()...\n");
	I_StartupSystem();

#ifdef SPLASH_SCREEN
	// Load the splash screen, and display it.
	ShowSplashScreen();
#endif

	// Init shared storage.
	if (StorageInit())
		StorageCheckPermission(); // Check storage permissions.

#ifdef LOGMESSAGES
	// Start logging.
	if (logging && I_StoragePermission())
		I_InitLogging();
#endif

	CONS_Printf("Sonic Robo Blast 2 for Android\n");

#ifdef LOGMESSAGES
	if (logstream)
		CONS_Printf("Logfile: %s\n", logfilename);
#endif

	// Begin the normal game setup and loop.
	CONS_Printf("Setting up SRB2...\n");
	D_SRB2Main();

	CONS_Printf("Entering main game loop...\n");
	D_SRB2Loop();

	return 0;
}
