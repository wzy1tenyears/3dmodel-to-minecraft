#define NOMINMAX
#include "preview_d3d12_renderer.h"

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>
#include <utility>
#include <vector>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

namespace native_mc {
namespace {

using Microsoft::WRL::ComPtr;
using namespace DirectX;

constexpr float kPi = 3.14159265358979323846f;
constexpr UINT kFrameCount = 3;
constexpr UINT kConstantBlocksPerFrame = 8;
constexpr UINT64 kConstantAlignment = D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT;
constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT;

struct RenderVertex {
    float position[3];
    float normal[3];
    std::uint8_t color[4];
};

static_assert(sizeof(RenderVertex) == 28, "Unexpected render vertex packing");

struct ColorVertex {
    float position[3];
    std::uint8_t color[4];
};

static_assert(sizeof(ColorVertex) == 16, "Unexpected color vertex packing");

struct FrameConstants {
    XMFLOAT4X4 worldViewProjection;
    XMFLOAT4X4 world;
    XMFLOAT4X4 normalWorld;
    XMFLOAT4 lightDirectionAndAmbient;
    XMFLOAT4 viewportAndPointSize;
};

constexpr const char* kShaderSource = R"hlsl(
cbuffer FrameConstants : register(b0) {
    row_major float4x4 worldViewProjection;
    row_major float4x4 world;
    row_major float4x4 normalWorld;
    float4 lightDirectionAndAmbient;
    float4 viewportAndPointSize;
};

struct VertexInput {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR0;
};

struct VertexOutput {
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float4 color : COLOR0;
};

struct ColorVertexInput {
    float3 position : POSITION;
    float4 color : COLOR0;
};

VertexOutput MainVS(VertexInput input) {
    VertexOutput output;
    output.position = mul(float4(input.position, 1.0f), worldViewProjection);
    output.normal = normalize(mul(float4(input.normal, 0.0f), normalWorld).xyz);
    output.color = input.color;
    return output;
}

VertexOutput ColorVS(ColorVertexInput input) {
    VertexOutput output;
    output.position = mul(float4(input.position, 1.0f), worldViewProjection);
    output.normal = float3(0.0f, 1.0f, 0.0f);
    output.color = input.color;
    return output;
}

[maxvertexcount(4)]
void PointGS(point VertexOutput input[1], inout TriangleStream<VertexOutput> outputStream) {
    const float2 viewport = max(viewportAndPointSize.xy, float2(1.0f, 1.0f));
    const float2 halfSizeNdc = viewportAndPointSize.zz / viewport;
    const float2 clipOffset = halfSizeNdc * input[0].position.w;
    const float2 corners[4] = {
        float2(-clipOffset.x, clipOffset.y),
        float2(clipOffset.x, clipOffset.y),
        float2(-clipOffset.x, -clipOffset.y),
        float2(clipOffset.x, -clipOffset.y)
    };
    [unroll]
    for (uint index = 0; index < 4; ++index) {
        VertexOutput output = input[0];
        output.position.xy += corners[index];
        outputStream.Append(output);
    }
    outputStream.RestartStrip();
}

float4 LitPS(VertexOutput input) : SV_TARGET {
    const float diffuse = abs(dot(normalize(input.normal), normalize(-lightDirectionAndAmbient.xyz)));
    const float light = saturate(lightDirectionAndAmbient.w + diffuse * (1.0f - lightDirectionAndAmbient.w));
    return float4(input.color.rgb * light, input.color.a);
}

float4 ColorPS(VertexOutput input) : SV_TARGET {
    return input.color;
}
)hlsl";

std::string HrText(const char* operation, HRESULT hr) {
    char buffer[112]{};
    std::snprintf(buffer, sizeof(buffer), "%s failed (HRESULT 0x%08lX)", operation,
                  static_cast<unsigned long>(hr));
    return buffer;
}

void SetError(std::string* output, const std::string& message) {
    if (output) *output = message;
}

std::string WideToUtf8(const wchar_t* value) {
    if (!value || !*value) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (required <= 1) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), required, nullptr, nullptr);
    result.pop_back();
    return result;
}

bool CompileShader(const char* entryPoint, const char* target, ComPtr<ID3DBlob>* output,
                   std::string* errorText) {
    if (!output) return false;
    UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
#if defined(_DEBUG)
    flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kShaderSource, std::strlen(kShaderSource),
                                  "preview_d3d12_renderer.hlsl", nullptr, nullptr,
                                  entryPoint, target, flags, 0, output->ReleaseAndGetAddressOf(),
                                  errors.GetAddressOf());
    if (SUCCEEDED(hr)) return true;
    if (errors && errors->GetBufferPointer() && errors->GetBufferSize()) {
        SetError(errorText, std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                        errors->GetBufferSize()));
    } else {
        SetError(errorText, HrText("D3DCompile", hr));
    }
    return false;
}

