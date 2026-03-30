#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <cerrno>
#include <cstring>
#include <set>
#include <map>
#include <iomanip>
#include <elf.h>
#include <tuple>
#include <atomic>
#include <mutex>
#include <thread>
#include "gpkg_archive.ipp"

// Configuration
std::string g_root_prefix = "";

std::string get_info_dir() {
    return g_root_prefix + "/var/lib/gpkg/info/";
}

std::string get_status_file_path() {
    return g_root_prefix + "/var/lib/gpkg/status";
}

std::string get_conffile_manifest_path(const std::string& pkg_name) {
    return get_info_dir() + pkg_name + ".conffiles";
}

struct PackageStatusRecord {
    std::string package;
    std::string want = "install";
    std::string flag = "ok";
    std::string status = "not-installed";
    std::string version;
};

struct InstallRollbackEntry;
struct InstalledManifestSnapshot {
    bool loaded = false;
    std::vector<std::string> installed_packages;
    std::map<std::string, std::vector<std::string>> file_lists_by_package;
    std::map<std::string, std::string> owner_by_path;
    std::map<std::string, std::string> base_owner_by_path;
};

std::string g_tmp_extract_path;

std::vector<PackageStatusRecord> load_package_status_records();
bool write_package_status_records(const std::vector<PackageStatusRecord>& records);
bool set_package_status_record(
    const std::string& pkg_name,
    const std::string& want,
    const std::string& flag,
    const std::string& status,
    const std::string& version
);
bool erase_package_status_record(const std::string& pkg_name);
bool get_package_status_record(const std::string& pkg_name, PackageStatusRecord* out = nullptr);
bool restore_package_status_snapshot(
    const std::string& pkg_name,
    bool had_record,
    const PackageStatusRecord& record
);

struct PackageStatusRollbackGuard {
    std::string pkg_name;
    bool active = false;
    bool had_record = false;
    PackageStatusRecord record;

    void begin(const std::string& name) {
        pkg_name = name;
        record = PackageStatusRecord{};
        had_record = get_package_status_record(name, &record);
        active = true;
    }

    void commit() {
        active = false;
    }

    ~PackageStatusRollbackGuard() {
        if (!active || pkg_name.empty()) return;
        if (!restore_package_status_snapshot(pkg_name, had_record, record)) {
            std::cerr << "W: Failed to restore package status for " << pkg_name << "." << std::endl;
        }
    }
};

std::string path_parent_dir(const std::string& full_path);
bool mkdir_p(const std::string& path);
bool path_exists_no_follow(const std::string& path);
bool write_text_file_atomic(const std::string& target_path, const std::string& content, mode_t mode = 0644);
bool copy_file_atomic(const std::string& source_path, const std::string& target_path);
bool copy_path_atomic_no_follow(const std::string& source_path, const std::string& target_path);
bool remove_tree_no_follow(const std::string& path);
bool path_is_directory_or_directory_symlink(const std::string& full_path, const struct stat* lstat_result = nullptr);
bool remove_live_path_exact(const std::string& live_full_path);
std::string canonical_existing_path(const std::string& path);
std::string allocate_sibling_temp_path(const std::string& live_full_path, const std::string& tag, int* fd_out = nullptr);
std::string get_package_version(const std::string& pkg_name);
bool validate_elf_file(const std::string& path, off_t size, std::string* error);
bool sync_multiarch_runtime_aliases();
bool ensure_symlink_target_if_possible(
    const std::string& link_path,
    const std::string& target,
    bool replace_non_symlink
);
std::vector<std::string> collect_shadowed_stale_runtime_provider_paths();
std::vector<std::pair<std::string, std::string>> collect_broken_runtime_linker_symlink_repairs();
std::vector<std::string> collect_broken_unowned_runtime_linker_symlink_paths();
std::vector<std::string> read_list_file(const std::string& pkg_name);
std::vector<std::string> get_installed_packages(const std::string& extension = ".list");
std::string path_basename(const std::string& path);
std::string read_symlink_target(const std::string& path);
bool file_list_touches_selinux_policy_store(const std::vector<std::string>& files);
bool restorecon_transaction_paths(const std::vector<std::string>& logical_paths, std::string* error_out = nullptr);
bool finalize_selinux_relabel_for_success(const std::vector<std::string>& logical_paths, std::string* error_out = nullptr);
bool action_refresh_selinux_label_state();
bool schedule_selinux_autorelabel(
    std::vector<InstallRollbackEntry>& rollback_entries,
    std::string* error_out = nullptr
);
bool backup_live_path_if_present(
    const std::string& live_full_path,
    const std::string& logical_path,
    std::vector<InstallRollbackEntry>& rollback_entries,
    bool* had_existing
);
const InstalledManifestSnapshot& ensure_installed_manifest_snapshot();
void invalidate_installed_manifest_snapshot();
std::string find_cached_file_owner(const std::string& pkg_name, const std::string& file_path);
std::string find_cached_base_file_owner(const std::string& file_path);

// Logging
bool g_verbose = false;
size_t g_parallel_jobs = 0;
bool g_defer_runtime_linker_refresh = false;
bool g_defer_selinux_relabel = false;
std::vector<PackageStatusRecord> g_status_records_cache;
bool g_status_records_cache_loaded = false;
#define VLOG(msg) do { if (g_verbose) std::cout << "[WORKER] " << msg << std::endl; } while(0)

// Utils
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if (std::string::npos == first) return str;
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}

bool parse_parallel_jobs_value(const std::string& text, size_t* out) {
    if (out) *out = 0;

    std::string trimmed = trim(text);
    if (trimmed.empty()) return false;

    char* end = nullptr;
    errno = 0;
    unsigned long value = std::strtoul(trimmed.c_str(), &end, 10);
    if (errno != 0 || end == trimmed.c_str()) return false;
    while (end && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (end && *end != '\0') return false;
    if (value == 0) return false;

    if (out) *out = static_cast<size_t>(value);
    return true;
}

size_t detected_parallel_jobs() {
    const char* env_jobs = std::getenv("GPKG_WORKER_JOBS");
    size_t parsed = 0;
    if (env_jobs && parse_parallel_jobs_value(env_jobs, &parsed)) return parsed;

    unsigned int hardware = std::thread::hardware_concurrency();
    if (hardware == 0) return 1;
    return static_cast<size_t>(hardware);
}

size_t parallel_worker_count_for_tasks(size_t task_count) {
    if (task_count == 0) return 1;
    size_t jobs = g_parallel_jobs > 0 ? g_parallel_jobs : detected_parallel_jobs();
    return std::max<size_t>(1, std::min(task_count, jobs));
}

std::vector<PackageStatusRecord> read_package_status_records_from_disk() {
    std::vector<PackageStatusRecord> records;
    std::ifstream f(get_status_file_path());
    if (!f) return records;

    PackageStatusRecord current;
    bool have_content = false;
    std::string line;
    auto flush_record = [&]() {
        if (current.package.empty()) {
            current = PackageStatusRecord{};
            have_content = false;
            return;
        }

        records.push_back(current);
        current = PackageStatusRecord{};
        have_content = false;
    };

    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            flush_record();
            continue;
        }

        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;

        std::string key = trim(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));
        if (key.empty()) continue;

        have_content = true;
        if (key == "Package") {
            current.package = value;
        } else if (key == "Status") {
            std::istringstream iss(value);
            std::string want;
            std::string flag;
            std::string state;
            if (iss >> want >> flag >> state) {
                current.want = want;
                current.flag = flag;
                current.status = state;
            }
        } else if (key == "Version") {
            current.version = value;
        }
    }

    if (have_content) flush_record();
    return records;
}

std::vector<PackageStatusRecord>& mutable_package_status_records() {
    if (!g_status_records_cache_loaded) {
        g_status_records_cache = read_package_status_records_from_disk();
        g_status_records_cache_loaded = true;
    }
    return g_status_records_cache;
}

std::vector<PackageStatusRecord> load_package_status_records() {
    return mutable_package_status_records();
}

bool write_package_status_records(const std::vector<PackageStatusRecord>& records) {
    std::vector<PackageStatusRecord> normalized;
    normalized.reserve(records.size());
    for (const auto& record : records) {
        if (record.package.empty()) continue;
        normalized.push_back(record);
    }

    std::sort(normalized.begin(), normalized.end(), [](const PackageStatusRecord& left, const PackageStatusRecord& right) {
        return left.package < right.package;
    });

    std::ostringstream buffer;
    for (size_t i = 0; i < normalized.size(); ++i) {
        const auto& record = normalized[i];
        buffer << "Package: " << record.package << "\n";
        buffer << "Status: " << (record.want.empty() ? "install" : record.want)
               << " " << (record.flag.empty() ? "ok" : record.flag)
               << " " << (record.status.empty() ? "not-installed" : record.status) << "\n";
        if (!record.version.empty()) buffer << "Version: " << record.version << "\n";
        buffer << "\n";
    }

    if (!mkdir_p(g_root_prefix + "/var/lib/gpkg")) return false;
    if (!write_text_file_atomic(get_status_file_path(), buffer.str(), 0644)) return false;
    g_status_records_cache = normalized;
    g_status_records_cache_loaded = true;
    return true;
}

bool get_package_status_record(const std::string& pkg_name, PackageStatusRecord* out) {
    const auto& records = mutable_package_status_records();
    for (const auto& record : records) {
        if (record.package != pkg_name) continue;
        if (out) *out = record;
        return true;
    }
    return false;
}

bool set_package_status_record(
    const std::string& pkg_name,
    const std::string& want,
    const std::string& flag,
    const std::string& status,
    const std::string& version
) {
    auto& records = mutable_package_status_records();
    for (auto& record : records) {
        if (record.package != pkg_name) continue;
        record.want = want;
        record.flag = flag;
        record.status = status;
        record.version = version;
        return write_package_status_records(records);
    }

    PackageStatusRecord record;
    record.package = pkg_name;
    record.want = want;
    record.flag = flag;
    record.status = status;
    record.version = version;
    records.push_back(record);
    return write_package_status_records(records);
}

bool erase_package_status_record(const std::string& pkg_name) {
    auto& records = mutable_package_status_records();
    size_t original_size = records.size();
    records.erase(
        std::remove_if(records.begin(), records.end(), [&](const PackageStatusRecord& record) {
            return record.package == pkg_name;
        }),
        records.end()
    );
    if (records.size() == original_size) return true;
    return write_package_status_records(records);
}

bool restore_package_status_snapshot(
    const std::string& pkg_name,
    bool had_record,
    const PackageStatusRecord& record
) {
    if (!had_record) return erase_package_status_record(pkg_name);
    return set_package_status_record(pkg_name, record.want, record.flag, record.status, record.version);
}

std::string shell_quote(const std::string& value) {
    if (value.empty()) return "''";

    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') quoted += "'\\''";
        else quoted += c;
    }
    quoted += "'";
    return quoted;
}

int run_command(const std::string& cmd) {
    VLOG("Exec: " << cmd);
    return system(cmd.c_str());
}

int decode_command_exit_status(int status) {
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}

int run_executable(const std::vector<std::string>& argv) {
    if (argv.empty() || argv.front().empty()) return -1;

    std::ostringstream rendered;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i != 0) rendered << " ";
        rendered << shell_quote(argv[i]);
    }
    VLOG("Execv: " << rendered.str());

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& arg : argv) {
            cargv.push_back(const_cast<char*>(arg.c_str()));
        }
        cargv.push_back(nullptr);
        execv(argv.front().c_str(), cargv.data());
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    return status;
}

int run_path_with_args(const std::string& path, const std::vector<std::string>& args = {}) {
    if (path.empty()) return -1;
    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(path);
    argv.insert(argv.end(), args.begin(), args.end());
    return decode_command_exit_status(run_executable(argv));
}

bool refresh_linker_cache_if_available() {
    if (!sync_multiarch_runtime_aliases()) return false;

    auto find_ldconfig_path = []() -> std::string {
        const char* candidates[] = {
            "/sbin/ldconfig",
            "/usr/sbin/ldconfig",
            "/bin/ldconfig",
            "/usr/bin/ldconfig",
        };

        for (const char* candidate : candidates) {
            if (access(candidate, X_OK) == 0) return candidate;
        }
        return "";
    };

    auto run_ldconfig = [&](const std::string& candidate) {
        if (candidate.empty()) return true;
        return run_path_with_args(candidate, g_root_prefix.empty()
            ? std::vector<std::string>{}
            : std::vector<std::string>{"-r", g_root_prefix}) == 0;
    };

    std::string ldconfig_path = find_ldconfig_path();
    if (!run_ldconfig(ldconfig_path)) return false;

    size_t repaired = 0;
    size_t removed = 0;
    size_t failed = 0;
    std::vector<std::pair<std::string, std::string>> broken_linker_repairs =
        collect_broken_runtime_linker_symlink_repairs();
    std::set<std::string> repaired_paths;
    for (const auto& repair : broken_linker_repairs) {
        if (repair.first.empty() || repair.second.empty()) continue;
        if (!ensure_symlink_target_if_possible(repair.first, repair.second, true)) {
            ++failed;
            VLOG("Failed to repair broken runtime linker symlink " << repair.first
                 << " -> " << repair.second);
            continue;
        }

        repaired_paths.insert(repair.first);
        ++repaired;
        VLOG("Repaired broken runtime linker symlink " << repair.first
             << " -> " << repair.second);
    }

    std::vector<std::string> cleanup_paths = collect_shadowed_stale_runtime_provider_paths();
    std::vector<std::string> broken_linker_symlinks =
        collect_broken_unowned_runtime_linker_symlink_paths();
    cleanup_paths.insert(
        cleanup_paths.end(),
        broken_linker_symlinks.begin(),
        broken_linker_symlinks.end()
    );
    if (cleanup_paths.empty()) return true;

    std::set<std::string> seen_cleanup_paths;
    for (const auto& full_path : cleanup_paths) {
        if (!seen_cleanup_paths.insert(full_path).second) continue;
        if (repaired_paths.count(full_path) != 0) continue;
        if (!remove_live_path_exact(full_path)) {
            ++failed;
            VLOG("Failed to prune stale runtime path " << full_path);
            continue;
        }

        ++removed;
        VLOG("Pruned stale runtime path " << full_path);
    }

    if ((repaired > 0 || removed > 0) && !run_ldconfig(ldconfig_path)) return false;
    if (failed > 0) {
        std::cerr << "W: Failed to prune " << failed
                  << " stale runtime path"
                  << (failed == 1 ? "" : "s")
                  << " after linker refresh." << std::endl;
    }
    return true;
}

bool finalize_runtime_linker_state_for_success(bool runtime_sensitive) {
    if (!runtime_sensitive) return true;
    if (g_defer_runtime_linker_refresh) return true;
    return refresh_linker_cache_if_available();
}

struct ScopedEnvOverrides {
    struct SavedEntry {
        std::string name;
        bool had_value = false;
        std::string value;
    };

    std::vector<SavedEntry> saved;

    void set(const std::string& name, const std::string& value) {
        SavedEntry entry;
        entry.name = name;
        const char* current = getenv(name.c_str());
        if (current) {
            entry.had_value = true;
            entry.value = current;
        }
        saved.push_back(entry);
        setenv(name.c_str(), value.c_str(), 1);
    }

    ~ScopedEnvOverrides() {
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            if (it->had_value) setenv(it->name.c_str(), it->value.c_str(), 1);
            else unsetenv(it->name.c_str());
        }
    }
};

bool file_list_contains_kernel_payload(const std::vector<std::string>& files) {
    for (const auto& path : files) {
        if (path.rfind("/boot/kernel-", 0) == 0 ||
            path.rfind("/lib/modules/", 0) == 0) {
            return true;
        }
    }
    return false;
}

std::string kernel_release_from_file_list(const std::vector<std::string>& files) {
    for (const auto& path : files) {
        if (path.rfind("/boot/kernel-", 0) == 0) {
            return path.substr(std::string("/boot/kernel-").size());
        }
    }

    const std::string modules_prefix = "/lib/modules/";
    for (const auto& path : files) {
        if (path.rfind(modules_prefix, 0) != 0) continue;
        std::string suffix = path.substr(modules_prefix.size());
        size_t slash = suffix.find('/');
        if (slash == std::string::npos) return suffix;
        if (slash > 0) return suffix.substr(0, slash);
    }

    return "";
}

std::string kernel_image_path_for_release(const std::string& kernel_release) {
    return kernel_release.empty() ? "" : "/boot/kernel-" + kernel_release;
}

std::string kernel_image_live_path_for_release(const std::string& kernel_release) {
    return g_root_prefix + kernel_image_path_for_release(kernel_release);
}

bool kernel_modules_dir_exists(const std::string& kernel_release) {
    if (kernel_release.empty()) return false;
    struct stat st;
    return stat((g_root_prefix + "/lib/modules/" + kernel_release).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool stage_kernel_boot_symlink_transaction(std::vector<InstallRollbackEntry>& rollback_entries) {
    std::string live_path = g_root_prefix + "/boot/kernel";
    return backup_live_path_if_present(live_path, "/boot/kernel", rollback_entries, nullptr);
}

bool sync_kernel_boot_symlink() {
    std::string boot_dir = g_root_prefix + "/boot";
    if (!mkdir_p(boot_dir)) return false;

    DIR* dir = opendir(boot_dir.c_str());
    if (!dir) {
        std::cerr << "E: Failed to inspect " << boot_dir << ": " << strerror(errno) << std::endl;
        return false;
    }

    std::vector<std::string> kernels;
    errno = 0;
    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name.rfind("kernel-", 0) != 0) continue;
        std::string full_path = boot_dir + "/" + name;
        struct stat st;
        if (lstat(full_path.c_str(), &st) != 0) continue;
        if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) continue;
        kernels.push_back(name);
    }
    int scan_errno = errno;
    closedir(dir);
    if (scan_errno != 0) {
        std::cerr << "E: Failed while scanning " << boot_dir << ": " << strerror(scan_errno) << std::endl;
        return false;
    }

    std::string live_link = boot_dir + "/kernel";
    if (kernels.empty()) {
        struct stat st;
        if (lstat(live_link.c_str(), &st) == 0) {
            if (!remove_live_path_exact(live_link)) {
                std::cerr << "E: Failed to clear stale /boot/kernel entry." << std::endl;
                return false;
            }
            VLOG("Removed stale /boot/kernel symlink.");
        }
        return true;
    }

    std::sort(kernels.begin(), kernels.end(), [](const std::string& left, const std::string& right) {
        return ::strverscmp(left.c_str(), right.c_str()) < 0;
    });
    const std::string& best = kernels.back();

    std::string current_target;
    struct stat current_st;
    if (lstat(live_link.c_str(), &current_st) == 0 && S_ISLNK(current_st.st_mode)) {
        current_target = read_symlink_target(live_link);
        if (current_target == best) return true;
    }

    std::string temp_link = allocate_sibling_temp_path(live_link, "kernel-link");
    if (temp_link.empty()) {
        std::cerr << "E: Failed to allocate temporary kernel symlink path." << std::endl;
        return false;
    }
    unlink(temp_link.c_str());
    if (symlink(best.c_str(), temp_link.c_str()) != 0) {
        std::cerr << "E: Failed to create temporary /boot/kernel symlink: "
                  << strerror(errno) << std::endl;
        unlink(temp_link.c_str());
        return false;
    }
    if (rename(temp_link.c_str(), live_link.c_str()) != 0) {
        std::cerr << "E: Failed to update /boot/kernel symlink: " << strerror(errno) << std::endl;
        unlink(temp_link.c_str());
        return false;
    }

    VLOG("Updated /boot/kernel -> " << best);
    return true;
}

bool run_depmod_for_kernel_release(const std::string& kernel_release, bool full_rebuild = false) {
    if (kernel_release.empty()) return true;
    if (!full_rebuild && !kernel_modules_dir_exists(kernel_release)) return true;

    const char* candidates[] = {
        "/sbin/depmod",
        "/usr/sbin/depmod",
        "/bin/depmod",
        "/usr/bin/depmod",
    };

    for (const char* candidate : candidates) {
        if (access(candidate, X_OK) != 0) continue;
        std::vector<std::string> args = {candidate};
        if (!g_root_prefix.empty()) {
            args.push_back("-b");
            args.push_back(g_root_prefix);
        }
        if (full_rebuild) {
            args.push_back("-a");
        } else {
            args.push_back(kernel_release);
        }
        return decode_command_exit_status(run_executable(args)) == 0;
    }

    return true;
}

std::vector<std::string> list_kernel_hook_scripts(const std::string& hook_dir) {
    std::vector<std::string> scripts;
    DIR* dir = opendir(hook_dir.c_str());
    if (!dir) return scripts;

    errno = 0;
    while (dirent* entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name.empty() || name == "." || name == ".." || name[0] == '.') continue;
        std::string full_path = hook_dir + "/" + name;
        struct stat st;
        if (lstat(full_path.c_str(), &st) != 0) continue;
        if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) continue;
        if (access(full_path.c_str(), X_OK) != 0) continue;
        scripts.push_back(full_path);
    }
    closedir(dir);
    std::sort(scripts.begin(), scripts.end());
    return scripts;
}

bool run_kernel_hook_directories(
    const std::string& hook_name,
    const std::string& kernel_release,
    const std::string& image_path,
    const std::vector<std::string>& maint_args
) {
    if (hook_name.empty() || kernel_release.empty() || image_path.empty()) return true;
    if (!g_root_prefix.empty()) {
        VLOG("Skipping kernel hook directories in --root mode for " << hook_name << ".");
        return true;
    }

    std::vector<std::string> hook_dirs = {
        "/etc/kernel/" + hook_name + ".d",
        "/usr/share/kernel/" + hook_name + ".d",
    };

    std::vector<std::string> scripts;
    for (const auto& dir : hook_dirs) {
        auto dir_scripts = list_kernel_hook_scripts(dir);
        scripts.insert(scripts.end(), dir_scripts.begin(), dir_scripts.end());
    }

    if (scripts.empty()) return true;

    std::ostringstream maint_params;
    for (size_t i = 0; i < maint_args.size(); ++i) {
        if (i != 0) maint_params << " ";
        maint_params << shell_quote(maint_args[i]);
    }

    ScopedEnvOverrides env;
    env.set("DEB_MAINT_PARAMS", maint_params.str());
    env.set("INITRD", "No");

    for (const auto& script : scripts) {
        VLOG("Running kernel " << hook_name << " hook: " << script);
        if (run_path_with_args(script, {kernel_release, image_path}) != 0) {
            std::cerr << "E: Kernel " << hook_name << " hook failed: " << script << std::endl;
            return false;
        }
    }

    return true;
}

