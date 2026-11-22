// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#pragma once

#include <d3d9.h>

typedef IDirect3D9 *(WINAPI *Direct3DCreate9_t)(UINT);
typedef BOOL(WINAPI *SetCursorPos_t)(int, int);

IDirect3D9 *CreateProxyDirect3D9(IDirect3D9Ex *realEx, Direct3DCreate9_t fallbackCreate9, UINT sdkVersion);
void        SetRawCursorPosFn(SetCursorPos_t fn);
HWND        GetActiveGameWindow();
bool        IsGameWindowDeactivating();
void        NotifyInputSample();
