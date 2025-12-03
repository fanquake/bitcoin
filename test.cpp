// minify https://github.com/bitcoin/bitcoin/pull/33775
// ./contrib/guix/guix-build
#include <filesystem>
#include <string>
#include <fstream>
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

bool IsBDBFile(const fs::path& path)
{
    // Removing this condition makes it deterministic.
    if (!fs::exists(path)) return false;
    return true;
}

bool IsSQLiteFile(const fs::path& path)
{
    // Removing this condition makes it deterministic.
    if (!fs::exists(path)) return false;
    return true;
}

std::vector<std::pair<fs::path, std::string>> ListDatabases(const fs::path& wallet_dir)
{
    std::vector<std::pair<fs::path, std::string>> paths;
    std::error_code ec;

    for (auto it = fs::recursive_directory_iterator(wallet_dir, ec); it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path path{it->path().lexically_relative(wallet_dir)};

        if (it->status().type() == fs::file_type::directory) {
            if (IsBDBFile(it->path())) {
                paths.emplace_back(path, "bdb");
            } else if (IsSQLiteFile(it->path())) {
                paths.emplace_back(path, "sqlite");
            }
        } else if (it.depth() == 0 && it->symlink_status().type() == fs::file_type::regular && it->path().extension() != ".bak") {
            if (it->path().filename() == "wallet.dat") {
                if (IsBDBFile(it->path())) {
                    paths.emplace_back(fs::path(), "bdb");
                } else if (IsSQLiteFile(it->path())) {
                    paths.emplace_back(fs::path(), "sqlite");
                }
            } else if (IsBDBFile(it->path())) {
                paths.emplace_back(path, "bdb");
            }
        }
    }

    return paths;
}