bool is_multiarch_runtime_alias_candidate(const std::string& name) {
    return (name.rfind("lib", 0) == 0 && name.find(".so.") != std::string::npos) ||
           name.rfind("ld-linux-", 0) == 0;
}

std::string read_symlink_target(const std::string& path) {
    char buffer[4096];
    ssize_t len = readlink(path.c_str(), buffer, sizeof(buffer) - 1);
    if (len < 0) return "";
    buffer[len] = '\0';
    return std::string(buffer);
}

std::vector<std::string> split_path_components(const std::string& path) {
    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (part.empty() || part == ".") continue;
        parts.push_back(part);
    }
    return parts;
}

std::string relative_symlink_target(const std::string& link_path, const std::string& target_path) {
    std::vector<std::string> from_parts = split_path_components(path_parent_dir(link_path));
    std::vector<std::string> to_parts = split_path_components(target_path);

    size_t common = 0;
    while (common < from_parts.size() &&
           common < to_parts.size() &&
           from_parts[common] == to_parts[common]) {
        ++common;
    }

    std::string relative;
    for (size_t i = common; i < from_parts.size(); ++i) {
        if (!relative.empty()) relative += "/";
        relative += "..";
    }
    for (size_t i = common; i < to_parts.size(); ++i) {
        if (!relative.empty()) relative += "/";
        relative += to_parts[i];
    }

    return relative.empty() ? "." : relative;
}

std::string resolved_directory_path(const std::string& path) {
    char resolved[4096];
    if (!realpath(path.c_str(), resolved)) return "";

    struct stat st;
    if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode)) return "";
    return std::string(resolved);
}

std::string stable_runtime_alias_target(
    const std::string& link_path,
    const std::string& target_path
) {
    std::string resolved_link_parent = resolved_directory_path(path_parent_dir(link_path));
    std::string resolved_target_parent = resolved_directory_path(path_parent_dir(target_path));
    if (resolved_link_parent.empty() || resolved_target_parent.empty()) {
        return relative_symlink_target(link_path, target_path);
    }

    size_t slash = target_path.find_last_of('/');
    std::string basename = slash == std::string::npos ? target_path : target_path.substr(slash + 1);
    std::string physical_target = resolved_target_parent + "/" + basename;
    return relative_symlink_target(resolved_link_parent + "/.gpkg-link", physical_target);
}

bool ensure_symlink_target_if_possible(
    const std::string& link_path,
    const std::string& target,
    bool replace_non_symlink = false
) {
    struct stat st;
    if (lstat(link_path.c_str(), &st) == 0) {
        if (!S_ISLNK(st.st_mode) && !replace_non_symlink) return true;
        if (S_ISDIR(st.st_mode)) return false;
        if (read_symlink_target(link_path) == target) return true;
        if (unlink(link_path.c_str()) != 0) return false;
    } else if (errno != ENOENT) {
        return false;
    }

    if (!mkdir_p(path_parent_dir(link_path))) return false;
    return symlink(target.c_str(), link_path.c_str()) == 0;
}

bool runtime_alias_paths_equivalent(
    const std::string& active_path,
    const std::string& compat_path
) {
    if (!path_exists_no_follow(active_path) || !path_exists_no_follow(compat_path)) return false;
    std::string active_real = canonical_existing_path(active_path);
    std::string compat_real = canonical_existing_path(compat_path);
    return !active_real.empty() && active_real == compat_real;
}

struct RuntimeAliasFamily {
    const char* canonical_prefix;
    const char* compat_prefix;
    const char* legacy_compat_prefix;
};

const RuntimeAliasFamily k_runtime_alias_families[] = {
    {"/lib/x86_64-linux-gnu", "/lib64", "/lib64/x86_64-linux-gnu"},
    {"/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib64/x86_64-linux-gnu"},
};

struct MultiarchLogicalPrefixMap {
    const char* path_prefix;
    const char* canonical_prefix;
};

const MultiarchLogicalPrefixMap k_multiarch_logical_prefix_maps[] = {
    {"/lib64/x86_64-linux-gnu", "/lib/x86_64-linux-gnu"},
    {"/lib64", "/lib/x86_64-linux-gnu"},
    {"/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu"},
    {"/usr/lib64/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu"},
    {"/usr/lib64", "/usr/lib/x86_64-linux-gnu"},
    {"/usr/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu"},
};

std::string canonical_multiarch_logical_path(const std::string& path) {
    for (const auto& map : k_multiarch_logical_prefix_maps) {
        std::string prefix = map.path_prefix;
        if (path == prefix) return map.canonical_prefix;
        if (path.rfind(prefix + "/", 0) != 0) continue;
        return std::string(map.canonical_prefix) + path.substr(prefix.size());
    }
    return path;
}

std::vector<std::string> normalize_owned_manifest_paths(const std::vector<std::string>& paths) {
    std::vector<std::string> normalized;
    std::set<std::string> seen;
    for (const auto& path : paths) {
        std::string canonical_path = canonical_multiarch_logical_path(path);
        if (!seen.insert(canonical_path).second) continue;
        normalized.push_back(canonical_path);
    }
    return normalized;
}

std::set<std::string> build_normalized_owned_path_set(const std::vector<std::string>& paths) {
    std::set<std::string> normalized;
    for (const auto& path : paths) {
        normalized.insert(canonical_multiarch_logical_path(path));
    }
    return normalized;
}

std::vector<std::string> collapse_multiarch_install_alias_paths(const std::vector<std::string>& paths) {
    std::set<std::string> raw_paths(paths.begin(), paths.end());
    std::vector<std::string> collapsed;
    std::set<std::string> seen;

    for (const auto& path : paths) {
        std::string canonical_path = canonical_multiarch_logical_path(path);
        std::string selected_path = path;

        // If the payload already carries the canonical multiarch path, install and own
        // that single logical entry. GeminiOS already exposes the compat prefixes as
        // directory symlinks into the canonical tree, so keeping both payload aliases
        // would overwrite the same on-disk object multiple times.
        if (canonical_path != path && raw_paths.count(canonical_path) != 0) {
            selected_path = canonical_path;
        }

        if (!seen.insert(selected_path).second) continue;
        collapsed.push_back(selected_path);
    }

    return collapsed;
}

std::vector<std::string> runtime_alias_family_prefixes(const RuntimeAliasFamily& family) {
    return {family.canonical_prefix, family.compat_prefix, family.legacy_compat_prefix};
}

bool runtime_alias_managed_prefix(const std::string& path) {
    static const char* prefixes[] = {
        "/lib/x86_64-linux-gnu/",
        "/lib64/",
        "/lib64/x86_64-linux-gnu/",
        "/usr/lib/x86_64-linux-gnu/",
        "/usr/lib64/",
        "/usr/lib64/x86_64-linux-gnu/",
    };

    for (const char* prefix : prefixes) {
        if (path.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

int runtime_alias_path_rank(const std::string& path) {
    if (path.rfind("/lib/x86_64-linux-gnu/", 0) == 0) return 0;
    if (path.rfind("/usr/lib/x86_64-linux-gnu/", 0) == 0) return 1;
    if (path.rfind("/lib64/", 0) == 0 && path.find("/x86_64-linux-gnu/", 0) == std::string::npos) return 2;
    if (path.rfind("/usr/lib64/", 0) == 0 && path.find("/x86_64-linux-gnu/", 0) == std::string::npos) return 3;
    if (path.rfind("/lib64/x86_64-linux-gnu/", 0) == 0) return 4;
    if (path.rfind("/usr/lib64/x86_64-linux-gnu/", 0) == 0) return 5;
    return 100;
}

std::map<std::string, std::string> build_runtime_file_owner_index() {
    std::map<std::string, std::string> owner_index;
    const auto& snapshot = ensure_installed_manifest_snapshot();
    for (const auto& entry : snapshot.owner_by_path) {
        if (!runtime_alias_managed_prefix(entry.first)) continue;
        if (!is_multiarch_runtime_alias_candidate(path_basename(entry.first))) continue;
        owner_index.emplace(entry.first, entry.second);
    }
    return owner_index;
}

bool runtime_path_resolves_to_valid_library(
    const std::string& full_path,
    std::string* resolved_path_out = nullptr,
    std::string* error_out = nullptr
) {
    if (resolved_path_out) resolved_path_out->clear();
    if (error_out) error_out->clear();

    struct stat st;
    if (lstat(full_path.c_str(), &st) != 0) {
        if (error_out) *error_out = strerror(errno);
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        if (error_out) *error_out = "path is a directory";
        return false;
    }

    std::string resolved = canonical_existing_path(full_path);
    if (resolved.empty()) {
        if (error_out) *error_out = "path does not resolve to an existing file";
        return false;
    }

    struct stat resolved_st;
    if (stat(resolved.c_str(), &resolved_st) != 0) {
        if (error_out) *error_out = strerror(errno);
        return false;
    }
    if (!S_ISREG(resolved_st.st_mode)) {
        if (error_out) *error_out = "resolved path is not a regular file";
        return false;
    }

    std::string elf_error;
    if (!validate_elf_file(resolved, resolved_st.st_size, &elf_error)) {
        if (error_out) *error_out = elf_error;
        return false;
    }

    if (resolved_path_out) *resolved_path_out = resolved;
    return true;
}

void collect_runtime_alias_names_for_prefix(
    const std::string& live_prefix,
    std::set<std::string>& names
) {
    std::string live_dir = g_root_prefix + live_prefix;
    DIR* dir = opendir(live_dir.c_str());
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == ".." || !is_multiarch_runtime_alias_candidate(name)) continue;

        std::string full_path = live_dir + "/" + name;
        struct stat st;
        if (lstat(full_path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) continue;
        names.insert(name);
    }

    closedir(dir);
}

std::string select_global_runtime_alias_canonical_path(
    const std::string& name,
    const std::map<std::string, std::string>& owner_index
) {
    struct Candidate {
        std::string path;
        bool owned = false;
        int rank = 100;
    };

    std::vector<Candidate> candidates;
    for (const auto& family : k_runtime_alias_families) {
        for (const auto& prefix : runtime_alias_family_prefixes(family)) {
            std::string logical_path = prefix + "/" + name;
            std::string full_path = g_root_prefix + logical_path;
            if (!path_exists_no_follow(full_path)) continue;
            if (!runtime_path_resolves_to_valid_library(full_path)) continue;

            Candidate candidate;
            candidate.path = full_path;
            candidate.owned = owner_index.count(logical_path) != 0;
            candidate.rank = runtime_alias_path_rank(logical_path);
            candidates.push_back(candidate);
        }
    }

    if (candidates.empty()) return "";

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& left, const Candidate& right) {
        if (left.owned != right.owned) return left.owned && !right.owned;
        if (left.rank != right.rank) return left.rank < right.rank;
        return left.path < right.path;
    });
    return candidates.front().path;
}

std::string runtime_canonical_link_name(const std::string& name) {
    size_t so_pos = name.find(".so");
    if (so_pos == std::string::npos) return name;

    size_t version_start = so_pos + 3;
    if (version_start >= name.size() || name[version_start] != '.') return name;

    size_t next_dot = name.find('.', version_start + 1);
    if (next_dot == std::string::npos) return name;
    return name.substr(0, next_dot);
}

bool discard_invalid_runtime_entries(
    const RuntimeAliasFamily& family,
    const std::string& name,
    const std::string& canonical_hint
) {
    bool ok = true;
    for (const auto& prefix : runtime_alias_family_prefixes(family)) {
        std::string full_path = g_root_prefix + prefix + "/" + name;
        if (!path_exists_no_follow(full_path)) continue;
        if (runtime_path_resolves_to_valid_library(full_path)) continue;

        if (!remove_live_path_exact(full_path)) {
            VLOG("Failed to discard stale invalid runtime entry " << full_path
                 << (canonical_hint.empty() ? "" : " while converging on " + canonical_hint));
            ok = false;
            continue;
        }

        VLOG("Discarded stale invalid runtime entry " << full_path
             << (canonical_hint.empty() ? "" : " while converging on " + canonical_hint));
    }

    return ok;
}

bool reconcile_runtime_alias_family(
    const RuntimeAliasFamily& family,
    const std::string& name,
    const std::string& canonical_source,
    const std::string& canonical_link_source
) {
    if (canonical_source.empty()) {
        return discard_invalid_runtime_entries(family, name, canonical_link_source);
    }

    bool ok = true;
    for (const auto& prefix : runtime_alias_family_prefixes(family)) {
        std::string live_path = g_root_prefix + prefix + "/" + name;
        if (live_path == canonical_source) continue;

        if (path_exists_no_follow(live_path) &&
            runtime_path_resolves_to_valid_library(live_path) &&
            runtime_alias_paths_equivalent(canonical_source, live_path)) {
            continue;
        }

        std::string alias_target = stable_runtime_alias_target(live_path, canonical_source);
        bool replace_non_symlink = path_exists_no_follow(live_path);
        if (!ensure_symlink_target_if_possible(live_path, alias_target, replace_non_symlink)) {
            VLOG("Failed to reconcile runtime alias " << live_path
                 << " -> " << canonical_source);
            ok = false;
        } else if (replace_non_symlink || !runtime_alias_paths_equivalent(canonical_source, live_path)) {
            VLOG("Reconciled runtime alias " << live_path
                 << " -> " << canonical_source);
        }
    }

    return ok;
}

void sync_multiarch_runtime_aliases_for_prefix(
    const std::string& active_live_prefix,
    const std::string& compat_live_prefix
) {
    std::string active_dir = g_root_prefix + active_live_prefix;
    std::string compat_dir = g_root_prefix + compat_live_prefix;
    if (!mkdir_p(active_dir) || !mkdir_p(compat_dir)) return;

    DIR* active = opendir(active_dir.c_str());
    if (active) {
        struct dirent* entry;
        while ((entry = readdir(active)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == ".." || !is_multiarch_runtime_alias_candidate(name)) continue;

            std::string active_path = active_dir + "/" + name;
            struct stat st;
            if (lstat(active_path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) continue;

            std::string compat_path = compat_dir + "/" + name;
            if (runtime_alias_paths_equivalent(active_path, compat_path)) continue;

            std::string compat_target = stable_runtime_alias_target(compat_path, active_path);
            bool replace_non_symlink = path_exists_no_follow(compat_path);
            if (!ensure_symlink_target_if_possible(compat_path, compat_target, replace_non_symlink)) {
                VLOG("Failed to refresh multiarch compat alias for " << compat_path);
            }
        }
        closedir(active);
    }

    DIR* compat = opendir(compat_dir.c_str());
    if (compat) {
        struct dirent* entry;
        while ((entry = readdir(compat)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == ".." || !is_multiarch_runtime_alias_candidate(name)) continue;

            std::string compat_path = compat_dir + "/" + name;
            struct stat st;
            if (lstat(compat_path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) continue;

            std::string active_path = active_dir + "/" + name;
            if (!path_exists_no_follow(active_path)) {
                std::string active_target = stable_runtime_alias_target(active_path, compat_path);
                if (!ensure_symlink_target_if_possible(active_path, active_target)) {
                    VLOG("Failed to backfill active runtime alias for " << active_path);
                }
            }
        }
        closedir(compat);
    }
}

bool sync_multiarch_runtime_aliases() {
    std::map<std::string, std::string> owner_index = build_runtime_file_owner_index();
    std::set<std::string> family_names[sizeof(k_runtime_alias_families) / sizeof(k_runtime_alias_families[0])];
    for (size_t i = 0; i < sizeof(k_runtime_alias_families) / sizeof(k_runtime_alias_families[0]); ++i) {
        for (const auto& prefix : runtime_alias_family_prefixes(k_runtime_alias_families[i])) {
            collect_runtime_alias_names_for_prefix(prefix, family_names[i]);
        }
    }

    bool ok = true;
    std::set<std::string> all_names = family_names[0];
    all_names.insert(family_names[1].begin(), family_names[1].end());
    for (const auto& name : all_names) {
        std::string canonical_source = select_global_runtime_alias_canonical_path(name, owner_index);
        std::string link_name = runtime_canonical_link_name(name);
        std::string canonical_link_source = link_name == name
            ? canonical_source
            : select_global_runtime_alias_canonical_path(link_name, owner_index);

        for (size_t i = 0; i < sizeof(k_runtime_alias_families) / sizeof(k_runtime_alias_families[0]); ++i) {
            if (family_names[i].count(name) == 0) continue;

            if (!reconcile_runtime_alias_family(
                    k_runtime_alias_families[i],
                    name,
                    canonical_source,
                    canonical_link_source)) {
                ok = false;
            }
        }
    }

    return ok;
}

std::vector<std::string> collect_shadowed_stale_runtime_provider_paths() {
    struct RuntimeProviderCandidate {
        std::string logical_path;
        std::string full_path;
        std::string resolved_path;
        bool owned = false;
        int rank = 100;
    };

    std::vector<std::string> stale_paths;
    std::set<std::string> seen;
    std::map<std::string, std::string> owner_index = build_runtime_file_owner_index();
    std::map<std::string, std::vector<RuntimeProviderCandidate>> candidates_by_link_name;

    for (const auto& family : k_runtime_alias_families) {
        std::string dir_path = g_root_prefix + family.canonical_prefix;
        DIR* dir = opendir(dir_path.c_str());
        if (!dir) continue;

        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            if (!is_multiarch_runtime_alias_candidate(name)) continue;

            std::string link_name = runtime_canonical_link_name(name);
            if (link_name == name) continue;

            std::string logical_path = canonical_multiarch_logical_path(
                std::string(family.canonical_prefix) + "/" + name);
            if (!seen.insert(logical_path).second) continue;

            std::string full_path = g_root_prefix + logical_path;
            std::string resolved_path;
            if (!runtime_path_resolves_to_valid_library(full_path, &resolved_path)) continue;

            RuntimeProviderCandidate candidate;
            candidate.logical_path = logical_path;
            candidate.full_path = full_path;
            candidate.resolved_path = resolved_path;
            candidate.owned = owner_index.count(logical_path) != 0;
            candidate.rank = runtime_alias_path_rank(logical_path);
            candidates_by_link_name[link_name].push_back(candidate);
        }

        closedir(dir);
    }

    for (const auto& entry : candidates_by_link_name) {
        const auto& candidates = entry.second;
        bool have_owned_candidate = false;
        for (const auto& candidate : candidates) {
            if (!candidate.owned) continue;
            have_owned_candidate = true;
            break;
        }
        if (!have_owned_candidate) continue;

        auto preferred = std::min_element(
            candidates.begin(),
            candidates.end(),
            [](const RuntimeProviderCandidate& left, const RuntimeProviderCandidate& right) {
                if (left.owned != right.owned) return left.owned && !right.owned;
                if (left.rank != right.rank) return left.rank < right.rank;
                return left.logical_path < right.logical_path;
            });
        if (preferred == candidates.end() || !preferred->owned) continue;

        for (const auto& candidate : candidates) {
            if (candidate.owned) continue;
            if (candidate.resolved_path == preferred->resolved_path) continue;
            stale_paths.push_back(candidate.full_path);
        }
    }

    return stale_paths;
}

bool looks_like_unversioned_runtime_linker_name(const std::string& name) {
    return name.rfind("lib", 0) == 0 &&
           name.size() > 3 &&
           name.compare(name.size() - 3, 3, ".so") == 0;
}

int runtime_linker_provider_name_rank(const std::string& name) {
    size_t so_pos = name.find(".so.");
    if (so_pos == std::string::npos) return 100;

    std::string suffix = name.substr(so_pos + 4);
    int dot_count = static_cast<int>(std::count(suffix.begin(), suffix.end(), '.'));
    return dot_count;
}

std::vector<std::pair<std::string, std::string>> collect_broken_runtime_linker_symlink_repairs() {
    struct ProviderCandidate {
        std::string logical_path;
        std::string full_path;
        bool owned = false;
        int path_rank = 100;
        int name_rank = 100;
    };

    std::vector<std::pair<std::string, std::string>> repairs;
    std::set<std::string> seen;
    std::map<std::string, std::string> owner_index = build_runtime_file_owner_index();

    for (const auto& family : k_runtime_alias_families) {
        for (const auto& prefix : runtime_alias_family_prefixes(family)) {
            std::string dir_path = g_root_prefix + prefix;
            DIR* dir = opendir(dir_path.c_str());
            if (!dir) continue;

            struct dirent* entry = nullptr;
            while ((entry = readdir(dir)) != nullptr) {
                std::string name = entry->d_name;
                if (name == "." || name == "..") continue;
                if (!looks_like_unversioned_runtime_linker_name(name)) continue;

                std::string full_path = dir_path + "/" + name;
                struct stat link_st {};
                if (lstat(full_path.c_str(), &link_st) != 0 || !S_ISLNK(link_st.st_mode)) continue;

                struct stat target_st {};
                if (stat(full_path.c_str(), &target_st) == 0) continue;
                if (!seen.insert(full_path).second) continue;

                std::vector<ProviderCandidate> candidates;
                for (const auto& candidate_prefix : runtime_alias_family_prefixes(family)) {
                    std::string candidate_dir = g_root_prefix + candidate_prefix;
                    DIR* candidate_dp = opendir(candidate_dir.c_str());
                    if (!candidate_dp) continue;

                    struct dirent* candidate_entry = nullptr;
                    while ((candidate_entry = readdir(candidate_dp)) != nullptr) {
                        std::string candidate_name = candidate_entry->d_name;
                        if (candidate_name == "." || candidate_name == "..") continue;
                        if (candidate_name.rfind(name + ".", 0) != 0) continue;
                        if (!is_multiarch_runtime_alias_candidate(candidate_name)) continue;

                        ProviderCandidate candidate;
                        candidate.logical_path = canonical_multiarch_logical_path(
                            candidate_prefix + "/" + candidate_name);
                        candidate.full_path = g_root_prefix + candidate.logical_path;

                        std::string resolved_path;
                        if (!runtime_path_resolves_to_valid_library(candidate.full_path, &resolved_path)) continue;
                        candidate.owned = owner_index.count(candidate.logical_path) != 0;
                        candidate.path_rank = runtime_alias_path_rank(candidate.logical_path);
                        candidate.name_rank = runtime_linker_provider_name_rank(candidate_name);
                        candidates.push_back(candidate);
                    }

                    closedir(candidate_dp);
                }

                if (candidates.empty()) continue;

                auto preferred = std::min_element(
                    candidates.begin(),
                    candidates.end(),
                    [](const ProviderCandidate& left, const ProviderCandidate& right) {
                        if (left.owned != right.owned) return left.owned && !right.owned;
                        if (left.name_rank != right.name_rank) return left.name_rank < right.name_rank;
                        if (left.path_rank != right.path_rank) return left.path_rank < right.path_rank;
                        return left.logical_path < right.logical_path;
                    });
                if (preferred != candidates.end()) {
                    repairs.emplace_back(
                        full_path,
                        stable_runtime_alias_target(full_path, preferred->full_path)
                    );
                }
            }

            closedir(dir);
        }
    }

    return repairs;
}

std::vector<std::string> collect_broken_unowned_runtime_linker_symlink_paths() {
    std::vector<std::string> broken_paths;
    std::set<std::string> seen;

    for (const auto& family : k_runtime_alias_families) {
        std::string dir_path = g_root_prefix + family.canonical_prefix;
        DIR* dir = opendir(dir_path.c_str());
        if (!dir) continue;

        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            if (!looks_like_unversioned_runtime_linker_name(name)) continue;

            std::string logical_path = canonical_multiarch_logical_path(
                std::string(family.canonical_prefix) + "/" + name);
            if (!seen.insert(logical_path).second) continue;

            std::string full_path = g_root_prefix + logical_path;
            struct stat link_st {};
            if (lstat(full_path.c_str(), &link_st) != 0 || !S_ISLNK(link_st.st_mode)) continue;

            struct stat target_st {};
            if (stat(full_path.c_str(), &target_st) == 0) continue;
            if (find_cached_file_owner("", logical_path).empty()) {
                broken_paths.push_back(full_path);
            }
        }

        closedir(dir);
    }

    return broken_paths;
}

