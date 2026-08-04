#pragma once

// PPSSPP-style game launcher for Xbox 360.
//
// Two-column layout matching the original PPSSPP UI:
//   Left:  Tab bar (Recent | Games) + scrollable game list/grid with icons
//   Right: PPSSPP logo, version, Settings button, Exit button
//
// Game icons are loaded from ICON0.PNG inside ISO/CSO files using
// the core ISOFileSystem + stb_image PNG decoder.

#include <xtl.h>
#include <string>
#include <vector>
#include <map>

#include "native/ext/stb_truetype/stb_truetype.h"
#include "UI/ui_atlas.h"
#include <cmath>

class XboxLauncher {
public:
	struct GameEntry {
		std::string path;
		std::string filename;
		std::string title;
	};

	enum Tab { TAB_RECENT = 0, TAB_GAMES, TAB_COUNT };
	enum ViewMode { VIEW_LIST = 0, VIEW_GRID };
	enum FocusRegion { FOCUS_TABS = 0, FOCUS_GAMES, FOCUS_RIGHT_PANEL };
	enum InGameMenuFocus { INGMENU_LEFT = 0, INGMENU_RIGHT };

	XboxLauncher();
	~XboxLauncher();

	void Init();
	void Shutdown();

	bool IsActive() const { return active_; }
	void SetActive(bool a) { active_ = a; }

	void Update();
	void Render();

	const std::string &GetSelectedGame() const { return selectedGame_; }
	bool WantsSettings() const { return wantsSettings_; }

	// In-game menu (used during emulation)
	bool IsInGameMenuActive() const { return inGameMenuActive_; }
	void SetInGameMenuActive(bool active) { inGameMenuActive_ = active; wantsExitToMenu_ = false; wantsExitToXbox_ = false; wantsResetGame_ = false; inGameMenuSel_ = 0; inGameMenuFocus_ = INGMENU_LEFT; }
	bool WantsExitToMenu() const { return wantsExitToMenu_; }
	void ClearExitToMenu() { wantsExitToMenu_ = false; }
	bool WantsExitToXbox() const { return wantsExitToXbox_; }
	void ClearExitToXbox() { wantsExitToXbox_ = false; }
	bool WantsResetGame() const { return wantsResetGame_; }
	void ClearResetGame() { wantsResetGame_ = false; }
	void UpdateInGameMenu(DWORD buttons);
	void RenderInGameMenu();
	void ShowToast(const char *msg);
	void RenderToast();

private:
	void ScanGames();
	void ScanRecent();
	void ScanDirs();
	void SaveRecent();
	void LoadRecent();
	void LoadSearchDirs();
	void SaveSearchDirs();
	void BrowseDirectories();
	void RenderSplash();

	IDirect3DTexture9 *LoadGameIcon(const std::string &gamePath);
	IDirect3DTexture9 *GetOrCreateIcon(const std::string &gamePath);
	IDirect3DTexture9 *LoadGamePoster(const std::string &gamePath);
	IDirect3DTexture9 *GetOrCreatePoster(const std::string &gamePath);
	IDirect3DTexture9 *LoadPNGTexture(const char *path);
	void DrawTexture(IDirect3DTexture9 *tex, float x, float y, float w, float h);

	void EnsureResources();
	void DrawRect(float x0, float y0, float x1, float y1, DWORD color);
	void DrawString(const char *str, int screenX, int screenY, int scale, DWORD color);
	void DrawStringCentered(const char *str, int screenY, int scale, DWORD color);
	int  StringWidth(const char *str, int scale);

	// Layout rendering
	void RenderTabBar();
	void RenderGameList();
	void RenderGrid();
	void RenderRightPanel();
	void RenderListItem(int gameIdx, float y, bool isSelected);
	void RenderGridItem(int gameIdx, float x, float y, bool isSelected);

	// State
	bool active_;
	bool wantsSettings_;
	bool scanDone_;
	std::vector<std::string> searchDirs_;
	int selectedIndex_;
	int scrollOffset_;
	Tab currentTab_;
	ViewMode viewMode_;
	FocusRegion focusRegion_;
	int rightPanelItem_;
	std::string selectedGame_;

	std::vector<GameEntry> recentGames_;
	std::vector<GameEntry> allGames_;

	// Game icons and posters: path -> DX9 texture
	std::map<std::string, IDirect3DTexture9 *> iconCache_;
	std::map<std::string, IDirect3DTexture9 *> posterCache_;

	// Asset textures
	IDirect3DTexture9 *unknownTex_;
	IDirect3DTexture9 *splashTex_;
	IDirect3DTexture9 *zimTex_;
	void LoadAtlas();
	void DrawTextureAtlas(IDirect3DTexture9 *atlasTex, const AtlasImage &img, float x, float y, float w, float h);
	void DrawTextureAtlasRotated(IDirect3DTexture9 *atlasTex, const AtlasImage &img, float x, float y, float w, float h, float angle, DWORD color);
	void DrawShadow4Grid(IDirect3DTexture9 *atlasTex, const AtlasImage &img, float x1, float y1, float x2, float y2, DWORD color, float cornerScale);

	// XInput
	DWORD prevButtons_;
	BYTE prevRightTrigger_;
	int pollDelay_;
	int repeatDelay_;

	// DX9 resources
	IDirect3DVertexDeclaration9 *vertexDecl_;
	LPDIRECT3DVERTEXSHADER9 vertexShader_;
	LPDIRECT3DPIXELSHADER9 pixelShader_;
	LPDIRECT3DPIXELSHADER9 pixelShaderTex_;
	LPDIRECT3DPIXELSHADER9 pixelShaderTexFull_;
	IDirect3DTexture9 *fontAtlasTex_;
	stbtt_packedchar *fontChars_;
	float fontAscent_;
	float fontDescent_;
	bool fontLoaded_;

	// In-game menu
	bool inGameMenuActive_;
	bool wantsExitToMenu_;
	bool wantsExitToXbox_;
	bool wantsResetGame_;
	int inGameMenuSel_;
	InGameMenuFocus inGameMenuFocus_;
	DWORD igmPrevButtons_;
	int igmRepeatDelay_;

	// Toast
	char toastMsg_[64];
	DWORD toastEndTime_;

	// Background animation
	float startTime_;
	float symX_[100];
	float symY_[100];
	bool initFailed_;
};
