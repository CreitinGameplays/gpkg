// Shared state, process lifecycle, and common types for gpkg.

#include "network.h"
#include "debug.h"
#include "signals.h"
#include "sys_info.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <openssl/sha.h>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace Color {
const std::string RESET   = "\033[0m";
const std::string RED     = "\033[31m";
const std::string GREEN   = "\033[32m";
const std::string YELLOW  = "\033[33m";
const std::string BLUE    = "\033[34m";
const std::string MAGENTA = "\033[35m";
const std::string CYAN    = "\033[36m";
const std::string BOLD    = "\033[1m";
}

#ifdef DEV_MODE
const std::string ROOT_PREFIX = "rootfs";
#else
const std::string ROOT_PREFIX = "";
#endif

const std::string REPO_CACHE_PATH = ROOT_PREFIX + "/var/repo/";
const std::string SOURCES_LIST_PATH = ROOT_PREFIX + "/etc/gpkg/sources.list";
const std::string SOURCES_DIR = ROOT_PREFIX + "/etc/gpkg/sources.list.d/";
// Legacy fallback files. New images keep both lists inside import-policy.json.
const std::string SYSTEM_PROVIDES_PATH = ROOT_PREFIX + "/etc/gpkg/system-provides.list";
const std::string UPGRADEABLE_SYSTEM_PATH = ROOT_PREFIX + "/etc/gpkg/upgradeable-system.list";
const std::string UPGRADE_COMPANIONS_PATH = ROOT_PREFIX + "/etc/gpkg/upgrade-companions.conf";
const std::string DEBIAN_CONFIG_PATH = ROOT_PREFIX + "/etc/gpkg/debian.conf";
const std::string IMPORT_POLICY_PATH = ROOT_PREFIX + "/etc/gpkg/import-policy.json";
const std::string DPKG_STATUS_FILE = ROOT_PREFIX + "/var/lib/dpkg/status";
const std::string BASE_SYSTEM_PROVIDER = "<base system policy>";
const std::string STATUS_FILE = ROOT_PREFIX + "/var/lib/gpkg/status";
const std::string EXTENDED_STATES_FILE = ROOT_PREFIX + "/var/lib/gpkg/extended_states";
const std::string INFO_DIR = ROOT_PREFIX + "/var/lib/gpkg/info/";
const std::string EXTENSION = ".gpkg";
const std::string LOCK_FILE = ROOT_PREFIX + "/var/lib/gpkg/lock";
constexpr size_t MAX_PARALLEL_PACKAGE_DOWNLOADS = 5;

int run_command(const std::string& cmd, bool verbose);
int run_command_argv(
    const std::vector<std::string>& argv,
    bool verbose,
    int stdout_fd = -1,
    int stderr_fd = -1
);
int decode_command_exit_status(int status);
int compare_versions(const std::string& v1, const std::string& v2);
std::string shell_quote(const std::string& value);

struct CommandCaptureResult {
    int exit_code = 0;
    std::string log_path;
};

struct PackageStatusRecord {
    std::string package;
    std::string want = "install";
    std::string flag = "ok";
    std::string status = "not-installed";
    std::string version;
};

struct PackageAutoStateRecord {
    std::string package;
    bool auto_installed = false;
};

CommandCaptureResult run_command_captured(const std::string& cmd, bool verbose, const std::string& log_prefix);
CommandCaptureResult run_command_captured_argv(
    const std::vector<std::string>& argv,
    bool verbose,
    const std::string& log_prefix
);
std::vector<PackageStatusRecord> load_package_status_records();
std::vector<PackageStatusRecord> load_dpkg_package_status_records();
bool get_package_status_record(const std::string& pkg_name, PackageStatusRecord* out = nullptr);
bool get_dpkg_package_status_record(const std::string& pkg_name, PackageStatusRecord* out = nullptr);
bool package_status_is_installed_like(const std::string& state);
std::vector<PackageAutoStateRecord> load_package_auto_state_records();
bool get_package_auto_installed_state(const std::string& pkg_name, bool* out_auto = nullptr);
bool set_package_auto_installed_state(const std::string& pkg_name, bool auto_installed);
bool erase_package_auto_installed_state(const std::string& pkg_name);

