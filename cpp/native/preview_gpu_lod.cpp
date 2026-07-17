#include "preview_gpu_lod.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

namespace native_mc {

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kThreadsPerGroup = 256;
constexpr std::uint32_t kMaxDispatchGroups = D3D11_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
constexpr std::uint64_t kMaxDispatchThreads =
    static_cast<std::uint64_t>(kThreadsPerGroup) * kMaxDispatchGroups;
constexpr std::uint64_t kMiB = 1024ull * 1024ull;

const char kLodShaderSource[] = R"hlsl(
cbuffer LodParameters : register(b0)
{
    float3 boundsMin;
    uint resolution;
    float3 boundsExtent;
    uint vertexCount;
    uint indexCount;
    uint triangleCount;
    uint workOffset;
    uint unusedParameter;
};

StructuredBuffer<float3> positions : register(t0);
StructuredBuffer<uint> inputIndices : register(t1);
StructuredBuffer<uint> representatives : register(t2);
StructuredBuffer<uint> bestScores : register(t3);
RWStructuredBuffer<uint> outputValues : register(u0);

uint ClusterCell(float3 position, out uint3 cell)
{
    float3 normalized;
    normalized.x = boundsExtent.x > 0.000001f
        ? (position.x - boundsMin.x) / boundsExtent.x : 0.5f;
    normalized.y = boundsExtent.y > 0.000001f
        ? (position.y - boundsMin.y) / boundsExtent.y : 0.5f;
    normalized.z = boundsExtent.z > 0.000001f
        ? (position.z - boundsMin.z) / boundsExtent.z : 0.5f;
    normalized = saturate(normalized);
    cell = min((uint3)(normalized * (float)resolution), resolution - 1u);
    return cell.x + resolution * (cell.y + resolution * cell.z);
}

uint ClusterScore(float3 position, uint3 cell)
{
    const float3 center = boundsMin
        + ((float3)cell + 0.5f) * boundsExtent / (float)resolution;
    const float3 scale = max(boundsExtent, float3(0.000001f, 0.000001f, 0.000001f));
    const float3 delta = (position - center) / scale;
    return asuint(dot(delta, delta));
}

[numthreads(256, 1, 1)]
void FindBestScore(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint vertex = dispatchThreadId.x + workOffset;
    if (vertex >= vertexCount) return;
    uint3 cell;
    const uint key = ClusterCell(positions[vertex], cell);
    InterlockedMin(outputValues[key], ClusterScore(positions[vertex], cell));
}

[numthreads(256, 1, 1)]
void FindRepresentative(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint vertex = dispatchThreadId.x + workOffset;
    if (vertex >= vertexCount) return;
    uint3 cell;
    const uint key = ClusterCell(positions[vertex], cell);
    if (bestScores[key] == ClusterScore(positions[vertex], cell)) {
        InterlockedMin(outputValues[key], vertex);
    }
}

uint RepresentativeForVertex(uint vertex)
{
    uint3 cell;
    const uint key = ClusterCell(positions[vertex], cell);
    return representatives[key];
}

[numthreads(256, 1, 1)]
void RemapTriangles(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    const uint triangleIndex = dispatchThreadId.x + workOffset;
    if (triangleIndex >= triangleCount) return;
    const uint base = triangleIndex * 3u;
    const uint ia = inputIndices[base];
    const uint ib = inputIndices[base + 1u];
    const uint ic = inputIndices[base + 2u];
    if (ia >= vertexCount || ib >= vertexCount || ic >= vertexCount) {
        outputValues[base] = 0xffffffffu;
        outputValues[base + 1u] = 0xffffffffu;
        outputValues[base + 2u] = 0xffffffffu;
        return;
    }
    const uint a = RepresentativeForVertex(ia);
    const uint b = RepresentativeForVertex(ib);
    const uint c = RepresentativeForVertex(ic);
    if (a == b || b == c || a == c ||
        a == 0xffffffffu || b == 0xffffffffu || c == 0xffffffffu) {
        outputValues[base] = 0xffffffffu;
        outputValues[base + 1u] = 0xffffffffu;
        outputValues[base + 2u] = 0xffffffffu;
        return;
    }
    outputValues[base] = a;
    outputValues[base + 1u] = b;
    outputValues[base + 2u] = c;
}
)hlsl";