constexpr UINT64 AlignUp(UINT64 value, UINT64 alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

bool FitsBufferView(std::size_t byteCount) {
    return byteCount > 0 && byteCount <= static_cast<std::size_t>(std::numeric_limits<UINT>::max());
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

D3D12_RESOURCE_DESC BufferDescription(UINT64 byteCount) {
    D3D12_RESOURCE_DESC description{};
    description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    description.Width = byteCount;
    description.Height = 1;
    description.DepthOrArraySize = 1;
    description.MipLevels = 1;
    description.Format = DXGI_FORMAT_UNKNOWN;
    description.SampleDesc.Count = 1;
    description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return description;
}

XMMATRIX BuildViewProjection(const PreviewD3D12Camera& camera, float aspect) {
    const float yaw = camera.yawDegrees * kPi / 180.0f;
    const float pitch = std::clamp(camera.pitchDegrees, -89.0f, 89.0f) * kPi / 180.0f;
    const float distance = std::max(0.01f, camera.distance);
    const XMVECTOR target = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
    const float horizontal = std::cos(pitch) * distance;
    const XMVECTOR offset = XMVectorSet(std::sin(yaw) * horizontal,
                                        std::sin(pitch) * distance,
                                        -std::cos(yaw) * horizontal, 0.0f);
    const XMVECTOR eye = XMVectorAdd(target, offset);
    const XMMATRIX view = XMMatrixLookAtRH(eye, target, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX pan = XMMatrixTranslation(camera.panX, camera.panY, 0.0f);
    const float nearPlane = std::max(0.001f, camera.nearPlane);
    const float farPlane = std::max(nearPlane + 1.0f, camera.farPlane);
    const float fov = std::clamp(camera.fieldOfViewDegrees, 5.0f, 175.0f) * kPi / 180.0f;
    return XMMatrixMultiply(XMMatrixMultiply(view, pan), XMMatrixPerspectiveFovRH(
        fov, std::max(0.01f, aspect), nearPlane, farPlane));
}

XMMATRIX BuildModelMatrix(const PreviewD3D12ModelTransform& transform) {
    XMVECTOR sourceQuaternion = XMVectorSet(transform.sourceQuaternion[1], transform.sourceQuaternion[2],
                                            transform.sourceQuaternion[3], transform.sourceQuaternion[0]);
    const float sourceLengthSquared = XMVectorGetX(XMVector4LengthSq(sourceQuaternion));
    sourceQuaternion = sourceLengthSquared > 1.0e-12f
        ? XMQuaternionNormalize(sourceQuaternion) : XMQuaternionIdentity();
    XMVECTOR quaternion = XMVectorSet(transform.quaternion[1], transform.quaternion[2],
                                      transform.quaternion[3], transform.quaternion[0]);
    const float lengthSquared = XMVectorGetX(XMVector4LengthSq(quaternion));
    quaternion = lengthSquared > 1.0e-12f ? XMQuaternionNormalize(quaternion) : XMQuaternionIdentity();
    const XMMATRIX center = XMMatrixTranslation(-transform.center[0], -transform.center[1],
                                                 -transform.center[2]);
    const XMMATRIX sourceRotation = XMMatrixRotationQuaternion(sourceQuaternion);
    const XMMATRIX scale = XMMatrixScaling(transform.scale[0], transform.scale[1], transform.scale[2]);
    const XMMATRIX rotation = XMMatrixRotationQuaternion(quaternion);
    const XMMATRIX translation = XMMatrixTranslation(transform.position[0], transform.position[1],
                                                       transform.position[2]);
    return XMMatrixMultiply(
        XMMatrixMultiply(XMMatrixMultiply(XMMatrixMultiply(center, sourceRotation), scale), rotation),
        translation);
}

void AddLine(std::vector<ColorVertex>* output, const std::array<float, 3>& a,
             const std::array<float, 3>& b, const std::array<std::uint8_t, 4>& color) {
    ColorVertex first{};
    ColorVertex second{};
    std::copy(a.begin(), a.end(), first.position);
    std::copy(b.begin(), b.end(), second.position);
    std::copy(color.begin(), color.end(), first.color);
    std::copy(color.begin(), color.end(), second.color);
    output->push_back(first);
    output->push_back(second);
}

}  // namespace

struct PreviewD3D12Renderer::Impl {
    HWND window = nullptr;
    PreviewD3D12Options options{};
    bool hardwareDevice = false;
    bool deviceOperational = true;
    bool tearingSupported = false;
    UINT swapChainFlags = 0;
    std::string adapterName;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t frameNumber = 0;
    PreviewD3D12RenderStats lastStats{};
    UINT constantBlockCursor = 0;

    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<IDXGISwapChain3> swapChain;
    HANDLE frameLatencyWaitable = nullptr;

    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    UINT rtvDescriptorSize = 0;
    std::array<ComPtr<ID3D12Resource>, kFrameCount> renderTargets{};
    ComPtr<ID3D12Resource> depthTarget;

    std::array<ComPtr<ID3D12CommandAllocator>, kFrameCount> frameAllocators{};
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12CommandAllocator> uploadAllocator;
    ComPtr<ID3D12GraphicsCommandList> uploadCommandList;
    ComPtr<ID3D12Fence> fence;
    HANDLE fenceEvent = nullptr;
    UINT64 nextFenceValue = 1;
    std::array<UINT64, kFrameCount> frameFenceValues{};
    std::vector<ComPtr<ID3D12Resource>> retainedFailedUploadResources;

    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> modelSolidPipeline;
    ComPtr<ID3D12PipelineState> modelWirePipeline;
    ComPtr<ID3D12PipelineState> worldTrianglePipeline;
    ComPtr<ID3D12PipelineState> worldPointPipeline;
    ComPtr<ID3D12PipelineState> lineDepthPipeline;
    ComPtr<ID3D12PipelineState> lineOverlayPipeline;

    ComPtr<ID3D12Resource> frameConstants;
    std::uint8_t* mappedFrameConstants = nullptr;
    UINT64 constantStride = AlignUp(sizeof(FrameConstants), kConstantAlignment);

    struct OverlayBuffer {
        ComPtr<ID3D12Resource> resource;
        std::uint8_t* mapped = nullptr;
        std::size_t capacity = 0;
        D3D12_VERTEX_BUFFER_VIEW view{};
    };
    std::array<OverlayBuffer, kFrameCount> overlayBuffers{};

    ComPtr<ID3D12Resource> modelVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW modelVertexView{};
    std::array<ComPtr<ID3D12Resource>, 4> modelIndexBuffers{};
    std::array<D3D12_INDEX_BUFFER_VIEW, 4> modelIndexViews{};
    std::array<UINT, 4> modelIndexCounts{};

    ComPtr<ID3D12Resource> worldVertexBuffer;
    D3D12_VERTEX_BUFFER_VIEW worldVertexView{};
    ComPtr<ID3D12Resource> worldIndexBuffer;
    D3D12_INDEX_BUFFER_VIEW worldIndexView{};
    UINT worldVertexCount = 0;
    UINT worldIndexCount = 0;
    PreviewD3D12WorldPrimitive worldPrimitive = PreviewD3D12WorldPrimitive::Quads;

    ~Impl() {
        if (mappedFrameConstants && frameConstants) frameConstants->Unmap(0, nullptr);
        mappedFrameConstants = nullptr;
        for (auto& overlay : overlayBuffers) {
            if (overlay.mapped && overlay.resource) overlay.resource->Unmap(0, nullptr);
            overlay.mapped = nullptr;
        }
        if (frameLatencyWaitable) CloseHandle(frameLatencyWaitable);
        if (fenceEvent) CloseHandle(fenceEvent);
    }

    bool DeviceFailure(const char* operation, HRESULT hr, std::string* errorText) {
        deviceOperational = false;
        if (device && (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET ||
                       hr == DXGI_ERROR_DEVICE_HUNG)) {
            SetError(errorText, HrText("D3D12 device removed", device->GetDeviceRemovedReason()));
        } else {
            SetError(errorText, HrText(operation, hr));
        }
        return false;
    }

    bool CreateDeviceAndSwapChain(std::string* errorText) {
        RECT bounds{};
        GetClientRect(window, &bounds);
        width = static_cast<std::uint32_t>(std::max<LONG>(1, bounds.right - bounds.left));
        height = static_cast<std::uint32_t>(std::max<LONG>(1, bounds.bottom - bounds.top));

        ComPtr<IDXGIFactory4> factory;
        HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(factory.GetAddressOf()));
        if (FAILED(hr)) return DeviceFailure("CreateDXGIFactory2", hr, errorText);

        auto tryAdapter = [this](IDXGIAdapter1* adapter) {
            ComPtr<ID3D12Device> candidate;
            const HRESULT result = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                                                      IID_PPV_ARGS(candidate.GetAddressOf()));
            if (FAILED(result)) return false;
            device = std::move(candidate);
            DXGI_ADAPTER_DESC1 description{};
            if (SUCCEEDED(adapter->GetDesc1(&description))) adapterName = WideToUtf8(description.Description);
            return true;
        };

        ComPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(factory.As(&factory6))) {
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> adapter;
                hr = factory6->EnumAdapterByGpuPreference(
                    index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    IID_PPV_ARGS(adapter.GetAddressOf()));
                if (hr == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(hr)) break;
                DXGI_ADAPTER_DESC1 description{};
                if (FAILED(adapter->GetDesc1(&description)) ||
                    (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
                if (tryAdapter(adapter.Get())) break;
            }
        }
        if (!device) {
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> adapter;
                hr = factory->EnumAdapters1(index, adapter.GetAddressOf());
                if (hr == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(hr)) break;
                DXGI_ADAPTER_DESC1 description{};
                if (FAILED(adapter->GetDesc1(&description)) ||
                    (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
                if (tryAdapter(adapter.Get())) break;
            }
        }
        hardwareDevice = device != nullptr;

        if (!device && options.allowWarpFallback) {
            ComPtr<IDXGIAdapter> warpAdapter;
            hr = factory->EnumWarpAdapter(IID_PPV_ARGS(warpAdapter.GetAddressOf()));
            if (SUCCEEDED(hr)) {
                hr = D3D12CreateDevice(warpAdapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                       IID_PPV_ARGS(device.GetAddressOf()));
                if (SUCCEEDED(hr)) {
                    ComPtr<IDXGIAdapter1> warpAdapter1;
                    DXGI_ADAPTER_DESC1 description{};
                    if (SUCCEEDED(warpAdapter.As(&warpAdapter1)) &&
                        SUCCEEDED(warpAdapter1->GetDesc1(&description))) {
                        adapterName = WideToUtf8(description.Description);
                    }
                }
            }
            hardwareDevice = false;
        }
        if (!device) {
            SetError(errorText, "No D3D12 feature-level 11.0 hardware adapter was available");
            return false;
        }
        if (adapterName.empty()) adapterName = hardwareDevice ? "D3D12 hardware" : "D3D12 WARP";

        D3D12_COMMAND_QUEUE_DESC queueDescription{};
        queueDescription.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        hr = device->CreateCommandQueue(&queueDescription, IID_PPV_ARGS(commandQueue.GetAddressOf()));
        if (FAILED(hr)) return DeviceFailure("ID3D12Device::CreateCommandQueue", hr, errorText);

        ComPtr<IDXGIFactory5> factory5;
        BOOL tearing = FALSE;
        if (SUCCEEDED(factory.As(&factory5)) &&
            SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                     &tearing, sizeof(tearing)))) {
            tearingSupported = tearing == TRUE;
        }
        swapChainFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (tearingSupported) swapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        DXGI_SWAP_CHAIN_DESC1 swapDescription{};
        swapDescription.Width = width;
        swapDescription.Height = height;
        swapDescription.Format = kBackBufferFormat;
        swapDescription.SampleDesc.Count = 1;
        swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDescription.BufferCount = kFrameCount;
        swapDescription.Scaling = DXGI_SCALING_STRETCH;
        swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDescription.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        swapDescription.Flags = swapChainFlags;

        ComPtr<IDXGISwapChain1> swapChain1;
        hr = factory->CreateSwapChainForHwnd(commandQueue.Get(), window, &swapDescription,
                                              nullptr, nullptr, swapChain1.GetAddressOf());
        if (FAILED(hr)) return DeviceFailure("IDXGIFactory::CreateSwapChainForHwnd", hr, errorText);
        hr = swapChain1.As(&swapChain);
        if (FAILED(hr)) return DeviceFailure("IDXGISwapChain1::QueryInterface", hr, errorText);
        factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
        hr = swapChain->SetMaximumFrameLatency(std::clamp(options.maximumFrameLatency, 1u, 16u));
        if (FAILED(hr)) return DeviceFailure("IDXGISwapChain::SetMaximumFrameLatency", hr, errorText);
        frameLatencyWaitable = swapChain->GetFrameLatencyWaitableObject();
        if (!frameLatencyWaitable) {
            SetError(errorText, "IDXGISwapChain::GetFrameLatencyWaitableObject returned no handle");
            return false;
        }
        return true;
    }

    bool WaitForFence(UINT64 value, std::string* errorText) {
        if (!fence || value == 0) return true;
        const UINT64 completedValue = fence->GetCompletedValue();
        if (completedValue == std::numeric_limits<UINT64>::max()) {
            return DeviceFailure("D3D12 fence completion", device->GetDeviceRemovedReason(), errorText);
        }
        if (completedValue >= value) return true;
        HRESULT hr = fence->SetEventOnCompletion(value, fenceEvent);
        if (FAILED(hr)) return DeviceFailure("ID3D12Fence::SetEventOnCompletion", hr, errorText);
        const DWORD waitResult = WaitForSingleObject(fenceEvent, INFINITE);
        if (waitResult == WAIT_OBJECT_0) return true;
        SetError(errorText, "D3D12 fence wait failed (Win32 error " +
                            std::to_string(GetLastError()) + ")");
        return false;
    }

    bool SignalAndWait(std::string* errorText) {
        const UINT64 value = nextFenceValue++;
        const HRESULT hr = commandQueue->Signal(fence.Get(), value);
        if (FAILED(hr)) return DeviceFailure("ID3D12CommandQueue::Signal", hr, errorText);
        return WaitForFence(value, errorText);
    }

    bool WaitForGpu(std::string* errorText) {
        if (!commandQueue || !fence || !fenceEvent) return true;
        return SignalAndWait(errorText);
    }

    bool CreateFrameResources(std::string* errorText) {
        D3D12_DESCRIPTOR_HEAP_DESC rtvDescription{};
        rtvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvDescription.NumDescriptors = kFrameCount;
        HRESULT hr = device->CreateDescriptorHeap(&rtvDescription,
                                                   IID_PPV_ARGS(rtvHeap.GetAddressOf()));
        if (FAILED(hr)) return DeviceFailure("ID3D12Device::CreateDescriptorHeap(RTV)", hr, errorText);
        rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_DESCRIPTOR_HEAP_DESC dsvDescription{};
        dsvDescription.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        dsvDescription.NumDescriptors = 1;
        hr = device->CreateDescriptorHeap(&dsvDescription, IID_PPV_ARGS(dsvHeap.GetAddressOf()));
        if (FAILED(hr)) return DeviceFailure("ID3D12Device::CreateDescriptorHeap(DSV)", hr, errorText);

        for (auto& allocator : frameAllocators) {
            hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(allocator.GetAddressOf()));
            if (FAILED(hr)) {
                return DeviceFailure("ID3D12Device::CreateCommandAllocator(frame)", hr, errorText);
            }
        }
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frameAllocators[0].Get(),
                                       nullptr, IID_PPV_ARGS(commandList.GetAddressOf()));
        if (FAILED(hr)) return DeviceFailure("ID3D12Device::CreateCommandList(frame)", hr, errorText);
        hr = commandList->Close();
        if (FAILED(hr)) return DeviceFailure("ID3D12GraphicsCommandList::Close(frame)", hr, errorText);

        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                             IID_PPV_ARGS(uploadAllocator.GetAddressOf()));
        if (FAILED(hr)) return DeviceFailure("ID3D12Device::CreateCommandAllocator(upload)", hr, errorText);
        hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploadAllocator.Get(),
                                       nullptr, IID_PPV_ARGS(uploadCommandList.GetAddressOf()));
        if (FAILED(hr)) return DeviceFailure("ID3D12Device::CreateCommandList(upload)", hr, errorText);
        hr = uploadCommandList->Close();
        if (FAILED(hr)) return DeviceFailure("ID3D12GraphicsCommandList::Close(upload)", hr, errorText);

        hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.GetAddressOf()));
        if (FAILED(hr)) return DeviceFailure("ID3D12Device::CreateFence", hr, errorText);
        fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!fenceEvent) {
            SetError(errorText, "CreateEventW for the D3D12 fence failed (Win32 error " +
                                std::to_string(GetLastError()) + ")");
            return false;
        }

        const UINT64 constantBytes = constantStride * kConstantBlocksPerFrame * kFrameCount;
        const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC constantsDescription = BufferDescription(constantBytes);
        hr = device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &constantsDescription,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(frameConstants.GetAddressOf()));
        if (FAILED(hr)) return DeviceFailure("ID3D12Device::CreateCommittedResource(constants)", hr, errorText);
        const D3D12_RANGE noRead{ 0, 0 };
        hr = frameConstants->Map(0, &noRead, reinterpret_cast<void**>(&mappedFrameConstants));
        if (FAILED(hr)) return DeviceFailure("ID3D12Resource::Map(constants)", hr, errorText);
        return true;
    }

    bool CreateTargets(std::string* errorText) {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT index = 0; index < kFrameCount; ++index) {
            HRESULT hr = swapChain->GetBuffer(index, IID_PPV_ARGS(renderTargets[index].GetAddressOf()));
            if (FAILED(hr)) return DeviceFailure("IDXGISwapChain::GetBuffer", hr, errorText);
            device->CreateRenderTargetView(renderTargets[index].Get(), nullptr, rtv);
            rtv.ptr += rtvDescriptorSize;
        }

        D3D12_RESOURCE_DESC depthDescription{};
        depthDescription.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDescription.Width = width;
        depthDescription.Height = height;
        depthDescription.DepthOrArraySize = 1;
        depthDescription.MipLevels = 1;
        depthDescription.Format = kDepthFormat;
        depthDescription.SampleDesc.Count = 1;
        depthDescription.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depthDescription.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clearValue{};
        clearValue.Format = kDepthFormat;
        clearValue.DepthStencil.Depth = 1.0f;
        const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        HRESULT hr = device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &depthDescription,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue, IID_PPV_ARGS(depthTarget.GetAddressOf()));
        if (FAILED(hr)) return DeviceFailure("ID3D12Device::CreateCommittedResource(depth)", hr, errorText);

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDescription{};
        dsvDescription.Format = kDepthFormat;
        dsvDescription.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(depthTarget.Get(), &dsvDescription,
                                       dsvHeap->GetCPUDescriptorHandleForHeapStart());
        return true;
    }

    bool CreatePipelineState(ID3DBlob* vertexShader, ID3DBlob* pixelShader,
                             ID3DBlob* geometryShader,
                             const D3D12_INPUT_ELEMENT_DESC* elements, UINT elementCount,
                             D3D12_PRIMITIVE_TOPOLOGY_TYPE topologyType,
                             D3D12_FILL_MODE fillMode, bool depthEnabled, INT depthBias,
                             float slopeScaledDepthBias, ComPtr<ID3D12PipelineState>* output,
                             std::string* errorText) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
        description.pRootSignature = rootSignature.Get();
        description.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
        description.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
        if (geometryShader) {
            description.GS = { geometryShader->GetBufferPointer(), geometryShader->GetBufferSize() };
        }

        description.BlendState.AlphaToCoverageEnable = FALSE;
        description.BlendState.IndependentBlendEnable = FALSE;
        auto& blend = description.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.LogicOpEnable = FALSE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        description.SampleMask = std::numeric_limits<UINT>::max();
        description.RasterizerState.FillMode = fillMode;
        description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        description.RasterizerState.FrontCounterClockwise = FALSE;
        description.RasterizerState.DepthBias = depthBias;
        description.RasterizerState.DepthBiasClamp = 0.0f;
        description.RasterizerState.SlopeScaledDepthBias = slopeScaledDepthBias;
        description.RasterizerState.DepthClipEnable = TRUE;
        description.RasterizerState.MultisampleEnable = FALSE;
        description.RasterizerState.AntialiasedLineEnable = FALSE;
        description.RasterizerState.ForcedSampleCount = 0;
        description.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

        description.DepthStencilState.DepthEnable = depthEnabled ? TRUE : FALSE;
        description.DepthStencilState.DepthWriteMask = depthEnabled
            ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        description.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        description.DepthStencilState.StencilEnable = FALSE;
        description.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
        description.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
        description.InputLayout = { elements, elementCount };
        description.PrimitiveTopologyType = topologyType;
        description.NumRenderTargets = 1;
        description.RTVFormats[0] = kBackBufferFormat;
        description.DSVFormat = kDepthFormat;
        description.SampleDesc.Count = 1;

        const HRESULT hr = device->CreateGraphicsPipelineState(
            &description, IID_PPV_ARGS(output->ReleaseAndGetAddressOf()));
        if (FAILED(hr)) return DeviceFailure("ID3D12Device::CreateGraphicsPipelineState", hr, errorText);
        return true;
    }

    bool CreatePipeline(std::string* errorText) {
        ComPtr<ID3DBlob> vertexBytecode;
        ComPtr<ID3DBlob> colorVertexBytecode;
        ComPtr<ID3DBlob> litPixelBytecode;
        ComPtr<ID3DBlob> colorPixelBytecode;
        ComPtr<ID3DBlob> pointGeometryBytecode;
        if (!CompileShader("MainVS", "vs_5_1", &vertexBytecode, errorText) ||
            !CompileShader("ColorVS", "vs_5_1", &colorVertexBytecode, errorText) ||
            !CompileShader("LitPS", "ps_5_1", &litPixelBytecode, errorText) ||
            !CompileShader("ColorPS", "ps_5_1", &colorPixelBytecode, errorText) ||
            !CompileShader("PointGS", "gs_5_1", &pointGeometryBytecode, errorText)) return false;

        D3D12_ROOT_PARAMETER rootParameter{};
        rootParameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParameter.Descriptor.ShaderRegister = 0;
        rootParameter.Descriptor.RegisterSpace = 0;
        rootParameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rootDescription{};
        rootDescription.NumParameters = 1;
        rootDescription.pParameters = &rootParameter;
        rootDescription.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> rootBytecode;
        ComPtr<ID3DBlob> rootErrors;
        HRESULT hr = D3D12SerializeRootSignature(&rootDescription, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  rootBytecode.GetAddressOf(), rootErrors.GetAddressOf());
        if (FAILED(hr)) {
            if (rootErrors && rootErrors->GetBufferPointer() && rootErrors->GetBufferSize()) {
                SetError(errorText, std::string(static_cast<const char*>(rootErrors->GetBufferPointer()),
                                                rootErrors->GetBufferSize()));
            } else {
                SetError(errorText, HrText("D3D12SerializeRootSignature", hr));
            }
            return false;
        }
        hr = device->CreateRootSignature(0, rootBytecode->GetBufferPointer(),
                                         rootBytecode->GetBufferSize(),
                                         IID_PPV_ARGS(rootSignature.GetAddressOf()));
        if (FAILED(hr)) return DeviceFailure("ID3D12Device::CreateRootSignature", hr, errorText);

        constexpr std::array<D3D12_INPUT_ELEMENT_DESC, 3> modelLayout = {{
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        }};
        constexpr std::array<D3D12_INPUT_ELEMENT_DESC, 2> colorLayout = {{
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        }};

        return CreatePipelineState(vertexBytecode.Get(), litPixelBytecode.Get(), nullptr,
                                   modelLayout.data(),
                                   static_cast<UINT>(modelLayout.size()),
                                   D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, D3D12_FILL_MODE_SOLID,
                                   true, 0, 0.0f, &modelSolidPipeline, errorText) &&
            CreatePipelineState(vertexBytecode.Get(), colorPixelBytecode.Get(), nullptr,
                                modelLayout.data(),
                                static_cast<UINT>(modelLayout.size()),
                                D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, D3D12_FILL_MODE_WIREFRAME,
                                true, -1, -0.5f, &modelWirePipeline, errorText) &&
            CreatePipelineState(colorVertexBytecode.Get(), colorPixelBytecode.Get(), nullptr,
                                colorLayout.data(),
                                static_cast<UINT>(colorLayout.size()),
                                D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE, D3D12_FILL_MODE_SOLID,
                                true, 0, 0.0f, &worldTrianglePipeline, errorText) &&
            CreatePipelineState(colorVertexBytecode.Get(), colorPixelBytecode.Get(),
                                pointGeometryBytecode.Get(), colorLayout.data(),
                                static_cast<UINT>(colorLayout.size()),
                                D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT, D3D12_FILL_MODE_SOLID,
                                true, 0, 0.0f, &worldPointPipeline, errorText) &&
            CreatePipelineState(colorVertexBytecode.Get(), colorPixelBytecode.Get(), nullptr,
                                colorLayout.data(),
                                static_cast<UINT>(colorLayout.size()),
                                D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, D3D12_FILL_MODE_SOLID,
                                true, 0, 0.0f, &lineDepthPipeline, errorText) &&
            CreatePipelineState(colorVertexBytecode.Get(), colorPixelBytecode.Get(), nullptr,
                                colorLayout.data(),
                                static_cast<UINT>(colorLayout.size()),
                                D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE, D3D12_FILL_MODE_SOLID,
                                false, 0, 0.0f, &lineOverlayPipeline, errorText);
    }

    bool BeginUpload(std::string* errorText) {
        if (!WaitForGpu(errorText)) return false;
        HRESULT hr = uploadAllocator->Reset();
        if (FAILED(hr)) return DeviceFailure("ID3D12CommandAllocator::Reset(upload)", hr, errorText);
        hr = uploadCommandList->Reset(uploadAllocator.Get(), nullptr);
        if (FAILED(hr)) return DeviceFailure("ID3D12GraphicsCommandList::Reset(upload)", hr, errorText);
        return true;
    }

    void AbortUpload() {
        if (uploadCommandList) uploadCommandList->Close();
    }

    bool RecordStaticBuffer(const void* data, std::size_t byteCount,
                            D3D12_RESOURCE_STATES finalState,
                            ComPtr<ID3D12Resource>* output,
                            std::vector<ComPtr<ID3D12Resource>>* stagingResources,
                            std::string* errorText) {
        if (!data || !output || !stagingResources || !FitsBufferView(byteCount)) {
            SetError(errorText, "D3D12 buffer is empty or exceeds the 4 GiB view limit");
            return false;
        }
        const UINT64 resourceBytes = static_cast<UINT64>(byteCount);
        const D3D12_RESOURCE_DESC description = BufferDescription(resourceBytes);
        const D3D12_HEAP_PROPERTIES defaultHeap = HeapProperties(D3D12_HEAP_TYPE_DEFAULT);
        const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        ComPtr<ID3D12Resource> destination;
        HRESULT hr = device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(destination.GetAddressOf()));
        if (FAILED(hr)) return DeviceFailure("ID3D12Device::CreateCommittedResource(default buffer)",
                                              hr, errorText);
        ComPtr<ID3D12Resource> staging;
        hr = device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(staging.GetAddressOf()));
        if (FAILED(hr)) return DeviceFailure("ID3D12Device::CreateCommittedResource(upload buffer)",
                                              hr, errorText);
        void* mapped = nullptr;
        const D3D12_RANGE noRead{ 0, 0 };
        hr = staging->Map(0, &noRead, &mapped);
        if (FAILED(hr)) return DeviceFailure("ID3D12Resource::Map(upload buffer)", hr, errorText);
        std::memcpy(mapped, data, byteCount);
        const D3D12_RANGE written{ 0, byteCount };
        staging->Unmap(0, &written);

        uploadCommandList->CopyBufferRegion(destination.Get(), 0, staging.Get(), 0, resourceBytes);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = destination.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = finalState;
        uploadCommandList->ResourceBarrier(1, &barrier);
        stagingResources->push_back(destination);
        stagingResources->push_back(staging);
        *output = std::move(destination);
        return true;
    }

    bool FinishUpload(std::vector<ComPtr<ID3D12Resource>>* lifetimeResources,
                      std::string* errorText) {
        HRESULT hr = uploadCommandList->Close();
        if (FAILED(hr)) {
            deviceOperational = false;
            return DeviceFailure("ID3D12GraphicsCommandList::Close(upload)", hr, errorText);
        }
        ID3D12CommandList* lists[] = { uploadCommandList.Get() };
        commandQueue->ExecuteCommandLists(1, lists);
        if (SignalAndWait(errorText)) return true;
        if (lifetimeResources) {
            retainedFailedUploadResources.swap(*lifetimeResources);
        }
        deviceOperational = false;
        return false;
    }

    bool SetConstants(UINT frameIndex, const XMMATRIX& world, const XMMATRIX& viewProjection,
                      std::string* errorText) {
        if (constantBlockCursor >= kConstantBlocksPerFrame) {
            SetError(errorText, "D3D12 frame exceeded its constant-buffer block budget");
            return false;
        }
        FrameConstants constants{};
        XMStoreFloat4x4(&constants.world, world);
        XMStoreFloat4x4(&constants.worldViewProjection, XMMatrixMultiply(world, viewProjection));
        XMVECTOR determinant{};
        const XMMATRIX inverseWorld = XMMatrixInverse(&determinant, world);
        const XMMATRIX normalWorld = std::abs(XMVectorGetX(determinant)) > 1.0e-12f
            ? XMMatrixTranspose(inverseWorld) : world;
        XMStoreFloat4x4(&constants.normalWorld, normalWorld);
        constants.lightDirectionAndAmbient = XMFLOAT4(0.4f, 1.0f, 0.6f, 0.22f);
        constants.viewportAndPointSize = XMFLOAT4(
            static_cast<float>(width), static_cast<float>(height), 3.0f, 0.0f);
        const UINT64 blockIndex = static_cast<UINT64>(frameIndex) * kConstantBlocksPerFrame +
                                  constantBlockCursor++;
        const UINT64 offset = blockIndex * constantStride;
        std::memcpy(mappedFrameConstants + static_cast<std::size_t>(offset), &constants,
                    sizeof(constants));
        commandList->SetGraphicsRootConstantBufferView(
            0, frameConstants->GetGPUVirtualAddress() + offset);
        return true;
    }

    bool EnsureOverlayCapacity(UINT frameIndex, std::size_t vertexCount,
                               std::string* errorText) {
        OverlayBuffer& overlay = overlayBuffers[frameIndex];
        if (overlay.resource && overlay.mapped && vertexCount <= overlay.capacity) return true;
        const std::size_t newCapacity = std::max<std::size_t>(256, vertexCount + vertexCount / 2);
        if (newCapacity > std::numeric_limits<std::size_t>::max() / sizeof(ColorVertex) ||
            !FitsBufferView(newCapacity * sizeof(ColorVertex))) {
            SetError(errorText, "D3D12 overlay buffer exceeds the API size limit");
            return false;
        }
        if (overlay.mapped && overlay.resource) overlay.resource->Unmap(0, nullptr);
        overlay = {};
        const std::size_t byteCount = newCapacity * sizeof(ColorVertex);
        const D3D12_HEAP_PROPERTIES uploadHeap = HeapProperties(D3D12_HEAP_TYPE_UPLOAD);
        const D3D12_RESOURCE_DESC description = BufferDescription(static_cast<UINT64>(byteCount));
        HRESULT hr = device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &description, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(overlay.resource.GetAddressOf()));
        if (FAILED(hr)) return DeviceFailure("ID3D12Device::CreateCommittedResource(overlay)",
                                              hr, errorText);
        const D3D12_RANGE noRead{ 0, 0 };
        hr = overlay.resource->Map(0, &noRead, reinterpret_cast<void**>(&overlay.mapped));
        if (FAILED(hr)) return DeviceFailure("ID3D12Resource::Map(overlay)", hr, errorText);
        overlay.capacity = newCapacity;
        overlay.view.BufferLocation = overlay.resource->GetGPUVirtualAddress();
        overlay.view.SizeInBytes = static_cast<UINT>(byteCount);
        overlay.view.StrideInBytes = sizeof(ColorVertex);
        return true;
    }

    bool DrawLines(UINT frameIndex, UINT startVertex, UINT vertexCount,
                   const XMMATRIX& viewProjection, bool depthEnabled,
                   PreviewD3D12RenderStats* stats, std::string* errorText) {
        if (vertexCount == 0) return true;
        if (!SetConstants(frameIndex, XMMatrixIdentity(), viewProjection, errorText)) return false;
        commandList->SetPipelineState(depthEnabled ? lineDepthPipeline.Get() : lineOverlayPipeline.Get());
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        commandList->IASetVertexBuffers(0, 1, &overlayBuffers[frameIndex].view);
        commandList->DrawInstanced(vertexCount, 1, startVertex, 0);
        if (stats) ++stats->drawCalls;
        return true;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE RenderTargetHandle(UINT frameIndex) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(frameIndex) * rtvDescriptorSize;
        return handle;
    }
};

