// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "d3d9_proxy.h"
#include "d3d11_presenter.h"
#include "async_screenshot.h"
#include "blit_bc.h"
#include "managed_resources.h"
#include "rain_lighting.h"
#include "player_reflection.h"
#include "../core/frame_pacer.h"
#include "../core/log.h"
#include "../core/patch.h"
#include "../core/samp.h"
#include "../core/scheduler.h"

#include <emmintrin.h>
#include <math.h>
#include <shobjidl.h>
#include <string.h>
#include <vector>

static constexpr UINT kMsgDeferMinimize = WM_APP + 0x4D1;
static UINT           g_msgTaskbarCreated = 0;

static void EnsureTaskbarTab(HWND hwnd)
{
	if (!IsWindow(hwnd)) return;
	static const CLSID kClsidTaskbarList = { 0x56FDF344, 0xFD6D, 0x11D0, { 0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90 } };
	static const IID   kIidTaskbarList   = { 0x56FDF342, 0xFD6D, 0x11D0, { 0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90 } };
	bool didInit = false;
	ITaskbarList *tb = nullptr;
	HRESULT hr = CoCreateInstance(kClsidTaskbarList, nullptr, CLSCTX_INPROC_SERVER, kIidTaskbarList, (void **)&tb);
	if (hr == CO_E_NOTINITIALIZED) {
		if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
			didInit = true;
			hr = CoCreateInstance(kClsidTaskbarList, nullptr, CLSCTX_INPROC_SERVER, kIidTaskbarList, (void **)&tb);
		}
	}
	if (SUCCEEDED(hr) && tb) {
		if (SUCCEEDED(tb->HrInit()))
			tb->AddTab(hwnd);
		tb->Release();
	}
	if (didInit)
		CoUninitialize();
}

#ifndef FOURCC_INTZ
#define FOURCC_INTZ ((D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'))
#endif

static inline bool Equal64SSE2(const void *a, const void *b)
{
	const auto *pa = (const __m128i *)a;
	const auto *pb = (const __m128i *)b;
	__m128i d0 = _mm_xor_si128(_mm_loadu_si128(pa + 0), _mm_loadu_si128(pb + 0));
	__m128i d1 = _mm_xor_si128(_mm_loadu_si128(pa + 1), _mm_loadu_si128(pb + 1));
	__m128i d2 = _mm_xor_si128(_mm_loadu_si128(pa + 2), _mm_loadu_si128(pb + 2));
	__m128i d3 = _mm_xor_si128(_mm_loadu_si128(pa + 3), _mm_loadu_si128(pb + 3));
	__m128i any = _mm_or_si128(_mm_or_si128(d0, d1), _mm_or_si128(d2, d3));
	return _mm_movemask_epi8(_mm_cmpeq_epi32(any, _mm_setzero_si128())) == 0xFFFF;
}

static inline bool Equal16SSE2(const void *a, const void *b)
{
	__m128i d = _mm_xor_si128(_mm_loadu_si128((const __m128i *)a), _mm_loadu_si128((const __m128i *)b));
	return _mm_movemask_epi8(_mm_cmpeq_epi32(d, _mm_setzero_si128())) == 0xFFFF;
}

static inline bool Equal68SSE2(const void *a, const void *b)
{
	const auto *pa = (const uint8_t *)a;
	const auto *pb = (const uint8_t *)b;
	return Equal64SSE2(pa, pb) && (*(const uint32_t *)(pa + 64) == *(const uint32_t *)(pb + 64));
}

static inline bool Equal104SSE2(const void *a, const void *b)
{
	const auto *pa = (const __m128i *)a;
	const auto *pb = (const __m128i *)b;
	__m128i d0 = _mm_xor_si128(_mm_loadu_si128(pa + 0), _mm_loadu_si128(pb + 0));
	__m128i d1 = _mm_xor_si128(_mm_loadu_si128(pa + 1), _mm_loadu_si128(pb + 1));
	__m128i d2 = _mm_xor_si128(_mm_loadu_si128(pa + 2), _mm_loadu_si128(pb + 2));
	__m128i d3 = _mm_xor_si128(_mm_loadu_si128(pa + 3), _mm_loadu_si128(pb + 3));
	__m128i d4 = _mm_xor_si128(_mm_loadu_si128(pa + 4), _mm_loadu_si128(pb + 4));
	__m128i d5 = _mm_xor_si128(_mm_loadu_si128(pa + 5), _mm_loadu_si128(pb + 5));
	__m128i any = _mm_or_si128(_mm_or_si128(_mm_or_si128(d0, d1), _mm_or_si128(d2, d3)), _mm_or_si128(d4, d5));
	if (_mm_movemask_epi8(_mm_cmpeq_epi32(any, _mm_setzero_si128())) != 0xFFFF) return false;
	return *(const uint64_t *)((const uint8_t *)a + 96) == *(const uint64_t *)((const uint8_t *)b + 96);
}

static inline UINT ComputePrimVertexCount(D3DPRIMITIVETYPE type, UINT primCount)
{
	switch (type) {
	case D3DPT_POINTLIST:     return primCount;
	case D3DPT_LINELIST:      return primCount * 2u;
	case D3DPT_LINESTRIP:     return primCount + 1u;
	case D3DPT_TRIANGLELIST:  return primCount * 3u;
	case D3DPT_TRIANGLESTRIP: return primCount + 2u;
	case D3DPT_TRIANGLEFAN:   return primCount + 2u;
	default:                  return 0u;
	}
}

static inline int TransformSlot(D3DTRANSFORMSTATETYPE st)
{
	const UINT s = (UINT)st;
	if (s == (UINT)D3DTS_VIEW)       return 0;
	if (s == (UINT)D3DTS_PROJECTION) return 1;
	if (s >= (UINT)D3DTS_TEXTURE0 && s <= (UINT)D3DTS_TEXTURE7) return 2 + (int)(s - (UINT)D3DTS_TEXTURE0);
	if (s >= 256u && s < 264u)       return 10 + (int)(s - 256u); // D3DTS_WORLD..D3DTS_WORLDMATRIX(7)
	return -1;
}

class ProxyDirect3DDevice9;
static ProxyDirect3DDevice9 *g_activeDev = nullptr;
static SetCursorPos_t        g_rawSetCursorPos = nullptr;
static GetClientRect_t       g_rawGetClientRect = nullptr;
static WNDPROC               g_savedWndProc = nullptr;
static bool                  g_savedWndUnicode = false;

static inline BOOL GetRealClientRect(HWND hwnd, LPRECT rc)
{
	return g_rawGetClientRect ? g_rawGetClientRect(hwnd, rc) : GetClientRect(hwnd, rc);
}

typedef HRESULT(STDMETHODCALLTYPE *D3D9Present_t)(IDirect3DDevice9 *, CONST RECT *, CONST RECT *, HWND, CONST RGNDATA *);
typedef HRESULT(STDMETHODCALLTYPE *D3D9Reset_t)(IDirect3DDevice9 *, D3DPRESENT_PARAMETERS *);
typedef HRESULT(STDMETHODCALLTYPE *D3D9EndScene_t)(IDirect3DDevice9 *);

static D3D9Present_t  g_d3d9PresentEntry  = nullptr;
static D3D9Present_t  g_origD3D9Present   = nullptr;
static D3D9Reset_t    g_d3d9ResetEntry    = nullptr;
static D3D9Reset_t    g_origD3D9Reset     = nullptr;
static D3D9EndScene_t g_d3d9EndSceneEntry = nullptr;
static D3D9EndScene_t g_origD3D9EndScene  = nullptr;
static bool           g_detourBridgeInit  = false;

template <typename T>
static inline void SafeRelease(T *&p)
{
	if (p) { p->Release(); p = nullptr; }
}

#pragma comment(lib, "gdi32.lib")
static HWND CreateHiddenDummyWindow(int w, int h)
{
	static ATOM s_atom = 0;
	HINSTANCE inst = GetModuleHandleA(nullptr);
	if (!s_atom) {
		WNDCLASSA wc = {};
		wc.lpfnWndProc = DefWindowProcA;
		wc.hInstance = inst;
		wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
		wc.lpszClassName = "ModernDisplayDummyD3D9";
		s_atom = RegisterClassA(&wc);
	}
	HWND hw = CreateWindowExA(
		WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
		"ModernDisplayDummyD3D9", "", WS_POPUP,
		-32000, -32000, w > 0 ? w : 1, h > 0 ? h : 1, nullptr, nullptr, inst, nullptr);
	if (hw)
		SetLayeredWindowAttributes(hw, 0, 0, LWA_ALPHA);
	return hw;
}

static void QueryMonitorRect(HWND hwnd, RECT &rc, UINT &refreshHz, RECT *workRcOut = nullptr)
{
	HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXW mi = {};
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoW(mon, &mi)) {
		rc = mi.rcMonitor;
		if (workRcOut) *workRcOut = mi.rcWork;
		DEVMODEW dm = {};
		dm.dmSize = sizeof(dm);
		if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 0)
			refreshHz = dm.dmDisplayFrequency;
		else
			refreshHz = 60;
	} else {
		rc = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
		if (workRcOut) *workRcOut = rc;
		refreshHz = 60;
	}
}

static void BringGameWindowToFront(HWND hwnd)
{
	if (!IsWindow(hwnd)) return;
	if (IsIconic(hwnd))
		ShowWindow(hwnd, SW_RESTORE);

	HWND fg = GetForegroundWindow();
	if (fg == hwnd) return;

	DWORD curTid = GetCurrentThreadId();
	DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
	bool attached = (fgTid != 0 && fgTid != curTid && AttachThreadInput(fgTid, curTid, TRUE) != FALSE);

	// Brief TOPMOST -> NOTOPMOST bounce guarantees visibility above samp.exe / launchers
	// even when Windows foreground lock timeout suppresses SetForegroundWindow.
	SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
	SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
	BringWindowToTop(hwnd);
	SetForegroundWindow(hwnd);
	if (GetForegroundWindow() == hwnd && GetFocus() != hwnd)
		SetFocus(hwnd);
	SetActiveWindow(hwnd);

	if (attached)
		AttachThreadInput(fgTid, curTid, FALSE);
}

class ProxyStateBlock9 final : public IDirect3DStateBlock9 {
public:
	ProxyStateBlock9(ProxyDirect3DDevice9 *dev, IDirect3DStateBlock9 *real);
	~ProxyStateBlock9();

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) override
	{
		if (!ppvObj) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IDirect3DStateBlock9) {
			*ppvObj = this;
			AddRef();
			return S_OK;
		}
		return m_real->QueryInterface(riid, ppvObj);
	}
	ULONG   STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
	ULONG   STDMETHODCALLTYPE Release() override
	{
		ULONG r = (ULONG)InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return r;
	}
	HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
	HRESULT STDMETHODCALLTYPE Capture() override;
	HRESULT STDMETHODCALLTYPE Apply() override;

private:
	ProxyDirect3DDevice9 *m_dev;
	IDirect3DStateBlock9 *m_real;
	volatile LONG         m_ref;
};

class ProxySwapChain9 final : public IDirect3DSwapChain9 {
public:
	explicit ProxySwapChain9(ProxyDirect3DDevice9 *dev);
	~ProxySwapChain9();

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) override;
	ULONG   STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
	ULONG   STDMETHODCALLTYPE Release() override
	{
		ULONG r = (ULONG)InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return r;
	}

	HRESULT STDMETHODCALLTYPE Present(CONST RECT *pSourceRect, CONST RECT *pDstRect, HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion, DWORD dwFlags) override;
	HRESULT STDMETHODCALLTYPE GetFrontBufferData(IDirect3DSurface9 *pDestSurface) override;
	HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE Type, IDirect3DSurface9 **ppBackBuffer) override;
	HRESULT STDMETHODCALLTYPE GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) override;
	HRESULT STDMETHODCALLTYPE GetDisplayMode(D3DDISPLAYMODE *pMode) override;
	HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
	HRESULT STDMETHODCALLTYPE GetPresentParameters(D3DPRESENT_PARAMETERS *pPresentationParameters) override;

private:
	ProxyDirect3DDevice9 *m_dev;
	volatile LONG         m_ref;
};

// Stable IDirect3DSurface9 proxy for main backbuffer / depth-stencil.
// RenderWare/GTA SA caches GetBackBuffer(0,0) and GetDepthStencilSurface() pointers once at init;
// this wrapper stays valid across zero-copy ping-pong swaps and live /md target rebuilds (0 dangling pointers).
class ProxyStableSurface9 final : public IDirect3DSurface9 {
public:
	ProxyStableSurface9(ProxyDirect3DDevice9 *dev, bool isDepth)
		: m_dev(dev), m_isDepth(isDepth), m_ref(1) {}
	void Detach() { m_dev = nullptr; }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) override
	{
		if (!ppvObj) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IDirect3DResource9 || riid == IID_IDirect3DSurface9) {
			*ppvObj = this;
			AddRef();
			return S_OK;
		}
		*ppvObj = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG r = (ULONG)InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return r;
	}

	HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **ppDevice) override;
	HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID refguid, CONST void *pData, DWORD SizeOfData, DWORD Flags) override;
	HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData) override;
	HRESULT STDMETHODCALLTYPE FreePrivateData(REFGUID refguid) override;
	DWORD   STDMETHODCALLTYPE SetPriority(DWORD PriorityNew) override;
	DWORD   STDMETHODCALLTYPE GetPriority() override;
	void    STDMETHODCALLTYPE PreLoad() override;
	D3DRESOURCETYPE STDMETHODCALLTYPE GetType() override { return D3DRTYPE_SURFACE; }
	HRESULT STDMETHODCALLTYPE GetContainer(REFIID riid, void **ppContainer) override;
	HRESULT STDMETHODCALLTYPE GetDesc(D3DSURFACE_DESC *pDesc) override;
	HRESULT STDMETHODCALLTYPE LockRect(D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags) override;
	HRESULT STDMETHODCALLTYPE UnlockRect() override;
	HRESULT STDMETHODCALLTYPE GetDC(HDC *phdc) override;
	HRESULT STDMETHODCALLTYPE ReleaseDC(HDC hdc) override;

	IDirect3DSurface9 *Resolve() const;

private:
	ProxyDirect3DDevice9 *m_dev;
	bool                  m_isDepth;
	volatile LONG         m_ref;
};

class ProxyDirect3DDevice9 final : public IDirect3DDevice9 {
public:
	ProxyDirect3DDevice9(IDirect3D9 *proxyD3D, IDirect3DDevice9Ex *realDevEx,
	                     HWND gameHwnd, HWND dummyHwnd, const D3DPRESENT_PARAMETERS &pp,
	                     const D3DPRESENT_PARAMETERS &realPP, bool usesRealSwapchain,
	                     UINT adapter, D3DDEVTYPE devType, DWORD behaviorFlags,
	                     LUID adapterLuid, bool hasLuid)
		: m_proxyD3D(proxyD3D), m_real(realDevEx), m_gameHwnd(gameHwnd), m_dummyHwnd(dummyHwnd),
		  m_pp(pp), m_realPP(realPP), m_realUsesSwapchain(usesRealSwapchain),
		  m_requestedMsaa(pp.MultiSampleType), m_requestedMsaaQ(pp.MultiSampleQuality),
		  m_adapter(adapter), m_devType(devType), m_behaviorFlags(behaviorFlags),
		  m_adapterLuid(adapterLuid), m_hasLuid(hasLuid), m_ref(1)
	{
		m_proxyD3D->AddRef();
		for (int i = 0; i < 256; ++i) {
			WORD v = (WORD)(i << 8);
			m_gamma.red[i] = m_gamma.green[i] = m_gamma.blue[i] = v;
		}
		for (int i = 0; i < D3D11Presenter::kNumSlots; ++i)
			m_real->CreateQuery(D3DQUERYTYPE_EVENT, &m_evQuery[i]);
		m_real->CreateStateBlock(D3DSBT_ALL, &m_defaultState);
		m_real->CreateStateBlock(D3DSBT_ALL, &m_depthCopySB);
		m_real->CreatePixelShader((const DWORD *)kD3D9DepthCopyPS, &m_depthCopyPS);
		m_proxyBackBuffer = new ProxyStableSurface9(this, false);
		m_proxyDepthSurf  = new ProxyStableSurface9(this, true);
		SchedulerInitProcess();
		m_mmcssGame = SchedulerRegisterGamesThread();
	}

	~ProxyDirect3DDevice9()
	{
		if (g_activeDev == this) {
			SetManagedTexDestroyCallback(nullptr);
			g_activeDev = nullptr;
		}
		ShutdownManagedMipWorker();
		if (m_proxyBackBuffer) { m_proxyBackBuffer->Detach(); m_proxyBackBuffer->Release(); m_proxyBackBuffer = nullptr; }
		if (m_proxyDepthSurf)  { m_proxyDepthSurf->Detach();  m_proxyDepthSurf->Release();  m_proxyDepthSurf = nullptr; }
		AsyncScreenshotFlush();
		UnhookWindowProc();
		ReleaseGameTargets();
		m_presenter.Shutdown();
		SafeRelease(m_depthCopyPS);
		SafeRelease(m_depthCopySB);
		SafeRelease(m_defaultState);
		for (int i = 0; i < D3D11Presenter::kNumSlots; ++i)
			SafeRelease(m_evQuery[i]);
		SafeRelease(m_real);
		SafeRelease(m_proxyD3D);
		if (m_dummyHwnd) { DestroyWindow(m_dummyHwnd); m_dummyHwnd = nullptr; }
		SchedulerUnregisterGamesThread(m_mmcssGame);
		m_mmcssGame = nullptr;
		SchedulerShutdownProcess();
	}

	static void OnManagedTexDestroyed(IDirect3DBaseTexture9 *rawTex)
	{
		ProxyDirect3DDevice9 *self = g_activeDev;
		if (!self || !rawTex) return;
		for (int s = 0; s < 8; ++s) {
			if (self->m_texManaged[s] == rawTex) {
				self->m_texManaged[s] = nullptr;
				self->m_boundManagedMask &= (uint8_t)~(1u << s);
				self->m_texGpu[s] = nullptr;
				self->m_texValid[s] = 0;
			}
		}
	}

	inline void FlushBoundManagedTextures()
	{
		uint8_t mask = m_boundManagedMask;
		if (!mask || !HasDirtyManagedResources()) return;
		for (int s = 0; mask != 0; ++s, mask >>= 1) {
			if ((mask & 1u) && m_texManaged[s])
				SyncManagedBaseTexture(m_texManaged[s]);
		}
	}

	bool InitializeBridge()
	{
		RECT monRc = {};
		QueryMonitorRect(m_gameHwnd, monRc, m_refreshHz);
		UINT monW = (UINT)(monRc.right - monRc.left);
		UINT monH = (UINT)(monRc.bottom - monRc.top);

		UINT baseW = m_pp.BackBufferWidth  ? m_pp.BackBufferWidth  : monW;
		UINT baseH = m_pp.BackBufferHeight ? m_pp.BackBufferHeight : monH;
		m_fullscreenW = baseW;
		m_fullscreenH = baseH;

		// Respect explicit windowed mode (Alt+Enter, SAMPFUNCS window-mode, WindowedMode.asi);
		// for INI windowed=1 on a fullscreen CreateDevice, keep baseW x baseH during CreateDevice
		// so GTA SA's GetClientRect (0x7F67A8) + CGame::InitialiseRenderWare (0x5BD600) succeed,
		// then apply native 1:1 windowed mode cleanly on frame 1 right after WinMain sets WS_POPUP.
		bool callerWindowed = (m_pp.Windowed != FALSE);
		m_lastCallerWindowed = callerWindowed;
		m_pendingForceWindowed = g_cfg.forceWindowed && !callerWindowed;
		m_forceWinInverted = m_pendingForceWindowed;
		m_borderless = !callerWindowed;
		m_initTick = GetTickCount();
		HookWindowProc();
		if (m_borderless) {
			ApplyBorderless(true);
		} else {
			ApplyWindowed(baseW, baseH, false);
		}

		RECT clientRc = {};
		GetRealClientRect(m_gameHwnd, &clientRc);
		UINT swapW = (clientRc.right > clientRc.left) ? (UINT)(clientRc.right - clientRc.left) : baseW;
		UINT swapH = (clientRc.bottom > clientRc.top) ? (UINT)(clientRc.bottom - clientRc.top) : baseH;

		if (!m_borderless && swapW >= 320 && swapH >= 240) {
			baseW = swapW;
			baseH = swapH;
		}
		m_logicalBaseW = baseW;
		m_logicalBaseH = baseH;

		UINT gameW = baseW, gameH = baseH;
		if (!m_pendingForceWindowed)
			ComputeScaledGameSize(baseW, baseH, gameW, gameH);
		m_pp.BackBufferWidth  = gameW;
		m_pp.BackBufferHeight = gameH;

		if (!m_presenter.Init(m_gameHwnd, swapW, swapH, m_adapterLuid, m_hasLuid))
			return false;
		m_presenter.SetGammaRamp(m_gamma);
		if (!CreateGameTargets(gameW, gameH, true))
			return false;
		if (!m_borderless || gameW != baseW || gameH != baseH)
			SyncGtaRenderResolution(gameW, gameH, !m_borderless);

		g_activeDev = this;
		SetManagedTexDestroyCallback(OnManagedTexDestroyed);
		TryInstallAsyncScreenshotHook();
		m_proxyCfgSeq = InterlockedCompareExchange(&g_cfgSeq, 0, 0);
		m_cachedClientW = swapW;
		m_cachedClientH = swapH;
		m_isMinimized = (IsIconic(m_gameHwnd) != FALSE);
		m_isForeground = (GetForegroundWindow() == m_gameHwnd);
		m_lastWinSyncTick = GetTickCount();
		if (m_isForeground)
			m_everActivated = true;
		m_curFrameId = m_presenter.Reflex().NextFrameId();
		Log("Bridge: ready (mode=%s, game %ux%u, swap %ux%u @ %uHz, MSAA=%d, intz=%d, realBB=%d)",
			m_borderless ? "borderless" : "windowed", gameW, gameH, swapW, swapH,
			m_refreshHz, (int)m_pp.MultiSampleType,
			m_intzDepthTex ? 1 : 0, m_realUsesSwapchain ? 1 : 0);
		return true;
	}

