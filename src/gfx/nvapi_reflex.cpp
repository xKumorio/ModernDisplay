// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "nvapi_reflex.h"
#include "../core/log.h"

namespace {

typedef uint32_t NvU32;
typedef uint64_t NvU64;
typedef uint8_t  NvU8;
typedef uint8_t  NvBool;
typedef int32_t  NvAPI_Status;

#define MAKE_NVAPI_VER(typeName, ver) (NvU32)(sizeof(typeName) | ((ver) << 16))

struct NV_GET_SLEEP_STATUS_PARAMS_V1 {
	NvU32  version;
	NvBool bLowLatencyMode;
	NvBool bFsVrr;
	NvBool bCplVsyncOn;
	NvU32  sleepIntervalUs;
	NvBool bUseGameSleep;
	NvBool bFullscreenIFlip;
	NvU8   fgMultiplier;
	NvBool bDfgControl;
	NvU32  dfgFrameTimeTargetUs;
	NvU8   rsvd[114];
};

struct NV_SET_SLEEP_MODE_PARAMS_V1 {
	NvU32  version;
	NvBool bLowLatencyMode;
	NvBool bLowLatencyBoost;
	NvU32  minimumIntervalUs;
	NvBool bUseMarkersToOptimize;
	NvBool bUseMinQueueTime;
	NvU8   rsvd[30];
};

struct NV_LATENCY_MARKER_PARAMS_V1 {
	NvU32   version;
	NvU64   frameID;
	int32_t markerType;
	NvU64   rsvd0;
	NvU8    rsvd[56];
};

struct NV_LATENCY_RESULT_PARAMS_V1 {
	NvU32 version;
	struct FrameReport {
		NvU64 frameID;
		NvU64 inputSampleTime;
		NvU64 simStartTime;
		NvU64 simEndTime;
		NvU64 renderSubmitStartTime;
		NvU64 renderSubmitEndTime;
		NvU64 presentStartTime;
		NvU64 presentEndTime;
		NvU64 driverStartTime;
		NvU64 driverEndTime;
		NvU64 osRenderQueueStartTime;
		NvU64 osRenderQueueEndTime;
		NvU64 gpuRenderStartTime;
		NvU64 gpuRenderEndTime;
		NvU32 gpuActiveRenderTimeUs;
		NvU32 gpuFrameTimeUs;
		NvU64 cameraConstructedTime;
		NvU32 crossAdapterCopyTimeUs;
		NvU32 aiFrameTimeUs;
		NvU8  rsvd[104];
	} frameReport[64];
	NvU8 rsvd[32];
};

typedef void *(__cdecl *NvAPI_QueryInterface_t)(NvU32 id);
typedef NvAPI_Status(__cdecl *NvAPI_Initialize_t)();
typedef NvAPI_Status(__cdecl *NvAPI_Unload_t)();
typedef NvAPI_Status(__cdecl *NvAPI_D3D_SetSleepMode_t)(IUnknown *, NV_SET_SLEEP_MODE_PARAMS_V1 *);
typedef NvAPI_Status(__cdecl *NvAPI_D3D_GetSleepStatus_t)(IUnknown *, NV_GET_SLEEP_STATUS_PARAMS_V1 *);
typedef NvAPI_Status(__cdecl *NvAPI_D3D_SetLatencyMarker_t)(IUnknown *, NV_LATENCY_MARKER_PARAMS_V1 *);
typedef NvAPI_Status(__cdecl *NvAPI_D3D_GetLatency_t)(IUnknown *, NV_LATENCY_RESULT_PARAMS_V1 *);

NvAPI_Initialize_t           g_fnInit   = nullptr;
NvAPI_Unload_t               g_fnUnload = nullptr;
NvAPI_D3D_SetSleepMode_t     g_fnSetSM  = nullptr;
NvAPI_D3D_GetSleepStatus_t   g_fnGetSS  = nullptr;
NvAPI_D3D_SetLatencyMarker_t g_fnMark   = nullptr;
NvAPI_D3D_GetLatency_t       g_fnGetLat = nullptr;

static inline const char *VendorLabel(UINT vendorId)
{
	switch (vendorId) {
	case 0x10DE: return "NVIDIA";
	case 0x1002: return "AMD";
	case 0x8086: return "Intel";
	default:     return "GPU";
	}
}

static inline int64_t QueryQpcNow()
{
	LARGE_INTEGER t = {};
	QueryPerformanceCounter(&t);
	return t.QuadPart;
}

} // namespace

