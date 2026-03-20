#include "network.h"
#include "debug.h"
#include "signals.h"
#include "sys_info.h"
#include <cstdio>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <algorithm>
#include <csignal>
#include <sstream>
#include <regex>
#include <set>
#include <map>
#include <cctype>
#include <openssl/sha.h>
#include <iomanip>

// Colors
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
// v2 Configuration
#ifdef DEV_MODE
    const std::string ROOT_PREFIX = "rootfs";
#else
    const std::string ROOT_PREFIX = "";
#endif

const std::string REPO_CACHE_PATH = ROOT_PREFIX + "/var/repo/";
const std::string SOURCES_LIST_PATH = ROOT_PREFIX + "/etc/gpkg/sources.list";
const std::string SOURCES_DIR = ROOT_PREFIX + "/etc/gpkg/sources.list.d/";
const std::string SYSTEM_PROVIDES_PATH = ROOT_PREFIX + "/etc/gpkg/system-provides.list";
const std::string STATUS_FILE = ROOT_PREFIX + "/var/lib/gpkg/status";
const std::string INFO_DIR = ROOT_PREFIX + "/var/lib/gpkg/info/";
const std::string TMP_EXTRACT_PATH = "/tmp/gpkg_extract/";
const std::string EXTENSION = ".gpkg";
const std::string LOCK_FILE = ROOT_PREFIX + "/var/lib/gpkg/lock";

// Forward Declaration
int run_command(const std::string& cmd, bool verbose);

bool mkdir_p(const std::string& path) {
    if (path.empty()) return false;
    std::string current_path = "";
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

void release_lock(bool verbose) {
    if (verbose) std::cout << "[DEBUG] Releasing lock: " << LOCK_FILE << std::endl;
    unlink(LOCK_FILE.c_str());
}

bool acquire_lock(bool verbose) {
    // Ensure directory exists
    std::string lock_dir = LOCK_FILE.substr(0, LOCK_FILE.find_last_of('/'));
    struct stat st;
    if (stat(lock_dir.c_str(), &st) != 0) {
        if (verbose) std::cout << "[DEBUG] Creating lock directory: " << lock_dir << std::endl;
        if (!mkdir_p(lock_dir)) {
            std::cerr << Color::RED << "E: Failed to create lock directory: " << lock_dir << " (errno: " << errno << ")" << Color::RESET << std::endl;
            return false;
        }
    }

    if (access(LOCK_FILE.c_str(), F_OK) == 0) {
        std::cerr << Color::RED << "E: Could not acquire lock (" << LOCK_FILE << "). Is another process using it?" << Color::RESET << std::endl;
        return false;
    }
    // Create lock file
    if (verbose) std::cout << "[DEBUG] Acquiring lock: " << LOCK_FILE << std::endl;
    int fd = open(LOCK_FILE.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        std::cerr << Color::RED << "E: Failed to create lock file: " << LOCK_FILE << " (errno: " << errno << ")" << Color::RESET << std::endl;
        return false;
    }
    close(fd);
    return true;
}

void check_triggers(const std::vector<std::string>& files) {
    for (const auto& file : files) {
         if (file.find("usr/share/glib-2.0/schemas") != std::string::npos) 
             g_pending_triggers.insert("glib-compile-schemas /usr/share/glib-2.0/schemas");
         
         if (file.find("usr/share/icons") != std::string::npos)
             g_pending_triggers.insert("gtk-update-icon-cache -q -t -f /usr/share/icons/hicolor"); 
         
         if (file.find("usr/share/mime") != std::string::npos)
             g_pending_triggers.insert("update-mime-database /usr/share/mime");
         
         if (file.find("usr/share/applications") != std::string::npos)
             g_pending_triggers.insert("update-desktop-database /usr/share/applications");
         
         if (file.find("lib/") != std::string::npos || file.find("lib64/") != std::string::npos)
             g_pending_triggers.insert("ldconfig");
    }
}

void run_triggers(bool verbose) {
    if (g_pending_triggers.empty()) return;
    std::cout << Color::CYAN << "Processing triggers..." << Color::RESET << std::endl;
    if (verbose) std::cout << "[DEBUG] " << g_pending_triggers.size() << " triggers pending." << std::endl;
    for (const auto& cmd : g_pending_triggers) {
        if (verbose) std::cout << "[DEBUG] Running trigger: " << cmd << std::endl;
        run_command(cmd, verbose);
    }
}

struct ScopedLock {
    bool locked = false;
    bool verbose = false;
    ScopedLock(bool active, bool v) : verbose(v) {
        if (active) {
            if (acquire_lock(verbose)) locked = true;
            else { exit(1); }
        }
    }
    ~ScopedLock() { if (locked) release_lock(verbose); }
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
    // Lock will be released by cleanup or forceful unlink if needed
    unlink(LOCK_FILE.c_str());
    std::cerr << "\n[!] Interrupted. Lock released." << std::endl;
    exit(130);
}

std::string get_command_output(const std::string& cmd) {
    char buffer[128];
    std::string result = "";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    while (!feof(pipe)) {
        if (fgets(buffer, 128, pipe) != NULL)
            result += buffer;
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

// Verbose logging helper
#define VLOG(v, msg) do { if (v) std::cout << "[DEBUG] " << msg << std::endl; } while(0)

// Start Helpers
// Helper to trim whitespace
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\n\r");
    if (std::string::npos == first) return str;
    size_t last = str.find_last_not_of(" \t\n\r");
    return str.substr(first, (last - first + 1));
}

std::string join_strings(const std::vector<std::string>& items, const std::string& separator = ", ") {
    std::stringstream ss;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) ss << separator;
        ss << items[i];
    }
    return ss.str();
}

unsigned int hex_digit_value(char c) {
    if (c >= '0' && c <= '9') return static_cast<unsigned int>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<unsigned int>(10 + (c - 'a'));
    if (c >= 'A' && c <= 'F') return static_cast<unsigned int>(10 + (c - 'A'));
    return 0;
}

void append_utf8_codepoint(std::string& out, unsigned int codepoint) {
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

std::string json_unescape(const std::string& input) {
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
                    codepoint = (codepoint << 4) | hex_digit_value(hex);
                }

                if (!valid) {
                    output += "\\u";
                    break;
                }

                append_utf8_codepoint(output, codepoint);
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

std::string normalize_whitespace(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    bool previous_was_space = false;

    for (char c : input) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!output.empty() && !previous_was_space) {
                output += ' ';
            }
            previous_was_space = true;
        } else {
            output += c;
            previous_was_space = false;
        }
    }

    return trim(output);
}

std::string description_summary(const std::string& description, size_t max_len = 140) {
    std::stringstream ss(description);
    std::string line;
    while (std::getline(ss, line)) {
        line = normalize_whitespace(line);
        if (!line.empty()) {
            if (line.size() > max_len) {
                return line.substr(0, max_len - 3) + "...";
            }
            return line;
        }
    }

    std::string condensed = normalize_whitespace(description);
    if (condensed.size() > max_len) {
        return condensed.substr(0, max_len - 3) + "...";
    }
    return condensed;
}

void print_wrapped_block(const std::string& prefix, const std::string& text, size_t width = 96) {
    std::istringstream words(text);
    std::string word;
    std::string line = prefix;
    size_t line_length = prefix.size();
    const size_t prefix_length = prefix.size();

    while (words >> word) {
        const size_t extra = (line_length > prefix_length ? 1 : 0) + word.size();
        if (line_length + extra > width && line_length > prefix_length) {
            std::cout << line << std::endl;
            line = prefix + word;
            line_length = prefix_length + word.size();
            continue;
        }

        if (line_length > prefix_length) {
            line += ' ';
            line_length++;
        }
        line += word;
        line_length += word.size();
    }

    if (line_length > prefix_length) {
        std::cout << line << std::endl;
    } else {
        std::cout << prefix << std::endl;
    }
}

