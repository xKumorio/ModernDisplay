// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "d3d11_presenter.h"
#include "blit_bc.h"
#include "road_puddles_bc.h"
#include "ao_reuse_bc.h"
#include "lut_loader.h"
#include "../core/log.h"
#include "../core/scheduler.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <string>

template <typename T>
static inline void SafeRelease(T *&p)
{
	if (p) { p->Release(); p = nullptr; }
}

static std::string WideToUtf8(const wchar_t *ws)
{
	if (!ws || !ws[0]) return "";
	int n = WideCharToMultiByte(CP_UTF8, 0, ws, -1, nullptr, 0, nullptr, nullptr);
	if (n <= 1) return "";
	std::string s((size_t)(n - 1), '\0');
	WideCharToMultiByte(CP_UTF8, 0, ws, -1, s.data(), n, nullptr, nullptr);
	return s;
}

namespace {

struct PostCBData {
	float    invSrcW;
	float    invSrcH;
	float    invDstW;
	float    invDstH;
	float    sharpness;
	float    vibrance;
	float    aoStrength;
	float    lutSize;
	uint32_t flags;
	float    debandThresh;
	float    ditherScale;
	float    bloomStrength;
	float    fogStrength;
	float    clarity;
	uint32_t tonemapMode;
	float    weatherFog;
	float    skyBot[3];
	float    nearZ;
	float    skyTop[3];
	float    farZ;
	float    sunDirView[3];
	float    fogStart;
	float    projScaleX;
	float    projScaleY;
	float    giStrength;
	float    rainDropAmt;
	float    timeSec;
	float    camPitchZ;
	float    compareSplitPx; // < 0 = off, >= 0 = exact swapchain pixel X of live Before/After wipe
	float    padPost3;
};
static_assert(sizeof(PostCBData) == 144, "PostCBData must match PostParams cbuffer");

struct SsrCBData {
	float    invViewProj[16];
	float    viewProj[16];
	float    camPosW[3];
	float    nearZ;
	float    invW;
	float    invH;
	float    farZ;
	float    rainWet;
	float    skyTop[3];
	uint32_t quality;
	float    skyBot[3];
	float    timeSec;
	float    roadGridOrigin[2];
	float    roadGridSpacing;
	float    unknownMaterialOffset;
	float    actorCrop[4], actorGround[4];
};
static_assert(sizeof(SsrCBData) == 240, "SsrCBData must match SsrParams cbuffer");

struct BloomCBData {
	float    invSrcW;
	float    invSrcH;
	float    threshold;
	float    knee;
	uint32_t flareOn;
	float    padBloom[3];
};
static_assert(sizeof(BloomCBData) == 32, "BloomCBData must match BloomParams cbuffer");

struct TaaCBData {
	float    currToPrevClip[16];
	float    invW;
	float    invH;
	float    prevNearZ;
	float    prevFarZ;
	uint32_t hasHistory;
	uint32_t hasSmaa;
	float    baseBlend;
	float    padTaa;
};
static_assert(sizeof(TaaCBData) == 96, "TaaCBData must match TaaParams cbuffer");

struct AoDepthCBData {
	float v0;
	float v1;
	float v2;
	float v3;
};
static_assert(sizeof(AoDepthCBData) == 16, "AoDepthCBData must match 16-byte cbuffer");

struct AoCBData {
	float    invDepthW;
	float    invDepthH;
	float    projScaleX;
	float    projScaleY;
	float    nearZ;
	float    farZ;
	float    radiusWorld;
	float    strength;
	uint32_t quality;
	float    depthBaseMip;
	uint32_t frameIndex;
	float    padAo;
};
static_assert(sizeof(AoCBData) == 48, "AoCBData must match AoParams cbuffer");

struct AoBlurCBData {
	float texelStepX;
	float texelStepY;
	float depthBaseMip;
	float edgeSharpness;
	float padBlur[4];
};
static_assert(sizeof(AoBlurCBData) == 32, "AoBlurCBData must match AoBlurParams cbuffer");

struct AoTemporalCBData {
	float    currToPrevClip[16];
	float    invAoW;
	float    invAoH;
	float    prevNearZ;
	float    prevFarZ;
	float    depthBaseMip;
	uint32_t hasHistory;
	float    blendWeight;
	float    padTemp;
};
static_assert(sizeof(AoTemporalCBData) == 96, "AoTemporalCBData must match AoTemporalParams cbuffer");

static bool Invert4x4Double(const double in[16], double out[16])
{
	double a[4][8] = {};
	for (int r = 0; r < 4; ++r) {
		for (int c = 0; c < 4; ++c) {
			a[r][c] = in[r * 4 + c];
			a[r][c + 4] = (r == c) ? 1.0 : 0.0;
		}
	}
	for (int col = 0; col < 4; ++col) {
		int piv = col;
		double maxAbs = fabs(a[col][col]);
		for (int r = col + 1; r < 4; ++r) {
			double v = fabs(a[r][col]);
			if (v > maxAbs) { maxAbs = v; piv = r; }
		}
		if (maxAbs < 1e-12) return false;
		if (piv != col) {
			for (int c = 0; c < 8; ++c) {
				double tmp = a[col][c];
				a[col][c] = a[piv][c];
				a[piv][c] = tmp;
			}
		}
		double invDiag = 1.0 / a[col][col];
		for (int c = 0; c < 8; ++c) a[col][c] *= invDiag;
		for (int r = 0; r < 4; ++r) {
			if (r == col) continue;
			double f = a[r][col];
			if (f == 0.0) continue;
			for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
		}
	}
	for (int r = 0; r < 4; ++r)
		for (int c = 0; c < 4; ++c)
			out[r * 4 + c] = a[r][c + 4];
	return true;
}

struct SmaaCBData {
	float    invW;
	float    invH;
	float    threshold;
	float    depthThresh;
	uint32_t maxSearch;
	uint32_t useDepth;
	float    nearZ;
	float    farZ;
};
static_assert(sizeof(SmaaCBData) == 32, "SmaaCBData must match SmaaParams cbuffer");

struct HudCBData {
	float invSwapW;
	float invSwapH;
	float pad[2];
};

struct HudVertex {
	float x, y;
	float u, v;
	float r, g, b, a;
};

constexpr int kHudMaxChars = 128;
constexpr int kHudMaxVerts = kHudMaxChars * 5 * 6; // 4-way 1px outline + foreground per glyph

// Crisp 5x7 bitmap font for ASCII 32..95 (uppercase, digits, punctuation) - 5 column bytes per glyph (LSB = top row)
static const uint8_t kFont5x7[64][5] = {
	{0x00,0x00,0x00,0x00,0x00}, // ' '
	{0x00,0x00,0x5F,0x00,0x00}, // '!'
	{0x00,0x07,0x00,0x07,0x00}, // '"'
	{0x14,0x7F,0x14,0x7F,0x14}, // '#'
	{0x24,0x2A,0x7F,0x2A,0x12}, // '$'
	{0x23,0x13,0x08,0x64,0x62}, // '%'
	{0x36,0x49,0x55,0x22,0x50}, // '&'
	{0x00,0x05,0x03,0x00,0x00}, // '\''
	{0x00,0x1C,0x22,0x41,0x00}, // '('
	{0x00,0x41,0x22,0x1C,0x00}, // ')'
	{0x14,0x08,0x3E,0x08,0x14}, // '*'
	{0x08,0x08,0x3E,0x08,0x08}, // '+'
	{0x00,0x50,0x30,0x00,0x00}, // ','
	{0x08,0x08,0x08,0x08,0x08}, // '-'
	{0x00,0x60,0x60,0x00,0x00}, // '.'
	{0x20,0x10,0x08,0x04,0x02}, // '/'
	{0x3E,0x51,0x49,0x45,0x3E}, // '0'
	{0x00,0x42,0x7F,0x40,0x00}, // '1'
	{0x42,0x61,0x51,0x49,0x46}, // '2'
	{0x21,0x41,0x45,0x4B,0x31}, // '3'
	{0x18,0x14,0x12,0x7F,0x10}, // '4'
	{0x27,0x45,0x45,0x45,0x39}, // '5'
	{0x3C,0x4A,0x49,0x49,0x30}, // '6'
	{0x01,0x71,0x09,0x05,0x03}, // '7'
	{0x36,0x49,0x49,0x49,0x36}, // '8'
	{0x06,0x49,0x49,0x29,0x1E}, // '9'
	{0x00,0x36,0x36,0x00,0x00}, // ':'
	{0x00,0x56,0x36,0x00,0x00}, // ';'
	{0x08,0x14,0x22,0x41,0x00}, // '<'
	{0x14,0x14,0x14,0x14,0x14}, // '='
	{0x00,0x41,0x22,0x14,0x08}, // '>'
	{0x02,0x01,0x51,0x09,0x06}, // '?'
	{0x32,0x49,0x79,0x41,0x3E}, // '@'
	{0x7E,0x11,0x11,0x11,0x7E}, // 'A'
	{0x7F,0x49,0x49,0x49,0x36}, // 'B'
	{0x3E,0x41,0x41,0x41,0x22}, // 'C'
	{0x7F,0x41,0x41,0x22,0x1C}, // 'D'
	{0x7F,0x49,0x49,0x49,0x41}, // 'E'
	{0x7F,0x09,0x09,0x09,0x01}, // 'F'
	{0x3E,0x41,0x49,0x49,0x7A}, // 'G'
	{0x7F,0x08,0x08,0x08,0x7F}, // 'H'
	{0x00,0x41,0x7F,0x41,0x00}, // 'I'
	{0x20,0x40,0x41,0x3F,0x01}, // 'J'
	{0x7F,0x08,0x14,0x22,0x41}, // 'K'
	{0x7F,0x40,0x40,0x40,0x40}, // 'L'
	{0x7F,0x02,0x0C,0x02,0x7F}, // 'M'
	{0x7F,0x04,0x08,0x10,0x7F}, // 'N'
	{0x3E,0x41,0x41,0x41,0x3E}, // 'O'
	{0x7F,0x09,0x09,0x09,0x06}, // 'P'
	{0x3E,0x41,0x51,0x21,0x5E}, // 'Q'
	{0x7F,0x09,0x19,0x29,0x46}, // 'R'
	{0x46,0x49,0x49,0x49,0x31}, // 'S'
	{0x01,0x01,0x7F,0x01,0x01}, // 'T'
	{0x3F,0x40,0x40,0x40,0x3F}, // 'U'
	{0x1F,0x20,0x40,0x20,0x1F}, // 'V'
	{0x3F,0x40,0x38,0x40,0x3F}, // 'W'
	{0x63,0x14,0x08,0x14,0x63}, // 'X'
	{0x07,0x08,0x70,0x08,0x07}, // 'Y'
	{0x61,0x51,0x49,0x45,0x43}, // 'Z'
	{0x00,0x7F,0x41,0x41,0x00}, // '['
	{0x02,0x04,0x08,0x10,0x20}, // '\'
	{0x00,0x41,0x41,0x7F,0x00}, // ']'
	{0x04,0x02,0x01,0x02,0x04}, // '^'
	{0x40,0x40,0x40,0x40,0x40}, // '_'
};

} // namespace

void ComputeDisplayViewport(UINT gameW, UINT gameH, UINT swapW, UINT swapH, ScalingMode mode,
                            float &vpX, float &vpY, float &vpW, float &vpH,
                            bool *usePointOut, bool *needBarsOut)
{
	vpX = 0.0f;
	vpY = 0.0f;
	vpW = (float)swapW;
	vpH = (float)swapH;
	bool usePoint = false;
	bool needBars = false;

	if (mode == ScalingMode::Integer && gameW > 0 && gameH > 0 && swapW >= gameW && swapH >= gameH) {
		UINT kx = swapW / gameW;
		UINT ky = swapH / gameH;
		UINT k = (kx < ky) ? kx : ky;
		if (k < 1) k = 1;
		UINT dstW = gameW * k;
		UINT dstH = gameH * k;
		vpX = (float)((swapW - dstW) / 2);
		vpY = (float)((swapH - dstH) / 2);
		vpW = (float)dstW;
		vpH = (float)dstH;
		usePoint = true;
		needBars = (dstW != swapW || dstH != swapH);
	} else if ((mode == ScalingMode::Aspect || mode == ScalingMode::Integer) && gameW > 0 && gameH > 0 && swapW > 0 && swapH > 0) {
		double srcAspect = (double)gameW / (double)gameH;
		double dstAspect = (double)swapW / (double)swapH;
		if (fabs(srcAspect - dstAspect) > 0.005) {
			double dstW = (double)swapW, dstH = (double)swapH;
			if (dstAspect > srcAspect) dstW = dstH * srcAspect;
			else                       dstH = dstW / srcAspect;
			vpX = (float)((swapW - dstW) * 0.5);
			vpY = (float)((swapH - dstH) * 0.5);
			vpW = (float)dstW;
			vpH = (float)dstH;
			needBars = true;
		}
	}

	if (usePointOut) *usePointOut = usePoint;
	if (needBarsOut) *needBarsOut = needBars;
}

bool D3D11Presenter::Init(HWND hwnd, UINT swapW, UINT swapH, LUID adapterLuid, bool hasLuid)
{
	m_hwnd = hwnd;
	m_swapW = swapW ? swapW : 1280;
	m_swapH = swapH ? swapH : 720;
	QueryPerformanceFrequency(&m_qpcFreq);
	m_lastPresentQpc.QuadPart = 0;

	if (!m_gammaCsInit) {
		InitializeCriticalSection(&m_gammaCs);
		m_gammaCsInit = true;
	}
	if (!m_asyncCsInit) {
		InitializeCriticalSection(&m_asyncCs);
		InitializeConditionVariable(&m_cvWork);
		InitializeConditionVariable(&m_cvDone);
		m_asyncCsInit = true;
	}

	for (int i = 0; i < 256; ++i) {
		WORD v = (WORD)(i << 8);
		m_gammaRamp.red[i] = m_gammaRamp.green[i] = m_gammaRamp.blue[i] = v;
	}
	m_gammaDirty = true;
	m_gammaIsIdentity = true;

	HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory5), (void **)&m_factory);
	if (FAILED(hr) || !m_factory) {
		Log("DXGI: CreateDXGIFactory1(IDXGIFactory5) failed (0x%08lX)", hr);
		Shutdown();
		return false;
	}

	IDXGIAdapter1 *matchedAdapter = nullptr;
	if (hasLuid) {
		IDXGIAdapter1 *cand = nullptr;
		for (UINT i = 0; m_factory->EnumAdapters1(i, &cand) != DXGI_ERROR_NOT_FOUND; ++i) {
			DXGI_ADAPTER_DESC1 desc = {};
			if (SUCCEEDED(cand->GetDesc1(&desc)) &&
			    desc.AdapterLuid.LowPart == adapterLuid.LowPart &&
			    desc.AdapterLuid.HighPart == adapterLuid.HighPart) {
				matchedAdapter = cand;
				break;
			}
			cand->Release();
		}
	}

	const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1 };
	D3D_FEATURE_LEVEL actualFl = D3D_FEATURE_LEVEL_11_0;
	hr = D3D11CreateDevice(
		matchedAdapter,
		matchedAdapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
		nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
		levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
		&m_dev, &actualFl, &m_ctx);
	SafeRelease(matchedAdapter);
	if (FAILED(hr) || !m_dev || !m_ctx) {
		Log("D3D11: D3D11CreateDevice failed (0x%08lX)", hr);
		Shutdown();
		return false;
	}

	std::string gpuName;
	UINT vendorId = 0;
	IDXGIDevice1 *dxgiDev = nullptr;
	IDXGIAdapter *adapter = nullptr;
	if (SUCCEEDED(m_dev->QueryInterface(__uuidof(IDXGIDevice1), (void **)&dxgiDev)) && dxgiDev) {
		dxgiDev->SetMaximumFrameLatency(2);
		if (SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter) {
			DXGI_ADAPTER_DESC desc = {};
			if (SUCCEEDED(adapter->GetDesc(&desc))) {
				gpuName = WideToUtf8(desc.Description);
				vendorId = desc.VendorId;
			}
			adapter->Release();
		}
		dxgiDev->Release();
	}

	BOOL tear = FALSE;
	if (SUCCEEDED(m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear))))
		m_tearingSupported = (tear != FALSE);

	m_factory->MakeWindowAssociation(m_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);

	auto tryCreateSwap = [&](DXGI_FORMAT fmt) -> bool {
		DXGI_SWAP_CHAIN_DESC1 scd = {};
		scd.Width = m_swapW;
		scd.Height = m_swapH;
		scd.Format = fmt;
		scd.SampleDesc.Count = 1;
		scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		scd.BufferCount = 3;
		scd.Scaling = DXGI_SCALING_STRETCH;
		scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
		            (m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
		hr = m_factory->CreateSwapChainForHwnd(m_dev, m_hwnd, &scd, nullptr, nullptr, &m_swapChain);
		if (SUCCEEDED(hr) && m_swapChain) {
			m_hasWaitable = true;
			m_swapFormat = fmt;
			return true;
		}
		scd.Flags = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
		hr = m_factory->CreateSwapChainForHwnd(m_dev, m_hwnd, &scd, nullptr, nullptr, &m_swapChain);
		if (SUCCEEDED(hr) && m_swapChain) {
			m_hasWaitable = false;
			m_swapFormat = fmt;
			return true;
		}
		return false;
	};

	bool swapOk = false;
	if (g_cfg.output10Bit)
		swapOk = tryCreateSwap(DXGI_FORMAT_R10G10B10A2_UNORM);
	if (!swapOk)
		swapOk = tryCreateSwap(DXGI_FORMAT_B8G8R8A8_UNORM);
	if (!swapOk) {
		Log("DXGI: CreateSwapChainForHwnd(FLIP_DISCARD) failed (0x%08lX)", hr);
		Shutdown();
		return false;
	}

	if (m_hasWaitable && SUCCEEDED(m_swapChain->QueryInterface(__uuidof(IDXGISwapChain2), (void **)&m_swapChain2)) && m_swapChain2) {
		m_swapChain2->SetMaximumFrameLatency(2);
		m_waitableObj = m_swapChain2->GetFrameLatencyWaitableObject();
	}

	if (!AcquireBackBuffer() || !CreatePostPipeline()) {
		Shutdown();
		return false;
	}
	bool aoReady    = CreateAoPipeline();    // Non-fatal
	bool smaaReady  = CreateSmaaPipeline();  // Non-fatal
	bool ssrReady   = CreateSsrPipeline();   // Non-fatal
	bool ssgiReady  = CreateSsgiPipeline();  // Non-fatal
	bool bloomReady = CreateBloomPipeline(); // Non-fatal
	bool taaReady   = CreateTaaPipeline();   // Non-fatal
	bool hudReady   = CreateHudPipeline();   // Non-fatal
	WarmupPipelines();
	UpdateLutIfChanged();

	D3D11_QUERY_DESC qd = {};
	qd.Query = D3D11_QUERY_EVENT;
	for (int i = 0; i < kNumSlots; ++i)
		m_dev->CreateQuery(&qd, &m_slotQuery[i]);
	if (m_slotQuery[0] && m_slotQuery[1] && SUCCEEDED(m_dev->QueryInterface(IID_PPV_ARGS(&m_queueDevice)))) {
		for (HANDLE &event : m_slotReady)
			event = CreateEventW(nullptr, TRUE, TRUE, nullptr);
		if (!m_slotReady[0] || !m_slotReady[1]) SafeRelease(m_queueDevice);
	}

	m_reflex.Init(m_dev, m_ctx, m_swapChain2, m_waitableObj, vendorId, g_cfg.reflex);
	m_appliedCfgSeq = InterlockedCompareExchange(&g_cfgSeq, 0, 0);

	m_stopThread = false;
	m_queued = {};
	m_inFlight = false;
	m_slotBusy[0] = m_slotBusy[1] = false;
	m_thread = CreateThread(nullptr, 0, PresentThreadProc, this, 0, nullptr);
	if (m_thread)
		SetThreadPriority(m_thread, THREAD_PRIORITY_ABOVE_NORMAL);

	Log("D3D11: Presenter ready on %s (vendor 0x%04X, FL 0x%X, %ux%u, %s, FLIP_DISCARD, tearing=%d, waitable=%d, async=%d, ao=%d, ssr=%d, ssgi=%d, bloom=%d, taa=%d, smaa=%d, hud=%d)",
		gpuName.c_str(), vendorId, (unsigned)actualFl, m_swapW, m_swapH,
		(m_swapFormat == DXGI_FORMAT_R10G10B10A2_UNORM) ? "10-bit" : "8-bit",
		m_tearingSupported ? 1 : 0, m_hasWaitable ? 1 : 0, m_thread ? 1 : 0,
		aoReady ? 1 : 0, ssrReady ? 1 : 0, ssgiReady ? 1 : 0, bloomReady ? 1 : 0, taaReady ? 1 : 0, smaaReady ? 1 : 0, hudReady ? 1 : 0);
	return true;
}

