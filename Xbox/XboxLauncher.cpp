#define STB_TRUETYPE_IMPLEMENTATION
#include "XboxLauncher.h"

#include <xtl.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <stdint.h>
#include <algorithm>
#include <vector>
#include "base/timeutil.h"

#ifndef INVALID_FILE_ATTRIBUTES
#define INVALID_FILE_ATTRIBUTES ((DWORD)-1)
#endif

#include "GPU/Directx9/helper/global.h"
#include "native/image/png_load.h"
#include "native/image/zim_load.h"
#include "UI/ui_atlas.h"
#include "UI/ui_atlas.cpp"
#include "Core/SaveState.h"
#include "Core/ELF/ParamSFO.h"
#include "Core/FileSystems/BlockDevices.h"
#include "Core/FileSystems/ISOFileSystem.h"

using namespace DX9;

// ---------------------------------------------------------------------------
// Minimal self-contained ISO9660 reader (for extracting ICON0.PNG)
// Only reads plain ISO files; CSO falls back to no icon.
// ---------------------------------------------------------------------------
static const int ISO_SECTOR_SIZE = 2048;

#pragma pack(push, 1)
struct ISO9660_DirRec {
	uint8_t  recLen;
	uint8_t  extAttrLen;
	uint32_t extentLBA_LE;
	uint32_t extentLBA_BE;
	uint32_t extentSize_LE;
	uint32_t extentSize_BE;
	uint8_t  recDateTime[7];
	uint8_t  flags;
	uint8_t  unitSize;
	uint8_t  gapSize;
	uint16_t volSeqNum_LE;
	uint16_t volSeqNum_BE;
	uint8_t  nameLen;
};
#pragma pack(pop)

static uint32_t readLE32(const uint8_t *p) {
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Read a sector from an ISO file
static bool ISO_ReadSector(FILE *f, uint32_t lba, uint8_t *buf) {
	if (fseek(f, (long)lba * ISO_SECTOR_SIZE, SEEK_SET) != 0)
		return false;
	return fread(buf, 1, ISO_SECTOR_SIZE, f) == ISO_SECTOR_SIZE;
}

// Find a file by path like "/PSP_GAME/ICON0.PNG" in ISO9660.
// Returns the LBA and size of the file, or false if not found.
static bool ISO_FindFile(FILE *f, uint32_t rootLBA, uint32_t rootSize,
                         const char *path, uint32_t &outLBA, uint32_t &outSize)
{
	// Work with mutable copy of path
	char pathBuf[256];
	strncpy(pathBuf, path, sizeof(pathBuf) - 1);
	pathBuf[255] = '\0';

	// Skip leading slash
	char *p = pathBuf;
	while (*p == '/') p++;

	// Split into components
	const char *components[16];
	int numComponents = 0;
	while (*p && numComponents < 16) {
		components[numComponents++] = p;
		char *slash = strchr(p, '/');
		if (slash) {
			*slash = '\0';
			p = slash + 1;
		} else {
			break;
		}
	}

	if (numComponents == 0)
		return false;

	uint8_t sector[ISO_SECTOR_SIZE];
	uint32_t curLBA = rootLBA;
	uint32_t curSize = rootSize;

	// Traverse path components (all but the last are directories)
	for (int comp = 0; comp < numComponents; comp++) {
		bool isLast = (comp == numComponents - 1);
		const char *name = components[comp];
		int nameLen = (int)strlen(name);
		bool isDir = !isLast;

		// Search directory entries
		uint32_t bytesSearched = 0;
		while (bytesSearched < curSize) {
			uint32_t offset = bytesSearched % ISO_SECTOR_SIZE;
			if (offset == 0) {
				if (!ISO_ReadSector(f, curLBA + bytesSearched / ISO_SECTOR_SIZE, sector))
					return false;
			}

			ISO9660_DirRec *rec = (ISO9660_DirRec *)(sector + offset);
			if (rec->recLen == 0) {
				// Move to next sector boundary
				bytesSearched = ((bytesSearched / ISO_SECTOR_SIZE) + 1) * ISO_SECTOR_SIZE;
				continue;
			}

			if (rec->nameLen == 0 && rec->recLen == 1) {
				// "." entry, skip
				bytesSearched += rec->recLen;
				continue;
			}

			// Get entry name (uppercase in ISO9660)
			char entryName[256];
			int copyLen = rec->nameLen;
			if (copyLen > 255) copyLen = 255;
			memcpy(entryName, sector + offset + sizeof(ISO9660_DirRec), copyLen);
			entryName[copyLen] = '\0';

			// Strip ";1" version suffix
			char *semi = strchr(entryName, ';');
			if (semi) *semi = '\0';

			bool match = false;
			if (_stricmp(entryName, name) == 0) {
				match = true;
			}

			if (match) {
				uint32_t fileLBA = readLE32((const uint8_t *)&rec->extentLBA_LE);
				uint32_t fileSize = readLE32((const uint8_t *)&rec->extentSize_LE);

				if (isDir) {
					curLBA = fileLBA;
					curSize = fileSize;
					break;
				} else {
					outLBA = fileLBA;
					outSize = fileSize;
					return true;
				}
			}

			bytesSearched += rec->recLen;
		}
	}
	return false;
}

// Read entire file content from ISO
static bool ISO_ReadFile(FILE *f, uint32_t lba, uint32_t size, std::vector<uint8_t> &out) {
	out.resize(size);
	uint32_t offset = 0;
	uint8_t sector[ISO_SECTOR_SIZE];

	while (offset < size) {
		uint32_t sectorIdx = offset / ISO_SECTOR_SIZE;
		uint32_t sectorOff = offset % ISO_SECTOR_SIZE;
		if (sectorOff == 0) {
			if (!ISO_ReadSector(f, lba + sectorIdx, sector))
				return false;
		}
		uint32_t toCopy = ISO_SECTOR_SIZE - sectorOff;
		if (toCopy > size - offset)
			toCopy = size - offset;
		memcpy(&out[offset], sector + sectorOff, toCopy);
		offset += toCopy;
	}
	return true;
}

// Try to read ICON0.PNG from a PSP ISO file (plain ISO only, not CSO)
static bool ISO_ReadIcon(const char *isoPath, std::vector<uint8_t> &outPNG) {
	// Check if it's a CSO (starts with "CISO") - we can't handle those here
	FILE *f = fopen(isoPath, "rb");
	if (!f)
		return false;

	uint8_t magic[4];
	if (fread(magic, 1, 4, f) != 4) { fclose(f); return false; }
	if (memcmp(magic, "CISO", 4) == 0) { fclose(f); return false; } // CSO not supported
	if (memcmp(magic, "\x00PBP", 4) == 0) { fclose(f); return false; } // PBP not supported

	// Read Primary Volume Descriptor (sector 16)
	uint8_t pvd[ISO_SECTOR_SIZE];
	if (!ISO_ReadSector(f, 16, pvd)) { fclose(f); return false; }

	// Verify it's a PVD (type=1, magic="CD001")
	if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) { fclose(f); return false; }

	// Root directory is at offset 156 in PVD (34-byte directory record)
	ISO9660_DirRec *rootRec = (ISO9660_DirRec *)(pvd + 156);
	uint32_t rootLBA = readLE32((const uint8_t *)&rootRec->extentLBA_LE);
	uint32_t rootSize = readLE32((const uint8_t *)&rootRec->extentSize_LE);

	// Find /PSP_GAME/ICON0.PNG
	uint32_t iconLBA, iconSize;
	if (!ISO_FindFile(f, rootLBA, rootSize, "/PSP_GAME/ICON0.PNG", iconLBA, iconSize)) {
		fclose(f);
		return false;
	}

	bool ok = ISO_ReadFile(f, iconLBA, iconSize, outPNG);
	fclose(f);
	return ok;
}

// Generic version to read any file from ISO by name
static bool ISO_ReadFileByName(const char *isoPath, const char *fileName, std::vector<uint8_t> &outData) {
	FILE *f = fopen(isoPath, "rb");
	if (!f) return false;

	uint8_t magic[4];
	if (fread(magic, 1, 4, f) != 4) { fclose(f); return false; }
	if (memcmp(magic, "CISO", 4) == 0) { fclose(f); return false; }
	if (memcmp(magic, "\x00PBP", 4) == 0) { fclose(f); return false; }

	uint8_t pvd[ISO_SECTOR_SIZE];
	if (!ISO_ReadSector(f, 16, pvd)) { fclose(f); return false; }
	if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) { fclose(f); return false; }

	ISO9660_DirRec *rootRec = (ISO9660_DirRec *)(pvd + 156);
	uint32_t rootLBA = readLE32((const uint8_t *)&rootRec->extentLBA_LE);
	uint32_t rootSize = readLE32((const uint8_t *)&rootRec->extentSize_LE);

	char searchPath[64];
	sprintf(searchPath, "/PSP_GAME/%s", fileName);
	uint32_t fileLBA, fileSize;
	if (!ISO_FindFile(f, rootLBA, rootSize, searchPath, fileLBA, fileSize)) {
		fclose(f);
		return false;
	}

	bool ok = ISO_ReadFile(f, fileLBA, fileSize, outData);
	fclose(f);
	return ok;
}

// Read game title from PARAM.SFO inside the ISO (handles both plain ISO and CSO)
static std::string ISO_ReadGameTitle(const char *isoPath) {
	BlockDevice *bd = constructBlockDevice(isoPath);
	if (!bd)
		return "";
	SequentialHandleAllocator alloc;
	ISOFileSystem isoFS(&alloc, bd);

	std::string sfoPath("PSP_GAME/PARAM.SFO");
	PSPFileInfo info = isoFS.GetFileInfo(sfoPath.c_str());
	if (!info.exists)
		return "";

	u8 *sfoData = new u8[(size_t)info.size];
	u32 fd = isoFS.OpenFile(sfoPath, FILEACCESS_READ);
	isoFS.ReadFile(fd, sfoData, info.size);
	isoFS.CloseFile(fd);

	ParamSFOData sfo;
	std::string title;
	if (sfo.ReadSFO(sfoData, (size_t)info.size)) {
		title = sfo.GetValueString("TITLE");
		if (!title.empty()) {
			std::string discId = sfo.GetValueString("DISC_ID");
			if (!discId.empty())
				title = discId + " - " + title;
		}
	}
	delete[] sfoData;
	return title;
}

// ---------------------------------------------------------------------------
// Vertex layout (screen-space float4 position + color)
// ---------------------------------------------------------------------------

struct LVertex {
	float x, y, z, w;
	DWORD color;
	float u, v;
};