PreviewD3D12Renderer::PreviewD3D12Renderer() : impl_(std::make_unique<Impl>()) {}

PreviewD3D12Renderer::~PreviewD3D12Renderer() {
    Shutdown();
}

PreviewD3D12Renderer::PreviewD3D12Renderer(PreviewD3D12Renderer&& other) noexcept
    : impl_(std::move(other.impl_)) {}

PreviewD3D12Renderer& PreviewD3D12Renderer::operator=(PreviewD3D12Renderer&& other) noexcept {
    if (this != &other) {
        Shutdown();
        impl_ = std::move(other.impl_);
    }
    return *this;
}

bool PreviewD3D12Renderer::Initialize(PreviewD3D12NativeWindow window,
                                      const PreviewD3D12Options& options,
                                      std::string* errorText) {
    Shutdown();
    impl_ = std::make_unique<Impl>();
    impl_->window = static_cast<HWND>(window);
    impl_->options = options;
    if (!impl_->window || !IsWindow(impl_->window)) {
        SetError(errorText, "PreviewD3D12Renderer requires a valid HWND");
        Shutdown();
        return false;
    }
    if (!impl_->CreateDeviceAndSwapChain(errorText) ||
        !impl_->CreateFrameResources(errorText) ||
        !impl_->CreateTargets(errorText) ||
        !impl_->CreatePipeline(errorText)) {
        Shutdown();
        return false;
    }
    return true;
}

