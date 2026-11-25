// =============================================================================
// KROM Engine - tests/test_gi.cpp
// Tests for the surface cache GI series (doc/PLAN_GI.md).
//
// Package 0: the radiance cache inserts keys into a hash table with
// InterlockedCompareExchange on a RWStructuredBuffer<uint>. This test compiles
// such a compute shader for DX11 (FXC, cs_5_0) and Vulkan (DXC -spirv), runs
// the DX11 bytecode on WARP and checks the result, and checks that the SPIR-V
// contains OpAtomicCompareExchange.
// =============================================================================
#include "TestFramework.hpp"
#include "assets/AssetRegistry.hpp"
#include "core/Debug.hpp"
#include "renderer/ShaderCompiler.hpp"
#include "renderer/ShaderToolPaths.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32) && defined(KROM_DX11_BACKEND)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>
#include <wrl/client.h>
#endif

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr uint32_t kGiHashTableSize = 64u;
constexpr uint32_t kGiHashThreads   = 256u;   // 4 groups of 64
constexpr uint32_t kGiHashKeyCount  = 37u;    // distinct keys 1..37

// Open addressing with linear probing, as the cache will do it: claim an empty
// slot with CompareExchange, or find the slot that already holds the key, then
// count the thread there with InterlockedAdd.
const char* const kGiHashInsertCsHlsl = R"(
RWStructuredBuffer<uint> gKeys   : register(u0);
RWStructuredBuffer<uint> gCounts : register(u1);

static const uint kTableSize = 64u;
static const uint kEmpty     = 0u;

[numthreads(64, 1, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    const uint key = (dtid.x % 37u) + 1u;
    uint slot = (key * 2654435761u) & (kTableSize - 1u);
    [loop] for (uint probe = 0u; probe < kTableSize; ++probe)
    {
        uint previous = kEmpty;
        InterlockedCompareExchange(gKeys[slot], kEmpty, key, previous);
        if (previous == kEmpty || previous == key)
        {
            InterlockedAdd(gCounts[slot], 1u);
            return;
        }
        slot = (slot + 1u) & (kTableSize - 1u);
    }
}
)";

// CPU expectation: thread x inserts key (x % 37) + 1.
uint32_t ExpectedCountForKey(uint32_t key)
{
    uint32_t count = 0u;
    for (uint32_t x = 0u; x < kGiHashThreads; ++x)
        if ((x % kGiHashKeyCount) + 1u == key)
            ++count;
    return count;
}

// Checks a filled table: every key 1..37 exactly once, with the right count,
// and nothing else.
void CheckHashTable(test::TestContext& ctx, const uint32_t* keys, const uint32_t* counts)
{
    std::vector<uint32_t> seen(kGiHashKeyCount + 1u, 0u);
    uint32_t total = 0u;
    for (uint32_t i = 0u; i < kGiHashTableSize; ++i)
    {
        if (keys[i] == 0u)
        {
            CHECK_EQ(ctx, counts[i], 0u);
            continue;
        }
        CHECK(ctx, keys[i] >= 1u && keys[i] <= kGiHashKeyCount);
        if (keys[i] < 1u || keys[i] > kGiHashKeyCount)
            continue;
        ++seen[keys[i]];
        CHECK_EQ(ctx, counts[i], ExpectedCountForKey(keys[i]));
        total += counts[i];
    }
    for (uint32_t key = 1u; key <= kGiHashKeyCount; ++key)
        CHECK_EQ(ctx, seen[key], 1u);
    CHECK_EQ(ctx, total, kGiHashThreads);
}

assets::ShaderAsset MakeHashInsertShader()
{
    assets::ShaderAsset cs{};
    cs.debugName = "GiHashInsertCS";
    cs.stage = assets::ShaderStage::Compute;
    cs.sourceLanguage = assets::ShaderSourceLanguage::HLSL;
    cs.entryPoint = "main";
    cs.sourceCode = kGiHashInsertCsHlsl;
    return cs;
}

// True if the SPIR-V module contains an instruction with this opcode.
bool SpirvHasOpcode(const std::vector<uint8_t>& bytecode, uint16_t opcode)
{
    if (bytecode.size() < 20u || bytecode.size() % 4u != 0u)
        return false;
    std::vector<uint32_t> words(bytecode.size() / 4u);
    std::memcpy(words.data(), bytecode.data(), bytecode.size());
    if (words[0] != 0x07230203u)   // SPIR-V magic
        return false;
    size_t i = 5u;                 // after the header
    while (i < words.size())
    {
        const uint32_t wordCount = words[i] >> 16u;
        if ((words[i] & 0xFFFFu) == opcode)
            return true;
        if (wordCount == 0u)
            return false;
        i += wordCount;
    }
    return false;
}