void D3D11Presenter::Shutdown()
{
	DrainAsync();
	if (m_thread) {
		EnterCriticalSection(&m_asyncCs);
		m_stopThread = true;
		WakeAllConditionVariable(&m_cvWork);
		LeaveCriticalSection(&m_asyncCs);
		WaitForSingleObject(m_thread, 3000);
		CloseHandle(m_thread);
		m_thread = nullptr;
	}

	m_reflex.Disable();
	if (m_waitableObj) { CloseHandle(m_waitableObj); m_waitableObj = nullptr; }

	UnbindSharedTextures();
	ReleaseTaaTargets();
	ReleaseBloomTargets();
	ReleaseSsgiTargets();
	ReleaseSsrTargets();
	ReleaseAoTargets();
	ReleaseDepthPyramid();
	ReleaseSmaaTargets();
	ReleaseBackBuffer();

	SafeRelease(m_lutSrv);
	SafeRelease(m_lutTex);
	m_lutSize = 0.0f;
	m_loadedLutName.clear();

	SafeRelease(m_taaCB);
	SafeRelease(m_taaPS);
	SafeRelease(m_bloomCB);
	SafeRelease(m_bloomUpPS);
	SafeRelease(m_bloomDownPS);
	SafeRelease(m_bloomPrePS);
	SafeRelease(m_ssgiTemporalPS);
	SafeRelease(m_ssgiBlurPS);
	SafeRelease(m_ssgiPS);
	SafeRelease(m_ssrCB);
	SafeRelease(m_ssrBlurPS);
	SafeRelease(m_roadPuddlesPS);
	SafeRelease(m_roadGridSrv);
	SafeRelease(m_roadGridTexture);
	m_uploadedRoadGridRev = 0;
	SafeRelease(m_ssrPS);
	SafeRelease(m_smaaCB);
	SafeRelease(m_smaaWeightPS);
	SafeRelease(m_smaaEdgePS);
	SafeRelease(m_aoTemporalCB);
	SafeRelease(m_aoBlurCB);
	SafeRelease(m_aoCB);
	SafeRelease(m_aoDepthCB);
	SafeRelease(m_aoTemporalPS);
	SafeRelease(m_aoBlurPS);
	SafeRelease(m_aoDenoise2dPS);
	SafeRelease(m_aoPS);
	SafeRelease(m_markReused);
	SafeRelease(m_traceMissing);
	SafeRelease(m_aoReusePS);
	SafeRelease(m_aoDepthMipPS);
	SafeRelease(m_aoDepthLinPS);
	SafeRelease(m_queueDevice);
	for (int i = 0; i < kNumSlots; ++i) {
		SafeRelease(m_slotQuery[i]);
		if (m_slotReady[i]) CloseHandle(m_slotReady[i]);
		m_slotReady[i] = nullptr;
	}

	SafeRelease(m_fontSrv);
	SafeRelease(m_fontTex);
	SafeRelease(m_hudVB);
	SafeRelease(m_hudCB);
	SafeRelease(m_hudLayout);
	SafeRelease(m_hudPS);
	SafeRelease(m_hudVS);

	SafeRelease(m_gammaSrv);
	SafeRelease(m_gammaTex);
	SafeRelease(m_dsOff);
	SafeRelease(m_bsAlpha);
	SafeRelease(m_bsOpaque);
	SafeRelease(m_rsNoCull);
	SafeRelease(m_smpPointMipLin);
	SafeRelease(m_smpPoint);
	SafeRelease(m_smpLinear);
	SafeRelease(m_postCB);
	SafeRelease(m_postPS);
	SafeRelease(m_blitVS);
	SafeRelease(m_swapChain2);
	SafeRelease(m_swapChain);
	SafeRelease(m_factory);
	SafeRelease(m_ctx);
	SafeRelease(m_dev);
	m_reflex.Shutdown();

	if (m_asyncCsInit) { DeleteCriticalSection(&m_asyncCs); m_asyncCsInit = false; }
	if (m_gammaCsInit) { DeleteCriticalSection(&m_gammaCs); m_gammaCsInit = false; }
}

bool D3D11Presenter::CreatePostPipeline()
{
	if (FAILED(m_dev->CreateVertexShader(kBlitVS, sizeof(kBlitVS), nullptr, &m_blitVS))) return false;
	if (FAILED(m_dev->CreatePixelShader(kPostPS, sizeof(kPostPS), nullptr, &m_postPS))) return false;

	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = sizeof(PostCBData);
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(m_dev->CreateBuffer(&cbd, nullptr, &m_postCB))) return false;

	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(m_dev->CreateSamplerState(&sd, &m_smpLinear))) return false;

	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	if (FAILED(m_dev->CreateSamplerState(&sd, &m_smpPoint))) return false;

	sd.Filter = D3D11_FILTER_MIN_MAG_POINT_MIP_LINEAR;
	if (FAILED(m_dev->CreateSamplerState(&sd, &m_smpPointMipLin))) return false;

	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = FALSE;
	if (FAILED(m_dev->CreateRasterizerState(&rd, &m_rsNoCull))) return false;

	D3D11_BLEND_DESC bd = {};
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (FAILED(m_dev->CreateBlendState(&bd, &m_bsOpaque))) return false;

	bd.RenderTarget[0].BlendEnable = TRUE;
	bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
	bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
	bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
	bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
	if (FAILED(m_dev->CreateBlendState(&bd, &m_bsAlpha))) return false;

	D3D11_DEPTH_STENCIL_DESC dsd = {};
	dsd.DepthEnable = FALSE;
	if (FAILED(m_dev->CreateDepthStencilState(&dsd, &m_dsOff))) return false;

	// 256x1 Hardware Gamma LUT texture
	D3D11_TEXTURE1D_DESC td = {};
	td.Width = 256;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
	td.Usage = D3D11_USAGE_DYNAMIC;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(m_dev->CreateTexture1D(&td, nullptr, &m_gammaTex))) return false;
	if (FAILED(m_dev->CreateShaderResourceView(m_gammaTex, nullptr, &m_gammaSrv))) return false;

	return true;
}

bool D3D11Presenter::CreateAoPipeline()
{
	if (FAILED(m_dev->CreatePixelShader(kAoDepthLinPS, sizeof(kAoDepthLinPS), nullptr, &m_aoDepthLinPS))) return false;
	if (FAILED(m_dev->CreatePixelShader(kAoDepthMipPS, sizeof(kAoDepthMipPS), nullptr, &m_aoDepthMipPS))) return false;
	if (FAILED(m_dev->CreatePixelShader(kAoPS, sizeof(kAoPS), nullptr, &m_aoPS))) return false;
	if (m_dev->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 &&
	    SUCCEEDED(m_dev->CreatePixelShader(kAoReusePS, sizeof(kAoReusePS), nullptr, &m_aoReusePS))) {
		D3D11_DEPTH_STENCIL_DESC ds = {};
		ds.DepthFunc = D3D11_COMPARISON_ALWAYS;
		ds.StencilEnable = TRUE;
		ds.StencilReadMask = ds.StencilWriteMask = 1;
		ds.FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_REPLACE, D3D11_COMPARISON_ALWAYS };
		ds.BackFace = ds.FrontFace;
		m_dev->CreateDepthStencilState(&ds, &m_markReused);
		ds.StencilWriteMask = 0;
		ds.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
		ds.FrontFace.StencilFunc = D3D11_COMPARISON_EQUAL;
		ds.BackFace = ds.FrontFace;
		m_dev->CreateDepthStencilState(&ds, &m_traceMissing);
	}
	if (FAILED(m_dev->CreatePixelShader(kAoDenoise2dPS, sizeof(kAoDenoise2dPS), nullptr, &m_aoDenoise2dPS))) return false;
	if (FAILED(m_dev->CreatePixelShader(kAoBlurPS, sizeof(kAoBlurPS), nullptr, &m_aoBlurPS))) return false;
	if (FAILED(m_dev->CreatePixelShader(kAoTemporalPS, sizeof(kAoTemporalPS), nullptr, &m_aoTemporalPS))) return false;

	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = sizeof(AoDepthCBData);
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(m_dev->CreateBuffer(&cbd, nullptr, &m_aoDepthCB))) return false;

	cbd.ByteWidth = sizeof(AoCBData);
	if (FAILED(m_dev->CreateBuffer(&cbd, nullptr, &m_aoCB))) return false;

	cbd.ByteWidth = sizeof(AoBlurCBData);
	if (FAILED(m_dev->CreateBuffer(&cbd, nullptr, &m_aoBlurCB))) return false;

	cbd.ByteWidth = sizeof(AoTemporalCBData);
	if (FAILED(m_dev->CreateBuffer(&cbd, nullptr, &m_aoTemporalCB))) return false;
	return true;
}

void D3D11Presenter::ReleaseDepthPyramid()
{
	for (UINT m = 0; m < kAoDepthMips; ++m) {
		SafeRelease(m_aoDepthMipSrv[m]);
		SafeRelease(m_aoDepthMipRtv[m]);
	}
	SafeRelease(m_aoDepthSrv);
	SafeRelease(m_aoDepthTex);
	SafeRelease(m_aoPrevDepthSrv);
	SafeRelease(m_aoPrevDepthTex);
	m_aoDepthW = m_aoDepthH = 0;
	m_hasPrevViewProj = false;
}

bool D3D11Presenter::EnsureDepthPyramid(UINT w, UINT h)
{
	if (w == 0 || h == 0 || !m_dev) return false;
	if (m_aoDepthSrv && m_aoPrevDepthSrv && m_aoDepthW == w && m_aoDepthH == h)
		return true;

	ReleaseDepthPyramid();

	D3D11_TEXTURE2D_DESC dtd = {};
	dtd.Width = w;
	dtd.Height = h;
	dtd.MipLevels = kAoDepthMips;
	dtd.ArraySize = 1;
	dtd.Format = DXGI_FORMAT_R32_FLOAT;
	dtd.SampleDesc.Count = 1;
	dtd.Usage = D3D11_USAGE_DEFAULT;
	dtd.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(m_dev->CreateTexture2D(&dtd, nullptr, &m_aoDepthTex)) || !m_aoDepthTex) {
		ReleaseDepthPyramid();
		return false;
	}

	D3D11_SHADER_RESOURCE_VIEW_DESC srvAll = {};
	srvAll.Format = DXGI_FORMAT_R32_FLOAT;
	srvAll.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvAll.Texture2D.MostDetailedMip = 0;
	srvAll.Texture2D.MipLevels = kAoDepthMips;
	if (FAILED(m_dev->CreateShaderResourceView(m_aoDepthTex, &srvAll, &m_aoDepthSrv)) || !m_aoDepthSrv) {
		ReleaseDepthPyramid();
		return false;
	}

	for (UINT m = 0; m < kAoDepthMips; ++m) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvM = {};
		rtvM.Format = DXGI_FORMAT_R32_FLOAT;
		rtvM.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rtvM.Texture2D.MipSlice = m;
		if (FAILED(m_dev->CreateRenderTargetView(m_aoDepthTex, &rtvM, &m_aoDepthMipRtv[m])) || !m_aoDepthMipRtv[m]) {
			ReleaseDepthPyramid();
			return false;
		}
		D3D11_SHADER_RESOURCE_VIEW_DESC srvM = {};
		srvM.Format = DXGI_FORMAT_R32_FLOAT;
		srvM.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvM.Texture2D.MostDetailedMip = m;
		srvM.Texture2D.MipLevels = 1;
		if (FAILED(m_dev->CreateShaderResourceView(m_aoDepthTex, &srvM, &m_aoDepthMipSrv[m])) || !m_aoDepthMipSrv[m]) {
			ReleaseDepthPyramid();
			return false;
		}
	}

	D3D11_TEXTURE2D_DESC pdtd = dtd;
	pdtd.MipLevels = 1;
	pdtd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(m_dev->CreateTexture2D(&pdtd, nullptr, &m_aoPrevDepthTex)) || !m_aoPrevDepthTex ||
	    FAILED(m_dev->CreateShaderResourceView(m_aoPrevDepthTex, nullptr, &m_aoPrevDepthSrv)) || !m_aoPrevDepthSrv) {
		ReleaseDepthPyramid();
		return false;
	}

	m_aoDepthW = w;
	m_aoDepthH = h;
	return true;
}

