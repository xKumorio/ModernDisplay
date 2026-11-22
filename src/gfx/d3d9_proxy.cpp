// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "d3d9_proxy.h"
#include "d3d11_presenter.h"
#include "managed_resources.h"
#include "../core/log.h"
#include "../core/samp.h"

#include <string.h>

class ProxyDirect3DDevice9;
static ProxyDirect3DDevice9 *g_activeDev = nullptr;
static SetCursorPos_t        g_rawSetCursorPos = nullptr;

template <typename T>
static inline void SafeRelease(T *&p)
{
	if (p) { p->Release(); p = nullptr; }
}

static HWND CreateHiddenDummyWindow()
{
	static ATOM s_atom = 0;
	HINSTANCE inst = GetModuleHandleA(nullptr);
	if (!s_atom) {
		WNDCLASSA wc = {};
		wc.lpfnWndProc = DefWindowProcA;
		wc.hInstance = inst;
		wc.lpszClassName = "ModernDisplayDummyD3D9";
		s_atom = RegisterClassA(&wc);
	}
	return CreateWindowExA(0, "ModernDisplayDummyD3D9", "", WS_POPUP,
	                       0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
}

static void QueryMonitorRect(HWND hwnd, RECT &rc, UINT &refreshHz)
{
	HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXW mi = {};
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoW(mon, &mi)) {
		rc = mi.rcMonitor;
		DEVMODEW dm = {};
		dm.dmSize = sizeof(dm);
		if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 0)
			refreshHz = dm.dmDisplayFrequency;
		else
			refreshHz = 60;
	} else {
		rc = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
		refreshHz = 60;
	}
}

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

	HRESULT STDMETHODCALLTYPE Present(CONST RECT *pSourceRect, CONST RECT *pDestRect, HWND hDestWindowOverride, CONST RGNDATA *pDirtyRegion, DWORD dwFlags) override;
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