std::string canonical_existing_path(const std::string& path) {
    char resolved[4096];
    if (!realpath(path.c_str(), resolved)) return "";
    return std::string(resolved);
}

std::string normalize_logical_absolute_path(const std::string& path) {
    if (path.empty() || path[0] != '/') return "";

    std::vector<std::string> parts;
    std::stringstream ss(path);
    std::string part;
    while (std::getline(ss, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }

    std::string normalized = "/";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) normalized += "/";
        normalized += parts[i];
    }
    return normalized;
}

std::string resolve_logical_symlink_target(
    const std::string& link_path,
    const std::string& symlink_target
) {
    if (symlink_target.empty()) return "";
    if (symlink_target[0] == '/') {
        return normalize_logical_absolute_path(symlink_target);
    }

    std::string base = link_path;
    size_t slash = base.find_last_of('/');
    if (slash == std::string::npos) return "";
    std::string combined = base.substr(0, slash + 1) + symlink_target;
    return normalize_logical_absolute_path(combined);
}

bool runtime_alias_pair_for_path(
    const std::string& path,
    std::string* active_prefix_out,
    std::string* compat_prefix_out,
    std::string* name_out
) {
    struct PrefixPair {
        const char* path_prefix;
        const char* active_prefix;
        const char* compat_prefix;
    };

    const PrefixPair pairs[] = {
        {"/lib/x86_64-linux-gnu/", "/lib/x86_64-linux-gnu", "/lib64"},
        {"/lib64/x86_64-linux-gnu/", "/lib/x86_64-linux-gnu", "/lib64"},
        {"/lib64/", "/lib/x86_64-linux-gnu", "/lib64"},
        {"/usr/lib/x86_64-linux-gnu/", "/usr/lib/x86_64-linux-gnu", "/usr/lib64"},
        {"/usr/lib64/x86_64-linux-gnu/", "/usr/lib/x86_64-linux-gnu", "/usr/lib64"},
        {"/usr/lib64/", "/usr/lib/x86_64-linux-gnu", "/usr/lib64"},
    };

    for (const auto& pair : pairs) {
        std::string prefix = pair.path_prefix;
        if (path.rfind(prefix, 0) != 0) continue;

        std::string remainder = path.substr(prefix.size());
        if (remainder.empty() || remainder.find('/') != std::string::npos) continue;
        if (!is_multiarch_runtime_alias_candidate(remainder)) return false;

        if (active_prefix_out) *active_prefix_out = pair.active_prefix;
        if (compat_prefix_out) *compat_prefix_out = pair.compat_prefix;
        if (name_out) *name_out = remainder;
        return true;
    }

    return false;
}

std::string canonical_runtime_logical_path(const std::string& path) {
    return canonical_multiarch_logical_path(path);
}

bool runtime_symlink_target_equivalent(
    const std::string& logical_path,
    const std::string& expected_target,
    const std::string& actual_target
) {
    if (expected_target == actual_target) return true;

    std::string expected_resolved = resolve_logical_symlink_target(logical_path, expected_target);
    std::string actual_resolved = resolve_logical_symlink_target(logical_path, actual_target);
    if (expected_resolved.empty() || actual_resolved.empty()) return false;

    expected_resolved = canonical_runtime_logical_path(expected_resolved);
    actual_resolved = canonical_runtime_logical_path(actual_resolved);
    return expected_resolved == actual_resolved;
}

bool mkdir_p(const std::string& path) {
    if (path.empty()) return false;
    if (path == "/" || path == ".") return true;

    std::string current;
    if (!path.empty() && path[0] == '/') current = "/";
    std::vector<std::string> parts = split_path_components(path);
    for (const auto& part : parts) {
        if (current.empty() || current == "/") current += part;
        else current += "/" + part;

        struct stat st;
        if (lstat(current.c_str(), &st) == 0) {
            if (!path_is_directory_or_directory_symlink(current, &st)) {
                errno = ENOTDIR;
                return false;
            }
            continue;
        }
        if (errno != ENOENT) return false;
        if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }

    return true;
}

bool path_exists_no_follow(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

bool remove_tree_no_follow(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;

    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        return unlink(path.c_str()) == 0 || errno == ENOENT;
    }

    DIR* dir = opendir(path.c_str());
    if (!dir) return false;

    bool ok = true;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (!remove_tree_no_follow(path + "/" + name)) {
            ok = false;
            break;
        }
    }
    int saved_errno = errno;
    closedir(dir);
    if (!ok) {
        errno = saved_errno;
        return false;
    }

    return rmdir(path.c_str()) == 0 || errno == ENOENT;
}

bool create_extract_workspace() {
    char path_template[] = "/tmp/gpkg_worker_extract-XXXXXX";
    char* created = mkdtemp(path_template);
    if (!created) return false;
    g_tmp_extract_path = std::string(created) + "/";
    return true;
}

void cleanup_extract_workspace() {
    if (g_tmp_extract_path.empty()) return;
    remove_tree_no_follow(g_tmp_extract_path);
    g_tmp_extract_path.clear();
}

struct ScopedExtractWorkspace {
    bool active = false;

    ~ScopedExtractWorkspace() {
        if (active) cleanup_extract_workspace();
    }
};

// --- Database (List File) Management ---

std::vector<std::string> read_list_file_from_disk(const std::string& pkg_name) {
    std::vector<std::string> files;
    std::string path = get_info_dir() + pkg_name + ".list";
    std::ifstream f(path);
    if (!f) return files;
    
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (!line.empty()) files.push_back(line);
    }
    return files;
}

// Get list of installed package names from INFO_DIR
std::vector<std::string> get_installed_packages_from_disk(const std::string& extension) {
    std::vector<std::string> pkgs;
    DIR* d = opendir(get_info_dir().c_str());
    if (!d) return pkgs;
    
    struct dirent* dir;
    while ((dir = readdir(d)) != NULL) {
        std::string fname = dir->d_name;
        if (fname.size() > extension.size() && 
            fname.substr(fname.size() - extension.size()) == extension) {
            std::string pkg_name = fname.substr(0, fname.size() - extension.size());
            if (pkg_name.size() >= 14 &&
                pkg_name.substr(pkg_name.size() - 14) == ".system-backup") {
                continue;
            }
            pkgs.push_back(pkg_name);
        }
    }
    closedir(d);
    return pkgs;
}

InstalledManifestSnapshot g_installed_manifest_snapshot;

std::string get_base_system_registry_path() {
    return g_root_prefix + "/usr/share/gpkg/base-system.json";
}

unsigned int json_hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return static_cast<unsigned int>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned int>(10 + (c - 'a'));
    if (c >= 'A' && c <= 'F') return static_cast<unsigned int>(10 + (c - 'A'));
    return 0;
}

void append_json_utf8_codepoint(std::string& out, unsigned int codepoint) {
    if (codepoint <= 0x7F) {
        out += static_cast<char>(codepoint);
    } else if (codepoint <= 0x7FF) {
        out += static_cast<char>(0xC0 | ((codepoint >> 6) & 0x1F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint <= 0xFFFF) {
        out += static_cast<char>(0xE0 | ((codepoint >> 12) & 0x0F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | ((codepoint >> 18) & 0x07));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
}

std::string json_unescape_token(const std::string& input) {
    std::string output;
    output.reserve(input.size());

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c != '\\' || i + 1 >= input.size()) {
            output += c;
            continue;
        }

        char esc = input[++i];
        switch (esc) {
            case '"': output += '"'; break;
            case '\\': output += '\\'; break;
            case '/': output += '/'; break;
            case 'b': output += '\b'; break;
            case 'f': output += '\f'; break;
            case 'n': output += '\n'; break;
            case 'r': output += '\r'; break;
            case 't': output += '\t'; break;
            case 'u': {
                if (i + 4 >= input.size()) {
                    output += "\\u";
                    break;
                }

                bool valid = true;
                unsigned int codepoint = 0;
                for (size_t j = 0; j < 4; ++j) {
                    char hex = input[i + 1 + j];
                    if (!std::isxdigit(static_cast<unsigned char>(hex))) {
                        valid = false;
                        break;
                    }
                    codepoint = (codepoint << 4) | json_hex_digit_value(hex);
                }

                if (!valid) {
                    output += "\\u";
                    break;
                }

                append_json_utf8_codepoint(output, codepoint);
                i += 4;
                break;
            }
            default:
                output += esc;
                break;
        }
    }

    return output;
}

template <typename Func>
void foreach_json_object_in_file(const std::string& filepath, Func callback) {
    std::ifstream f(filepath);
    if (!f) return;

    std::string obj;
    obj.reserve(8192);

    bool in_string = false;
    bool escape = false;
    int depth = 0;
    char ch = '\0';
    while (f.get(ch)) {
        if (depth == 0) {
            if (ch != '{') continue;
            obj.clear();
            obj.push_back(ch);
            depth = 1;
            in_string = false;
            escape = false;
            continue;
        }

        obj.push_back(ch);

        if (escape) {
            escape = false;
            continue;
        }
        if (ch == '\\' && in_string) {
            escape = true;
            continue;
        }
        if (ch == '"') {
            in_string = !in_string;
            continue;
        }
        if (in_string) continue;

        if (ch == '{') {
            ++depth;
            continue;
        }
        if (ch == '}') {
            --depth;
            if (depth == 0) {
                if (!callback(obj)) break;
                obj.clear();
            }
        }
    }
}

bool get_json_string_value_from_object(const std::string& obj, const std::string& key, std::string& out_val) {
    size_t key_pos = obj.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return false;

    size_t colon = obj.find(':', key_pos);
    if (colon == std::string::npos) return false;

    size_t value_start = obj.find('"', colon);
    if (value_start == std::string::npos) return false;

    size_t value_end = obj.find('"', value_start + 1);
    while (value_end != std::string::npos && obj[value_end - 1] == '\\') {
        value_end = obj.find('"', value_end + 1);
    }
    if (value_end == std::string::npos) return false;

    out_val = json_unescape_token(obj.substr(value_start + 1, value_end - value_start - 1));
    return true;
}

bool get_json_string_array_from_object(
    const std::string& obj,
    const std::string& key,
    std::vector<std::string>& out_arr
) {
    out_arr.clear();

    size_t key_pos = obj.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return false;

    size_t colon = obj.find(':', key_pos);
    size_t arr_start = obj.find('[', colon);
    size_t arr_end = obj.find(']', arr_start);
    if (arr_start == std::string::npos || arr_end == std::string::npos) return false;

    size_t pos = arr_start + 1;
    while (pos < arr_end) {
        size_t value_start = obj.find('"', pos);
        if (value_start == std::string::npos || value_start >= arr_end) break;

        size_t value_end = obj.find('"', value_start + 1);
        while (value_end != std::string::npos && obj[value_end - 1] == '\\') {
            value_end = obj.find('"', value_end + 1);
        }
        if (value_end == std::string::npos || value_end > arr_end) break;

        out_arr.push_back(
            json_unescape_token(obj.substr(value_start + 1, value_end - value_start - 1))
        );
        pos = value_end + 1;
    }

    return true;
}

void populate_base_system_owner_map(std::map<std::string, std::string>& owner_by_path) {
    foreach_json_object_in_file(get_base_system_registry_path(), [&](const std::string& obj) {
        std::string package;
        std::vector<std::string> files;
        if (!get_json_string_value_from_object(obj, "package", package)) return true;
        if (!get_json_string_array_from_object(obj, "files", files)) return true;

        for (const auto& owned_path : files) {
            std::string canonical_path = canonical_multiarch_logical_path(owned_path);
            if (canonical_path.empty()) continue;
            owner_by_path.emplace(canonical_path, package);
        }
        return true;
    });
}

const InstalledManifestSnapshot& ensure_installed_manifest_snapshot() {
    if (g_installed_manifest_snapshot.loaded) return g_installed_manifest_snapshot;

    g_installed_manifest_snapshot.loaded = true;
    g_installed_manifest_snapshot.installed_packages = get_installed_packages_from_disk(".list");
    for (const auto& pkg_name : g_installed_manifest_snapshot.installed_packages) {
        std::vector<std::string> files = read_list_file_from_disk(pkg_name);
        g_installed_manifest_snapshot.file_lists_by_package.emplace(pkg_name, files);
        for (const auto& owned_path : files) {
            std::string canonical_path = canonical_multiarch_logical_path(owned_path);
            if (canonical_path.empty()) continue;
            g_installed_manifest_snapshot.owner_by_path.emplace(canonical_path, pkg_name);
        }
    }
    populate_base_system_owner_map(g_installed_manifest_snapshot.base_owner_by_path);

    return g_installed_manifest_snapshot;
}

void invalidate_installed_manifest_snapshot() {
    g_installed_manifest_snapshot = InstalledManifestSnapshot{};
}

std::vector<std::string> read_installed_list_file_cached(const std::string& pkg_name) {
    const auto& snapshot = ensure_installed_manifest_snapshot();
    auto it = snapshot.file_lists_by_package.find(pkg_name);
    if (it != snapshot.file_lists_by_package.end()) return it->second;
    return read_list_file_from_disk(pkg_name);
}

std::string find_cached_file_owner(const std::string& pkg_name, const std::string& file_path) {
    const auto& snapshot = ensure_installed_manifest_snapshot();
    std::string canonical_path = canonical_multiarch_logical_path(file_path);
    auto it = snapshot.owner_by_path.find(canonical_path);
    if (it == snapshot.owner_by_path.end() || it->second == pkg_name) return "";
    return it->second;
}

std::string find_cached_base_file_owner(const std::string& file_path) {
    const auto& snapshot = ensure_installed_manifest_snapshot();
    std::string canonical_path = canonical_multiarch_logical_path(file_path);
    auto it = snapshot.base_owner_by_path.find(canonical_path);
    if (it == snapshot.base_owner_by_path.end()) return "";
    return it->second;
}

std::vector<std::string> read_list_file(const std::string& pkg_name) {
    return read_list_file_from_disk(pkg_name);
}

std::vector<std::string> get_installed_packages(const std::string& extension) {
    return get_installed_packages_from_disk(extension);
}

bool read_file_prefix(const std::string& path, unsigned char* buffer, size_t count) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    in.read(reinterpret_cast<char*>(buffer), static_cast<std::streamsize>(count));
    return static_cast<size_t>(in.gcount()) == count;
}

bool looks_like_linker_script_prefix(const std::string& prefix) {
    return prefix.rfind("/*", 0) == 0 ||
           prefix.rfind("INPUT(", 0) == 0 ||
           prefix.rfind("GROUP(", 0) == 0 ||
           prefix.rfind("OUTPUT_FORMAT(", 0) == 0;
}

std::string path_basename(const std::string& path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return path;
    return path.substr(slash + 1);
}

bool shared_object_suffix_is_valid(const std::string& suffix) {
    if (suffix.empty()) return true;

    size_t pos = 0;
    while (pos < suffix.size()) {
        if (suffix[pos] != '.') return false;
        ++pos;

        size_t start = pos;
        while (pos < suffix.size() && std::isdigit(static_cast<unsigned char>(suffix[pos]))) {
            ++pos;
        }
        if (pos == start) return false;
    }

    return true;
}

bool looks_like_shared_object_path(const std::string& path) {
    std::string name = path_basename(path);

    size_t so_pos = name.find(".so");
    if (so_pos != std::string::npos) {
        bool valid_prefix = name.rfind("lib", 0) == 0 || name.rfind("ld-linux-", 0) == 0;
        if (valid_prefix && shared_object_suffix_is_valid(name.substr(so_pos + 3))) {
            return true;
        }
    }

    return name == "lib.so";
}

bool should_validate_as_elf(const std::string& path, off_t size) {
    if (looks_like_shared_object_path(path)) return true;
    if (size < 4) return false;

    unsigned char ident[4];
    if (!read_file_prefix(path, ident, sizeof(ident))) return false;
    return ident[EI_MAG0] == ELFMAG0 &&
           ident[EI_MAG1] == ELFMAG1 &&
           ident[EI_MAG2] == ELFMAG2 &&
           ident[EI_MAG3] == ELFMAG3;
}

bool validate_elf_file(const std::string& path, off_t size, std::string* error) {
    bool shared_object_candidate = looks_like_shared_object_path(path);
    if (!should_validate_as_elf(path, size)) return true;

    if (size < static_cast<off_t>(EI_NIDENT)) {
        if (error) *error = shared_object_candidate
            ? "shared object file is too small to be valid"
            : "ELF file is too small to be valid";
        return false;
    }

    unsigned char ident[EI_NIDENT];
    if (!read_file_prefix(path, ident, sizeof(ident))) {
        if (error) *error = "unable to read ELF identification bytes";
        return false;
    }

    if (!(ident[EI_MAG0] == ELFMAG0 &&
          ident[EI_MAG1] == ELFMAG1 &&
          ident[EI_MAG2] == ELFMAG2 &&
          ident[EI_MAG3] == ELFMAG3)) {
        if (shared_object_candidate) {
            char text_prefix[16] = {0};
            std::ifstream in(path, std::ios::binary);
            if (in) {
                in.read(text_prefix, sizeof(text_prefix) - 1);
            }
            if (looks_like_linker_script_prefix(text_prefix)) return true;
            if (error) *error = "shared object is neither a valid ELF nor a linker script";
            return false;
        }
        if (error) *error = "missing ELF magic";
        return false;
    }

    if (ident[EI_CLASS] == ELFCLASS64) {
        if (size < static_cast<off_t>(sizeof(Elf64_Ehdr))) {
            if (error) *error = "ELF64 header is truncated";
            return false;
        }

        Elf64_Ehdr ehdr {};
        std::ifstream in(path, std::ios::binary);
        if (!in || !in.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr))) {
            if (error) *error = "unable to read ELF64 header";
            return false;
        }

        if (ehdr.e_phoff > 0 &&
            (static_cast<unsigned long long>(ehdr.e_phoff) +
             static_cast<unsigned long long>(ehdr.e_phentsize) * ehdr.e_phnum) >
                static_cast<unsigned long long>(size)) {
            if (error) *error = "ELF64 program header table extends past end of file";
            return false;
        }
        if (ehdr.e_shoff > 0 &&
            (static_cast<unsigned long long>(ehdr.e_shoff) +
             static_cast<unsigned long long>(ehdr.e_shentsize) * ehdr.e_shnum) >
                static_cast<unsigned long long>(size)) {
            if (error) *error = "ELF64 section header table extends past end of file";
            return false;
        }
        if (ehdr.e_phoff > 0 && ehdr.e_phnum > 0) {
            if (ehdr.e_phentsize < sizeof(Elf64_Phdr)) {
                if (error) *error = "ELF64 program header entries are smaller than expected";
                return false;
            }

            for (size_t i = 0; i < ehdr.e_phnum; ++i) {
                unsigned long long phdr_offset =
                    static_cast<unsigned long long>(ehdr.e_phoff) +
                    static_cast<unsigned long long>(ehdr.e_phentsize) * i;
                if (phdr_offset + sizeof(Elf64_Phdr) >
                    static_cast<unsigned long long>(size)) {
                    if (error) *error = "ELF64 program header table extends past end of file";
                    return false;
                }

                Elf64_Phdr phdr {};
                in.clear();
                in.seekg(static_cast<std::streamoff>(phdr_offset), std::ios::beg);
                if (!in.read(reinterpret_cast<char*>(&phdr), sizeof(phdr))) {
                    if (error) *error = "unable to read ELF64 program header";
                    return false;
                }

                if (phdr.p_filesz == 0) continue;
                if ((static_cast<unsigned long long>(phdr.p_offset) +
                     static_cast<unsigned long long>(phdr.p_filesz)) >
                    static_cast<unsigned long long>(size)) {
                    if (error) *error = "ELF64 segment extends past end of file";
                    return false;
                }
            }
        }
        return true;
    }

    if (ident[EI_CLASS] == ELFCLASS32) {
        if (size < static_cast<off_t>(sizeof(Elf32_Ehdr))) {
            if (error) *error = "ELF32 header is truncated";
            return false;
        }

        Elf32_Ehdr ehdr {};
        std::ifstream in(path, std::ios::binary);
        if (!in || !in.read(reinterpret_cast<char*>(&ehdr), sizeof(ehdr))) {
            if (error) *error = "unable to read ELF32 header";
            return false;
        }

        if (ehdr.e_phoff > 0 &&
            (static_cast<unsigned long long>(ehdr.e_phoff) +
             static_cast<unsigned long long>(ehdr.e_phentsize) * ehdr.e_phnum) >
                static_cast<unsigned long long>(size)) {
            if (error) *error = "ELF32 program header table extends past end of file";
            return false;
        }
        if (ehdr.e_shoff > 0 &&
            (static_cast<unsigned long long>(ehdr.e_shoff) +
             static_cast<unsigned long long>(ehdr.e_shentsize) * ehdr.e_shnum) >
                static_cast<unsigned long long>(size)) {
            if (error) *error = "ELF32 section header table extends past end of file";
            return false;
        }
        if (ehdr.e_phoff > 0 && ehdr.e_phnum > 0) {
            if (ehdr.e_phentsize < sizeof(Elf32_Phdr)) {
                if (error) *error = "ELF32 program header entries are smaller than expected";
                return false;
            }

            for (size_t i = 0; i < ehdr.e_phnum; ++i) {
                unsigned long long phdr_offset =
                    static_cast<unsigned long long>(ehdr.e_phoff) +
                    static_cast<unsigned long long>(ehdr.e_phentsize) * i;
                if (phdr_offset + sizeof(Elf32_Phdr) >
                    static_cast<unsigned long long>(size)) {
                    if (error) *error = "ELF32 program header table extends past end of file";
                    return false;
                }

                Elf32_Phdr phdr {};
                in.clear();
                in.seekg(static_cast<std::streamoff>(phdr_offset), std::ios::beg);
                if (!in.read(reinterpret_cast<char*>(&phdr), sizeof(phdr))) {
                    if (error) *error = "unable to read ELF32 program header";
                    return false;
                }

                if (phdr.p_filesz == 0) continue;
                if ((static_cast<unsigned long long>(phdr.p_offset) +
                     static_cast<unsigned long long>(phdr.p_filesz)) >
                    static_cast<unsigned long long>(size)) {
                    if (error) *error = "ELF32 segment extends past end of file";
                    return false;
                }
            }
        }
        return true;
    }

    if (error) *error = "ELF file has an unknown class";
    return false;
}

