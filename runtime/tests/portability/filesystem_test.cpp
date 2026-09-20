#include "gb_filesystem.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
static std::set<std::string> names(const std::filesystem::path& path) {
    GBDirectory* directory = gb_directory_open(path.string().c_str());
    require(directory != nullptr, "opening existing directory");
    std::set<std::string> result;
    while (const char* name = gb_directory_next(directory)) result.insert(name);
    require(gb_directory_next(directory) == nullptr, "iteration remains finished");
    gb_directory_close(directory);
    return result;
}
int main() {
    namespace fs = std::filesystem;
    fs::path root;
    try {
        root = fs::temp_directory_path() / ("gbrt-filesystem-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(!fs::exists(root), "temporary path must be unique");
        require(gb_make_directory(root.string().c_str()) == 0, "create directory");
        require(gb_make_directory(root.string().c_str()) == 0, "existing directory succeeds");
        require(names(root).empty(), "empty directory without dot entries");
        std::ofstream(root / "file with spaces.cht") << "test";
        std::ofstream(root / "printer_0007.png") << "test";
        require(gb_make_directory((root / "nested").string().c_str()) == 0, "nested directory");
        require(names(root) == std::set<std::string>{"file with spaces.cht", "printer_0007.png", "nested"},
                "all basenames returned exactly once");
        require(gb_make_directory((root / "printer_0007.png").string().c_str()) == -1,
                "existing file is not a directory");
        require(!gb_directory_open((root / "printer_0007.png").string().c_str()), "reject file");
        require(!gb_directory_open((root / "missing").string().c_str()), "reject missing path");
        require(!gb_directory_open(nullptr) && !gb_directory_open(""), "reject null/empty directory");
        require(!gb_directory_next(nullptr), "null iteration");
        require(gb_make_directory(nullptr) == -1 && gb_make_directory("") == -1, "reject null/empty mkdir");
        gb_directory_close(nullptr);
        fs::remove_all(root);
        std::cout << "PASS: create, enumerate, empty/end, names, existing file and invalid paths\n";
        return 0;
    } catch (const std::exception& error) {
        std::error_code ignored;
        if (!root.empty()) fs::remove_all(root, ignored);
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
