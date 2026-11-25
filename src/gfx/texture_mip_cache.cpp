#include "texture_mip_cache.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace {
struct Pixel { uint8_t b, g, r, a; };

uint16_t Pack565(Pixel p)
{
	return (uint16_t)(((p.r * 31 + 127) / 255) << 11 | ((p.g * 63 + 127) / 255) << 5 | ((p.b * 31 + 127) / 255));
}

Pixel Expand565(uint16_t value)
{
	const int r = (value >> 11) & 31, g = (value >> 5) & 63, b = value & 31;
	return { (uint8_t)((b << 3) | (b >> 2)), (uint8_t)((g << 2) | (g >> 4)),
	         (uint8_t)((r << 3) | (r >> 2)), 255 };
}

Pixel Blend(Pixel a, Pixel b, int x, int y)
{
	return { (uint8_t)((a.b * x + b.b * y) / (x + y)), (uint8_t)((a.g * x + b.g * y) / (x + y)),
	         (uint8_t)((a.r * x + b.r * y) / (x + y)), 255 };
}

int ColorError(Pixel a, Pixel b)
{
	const int dr = (int)a.r - b.r, dg = (int)a.g - b.g, db = (int)a.b - b.b;
	return dr * dr * 2 + dg * dg * 4 + db * db;
}

void EncodeColor(const Pixel (&pixels)[16], bool bc1, uint8_t *out)
{
	const bool transparent = bc1 && std::any_of(pixels, pixels + 16, [](Pixel p) { return p.a < 128; });
	const auto use = [transparent](Pixel p) { return !transparent || p.a >= 128; };
	Pixel low = {}, high = {};
	int longest = -1;
	for (int a = 0; a < 16; ++a) {
		if (!use(pixels[a])) continue;
		for (int b = 0; b <= a; ++b) {
			if (!use(pixels[b])) continue;
			const int score = ColorError(pixels[a], pixels[b]);
			if (score <= longest) continue;
			longest = score;
			low = pixels[a];
			high = pixels[b];
		}
	}
	uint16_t a = Pack565(low), b = Pack565(high);
	if ((transparent && a > b) || (!transparent && a < b)) std::swap(a, b);
	const bool three = bc1 && a <= b;
	const Pixel p0 = Expand565(a), p1 = Expand565(b);
	const Pixel palette[] = { p0, p1, three ? Blend(p0, p1, 1, 1) : Blend(p0, p1, 2, 1), Blend(p0, p1, 1, 2) };
	uint32_t indices = 0;
	for (int i = 0; i < 16; ++i) {
		int best = 3;
		if (use(pixels[i])) {
			int error = 0x7fffffff;
			for (int j = 0; j < (three ? 3 : 4); ++j) {
				const int cost = ColorError(pixels[i], palette[j]);
				if (cost < error) { best = j; error = cost; }
			}
		}
		indices |= (uint32_t)best << (2 * i);
	}
	std::memcpy(out, &a, sizeof(a));
	std::memcpy(out + 2, &b, sizeof(b));
	std::memcpy(out + 4, &indices, sizeof(indices));
}

void EncodeBlock(const Pixel (&pixels)[16], D3DFORMAT fmt, uint8_t *out)
{
	if (fmt == D3DFMT_DXT3) {
		for (int i = 0; i < 8; ++i)
			out[i] = (uint8_t)(((pixels[i * 2].a + 8) / 17) | ((pixels[i * 2 + 1].a + 8) / 17) << 4);
	} else if (fmt == D3DFMT_DXT5) {
		int low = 255, high = 0;
		for (Pixel p : pixels) { low = (std::min)(low, (int)p.a); high = (std::max)(high, (int)p.a); }
		out[0] = (uint8_t)high;
		out[1] = (uint8_t)low;
		uint64_t bits = 0;
		for (int i = 0; i < 16; ++i) {
			int best = 0, error = 256;
			for (int j = 0; j < 8; ++j) {
				const int value = j == 0 ? high : j == 1 ? low : (high * (8 - j) + low * (j - 1)) / 7;
				const int cost = std::abs(value - pixels[i].a);
				if (cost < error) { best = j; error = cost; }
			}
			bits |= (uint64_t)best << (3 * i);
		}
		std::memcpy(out + 2, &bits, 6);
	}
	EncodeColor(pixels, fmt == D3DFMT_DXT1, out + (fmt == D3DFMT_DXT1 ? 0 : 8));
}

