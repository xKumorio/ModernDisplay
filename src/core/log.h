// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#pragma once

#include <windows.h>

#define MD_NAME       "ModernDisplay"
#define MD_VERSION    "1.0"
#define MD_AUTHOR     "Softik Inquisitor"
#define MD_AUTHOR_URL "https://vk.ru/miurawr"

extern HMODULE g_module;
extern char    g_dir[MAX_PATH];
extern wchar_t g_dirW[MAX_PATH];

void LogInit();
void Log(const char *fmt, ...);