static const D3DVERTEXELEMENT9 LVertexElements[] = {
	{ 0, 0,  D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
	{ 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
	{ 0, 20, D3DDECLTYPE_FLOAT2,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
	D3DDECL_END()
};

static const char *lVsCode =
	" struct VS_IN                                            "
	" {                                                        "
	"   float4 Pos   : POSITION;                               "
	"   float4 Color : COLOR0;                                 "
	"   float2 Tex   : TEXCOORD0;                              "
	" };                                                       "
	" struct VS_OUT                                            "
	" {                                                        "
	"   float4 ProjPos : POSITION;                             "
	"   float4 Color   : COLOR0;                                "
	"   float2 Tex     : TEXCOORD0;                             "
	" };                                                       "
	" VS_OUT main( VS_IN In )                                  "
	" {                                                        "
	"   VS_OUT Out;                                            "
	"   float2 screenSize = float2(1280.0, 720.0);             "
	"   float2 clip = ((In.Pos.xy + float2(0.0, 8.0)) / screenSize) * 2.0 - 1.0;    "
	"   clip.y = -clip.y;                                      "
	"   Out.ProjPos = float4(clip, 0.0, 1.0);                  "
	"   Out.Color = In.Color;                                  "
	"   Out.Tex = In.Tex;                                      "
	"   return Out;                                            "
	" }                                                        ";

static const char *lPsCode =
	" struct PS_IN                                             "
	" {                                                        "
	"   float4 Color : COLOR0;                                 "
	" };                                                       "
	" float4 main( PS_IN In ) : COLOR                          "
	" {                                                        "
	"   return In.Color;                                       "
	" }                                                        ";

static const char *lPsTexCode =
	" sampler s: register(s0);                             "
	" struct PS_IN                                         "
	" {                                                    "
	"   float4 Color : COLOR0;                              "
	"   float2 Tex   : TEXCOORD0;                          "
	" };                                                   "
	" float4 main( PS_IN In ) : COLOR                      "
	" {                                                    "
	"   float a = tex2D(s, In.Tex).a;                      "
	"   return float4(In.Color.rgb, In.Color.a * a);       "
	" }                                                    ";

static const char *lPsTexFullCode =
	" sampler s: register(s0);                             "
	" struct PS_IN                                         "
	" {                                                    "
	"   float4 Color : COLOR0;                              "
	"   float2 Tex   : TEXCOORD0;                          "
	" };                                                   "
	" float4 main( PS_IN In ) : COLOR                      "
	" {                                                    "
	"   return tex2D(s, In.Tex);                           "
	" }                                                    ";

// ---------------------------------------------------------------------------
// ID3DXFont-based text rendering (smooth TrueType fonts)
// Font heights chosen to approximate original pixel-font scales
// ---------------------------------------------------------------------------

static int FONT_H_LARGE = 20;  // recomputed after font packing
static int FONT_H_SMALL = 14;  // recomputed after font packing

// ---------------------------------------------------------------------------
// Layout constants (match PPSSPP Windows style)
// ---------------------------------------------------------------------------

struct Margins {
	int l, t, r, b;
	Margins() : l(0), t(0), r(0), b(0) {}
	Margins(int all) : l(all), t(all), r(all), b(all) {}
	Margins(int h, int v) : l(h), t(v), r(h), b(v) {}
	Margins(int l, int t, int r, int b) : l(l), t(t), r(r), b(b) {}
};

static const int SCREEN_W = 1280;
static const int SCREEN_H = 720;

// Safe-area margins for TV overscan (~5%)
static const int SAFE_X = 64;
static const int SAFE_Y = 36;

// Left column — game browser
static const int COL_LEFT_X = SAFE_X;
static const int COL_LEFT_W = 780;
static const int COL_LEFT_H = SCREEN_H - SAFE_Y * 2;
static const Margins LCOL_PAD(30, 10);

// Right column — detail panel
static const int COL_RIGHT_X = COL_LEFT_X + COL_LEFT_W;
static const int COL_RIGHT_W = SCREEN_W - SAFE_X - COL_RIGHT_X;
static const int COL_RIGHT_H = SCREEN_H - SAFE_Y * 2;
static const Margins RCOL_PAD(20, 15);
// RCOL_BORDER_W removed — no vertical divider

// Tab bar
static const int TAB_H = 64;
static const int TAB_PAD = 20;
static const int TAB_BORDER_H = 3;
static const int TAB_BADGE_OFF = 8;

// Game list (list mode)
static const int LIST_Y = TAB_H;
static const int LIST_H = SCREEN_H - TAB_H;
static const int LIST_ITEM_H = 60;
static const int LIST_ITEM_MARGIN = 8;
static const int LIST_PAD_LEFT = 30;
static const int LIST_TOP_PAD = 8;
static const int ICON_SIZE = 40;
static const int SCROLLBAR_W = 4;
static const int SCROLLBAR_GAP = 8;

// Grid mode
static const int GRID_CELL_W = 170;
static const int GRID_CELL_H = 100;
static const int GRID_PAD = 10;
static const int GRID_COLS = (COL_LEFT_W - 2 * GRID_PAD) / (GRID_CELL_W + GRID_PAD);
static const int GRID_MAX_VISIBLE = (LIST_H / (GRID_CELL_H + GRID_PAD));
static const Margins GRID_CELL_PAD(8, 8);
static const Margins GRID_SEL_PAD(2, 2);
static const int GRID_ICON_H = 64;
static const int GRID_TITLE_CHARS = (GRID_CELL_W - 8) / 8;

// Right panel — branding, stats, poster, buttons
static const int RP_BRAND_LOGO_SCALE = 1;
static const int RP_BRAND_SPACING = 12;
static const int RP_BRAND_TOP = 15;
static const int RP_VERSION_Y = 120;
static const int RP_STATS_TOP = 145;
static const int RP_STATS_LINE_H = 25;
static const int RP_DIVIDER_Y = 195;
static const int RP_DIVIDER_H = 1;
static const int RP_POSTER_TOP = 210;
static const int RP_POSTER_H = 150;
static const int RP_LIST_BTN_TOP = 395;
static const int RP_LIST_BTN_H = 80;
static const int RP_LIST_BTN_SPACING = 0;
static const int RP_HINT_RECT_H = 140;
static const int RP_HINT_PAD = 8;
static const int RP_HINT_LINE_H = 30;
static const int RP_BOTTOM_PAD = 15;

// Splash screen
static const float SPLASH_ICON_SCALE = 2.5f;
static const float SPLASH_LOGO_SCALE = 2.0f;
static const float SPLASH_SPACING = 20.0f;
static const int SPLASH_CENTER_Y_OFF = -40;
static const int SPLASH_LOADING_OFF = 80;

// Background animation
static const int BG_SYM_COUNT = 100;
static const float BG_SYM_SIZE = 24.0f;

// Font scales
static const int SCALE_LARGE = 3;
static const int SCALE_MED   = 2;
static const int SCALE_SMALL = 2;

// Colors (ARGB) - user-selected theme
// Color palette (0=darkest/transparent, 1=medium, 2=lightest)
static const DWORD COL_C0 = 0x60000000;   // semi-transparent black — inactive buttons, hint rect, shadows
static const DWORD COL_C1 = 0xFF3999BD;   // RGB(57,153,189)     — active tab, accent line, vertical border
static const DWORD COL_C2 = 0xFF4CC2ED;   // RGB(76,194,237)     — focused elements

static const DWORD COL_BG           = 0xFF181828;
// COL_RIGHT_BG removed — no background on right panel
static const DWORD COL_TAB_TEXT     = 0xFFFFFFFF;
static const DWORD COL_TITLE        = 0xFFFFFFFF;
static const DWORD COL_VERSION      = 0xFF888888;
static const DWORD COL_ITEM_TEXT    = 0xFFFFFFFF;
static const DWORD COL_ITEM_DIM     = 0xFF999999;
static const DWORD COL_DIVIDER      = 0x603999BD;  // RGB(57,153,189) transparent
static const DWORD COL_HINT         = 0xFFAAAAAA;
static const DWORD COL_SCROLLBAR    = 0x403999BD;  // RGB(57,153,189) very transparent
static const DWORD COL_SCROLL_THUMB = 0xFF4CC2ED;  // RGB(76,194,237)

// ---------------------------------------------------------------------------
// XboxLauncher
// ---------------------------------------------------------------------------

static bool CompareGames(const XboxLauncher::GameEntry &a, const XboxLauncher::GameEntry &b) {
	return _stricmp(a.filename.c_str(), b.filename.c_str()) < 0;
}

XboxLauncher::XboxLauncher()
	: active_(false)
	, wantsSettings_(false)
	, selectedIndex_(0)
	, scrollOffset_(0)
	, currentTab_(TAB_GAMES)
	, viewMode_(VIEW_GRID)
	, focusRegion_(FOCUS_GAMES)
	, rightPanelItem_(0)
	, prevButtons_(0)
	, prevRightTrigger_(0)
	, pollDelay_(0)
	, repeatDelay_(0)
	, vertexDecl_(NULL)
	, vertexShader_(NULL)
	, pixelShader_(NULL)
	, pixelShaderTex_(NULL)
	, pixelShaderTexFull_(NULL)
	, fontAtlasTex_(NULL)
	, fontChars_(NULL)
	, fontAscent_(0)
	, fontDescent_(0)
	, fontLoaded_(false)
	, unknownTex_(NULL)
	, splashTex_(NULL)
	, zimTex_(NULL)
	, scanDone_(false)
	, initFailed_(false)
	, startTime_(0)
	, inGameMenuActive_(false)
	, wantsExitToMenu_(false)
	, wantsExitToXbox_(false)
	, wantsResetGame_(false)
	, inGameMenuSel_(0)
	, inGameMenuFocus_(INGMENU_LEFT)
	, igmPrevButtons_(0)
	, igmRepeatDelay_(0)
	, toastEndTime_(0)
{
	toastMsg_[0] = 0;
	// Pre-compute pseudo-random positions for floating background symbols
	for (int i = 0; i < 100; i++) {
		symX_[i] = (float)((i * 139 + 211) % SCREEN_W);
		symY_[i] = (float)((i * 97 + 53) % (SCREEN_H + 80)) - 40.0f;
	}
}

XboxLauncher::~XboxLauncher() {
	Shutdown();
}

void XboxLauncher::Init() {
	active_ = true;
	wantsSettings_ = false;
	selectedIndex_ = 0;
	scrollOffset_ = 0;
	startTime_ = (float)real_time_now();
	prevButtons_ = 0;
	pollDelay_ = 0;
	repeatDelay_ = 0;
	selectedGame_.clear();
	recentGames_.clear();
	allGames_.clear();

	// Clear caches
	for (std::map<std::string, IDirect3DTexture9 *>::iterator it = iconCache_.begin();
	     it != iconCache_.end(); ++it) {
		if (it->second) it->second->Release();
	}
	iconCache_.clear();
	for (std::map<std::string, IDirect3DTexture9 *>::iterator it = posterCache_.begin();
	     it != posterCache_.end(); ++it) {
		if (it->second) it->second->Release();
	}
	posterCache_.clear();

	// Load search dirs from file, then scan later in Render()
	LoadSearchDirs();
	LoadRecent();
	scanDone_ = false;

	// Load assets (fast, no disk scan)
	unknownTex_ = LoadPNGTexture("game:\\assets\\unknown.png");
	splashTex_ = LoadPNGTexture("game:\\assets\\splash.png");
	LoadAtlas();

	currentTab_ = TAB_GAMES;
}

void XboxLauncher::Shutdown() {
	active_ = false;
	SaveRecent();
	SaveSearchDirs();
	// Unset device state before releasing shaders so the D3D device
	// doesn't hold dangling pointers that break game rendering and
	// the Xbox guide overlay compositor.
	pD3Ddevice->SetVertexShader(NULL);
	pD3Ddevice->SetPixelShader(NULL);
	pD3Ddevice->SetVertexDeclaration(NULL);
	if (vertexDecl_) { vertexDecl_->Release(); vertexDecl_ = NULL; }
	if (vertexShader_) { vertexShader_->Release(); vertexShader_ = NULL; }
	if (pixelShader_) { pixelShader_->Release(); pixelShader_ = NULL; }
	if (pixelShaderTex_) { pixelShaderTex_->Release(); pixelShaderTex_ = NULL; }
	if (pixelShaderTexFull_) { pixelShaderTexFull_->Release(); pixelShaderTexFull_ = NULL; }
	if (fontAtlasTex_) { fontAtlasTex_->Release(); fontAtlasTex_ = NULL; }
	if (fontChars_) { free(fontChars_); fontChars_ = NULL; }

	for (std::map<std::string, IDirect3DTexture9 *>::iterator it = iconCache_.begin();
	     it != iconCache_.end(); ++it) {
		if (it->second) it->second->Release();
	}
	iconCache_.clear();

	for (std::map<std::string, IDirect3DTexture9 *>::iterator it = posterCache_.begin();
	     it != posterCache_.end(); ++it) {
		if (it->second) it->second->Release();
	}
	posterCache_.clear();

	if (unknownTex_) { unknownTex_->Release(); unknownTex_ = NULL; }
	if (splashTex_) { splashTex_->Release(); splashTex_ = NULL; }
	if (zimTex_) { zimTex_->Release(); zimTex_ = NULL; }
	initFailed_ = false;
}

// ---------------------------------------------------------------------------
// Game scanning, recent files, directories
// ---------------------------------------------------------------------------

void XboxLauncher::ScanDirs() {
	allGames_.clear();
	iconCache_.clear();

	const char *exts[] = { "*.iso", "*.cso", "*.ISO", "*.CSO" };

	// If no saved dirs, scan game:\ root
	if (searchDirs_.empty())
		searchDirs_.push_back("game:\\");

	for (size_t d = 0; d < searchDirs_.size(); d++) {
		for (int e = 0; e < 4; e++) {
			WIN32_FIND_DATA fd;
			char search[MAX_PATH];
			sprintf(search, "%s%s", searchDirs_[d].c_str(), exts[e]);

			HANDLE hFind = FindFirstFile(search, &fd);
			if (hFind == INVALID_HANDLE_VALUE)
				continue;

			do {
				if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
					continue;

				char fullPath[MAX_PATH];
				sprintf(fullPath, "%s%s", searchDirs_[d].c_str(), fd.cFileName);

				GameEntry entry;
				entry.path = fullPath;
				entry.filename = fd.cFileName;
				entry.title = ISO_ReadGameTitle(fullPath);
				if (entry.title.empty())
					entry.title = fd.cFileName;

				bool dup = false;
				for (size_t i = 0; i < allGames_.size(); i++) {
					if (_stricmp(allGames_[i].path.c_str(), entry.path.c_str()) == 0) {
						dup = true;
						break;
					}
				}
				if (!dup)
					allGames_.push_back(entry);
			} while (FindNextFile(hFind, &fd));
			FindClose(hFind);
		}
	}

	std::sort(allGames_.begin(), allGames_.end(), CompareGames);

	// After scanning, switch to Recent tab if we have recent games
	if (!recentGames_.empty())
		currentTab_ = TAB_RECENT;
}

void XboxLauncher::SaveRecent() {
	FILE *f = fopen("game:\\recent.txt", "w");
	if (!f) return;
	for (size_t i = 0; i < recentGames_.size(); i++) {
		fprintf(f, "%s\n", recentGames_[i].path.c_str());
	}
	fclose(f);
}

void XboxLauncher::LoadRecent() {
	recentGames_.clear();

	FILE *f = fopen("game:\\recent.txt", "r");
	if (!f) return;

	char line[MAX_PATH];
	while (fgets(line, sizeof(line), f)) {
		int len = (int)strlen(line);
		while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
			line[--len] = '\0';
		if (len == 0) continue;

		DWORD attr = GetFileAttributes(line);
		if (attr == INVALID_FILE_ATTRIBUTES) continue;

		GameEntry entry;
		entry.path = line;
		const char *slash = strrchr(line, '\\');
		if (!slash) slash = strrchr(line, '/');
		entry.filename = slash ? slash + 1 : line;
		entry.title = ISO_ReadGameTitle(line);
		if (entry.title.empty())
			entry.title = entry.filename;

		bool dup = false;
		for (size_t i = 0; i < recentGames_.size(); i++) {
			if (_stricmp(recentGames_[i].path.c_str(), entry.path.c_str()) == 0) {
				dup = true; break;
			}
		}
		if (!dup)
			recentGames_.push_back(entry);
	}
	fclose(f);
}

void XboxLauncher::SaveSearchDirs() {
	FILE *f = fopen("game:\\searchdirs.txt", "w");
	if (!f) return;
	for (size_t i = 0; i < searchDirs_.size(); i++) {
		fprintf(f, "%s\n", searchDirs_[i].c_str());
	}
	fclose(f);
}

void XboxLauncher::LoadSearchDirs() {
	searchDirs_.clear();

	FILE *f = fopen("game:\\searchdirs.txt", "r");
	if (!f) return;

	char line[MAX_PATH];
	while (fgets(line, sizeof(line), f)) {
		int len = (int)strlen(line);
		while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
			line[--len] = '\0';
		if (len > 0)
			searchDirs_.push_back(line);
	}
	fclose(f);
}

void XboxLauncher::BrowseDirectories() {
	// Simple directory browser on game:\
	// Uses the same rendering path; stores selected dir and rescans
	char path[MAX_PATH] = "game:\\";

	// Try to find a subdirectory
	WIN32_FIND_DATA fd;
	sprintf(path, "game:\\*");
	HANDLE hFind = FindFirstFile(path, &fd);
	if (hFind == INVALID_HANDLE_VALUE)
		return;

	std::vector<std::string> dirs;
	do {
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
		    strcmp(fd.cFileName, ".") != 0 &&
		    strcmp(fd.cFileName, "..") != 0) {
			char dirPath[MAX_PATH];
			sprintf(dirPath, "game:\\%s\\", fd.cFileName);
			dirs.push_back(dirPath);
		}
	} while (FindNextFile(hFind, &fd));
	FindClose(hFind);

	if (dirs.empty())
		return;

	// Add first found directory to search dirs
	searchDirs_.push_back(dirs[0]);

	// Deduplicate
	std::sort(searchDirs_.begin(), searchDirs_.end());
	searchDirs_.erase(std::unique(searchDirs_.begin(), searchDirs_.end()), searchDirs_.end());

	SaveSearchDirs();
	scanDone_ = false; // trigger rescan next frame
}

// ---------------------------------------------------------------------------
// Icon loading from ISO/CSO (ICON0.PNG)
// ---------------------------------------------------------------------------

IDirect3DTexture9 *XboxLauncher::LoadGameIcon(const std::string &gamePath) {
	IDirect3DTexture9 *tex = NULL;

	// Read ICON0.PNG from ISO using our self-contained ISO9660 reader
	std::vector<uint8_t> pngData;
	if (!ISO_ReadIcon(gamePath.c_str(), pngData))
		return NULL;

	// Decode PNG to RGBA using stb_image
	int w = 0, h = 0;
	unsigned char *rgba = NULL;
	if (pngLoadPtr(&pngData[0], pngData.size(), &w, &h, &rgba, false) != 1)
		return NULL;
	if (!rgba || w <= 0 || h <= 0) {
		free(rgba);
		return NULL;
	}

	// Create DX9 texture
	if (SUCCEEDED(pD3Ddevice->CreateTexture(w, h, 1, 0,
		D3DFMT(D3DFMT_A8R8G8B8), D3DPOOL_MANAGED, &tex, NULL)))
	{
		D3DLOCKED_RECT rect;
		if (SUCCEEDED(tex->LockRect(0, &rect, NULL, 0))) {
			// Copy RGBA data swizzling for Xbox 360 big-endian D3DFMT_A8R8G8B8
			// stb_image: R,G,B,A bytes per pixel
			// D3D format A8R8G8B8 on BE: uint32 = 0xAARRGGBB stored as A,R,G,B
			uint8_t *src = rgba;
			uint8_t *dst = (uint8_t *)rect.pBits;
			int pitch = rect.Pitch;
			for (int y = 0; y < h; y++) {
				uint32_t *dstRow = (uint32_t *)(dst + y * pitch);
				uint8_t *srcRow = src + y * w * 4;
				for (int x = 0; x < w; x++) {
					uint8_t r = srcRow[x * 4 + 0];
					uint8_t g = srcRow[x * 4 + 1];
					uint8_t b = srcRow[x * 4 + 2];
					uint8_t a = srcRow[x * 4 + 3];
					dstRow[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
				}
			}
			tex->UnlockRect(0);
		}

	free(rgba);
	return tex;
}
}

IDirect3DTexture9 *XboxLauncher::GetOrCreateIcon(const std::string &gamePath) {
	std::map<std::string, IDirect3DTexture9 *>::iterator it = iconCache_.find(gamePath);
	if (it != iconCache_.end())
		return it->second;

	IDirect3DTexture9 *tex = LoadGameIcon(gamePath);
	if (!tex)
		tex = unknownTex_;
	iconCache_[gamePath] = tex;
	return tex;
}

IDirect3DTexture9 *XboxLauncher::LoadGamePoster(const std::string &gamePath) {
	IDirect3DTexture9 *tex = NULL;

	std::vector<uint8_t> pngData;
	if (!ISO_ReadFileByName(gamePath.c_str(), "PIC1.PNG", pngData))
		return NULL;

	int w = 0, h = 0;
	unsigned char *rgba = NULL;
	if (pngLoadPtr(&pngData[0], pngData.size(), &w, &h, &rgba, false) != 1)
		return NULL;
	if (!rgba || w <= 0 || h <= 0) {
		free(rgba);
		return NULL;
	}

	if (SUCCEEDED(pD3Ddevice->CreateTexture(w, h, 1, 0,
		D3DFMT(D3DFMT_A8R8G8B8), D3DPOOL_MANAGED, &tex, NULL)))
	{
		D3DLOCKED_RECT rect;
		if (SUCCEEDED(tex->LockRect(0, &rect, NULL, 0))) {
			uint8_t *src = rgba;
			uint8_t *dst = (uint8_t *)rect.pBits;
			int pitch = rect.Pitch;
			for (int y = 0; y < h; y++) {
				uint32_t *dstRow = (uint32_t *)(dst + y * pitch);
				uint8_t *srcRow = src + y * w * 4;
				for (int x = 0; x < w; x++) {
					uint8_t r = srcRow[x * 4 + 0];
					uint8_t g = srcRow[x * 4 + 1];
					uint8_t b = srcRow[x * 4 + 2];
					uint8_t a = srcRow[x * 4 + 3];
					dstRow[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
				}
			}
			tex->UnlockRect(0);
		}
	}
	free(rgba);
	return tex;
}

IDirect3DTexture9 *XboxLauncher::GetOrCreatePoster(const std::string &gamePath) {
	std::map<std::string, IDirect3DTexture9 *>::iterator it = posterCache_.find(gamePath);
	if (it != posterCache_.end())
		return it->second;

	IDirect3DTexture9 *tex = LoadGamePoster(gamePath);
	posterCache_[gamePath] = tex;
	return tex;
}

IDirect3DTexture9 *XboxLauncher::LoadPNGTexture(const char *path) {
	IDirect3DTexture9 *tex = NULL;

	int w = 0, h = 0;
	unsigned char *rgba = NULL;
	if (pngLoad((const char *)path, &w, &h, &rgba, false) != 1)
		return NULL;
	if (!rgba || w <= 0 || h <= 0) {
		free(rgba);
		return NULL;
	}

	if (SUCCEEDED(pD3Ddevice->CreateTexture(w, h, 1, 0,
		D3DFMT(D3DFMT_A8R8G8B8), D3DPOOL_MANAGED, &tex, NULL)))
	{
		D3DLOCKED_RECT rect;
		if (SUCCEEDED(tex->LockRect(0, &rect, NULL, 0))) {
			for (int y = 0; y < h; y++) {
				uint32_t *dstRow = (uint32_t *)((uint8_t *)rect.pBits + y * rect.Pitch);
				uint8_t *srcRow = rgba + y * w * 4;
				for (int x = 0; x < w; x++) {
					uint8_t r = srcRow[x * 4 + 0];
					uint8_t g = srcRow[x * 4 + 1];
					uint8_t b = srcRow[x * 4 + 2];
					uint8_t a = srcRow[x * 4 + 3];
					dstRow[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
				}
			}
			tex->UnlockRect(0);
		}
		free(rgba);
		return tex;
	}
}

void XboxLauncher::LoadAtlas() {
	if (zimTex_) return;

	int w = 0, h = 0;
	int flags = 0;
	unsigned char *img = NULL;
	if (LoadZIM("ui_atlas.zim", &w, &h, &flags, &img) <= 0)
		return;

	if (SUCCEEDED(pD3Ddevice->CreateTexture(w, h, 1, 0,
		D3DFMT(D3DFMT_A8R8G8B8), D3DPOOL_MANAGED, &zimTex_, NULL)))
	{
		D3DLOCKED_RECT rect;
		if (SUCCEEDED(zimTex_->LockRect(0, &rect, NULL, 0))) {
			uint8_t *dst = (uint8_t *)rect.pBits;
			int pitch = rect.Pitch;
			int fmt = flags & 0xF;
			for (int y = 0; y < h; y++) {
				uint32_t *dstRow = (uint32_t *)(dst + y * pitch);
				for (int x = 0; x < w; x++) {
					int srcOffset = fmt == ZIM_RGB888 ? y * w * 3 + x * 3 : y * w * 4 + x * 4;
					uint8_t r, g, b, a;
					switch (fmt) {
					case ZIM_RGBA8888:
						r = img[srcOffset + 0];
						g = img[srcOffset + 1];
						b = img[srcOffset + 2];
						a = img[srcOffset + 3];
						break;
					case ZIM_RGB565: {
						uint16_t p = *(uint16_t *)(img + y * w * 2 + x * 2);
						r = ((p >> 11) & 0x1F) << 3;
						g = ((p >> 5) & 0x3F) << 2;
						b = (p & 0x1F) << 3;
						a = 255;
						break;
					}
					case ZIM_RGBA4444: {
						uint16_t p = *(uint16_t *)(img + y * w * 2 + x * 2);
						r = ((p >> 12) & 0xF) << 4;
						g = ((p >> 8) & 0xF) << 4;
						b = ((p >> 4) & 0xF) << 4;
						a = (p & 0xF) << 4;
						break;
					}
					case ZIM_RGB888:
						r = img[srcOffset + 0];
						g = img[srcOffset + 1];
						b = img[srcOffset + 2];
						a = 255;
						break;
					default:
						r = g = b = a = 255;
						break;
					}
					dstRow[x] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
				}
			}
		}
		zimTex_->UnlockRect(0);
	}
	free(img);
}

void XboxLauncher::DrawTextureAtlas(IDirect3DTexture9 *atlasTex, const AtlasImage &img, float x, float y, float w, float h) {
	if (!atlasTex) return;

	pD3Ddevice->SetVertexDeclaration(vertexDecl_);
	pD3Ddevice->SetVertexShader(vertexShader_);
	pD3Ddevice->SetPixelShader(pixelShaderTexFull_ ? pixelShaderTexFull_ : pixelShader_);
	pD3Ddevice->SetTexture(0, atlasTex);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	pD3Ddevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	pD3Ddevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	pD3Ddevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	pD3Ddevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	pD3Ddevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
	pD3Ddevice->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);

	LVertex v[6];
	v[0].x=x;   v[0].y=y;   v[0].z=0; v[0].w=1; v[0].color=0xFFFFFFFF; v[0].u=img.u1; v[0].v=img.v1;
	v[1].x=x+w; v[1].y=y;   v[1].z=0; v[1].w=1; v[1].color=0xFFFFFFFF; v[1].u=img.u2; v[1].v=img.v1;
	v[2].x=x;   v[2].y=y+h; v[2].z=0; v[2].w=1; v[2].color=0xFFFFFFFF; v[2].u=img.u1; v[2].v=img.v2;
	v[3].x=x+w; v[3].y=y;   v[3].z=0; v[3].w=1; v[3].color=0xFFFFFFFF; v[3].u=img.u2; v[3].v=img.v1;
	v[4].x=x+w; v[4].y=y+h; v[4].z=0; v[4].w=1; v[4].color=0xFFFFFFFF; v[4].u=img.u2; v[4].v=img.v2;
	v[5].x=x;   v[5].y=y+h; v[5].z=0; v[5].w=1; v[5].color=0xFFFFFFFF; v[5].u=img.u1; v[5].v=img.v2;
	pD3Ddevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(LVertex));

	pD3Ddevice->SetTexture(0, NULL);
	pD3Ddevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

void XboxLauncher::DrawTextureAtlasRotated(IDirect3DTexture9 *atlasTex, const AtlasImage &img, float x, float y, float w, float h, float angle, DWORD color) {
	if (!atlasTex) return;

	pD3Ddevice->SetVertexDeclaration(vertexDecl_);
	pD3Ddevice->SetVertexShader(vertexShader_);
	pD3Ddevice->SetPixelShader(pixelShaderTex_ ? pixelShaderTex_ : pixelShader_);
	pD3Ddevice->SetTexture(0, atlasTex);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	pD3Ddevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	pD3Ddevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	pD3Ddevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	pD3Ddevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	pD3Ddevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
	pD3Ddevice->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);

	float cx = x + w * 0.5f;
	float cy = y + h * 0.5f;
	float cosA = (float)cos(angle);
	float sinA = (float)sin(angle);
	float hw = w * 0.5f;
	float hh = h * 0.5f;

	LVertex v[6];
	float x0 = -hw, y0 = -hh;
	float x1 =  hw, y1 = -hh;
	float x2 = -hw, y2 =  hh;
	float x3 =  hw, y3 =  hh;

	v[0].x = cx + x0*cosA - y0*sinA; v[0].y = cy + x0*sinA + y0*cosA;
	v[0].z = 0; v[0].w = 1; v[0].color = color; v[0].u = img.u1; v[0].v = img.v1;

	v[1].x = cx + x1*cosA - y1*sinA; v[1].y = cy + x1*sinA + y1*cosA;
	v[1].z = 0; v[1].w = 1; v[1].color = color; v[1].u = img.u2; v[1].v = img.v1;

	v[2].x = cx + x2*cosA - y2*sinA; v[2].y = cy + x2*sinA + y2*cosA;
	v[2].z = 0; v[2].w = 1; v[2].color = color; v[2].u = img.u1; v[2].v = img.v2;

	v[3] = v[1];

	v[4].x = cx + x3*cosA - y3*sinA; v[4].y = cy + x3*sinA + y3*cosA;
	v[4].z = 0; v[4].w = 1; v[4].color = color; v[4].u = img.u2; v[4].v = img.v2;

	v[5] = v[2];

	pD3Ddevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(LVertex));

	pD3Ddevice->SetTexture(0, NULL);
	pD3Ddevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

void XboxLauncher::DrawTexture(IDirect3DTexture9 *tex, float x, float y, float w, float h) {
	if (!tex)
		return;

	pD3Ddevice->SetVertexDeclaration(vertexDecl_);
	pD3Ddevice->SetVertexShader(vertexShader_);
	pD3Ddevice->SetPixelShader(pixelShaderTexFull_ ? pixelShaderTexFull_ : pixelShader_);
	pD3Ddevice->SetTexture(0, tex);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	pD3Ddevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	pD3Ddevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	pD3Ddevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	pD3Ddevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	pD3Ddevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);

	LVertex v[6];
	v[0].x=x;   v[0].y=y;   v[0].z=0; v[0].w=1; v[0].color=0xFFFFFFFF; v[0].u=0; v[0].v=0;
	v[1].x=x+w; v[1].y=y;   v[1].z=0; v[1].w=1; v[1].color=0xFFFFFFFF; v[1].u=1; v[1].v=0;
	v[2].x=x;   v[2].y=y+h; v[2].z=0; v[2].w=1; v[2].color=0xFFFFFFFF; v[2].u=0; v[2].v=1;
	v[3].x=x+w; v[3].y=y;   v[3].z=0; v[3].w=1; v[3].color=0xFFFFFFFF; v[3].u=1; v[3].v=0;
	v[4].x=x+w; v[4].y=y+h; v[4].z=0; v[4].w=1; v[4].color=0xFFFFFFFF; v[4].u=1; v[4].v=1;
	v[5].x=x;   v[5].y=y+h; v[5].z=0; v[5].w=1; v[5].color=0xFFFFFFFF; v[5].u=0; v[5].v=1;
	pD3Ddevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(LVertex));

	pD3Ddevice->SetTexture(0, NULL);
	pD3Ddevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

// ---------------------------------------------------------------------------
// 4-grid shadow (9-slice from atlas shadow image)
// ---------------------------------------------------------------------------
void XboxLauncher::DrawShadow4Grid(IDirect3DTexture9 *atlasTex, const AtlasImage &img, float x1, float y1, float x2, float y2, DWORD color, float cornerScale) {
	if (!atlasTex) return;

	float u1 = img.u1, v1 = img.v1, u2 = img.u2, v2 = img.v2;
	float um = (u1 + u2) * 0.5f;
	float vm = (v1 + v2) * 0.5f;
	float iw2 = (float)img.w * 0.5f * cornerScale;
	float ih2 = (float)img.h * 0.5f * cornerScale;
	float xa = x1 + iw2;
	float xb = x2 - iw2;
	float ya = y1 + ih2;
	float yb = y2 - ih2;

	pD3Ddevice->SetVertexDeclaration(vertexDecl_);
	pD3Ddevice->SetVertexShader(vertexShader_);
	pD3Ddevice->SetPixelShader(pixelShaderTex_ ? pixelShaderTex_ : pixelShader_);
	pD3Ddevice->SetTexture(0, atlasTex);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	pD3Ddevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	pD3Ddevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	pD3Ddevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	pD3Ddevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	pD3Ddevice->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
	pD3Ddevice->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);

	// Helper to add a quad (2 triangles, 6 vertices) to the batch
	struct { float l, r, t, b, uL, uR, vT, vB; } slices[9] = {
		// Top row: left corner, top edge, right corner
		{x1, xa, y1, ya, u1, um, v1, vm},
		{xa, xb, y1, ya, um, um, v1, vm},
		{xb, x2, y1, ya, um, u2, v1, vm},
		// Middle row: left edge, center, right edge
		{x1, xa, ya, yb, u1, um, vm, vm},
		{xa, xb, ya, yb, um, um, vm, vm},
		{xb, x2, ya, yb, um, u2, vm, vm},
		// Bottom row: left corner, bottom edge, right corner
		{x1, xa, yb, y2, u1, um, vm, v2},
		{xa, xb, yb, y2, um, um, vm, v2},
		{xb, x2, yb, y2, um, u2, vm, v2},
	};

	LVertex verts[54];
	int vi = 0;
	for (int i = 0; i < 9; i++) {
		float l = slices[i].l, r = slices[i].r;
		float t = slices[i].t, b = slices[i].b;
		float uL = slices[i].uL, uR = slices[i].uR;
		float vT = slices[i].vT, vB = slices[i].vB;

		LVertex v[6] = {
			{l, t, 0, 1, color, uL, vT},
			{r, t, 0, 1, color, uR, vT},
			{l, b, 0, 1, color, uL, vB},
			{r, t, 0, 1, color, uR, vT},
			{r, b, 0, 1, color, uR, vB},
			{l, b, 0, 1, color, uL, vB},
		};
		memcpy(&verts[vi], v, sizeof(v));
		vi += 6;
	}
	pD3Ddevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 18, verts, sizeof(LVertex));

	pD3Ddevice->SetTexture(0, NULL);
	pD3Ddevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

// ---------------------------------------------------------------------------
// XInput
// ---------------------------------------------------------------------------

static const float DEADZONE = 0.3f;

void XboxLauncher::Update() {
	if (!active_)
		return;

	pollDelay_++;
	if (pollDelay_ < 2) return;
	pollDelay_ = 0;

	XINPUT_STATE state;
	ZeroMemory(&state, sizeof(state));
	if (XInputGetState(0, &state) != ERROR_SUCCESS)
		return;

	DWORD buttons = state.Gamepad.wButtons;
	DWORD pressed = (buttons ^ prevButtons_) & buttons;
	prevButtons_ = buttons;

	int count = (currentTab_ == TAB_RECENT) ? (int)recentGames_.size() : (int)allGames_.size();

	// D-pad navigation with repeat
	bool up = false, down = false, left = false, right = false;
	if (pressed & XINPUT_GAMEPAD_DPAD_UP) { up = true; repeatDelay_ = 0; }
	if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) { down = true; repeatDelay_ = 0; }
	if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) { left = true; repeatDelay_ = 0; }
	if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) { right = true; repeatDelay_ = 0; }

	// Analog stick
	float stickY = state.Gamepad.sThumbLY / 32767.0f;
	float stickX = state.Gamepad.sThumbLX / 32767.0f;
	if (stickY > DEADZONE) { up = true; repeatDelay_ = 0; }
	else if (stickY < -DEADZONE) { down = true; repeatDelay_ = 0; }
	if (stickX > DEADZONE) { right = true; repeatDelay_ = 0; }
	else if (stickX < -DEADZONE) { left = true; repeatDelay_ = 0; }

	// Repeat
	if (up || down || left || right) {
		// first press handled above
	} else if ((buttons & (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT)) ||
	           (stickY > DEADZONE || stickY < -DEADZONE || stickX > DEADZONE || stickX < -DEADZONE)) {
		repeatDelay_++;
		if (repeatDelay_ > 15) {
			if (stickY > DEADZONE || (buttons & XINPUT_GAMEPAD_DPAD_UP))
				up = true;
			else if (stickY < -DEADZONE || (buttons & XINPUT_GAMEPAD_DPAD_DOWN))
				down = true;
			if (stickX > DEADZONE || (buttons & XINPUT_GAMEPAD_DPAD_RIGHT))
				right = true;
			else if (stickX < -DEADZONE || (buttons & XINPUT_GAMEPAD_DPAD_LEFT))
				left = true;
		}
	} else {
		repeatDelay_ = 0;
	}

	// Focus-based navigation
	switch (focusRegion_) {
	case FOCUS_TABS:
		if (left && currentTab_ == TAB_GAMES) currentTab_ = TAB_RECENT;
		if (right && currentTab_ == TAB_RECENT) currentTab_ = TAB_GAMES;
		if (down) { focusRegion_ = FOCUS_GAMES; selectedIndex_ = 0; scrollOffset_ = 0; }
		break;

	case FOCUS_GAMES:
		// If no games, redirect to tabs
		if (count <= 0) {
			focusRegion_ = FOCUS_TABS;
			break;
		}
		if (right) {
			if (viewMode_ == VIEW_GRID && (selectedIndex_ % GRID_COLS) < GRID_COLS - 1 && selectedIndex_ < count - 1) {
				selectedIndex_++;
			} else {
				focusRegion_ = FOCUS_RIGHT_PANEL;
				rightPanelItem_ = 0;
			}
		}
		if (left) {
			if (viewMode_ == VIEW_GRID && (selectedIndex_ % GRID_COLS) > 0) {
				selectedIndex_--;
			}
		}
		if (up) {
			if (viewMode_ == VIEW_GRID) {
				if (selectedIndex_ >= GRID_COLS)
					selectedIndex_ -= GRID_COLS;
				else
					focusRegion_ = FOCUS_TABS;
			} else {
				if (selectedIndex_ > 0)
					selectedIndex_--;
				else
					focusRegion_ = FOCUS_TABS;
			}
		}
		if (down) {
			if (viewMode_ == VIEW_GRID) {
				if (selectedIndex_ < count - GRID_COLS)
					selectedIndex_ += GRID_COLS;
			} else {
				if (selectedIndex_ < count - 1)
					selectedIndex_++;
			}
		}
		// Clamp
		if (selectedIndex_ < 0) selectedIndex_ = 0;
		if (selectedIndex_ >= count) selectedIndex_ = count - 1;
		// Keep selection in view
		if (viewMode_ == VIEW_LIST) {
			int maxVisible = LIST_H / LIST_ITEM_H;
			if (selectedIndex_ < scrollOffset_) scrollOffset_ = selectedIndex_;
			if (selectedIndex_ >= scrollOffset_ + maxVisible) scrollOffset_ = selectedIndex_ - maxVisible + 1;
		} else {
			int visibleRows = GRID_MAX_VISIBLE;
			int selRow = selectedIndex_ / GRID_COLS;
			if (selRow < scrollOffset_) scrollOffset_ = selRow;
			if (selRow >= scrollOffset_ + visibleRows) scrollOffset_ = selRow - visibleRows + 1;
		}
		break;

	case FOCUS_RIGHT_PANEL:
		if (left) focusRegion_ = FOCUS_GAMES;
		if (up && rightPanelItem_ > 0) rightPanelItem_--;
		if (down && rightPanelItem_ < 1) rightPanelItem_++;
		if (pressed & XINPUT_GAMEPAD_A) {
			if (rightPanelItem_ == 1) { selectedGame_ = ""; active_ = false; } // Exit
		}
		break;
	}

	// LB/RB = Switch Tab (matches hint rect)
	if ((pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) || (pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER)) {
		currentTab_ = (currentTab_ == TAB_RECENT) ? TAB_GAMES : TAB_RECENT;
		selectedIndex_ = 0;
		scrollOffset_ = 0;
		focusRegion_ = FOCUS_GAMES;
	}

	// RT = Grid/List toggle (matches hint rect)
	{
		BYTE rt = state.Gamepad.bRightTrigger;
		bool rtPressed = (rt > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) && !(prevRightTrigger_ > XINPUT_GAMEPAD_TRIGGER_THRESHOLD);
		prevRightTrigger_ = rt;
		if (rtPressed && currentTab_ == TAB_GAMES) {
			viewMode_ = (viewMode_ == VIEW_LIST) ? VIEW_GRID : VIEW_LIST;
		}
	}

	// A button = launch game (only in FOCUS_GAMES)
	if ((pressed & XINPUT_GAMEPAD_A) && focusRegion_ == FOCUS_GAMES) {
		if (count > 0 && selectedIndex_ >= 0 && selectedIndex_ < count) {
			const std::string &path = (currentTab_ == TAB_RECENT)
				? recentGames_[selectedIndex_].path
				: allGames_[selectedIndex_].path;
			selectedGame_ = path;

			GameEntry entry;
			entry.path = path;
			const char *slash = strrchr(path.c_str(), '\\');
			if (!slash) slash = strrchr(path.c_str(), '/');
			entry.filename = slash ? slash + 1 : path.c_str();
			entry.title = ISO_ReadGameTitle(path.c_str());
			if (entry.title.empty())
				entry.title = entry.filename;

			for (size_t i = 0; i < recentGames_.size(); i++) {
				if (_stricmp(recentGames_[i].path.c_str(), path.c_str()) == 0) {
					recentGames_.erase(recentGames_.begin() + i);
					break;
				}
			}
			recentGames_.insert(recentGames_.begin(), entry);
			SaveRecent();

			active_ = false;
		}
	}

	// B button = exit launcher (global)
	if (pressed & XINPUT_GAMEPAD_B) {
		selectedGame_ = "";
		active_ = false;
	}

	// Back button = add directory and rescan
	if (pressed & XINPUT_GAMEPAD_BACK) {
		BrowseDirectories();
	}
}

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

