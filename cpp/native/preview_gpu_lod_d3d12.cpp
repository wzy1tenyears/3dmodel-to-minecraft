#include "preview_gpu_lod_d3d12.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
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

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

namespace native_mc {

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kThreadsPerGroup = 256;
constexpr std::uint32_t kMaxDispatchGroups = 65535;
constexpr std::uint64_t kMaxDispatchThreads =
    static_cast<std::uint64_t>(kThreadsPerGroup) * kMaxDispatchGroups;
constexpr std::uint64_t kMaxStructuredElements = 1ull << 27;
constexpr std::uint64_t kMiB = 1024ull * 1024ull;
constexpr UINT kDescriptorsPerPass = 5;
constexpr UINT kPassCount = 3;
constexpr DWORD kFenceWaitSliceMs = 250;
constexpr ULONGLONG kFenceWaitTimeoutMs = 30000;

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

static_assert(sizeof(LodParameters) == 48, "D3D root-constant layout changed");

struct TriangleKey {
    std::array<std::uint32_t, 3> vertices{};
    bool operator==(const TriangleKey& other) const { return vertices == other.vertices; }
};

struct TriangleKeyHash {
    std::size_t operator()(const TriangleKey& key) const {
        std::size_t hash = static_cast<std::size_t>(key.vertices[0]) * 0x9e3779b1u;
        hash ^= static_cast<std::size_t>(key.vertices[1]) + 0x9e3779b9u
            + (hash << 6) + (hash >> 2);
        hash ^= static_cast<std::size_t>(key.vertices[2]) + 0x85ebca6bu
            + (hash << 6) + (hash >> 2);
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

std::string Win32Message(const char* action, DWORD error) {
    std::ostringstream text;
    text << action << " failed (Win32 error " << error << ')';
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

D3D12_HEAP_PROPERTIES HeapProperties(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = type;
    properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    properties.CreationNodeMask = 1;
    properties.VisibleNodeMask = 1;
    return properties;
}

D3D12_RESOURCE_DESC BufferDescription(
    std::uint64_t byteWidth,
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Alignment = 0;
    description.Width = byteWidth;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    description.SampleDesc.Quality = 0;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    description.Flags = flags;
    return description;
}

D3D12_RESOURCE_BARRIER TransitionBarrier(
    ID3D12Resource* resource,
    D3D12_RESOURCE_STATES before,
    D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.UAV.pResource = resource;
    return barrier;
}

class D3D12LodRuntime {
public:
    std::mutex mutex;
    // Declared before D3D objects so retained in-flight resources are released
    // after the queue, command list and device during static destruction.
    std::vector<ComPtr<ID3D12Object>> abandonedSubmissionObjects;
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> scorePipeline;
    ComPtr<ID3D12PipelineState> representativePipeline;
    ComPtr<ID3D12PipelineState> remapPipeline;
    HANDLE fenceEvent = nullptr;
    std::uint64_t fenceValue = 0;
    std::uint64_t dedicatedVideoMemory = 0;
    std::string backendName = "D3D12 Compute";
    bool initializationAttempted = false;
    bool submissionPoisoned = false;
    std::string initializationError;

    ~D3D12LodRuntime() {
        if (fenceEvent) CloseHandle(fenceEvent);
    }

    bool EnsureInitialized() {
        if (submissionPoisoned) return false;
        if (device) return true;
        if (initializationAttempted) return false;
        initializationAttempted = true;

        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(hr)) {
            initializationError = HrMessage("CreateDXGIFactory2", hr);
            return false;
        }

        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            hr = factory->EnumAdapterByGpuPreference(
                index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(candidate.GetAddressOf()));
            if (hr == DXGI_ERROR_NOT_FOUND) break;
            if (FAILED(hr)) {
                initializationError = HrMessage("EnumAdapterByGpuPreference", hr);
                ResetDevice();
                return false;
            }
            DXGI_ADAPTER_DESC1 description{};
            if (FAILED(candidate->GetDesc1(&description)) ||
                (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                continue;
            }
            ComPtr<ID3D12Device> candidateDevice;
            hr = D3D12CreateDevice(
                candidate.Get(), D3D_FEATURE_LEVEL_11_0,
                IID_PPV_ARGS(candidateDevice.GetAddressOf()));
            if (SUCCEEDED(hr)) {
                adapter = std::move(candidate);
                device = std::move(candidateDevice);
                dedicatedVideoMemory = static_cast<std::uint64_t>(description.DedicatedVideoMemory);
                const std::string adapterName = WideToUtf8(description.Description);
                if (!adapterName.empty()) backendName += " (" + adapterName + ')';
                break;
            }
        }
        if (!device) {
            initializationError = "No high-performance D3D12 hardware adapter supports feature level 11.0";
            ResetDevice();
            return false;
        }

        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        queueDescription.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queueDescription.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        queueDescription.NodeMask = 0;
        hr = device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(queue.GetAddressOf()));
        if (FAILED(hr)) return FailInitialization("CreateCommandQueue(compute)", hr);
        hr = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(allocator.GetAddressOf()));
        if (FAILED(hr)) return FailInitialization("CreateCommandAllocator(compute)", hr);
        hr = device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_COMPUTE, allocator.Get(), nullptr,
            IID_PPV_ARGS(commandList.GetAddressOf()));
        if (FAILED(hr)) return FailInitialization("CreateCommandList(compute)", hr);
        hr = commandList->Close();
        if (FAILED(hr)) return FailInitialization("Close(initial command list)", hr);
        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
        if (FAILED(hr)) return FailInitialization("CreateFence", hr);
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) {
            initializationError = Win32Message("CreateEventW", GetLastError());
            ResetDevice();
            return false;
        }
        if (!CreateRootSignature() ||
            !CreatePipeline("FindBestScore", &scorePipeline) ||
            !CreatePipeline("FindRepresentative", &representativePipeline) ||
            !CreatePipeline("RemapTriangles", &remapPipeline)) {
            ResetDevice();
            return false;
        }
        return true;
    }

    bool Begin(ID3D12PipelineState* initialPipeline, std::string* error) {
        if (submissionPoisoned) {
            if (error) {
                *error = initializationError.empty()
                    ? "D3D12 compute runtime cannot reuse an incomplete submission"
                    : initializationError;
            }
            return false;
        }
        HRESULT hr = allocator->Reset();
        if (FAILED(hr)) {
            if (error) *error = HrMessage("Reset(compute allocator)", hr);
            return false;
        }
        hr = commandList->Reset(allocator.Get(), initialPipeline);
        if (FAILED(hr)) {
            if (error) *error = HrMessage("Reset(compute command list)", hr);
            return false;
        }
        return true;
    }

    bool ExecuteAndWait(
        ID3D12Object* const* submissionObjects,
        std::size_t submissionObjectCount,
        std::string* error) {
        std::vector<ComPtr<ID3D12Object>> retainedSubmissionObjects;
        try {
            retainedSubmissionObjects.reserve(submissionObjectCount);
            for (std::size_t index = 0; index < submissionObjectCount; ++index) {
                ID3D12Object* object = submissionObjects[index];
                if (!object) continue;
                object->AddRef();
                ComPtr<ID3D12Object> retained;
                retained.Attach(object);
                retainedSubmissionObjects.push_back(std::move(retained));
            }
        } catch (...) {
            submissionPoisoned = true;
            commandList->Close();
            if (error) {
                try {
                    *error = "D3D12 could not retain submission resources before execution; "
                        "compute runtime disabled";
                } catch (...) {
                    error->clear();
                }
            }
            return false;
        }

        HRESULT hr = commandList->Close();
        if (FAILED(hr)) {
            if (error) *error = HrMessage("Close(compute command list)", hr);
            return false;
        }
        ID3D12CommandList* lists[] = { commandList.Get() };
        queue->ExecuteCommandLists(1, lists);
        const std::uint64_t target = ++fenceValue;
        hr = queue->Signal(fence.Get(), target);
        if (FAILED(hr)) {
            return AbandonSubmittedWork(
                std::move(retainedSubmissionObjects), "Signal(compute fence)", hr, error);
        }
        const std::uint64_t completed = fence->GetCompletedValue();
        if (completed == std::numeric_limits<std::uint64_t>::max()) {
            return DeviceRemoved("GetCompletedValue(compute fence)", error);
        }
        if (completed >= target) return true;
        hr = fence->SetEventOnCompletion(target, fenceEvent);
        const ULONGLONG deadline = GetTickCount64() + kFenceWaitTimeoutMs;
        if (FAILED(hr)) {
            return PollForFence(target, deadline, std::move(retainedSubmissionObjects),
                "SetEventOnCompletion", hr, error);
        }

        for (;;) {
            const DWORD waitResult = WaitForSingleObject(fenceEvent, kFenceWaitSliceMs);
            const std::uint64_t afterWait = fence->GetCompletedValue();
            if (afterWait == std::numeric_limits<std::uint64_t>::max()) {
                return DeviceRemoved("GetCompletedValue(after fence wait)", error);
            }
            if (afterWait >= target) return true;
            if (waitResult == WAIT_FAILED) {
                const HRESULT waitHr = HRESULT_FROM_WIN32(GetLastError());
                return PollForFence(target, deadline, std::move(retainedSubmissionObjects),
                    "WaitForSingleObject(compute fence)", waitHr, error);
            }
            if (GetTickCount64() >= deadline) {
                return AbandonSubmittedWork(std::move(retainedSubmissionObjects),
                    "WaitForSingleObject(compute fence timeout)",
                    HRESULT_FROM_WIN32(ERROR_TIMEOUT), error);
            }
        }
    }

