// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>
#include <string.h>

#include "core/log.h"
#include "core/patch.h"
#include "gfx/d3d9_proxy.h"

typedef HRESULT(WINAPI *Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex **);
typedef HMODULE(WINAPI *LoadLibraryA_t)(LPCSTR);
typedef FARPROC(WINAPI *GetProcAddress_t)(HMODULE, LPCSTR);
typedef BOOL(WINAPI *ClipCursor_t)(const RECT *);

static bool IsWindows10OrNewer()
{
	typedef LONG(WINAPI *RtlGetVersion_t)(OSVERSIONINFOEXW *);
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	if (!ntdll) return false;
	auto fn = (RtlGetVersion_t)GetProcAddress(ntdll, "RtlGetVersion");
	if (!fn) return false;
	OSVERSIONINFOEXW vi = {};
	vi.dwOSVersionInfoSize = sizeof(vi);
	return fn(&vi) == 0 && vi.dwMajorVersion >= 10;
}

static Direct3DCreate9_t   g_origDirect3DCreate9 = nullptr;
static Direct3DCreate9Ex_t g_fnDirect3DCreate9Ex = nullptr;
static LoadLibraryA_t      g_origLoadLibraryA = nullptr;
static GetProcAddress_t    g_origGetProcAddress = nullptr;
static SetCursorPos_t      g_origSetCursorPos = nullptr;
static ClipCursor_t        g_origClipCursor = nullptr;
static bool                g_d3d9Hooked = false;
static bool                g_d3d9Incompatible = false;

static BOOL WINAPI hkSetCursorPos(int x, int y)
{
	HWND gw = GetActiveGameWindow();
	if (gw && (IsGameWindowDeactivating() || IsIconic(gw) || GetForegroundWindow() != gw))
		return TRUE;
	return g_origSetCursorPos ? g_origSetCursorPos(x, y) : TRUE;
}

static BOOL WINAPI hkClipCursor(const RECT *lpRect)
{
	HWND gw = GetActiveGameWindow();
	if (lpRect && gw) {
		bool degenerate = (lpRect->right - lpRect->left <= 4) || (lpRect->bottom - lpRect->top <= 4);
		if (degenerate || IsGameWindowDeactivating() || IsIconic(gw) || GetForegroundWindow() != gw)
			return g_origClipCursor ? g_origClipCursor(nullptr) : TRUE;
	}
	return g_origClipCursor ? g_origClipCursor(lpRect) : TRUE;
}

static DWORD g_origGameProcess = 0;

static void __cdecl hkGameProcess()
{
	NotifyInputSample();
	((void(__cdecl *)())g_origGameProcess)();
}

static bool IsSystemD3D9(HMODULE mod, wchar_t *pathOut, DWORD cap)
{
	if (!mod || !GetModuleFileNameW(mod, pathOut, cap)) return false;

	wchar_t sysDir[MAX_PATH] = {}, wowDir[MAX_PATH] = {};
	GetSystemDirectoryW(sysDir, MAX_PATH);
	GetSystemWow64DirectoryW(wowDir, MAX_PATH);

	auto startsWithI = [](const wchar_t *s, const wchar_t *prefix) -> bool {
		size_t n = wcslen(prefix);
		return n > 0 && _wcsnicmp(s, prefix, n) == 0 && (s[n] == L'\\' || s[n] == L'/');
	};
	return startsWithI(pathOut, sysDir) || startsWithI(pathOut, wowDir);
}

static IDirect3D9 *WINAPI hkDirect3DCreate9(UINT sdkVersion)
{
	if (g_fnDirect3DCreate9Ex && !g_d3d9Incompatible) {
		IDirect3D9Ex *d3d9Ex = nullptr;
		HRESULT hr = g_fnDirect3DCreate9Ex(sdkVersion, &d3d9Ex);
		if (SUCCEEDED(hr) && d3d9Ex) {
			Log("Hook: Direct3DCreate9(%u) upgraded to IDirect3D9Ex", sdkVersion);
			return CreateProxyDirect3D9(d3d9Ex, g_origDirect3DCreate9, sdkVersion);
		}
		Log("Hook: Direct3DCreate9Ex failed (0x%08lX) - falling back to native D3D9", hr);
	}
	return g_origDirect3DCreate9 ? g_origDirect3DCreate9(sdkVersion) : nullptr;
}

static void TryHookD3D9(HMODULE mod)
{
	if (g_d3d9Hooked || g_d3d9Incompatible || !mod) return;

	wchar_t path[MAX_PATH] = {};
	if (!IsSystemD3D9(mod, path, MAX_PATH)) {
		g_d3d9Incompatible = true;
		char utf8[MAX_PATH * 2] = {};
		WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), nullptr, nullptr);
		Log("Compatibility: non-system d3d9.dll detected (%s) - bridge disabled", utf8);
		return;
	}

	auto fn9   = (Direct3DCreate9_t)GetProcAddress(mod, "Direct3DCreate9");
	auto fn9Ex = (Direct3DCreate9Ex_t)GetProcAddress(mod, "Direct3DCreate9Ex");
	if (!fn9 || !fn9Ex) {
		g_d3d9Incompatible = true;
		Log("Compatibility: Direct3DCreate9Ex unavailable in d3d9.dll");
		return;
	}
	g_fnDirect3DCreate9Ex = fn9Ex;

	void *tramp = nullptr;
	if (HookHotpatchFn((void *)fn9, (void *)hkDirect3DCreate9, &tramp)) {
		g_origDirect3DCreate9 = (Direct3DCreate9_t)tramp;
		g_d3d9Hooked = true;
		Log("Hook: system d3d9!Direct3DCreate9 hooked");
	} else {
		g_origDirect3DCreate9 = fn9;
		g_d3d9Hooked = true;
		Log("Hook: using GetProcAddress hook for Direct3DCreate9");
	}
}

