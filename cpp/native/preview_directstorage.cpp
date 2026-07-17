#include "preview_directstorage.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wrl/client.h>

#include "../third_party/directstorage/include/dstorage.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace native_mc {

namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kReadChunkBytes = 32u * 1024u * 1024u;
constexpr DWORD kReadTimeoutMilliseconds = 30u * 1000u;
constexpr char kQueueName[] = "Preview cache CPU-memory reads";
constexpr char kRequestName[] = "Preview cache read";

using DStorageGetFactoryFn = HRESULT(WINAPI*)(REFIID, void**);

std::string HrMessage(const char* action, HRESULT hr) {
    std::ostringstream text;
    text << action << " failed (HRESULT 0x" << std::hex << std::setw(8)
         << std::setfill('0') << static_cast<unsigned long>(hr) << ')';
    return text.str();
}

std::string Win32Message(const char* action, DWORD error) {
    std::ostringstream text;
    text << action << " failed (Win32 " << error << ')';

    wchar_t* message = nullptr;
    const DWORD chars = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    if (chars != 0 && message) {
        DWORD messageChars = chars;
        while (messageChars > 0 && (message[messageChars - 1] == L'\r' ||
                                    message[messageChars - 1] == L'\n' ||
                                    message[messageChars - 1] == L' ')) {
            message[--messageChars] = L'\0';
        }
        const int byteCount = WideCharToMultiByte(
            CP_UTF8, 0, message, -1, nullptr, 0, nullptr, nullptr);
        if (byteCount > 1) {
            std::string utf8(static_cast<std::size_t>(byteCount), '\0');
            WideCharToMultiByte(
                CP_UTF8, 0, message, -1, utf8.data(), byteCount, nullptr, nullptr);
            utf8.resize(static_cast<std::size_t>(byteCount - 1));
            text << ": " << utf8;
        }
        LocalFree(message);
    }
    return text.str();
}

std::filesystem::path AbsolutePath(const std::filesystem::path& path) {
    std::error_code ec;
    const auto absolute = std::filesystem::absolute(path, ec);
    return (ec ? path : absolute).lexically_normal();
}

std::uint64_t FileSize(const BY_HANDLE_FILE_INFORMATION& information) {
    return (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32u) |
           static_cast<std::uint64_t>(information.nFileSizeLow);
}

bool ResizeOutput(std::uint64_t byteCount, std::vector<std::uint8_t>* output,
                   std::string* error) {
    if (!output) {
        if (error) *error = "output buffer is null";
        return false;
    }
    if (byteCount > static_cast<std::uint64_t>(output->max_size()) ||
        byteCount > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        if (error) *error = "file is too large for an in-memory buffer";
        return false;
    }
    try {
        output->resize(static_cast<std::size_t>(byteCount));
    } catch (const std::exception& exception) {
        if (error) *error = std::string("could not allocate file buffer: ") + exception.what();
        return false;
    }
    return true;
}

bool CheckReadLimit(std::uint64_t byteCount, std::uint64_t maxBytes,
                    std::string* error) {
    if (byteCount <= maxBytes) return true;
    if (error) {
        std::ostringstream text;
        text << "file size " << byteCount
             << " exceeds the configured in-memory read limit " << maxBytes;
        *error = text.str();
    }
    return false;
}

std::filesystem::path ExecutableDirectory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true) {
        const DWORD chars = GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (chars == 0) return {};
        if (chars < buffer.size() - 1) {
            return std::filesystem::path(std::wstring(buffer.data(), chars)).parent_path();
        }
        if (buffer.size() >= 32768) return {};
        buffer.resize(std::min<std::size_t>(32768, buffer.size() * 2));
    }
}