void print_description_block(const std::string& label, const std::string& text) {
    std::cout << "  " << label << ":" << std::endl;

    std::stringstream ss(text);
    std::string line;
    std::string paragraph;
    bool printed_any = false;

    auto flush_paragraph = [&]() {
        std::string normalized = normalize_whitespace(paragraph);
        if (!normalized.empty()) {
            print_wrapped_block("    ", normalized);
            printed_any = true;
        }
        paragraph.clear();
    };

    while (std::getline(ss, line)) {
        line = trim(line);
        if (line == ".") line.clear();
        if (line.empty()) {
            flush_paragraph();
            continue;
        }
        if (!paragraph.empty()) paragraph += ' ';
        paragraph += line;
    }

    flush_paragraph();

    if (!printed_any) {
        std::cout << "    (none)" << std::endl;
    }
}

std::string normalize_repo_base_url(const std::string& raw_url) {
    std::string url = trim(raw_url);
    const std::string index_suffix = "/" + std::string(OS_ARCH) + "/Packages.json.zst";
    const std::string arch_suffix = "/" + std::string(OS_ARCH);

    if (url.size() >= index_suffix.size() &&
        url.compare(url.size() - index_suffix.size(), index_suffix.size(), index_suffix) == 0) {
        url = url.substr(0, url.size() - index_suffix.size());
    }

    while (url.size() > 1 && url.back() == '/') url.pop_back();

    if (url.size() >= arch_suffix.size() &&
        url.compare(url.size() - arch_suffix.size(), arch_suffix.size(), arch_suffix) == 0) {
        url = url.substr(0, url.size() - arch_suffix.size());
    }

    while (url.size() > 1 && url.back() == '/') url.pop_back();
    return url;
}

std::string build_repo_index_url(const std::string& base_url) {
    std::string normalized = normalize_repo_base_url(base_url);
    return normalized + "/" + std::string(OS_ARCH) + "/Packages.json.zst";
}

std::string build_repo_package_url(const std::string& base_url, const std::string& filename) {
    std::string normalized = normalize_repo_base_url(base_url);
    return normalized + "/" + std::string(OS_ARCH) + "/" + filename;
}

std::string json_escape(const std::string& input) {
    std::string escaped;
    for (char c : input) {
        switch (c) {
            case '\\': escaped += "\\\\"; break;
            case '"': escaped += "\\\""; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default: escaped += c; break;
        }
    }
    return escaped;
}

std::string inject_repo_url(const std::string& obj, const std::string& repo_url) {
    size_t end = obj.rfind('}');
    if (end == std::string::npos) return obj;
    return obj.substr(0, end) + ",\"repo_url\":\"" + json_escape(normalize_repo_base_url(repo_url)) + "\"}";
}

// Extract the next JSON object from content starting at pos. Returns object bounds or empty string.
bool extract_json_object(const std::string& content, size_t& pos, std::string& out_obj) {
    pos = content.find("{", pos);
    if (pos == std::string::npos) return false;
    
    int depth = 0;
    for (size_t i = pos; i < content.length(); ++i) {
        if (content[i] == '{') depth++;
        else if (content[i] == '}' && --depth == 0) {
            out_obj = content.substr(pos, i - pos + 1);
            pos = i + 1;
            return true;
        }
    }
    return false;
}

// Iterate all JSON objects in file, calling callback for each. Stops if callback returns false.
template<typename Func>
void foreach_json_object(const std::string& filepath, Func callback) {
    std::ifstream f(filepath);
    if (!f) return;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    
    size_t pos = 0;
    std::string obj;
    while (extract_json_object(content, pos, obj)) {
        if (!callback(obj)) break;
    }
}

// Get list of installed package names from INFO_DIR
std::vector<std::string> get_installed_packages(const std::string& extension = ".json") {
    std::vector<std::string> pkgs;
    DIR* d = opendir(INFO_DIR.c_str());
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

// Helper to extract a string value from a simple JSON object string
bool get_json_value(const std::string& obj, const std::string& key, std::string& out_val) {
    size_t key_pos = obj.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return false;
    
    size_t colon = obj.find(":", key_pos);
    if (colon == std::string::npos) return false;
    
    size_t v_start = obj.find("\"", colon);
    if (v_start == std::string::npos) {
         return false; 
    }
    
    size_t v_end = obj.find("\"", v_start + 1);
    while (v_end != std::string::npos && obj[v_end-1] == '\\') {
         v_end = obj.find("\"", v_end + 1);
    }
    
    if (v_end == std::string::npos) return false;
    
    out_val = json_unescape(obj.substr(v_start + 1, v_end - v_start - 1));
    return true;
}

// Helper to extract array of strings
bool get_json_array(const std::string& obj, const std::string& key, std::vector<std::string>& out_arr) {
    out_arr.clear();
    size_t key_pos = obj.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return false;

    size_t colon = obj.find(":", key_pos);
    size_t arr_start = obj.find("[", colon);
    size_t arr_end = obj.find("]", arr_start);
    
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
        out_arr.push_back(json_unescape(obj.substr(value_start + 1, value_end - value_start - 1)));
        pos = value_end + 1;
    }
    return true;
}

// Helper: Ask user yes/no
bool ask_confirmation(const std::string& query) {
    std::cout << Color::YELLOW << query << " [Y/n] " << Color::RESET;
    std::string response;
    std::getline(std::cin, response);
    return (response.empty() || response == "y" || response == "Y" || response == "yes");
}
// End Helpers

struct DebianVersion {
    long long epoch = 0;
    std::string upstream;
    std::string revision;
};

int debian_char_order(char c) {
    if (c == '~') return -1;
    if (c == '\0') return 0;
    if (std::isalpha(static_cast<unsigned char>(c))) return static_cast<unsigned char>(c);
    return static_cast<unsigned char>(c) + 256;
}