bool D3D11Presenter::BuildDepthPyramid(int slot, const FrameCameraParams &cam)
{
	if (!m_aoDepthLinPS || !m_aoDepthMipPS || !m_aoDepthCB || !m_sharedDepthSrv[slot] || m_gameW == 0 || m_gameH == 0)
		return false;
	if (!EnsureDepthPyramid(m_gameW, m_gameH))
		return false;

	ID3D11ShaderResourceView *nullSrv = nullptr;
	m_ctx->RSSetState(m_rsNoCull);
	m_ctx->OMSetBlendState(m_bsOpaque, nullptr, 0xFFFFFFFF);
	m_ctx->OMSetDepthStencilState(m_dsOff, 0);
	m_ctx->IASetInputLayout(nullptr);
	m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_ctx->VSSetShader(m_blitVS, nullptr, 0);
	m_ctx->PSSetSamplers(0, 1, &m_smpPoint);
	m_ctx->PSSetConstantBuffers(0, 1, &m_aoDepthCB);

	D3D11_VIEWPORT vpMip = { 0.0f, 0.0f, (float)m_gameW, (float)m_gameH, 0.0f, 1.0f };
	m_ctx->RSSetViewports(1, &vpMip);
	D3D11_MAPPED_SUBRESOURCE ms = {};
	if (SUCCEEDED(m_ctx->Map(m_aoDepthCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
		auto *cb = (AoDepthCBData *)ms.pData;
		cb->v0 = 1.0f / (float)m_gameW;
		cb->v1 = 1.0f / (float)m_gameH;
		cb->v2 = cam.nearZ;
		cb->v3 = cam.farZ;
		m_ctx->Unmap(m_aoDepthCB, 0);
	}
	m_ctx->OMSetRenderTargets(1, &m_aoDepthMipRtv[0], nullptr);
	m_ctx->PSSetShader(m_aoDepthLinPS, nullptr, 0);
	m_ctx->PSSetShaderResources(0, 1, &m_sharedDepthSrv[slot]);
	m_ctx->Draw(3, 0);
	m_ctx->PSSetShaderResources(0, 1, &nullSrv);

	m_ctx->PSSetShader(m_aoDepthMipPS, nullptr, 0);
	UINT parentW = m_gameW, parentH = m_gameH;
	for (UINT m = 1; m < kAoDepthMips; ++m) {
		UINT curW = (parentW > 1u) ? (parentW >> 1) : 1u;
		UINT curH = (parentH > 1u) ? (parentH >> 1) : 1u;
		vpMip.Width  = (float)curW;
		vpMip.Height = (float)curH;
		m_ctx->RSSetViewports(1, &vpMip);
		if (SUCCEEDED(m_ctx->Map(m_aoDepthCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
			auto *cb = (AoDepthCBData *)ms.pData;
			cb->v0 = 1.0f / (float)parentW;
			cb->v1 = 1.0f / (float)parentH;
			cb->v2 = 0.0f;
			cb->v3 = 0.0f;
			m_ctx->Unmap(m_aoDepthCB, 0);
		}
		m_ctx->OMSetRenderTargets(1, &m_aoDepthMipRtv[m], nullptr);
		m_ctx->PSSetShaderResources(0, 1, &m_aoDepthMipSrv[m - 1]);
		m_ctx->Draw(3, 0);
		m_ctx->PSSetShaderResources(0, 1, &nullSrv);
		parentW = curW;
		parentH = curH;
	}
	return true;
}

void D3D11Presenter::ReleaseAoTargets()
{
	for (int i = 0; i < 2; ++i) {
		SafeRelease(m_aoHistSrv[i]);
		SafeRelease(m_aoHistRtv[i]);
		SafeRelease(m_aoHistTex[i]);
	}
	m_aoOutputSrv = nullptr;
	m_aoHistIdx = 0;
	m_aoFrameIdx = 0;

	SafeRelease(m_aoFinalSrv);
	SafeRelease(m_aoFinalRtv);
	SafeRelease(m_aoFinalTex);
	SafeRelease(m_aoTmpSrv);
	SafeRelease(m_aoTmpRtv);
	SafeRelease(m_aoTmpTex);
	SafeRelease(m_aoRawSrv);
	SafeRelease(m_aoStencil);
	SafeRelease(m_prevRawTex);
	SafeRelease(m_prevRawRtv);
	SafeRelease(m_prevRawSrv);
	m_prevRawQuality = 0;
	SafeRelease(m_aoRawRtv);
	SafeRelease(m_aoRawTex);
	m_aoW = m_aoH = 0;
}

bool D3D11Presenter::EnsureAoTargets(UINT w, UINT h)
{
	if (w == 0 || h == 0 || !m_dev) return false;
	if (m_aoFinalSrv && m_aoW == w && m_aoH == h)
		return true;

	ReleaseAoTargets();

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	auto makeTarget = [&](ID3D11Texture2D *&tex, ID3D11RenderTargetView *&rtv, ID3D11ShaderResourceView *&srv) -> bool {
		if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &tex)) || !tex) return false;
		if (FAILED(m_dev->CreateRenderTargetView(tex, nullptr, &rtv)) || !rtv) return false;
		if (FAILED(m_dev->CreateShaderResourceView(tex, nullptr, &srv)) || !srv) return false;
		return true;
	};

	if (!makeTarget(m_aoRawTex, m_aoRawRtv, m_aoRawSrv) ||
	    !makeTarget(m_aoTmpTex, m_aoTmpRtv, m_aoTmpSrv) ||
	    !makeTarget(m_aoFinalTex, m_aoFinalRtv, m_aoFinalSrv) ||
	    !makeTarget(m_aoHistTex[0], m_aoHistRtv[0], m_aoHistSrv[0]) ||
	    !makeTarget(m_aoHistTex[1], m_aoHistRtv[1], m_aoHistSrv[1])) {
		ReleaseAoTargets();
		return false;
	}
	m_aoW = w;
	if (m_markReused && m_traceMissing && makeTarget(m_prevRawTex, m_prevRawRtv, m_prevRawSrv)) {
		td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
		td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
		ID3D11Texture2D *stencil = nullptr;
		if (SUCCEEDED(m_dev->CreateTexture2D(&td, nullptr, &stencil))) {
			m_dev->CreateDepthStencilView(stencil, nullptr, &m_aoStencil);
			stencil->Release();
		}
	}
	m_aoH = h;
	return true;
}

void D3D11Presenter::RenderXeGtao(int slot, int quality, float strength, const float reProj[16], bool useHist, const FrameCameraParams &cam)
{
	if (!m_aoPS || !m_aoBlurPS || !m_aoCB || !m_aoBlurCB || !m_aoDepthSrv || m_gameW == 0 || m_gameH == 0)
		return;

	const bool halfRes = (quality == 1);
	UINT aoW = halfRes ? ((m_gameW / 2) > 320u ? (m_gameW / 2) : 320u) : m_gameW;
	UINT aoH = halfRes ? ((m_gameH / 2) > 180u ? (m_gameH / 2) : 180u) : m_gameH;
	if (!EnsureAoTargets(aoW, aoH))
		return;

	static bool s_aoActiveLogged = false;
	if (!s_aoActiveLogged) {
		s_aoActiveLogged = true;
		Log("AO: D3D11 Temporal XeGTAO running (%ux%u, 5-mip depth, temporal=%d, q=%d, str=%.2f, cam near=%.2f far=%.0f px=%.2f py=%.2f)",
			aoW, aoH, cam.hasViewProj ? 1 : 0, quality, strength, cam.nearZ, cam.farZ, cam.projScaleX, cam.projScaleY);
	}

	ID3D11ShaderResourceView *nullSrvs[2] = {};
	D3D11_MAPPED_SUBRESOURCE ms = {};
	D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)aoW, (float)aoH, 0.0f, 1.0f };
	m_ctx->RSSetViewports(1, &vp);
	m_ctx->RSSetState(m_rsNoCull);
	m_ctx->OMSetBlendState(m_bsOpaque, nullptr, 0xFFFFFFFF);
	m_ctx->OMSetDepthStencilState(m_dsOff, 0);
	m_ctx->IASetInputLayout(nullptr);
	m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_ctx->VSSetShader(m_blitVS, nullptr, 0);

	ID3D11SamplerState *aoSmps[2] = { m_smpPoint, m_smpPointMipLin };
	m_ctx->PSSetSamplers(0, 2, aoSmps);

	const float baseMip = halfRes ? 1.0f : 0.0f;
	const bool canTemporal = (m_aoTemporalPS != nullptr && m_aoTemporalCB != nullptr &&
	                          m_aoHistSrv[0] != nullptr && m_aoHistSrv[1] != nullptr &&
	                          m_aoPrevDepthSrv != nullptr && cam.hasViewProj);
	if (SUCCEEDED(m_ctx->Map(m_aoCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
		auto *cb = (AoCBData *)ms.pData;
		cb->invDepthW    = 1.0f / (float)aoW;
		cb->invDepthH    = 1.0f / (float)aoH;
		cb->projScaleX   = cam.projScaleX;
		cb->projScaleY   = cam.projScaleY;
		cb->nearZ        = cam.nearZ;
		cb->farZ         = cam.farZ;
		cb->radiusWorld  = 1.20f;
		cb->strength     = strength;
		cb->quality      = (uint32_t)quality;
		cb->depthBaseMip = baseMip;
		cb->frameIndex   = (canTemporal && useHist) ? (m_aoFrameIdx + 1u) : 0u;
		cb->padAo        = 0.0f;
		m_ctx->Unmap(m_aoCB, 0);
	}
	bool reuse = m_aoStencil && canTemporal && useHist && !halfRes &&
	             quality == m_prevRawQuality && strength == m_prevRawStrength;
	if (reuse) {
		reuse = SUCCEEDED(m_ctx->Map(m_aoTemporalCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms));
		if (reuse) {
			auto *cb = static_cast<AoTemporalCBData *>(ms.pData);
			memcpy(cb->currToPrevClip, reProj, sizeof(cb->currToPrevClip));
			cb->invAoW = 1.0f / aoW; cb->invAoH = 1.0f / aoH;
			cb->prevNearZ = m_prevNearZ; cb->prevFarZ = m_prevFarZ;
			memcpy(&cb->padTemp, &m_aoFrameIdx, sizeof(m_aoFrameIdx));
			m_ctx->Unmap(m_aoTemporalCB, 0);
			m_ctx->ClearDepthStencilView(m_aoStencil, D3D11_CLEAR_STENCIL, 1, 0);
			m_ctx->OMSetRenderTargets(1, &m_aoRawRtv, m_aoStencil);
			m_ctx->OMSetDepthStencilState(m_markReused, 1);
			ID3D11ShaderResourceView *sources[] = { m_prevRawSrv, m_sharedDepthSrv[slot], m_aoDepthSrv, m_aoPrevDepthSrv };
			m_ctx->PSSetShaderResources(0, 4, sources);
			m_ctx->PSSetConstantBuffers(0, 1, &m_aoTemporalCB);
			m_ctx->PSSetShader(m_aoReusePS, nullptr, 0);
			m_ctx->Draw(3, 0);
			ID3D11ShaderResourceView *empty[4] = {};
			m_ctx->PSSetShaderResources(0, 4, empty);
			m_ctx->OMSetDepthStencilState(m_traceMissing, 0);
		}
	}
	m_ctx->OMSetRenderTargets(1, &m_aoRawRtv, reuse ? m_aoStencil : nullptr);
	m_ctx->PSSetShader(m_aoPS, nullptr, 0);
	m_ctx->PSSetConstantBuffers(0, 1, &m_aoCB);
	m_ctx->PSSetShaderResources(0, 1, &m_aoDepthSrv);
	m_ctx->Draw(3, 0);
	m_ctx->PSSetShaderResources(0, 2, nullSrvs);

	// Stage 2a: 2D 3x3 Diagonal-Bridging Cross-Bilateral Pre-Denoiser
	if (reuse) m_ctx->OMSetDepthStencilState(m_dsOff, 0);
	ID3D11SamplerState *blurSmps[2] = { m_smpPoint, m_smpLinear };
	m_ctx->PSSetSamplers(0, 2, blurSmps);
	ID3D11ShaderResourceView *hInputSrv = m_aoRawSrv;
	if (m_aoDenoise2dPS) {
		if (SUCCEEDED(m_ctx->Map(m_aoBlurCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
			auto *cb = (AoBlurCBData *)ms.pData;
			cb->texelStepX    = 1.0f / (float)aoW;
			cb->texelStepY    = 1.0f / (float)aoH;
			cb->depthBaseMip  = baseMip;
			cb->edgeSharpness = 1.65f;
			cb->padBlur[0] = cb->padBlur[1] = cb->padBlur[2] = cb->padBlur[3] = 0.0f;
			m_ctx->Unmap(m_aoBlurCB, 0);
		}
		ID3D11ShaderResourceView *d2Srvs[2] = { m_aoRawSrv, m_aoDepthSrv };
		m_ctx->OMSetRenderTargets(1, &m_aoFinalRtv, nullptr);
		m_ctx->PSSetShader(m_aoDenoise2dPS, nullptr, 0);
		m_ctx->PSSetConstantBuffers(0, 1, &m_aoBlurCB);
		m_ctx->PSSetShaderResources(0, 2, d2Srvs);
		m_ctx->Draw(3, 0);
		m_ctx->PSSetShaderResources(0, 2, nullSrvs);
		hInputSrv = m_aoFinalSrv;
	}

	// Stage 2b-H + 2b-V: Plane-Aware Bayer-Notch 9x9 Denoiser
	if (SUCCEEDED(m_ctx->Map(m_aoBlurCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
		auto *cb = (AoBlurCBData *)ms.pData;
		cb->texelStepX    = 1.0f / (float)aoW;
		cb->texelStepY    = 0.0f;
		cb->depthBaseMip  = baseMip;
		cb->edgeSharpness = 1.65f;
		cb->padBlur[0] = cb->padBlur[1] = cb->padBlur[2] = cb->padBlur[3] = 0.0f;
		m_ctx->Unmap(m_aoBlurCB, 0);
	}
	ID3D11ShaderResourceView *blurHSrvs[2] = { hInputSrv, m_aoDepthSrv };
	m_ctx->OMSetRenderTargets(1, &m_aoTmpRtv, nullptr);
	m_ctx->PSSetShader(m_aoBlurPS, nullptr, 0);
	m_ctx->PSSetConstantBuffers(0, 1, &m_aoBlurCB);
	m_ctx->PSSetShaderResources(0, 2, blurHSrvs);
	m_ctx->Draw(3, 0);
	m_ctx->PSSetShaderResources(0, 2, nullSrvs);

	if (SUCCEEDED(m_ctx->Map(m_aoBlurCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
		auto *cb = (AoBlurCBData *)ms.pData;
		cb->texelStepX    = 0.0f;
		cb->texelStepY    = 1.0f / (float)aoH;
		cb->depthBaseMip  = baseMip;
		cb->edgeSharpness = 1.65f;
		cb->padBlur[0] = cb->padBlur[1] = cb->padBlur[2] = cb->padBlur[3] = 0.0f;
		m_ctx->Unmap(m_aoBlurCB, 0);
	}
	ID3D11ShaderResourceView *blurVSrvs[2] = { m_aoTmpSrv, m_aoDepthSrv };
	m_ctx->OMSetRenderTargets(1, &m_aoFinalRtv, nullptr);
	m_ctx->PSSetShaderResources(0, 2, blurVSrvs);
	m_ctx->Draw(3, 0);
	m_ctx->PSSetShaderResources(0, 2, nullSrvs);

	// Stage 3: Camera-Reprojected Temporal Accumulation (TAA-AO)
	m_aoOutputSrv = m_aoFinalSrv;
	if (canTemporal) {
		const int readIdx  = m_aoHistIdx;
		const int writeIdx = m_aoHistIdx ^ 1;
		if (SUCCEEDED(m_ctx->Map(m_aoTemporalCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
			auto *cb = (AoTemporalCBData *)ms.pData;
			memcpy(cb->currToPrevClip, reProj, sizeof(cb->currToPrevClip));
			cb->invAoW       = 1.0f / (float)aoW;
			cb->invAoH       = 1.0f / (float)aoH;
			cb->prevNearZ    = m_prevNearZ;
			cb->prevFarZ     = m_prevFarZ;
			cb->depthBaseMip = baseMip;
			cb->hasHistory   = useHist ? 1u : 0u;
			cb->blendWeight  = 0.84f;
			cb->padTemp      = 0.0f;
			m_ctx->Unmap(m_aoTemporalCB, 0);
		}
		ID3D11ShaderResourceView *tSrvs[5] = {
			m_aoFinalSrv,
			m_aoHistSrv[readIdx],
			m_sharedDepthSrv[slot],
			m_aoDepthSrv,
			m_aoPrevDepthSrv
		};
		ID3D11ShaderResourceView *null5[5] = {};
		m_ctx->OMSetRenderTargets(1, &m_aoHistRtv[writeIdx], nullptr);
		m_ctx->PSSetShader(m_aoTemporalPS, nullptr, 0);
		m_ctx->PSSetConstantBuffers(0, 1, &m_aoTemporalCB);
		m_ctx->PSSetShaderResources(0, 5, tSrvs);
		m_ctx->Draw(3, 0);
		m_ctx->PSSetShaderResources(0, 5, null5);

		m_aoHistIdx = writeIdx;
		++m_aoFrameIdx;
		m_aoOutputSrv = m_aoHistSrv[writeIdx];
	}
	if (m_aoStencil) {
		std::swap(m_aoRawTex, m_prevRawTex);
		std::swap(m_aoRawRtv, m_prevRawRtv);
		std::swap(m_aoRawSrv, m_prevRawSrv);
		m_prevRawQuality = quality;
		m_prevRawStrength = strength;
	}
}

bool D3D11Presenter::CreateSsrPipeline()
{
	if (FAILED(m_dev->CreatePixelShader(kSsrPS, sizeof(kSsrPS), nullptr, &m_ssrPS))) return false;
	if (FAILED(m_dev->CreatePixelShader(kSsrBlurPS, sizeof(kSsrBlurPS), nullptr, &m_ssrBlurPS))) return false;
	D3D11_TEXTURE2D_DESC roadDesc = {};
	roadDesc.Width = roadDesc.Height = kRoadGridSize;
	roadDesc.MipLevels = roadDesc.ArraySize = roadDesc.SampleDesc.Count = 1;
	roadDesc.Format = DXGI_FORMAT_R32_FLOAT;
	roadDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(m_dev->CreatePixelShader(kRoadPuddlesPS, sizeof(kRoadPuddlesPS), nullptr, &m_roadPuddlesPS)) ||
	    FAILED(m_dev->CreateTexture2D(&roadDesc, nullptr, &m_roadGridTexture)) ||
	    FAILED(m_dev->CreateShaderResourceView(m_roadGridTexture, nullptr, &m_roadGridSrv)))
		Log("Rain: road puddle shader unavailable; using classic reflections");
	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = sizeof(SsrCBData);
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(m_dev->CreateBuffer(&cbd, nullptr, &m_ssrCB))) return false;
	return true;
}

void D3D11Presenter::ReleaseSsrTargets()
{
	SafeRelease(m_puddleSrv);
	SafeRelease(m_puddleRtv);
	m_puddleW = m_puddleH = 0;
	SafeRelease(m_ssrTmpSrv);
	SafeRelease(m_ssrTmpRtv);
	SafeRelease(m_ssrTmpTex);
	SafeRelease(m_ssrSrv);
	SafeRelease(m_ssrRtv);
	SafeRelease(m_ssrTex);
	m_ssrW = m_ssrH = 0;
}

bool D3D11Presenter::EnsureSsrTargets(UINT w, UINT h)
{
	if (w == 0 || h == 0 || !m_dev) return false;
	if (m_ssrSrv && m_ssrTmpSrv && m_ssrW == w && m_ssrH == h) return true;
	ReleaseSsrTargets();
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_ssrTex)) || !m_ssrTex ||
	    FAILED(m_dev->CreateRenderTargetView(m_ssrTex, nullptr, &m_ssrRtv)) || !m_ssrRtv ||
	    FAILED(m_dev->CreateShaderResourceView(m_ssrTex, nullptr, &m_ssrSrv)) || !m_ssrSrv ||
	    FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_ssrTmpTex)) || !m_ssrTmpTex ||
	    FAILED(m_dev->CreateRenderTargetView(m_ssrTmpTex, nullptr, &m_ssrTmpRtv)) || !m_ssrTmpRtv ||
	    FAILED(m_dev->CreateShaderResourceView(m_ssrTmpTex, nullptr, &m_ssrTmpSrv)) || !m_ssrTmpSrv) {
		ReleaseSsrTargets();
		return false;
	}
	m_ssrW = w;
	m_ssrH = h;
	return true;
}

bool D3D11Presenter::EnsurePuddleTarget()
{
	if (m_puddleW == m_gameW && m_puddleH == m_gameH) return m_puddleSrv != nullptr;
	m_puddleW = m_gameW;
	m_puddleH = m_gameH;
	SafeRelease(m_puddleSrv);
	SafeRelease(m_puddleRtv);
	D3D11_TEXTURE2D_DESC desc = {};
	desc.Width = m_gameW;
	desc.Height = m_gameH;
	desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
	desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	ID3D11Texture2D *texture = nullptr;
	if (FAILED(m_dev->CreateTexture2D(&desc, nullptr, &texture))) return false;
	bool ok = SUCCEEDED(m_dev->CreateRenderTargetView(texture, nullptr, &m_puddleRtv)) &&
	          SUCCEEDED(m_dev->CreateShaderResourceView(texture, nullptr, &m_puddleSrv));
	texture->Release();
	if (!ok) SafeRelease(m_puddleRtv);
	return ok;
}

ID3D11ShaderResourceView *D3D11Presenter::RenderSsr(int slot, int quality, bool use3dTex, bool rainFx, const FrameCameraParams &cam)
{
	if (!m_ssrPS || !m_ssrCB || !m_aoDepthSrv || !cam.hasViewProj || cam.inInterior || m_gameW == 0 || m_gameH == 0)
		return nullptr;
	ID3D11ShaderResourceView *colSrv = (use3dTex && m_shared3dSrv[slot]) ? m_shared3dSrv[slot] : m_sharedSrv[slot];
	if (!colSrv) return nullptr;

	UINT ssrW = (m_gameW / 2) > 320u ? (m_gameW / 2) : 320u;
	UINT ssrH = (m_gameH / 2) > 180u ? (m_gameH / 2) : 180u;
	if (!EnsureSsrTargets(ssrW, ssrH)) return nullptr;

	double viewD[16], invViewD[16], vpD[16], invVpD[16];
	for (int k = 0; k < 16; ++k) {
		viewD[k] = (double)cam.viewMat[k];
		vpD[k]   = (double)cam.viewProj[k];
	}
	if (!Invert4x4Double(viewD, invViewD) || !Invert4x4Double(vpD, invVpD)) return nullptr;

	// Only wet asphalt during actual rain (prevents dry foggy/Halloween concrete from turning into mirrors!)
	float wetAmt = 0.0f;
	if (cam.rain > 0.05f && !cam.sheltered)
		wetAmt = (cam.rain > cam.wetRoads) ? cam.rain : cam.wetRoads;
	else if (cam.wetRoads > 0.65f && !cam.sheltered)
		wetAmt = (cam.wetRoads - 0.65f) * 1.4f;
	if (wetAmt > 1.0f) wetAmt = 1.0f;
	const bool roadPuddles = rainFx && wetAmt > 0.01f && use3dTex && cam.hasRoadGrid &&
	                         m_roadPuddlesPS && m_roadGridSrv && m_sharedDepthSrv[slot];
	const auto &roadGrid = m_roadGrids[slot];

	D3D11_MAPPED_SUBRESOURCE ms = {};
	if (SUCCEEDED(m_ctx->Map(m_ssrCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
		auto *cb = (SsrCBData *)ms.pData;
		for (int k = 0; k < 16; ++k) {
			cb->invViewProj[k] = (float)invVpD[k];
			cb->viewProj[k]    = cam.viewProj[k];
		}
		cb->camPosW[0] = (float)invViewD[12];
		cb->camPosW[1] = (float)invViewD[13];
		cb->camPosW[2] = (float)invViewD[14];
		cb->nearZ      = cam.nearZ;
		cb->invW       = 1.0f / (float)ssrW;
		cb->invH       = 1.0f / (float)ssrH;
		cb->farZ       = cam.farZ;
		cb->rainWet    = wetAmt;
		cb->skyTop[0]  = cam.skyTop[0]; cb->skyTop[1] = cam.skyTop[1]; cb->skyTop[2] = cam.skyTop[2];
		cb->quality    = (uint32_t)quality | (rainFx ? 256u : 0u);
		cb->skyBot[0]  = cam.skyBot[0]; cb->skyBot[1] = cam.skyBot[1]; cb->skyBot[2] = cam.skyBot[2];
		cb->timeSec    = (float)(GetTickCount() & 0xFFFFFu) * 0.001f;
		cb->roadGridOrigin[0] = (float)roadGrid.originX;
		cb->roadGridOrigin[1] = (float)roadGrid.originY;
		cb->roadGridSpacing = kRoadGridSpacing;
		cb->unknownMaterialOffset = kUnknownMaterialOffset;
		memcpy(cb->actorCrop, cam.actorCrop, sizeof(cb->actorCrop));
		memcpy(cb->actorGround, cam.actorGround, sizeof(cb->actorGround));
		if (!m_actorSrv[slot]) cb->actorCrop[2] = 0;
		m_ctx->Unmap(m_ssrCB, 0);
	} else return nullptr;

	D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)ssrW, (float)ssrH, 0.0f, 1.0f };
	ID3D11SamplerState *smps[2] = { m_smpPoint, m_smpLinear };
	ID3D11ShaderResourceView *srvs[2] = { colSrv, m_aoDepthSrv };
	ID3D11ShaderResourceView *null2[2] = {};
	m_ctx->RSSetViewports(1, &vp);
	m_ctx->RSSetState(m_rsNoCull);
	m_ctx->OMSetBlendState(m_bsOpaque, nullptr, 0xFFFFFFFF);
	m_ctx->OMSetDepthStencilState(m_dsOff, 0);
	m_ctx->IASetInputLayout(nullptr);
	m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_ctx->VSSetShader(m_blitVS, nullptr, 0);
	m_ctx->OMSetRenderTargets(1, &m_ssrRtv, nullptr);
	m_ctx->PSSetShader(m_ssrPS, nullptr, 0);
	m_ctx->PSSetConstantBuffers(0, 1, &m_ssrCB);
	m_ctx->PSSetSamplers(0, 2, smps);
	m_ctx->PSSetShaderResources(0, 2, srvs);
	m_ctx->Draw(3, 0);
	m_ctx->PSSetShaderResources(0, 2, null2);

	// 2-pass depth-aware separable bilateral roughness filter (eliminates 100% of half-res pixelation & edge bleed!)
	if (m_ssrBlurPS && m_aoBlurCB && m_ssrTmpRtv && m_ssrTmpSrv) {
		if (SUCCEEDED(m_ctx->Map(m_aoBlurCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
			auto *cb = (AoBlurCBData *)ms.pData;
			cb->texelStepX = 1.0f / (float)ssrW;
			cb->texelStepY = 0.0f;
			cb->depthBaseMip = 1.0f;
			cb->edgeSharpness = 1.0f;
			cb->padBlur[0] = cb->padBlur[1] = cb->padBlur[2] = cb->padBlur[3] = 0.0f;
			m_ctx->Unmap(m_aoBlurCB, 0);
		}
		ID3D11ShaderResourceView *bh[2] = { m_ssrSrv, m_aoDepthSrv };
		m_ctx->OMSetRenderTargets(1, &m_ssrTmpRtv, nullptr);
		m_ctx->PSSetShader(m_ssrBlurPS, nullptr, 0);
		m_ctx->PSSetConstantBuffers(0, 1, &m_aoBlurCB);
		m_ctx->PSSetShaderResources(0, 2, bh);
		m_ctx->Draw(3, 0);
		m_ctx->PSSetShaderResources(0, 2, null2);

		if (SUCCEEDED(m_ctx->Map(m_aoBlurCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
			auto *cb = (AoBlurCBData *)ms.pData;
			cb->texelStepX = 0.0f;
			cb->texelStepY = 1.0f / (float)ssrH;
			cb->depthBaseMip = 1.0f;
			cb->edgeSharpness = 1.0f;
			cb->padBlur[0] = cb->padBlur[1] = cb->padBlur[2] = cb->padBlur[3] = 0.0f;
			m_ctx->Unmap(m_aoBlurCB, 0);
		}
		ID3D11ShaderResourceView *bv[2] = { m_ssrTmpSrv, m_ssrSrv ? m_aoDepthSrv : nullptr };
		m_ctx->OMSetRenderTargets(1, &m_ssrRtv, nullptr);
		m_ctx->PSSetShaderResources(0, 2, bv);
		m_ctx->Draw(3, 0);
		m_ctx->PSSetShaderResources(0, 2, null2);
	}

	if (roadPuddles) {
		const bool native = EnsurePuddleTarget();
		static bool loggedRoadPuddles = false;
		if (!loggedRoadPuddles) {
			loggedRoadPuddles = true;
			Log("Rain: road puddles active at %ux%u (wet=%.2f)", native ? m_gameW : ssrW, native ? m_gameH : ssrH, wetAmt);
		}
		if (roadGrid.revision != m_uploadedRoadGridRev || m_uploadedRoadGridRev == 0) {
			m_ctx->UpdateSubresource(m_roadGridTexture, 0, nullptr, roadGrid.heights, kRoadGridSize * sizeof(float), 0);
			m_uploadedRoadGridRev = roadGrid.revision;
		}
		ID3D11ShaderResourceView *roadSrvs[] = {
			colSrv, m_aoDepthSrv, m_sharedDepthSrv[slot], m_roadGridSrv, m_ssrSrv, m_actorSrv[slot]
		};
		if (native) {
			vp.Width = (float)m_gameW;
			vp.Height = (float)m_gameH;
			m_ctx->RSSetViewports(1, &vp);
		}
		m_ctx->OMSetRenderTargets(1, native ? &m_puddleRtv : &m_ssrTmpRtv, nullptr);
		m_ctx->PSSetShader(m_roadPuddlesPS, nullptr, 0);
		m_ctx->PSSetConstantBuffers(0, 1, &m_ssrCB);
		m_ctx->PSSetShaderResources(0, ARRAYSIZE(roadSrvs), roadSrvs);
		m_ctx->Draw(3, 0);
		ID3D11ShaderResourceView *nullRoadSrvs[ARRAYSIZE(roadSrvs)] = {};
		m_ctx->PSSetShaderResources(0, ARRAYSIZE(roadSrvs), nullRoadSrvs);
		return native ? m_puddleSrv : m_ssrTmpSrv;
	}
	return m_ssrSrv;
}

bool D3D11Presenter::CreateSsgiPipeline()
{
	if (FAILED(m_dev->CreatePixelShader(kSsgiPS, sizeof(kSsgiPS), nullptr, &m_ssgiPS))) return false;
	if (FAILED(m_dev->CreatePixelShader(kSsgiBlurPS, sizeof(kSsgiBlurPS), nullptr, &m_ssgiBlurPS))) return false;
	if (FAILED(m_dev->CreatePixelShader(kSsgiTemporalPS, sizeof(kSsgiTemporalPS), nullptr, &m_ssgiTemporalPS))) return false;
	return true;
}

void D3D11Presenter::ReleaseSsgiTargets()
{
	for (int i = 0; i < 2; ++i) {
		SafeRelease(m_ssgiHistSrv[i]);
		SafeRelease(m_ssgiHistRtv[i]);
		SafeRelease(m_ssgiHistTex[i]);
	}
	SafeRelease(m_ssgiTmpSrv);
	SafeRelease(m_ssgiTmpRtv);
	SafeRelease(m_ssgiTmpTex);
	SafeRelease(m_ssgiRawSrv);
	SafeRelease(m_ssgiRawRtv);
	SafeRelease(m_ssgiRawTex);
	m_ssgiOutputSrv = nullptr;
	m_ssgiHistIdx = 0;
	m_ssgiHasHist = false;
	m_ssgiFrameIdx = 0;
	m_ssgiW = m_ssgiH = 0;
}

bool D3D11Presenter::EnsureSsgiTargets(UINT w, UINT h)
{
	if (w == 0 || h == 0 || !m_dev) return false;
	if (m_ssgiRawSrv && m_ssgiTmpSrv && m_ssgiHistSrv[0] && m_ssgiHistSrv[1] && m_ssgiW == w && m_ssgiH == h)
		return true;
	ReleaseSsgiTargets();

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R11G11B10_FLOAT;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	auto makeT = [&](ID3D11Texture2D *&tex, ID3D11RenderTargetView *&rtv, ID3D11ShaderResourceView *&srv) -> bool {
		return SUCCEEDED(m_dev->CreateTexture2D(&td, nullptr, &tex)) && tex &&
		       SUCCEEDED(m_dev->CreateRenderTargetView(tex, nullptr, &rtv)) && rtv &&
		       SUCCEEDED(m_dev->CreateShaderResourceView(tex, nullptr, &srv)) && srv;
	};
	if (!makeT(m_ssgiRawTex, m_ssgiRawRtv, m_ssgiRawSrv) ||
	    !makeT(m_ssgiTmpTex, m_ssgiTmpRtv, m_ssgiTmpSrv) ||
	    !makeT(m_ssgiHistTex[0], m_ssgiHistRtv[0], m_ssgiHistSrv[0]) ||
	    !makeT(m_ssgiHistTex[1], m_ssgiHistRtv[1], m_ssgiHistSrv[1])) {
		ReleaseSsgiTargets();
		return false;
	}
	m_ssgiW = w;
	m_ssgiH = h;
	return true;
}

void D3D11Presenter::RenderSsgi(int slot, bool use3dTex, const float reProj[16], bool useHist, const FrameCameraParams &cam)
{
	m_ssgiOutputSrv = nullptr;
	if (!m_ssgiPS || !m_ssgiBlurPS || !m_aoCB || !m_aoBlurCB || !m_aoDepthSrv || m_gameW == 0 || m_gameH == 0)
		return;
	ID3D11ShaderResourceView *colSrv = (use3dTex && m_shared3dSrv[slot]) ? m_shared3dSrv[slot] : m_sharedSrv[slot];
	if (!colSrv) return;

	UINT giBaseW = m_gameW;
	UINT giBaseH = m_gameH;
	if (g_cfg.ssgiDisplayRes && (m_gameW > m_swapW || m_gameH > m_swapH)) {
		float x, y, width, height;
		ComputeDisplayViewport(m_gameW, m_gameH, m_swapW, m_swapH, g_cfg.scaling, x, y, width, height);
		giBaseW = (std::min)(giBaseW, (UINT)width);
		giBaseH = (std::min)(giBaseH, (UINT)height);
	}
	const UINT giW = (std::max)(320u, giBaseW / 2);
	const UINT giH = (std::max)(180u, giBaseH / 2);
	const float depthMip = (giBaseW == m_gameW && giBaseH == m_gameH) ? 1.0f :
	                      log2f((std::max)((float)m_gameW / giW, (float)m_gameH / giH));
	if (!EnsureSsgiTargets(giW, giH)) return;
	static bool loggedDisplayRes = false;
	if (!loggedDisplayRes && g_cfg.ssgiDisplayRes && (giBaseW != m_gameW || giBaseH != m_gameH)) {
		loggedDisplayRes = true;
		Log("SSGI: display-resolution target %ux%u (game %ux%u, depth mip %.2f)", giW, giH, m_gameW, m_gameH, depthMip);
	}

	D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)giW, (float)giH, 0.0f, 1.0f };
	ID3D11ShaderResourceView *null5[5] = {};
	m_ctx->RSSetViewports(1, &vp);
	m_ctx->RSSetState(m_rsNoCull);
	m_ctx->OMSetBlendState(m_bsOpaque, nullptr, 0xFFFFFFFF);
	m_ctx->OMSetDepthStencilState(m_dsOff, 0);
	m_ctx->IASetInputLayout(nullptr);
	m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_ctx->VSSetShader(m_blitVS, nullptr, 0);

	// Stage 1: Horizon-occluded 3D indirect color bounce gather (4x4 Bayer + 8-frame temporal rotation)
	D3D11_MAPPED_SUBRESOURCE ms = {};
	if (SUCCEEDED(m_ctx->Map(m_aoCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
		auto *cb = (AoCBData *)ms.pData;
		cb->invDepthW    = 1.0f / (float)giW;
		cb->invDepthH    = 1.0f / (float)giH;
		cb->projScaleX   = cam.projScaleX;
		cb->projScaleY   = cam.projScaleY;
		cb->nearZ        = cam.nearZ;
		cb->farZ         = cam.farZ;
		cb->radiusWorld  = 7.5f;
		cb->strength     = 1.0f;
		cb->quality      = 1u;
		cb->depthBaseMip = depthMip;
		cb->frameIndex   = m_ssgiFrameIdx;
		cb->padAo        = 0.0f;
		m_ctx->Unmap(m_aoCB, 0);
	}
	ID3D11SamplerState *giSmps[2] = { m_smpPointMipLin ? m_smpPointMipLin : m_smpPoint, m_smpLinear };
	ID3D11ShaderResourceView *giSrvs[2] = { colSrv, m_aoDepthSrv };
	m_ctx->OMSetRenderTargets(1, &m_ssgiRawRtv, nullptr);
	m_ctx->PSSetShader(m_ssgiPS, nullptr, 0);
	m_ctx->PSSetConstantBuffers(0, 1, &m_aoCB);
	m_ctx->PSSetSamplers(0, 2, giSmps);
	m_ctx->PSSetShaderResources(0, 2, giSrvs);
	m_ctx->Draw(3, 0);
	m_ctx->PSSetShaderResources(0, 2, null5);

	// Stage 2: 2-pass separable depth-aware bilateral denoiser (H + V)
	ID3D11SamplerState *blSmps[2] = { m_smpPoint, m_smpLinear };
	m_ctx->PSSetSamplers(0, 2, blSmps);
	m_ctx->PSSetShader(m_ssgiBlurPS, nullptr, 0);
	m_ctx->PSSetConstantBuffers(0, 1, &m_aoBlurCB);

	if (SUCCEEDED(m_ctx->Map(m_aoBlurCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
		auto *cb = (AoBlurCBData *)ms.pData;
		cb->texelStepX = 1.0f / (float)giW; cb->texelStepY = 0.0f;
		cb->depthBaseMip = depthMip; cb->edgeSharpness = 1.0f;
		cb->padBlur[0] = cb->padBlur[1] = cb->padBlur[2] = cb->padBlur[3] = 0.0f;
		m_ctx->Unmap(m_aoBlurCB, 0);
	}
	ID3D11ShaderResourceView *bh[2] = { m_ssgiRawSrv, m_aoDepthSrv };
	m_ctx->OMSetRenderTargets(1, &m_ssgiTmpRtv, nullptr);
	m_ctx->PSSetShaderResources(0, 2, bh);
	m_ctx->Draw(3, 0);
	m_ctx->PSSetShaderResources(0, 2, null5);

	if (SUCCEEDED(m_ctx->Map(m_aoBlurCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
		auto *cb = (AoBlurCBData *)ms.pData;
		cb->texelStepX = 0.0f; cb->texelStepY = 1.0f / (float)giH;
		cb->depthBaseMip = depthMip; cb->edgeSharpness = 1.0f;
		cb->padBlur[0] = cb->padBlur[1] = cb->padBlur[2] = cb->padBlur[3] = 0.0f;
		m_ctx->Unmap(m_aoBlurCB, 0);
	}
	ID3D11ShaderResourceView *bv[2] = { m_ssgiTmpSrv, m_aoDepthSrv };
	m_ctx->OMSetRenderTargets(1, &m_ssgiRawRtv, nullptr);
	m_ctx->PSSetShaderResources(0, 2, bv);
	m_ctx->Draw(3, 0);
	m_ctx->PSSetShaderResources(0, 2, null5);

	// Stage 3: Camera-reprojected temporal accumulation (100% smooth & flicker-free)
	m_ssgiOutputSrv = m_ssgiRawSrv;
	if (m_ssgiTemporalPS && m_aoTemporalCB && m_aoPrevDepthSrv && m_sharedDepthSrv[slot] && cam.hasViewProj) {
		const int readIdx  = m_ssgiHistIdx;
		const int writeIdx = m_ssgiHistIdx ^ 1;
		if (SUCCEEDED(m_ctx->Map(m_aoTemporalCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
			auto *cb = (AoTemporalCBData *)ms.pData;
			memcpy(cb->currToPrevClip, reProj, sizeof(cb->currToPrevClip));
			cb->invAoW       = 1.0f / (float)giW;
			cb->invAoH       = 1.0f / (float)giH;
			cb->prevNearZ    = m_prevNearZ;
			cb->prevFarZ     = m_prevFarZ;
			cb->depthBaseMip = depthMip;
			cb->hasHistory   = (useHist && m_ssgiHasHist) ? 1u : 0u;
			cb->blendWeight  = 0.86f;
			cb->padTemp      = 0.0f;
			m_ctx->Unmap(m_aoTemporalCB, 0);
		}
		ID3D11ShaderResourceView *ts[5] = {
			m_ssgiRawSrv,
			m_ssgiHistSrv[readIdx],
			m_sharedDepthSrv[slot],
			m_aoDepthSrv,
			m_aoPrevDepthSrv
		};
		m_ctx->OMSetRenderTargets(1, &m_ssgiHistRtv[writeIdx], nullptr);
		m_ctx->PSSetShader(m_ssgiTemporalPS, nullptr, 0);
		m_ctx->PSSetConstantBuffers(0, 1, &m_aoTemporalCB);
		m_ctx->PSSetShaderResources(0, 5, ts);
		m_ctx->Draw(3, 0);
		m_ctx->PSSetShaderResources(0, 5, null5);

		m_ssgiHistIdx = writeIdx;
		m_ssgiHasHist = true;
		++m_ssgiFrameIdx;
		m_ssgiOutputSrv = m_ssgiHistSrv[writeIdx];
	}
}

bool D3D11Presenter::CreateBloomPipeline()
{
	if (FAILED(m_dev->CreatePixelShader(kBloomPrePS, sizeof(kBloomPrePS), nullptr, &m_bloomPrePS))) return false;
	if (FAILED(m_dev->CreatePixelShader(kBloomDownPS, sizeof(kBloomDownPS), nullptr, &m_bloomDownPS))) return false;
	if (FAILED(m_dev->CreatePixelShader(kBloomUpPS, sizeof(kBloomUpPS), nullptr, &m_bloomUpPS))) return false;
	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = sizeof(BloomCBData);
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(m_dev->CreateBuffer(&cbd, nullptr, &m_bloomCB))) return false;
	return true;
}

void D3D11Presenter::ReleaseBloomTargets()
{
	for (UINT i = 0; i < kBloomMips; ++i) {
		SafeRelease(m_bloomDownSrv[i]);
		SafeRelease(m_bloomDownRtv[i]);
		SafeRelease(m_bloomDownTex[i]);
	}
	for (UINT i = 0; i < kBloomMips - 1; ++i) {
		SafeRelease(m_bloomUpSrv[i]);
		SafeRelease(m_bloomUpRtv[i]);
		SafeRelease(m_bloomUpTex[i]);
	}
	m_bloomW = m_bloomH = 0;
}

bool D3D11Presenter::EnsureBloomTargets(UINT w, UINT h)
{
	if (w < 64 || h < 64 || !m_dev) return false;
	if (m_bloomUpSrv[0] && m_bloomW == w && m_bloomH == h) return true;
	ReleaseBloomTargets();

	UINT mw = w >> 1, mh = h >> 1;
	for (UINT i = 0; i < kBloomMips; ++i) {
		UINT cw = mw > 4u ? mw : 4u;
		UINT ch = mh > 4u ? mh : 4u;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = cw; td.Height = ch; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
		if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_bloomDownTex[i])) || !m_bloomDownTex[i] ||
		    FAILED(m_dev->CreateRenderTargetView(m_bloomDownTex[i], nullptr, &m_bloomDownRtv[i])) || !m_bloomDownRtv[i] ||
		    FAILED(m_dev->CreateShaderResourceView(m_bloomDownTex[i], nullptr, &m_bloomDownSrv[i])) || !m_bloomDownSrv[i]) {
			ReleaseBloomTargets();
			return false;
		}
		if (i < kBloomMips - 1) {
			if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_bloomUpTex[i])) || !m_bloomUpTex[i] ||
			    FAILED(m_dev->CreateRenderTargetView(m_bloomUpTex[i], nullptr, &m_bloomUpRtv[i])) || !m_bloomUpRtv[i] ||
			    FAILED(m_dev->CreateShaderResourceView(m_bloomUpTex[i], nullptr, &m_bloomUpSrv[i])) || !m_bloomUpSrv[i]) {
				ReleaseBloomTargets();
				return false;
			}
		}
		mw >>= 1; mh >>= 1;
	}
	m_bloomW = w;
	m_bloomH = h;
	return true;
}

void D3D11Presenter::RenderBloom(int slot, bool use3dTex, bool flareOn)
{
	if (!m_bloomPrePS || !m_bloomDownPS || !m_bloomUpPS || !m_bloomCB || m_gameW == 0 || m_gameH == 0)
		return;
	ID3D11ShaderResourceView *srcSrv = (use3dTex && m_shared3dSrv[slot]) ? m_shared3dSrv[slot] : m_sharedSrv[slot];
	if (!srcSrv || !EnsureBloomTargets(m_gameW, m_gameH)) return;

	UINT mipW[kBloomMips], mipH[kBloomMips];
	UINT mw = m_gameW >> 1, mh = m_gameH >> 1;
	for (UINT i = 0; i < kBloomMips; ++i) {
		mipW[i] = mw > 4u ? mw : 4u;
		mipH[i] = mh > 4u ? mh : 4u;
		mw >>= 1; mh >>= 1;
	}

	ID3D11ShaderResourceView *null2[2] = {};
	m_ctx->RSSetState(m_rsNoCull);
	m_ctx->OMSetBlendState(m_bsOpaque, nullptr, 0xFFFFFFFF);
	m_ctx->OMSetDepthStencilState(m_dsOff, 0);
	m_ctx->IASetInputLayout(nullptr);
	m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_ctx->VSSetShader(m_blitVS, nullptr, 0);
	m_ctx->PSSetSamplers(0, 1, &m_smpLinear);
	m_ctx->PSSetConstantBuffers(0, 1, &m_bloomCB);

	// Prefilter (Karis average + soft knee bright-pass on 3D-only world)
	D3D11_MAPPED_SUBRESOURCE ms = {};
	if (SUCCEEDED(m_ctx->Map(m_bloomCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
		auto *cb = (BloomCBData *)ms.pData;
		cb->invSrcW   = 1.0f / (float)m_gameW;
		cb->invSrcH   = 1.0f / (float)m_gameH;
		cb->threshold = 0.88f;
		cb->knee      = 0.14f;
		cb->flareOn   = flareOn ? 1u : 0u;
		cb->padBloom[0] = cb->padBloom[1] = cb->padBloom[2] = 0.0f;
		m_ctx->Unmap(m_bloomCB, 0);
	}
	D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)mipW[0], (float)mipH[0], 0.0f, 1.0f };
	m_ctx->RSSetViewports(1, &vp);
	m_ctx->OMSetRenderTargets(1, &m_bloomDownRtv[0], nullptr);
	m_ctx->PSSetShader(m_bloomPrePS, nullptr, 0);
	m_ctx->PSSetShaderResources(0, 1, &srcSrv);
	m_ctx->Draw(3, 0);
	m_ctx->PSSetShaderResources(0, 1, null2);

	// Downsample 0 -> 1 -> 2 -> 3 -> 4
	m_ctx->PSSetShader(m_bloomDownPS, nullptr, 0);
	for (UINT i = 1; i < kBloomMips; ++i) {
		vp.Width = (float)mipW[i]; vp.Height = (float)mipH[i];
		m_ctx->RSSetViewports(1, &vp);
		if (SUCCEEDED(m_ctx->Map(m_bloomCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
			auto *cb = (BloomCBData *)ms.pData;
			cb->invSrcW = 1.0f / (float)mipW[i - 1];
			cb->invSrcH = 1.0f / (float)mipH[i - 1];
			cb->threshold = 0.0f; cb->knee = 0.0f; cb->flareOn = 0u;
			m_ctx->Unmap(m_bloomCB, 0);
		}
		m_ctx->OMSetRenderTargets(1, &m_bloomDownRtv[i], nullptr);
		m_ctx->PSSetShaderResources(0, 1, &m_bloomDownSrv[i - 1]);
		m_ctx->Draw(3, 0);
		m_ctx->PSSetShaderResources(0, 1, null2);
	}

	// Upsample 4 -> 3 -> 2 (with anamorphic flare) -> 1 -> 0
	m_ctx->PSSetShader(m_bloomUpPS, nullptr, 0);
	for (int i = (int)kBloomMips - 2; i >= 0; --i) {
		vp.Width = (float)mipW[i]; vp.Height = (float)mipH[i];
		m_ctx->RSSetViewports(1, &vp);
		ID3D11ShaderResourceView *lowSrv = (i == (int)kBloomMips - 2) ? m_bloomDownSrv[i + 1] : m_bloomUpSrv[i + 1];
		if (SUCCEEDED(m_ctx->Map(m_bloomCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
			auto *cb = (BloomCBData *)ms.pData;
			cb->invSrcW   = 1.0f / (float)mipW[i + 1];
			cb->invSrcH   = 1.0f / (float)mipH[i + 1];
			cb->threshold = 0.58f; // Energy-conserving lerp weight between high & low mip
			cb->knee      = (i == 2) ? 1.0f : 0.0f; // Anamorphic flare streak on 1/8 mip
			cb->flareOn   = flareOn ? 1u : 0u;
			m_ctx->Unmap(m_bloomCB, 0);
		}
		ID3D11ShaderResourceView *upSrvs[2] = { lowSrv, m_bloomDownSrv[i] };
		m_ctx->OMSetRenderTargets(1, &m_bloomUpRtv[i], nullptr);
		m_ctx->PSSetShaderResources(0, 2, upSrvs);
		m_ctx->Draw(3, 0);
		m_ctx->PSSetShaderResources(0, 2, null2);
	}
}

bool D3D11Presenter::CreateTaaPipeline()
{
	if (FAILED(m_dev->CreatePixelShader(kTaaPS, sizeof(kTaaPS), nullptr, &m_taaPS))) return false;
	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = sizeof(TaaCBData);
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(m_dev->CreateBuffer(&cbd, nullptr, &m_taaCB))) return false;
	return true;
}

void D3D11Presenter::ReleaseTaaTargets()
{
	for (int i = 0; i < 2; ++i) {
		SafeRelease(m_taaHistSrv[i]);
		SafeRelease(m_taaHistRtv[i]);
		SafeRelease(m_taaHistTex[i]);
	}
	m_taaOutputSrv = nullptr;
	m_taaHistIdx = 0;
	m_taaHasHist = false;
	m_taaW = m_taaH = 0;
}

bool D3D11Presenter::EnsureTaaTargets(UINT w, UINT h)
{
	if (w == 0 || h == 0 || !m_dev) return false;
	if (m_taaHistSrv[0] && m_taaHistSrv[1] && m_taaW == w && m_taaH == h) return true;
	ReleaseTaaTargets();

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	for (int i = 0; i < 2; ++i) {
		if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_taaHistTex[i])) || !m_taaHistTex[i] ||
		    FAILED(m_dev->CreateRenderTargetView(m_taaHistTex[i], nullptr, &m_taaHistRtv[i])) || !m_taaHistRtv[i] ||
		    FAILED(m_dev->CreateShaderResourceView(m_taaHistTex[i], nullptr, &m_taaHistSrv[i])) || !m_taaHistSrv[i]) {
			ReleaseTaaTargets();
			return false;
		}
	}
	m_taaW = w;
	m_taaH = h;
	return true;
}

void D3D11Presenter::RenderTaa(int slot, bool use3dTex, bool hasSmaa, const float reProj[16], bool useHist, const FrameCameraParams &cam)
{
	(void)cam;
	m_taaOutputSrv = nullptr;
	if (!m_taaPS || !m_taaCB || !m_aoDepthSrv || !m_aoPrevDepthSrv || !m_sharedDepthSrv[slot] || m_gameW == 0 || m_gameH == 0)
		return;
	ID3D11ShaderResourceView *currSrv = (use3dTex && m_shared3dSrv[slot]) ? m_shared3dSrv[slot] : m_sharedSrv[slot];
	if (!currSrv || !EnsureTaaTargets(m_gameW, m_gameH)) return;

	const int readIdx  = m_taaHistIdx;
	const int writeIdx = m_taaHistIdx ^ 1;
	const bool histReady = useHist && m_taaHasHist;

	D3D11_MAPPED_SUBRESOURCE ms = {};
	if (SUCCEEDED(m_ctx->Map(m_taaCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
		auto *cb = (TaaCBData *)ms.pData;
		memcpy(cb->currToPrevClip, reProj, sizeof(cb->currToPrevClip));
		cb->invW       = 1.0f / (float)m_gameW;
		cb->invH       = 1.0f / (float)m_gameH;
		cb->prevNearZ  = m_prevNearZ;
		cb->prevFarZ   = m_prevFarZ;
		cb->hasHistory = histReady ? 1u : 0u;
		cb->hasSmaa    = (hasSmaa && m_smaaWeightSrv) ? 1u : 0u;
		cb->baseBlend  = 0.84f;
		cb->padTaa     = 0.0f;
		m_ctx->Unmap(m_taaCB, 0);
	}

	D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)m_gameW, (float)m_gameH, 0.0f, 1.0f };
	ID3D11SamplerState *smps[2] = { m_smpPoint, m_smpLinear };
	ID3D11ShaderResourceView *srvs[6] = {
		currSrv,
		m_taaHistSrv[readIdx],
		(hasSmaa && m_smaaWeightSrv) ? m_smaaWeightSrv : nullptr,
		m_sharedDepthSrv[slot],
		m_aoDepthSrv,
		m_aoPrevDepthSrv
	};
	ID3D11ShaderResourceView *null6[6] = {};
	m_ctx->RSSetViewports(1, &vp);
	m_ctx->RSSetState(m_rsNoCull);
	m_ctx->OMSetBlendState(m_bsOpaque, nullptr, 0xFFFFFFFF);
	m_ctx->OMSetDepthStencilState(m_dsOff, 0);
	m_ctx->IASetInputLayout(nullptr);
	m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_ctx->VSSetShader(m_blitVS, nullptr, 0);
	m_ctx->OMSetRenderTargets(1, &m_taaHistRtv[writeIdx], nullptr);
	m_ctx->PSSetShader(m_taaPS, nullptr, 0);
	m_ctx->PSSetConstantBuffers(0, 1, &m_taaCB);
	m_ctx->PSSetSamplers(0, 2, smps);
	m_ctx->PSSetShaderResources(0, 6, srvs);
	m_ctx->Draw(3, 0);
	m_ctx->PSSetShaderResources(0, 6, null6);

	m_taaHistIdx = writeIdx;
	m_taaHasHist = true;
	m_taaOutputSrv = m_taaHistSrv[writeIdx];
}

void D3D11Presenter::WarmupPipelines()
{
	if (!m_dev || !m_ctx || !m_blitVS) return;
	// Prime driver shader microcode on a tiny 16x16 dummy target so live /md toggles & rain transitions have 0ms hitch
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = 16; td.Height = 16; td.MipLevels = 1; td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	ID3D11Texture2D *dummyTex = nullptr;
	ID3D11RenderTargetView *dummyRtv = nullptr;
	if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &dummyTex)) || !dummyTex) return;
	if (FAILED(m_dev->CreateRenderTargetView(dummyTex, nullptr, &dummyRtv)) || !dummyRtv) {
		SafeRelease(dummyTex);
		return;
	}

	D3D11_VIEWPORT vp = { 0.0f, 0.0f, 16.0f, 16.0f, 0.0f, 1.0f };
	m_ctx->OMSetRenderTargets(1, &dummyRtv, nullptr);
	m_ctx->RSSetViewports(1, &vp);
	m_ctx->RSSetState(m_rsNoCull);
	m_ctx->OMSetBlendState(m_bsOpaque, nullptr, 0xFFFFFFFF);
	m_ctx->OMSetDepthStencilState(m_dsOff, 0);
	m_ctx->IASetInputLayout(nullptr);
	m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_ctx->VSSetShader(m_blitVS, nullptr, 0);
	ID3D11SamplerState *smps[2] = { m_smpPoint, m_smpLinear };
	m_ctx->PSSetSamplers(0, 2, smps);

	ID3D11PixelShader *shaders[] = {
		m_postPS, m_aoDepthLinPS, m_aoDepthMipPS, m_aoPS, m_aoDenoise2dPS,
		m_aoBlurPS, m_aoTemporalPS, m_ssrPS, m_ssrBlurPS, m_ssgiPS, m_ssgiBlurPS,
		m_ssgiTemporalPS, m_bloomPrePS, m_bloomDownPS, m_bloomUpPS, m_taaPS,
		m_smaaEdgePS, m_smaaWeightPS
	};
	for (ID3D11PixelShader *ps : shaders) {
		if (!ps) continue;
		m_ctx->PSSetShader(ps, nullptr, 0);
		m_ctx->Draw(3, 0);
	}
	m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
	m_ctx->Flush();
	SafeRelease(dummyRtv);
	SafeRelease(dummyTex);
}

bool D3D11Presenter::CreateSmaaPipeline()
{
	if (FAILED(m_dev->CreatePixelShader(kSmaaEdgePS, sizeof(kSmaaEdgePS), nullptr, &m_smaaEdgePS))) return false;
	if (FAILED(m_dev->CreatePixelShader(kSmaaWeightPS, sizeof(kSmaaWeightPS), nullptr, &m_smaaWeightPS))) return false;

	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = sizeof(SmaaCBData);
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(m_dev->CreateBuffer(&cbd, nullptr, &m_smaaCB))) return false;
	return true;
}

void D3D11Presenter::ReleaseSmaaTargets()
{
	SafeRelease(m_smaaWeightSrv);
	SafeRelease(m_smaaWeightRtv);
	SafeRelease(m_smaaWeightTex);
	SafeRelease(m_smaaEdgeSrv);
	SafeRelease(m_smaaEdgeRtv);
	SafeRelease(m_smaaEdgeTex);
	m_smaaW = m_smaaH = 0;
}

bool D3D11Presenter::EnsureSmaaTargets(UINT w, UINT h)
{
	if (w == 0 || h == 0 || !m_dev) return false;
	if (m_smaaWeightSrv && m_smaaW == w && m_smaaH == h) return true;

	ReleaseSmaaTargets();
	D3D11_TEXTURE2D_DESC td = {};
	td.Width = w;
	td.Height = h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	td.Format = DXGI_FORMAT_R8G8_UNORM;
	if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_smaaEdgeTex)) || !m_smaaEdgeTex ||
	    FAILED(m_dev->CreateRenderTargetView(m_smaaEdgeTex, nullptr, &m_smaaEdgeRtv)) || !m_smaaEdgeRtv ||
	    FAILED(m_dev->CreateShaderResourceView(m_smaaEdgeTex, nullptr, &m_smaaEdgeSrv)) || !m_smaaEdgeSrv) {
		ReleaseSmaaTargets();
		return false;
	}

	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	if (FAILED(m_dev->CreateTexture2D(&td, nullptr, &m_smaaWeightTex)) || !m_smaaWeightTex ||
	    FAILED(m_dev->CreateRenderTargetView(m_smaaWeightTex, nullptr, &m_smaaWeightRtv)) || !m_smaaWeightRtv ||
	    FAILED(m_dev->CreateShaderResourceView(m_smaaWeightTex, nullptr, &m_smaaWeightSrv)) || !m_smaaWeightSrv) {
		ReleaseSmaaTargets();
		return false;
	}

	m_smaaW = w;
	m_smaaH = h;
	return true;
}