HMODULE LoadDirectStorageModule(std::string* error) {
    const auto executableDirectory = ExecutableDirectory();
    if (!executableDirectory.empty()) {
        const auto bundledPath = executableDirectory / L"dstorage.dll";
        HMODULE module = LoadLibraryExW(
            bundledPath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module) return module;

        const DWORD bundledError = GetLastError();
        module = LoadLibraryExW(
            L"dstorage.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module) return module;

        if (error) {
            *error = Win32Message("LoadLibraryExW(dstorage.dll)", bundledError);
        }
        return nullptr;
    }

    HMODULE module = LoadLibraryExW(
        L"dstorage.dll", nullptr,
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module && error) {
        *error = Win32Message("LoadLibraryExW(dstorage.dll)", GetLastError());
    }
    return module;
}

class DirectStorageRuntime {
public:
    PreviewDirectStorageAvailability Probe() {
        std::lock_guard<std::mutex> lock(mutex_);
        PreviewDirectStorageAvailability result;
        result.available = EnsureInitialized();
        result.status = result.available ? "DirectStorage factory ready"
                                         : initializationError_;
        return result;
    }

    bool AcquireFactory(ComPtr<IDStorageFactory>* factory, std::string* error) {
        if (!factory) {
            if (error) *error = "DirectStorage factory output pointer is null";
            return false;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureInitialized()) {
            if (error) *error = initializationError_;
            return false;
        }
        *factory = factory_;
        return true;
    }

private:
    bool EnsureInitialized() {
        if (factory_) return true;
        if (initializationAttempted_) return false;
        initializationAttempted_ = true;

        module_ = LoadDirectStorageModule(&initializationError_);
        if (!module_) return false;

        const auto getFactory = reinterpret_cast<DStorageGetFactoryFn>(
            GetProcAddress(module_, "DStorageGetFactory"));
        if (!getFactory) {
            initializationError_ = Win32Message(
                "GetProcAddress(DStorageGetFactory)", GetLastError());
            ResetObjectsAndModule();
            return false;
        }

        const HRESULT hr = getFactory(IID_PPV_ARGS(&factory_));
        if (FAILED(hr)) {
            initializationError_ = HrMessage("DStorageGetFactory", hr);
            ResetObjectsAndModule();
            return false;
        }

        initializationError_.clear();
        return true;
    }

    void ResetObjectsAndModule() {
        factory_.Reset();
        if (module_) {
            FreeLibrary(module_);
            module_ = nullptr;
        }
    }

    std::mutex mutex_;
    HMODULE module_ = nullptr;
    ComPtr<IDStorageFactory> factory_;
    bool initializationAttempted_ = false;
    std::string initializationError_;
};

class DirectStorageReadGuard {
public:
    DirectStorageReadGuard(IDStorageQueue* queue, IDStorageFile* file, HANDLE event)
        : queue_(queue), file_(file), event_(event) {}

    ~DirectStorageReadGuard() { Close(); }

    DirectStorageReadGuard(const DirectStorageReadGuard&) = delete;
    DirectStorageReadGuard& operator=(const DirectStorageReadGuard&) = delete;

    void Close() noexcept {
        // IDStorageQueue::Close guarantees that, after it returns, this queue
        // will not complete further requests. The destination buffer may only
        // be cleared or reused after this call.
        if (queue_) {
            queue_->Close();
            queue_ = nullptr;
        }
        if (file_) {
            file_->Close();
            file_ = nullptr;
        }
        if (event_) {
            CloseHandle(event_);
            event_ = nullptr;
        }
    }

private:
    IDStorageQueue* queue_ = nullptr;
    IDStorageFile* file_ = nullptr;
    HANDLE event_ = nullptr;
};

DirectStorageRuntime& Runtime() {
    // Preview jobs are detached from the GUI message loop. Keep the factory and
    // its implementing DLL alive until process teardown so a closing window
    // cannot race static destruction with an in-flight read.
    static DirectStorageRuntime* runtime = new DirectStorageRuntime();
    return *runtime;
}

bool ReadWithDirectStorage(const std::filesystem::path& path,
                           std::vector<std::uint8_t>* output,
                           std::uint64_t maxBytes,
                           std::uint64_t* expectedBytes,
                           std::uint64_t* bytesRead,
                           std::string* error) {
    if (expectedBytes) *expectedBytes = 0;
    if (bytesRead) *bytesRead = 0;

    ComPtr<IDStorageFactory> factory;
    if (!Runtime().AcquireFactory(&factory, error)) return false;

    const auto absolutePath = AbsolutePath(path);
    ComPtr<IDStorageFile> file;
    HRESULT hr = factory->OpenFile(absolutePath.c_str(), IID_PPV_ARGS(&file));
    if (FAILED(hr)) {
        if (error) *error = HrMessage("IDStorageFactory::OpenFile", hr);
        return false;
    }

    BY_HANDLE_FILE_INFORMATION before{};
    hr = file->GetFileInformation(&before);
    if (FAILED(hr)) {
        if (error) *error = HrMessage("IDStorageFile::GetFileInformation", hr);
        file->Close();
        return false;
    }

    const std::uint64_t fileBytes = FileSize(before);
    if (expectedBytes) *expectedBytes = fileBytes;
    if (!CheckReadLimit(fileBytes, maxBytes, error)) {
        file->Close();
        return false;
    }
    if (!ResizeOutput(fileBytes, output, error)) {
        file->Close();
        return false;
    }
    if (fileBytes == 0) {
        file->Close();
        return true;
    }

    DSTORAGE_QUEUE_DESC description{};
    description.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
    description.Capacity = DSTORAGE_MIN_QUEUE_CAPACITY;
    description.Priority = DSTORAGE_PRIORITY_NORMAL;
    description.Name = kQueueName;
    description.Device = nullptr;
    ComPtr<IDStorageQueue1> queue;
    hr = factory->CreateQueue(&description, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) {
        if (error) *error = HrMessage("IDStorageFactory::CreateQueue", hr);
        file->Close();
        return false;
    }

    ComPtr<IDStorageStatusArray> status;
    hr = factory->CreateStatusArray(
        1, "Preview cache read status", IID_PPV_ARGS(&status));
    if (FAILED(hr)) {
        if (error) *error = HrMessage("IDStorageFactory::CreateStatusArray", hr);
        queue->Close();
        file->Close();
        return false;
    }

    HANDLE completionEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!completionEvent) {
        if (error) *error = Win32Message("CreateEventW", GetLastError());
        queue->Close();
        file->Close();
        return false;
    }

    DirectStorageReadGuard readGuard(queue.Get(), file.Get(), completionEvent);

    std::uint64_t scheduledBytes = 0;
    while (scheduledBytes < fileBytes) {
        const auto remaining = fileBytes - scheduledBytes;
        const auto chunkBytes = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(remaining, kReadChunkBytes));

        DSTORAGE_REQUEST request{};
        request.Options.SourceType = DSTORAGE_REQUEST_SOURCE_FILE;
        request.Options.DestinationType = DSTORAGE_REQUEST_DESTINATION_MEMORY;
        request.Options.CompressionFormat = DSTORAGE_COMPRESSION_FORMAT_NONE;
        request.Source.File.Source = file.Get();
        request.Source.File.Offset = scheduledBytes;
        request.Source.File.Size = chunkBytes;
        request.Destination.Memory.Buffer = output->data() +
            static_cast<std::size_t>(scheduledBytes);
        request.Destination.Memory.Size = chunkBytes;
        request.UncompressedSize = chunkBytes;
        request.Name = kRequestName;
        queue->EnqueueRequest(&request);
        scheduledBytes += chunkBytes;
    }

    if (scheduledBytes != fileBytes) {
        if (error) *error = "DirectStorage scheduled byte count does not match file size";
        return false;
    }

    queue->EnqueueStatus(status.Get(), 0);
    queue->EnqueueSetEvent(completionEvent);
    queue->Submit();

    const DWORD waitResult = WaitForSingleObject(
        completionEvent, kReadTimeoutMilliseconds);
    if (waitResult != WAIT_OBJECT_0) {
        if (error) {
            if (waitResult == WAIT_TIMEOUT) {
                *error = "DirectStorage read timed out after " +
                         std::to_string(kReadTimeoutMilliseconds) + " ms";
            } else if (waitResult == WAIT_FAILED) {
                *error = Win32Message(
                    "WaitForSingleObject(DirectStorage)", GetLastError());
            } else {
                *error = "DirectStorage completion wait returned an unexpected result";
            }
        }
        return false;
    }

    hr = status->GetHResult(0);
    if (FAILED(hr)) {
        std::string detail = HrMessage("DirectStorage request", hr);
        const HANDLE errorEvent = queue->GetErrorEvent();
        if (errorEvent && WaitForSingleObject(errorEvent, 0) == WAIT_OBJECT_0) {
            DSTORAGE_ERROR_RECORD record{};
            queue->RetrieveErrorRecord(&record);
            if (record.FailureCount != 0) {
                std::ostringstream queueError;
                queueError << "queue reported " << record.FailureCount
                           << " failure(s), first HRESULT 0x" << std::hex
                           << std::setw(8) << std::setfill('0')
                           << static_cast<unsigned long>(record.FirstFailure.HResult);
                detail += "; " + queueError.str();
            }
        }
        if (error) *error = std::move(detail);
        return false;
    }

    BY_HANDLE_FILE_INFORMATION after{};
    hr = file->GetFileInformation(&after);
    if (FAILED(hr)) {
        if (error) *error = HrMessage("IDStorageFile::GetFileInformation(after read)", hr);
        return false;
    }
    if (FileSize(after) != fileBytes) {
        if (error) *error = "file size changed during DirectStorage read";
        return false;
    }
    if (scheduledBytes != output->size()) {
        if (error) *error = "DirectStorage output length validation failed";
        return false;
    }

    readGuard.Close();
    if (bytesRead) *bytesRead = scheduledBytes;
    return true;
}

