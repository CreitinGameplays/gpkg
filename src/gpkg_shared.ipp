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
#include <sched.h>
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
const std::string UPGRADE_CATALOG_PATH = ROOT_PREFIX + "/var/lib/gpkg/upgrade-catalog.json";
const std::string DEBIAN_CONFIG_PATH = ROOT_PREFIX + "/etc/gpkg/debian.conf";
const std::string IMPORT_POLICY_PATH = ROOT_PREFIX + "/etc/gpkg/import-policy.json";
const std::string DPKG_STATUS_FILE = ROOT_PREFIX + "/var/lib/dpkg/status";
const std::string BASE_SYSTEM_REGISTRY_PATH = ROOT_PREFIX + "/usr/share/gpkg/base-system.json";
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

struct BaseSystemRegistryEntry {
    std::string package;
    std::string version;
    std::vector<std::string> files;
};

struct PackageAutoStateRecord {
    std::string package;
    bool auto_installed = false;
};

enum class TransactionDependencyKind {
    Required,
    Recommended,
    Suggested
};

struct TransactionDependencyEdge {
    std::string relation;
    TransactionDependencyKind kind = TransactionDependencyKind::Required;
};

CommandCaptureResult run_command_captured(const std::string& cmd, bool verbose, const std::string& log_prefix);
CommandCaptureResult run_command_captured_argv(
    const std::vector<std::string>& argv,
    bool verbose,
    const std::string& log_prefix
);
std::vector<PackageStatusRecord> load_package_status_records();
std::vector<PackageStatusRecord> load_dpkg_package_status_records();
std::vector<PackageStatusRecord> load_base_system_package_status_records();
std::vector<BaseSystemRegistryEntry> load_base_system_registry_entries();
bool base_system_registry_entry_looks_present(const BaseSystemRegistryEntry& entry);
bool get_package_status_record(const std::string& pkg_name, PackageStatusRecord* out = nullptr);
bool get_dpkg_package_status_record(const std::string& pkg_name, PackageStatusRecord* out = nullptr);
bool get_base_system_package_status_record(const std::string& pkg_name, PackageStatusRecord* out = nullptr);
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

bool remove_path_recursive(const std::string& path) {
    if (path.empty() || path == "/" || path == "." || path == "..") return false;

    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT;
    }

    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR* dir = opendir(path.c_str());
        if (!dir) return false;

        bool ok = true;
        while (true) {
            errno = 0;
            dirent* entry = readdir(dir);
            if (!entry) break;

            std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            if (!remove_path_recursive(path + "/" + name)) ok = false;
        }

        int read_errno = errno;
        closedir(dir);
        if (read_errno != 0) return false;
        if (!ok) return false;
        return rmdir(path.c_str()) == 0 || errno == ENOENT;
    }

    return unlink(path.c_str()) == 0 || errno == ENOENT;
}

