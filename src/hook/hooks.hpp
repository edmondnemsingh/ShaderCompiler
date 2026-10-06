#pragma once

#include <windows.h>
#include <unknwn.h>

namespace sp::hook {

void set_module(HMODULE self);
// Called after the real D3D12CreateDevice / ID3D12DeviceFactory::CreateDevice succeeded.
void on_device_created(IUnknown* device);
// Called when the game obtains an ID3D12DeviceFactory (Agility SDK device creation path).
void on_device_factory(IUnknown* factory);

}  // namespace sp::hook
