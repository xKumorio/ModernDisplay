// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#pragma once

#include <windows.h>
#include <stdint.h>
#include <d3d11.h>
#include <dxgi1_6.h>

#include "../core/config.h"

enum ReflexMarker : int32_t {
	RM_SIMULATION_START   = 0,
	RM_SIMULATION_END     = 1,
	RM_RENDERSUBMIT_START = 2,
	RM_RENDERSUBMIT_END   = 3,
	RM_PRESENT_START      = 4,
	RM_PRESENT_END        = 5,
	RM_INPUT_SAMPLE       = 6,
};

enum class LowLatencyBackend : int {
	None          = 0,
	NvidiaReflex  = 1, // Native 32-bit NVAPI Reflex (NVIDIA GPUs)
	UniversalDx11 = 2, // Universal DX11 Low-Latency (AMD Radeon, Intel Arc/UHD/Xe, fallback)
};

struct ReflexStats {
	float pcLatMs   = 0.0f;
	float gpuMs     = 0.0f;
	float osQueueMs = 0.0f;
	bool  lowLat    = false;
	bool  iFlip     = false;
	bool  vrr       = false;
	bool  valid     = false;
};

class NvReflex {
public:
	static constexpr int kUllSlots = 4;

	bool Init(ID3D11Device *dev11, ID3D11DeviceContext *ctx11, IDXGISwapChain2 *swapChain2,
	          HANDLE waitableObj, UINT vendorId, ReflexMode initialMode, bool forceUniversal = false);
	void ApplyMode(ReflexMode mode);
	void Disable();
	void Shutdown();

	uint64_t NextFrameId();
	void     MarkFrame(uint64_t frameId, ReflexMarker type);

	// Called on the main game thread right before GTA SA samples keyboard/mouse (CGame::Process).
	// On AMD/Intel (UniversalDx11), waits for the DXGI frame latency waitable object BEFORE input
	// sampling so the CPU never queues frames ahead of the GPU (cutting GPU-bound input lag by 40-60%).
	int64_t  PreInputSync();
	int64_t  CurrentInputSampleQpc() const;

	// Called strictly on the D3D11 present thread around GPU submission & Present().
	void     BeginGpuWork(uint64_t frameId, int64_t inputSampleQpc);
	void     EndGpuWork();
	void     OnPresentFinished();

	void        PollTelemetry();
	ReflexStats GetStats() const;

	LowLatencyBackend Backend() const { return m_backend; }
	const char       *BackendName() const;
	const char       *ShortHudTag() const;
	bool              Available() const { return m_available; }
	bool              Active() const { return m_available && m_mode != ReflexMode::Off; }
	bool              IsNvidiaReflex() const { return m_backend == LowLatencyBackend::NvidiaReflex && m_available; }
	bool              IsUniversalDx11() const { return m_backend == LowLatencyBackend::UniversalDx11 && m_available; }

private:
	struct UllQuerySlot {
		ID3D11Query *disjoint = nullptr;
		ID3D11Query *tsStart  = nullptr;
		ID3D11Query *tsEnd    = nullptr;
		ID3D11Query *doneEv   = nullptr;
		uint64_t     frameId  = 0;
		int64_t      inputQpc = 0;
		int64_t      presQpc  = 0;
		bool         inFlight = false;
	};

	bool InitNvidiaReflex(ReflexMode initialMode);
	bool InitUniversalDx11(ReflexMode initialMode);
	void ApplyDxgiMaxLatency(UINT maxLat);
	void ReleaseUllQueries();
	void PollUllTelemetry();

	HMODULE              m_nvapi = nullptr;
	ID3D11Device        *m_dev = nullptr;
	ID3D11DeviceContext *m_ctx = nullptr;
	IDXGISwapChain2     *m_swapChain2 = nullptr;
	HANDLE               m_waitableObj = nullptr;
	HANDLE               m_gpuDoneEvent = nullptr;
	UINT                 m_vendorId = 0;
	LowLatencyBackend    m_backend = LowLatencyBackend::None;
	ReflexMode           m_mode = ReflexMode::Boost;
	bool                 m_available = false;
	bool                 m_initialized = false;
	bool                 m_verifiedLogged = false;
	volatile LONG64      m_frameId = 0;
	volatile LONG64      m_curInputQpc = 0;
	volatile LONG        m_presentSeq = 0;
	LONG                 m_lastWaitedSeq = 0;
	LARGE_INTEGER        m_qpcFreq = {};
	UllQuerySlot         m_ull[kUllSlots] = {};
	int                  m_ullWrite = 0;
	int                  m_ullActiveSlot = -1;
	bool                 m_ullQueriesReady = false;
	float                m_emaPcLat = 0.0f;
	float                m_emaGpu = 0.0f;
	float                m_emaOsQ = 0.0f;
	DWORD                m_lastPollTick = 0;
	mutable SRWLOCK      m_statsLock = SRWLOCK_INIT;
	ReflexStats          m_stats = {};
};
