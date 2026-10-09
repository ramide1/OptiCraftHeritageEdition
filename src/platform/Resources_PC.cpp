#include "platform/Resources.h"
#include "platform/storage/AssetPak.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#else
#include <system_error>
#endif

std::string PlatformResources::baseDir()
{
    // The executable's own directory — what SDL_GetBasePath used to report.
#if defined(_WIN32)
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        return std::filesystem::path(std::wstring(buffer, length)).parent_path().lexically_normal().string();
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    if (size > 0)
    {
        std::vector<char> buffer(size);
        if (_NSGetExecutablePath(buffer.data(), &size) == 0)
            return std::filesystem::path(buffer.data()).parent_path().lexically_normal().string();
    }
#else
    // /proc/self/exe is a symlink to the executable on Linux (and the BSDs
    // that ship procfs, which covers every CI desktop target).
    std::error_code ec;
    const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
        return exe.parent_path().lexically_normal().string();
#endif
    return std::filesystem::current_path().string();
}

std::string PlatformResources::assetsDir()
{
    return (std::filesystem::path(baseDir()) / "assets").string();
}

std::string PlatformResources::audioDir()
{
    return (std::filesystem::path(baseDir()) / "resources").string();
}

std::string PlatformResources::resolveExisting(const std::string& path)
{
    // The pak sits beside the exe and is keyed the way `path` is spelled here
    // ("assets/...", "resources/..."); a hit answers with a pak:// path.
    if (AssetPak::mountFrom(baseDir()) && AssetPak::exists(path))
        return AssetPak::makePath(path);

    std::string resolved;
    if (path.rfind("assets/", 0) == 0)
        resolved = assetsDir() + "/" + path.substr(7);
    else
        resolved = baseDir() + "/" + path;

    std::ifstream file(resolved, std::ios::binary);
    return file.good() ? resolved : std::string();
}

std::string PlatformResources::resolveAsset(const std::string& input)
{
    std::string path = input;
    if (!path.empty() && path[0] == '/')
        path.erase(path.begin());
    return resolveExisting("assets/" + path);
}

long PlatformResources::fileSize(const std::string& path)
{
    if (AssetPak::isPakPath(path))
        return AssetPak::size(AssetPak::keyOf(path));
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    return file ? static_cast<long>(file.tellg()) : -1L;
}

unsigned char* PlatformResources::loadFile(const std::string& path, unsigned int* outSize)
{
    if (AssetPak::isPakPath(path))
        return AssetPak::load(AssetPak::keyOf(path), outSize);
    if (outSize)
        *outSize = 0;
    const long size = fileSize(path);
    if (size <= 0)
        return nullptr;

    unsigned char* data = static_cast<unsigned char*>(std::malloc(static_cast<std::size_t>(size)));
    if (!data)
        return nullptr;

    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(data), size))
    {
        std::free(data);
        return nullptr;
    }
    if (outSize)
        *outSize = static_cast<unsigned int>(size);
    return data;
}