enum class OptionalDependencyMode {
    Auto,
    ForceYes,
    ForceNo,
};

struct OptionalDependencyPolicy {
    OptionalDependencyMode recommends = OptionalDependencyMode::Auto;
    OptionalDependencyMode suggests = OptionalDependencyMode::Auto;
};

bool mkdir_p(const std::string& path) {
    if (path.empty()) return false;

    std::string current_path;
    std::stringstream ss(path);
    std::string segment;
    if (path[0] == '/') current_path = "/";

    while (std::getline(ss, segment, '/')) {
        if (segment.empty()) continue;
        current_path += segment + "/";

        struct stat st;
        if (stat(current_path.c_str(), &st) != 0) {
            if (mkdir(current_path.c_str(), 0755) != 0 && errno != EEXIST) {
                return false;
            }
        }
    }

    return true;
}

bool is_system_provided(const std::string& pkg, const std::string& op = "", const std::string& req_version = "");
bool is_upgradeable_system_package(const std::string& pkg);
bool package_is_base_system_provided(const std::string& pkg_name, std::string* reason_out = nullptr);

std::string first_command_token(const std::string& cmd) {
    size_t start = cmd.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";

    size_t end = cmd.find_first_of(" \t\n\r", start);
    if (end == std::string::npos) return cmd.substr(start);
    return cmd.substr(start, end - start);
}

bool is_executable_command_available(const std::string& cmd) {
    std::string token = first_command_token(cmd);
    if (token.empty()) return false;

    if (token.find('/') != std::string::npos) {
        return access(token.c_str(), X_OK) == 0;
    }

    const char* path_env = getenv("PATH");
    std::string path = path_env ? path_env : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    std::stringstream ss(path);
    std::string segment;
    while (std::getline(ss, segment, ':')) {
        if (segment.empty()) continue;
        std::string candidate = segment + "/" + token;
        if (access(candidate.c_str(), X_OK) == 0) return true;
    }

    return false;
}

size_t visible_text_width(const std::string& value) {
    size_t width = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        unsigned char ch = static_cast<unsigned char>(value[i]);
        if (ch == '\033' && i + 1 < value.size() && value[i + 1] == '[') {
            i += 2;
            while (i < value.size()) {
                unsigned char seq = static_cast<unsigned char>(value[i]);
                if ((seq >= '@' && seq <= '~') || std::isalpha(seq)) break;
                ++i;
            }
            continue;
        }
        ++width;
    }
    return width;
}

size_t get_terminal_width(size_t fallback = 80) {
    const char* columns_env = getenv("COLUMNS");
    if (columns_env) {
        char* end = nullptr;
        long parsed = std::strtol(columns_env, &end, 10);
        if (end != columns_env && parsed > 0) return static_cast<size_t>(parsed);
    }

    struct winsize ws {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return static_cast<size_t>(ws.ws_col);
    }

    return fallback;
}

std::string truncate_progress_label(const std::string& value, size_t max_len) {
    if (value.size() <= max_len) return value;
    if (max_len <= 3) return value.substr(0, max_len);
    return value.substr(0, max_len - 3) + "...";
}

size_t detected_cpu_worker_count() {
    unsigned int count = std::thread::hardware_concurrency();
    if (count == 0) return 1;
    return static_cast<size_t>(count);
}

size_t recommended_parallel_worker_count(size_t task_count) {
    if (task_count == 0) return 1;
    return std::max<size_t>(1, std::min(task_count, detected_cpu_worker_count()));
}

std::set<std::string> g_pending_triggers;
bool g_assume_yes = false;
bool g_force_reinstall = false;
OptionalDependencyPolicy g_optional_dependency_policy;

bool is_optional_dependency_option(const std::string& arg) {
    return arg == "--recommended-yes" ||
           arg == "--recommended-no" ||
           arg == "--suggested-yes" ||
           arg == "--suggested-no";
}

bool is_known_cli_option(const std::string& arg) {
    return arg == "-h" ||
           arg == "--help" ||
           arg == "-v" ||
           arg == "--verbose" ||
           arg == "-y" ||
           arg == "--yes" ||
           arg == "-r" ||
           arg == "--repair" ||
           arg == "--reinstall" ||
           arg == "-V" ||
           arg == "--version" ||
           arg == "--purge" ||
           arg == "--autoremove" ||
           is_optional_dependency_option(arg);
}