int compare_debian_part(const std::string& left, const std::string& right) {
    size_t i = 0;
    size_t j = 0;

    while (i < left.size() || j < right.size()) {
        while ((i < left.size() && !std::isdigit(static_cast<unsigned char>(left[i]))) ||
               (j < right.size() && !std::isdigit(static_cast<unsigned char>(right[j])))) {
            char lc = i < left.size() ? left[i] : '\0';
            char rc = j < right.size() ? right[j] : '\0';
            int lo = debian_char_order(lc);
            int ro = debian_char_order(rc);
            if (lo < ro) return -1;
            if (lo > ro) return 1;
            if (i < left.size()) ++i;
            if (j < right.size()) ++j;
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

DebianVersion parse_debian_version(const std::string& version) {
    DebianVersion parsed;
    std::string remainder = version;

    size_t epoch_sep = version.find(':');
    if (epoch_sep != std::string::npos) {
        std::string epoch_str = version.substr(0, epoch_sep);
        if (!epoch_str.empty()) {
            parsed.epoch = std::strtoll(epoch_str.c_str(), nullptr, 10);
        }
        remainder = version.substr(epoch_sep + 1);
    }

    size_t revision_sep = remainder.rfind('-');
    if (revision_sep != std::string::npos) {
        parsed.upstream = remainder.substr(0, revision_sep);
        parsed.revision = remainder.substr(revision_sep + 1);
    } else {
        parsed.upstream = remainder;
        parsed.revision = "";
    }

    return parsed;
}

// Returns: -1 if v1 < v2, 0 if v1 == v2, 1 if v1 > v2
int compare_versions(const std::string& v1, const std::string& v2) {
    if (v1 == v2) return 0;
    DebianVersion left = parse_debian_version(v1);
    DebianVersion right = parse_debian_version(v2);

    if (left.epoch < right.epoch) return -1;
    if (left.epoch > right.epoch) return 1;

    int upstream_cmp = compare_debian_part(left.upstream, right.upstream);
    if (upstream_cmp != 0) return upstream_cmp;

    return compare_debian_part(left.revision, right.revision);
}

// Check if package is installed and optionally return its version
bool is_installed(const std::string& pkg, std::string* out_version = nullptr) {
    std::string info_path = INFO_DIR + pkg + ".json";
    if (access(info_path.c_str(), F_OK) == 0) {
        if (out_version) {
            std::ifstream f(info_path);
            if (f) {
                std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                return get_json_value(content, "version", *out_version);
            }
            return false;
        }
        return true;
    }
    
    return false;
}

// Helper to run shell commands and get output
int run_command(const std::string& cmd, bool verbose) {
    if (verbose) std::cout << "[DEBUG] Executing: " << cmd << std::endl;
    return system(cmd.c_str());
}

// Helper to get file list from tar
std::vector<std::string> get_tar_file_list(const std::string& tar_path, bool strip_data) {
    std::string tar_list_cmd = "tar -tf " + tar_path;
    
    std::string raw_list = get_command_output(tar_list_cmd);
    std::stringstream ss_in(raw_list);
    std::string line;
    std::vector<std::string> file_list;

    while (std::getline(ss_in, line)) {
        line = trim(line);
        if (line.empty() || line == "." || line == "./") continue;
        
        // Remove ./ prefix if it exists
        if (line.size() >= 2 && line.substr(0, 2) == "./") {
            line = line.substr(2);
        }

        if (line.empty() || line == ".") continue;

        if (strip_data) {
            size_t first_slash = line.find('/');
            if (first_slash != std::string::npos) {
                line = line.substr(first_slash + 1);
            } else {
                continue; // It was just the prefix directory itself
            }
        }
        
        if (line.empty() || line == "." || line == "./") continue;
        
        file_list.push_back(line);
    }
    return file_list;
}

bool check_file_collisions(const std::string& pkg_name, const std::vector<std::string>& new_files, bool verbose) {
    // 1. Get current package's file list if installed (to allow upgrades)
    std::set<std::string> owned_files;
    if (is_installed(pkg_name)) {
        std::string list_path = INFO_DIR + pkg_name + ".list";
        std::ifstream f(list_path);
        std::string line;
        while (std::getline(f, line)) {
            line = trim(line);
            if (!line.empty()) owned_files.insert(line);
        }
    }

    // 2. Scan for collisions
    std::vector<std::string> collisions;
    for (const auto& file : new_files) {
        // Construct absolute path
        std::string abs_path = (file[0] == '/') ? file : ("/" + file);
        std::string check_path = ROOT_PREFIX + abs_path;
        
        // If file exists
        if (access(check_path.c_str(), F_OK) == 0) {
            // Check if directory - if so, usually fine (shared)
            struct stat st;
            if (stat(check_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                continue;
            }

            // If it's owned by us, it's fine
            if (owned_files.count(abs_path)) {
                continue;
            }
            
            // Special case: Ignore /usr/share/info/dir as it's a shared directory index
            if (abs_path == "/usr/share/info/dir") {
                continue;
            }

            collisions.push_back(abs_path);
        }
    }

    if (collisions.empty()) return true;

    // 3. Identify owners of collisions
    bool fatal = false;
    auto installed_pkgs = get_installed_packages(".list");

    for (const auto& col : collisions) {
        bool found_owner = false;
        for (const auto& other_pkg : installed_pkgs) {
            if (other_pkg == pkg_name) continue;

            std::ifstream f(INFO_DIR + other_pkg + ".list");
            std::string line;
            while(std::getline(f, line)) {
                if (trim(line) == col) {
                    std::cerr << Color::RED << "E: File conflict! " << col << " is owned by " << other_pkg << Color::RESET << std::endl;
                    found_owner = true;
                    fatal = true;
                    break;
                }
            }
            if (found_owner) break;
        }
        if (!found_owner) {
             // File exists but not owned by any package.
             if (verbose) std::cerr << Color::YELLOW << "W: " << col << " exists but is not owned by any package. Overwriting." << Color::RESET << std::endl;
        }
    }

    return !fatal;
}

std::vector<std::string> get_repo_urls() {
    std::vector<std::string> urls;
    std::set<std::string> seen_urls;
    
    // Default repo from sys_info if sources.list missing? 
    // Let's assume we always have a sources.list
    std::ifstream f(SOURCES_LIST_PATH);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        std::string normalized = normalize_repo_base_url(line);
        if (!normalized.empty() && seen_urls.insert(normalized).second) {
            urls.push_back(normalized);
        }
    }

    // Also scan sources.list.d/
    DIR* dir = opendir(SOURCES_DIR.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strstr(entry->d_name, ".list")) {
                std::ifstream sf(SOURCES_DIR + entry->d_name);
                while (std::getline(sf, line)) {
                    line = trim(line);
                    if (line.empty() || line[0] == '#') continue;
                    std::string normalized = normalize_repo_base_url(line);
                    if (!normalized.empty() && seen_urls.insert(normalized).second) {
                        urls.push_back(normalized);
                    }
                }
            }
        }
        closedir(dir);
    }

    return urls;
}

bool ensure_repo_urls(const std::vector<std::string>& urls) {
    if (!urls.empty()) return true;
    std::cerr << Color::RED
              << "E: No repositories configured. Add one with 'gpkg add-repo <url>' "
              << "or create " << SOURCES_DIR << "*.list"
              << Color::RESET << std::endl;
    return false;
}

bool ensure_repo_index_available() {
    if (access((REPO_CACHE_PATH + "Packages.json").c_str(), F_OK) == 0) return true;
    std::cerr << Color::RED
              << "E: No package index available. Run 'gpkg update' first."
              << Color::RESET << std::endl;
    return false;
}

bool resolve_download_url(const PackageMetadata& meta, std::string& out_url) {
    if (!meta.source_url.empty()) {
        out_url = build_repo_package_url(meta.source_url, meta.filename);
        return true;
    }

    auto urls = get_repo_urls();
    if (!ensure_repo_urls(urls)) return false;
    out_url = build_repo_package_url(urls[0], meta.filename);
    return true;
}


bool get_repo_package_info(const std::string& pkg_name, PackageMetadata& out_meta) {
    bool found = false;
    foreach_json_object(REPO_CACHE_PATH + "Packages.json", [&](const std::string& obj) {
        std::string name;
        if (get_json_value(obj, "package", name) && trim(name) == pkg_name) {
            out_meta.name = trim(name);
            get_json_value(obj, "version", out_meta.version);
            get_json_value(obj, "description", out_meta.description);
            get_json_value(obj, "filename", out_meta.filename);
            get_json_value(obj, "sha512", out_meta.sha512);
            get_json_value(obj, "repo_url", out_meta.source_url);
            get_json_array(obj, "depends", out_meta.depends);
            get_json_array(obj, "conflicts", out_meta.conflicts);
            get_json_array(obj, "provides", out_meta.provides);
            found = true;
            return false; // Stop iteration
        }
        return true; // Continue
    });
    return found;
}

struct Dependency {
    std::string name;
    std::string op;
    std::string version;
};

Dependency parse_dependency(const std::string& dep_str) {
    Dependency dep;
    size_t open_paren = dep_str.find('(');
    
    if (open_paren == std::string::npos) {
        dep.name = trim(dep_str);
        return dep;
    }
    
    dep.name = trim(dep_str.substr(0, open_paren));
    
    size_t close_paren = dep_str.find(')', open_paren);
    if (close_paren == std::string::npos) return dep; // Malformed?
    
    std::string content = trim(dep_str.substr(open_paren + 1, close_paren - open_paren - 1));
    
    // Parse operator
    std::vector<std::string> ops = {">=", "<=", "<<", ">>", "==", "=", ">", "<"};
    for (const auto& op : ops) {
        if (content.substr(0, op.length()) == op) {
            dep.op = op;
            dep.version = trim(content.substr(op.length()));
            break;
        }
    }
    
    return dep;
}

bool version_satisfies(const std::string& current_ver, const std::string& op, const std::string& req_ver) {
    if (op.empty()) return true;
    
    int cmp = compare_versions(current_ver, req_ver);
    
    if (op == ">=" && cmp >= 0) return true;
    if (op == "<=" && cmp <= 0) return true;
    if (op == ">"  && cmp > 0) return true;
    if (op == "<"  && cmp < 0) return true;
    if (op == ">>" && cmp > 0) return true;
    if (op == "<<" && cmp < 0) return true;
    if (op == "="  && cmp == 0) return true;
    if (op == "==" && cmp == 0) return true;
    
    return false;
}

std::vector<std::string> load_system_provides() {
    std::vector<std::string> entries;
    std::ifstream f(SYSTEM_PROVIDES_PATH);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        entries.push_back(line);
    }
    return entries;
}