bool ReadWithWin32(const std::filesystem::path& filePath,
                   std::vector<std::uint8_t>* output,
                   std::uint64_t maxBytes,
                   std::uint64_t* expectedBytes,
                   std::uint64_t* bytesRead,
                   std::string* error) {
    if (expectedBytes) *expectedBytes = 0;
    if (bytesRead) *bytesRead = 0;

    const auto absolutePath = AbsolutePath(filePath);
    HANDLE file = CreateFileW(
        absolutePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = Win32Message("CreateFileW", GetLastError());
        return false;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) {
        if (error) *error = Win32Message("GetFileSizeEx", GetLastError());
        CloseHandle(file);
        return false;
    }
    if (size.QuadPart < 0) {
        if (error) *error = "GetFileSizeEx returned a negative size";
        CloseHandle(file);
        return false;
    }

    const std::uint64_t fileBytes = static_cast<std::uint64_t>(size.QuadPart);
    if (expectedBytes) *expectedBytes = fileBytes;
    if (!CheckReadLimit(fileBytes, maxBytes, error)) {
        CloseHandle(file);
        return false;
    }
    if (!ResizeOutput(fileBytes, output, error)) {
        CloseHandle(file);
        return false;
    }

    std::uint64_t totalRead = 0;
    while (totalRead < fileBytes) {
        const auto remaining = fileBytes - totalRead;
        const DWORD requested = static_cast<DWORD>(
            std::min<std::uint64_t>(remaining, kReadChunkBytes));
        DWORD completed = 0;
        if (!ReadFile(
                file, output->data() + static_cast<std::size_t>(totalRead),
                requested, &completed, nullptr)) {
            if (error) *error = Win32Message("ReadFile", GetLastError());
            CloseHandle(file);
            return false;
        }
        if (completed == 0) {
            if (error) *error = "ReadFile reached end of file before the expected length";
            CloseHandle(file);
            return false;
        }
        totalRead += completed;
    }

    LARGE_INTEGER after{};
    if (!GetFileSizeEx(file, &after)) {
        if (error) *error = Win32Message("GetFileSizeEx(after read)", GetLastError());
        CloseHandle(file);
        return false;
    }
    CloseHandle(file);

    if (after.QuadPart < 0 || static_cast<std::uint64_t>(after.QuadPart) != fileBytes) {
        if (error) *error = "file size changed during Win32 read";
        return false;
    }
    if (totalRead != fileBytes || totalRead != output->size()) {
        if (error) *error = "Win32 output length validation failed";
        return false;
    }

    if (bytesRead) *bytesRead = totalRead;
    return true;
}

}  // namespace