std::vector<std::string> collect_cli_operands(int argc, char* argv[], int start_index = 2) {
    std::vector<std::string> operands;
    for (int i = start_index; i < argc; ++i) {
        std::string arg = argv[i];
        if (is_known_cli_option(arg)) continue;
        operands.push_back(arg);
    }
    return operands;
}

std::string first_cli_operand(int argc, char* argv[], int start_index = 2) {
    auto operands = collect_cli_operands(argc, argv, start_index);
    return operands.empty() ? "" : operands.front();
}

std::vector<std::string> read_installed_file_list(const std::string& pkg_name) {
    std::vector<std::string> files;
    std::ifstream in(INFO_DIR + pkg_name + ".list");
    if (!in) return files;

    std::string line;
    while (std::getline(in, line)) {
        size_t first = line.find_first_not_of(" \t\n\r");
        if (first == std::string::npos) continue;
        size_t last = line.find_last_not_of(" \t\n\r");
        files.push_back(line.substr(first, last - first + 1));
    }
    return files;
}

bool installed_file_is_kernel_payload_path(const std::string& path) {
    return path.rfind("/boot/kernel-", 0) == 0 ||
           path.rfind("/lib/modules/", 0) == 0;
}

bool installed_file_list_contains_kernel_payload(const std::vector<std::string>& files) {
    for (const auto& path : files) {
        if (installed_file_is_kernel_payload_path(path)) return true;
    }
    return false;
}

