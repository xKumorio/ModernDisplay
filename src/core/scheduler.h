// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#pragma once

#include <windows.h>

// Boosts process priority, opts out of Windows 11 EcoQoS/timer throttling, and requests 0.5ms kernel timer resolution.
void SchedulerInitProcess();
void SchedulerShutdownProcess();

// Registers the calling thread with Windows MMCSS ("Games" profile, AVRT_PRIORITY_HIGH) for rock-solid 0.1% lows.
HANDLE SchedulerRegisterGamesThread();
void   SchedulerUnregisterGamesThread(HANDLE mmcssHandle);
