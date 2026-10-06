// Conversion between real D3D12 pipeline descriptions and the portable PsoDesc, and replay of a
// PsoDesc on a device (which is what puts the compiled pipeline into the driver's shader cache).
#pragma once

#include "win/d3d12.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include "core/psodb.hpp"

namespace sp::win {

// Stores a blob (shader bytecode / serialized root signature) and returns its hash.
using BlobSink = std::function<uint64_t(BlobKind kind, const void* data, size_t size)>;
// Maps a live root signature object to the hash of the blob it was created from (0 if unknown).
using RootSigLookup = std::function<uint64_t(ID3D12RootSignature* rs)>;

bool capture_graphics(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, const BlobSink& sink,
                      const RootSigLookup& rootSigs, PsoDesc& out, std::string* why = nullptr);
bool capture_compute(const D3D12_COMPUTE_PIPELINE_STATE_DESC& desc, const BlobSink& sink,
                     const RootSigLookup& rootSigs, PsoDesc& out, std::string* why = nullptr);
bool capture_stream(const D3D12_PIPELINE_STATE_STREAM_DESC& desc, const BlobSink& sink,
                    const RootSigLookup& rootSigs, PsoDesc& out, std::string* why = nullptr);

// Root signatures created from blobs for replay, shared by all warm worker threads.
class RootSigCache {
public:
    ~RootSigCache() { clear(); }
    ID3D12RootSignature* get(ID3D12Device* device, uint64_t hash, const PsoDb& blobs);
    void clear();

private:
    std::mutex mu_;
    std::unordered_map<uint64_t, ID3D12RootSignature*> cache_;
    std::unordered_map<uint64_t, bool> failed_;
};

// Creates (and immediately releases) the pipeline. Uses the same entry point the game used.
HRESULT replay(ID3D12Device* device, const PsoDesc& desc, const PsoDb& blobs, RootSigCache& rootSigs,
               ID3D12PipelineState** keep = nullptr);

}  // namespace sp::win