struct alignas(16) LodParameters {
    float boundsMin[3]{};
    std::uint32_t resolution = 0;
    float boundsExtent[3]{};
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    std::uint32_t triangleCount = 0;
    std::uint32_t workOffset = 0;
    std::uint32_t unusedParameter = 0;
};

static_assert(sizeof(LodParameters) == 48, "D3D constant buffer layout changed");

struct TriangleKey {
    std::array<std::uint32_t, 3> vertices{};
    bool operator==(const TriangleKey& other) const { return vertices == other.vertices; }
};

struct TriangleKeyHash {
    std::size_t operator()(const TriangleKey& key) const {
        std::size_t hash = static_cast<std::size_t>(key.vertices[0]) * 0x9e3779b1u;
        hash ^= static_cast<std::size_t>(key.vertices[1]) + 0x9e3779b9u + (hash << 6) + (hash >> 2);
        hash ^= static_cast<std::size_t>(key.vertices[2]) + 0x85ebca6bu + (hash << 6) + (hash >> 2);
        return hash;
    }
};

TriangleKey OrientedTriangleKey(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    if (a < b && a < c) return TriangleKey{ { a, b, c } };
    if (b < a && b < c) return TriangleKey{ { b, c, a } };
    return TriangleKey{ { c, a, b } };
}

std::string HrMessage(const char* action, HRESULT hr) {
    std::ostringstream text;
    text << action << " failed (HRESULT 0x" << std::hex
         << static_cast<unsigned long>(hr) << ')';
    return text.str();
}

std::string WideToUtf8(const wchar_t* value) {
    if (!value || !*value) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) return {};
    std::string result(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), bytes, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(bytes - 1));
    return result;
}

class D3D11LodRuntime {
public:
    std::mutex mutex;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> scoreShader;
    ComPtr<ID3D11ComputeShader> representativeShader;
    ComPtr<ID3D11ComputeShader> remapShader;
    ComPtr<ID3D11Buffer> parameterBuffer;
    std::string backendName = "D3D11 Compute";
    std::uint64_t dedicatedVideoMemory = 0;
    bool initializationAttempted = false;
    std::string initializationError;

    bool EnsureInitialized() {
        if (device) return true;
        if (initializationAttempted) return false;
        initializationAttempted = true;

        constexpr UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        const std::array<D3D_FEATURE_LEVEL, 2> featureLevels = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0
        };
        D3D_FEATURE_LEVEL selectedLevel = D3D_FEATURE_LEVEL_9_1;
        HRESULT hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            featureLevels.data(), static_cast<UINT>(featureLevels.size()),
            D3D11_SDK_VERSION, &device, &selectedLevel, &context);
        if (hr == E_INVALIDARG) {
            hr = D3D11CreateDevice(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                &featureLevels[1], 1, D3D11_SDK_VERSION,
                &device, &selectedLevel, &context);
        }
        if (FAILED(hr)) {
            initializationError = HrMessage("D3D11CreateDevice", hr);
            return false;
        }
        if (selectedLevel < D3D_FEATURE_LEVEL_11_0) {
            initializationError = "D3D11 feature level 11.0 is required for GPU LOD";
            ResetDevice();
            return false;
        }

        ReadAdapterDescription();
        if (!CreateShader("FindBestScore", &scoreShader) ||
            !CreateShader("FindRepresentative", &representativeShader) ||
            !CreateShader("RemapTriangles", &remapShader)) {
            ResetDevice();
            return false;
        }

        D3D11_BUFFER_DESC parameters{};
        parameters.ByteWidth = sizeof(LodParameters);
        parameters.Usage = D3D11_USAGE_DYNAMIC;
        parameters.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        parameters.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&parameters, nullptr, &parameterBuffer);
        if (FAILED(hr)) {
            initializationError = HrMessage("CreateBuffer(parameters)", hr);
            ResetDevice();
            return false;
        }
        return true;
    }

    void UnbindComputeResources() {
        ID3D11ShaderResourceView* nullSrvs[4] = {};
        ID3D11UnorderedAccessView* nullUavs[1] = {};
        context->CSSetShaderResources(0, 4, nullSrvs);
        context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
        context->CSSetShader(nullptr, nullptr, 0);
    }

