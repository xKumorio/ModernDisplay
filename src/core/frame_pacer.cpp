// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "frame_pacer.h"

#include <intrin.h>
#include <math.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

FramePacer::FramePacer()
{
	QueryPerformanceFrequency(&m_freq);

	typedef HANDLE(WINAPI *CreateWaitableTimerExW_t)(LPSECURITY_ATTRIBUTES, LPCWSTR, DWORD, DWORD);
	HMODULE k32 = GetModuleHandleA("kernel32.dll");
	auto fnEx = k32 ? (CreateWaitableTimerExW_t)GetProcAddress(k32, "CreateWaitableTimerExW") : nullptr;
	if (fnEx) {
		m_timer = fnEx(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
		if (m_timer) m_highResTimer = true;
	}
	if (!m_timer)
		m_timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
}

FramePacer::~FramePacer()
{
	if (m_timer) {
		CloseHandle(m_timer);
		m_timer = nullptr;
	}
}

void FramePacer::Reset()
{
	m_nextDeadline = 0;
	m_lastTarget = 0.0;
}

void FramePacer::Pace(double targetFps)
{
	if (targetFps <= 0.5 || m_freq.QuadPart <= 0) {
		m_nextDeadline = 0;
		m_lastTarget = 0.0;
		return;
	}

	LARGE_INTEGER now = {};
	QueryPerformanceCounter(&now);

	const LONGLONG intervalTicks = (LONGLONG)((double)m_freq.QuadPart / targetFps + 0.5);
	if (intervalTicks <= 0) return;

	if (m_nextDeadline == 0 || fabs(targetFps - m_lastTarget) > 0.01) {
		m_lastTarget = targetFps;
		m_nextDeadline = now.QuadPart + intervalTicks;
		return;
	}

	// If a frame hitch exceeded 2 intervals, snap forward cleanly so we never burst-replay frames.
	if (now.QuadPart >= m_nextDeadline) {
		if (now.QuadPart - m_nextDeadline > intervalTicks * 2)
			m_nextDeadline = now.QuadPart + intervalTicks;
		else
			m_nextDeadline += intervalTicks;
		return;
	}

	// Coarse sleep via high-resolution waitable timer, leaving a small spin margin for <0.02ms precision.
	const double spinMarginMs = m_highResTimer ? 0.75 : 2.0;
	const LONGLONG spinMarginTicks = (LONGLONG)((double)m_freq.QuadPart * (spinMarginMs * 0.001));

	if (m_timer) {
		LONGLONG rem = m_nextDeadline - now.QuadPart;
		if (rem > spinMarginTicks) {
			double sleepSec = (double)(rem - spinMarginTicks) / (double)m_freq.QuadPart;
			LONGLONG due100ns = -(LONGLONG)(sleepSec * 10000000.0);
			if (due100ns < -1000) {
				LARGE_INTEGER li = {};
				li.QuadPart = due100ns;
				if (SetWaitableTimer(m_timer, &li, 0, nullptr, nullptr, FALSE))
					WaitForSingleObject(m_timer, 100);
			}
		}
	}

	// Final sub-millisecond precision spin-wait
	for (;;) {
		QueryPerformanceCounter(&now);
		if (now.QuadPart >= m_nextDeadline)
			break;
		_mm_pause();
	}

	// Advance deadline strictly by exact interval to eliminate cumulative timing drift
	m_nextDeadline += intervalTicks;
	if (now.QuadPart - m_nextDeadline > intervalTicks)
		m_nextDeadline = now.QuadPart + intervalTicks;
}