bool NvReflex::Init(ID3D11Device *dev11, ID3D11DeviceContext *ctx11, IDXGISwapChain2 *swapChain2,
                    HANDLE waitableObj, UINT vendorId, ReflexMode initialMode, bool forceUniversal)
{
	m_dev = dev11;
	m_ctx = ctx11;
	m_swapChain2 = swapChain2;
	m_waitableObj = waitableObj;
	m_vendorId = vendorId;
	m_mode = initialMode;
	m_available = false;
	m_backend = LowLatencyBackend::None;
	m_verifiedLogged = false;
	m_ullWrite = 0;
	m_ullActiveSlot = -1;
	m_emaPcLat = m_emaGpu = m_emaOsQ = 0.0f;
	InterlockedExchange(&m_presentSeq, 0);
	m_lastWaitedSeq = 0;
	QueryPerformanceFrequency(&m_qpcFreq);
	if (!dev11) return false;

	// Tier 1: Native 32-bit NVIDIA Reflex on NVIDIA GPUs
	if (!forceUniversal && (vendorId == 0x10DE || vendorId == 0)) {
		if (InitNvidiaReflex(initialMode))
			return true;
	}

	// Tier 2: Universal DX11 Pre-Input Low-Latency (AMD Radeon, Intel Arc/UHD/Xe, non-Reflex GPUs)
	// Note: Official AMD Anti-Lag 2 (amdxx64.dll) and Intel XeLL (libxell.dll / D3D12) SDKs are 64-bit-only,
	// whereas GTA:SA is 32-bit x86. ULL-DX11 delivers the exact same zero-queue pre-input sync algorithm
	// (MaxFrameLatency=1 + pre-input DXGI waitable pacing + D3D11 HW timestamp telemetry) on all GPUs.
	return InitUniversalDx11(initialMode);
}

bool NvReflex::InitNvidiaReflex(ReflexMode initialMode)
{
	m_nvapi = LoadLibraryA("nvapi.dll");
	if (!m_nvapi)
		return false;

	auto qi = (NvAPI_QueryInterface_t)GetProcAddress(m_nvapi, "nvapi_QueryInterface");
	if (!qi) {
		FreeLibrary(m_nvapi);
		m_nvapi = nullptr;
		return false;
	}

	g_fnInit   = (NvAPI_Initialize_t)qi(0x0150e828);
	g_fnUnload = (NvAPI_Unload_t)qi(0xd22bdd7e);
	g_fnSetSM  = (NvAPI_D3D_SetSleepMode_t)qi(0xac1ca9e0);
	g_fnGetSS  = (NvAPI_D3D_GetSleepStatus_t)qi(0xaef96ca1);
	g_fnMark   = (NvAPI_D3D_SetLatencyMarker_t)qi(0xd9984c05);
	g_fnGetLat = (NvAPI_D3D_GetLatency_t)qi(0x1a587f9c);

	if (!g_fnInit || !g_fnSetSM || !g_fnMark) {
		FreeLibrary(m_nvapi);
		m_nvapi = nullptr;
		return false;
	}

	NvAPI_Status st = g_fnInit();
	if (st != 0) {
		Log("NVAPI: NvAPI_Initialize failed (%d) - switching to Universal DX11 Low-Latency", st);
		FreeLibrary(m_nvapi);
		m_nvapi = nullptr;
		return false;
	}
	m_initialized = true;

	NV_SET_SLEEP_MODE_PARAMS_V1 sm = {};
	sm.version = MAKE_NVAPI_VER(NV_SET_SLEEP_MODE_PARAMS_V1, 1);
	sm.bLowLatencyMode = (initialMode != ReflexMode::Off) ? 1 : 0;
	sm.bLowLatencyBoost = (initialMode == ReflexMode::Boost) ? 1 : 0;
	sm.minimumIntervalUs = 0; // Never cap FPS
	sm.bUseMarkersToOptimize = 0; // Only valid when calling NvAPI_D3D_Sleep(); otherwise stalls Present()!

	st = g_fnSetSM(m_dev, &sm);
	if (st != 0) {
		Log("NVAPI: NvAPI_D3D_SetSleepMode failed (%d) - switching to Universal DX11 Low-Latency", st);
		return false;
	}

	m_backend = LowLatencyBackend::NvidiaReflex;
	m_available = true;
	Log("NVAPI: NVIDIA Reflex ready (mode=%s)", ReflexModeName(initialMode));
	return true;
}