private:
    bool CreateShader(const char* entryPoint, ComPtr<ID3D11ComputeShader>* output) {
        ComPtr<ID3DBlob> bytecode;
        ComPtr<ID3DBlob> errors;
        const UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        const HRESULT hr = D3DCompile(
            kLodShaderSource, std::strlen(kLodShaderSource), "preview_gpu_lod.hlsl",
            nullptr, nullptr, entryPoint, "cs_5_0", compileFlags, 0,
            &bytecode, &errors);
        if (FAILED(hr)) {
            initializationError = HrMessage("D3DCompile", hr);
            if (errors && errors->GetBufferPointer()) {
                initializationError += ": ";
                initializationError.append(
                    static_cast<const char*>(errors->GetBufferPointer()),
                    errors->GetBufferSize());
            }
            return false;
        }
        const HRESULT createHr = device->CreateComputeShader(
            bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, output->GetAddressOf());
        if (FAILED(createHr)) {
            initializationError = HrMessage("CreateComputeShader", createHr);
            return false;
        }
        return true;
    }

    void ReadAdapterDescription() {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC description{};
        if (SUCCEEDED(device.As(&dxgiDevice)) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
            SUCCEEDED(adapter->GetDesc(&description))) {
            dedicatedVideoMemory = static_cast<std::uint64_t>(description.DedicatedVideoMemory);
            const std::string adapterName = WideToUtf8(description.Description);
            if (!adapterName.empty()) backendName += " (" + adapterName + ')';
        }
    }

    void ResetDevice() {
        parameterBuffer.Reset();
        remapShader.Reset();
        representativeShader.Reset();
        scoreShader.Reset();
        context.Reset();
        device.Reset();
    }
};

D3D11LodRuntime& SharedRuntime() {
    static D3D11LodRuntime runtime;
    return runtime;
}

bool CheckedByteWidth(std::uint64_t elementCount, std::uint32_t stride, UINT* output) {
    if (!output || elementCount == 0 || stride == 0) return false;
    const std::uint64_t bytes = elementCount * stride;
    if (bytes > std::numeric_limits<UINT>::max()) return false;
    *output = static_cast<UINT>(bytes);
    return true;
}

bool CreateStructuredBuffer(
    ID3D11Device* device,
    const void* data,
    std::uint64_t elementCount,
    std::uint32_t stride,
    UINT bindFlags,
    ComPtr<ID3D11Buffer>* output,
    std::string* error) {
    UINT byteWidth = 0;
    constexpr std::uint64_t maxElements =
        1ull << D3D11_REQ_BUFFER_RESOURCE_TEXEL_COUNT_2_TO_EXP;
    if (!CheckedByteWidth(elementCount, stride, &byteWidth) ||
        elementCount > maxElements) {
        if (error) *error = "D3D11 structured buffer exceeds the API size limit";
        return false;
    }
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = byteWidth;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = bindFlags;
    description.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    description.StructureByteStride = stride;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = data;
    const HRESULT hr = device->CreateBuffer(&description, data ? &initial : nullptr, output->GetAddressOf());
    if (FAILED(hr)) {
        if (error) *error = HrMessage("CreateBuffer(structured)", hr);
        return false;
    }
    return true;
}

bool CreateStructuredSrv(
    ID3D11Device* device,
    ID3D11Buffer* buffer,
    std::uint32_t elements,
    ComPtr<ID3D11ShaderResourceView>* output,
    std::string* error) {
    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    description.Buffer.NumElements = elements;
    const HRESULT hr = device->CreateShaderResourceView(buffer, &description, output->GetAddressOf());
    if (FAILED(hr)) {
        if (error) *error = HrMessage("CreateShaderResourceView", hr);
        return false;
    }
    return true;
}

bool CreateStructuredUav(
    ID3D11Device* device,
    ID3D11Buffer* buffer,
    std::uint32_t elements,
    ComPtr<ID3D11UnorderedAccessView>* output,
    std::string* error) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC description{};
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    description.Buffer.NumElements = elements;
    const HRESULT hr = device->CreateUnorderedAccessView(buffer, &description, output->GetAddressOf());
    if (FAILED(hr)) {
        if (error) *error = HrMessage("CreateUnorderedAccessView", hr);
        return false;
    }
    return true;
}

