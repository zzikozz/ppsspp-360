#include "XboxFpsOverlay.h"

#include <xtl.h>
#include <cstdio>
#include <vector>

#include "GPU/Directx9/helper/global.h"
#include "Common/Timer.h"

namespace XboxFpsOverlay {

using namespace DX9;

struct FpsVertex {
	float x, y, z, w;
	DWORD color;
};

// 3 columns x 5 rows per digit. Bit 2 = leftmost pixel, bit 0 = rightmost.
// (Written as plain decimal, not 0b literals - those are C++14, VS2010/the
// Xbox 360 compiler don't support them.)
static const unsigned char digitFont[10][5] = {
	{7,5,5,5,7}, // 0
	{2,6,2,2,7}, // 1
	{7,1,7,4,7}, // 2
	{7,1,7,1,7}, // 3
	{5,5,7,1,1}, // 4
	{7,4,7,1,7}, // 5
	{7,4,7,5,7}, // 6
	{7,1,1,1,1}, // 7
	{7,5,7,5,7}, // 8
	{7,5,7,1,7}, // 9
};

static const int PIXEL_SIZE = 3;
static const int DIGIT_W = 3 * PIXEL_SIZE;
static const int DIGIT_SPACING = PIXEL_SIZE * 2;
static const int ORIGIN_X = 20;
static const int ORIGIN_Y = 20;

// The screen-to-clip-space conversion below is hardcoded to 1280x720 to
// match the fixed back buffer size set up in
// GPU/Directx9/helper/global.cpp (d3dpp.BackBufferWidth/Height). If that
// ever changes, update the numbers inside overlayVsCode below too.
//
// The Xbox 360 GPU has no fixed-function pipeline at all (confirmed: there's
// zero usage of D3DFVF_/SetFVF anywhere else in this codebase) - so unlike a
// PC D3D9 port, we can't just set FVF and go. We need a real, tiny,
// hand-written passthrough vertex+pixel shader, exactly like the other DX9
// helper code in this project already does for its own draws (see vscode/
// pscode in global.cpp - this mirrors that same struct-based HLSL style).
static const D3DVERTEXELEMENT9 FpsOverlayVertexElements[] = {
	{ 0, 0,  D3DDECLTYPE_FLOAT4,   D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
	{ 0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR,    0 },
	D3DDECL_END()
};

static const char *overlayVsCode =
	" struct VS_IN                                            "
	" {                                                        "
	"   float4 Pos   : POSITION;                               "
	"   float4 Color : COLOR0;                                 "
	" };                                                       "
	" struct VS_OUT                                            "
	" {                                                        "
	"   float4 ProjPos : POSITION;                             "
	"   float4 Color   : COLOR0;                                "
	" };                                                       "
	" VS_OUT main( VS_IN In )                                  "
	" {                                                        "
	"   VS_OUT Out;                                            "
	"   float2 screenSize = float2(1280.0, 720.0);             "
	"   float2 clip = (In.Pos.xy / screenSize) * 2.0 - 1.0;    "
	"   clip.y = -clip.y;                                      "
	"   Out.ProjPos = float4(clip, 0.0, 1.0);                  "
	"   Out.Color = In.Color;                                  "
	"   return Out;                                            "
	" }                                                        ";

static const char *overlayPsCode =
	" struct PS_IN                                             "
	" {                                                        "
	"   float4 Color : COLOR0;                                 "
	" };                                                       "
	" float4 main( PS_IN In ) : COLOR                          "
	" {                                                        "
	"   return In.Color;                                       "
	" }                                                        ";

static IDirect3DVertexDeclaration9 *overlayVertexDecl = NULL;
static LPDIRECT3DVERTEXSHADER9 overlayVertexShader = NULL;
static LPDIRECT3DPIXELSHADER9 overlayPixelShader = NULL;
static bool overlayInitFailed = false;

static bool EnsureOverlayResourcesCreated() {
	if (overlayVertexDecl && overlayVertexShader && overlayPixelShader)
		return true;
	if (overlayInitFailed)
		return false;

	HRESULT hr = pD3Ddevice->CreateVertexDeclaration(FpsOverlayVertexElements, &overlayVertexDecl);
	if (FAILED(hr)) {
		overlayInitFailed = true;
		return false;
	}

	LPD3DXCONSTANTTABLE dummyTable = NULL;
	if (!CompileVertexShader(overlayVsCode, &overlayVertexShader, &dummyTable)) {
		overlayInitFailed = true;
		return false;
	}
	dummyTable = NULL;
	if (!CompilePixelShader(overlayPsCode, &overlayPixelShader, &dummyTable)) {
		overlayInitFailed = true;
		return false;
	}

	return true;
}

static void AddQuad(std::vector<FpsVertex> &verts, float x0, float y0, float x1, float y1, DWORD color) {
	FpsVertex v0 = { x0, y0, 0.0f, 1.0f, color };
	FpsVertex v1 = { x1, y0, 0.0f, 1.0f, color };
	FpsVertex v2 = { x0, y1, 0.0f, 1.0f, color };
	FpsVertex v3 = { x1, y1, 0.0f, 1.0f, color };

	verts.push_back(v0);
	verts.push_back(v1);
	verts.push_back(v2);

	verts.push_back(v1);
	verts.push_back(v3);
	verts.push_back(v2);
}

static void AddDigit(std::vector<FpsVertex> &verts, int digit, int screenX, int screenY, DWORD color) {
	if (digit < 0 || digit > 9)
		return;
	for (int row = 0; row < 5; row++) {
		unsigned char bits = digitFont[digit][row];
		for (int col = 0; col < 3; col++) {
			if (bits & (1 << (2 - col))) {
				float x0 = (float)(screenX + col * PIXEL_SIZE);
				float y0 = (float)(screenY + row * PIXEL_SIZE);
				AddQuad(verts, x0, y0, x0 + PIXEL_SIZE, y0 + PIXEL_SIZE, color);
			}
		}
	}
}

void DrawFpsOverlay() {
	static u32 lastSampleTimeMs = 0;
	static int frameCount = 0;
	static int currentFps = 0;

	frameCount++;
	u32 nowMs = Common::Timer::GetTimeMs();
	if (lastSampleTimeMs == 0)
		lastSampleTimeMs = nowMs;

	u32 elapsed = nowMs - lastSampleTimeMs;
	if (elapsed >= 1000) {
		currentFps = (int)(frameCount * 1000.0 / (double)elapsed);
		frameCount = 0;
		lastSampleTimeMs = nowMs;
	}

	int fps = currentFps;
	if (fps < 0) fps = 0;
	if (fps > 999) fps = 999;

	char buf[8];
	sprintf(buf, "%3d", fps); // space-padded, always 3 chars + null

	std::vector<FpsVertex> verts;
	verts.reserve(64);

	int x = ORIGIN_X;
	for (int i = 0; i < 3; i++) {
		if (buf[i] >= '0' && buf[i] <= '9') {
			AddDigit(verts, buf[i] - '0', x, ORIGIN_Y, 0xFF00FF00 /* opaque green, ARGB */);
		}
		x += DIGIT_W + DIGIT_SPACING;
	}

	if (verts.empty())
		return;

	if (!EnsureOverlayResourcesCreated())
		return; // shader/decl creation failed once - don't retry every frame

	// Draw on top of the current render target (set up by BeginFrame).
	// Do NOT grab the backbuffer here - with predicated tiling the render
	// target is pTilingRenderTarget and EndTiling resolves it to the front
	// buffer texture that SwapBuffers presents.

	D3DVIEWPORT9 vp;
	vp.X = 0; vp.Y = 0; vp.Width = 1280; vp.Height = 720;
	vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
	pD3Ddevice->SetViewport(&vp);

	pD3Ddevice->SetVertexDeclaration(overlayVertexDecl);
	pD3Ddevice->SetVertexShader(overlayVertexShader);
	pD3Ddevice->SetPixelShader(overlayPixelShader);
	pD3Ddevice->SetTexture(0, NULL);

	// Aggressive state reset
	pD3Ddevice->SetRenderState(D3DRS_ZENABLE, FALSE);
	pD3Ddevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	pD3Ddevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	pD3Ddevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	pD3Ddevice->SetRenderState(D3DRS_STENCILENABLE, FALSE);

	pD3Ddevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, (UINT)(verts.size() / 3), &verts[0], sizeof(FpsVertex));
}

} // namespace XboxFpsOverlay