bool PreviewD3D12Renderer::Resize(std::uint32_t width, std::uint32_t height,
                                  std::string* errorText) {
    if (!impl_ || !impl_->swapChain || !impl_->commandQueue) {
        SetError(errorText, "D3D12 renderer is not initialized");
        return false;
    }
    if (width == 0 || height == 0) return true;
    if (width == impl_->width && height == impl_->height && impl_->depthTarget &&
        impl_->renderTargets[0]) return true;
    if (!impl_->WaitForGpu(errorText)) return false;
    for (auto& target : impl_->renderTargets) target.Reset();
    impl_->depthTarget.Reset();
    const HRESULT hr = impl_->swapChain->ResizeBuffers(
        kFrameCount, width, height, kBackBufferFormat, impl_->swapChainFlags);
    if (FAILED(hr)) return impl_->DeviceFailure("IDXGISwapChain::ResizeBuffers", hr, errorText);
    impl_->width = width;
    impl_->height = height;
    impl_->frameFenceValues.fill(0);
    return impl_->CreateTargets(errorText);
}

void PreviewD3D12Renderer::Shutdown() {
    if (!impl_) return;
    if (!impl_->WaitForGpu(nullptr) && impl_->device &&
        SUCCEEDED(impl_->device->GetDeviceRemovedReason())) {
        // A live device without a completed fence may still reference every
        // resource in Impl. Quarantine it until process teardown rather than
        // risking a driver use-after-free on this catastrophic sync failure.
        static_cast<void>(impl_.release());
        return;
    }
    impl_.reset();
}