bool CreateReadbackBuffer(
    ID3D11Device* device,
    UINT byteWidth,
    ComPtr<ID3D11Buffer>* output,
    std::string* error) {
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = byteWidth;
    description.Usage = D3D11_USAGE_STAGING;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    const HRESULT hr = device->CreateBuffer(&description, nullptr, output->GetAddressOf());
    if (FAILED(hr)) {
        if (error) *error = HrMessage("CreateBuffer(readback)", hr);
        return false;
    }
    return true;
}

bool UpdateParameters(D3D11LodRuntime* runtime, const LodParameters& parameters, std::string* error) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = runtime->context->Map(
        runtime->parameterBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr)) {
        if (error) *error = HrMessage("Map(parameters)", hr);
        return false;
    }
    std::memcpy(mapped.pData, &parameters, sizeof(parameters));
    runtime->context->Unmap(runtime->parameterBuffer.Get(), 0);
    return true;
}

bool DispatchBatched(
    D3D11LodRuntime* runtime,
    ID3D11ComputeShader* shader,
    std::uint32_t itemCount,
    LodParameters parameters,
    std::string* error) {
    runtime->context->CSSetShader(shader, nullptr, 0);
    ID3D11Buffer* parameterBuffer = runtime->parameterBuffer.Get();
    runtime->context->CSSetConstantBuffers(0, 1, &parameterBuffer);
    std::uint64_t offset = 0;
    while (offset < itemCount) {
        const std::uint64_t batch = std::min<std::uint64_t>(itemCount - offset, kMaxDispatchThreads);
        parameters.workOffset = static_cast<std::uint32_t>(offset);
        if (!UpdateParameters(runtime, parameters, error)) return false;
        const UINT groups = static_cast<UINT>((batch + kThreadsPerGroup - 1) / kThreadsPerGroup);
        runtime->context->Dispatch(groups, 1, 1);
        offset += batch;
    }
    return true;
}

std::uint64_t EstimateWorkingSetBytes(
    std::uint64_t vertexCount,
    std::uint64_t indexCount,
    std::uint64_t cellCount) {
    // Source positions + source indices + remapped output + staging readback
    // + score and representative buffers. Add 10% driver/resource overhead.
    const std::uint64_t raw = vertexCount * 3ull * sizeof(float)
        + indexCount * 3ull * sizeof(std::uint32_t)
        + cellCount * 2ull * sizeof(std::uint32_t);
    return raw + raw / 10ull;
}

std::string FormatMiB(std::uint64_t bytes) {
    std::ostringstream text;
    text.setf(std::ios::fixed);
    text.precision(1);
    text << (static_cast<double>(bytes) / static_cast<double>(kMiB)) << " MiB";
    return text.str();
}

bool CompactTriangles(
    const std::uint32_t* remapped,
    std::size_t indexCount,
    std::vector<std::uint32_t>* output,
    std::string* error) {
    if (!remapped || !output) {
        if (error) *error = "GPU LOD readback data was null";
        return false;
    }
    output->clear();
    output->reserve(indexCount);
    std::unordered_set<TriangleKey, TriangleKeyHash> seen;
    seen.reserve(indexCount / 6);
    for (std::size_t i = 0; i + 2 < indexCount; i += 3) {
        const std::uint32_t a = remapped[i];
        const std::uint32_t b = remapped[i + 1];
        const std::uint32_t c = remapped[i + 2];
        if (a == std::numeric_limits<std::uint32_t>::max() ||
            b == std::numeric_limits<std::uint32_t>::max() ||
            c == std::numeric_limits<std::uint32_t>::max() ||
            a == b || b == c || a == c) {
            continue;
        }
        const TriangleKey key = OrientedTriangleKey(a, b, c);
        if (!seen.insert(key).second) continue;
        output->push_back(a);
        output->push_back(b);
        output->push_back(c);
    }
    output->shrink_to_fit();
    return true;
}

