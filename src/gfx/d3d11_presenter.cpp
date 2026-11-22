// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "d3d11_presenter.h"
#include "blit_bc.h"
#include "../core/log.h"

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

bool D3D11Presenter::Init(HWND hwnd, UINT swapW, UINT swapH, LUID adapterLuid, bool hasLuid)
{
	m_hwnd = hwnd;
	m_swapW = swapW ? swapW : 1280;
	m_swapH = swapH ? swapH : 720;

	HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory5), (void **)&m_factory);
	if (FAILED(hr) || !m_factory) {
		Log("DXGI: CreateDXGIFactory1(IDXGIFactory5) failed (0x%08lX)", hr);
		Shutdown();
		return false;
	}

	// Match the exact DXGI adapter used by D3D9Ex so shared handles work on hybrid/multi-GPU laptops.
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
		dxgiDev->SetMaximumFrameLatency(1);
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

	DXGI_SWAP_CHAIN_DESC1 scd = {};
	scd.Width = m_swapW;
	scd.Height = m_swapH;
	scd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	scd.SampleDesc.Count = 1;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.BufferCount = 2;
	scd.Scaling = DXGI_SCALING_STRETCH;
	scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	scd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
	scd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
	            (m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);

	hr = m_factory->CreateSwapChainForHwnd(m_dev, m_hwnd, &scd, nullptr, nullptr, &m_swapChain);
	if (SUCCEEDED(hr) && m_swapChain) {
		m_hasWaitable = true;
	} else {
		scd.Flags = m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u;
		hr = m_factory->CreateSwapChainForHwnd(m_dev, m_hwnd, &scd, nullptr, nullptr, &m_swapChain);
		m_hasWaitable = false;
	}
	if (FAILED(hr) || !m_swapChain) {
		Log("DXGI: CreateSwapChainForHwnd(FLIP_DISCARD) failed (0x%08lX)", hr);
		Shutdown();
		return false;
	}

	if (m_hasWaitable && SUCCEEDED(m_swapChain->QueryInterface(__uuidof(IDXGISwapChain2), (void **)&m_swapChain2)) && m_swapChain2) {
		m_swapChain2->SetMaximumFrameLatency(1);
		m_waitableObj = m_swapChain2->GetFrameLatencyWaitableObject();
	}

	if (!AcquireBackBuffer() || !CreateBlitPipeline()) {
		Shutdown();
		return false;
	}

	Log("D3D11: Presenter ready on %s (vendor 0x%04X, FL 0x%X, %ux%u, FLIP_DISCARD, tearing=%d, waitable=%d)",
		gpuName.c_str(), vendorId, (unsigned)actualFl, m_swapW, m_swapH,
		m_tearingSupported ? 1 : 0, m_hasWaitable ? 1 : 0);

	m_reflex.Init(m_dev);
	return true;
}

void D3D11Presenter::Shutdown()
{
	m_reflex.Shutdown();
	if (m_waitableObj) { CloseHandle(m_waitableObj); m_waitableObj = nullptr; }
	SafeRelease(m_sharedSrv);
	SafeRelease(m_sharedTex);
	ReleaseBackBuffer();
	SafeRelease(m_dsOff);
	SafeRelease(m_bsOpaque);
	SafeRelease(m_rsNoCull);
	SafeRelease(m_smpLinear);
	SafeRelease(m_blitPS);
	SafeRelease(m_blitVS);
	SafeRelease(m_swapChain2);
	SafeRelease(m_swapChain);
	SafeRelease(m_factory);
	SafeRelease(m_ctx);
	SafeRelease(m_dev);
}

bool D3D11Presenter::CreateBlitPipeline()
{
	if (FAILED(m_dev->CreateVertexShader(kBlitVS, sizeof(kBlitVS), nullptr, &m_blitVS))) return false;
	if (FAILED(m_dev->CreatePixelShader(kBlitPS, sizeof(kBlitPS), nullptr, &m_blitPS))) return false;

	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(m_dev->CreateSamplerState(&sd, &m_smpLinear))) return false;

	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = FALSE;
	if (FAILED(m_dev->CreateRasterizerState(&rd, &m_rsNoCull))) return false;

	D3D11_BLEND_DESC bd = {};
	bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (FAILED(m_dev->CreateBlendState(&bd, &m_bsOpaque))) return false;

	D3D11_DEPTH_STENCIL_DESC dsd = {};
	dsd.DepthEnable = FALSE;
	if (FAILED(m_dev->CreateDepthStencilState(&dsd, &m_dsOff))) return false;

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