void Downsample(const std::vector<Pixel> &source, UINT w, UINT h, std::vector<Pixel> &target, UINT outW, UINT outH)
{
	target.resize((size_t)outW * outH);
	for (UINT y = 0; y < outH; ++y) {
		for (UINT x = 0; x < outW; ++x) {
			int b = 0, g = 0, r = 0, a = 0;
			for (UINT sy = y * 2; sy < y * 2 + 2; ++sy) {
				for (UINT sx = x * 2; sx < x * 2 + 2; ++sx) {
					const Pixel p = source[(size_t)(std::min)(sy, h - 1) * w + (std::min)(sx, w - 1)];
					b += p.b * p.a;
					g += p.g * p.a;
					r += p.r * p.a;
					a += p.a;
				}
			}
			const int total = (std::max)(a, 1);
			target[(size_t)y * outW + x] = { (uint8_t)(b / total), (uint8_t)(g / total), (uint8_t)(r / total), (uint8_t)((a + 2) / 4) };
		}
	}
}

struct AlphaCutoff { int value; float tiedCoverage; };

AlphaCutoff FitAlphaCoverage(const std::vector<Pixel> &level, float expectedCoverage)
{
	size_t histogram[256] = {};
	for (Pixel p : level) ++histogram[p.a];
	float wanted = expectedCoverage * level.size();
	for (int i = 255; i >= 1; --i) {
		if (histogram[i] == 0) continue;
		if (wanted <= histogram[i]) return { i, wanted / histogram[i] };
		wanted -= histogram[i];
	}
	return { 1, 1.0f };
}

uint64_t Hash(const uint8_t *p, size_t len, uint64_t seed)
{
	while (len-- != 0) seed = (seed ^ *p++) * 1099511628211ULL;
	return seed;
}

std::filesystem::path CachePath(D3DFORMAT fmt, UINT w, UINT h, int pitch, const std::vector<uint8_t> &base)
{
	static const std::filesystem::path cacheDir = [] {
		HMODULE module = nullptr;
		wchar_t path[MAX_PATH] = {};
		const auto address = reinterpret_cast<LPCWSTR>(&DxtBlockBytes);
		if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                        address, &module)) return std::filesystem::path{};
		const DWORD len = GetModuleFileNameW(module, path, ARRAYSIZE(path));
		if (!len || len >= ARRAYSIZE(path)) return std::filesystem::path{};
		return std::filesystem::path(path).parent_path() / L"ModernDisplay" / L"mip-cache-v2";
	}();
	if (cacheDir.empty()) return {};
	const size_t rowBytes = ((w + 3) / 4) * DxtBlockBytes(fmt);
	uint64_t digest = 14695981039346656037ULL;
	for (UINT row = 0; row < (h + 3) / 4; ++row)
		digest = Hash(base.data() + (size_t)row * pitch, rowBytes, digest);
	wchar_t name[96];
	swprintf_s(name, L"%08x-%u-%u-%016llx.bin", (unsigned)fmt, w, h, digest);
	return cacheDir / name;
}

void AppendBase(D3DFORMAT fmt, UINT w, UINT h, int pitch, const std::vector<uint8_t> &base, std::vector<uint8_t> &chain)
{
	const size_t rowBytes = ((w + 3) / 4) * DxtBlockBytes(fmt);
	chain.resize(DxtLevelBytes(fmt, w, h));
	for (UINT y = 0; y < (h + 3) / 4; ++y)
		std::memcpy(chain.data() + y * rowBytes, base.data() + (size_t)y * pitch, rowBytes);
}

size_t MipChainBytes(D3DFORMAT fmt, UINT w, UINT h)
{
	size_t total = 0;
	for (;;) {
		total += DxtLevelBytes(fmt, w, h);
		if (w == 1 && h == 1) return total;
		w = (std::max)(1u, w / 2);
		h = (std::max)(1u, h / 2);
	}
}