bool PreviewD3D12Renderer::UploadMesh(const PreviewMesh& mesh, std::string* errorText) {
    if (!IsReady()) {
        SetError(errorText, "D3D12 renderer is not initialized");
        return false;
    }
    const std::size_t vertexCount = mesh.positions.size() / 3;
    if (vertexCount == 0 || mesh.positions.size() != vertexCount * 3 || mesh.indices.empty()) {
        SetError(errorText, "Preview mesh has no valid vertices or indices");
        return false;
    }
    if (vertexCount > std::numeric_limits<std::uint32_t>::max() ||
        vertexCount > std::numeric_limits<UINT>::max() / sizeof(RenderVertex)) {
        SetError(errorText, "Preview mesh vertex data exceeds the D3D12 buffer-view limit");
        return false;
    }
    const std::size_t vertexBytes = vertexCount * sizeof(RenderVertex);
    std::vector<RenderVertex> vertices;
    std::vector<ComPtr<ID3D12Resource>> stagingResources;
    try {
        vertices.resize(vertexCount);
        stagingResources.reserve(10);
    } catch (const std::bad_alloc&) {
        SetError(errorText, "Not enough memory to prepare the D3D12 preview mesh upload");
        return false;
    }
    const bool hasNormals = mesh.normals.size() == vertexCount * 3;
    const bool hasColors = mesh.colors.size() == vertexCount * 4;
    for (std::size_t index = 0; index < vertexCount; ++index) {
        std::copy_n(mesh.positions.data() + index * 3, 3, vertices[index].position);
        if (hasNormals) {
            std::copy_n(mesh.normals.data() + index * 3, 3, vertices[index].normal);
        } else {
            vertices[index].normal[0] = 0.0f;
            vertices[index].normal[1] = 1.0f;
            vertices[index].normal[2] = 0.0f;
        }
        if (hasColors) {
            std::copy_n(mesh.colors.data() + index * 4, 4, vertices[index].color);
        } else {
            vertices[index].color[0] = 205;
            vertices[index].color[1] = 210;
            vertices[index].color[2] = 215;
            vertices[index].color[3] = 255;
        }
    }
    if (!impl_->BeginUpload(errorText)) return false;
    ComPtr<ID3D12Resource> vertexBuffer;
    if (!impl_->RecordStaticBuffer(vertices.data(), vertexBytes,
                                   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                                   &vertexBuffer, &stagingResources, errorText)) {
        impl_->AbortUpload();
        return false;
    }
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    vertexView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
    vertexView.SizeInBytes = static_cast<UINT>(vertexBytes);
    vertexView.StrideInBytes = sizeof(RenderVertex);

    std::array<ComPtr<ID3D12Resource>, 4> indexBuffers{};
    std::array<D3D12_INDEX_BUFFER_VIEW, 4> indexViews{};
    std::array<UINT, 4> indexCounts{};
    for (std::size_t level = 0; level < indexBuffers.size(); ++level) {
        if (level > 0 && mesh.lodIndices[level].empty()) {
            indexBuffers[level] = indexBuffers[0];
            indexViews[level] = indexViews[0];
            indexCounts[level] = indexCounts[0];
            continue;
        }
        const auto& indices = level == 0 ? mesh.indices : mesh.lodIndices[level];
        if (indices.empty() || indices.size() > std::numeric_limits<UINT>::max()) {
            SetError(errorText, "Preview mesh LOD index buffer is empty or too large");
            impl_->AbortUpload();
            return false;
        }
        if (std::any_of(indices.begin(), indices.end(),
                        [vertexCount](std::uint32_t index) { return index >= vertexCount; })) {
            SetError(errorText, "Preview mesh LOD contains an out-of-range vertex index");
            impl_->AbortUpload();
            return false;
        }
        const std::size_t indexBytes = indices.size() * sizeof(std::uint32_t);
        if (!impl_->RecordStaticBuffer(indices.data(), indexBytes, D3D12_RESOURCE_STATE_INDEX_BUFFER,
                                       &indexBuffers[level], &stagingResources, errorText)) {
            impl_->AbortUpload();
            return false;
        }
        indexViews[level].BufferLocation = indexBuffers[level]->GetGPUVirtualAddress();
        indexViews[level].SizeInBytes = static_cast<UINT>(indexBytes);
        indexViews[level].Format = DXGI_FORMAT_R32_UINT;
        indexCounts[level] = static_cast<UINT>(indices.size());
    }
    if (!impl_->FinishUpload(&stagingResources, errorText)) return false;
    impl_->modelVertexBuffer = std::move(vertexBuffer);
    impl_->modelVertexView = vertexView;
    impl_->modelIndexBuffers = std::move(indexBuffers);
    impl_->modelIndexViews = indexViews;
    impl_->modelIndexCounts = indexCounts;
    return true;
}