	D3D11Presenter &Presenter() { return m_presenter; }
	HWND GameHwnd() const { return m_gameHwnd; }
	bool GetGameSize(UINT &w, UINT &h) const
	{
		if (m_pp.BackBufferWidth == 0 || m_pp.BackBufferHeight == 0) return false;
		w = m_pp.BackBufferWidth;
		h = m_pp.BackBufferHeight;
		return true;
	}
	bool NeedsVirtualMapping() const
	{
		if (!g_cfg.windowUiFix) return false;
		const UINT gw = m_pp.BackBufferWidth, gh = m_pp.BackBufferHeight;
		if (gw == 0 || gh == 0) return false;
		UINT sw = m_cachedClientW, sh = m_cachedClientH;
		if ((sw == 0 || sh == 0) && IsWindow(m_gameHwnd)) {
			RECT rc = {};
			if (GetRealClientRect(m_gameHwnd, &rc)) {
				sw = (UINT)(rc.right - rc.left);
				sh = (UINT)(rc.bottom - rc.top);
			}
		}
		return sw > 0 && sh > 0 && (gw != sw || gh != sh);
	}
	void MapClientToGame(LONG &x, LONG &y) const
	{
		const UINT gw = m_pp.BackBufferWidth, gh = m_pp.BackBufferHeight;
		UINT sw = m_cachedClientW, sh = m_cachedClientH;
		if ((sw == 0 || sh == 0) && IsWindow(m_gameHwnd)) {
			RECT rc = {};
			if (GetRealClientRect(m_gameHwnd, &rc)) {
				sw = (UINT)(rc.right - rc.left);
				sh = (UINT)(rc.bottom - rc.top);
			}
		}
		if (gw == 0 || gh == 0 || sw == 0 || sh == 0) return;
		float vpX, vpY, vpW, vpH;
		ComputeDisplayViewport(gw, gh, sw, sh, g_cfg.scaling, vpX, vpY, vpW, vpH);
		if (vpW <= 1.0f || vpH <= 1.0f) return;
		if (vpX == 0.0f && vpY == 0.0f && vpW == (float)gw && vpH == (float)gh) return;
		x = (LONG)floorf((((float)x + 0.5f) - vpX) * ((float)gw / vpW));
		y = (LONG)floorf((((float)y + 0.5f) - vpY) * ((float)gh / vpH));
	}
	void MapGameToClient(LONG &x, LONG &y) const
	{
		const UINT gw = m_pp.BackBufferWidth, gh = m_pp.BackBufferHeight;
		UINT sw = m_cachedClientW, sh = m_cachedClientH;
		if ((sw == 0 || sh == 0) && IsWindow(m_gameHwnd)) {
			RECT rc = {};
			if (GetRealClientRect(m_gameHwnd, &rc)) {
				sw = (UINT)(rc.right - rc.left);
				sh = (UINT)(rc.bottom - rc.top);
			}
		}
		if (gw == 0 || gh == 0 || sw == 0 || sh == 0) return;
		float vpX, vpY, vpW, vpH;
		ComputeDisplayViewport(gw, gh, sw, sh, g_cfg.scaling, vpX, vpY, vpW, vpH);
		if (vpW <= 1.0f || vpH <= 1.0f) return;
		if (vpX == 0.0f && vpY == 0.0f && vpW == (float)gw && vpH == (float)gh) return;
		x = (LONG)lroundf(vpX + (float)x * (vpW / (float)gw));
		y = (LONG)lroundf(vpY + (float)y * (vpH / (float)gh));
	}
	static bool IsGtaSaUs10()
	{
		static int s_cached = -1;
		if (s_cached >= 0) return s_cached != 0;
		bool ok = false;
		__try {
			MEMORY_BASIC_INFORMATION mbi1 = {}, mbi2 = {};
			if (VirtualQuery((void *)0x0072FC70, &mbi1, sizeof(mbi1)) && mbi1.State == MEM_COMMIT &&
			    VirtualQuery((void *)0x00619B60, &mbi2, sizeof(mbi2)) && mbi2.State == MEM_COMMIT &&
			    *(const DWORD *)0x0072FC70 == 0x5734EC83u &&
			    *(const DWORD *)0x00619B60 == 0x24748B56u) {
				ok = true;
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			ok = false;
		}
		s_cached = ok ? 1 : 0;
		return ok;
	}

	// Keeps GTA SA 1.0 US RenderWare camera, RsGlobal, RwD3D9PresentParams, and video-mode
	// exclusive flag in 1:1 lockstep with our native windowed or borderless backbuffer size.
	void SyncGtaRenderResolution(UINT w, UINT h, bool windowed)
	{
		if (w == 0 || h == 0 || !IsGtaSaUs10()) return;
		struct RwD3D9VideoModeEntry {
			UINT      width;
			UINT      height;
			UINT      refreshRate;
			D3DFORMAT format;
			DWORD     flags;
		};
		__try {
			int curMode = *(const int *)0x00C97C18;
			int numModes = *(const int *)0x00C97C40;
			auto *modes = *(RwD3D9VideoModeEntry **)0x00C97C48;
			if (modes && curMode >= 0 && curMode < numModes) {
				if (!m_hasSavedRwMode || m_savedRwModeIdx != curMode) {
					m_savedRwModeIdx = curMode;
					m_savedRwModeFlags = modes[curMode].flags;
					m_hasSavedRwMode = true;
				}
				// Clear rwVIDEOMODEEXCLUSIVE (bit 0) whenever windowed OR SSAA-scaled (w/h != vidMode)
				// so CameraSize(cam, NULL) preserves our RwCamera/RsGlobal dimensions instead of snapping back.
				const bool needsNonExclusive = windowed || (modes[curMode].width != w || modes[curMode].height != h);
				modes[curMode].flags = needsNonExclusive ? (m_savedRwModeFlags & ~1u) : m_savedRwModeFlags;
			}

			*(UINT *)0x00C9C040 = w;
			*(UINT *)0x00C9C044 = h;
			*(BOOL *)0x00C9C060 = windowed ? TRUE : FALSE;
			*(UINT *)0x00C9BEE4 = w;
			*(UINT *)0x00C9BEE8 = h;

			if (*(void **)0x00C1703C != nullptr) {
				RECT rc = { 0, 0, (LONG)w, (LONG)h };
				((int(__cdecl *)(int, void *))0x00619B60)(0 /* rsCAMERASIZE */, &rc);
			}
			*(int *)0x00C17044 = (int)w;
			*(int *)0x00C17048 = (int)h;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	bool IsDeactivating() const { return m_deactivating; }
	bool UsesRealSwapchain() const { return m_realUsesSwapchain; }
	UINT RefreshHz() const { return m_refreshHz; }
	void MarkInputSampled()
	{
		m_inputSampledThisFrame = true;
		m_curInputQpc = m_presenter.Reflex().PreInputSync();
		m_presenter.Reflex().MarkFrame(m_curFrameId, RM_INPUT_SAMPLE);
	}

	static bool WantDepthTargets()
	{
		return g_cfg.aoQuality > 0 || g_cfg.ssrQuality > 0 || g_cfg.giStrength > 0.001f ||
		       g_cfg.fogStrength > 0.001f || g_cfg.aaQuality >= 3;
	}

	static bool Want3dMaskTargets()
	{
		return WantRoadPuddles() || (g_cfg.depthCas && (g_cfg.aoQuality > 0 || g_cfg.aaQuality > 0 || g_cfg.ssrQuality > 0 ||
		                          g_cfg.giStrength > 0.001f || g_cfg.rainFx ||
		                          g_cfg.bloomStrength > 0.001f || g_cfg.fogStrength > 0.001f ||
		                          g_cfg.tonemapMode > 0 || g_cfg.clarity > 0.001f ||
		                          g_cfg.sharpness > 0.001f || g_cfg.lutName[0] != 0));
	}

	static bool WantRoadPuddles()
	{
		return g_cfg.rainFx && g_cfg.ssrQuality > 0;
	}

	static void ComputeScaledGameSize(UINT baseW, UINT baseH, UINT &outW, UINT &outH)
	{
		float sc = g_cfg.renderScale;
		if (sc <= 1.01f || baseW == 0 || baseH == 0) {
			outW = baseW;
			outH = baseH;
			return;
		}
		if (sc > 2.0f) sc = 2.0f;
		UINT w = ((UINT)lroundf((float)baseW * sc)) & ~1u;
		UINT h = ((UINT)lroundf((float)baseH * sc)) & ~1u;
		if (w > 3840u) { h = (UINT)((uint64_t)h * 3840u / w) & ~1u; w = 3840u; }
		if (h > 2160u) { w = (UINT)((uint64_t)w * 2160u / h) & ~1u; h = 2160u; }
		if (w < 320u) w = 320u;
		if (h < 240u) h = 240u;
		outW = w;
		outH = h;
	}

	static bool IsSceneProjection(const D3DMATRIX &p)
	{
		return fabsf(p._34 - 1.0f) < 0.02f && fabsf(p._44) < 0.02f &&
		       fabsf(p._11) > 0.30f && fabsf(p._22) > 0.30f && p._33 > 0.99f && p._33 < 1.15f;
	}

	void ReadGtaCameraParams(FrameCameraParams &out) const
	{
		out = {};
		if (m_pp.BackBufferWidth > 0 && m_pp.BackBufferHeight > 0) {
			float aspect = (float)m_pp.BackBufferWidth / (float)m_pp.BackBufferHeight;
			out.projScaleY = 1.428f;
			out.projScaleX = out.projScaleY / aspect;
		}
		if (!IsGtaSaUs10()) return;
		__try {
			const BYTE *cam = *(const BYTE *const *)0x00C1703C;
			if (!cam) return;
			float rvx   = *(const float *)(cam + 0x70); // 1 / viewWindow.x
			float rvy   = *(const float *)(cam + 0x74); // 1 / viewWindow.y
			float nearZ = *(const float *)(cam + 0x80);
			float farZ  = *(const float *)(cam + 0x84);
			if (rvx > 0.1f && rvx < 50.0f && rvy > 0.1f && rvy < 50.0f) {
				out.projScaleX = rvx;
				out.projScaleY = rvy;
			}
			if (nearZ > 0.05f && nearZ < 20.0f && farZ > nearZ + 10.0f && farZ < 20000.0f) {
				out.nearZ = nearZ;
				out.farZ  = farZ;
			}

			// Live GTA SA weather, interior state, sky horizon colors & sun vector
			float rain = *(const float *)0x00C81324;
			float wet  = *(const float *)0x00C81308;
			float wfog = *(const float *)0x00C81300;
			int   area = *(const int *)0x00B72914;
			if (rain >= 0.0f && rain <= 2.0f) out.rain       = (rain > 1.0f) ? 1.0f : rain;
			if (wet  >= 0.0f && wet  <= 2.0f) out.wetRoads   = (wet  > 1.0f) ? 1.0f : wet;
			if (wfog >= 0.0f && wfog <= 2.0f) out.weatherFog = (wfog > 1.0f) ? 1.0f : wfog;
			out.inInterior = (area != 0);
			if (g_cfg.rainFx && !out.inInterior && (out.rain > 0.05f || out.wetRoads > 0.65f)) {
				const auto cameraNoRain = reinterpret_cast<bool(__cdecl *)()>(0x72DDB0);
				out.sheltered = cameraNoRain();
			}

			const BYTE *cc = (const BYTE *)0x00B7C4A0; // CTimeCycle::m_CurrentColours
			uint16_t stR = *(const uint16_t *)(cc + 0x28);
			uint16_t stG = *(const uint16_t *)(cc + 0x2A);
			uint16_t stB = *(const uint16_t *)(cc + 0x2C);
			uint16_t sbR = *(const uint16_t *)(cc + 0x2E);
			uint16_t sbG = *(const uint16_t *)(cc + 0x30);
			uint16_t sbB = *(const uint16_t *)(cc + 0x32);
			if (stR <= 255 && stG <= 255 && stB <= 255) {
				out.skyTop[0] = (float)stR / 255.0f;
				out.skyTop[1] = (float)stG / 255.0f;
				out.skyTop[2] = (float)stB / 255.0f;
			}
			if (sbR <= 255 && sbG <= 255 && sbB <= 255) {
				out.skyBot[0] = (float)sbR / 255.0f;
				out.skyBot[1] = (float)sbG / 255.0f;
				out.skyBot[2] = (float)sbB / 255.0f;
			}
			float fs = *(const float *)(cc + 0x60);
			if (fs >= -50.0f && fs <= 1500.0f) out.fogStart = fs;

			int sunIdx = *(const int *)0x00B79FD0;
			if (sunIdx >= 0 && sunIdx < 16) {
				const float *sv = (const float *)(0x00B7CA50 + (size_t)sunIdx * 12u);
				float slen = sqrtf(sv[0] * sv[0] + sv[1] * sv[1] + sv[2] * sv[2]);
				if (slen > 0.01f && slen < 10.0f) {
					out.sunDirWorld[0] = sv[0] / slen;
					out.sunDirWorld[1] = sv[1] / slen;
					out.sunDirWorld[2] = sv[2] / slen;
				}
			}
		} __except (EXCEPTION_EXECUTE_HANDLER) {
		}
		if (m_hasSceneMats) {
			const float *v = &m_sceneViewMat._11;
			const float *p = &m_sceneProjMat._11;
			out.projSignX = (m_sceneProjMat._11 < 0.0f) ? -1.0f : 1.0f;
			out.camPitchZ = m_sceneViewMat._33; // dot(WorldUp +Z, CamFwd) in [-1..+1]
			memcpy(out.viewMat, v, sizeof(out.viewMat));
			for (int r = 0; r < 4; ++r) {
				for (int c = 0; c < 4; ++c) {
					double s = 0.0;
					for (int k = 0; k < 4; ++k)
						s += (double)v[r * 4 + k] * (double)p[k * 4 + c];
					out.viewProj[r * 4 + c] = (float)s;
				}
			}
			out.hasViewProj = true;
		}
	}

	bool CopySceneDepth(IDirect3DSurface9 *destination, UINT w, UINT h)
	{
		bool copied = false;
		if (m_intzDepthTex && destination && m_depthCopyPS && m_depthCopySB &&
		    SUCCEEDED(m_depthCopySB->Capture())) {
			IDirect3DSurface9 *prevRt0 = nullptr;
			IDirect3DSurface9 *prevDs  = nullptr;
			D3DVIEWPORT9 prevVp = {};
			m_real->GetRenderTarget(0, &prevRt0);
			m_real->GetDepthStencilSurface(&prevDs);
			m_real->GetViewport(&prevVp);
			// Unbind INTZ as depth-stencil before binding it as SRV texture on stage 0.
			m_real->SetDepthStencilSurface(nullptr);
			if (SUCCEEDED(m_real->SetRenderTarget(0, destination))) {
				D3DVIEWPORT9 vp = { 0, 0, w, h, 0.0f, 1.0f };
				m_real->SetViewport(&vp);
				m_real->SetRenderState(D3DRS_ZENABLE, FALSE);
				m_real->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
				m_real->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
				m_real->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
				m_real->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
				m_real->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
				m_real->SetRenderState(D3DRS_FOGENABLE, FALSE);
				m_real->SetRenderState(D3DRS_COLORWRITEENABLE, 0x0F);
				m_real->SetVertexShader(nullptr);
				m_real->SetPixelShader(m_depthCopyPS);
				m_real->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
				m_real->SetTexture(0, m_intzDepthTex);
				m_real->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
				m_real->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
				m_real->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
				m_real->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
				m_real->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

				struct QuadVert { float x, y, z, rhw, u, v; };
				const float fw = (float)w, fh = (float)h;
				const QuadVert q[4] = {
					{ -0.5f,      -0.5f,      0.0f, 1.0f, 0.0f, 0.0f },
					{ fw - 0.5f,  -0.5f,      0.0f, 1.0f, 1.0f, 0.0f },
					{ -0.5f,      fh - 0.5f,  0.0f, 1.0f, 0.0f, 1.0f },
					{ fw - 0.5f,  fh - 0.5f,  0.0f, 1.0f, 1.0f, 1.0f },
				};
				if (SUCCEEDED(m_real->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVert))))
					copied = true;
				m_real->SetTexture(0, nullptr);
			}

			m_depthCopySB->Apply();
			m_real->SetRenderTarget(0, prevRt0 ? prevRt0 : m_backBufferSurf);
			m_real->SetDepthStencilSurface(prevDs ? prevDs : m_depthSurf);
			m_real->SetViewport(&prevVp);
			SafeRelease(prevRt0);
			SafeRelease(prevDs);
			InvalidateStateCache();
		}
		return copied;
	}

	void *RenderPlayer(void *clump, RenderRwObject render)
	{
		if (m_actorCaptured || m_frontEndFrame || m_captured3dThisFrame || !m_rt0IsBackBuffer ||
		    !m_playerMirror.handles[m_writeSlot] || !WantRoadPuddles() || !IsGtaSaUs10()) return render(clump);
		const float rain = *(const float *)0xC81324, wet = *(const float *)0xC81308;
		if ((rain <= .05f && wet <= .65f) || *(const int *)0xB72914 ||
		    reinterpret_cast<bool(__cdecl *)()>(0x72DDB0)()) return render(clump);
		const BYTE *ped = *reinterpret_cast<const BYTE *const *>(0xB6F5F0);
		constexpr DWORD inVehicle = 1u << 8;
		if (!ped || (*reinterpret_cast<const DWORD *>(ped + 0x46C) & inVehicle)) return render(clump);
		const BYTE *matrix = *reinterpret_cast<const BYTE *const *>(ped + 0x14);
		const float *pos = reinterpret_cast<const float *>(matrix ? matrix + 0x30 : ped + 4);
		float ground[4] = {pos[0], pos[1], pos[2], 0};
		if (!FindPavedGround(pos[0],pos[1],pos[2],ground[3])) return render(clump);
		FlushUpBatch();
		if (!m_playerMirror.Begin(m_real,ground)) return render(clump);
		m_replayPlayerDraw = true;
		void *result = render(clump);
		m_replayPlayerDraw = false;
		m_actorCaptured = m_playerMirror.End();
		if (m_actorCaptured) {
			memcpy(m_actorGround,ground,sizeof(ground));
			static bool logged = false;
			if (!logged) { logged = true; Log("Rain: reflected local player's animated clump"); }
		}
		return result;
	}

	void *RenderPlayerWeapon(void *atomic, RenderRwObject render)
	{
		if (!m_actorCaptured || m_replayPlayerDraw || m_captured3dThisFrame || !m_rt0IsBackBuffer)
			return render(atomic);
		FlushUpBatch();
		if (!m_playerMirror.Resume(m_real)) return render(atomic);
		m_replayPlayerDraw = true;
		void *result = render(atomic);
		m_replayPlayerDraw = false;
		// A custom weapon material may be unsupported. Keep the completed body pass.
		const bool weaponCaptured = m_playerMirror.End();
		if (weaponCaptured) {
			static bool logged = false;
			if (!logged) { logged = true; Log("Rain: captured equipped weapon in player reflection"); }
		}
		return result;
	}

	// Capture the finished world before the game and SA-MP draw their interfaces.
	void OnBefore2dStuff()
	{
		FlushUpBatch();
		if (m_frontEndFrame || m_captured3dThisFrame || !m_real || !m_backBufferSurf) return;
		const int slot = m_writeSlot;
		const bool doDepth = m_intzDepthTex && m_sharedDepthSurf[slot] && m_depthCopyPS;
		if (doDepth || (g_cfg.rainFx && !m_hasCameraParamsThisFrame)) {
			ReadGtaCameraParams(m_curCamParams);
			m_hasCameraParamsThisFrame = true;
		}
		const bool captureRain = WantRoadPuddles() && !m_curCamParams.inInterior && !m_curCamParams.sheltered &&
		                         (m_curCamParams.rain > 0.05f || m_curCamParams.wetRoads > 0.65f);
		const bool do3d = m_shared3dSurf[slot] && (g_cfg.depthCas || captureRain);
		if (!do3d && !doDepth) return;

		m_captured3dThisFrame = true;
		m_presenter.WaitForSlot(slot);
		if (m_actorCaptured) m_actorCaptured = captureRain && m_playerMirror.CopyToSlot(m_real, slot);
		if (do3d && SUCCEEDED(m_real->StretchRect(m_backBufferSurf, nullptr, m_shared3dSurf[slot], nullptr, D3DTEXF_NONE)))
			m_has3dMaskThisFrame = true;
		if (doDepth)
			m_hasDepthThisFrame = CopySceneDepth(m_sharedDepthSurf[slot], m_pp.BackBufferWidth, m_pp.BackBufferHeight);
	}

	void ApplyBorderless(bool force)
	{
		if (!m_borderless || !IsWindow(m_gameHwnd)) return;
		if (!force && IsIconic(m_gameHwnd)) return;
		RECT monRc = {};
		QueryMonitorRect(m_gameHwnd, monRc, m_refreshHz);
		int monW = monRc.right - monRc.left;
		int monH = monRc.bottom - monRc.top;

		// Keep WS_SYSMENU | WS_MINIMIZEBOX on borderless WS_POPUP (zero visual chrome, guarantees
		// Windows 10/11 explorer.exe Taskband keeps the taskbar button & right-click menu intact).
		LONG style = GetWindowLongA(m_gameHwnd, GWL_STYLE);
		LONG wantStyle = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME | WS_MAXIMIZE | WS_MINIMIZE))
		                 | WS_POPUP | WS_VISIBLE | WS_MINIMIZEBOX | WS_SYSMENU;
		LONG exStyle = GetWindowLongA(m_gameHwnd, GWL_EXSTYLE);
		LONG wantEx = (exStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW))
		              | WS_EX_APPWINDOW;

		RECT curRc = {};
		GetWindowRect(m_gameHwnd, &curRc);
		bool zoomed = (IsZoomed(m_gameHwnd) != FALSE);
		bool iconic = (IsIconic(m_gameHwnd) != FALSE);
		if (force || zoomed || iconic || style != wantStyle || exStyle != wantEx ||
		    curRc.left != monRc.left || curRc.top != monRc.top ||
		    (curRc.right - curRc.left) != monW || (curRc.bottom - curRc.top) != monH) {
			m_inOwnResize = true;
			SetClassLongPtrA(m_gameHwnd, GCLP_HBRBACKGROUND, (LONG_PTR)GetStockObject(BLACK_BRUSH));
			if (zoomed || iconic)
				ShowWindow(m_gameHwnd, SW_SHOWNORMAL);
			SetWindowLongA(m_gameHwnd, GWL_STYLE, wantStyle);
			SetWindowLongA(m_gameHwnd, GWL_EXSTYLE, wantEx);
			m_appliedBorderlessStyle = true;
			UINT swpFlags = SWP_FRAMECHANGED | SWP_SHOWWINDOW | (force ? 0u : SWP_NOACTIVATE);
			SetWindowPos(m_gameHwnd, force ? HWND_TOP : HWND_NOTOPMOST, monRc.left, monRc.top, monW, monH, swpFlags);
			m_inOwnResize = false;
		}
		if (force) {
			m_modeGuardUntil = GetTickCount() + 400;
			m_inOwnResize = true;
			ShowWindow(m_gameHwnd, SW_SHOW);
			BringGameWindowToFront(m_gameHwnd);
			EnsureTaskbarTab(m_gameHwnd);
			m_inOwnResize = false;
		}
		RECT cr = {};
		if (GetRealClientRect(m_gameHwnd, &cr) && cr.right > cr.left && cr.bottom > cr.top) {
			m_cachedClientW = (UINT)(cr.right - cr.left);
			m_cachedClientH = (UINT)(cr.bottom - cr.top);
		}
	}

	void ApplyWindowed(UINT gameW, UINT gameH, bool forceStyle)
	{
		if (m_borderless || !IsWindow(m_gameHwnd)) return;
		RECT monRc = {}, workRc = {};
		QueryMonitorRect(m_gameHwnd, monRc, m_refreshHz, &workRc);

		// Restore standard resizable window frame if we previously promoted to borderless
		// (e.g. Alt+Enter from fullscreen -> windowed), while leaving third-party windowed
		// mods (e.g. SAMPFUNCS window-mode-no-border) untouched when they launched windowed.
		if (forceStyle || m_appliedBorderlessStyle) {
			m_appliedBorderlessStyle = false;
			m_inOwnResize = true;
			if (IsZoomed(m_gameHwnd) || IsIconic(m_gameHwnd))
				ShowWindow(m_gameHwnd, SW_RESTORE);

			LONG style = GetWindowLongA(m_gameHwnd, GWL_STYLE);
			LONG wantStyle = (style & ~(WS_POPUP | WS_MINIMIZE | WS_MAXIMIZE)) | WS_OVERLAPPEDWINDOW | WS_VISIBLE;
			LONG exStyle = GetWindowLongA(m_gameHwnd, GWL_EXSTYLE);
			LONG wantEx = (exStyle & ~(WS_EX_TOPMOST | WS_EX_TOOLWINDOW)) | WS_EX_APPWINDOW;
			SetWindowLongA(m_gameHwnd, GWL_STYLE, wantStyle);
			SetWindowLongA(m_gameHwnd, GWL_EXSTYLE, wantEx);

			int workW = workRc.right - workRc.left;
			int workH = workRc.bottom - workRc.top;
			int x = 0, y = 0, winW = 0, winH = 0;

			if (m_hasUserWinRect) {
				int uw = m_userWinRect.right - m_userWinRect.left;
				int uh = m_userWinRect.bottom - m_userWinRect.top;
				if (uw >= 320 && uh >= 240 && uw <= workW + 64 && uh <= workH + 64) {
					winW = uw;
					winH = uh;
					x = m_userWinRect.left;
					y = m_userWinRect.top;
					if (x < workRc.left - 32 || x + 120 > workRc.right)  x = workRc.left + (workW - winW) / 2;
					if (y < workRc.top  || y + 80  > workRc.bottom)      y = workRc.top  + (workH - winH) / 2;
				}
			}

			if (winW < 320 || winH < 240) {
				int cw = (int)(gameW ? gameW : 1280);
				int ch = (int)(gameH ? gameH : 720);

				RECT frameProbe = { 0, 0, 0, 0 };
				AdjustWindowRectEx(&frameProbe, wantStyle, FALSE, wantEx);
				int borderW = frameProbe.right - frameProbe.left;
				int borderH = frameProbe.bottom - frameProbe.top;
				int availW = workW - borderW;
				int availH = workH - borderH;

				if (cw > availW || ch > availH) {
					// Scale client area cleanly while preserving exact aspect ratio (and snapping 16:9 >=1080p to 1600x900)
					double aspect = (ch > 0) ? ((double)cw / (double)ch) : (16.0 / 9.0);
					if (cw >= 1920 && ch >= 1080 && fabs(aspect - (16.0 / 9.0)) < 0.02 && availW >= 1600 && availH >= 900) {
						cw = 1600;
						ch = 900;
					} else {
						double sx = (double)availW * 0.85 / (double)cw;
						double sy = (double)availH * 0.85 / (double)ch;
						double s = (sx < sy) ? sx : sy;
						if (s > 1.0) s = 1.0;
						cw = ((int)(cw * s)) & ~1;
						ch = ((int)(ch * s)) & ~1;
						if (cw < 640) cw = 640;
						if (ch < 480) ch = 480;
					}
				}

				RECT adj = { 0, 0, cw, ch };
				AdjustWindowRectEx(&adj, wantStyle, FALSE, wantEx);
				winW = adj.right - adj.left;
				winH = adj.bottom - adj.top;
				x = workRc.left + (workW - winW) / 2;
				y = workRc.top  + (workH - winH) / 2;
				if (x < workRc.left) x = workRc.left;
				if (y < workRc.top)  y = workRc.top;
			}

			m_wantWinRect = { x, y, x + winW, y + winH };
			m_hasWantWinRect = true;

			// Replace GTA SA's stale 136x39 CreateWindowEx normal placement (in workspace coords)
			// so SW_RESTORE never collapses the window.
			int offX = workRc.left - monRc.left;
			int offY = workRc.top  - monRc.top;
			WINDOWPLACEMENT wp = {};
			wp.length = sizeof(wp);
			wp.showCmd = SW_SHOWNORMAL;
			wp.rcNormalPosition = { x - offX, y - offY, x + winW - offX, y + winH - offY };
			SetWindowPlacement(m_gameHwnd, &wp);

			SetWindowPos(m_gameHwnd, HWND_NOTOPMOST, x, y, winW, winH, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
			ShowWindow(m_gameHwnd, SW_SHOW);
			BringGameWindowToFront(m_gameHwnd);
			m_inOwnResize = false;
			m_modeGuardUntil = GetTickCount() + 450;
		} else {
			m_inOwnResize = true;
			ShowWindow(m_gameHwnd, SW_SHOW);
			BringGameWindowToFront(m_gameHwnd);
			m_inOwnResize = false;
		}
		RECT cr = {};
		if (GetRealClientRect(m_gameHwnd, &cr) && cr.right > cr.left && cr.bottom > cr.top) {
			m_cachedClientW = (UINT)(cr.right - cr.left);
			m_cachedClientH = (UINT)(cr.bottom - cr.top);
		}
	}

	static bool ShouldSkipAutoMinimize(HWND gameHwnd)
	{
		HWND fg = GetForegroundWindow();
		if (!fg || fg == gameHwnd) return true;
		if (MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) != MonitorFromWindow(gameHwnd, MONITOR_DEFAULTTONEAREST))
			return true;
		LONG ex = GetWindowLongW(fg, GWL_EXSTYLE);
		if ((ex & (WS_EX_TOPMOST | WS_EX_LAYERED)) == (WS_EX_TOPMOST | WS_EX_LAYERED))
			return true;
		DWORD fgPid = 0;
		GetWindowThreadProcessId(fg, &fgPid);
		if (fgPid) {
			HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, fgPid);
			if (hp) {
				wchar_t exePath[MAX_PATH] = {};
				DWORD len = MAX_PATH;
				bool isOverlay = false;
				if (QueryFullProcessImageNameW(hp, 0, exePath, &len)) {
					const wchar_t *base = wcsrchr(exePath, L'\\');
					base = base ? base + 1 : exePath;
					if (_wcsicmp(base, L"NVIDIA Overlay.exe") == 0 ||
					    _wcsicmp(base, L"NVIDIA Share.exe") == 0 ||
					    _wcsicmp(base, L"GameBar.exe") == 0)
						isOverlay = true;
				}
				CloseHandle(hp);
				if (isOverlay) return true;
			}
		}
		return false;
	}

	static LRESULT CALLBACK BorderlessWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		ProxyDirect3DDevice9 *self = g_activeDev;
		WNDPROC prev = self ? self->m_prevWndProc : g_savedWndProc;
		bool isUnicode = self ? self->m_isUnicodeWnd : g_savedWndUnicode;
		auto callPrev = [&](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
			if (!isUnicode)
				return prev ? CallWindowProcA(prev, h, m, w, l) : DefWindowProcA(h, m, w, l);
			return prev ? CallWindowProcW(prev, h, m, w, l) : DefWindowProcW(h, m, w, l);
		};

		// Suppress Windows system-menu ding/beep right after our Alt+F10 / Alt+Enter hotkeys
		static bool s_swallowSysMenu = false;
		if (self && hwnd == self->m_gameHwnd) {
			if (msg == WM_CLOSE || msg == WM_DESTROY)
				self->m_closing = true;
			if (g_msgTaskbarCreated != 0 && msg == g_msgTaskbarCreated && !self->m_closing) {
				EnsureTaskbarTab(hwnd);
			}
			if (msg == kMsgDeferMinimize) {
				if (self->m_borderless && !self->m_closing && !self->m_isForeground && !ShouldSkipAutoMinimize(hwnd)) {
					if (!IsIconic(hwnd)) {
						self->m_inOwnResize = true;
						ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
						self->m_inOwnResize = false;
					}
					LONG st = GetWindowLongA(hwnd, GWL_STYLE);
					LONG ex = GetWindowLongA(hwnd, GWL_EXSTYLE);
					if (!(st & WS_VISIBLE) || (ex & WS_EX_TOOLWINDOW) || !(ex & WS_EX_APPWINDOW)) {
						self->m_inOwnResize = true;
						SetWindowLongA(hwnd, GWL_EXSTYLE, (ex & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW);
						if (!(st & WS_VISIBLE))
							ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
						self->m_inOwnResize = false;
					}
					EnsureTaskbarTab(hwnd);
					ClipCursor(nullptr);
				}
				return 0;
			}
			if (msg == WM_SYSCHAR && s_swallowSysMenu) {
				s_swallowSysMenu = false;
				return 0;
			}
			if (msg == WM_SYSCOMMAND) {
				UINT sc = (UINT)(wp & 0xFFF0);
				// Never let Alt / Alt+Space open the Win32 system menu modal loop (which freezes the game).
				// Alt+Tab (DWM), Alt+F4 (SC_CLOSE), Alt+Shift, and Alt+Enter/F9/F10 are unaffected.
				if (sc == SC_KEYMENU) {
					s_swallowSysMenu = false;
					return 0;
				}
				// User-initiated maximize/minimize/resize/move always overrides post-Reset settle guard
				if (sc == SC_MAXIMIZE || sc == SC_MINIMIZE || sc == SC_SIZE || sc == SC_MOVE || sc == SC_RESTORE)
					self->m_modeGuardUntil = 0;
			}
			// Prevent SA-MP / GTA SA post-Reset SetWindowPos/SetWindowPlacement from collapsing
			// the windowed game into a tiny ~136x39 titlebar with zero client area on Alt+Enter.
			if (msg == WM_GETMINMAXINFO && !self->m_borderless && lp) {
				LRESULT res = callPrev(hwnd, msg, wp, lp);
				auto *mmi = (MINMAXINFO *)lp;
				if (mmi->ptMinTrackSize.x < 320) mmi->ptMinTrackSize.x = 320;
				if (mmi->ptMinTrackSize.y < 240) mmi->ptMinTrackSize.y = 240;
				return res;
			}
			if (msg == WM_STYLECHANGING && !self->m_inOwnResize && !self->m_closing && lp) {
				auto *ss = (STYLESTRUCT *)lp;
				if (wp == (WPARAM)GWL_EXSTYLE) {
					// Never allow any mod/GTA/D3D9 call to strip WS_EX_APPWINDOW or add WS_EX_TOOLWINDOW
					// (which instantly removes the running game from the Windows taskbar).
					ss->styleNew = (ss->styleNew & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW;
				} else if (wp == (WPARAM)GWL_STYLE) {
					bool inGuard = (self->m_modeGuardUntil != 0 && (int)(self->m_modeGuardUntil - GetTickCount()) > 0);
					if (inGuard) {
						if (!self->m_borderless) {
							ss->styleNew = (ss->styleNew & ~(WS_POPUP | WS_MINIMIZE | WS_MAXIMIZE)) | WS_OVERLAPPEDWINDOW | WS_VISIBLE;
						} else {
							ss->styleNew = (ss->styleNew & ~(WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_BORDER | WS_DLGFRAME | WS_MAXIMIZE))
							               | WS_POPUP | WS_VISIBLE | WS_MINIMIZEBOX | WS_SYSMENU;
						}
					} else {
						ss->styleNew |= WS_VISIBLE;
						if (self->m_borderless)
							ss->styleNew |= (WS_MINIMIZEBOX | WS_SYSMENU);
					}
				}
			}
			if (msg == WM_WINDOWPOSCHANGING && !self->m_inOwnResize && lp) {
				auto *pos = (WINDOWPOS *)lp;
				// Block unexpected SW_HIDE / SWP_HIDEWINDOW during gameplay so the game can NEVER vanish
				// from the taskbar into an unreachable background process while minimizing/alt-tabbing.
				if (!self->m_closing && (pos->flags & SWP_HIDEWINDOW)) {
					pos->flags &= ~SWP_HIDEWINDOW;
					Log("Window: blocked SWP_HIDEWINDOW on active game window (flags=0x%X)", pos->flags);
				}
				bool iconic = (IsIconic(hwnd) != FALSE) || ((GetWindowLongA(hwnd, GWL_STYLE) & WS_MINIMIZE) != 0);
				bool isMinMove = (pos->x <= -10000 || pos->y <= -10000);
				bool inGuard = (self->m_modeGuardUntil != 0 && (int)(self->m_modeGuardUntil - GetTickCount()) > 0);
				if (!iconic && !isMinMove && !(pos->flags & SWP_HIDEWINDOW) && !(pos->flags & SWP_NOSIZE)) {
					if (!self->m_borderless && self->m_hasWantWinRect) {
						bool collapsed = (pos->cx < 320 || pos->cy < 240);
						if (collapsed || inGuard) {
							int wantW = self->m_wantWinRect.right - self->m_wantWinRect.left;
							int wantH = self->m_wantWinRect.bottom - self->m_wantWinRect.top;
							bool posDiff = !(pos->flags & SWP_NOMOVE) &&
							               (pos->x != self->m_wantWinRect.left || pos->y != self->m_wantWinRect.top);
							if (pos->cx != wantW || pos->cy != wantH || posDiff) {
								Log("Window: blocked %s windowed resize (%dx%d -> %dx%d, flags=0x%X)",
									collapsed ? "collapsed" : "post-Reset", pos->cx, pos->cy, wantW, wantH, pos->flags);
								pos->x = self->m_wantWinRect.left;
								pos->y = self->m_wantWinRect.top;
								pos->cx = wantW;
								pos->cy = wantH;
								pos->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
							}
						}
					} else if (self->m_borderless && inGuard) {
						RECT monRc = {};
						UINT hz = 60;
						QueryMonitorRect(hwnd, monRc, hz);
						int monW = monRc.right - monRc.left;
						int monH = monRc.bottom - monRc.top;
						bool posDiff = !(pos->flags & SWP_NOMOVE) &&
						               (pos->x != monRc.left || pos->y != monRc.top);
						if (pos->cx != monW || pos->cy != monH || posDiff) {
							pos->x = monRc.left;
							pos->y = monRc.top;
							pos->cx = monW;
							pos->cy = monH;
							pos->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
						}
					}
				}
			}
			if (msg == WM_ENTERSIZEMOVE) {
				self->m_inSizeMove = true;
			}
			if (msg == WM_SIZE) {
				if (wp == SIZE_MINIMIZED) {
					self->m_isMinimized = true;
				} else {
					self->m_isMinimized = false;
					self->m_cachedClientW = LOWORD(lp);
					self->m_cachedClientH = HIWORD(lp);
					if (self->m_borderless) {
						// Protect 4:3 stretched borderless from having GTA SA overwrite RsGlobal to monitor size.
						if (self->NeedsVirtualMapping())
							lp = MAKELPARAM((WORD)self->m_pp.BackBufferWidth, (WORD)self->m_pp.BackBufferHeight);
					} else if (!self->m_inSizeMove && !self->m_inOwnResize &&
					           self->m_cachedClientW >= 200 && self->m_cachedClientH >= 120 &&
					           (self->m_cachedClientW != self->m_logicalBaseW || self->m_cachedClientH != self->m_logicalBaseH)) {
						// In native 1:1 windowed mode, schedule fallback target sync if SA-MP doesn't Reset within 120ms.
						self->m_pendingWinResizeTick = GetTickCount() | 1u;
					}
				}
			}
			if (msg == WM_EXITSIZEMOVE) {
				self->m_inSizeMove = false;
				if (!self->m_borderless && !IsIconic(hwnd)) {
					if (!IsZoomed(hwnd)) {
						RECT wr = {};
						if (GetWindowRect(hwnd, &wr) && (wr.right - wr.left) >= 320 && (wr.bottom - wr.top) >= 240) {
							self->m_userWinRect = wr;
							self->m_hasUserWinRect = true;
							self->m_wantWinRect = wr;
							self->m_hasWantWinRect = true;
						}
					}
					RECT cr = {};
					if (GetRealClientRect(hwnd, &cr) && cr.right > cr.left && cr.bottom > cr.top) {
						self->m_cachedClientW = (UINT)(cr.right - cr.left);
						self->m_cachedClientH = (UINT)(cr.bottom - cr.top);
						if (self->m_cachedClientW != self->m_logicalBaseW || self->m_cachedClientH != self->m_logicalBaseH)
							self->m_pendingWinResizeTick = GetTickCount() | 1u;
					}
				}
			}
			// Alt+F9 cycles Live Before/After Split-Screen Wipe (OFF -> 50% -> Auto-Sweep -> Mouse Follow)
			if (msg == WM_SYSKEYDOWN && wp == VK_F9 && !(lp & 0x40000000)) {
				s_swallowSysMenu = true;
				g_cfg.compareMode = (g_cfg.compareMode + 1) % 4;
				ConfigNotifyChanged();
				return 0;
			}
			// Alt+Left / Alt+Right nudges split line by +/-5% while compare mode is active
			if (msg == WM_SYSKEYDOWN && (wp == VK_LEFT || wp == VK_RIGHT) && g_cfg.compareMode != 0) {
				s_swallowSysMenu = true;
				g_cfg.compareMode = 1;
				float nextPos = g_cfg.comparePos + ((wp == VK_RIGHT) ? 0.05f : -0.05f);
				if (nextPos < 0.05f) nextPos = 0.05f;
				if (nextPos > 0.95f) nextPos = 0.95f;
				g_cfg.comparePos = nextPos;
				ConfigNotifyChanged();
				return 0;
			}
			// Alt+F10 toggles minimal bare-typography D3D11 latency/FPS telemetry HUD
			if (msg == WM_SYSKEYDOWN && wp == VK_F10 && !(lp & 0x40000000)) {
				s_swallowSysMenu = true;
				g_cfg.overlay = !g_cfg.overlay;
				ConfigNotifyChanged();
				ConfigSave();
				return 0;
			}
			// Singleplayer Alt+Enter toggle between borderless fullscreen and native 1:1 windowed mode
			// (when SA-MP is loaded, let SA-MP's own Alt+Enter -> Reset(Windowed) path handle it).
			if (msg == WM_SYSKEYDOWN && wp == VK_RETURN && !(lp & 0x40000000)) {
				if (!GetModuleHandleA("samp.dll")) {
					s_swallowSysMenu = true;
					self->m_borderless = !self->m_borderless;
					self->m_pp.Windowed = self->m_borderless ? FALSE : TRUE;
					if (self->m_borderless) {
						self->ApplyBorderless(true);
						UINT baseW = self->m_fullscreenW ? self->m_fullscreenW : self->m_cachedClientW;
						UINT baseH = self->m_fullscreenH ? self->m_fullscreenH : self->m_cachedClientH;
						self->m_logicalBaseW = baseW;
						self->m_logicalBaseH = baseH;
						UINT gw = baseW, gh = baseH;
						ComputeScaledGameSize(baseW, baseH, gw, gh);
						self->m_pp.BackBufferWidth = gw;
						self->m_pp.BackBufferHeight = gh;
						self->m_presenter.ResizeSwapChain(self->m_cachedClientW, self->m_cachedClientH);
						self->CreateGameTargets(gw, gh, false);
						self->SyncGtaRenderResolution(gw, gh, false);
					} else {
						self->ApplyWindowed(self->m_fullscreenW, self->m_fullscreenH, true);
						UINT cw = self->m_cachedClientW;
						UINT ch = self->m_cachedClientH;
						if (cw >= 320 && ch >= 240) {
							self->m_logicalBaseW = cw;
							self->m_logicalBaseH = ch;
							UINT gw = cw, gh = ch;
							ComputeScaledGameSize(cw, ch, gw, gh);
							self->m_pp.BackBufferWidth = gw;
							self->m_pp.BackBufferHeight = gh;
							self->m_presenter.ResizeSwapChain(cw, ch);
							self->CreateGameTargets(gw, gh, false);
							self->SyncGtaRenderResolution(gw, gh, true);
						}
					}
					return 0;
				}
			}
		}

		if (self && hwnd == self->m_gameHwnd && msg == WM_SETFOCUS) {
			self->m_isForeground = true;
			self->m_isMinimized = false;
			self->m_deactivating = false;
			self->m_lastWinSyncTick = GetTickCount();
			self->m_pacer.Reset();
		}

		if (self && hwnd == self->m_gameHwnd && msg == WM_ACTIVATEAPP) {
			static bool s_inActivate = false;
			if (!s_inActivate) {
				s_inActivate = true;
				self->m_isForeground = (wp != 0);
				if (!wp) {
					self->m_deactivating = true;
					LRESULT res = callPrev(hwnd, msg, wp, lp);
					ClipCursor(nullptr);
					if (self->m_borderless) {
						// Defer SW_SHOWMINNOACTIVE via PostMessage instead of calling synchronous SW_MINIMIZE
						// inside WM_ACTIVATEAPP: avoids nested activation races that can cause explorer.exe
						// Taskband to drop the taskbar button after repeated Alt-Tabs / Win-D minimizes.
						if (!self->m_closing)
							PostMessageA(hwnd, kMsgDeferMinimize, 0, 0);
						ClipCursor(nullptr);
						POINT after = {};
						RECT monRc = {};
						UINT hz = 60;
						QueryMonitorRect(hwnd, monRc, hz);
						if (GetCursorPos(&after) && after.x <= monRc.left + 4 && after.y <= monRc.top + 4) {
							int tx = self->m_hasDesktopCursor ? self->m_desktopCursor.x : (monRc.left + (monRc.right - monRc.left) / 2);
							int ty = self->m_hasDesktopCursor ? self->m_desktopCursor.y : (monRc.top  + (monRc.bottom - monRc.top) / 2);
							if (g_rawSetCursorPos) g_rawSetCursorPos(tx, ty);
							else SetCursorPos(tx, ty);
						}
					}
					s_inActivate = false;
					return res;
				} else {
					self->m_everActivated = true;
					self->m_isMinimized = false;
					self->m_lastWinSyncTick = GetTickCount();
					POINT cur = {};
					if (GetCursorPos(&cur)) {
						RECT monRc = {};
						UINT hz = 60;
						QueryMonitorRect(hwnd, monRc, hz);
						if (cur.x > monRc.left + 4 || cur.y > monRc.top + 4) {
							self->m_desktopCursor = cur;
							self->m_hasDesktopCursor = true;
						}
					}
					self->m_deactivating = false;
					self->m_pacer.Reset();
					if (self->m_borderless && IsIconic(hwnd)) {
						self->m_inOwnResize = true;
						ShowWindow(hwnd, SW_RESTORE);
						self->m_inOwnResize = false;
					}
					LRESULT res = callPrev(hwnd, msg, wp, lp);
					if (self->m_borderless && !IsIconic(hwnd))
						self->ApplyBorderless(false);
					s_inActivate = false;
					return res;
				}
			}
		}

		return callPrev(hwnd, msg, wp, lp);
	}

	void HookWindowProc()
	{
		if (m_prevWndProc || !IsWindow(m_gameHwnd)) return;
		if (g_msgTaskbarCreated == 0)
			g_msgTaskbarCreated = RegisterWindowMessageA("TaskbarCreated");
		m_isUnicodeWnd = (IsWindowUnicode(m_gameHwnd) != FALSE);
		m_prevWndProc = m_isUnicodeWnd
			? (WNDPROC)SetWindowLongPtrW(m_gameHwnd, GWLP_WNDPROC, (LONG_PTR)BorderlessWndProc)
			: (WNDPROC)SetWindowLongPtrA(m_gameHwnd, GWLP_WNDPROC, (LONG_PTR)BorderlessWndProc);
		g_savedWndProc = m_prevWndProc;
		g_savedWndUnicode = m_isUnicodeWnd;
	}

	void UnhookWindowProc()
	{
		if (m_prevWndProc && IsWindow(m_gameHwnd)) {
			WNDPROC cur = m_isUnicodeWnd
				? (WNDPROC)GetWindowLongPtrW(m_gameHwnd, GWLP_WNDPROC)
				: (WNDPROC)GetWindowLongPtrA(m_gameHwnd, GWLP_WNDPROC);
			if (cur == BorderlessWndProc) {
				if (m_isUnicodeWnd) SetWindowLongPtrW(m_gameHwnd, GWLP_WNDPROC, (LONG_PTR)m_prevWndProc);
				else                SetWindowLongPtrA(m_gameHwnd, GWLP_WNDPROC, (LONG_PTR)m_prevWndProc);
			}
			m_prevWndProc = nullptr;
		}
	}

	HRESULT FinishPresent()
	{
		if (!g_cfg.stateCache) {
			if (!m_cacheBypassed) {
				m_cacheBypassed = true;
				InvalidateStateCache();
			}
		} else if (m_cacheBypassed) {
			m_cacheBypassed = false;
			InvalidateStateCache();
		}

		if (m_pendingForceWindowed) {
			m_pendingForceWindowed = false;
			m_borderless = false;
			m_pp.Windowed = TRUE;
			ApplyWindowed(m_fullscreenW, m_fullscreenH, true);
			UINT cw = m_cachedClientW, ch = m_cachedClientH;
			if (cw >= 320 && ch >= 240) {
				m_logicalBaseW = cw;
				m_logicalBaseH = ch;
				UINT scaledW = cw, scaledH = ch;
				ComputeScaledGameSize(cw, ch, scaledW, scaledH);
				if (scaledW != m_pp.BackBufferWidth || scaledH != m_pp.BackBufferHeight) {
					m_presenter.DrainAsync();
					m_pp.BackBufferWidth = scaledW;
					m_pp.BackBufferHeight = scaledH;
					m_presenter.ResizeSwapChain(cw, ch);
					CreateGameTargets(scaledW, scaledH, false);
					SyncGtaRenderResolution(scaledW, scaledH, true);
				}
			}
			Log("Bridge: applied startup windowed=1 (game %ux%u, win %ux%u)",
				m_pp.BackBufferWidth, m_pp.BackBufferHeight, m_cachedClientW, m_cachedClientH);
		}

		// Event-driven window state (updated on WM_SIZE / WM_ACTIVATEAPP), resynced defensively every 250ms
		// so we eliminate 5+ per-frame Win32 kernel transitions at 1000+ FPS.
		DWORD nowTick = GetTickCount();
		if (m_startupFrames < 30 || (nowTick - m_lastWinSyncTick) >= 250) {
			m_lastWinSyncTick = nowTick;
			m_isMinimized = (IsIconic(m_gameHwnd) != FALSE);
			m_isForeground = (GetForegroundWindow() == m_gameHwnd);
			if (!m_isMinimized) {
				RECT rc = {};
				if (GetRealClientRect(m_gameHwnd, &rc)) {
					m_cachedClientW = (UINT)(rc.right - rc.left);
					m_cachedClientH = (UINT)(rc.bottom - rc.top);
				}
			}
			// Taskbar presence watchdog: if any mod/driver hid the game window or stripped WS_EX_APPWINDOW
			// while running, restore visibility + taskbar tab immediately without stealing focus.
			if (!m_closing && IsWindow(m_gameHwnd)) {
				LONG st = GetWindowLongA(m_gameHwnd, GWL_STYLE);
				LONG ex = GetWindowLongA(m_gameHwnd, GWL_EXSTYLE);
				bool lostVis = ((st & WS_VISIBLE) == 0);
				bool badEx   = ((ex & WS_EX_TOOLWINDOW) != 0) || ((ex & WS_EX_APPWINDOW) == 0);
				if (lostVis || badEx) {
					m_inOwnResize = true;
					if (badEx)
						SetWindowLongA(m_gameHwnd, GWL_EXSTYLE, (ex & ~WS_EX_TOOLWINDOW) | WS_EX_APPWINDOW);
					if (lostVis)
						ShowWindow(m_gameHwnd, m_isMinimized ? SW_SHOWMINNOACTIVE : SW_SHOWNA);
					m_inOwnResize = false;
					EnsureTaskbarTab(m_gameHwnd);
					Log("Window: recovered missing taskbar state (lostVis=%d, badEx=%d, min=%d)",
						lostVis ? 1 : 0, badEx ? 1 : 0, m_isMinimized ? 1 : 0);
				}
			}
		}

		if (m_deactivating && !m_isMinimized && m_isForeground)
			m_deactivating = false;

		if (m_startupFrames < 30) {
			++m_startupFrames;
			if (m_borderless)
				ApplyBorderless(m_startupFrames == 1);
			if (!m_everActivated && m_isForeground)
				m_everActivated = true;
		}

		// Keep GTA SA's RwCamera framebuffer locked to SSAA/native dimensions once Scene.camera is created.
		if (IsGtaSaUs10() && m_pp.BackBufferWidth > 0 && m_pp.BackBufferHeight > 0 &&
		    (m_pp.BackBufferWidth != m_logicalBaseW || m_pp.BackBufferHeight != m_logicalBaseH || !m_borderless)) {
			bool camMismatch = false;
			__try {
				const BYTE *cam = *(const BYTE *const *)0x00C1703C;
				if (cam) {
					const BYTE *fb = *(const BYTE *const *)(cam + 0x60);
					if (fb && (*(const UINT *)(fb + 0x0C) != m_pp.BackBufferWidth ||
					           *(const UINT *)(fb + 0x10) != m_pp.BackBufferHeight)) {
						camMismatch = true;
					}
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			if (camMismatch)
				SyncGtaRenderResolution(m_pp.BackBufferWidth, m_pp.BackBufferHeight, !m_borderless);
		}

		if (!m_isMinimized) {
			UINT cw = m_cachedClientW;
			UINT ch = m_cachedClientH;
			// Safety net: if a third-party WndProc subclass still collapsed the windowed frame
			// to a tiny titlebar-only rect (0px client), immediately restore m_wantWinRect.
			if (!m_borderless && m_hasWantWinRect && (cw < 200 || ch < 120)) {
				int wantW = m_wantWinRect.right - m_wantWinRect.left;
				int wantH = m_wantWinRect.bottom - m_wantWinRect.top;
				Log("Window: recovered collapsed client %ux%u -> %dx%d", cw, ch, wantW, wantH);
				m_inOwnResize = true;
				SetWindowPos(m_gameHwnd, HWND_NOTOPMOST, m_wantWinRect.left, m_wantWinRect.top,
				             wantW, wantH, SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
				m_inOwnResize = false;
				RECT rc = {};
				if (GetRealClientRect(m_gameHwnd, &rc)) {
					cw = m_cachedClientW = (UINT)(rc.right - rc.left);
					ch = m_cachedClientH = (UINT)(rc.bottom - rc.top);
				}
			}
			if (cw > 0 && ch > 0 && (cw != m_presenter.SwapWidth() || ch != m_presenter.SwapHeight()))
				m_presenter.ResizeSwapChain(cw, ch);

			// Native 1:1 windowed fallback sync (for singleplayer or if SA-MP didn't Reset after Maximize/Resize)
			if (!m_borderless && !m_inSizeMove && m_pendingWinResizeTick != 0 &&
			    (nowTick - m_pendingWinResizeTick) >= 120) {
				m_pendingWinResizeTick = 0;
				if (cw >= 200 && ch >= 120 && (cw != m_logicalBaseW || ch != m_logicalBaseH)) {
					m_logicalBaseW = cw;
					m_logicalBaseH = ch;
					UINT scaledW = cw, scaledH = ch;
					ComputeScaledGameSize(cw, ch, scaledW, scaledH);
					m_presenter.DrainAsync();
					m_pp.BackBufferWidth = scaledW;
					m_pp.BackBufferHeight = scaledH;
					m_presenter.ResizeSwapChain(cw, ch);
					if (CreateGameTargets(scaledW, scaledH, false))
						SyncGtaRenderResolution(scaledW, scaledH, true);
					Log("Window: native resize synced (game %ux%u, win %ux%u)", scaledW, scaledH, cw, ch);
				}
			}
		}

		// Live target rebuild on runtime config changes (/md ao, /md ssaa, /md af, /md sharp, /md lut)
		LONG cfgSeq = InterlockedCompareExchange(&g_cfgSeq, 0, 0);
		if (cfgSeq != m_proxyCfgSeq) {
			m_proxyCfgSeq = cfgSeq;
			memset(m_ssValid, 0, sizeof(m_ssValid)); // Apply updated AF immediately
			const bool wantDepth  = WantDepthTargets();
			const bool want3dMask = Want3dMaskTargets();
			UINT wantW = m_logicalBaseW ? m_logicalBaseW : m_pp.BackBufferWidth;
			UINT wantH = m_logicalBaseH ? m_logicalBaseH : m_pp.BackBufferHeight;
			ComputeScaledGameSize(wantW, wantH, wantW, wantH);
			bool needRebuild = (wantW != m_pp.BackBufferWidth || wantH != m_pp.BackBufferHeight) ||
			                   (wantDepth != (m_intzDepthTex != nullptr)) ||
				                   (want3dMask && !m_shared3dTex[0]);
			if (needRebuild && wantW >= 320 && wantH >= 240) {
				m_presenter.DrainAsync();
				m_pp.BackBufferWidth = wantW;
				m_pp.BackBufferHeight = wantH;
				// Never call ResetEx mid-game (allowResetEx=false): live rebuild uses safe offscreen/INTZ
				// targets so GTA SA / SA-MP / SAMPFUNCS D3DPOOL_DEFAULT resources stay 100% intact.
				if (CreateGameTargets(wantW, wantH, false))
					SyncGtaRenderResolution(wantW, wantH, !m_borderless);
				Log("Graphics: live targets rebuilt (%ux%u, ao=%d, intz=%d, 3dmask=%d, ssaa=%.2f)",
					wantW, wantH, g_cfg.aoQuality, m_intzDepthTex ? 1 : 0,
					m_shared3dTex[0] ? 1 : 0, g_cfg.renderScale);
			}
		}

		FlushUpBatch();
		int slot = m_writeSlot;
		if (m_sharedSurf[slot]) {
			m_presenter.WaitForSlot(slot);
			if (m_backBufferSurf && m_backBufferSurf != m_sharedSurf[slot])
				m_real->StretchRect(m_backBufferSurf, nullptr, m_sharedSurf[slot], nullptr, D3DTEXF_NONE);

			if (m_evQuery[slot]) {
				m_evQuery[slot]->Issue(D3DISSUE_END);
				m_evQuery[slot]->GetData(nullptr, 0, D3DGETDATA_FLUSH);
			}
		}

		// Advance the real D3D9Ex swapchain frame on the hidden dummy window (~3 us, S_PRESENT_OCCLUDED)
		// so NVIDIA driver per-frame tracking (e.g. Inspector HBAO+) sees real frame boundaries.
		if (m_realUsesSwapchain) {
			if (g_origD3D9Present)
				g_origD3D9Present(m_real, nullptr, nullptr, nullptr, nullptr);
			else
				m_real->PresentEx(nullptr, nullptr, nullptr, nullptr, 0);
		}
		if (m_directFallbackSurf) {
			const int nextSlot = slot ^ 1;
			if (g_cfg.asyncPresent) {
				m_presenter.WaitForSlot(nextSlot);
				if (FAILED(SwitchBackBufferSurface(m_sharedSurf[nextSlot])) && !DisableDirectRendering())
					return D3DERR_DEVICELOST;
			} else if (!DisableDirectRendering()) {
				return D3DERR_DEVICELOST;
			}
		}

		m_frontSlot = slot;
		uint64_t fid = m_curFrameId;
		int64_t inQpc = m_curInputQpc;
		if (!m_hasCameraParamsThisFrame) {
			m_curCamParams = {};
			if (g_cfg.rainFx)
				ReadGtaCameraParams(m_curCamParams);
		}
		if (m_hasDepthThisFrame && m_has3dMaskThisFrame && WantRoadPuddles() && IsGtaSaUs10() &&
		    m_curCamParams.hasViewProj && !m_curCamParams.inInterior && !m_curCamParams.sheltered &&
		    (m_curCamParams.rain > 0.05f || m_curCamParams.wetRoads > 0.65f)) {
			RoadSurfaceGrid &grid = m_presenter.RoadGridForSlot(slot);
			m_curCamParams.hasRoadGrid = m_roadSurfaceCache.Update(m_curCamParams.viewMat, grid);
			if (m_actorCaptured) {
				memcpy(m_curCamParams.actorCrop, m_playerMirror.crop, sizeof(m_curCamParams.actorCrop));
				memcpy(m_curCamParams.actorGround, m_actorGround, sizeof(m_curCamParams.actorGround));
			}
		}
		m_presenter.SubmitFrame(slot, fid, g_cfg.asyncPresent, m_isMinimized, inQpc,
		                        m_has3dMaskThisFrame, m_hasDepthThisFrame, &m_curCamParams, m_frontEndFrame);
		m_writeSlot ^= 1;

		// Upload up to 4 background-decoded mipmapped textures during async D3D11 overlap (0ms frame-time hitch!)
		DrainAsyncMipUploads(4);

		// High-precision hybrid frame pacer placed right after async Present handoff
		// and right before NextFrameId() / input sampling for lowest possible input latency.
		double targetFps = 0.0;
		bool inBackground = m_isMinimized || (m_everActivated && !m_isForeground);
		if (inBackground && g_cfg.bgFpsLimit > 0) {
			targetFps = (double)g_cfg.bgFpsLimit;
		} else if (g_cfg.fpsLimit < 0) {
			targetFps = (m_refreshHz > 40) ? (double)(m_refreshHz - 3) : 60.0;
		} else if (g_cfg.fpsLimit > 0) {
			targetFps = (double)g_cfg.fpsLimit;
		}
		m_pacer.Pace(targetFps);

		m_curFrameId = m_presenter.Reflex().NextFrameId();
		SampTick();
		if (IsGtaSaUs10()) UpdateRainLighting(g_cfg.rainLighting);
		m_renderStartedThisFrame = false;
		m_inputSampledThisFrame = false;
		m_captured3dThisFrame = false;
		m_actorCaptured = false;
		m_has3dMaskThisFrame = false;
		m_hasDepthThisFrame = false;
		m_hasCameraParamsThisFrame = false;
		m_hasSceneMats = false;
		m_checkedDrawTransforms = false;
		m_curInputQpc = 0;
		return S_OK;
	}

	HRESULT EndSceneInternal()
	{
		FlushUpBatch();
		if (g_origD3D9EndScene)
			return g_origD3D9EndScene(m_real);
		HRESULT hr = m_real->EndScene();
		if (g_d3d9EndSceneEntry && (*(BYTE *)g_d3d9EndSceneEntry == 0xE9 || *(BYTE *)g_d3d9EndSceneEntry == 0xEB))
			InvalidateStateCache();
		return hr;
	}

	HRESULT ResetInternal(D3DPRESENT_PARAMETERS *pPP)
	{
		if (!pPP) return D3DERR_INVALIDCALL;
		// Always drain in-flight async D3D11 work before touching window, swapchain, or D3D9 targets.
		m_presenter.DrainAsync();
		RECT monRc = {};
		QueryMonitorRect(m_gameHwnd, monRc, m_refreshHz);
		UINT monW = (UINT)(monRc.right - monRc.left);
		UINT monH = (UINT)(monRc.bottom - monRc.top);

		UINT reqW = pPP->BackBufferWidth;
		UINT reqH = pPP->BackBufferHeight;
		// Prevent double SSAA multiplication when SA-MP/GTA SA passes back our already-scaled m_pp dimensions.
		if (m_logicalBaseW > 0 && m_logicalBaseH > 0 &&
		    reqW == m_pp.BackBufferWidth && reqH == m_pp.BackBufferHeight &&
		    (m_pp.BackBufferWidth != m_logicalBaseW || m_pp.BackBufferHeight != m_logicalBaseH)) {
			reqW = m_logicalBaseW;
			reqH = m_logicalBaseH;
		}
		UINT newW = reqW ? reqW : (m_fullscreenW ? m_fullscreenW : monW);
		UINT newH = reqH ? reqH : (m_fullscreenH ? m_fullscreenH : monH);

		bool wasBorderless = m_borderless;
		bool callerWin = (pPP->Windowed != FALSE);
		bool inStartup = (m_startupFrames < 30) || ((GetTickCount() - m_initTick) < 2500);
		if (m_forceWinInverted) {
			if (inStartup) {
				m_pendingForceWindowed = false;
				m_borderless = false;
				m_lastCallerWindowed = callerWin;
			} else if (callerWin != m_lastCallerWindowed) {
				// First Alt+Enter after startup windowed=1 toggles cleanly to borderless, and back on next press.
				m_lastCallerWindowed = callerWin;
				m_borderless = !m_borderless;
			}
		} else {
			m_lastCallerWindowed = callerWin;
			m_borderless = !callerWin;
		}

		// Preserve user's borderless/fullscreen resolution across Windowed <-> Borderless Alt+Enter toggles.
		if (m_borderless) {
			if (!wasBorderless && m_fullscreenW > 0 && m_fullscreenH > 0 &&
			    (reqW == 0 || (reqW == m_cachedClientW && reqH == m_cachedClientH))) {
				newW = m_fullscreenW;
				newH = m_fullscreenH;
			} else if (reqW > 0 && reqH > 0) {
				m_fullscreenW = reqW;
				m_fullscreenH = reqH;
			}
		}

		// Keep m_pp populated BEFORE ApplyWindowed/ApplyBorderless so synchronous WM_SIZE never sees 0x0.
		m_requestedMsaa  = pPP->MultiSampleType;
		m_requestedMsaaQ = pPP->MultiSampleQuality;
		m_pp = *pPP;
		m_pp.Windowed = m_borderless ? FALSE : TRUE;
		m_pp.BackBufferWidth = newW;
		m_pp.BackBufferHeight = newH;
		m_pendingWinResizeTick = 0;

		if (m_borderless) {
			ApplyBorderless(!wasBorderless);
		} else {
			ApplyWindowed(m_fullscreenW ? m_fullscreenW : newW, m_fullscreenH ? m_fullscreenH : newH,
			              wasBorderless || (m_forceWinInverted && inStartup));
		}

		RECT clientRc = {};
		GetRealClientRect(m_gameHwnd, &clientRc);
		UINT swapW = (clientRc.right > clientRc.left) ? (UINT)(clientRc.right - clientRc.left) : newW;
		UINT swapH = (clientRc.bottom > clientRc.top) ? (UINT)(clientRc.bottom - clientRc.top) : newH;

		// In windowed mode, base logical resolution matches client 1:1; optional SSAA render_scale
		// scales the internal game target cleanly on top of baseW x baseH.
		if (!m_borderless && swapW >= 200 && swapH >= 120) {
			newW = swapW;
			newH = swapH;
		}
		m_logicalBaseW = newW;
		m_logicalBaseH = newH;
		ComputeScaledGameSize(m_logicalBaseW, m_logicalBaseH, newW, newH);

		if (!m_borderless) {
			pPP->Windowed = TRUE;
			pPP->FullScreen_RefreshRateInHz = 0;
		} else {
			pPP->Windowed = FALSE;
		}
		pPP->BackBufferWidth  = newW;
		pPP->BackBufferHeight = newH;
		m_pp.Windowed = m_borderless ? FALSE : TRUE;
		m_pp.BackBufferWidth  = newW;
		m_pp.BackBufferHeight = newH;

		if (m_defaultState) m_defaultState->Apply();
		m_presenter.ResizeSwapChain(swapW, swapH);

		D3DFORMAT wantDs = (m_pp.EnableAutoDepthStencil && m_pp.AutoDepthStencilFormat != D3DFMT_UNKNOWN)
			? m_pp.AutoDepthStencilFormat : D3DFMT_D24S8;
		const bool wantDepth  = WantDepthTargets();
		const bool want3dMask = Want3dMaskTargets();
		const D3DMULTISAMPLE_TYPE effMsaa = wantDepth ? D3DMULTISAMPLE_NONE : m_requestedMsaa;
		bool sameRealTargets = !wantDepth && m_realUsesSwapchain && m_backBufferSurf && m_depthSurf &&
		                       m_sharedTex[0] && m_sharedTex[1] &&
		                       (!want3dMask || m_shared3dTex[0] != nullptr) &&
		                       m_realPP.BackBufferWidth == newW && m_realPP.BackBufferHeight == newH &&
		                       m_realPP.MultiSampleType == effMsaa &&
		                       m_realPP.MultiSampleQuality == m_requestedMsaaQ &&
		                       m_realPP.AutoDepthStencilFormat == wantDs;

		if (sameRealTargets) {
			m_real->SetRenderTarget(0, m_backBufferSurf);
			m_real->SetDepthStencilSurface(m_depthSurf);
			D3DVIEWPORT9 vp = { 0, 0, newW, newH, 0.0f, 1.0f };
			m_real->SetViewport(&vp);
			m_real->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
			InvalidateStateCache();
		} else {
			if (!CreateGameTargets(newW, newH, true))
				return D3DERR_OUTOFVIDEOMEMORY;
		}

		// Sync GTA SA's RsGlobal, RwCamera, and exclusive-mode flag so CameraSize(NULL) never reverts us.
		SyncGtaRenderResolution(newW, newH, !m_borderless);

		m_cachedClientW = swapW;
		m_cachedClientH = swapH;
		m_isMinimized = (IsIconic(m_gameHwnd) != FALSE);
		m_isForeground = (GetForegroundWindow() == m_gameHwnd);
		m_lastWinSyncTick = GetTickCount();

		m_pacer.Reset();
		Log("Bridge: Reset handled cleanly (mode=%s, game %ux%u, swap %ux%u, MSAA=%d, realBB=%d)",
			m_borderless ? "borderless" : "windowed", newW, newH, swapW, swapH,
			(int)m_pp.MultiSampleType, m_realUsesSwapchain ? 1 : 0);
		return S_OK;
	}

	IDirect3DSurface9 *ActiveBackBuffer() const { return m_backBufferSurf; }
	IDirect3DSurface9 *ActiveDepthSurf()  const { return m_depthSurf; }

	HRESULT GetBackBufferInternal(UINT iBackBuffer, IDirect3DSurface9 **ppBackBuffer)
	{
		if (!ppBackBuffer || iBackBuffer != 0 || !m_backBufferSurf) return D3DERR_INVALIDCALL;
		if (m_proxyBackBuffer) {
			*ppBackBuffer = m_proxyBackBuffer;
			m_proxyBackBuffer->AddRef();
			return S_OK;
		}
		*ppBackBuffer = m_backBufferSurf;
		m_backBufferSurf->AddRef();
		return S_OK;
	}

	HRESULT SwitchBackBufferSurface(IDirect3DSurface9 *next)
	{
		if (!next || !m_backBufferSurf) return D3DERR_INVALIDCALL;
		IDirect3DSurface9 *bound = nullptr;
		HRESULT hr = m_real->GetRenderTarget(0, &bound);
		if (FAILED(hr)) return hr;
		const bool isBound = bound == m_backBufferSurf;
		SafeRelease(bound);
		if (isBound) {
			D3DVIEWPORT9 viewport = {};
			hr = m_real->GetViewport(&viewport);
			if (FAILED(hr)) return hr;
			hr = m_real->SetRenderTarget(0, next);
			if (FAILED(hr)) return hr;
			m_real->SetViewport(&viewport);
		}
		next->AddRef();
		SafeRelease(m_backBufferSurf);
		m_backBufferSurf = next;
	m_dirtySinceClear = true;
		return S_OK;
	}

	bool DisableDirectRendering()
	{
		if (FAILED(SwitchBackBufferSurface(m_directFallbackSurf))) return false;
		SafeRelease(m_directFallbackSurf);
		Log("Bridge: returned to copied backbuffer");
		return true;
	}

	HRESULT GetFrontBufferDataInternal(IDirect3DSurface9 *pDestSurface)
	{
		// m_sharedSurf[m_frontSlot] holds the last complete presented front buffer
		// (including SA-MP chat, nametags, dialogs, and HUD).
		IDirect3DSurface9 *frontSurf = m_sharedSurf[m_frontSlot];
		if (!pDestSurface || !frontSurf) return D3DERR_INVALIDCALL;
		IDirect3DSurface9 *dst = UnwrapSurfaceForSys(pDestSurface);
		D3DSURFACE_DESC dstDesc = {}, srcDesc = {};
		if (FAILED(dst->GetDesc(&dstDesc)) || FAILED(frontSurf->GetDesc(&srcDesc)))
			return D3DERR_INVALIDCALL;

		// GetRenderTargetData only accepts D3DPOOL_SYSTEMMEM; SA-MP F8 uses D3DPOOL_SCRATCH.
		if (dstDesc.Pool == D3DPOOL_SYSTEMMEM &&
		    dstDesc.Width == srcDesc.Width && dstDesc.Height == srcDesc.Height && dstDesc.Format == srcDesc.Format) {
			HRESULT hr = m_real->GetRenderTargetData(frontSurf, dst);
			if (SUCCEEDED(hr)) MarkSurfaceDirtyIfManaged(pDestSurface);
			return hr;
		}

		// Reuse persistent SYSTEMMEM staging surface across F8 captures (avoids 8 MB alloc/free hitch)
		if (!m_sysReadbackSurf || m_sysReadbackW != srcDesc.Width || m_sysReadbackH != srcDesc.Height || m_sysReadbackFmt != srcDesc.Format) {
			SafeRelease(m_sysReadbackSurf);
			HRESULT chr = m_real->CreateOffscreenPlainSurface(srcDesc.Width, srcDesc.Height, srcDesc.Format, D3DPOOL_SYSTEMMEM, &m_sysReadbackSurf, nullptr);
			if (FAILED(chr) || !m_sysReadbackSurf) return chr;
			m_sysReadbackW = srcDesc.Width;
			m_sysReadbackH = srcDesc.Height;
			m_sysReadbackFmt = srcDesc.Format;
		}

		HRESULT hr = m_real->GetRenderTargetData(frontSurf, m_sysReadbackSurf);
		if (SUCCEEDED(hr)) {
			D3DLOCKED_RECT lrS = {}, lrD = {};
			if (SUCCEEDED(m_sysReadbackSurf->LockRect(&lrS, nullptr, D3DLOCK_READONLY)) && SUCCEEDED(dst->LockRect(&lrD, nullptr, 0))) {
				if (srcDesc.Width == dstDesc.Width && srcDesc.Height == dstDesc.Height) {
					UINT rowBytes = srcDesc.Width * 4;
					if ((UINT)lrS.Pitch == rowBytes && (UINT)lrD.Pitch == rowBytes) {
						memcpy(lrD.pBits, lrS.pBits, (size_t)rowBytes * srcDesc.Height);
					} else {
						for (UINT y = 0; y < srcDesc.Height; ++y)
							memcpy((BYTE *)lrD.pBits + y * lrD.Pitch, (const BYTE *)lrS.pBits + y * lrS.Pitch, rowBytes);
					}
				} else if (!m_borderless) {
					// Match native D3D9 GetFrontBufferData desktop layout in windowed mode: place the
					// windowed game frame at its screen client origin (pt.x, pt.y) so SA-MP's F8 pSrcRect
					// crop captures 100% of the exact game client area with zero offset.
					POINT pt = { 0, 0 };
					if (IsWindow(m_gameHwnd))
						ClientToScreen(m_gameHwnd, &pt);
					for (UINT y = 0; y < dstDesc.Height; ++y)
						memset((BYTE *)lrD.pBits + (size_t)y * (size_t)lrD.Pitch, 0, (size_t)dstDesc.Width * 4u);
					LONG sx0 = (pt.x < 0) ? -pt.x : 0;
					LONG sy0 = (pt.y < 0) ? -pt.y : 0;
					LONG dx0 = (pt.x > 0) ?  pt.x : 0;
					LONG dy0 = (pt.y > 0) ?  pt.y : 0;
					LONG copyW = (LONG)srcDesc.Width - sx0;
					LONG copyH = (LONG)srcDesc.Height - sy0;
					if (dx0 + copyW > (LONG)dstDesc.Width)  copyW = (LONG)dstDesc.Width - dx0;
					if (dy0 + copyH > (LONG)dstDesc.Height) copyH = (LONG)dstDesc.Height - dy0;
					if (copyW > 0 && copyH > 0) {
						const size_t copyBytes = (size_t)copyW * 4u;
						for (LONG y = 0; y < copyH; ++y) {
							memcpy((BYTE *)lrD.pBits + (size_t)(dy0 + y) * (size_t)lrD.Pitch + (size_t)dx0 * 4u,
							       (const BYTE *)lrS.pBits + (size_t)(sy0 + y) * (size_t)lrS.Pitch + (size_t)sx0 * 4u,
							       copyBytes);
						}
					}
				} else {
					// Nearest-scale into desktop-sized screenshot buffer (e.g. stretched 4:3 -> 1920x1080)
					for (UINT y = 0; y < dstDesc.Height; ++y) {
						UINT sy = (UINT)((uint64_t)y * srcDesc.Height / dstDesc.Height);
						const DWORD *srcRow = (const DWORD *)((const BYTE *)lrS.pBits + sy * lrS.Pitch);
						DWORD *dstRow = (DWORD *)((BYTE *)lrD.pBits + y * lrD.Pitch);
						for (UINT x = 0; x < dstDesc.Width; ++x) {
							UINT sx = (UINT)((uint64_t)x * srcDesc.Width / dstDesc.Width);
							dstRow[x] = srcRow[sx];
						}
					}
				}
				dst->UnlockRect();
				m_sysReadbackSurf->UnlockRect();
				MarkSurfaceDirtyIfManaged(pDestSurface);
			} else {
				hr = D3DERR_INVALIDCALL;
			}
		}
		return hr;
	}

	HRESULT GetDisplayModeInternal(D3DDISPLAYMODE *pMode)
	{
		if (!pMode) return D3DERR_INVALIDCALL;
		pMode->Width = m_pp.BackBufferWidth;
		pMode->Height = m_pp.BackBufferHeight;
		pMode->RefreshRate = m_refreshHz;
		pMode->Format = D3DFMT_X8R8G8B8;
		return S_OK;
	}

	HRESULT GetPresentParametersInternal(D3DPRESENT_PARAMETERS *pPP)
	{
		if (!pPP) return D3DERR_INVALIDCALL;
		*pPP = m_pp;
		return S_OK;
	}

	// ---- IUnknown ----
	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) override
	{
		if (!ppvObj) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IDirect3DDevice9) {
			*ppvObj = this;
			AddRef();
			return S_OK;
		}
		*ppvObj = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG r = (ULONG)InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return r;
	}

	// ---- IDirect3DDevice9 ----
	HRESULT STDMETHODCALLTYPE TestCooperativeLevel() override { return S_OK; }
	UINT    STDMETHODCALLTYPE GetAvailableTextureMem() override { return m_real->GetAvailableTextureMem(); }
	HRESULT STDMETHODCALLTYPE EvictManagedResources() override { return S_OK; }
	HRESULT STDMETHODCALLTYPE GetDirect3D(IDirect3D9 **ppD3D9) override
	{
		if (!ppD3D9) return D3DERR_INVALIDCALL;
		*ppD3D9 = m_proxyD3D;
		m_proxyD3D->AddRef();
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE GetDeviceCaps(D3DCAPS9 *pCaps) override { return m_real->GetDeviceCaps(pCaps); }
	HRESULT STDMETHODCALLTYPE GetDisplayMode(UINT iSwapChain, D3DDISPLAYMODE *pMode) override
	{
		return (iSwapChain == 0) ? GetDisplayModeInternal(pMode) : m_real->GetDisplayMode(iSwapChain, pMode);
	}
	HRESULT STDMETHODCALLTYPE GetCreationParameters(D3DDEVICE_CREATION_PARAMETERS *pParams) override
	{
		if (!pParams) return D3DERR_INVALIDCALL;
		pParams->AdapterOrdinal = m_adapter;
		pParams->DeviceType     = m_devType;
		pParams->hFocusWindow   = m_gameHwnd;
		pParams->BehaviorFlags  = m_behaviorFlags;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetCursorProperties(UINT XHotSpot, UINT YHotSpot, IDirect3DSurface9 *pCursorBitmap) override { return m_real->SetCursorProperties(XHotSpot, YHotSpot, UnwrapSurfaceForGpu(pCursorBitmap)); }
	void    STDMETHODCALLTYPE SetCursorPosition(int X, int Y, DWORD Flags) override
	{
		if (m_deactivating || IsIconic(m_gameHwnd) || GetForegroundWindow() != m_gameHwnd) return;
		m_real->SetCursorPosition(X, Y, Flags);
	}
	BOOL    STDMETHODCALLTYPE ShowCursor(BOOL bShow) override { return m_real->ShowCursor(bShow); }
	HRESULT STDMETHODCALLTYPE CreateAdditionalSwapChain(D3DPRESENT_PARAMETERS *pPP, IDirect3DSwapChain9 **pSwapChain) override { return m_real->CreateAdditionalSwapChain(pPP, pSwapChain); }
	HRESULT STDMETHODCALLTYPE GetSwapChain(UINT iSwapChain, IDirect3DSwapChain9 **pSwapChain) override
	{
		if (!pSwapChain) return D3DERR_INVALIDCALL;
		if (iSwapChain == 0) {
			*pSwapChain = new ProxySwapChain9(this);
			return S_OK;
		}
		return m_real->GetSwapChain(iSwapChain, pSwapChain);
	}
	UINT    STDMETHODCALLTYPE GetNumberOfSwapChains() override { return 1; }
	HRESULT STDMETHODCALLTYPE Reset(D3DPRESENT_PARAMETERS *pPP) override
	{
		if (!pPP) return D3DERR_INVALIDCALL;
		if (g_d3d9ResetEntry && g_origD3D9Reset && !m_inReset) {
			m_inReset = true;
			HRESULT hr = g_d3d9ResetEntry(this, pPP);
			m_inReset = false;
			return hr;
		}
		return ResetInternal(pPP);
	}
	HRESULT STDMETHODCALLTYPE Present(CONST RECT *pSrc, CONST RECT *pDst, HWND hWnd, CONST RGNDATA *pDirty) override
	{
		if (g_d3d9PresentEntry && g_origD3D9Present && !m_inPresent) {
			m_inPresent = true;
			HRESULT hr = g_d3d9PresentEntry(this, pSrc, pDst, hWnd, pDirty);
			m_inPresent = false;
			return hr;
		}
		return FinishPresent();
	}
	HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **ppBackBuffer) override
	{
		return (iSwapChain == 0) ? GetBackBufferInternal(iBackBuffer, ppBackBuffer) : D3DERR_INVALIDCALL;
	}
	HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus) override { return m_real->GetRasterStatus(iSwapChain, pRasterStatus); }
	HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL) override { return S_OK; }
	void    STDMETHODCALLTYPE SetGammaRamp(UINT, DWORD, CONST D3DGAMMARAMP *pRamp) override
	{
		if (pRamp) {
			m_gamma = *pRamp;
			m_presenter.SetGammaRamp(*pRamp);
		}
	}
	void    STDMETHODCALLTYPE GetGammaRamp(UINT, D3DGAMMARAMP *pRamp) override { if (pRamp) *pRamp = m_gamma; }

	HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) override
	{
		memset(m_texValid, 0, sizeof(m_texValid)); // Guard against heap ABA pointer reuse
		if (Pool == D3DPOOL_MANAGED)
			return CreateManagedTexture9(m_real, this, Width, Height, Levels, Usage, Format, ppTexture);
		HRESULT hr = m_real->CreateTexture(Width, Height, Levels, Usage, Format, Pool, ppTexture, pSharedHandle);
		if (FAILED(hr) && m_errLogs < 16) {
			++m_errLogs;
			Log("D3D9Ex: CreateTexture failed hr=0x%08lX (%ux%u lvls=%u usage=0x%lX fmt=%u pool=%u)", hr, Width, Height, Levels, Usage, (unsigned)Format, (unsigned)Pool);
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE CreateVolumeTexture(UINT Width, UINT Height, UINT Depth, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DVolumeTexture9 **ppVolumeTexture, HANDLE *pSharedHandle) override
	{
		memset(m_texValid, 0, sizeof(m_texValid));
		if (Pool == D3DPOOL_MANAGED)
			return CreateManagedVolumeTexture9(m_real, this, Width, Height, Depth, Levels, Usage, Format, ppVolumeTexture);
		return m_real->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle);
	}
	HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle) override
	{
		memset(m_texValid, 0, sizeof(m_texValid));
		if (Pool == D3DPOOL_MANAGED)
			return CreateManagedCubeTexture9(m_real, this, EdgeLength, Levels, Usage, Format, ppCubeTexture);
		return m_real->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
	}
	HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle) override
	{
		if (Pool == D3DPOOL_SYSTEMMEM && !(Usage & D3DUSAGE_WRITEONLY)) m_hasCpuMeshVertices = true;
		memset(m_streamValid, 0, sizeof(m_streamValid));
		if (Pool == D3DPOOL_MANAGED) Pool = D3DPOOL_DEFAULT;
		return m_real->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
	}
	HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) override
	{
		if (Pool == D3DPOOL_SYSTEMMEM && !(Usage & D3DUSAGE_WRITEONLY)) m_hasCpuMeshIndices = true;
		m_ibValid = false;
		if (Pool == D3DPOOL_MANAGED) Pool = D3DPOOL_DEFAULT;
		return m_real->CreateIndexBuffer(Length, Usage, Format, Pool, ppIndexBuffer, pSharedHandle);
	}
	HRESULT STDMETHODCALLTYPE CreateRenderTarget(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Lockable, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) override
	{
		return m_real->CreateRenderTarget(Width, Height, Format, MultiSample, MultisampleQuality, Lockable, ppSurface, pSharedHandle);
	}
	HRESULT STDMETHODCALLTYPE CreateDepthStencilSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DMULTISAMPLE_TYPE MultiSample, DWORD MultisampleQuality, BOOL Discard, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) override
	{
		return m_real->CreateDepthStencilSurface(Width, Height, Format, MultiSample, MultisampleQuality, Discard, ppSurface, pSharedHandle);
	}
	IDirect3DSurface9 *ResolveSurfaceForGpu(IDirect3DSurface9 *surf) const
	{
		if (!surf) return nullptr;
		if (m_proxyBackBuffer && surf == static_cast<IDirect3DSurface9 *>(m_proxyBackBuffer))
			return m_backBufferSurf;
		if (m_proxyDepthSurf && surf == static_cast<IDirect3DSurface9 *>(m_proxyDepthSurf))
			return m_depthSurf;
		return UnwrapSurfaceForGpu(surf);
	}

	IDirect3DSurface9 *ResolveSurfaceForSys(IDirect3DSurface9 *surf) const
	{
		if (!surf) return nullptr;
		if (m_proxyBackBuffer && surf == static_cast<IDirect3DSurface9 *>(m_proxyBackBuffer))
			return m_backBufferSurf;
		if (m_proxyDepthSurf && surf == static_cast<IDirect3DSurface9 *>(m_proxyDepthSurf))
			return m_depthSurf;
		return UnwrapSurfaceForSys(surf);
	}

	HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface, CONST POINT *pDestPoint) override
	{
		FlushUpBatch();
		return m_real->UpdateSurface(ResolveSurfaceForSys(pSourceSurface), pSourceRect,
		                             ResolveSurfaceForGpu(pDestinationSurface), pDestPoint);
	}
	HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture) override
	{
		FlushUpBatch();
		return m_real->UpdateTexture(UnwrapBaseTextureForSys(pSourceTexture), UnwrapBaseTextureForGpu(pDestinationTexture));
	}
	HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) override
	{
		FlushUpBatch();
		IDirect3DSurface9 *src = ResolveSurfaceForGpu(pRenderTarget);
		int slot = m_writeSlot;
		if (src && src == m_backBufferSurf && m_pp.MultiSampleType != D3DMULTISAMPLE_NONE && m_sharedSurf[slot]) {
			m_presenter.WaitForSlot(slot);
			m_real->StretchRect(m_backBufferSurf, nullptr, m_sharedSurf[slot], nullptr, D3DTEXF_NONE);
			src = m_sharedSurf[slot];
		}
		HRESULT hr = m_real->GetRenderTargetData(src, ResolveSurfaceForSys(pDestSurface));
		if (SUCCEEDED(hr)) MarkSurfaceDirtyIfManaged(pDestSurface);
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDestSurface) override
	{
		return (iSwapChain == 0) ? GetFrontBufferDataInternal(pDestSurface) : m_real->GetFrontBufferData(iSwapChain, ResolveSurfaceForSys(pDestSurface));
	}
	HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect, IDirect3DSurface9 *pDestSurface, CONST RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter) override
	{
		FlushUpBatch();
		m_dirtySinceClear = true;
		return m_real->StretchRect(ResolveSurfaceForGpu(pSourceSurface), pSourceRect,
		                           ResolveSurfaceForGpu(pDestSurface), pDestRect, Filter);
	}
	HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9 *pSurface, CONST RECT *pRect, D3DCOLOR color) override
	{
		FlushUpBatch();
		m_dirtySinceClear = true;
		return m_real->ColorFill(ResolveSurfaceForGpu(pSurface), pRect, color);
	}
	HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) override
	{
		if (Pool == D3DPOOL_MANAGED) Pool = D3DPOOL_SYSTEMMEM;
		return m_real->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
	}
	HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) override
	{
		FlushUpBatch();
		m_dirtySinceClear = true;
		IDirect3DSurface9 *target = ResolveSurfaceForGpu(pRenderTarget);
		if (RenderTargetIndex == 0)
			m_rt0IsBackBuffer = (target != nullptr && target == m_backBufferSurf);
		HRESULT hr = m_real->SetRenderTarget(RenderTargetIndex, target);
		if (SUCCEEDED(hr) && RenderTargetIndex == 0)
			m_vpValid = false; // D3D9 implicitly resets viewport to the new RT0 dimensions
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) override
	{
		if (!ppRenderTarget) return D3DERR_INVALIDCALL;
		IDirect3DSurface9 *raw = nullptr;
		HRESULT hr = m_real->GetRenderTarget(RenderTargetIndex, &raw);
		if (FAILED(hr)) return hr;
		if (RenderTargetIndex == 0 && m_proxyBackBuffer && raw &&
		    (raw == m_backBufferSurf || raw == m_sharedSurf[0] || raw == m_sharedSurf[1])) {
			raw->Release();
			*ppRenderTarget = m_proxyBackBuffer;
			m_proxyBackBuffer->AddRef();
			return S_OK;
		}
		*ppRenderTarget = raw;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) override
	{
		FlushUpBatch();
		m_dirtySinceClear = true;
		return m_real->SetDepthStencilSurface(ResolveSurfaceForGpu(pNewZStencil));
	}
	HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) override
	{
		if (!ppZStencilSurface) return D3DERR_INVALIDCALL;
		IDirect3DSurface9 *raw = nullptr;
		HRESULT hr = m_real->GetDepthStencilSurface(&raw);
		if (FAILED(hr)) return hr;
		if (m_proxyDepthSurf && raw && raw == m_depthSurf) {
			raw->Release();
			*ppZStencilSurface = m_proxyDepthSurf;
			m_proxyDepthSurf->AddRef();
			return S_OK;
		}
		*ppZStencilSurface = raw;
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE BeginScene() override
	{
		if (!m_renderStartedThisFrame) {
			m_renderStartedThisFrame = true;
			// FrontEndMenuManager.m_bMenuActive, sampled on the game thread.
			m_frontEndFrame = IsGtaSaUs10() && *reinterpret_cast<const uint8_t *>(0xBA67A4) != 0;
			if (!m_inputSampledThisFrame)
				MarkInputSampled();
			m_presenter.Reflex().MarkFrame(m_curFrameId, RM_SIMULATION_END);
			m_presenter.Reflex().MarkFrame(m_curFrameId, RM_RENDERSUBMIT_START);
		}
		return m_real->BeginScene();
	}
	HRESULT STDMETHODCALLTYPE EndScene() override
	{
		if (g_d3d9EndSceneEntry && g_origD3D9EndScene && !m_inEndScene) {
			m_inEndScene = true;
			HRESULT hr = g_d3d9EndSceneEntry(this);
			m_inEndScene = false;
			return hr;
		}
		return EndSceneInternal();
	}
	HRESULT STDMETHODCALLTYPE Clear(DWORD Count, CONST D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override
	{
		FlushUpBatch();
		if (CacheActive()) {
			if (Flags == 0) return S_OK;
			if (Count == 0 && pRects == nullptr && !m_dirtySinceClear &&
			    (Flags & m_lastClearFlags) == Flags &&
			    (!(Flags & D3DCLEAR_TARGET)  || m_lastClearColor   == Color) &&
			    (!(Flags & D3DCLEAR_ZBUFFER) || m_lastClearZ       == Z) &&
			    (!(Flags & D3DCLEAR_STENCIL) || m_lastClearStencil == Stencil)) {
				return S_OK;
			}
			HRESULT hr = m_real->Clear(Count, pRects, Flags, Color, Z, Stencil);
			if (SUCCEEDED(hr) && Count == 0 && pRects == nullptr) {
				if (m_dirtySinceClear) {
					m_lastClearFlags = Flags;
				} else {
					m_lastClearFlags |= Flags;
				}
				if (Flags & D3DCLEAR_TARGET)  m_lastClearColor   = Color;
				if (Flags & D3DCLEAR_ZBUFFER) m_lastClearZ       = Z;
				if (Flags & D3DCLEAR_STENCIL) m_lastClearStencil = Stencil;
				m_dirtySinceClear = false;
			} else {
				m_dirtySinceClear = true;
			}
			return hr;
		}
		m_dirtySinceClear = true;
		return m_real->Clear(Count, pRects, Flags, Color, Z, Stencil);
	}
	inline bool CacheActive() const { return g_cfg.stateCache && !m_inStateBlock; }

	HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX *pMatrix) override
	{
		if (pMatrix) {
			if (State == D3DTS_VIEW) {
				m_lastViewMat = *pMatrix;
				m_hasLastView = true;
				if (m_sceneProjActive && m_rt0IsBackBuffer && !m_captured3dThisFrame) {
					float tMag = fabsf(pMatrix->_41) + fabsf(pMatrix->_42) + fabsf(pMatrix->_43);
					if (tMag > 0.25f) {
						m_sceneViewMat = *pMatrix;
						m_hasSceneMats = true;
					}
				}
			} else if (State == D3DTS_PROJECTION) {
				bool isPersp = IsSceneProjection(*pMatrix);
				if (m_rt0IsBackBuffer && !m_captured3dThisFrame) {
					m_sceneProjActive = isPersp;
					if (isPersp) {
						m_sceneProjMat = *pMatrix;
						if (m_hasLastView) {
							float tMag = fabsf(m_lastViewMat._41) + fabsf(m_lastViewMat._42) + fabsf(m_lastViewMat._43);
							if (tMag > 0.25f) {
								m_sceneViewMat = m_lastViewMat;
								m_hasSceneMats = true;
							}
						}
					}
				}
			}
		}
		if (CacheActive() && pMatrix) {
			const int slot = TransformSlot(State);
			if (slot >= 0) {
				if (m_xformValid[slot] && Equal64SSE2(&m_xformVal[slot], pMatrix))
					return S_OK;
				if (State != D3DTS_WORLD || !m_batchLocalXYZ || !TranslationOnly(*pMatrix))
					FlushUpBatch();
				HRESULT hr = m_real->SetTransform(State, pMatrix);
				if (SUCCEEDED(hr)) { m_xformValid[slot] = 1; m_xformVal[slot] = *pMatrix; }
				return hr;
			}
		}
		FlushUpBatch();
		return m_real->SetTransform(State, pMatrix);
	}
	HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) override
	{
		if (CacheActive() && pMatrix) {
			const int slot = TransformSlot(State);
			if (slot >= 0) {
				if (m_xformValid[slot]) { *pMatrix = m_xformVal[slot]; return S_OK; }
				HRESULT hr = m_real->GetTransform(State, pMatrix);
				if (SUCCEEDED(hr)) { m_xformValid[slot] = 1; m_xformVal[slot] = *pMatrix; }
				return hr;
			}
		}
		return m_real->GetTransform(State, pMatrix);
	}
	HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX *pMatrix) override
	{
		FlushUpBatch();
		const int slot = TransformSlot(State);
		if (slot >= 0) m_xformValid[slot] = 0;
		return m_real->MultiplyTransform(State, pMatrix);
	}
	HRESULT STDMETHODCALLTYPE SetViewport(CONST D3DVIEWPORT9 *pViewport) override
	{
		if (CacheActive() && pViewport) {
			if (m_vpValid && memcmp(&m_vpVal, pViewport, sizeof(D3DVIEWPORT9)) == 0) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetViewport(pViewport);
			if (SUCCEEDED(hr)) { m_vpValid = true; m_vpVal = *pViewport; }
			return hr;
		}
		FlushUpBatch();
		return m_real->SetViewport(pViewport);
	}
	HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *pViewport) override
	{
		if (CacheActive() && pViewport && m_vpValid) { *pViewport = m_vpVal; return S_OK; }
		HRESULT hr = m_real->GetViewport(pViewport);
		if (SUCCEEDED(hr) && CacheActive() && pViewport) { m_vpValid = true; m_vpVal = *pViewport; }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE SetMaterial(CONST D3DMATERIAL9 *pMaterial) override
	{
		static_assert(sizeof(D3DMATERIAL9) == 68, "D3DMATERIAL9 size");
		if (CacheActive() && pMaterial) {
			if (m_matValid && Equal68SSE2(&m_matVal, pMaterial)) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetMaterial(pMaterial);
			if (SUCCEEDED(hr)) { m_matValid = true; m_matVal = *pMaterial; }
			return hr;
		}
		FlushUpBatch();
		return m_real->SetMaterial(pMaterial);
	}
	HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9 *pMaterial) override
	{
		if (CacheActive() && pMaterial && m_matValid) { *pMaterial = m_matVal; return S_OK; }
		HRESULT hr = m_real->GetMaterial(pMaterial);
		if (SUCCEEDED(hr) && CacheActive() && pMaterial) { m_matValid = true; m_matVal = *pMaterial; }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE SetLight(DWORD Index, CONST D3DLIGHT9 *pLight) override
	{
		static_assert(sizeof(D3DLIGHT9) == 104, "D3DLIGHT9 size");
		if (CacheActive() && pLight && Index < 16) {
			if (m_lightValid[Index] && Equal104SSE2(&m_lightVal[Index], pLight))
				return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetLight(Index, pLight);
			if (SUCCEEDED(hr)) { m_lightValid[Index] = 1; m_lightVal[Index] = *pLight; }
			return hr;
		}
		FlushUpBatch();
		return m_real->SetLight(Index, pLight);
	}
	HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9 *pLight) override
	{
		if (CacheActive() && pLight && Index < 16 && m_lightValid[Index]) {
			*pLight = m_lightVal[Index];
			return S_OK;
		}
		HRESULT hr = m_real->GetLight(Index, pLight);
		if (SUCCEEDED(hr) && CacheActive() && pLight && Index < 16) {
			m_lightValid[Index] = 1;
			m_lightVal[Index] = *pLight;
		}
		return hr;
	}
	HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, BOOL Enable) override
	{
		BOOL norm = Enable ? TRUE : FALSE;
		if (CacheActive() && Index < 16) {
			if (m_lightEnValid[Index] && m_lightEnVal[Index] == norm) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->LightEnable(Index, Enable);
			if (SUCCEEDED(hr)) { m_lightEnValid[Index] = 1; m_lightEnVal[Index] = norm; }
			return hr;
		}
		FlushUpBatch();
		return m_real->LightEnable(Index, Enable);
	}
	HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, BOOL *pEnable) override
	{
		if (CacheActive() && pEnable && Index < 16 && m_lightEnValid[Index]) { *pEnable = m_lightEnVal[Index]; return S_OK; }
		HRESULT hr = m_real->GetLightEnable(Index, pEnable);
		if (SUCCEEDED(hr) && CacheActive() && pEnable && Index < 16) { m_lightEnValid[Index] = 1; m_lightEnVal[Index] = *pEnable ? TRUE : FALSE; }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index, CONST float *pPlane) override
	{
		if (CacheActive() && pPlane && Index < 6) {
			if (m_clipValid[Index] && Equal16SSE2(m_clipVal[Index], pPlane)) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetClipPlane(Index, pPlane);
			if (SUCCEEDED(hr)) { m_clipValid[Index] = 1; memcpy(m_clipVal[Index], pPlane, 16u); }
			return hr;
		}
		FlushUpBatch();
		return m_real->SetClipPlane(Index, pPlane);
	}
	HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float *pPlane) override
	{
		if (CacheActive() && pPlane && Index < 6 && m_clipValid[Index]) { memcpy(pPlane, m_clipVal[Index], 16u); return S_OK; }
		return m_real->GetClipPlane(Index, pPlane);
	}
	void InvalidateStateCache()
	{
		FlushUpBatch();
		memset(m_rsValid, 0, sizeof(m_rsValid));
		memset(m_ssValid, 0, sizeof(m_ssValid));
		memset(m_tssValid, 0, sizeof(m_tssValid));
		memset(m_texValid, 0, sizeof(m_texValid));
		memset(m_texManaged, 0, sizeof(m_texManaged));
		m_boundManagedMask = 0;
		memset(m_streamValid, 0, sizeof(m_streamValid));
		memset(m_streamFreqValid, 0, sizeof(m_streamFreqValid));
		memset(m_lightEnValid, 0, sizeof(m_lightEnValid));
		memset(m_lightValid, 0, sizeof(m_lightValid));
		memset(m_clipValid, 0, sizeof(m_clipValid));
		memset(m_xformValid, 0, sizeof(m_xformValid));
		memset(m_vsConstFValid, 0, sizeof(m_vsConstFValid));
		memset(m_psConstFValid, 0, sizeof(m_psConstFValid));
		memset(m_vsConstIValid, 0, sizeof(m_vsConstIValid));
		memset(m_psConstIValid, 0, sizeof(m_psConstIValid));
		memset(m_vsConstBValid, 0, sizeof(m_vsConstBValid));
		memset(m_psConstBValid, 0, sizeof(m_psConstBValid));
		m_vpValid = false;
		m_scissorValid = false;
		m_matValid = false;
		m_fvfValid = false;
		m_vdeclValid = false;
		m_vsValid = false;
		m_psValid = false;
		m_ibValid = false;
		m_dirtySinceClear = true;
	}

	HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override
	{
		const UINT s = (UINT)State;
		// Never cache driver trigger states (e.g. RESZ depth resolve, ATOC/NVDB).
		const bool uncacheable = (State == D3DRS_POINTSIZE || State == D3DRS_ADAPTIVETESS_Y || State == D3DRS_ADAPTIVETESS_X);
		if (CacheActive() && !uncacheable && s < 256) {
			if (m_rsValid[s] && m_rsVal[s] == Value) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetRenderState(State, Value);
			if (SUCCEEDED(hr)) { m_rsValid[s] = 1; m_rsVal[s] = Value; }
			return hr;
		}
		FlushUpBatch();
		return m_real->SetRenderState(State, Value);
	}
	HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) override
	{
		const UINT s = (UINT)State;
		if (CacheActive() && pValue && s < 256 && m_rsValid[s]) { *pValue = m_rsVal[s]; return S_OK; }
		HRESULT hr = m_real->GetRenderState(State, pValue);
		const bool uncacheable = (State == D3DRS_POINTSIZE || State == D3DRS_ADAPTIVETESS_Y || State == D3DRS_ADAPTIVETESS_X);
		if (SUCCEEDED(hr) && CacheActive() && !uncacheable && pValue && s < 256) { m_rsValid[s] = 1; m_rsVal[s] = *pValue; }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) override
	{
		if (!ppSB) return D3DERR_INVALIDCALL;
		FlushUpBatch();
		IDirect3DStateBlock9 *realSB = nullptr;
		HRESULT hr = m_real->CreateStateBlock(Type, &realSB);
		if (FAILED(hr) || !realSB) return hr;
		*ppSB = new ProxyStateBlock9(this, realSB);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE BeginStateBlock() override
	{
		FlushUpBatch();
		HRESULT hr = m_real->BeginStateBlock();
		if (SUCCEEDED(hr)) m_inStateBlock = true;
		return hr;
	}
	HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9 **ppSB) override
	{
		m_inStateBlock = false;
		if (!ppSB) return D3DERR_INVALIDCALL;
		IDirect3DStateBlock9 *realSB = nullptr;
		HRESULT hr = m_real->EndStateBlock(&realSB);
		if (FAILED(hr) || !realSB) return hr;
		*ppSB = new ProxyStateBlock9(this, realSB);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE SetClipStatus(CONST D3DCLIPSTATUS9 *pClipStatus) override { FlushUpBatch(); return m_real->SetClipStatus(pClipStatus); }
	HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9 *pClipStatus) override { return m_real->GetClipStatus(pClipStatus); }
	HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) override { return m_real->GetTexture(Stage, ppTexture); }
	HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) override
	{
		bool isManaged = false;
		IDirect3DBaseTexture9 *gpu = UnwrapBaseTextureForGpu(pTexture, &isManaged);
		if (!m_inStateBlock && Stage < 8) {
			if (isManaged) {
				m_texManaged[Stage] = pTexture;
				m_boundManagedMask |= (uint8_t)(1u << Stage);
			} else {
				m_texManaged[Stage] = nullptr;
				m_boundManagedMask &= (uint8_t)~(1u << Stage);
			}
			if (g_cfg.stateCache && m_texValid[Stage] && m_texGpu[Stage] == gpu)
				return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetTexture(Stage, gpu);
			if (SUCCEEDED(hr) && g_cfg.stateCache) {
				m_texValid[Stage] = 1;
				m_texGpu[Stage] = gpu;
			}
			return hr;
		}
		FlushUpBatch();
		return m_real->SetTexture(Stage, gpu);
	}
	HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) override
	{
		const UINT t = (UINT)Type;
		if (CacheActive() && pValue && Stage < 8 && t < 36 && m_tssValid[Stage][t]) { *pValue = m_tssVal[Stage][t]; return S_OK; }
		HRESULT hr = m_real->GetTextureStageState(Stage, Type, pValue);
		if (SUCCEEDED(hr) && CacheActive() && pValue && Stage < 8 && t < 36) { m_tssValid[Stage][t] = 1; m_tssVal[Stage][t] = *pValue; }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override
	{
		const UINT t = (UINT)Type;
		if (CacheActive() && Stage < 8 && t < 36) {
			if (m_tssValid[Stage][t] && m_tssVal[Stage][t] == Value) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetTextureStageState(Stage, Type, Value);
			if (SUCCEEDED(hr)) { m_tssValid[Stage][t] = 1; m_tssVal[Stage][t] = Value; }
			return hr;
		}
		FlushUpBatch();
		return m_real->SetTextureStageState(Stage, Type, Value);
	}
	HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) override
	{
		const UINT t = (UINT)Type;
		if (CacheActive() && pValue && Sampler < 8 && t < 16 && m_ssValid[Sampler][t]) { *pValue = m_ssVal[Sampler][t]; return S_OK; }
		HRESULT hr = m_real->GetSamplerState(Sampler, Type, pValue);
		if (SUCCEEDED(hr) && CacheActive() && pValue && Sampler < 8 && t < 16) { m_ssValid[Sampler][t] = 1; m_ssVal[Sampler][t] = *pValue; }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override
	{
		const UINT t = (UINT)Type;
		const int af = g_cfg.anisotropy;
		if (af >= 2 && Sampler < 8 && !m_inStateBlock && !m_frontEndFrame) {
			if (Type == D3DSAMP_MINFILTER && Value == D3DTEXF_LINEAR) {
				Value = D3DTEXF_ANISOTROPIC;
				const DWORD wantAf = (DWORD)af;
				if (!m_ssValid[Sampler][D3DSAMP_MAXANISOTROPY] || m_ssVal[Sampler][D3DSAMP_MAXANISOTROPY] != wantAf) {
					FlushUpBatch();
					if (SUCCEEDED(m_real->SetSamplerState(Sampler, D3DSAMP_MAXANISOTROPY, wantAf))) {
						m_ssValid[Sampler][D3DSAMP_MAXANISOTROPY] = 1;
						m_ssVal[Sampler][D3DSAMP_MAXANISOTROPY] = wantAf;
					}
				}
				// Ensure 3D world textures (including newly auto-mipmapped railway/road TXDs!) always use Trilinear mip filtering
				if (!m_captured3dThisFrame && (!m_ssValid[Sampler][D3DSAMP_MIPFILTER] || m_ssVal[Sampler][D3DSAMP_MIPFILTER] != D3DTEXF_LINEAR)) {
					FlushUpBatch();
					if (SUCCEEDED(m_real->SetSamplerState(Sampler, D3DSAMP_MIPFILTER, D3DTEXF_LINEAR))) {
						m_ssValid[Sampler][D3DSAMP_MIPFILTER] = 1;
						m_ssVal[Sampler][D3DSAMP_MIPFILTER] = D3DTEXF_LINEAR;
					}
				}
			} else if (Type == D3DSAMP_MIPFILTER && (Value == D3DTEXF_POINT || (!m_captured3dThisFrame && Value == D3DTEXF_NONE))) {
				// Upgrade mip-none/point -> smooth Trilinear AF during 3D world rendering (eliminates rail/road texture shimmer!)
				Value = D3DTEXF_LINEAR;
			} else if (Type == D3DSAMP_MAXANISOTROPY && Value < (DWORD)af) {
				Value = (DWORD)af;
			} else if (Type == D3DSAMP_MIPMAPLODBIAS) {
				// Clamp negative LOD bias (< 0) that causes shimmering texture crawl on roads/fences/roofs
				float bias = 0.0f;
				memcpy(&bias, &Value, sizeof(float));
				if (bias < 0.0f) {
					bias = 0.0f;
					memcpy(&Value, &bias, sizeof(DWORD));
				}
			}
		}
		if (CacheActive() && Sampler < 8 && t < 16) {
			if (m_ssValid[Sampler][t] && m_ssVal[Sampler][t] == Value) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetSamplerState(Sampler, Type, Value);
			if (SUCCEEDED(hr)) { m_ssValid[Sampler][t] = 1; m_ssVal[Sampler][t] = Value; }
			return hr;
		}
		FlushUpBatch();
		return m_real->SetSamplerState(Sampler, Type, Value);
	}
	HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *pNumPasses) override { return m_real->ValidateDevice(pNumPasses); }
	HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY *pEntries) override { return m_real->SetPaletteEntries(PaletteNumber, pEntries); }
	HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) override { return m_real->GetPaletteEntries(PaletteNumber, pEntries); }
	HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override { FlushUpBatch(); return m_real->SetCurrentTexturePalette(PaletteNumber); }
	HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber) override { return m_real->GetCurrentTexturePalette(PaletteNumber); }
	HRESULT STDMETHODCALLTYPE SetScissorRect(CONST RECT *pRect) override
	{
		if (CacheActive() && pRect) {
			if (m_scissorValid && memcmp(&m_scissorVal, pRect, sizeof(RECT)) == 0) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetScissorRect(pRect);
			if (SUCCEEDED(hr)) { m_scissorValid = true; m_scissorVal = *pRect; }
			return hr;
		}
		FlushUpBatch();
		return m_real->SetScissorRect(pRect);
	}
	HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *pRect) override
	{
		if (CacheActive() && pRect && m_scissorValid) { *pRect = m_scissorVal; return S_OK; }
		HRESULT hr = m_real->GetScissorRect(pRect);
		if (SUCCEEDED(hr) && CacheActive() && pRect) { m_scissorValid = true; m_scissorVal = *pRect; }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL bSoftware) override { FlushUpBatch(); return m_real->SetSoftwareVertexProcessing(bSoftware); }
	BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return m_real->GetSoftwareVertexProcessing(); }
	HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments) override { FlushUpBatch(); return m_real->SetNPatchMode(nSegments); }
	float   STDMETHODCALLTYPE GetNPatchMode() override { return m_real->GetNPatchMode(); }
	HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override
	{
		if (g_cfg.stateCache && PrimitiveCount == 0) return S_OK;
		FlushUpBatch();
		m_dirtySinceClear = true;
		FlushBoundManagedTextures();
		if (m_replayPlayerDraw && m_playerMirror.Bind(m_real)) {
			HRESULT replay = m_real->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
			m_playerMirror.Restore(m_real, replay);
		}
		return m_real->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount);
	}
	HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE Type, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) override
	{
		if (g_cfg.stateCache && primCount == 0) return S_OK;
		// A stationary camera may keep the device transforms across frames without a SetTransform call.
		if (!m_hasSceneMats && !m_checkedDrawTransforms && !m_captured3dThisFrame &&
		    m_rt0IsBackBuffer && WantRoadPuddles()) {
			m_checkedDrawTransforms = true;
			D3DMATRIX view, projection;
			if (SUCCEEDED(m_real->GetTransform(D3DTS_PROJECTION, &projection)) && IsSceneProjection(projection) &&
			    SUCCEEDED(m_real->GetTransform(D3DTS_VIEW, &view)) &&
			    fabsf(view._41) + fabsf(view._42) + fabsf(view._43) > 0.25f) {
				m_sceneViewMat = view;
				m_sceneProjMat = projection;
				m_hasSceneMats = true;
			}
		}
		m_dirtySinceClear = true;
		if (!m_replayPlayerDraw && m_hasCpuMeshVertices && m_hasCpuMeshIndices && !(m_boundManagedMask && HasDirtyManagedResources()) &&
		    TryBatchBufferedMesh(Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount))
			return S_OK;
		FlushUpBatch();
		FlushBoundManagedTextures();
		if (m_replayPlayerDraw && m_playerMirror.Bind(m_real)) {
			HRESULT replay = m_real->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
			m_playerMirror.Restore(m_real, replay);
		}
		return m_real->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount);
	}
	HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override
	{
		if (g_cfg.stateCache && PrimitiveCount == 0) return S_OK;
		if (m_batchBuffered) FlushUpBatch();
		m_dirtySinceClear = true;
		if (m_boundManagedMask && HasDirtyManagedResources()) {
			FlushUpBatch();
			FlushBoundManagedTextures();
		}
		// State-cache-aware TRIANGLELIST coalescing: merges consecutive identical-state UP draws
		// (SA-MP chat glyphs, nametags, 3D texts, ImGui quads) into a single GPU draw call!
		// Uses raw fixed-capacity staging buffer (0 vector zero-fill, fits inside L1/driver 64KB UP fast-path).
		if (!m_replayPlayerDraw && CacheActive() && PrimitiveType == D3DPT_TRIANGLELIST && pVertexStreamZeroData && VertexStreamZeroStride > 0 && PrimitiveCount <= 512u) {
			const UINT vCount = PrimitiveCount * 3u;
			if (vCount && VertexStreamZeroStride <= kUpBatchMaxBytes / vCount) {
				const UINT vBytes = vCount * VertexStreamZeroStride;
				if (m_batchPrimCount > 0 && (m_batchIndexed || m_batchStride != VertexStreamZeroStride || m_batchVertBytes + vBytes > kUpBatchMaxBytes))
					FlushUpBatch();
				if (m_batchPrimCount == 0) {
					m_batchIndexed   = false;
					m_batchStride    = VertexStreamZeroStride;
					m_batchVertBytes = 0;
				}
				AppendUpVertices(pVertexStreamZeroData, vCount, VertexStreamZeroStride);
				m_batchVertBytes += vBytes;
				m_batchPrimCount += PrimitiveCount;
				// D3D9 spec: DrawPrimitiveUP unbinds Stream 0 (so any subsequent SetStreamSource(0) flushes first!)
				m_streamValid[0] = 0;
				return S_OK;
			}
		}
		FlushUpBatch();
		m_streamValid[0] = 0;
		if (m_replayPlayerDraw && m_playerMirror.Bind(m_real)) {
			m_playerMirror.Restore(m_real, m_real->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride));
		}
		return m_real->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride);
	}
	HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, CONST void *pIndexData, D3DFORMAT IndexDataFormat, CONST void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override
	{
		if (g_cfg.stateCache && PrimitiveCount == 0) return S_OK;
		if (m_batchBuffered) FlushUpBatch();
		m_dirtySinceClear = true;
		if (m_boundManagedMask && HasDirtyManagedResources()) {
			FlushUpBatch();
			FlushBoundManagedTextures();
		}
		if (!m_replayPlayerDraw && CacheActive() && PrimitiveType == D3DPT_TRIANGLELIST && IndexDataFormat == D3DFMT_INDEX16 &&
		    pVertexStreamZeroData && pIndexData && VertexStreamZeroStride > 0 && MinVertexIndex == 0 && NumVertices > 0 && PrimitiveCount <= 512u) {
			const UINT idxCount = PrimitiveCount * 3u;
			if (VertexStreamZeroStride <= kUpBatchMaxBytes / NumVertices && idxCount <= kUpBatchMaxIndices) {
				const UINT vBytes = NumVertices * VertexStreamZeroStride;
				if (m_batchPrimCount > 0 && (!m_batchIndexed || m_batchStride != VertexStreamZeroStride ||
				    m_batchVertBytes + vBytes > kUpBatchMaxBytes || m_batchVertCount + NumVertices > 65535u ||
				    m_batchIdxCount + idxCount > kUpBatchMaxIndices))
					FlushUpBatch();
				if (m_batchPrimCount == 0) {
					m_batchIndexed   = true;
					m_batchStride    = VertexStreamZeroStride;
					m_batchVertBytes = 0;
					m_batchVertCount = 0;
					m_batchIdxCount  = 0;
				}
				const uint16_t baseV = (uint16_t)m_batchVertCount;
				AppendUpVertices(pVertexStreamZeroData, NumVertices, VertexStreamZeroStride);
				m_batchVertBytes += vBytes;
				const auto *srcIdx = (const uint16_t *)pIndexData;
				uint16_t *dstIdx = m_batchIdxBuf + m_batchIdxCount;
				if (baseV == 0) {
					memcpy(dstIdx, srcIdx, (size_t)idxCount * 2u);
				} else {
					for (UINT k = 0; k < idxCount; ++k)
						dstIdx[k] = (uint16_t)(srcIdx[k] + baseV);
				}
				m_batchIdxCount  += idxCount;
				m_batchVertCount += NumVertices;
				m_batchPrimCount += PrimitiveCount;
				// D3D9 spec: DrawIndexedPrimitiveUP unbinds Stream 0 and Indices
				m_streamValid[0] = 0;
				m_ibValid = false;
				return S_OK;
			}
		}
		FlushUpBatch();
		m_streamValid[0] = 0;
		m_ibValid = false;
		if (m_replayPlayerDraw && m_playerMirror.Bind(m_real)) {
			m_playerMirror.Restore(m_real, m_real->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride));
		}
		return m_real->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride);
	}
	HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9 *pDestBuffer, IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags) override { FlushUpBatch(); return m_real->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags); }
	HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(CONST D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) override { m_vdeclValid = false; return m_real->CreateVertexDeclaration(pVertexElements, ppDecl); }
	HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) override
	{
		if (CacheActive()) {
			if (m_vdeclValid && m_vdecl == pDecl) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetVertexDeclaration(pDecl);
			if (SUCCEEDED(hr)) { m_vdeclValid = true; m_vdecl = pDecl; m_fvfValid = false; }
			return hr;
		}
		FlushUpBatch();
		return m_real->SetVertexDeclaration(pDecl);
	}
	HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) override { return m_real->GetVertexDeclaration(ppDecl); }
	HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) override
	{
		if (CacheActive()) {
			if (m_fvfValid && m_fvfVal == FVF) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetFVF(FVF);
			if (SUCCEEDED(hr)) { m_fvfValid = true; m_fvfVal = FVF; m_vdeclValid = false; }
			return hr;
		}
		FlushUpBatch();
		return m_real->SetFVF(FVF);
	}
	HRESULT STDMETHODCALLTYPE GetFVF(DWORD *pFVF) override
	{
		if (CacheActive() && pFVF && m_fvfValid) { *pFVF = m_fvfVal; return S_OK; }
		HRESULT hr = m_real->GetFVF(pFVF);
		if (SUCCEEDED(hr) && CacheActive() && pFVF) { m_fvfValid = true; m_fvfVal = *pFVF; }
		return hr;
	}
	HRESULT STDMETHODCALLTYPE CreateVertexShader(CONST DWORD *pFunction, IDirect3DVertexShader9 **ppShader) override { m_vsValid = false; return m_real->CreateVertexShader(pFunction, ppShader); }
	HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9 *pShader) override
	{
		if (CacheActive()) {
			if (m_vsValid && m_vs == pShader) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetVertexShader(pShader);
			if (SUCCEEDED(hr)) { m_vsValid = true; m_vs = pShader; }
			return hr;
		}
		FlushUpBatch();
		return m_real->SetVertexShader(pShader);
	}
	HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9 **ppShader) override { return m_real->GetVertexShader(ppShader); }
	HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT StartRegister, CONST float *pConstantData, UINT Vector4fCount) override
	{
		if (CacheActive() && pConstantData && Vector4fCount > 0 && StartRegister < 256 && Vector4fCount <= 256 - StartRegister) {
			UINT first = 0;
			while (first < Vector4fCount && m_vsConstFValid[StartRegister + first] &&
			       Equal16SSE2(m_vsConstF[StartRegister + first], pConstantData + first * 4)) {
				++first;
			}
			if (first == Vector4fCount) return S_OK;
			UINT last = Vector4fCount;
			while (last > first && m_vsConstFValid[StartRegister + last - 1] &&
			       Equal16SSE2(m_vsConstF[StartRegister + last - 1], pConstantData + (last - 1) * 4)) {
				--last;
			}
			const UINT trimmedCount = last - first;
			FlushUpBatch();
			HRESULT hr = m_real->SetVertexShaderConstantF(StartRegister + first, pConstantData + first * 4, trimmedCount);
			if (SUCCEEDED(hr)) {
				memcpy(m_vsConstF[StartRegister + first], pConstantData + first * 4, (size_t)trimmedCount * 16u);
				memset(m_vsConstFValid + StartRegister + first, 1, trimmedCount);
			}
			return hr;
		}
		FlushUpBatch();
		return m_real->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}
	HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) override
	{
		if (CacheActive() && pConstantData && Vector4fCount > 0 && StartRegister < 256 && Vector4fCount <= 256 - StartRegister) {
			bool allValid = true;
			for (UINT i = 0; i < Vector4fCount; ++i) {
				if (!m_vsConstFValid[StartRegister + i]) { allValid = false; break; }
			}
			if (allValid) {
				memcpy(pConstantData, m_vsConstF[StartRegister], (size_t)Vector4fCount * 16u);
				return S_OK;
			}
			HRESULT hr = m_real->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
			if (SUCCEEDED(hr)) {
				memcpy(m_vsConstF[StartRegister], pConstantData, (size_t)Vector4fCount * 16u);
				memset(m_vsConstFValid + StartRegister, 1, Vector4fCount);
			}
			return hr;
		}
		return m_real->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}
	HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT StartRegister, CONST int *pConstantData, UINT Vector4iCount) override
	{
		if (CacheActive() && pConstantData && Vector4iCount > 0 && StartRegister < 16 && Vector4iCount <= 16 - StartRegister) {
			UINT first = 0;
			while (first < Vector4iCount && m_vsConstIValid[StartRegister + first] &&
			       Equal16SSE2(m_vsConstI[StartRegister + first], pConstantData + first * 4)) {
				++first;
			}
			if (first == Vector4iCount) return S_OK;
			UINT last = Vector4iCount;
			while (last > first && m_vsConstIValid[StartRegister + last - 1] &&
			       Equal16SSE2(m_vsConstI[StartRegister + last - 1], pConstantData + (last - 1) * 4)) {
				--last;
			}
			const UINT trimmedCount = last - first;
			FlushUpBatch();
			HRESULT hr = m_real->SetVertexShaderConstantI(StartRegister + first, pConstantData + first * 4, trimmedCount);
			if (SUCCEEDED(hr)) {
				memcpy(m_vsConstI[StartRegister + first], pConstantData + first * 4, (size_t)trimmedCount * 16u);
				memset(m_vsConstIValid + StartRegister + first, 1, trimmedCount);
			}
			return hr;
		}
		FlushUpBatch();
		return m_real->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}
	HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) override { return m_real->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
	HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT StartRegister, CONST BOOL *pConstantData, UINT BoolCount) override
	{
		if (CacheActive() && pConstantData && BoolCount > 0 && StartRegister < 16 && BoolCount <= 16 - StartRegister) {
			bool same = true;
			for (UINT i = 0; i < BoolCount; ++i) {
				BOOL norm = pConstantData[i] ? TRUE : FALSE;
				if (!m_vsConstBValid[StartRegister + i] || m_vsConstB[StartRegister + i] != norm) { same = false; break; }
			}
			if (same) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
			if (SUCCEEDED(hr)) {
				for (UINT i = 0; i < BoolCount; ++i) {
					m_vsConstBValid[StartRegister + i] = 1;
					m_vsConstB[StartRegister + i] = pConstantData[i] ? TRUE : FALSE;
				}
			}
			return hr;
		}
		FlushUpBatch();
		return m_real->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount);
	}
	HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) override { return m_real->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
	HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride) override
	{
		if (CacheActive() && StreamNumber < 4) {
			if (m_streamValid[StreamNumber] && m_streamVb[StreamNumber] == pStreamData &&
			    m_streamOff[StreamNumber] == OffsetInBytes && m_streamStride[StreamNumber] == Stride)
				return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride);
			if (SUCCEEDED(hr)) {
				m_streamValid[StreamNumber] = 1;
				m_streamVb[StreamNumber] = pStreamData;
				m_streamOff[StreamNumber] = OffsetInBytes;
				m_streamStride[StreamNumber] = Stride;
			}
			return hr;
		}
		FlushUpBatch();
		return m_real->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride);
	}
	HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes, UINT *pStride) override
	{
		if (StreamNumber == 0) FlushUpBatch();
		return m_real->GetStreamSource(StreamNumber, ppStreamData, pOffsetInBytes, pStride);
	}
	HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber, UINT Setting) override
	{
		if (CacheActive() && StreamNumber < 4) {
			if (m_streamFreqValid[StreamNumber] && m_streamFreqVal[StreamNumber] == Setting) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetStreamSourceFreq(StreamNumber, Setting);
			if (SUCCEEDED(hr)) { m_streamFreqValid[StreamNumber] = 1; m_streamFreqVal[StreamNumber] = Setting; }
			return hr;
		}
		FlushUpBatch();
		return m_real->SetStreamSourceFreq(StreamNumber, Setting);
	}
	HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber, UINT *pSetting) override
	{
		if (CacheActive() && pSetting && StreamNumber < 4 && m_streamFreqValid[StreamNumber]) { *pSetting = m_streamFreqVal[StreamNumber]; return S_OK; }
		return m_real->GetStreamSourceFreq(StreamNumber, pSetting);
	}
	HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9 *pIndexData) override
	{
		if (CacheActive()) {
			if (m_ibValid && m_ib == pIndexData) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetIndices(pIndexData);
			if (SUCCEEDED(hr)) { m_ibValid = true; m_ib = pIndexData; }
			return hr;
		}
		FlushUpBatch();
		return m_real->SetIndices(pIndexData);
	}
	HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9 **ppIndexData) override
	{
		FlushUpBatch();
		return m_real->GetIndices(ppIndexData);
	}
	HRESULT STDMETHODCALLTYPE CreatePixelShader(CONST DWORD *pFunction, IDirect3DPixelShader9 **ppShader) override { m_psValid = false; return m_real->CreatePixelShader(pFunction, ppShader); }
	HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9 *pShader) override
	{
		if (CacheActive()) {
			if (m_psValid && m_ps == pShader) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetPixelShader(pShader);
			if (SUCCEEDED(hr)) { m_psValid = true; m_ps = pShader; }
			return hr;
		}
		FlushUpBatch();
		return m_real->SetPixelShader(pShader);
	}
	HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9 **ppShader) override { return m_real->GetPixelShader(ppShader); }
	HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT StartRegister, CONST float *pConstantData, UINT Vector4fCount) override
	{
		if (CacheActive() && pConstantData && Vector4fCount > 0 && StartRegister < 32 && Vector4fCount <= 32 - StartRegister) {
			UINT first = 0;
			while (first < Vector4fCount && m_psConstFValid[StartRegister + first] &&
			       Equal16SSE2(m_psConstF[StartRegister + first], pConstantData + first * 4)) {
				++first;
			}
			if (first == Vector4fCount) return S_OK;
			UINT last = Vector4fCount;
			while (last > first && m_psConstFValid[StartRegister + last - 1] &&
			       Equal16SSE2(m_psConstF[StartRegister + last - 1], pConstantData + (last - 1) * 4)) {
				--last;
			}
			const UINT trimmedCount = last - first;
			FlushUpBatch();
			HRESULT hr = m_real->SetPixelShaderConstantF(StartRegister + first, pConstantData + first * 4, trimmedCount);
			if (SUCCEEDED(hr)) {
				memcpy(m_psConstF[StartRegister + first], pConstantData + first * 4, (size_t)trimmedCount * 16u);
				memset(m_psConstFValid + StartRegister + first, 1, trimmedCount);
			}
			return hr;
		}
		FlushUpBatch();
		return m_real->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}
	HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) override
	{
		if (CacheActive() && pConstantData && Vector4fCount > 0 && StartRegister < 32 && Vector4fCount <= 32 - StartRegister) {
			bool allValid = true;
			for (UINT i = 0; i < Vector4fCount; ++i) {
				if (!m_psConstFValid[StartRegister + i]) { allValid = false; break; }
			}
			if (allValid) {
				memcpy(pConstantData, m_psConstF[StartRegister], (size_t)Vector4fCount * 16u);
				return S_OK;
			}
			HRESULT hr = m_real->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
			if (SUCCEEDED(hr)) {
				memcpy(m_psConstF[StartRegister], pConstantData, (size_t)Vector4fCount * 16u);
				memset(m_psConstFValid + StartRegister, 1, Vector4fCount);
			}
			return hr;
		}
		return m_real->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount);
	}
	HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT StartRegister, CONST int *pConstantData, UINT Vector4iCount) override
	{
		if (CacheActive() && pConstantData && Vector4iCount > 0 && StartRegister < 16 && Vector4iCount <= 16 - StartRegister) {
			UINT first = 0;
			while (first < Vector4iCount && m_psConstIValid[StartRegister + first] &&
			       Equal16SSE2(m_psConstI[StartRegister + first], pConstantData + first * 4)) {
				++first;
			}
			if (first == Vector4iCount) return S_OK;
			UINT last = Vector4iCount;
			while (last > first && m_psConstIValid[StartRegister + last - 1] &&
			       Equal16SSE2(m_psConstI[StartRegister + last - 1], pConstantData + (last - 1) * 4)) {
				--last;
			}
			const UINT trimmedCount = last - first;
			FlushUpBatch();
			HRESULT hr = m_real->SetPixelShaderConstantI(StartRegister + first, pConstantData + first * 4, trimmedCount);
			if (SUCCEEDED(hr)) {
				memcpy(m_psConstI[StartRegister + first], pConstantData + first * 4, (size_t)trimmedCount * 16u);
				memset(m_psConstIValid + StartRegister + first, 1, trimmedCount);
			}
			return hr;
		}
		FlushUpBatch();
		return m_real->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount);
	}
	HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) override { return m_real->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
	HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT StartRegister, CONST BOOL *pConstantData, UINT BoolCount) override
	{
		if (CacheActive() && pConstantData && BoolCount > 0 && StartRegister < 16 && BoolCount <= 16 - StartRegister) {
			bool same = true;
			for (UINT i = 0; i < BoolCount; ++i) {
				BOOL norm = pConstantData[i] ? TRUE : FALSE;
				if (!m_psConstBValid[StartRegister + i] || m_psConstB[StartRegister + i] != norm) { same = false; break; }
			}
			if (same) return S_OK;
			FlushUpBatch();
			HRESULT hr = m_real->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
			if (SUCCEEDED(hr)) {
				for (UINT i = 0; i < BoolCount; ++i) {
					m_psConstBValid[StartRegister + i] = 1;
					m_psConstB[StartRegister + i] = pConstantData[i] ? TRUE : FALSE;
				}
			}
			return hr;
		}
		FlushUpBatch();
		return m_real->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount);
	}
	HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) override { return m_real->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
	HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT Handle, CONST float *pNumSegs, CONST D3DRECTPATCH_INFO *pRectPatchInfo) override { FlushUpBatch(); return m_real->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo); }
	HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT Handle, CONST float *pNumSegs, CONST D3DTRIPATCH_INFO *pTriPatchInfo) override { FlushUpBatch(); return m_real->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo); }
	HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override { return m_real->DeletePatch(Handle); }
	HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) override
	{
		if (ppQuery) FlushUpBatch();
		HRESULT hr = m_real->CreateQuery(Type, ppQuery);
		if (ppQuery && SUCCEEDED(hr)) m_appUsesQueries = true;
		return hr;
	}

	inline void FlushUpBatch()
	{
		if (m_batchPrimCount != 0)
			FlushUpBatchSlow();
	}

