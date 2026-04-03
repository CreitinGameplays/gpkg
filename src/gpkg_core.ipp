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
    std::string normalized = normalize_repo_base_url(repo_url);
    return obj.substr(0, end)
        + ",\"repo_url\":\"" + json_escape(normalized) + "\""
        + ",\"source_url\":\"" + json_escape(normalized) + "\""
        + ",\"source_kind\":\"gpkg_repo\"}";
}

template <typename Func>
void foreach_json_object(const std::string& filepath, Func callback) {
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

std::vector<std::string> get_installed_packages(const std::string& extension = ".json") {
    std::vector<std::string> pkgs;
    DIR* d = opendir(INFO_DIR.c_str());
    if (!d) return pkgs;

    struct dirent* dir;
    while ((dir = readdir(d)) != nullptr) {
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

std::vector<PackageStatusRecord> load_status_records_from_file(const std::string& path) {
    std::vector<PackageStatusRecord> records;
    std::ifstream f(path);
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

std::vector<PackageStatusRecord> load_package_status_records() {
    return load_status_records_from_file(STATUS_FILE);
}

std::vector<PackageStatusRecord> load_dpkg_package_status_records() {
    return load_status_records_from_file(DPKG_STATUS_FILE);
}

bool get_json_value(const std::string& obj, const std::string& key, std::string& out_val);

bool write_text_file_atomically_core(const std::string& path, const std::string& content) {
    if (!mkdir_parent(path)) return false;

    std::string pattern = path + ".XXXXXX";
    std::vector<char> tmpl(pattern.begin(), pattern.end());
    tmpl.push_back('\0');

    int fd = mkstemp(tmpl.data());
    if (fd < 0) return false;

    bool ok = true;
    ssize_t remaining = static_cast<ssize_t>(content.size());
    const char* cursor = content.data();
    while (remaining > 0) {
        ssize_t written = write(fd, cursor, static_cast<size_t>(remaining));
        if (written < 0) {
            ok = false;
            break;
        }
        remaining -= written;
        cursor += written;
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (fchmod(fd, 0644) != 0) ok = false;
    close(fd);

    if (!ok) {
        unlink(tmpl.data());
        return false;
    }

    if (rename(tmpl.data(), path.c_str()) != 0) {
        unlink(tmpl.data());
        return false;
    }

    return true;
}

bool get_package_status_record(const std::string& pkg_name, PackageStatusRecord* out) {
    std::vector<PackageStatusRecord> records = load_package_status_records();
    for (const auto& record : records) {
        if (record.package != pkg_name) continue;
        if (out) *out = record;
        return true;
    }
    return false;
}

bool get_dpkg_package_status_record(const std::string& pkg_name, PackageStatusRecord* out) {
    std::vector<PackageStatusRecord> records = load_dpkg_package_status_records();
    for (const auto& record : records) {
        if (record.package != pkg_name) continue;
        if (out) *out = record;
        return true;
    }
    return false;
}

bool get_base_system_exact_live_version_hint(
    const std::string& pkg_name,
    std::string* version_out = nullptr
);

NativeSyntheticStateRecord normalize_native_synthetic_state_record(const NativeSyntheticStateRecord& record) {
    NativeSyntheticStateRecord normalized = record;
    normalized.package = trim(normalized.package);
    normalized.version = trim(normalized.version);
    normalized.provenance = trim(normalized.provenance);
    normalized.version_confidence = trim(normalized.version_confidence);

    if (normalized.provenance.empty()) normalized.provenance = "unknown";
    if (normalized.version_confidence.empty()) normalized.version_confidence = "unknown";

    bool exact =
        normalized.version_confidence == "exact" &&
        normalized.satisfies_versioned_deps &&
        native_dpkg_version_is_exact(normalized.version);
    if (exact) {
        std::string live_exact_version;
        if ((normalized.provenance == "base_registry" ||
             package_has_present_base_registry_entry_exact(normalized.package)) &&
            (!get_base_system_exact_live_version_hint(normalized.package, &live_exact_version) ||
             trim(live_exact_version) != normalized.version)) {
            exact = false;
        }
    }
    if (!exact) {
        normalized.version.clear();
        normalized.version_confidence = "unknown";
        normalized.satisfies_versioned_deps = false;
    }

    return normalized;
}

bool native_synthetic_state_record_has_exact_version(const NativeSyntheticStateRecord& record) {
    NativeSyntheticStateRecord normalized = normalize_native_synthetic_state_record(record);
    return normalized.version_confidence == "exact" &&
           normalized.satisfies_versioned_deps &&
           native_dpkg_version_is_exact(normalized.version);
}

std::vector<NativeSyntheticStateRecord> load_native_synthetic_state_records() {
    std::vector<NativeSyntheticStateRecord> records;
    std::ifstream f(NATIVE_SYNTHETIC_STATUS_FILE);
    if (!f) return records;

    NativeSyntheticStateRecord current;
    bool have_content = false;
    std::string line;
    auto flush_record = [&]() {
        if (current.package.empty()) {
            current = NativeSyntheticStateRecord{};
            have_content = false;
            return;
        }

        records.push_back(normalize_native_synthetic_state_record(current));
        current = NativeSyntheticStateRecord{};
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
        if (key == "Package") current.package = value;
        else if (key == "Version") current.version = value;
        else if (key == "Gpkg-Provenance") current.provenance = value;
        else if (key == "Gpkg-Version-Confidence") current.version_confidence = value;
        else if (key == "Gpkg-Owns-Files") current.owns_files = (value != "0" && value != "false");
        else if (key == "Gpkg-Satisfies-Versioned-Deps") {
            current.satisfies_versioned_deps = (value == "1" || value == "true");
        }
    }

    if (have_content) flush_record();
    return records;
}

bool get_native_synthetic_state_record(const std::string& pkg_name, NativeSyntheticStateRecord* out) {
    std::vector<NativeSyntheticStateRecord> records = load_native_synthetic_state_records();
    for (const auto& record : records) {
        if (record.package != pkg_name) continue;
        if (out) *out = record;
        return true;
    }
    return false;
}

bool save_native_synthetic_state_records(const std::vector<NativeSyntheticStateRecord>& records) {
    if (records.empty()) {
        return unlink(NATIVE_SYNTHETIC_STATUS_FILE.c_str()) == 0 || errno == ENOENT;
    }

    std::ostringstream out;
    for (const auto& record : records) {
        NativeSyntheticStateRecord normalized = normalize_native_synthetic_state_record(record);
        if (normalized.package.empty()) continue;
        out << "Package: " << normalized.package << "\n";
        if (!normalized.version.empty()) out << "Version: " << normalized.version << "\n";
        out << "Gpkg-Provenance: " << normalized.provenance << "\n";
        out << "Gpkg-Version-Confidence: "
            << normalized.version_confidence << "\n";
        out << "Gpkg-Owns-Files: " << (normalized.owns_files ? "1" : "0") << "\n";
        out << "Gpkg-Satisfies-Versioned-Deps: "
            << (normalized.satisfies_versioned_deps ? "1" : "0") << "\n\n";
    }

    return write_text_file_atomically_core(NATIVE_SYNTHETIC_STATUS_FILE, out.str());
}

bool native_dpkg_package_has_real_control_artifacts(const std::string& pkg_name) {
    if (pkg_name.empty()) return false;

    DIR* dir = opendir(DPKG_INFO_DIR.c_str());
    if (!dir) return false;

    std::string prefix = pkg_name + ".";
    std::string plain_list = prefix + "list";
    bool found = false;

    while (true) {
        errno = 0;
        dirent* entry = readdir(dir);
        if (!entry) break;

        std::string name = entry->d_name;
        if (name.empty() || name == "." || name == "..") continue;
        if (name.rfind(prefix, 0) != 0) continue;
        if (name == plain_list) continue;
        found = true;
        break;
    }

    closedir(dir);
    return found;
}

bool native_dpkg_package_has_exact_registered_live_version(
    const std::string& pkg_name,
    std::string* version_out = nullptr
) {
    if (version_out) version_out->clear();
    if (pkg_name.empty()) return false;

    PackageStatusRecord record;
    if (get_package_status_record(pkg_name, &record) &&
        !package_status_is_installed_like(record.status)) {
        return false;
    }

    if (get_package_status_record(pkg_name, &record) &&
        package_status_is_installed_like(record.status) &&
        !trim(record.version).empty()) {
        if (version_out) *version_out = trim(record.version);
        return true;
    }

    std::ifstream f(INFO_DIR + pkg_name + ".json");
    if (!f) return false;

    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string version;
    if (!get_json_value(content, "version", version) || trim(version).empty()) return false;
    if (version_out) *version_out = trim(version);
    return true;
}

bool get_repo_native_live_payload_version_hint(
    const std::string& pkg_name,
    std::string* version_out
) {
    if (version_out) version_out->clear();

    std::string canonical_name = canonicalize_package_name(pkg_name);
    if (canonical_name != "gpkg") return false;

    const std::vector<std::string> probe_paths = {
        ROOT_PREFIX + "/bin/apps/system/gpkg",
        ROOT_PREFIX + "/bin/gpkg",
    };
    bool live_payload_present = false;
    for (const auto& path : probe_paths) {
        if (!path.empty() && access(path.c_str(), F_OK) == 0) {
            live_payload_present = true;
            break;
        }
    }
    if (!live_payload_present) return false;

    std::string version = trim(GPKG_VERSION);
    if (version.empty() || version == OS_VERSION) return false;

    if (version_out) *version_out = version;
    return true;
}

bool base_system_package_prefers_live_version_inference(const std::string& pkg_name) {
    std::string canonical_name = canonicalize_package_name(pkg_name);
    return canonical_name == "libc6" ||
           canonical_name == "libform6" ||
           canonical_name == "libmenu6" ||
           canonical_name == "libncurses6" ||
           canonical_name == "libncursesw6" ||
           canonical_name == "libpanel6" ||
           canonical_name == "libtinfo6";
}

std::string base_system_runtime_family_for_package(const std::string& pkg_name) {
    std::string canonical_name = canonicalize_package_name(pkg_name);
    if (canonical_name == "libform6") return "libform.so.6";
    if (canonical_name == "libmenu6") return "libmenu.so.6";
    if (canonical_name == "libncurses6") return "libncurses.so.6";
    if (canonical_name == "libncursesw6") return "libncursesw.so.6";
    if (canonical_name == "libpanel6") return "libpanel.so.6";
    if (canonical_name == "libtinfo6") return "libtinfo.so.6";
    return "";
}

std::string extract_leading_numeric_version(const std::string& value) {
    std::string normalized = trim(value);
    std::string extracted;
    extracted.reserve(normalized.size());

    bool saw_digit = false;
    for (char ch : normalized) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            extracted += ch;
            saw_digit = true;
            continue;
        }
        if (ch == '.' && saw_digit) {
            extracted += ch;
            continue;
        }
        break;
    }

    while (!extracted.empty() && extracted.back() == '.') extracted.pop_back();
    return extracted;
}

std::string resolve_existing_realpath(const std::string& path) {
    char resolved[4096];
    if (!realpath(path.c_str(), resolved)) return "";
    return std::string(resolved);
}

std::string extract_glibc_release_version_from_binary(const std::string& binary_path) {
    std::ifstream in(binary_path, std::ios::binary);
    if (!in) return "";

    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    static const std::string needle = "GNU C Library (GNU libc) stable release version ";
    size_t pos = content.find(needle);
    if (pos == std::string::npos) return "";

    pos += needle.size();
    size_t end = pos;
    while (end < content.size()) {
        char ch = content[end];
        if (!std::isdigit(static_cast<unsigned char>(ch)) && ch != '.') break;
        ++end;
    }

    return content.substr(pos, end - pos);
}

bool infer_base_system_runtime_library_version_hint(
    const std::string& pkg_name,
    std::string* version_out = nullptr
) {
    if (version_out) version_out->clear();

    std::string family = base_system_runtime_family_for_package(pkg_name);
    if (family.empty()) return false;

    const std::vector<std::string> prefixes = {
        ROOT_PREFIX + "/usr/lib/x86_64-linux-gnu",
        ROOT_PREFIX + "/lib/x86_64-linux-gnu",
    };
    for (const auto& prefix : prefixes) {
        std::string resolved = resolve_existing_realpath(prefix + "/" + family);
        if (resolved.empty()) continue;

        std::string basename = path_basename(resolved);
        size_t so_pos = basename.find(".so.");
        if (so_pos == std::string::npos) continue;

        std::string version = extract_leading_numeric_version(basename.substr(so_pos + 4));
        if (version.empty()) continue;

        if (version_out) *version_out = version;
        return true;
    }

    return false;
}

bool infer_base_system_glibc_version_hint(std::string* version_out = nullptr) {
    if (version_out) version_out->clear();

    static bool cached = false;
    static std::string cached_version;
    if (cached) {
        if (version_out) *version_out = cached_version;
        return !cached_version.empty();
    }
    cached = true;

    const std::vector<std::string> candidates = {
        ROOT_PREFIX + "/lib/x86_64-linux-gnu/libc.so.6",
        ROOT_PREFIX + "/usr/lib/x86_64-linux-gnu/libc.so.6",
    };
    for (const auto& candidate : candidates) {
        std::string version = trim(extract_glibc_release_version_from_binary(candidate));
        if (version.empty()) continue;
        cached_version = version;
        break;
    }

    if (version_out) *version_out = cached_version;
    return !cached_version.empty();
}

std::string get_raw_base_system_registry_version_for_package(const std::string& pkg_name) {
    if (pkg_name.empty()) return "";
    std::string canonical_name = canonicalize_package_name(pkg_name);

    for (const auto& entry : load_base_debian_package_registry_entries()) {
        if (!base_system_registry_entry_looks_present(entry)) continue;
        if (canonicalize_package_name(entry.package) != canonical_name) continue;
        return trim(entry.version);
    }

    for (const auto& entry : load_base_system_registry_entries()) {
        if (!base_system_registry_entry_looks_present(entry)) continue;
        if (!base_registry_identity_has_exact_registry_version(canonical_name)) continue;

        std::vector<std::string> identities = get_base_registry_package_identities(entry);
        if (std::find(identities.begin(), identities.end(), canonical_name) == identities.end()) {
            continue;
        }

        return trim(entry.version);
    }
    return "";
}

bool get_base_system_exact_live_version_hint(
    const std::string& pkg_name,
    std::string* version_out
) {
    if (version_out) version_out->clear();
    if (pkg_name.empty()) return false;

    std::string canonical_name = canonicalize_package_name(pkg_name);
    if (canonical_name == "libc6") {
        return infer_base_system_glibc_version_hint(version_out);
    }

    if (base_system_package_prefers_live_version_inference(canonical_name)) {
        return infer_base_system_runtime_library_version_hint(canonical_name, version_out);
    }

    std::string registry_version = trim(get_raw_base_system_registry_version_for_package(canonical_name));
    if (registry_version.empty()) return false;

    if (version_out) *version_out = registry_version;
    return true;
}

std::string resolve_base_system_status_version(
    const std::string& pkg_name,
    const std::string& registry_version
) {
    std::string normalized_registry_version = trim(registry_version);
    std::string exact_version;
    if (native_dpkg_package_has_exact_registered_live_version(pkg_name, &exact_version)) {
        return exact_version;
    }

    if (get_base_system_exact_live_version_hint(pkg_name, &exact_version) &&
        !trim(exact_version).empty()) {
        return trim(exact_version);
    }

    NativeSyntheticStateRecord synthetic_record;
    if (!get_native_synthetic_state_record(pkg_name, &synthetic_record)) {
        PackageStatusRecord dpkg_record;
        if (get_dpkg_package_status_record(pkg_name, &dpkg_record) &&
            package_status_is_installed_like(dpkg_record.status) &&
            native_dpkg_version_is_exact(dpkg_record.version)) {
            return trim(dpkg_record.version);
        }
    }

    if (!normalized_registry_version.empty() &&
        !base_system_package_prefers_live_version_inference(pkg_name)) {
        return normalized_registry_version;
    }

    if (!base_system_package_prefers_live_version_inference(pkg_name)) {
        std::string exact_registry_version = get_raw_base_system_registry_version_for_package(pkg_name);
        if (!exact_registry_version.empty()) {
            return exact_registry_version;
        }
    }

    return "";
}

bool package_status_is_installed_like(const std::string& state) {
    return state == "half-installed" ||
           state == "unpacked" ||
           state == "half-configured" ||
           state == "triggers-awaited" ||
           state == "triggers-pending" ||
           state == "installed";
}

bool native_dpkg_version_is_exact(const std::string& version) {
    return !trim(version).empty();
}

bool get_native_dpkg_exact_live_version_hint(
    const std::string& pkg_name,
    std::string* version_out
) {
    if (version_out) version_out->clear();
    if (pkg_name.empty()) return false;

    std::string exact_version;
    if (native_dpkg_package_has_exact_registered_live_version(pkg_name, &exact_version) &&
        !trim(exact_version).empty()) {
        if (version_out) *version_out = trim(exact_version);
        return true;
    }

    if (get_repo_native_live_payload_version_hint(pkg_name, &exact_version) &&
        !trim(exact_version).empty()) {
        if (version_out) *version_out = trim(exact_version);
        return true;
    }

    if (get_base_system_exact_live_version_hint(pkg_name, &exact_version) &&
        !trim(exact_version).empty()) {
        if (version_out) *version_out = trim(exact_version);
        return true;
    }

    NativeSyntheticStateRecord synthetic_record;
    if (get_native_synthetic_state_record(pkg_name, &synthetic_record) &&
        native_synthetic_state_record_has_exact_version(synthetic_record)) {
        if (version_out) *version_out = trim(synthetic_record.version);
        return true;
    }

    if (!base_system_package_prefers_live_version_inference(pkg_name)) {
        exact_version = trim(get_raw_base_system_registry_version_for_package(pkg_name));
        if (!exact_version.empty()) {
            if (version_out) *version_out = exact_version;
            return true;
        }
    }

    PackageStatusRecord dpkg_record;
    if (get_dpkg_package_status_record(pkg_name, &dpkg_record) &&
        package_status_is_installed_like(dpkg_record.status) &&
        native_dpkg_version_is_exact(dpkg_record.version)) {
        if (version_out) *version_out = trim(dpkg_record.version);
        return true;
    }

    return false;
}

bool resolve_native_live_package_state(const std::string& pkg_name, NativeLivePackageState* out) {
    if (out) *out = NativeLivePackageState{};
    if (pkg_name.empty()) return false;

    NativeLivePackageState state;
    state.package = pkg_name;

    std::string exact_version;
    if (get_native_dpkg_exact_live_version_hint(pkg_name, &exact_version)) {
        state.present = true;
        state.version = trim(exact_version);
        state.exact_version_known = native_dpkg_version_is_exact(state.version);
        state.admissible_for_dpkg_status = state.exact_version_known;
        state.version_confidence = state.exact_version_known ? "exact" : "unknown";

        NativeSyntheticStateRecord synthetic_record;
        if (get_native_synthetic_state_record(pkg_name, &synthetic_record) &&
            native_synthetic_state_record_has_exact_version(synthetic_record)) {
            state.provenance = synthetic_record.provenance.empty() ? "synthetic_exact" : synthetic_record.provenance;
        } else if (native_dpkg_package_has_exact_registered_live_version(pkg_name)) {
            state.provenance = "registered";
        } else if (get_repo_native_live_payload_version_hint(pkg_name)) {
            state.provenance = "live_payload";
        } else if (get_base_system_exact_live_version_hint(pkg_name)) {
            state.provenance = "base_runtime";
        } else if (!get_raw_base_system_registry_version_for_package(pkg_name).empty()) {
            state.provenance = "base_registry";
        } else {
            state.provenance = "dpkg_status";
        }
        if (out) *out = state;
        return true;
    }

    PackageStatusRecord registered_record;
    if (get_package_status_record(pkg_name, &registered_record) &&
        package_status_is_installed_like(registered_record.status)) {
        state.present = true;
        state.provenance = "registered";
    }

    NativeSyntheticStateRecord synthetic_record;
    if (get_native_synthetic_state_record(pkg_name, &synthetic_record)) {
        synthetic_record = normalize_native_synthetic_state_record(synthetic_record);
        state.present = true;
        state.provenance = synthetic_record.provenance.empty() ? state.provenance : synthetic_record.provenance;
        state.version_confidence = synthetic_record.version_confidence.empty() ? state.version_confidence : synthetic_record.version_confidence;
    }

    PackageStatusRecord dpkg_record;
    if (get_dpkg_package_status_record(pkg_name, &dpkg_record) &&
        package_status_is_installed_like(dpkg_record.status)) {
        state.present = true;
        if (state.provenance == "unknown") state.provenance = "dpkg_status";
    }

    PackageStatusRecord base_record;
    if (get_base_system_package_status_record(pkg_name, &base_record) &&
        package_status_is_installed_like(base_record.status)) {
        state.present = true;
        if (state.provenance == "unknown") state.provenance = "base_registry";
    }

    if (!state.present && (package_is_base_system_provided(pkg_name) ||
                           package_has_present_base_registry_entry_exact(pkg_name))) {
        state.present = true;
        state.provenance = "base_registry";
    }

    if (out) *out = state;
    return state.present;
}

bool write_package_auto_state_records(const std::vector<PackageAutoStateRecord>& records) {
    std::vector<PackageAutoStateRecord> normalized;
    normalized.reserve(records.size());
    for (const auto& record : records) {
        if (record.package.empty() || !record.auto_installed) continue;
        normalized.push_back(record);
    }

    std::sort(normalized.begin(), normalized.end(), [](const PackageAutoStateRecord& left, const PackageAutoStateRecord& right) {
        return left.package < right.package;
    });
    normalized.erase(
        std::unique(normalized.begin(), normalized.end(), [](const PackageAutoStateRecord& left, const PackageAutoStateRecord& right) {
            return left.package == right.package;
        }),
        normalized.end()
    );

    if (normalized.empty()) {
        return unlink(EXTENDED_STATES_FILE.c_str()) == 0 || errno == ENOENT;
    }

    if (!mkdir_p(ROOT_PREFIX + "/var/lib/gpkg")) return false;

    std::string pattern = ROOT_PREFIX + "/var/lib/gpkg/.extended_states.XXXXXX";
    std::vector<char> tmpl(pattern.begin(), pattern.end());
    tmpl.push_back('\0');
    int fd = mkstemp(tmpl.data());
    if (fd < 0) return false;

    std::ostringstream out;
    for (const auto& record : normalized) {
        out << "Package: " << record.package << "\n";
        out << "Auto-Installed: 1\n\n";
    }
    std::string content = out.str();

    bool ok = true;
    ssize_t remaining = static_cast<ssize_t>(content.size());
    const char* cursor = content.data();
    while (remaining > 0) {
        ssize_t written = write(fd, cursor, static_cast<size_t>(remaining));
        if (written < 0) {
            ok = false;
            break;
        }
        remaining -= written;
        cursor += written;
    }
    if (ok && fsync(fd) != 0) ok = false;
    close(fd);

    if (!ok) {
        unlink(tmpl.data());
        return false;
    }
    if (rename(tmpl.data(), EXTENDED_STATES_FILE.c_str()) != 0) {
        unlink(tmpl.data());
        return false;
    }
    return true;
}

std::vector<PackageAutoStateRecord> load_package_auto_state_records() {
    std::vector<PackageAutoStateRecord> records;
    std::ifstream f(EXTENDED_STATES_FILE);
    if (!f) return records;

    PackageAutoStateRecord current;
    bool have_content = false;
    std::string line;
    auto flush_record = [&]() {
        if (current.package.empty()) {
            current = PackageAutoStateRecord{};
            have_content = false;
            return;
        }

        if (current.auto_installed) records.push_back(current);
        current = PackageAutoStateRecord{};
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
        } else if (key == "Auto-Installed") {
            current.auto_installed =
                value == "1" || value == "yes" || value == "true" || value == "True";
        }
    }

    if (have_content) flush_record();
    return records;
}

bool get_package_auto_installed_state(const std::string& pkg_name, bool* out_auto) {
    if (out_auto) *out_auto = false;
    for (const auto& record : load_package_auto_state_records()) {
        if (record.package != pkg_name) continue;
        if (out_auto) *out_auto = record.auto_installed;
        return true;
    }
    return false;
}

bool set_package_auto_installed_state(const std::string& pkg_name, bool auto_installed) {
    std::vector<PackageAutoStateRecord> records = load_package_auto_state_records();
    bool found = false;
    for (auto& record : records) {
        if (record.package != pkg_name) continue;
        record.auto_installed = auto_installed;
        found = true;
        break;
    }

    if (!found && auto_installed) {
        PackageAutoStateRecord record;
        record.package = pkg_name;
        record.auto_installed = true;
        records.push_back(record);
    }

    return write_package_auto_state_records(records);
}

bool erase_package_auto_installed_state(const std::string& pkg_name) {
    std::vector<PackageAutoStateRecord> records = load_package_auto_state_records();
    size_t original_size = records.size();
    records.erase(
        std::remove_if(records.begin(), records.end(), [&](const PackageAutoStateRecord& record) {
            return record.package == pkg_name;
        }),
        records.end()
    );
    if (records.size() == original_size) return true;
    return write_package_auto_state_records(records);
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

std::vector<PackageStatusRecord> load_base_system_package_status_records() {
    std::map<std::string, PackageStatusRecord> records_by_package;
    auto remember_record = [&](const PackageStatusRecord& record) {
        if (record.package.empty()) return;
        auto existing = records_by_package.find(record.package);
        if (existing == records_by_package.end() ||
            (!native_dpkg_version_is_exact(existing->second.version) &&
             native_dpkg_version_is_exact(record.version))) {
            records_by_package[record.package] = record;
        }
    };

    for (const auto& entry : load_base_debian_package_registry_entries()) {
        if (!base_system_registry_entry_looks_present(entry)) continue;

        PackageStatusRecord record;
        record.package = canonicalize_package_name(entry.package);
        record.version = resolve_base_system_status_version(record.package, entry.version);
        record.want = "install";
        record.flag = "ok";
        record.status = "installed";
        remember_record(record);
    }

    for (const auto& entry : load_base_system_registry_entries()) {
        if (!base_system_registry_entry_looks_present(entry)) continue;

        std::vector<std::string> identities = get_base_registry_package_identities(entry);
        for (const auto& identity : identities) {
            if (identity.empty()) continue;

            PackageStatusRecord record;
            record.package = identity;
            record.version = resolve_base_system_status_version(
                identity,
                base_registry_identity_has_exact_registry_version(identity) ? entry.version : ""
            );
            record.want = "install";
            record.flag = "ok";
            record.status = "installed";
            remember_record(record);
        }
    }

    std::vector<PackageStatusRecord> records;
    records.reserve(records_by_package.size());
    for (const auto& pair : records_by_package) {
        records.push_back(pair.second);
    }
    return records;
}

bool get_base_system_package_status_record(const std::string& pkg_name, PackageStatusRecord* out) {
    std::vector<PackageStatusRecord> records = load_base_system_package_status_records();
    for (const auto& record : records) {
        if (record.package != pkg_name) continue;
        if (out) *out = record;
        return true;
    }
    return false;
}

std::vector<BaseSystemRegistryEntry> load_base_registry_entries_from_path(const std::string& path) {
    std::vector<BaseSystemRegistryEntry> entries;
    foreach_json_object(path, [&](const std::string& obj) {
        BaseSystemRegistryEntry entry;
        if (!get_json_value(obj, "package", entry.package)) return true;
        get_json_value(obj, "version", entry.version);
        get_json_array(obj, "files", entry.files);
        entries.push_back(std::move(entry));
        return true;
    });
    return entries;
}

std::vector<BaseSystemRegistryEntry> load_base_debian_package_registry_entries() {
    return load_base_registry_entries_from_path(BASE_DEBIAN_PACKAGE_REGISTRY_PATH);
}

std::vector<BaseSystemRegistryEntry> load_base_system_registry_entries() {
    return load_base_registry_entries_from_path(BASE_SYSTEM_REGISTRY_PATH);
}

bool base_system_registry_entry_looks_present(const BaseSystemRegistryEntry& entry) {
    if (entry.package.empty()) return false;
    if (entry.files.empty()) return false;

    for (const auto& rel_path : entry.files) {
        if (rel_path.empty() || rel_path[0] != '/') continue;
        std::string full_path = ROOT_PREFIX + rel_path;
        struct stat st {};
        if (lstat(full_path.c_str(), &st) == 0) return true;
    }

    return false;
}

bool ask_confirmation(const std::string& query) {
    if (g_assume_yes) {
        std::cout << Color::YELLOW << query << " [Y/n] y" << Color::RESET << std::endl;
        return true;
    }
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
        // Debian compares maximal non-digit segments first. If one side is
        // already at a digit, it contributes end-of-segment here rather than
        // the digit byte itself.
        while ((i < left.size() && !std::isdigit(static_cast<unsigned char>(left[i]))) ||
               (j < right.size() && !std::isdigit(static_cast<unsigned char>(right[j])))) {
            char lc = (i < left.size() && !std::isdigit(static_cast<unsigned char>(left[i])))
                ? left[i]
                : '\0';
            char rc = (j < right.size() && !std::isdigit(static_cast<unsigned char>(right[j])))
                ? right[j]
                : '\0';
            int lo = debian_char_order(lc);
            int ro = debian_char_order(rc);
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
    PackageStatusRecord record;
    if (get_package_status_record(pkg, &record)) {
        if (!package_status_is_installed_like(record.status)) return false;

        if (out_version) {
            if (!record.version.empty()) {
                *out_version = record.version;
            } else {
                std::string info_path = INFO_DIR + pkg + ".json";
                std::ifstream f(info_path);
                if (!f) return false;
                std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
                return get_json_value(content, "version", *out_version);
            }
        }
        return true;
    }

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

int run_command_argv(
    const std::vector<std::string>& argv,
    bool verbose,
    int stdout_fd,
    int stderr_fd
) {
    if (argv.empty() || argv.front().empty()) return -1;

    if (verbose) {
        std::ostringstream rendered;
        for (size_t i = 0; i < argv.size(); ++i) {
            if (i != 0) rendered << ' ';
            rendered << shell_quote(argv[i]);
        }
        std::cout << "[DEBUG] Executing: " << rendered.str() << std::endl;
    }

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (stdout_fd >= 0 && dup2(stdout_fd, STDOUT_FILENO) < 0) _exit(127);
        if (stderr_fd >= 0 && dup2(stderr_fd, STDERR_FILENO) < 0) _exit(127);

        std::vector<char*> cargv;
        cargv.reserve(argv.size() + 1);
        for (const auto& arg : argv) {
            cargv.push_back(const_cast<char*>(arg.c_str()));
        }
        cargv.push_back(nullptr);
        execvp(argv.front().c_str(), cargv.data());
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) continue;
        return -1;
    }
    return status;
}

int decode_command_exit_status(int status) {
    if (status == -1) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
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
        return {decode_command_exit_status(run_command(cmd, true)), ""};
    }

    std::string prefix = "/tmp/" + log_prefix + "-XXXXXX.log";
    std::vector<char> tmpl(prefix.begin(), prefix.end());
    tmpl.push_back('\0');

    int fd = mkstemps(tmpl.data(), 4);
    if (fd < 0) {
        return {decode_command_exit_status(run_command(cmd, false)), ""};
    }
    close(fd);

    std::string log_path(tmpl.data());
    std::string wrapped = cmd + " >" + shell_quote(log_path) + " 2>&1";
    return {decode_command_exit_status(run_command(wrapped, false)), log_path};
}

CommandCaptureResult run_command_captured_argv(
    const std::vector<std::string>& argv,
    bool verbose,
    const std::string& log_prefix
) {
    if (verbose) {
        return {decode_command_exit_status(run_command_argv(argv, true)), ""};
    }

    std::string prefix = "/tmp/" + log_prefix + "-XXXXXX.log";
    std::vector<char> tmpl(prefix.begin(), prefix.end());
    tmpl.push_back('\0');

    int fd = mkstemps(tmpl.data(), 4);
    if (fd < 0) {
        return {decode_command_exit_status(run_command_argv(argv, false)), ""};
    }

    std::string log_path(tmpl.data());
    int status = run_command_argv(argv, false, fd, fd);
    close(fd);
    return {decode_command_exit_status(status), log_path};
}
