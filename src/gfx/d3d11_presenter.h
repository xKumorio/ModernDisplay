// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#pragma once

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <stdint.h>
#include <string>

#include "nvapi_reflex.h"
#include "road_surface.h"
#include "../core/config.h"

void ComputeDisplayViewport(UINT gameW, UINT gameH, UINT swapW, UINT swapH, ScalingMode mode,
                            float &vpX, float &vpY, float &vpW, float &vpH,
                            bool *usePointOut = nullptr, bool *needBarsOut = nullptr);

struct FrameCameraParams {
	float nearZ         = 0.9f;
	float farZ          = 1000.0f;
	float projScaleX    = 1.428f; // 1 / tan(fovX / 2)
	float projScaleY    = 2.539f; // 1 / tan(fovY / 2)
	float projSignX     = 1.0f;   // Sign of D3DTS_PROJECTION._11 (-1 in RenderWare/GTA SA)
	float viewMat[16]   = {};     // Row-major D3D9 View matrix
	float viewProj[16]  = {};     // Row-major D3D9 View * Projection matrix
	bool  hasViewProj   = false;
	// Live GTA:SA world atmosphere & weather (read safely in OnBefore2dStuff)
	float rain          = 0.0f;   // CWeather::Rain (0..1)
	float wetRoads      = 0.0f;   // CWeather::WetRoads (0..1)
	float weatherFog    = 0.0f;   // CWeather::Foggyness (0..1)
	float fogStart      = 10.0f;  // CTimeCycle::m_CurrentColours.m_fFogStart
	float skyTop[3]     = { 0.45f, 0.65f, 0.90f };
	float skyBot[3]     = { 0.72f, 0.84f, 0.95f };
	float sunDirWorld[3]= { 0.3f, 0.5f, 0.8f };
	float camPitchZ     = 0.0f;   // Camera forward world Z (-1=down, 0=horizon, +1=up)
	bool  inInterior    = false;  // CGame::currArea != 0
	bool  sheltered     = false;
	bool  hasRoadGrid   = false;
	float actorCrop[4] = {}; // screen UV origin and extent of reflected player
	float actorGround[4] = {}; // player XYZ and reflecting plane Z
};

class D3D11Presenter {
public:
	static constexpr int kNumSlots = 2;

	bool Init(HWND hwnd, UINT swapW, UINT swapH, LUID adapterLuid, bool hasLuid);
	void Shutdown();

	bool BindSharedTextures(const HANDLE colorHandles[kNumSlots],
	                        const HANDLE world3dHandles[kNumSlots],
	                        const HANDLE depthHandles[kNumSlots], const HANDLE actorHandles[kNumSlots],
	                        UINT gameW, UINT gameH);
	void UnbindSharedTextures();
	bool ResizeSwapChain(UINT swapW, UINT swapH);

	// Thread-safe ping-pong submission (async when g_cfg.asyncPresent is enabled)
	void WaitForSlot(int slot);
	bool CanShareRenderTargets() const;
	RoadSurfaceGrid &RoadGridForSlot(int slot) { return m_roadGrids[slot]; }
	void SubmitFrame(int slot, uint64_t frameId, bool async, bool isMinimized = false,
	                 int64_t inputSampleQpc = 0, bool has3dMask = false, bool hasDepth = false,
	                 const FrameCameraParams *cam = nullptr, bool frontEnd = false);
	void DrainAsync();

	void SetGammaRamp(const D3DGAMMARAMP &ramp);
	DisplayTelemetry SnapshotTelemetry(bool realBB, UINT refreshHz) const;

	NvReflex &Reflex() { return m_reflex; }
	UINT SwapWidth() const { return m_swapW; }
	UINT SwapHeight() const { return m_swapH; }

private:
	struct QueuedFrame {
		int               slot = -1;
		uint64_t          frameId = 0;
		int64_t           inputQpc = 0;
		bool              has3dMask = false;
		bool              hasDepth = false;
		bool              frontEnd = false;
		FrameCameraParams cam = {};
	};

	static DWORD WINAPI PresentThreadProc(LPVOID param);
	void ThreadLoop();
	void PresentSlotOnD3D11(const QueuedFrame &qf);