bool NvReflex::InitUniversalDx11(ReflexMode initialMode)
{
	if (!m_dev) return false;

	// Allocate 4-slot non-blocking D3D11 hardware timestamp + event query ring for real PC Lat & GPU ms
	D3D11_QUERY_DESC qd = {};
	bool allOk = (m_ctx != nullptr);
	for (int i = 0; i < kUllSlots && allOk; ++i) {
		qd.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
		if (FAILED(m_dev->CreateQuery(&qd, &m_ull[i].disjoint))) allOk = false;
		qd.Query = D3D11_QUERY_TIMESTAMP;
		if (allOk && FAILED(m_dev->CreateQuery(&qd, &m_ull[i].tsStart))) allOk = false;
		if (allOk && FAILED(m_dev->CreateQuery(&qd, &m_ull[i].tsEnd)))   allOk = false;
		qd.Query = D3D11_QUERY_EVENT;
		if (allOk && FAILED(m_dev->CreateQuery(&qd, &m_ull[i].doneEv)))  allOk = false;
		m_ull[i].inFlight = false;
	}
	if (!allOk)
		ReleaseUllQueries();
	m_ullQueriesReady = allOk;

	UINT maxLat = (initialMode != ReflexMode::Off) ? 1u : 2u;
	ApplyDxgiMaxLatency(maxLat);
	if (!m_gpuDoneEvent)
		m_gpuDoneEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
	else
		SetEvent(m_gpuDoneEvent);

	m_backend = LowLatencyBackend::UniversalDx11;
	m_available = true;
	AcquireSRWLockExclusive(&m_statsLock);
	m_stats.lowLat = (initialMode != ReflexMode::Off);
	m_stats.iFlip  = false;
	m_stats.vrr    = false;
	m_stats.valid  = false;
	ReleaseSRWLockExclusive(&m_statsLock);

	Log("LowLatency: %s Universal DX11 Low-Latency ready (mode=%s, MaxLat=%u, waitable=%d, hwTs=%d)",
		VendorLabel(m_vendorId), ReflexModeName(initialMode), maxLat,
		m_waitableObj ? 1 : 0, m_ullQueriesReady ? 1 : 0);
	return true;
}

void NvReflex::ApplyDxgiMaxLatency(UINT maxLat)
{
	if (m_dev) {
		IDXGIDevice1 *dxgiDev = nullptr;
		if (SUCCEEDED(m_dev->QueryInterface(__uuidof(IDXGIDevice1), (void **)&dxgiDev)) && dxgiDev) {
			dxgiDev->SetMaximumFrameLatency(maxLat);
			dxgiDev->Release();
		}
	}
	if (m_swapChain2)
		m_swapChain2->SetMaximumFrameLatency(maxLat);
}

void NvReflex::ReleaseUllQueries()
{
	for (int i = 0; i < kUllSlots; ++i) {
		if (m_ull[i].disjoint) { m_ull[i].disjoint->Release(); m_ull[i].disjoint = nullptr; }
		if (m_ull[i].tsStart)  { m_ull[i].tsStart->Release();  m_ull[i].tsStart  = nullptr; }
		if (m_ull[i].tsEnd)    { m_ull[i].tsEnd->Release();    m_ull[i].tsEnd    = nullptr; }
		if (m_ull[i].doneEv)   { m_ull[i].doneEv->Release();   m_ull[i].doneEv   = nullptr; }
		m_ull[i].inFlight = false;
	}
	m_ullQueriesReady = false;
	m_ullActiveSlot = -1;
}

