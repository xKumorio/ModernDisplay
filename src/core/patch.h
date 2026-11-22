// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#pragma once

#include <windows.h>
#include <stdint.h>

bool PatchBytes(void *addr, const void *data, size_t n);
bool PatchIat(HMODULE mod, const char *dll, const char *fn, void *hook, void **orig);
bool HookHotpatchFn(void *target, void *detour, void **trampolineOut);
bool HookCallSite(DWORD siteAddr, void *dest, DWORD *origTargetOut);