	bool CreatePostPipeline();
	bool CreateAoPipeline();
	bool EnsureDepthPyramid(UINT w, UINT h);
	void ReleaseDepthPyramid();
	bool EnsureAoTargets(UINT w, UINT h);
	void ReleaseAoTargets();
	bool BuildDepthPyramid(int slot, const FrameCameraParams &cam);
	void RenderXeGtao(int slot, int quality, float strength, const float reProj[16], bool useHist, const FrameCameraParams &cam);
	bool CreateSsrPipeline();
	bool EnsureSsrTargets(UINT w, UINT h);
	bool EnsurePuddleTarget();
	void ReleaseSsrTargets();
	ID3D11ShaderResourceView *RenderSsr(int slot, int quality, bool use3dTex, bool rainFx, const FrameCameraParams &cam);
	bool CreateSsgiPipeline();
	bool EnsureSsgiTargets(UINT w, UINT h);
	void ReleaseSsgiTargets();
	void RenderSsgi(int slot, bool use3dTex, const float reProj[16], bool useHist, const FrameCameraParams &cam);
	bool CreateBloomPipeline();
	bool EnsureBloomTargets(UINT w, UINT h);
	void ReleaseBloomTargets();
	void RenderBloom(int slot, bool use3dTex, bool flareOn);
	bool CreateTaaPipeline();
	bool EnsureTaaTargets(UINT w, UINT h);
	void ReleaseTaaTargets();
	void RenderTaa(int slot, bool use3dTex, bool hasSmaa, const float reProj[16], bool useHist, const FrameCameraParams &cam);
	bool CreateSmaaPipeline();
	bool EnsureSmaaTargets(UINT w, UINT h);
	void ReleaseSmaaTargets();
	void RenderSmaa(int slot, int quality, bool use3dTex, bool hasDepth, ID3D11ShaderResourceView *aoSrv, const FrameCameraParams &cam);
	void WarmupPipelines();
	void UpdateLutIfChanged();
	void UpdateSwapFormatIfChanged();
	bool CreateHudPipeline();
	bool AcquireBackBuffer();
	void ReleaseBackBuffer();
	bool UploadGammaIfDirty();
	void RenderHudOverlay(bool showTelemetry, float compareSplitPx);

	HWND                      m_hwnd = nullptr;
	ID3D11Device             *m_dev = nullptr;
	ID3D11DeviceContext      *m_ctx = nullptr;
	IDXGIFactory5            *m_factory = nullptr;
	IDXGISwapChain1          *m_swapChain = nullptr;
	IDXGISwapChain2          *m_swapChain2 = nullptr;
	HANDLE                    m_waitableObj = nullptr;
	DXGI_FORMAT               m_swapFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

	ID3D11Texture2D          *m_backBuffer = nullptr;
	ID3D11RenderTargetView   *m_backBufferRtv = nullptr;

	ID3D11Texture2D          *m_sharedTex[kNumSlots] = {};
	ID3D11ShaderResourceView *m_sharedSrv[kNumSlots] = {};
	ID3D11Texture2D          *m_shared3dTex[kNumSlots] = {};
	ID3D11ShaderResourceView *m_shared3dSrv[kNumSlots] = {};
	ID3D11Texture2D          *m_actorTex[kNumSlots] = {};
	ID3D11ShaderResourceView *m_actorSrv[kNumSlots] = {};
	ID3D11Texture2D          *m_sharedDepthTex[kNumSlots] = {};
	ID3D11ShaderResourceView *m_sharedDepthSrv[kNumSlots] = {};

