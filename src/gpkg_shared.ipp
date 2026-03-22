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
#include <map>
#include <mutex>
#include <openssl/sha.h>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <sys/stat.h>
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
const std::string SYSTEM_PROVIDES_PATH = ROOT_PREFIX + "/etc/gpkg/system-provides.list";
const std::string UPGRADEABLE_SYSTEM_PATH = ROOT_PREFIX + "/etc/gpkg/upgradeable-system.list";
const std::string UPGRADE_COMPANIONS_PATH = ROOT_PREFIX + "/etc/gpkg/upgrade-companions.conf";
const std::string STATUS_FILE = ROOT_PREFIX + "/var/lib/gpkg/status";
const std::string INFO_DIR = ROOT_PREFIX + "/var/lib/gpkg/info/";
const std::string EXTENSION = ".gpkg";
const std::string LOCK_FILE = ROOT_PREFIX + "/var/lib/gpkg/lock";
constexpr size_t MAX_PARALLEL_PACKAGE_DOWNLOADS = 5;

int run_command(const std::string& cmd, bool verbose);
std::string shell_quote(const std::string& value);

struct CommandCaptureResult {
    int exit_code = 0;
    std::string log_path;
};

CommandCaptureResult run_command_captured(const std::string& cmd, bool verbose, const std::string& log_prefix);

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

std::set<std::string> g_pending_triggers;

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

void run_triggers(bool verbose) {
    if (g_pending_triggers.empty()) return;

    std::cout << Color::CYAN << "Processing triggers..." << Color::RESET << std::endl;
    if (verbose) std::cout << "[DEBUG] " << g_pending_triggers.size() << " triggers pending." << std::endl;

    std::vector<std::string> failed_triggers;
    for (const auto& cmd : g_pending_triggers) {
        if (verbose) std::cout << "[DEBUG] Running trigger: " << cmd << std::endl;
        if (run_command(cmd, verbose) != 0) {
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
    std::string filename;
    std::string sha512;
    std::string source_url;
    std::vector<std::string> depends;
    std::vector<std::string> conflicts;
    std::vector<std::string> provides;
};

#define VLOG(v, msg) do { if (v) std::cout << "[DEBUG] " << msg << std::endl; } while(0)
