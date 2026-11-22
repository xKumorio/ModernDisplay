// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "samp.h"
#include "log.h"

#include <windows.h>

namespace {

struct SampOffsets {
	const char *name;
	DWORD entryPoint;
	DWORD netGame;
	DWORD chat;
	DWORD chatAddMessage;
	int   netStateOff;
	int   netConnectedVal;
};

const SampOffsets kVersions[] = {
	{ "0.3.7-R1",   0x31DF13, 0x21A0F8, 0x21A0E4, 0x645A0, 0x3BD, 14 },
	{ "0.3.7-R2",   0x3195DD, 0x21A100, 0x21A0EC, 0x64670, 0x3B9, 14 },
	{ "0.3.7-R3-1", 0x0CC4D0, 0x26E8DC, 0x26E8C8, 0x679F0, 0x3CD,  5 },
	{ "0.3.7-R4",   0x0CBCB0, 0x26EA0C, 0x26E9F8, 0x68130, 0x3CD,  5 },
	{ "0.3.7-R5-1", 0x0CBC90, 0x26EB94, 0x26EB80, 0x68170, 0x3CD,  5 },
	{ "0.3.DL-R1",  0x0FDB60, 0x2ACA24, 0x2ACA10, 0x67BE0, 0x3CD,  5 },
};

BYTE              *g_base = nullptr;
const SampOffsets *g_ver = nullptr;
bool               g_checked = false;
DWORD              g_startTick = 0;
DWORD              g_lastCheckTick = 0;
bool               g_greeted = false;
DWORD              g_connectedAt = 0;

void IdentifySamp()
{
	if (g_checked) return;
	DWORD now = GetTickCount();
	if (!g_startTick) g_startTick = now;
	if (g_lastCheckTick && now - g_lastCheckTick < 500) return;
	g_lastCheckTick = now;

	HMODULE mod = GetModuleHandleA("samp.dll");
	if (!mod) {
		if (now - g_startTick > 30000) g_checked = true;
		return;
	}
	g_checked = true;

	BYTE *base = (BYTE *)mod;
	auto *dos = (IMAGE_DOS_HEADER *)base;
	auto *nt  = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
	DWORD ep  = nt->OptionalHeader.AddressOfEntryPoint;

	for (const auto &v : kVersions) {
		if (v.entryPoint == ep) {
			g_base = base;
			g_ver = &v;
			Log("SA-MP %s detected", v.name);
			return;
		}
	}
}

} // namespace

void SampTickGreeting()
{
	IdentifySamp();
	if (!g_base || !g_ver) return;

	BYTE *net = *(BYTE **)(g_base + g_ver->netGame);
	bool connected = net && *(int *)(net + g_ver->netStateOff) == g_ver->netConnectedVal;
	if (!connected) {
		g_greeted = false;
		g_connectedAt = 0;
		return;
	}
	if (g_greeted) return;

	DWORD now = GetTickCount();
	if (!g_connectedAt) g_connectedAt = now;
	if (now - g_connectedAt < 3000) return;

	void *chat = *(void **)(g_base + g_ver->chat);
	if (!chat) return;

	g_greeted = true;
	auto addMsg = (void(__thiscall *)(void *, DWORD, const char *))(g_base + g_ver->chatAddMessage);
	addMsg(chat, 0xFFFFFFFF, "{FF6EC7}<" MD_NAME "> {FFFFFF}by {FF6EC7}" MD_AUTHOR " {FFFFFF}loaded!");
}
