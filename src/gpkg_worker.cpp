#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <cerrno>
#include <cstring>
#include <set>
#include <map>
#include <iomanip>

// Configuration
std::string g_root_prefix = "";

std::string get_info_dir() {
    return g_root_prefix + "/var/lib/gpkg/info/";
}

const std::string TMP_EXTRACT_PATH = "/tmp/gpkg_worker_extract/";

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
    std::string cmd = "mkdir -p " + path;
    return run_command(cmd) == 0;
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
            pkgs.push_back(fname.substr(0, fname.size() - extension.size()));
        }
    }
    closedir(d);
    return pkgs;
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

bool action_remove_safe(const std::string& pkg_name) {
    std::cout << "Removing " << pkg_name << "..." << std::endl;
    
    // prerm
    std::string prerm = get_info_dir() + pkg_name + ".prerm";
    if (access(prerm.c_str(), X_OK) == 0) {
        if (run_command(prerm) != 0) {
            std::cerr << "E: prerm script failed." << std::endl;
            return false;
        }
    }

    // Run registered undo commands (in reverse order)
    std::string undo_path = get_info_dir() + pkg_name + ".undo";
    std::vector<std::string> undo_cmds;
    std::ifstream undo_f(undo_path);
    if (undo_f) {
        std::string line;
        while (std::getline(undo_f, line)) {
            line = trim(line);
            if (!line.empty()) undo_cmds.push_back(line);
        }
        undo_f.close();
    }
    if (!undo_cmds.empty()) {
        VLOG("Executing " << undo_cmds.size() << " registered undo commands...");
        for (auto it = undo_cmds.rbegin(); it != undo_cmds.rend(); ++it) {
            run_command(*it);
        }
    }

    std::vector<std::string> owned_files = read_list_file(pkg_name);
    // Remove files
    for (auto it = owned_files.rbegin(); it != owned_files.rend(); ++it) {
        remove_path(*it);
    }

    // postrm
    std::string postrm = get_info_dir() + pkg_name + ".postrm";
    if (access(postrm.c_str(), X_OK) == 0) {
        run_command(postrm);
    }

    // Cleanup metadata
    run_command("rm -f " + get_info_dir() + pkg_name + ".*");
    refresh_linker_cache_if_available();
    
    std::cout << "✓ Removed " << pkg_name << std::endl;
    return true;
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
             if (owned_by_me.count(file)) continue;
             
             // Special case: Ignore /usr/share/info/dir as it's a shared directory index
             if (file == "/usr/share/info/dir") continue;

             collisions.push_back(file);
        }
    }

    if (collisions.empty()) return true;

    bool fatal = false;
    for (const auto& col : collisions) {
        bool owned = false;
        // Check who owns it
        for (const auto& other : get_installed_packages()) {
            if (other == pkg_name) continue;
            auto other_files = read_list_file(other);
            for (const auto& of : other_files) {
                if (of == col) {
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

    // 4. Preinst
    std::string preinst = TMP_EXTRACT_PATH + "scripts/preinst";
    if (access(preinst.c_str(), X_OK) == 0) {
        std::string cmd = preinst + " " + (is_upgrade ? "upgrade " + old_version : "install");
        if (run_command(cmd) != 0) {
             std::cerr << "E: preinst failed." << std::endl;
             return false;
        }
    }

    // 5. Extract to Root (Actual Install)
    std::string dest = g_root_prefix.empty() ? "/" : g_root_prefix;
    // CRITICAL FIX: --keep-directory-symlink prevents tar from replacing existing 
    // symlinks (like /usr/lib -> lib64) with directories from the package.
    std::string extract_cmd = "tar --keep-directory-symlink -xf " + data_tar + " -C " + dest;
    if (strip_data) extract_cmd += " --strip-components=1";
    
    if (run_command(extract_cmd) != 0) {
        std::cerr << "E: Extraction failed." << std::endl;
        return false;
    }

    // 6. Register in Database
    run_command("mkdir -p " + get_info_dir());

    std::string list_path = get_info_dir() + pkg_name + ".list";
    std::ofstream list_out(list_path);
    for (const auto& f : new_files) {
        list_out << f << "\n";
    }
    list_out.close();
    
    run_command("cp " + TMP_EXTRACT_PATH + "control.json " + get_info_dir() + pkg_name + ".json");
    
    // Copy scripts
    std::vector<std::string> scripts = {"preinst", "postinst", "prerm", "postrm"};
    for(const auto& s : scripts) {
        std::string src = TMP_EXTRACT_PATH + "scripts/" + s;
        if(access(src.c_str(), F_OK) == 0) {
             run_command("cp " + src + " " + get_info_dir() + pkg_name + "." + s);
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
        std::set<std::string> new_files_set(new_files.begin(), new_files.end());
        std::vector<std::string> orphans;
        for (const auto& old : old_files_set) {
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
             } else {
                 // We expect a file or symlink
                 if (S_ISDIR(st.st_mode)) {
                     std::cerr << "TYPE MISMATCH (Expected File): " << f << std::endl;
                     passed = false;
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