void XboxLauncher::EnsureResources() {
	if (vertexDecl_ && vertexShader_ && pixelShader_)
		return;
	if (initFailed_)
		return;

	if (FAILED(pD3Ddevice->CreateVertexDeclaration(LVertexElements, &vertexDecl_))) {
		initFailed_ = true;
		return;
	}

	LPD3DXCONSTANTTABLE table = NULL;
	if (!CompileVertexShader(lVsCode, &vertexShader_, &table)) {
		initFailed_ = true;
		return;
	}
	table = NULL;
	if (!CompilePixelShader(lPsCode, &pixelShader_, &table)) {
		initFailed_ = true;
		return;
	}
	table = NULL;
	if (!CompilePixelShader(lPsTexCode, &pixelShaderTex_, &table)) {
		pixelShaderTex_ = NULL;
	}
	table = NULL;
	if (!CompilePixelShader(lPsTexFullCode, &pixelShaderTexFull_, &table)) {
		pixelShaderTexFull_ = NULL;
	}

	if (!fontAtlasTex_) {
		const float fontSize = 20.0f;
		const int atlasW = 512;
		const int atlasH = 512;

		FILE *f = fopen("game:\\assets\\Roboto-Condensed.ttf", "rb");
		if (f) {
			fseek(f, 0, SEEK_END);
			long ttfSize = ftell(f);
			fseek(f, 0, SEEK_SET);
			unsigned char *ttfData = (unsigned char *)malloc(ttfSize);
			if (ttfData) {
				if ((int)fread(ttfData, 1, ttfSize, f) == ttfSize) {
					stbtt_pack_context pc;
					unsigned char *atlasPixels = (unsigned char *)calloc(atlasW * atlasH, 1);
					stbtt_packedchar *chars = (stbtt_packedchar *)calloc(96, sizeof(stbtt_packedchar));

					stbtt_PackBegin(&pc, atlasPixels, atlasW, atlasH, 0, 1, NULL);
					stbtt_PackSetOversampling(&pc, 2, 2);
					stbtt_PackFontRange(&pc, ttfData, 0, fontSize, 32, 96, chars);
					stbtt_PackEnd(&pc);

					float ascent = 0, descent = 0;
					for (int i = 0; i < 96; i++) {
						float a = -chars[i].yoff;
						float d = chars[i].yoff2;
						if (a > ascent) ascent = a;
						if (d > descent) descent = d;
					}
					fontAscent_ = ascent;
					fontDescent_ = descent;
					FONT_H_LARGE = (int)((ascent + descent) * SCALE_LARGE);
					FONT_H_SMALL = (int)((ascent + descent) * SCALE_SMALL);

				if (SUCCEEDED(pD3Ddevice->CreateTexture(atlasW, atlasH, 1, 0,
					D3DFMT(D3DFMT_A8R8G8B8), D3DPOOL_MANAGED, &fontAtlasTex_, NULL)))
					{
						D3DLOCKED_RECT rect;
						if (SUCCEEDED(fontAtlasTex_->LockRect(0, &rect, NULL, 0))) {
							for (int y = 0; y < atlasH; y++) {
								uint32_t *dstRow = (uint32_t *)((uint8_t *)rect.pBits + y * rect.Pitch);
								uint8_t *srcRow = atlasPixels + y * atlasW;
								for (int x = 0; x < atlasW; x++) {
									uint8_t a = srcRow[x];
									dstRow[x] = ((uint32_t)a << 24) | ((uint32_t)0xFF << 16) | ((uint32_t)0xFF << 8) | (uint32_t)0xFF;
								}
							}
							fontAtlasTex_->UnlockRect(0);
						}
					}

					fontChars_ = chars;
					free(atlasPixels);
				}
				free(ttfData);
			}
			fclose(f);
		}
	}
}

