#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace native_mc {

// Preview cache entries are per-model proxy meshes and should remain well below
// this ceiling. Callers with a tighter format-specific bound should pass it
// explicitly to ReadPreviewFileToMemory.
inline constexpr std::uint64_t kDefaultPreviewFileReadMaxBytes =
    1ull * 1024ull * 1024ull * 1024ull;

enum class PreviewFileReadBackend {
    None,
    DirectStorage,
    Win32,
};

const char* PreviewFileReadBackendName(PreviewFileReadBackend backend);

struct PreviewDirectStorageAvailability {
    bool available = false;
    std::string status;
};

struct PreviewFileReadResult {
    bool success = false;
    PreviewFileReadBackend backend = PreviewFileReadBackend::None;
    bool directStorageAvailable = false;
    bool usedFallback = false;
    std::uint64_t expectedBytes = 0;
    std::uint64_t bytesRead = 0;
    double elapsedMilliseconds = 0.0;

    // A concise description suitable for a status line.
    std::string status;

    // Populated when DirectStorage was unavailable or its read attempt failed.
    // A successful Win32 fallback keeps this diagnostic for logging.
    std::string directStorageStatus;
    std::string error;
};

// Probes the redistributable runtime and creates the shared factory. Each read
// creates its own CPU-memory queue so concurrent preview workers do not block
// behind one process-wide completion event.
// Runtime DLLs are loaded dynamically so a missing/incompatible runtime does
// not prevent the application from starting or using the Win32 fallback.
PreviewDirectStorageAvailability ProbePreviewDirectStorage();

// Reads the complete file into memory. DirectStorage is attempted first; any
// runtime, open, queue, or request failure automatically falls back to Win32.
// A successful result guarantees bytesRead == expectedBytes == output->size().
PreviewFileReadResult ReadPreviewFileToMemory(
    const std::filesystem::path& filePath,
    std::vector<std::uint8_t>* output,
    std::uint64_t maxBytes = kDefaultPreviewFileReadMaxBytes);

}  // namespace native_mc