bool is_system_provided(const std::string& pkg, const std::string& op = "", const std::string& req_version = "") {
    for (const auto& entry : load_system_provides()) {
        Dependency dep = parse_dependency(entry);
        if (dep.name != pkg) continue;
        if (op.empty() || dep.version.empty() || version_satisfies(dep.version, op, req_version)) {
            return true;
        }
    }
    return false;
}

// Helper: Find a package that provides a capability (Scanning the entire repo index)
// Returns empty string if not found, or the name of the real package if found.
std::string find_provider(const std::string& capability, const std::string& op, const std::string& req_version, bool verbose) {
    std::string result;
    foreach_json_object(REPO_CACHE_PATH + "Packages.json", [&](const std::string& obj) {
        std::vector<std::string> provides;
        if (!get_json_array(obj, "provides", provides)) return true;
        
        for (const auto& p : provides) {
            Dependency prov_dep = parse_dependency(p);
            if (prov_dep.name != capability) continue;
            
            // Check version satisfaction
            bool satisfies = op.empty() || 
                (!prov_dep.version.empty() && version_satisfies(prov_dep.version, op, req_version));
            
            if (satisfies) {
                get_json_value(obj, "package", result);
                result = trim(result);
                VLOG(verbose, "Found provider for " << capability << (op.empty() ? "" : (" (" + op + " " + req_version + ")")) << ": " << result);
                return false; // Stop iteration
            }
        }
        return true; // Continue
    });
    return result;
}

// Recursive dependency resolver
bool resolve_dependencies(const std::string& pkg, const std::string& op, const std::string& req_version, std::vector<PackageMetadata>& install_queue, std::set<std::string>& visited, std::set<std::string>& installed_cache, bool verbose) {
    if (visited.count(pkg)) {
        // If it's already visited, it might be in the queue or currently being resolved (circular).
        // Check if it's in the queue and verify version
        for (const auto& m : install_queue) {
            if (m.name == pkg) {
                 if (!version_satisfies(m.version, op, req_version)) {
                     std::cerr << Color::RED << "E: Dependency conflict! " << pkg << " " << m.version << " is queued, but " << op << " " << req_version << " is required." << Color::RESET << std::endl;
                     return false;
                 }
                 return true;
            }
        }
        // If not in queue, it's being resolved up the stack. We assume it's fine (circular dep).
        return true;
    }

    if (verbose) VLOG(verbose, "Resolving dependencies for: " << pkg << (op.empty() ? "" : (" (" + op + " " + req_version + ")")));

    if (is_system_provided(pkg, op, req_version)) {
        VLOG(verbose, pkg << " is satisfied by " << SYSTEM_PROVIDES_PATH);
        return true;
    }

    // Check if already installed (directly or via smart detection)
    std::string installed_ver;
    if (is_installed(pkg, &installed_ver)) {
         if (version_satisfies(installed_ver, op, req_version)) {
             VLOG(verbose, pkg << " " << installed_ver << " is installed and satisfies constraints.");
             return true;
         } else {
             std::cerr << Color::YELLOW << "W: " << pkg << " " << installed_ver << " is installed but does not meet requirements (" << op << " " << req_version << ")." << Color::RESET << std::endl;
         }
    }

    // Check PROVIDES of installed packages
    for (const auto& p_name : get_installed_packages()) {
        std::ifstream f(INFO_DIR + p_name + ".json");
        if (!f) continue;
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<std::string> p_provides;
        if (get_json_array(content, "provides", p_provides)) {
            for (const auto& prov : p_provides) {
                std::string prov_name = prov;
                size_t space = prov.find(' ');
                if (space != std::string::npos) prov_name = prov.substr(0, space);
                
                if (prov_name == pkg) {
                    VLOG(verbose, pkg << " is provided by installed package " << p_name);
                    return true;
                }
            }
        }
    }
    
    PackageMetadata meta;
    bool found_exact = get_repo_package_info(pkg, meta);
    
    // If exact match not found, try to find a provider
    if (!found_exact) {
        VLOG(verbose, "Exact match for " << pkg << " not found. Searching for providers...");
        std::string provider = find_provider(pkg, op, req_version, verbose);
        if (!provider.empty()) {
            VLOG(verbose, "Redirecting " << pkg << " -> " << provider);
            // Recursively resolve the discovered provider for the virtual capability
            return resolve_dependencies(provider, "", "", install_queue, visited, installed_cache, verbose);
        }
        
        std::cerr << Color::RED << "E: Unable to locate package " << pkg << Color::RESET << std::endl;
        return false;
    }
    
    if (!version_satisfies(meta.version, op, req_version)) {
        std::cerr << Color::RED << "E: Package " << pkg << " found (v" << meta.version << ") but does not meet requirements (" << op << " " << req_version << ")" << Color::RESET << std::endl;
        return false;
    }
    
    VLOG(verbose, "Found " << pkg << " in repository (version: " << meta.version << ")");
    if (verbose && !meta.depends.empty()) {
        std::stringstream ss;
        for (size_t i = 0; i < meta.depends.size(); ++i) {
            ss << meta.depends[i] << (i == meta.depends.size() - 1 ? "" : ", ");
        }
        VLOG(verbose, pkg << " depends on: " << ss.str());
    }

    visited.insert(pkg);

    for (const auto& dep_str : meta.depends) {
        Dependency dep = parse_dependency(dep_str);
        if (!resolve_dependencies(dep.name, dep.op, dep.version, install_queue, visited, installed_cache, verbose)) {
            return false;
        }
    }
    
    if (verbose) std::cout << "[DEBUG] Adding " << pkg << " to installation queue." << std::endl;
    install_queue.push_back(meta);
    return true;
}

