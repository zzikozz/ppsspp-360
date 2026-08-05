// Headless version of PPSSPP, for testing using http://code.google.com/p/pspautotests/ .
// See headless.txt.
// To build on non-windows systems, just run CMake in the SDL directory, it will build both a normal ppsspp and the headless version.

#include <xtl.h>
#include <stdio.h>


#include "Core/Config.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/System.h"
#include "Core/HLE/sceUtility.h"
#include "Core/HLE/sceCtrl.h"
#include "Core/MIPS/MIPS.h"
#include "GPU/GPUState.h"
#include "Core/Host.h"
#include "Log.h"
#include "LogManager.h"
#include "native/input/input_state.h"
#include "native/base/NativeApp.h"

#include "Compare.h"
#include "StubHost.h"
#include "XboxHost.h"
#include "InputDevice.h"
#include "XinputDevice.h"

#include "XboxOnScreenDisplay.h"
#include "XboxFpsOverlay.h"
#include "XboxLauncher.h"
#include "Gpu/Directx9/helper/global.h"

XinputDevice XInput;

// 1 megabyte
#define MB	(1024*1024)

// Add one line of text to the output buffer.
#define AddStr(a,b) (pstrOut += wsprintf( pstrOut, a, b ))

void displaymem()
{
    MEMORYSTATUS stat;
    CHAR strOut[1024], *pstrOut;

    // Get the memory status.
    GlobalMemoryStatus( &stat );

    // Setup the output string.
    pstrOut = strOut;
    AddStr( "%4u total MB of virtual memory.\n", stat.dwTotalVirtual / MB );
    AddStr( "%4u  free MB of virtual memory.\n", stat.dwAvailVirtual / MB );
    AddStr( "%4u total MB of physical memory.\n", stat.dwTotalPhys / MB );
    AddStr( "%4u  free MB of physical memory.\n", stat.dwAvailPhys / MB );
    AddStr( "%4u total MB of paging file.\n", stat.dwTotalPageFile / MB );
    AddStr( "%4u  free MB of paging file.\n", stat.dwAvailPageFile / MB );
    AddStr( "%4u  percent of memory is in use.\n", stat.dwMemoryLoad );

    // Output the string.
    OutputDebugString( strOut );
}


class PrintfLogger : public LogListener
{
public:
	void Log(LogTypes::LOG_LEVELS level, const char *msg)
	{
		switch (level)
		{
		case LogTypes::LVERBOSE:
			fprintf(stderr, "V %s", msg);
			break;
		case LogTypes::LDEBUG:
			fprintf(stderr, "D %s", msg);
			break;
		case LogTypes::LINFO:
			fprintf(stderr, "I %s", msg);
			break;
		case LogTypes::LERROR:
			fprintf(stderr, "E %s", msg);
			break;
		case LogTypes::LWARNING:
			fprintf(stderr, "W %s", msg);
			break;
		case LogTypes::LNOTICE:
		default:
			fprintf(stderr, "N %s", msg);
			break;
		}
	}
};


struct InputState;
// Temporary hack around annoying linking error.
void GL_SwapBuffers() { 
	DebugBreak();
}
void NativeUpdate(InputState &input_state) { }
void NativeRender() { }

extern InputState input_state;


static inline float curve1(float x) {
	const float deadzone = 0.15f;
	const float factor = 1.0f / (1.0f - deadzone);
	if (x > deadzone) {
		return (x - deadzone) * (x - deadzone) * factor;
	} else if (x < -0.1f) {
		return -(x + deadzone) * (x + deadzone) * factor;
	} else {
		return 0.0f;
	}
}

static inline float clamp1(float x) {
	if (x > 1.0f) return 1.0f;
	if (x < -1.0f) return -1.0f;
	return x;
}

