#include "fast_rm_files.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <unistd.h> // ::unlink, ::rmdir

namespace avs_analyzer::fast_remove_files {

// ---------- Transparent hash/equal for pmr::string ----------
struct PmrStringHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view sv) const noexcept {
        return std::hash<std::string_view>{}(sv);
    }
    std::size_t operator()(const std::pmr::string& s) const noexcept {
        return (*this)(std::string_view{s});
    }
};

struct PmrStringEq {
    using is_transparent = void;

    bool operator()(std::string_view a, std::string_view b) const noexcept { return a == b; }
    bool operator()(const std::pmr::string& a, std::string_view b) const noexcept { return a == b; }
    bool operator()(std::string_view a, const std::pmr::string& b) const noexcept { return a == b; }
    bool operator()(const std::pmr::string& a, const std::pmr::string& b) const noexcept { return a == b; }
};

// ---------- Trie node ----------
struct Node {
    // children[name] -> Node*
    std::pmr::unordered_map<std::pmr::string, Node*, PmrStringHash, PmrStringEq> children;
    bool unlink_target = false; // mark: attempt unlink() at this path

    explicit Node(std::pmr::memory_resource* r) : children(r) {}
};

// ---------- Helpers ----------
inline void push_error(Report& rep, const Options& opt, Error::Op op, int err, std::string path) {
    if (!opt.collect_errors) return;
    rep.errors.push_back(Error{op, err, std::move(path), std::strerror(err)});
}

// Normalize and (optionally) enforce ".." policy.
// Return: normalized string path (POSIX style), or empty if skipped.
 std::string normalize_path_or_empty(const std::string& in, const Options& opt, Report& rep) {
    std::filesystem::path p(in);
    p = p.lexically_normal();

    // Note: lexically_normal can still contain ".." depending on input/root.
    // We optionally reject.
    if (!opt.allow_dotdot) {
        for (const auto& part : p) {
            if (part == "..") {
                rep.skipped_paths++;
                push_error(rep, opt, Error::Op::SkipPath, EINVAL, p.string());
                return {};
            }
        }
    }
    return p.string();
}

// Split normalized path into (root_prefix, components) without extra allocations.
// - root_prefix: "/" if absolute, "" otherwise
// - components: vector of string_view into `path_str`
 bool split_components(std::string_view path_str,
                            std::string_view& root_prefix,
                            std::vector<std::string_view>& comps_out) {
    comps_out.clear();

    bool abs = !path_str.empty() && path_str.front() == '/';
    root_prefix = abs ? std::string_view{"/"} : std::string_view{""};

    // Skip leading '/'
    std::size_t i = abs ? 1 : 0;
    while (i < path_str.size()) {
        while (i < path_str.size() && path_str[i] == '/') ++i;
        if (i >= path_str.size()) break;

        std::size_t j = i;
        while (j < path_str.size() && path_str[j] != '/') ++j;

        std::string_view part = path_str.substr(i, j - i);
        if (!part.empty() && part != ".") {
            comps_out.push_back(part);
        }
        i = j;
    }
    return !comps_out.empty();
}

// Attempt unlink; treat ENOENT as OK; treat EISDIR as "not a file" (not fatal for our purposes).
bool unlink_path(const char* p, Report& rep, const Options& opt) {
    if (::unlink(p) == 0) {
        rep.files_removed++;
        return true;
    }
    int e = errno;
    if (e == ENOENT) {
        rep.files_missing_ok++;
        return true;
    }
    if (e == EISDIR) {
        // The path is a directory; not a fatal error (we may attempt rmdir later).
        // Do not increment unlink_failed, because it is an expected “type mismatch” case.
        return true;
    }
    rep.unlink_failed++;
    push_error(rep, opt, Error::Op::Unlink, e, p);
    return false;
}

// Attempt rmdir; treat ENOENT as OK; treat ENOTDIR as "not a directory" (OK); ENOTEMPTY stops cascading.
bool rmdir_path(const char* p, Report& rep, const Options& opt) {
    if (::rmdir(p) == 0) {
        rep.dirs_removed++;
        return true;
    }
    int e = errno;
    if (e == ENOENT) return true;
    if (e == ENOTDIR) return true;
    if (e == ENOTEMPTY || e == EEXIST) {
        // Not empty: stop cascade upward.
        return false;
    }
    rep.rmdir_failed++;
    push_error(rep, opt, Error::Op::RmDir, e, p);
    return false;
}