private:

	static bool TranslationOnly(const D3DMATRIX &m)
	{
		return m._11 == 1 && m._22 == 1 && m._33 == 1 && m._44 == 1 &&
		       m._12 == 0 && m._13 == 0 && m._14 == 0 && m._21 == 0 && m._23 == 0 &&
		       m._24 == 0 && m._31 == 0 && m._32 == 0 && m._34 == 0;
	}

	bool CanBatchPlacements(UINT stride) const
	{
		const int world = TransformSlot(D3DTS_WORLD);
		return !m_appUsesQueries && !m_frontEndFrame && !m_captured3dThisFrame && m_rt0IsBackBuffer &&
		       m_fvfValid && (m_fvfVal & D3DFVF_POSITION_MASK) == D3DFVF_XYZ && stride >= 3 * sizeof(float) &&
		       m_vsValid && !m_vs && m_rsValid[D3DRS_ALPHABLENDENABLE] && !m_rsVal[D3DRS_ALPHABLENDENABLE] &&
		       m_rsValid[D3DRS_VERTEXBLEND] && m_rsVal[D3DRS_VERTEXBLEND] == D3DVBF_DISABLE &&
		       m_xformValid[world] && TranslationOnly(m_xformVal[world]);
	}

	bool TryBatchBufferedMesh(D3DPRIMITIVETYPE type, INT base, UINT minVertex, UINT vertices, UINT start, UINT prims)
	{
		const bool strip = type == D3DPT_TRIANGLESTRIP;
		if (!CacheActive() || (!strip && type != D3DPT_TRIANGLELIST) || !prims || prims > 512 || !vertices ||
		    !m_streamValid[0] || !m_streamVb[0] || !m_ibValid || !m_ib ||
		    !CanBatchPlacements(m_streamStride[0])) return false;
		if (strip && (!m_rsValid[D3DRS_SHADEMODE] || m_rsVal[D3DRS_SHADEMODE] != D3DSHADE_GOURAUD)) return false;
		const UINT stride = m_streamStride[0];
		if (vertices > kUpBatchMaxBytes / stride) return false;
		if (m_streamFreqValid[0] && m_streamFreqVal[0] != 1) return false;
		UINT frequency = 0;
		if (!m_streamFreqValid[0] && (FAILED(m_real->GetStreamSourceFreq(0, &frequency)) || frequency != 1)) return false;
		D3DVERTEXBUFFER_DESC vb{};
		D3DINDEXBUFFER_DESC ib{};
		if (FAILED(m_streamVb[0]->GetDesc(&vb)) || FAILED(m_ib->GetDesc(&ib)) ||
		    vb.Pool != D3DPOOL_SYSTEMMEM || ib.Pool != D3DPOOL_SYSTEMMEM || ib.Format != D3DFMT_INDEX16 ||
		    ((vb.Usage | ib.Usage) & D3DUSAGE_WRITEONLY)) return false;
		const int64_t first = int64_t(base) + minVertex;
		if (first < 0) return false;
		const uint64_t byteStart = m_streamOff[0] + uint64_t(first) * stride;
		const UINT bytes = vertices * stride, indices = prims * 3;
		const UINT sourceCount = strip ? prims + 2 : indices;
		if (byteStart + bytes > vb.Size || (uint64_t(start) + sourceCount) * 2 > ib.Size) return false;
		if (m_batchPrimCount && (!m_batchBuffered || m_batchVertBytes + bytes > kUpBatchMaxBytes ||
		    m_batchIdxCount + indices > kUpBatchMaxIndices)) FlushUpBatch();
		void *v = nullptr, *idx = nullptr;
		if (FAILED(m_streamVb[0]->Lock(UINT(byteStart), bytes, &v, D3DLOCK_READONLY))) return false;
		if (FAILED(m_ib->Lock(start * 2, sourceCount * 2, &idx, D3DLOCK_READONLY))) {
			m_streamVb[0]->Unlock();
			return false;
		}
		const auto *source = static_cast<const uint16_t *>(idx);
		bool valid = true;
		for (UINT i = 0; i < indices; ++i) {
			UINT read = i;
			if (strip) {
				const UINT face = i / 3, corner = i % 3;
				read = face + ((face & 1) && corner < 2 ? 1 - corner : corner);
			}
			const UINT relative = UINT(source[read]) - minVertex;
			if (relative >= vertices) { valid = false; break; }
			m_batchIdxBuf[m_batchIdxCount + i] = uint16_t(m_batchVertCount + relative);
		}
		if (valid) {
			AppendUpVertices(v, vertices, stride);
			m_batchIndexed = m_batchBuffered = true;
			m_batchStride = stride;
			m_batchVertBytes += bytes;
			m_batchVertCount += vertices;
			m_batchIdxCount += indices;
			m_batchPrimCount += prims;
		}
		m_ib->Unlock();
		m_streamVb[0]->Unlock();
		return valid;
	}

	void AppendUpVertices(const void *vertices, UINT count, UINT stride)
	{
		uint8_t *dst = m_batchVertBuf + m_batchVertBytes;
		memcpy(dst, vertices, size_t(count) * stride);
		const int world = TransformSlot(D3DTS_WORLD);
		const D3DMATRIX &matrix = m_xformVal[world];
		if (!m_batchPrimCount) {
			m_batchLocalXYZ = CanBatchPlacements(stride);
			if (m_batchLocalXYZ) m_batchWorld = matrix;
		}
		if (!m_batchLocalXYZ) return;
		const float dx = matrix._41 - m_batchWorld._41, dy = matrix._42 - m_batchWorld._42, dz = matrix._43 - m_batchWorld._43;
		if (!dx && !dy && !dz) return;
		for (UINT i = 0; i < count; ++i, dst += stride) {
			float xyz[3];
			memcpy(xyz, dst, sizeof(xyz));
			xyz[0] += dx; xyz[1] += dy; xyz[2] += dz;
			memcpy(dst, xyz, sizeof(xyz));
		}
	}

	void FlushUpBatchSlow()
	{
		const UINT prims = m_batchPrimCount;
		if (prims == 0 || !m_real) return;
		const auto &world = m_xformVal[TransformSlot(D3DTS_WORLD)];
		const bool restore = m_batchLocalXYZ && !Equal64SSE2(&m_batchWorld, &world);
		if (restore) m_real->SetTransform(D3DTS_WORLD, &m_batchWorld);
		m_batchLocalXYZ = false;
		if (m_batchBuffered) {
			m_streamVb[0]->AddRef();
			m_ib->AddRef();
		}
		m_batchPrimCount = 0;
		m_batchVertBytes = 0;
		if (m_batchIndexed) {
			const UINT vCnt = m_batchVertCount;
			m_batchVertCount = 0;
			m_batchIdxCount  = 0;
			m_streamValid[0] = 0;
			m_ibValid = false;
			m_real->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, vCnt, prims,
			                               m_batchIdxBuf, D3DFMT_INDEX16,
			                               m_batchVertBuf, m_batchStride);
		} else {
			m_streamValid[0] = 0;
			m_real->DrawPrimitiveUP(D3DPT_TRIANGLELIST, prims, m_batchVertBuf, m_batchStride);
		}
		if (restore) m_real->SetTransform(D3DTS_WORLD, &world);
		if (m_batchBuffered) {
			m_streamValid[0] = SUCCEEDED(m_real->SetStreamSource(0, m_streamVb[0], m_streamOff[0], m_streamStride[0]));
			m_ibValid = SUCCEEDED(m_real->SetIndices(m_ib));
			m_streamVb[0]->Release();
			m_ib->Release();
			m_batchBuffered = false;
		}
	}

	void ReleaseGameTargets()
	{
		FlushUpBatch();
		m_presenter.UnbindSharedTextures();
		m_playerMirror.Release();
		m_actorCaptured = false;
		ReleaseManagedMipScratch();
		if (m_real) {
			if ((!m_realUsesSwapchain || m_directFallbackSurf) && m_backBufferSurf) {
				IDirect3DSurface9 *bb = nullptr;
				if (SUCCEEDED(m_real->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
					m_real->SetRenderTarget(0, bb);
					bb->Release();
				}
			}
			if (m_depthSurf)
				m_real->SetDepthStencilSurface(nullptr);
		}
		SafeRelease(m_sysReadbackSurf);
		m_sysReadbackW = m_sysReadbackH = 0;
		m_sysReadbackFmt = D3DFMT_UNKNOWN;
		SafeRelease(m_depthSurf);
		SafeRelease(m_intzDepthTex);
		SafeRelease(m_backBufferSurf);
		SafeRelease(m_directFallbackSurf);
		m_rt0IsBackBuffer = true;
		m_has3dMaskThisFrame = false;
		m_hasDepthThisFrame = false;
		for (int i = 0; i < D3D11Presenter::kNumSlots; ++i) {
			SafeRelease(m_sharedSurf[i]);
			SafeRelease(m_sharedTex[i]);
			m_sharedHandle[i] = nullptr;
			SafeRelease(m_shared3dSurf[i]);
			SafeRelease(m_shared3dTex[i]);
			m_shared3dHandle[i] = nullptr;
			SafeRelease(m_sharedDepthSurf[i]);
			SafeRelease(m_sharedDepthTex[i]);
			m_sharedDepthHandle[i] = nullptr;
		}
	}

	bool CreateGameTargets(UINT w, UINT h, bool allowResetEx)
	{
		ReleaseGameTargets();

		D3DFORMAT dsFmt = (m_pp.EnableAutoDepthStencil && m_pp.AutoDepthStencilFormat != D3DFMT_UNKNOWN)
			? m_pp.AutoDepthStencilFormat : D3DFMT_D24S8;

		const bool wantDepth  = WantDepthTargets();
		const bool want3dMask = Want3dMaskTargets();
		D3DMULTISAMPLE_TYPE effMsaa = wantDepth ? D3DMULTISAMPLE_NONE : m_requestedMsaa;
		DWORD effMsaaQ = wantDepth ? 0u : m_requestedMsaaQ;
		if (wantDepth && m_requestedMsaa != D3DMULTISAMPLE_NONE) {
			Log("AO: D3D11 XeGTAO active (quality=%d) - overriding MSAA=%d to non-MSAA INTZ depth (use render_scale=1.5 for SSAA)",
				g_cfg.aoQuality, (int)m_requestedMsaa);
		}
		m_pp.MultiSampleType = effMsaa;
		m_pp.MultiSampleQuality = effMsaaQ;

		// If we need ResetEx on the hidden D3D9Ex swapchain, ALWAYS run it BEFORE allocating any
		// D3DPOOL_DEFAULT shared textures or INTZ surfaces, and ONLY when allowResetEx == true
		// (never mid-game during live /md rebuilds where GTA SA / SA-MP hold D3DPOOL_DEFAULT objects).
		bool useRealSwapchainNow = false;
		if (m_realUsesSwapchain) {
			bool ppMatches = (m_realPP.BackBufferWidth == w && m_realPP.BackBufferHeight == h &&
			                  m_realPP.MultiSampleType == effMsaa &&
			                  m_realPP.MultiSampleQuality == effMsaaQ &&
			                  m_realPP.AutoDepthStencilFormat == dsFmt);
			if (ppMatches) {
				useRealSwapchainNow = true;
			} else if (allowResetEx) {
				if (m_dummyHwnd)
					SetWindowPos(m_dummyHwnd, nullptr, 0, 0, (int)w, (int)h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
				D3DPRESENT_PARAMETERS nextPP = m_realPP;
				nextPP.BackBufferWidth = w;
				nextPP.BackBufferHeight = h;
				nextPP.MultiSampleType = effMsaa;
				nextPP.MultiSampleQuality = effMsaaQ;
				nextPP.EnableAutoDepthStencil = TRUE;
				nextPP.AutoDepthStencilFormat = dsFmt;
				HRESULT rhr = m_real->ResetEx(&nextPP, nullptr);
				if (FAILED(rhr) && (nextPP.MultiSampleType != D3DMULTISAMPLE_NONE || nextPP.AutoDepthStencilFormat != D3DFMT_D24S8)) {
					nextPP.MultiSampleType = D3DMULTISAMPLE_NONE;
					nextPP.MultiSampleQuality = 0;
					nextPP.AutoDepthStencilFormat = D3DFMT_D24S8;
					rhr = m_real->ResetEx(&nextPP, nullptr);
				}
				if (SUCCEEDED(rhr)) {
					m_realPP = nextPP;
					m_pp.MultiSampleType = nextPP.MultiSampleType;
					m_pp.MultiSampleQuality = nextPP.MultiSampleQuality;
					useRealSwapchainNow = true;
				} else {
					Log("D3D9Ex: ResetEx(%ux%u) failed (0x%08lX) - switching to offscreen targets", w, h, rhr);
					m_realUsesSwapchain = false;
				}
			}
		}

		// Ping-pong shared color textures between D3D9Ex and the async D3D11 presenter thread.
		for (int i = 0; i < D3D11Presenter::kNumSlots; ++i) {
			HANDLE sh = nullptr;
			HRESULT hr = m_real->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_sharedTex[i], &sh);
			if (FAILED(hr) || !m_sharedTex[i] || !sh) {
				Log("D3D9Ex: CreateTexture(shared[%d] %ux%u) failed (0x%08lX)", i, w, h, hr);
				return false;
			}
			m_sharedHandle[i] = sh;
			hr = m_sharedTex[i]->GetSurfaceLevel(0, &m_sharedSurf[i]);
			if (FAILED(hr) || !m_sharedSurf[i]) return false;
		}
		m_writeSlot = 0;
		m_frontSlot = 0;

		// Optional 3D-only snapshot ping-pong targets (isolates 2D SA-MP chat/HUD/dialogs from XeGTAO/CAS/LUT)
		if (want3dMask) {
			for (int i = 0; i < D3D11Presenter::kNumSlots; ++i) {
				HANDLE sh = nullptr;
				if (SUCCEEDED(m_real->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_shared3dTex[i], &sh)) &&
				    m_shared3dTex[i] && sh &&
				    SUCCEEDED(m_shared3dTex[i]->GetSurfaceLevel(0, &m_shared3dSurf[i])) && m_shared3dSurf[i]) {
					m_shared3dHandle[i] = sh;
				} else {
					SafeRelease(m_shared3dSurf[i]);
					SafeRelease(m_shared3dTex[i]);
					m_shared3dHandle[i] = nullptr;
				}
			}
		}

		// Optional INTZ depth texture + shared R32F depth resolve targets for native D3D11 XeGTAO
		bool intzReady = false;
		if (wantDepth && m_depthCopyPS) {
			if (SUCCEEDED(m_real->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL, FOURCC_INTZ, D3DPOOL_DEFAULT, &m_intzDepthTex, nullptr)) &&
			    m_intzDepthTex &&
			    SUCCEEDED(m_intzDepthTex->GetSurfaceLevel(0, &m_depthSurf)) && m_depthSurf) {
				bool r32Ok = true;
				for (int i = 0; i < D3D11Presenter::kNumSlots; ++i) {
					HANDLE sh = nullptr;
					if (FAILED(m_real->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &m_sharedDepthTex[i], &sh)) ||
					    !m_sharedDepthTex[i] || !sh ||
					    FAILED(m_sharedDepthTex[i]->GetSurfaceLevel(0, &m_sharedDepthSurf[i])) || !m_sharedDepthSurf[i]) {
						r32Ok = false;
						break;
					}
					m_sharedDepthHandle[i] = sh;
				}
				if (r32Ok) {
					intzReady = true;
				} else {
					SafeRelease(m_depthSurf);
					SafeRelease(m_intzDepthTex);
					for (int i = 0; i < D3D11Presenter::kNumSlots; ++i) {
						SafeRelease(m_sharedDepthSurf[i]);
						SafeRelease(m_sharedDepthTex[i]);
						m_sharedDepthHandle[i] = nullptr;
					}
				}
			} else {
				SafeRelease(m_depthSurf);
				SafeRelease(m_intzDepthTex);
			}
		}

		const bool wantDirect = g_cfg.directRenderTargets && g_cfg.asyncPresent && m_presenter.CanShareRenderTargets() &&
		                        m_pp.MultiSampleType == D3DMULTISAMPLE_NONE && m_pp.SwapEffect == D3DSWAPEFFECT_DISCARD;
		if (wantDirect) useRealSwapchainNow = false;
		if (useRealSwapchainNow) {
			HRESULT hr1 = m_real->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_backBufferSurf);
			HRESULT hr2 = intzReady ? S_OK : m_real->GetDepthStencilSurface(&m_depthSurf);
			if (FAILED(hr1) || !m_backBufferSurf || FAILED(hr2) || !m_depthSurf) {
				SafeRelease(m_backBufferSurf);
				if (!intzReady) SafeRelease(m_depthSurf);
				useRealSwapchainNow = false;
			}
		}

		if (!useRealSwapchainNow) {
			HRESULT hr = m_real->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, m_pp.MultiSampleType, m_pp.MultiSampleQuality, FALSE, &m_backBufferSurf, nullptr);
			if (FAILED(hr) || !m_backBufferSurf) {
				m_pp.MultiSampleType = D3DMULTISAMPLE_NONE;
				m_pp.MultiSampleQuality = 0;
				hr = m_real->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &m_backBufferSurf, nullptr);
				if (FAILED(hr) || !m_backBufferSurf) {
					Log("D3D9Ex: CreateRenderTarget(%ux%u) failed (0x%08lX)", w, h, hr);
					return false;
				}
			}
			if (!intzReady) {
				hr = m_real->CreateDepthStencilSurface(w, h, dsFmt, m_pp.MultiSampleType, m_pp.MultiSampleQuality, TRUE, &m_depthSurf, nullptr);
				if (FAILED(hr))
					hr = m_real->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, m_pp.MultiSampleType, m_pp.MultiSampleQuality, TRUE, &m_depthSurf, nullptr);
				if (FAILED(hr) || !m_depthSurf) {
					Log("D3D9Ex: CreateDepthStencilSurface(%ux%u) failed (0x%08lX)", w, h, hr);
					return false;
				}
			}
		}

		for (int i = 0; i < D3D11Presenter::kNumSlots; ++i) {
			m_real->SetRenderTarget(0, m_sharedSurf[i]);
			m_real->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
		}
		if (wantDirect && SUCCEEDED(m_real->SetRenderTarget(0, m_sharedSurf[m_writeSlot]))) {
			m_directFallbackSurf = m_backBufferSurf;
			m_backBufferSurf = m_sharedSurf[m_writeSlot];
			m_backBufferSurf->AddRef();
			Log("Bridge: rendering directly into shared color slots");
		} else {
			m_real->SetRenderTarget(0, m_backBufferSurf);
		}
		m_real->SetDepthStencilSurface(m_depthSurf);
		m_rt0IsBackBuffer = true;
		D3DVIEWPORT9 vp = { 0, 0, w, h, 0.0f, 1.0f };
		m_real->SetViewport(&vp);
		m_real->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);
		InvalidateStateCache();

		if (intzReady && want3dMask) m_playerMirror.Create(m_real);
		return m_presenter.BindSharedTextures(
			m_sharedHandle,
			m_shared3dHandle[0] ? m_shared3dHandle : nullptr,
			m_sharedDepthHandle[0] ? m_sharedDepthHandle : nullptr, m_playerMirror.handles,
			w, h);
	}

	IDirect3D9            *m_proxyD3D;
	IDirect3DDevice9Ex    *m_real;
	HWND                   m_gameHwnd;
	HWND                   m_dummyHwnd;
	D3DPRESENT_PARAMETERS  m_pp;
	D3DPRESENT_PARAMETERS  m_realPP;
	bool                   m_realUsesSwapchain = false;
	D3DMULTISAMPLE_TYPE    m_requestedMsaa = D3DMULTISAMPLE_NONE;
	DWORD                  m_requestedMsaaQ = 0;
	UINT                   m_adapter;
	D3DDEVTYPE             m_devType;
	DWORD                  m_behaviorFlags;
	LUID                   m_adapterLuid = {};
	bool                   m_hasLuid = false;
	UINT                   m_refreshHz = 60;
	volatile LONG          m_ref;
	D3DGAMMARAMP           m_gamma = {};

	IDirect3DStateBlock9  *m_defaultState = nullptr;
	IDirect3DStateBlock9  *m_depthCopySB = nullptr;
	IDirect3DPixelShader9 *m_depthCopyPS = nullptr;
	ProxyStableSurface9   *m_proxyBackBuffer = nullptr;
	ProxyStableSurface9   *m_proxyDepthSurf = nullptr;
	IDirect3DTexture9     *m_sharedTex[D3D11Presenter::kNumSlots] = {};
	IDirect3DSurface9     *m_sharedSurf[D3D11Presenter::kNumSlots] = {};
	HANDLE                 m_sharedHandle[D3D11Presenter::kNumSlots] = {};
	IDirect3DTexture9     *m_shared3dTex[D3D11Presenter::kNumSlots] = {};
	IDirect3DSurface9     *m_shared3dSurf[D3D11Presenter::kNumSlots] = {};
	HANDLE                 m_shared3dHandle[D3D11Presenter::kNumSlots] = {};
	IDirect3DTexture9     *m_sharedDepthTex[D3D11Presenter::kNumSlots] = {};
	IDirect3DSurface9     *m_sharedDepthSurf[D3D11Presenter::kNumSlots] = {};
	HANDLE                 m_sharedDepthHandle[D3D11Presenter::kNumSlots] = {};
	IDirect3DTexture9     *m_intzDepthTex = nullptr;
	IDirect3DQuery9       *m_evQuery[D3D11Presenter::kNumSlots] = {};
	int                    m_writeSlot = 0;
	int                    m_frontSlot = 0;
	uint64_t               m_curFrameId = 0;
	int64_t                m_curInputQpc = 0;
	PlayerReflection       m_playerMirror;
	float                  m_actorGround[4] = {};
	bool                   m_actorCaptured = false;
	bool m_replayPlayerDraw = false;
	FrameCameraParams      m_curCamParams = {};
	RoadSurfaceCache       m_roadSurfaceCache;
	bool                   m_hasCameraParamsThisFrame = false;
	bool                   m_rt0IsBackBuffer = true;
	bool                   m_frontEndFrame = false;
	bool                   m_captured3dThisFrame = false;
	bool                   m_has3dMaskThisFrame = false;
	bool                   m_hasDepthThisFrame = false;
	LONG                   m_proxyCfgSeq = 0;
	UINT                   m_logicalBaseW = 0;
	UINT                   m_logicalBaseH = 0;
	IDirect3DSurface9     *m_backBufferSurf = nullptr;
	IDirect3DSurface9     *m_directFallbackSurf = nullptr;
	IDirect3DSurface9     *m_depthSurf = nullptr;
	IDirect3DSurface9     *m_sysReadbackSurf = nullptr;
	UINT                   m_sysReadbackW = 0;
	UINT                   m_sysReadbackH = 0;
	D3DFORMAT              m_sysReadbackFmt = D3DFMT_UNKNOWN;

	D3D11Presenter         m_presenter;
	FramePacer             m_pacer;
	WNDPROC                m_prevWndProc = nullptr;
	POINT                  m_desktopCursor = {};
	bool                   m_hasDesktopCursor = false;
	bool                   m_isUnicodeWnd = false;
	bool                   m_isMinimized = false;
	bool                   m_isForeground = false;
	UINT                   m_cachedClientW = 0;
	UINT                   m_cachedClientH = 0;
	DWORD                  m_lastWinSyncTick = 0;
	bool                   m_borderless = true;
	bool                   m_appliedBorderlessStyle = false;
	bool                   m_pendingForceWindowed = false;
	bool                   m_forceWinInverted = false;
	bool                   m_lastCallerWindowed = false;
	RECT                   m_wantWinRect = {};
	bool                   m_hasWantWinRect = false;
	RECT                   m_userWinRect = {};
	bool                   m_hasUserWinRect = false;
	bool                   m_inOwnResize = false;
	bool                   m_inSizeMove = false;
	DWORD                  m_pendingWinResizeTick = 0;
	UINT                   m_fullscreenW = 0;
	UINT                   m_fullscreenH = 0;
	bool                   m_hasSavedRwMode = false;
	int                    m_savedRwModeIdx = -1;
	DWORD                  m_savedRwModeFlags = 0;
	DWORD                  m_modeGuardUntil = 0;
	bool                   m_everActivated = false;
	bool                   m_deactivating = false;
	bool                   m_closing = false;
	bool                   m_renderStartedThisFrame = false;
	bool                   m_inputSampledThisFrame = false;
	bool                   m_inPresent = false;
	bool                   m_inReset = false;
	bool                   m_inEndScene = false;
	DWORD                  m_initTick = 0;
	int                    m_startupFrames = 0;
	int                    m_errLogs = 0;

	// Redundant state read/write cache (invalidated on Reset, CreateGameTargets, and StateBlock::Apply)
	bool                   m_inStateBlock = false;
	bool                   m_cacheBypassed = false;
	BYTE                   m_rsValid[256] = {};
	DWORD                  m_rsVal[256] = {};
	BYTE                   m_ssValid[8][16] = {};
	DWORD                  m_ssVal[8][16] = {};
	BYTE                   m_tssValid[8][36] = {};
	DWORD                  m_tssVal[8][36] = {};
	BYTE                   m_texValid[8] = {};
	IDirect3DBaseTexture9 *m_texGpu[8] = {};
	IDirect3DBaseTexture9 *m_texManaged[8] = {};
	uint8_t                m_boundManagedMask = 0;
	BYTE                   m_streamValid[4] = {};
	IDirect3DVertexBuffer9 *m_streamVb[4] = {};
	UINT                   m_streamOff[4] = {};
	UINT                   m_streamStride[4] = {};
	BYTE                   m_streamFreqValid[4] = {};
	UINT                   m_streamFreqVal[4] = {};
	BYTE                   m_lightEnValid[16] = {};
	BOOL                   m_lightEnVal[16] = {};
	BYTE                   m_lightValid[16] = {};
	D3DLIGHT9              m_lightVal[16] = {};
	BYTE                   m_clipValid[6] = {};
	float                  m_clipVal[6][4] = {};
	bool                   m_dirtySinceClear = true;
	DWORD                  m_lastClearFlags = 0;
	D3DCOLOR               m_lastClearColor = 0;
	float                  m_lastClearZ = 1.0f;
	DWORD                  m_lastClearStencil = 0;

	// State-cache-aware TRIANGLELIST UP draw-call coalescing (zero-alloc fixed 48KB staging fits driver UP fast-path!)
	static constexpr UINT  kUpBatchMaxBytes   = 48u * 1024u;
	static constexpr UINT  kUpBatchMaxIndices = 12288u;
	uint8_t                m_batchVertBuf[kUpBatchMaxBytes];
	uint16_t               m_batchIdxBuf[kUpBatchMaxIndices];
	UINT                   m_batchVertBytes = 0;
	UINT                   m_batchIdxCount  = 0;
	UINT                   m_batchPrimCount = 0;
	UINT                   m_batchVertCount = 0;
	UINT                   m_batchStride = 0;
	bool                   m_batchIndexed = false;
	bool                   m_batchLocalXYZ = false;
	bool                   m_batchBuffered = false;
	bool                   m_appUsesQueries = false;
	bool                   m_hasCpuMeshVertices = false;
	bool                   m_hasCpuMeshIndices = false;
	D3DMATRIX              m_batchWorld = {};
	HANDLE                 m_mmcssGame = nullptr;
	BYTE                   m_xformValid[18] = {};
	D3DMATRIX              m_xformVal[18] = {};
	BYTE                   m_vsConstFValid[256] = {};
	float                  m_vsConstF[256][4] = {};
	BYTE                   m_psConstFValid[32] = {};
	float                  m_psConstF[32][4] = {};
	BYTE                   m_vsConstIValid[16] = {};
	int                    m_vsConstI[16][4] = {};
	BYTE                   m_psConstIValid[16] = {};
	int                    m_psConstI[16][4] = {};
	BYTE                   m_vsConstBValid[16] = {};
	BOOL                   m_vsConstB[16] = {};
	BYTE                   m_psConstBValid[16] = {};
	BOOL                   m_psConstB[16] = {};
	bool                   m_vpValid = false;
	D3DVIEWPORT9           m_vpVal = {};
	bool                   m_scissorValid = false;
	RECT                   m_scissorVal = {};
	bool                   m_matValid = false;
	D3DMATERIAL9           m_matVal = {};
	bool                   m_fvfValid = false;
	DWORD                  m_fvfVal = 0;
	bool                   m_vdeclValid = false;
	IDirect3DVertexDeclaration9 *m_vdecl = nullptr;
	bool                   m_vsValid = false;
	IDirect3DVertexShader9 *m_vs = nullptr;
	bool                   m_psValid = false;
	IDirect3DPixelShader9  *m_ps = nullptr;
	bool                   m_ibValid = false;
	IDirect3DIndexBuffer9  *m_ib = nullptr;
	D3DMATRIX              m_lastViewMat = {};
	D3DMATRIX              m_sceneViewMat = {};
	D3DMATRIX              m_sceneProjMat = {};
	bool                   m_hasLastView = false;
	bool                   m_sceneProjActive = false;
	bool                   m_hasSceneMats = false;
	bool                   m_checkedDrawTransforms = false;
};