void XboxLauncher::DrawRect(float x0, float y0, float x1, float y1, DWORD color) {
	if (vertexDecl_) {
		pD3Ddevice->SetVertexDeclaration(vertexDecl_);
		pD3Ddevice->SetVertexShader(vertexShader_);
		pD3Ddevice->SetPixelShader(pixelShader_);
		pD3Ddevice->SetTexture(0, NULL);
	}
	pD3Ddevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	pD3Ddevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	pD3Ddevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

	LVertex v[6];
	v[0].x=x0; v[0].y=y0; v[0].z=0; v[0].w=1; v[0].color=color; v[0].u=0; v[0].v=0;
	v[1].x=x1; v[1].y=y0; v[1].z=0; v[1].w=1; v[1].color=color; v[1].u=0; v[1].v=0;
	v[2].x=x0; v[2].y=y1; v[2].z=0; v[2].w=1; v[2].color=color; v[2].u=0; v[2].v=0;
	v[3].x=x1; v[3].y=y0; v[3].z=0; v[3].w=1; v[3].color=color; v[3].u=0; v[3].v=0;
	v[4].x=x1; v[4].y=y1; v[4].z=0; v[4].w=1; v[4].color=color; v[4].u=0; v[4].v=0;
	v[5].x=x0; v[5].y=y1; v[5].z=0; v[5].w=1; v[5].color=color; v[5].u=0; v[5].v=0;
	pD3Ddevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, v, sizeof(LVertex));

	pD3Ddevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

