/* COM vtable slot numbers of the D3D12 methods shaderprep hooks. Checked at compile time against
 * the C vtable structs in d3d12.h by vtable_check.c, so a header update can't silently shift them. */
#pragma once

/* ID3D12Device */
#define SP_VT_DEVICE_CREATE_GRAPHICS_PSO 10
#define SP_VT_DEVICE_CREATE_COMPUTE_PSO 11
#define SP_VT_DEVICE_CREATE_ROOT_SIGNATURE 16
#define SP_VT_DEVICE_GET_ADAPTER_LUID 43
/* ID3D12Device1 */
#define SP_VT_DEVICE1_CREATE_PIPELINE_LIBRARY 44
/* ID3D12Device2 */
#define SP_VT_DEVICE2_CREATE_PIPELINE_STATE 47
/* ID3D12PipelineLibrary / ID3D12PipelineLibrary1 */
#define SP_VT_PIPELINE_LIBRARY_LOAD_GRAPHICS 9
#define SP_VT_PIPELINE_LIBRARY_LOAD_COMPUTE 10
#define SP_VT_PIPELINE_LIBRARY1_LOAD_PIPELINE 13
/* ID3D12DeviceFactory */
#define SP_VT_DEVICE_FACTORY_CREATE_DEVICE 9