ProxyStateBlock9::ProxyStateBlock9(ProxyDirect3DDevice9 *dev, IDirect3DStateBlock9 *real)
	: m_dev(dev), m_real(real), m_ref(1)
{
	if (m_dev) m_dev->AddRef();
}
ProxyStateBlock9::~ProxyStateBlock9()
{
	SafeRelease(m_real);
	if (m_dev) m_dev->Release();
}
HRESULT STDMETHODCALLTYPE ProxyStateBlock9::GetDevice(IDirect3DDevice9 **ppDevice)
{
	if (!ppDevice) return D3DERR_INVALIDCALL;
	*ppDevice = m_dev;
	m_dev->AddRef();
	return S_OK;
}
HRESULT STDMETHODCALLTYPE ProxyStateBlock9::Capture()
{
	if (m_dev) m_dev->FlushUpBatch();
	return m_real->Capture();
}
HRESULT STDMETHODCALLTYPE ProxyStateBlock9::Apply()
{
	// CRITICAL: Flush any buffered TRIANGLELIST UP draws (e.g. SA-MP player HP/armor bars & ID3DXSprite!)
	// BEFORE applying the StateBlock, otherwise restoring the previous D3D9 state overwrites FVF/Z/Texture/Transform!
	if (m_dev) m_dev->FlushUpBatch();
	if (HasDirtyManagedResources())
		SyncAllDirtyManagedResources();
	HRESULT hr = m_real->Apply();
	if (m_dev) m_dev->InvalidateStateCache();
	return hr;
}