bool files_touch_runtime_linker_state(const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
        if (path.find("/lib64/") != std::string::npos ||
            path.find("/lib/") != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool compare_regular_files_exact(
    const std::string& expected_path,
    const std::string& actual_path,
    std::string* error = nullptr
) {
    if (error) error->clear();

    struct stat expected_st;
    struct stat actual_st;
    if (stat(expected_path.c_str(), &expected_st) != 0 || !S_ISREG(expected_st.st_mode)) {
        if (error) *error = "failed to inspect staged file";
        return false;
    }
    if (stat(actual_path.c_str(), &actual_st) != 0 || !S_ISREG(actual_st.st_mode)) {
        if (error) *error = "failed to inspect installed file";
        return false;
    }
    if (expected_st.st_size != actual_st.st_size) {
        if (error) *error = "installed file size differs from staged payload";
        return false;
    }

    int expected_fd = open(expected_path.c_str(), O_RDONLY);
    if (expected_fd < 0) {
        if (error) *error = "failed to open staged file";
        return false;
    }

    int actual_fd = open(actual_path.c_str(), O_RDONLY);
    if (actual_fd < 0) {
        close(expected_fd);
        if (error) *error = "failed to open installed file";
        return false;
    }

    char expected_buffer[65536];
    char actual_buffer[65536];
    bool ok = true;
    while (ok) {
        ssize_t expected_read = read(expected_fd, expected_buffer, sizeof(expected_buffer));
        ssize_t actual_read = read(actual_fd, actual_buffer, sizeof(actual_buffer));
        if (expected_read < 0 || actual_read < 0) {
            if (error) *error = "failed while comparing file contents";
            ok = false;
            break;
        }
        if (expected_read != actual_read) {
            if (error) *error = "installed file length differs while reading";
            ok = false;
            break;
        }
        if (expected_read == 0) break;
        if (memcmp(expected_buffer, actual_buffer, static_cast<size_t>(expected_read)) != 0) {
            if (error) *error = "installed file contents differ from staged payload";
            ok = false;
            break;
        }
    }

    close(expected_fd);
    close(actual_fd);
    return ok;
}

bool is_etc_config_path(const std::string& path) {
    return path.size() > 5 && path.rfind("/etc/", 0) == 0;
}

std::string canonical_conffile_path_for_owned_entry(const std::string& path) {
    if (is_etc_config_path(path)) return path;

    const std::string suffix = ".gpkg-new";
    if (path.size() > suffix.size() &&
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0) {
        std::string base = path.substr(0, path.size() - suffix.size());
        if (is_etc_config_path(base)) return base;
    }

    return "";
}

std::vector<std::string> collect_package_conffiles_from_entries(const std::vector<std::string>& entries) {
    std::vector<std::string> conffiles;
    std::set<std::string> seen;
    for (const auto& entry : entries) {
        std::string canonical = canonical_conffile_path_for_owned_entry(entry);
        if (canonical.empty()) continue;
        if (seen.insert(canonical).second) conffiles.push_back(canonical);
    }
    return conffiles;
}

std::vector<std::string> load_package_conffiles(const std::string& pkg_name) {
    std::vector<std::string> conffiles;
    std::ifstream in(get_conffile_manifest_path(pkg_name));
    if (in) {
        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty()) continue;
            if (std::find(conffiles.begin(), conffiles.end(), line) == conffiles.end()) {
                conffiles.push_back(line);
            }
        }
        return conffiles;
    }

    return collect_package_conffiles_from_entries(read_list_file(pkg_name));
}

bool write_package_conffiles(const std::string& pkg_name, const std::vector<std::string>& conffiles) {
    std::vector<std::string> normalized = conffiles;
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());

    std::string path = get_conffile_manifest_path(pkg_name);
    if (normalized.empty()) {
        return remove_live_path_exact(path);
    }

    std::ostringstream out;
    for (const auto& entry : normalized) out << entry << "\n";
    return write_text_file_atomic(path, out.str(), 0644);
}

std::string find_file_owner(const std::string& pkg_name, const std::string& file_path) {
    return find_cached_file_owner(pkg_name, file_path);
}

struct PreservedConfigFile {
    std::string path;
    std::string backup_path;
    std::string staged_path;
};

struct ReplacedSystemFile {
    std::string path;
    std::string backup_path;
};

bool path_is_directory_or_directory_symlink(
    const std::string& full_path,
    const struct stat* lstat_result
) {
    struct stat local_st;
    const struct stat* st = lstat_result;
    if (!st) {
        if (lstat(full_path.c_str(), &local_st) != 0) return false;
        st = &local_st;
    }

    if (S_ISDIR(st->st_mode)) return true;
    if (!S_ISLNK(st->st_mode)) return false;

    struct stat target_st;
    return stat(full_path.c_str(), &target_st) == 0 && S_ISDIR(target_st.st_mode);
}

std::string get_replaced_system_dir(const std::string& pkg_name) {
    return get_info_dir() + pkg_name + ".system-backup";
}

std::string get_replaced_system_manifest(const std::string& pkg_name) {
    return get_info_dir() + pkg_name + ".system-backup.list";
}

bool should_preserve_local_config_file(
    const std::string& pkg_name,
    const std::string& file_path
) {
    if (!is_etc_config_path(file_path)) return false;

    std::string full_path = g_root_prefix + file_path;
    struct stat st;
    if (lstat(full_path.c_str(), &st) != 0) return false;
    if (path_is_directory_or_directory_symlink(full_path, &st)) return false;

    return find_file_owner(pkg_name, file_path).empty();
}

std::vector<PreservedConfigFile> collect_preserved_config_files(
    const std::string& pkg_name,
    const std::vector<std::string>& new_files
) {
    std::vector<PreservedConfigFile> preserved;
    size_t preserve_index = 0;

    for (const auto& file : new_files) {
        if (!should_preserve_local_config_file(pkg_name, file)) continue;

        PreservedConfigFile entry;
        entry.path = file;
        entry.backup_path = g_tmp_extract_path + "preserve/" + std::to_string(preserve_index++) + ".orig";
        preserved.push_back(entry);
    }

    return preserved;
}

bool backup_preserved_config_files(const std::vector<PreservedConfigFile>& preserved) {
    if (preserved.empty()) return true;
    if (!mkdir_p(g_tmp_extract_path + "preserve")) return false;

    for (const auto& entry : preserved) {
        std::string source_path = g_root_prefix + entry.path;
        if (!copy_path_atomic_no_follow(source_path, entry.backup_path)) {
            std::cerr << "E: Failed to back up local config " << entry.path << std::endl;
            return false;
        }
    }

    return true;
}

bool paths_are_identical(const std::string& left, const std::string& right) {
    struct stat left_st;
    struct stat right_st;
    if (lstat(left.c_str(), &left_st) != 0) return false;
    if (lstat(right.c_str(), &right_st) != 0) return false;

    mode_t left_type = left_st.st_mode & S_IFMT;
    mode_t right_type = right_st.st_mode & S_IFMT;
    if (left_type != right_type) return false;

    if (S_ISLNK(left_st.st_mode)) {
        return read_symlink_target(left) == read_symlink_target(right);
    }

    if (S_ISREG(left_st.st_mode)) {
        return compare_regular_files_exact(left, right);
    }

    return false;
}

void apply_preserved_config_metadata(
    std::vector<std::string>& installed_files,
    const std::vector<PreservedConfigFile>& preserved
) {
    for (const auto& entry : preserved) {
        auto it = std::find(installed_files.begin(), installed_files.end(), entry.path);
        if (it == installed_files.end()) continue;

        if (entry.staged_path.empty()) installed_files.erase(it);
        else *it = entry.staged_path;
    }
}

bool finalize_preserved_config_files(std::vector<PreservedConfigFile>& preserved) {
    for (auto& entry : preserved) {
        std::string live_path = g_root_prefix + entry.path;
        std::string staged_live_path = live_path + ".gpkg-new";

        if (access(live_path.c_str(), F_OK) != 0) {
            std::cerr << "E: Expected package config file was not installed: " << entry.path << std::endl;
            return false;
        }

        if (paths_are_identical(entry.backup_path, live_path)) {
            if (!remove_live_path_exact(live_path)) {
                std::cerr << "E: Failed to discard duplicate package config " << entry.path << std::endl;
                return false;
            }
            entry.staged_path.clear();
            VLOG("Keeping existing config " << entry.path << " (package copy was identical).");
        } else {
            if (!remove_live_path_exact(staged_live_path)) {
                std::cerr << "E: Failed to clear stale staged config " << entry.path << ".gpkg-new" << std::endl;
                return false;
            }
            if (rename(live_path.c_str(), staged_live_path.c_str()) != 0) {
                std::cerr << "E: Failed to stage package config as " << entry.path << ".gpkg-new" << std::endl;
                return false;
            }
            entry.staged_path = entry.path + ".gpkg-new";
            std::cout << "W: Preserving local config " << entry.path
                      << "; package version saved as " << entry.staged_path << std::endl;
        }

        if (!copy_path_atomic_no_follow(entry.backup_path, live_path)) {
            std::cerr << "E: Failed to restore preserved config " << entry.path << std::endl;
            return false;
        }
    }

    return true;
}

std::vector<ReplacedSystemFile> load_replaced_system_files(const std::string& pkg_name) {
    std::vector<ReplacedSystemFile> entries;
    std::ifstream in(get_replaced_system_manifest(pkg_name));
    if (!in) return entries;

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty()) continue;
        size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        ReplacedSystemFile entry;
        entry.path = line.substr(0, tab);
        entry.backup_path = line.substr(tab + 1);
        if (!entry.path.empty() && !entry.backup_path.empty()) {
            entries.push_back(entry);
        }
    }
    return entries;
}

bool write_replaced_system_files(
    const std::string& pkg_name,
    const std::vector<ReplacedSystemFile>& entries
) {
    if (entries.empty()) {
        unlink(get_replaced_system_manifest(pkg_name).c_str());
        if (!remove_tree_no_follow(get_replaced_system_dir(pkg_name)) && errno != ENOENT) {
            std::cerr << "E: Failed to remove stale system backup directory for "
                      << pkg_name << ": " << strerror(errno) << std::endl;
            return false;
        }
        return true;
    }

    std::ostringstream out;
    for (const auto& entry : entries) {
        out << entry.path << "\t" << entry.backup_path << "\n";
    }
    if (!write_text_file_atomic(get_replaced_system_manifest(pkg_name), out.str(), 0644)) {
        std::cerr << "E: Failed to write system backup manifest for " << pkg_name << std::endl;
        return false;
    }
    return true;
}

bool should_backup_replaced_system_file(
    const std::string& pkg_name,
    const std::string& file_path,
    const std::set<std::string>& owned_by_me
) {
    std::string full_path = g_root_prefix + file_path;
    struct stat st;
    if (lstat(full_path.c_str(), &st) != 0) return false;
    if (path_is_directory_or_directory_symlink(full_path, &st)) return false;
    if (should_preserve_local_config_file(pkg_name, file_path)) return false;
    if (owned_by_me.count(canonical_multiarch_logical_path(file_path))) return false;
    return find_file_owner(pkg_name, file_path).empty();
}

std::vector<ReplacedSystemFile> collect_replaced_system_files(
    const std::string& pkg_name,
    const std::vector<std::string>& new_files,
    const std::set<std::string>& owned_by_me
) {
    std::vector<ReplacedSystemFile> entries = load_replaced_system_files(pkg_name);
    std::set<std::string> tracked_paths;
    for (const auto& entry : entries) {
        tracked_paths.insert(canonical_multiarch_logical_path(entry.path));
    }

    size_t next_index = entries.size();
    for (const auto& file : new_files) {
        std::string canonical_file = canonical_multiarch_logical_path(file);
        if (tracked_paths.count(canonical_file)) continue;
        if (!should_backup_replaced_system_file(pkg_name, file, owned_by_me)) continue;

        ReplacedSystemFile entry;
        entry.path = canonical_file;
        entry.backup_path = get_replaced_system_dir(pkg_name) + "/" + std::to_string(next_index++);
        entries.push_back(entry);
        tracked_paths.insert(canonical_file);
    }

    return entries;
}

bool backup_replaced_system_files(const std::vector<ReplacedSystemFile>& entries) {
    if (entries.empty()) return true;
    if (!mkdir_p(entries.front().backup_path.substr(0, entries.front().backup_path.find_last_of('/')))) {
        return false;
    }

    for (const auto& entry : entries) {
        if (path_exists_no_follow(entry.backup_path)) continue;

        std::string source_path = g_root_prefix + entry.path;
        struct stat st;
        if (lstat(source_path.c_str(), &st) != 0) {
            if (errno == ENOENT) continue;
            std::cerr << "E: Failed to inspect replaced base file " << entry.path
                      << ": " << strerror(errno) << std::endl;
            return false;
        }
        if (path_is_directory_or_directory_symlink(source_path, &st)) {
            VLOG("Skipping backup of directory anchor " << entry.path);
            continue;
        }

        std::string parent_dir = entry.backup_path.substr(0, entry.backup_path.find_last_of('/'));
        if (!mkdir_p(parent_dir)) {
            std::cerr << "E: Failed to create system backup directory " << parent_dir << std::endl;
            return false;
        }

        if (!copy_path_atomic_no_follow(source_path, entry.backup_path)) {
            std::cerr << "E: Failed to back up replaced base file " << entry.path << std::endl;
            return false;
        }
    }

    return true;
}

// --- Removal Logic ---

bool action_register_file(const std::string& pkg_name, const std::string& file_path) {
    if (pkg_name.empty() || file_path.empty()) return false;
    std::string list_path = get_info_dir() + pkg_name + ".list";
    std::ofstream list_out(list_path, std::ios::app);
    if (!list_out) {
        std::cerr << "E: Failed to open " << list_path << " for appending." << std::endl;
        return false;
    }
    // Normalize path to start with /
    std::string safe_path = (file_path[0] == '/') ? file_path : "/" + file_path;
    list_out << safe_path << "\n";
    VLOG("Registered file " << safe_path << " for package " << pkg_name);
    return true;
}

bool action_register_undo(const std::string& pkg_name, const std::string& cmd) {
    if (pkg_name.empty() || cmd.empty()) return false;
    std::string undo_path = get_info_dir() + pkg_name + ".undo";
    std::ofstream undo_out(undo_path, std::ios::app);
    if (!undo_out) {
        std::cerr << "E: Failed to open " << undo_path << " for appending." << std::endl;
        return false;
    }
    undo_out << cmd << "\n";
    VLOG("Registered undo command '" << cmd << "' for package " << pkg_name);
    return true;
}

bool remove_path(const std::string& abs_path) {
    std::string safe_abs = (abs_path.length() > 0 && abs_path[0] != '/') ? "/" + abs_path : abs_path;
    std::string full_path = g_root_prefix + safe_abs;
    
    struct stat st;
    
    if (lstat(full_path.c_str(), &st) != 0) {
        if (errno == ENOENT) return true; // Already gone
        std::cerr << "W: Failed to stat " << full_path << ": " << strerror(errno) << std::endl;
        return false;
    }

    if (path_is_directory_or_directory_symlink(full_path, &st) && !S_ISDIR(st.st_mode)) {
        VLOG("Skipping removal of directory symlink: " << full_path);
        return true;
    }

    if (S_ISDIR(st.st_mode)) {
        // Strict check: manually verify if directory is empty
        VLOG("Inspecting directory for removal: " << full_path);
        
        std::vector<std::string> contents;
        errno = 0;
        DIR* d = opendir(full_path.c_str());
        if (d) {
            struct dirent* dir;
            while ((dir = readdir(d)) != NULL) {
                if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
                contents.push_back(dir->d_name);
            }
            if (errno != 0) {
                 std::cerr << "E: readdir failed with errno: " << errno << " (" << strerror(errno) << ")" << std::endl;
            }
            closedir(d);
        } else {
            std::cerr << "W: Could not open directory for empty check: " << full_path << " (" << strerror(errno) << ")" << std::endl;
            return true; // Safety: Assume not empty if we can't read it
        }

        size_t count = contents.size();
        
        if (count > 0) {
            // Detailed logging of contents
            if (g_verbose) {
                std::cout << "[WORKER] Directory " << full_path << " contains " << count << " items:" << std::endl;
                for (const auto& item : contents) {
                    std::cout << "[WORKER]  - " << item << std::endl;
                }
            }

            if (count > 1) {
                // this may spam the terminal. leave this commented out
                // std::cout << "W: Directory " << full_path << " contains " << count << " items (>1). Aborting removal instantly." << std::endl;
                return true; 
            } else {
                // count == 1
                VLOG("Directory contains 1 item. Skipping removal: " << full_path);
                return true; 
            }
        } else {
            // count == 0
            VLOG("Directory is empty (count 0). Removing.");
            if (rmdir(full_path.c_str()) == 0) {
                VLOG("Removed directory: " << full_path);
                return true;
            } else {
                std::cerr << "W: Failed to remove directory " << full_path << std::endl;
                return false;
            }
        }
    } else if (S_ISLNK(st.st_mode)) {
        // It is a symlink. Check if it points to a directory.
        struct stat target_st;
        if (stat(full_path.c_str(), &target_st) == 0 && S_ISDIR(target_st.st_mode)) {
             // This is a symlink to a directory (e.g. /lib -> /usr/lib).
             // Deleting this would break the system pathing.
             VLOG("Skipping removal of directory symlink: " << full_path);
             return true; 
        }
        
        if (unlink(full_path.c_str()) == 0) {
            VLOG("Removed symlink: " << full_path);
            return true;
        } else {
            std::cerr << "E: Failed to remove symlink " << full_path << ": " << strerror(errno) << std::endl;
            return false;
        }
    } else {
        if (unlink(full_path.c_str()) == 0) {
            VLOG("Removed file: " << full_path);
            return true;
        } else {
            std::cerr << "E: Failed to remove file " << full_path << ": " << strerror(errno) << std::endl;
            return false;
        }
    }
}

// --- Installation Logic ---

std::string normalize_tar_member_path(const std::string& raw_line, bool strip_data);

struct TarPayloadInspection {
    bool strip_data = false;
    std::vector<std::string> paths;
};

TarPayloadInspection inspect_tar_payload(const std::string& tar_path) {
    TarPayloadInspection inspection;
    std::vector<GpkgArchive::TarEntry> entries;
    std::string error;
    if (!GpkgArchive::tar_list_entries(tar_path, entries, &error)) {
        VLOG("Failed to inspect tar archive " << tar_path << ": " << error);
        return inspection;
    }

    for (const auto& entry : entries) {
        std::string normalized = trim(entry.path);
        if (normalized.rfind("./", 0) == 0) normalized.erase(0, 2);
        if (normalized.rfind("data/", 0) == 0 || normalized == "data") {
            inspection.strip_data = true;
            break;
        }
    }

    std::set<std::string> seen;
    for (const auto& entry : entries) {
        std::string line = normalize_tar_member_path(entry.path, inspection.strip_data);
        if (line.empty()) continue;
        if (seen.insert(line).second) inspection.paths.push_back(line);
    }
    return inspection;
}

