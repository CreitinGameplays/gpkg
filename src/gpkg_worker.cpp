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
#include <chrono>
#include <fnmatch.h>
#include <mutex>
#include <thread>
#include <sys/mount.h>
#include <sys/xattr.h>
#include <sys/sysmacros.h>
#include "gpkg_archive.ipp"

// Configuration
std::string g_root_prefix = "";

std::string get_info_dir() {
    return g_root_prefix + "/var/lib/gpkg/info/";
}

std::string get_status_file_path() {
    return g_root_prefix + "/var/lib/gpkg/status";
}

std::string get_repo_cache_path() {
    return g_root_prefix + "/var/repo/";
}

std::string get_debian_pool_cache_dir() {
    return get_repo_cache_path() + "debian/pool/";
}

std::string get_imported_cache_dir() {
    return get_repo_cache_path() + "imported/";
}

std::string safe_repo_filename_component_local(const std::string& value) {
    std::string safe;
    safe.reserve(value.size());
    for (char ch : value) {
        if (ch == '/' || ch == ' ') safe += '_';
        else safe += ch;
    }
    return safe;
}

std::string cache_safe_component_local(const std::string& value) {
    std::string safe;
    safe.reserve(value.size());
    for (char ch : value) {
        if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '.' || ch == '_' || ch == '-') {
            safe += ch;
        } else {
            safe += '_';
        }
    }
    return safe.empty() ? "unknown" : safe;
}

std::string native_package_architecture() {
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#elif defined(__arm__)
    return "armv7l";
#else
    return "";
#endif
}

std::string get_conffile_manifest_path(const std::string& pkg_name) {
    return get_info_dir() + pkg_name + ".conffiles";
}

std::string get_debian_control_sidecar_manifest_path(const std::string& pkg_name) {
    return get_info_dir() + pkg_name + ".debctl.list";
}

std::string get_debian_control_sidecar_path(
    const std::string& pkg_name,
    const std::string& control_name
) {
    return get_info_dir() + pkg_name + ".debctl." + control_name;
}

std::string get_trigger_state_dir() {
    return g_root_prefix + "/var/lib/gpkg/triggers";
}