void NvReflex::ApplyMode(ReflexMode mode)
{
	if (!m_available || !m_dev || mode == m_mode) return;
	m_mode = mode;

	if (m_backend == LowLatencyBackend::NvidiaReflex && g_fnSetSM) {
		NV_SET_SLEEP_MODE_PARAMS_V1 sm = {};
		sm.version = MAKE_NVAPI_VER(NV_SET_SLEEP_MODE_PARAMS_V1, 1);
		sm.bLowLatencyMode = (mode != ReflexMode::Off) ? 1 : 0;
		sm.bLowLatencyBoost = (mode == ReflexMode::Boost) ? 1 : 0;
		sm.minimumIntervalUs = 0;
		sm.bUseMarkersToOptimize = 0;
		g_fnSetSM(m_dev, &sm);
		Log("NVAPI: NVIDIA Reflex switched to %s", ReflexModeName(mode));
	} else if (m_backend == LowLatencyBackend::UniversalDx11) {
		UINT maxLat = (mode != ReflexMode::Off) ? 1u : 2u;
		ApplyDxgiMaxLatency(maxLat);
		AcquireSRWLockExclusive(&m_statsLock);
		m_stats.lowLat = (mode != ReflexMode::Off);
		ReleaseSRWLockExclusive(&m_statsLock);
		Log("LowLatency: %s ULL-DX11 switched to %s (MaxLat=%u)",
			VendorLabel(m_vendorId), ReflexModeName(mode), maxLat);
	}
}

void NvReflex::Disable()
{
	if (m_available && m_backend == LowLatencyBackend::NvidiaReflex && m_dev && g_fnSetSM) {
		NV_SET_SLEEP_MODE_PARAMS_V1 sm = {};
		sm.version = MAKE_NVAPI_VER(NV_SET_SLEEP_MODE_PARAMS_V1, 1);
		g_fnSetSM(m_dev, &sm);
	}
	ReleaseUllQueries();
	if (m_gpuDoneEvent) {
		SetEvent(m_gpuDoneEvent);
		CloseHandle(m_gpuDoneEvent);
		m_gpuDoneEvent = nullptr;
	}
	m_available = false;
	m_backend = LowLatencyBackend::None;
	m_dev = nullptr;
	m_ctx = nullptr;
	m_swapChain2 = nullptr;
	m_waitableObj = nullptr;
}

void NvReflex::Shutdown()
{
	Disable();
	if (m_initialized && g_fnUnload) {
		g_fnUnload();
		m_initialized = false;
	}
	if (m_nvapi) {
		FreeLibrary(m_nvapi);
		m_nvapi = nullptr;
	}
}

uint64_t NvReflex::NextFrameId()
{
	uint64_t id = (uint64_t)InterlockedIncrement64(&m_frameId);
	MarkFrame(id, RM_SIMULATION_START);
	return id;
}

void NvReflex::MarkFrame(uint64_t frameId, ReflexMarker type)
{
	if (!m_available || !m_dev || frameId == 0) return;
	if (m_backend == LowLatencyBackend::NvidiaReflex && g_fnMark) {
		NV_LATENCY_MARKER_PARAMS_V1 mp = {};
		mp.version = MAKE_NVAPI_VER(NV_LATENCY_MARKER_PARAMS_V1, 1);
		mp.frameID = frameId;
		mp.markerType = (int32_t)type;
		g_fnMark(m_dev, &mp);
	}
}