std::string normalize_tar_member_path(const std::string& raw_line, bool strip_data) {
    std::string line = trim(raw_line);
    if (line.empty() || line == "." || line == "./") return "";

    if (line.find("./") == 0) line = line.substr(2);

    if (strip_data) {
        if (line.find("data/") == 0) {
            line = line.substr(5);
        } else {
            return "";
        }
    }

    if (!line.empty() && line.back() == '/') line.pop_back();
    if (line.empty()) return "";
    return "/" + line;
}

bool is_existing_symlink_directory(const std::string& full_path) {
    struct stat link_st;
    if (lstat(full_path.c_str(), &link_st) != 0 || !S_ISLNK(link_st.st_mode)) return false;

    return path_is_directory_or_directory_symlink(full_path, &link_st);
}

struct StagedInstallEntry {
    std::string path;
    std::string staged_path;
    bool is_directory = false;
    bool is_symlink = false;
    mode_t mode = 0644;
    std::string symlink_target;
    size_t depth = 0;
};

void prune_non_owned_directory_symlink_entries(
    std::vector<std::string>& installed_files,
    const std::vector<StagedInstallEntry>& staged_entries
) {
    std::set<std::string> skipped_paths;
    for (const auto& entry : staged_entries) {
        if (!entry.is_directory) continue;
        if (!is_existing_symlink_directory(g_root_prefix + entry.path)) continue;
        skipped_paths.insert(entry.path);
    }
    if (skipped_paths.empty()) return;

    installed_files.erase(
        std::remove_if(
            installed_files.begin(),
            installed_files.end(),
            [&](const std::string& path) { return skipped_paths.count(path) != 0; }
        ),
        installed_files.end()
    );
}

struct InstallRollbackEntry {
    std::string path;
    std::string live_full_path;
    std::string backup_full_path;
    bool created_only = false;
};

void rollback_install_changes(const std::vector<InstallRollbackEntry>& rollback_entries);
void discard_install_backups(const std::vector<InstallRollbackEntry>& rollback_entries);

std::vector<std::string> collect_install_relabel_paths(
    const std::vector<StagedInstallEntry>& staged_entries,
    const std::vector<InstallRollbackEntry>& rollback_entries
) {
    // Relabel files we actually wrote plus directories we created; pre-existing
    // parent directories like /usr or /usr/share do not need recursive relabels.
    std::set<std::string> created_paths;
    for (const auto& entry : rollback_entries) {
        if (!entry.created_only) continue;
        created_paths.insert(canonical_multiarch_logical_path(entry.path));
    }

    std::vector<std::string> relabel_paths;
    std::set<std::string> seen;
    for (const auto& entry : staged_entries) {
        std::string canonical_path = canonical_multiarch_logical_path(entry.path);
        if (canonical_path.empty()) continue;

        if (entry.is_directory && created_paths.count(canonical_path) == 0) continue;
        if (!seen.insert(canonical_path).second) continue;
        relabel_paths.push_back(canonical_path);
    }

    return relabel_paths;
}

bool install_path_is_early_selinux_relabel_candidate(const StagedInstallEntry& entry) {
    std::string path = canonical_multiarch_logical_path(entry.path);
    if (path.empty()) return false;

    const char* critical_prefixes[] = {
        "/bin",
        "/sbin",
        "/lib",
        "/lib64",
        "/usr/bin",
        "/usr/sbin",
        "/usr/lib",
        "/usr/lib64",
        "/usr/libexec",
    };
    for (const char* prefix : critical_prefixes) {
        std::string prefix_str = prefix;
        if (path == prefix_str || path.rfind(prefix_str + "/", 0) == 0) return true;
    }

    return !entry.is_directory && (entry.mode & 0111) != 0;
}

std::vector<std::string> collect_early_install_relabel_paths(
    const std::vector<StagedInstallEntry>& staged_entries,
    const std::vector<InstallRollbackEntry>& rollback_entries
) {
    std::set<std::string> created_paths;
    for (const auto& entry : rollback_entries) {
        if (!entry.created_only) continue;
        created_paths.insert(canonical_multiarch_logical_path(entry.path));
    }

    std::vector<std::string> relabel_paths;
    std::set<std::string> seen;
    for (const auto& entry : staged_entries) {
        if (!install_path_is_early_selinux_relabel_candidate(entry)) continue;

        std::string canonical_path = canonical_multiarch_logical_path(entry.path);
        if (canonical_path.empty()) continue;

        if (entry.is_directory && created_paths.count(canonical_path) == 0) continue;
        if (!seen.insert(canonical_path).second) continue;
        relabel_paths.push_back(canonical_path);
    }

    return relabel_paths;
}

std::vector<std::string> collect_postinstall_relabel_delta(
    const std::string& pkg_name,
    const std::vector<std::string>& already_relabelled_paths
) {
    std::set<std::string> seen;
    for (const auto& path : already_relabelled_paths) {
        seen.insert(canonical_multiarch_logical_path(path));
    }

    std::vector<std::string> delta;
    for (const auto& path : normalize_owned_manifest_paths(read_list_file(pkg_name))) {
        std::string canonical_path = canonical_multiarch_logical_path(path);
        if (canonical_path.empty()) continue;
        if (!seen.insert(canonical_path).second) continue;
        delta.push_back(canonical_path);
    }

    return delta;
}

size_t path_depth(const std::string& path) {
    return static_cast<size_t>(std::count(path.begin(), path.end(), '/'));
}

std::string path_parent_dir(const std::string& full_path) {
    size_t slash = full_path.find_last_of('/');
    if (slash == std::string::npos) return ".";
    if (slash == 0) return "/";
    return full_path.substr(0, slash);
}

std::string path_basename_component(const std::string& full_path) {
    size_t slash = full_path.find_last_of('/');
    if (slash == std::string::npos) return full_path;
    return full_path.substr(slash + 1);
}

std::string allocate_sibling_temp_path(const std::string& live_full_path, const std::string& tag, int* fd_out) {
    if (fd_out) *fd_out = -1;

    std::string parent = path_parent_dir(live_full_path);
    std::string base = path_basename_component(live_full_path);
    if (base.empty()) base = "entry";
    std::string pattern = parent + "/." + base + "." + tag + "-XXXXXX";

    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    int fd = mkstemp(buffer.data());
    if (fd < 0) return "";

    if (fd_out) {
        *fd_out = fd;
    } else {
        close(fd);
        unlink(buffer.data());
    }
    return std::string(buffer.data());
}

bool remove_live_path_exact(const std::string& live_full_path) {
    struct stat st;
    if (lstat(live_full_path.c_str(), &st) != 0) {
        return errno == ENOENT;
    }

    if (S_ISDIR(st.st_mode)) {
        return rmdir(live_full_path.c_str()) == 0 || errno == ENOENT;
    }
    return unlink(live_full_path.c_str()) == 0 || errno == ENOENT;
}

bool backup_live_path_if_present(
    const std::string& live_full_path,
    const std::string& path,
    std::vector<InstallRollbackEntry>& rollback_entries,
    bool* had_existing = nullptr
) {
    if (had_existing) *had_existing = false;

    struct stat st;
    if (lstat(live_full_path.c_str(), &st) != 0) {
        if (errno == ENOENT) return true;
        std::cerr << "E: Failed to inspect existing path " << live_full_path << ": "
                  << strerror(errno) << std::endl;
        return false;
    }

    std::string backup_full_path = allocate_sibling_temp_path(live_full_path, "gpkg-backup");
    if (backup_full_path.empty()) {
        std::cerr << "E: Failed to reserve backup path for " << live_full_path << std::endl;
        return false;
    }

    if (rename(live_full_path.c_str(), backup_full_path.c_str()) != 0) {
        if (errno == EXDEV) {
            if (!copy_path_atomic_no_follow(live_full_path, backup_full_path)) {
                std::cerr << "E: Failed to copy existing path aside for " << live_full_path << ": "
                          << strerror(errno) << std::endl;
                remove_tree_no_follow(backup_full_path);
                return false;
            }
            if (!remove_tree_no_follow(live_full_path)) {
                std::cerr << "E: Failed to remove existing path after copying backup for "
                          << live_full_path << ": " << strerror(errno) << std::endl;
                remove_tree_no_follow(backup_full_path);
                return false;
            }
        } else {
            std::cerr << "E: Failed to move existing path aside for " << live_full_path << ": "
                      << strerror(errno) << std::endl;
            remove_tree_no_follow(backup_full_path);
            return false;
        }
    }

    if (had_existing) *had_existing = true;
    rollback_entries.push_back({path, live_full_path, backup_full_path, false});
    return true;
}

bool prepare_path_for_transaction_write(
    const std::string& live_full_path,
    const std::string& logical_path,
    std::vector<InstallRollbackEntry>& rollback_entries
) {
    if (!mkdir_p(path_parent_dir(live_full_path))) {
        std::cerr << "E: Failed to prepare parent directory for " << live_full_path << std::endl;
        return false;
    }

    bool had_existing = false;
    if (!backup_live_path_if_present(live_full_path, logical_path, rollback_entries, &had_existing)) {
        return false;
    }

    if (!had_existing) {
        rollback_entries.push_back({logical_path, live_full_path, "", true});
    }

    return true;
}

bool copy_regular_file_contents(const std::string& source_path, int dest_fd) {
    int source_fd = open(source_path.c_str(), O_RDONLY);
    if (source_fd < 0) {
        std::cerr << "E: Failed to open staged file " << source_path << ": "
                  << strerror(errno) << std::endl;
        return false;
    }

    char buffer[65536];
    while (true) {
        ssize_t bytes_read = read(source_fd, buffer, sizeof(buffer));
        if (bytes_read == 0) break;
        if (bytes_read < 0) {
            std::cerr << "E: Failed to read staged file " << source_path << ": "
                      << strerror(errno) << std::endl;
            close(source_fd);
            return false;
        }

        ssize_t offset = 0;
        while (offset < bytes_read) {
            ssize_t bytes_written = write(dest_fd, buffer + offset, static_cast<size_t>(bytes_read - offset));
            if (bytes_written < 0) {
                std::cerr << "E: Failed while writing staged file " << source_path << ": "
                          << strerror(errno) << std::endl;
                close(source_fd);
                return false;
            }
            offset += bytes_written;
        }
    }

    if (fsync(dest_fd) != 0) {
        std::cerr << "E: Failed to flush staged file " << source_path << ": "
                  << strerror(errno) << std::endl;
        close(source_fd);
        return false;
    }

    close(source_fd);
    return true;
}

