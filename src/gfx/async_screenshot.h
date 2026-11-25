// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#pragma once

#include <windows.h>

// Lazily hooks d3dx9_25.dll!D3DXSaveSurfaceToFileA so SA-MP F8 screenshots
// return in <1 ms and encode PNG asynchronously via Windows Imaging Component (WIC).
void TryInstallAsyncScreenshotHook();

// Flushes any queued screenshots (call before device destruction, never inside DllMain).
void AsyncScreenshotFlush();
