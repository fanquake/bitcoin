// minify https://github.com/bitcoin/bitcoin/pull/33775
// ./contrib/guix/guix-build
// Produces this non-determinism, when comparing build
// output from x86_64 and aarch64. riscv matches x86_64.
/*
-test.cpp.obj.aarch64:     file format pe-x86-64
+test.cpp.obj.x86_64:     file format pe-x86-64


 Disassembly of section .text:

 0000000000000000 <Func[abi:cxx11](fs::path const&)>:
        0:  41 57                   push   %r15
        2:  41 56                   push   %r14
@@ -714,18 +714,18 @@
      b3f:  49 39 f5                cmp    %rsi,%r13
      b42:  4c 0f 42 ee             cmovb  %rsi,%r13
      b46:  49 83 c6 48             add    $0x48,%r14
      b4a:  4f 8d 64 2c b8          lea    -0x48(%r12,%r13,1),%r12
      b4f:  49 39 ff                cmp    %rdi,%r15
      b52:  0f 84 78 0d 00 00       je     18d0 <Func[abi:cxx11](fs::path const&)+0x18d0>
      b58:  48 89 c5                mov    %rax,%rbp
-     b5b:  4c 39 e3                cmp    %r12,%rbx
-     b5e:  4c 89 e6                mov    %r12,%rsi
+     b5b:  49 39 dc                cmp    %rbx,%r12
+     b5e:  48 89 de                mov    %rbx,%rsi
      b61:  48 8d 4d 10             lea    0x10(%rbp),%rcx
-     b65:  48 0f 43 f3             cmovae %rbx,%rsi
+     b65:  49 0f 43 f4             cmovae %r12,%rsi
      b69:  48 89 4d 00             mov    %rcx,0x0(%rbp)
*/

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