std::string extract_kernel_release_from_installed_file_list(const std::vector<std::string>& files) {
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

std::string read_running_kernel_release() {
    std::vector<std::string> candidates;
    if (!ROOT_PREFIX.empty()) candidates.push_back(ROOT_PREFIX + "/proc/sys/kernel/osrelease");
    candidates.push_back("/proc/sys/kernel/osrelease");

    for (const auto& path : candidates) {
        std::ifstream in(path);
        if (!in) continue;
        std::string release;
        std::getline(in, release);
        size_t first = release.find_first_not_of(" \t\n\r");
        if (first == std::string::npos) continue;
        size_t last = release.find_last_not_of(" \t\n\r");
        release = release.substr(first, last - first + 1);
        if (!release.empty()) return release;
    }

    return "";
}

void release_lock(bool verbose) {
    if (verbose) std::cout << "[DEBUG] Releasing lock: " << LOCK_FILE << std::endl;
    unlink(LOCK_FILE.c_str());
}

bool acquire_lock(bool verbose) {
    std::string lock_dir = LOCK_FILE.substr(0, LOCK_FILE.find_last_of('/'));
    struct stat st;
    if (stat(lock_dir.c_str(), &st) != 0) {
        if (verbose) std::cout << "[DEBUG] Creating lock directory: " << lock_dir << std::endl;
        if (!mkdir_p(lock_dir)) {
            std::cerr << Color::RED << "E: Failed to create lock directory: "
                      << lock_dir << " (errno: " << errno << ")" << Color::RESET << std::endl;
            return false;
        }
    }

    if (access(LOCK_FILE.c_str(), F_OK) == 0) {
        std::cerr << Color::RED << "E: Could not acquire lock (" << LOCK_FILE
                  << "). Is another process using it?" << Color::RESET << std::endl;
        return false;
    }

    if (verbose) std::cout << "[DEBUG] Acquiring lock: " << LOCK_FILE << std::endl;
    int fd = open(LOCK_FILE.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        std::cerr << Color::RED << "E: Failed to create lock file: " << LOCK_FILE
                  << " (errno: " << errno << ")" << Color::RESET << std::endl;
        return false;
    }

    close(fd);
    return true;
}

void check_triggers(const std::vector<std::string>& files) {
    for (const auto& file : files) {
        if (file.find("usr/share/glib-2.0/schemas") != std::string::npos) {
            g_pending_triggers.insert("glib-compile-schemas /usr/share/glib-2.0/schemas");
        }
        if (file.find("usr/share/icons") != std::string::npos) {
            g_pending_triggers.insert("gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor");
        }
        if (file.find("usr/share/mime") != std::string::npos) {
            g_pending_triggers.insert("update-mime-database /usr/share/mime");
        }
        if (file.find("usr/share/applications") != std::string::npos) {
            g_pending_triggers.insert("update-desktop-database /usr/share/applications");
        }
        if (file.find("lib/") != std::string::npos || file.find("lib64/") != std::string::npos) {
            g_pending_triggers.insert("ldconfig");
        }
    }
}

void queue_triggers_for_package(const std::string& pkg_name) {
    check_triggers(read_installed_file_list(pkg_name));
}

int run_ldconfig_trigger(bool verbose) {
    std::vector<std::string> argv = {"gpkg-worker", "--refresh-runtime-linker-state"};
    if (verbose) argv.push_back("--verbose");
    if (!ROOT_PREFIX.empty()) {
        argv.push_back("--root");
        argv.push_back(ROOT_PREFIX);
    }

    return decode_command_exit_status(run_command_argv(argv, verbose));
}

void run_triggers(bool verbose) {
    if (g_pending_triggers.empty()) return;

    std::cout << Color::CYAN << "Processing triggers..." << Color::RESET << std::endl;
    if (verbose) std::cout << "[DEBUG] " << g_pending_triggers.size() << " triggers pending." << std::endl;

    std::vector<std::string> failed_triggers;
    for (const auto& cmd : g_pending_triggers) {
        if (cmd == "ldconfig") {
            if (!is_executable_command_available("gpkg-worker")) {
                if (verbose) {
                    std::cout << "[DEBUG] Skipping ldconfig trigger because gpkg-worker is unavailable."
                              << std::endl;
                }
                continue;
            }
            if (verbose) std::cout << "[DEBUG] Running trigger via gpkg-worker: " << cmd << std::endl;
            if (run_ldconfig_trigger(verbose) != 0) {
                failed_triggers.push_back(cmd);
            }
            continue;
        }

        if (!is_executable_command_available(cmd)) {
            if (verbose) {
                std::cout << "[DEBUG] Skipping trigger because its command is unavailable: "
                          << cmd << std::endl;
            }
            continue;
        }
        if (verbose) std::cout << "[DEBUG] Running trigger: " << cmd << std::endl;
        int rc = decode_command_exit_status(run_command(cmd, verbose));
        if (rc == 127) {
            if (verbose) {
                std::cout << "[DEBUG] Skipping trigger because its command resolved to shell exit 127: "
                          << cmd << std::endl;
            }
            continue;
        }
        if (rc != 0) {
            failed_triggers.push_back(cmd);
        }
    }
    g_pending_triggers.clear();

    if (!failed_triggers.empty()) {
        std::ostringstream joined;
        for (size_t i = 0; i < failed_triggers.size(); ++i) {
            if (i > 0) joined << ", ";
            joined << failed_triggers[i];
        }
        std::cerr << Color::RED
                  << "E: Trigger processing failed for: " << joined.str()
                  << Color::RESET << std::endl;
    }
}

struct ScopedLock {
    bool locked = false;
    bool verbose = false;

    ScopedLock(bool active, bool v) : verbose(v) {
        if (active) {
            if (acquire_lock(verbose)) {
                locked = true;
            } else {
                exit(1);
            }
        }
    }

    ~ScopedLock() {
        if (locked) release_lock(verbose);
    }
};

struct TransactionGuard {
    ScopedLock lock;
    bool active;
    bool verbose;

    TransactionGuard(bool need_lock, bool v) : lock(need_lock, v), active(need_lock), verbose(v) {}

    ~TransactionGuard() {
        if (active) run_triggers(verbose);
    }
};

void sig_handler(int) {
    g_stop_sig = 1;
    unlink(LOCK_FILE.c_str());
    std::cerr << "\n[!] Interrupted. Lock released." << std::endl;
    exit(130);
}

std::string get_command_output(const std::string& cmd) {
    char buffer[128];
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";

    while (!feof(pipe)) {
        if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            result += buffer;
        }
    }

    pclose(pipe);
    return result;
}