class ProxyDirect3DDevice9 final : public IDirect3DDevice9 {
public:
	ProxyDirect3DDevice9(IDirect3D9 *proxyD3D, IDirect3DDevice9Ex *realDevEx,
	                     HWND gameHwnd, HWND dummyHwnd, const D3DPRESENT_PARAMETERS &pp,
	                     UINT adapter, D3DDEVTYPE devType, DWORD behaviorFlags,
	                     LUID adapterLuid, bool hasLuid)
		: m_proxyD3D(proxyD3D), m_real(realDevEx), m_gameHwnd(gameHwnd), m_dummyHwnd(dummyHwnd),
		  m_pp(pp), m_adapter(adapter), m_devType(devType), m_behaviorFlags(behaviorFlags),
		  m_adapterLuid(adapterLuid), m_hasLuid(hasLuid), m_ref(1)
	{
		m_proxyD3D->AddRef();
		for (int i = 0; i < 256; ++i) {
			WORD v = (WORD)(i << 8);
			m_gamma.red[i] = m_gamma.green[i] = m_gamma.blue[i] = v;
		}
		m_real->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &m_dummyBackBuffer);
		m_real->CreateQuery(D3DQUERYTYPE_EVENT, &m_evQuery);
		m_real->CreateStateBlock(D3DSBT_ALL, &m_defaultState);
	}

	~ProxyDirect3DDevice9()
	{
		if (g_activeDev == this) g_activeDev = nullptr;
		UnhookWindowProc();
		ReleaseGameTargets();
		m_presenter.Shutdown();
		SafeRelease(m_defaultState);
		SafeRelease(m_dummyBackBuffer);
		SafeRelease(m_evQuery);
		SafeRelease(m_real);
		SafeRelease(m_proxyD3D);
		if (m_dummyHwnd) { DestroyWindow(m_dummyHwnd); m_dummyHwnd = nullptr; }
	}

	bool InitializeBridge()
	{
		RECT monRc = {};
		QueryMonitorRect(m_gameHwnd, monRc, m_refreshHz);
		UINT monW = (UINT)(monRc.right - monRc.left);
		UINT monH = (UINT)(monRc.bottom - monRc.top);

		UINT gameW = m_pp.BackBufferWidth  ? m_pp.BackBufferWidth  : monW;
		UINT gameH = m_pp.BackBufferHeight ? m_pp.BackBufferHeight : monH;
		m_pp.BackBufferWidth  = gameW;
		m_pp.BackBufferHeight = gameH;

		// Preserve explicit windowed mode if a windowed-mode mod requested a smaller window;
		// otherwise promote fullscreen to borderless Flip Model for instant Alt-Tab + iFlip.
		m_borderless = !(m_pp.Windowed && gameW < monW && gameH < monH);
		if (m_borderless) {
			ApplyBorderless(true);
			HookWindowProc();
		}

		RECT clientRc = {};
		GetClientRect(m_gameHwnd, &clientRc);
		UINT swapW = (clientRc.right > clientRc.left) ? (UINT)(clientRc.right - clientRc.left) : monW;
		UINT swapH = (clientRc.bottom > clientRc.top) ? (UINT)(clientRc.bottom - clientRc.top) : monH;

		if (!m_presenter.Init(m_gameHwnd, swapW, swapH, m_adapterLuid, m_hasLuid))
			return false;
		if (!CreateGameTargets(gameW, gameH))
			return false;

		g_activeDev = this;
		m_presenter.Reflex().BeginFrame();
		Log("Bridge: ready (mode=%s, game %ux%u, swap %ux%u @ %uHz, MSAA=%d)",
			m_borderless ? "borderless" : "windowed", gameW, gameH, swapW, swapH,
			m_refreshHz, (int)m_pp.MultiSampleType);
		return true;
	}

	D3D11Presenter &Presenter() { return m_presenter; }
	HWND GameHwnd() const { return m_gameHwnd; }
	bool IsDeactivating() const { return m_deactivating; }
	void MarkInputSampled() { m_inputSampledThisFrame = true; m_presenter.Reflex().Mark(RM_INPUT_SAMPLE); }

	void ApplyBorderless(bool force)
	{
		if (!m_borderless || !IsWindow(m_gameHwnd) || IsIconic(m_gameHwnd)) return;
		RECT monRc = {};
		QueryMonitorRect(m_gameHwnd, monRc, m_refreshHz);
		int monW = monRc.right - monRc.left;
		int monH = monRc.bottom - monRc.top;

		LONG style = GetWindowLongA(m_gameHwnd, GWL_STYLE);
		LONG wantStyle = (style & ~(WS_CAPTION | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_SYSMENU | WS_BORDER | WS_DLGFRAME))
		                 | WS_POPUP | WS_VISIBLE | WS_MINIMIZEBOX;
		LONG exStyle = GetWindowLongA(m_gameHwnd, GWL_EXSTYLE);
		LONG wantEx = exStyle & ~(WS_EX_DLGMODALFRAME | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE | WS_EX_WINDOWEDGE | WS_EX_TOPMOST);

		RECT curRc = {};
		GetWindowRect(m_gameHwnd, &curRc);
		if (force || style != wantStyle || exStyle != wantEx ||
		    curRc.left != monRc.left || curRc.top != monRc.top ||
		    (curRc.right - curRc.left) != monW || (curRc.bottom - curRc.top) != monH) {
			SetWindowLongA(m_gameHwnd, GWL_STYLE, wantStyle);
			SetWindowLongA(m_gameHwnd, GWL_EXSTYLE, wantEx);
			SetWindowPos(m_gameHwnd, HWND_NOTOPMOST, monRc.left, monRc.top, monW, monH,
			             SWP_FRAMECHANGED | SWP_SHOWWINDOW | SWP_NOACTIVATE);
		}
	}

	static LRESULT CALLBACK BorderlessWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
	{
		ProxyDirect3DDevice9 *self = g_activeDev;
		WNDPROC prev = self ? self->m_prevWndProc : nullptr;
		auto callPrev = [&](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
			if (self && !self->m_isUnicodeWnd)
				return prev ? CallWindowProcA(prev, h, m, w, l) : DefWindowProcA(h, m, w, l);
			return prev ? CallWindowProcW(prev, h, m, w, l) : DefWindowProcW(h, m, w, l);
		};

		if (self && self->m_borderless && hwnd == self->m_gameHwnd && msg == WM_ACTIVATEAPP) {
			static bool s_inActivate = false;
			if (!s_inActivate) {
				s_inActivate = true;
				if (!wp) {
					self->m_deactivating = true;
					LRESULT res = callPrev(hwnd, msg, wp, lp);
					ClipCursor(nullptr);
					HWND fg = GetForegroundWindow();
					DWORD fgPid = 0;
					if (fg) GetWindowThreadProcessId(fg, &fgPid);
					if (fgPid != GetCurrentProcessId()) {
						HMONITOR gameMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
						HMONITOR fgMon   = fg ? MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) : gameMon;
						if (fgMon == gameMon && !IsIconic(hwnd))
							ShowWindow(hwnd, SW_MINIMIZE);
					}
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
					s_inActivate = false;
					return res;
				} else {
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
					if (IsIconic(hwnd))
						ShowWindow(hwnd, SW_RESTORE);
					LRESULT res = callPrev(hwnd, msg, wp, lp);
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
		m_isUnicodeWnd = (IsWindowUnicode(m_gameHwnd) != FALSE);
		m_prevWndProc = m_isUnicodeWnd
			? (WNDPROC)SetWindowLongPtrW(m_gameHwnd, GWLP_WNDPROC, (LONG_PTR)BorderlessWndProc)
			: (WNDPROC)SetWindowLongPtrA(m_gameHwnd, GWLP_WNDPROC, (LONG_PTR)BorderlessWndProc);
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

	HRESULT PresentInternal()
	{
		if (m_deactivating && !IsIconic(m_gameHwnd) && GetForegroundWindow() == m_gameHwnd)
			m_deactivating = false;

		if (m_startupFrames < 10) {
			++m_startupFrames;
			ApplyBorderless(false);
		}

		if (!IsIconic(m_gameHwnd)) {
			RECT rc = {};
			if (GetClientRect(m_gameHwnd, &rc)) {
				UINT cw = (UINT)(rc.right - rc.left);
				UINT ch = (UINT)(rc.bottom - rc.top);
				if (cw > 0 && ch > 0 && (cw != m_presenter.SwapWidth() || ch != m_presenter.SwapHeight()))
					m_presenter.ResizeSwapChain(cw, ch);
			}
		}

		if (m_backBufferSurf && m_sharedSurf)
			m_real->StretchRect(m_backBufferSurf, nullptr, m_sharedSurf, nullptr, D3DTEXF_NONE);

		if (m_evQuery) {
			m_evQuery->Issue(D3DISSUE_END);
			m_evQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH);
		}

		if (m_presenter.PresentFrame())
			m_presenter.PaceNextFrame();
		SampTickGreeting();
		m_renderStartedThisFrame = false;
		m_inputSampledThisFrame = false;
		return S_OK;
	}

	HRESULT GetBackBufferInternal(UINT iBackBuffer, IDirect3DSurface9 **ppBackBuffer)
	{
		if (!ppBackBuffer || iBackBuffer != 0 || !m_backBufferSurf) return D3DERR_INVALIDCALL;
		*ppBackBuffer = m_backBufferSurf;
		m_backBufferSurf->AddRef();
		return S_OK;
	}

	HRESULT GetFrontBufferDataInternal(IDirect3DSurface9 *pDestSurface)
	{
		// m_sharedSurf is updated only in PresentInternal(), so it always holds the last complete
		// presented front buffer (including SA-MP chat, nametags, dialogs, and HUD).
		if (!pDestSurface || !m_sharedSurf) return D3DERR_INVALIDCALL;
		IDirect3DSurface9 *dst = UnwrapSurfaceForSys(pDestSurface);
		D3DSURFACE_DESC dstDesc = {}, srcDesc = {};
		if (FAILED(dst->GetDesc(&dstDesc)) || FAILED(m_sharedSurf->GetDesc(&srcDesc)))
			return D3DERR_INVALIDCALL;

		// GetRenderTargetData only accepts D3DPOOL_SYSTEMMEM; SA-MP F8 uses D3DPOOL_SCRATCH.
		if (dstDesc.Pool == D3DPOOL_SYSTEMMEM &&
		    dstDesc.Width == srcDesc.Width && dstDesc.Height == srcDesc.Height && dstDesc.Format == srcDesc.Format) {
			HRESULT hr = m_real->GetRenderTargetData(m_sharedSurf, dst);
			if (SUCCEEDED(hr)) MarkSurfaceDirtyIfManaged(pDestSurface);
			return hr;
		}

		IDirect3DSurface9 *tmp = nullptr;
		HRESULT hr = m_real->CreateOffscreenPlainSurface(srcDesc.Width, srcDesc.Height, srcDesc.Format, D3DPOOL_SYSTEMMEM, &tmp, nullptr);
		if (FAILED(hr) || !tmp) return hr;

		hr = m_real->GetRenderTargetData(m_sharedSurf, tmp);
		if (SUCCEEDED(hr)) {
			D3DLOCKED_RECT lrS = {}, lrD = {};
			if (SUCCEEDED(tmp->LockRect(&lrS, nullptr, D3DLOCK_READONLY)) && SUCCEEDED(dst->LockRect(&lrD, nullptr, 0))) {
				if (srcDesc.Width == dstDesc.Width && srcDesc.Height == dstDesc.Height) {
					UINT rowBytes = srcDesc.Width * 4;
					for (UINT y = 0; y < srcDesc.Height; ++y)
						memcpy((BYTE *)lrD.pBits + y * lrD.Pitch, (const BYTE *)lrS.pBits + y * lrS.Pitch, rowBytes);
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
				tmp->UnlockRect();
				MarkSurfaceDirtyIfManaged(pDestSurface);
			} else {
				hr = D3DERR_INVALIDCALL;
			}
		}
		tmp->Release();
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
		RECT monRc = {};
		QueryMonitorRect(m_gameHwnd, monRc, m_refreshHz);
		UINT monW = (UINT)(monRc.right - monRc.left);
		UINT monH = (UINT)(monRc.bottom - monRc.top);

		UINT newW = pPP->BackBufferWidth  ? pPP->BackBufferWidth  : monW;
		UINT newH = pPP->BackBufferHeight ? pPP->BackBufferHeight : monH;
		m_pp = *pPP;
		m_pp.BackBufferWidth  = newW;
		m_pp.BackBufferHeight = newH;

		ApplyBorderless(false);

		RECT clientRc = {};
		GetClientRect(m_gameHwnd, &clientRc);
		UINT swapW = (clientRc.right > clientRc.left) ? (UINT)(clientRc.right - clientRc.left) : monW;
		UINT swapH = (clientRc.bottom > clientRc.top) ? (UINT)(clientRc.bottom - clientRc.top) : monH;

		if (m_defaultState) m_defaultState->Apply();
		m_presenter.ResizeSwapChain(swapW, swapH);
		if (!CreateGameTargets(newW, newH))
			return D3DERR_OUTOFVIDEOMEMORY;

		Log("Bridge: Reset handled cleanly (game %ux%u, swap %ux%u, MSAA=%d)",
			newW, newH, swapW, swapH, (int)m_pp.MultiSampleType);
		return S_OK;
	}
	HRESULT STDMETHODCALLTYPE Present(CONST RECT *, CONST RECT *, HWND, CONST RGNDATA *) override { return PresentInternal(); }
	HRESULT STDMETHODCALLTYPE GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE, IDirect3DSurface9 **ppBackBuffer) override
	{
		return (iSwapChain == 0) ? GetBackBufferInternal(iBackBuffer, ppBackBuffer) : D3DERR_INVALIDCALL;
	}
	HRESULT STDMETHODCALLTYPE GetRasterStatus(UINT iSwapChain, D3DRASTER_STATUS *pRasterStatus) override { return m_real->GetRasterStatus(iSwapChain, pRasterStatus); }
	HRESULT STDMETHODCALLTYPE SetDialogBoxMode(BOOL) override { return S_OK; }
	void    STDMETHODCALLTYPE SetGammaRamp(UINT, DWORD, CONST D3DGAMMARAMP *pRamp) override { if (pRamp) m_gamma = *pRamp; }
	void    STDMETHODCALLTYPE GetGammaRamp(UINT, D3DGAMMARAMP *pRamp) override { if (pRamp) *pRamp = m_gamma; }

	HRESULT STDMETHODCALLTYPE CreateTexture(UINT Width, UINT Height, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DTexture9 **ppTexture, HANDLE *pSharedHandle) override
	{
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
		if (Pool == D3DPOOL_MANAGED)
			return CreateManagedVolumeTexture9(m_real, this, Width, Height, Depth, Levels, Usage, Format, ppVolumeTexture);
		return m_real->CreateVolumeTexture(Width, Height, Depth, Levels, Usage, Format, Pool, ppVolumeTexture, pSharedHandle);
	}
	HRESULT STDMETHODCALLTYPE CreateCubeTexture(UINT EdgeLength, UINT Levels, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DCubeTexture9 **ppCubeTexture, HANDLE *pSharedHandle) override
	{
		if (Pool == D3DPOOL_MANAGED)
			return CreateManagedCubeTexture9(m_real, this, EdgeLength, Levels, Usage, Format, ppCubeTexture);
		return m_real->CreateCubeTexture(EdgeLength, Levels, Usage, Format, Pool, ppCubeTexture, pSharedHandle);
	}
	HRESULT STDMETHODCALLTYPE CreateVertexBuffer(UINT Length, DWORD Usage, DWORD FVF, D3DPOOL Pool, IDirect3DVertexBuffer9 **ppVertexBuffer, HANDLE *pSharedHandle) override
	{
		if (Pool == D3DPOOL_MANAGED) Pool = D3DPOOL_DEFAULT;
		return m_real->CreateVertexBuffer(Length, Usage, FVF, Pool, ppVertexBuffer, pSharedHandle);
	}
	HRESULT STDMETHODCALLTYPE CreateIndexBuffer(UINT Length, DWORD Usage, D3DFORMAT Format, D3DPOOL Pool, IDirect3DIndexBuffer9 **ppIndexBuffer, HANDLE *pSharedHandle) override
	{
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
	HRESULT STDMETHODCALLTYPE UpdateSurface(IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect, IDirect3DSurface9 *pDestinationSurface, CONST POINT *pDestPoint) override
	{
		return m_real->UpdateSurface(UnwrapSurfaceForSys(pSourceSurface), pSourceRect, UnwrapSurfaceForGpu(pDestinationSurface), pDestPoint);
	}
	HRESULT STDMETHODCALLTYPE UpdateTexture(IDirect3DBaseTexture9 *pSourceTexture, IDirect3DBaseTexture9 *pDestinationTexture) override
	{
		return m_real->UpdateTexture(UnwrapBaseTextureForSys(pSourceTexture), UnwrapBaseTextureForGpu(pDestinationTexture));
	}
	HRESULT STDMETHODCALLTYPE GetRenderTargetData(IDirect3DSurface9 *pRenderTarget, IDirect3DSurface9 *pDestSurface) override
	{
		IDirect3DSurface9 *src = UnwrapSurfaceForGpu(pRenderTarget);
		if (src && src == m_backBufferSurf && m_pp.MultiSampleType != D3DMULTISAMPLE_NONE && m_sharedSurf) {
			m_real->StretchRect(m_backBufferSurf, nullptr, m_sharedSurf, nullptr, D3DTEXF_NONE);
			src = m_sharedSurf;
		}
		HRESULT hr = m_real->GetRenderTargetData(src, UnwrapSurfaceForSys(pDestSurface));
		if (SUCCEEDED(hr)) MarkSurfaceDirtyIfManaged(pDestSurface);
		return hr;
	}
	HRESULT STDMETHODCALLTYPE GetFrontBufferData(UINT iSwapChain, IDirect3DSurface9 *pDestSurface) override
	{
		return (iSwapChain == 0) ? GetFrontBufferDataInternal(pDestSurface) : m_real->GetFrontBufferData(iSwapChain, UnwrapSurfaceForSys(pDestSurface));
	}
	HRESULT STDMETHODCALLTYPE StretchRect(IDirect3DSurface9 *pSourceSurface, CONST RECT *pSourceRect, IDirect3DSurface9 *pDestSurface, CONST RECT *pDestRect, D3DTEXTUREFILTERTYPE Filter) override
	{
		return m_real->StretchRect(UnwrapSurfaceForGpu(pSourceSurface), pSourceRect, UnwrapSurfaceForGpu(pDestSurface), pDestRect, Filter);
	}
	HRESULT STDMETHODCALLTYPE ColorFill(IDirect3DSurface9 *pSurface, CONST RECT *pRect, D3DCOLOR color) override
	{
		return m_real->ColorFill(UnwrapSurfaceForGpu(pSurface), pRect, color);
	}
	HRESULT STDMETHODCALLTYPE CreateOffscreenPlainSurface(UINT Width, UINT Height, D3DFORMAT Format, D3DPOOL Pool, IDirect3DSurface9 **ppSurface, HANDLE *pSharedHandle) override
	{
		if (Pool == D3DPOOL_MANAGED) Pool = D3DPOOL_SYSTEMMEM;
		return m_real->CreateOffscreenPlainSurface(Width, Height, Format, Pool, ppSurface, pSharedHandle);
	}
	HRESULT STDMETHODCALLTYPE SetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget) override
	{
		return m_real->SetRenderTarget(RenderTargetIndex, UnwrapSurfaceForGpu(pRenderTarget));
	}
	HRESULT STDMETHODCALLTYPE GetRenderTarget(DWORD RenderTargetIndex, IDirect3DSurface9 **ppRenderTarget) override { return m_real->GetRenderTarget(RenderTargetIndex, ppRenderTarget); }
	HRESULT STDMETHODCALLTYPE SetDepthStencilSurface(IDirect3DSurface9 *pNewZStencil) override { return m_real->SetDepthStencilSurface(UnwrapSurfaceForGpu(pNewZStencil)); }
	HRESULT STDMETHODCALLTYPE GetDepthStencilSurface(IDirect3DSurface9 **ppZStencilSurface) override { return m_real->GetDepthStencilSurface(ppZStencilSurface); }
	HRESULT STDMETHODCALLTYPE BeginScene() override
	{
		if (!m_renderStartedThisFrame) {
			m_renderStartedThisFrame = true;
			if (!m_inputSampledThisFrame) {
				m_inputSampledThisFrame = true;
				m_presenter.Reflex().Mark(RM_INPUT_SAMPLE);
			}
			m_presenter.Reflex().Mark(RM_SIMULATION_END);
			m_presenter.Reflex().Mark(RM_RENDERSUBMIT_START);
		}
		return m_real->BeginScene();
	}
	HRESULT STDMETHODCALLTYPE EndScene() override { return m_real->EndScene(); }
	HRESULT STDMETHODCALLTYPE Clear(DWORD Count, CONST D3DRECT *pRects, DWORD Flags, D3DCOLOR Color, float Z, DWORD Stencil) override { return m_real->Clear(Count, pRects, Flags, Color, Z, Stencil); }
	HRESULT STDMETHODCALLTYPE SetTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX *pMatrix) override { return m_real->SetTransform(State, pMatrix); }
	HRESULT STDMETHODCALLTYPE GetTransform(D3DTRANSFORMSTATETYPE State, D3DMATRIX *pMatrix) override { return m_real->GetTransform(State, pMatrix); }
	HRESULT STDMETHODCALLTYPE MultiplyTransform(D3DTRANSFORMSTATETYPE State, CONST D3DMATRIX *pMatrix) override { return m_real->MultiplyTransform(State, pMatrix); }
	HRESULT STDMETHODCALLTYPE SetViewport(CONST D3DVIEWPORT9 *pViewport) override { return m_real->SetViewport(pViewport); }
	HRESULT STDMETHODCALLTYPE GetViewport(D3DVIEWPORT9 *pViewport) override { return m_real->GetViewport(pViewport); }
	HRESULT STDMETHODCALLTYPE SetMaterial(CONST D3DMATERIAL9 *pMaterial) override { return m_real->SetMaterial(pMaterial); }
	HRESULT STDMETHODCALLTYPE GetMaterial(D3DMATERIAL9 *pMaterial) override { return m_real->GetMaterial(pMaterial); }
	HRESULT STDMETHODCALLTYPE SetLight(DWORD Index, CONST D3DLIGHT9 *pLight) override { return m_real->SetLight(Index, pLight); }
	HRESULT STDMETHODCALLTYPE GetLight(DWORD Index, D3DLIGHT9 *pLight) override { return m_real->GetLight(Index, pLight); }
	HRESULT STDMETHODCALLTYPE LightEnable(DWORD Index, BOOL Enable) override { return m_real->LightEnable(Index, Enable); }
	HRESULT STDMETHODCALLTYPE GetLightEnable(DWORD Index, BOOL *pEnable) override { return m_real->GetLightEnable(Index, pEnable); }
	HRESULT STDMETHODCALLTYPE SetClipPlane(DWORD Index, CONST float *pPlane) override { return m_real->SetClipPlane(Index, pPlane); }
	HRESULT STDMETHODCALLTYPE GetClipPlane(DWORD Index, float *pPlane) override { return m_real->GetClipPlane(Index, pPlane); }
	HRESULT STDMETHODCALLTYPE SetRenderState(D3DRENDERSTATETYPE State, DWORD Value) override { return m_real->SetRenderState(State, Value); }
	HRESULT STDMETHODCALLTYPE GetRenderState(D3DRENDERSTATETYPE State, DWORD *pValue) override { return m_real->GetRenderState(State, pValue); }
	HRESULT STDMETHODCALLTYPE CreateStateBlock(D3DSTATEBLOCKTYPE Type, IDirect3DStateBlock9 **ppSB) override { return m_real->CreateStateBlock(Type, ppSB); }
	HRESULT STDMETHODCALLTYPE BeginStateBlock() override { return m_real->BeginStateBlock(); }
	HRESULT STDMETHODCALLTYPE EndStateBlock(IDirect3DStateBlock9 **ppSB) override { return m_real->EndStateBlock(ppSB); }
	HRESULT STDMETHODCALLTYPE SetClipStatus(CONST D3DCLIPSTATUS9 *pClipStatus) override { return m_real->SetClipStatus(pClipStatus); }
	HRESULT STDMETHODCALLTYPE GetClipStatus(D3DCLIPSTATUS9 *pClipStatus) override { return m_real->GetClipStatus(pClipStatus); }
	HRESULT STDMETHODCALLTYPE GetTexture(DWORD Stage, IDirect3DBaseTexture9 **ppTexture) override { return m_real->GetTexture(Stage, ppTexture); }
	HRESULT STDMETHODCALLTYPE SetTexture(DWORD Stage, IDirect3DBaseTexture9 *pTexture) override
	{
		return m_real->SetTexture(Stage, UnwrapBaseTextureForGpu(pTexture));
	}
	HRESULT STDMETHODCALLTYPE GetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD *pValue) override { return m_real->GetTextureStageState(Stage, Type, pValue); }
	HRESULT STDMETHODCALLTYPE SetTextureStageState(DWORD Stage, D3DTEXTURESTAGESTATETYPE Type, DWORD Value) override { return m_real->SetTextureStageState(Stage, Type, Value); }
	HRESULT STDMETHODCALLTYPE GetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD *pValue) override { return m_real->GetSamplerState(Sampler, Type, pValue); }
	HRESULT STDMETHODCALLTYPE SetSamplerState(DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value) override { return m_real->SetSamplerState(Sampler, Type, Value); }
	HRESULT STDMETHODCALLTYPE ValidateDevice(DWORD *pNumPasses) override { return m_real->ValidateDevice(pNumPasses); }
	HRESULT STDMETHODCALLTYPE SetPaletteEntries(UINT PaletteNumber, CONST PALETTEENTRY *pEntries) override { return m_real->SetPaletteEntries(PaletteNumber, pEntries); }
	HRESULT STDMETHODCALLTYPE GetPaletteEntries(UINT PaletteNumber, PALETTEENTRY *pEntries) override { return m_real->GetPaletteEntries(PaletteNumber, pEntries); }
	HRESULT STDMETHODCALLTYPE SetCurrentTexturePalette(UINT PaletteNumber) override { return m_real->SetCurrentTexturePalette(PaletteNumber); }
	HRESULT STDMETHODCALLTYPE GetCurrentTexturePalette(UINT *PaletteNumber) override { return m_real->GetCurrentTexturePalette(PaletteNumber); }
	HRESULT STDMETHODCALLTYPE SetScissorRect(CONST RECT *pRect) override { return m_real->SetScissorRect(pRect); }
	HRESULT STDMETHODCALLTYPE GetScissorRect(RECT *pRect) override { return m_real->GetScissorRect(pRect); }
	HRESULT STDMETHODCALLTYPE SetSoftwareVertexProcessing(BOOL bSoftware) override { return m_real->SetSoftwareVertexProcessing(bSoftware); }
	BOOL    STDMETHODCALLTYPE GetSoftwareVertexProcessing() override { return m_real->GetSoftwareVertexProcessing(); }
	HRESULT STDMETHODCALLTYPE SetNPatchMode(float nSegments) override { return m_real->SetNPatchMode(nSegments); }
	float   STDMETHODCALLTYPE GetNPatchMode() override { return m_real->GetNPatchMode(); }
	HRESULT STDMETHODCALLTYPE DrawPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT StartVertex, UINT PrimitiveCount) override { return m_real->DrawPrimitive(PrimitiveType, StartVertex, PrimitiveCount); }
	HRESULT STDMETHODCALLTYPE DrawIndexedPrimitive(D3DPRIMITIVETYPE Type, INT BaseVertexIndex, UINT MinVertexIndex, UINT NumVertices, UINT startIndex, UINT primCount) override { return m_real->DrawIndexedPrimitive(Type, BaseVertexIndex, MinVertexIndex, NumVertices, startIndex, primCount); }
	HRESULT STDMETHODCALLTYPE DrawPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT PrimitiveCount, CONST void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override { return m_real->DrawPrimitiveUP(PrimitiveType, PrimitiveCount, pVertexStreamZeroData, VertexStreamZeroStride); }
	HRESULT STDMETHODCALLTYPE DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE PrimitiveType, UINT MinVertexIndex, UINT NumVertices, UINT PrimitiveCount, CONST void *pIndexData, D3DFORMAT IndexDataFormat, CONST void *pVertexStreamZeroData, UINT VertexStreamZeroStride) override { return m_real->DrawIndexedPrimitiveUP(PrimitiveType, MinVertexIndex, NumVertices, PrimitiveCount, pIndexData, IndexDataFormat, pVertexStreamZeroData, VertexStreamZeroStride); }
	HRESULT STDMETHODCALLTYPE ProcessVertices(UINT SrcStartIndex, UINT DestIndex, UINT VertexCount, IDirect3DVertexBuffer9 *pDestBuffer, IDirect3DVertexDeclaration9 *pVertexDecl, DWORD Flags) override { return m_real->ProcessVertices(SrcStartIndex, DestIndex, VertexCount, pDestBuffer, pVertexDecl, Flags); }
	HRESULT STDMETHODCALLTYPE CreateVertexDeclaration(CONST D3DVERTEXELEMENT9 *pVertexElements, IDirect3DVertexDeclaration9 **ppDecl) override { return m_real->CreateVertexDeclaration(pVertexElements, ppDecl); }
	HRESULT STDMETHODCALLTYPE SetVertexDeclaration(IDirect3DVertexDeclaration9 *pDecl) override { return m_real->SetVertexDeclaration(pDecl); }
	HRESULT STDMETHODCALLTYPE GetVertexDeclaration(IDirect3DVertexDeclaration9 **ppDecl) override { return m_real->GetVertexDeclaration(ppDecl); }
	HRESULT STDMETHODCALLTYPE SetFVF(DWORD FVF) override { return m_real->SetFVF(FVF); }
	HRESULT STDMETHODCALLTYPE GetFVF(DWORD *pFVF) override { return m_real->GetFVF(pFVF); }
	HRESULT STDMETHODCALLTYPE CreateVertexShader(CONST DWORD *pFunction, IDirect3DVertexShader9 **ppShader) override { return m_real->CreateVertexShader(pFunction, ppShader); }
	HRESULT STDMETHODCALLTYPE SetVertexShader(IDirect3DVertexShader9 *pShader) override { return m_real->SetVertexShader(pShader); }
	HRESULT STDMETHODCALLTYPE GetVertexShader(IDirect3DVertexShader9 **ppShader) override { return m_real->GetVertexShader(ppShader); }
	HRESULT STDMETHODCALLTYPE SetVertexShaderConstantF(UINT StartRegister, CONST float *pConstantData, UINT Vector4fCount) override { return m_real->SetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
	HRESULT STDMETHODCALLTYPE GetVertexShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) override { return m_real->GetVertexShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
	HRESULT STDMETHODCALLTYPE SetVertexShaderConstantI(UINT StartRegister, CONST int *pConstantData, UINT Vector4iCount) override { return m_real->SetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
	HRESULT STDMETHODCALLTYPE GetVertexShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) override { return m_real->GetVertexShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
	HRESULT STDMETHODCALLTYPE SetVertexShaderConstantB(UINT StartRegister, CONST BOOL *pConstantData, UINT BoolCount) override { return m_real->SetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
	HRESULT STDMETHODCALLTYPE GetVertexShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) override { return m_real->GetVertexShaderConstantB(StartRegister, pConstantData, BoolCount); }
	HRESULT STDMETHODCALLTYPE SetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 *pStreamData, UINT OffsetInBytes, UINT Stride) override { return m_real->SetStreamSource(StreamNumber, pStreamData, OffsetInBytes, Stride); }
	HRESULT STDMETHODCALLTYPE GetStreamSource(UINT StreamNumber, IDirect3DVertexBuffer9 **ppStreamData, UINT *pOffsetInBytes, UINT *pStride) override { return m_real->GetStreamSource(StreamNumber, ppStreamData, pOffsetInBytes, pStride); }
	HRESULT STDMETHODCALLTYPE SetStreamSourceFreq(UINT StreamNumber, UINT Setting) override { return m_real->SetStreamSourceFreq(StreamNumber, Setting); }
	HRESULT STDMETHODCALLTYPE GetStreamSourceFreq(UINT StreamNumber, UINT *pSetting) override { return m_real->GetStreamSourceFreq(StreamNumber, pSetting); }
	HRESULT STDMETHODCALLTYPE SetIndices(IDirect3DIndexBuffer9 *pIndexData) override { return m_real->SetIndices(pIndexData); }
	HRESULT STDMETHODCALLTYPE GetIndices(IDirect3DIndexBuffer9 **ppIndexData) override { return m_real->GetIndices(ppIndexData); }
	HRESULT STDMETHODCALLTYPE CreatePixelShader(CONST DWORD *pFunction, IDirect3DPixelShader9 **ppShader) override { return m_real->CreatePixelShader(pFunction, ppShader); }
	HRESULT STDMETHODCALLTYPE SetPixelShader(IDirect3DPixelShader9 *pShader) override { return m_real->SetPixelShader(pShader); }
	HRESULT STDMETHODCALLTYPE GetPixelShader(IDirect3DPixelShader9 **ppShader) override { return m_real->GetPixelShader(ppShader); }
	HRESULT STDMETHODCALLTYPE SetPixelShaderConstantF(UINT StartRegister, CONST float *pConstantData, UINT Vector4fCount) override { return m_real->SetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
	HRESULT STDMETHODCALLTYPE GetPixelShaderConstantF(UINT StartRegister, float *pConstantData, UINT Vector4fCount) override { return m_real->GetPixelShaderConstantF(StartRegister, pConstantData, Vector4fCount); }
	HRESULT STDMETHODCALLTYPE SetPixelShaderConstantI(UINT StartRegister, CONST int *pConstantData, UINT Vector4iCount) override { return m_real->SetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
	HRESULT STDMETHODCALLTYPE GetPixelShaderConstantI(UINT StartRegister, int *pConstantData, UINT Vector4iCount) override { return m_real->GetPixelShaderConstantI(StartRegister, pConstantData, Vector4iCount); }
	HRESULT STDMETHODCALLTYPE SetPixelShaderConstantB(UINT StartRegister, CONST BOOL *pConstantData, UINT BoolCount) override { return m_real->SetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
	HRESULT STDMETHODCALLTYPE GetPixelShaderConstantB(UINT StartRegister, BOOL *pConstantData, UINT BoolCount) override { return m_real->GetPixelShaderConstantB(StartRegister, pConstantData, BoolCount); }
	HRESULT STDMETHODCALLTYPE DrawRectPatch(UINT Handle, CONST float *pNumSegs, CONST D3DRECTPATCH_INFO *pRectPatchInfo) override { return m_real->DrawRectPatch(Handle, pNumSegs, pRectPatchInfo); }
	HRESULT STDMETHODCALLTYPE DrawTriPatch(UINT Handle, CONST float *pNumSegs, CONST D3DTRIPATCH_INFO *pTriPatchInfo) override { return m_real->DrawTriPatch(Handle, pNumSegs, pTriPatchInfo); }
	HRESULT STDMETHODCALLTYPE DeletePatch(UINT Handle) override { return m_real->DeletePatch(Handle); }
	HRESULT STDMETHODCALLTYPE CreateQuery(D3DQUERYTYPE Type, IDirect3DQuery9 **ppQuery) override { return m_real->CreateQuery(Type, ppQuery); }

private:
	void ReleaseGameTargets()
	{
		m_presenter.UnbindSharedTexture();
		if (m_real) {
			if (m_dummyBackBuffer) m_real->SetRenderTarget(0, m_dummyBackBuffer);
			m_real->SetDepthStencilSurface(nullptr);
		}
		SafeRelease(m_depthSurf);
		SafeRelease(m_backBufferSurf);
		SafeRelease(m_sharedSurf);
		SafeRelease(m_sharedTex);
		m_sharedHandle = nullptr;
	}

	bool CreateGameTargets(UINT w, UINT h)
	{
		ReleaseGameTargets();

		// m_sharedTex / m_sharedSurf acts as the Front Buffer shared with D3D11 (updated only in Present).
		HANDLE sh = nullptr;
		HRESULT hr = m_real->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &m_sharedTex, &sh);
		if (FAILED(hr) || !m_sharedTex || !sh) {
			Log("D3D9Ex: CreateTexture(shared %ux%u) failed (0x%08lX)", w, h, hr);
			return false;
		}
		m_sharedHandle = sh;
		hr = m_sharedTex->GetSurfaceLevel(0, &m_sharedSurf);
		if (FAILED(hr) || !m_sharedSurf) return false;

		// Dedicated Back Buffer (supports both MSAA and non-MSAA while keeping Front Buffer intact for F8).
		hr = m_real->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, m_pp.MultiSampleType, m_pp.MultiSampleQuality, FALSE, &m_backBufferSurf, nullptr);
		if (FAILED(hr) || !m_backBufferSurf) {
			m_pp.MultiSampleType = D3DMULTISAMPLE_NONE;
			m_pp.MultiSampleQuality = 0;
			hr = m_real->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &m_backBufferSurf, nullptr);
			if (FAILED(hr) || !m_backBufferSurf) {
				Log("D3D9Ex: CreateRenderTarget(%ux%u) failed (0x%08lX)", w, h, hr);
				return false;
			}
		}

		D3DFORMAT dsFmt = m_pp.EnableAutoDepthStencil ? m_pp.AutoDepthStencilFormat : D3DFMT_D24S8;
		if (dsFmt == D3DFMT_UNKNOWN) dsFmt = D3DFMT_D24S8;
		hr = m_real->CreateDepthStencilSurface(w, h, dsFmt, m_pp.MultiSampleType, m_pp.MultiSampleQuality, TRUE, &m_depthSurf, nullptr);
		if (FAILED(hr))
			hr = m_real->CreateDepthStencilSurface(w, h, D3DFMT_D24S8, m_pp.MultiSampleType, m_pp.MultiSampleQuality, TRUE, &m_depthSurf, nullptr);
		if (FAILED(hr) || !m_depthSurf) {
			Log("D3D9Ex: CreateDepthStencilSurface(%ux%u) failed (0x%08lX)", w, h, hr);
			return false;
		}

		m_real->SetRenderTarget(0, m_sharedSurf);
		m_real->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
		m_real->SetRenderTarget(0, m_backBufferSurf);
		m_real->SetDepthStencilSurface(m_depthSurf);
		D3DVIEWPORT9 vp = { 0, 0, w, h, 0.0f, 1.0f };
		m_real->SetViewport(&vp);
		m_real->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFF000000, 1.0f, 0);

		return m_presenter.BindSharedTexture(m_sharedHandle, w, h);
	}

	IDirect3D9            *m_proxyD3D;
	IDirect3DDevice9Ex    *m_real;
	HWND                   m_gameHwnd;
	HWND                   m_dummyHwnd;
	D3DPRESENT_PARAMETERS  m_pp;
	UINT                   m_adapter;
	D3DDEVTYPE             m_devType;
	DWORD                  m_behaviorFlags;
	LUID                   m_adapterLuid = {};
	bool                   m_hasLuid = false;
	UINT                   m_refreshHz = 60;
	volatile LONG          m_ref;
	D3DGAMMARAMP           m_gamma = {};

	IDirect3DSurface9     *m_dummyBackBuffer = nullptr;
	IDirect3DStateBlock9  *m_defaultState = nullptr;
	IDirect3DTexture9     *m_sharedTex = nullptr;
	IDirect3DSurface9     *m_sharedSurf = nullptr;
	IDirect3DSurface9     *m_backBufferSurf = nullptr;
	IDirect3DSurface9     *m_depthSurf = nullptr;
	HANDLE                 m_sharedHandle = nullptr;
	IDirect3DQuery9       *m_evQuery = nullptr;

	D3D11Presenter         m_presenter;
	WNDPROC                m_prevWndProc = nullptr;
	POINT                  m_desktopCursor = {};
	bool                   m_hasDesktopCursor = false;
	bool                   m_isUnicodeWnd = false;
	bool                   m_borderless = true;
	bool                   m_deactivating = false;
	bool                   m_renderStartedThisFrame = false;
	bool                   m_inputSampledThisFrame = false;
	int                    m_startupFrames = 0;
	int                    m_errLogs = 0;
};

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
HRESULT STDMETHODCALLTYPE ProxySwapChain9::Present(CONST RECT *, CONST RECT *, HWND, CONST RGNDATA *, DWORD) { return m_dev->PresentInternal(); }
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
		HWND dummyHwnd = CreateHiddenDummyWindow();
		if (!dummyHwnd)
			return FallbackNativeCreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppReturnedDeviceInterface);

		D3DPRESENT_PARAMETERS dummyPP = {};
		dummyPP.Windowed = TRUE;
		dummyPP.SwapEffect = D3DSWAPEFFECT_DISCARD;
		dummyPP.BackBufferFormat = D3DFMT_A8R8G8B8;
		dummyPP.BackBufferWidth = 1;
		dummyPP.BackBufferHeight = 1;
		dummyPP.BackBufferCount = 1;
		dummyPP.hDeviceWindow = dummyHwnd;
		dummyPP.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

		DWORD flags = BehaviorFlags | D3DCREATE_FPU_PRESERVE | D3DCREATE_MULTITHREADED;
		IDirect3DDevice9Ex *realDevEx = nullptr;
		HRESULT hr = m_real->CreateDeviceEx(Adapter, DeviceType, dummyHwnd, flags, &dummyPP, nullptr, &realDevEx);
		if (FAILED(hr) || !realDevEx) {
			Log("D3D9Ex: CreateDeviceEx failed (0x%08lX) - falling back to classic D3D9", hr);
			DestroyWindow(dummyHwnd);
			return FallbackNativeCreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppReturnedDeviceInterface);
		}

		LUID luid = {};
		bool hasLuid = SUCCEEDED(m_real->GetAdapterLUID(Adapter, &luid));
		auto *proxy = new ProxyDirect3DDevice9(this, realDevEx, gameHwnd, dummyHwnd, *pPP, Adapter, DeviceType, BehaviorFlags, luid, hasLuid);
		if (!proxy->InitializeBridge()) {
			Log("Bridge: InitializeBridge failed - falling back to classic D3D9");
			proxy->Release();
			return FallbackNativeCreateDevice(Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPP, ppReturnedDeviceInterface);
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

IDirect3D9 *CreateProxyDirect3D9(IDirect3D9Ex *realEx, Direct3DCreate9_t fallbackCreate9, UINT sdkVersion)
{
	return realEx ? new ProxyDirect3D9(realEx, fallbackCreate9, sdkVersion) : nullptr;
}

void SetRawCursorPosFn(SetCursorPos_t fn)
{
	g_rawSetCursorPos = fn;
}

HWND GetActiveGameWindow()
{
	return g_activeDev ? g_activeDev->GameHwnd() : nullptr;
}

bool IsGameWindowDeactivating()
{
	return g_activeDev ? g_activeDev->IsDeactivating() : false;
}

void NotifyInputSample()
{
	if (g_activeDev) g_activeDev->MarkInputSampled();
}
