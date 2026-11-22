// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include "nvapi_reflex.h"

class D3D11Presenter {
public:
	bool Init(HWND hwnd, UINT swapW, UINT swapH, LUID adapterLuid, bool hasLuid);
	void Shutdown();

	bool BindSharedTexture(HANDLE sharedHandle, UINT gameW, UINT gameH);
	void UnbindSharedTexture();
	bool ResizeSwapChain(UINT swapW, UINT swapH);
	bool PresentFrame();
	void PaceNextFrame();

	NvReflex &Reflex() { return m_reflex; }
	UINT SwapWidth() const { return m_swapW; }
	UINT SwapHeight() const { return m_swapH; }

private:
	bool CreateBlitPipeline();
	bool AcquireBackBuffer();
	void ReleaseBackBuffer();

	HWND                      m_hwnd = nullptr;
	ID3D11Device             *m_dev = nullptr;
	ID3D11DeviceContext      *m_ctx = nullptr;
	IDXGIFactory5            *m_factory = nullptr;
	IDXGISwapChain1          *m_swapChain = nullptr;
	IDXGISwapChain2          *m_swapChain2 = nullptr;
	HANDLE                    m_waitableObj = nullptr;

	ID3D11Texture2D          *m_backBuffer = nullptr;
	ID3D11RenderTargetView   *m_backBufferRtv = nullptr;

	ID3D11Texture2D          *m_sharedTex = nullptr;
	ID3D11ShaderResourceView *m_sharedSrv = nullptr;

	ID3D11VertexShader       *m_blitVS = nullptr;
	ID3D11PixelShader        *m_blitPS = nullptr;
	ID3D11SamplerState       *m_smpLinear = nullptr;
	ID3D11RasterizerState    *m_rsNoCull = nullptr;
	ID3D11BlendState         *m_bsOpaque = nullptr;
	ID3D11DepthStencilState  *m_dsOff = nullptr;

	NvReflex m_reflex;
	UINT     m_swapW = 0;
	UINT     m_swapH = 0;
	UINT     m_gameW = 0;
	UINT     m_gameH = 0;
	bool     m_tearingSupported = false;
	bool     m_hasWaitable = false;
	bool     m_deviceLostLogged = false;
};