void XboxLauncher::DrawString(const char *str, int screenX, int screenY, int scale, DWORD color) {
	if (!fontAtlasTex_ || !fontChars_ || !pixelShaderTex_ || !str || !str[0])
		return;

	pD3Ddevice->SetVertexDeclaration(vertexDecl_);
	pD3Ddevice->SetVertexShader(vertexShader_);
	pD3Ddevice->SetPixelShader(pixelShaderTex_);
	pD3Ddevice->SetTexture(0, fontAtlasTex_);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	pD3Ddevice->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	pD3Ddevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	pD3Ddevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	pD3Ddevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

	const float atlasW = 512.0f;
	const float atlasH = 512.0f;
	float x = (float)screenX;
	float baseline = (float)screenY + fontAscent_ * (float)scale;

	LVertex verts[256 * 6];
	int vertCount = 0;
	int maxVerts = 256 * 6;

	const char *p = str;
	while (*p && vertCount + 6 <= maxVerts) {
		int ch = (unsigned char)*p;
		if (ch >= 32 && ch < 128) {
			const stbtt_packedchar &bc = fontChars_[ch - 32];

			float cx = x + bc.xoff * (float)scale;
			float cy = baseline + bc.yoff * (float)scale;
			float cw = (bc.xoff2 - bc.xoff) * (float)scale;
			float ch2 = (bc.yoff2 - bc.yoff) * (float)scale;

			float u0 = (float)bc.x0 / atlasW;
			float v0 = (float)bc.y0 / atlasH;
			float u1 = (float)bc.x1 / atlasW;
			float v1 = (float)bc.y1 / atlasH;

			verts[vertCount+0].x=cx;     verts[vertCount+0].y=cy;     verts[vertCount+0].z=0; verts[vertCount+0].w=1; verts[vertCount+0].color=color; verts[vertCount+0].u=u0; verts[vertCount+0].v=v0;
			verts[vertCount+1].x=cx+cw;  verts[vertCount+1].y=cy;     verts[vertCount+1].z=0; verts[vertCount+1].w=1; verts[vertCount+1].color=color; verts[vertCount+1].u=u1; verts[vertCount+1].v=v0;
			verts[vertCount+2].x=cx;     verts[vertCount+2].y=cy+ch2; verts[vertCount+2].z=0; verts[vertCount+2].w=1; verts[vertCount+2].color=color; verts[vertCount+2].u=u0; verts[vertCount+2].v=v1;
			verts[vertCount+3].x=cx+cw;  verts[vertCount+3].y=cy;     verts[vertCount+3].z=0; verts[vertCount+3].w=1; verts[vertCount+3].color=color; verts[vertCount+3].u=u1; verts[vertCount+3].v=v0;
			verts[vertCount+4].x=cx+cw;  verts[vertCount+4].y=cy+ch2; verts[vertCount+4].z=0; verts[vertCount+4].w=1; verts[vertCount+4].color=color; verts[vertCount+4].u=u1; verts[vertCount+4].v=v1;
			verts[vertCount+5].x=cx;     verts[vertCount+5].y=cy+ch2; verts[vertCount+5].z=0; verts[vertCount+5].w=1; verts[vertCount+5].color=color; verts[vertCount+5].u=u0; verts[vertCount+5].v=v1;
			vertCount += 6;
			x += bc.xadvance * (float)scale;
		} else {
			x += 10.0f * (float)scale;
		}
		p++;
	}

	if (vertCount > 0)
		pD3Ddevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, vertCount / 3, verts, sizeof(LVertex));
}