private:
    bool FailInitialization(const char* action, HRESULT hr) {
        initializationError = HrMessage(action, hr);
        ResetDevice();
        return false;
    }

    bool PollForFence(
        std::uint64_t target,
        ULONGLONG deadline,
        std::vector<ComPtr<ID3D12Object>>&& retainedSubmissionObjects,
        const char* failedAction,
        HRESULT originalError,
        std::string* error) {
        for (;;) {
            const std::uint64_t completed = fence->GetCompletedValue();
            if (completed == std::numeric_limits<std::uint64_t>::max()) {
                return DeviceRemoved(failedAction, error, originalError);
            }
            if (completed >= target) return true;
            if (GetTickCount64() >= deadline) {
                return AbandonSubmittedWork(std::move(retainedSubmissionObjects),
                    failedAction, HRESULT_FROM_WIN32(ERROR_TIMEOUT), error);
            }
            Sleep(1);
        }
    }

    bool DeviceRemoved(
        const char* action,
        std::string* error,
        HRESULT precedingError = S_OK) {
        submissionPoisoned = true;
        const HRESULT removed = device->GetDeviceRemovedReason();
        initializationError = precedingError == S_OK
            ? std::string(action) + ": " + HrMessage("GetDeviceRemovedReason", removed)
            : HrMessage(action, precedingError)
                + "; " + HrMessage("GetDeviceRemovedReason", removed);
        if (error) *error = initializationError;
        return false;
    }

    bool AbandonSubmittedWork(
        std::vector<ComPtr<ID3D12Object>>&& submissionObjects,
        const char* action,
        HRESULT hr,
        std::string* error) {
        submissionPoisoned = true;
        abandonedSubmissionObjects.swap(submissionObjects);
        try {
            initializationError = HrMessage(action, hr)
                + "; D3D12 compute runtime disabled after an incomplete submission";
            if (error) *error = initializationError;
        } catch (...) {
            initializationError.clear();
            if (error) error->clear();
        }
        return false;
    }

    bool CreateRootSignature() {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 4;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].RegisterSpace = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.ShaderRegister = 0;
        parameters[0].Constants.RegisterSpace = 0;
        parameters[0].Constants.Num32BitValues = sizeof(LodParameters) / sizeof(std::uint32_t);
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &ranges[0];
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[2].DescriptorTable.NumDescriptorRanges = 1;
        parameters[2].DescriptorTable.pDescriptorRanges = &ranges[1];
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC description{};
        description.NumParameters = 3;
        description.pParameters = parameters;
        description.NumStaticSamplers = 0;
        description.pStaticSamplers = nullptr;
        description.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
        ComPtr<ID3DBlob> bytecode;
        ComPtr<ID3DBlob> errors;
        HRESULT hr = D3D12SerializeRootSignature(
            &description, D3D_ROOT_SIGNATURE_VERSION_1,
            bytecode.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(hr)) {
            initializationError = HrMessage("D3D12SerializeRootSignature", hr);
            AppendCompilerErrors(errors.Get());
            return false;
        }
        hr = device->CreateRootSignature(
            0, bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
            IID_PPV_ARGS(rootSignature.GetAddressOf()));
        if (FAILED(hr)) {
            initializationError = HrMessage("CreateRootSignature", hr);
            return false;
        }
        return true;
    }

    bool CreatePipeline(const char* entryPoint, ComPtr<ID3D12PipelineState>* output) {
        ComPtr<ID3DBlob> bytecode;
        ComPtr<ID3DBlob> errors;
        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        HRESULT hr = D3DCompile(
            kLodShaderSource, std::strlen(kLodShaderSource), "preview_gpu_lod_d3d12.hlsl",
            nullptr, nullptr, entryPoint, "cs_5_0", flags, 0,
            bytecode.GetAddressOf(), errors.GetAddressOf());
        if (FAILED(hr)) {
            initializationError = HrMessage("D3DCompile(compute)", hr);
            AppendCompilerErrors(errors.Get());
            return false;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC description{};
        description.pRootSignature = rootSignature.Get();
        description.CS.pShaderBytecode = bytecode->GetBufferPointer();
        description.CS.BytecodeLength = bytecode->GetBufferSize();
        description.NodeMask = 0;
        description.CachedPSO = {};
        description.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        hr = device->CreateComputePipelineState(&description, IID_PPV_ARGS(output->GetAddressOf()));
        if (FAILED(hr)) {
            initializationError = HrMessage("CreateComputePipelineState", hr);
            return false;
        }
        return true;
    }

    void AppendCompilerErrors(ID3DBlob* errors) {
        if (!errors || !errors->GetBufferPointer()) return;
        initializationError += ": ";
        initializationError.append(
            static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
    }

    void ResetDevice() {
        if (fenceEvent) {
            CloseHandle(fenceEvent);
            fenceEvent = nullptr;
        }
        remapPipeline.Reset();
        representativePipeline.Reset();
        scorePipeline.Reset();
        rootSignature.Reset();
        fence.Reset();
        commandList.Reset();
        allocator.Reset();
        queue.Reset();
        device.Reset();
        adapter.Reset();
        factory.Reset();
    }
};

D3D12LodRuntime& SharedRuntime() {
    static D3D12LodRuntime runtime;
    return runtime;
}

bool CheckedBufferBytes(
    std::uint64_t elementCount,
    std::uint32_t stride,
    std::uint64_t* output) {
    if (!output || elementCount == 0 || stride == 0 ||
        elementCount > kMaxStructuredElements ||
        elementCount > std::numeric_limits<std::uint64_t>::max() / stride) {
        return false;
    }
    *output = elementCount * stride;
    return true;
}

bool CreateBuffer(
    ID3D12Device* device,
    std::uint64_t byteWidth,
    D3D12_HEAP_TYPE heapType,
    D3D12_RESOURCE_STATES initialState,
    D3D12_RESOURCE_FLAGS flags,
    ComPtr<ID3D12Resource>* output,
    std::string* error) {
    if (!device || !output || byteWidth == 0) {
        if (error) *error = "D3D12 buffer arguments were invalid";
        return false;
    }
    const D3D12_HEAP_PROPERTIES heap = HeapProperties(heapType);
    const D3D12_RESOURCE_DESC description = BufferDescription(byteWidth, flags);
    const HRESULT hr = device->CreateCommittedResource(
        &heap, D3D12_HEAP_FLAG_NONE, &description, initialState,
        nullptr, IID_PPV_ARGS(output->GetAddressOf()));
    if (FAILED(hr)) {
        if (error) *error = HrMessage("CreateCommittedResource(buffer)", hr);
        return false;
    }
    return true;
}

bool FillUploadBuffer(
    ID3D12Resource* resource,
    const void* data,
    std::uint64_t byteWidth,
    std::string* error) {
    if (!resource || !data || byteWidth == 0 ||
        byteWidth > static_cast<std::uint64_t>(std::numeric_limits<SIZE_T>::max())) {
        if (error) *error = "D3D12 upload arguments were invalid";
        return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE noRead = { 0, 0 };
    HRESULT hr = resource->Map(0, &noRead, &mapped);
    if (FAILED(hr)) {
        if (error) *error = HrMessage("Map(upload buffer)", hr);
        return false;
    }
    std::memcpy(mapped, data, static_cast<std::size_t>(byteWidth));
    const D3D12_RANGE written = { 0, static_cast<SIZE_T>(byteWidth) };
    resource->Unmap(0, &written);
    return true;
}

bool FillUploadBufferWithOnes(
    ID3D12Resource* resource,
    std::uint64_t byteWidth,
    std::string* error) {
    if (!resource || byteWidth == 0 ||
        byteWidth > static_cast<std::uint64_t>(std::numeric_limits<SIZE_T>::max())) {
        if (error) *error = "D3D12 initialization upload arguments were invalid";
        return false;
    }
    void* mapped = nullptr;
    const D3D12_RANGE noRead = { 0, 0 };
    HRESULT hr = resource->Map(0, &noRead, &mapped);
    if (FAILED(hr)) {
        if (error) *error = HrMessage("Map(initialization upload)", hr);
        return false;
    }
    std::memset(mapped, 0xff, static_cast<std::size_t>(byteWidth));
    const D3D12_RANGE written = { 0, static_cast<SIZE_T>(byteWidth) };
    resource->Unmap(0, &written);
    return true;
}

bool UploadSourceBuffers(
    D3D12LodRuntime* runtime,
    const PreviewMesh& mesh,
    ComPtr<ID3D12Resource>* positionBuffer,
    ComPtr<ID3D12Resource>* indexBuffer,
    std::string* error) {
    const std::uint64_t vertexCount = mesh.positions.size() / 3;
    const std::uint64_t indexCount = mesh.indices.size();
    std::uint64_t positionBytes = 0;
    std::uint64_t indexBytes = 0;
    if (!CheckedBufferBytes(vertexCount, 3u * sizeof(float), &positionBytes) ||
        !CheckedBufferBytes(indexCount, sizeof(std::uint32_t), &indexBytes)) {
        if (error) *error = "D3D12 source buffer exceeds the structured-buffer limit";
        return false;
    }

    ComPtr<ID3D12Resource> positionUpload;
    ComPtr<ID3D12Resource> indexUpload;
    if (!CreateBuffer(runtime->device.Get(), positionBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, positionBuffer, error) ||
        !CreateBuffer(runtime->device.Get(), indexBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, indexBuffer, error) ||
        !CreateBuffer(runtime->device.Get(), positionBytes, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, &positionUpload, error) ||
        !CreateBuffer(runtime->device.Get(), indexBytes, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, &indexUpload, error) ||
        !FillUploadBuffer(positionUpload.Get(), mesh.positions.data(), positionBytes, error) ||
        !FillUploadBuffer(indexUpload.Get(), mesh.indices.data(), indexBytes, error) ||
        !runtime->Begin(nullptr, error)) {
        return false;
    }

    runtime->commandList->CopyBufferRegion(
        positionBuffer->Get(), 0, positionUpload.Get(), 0, positionBytes);
    runtime->commandList->CopyBufferRegion(
        indexBuffer->Get(), 0, indexUpload.Get(), 0, indexBytes);
    D3D12_RESOURCE_BARRIER barriers[2] = {
        TransitionBarrier(positionBuffer->Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        TransitionBarrier(indexBuffer->Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    };
    runtime->commandList->ResourceBarrier(2, barriers);
    const std::array<ID3D12Object*, 4> submissionObjects = {
        positionBuffer->Get(), indexBuffer->Get(), positionUpload.Get(), indexUpload.Get()
    };
    return runtime->ExecuteAndWait(
        submissionObjects.data(), submissionObjects.size(), error);
}

void CreateStructuredSrv(
    ID3D12Device* device,
    ID3D12Resource* resource,
    std::uint32_t elements,
    std::uint32_t stride,
    D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    D3D12_SHADER_RESOURCE_VIEW_DESC description{};
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    description.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    description.Buffer.FirstElement = 0;
    description.Buffer.NumElements = elements;
    description.Buffer.StructureByteStride = stride;
    description.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    device->CreateShaderResourceView(resource, &description, destination);
}

void CreateStructuredUav(
    ID3D12Device* device,
    ID3D12Resource* resource,
    std::uint32_t elements,
    std::uint32_t stride,
    D3D12_CPU_DESCRIPTOR_HANDLE destination) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC description{};
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    description.Buffer.FirstElement = 0;
    description.Buffer.NumElements = elements;
    description.Buffer.StructureByteStride = stride;
    description.Buffer.CounterOffsetInBytes = 0;
    description.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
    device->CreateUnorderedAccessView(resource, nullptr, &description, destination);
}

std::uint64_t EstimateWorkingSetBytes(
    std::uint64_t vertexCount,
    std::uint64_t indexCount,
    std::uint64_t cellCount) {
    const std::uint64_t source = vertexCount * 3ull * sizeof(float)
        + indexCount * sizeof(std::uint32_t);
    const std::uint64_t uploadPeak = source * 2ull;
    const std::uint64_t levelPeak = source
        + indexCount * 2ull * sizeof(std::uint32_t)
        + cellCount * 3ull * sizeof(std::uint32_t);
    const std::uint64_t raw = std::max(uploadPeak, levelPeak);
    return raw + raw / 10ull;
}

std::string FormatMiB(std::uint64_t bytes) {
    std::ostringstream text;
    text.setf(std::ios::fixed);
    text.precision(1);
    text << (static_cast<double>(bytes) / static_cast<double>(kMiB)) << " MiB";
    return text.str();
}

bool CheckWorkingSetLimit(
    const D3D12LodRuntime& runtime,
    std::uint64_t vertexCount,
    std::uint64_t indexCount,
    std::uint32_t resolution,
    std::uint64_t memoryLimit,
    std::string* error) {
    const std::uint64_t r = resolution;
    if (r == 0 || r * r > std::numeric_limits<std::uint64_t>::max() / r) {
        if (error) *error = "D3D12 LOD resolution is invalid";
        return false;
    }
    const std::uint64_t cellCount = r * r * r;
    if (cellCount > std::numeric_limits<std::uint32_t>::max() ||
        cellCount > kMaxStructuredElements) {
        if (error) *error = "GPU LOD input exceeds D3D12 structured-buffer addressing";
        return false;
    }
    const std::uint64_t workingSet = EstimateWorkingSetBytes(
        vertexCount, indexCount, cellCount);
    std::uint64_t effectiveLimit = memoryLimit;
    if (runtime.dedicatedVideoMemory > 0) {
        const std::uint64_t adapterLimit = std::max<std::uint64_t>(
            128ull * kMiB, runtime.dedicatedVideoMemory / 2ull);
        effectiveLimit = effectiveLimit == 0
            ? adapterLimit : std::min(effectiveLimit, adapterLimit);
    }
    if (effectiveLimit != 0 && workingSet > effectiveLimit) {
        if (error) {
            *error = "D3D12 GPU LOD needs approximately " + FormatMiB(workingSet)
                + " but its working-set limit is " + FormatMiB(effectiveLimit);
        }
        return false;
    }
    return true;
}

bool CompactTriangles(
    const std::uint32_t* remapped,
    std::size_t indexCount,
    std::vector<std::uint32_t>* output,
    std::string* error) {
    if (!remapped || !output) {
        if (error) *error = "D3D12 LOD readback data was null";
        return false;
    }
    output->clear();
    output->reserve(indexCount);
    std::unordered_set<TriangleKey, TriangleKeyHash> seen;
    seen.reserve(indexCount / 6);
    for (std::size_t index = 0; index + 2 < indexCount; index += 3) {
        const std::uint32_t a = remapped[index];
        const std::uint32_t b = remapped[index + 1];
        const std::uint32_t c = remapped[index + 2];
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

void SetPassDescriptors(
    ID3D12Device* device,
    D3D12_CPU_DESCRIPTOR_HANDLE heapStart,
    UINT descriptorIncrement,
    UINT pass,
    ID3D12Resource* srv0,
    std::uint32_t srv0Elements,
    std::uint32_t srv0Stride,
    ID3D12Resource* srv1,
    std::uint32_t srv1Elements,
    std::uint32_t srv1Stride,
    ID3D12Resource* srv2,
    std::uint32_t srv2Elements,
    std::uint32_t srv2Stride,
    ID3D12Resource* srv3,
    std::uint32_t srv3Elements,
    std::uint32_t srv3Stride,
    ID3D12Resource* uav,
    std::uint32_t uavElements) {
    const SIZE_T base = static_cast<SIZE_T>(pass * kDescriptorsPerPass) * descriptorIncrement;
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heapStart;
    handle.ptr += base;
    CreateStructuredSrv(device, srv0, srv0Elements, srv0Stride, handle);
    handle.ptr += descriptorIncrement;
    CreateStructuredSrv(device, srv1, srv1Elements, srv1Stride, handle);
    handle.ptr += descriptorIncrement;
    CreateStructuredSrv(device, srv2, srv2Elements, srv2Stride, handle);
    handle.ptr += descriptorIncrement;
    CreateStructuredSrv(device, srv3, srv3Elements, srv3Stride, handle);
    handle.ptr += descriptorIncrement;
    CreateStructuredUav(device, uav, uavElements, sizeof(std::uint32_t), handle);
}

void BindPass(
    D3D12LodRuntime* runtime,
    ID3D12DescriptorHeap* descriptorHeap,
    UINT descriptorIncrement,
    UINT pass,
    ID3D12PipelineState* pipeline) {
    runtime->commandList->SetPipelineState(pipeline);
    runtime->commandList->SetComputeRootSignature(runtime->rootSignature.Get());
    ID3D12DescriptorHeap* heaps[] = { descriptorHeap };
    runtime->commandList->SetDescriptorHeaps(1, heaps);
    D3D12_GPU_DESCRIPTOR_HANDLE srv = descriptorHeap->GetGPUDescriptorHandleForHeapStart();
    srv.ptr += static_cast<UINT64>(pass * kDescriptorsPerPass) * descriptorIncrement;
    D3D12_GPU_DESCRIPTOR_HANDLE uav = srv;
    uav.ptr += static_cast<UINT64>(4) * descriptorIncrement;
    runtime->commandList->SetComputeRootDescriptorTable(1, srv);
    runtime->commandList->SetComputeRootDescriptorTable(2, uav);
}

void DispatchBatched(
    D3D12LodRuntime* runtime,
    std::uint32_t itemCount,
    LodParameters parameters,
    ID3D12Resource* outputUav) {
    std::uint64_t offset = 0;
    while (offset < itemCount) {
        const std::uint64_t batch = std::min<std::uint64_t>(itemCount - offset, kMaxDispatchThreads);
        parameters.workOffset = static_cast<std::uint32_t>(offset);
        runtime->commandList->SetComputeRoot32BitConstants(
            0, sizeof(LodParameters) / sizeof(std::uint32_t), &parameters, 0);
        const UINT groups = static_cast<UINT>(
            (batch + kThreadsPerGroup - 1) / kThreadsPerGroup);
        runtime->commandList->Dispatch(groups, 1, 1);
        offset += batch;
        if (offset < itemCount) {
            const D3D12_RESOURCE_BARRIER barrier = UavBarrier(outputUav);
            runtime->commandList->ResourceBarrier(1, &barrier);
        }
    }
}

bool GenerateLevel(
    D3D12LodRuntime* runtime,
    ID3D12Resource* positionBuffer,
    ID3D12Resource* indexBuffer,
    const PreviewMesh& mesh,
    std::uint32_t resolution,
    std::uint64_t memoryLimit,
    std::vector<std::uint32_t>* output,
    std::string* error) {
    const std::uint64_t vertexCount64 = mesh.positions.size() / 3;
    const std::uint64_t indexCount64 = mesh.indices.size();
    const std::uint64_t r = resolution;
    if (r == 0 || r * r > std::numeric_limits<std::uint64_t>::max() / r) {
        if (error) *error = "D3D12 LOD resolution is invalid";
        return false;
    }
    const std::uint64_t cellCount64 = r * r * r;
    if (vertexCount64 > std::numeric_limits<std::uint32_t>::max() ||
        indexCount64 > std::numeric_limits<std::uint32_t>::max() ||
        cellCount64 > std::numeric_limits<std::uint32_t>::max() ||
        vertexCount64 > kMaxStructuredElements ||
        indexCount64 > kMaxStructuredElements ||
        cellCount64 > kMaxStructuredElements) {
        if (error) *error = "GPU LOD input exceeds D3D12 structured-buffer addressing";
        return false;
    }

    if (!CheckWorkingSetLimit(*runtime, vertexCount64, indexCount64,
            resolution, memoryLimit, error)) return false;

    const auto vertexCount = static_cast<std::uint32_t>(vertexCount64);
    const auto indexCount = static_cast<std::uint32_t>(indexCount64);
    const auto triangleCount = indexCount / 3u;
    const auto cellCount = static_cast<std::uint32_t>(cellCount64);
    std::uint64_t cellBytes = 0;
    std::uint64_t indexBytes = 0;
    if (!CheckedBufferBytes(cellCount64, sizeof(std::uint32_t), &cellBytes) ||
        !CheckedBufferBytes(indexCount64, sizeof(std::uint32_t), &indexBytes)) {
        if (error) *error = "D3D12 LOD buffer size calculation failed";
        return false;
    }

    ComPtr<ID3D12Resource> initializationUpload;
    ComPtr<ID3D12Resource> scoreBuffer;
    ComPtr<ID3D12Resource> representativeBuffer;
    ComPtr<ID3D12Resource> remappedBuffer;
    ComPtr<ID3D12Resource> readbackBuffer;
    if (!CreateBuffer(runtime->device.Get(), cellBytes, D3D12_HEAP_TYPE_UPLOAD,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
            &initializationUpload, error) ||
        !FillUploadBufferWithOnes(initializationUpload.Get(), cellBytes, error) ||
        !CreateBuffer(runtime->device.Get(), cellBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            &scoreBuffer, error) ||
        !CreateBuffer(runtime->device.Get(), cellBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            &representativeBuffer, error) ||
        !CreateBuffer(runtime->device.Get(), indexBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            &remappedBuffer, error) ||
        !CreateBuffer(runtime->device.Get(), indexBytes, D3D12_HEAP_TYPE_READBACK,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE,
            &readbackBuffer, error)) {
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDescription{};
    heapDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDescription.NumDescriptors = kDescriptorsPerPass * kPassCount;
    heapDescription.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    heapDescription.NodeMask = 0;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    HRESULT hr = runtime->device->CreateDescriptorHeap(
        &heapDescription, IID_PPV_ARGS(descriptorHeap.GetAddressOf()));
    if (FAILED(hr)) {
        if (error) *error = HrMessage("CreateDescriptorHeap(LOD)", hr);
        return false;
    }
    const UINT increment = runtime->device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const D3D12_CPU_DESCRIPTOR_HANDLE heapStart =
        descriptorHeap->GetCPUDescriptorHandleForHeapStart();
    SetPassDescriptors(runtime->device.Get(), heapStart, increment, 0,
        positionBuffer, vertexCount, 3u * sizeof(float),
        nullptr, 1, sizeof(std::uint32_t),
        nullptr, 1, sizeof(std::uint32_t),
        nullptr, 1, sizeof(std::uint32_t),
        scoreBuffer.Get(), cellCount);
    SetPassDescriptors(runtime->device.Get(), heapStart, increment, 1,
        positionBuffer, vertexCount, 3u * sizeof(float),
        nullptr, 1, sizeof(std::uint32_t),
        nullptr, 1, sizeof(std::uint32_t),
        scoreBuffer.Get(), cellCount, sizeof(std::uint32_t),
        representativeBuffer.Get(), cellCount);
    SetPassDescriptors(runtime->device.Get(), heapStart, increment, 2,
        positionBuffer, vertexCount, 3u * sizeof(float),
        indexBuffer, indexCount, sizeof(std::uint32_t),
        representativeBuffer.Get(), cellCount, sizeof(std::uint32_t),
        nullptr, 1, sizeof(std::uint32_t),
        remappedBuffer.Get(), indexCount);

    LodParameters parameters{};
    std::copy(mesh.min.begin(), mesh.min.end(), parameters.boundsMin);
    for (int axis = 0; axis < 3; ++axis) {
        parameters.boundsExtent[axis] = mesh.max[axis] - mesh.min[axis];
    }
    parameters.resolution = resolution;
    parameters.vertexCount = vertexCount;
    parameters.indexCount = indexCount;
    parameters.triangleCount = triangleCount;

    if (!runtime->Begin(runtime->scorePipeline.Get(), error)) return false;
    runtime->commandList->CopyBufferRegion(
        scoreBuffer.Get(), 0, initializationUpload.Get(), 0, cellBytes);
    runtime->commandList->CopyBufferRegion(
        representativeBuffer.Get(), 0, initializationUpload.Get(), 0, cellBytes);
    D3D12_RESOURCE_BARRIER initializeBarriers[2] = {
        TransitionBarrier(scoreBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        TransitionBarrier(representativeBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
    };
    runtime->commandList->ResourceBarrier(2, initializeBarriers);

    BindPass(runtime, descriptorHeap.Get(), increment, 0, runtime->scorePipeline.Get());
    DispatchBatched(runtime, vertexCount, parameters, scoreBuffer.Get());
    D3D12_RESOURCE_BARRIER scoreBarriers[2] = {
        UavBarrier(scoreBuffer.Get()),
        TransitionBarrier(scoreBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    };
    runtime->commandList->ResourceBarrier(2, scoreBarriers);

    BindPass(runtime, descriptorHeap.Get(), increment, 1, runtime->representativePipeline.Get());
    DispatchBatched(runtime, vertexCount, parameters, representativeBuffer.Get());
    D3D12_RESOURCE_BARRIER representativeBarriers[2] = {
        UavBarrier(representativeBuffer.Get()),
        TransitionBarrier(representativeBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
    };
    runtime->commandList->ResourceBarrier(2, representativeBarriers);

    BindPass(runtime, descriptorHeap.Get(), increment, 2, runtime->remapPipeline.Get());
    DispatchBatched(runtime, triangleCount, parameters, remappedBuffer.Get());
    D3D12_RESOURCE_BARRIER remapBarriers[2] = {
        UavBarrier(remappedBuffer.Get()),
        TransitionBarrier(remappedBuffer.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_COPY_SOURCE)
    };
    runtime->commandList->ResourceBarrier(2, remapBarriers);
    runtime->commandList->CopyBufferRegion(
        readbackBuffer.Get(), 0, remappedBuffer.Get(), 0, indexBytes);
    const std::array<ID3D12Object*, 8> submissionObjects = {
        positionBuffer, indexBuffer, initializationUpload.Get(), scoreBuffer.Get(),
        representativeBuffer.Get(), remappedBuffer.Get(), readbackBuffer.Get(),
        descriptorHeap.Get()
    };
    if (!runtime->ExecuteAndWait(
            submissionObjects.data(), submissionObjects.size(), error)) {
        return false;
    }

    void* mapped = nullptr;
    const D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(indexBytes) };
    hr = readbackBuffer->Map(0, &readRange, &mapped);
    if (FAILED(hr)) {
        if (error) *error = HrMessage("Map(LOD readback)", hr);
        return false;
    }
    const bool compacted = CompactTriangles(
        static_cast<const std::uint32_t*>(mapped), indexCount, output, error);
    const D3D12_RANGE noWrite = { 0, 0 };
    readbackBuffer->Unmap(0, &noWrite);
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
    result.backendName = "D3D12 Compute";
    auto finish = [&]() {
        result.elapsedMilliseconds = std::chrono::duration<double, std::milli>(
            Clock::now() - started).count();
        return result;
    };

    if (!mesh || !generated || firstLevel >= generated->size()) {
        result.error = "D3D12 GPU LOD argument was null or invalid; CPU fallback required";
        return finish();
    }
    const std::size_t vertexCount = mesh->positions.size() / 3;
    if (vertexCount == 0 || mesh->positions.size() % 3 != 0 ||
        mesh->indices.size() < 3 || mesh->indices.size() % 3 != 0) {
        result.error = "D3D12 GPU LOD mesh data is empty or malformed; CPU fallback required";
        return finish();
    }
    for (std::size_t level = firstLevel; level < resolutions.size(); ++level) {
        if (resolutions[level] == 0) {
            result.error = "D3D12 GPU LOD resolution must be greater than zero; CPU fallback required";
            return finish();
        }
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(mesh->min[axis]) || !std::isfinite(mesh->max[axis]) ||
            mesh->max[axis] < mesh->min[axis]) {
            result.error = "D3D12 GPU LOD mesh bounds are invalid; CPU fallback required";
            return finish();
        }
    }

    D3D12LodRuntime& runtime = SharedRuntime();
    std::lock_guard<std::mutex> lock(runtime.mutex);
    if (!runtime.EnsureInitialized()) {
        result.error = runtime.initializationError.empty()
            ? "D3D12 Compute initialization failed; CPU fallback required"
            : runtime.initializationError + "; CPU fallback required";
        return finish();
    }
    result.backendName = runtime.backendName;

    const std::uint64_t vertexCount64 = vertexCount;
    const std::uint64_t indexCount64 = mesh->indices.size();
    if (vertexCount64 > std::numeric_limits<std::uint32_t>::max() ||
        indexCount64 > std::numeric_limits<std::uint32_t>::max() ||
        vertexCount64 > kMaxStructuredElements || indexCount64 > kMaxStructuredElements) {
        result.error = "GPU LOD input exceeds D3D12 structured-buffer addressing; CPU fallback required";
        return finish();
    }

    for (std::size_t level = firstLevel; level < resolutions.size(); ++level) {
        if (!CheckWorkingSetLimit(runtime, vertexCount64, indexCount64,
                resolutions[level], maxWorkingSetBytes, &result.error)) {
            result.error += "; CPU fallback required";
            return finish();
        }
    }

    ComPtr<ID3D12Resource> positionBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    if (!UploadSourceBuffers(
            &runtime, *mesh, &positionBuffer, &indexBuffer, &result.error)) {
        result.error += "; CPU fallback required";
        return finish();
    }

    std::array<std::vector<std::uint32_t>, 4> pending;
    for (std::size_t level = firstLevel; level < pending.size(); ++level) {
        const auto levelStarted = Clock::now();
        if (!GenerateLevel(&runtime, positionBuffer.Get(), indexBuffer.Get(), *mesh,
                resolutions[level], maxWorkingSetBytes, &pending[level], &result.error)) {
            result.error += "; CPU fallback required";
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

PreviewGpuLodResult BuildPreviewMeshLodsD3D12(
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

PreviewGpuLodResult BuildPreviewMeshGpuProxiesD3D12(
    const PreviewMesh& mesh,
    const std::array<std::uint32_t, 4>& resolutions,
    std::array<std::vector<std::uint32_t>, 4>* proxyIndices,
    std::uint64_t maxWorkingSetBytes) {
    return GenerateGpuIndexLevels(
        &mesh, resolutions, 0, maxWorkingSetBytes, proxyIndices);
}

}  // namespace native_mc
