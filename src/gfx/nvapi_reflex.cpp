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
typedef NvAPI_Status(__cdecl *NvAPI_D3D_Sleep_t)(IUnknown *);
typedef NvAPI_Status(__cdecl *NvAPI_D3D_SetLatencyMarker_t)(IUnknown *, NV_LATENCY_MARKER_PARAMS_V1 *);
typedef NvAPI_Status(__cdecl *NvAPI_D3D_GetLatency_t)(IUnknown *, NV_LATENCY_RESULT_PARAMS_V1 *);

NvAPI_Initialize_t           g_fnInit   = nullptr;
NvAPI_Unload_t               g_fnUnload = nullptr;
NvAPI_D3D_SetSleepMode_t     g_fnSetSM  = nullptr;
NvAPI_D3D_GetSleepStatus_t   g_fnGetSS  = nullptr;
NvAPI_D3D_Sleep_t            g_fnSleep  = nullptr;
NvAPI_D3D_SetLatencyMarker_t g_fnMark   = nullptr;
NvAPI_D3D_GetLatency_t       g_fnGetLat = nullptr;

} // namespace

bool NvReflex::Init(ID3D11Device *dev11)
{
	m_dev = dev11;
	m_available = false;
	if (!dev11) return false;

	m_nvapi = LoadLibraryA("nvapi.dll");
	if (!m_nvapi) {
		Log("NVAPI: nvapi.dll not present - using DXGI Waitable SwapChain low-latency pacing");
		return false;
	}

	auto qi = (NvAPI_QueryInterface_t)GetProcAddress(m_nvapi, "nvapi_QueryInterface");
	if (!qi) {
		Log("NVAPI: nvapi_QueryInterface missing");
		return false;
	}

	g_fnInit   = (NvAPI_Initialize_t)qi(0x0150e828);
	g_fnUnload = (NvAPI_Unload_t)qi(0xd22bdd7e);
	g_fnSetSM  = (NvAPI_D3D_SetSleepMode_t)qi(0xac1ca9e0);
	g_fnGetSS  = (NvAPI_D3D_GetSleepStatus_t)qi(0xaef96ca1);
	g_fnSleep  = (NvAPI_D3D_Sleep_t)qi(0x852cd1d2);
	g_fnMark   = (NvAPI_D3D_SetLatencyMarker_t)qi(0xd9984c05);
	g_fnGetLat = (NvAPI_D3D_GetLatency_t)qi(0x1a587f9c);

	if (!g_fnInit || !g_fnSetSM || !g_fnSleep || !g_fnMark) {
		Log("NVAPI: Reflex functions unavailable on installed driver");
		return false;
	}

	NvAPI_Status st = g_fnInit();
	if (st != 0) {
		Log("NVAPI: NvAPI_Initialize failed (%d)", st);
		return false;
	}
	m_initialized = true;

	NV_SET_SLEEP_MODE_PARAMS_V1 sm = {};
	sm.version = MAKE_NVAPI_VER(NV_SET_SLEEP_MODE_PARAMS_V1, 1);
	sm.bLowLatencyMode = 1;
	sm.bLowLatencyBoost = 1;
	sm.minimumIntervalUs = 0;
	sm.bUseMarkersToOptimize = 1;

	st = g_fnSetSM(m_dev, &sm);
	if (st != 0) {
		Log("NVAPI: NvAPI_D3D_SetSleepMode failed (%d) - falling back to DXGI Waitable SwapChain", st);
		return false;
	}

	m_available = true;
	Log("NVAPI: NVIDIA Reflex active (Low Latency + Boost)");
	return true;
}

void NvReflex::Shutdown()
{
	if (m_available && m_dev && g_fnSetSM) {
		NV_SET_SLEEP_MODE_PARAMS_V1 sm = {};
		sm.version = MAKE_NVAPI_VER(NV_SET_SLEEP_MODE_PARAMS_V1, 1);
		g_fnSetSM(m_dev, &sm);
	}
	m_available = false;
	m_dev = nullptr;
	if (m_initialized && g_fnUnload) {
		g_fnUnload();
		m_initialized = false;
	}
	if (m_nvapi) {
		FreeLibrary(m_nvapi);
		m_nvapi = nullptr;
	}
}

void NvReflex::Sleep()
{
	if (m_available && m_dev && g_fnSleep)
		g_fnSleep(m_dev);
}

void NvReflex::BeginFrame()
{
	++m_frameId;
	Mark(RM_SIMULATION_START);
}

void NvReflex::Mark(ReflexMarker type)
{
	if (!m_available || !m_dev || !g_fnMark || m_frameId == 0) return;
	NV_LATENCY_MARKER_PARAMS_V1 mp = {};
	mp.version = MAKE_NVAPI_VER(NV_LATENCY_MARKER_PARAMS_V1, 1);
	mp.frameID = m_frameId;
	mp.markerType = (int32_t)type;
	g_fnMark(m_dev, &mp);
}

void NvReflex::VerifyOnce()
{
	if (m_verifiedLogged || !m_available || !m_dev || m_frameId < 180) return;
	m_verifiedLogged = true;

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

	if (gotSS && n > 0) {
		Log("NVAPI: verified (lowLat=%u, gameSleep=%u, iFlip=%u, fsVrr=%u, cplVsync=%u | pcLat=%.2fms, osQueue=%.2fms, gpu=%.2fms)",
		    ss.bLowLatencyMode, ss.bUseGameSleep, ss.bFullscreenIFlip, ss.bFsVrr, ss.bCplVsyncOn,
		    pcMs / n, osqMs / n, gpuMs / n);
	} else if (gotSS) {
		Log("NVAPI: verified (lowLat=%u, gameSleep=%u, iFlip=%u, fsVrr=%u, cplVsync=%u)",
		    ss.bLowLatencyMode, ss.bUseGameSleep, ss.bFullscreenIFlip, ss.bFsVrr, ss.bCplVsyncOn);
	}
}