	// Post-processing pipeline (Catmull-Rom SSAA + XeGTAO + 3D-Aware CAS + 3D LUT + Deband + Vibrance + Gamma LUT)
	ID3D11VertexShader       *m_blitVS = nullptr;
	ID3D11PixelShader        *m_postPS = nullptr;
	ID3D11Buffer             *m_postCB = nullptr;
	ID3D11SamplerState       *m_smpLinear = nullptr;
	ID3D11SamplerState       *m_smpPoint = nullptr;
	ID3D11SamplerState       *m_smpPointMipLin = nullptr;
	ID3D11RasterizerState    *m_rsNoCull = nullptr;
	ID3D11BlendState         *m_bsOpaque = nullptr;
	ID3D11BlendState         *m_bsAlpha = nullptr;
	ID3D11DepthStencilState  *m_dsOff = nullptr;
	ID3D11Texture1D          *m_gammaTex = nullptr;
	ID3D11ShaderResourceView *m_gammaSrv = nullptr;

	// Native D3D11 XeGTAO pipeline (5-Mip Prefiltered Linear Depth Pyramid + Horizon AO + Plane-Aware Denoiser)
	static constexpr UINT     kAoDepthMips = 5;
	ID3D11PixelShader        *m_aoDepthLinPS = nullptr;
	ID3D11PixelShader        *m_aoDepthMipPS = nullptr;
	ID3D11PixelShader        *m_aoPS = nullptr;
	ID3D11PixelShader        *m_aoReusePS = nullptr;
	ID3D11DepthStencilState  *m_markReused = nullptr;
	ID3D11DepthStencilState  *m_traceMissing = nullptr;
	ID3D11DepthStencilView   *m_aoStencil = nullptr;
	ID3D11Texture2D          *m_prevRawTex = nullptr;
	ID3D11RenderTargetView   *m_prevRawRtv = nullptr;
	ID3D11ShaderResourceView *m_prevRawSrv = nullptr;
	int                       m_prevRawQuality = 0;
	float                     m_prevRawStrength = 0.0f;
	ID3D11PixelShader        *m_aoDenoise2dPS = nullptr;
	ID3D11PixelShader        *m_aoBlurPS = nullptr;
	ID3D11PixelShader        *m_aoTemporalPS = nullptr;
	ID3D11Buffer             *m_aoDepthCB = nullptr;
	ID3D11Buffer             *m_aoCB = nullptr;
	ID3D11Buffer             *m_aoBlurCB = nullptr;
	ID3D11Buffer             *m_aoTemporalCB = nullptr;
	ID3D11Texture2D          *m_aoDepthTex = nullptr;
	ID3D11ShaderResourceView *m_aoDepthSrv = nullptr;
	ID3D11RenderTargetView   *m_aoDepthMipRtv[kAoDepthMips] = {};
	ID3D11ShaderResourceView *m_aoDepthMipSrv[kAoDepthMips] = {};
	ID3D11Texture2D          *m_aoPrevDepthTex = nullptr;
	ID3D11ShaderResourceView *m_aoPrevDepthSrv = nullptr;
	UINT                      m_aoDepthW = 0;
	UINT                      m_aoDepthH = 0;
	ID3D11Texture2D          *m_aoRawTex = nullptr;
	ID3D11RenderTargetView   *m_aoRawRtv = nullptr;
	ID3D11ShaderResourceView *m_aoRawSrv = nullptr;
	ID3D11Texture2D          *m_aoTmpTex = nullptr;
	ID3D11RenderTargetView   *m_aoTmpRtv = nullptr;
	ID3D11ShaderResourceView *m_aoTmpSrv = nullptr;
	ID3D11Texture2D          *m_aoFinalTex = nullptr;
	ID3D11RenderTargetView   *m_aoFinalRtv = nullptr;
	ID3D11ShaderResourceView *m_aoFinalSrv = nullptr;
	ID3D11Texture2D          *m_aoHistTex[2] = {};
	ID3D11RenderTargetView   *m_aoHistRtv[2] = {};
	ID3D11ShaderResourceView *m_aoHistSrv[2] = {};
	ID3D11ShaderResourceView *m_aoOutputSrv = nullptr; // Points to latest temporal (or spatial) AO SRV
	double                    m_prevViewProj[16] = {};
	float                     m_prevNearZ = 0.3f;
	float                     m_prevFarZ = 1500.0f;
	bool                      m_hasPrevViewProj = false;
	int                       m_aoHistIdx = 0;
	uint32_t                  m_aoFrameIdx = 0;
	UINT                      m_aoW = 0;
	UINT                      m_aoH = 0;