ProxySwapChain9::ProxySwapChain9(ProxyDirect3DDevice9 *dev) : m_dev(dev), m_ref(1)
{
	if (m_dev) m_dev->AddRef();
}
ProxySwapChain9::~ProxySwapChain9()
{
	if (m_dev) m_dev->Release();
}

HRESULT STDMETHODCALLTYPE ProxySwapChain9::QueryInterface(REFIID riid, void **ppvObj)
{
	if (!ppvObj) return E_POINTER;
	if (riid == IID_IUnknown || riid == IID_IDirect3DSwapChain9) {
		*ppvObj = this;
		AddRef();
		return S_OK;
	}
	*ppvObj = nullptr;
	return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE ProxySwapChain9::Present(CONST RECT *pSrc, CONST RECT *pDst, HWND hWnd, CONST RGNDATA *pDirty, DWORD) { return m_dev->Present(pSrc, pDst, hWnd, pDirty); }
HRESULT STDMETHODCALLTYPE ProxySwapChain9::GetFrontBufferData(IDirect3DSurface9 *pDestSurface) { return m_dev->GetFrontBufferDataInternal(pDestSurface); }
HRESULT STDMETHODCALLTYPE ProxySwapChain9::GetBackBuffer(UINT iBackBuffer, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **ppBackBuffer) { return m_dev->GetBackBufferInternal(iBackBuffer, ppBackBuffer); }
HRESULT STDMETHODCALLTYPE ProxySwapChain9::GetRasterStatus(D3DRASTER_STATUS *pRasterStatus) { return m_dev->GetRasterStatus(0, pRasterStatus); }
HRESULT STDMETHODCALLTYPE ProxySwapChain9::GetDisplayMode(D3DDISPLAYMODE *pMode) { return m_dev->GetDisplayModeInternal(pMode); }
HRESULT STDMETHODCALLTYPE ProxySwapChain9::GetDevice(IDirect3DDevice9 **ppDevice)
{
	if (!ppDevice) return D3DERR_INVALIDCALL;
	*ppDevice = m_dev;
	m_dev->AddRef();
	return S_OK;
}
HRESULT STDMETHODCALLTYPE ProxySwapChain9::GetPresentParameters(D3DPRESENT_PARAMETERS *pPP) { return m_dev->GetPresentParametersInternal(pPP); }

IDirect3DSurface9 *ProxyStableSurface9::Resolve() const
{
	if (!m_dev) return nullptr;
	return m_isDepth ? m_dev->ActiveDepthSurf() : m_dev->ActiveBackBuffer();
}
HRESULT STDMETHODCALLTYPE ProxyStableSurface9::GetDevice(IDirect3DDevice9 **ppDevice)
{
	if (!ppDevice || !m_dev) return D3DERR_INVALIDCALL;
	*ppDevice = m_dev;
	m_dev->AddRef();
	return S_OK;
}
HRESULT STDMETHODCALLTYPE ProxyStableSurface9::SetPrivateData(REFGUID refguid, CONST void *pData, DWORD SizeOfData, DWORD Flags)
{
	IDirect3DSurface9 *s = Resolve();
	return s ? s->SetPrivateData(refguid, pData, SizeOfData, Flags) : D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE ProxyStableSurface9::GetPrivateData(REFGUID refguid, void *pData, DWORD *pSizeOfData)
{
	IDirect3DSurface9 *s = Resolve();
	return s ? s->GetPrivateData(refguid, pData, pSizeOfData) : D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE ProxyStableSurface9::FreePrivateData(REFGUID refguid)
{
	IDirect3DSurface9 *s = Resolve();
	return s ? s->FreePrivateData(refguid) : D3DERR_INVALIDCALL;
}
DWORD STDMETHODCALLTYPE ProxyStableSurface9::SetPriority(DWORD PriorityNew)
{
	IDirect3DSurface9 *s = Resolve();
	return s ? s->SetPriority(PriorityNew) : 0;
}
DWORD STDMETHODCALLTYPE ProxyStableSurface9::GetPriority()
{
	IDirect3DSurface9 *s = Resolve();
	return s ? s->GetPriority() : 0;
}
void STDMETHODCALLTYPE ProxyStableSurface9::PreLoad()
{
	IDirect3DSurface9 *s = Resolve();
	if (s) s->PreLoad();
}
HRESULT STDMETHODCALLTYPE ProxyStableSurface9::GetContainer(REFIID riid, void **ppContainer)
{
	IDirect3DSurface9 *s = Resolve();
	return s ? s->GetContainer(riid, ppContainer) : D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE ProxyStableSurface9::GetDesc(D3DSURFACE_DESC *pDesc)
{
	IDirect3DSurface9 *s = Resolve();
	return s ? s->GetDesc(pDesc) : D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE ProxyStableSurface9::LockRect(D3DLOCKED_RECT *pLockedRect, CONST RECT *pRect, DWORD Flags)
{
	IDirect3DSurface9 *s = Resolve();
	return s ? s->LockRect(pLockedRect, pRect, Flags) : D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE ProxyStableSurface9::UnlockRect()
{
	IDirect3DSurface9 *s = Resolve();
	return s ? s->UnlockRect() : D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE ProxyStableSurface9::GetDC(HDC *phdc)
{
	IDirect3DSurface9 *s = Resolve();
	return s ? s->GetDC(phdc) : D3DERR_INVALIDCALL;
}
HRESULT STDMETHODCALLTYPE ProxyStableSurface9::ReleaseDC(HDC hdc)
{
	IDirect3DSurface9 *s = Resolve();
	return s ? s->ReleaseDC(hdc) : D3DERR_INVALIDCALL;
}

class ProxyDirect3D9 final : public IDirect3D9 {
public:
	ProxyDirect3D9(IDirect3D9Ex *realEx, Direct3DCreate9_t fallbackCreate9, UINT sdkVersion)
		: m_real(realEx), m_fallbackCreate9(fallbackCreate9), m_sdkVersion(sdkVersion), m_ref(1) {}
	~ProxyDirect3D9() { SafeRelease(m_real); }

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObj) override
	{
		if (!ppvObj) return E_POINTER;
		if (riid == IID_IUnknown || riid == IID_IDirect3D9) {
			*ppvObj = this;
			AddRef();
			return S_OK;
		}
		*ppvObj = nullptr;
		return E_NOINTERFACE;
	}
	ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&m_ref); }
	ULONG STDMETHODCALLTYPE Release() override
	{
		ULONG r = (ULONG)InterlockedDecrement(&m_ref);
		if (r == 0) delete this;
		return r;
	}

	HRESULT STDMETHODCALLTYPE RegisterSoftwareDevice(void *pInitializeFunction) override { return m_real->RegisterSoftwareDevice(pInitializeFunction); }
	UINT    STDMETHODCALLTYPE GetAdapterCount() override { return m_real->GetAdapterCount(); }
	HRESULT STDMETHODCALLTYPE GetAdapterIdentifier(UINT Adapter, DWORD Flags, D3DADAPTER_IDENTIFIER9 *pIdentifier) override { return m_real->GetAdapterIdentifier(Adapter, Flags, pIdentifier); }
	UINT    STDMETHODCALLTYPE GetAdapterModeCount(UINT Adapter, D3DFORMAT Format) override { return m_real->GetAdapterModeCount(Adapter, Format); }
	HRESULT STDMETHODCALLTYPE EnumAdapterModes(UINT Adapter, D3DFORMAT Format, UINT Mode, D3DDISPLAYMODE *pMode) override { return m_real->EnumAdapterModes(Adapter, Format, Mode, pMode); }
	HRESULT STDMETHODCALLTYPE GetAdapterDisplayMode(UINT Adapter, D3DDISPLAYMODE *pMode) override { return m_real->GetAdapterDisplayMode(Adapter, pMode); }
	HRESULT STDMETHODCALLTYPE CheckDeviceType(UINT Adapter, D3DDEVTYPE DevType, D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, BOOL bWindowed) override { return m_real->CheckDeviceType(Adapter, DevType, AdapterFormat, BackBufferFormat, bWindowed); }
	HRESULT STDMETHODCALLTYPE CheckDeviceFormat(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, DWORD Usage, D3DRESOURCETYPE RType, D3DFORMAT CheckFormat) override { return m_real->CheckDeviceFormat(Adapter, DeviceType, AdapterFormat, Usage, RType, CheckFormat); }
	HRESULT STDMETHODCALLTYPE CheckDeviceMultiSampleType(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SurfaceFormat, BOOL Windowed, D3DMULTISAMPLE_TYPE MultiSampleType, DWORD *pQualityLevels) override { return m_real->CheckDeviceMultiSampleType(Adapter, DeviceType, SurfaceFormat, Windowed, MultiSampleType, pQualityLevels); }
	HRESULT STDMETHODCALLTYPE CheckDepthStencilMatch(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT AdapterFormat, D3DFORMAT RenderTargetFormat, D3DFORMAT DepthStencilFormat) override { return m_real->CheckDepthStencilMatch(Adapter, DeviceType, AdapterFormat, RenderTargetFormat, DepthStencilFormat); }
	HRESULT STDMETHODCALLTYPE CheckDeviceFormatConversion(UINT Adapter, D3DDEVTYPE DeviceType, D3DFORMAT SourceFormat, D3DFORMAT TargetFormat) override { return m_real->CheckDeviceFormatConversion(Adapter, DeviceType, SourceFormat, TargetFormat); }
	HRESULT STDMETHODCALLTYPE GetDeviceCaps(UINT Adapter, D3DDEVTYPE DeviceType, D3DCAPS9 *pCaps) override { return m_real->GetDeviceCaps(Adapter, DeviceType, pCaps); }
	HMONITOR STDMETHODCALLTYPE GetAdapterMonitor(UINT Adapter) override { return m_real->GetAdapterMonitor(Adapter); }

	HRESULT FallbackNativeCreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
	                                   D3DPRESENT_PARAMETERS *pPP, IDirect3DDevice9 **ppReturnedDeviceInterface)
	{
		if (m_fallbackCreate9) {
			IDirect3D9 *classic = m_fallbackCreate9(m_sdkVersion);
			if (classic) {
				HRESULT hr = classic->CreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppReturnedDeviceInterface);
				classic->Release();
				return hr;
			}
		}
		return D3DERR_NOTAVAILABLE;
	}

	HRESULT STDMETHODCALLTYPE CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, HWND hFocusWindow, DWORD BehaviorFlags,
	                                       D3DPRESENT_PARAMETERS *pPP, IDirect3DDevice9 **ppReturnedDeviceInterface) override
	{
		if (!pPP || !ppReturnedDeviceInterface) return D3DERR_INVALIDCALL;
		HWND gameHwnd = hFocusWindow ? hFocusWindow : pPP->hDeviceWindow;
		if (!gameHwnd)
			return FallbackNativeCreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppReturnedDeviceInterface);

		RECT monRc = {};
		UINT hz = 60;
		QueryMonitorRect(gameHwnd, monRc, hz);
		UINT monW = (UINT)(monRc.right - monRc.left);
		UINT monH = (UINT)(monRc.bottom - monRc.top);
		UINT gameW = pPP->BackBufferWidth  ? pPP->BackBufferWidth  : monW;
		UINT gameH = pPP->BackBufferHeight ? pPP->BackBufferHeight : monH;
		D3DFORMAT dsFmt = (pPP->EnableAutoDepthStencil && pPP->AutoDepthStencilFormat != D3DFMT_UNKNOWN)
			? pPP->AutoDepthStencilFormat : D3DFMT_D24S8;

		HWND dummyHwnd = CreateHiddenDummyWindow((int)gameW, (int)gameH);
		if (!dummyHwnd)
			return FallbackNativeCreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppReturnedDeviceInterface);

		D3DPRESENT_PARAMETERS realPP = {};
		realPP.Windowed = TRUE;
		realPP.SwapEffect = D3DSWAPEFFECT_DISCARD;
		realPP.BackBufferFormat = D3DFMT_A8R8G8B8;
		realPP.BackBufferWidth = gameW;
		realPP.BackBufferHeight = gameH;
		realPP.BackBufferCount = 1;
		realPP.MultiSampleType = pPP->MultiSampleType;
		realPP.MultiSampleQuality = pPP->MultiSampleQuality;
		realPP.EnableAutoDepthStencil = TRUE;
		realPP.AutoDepthStencilFormat = dsFmt;
		realPP.hDeviceWindow = dummyHwnd;
		realPP.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

		DWORD flags = BehaviorFlags | D3DCREATE_FPU_PRESERVE;
		IDirect3DDevice9Ex *realDevEx = nullptr;
		bool usesRealSwapchain = true;
		HRESULT hr = m_real->CreateDeviceEx(Adapter, DeviceType, dummyHwnd, flags, &realPP, nullptr, &realDevEx);
		if (FAILED(hr) || !realDevEx) {
			realPP.MultiSampleType = D3DMULTISAMPLE_NONE;
			realPP.MultiSampleQuality = 0;
			realPP.AutoDepthStencilFormat = D3DFMT_D24S8;
			hr = m_real->CreateDeviceEx(Adapter, DeviceType, dummyHwnd, flags, &realPP, nullptr, &realDevEx);
		}
		if (FAILED(hr) || !realDevEx) {
			usesRealSwapchain = false;
			realPP.BackBufferWidth = 1;
			realPP.BackBufferHeight = 1;
			realPP.EnableAutoDepthStencil = FALSE;
			realPP.AutoDepthStencilFormat = D3DFMT_UNKNOWN;
			hr = m_real->CreateDeviceEx(Adapter, DeviceType, dummyHwnd, flags, &realPP, nullptr, &realDevEx);
		}
		if (FAILED(hr) || !realDevEx) {
			Log("D3D9Ex: CreateDeviceEx failed (0x%08lX) - falling back to classic D3D9", hr);
			DestroyWindow(dummyHwnd);
			return FallbackNativeCreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppReturnedDeviceInterface);
		}
		InstallD3D9DetourBridgeFromDevice(realDevEx);

		D3DPRESENT_PARAMETERS effPP = *pPP;
		if (usesRealSwapchain) {
			effPP.MultiSampleType = realPP.MultiSampleType;
			effPP.MultiSampleQuality = realPP.MultiSampleQuality;
		}
		LUID luid = {};
		bool hasLuid = SUCCEEDED(m_real->GetAdapterLUID(Adapter, &luid));
		auto *proxy = new ProxyDirect3DDevice9(this, realDevEx, gameHwnd, dummyHwnd, effPP, realPP, usesRealSwapchain,
		                                       Adapter, DeviceType, BehaviorFlags, luid, hasLuid);
		if (!proxy->InitializeBridge()) {
			Log("Bridge: InitializeBridge failed - falling back to classic D3D9");
			proxy->Release();
			return FallbackNativeCreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppReturnedDeviceInterface);
		}
		// Report effective internal dimensions (e.g. SSAA 2880x1620) back to RenderWare so all
		// initial cameras, post-effect rasters, and SA-MP viewports allocate at full SSAA size.
		UINT effW = 0, effH = 0;
		if (proxy->GetGameSize(effW, effH) && effW > 0 && effH > 0) {
			pPP->BackBufferWidth  = effW;
			pPP->BackBufferHeight = effH;
		}

		*ppReturnedDeviceInterface = proxy;
		return S_OK;
	}

