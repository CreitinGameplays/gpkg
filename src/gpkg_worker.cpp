#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cerrno>
#include <cstring>
#include <set>
#include <map>
#include <iomanip>
#include <elf.h>

// Configuration
std::string g_root_prefix = "";

std::string get_info_dir() {
    return g_root_prefix + "/var/lib/gpkg/info/";
}

const std::string TMP_EXTRACT_PATH = "/tmp/gpkg_worker_extract/";

std::string path_parent_dir(const std::string& full_path);
bool write_text_file_atomic(const std::string& target_path, const std::string& content, mode_t mode = 0644);
bool copy_file_atomic(const std::string& source_path, const std::string& target_path);

// Logging
bool g_verbose = false;
#define VLOG(msg) do { if (g_verbose) std::cout << "[WORKER] " << msg << std::endl; } while(0)

// Utils
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if (std::string::npos == first) return str;
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
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

void refresh_linker_cache_if_available() {
    if (access("/sbin/ldconfig", X_OK) == 0) {
        run_command("/sbin/ldconfig");
        return;
    }
    if (access("/usr/sbin/ldconfig", X_OK) == 0) {
        run_command("/usr/sbin/ldconfig");
        return;
    }
    if (access("/bin/ldconfig", X_OK) == 0) {
        run_command("/bin/ldconfig");
        return;
    }
    if (access("/usr/bin/ldconfig", X_OK) == 0) {
        run_command("/usr/bin/ldconfig");
    }
}

bool mkdir_p(const std::string& path) {
    std::string cmd = "mkdir -p " + shell_quote(path);
    return run_command(cmd) == 0;
}

