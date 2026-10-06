// Byte layouts of the few D3D12 POD structs the portable code needs to read or synthesise.
// These are fixed by the Windows x64 ABI; the Windows build static_asserts them against d3d12.h.
#pragma once

#include <cstdint>

namespace sp::pod {

// D3D12_RT_FORMAT_ARRAY { DXGI_FORMAT RTFormats[8]; UINT NumRenderTargets; }
constexpr uint32_t kRtFormatArraySize = 36;
constexpr uint32_t kRtFormatArrayCountOffset = 32;

// DXGI_SAMPLE_DESC { UINT Count; UINT Quality; }
constexpr uint32_t kSampleDescSize = 8;

// D3D12_BLEND_DESC / D3D12_RASTERIZER_DESC / D3D12_DEPTH_STENCIL_DESC
constexpr uint32_t kBlendDescSize = 328;
constexpr uint32_t kRasterizerDescSize = 44;
constexpr uint32_t kDepthStencilDescSize = 52;

// D3D12_PRIMITIVE_TOPOLOGY_TYPE
constexpr uint32_t kTopologyTriangle = 3;

// DXGI_FORMAT values the inference uses when it has to synthesise defaults.
constexpr uint32_t DXGI_FORMAT_UNKNOWN = 0;
constexpr uint32_t DXGI_FORMAT_R32G32B32A32_FLOAT = 2;
constexpr uint32_t DXGI_FORMAT_R32G32B32A32_UINT = 3;
constexpr uint32_t DXGI_FORMAT_R32G32B32A32_SINT = 4;
constexpr uint32_t DXGI_FORMAT_R32G32B32_FLOAT = 6;
constexpr uint32_t DXGI_FORMAT_R32G32B32_UINT = 7;
constexpr uint32_t DXGI_FORMAT_R32G32B32_SINT = 8;
constexpr uint32_t DXGI_FORMAT_R16G16B16A16_FLOAT = 10;
constexpr uint32_t DXGI_FORMAT_R32G32_FLOAT = 16;
constexpr uint32_t DXGI_FORMAT_R32G32_UINT = 17;
constexpr uint32_t DXGI_FORMAT_R32G32_SINT = 18;
constexpr uint32_t DXGI_FORMAT_R8G8B8A8_UNORM = 28;
constexpr uint32_t DXGI_FORMAT_D32_FLOAT = 40;
constexpr uint32_t DXGI_FORMAT_R32_FLOAT = 41;
constexpr uint32_t DXGI_FORMAT_R32_UINT = 42;
constexpr uint32_t DXGI_FORMAT_R32_SINT = 43;

}  // namespace sp::pod