bool check_conflicts(const std::vector<PackageMetadata>& queue, const std::set<std::string>& installed, bool verbose) {
    bool has_conflict = false;
    for (const auto& pkg : queue) {
        for (const auto& conflict : pkg.conflicts) {
            // Check against installed packages
            if (installed.count(conflict)) {
                std::cerr << Color::RED << "E: Conflict detected! " << pkg.name << " conflicts with installed package " << conflict << Color::RESET << std::endl;
                has_conflict = true;
            }
            // Check against other packages in the queue
            for (const auto& other : queue) {
                if (other.name == conflict) {
                     std::cerr << Color::RED << "E: Conflict detected in transaction! " << pkg.name << " conflicts with " << other.name << Color::RESET << std::endl;
                     has_conflict = true;
                }
            }
        }
    }
    return !has_conflict;
}


bool verify_hash(const std::string& file, const std::string& expected_hash, const std::string& label = "") {
    std::string subject = label.empty() ? file : label;
    std::cout << "Verifying " << subject << "..." << std::endl;
    
    std::ifstream f(file, std::ios::binary);
    if (!f) {
        std::cerr << "E: Could not open " << subject << " for verification: " << file << std::endl;
        return false;
    }

    SHA512_CTX sha512;
    SHA512_Init(&sha512);
    char buffer[32768];
    while (f.read(buffer, sizeof(buffer)) || f.gcount() > 0) {
        SHA512_Update(&sha512, buffer, f.gcount());
    }

    unsigned char hash[SHA512_DIGEST_LENGTH];
    SHA512_Final(hash, &sha512);

    std::stringstream ss;
    for (int i = 0; i < SHA512_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << (int)hash[i];
    }
    
    std::string calculated = ss.str();
    if (calculated != expected_hash) {
        std::cerr << "E: Hash mismatch for " << subject << "!" << std::endl;
        std::cerr << "   Expected:   " << expected_hash << std::endl;
        std::cerr << "   Calculated: " << calculated << std::endl;
        return false;
    }
    
    return true;
}

std::string get_cached_package_path(const PackageMetadata& meta) {
    return REPO_CACHE_PATH + meta.name + EXTENSION;
}

bool fetch_package_archive(const PackageMetadata& meta, size_t index, size_t total, bool verbose, bool* reused_out = nullptr) {
    if (reused_out) *reused_out = false;

    if (!mkdir_p(REPO_CACHE_PATH)) {
        std::cerr << Color::RED << "E: Failed to create cache directory " << REPO_CACHE_PATH << Color::RESET << std::endl;
        return false;
    }

    std::string local_path = get_cached_package_path(meta);
    std::string verify_label = "package archive " + meta.name;

    if (access(local_path.c_str(), F_OK) == 0) {
        std::cout << "Using cached (" << index << "/" << total << ") " << meta.name << "..." << std::endl;
        if (verify_hash(local_path, meta.sha512, "cached " + verify_label)) {
            if (reused_out) *reused_out = true;
            return true;
        }

        std::cerr << Color::YELLOW << "W: Cached archive for " << meta.name
                  << " is invalid. Removing it and downloading a fresh copy." << Color::RESET << std::endl;
        remove(local_path.c_str());
    }

    std::string url;
    if (!resolve_download_url(meta, url)) return false;

    const int max_fetch_attempts = 2;
    for (int attempt = 1; attempt <= max_fetch_attempts; ++attempt) {
        std::cout << "Downloading (" << index << "/" << total << ") " << meta.name;
        if (attempt > 1) std::cout << " [retry " << attempt << "/" << max_fetch_attempts << "]";
        std::cout << "..." << std::endl;

        std::string download_error;
        if (!DownloadFile(url, local_path, verbose, &download_error)) {
            remove(local_path.c_str());
            std::cerr << Color::YELLOW << "W: Failed to download " << meta.name << " from " << url;
            if (!download_error.empty()) std::cerr << " (" << download_error << ")";
            std::cerr << Color::RESET << std::endl;
            continue;
        }

        if (verify_hash(local_path, meta.sha512, verify_label)) {
            return true;
        }

        std::cerr << Color::YELLOW << "W: Downloaded archive for " << meta.name
                  << " failed integrity verification. Re-downloading." << Color::RESET << std::endl;
        remove(local_path.c_str());
    }

    std::cerr << Color::RED << "E: Failed to fetch a valid archive for " << meta.name
              << " from " << url << Color::RESET << std::endl;
    return false;
}

int handle_list_repos() {
    auto urls = get_repo_urls();
    if (urls.empty()) {
        std::cout << "No repositories configured." << std::endl;
        return 0;
    }

    std::cout << "Configured repositories:" << std::endl;
    for (size_t i = 0; i < urls.size(); ++i) {
        std::cout << "  " << (i + 1) << ". " << normalize_repo_base_url(urls[i]) << std::endl;
    }
    return 0;
}

bool save_package_metadata(const std::string& pkg_name, const std::string& tmp_path, const std::string& tar_path, bool strip_data, bool verbose) {
    if (verbose) std::cout << "[DEBUG] Saving metadata for " << pkg_name << " to " << INFO_DIR << std::endl;
    run_command("mkdir -p " + INFO_DIR, verbose);
    
    // 1. Copy control file
    if (verbose) std::cout << "[DEBUG] Copying control.json to " << INFO_DIR << pkg_name << ".json" << std::endl;
    run_command("cp " + tmp_path + "control.json " + INFO_DIR + pkg_name + ".json", verbose);

    // 2. Copy scripts
    std::vector<std::string> scripts = {"preinst", "postinst", "prerm", "postrm"};
    for(const auto& script : scripts) {
        std::string src = tmp_path + "scripts/" + script;
        if(access(src.c_str(), F_OK) == 0) {
            if (verbose) std::cout << "[DEBUG] Copying script " << script << " to " << INFO_DIR << pkg_name << "." << script << std::endl;
            run_command("cp " + src + " " + INFO_DIR + pkg_name + "." + script, verbose);
        }
    }

    // 3. Generate file list
    if (verbose) std::cout << "[DEBUG] Generating file list from " << tar_path << std::endl;
    
    std::vector<std::string> file_list = get_tar_file_list(tar_path, strip_data);
    std::stringstream ss_out;

    for (const auto& line : file_list) {
        std::string p = trim(line);
        // Remove leading ./ or /
        while (p.size() >= 2 && p.substr(0, 2) == "./") p = p.substr(2);
        while (!p.empty() && p[0] == '/') p = p.substr(1);
        // Remove trailing /
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        
        if (p.empty() || p == ".") continue;

        // Prepend / for the .list file to represent absolute paths in the system
        ss_out << "/" << p << "\n";
    }
    
    std::ofstream list_file(INFO_DIR + pkg_name + ".list");
    if (list_file) {
        list_file << ss_out.str();
        list_file.close();
        if (verbose) std::cout << "[DEBUG] File list saved to " << INFO_DIR << pkg_name << ".list" << std::endl;
    } else {
        std::cerr << "E: Failed to write file list to " << INFO_DIR << pkg_name << ".list" << std::endl;
    }
    
    if (verbose) std::cout << "[DEBUG] Analyzing " << file_list.size() << " files for triggers." << std::endl;
    check_triggers(file_list);

    return true;
}

// Helper to run package scripts (preinst, postinst, etc)
bool run_package_script(const std::string& path, const std::string& name, bool verbose) {
    if (access(path.c_str(), X_OK) == 0) {
        VLOG(verbose, "Running " << name << " script: " << path);
        if (run_command(path, verbose) != 0) {
            std::cerr << "E: " << name << " script failed." << std::endl;
            return false;
        }
    }
    return true;
}

std::string find_installed_provider(const std::string& capability);