bool path_exists_no_follow(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

// --- Database (List File) Management ---

std::vector<std::string> read_list_file(const std::string& pkg_name) {
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
std::vector<std::string> get_installed_packages(const std::string& extension = ".list") {
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

bool looks_like_shared_object_path(const std::string& path) {
    if (path.length() >= 3 && path.substr(path.length() - 3) == ".so") return true;
    if (path.find(".so.") != std::string::npos) return true;
    return false;
}

bool validate_elf_file(const std::string& path, off_t size, std::string* error) {
    if (size < static_cast<off_t>(EI_NIDENT)) {
        if (looks_like_shared_object_path(path)) {
            if (error) *error = "shared object file is too small to be valid";
            return false;
        }
        return true;
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
        if (looks_like_shared_object_path(path)) {
            char text_prefix[16] = {0};
            std::ifstream in(path, std::ios::binary);
            if (in) {
                in.read(text_prefix, sizeof(text_prefix) - 1);
            }
            if (looks_like_linker_script_prefix(text_prefix)) return true;
            if (error) *error = "shared object is neither a valid ELF nor a linker script";
            return false;
        }
        return true;
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
        return true;
    }

    if (error) *error = "ELF file has an unknown class";
    return false;
}

bool is_etc_config_path(const std::string& path) {
    return path.size() > 5 && path.rfind("/etc/", 0) == 0;
}

std::string find_file_owner(const std::string& pkg_name, const std::string& file_path) {
    for (const auto& other : get_installed_packages()) {
        if (other == pkg_name) continue;
        auto other_files = read_list_file(other);
        for (const auto& owned_path : other_files) {
            if (owned_path == file_path) return other;
        }
    }
    return "";
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
    if (S_ISDIR(st.st_mode)) return false;

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
        entry.backup_path = TMP_EXTRACT_PATH + "preserve/" + std::to_string(preserve_index++) + ".orig";
        preserved.push_back(entry);
    }

    return preserved;
}

bool backup_preserved_config_files(const std::vector<PreservedConfigFile>& preserved) {
    if (preserved.empty()) return true;
    if (!mkdir_p(TMP_EXTRACT_PATH + "preserve")) return false;

    for (const auto& entry : preserved) {
        std::string source_path = g_root_prefix + entry.path;
        std::string cmd = "cp -a " + shell_quote(source_path) + " " + shell_quote(entry.backup_path);
        if (run_command(cmd) != 0) {
            std::cerr << "E: Failed to back up local config " << entry.path << std::endl;
            return false;
        }
    }

    return true;
}

bool paths_are_identical(const std::string& left, const std::string& right) {
    return run_command("cmp -s " + shell_quote(left) + " " + shell_quote(right)) == 0;
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
            if (run_command("rm -f " + shell_quote(live_path)) != 0) {
                std::cerr << "E: Failed to discard duplicate package config " << entry.path << std::endl;
                return false;
            }
            entry.staged_path.clear();
            VLOG("Keeping existing config " << entry.path << " (package copy was identical).");
        } else {
            if (run_command("mv -f " + shell_quote(live_path) + " " + shell_quote(staged_live_path)) != 0) {
                std::cerr << "E: Failed to stage package config as " << entry.path << ".gpkg-new" << std::endl;
                return false;
            }
            entry.staged_path = entry.path + ".gpkg-new";
            std::cout << "W: Preserving local config " << entry.path
                      << "; package version saved as " << entry.staged_path << std::endl;
        }

        if (run_command("cp -a " + shell_quote(entry.backup_path) + " " + shell_quote(live_path)) != 0) {
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
    if (entries.empty()) return true;

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
    if (S_ISDIR(st.st_mode)) return false;
    if (should_preserve_local_config_file(pkg_name, file_path)) return false;
    if (owned_by_me.count(file_path)) return false;
    return find_file_owner(pkg_name, file_path).empty();
}

std::vector<ReplacedSystemFile> collect_replaced_system_files(
    const std::string& pkg_name,
    const std::vector<std::string>& new_files,
    const std::set<std::string>& owned_by_me
) {
    std::vector<ReplacedSystemFile> entries = load_replaced_system_files(pkg_name);
    std::set<std::string> tracked_paths;
    for (const auto& entry : entries) tracked_paths.insert(entry.path);

    size_t next_index = entries.size();
    for (const auto& file : new_files) {
        if (tracked_paths.count(file)) continue;
        if (!should_backup_replaced_system_file(pkg_name, file, owned_by_me)) continue;

        ReplacedSystemFile entry;
        entry.path = file;
        entry.backup_path = get_replaced_system_dir(pkg_name) + "/" + std::to_string(next_index++);
        entries.push_back(entry);
        tracked_paths.insert(file);
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
        std::string parent_dir = entry.backup_path.substr(0, entry.backup_path.find_last_of('/'));
        if (!mkdir_p(parent_dir)) {
            std::cerr << "E: Failed to create system backup directory " << parent_dir << std::endl;
            return false;
        }

        std::string cmd = "cp -a " + shell_quote(source_path) + " " + shell_quote(entry.backup_path);
        if (run_command(cmd) != 0) {
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
            std::string cmd = "rmdir \"" + full_path + "\"";
            if (run_command(cmd) == 0) {
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

// Helper to detect if archive has data/ prefix
bool detect_data_prefix(const std::string& tar_path) {
    std::string cmd = "tar -tf " + tar_path + " | head -n 5";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return false;
    char buffer[1024];
    bool has_data = false;
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line = trim(buffer);
        if (line.find("./data/") == 0 || line.find("data/") == 0) {
            has_data = true;
            break;
        }
    }
    pclose(pipe);
    return has_data;
}

std::vector<std::string> get_tar_contents(const std::string& tar_path, bool strip_data) {
    std::vector<std::string> list;
    std::string cmd = "tar -tf " + tar_path;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return list;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line = trim(buffer);
        if (line.empty() || line == "." || line == "./") continue;
        
        if (line.find("./") == 0) line = line.substr(2);
        
        if (strip_data) {
            if (line.find("data/") == 0) {
                line = line.substr(5);
            } else {
                continue; // Skip items not in data/ if stripping is active
            }
        }

        // Remove trailing /
        if (!line.empty() && line.back() == '/') line.pop_back();
        
        if (!line.empty()) list.push_back("/" + line); 
    }
    pclose(pipe);
    return list;
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

    struct stat target_st;
    return stat(full_path.c_str(), &target_st) == 0 && S_ISDIR(target_st.st_mode);
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

struct InstallRollbackEntry {
    std::string path;
    std::string live_full_path;
    std::string backup_full_path;
    bool created_only = false;
};

void rollback_install_changes(const std::vector<InstallRollbackEntry>& rollback_entries);
void discard_install_backups(const std::vector<InstallRollbackEntry>& rollback_entries);

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

std::string allocate_sibling_temp_path(const std::string& live_full_path, const std::string& tag, int* fd_out = nullptr) {
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
            std::string cmd = "mv -f " + shell_quote(live_full_path) + " " + shell_quote(backup_full_path);
            if (run_command(cmd) != 0) {
                std::cerr << "E: Failed to move existing path aside for " << live_full_path << " (via mv fallback)" << std::endl;
                // 'mv' failed, potentially leaving a partial copy. Use rm -rf to clean it up since it might be a directory
                run_command("rm -rf " + shell_quote(backup_full_path));
                return false;
            }
        } else {
            std::cerr << "E: Failed to move existing path aside for " << live_full_path << ": "
                      << strerror(errno) << std::endl;
            unlink(backup_full_path.c_str());
            return false;
        }
    }

    if (had_existing) *had_existing = true;
    rollback_entries.push_back({path, live_full_path, backup_full_path, false});
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
    std::vector<InstallRollbackEntry>& rollback_entries
) {
    std::vector<std::string> metadata_paths = {
        get_info_dir() + pkg_name + ".list",
        get_info_dir() + pkg_name + ".json",
        get_info_dir() + pkg_name + ".undo",
        get_info_dir() + pkg_name + ".preinst",
        get_info_dir() + pkg_name + ".postinst",
        get_info_dir() + pkg_name + ".prerm",
        get_info_dir() + pkg_name + ".postrm",
        get_replaced_system_manifest(pkg_name),
        get_replaced_system_dir(pkg_name)
    };

    for (const auto& full_path : metadata_paths) {
        if (!backup_live_path_if_present(full_path, full_path, rollback_entries)) {
            return false;
        }
    }

    return true;
}

bool action_remove_safe(const std::string& pkg_name) {
    std::cout << "Removing " << pkg_name << "..." << std::endl;

    std::string prerm = get_info_dir() + pkg_name + ".prerm";
    if (access(prerm.c_str(), X_OK) == 0) {
        if (run_command(prerm) != 0) {
            std::cerr << "E: prerm script failed." << std::endl;
            return false;
        }
    }

    std::string undo_path = get_info_dir() + pkg_name + ".undo";
    std::vector<std::string> undo_cmds;
    std::ifstream undo_f(undo_path);
    if (undo_f) {
        std::string line;
        while (std::getline(undo_f, line)) {
            line = trim(line);
            if (!line.empty()) undo_cmds.push_back(line);
        }
    }

    std::vector<std::string> owned_files = read_list_file(pkg_name);
    sort_paths_for_removal(owned_files);

    std::vector<InstallRollbackEntry> removal_rollback_entries;
    for (const auto& path : owned_files) {
        if (!stage_owned_path_removal(path, removal_rollback_entries)) {
            rollback_install_changes(removal_rollback_entries);
            std::cerr << "E: Failed while staging removal of " << path << std::endl;
            return false;
        }
    }

    if (!stage_replaced_system_restore(pkg_name, removal_rollback_entries)) {
        rollback_install_changes(removal_rollback_entries);
        std::cerr << "E: Failed to restore replaced system files safely." << std::endl;
        return false;
    }

    if (!undo_cmds.empty()) {
        VLOG("Executing " << undo_cmds.size() << " registered undo commands...");
        for (auto it = undo_cmds.rbegin(); it != undo_cmds.rend(); ++it) {
            if (run_command(*it) != 0) {
                rollback_install_changes(removal_rollback_entries);
                std::cerr << "E: Undo command failed during removal." << std::endl;
                return false;
            }
        }
    }

    std::string postrm = get_info_dir() + pkg_name + ".postrm";
    if (access(postrm.c_str(), X_OK) == 0) {
        if (run_command(postrm) != 0) {
            rollback_install_changes(removal_rollback_entries);
            std::cerr << "E: postrm script failed." << std::endl;
            return false;
        }
    }

    if (!stage_package_metadata_removal(pkg_name, removal_rollback_entries)) {
        rollback_install_changes(removal_rollback_entries);
        std::cerr << "E: Failed to remove package metadata safely." << std::endl;
        return false;
    }

    refresh_linker_cache_if_available();
    discard_install_backups(removal_rollback_entries);

    std::cout << "✓ Removed " << pkg_name << std::endl;
    return true;
}

bool build_staged_install_entries(
    const std::vector<std::string>& new_files,
    const std::string& payload_root,
    std::vector<StagedInstallEntry>& entries
) {
    entries.clear();

    for (const auto& path : new_files) {
        std::string staged_path = payload_root + path;
        struct stat st;
        if (lstat(staged_path.c_str(), &st) != 0) {
            if (errno == ENOENT) continue;
            std::cerr << "E: Failed to inspect staged payload entry " << staged_path
                      << ": " << strerror(errno) << std::endl;
            return false;
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
                std::cerr << "E: Failed to read staged symlink " << staged_path << ": "
                          << strerror(errno) << std::endl;
                return false;
            }
            target[static_cast<size_t>(len)] = '\0';
            entry.symlink_target.assign(target.data(), static_cast<size_t>(len));
        }

        if (!entry.is_directory && !entry.is_symlink && !S_ISREG(st.st_mode)) {
            std::cerr << "E: Unsupported staged payload entry type for " << path << std::endl;
            return false;
        }

        entries.push_back(entry);
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
        remove_live_path_exact(it->live_full_path);
        if (!it->backup_full_path.empty()) {
            if (rename(it->backup_full_path.c_str(), it->live_full_path.c_str()) != 0) {
                if (errno == EXDEV) {
                    std::string cmd = "mv -f " + shell_quote(it->backup_full_path) + " " + shell_quote(it->live_full_path);
                    run_command(cmd);
                }
            }
        }
    }
}

void discard_install_backups(const std::vector<InstallRollbackEntry>& rollback_entries) {
    for (const auto& entry : rollback_entries) {
        if (entry.backup_full_path.empty()) continue;
        run_command("rm -rf " + shell_quote(entry.backup_full_path));
    }
}

bool apply_staged_install_entries(
    const std::vector<StagedInstallEntry>& entries,
    std::vector<InstallRollbackEntry>& rollback_entries
) {
    rollback_entries.clear();

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

std::vector<std::string> get_staged_replaces() {
    std::vector<std::string> replaced;
    std::ifstream f(TMP_EXTRACT_PATH + "control.json");
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
    std::set<std::string> owned_by_me;
    auto existing_files = read_list_file(pkg_name);
    for(const auto& f : existing_files) owned_by_me.insert(f);

    std::vector<std::string> collisions;

    for (const auto& file : new_files) {
        std::string full_path = g_root_prefix + file;
        if (access(full_path.c_str(), F_OK) == 0) {
             struct stat st;
             if (stat(full_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) continue;
             if (should_preserve_local_config_file(pkg_name, file)) continue;
             if (owned_by_me.count(file)) continue;
             
             // Special case: Ignore /usr/share/info/dir as it's a shared directory index
             if (file == "/usr/share/info/dir") continue;

             collisions.push_back(file);
        }
    }

    if (collisions.empty()) return true;

    bool fatal = false;
    std::vector<std::string> replaced = get_staged_replaces();
    
    for (const auto& col : collisions) {
        bool owned = false;
        // Check who owns it
        for (const auto& other : get_installed_packages()) {
            if (other == pkg_name) continue;
            auto other_files = read_list_file(other);
            for (const auto& of : other_files) {
                if (of == col) {
                    if (std::find(replaced.begin(), replaced.end(), other) != replaced.end()) {
                        std::cout << "W: Permitted overwrite of " << col << " because " << pkg_name << " replaces " << other << std::endl;
                        owned = true;
                        break;
                    }
                    std::cerr << "E: Conflict: " << col << " is owned by " << other << std::endl;
                    owned = true;
                    fatal = true;
                    break;
                }
            }
            if (owned) break;
        }
        if (!owned) {
             std::cerr << "W: Overwriting unowned file " << col << std::endl;
        }
    }
    
    return !fatal;
}

// Helper to get version from installed package
std::string get_package_version(const std::string& pkg_name) {
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

bool action_install(const std::string& pkg_file) {
    // 1. Unpack to temp
    run_command("rm -rf " + TMP_EXTRACT_PATH + " && mkdir -p " + TMP_EXTRACT_PATH);
    std::string tmp_tar = TMP_EXTRACT_PATH + "temp.tar";
    
    if (run_command("zstd -df " + pkg_file + " -o " + tmp_tar) != 0) {
        std::cerr << "E: Decompression failed." << std::endl;
        return false;
    }
    
    run_command("tar -xf " + tmp_tar + " -C " + TMP_EXTRACT_PATH);
    
    std::string data_tar_zst = TMP_EXTRACT_PATH + "data.tar.zst";
    std::string data_tar = TMP_EXTRACT_PATH + "data.tar";
    
    if (run_command("zstd -df " + data_tar_zst + " -o " + data_tar) != 0) {
         std::cerr << "E: Data decompression failed." << std::endl;
         return false;
    }

    // 2. Get File List & Pkg Name
    bool strip_data = detect_data_prefix(data_tar);
    if (strip_data) VLOG("Detected 'data/' prefix. Will strip components.");
    
    std::vector<std::string> new_files = get_tar_contents(data_tar, strip_data);
    
    std::string pkg_name;
    std::string new_version;
    std::ifstream control_file(TMP_EXTRACT_PATH + "control.json");
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

    // 3. Check Collisions & Detect Upgrade
    if (!check_collisions(pkg_name, new_files)) {
        run_command("rm -rf " + TMP_EXTRACT_PATH);
        return false;
    }

    bool is_upgrade = false;
    std::string old_version = get_package_version(pkg_name);
    std::set<std::string> old_files_set;
    if (!old_version.empty()) {
        is_upgrade = true;
        auto old_files_vec = read_list_file(pkg_name);
        for(const auto& f : old_files_vec) old_files_set.insert(f);
    }
    std::vector<ReplacedSystemFile> replaced_system_files =
        collect_replaced_system_files(pkg_name, new_files, old_files_set);

    // 4. Preinst
    std::string preinst = TMP_EXTRACT_PATH + "scripts/preinst";
    if (access(preinst.c_str(), X_OK) == 0) {
        std::string cmd = preinst + " " + (is_upgrade ? "upgrade " + old_version : "install");
        if (run_command(cmd) != 0) {
             std::cerr << "E: preinst failed." << std::endl;
             return false;
        }
    }

    std::vector<PreservedConfigFile> preserved_configs =
        collect_preserved_config_files(pkg_name, new_files);
    if (!backup_preserved_config_files(preserved_configs)) {
        return false;
    }
    if (!backup_replaced_system_files(replaced_system_files)) {
        return false;
    }

    // 5. Extract into a staging tree first, then apply entries atomically.
    std::string payload_root = TMP_EXTRACT_PATH + "payload";
    run_command("rm -rf " + shell_quote(payload_root));
    if (!mkdir_p(payload_root)) {
        std::cerr << "E: Failed to create payload staging directory." << std::endl;
        return false;
    }

    std::string extract_cmd = "tar -xf " + data_tar + " -C " + payload_root;
    if (strip_data) extract_cmd += " --strip-components=1";
    
    if (run_command(extract_cmd) != 0) {
        std::cerr << "E: Extraction failed." << std::endl;
        return false;
    }

    std::vector<StagedInstallEntry> staged_entries;
    if (!build_staged_install_entries(new_files, payload_root, staged_entries)) {
        return false;
    }

    std::vector<InstallRollbackEntry> install_rollback_entries;
    if (!apply_staged_install_entries(staged_entries, install_rollback_entries)) {
        rollback_install_changes(install_rollback_entries);
        std::cerr << "E: Failed to apply staged filesystem changes safely." << std::endl;
        return false;
    }

    if (!finalize_preserved_config_files(preserved_configs)) {
        rollback_install_changes(install_rollback_entries);
        return false;
    }

    std::vector<std::string> installed_files = new_files;
    apply_preserved_config_metadata(installed_files, preserved_configs);

    // 6. Register in Database
    run_command("mkdir -p " + get_info_dir());

    std::ostringstream list_buffer;
    for (const auto& f : installed_files) {
        list_buffer << f << "\n";
    }
    if (!write_text_file_atomic(get_info_dir() + pkg_name + ".list", list_buffer.str(), 0644)) {
        rollback_install_changes(install_rollback_entries);
        std::cerr << "E: Failed to write package file manifest." << std::endl;
        return false;
    }
    
    if (!copy_file_atomic(TMP_EXTRACT_PATH + "control.json", get_info_dir() + pkg_name + ".json")) {
        rollback_install_changes(install_rollback_entries);
        std::cerr << "E: Failed to write installed package metadata." << std::endl;
        return false;
    }
    if (!write_replaced_system_files(pkg_name, replaced_system_files)) {
        rollback_install_changes(install_rollback_entries);
        return false;
    }
    
    // Copy scripts
    std::vector<std::string> scripts = {"preinst", "postinst", "prerm", "postrm"};
    for(const auto& s : scripts) {
        std::string src = TMP_EXTRACT_PATH + "scripts/" + s;
        if(access(src.c_str(), F_OK) == 0) {
             if (!copy_file_atomic(src, get_info_dir() + pkg_name + "." + s)) {
                 rollback_install_changes(install_rollback_entries);
                 std::cerr << "E: Failed to install maintainer script " << s << "." << std::endl;
                 return false;
             }
        }
    }

    // 7. Postinst
    std::string installed_postinst = get_info_dir() + pkg_name + ".postinst";
    if (access(installed_postinst.c_str(), X_OK) == 0) {
         std::string cmd = installed_postinst + " " + (is_upgrade ? "configure " + old_version : "configure");
         run_command(cmd);
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

    refresh_linker_cache_if_available();
    discard_install_backups(install_rollback_entries);

    std::cout << "✓ Installed " << pkg_name << " (" << new_version << ")" << std::endl;
    
    run_command("rm -rf " + TMP_EXTRACT_PATH);
    return true;
}

// --- Verification Logic ---

bool action_verify(const std::string& pkg_name) {
    if (pkg_name.empty()) {
        std::cerr << "E: No package specified for verification." << std::endl;
        return false;
    }

    std::vector<std::string> files = read_list_file(pkg_name);
    if (files.empty()) {
        std::cerr << "E: Package " << pkg_name << " not found or empty." << std::endl;
        return false;
    }

    std::cout << "Verifying " << pkg_name << "..." << std::endl;
    bool passed = true;
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
    }
    
    if (passed) std::cout << "✓ Verification passed." << std::endl;
    else std::cout << "X Verification failed." << std::endl;
    return passed;
}

int main(int argc, char* argv[]) {

    if (argc < 2) {

        std::cout << "Usage: gpkg-worker [options]\nOptions:\n  --install <file>\n  --remove <pkg>\n  --verify <pkg>\n  --register-file <path> --pkg <name>\n  --register-undo <cmd> --pkg <name>\n";

        return 1;

    }

    std::string mode, target, pkg_name;

    for (int i = 1; i < argc; ++i) {

        std::string arg = argv[i];

        if (arg == "--install") mode = "install", target = argv[++i];

        else if (arg == "--remove") mode = "remove", target = argv[++i];

        else if (arg == "--verify") mode = "verify", target = argv[++i];

        else if (arg == "--register-file") mode = "register-file", target = argv[++i];

        else if (arg == "--register-undo") mode = "register-undo", target = argv[++i];

        else if (arg == "--pkg") pkg_name = argv[++i];

        else if (arg == "--root") g_root_prefix = argv[++i];

        else if (arg == "-v" || arg == "--verbose") g_verbose = true;

    }

    if (mode == "remove" && !target.empty()) return action_remove_safe(target) ? 0 : 1;

    if (mode == "install" && !target.empty()) return action_install(target) ? 0 : 1;

    if (mode == "verify" && !target.empty()) return action_verify(target) ? 0 : 1;

    if (mode == "register-file" && !target.empty() && !pkg_name.empty()) return action_register_file(pkg_name, target) ? 0 : 1;

    if (mode == "register-undo" && !target.empty() && !pkg_name.empty()) return action_register_undo(pkg_name, target) ? 0 : 1;

    std::cerr << "Invalid arguments or missing target/pkg.\n";

    return 1;

}