const char* PreviewFileReadBackendName(PreviewFileReadBackend backend) {
    switch (backend) {
    case PreviewFileReadBackend::DirectStorage:
        return "DirectStorage";
    case PreviewFileReadBackend::Win32:
        return "Win32";
    default:
        return "None";
    }
}

PreviewDirectStorageAvailability ProbePreviewDirectStorage() {
    return Runtime().Probe();
}

PreviewFileReadResult ReadPreviewFileToMemory(
    const std::filesystem::path& filePath,
    std::vector<std::uint8_t>* output,
    std::uint64_t maxBytes) {
    const auto started = std::chrono::steady_clock::now();
    PreviewFileReadResult result;
    if (!output) {
        result.error = "output buffer is null";
        result.status = result.error;
        return result;
    }
    output->clear();

    const auto availability = Runtime().Probe();
    result.directStorageAvailable = availability.available;

    if (availability.available) {
        std::string directStorageError;
        if (ReadWithDirectStorage(
                filePath, output, maxBytes, &result.expectedBytes, &result.bytesRead,
                &directStorageError)) {
            result.success = true;
            result.backend = PreviewFileReadBackend::DirectStorage;
            result.status = "DirectStorage CPU-memory read completed";
        } else {
            result.directStorageStatus = directStorageError;
            result.usedFallback = true;
        }
    } else {
        result.directStorageStatus = availability.status;
        result.usedFallback = true;
    }

    if (!result.success) {
        output->clear();
        std::string win32Error;
        if (ReadWithWin32(
                filePath, output, maxBytes, &result.expectedBytes, &result.bytesRead,
                &win32Error)) {
            result.success = true;
            result.backend = PreviewFileReadBackend::Win32;
            if (result.directStorageStatus.empty()) {
                result.status = "Win32 file read completed";
            } else if (result.directStorageAvailable) {
                result.status = "Win32 fallback completed after DirectStorage read failed";
            } else {
                result.status = "Win32 fallback completed after DirectStorage was unavailable";
            }
        } else {
            result.backend = PreviewFileReadBackend::Win32;
            result.error = win32Error;
            if (!result.directStorageStatus.empty()) {
                result.error = "DirectStorage: " + result.directStorageStatus +
                               "; Win32 fallback: " + result.error;
            }
            result.status = result.error;
            output->clear();
            result.bytesRead = 0;
        }
    }

    if (result.success &&
        (result.bytesRead != result.expectedBytes || result.bytesRead != output->size())) {
        result.success = false;
        result.error = "file read length validation failed";
        result.status = result.error;
        output->clear();
        result.bytesRead = 0;
    }

    const auto finished = std::chrono::steady_clock::now();
    result.elapsedMilliseconds =
        std::chrono::duration<double, std::milli>(finished - started).count();
    return result;
}

}  // namespace native_mc
