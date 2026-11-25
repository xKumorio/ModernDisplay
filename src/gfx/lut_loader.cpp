// ModernDisplay - Copyright (c) 2026 Softik Inquisitor (https://vk.ru/miurawr). All rights reserved.
#include "lut_loader.h"
#include "../core/log.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace {

constexpr UINT   kMinLutSize = 2;
constexpr UINT   kMaxLutSize = 65;
constexpr size_t kMaxFileBytes = 64u * 1024u * 1024u;

bool EndsWithCubeExt(const std::string &s)
{
	if (s.size() < 5) return false;
	return _stricmp(s.c_str() + (s.size() - 5), ".cube") == 0;
}

bool ContainsPathSep(const std::string &s)
{
	return s.find('\\') != std::string::npos || s.find('/') != std::string::npos;
}

std::string TrimAscii(const char *s)
{
	if (!s) return {};
	const char *b = s;
	while (*b == ' ' || *b == '\t' || *b == '\r' || *b == '\n') ++b;
	const char *e = b + strlen(b);
	while (e > b && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
	return std::string(b, static_cast<size_t>(e - b));
}

std::string JoinPath(const char *dir, const char *sub, const std::string &file)
{
	std::string p = dir ? dir : "";
	while (!p.empty() && (p.back() == '\\' || p.back() == '/'))
		p.pop_back();
	if (!p.empty()) p.push_back('\\');
	if (sub && sub[0]) {
		p += sub;
		p.push_back('\\');
	}
	p += file;
	return p;
}

bool FileExistsRegular(const std::string &path)
{
	DWORD attr = GetFileAttributesA(path.c_str());
	return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool ReadWholeFile(const std::string &path, std::string &bytes)
{
	FILE *f = nullptr;
	if (fopen_s(&f, path.c_str(), "rb") != 0 || !f)
		return false;

	if (_fseeki64(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	const __int64 sz64 = _ftelli64(f);
	if (sz64 <= 0 || static_cast<uint64_t>(sz64) > static_cast<uint64_t>(kMaxFileBytes)) {
		fclose(f);
		return false;
	}
	if (_fseeki64(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return false;
	}

	const size_t sz = static_cast<size_t>(sz64);
	bytes.resize(sz);
	const size_t got = fread(bytes.data(), 1, sz, f);
	fclose(f);
	return got == sz;
}

inline const char *SkipWs(const char *p, const char *end)
{
	while (p < end && (*p == ' ' || *p == '\t')) ++p;
	return p;
}

bool MatchKeyword(const char *p, const char *end, const char *kw, const char *&afterKw)
{
	const size_t kwLen = strlen(kw);
	if (static_cast<size_t>(end - p) < kwLen) return false;
	if (_strnicmp(p, kw, kwLen) != 0) return false;
	const char *next = p + kwLen;
	if (next < end && *next != ' ' && *next != '\t') return false;
	afterKw = SkipWs(next, end);
	return true;
}

bool ParseFloatToken(const char *&p, const char *end, float &outVal)
{
	p = SkipWs(p, end);
	if (p >= end) return false;

	const char *start = p;
	if (*start == '+' && (start + 1) < end && (isdigit(static_cast<unsigned char>(start[1])) || start[1] == '.'))
		++start;

	float v = 0.0f;
	auto res = std::from_chars(start, end, v);
	if (res.ec != std::errc{} || res.ptr == start) return false;
	if (res.ptr < end && *res.ptr != ' ' && *res.ptr != '\t') return false;
	if (!std::isfinite(v)) return false;

	p = res.ptr;
	outVal = v;
	return true;
}

bool ParseUintToken(const char *&p, const char *end, UINT &outVal)
{
	p = SkipWs(p, end);
	if (p >= end) return false;

	unsigned int v = 0;
	auto res = std::from_chars(p, end, v);
	if (res.ec != std::errc{} || res.ptr == p) return false;
	if (res.ptr < end && *res.ptr != ' ' && *res.ptr != '\t') return false;

	p = res.ptr;
	outVal = static_cast<UINT>(v);
	return true;
}

bool ParseThreeFloatsExact(const char *p, const char *end, float &r, float &g, float &b)
{
	if (!ParseFloatToken(p, end, r)) return false;
	if (!ParseFloatToken(p, end, g)) return false;
	if (!ParseFloatToken(p, end, b)) return false;
	p = SkipWs(p, end);
	return p == end;
}

inline uint16_t FloatToUnorm16(float v)
{
	if (v <= 0.0f) return 0u;
	if (v >= 1.0f) return 65535u;
	return static_cast<uint16_t>(v * 65535.0f + 0.5f);
}

bool ParseCubeLutBuffer(const std::string &buf, Lut3DData &out)
{
	const char *cur = buf.data();
	const char *bufEnd = cur + buf.size();

	// Skip UTF-8 BOM if present
	if (buf.size() >= 3 &&
	    static_cast<unsigned char>(cur[0]) == 0xEF &&
	    static_cast<unsigned char>(cur[1]) == 0xBB &&
	    static_cast<unsigned char>(cur[2]) == 0xBF) {
		cur += 3;
	}

	UINT  lutSize = 0;
	float domMin[3] = { 0.0f, 0.0f, 0.0f };
	float domMax[3] = { 1.0f, 1.0f, 1.0f };
	float invRange[3] = { 1.0f, 1.0f, 1.0f };
	bool  dataStarted = false;
	size_t expectedTexels = 0;
	size_t readTexels = 0;

	std::vector<uint16_t> rgba16;

	while (cur < bufEnd) {
		const char *lineStart = cur;
		const char *lineEnd = static_cast<const char *>(memchr(cur, '\n', static_cast<size_t>(bufEnd - cur)));
		if (lineEnd) {
			cur = lineEnd + 1;
		} else {
			lineEnd = bufEnd;
			cur = bufEnd;
		}
		if (lineEnd > lineStart && lineEnd[-1] == '\r')
			--lineEnd;

		// Strip '#' comments
		const char *hash = static_cast<const char *>(memchr(lineStart, '#', static_cast<size_t>(lineEnd - lineStart)));
		if (hash) lineEnd = hash;

		// Trim spaces and tabs
		lineStart = SkipWs(lineStart, lineEnd);
		while (lineEnd > lineStart && (lineEnd[-1] == ' ' || lineEnd[-1] == '\t'))
			--lineEnd;

		if (lineStart >= lineEnd)
			continue;

		const char *afterKw = nullptr;
		if (MatchKeyword(lineStart, lineEnd, "TITLE", afterKw)) {
			if (dataStarted) return false;
			continue;
		}
		if (MatchKeyword(lineStart, lineEnd, "LUT_1D_SIZE", afterKw)) {
			if (dataStarted) return false;
			continue;
		}
		if (MatchKeyword(lineStart, lineEnd, "LUT_3D_SIZE", afterKw)) {
			if (dataStarted || lutSize != 0) return false;
			UINT n = 0;
			const char *p = afterKw;
			if (!ParseUintToken(p, lineEnd, n)) return false;
			if (SkipWs(p, lineEnd) != lineEnd) return false;
			if (n < kMinLutSize || n > kMaxLutSize) return false;
			lutSize = n;
			expectedTexels = static_cast<size_t>(n) * static_cast<size_t>(n) * static_cast<size_t>(n);
			continue;
		}
		if (MatchKeyword(lineStart, lineEnd, "DOMAIN_MIN", afterKw)) {
			if (dataStarted) return false;
			if (!ParseThreeFloatsExact(afterKw, lineEnd, domMin[0], domMin[1], domMin[2]))
				return false;
			continue;
		}
		if (MatchKeyword(lineStart, lineEnd, "DOMAIN_MAX", afterKw)) {
			if (dataStarted) return false;
			if (!ParseThreeFloatsExact(afterKw, lineEnd, domMax[0], domMax[1], domMax[2]))
				return false;
			continue;
		}

		// Data row: R G B float triplet
		if (lutSize == 0)
			return false;

		if (!dataStarted) {
			for (int c = 0; c < 3; ++c) {
				const float range = domMax[c] - domMin[c];
				if (!(range > 1e-6f) || !std::isfinite(range))
					return false;
				invRange[c] = 1.0f / range;
			}
			rgba16.resize(expectedTexels * 4u);
			dataStarted = true;
		}

		if (readTexels >= expectedTexels)
			return false;

		float r = 0.0f, g = 0.0f, b = 0.0f;
		if (!ParseThreeFloatsExact(lineStart, lineEnd, r, g, b))
			return false;

		const size_t base = readTexels * 4u;
		rgba16[base + 0u] = FloatToUnorm16((r - domMin[0]) * invRange[0]);
		rgba16[base + 1u] = FloatToUnorm16((g - domMin[1]) * invRange[1]);
		rgba16[base + 2u] = FloatToUnorm16((b - domMin[2]) * invRange[2]);
		rgba16[base + 3u] = 65535u;
		++readTexels;
	}

	if (lutSize == 0 || readTexels != expectedTexels)
		return false;

	out.size = lutSize;
	out.rgba16 = std::move(rgba16);
	return true;
}

void ScanCubeDir(const std::string &dirPath, std::vector<std::string> &names)
{
	const std::string pattern = dirPath + "\\*.cube";
	WIN32_FIND_DATAA fd = {};
	HANDLE hFind = FindFirstFileA(pattern.c_str(), &fd);
	if (hFind == INVALID_HANDLE_VALUE)
		return;

	do {
		if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
			continue;

		std::string fn = fd.cFileName;
		if (!EndsWithCubeExt(fn))
			continue;

		std::string base = fn.substr(0, fn.size() - 5u);
		if (base.empty())
			continue;

		bool exists = false;
		for (const auto &cur : names) {
			if (_stricmp(cur.c_str(), base.c_str()) == 0) {
				exists = true;
				break;
			}
		}
		if (!exists)
			names.push_back(std::move(base));
	} while (FindNextFileA(hFind, &fd));

	FindClose(hFind);
}

} // namespace

bool LoadCubeLutFile(const char *lutNameOrPath, Lut3DData &out, std::string *resolvedPathOut)
{
	out.size = 0;
	out.rgba16.clear();
	if (resolvedPathOut)
		resolvedPathOut->clear();

	std::string name = TrimAscii(lutNameOrPath);
	if (name.empty())
		return false;

	std::vector<std::string> candidates;
	if (ContainsPathSep(name)) {
		candidates.push_back(name);
	} else {
		const bool hasExt = EndsWithCubeExt(name);
		const std::string withExt = hasExt ? name : (name + ".cube");
		if (hasExt) {
			candidates.push_back(JoinPath(g_dir, "ModernDisplay\\luts", name));
			candidates.push_back(JoinPath(g_dir, "luts", name));
		} else {
			candidates.push_back(JoinPath(g_dir, "ModernDisplay\\luts", withExt));
			candidates.push_back(JoinPath(g_dir, "luts", withExt));
		}
	}

	std::string chosenPath;
	for (const auto &cand : candidates) {
		if (FileExistsRegular(cand)) {
			chosenPath = cand;
			break;
		}
	}
	if (chosenPath.empty())
		return false;

	std::string fileBytes;
	if (!ReadWholeFile(chosenPath, fileBytes))
		return false;

	if (!ParseCubeLutBuffer(fileBytes, out))
		return false;

	if (resolvedPathOut)
		*resolvedPathOut = chosenPath;

	return true;
}

std::vector<std::string> ListAvailableCubeLuts()
{
	std::vector<std::string> names;
	std::string baseDir = g_dir;
	while (!baseDir.empty() && (baseDir.back() == '\\' || baseDir.back() == '/'))
		baseDir.pop_back();

	if (!baseDir.empty()) {
		ScanCubeDir(baseDir + "\\ModernDisplay\\luts", names);
		ScanCubeDir(baseDir + "\\luts", names);
	} else {
		ScanCubeDir("ModernDisplay\\luts", names);
		ScanCubeDir("luts", names);
	}

	std::sort(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
		return _stricmp(a.c_str(), b.c_str()) < 0;
	});
	return names;
}