int64_t NvReflex::PreInputSync()
{
	// On AMD/Intel (UniversalDx11), synchronize on GPU completion + DXGI waitable object
	// RIGHT BEFORE the game samples mouse/keyboard input (CGame::Process), matching the exact
	// pre-input sleep architecture of NVIDIA Reflex, AMD Anti-Lag 2, and Intel XeLL:
	// - When CPU-bound: GPU is already done -> returns in ~0.001 ms (0 FPS loss).
	// - When GPU-bound: pauses CPU right before input read until GPU finishes the in-flight frame,
	//   draining the render queue to 0 frames (cutting GPU-bound PC latency by ~45-55%).
	if (m_available && m_backend == LowLatencyBackend::UniversalDx11 && m_mode != ReflexMode::Off) {
		LONG curSeq = InterlockedCompareExchange(&m_presentSeq, 0, 0);
		if (curSeq != m_lastWaitedSeq) {
			m_lastWaitedSeq = curSeq;
			if (m_gpuDoneEvent)
				WaitForSingleObject(m_gpuDoneEvent, 16);
			if (m_waitableObj)
				WaitForSingleObjectEx(m_waitableObj, 16, TRUE);
		}
	}

	int64_t qpc = QueryQpcNow();
	InterlockedExchange64(&m_curInputQpc, qpc);
	return qpc;
}

int64_t NvReflex::CurrentInputSampleQpc() const
{
	return InterlockedCompareExchange64(const_cast<volatile LONG64 *>(&m_curInputQpc), 0, 0);
}

void NvReflex::BeginGpuWork(uint64_t frameId, int64_t inputSampleQpc)
{
	m_ullActiveSlot = -1;
	if (!m_available || m_backend != LowLatencyBackend::UniversalDx11 || !m_ullQueriesReady || !m_ctx)
		return;

	int idx = m_ullWrite;
	if (m_ull[idx].inFlight)
		return; // Ring still busy; skip instrumentation for this frame without stalling GPU

	m_ull[idx].frameId  = frameId;
	m_ull[idx].inputQpc = inputSampleQpc ? inputSampleQpc : QueryQpcNow();
	m_ull[idx].presQpc  = 0;
	m_ctx->Begin(m_ull[idx].disjoint);
	m_ctx->End(m_ull[idx].tsStart);
	m_ullActiveSlot = idx;
}

void NvReflex::EndGpuWork()
{
	int idx = m_ullActiveSlot;
	if (idx < 0 || !m_ullQueriesReady || !m_ctx)
		return;

	m_ull[idx].presQpc = QueryQpcNow();
	m_ctx->End(m_ull[idx].tsEnd);
	m_ctx->End(m_ull[idx].disjoint);
	m_ctx->End(m_ull[idx].doneEv);
	m_ull[idx].inFlight = true;
	m_ullWrite = (idx + 1) % kUllSlots;
	m_ullActiveSlot = -1;
}