std::string find_installed_provider(const std::string& capability) {
    for (const auto& p_name : get_installed_packages()) {
        std::ifstream f(INFO_DIR + p_name + ".json");
        if (!f) continue;
        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<std::string> p_provides;
        if (get_json_array(content, "provides", p_provides)) {
            for (const auto& prov : p_provides) {
                Dependency d = parse_dependency(prov);
                if (d.name == capability) return p_name;
            }
        }
    }
    return "";
}

bool install_package_from_file(const std::string& pkg_file, bool verbose) {
    std::string cmd = "gpkg-worker --install " + pkg_file;
    if (verbose) cmd += " --verbose";
    
    // Pass root prefix if set (dev mode)
    if (!ROOT_PREFIX.empty()) cmd += " --root " + ROOT_PREFIX;

    int ret = run_command(cmd, verbose);
    return (ret == 0);
}

bool install_package_v2(const std::string& pkg_name, bool verbose) {
    PackageMetadata meta;
    meta.name = pkg_name;
    std::string pkg_file = get_cached_package_path(meta);
    return install_package_from_file(pkg_file, verbose);
}

void print_help() {
    std::cout << "Usage: gpkg <command> [args] [--verbose]\n"
              << "GeminiOS Package Manager (v2.1 - Genesis)\n\n"
              << "Options:\n"
              << "  -v, --verbose   Show detailed logging information\n\n"
              << "Commands:\n"
              << "  install <pkg>   Download and install a package\n"
              << "  remove <pkg>    Remove an installed package (--purge to remove unneeded deps)\n"
              << "  upgrade         Upgrade all installed packages\n"
              << "  update          Update local package indices\n"
              << "  search <query>  Search for packages\n"
              << "  show <pkg>      Show package metadata and source repository\n"
              << "  add-repo <url>  Add a third-party repository\n"
              << "  list-repos      Show configured repositories\n"
              << "  clean           Clear package cache\n";
}

// --- Command Handlers ---

int handle_update(bool verbose) {
    auto urls = get_repo_urls();
    if (!ensure_repo_urls(urls)) return 1;
    VLOG(verbose, "Found " << urls.size() << " repository URLs.");
    std::cout << Color::BLUE << "Updating package indices..." << Color::RESET << std::endl;
    run_command("mkdir -p " + REPO_CACHE_PATH, verbose);

    std::string merged_tmp = REPO_CACHE_PATH + "Packages.json.tmp";
    std::ofstream merged(merged_tmp);
    if (!merged) {
        std::cerr << Color::RED << "E: Failed to open merged index file for writing." << Color::RESET << std::endl;
        return 1;
    }

    merged << "[\n";
    bool first_object = true;
    int success_count = 0;
    int total_packages = 0;

    for (size_t i = 0; i < urls.size(); ++i) {
        const auto& url = urls[i];
        std::string full_url = build_repo_index_url(url);
        std::string dest_zst = REPO_CACHE_PATH + "repo_index_" + std::to_string(i) + ".json.zst";
        std::string dest_json = REPO_CACHE_PATH + "repo_index_" + std::to_string(i) + ".json";

        VLOG(verbose, "Fetching index from: " << full_url);
        std::cout << "Get: " << full_url << std::endl;

        std::string download_error;
        if (!DownloadFile(full_url, dest_zst, verbose, &download_error)) {
            std::cerr << Color::YELLOW << "W: Failed to fetch index from " << url;
            if (!download_error.empty()) std::cerr << " (" << download_error << ")";
            std::cerr << Color::RESET << std::endl;
            continue;
        }

        if (run_command("zstd -df " + dest_zst + " -o " + dest_json, verbose) != 0) {
            std::cerr << Color::YELLOW << "W: Failed to decompress index from " << url << Color::RESET << std::endl;
            remove(dest_zst.c_str());
            continue;
        }

        int repo_package_count = 0;
        foreach_json_object(dest_json, [&](const std::string& obj) {
            if (!first_object) merged << ",\n";
            merged << inject_repo_url(obj, url);
            first_object = false;
            repo_package_count++;
            total_packages++;
            return true;
        });

        remove(dest_zst.c_str());
        remove(dest_json.c_str());

        success_count++;
        std::cout << Color::GREEN << "✓ Updated index from " << url
                  << " (" << repo_package_count << " packages)" << Color::RESET << std::endl;
    }

    merged << "\n]\n";
    merged.close();

    if (success_count == 0) {
        remove(merged_tmp.c_str());
        std::cerr << Color::RED << "E: Failed to update any package indices." << Color::RESET << std::endl;
        return 1;
    }

    if (rename(merged_tmp.c_str(), (REPO_CACHE_PATH + "Packages.json").c_str()) != 0) {
        std::cerr << Color::RED << "E: Failed to replace merged package index." << Color::RESET << std::endl;
        return 1;
    }

    std::cout << Color::GREEN << "✓ Merged " << total_packages << " packages from "
              << success_count << " repositories." << Color::RESET << std::endl;
    return 0;
}

int handle_upgrade(const std::set<std::string>& installed_cache, bool verbose) {
    if (!ensure_repo_index_available()) return 1;
    std::cout << "Reading package lists..." << std::endl;
    VLOG(verbose, "Checking " << installed_cache.size() << " installed packages for updates.");
    
    std::vector<PackageMetadata> updates;
    for (const auto& pkg : installed_cache) {
        std::string current_ver;
        if (is_installed(pkg, &current_ver)) {
            PackageMetadata repo_meta;
            if (get_repo_package_info(pkg, repo_meta)) {
                if (compare_versions(repo_meta.version, current_ver) > 0) {
                    VLOG(verbose, "Update found for " << pkg << ": " << current_ver << " -> " << repo_meta.version);
                    updates.push_back(repo_meta);
                }
            }
        }
    }
    
    if (updates.empty()) {
        std::cout << "All packages are up to date." << std::endl;
        return 0;
    }
    
    std::cout << "The following packages will be upgraded:" << std::endl;
    for (const auto& u : updates) std::cout << "  " << Color::GREEN << u.name << Color::RESET << " (" << u.version << ")" << std::endl;
    if (!ask_confirmation("Do you want to continue?")) return 0;
    
    size_t upgraded_count = 0;
    size_t reused_count = 0;
    size_t downloaded_count = 0;
    std::vector<std::string> failures;

    for (size_t i = 0; i < updates.size(); ++i) {
        const auto& meta = updates[i];
        bool reused = false;
        if (!fetch_package_archive(meta, i + 1, updates.size(), verbose, &reused)) {
            failures.push_back(meta.name);
            continue;
        }

        if (reused) reused_count++;
        else downloaded_count++;

        std::cout << "Upgrading (" << (i + 1) << "/" << updates.size() << ") " << meta.name << "..." << std::endl;
        if (!install_package_v2(meta.name, verbose)) {
            std::cerr << Color::RED << "E: Failed to upgrade " << meta.name << Color::RESET << std::endl;
            failures.push_back(meta.name);
            continue;
        }

        upgraded_count++;
    }

    std::cout << Color::CYAN << "Upgrade summary: "
              << upgraded_count << " upgraded, "
              << downloaded_count << " downloaded, "
              << reused_count << " reused from cache." << Color::RESET << std::endl;

    if (!failures.empty()) {
        std::cerr << Color::RED << "E: Upgrade completed with failures: "
                  << join_strings(failures) << Color::RESET << std::endl;
        return 1;
    }

    return 0;
}