private:
	IDirect3D9Ex     *m_real;
	Direct3DCreate9_t m_fallbackCreate9;
	UINT              m_sdkVersion;
	volatile LONG     m_ref;
};

// Bottom-of-chain d3d9.dll detour bridge: ensures external mods that pattern-scan d3d9.dll
// and detour CD3DBase::Present / Reset / EndScene (e.g. UltraWH, ESPs, overlays) run with
// ProxyDirect3DDevice9* (so D3DPOOL_MANAGED and state caching work transparently).
// Frame pointer preserved so Microsoft Detours 3.0 can safely chain on top if it follows E9.
#pragma optimize("y", off)
static __declspec(noinline) HRESULT STDMETHODCALLTYPE md_bottomPresent(
	IDirect3DDevice9 *dev, CONST RECT *pSrc, CONST RECT *pDst, HWND hWnd, CONST RGNDATA *pDirty)
{
	ProxyDirect3DDevice9 *active = g_activeDev;
	if (active && dev == static_cast<IDirect3DDevice9 *>(active))
		return active->FinishPresent();
	return g_origD3D9Present ? g_origD3D9Present(dev, pSrc, pDst, hWnd, pDirty) : D3DERR_INVALIDCALL;
}

static __declspec(noinline) HRESULT STDMETHODCALLTYPE md_bottomReset(
	IDirect3DDevice9 *dev, D3DPRESENT_PARAMETERS *pPP)
{
	ProxyDirect3DDevice9 *active = g_activeDev;
	if (active && dev == static_cast<IDirect3DDevice9 *>(active))
		return active->ResetInternal(pPP);
	return g_origD3D9Reset ? g_origD3D9Reset(dev, pPP) : D3DERR_INVALIDCALL;
}

