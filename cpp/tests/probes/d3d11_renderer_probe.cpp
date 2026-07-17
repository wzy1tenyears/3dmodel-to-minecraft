#define NOMINMAX
#include <Windows.h>

#include "native/preview_d3d11_renderer.h"

#include <iostream>

int main() {
    const wchar_t* className = L"PreviewD3D11RendererProbe";
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = className;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 2;
    HWND window = CreateWindowExW(0, className, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 640, 480, nullptr, nullptr,
                                  windowClass.hInstance, nullptr);
    if (!window) return 3;

    native_mc::PreviewD3D11Renderer renderer;
    native_mc::PreviewD3D11Options options;
    options.verticalSync = false;
    std::string error;
    if (!renderer.Initialize(window, options, &error)) {
        std::cerr << error << '\n';
        DestroyWindow(window);
        return 4;
    }

    native_mc::PreviewMesh mesh;
    mesh.positions = { -1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.5f, 0.0f };
    mesh.normals = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
    mesh.colors = { 255, 64, 64, 255, 64, 255, 64, 255, 64, 64, 255, 255 };
    mesh.indices = { 0, 1, 2 };
    for (auto& lod : mesh.lodIndices) lod = mesh.indices;
    if (!renderer.UploadMesh(mesh, &error)) {
        std::cerr << error << '\n';
        DestroyWindow(window);
        return 5;
    }

    const float worldPositions[] = {
        -2.0f, -0.1f, -2.0f, 2.0f, -0.1f, -2.0f,
         2.0f, -0.1f,  2.0f, -2.0f, -0.1f, 2.0f
    };
    const unsigned char worldColors[] = {
        80, 100, 120, 80, 100, 120, 80, 100, 120, 80, 100, 120
    };
    native_mc::PreviewD3D11WorldMeshView world;
    world.positions = worldPositions;
    world.positionFloatCount = sizeof(worldPositions) / sizeof(worldPositions[0]);
    world.colors = worldColors;
    world.colorByteCount = sizeof(worldColors);
    world.primitive = native_mc::PreviewD3D11WorldPrimitive::Quads;
    if (!renderer.UploadWorldMesh(world, &error)) {
        std::cerr << error << '\n';
        DestroyWindow(window);
        return 6;
    }

    native_mc::PreviewD3D11Frame frame;
    frame.camera.distance = 6.0f;
    frame.overlay.gizmoLength = 1.0f;
    native_mc::PreviewD3D11RenderStats stats;
    double totalCpuMs = 0.0;
    for (int frameIndex = 0; frameIndex < 120; ++frameIndex) {
        frame.camera.yawDegrees += 0.25f;
        if (!renderer.Render(frame, &stats, &error)) {
            std::cerr << error << '\n';
            DestroyWindow(window);
            return 7;
        }
        totalCpuMs += stats.cpuMilliseconds;
    }
    std::cout << renderer.AdapterName() << " hardware=" << renderer.IsHardwareDevice()
              << " draws=" << stats.drawCalls << " modelTriangles=" << stats.modelTriangles
              << " worldTriangles=" << stats.worldTriangles << " avgCpuMs=" << (totalCpuMs / 120.0) << '\n';
    renderer.Shutdown();
    DestroyWindow(window);
    return 0;
}
