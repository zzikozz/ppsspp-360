#pragma once

// Minimal, self-contained on-screen FPS counter for the Xbox 360 port.
//
// The existing PPSSPP FPS counter (g_Config.iShowFPSCounter, drawn in
// UI/EmuScreen.cpp) never runs on Xbox: XboxMain.cpp drives PSP_Init /
// PSP_RunLoopUntil directly and never instantiates EmuScreen/ScreenManager.
// XboxOnScreenDisplay.cpp (osm) is also an empty stub - no text rendering
// exists on this platform at all.
//
// This draws the FPS value as a handful of filled quads (a tiny hand-rolled
// 3x5 pixel digit font), directly via D3D9 fixed-function draws. No texture,
// no font asset, no dependency on the UI/ScreenManager framework - just
// raw screen-space triangles, so it should be safe to call from the bare
// Xbox game loop.
//
// Call DrawFpsOverlay() once per frame, after the game's own rendering has
// finished (after xbhost->EndFrame()) and before xbhost->SwapBuffers()/Present.

namespace XboxFpsOverlay {
	// Call once per frame from the main loop. Handles its own frame timing
	// internally - just call it every frame and it'll update/display the
	// FPS value on its own schedule (recomputed once per second).
	void DrawFpsOverlay();
}