void StoreChain(const std::filesystem::path &path, const std::vector<uint8_t> &chain)
{
	if (path.empty()) return;
	constexpr uintmax_t maxCacheBytes = 256ull * 1024 * 1024;
	static std::mutex mutex;
	std::scoped_lock lock(mutex);
	std::error_code error;
	std::filesystem::create_directories(path.parent_path(), error);
	if (error) return;
	static uintmax_t cacheBytes = 0;
	static bool sized = false;
	if (!sized) {
		uintmax_t existing = 0;
		for (std::filesystem::directory_iterator it(path.parent_path(), error), end; !error && it != end; it.increment(error)) {
			if (it->is_regular_file(error) && it->path().extension() == L".bin")
				existing += it->file_size(error);
		}
		if (error) return;
		cacheBytes = existing;
		sized = true;
	}
	uintmax_t previousBytes = 0;
	if (std::filesystem::exists(path, error)) previousBytes = std::filesystem::file_size(path, error);
	if (error || cacheBytes - (std::min)(previousBytes, cacheBytes) + chain.size() > maxCacheBytes) return;
	std::filesystem::path temp = path;
	temp += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
	std::ofstream out(temp, std::ios::binary | std::ios::trunc);
	out.write(reinterpret_cast<const char *>(chain.data()), chain.size());
	out.close();
	if (out && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
		cacheBytes = cacheBytes - (std::min)(previousBytes, cacheBytes) + chain.size();
	else std::filesystem::remove(temp, error);
}
}

UINT DxtBlockBytes(D3DFORMAT format) { return format == D3DFMT_DXT1 ? 8u : 16u; }

size_t DxtLevelBytes(D3DFORMAT fmt, UINT w, UINT h)
{
	return (size_t)((w + 3) / 4) * ((h + 3) / 4) * DxtBlockBytes(fmt);
}

bool LoadDxtMipChain(D3DFORMAT fmt, UINT w, UINT h, int pitch, const std::vector<uint8_t> &base, std::vector<uint8_t> &chain)
{
	const auto path = CachePath(fmt, w, h, pitch, base);
	if (path.empty()) return false;
	std::ifstream in(path, std::ios::binary | std::ios::ate);
	const size_t size = MipChainBytes(fmt, w, h);
	if (!in || in.tellg() != (std::streamoff)size) return false;
	std::vector<uint8_t> copy(size);
	in.seekg(0);
	if (!in.read(reinterpret_cast<char *>(copy.data()), size)) return false;
	AppendBase(fmt, w, h, pitch, base, chain);
	if (!std::equal(chain.begin(), chain.end(), copy.begin())) { chain.clear(); return false; }
	chain = std::move(copy);
	return true;
}

void BuildDxtMipChain(D3DFORMAT fmt, UINT w, UINT h, int pitch, const std::vector<uint8_t> &base,
                      std::vector<uint8_t> bgra, std::vector<uint8_t> &chain)
{
	const auto path = CachePath(fmt, w, h, pitch, base);
	std::vector<Pixel> pixels((size_t)w * h), next;
	std::memcpy(pixels.data(), bgra.data(), bgra.size());
	const float alphaCoverage = fmt == D3DFMT_DXT1 ?
	    (float)std::count_if(pixels.begin(), pixels.end(), [](Pixel p) { return p.a >= 128; }) / pixels.size() : 1.0f;
	chain.reserve(MipChainBytes(fmt, w, h));
	AppendBase(fmt, w, h, pitch, base, chain);
	while (w > 1 || h > 1) {
		const UINT dstW = (std::max)(1u, w / 2), dstH = (std::max)(1u, h / 2);
		Downsample(pixels, w, h, next, dstW, dstH);
		w = dstW;
		h = dstH;
		const AlphaCutoff cutoff = (alphaCoverage > 0.0f && alphaCoverage < 1.0f) ?
		                          FitAlphaCoverage(next, alphaCoverage) : AlphaCutoff{ 128, 1.0f };
		auto offset = chain.size();
		chain.resize(offset + DxtLevelBytes(fmt, w, h));
		for (UINT by = 0; by < h; by += 4) {
			for (UINT bx = 0; bx < w; bx += 4) {
				Pixel block[16];
			for (UINT y = 0; y < 4; ++y) {
				for (UINT x = 0; x < 4; ++x) {
					const UINT px = (std::min)(bx + x, w - 1), py = (std::min)(by + y, h - 1);
					Pixel pixel = next[(size_t)py * w + px];
					if (fmt == D3DFMT_DXT1) {
						const uint32_t noise = ((px * 73856093u) ^ (py * 19349663u)) & 255u;
						const bool visible = pixel.a > cutoff.value ||
						                     (pixel.a == cutoff.value && (float)noise / 256.0f < cutoff.tiedCoverage);
						pixel.a = visible ? 255 : 0;
					}
					block[y * 4 + x] = pixel;
				}
			}
				EncodeBlock(block, fmt, chain.data() + offset);
				offset += DxtBlockBytes(fmt);
			}
		}
		pixels.swap(next);
	}
	StoreChain(path, chain);
}
