/* Compile-time verification of vtable_index.h against the C interface definitions in d3d12.h. */
#define COBJMACROS
#define CINTERFACE
#include <stddef.h>
#include <windows.h>
#include <directx/d3d12.h>

#include "vtable_index.h"

#define SLOT(vtbl, method) (offsetof(vtbl, method) / sizeof(void*))

_Static_assert(SLOT(ID3D12DeviceVtbl, CreateGraphicsPipelineState) == SP_VT_DEVICE_CREATE_GRAPHICS_PSO, "vtable");
_Static_assert(SLOT(ID3D12DeviceVtbl, CreateComputePipelineState) == SP_VT_DEVICE_CREATE_COMPUTE_PSO, "vtable");
_Static_assert(SLOT(ID3D12DeviceVtbl, CreateRootSignature) == SP_VT_DEVICE_CREATE_ROOT_SIGNATURE, "vtable");
_Static_assert(SLOT(ID3D12DeviceVtbl, GetAdapterLuid) == SP_VT_DEVICE_GET_ADAPTER_LUID, "vtable");
_Static_assert(SLOT(ID3D12Device1Vtbl, CreatePipelineLibrary) == SP_VT_DEVICE1_CREATE_PIPELINE_LIBRARY, "vtable");
_Static_assert(SLOT(ID3D12Device2Vtbl, CreatePipelineState) == SP_VT_DEVICE2_CREATE_PIPELINE_STATE, "vtable");
_Static_assert(SLOT(ID3D12PipelineLibraryVtbl, LoadGraphicsPipeline) == SP_VT_PIPELINE_LIBRARY_LOAD_GRAPHICS, "vtable");
_Static_assert(SLOT(ID3D12PipelineLibraryVtbl, LoadComputePipeline) == SP_VT_PIPELINE_LIBRARY_LOAD_COMPUTE, "vtable");
_Static_assert(SLOT(ID3D12PipelineLibrary1Vtbl, LoadPipeline) == SP_VT_PIPELINE_LIBRARY1_LOAD_PIPELINE, "vtable");
_Static_assert(SLOT(ID3D12DeviceFactoryVtbl, CreateDevice) == SP_VT_DEVICE_FACTORY_CREATE_DEVICE, "vtable");

int sp_vtable_check_dummy;
