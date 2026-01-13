#ifndef ANALYZER_FAST_REMOVE_FILES_H_H
#define ANALYZER_FAST_REMOVE_FILES_H_H
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

namespace avs_analyzer::fast_remove_files {
struct Error {
    enum class Op { Unlink, RmDir, SkipPath };
    Op op{};
    int err{};
    std::string path;
    std::string message;
};

struct Report {
    std::size_t files_requested = 0;
    std::size_t files_removed = 0;
    std::size_t files_missing_ok = 0;
    std::size_t dirs_removed = 0;

    std::size_t unlink_failed = 0;
    std::size_t rmdir_failed = 0;
    std::size_t skipped_paths = 0;

    std::vector<Error> errors;
};
struct Options {
    // Never remove this directory nor any ancestor. Children may still be removed if empty.
    std::optional<std::string> stop_at;

    // If false, skip any path that contains ".." after lexical normalization.
    bool allow_dotdot = false;

    // Collect detailed errors (minor overhead).
    bool collect_errors = true;
};

Report remove_files_and_empty_parents_pmr(const std::vector<std::string>& filenames,
                                            const Options& opt = {},
                                            std::pmr::memory_resource* upstream = std::pmr::new_delete_resource());

}
#endif
/*
Example:

int main() {
    std::vector<std::string> files = {
        "tmp/a/b/c.txt",
        "tmp/a/b/d.txt",
        "/var/tmp/x/y/z.bin"
    };

    fast_remove_files::Options opt;
    opt.stop_at = "tmp"; // do not remove "tmp" or above

    auto rep = fast_remove_files::remove_files_and_empty_parents_pmr(files, opt);
}
*/