void XboxLauncher::DrawStringCentered(const char *str, int screenY, int scale, DWORD color) {
	int w = StringWidth(str, scale);
	int x = (SCREEN_W - w) / 2;
	DrawString(str, x, screenY, scale, color);
}

int XboxLauncher::StringWidth(const char *str, int scale) {
	if (!fontChars_ || !str || !str[0])
		return 0;

	float w = 0;
	const char *p = str;
	while (*p) {
		int ch = (unsigned char)*p;
		if (ch >= 32 && ch < 128) {
			w += fontChars_[ch - 32].xadvance;
		}
		p++;
	}
	return (int)(w * (float)scale);
}

// ---------------------------------------------------------------------------
// Tab bar
// ---------------------------------------------------------------------------

void XboxLauncher::RenderTabBar() {
	float x0 = (float)COL_LEFT_X;
	float x1 = (float)(COL_LEFT_X + COL_LEFT_W);
	float barTop = 0;
	float barBot = (float)TAB_H;

	const char *tabNames[TAB_COUNT] = { "Recent", "Games" };
	float x = (float)(COL_LEFT_X + TAB_PAD);
	float tabTextPad = 15.0f;
	for (int t = 0; t < TAB_COUNT; t++) {
		float tw = (float)(StringWidth(tabNames[t], SCALE_LARGE) + 30);
		int count = (t == TAB_RECENT) ? (int)recentGames_.size() : (int)allGames_.size();

		char badge[16];
		sprintf(badge, "%d", count);
		float badgeW = (float)StringWidth(badge, SCALE_SMALL);
		float totalW = tw + (float)TAB_BADGE_OFF + badgeW;

		if (t == currentTab_) {
			DWORD tabBg = (focusRegion_ == FOCUS_TABS) ? COL_C2 : COL_C1;
			DrawRect(x, barTop, x + totalW, barBot, tabBg);
		} else {
			DrawRect(x, barTop, x + totalW, barBot, COL_C0);
		}

		DrawString(tabNames[t], (int)(x + tabTextPad), (int)((TAB_H - FONT_H_LARGE) / 2), SCALE_LARGE,
			t == currentTab_ ? COL_TAB_TEXT : COL_ITEM_DIM);

		DrawString(badge, (int)(x + tw + (float)TAB_BADGE_OFF), (int)((TAB_H - FONT_H_SMALL) / 2), SCALE_SMALL, COL_HINT);

		x += totalW;
	}

	// Bottom accent line from first tab button → right panel edge
	DrawRect((float)(COL_LEFT_X + TAB_PAD), barBot - (float)TAB_BORDER_H, (float)COL_RIGHT_X, barBot, COL_C1);

	// View mode indicator with atlas icon (right side of tab bar)
	if (currentTab_ == TAB_GAMES && zimTex_) {
		int iconId = (viewMode_ == VIEW_LIST) ? I_GRID : I_LINES;
		const AtlasImage &modeIcon = ui_atlas.images[iconId];
		float iconW = 37.0f;
		float iconH = 26.0f;
		float mx = x1 - iconW - (float)TAB_PAD;
		float my = (barBot - iconH) / 2.0f;
		DrawRect(mx - 4, barTop + 4, x1 - (float)TAB_PAD + 4, barBot - 4, COL_C0);
		DrawTextureAtlas(zimTex_, modeIcon, mx, my, iconW, iconH);
	}
}

// ---------------------------------------------------------------------------
// Game list (list mode)
// ---------------------------------------------------------------------------

void XboxLauncher::RenderListItem(int gameIdx, float y, bool isSelected) {
	const GameEntry &entry = (currentTab_ == TAB_RECENT)
		? recentGames_[gameIdx] : allGames_[gameIdx];

	float x0 = (float)(COL_LEFT_X + LIST_PAD_LEFT);
	float x1 = (float)(COL_LEFT_X + COL_LEFT_W - 10);
	float margin = (float)LIST_ITEM_MARGIN;
	float h = (float)LIST_ITEM_H - margin;
	y += margin * 0.5f;

	// Item background
	DrawRect(x0 - 5, y, x1, y + h, isSelected ? COL_C2 : COL_C0);

	// Try to load game cover (PIC1.PNG poster)
	IDirect3DTexture9 *cover = GetOrCreatePoster(entry.path);
	if (!cover)
		cover = GetOrCreateIcon(entry.path);
	float textX = x0 + 8;

		if (cover) {
		D3DSURFACE_DESC desc;
		cover->GetLevelDesc(0, &desc);
		float texW = (float)desc.Width;
		float texH = (float)desc.Height;
		if (texW < 1) texW = 1;
		if (texH < 1) texH = 1;

		float coverH = h;
		float coverW = coverH * (texW / texH);
		if (coverW < 1) coverW = 1;
		if (coverH < 1) coverH = 1;

		float coverX = x0 + 4;
		float coverY = y;

		DrawTexture(cover, coverX, coverY, coverW, coverH);

		textX = coverX + coverW + 12;
	}

	// Game title
	float textY = y + (h - FONT_H_SMALL) / 2;
	int maxChars = (int)((x1 - textX) / 8);
	const std::string &title = entry.title.empty() ? entry.filename : entry.title;
	const char *fn = title.c_str();
	char displayName[128];
	int nameLen = (int)strlen(fn);
	if (nameLen > maxChars) {
		strncpy(displayName, fn, maxChars - 3);
		displayName[maxChars - 3] = '\0';
		strcat(displayName, "...");
	} else {
		strncpy(displayName, fn, 127);
		displayName[127] = '\0';
	}

	DrawString(displayName, (int)textX, (int)textY, SCALE_MED,
		isSelected ? COL_ITEM_TEXT : COL_ITEM_DIM);
}

