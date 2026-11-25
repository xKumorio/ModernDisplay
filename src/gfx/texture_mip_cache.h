#pragma once

#include <d3d9.h>
#include <cstdint>
#include <vector>

UINT DxtBlockBytes(D3DFORMAT format);
size_t DxtLevelBytes(D3DFORMAT format, UINT width, UINT height);
bool LoadDxtMipChain(D3DFORMAT format, UINT width, UINT height, int pitch,
                     const std::vector<uint8_t> &base, std::vector<uint8_t> &chain);
void BuildDxtMipChain(D3DFORMAT format, UINT width, UINT height, int pitch,
                      const std::vector<uint8_t> &base, std::vector<uint8_t> pixels,
                      std::vector<uint8_t> &chain);
