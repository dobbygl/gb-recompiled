#include "gb_filesystem.h"
#include <filesystem>
#include <new>
#include <string>
#include <system_error>
#include <cerrno>
#ifndef _WIN32
#include <sys/stat.h>
#endif

struct GBDirectory {
    std::filesystem::directory_iterator iterator;
    std::string name;
    bool advance = false;
    bool finished = false;
};

extern "C" GBDirectory* gb_directory_open(const char* path) {
    if (!path || !*path) return nullptr;
    try {
        std::error_code error;
        auto iterator = std::filesystem::directory_iterator(path, error);
        if (error) return nullptr;
        return new GBDirectory{iterator, {}, false};
    } catch (...) {
        return nullptr; // C callers must never receive a C++ exception.
    }
}

extern "C" const char* gb_directory_next(GBDirectory* directory) {
    if (!directory || directory->finished) return nullptr;
    try {
        auto& iterator = directory->iterator;
        if (iterator == std::filesystem::directory_iterator{}) return nullptr;
        if (directory->advance) {
            std::error_code error;
            iterator.increment(error);
            if (error || iterator == std::filesystem::directory_iterator{}) {
                directory->finished = true;
                return nullptr;
            }
        }
        directory->name = iterator->path().filename().string();
        directory->advance = true;
        return directory->name.c_str();
    } catch (...) {
        // Keep the iterator owned until close; further C calls must remain at EOF.
        // Assigning an end iterator here also trips GCC 13's optimized
        // libstdc++ directory_iterator move-assignment linkage.
        directory->finished = true;
        return nullptr;
    }
}

extern "C" void gb_directory_close(GBDirectory* directory) {
    delete directory;
}

static int directory_error(const std::error_code& error) {
    const auto condition = error.default_error_condition();
    errno = condition.category() == std::generic_category() ? condition.value() : EIO;
    return -1;
}

extern "C" int gb_make_directory(const char* path) {
    if (!path || !*path) { errno = EINVAL; return -1; }
    try {
#ifndef _WIN32
        // Preserve the runtime's original 0755 creation mode on POSIX.
        if (mkdir(path, 0755) == 0) return 0;
        if (errno != EEXIST) return -1;
#endif
        std::error_code error;
#ifdef _WIN32
        if (std::filesystem::create_directory(path, error)) return 0;
        if (error) return directory_error(error);
#endif
        const bool directory = std::filesystem::is_directory(path, error);
        if (error) return directory_error(error);
        if (directory) return 0;
        errno = ENOTDIR;
        return -1;
    } catch (const std::bad_alloc&) {
        errno = ENOMEM;
        return -1;
    } catch (...) {
        errno = EIO;
        return -1;
    }
}
