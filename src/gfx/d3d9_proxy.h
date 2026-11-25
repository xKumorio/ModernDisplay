// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#pragma once

#include <d3d9.h>
#include "../core/config.h"

typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);
typedef BOOL(WINAPI *SetCursorPos_t)(int, int);
typedef BOOL(WINAPI *GetClientRect_t)(HWND, LPRECT);

IDirect3D9      *CreateProxyDirect3D9(IDirect3D9Ex *realEx, Direct3DCreate9_t fallbackCreate9, UINT sdkVersion);
void             InstallD3D9DetourBridgeFromDevice(IDirect3DDevice9 *realDev);
void             SetRawCursorPosFn(SetCursorPos_t fn);
void             SetRawGetClientRectFn(GetClientRect_t fn);
HWND             GetActiveGameWindow();
bool             GetActiveGameSize(UINT &gameW, UINT &gameH);
bool             NeedsVirtualClientMapping();
void             MapPhysicalClientToGame(LONG &x, LONG &y);
void             MapGameToPhysicalClient(LONG &x, LONG &y);
bool             IsGameWindowDeactivating();
void             NotifyInputSample();
void             NotifyBefore2dStuff();
using RenderRwObject = void *(__cdecl *)(void *);
void            *RenderPlayerClump(void *clump, RenderRwObject render);
void            *RenderEquippedAtomic(void *atomic, RenderRwObject render);
DisplayTelemetry GetDisplayTelemetry();