static __declspec(noinline) HRESULT STDMETHODCALLTYPE md_bottomEndScene(IDirect3DDevice9 *dev)
{
	ProxyDirect3DDevice9 *active = g_activeDev;
	if (active && dev == static_cast<IDirect3DDevice9 *>(active))
		return active->EndSceneInternal();
	return g_origD3D9EndScene ? g_origD3D9EndScene(dev) : D3DERR_INVALIDCALL;
}
#pragma optimize("", on)

static bool GetModuleImageRange(HMODULE mod, uintptr_t &imgStartOut, uintptr_t &imgEndOut)
{
	if (!mod) return false;
	__try {
		BYTE *base = (BYTE *)mod;
		auto *dos = (IMAGE_DOS_HEADER *)base;
		if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
		auto *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
		if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
		imgStartOut = (uintptr_t)base;
		imgEndOut   = (uintptr_t)(base + nt->OptionalHeader.SizeOfImage);
		return true;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return false;
	}
}

void InstallD3D9DetourBridgeFromDevice(IDirect3DDevice9 *realDev)
{
	if (g_detourBridgeInit || !realDev) return;
	g_detourBridgeInit = true;

	HMODULE d3d9Mod = GetModuleHandleA("d3d9.dll");
	uintptr_t imgStart = 0, imgEnd = 0;
	if (!GetModuleImageRange(d3d9Mod, imgStart, imgEnd)) return;

	void **vt = *(void ***)realDev;
	if (!vt) return;

	auto inImg = [&](void *p) {
		uintptr_t a = (uintptr_t)p;
		return a >= imgStart && a + 16 <= imgEnd;
	};

	bool okPresent = false, okReset = false, okEndScene = false;
	if (inImg(vt[17])) {
		void *tramp = nullptr;
		if (HookInlineFn(vt[17], (void *)md_bottomPresent, &tramp)) {
			g_d3d9PresentEntry = (D3D9Present_t)vt[17];
			g_origD3D9Present  = (D3D9Present_t)tramp;
			okPresent = true;
		}
	}
	if (inImg(vt[16])) {
		void *tramp = nullptr;
		if (HookInlineFn(vt[16], (void *)md_bottomReset, &tramp)) {
			g_d3d9ResetEntry = (D3D9Reset_t)vt[16];
			g_origD3D9Reset  = (D3D9Reset_t)tramp;
			okReset = true;
		}
	}
	if (inImg(vt[42])) {
		g_d3d9EndSceneEntry = (D3D9EndScene_t)vt[42];
		void *tramp = nullptr;
		if (HookInlineFn(vt[42], (void *)md_bottomEndScene, &tramp)) {
			g_origD3D9EndScene = (D3D9EndScene_t)tramp;
			okEndScene = true;
		}
	}

	Log("Bridge: d3d9 detour chain ready (Present=%d, Reset=%d, EndScene=%d)",
		okPresent ? 1 : 0, okReset ? 1 : 0, okEndScene ? 1 : 0);
}