void TestHashInsertCompareExchangeDx11(test::TestContext& ctx)
{
    const assets::ShaderAsset cs = MakeHashInsertShader();
    assets::CompiledShaderArtifact compiled{};
    std::string error;
    const bool ok = ShaderCompiler::CompileForTarget(cs, assets::ShaderTargetProfile::DirectX11_SM5, compiled, &error);
    if (!ok)
        std::fprintf(stderr, "test_gi.cpp: DX11 compile of GiHashInsertCS failed: %s\n", error.c_str());
    CHECK(ctx, ok);
    CHECK(ctx, !compiled.bytecode.empty());
    if (!ok || compiled.bytecode.empty())
        return;

#if defined(_WIN32) && defined(KROM_DX11_BACKEND)
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> command;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, nullptr, &command);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;

    ComPtr<ID3D11ComputeShader> shader;
    hr = device->CreateComputeShader(compiled.bytecode.data(), compiled.bytecode.size(), nullptr, &shader);
    CHECK(ctx, SUCCEEDED(hr));
    if (FAILED(hr))
        return;

    const auto makeBuffer = [&](ComPtr<ID3D11Buffer>& buffer, ComPtr<ID3D11UnorderedAccessView>& uav,
                                ComPtr<ID3D11Buffer>& staging) -> bool
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = kGiHashTableSize * sizeof(uint32_t);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(uint32_t);
        const std::vector<uint32_t> zeros(kGiHashTableSize, 0u);
        D3D11_SUBRESOURCE_DATA init{ zeros.data(), 0u, 0u };
        if (FAILED(device->CreateBuffer(&desc, &init, &buffer)))
            return false;
        D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
        uavDesc.Format = DXGI_FORMAT_UNKNOWN;
        uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        uavDesc.Buffer.NumElements = kGiHashTableSize;
        if (FAILED(device->CreateUnorderedAccessView(buffer.Get(), &uavDesc, &uav)))
            return false;
        D3D11_BUFFER_DESC stagingDesc = desc;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0u;
        stagingDesc.MiscFlags = 0u;
        stagingDesc.StructureByteStride = 0u;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(device->CreateBuffer(&stagingDesc, nullptr, &staging));
    };

    ComPtr<ID3D11Buffer> keys, counts, keysStaging, countsStaging;
    ComPtr<ID3D11UnorderedAccessView> keysUav, countsUav;
    const bool buffersOk = makeBuffer(keys, keysUav, keysStaging) && makeBuffer(counts, countsUav, countsStaging);
    CHECK(ctx, buffersOk);
    if (!buffersOk)
        return;

    ID3D11UnorderedAccessView* uavs[2] = { keysUav.Get(), countsUav.Get() };
    command->CSSetShader(shader.Get(), nullptr, 0u);
    command->CSSetUnorderedAccessViews(0u, 2u, uavs, nullptr);
    command->Dispatch(kGiHashThreads / 64u, 1u, 1u);
    ID3D11UnorderedAccessView* nullUavs[2] = {};
    command->CSSetUnorderedAccessViews(0u, 2u, nullUavs, nullptr);
    command->CopyResource(keysStaging.Get(), keys.Get());
    command->CopyResource(countsStaging.Get(), counts.Get());

    D3D11_MAPPED_SUBRESOURCE mappedKeys{}, mappedCounts{};
    const bool mapped = SUCCEEDED(command->Map(keysStaging.Get(), 0u, D3D11_MAP_READ, 0u, &mappedKeys)) &&
                        SUCCEEDED(command->Map(countsStaging.Get(), 0u, D3D11_MAP_READ, 0u, &mappedCounts));
    CHECK(ctx, mapped);
    if (mapped)
        CheckHashTable(ctx, static_cast<const uint32_t*>(mappedKeys.pData),
                       static_cast<const uint32_t*>(mappedCounts.pData));
    command->Unmap(keysStaging.Get(), 0u);
    command->Unmap(countsStaging.Get(), 0u);
#else
    std::fprintf(stdout, "  # test_gi.cpp: no DX11 backend, WARP run skipped\n");
#endif
}

void TestHashInsertCompareExchangeVulkan(test::TestContext& ctx)
{
    if (FindShaderTool(ShaderTool::Dxc).path.empty())
    {
        std::fprintf(stdout, "  # test_gi.cpp: dxc.exe not found, Vulkan compile skipped\n");
        return;
    }
    const assets::ShaderAsset cs = MakeHashInsertShader();
    assets::CompiledShaderArtifact compiled{};
    std::string error;
    const bool ok = ShaderCompiler::CompileForTarget(cs, assets::ShaderTargetProfile::Vulkan_SPIRV, compiled, &error);
    if (!ok)
        std::fprintf(stderr, "test_gi.cpp: Vulkan compile of GiHashInsertCS failed: %s\n", error.c_str());
    CHECK(ctx, ok);
    constexpr uint16_t kOpAtomicCompareExchange = 230u;
    constexpr uint16_t kOpAtomicIAdd = 234u;
    CHECK(ctx, SpirvHasOpcode(compiled.bytecode, kOpAtomicCompareExchange));
    CHECK(ctx, SpirvHasOpcode(compiled.bytecode, kOpAtomicIAdd));
}

} // namespace

int RunGiTests()
{
    engine::Debug::MinLevel = engine::LogLevel::Fatal;
    test::TestSuite suite("GI");
    suite.Add("Hash insert with InterlockedCompareExchange on WARP (DX11)", TestHashInsertCompareExchangeDx11)
         .Add("Hash insert compiles to OpAtomicCompareExchange (Vulkan)", TestHashInsertCompareExchangeVulkan);
    return suite.Run();
}