struct PackageMetadata {
    std::string name;
    std::string version;
    std::string arch;
    std::string description;
    std::string maintainer;
    std::string section;
    std::string priority;
    std::string filename;
    std::string sha256;
    std::string sha512;
    std::string source_url;
    std::string source_kind;
    std::string debian_package;
    std::string debian_version;
    std::string package_scope;
    std::string installed_from;
    std::string size;
    std::string installed_size_bytes;
    std::vector<std::string> depends;
    std::vector<std::string> recommends;
    std::vector<std::string> suggests;
    std::vector<std::string> conflicts;
    std::vector<std::string> provides;
    std::vector<std::string> replaces;
};

bool package_scope_contains(const std::string& scope, const std::string& token) {
    if (scope.empty() || token.empty()) return false;

    std::string current;
    std::vector<std::string> parts;
    for (char ch : scope) {
        if (ch == '+') {
            if (!current.empty()) parts.push_back(current);
            current.clear();
            continue;
        }
        current += ch;
    }
    if (!current.empty()) parts.push_back(current);
    return std::find(parts.begin(), parts.end(), token) != parts.end();
}

std::string describe_optional_dependency_mode(OptionalDependencyMode mode) {
    switch (mode) {
        case OptionalDependencyMode::ForceYes:
            return "yes";
        case OptionalDependencyMode::ForceNo:
            return "no";
        case OptionalDependencyMode::Auto:
        default:
            return "auto";
    }
}

bool should_include_optional_group(
    OptionalDependencyMode mode,
    const PackageMetadata& meta,
    const std::string& token
) {
    if (mode == OptionalDependencyMode::ForceYes) return true;
    if (mode == OptionalDependencyMode::ForceNo) return false;
    return package_scope_contains(meta.package_scope, token);
}

bool should_include_recommends_for_transaction(const PackageMetadata& meta) {
    return should_include_optional_group(g_optional_dependency_policy.recommends, meta, "recommends");
}

bool should_include_suggests_for_transaction(const PackageMetadata& meta) {
    return should_include_optional_group(g_optional_dependency_policy.suggests, meta, "suggests");
}

std::vector<std::string> collect_transaction_dependency_edges(const PackageMetadata& meta) {
    std::vector<std::string> edges = meta.depends;
    if (should_include_recommends_for_transaction(meta)) {
        edges.insert(edges.end(), meta.recommends.begin(), meta.recommends.end());
    }
    if (should_include_suggests_for_transaction(meta)) {
        edges.insert(edges.end(), meta.suggests.begin(), meta.suggests.end());
    }

    std::vector<std::string> unique;
    std::set<std::string> seen;
    for (const auto& edge : edges) {
        if (seen.insert(edge).second) unique.push_back(edge);
    }
    return unique;
}

std::vector<std::string> collect_integrity_dependency_edges(const PackageMetadata& meta) {
    std::vector<std::string> unique;
    std::set<std::string> seen;
    for (const auto& edge : meta.depends) {
        if (seen.insert(edge).second) unique.push_back(edge);
    }
    return unique;
}

std::string describe_optional_dependency_policy() {
    std::ostringstream out;
    out << "recommends=" << describe_optional_dependency_mode(g_optional_dependency_policy.recommends)
        << ", suggests=" << describe_optional_dependency_mode(g_optional_dependency_policy.suggests);
    return out.str();
}

std::string cache_safe_component(const std::string& value) {
    std::string safe;
    safe.reserve(value.size());
    for (char c : value) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-') {
            safe += c;
        } else {
            safe += '_';
        }
    }
    if (safe.empty()) return "unknown";
    return safe;
}

std::string path_dirname(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

std::string path_basename(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    return path.substr(pos + 1);
}

bool mkdir_parent(const std::string& path) {
    return mkdir_p(path_dirname(path));
}

std::string join_url_path(const std::string& base, const std::string& relative) {
    if (base.empty()) return relative;
    if (relative.empty()) return base;

    std::string normalized_base = base;
    while (normalized_base.size() > 1 && normalized_base.back() == '/') {
        normalized_base.pop_back();
    }

    if (relative[0] == '/') return normalized_base + relative;
    return normalized_base + "/" + relative;
}

bool package_is_debian_source(const PackageMetadata& meta) {
    return meta.source_kind == "debian";
}

#define VLOG(v, msg) do { if (v) std::cout << "[DEBUG] " << msg << std::endl; } while(0)
