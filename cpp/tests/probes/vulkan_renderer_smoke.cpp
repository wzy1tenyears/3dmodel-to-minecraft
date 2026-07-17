#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include "native/preview_vulkan_renderer.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

namespace {

LRESULT CALLBACK SmokeWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(window, message, wParam, lParam);
}

void PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

}  // namespace

int main() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = SmokeWindowProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = L"NativeMcVulkanSmokeWindow";
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::cerr << "RegisterClassW failed\n";
        return 1;
    }
    HWND window = CreateWindowExW(
        0, windowClass.lpszClassName, L"hidden Vulkan preview smoke",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480,
        nullptr, nullptr, instance, nullptr);
    if (!window) {
        std::cerr << "CreateWindowExW failed\n";
        return 1;
    }

    native_mc::PreviewVulkanRenderer renderer;
    native_mc::PreviewVulkanOptions options;
    options.verticalSync = false;
    options.framesInFlight = 2;
    std::string error;
    if (!renderer.Initialize(window, options, &error)) {
        std::cerr << "Initialize failed: " << error << "\n";
        DestroyWindow(window);
        return 2;
    }

    native_mc::PreviewMesh mesh;
    mesh.positions = {
        -0.8f, -0.6f, 0.0f,
         0.8f, -0.6f, 0.0f,
         0.0f,  0.8f, 0.0f,
    };
    mesh.normals = {
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
    };
    mesh.colors = {
        255, 80, 80, 255,
        80, 255, 80, 255,
        80, 120, 255, 255,
    };
    mesh.indices = { 0, 1, 2 };
    if (!renderer.UploadMesh(mesh, &error)) {
        std::cerr << "UploadMesh failed: " << error << "\n";
        DestroyWindow(window);
        return 3;
    }

    const std::array<float, 12> quadPositions = {
        -1.0f, -0.8f, -0.5f,
         1.0f, -0.8f, -0.5f,
         1.0f, -0.8f,  0.5f,
        -1.0f, -0.8f,  0.5f,
    };
    const std::array<std::uint8_t, 12> quadColors = {
        70, 70, 70, 80, 80, 80, 90, 90, 90, 70, 70, 70,
    };
    native_mc::PreviewVulkanWorldMeshView world;
    world.positions = quadPositions.data();
    world.positionFloatCount = quadPositions.size();
    world.colors = quadColors.data();
    world.colorByteCount = quadColors.size();
    world.colorChannels = 3;
    world.primitive = native_mc::PreviewVulkanWorldPrimitive::Quads;
    if (!renderer.UploadWorldMesh(world, &error)) {
        std::cerr << "UploadWorldMesh failed: " << error << "\n";
        DestroyWindow(window);
        return 4;
    }

    native_mc::PreviewVulkanFrame frame;
    frame.camera.distance = 3.5f;
    frame.camera.yawDegrees = 0.0f;
    frame.camera.pitchDegrees = 0.0f;
    frame.overlay.gridLineCount = 8;
    frame.overlay.gizmoLength = 0.5f;
    frame.model.sourceQuaternion = { 0.9238795f, 0.0f, 0.3826834f, 0.0f };

    native_mc::PreviewVulkanRenderStats stats{};
    constexpr std::array<native_mc::PreviewVulkanDrawMode, 3> modes = {
        native_mc::PreviewVulkanDrawMode::Solid,
        native_mc::PreviewVulkanDrawMode::Wireframe,
        native_mc::PreviewVulkanDrawMode::SolidWire,
    };
    for (const auto mode : modes) {
        frame.drawMode = mode;
        for (int index = 0; index < 20; ++index) {
            frame.lodLevel = static_cast<std::uint32_t>(index % 4);
            PumpMessages();
            if (!renderer.Render(frame, &stats, &error)) {
                std::cerr << "Render failed: " << error << "\n";
                DestroyWindow(window);
                return 5;
            }
        }
    }

    const std::array<float, 9> trianglePositions = {
        -0.5f, -0.9f, -0.2f, 0.5f, -0.9f, -0.2f, 0.0f, -0.9f, 0.6f,
    };
    const std::array<std::uint8_t, 9> triangleColors = {
        120, 160, 210, 120, 160, 210, 120, 160, 210,
    };
    world.positions = trianglePositions.data();
    world.positionFloatCount = trianglePositions.size();
    world.colors = triangleColors.data();
    world.colorByteCount = triangleColors.size();
    world.primitive = native_mc::PreviewVulkanWorldPrimitive::Triangles;
    if (!renderer.UploadWorldMesh(world, &error) || !renderer.Render(frame, &stats, &error)) {
        std::cerr << "Triangle world render failed: " << error << "\n";
        DestroyWindow(window);
        return 6;
    }
    world.primitive = native_mc::PreviewVulkanWorldPrimitive::Points;
    if (!renderer.UploadWorldMesh(world, &error) || !renderer.Render(frame, &stats, &error)) {
        std::cerr << "Point world render failed: " << error << "\n";
        DestroyWindow(window);
        return 7;
    }

    SetWindowPos(window, nullptr, 0, 0, 800, 600, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    PumpMessages();
    if (!renderer.Resize(800, 600, &error) || !renderer.Render(frame, &stats, &error)) {
        std::cerr << "Resize/render failed: " << error << "\n";
        DestroyWindow(window);
        return 8;
    }

    const std::uint32_t apiVersion = renderer.ApiVersion();
    std::cout << "adapter=" << renderer.AdapterName()
              << " api=" << (apiVersion >> 22u) << '.'
              << ((apiVersion >> 12u) & 0x3ffu) << '.'
              << (apiVersion & 0xfffu)
              << " hardware=" << renderer.IsHardwareDevice()
              << " wireframe=" << renderer.SupportsWireframe()
              << " frame=" << stats.frameNumber
              << " draws=" << stats.drawCalls
              << " triangles=" << stats.modelTriangles
              << " cpu_ms=" << stats.cpuMilliseconds << "\n";
    renderer.Shutdown();
    if (!renderer.Initialize(window, options, &error) || !renderer.Render(frame, &stats, &error)) {
        std::cerr << "Reinitialize/render failed: " << error << "\n";
        DestroyWindow(window);
        return 9;
    }
    renderer.Shutdown();
    DestroyWindow(window);
    return 0;
}