static HMODULE WINAPI hkLoadLibraryA(LPCSTR name)
{
	HMODULE mod = g_origLoadLibraryA(name);
	if (mod && name && !g_d3d9Hooked && !g_d3d9Incompatible) {
		const char *base = strrchr(name, '\\');
		base = base ? base + 1 : name;
		if (_stricmp(base, "d3d9.dll") == 0 || _stricmp(base, "d3d9") == 0)
			TryHookD3D9(mod);
	}
	return mod;
}

static FARPROC WINAPI hkGetProcAddress(HMODULE mod, LPCSTR proc)
{
	if (!g_d3d9Incompatible && proc && ((uintptr_t)proc >> 16) != 0 && strcmp(proc, "Direct3DCreate9") == 0) {
		TryHookD3D9(mod);
		if (g_d3d9Hooked && !g_d3d9Incompatible)
			return (FARPROC)hkDirect3DCreate9;
	}
	return g_origGetProcAddress(mod, proc);
}

static void HookGtaInputMarker()
{
	// GTA:SA 1.0 US: CGame::Process call in Idle (0x53E981 -> 0x53BEE0), right before CPad::UpdatePads.
	const DWORD kCallSite = 0x53E981;
	const DWORD kGameProcess = 0x53BEE0;
	BYTE *p = (BYTE *)(uintptr_t)kCallSite;
	if (p[0] == 0xE8) {
		int32_t rel = 0;
		memcpy(&rel, p + 1, 4);
		if ((DWORD)(kCallSite + 5 + rel) == kGameProcess) {
			if (HookCallSite(kCallSite, (void *)hkGameProcess, &g_origGameProcess))
				Log("Reflex: input marker hooked at 0x%08lX", kCallSite);
		}
	}
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(inst);
		g_module = inst;
		GetModuleFileNameA(inst, g_dir, MAX_PATH);
		if (char *s = strrchr(g_dir, '\\')) *s = 0;
		GetModuleFileNameW(inst, g_dirW, MAX_PATH);
		if (wchar_t *s = wcsrchr(g_dirW, L'\\')) *s = 0;

		LogInit();
		Log(MD_NAME " v" MD_VERSION " - (c) 2026 " MD_AUTHOR " (" MD_AUTHOR_URL ")");

		if (!IsWindows10OrNewer()) {
			g_d3d9Incompatible = true;
			Log("Compatibility: Windows 10 or newer is required for DXGI Flip Model - staying passive");
			return TRUE;
		}

		// Check game executable directory (works even if the .asi is inside a scripts/ subfolder).
		wchar_t exeDirW[MAX_PATH] = {};
		GetModuleFileNameW(nullptr, exeDirW, MAX_PATH);
		if (wchar_t *s = wcsrchr(exeDirW, L'\\')) *s = 0;
		wchar_t localD3D9[MAX_PATH];
		swprintf_s(localD3D9, L"%ls\\d3d9.dll", exeDirW);
		if (GetFileAttributesW(localD3D9) != INVALID_FILE_ATTRIBUTES) {
			g_d3d9Incompatible = true;
			Log("Compatibility: local d3d9.dll found in game folder (DXVK/ENB/ReShade) - staying passive");
			return TRUE;
		}

		HMODULE exe = GetModuleHandleA(nullptr);
		PatchIat(exe, "kernel32.dll", "LoadLibraryA", (void *)hkLoadLibraryA, (void **)&g_origLoadLibraryA);
		PatchIat(exe, "kernel32.dll", "GetProcAddress", (void *)hkGetProcAddress, (void **)&g_origGetProcAddress);
		if (HMODULE u32 = GetModuleHandleA("user32.dll")) {
			void *fn = (void *)GetProcAddress(u32, "SetCursorPos");
			void *tramp = nullptr;
			if (fn && HookHotpatchFn(fn, (void *)hkSetCursorPos, &tramp)) {
				g_origSetCursorPos = (SetCursorPos_t)tramp;
				SetRawCursorPosFn(g_origSetCursorPos);
			} else if (PatchIat(exe, "user32.dll", "SetCursorPos", (void *)hkSetCursorPos, (void **)&g_origSetCursorPos)) {
				SetRawCursorPosFn(g_origSetCursorPos);
			}
			void *fnClip = (void *)GetProcAddress(u32, "ClipCursor");
			void *trampClip = nullptr;
			if (fnClip && HookHotpatchFn(fnClip, (void *)hkClipCursor, &trampClip))
				g_origClipCursor = (ClipCursor_t)trampClip;
			else
				PatchIat(exe, "user32.dll", "ClipCursor", (void *)hkClipCursor, (void **)&g_origClipCursor);
		}
		if (HMODULE existing = GetModuleHandleA("d3d9.dll"))
			TryHookD3D9(existing);

		HookGtaInputMarker();
	}
	return TRUE;
}