void D3D11Presenter::UnbindSharedTexture()
{
	if (m_ctx) {
		ID3D11ShaderResourceView *nullSrv = nullptr;
		m_ctx->PSSetShaderResources(0, 1, &nullSrv);
		m_ctx->Flush();
	}
	SafeRelease(m_sharedSrv);
	SafeRelease(m_sharedTex);
}

bool D3D11Presenter::BindSharedTexture(HANDLE sharedHandle, UINT gameW, UINT gameH)
{
	UnbindSharedTexture();
	m_gameW = gameW;
	m_gameH = gameH;
	if (!m_dev || !sharedHandle) return false;

	HRESULT hr = m_dev->OpenSharedResource(sharedHandle, __uuidof(ID3D11Texture2D), (void **)&m_sharedTex);
	if (FAILED(hr) || !m_sharedTex) {
		Log("D3D11: OpenSharedResource failed (0x%08lX)", hr);
		return false;
	}
	hr = m_dev->CreateShaderResourceView(m_sharedTex, nullptr, &m_sharedSrv);
	if (FAILED(hr) || !m_sharedSrv) {
		Log("D3D11: CreateShaderResourceView failed (0x%08lX)", hr);
		return false;
	}
	return true;
}

bool D3D11Presenter::ResizeSwapChain(UINT swapW, UINT swapH)
{
	if (!m_swapChain || swapW == 0 || swapH == 0) return false;
	if (swapW == m_swapW && swapH == m_swapH) return true;

	ReleaseBackBuffer();
	UINT flags = (m_hasWaitable ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0u) |
	             (m_tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
	HRESULT hr = m_swapChain->ResizeBuffers(2, swapW, swapH, DXGI_FORMAT_B8G8R8A8_UNORM, flags);
	if (FAILED(hr)) {
		Log("DXGI: ResizeBuffers(%ux%u) failed (0x%08lX)", swapW, swapH, hr);
		return false;
	}
	m_swapW = swapW;
	m_swapH = swapH;
	return AcquireBackBuffer();
}

bool D3D11Presenter::PresentFrame()
{
	if (!m_swapChain || !m_backBuffer || !m_sharedTex) return false;

	if (IsIconic(m_hwnd)) {
		Sleep(16);
		return false;
	}

	if (m_gameW == m_swapW && m_gameH == m_swapH) {
		m_ctx->CopyResource(m_backBuffer, m_sharedTex);
	} else {
		D3D11_VIEWPORT vp = { 0.0f, 0.0f, (float)m_swapW, (float)m_swapH, 0.0f, 1.0f };
		m_ctx->OMSetRenderTargets(1, &m_backBufferRtv, nullptr);
		m_ctx->RSSetViewports(1, &vp);
		m_ctx->RSSetState(m_rsNoCull);
		m_ctx->OMSetBlendState(m_bsOpaque, nullptr, 0xFFFFFFFF);
		m_ctx->OMSetDepthStencilState(m_dsOff, 0);
		m_ctx->IASetInputLayout(nullptr);
		m_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		m_ctx->VSSetShader(m_blitVS, nullptr, 0);
		m_ctx->PSSetShader(m_blitPS, nullptr, 0);
		m_ctx->PSSetSamplers(0, 1, &m_smpLinear);
		m_ctx->PSSetShaderResources(0, 1, &m_sharedSrv);
		m_ctx->Draw(3, 0);

		ID3D11ShaderResourceView *nullSrv = nullptr;
		m_ctx->PSSetShaderResources(0, 1, &nullSrv);
	}

	m_ctx->Flush();
	m_reflex.Mark(RM_RENDERSUBMIT_END);

	UINT flags = m_tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0u;
	m_reflex.Mark(RM_PRESENT_START);
	HRESULT hr = m_swapChain->Present(0, flags);
	m_reflex.Mark(RM_PRESENT_END);

	if ((hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) && !m_deviceLostLogged) {
		m_deviceLostLogged = true;
		Log("DXGI: Present device removed (hr=0x%08lX, reason=0x%08lX)", hr, m_dev->GetDeviceRemovedReason());
	}

	m_reflex.VerifyOnce();
	return true;
}

void D3D11Presenter::PaceNextFrame()
{
	if (m_reflex.Available()) {
		m_reflex.Sleep();
	} else if (m_waitableObj) {
		WaitForSingleObjectEx(m_waitableObj, 1000, TRUE);
	}
	m_reflex.BeginFrame();
}