// Post-order traversal.
// Returns whether this path can be forgotten from the trie *and* allows parent directory removal attempt.
bool process_node(Node* node,
                         std::string& path_buf,
                         const std::optional<std::string>& stop_at_norm,
                         Report& rep,
                         const Options& opt) {
    bool children_all_gone = true;

    for (auto it = node->children.begin(); it != node->children.end();) {
        const std::pmr::string& name = it->first;
        Node* child = it->second;

        std::size_t old_len = path_buf.size();
        if (!path_buf.empty() && path_buf.back() != '/') path_buf.push_back('/');
        path_buf.append(name.data(), name.size());

        bool child_gone = process_node(child, path_buf, stop_at_norm, rep, opt);

        path_buf.resize(old_len);

        if (child_gone) {
            it = node->children.erase(it);
        } else {
            ++it;
            children_all_gone = false;
        }
    }

    // Optional unlink target at this exact path.
    bool unlink_ok = true;
    if (node->unlink_target) {
        unlink_ok = unlink_path(path_buf.c_str(), rep, opt);
    }

    // Respect stop boundary: do not remove this directory nor allow parent cascade.
    if (stop_at_norm && path_buf == *stop_at_norm) {
        return false;
    }

    // If children are gone, try to remove this directory (cheap: no scans).
    // Even if this node was a file target, attempting rmdir is safe; ENOTDIR will be treated as OK.
    bool rmdir_ok = true;
    if (children_all_gone && node->children.empty()) {
        rmdir_ok = rmdir_path(path_buf.c_str(), rep, opt);
        if (!rmdir_ok) {
            // ENOTEMPTY case: block parent cascade.
            return false;
        }
    }

    // We can forget this node from trie only if:
    // - all children are gone, AND
    // - unlink (if requested) did not hit a hard error, AND
    // - rmdir did not block cascade
    return children_all_gone && unlink_ok && rmdir_ok;
}

// Heuristic sizing for the monotonic buffer to reduce upstream allocations.
std::size_t estimate_arena_bytes(const std::vector<std::string>& filenames) {
    std::size_t total_len = 0;
    std::size_t slash_count = 0;
    for (const auto& s : filenames) {
        total_len += s.size();
        for (char c : s) slash_count += (c == '/');
    }

    // Rough heuristic:
    // - Node + hash buckets + strings
    // - Clamp to avoid pathological allocations.
    std::size_t est = 64 * 1024; // minimum
    est += total_len * 2;
    est += (filenames.size() + slash_count) * 96;

    const std::size_t max_est = 64ull * 1024 * 1024; // 64 MiB clamp
    if (est > max_est) est = max_est;
    return est;
}

// ---------- Public API ----------
Report remove_files_and_empty_parents_pmr(const std::vector<std::string>& filenames,
                                                const Options& opt,
                                                std::pmr::memory_resource* upstream) {
    Report rep;
    rep.files_requested = filenames.size();

    // Arena (PMR)
    const std::size_t arena_bytes = estimate_arena_bytes(filenames);
    std::vector<std::byte> buffer(arena_bytes);
    std::pmr::monotonic_buffer_resource arena(buffer.data(), buffer.size(), upstream);

    // Roots keyed by root prefix: "" or "/"
    std::pmr::unordered_map<std::pmr::string, Node*, PmrStringHash, PmrStringEq> roots(&arena);

    auto new_node = [&](void) -> Node* {
        std::pmr::polymorphic_allocator<Node> a(&arena);
        return a.new_object<Node>(&arena);
    };

    auto get_root = [&](std::string_view root_prefix) -> Node* {
        auto it = roots.find(root_prefix);
        if (it != roots.end()) return it->second;

        Node* r = new_node();
        std::pmr::polymorphic_allocator<char> ca(&arena);
        std::pmr::string key(root_prefix, ca);
        roots.emplace(std::move(key), r);
        return r;
    };

    // Normalize stop_at (string-based; not PMR-critical)
    std::optional<std::string> stop_at_norm;
    if (opt.stop_at && !opt.stop_at->empty()) {
        std::filesystem::path sp(*opt.stop_at);
        sp = sp.lexically_normal();
        stop_at_norm = sp.string();
        if (*stop_at_norm != "/" && !stop_at_norm->empty() && stop_at_norm->back() == '/') {
            stop_at_norm->pop_back();
        }
    }

    std::vector<std::string_view> comps;
    comps.reserve(32);

    // Build trie
    for (const auto& s : filenames) {
        std::string norm = normalize_path_or_empty(s, opt, rep);
        if (norm.empty()) continue;

        std::string_view root_prefix;
        if (!split_components(norm, root_prefix, comps)) {
            rep.skipped_paths++;
            push_error(rep, opt, Error::Op::SkipPath, EINVAL, norm);
            continue;
        }

        Node* cur = get_root(root_prefix);

        // Insert each component
        for (std::size_t i = 0; i < comps.size(); ++i) {
            const std::string_view part = comps[i];

            auto it = cur->children.find(part);
            if (it == cur->children.end()) {
                Node* child = new_node();
                std::pmr::polymorphic_allocator<char> ca(&arena);
                std::pmr::string k(part, ca);
                cur->children.emplace(std::move(k), child);
                cur = child;
            } else {
                cur = it->second;
            }
        }

        // Leaf: request unlink at this path.
        cur->unlink_target = true;
    }

    // Process each root
    for (auto& kv : roots) {
        const std::pmr::string& root_prefix = kv.first;
        Node* root_node = kv.second;

        std::string buf(root_prefix.data(), root_prefix.size());
        if (buf.size() > 1 && buf.back() == '/') buf.pop_back(); // keep "/" as-is

        for (auto it = root_node->children.begin(); it != root_node->children.end();) {
            const std::pmr::string& name = it->first;
            Node* child = it->second;

            std::size_t old_len = buf.size();
            if (!buf.empty() && buf.back() != '/') buf.push_back('/');
            buf.append(name.data(), name.size());

            bool gone = process_node(child, buf, stop_at_norm, rep, opt);

            buf.resize(old_len);

            if (gone) it = root_node->children.erase(it);
            else ++it;
        }
    }

    // All PMR allocations are released here when `arena` is destroyed.
    return rep;
}

} // namespace fast_rm_pmr