int handle_install(int argc, char* argv[], const std::set<std::string>& installed_cache, bool verbose) {
    std::vector<PackageMetadata> install_queue;
    std::vector<std::string> local_files;
    std::set<std::string> visited;
    bool needs_repo_index = false;
    
    std::cout << "Resolving dependencies..." << std::endl;
    for (int i = 2; i < argc; ++i) {
        std::string arg = trim(argv[i]);
        if (arg == "-v" || arg == "--verbose") continue;
        
        if (arg.length() > 5 && arg.substr(arg.length() - 5) == ".gpkg" && access(arg.c_str(), F_OK) == 0) {
            local_files.push_back(arg);
        } else {
            needs_repo_index = true;
        }
    }

    if (needs_repo_index && !ensure_repo_index_available()) {
        return 1;
    }

    for (int i = 2; i < argc; ++i) {
        std::string arg = trim(argv[i]);
        if (arg == "-v" || arg == "--verbose") continue;
        if (arg.length() > 5 && arg.substr(arg.length() - 5) == ".gpkg" && access(arg.c_str(), F_OK) == 0) {
            continue;
        }
        if (!resolve_dependencies(arg, "", "", install_queue, visited, (std::set<std::string>&)installed_cache, verbose)) {
            std::cerr << Color::RED << "E: Failed to resolve dependencies for " << arg << Color::RESET << std::endl;
            return 1;
        }
    }
    
    for (const auto& lfile : local_files) {
        std::cout << "Installing local package: " << lfile << std::endl;
        if (!install_package_from_file(lfile, verbose)) return 1;
    }
    
    if (install_queue.empty()) {
        if (local_files.empty()) std::cout << "Nothing to do." << std::endl;
        return 0;
    }
    
    std::cout << "The following NEW packages will be installed:" << std::endl;
    for (const auto& m : install_queue) std::cout << "  " << Color::GREEN << m.name << Color::RESET << " (" << m.version << ")" << std::endl;
    if (!check_conflicts(install_queue, installed_cache, verbose)) return 1;
    if (!ask_confirmation("Do you want to continue?")) return 0;

    std::cout << Color::CYAN << "[*] Downloading packages..." << Color::RESET << std::endl;
    size_t reused_count = 0;
    size_t downloaded_count = 0;
    for (size_t i = 0; i < install_queue.size(); ++i) {
        const auto& meta = install_queue[i];
        bool reused = false;
        if (!fetch_package_archive(meta, i + 1, install_queue.size(), verbose, &reused)) {
            std::cerr << Color::RED << "E: Aborting install because " << meta.name
                      << " could not be fetched safely." << Color::RESET << std::endl;
            return 1;
        }
        if (reused) reused_count++;
        else downloaded_count++;
    }
    std::cout << Color::CYAN << "[*] Download summary: " << downloaded_count
              << " downloaded, " << reused_count << " reused from cache."
              << Color::RESET << std::endl;

    std::cout << Color::CYAN << "[*] Installing packages..." << Color::RESET << std::endl;
    size_t installed_count = 0;
    for (size_t i = 0; i < install_queue.size(); ++i) {
        std::cout << "Installing (" << (i+1) << "/" << install_queue.size() << ") " << install_queue[i].name << "..." << std::endl;
        if (!install_package_v2(install_queue[i].name, verbose)) {
            std::cerr << Color::RED << "E: Installation stopped at " << install_queue[i].name
                      << ". " << installed_count << " package(s) were installed before the failure."
                      << Color::RESET << std::endl;
            return 1;
        }
        installed_count++;
    }
    std::cout << Color::GREEN << "✓ Installed " << installed_count << " package(s)." << Color::RESET << std::endl;
    return 0;
}

// Get metadata of an installed package
bool get_installed_package_metadata(const std::string& pkg_name, PackageMetadata& out_meta) {
    std::string info_path = INFO_DIR + pkg_name + ".json";
    std::ifstream f(info_path);
    if (!f) return false;
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    
    out_meta.name = pkg_name;
    get_json_value(content, "version", out_meta.version);
    get_json_value(content, "description", out_meta.description);
    get_json_array(content, "depends", out_meta.depends);
    get_json_array(content, "conflicts", out_meta.conflicts);
    get_json_array(content, "provides", out_meta.provides);
    return true;
}

// Check if any installed package (other than the ones in 'excluding') depends on 'pkg'
bool is_required_by_others(const std::string& pkg, const std::set<std::string>& excluding, bool verbose) {
    auto all_installed = get_installed_packages();
    for (const auto& other : all_installed) {
        if (excluding.count(other)) continue;
        
        PackageMetadata meta;
        if (get_installed_package_metadata(other, meta)) {
            for (const auto& dep_str : meta.depends) {
                Dependency dep = parse_dependency(dep_str);
                if (dep.name == pkg) {
                    VLOG(verbose, pkg << " is still required by " << other);
                    return true;
                }
                
                // Also check if 'pkg' provides what 'other' depends on
                PackageMetadata pkg_meta;
                if (get_installed_package_metadata(pkg, pkg_meta)) {
                    for (const auto& prov_str : pkg_meta.provides) {
                        Dependency prov = parse_dependency(prov_str);
                        if (prov.name == dep.name) {
                            VLOG(verbose, pkg << " provides " << prov.name << " which is required by " << other);
                            return true;
                        }
                    }
                }
            }
        }
    }
    return false;
}

int handle_remove(int argc, char* argv[], bool verbose, bool purge) {
    if (argc < 3) {
        std::cerr << "Usage: gpkg remove <package_name> [--purge]" << std::endl;
        return 1;
    }
    
    std::string target_pkg = "";
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg != "--purge" && arg != "-v" && arg != "--verbose") {
            target_pkg = arg;
            break;
        }
    }

    if (target_pkg.empty()) {
        std::cerr << "E: No package specified for removal." << std::endl;
        return 1;
    }

    if (!is_installed(target_pkg)) {
        std::cerr << Color::RED << "E: Package '" << target_pkg << "' is not installed." << Color::RESET << std::endl;
        return 1;
    }

    std::vector<std::string> to_remove;
    to_remove.push_back(target_pkg);
    std::set<std::string> removal_set;
    removal_set.insert(target_pkg);

    if (purge) {
        std::cout << "Calculating dependencies for purge..." << std::endl;
        bool changed = true;
        while (changed) {
            changed = false;
            std::vector<std::string> current_removals = to_remove;
            for (const auto& pkg : current_removals) {
                PackageMetadata meta;
                if (get_installed_package_metadata(pkg, meta)) {
                    for (const auto& dep_str : meta.depends) {
                        Dependency dep = parse_dependency(dep_str);
                        // Check if dep is installed and not already in removal list
                        if (is_installed(dep.name) && !removal_set.count(dep.name)) {
                            // Check if it's required by anyone NOT in the removal list
                            if (!is_required_by_others(dep.name, removal_set, verbose)) {
                                to_remove.push_back(dep.name);
                                removal_set.insert(dep.name);
                                changed = true;
                            }
                        }
                    }
                }
            }
        }
    }

    std::cout << "The following packages will be REMOVED:" << std::endl;
    for (const auto& p : to_remove) {
        std::cout << "  " << Color::RED << p << Color::RESET << std::endl;
    }

    if (!ask_confirmation("Do you want to continue?")) {
        std::cout << "Abort." << std::endl;
        return 0;
    }

    for (const auto& pkg : to_remove) {
        std::string cmd = "gpkg-worker --remove " + pkg;
        if (verbose) cmd += " --verbose";
        if (!ROOT_PREFIX.empty()) cmd += " --root " + ROOT_PREFIX;
        
        if (run_command(cmd, verbose) != 0) {
            std::cerr << Color::RED << "E: Failed to remove " << pkg << Color::RESET << std::endl;
            return 1;
        }
    }
    
    return 0;
}

