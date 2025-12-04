// minify https://github.com/bitcoin/bitcoin/pull/33775
// ./contrib/guix/guix-build
#include <filesystem>
#include <string>
#include <vector>

#include <cstring> // Unused, but removing this makes it determinstic

namespace fs {

using namespace std::filesystem;

// Dummy class. Using "path = std::filesystem::path" makes it deterministic.
class path : public std::filesystem::path
{
public:
    using std::filesystem::path::path;

    path(std::filesystem::path path) : std::filesystem::path::path(std::move(path)) {}
};

}

bool func();

std::vector<std::pair<fs::path, std::string>> ListDatabases(const fs::path& wallet_dir)
{
    std::vector<std::pair<fs::path, std::string>> paths;
    std::error_code ec;

    for (auto it = fs::recursive_directory_iterator(wallet_dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path path{it->path().lexically_relative(wallet_dir)};

        if (func()) {
            if (func()) {
                paths.emplace_back(path, "aaa");
                paths.emplace_back(path, "aaaaaa");
            }
        }
        if (func()) {
            if (func()) {
                paths.emplace_back(fs::path(), "aaa");
                paths.emplace_back(fs::path(), "aaaaaa");
                paths.emplace_back(path, "aaaaaaaaaaaaa");
            }
        }
    }

    return paths;
}