IDirect3D9 *CreateProxyDirect3D9(IDirect3D9Ex *realEx, Direct3DCreate9_t fallbackCreate9, UINT sdkVersion)
{
	return realEx ? new ProxyDirect3D9(realEx, fallbackCreate9, sdkVersion) : nullptr;
}

void SetRawCursorPosFn(SetCursorPos_t fn)
{
	g_rawSetCursorPos = fn;
}

void SetRawGetClientRectFn(GetClientRect_t fn)
{
	g_rawGetClientRect = fn;
}

HWND GetActiveGameWindow()
{
	return g_activeDev ? g_activeDev->GameHwnd() : nullptr;
}

bool GetActiveGameSize(UINT &gameW, UINT &gameH)
{
	return g_activeDev ? g_activeDev->GetGameSize(gameW, gameH) : false;
}

bool NeedsVirtualClientMapping()
{
	return g_activeDev ? g_activeDev->NeedsVirtualMapping() : false;
}

void MapPhysicalClientToGame(LONG &x, LONG &y)
{
	if (g_activeDev) g_activeDev->MapClientToGame(x, y);
}

void MapGameToPhysicalClient(LONG &x, LONG &y)
{
	if (g_activeDev) g_activeDev->MapGameToClient(x, y);
}

bool IsGameWindowDeactivating()
{
	return g_activeDev ? g_activeDev->IsDeactivating() : false;
}

void NotifyInputSample()
{
	if (g_activeDev) g_activeDev->MarkInputSampled();
}

void *RenderEquippedAtomic(void *atomic, RenderRwObject render)
{
	return g_activeDev ? g_activeDev->RenderPlayerWeapon(atomic, render) : render(atomic);
}

void *RenderPlayerClump(void *clump, RenderRwObject render)
{
	return g_activeDev ? g_activeDev->RenderPlayer(clump, render) : render(clump);
}

void NotifyBefore2dStuff()
{
	if (g_activeDev) g_activeDev->OnBefore2dStuff();
}


DisplayTelemetry GetDisplayTelemetry()
{
	if (g_activeDev)
		return g_activeDev->Presenter().SnapshotTelemetry(g_activeDev->UsesRealSwapchain(), g_activeDev->RefreshHz());
	return {};
}