size_t prune_directory_entries_with_prefixes(
    const std::string& dir_path,
    const std::vector<std::string>& prefixes
) {
    if (dir_path.empty() || prefixes.empty()) return 0;

    DIR* dir = opendir(dir_path.c_str());
    if (!dir) return 0;

    size_t removed_count = 0;
    while (true) {
        errno = 0;
        dirent* entry = readdir(dir);
        if (!entry) break;

        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        bool matches = false;
        for (const auto& prefix : prefixes) {
            if (!prefix.empty() && name.rfind(prefix, 0) == 0) {
                matches = true;
                break;
            }
        }
        if (!matches) continue;

        if (remove_path_recursive(dir_path + "/" + name)) {
            ++removed_count;
        }
    }

    closedir(dir);
    return removed_count;
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

std::string resolve_gpkg_worker_command() {
    const std::vector<std::string> candidates = {
        ROOT_PREFIX + "/bin/apps/system/gpkg-worker",
        ROOT_PREFIX + "/bin/gpkg-worker",
        "/bin/apps/system/gpkg-worker",
        "/bin/gpkg-worker",
        "/usr/bin/gpkg-worker",
        "/usr/local/bin/gpkg-worker",
    };

    for (const auto& candidate : candidates) {
        if (candidate.empty()) continue;
        if (access(candidate.c_str(), X_OK) == 0) return candidate;
    }

    if (is_executable_command_available("gpkg-worker")) return "gpkg-worker";
    return "";
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

std::string ascii_lower_copy(const std::string& value) {
    std::string lowered = value;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return lowered;
}

bool stdout_is_interactive_terminal() {
    if (!isatty(STDOUT_FILENO) || !isatty(STDIN_FILENO)) return false;
    const char* term_env = getenv("TERM");
    if (!term_env || term_env[0] == '\0') return false;
    return std::string(term_env) != "dumb";
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

std::string default_interactive_pager_command() {
    if (is_executable_command_available("less")) {
        return "LESS=FRXMK less -R";
    }
    if (is_executable_command_available("pager")) return "pager";
    if (is_executable_command_available("more")) return "more";
    return "";
}

std::string resolve_interactive_pager_command() {
    const char* gpkg_pager_env = getenv("GPKG_PAGER");
    if (gpkg_pager_env) {
        std::string pager = gpkg_pager_env;
        if (pager == "0" || ascii_lower_copy(pager) == "none" || ascii_lower_copy(pager) == "cat") {
            return "";
        }
        if (pager == "1") return default_interactive_pager_command();
        if (!pager.empty()) return pager;
    }

    const char* pager_env = getenv("PAGER");
    if (pager_env && pager_env[0] != '\0') {
        std::string pager = pager_env;
        if (ascii_lower_copy(pager) == "cat" || ascii_lower_copy(pager) == "none") return "";
        return pager;
    }

    return default_interactive_pager_command();
}

bool write_text_via_pager(const std::string& text, bool verbose) {
    if (!stdout_is_interactive_terminal()) return false;

    std::string pager_command = resolve_interactive_pager_command();
    if (pager_command.empty()) return false;

    char tmpl[] = "/tmp/gpkg-pager-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return false;

    bool wrote_all = true;
    ssize_t offset = 0;
    while (offset < static_cast<ssize_t>(text.size())) {
        ssize_t written = write(fd, text.data() + offset, static_cast<size_t>(text.size() - offset));
        if (written < 0) {
            if (errno == EINTR) continue;
            wrote_all = false;
            break;
        }
        offset += written;
    }
    close(fd);

    if (!wrote_all) {
        unlink(tmpl);
        return false;
    }

    int rc = decode_command_exit_status(run_command(pager_command + " " + shell_quote(tmpl), verbose));
    unlink(tmpl);
    return rc == 0;
}

std::string truncate_progress_label(const std::string& value, size_t max_len) {
    if (value.size() <= max_len) return value;
    if (max_len <= 3) return value.substr(0, max_len);
    return value.substr(0, max_len - 3) + "...";
}

size_t detected_cpu_worker_count() {
    const char* env = getenv("GPKG_WORKERS");
    if (env && env[0] != '\0') {
        char* end = nullptr;
        errno = 0;
        unsigned long requested = std::strtoul(env, &end, 10);
        while (end && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
        if (errno == 0 &&
            end != env &&
            (!end || *end == '\0') &&
            requested > 0 &&
            requested <= std::numeric_limits<size_t>::max()) {
            return static_cast<size_t>(requested);
        }
    }

#ifdef __linux__
    cpu_set_t affinity_set;
    CPU_ZERO(&affinity_set);
    if (sched_getaffinity(0, sizeof(affinity_set), &affinity_set) == 0) {
        size_t affinity_count = static_cast<size_t>(CPU_COUNT(&affinity_set));
        if (affinity_count > 0) return affinity_count;
    }
#endif

    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online > 0) return static_cast<size_t>(online);

    unsigned int count = std::thread::hardware_concurrency();
    if (count == 0) return 1;
    return static_cast<size_t>(count);
}

size_t recommended_parallel_worker_count(size_t task_count) {
    if (task_count == 0) return 1;
    return std::max<size_t>(1, std::min(task_count, detected_cpu_worker_count()));
}

std::set<std::string> g_pending_triggers;
bool g_pending_runtime_linker_refresh = false;
bool g_pending_selinux_relabel = false;
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

int find_cli_action_index(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (is_known_cli_option(arg)) continue;
        if (arg.empty() || arg[0] == '-') continue;
        return i;
    }
    return -1;
}

std::vector<std::string> collect_cli_operands(int argc, char* argv[], int start_index = 2) {
    std::vector<std::string> operands;
    int action_index = find_cli_action_index(argc, argv);
    int effective_start = start_index;
    if (action_index >= 0) effective_start = std::max(start_index, action_index + 1);

    for (int i = effective_start; i < argc; ++i) {
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
            g_pending_runtime_linker_refresh = true;
        }
    }
}

void queue_triggers_for_package(const std::string& pkg_name) {
    check_triggers(read_installed_file_list(pkg_name));
}

void queue_runtime_linker_state_refresh() {
    g_pending_runtime_linker_refresh = true;
}

void queue_selinux_label_state_refresh() {
    g_pending_selinux_relabel = true;
}

int run_ldconfig_trigger(bool verbose, const std::string& worker_command = "") {
    std::string resolved_worker = worker_command.empty()
        ? resolve_gpkg_worker_command()
        : worker_command;
    if (resolved_worker.empty()) return 127;

    std::vector<std::string> argv = {resolved_worker, "--refresh-runtime-linker-state"};
    if (verbose) argv.push_back("--verbose");
    if (!ROOT_PREFIX.empty()) {
        argv.push_back("--root");
        argv.push_back(ROOT_PREFIX);
    }

    return decode_command_exit_status(run_command_argv(argv, verbose));
}

int run_selinux_relabel_trigger(bool verbose, const std::string& worker_command = "") {
    std::string resolved_worker = worker_command.empty()
        ? resolve_gpkg_worker_command()
        : worker_command;
    if (resolved_worker.empty()) return 127;

    std::vector<std::string> argv = {resolved_worker, "--refresh-selinux-label-state"};
    if (verbose) argv.push_back("--verbose");
    if (!ROOT_PREFIX.empty()) {
        argv.push_back("--root");
        argv.push_back(ROOT_PREFIX);
    }

    return decode_command_exit_status(run_command_argv(argv, verbose));
}

void run_triggers(bool verbose) {
    bool pending_runtime_refresh = g_pending_runtime_linker_refresh;
    bool pending_selinux_relabel = g_pending_selinux_relabel;
    std::set<std::string> pending_commands = g_pending_triggers;

    if (pending_commands.empty() &&
        !pending_runtime_refresh &&
        !pending_selinux_relabel) {
        return;
    }

    g_pending_triggers.clear();
    g_pending_runtime_linker_refresh = false;
    g_pending_selinux_relabel = false;

    std::cout << Color::CYAN << "Processing triggers..." << Color::RESET << std::endl;
    if (verbose) {
        size_t pending_count = pending_commands.size();
        if (pending_runtime_refresh) ++pending_count;
        if (pending_selinux_relabel) ++pending_count;
        std::cout << "[DEBUG] " << pending_count << " triggers pending." << std::endl;
    }

    std::vector<std::string> failed_triggers;
    std::string worker_command = resolve_gpkg_worker_command();

    if (pending_runtime_refresh) {
        if (worker_command.empty()) {
            if (verbose) {
                std::cout << "[DEBUG] Skipping runtime linker refresh because gpkg-worker is unavailable."
                          << std::endl;
            }
        } else {
            if (verbose) {
                std::cout << "[DEBUG] Running trigger via gpkg-worker: --refresh-runtime-linker-state"
                          << std::endl;
            }
            if (run_ldconfig_trigger(verbose, worker_command) != 0) {
                failed_triggers.push_back("gpkg-worker --refresh-runtime-linker-state");
            }
        }
    }

    for (const auto& cmd : pending_commands) {
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

    if (pending_selinux_relabel) {
        if (worker_command.empty()) {
            if (verbose) {
                std::cout << "[DEBUG] Skipping SELinux relabel trigger because gpkg-worker is unavailable."
                          << std::endl;
            }
        } else {
            if (verbose) {
                std::cout << "[DEBUG] Running trigger via gpkg-worker: --refresh-selinux-label-state"
                          << std::endl;
            }
            if (run_selinux_relabel_trigger(verbose, worker_command) != 0) {
                failed_triggers.push_back("gpkg-worker --refresh-selinux-label-state");
            }
        }
    }

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

    void release() {
        if (!locked) return;
        release_lock(verbose);
        locked = false;
    }

    ~ScopedLock() {
        release();
    }
};

struct TransactionGuard {
    ScopedLock lock;
    bool active;
    bool verbose;

    TransactionGuard(bool need_lock, bool v) : lock(need_lock, v), active(need_lock), verbose(v) {}

    ~TransactionGuard() {
        if (!active) return;
        run_triggers(verbose);
        lock.release();
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

struct Dependency;

struct UpgradeCatalogSkipEntry {
    std::string kind;
    std::string trigger;
    std::string configured_name;
    std::string resolved_name;
    std::string reason;
};

struct ResolvedUpgradeCatalog {
    std::string fingerprint;
    std::vector<std::string> resolved_roots;
    std::map<std::string, std::vector<std::string>> resolved_companions;
    std::vector<UpgradeCatalogSkipEntry> skipped_entries;
};

struct UpgradeContext {
    std::vector<PackageStatusRecord> registered_status_records;
    std::vector<PackageStatusRecord> dpkg_status_records;
    std::vector<BaseSystemRegistryEntry> base_entries;
    std::map<std::string, PackageStatusRecord> registered_status_by_package;
    std::map<std::string, PackageStatusRecord> dpkg_status_by_package;
    std::map<std::string, PackageStatusRecord> base_status_by_package;
    std::map<std::string, bool> base_presence_by_package;
    std::vector<std::string> registered_package_names;
    std::set<std::string> registered_package_set;
    std::set<std::string> exact_live_packages;
    std::set<std::string> present_base_packages;
    std::map<std::string, std::string> normalized_root_by_raw;
    std::map<std::string, std::vector<std::string>> shadowed_aliases_by_target;
    std::map<std::string, std::string> shadowed_base_alias_target;
    ResolvedUpgradeCatalog upgrade_catalog;
    bool upgrade_catalog_available = false;
    std::string upgrade_catalog_problem;
    mutable std::map<std::string, PackageMetadata> live_metadata_cache;
    mutable std::set<std::string> missing_live_metadata;
    mutable std::map<std::string, std::string> registered_version_cache;
    mutable std::set<std::string> missing_registered_versions;
};

UpgradeContext build_upgrade_context(bool verbose = false);
bool get_local_installed_package_version(
    const std::string& pkg_name,
    std::string* version_out = nullptr,
    UpgradeContext* context = nullptr
);
bool package_has_exact_live_install_state(
    const std::string& pkg_name,
    std::string* version_out = nullptr,
    UpgradeContext* context = nullptr
);
bool get_context_live_installed_package_metadata(
    UpgradeContext& context,
    const std::string& pkg_name,
    PackageMetadata& out_meta
);
bool load_upgrade_catalog(
    ResolvedUpgradeCatalog& out_catalog,
    std::string* problem_out = nullptr,
    bool verbose = false
);
std::map<std::string, std::vector<std::string>> get_planner_upgrade_companion_map(
    UpgradeContext* context = nullptr,
    bool verbose = false
);
std::string normalize_upgrade_root_name(
    const std::string& raw_name,
    UpgradeContext& context,
    bool verbose
);

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

const char* transaction_dependency_kind_label(TransactionDependencyKind kind) {
    switch (kind) {
        case TransactionDependencyKind::Recommended:
            return "recommended";
        case TransactionDependencyKind::Suggested:
            return "suggested";
        case TransactionDependencyKind::Required:
        default:
            return "required";
    }
}

bool transaction_dependency_is_optional(TransactionDependencyKind kind) {
    return kind != TransactionDependencyKind::Required;
}

std::vector<TransactionDependencyEdge> collect_transaction_dependency_edge_details(const PackageMetadata& meta) {
    std::vector<TransactionDependencyEdge> edges;
    std::set<std::string> seen;

    auto append_edges = [&](const std::vector<std::string>& relations, TransactionDependencyKind kind) {
        for (const auto& relation : relations) {
            if (!seen.insert(relation).second) continue;
            edges.push_back({relation, kind});
        }
    };

    append_edges(meta.depends, TransactionDependencyKind::Required);
    if (should_include_recommends_for_transaction(meta)) {
        append_edges(meta.recommends, TransactionDependencyKind::Recommended);
    }
    if (should_include_suggests_for_transaction(meta)) {
        append_edges(meta.suggests, TransactionDependencyKind::Suggested);
    }

    return edges;
}

std::vector<std::string> collect_transaction_dependency_edges(const PackageMetadata& meta) {
    std::vector<std::string> edges;
    for (const auto& edge : collect_transaction_dependency_edge_details(meta)) {
        edges.push_back(edge.relation);
    }
    return edges;
}

std::vector<std::string> collect_required_transaction_dependency_edges(const PackageMetadata& meta) {
    std::vector<std::string> unique;
    std::set<std::string> seen;
    for (const auto& edge : meta.depends) {
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