bool GenerateLevel(
    D3D11LodRuntime* runtime,
    ID3D11ShaderResourceView* positionSrv,
    ID3D11ShaderResourceView* indexSrv,
    const PreviewMesh& mesh,
    std::uint32_t resolution,
    std::uint64_t memoryLimit,
    std::vector<std::uint32_t>* output,
    std::string* error) {
    const std::uint64_t vertexCount64 = mesh.positions.size() / 3;
    const std::uint64_t indexCount64 = mesh.indices.size();
    const std::uint64_t cellCount64 = static_cast<std::uint64_t>(resolution)
        * resolution * resolution;
    if (vertexCount64 > std::numeric_limits<std::uint32_t>::max() ||
        indexCount64 > std::numeric_limits<std::uint32_t>::max() ||
        cellCount64 > std::numeric_limits<std::uint32_t>::max()) {
        if (error) *error = "GPU LOD input exceeds D3D11 32-bit addressing; CPU fallback required";
        return false;
    }

    const std::uint64_t workingSet = EstimateWorkingSetBytes(vertexCount64, indexCount64, cellCount64);
    std::uint64_t effectiveLimit = memoryLimit;
    if (runtime->dedicatedVideoMemory > 0) {
        const std::uint64_t adapterLimit = std::max<std::uint64_t>(128ull * kMiB,
            runtime->dedicatedVideoMemory / 2ull);
        effectiveLimit = effectiveLimit == 0 ? adapterLimit : std::min(effectiveLimit, adapterLimit);
    }
    if (effectiveLimit != 0 && workingSet > effectiveLimit) {
        if (error) {
            *error = "GPU LOD needs approximately " + FormatMiB(workingSet)
                + " but its working-set limit is " + FormatMiB(effectiveLimit)
                + "; CPU fallback required";
        }
        return false;
    }

    const auto vertexCount = static_cast<std::uint32_t>(vertexCount64);
    const auto indexCount = static_cast<std::uint32_t>(indexCount64);
    const auto triangleCount = indexCount / 3u;
    const auto cellCount = static_cast<std::uint32_t>(cellCount64);

    ComPtr<ID3D11Buffer> scoreBuffer;
    ComPtr<ID3D11ShaderResourceView> scoreSrv;
    ComPtr<ID3D11UnorderedAccessView> scoreUav;
    ComPtr<ID3D11Buffer> representativeBuffer;
    ComPtr<ID3D11ShaderResourceView> representativeSrv;
    ComPtr<ID3D11UnorderedAccessView> representativeUav;
    ComPtr<ID3D11Buffer> remappedBuffer;
    ComPtr<ID3D11UnorderedAccessView> remappedUav;
    ComPtr<ID3D11Buffer> readbackBuffer;

    if (!CreateStructuredBuffer(runtime->device.Get(), nullptr, cellCount, sizeof(std::uint32_t),
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &scoreBuffer, error) ||
        !CreateStructuredSrv(runtime->device.Get(), scoreBuffer.Get(), cellCount, &scoreSrv, error) ||
        !CreateStructuredUav(runtime->device.Get(), scoreBuffer.Get(), cellCount, &scoreUav, error) ||
        !CreateStructuredBuffer(runtime->device.Get(), nullptr, cellCount, sizeof(std::uint32_t),
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, &representativeBuffer, error) ||
        !CreateStructuredSrv(runtime->device.Get(), representativeBuffer.Get(), cellCount, &representativeSrv, error) ||
        !CreateStructuredUav(runtime->device.Get(), representativeBuffer.Get(), cellCount, &representativeUav, error) ||
        !CreateStructuredBuffer(runtime->device.Get(), nullptr, indexCount, sizeof(std::uint32_t),
            D3D11_BIND_UNORDERED_ACCESS, &remappedBuffer, error) ||
        !CreateStructuredUav(runtime->device.Get(), remappedBuffer.Get(), indexCount, &remappedUav, error)) {
        return false;
    }
    UINT readbackBytes = 0;
    if (!CheckedByteWidth(indexCount, sizeof(std::uint32_t), &readbackBytes) ||
        !CreateReadbackBuffer(runtime->device.Get(), readbackBytes, &readbackBuffer, error)) {
        return false;
    }

    LodParameters parameters{};
    std::copy(mesh.min.begin(), mesh.min.end(), parameters.boundsMin);
    for (int axis = 0; axis < 3; ++axis) {
        parameters.boundsExtent[axis] = mesh.max[axis] - mesh.min[axis];
    }
    parameters.resolution = resolution;
    parameters.vertexCount = vertexCount;
    parameters.indexCount = indexCount;
    parameters.triangleCount = triangleCount;

    const UINT clearValues[4] = { 0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu };
    runtime->context->ClearUnorderedAccessViewUint(scoreUav.Get(), clearValues);
    ID3D11ShaderResourceView* scorePassSrvs[1] = { positionSrv };
    ID3D11UnorderedAccessView* scorePassUavs[1] = { scoreUav.Get() };
    runtime->context->CSSetShaderResources(0, 1, scorePassSrvs);
    runtime->context->CSSetUnorderedAccessViews(0, 1, scorePassUavs, nullptr);
    if (!DispatchBatched(runtime, runtime->scoreShader.Get(), vertexCount, parameters, error)) {
        runtime->UnbindComputeResources();
        return false;
    }
    runtime->UnbindComputeResources();

    runtime->context->ClearUnorderedAccessViewUint(representativeUav.Get(), clearValues);
    ID3D11ShaderResourceView* representativePassSrvs[4] = {
        positionSrv, nullptr, nullptr, scoreSrv.Get()
    };
    ID3D11UnorderedAccessView* representativePassUavs[1] = { representativeUav.Get() };
    runtime->context->CSSetShaderResources(0, 4, representativePassSrvs);
    runtime->context->CSSetUnorderedAccessViews(0, 1, representativePassUavs, nullptr);
    if (!DispatchBatched(runtime, runtime->representativeShader.Get(), vertexCount, parameters, error)) {
        runtime->UnbindComputeResources();
        return false;
    }
    runtime->UnbindComputeResources();

    ID3D11ShaderResourceView* remapPassSrvs[3] = {
        positionSrv, indexSrv, representativeSrv.Get()
    };
    ID3D11UnorderedAccessView* remapPassUavs[1] = { remappedUav.Get() };
    runtime->context->CSSetShaderResources(0, 3, remapPassSrvs);
    runtime->context->CSSetUnorderedAccessViews(0, 1, remapPassUavs, nullptr);
    if (!DispatchBatched(runtime, runtime->remapShader.Get(), triangleCount, parameters, error)) {
        runtime->UnbindComputeResources();
        return false;
    }
    runtime->UnbindComputeResources();

    runtime->context->CopyResource(readbackBuffer.Get(), remappedBuffer.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT mapHr = runtime->context->Map(readbackBuffer.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(mapHr)) {
        if (error) *error = HrMessage("Map(LOD readback)", mapHr);
        return false;
    }
    const bool compacted = CompactTriangles(
        static_cast<const std::uint32_t*>(mapped.pData), indexCount, output, error);
    runtime->context->Unmap(readbackBuffer.Get(), 0);
    return compacted;
}

PreviewGpuLodResult GenerateGpuIndexLevels(
    const PreviewMesh* mesh,
    const std::array<std::uint32_t, 4>& resolutions,
    std::size_t firstLevel,
    std::uint64_t maxWorkingSetBytes,
    std::array<std::vector<std::uint32_t>, 4>* generated) {
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    PreviewGpuLodResult result;
    auto finish = [&]() {
        result.elapsedMilliseconds = std::chrono::duration<double, std::milli>(
            Clock::now() - started).count();
        return result;
    };

    if (!mesh || !generated || firstLevel >= generated->size()) {
        result.error = "GPU LOD argument was null or invalid; CPU fallback required";
        return finish();
    }
    const std::size_t vertexCount = mesh->positions.size() / 3;
    if (vertexCount == 0 || mesh->positions.size() % 3 != 0 ||
        mesh->indices.size() < 3 || mesh->indices.size() % 3 != 0) {
        result.error = "GPU LOD mesh data is empty or malformed; CPU fallback required";
        return finish();
    }
    for (std::size_t level = firstLevel; level < resolutions.size(); ++level) {
        if (resolutions[level] == 0) {
            result.error = "GPU LOD resolution must be greater than zero; CPU fallback required";
            return finish();
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(mesh->min[axis]) || !std::isfinite(mesh->max[axis]) ||
            mesh->max[axis] < mesh->min[axis]) {
            result.error = "GPU LOD mesh bounds are invalid; CPU fallback required";
            return finish();
        }
    }

    D3D11LodRuntime& runtime = SharedRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.EnsureInitialized()) {
        result.error = runtime.initializationError.empty()
            ? "D3D11 Compute initialization failed; CPU fallback required"
            : runtime.initializationError + "; CPU fallback required";
        return finish();
    }
    result.backendName = runtime.backendName;

    const std::uint64_t vertexCount64 = vertexCount;
    const std::uint64_t indexCount64 = mesh->indices.size();
    if (vertexCount64 > std::numeric_limits<std::uint32_t>::max() ||
        indexCount64 > std::numeric_limits<std::uint32_t>::max()) {
        result.error = "GPU LOD input exceeds D3D11 32-bit addressing; CPU fallback required";
        return finish();
    }

    ComPtr<ID3D11Buffer> positionBuffer;
    ComPtr<ID3D11ShaderResourceView> positionSrv;
    ComPtr<ID3D11Buffer> indexBuffer;
    ComPtr<ID3D11ShaderResourceView> indexSrv;
    if (!CreateStructuredBuffer(runtime.device.Get(), mesh->positions.data(), vertexCount64,
            3u * sizeof(float), D3D11_BIND_SHADER_RESOURCE, &positionBuffer, &result.error) ||
        !CreateStructuredSrv(runtime.device.Get(), positionBuffer.Get(),
            static_cast<std::uint32_t>(vertexCount64), &positionSrv, &result.error) ||
        !CreateStructuredBuffer(runtime.device.Get(), mesh->indices.data(), indexCount64,
            sizeof(std::uint32_t), D3D11_BIND_SHADER_RESOURCE, &indexBuffer, &result.error) ||
        !CreateStructuredSrv(runtime.device.Get(), indexBuffer.Get(),
            static_cast<std::uint32_t>(indexCount64), &indexSrv, &result.error)) {
        result.error += "; CPU fallback required";
        return finish();
    }

    std::array<std::vector<std::uint32_t>, 4> pending;
    for (std::size_t level = firstLevel; level < pending.size(); ++level) {
        const auto levelStarted = Clock::now();
        if (!GenerateLevel(&runtime, positionSrv.Get(), indexSrv.Get(), *mesh,
                resolutions[level], maxWorkingSetBytes,
                &pending[level], &result.error)) {
            runtime.UnbindComputeResources();
            return finish();
        }
        result.levelMilliseconds[level] = std::chrono::duration<double, std::milli>(
            Clock::now() - levelStarted).count();
        result.triangleCounts[level] = pending[level].size() / 3;
    }

    *generated = std::move(pending);
    result.success = true;
    return finish();
}

}  // namespace

PreviewGpuLodResult BuildPreviewMeshLodsD3D11(
    PreviewMesh* mesh,
    const PreviewGpuLodOptions& options) {
    std::array<std::vector<std::uint32_t>, 4> generated;
    PreviewGpuLodResult result = GenerateGpuIndexLevels(
        mesh, options.resolutions, 1, options.maxWorkingSetBytes, &generated);
    if (!result.success) return result;

    mesh->lodIndices[0].clear();
    for (std::size_t level = 1; level < generated.size(); ++level) {
        mesh->lodIndices[level] = std::move(generated[level]);
    }
    result.triangleCounts[0] = mesh->indices.size() / 3;
    return result;
}

PreviewGpuLodResult BuildPreviewMeshGpuProxiesD3D11(
    const PreviewMesh& mesh,
    const std::array<std::uint32_t, 4>& resolutions,
    std::array<std::vector<std::uint32_t>, 4>* proxyIndices,
    std::uint64_t maxWorkingSetBytes) {
    return GenerateGpuIndexLevels(
        &mesh, resolutions, 0, maxWorkingSetBytes, proxyIndices);
}

}  // namespace native_mc