bool copy_directory_tree_no_follow(const std::string& source_path, const std::string& target_path) {
    struct stat st;
    if (lstat(source_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;

    if (!mkdir_p(path_parent_dir(target_path))) return false;
    if (mkdir(target_path.c_str(), st.st_mode & 07777) != 0) {
        if (errno != EEXIST) return false;

        struct stat target_st;
        if (lstat(target_path.c_str(), &target_st) != 0 || !S_ISDIR(target_st.st_mode)) {
            return false;
        }
    }
    chmod(target_path.c_str(), st.st_mode & 07777);

    DIR* dir = opendir(source_path.c_str());
    if (!dir) return false;

    bool ok = true;
    int saved_errno = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        std::string child_source = source_path + "/" + name;
        std::string child_target = target_path + "/" + name;
        if (!copy_path_atomic_no_follow(child_source, child_target)) {
            ok = false;
            saved_errno = errno;
            break;
        }
    }
    closedir(dir);

    if (!ok) {
        if (!remove_tree_no_follow(target_path) && errno != ENOENT) {
            std::cerr << "W: Failed to discard partial directory copy " << target_path
                      << ": " << strerror(errno) << std::endl;
        }
        if (saved_errno != 0) errno = saved_errno;
        return false;
    }

    return true;
}

bool write_text_file_atomic(const std::string& target_path, const std::string& content, mode_t mode) {
    if (!mkdir_p(path_parent_dir(target_path))) return false;

    int temp_fd = -1;
    std::string temp_path = allocate_sibling_temp_path(target_path, "gpkg-write", &temp_fd);
    if (temp_path.empty() || temp_fd < 0) return false;

    bool ok = true;
    ssize_t remaining = static_cast<ssize_t>(content.size());
    const char* cursor = content.data();
    while (remaining > 0) {
        ssize_t written = write(temp_fd, cursor, static_cast<size_t>(remaining));
        if (written < 0) {
            ok = false;
            break;
        }
        remaining -= written;
        cursor += written;
    }

    if (ok && fchmod(temp_fd, mode) != 0) ok = false;
    if (ok && fsync(temp_fd) != 0) ok = false;
    close(temp_fd);

    if (!ok) {
        unlink(temp_path.c_str());
        return false;
    }
    if (rename(temp_path.c_str(), target_path.c_str()) != 0) {
        unlink(temp_path.c_str());
        return false;
    }
    return true;
}

bool copy_file_atomic(const std::string& source_path, const std::string& target_path) {
    struct stat st;
    if (stat(source_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
    if (!mkdir_p(path_parent_dir(target_path))) return false;

    int temp_fd = -1;
    std::string temp_path = allocate_sibling_temp_path(target_path, "gpkg-copy", &temp_fd);
    if (temp_path.empty() || temp_fd < 0) return false;

    bool ok = copy_regular_file_contents(source_path, temp_fd);
    if (ok && fchmod(temp_fd, st.st_mode & 07777) != 0) ok = false;
    if (ok && fsync(temp_fd) != 0) ok = false;
    close(temp_fd);

    if (!ok) {
        unlink(temp_path.c_str());
        return false;
    }
    if (rename(temp_path.c_str(), target_path.c_str()) != 0) {
        unlink(temp_path.c_str());
        return false;
    }
    return true;
}

bool copy_path_atomic_no_follow(const std::string& source_path, const std::string& target_path) {
    struct stat st;
    if (lstat(source_path.c_str(), &st) != 0) return false;

    if (S_ISLNK(st.st_mode)) {
        std::string link_target = read_symlink_target(source_path);
        if (link_target.empty() && st.st_size > 0) return false;
        if (!mkdir_p(path_parent_dir(target_path))) return false;

        std::string temp_path = allocate_sibling_temp_path(target_path, "gpkg-copy");
        if (temp_path.empty()) return false;

        if (symlink(link_target.c_str(), temp_path.c_str()) != 0) {
            unlink(temp_path.c_str());
            return false;
        }
        if (rename(temp_path.c_str(), target_path.c_str()) != 0) {
            unlink(temp_path.c_str());
            return false;
        }
        return true;
    }

    if (S_ISDIR(st.st_mode)) {
        return copy_directory_tree_no_follow(source_path, target_path);
    }

    if (!S_ISREG(st.st_mode)) return false;
    return copy_file_atomic(source_path, target_path);
}

std::vector<std::string> load_registered_undo_commands(const std::string& pkg_name) {
    std::vector<std::string> undo_cmds;
    std::string undo_path = get_info_dir() + pkg_name + ".undo";
    std::ifstream undo_f(undo_path);
    if (!undo_f) return undo_cmds;

    std::string line;
    while (std::getline(undo_f, line)) {
        line = trim(line);
        if (!line.empty()) undo_cmds.push_back(line);
    }

    return undo_cmds;
}

bool run_registered_undo_commands_reverse(
    const std::vector<std::string>& undo_cmds,
    const std::string& context,
    bool best_effort = false
) {
    for (auto it = undo_cmds.rbegin(); it != undo_cmds.rend(); ++it) {
        if (run_command(*it) == 0) continue;
        if (best_effort) {
            std::cerr << "W: Undo command failed during " << context << "." << std::endl;
            continue;
        }
        std::cerr << "E: Undo command failed during " << context << "." << std::endl;
        return false;
    }
    return true;
}

bool run_postinst_abort_remove(const std::string& pkg_name, bool best_effort = true) {
    std::string postinst = get_info_dir() + pkg_name + ".postinst";
    if (access(postinst.c_str(), X_OK) != 0) return true;

    int rc = run_path_with_args(postinst, {"abort-remove"});
    if (rc == 0) return true;
    if (best_effort) {
        std::cerr << "W: postinst abort-remove failed for " << pkg_name << "." << std::endl;
        return false;
    }

    std::cerr << "E: postinst abort-remove failed for " << pkg_name << "." << std::endl;
    return false;
}

void rollback_remove_transaction(
    const std::string& pkg_name,
    std::vector<InstallRollbackEntry>& rollback_entries,
    bool runtime_sensitive,
    bool try_abort_remove = true
) {
    rollback_install_changes(rollback_entries);
    if (runtime_sensitive) {
        sync_multiarch_runtime_aliases();
        refresh_linker_cache_if_available();
    }
    if (try_abort_remove) {
        run_postinst_abort_remove(pkg_name, true);
    }
}

bool activate_live_path_from_source(
    const std::string& source_path,
    const std::string& live_full_path,
    const std::string& logical_path,
    std::vector<InstallRollbackEntry>& rollback_entries
) {
    struct stat st;
    if (lstat(source_path.c_str(), &st) != 0) {
        std::cerr << "E: Failed to inspect source path " << source_path << ": "
                  << strerror(errno) << std::endl;
        return false;
    }

    if (!mkdir_p(path_parent_dir(live_full_path))) {
        std::cerr << "E: Failed to create parent directory for " << live_full_path << std::endl;
        return false;
    }

    bool had_existing = false;
    if (!backup_live_path_if_present(live_full_path, logical_path, rollback_entries, &had_existing)) {
        return false;
    }

    if (S_ISDIR(st.st_mode)) {
        if (mkdir(live_full_path.c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) {
            std::cerr << "E: Failed to restore directory " << live_full_path << ": "
                      << strerror(errno) << std::endl;
            return false;
        }
        chmod(live_full_path.c_str(), st.st_mode & 07777);
        if (!had_existing) {
            rollback_entries.push_back({logical_path, live_full_path, "", true});
        }
        return true;
    }

    if (S_ISLNK(st.st_mode)) {
        std::vector<char> target(static_cast<size_t>(st.st_size) + 2, '\0');
        ssize_t len = readlink(source_path.c_str(), target.data(), target.size() - 1);
        if (len < 0) {
            std::cerr << "E: Failed to read source symlink " << source_path << ": "
                      << strerror(errno) << std::endl;
            return false;
        }
        target[static_cast<size_t>(len)] = '\0';

        std::string temp_path = allocate_sibling_temp_path(live_full_path, "gpkg-restore");
        if (temp_path.empty()) {
            std::cerr << "E: Failed to reserve temporary restore path for " << logical_path << std::endl;
            return false;
        }

        if (symlink(target.data(), temp_path.c_str()) != 0) {
            std::cerr << "E: Failed to stage restored symlink " << logical_path << ": "
                      << strerror(errno) << std::endl;
            unlink(temp_path.c_str());
            return false;
        }
        if (rename(temp_path.c_str(), live_full_path.c_str()) != 0) {
            std::cerr << "E: Failed to activate restored symlink " << logical_path << ": "
                      << strerror(errno) << std::endl;
            unlink(temp_path.c_str());
            return false;
        }
        if (!had_existing) {
            rollback_entries.push_back({logical_path, live_full_path, "", true});
        }
        return true;
    }

    if (!S_ISREG(st.st_mode)) {
        std::cerr << "E: Unsupported restore source type for " << source_path << std::endl;
        return false;
    }

    int temp_fd = -1;
    std::string temp_path = allocate_sibling_temp_path(live_full_path, "gpkg-restore", &temp_fd);
    if (temp_path.empty() || temp_fd < 0) {
        std::cerr << "E: Failed to reserve temporary restore path for " << logical_path << std::endl;
        return false;
    }

    bool ok = copy_regular_file_contents(source_path, temp_fd);
    if (ok && fchmod(temp_fd, st.st_mode & 07777) != 0) {
        std::cerr << "E: Failed to restore file permissions for " << logical_path << ": "
                  << strerror(errno) << std::endl;
        ok = false;
    }
    close(temp_fd);

    if (!ok) {
        unlink(temp_path.c_str());
        return false;
    }
    if (rename(temp_path.c_str(), live_full_path.c_str()) != 0) {
        std::cerr << "E: Failed to activate restored file " << logical_path << ": "
                  << strerror(errno) << std::endl;
        unlink(temp_path.c_str());
        return false;
    }
    if (!had_existing) {
        rollback_entries.push_back({logical_path, live_full_path, "", true});
    }
    return true;
}

void sort_paths_for_removal(std::vector<std::string>& paths) {
    std::sort(paths.begin(), paths.end(), [](const std::string& left, const std::string& right) {
        size_t left_depth = path_depth(left);
        size_t right_depth = path_depth(right);
        if (left_depth != right_depth) return left_depth > right_depth;
        if (left.size() != right.size()) return left.size() > right.size();
        return left > right;
    });
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
}

bool stage_owned_path_removal(
    const std::string& abs_path,
    std::vector<InstallRollbackEntry>& rollback_entries
) {
    std::string safe_abs = (!abs_path.empty() && abs_path[0] != '/') ? "/" + abs_path : abs_path;
    std::string live_full_path = g_root_prefix + safe_abs;

    struct stat st;
    if (lstat(live_full_path.c_str(), &st) != 0) {
        if (errno == ENOENT) return true;
        std::cerr << "W: Failed to stat " << live_full_path << ": " << strerror(errno) << std::endl;
        return false;
    }

    if (S_ISDIR(st.st_mode)) {
        errno = 0;
        DIR* d = opendir(live_full_path.c_str());
        if (!d) {
            std::cerr << "W: Could not inspect directory for safe removal: " << live_full_path
                      << " (" << strerror(errno) << ")" << std::endl;
            return true;
        }

        bool has_entries = false;
        struct dirent* dir;
        while ((dir = readdir(d)) != NULL) {
            if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) continue;
            has_entries = true;
            break;
        }
        int readdir_errno = errno;
        closedir(d);

        if (readdir_errno != 0) {
            std::cerr << "W: Failed while reading directory " << live_full_path
                      << ": " << strerror(readdir_errno) << std::endl;
            return true;
        }
        if (has_entries) {
            VLOG("Skipping removal of non-empty directory: " << live_full_path);
            return true;
        }
    } else if (S_ISLNK(st.st_mode)) {
        struct stat target_st;
        if (stat(live_full_path.c_str(), &target_st) == 0 && S_ISDIR(target_st.st_mode)) {
            VLOG("Skipping removal of directory symlink: " << live_full_path);
            return true;
        }
    }

    return backup_live_path_if_present(live_full_path, safe_abs, rollback_entries);
}

bool stage_replaced_system_restore(
    const std::string& pkg_name,
    std::vector<InstallRollbackEntry>& rollback_entries
) {
    std::vector<ReplacedSystemFile> entries = load_replaced_system_files(pkg_name);
    for (const auto& entry : entries) {
        if (!path_exists_no_follow(entry.backup_path)) {
            std::cerr << "E: Missing saved base file backup for " << entry.path << std::endl;
            return false;
        }
        if (!activate_live_path_from_source(
                entry.backup_path,
                g_root_prefix + entry.path,
                entry.path,
                rollback_entries)) {
            return false;
        }
    }
    return true;
}

bool stage_package_metadata_removal(
    const std::string& pkg_name,
    std::vector<InstallRollbackEntry>& rollback_entries,
    bool keep_for_config_files = false
) {
    std::vector<std::string> metadata_paths = {
        get_info_dir() + pkg_name + ".list",
        get_info_dir() + pkg_name + ".json",
        get_conffile_manifest_path(pkg_name),
        get_info_dir() + pkg_name + ".undo",
        get_info_dir() + pkg_name + ".preinst",
        get_info_dir() + pkg_name + ".postinst",
        get_info_dir() + pkg_name + ".prerm",
        get_info_dir() + pkg_name + ".postrm",
        get_replaced_system_manifest(pkg_name),
        get_replaced_system_dir(pkg_name)
    };

    std::set<std::string> keep_paths;
    if (keep_for_config_files) {
        keep_paths.insert(get_info_dir() + pkg_name + ".json");
        keep_paths.insert(get_conffile_manifest_path(pkg_name));
        keep_paths.insert(get_info_dir() + pkg_name + ".postrm");
    }

    for (const auto& full_path : metadata_paths) {
        if (keep_paths.count(full_path) != 0) continue;
        if (!backup_live_path_if_present(full_path, full_path, rollback_entries)) {
            return false;
        }
    }

    return true;
}

bool action_remove_safe(const std::string& pkg_name) {
    std::cout << "Removing " << pkg_name << "..." << std::endl;

    PackageStatusRollbackGuard status_guard;
    status_guard.begin(pkg_name);
    std::string current_version = !status_guard.record.version.empty()
        ? status_guard.record.version
        : get_package_version(pkg_name);

    std::string prerm = get_info_dir() + pkg_name + ".prerm";
    if (access(prerm.c_str(), X_OK) == 0) {
        if (run_path_with_args(prerm, {"remove"}) != 0) {
            run_postinst_abort_remove(pkg_name, true);
            std::cerr << "E: prerm script failed." << std::endl;
            return false;
        }
    }

    if (!set_package_status_record(pkg_name, "deinstall", "ok", "half-installed", current_version)) {
        std::cerr << "E: Failed to update package status before removal." << std::endl;
        return false;
    }

    std::vector<std::string> undo_cmds = load_registered_undo_commands(pkg_name);

    std::vector<std::string> owned_files = normalize_owned_manifest_paths(read_list_file(pkg_name));
    bool kernel_payload = file_list_contains_kernel_payload(owned_files);
    std::string kernel_release = kernel_release_from_file_list(owned_files);
    std::string kernel_image_path = kernel_image_path_for_release(kernel_release);
    std::vector<std::string> conffiles = normalize_owned_manifest_paths(load_package_conffiles(pkg_name));
    std::set<std::string> conffile_set(conffiles.begin(), conffiles.end());
    bool runtime_sensitive = files_touch_runtime_linker_state(owned_files);
    bool selinux_policy_touched = file_list_touches_selinux_policy_store(owned_files);
    std::vector<std::string> selinux_relabel_paths = owned_files;
    for (const auto& entry : load_replaced_system_files(pkg_name)) {
        selinux_relabel_paths.push_back(entry.path);
    }
    sort_paths_for_removal(owned_files);

    std::vector<InstallRollbackEntry> removal_rollback_entries;
    if (kernel_payload && !stage_kernel_boot_symlink_transaction(removal_rollback_entries)) {
        rollback_remove_transaction(pkg_name, removal_rollback_entries, runtime_sensitive);
        std::cerr << "E: Failed to stage /boot/kernel rollback before removal." << std::endl;
        return false;
    }
    for (const auto& path : owned_files) {
        if (conffile_set.count(path) != 0) {
            VLOG("Keeping conffile during remove: " << path);
            continue;
        }
        if (!stage_owned_path_removal(path, removal_rollback_entries)) {
            rollback_remove_transaction(pkg_name, removal_rollback_entries, runtime_sensitive);
            std::cerr << "E: Failed while staging removal of " << path << std::endl;
            return false;
        }
    }

    if (!stage_replaced_system_restore(pkg_name, removal_rollback_entries)) {
        rollback_remove_transaction(pkg_name, removal_rollback_entries, runtime_sensitive);
        std::cerr << "E: Failed to restore replaced system files safely." << std::endl;
        return false;
    }

    if (!undo_cmds.empty()) {
        VLOG("Executing " << undo_cmds.size() << " registered undo commands...");
        if (!run_registered_undo_commands_reverse(undo_cmds, "removal")) {
            rollback_remove_transaction(pkg_name, removal_rollback_entries, runtime_sensitive);
            return false;
        }
    }

    std::string conffiles_path = get_conffile_manifest_path(pkg_name);
    if (!prepare_path_for_transaction_write(conffiles_path, conffiles_path, removal_rollback_entries) ||
        !write_package_conffiles(pkg_name, conffiles)) {
        rollback_remove_transaction(pkg_name, removal_rollback_entries, runtime_sensitive);
        std::cerr << "E: Failed to preserve conffile metadata for " << pkg_name << "." << std::endl;
        return false;
    }

    std::string postrm = get_info_dir() + pkg_name + ".postrm";
    if (access(postrm.c_str(), X_OK) == 0) {
        if (run_path_with_args(postrm, {"remove"}) != 0) {
            rollback_remove_transaction(pkg_name, removal_rollback_entries, runtime_sensitive);
            std::cerr << "E: postrm script failed." << std::endl;
            return false;
        }
    }

    if (!stage_package_metadata_removal(pkg_name, removal_rollback_entries, true)) {
        rollback_remove_transaction(pkg_name, removal_rollback_entries, runtime_sensitive, false);
        std::cerr << "E: Failed to remove package metadata safely." << std::endl;
        return false;
    }

    if (kernel_payload) {
        if (!sync_kernel_boot_symlink()) {
            rollback_remove_transaction(pkg_name, removal_rollback_entries, runtime_sensitive, false);
            std::cerr << "E: Failed to update /boot/kernel after removing " << pkg_name << "." << std::endl;
            return false;
        }
        if (!run_depmod_for_kernel_release(kernel_release, true)) {
            rollback_remove_transaction(pkg_name, removal_rollback_entries, runtime_sensitive, false);
            std::cerr << "E: depmod failed after removing kernel " << kernel_release << "." << std::endl;
            return false;
        }
        if (!run_kernel_hook_directories("postrm", kernel_release, kernel_image_path, {"remove"})) {
            rollback_remove_transaction(pkg_name, removal_rollback_entries, runtime_sensitive, false);
            std::cerr << "E: Kernel postrm hooks failed for " << pkg_name << "." << std::endl;
            return false;
        }
    }

    sync_multiarch_runtime_aliases();
    if (!finalize_runtime_linker_state_for_success(runtime_sensitive)) {
        rollback_remove_transaction(pkg_name, removal_rollback_entries, runtime_sensitive, false);
        std::cerr << "E: ldconfig failed after removing runtime files for "
                  << pkg_name << "." << std::endl;
        return false;
    }

    std::string selinux_error;
    if (!finalize_selinux_relabel_for_success(selinux_relabel_paths, &selinux_error)) {
        rollback_remove_transaction(pkg_name, removal_rollback_entries, runtime_sensitive, false);
        std::cerr << "E: " << selinux_error << std::endl;
        return false;
    }
    if (selinux_policy_touched &&
        !schedule_selinux_autorelabel(removal_rollback_entries, &selinux_error)) {
        rollback_remove_transaction(pkg_name, removal_rollback_entries, runtime_sensitive, false);
        std::cerr << "E: " << selinux_error << std::endl;
        return false;
    }

    if (!set_package_status_record(pkg_name, "deinstall", "ok", "config-files", current_version)) {
        rollback_remove_transaction(pkg_name, removal_rollback_entries, runtime_sensitive, false);
        std::cerr << "E: Failed to finalize package status after removal." << std::endl;
        return false;
    }

    invalidate_installed_manifest_snapshot();
    discard_install_backups(removal_rollback_entries);
    status_guard.commit();

    std::cout << "✓ Removed " << pkg_name << std::endl;
    return true;
}

bool action_purge_safe(const std::string& pkg_name) {
    std::cout << "Purging " << pkg_name << "..." << std::endl;

    PackageStatusRollbackGuard status_guard;
    status_guard.begin(pkg_name);
    if (status_guard.had_record &&
        status_guard.record.status != "config-files" &&
        status_guard.record.status != "not-installed") {
        std::cerr << "E: Package " << pkg_name
                  << " is still installed. Remove it before purging." << std::endl;
        return false;
    }

    std::vector<std::string> conffiles = normalize_owned_manifest_paths(load_package_conffiles(pkg_name));
    bool selinux_policy_touched = file_list_touches_selinux_policy_store(conffiles);
    sort_paths_for_removal(conffiles);

    std::vector<InstallRollbackEntry> purge_rollback_entries;
    for (const auto& path : conffiles) {
        if (!stage_owned_path_removal(path, purge_rollback_entries)) {
            rollback_install_changes(purge_rollback_entries);
            std::cerr << "E: Failed while purging conffile " << path << std::endl;
            return false;
        }
    }

    std::string postrm = get_info_dir() + pkg_name + ".postrm";
    if (access(postrm.c_str(), X_OK) == 0) {
        if (run_path_with_args(postrm, {"purge"}) != 0) {
            rollback_install_changes(purge_rollback_entries);
            std::cerr << "E: postrm purge script failed." << std::endl;
            return false;
        }
    }

    if (!stage_package_metadata_removal(pkg_name, purge_rollback_entries, false)) {
        rollback_install_changes(purge_rollback_entries);
        std::cerr << "E: Failed to purge package metadata safely." << std::endl;
        return false;
    }

    std::string selinux_error;
    if (selinux_policy_touched &&
        !schedule_selinux_autorelabel(purge_rollback_entries, &selinux_error)) {
        rollback_install_changes(purge_rollback_entries);
        std::cerr << "E: " << selinux_error << std::endl;
        return false;
    }

    if (!erase_package_status_record(pkg_name)) {
        rollback_install_changes(purge_rollback_entries);
        std::cerr << "E: Failed to finalize package status after purge." << std::endl;
        return false;
    }

    invalidate_installed_manifest_snapshot();
    discard_install_backups(purge_rollback_entries);
    status_guard.commit();

    std::cout << "✓ Purged " << pkg_name << std::endl;
    return true;
}

bool action_retire_safe(const std::string& pkg_name) {
    std::cout << "Retiring " << pkg_name << "..." << std::endl;

    PackageStatusRollbackGuard status_guard;
    status_guard.begin(pkg_name);
    std::string current_version = !status_guard.record.version.empty()
        ? status_guard.record.version
        : get_package_version(pkg_name);

    if (!set_package_status_record(pkg_name, "deinstall", "ok", "half-installed", current_version)) {
        std::cerr << "E: Failed to update package status before retirement." << std::endl;
        return false;
    }

    std::vector<std::string> owned_files = normalize_owned_manifest_paths(read_list_file(pkg_name));
    bool runtime_sensitive = files_touch_runtime_linker_state(owned_files);
    bool selinux_policy_touched = file_list_touches_selinux_policy_store(owned_files);
    sort_paths_for_removal(owned_files);

    std::vector<InstallRollbackEntry> rollback_entries;
    for (const auto& path : owned_files) {
        if (!find_file_owner(pkg_name, path).empty()) continue;
        if (!stage_owned_path_removal(path, rollback_entries)) {
            rollback_install_changes(rollback_entries);
            std::cerr << "E: Failed while retiring " << path << std::endl;
            return false;
        }
    }

    if (!stage_package_metadata_removal(pkg_name, rollback_entries)) {
        rollback_install_changes(rollback_entries);
        std::cerr << "E: Failed to retire package metadata safely." << std::endl;
        return false;
    }

    sync_multiarch_runtime_aliases();
    if (!finalize_runtime_linker_state_for_success(runtime_sensitive)) {
        rollback_install_changes(rollback_entries);
        sync_multiarch_runtime_aliases();
        refresh_linker_cache_if_available();
        std::cerr << "E: ldconfig failed after retiring runtime files for "
                  << pkg_name << "." << std::endl;
        return false;
    }

    std::string selinux_error;
    if (!finalize_selinux_relabel_for_success(owned_files, &selinux_error)) {
        rollback_install_changes(rollback_entries);
        if (runtime_sensitive) {
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
        }
        std::cerr << "E: " << selinux_error << std::endl;
        return false;
    }
    if (selinux_policy_touched &&
        !schedule_selinux_autorelabel(rollback_entries, &selinux_error)) {
        rollback_install_changes(rollback_entries);
        if (runtime_sensitive) {
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
        }
        std::cerr << "E: " << selinux_error << std::endl;
        return false;
    }

    if (!erase_package_status_record(pkg_name)) {
        rollback_install_changes(rollback_entries);
        if (runtime_sensitive) {
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
        }
        std::cerr << "E: Failed to finalize package status after retirement." << std::endl;
        return false;
    }

    invalidate_installed_manifest_snapshot();
    discard_install_backups(rollback_entries);
    status_guard.commit();

    std::cout << "✓ Retired " << pkg_name << std::endl;
    return true;
}

bool build_staged_install_entries(
    const std::vector<std::string>& new_files,
    const std::string& payload_root,
    std::vector<StagedInstallEntry>& entries
) {
    entries.clear();
    if (new_files.empty()) return true;

    const size_t worker_count = parallel_worker_count_for_tasks(new_files.size());
    VLOG("Inspecting staged payload with up to " << worker_count << " worker(s).");

    std::vector<StagedInstallEntry> discovered(new_files.size());
    std::vector<unsigned char> present(new_files.size(), 0);
    std::atomic<size_t> next_index{0};
    std::atomic<bool> failed{false};
    std::string error_message;
    std::mutex error_mutex;

    auto worker = [&]() {
        while (!failed.load(std::memory_order_relaxed)) {
            size_t index = next_index.fetch_add(1);
            if (index >= new_files.size()) return;

            const std::string& path = new_files[index];
            std::string staged_path = payload_root + path;
            struct stat st {};
            if (lstat(staged_path.c_str(), &st) != 0) {
                if (errno == ENOENT) continue;
                std::lock_guard<std::mutex> lock(error_mutex);
                if (error_message.empty()) {
                    error_message = "E: Failed to inspect staged payload entry " + staged_path +
                        ": " + std::string(strerror(errno));
                }
                failed.store(true, std::memory_order_relaxed);
                return;
            }

            StagedInstallEntry entry;
            entry.path = path;
            entry.staged_path = staged_path;
            entry.is_directory = S_ISDIR(st.st_mode);
            entry.is_symlink = S_ISLNK(st.st_mode);
            entry.mode = st.st_mode;
            entry.depth = path_depth(path);

            if (entry.is_symlink) {
                std::vector<char> target(static_cast<size_t>(st.st_size) + 2, '\0');
                ssize_t len = readlink(staged_path.c_str(), target.data(), target.size() - 1);
                if (len < 0) {
                    std::lock_guard<std::mutex> lock(error_mutex);
                    if (error_message.empty()) {
                        error_message = "E: Failed to read staged symlink " + staged_path +
                            ": " + std::string(strerror(errno));
                    }
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
                target[static_cast<size_t>(len)] = '\0';
                entry.symlink_target.assign(target.data(), static_cast<size_t>(len));
            }

            if (!entry.is_directory && !entry.is_symlink && !S_ISREG(st.st_mode)) {
                std::lock_guard<std::mutex> lock(error_mutex);
                if (error_message.empty()) {
                    error_message = "E: Unsupported staged payload entry type for " + path;
                }
                failed.store(true, std::memory_order_relaxed);
                return;
            }

            discovered[index] = std::move(entry);
            present[index] = 1;
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count > 0 ? worker_count - 1 : 0);
    for (size_t worker_index = 1; worker_index < worker_count; ++worker_index) {
        workers.emplace_back(worker);
    }
    worker();
    for (auto& thread : workers) {
        thread.join();
    }

    if (failed.load(std::memory_order_relaxed)) {
        if (!error_message.empty()) std::cerr << error_message << std::endl;
        return false;
    }

    entries.reserve(new_files.size());
    for (size_t index = 0; index < discovered.size(); ++index) {
        if (present[index] == 0) continue;
        entries.push_back(std::move(discovered[index]));
    }

    std::sort(entries.begin(), entries.end(), [](const StagedInstallEntry& left, const StagedInstallEntry& right) {
        if (left.depth != right.depth) return left.depth < right.depth;
        if (left.is_directory != right.is_directory) return left.is_directory && !right.is_directory;
        return left.path < right.path;
    });

    return true;
}

void rollback_install_changes(const std::vector<InstallRollbackEntry>& rollback_entries) {
    for (auto it = rollback_entries.rbegin(); it != rollback_entries.rend(); ++it) {
        if (it->created_only) remove_tree_no_follow(it->live_full_path);
        else remove_live_path_exact(it->live_full_path);
        if (!it->backup_full_path.empty()) {
            if (rename(it->backup_full_path.c_str(), it->live_full_path.c_str()) != 0) {
                std::cerr << "W: Failed to restore backup for " << it->path << ": "
                          << strerror(errno) << std::endl;
            }
        }
    }
}

void discard_install_backups(const std::vector<InstallRollbackEntry>& rollback_entries) {
    for (const auto& entry : rollback_entries) {
        if (entry.backup_full_path.empty()) continue;
        if (!remove_tree_no_follow(entry.backup_full_path) && errno != ENOENT) {
            std::cerr << "W: Failed to discard backup path " << entry.backup_full_path
                      << ": " << strerror(errno) << std::endl;
        }
    }
}

bool apply_staged_install_entries(
    const std::vector<StagedInstallEntry>& entries,
    std::vector<InstallRollbackEntry>& rollback_entries
) {
    for (const auto& entry : entries) {
        std::string live_full_path = g_root_prefix + entry.path;

        if (entry.is_directory) {
            struct stat existing_st;
            if (lstat(live_full_path.c_str(), &existing_st) == 0) {
                if (S_ISDIR(existing_st.st_mode)) {
                    chmod(live_full_path.c_str(), entry.mode & 07777);
                    continue;
                }
                if (S_ISLNK(existing_st.st_mode)) {
                    struct stat target_st;
                    if (stat(live_full_path.c_str(), &target_st) == 0 && S_ISDIR(target_st.st_mode)) {
                        continue;
                    }
                }
            }

            if (!mkdir_p(path_parent_dir(live_full_path))) {
                std::cerr << "E: Failed to create parent directory for " << entry.path << std::endl;
                return false;
            }

            bool had_existing = false;
            if (!backup_live_path_if_present(live_full_path, entry.path, rollback_entries, &had_existing)) {
                return false;
            }

            if (mkdir(live_full_path.c_str(), entry.mode & 07777) != 0 && errno != EEXIST) {
                std::cerr << "E: Failed to create directory " << live_full_path << ": "
                          << strerror(errno) << std::endl;
                return false;
            }
            chmod(live_full_path.c_str(), entry.mode & 07777);
            if (!had_existing) {
                rollback_entries.push_back({entry.path, live_full_path, "", true});
            }
            continue;
        }

        if (!mkdir_p(path_parent_dir(live_full_path))) {
            std::cerr << "E: Failed to create parent directory for " << entry.path << std::endl;
            return false;
        }

        bool had_existing = false;
        if (!backup_live_path_if_present(live_full_path, entry.path, rollback_entries, &had_existing)) {
            return false;
        }

        if (entry.is_symlink) {
            std::string temp_path = allocate_sibling_temp_path(live_full_path, "gpkg-install");
            if (temp_path.empty()) {
                std::cerr << "E: Failed to reserve temporary symlink path for " << entry.path << std::endl;
                return false;
            }

            if (symlink(entry.symlink_target.c_str(), temp_path.c_str()) != 0) {
                std::cerr << "E: Failed to stage symlink " << entry.path << ": "
                          << strerror(errno) << std::endl;
                unlink(temp_path.c_str());
                return false;
            }

            if (rename(temp_path.c_str(), live_full_path.c_str()) != 0) {
                std::cerr << "E: Failed to activate symlink " << entry.path << ": "
                          << strerror(errno) << std::endl;
                unlink(temp_path.c_str());
                return false;
            }
        } else {
            int temp_fd = -1;
            std::string temp_path = allocate_sibling_temp_path(live_full_path, "gpkg-install", &temp_fd);
            if (temp_path.empty() || temp_fd < 0) {
                std::cerr << "E: Failed to reserve temporary file path for " << entry.path << std::endl;
                return false;
            }

            bool copied = copy_regular_file_contents(entry.staged_path, temp_fd);
            if (copied && fchmod(temp_fd, entry.mode & 07777) != 0) {
                std::cerr << "E: Failed to set file mode for " << entry.path << ": "
                          << strerror(errno) << std::endl;
                copied = false;
            }
            close(temp_fd);

            if (!copied) {
                unlink(temp_path.c_str());
                return false;
            }

            if (rename(temp_path.c_str(), live_full_path.c_str()) != 0) {
                std::cerr << "E: Failed to activate file " << entry.path << ": "
                          << strerror(errno) << std::endl;
                unlink(temp_path.c_str());
                return false;
            }
        }

        if (!had_existing) {
            rollback_entries.push_back({entry.path, live_full_path, "", true});
        }
    }

    return true;
}

bool verify_staged_install_entries(
    const std::vector<StagedInstallEntry>& entries,
    std::vector<std::string>& issues
) {
    issues.clear();
    std::set<std::tuple<std::string, std::string, std::string>> runtime_alias_candidates;
    if (!entries.empty()) {
        const size_t worker_count = parallel_worker_count_for_tasks(entries.size());
        VLOG("Verifying installed payload with up to " << worker_count << " worker(s).");

        std::atomic<size_t> next_index{0};
        std::vector<std::vector<std::string>> worker_issues(worker_count);
        std::vector<std::set<std::tuple<std::string, std::string, std::string>>> worker_aliases(worker_count);

        auto worker = [&](size_t worker_index) {
            auto& local_issues = worker_issues[worker_index];
            auto& local_aliases = worker_aliases[worker_index];

            while (true) {
                size_t index = next_index.fetch_add(1);
                if (index >= entries.size()) return;

                const auto& entry = entries[index];
                std::string live_full_path = g_root_prefix + entry.path;
                struct stat live_st {};
                if (lstat(live_full_path.c_str(), &live_st) != 0) {
                    local_issues.push_back(entry.path + ": installed path is missing");
                    continue;
                }

                if (entry.is_directory) {
                    if (S_ISDIR(live_st.st_mode)) {
                        std::string active_prefix;
                        std::string compat_prefix;
                        std::string name;
                        if (runtime_alias_pair_for_path(entry.path, &active_prefix, &compat_prefix, &name)) {
                            local_aliases.insert(std::make_tuple(active_prefix, compat_prefix, name));
                        }
                        continue;
                    }
                    if (S_ISLNK(live_st.st_mode) && is_existing_symlink_directory(live_full_path)) continue;
                    local_issues.push_back(entry.path + ": expected directory after install");
                    continue;
                }

                if (entry.is_symlink) {
                    if (!S_ISLNK(live_st.st_mode)) {
                        local_issues.push_back(entry.path + ": expected symlink after install");
                        continue;
                    }
                    std::string live_target = read_symlink_target(live_full_path);
                    if (!runtime_symlink_target_equivalent(entry.path, entry.symlink_target, live_target)) {
                        local_issues.push_back(entry.path + ": symlink target differs from staged payload");
                        continue;
                    }
                } else {
                    if (!S_ISREG(live_st.st_mode)) {
                        local_issues.push_back(entry.path + ": expected regular file after install");
                        continue;
                    }

                    std::string compare_error;
                    if (!compare_regular_files_exact(entry.staged_path, live_full_path, &compare_error)) {
                        local_issues.push_back(entry.path + ": " + compare_error);
                        continue;
                    }

                    std::string elf_error;
                    if (!validate_elf_file(live_full_path, live_st.st_size, &elf_error)) {
                        local_issues.push_back(entry.path + ": " + elf_error);
                        continue;
                    }
                }

                std::string active_prefix;
                std::string compat_prefix;
                std::string name;
                if (runtime_alias_pair_for_path(entry.path, &active_prefix, &compat_prefix, &name)) {
                    local_aliases.insert(std::make_tuple(active_prefix, compat_prefix, name));
                }
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(worker_count > 0 ? worker_count - 1 : 0);
        for (size_t worker_index = 1; worker_index < worker_count; ++worker_index) {
            workers.emplace_back(worker, worker_index);
        }
        worker(0);
        for (auto& thread : workers) {
            thread.join();
        }

        for (size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
            issues.insert(
                issues.end(),
                worker_issues[worker_index].begin(),
                worker_issues[worker_index].end()
            );
            runtime_alias_candidates.insert(
                worker_aliases[worker_index].begin(),
                worker_aliases[worker_index].end()
            );
        }
    }

    for (const auto& candidate : runtime_alias_candidates) {
        const std::string& active_prefix = std::get<0>(candidate);
        const std::string& compat_prefix = std::get<1>(candidate);
        const std::string& name = std::get<2>(candidate);

        std::string active_path = g_root_prefix + active_prefix + "/" + name;
        std::string compat_path = g_root_prefix + compat_prefix + "/" + name;
        if (!path_exists_no_follow(active_path)) {
            issues.push_back(active_prefix + "/" + name + ": runtime alias source is missing");
            continue;
        }
        if (!path_exists_no_follow(compat_path)) {
            issues.push_back(compat_prefix + "/" + name + ": runtime alias is missing");
            continue;
        }

        std::string active_real = canonical_existing_path(active_path);
        std::string compat_real = canonical_existing_path(compat_path);
        if (active_real.empty() || compat_real.empty() || active_real != compat_real) {
            issues.push_back(active_prefix + "/" + name + ": runtime alias does not resolve consistently");
            continue;
        }

        struct stat resolved_st;
        if (stat(active_real.c_str(), &resolved_st) == 0 && S_ISREG(resolved_st.st_mode)) {
            std::string elf_error;
            if (!validate_elf_file(active_real, resolved_st.st_size, &elf_error)) {
                issues.push_back(active_prefix + "/" + name + ": " + elf_error);
            }
        }
    }

    return issues.empty();
}

std::vector<std::string> get_staged_replaces() {
    std::vector<std::string> replaced;
    std::ifstream f(g_tmp_extract_path + "control.json");
    if (!f) return replaced;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t key_pos = content.find("\"replaces\"");
    if (key_pos == std::string::npos) return replaced;
    size_t arr_start = content.find("[", key_pos);
    if (arr_start == std::string::npos) return replaced;
    size_t arr_end = content.find("]", arr_start);
    if (arr_end == std::string::npos) return replaced;
    
    std::string arr_content = content.substr(arr_start + 1, arr_end - arr_start - 1);
    std::istringstream iss(arr_content);
    std::string token;
    while (std::getline(iss, token, ',')) {
        size_t q1 = token.find("\"");
        if (q1 == std::string::npos) continue;
        size_t q2 = token.find("\"", q1 + 1);
        if (q2 == std::string::npos) continue;
        replaced.push_back(token.substr(q1 + 1, q2 - q1 - 1));
    }
    return replaced;
}

bool check_collisions(const std::string& pkg_name, const std::vector<std::string>& new_files) {
    // 1. Get current package's file list (for upgrades)
    std::set<std::string> owned_by_me = build_normalized_owned_path_set(read_list_file(pkg_name));

    std::vector<std::string> collisions;
    std::map<std::string, std::string> owner_by_collision;
    std::map<std::string, std::string> base_owner_by_collision;

    for (const auto& file : new_files) {
        std::string canonical_file = canonical_multiarch_logical_path(file);
        std::string full_path = g_root_prefix + canonical_file;
        if (access(full_path.c_str(), F_OK) == 0) {
             struct stat st;
             if (stat(full_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) continue;
             if (should_preserve_local_config_file(pkg_name, canonical_file)) continue;
             if (owned_by_me.count(canonical_file)) continue;
             
             // Special case: Ignore /usr/share/info/dir as it's a shared directory index
             if (canonical_file == "/usr/share/info/dir") continue;

             collisions.push_back(canonical_file);
             owner_by_collision.emplace(canonical_file, find_cached_file_owner(pkg_name, canonical_file));
             base_owner_by_collision.emplace(canonical_file, find_cached_base_file_owner(canonical_file));
        }
    }

    if (collisions.empty()) return true;

    bool fatal = false;
    std::vector<std::string> replaced = get_staged_replaces();
    bool import_like_adoption = owned_by_me.empty() &&
        !new_files.empty() &&
        collisions.size() * 2 >= new_files.size();
    size_t same_package_base_takeovers = 0;
    std::map<std::string, size_t> base_takeovers_by_owner;
    size_t unmanaged_adoption_count = 0;
    
    for (const auto& col : collisions) {
        std::string owner;
        auto owner_it = owner_by_collision.find(col);
        if (owner_it != owner_by_collision.end()) owner = owner_it->second;
        std::string base_owner;
        auto base_owner_it = base_owner_by_collision.find(col);
        if (base_owner_it != base_owner_by_collision.end()) base_owner = base_owner_it->second;

        if (!owner.empty()) {
            if (std::find(replaced.begin(), replaced.end(), owner) != replaced.end()) {
                std::cout << "W: Permitted overwrite of " << col << " because "
                          << pkg_name << " replaces " << owner << std::endl;
                continue;
            }
            std::cerr << "E: Conflict: " << col << " is owned by " << owner << std::endl;
            fatal = true;
            continue;
        }

        if (!base_owner.empty()) {
            if (base_owner == pkg_name) {
                ++same_package_base_takeovers;
            } else {
                ++base_takeovers_by_owner[base_owner];
            }
            continue;
        }

        if (import_like_adoption) {
            ++unmanaged_adoption_count;
            continue;
        }

        std::cerr << "W: Overwriting unowned file " << col << std::endl;
    }

    if (unmanaged_adoption_count > 0) {
        std::cout << "W: Adopting " << unmanaged_adoption_count
                  << " existing unmanaged path"
                  << (unmanaged_adoption_count == 1 ? "" : "s")
                  << " while importing " << pkg_name
                  << " into gpkg ownership." << std::endl;
    }
    if (same_package_base_takeovers > 0) {
        VLOG("Adopting " << same_package_base_takeovers
             << " existing base-system path"
             << (same_package_base_takeovers == 1 ? "" : "s")
             << " for " << pkg_name << ".");
    }
    for (const auto& entry : base_takeovers_by_owner) {
        VLOG("Adopting " << entry.second
             << " base-system path"
             << (entry.second == 1 ? "" : "s")
             << " from " << entry.first
             << " while installing " << pkg_name << ".");
    }
    
    return !fatal;
}

// Helper to get version from installed package
std::string get_package_version(const std::string& pkg_name) {
    PackageStatusRecord status_record;
    if (get_package_status_record(pkg_name, &status_record) && !status_record.version.empty()) {
        return status_record.version;
    }

    std::string path = get_info_dir() + pkg_name + ".json";
    std::ifstream f(path);
    if (!f) return "";
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    
    // Quick parse for version
    size_t key_pos = content.find("\"version\"");
    if (key_pos == std::string::npos) return "";
    size_t val_start = content.find("\"", content.find(":", key_pos));
    if (val_start == std::string::npos) return "";
    size_t val_end = content.find("\"", val_start + 1);
    if (val_end == std::string::npos) return "";
    
    return content.substr(val_start + 1, val_end - val_start - 1);
}

std::string to_lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool path_matches_prefix_or_exact(const std::string& path, const std::string& prefix) {
    if (path == prefix) return true;
    return path.rfind(prefix + "/", 0) == 0;
}

bool selinux_config_requests_enabled() {
    std::ifstream f(g_root_prefix + "/etc/selinux/config");
    if (!f) return false;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        if (line.rfind("SELINUX=", 0) != 0) continue;
        return to_lower_ascii(trim(line.substr(std::string("SELINUX=").size()))) != "disabled";
    }

    return false;
}

bool selinux_runtime_active() {
    if (!g_root_prefix.empty()) return false;
    return access("/sys/fs/selinux/enforce", F_OK) == 0;
}

std::string find_live_executable_path(const std::vector<std::string>& candidates) {
    if (!g_root_prefix.empty()) return "";
    for (const auto& candidate : candidates) {
        if (!candidate.empty() && access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return "";
}

bool file_list_touches_selinux_policy_store(const std::vector<std::string>& files) {
    for (const auto& path : files) {
        if (path_matches_prefix_or_exact(path, "/etc/selinux") ||
            path_matches_prefix_or_exact(path, "/usr/share/selinux") ||
            path_matches_prefix_or_exact(path, "/var/lib/selinux")) {
            return true;
        }
    }
    return false;
}

struct SelinuxRelabelTarget {
    std::string full_path;
    bool recursive = false;
};

bool path_is_same_or_descendant_of(const std::string& path, const std::string& ancestor) {
    if (path == ancestor) return true;
    if (ancestor.empty()) return false;
    return path.rfind(ancestor + "/", 0) == 0;
}

std::vector<SelinuxRelabelTarget> existing_selinux_relabel_targets(const std::vector<std::string>& logical_paths) {
    std::vector<SelinuxRelabelTarget> candidates;
    std::set<std::string> seen;

    for (const auto& path : logical_paths) {
        std::string normalized = canonical_multiarch_logical_path(path);
        if (normalized.empty()) continue;

        std::string full_path = g_root_prefix + normalized;
        struct stat st {};
        if (lstat(full_path.c_str(), &st) != 0) continue;

        std::string relabel_target = full_path;
        struct stat relabel_st = st;
        if (S_ISLNK(st.st_mode)) {
            std::string resolved = canonical_existing_path(full_path);
            if (resolved.empty()) {
                VLOG("Skipping SELinux relabel for dangling or unreadable symlink " << normalized);
                continue;
            }
            relabel_target = resolved;
            if (lstat(relabel_target.c_str(), &relabel_st) != 0) continue;
        }

        if (!seen.insert(relabel_target).second) continue;
        candidates.push_back({relabel_target, S_ISDIR(relabel_st.st_mode)});
    }

    std::sort(candidates.begin(), candidates.end(), [](const SelinuxRelabelTarget& left, const SelinuxRelabelTarget& right) {
        if (left.recursive != right.recursive) return left.recursive > right.recursive;
        size_t left_depth = static_cast<size_t>(std::count(left.full_path.begin(), left.full_path.end(), '/'));
        size_t right_depth = static_cast<size_t>(std::count(right.full_path.begin(), right.full_path.end(), '/'));
        if (left_depth != right_depth) return left_depth > right_depth;
        if (left.full_path.size() != right.full_path.size()) return left.full_path.size() > right.full_path.size();
        return left.full_path < right.full_path;
    });

    std::vector<SelinuxRelabelTarget> selected;
    for (const auto& candidate : candidates) {
        bool covered_by_recursive_target = false;
        for (const auto& target : selected) {
            if (!target.recursive) continue;
            if (path_is_same_or_descendant_of(candidate.full_path, target.full_path)) {
                covered_by_recursive_target = true;
                break;
            }
        }
        if (covered_by_recursive_target) continue;

        if (candidate.recursive) {
            bool broader_than_existing_target = false;
            for (const auto& target : selected) {
                if (path_is_same_or_descendant_of(target.full_path, candidate.full_path)) {
                    broader_than_existing_target = true;
                    break;
                }
            }
            if (broader_than_existing_target) continue;
        }

        selected.push_back(candidate);
    }

    return selected;
}

bool relabel_path_with_restorecon(const std::string& restorecon, const std::string& full_path, std::string* error_out) {
    struct stat st;
    if (lstat(full_path.c_str(), &st) != 0) {
        if (errno == ENOENT) return true;
        if (error_out) *error_out = "lstat failed for " + full_path + ": " + strerror(errno);
        return false;
    }

    std::vector<std::string> args = {"-F"};
    if (S_ISDIR(st.st_mode)) {
        args.push_back("-R");
    }
    args.push_back(full_path);

    int rc = run_path_with_args(restorecon, args);
    if (rc == 0) return true;
    if (error_out) {
        *error_out = "restorecon failed for " + full_path + " (exit " + std::to_string(rc) + ")";
    }
    return false;
}

bool restorecon_transaction_paths(const std::vector<std::string>& logical_paths, std::string* error_out) {
    if (error_out) error_out->clear();
    if (!selinux_config_requests_enabled() || !selinux_runtime_active()) return true;

    std::string restorecon = find_live_executable_path({
        "/usr/sbin/restorecon",
        "/sbin/restorecon",
        "/usr/bin/restorecon",
        "/bin/restorecon",
    });
    if (restorecon.empty()) {
        if (error_out) *error_out = "SELinux is enabled but restorecon is not available.";
        return false;
    }

    for (const auto& target : existing_selinux_relabel_targets(logical_paths)) {
        if (!relabel_path_with_restorecon(restorecon, target.full_path, error_out)) {
            return false;
        }
    }

    return true;
}

std::string deferred_selinux_relabel_queue_path() {
    return g_root_prefix + "/var/lib/gpkg/triggers/selinux-relabel.list";
}

bool append_deferred_selinux_relabel_paths(const std::vector<std::string>& logical_paths, std::string* error_out) {
    if (error_out) error_out->clear();
    if (!selinux_config_requests_enabled()) return true;

    std::vector<std::string> normalized = normalize_owned_manifest_paths(logical_paths);
    if (normalized.empty()) return true;

    std::string queue_path = deferred_selinux_relabel_queue_path();
    if (!mkdir_p(path_parent_dir(queue_path))) {
        if (error_out) *error_out = "Failed to prepare deferred SELinux relabel queue directory.";
        return false;
    }

    std::ofstream out(queue_path, std::ios::app);
    if (!out) {
        if (error_out) *error_out = "Failed to append deferred SELinux relabel queue.";
        return false;
    }

    for (const auto& path : normalized) out << path << "\n";
    return out.good();
}

bool finalize_selinux_relabel_for_success(const std::vector<std::string>& logical_paths, std::string* error_out) {
    if (!g_defer_selinux_relabel) return restorecon_transaction_paths(logical_paths, error_out);
    return append_deferred_selinux_relabel_paths(logical_paths, error_out);
}

bool action_refresh_selinux_label_state() {
    std::string queue_path = deferred_selinux_relabel_queue_path();
    std::ifstream in(queue_path);
    if (!in) return true;

    std::vector<std::string> logical_paths;
    std::set<std::string> seen;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty()) continue;
        std::string normalized = canonical_multiarch_logical_path(line);
        if (normalized.empty()) continue;
        if (!seen.insert(normalized).second) continue;
        logical_paths.push_back(normalized);
    }
    in.close();

    std::string error;
    if (!restorecon_transaction_paths(logical_paths, &error)) {
        if (!error.empty()) std::cerr << "E: " << error << std::endl;
        return false;
    }

    if (unlink(queue_path.c_str()) != 0 && errno != ENOENT) {
        std::cerr << "E: Failed to clear deferred SELinux relabel queue: "
                  << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

bool schedule_selinux_autorelabel(
    std::vector<InstallRollbackEntry>& rollback_entries,
    std::string* error_out
) {
    if (error_out) error_out->clear();
    if (!selinux_config_requests_enabled()) return true;

    std::string autorelabel_path = g_root_prefix + "/.autorelabel";
    if (!prepare_path_for_transaction_write(autorelabel_path, "/.autorelabel", rollback_entries) ||
        !write_text_file_atomic(autorelabel_path, "", 0644)) {
        if (error_out) *error_out = "Failed to schedule SELinux autorelabel.";
        return false;
    }

    return true;
}

bool action_install(const std::string& pkg_file) {
    // 1. Unpack to temp
    ScopedExtractWorkspace workspace;
    if (!create_extract_workspace()) {
        std::cerr << "E: Failed to prepare extraction workspace." << std::endl;
        return false;
    }
    workspace.active = true;
    std::string tmp_tar = g_tmp_extract_path + "temp.tar";

    std::string archive_error;
    if (!GpkgArchive::decompress_zstd_file(pkg_file, tmp_tar, &archive_error)) {
        std::cerr << "E: Decompression failed.";
        if (!archive_error.empty()) std::cerr << " " << archive_error;
        std::cerr << std::endl;
        return false;
    }

    if (!GpkgArchive::tar_extract_to_directory(tmp_tar, g_tmp_extract_path, {}, &archive_error)) {
        std::cerr << "E: Package extraction failed.";
        if (!archive_error.empty()) std::cerr << " " << archive_error;
        std::cerr << std::endl;
        return false;
    }
    
    std::string data_tar_zst = g_tmp_extract_path + "data.tar.zst";
    std::string data_tar = g_tmp_extract_path + "data.tar";

    if (!GpkgArchive::decompress_zstd_file(data_tar_zst, data_tar, &archive_error)) {
         std::cerr << "E: Data decompression failed.";
         if (!archive_error.empty()) std::cerr << " " << archive_error;
         std::cerr << std::endl;
         return false;
    }

    // 2. Get File List & Pkg Name
    TarPayloadInspection payload_inspection = inspect_tar_payload(data_tar);
    bool strip_data = payload_inspection.strip_data;
    if (strip_data) VLOG("Detected 'data/' prefix. Will strip components.");
    
    std::vector<std::string> new_files = collapse_multiarch_install_alias_paths(
        payload_inspection.paths);
    bool runtime_sensitive = files_touch_runtime_linker_state(new_files);
    bool selinux_policy_touched = file_list_touches_selinux_policy_store(new_files);
    
    std::string pkg_name;
    std::string new_version;
    std::ifstream control_file(g_tmp_extract_path + "control.json");
    std::string content((std::istreambuf_iterator<char>(control_file)), std::istreambuf_iterator<char>());
    
    // Parse name
    size_t p_pos = content.find("\"package\"");
    if (p_pos != std::string::npos) {
        size_t start = content.find("\"", content.find(":", p_pos)) + 1;
        size_t end = content.find("\"", start);
        pkg_name = content.substr(start, end - start);
    }

    // Parse version
    size_t v_pos = content.find("\"version\"");
    if (v_pos != std::string::npos) {
        size_t start = content.find("\"", content.find(":", v_pos)) + 1;
        size_t end = content.find("\"", start);
        new_version = content.substr(start, end - start);
    }
    
    if (pkg_name.empty()) {
        std::cerr << "E: Could not determine package name." << std::endl;
        return false;
    }

    PackageStatusRollbackGuard status_guard;
    status_guard.begin(pkg_name);

    // 3. Check Collisions & Detect Upgrade
    if (!check_collisions(pkg_name, new_files)) {
        return false;
    }

    bool is_upgrade = false;
    std::string old_version = get_package_version(pkg_name);
    std::set<std::string> old_files_set;
    if (!old_version.empty()) {
        is_upgrade = true;
        old_files_set = build_normalized_owned_path_set(read_list_file(pkg_name));
    }

    if (!set_package_status_record(pkg_name, "install", "ok", "half-installed", new_version)) {
        std::cerr << "E: Failed to record package status before installation." << std::endl;
        return false;
    }

    std::vector<ReplacedSystemFile> replaced_system_files =
        collect_replaced_system_files(pkg_name, new_files, old_files_set);
    std::vector<InstallRollbackEntry> install_rollback_entries;
    bool kernel_payload = file_list_contains_kernel_payload(new_files);
    std::string kernel_release = kernel_release_from_file_list(new_files);
    std::string kernel_image_path = kernel_image_path_for_release(kernel_release);

    // 4. Preinst
    std::string preinst = g_tmp_extract_path + "scripts/preinst";
    if (access(preinst.c_str(), X_OK) == 0) {
        std::vector<std::string> preinst_args = {is_upgrade ? "upgrade" : "install"};
        if (is_upgrade) preinst_args.push_back(old_version);
        if (run_path_with_args(preinst, preinst_args) != 0) {
             std::cerr << "E: preinst failed." << std::endl;
             return false;
        }
    }

    std::vector<PreservedConfigFile> preserved_configs =
        collect_preserved_config_files(pkg_name, new_files);
    if (!backup_preserved_config_files(preserved_configs)) {
        return false;
    }
    if (!prepare_path_for_transaction_write(
            get_replaced_system_dir(pkg_name),
            get_replaced_system_dir(pkg_name),
            install_rollback_entries)) {
        rollback_install_changes(install_rollback_entries);
        return false;
    }
    if (!backup_replaced_system_files(replaced_system_files)) {
        rollback_install_changes(install_rollback_entries);
        return false;
    }
    if (kernel_payload && !stage_kernel_boot_symlink_transaction(install_rollback_entries)) {
        rollback_install_changes(install_rollback_entries);
        std::cerr << "E: Failed to stage /boot/kernel rollback before installation." << std::endl;
        return false;
    }

    // 5. Extract into a staging tree first, then apply entries atomically.
    std::string payload_root = g_tmp_extract_path + "payload";
    remove_tree_no_follow(payload_root);
    if (!mkdir_p(payload_root)) {
        rollback_install_changes(install_rollback_entries);
        std::cerr << "E: Failed to create payload staging directory." << std::endl;
        return false;
    }

    GpkgArchive::TarExtractOptions extract_options;
    extract_options.strip_components = strip_data ? 1 : 0;
    if (!GpkgArchive::tar_extract_to_directory(data_tar, payload_root, extract_options, &archive_error)) {
        rollback_install_changes(install_rollback_entries);
        std::cerr << "E: Extraction failed." << std::endl;
        if (!archive_error.empty()) std::cerr << "E: " << archive_error << std::endl;
        return false;
    }

    std::vector<StagedInstallEntry> staged_entries;
    if (!build_staged_install_entries(new_files, payload_root, staged_entries)) {
        rollback_install_changes(install_rollback_entries);
        return false;
    }

    if (!apply_staged_install_entries(staged_entries, install_rollback_entries)) {
        rollback_install_changes(install_rollback_entries);
        std::cerr << "E: Failed to apply staged filesystem changes safely." << std::endl;
        return false;
    }

    std::vector<std::string> initial_selinux_relabel_paths =
        collect_install_relabel_paths(staged_entries, install_rollback_entries);
    std::vector<std::string> early_selinux_relabel_paths =
        collect_early_install_relabel_paths(staged_entries, install_rollback_entries);

    if (runtime_sensitive) {
        sync_multiarch_runtime_aliases();

        std::vector<std::string> verification_issues;
        if (!verify_staged_install_entries(staged_entries, verification_issues)) {
            rollback_install_changes(install_rollback_entries);
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
            std::cerr << "E: Installed runtime state failed verification after applying "
                      << pkg_name << ":" << std::endl;
            for (const auto& issue : verification_issues) {
                std::cerr << "  - " << issue << std::endl;
            }
            return false;
        }
    }

    if (!finalize_preserved_config_files(preserved_configs)) {
        rollback_install_changes(install_rollback_entries);
        if (runtime_sensitive) {
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
        }
        return false;
    }

    if (!finalize_runtime_linker_state_for_success(runtime_sensitive)) {
        rollback_install_changes(install_rollback_entries);
        sync_multiarch_runtime_aliases();
        refresh_linker_cache_if_available();
        std::cerr << "E: ldconfig failed after installing runtime files for "
                  << pkg_name << "." << std::endl;
        return false;
    }

    std::string selinux_error;
    const std::vector<std::string>& pre_postinst_selinux_paths =
        g_defer_selinux_relabel ? early_selinux_relabel_paths : initial_selinux_relabel_paths;
    if (!restorecon_transaction_paths(pre_postinst_selinux_paths, &selinux_error)) {
        rollback_install_changes(install_rollback_entries);
        if (runtime_sensitive) {
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
        }
        std::cerr << "E: " << selinux_error << std::endl;
        return false;
    }

    std::vector<std::string> installed_files = normalize_owned_manifest_paths(new_files);
    apply_preserved_config_metadata(installed_files, preserved_configs);
    prune_non_owned_directory_symlink_entries(installed_files, staged_entries);
    installed_files = normalize_owned_manifest_paths(installed_files);
    std::vector<std::string> conffiles = collect_package_conffiles_from_entries(new_files);

    // 6. Register in Database
    if (!mkdir_p(get_info_dir())) {
        rollback_install_changes(install_rollback_entries);
        if (runtime_sensitive) {
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
        }
        std::cerr << "E: Failed to prepare package metadata directory." << std::endl;
        return false;
    }

    std::ostringstream list_buffer;
    for (const auto& f : installed_files) {
        list_buffer << f << "\n";
    }
    std::string list_path = get_info_dir() + pkg_name + ".list";
    if (!prepare_path_for_transaction_write(list_path, list_path, install_rollback_entries) ||
        !write_text_file_atomic(list_path, list_buffer.str(), 0644)) {
        rollback_install_changes(install_rollback_entries);
        if (runtime_sensitive) {
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
        }
        std::cerr << "E: Failed to write package file manifest." << std::endl;
        return false;
    }
    
    std::string json_path = get_info_dir() + pkg_name + ".json";
    if (!prepare_path_for_transaction_write(json_path, json_path, install_rollback_entries) ||
        !copy_file_atomic(g_tmp_extract_path + "control.json", json_path)) {
        rollback_install_changes(install_rollback_entries);
        if (runtime_sensitive) {
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
        }
        std::cerr << "E: Failed to write installed package metadata." << std::endl;
        return false;
    }
    std::string conffiles_path = get_conffile_manifest_path(pkg_name);
    if (!prepare_path_for_transaction_write(conffiles_path, conffiles_path, install_rollback_entries) ||
        !write_package_conffiles(pkg_name, conffiles)) {
        rollback_install_changes(install_rollback_entries);
        if (runtime_sensitive) {
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
        }
        std::cerr << "E: Failed to write package conffile metadata." << std::endl;
        return false;
    }
    std::string replaced_manifest_path = get_replaced_system_manifest(pkg_name);
    if (!prepare_path_for_transaction_write(replaced_manifest_path, replaced_manifest_path, install_rollback_entries)) {
        rollback_install_changes(install_rollback_entries);
        if (runtime_sensitive) {
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
        }
        std::cerr << "E: Failed to prepare replaced-system manifest path." << std::endl;
        return false;
    }
    if (!write_replaced_system_files(pkg_name, replaced_system_files)) {
        rollback_install_changes(install_rollback_entries);
        if (runtime_sensitive) {
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
        }
        return false;
    }
    
    // Copy scripts
    std::vector<std::string> scripts = {"preinst", "postinst", "prerm", "postrm"};
    for(const auto& s : scripts) {
        std::string src = g_tmp_extract_path + "scripts/" + s;
        std::string target = get_info_dir() + pkg_name + "." + s;
        if (!prepare_path_for_transaction_write(target, target, install_rollback_entries)) {
            rollback_install_changes(install_rollback_entries);
            if (runtime_sensitive) {
                sync_multiarch_runtime_aliases();
                refresh_linker_cache_if_available();
            }
            std::cerr << "E: Failed to prepare maintainer script target " << s << "." << std::endl;
            return false;
        }
        if(access(src.c_str(), F_OK) == 0) {
            if (!copy_file_atomic(src, target)) {
                rollback_install_changes(install_rollback_entries);
                if (runtime_sensitive) {
                    sync_multiarch_runtime_aliases();
                    refresh_linker_cache_if_available();
                }
                std::cerr << "E: Failed to install maintainer script " << s << "." << std::endl;
                return false;
            }
        }
    }

    std::string undo_path = get_info_dir() + pkg_name + ".undo";
    if (!prepare_path_for_transaction_write(undo_path, undo_path, install_rollback_entries)) {
        rollback_install_changes(install_rollback_entries);
        if (runtime_sensitive) {
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
        }
        std::cerr << "E: Failed to prepare undo metadata path." << std::endl;
        return false;
    }

    if (!set_package_status_record(pkg_name, "install", "ok", "unpacked", new_version)) {
        rollback_install_changes(install_rollback_entries);
        if (runtime_sensitive) {
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
        }
        std::cerr << "E: Failed to record unpacked package state." << std::endl;
        return false;
    }

    // 7. Postinst
    std::string installed_postinst = get_info_dir() + pkg_name + ".postinst";
    if (!set_package_status_record(pkg_name, "install", "ok", "half-configured", new_version)) {
         rollback_install_changes(install_rollback_entries);
         if (runtime_sensitive) {
             sync_multiarch_runtime_aliases();
             refresh_linker_cache_if_available();
         }
         std::cerr << "E: Failed to record half-configured package state." << std::endl;
         return false;
    }
    if (access(installed_postinst.c_str(), X_OK) == 0) {
         std::vector<std::string> postinst_args = {"configure"};
         if (is_upgrade) postinst_args.push_back(old_version);
         if (run_path_with_args(installed_postinst, postinst_args) != 0) {
             std::vector<std::string> undo_cmds = load_registered_undo_commands(pkg_name);
             if (!undo_cmds.empty()) {
                 run_registered_undo_commands_reverse(undo_cmds, "failed postinst rollback", true);
             }
             rollback_install_changes(install_rollback_entries);
             if (runtime_sensitive) {
                 sync_multiarch_runtime_aliases();
                 refresh_linker_cache_if_available();
             }
             std::cerr << "E: postinst failed." << std::endl;
             return false;
         }
    }

    if (kernel_payload) {
        if (!sync_kernel_boot_symlink()) {
            rollback_install_changes(install_rollback_entries);
            if (runtime_sensitive) {
                sync_multiarch_runtime_aliases();
                refresh_linker_cache_if_available();
            }
            std::cerr << "E: Failed to update /boot/kernel after installing " << pkg_name << "." << std::endl;
            return false;
        }
        if (!run_depmod_for_kernel_release(kernel_release, false)) {
            rollback_install_changes(install_rollback_entries);
            if (runtime_sensitive) {
                sync_multiarch_runtime_aliases();
                refresh_linker_cache_if_available();
            }
            std::cerr << "E: depmod failed after installing kernel " << kernel_release << "." << std::endl;
            return false;
        }
        std::vector<std::string> kernel_postinst_args = {"configure"};
        if (is_upgrade) kernel_postinst_args.push_back(old_version);
        if (!run_kernel_hook_directories("postinst", kernel_release, kernel_image_path, kernel_postinst_args)) {
            rollback_install_changes(install_rollback_entries);
            if (runtime_sensitive) {
                sync_multiarch_runtime_aliases();
                refresh_linker_cache_if_available();
            }
            std::cerr << "E: Kernel postinst hooks failed for " << pkg_name << "." << std::endl;
            return false;
        }
    }

    // 8. Cleanup Orphans (Upgrade only)
    if (is_upgrade) {
        std::set<std::string> new_files_set(installed_files.begin(), installed_files.end());
        std::set<std::string> preserved_original_paths;
        for (const auto& entry : preserved_configs) {
            preserved_original_paths.insert(entry.path);
        }
        std::vector<std::string> orphans;
        for (const auto& old : old_files_set) {
            if (preserved_original_paths.count(old)) continue;
            if (new_files_set.find(old) == new_files_set.end()) {
                orphans.push_back(old);
            }
        }
        
        // Remove orphans in reverse order (deepest first)
        std::sort(orphans.rbegin(), orphans.rend()); 
        
        if (!orphans.empty()) {
            VLOG("Cleaning up " << orphans.size() << " orphaned files...");
            for (const auto& orphan : orphans) {
                remove_path(orphan);
            }
        }
    }

    std::vector<std::string> postinstall_selinux_delta =
        collect_postinstall_relabel_delta(pkg_name, initial_selinux_relabel_paths);
    if (g_defer_selinux_relabel) {
        std::vector<std::string> deferred_selinux_paths = initial_selinux_relabel_paths;
        deferred_selinux_paths.insert(
            deferred_selinux_paths.end(),
            postinstall_selinux_delta.begin(),
            postinstall_selinux_delta.end()
        );
        if (!finalize_selinux_relabel_for_success(deferred_selinux_paths, &selinux_error)) {
            rollback_install_changes(install_rollback_entries);
            if (runtime_sensitive) {
                sync_multiarch_runtime_aliases();
                refresh_linker_cache_if_available();
            }
            std::cerr << "E: " << selinux_error << std::endl;
            return false;
        }
    } else if (!postinstall_selinux_delta.empty()) {
        if (!restorecon_transaction_paths(postinstall_selinux_delta, &selinux_error)) {
            rollback_install_changes(install_rollback_entries);
            if (runtime_sensitive) {
                sync_multiarch_runtime_aliases();
                refresh_linker_cache_if_available();
            }
            std::cerr << "E: " << selinux_error << std::endl;
            return false;
        }
    }

    if (selinux_policy_touched && selinux_config_requests_enabled()) {
        std::string autorelabel_path = g_root_prefix + "/.autorelabel";
        if (!prepare_path_for_transaction_write(autorelabel_path, "/.autorelabel", install_rollback_entries) ||
            !write_text_file_atomic(autorelabel_path, "", 0644)) {
            rollback_install_changes(install_rollback_entries);
            if (runtime_sensitive) {
                sync_multiarch_runtime_aliases();
                refresh_linker_cache_if_available();
            }
            std::cerr << "E: Failed to schedule SELinux autorelabel after updating policy files." << std::endl;
            return false;
        }
    }

    if (!set_package_status_record(pkg_name, "install", "ok", "installed", new_version)) {
        rollback_install_changes(install_rollback_entries);
        if (runtime_sensitive) {
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
        }
        std::cerr << "E: Failed to finalize package status after installation." << std::endl;
        return false;
    }
    invalidate_installed_manifest_snapshot();
    discard_install_backups(install_rollback_entries);
    status_guard.commit();

    std::cout << "✓ Installed " << pkg_name << " (" << new_version << ")" << std::endl;
    return true;
}

// --- Verification Logic ---

bool action_verify(const std::string& pkg_name) {
    if (pkg_name.empty()) {
        std::cerr << "E: No package specified for verification." << std::endl;
        return false;
    }

    std::vector<std::string> files = normalize_owned_manifest_paths(read_list_file(pkg_name));
    if (files.empty()) {
        std::cerr << "E: Package " << pkg_name << " not found or empty." << std::endl;
        return false;
    }

    std::cout << "Verifying " << pkg_name << "..." << std::endl;
    bool passed = true;
    std::set<std::tuple<std::string, std::string, std::string>> runtime_alias_candidates;
    for (const auto& f : files) {
        std::string full_path = g_root_prefix + f;
        struct stat st;
        if (lstat(full_path.c_str(), &st) != 0) {
             std::cerr << "MISSING: " << f << std::endl;
             passed = false;
        } else {
             // Basic type check
             if (f.back() == '/' || S_ISDIR(st.st_mode)) {
                 if (!S_ISDIR(st.st_mode)) {
                     std::cerr << "TYPE MISMATCH (Expected Dir): " << f << std::endl;
                     passed = false;
                 }
             } else if (S_ISLNK(st.st_mode)) {
                 struct stat target_st;
                 if (stat(full_path.c_str(), &target_st) != 0) {
                     std::cerr << "W: DANGLING SYMLINK: " << f << std::endl;
                 }
             } else {
                 // We expect a file or symlink
                 if (S_ISDIR(st.st_mode)) {
                     std::cerr << "TYPE MISMATCH (Expected File): " << f << std::endl;
                     passed = false;
                 } else if (S_ISREG(st.st_mode)) {
                     std::string elf_error;
                     if (!validate_elf_file(full_path, st.st_size, &elf_error)) {
                         std::cerr << "CORRUPT ELF: " << f << " (" << elf_error << ")" << std::endl;
                         passed = false;
                     }
                 }
             }
        }

        std::string active_prefix;
        std::string compat_prefix;
        std::string name;
        if (runtime_alias_pair_for_path(f, &active_prefix, &compat_prefix, &name)) {
            runtime_alias_candidates.insert(std::make_tuple(active_prefix, compat_prefix, name));
        }
    }

    for (const auto& candidate : runtime_alias_candidates) {
        const std::string& active_prefix = std::get<0>(candidate);
        const std::string& compat_prefix = std::get<1>(candidate);
        const std::string& name = std::get<2>(candidate);

        std::string active_path = g_root_prefix + active_prefix + "/" + name;
        std::string compat_path = g_root_prefix + compat_prefix + "/" + name;
        if (!path_exists_no_follow(active_path) || !path_exists_no_follow(compat_path)) continue;

        std::string active_real = canonical_existing_path(active_path);
        std::string compat_real = canonical_existing_path(compat_path);
        if (active_real.empty() || compat_real.empty() || active_real != compat_real) {
            std::cerr << "RUNTIME ALIAS MISMATCH: " << active_prefix << "/" << name
                      << " <-> " << compat_prefix << "/" << name << std::endl;
            passed = false;
        }
    }
    
    if (passed) std::cout << "✓ Verification passed." << std::endl;
    else std::cout << "X Verification failed." << std::endl;
    return passed;
}

bool action_refresh_runtime_linker_state() {
    return refresh_linker_cache_if_available();
}

int main(int argc, char* argv[]) {

    if (argc < 2) {

        std::cout << "Usage: gpkg-worker [options]\nOptions:\n  --install <file>\n  --remove <pkg>\n  --purge <pkg>\n  --retire <pkg>\n  --verify <pkg>\n  --refresh-runtime-linker-state\n  --refresh-selinux-label-state\n  --register-file <path> --pkg <name>\n  --register-undo <cmd> --pkg <name>\n  --jobs <n>\n  --defer-runtime-linker-refresh\n  --defer-selinux-relabel\n";

        return 1;

    }

    std::string mode, target, pkg_name;

    for (int i = 1; i < argc; ++i) {

        std::string arg = argv[i];

        if (arg == "--install") mode = "install", target = argv[++i];

        else if (arg == "--remove") mode = "remove", target = argv[++i];

        else if (arg == "--purge") mode = "purge", target = argv[++i];

        else if (arg == "--retire") mode = "retire", target = argv[++i];

        else if (arg == "--verify") mode = "verify", target = argv[++i];

        else if (arg == "--refresh-runtime-linker-state") mode = "refresh-runtime-linker-state";

        else if (arg == "--register-file") mode = "register-file", target = argv[++i];

        else if (arg == "--register-undo") mode = "register-undo", target = argv[++i];

        else if (arg == "--refresh-selinux-label-state") mode = "refresh-selinux-label-state";

        else if (arg == "--pkg") pkg_name = argv[++i];

        else if (arg == "--root") g_root_prefix = argv[++i];

        else if (arg == "--jobs") {
            size_t parsed_jobs = 0;
            if (i + 1 >= argc || !parse_parallel_jobs_value(argv[++i], &parsed_jobs)) {
                std::cerr << "Invalid value for --jobs.\n";
                return 1;
            }
            g_parallel_jobs = parsed_jobs;
        }

        else if (arg == "--defer-runtime-linker-refresh") g_defer_runtime_linker_refresh = true;

        else if (arg == "--defer-selinux-relabel") g_defer_selinux_relabel = true;

        else if (arg == "-v" || arg == "--verbose") g_verbose = true;

    }

    if (mode == "remove" && !target.empty()) return action_remove_safe(target) ? 0 : 1;

    if (mode == "purge" && !target.empty()) return action_purge_safe(target) ? 0 : 1;

    if (mode == "retire" && !target.empty()) return action_retire_safe(target) ? 0 : 1;

    if (mode == "install" && !target.empty()) return action_install(target) ? 0 : 1;

    if (mode == "verify" && !target.empty()) return action_verify(target) ? 0 : 1;

    if (mode == "refresh-runtime-linker-state") return action_refresh_runtime_linker_state() ? 0 : 1;

    if (mode == "refresh-selinux-label-state") return action_refresh_selinux_label_state() ? 0 : 1;

    if (mode == "register-file" && !target.empty() && !pkg_name.empty()) return action_register_file(pkg_name, target) ? 0 : 1;

    if (mode == "register-undo" && !target.empty() && !pkg_name.empty()) return action_register_undo(pkg_name, target) ? 0 : 1;

    std::cerr << "Invalid arguments or missing target/pkg.\n";

    return 1;

}