std::string get_pending_dpkg_trigger_queue_path() {
    return get_trigger_state_dir() + "/dpkg-pending.list";
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
bool action_refresh_dpkg_trigger_state();
bool action_configure(const std::string& pkg_name, const std::string& old_version);
bool action_prepare_transaction_root(const std::string& txn_dir);
bool action_commit_transaction_root(const std::string& txn_dir);
bool action_cleanup_transaction_root(const std::string& txn_dir);
bool append_pending_dpkg_trigger_name(const std::string& trigger_name, std::string* error_out = nullptr);
void mark_packages_trigger_pending(const std::vector<std::string>& trigger_names);
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
std::string normalize_logical_absolute_path(const std::string& path);
std::string logical_path_from_rooted_path(const std::string& full_path);

// Logging
bool g_verbose = false;
size_t g_parallel_jobs = 0;
bool g_defer_runtime_linker_refresh = false;
bool g_defer_selinux_relabel = false;
bool g_defer_configure = false;
bool g_unsafe_io = false;
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

bool env_var_is_truthy(const char* value) {
    if (!value || !*value) return false;
    std::string normalized = trim(value);
    std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return normalized == "1" ||
           normalized == "true" ||
           normalized == "yes" ||
           normalized == "on";
}

std::string get_etc_passwd_path() {
    return g_root_prefix + "/etc/passwd";
}

std::string get_etc_group_path() {
    return g_root_prefix + "/etc/group";
}

std::string get_etc_shadow_path() {
    return g_root_prefix + "/etc/shadow";
}

std::vector<std::string> split_preserve_empty_local(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    for (char ch : text) {
        if (ch == delimiter) {
            parts.push_back(current);
            current.clear();
        } else {
            current += ch;
        }
    }
    parts.push_back(current);
    return parts;
}

std::vector<std::string> read_text_lines_local(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream file(path);
    if (!file) return lines;
    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    return lines;
}

bool parse_small_int_local(const std::string& text, int* out) {
    if (out) *out = -1;
    std::string normalized = trim(text);
    if (normalized.empty()) return false;
    char* end = nullptr;
    errno = 0;
    long value = std::strtol(normalized.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0' || value < 0 || value > 65534) return false;
    if (out) *out = static_cast<int>(value);
    return true;
}

struct DebianStaticGroupSpec {
    const char* name;
    int preferred_gid;
    const char* members;
};

struct DebianStaticUserSpec {
    const char* name;
    int preferred_uid;
    const char* primary_group;
    const char* gecos;
    const char* home;
    const char* shell;
};

const DebianStaticGroupSpec kDebianStaticGroupBaseline[] = {
    {"daemon", 1, ""},
    {"bin", 2, ""},
    {"sys", 3, ""},
    {"adm", 4, ""},
    {"tty", 5, ""},
    {"disk", 6, ""},
    {"lp", 7, ""},
    {"mail", 8, ""},
    {"news", 9, ""},
    {"uucp", 10, ""},
    {"man", 12, ""},
    {"proxy", 13, ""},
    {"kmem", 15, ""},
    {"dialout", 20, ""},
    {"fax", 21, ""},
    {"voice", 22, ""},
    {"cdrom", 24, ""},
    {"floppy", 25, ""},
    {"tape", 26, ""},
    {"sudo", 27, "root"},
    {"audio", 29, ""},
    {"dip", 30, ""},
    {"www-data", 33, ""},
    {"backup", 34, ""},
    {"operator", 37, ""},
    {"list", 38, ""},
    {"irc", 39, ""},
    {"src", 40, ""},
    {"shadow", 42, ""},
    {"utmp", 43, ""},
    {"video", 44, ""},
    {"sasl", 45, ""},
    {"plugdev", 46, ""},
    {"staff", 50, ""},
    {"games", 60, ""},
    {"users", 100, ""},
    {"netdev", 106, ""},
    {"nogroup", 65534, ""},
};

const DebianStaticUserSpec kDebianStaticUserBaseline[] = {
    {"daemon", 1, "daemon", "daemon", "/usr/sbin", "/usr/sbin/nologin"},
    {"bin", 2, "bin", "bin", "/bin", "/usr/sbin/nologin"},
    {"sys", 3, "sys", "sys", "/dev", "/usr/sbin/nologin"},
    {"sync", 4, "nogroup", "sync", "/bin", "/bin/sync"},
    {"games", 5, "games", "games", "/usr/games", "/usr/sbin/nologin"},
    {"man", 6, "man", "man", "/var/cache/man", "/usr/sbin/nologin"},
    {"lp", 7, "lp", "lp", "/var/spool/lpd", "/usr/sbin/nologin"},
    {"mail", 8, "mail", "mail", "/var/mail", "/usr/sbin/nologin"},
    {"news", 9, "news", "news", "/var/spool/news", "/usr/sbin/nologin"},
    {"uucp", 10, "uucp", "uucp", "/var/spool/uucp", "/usr/sbin/nologin"},
    {"proxy", 13, "proxy", "proxy", "/bin", "/usr/sbin/nologin"},
    {"www-data", 33, "www-data", "www-data", "/var/www", "/usr/sbin/nologin"},
    {"backup", 34, "backup", "backup", "/var/backups", "/usr/sbin/nologin"},
    {"list", 38, "list", "Mailing List Manager", "/var/list", "/usr/sbin/nologin"},
    {"irc", 39, "irc", "ircd", "/run/ircd", "/usr/sbin/nologin"},
    {"_apt", 42, "nogroup", "", "/nonexistent", "/usr/sbin/nologin"},
    {"nobody", 65534, "nogroup", "nobody", "/nonexistent", "/usr/sbin/nologin"},
};

int choose_available_compat_id(int preferred_id, const std::set<int>& used_ids, int first_id, int last_id) {
    if (preferred_id >= 0 && used_ids.count(preferred_id) == 0) return preferred_id;
    for (int candidate = first_id; candidate <= last_id; ++candidate) {
        if (used_ids.count(candidate) == 0) return candidate;
    }
    for (int candidate = last_id + 1; candidate < 65534; ++candidate) {
        if (used_ids.count(candidate) == 0) return candidate;
    }
    return -1;
}

bool ensure_debian_static_identity_baseline(std::string* error_out = nullptr) {
    if (error_out) error_out->clear();

    std::vector<std::string> passwd_lines = read_text_lines_local(get_etc_passwd_path());
    std::vector<std::string> group_lines = read_text_lines_local(get_etc_group_path());
    std::vector<std::string> shadow_lines = read_text_lines_local(get_etc_shadow_path());

    std::set<std::string> group_names;
    std::set<int> group_ids;
    std::map<std::string, int> gid_by_group_name;
    for (const auto& line : group_lines) {
        auto fields = split_preserve_empty_local(line, ':');
        if (fields.size() < 4 || fields[0].empty()) continue;
        int gid = -1;
        if (!parse_small_int_local(fields[2], &gid)) continue;
        group_names.insert(fields[0]);
        group_ids.insert(gid);
        gid_by_group_name[fields[0]] = gid;
    }

    bool group_modified = false;
    for (const auto& spec : kDebianStaticGroupBaseline) {
        if (group_names.count(spec.name) != 0) continue;
        int gid = choose_available_compat_id(spec.preferred_gid, group_ids, 100, 999);
        if (gid < 0) {
            if (error_out) *error_out = "no available GID for " + std::string(spec.name);
            return false;
        }
        std::ostringstream line;
        line << spec.name << ":x:" << gid << ":" << spec.members;
        group_lines.push_back(line.str());
        group_names.insert(spec.name);
        group_ids.insert(gid);
        gid_by_group_name[spec.name] = gid;
        group_modified = true;
    }

    std::set<std::string> user_names;
    std::set<int> user_ids;
    for (const auto& line : passwd_lines) {
        auto fields = split_preserve_empty_local(line, ':');
        if (fields.size() < 7 || fields[0].empty()) continue;
        int uid = -1;
        if (!parse_small_int_local(fields[2], &uid)) continue;
        user_names.insert(fields[0]);
        user_ids.insert(uid);
    }

    bool passwd_modified = false;
    for (const auto& spec : kDebianStaticUserBaseline) {
        if (user_names.count(spec.name) != 0) continue;
        auto gid_it = gid_by_group_name.find(spec.primary_group);
        if (gid_it == gid_by_group_name.end()) {
            if (error_out) {
                *error_out = "missing primary group " + std::string(spec.primary_group) +
                             " while creating " + spec.name;
            }
            return false;
        }
        int uid = choose_available_compat_id(spec.preferred_uid, user_ids, 100, 999);
        if (uid < 0) {
            if (error_out) *error_out = "no available UID for " + std::string(spec.name);
            return false;
        }
        std::ostringstream line;
        line << spec.name << ":x:" << uid << ":" << gid_it->second << ":" << spec.gecos << ":"
             << spec.home << ":" << spec.shell;
        passwd_lines.push_back(line.str());
        user_names.insert(spec.name);
        user_ids.insert(uid);
        passwd_modified = true;
    }

    std::set<std::string> shadow_names;
    for (const auto& line : shadow_lines) {
        auto fields = split_preserve_empty_local(line, ':');
        if (!fields.empty() && !fields[0].empty()) shadow_names.insert(fields[0]);
    }

    bool shadow_modified = false;
    for (const auto& spec : kDebianStaticUserBaseline) {
        if (user_names.count(spec.name) == 0 || shadow_names.count(spec.name) != 0) continue;
        shadow_lines.push_back(std::string(spec.name) + ":!:19000:0:99999:7:::");
        shadow_names.insert(spec.name);
        shadow_modified = true;
    }

    if (group_modified) {
        std::ostringstream content;
        for (const auto& line : group_lines) content << line << "\n";
        if (!write_text_file_atomic(get_etc_group_path(), content.str(), 0644)) {
            if (error_out) *error_out = "failed to update /etc/group";
            return false;
        }
    }
    if (passwd_modified) {
        std::ostringstream content;
        for (const auto& line : passwd_lines) content << line << "\n";
        if (!write_text_file_atomic(get_etc_passwd_path(), content.str(), 0644)) {
            if (error_out) *error_out = "failed to update /etc/passwd";
            return false;
        }
    }
    if (shadow_modified) {
        std::ostringstream content;
        for (const auto& line : shadow_lines) content << line << "\n";
        if (!write_text_file_atomic(get_etc_shadow_path(), content.str(), 0600)) {
            if (error_out) *error_out = "failed to update /etc/shadow";
            return false;
        }
    }

    return true;
}

std::string format_phase_seconds(double seconds) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(seconds >= 10.0 ? 1 : 2) << seconds << "s";
    return out.str();
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
        bool rooted_exec = false;
        std::string rooted_path;
        if (!g_root_prefix.empty()) {
            rooted_path = logical_path_from_rooted_path(argv.front());
            rooted_exec = !rooted_path.empty();
        }

        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        if (rooted_exec) {
            if (chroot(g_root_prefix.c_str()) != 0 || chdir("/") != 0) _exit(127);
            cargv.push_back(const_cast<char*>(rooted_path.c_str()));
            for (size_t i = 1; i < argv.size(); ++i) {
                cargv.push_back(const_cast<char*>(argv[i].c_str()));
            }
        } else {
            for (const auto& arg : argv) {
                cargv.push_back(const_cast<char*>(arg.c_str()));
            }
        }
        cargv.push_back(nullptr);
        execv(rooted_exec ? rooted_path.c_str() : argv.front().c_str(), cargv.data());
        if (errno == ENOEXEC && access("/bin/sh", X_OK) == 0) {
            std::vector<char*> sh_argv;
            sh_argv.reserve(argv.size() + 2);
            sh_argv.push_back(const_cast<char*>("/bin/sh"));
            sh_argv.push_back(const_cast<char*>((rooted_exec ? rooted_path : argv.front()).c_str()));
            for (size_t i = 1; i < argv.size(); ++i) {
                sh_argv.push_back(const_cast<char*>(argv[i].c_str()));
            }
            sh_argv.push_back(nullptr);
            execv("/bin/sh", sh_argv.data());
        }
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

    size_t failed = 0;
    constexpr size_t kMaxRefreshPasses = 8;
    bool converged = false;
    for (size_t pass = 0; pass < kMaxRefreshPasses; ++pass) {
        size_t repaired = 0;
        size_t removed = 0;
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

        if (repaired == 0 && removed == 0) {
            converged = true;
            break;
        }
        if (!run_ldconfig(ldconfig_path)) return false;
    }

    if (!converged) {
        std::cerr << "W: Runtime linker refresh hit the convergence limit after "
                  << kMaxRefreshPasses << " pass"
                  << (kMaxRefreshPasses == 1 ? "" : "es")
                  << "." << std::endl;
    }
    if (failed > 0) {
        std::cerr << "W: Failed to apply " << failed
                  << " runtime linker state fixup"
                  << (failed == 1 ? "" : "s")
                  << "." << std::endl;
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

std::string logical_path_from_rooted_path(const std::string& full_path) {
    if (g_root_prefix.empty()) return "";
    if (full_path == g_root_prefix) return "/";
    std::string normalized_root = g_root_prefix;
    while (normalized_root.size() > 1 && normalized_root.back() == '/') normalized_root.pop_back();
    if (normalized_root.empty()) normalized_root = "/";
    if (full_path == normalized_root) return "/";
    std::string prefix = normalized_root + "/";
    if (full_path.rfind(prefix, 0) != 0) return "";
    return normalize_logical_absolute_path(full_path.substr(normalized_root.size()));
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

std::vector<std::string> read_trimmed_line_file(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    if (!in) return lines;

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

std::vector<std::string> load_debian_control_sidecar_names(const std::string& pkg_name) {
    return read_trimmed_line_file(get_debian_control_sidecar_manifest_path(pkg_name));
}

std::vector<std::string> collect_extracted_debian_control_sidecar_names(
    const std::string& control_root
) {
    std::vector<std::string> names;
    DIR* dir = opendir(control_root.c_str());
    if (!dir) return names;

    std::set<std::string> seen;
    while (true) {
        errno = 0;
        dirent* entry = readdir(dir);
        if (!entry) break;

        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        std::string full_path = control_root + "/" + name;
        struct stat st {};
        if (stat(full_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (!seen.insert(name).second) continue;
        names.push_back(name);
    }

    closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
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

std::string maintscript_package_name_from_metadata_object(
    const std::string& obj,
    const std::string& fallback_pkg_name
) {
    std::string package_name;
    if (get_json_string_value_from_object(obj, "debian_package", package_name)) {
        package_name = trim(package_name);
        if (!package_name.empty()) return package_name;
    }
    if (get_json_string_value_from_object(obj, "package", package_name)) {
        package_name = trim(package_name);
        if (!package_name.empty()) return package_name;
    }
    return fallback_pkg_name;
}

std::string read_maintscript_package_name_from_metadata_path(
    const std::string& metadata_path,
    const std::string& fallback_pkg_name
) {
    if (metadata_path.empty()) return fallback_pkg_name;

    std::ifstream in(metadata_path);
    if (!in) return fallback_pkg_name;

    std::string content(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>()
    );
    if (content.empty()) return fallback_pkg_name;
    return maintscript_package_name_from_metadata_object(content, fallback_pkg_name);
}

std::string read_json_string_from_metadata_path(
    const std::string& metadata_path,
    const std::string& key
) {
    if (metadata_path.empty()) return "";

    std::ifstream in(metadata_path);
    if (!in) return "";

    std::string content(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>()
    );
    if (content.empty()) return "";

    std::string value;
    if (!get_json_string_value_from_object(content, key, value)) return "";
    return trim(value);
}

std::string compat_debian_architecture_for_package_arch(const std::string& package_arch) {
    std::string arch = trim(package_arch);
    if (arch == "amd64" || arch == "all") return arch;
    if (arch == "x86_64") return "amd64";
    if (arch == "aarch64") return "arm64";
    if (arch == "armv7l") return "armhf";
    if (arch.empty()) {
#if defined(__x86_64__)
        return "amd64";
#elif defined(__aarch64__)
        return "arm64";
#elif defined(__arm__)
        return "armhf";
#else
        return "";
#endif
    }
    return arch;
}

std::string read_maintscript_package_arch_from_metadata_path(const std::string& metadata_path) {
    return compat_debian_architecture_for_package_arch(
        read_json_string_from_metadata_path(metadata_path, "architecture")
    );
}

bool write_executable_script_local(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << contents;
    out.close();
    if (!out) return false;
    return chmod(path.c_str(), 0755) == 0;
}

std::string current_worker_executable_path() {
    std::vector<char> buffer(4096, '\0');
    ssize_t len = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (len <= 0) return "";
    buffer[static_cast<size_t>(len)] = '\0';
    return std::string(buffer.data(), static_cast<size_t>(len));
}

std::string render_maintainer_script_params(const std::vector<std::string>& args) {
    std::ostringstream rendered;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i != 0) rendered << " ";
        rendered << shell_quote(args[i]);
    }
    return rendered.str();
}

std::string maintscript_default_home() {
    if (g_root_prefix.empty()) return "/root";
    std::string candidate = g_root_prefix + "/root";
    struct stat st {};
    if (stat(candidate.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return "/root";
    return "/";
}

std::string maintscript_default_tmpdir() {
    return "/tmp";
}

std::string compat_dpkg_admindir() {
    return "/var/lib/gpkg";
}

std::string compat_dpkg_running_version() {
    return "1.22.21+gpkg1";
}

std::string build_maintscript_search_path(const std::string& wrapper_dir) {
    std::vector<std::string> parts;
    std::set<std::string> seen;

    auto append = [&](const std::string& entry) {
        std::string value = trim(entry);
        if (value.empty()) return;
        if (!seen.insert(value).second) return;
        parts.push_back(value);
    };

    append(wrapper_dir);

    const char* current_path = getenv("PATH");
    if (current_path && *current_path) {
        for (const auto& segment : split_preserve_empty_local(current_path, ':')) {
            append(segment);
        }
    }

    const char* standard_paths[] = {
        "/usr/local/sbin",
        "/usr/local/bin",
        "/usr/sbin",
        "/usr/bin",
        "/sbin",
        "/bin",
    };
    for (const char* standard_path : standard_paths) append(standard_path);

    std::ostringstream rendered;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) rendered << ":";
        rendered << parts[i];
    }
    return rendered.str();
}

bool maintscript_dialog_frontend_available() {
    if (!g_root_prefix.empty()) return false;
    const char* candidates[] = {
        "/usr/bin/dialog",
        "/bin/dialog",
        "/usr/bin/whiptail",
        "/bin/whiptail",
    };
    for (const char* candidate : candidates) {
        if (access(candidate, X_OK) == 0) return true;
    }
    return false;
}

std::string default_maintscript_debian_frontend() {
    const char* explicit_frontend = getenv("DEBIAN_FRONTEND");
    if (explicit_frontend && *explicit_frontend) return "";

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return "noninteractive";
    if (!maintscript_dialog_frontend_available()) return "readline";
    return "";
}

struct ScopedMaintainerScriptCompat {
    ScopedEnvOverrides env;
    std::string wrapper_dir;
    std::string logical_wrapper_dir;

    ScopedMaintainerScriptCompat(
        const std::string& worker_path,
        const std::string& root_prefix,
        bool verbose
    ) {
        if (worker_path.empty()) return;

        std::string base_template = root_prefix.empty()
            ? "/tmp/gpkg-maintscript-compat-XXXXXX"
            : root_prefix + "/tmp/gpkg-maintscript-compat-XXXXXX";
        std::vector<char> wrapper_template(base_template.begin(), base_template.end());
        wrapper_template.push_back('\0');
        char* wrapper_root = mkdtemp(wrapper_template.data());
        if (!wrapper_root) return;
        wrapper_dir = wrapper_root;
        logical_wrapper_dir = root_prefix.empty()
            ? wrapper_dir
            : logical_path_from_rooted_path(wrapper_dir);

        std::string compat_worker_path = worker_path;
        if (!root_prefix.empty()) {
            std::string copied_worker_path = wrapper_dir + "/gpkg-worker";
            if (copy_file_atomic(worker_path, copied_worker_path)) {
                chmod(copied_worker_path.c_str(), 0755);
                std::string logical_worker = logical_path_from_rooted_path(copied_worker_path);
                if (!logical_worker.empty()) compat_worker_path = logical_worker;
            } else {
                const char* worker_candidates[] = {
                    "/bin/apps/system/gpkg-worker",
                    "/bin/gpkg-worker",
                };
                for (const char* candidate : worker_candidates) {
                    std::string full_candidate = root_prefix + candidate;
                    if (access(full_candidate.c_str(), X_OK) == 0) {
                        compat_worker_path = candidate;
                        break;
                    }
                }
            }
        }

        struct CompatWrapperSpec {
            const char* name;
            const char* action;
        };

        const CompatWrapperSpec wrappers[] = {
            {"dpkg", "--compat-dpkg"},
            {"dpkg-query", "--compat-dpkg-query"},
            {"dpkg-trigger", "--compat-dpkg-trigger"},
            {"update-rc.d", "--compat-update-rc.d"},
            {"invoke-rc.d", "--compat-invoke-rc.d"},
            {"service", "--compat-service"},
            {"systemctl", "--compat-systemctl"},
            {"deb-systemd-invoke", "--compat-deb-systemd-invoke"},
            {"initctl", "--compat-initctl"},
        };

        bool wrappers_ok = true;
        for (const auto& spec : wrappers) {
            std::ostringstream script;
            script << "#!/bin/sh\n"
                   << "worker=" << shell_quote(compat_worker_path) << "\n"
                   << "exec \"$worker\"";
            if (verbose) script << " --verbose";
            script << " " << spec.action << " \"$@\"\n";

            std::string wrapper_path = wrapper_dir + "/" + spec.name;
            if (!write_executable_script_local(wrapper_path, script.str())) {
                wrappers_ok = false;
                break;
            }
        }

        if (!wrappers_ok) {
            remove_tree_no_follow(wrapper_dir);
            wrapper_dir.clear();
            return;
        }

        env.set(
            "PATH",
            build_maintscript_search_path(
                logical_wrapper_dir.empty() ? wrapper_dir : logical_wrapper_dir
            )
        );
    }

    ~ScopedMaintainerScriptCompat() {
        if (!wrapper_dir.empty()) remove_tree_no_follow(wrapper_dir);
    }
};

struct ScopedRootedMaintainerScriptStage {
    std::string staged_path;
    std::string staging_dir;

    explicit ScopedRootedMaintainerScriptStage(const std::string& script_path) {
        if (g_root_prefix.empty() || script_path.empty()) {
            staged_path = script_path;
            return;
        }

        if (!logical_path_from_rooted_path(script_path).empty()) {
            staged_path = script_path;
            return;
        }

        std::string base_template = g_root_prefix + "/tmp/gpkg-maintscript-stage-XXXXXX";
        std::vector<char> dir_template(base_template.begin(), base_template.end());
        dir_template.push_back('\0');
        char* staged_root = mkdtemp(dir_template.data());
        if (!staged_root) return;
        staging_dir = staged_root;

        std::string target_path = staging_dir + "/" + path_basename(script_path);
        if (!copy_file_atomic(script_path, target_path)) {
            remove_tree_no_follow(staging_dir);
            staging_dir.clear();
            return;
        }
        chmod(target_path.c_str(), 0755);
        staged_path = target_path;
    }

    ~ScopedRootedMaintainerScriptStage() {
        if (!staging_dir.empty()) remove_tree_no_follow(staging_dir);
    }
};

int run_maintainer_script_with_args(
    const std::string& script_path,
    const std::string& script_name,
    const std::string& fallback_pkg_name,
    const std::string& metadata_path,
    const std::vector<std::string>& args = {}
) {
    std::string identity_error;
    if (!ensure_debian_static_identity_baseline(&identity_error)) {
        std::cerr << "E: Failed to prepare Debian static account/group compatibility baseline";
        if (!identity_error.empty()) std::cerr << ": " << identity_error;
        std::cerr << std::endl;
        return 1;
    }

    std::string maintscript_package =
        read_maintscript_package_name_from_metadata_path(metadata_path, fallback_pkg_name);
    if (maintscript_package.empty()) maintscript_package = fallback_pkg_name;
    std::string maintscript_arch = read_maintscript_package_arch_from_metadata_path(metadata_path);
    std::string maintscript_version = read_json_string_from_metadata_path(metadata_path, "version");
    ScopedRootedMaintainerScriptStage staged_script(script_path);
    std::string runnable_script = staged_script.staged_path.empty() ? script_path : staged_script.staged_path;

    ScopedEnvOverrides env;
    ScopedMaintainerScriptCompat compat(current_worker_executable_path(), g_root_prefix, g_verbose);
    env.set("DPKG_MAINTSCRIPT_NAME", script_name);
    if (!maintscript_package.empty()) {
        env.set("DPKG_MAINTSCRIPT_PACKAGE", maintscript_package);
    }
    if (!maintscript_arch.empty()) env.set("DPKG_MAINTSCRIPT_ARCH", maintscript_arch);
    env.set("DPKG_MAINTSCRIPT_PACKAGE_REFCOUNT", "1");
    env.set("DPKG_ADMINDIR", compat_dpkg_admindir());
    env.set("DPKG_RUNNING_VERSION", compat_dpkg_running_version());
    if (!g_root_prefix.empty()) env.set("DPKG_ROOT", "/");
    env.set("DEB_MAINT_PARAMS", render_maintainer_script_params(args));
    std::string debconf_frontend = default_maintscript_debian_frontend();
    if (!debconf_frontend.empty()) env.set("DEBIAN_FRONTEND", debconf_frontend);
    if (!maintscript_version.empty()) env.set("GPKG_MAINTSCRIPT_VERSION", maintscript_version);
    if (!getenv("SHELL") || !*getenv("SHELL")) env.set("SHELL", "/bin/sh");
    if (!getenv("HOME") || !*getenv("HOME")) env.set("HOME", maintscript_default_home());
    if (!getenv("TMPDIR") || !*getenv("TMPDIR")) env.set("TMPDIR", maintscript_default_tmpdir());

    return run_path_with_args(runnable_script, args);
}

std::string installed_maintainer_script_path(
    const std::string& pkg_name,
    const std::string& script_name
) {
    return get_info_dir() + pkg_name + "." + script_name;
}

std::string extracted_maintainer_script_path(const std::string& script_name) {
    return g_tmp_extract_path + "scripts/" + script_name;
}

int run_optional_maintainer_script_with_args(
    const std::string& script_path,
    const std::string& script_name,
    const std::string& fallback_pkg_name,
    const std::string& metadata_path,
    const std::vector<std::string>& args = {}
) {
    if (access(script_path.c_str(), X_OK) != 0) return 0;
    return run_maintainer_script_with_args(
        script_path,
        script_name,
        fallback_pkg_name,
        metadata_path,
        args
    );
}

bool run_optional_maintainer_script_with_reporting(
    const std::string& script_path,
    const std::string& script_name,
    const std::string& fallback_pkg_name,
    const std::string& metadata_path,
    const std::vector<std::string>& args,
    const std::string& label,
    bool best_effort,
    int* rc_out = nullptr
) {
    if (rc_out) *rc_out = 0;
    if (access(script_path.c_str(), X_OK) != 0) return true;

    int rc = run_maintainer_script_with_args(
        script_path,
        script_name,
        fallback_pkg_name,
        metadata_path,
        args
    );
    if (rc_out) *rc_out = rc;
    if (rc == 0) return true;

    std::cerr << (best_effort ? "W: " : "E: ")
              << label << " failed";
    if (!fallback_pkg_name.empty()) std::cerr << " for " << fallback_pkg_name;
    std::cerr << "." << std::endl;
    return false;
}

bool compat_status_is_installed_like(const std::string& status) {
    return status == "half-installed" ||
           status == "unpacked" ||
           status == "half-configured" ||
           status == "triggers-awaited" ||
           status == "triggers-pending" ||
           status == "installed";
}

bool compat_wildcard_match(const std::string& value, const std::string& pattern) {
    return fnmatch(pattern.c_str(), value.c_str(), 0) == 0;
}

bool compat_pattern_has_glob(const std::string& pattern) {
    return pattern.find_first_of("*?[]") != std::string::npos;
}

std::string strip_debian_arch_qualifier(const std::string& value) {
    size_t colon = value.rfind(':');
    if (colon == std::string::npos) return value;

    std::string suffix = value.substr(colon + 1);
    if (suffix.empty()) return value;
    for (char ch : suffix) {
        if (!(std::isalnum(static_cast<unsigned char>(ch)) || ch == '-')) return value;
    }
    return value.substr(0, colon);
}

struct CompatDpkgPackageInfo {
    std::string query_name;
    std::string gpkg_name;
    std::string debian_name;
    std::string version;
    std::string arch;
    std::string package_arch;
    std::string description;
    std::string want = "install";
    std::string flag = "ok";
    std::string status = "installed";
    std::string metadata_path;
    std::string source_kind;
    std::string installed_from;
    std::string filename;
    bool is_base_system = false;
    std::vector<std::string> file_list;
};

bool ensure_compat_package_control_metadata(
    const CompatDpkgPackageInfo& info,
    const std::string& requested_control_name
);

bool load_compat_dpkg_package_by_gpkg_name(
    const std::string& gpkg_name,
    CompatDpkgPackageInfo& out_info
) {
    std::string metadata_path = get_info_dir() + gpkg_name + ".json";
    std::ifstream in(metadata_path);
    if (!in) return false;

    std::string content(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>()
    );
    if (content.empty()) return false;

    out_info = {};
    out_info.gpkg_name = gpkg_name;
    out_info.metadata_path = metadata_path;
    get_json_string_value_from_object(content, "debian_package", out_info.debian_name);
    if (out_info.debian_name.empty()) get_json_string_value_from_object(content, "package", out_info.debian_name);
    if (out_info.debian_name.empty()) out_info.debian_name = gpkg_name;
    get_json_string_value_from_object(content, "version", out_info.version);
    get_json_string_value_from_object(content, "description", out_info.description);
    out_info.package_arch = read_json_string_from_metadata_path(metadata_path, "architecture");
    out_info.arch = compat_debian_architecture_for_package_arch(out_info.package_arch);
    get_json_string_value_from_object(content, "source_kind", out_info.source_kind);
    get_json_string_value_from_object(content, "installed_from", out_info.installed_from);
    get_json_string_value_from_object(content, "filename", out_info.filename);

    PackageStatusRecord record;
    if (get_package_status_record(gpkg_name, &record)) {
        if (!record.want.empty()) out_info.want = record.want;
        if (!record.flag.empty()) out_info.flag = record.flag;
        if (!record.status.empty()) out_info.status = record.status;
        if (out_info.version.empty()) out_info.version = record.version;
    }

    return true;
}

bool load_compat_dpkg_package_from_base_system_object(
    const std::string& obj,
    CompatDpkgPackageInfo& out_info
) {
    std::string package;
    if (!get_json_string_value_from_object(obj, "package", package) || package.empty()) return false;

    out_info = {};
    out_info.gpkg_name = package;
    out_info.debian_name = package;
    get_json_string_value_from_object(obj, "version", out_info.version);
    get_json_string_value_from_object(obj, "source_kind", out_info.source_kind);
    get_json_string_value_from_object(obj, "installed_from", out_info.installed_from);
    out_info.package_arch = native_package_architecture();
    out_info.arch = compat_debian_architecture_for_package_arch(out_info.package_arch);
    out_info.is_base_system = true;
    get_json_string_array_from_object(obj, "files", out_info.file_list);
    return true;
}

bool load_compat_dpkg_package_from_base_system(
    const std::string& pkg_name,
    CompatDpkgPackageInfo& out_info
) {
    bool found = false;
    foreach_json_object_in_file(get_base_system_registry_path(), [&](const std::string& obj) {
        CompatDpkgPackageInfo candidate;
        if (!load_compat_dpkg_package_from_base_system_object(obj, candidate)) return true;
        if (candidate.gpkg_name != pkg_name) return true;
        out_info = std::move(candidate);
        found = true;
        return false;
    });
    return found;
}

std::vector<CompatDpkgPackageInfo> collect_all_compat_dpkg_packages() {
    std::vector<CompatDpkgPackageInfo> packages;
    std::set<std::string> seen;
    for (const auto& gpkg_name : get_installed_packages_from_disk(".json")) {
        if (!seen.insert(gpkg_name).second) continue;
        CompatDpkgPackageInfo info;
        if (!load_compat_dpkg_package_by_gpkg_name(gpkg_name, info)) continue;
        packages.push_back(std::move(info));
    }
    foreach_json_object_in_file(get_base_system_registry_path(), [&](const std::string& obj) {
        CompatDpkgPackageInfo info;
        if (!load_compat_dpkg_package_from_base_system_object(obj, info)) return true;
        std::string package = info.gpkg_name;
        if (!seen.insert(package).second) return true;
        packages.push_back(std::move(info));
        return true;
    });
    return packages;
}

bool resolve_compat_dpkg_package(
    const std::string& query_name,
    CompatDpkgPackageInfo& out_info
) {
    std::string normalized = strip_debian_arch_qualifier(trim(query_name));
    if (normalized.empty()) return false;

    if (access((get_info_dir() + normalized + ".json").c_str(), F_OK) == 0) {
        if (load_compat_dpkg_package_by_gpkg_name(normalized, out_info)) {
            out_info.query_name = query_name;
            return true;
        }
    }
    if (load_compat_dpkg_package_from_base_system(normalized, out_info)) {
        out_info.query_name = query_name;
        return true;
    }

    for (const auto& info : collect_all_compat_dpkg_packages()) {
        if (info.debian_name == normalized || info.gpkg_name == normalized) {
            out_info = info;
            out_info.query_name = query_name;
            return true;
        }
        if (!info.arch.empty() && (info.debian_name + ":" + info.arch) == query_name) {
            out_info = info;
            out_info.query_name = query_name;
            return true;
        }
    }

    return false;
}

std::vector<CompatDpkgPackageInfo> match_compat_dpkg_packages(const std::string& pattern) {
    std::vector<CompatDpkgPackageInfo> matches;
    std::set<std::string> seen;
    bool has_glob = compat_pattern_has_glob(pattern);
    for (const auto& info : collect_all_compat_dpkg_packages()) {
        std::vector<std::string> candidates = {
            info.gpkg_name,
            info.debian_name
        };
        if (!info.arch.empty()) candidates.push_back(info.debian_name + ":" + info.arch);

        bool matched = false;
        for (const auto& candidate : candidates) {
            if (has_glob ? compat_wildcard_match(candidate, pattern) : candidate == strip_debian_arch_qualifier(pattern) || candidate == pattern) {
                matched = true;
                break;
            }
        }
        if (!matched) continue;
        if (!seen.insert(info.gpkg_name).second) continue;
        matches.push_back(info);
    }
    return matches;
}

std::vector<std::string> load_compat_package_file_list(const CompatDpkgPackageInfo& info) {
    if (!info.file_list.empty() || info.is_base_system) return info.file_list;
    return read_list_file_from_disk(info.gpkg_name);
}

std::vector<std::string> load_compat_package_conffiles(const CompatDpkgPackageInfo& info) {
    return read_trimmed_line_file(get_conffile_manifest_path(info.gpkg_name));
}

std::map<std::string, std::string> load_compat_package_md5sums(const CompatDpkgPackageInfo& info) {
    std::map<std::string, std::string> md5sums;
    std::ifstream in(get_debian_control_sidecar_path(info.gpkg_name, "md5sums"));
    if (!in) return md5sums;

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.size() < 34) continue;

        size_t split = line.find_first_of(" \t");
        if (split == std::string::npos) continue;
        std::string md5 = trim(line.substr(0, split));
        std::string rel_path = trim(line.substr(split));
        if (md5.empty() || rel_path.empty()) continue;
        if (rel_path[0] != '/') rel_path = "/" + rel_path;
        md5sums[rel_path] = md5;
    }

    return md5sums;
}

std::string compat_package_status_triplet(const CompatDpkgPackageInfo& info) {
    return info.want + " " + info.flag + " " + info.status;
}

char compat_status_state_abbrev(const std::string& status) {
    if (status == "not-installed") return 'n';
    if (status == "config-files") return 'c';
    if (status == "half-installed") return 'H';
    if (status == "unpacked") return 'U';
    if (status == "half-configured") return 'F';
    if (status == "triggers-awaited") return 'W';
    if (status == "triggers-pending") return 't';
    if (status == "installed") return 'i';
    return 'u';
}

char compat_status_want_abbrev(const std::string& want) {
    if (want == "install") return 'i';
    if (want == "deinstall") return 'r';
    if (want == "purge") return 'p';
    if (want == "hold") return 'h';
    return 'u';
}

std::string compat_package_status_abbrev(const CompatDpkgPackageInfo& info) {
    std::string value;
    value += compat_status_want_abbrev(info.want);
    value += compat_status_state_abbrev(info.status);
    value += (info.flag == "reinstreq") ? 'R' : ' ';
    return value;
}

std::string compat_package_binary_name(const CompatDpkgPackageInfo& info) {
    if (info.debian_name.empty()) return info.gpkg_name;
    if (info.arch.empty() || info.arch == "all") return info.debian_name;
    return info.debian_name + ":" + info.arch;
}

std::string compat_package_conffiles_field(const CompatDpkgPackageInfo& info) {
    std::vector<std::string> conffiles = load_compat_package_conffiles(info);
    if (conffiles.empty()) {
        ensure_compat_package_control_metadata(info, "conffiles");
        conffiles = load_compat_package_conffiles(info);
    }
    if (conffiles.empty()) return "";

    std::map<std::string, std::string> md5sums = load_compat_package_md5sums(info);
    std::ostringstream out;
    for (const auto& path : conffiles) {
        out << " " << path;
        auto it = md5sums.find(path);
        if (it != md5sums.end()) out << " " << it->second;
        out << "\n";
    }
    return out.str();
}

std::string compat_package_field_value(
    const CompatDpkgPackageInfo& info,
    const std::string& field
) {
    if (field == "Package") return info.debian_name.empty() ? info.gpkg_name : info.debian_name;
    if (field == "binary:Package") return compat_package_binary_name(info);
    if (field == "Version") return info.version;
    if (field == "Architecture") return info.arch;
    if (field == "Status") return compat_package_status_triplet(info);
    if (field == "db:Status-Abbrev") return compat_package_status_abbrev(info);
    if (field == "db:Status-Want") return info.want;
    if (field == "db:Status-Status") return info.status;
    if (field == "db:Status-Eflag") return info.flag;
    if (field == "Conffiles") return compat_package_conffiles_field(info);
    if (field == "Description") return info.description;
    return "";
}

std::string decode_compat_showformat_escapes(const std::string& format) {
    std::string decoded;
    decoded.reserve(format.size());
    for (size_t i = 0; i < format.size(); ++i) {
        char ch = format[i];
        if (ch != '\\' || i + 1 >= format.size()) {
            decoded += ch;
            continue;
        }
        char esc = format[++i];
        switch (esc) {
            case 'n': decoded += '\n'; break;
            case 'r': decoded += '\r'; break;
            case 't': decoded += '\t'; break;
            case '\\': decoded += '\\'; break;
            default:
                decoded += esc;
                break;
        }
    }
    return decoded;
}

std::string render_compat_showformat(
    const std::string& format,
    const CompatDpkgPackageInfo& info
) {
    std::string decoded = decode_compat_showformat_escapes(format);
    std::string rendered;
    size_t pos = 0;
    while (true) {
        size_t start = decoded.find("${", pos);
        if (start == std::string::npos) {
            rendered += decoded.substr(pos);
            break;
        }
        rendered += decoded.substr(pos, start - pos);
        size_t end = decoded.find('}', start + 2);
        if (end == std::string::npos) {
            rendered += decoded.substr(start);
            break;
        }

        std::string token = decoded.substr(start + 2, end - (start + 2));
        size_t semi = token.find(';');
        if (semi != std::string::npos) token = token.substr(0, semi);
        rendered += compat_package_field_value(info, token);
        pos = end + 1;
    }
    return rendered;
}

std::string compat_existing_package_control_path(
    const CompatDpkgPackageInfo& info,
    const std::string& control_name
) {
    const std::vector<std::string> maintscript_names = {"preinst", "postinst", "prerm", "postrm"};
    if (std::find(maintscript_names.begin(), maintscript_names.end(), control_name) != maintscript_names.end()) {
        std::string path = get_info_dir() + info.gpkg_name + "." + control_name;
        if (access(path.c_str(), F_OK) == 0) return path;
        return "";
    }

    std::string sidecar_path = get_debian_control_sidecar_path(info.gpkg_name, control_name);
    if (access(sidecar_path.c_str(), F_OK) == 0) return sidecar_path;
    return "";
}

void compat_append_existing_regular_file(
    const std::string& path,
    std::vector<std::string>& out,
    std::set<std::string>& seen
) {
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return;
    if (seen.insert(path).second) out.push_back(path);
}

std::vector<std::string> compat_candidate_package_names(const CompatDpkgPackageInfo& info) {
    std::vector<std::string> names;
    std::set<std::string> seen;
    const std::string candidates[] = {info.gpkg_name, info.debian_name};
    for (const auto& candidate : candidates) {
        std::string trimmed = trim(candidate);
        if (trimmed.empty()) continue;
        if (seen.insert(trimmed).second) names.push_back(trimmed);
    }
    return names;
}

bool compat_filename_matches_debian_archive(
    const std::string& filename,
    const CompatDpkgPackageInfo& info
) {
    if (!GpkgArchive::path_has_suffix(filename, ".deb")) return false;
    std::vector<std::string> names = compat_candidate_package_names(info);
    if (names.empty()) return false;

    bool name_match = false;
    for (const auto& name : names) {
        if (filename.rfind(name + "_", 0) == 0) {
            name_match = true;
            break;
        }
    }
    if (!name_match) return false;

    if (!info.version.empty()) {
        std::string version_token = "_" + safe_repo_filename_component_local(info.version) + "_";
        if (filename.find(version_token) == std::string::npos &&
            filename.find("_" + info.version + "_") == std::string::npos) {
            return false;
        }
    }
    return true;
}

bool compat_filename_matches_imported_archive(
    const std::string& filename,
    const CompatDpkgPackageInfo& info
) {
    const std::string extension = ".gpkg";
    if (!GpkgArchive::path_has_suffix(filename, extension)) return false;
    std::vector<std::string> names = compat_candidate_package_names(info);
    if (names.empty()) return false;

    std::string stem = filename.substr(0, filename.size() - extension.size());
    bool name_match = false;
    for (const auto& name : names) {
        std::string safe_name = cache_safe_component_local(name);
        if (stem.rfind(safe_name + "_", 0) == 0) {
            name_match = true;
            break;
        }
    }
    if (!name_match) return false;

    if (!info.version.empty()) {
        std::string safe_version = safe_repo_filename_component_local(info.version);
        if (stem.find("_" + safe_version + "_") == std::string::npos &&
            stem.find("_" + info.version + "_") == std::string::npos) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> compat_find_cached_debian_archive_paths(const CompatDpkgPackageInfo& info) {
    std::vector<std::string> candidates;
    std::set<std::string> seen;
    std::vector<std::string> package_names = compat_candidate_package_names(info);
    std::string basename = path_basename(info.filename);

    if (!basename.empty()) {
        for (const auto& name : package_names) {
            compat_append_existing_regular_file(
                get_debian_pool_cache_dir() + cache_safe_component_local(name) + "/" + basename,
                candidates,
                seen
            );
        }
    }

    DIR* pool = opendir(get_debian_pool_cache_dir().c_str());
    if (!pool) return candidates;
    while (true) {
        errno = 0;
        dirent* dir_entry = readdir(pool);
        if (!dir_entry) break;

        std::string dir_name = dir_entry->d_name;
        if (dir_name == "." || dir_name == "..") continue;
        std::string dir_path = get_debian_pool_cache_dir() + dir_name;
        struct stat dir_st {};
        if (stat(dir_path.c_str(), &dir_st) != 0 || !S_ISDIR(dir_st.st_mode)) continue;

        DIR* pkg_dir = opendir(dir_path.c_str());
        if (!pkg_dir) continue;
        while (true) {
            errno = 0;
            dirent* file_entry = readdir(pkg_dir);
            if (!file_entry) break;

            std::string filename = file_entry->d_name;
            if (filename == "." || filename == "..") continue;
            if (!basename.empty() && filename == basename) {
                compat_append_existing_regular_file(dir_path + "/" + filename, candidates, seen);
                continue;
            }
            if (!compat_filename_matches_debian_archive(filename, info)) continue;
            compat_append_existing_regular_file(dir_path + "/" + filename, candidates, seen);
        }
        closedir(pkg_dir);
    }
    closedir(pool);
    return candidates;
}

std::vector<std::string> compat_find_cached_imported_gpkg_paths(const CompatDpkgPackageInfo& info) {
    std::vector<std::string> candidates;
    std::set<std::string> seen;
    DIR* imported_root = opendir(get_imported_cache_dir().c_str());
    if (!imported_root) return candidates;

    while (true) {
        errno = 0;
        dirent* version_entry = readdir(imported_root);
        if (!version_entry) break;

        std::string version_dir_name = version_entry->d_name;
        if (version_dir_name == "." || version_dir_name == "..") continue;
        std::string version_dir = get_imported_cache_dir() + version_dir_name;
        struct stat version_st {};
        if (stat(version_dir.c_str(), &version_st) != 0 || !S_ISDIR(version_st.st_mode)) continue;

        DIR* source_root = opendir(version_dir.c_str());
        if (!source_root) continue;
        while (true) {
            errno = 0;
            dirent* source_entry = readdir(source_root);
            if (!source_entry) break;

            std::string source_dir_name = source_entry->d_name;
            if (source_dir_name == "." || source_dir_name == "..") continue;
            if (!info.source_kind.empty() &&
                source_dir_name != info.source_kind &&
                !(info.source_kind == "base_image" && source_dir_name == "debian")) {
                continue;
            }

            std::string source_dir = version_dir + "/" + source_dir_name;
            struct stat source_st {};
            if (stat(source_dir.c_str(), &source_st) != 0 || !S_ISDIR(source_st.st_mode)) continue;

            DIR* package_dir = opendir(source_dir.c_str());
            if (!package_dir) continue;
            while (true) {
                errno = 0;
                dirent* file_entry = readdir(package_dir);
                if (!file_entry) break;

                std::string filename = file_entry->d_name;
                if (filename == "." || filename == "..") continue;
                if (!compat_filename_matches_imported_archive(filename, info)) continue;
                compat_append_existing_regular_file(source_dir + "/" + filename, candidates, seen);
            }
            closedir(package_dir);
        }
        closedir(source_root);
    }
    closedir(imported_root);
    return candidates;
}

bool compat_locate_deb_member_archive(
    const std::string& directory,
    const std::string& prefix,
    std::string& out_path
) {
    out_path.clear();
    DIR* dir = opendir(directory.c_str());
    if (!dir) return false;
    while (true) {
        errno = 0;
        dirent* entry = readdir(dir);
        if (!entry) break;
        std::string name = entry->d_name;
        if (name.rfind(prefix, 0) != 0) continue;
        out_path = directory + "/" + name;
        closedir(dir);
        return true;
    }
    closedir(dir);
    return false;
}

bool compat_materialize_deb_control_tar(
    const std::string& archive_path,
    const std::string& output_path,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    if (GpkgArchive::path_has_suffix(archive_path, ".tar")) {
        if (!copy_file_atomic(archive_path, output_path)) {
            if (error_out) *error_out = "failed to copy control.tar";
            return false;
        }
        return true;
    }
    if (GpkgArchive::path_has_suffix(archive_path, ".tar.gz") || GpkgArchive::path_has_suffix(archive_path, ".tgz")) {
        return GpkgArchive::decompress_gzip_file(archive_path, output_path, error_out);
    }
    if (GpkgArchive::path_has_suffix(archive_path, ".tar.xz") || GpkgArchive::path_has_suffix(archive_path, ".tar.lzma")) {
        return GpkgArchive::decompress_xz_file(archive_path, output_path, error_out);
    }
    if (GpkgArchive::path_has_suffix(archive_path, ".tar.zst") || GpkgArchive::path_has_suffix(archive_path, ".tar.zstd")) {
        return GpkgArchive::decompress_zstd_file(archive_path, output_path, error_out);
    }
    if (error_out) *error_out = "unsupported control archive compression";
    return false;
}

bool compat_backfill_conffile_manifest(
    const CompatDpkgPackageInfo& info,
    const std::string& control_root
) {
    std::string conffile_manifest = get_conffile_manifest_path(info.gpkg_name);
    if (access(conffile_manifest.c_str(), F_OK) == 0) return true;

    std::string source_path = control_root + "/conffiles";
    struct stat st {};
    if (stat(source_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return true;

    std::vector<std::string> conffiles = read_trimmed_line_file(source_path);
    std::ostringstream out;
    for (const auto& entry : conffiles) {
        if (!entry.empty()) out << entry << "\n";
    }
    return write_text_file_atomic(conffile_manifest, out.str(), 0644);
}

bool compat_install_control_metadata_from_dirs(
    const CompatDpkgPackageInfo& info,
    const std::string& control_root,
    const std::string& scripts_root,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    if (!mkdir_p(get_info_dir())) {
        if (error_out) *error_out = "failed to create gpkg info dir";
        return false;
    }

    const std::vector<std::string> maintscript_names = {"preinst", "postinst", "prerm", "postrm"};
    std::set<std::string> maintscript_set(maintscript_names.begin(), maintscript_names.end());
    std::vector<std::string> extracted_names = collect_extracted_debian_control_sidecar_names(control_root);
    std::vector<std::string> sidecar_names;
    for (const auto& name : extracted_names) {
        if (maintscript_set.count(name) != 0) continue;
        sidecar_names.push_back(name);
    }

    bool changed = false;
    for (const auto& sidecar_name : sidecar_names) {
        std::string source_path = control_root + "/" + sidecar_name;
        std::string target_path = get_debian_control_sidecar_path(info.gpkg_name, sidecar_name);
        if (!copy_file_atomic(source_path, target_path)) {
            if (error_out) *error_out = "failed to install control sidecar " + sidecar_name;
            return false;
        }
        changed = true;
    }

    std::ostringstream manifest;
    for (const auto& sidecar_name : sidecar_names) manifest << sidecar_name << "\n";
    if (!sidecar_names.empty() &&
        !write_text_file_atomic(get_debian_control_sidecar_manifest_path(info.gpkg_name), manifest.str(), 0644)) {
        if (error_out) *error_out = "failed to write control sidecar manifest";
        return false;
    }

    for (const auto& script_name : maintscript_names) {
        std::string source_path = scripts_root + "/" + script_name;
        struct stat st {};
        if (stat(source_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            source_path = control_root + "/" + script_name;
            if (stat(source_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        }

        std::string target_path = get_info_dir() + info.gpkg_name + "." + script_name;
        if (access(target_path.c_str(), F_OK) == 0) continue;
        if (!copy_file_atomic(source_path, target_path)) {
            if (error_out) *error_out = "failed to backfill maintainer script " + script_name;
            return false;
        }
        changed = true;
    }

    if (!compat_backfill_conffile_manifest(info, control_root)) {
        if (error_out) *error_out = "failed to backfill conffile manifest";
        return false;
    }

    return changed || !sidecar_names.empty();
}

bool compat_backfill_control_metadata_from_cached_deb(
    const CompatDpkgPackageInfo& info,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    for (const auto& deb_path : compat_find_cached_debian_archive_paths(info)) {
        char temp_template[] = "/tmp/gpkg-compat-deb-XXXXXX";
        char* temp_dir = mkdtemp(temp_template);
        if (!temp_dir) continue;

        std::string temp_root = temp_dir;
        std::string ar_error;
        bool ok = GpkgArchive::extract_ar_archive_to_directory(deb_path, temp_root, &ar_error);
        if (!ok) {
            remove_tree_no_follow(temp_root);
            continue;
        }

        std::string control_archive;
        if (!compat_locate_deb_member_archive(temp_root, "control.tar", control_archive)) {
            remove_tree_no_follow(temp_root);
            continue;
        }

        std::string control_tar = temp_root + "/control.tar";
        std::string materialize_error;
        if (!compat_materialize_deb_control_tar(control_archive, control_tar, &materialize_error)) {
            remove_tree_no_follow(temp_root);
            continue;
        }

        std::string control_root = temp_root + "/control";
        if (!mkdir_p(control_root)) {
            remove_tree_no_follow(temp_root);
            continue;
        }

        std::string archive_error;
        if (!GpkgArchive::tar_extract_to_directory(control_tar, control_root, {}, &archive_error)) {
            remove_tree_no_follow(temp_root);
            continue;
        }

        std::string install_error;
        bool installed = compat_install_control_metadata_from_dirs(
            info,
            control_root,
            control_root,
            &install_error
        );
        remove_tree_no_follow(temp_root);
        if (installed) return true;
        if (error_out && !install_error.empty()) *error_out = install_error;
    }
    return false;
}

bool compat_backfill_control_metadata_from_imported_gpkg(
    const CompatDpkgPackageInfo& info,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    for (const auto& gpkg_path : compat_find_cached_imported_gpkg_paths(info)) {
        char temp_template[] = "/tmp/gpkg-compat-import-XXXXXX";
        char* temp_dir = mkdtemp(temp_template);
        if (!temp_dir) continue;

        std::string temp_root = temp_dir;
        std::string package_tar = temp_root + "/package.tar";
        std::string archive_error;
        if (!GpkgArchive::decompress_zstd_file(gpkg_path, package_tar, &archive_error) ||
            !GpkgArchive::tar_extract_to_directory(package_tar, temp_root, {}, &archive_error)) {
            remove_tree_no_follow(temp_root);
            continue;
        }

        std::string install_error;
        bool installed = compat_install_control_metadata_from_dirs(
            info,
            temp_root + "/control",
            temp_root + "/scripts",
            &install_error
        );
        remove_tree_no_follow(temp_root);
        if (installed) return true;
        if (error_out && !install_error.empty()) *error_out = install_error;
    }
    return false;
}

bool ensure_compat_package_control_metadata(
    const CompatDpkgPackageInfo& info,
    const std::string& requested_control_name = ""
) {
    if (!requested_control_name.empty() &&
        !compat_existing_package_control_path(info, requested_control_name).empty()) {
        return true;
    }

    std::string error_detail;
    if (compat_backfill_control_metadata_from_cached_deb(info, &error_detail)) {
        VLOG("Backfilled Debian control metadata for " << info.gpkg_name << " from cached .deb.");
        return true;
    }
    if (compat_backfill_control_metadata_from_imported_gpkg(info, &error_detail)) {
        VLOG("Backfilled Debian control metadata for " << info.gpkg_name << " from cached imported package.");
        return true;
    }
    if (g_verbose && !error_detail.empty()) {
        VLOG("Unable to backfill Debian control metadata for " << info.gpkg_name << ": " << error_detail);
    }
    return false;
}

std::string compat_package_control_path(
    const CompatDpkgPackageInfo& info,
    const std::string& control_name
) {
    std::string existing_path = compat_existing_package_control_path(info, control_name);
    if (!existing_path.empty()) return existing_path;
    if (!ensure_compat_package_control_metadata(info, control_name)) return "";
    return compat_existing_package_control_path(info, control_name);
}

std::vector<std::string> compat_package_control_names(const CompatDpkgPackageInfo& info) {
    ensure_compat_package_control_metadata(info);
    std::vector<std::string> names = load_debian_control_sidecar_names(info.gpkg_name);
    const std::vector<std::string> maintscript_names = {"preinst", "postinst", "prerm", "postrm"};
    for (const auto& control_name : maintscript_names) {
        if (!compat_existing_package_control_path(info, control_name).empty()) names.push_back(control_name);
    }
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

struct CompatDebianVersion {
    long long epoch = 0;
    std::string upstream;
    std::string revision;
};

int compat_debian_char_order(char c) {
    if (c == '~') return -1;
    if (c == '\0') return 0;
    if (std::isalpha(static_cast<unsigned char>(c))) return static_cast<unsigned char>(c);
    return static_cast<unsigned char>(c) + 256;
}

int compat_compare_debian_part(const std::string& left, const std::string& right) {
    size_t i = 0;
    size_t j = 0;

    while (i < left.size() || j < right.size()) {
        while ((i < left.size() && !std::isdigit(static_cast<unsigned char>(left[i]))) ||
               (j < right.size() && !std::isdigit(static_cast<unsigned char>(right[j])))) {
            char lc = (i < left.size() && !std::isdigit(static_cast<unsigned char>(left[i])))
                ? left[i]
                : '\0';
            char rc = (j < right.size() && !std::isdigit(static_cast<unsigned char>(right[j])))
                ? right[j]
                : '\0';
            int lo = compat_debian_char_order(lc);
            int ro = compat_debian_char_order(rc);
            if (lo < ro) return -1;
            if (lo > ro) return 1;
            if (lc != '\0') ++i;
            if (rc != '\0') ++j;
        }

        while (i < left.size() && left[i] == '0') ++i;
        while (j < right.size() && right[j] == '0') ++j;

        size_t left_digits_start = i;
        size_t right_digits_start = j;
        while (i < left.size() && std::isdigit(static_cast<unsigned char>(left[i]))) ++i;
        while (j < right.size() && std::isdigit(static_cast<unsigned char>(right[j]))) ++j;

        size_t left_digits_len = i - left_digits_start;
        size_t right_digits_len = j - right_digits_start;
        if (left_digits_len < right_digits_len) return -1;
        if (left_digits_len > right_digits_len) return 1;

        for (size_t k = 0; k < left_digits_len; ++k) {
            char lc = left[left_digits_start + k];
            char rc = right[right_digits_start + k];
            if (lc < rc) return -1;
            if (lc > rc) return 1;
        }
    }

    return 0;
}

CompatDebianVersion compat_parse_debian_version(const std::string& version) {
    CompatDebianVersion parsed;
    std::string remainder = version;

    size_t epoch_sep = version.find(':');
    if (epoch_sep != std::string::npos) {
        std::string epoch_str = version.substr(0, epoch_sep);
        if (!epoch_str.empty()) parsed.epoch = std::strtoll(epoch_str.c_str(), nullptr, 10);
        remainder = version.substr(epoch_sep + 1);
    }

    size_t revision_sep = remainder.rfind('-');
    if (revision_sep != std::string::npos) {
        parsed.upstream = remainder.substr(0, revision_sep);
        parsed.revision = remainder.substr(revision_sep + 1);
    } else {
        parsed.upstream = remainder;
        parsed.revision.clear();
    }

    return parsed;
}

int compat_compare_versions(const std::string& left, const std::string& right) {
    if (left == right) return 0;

    CompatDebianVersion parsed_left = compat_parse_debian_version(left);
    CompatDebianVersion parsed_right = compat_parse_debian_version(right);
    if (parsed_left.epoch < parsed_right.epoch) return -1;
    if (parsed_left.epoch > parsed_right.epoch) return 1;

    int upstream_cmp = compat_compare_debian_part(parsed_left.upstream, parsed_right.upstream);
    if (upstream_cmp != 0) return upstream_cmp;
    return compat_compare_debian_part(parsed_left.revision, parsed_right.revision);
}

bool compat_version_relation_matches(int cmp, const std::string& relation) {
    if (relation == "lt" || relation == "<<") return cmp < 0;
    if (relation == "le" || relation == "<=") return cmp <= 0;
    if (relation == "eq" || relation == "=") return cmp == 0;
    if (relation == "ne") return cmp != 0;
    if (relation == "ge" || relation == ">=") return cmp >= 0;
    if (relation == "gt" || relation == ">>") return cmp > 0;
    return false;
}

std::string resolve_real_dpkg_path() {
    const char* candidates[] = {
        "/usr/bin/dpkg",
        "/bin/dpkg",
        "/usr/sbin/dpkg",
        "/sbin/dpkg",
    };
    for (const char* candidate : candidates) {
        if (access(candidate, X_OK) == 0) return candidate;
    }
    return "";
}

int run_real_dpkg_with_args(const std::vector<std::string>& args) {
    std::string real_path = resolve_real_dpkg_path();
    if (real_path.empty()) return 1;

    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(real_path);
    argv.insert(argv.end(), args.begin(), args.end());
    return decode_command_exit_status(run_executable(argv));
}

int action_compat_dpkg(const std::vector<std::string>& raw_args) {
    if (raw_args.empty()) return run_real_dpkg_with_args(raw_args);

    std::vector<std::string> args;
    args.reserve(raw_args.size());
    for (size_t i = 0; i < raw_args.size(); ++i) {
        const std::string& arg = raw_args[i];
        if ((arg == "--admindir" || arg == "--root") && i + 1 < raw_args.size()) {
            ++i;
            continue;
        }
        if (arg.rfind("--admindir=", 0) == 0 || arg.rfind("--root=", 0) == 0) continue;
        args.push_back(arg);
    }

    if (args.empty()) return run_real_dpkg_with_args(raw_args);

    if (args.size() == 1 && args[0] == "--print-architecture") {
        std::cout << compat_debian_architecture_for_package_arch(native_package_architecture()) << std::endl;
        return 0;
    }
    if (args.size() == 1 && args[0] == "--print-foreign-architectures") {
        return 0;
    }
    if (args.size() >= 1 &&
        (args[0] == "--assert-multi-arch" || args[0] == "--assert-support-predepends")) {
        return 0;
    }
    if (args.size() >= 4 && args[0] == "--compare-versions") {
        int cmp = compat_compare_versions(args[1], args[3]);
        return compat_version_relation_matches(cmp, args[2]) ? 0 : 1;
    }

    return run_real_dpkg_with_args(raw_args);
}

int action_compat_dpkg_trigger(const std::vector<std::string>& raw_args) {
    bool check_supported = false;
    std::vector<std::string> trigger_names;

    for (size_t i = 0; i < raw_args.size(); ++i) {
        const std::string& arg = raw_args[i];
        if (arg == "--admindir" || arg == "--root" || arg == "--by-package") {
            if (i + 1 < raw_args.size()) ++i;
            continue;
        }
        if (arg.rfind("--admindir=", 0) == 0 ||
            arg.rfind("--root=", 0) == 0 ||
            arg.rfind("--by-package=", 0) == 0) {
            continue;
        }
        if (arg == "--check-supported") {
            check_supported = true;
            continue;
        }
        if (arg == "--no-await" || arg == "--await" || arg == "--no-act") continue;
        if (!arg.empty() && arg[0] == '-') continue;
        trigger_names.push_back(arg);
    }

    if (check_supported) return 0;
    if (trigger_names.empty()) {
        std::cerr << "dpkg-trigger: missing trigger name" << std::endl;
        return 1;
    }

    for (const auto& trigger_name : trigger_names) {
        std::string queue_error;
        if (!append_pending_dpkg_trigger_name(trigger_name, &queue_error)) {
            if (!queue_error.empty()) {
                std::cerr << "dpkg-trigger: " << queue_error << std::endl;
            }
            return 1;
        }
    }

    mark_packages_trigger_pending(trigger_names);
    return 0;
}

std::string resolve_real_dpkg_query_path() {
    const char* candidates[] = {
        "/usr/bin/dpkg-query",
        "/bin/dpkg-query",
        "/usr/sbin/dpkg-query",
        "/sbin/dpkg-query",
    };
    for (const char* candidate : candidates) {
        if (access(candidate, X_OK) == 0) return candidate;
    }
    return "";
}

int run_real_dpkg_query_with_args(const std::vector<std::string>& args) {
    std::string real_path = resolve_real_dpkg_query_path();
    if (real_path.empty()) return 1;

    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(real_path);
    argv.insert(argv.end(), args.begin(), args.end());
    return decode_command_exit_status(run_executable(argv));
}

bool compat_find_owned_paths(
    const std::string& pattern,
    std::vector<std::pair<std::string, std::string>>& matches_out
) {
    bool has_glob = compat_pattern_has_glob(pattern);
    bool matched = false;
    for (const auto& info : collect_all_compat_dpkg_packages()) {
        for (const auto& path : load_compat_package_file_list(info)) {
            if ((has_glob && compat_wildcard_match(path, pattern)) || (!has_glob && path == pattern)) {
                matches_out.push_back({info.debian_name.empty() ? info.gpkg_name : info.debian_name, path});
                matched = true;
            }
        }
    }
    return matched;
}

int action_compat_dpkg_query(const std::vector<std::string>& raw_args) {
    if (raw_args.empty()) return run_real_dpkg_query_with_args(raw_args);

    std::string command;
    std::string showformat;
    std::vector<std::string> operands;

    for (size_t i = 0; i < raw_args.size(); ++i) {
        const std::string& arg = raw_args[i];
        if (arg == "--no-pager") continue;
        if ((arg == "-f" || arg == "--showformat") && i + 1 < raw_args.size()) {
            showformat = raw_args[++i];
            continue;
        }
        if (arg.rfind("--showformat=", 0) == 0) {
            showformat = arg.substr(std::string("--showformat=").size());
            continue;
        }
        if (arg.rfind("--admindir=", 0) == 0 || arg.rfind("--root=", 0) == 0) continue;
        if (command.empty() &&
            (arg == "-W" || arg == "--show" ||
             arg == "-L" || arg == "--listfiles" ||
             arg == "-S" || arg == "--search" ||
             arg == "-l" || arg == "--list" ||
             arg == "-c" || arg == "--control-path" ||
             arg == "--control-list" ||
             arg == "--control-show")) {
            command = arg;
            continue;
        }
        operands.push_back(arg);
    }

    if (command.empty()) return run_real_dpkg_query_with_args(raw_args);

    if (command == "-c" || command == "--control-path") {
        if (operands.empty()) return run_real_dpkg_query_with_args(raw_args);
        CompatDpkgPackageInfo info;
        if (!resolve_compat_dpkg_package(operands[0], info)) {
            return run_real_dpkg_query_with_args(raw_args);
        }
        std::string control_name = operands.size() > 1 ? operands[1] : "control";
        std::string path = compat_package_control_path(info, control_name);
        if (path.empty()) {
            std::cerr << "dpkg-query: error: control file '" << control_name
                      << "' is not available for package '"
                      << (info.debian_name.empty() ? info.gpkg_name : info.debian_name)
                      << "'" << std::endl;
            return 1;
        }
        std::cout << path << std::endl;
        return 0;
    }

    if (command == "--control-list") {
        if (operands.empty()) return run_real_dpkg_query_with_args(raw_args);
        CompatDpkgPackageInfo info;
        if (!resolve_compat_dpkg_package(operands[0], info)) {
            return run_real_dpkg_query_with_args(raw_args);
        }
        for (const auto& name : compat_package_control_names(info)) {
            std::cout << name << std::endl;
        }
        return 0;
    }

    if (command == "--control-show") {
        if (operands.size() < 2) return run_real_dpkg_query_with_args(raw_args);
        CompatDpkgPackageInfo info;
        if (!resolve_compat_dpkg_package(operands[0], info)) {
            return run_real_dpkg_query_with_args(raw_args);
        }
        std::string path = compat_package_control_path(info, operands[1]);
        if (path.empty()) {
            std::cerr << "dpkg-query: error: control file '" << operands[1]
                      << "' is not available for package '"
                      << (info.debian_name.empty() ? info.gpkg_name : info.debian_name)
                      << "'" << std::endl;
            return 1;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) return 1;
        std::cout << in.rdbuf();
        return in.good() || in.eof() ? 0 : 1;
    }

    if (command == "-L" || command == "--listfiles") {
        bool printed = false;
        std::vector<std::string> real_operands;
        for (const auto& operand : operands) {
            CompatDpkgPackageInfo info;
            if (!resolve_compat_dpkg_package(operand, info)) {
                real_operands.push_back(operand);
                continue;
            }
            for (const auto& path : load_compat_package_file_list(info)) {
                std::cout << path << std::endl;
            }
            printed = true;
        }

        int real_rc = 1;
        if (!real_operands.empty()) {
            std::vector<std::string> real_args = {command};
            real_args.insert(real_args.end(), real_operands.begin(), real_operands.end());
            real_rc = run_real_dpkg_query_with_args(real_args);
        }
        return printed ? 0 : real_rc;
    }

    if (command == "-S" || command == "--search") {
        bool printed = false;
        std::vector<std::string> real_operands;
        for (const auto& operand : operands) {
            std::vector<std::pair<std::string, std::string>> matches;
            if (!compat_find_owned_paths(operand, matches)) {
                real_operands.push_back(operand);
                continue;
            }
            for (const auto& match : matches) {
                std::cout << match.first << ": " << match.second << std::endl;
            }
            printed = true;
        }

        int real_rc = 1;
        if (!real_operands.empty()) {
            std::vector<std::string> real_args = {command};
            real_args.insert(real_args.end(), real_operands.begin(), real_operands.end());
            real_rc = run_real_dpkg_query_with_args(real_args);
        }
        return printed ? 0 : real_rc;
    }

    if (command == "-W" || command == "--show") {
        std::string effective_format = showformat.empty()
            ? "${binary:Package}\t${Version}\n"
            : showformat;
        bool printed = false;
        std::vector<std::string> real_operands;
        std::set<std::string> emitted;
        std::vector<std::string> patterns = operands.empty()
            ? std::vector<std::string>{"*"}
            : operands;

        for (const auto& pattern : patterns) {
            std::vector<CompatDpkgPackageInfo> matches = match_compat_dpkg_packages(pattern);
            if (matches.empty()) {
                real_operands.push_back(pattern);
                continue;
            }
            for (const auto& info : matches) {
                if (!emitted.insert(info.gpkg_name).second) continue;
                std::cout << render_compat_showformat(effective_format, info);
                printed = true;
            }
        }

        int real_rc = 1;
        if (!real_operands.empty()) {
            std::vector<std::string> real_args = {command};
            if (!showformat.empty()) real_args.push_back("--showformat=" + showformat);
            real_args.insert(real_args.end(), real_operands.begin(), real_operands.end());
            real_rc = run_real_dpkg_query_with_args(real_args);
        }
        return printed ? 0 : real_rc;
    }

    if (command == "-l" || command == "--list") {
        bool printed = false;
        std::set<std::string> emitted;
        std::vector<std::string> patterns = operands.empty()
            ? std::vector<std::string>{"*"}
            : operands;
        std::vector<std::string> real_operands;

        if (operands.empty()) {
            run_real_dpkg_query_with_args({command});
        }

        for (const auto& pattern : patterns) {
            std::vector<CompatDpkgPackageInfo> matches = match_compat_dpkg_packages(pattern);
            if (matches.empty()) {
                if (!operands.empty()) real_operands.push_back(pattern);
                continue;
            }
            for (const auto& info : matches) {
                if (!emitted.insert(info.gpkg_name).second) continue;
                std::string summary = info.description;
                size_t newline = summary.find('\n');
                if (newline != std::string::npos) summary = summary.substr(0, newline);
                std::cout << compat_package_status_abbrev(info) << "  "
                          << (info.debian_name.empty() ? info.gpkg_name : info.debian_name) << " "
                          << info.version << " " << info.arch << " "
                          << summary << std::endl;
                printed = true;
            }
        }

        if (!real_operands.empty()) {
            std::vector<std::string> real_args = {command};
            real_args.insert(real_args.end(), real_operands.begin(), real_operands.end());
            int real_rc = run_real_dpkg_query_with_args(real_args);
            return printed ? 0 : real_rc;
        }

        return printed ? 0 : 1;
    }

    return run_real_dpkg_query_with_args(raw_args);
}

std::string compat_etc_directory() {
    return g_root_prefix + "/etc";
}

std::string compat_init_script_path(const std::string& service_name) {
    return compat_etc_directory() + "/init.d/" + service_name;
}

std::vector<std::string> compat_rc_directories() {
    const char* suffixes[] = {"0", "1", "2", "3", "4", "5", "6", "S"};
    std::vector<std::string> dirs;
    dirs.reserve(sizeof(suffixes) / sizeof(suffixes[0]));
    for (const char* suffix : suffixes) {
        dirs.push_back(compat_etc_directory() + "/rc" + std::string(suffix) + ".d");
    }
    return dirs;
}

bool compat_is_service_action_suppressed(const std::string& action) {
    return action == "start" ||
           action == "restart" ||
           action == "try-restart" ||
           action == "reload" ||
           action == "force-reload" ||
           action == "reload-or-restart" ||
           action == "reload-or-try-restart" ||
           action == "condrestart";
}

bool compat_is_runlevel_token(const std::string& token) {
    if (token.size() != 1) return false;
    char runlevel = token[0];
    return (runlevel >= '0' && runlevel <= '6') || runlevel == 'S';
}

bool compat_rc_entry_matches_service(const std::string& entry_name, const std::string& service_name) {
    return entry_name.size() == service_name.size() + 3 &&
           (entry_name[0] == 'S' || entry_name[0] == 'K') &&
           std::isdigit(static_cast<unsigned char>(entry_name[1])) &&
           std::isdigit(static_cast<unsigned char>(entry_name[2])) &&
           entry_name.substr(3) == service_name;
}

std::string compat_logical_path_from_full_path(const std::string& full_path) {
    if (!g_root_prefix.empty() && full_path.rfind(g_root_prefix, 0) == 0) {
        std::string logical = full_path.substr(g_root_prefix.size());
        if (logical.empty()) return "/";
        return logical[0] == '/' ? logical : "/" + logical;
    }
    return full_path;
}

bool compat_rc_entry_targets_service(
    const std::string& entry_path,
    const std::string& service_name
) {
    struct stat st {};
    if (lstat(entry_path.c_str(), &st) != 0 || !S_ISLNK(st.st_mode)) return false;

    std::string symlink_target = read_symlink_target(entry_path);
    if (symlink_target.empty()) return false;

    std::string resolved_target = resolve_logical_symlink_target(
        compat_logical_path_from_full_path(entry_path),
        symlink_target
    );
    return resolved_target == "/etc/init.d/" + service_name;
}

bool compat_service_has_registered_links(const std::string& service_name) {
    for (const auto& dir_path : compat_rc_directories()) {
        DIR* dir = opendir(dir_path.c_str());
        if (!dir) continue;

        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            if (compat_rc_entry_matches_service(name, service_name) &&
                compat_rc_entry_targets_service(dir_path + "/" + name, service_name)) {
                closedir(dir);
                return true;
            }
        }
        closedir(dir);
    }
    return false;
}

bool compat_remove_registered_links(const std::string& service_name) {
    for (const auto& dir_path : compat_rc_directories()) {
        DIR* dir = opendir(dir_path.c_str());
        if (!dir) continue;

        std::vector<std::string> to_remove;
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            std::string full_path = dir_path + "/" + name;
            if (compat_rc_entry_matches_service(name, service_name) &&
                compat_rc_entry_targets_service(full_path, service_name)) {
                to_remove.push_back(full_path);
            }
        }
        closedir(dir);

        for (const auto& full_path : to_remove) {
            if (unlink(full_path.c_str()) != 0 && errno != ENOENT) {
                std::cerr << "update-rc.d: unable to remove " << full_path
                          << ": " << strerror(errno) << std::endl;
                return false;
            }
        }
    }
    return true;
}

bool compat_service_init_script_exists(const std::string& service_name) {
    struct stat st {};
    return lstat(compat_init_script_path(service_name).c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
}

std::string compat_rc_entry_name(char kind, int priority, const std::string& service_name) {
    std::ostringstream name;
    name << kind << std::setw(2) << std::setfill('0') << priority << service_name;
    return name.str();
}

bool compat_install_rc_link(
    char runlevel,
    char kind,
    int priority,
    const std::string& service_name
) {
    std::string dir_path = compat_etc_directory() + "/rc" + std::string(1, runlevel) + ".d";
    if (!mkdir_p(dir_path)) {
        std::cerr << "update-rc.d: unable to create " << dir_path
                  << ": " << strerror(errno) << std::endl;
        return false;
    }

    std::string entry_name = compat_rc_entry_name(kind, priority, service_name);
    std::string entry_path = dir_path + "/" + entry_name;
    std::string expected_target = "../init.d/" + service_name;

    struct stat st {};
    if (lstat(entry_path.c_str(), &st) == 0) {
        if (S_ISLNK(st.st_mode) && read_symlink_target(entry_path) == expected_target) {
            return true;
        }
        if (!remove_tree_no_follow(entry_path)) {
            std::cerr << "update-rc.d: unable to replace " << entry_path
                      << ": " << strerror(errno) << std::endl;
            return false;
        }
    } else if (errno != ENOENT) {
        std::cerr << "update-rc.d: unable to inspect " << entry_path
                  << ": " << strerror(errno) << std::endl;
        return false;
    }

    if (symlink(expected_target.c_str(), entry_path.c_str()) != 0) {
        std::cerr << "update-rc.d: unable to create " << entry_path
                  << ": " << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

// Native maintainer-script shims keep Debian service helpers working even when
// the image does not ship the traditional SysV management utilities.
struct CompatUpdateRcLinkSpec {
    char kind = 'S';
    int priority = 20;
    std::vector<char> runlevels;
};

std::vector<CompatUpdateRcLinkSpec> compat_default_update_rc_specs(bool disabled) {
    std::vector<CompatUpdateRcLinkSpec> specs;

    CompatUpdateRcLinkSpec start_spec;
    start_spec.kind = disabled ? 'K' : 'S';
    start_spec.priority = disabled ? 80 : 20;
    start_spec.runlevels = {'2', '3', '4', '5'};
    specs.push_back(start_spec);

    CompatUpdateRcLinkSpec stop_spec;
    stop_spec.kind = 'K';
    stop_spec.priority = 20;
    stop_spec.runlevels = {'0', '1', '6'};
    specs.push_back(stop_spec);

    return specs;
}

bool compat_apply_update_rc_specs(
    const std::string& service_name,
    const std::vector<CompatUpdateRcLinkSpec>& specs
) {
    for (const auto& spec : specs) {
        if (spec.priority < 0 || spec.priority > 99) {
            std::cerr << "update-rc.d: unsupported sequence " << spec.priority << std::endl;
            return false;
        }
        for (char runlevel : spec.runlevels) {
            if (!compat_install_rc_link(runlevel, spec.kind, spec.priority, service_name)) {
                return false;
            }
        }
    }
    return true;
}

bool compat_parse_update_rc_explicit_specs(
    const std::vector<std::string>& operands,
    size_t start_index,
    std::vector<CompatUpdateRcLinkSpec>* specs_out
) {
    if (!specs_out) return false;
    specs_out->clear();

    size_t index = start_index;
    while (index < operands.size()) {
        CompatUpdateRcLinkSpec spec;
        if (operands[index] == "start") spec.kind = 'S';
        else if (operands[index] == "stop") spec.kind = 'K';
        else return false;
        ++index;

        if (index >= operands.size() || !parse_small_int_local(operands[index], &spec.priority) ||
            spec.priority > 99) {
            return false;
        }
        ++index;

        while (index < operands.size() && operands[index] != ".") {
            if (!compat_is_runlevel_token(operands[index])) return false;
            spec.runlevels.push_back(operands[index][0]);
            ++index;
        }

        if (spec.runlevels.empty() || index >= operands.size() || operands[index] != ".") {
            return false;
        }
        ++index;
        specs_out->push_back(spec);
    }

    return !specs_out->empty();
}

bool compat_toggle_registered_service_links(const std::string& service_name, bool enable) {
    for (const auto& dir_path : compat_rc_directories()) {
        std::string dir_name = path_basename(dir_path);
        if (dir_name.size() != 5 || dir_name.rfind("rc", 0) != 0 || dir_name.substr(3) != ".d") continue;
        char runlevel = dir_name[2];
        if (runlevel == '0' || runlevel == '1' || runlevel == '6') continue;

        DIR* dir = opendir(dir_path.c_str());
        if (!dir) continue;

        std::vector<std::pair<std::string, std::string>> renames;
        struct dirent* entry = nullptr;
        while ((entry = readdir(dir)) != nullptr) {
            std::string current_name = entry->d_name;
            if (!compat_rc_entry_matches_service(current_name, service_name)) continue;
            std::string current_path = dir_path + "/" + current_name;
            if (!compat_rc_entry_targets_service(current_path, service_name)) continue;

            char current_kind = current_name[0];
            if ((enable && current_kind != 'K') || (!enable && current_kind != 'S')) continue;

            int current_priority =
                (current_name[1] - '0') * 10 +
                (current_name[2] - '0');
            int new_priority = 100 - current_priority;
            if (new_priority < 0) new_priority = 0;
            if (new_priority > 99) new_priority = 99;

            std::string new_name = compat_rc_entry_name(enable ? 'S' : 'K', new_priority, service_name);
            if (new_name == current_name) continue;
            renames.push_back({dir_path + "/" + current_name, dir_path + "/" + new_name});
        }
        closedir(dir);

        for (const auto& rename_pair : renames) {
            remove_tree_no_follow(rename_pair.second);
            if (rename(rename_pair.first.c_str(), rename_pair.second.c_str()) != 0) {
                std::cerr << "update-rc.d: unable to rename " << rename_pair.first
                          << " to " << rename_pair.second << ": "
                          << strerror(errno) << std::endl;
                return false;
            }
        }
    }
    return true;
}

bool compat_parse_service_invocation(
    const std::vector<std::string>& raw_args,
    std::string* service_name_out,
    std::string* action_out,
    std::vector<std::string>* script_args_out
) {
    if (service_name_out) service_name_out->clear();
    if (action_out) action_out->clear();
    if (script_args_out) script_args_out->clear();

    std::vector<std::string> positional;
    for (const auto& arg : raw_args) {
        if (!arg.empty() && arg[0] == '-') continue;
        positional.push_back(arg);
    }

    if (positional.size() < 2) return false;

    std::string service_name = positional[0];
    std::string action = positional[1];
    if (service_name.empty() || action.empty()) return false;

    if (service_name_out) *service_name_out = service_name;
    if (action_out) *action_out = action;

    if (script_args_out) {
        script_args_out->push_back(action);
        for (size_t i = 2; i < positional.size(); ++i) {
            script_args_out->push_back(positional[i]);
        }
    }

    return true;
}

struct CompatInvokeRcOptions {
    bool disclose_deny = false;
    bool query = false;
};

CompatInvokeRcOptions parse_compat_invoke_rc_options(const std::vector<std::string>& raw_args) {
    CompatInvokeRcOptions options;
    for (const auto& arg : raw_args) {
        if (arg == "--disclose-deny") options.disclose_deny = true;
        if (arg == "--query") {
            options.query = true;
            options.disclose_deny = true;
        }
    }
    return options;
}

int compat_service_denied_exit_code(const CompatInvokeRcOptions& options) {
    return options.disclose_deny ? 101 : 0;
}

std::string resolve_first_executable(const std::vector<std::string>& candidates) {
    for (const auto& candidate : candidates) {
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }
    return "";
}

int run_real_executable_with_args(
    const std::vector<std::string>& candidates,
    const std::vector<std::string>& args
) {
    std::string path = resolve_first_executable(candidates);
    if (path.empty()) return 1;

    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(path);
    argv.insert(argv.end(), args.begin(), args.end());
    return decode_command_exit_status(run_executable(argv));
}

bool compat_system_service_action_is_mutating(const std::string& action) {
    return compat_is_service_action_suppressed(action) ||
           action == "stop" ||
           action == "kill";
}

int action_compat_update_rc_d(const std::vector<std::string>& raw_args) {
    bool force = false;
    std::vector<std::string> operands;
    operands.reserve(raw_args.size());

    for (const auto& arg : raw_args) {
        if (arg == "-f" || arg == "--force") {
            force = true;
            continue;
        }
        if (!arg.empty() && arg[0] == '-') continue;
        operands.push_back(arg);
    }

    if (operands.size() < 2) {
        std::cerr << "update-rc.d: missing service name or command" << std::endl;
        return 1;
    }

    const std::string& service_name = operands[0];
    const std::string& command = operands[1];
    bool init_script_exists = compat_service_init_script_exists(service_name);

    if (command == "remove") {
        if (init_script_exists && !force) {
            std::cerr << "update-rc.d: refusing to remove " << service_name
                      << " because /etc/init.d/" << service_name
                      << " still exists; use -f to force removal" << std::endl;
            return 1;
        }
        return compat_remove_registered_links(service_name) ? 0 : 1;
    }

    if (!init_script_exists) {
        std::cerr << "update-rc.d: /etc/init.d/" << service_name << " is missing" << std::endl;
        return 1;
    }

    if (command == "defaults" || command == "defaults-disabled") {
        if (compat_service_has_registered_links(service_name)) return 0;
        return compat_apply_update_rc_specs(
            service_name,
            compat_default_update_rc_specs(command == "defaults-disabled")
        ) ? 0 : 1;
    }

    if (command == "disable" || command == "enable") {
        return compat_toggle_registered_service_links(service_name, command == "enable") ? 0 : 1;
    }

    std::vector<CompatUpdateRcLinkSpec> specs;
    if (!compat_parse_update_rc_explicit_specs(operands, 1, &specs)) {
        std::cerr << "update-rc.d: unsupported arguments";
        for (const auto& arg : raw_args) std::cerr << " " << arg;
        std::cerr << std::endl;
        return 1;
    }
    if (compat_service_has_registered_links(service_name)) return 0;
    return compat_apply_update_rc_specs(service_name, specs) ? 0 : 1;
}

int action_compat_service_like(const std::vector<std::string>& raw_args) {
    CompatInvokeRcOptions options = parse_compat_invoke_rc_options(raw_args);
    std::string service_name;
    std::string action;
    std::vector<std::string> script_args;
    if (!compat_parse_service_invocation(raw_args, &service_name, &action, &script_args)) {
        std::cerr << "invoke-rc.d: missing service name or action" << std::endl;
        return 1;
    }

    if (!g_root_prefix.empty()) {
        return options.query ? 101 : compat_service_denied_exit_code(options);
    }

    if (compat_is_service_action_suppressed(action) &&
        env_var_is_truthy(getenv("GPKG_DEFER_SERVICES"))) {
        return options.query ? 101 : compat_service_denied_exit_code(options);
    }

    std::string script_path = compat_init_script_path(service_name);
    struct stat st {};
    if (lstat(script_path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        return options.query ? 100 : 0;
    }

    if (options.query) return 104;

    const char* shell_path = access("/bin/sh", X_OK) == 0 ? "/bin/sh" : nullptr;
    if (!shell_path) {
        std::cerr << "invoke-rc.d: unable to execute /bin/sh for " << service_name << std::endl;
        return 1;
    }

    std::vector<std::string> argv;
    argv.reserve(script_args.size() + 2);
    argv.push_back(shell_path);
    argv.push_back(script_path);
    argv.insert(argv.end(), script_args.begin(), script_args.end());
    return decode_command_exit_status(run_executable(argv));
}

int action_compat_invoke_rc_d(const std::vector<std::string>& raw_args) {
    return action_compat_service_like(raw_args);
}

int action_compat_service(const std::vector<std::string>& raw_args) {
    return action_compat_service_like(raw_args);
}

std::string compat_extract_first_non_option(const std::vector<std::string>& raw_args) {
    for (const auto& arg : raw_args) {
        if (!arg.empty() && arg[0] == '-') continue;
        return arg;
    }
    return "";
}

int action_compat_systemctl_like(
    const std::vector<std::string>& raw_args,
    const std::vector<std::string>& real_candidates
) {
    std::string action = compat_extract_first_non_option(raw_args);
    bool suppressed =
        g_root_prefix.empty()
            ? (compat_system_service_action_is_mutating(action) &&
               env_var_is_truthy(getenv("GPKG_DEFER_SERVICES")))
            : true;

    if (suppressed) return 0;

    int real_rc = run_real_executable_with_args(real_candidates, raw_args);
    if (real_rc == 1 && resolve_first_executable(real_candidates).empty()) return 0;
    return real_rc;
}

int action_compat_systemctl(const std::vector<std::string>& raw_args) {
    return action_compat_systemctl_like(
        raw_args,
        {"/usr/bin/systemctl", "/bin/systemctl"}
    );
}

int action_compat_deb_systemd_invoke(const std::vector<std::string>& raw_args) {
    return action_compat_systemctl_like(
        raw_args,
        {"/usr/bin/deb-systemd-invoke", "/bin/deb-systemd-invoke"}
    );
}

int action_compat_initctl(const std::vector<std::string>& raw_args) {
    return action_compat_systemctl_like(
        raw_args,
        {"/sbin/initctl", "/usr/sbin/initctl", "/bin/initctl", "/usr/bin/initctl"}
    );
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

    if (!g_unsafe_io && fsync(dest_fd) != 0) {
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
    if (ok && !g_unsafe_io && fsync(temp_fd) != 0) ok = false;
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
    if (ok && !g_unsafe_io && fsync(temp_fd) != 0) ok = false;
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

std::string transaction_mounts_manifest_path(const std::string& txn_dir) {
    return txn_dir + "/mounts.list";
}

std::string transaction_mounted_top_level_manifest_path(const std::string& txn_dir) {
    return txn_dir + "/mounted-top-level.list";
}

std::string transaction_initial_root_entries_manifest_path(const std::string& txn_dir) {
    return txn_dir + "/initial-root-entries.list";
}

std::string transaction_merged_root_path(const std::string& txn_dir) {
    return txn_dir + "/root";
}

std::string transaction_upper_root_path(const std::string& txn_dir) {
    return txn_dir + "/upper";
}

std::string transaction_work_root_path(const std::string& txn_dir) {
    return txn_dir + "/work";
}

std::string worker_live_root_path() {
    return g_root_prefix.empty() ? "/" : g_root_prefix;
}

bool transaction_should_overlay_top_level(const std::string& name) {
    return name != "proc" &&
           name != "sys" &&
           name != "dev" &&
           name != "run" &&
           name != "tmp" &&
           name != ".gpkg-txn";
}

bool transaction_bind_mount_path(
    const std::string& source,
    const std::string& target,
    bool recursive = false
) {
    if (!mkdir_p(target)) return false;
    unsigned long flags = MS_BIND;
    if (recursive) flags |= MS_REC;
    if (mount(source.c_str(), target.c_str(), nullptr, flags, nullptr) != 0) {
        std::cerr << "E: Failed to bind-mount " << source << " -> " << target << ": "
                  << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

bool transaction_mount_tmpfs_path(const std::string& target, mode_t mode) {
    if (!mkdir_p(target)) return false;
    std::ostringstream options;
    options << "mode=" << std::oct << (mode & 07777);
    if (mount("tmpfs", target.c_str(), "tmpfs", 0, options.str().c_str()) != 0) {
        std::cerr << "E: Failed to mount tmpfs at " << target << ": "
                  << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

bool transaction_mount_proc_path(const std::string& target) {
    if (!mkdir_p(target)) return false;
    if (mount("proc", target.c_str(), "proc", 0, "") != 0) {
        std::cerr << "E: Failed to mount proc at " << target << ": "
                  << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

bool transaction_mount_overlay_path(
    const std::string& lower,
    const std::string& upper,
    const std::string& work,
    const std::string& target
) {
    if (!mkdir_p(upper) || !mkdir_p(work) || !mkdir_p(target)) return false;
    std::ostringstream options;
    options << "lowerdir=" << lower << ",upperdir=" << upper << ",workdir=" << work;
    if (mount("overlay", target.c_str(), "overlay", 0, options.str().c_str()) != 0) {
        std::cerr << "E: Failed to mount overlay at " << target << ": "
                  << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

bool append_transaction_manifest_line(const std::string& path, const std::string& line) {
    std::ofstream out(path, std::ios::app);
    if (!out) return false;
    out << line << "\n";
    return out.good();
}

bool overlay_upper_entry_is_whiteout(const struct stat& st) {
    return S_ISCHR(st.st_mode) && major(st.st_rdev) == 0 && minor(st.st_rdev) == 0;
}

bool overlay_directory_is_opaque(const std::string& path) {
    char value[16];
    ssize_t size = getxattr(path.c_str(), "trusted.overlay.opaque", value, sizeof(value));
    if (size <= 0) return false;
    return value[0] == 'y' || value[0] == 'Y';
}

bool apply_transaction_live_removal(
    const std::string& logical_path,
    std::vector<InstallRollbackEntry>& rollback_entries
) {
    std::string live_full_path = g_root_prefix + logical_path;
    bool had_existing = false;
    if (!backup_live_path_if_present(live_full_path, logical_path, rollback_entries, &had_existing)) {
        return false;
    }
    if (!had_existing) return true;
    if (!remove_live_path_exact(live_full_path)) {
        std::cerr << "E: Failed to remove live path " << live_full_path << ": "
                  << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

bool commit_transaction_root_entry(
    const std::string& source_path,
    const std::string& logical_path,
    std::vector<InstallRollbackEntry>& rollback_entries
) {
    std::string live_full_path = g_root_prefix + logical_path;
    if (!mkdir_p(path_parent_dir(live_full_path))) {
        std::cerr << "E: Failed to prepare parent directory for " << live_full_path << std::endl;
        return false;
    }

    bool had_existing = false;
    if (!backup_live_path_if_present(live_full_path, logical_path, rollback_entries, &had_existing)) {
        return false;
    }

    if (!copy_path_atomic_no_follow(source_path, live_full_path)) {
        std::cerr << "E: Failed to commit " << logical_path << " from transaction root." << std::endl;
        return false;
    }

    if (!had_existing) {
        rollback_entries.push_back({logical_path, live_full_path, "", true});
    }
    return true;
}

bool commit_overlay_upper_tree_recursive(
    const std::string& upper_path,
    const std::string& logical_path,
    std::vector<InstallRollbackEntry>& rollback_entries
) {
    struct stat st {};
    if (lstat(upper_path.c_str(), &st) != 0) {
        std::cerr << "E: Failed to inspect transaction entry " << upper_path << ": "
                  << strerror(errno) << std::endl;
        return false;
    }

    if (overlay_upper_entry_is_whiteout(st)) {
        return apply_transaction_live_removal(logical_path, rollback_entries);
    }

    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        return commit_transaction_root_entry(upper_path, logical_path, rollback_entries);
    }

    std::string live_full_path = g_root_prefix + logical_path;
    bool had_existing = false;
    struct stat live_st {};
    bool live_is_directory =
        lstat(live_full_path.c_str(), &live_st) == 0 &&
        S_ISDIR(live_st.st_mode) &&
        !S_ISLNK(live_st.st_mode);
    if (!live_is_directory) {
        if (!backup_live_path_if_present(live_full_path, logical_path, rollback_entries, &had_existing)) {
            return false;
        }
        if (!mkdir_p(path_parent_dir(live_full_path))) {
            std::cerr << "E: Failed to prepare parent directory for " << live_full_path << std::endl;
            return false;
        }
        if (mkdir(live_full_path.c_str(), st.st_mode & 07777) != 0 && errno != EEXIST) {
            std::cerr << "E: Failed to create live directory " << live_full_path << ": "
                      << strerror(errno) << std::endl;
            return false;
        }
        if (!had_existing) rollback_entries.push_back({logical_path, live_full_path, "", true});
    }
    chmod(live_full_path.c_str(), st.st_mode & 07777);

    std::set<std::string> upper_names;
    DIR* dir = opendir(upper_path.c_str());
    if (!dir) {
        std::cerr << "E: Failed to open transaction directory " << upper_path << ": "
                  << strerror(errno) << std::endl;
        return false;
    }

    bool ok = true;
    int saved_errno = 0;
    struct dirent* entry = nullptr;
    errno = 0;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        upper_names.insert(name);
    }
    saved_errno = errno;
    rewinddir(dir);
    if (saved_errno != 0) ok = false;

    if (ok && overlay_directory_is_opaque(upper_path)) {
        DIR* live_dir = opendir(live_full_path.c_str());
        if (!live_dir) {
            ok = false;
            saved_errno = errno;
        } else {
            struct dirent* live_entry = nullptr;
            while ((live_entry = readdir(live_dir)) != nullptr) {
                std::string name = live_entry->d_name;
                if (name == "." || name == "..") continue;
                if (upper_names.count(name) != 0) continue;
                if (!apply_transaction_live_removal(logical_path + "/" + name, rollback_entries)) {
                    ok = false;
                    break;
                }
            }
            if (errno != 0 && ok) {
                ok = false;
                saved_errno = errno;
            }
            closedir(live_dir);
        }
    }

    errno = 0;
    while (ok && (entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (!commit_overlay_upper_tree_recursive(
                upper_path + "/" + name,
                logical_path + "/" + name,
                rollback_entries)) {
            ok = false;
            break;
        }
    }
    if (errno != 0 && ok) {
        ok = false;
        saved_errno = errno;
    }
    closedir(dir);

    if (!ok && saved_errno != 0) errno = saved_errno;
    return ok;
}

bool action_prepare_transaction_root(const std::string& txn_dir) {
    if (txn_dir.empty() || txn_dir[0] != '/') {
        std::cerr << "E: Transaction directory must be an absolute path." << std::endl;
        return false;
    }

    std::string live_root = worker_live_root_path();
    std::string merged_root = transaction_merged_root_path(txn_dir);
    std::string upper_root = transaction_upper_root_path(txn_dir);
    std::string work_root = transaction_work_root_path(txn_dir);

    if (!mkdir_p(txn_dir) ||
        !mkdir_p(merged_root) ||
        !mkdir_p(upper_root) ||
        !mkdir_p(work_root)) {
        std::cerr << "E: Failed to prepare transaction root directories." << std::endl;
        return false;
    }

    std::string mounts_manifest = transaction_mounts_manifest_path(txn_dir);
    std::string mounted_names_manifest = transaction_mounted_top_level_manifest_path(txn_dir);
    std::string initial_root_manifest = transaction_initial_root_entries_manifest_path(txn_dir);
    unlink(mounts_manifest.c_str());
    unlink(mounted_names_manifest.c_str());
    unlink(initial_root_manifest.c_str());

    DIR* root_dir = opendir(live_root.c_str());
    if (!root_dir) {
        std::cerr << "E: Failed to inspect live root " << live_root << ": "
                  << strerror(errno) << std::endl;
        return false;
    }

    bool ok = true;
    int saved_errno = 0;
    struct dirent* entry = nullptr;
    errno = 0;
    while ((entry = readdir(root_dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == ".." || name == ".gpkg-txn") continue;

        std::string source_path =
            live_root == "/" ? "/" + name : live_root + "/" + name;
        std::string target_path = merged_root + "/" + name;

        struct stat st {};
        if (lstat(source_path.c_str(), &st) != 0) {
            ok = false;
            saved_errno = errno;
            break;
        }

        if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
            if (!copy_path_atomic_no_follow(source_path, target_path) ||
                !append_transaction_manifest_line(initial_root_manifest, name)) {
                ok = false;
                break;
            }
            continue;
        }

        bool mounted = false;
        if (name == "proc") {
            mounted = transaction_mount_proc_path(target_path);
        } else if (name == "dev" || name == "sys") {
            mounted = transaction_bind_mount_path(source_path, target_path, true);
        } else if (name == "run") {
            mounted = transaction_mount_tmpfs_path(target_path, 0755);
        } else if (name == "tmp") {
            mounted = transaction_mount_tmpfs_path(target_path, 01777);
        } else if (transaction_should_overlay_top_level(name)) {
            mounted = transaction_mount_overlay_path(
                source_path,
                upper_root + "/" + name,
                work_root + "/" + name,
                target_path
            );
        } else {
            mounted = transaction_bind_mount_path(source_path, target_path, true);
        }

        if (!mounted ||
            !append_transaction_manifest_line(mounts_manifest, target_path) ||
            !append_transaction_manifest_line(mounted_names_manifest, name)) {
            ok = false;
            break;
        }
    }
    if (errno != 0 && ok) {
        ok = false;
        saved_errno = errno;
    }
    closedir(root_dir);

    if (!ok && saved_errno != 0) errno = saved_errno;
    if (!ok) {
        action_cleanup_transaction_root(txn_dir);
        if (saved_errno != 0) {
            std::cerr << "E: Failed to populate transaction root: " << strerror(saved_errno)
                      << std::endl;
        } else {
            std::cerr << "E: Failed to populate transaction root." << std::endl;
        }
        return false;
    }

    return true;
}

bool action_cleanup_transaction_root(const std::string& txn_dir) {
    if (txn_dir.empty() || txn_dir == "/" || txn_dir == ".") return false;

    std::vector<std::string> mounts = read_text_lines_local(transaction_mounts_manifest_path(txn_dir));
    for (auto it = mounts.rbegin(); it != mounts.rend(); ++it) {
        if (it->empty()) continue;
        if (umount2(it->c_str(), MNT_DETACH) != 0 && errno != EINVAL && errno != ENOENT) {
            std::cerr << "W: Failed to unmount " << *it << ": " << strerror(errno) << std::endl;
        }
    }

    if (!remove_tree_no_follow(txn_dir) && errno != ENOENT) {
        std::cerr << "W: Failed to remove transaction root " << txn_dir << ": "
                  << strerror(errno) << std::endl;
        return false;
    }
    return true;
}

bool action_commit_transaction_root(const std::string& txn_dir) {
    if (txn_dir.empty() || txn_dir[0] != '/') {
        std::cerr << "E: Transaction directory must be an absolute path." << std::endl;
        return false;
    }

    std::string merged_root = transaction_merged_root_path(txn_dir);
    std::string upper_root = transaction_upper_root_path(txn_dir);
    std::vector<std::string> mounted_names =
        read_text_lines_local(transaction_mounted_top_level_manifest_path(txn_dir));
    std::set<std::string> mounted_name_set(mounted_names.begin(), mounted_names.end());
    std::vector<std::string> initial_root_entries =
        read_text_lines_local(transaction_initial_root_entries_manifest_path(txn_dir));
    std::set<std::string> initial_root_entry_set(
        initial_root_entries.begin(),
        initial_root_entries.end()
    );

    std::vector<InstallRollbackEntry> rollback_entries;

    DIR* upper_dir = opendir(upper_root.c_str());
    if (upper_dir) {
        struct dirent* entry = nullptr;
        errno = 0;
        while ((entry = readdir(upper_dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            if (!commit_overlay_upper_tree_recursive(
                    upper_root + "/" + name,
                    "/" + name,
                    rollback_entries)) {
                closedir(upper_dir);
                rollback_install_changes(rollback_entries);
                return false;
            }
        }
        closedir(upper_dir);
    }

    DIR* merged_dir = opendir(merged_root.c_str());
    if (!merged_dir) {
        std::cerr << "E: Failed to inspect merged transaction root " << merged_root << ": "
                  << strerror(errno) << std::endl;
        rollback_install_changes(rollback_entries);
        return false;
    }

    std::set<std::string> current_root_entries;
    struct dirent* entry = nullptr;
    errno = 0;
    while ((entry = readdir(merged_dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        current_root_entries.insert(name);
        if (mounted_name_set.count(name) != 0) continue;
        if (!commit_transaction_root_entry(merged_root + "/" + name, "/" + name, rollback_entries)) {
            closedir(merged_dir);
            rollback_install_changes(rollback_entries);
            return false;
        }
    }
    closedir(merged_dir);

    for (const auto& name : initial_root_entry_set) {
        if (current_root_entries.count(name) != 0) continue;
        if (mounted_name_set.count(name) != 0) continue;
        if (!apply_transaction_live_removal("/" + name, rollback_entries)) {
            rollback_install_changes(rollback_entries);
            return false;
        }
    }

    discard_install_backups(rollback_entries);
    return true;
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

bool package_status_is_config_files_like(const std::string& status) {
    return status == "config-files";
}

bool package_status_has_installed_history(const std::string& status) {
    return status == "installed" ||
           status == "half-installed" ||
           status == "unpacked" ||
           status == "half-configured" ||
           status == "triggers-awaited" ||
           status == "triggers-pending";
}

std::vector<std::string> split_ascii_whitespace_tokens(const std::string& line) {
    std::vector<std::string> tokens;
    std::istringstream iss(line);
    std::string token;
    while (iss >> token) tokens.push_back(token);
    return tokens;
}

struct DebianTriggerInterest {
    std::string name;
    bool no_await = false;
};

std::vector<DebianTriggerInterest> load_debian_trigger_interests_for_package(
    const std::string& pkg_name
) {
    std::vector<DebianTriggerInterest> interests;
    std::ifstream in(get_debian_control_sidecar_path(pkg_name, "triggers"));
    if (!in) return interests;

    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        std::vector<std::string> tokens = split_ascii_whitespace_tokens(line);
        if (tokens.size() < 2) continue;

        const std::string& directive = tokens[0];
        if (directive != "interest" &&
            directive != "interest-await" &&
            directive != "interest-noawait") {
            continue;
        }

        bool no_await = directive == "interest-noawait";
        for (size_t i = 1; i < tokens.size(); ++i) {
            DebianTriggerInterest interest;
            interest.name = trim(tokens[i]);
            interest.no_await = no_await;
            if (!interest.name.empty()) interests.push_back(interest);
        }
    }

    return interests;
}

std::vector<std::string> load_pending_dpkg_trigger_names() {
    std::vector<std::string> loaded = read_trimmed_line_file(get_pending_dpkg_trigger_queue_path());
    std::vector<std::string> pending;
    std::set<std::string> seen;
    for (const auto& raw_name : loaded) {
        std::string name = trim(raw_name);
        if (name.empty()) continue;
        if (!seen.insert(name).second) continue;
        pending.push_back(name);
    }
    return pending;
}

bool rewrite_pending_dpkg_trigger_names(
    const std::vector<std::string>& trigger_names,
    std::string* error_out = nullptr
) {
    if (error_out) *error_out = "";

    std::string queue_path = get_pending_dpkg_trigger_queue_path();
    if (trigger_names.empty()) {
        if (unlink(queue_path.c_str()) != 0 && errno != ENOENT) {
            if (error_out) *error_out = "Failed to clear pending dpkg trigger queue.";
            return false;
        }
        return true;
    }

    std::ostringstream content;
    std::set<std::string> seen;
    for (const auto& raw_name : trigger_names) {
        std::string name = trim(raw_name);
        if (name.empty()) continue;
        if (!seen.insert(name).second) continue;
        content << name << "\n";
    }

    if (!mkdir_p(path_parent_dir(queue_path))) {
        if (error_out) *error_out = "Failed to prepare pending dpkg trigger directory.";
        return false;
    }
    if (!write_text_file_atomic(queue_path, content.str(), 0644)) {
        if (error_out) *error_out = "Failed to write pending dpkg trigger queue.";
        return false;
    }
    return true;
}

bool append_pending_dpkg_trigger_name(
    const std::string& trigger_name,
    std::string* error_out
) {
    if (error_out) *error_out = "";
    std::string normalized = trim(trigger_name);
    if (normalized.empty()) {
        if (error_out) *error_out = "Empty dpkg trigger name.";
        return false;
    }

    std::vector<std::string> pending = load_pending_dpkg_trigger_names();
    pending.push_back(normalized);
    return rewrite_pending_dpkg_trigger_names(pending, error_out);
}

std::map<std::string, std::vector<std::string>> build_installed_trigger_interest_map(
    const std::vector<std::string>& trigger_names
) {
    std::map<std::string, std::vector<std::string>> interest_map;
    std::set<std::string> wanted(trigger_names.begin(), trigger_names.end());
    if (wanted.empty()) return interest_map;

    for (const auto& record : load_package_status_records()) {
        if (record.package.empty()) continue;
        if (!package_status_has_installed_history(record.status)) continue;

        for (const auto& interest : load_debian_trigger_interests_for_package(record.package)) {
            if (wanted.count(interest.name) == 0) continue;
            interest_map[record.package].push_back(interest.name);
        }
    }

    for (auto& entry : interest_map) {
        auto& names = entry.second;
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
    }
    return interest_map;
}

void mark_packages_trigger_pending(const std::vector<std::string>& trigger_names) {
    for (const auto& entry : build_installed_trigger_interest_map(trigger_names)) {
        PackageStatusRecord record;
        if (!get_package_status_record(entry.first, &record)) continue;
        if (!package_status_has_installed_history(record.status)) continue;
        if (record.status == "half-installed" ||
            record.status == "unpacked" ||
            record.status == "half-configured") {
            continue;
        }

        if (!set_package_status_record(
                entry.first,
                record.want.empty() ? "install" : record.want,
                record.flag.empty() ? "ok" : record.flag,
                "triggers-pending",
                record.version)) {
            std::cerr << "W: Failed to record triggers-pending state for "
                      << entry.first << "." << std::endl;
        }
    }
}

std::vector<std::string> build_preinst_arguments(
    const std::string& previous_status,
    const std::string& old_version,
    const std::string& new_version
) {
    if (old_version.empty()) return {"install"};
    if (package_status_is_config_files_like(previous_status)) {
        return {"install", old_version, new_version};
    }
    return {"upgrade", old_version, new_version};
}

std::vector<std::string> build_postinst_configure_arguments(const std::string& old_version) {
    return {"configure", old_version};
}

bool finalize_failed_configuration_state(
    const std::string& pkg_name,
    std::vector<InstallRollbackEntry>& rollback_entries,
    PackageStatusRollbackGuard& status_guard,
    const std::string& message
) {
    invalidate_installed_manifest_snapshot();
    discard_install_backups(rollback_entries);
    status_guard.commit();
    std::cerr << "E: " << message << std::endl;
    std::cerr << "E: Package " << pkg_name
              << " remains installed in a half-configured state." << std::endl;
    return false;
}

bool report_configure_failure(const std::string& pkg_name, const std::string& message) {
    std::cerr << "E: " << message << std::endl;
    std::cerr << "E: Package " << pkg_name
              << " remains installed in a half-configured state." << std::endl;
    return false;
}

bool action_refresh_dpkg_trigger_state() {
    std::vector<std::string> pending = load_pending_dpkg_trigger_names();
    if (pending.empty()) {
        rewrite_pending_dpkg_trigger_names({});
        return true;
    }

    std::map<std::string, std::vector<std::string>> interest_map =
        build_installed_trigger_interest_map(pending);
    if (interest_map.empty()) {
        rewrite_pending_dpkg_trigger_names({});
        return true;
    }

    std::string queue_error;
    if (!rewrite_pending_dpkg_trigger_names({}, &queue_error)) {
        if (!queue_error.empty()) {
            std::cerr << "E: " << queue_error << std::endl;
        }
        return false;
    }

    std::set<std::string> remaining_triggers;
    std::vector<std::string> failed_packages;
    for (const auto& entry : interest_map) {
        const std::string& pkg_name = entry.first;
        const std::vector<std::string>& pkg_triggers = entry.second;

        PackageStatusRecord record;
        if (!get_package_status_record(pkg_name, &record)) continue;
        if (!package_status_has_installed_history(record.status)) continue;

        std::string version = !record.version.empty() ? record.version : get_package_version(pkg_name);
        std::string want = record.want.empty() ? "install" : record.want;
        std::string flag = record.flag.empty() ? "ok" : record.flag;
        set_package_status_record(pkg_name, want, flag, "triggers-pending", version);

        std::string postinst_path = installed_maintainer_script_path(pkg_name, "postinst");
        if (access(postinst_path.c_str(), X_OK) == 0) {
            std::vector<std::string> args = {"triggered"};
            args.insert(args.end(), pkg_triggers.begin(), pkg_triggers.end());
            int rc = run_maintainer_script_with_args(
                postinst_path,
                "postinst",
                pkg_name,
                get_info_dir() + pkg_name + ".json",
                args
            );
            if (rc != 0) {
                set_package_status_record(pkg_name, want, flag, "half-configured", version);
                remaining_triggers.insert(pkg_triggers.begin(), pkg_triggers.end());
                failed_packages.push_back(pkg_name);
                continue;
            }
        }

        if (!set_package_status_record(pkg_name, want, flag, "installed", version)) {
            std::cerr << "W: Failed to restore installed state after trigger processing for "
                      << pkg_name << "." << std::endl;
        }
    }

    for (const auto& queued_name : load_pending_dpkg_trigger_names()) {
        remaining_triggers.insert(queued_name);
    }

    std::vector<std::string> remaining(remaining_triggers.begin(), remaining_triggers.end());
    if (!rewrite_pending_dpkg_trigger_names(remaining, &queue_error) && !queue_error.empty()) {
        std::cerr << "W: " << queue_error << std::endl;
    }

    if (failed_packages.empty()) return true;

    std::cerr << "E: Maintainer trigger processing failed for";
    for (size_t i = 0; i < failed_packages.size(); ++i) {
        std::cerr << (i == 0 ? " " : ", ") << failed_packages[i];
    }
    std::cerr << "." << std::endl;
    return false;
}

bool run_postinst_abort_remove(const std::string& pkg_name, bool best_effort = true) {
    std::string postinst = get_info_dir() + pkg_name + ".postinst";
    if (access(postinst.c_str(), X_OK) != 0) return true;
    std::string metadata_path = get_info_dir() + pkg_name + ".json";

    int rc = run_maintainer_script_with_args(
        postinst,
        "postinst",
        pkg_name,
        metadata_path,
        {"abort-remove"}
    );
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
        get_replaced_system_dir(pkg_name),
        get_debian_control_sidecar_manifest_path(pkg_name)
    };

    for (const auto& sidecar_name : load_debian_control_sidecar_names(pkg_name)) {
        metadata_paths.push_back(get_debian_control_sidecar_path(pkg_name, sidecar_name));
    }

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
        if (run_maintainer_script_with_args(
                prerm,
                "prerm",
                pkg_name,
                get_info_dir() + pkg_name + ".json",
                {"remove"}
            ) != 0) {
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
        if (run_maintainer_script_with_args(
                postrm,
                "postrm",
                pkg_name,
                get_info_dir() + pkg_name + ".json",
                {"remove"}
            ) != 0) {
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
        if (run_maintainer_script_with_args(
                postrm,
                "postrm",
                pkg_name,
                get_info_dir() + pkg_name + ".json",
                {"purge"}
            ) != 0) {
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
    return get_trigger_state_dir() + "/selinux-relabel.list";
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

bool action_configure(const std::string& pkg_name, const std::string& old_version) {
    using InstallClock = std::chrono::steady_clock;
    auto total_start = InstallClock::now();
    auto phase_start = total_start;
    double postinst_seconds = 0.0;
    double cleanup_seconds = 0.0;
    auto finish_phase = [&](double& bucket) {
        auto now = InstallClock::now();
        bucket = std::chrono::duration<double>(now - phase_start).count();
        phase_start = now;
    };

    PackageStatusRecord record;
    if (!get_package_status_record(pkg_name, &record)) {
        std::cerr << "E: Package " << pkg_name << " is not registered." << std::endl;
        return false;
    }

    std::string current_version = !record.version.empty() ? record.version : get_package_version(pkg_name);
    if (current_version.empty()) {
        std::cerr << "E: Package " << pkg_name << " does not have an installed version to configure."
                  << std::endl;
        return false;
    }

    if (record.status == "installed") return true;

    std::string json_path = get_info_dir() + pkg_name + ".json";
    if (access(json_path.c_str(), R_OK) != 0) {
        std::cerr << "E: Installed metadata for " << pkg_name << " is missing." << std::endl;
        return false;
    }

    std::vector<std::string> installed_files = normalize_owned_manifest_paths(read_list_file(pkg_name));
    bool runtime_sensitive = files_touch_runtime_linker_state(installed_files);
    bool selinux_policy_touched = file_list_touches_selinux_policy_store(installed_files);
    bool kernel_payload = file_list_contains_kernel_payload(installed_files);
    std::string kernel_release = kernel_release_from_file_list(installed_files);
    std::string kernel_image_path = kernel_image_path_for_release(kernel_release);

    if (!set_package_status_record(
            pkg_name,
            record.want.empty() ? "install" : record.want,
            record.flag.empty() ? "ok" : record.flag,
            "half-configured",
            current_version)) {
        std::cerr << "E: Failed to record half-configured package state." << std::endl;
        return false;
    }

    if (!finalize_runtime_linker_state_for_success(runtime_sensitive)) {
        return report_configure_failure(
            pkg_name,
            "ldconfig failed before configuring " + pkg_name + "."
        );
    }

    std::string installed_postinst = get_info_dir() + pkg_name + ".postinst";
    if (access(installed_postinst.c_str(), X_OK) == 0) {
        std::vector<std::string> postinst_args = build_postinst_configure_arguments(old_version);
        if (run_maintainer_script_with_args(
                installed_postinst,
                "postinst",
                pkg_name,
                json_path,
                postinst_args
            ) != 0) {
            return report_configure_failure(pkg_name, "postinst failed.");
        }
    }

    if (kernel_payload) {
        if (!sync_kernel_boot_symlink()) {
            return report_configure_failure(
                pkg_name,
                "Failed to update /boot/kernel after installing " + pkg_name + "."
            );
        }
        if (!run_depmod_for_kernel_release(kernel_release, false)) {
            return report_configure_failure(
                pkg_name,
                "depmod failed after installing kernel " + kernel_release + "."
            );
        }
        std::vector<std::string> kernel_postinst_args =
            build_postinst_configure_arguments(old_version);
        if (!run_kernel_hook_directories("postinst", kernel_release, kernel_image_path, kernel_postinst_args)) {
            return report_configure_failure(
                pkg_name,
                "Kernel postinst hooks failed for " + pkg_name + "."
            );
        }
    }
    finish_phase(postinst_seconds);

    std::string selinux_error;
    if (!installed_files.empty()) {
        if (g_defer_selinux_relabel) {
            if (!finalize_selinux_relabel_for_success(installed_files, &selinux_error)) {
                return report_configure_failure(pkg_name, selinux_error);
            }
        } else if (!restorecon_transaction_paths(installed_files, &selinux_error)) {
            return report_configure_failure(pkg_name, selinux_error);
        }
    }

    if (selinux_policy_touched) {
        std::vector<InstallRollbackEntry> no_rollback;
        if (!schedule_selinux_autorelabel(no_rollback, &selinux_error)) {
            return report_configure_failure(pkg_name, selinux_error);
        }
    }

    if (!set_package_status_record(
            pkg_name,
            record.want.empty() ? "install" : record.want,
            record.flag.empty() ? "ok" : record.flag,
            "installed",
            current_version)) {
        std::cerr << "E: Failed to finalize package status after configuration." << std::endl;
        return false;
    }
    finish_phase(cleanup_seconds);
    invalidate_installed_manifest_snapshot();

    if (g_verbose) {
        double total_seconds = std::chrono::duration<double>(InstallClock::now() - total_start).count();
        VLOG("Configure timing for " << pkg_name
             << ": postinst=" << format_phase_seconds(postinst_seconds)
             << ", cleanup=" << format_phase_seconds(cleanup_seconds)
             << ", total=" << format_phase_seconds(total_seconds));
    }
    return true;
}

bool action_install(const std::string& pkg_file) {
    using InstallClock = std::chrono::steady_clock;
    auto total_start = InstallClock::now();
    auto phase_start = total_start;
    double unpack_seconds = 0.0;
    double preinst_seconds = 0.0;
    double apply_seconds = 0.0;
    double metadata_seconds = 0.0;
    double postinst_seconds = 0.0;
    double cleanup_seconds = 0.0;
    auto finish_phase = [&](double& bucket) {
        auto now = InstallClock::now();
        bucket = std::chrono::duration<double>(now - phase_start).count();
        phase_start = now;
    };

    if (g_unsafe_io && g_verbose) {
        VLOG("Unsafe I/O enabled: fsync calls will be skipped for faster package writes.");
    }

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
    finish_phase(unpack_seconds);

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

    std::vector<ReplacedSystemFile> replaced_system_files =
        collect_replaced_system_files(pkg_name, new_files, old_files_set);
    std::vector<InstallRollbackEntry> install_rollback_entries;
    bool kernel_payload = file_list_contains_kernel_payload(new_files);
    std::string kernel_release = kernel_release_from_file_list(new_files);
    std::string kernel_image_path = kernel_image_path_for_release(kernel_release);
    std::string previous_status =
        status_guard.had_record ? status_guard.record.status : "not-installed";
    bool upgrade_from_installed =
        !old_version.empty() && package_status_has_installed_history(previous_status);
    std::string existing_metadata_path = get_info_dir() + pkg_name + ".json";
    std::string extracted_metadata_path = g_tmp_extract_path + "control.json";

    auto run_installed_script = [&](const std::string& script_name,
                                    const std::vector<std::string>& args) {
        return run_optional_maintainer_script_with_args(
            installed_maintainer_script_path(pkg_name, script_name),
            script_name,
            pkg_name,
            existing_metadata_path,
            args
        );
    };

    auto report_installed_script = [&](const std::string& script_name,
                                       const std::vector<std::string>& args,
                                       const std::string& label,
                                       bool best_effort) {
        return run_optional_maintainer_script_with_reporting(
            installed_maintainer_script_path(pkg_name, script_name),
            script_name,
            pkg_name,
            existing_metadata_path,
            args,
            label,
            best_effort
        );
    };

    auto run_new_script = [&](const std::string& script_name,
                              const std::vector<std::string>& args) {
        return run_optional_maintainer_script_with_args(
            extracted_maintainer_script_path(script_name),
            script_name,
            pkg_name,
            extracted_metadata_path,
            args
        );
    };

    auto report_new_script = [&](const std::string& script_name,
                                 const std::vector<std::string>& args,
                                 const std::string& label,
                                 bool best_effort) {
        return run_optional_maintainer_script_with_reporting(
            extracted_maintainer_script_path(script_name),
            script_name,
            pkg_name,
            extracted_metadata_path,
            args,
            label,
            best_effort
        );
    };

    if (upgrade_from_installed) {
        if (run_installed_script("prerm", {"upgrade", new_version}) != 0) {
            if (!report_new_script(
                    "prerm",
                    {"failed-upgrade", old_version, new_version},
                    "new prerm failed-upgrade",
                    true)) {
                report_installed_script(
                    "postinst",
                    {"abort-upgrade", new_version},
                    "old postinst abort-upgrade",
                    true);
                std::cerr << "E: prerm upgrade failed." << std::endl;
                return false;
            }
        }
    }

    // 4. Preinst
    std::vector<std::string> preinst_args =
        build_preinst_arguments(previous_status, old_version, new_version);
    if (run_new_script("preinst", preinst_args) != 0) {
        if (upgrade_from_installed) {
            bool abort_upgrade_ok = report_new_script(
                "postrm",
                {"abort-upgrade", old_version, new_version},
                "new postrm abort-upgrade",
                true);
            if (abort_upgrade_ok) {
                report_installed_script(
                    "postinst",
                    {"abort-upgrade", new_version},
                    "old postinst abort-upgrade",
                    true);
            }
        } else if (!old_version.empty() && package_status_is_config_files_like(previous_status)) {
            report_new_script(
                "postrm",
                {"abort-install", old_version, new_version},
                "new postrm abort-install",
                true);
        } else {
            report_new_script(
                "postrm",
                {"abort-install"},
                "new postrm abort-install",
                true);
        }
        std::cerr << "E: preinst failed." << std::endl;
        return false;
    }
    if (!set_package_status_record(pkg_name, "install", "ok", "half-installed", new_version)) {
        std::cerr << "E: Failed to record package status before unpacking." << std::endl;
        return false;
    }
    finish_phase(preinst_seconds);

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
    finish_phase(apply_seconds);

    if (upgrade_from_installed) {
        if (run_installed_script("postrm", {"upgrade", new_version}) != 0) {
            if (!report_new_script(
                    "postrm",
                    {"failed-upgrade", old_version, new_version},
                    "new postrm failed-upgrade",
                    true)) {
                report_installed_script(
                    "preinst",
                    {"abort-upgrade", new_version},
                    "old preinst abort-upgrade",
                    true);
                report_new_script(
                    "postrm",
                    {"abort-upgrade", old_version, new_version},
                    "new postrm abort-upgrade",
                    true);
                report_installed_script(
                    "postinst",
                    {"abort-upgrade", new_version},
                    "old postinst abort-upgrade",
                    true);
                rollback_install_changes(install_rollback_entries);
                if (runtime_sensitive) {
                    sync_multiarch_runtime_aliases();
                    refresh_linker_cache_if_available();
                }
                std::cerr << "E: postrm upgrade failed." << std::endl;
                return false;
            }
        }
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

    std::vector<std::string> control_sidecars =
        collect_extracted_debian_control_sidecar_names(g_tmp_extract_path + "control");
    std::set<std::string> new_control_sidecar_set(
        control_sidecars.begin(),
        control_sidecars.end()
    );
    for (const auto& stale_name : load_debian_control_sidecar_names(pkg_name)) {
        if (new_control_sidecar_set.count(stale_name) != 0) continue;
        std::string stale_path = get_debian_control_sidecar_path(pkg_name, stale_name);
        if (!prepare_path_for_transaction_write(stale_path, stale_path, install_rollback_entries)) {
            rollback_install_changes(install_rollback_entries);
            if (runtime_sensitive) {
                sync_multiarch_runtime_aliases();
                refresh_linker_cache_if_available();
            }
            std::cerr << "E: Failed to prepare stale Debian control sidecar removal for "
                      << stale_name << "." << std::endl;
            return false;
        }
        if (unlink(stale_path.c_str()) != 0 && errno != ENOENT) {
            rollback_install_changes(install_rollback_entries);
            if (runtime_sensitive) {
                sync_multiarch_runtime_aliases();
                refresh_linker_cache_if_available();
            }
            std::cerr << "E: Failed to remove stale Debian control sidecar "
                      << stale_name << "." << std::endl;
            return false;
        }
    }

    for (const auto& control_name : control_sidecars) {
        std::string src = g_tmp_extract_path + "control/" + control_name;
        std::string target = get_debian_control_sidecar_path(pkg_name, control_name);
        if (!prepare_path_for_transaction_write(target, target, install_rollback_entries)) {
            rollback_install_changes(install_rollback_entries);
            if (runtime_sensitive) {
                sync_multiarch_runtime_aliases();
                refresh_linker_cache_if_available();
            }
            std::cerr << "E: Failed to prepare Debian control sidecar target "
                      << control_name << "." << std::endl;
            return false;
        }
        if (!copy_file_atomic(src, target)) {
            rollback_install_changes(install_rollback_entries);
            if (runtime_sensitive) {
                sync_multiarch_runtime_aliases();
                refresh_linker_cache_if_available();
            }
            std::cerr << "E: Failed to install Debian control sidecar "
                      << control_name << "." << std::endl;
            return false;
        }
    }

    std::string control_manifest_path = get_debian_control_sidecar_manifest_path(pkg_name);
    std::ostringstream control_manifest;
    for (const auto& control_name : control_sidecars) {
        control_manifest << control_name << "\n";
    }
    if (!prepare_path_for_transaction_write(
            control_manifest_path,
            control_manifest_path,
            install_rollback_entries) ||
        !write_text_file_atomic(control_manifest_path, control_manifest.str(), 0644)) {
        rollback_install_changes(install_rollback_entries);
        if (runtime_sensitive) {
            sync_multiarch_runtime_aliases();
            refresh_linker_cache_if_available();
        }
        std::cerr << "E: Failed to write Debian control sidecar manifest." << std::endl;
        return false;
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

        std::sort(orphans.rbegin(), orphans.rend());
        if (!orphans.empty()) {
            VLOG("Cleaning up " << orphans.size() << " orphaned files...");
            for (const auto& orphan : orphans) {
                remove_path(orphan);
            }
        }
    }

    // 7. Postinst
    if (g_defer_configure) {
        finish_phase(metadata_seconds);
        finish_phase(postinst_seconds);
        finish_phase(cleanup_seconds);
        invalidate_installed_manifest_snapshot();
        discard_install_backups(install_rollback_entries);
        status_guard.commit();

        if (g_verbose) {
            double total_seconds = std::chrono::duration<double>(InstallClock::now() - total_start).count();
            VLOG("Install timing for " << pkg_name
                 << ": unpack=" << format_phase_seconds(unpack_seconds)
                 << ", preinst=" << format_phase_seconds(preinst_seconds)
                 << ", apply=" << format_phase_seconds(apply_seconds)
                 << ", metadata=" << format_phase_seconds(metadata_seconds)
                 << ", postinst=" << format_phase_seconds(postinst_seconds)
                 << ", cleanup=" << format_phase_seconds(cleanup_seconds)
                 << ", total=" << format_phase_seconds(total_seconds)
                 << " (configure deferred)");
        }
        return true;
    }

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
    finish_phase(metadata_seconds);
    if (access(installed_postinst.c_str(), X_OK) == 0) {
         std::vector<std::string> postinst_args = build_postinst_configure_arguments(old_version);
         if (run_maintainer_script_with_args(
                 installed_postinst,
                 "postinst",
                 pkg_name,
                 json_path,
                 postinst_args
             ) != 0) {
             return finalize_failed_configuration_state(
                 pkg_name,
                 install_rollback_entries,
                 status_guard,
                 "postinst failed."
             );
         }
    }

    if (kernel_payload) {
        if (!sync_kernel_boot_symlink()) {
            return finalize_failed_configuration_state(
                pkg_name,
                install_rollback_entries,
                status_guard,
                "Failed to update /boot/kernel after installing " + pkg_name + "."
            );
        }
        if (!run_depmod_for_kernel_release(kernel_release, false)) {
            return finalize_failed_configuration_state(
                pkg_name,
                install_rollback_entries,
                status_guard,
                "depmod failed after installing kernel " + kernel_release + "."
            );
        }
        std::vector<std::string> kernel_postinst_args =
            build_postinst_configure_arguments(old_version);
        if (!run_kernel_hook_directories("postinst", kernel_release, kernel_image_path, kernel_postinst_args)) {
            return finalize_failed_configuration_state(
                pkg_name,
                install_rollback_entries,
                status_guard,
                "Kernel postinst hooks failed for " + pkg_name + "."
            );
        }
    }
    finish_phase(postinst_seconds);

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
            return finalize_failed_configuration_state(
                pkg_name,
                install_rollback_entries,
                status_guard,
                selinux_error
            );
        }
    } else if (!postinstall_selinux_delta.empty()) {
        if (!restorecon_transaction_paths(postinstall_selinux_delta, &selinux_error)) {
            return finalize_failed_configuration_state(
                pkg_name,
                install_rollback_entries,
                status_guard,
                selinux_error
            );
        }
    }

    if (selinux_policy_touched && selinux_config_requests_enabled()) {
        std::string autorelabel_path = g_root_prefix + "/.autorelabel";
        if (!prepare_path_for_transaction_write(autorelabel_path, "/.autorelabel", install_rollback_entries) ||
            !write_text_file_atomic(autorelabel_path, "", 0644)) {
            return finalize_failed_configuration_state(
                pkg_name,
                install_rollback_entries,
                status_guard,
                "Failed to schedule SELinux autorelabel after updating policy files."
            );
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
    finish_phase(cleanup_seconds);
    invalidate_installed_manifest_snapshot();
    discard_install_backups(install_rollback_entries);
    status_guard.commit();

    if (g_verbose) {
        double total_seconds = std::chrono::duration<double>(InstallClock::now() - total_start).count();
        VLOG("Install timing for " << pkg_name
             << ": unpack=" << format_phase_seconds(unpack_seconds)
             << ", preinst=" << format_phase_seconds(preinst_seconds)
             << ", apply=" << format_phase_seconds(apply_seconds)
             << ", metadata=" << format_phase_seconds(metadata_seconds)
             << ", postinst=" << format_phase_seconds(postinst_seconds)
             << ", cleanup=" << format_phase_seconds(cleanup_seconds)
             << ", total=" << format_phase_seconds(total_seconds));
    }

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

bool action_hydrate_control_metadata(const std::string& pkg_name) {
    if (pkg_name.empty()) {
        std::cerr << "E: Missing package name for control metadata hydration." << std::endl;
        return false;
    }

    CompatDpkgPackageInfo info;
    if (!resolve_compat_dpkg_package(pkg_name, info)) {
        std::cerr << "E: Package " << pkg_name
                  << " is not installed, so gpkg cannot hydrate its Debian control metadata."
                  << std::endl;
        return false;
    }

    if (!ensure_compat_package_control_metadata(info)) {
        std::cerr << "E: Unable to hydrate Debian control metadata for " << pkg_name << "." << std::endl;
        return false;
    }

    return true;
}

int main(int argc, char* argv[]) {

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--root" && i + 1 < argc) {
            g_root_prefix = argv[++i];
            continue;
        }
        if (arg == "--compat-dpkg-query") {
            std::vector<std::string> compat_args;
            for (int j = i + 1; j < argc; ++j) compat_args.push_back(argv[j]);
            return action_compat_dpkg_query(compat_args);
        }
        if (arg == "--compat-dpkg") {
            std::vector<std::string> compat_args;
            for (int j = i + 1; j < argc; ++j) compat_args.push_back(argv[j]);
            return action_compat_dpkg(compat_args);
        }
        if (arg == "--compat-dpkg-trigger") {
            std::vector<std::string> compat_args;
            for (int j = i + 1; j < argc; ++j) compat_args.push_back(argv[j]);
            return action_compat_dpkg_trigger(compat_args);
        }
        if (arg == "--compat-update-rc.d") {
            std::vector<std::string> compat_args;
            for (int j = i + 1; j < argc; ++j) compat_args.push_back(argv[j]);
            return action_compat_update_rc_d(compat_args);
        }
        if (arg == "--compat-invoke-rc.d") {
            std::vector<std::string> compat_args;
            for (int j = i + 1; j < argc; ++j) compat_args.push_back(argv[j]);
            return action_compat_invoke_rc_d(compat_args);
        }
        if (arg == "--compat-service") {
            std::vector<std::string> compat_args;
            for (int j = i + 1; j < argc; ++j) compat_args.push_back(argv[j]);
            return action_compat_service(compat_args);
        }
        if (arg == "--compat-systemctl") {
            std::vector<std::string> compat_args;
            for (int j = i + 1; j < argc; ++j) compat_args.push_back(argv[j]);
            return action_compat_systemctl(compat_args);
        }
        if (arg == "--compat-deb-systemd-invoke") {
            std::vector<std::string> compat_args;
            for (int j = i + 1; j < argc; ++j) compat_args.push_back(argv[j]);
            return action_compat_deb_systemd_invoke(compat_args);
        }
        if (arg == "--compat-initctl") {
            std::vector<std::string> compat_args;
            for (int j = i + 1; j < argc; ++j) compat_args.push_back(argv[j]);
            return action_compat_initctl(compat_args);
        }
    }

    if (argc < 2) {

        std::cout << "Usage: gpkg-worker [options]\nOptions:\n  --install <file>\n  --configure <pkg> [old-version]\n  --remove <pkg>\n  --purge <pkg>\n  --retire <pkg>\n  --verify <pkg>\n  --hydrate-control-metadata <pkg>\n  --prepare-transaction-root <dir>\n  --commit-transaction-root <dir>\n  --cleanup-transaction-root <dir>\n  --compat-dpkg [dpkg args...]\n  --compat-dpkg-query [dpkg-query args...]\n  --compat-dpkg-trigger [dpkg-trigger args...]\n  --compat-update-rc.d [update-rc.d args...]\n  --compat-invoke-rc.d [invoke-rc.d args...]\n  --compat-service [service args...]\n  --compat-systemctl [systemctl args...]\n  --compat-deb-systemd-invoke [args...]\n  --compat-initctl [args...]\n  --refresh-dpkg-trigger-state\n  --refresh-runtime-linker-state\n  --refresh-selinux-label-state\n  --register-file <path> --pkg <name>\n  --register-undo <cmd> --pkg <name>\n  --jobs <n>\n  --defer-runtime-linker-refresh\n  --defer-selinux-relabel\n  --defer-configure\n  --unsafe-io\n";

        return 1;

    }

    std::string mode, target, pkg_name;
    std::string configure_old_version;
    g_unsafe_io = env_var_is_truthy(getenv("GPKG_UNSAFE_IO"));

    for (int i = 1; i < argc; ++i) {

        std::string arg = argv[i];

        if (arg == "--install") mode = "install", target = argv[++i];

        else if (arg == "--configure") {
            mode = "configure";
            target = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') configure_old_version = argv[++i];
        }

        else if (arg == "--remove") mode = "remove", target = argv[++i];

        else if (arg == "--purge") mode = "purge", target = argv[++i];

        else if (arg == "--retire") mode = "retire", target = argv[++i];

        else if (arg == "--verify") mode = "verify", target = argv[++i];

        else if (arg == "--hydrate-control-metadata") mode = "hydrate-control-metadata", target = argv[++i];

        else if (arg == "--prepare-transaction-root") mode = "prepare-transaction-root", target = argv[++i];

        else if (arg == "--commit-transaction-root") mode = "commit-transaction-root", target = argv[++i];

        else if (arg == "--cleanup-transaction-root") mode = "cleanup-transaction-root", target = argv[++i];

        else if (arg == "--refresh-dpkg-trigger-state") mode = "refresh-dpkg-trigger-state";

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

        else if (arg == "--defer-configure") g_defer_configure = true;

        else if (arg == "--unsafe-io") g_unsafe_io = true;

        else if (arg == "-v" || arg == "--verbose") g_verbose = true;

    }

    if (mode == "remove" && !target.empty()) return action_remove_safe(target) ? 0 : 1;

    if (mode == "purge" && !target.empty()) return action_purge_safe(target) ? 0 : 1;

    if (mode == "retire" && !target.empty()) return action_retire_safe(target) ? 0 : 1;

    if (mode == "install" && !target.empty()) return action_install(target) ? 0 : 1;

    if (mode == "configure" && !target.empty()) return action_configure(target, configure_old_version) ? 0 : 1;

    if (mode == "verify" && !target.empty()) return action_verify(target) ? 0 : 1;

    if (mode == "hydrate-control-metadata" && !target.empty()) {
        return action_hydrate_control_metadata(target) ? 0 : 1;
    }

    if (mode == "prepare-transaction-root" && !target.empty()) {
        return action_prepare_transaction_root(target) ? 0 : 1;
    }

    if (mode == "commit-transaction-root" && !target.empty()) {
        return action_commit_transaction_root(target) ? 0 : 1;
    }

    if (mode == "cleanup-transaction-root" && !target.empty()) {
        return action_cleanup_transaction_root(target) ? 0 : 1;
    }

    if (mode == "refresh-dpkg-trigger-state") return action_refresh_dpkg_trigger_state() ? 0 : 1;

    if (mode == "refresh-runtime-linker-state") return action_refresh_runtime_linker_state() ? 0 : 1;

    if (mode == "refresh-selinux-label-state") return action_refresh_selinux_label_state() ? 0 : 1;

    if (mode == "register-file" && !target.empty() && !pkg_name.empty()) return action_register_file(pkg_name, target) ? 0 : 1;

    if (mode == "register-undo" && !target.empty() && !pkg_name.empty()) return action_register_undo(pkg_name, target) ? 0 : 1;

    std::cerr << "Invalid arguments or missing target/pkg.\n";

    return 1;

}
