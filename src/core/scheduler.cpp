// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "scheduler.h"
#include "log.h"

#include <stdint.h>

namespace {

typedef LONG(NTAPI *NtSetTimerResolution_t)(ULONG DesiredResolution, BOOLEAN SetResolution, PULONG CurrentResolution);
typedef UINT(WINAPI *TimeBeginEndPeriod_t)(UINT uPeriod);
typedef HANDLE(WINAPI *AvSetMmThreadCharacteristicsW_t)(LPCWSTR TaskName, LPDWORD TaskIndex);
typedef BOOL(WINAPI *AvSetMmThreadPriority_t)(HANDLE AvrtHandle, int Priority);
typedef BOOL(WINAPI *AvRevertMmThreadCharacteristics_t)(HANDLE AvrtHandle);

struct MdPowerThrottlingState {
	ULONG Version;
	ULONG ControlMask;
	ULONG StateMask;
};
typedef BOOL(WINAPI *SetProcessInformation_t)(HANDLE, int, LPVOID, DWORD);

static volatile LONG                 g_schedRefCount = 0;
static NtSetTimerResolution_t        g_ntSetTimerRes = nullptr;
static ULONG                         g_setTimerRes100ns = 0;
static HMODULE                       g_winmmMod = nullptr;
static TimeBeginEndPeriod_t          g_timeEndPeriod = nullptr;
static bool                          g_winmmPeriodActive = false;

static SRWLOCK                       g_avrtLock = SRWLOCK_INIT;
static HMODULE                       g_avrtMod = nullptr;
static bool                          g_avrtLoaded = false;
static AvSetMmThreadCharacteristicsW_t g_avSetChar = nullptr;
static AvSetMmThreadPriority_t         g_avSetPrio = nullptr;
static AvRevertMmThreadCharacteristics_t g_avRevert = nullptr;

static void EnsureAvrtLoaded()
{
	AcquireSRWLockExclusive(&g_avrtLock);
	if (!g_avrtLoaded) {
		g_avrtLoaded = true;
		g_avrtMod = LoadLibraryA("avrt.dll");
		if (g_avrtMod) {
			g_avSetChar = (AvSetMmThreadCharacteristicsW_t)GetProcAddress(g_avrtMod, "AvSetMmThreadCharacteristicsW");
			g_avSetPrio = (AvSetMmThreadPriority_t)GetProcAddress(g_avrtMod, "AvSetMmThreadPriority");
			g_avRevert  = (AvRevertMmThreadCharacteristics_t)GetProcAddress(g_avrtMod, "AvRevertMmThreadCharacteristics");
		}
	}
	ReleaseSRWLockExclusive(&g_avrtLock);
}

} // namespace

void SchedulerInitProcess()
{
	if (InterlockedIncrement(&g_schedRefCount) != 1)
		return;

	// 1. Elevate process priority cleanly to HIGH_PRIORITY_CLASS (never overrides REALTIME)
	DWORD curPrio = GetPriorityClass(GetCurrentProcess());
	bool prioRaised = false;
	if (curPrio == NORMAL_PRIORITY_CLASS || curPrio == ABOVE_NORMAL_PRIORITY_CLASS || curPrio == BELOW_NORMAL_PRIORITY_CLASS) {
		if (SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
			prioRaised = true;
	}

	// 2. Opt out of Windows 11 EcoQoS (prevents hybrid P/E-core demotion) and Timer Resolution throttling
	bool ecoOptOut = false;
	if (HMODULE k32 = GetModuleHandleA("kernel32.dll")) {
		auto fnSetProcInfo = (SetProcessInformation_t)GetProcAddress(k32, "SetProcessInformation");
		if (fnSetProcInfo) {
			constexpr int   kProcessPowerThrottling = 4;
			constexpr ULONG kExecutionSpeed = 0x1u;
			constexpr ULONG kIgnoreTimerRes = 0x4u;
			MdPowerThrottlingState pts = {};
			pts.Version = 1;
			pts.ControlMask = kExecutionSpeed | kIgnoreTimerRes;
			pts.StateMask   = 0; // Explicitly disable both throttles!
			if (fnSetProcInfo(GetCurrentProcess(), kProcessPowerThrottling, &pts, sizeof(pts))) {
				ecoOptOut = true;
			} else {
				// Fallback for older Win10 builds that only know ExecutionSpeed
				pts.ControlMask = kExecutionSpeed;
				pts.StateMask   = 0;
				if (fnSetProcInfo(GetCurrentProcess(), kProcessPowerThrottling, &pts, sizeof(pts)))
					ecoOptOut = true;
			}
		}
	}

	// 3. Request 0.50 ms kernel timer resolution via NtSetTimerResolution (fallback: winmm timeBeginPeriod(1))
	double timerMs = 0.0;
	if (HMODULE ntdll = GetModuleHandleA("ntdll.dll")) {
		g_ntSetTimerRes = (NtSetTimerResolution_t)GetProcAddress(ntdll, "NtSetTimerResolution");
		if (g_ntSetTimerRes) {
			ULONG actual = 0;
			if (g_ntSetTimerRes(5000, TRUE, &actual) == 0) {
				g_setTimerRes100ns = 5000;
				timerMs = (double)actual * 0.0001;
			}
		}
	}
	if (g_setTimerRes100ns == 0) {
		g_winmmMod = LoadLibraryA("winmm.dll");
		if (g_winmmMod) {
			auto fnBegin = (TimeBeginEndPeriod_t)GetProcAddress(g_winmmMod, "timeBeginPeriod");
			g_timeEndPeriod = (TimeBeginEndPeriod_t)GetProcAddress(g_winmmMod, "timeEndPeriod");
			if (fnBegin && fnBegin(1) == 0) {
				g_winmmPeriodActive = true;
				timerMs = 1.0;
			}
		}
	}

	EnsureAvrtLoaded();
	Log("Scheduler: MMCSS 'Games' ready=%d, prioHigh=%d, ecoOptOut=%d, timer=%.2fms",
		(g_avSetChar != nullptr) ? 1 : 0, prioRaised ? 1 : 0, ecoOptOut ? 1 : 0, timerMs);
}

void SchedulerShutdownProcess()
{
	if (InterlockedDecrement(&g_schedRefCount) != 0)
		return;

	if (g_ntSetTimerRes && g_setTimerRes100ns != 0) {
		ULONG actual = 0;
		g_ntSetTimerRes(g_setTimerRes100ns, FALSE, &actual);
		g_setTimerRes100ns = 0;
	}
	if (g_winmmPeriodActive && g_timeEndPeriod) {
		g_timeEndPeriod(1);
		g_winmmPeriodActive = false;
	}
}

HANDLE SchedulerRegisterGamesThread()
{
	EnsureAvrtLoaded();
	if (!g_avSetChar)
		return nullptr;
	DWORD taskIdx = 0;
	HANDLE h = g_avSetChar(L"Games", &taskIdx);
	if (h && g_avSetPrio)
		g_avSetPrio(h, 1); // AVRT_PRIORITY_HIGH
	return h;
}

void SchedulerUnregisterGamesThread(HANDLE mmcssHandle)
{
	if (mmcssHandle && g_avRevert)
		g_avRevert(mmcssHandle);
}
