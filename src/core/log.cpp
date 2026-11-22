// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "log.h"

#include <share.h>
#include <stdarg.h>
#include <stdio.h>

HMODULE g_module = nullptr;
char    g_dir[MAX_PATH] = {};
wchar_t g_dirW[MAX_PATH] = {};

static CRITICAL_SECTION g_logCs;
static bool g_logReady = false;
static FILE *g_logFile = nullptr;

void LogInit()
{
	if (!g_logReady) {
		InitializeCriticalSection(&g_logCs);
		g_logReady = true;
	}
	char path[MAX_PATH];
	sprintf_s(path, "%s\\" MD_NAME ".log", g_dir);
	g_logFile = _fsopen(path, "w", _SH_DENYWR);
}

void Log(const char *fmt, ...)
{
	if (!g_logReady || !g_logFile) return;

	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
	va_end(ap);

	SYSTEMTIME st;
	GetLocalTime(&st);

	EnterCriticalSection(&g_logCs);
	fprintf(g_logFile, "[%02u:%02u:%02u.%03u] %s\n",
	        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, msg);
	fflush(g_logFile);
	LeaveCriticalSection(&g_logCs);
}