bool PreviewD3D12Renderer::UploadWorldMesh(const PreviewD3D12WorldMeshView& mesh,
                                           std::string* errorText) {
    if (!IsReady()) {
        SetError(errorText, "D3D12 renderer is not initialized");
        return false;
    }
    if (!mesh.positions || !mesh.colors || mesh.positionFloatCount == 0 ||
        mesh.positionFloatCount % 3 != 0 || (mesh.colorChannels != 3 && mesh.colorChannels != 4)) {
        SetError(errorText, "World preview mesh has invalid position or color data");
        return false;
    }
    const std::size_t vertexCount = mesh.positionFloatCount / 3;
    if (vertexCount > std::numeric_limits<UINT>::max() / sizeof(ColorVertex) ||
        vertexCount > std::numeric_limits<std::size_t>::max() / mesh.colorChannels ||
        mesh.colorByteCount < vertexCount * mesh.colorChannels) {
        SetError(errorText, "World preview mesh color data is short or the vertex count is too large");
        return false;
    }
    if (mesh.primitive == PreviewD3D12WorldPrimitive::Quads && vertexCount % 4 != 0) {
        SetError(errorText, "World preview quad vertex count must be divisible by four");
        return false;
    }
    if (mesh.primitive == PreviewD3D12WorldPrimitive::Triangles && vertexCount % 3 != 0) {
        SetError(errorText, "World preview triangle vertex count must be divisible by three");
        return false;
    }

    const std::size_t vertexBytes = vertexCount * sizeof(ColorVertex);
    const std::size_t quadCount = mesh.primitive == PreviewD3D12WorldPrimitive::Quads
        ? vertexCount / 4 : 0;
    if (quadCount > std::numeric_limits<UINT>::max() /
                        (6u * static_cast<UINT>(sizeof(std::uint32_t)))) {
        SetError(errorText, "World preview quad index buffer exceeds the D3D12 view size limit");
        return false;
    }
    std::vector<ColorVertex> vertices;
    std::vector<std::uint32_t> quadIndices;
    std::vector<ComPtr<ID3D12Resource>> stagingResources;
    try {
        vertices.resize(vertexCount);
        if (quadCount > 0) quadIndices.reserve(quadCount * 6);
        stagingResources.reserve(4);
    } catch (const std::bad_alloc&) {
        SetError(errorText, "Not enough memory to prepare the D3D12 world preview upload");
        return false;
    }
    for (std::size_t index = 0; index < vertexCount; ++index) {
        std::copy_n(mesh.positions + index * 3, 3, vertices[index].position);
        const std::size_t colorOffset = index * mesh.colorChannels;
        vertices[index].color[0] = mesh.colors[colorOffset];
        vertices[index].color[1] = mesh.colors[colorOffset + 1];
        vertices[index].color[2] = mesh.colors[colorOffset + 2];
        vertices[index].color[3] = mesh.colorChannels == 4 ? mesh.colors[colorOffset + 3] : 255;
    }
    if (quadCount > 0) {
        for (std::size_t quad = 0; quad < quadCount; ++quad) {
            const std::uint32_t base = static_cast<std::uint32_t>(quad * 4);
            quadIndices.insert(quadIndices.end(),
                               { base, base + 1, base + 2, base, base + 2, base + 3 });
        }
    }

    if (!impl_->BeginUpload(errorText)) return false;
    ComPtr<ID3D12Resource> vertexBuffer;
    if (!impl_->RecordStaticBuffer(vertices.data(), vertexBytes,
                                   D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
                                   &vertexBuffer, &stagingResources, errorText)) {
        impl_->AbortUpload();
        return false;
    }
    D3D12_VERTEX_BUFFER_VIEW vertexView{};
    vertexView.BufferLocation = vertexBuffer->GetGPUVirtualAddress();
    vertexView.SizeInBytes = static_cast<UINT>(vertexBytes);
    vertexView.StrideInBytes = sizeof(ColorVertex);

    ComPtr<ID3D12Resource> indexBuffer;
    D3D12_INDEX_BUFFER_VIEW indexView{};
    UINT indexCount = 0;
    if (!quadIndices.empty()) {
        const std::size_t indexBytes = quadIndices.size() * sizeof(std::uint32_t);
        if (!impl_->RecordStaticBuffer(quadIndices.data(), indexBytes,
                                       D3D12_RESOURCE_STATE_INDEX_BUFFER, &indexBuffer,
                                       &stagingResources, errorText)) {
            impl_->AbortUpload();
            return false;
        }
        indexView.BufferLocation = indexBuffer->GetGPUVirtualAddress();
        indexView.SizeInBytes = static_cast<UINT>(indexBytes);
        indexView.Format = DXGI_FORMAT_R32_UINT;
        indexCount = static_cast<UINT>(quadIndices.size());
    }
    if (!impl_->FinishUpload(&stagingResources, errorText)) return false;
    impl_->worldVertexBuffer = std::move(vertexBuffer);
    impl_->worldVertexView = vertexView;
    impl_->worldIndexBuffer = std::move(indexBuffer);
    impl_->worldIndexView = indexView;
    impl_->worldVertexCount = static_cast<UINT>(vertexCount);
    impl_->worldIndexCount = indexCount;
    impl_->worldPrimitive = mesh.primitive;
    return true;
}

