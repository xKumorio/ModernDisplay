// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "patch.h"

#include <string.h>

bool PatchBytes(void *addr, const void *data, size_t n)
{
	DWORD oldProt = 0;
	if (!VirtualProtect(addr, n, PAGE_EXECUTE_READWRITE, &oldProt))
		return false;
	memcpy(addr, data, n);
	FlushInstructionCache(GetCurrentProcess(), addr, n);
	DWORD dummy = 0;
	VirtualProtect(addr, n, oldProt, &dummy);
	return true;
}

bool PatchIat(HMODULE mod, const char *dll, const char *fn, void *hook, void **orig)
{
	if (!mod) return false;
	BYTE *base = (BYTE *)mod;
	auto *dos = (IMAGE_DOS_HEADER *)base;
	if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
	auto *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	if (nt->Signature != IMAGE_NT_SIGNATURE) return false;

	auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress) return false;

	for (auto *imp = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); imp->Name; ++imp) {
		const char *name = (const char *)(base + imp->Name);
		if (_stricmp(name, dll) != 0) continue;

		auto *oft = (IMAGE_THUNK_DATA *)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
		auto *ft  = (IMAGE_THUNK_DATA *)(base + imp->FirstThunk);
		for (; oft->u1.AddressOfData; ++oft, ++ft) {
			if (IMAGE_SNAP_BY_ORDINAL(oft->u1.Ordinal)) continue;
			auto *byName = (IMAGE_IMPORT_BY_NAME *)(base + oft->u1.AddressOfData);
			if (strcmp((const char *)byName->Name, fn) != 0) continue;

			if (orig) *orig = (void *)(uintptr_t)ft->u1.Function;
			DWORD val = (DWORD)(uintptr_t)hook;
			return PatchBytes(&ft->u1.Function, &val, sizeof(val));
		}
	}
	return false;
}

bool HookHotpatchFn(void *target, void *detour, void **trampolineOut)
{
	if (!target || !detour || !trampolineOut) return false;
	BYTE *p = (BYTE *)target;

	bool hasHotpatch = (p[0] == 0x8B && p[1] == 0xFF);
	if (hasHotpatch) {
		for (int i = -5; i < 0; ++i) {
			if (p[i] != 0xCC && p[i] != 0x90) {
				hasHotpatch = false;
				break;
			}
		}
	}
	if (hasHotpatch) {
		BYTE patch[7];
		patch[0] = 0xE9;
		int32_t rel = (int32_t)((uintptr_t)detour - (uintptr_t)p);
		memcpy(patch + 1, &rel, 4);
		patch[5] = 0xEB;
		patch[6] = 0xF9;
		if (!PatchBytes(p - 5, patch, 7)) return false;
		*trampolineOut = p + 2;
		return true;
	}

	if (p[0] == 0x8B && p[1] == 0xFF && p[2] == 0x55 && p[3] == 0x8B && p[4] == 0xEC) {
		BYTE *tramp = (BYTE *)VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
		if (!tramp) return false;
		memcpy(tramp, p, 5);
		tramp[5] = 0xE9;
		int32_t back = (int32_t)((uintptr_t)(p + 5) - (uintptr_t)(tramp + 10));
		memcpy(tramp + 6, &back, 4);
		FlushInstructionCache(GetCurrentProcess(), tramp, 10);

		BYTE jmp[5];
		jmp[0] = 0xE9;
		int32_t rel = (int32_t)((uintptr_t)detour - (uintptr_t)(p + 5));
		memcpy(jmp + 1, &rel, 4);
		if (!PatchBytes(p, jmp, 5)) {
			VirtualFree(tramp, 0, MEM_RELEASE);
			return false;
		}
		*trampolineOut = tramp;
		return true;
	}

	// Direct jmp [mem32] import thunk (e.g. Win11 SysWOW64 user32!SetCursorPos -> win32u)
	if (p[0] == 0xFF && p[1] == 0x25) {
		void **slot = nullptr;
		memcpy(&slot, p + 2, 4);
		if (!slot || !*slot) return false;
		*trampolineOut = *slot;
		BYTE jmp[6];
		jmp[0] = 0xE9;
		int32_t rel = (int32_t)((uintptr_t)detour - (uintptr_t)(p + 5));
		memcpy(jmp + 1, &rel, 4);
		jmp[5] = 0x90;
		return PatchBytes(p, jmp, 6);
	}

	return false;
}

bool HookInlineFn(void *target, void *detour, void **trampolineOut)
{
	if (!target || !detour || !trampolineOut) return false;
	BYTE *p = (BYTE *)target;

	// Never overwrite an already-detoured entry point (would put us above the other mod).
	if (p[0] == 0xE9 || p[0] == 0xEB)
		return false;

	size_t copyLen = 0;
	if (p[0] == 0x8B && p[1] == 0xFF && p[2] == 0x55 && p[3] == 0x8B && p[4] == 0xEC) {
		copyLen = 5; // mov edi, edi; push ebp; mov ebp, esp
	} else if (p[0] == 0x6A && p[2] == 0xB8) {
		copyLen = 7; // push imm8; mov eax, imm32 (Win10/11 d3d9 SEH prolog)
	} else if (p[0] == 0x55 && p[1] == 0x8B && p[2] == 0xEC) {
		if (p[3] == 0x83 && (p[4] == 0xEC || p[4] == 0xE4))
			copyLen = 6; // push ebp; mov ebp, esp; sub/and esp, imm8
		else if (p[3] == 0x6A)
			copyLen = 5; // push ebp; mov ebp, esp; push imm8
		else if (p[3] >= 0x50 && p[3] <= 0x57 && p[4] >= 0x50 && p[4] <= 0x57)
			copyLen = 5; // push ebp; mov ebp, esp; push reg; push reg
	}
	if (copyLen < 5)
		return false;

	BYTE *tramp = (BYTE *)VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!tramp) return false;
	memcpy(tramp, p, copyLen);
	tramp[copyLen] = 0xE9;
	int32_t back = (int32_t)((uintptr_t)(p + copyLen) - (uintptr_t)(tramp + copyLen + 5));
	memcpy(tramp + copyLen + 1, &back, 4);
	FlushInstructionCache(GetCurrentProcess(), tramp, copyLen + 5);

	BYTE patch[16];
	memset(patch, 0x90, copyLen);
	patch[0] = 0xE9;
	int32_t rel = (int32_t)((uintptr_t)detour - (uintptr_t)(p + 5));
	memcpy(patch + 1, &rel, 4);
	if (!PatchBytes(p, patch, copyLen)) {
		VirtualFree(tramp, 0, MEM_RELEASE);
		return false;
	}
	*trampolineOut = tramp;
	return true;
}

bool HookCallSite(DWORD siteAddr, void *dest, DWORD *origTargetOut)
{
	BYTE *site = (BYTE *)(uintptr_t)siteAddr;
	if (site[0] != 0xE8) return false;
	int32_t oldRel = 0;
	memcpy(&oldRel, site + 1, 4);
	DWORD oldTarget = siteAddr + 5 + (DWORD)oldRel;
	int32_t newRel = (int32_t)((uintptr_t)dest - (uintptr_t)(site + 5));
	if (!PatchBytes(site + 1, &newRel, 4)) return false;
	if (origTargetOut) *origTargetOut = oldTarget;
	return true;
}