int handle_search(const std::string& query, bool verbose) {
    if (!ensure_repo_index_available()) return 1;
    VLOG(verbose, "Searching for '" << query << "' in " << REPO_CACHE_PATH << "Packages.json");
    std::map<std::string, PackageMetadata> matches;
    foreach_json_object(REPO_CACHE_PATH + "Packages.json", [&](const std::string& obj) {
        PackageMetadata meta;
        get_json_value(obj, "package", meta.name);
        get_json_value(obj, "description", meta.description);
        get_json_value(obj, "version", meta.version);
        get_json_value(obj, "repo_url", meta.source_url);
        
        if (meta.name.find(query) != std::string::npos || meta.description.find(query) != std::string::npos) {
            auto it = matches.find(meta.name);
            if (it == matches.end() || compare_versions(meta.version, it->second.version) > 0) {
                matches[meta.name] = meta;
            }
        }
        return true;
    });

    if (matches.empty()) {
        std::cout << "No matches found for '" << query << "'" << std::endl;
        return 0;
    }

    for (const auto& entry : matches) {
        const auto& meta = entry.second;
        std::string installed_ver;
        std::string status_str = "";
        std::string repo_str = meta.source_url.empty() ? "" : (Color::CYAN + " [repo: " + meta.source_url + "]" + Color::RESET);
        
        if (is_installed(meta.name, &installed_ver)) {
            if (compare_versions(installed_ver, meta.version) == 0) {
                 status_str = Color::BLUE + " [installed]" + Color::RESET;
            } else {
                 status_str = Color::BLUE + " [installed: " + installed_ver + "]" + Color::RESET;
            }
        }

        std::cout << Color::GREEN << meta.name << Color::RESET
                  << " (" << meta.version << ")"
                  << status_str
                  << repo_str
                  << " - "
                  << description_summary(meta.description)
                  << std::endl;
    }
    return 0;
}

int handle_show(const std::string& pkg_name, bool verbose) {
    if (!ensure_repo_index_available()) return 1;
    VLOG(verbose, "Showing package metadata for '" << pkg_name << "'");
    PackageMetadata meta;
    if (!get_repo_package_info(pkg_name, meta)) {
        std::cerr << Color::RED << "E: Package '" << pkg_name << "' was not found in the local index. Run 'gpkg update' first." << Color::RESET << std::endl;
        return 1;
    }

    std::cout << Color::GREEN << meta.name << Color::RESET << std::endl;
    std::cout << "  Version:     " << meta.version << std::endl;
    std::cout << "  Repository:  " << (meta.source_url.empty() ? "(unknown)" : meta.source_url) << std::endl;
    std::cout << "  Filename:    " << meta.filename << std::endl;
    if (!meta.description.empty()) print_description_block("Description", meta.description);
    if (!meta.depends.empty()) print_wrapped_block("  Depends:     ", join_strings(meta.depends));
    if (!meta.conflicts.empty()) print_wrapped_block("  Conflicts:   ", join_strings(meta.conflicts));
    if (!meta.provides.empty()) print_wrapped_block("  Provides:    ", join_strings(meta.provides));

    std::string installed_ver;
    if (is_installed(meta.name, &installed_ver)) {
        std::cout << "  Installed:   yes (" << installed_ver << ")" << std::endl;
    } else {
        std::cout << "  Installed:   no" << std::endl;
    }
    return 0;
}

int handle_add_repo(const std::string& url, bool verbose) {
    std::string normalized = normalize_repo_base_url(url);
    if (normalized.find("http://") != 0 && normalized.find("https://") != 0) {
        std::cerr << "E: Invalid repository URL. Must start with http:// or https://" << std::endl;
        return 1;
    }

    for (const auto& existing : get_repo_urls()) {
        if (normalize_repo_base_url(existing) == normalized) {
            std::cout << Color::YELLOW << "W: Repository already configured: " << normalized << Color::RESET << std::endl;
            return 0;
        }
    }

    std::string check_url = build_repo_index_url(normalized);
    std::cout << "Validating repository " << normalized << "..." << std::endl;
    std::string tmp_index = "/tmp/gpkg_validation_index.zst";
    std::string download_error;
    if (DownloadFile(check_url, tmp_index, verbose, &download_error)) {
        run_command("zstd -df " + tmp_index + " -o /tmp/gpkg_validation.json", verbose);
        std::ifstream f_check("/tmp/gpkg_validation.json");
        std::string content((std::istreambuf_iterator<char>(f_check)), std::istreambuf_iterator<char>());
        if (content.find("\"package\":") == std::string::npos) {
            std::cerr << Color::RED << "E: Invalid repository index." << Color::RESET << std::endl;
            return 1;
        }
        std::cout << Color::GREEN << "✓ Repository validated." << Color::RESET << std::endl;
        std::string name = "repo_" + std::to_string(time(NULL)) + ".list";
        run_command("mkdir -p " + SOURCES_DIR, verbose);
        std::ofstream f(SOURCES_DIR + name);
        if (f) f << normalized << std::endl;
        else std::cerr << "E: Failed to write to " << SOURCES_DIR << name << std::endl;
    } else {
        std::cerr << Color::RED << "E: Validation failed.";
        if (!download_error.empty()) std::cerr << " " << download_error;
        std::cerr << Color::RESET << std::endl;
        return 1;
    }
    return 0;
}

int handle_clean(bool verbose) {
    std::cout << "Cleaning package cache..." << std::endl;
    run_command("rm -f " + REPO_CACHE_PATH + "*" + EXTENSION, verbose);
    run_command("rm -f " + REPO_CACHE_PATH + "Packages.json", verbose);
    run_command("rm -f " + REPO_CACHE_PATH + "*.zst", verbose);
    return 0;
}

int main(int argc, char* argv[]) {
    // Register signal handlers
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    if (argc < 2) {
        print_help();
        return 1;
    }

    std::string action = argv[1];
    bool verbose = false;
    bool purge = false;
    for (int i=1; i<argc; i++) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose") verbose = true;
        if (arg == "--purge") purge = true;
    }

#ifndef DEV_MODE
    if (geteuid() != 0 && (action == "install" || action == "remove" || action == "update" || action == "add-repo" || action == "clean" || action == "upgrade")) {
        std::cerr << Color::RED << "E: This command requires root privileges." << Color::RESET << std::endl;
        return 1;
    }
#endif

    bool needs_trans = (action == "install" || action == "remove" || action == "upgrade" || action == "update" || action == "add-repo" || action == "clean");
    TransactionGuard guard(needs_trans, verbose);

    // Common setup
    std::set<std::string> installed_cache; 
    for (const auto& pkg : get_installed_packages()) {
        installed_cache.insert(pkg);
    }

    if (action == "update") return handle_update(verbose);
    if (action == "upgrade") return handle_upgrade(installed_cache, verbose);
    if (action == "install" && argc > 2) return handle_install(argc, argv, installed_cache, verbose);
    if (action == "remove" && argc > 2) return handle_remove(argc, argv, verbose, purge);
    if (action == "search" && argc > 2) return handle_search(argv[2], verbose);
    if (action == "show" && argc > 2) return handle_show(argv[2], verbose);
    if (action == "clean") return handle_clean(verbose);
    if (action == "add-repo" && argc > 2) return handle_add_repo(argv[2], verbose);
    if (action == "list-repos") return handle_list_repos();

    print_help();
    return 0;
}