void XboxLauncher::RenderGameList() {
	int count = (currentTab_ == TAB_RECENT) ? (int)recentGames_.size() : (int)allGames_.size();
	float cw = (float)COL_LEFT_W;
	float sx = (float)COL_LEFT_X;

	if (count == 0) {
		const char *msg = (currentTab_ == TAB_RECENT)
			? "No recent games" : "No games found on game:\\";
		DrawStringCentered(msg, SCREEN_H / 2 - 20, SCALE_MED, COL_HINT);
		DrawStringCentered("Place .iso or .cso files on game:\\", SCREEN_H / 2 + 20, SCALE_SMALL, COL_HINT);
		return;
	}

	if (viewMode_ == VIEW_GRID || currentTab_ == TAB_RECENT) {
		RenderGrid();
		return;
	}

	// List mode
	int maxVisible = LIST_H / LIST_ITEM_H;
	float listTop = (float)(LIST_Y + LIST_TOP_PAD);

	for (int i = 0; i < maxVisible; i++) {
		int idx = scrollOffset_ + i;
		if (idx >= count) break;

		float y = listTop + (float)i * (float)LIST_ITEM_H;
		bool selected = (idx == selectedIndex_) && (focusRegion_ == FOCUS_GAMES);
		RenderListItem(idx, y, selected);
	}

	// Scrollbar
	if (count > maxVisible) {
		float barX = sx + cw - (float)(SCROLLBAR_GAP + SCROLLBAR_W);
		float barTop = listTop;
		float barH = (float)(maxVisible * LIST_ITEM_H);
		float barW = (float)SCROLLBAR_W;
		DrawRect(barX, barTop, barX + barW, barTop + barH, COL_SCROLLBAR);
		float thumbH = barH * (float)maxVisible / (float)count;
		float thumbY = barTop + barH * (float)scrollOffset_ / (float)count;
		DrawRect(barX, thumbY, barX + barW, thumbY + thumbH, COL_SCROLL_THUMB);
	}
}

// ---------------------------------------------------------------------------
// Game grid
// ---------------------------------------------------------------------------

void XboxLauncher::RenderGridItem(int gameIdx, float x, float y, bool isSelected) {
	const GameEntry &entry = (currentTab_ == TAB_RECENT)
		? recentGames_[gameIdx] : allGames_[gameIdx];
	float cw = (float)GRID_CELL_W;
	float ch = (float)GRID_CELL_H;
	float pad = (float)GRID_CELL_PAD.l;

	// Shadow behind every cell
	if (zimTex_) {
		float selPad = isSelected ? 6.0f : 2.0f;
		DWORD shadowCol = isSelected ? 0x60FFFFFF : COL_C0;
		DrawShadow4Grid(zimTex_, ui_atlas.images[I_DROP_SHADOW],
			x - selPad, y - selPad,
			x + cw + selPad, y + ch + selPad,
			shadowCol, 1.0f);
	}

	// Try to load icon
	IDirect3DTexture9 *icon = GetOrCreateIcon(entry.path);

	if (icon) {
		float iconW = cw - pad * 2;
		float iconH = ch - pad * 2;
		float iconX = x + pad;
		float iconY = y + pad;
		DrawTexture(icon, iconX, iconY, iconW, iconH);
	}
}

void XboxLauncher::RenderGrid() {
	int count = (currentTab_ == TAB_RECENT) ? (int)recentGames_.size() : (int)allGames_.size();
	float listTop = (float)(LIST_Y + LIST_TOP_PAD);
	float startX = (float)(COL_LEFT_X + GRID_PAD);
	float cellStepW = (float)(GRID_CELL_W + GRID_PAD);
	float cellStepH = (float)(GRID_CELL_H + GRID_PAD);
	float cw = (float)COL_LEFT_W;

	int totalRows = (count + GRID_COLS - 1) / GRID_COLS;

	for (int row = 0; row < GRID_MAX_VISIBLE; row++) {
		int gridIdx = (scrollOffset_ + row) * GRID_COLS;
		if (gridIdx >= count) break;

		float y = listTop + (float)row * cellStepH;

		for (int col = 0; col < GRID_COLS; col++) {
			int idx = gridIdx + col;
			if (idx >= count) break;

			float x = startX + (float)col * cellStepW;
			bool selected = (idx == selectedIndex_) && (focusRegion_ == FOCUS_GAMES);
			RenderGridItem(idx, x, y, selected);
		}
	}

	// Scrollbar
	if (totalRows > GRID_MAX_VISIBLE) {
		float barX = (float)(COL_LEFT_X + COL_LEFT_W - (SCROLLBAR_GAP + SCROLLBAR_W));
		float barW = (float)SCROLLBAR_W;
		float barTop = listTop;
		float barH = (float)GRID_MAX_VISIBLE * cellStepH;
		DrawRect(barX, barTop, barX + barW, barTop + barH, COL_SCROLLBAR);
		float thumbH = barH * (float)GRID_MAX_VISIBLE / (float)totalRows;
		float thumbY = barTop + barH * (float)scrollOffset_ / (float)totalRows;
		DrawRect(barX, thumbY, barX + barW, thumbY + thumbH, COL_SCROLL_THUMB);
	}
}

// ---------------------------------------------------------------------------
// Right panel (PPSSPP logo + buttons)
// ---------------------------------------------------------------------------

void XboxLauncher::RenderRightPanel() {
	float x0 = (float)COL_RIGHT_X;
	float x1 = (float)(SCREEN_W - SAFE_X);
	float panelW = x1 - x0;
	float inset = (float)RCOL_PAD.l;
	float halfInset = (float)(inset / 2);

	// PPSSPP branding: I_ICON + I_LOGO (same height)
	if (zimTex_) {
		const AtlasImage &icon = ui_atlas.images[I_ICON];
		const AtlasImage &logo = ui_atlas.images[I_LOGO];

		float logoW = (float)logo.w * RP_BRAND_LOGO_SCALE;
		float logoH = (float)logo.h * RP_BRAND_LOGO_SCALE;
		// Icon height matches logo; width scales proportionally
		float iconH = logoH;
		float iconW = iconH * ((float)icon.w / (float)icon.h);
		float spacing = (float)RP_BRAND_SPACING;
		float totalW = iconW + spacing + logoW;
		float centerX = x0 + panelW / 2;
		float iconX = centerX - totalW / 2;
		float iconY = (float)RP_BRAND_TOP;
		DrawTextureAtlas(zimTex_, icon, iconX, iconY, iconW, iconH);

		float logoX = iconX + iconW + spacing;
		float logoY = iconY + (iconH - logoH) / 2.0f;
		DrawTextureAtlas(zimTex_, logo, logoX, logoY, logoW, logoH);
	}

	// Version text
	DrawStringCentered("PPSSPP v0.9.6b - Xbox 360", RP_VERSION_Y, SCALE_SMALL, COL_HINT);

	// Divider
	DrawRect(x0 + (float)RCOL_PAD.l, (float)RP_DIVIDER_Y,
	         x1 - (float)RCOL_PAD.l, (float)(RP_DIVIDER_Y + RP_DIVIDER_H), COL_DIVIDER);

	// Game poster (PIC1.PNG from selected game)
	{
		int count = (currentTab_ == TAB_RECENT) ? (int)recentGames_.size() : (int)allGames_.size();
		if (count > 0 && selectedIndex_ >= 0 && selectedIndex_ < count) {
			const std::string &selPath = (currentTab_ == TAB_RECENT)
				? recentGames_[selectedIndex_].path
				: allGames_[selectedIndex_].path;
			IDirect3DTexture9 *poster = GetOrCreatePoster(selPath);
			if (poster) {
				float posterW = panelW - (float)(RCOL_PAD.l * 2);
				float posterH = (float)RP_POSTER_H;
				DrawTexture(poster, x0 + inset, (float)RP_POSTER_TOP, posterW, posterH);
			}
		}
	}

	// Settings / Exit list buttons
	float btnX = x0 + inset;
	float btnW = panelW - (float)(RCOL_PAD.l * 2);
	float listBtnY = (float)RP_LIST_BTN_TOP;
	float listBtnH = (float)RP_LIST_BTN_H;

	int btnTextY = (int)(listBtnY + (listBtnH - FONT_H_LARGE) / 2);
	DWORD settingsBg = (focusRegion_ == FOCUS_RIGHT_PANEL && rightPanelItem_ == 0)
		? COL_C2 : COL_C0;
	DrawRect(btnX, listBtnY, btnX + btnW, listBtnY + listBtnH, settingsBg);
	DrawString("Settings", (int)btnX + 10, btnTextY, SCALE_LARGE, COL_TAB_TEXT);

	listBtnY += listBtnH + (float)RP_LIST_BTN_SPACING;
	btnTextY = (int)(listBtnY + (listBtnH - FONT_H_LARGE) / 2);
	DWORD exitBg = (focusRegion_ == FOCUS_RIGHT_PANEL && rightPanelItem_ == 1)
		? COL_C2 : COL_C0;
	DrawRect(btnX, listBtnY, btnX + btnW, listBtnY + listBtnH, exitBg);
	DrawString("Exit", (int)btnX + 10, btnTextY, SCALE_LARGE, COL_TAB_TEXT);

	// Hint rect with gamepad commands (below Exit button)
	float hintTop = listBtnY + listBtnH + 10.0f;
	float hintH = (float)RP_HINT_RECT_H;
	float hintPad = (float)RP_HINT_PAD;
	float hintLineH = (float)RP_HINT_LINE_H;
	DrawRect(btnX, hintTop, btnX + btnW, hintTop + hintH, COL_C0);

	float hy = hintTop + hintPad;
	DrawString("RB LB: Switch Tab", (int)btnX + 10, (int)hy, SCALE_SMALL, COL_TAB_TEXT);
	hy += hintLineH;

	DrawString("RT: Grid/List", (int)btnX + 10, (int)hy, SCALE_SMALL, COL_TAB_TEXT);
	hy += hintLineH;

	DrawString("RT+LT+LSB: In Game Menu", (int)btnX + 10, (int)hy, SCALE_SMALL, COL_TAB_TEXT);
	hy += hintLineH;

	DrawString("B: Exit", (int)btnX + 10, (int)hy, SCALE_SMALL, COL_TAB_TEXT);
}

// ---------------------------------------------------------------------------
// In-game menu (overlay during emulation)
// ---------------------------------------------------------------------------

static const char *ingMenuLabelsR[] = { "Continue Game", "Settings", "Reset Game", "Exit to Xbox", "Exit to Menu" };

