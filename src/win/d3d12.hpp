// Single include point for D3D12 on Windows (MSVC and MinGW).
#pragma once

#include <windows.h>
#include <directx/d3d12.h>
// Defines __uuidof / IID_PPV_ARGS support for D3D12 interfaces on toolchains without MS intrinsics.
#include <dxguids/dxguids.h>
