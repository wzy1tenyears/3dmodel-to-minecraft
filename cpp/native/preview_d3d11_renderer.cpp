#define NOMINMAX
#include "preview_d3d11_renderer.h"

#include <Windows.h>
#include <d3d11.h>
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
#include <utility>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

namespace native_mc {
namespace {

using Microsoft::WRL::ComPtr;
using namespace DirectX;

constexpr float kPi = 3.14159265358979323846f;

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
    XMFLOAT4 lightDirectionAndAmbient;
};

constexpr const char* kShaderSource = R"hlsl(
cbuffer FrameConstants : register(b0) {
    row_major float4x4 worldViewProjection;
    row_major float4x4 world;
    float4 lightDirectionAndAmbient;
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
    output.normal = normalize(mul(float4(input.normal, 0.0f), world).xyz);
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
    char buffer[96]{};
    std::snprintf(buffer, sizeof(buffer), "%s failed (HRESULT 0x%08lX)", operation,
                  static_cast<unsigned long>(hr));
    return buffer;
}

void SetError(std::string* output, const std::string& message) {
    if (output) *output = message;
}

bool FitsD3D11Buffer(std::size_t byteCount) {
    return byteCount > 0 && byteCount <= static_cast<std::size_t>(std::numeric_limits<UINT>::max());
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
    const HRESULT hr = D3DCompile(kShaderSource, std::strlen(kShaderSource), "preview_d3d11_renderer.hlsl",
                                  nullptr, nullptr, entryPoint, target, flags, 0, output->GetAddressOf(),
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

bool CreateImmutableBuffer(ID3D11Device* device, UINT bindFlags, const void* data,
                           std::size_t byteCount, ComPtr<ID3D11Buffer>* output,
                           std::string* errorText) {
    if (!device || !data || !output || !FitsD3D11Buffer(byteCount)) {
        SetError(errorText, "D3D11 buffer is empty or exceeds the 4 GiB API limit");
        return false;
    }
    D3D11_BUFFER_DESC description{};
    description.ByteWidth = static_cast<UINT>(byteCount);
    description.Usage = D3D11_USAGE_IMMUTABLE;
    description.BindFlags = bindFlags;
    D3D11_SUBRESOURCE_DATA initial{};
    initial.pSysMem = data;
    const HRESULT hr = device->CreateBuffer(&description, &initial, output->ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        SetError(errorText, HrText("ID3D11Device::CreateBuffer", hr));
        return false;
    }
    return true;
}

XMMATRIX BuildViewProjection(const PreviewD3D11Camera& camera, float aspect) {
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
    return XMMatrixMultiply(XMMatrixMultiply(view, pan),
        XMMatrixPerspectiveFovRH(fov, std::max(0.01f, aspect), nearPlane, farPlane));
}

XMMATRIX BuildModelMatrix(const PreviewD3D11ModelTransform& transform) {
    XMVECTOR sourceQuaternion = XMVectorSet(transform.sourceQuaternion[1], transform.sourceQuaternion[2],
                                            transform.sourceQuaternion[3], transform.sourceQuaternion[0]);
    const float sourceLengthSquared = XMVectorGetX(XMVector4LengthSq(sourceQuaternion));
    sourceQuaternion = sourceLengthSquared > 1.0e-12f
        ? XMQuaternionNormalize(sourceQuaternion) : XMQuaternionIdentity();
    XMVECTOR quaternion = XMVectorSet(transform.quaternion[1], transform.quaternion[2],
                                      transform.quaternion[3], transform.quaternion[0]);
    const float lengthSquared = XMVectorGetX(XMVector4LengthSq(quaternion));
    quaternion = lengthSquared > 1.0e-12f ? XMQuaternionNormalize(quaternion) : XMQuaternionIdentity();
    const XMMATRIX center = XMMatrixTranslation(-transform.center[0], -transform.center[1], -transform.center[2]);
    const XMMATRIX sourceRotation = XMMatrixRotationQuaternion(sourceQuaternion);
    const XMMATRIX scale = XMMatrixScaling(transform.scale[0], transform.scale[1], transform.scale[2]);
    const XMMATRIX rotation = XMMatrixRotationQuaternion(quaternion);
    const XMMATRIX translation = XMMatrixTranslation(transform.position[0], transform.position[1],
                                                       transform.position[2]);
    return XMMatrixMultiply(
        XMMatrixMultiply(XMMatrixMultiply(XMMatrixMultiply(center, sourceRotation), scale), rotation),
        translation);
}

}  // namespace

struct PreviewD3D11Renderer::Impl {
    HWND window = nullptr;
    PreviewD3D11Options options{};
    bool hardwareDevice = false;
    std::string adapterName;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint64_t frameNumber = 0;
    UINT swapChainFlags = 0;
    bool allowTearing = false;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swapChain;
    ComPtr<ID3D11RenderTargetView> renderTarget;
    ComPtr<ID3D11Texture2D> depthTexture;
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11VertexShader> colorVertexShader;
    ComPtr<ID3D11PixelShader> litPixelShader;
    ComPtr<ID3D11PixelShader> colorPixelShader;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11InputLayout> colorInputLayout;
    ComPtr<ID3D11Buffer> frameConstants;
    ComPtr<ID3D11RasterizerState> solidRasterizer;
    ComPtr<ID3D11RasterizerState> wireRasterizer;
    ComPtr<ID3D11DepthStencilState> depthState;
    ComPtr<ID3D11DepthStencilState> overlayDepthState;
    ComPtr<ID3D11BlendState> blendState;
    ComPtr<ID3D11Buffer> overlayVertexBuffer;
    std::size_t overlayVertexCapacity = 0;

    ComPtr<ID3D11Buffer> modelVertexBuffer;
    std::array<ComPtr<ID3D11Buffer>, 4> modelIndexBuffers{};
    std::array<UINT, 4> modelIndexCounts{};

    ComPtr<ID3D11Buffer> worldVertexBuffer;
    ComPtr<ID3D11Buffer> worldIndexBuffer;
    UINT worldVertexCount = 0;
    UINT worldIndexCount = 0;
    PreviewD3D11WorldPrimitive worldPrimitive = PreviewD3D11WorldPrimitive::Quads;

    bool CreateDeviceAndSwapChain(std::string* errorText) {
        RECT bounds{};
        GetClientRect(window, &bounds);
        width = static_cast<std::uint32_t>(std::max<LONG>(1, bounds.right - bounds.left));
        height = static_cast<std::uint32_t>(std::max<LONG>(1, bounds.bottom - bounds.top));

        ComPtr<IDXGIFactory2> factory;
        HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(factory.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) {
            SetError(errorText, HrText("CreateDXGIFactory1", hr));
            return false;
        }

        constexpr std::array<D3D_FEATURE_LEVEL, 4> featureLevels = {
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        auto createDevice = [&](IDXGIAdapter* adapter, D3D_DRIVER_TYPE driverType) {
            device.Reset();
            context.Reset();
            HRESULT createHr = D3D11CreateDevice(
                adapter, driverType, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                featureLevels.data(), static_cast<UINT>(featureLevels.size()), D3D11_SDK_VERSION,
                device.ReleaseAndGetAddressOf(), nullptr, context.ReleaseAndGetAddressOf());
            if (createHr == E_INVALIDARG) {
                device.Reset();
                context.Reset();
                createHr = D3D11CreateDevice(
                    adapter, driverType, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                    featureLevels.data() + 1, static_cast<UINT>(featureLevels.size() - 1),
                    D3D11_SDK_VERSION, device.ReleaseAndGetAddressOf(), nullptr,
                    context.ReleaseAndGetAddressOf());
            }
            return createHr;
        };

        hr = DXGI_ERROR_NOT_FOUND;
        ComPtr<IDXGIFactory6> factory6;
        if (SUCCEEDED(factory.As(&factory6))) {
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> candidate;
                const HRESULT enumHr = factory6->EnumAdapterByGpuPreference(
                    index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                    IID_PPV_ARGS(candidate.ReleaseAndGetAddressOf()));
                if (enumHr == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(enumHr)) break;
                DXGI_ADAPTER_DESC1 description{};
                if (FAILED(candidate->GetDesc1(&description)) ||
                    (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                    continue;
                }
                hr = createDevice(candidate.Get(), D3D_DRIVER_TYPE_UNKNOWN);
                if (SUCCEEDED(hr)) break;
            }
        }

        // Older DXGI versions do not expose GPU preference ordering. Preserve
        // hardware selection there by probing adapters in factory order.
        if (!device) {
            for (UINT index = 0;; ++index) {
                ComPtr<IDXGIAdapter1> candidate;
                const HRESULT enumHr = factory->EnumAdapters1(index, candidate.ReleaseAndGetAddressOf());
                if (enumHr == DXGI_ERROR_NOT_FOUND) break;
                if (FAILED(enumHr)) break;
                DXGI_ADAPTER_DESC1 description{};
                if (FAILED(candidate->GetDesc1(&description)) ||
                    (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                    continue;
                }
                hr = createDevice(candidate.Get(), D3D_DRIVER_TYPE_UNKNOWN);
                if (SUCCEEDED(hr)) break;
            }
        }

        hardwareDevice = device.Get() != nullptr;
        if (!device && options.allowWarpFallback) {
            hr = createDevice(nullptr, D3D_DRIVER_TYPE_WARP);
            hardwareDevice = false;
        }
        if (!device || FAILED(hr)) {
            SetError(errorText, HrText("D3D11CreateDevice", hr));
            return false;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapterBase;
        ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 adapterDescription{};
        if (SUCCEEDED(device.As(&dxgiDevice)) &&
            SUCCEEDED(dxgiDevice->GetAdapter(adapterBase.GetAddressOf())) &&
            SUCCEEDED(adapterBase.As(&adapter)) &&
            SUCCEEDED(adapter->GetDesc1(&adapterDescription))) {
            adapterName = WideToUtf8(adapterDescription.Description);
            hardwareDevice = (adapterDescription.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0;
        }
        if (adapterName.empty()) adapterName = hardwareDevice ? "D3D11 hardware" : "D3D11 WARP";

        ComPtr<IDXGIDevice1> dxgiDevice1;
        if (SUCCEEDED(device.As(&dxgiDevice1))) {
            hr = dxgiDevice1->SetMaximumFrameLatency(std::clamp(options.maximumFrameLatency, 1u, 16u));
            if (FAILED(hr)) {
                SetError(errorText, HrText("IDXGIDevice1::SetMaximumFrameLatency", hr));
                return false;
            }
        }

        ComPtr<IDXGIFactory5> factory5;
        BOOL tearingSupported = FALSE;
        if (SUCCEEDED(factory.As(&factory5)) &&
            SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                     &tearingSupported,
                                                     sizeof(tearingSupported))) &&
            tearingSupported == TRUE) {
            allowTearing = true;
            swapChainFlags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        }

        DXGI_SWAP_CHAIN_DESC1 swapDescription{};
        swapDescription.Width = width;
        swapDescription.Height = height;
        swapDescription.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swapDescription.SampleDesc.Count = 1;
        swapDescription.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swapDescription.BufferCount = 2;
        swapDescription.Scaling = DXGI_SCALING_STRETCH;
        swapDescription.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        swapDescription.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        swapDescription.Flags = swapChainFlags;
        hr = factory->CreateSwapChainForHwnd(device.Get(), window, &swapDescription, nullptr, nullptr,
                                             swapChain.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("IDXGIFactory2::CreateSwapChainForHwnd", hr));
            return false;
        }

        HWND rootWindow = GetAncestor(window, GA_ROOT);
        if (!rootWindow) rootWindow = window;
        const HRESULT associationHr = factory->MakeWindowAssociation(rootWindow, DXGI_MWA_NO_ALT_ENTER);
        if (FAILED(associationHr) && associationHr != DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
            SetError(errorText, HrText("IDXGIFactory::MakeWindowAssociation", associationHr));
            return false;
        }
        // DXGI_ERROR_NOT_CURRENTLY_AVAILABLE only means DXGI cannot own the
        // Alt+Enter association (for example in some remote sessions). This
        // renderer never enters exclusive fullscreen, so continuing is safe.
        return true;
    }

    bool CreateTargets(std::string* errorText) {
        ComPtr<ID3D11Texture2D> backBuffer;
        HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
        if (FAILED(hr)) {
            SetError(errorText, HrText("IDXGISwapChain::GetBuffer", hr));
            return false;
        }
        hr = device->CreateRenderTargetView(backBuffer.Get(), nullptr, renderTarget.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreateRenderTargetView", hr));
            return false;
        }

        D3D11_TEXTURE2D_DESC depthDescription{};
        depthDescription.Width = width;
        depthDescription.Height = height;
        depthDescription.MipLevels = 1;
        depthDescription.ArraySize = 1;
        depthDescription.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        depthDescription.SampleDesc.Count = 1;
        depthDescription.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        hr = device->CreateTexture2D(&depthDescription, nullptr, depthTexture.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreateTexture2D(depth)", hr));
            return false;
        }
        hr = device->CreateDepthStencilView(depthTexture.Get(), nullptr, depthView.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreateDepthStencilView", hr));
            return false;
        }
        return true;
    }

    bool CreatePipeline(std::string* errorText) {
        ComPtr<ID3DBlob> vertexBytecode;
        ComPtr<ID3DBlob> colorVertexBytecode;
        ComPtr<ID3DBlob> litPixelBytecode;
        ComPtr<ID3DBlob> colorPixelBytecode;
        if (!CompileShader("MainVS", "vs_4_0", &vertexBytecode, errorText) ||
            !CompileShader("ColorVS", "vs_4_0", &colorVertexBytecode, errorText) ||
            !CompileShader("LitPS", "ps_4_0", &litPixelBytecode, errorText) ||
            !CompileShader("ColorPS", "ps_4_0", &colorPixelBytecode, errorText)) return false;

        HRESULT hr = device->CreateVertexShader(vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
                                                 nullptr, vertexShader.GetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreateVertexShader", hr));
            return false;
        }
        hr = device->CreateVertexShader(colorVertexBytecode->GetBufferPointer(),
                                        colorVertexBytecode->GetBufferSize(), nullptr,
                                        colorVertexShader.GetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreateVertexShader(color)", hr));
            return false;
        }
        hr = device->CreatePixelShader(litPixelBytecode->GetBufferPointer(), litPixelBytecode->GetBufferSize(),
                                       nullptr, litPixelShader.GetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreatePixelShader(lit)", hr));
            return false;
        }
        hr = device->CreatePixelShader(colorPixelBytecode->GetBufferPointer(), colorPixelBytecode->GetBufferSize(),
                                       nullptr, colorPixelShader.GetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreatePixelShader(color)", hr));
            return false;
        }

        constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 3> layout = {{
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        }};
        hr = device->CreateInputLayout(layout.data(), static_cast<UINT>(layout.size()),
                                       vertexBytecode->GetBufferPointer(), vertexBytecode->GetBufferSize(),
                                       inputLayout.GetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreateInputLayout", hr));
            return false;
        }
        constexpr std::array<D3D11_INPUT_ELEMENT_DESC, 2> colorLayout = {{
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 }
        }};
        hr = device->CreateInputLayout(colorLayout.data(), static_cast<UINT>(colorLayout.size()),
                                       colorVertexBytecode->GetBufferPointer(), colorVertexBytecode->GetBufferSize(),
                                       colorInputLayout.GetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreateInputLayout(color)", hr));
            return false;
        }

        D3D11_BUFFER_DESC constantDescription{};
        constantDescription.ByteWidth = sizeof(FrameConstants);
        constantDescription.Usage = D3D11_USAGE_DYNAMIC;
        constantDescription.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constantDescription.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device->CreateBuffer(&constantDescription, nullptr, frameConstants.GetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreateBuffer(constants)", hr));
            return false;
        }

        D3D11_RASTERIZER_DESC rasterDescription{};
        rasterDescription.FillMode = D3D11_FILL_SOLID;
        rasterDescription.CullMode = D3D11_CULL_NONE;
        rasterDescription.DepthClipEnable = TRUE;
        hr = device->CreateRasterizerState(&rasterDescription, solidRasterizer.GetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreateRasterizerState(solid)", hr));
            return false;
        }
        rasterDescription.FillMode = D3D11_FILL_WIREFRAME;
        rasterDescription.DepthBias = -1;
        rasterDescription.SlopeScaledDepthBias = -0.5f;
        hr = device->CreateRasterizerState(&rasterDescription, wireRasterizer.GetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreateRasterizerState(wire)", hr));
            return false;
        }

        D3D11_DEPTH_STENCIL_DESC depthDescription{};
        depthDescription.DepthEnable = TRUE;
        depthDescription.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depthDescription.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
        hr = device->CreateDepthStencilState(&depthDescription, depthState.GetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreateDepthStencilState", hr));
            return false;
        }
        depthDescription.DepthEnable = FALSE;
        depthDescription.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        hr = device->CreateDepthStencilState(&depthDescription, overlayDepthState.GetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreateDepthStencilState(overlay)", hr));
            return false;
        }

        D3D11_BLEND_DESC blendDescription{};
        blendDescription.RenderTarget[0].BlendEnable = TRUE;
        blendDescription.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        blendDescription.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blendDescription.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blendDescription.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        blendDescription.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blendDescription.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blendDescription.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = device->CreateBlendState(&blendDescription, blendState.GetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreateBlendState", hr));
            return false;
        }
        return true;
    }

    bool UpdateConstants(const XMMATRIX& world, const XMMATRIX& viewProjection,
                         std::string* errorText) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT hr = context->Map(frameConstants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11DeviceContext::Map(constants)", hr));
            return false;
        }
        FrameConstants constants{};
        XMStoreFloat4x4(&constants.world, world);
        XMStoreFloat4x4(&constants.worldViewProjection, XMMatrixMultiply(world, viewProjection));
        constants.lightDirectionAndAmbient = XMFLOAT4(0.4f, 1.0f, 0.6f, 0.22f);
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context->Unmap(frameConstants.Get(), 0);
        ID3D11Buffer* buffers[] = { frameConstants.Get() };
        context->VSSetConstantBuffers(0, 1, buffers);
        context->PSSetConstantBuffers(0, 1, buffers);
        return true;
    }

    bool EnsureOverlayCapacity(std::size_t vertexCount, std::string* errorText) {
        if (vertexCount <= overlayVertexCapacity && overlayVertexBuffer) return true;
        const std::size_t newCapacity = std::max<std::size_t>(256, vertexCount + vertexCount / 2);
        const std::size_t byteCount = newCapacity * sizeof(ColorVertex);
        if (!FitsD3D11Buffer(byteCount)) {
            SetError(errorText, "D3D11 overlay buffer exceeds the API size limit");
            return false;
        }
        D3D11_BUFFER_DESC description{};
        description.ByteWidth = static_cast<UINT>(byteCount);
        description.Usage = D3D11_USAGE_DYNAMIC;
        description.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        description.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        const HRESULT hr = device->CreateBuffer(&description, nullptr, overlayVertexBuffer.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11Device::CreateBuffer(overlay)", hr));
            return false;
        }
        overlayVertexCapacity = newCapacity;
        return true;
    }

    bool DrawLines(const std::vector<ColorVertex>& vertices, const XMMATRIX& viewProjection,
                   bool depthEnabled, PreviewD3D11RenderStats* stats, std::string* errorText) {
        if (vertices.empty()) return true;
        if (!EnsureOverlayCapacity(vertices.size(), errorText)) return false;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT hr = context->Map(overlayVertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr)) {
            SetError(errorText, HrText("ID3D11DeviceContext::Map(overlay)", hr));
            return false;
        }
        std::memcpy(mapped.pData, vertices.data(), vertices.size() * sizeof(ColorVertex));
        context->Unmap(overlayVertexBuffer.Get(), 0);
        if (!UpdateConstants(XMMatrixIdentity(), viewProjection, errorText)) return false;
        const UINT stride = sizeof(ColorVertex);
        const UINT offset = 0;
        ID3D11Buffer* vertexBuffer = overlayVertexBuffer.Get();
        context->IASetInputLayout(colorInputLayout.Get());
        context->IASetVertexBuffers(0, 1, &vertexBuffer, &stride, &offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        context->OMSetDepthStencilState(depthEnabled ? depthState.Get() : overlayDepthState.Get(), 0);
        context->RSSetState(solidRasterizer.Get());
        context->VSSetShader(colorVertexShader.Get(), nullptr, 0);
        context->PSSetShader(colorPixelShader.Get(), nullptr, 0);
        context->Draw(static_cast<UINT>(vertices.size()), 0);
        if (stats) ++stats->drawCalls;
        return true;
    }
};

PreviewD3D11Renderer::PreviewD3D11Renderer() : impl_(std::make_unique<Impl>()) {}
PreviewD3D11Renderer::~PreviewD3D11Renderer() { Shutdown(); }
PreviewD3D11Renderer::PreviewD3D11Renderer(PreviewD3D11Renderer&&) noexcept = default;
PreviewD3D11Renderer& PreviewD3D11Renderer::operator=(PreviewD3D11Renderer&&) noexcept = default;

bool PreviewD3D11Renderer::Initialize(PreviewD3D11NativeWindow window,
                                      const PreviewD3D11Options& options,
                                      std::string* errorText) {
    Shutdown();
    impl_ = std::make_unique<Impl>();
    impl_->window = static_cast<HWND>(window);
    impl_->options = options;
    if (!impl_->window || !IsWindow(impl_->window)) {
        SetError(errorText, "PreviewD3D11Renderer requires a valid HWND");
        return false;
    }
    if (!impl_->CreateDeviceAndSwapChain(errorText) || !impl_->CreateTargets(errorText) ||
        !impl_->CreatePipeline(errorText)) {
        Shutdown();
        return false;
    }
    return true;
}

bool PreviewD3D11Renderer::Resize(std::uint32_t width, std::uint32_t height,
                                  std::string* errorText) {
    if (!impl_ || !impl_->swapChain || !impl_->context) {
        SetError(errorText, "D3D11 renderer is not initialized");
        return false;
    }
    if (width == 0 || height == 0) return true;
    if (width == impl_->width && height == impl_->height && impl_->renderTarget && impl_->depthView) return true;
    impl_->context->OMSetRenderTargets(0, nullptr, nullptr);
    impl_->renderTarget.Reset();
    impl_->depthView.Reset();
    impl_->depthTexture.Reset();
    const HRESULT hr = impl_->swapChain->ResizeBuffers(
        0, width, height, DXGI_FORMAT_UNKNOWN, impl_->swapChainFlags);
    if (FAILED(hr)) {
        SetError(errorText, HrText("IDXGISwapChain::ResizeBuffers", hr));
        return false;
    }
    impl_->width = width;
    impl_->height = height;
    return impl_->CreateTargets(errorText);
}

void PreviewD3D11Renderer::Shutdown() {
    if (!impl_) return;
    if (impl_->context) {
        impl_->context->ClearState();
        impl_->context->Flush();
    }
    impl_.reset();
}

bool PreviewD3D11Renderer::UploadMesh(const PreviewMesh& mesh, std::string* errorText) {
    if (!IsReady()) {
        SetError(errorText, "D3D11 renderer is not initialized");
        return false;
    }
    const std::size_t vertexCount = mesh.positions.size() / 3;
    if (vertexCount == 0 || mesh.positions.size() != vertexCount * 3 || mesh.indices.empty()) {
        SetError(errorText, "Preview mesh has no valid vertices or indices");
        return false;
    }
    if (vertexCount > std::numeric_limits<std::uint32_t>::max()) {
        SetError(errorText, "Preview mesh has more vertices than D3D11 32-bit indices can address");
        return false;
    }

    std::vector<RenderVertex> vertices(vertexCount);
    const bool hasNormals = mesh.normals.size() == vertexCount * 3;
    const bool hasColors = mesh.colors.size() == vertexCount * 4;
    for (std::size_t i = 0; i < vertexCount; ++i) {
        std::copy_n(mesh.positions.data() + i * 3, 3, vertices[i].position);
        if (hasNormals) {
            std::copy_n(mesh.normals.data() + i * 3, 3, vertices[i].normal);
        } else {
            vertices[i].normal[0] = 0.0f;
            vertices[i].normal[1] = 1.0f;
            vertices[i].normal[2] = 0.0f;
        }
        if (hasColors) {
            std::copy_n(mesh.colors.data() + i * 4, 4, vertices[i].color);
        } else {
            vertices[i].color[0] = 205;
            vertices[i].color[1] = 210;
            vertices[i].color[2] = 215;
            vertices[i].color[3] = 255;
        }
    }

    ComPtr<ID3D11Buffer> vertexBuffer;
    if (!CreateImmutableBuffer(impl_->device.Get(), D3D11_BIND_VERTEX_BUFFER, vertices.data(),
                               vertices.size() * sizeof(RenderVertex), &vertexBuffer, errorText)) return false;
    std::array<ComPtr<ID3D11Buffer>, 4> indexBuffers{};
    std::array<UINT, 4> indexCounts{};
    for (std::size_t level = 0; level < indexBuffers.size(); ++level) {
        const auto& indices = (level == 0 || mesh.lodIndices[level].empty())
            ? mesh.indices : mesh.lodIndices[level];
        if (indices.empty() || indices.size() > std::numeric_limits<UINT>::max()) {
            SetError(errorText, "Preview mesh LOD index buffer is empty or too large");
            return false;
        }
        if (std::any_of(indices.begin(), indices.end(),
                        [vertexCount](std::uint32_t index) { return index >= vertexCount; })) {
            SetError(errorText, "Preview mesh LOD contains an out-of-range vertex index");
            return false;
        }
        if (!CreateImmutableBuffer(impl_->device.Get(), D3D11_BIND_INDEX_BUFFER, indices.data(),
                                   indices.size() * sizeof(std::uint32_t), &indexBuffers[level], errorText)) return false;
        indexCounts[level] = static_cast<UINT>(indices.size());
    }
    impl_->modelVertexBuffer = std::move(vertexBuffer);
    impl_->modelIndexBuffers = std::move(indexBuffers);
    impl_->modelIndexCounts = indexCounts;
    return true;
}

bool PreviewD3D11Renderer::UploadWorldMesh(const PreviewD3D11WorldMeshView& mesh,
                                           std::string* errorText) {
    if (!IsReady()) {
        SetError(errorText, "D3D11 renderer is not initialized");
        return false;
    }
    if (!mesh.positions || !mesh.colors || mesh.positionFloatCount == 0 ||
        mesh.positionFloatCount % 3 != 0 || (mesh.colorChannels != 3 && mesh.colorChannels != 4)) {
        SetError(errorText, "World preview mesh has invalid position or color data");
        return false;
    }
    const std::size_t vertexCount = mesh.positionFloatCount / 3;
    if (mesh.colorByteCount < vertexCount * mesh.colorChannels ||
        vertexCount > std::numeric_limits<UINT>::max()) {
        SetError(errorText, "World preview mesh color data is short or the vertex count is too large");
        return false;
    }
    if (mesh.primitive == PreviewD3D11WorldPrimitive::Quads && vertexCount % 4 != 0) {
        SetError(errorText, "World preview quad vertex count must be divisible by four");
        return false;
    }
    if (mesh.primitive == PreviewD3D11WorldPrimitive::Triangles && vertexCount % 3 != 0) {
        SetError(errorText, "World preview triangle vertex count must be divisible by three");
        return false;
    }

    std::vector<ColorVertex> vertices(vertexCount);
    for (std::size_t i = 0; i < vertexCount; ++i) {
        std::copy_n(mesh.positions + i * 3, 3, vertices[i].position);
        const std::size_t colorOffset = i * mesh.colorChannels;
        vertices[i].color[0] = mesh.colors[colorOffset];
        vertices[i].color[1] = mesh.colors[colorOffset + 1];
        vertices[i].color[2] = mesh.colors[colorOffset + 2];
        vertices[i].color[3] = mesh.colorChannels == 4 ? mesh.colors[colorOffset + 3] : 255;
    }
    ComPtr<ID3D11Buffer> vertexBuffer;
    if (!CreateImmutableBuffer(impl_->device.Get(), D3D11_BIND_VERTEX_BUFFER, vertices.data(),
                               vertices.size() * sizeof(ColorVertex), &vertexBuffer, errorText)) return false;

    ComPtr<ID3D11Buffer> indexBuffer;
    UINT indexCount = 0;
    if (mesh.primitive == PreviewD3D11WorldPrimitive::Quads) {
        const std::size_t quadCount = vertexCount / 4;
        if (quadCount > std::numeric_limits<UINT>::max() / 6u) {
            SetError(errorText, "World preview quad index count is too large");
            return false;
        }
        std::vector<std::uint32_t> indices;
        indices.reserve(quadCount * 6);
        for (std::size_t quad = 0; quad < quadCount; ++quad) {
            const std::uint32_t base = static_cast<std::uint32_t>(quad * 4);
            indices.insert(indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
        }
        if (!CreateImmutableBuffer(impl_->device.Get(), D3D11_BIND_INDEX_BUFFER, indices.data(),
                                   indices.size() * sizeof(std::uint32_t), &indexBuffer, errorText)) return false;
        indexCount = static_cast<UINT>(indices.size());
    }
    impl_->worldVertexBuffer = std::move(vertexBuffer);
    impl_->worldIndexBuffer = std::move(indexBuffer);
    impl_->worldVertexCount = static_cast<UINT>(vertexCount);
    impl_->worldIndexCount = indexCount;
    impl_->worldPrimitive = mesh.primitive;
    return true;
}

void PreviewD3D11Renderer::ClearMesh() {
    if (!impl_) return;
    impl_->modelVertexBuffer.Reset();
    for (auto& buffer : impl_->modelIndexBuffers) buffer.Reset();
    impl_->modelIndexCounts.fill(0);
}

void PreviewD3D11Renderer::ClearWorldMesh() {
    if (!impl_) return;
    impl_->worldVertexBuffer.Reset();
    impl_->worldIndexBuffer.Reset();
    impl_->worldVertexCount = 0;
    impl_->worldIndexCount = 0;
}

bool PreviewD3D11Renderer::Render(const PreviewD3D11Frame& frame,
                                  PreviewD3D11RenderStats* stats,
                                  std::string* errorText) {
    if (!IsReady()) {
        SetError(errorText, "D3D11 renderer is not initialized");
        return false;
    }
    RECT bounds{};
    GetClientRect(impl_->window, &bounds);
    const std::uint32_t width = static_cast<std::uint32_t>(std::max<LONG>(0, bounds.right - bounds.left));
    const std::uint32_t height = static_cast<std::uint32_t>(std::max<LONG>(0, bounds.bottom - bounds.top));
    if (width == 0 || height == 0) return true;
    if (!Resize(width, height, errorText)) return false;

    const auto start = std::chrono::steady_clock::now();
    PreviewD3D11RenderStats localStats{};
    localStats.frameNumber = ++impl_->frameNumber;
    ID3D11RenderTargetView* renderTargets[] = { impl_->renderTarget.Get() };
    impl_->context->OMSetRenderTargets(1, renderTargets, impl_->depthView.Get());
    impl_->context->ClearRenderTargetView(impl_->renderTarget.Get(), frame.clearColor.data());
    impl_->context->ClearDepthStencilView(impl_->depthView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(impl_->width);
    viewport.Height = static_cast<float>(impl_->height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    impl_->context->RSSetViewports(1, &viewport);
    const float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    impl_->context->OMSetBlendState(impl_->blendState.Get(), blendFactor, 0xFFFFFFFFu);
    impl_->context->OMSetDepthStencilState(impl_->depthState.Get(), 0);

    const XMMATRIX viewProjection = BuildViewProjection(
        frame.camera, static_cast<float>(impl_->width) / static_cast<float>(impl_->height));

    std::vector<ColorVertex> sceneLines;
    auto addLine = [&sceneLines](const std::array<float, 3>& a, const std::array<float, 3>& b,
                                 const std::array<std::uint8_t, 4>& color) {
        ColorVertex first{};
        ColorVertex second{};
        std::copy(a.begin(), a.end(), first.position);
        std::copy(b.begin(), b.end(), second.position);
        std::copy(color.begin(), color.end(), first.color);
        std::copy(color.begin(), color.end(), second.color);
        sceneLines.push_back(first);
        sceneLines.push_back(second);
    };
    if (frame.overlay.showGrid) {
        const float step = std::max(0.001f, frame.overlay.gridStep);
        const std::uint32_t count = std::min<std::uint32_t>(frame.overlay.gridLineCount, 4096);
        const float extent = step * static_cast<float>(count);
        for (std::int32_t i = -static_cast<std::int32_t>(count);
             i <= static_cast<std::int32_t>(count); ++i) {
            const float line = static_cast<float>(i) * step;
            addLine({ line, 0.0f, -extent }, { line, 0.0f, extent }, { 46, 49, 54, 255 });
            addLine({ -extent, 0.0f, line }, { extent, 0.0f, line }, { 46, 49, 54, 255 });
        }
    }
    if (frame.overlay.showAxes) {
        addLine({ 0.0f, 0.0f, 0.0f }, { 4.0f, 0.0f, 0.0f }, { 255, 82, 82, 255 });
        addLine({ 0.0f, 0.0f, 0.0f }, { 0.0f, 4.0f, 0.0f }, { 118, 255, 3, 255 });
        addLine({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 4.0f }, { 64, 156, 255, 255 });
    }
    if (!impl_->DrawLines(sceneLines, viewProjection, true, &localStats, errorText)) return false;

    const UINT offset = 0;
    if (frame.showWorld && impl_->worldVertexBuffer) {
        const XMMATRIX world = XMMatrixTranslation(frame.worldOffset[0], frame.worldOffset[1], frame.worldOffset[2]);
        if (!impl_->UpdateConstants(world, viewProjection, errorText)) return false;
        const UINT worldStride = sizeof(ColorVertex);
        ID3D11Buffer* vertexBuffer = impl_->worldVertexBuffer.Get();
        impl_->context->IASetInputLayout(impl_->colorInputLayout.Get());
        impl_->context->IASetVertexBuffers(0, 1, &vertexBuffer, &worldStride, &offset);
        impl_->context->VSSetShader(impl_->colorVertexShader.Get(), nullptr, 0);
        impl_->context->PSSetShader(impl_->colorPixelShader.Get(), nullptr, 0);
        impl_->context->RSSetState(impl_->solidRasterizer.Get());
        if (impl_->worldPrimitive == PreviewD3D11WorldPrimitive::Quads) {
            impl_->context->IASetIndexBuffer(impl_->worldIndexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
            impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            impl_->context->DrawIndexed(impl_->worldIndexCount, 0, 0);
            localStats.worldTriangles = impl_->worldIndexCount / 3;
        } else {
            impl_->context->IASetPrimitiveTopology(
                impl_->worldPrimitive == PreviewD3D11WorldPrimitive::Points
                    ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            impl_->context->Draw(impl_->worldVertexCount, 0);
            if (impl_->worldPrimitive == PreviewD3D11WorldPrimitive::Points) {
                localStats.worldPoints = impl_->worldVertexCount;
            } else {
                localStats.worldTriangles = impl_->worldVertexCount / 3;
            }
        }
        ++localStats.drawCalls;
    }

    if (frame.showModel && impl_->modelVertexBuffer) {
        const std::size_t level = std::min<std::size_t>(frame.lodLevel, impl_->modelIndexBuffers.size() - 1);
        if (impl_->modelIndexBuffers[level] && impl_->modelIndexCounts[level]) {
            const XMMATRIX world = BuildModelMatrix(frame.model);
            if (!impl_->UpdateConstants(world, viewProjection, errorText)) return false;
            const UINT modelStride = sizeof(RenderVertex);
            ID3D11Buffer* vertexBuffer = impl_->modelVertexBuffer.Get();
            impl_->context->IASetInputLayout(impl_->inputLayout.Get());
            impl_->context->IASetVertexBuffers(0, 1, &vertexBuffer, &modelStride, &offset);
            impl_->context->IASetIndexBuffer(impl_->modelIndexBuffers[level].Get(), DXGI_FORMAT_R32_UINT, 0);
            impl_->context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            impl_->context->VSSetShader(impl_->vertexShader.Get(), nullptr, 0);
            impl_->context->PSSetShader(impl_->litPixelShader.Get(), nullptr, 0);
            impl_->context->OMSetDepthStencilState(impl_->depthState.Get(), 0);
            if (frame.drawMode == PreviewD3D11DrawMode::Solid ||
                frame.drawMode == PreviewD3D11DrawMode::SolidWire) {
                impl_->context->RSSetState(impl_->solidRasterizer.Get());
                impl_->context->DrawIndexed(impl_->modelIndexCounts[level], 0, 0);
                ++localStats.drawCalls;
            }
            if (frame.drawMode == PreviewD3D11DrawMode::Wireframe ||
                frame.drawMode == PreviewD3D11DrawMode::SolidWire) {
                impl_->context->PSSetShader(impl_->colorPixelShader.Get(), nullptr, 0);
                impl_->context->RSSetState(impl_->wireRasterizer.Get());
                impl_->context->DrawIndexed(impl_->modelIndexCounts[level], 0, 0);
                ++localStats.drawCalls;
            }
            localStats.modelTriangles = impl_->modelIndexCounts[level] / 3;
        }
    }

    if (frame.overlay.gizmoLength > 0.0f) {
        const auto& p = frame.overlay.gizmoPosition;
        const float length = frame.overlay.gizmoLength;
        std::vector<ColorVertex> gizmoLines;
        auto addGizmo = [&gizmoLines](const std::array<float, 3>& a, const std::array<float, 3>& b,
                                      const std::array<std::uint8_t, 4>& color) {
            ColorVertex first{};
            ColorVertex second{};
            std::copy(a.begin(), a.end(), first.position);
            std::copy(b.begin(), b.end(), second.position);
            std::copy(color.begin(), color.end(), first.color);
            std::copy(color.begin(), color.end(), second.color);
            gizmoLines.push_back(first);
            gizmoLines.push_back(second);
        };
        addGizmo(p, { p[0] + length, p[1], p[2] }, { 255, 92, 92, 255 });
        addGizmo(p, { p[0], p[1] + length, p[2] }, { 118, 255, 3, 255 });
        addGizmo(p, { p[0], p[1], p[2] + length }, { 64, 156, 255, 255 });
        if (!impl_->DrawLines(gizmoLines, viewProjection, false, &localStats, errorText)) return false;
    }

    const UINT syncInterval = impl_->options.verticalSync ? 1u : 0u;
    const UINT presentFlags = syncInterval == 0 && impl_->allowTearing
        ? DXGI_PRESENT_ALLOW_TEARING : 0u;
    const HRESULT presentHr = impl_->swapChain->Present(syncInterval, presentFlags);
    if (FAILED(presentHr)) {
        if (presentHr == DXGI_ERROR_DEVICE_REMOVED || presentHr == DXGI_ERROR_DEVICE_RESET) {
            const HRESULT reason = impl_->device->GetDeviceRemovedReason();
            SetError(errorText, HrText("D3D11 device removed", reason));
        } else {
            SetError(errorText, HrText("IDXGISwapChain::Present", presentHr));
        }
        return false;
    }
    localStats.cpuMilliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    if (stats) *stats = localStats;
    return true;
}

bool PreviewD3D11Renderer::IsReady() const noexcept {
    return impl_ && impl_->device && impl_->context && impl_->swapChain &&
        impl_->renderTarget && impl_->depthView;
}

bool PreviewD3D11Renderer::IsHardwareDevice() const noexcept {
    return impl_ && impl_->hardwareDevice;
}

const std::string& PreviewD3D11Renderer::AdapterName() const noexcept {
    static const std::string empty;
    return impl_ ? impl_->adapterName : empty;
}

}  // namespace native_mc