void NvReflex::OnPresentFinished()
{
	if (m_backend != LowLatencyBackend::UniversalDx11)
		return;

	if (m_mode != ReflexMode::Off && m_ullQueriesReady && m_ctx) {
		int lastSlot = (m_ullWrite + kUllSlots - 1) % kUllSlots;
		auto &s = m_ull[lastSlot];
		if (s.inFlight && s.doneEv) {
			if (m_gpuDoneEvent)
				ResetEvent(m_gpuDoneEvent);
			InterlockedIncrement(&m_presentSeq);

			// Wait on the D3D11 thread for GPU completion of the just-presented frame (zero queue buildup).
			// In Boost mode, spin with YieldProcessor for sub-0.02ms wakeup; in On mode, yield cleanly.
			m_ctx->Flush();
			BOOL done = FALSE;
			int64_t deadline = QueryQpcNow() + (m_qpcFreq.QuadPart * 16) / 1000;
			while (m_ctx->GetData(s.doneEv, &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK || !done) {
				if (QueryQpcNow() >= deadline)
					break;
				if (m_mode == ReflexMode::Boost)
					YieldProcessor();
				else
					SwitchToThread();
			}
			if (m_gpuDoneEvent)
				SetEvent(m_gpuDoneEvent);
			return;
		}
	}

	if (m_gpuDoneEvent)
		SetEvent(m_gpuDoneEvent);
	InterlockedIncrement(&m_presentSeq);
	// Keep DXGI waitable semaphore drained non-blockingly when ULL is switched OFF
	if (m_mode == ReflexMode::Off && m_waitableObj)
		WaitForSingleObject(m_waitableObj, 0);
}

void NvReflex::PollUllTelemetry()
{
	if (!m_ullQueriesReady || !m_ctx || m_qpcFreq.QuadPart <= 0)
		return;

	bool updated = false;
	const double qpcToMs = 1000.0 / (double)m_qpcFreq.QuadPart;

	for (int i = 0; i < kUllSlots; ++i) {
		auto &s = m_ull[i];
		if (!s.inFlight) continue;

		BOOL done = FALSE;
		HRESULT hrEv = m_ctx->GetData(s.doneEv, &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH);
		if (hrEv != S_OK || !done)
			continue;

		int64_t doneQpc = QueryQpcNow();
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disj = {};
		UINT64 t0 = 0, t1 = 0;
		HRESULT hrD = m_ctx->GetData(s.disjoint, &disj, sizeof(disj), D3D11_ASYNC_GETDATA_DONOTFLUSH);
		HRESULT hr0 = m_ctx->GetData(s.tsStart,  &t0,   sizeof(t0),   D3D11_ASYNC_GETDATA_DONOTFLUSH);
		HRESULT hr1 = m_ctx->GetData(s.tsEnd,    &t1,   sizeof(t1),   D3D11_ASYNC_GETDATA_DONOTFLUSH);

		s.inFlight = false;

		if (s.inputQpc > 0 && doneQpc > s.inputQpc) {
			double pcMs = (double)(doneQpc - s.inputQpc) * qpcToMs;
			if (pcMs > 0.02 && pcMs < 250.0) {
				m_emaPcLat = (m_emaPcLat <= 0.001f) ? (float)pcMs : (m_emaPcLat * 0.88f + (float)pcMs * 0.12f);
				updated = true;
			}
		}
		if (s.presQpc > 0 && doneQpc >= s.presQpc) {
			double osqMs = (double)(doneQpc - s.presQpc) * qpcToMs;
			if (osqMs >= 0.0 && osqMs < 250.0) {
				m_emaOsQ = (m_emaOsQ <= 0.001f) ? (float)osqMs : (m_emaOsQ * 0.88f + (float)osqMs * 0.12f);
			}
		}
		if (hrD == S_OK && hr0 == S_OK && hr1 == S_OK && !disj.Disjoint && disj.Frequency > 0 && t1 >= t0) {
			double gMs = (double)(t1 - t0) * 1000.0 / (double)disj.Frequency;
			if (gMs >= 0.0 && gMs < 250.0) {
				m_emaGpu = (m_emaGpu <= 0.001f) ? (float)gMs : (m_emaGpu * 0.88f + (float)gMs * 0.12f);
				updated = true;
			}
		}
	}

	if (updated) {
		AcquireSRWLockExclusive(&m_statsLock);
		m_stats.pcLatMs   = m_emaPcLat;
		m_stats.gpuMs     = m_emaGpu;
		m_stats.osQueueMs = m_emaOsQ;
		m_stats.lowLat    = (m_mode != ReflexMode::Off);
		m_stats.valid     = true;
		ReleaseSRWLockExclusive(&m_statsLock);
	}

	uint64_t fid = (uint64_t)InterlockedCompareExchange64(&m_frameId, 0, 0);
	if (!m_verifiedLogged && fid >= 180 && m_emaPcLat > 0.01f) {
		m_verifiedLogged = true;
		Log("LowLatency: %s ULL-DX11 verified (mode=%s, MaxLat=%u | pcLat=%.2fms, osQueue=%.2fms, gpu=%.2fms)",
		    VendorLabel(m_vendorId), ReflexModeName(m_mode), (m_mode != ReflexMode::Off) ? 1u : 2u,
		    m_emaPcLat, m_emaOsQ, m_emaGpu);
	}
}

void NvReflex::PollTelemetry()
{
	if (!m_available || !m_dev) return;

	if (m_backend == LowLatencyBackend::UniversalDx11) {
		PollUllTelemetry();
		return;
	}

	if (m_backend != LowLatencyBackend::NvidiaReflex) return;
	uint64_t fid = (uint64_t)InterlockedCompareExchange64(&m_frameId, 0, 0);
	if (fid < 120) return;

	DWORD now = GetTickCount();
	if (m_verifiedLogged && m_lastPollTick && (now - m_lastPollTick) < 500)
		return;
	m_lastPollTick = now;

	NV_GET_SLEEP_STATUS_PARAMS_V1 ss = {};
	ss.version = MAKE_NVAPI_VER(NV_GET_SLEEP_STATUS_PARAMS_V1, 1);
	bool gotSS = g_fnGetSS && (g_fnGetSS(m_dev, &ss) == 0);

	double pcMs = 0.0, gpuMs = 0.0, osqMs = 0.0;
	int n = 0;
	if (g_fnGetLat) {
		NV_LATENCY_RESULT_PARAMS_V1 res = {};
		res.version = MAKE_NVAPI_VER(NV_LATENCY_RESULT_PARAMS_V1, 1);
		if (g_fnGetLat(m_dev, &res) == 0) {
			for (int i = 48; i < 64; ++i) {
				const auto &r = res.frameReport[i];
				if (r.frameID == 0 || r.simStartTime == 0 || r.gpuRenderEndTime <= r.simStartTime) continue;
				double pc = (double)(r.gpuRenderEndTime - r.simStartTime) * 0.001;
				if (pc > 500.0) continue;
				pcMs  += pc;
				gpuMs += (double)r.gpuActiveRenderTimeUs * 0.001;
				osqMs += (r.osRenderQueueEndTime > r.osRenderQueueStartTime)
				       ? (double)(r.osRenderQueueEndTime - r.osRenderQueueStartTime) * 0.001 : 0.0;
				++n;
			}
		}
	}

	if (gotSS || n > 0) {
		AcquireSRWLockExclusive(&m_statsLock);
		if (gotSS) {
			m_stats.lowLat = (ss.bLowLatencyMode != 0);
			m_stats.iFlip  = (ss.bFullscreenIFlip != 0);
			m_stats.vrr    = (ss.bFsVrr != 0);
			m_stats.valid  = true;
		}
		if (n > 0) {
			m_stats.pcLatMs   = (float)(pcMs / n);
			m_stats.gpuMs     = (float)(gpuMs / n);
			m_stats.osQueueMs = (float)(osqMs / n);
		}
		ReleaseSRWLockExclusive(&m_statsLock);
	}

	if (!m_verifiedLogged && fid >= 180) {
		m_verifiedLogged = true;
		if (gotSS && n > 0) {
			Log("NVAPI: verified (lowLat=%u, iFlip=%u, fsVrr=%u, cplVsync=%u | pcLat=%.2fms, osQueue=%.2fms, gpu=%.2fms)",
			    ss.bLowLatencyMode, ss.bFullscreenIFlip, ss.bFsVrr, ss.bCplVsyncOn,
			    pcMs / n, osqMs / n, gpuMs / n);
		} else if (gotSS) {
			Log("NVAPI: verified (lowLat=%u, iFlip=%u, fsVrr=%u, cplVsync=%u)",
			    ss.bLowLatencyMode, ss.bFullscreenIFlip, ss.bFsVrr, ss.bCplVsyncOn);
		}
	}
}

ReflexStats NvReflex::GetStats() const
{
	AcquireSRWLockShared(&m_statsLock);
	ReflexStats copy = m_stats;
	ReleaseSRWLockShared(&m_statsLock);
	return copy;
}

const char *NvReflex::BackendName() const
{
	if (m_backend == LowLatencyBackend::NvidiaReflex)
		return "NVIDIA Reflex";
	if (m_backend == LowLatencyBackend::UniversalDx11) {
		if (m_vendorId == 0x1002) return "AMD ULL-DX11";
		if (m_vendorId == 0x8086) return "Intel ULL-DX11";
		return "DX11 ULL";
	}
	return "Low-Latency";
}

const char *NvReflex::ShortHudTag() const
{
	if (m_backend == LowLatencyBackend::NvidiaReflex) {
		ReflexStats rs = GetStats();
		return rs.iFlip ? "IFLIP" : "FLIP";
	}
	if (m_backend == LowLatencyBackend::UniversalDx11)
		return (m_mode != ReflexMode::Off) ? "ULL" : "FLIP";
	return "FLIP";
}
