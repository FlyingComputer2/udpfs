#pragma once

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <string>

namespace udpfs_test
{

// A fresh directory under the system temp dir, removed with the object.
class temp_dir
{
public:
    temp_dir()
    {
        std::string tmpl = (std::filesystem::temp_directory_path() / "udpfs_test_XXXXXX").string();
        char *p = mkdtemp(tmpl.data());
        path_ = p ? p : "";
    }
    ~temp_dir()
    {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    [[nodiscard]] const std::string &path() const { return path_; }
    [[nodiscard]] std::string operator/(const std::string &name) const { return path_ + "/" + name; }

private:
    std::string path_;
};

}  // namespace udpfs_test