static void UpdateInput(InputState &input) {
	input.pad_buttons = 0;
	input.pad_lstick_x = 0;
	input.pad_lstick_y = 0;
	input.pad_rstick_x = 0;
	input.pad_rstick_y = 0;

	// Update input from xinput
	XInput.UpdateState(input);

	input.pad_buttons_down = (input.pad_last_buttons ^ input.pad_buttons) & input.pad_buttons;
	input.pad_buttons_up = (input.pad_last_buttons ^ input.pad_buttons) & input.pad_last_buttons;

	// Then translate pad input into PSP pad input. Also, add in tilt.
	static const int mapping[12][2] = {
		{PAD_BUTTON_A, CTRL_CROSS},
		{PAD_BUTTON_B, CTRL_CIRCLE},
		{PAD_BUTTON_X, CTRL_SQUARE},
		{PAD_BUTTON_Y, CTRL_TRIANGLE},
		{PAD_BUTTON_UP, CTRL_UP},
		{PAD_BUTTON_DOWN, CTRL_DOWN},
		{PAD_BUTTON_LEFT, CTRL_LEFT},
		{PAD_BUTTON_RIGHT, CTRL_RIGHT},
		{PAD_BUTTON_LBUMPER, CTRL_LTRIGGER},
		{PAD_BUTTON_RBUMPER, CTRL_RTRIGGER},
		{PAD_BUTTON_START, CTRL_START},
		{PAD_BUTTON_SELECT, CTRL_SELECT},
	};

	for (int i = 0; i < 12; i++) {
		if (input.pad_buttons_down & mapping[i][0]) {
			__CtrlButtonDown(mapping[i][1]);
		}
		if (input.pad_buttons_up & mapping[i][0]) {
			__CtrlButtonUp(mapping[i][1]);
		}
	}

	float stick_x = input.pad_lstick_x;
	float stick_y = input.pad_lstick_y;
	float rightstick_x = input.pad_rstick_x;
	float rightstick_y = input.pad_rstick_y;

	// Apply tilt to left stick
	if (g_Config.bAccelerometerToAnalogHoriz) {
		// TODO: Deadzone, etc.
		stick_x += clamp1(curve1(input.acc.y) * 2.0f);
		stick_x = clamp1(stick_x);
	}

	__CtrlSetAnalogX(stick_x, 0);
	__CtrlSetAnalogY(stick_y, 0);

	__CtrlSetAnalogX(rightstick_x, 1);
	__CtrlSetAnalogY(rightstick_y, 1);

	input.pad_last_buttons = input.pad_buttons;
}


std::string System_GetProperty(SystemProperty prop) { return ""; }

extern bool useVsync;