	// 3D-Only SMAA 1x pipeline (Color + Depth + AO Edge Detection -> Analytical Span Area Weights)
	ID3D11PixelShader        *m_smaaEdgePS = nullptr;
	ID3D11PixelShader        *m_smaaWeightPS = nullptr;
	ID3D11Buffer             *m_smaaCB = nullptr;
	ID3D11Texture2D          *m_smaaEdgeTex = nullptr;
	ID3D11RenderTargetView   *m_smaaEdgeRtv = nullptr;
	ID3D11ShaderResourceView *m_smaaEdgeSrv = nullptr;
	ID3D11Texture2D          *m_smaaWeightTex = nullptr;
	ID3D11RenderTargetView   *m_smaaWeightRtv = nullptr;
	ID3D11ShaderResourceView *m_smaaWeightSrv = nullptr;
	UINT                      m_smaaW = 0;
	UINT                      m_smaaH = 0;

	// Screen-Space Reflections (SSR) — half-res water & rain-wet puddles + bilateral roughness blur
	ID3D11PixelShader        *m_ssrPS = nullptr;
	ID3D11PixelShader        *m_roadPuddlesPS = nullptr;
	ID3D11Texture2D          *m_roadGridTexture = nullptr;
	ID3D11ShaderResourceView *m_roadGridSrv = nullptr;
	RoadSurfaceGrid          m_roadGrids[kNumSlots];
	unsigned                 m_uploadedRoadGridRev = 0;
	ID3D11PixelShader        *m_ssrBlurPS = nullptr;
	ID3D11Buffer             *m_ssrCB = nullptr;
	ID3D11Texture2D          *m_ssrTex = nullptr;
	ID3D11RenderTargetView   *m_ssrRtv = nullptr;
	ID3D11ShaderResourceView *m_ssrSrv = nullptr;
	ID3D11Texture2D          *m_ssrTmpTex = nullptr;
	ID3D11RenderTargetView   *m_ssrTmpRtv = nullptr;
	ID3D11ShaderResourceView *m_ssrTmpSrv = nullptr;
	UINT                      m_ssrW = 0;
	UINT                      m_ssrH = 0;
	ID3D11RenderTargetView   *m_puddleRtv = nullptr;
	ID3D11ShaderResourceView *m_puddleSrv = nullptr;
	UINT                      m_puddleW = 0, m_puddleH = 0;

	// Temporal Screen-Space Global Illumination (SSGI — half-res 3D color bounce + bilateral blur + temporal reprojection)
	ID3D11PixelShader        *m_ssgiPS = nullptr;
	ID3D11PixelShader        *m_ssgiBlurPS = nullptr;
	ID3D11PixelShader        *m_ssgiTemporalPS = nullptr;
	ID3D11Texture2D          *m_ssgiRawTex = nullptr;
	ID3D11RenderTargetView   *m_ssgiRawRtv = nullptr;
	ID3D11ShaderResourceView *m_ssgiRawSrv = nullptr;
	ID3D11Texture2D          *m_ssgiTmpTex = nullptr;
	ID3D11RenderTargetView   *m_ssgiTmpRtv = nullptr;
	ID3D11ShaderResourceView *m_ssgiTmpSrv = nullptr;
	ID3D11Texture2D          *m_ssgiHistTex[2] = {};
	ID3D11RenderTargetView   *m_ssgiHistRtv[2] = {};
	ID3D11ShaderResourceView *m_ssgiHistSrv[2] = {};
	ID3D11ShaderResourceView *m_ssgiOutputSrv = nullptr;
	int                       m_ssgiHistIdx = 0;
	bool                      m_ssgiHasHist = false;
	uint32_t                  m_ssgiFrameIdx = 0;
	UINT                      m_ssgiW = 0;
	UINT                      m_ssgiH = 0;