void PreviewD3D12Renderer::ClearMesh() {
    if (!impl_) return;
    if (!impl_->WaitForGpu(nullptr) && impl_->device &&
        SUCCEEDED(impl_->device->GetDeviceRemovedReason())) return;
    impl_->modelVertexBuffer.Reset();
    impl_->modelVertexView = {};
    for (auto& buffer : impl_->modelIndexBuffers) buffer.Reset();
    impl_->modelIndexViews = {};
    impl_->modelIndexCounts.fill(0);
}

void PreviewD3D12Renderer::ClearWorldMesh() {
    if (!impl_) return;
    if (!impl_->WaitForGpu(nullptr) && impl_->device &&
        SUCCEEDED(impl_->device->GetDeviceRemovedReason())) return;
    impl_->worldVertexBuffer.Reset();
    impl_->worldVertexView = {};
    impl_->worldIndexBuffer.Reset();
    impl_->worldIndexView = {};
    impl_->worldVertexCount = 0;
    impl_->worldIndexCount = 0;
}

bool PreviewD3D12Renderer::Render(const PreviewD3D12Frame& frame,
                                  PreviewD3D12RenderStats* stats,
                                  std::string* errorText) {
    if (!IsReady()) {
        SetError(errorText, "D3D12 renderer is not initialized");
        return false;
    }
    RECT bounds{};
    GetClientRect(impl_->window, &bounds);
    const std::uint32_t clientWidth = static_cast<std::uint32_t>(
        std::max<LONG>(0, bounds.right - bounds.left));
    const std::uint32_t clientHeight = static_cast<std::uint32_t>(
        std::max<LONG>(0, bounds.bottom - bounds.top));
    if (clientWidth == 0 || clientHeight == 0) return true;
    if (!Resize(clientWidth, clientHeight, errorText)) return false;

    const auto start = std::chrono::steady_clock::now();
    if (impl_->frameLatencyWaitable) {
        const DWORD waitResult = WaitForSingleObject(impl_->frameLatencyWaitable, 0);
        if (waitResult == WAIT_FAILED) {
            SetError(errorText, "D3D12 frame-latency wait failed (Win32 error " +
                                std::to_string(GetLastError()) + ")");
            return false;
        }
        if (waitResult == WAIT_TIMEOUT) {
            if (stats) *stats = impl_->lastStats;
            return true;
        }
    }

    const UINT frameIndex = impl_->swapChain->GetCurrentBackBufferIndex();
    if (frameIndex >= kFrameCount) {
        SetError(errorText, "D3D12 swap chain returned an invalid back-buffer index");
        return false;
    }
    if (!impl_->WaitForFence(impl_->frameFenceValues[frameIndex], errorText)) return false;
    HRESULT hr = impl_->frameAllocators[frameIndex]->Reset();
    if (FAILED(hr)) {
        return impl_->DeviceFailure("ID3D12CommandAllocator::Reset(frame)", hr, errorText);
    }
    hr = impl_->commandList->Reset(impl_->frameAllocators[frameIndex].Get(), nullptr);
    if (FAILED(hr)) {
        return impl_->DeviceFailure("ID3D12GraphicsCommandList::Reset(frame)", hr, errorText);
    }
    const auto abortFrame = [this]() {
        impl_->commandList->Close();
        return false;
    };
    impl_->constantBlockCursor = 0;
    PreviewD3D12RenderStats localStats{};
    localStats.frameNumber = ++impl_->frameNumber;

    const XMMATRIX viewProjection = BuildViewProjection(
        frame.camera, static_cast<float>(impl_->width) / static_cast<float>(impl_->height));
    std::vector<ColorVertex> overlayVertices;
    const std::uint32_t gridCount = std::min<std::uint32_t>(frame.overlay.gridLineCount, 4096);
    overlayVertices.reserve(static_cast<std::size_t>(gridCount) * 4 + 24);
    if (frame.overlay.showGrid) {
        const float step = std::max(0.001f, frame.overlay.gridStep);
        const float extent = step * static_cast<float>(gridCount);
        for (std::int32_t index = -static_cast<std::int32_t>(gridCount);
             index <= static_cast<std::int32_t>(gridCount); ++index) {
            const float line = static_cast<float>(index) * step;
            AddLine(&overlayVertices, { line, 0.0f, -extent }, { line, 0.0f, extent },
                    { 46, 49, 54, 255 });
            AddLine(&overlayVertices, { -extent, 0.0f, line }, { extent, 0.0f, line },
                    { 46, 49, 54, 255 });
        }
    }
    if (frame.overlay.showAxes) {
        AddLine(&overlayVertices, { 0.0f, 0.0f, 0.0f }, { 4.0f, 0.0f, 0.0f },
                { 255, 82, 82, 255 });
        AddLine(&overlayVertices, { 0.0f, 0.0f, 0.0f }, { 0.0f, 4.0f, 0.0f },
                { 118, 255, 3, 255 });
        AddLine(&overlayVertices, { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 4.0f },
                { 64, 156, 255, 255 });
    }
    const UINT sceneLineVertices = static_cast<UINT>(overlayVertices.size());
    if (frame.overlay.gizmoLength > 0.0f) {
        const auto& position = frame.overlay.gizmoPosition;
        const float length = frame.overlay.gizmoLength;
        AddLine(&overlayVertices, position,
                { position[0] + length, position[1], position[2] }, { 255, 92, 92, 255 });
        AddLine(&overlayVertices, position,
                { position[0], position[1] + length, position[2] }, { 118, 255, 3, 255 });
        AddLine(&overlayVertices, position,
                { position[0], position[1], position[2] + length }, { 64, 156, 255, 255 });
    }
    const UINT gizmoVertices = static_cast<UINT>(overlayVertices.size()) - sceneLineVertices;
    if (!overlayVertices.empty()) {
        if (!impl_->EnsureOverlayCapacity(frameIndex, overlayVertices.size(), errorText)) {
            return abortFrame();
        }
        std::memcpy(impl_->overlayBuffers[frameIndex].mapped, overlayVertices.data(),
                    overlayVertices.size() * sizeof(ColorVertex));
    }

    D3D12_RESOURCE_BARRIER toRenderTarget{};
    toRenderTarget.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toRenderTarget.Transition.pResource = impl_->renderTargets[frameIndex].Get();
    toRenderTarget.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    toRenderTarget.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    toRenderTarget.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    impl_->commandList->ResourceBarrier(1, &toRenderTarget);

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(impl_->width);
    viewport.Height = static_cast<float>(impl_->height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    impl_->commandList->RSSetViewports(1, &viewport);
    D3D12_RECT scissor{};
    scissor.right = static_cast<LONG>(std::min<std::uint32_t>(
        impl_->width, static_cast<std::uint32_t>(std::numeric_limits<LONG>::max())));
    scissor.bottom = static_cast<LONG>(std::min<std::uint32_t>(
        impl_->height, static_cast<std::uint32_t>(std::numeric_limits<LONG>::max())));
    impl_->commandList->RSSetScissorRects(1, &scissor);
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = impl_->RenderTargetHandle(frameIndex);
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = impl_->dsvHeap->GetCPUDescriptorHandleForHeapStart();
    impl_->commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    impl_->commandList->ClearRenderTargetView(rtv, frame.clearColor.data(), 0, nullptr);
    impl_->commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    impl_->commandList->SetGraphicsRootSignature(impl_->rootSignature.Get());

    if (!impl_->DrawLines(frameIndex, 0, sceneLineVertices, viewProjection, true,
                          &localStats, errorText)) {
        return abortFrame();
    }

    if (frame.showWorld && impl_->worldVertexBuffer) {
        const XMMATRIX world = XMMatrixTranslation(frame.worldOffset[0], frame.worldOffset[1],
                                                    frame.worldOffset[2]);
        if (!impl_->SetConstants(frameIndex, world, viewProjection, errorText)) return abortFrame();
        impl_->commandList->IASetVertexBuffers(0, 1, &impl_->worldVertexView);
        if (impl_->worldPrimitive == PreviewD3D12WorldPrimitive::Points) {
            impl_->commandList->SetPipelineState(impl_->worldPointPipeline.Get());
            impl_->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
            impl_->commandList->DrawInstanced(impl_->worldVertexCount, 1, 0, 0);
            localStats.worldPoints = impl_->worldVertexCount;
        } else if (impl_->worldPrimitive == PreviewD3D12WorldPrimitive::Quads) {
            impl_->commandList->SetPipelineState(impl_->worldTrianglePipeline.Get());
            impl_->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            impl_->commandList->IASetIndexBuffer(&impl_->worldIndexView);
            impl_->commandList->DrawIndexedInstanced(impl_->worldIndexCount, 1, 0, 0, 0);
            localStats.worldTriangles = impl_->worldIndexCount / 3;
        } else {
            impl_->commandList->SetPipelineState(impl_->worldTrianglePipeline.Get());
            impl_->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            impl_->commandList->DrawInstanced(impl_->worldVertexCount, 1, 0, 0);
            localStats.worldTriangles = impl_->worldVertexCount / 3;
        }
        ++localStats.drawCalls;
    }

    if (frame.showModel && impl_->modelVertexBuffer) {
        const std::size_t level = std::min<std::size_t>(
            frame.lodLevel, impl_->modelIndexBuffers.size() - 1);
        if (impl_->modelIndexBuffers[level] && impl_->modelIndexCounts[level] != 0) {
            const XMMATRIX world = BuildModelMatrix(frame.model);
            if (!impl_->SetConstants(frameIndex, world, viewProjection, errorText)) return abortFrame();
            impl_->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            impl_->commandList->IASetVertexBuffers(0, 1, &impl_->modelVertexView);
            impl_->commandList->IASetIndexBuffer(&impl_->modelIndexViews[level]);
            if (frame.drawMode == PreviewD3D12DrawMode::Solid ||
                frame.drawMode == PreviewD3D12DrawMode::SolidWire) {
                impl_->commandList->SetPipelineState(impl_->modelSolidPipeline.Get());
                impl_->commandList->DrawIndexedInstanced(impl_->modelIndexCounts[level], 1, 0, 0, 0);
                ++localStats.drawCalls;
            }
            if (frame.drawMode == PreviewD3D12DrawMode::Wireframe ||
                frame.drawMode == PreviewD3D12DrawMode::SolidWire) {
                impl_->commandList->SetPipelineState(impl_->modelWirePipeline.Get());
                impl_->commandList->DrawIndexedInstanced(impl_->modelIndexCounts[level], 1, 0, 0, 0);
                ++localStats.drawCalls;
            }
            localStats.modelTriangles = impl_->modelIndexCounts[level] / 3;
        }
    }

    if (!impl_->DrawLines(frameIndex, sceneLineVertices, gizmoVertices, viewProjection, false,
                          &localStats, errorText)) {
        return abortFrame();
    }

    D3D12_RESOURCE_BARRIER toPresent = toRenderTarget;
    toPresent.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    toPresent.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    impl_->commandList->ResourceBarrier(1, &toPresent);
    hr = impl_->commandList->Close();
    if (FAILED(hr)) {
        return impl_->DeviceFailure("ID3D12GraphicsCommandList::Close(frame)", hr, errorText);
    }
    ID3D12CommandList* lists[] = { impl_->commandList.Get() };
    impl_->commandQueue->ExecuteCommandLists(1, lists);
    const UINT presentFlags = !impl_->options.verticalSync && impl_->tearingSupported
        ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const HRESULT presentHr = impl_->swapChain->Present(
        impl_->options.verticalSync ? 1u : 0u, presentFlags);
    const UINT64 fenceValue = impl_->nextFenceValue++;
    const HRESULT signalHr = impl_->commandQueue->Signal(impl_->fence.Get(), fenceValue);
    if (FAILED(signalHr)) {
        return impl_->DeviceFailure("ID3D12CommandQueue::Signal(frame)", signalHr, errorText);
    }
    impl_->frameFenceValues[frameIndex] = fenceValue;
    if (FAILED(presentHr)) {
        return impl_->DeviceFailure("IDXGISwapChain::Present", presentHr, errorText);
    }

    localStats.cpuMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    impl_->lastStats = localStats;
    if (stats) *stats = localStats;
    return true;
}

bool PreviewD3D12Renderer::IsReady() const noexcept {
    return impl_ && impl_->deviceOperational && impl_->device && impl_->commandQueue && impl_->swapChain &&
        impl_->commandList && impl_->rootSignature && impl_->modelSolidPipeline &&
        impl_->worldTrianglePipeline && impl_->lineDepthPipeline && impl_->frameConstants &&
        impl_->mappedFrameConstants && impl_->rtvHeap && impl_->dsvHeap &&
        impl_->renderTargets[0] && impl_->depthTarget;
}

bool PreviewD3D12Renderer::IsHardwareDevice() const noexcept {
    return impl_ && impl_->hardwareDevice;
}

const std::string& PreviewD3D12Renderer::AdapterName() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->adapterName : empty;
}

}  // namespace native_mc