// No devkit / XBDM available on this console, so there's no debugger to catch
// unhandled exceptions. This SEH filter runs before the process is killed and
// writes the exception code + faulting address to a log file we can retrieve
// over FTP afterwards. It won't catch a hard kernel-level fault (those happen
// before any of our code runs), but it will catch normal access violations,
// bad pointer derefs, etc. happening inside the emulation loop below.
static int LogCrashAndContinue(unsigned int code, struct _EXCEPTION_POINTERS *ep)
{
	FILE *f = fopen("game:\\crash_log.txt", "a");
	if (f) {
		fprintf(f, "FATAL EXCEPTION: code=0x%08X at address=0x%p\r\n",
			code, ep->ExceptionRecord->ExceptionAddress);
		fclose(f);
	}
	return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, const char* argv[])
{
	bool fullLog = true;
	bool useJit = true;
	bool autoCompare = false;
	bool useGraphics = false;
	
	const char *bootFilename = 0;
	const char *mountIso = 0;
	const char *screenshotFilename = 0;
	bool readMount = false;

	displaymem();

	DWORD launchSize = 0;
	XGetLaunchDataSize(&launchSize);

	useJit = true;
	
	bootFilename = "game:\\psp.iso";

	/*
	swap32_struct_t l;
	printf("Szir of u32_le: %d\r\n", sizeof(u32_le));
	*/
	DX9::DirectxInit(NULL);

	XboxHost *xbhost = new XboxHost();
	host = xbhost;

	std::string error_string;
	bool glWorking = host->InitGL(&error_string);

	LogManager::Init();
	LogManager::GetInstance()->ChangeFileLog("game:\\ppsspp_log.txt");

	// Load config first so the launcher can use saved settings
	g_Config.Load("game:\\ppsspp.ini", "game:\\controls.ini");

	g_Config.memCardDirectory = "game:\\memstick\\";
	g_Config.flash0Directory = "game:\\flash0\\";
	g_Config.bEnableLogging = true;
	g_Config.bEnableSound = true;
	g_Config.bLowLatencyAudio = true;
	g_Config.bFirstRun = false;
	g_Config.bIgnoreBadMemAccess = true;
	g_Config.bFastMemory = true;
	g_Config.sReportHost = "";
	g_Config.bAutoSaveSymbolMap = false;
	g_Config.iRenderingMode = 0;
	g_Config.bHardwareTransform = true;
	g_Config.iAnisotropyLevel = 8;
	g_Config.bVertexCache = false;
	g_Config.bTrueColor = true;
	g_Config.iLanguage = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
	g_Config.iTimeFormat = PSP_SYSTEMPARAM_TIME_FORMAT_24HR;
	g_Config.bEncryptSave = true;
	g_Config.sNickName = "shadow";
	g_Config.iTimeZone = 60;
	g_Config.iDateFormat = PSP_SYSTEMPARAM_DATE_FORMAT_DDMMYYYY;
	g_Config.iButtonPreference = PSP_SYSTEMPARAM_BUTTON_CROSS;
	g_Config.iLockParentalLevel = 9;
	g_Config.iShowFPSCounter = true;
	g_Config.bSeparateCPUThread = true;
	g_Config.bSeparateIOThread = true;
	g_Config.iTexScalingLevel = 0;
	g_Config.iTexScalingType = 0;
	g_Config.iNumWorkerThreads = 5;
	g_Config.iFpsLimit = 60;
	g_Config.iForceMaxEmulatedFPS = 60;
	g_Config.iFrameSkip = 0;
	g_Config.iLockedCPUSpeed = 111;

	// Save after applying Xbox defaults (user can edit via FTP)
	g_Config.Save();

	// ---------------------------------------------------------------
	// Game launcher + PSP emulation (PSP restarted on re-select)
	// ---------------------------------------------------------------

	XboxLauncher launcher;
	launcher.Init();

	bool pspInited = false;
	std::string currentGame;

	while (true) {
		// ---- Launcher loop (runs at least once, repeats after Exit to Menu) ----
		while (launcher.IsActive()) {
			launcher.Update();
			DX9::BeginFrame();
			launcher.Render();
			DX9::EndFrame();
			DX9::SwapBuffers();
			DX9::pD3Ddevice->SetVertexShader(NULL);
			DX9::pD3Ddevice->SetPixelShader(NULL);
			DX9::pD3Ddevice->SetVertexDeclaration(NULL);
			static DWORD launcherTick = 0;
			DWORD t = GetTickCount();
			if (launcherTick > 0) {
				DWORD elapsed = t - launcherTick;
				if (elapsed < 15) {
					Sleep(16 - (int)elapsed);
				}
			}
			launcherTick = GetTickCount();
		}

		// If no game selected, user wants to exit
		if (launcher.GetSelectedGame().empty()) {
			if (launcher.WantsSettings()) {
				launcher.SetActive(true);
				continue;
			}
			break;
		}

		std::string selectedGame = launcher.GetSelectedGame();

		// ---- Init or switch PSP game ----
		if (!pspInited) {
			// First launch: init fresh
			CoreParameter coreParameter;
			coreParameter.cpuCore = CPU_JIT;
			coreParameter.gpuCore = GPU_DIRECTX9;
			coreParameter.enableSound = g_Config.bEnableSound;
			coreParameter.fileToStart = selectedGame;
			coreParameter.mountIso = "";
			coreParameter.startPaused = false;
			coreParameter.printfEmuLog = true;
			coreParameter.headLess = false;
			coreParameter.renderWidth = 1280;
			coreParameter.renderHeight = 720;
			coreParameter.outputWidth = 480*2;
			coreParameter.outputHeight = 272*2;
			coreParameter.pixelWidth = 1280;
			coreParameter.pixelHeight = 720;
			coreParameter.unthrottle = false;

			if (!PSP_Init(coreParameter, &error_string)) {
				fprintf(stderr, "Failed to start %s. Error: %s\n", selectedGame.c_str(), error_string.c_str());
				printf("TESTERROR\n");
				launcher.SetActive(true);
				continue;
			}
			pspInited = true;
			currentGame = selectedGame;
			host->BootDone();
			xbhost->BeginFrame();
		} else {
			// Full restart (same game or different game)
			PSP_Shutdown();

			CoreParameter coreParam;
			coreParam.cpuCore = CPU_JIT;
			coreParam.gpuCore = GPU_DIRECTX9;
			coreParam.enableSound = g_Config.bEnableSound;
			coreParam.fileToStart = selectedGame;
			coreParam.mountIso = "";
			coreParam.startPaused = false;
			coreParam.printfEmuLog = true;
			coreParam.headLess = false;
			coreParam.renderWidth = 1280;
			coreParam.renderHeight = 720;
			coreParam.outputWidth = 480*2;
			coreParam.outputHeight = 272*2;
			coreParam.pixelWidth = 1280;
			coreParam.pixelHeight = 720;
			coreParam.unthrottle = false;

			if (!PSP_Init(coreParam, &error_string)) {
				fprintf(stderr, "Failed to restart %s. Error: %s\n", selectedGame.c_str(), error_string.c_str());
				launcher.SetActive(true);
				pspInited = false;
				continue;
			}
			host->BootDone();
			currentGame = selectedGame;
		}

		launcher.SetInGameMenuActive(false);
		xbhost->BeginFrame();

		// ---- Emulation loop ----
		bool returnToMenu = false;
		bool resetGame = false;
		bool exitToXbox = false;
		bool igmWasActive = false;
		__try
		{
			coreState = CORE_RUNNING;
			XINPUT_STATE igmState;
			ZeroMemory(&igmState, sizeof(igmState));
			while (coreState == CORE_RUNNING)
			{
				XInputGetState(0, &igmState);
				DWORD igmButtons = igmState.Gamepad.wButtons;
				BYTE igmLT = igmState.Gamepad.bLeftTrigger;
				BYTE igmRT = igmState.Gamepad.bRightTrigger;

				bool comboHeld = (igmButtons & XINPUT_GAMEPAD_LEFT_THUMB)
					&& igmLT > XINPUT_GAMEPAD_TRIGGER_THRESHOLD && igmRT > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;

				static bool comboPrev = false;
				bool comboPressed = comboHeld && !comboPrev;
				comboPrev = comboHeld;

				if (comboPressed && !launcher.IsInGameMenuActive()) {
					launcher.SetInGameMenuActive(true);
				}

				// Fast forward: RB + RT, matching PPSSPP's fast-forward.
				// Unthrottling the emulator (which DoFrameTiming respects) makes
				// it run as fast as the CPU can go, and only while held.
				bool ffHeld = (igmButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER)
					&& igmRT > XINPUT_GAMEPAD_TRIGGER_THRESHOLD;
				PSP_CoreParameter().unthrottle = ffHeld;

				if (launcher.IsInGameMenuActive()) {
					launcher.UpdateInGameMenu(igmButtons);
					launcher.RenderInGameMenu();

					xbhost->EndFrame();
					xbhost->SwapBuffers();
					xbhost->BeginFrame();

					static DWORD menuTick = 0;
					DWORD t = GetTickCount();
					if (menuTick > 0) {
						DWORD elapsed = t - menuTick;
						if (elapsed < 15) {
							Sleep(16 - (int)elapsed);
						}
					}
					menuTick = GetTickCount();

					if (launcher.WantsExitToMenu()) {
						launcher.ClearExitToMenu();
						returnToMenu = true;
						coreState = CORE_ERROR;
					} else if (launcher.WantsResetGame()) {
						launcher.ClearResetGame();
						resetGame = true;
						coreState = CORE_ERROR;
					} else if (launcher.WantsExitToXbox()) {
						launcher.ClearExitToXbox();
						exitToXbox = true;
						coreState = CORE_ERROR;
					}
				} else {
					// On transition from menu -> game, consume held buttons
					// so they don't leak through to the emulated PSP
					if (igmWasActive) {
						XInput.UpdateState(input_state);
						input_state.pad_last_buttons = input_state.pad_buttons;
					}

					u64 nowTicks = CoreTiming::GetTicks();
					u64 frameTicks = usToCycles(1000000/60);

					PSP_RunLoopUntil(nowTicks + frameTicks);

					UpdateInput(input_state);

					if (coreState == CORE_NEXTFRAME) {
						coreState = CORE_RUNNING;

					XboxFpsOverlay::DrawFpsOverlay();
					launcher.RenderToast();
					xbhost->EndFrame();
						xbhost->SwapBuffers();
						xbhost->BeginFrame();
					}
					igmWasActive = launcher.IsInGameMenuActive();
				}
			}
		}
		__except (LogCrashAndContinue(GetExceptionCode(), GetExceptionInformation()))
		{
			pspInited = false;
		}

		if (returnToMenu) {
			launcher.SetActive(true);
			launcher.SetInGameMenuActive(false);
			// PSP stays alive - restarted when same game is re-selected
			continue;
		}
		if (resetGame) {
			// Reset the current game: shut down and re-init the same ISO.
			if (pspInited) {
				PSP_Shutdown();
			}
			pspInited = false;
			launcher.SetInGameMenuActive(false);
			continue;
		}
		if (exitToXbox) {
			launcher.SetInGameMenuActive(false);
			break;
		}
		break;
	}

	// Cleanup
	if (pspInited) {
		PSP_Shutdown();
	}
	host->ShutdownGL();
	delete host;
	host = NULL;
	xbhost = NULL;

	launcher.Shutdown();
	return 0;
}