	// 5-Level 3D-Only Dual-Kawase HDR Bloom + Anamorphic Flares (R11G11B10_FLOAT)
	static constexpr UINT     kBloomMips = 5;
	ID3D11PixelShader        *m_bloomPrePS = nullptr;
	ID3D11PixelShader        *m_bloomDownPS = nullptr;
	ID3D11PixelShader        *m_bloomUpPS = nullptr;
	ID3D11Buffer             *m_bloomCB = nullptr;
	ID3D11Texture2D          *m_bloomDownTex[kBloomMips] = {};
	ID3D11RenderTargetView   *m_bloomDownRtv[kBloomMips] = {};
	ID3D11ShaderResourceView *m_bloomDownSrv[kBloomMips] = {};
	ID3D11Texture2D          *m_bloomUpTex[kBloomMips - 1] = {};
	ID3D11RenderTargetView   *m_bloomUpRtv[kBloomMips - 1] = {};
	ID3D11ShaderResourceView *m_bloomUpSrv[kBloomMips - 1] = {};
	UINT                      m_bloomW = 0;
	UINT                      m_bloomH = 0;

	// 3D-Only SMAA T2x (Temporal Anti-Aliasing with Catmull-Rom + YCoCg Variance Clip)
	ID3D11PixelShader        *m_taaPS = nullptr;
	ID3D11Buffer             *m_taaCB = nullptr;
	ID3D11Texture2D          *m_taaHistTex[2] = {};
	ID3D11RenderTargetView   *m_taaHistRtv[2] = {};
	ID3D11ShaderResourceView *m_taaHistSrv[2] = {};
	ID3D11ShaderResourceView *m_taaOutputSrv = nullptr;
	int                       m_taaHistIdx = 0;
	bool                      m_taaHasHist = false;
	UINT                      m_taaW = 0;
	UINT                      m_taaH = 0;

	// 3D .cube LUT color grading
	ID3D11Texture3D          *m_lutTex = nullptr;
	ID3D11ShaderResourceView *m_lutSrv = nullptr;
	float                     m_lutSize = 0.0f;
	std::string               m_loadedLutName;

	// Minimal bare-typography telemetry overlay (zero box/card chrome)
	ID3D11VertexShader       *m_hudVS = nullptr;
	ID3D11PixelShader        *m_hudPS = nullptr;
	ID3D11InputLayout        *m_hudLayout = nullptr;
	ID3D11Buffer             *m_hudCB = nullptr;
	ID3D11Buffer             *m_hudVB = nullptr;
	ID3D11Texture2D          *m_fontTex = nullptr;
	ID3D11ShaderResourceView *m_fontSrv = nullptr;
	char                      m_hudLine[96] = {};
	DWORD                     m_lastHudUpdateTick = 0;

	// Gamma state (protected by m_gammaCs)
	CRITICAL_SECTION          m_gammaCs = {};
	bool                      m_gammaCsInit = false;
	D3DGAMMARAMP              m_gammaRamp = {};
	bool                      m_gammaDirty = true;
	bool                      m_gammaIsIdentity = true;

	// Async presenter thread synchronization
	CRITICAL_SECTION          m_asyncCs = {};
	CONDITION_VARIABLE        m_cvWork = {};
	CONDITION_VARIABLE        m_cvDone = {};
	bool                      m_asyncCsInit = false;
	HANDLE                    m_thread = nullptr;
	bool                      m_stopThread = false;
	bool                      m_slotBusy[kNumSlots] = {};
	ID3D11Query              *m_slotQuery[kNumSlots] = {};
	IDXGIDevice2             *m_queueDevice = nullptr;
	HANDLE                    m_slotReady[kNumSlots] = {};
	QueuedFrame               m_queued = {};
	bool                      m_inFlight = false;

	// Timing & telemetry
	NvReflex                  m_reflex;
	LARGE_INTEGER             m_qpcFreq = {};
	LARGE_INTEGER             m_lastPresentQpc = {};
	volatile LONG             m_emaFrameMsBits = 0;
	LONG                      m_appliedCfgSeq = 0;
	UINT                      m_swapW = 0;
	UINT                      m_swapH = 0;
	UINT                      m_gameW = 0;
	UINT                      m_gameH = 0;
	bool                      m_tearingSupported = false;
	bool                      m_hasWaitable = false;
	bool                      m_deviceLostLogged = false;
};