void XboxLauncher::UpdateInGameMenu(DWORD buttons) {
	if (!inGameMenuActive_) return;

	DWORD justPressed = (buttons ^ igmPrevButtons_) & buttons;
	igmPrevButtons_ = buttons;

	// D-pad / stick navigation (simple, no repeat)
	bool up = false, down = false, left = false, right = false;
	if (justPressed & XINPUT_GAMEPAD_DPAD_UP) up = true;
	if (justPressed & XINPUT_GAMEPAD_DPAD_DOWN) down = true;
	if (justPressed & XINPUT_GAMEPAD_DPAD_LEFT) left = true;
	if (justPressed & XINPUT_GAMEPAD_DPAD_RIGHT) right = true;

	int leftMax = SaveState::SAVESTATESLOTS - 1;
	int rightMax = 4;
	if (inGameMenuFocus_ == INGMENU_LEFT) {
		if (left) inGameMenuFocus_ = INGMENU_RIGHT;
		if (right) inGameMenuFocus_ = INGMENU_RIGHT;
		if (up && inGameMenuSel_ > 0) inGameMenuSel_--;
		if (down && inGameMenuSel_ < leftMax) inGameMenuSel_++;
	} else {
		if (left) inGameMenuFocus_ = INGMENU_LEFT;
		if (right) inGameMenuFocus_ = INGMENU_LEFT;
		if (up && inGameMenuSel_ > 0) inGameMenuSel_--;
		if (down && inGameMenuSel_ < rightMax) inGameMenuSel_++;
	}

	// Clamp sel when switching panels
	int maxSel = (inGameMenuFocus_ == INGMENU_LEFT) ? leftMax : rightMax;
	if (inGameMenuSel_ > maxSel) inGameMenuSel_ = maxSel;

	// B = close menu (continue game)
	if (justPressed & XINPUT_GAMEPAD_B) {
		inGameMenuActive_ = false;
		return;
	}

	// A = confirm
	if (inGameMenuFocus_ == INGMENU_LEFT) {
		int slot = inGameMenuSel_;
		if (slot >= 0 && slot < SaveState::SAVESTATESLOTS) {
		if (justPressed & XINPUT_GAMEPAD_A) {
			SaveState::SaveSlot(slot, 0, 0);
			SaveState::Process();
			inGameMenuActive_ = false;
			char msg[32];
			sprintf(msg, "State %d saved", slot + 1);
			ShowToast(msg);
		}
		if (justPressed & XINPUT_GAMEPAD_X) {
			SaveState::LoadSlot(slot, 0, 0);
			SaveState::Process();
			inGameMenuActive_ = false;
			char msg[32];
			sprintf(msg, "State %d loaded", slot + 1);
			ShowToast(msg);
		}
		if (justPressed & XINPUT_GAMEPAD_Y && SaveState::HasSaveInSlot(slot)) {
			SaveState::DeleteSlot(slot);
			char msg[32];
			sprintf(msg, "State %d deleted", slot + 1);
			ShowToast(msg);
		}
		}
		} else {
		if (justPressed & XINPUT_GAMEPAD_A) {
			switch (inGameMenuSel_) {
			case 0: // Continue Game
				inGameMenuActive_ = false;
				break;
			case 1: // Settings
				// TODO: open settings
				inGameMenuActive_ = false;
				break;
			case 2: // Reset Game
				inGameMenuActive_ = false;
				wantsResetGame_ = true;
				break;
			case 3: // Exit to Xbox
				inGameMenuActive_ = false;
				wantsExitToXbox_ = true;
				break;
			case 4: // Exit to Menu
				inGameMenuActive_ = false;
				wantsExitToMenu_ = true;
				break;
			}
		}
	}
}

void XboxLauncher::ShowToast(const char *msg) {
	strncpy(toastMsg_, msg, sizeof(toastMsg_) - 1);
	toastMsg_[sizeof(toastMsg_) - 1] = 0;
	toastEndTime_ = GetTickCount() + 2000;
}

void XboxLauncher::RenderToast() {
	if (toastMsg_[0] == 0) return;
	DWORD now = GetTickCount();
	if (now >= toastEndTime_) {
		toastMsg_[0] = 0;
		return;
	}

	EnsureResources();

	float alpha = 1.0f;
	DWORD remaining = toastEndTime_ - now;
	if (remaining < 500) {
		alpha = (float)remaining / 500.0f;
	}

	int strW = StringWidth(toastMsg_, SCALE_SMALL);
	float padX = 20.0f;
	float padY = 8.0f;
	float w = (float)strW + padX * 2;
	float h = (float)FONT_H_SMALL + padY * 2;
	float x = (float)(SCREEN_W - (int)w) / 2.0f;
	float y = (float)SCREEN_H - h - 40.0f;

	DWORD bg = ((DWORD)(alpha * 180.0f) << 24) | 0x00202020;
	DWORD fg = ((DWORD)(alpha * 255.0f) << 24) | 0x00FFFFFF;

	DrawRect(x, y, x + w, y + h, bg);
	DrawString(toastMsg_, (int)x + (int)padX, (int)y + (int)padY, SCALE_SMALL, fg);
}

void XboxLauncher::RenderInGameMenu() {
	if (!inGameMenuActive_) return;

	EnsureResources();

	// Full-screen dimming overlay (behind menu)
	DrawRect(0, 0, (float)SCREEN_W, (float)SCREEN_H, 0xA0000000);

	int totalSlots = SaveState::SAVESTATESLOTS; // 5
	float leftX = 100.0f;
	float leftW = 400.0f;
	float rightX = leftX + leftW + 80.0f;
	float rightW = 500.0f;
	float menuY = 100.0f;
	float itemH = 50.0f;
	float totalH = itemH * (float)totalSlots + 10.0f;

	// Left panel background
	DrawRect(leftX, menuY, leftX + leftW, menuY + totalH, COL_C0);
	DrawRect(leftX + leftW, menuY, leftX + leftW + 2, menuY + totalH, COL_C1);

	// Right panel background
	DrawRect(rightX, menuY, rightX + rightW, menuY + totalH, COL_C0);

	// Left panel items (save/load state slots)
	for (int i = 0; i < totalSlots; i++) {
		float y = menuY + (float)i * itemH + 4.0f;
		float h = itemH - 8.0f;
		bool sel = (inGameMenuFocus_ == INGMENU_LEFT && inGameMenuSel_ == i);
		bool hasSave = SaveState::HasSaveInSlot(i);

		if (sel) DrawRect(leftX + 4, y, leftX + leftW - 4, y + h, COL_C2);

		char label[64];
		if (hasSave)
			sprintf(label, "Slot %d [SAVED]   (A:Save  X:Load  Y:Del)", i + 1);
		else
			sprintf(label, "Slot %d (empty)   (A:Save)", i + 1);

		DrawString(label, (int)leftX + 12, (int)y + (int)(h - FONT_H_SMALL) / 2, SCALE_SMALL, sel ? 0xFFFFFFFF : COL_TAB_TEXT);
	}

	// Right panel items
	for (int i = 0; i < 5; i++) {
		float y = menuY + (float)i * itemH + 4.0f;
		float h = itemH - 8.0f;
		bool sel = (inGameMenuFocus_ == INGMENU_RIGHT && inGameMenuSel_ == i);

		if (sel) DrawRect(rightX + 4, y, rightX + rightW - 4, y + h, COL_C2);

		DrawString(ingMenuLabelsR[i], (int)rightX + 12, (int)y + (int)(h - FONT_H_SMALL) / 2, SCALE_SMALL, sel ? 0xFFFFFFFF : COL_TAB_TEXT);
	}

	// Bottom hints
	float hintY = menuY + totalH + 20.0f;
	DrawString("D-Pad: Navigate   A: Save   X: Load   Y: Delete   B: Close Menu", (int)leftX, (int)hintY, SCALE_SMALL, COL_TAB_TEXT);

	RenderToast();
}

// ---------------------------------------------------------------------------
// Main render
// ---------------------------------------------------------------------------

void XboxLauncher::Render() {
	if (!active_)
		return;

	EnsureResources();
	if (initFailed_)
		return;

	D3DVIEWPORT9 vp = { 0, 0, SCREEN_W, SCREEN_H, 0.0f, 1.0f };
	pD3Ddevice->SetViewport(&vp);

	if (!scanDone_) {
		RenderSplash();
		ScanDirs();
		scanDone_ = true;
		// On next frame, render the actual menu
		return;
	}

	pD3Ddevice->SetVertexDeclaration(vertexDecl_);
	pD3Ddevice->SetVertexShader(vertexShader_);
	pD3Ddevice->SetPixelShader(pixelShader_);
	pD3Ddevice->SetTexture(0, NULL);
	pD3Ddevice->SetRenderState(D3DRS_ZENABLE, FALSE);
	pD3Ddevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	pD3Ddevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	pD3Ddevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	pD3Ddevice->SetRenderState(D3DRS_STENCILENABLE, FALSE);

	// Animated background (like Windows PPSSPP)
	{
		DrawRect(0, 0, (float)SCREEN_W, (float)SCREEN_H, COL_BG);

		if (zimTex_) {
			const AtlasImage &bg = ui_atlas.images[I_BG];
			DrawTextureAtlas(zimTex_, bg, 0, 0, (float)SCREEN_W, (float)SCREEN_H);

			static const int symbols[4] = { I_CROSS, I_CIRCLE, I_SQUARE, I_TRIANGLE };
			float t = (float)(real_time_now() - (double)startTime_);
			float symSize = BG_SYM_SIZE;
			for (int i = 0; i < BG_SYM_COUNT; i++) {
				float x = symX_[i];
				float y = symY_[i] + 40.0f * cos((double)i * 7.2 + (double)t * 1.3);
				float angle = (float)sin((double)i + (double)t);
				int n = i & 3;
				const AtlasImage &sym = ui_atlas.images[symbols[n]];
                DrawTextureAtlasRotated(zimTex_, sym, x, y, symSize, symSize, angle, 0x20FFFFFF);
			}
		}
	}

	// Tab bar
	RenderTabBar();

	// Game list/grid
	RenderGameList();

	// Right panel
	RenderRightPanel();
}

void XboxLauncher::RenderSplash() {
	EnsureResources();
	if (initFailed_)
		return;

	pD3Ddevice->SetVertexDeclaration(vertexDecl_);
	pD3Ddevice->SetVertexShader(vertexShader_);
	pD3Ddevice->SetPixelShader(pixelShader_);
	pD3Ddevice->SetTexture(0, NULL);
	pD3Ddevice->SetRenderState(D3DRS_ZENABLE, FALSE);
	pD3Ddevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	pD3Ddevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	pD3Ddevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	pD3Ddevice->SetRenderState(D3DRS_STENCILENABLE, FALSE);

	DrawRect(0, 0, (float)SCREEN_W, (float)SCREEN_H, COL_BG);

	if (zimTex_) {
		const AtlasImage &icon = ui_atlas.images[I_ICON];
		const AtlasImage &logo = ui_atlas.images[I_LOGO];

		float iconW = (float)icon.w * SPLASH_ICON_SCALE;
		float iconH = (float)icon.h * SPLASH_ICON_SCALE;
		float logoW = (float)logo.w * SPLASH_LOGO_SCALE;
		float logoH = (float)logo.h * SPLASH_LOGO_SCALE;
		float spacing = SPLASH_SPACING;
		float totalW = iconW + spacing + logoW;
		float centerX = (float)SCREEN_W / 2;
		float centerY = (float)SCREEN_H / 2 + (float)SPLASH_CENTER_Y_OFF;

		float iconX = centerX - totalW / 2;
		float iconY = centerY - iconH / 2;
		DrawTextureAtlas(zimTex_, icon, iconX, iconY, iconW, iconH);

		float logoX = iconX + iconW + spacing;
		float logoY = centerY - logoH / 2;
		DrawTextureAtlas(zimTex_, logo, logoX, logoY, logoW, logoH);
	} else if (splashTex_) {
		DrawTexture(splashTex_, 0, 0, (float)SCREEN_W, (float)SCREEN_H);
	}

	DrawStringCentered("Loading...", SCREEN_H - SPLASH_LOADING_OFF, SCALE_SMALL, COL_ITEM_TEXT);
}
