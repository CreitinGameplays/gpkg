// Generic helpers, JSON parsing, installed-package metadata, and version handling.

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
            if (!output.empty() && !previous_was_space) output += ' ';
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
            if (line.size() > max_len) return line.substr(0, max_len - 3) + "...";
            return line;
        }
    }

    std::string condensed = normalize_whitespace(description);
    if (condensed.size() > max_len) return condensed.substr(0, max_len - 3) + "...";
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
            ++line_length;
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
    return normalize_repo_base_url(base_url) + "/" + std::string(OS_ARCH) + "/Packages.json.zst";
}

std::string build_repo_package_url(const std::string& base_url, const std::string& filename) {
    return normalize_repo_base_url(base_url) + "/" + std::string(OS_ARCH) + "/" + filename;
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

template <typename Func>
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

std::vector<std::string> get_installed_packages(const std::string& extension = ".json") {
    std::vector<std::string> pkgs;
    DIR* d = opendir(INFO_DIR.c_str());
    if (!d) return pkgs;

    struct dirent* dir;
    while ((dir = readdir(d)) != nullptr) {
        std::string fname = dir->d_name;
        if (fname.size() > extension.size() &&
            fname.substr(fname.size() - extension.size()) == extension) {
            pkgs.push_back(fname.substr(0, fname.size() - extension.size()));
        }
    }
    closedir(d);
    return pkgs;
}

bool get_json_value(const std::string& obj, const std::string& key, std::string& out_val) {
    size_t key_pos = obj.find("\"" + key + "\"");
    if (key_pos == std::string::npos) return false;

    size_t colon = obj.find(':', key_pos);
    if (colon == std::string::npos) return false;

    size_t v_start = obj.find('"', colon);
    if (v_start == std::string::npos) return false;

    size_t v_end = obj.find('"', v_start + 1);
    while (v_end != std::string::npos && obj[v_end - 1] == '\\') {
        v_end = obj.find('"', v_end + 1);
    }

    if (v_end == std::string::npos) return false;
    out_val = json_unescape(obj.substr(v_start + 1, v_end - v_start - 1));
    return true;
}

bool get_json_array(const std::string& obj, const std::string& key, std::vector<std::string>& out_arr) {
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

        out_arr.push_back(json_unescape(obj.substr(value_start + 1, value_end - value_start - 1)));
        pos = value_end + 1;
    }

    return true;
}

bool ask_confirmation(const std::string& query) {
    std::cout << Color::YELLOW << query << " [Y/n] " << Color::RESET;
    std::string response;
    std::getline(std::cin, response);
    return response.empty() || response == "y" || response == "Y" || response == "yes";
}

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

bool is_installed(const std::string& pkg, std::string* out_version = nullptr) {
    std::string info_path = INFO_DIR + pkg + ".json";
    if (access(info_path.c_str(), F_OK) != 0) return false;

    if (!out_version) return true;

    std::ifstream f(info_path);
    if (!f) return false;

    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return get_json_value(content, "version", *out_version);
}

int run_command(const std::string& cmd, bool verbose) {
    if (verbose) std::cout << "[DEBUG] Executing: " << cmd << std::endl;
    return system(cmd.c_str());
}

std::string shell_quote(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

CommandCaptureResult run_command_captured(const std::string& cmd, bool verbose, const std::string& log_prefix) {
    if (verbose) {
        return {run_command(cmd, true), ""};
    }

    std::string prefix = "/tmp/" + log_prefix + "-XXXXXX.log";
    std::vector<char> tmpl(prefix.begin(), prefix.end());
    tmpl.push_back('\0');

    int fd = mkstemps(tmpl.data(), 4);
    if (fd < 0) {
        return {run_command(cmd, false), ""};
    }
    close(fd);

    std::string log_path(tmpl.data());
    std::string wrapped = cmd + " >" + shell_quote(log_path) + " 2>&1";
    return {run_command(wrapped, false), log_path};
}
