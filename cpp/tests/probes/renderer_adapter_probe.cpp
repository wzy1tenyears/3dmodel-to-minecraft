#define NOMINMAX
#include <Windows.h>

#include "native/preview_renderer.h"

#include <iostream>

int main(int argc, char** argv) {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"PreviewRendererAdapterProbe";
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 2;
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"probe", WS_OVERLAPPEDWINDOW,
                                  0, 0, 640, 480, nullptr, nullptr,
                                  windowClass.hInstance, nullptr);
    if (!window) return 3;

    native_mc::PreviewRenderer renderer;
    native_mc::PreviewRendererOptions options;
    options.backend = native_mc::PreviewRendererBackend::Auto;
    if (argc > 1) {
        const std::string requested = argv[1];
        if (requested == "d3d11") options.backend = native_mc::PreviewRendererBackend::D3D11;
        if (requested == "d3d12") options.backend = native_mc::PreviewRendererBackend::D3D12;
        if (requested == "vulkan") options.backend = native_mc::PreviewRendererBackend::Vulkan;
        if (requested == "opengl") options.backend = native_mc::PreviewRendererBackend::OpenGL;
    }
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

    const float positions[] = {
        -2.0f, -0.1f, -2.0f, 2.0f, -0.1f, -2.0f,
         2.0f, -0.1f,  2.0f, -2.0f, -0.1f,  2.0f
    };
    const unsigned char colors[] = {
        80, 100, 120, 80, 100, 120, 80, 100, 120, 80, 100, 120
    };
    native_mc::PreviewRendererWorldMeshView world;
    world.positions = positions;
    world.positionFloatCount = sizeof(positions) / sizeof(positions[0]);
    world.colors = colors;
    world.colorByteCount = sizeof(colors);
    world.primitive = native_mc::PreviewRendererWorldPrimitive::Quads;
    if (!renderer.UploadWorldMesh(world, &error)) {
        std::cerr << error << '\n';
        DestroyWindow(window);
        return 6;
    }

    native_mc::PreviewRendererFrame frame;
    frame.camera.distance = 6.0f;
    frame.model.sourceQuaternion = { 0.9238795f, -0.3826834f, 0.0f, 0.0f };
    frame.model.quaternion = { 0.9914449f, 0.0f, 0.1305262f, 0.0f };
    frame.overlay.gizmoLength = 1.0f;
    native_mc::PreviewRendererStats stats;
    for (int index = 0; index < 16; ++index) {
        frame.camera.yawDegrees += 1.0f;
        if (!renderer.Render(frame, &stats, &error)) {
            std::cerr << error << '\n';
            DestroyWindow(window);
            return 7;
        }
    }
    std::cout << "requested=" << native_mc::PreviewRendererBackendName(renderer.RequestedBackend())
              << " active=" << native_mc::PreviewRendererBackendName(renderer.ActiveBackend())
              << " adapter=" << renderer.AdapterName() << " hardware=" << renderer.IsHardware()
              << " draws=" << stats.drawCalls << " triangles="
              << (stats.modelTriangles + stats.worldTriangles) << " cpuMs=" << stats.cpuMilliseconds
              << " diagnostics=" << renderer.LastError() << '\n';
    renderer.Shutdown();
    DestroyWindow(window);
    return 0;
}
