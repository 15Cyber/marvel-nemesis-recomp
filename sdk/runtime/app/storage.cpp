// Portable title data. Publish a complete legacy E: copy before Boot can write
// anything in the new directory. The old hdd is deliberately left intact.
#include "app/app.h"
#include "core/log.h"
#include <chrono>

namespace app {
namespace {
namespace fs = std::filesystem;

// No links/special files: migrating saves must not follow a path outside E:.
bool CopyUserData(const fs::path& source, const fs::path& destination, std::string& error) {
    std::error_code ec;
    const auto status = fs::symlink_status(source, ec);
    if (ec || !fs::is_directory(status) || fs::is_symlink(status)) {
        error = "Legacy E: is not an ordinary directory: " + source.string();
        return false;
    }
    const auto source_root = fs::weakly_canonical(source, ec);
    if (ec) { error = ec.message(); return false; }
    fs::create_directories(destination, ec);
    if (ec) { error = ec.message(); return false; }
    uint64_t files = 0;
    fs::recursive_directory_iterator it(source, ec), end;
    while (!ec && it != end) {
        const auto from = it->path();
        const auto item = it->symlink_status(ec);
        if (ec) break;
        const auto resolved = fs::weakly_canonical(from, ec);
        if (ec) break;
        const auto relative = resolved.lexically_relative(source_root);
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..") {
            error = "Legacy save path escapes E: " + from.string(); return false;
        }
        const auto to = destination / from.lexically_relative(source);
        if (fs::is_symlink(item) || (!fs::is_directory(item) && !fs::is_regular_file(item))) {
            error = "Unsupported link/special file in legacy saves: " + from.string();
            return false;
        }
        if (fs::is_directory(item)) fs::create_directories(to, ec);
        else {
            const auto size = fs::file_size(from, ec);
            if (ec) break;
            const auto time = fs::last_write_time(from, ec);
            if (ec) break;
            fs::copy_file(from, to, fs::copy_options::none, ec);
            if (ec) break;
            const auto copied_size = fs::file_size(to, ec);
            if (ec) break;
            const auto current_size = fs::file_size(from, ec);
            if (ec) break;
            const auto current_time = fs::last_write_time(from, ec);
            if (ec) break;
            if (copied_size != size || current_size != size || current_time != time) {
                error = "Legacy saves changed while copying; close other game instances and retry.";
                return false;
            }
            ++files;
        }
        if (!ec) it.increment(ec);
    }
    if (ec) { error = ec.message(); return false; }
    LOGR("storage: copied %llu legacy E: files to staging", static_cast<unsigned long long>(files));
    return true;
}
}

bool PrepareStorage(std::filesystem::path& directory, std::string& error) {
    const std::string suffix = Instance() > 1 ? std::to_string(Instance()) : "";
    const fs::path legacy = ExeDir() / ("hdd" + suffix);
    if (!kGame.data_directory || !*kGame.data_directory) { directory = legacy; return true; }
    const fs::path name(kGame.data_directory);
    if (name.has_parent_path() || name == "." || name == "..") {
        error = "data_directory must name one folder beside the executable."; return false;
    }
    directory = ExeDir() / (name.string() + suffix);
    if (directory == legacy) return true;
    std::error_code ec;
    const bool existing = fs::exists(directory, ec);
    if (ec) { error = ec.message(); return false; }
    if (existing && (!fs::is_directory(directory, ec) || ec)) {
        error = "Storage path is not a directory: " + directory.string(); return false;
    }
    const auto source = legacy / "E";
    const bool old_data = fs::exists(source, ec);
    if (ec) { error = ec.message(); return false; }
    bool empty = false;
    if (existing) {
        empty = fs::is_empty(directory, ec);
        if (ec) { error = ec.message(); return false; }
        if (!empty) {
            // A populated destination is authoritative. Never merge old slots
            // into it or replace a newer save with the preserved legacy copy.
            const bool has_e = fs::is_directory(directory / "E", ec);
            if (ec) { error = ec.message(); return false; }
            if (old_data && !has_e) {
                error = "Existing saves folder has no E directory; refusing to hide legacy saves: " + directory.string();
                return false;
            }
            LOGR("storage: using %s (no legacy files replaced)", directory.string().c_str());
            return true;
        }
    }
    if (!old_data) {
        fs::create_directories(directory, ec);
        if (ec) { error = ec.message(); return false; }
        LOGR("storage: using %s (new data directory)", directory.string().c_str());
        return true;
    }
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto staging = directory.parent_path() / ("." + directory.filename().string() + "-migration-" + std::to_string(stamp));
    if (!CopyUserData(source, staging / "E", error)) {
        LOGW("storage: unpublished migration retained at %s", staging.string().c_str());
        return false;
    }
    // Only an empty destination may be removed, never recursively. If another
    // process has populated it since the check, remove/rename fails safely.
    if (existing && empty) {
        fs::remove(directory, ec);
        if (ec) { error = ec.message(); return false; }
    }
    fs::rename(staging, directory, ec);
    if (ec) { error = "Could not publish saves copy: " + ec.message(); return false; }
    LOGR("storage: migrated %s to %s; original hdd preserved", source.string().c_str(), (directory / "E").string().c_str());
    return true;
}
} // namespace app
