// Helpers shared by unit tests.
#pragma once

#include <stdlib.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace execcontract::test {

// Temporary directory removed on destruction.
class TempDir {
   public:
    TempDir() {
        std::string tmpl = (std::filesystem::temp_directory_path() / "ec-unit-XXXXXX").string();
        if (::mkdtemp(tmpl.data()) == nullptr) {
            throw std::runtime_error("mkdtemp failed");
        }
        path_ = tmpl;
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ec);
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

    std::filesystem::path write(const std::string& name, const std::string& content) const {
        const auto p = path_ / name;
        std::ofstream(p, std::ios::binary) << content;
        return p;
    }

   private:
    std::filesystem::path path_;
};

}  // namespace execcontract::test