void D3D11Presenter::RenderSmaa(int slot, int quality, bool use3dTex, bool hasDepth, ID3D11ShaderResourceView *aoSrv, const FrameCameraParams &cam)
{
	if (!m_smaaEdgePS || !m_smaaWeightPS || !m_smaaCB || m_gameW == 0 || m_gameH == 0)
		return;
	ID3D11ShaderResourceView *srcSrv = (use3dTex && m_shared3dSrv[slot]) ? m_shared3dSrv[slot] : m_sharedSrv[slot];
	if (!srcSrv) return;
	if (!EnsureSmaaTargets(m_gameW, m_gameH))
		return;

	D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)m_gameW, (float)m_gameH, 0.0f, 1.0f };
	ID3D11ShaderResourceView *nullSrvs[3] = {};

	uint32_t edgeFlags = 0u;
	if (quality >= 2 && hasDepth && m_sharedDepthSrv[slot] != nullptr) edgeFlags |= 1u;
	if (aoSrv != nullptr) edgeFlags |= 2u;

	D3D11_MAPPED_SUBRESOURCE ms = {};
	if (SUCCEEDED(m_ctx->Map(m_smaaCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
		auto *cb = (SmaaCBData *)ms.pData;
		cb->invW        = 1.0f / (float)m_gameW;
		cb->invH        = 1.0f / (float)m_gameH;
		cb->threshold   = (quality >= 2) ? 0.075f : 0.10f;
		cb->depthThresh = 0.045f;
		cb->maxSearch   = (quality >= 2) ? 18u : 10u;
		cb->useDepth    = edgeFlags;
		cb->nearZ       = cam.nearZ;
		cb->farZ        = cam.farZ;
		m_ctx->Unmap(m_smaaCB, 0);
	}

	m_ctx->RSSetViewports(1, &vp);
	m_ctx->RSSetState(m_rsNoCull);
	m_ctx->OMSetBlendState(m_bsOpaque, nullptr, 0xFFFFFFFF);
	m_ctx->OMSetDepthStencilState(m_dsOff, 0);
	m_ctx->IASetInputLayout(nullptr);
	m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_ctx->VSSetShader(m_blitVS, nullptr, 0);
	m_ctx->PSSetSamplers(0, 1, &m_smpPoint);
	m_ctx->PSSetConstantBuffers(0, 1, &m_smaaCB);

	// Pass 1: Color + Depth + AO Edge Detection
	ID3D11ShaderResourceView *edgeSrvs[3] = {
		srcSrv,
		(edgeFlags & 1u) ? m_sharedDepthSrv[slot] : nullptr,
		(edgeFlags & 2u) ? aoSrv : nullptr
	};
	m_ctx->OMSetRenderTargets(1, &m_smaaEdgeRtv, nullptr);
	m_ctx->PSSetShader(m_smaaEdgePS, nullptr, 0);
	m_ctx->PSSetShaderResources(0, 3, edgeSrvs);
	m_ctx->Draw(3, 0);
	m_ctx->PSSetShaderResources(0, 3, nullSrvs);

	// Pass 2: Span Tracing & Analytical Area Blending Weights
	m_ctx->OMSetRenderTargets(1, &m_smaaWeightRtv, nullptr);
	m_ctx->PSSetShader(m_smaaWeightPS, nullptr, 0);
	m_ctx->PSSetShaderResources(0, 1, &m_smaaEdgeSrv);
	m_ctx->Draw(3, 0);
	m_ctx->PSSetShaderResources(0, 1, nullSrvs);
}

void D3D11Presenter::UpdateSwapFormatIfChanged()
{
	if (!m_swapChain || m_swapW == 0 || m_swapH == 0) return;
	DXGI_FORMAT wantFmt = g_cfg.output10Bit ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM;
	if (wantFmt == m_swapFormat) return;

	ReleaseBackBuffer();
	UINT flags = (m_hasWaitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0u) |
	             (m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
	HRESULT hr = m_swapChain->ResizeBuffers(3, m_swapW, m_swapH, wantFmt, flags);
	if (SUCCEEDED(hr)) {
		m_swapFormat = wantFmt;
		Log("DXGI: live swapchain format switched to %s",
			(m_swapFormat == DXGI_FORMAT_R10G10B10A2_UNORM) ? "10-bit (R10G10B10A2)" : "8-bit (B8G8R8A8)");
	} else {
		// Revert cleanly to previous format if 10-bit isn't supported
		m_swapChain->ResizeBuffers(3, m_swapW, m_swapH, m_swapFormat, flags);
		Log("DXGI: live format switch failed (0x%08lX), staying on %s", hr,
			(m_swapFormat == DXGI_FORMAT_R10G10B10A2_UNORM) ? "10-bit" : "8-bit");
	}
	AcquireBackBuffer();
}

void D3D11Presenter::UpdateLutIfChanged()
{
	if (!m_dev) return;
	std::string want(g_cfg.lutName);
	if (want == m_loadedLutName)
		return;
	m_loadedLutName = want;

	SafeRelease(m_lutSrv);
	SafeRelease(m_lutTex);
	m_lutSize = 0.0f;

	if (want.empty())
		return;

	Lut3DData lut;
	std::string resolved;
	if (!LoadCubeLutFile(want.c_str(), lut, &resolved) || lut.size < 2) {
		Log("LUT: failed to load '%s' (put .cube files in ModernDisplay\\luts\\)", want.c_str());
		return;
	}

	D3D11_TEXTURE3D_DESC td = {};
	td.Width = lut.size;
	td.Height = lut.size;
	td.Depth = lut.size;
	td.MipLevels = 1;
	td.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
	td.Usage = D3D11_USAGE_IMMUTABLE;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA init = {};
	init.pSysMem = lut.rgba16.data();
	init.SysMemPitch = lut.size * 4u * sizeof(uint16_t);
	init.SysMemSlicePitch = init.SysMemPitch * lut.size;

	if (SUCCEEDED(m_dev->CreateTexture3D(&td, &init, &m_lutTex)) && m_lutTex &&
	    SUCCEEDED(m_dev->CreateShaderResourceView(m_lutTex, nullptr, &m_lutSrv)) && m_lutSrv) {
		m_lutSize = (float)lut.size;
		Log("LUT: loaded 3D LUT '%s' (%ux%ux%u)", resolved.c_str(), lut.size, lut.size, lut.size);
	} else {
		SafeRelease(m_lutSrv);
		SafeRelease(m_lutTex);
		Log("LUT: D3D11 CreateTexture3D failed for '%s'", want.c_str());
	}
}

bool D3D11Presenter::CreateHudPipeline()
{
	if (FAILED(m_dev->CreateVertexShader(kHudVS, sizeof(kHudVS), nullptr, &m_hudVS))) return false;
	if (FAILED(m_dev->CreatePixelShader(kHudPS, sizeof(kHudPS), nullptr, &m_hudPS))) return false;

	const D3D11_INPUT_ELEMENT_DESC elems[] = {
		{ "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
		{ "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
	};
	if (FAILED(m_dev->CreateInputLayout(elems, ARRAYSIZE(elems), kHudVS, sizeof(kHudVS), &m_hudLayout))) return false;

	D3D11_BUFFER_DESC cbd = {};
	cbd.ByteWidth = sizeof(HudCBData);
	cbd.Usage = D3D11_USAGE_DYNAMIC;
	cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(m_dev->CreateBuffer(&cbd, nullptr, &m_hudCB))) return false;

	D3D11_BUFFER_DESC vbd = {};
	vbd.ByteWidth = sizeof(HudVertex) * kHudMaxVerts;
	vbd.Usage = D3D11_USAGE_DYNAMIC;
	vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
	vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	if (FAILED(m_dev->CreateBuffer(&vbd, nullptr, &m_hudVB))) return false;

	// Build 64*6 = 384 x 8 monochrome bitmap font atlas (R8_UNORM)
	constexpr UINT kAtlasW = 64 * 6;
	constexpr UINT kAtlasH = 8;
	uint8_t atlas[kAtlasW * kAtlasH] = {};
	for (int g = 0; g < 64; ++g) {
		for (int col = 0; col < 5; ++col) {
			uint8_t bits = kFont5x7[g][col];
			for (int row = 0; row < 7; ++row) {
				if (bits & (1u << row))
					atlas[row * kAtlasW + g * 6 + col] = 255;
			}
		}
	}
	D3D11_TEXTURE2D_DESC ftd = {};
	ftd.Width = kAtlasW;
	ftd.Height = kAtlasH;
	ftd.MipLevels = 1;
	ftd.ArraySize = 1;
	ftd.Format = DXGI_FORMAT_R8_UNORM;
	ftd.SampleDesc.Count = 1;
	ftd.Usage = D3D11_USAGE_IMMUTABLE;
	ftd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	D3D11_SUBRESOURCE_DATA initData = { atlas, kAtlasW, 0 };
	if (FAILED(m_dev->CreateTexture2D(&ftd, &initData, &m_fontTex))) return false;
	if (FAILED(m_dev->CreateShaderResourceView(m_fontTex, nullptr, &m_fontSrv))) return false;
	return true;
}

bool D3D11Presenter::AcquireBackBuffer()
{
	ReleaseBackBuffer();
	if (FAILED(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&m_backBuffer)) || !m_backBuffer)
		return false;
	if (FAILED(m_dev->CreateRenderTargetView(m_backBuffer, nullptr, &m_backBufferRtv)) || !m_backBufferRtv)
		return false;
	return true;
}

void D3D11Presenter::ReleaseBackBuffer()
{
	if (m_ctx) m_ctx->OMSetRenderTargets(0, nullptr, nullptr);
	SafeRelease(m_backBufferRtv);
	SafeRelease(m_backBuffer);
}

void D3D11Presenter::UnbindSharedTextures()
{
	DrainAsync();
	if (m_ctx) {
		ID3D11ShaderResourceView *nullSrvs[11] = {};
		m_ctx->PSSetShaderResources(0, ARRAYSIZE(nullSrvs), nullSrvs);
		m_ctx->Flush();
	}
	for (int i = 0; i < kNumSlots; ++i) {
		SafeRelease(m_sharedSrv[i]);
		SafeRelease(m_sharedTex[i]);
		SafeRelease(m_shared3dSrv[i]);
		SafeRelease(m_shared3dTex[i]);
		SafeRelease(m_actorSrv[i]);
		SafeRelease(m_actorTex[i]);
		SafeRelease(m_sharedDepthSrv[i]);
		SafeRelease(m_sharedDepthTex[i]);
	}
}

bool D3D11Presenter::BindSharedTextures(const HANDLE colorHandles[kNumSlots],
                                        const HANDLE world3dHandles[kNumSlots],
                                        const HANDLE depthHandles[kNumSlots], const HANDLE actorHandles[kNumSlots],
                                        UINT gameW, UINT gameH)
{
	UnbindSharedTextures();
	m_gameW = gameW;
	m_gameH = gameH;
	if (!m_dev || !colorHandles || !colorHandles[0]) return false;

	auto openSlot = [&](HANDLE h, ID3D11Texture2D *&tex, ID3D11ShaderResourceView *&srv) -> bool {
		if (!h) return true;
		HRESULT hr = m_dev->OpenSharedResource(h, __uuidof(ID3D11Texture2D), (void **)&tex);
		if (FAILED(hr) || !tex) return false;
		hr = m_dev->CreateShaderResourceView(tex, nullptr, &srv);
		if (FAILED(hr) || !srv) {
			SafeRelease(tex);
			return false;
		}
		return true;
	};

	for (int i = 0; i < kNumSlots; ++i) {
		if (!openSlot(colorHandles[i], m_sharedTex[i], m_sharedSrv[i])) {
			Log("D3D11: OpenSharedResource(color[%d]) failed", i);
			return false;
		}
		if (world3dHandles && world3dHandles[i])
			openSlot(world3dHandles[i], m_shared3dTex[i], m_shared3dSrv[i]);
		if (actorHandles && actorHandles[i])
			openSlot(actorHandles[i], m_actorTex[i], m_actorSrv[i]);
		if (depthHandles && depthHandles[i])
			openSlot(depthHandles[i], m_sharedDepthTex[i], m_sharedDepthSrv[i]);
	}
	return true;
}

bool D3D11Presenter::ResizeSwapChain(UINT swapW, UINT swapH)
{
	if (!m_swapChain || swapW == 0 || swapH == 0) return false;
	if (swapW == m_swapW && swapH == m_swapH) return true;

	DrainAsync();
	ReleaseBackBuffer();
	UINT flags = (m_hasWaitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0u) |
	             (m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
	HRESULT hr = m_swapChain->ResizeBuffers(3, swapW, swapH, m_swapFormat, flags);
	if (FAILED(hr)) {
		Log("DXGI: ResizeBuffers(%ux%u) failed (0x%08lX)", swapW, swapH, hr);
		return false;
	}
	m_swapW = swapW;
	m_swapH = swapH;
	return AcquireBackBuffer();
}

void D3D11Presenter::SetGammaRamp(const D3DGAMMARAMP &ramp)
{
	bool identity = true;
	for (int i = 0; i < 256; ++i) {
		int expected = (i << 8) | i;
		if (abs((int)ramp.red[i] - expected) > 384 ||
		    abs((int)ramp.green[i] - expected) > 384 ||
		    abs((int)ramp.blue[i] - expected) > 384) {
			identity = false;
			break;
		}
	}
	EnterCriticalSection(&m_gammaCs);
	m_gammaRamp = ramp;
	m_gammaIsIdentity = identity;
	m_gammaDirty = true;
	LeaveCriticalSection(&m_gammaCs);
}

static float LoadAtomicFloat(const volatile LONG *bits)
{
	LONG b = InterlockedCompareExchange(const_cast<volatile LONG *>(bits), 0, 0);
	float f = 0.0f;
	memcpy(&f, &b, sizeof(f));
	return f;
}

static void StoreAtomicFloat(volatile LONG *bits, float f)
{
	LONG b = 0;
	memcpy(&b, &f, sizeof(b));
	InterlockedExchange(bits, b);
}

bool D3D11Presenter::UploadGammaIfDirty()
{
	if (!m_gammaTex || !m_ctx) return false;
	D3DGAMMARAMP local = {};
	bool dirty = false;
	bool identity = true;
	EnterCriticalSection(&m_gammaCs);
	identity = m_gammaIsIdentity;
	if (!identity && m_gammaDirty) {
		local = m_gammaRamp;
		m_gammaDirty = false;
		dirty = true;
	}
	LeaveCriticalSection(&m_gammaCs);

	if (dirty) {
		D3D11_MAPPED_SUBRESOURCE ms = {};
		if (SUCCEEDED(m_ctx->Map(m_gammaTex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
			uint16_t *dst = (uint16_t *)ms.pData;
			for (int i = 0; i < 256; ++i) {
				dst[i * 4 + 0] = local.red[i];
				dst[i * 4 + 1] = local.green[i];
				dst[i * 4 + 2] = local.blue[i];
				dst[i * 4 + 3] = 0xFFFF;
			}
			m_ctx->Unmap(m_gammaTex, 0);
		}
	}
	return !identity;
}

void D3D11Presenter::WaitForSlot(int slot)
{
	if (!m_asyncCsInit || slot < 0 || slot >= kNumSlots) return;
	if (m_queueDevice) {
		while (WaitForSingleObject(m_slotReady[slot], 100) == WAIT_TIMEOUT) {
			if (FAILED(m_dev->GetDeviceRemovedReason())) break;
		}
		return;
	}
	EnterCriticalSection(&m_asyncCs);
	while (m_slotBusy[slot] && !m_stopThread)
		SleepConditionVariableCS(&m_cvDone, &m_asyncCs, 50);
	LeaveCriticalSection(&m_asyncCs);
}

bool D3D11Presenter::CanShareRenderTargets() const
{
	return m_thread && m_slotQuery[0] && m_slotQuery[1];
}

void D3D11Presenter::DrainAsync()
{
	if (!m_asyncCsInit) return;
	EnterCriticalSection(&m_asyncCs);
	while ((m_queued.slot >= 0 || m_inFlight) && !m_stopThread)
		SleepConditionVariableCS(&m_cvDone, &m_asyncCs, 50);
	LeaveCriticalSection(&m_asyncCs);
	if (m_queueDevice)
		for (int slot = 0; slot < kNumSlots; ++slot) WaitForSlot(slot);
}

void D3D11Presenter::SubmitFrame(int slot, uint64_t frameId, bool async, bool isMinimized,
                                 int64_t inputSampleQpc, bool has3dMask, bool hasDepth,
                                 const FrameCameraParams *cam, bool frontEnd)
{
	if (!m_swapChain || !m_backBuffer || slot < 0 || slot >= kNumSlots || !m_sharedTex[slot])
		return;

	if (isMinimized) {
		if (g_cfg.bgFpsLimit <= 0)
			Sleep(16);
		return;
	}

	QueuedFrame qf = {};
	qf.frontEnd  = frontEnd;
	qf.slot      = slot;
	qf.frameId   = frameId;
	qf.inputQpc  = inputSampleQpc;
	qf.has3dMask = has3dMask;
	qf.hasDepth  = hasDepth;
	if (cam) qf.cam = *cam;

	if (async && m_thread) {
		EnterCriticalSection(&m_asyncCs);
		// Wait if a previous unconsumed frame is still queued (keeps latency <= 1 frame)
		while (m_queued.slot >= 0 && !m_stopThread)
			SleepConditionVariableCS(&m_cvDone, &m_asyncCs, 50);
		m_slotBusy[slot] = true;
		if (m_queueDevice) ResetEvent(m_slotReady[slot]);
		m_queued = qf;
		WakeConditionVariable(&m_cvWork);
		LeaveCriticalSection(&m_asyncCs);
	} else {
		DrainAsync();
		if (m_queueDevice) ResetEvent(m_slotReady[slot]);
		PresentSlotOnD3D11(qf);
	}
}

DWORD WINAPI D3D11Presenter::PresentThreadProc(LPVOID param)
{
	static_cast<D3D11Presenter *>(param)->ThreadLoop();
	return 0;
}

void D3D11Presenter::ThreadLoop()
{
	HANDLE mmcss = SchedulerRegisterGamesThread();
	for (;;) {
		QueuedFrame qf;

		EnterCriticalSection(&m_asyncCs);
		while (m_queued.slot < 0 && !m_stopThread)
			SleepConditionVariableCS(&m_cvWork, &m_asyncCs, INFINITE);
		if (m_stopThread) {
			LeaveCriticalSection(&m_asyncCs);
			SchedulerUnregisterGamesThread(mmcss);
			return;
		}
		qf = m_queued;
		m_queued.slot = -1;
		m_inFlight = true;
		LeaveCriticalSection(&m_asyncCs);

		PresentSlotOnD3D11(qf);

		EnterCriticalSection(&m_asyncCs);
		m_slotBusy[qf.slot] = false;
		m_inFlight = false;
		WakeAllConditionVariable(&m_cvDone);
		LeaveCriticalSection(&m_asyncCs);
	}
}

void D3D11Presenter::RenderHudOverlay(bool showTelemetry, float compareSplitPx)
{
	if (!m_hudVS || !m_hudPS || !m_hudLayout || !m_hudCB || !m_hudVB || !m_fontSrv)
		return;
	if (!showTelemetry && compareSplitPx < 0.0f)
		return;

	int len = 0;
	if (showTelemetry) {
		DWORD now = GetTickCount();
		if (!m_hudLine[0] || (now - m_lastHudUpdateTick) >= 250) {
			m_lastHudUpdateTick = now;
			ReflexStats rs = m_reflex.GetStats();
			float emaMs = LoadAtomicFloat(&m_emaFrameMsBits);
			float fps = (emaMs > 0.01f) ? (1000.0f / emaMs) : 0.0f;
			const char *tag = m_reflex.ShortHudTag();
			if (rs.valid && rs.pcLatMs > 0.05f) {
				snprintf(m_hudLine, sizeof(m_hudLine), "%.0f FPS  %.1fMS  LAT %.1fMS  %s",
					fps, emaMs, rs.pcLatMs, tag);
			} else {
				snprintf(m_hudLine, sizeof(m_hudLine), "%.0f FPS  %.1fMS  %s",
					fps, emaMs, tag);
			}
		}
		len = (int)strlen(m_hudLine);
		if (len > 90) len = 90;
	}

	D3D11_MAPPED_SUBRESOURCE ms = {};
	if (FAILED(m_ctx->Map(m_hudVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)))
		return;

	auto *verts = (HudVertex *)ms.pData;
	int vCount = 0;
	const float scale = 2.0f; // Crisp 10x14 pixel-snapped monospace
	const float cellW = 6.0f * scale;
	const float glyphW = 5.0f * scale;
	const float glyphH = 7.0f * scale;
	constexpr float invAtlasW = 1.0f / (64.0f * 6.0f);
	constexpr float invAtlasH = 1.0f / 8.0f;

	auto emitQuad = [&](float x, float y, int glyphIdx, float r, float g, float b, float a) {
		if (vCount + 6 > kHudMaxVerts) return;
		float u0 = (glyphIdx * 6.0f) * invAtlasW;
		float u1 = (glyphIdx * 6.0f + 5.0f) * invAtlasW;
		float v0 = 0.0f;
		float v1 = 7.0f * invAtlasH;
		verts[vCount++] = { x,          y,          u0, v0, r, g, b, a };
		verts[vCount++] = { x + glyphW, y,          u1, v0, r, g, b, a };
		verts[vCount++] = { x,          y + glyphH, u0, v1, r, g, b, a };
		verts[vCount++] = { x + glyphW, y,          u1, v0, r, g, b, a };
		verts[vCount++] = { x + glyphW, y + glyphH, u1, v1, r, g, b, a };
		verts[vCount++] = { x,          y + glyphH, u0, v1, r, g, b, a };
	};

	static const float kOutOff[4][2] = { { -1.5f, 0.0f }, { 1.5f, 0.0f }, { 0.0f, -1.5f }, { 0.0f, 1.5f } };

	auto emitLabel = [&](const char *str, int slen, float x0, float y0,
	                     float cr, float cg, float cb, float ca, bool highlightNums) {
		for (int o = 0; o < 4; ++o) {
			for (int i = 0; i < slen; ++i) {
				unsigned char c = (unsigned char)str[i];
				if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
				if (c <= 32 || c > 95) continue;
				emitQuad(x0 + i * cellW + kOutOff[o][0], y0 + kOutOff[o][1], c - 32, 0.0f, 0.0f, 0.0f, 0.88f);
			}
		}
		for (int i = 0; i < slen; ++i) {
			unsigned char c = (unsigned char)str[i];
			if (c >= 'a' && c <= 'z') c = (unsigned char)(c - 32);
			if (c <= 32 || c > 95) continue;
			float r = cr, g = cg, b = cb;
			if (highlightNums && ((c >= '0' && c <= '9') || c == '.')) {
				r = 0.96f; g = 0.97f; b = 0.98f;
			}
			emitQuad(x0 + i * cellW, y0, c - 32, r, g, b, ca);
		}
	};

	// 1. Optional top-center bare-typography telemetry HUD (clear of SA-MP chat and GTA HUD)
	if (showTelemetry && len > 0) {
		const float totalW = len * cellW - scale;
		const float startX = floorf(((float)m_swapW - totalW) * 0.5f);
		emitLabel(m_hudLine, len, startX, 10.0f, 0.76f, 0.80f, 0.84f, 0.98f, true);
	}

	// 2. Editorial bare-typography split labels (Digital Foundry / NVIDIA ICAT restraint, zero boxes/pills)
	if (compareSplitPx >= 0.0f && m_swapH >= 64) {
		const float labelY = floorf((float)m_swapH - 28.0f);
		static const char kLeftLbl[]  = "VANILLA";
		static const char kRightLbl[] = "MODERNDISPLAY";
		constexpr int kLeftLen  = 7;
		constexpr int kRightLen = 13;
		const float leftW  = kLeftLen * cellW - scale;
		const float rightW = kRightLen * cellW - scale;
		const float leftX  = floorf(compareSplitPx - 14.0f - leftW);
		const float rightX = floorf(compareSplitPx + 15.0f);
		if (leftX >= 16.0f)
			emitLabel(kLeftLbl, kLeftLen, leftX, labelY, 0.76f, 0.80f, 0.84f, 0.94f, false);
		if (rightX + rightW <= (float)m_swapW - 16.0f)
			emitLabel(kRightLbl, kRightLen, rightX, labelY, 0.96f, 0.97f, 0.99f, 0.98f, false);
	}

	m_ctx->Unmap(m_hudVB, 0);
	if (vCount == 0) return;

	if (SUCCEEDED(m_ctx->Map(m_hudCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
		auto *cb = (HudCBData *)ms.pData;
		cb->invSwapW = 1.0f / (float)m_swapW;
		cb->invSwapH = 1.0f / (float)m_swapH;
		cb->pad[0] = cb->pad[1] = 0.0f;
		m_ctx->Unmap(m_hudCB, 0);
	}

	D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)m_swapW, (float)m_swapH, 0.0f, 1.0f };
	UINT stride = sizeof(HudVertex), offset = 0;
	m_ctx->OMSetRenderTargets(1, &m_backBufferRtv, nullptr);
	m_ctx->RSSetViewports(1, &vp);
	m_ctx->RSSetState(m_rsNoCull);
	m_ctx->OMSetBlendState(m_bsAlpha, nullptr, 0xFFFFFFFF);
	m_ctx->OMSetDepthStencilState(m_dsOff, 0);
	m_ctx->IASetInputLayout(m_hudLayout);
	m_ctx->IASetVertexBuffers(0, 1, &m_hudVB, &stride, &offset);
	m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	m_ctx->VSSetShader(m_hudVS, nullptr, 0);
	m_ctx->VSSetConstantBuffers(0, 1, &m_hudCB);
	m_ctx->PSSetShader(m_hudPS, nullptr, 0);
	m_ctx->PSSetSamplers(0, 1, &m_smpPoint);
	m_ctx->PSSetShaderResources(0, 1, &m_fontSrv);
	m_ctx->Draw((UINT)vCount, 0);

	ID3D11ShaderResourceView *nullSrv = nullptr;
	m_ctx->PSSetShaderResources(0, 1, &nullSrv);
}

void D3D11Presenter::PresentSlotOnD3D11(const QueuedFrame &qf)
{
	const int slot = qf.slot;
	if (!m_swapChain || !m_backBuffer || slot < 0 || slot >= kNumSlots || !m_sharedTex[slot])
		return;

	LONG seq = InterlockedCompareExchange(&g_cfgSeq, 0, 0);
	if (seq != m_appliedCfgSeq) {
		m_appliedCfgSeq = seq;
		m_reflex.ApplyMode(g_cfg.reflex);
		UpdateLutIfChanged();
		UpdateSwapFormatIfChanged();
	}

	m_reflex.BeginGpuWork(qf.frameId, qf.inputQpc);

	bool gammaActive = g_cfg.gammaLut && UploadGammaIfDirty();
	ScalingMode scaleMode = g_cfg.scaling;
	const bool world = !qf.frontEnd;
	float sharp = world ? g_cfg.sharpness : 0.0f;
	float vib = world ? g_cfg.vibrance : 0;
	bool overlay = g_cfg.overlay;
	bool deband = world && g_cfg.deband;
	int aoQuality = g_cfg.aoQuality;
	float aoStrength = g_cfg.aoStrength;
	int aaQuality = world ? g_cfg.aaQuality : 0;
	int ssrQuality = g_cfg.ssrQuality;
	float giStr = g_cfg.giStrength;
	bool rainFx = world && g_cfg.rainFx;
	float bloomStr = world ? g_cfg.bloomStrength : 0;
	bool bloomFlare = g_cfg.bloomFlare;
	float fogStr = world ? g_cfg.fogStrength : 0;
	int tonemapMode = world ? g_cfg.tonemapMode : 0;
	float clarity = world ? g_cfg.clarity : 0;
	bool has3dMask = world && qf.has3dMask && m_shared3dSrv[slot] && (g_cfg.depthCas || (rainFx && qf.cam.hasRoadGrid));
	bool hasLut = world && (m_lutSrv != nullptr && m_lutSize >= 2.0f);
	bool hasDepth = world && qf.hasDepth && (m_sharedDepthSrv[slot] != nullptr);

	bool wantAo    = (aoQuality > 0 && hasDepth && m_aoPS != nullptr);
	bool wantSsr   = (ssrQuality > 0 && hasDepth && qf.cam.hasViewProj && !qf.cam.inInterior && m_ssrPS != nullptr);
	bool wantSsgi  = (giStr > 0.001f && hasDepth && m_ssgiPS != nullptr);
	bool wantTaa   = (aaQuality >= 3 && hasDepth && qf.cam.hasViewProj && m_taaPS != nullptr);
	bool wantBloom = (bloomStr > 0.001f && m_bloomPrePS != nullptr);
	bool wantDepthPyr = wantAo || wantSsr || wantSsgi || wantTaa;

	bool depthPyrReady = wantDepthPyr && BuildDepthPyramid(slot, qf.cam);

	// Compute shared exact 64-bit currClip -> prevClip reprojection matrix ONCE for Temporal XeGTAO, SSGI & SMAA T2x
	double currVP[16] = {};
	float reProjF[16] = {};
	bool validCurrVP = false;
	bool useTempHist = false;
	if (depthPyrReady && (wantAo || wantSsgi || wantTaa) && qf.cam.hasViewProj) {
		double invCurrVP[16] = {};
		for (int k = 0; k < 16; ++k) currVP[k] = (double)qf.cam.viewProj[k];
		validCurrVP = Invert4x4Double(currVP, invCurrVP);
		useTempHist = m_hasPrevViewProj && validCurrVP;
		if (useTempHist) {
			for (int r = 0; r < 4; ++r) {
				for (int c = 0; c < 4; ++c) {
					double s = 0.0;
					for (int k = 0; k < 4; ++k)
						s += invCurrVP[r * 4 + k] * m_prevViewProj[k * 4 + c];
					reProjF[r * 4 + c] = (float)s;
				}
			}
		} else {
			for (int r = 0; r < 4; ++r)
				for (int c = 0; c < 4; ++c)
					reProjF[r * 4 + c] = (r == c) ? 1.0f : 0.0f;
		}
	}

	if (wantAo && depthPyrReady)
		RenderXeGtao(slot, aoQuality, aoStrength, reProjF, useTempHist, qf.cam);
	else
		m_prevRawQuality = 0;
	bool hasAo = wantAo && depthPyrReady && (m_aoOutputSrv != nullptr);

	ID3D11ShaderResourceView *ssrOutput = (wantSsr && depthPyrReady) ?
		RenderSsr(slot, ssrQuality, has3dMask, rainFx, qf.cam) : nullptr;
	bool hasSsr = ssrOutput != nullptr;

	if (wantSsgi && depthPyrReady)
		RenderSsgi(slot, has3dMask, reProjF, useTempHist, qf.cam);
	else
		m_ssgiHasHist = false;
	bool hasSsgi = wantSsgi && depthPyrReady && (m_ssgiOutputSrv != nullptr);

	bool runSmaa = (aaQuality > 0 && m_smaaEdgePS != nullptr && m_smaaWeightPS != nullptr);
	if (runSmaa)
		RenderSmaa(slot, (aaQuality >= 2) ? 2 : 1, has3dMask, hasDepth, hasAo ? m_aoOutputSrv : nullptr, qf.cam);
	bool hasSmaa = runSmaa && (m_smaaWeightSrv != nullptr);

	if (wantTaa && depthPyrReady)
		RenderTaa(slot, has3dMask, hasSmaa, reProjF, useTempHist, qf.cam);
	else
		m_taaHasHist = false;
	bool hasTaa = wantTaa && depthPyrReady && (m_taaOutputSrv != nullptr);

	// Save current Mip 0 linear depth + View*Proj ONCE after all temporal passes complete
	if (depthPyrReady && (wantAo || wantSsgi || wantTaa) && m_aoPrevDepthTex && m_aoDepthTex) {
		m_ctx->CopySubresourceRegion(m_aoPrevDepthTex, 0, 0, 0, 0, m_aoDepthTex, 0, nullptr);
		if (validCurrVP) {
			for (int k = 0; k < 16; ++k) m_prevViewProj[k] = currVP[k];
			m_prevNearZ = qf.cam.nearZ;
			m_prevFarZ  = qf.cam.farZ;
			m_hasPrevViewProj = true;
		} else {
			m_hasPrevViewProj = false;
		}
	} else if (!wantAo && !wantSsgi && !wantTaa) {
		m_hasPrevViewProj = false;
	}

	if (wantBloom)
		RenderBloom(slot, has3dMask, bloomFlare);
	bool hasBloom = wantBloom && (m_bloomUpSrv[0] != nullptr);

	// Screen raindrops intensity: strictly in real outdoor rain, attenuated gently when looking straight down
	float rainDropAmt = 0.0f;
	if (rainFx && g_cfg.rainLensDrops && !qf.cam.inInterior && !qf.cam.sheltered && qf.cam.rain > 0.08f) {
		float pitchGate = (qf.cam.camPitchZ + 0.60f) / 0.85f;
		if (pitchGate < 0.15f) pitchGate = 0.15f;
		if (pitchGate > 1.0f)  pitchGate = 1.0f;
		rainDropAmt = qf.cam.rain * pitchGate * 0.70f;
		if (rainDropAmt > 1.0f) rainDropAmt = 1.0f;
	}
	bool hasRainDrops = (rainDropAmt > 0.04f);
	static bool loggedRainDrops = false;
	if (hasRainDrops && !loggedRainDrops) {
		loggedRainDrops = true;
		Log("Rain: lens droplets active (depth=%d)", hasDepth);
	}

	// Live Before/After split-screen wipe position (0..1 within active viewport, < 0 when OFF)
	int compareMode = world ? g_cfg.compareMode : 0;
	float compareSplitNorm = -1.0f;
	if (compareMode == 1) {
		compareSplitNorm = g_cfg.comparePos;
		if (compareSplitNorm < 0.02f) compareSplitNorm = 0.02f;
		if (compareSplitNorm > 0.98f) compareSplitNorm = 0.98f;
	} else if (compareMode == 2) {
		// Smooth cosine ease-in-out sweep (12% <-> 88%, ~6.0s round-trip)
		double sec = (double)GetTickCount64() * 0.001;
		compareSplitNorm = 0.50f - 0.38f * cosf((float)fmod(sec * 1.05, 6.283185307));
	} else if (compareMode == 3) {
		POINT pt = {}, origin = { 0, 0 };
		if (m_hwnd && m_swapW > 0 && GetCursorPos(&pt) && ClientToScreen(m_hwnd, &origin)) {
			float norm = (float)(pt.x - origin.x) / (float)m_swapW;
			if (norm < 0.03f) norm = 0.03f;
			if (norm > 0.97f) norm = 0.97f;
			compareSplitNorm = norm;
		} else {
			compareSplitNorm = g_cfg.comparePos;
		}
	}
	float compareSplitPx = -1.0f;

	bool is10Bit = (m_swapFormat == DXGI_FORMAT_R10G10B10A2_UNORM);
	bool needShader = is10Bit || (m_gameW != m_swapW || m_gameH != m_swapH) ||
	                  (sharp > 0.001f) || (fabsf(vib) > 0.001f) || (clarity > 0.001f) ||
	                  (fogStr > 0.001f) || (tonemapMode > 0) || (compareSplitNorm >= 0.0f) ||
	                  gammaActive || deband || hasAo || hasSsr || hasSsgi || hasRainDrops || hasBloom || hasTaa || hasSmaa || hasLut ||
	                  (scaleMode != ScalingMode::Stretch);

	if (!needShader) {
		m_ctx->CopyResource(m_backBuffer, m_sharedTex[slot]);
	} else {
		D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)m_swapW, (float)m_swapH, 0.0f, 1.0f };
		bool usePoint = false;
		bool needBars = false;
		ComputeDisplayViewport(m_gameW, m_gameH, m_swapW, m_swapH, scaleMode,
		                       vp.TopLeftX, vp.TopLeftY, vp.Width, vp.Height,
		                       &usePoint, &needBars);
		if (compareSplitNorm >= 0.0f)
			compareSplitPx = floorf(vp.TopLeftX + compareSplitNorm * vp.Width);

		if (needBars) {
			const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
			m_ctx->ClearRenderTargetView(m_backBufferRtv, black);
		}

		bool bicubicSsaa = !usePoint && (m_gameW > m_swapW || m_gameH > m_swapH);
		uint32_t postFlags = 0;
		if (has3dMask)         postFlags |= 1u;
		if (gammaActive)       postFlags |= 2u;
		if (usePoint)          postFlags |= 4u;
		if (deband)            postFlags |= 8u;
		if (bicubicSsaa)       postFlags |= 16u;
		if (hasAo)             postFlags |= 32u;
		if (hasLut)            postFlags |= 64u;
		if (g_cfg.depthCas)    postFlags |= 128u;
		if (hasSmaa)           postFlags |= 256u;
		if (hasDepth)          postFlags |= 512u;
		if (hasSsr)            postFlags |= 1024u;
		if (hasTaa)            postFlags |= 2048u;
		if (hasBloom)          postFlags |= 4096u;
		if (qf.cam.inInterior) postFlags |= 8192u;
		if (hasSsgi)           postFlags |= 16384u;
		if (hasRainDrops)      postFlags |= 32768u;

		// Transform world sun direction into camera view space (aligned with screen +X/+Y/+Z) for Mie forward scattering
		float sunV[3] = { 0.0f, 0.45f, 0.89f };
		if (qf.cam.hasViewProj) {
			const float *v = qf.cam.viewMat;
			const float *sw = qf.cam.sunDirWorld;
			float sx = (sw[0] * v[0] + sw[1] * v[4] + sw[2] * v[8]) * qf.cam.projSignX;
			float sy = sw[0] * v[1] + sw[1] * v[5] + sw[2] * v[9];
			float sz = sw[0] * v[2] + sw[1] * v[6] + sw[2] * v[10];
			float sl = sqrtf(sx * sx + sy * sy + sz * sz);
			if (sl > 1e-4f) { sunV[0] = sx / sl; sunV[1] = sy / sl; sunV[2] = sz / sl; }
		}

		D3D11_MAPPED_SUBRESOURCE ms = {};
		if (SUCCEEDED(m_ctx->Map(m_postCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
			auto *cb = (PostCBData *)ms.pData;
			cb->invSrcW       = (m_gameW > 0) ? (1.0f / (float)m_gameW) : (1.0f / 1920.0f);
			cb->invSrcH       = (m_gameH > 0) ? (1.0f / (float)m_gameH) : (1.0f / 1080.0f);
			cb->invDstW       = (m_swapW > 0) ? (1.0f / (float)m_swapW) : (1.0f / 1920.0f);
			cb->invDstH       = (m_swapH > 0) ? (1.0f / (float)m_swapH) : (1.0f / 1080.0f);
			cb->sharpness     = sharp;
			cb->vibrance      = vib;
			cb->aoStrength    = hasAo ? aoStrength : 0.0f;
			cb->lutSize       = hasLut ? m_lutSize : 0.0f;
			cb->flags         = postFlags;
			cb->debandThresh  = 0.026f;
			cb->ditherScale   = 1.0f / 255.0f;
			cb->bloomStrength = hasBloom ? bloomStr : 0.0f;
			cb->fogStrength   = hasDepth ? fogStr : 0.0f;
			cb->clarity       = clarity;
			cb->tonemapMode   = (uint32_t)tonemapMode;
			cb->weatherFog    = qf.cam.weatherFog;
			cb->skyBot[0]     = qf.cam.skyBot[0]; cb->skyBot[1] = qf.cam.skyBot[1]; cb->skyBot[2] = qf.cam.skyBot[2];
			cb->nearZ         = qf.cam.nearZ;
			cb->skyTop[0]     = qf.cam.skyTop[0]; cb->skyTop[1] = qf.cam.skyTop[1]; cb->skyTop[2] = qf.cam.skyTop[2];
			cb->farZ          = qf.cam.farZ;
			cb->sunDirView[0] = sunV[0]; cb->sunDirView[1] = sunV[1]; cb->sunDirView[2] = sunV[2];
			cb->fogStart      = qf.cam.fogStart;
			cb->projScaleX    = qf.cam.projScaleX;
			cb->projScaleY    = qf.cam.projScaleY;
			cb->giStrength    = hasSsgi ? giStr : 0.0f;
			cb->rainDropAmt   = rainDropAmt;
			cb->timeSec        = (float)(GetTickCount() & 0xFFFFFu) * 0.001f;
			cb->camPitchZ      = qf.cam.camPitchZ;
			cb->compareSplitPx = compareSplitPx;
			cb->padPost3       = 0.0f;
			m_ctx->Unmap(m_postCB, 0);
		}

		ID3D11SamplerState *smps[2] = { m_smpLinear, m_smpPoint };
		ID3D11ShaderResourceView *srvs[11] = {
			m_sharedSrv[slot],
			m_gammaSrv,
			has3dMask ? m_shared3dSrv[slot] : nullptr,
			hasAo ? m_aoOutputSrv : nullptr,
			hasLut ? m_lutSrv : nullptr,
			hasSmaa ? m_smaaWeightSrv : nullptr,
			hasDepth ? m_sharedDepthSrv[slot] : nullptr,
			ssrOutput,
			hasBloom ? m_bloomUpSrv[0] : nullptr,
			hasTaa ? m_taaOutputSrv : nullptr,
			hasSsgi ? m_ssgiOutputSrv : nullptr,
		};

		m_ctx->OMSetRenderTargets(1, &m_backBufferRtv, nullptr);
		m_ctx->RSSetViewports(1, &vp);
		m_ctx->RSSetState(m_rsNoCull);
		m_ctx->OMSetBlendState(m_bsOpaque, nullptr, 0xFFFFFFFF);
		m_ctx->OMSetDepthStencilState(m_dsOff, 0);
		m_ctx->IASetInputLayout(nullptr);
		m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		m_ctx->VSSetShader(m_blitVS, nullptr, 0);
		m_ctx->PSSetShader(m_postPS, nullptr, 0);
		m_ctx->PSSetConstantBuffers(0, 1, &m_postCB);
		m_ctx->PSSetSamplers(0, 2, smps);
		m_ctx->PSSetShaderResources(0, 11, srvs);
		m_ctx->Draw(3, 0);

		ID3D11ShaderResourceView *nullSrvs[11] = {};
		m_ctx->PSSetShaderResources(0, 11, nullSrvs);
	}
	if (m_slotQuery[slot])
		m_ctx->End(m_slotQuery[slot]);
	// The overlay, Present and telemetry only use private D3D11 resources.
	const bool signaledByGpu = m_queueDevice && SUCCEEDED(m_queueDevice->EnqueueSetEvent(m_slotReady[slot]));

	if (overlay || compareSplitPx >= 0.0f)
		RenderHudOverlay(overlay, compareSplitPx);

	m_reflex.EndGpuWork();
	m_reflex.MarkFrame(qf.frameId, RM_RENDERSUBMIT_END);

	UINT flags = m_tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0u;
	m_reflex.MarkFrame(qf.frameId, RM_PRESENT_START);
	HRESULT hr = m_swapChain->Present(0, flags);
	if (hr == DXGI_ERROR_INVALID_CALL && flags != 0)
		hr = m_swapChain->Present(0, 0);
	m_reflex.MarkFrame(qf.frameId, RM_PRESENT_END);
	m_reflex.OnPresentFinished();
	if (!signaledByGpu && m_slotQuery[slot]) {
		BOOL done = FALSE;
		while (m_ctx->GetData(m_slotQuery[slot], &done, sizeof(done), 0) == S_FALSE)
			SwitchToThread();
		if (m_queueDevice) SetEvent(m_slotReady[slot]);
	}

	if ((hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) && !m_deviceLostLogged) {
		m_deviceLostLogged = true;
		Log("DXGI: Present device removed (hr=0x%08lX, reason=0x%08lX)", hr, m_dev->GetDeviceRemovedReason());
	}

	LARGE_INTEGER nowQpc = {};
	QueryPerformanceCounter(&nowQpc);
	if (m_lastPresentQpc.QuadPart > 0 && m_qpcFreq.QuadPart > 0) {
		float dtMs = (float)((double)(nowQpc.QuadPart - m_lastPresentQpc.QuadPart) * 1000.0 / (double)m_qpcFreq.QuadPart);
		if (dtMs > 0.05f && dtMs < 500.0f) {
			float prev = LoadAtomicFloat(&m_emaFrameMsBits);
			float next = (prev <= 0.01f) ? dtMs : (prev * 0.92f + dtMs * 0.08f);
			StoreAtomicFloat(&m_emaFrameMsBits, next);
		}
	}
	m_lastPresentQpc = nowQpc;

	if (m_reflex.Backend() == LowLatencyBackend::None && m_waitableObj)
		WaitForSingleObjectEx(m_waitableObj, 16, TRUE);

	m_reflex.PollTelemetry();
}

DisplayTelemetry D3D11Presenter::SnapshotTelemetry(bool realBB, UINT refreshHz) const
{
	DisplayTelemetry t = {};
	ReflexStats rs = m_reflex.GetStats();
	float emaMs = LoadAtomicFloat(&m_emaFrameMsBits);
	t.frameTimeMs   = emaMs;
	t.fps           = (emaMs > 0.01f) ? (1000.0f / emaMs) : 0.0f;
	t.pcLatMs       = rs.pcLatMs;
	t.gpuMs         = rs.gpuMs;
	t.osQueueMs     = rs.osQueueMs;
	t.reflexOn      = m_reflex.Active();
	t.iFlip         = rs.iFlip;
	t.vrr           = rs.vrr;
	t.tearing       = m_tearingSupported;
	t.asyncOn       = g_cfg.asyncPresent && (m_thread != nullptr);
	t.realBB        = realBB;
	t.lowLatBackend = m_reflex.BackendName();
	t.gameW         = m_gameW;
	t.gameH         = m_gameH;
	t.swapW         = m_swapW;
	t.swapH         = m_swapH;
	t.refreshHz     = refreshHz;
	return t;
}
