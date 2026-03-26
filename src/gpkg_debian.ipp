// Debian testing backend: config loading, metadata import, and .deb to .gpkg conversion.

#include <cstdint>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

struct DebianBackendConfig {
    std::string packages_url = "https://deb.debian.org/debian/dists/testing/main/binary-amd64/Packages.gz";
    std::string base_url = "https://deb.debian.org/debian";
    std::string apt_arch = "amd64";
};

struct DebianPackageRecord {
    std::string package;
    std::string version;
    std::string architecture;
    std::string maintainer;
    std::string section;
    std::string priority;
    std::string filename;
    std::string sha256;
    std::string size;
    std::string installed_size;
    std::string depends_raw;
    std::string pre_depends_raw;
    std::string recommends_raw;
    std::string suggests_raw;
    std::string conflicts_raw;
    std::string provides_raw;
    std::string replaces_raw;
    std::string description;
    bool essential = false;
};

struct DebianPackagesCacheState {
    std::string packages_url;
    std::string etag;
    std::string last_modified;
    long content_length = -1;
};

std::string sanitize_section_name(const std::string& raw_section) {
    std::string top_level = raw_section;
    size_t slash = top_level.find('/');
    if (slash != std::string::npos) top_level = top_level.substr(0, slash);
    top_level = trim(top_level);
    std::string sanitized;
    for (char ch : top_level) {
        char lowered = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        if (std::isalnum(static_cast<unsigned char>(lowered)) || lowered == '.' || lowered == '+' || lowered == '-') {
            sanitized += lowered;
        } else if (!sanitized.empty() && sanitized.back() != '-') {
            sanitized += '-';
        }
    }
    while (!sanitized.empty() && sanitized.back() == '-') sanitized.pop_back();
    return sanitized.empty() ? "misc" : sanitized;
}

std::string safe_repo_filename_component(const std::string& value) {
    std::string safe;
    safe.reserve(value.size());
    for (char ch : value) {
        if (ch == '/' || ch == ' ') safe += '_';
        else safe += ch;
    }
    return safe;
}

bool path_exists_no_follow_debian(const std::string& path) {
    struct stat st;
    return lstat(path.c_str(), &st) == 0;
}

bool create_payload_symlink_if_missing(
    const std::string& link_path,
    const std::string& target_path,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    if (path_exists_no_follow_debian(link_path)) return true;
    if (!mkdir_parent(link_path)) {
        if (error_out) *error_out = "failed to create parent directory";
        return false;
    }
    if (symlink(target_path.c_str(), link_path.c_str()) != 0) {
        if (error_out) *error_out = strerror(errno);
        return false;
    }
    return true;
}

bool should_promote_multiarch_runtime_entry(
    const std::string& name,
    const struct stat& st
) {
    if (S_ISDIR(st.st_mode)) return false;
    return (name.rfind("lib", 0) == 0 && name.find(".so.") != std::string::npos) ||
           name.rfind("ld-linux-", 0) == 0;
}

bool ensure_runtime_compat_payload_aliases(
    const std::string& payload_root,
    const std::string& canonical_prefix,
    const std::string& compat_prefix,
    const std::string& legacy_compat_prefix,
    bool verbose
) {
    std::string source_root = payload_root + canonical_prefix;
    DIR* dir = opendir(source_root.c_str());
    if (!dir) {
        return errno == ENOENT;
    }

    if (!mkdir_p(payload_root + compat_prefix) ||
        !mkdir_p(payload_root + legacy_compat_prefix)) {
        closedir(dir);
        return false;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        std::string source_full_path = source_root + "/" + name;
        std::string source_path = canonical_prefix + "/" + name;
        std::string compat_path = payload_root + compat_prefix + "/" + name;
        std::string legacy_compat_path = payload_root + legacy_compat_prefix + "/" + name;
        struct stat st {};
        if (lstat(source_full_path.c_str(), &st) != 0) {
            closedir(dir);
            std::cerr << Color::RED << "E: Failed to inspect imported payload entry "
                      << source_path << " (" << strerror(errno) << ")"
                      << Color::RESET << std::endl;
            return false;
        }

        if (!should_promote_multiarch_runtime_entry(name, st)) {
            VLOG(verbose, "Skipping non-runtime multiarch alias candidate " << source_path);
            continue;
        }

        std::string error;
        if (!create_payload_symlink_if_missing(compat_path, source_path, &error)) {
            closedir(dir);
            std::cerr << Color::RED << "E: Failed to create runtime compatibility alias "
                      << compat_prefix << "/" << name << " -> " << source_path;
            if (!error.empty()) std::cerr << " (" << error << ")";
            std::cerr << Color::RESET << std::endl;
            return false;
        }

        if (!create_payload_symlink_if_missing(legacy_compat_path, compat_prefix + "/" + name, &error)) {
            closedir(dir);
            std::cerr << Color::RED << "E: Failed to create legacy runtime compatibility alias "
                      << legacy_compat_prefix << "/" << name << " -> "
                      << compat_prefix << "/" << name;
            if (!error.empty()) std::cerr << " (" << error << ")";
            std::cerr << Color::RESET << std::endl;
            return false;
        }

        VLOG(verbose, "Added Debian runtime compatibility alias " << compat_prefix << "/" << name
             << " -> " << source_path);
        VLOG(verbose, "Added Debian legacy runtime compatibility alias "
             << legacy_compat_prefix << "/" << name << " -> "
             << compat_prefix << "/" << name);
    }

    closedir(dir);
    return true;
}

bool normalize_imported_payload_layout(
    const std::string& payload_root,
    bool verbose
) {
    struct PrefixMap {
        const char* canonical_prefix;
        const char* compat_prefix;
        const char* legacy_compat_prefix;
    };

    const PrefixMap maps[] = {
        {"/lib/x86_64-linux-gnu", "/lib64", "/lib64/x86_64-linux-gnu"},
        {"/usr/lib/x86_64-linux-gnu", "/usr/lib64", "/usr/lib64/x86_64-linux-gnu"},
    };

    for (const auto& map : maps) {
        if (!ensure_runtime_compat_payload_aliases(
                payload_root,
                map.canonical_prefix,
                map.compat_prefix,
                map.legacy_compat_prefix,
                verbose)) {
            return false;
        }
    }

    return true;
}

DebianBackendConfig load_debian_backend_config(bool verbose = false) {
    DebianBackendConfig config;
    std::ifstream f(DEBIAN_CONFIG_PATH);
    if (!f) return config;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string value = trim(line.substr(eq + 1));
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }

        if (key == "PACKAGES_URL" && !value.empty()) config.packages_url = value;
        else if (key == "BASE_URL" && !value.empty()) config.base_url = value;
        else if (key == "APT_ARCH" && !value.empty()) config.apt_arch = value;
    }

    if (verbose) {
        std::cout << "[DEBUG] Debian backend: packages_url=" << config.packages_url
                  << " base_url=" << config.base_url
                  << " apt_arch=" << config.apt_arch << std::endl;
    }
    return config;
}

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), ::tolower);
    return value;
}

std::string extract_http_header_value(
    const std::string& headers,
    const std::string& lower_headers,
    const std::string& header_name
) {
    size_t pos = lower_headers.find(header_name);
    if (pos == std::string::npos) return "";

    size_t start = pos + header_name.size();
    size_t end = lower_headers.find("\r\n", start);
    if (end == std::string::npos) return "";
    return trim(headers.substr(start, end - start));
}

bool fetch_remote_packages_index_state(
    const std::string& url,
    DebianPackagesCacheState& state,
    bool verbose,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    HttpOptions opts;
    opts.method = "HEAD";
    opts.include_headers = true;
    opts.follow_location = true;
    opts.show_progress = false;
    opts.verbose = verbose;

    std::stringstream response;
    if (!HttpRequest(url, response, opts, error_out)) return false;

    std::string headers = response.str();
    std::string lower_headers = lowercase_copy(headers);

    state = {};
    state.packages_url = url;
    state.etag = extract_http_header_value(headers, lower_headers, "etag: ");
    state.last_modified = extract_http_header_value(headers, lower_headers, "last-modified: ");

    std::string content_length = extract_http_header_value(headers, lower_headers, "content-length: ");
    if (!content_length.empty()) state.content_length = std::atol(content_length.c_str());

    return true;
}

std::string get_debian_packages_state_path() {
    return REPO_CACHE_PATH + "debian/Packages.state";
}

bool load_debian_packages_cache_state(const std::string& path, DebianPackagesCacheState& state) {
    std::ifstream f(path);
    if (!f) return false;

    state = {};
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;

        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = trim(line.substr(0, eq));
        std::string value = line.substr(eq + 1);
        if (key == "PACKAGES_URL") state.packages_url = value;
        else if (key == "ETAG") state.etag = value;
        else if (key == "LAST_MODIFIED") state.last_modified = value;
        else if (key == "CONTENT_LENGTH") state.content_length = std::atol(value.c_str());
    }

    return !state.packages_url.empty();
}

bool save_debian_packages_cache_state(const std::string& path, const DebianPackagesCacheState& state) {
    if (!mkdir_parent(path)) return false;

    std::string temp_path = path + ".tmp";
    std::ofstream out(temp_path, std::ios::trunc);
    if (!out) return false;

    out << "PACKAGES_URL=" << state.packages_url << "\n";
    out << "ETAG=" << state.etag << "\n";
    out << "LAST_MODIFIED=" << state.last_modified << "\n";
    out << "CONTENT_LENGTH=" << state.content_length << "\n";
    out.close();

    if (!out) {
        remove(temp_path.c_str());
        return false;
    }

    if (rename(temp_path.c_str(), path.c_str()) != 0) {
        remove(temp_path.c_str());
        return false;
    }

    return true;
}

bool remote_packages_index_matches_cache(
    const DebianPackagesCacheState& cached,
    const DebianPackagesCacheState& remote
) {
    if (cached.packages_url.empty() || remote.packages_url.empty()) return false;
    if (cached.packages_url != remote.packages_url) return false;

    if (!cached.etag.empty() && !remote.etag.empty()) {
        return cached.etag == remote.etag;
    }

    if (!cached.last_modified.empty() && !remote.last_modified.empty()) {
        if (cached.last_modified != remote.last_modified) return false;
        if (cached.content_length > 0 && remote.content_length > 0) {
            return cached.content_length == remote.content_length;
        }
        return true;
    }

    return false;
}

std::vector<std::map<std::string, std::string>> parse_debian_control_stanzas(const std::string& text) {
    std::vector<std::map<std::string, std::string>> stanzas;
    std::map<std::string, std::string> current;
    std::string last_key;

    std::istringstream iss(text);
    std::string raw_line;
    while (std::getline(iss, raw_line)) {
        if (!raw_line.empty() && raw_line.back() == '\r') raw_line.pop_back();
        if (trim(raw_line).empty()) {
            if (!current.empty()) {
                stanzas.push_back(current);
                current.clear();
                last_key.clear();
            }
            continue;
        }

        if (!raw_line.empty() && std::isspace(static_cast<unsigned char>(raw_line[0]))) {
            if (last_key.empty()) continue;
            current[last_key] += "\n" + raw_line.substr(1);
            continue;
        }

        size_t colon = raw_line.find(':');
        if (colon == std::string::npos) continue;
        last_key = raw_line.substr(0, colon);
        current[last_key] = trim(raw_line.substr(colon + 1));
    }

    if (!current.empty()) stanzas.push_back(current);
    return stanzas;
}

std::string debian_description_text(const std::string& raw_description) {
    if (raw_description.empty()) return "";

    std::istringstream iss(raw_description);
    std::string line;
    std::vector<std::string> lines;
    while (std::getline(iss, line)) lines.push_back(line);
    if (lines.empty()) return raw_description;

    std::string summary = trim(lines[0]);
    std::vector<std::string> body_lines;
    for (size_t i = 1; i < lines.size(); ++i) {
        std::string stripped = trim(lines[i]);
        if (stripped == ".") body_lines.push_back("");
        else body_lines.push_back(stripped);
    }

    std::string body;
    for (size_t i = 0; i < body_lines.size(); ++i) {
        if (i > 0) body += "\n";
        body += body_lines[i];
    }
    body = trim(body);
    if (body.empty()) return summary;
    return summary + "\n\n" + body;
}

std::string json_array_from_strings(const std::vector<std::string>& items) {
    std::ostringstream out;
    out << "[";
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out << ",";
        out << "\"" << json_escape(items[i]) << "\"";
    }
    out << "]";
    return out.str();
}

std::string json_string_field(const std::string& key, const std::string& value) {
    return "\"" + json_escape(key) + "\":\"" + json_escape(value) + "\"";
}

std::string package_metadata_to_json(const PackageMetadata& meta) {
    std::vector<std::string> fields;
    fields.push_back(json_string_field("package", meta.name));
    fields.push_back(json_string_field("version", meta.version));
    fields.push_back(json_string_field("architecture", meta.arch));
    fields.push_back(json_string_field("maintainer", meta.maintainer));
    fields.push_back(json_string_field("description", meta.description));
    fields.push_back(json_string_field("package_scope", meta.package_scope));
    fields.push_back("\"depends\":" + json_array_from_strings(meta.depends));
    fields.push_back("\"recommends\":" + json_array_from_strings(meta.recommends));
    fields.push_back("\"suggests\":" + json_array_from_strings(meta.suggests));
    fields.push_back("\"conflicts\":" + json_array_from_strings(meta.conflicts));
    fields.push_back("\"provides\":" + json_array_from_strings(meta.provides));
    fields.push_back("\"replaces\":" + json_array_from_strings(meta.replaces));
    fields.push_back(json_string_field("section", meta.section));
    fields.push_back(json_string_field("priority", meta.priority));
    fields.push_back(json_string_field("filename", meta.filename));
    fields.push_back(json_string_field("source_kind", meta.source_kind));
    fields.push_back(json_string_field("source_url", meta.source_url));
    fields.push_back(json_string_field("repo_url", meta.source_url));
    fields.push_back(json_string_field("debian_package", meta.debian_package));
    fields.push_back(json_string_field("debian_version", meta.debian_version));
    fields.push_back(json_string_field("installed_from", meta.installed_from));
    fields.push_back(json_string_field("size", meta.size));
    fields.push_back(json_string_field("installed_size_bytes", meta.installed_size_bytes));
    if (!meta.sha256.empty()) fields.push_back(json_string_field("sha256", meta.sha256));
    if (!meta.sha512.empty()) fields.push_back(json_string_field("sha512", meta.sha512));

    std::ostringstream out;
    out << "{";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) out << ",";
        out << fields[i];
    }
    out << "}";
    return out.str();
}

std::vector<DebianPackageRecord> parse_debian_packages_file(
    const std::string& packages_path,
    const DebianBackendConfig& config,
    bool verbose
) {
    std::ifstream f(packages_path);
    if (!f) return {};
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());

    std::vector<DebianPackageRecord> parsed;
    for (const auto& fields : parse_debian_control_stanzas(content)) {
        DebianPackageRecord record;
        auto get_field = [&](const std::string& key) -> std::string {
            auto it = fields.find(key);
            return it == fields.end() ? "" : it->second;
        };

        record.package = get_field("Package");
        record.version = get_field("Version");
        record.architecture = get_field("Architecture");
        if (record.package.empty() || record.version.empty()) continue;
        if (!(record.architecture.empty() || record.architecture == "all" || record.architecture == config.apt_arch)) {
            continue;
        }

        record.maintainer = get_field("Maintainer");
        record.section = get_field("Section");
        record.priority = get_field("Priority");
        record.filename = get_field("Filename");
        record.sha256 = get_field("SHA256");
        record.size = get_field("Size");
        record.installed_size = get_field("Installed-Size");
        record.pre_depends_raw = get_field("Pre-Depends");
        record.depends_raw = get_field("Depends");
        record.recommends_raw = get_field("Recommends");
        record.suggests_raw = get_field("Suggests");
        record.conflicts_raw = get_field("Conflicts");
        record.provides_raw = get_field("Provides");
        record.replaces_raw = get_field("Replaces");
        record.description = debian_description_text(get_field("Description"));
        std::string essential = get_field("Essential");
        record.essential = !essential.empty() && trim(essential) == "yes";

        if (verbose && record.filename.empty()) {
            std::cout << "[DEBUG] Debian record missing Filename: " << record.package << std::endl;
        }
        parsed.push_back(record);
    }

    return parsed;
}

std::string debian_installed_size_kib_to_bytes_string(const std::string& kib_text) {
    std::string trimmed = trim(kib_text);
    if (trimmed.empty()) return "";

    char* end = nullptr;
    errno = 0;
    unsigned long long kib = std::strtoull(trimmed.c_str(), &end, 10);
    if (errno != 0 || end == trimmed.c_str()) return "";
    while (end && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (end && *end != '\0') return "";

    constexpr unsigned long long kBytesPerKib = 1024ULL;
    if (kib > std::numeric_limits<unsigned long long>::max() / kBytesPerKib) return "";
    return std::to_string(kib * kBytesPerKib);
}

std::string derive_debian_t64_legacy_alias(const std::string& package_name) {
    if (package_name.size() <= 3) return "";
    if (package_name.rfind("lib", 0) != 0) return "";
    if (package_name.size() < 4 || package_name.substr(package_name.size() - 3) != "t64") return "";

    std::string legacy = package_name.substr(0, package_name.size() - 3);
    return legacy == package_name ? std::string() : legacy;
}

void append_debian_t64_legacy_provides(
    std::vector<std::string>& provides,
    const std::string& package_name,
    const std::string& version
) {
    std::string legacy = derive_debian_t64_legacy_alias(package_name);
    if (legacy.empty()) return;

    if (std::find(provides.begin(), provides.end(), legacy) == provides.end()) {
        provides.push_back(legacy);
    }

    if (!version.empty()) {
        std::string versioned = legacy + " (= " + version + ")";
        if (std::find(provides.begin(), provides.end(), versioned) == provides.end()) {
            provides.push_back(versioned);
        }
    }
}

void append_debian_t64_legacy_conflicts_and_replaces(
    std::vector<std::string>& conflicts,
    std::vector<std::string>& replaces,
    const std::string& package_name
) {
    std::string legacy = derive_debian_t64_legacy_alias(package_name);
    if (legacy.empty()) return;

    if (std::find(conflicts.begin(), conflicts.end(), legacy) == conflicts.end()) {
        conflicts.push_back(legacy);
    }
    if (std::find(replaces.begin(), replaces.end(), legacy) == replaces.end()) {
        replaces.push_back(legacy);
    }
}

std::map<std::string, std::vector<std::string>> build_debian_provider_map(
    const std::vector<DebianPackageRecord>& records,
    const std::string& apt_arch
) {
    std::map<std::string, std::vector<std::string>> providers;
    for (const auto& record : records) {
        std::vector<std::string> provided = normalize_relation_field_value(record.provides_raw, apt_arch);
        append_debian_t64_legacy_provides(provided, record.package, record.version);
        for (const auto& capability : provided) {
            RelationAtom atom = normalize_relation_atom(capability, apt_arch);
            if (!atom.valid) continue;
            auto& entry = providers[atom.name];
            if (std::find(entry.begin(), entry.end(), record.package) == entry.end()) {
                entry.push_back(record.package);
            }
        }
    }
    return providers;
}

std::vector<std::string> apply_dependency_removals(
    const std::vector<std::string>& dependencies,
    const PackageOverridePolicy& package_override
) {
    std::vector<std::string> filtered = unique_string_list(dependencies);
    if (package_override.depends_remove.empty()) return filtered;

    std::vector<std::string> result;
    for (const auto& dep : filtered) {
        if (std::find(
                package_override.depends_remove.begin(),
                package_override.depends_remove.end(),
                dep
            ) == package_override.depends_remove.end()) {
            result.push_back(dep);
        }
    }
    return result;
}

PackageMetadata build_debian_package_metadata(
    const DebianPackageRecord& record,
    const DebianBackendConfig& config,
    const ImportPolicy& policy,
    const std::set<std::string>& available_packages,
    const std::map<std::string, std::vector<std::string>>& provider_map,
    const std::vector<std::string>& system_drop_patterns
) {
    PackageMetadata meta;
    auto override_it = policy.package_overrides.find(record.package);
    PackageOverridePolicy package_override;
    if (override_it != policy.package_overrides.end()) package_override = override_it->second;

    std::string package_name = package_override.rename.empty() ? record.package : package_override.rename;
    bool include_recommends = true;
    if (package_override.has_include_recommends) include_recommends = package_override.include_recommends;

    std::string required_dependency_text;
    if (!record.pre_depends_raw.empty()) required_dependency_text += record.pre_depends_raw;
    if (!record.depends_raw.empty()) {
        if (!required_dependency_text.empty()) required_dependency_text += ", ";
        required_dependency_text += record.depends_raw;
    }

    std::vector<std::string> depends = normalize_dependency_relation_value(
        required_dependency_text,
        record.package,
        config.apt_arch,
        true,
        policy,
        available_packages,
        provider_map,
        system_drop_patterns
    );
    for (const auto& dep : package_override.depends_add) depends.push_back(dep);
    depends = apply_dependency_removals(depends, package_override);
    std::vector<std::string> recommends = apply_dependency_removals(
        normalize_dependency_relation_value(
            record.recommends_raw,
            record.package,
            config.apt_arch,
            false,
            policy,
            available_packages,
            provider_map,
            system_drop_patterns
        ),
        package_override
    );
    std::vector<std::string> suggests = apply_dependency_removals(
        normalize_dependency_relation_value(
            record.suggests_raw,
            record.package,
            config.apt_arch,
            false,
            policy,
            available_packages,
            provider_map,
            system_drop_patterns
        ),
        package_override
    );

    meta.name = package_name;
    meta.version = record.version;
    meta.arch = record.architecture == "all" ? std::string(OS_ARCH) : std::string(OS_ARCH);
    meta.maintainer = record.maintainer.empty() ? "Debian Maintainers" : record.maintainer;
    meta.description = record.description.empty() ? record.package : record.description;
    meta.filename = record.filename;
    meta.sha256 = record.sha256;
    meta.source_url = config.base_url;
    meta.source_kind = "debian";
    meta.debian_package = record.package;
    meta.debian_version = record.version;
    meta.section = sanitize_section_name(package_override.section.empty() ? record.section : package_override.section);
    meta.priority = record.priority;
    meta.size = record.size;
    meta.installed_size_bytes = debian_installed_size_kib_to_bytes_string(record.installed_size);
    meta.depends = depends;
    meta.recommends = recommends;
    meta.suggests = suggests;
    meta.conflicts = normalize_relation_field_value(record.conflicts_raw, config.apt_arch);
    for (const auto& dep : package_override.conflicts_add) meta.conflicts.push_back(dep);
    
    meta.provides = normalize_relation_field_value(record.provides_raw, config.apt_arch);
    for (const auto& dep : package_override.provides_add) meta.provides.push_back(dep);
    
    meta.replaces = normalize_relation_field_value(record.replaces_raw, config.apt_arch);
    for (const auto& dep : package_override.replaces_add) meta.replaces.push_back(dep);

    append_debian_t64_legacy_provides(meta.provides, record.package, record.version);
    append_debian_t64_legacy_conflicts_and_replaces(meta.conflicts, meta.replaces, record.package);
    
    meta.package_scope = include_recommends ? "depends+recommends" : "depends";
    meta.installed_from = config.packages_url;
    return meta;
}

std::vector<PackageMetadata> load_debian_index_entries(
    const std::string& packages_path,
    bool verbose,
    std::vector<std::string>* skipped_policy = nullptr
) {
    DebianBackendConfig config = load_debian_backend_config(verbose);
    ImportPolicy policy = get_import_policy(verbose);
    std::vector<DebianPackageRecord> records = parse_debian_packages_file(packages_path, config, verbose);

    std::set<std::string> available_packages;
    for (const auto& record : records) available_packages.insert(record.package);
    std::vector<std::string> system_drop_patterns = build_system_drop_patterns(policy, available_packages);
    auto provider_map = build_debian_provider_map(records, config.apt_arch);

    const size_t worker_count = recommended_parallel_worker_count(records.size());
    if (verbose) {
        std::cout << "[DEBUG] Importing Debian metadata with "
                  << worker_count << " worker(s)." << std::endl;
    }

    std::atomic<size_t> next_record{0};
    std::vector<std::map<std::string, PackageMetadata>> worker_selected(worker_count);
    std::vector<std::vector<std::string>> worker_skipped(worker_count);

    auto worker = [&](size_t worker_index) {
        auto& selected = worker_selected[worker_index];
        auto& skipped = worker_skipped[worker_index];

        while (true) {
            size_t record_index = next_record.fetch_add(1);
            if (record_index >= records.size()) return;

            const auto& record = records[record_index];
            if (record.filename.empty() || record.sha256.empty()) continue;
            if (record.essential &&
                !matches_any_pattern(record.package, policy.allow_essential_packages)) {
                if (skipped_policy) skipped.push_back(record.package + ": Essential: yes");
                continue;
            }
            if (matches_any_pattern(record.package, policy.skip_packages)) {
                if (skipped_policy) skipped.push_back(record.package + ": blocked by policy");
                continue;
            }

            PackageMetadata meta = build_debian_package_metadata(
                record,
                config,
                policy,
                available_packages,
                provider_map,
                system_drop_patterns
            );

            auto it = selected.find(meta.name);
            if (it == selected.end() || compare_versions(meta.version, it->second.version) > 0) {
                selected[meta.name] = meta;
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

    std::map<std::string, PackageMetadata> selected;
    for (size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
        for (const auto& entry : worker_selected[worker_index]) {
            auto it = selected.find(entry.first);
            if (it == selected.end() || compare_versions(entry.second.version, it->second.version) > 0) {
                selected[entry.first] = entry.second;
            }
        }
        if (skipped_policy) {
            skipped_policy->insert(
                skipped_policy->end(),
                worker_skipped[worker_index].begin(),
                worker_skipped[worker_index].end()
            );
        }
    }

    std::vector<PackageMetadata> entries;
    for (const auto& entry : selected) entries.push_back(entry.second);
    return entries;
}

std::string get_debian_packages_gz_cache_path() {
    return REPO_CACHE_PATH + "debian/Packages.gz";
}

std::string get_debian_packages_cache_path() {
    return REPO_CACHE_PATH + "debian/Packages";
}

bool update_debian_backend_index(
    std::ofstream& merged,
    bool& first_object,
    int& total_packages,
    bool verbose
) {
    DebianBackendConfig config = load_debian_backend_config(verbose);
    std::string packages_gz = get_debian_packages_gz_cache_path();
    std::string packages_txt = get_debian_packages_cache_path();
    std::string packages_state = get_debian_packages_state_path();
    if (!mkdir_parent(packages_gz)) return false;

    DebianPackagesCacheState cached_state;
    bool have_cached_state = load_debian_packages_cache_state(packages_state, cached_state);
    bool have_packages_txt = access(packages_txt.c_str(), F_OK) == 0;

    DebianPackagesCacheState remote_state;
    std::string probe_error;
    bool have_remote_state = fetch_remote_packages_index_state(
        config.packages_url,
        remote_state,
        verbose,
        &probe_error
    );
    bool needs_download = true;

    if (have_packages_txt && have_cached_state && have_remote_state &&
        remote_packages_index_matches_cache(cached_state, remote_state)) {
        needs_download = false;
        VLOG(verbose, "Debian Packages index is unchanged on the server; reusing cached copy.");
    } else if (verbose && !have_remote_state && !probe_error.empty()) {
        VLOG(verbose, "Unable to probe Debian Packages metadata; falling back to full download: " << probe_error);
    }

    if (needs_download) {
        std::string download_error;
        if (!DownloadFile(config.packages_url, packages_gz, verbose, &download_error)) {
            std::cerr << Color::YELLOW << "W: Failed to fetch Debian Packages index from "
                      << config.packages_url;
            if (!download_error.empty()) std::cerr << " (" << download_error << ")";
            std::cerr << Color::RESET << std::endl;
            return false;
        }

        std::string unpack_error;
        if (!GpkgArchive::decompress_gzip_file(packages_gz, packages_txt, &unpack_error)) {
            std::cerr << Color::YELLOW << "W: Failed to unpack Debian Packages index." << Color::RESET << std::endl;
            if (verbose && !unpack_error.empty()) {
                std::cerr << "[DEBUG] Debian Packages unpack error: " << unpack_error << std::endl;
            }
            return false;
        }
    }

    std::vector<std::string> skipped_policy;
    std::vector<PackageMetadata> entries = load_debian_index_entries(packages_txt, verbose, &skipped_policy);
    for (const auto& meta : entries) {
        if (!first_object) merged << ",\n";
        merged << package_metadata_to_json(meta);
        first_object = false;
        ++total_packages;
    }

    if (needs_download) {
        if (have_remote_state) {
            save_debian_packages_cache_state(packages_state, remote_state);
        } else {
            remove(packages_state.c_str());
        }
        std::cout << Color::GREEN << "✓ Updated packages index"
                  << " (" << entries.size() << " packages)" << Color::RESET << std::endl;
    } else {
        std::cout << Color::GREEN << "✓ Reused cached packages index"
                  << " (" << entries.size() << " packages)" << Color::RESET << std::endl;
    }
    return true;
}

std::string get_imported_gpkg_path(const PackageMetadata& meta) {
    std::string base = REPO_CACHE_PATH + "imported/v6/"
        + cache_safe_component(meta.source_kind) + "/"
        + cache_safe_component(meta.name);
    return base + "_" + safe_repo_filename_component(meta.version) + "_" + cache_safe_component(meta.arch) + EXTENSION;
}

std::string get_cached_debian_archive_path(const PackageMetadata& meta) {
    std::string filename = path_basename(meta.filename);
    if (filename.empty()) filename = meta.name + "_" + safe_repo_filename_component(meta.version) + ".deb";
    return REPO_CACHE_PATH + "debian/pool/" + cache_safe_component(meta.name) + "/" + filename;
}

std::string get_debian_package_url(const PackageMetadata& meta) {
    return join_url_path(meta.source_url, meta.filename);
}

bool locate_deb_data_archive(const std::string& directory, std::string& out_path) {
    out_path.clear();
    DIR* dir = opendir(directory.c_str());
    if (!dir) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.rfind("data.tar", 0) != 0) continue;
        out_path = directory + "/" + name;
        closedir(dir);
        return true;
    }

    closedir(dir);
    return false;
}

bool path_has_suffix(const std::string& path, const std::string& suffix) {
    return path.size() >= suffix.size() &&
           path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string lzma_error_string(lzma_ret ret) {
    switch (ret) {
        case LZMA_OK:
            return "ok";
        case LZMA_STREAM_END:
            return "stream end";
        case LZMA_MEM_ERROR:
            return "out of memory";
        case LZMA_FORMAT_ERROR:
            return "input is not a valid .xz/.lzma stream";
        case LZMA_OPTIONS_ERROR:
            return "unsupported compression options";
        case LZMA_DATA_ERROR:
            return "corrupt compressed data";
        case LZMA_BUF_ERROR:
            return "truncated compressed data";
        default:
            return "liblzma error";
    }
}

bool decompress_xz_file(
    const std::string& input_path,
    const std::string& output_path,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    if (is_executable_command_available("xz")) {
        // Use run_command directly; run_command_captured would add
        // a second stdout redirect, clobbering the ">" to output_path.
        std::string cmd = "xz -T0 -d -c " + shell_quote(input_path)
            + " > " + shell_quote(output_path) + " 2>/dev/null";
        int rc = run_command(cmd, false);
        if (rc == 0) return true;
        if (error_out) *error_out = "xz failed to decompress Debian payload";
        return false;
    }

    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        if (error_out) *error_out = "could not open compressed archive";
        return false;
    }

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        if (error_out) *error_out = "could not open decompression target";
        return false;
    }

    lzma_stream stream = LZMA_STREAM_INIT;
    lzma_ret init_ret = lzma_auto_decoder(&stream, UINT64_MAX, LZMA_CONCATENATED);
    if (init_ret != LZMA_OK) {
        if (error_out) *error_out = lzma_error_string(init_ret);
        return false;
    }

    bool success = false;
    bool input_finished = false;
    uint8_t input_buffer[32768];
    uint8_t output_buffer[32768];

    while (true) {
        if (stream.avail_in == 0 && !input_finished) {
            input.read(reinterpret_cast<char*>(input_buffer), sizeof(input_buffer));
            stream.next_in = input_buffer;
            stream.avail_in = static_cast<size_t>(input.gcount());
            if (input.bad()) {
                if (error_out) *error_out = "failed while reading compressed archive";
                break;
            }
            input_finished = input.eof();
        }

        stream.next_out = output_buffer;
        stream.avail_out = sizeof(output_buffer);
        lzma_ret ret = lzma_code(&stream, input_finished ? LZMA_FINISH : LZMA_RUN);

        size_t produced = sizeof(output_buffer) - stream.avail_out;
        if (produced > 0) {
            output.write(reinterpret_cast<const char*>(output_buffer), produced);
            if (!output) {
                if (error_out) *error_out = "failed while writing decompressed archive";
                break;
            }
        }

        if (ret == LZMA_STREAM_END) {
            success = true;
            break;
        }
        if (ret != LZMA_OK) {
            if (error_out) *error_out = lzma_error_string(ret);
            break;
        }
        if (input_finished && stream.avail_in == 0 && produced == 0) {
            if (error_out) *error_out = "truncated compressed archive";
            break;
        }
    }

    lzma_end(&stream);
    return success;
}

bool decompress_gzip_file(
    const std::string& input_path,
    const std::string& output_path,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    gzFile input = gzopen(input_path.c_str(), "rb");
    if (!input) {
        if (error_out) *error_out = "could not open gzip archive";
        return false;
    }

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        gzclose(input);
        if (error_out) *error_out = "could not open decompression target";
        return false;
    }

    char buffer[32768];
    int bytes_read = 0;
    while ((bytes_read = gzread(input, buffer, sizeof(buffer))) > 0) {
        output.write(buffer, bytes_read);
        if (!output) {
            gzclose(input);
            if (error_out) *error_out = "failed while writing decompressed archive";
            return false;
        }
    }

    if (bytes_read < 0) {
        int errnum = Z_OK;
        const char* message = gzerror(input, &errnum);
        gzclose(input);
        if (error_out) {
            *error_out = (message && *message) ? message : "gzip decompression failed";
        }
        return false;
    }

    gzclose(input);
    return true;
}

bool decompress_zstd_file(
    const std::string& input_path,
    const std::string& output_path,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    std::ifstream input(input_path, std::ios::binary);
    if (!input) {
        if (error_out) *error_out = "could not open zstd archive";
        return false;
    }

    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        if (error_out) *error_out = "could not open decompression target";
        return false;
    }

    ZSTD_DStream* stream = ZSTD_createDStream();
    if (!stream) {
        if (error_out) *error_out = "could not allocate zstd decompressor";
        return false;
    }

    size_t init_ret = ZSTD_initDStream(stream);
    if (ZSTD_isError(init_ret)) {
        if (error_out) *error_out = ZSTD_getErrorName(init_ret);
        ZSTD_freeDStream(stream);
        return false;
    }

    char input_buffer[32768];
    char output_buffer[32768];
    size_t last_ret = 1;

    while (input.read(input_buffer, sizeof(input_buffer)) || input.gcount() > 0) {
        ZSTD_inBuffer in_buffer = {
            input_buffer,
            static_cast<size_t>(input.gcount()),
            0
        };

        while (in_buffer.pos < in_buffer.size) {
            ZSTD_outBuffer out_buffer = {output_buffer, sizeof(output_buffer), 0};
            last_ret = ZSTD_decompressStream(stream, &out_buffer, &in_buffer);
            if (ZSTD_isError(last_ret)) {
                if (error_out) *error_out = ZSTD_getErrorName(last_ret);
                ZSTD_freeDStream(stream);
                return false;
            }
            if (out_buffer.pos > 0) {
                output.write(output_buffer, out_buffer.pos);
                if (!output) {
                    if (error_out) *error_out = "failed while writing decompressed archive";
                    ZSTD_freeDStream(stream);
                    return false;
                }
            }
        }
    }

    ZSTD_freeDStream(stream);
    if (!input.eof()) {
        if (error_out) *error_out = "failed while reading compressed archive";
        return false;
    }
    if (last_ret != 0) {
        if (error_out) *error_out = "truncated compressed archive";
        return false;
    }
    return true;
}

bool materialize_deb_payload_tar(
    const std::string& archive_path,
    const std::string& temp_root,
    std::string& tar_path_out,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    tar_path_out = temp_root + "/data.tar";
    if (path_has_suffix(archive_path, ".tar")) {
        std::ifstream input(archive_path, std::ios::binary);
        std::ofstream output(tar_path_out, std::ios::binary);
        if (!input || !output) {
            if (error_out) *error_out = "could not copy uncompressed payload tar";
            return false;
        }

        char buffer[32768];
        while (input.read(buffer, sizeof(buffer)) || input.gcount() > 0) {
            output.write(buffer, input.gcount());
            if (!output) {
                if (error_out) *error_out = "failed while copying payload tar";
                return false;
            }
        }
        if (!input.eof()) {
            if (error_out) *error_out = "failed while reading payload tar";
            return false;
        }
        return true;
    }
    if (path_has_suffix(archive_path, ".tar.gz") || path_has_suffix(archive_path, ".tgz")) {
        return GpkgArchive::decompress_gzip_file(archive_path, tar_path_out, error_out);
    }
    if (path_has_suffix(archive_path, ".tar.xz") || path_has_suffix(archive_path, ".tar.lzma")) {
        return GpkgArchive::decompress_xz_file(archive_path, tar_path_out, error_out);
    }
    if (path_has_suffix(archive_path, ".tar.zst") || path_has_suffix(archive_path, ".tar.zstd")) {
        return GpkgArchive::decompress_zstd_file(archive_path, tar_path_out, error_out);
    }

    if (error_out) *error_out = "unsupported Debian payload compression";
    return false;
}

bool write_imported_control_json(const PackageMetadata& meta, const std::string& control_path) {
    if (!mkdir_parent(control_path)) return false;
    std::ofstream out(control_path);
    if (!out) return false;
    out << package_metadata_to_json(meta) << "\n";
    return true;
}

bool run_quiet_import_command(
    const std::string& cmd,
    const std::string& log_prefix,
    std::string* failure_log_out = nullptr
) {
    if (failure_log_out) failure_log_out->clear();

    CommandCaptureResult result = run_command_captured(cmd, false, log_prefix);
    if (result.exit_code == 0) {
        if (!result.log_path.empty()) unlink(result.log_path.c_str());
        return true;
    }

    if (failure_log_out) *failure_log_out = result.log_path;
    return false;
}

std::string format_import_failure_hint(const std::string& log_path) {
    if (log_path.empty()) return "";
    return " (see " + log_path + ")";
}

bool build_imported_gpkg_archive(
    const PackageMetadata& meta,
    const std::string& payload_root,
    const std::string& output_path,
    bool verbose
) {
    (void)verbose;

    if (access(output_path.c_str(), F_OK) == 0) return true;
    if (!mkdir_parent(output_path)) {
        std::cerr << Color::RED << "E: Failed to create converted package cache directory for "
                  << meta.name << Color::RESET << std::endl;
        return false;
    }

    char temp_template[] = "/tmp/gpkg-import-build-XXXXXX";
    char* temp_dir = mkdtemp(temp_template);
    if (!temp_dir) return false;
    std::string temp_root = temp_dir;
    std::string control_json = temp_root + "/control.json";
    std::string data_tar = temp_root + "/data.tar";
    std::string data_tar_zst = temp_root + "/data.tar.zst";
    std::string final_tar = temp_root + "/package.tar";

    if (!write_imported_control_json(meta, control_json)) {
        std::cerr << Color::RED << "E: Failed to write converted package metadata for "
                  << meta.name << Color::RESET << std::endl;
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    std::string archive_error;
    if (!GpkgArchive::tar_create_from_directory(payload_root, data_tar, &archive_error)) {
        std::cerr << Color::RED << "E: Failed to stage the converted payload for "
                  << meta.name;
        if (!archive_error.empty()) std::cerr << ": " << archive_error;
        std::cerr << Color::RESET << std::endl;
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    if (!GpkgArchive::compress_zstd_file(data_tar, data_tar_zst, 10, &archive_error)) {
        std::cerr << Color::RED << "E: Failed to compress the converted payload for "
                  << meta.name;
        if (!archive_error.empty()) std::cerr << ": " << archive_error;
        std::cerr << Color::RESET << std::endl;
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    std::vector<GpkgArchive::TarSource> top_level_sources = {
        {"control.json", control_json, GpkgArchive::TarEntryType::Regular, 0644, ""},
        {"data.tar.zst", data_tar_zst, GpkgArchive::TarEntryType::Regular, 0644, ""},
    };
    if (!GpkgArchive::tar_create_from_sources(top_level_sources, final_tar, &archive_error)) {
        std::cerr << Color::RED << "E: Failed to assemble the converted package for "
                  << meta.name;
        if (!archive_error.empty()) std::cerr << ": " << archive_error;
        std::cerr << Color::RESET << std::endl;
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    bool ok = GpkgArchive::compress_zstd_file(final_tar, output_path, 10, &archive_error);
    if (!ok) {
        std::cerr << Color::RED << "E: Failed to write the converted package cache entry for "
                  << meta.name;
        if (!archive_error.empty()) std::cerr << ": " << archive_error;
        std::cerr << Color::RESET << std::endl;
    }
    run_command("rm -rf " + shell_quote(temp_root), false);
    return ok;
}

bool convert_debian_archive_to_gpkg(const PackageMetadata& meta, bool verbose, std::string* out_path = nullptr) {
    (void)verbose;

    std::string output_path = get_imported_gpkg_path(meta);
    if (out_path) *out_path = output_path;
    if (access(output_path.c_str(), F_OK) == 0) return true;

    std::string deb_path = get_cached_debian_archive_path(meta);
    if (access(deb_path.c_str(), F_OK) != 0) {
        std::cerr << Color::RED << "E: Missing cached Debian archive for " << meta.name
                  << ": " << deb_path << Color::RESET << std::endl;
        return false;
    }

    char temp_template[] = "/tmp/gpkg-deb-import-XXXXXX";
    char* temp_dir = mkdtemp(temp_template);
    if (!temp_dir) return false;
    std::string temp_root = temp_dir;
    std::string archive_error;
    if (!GpkgArchive::extract_ar_archive_to_directory(deb_path, temp_root, &archive_error)) {
        std::cerr << Color::RED << "E: Failed to unpack the Debian archive for "
                  << meta.name;
        if (!archive_error.empty()) std::cerr << ": " << archive_error;
        std::cerr << Color::RESET << std::endl;
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    std::string data_archive;
    if (!locate_deb_data_archive(temp_root, data_archive)) {
        std::cerr << Color::RED << "E: Could not locate data.tar.* inside " << deb_path << Color::RESET << std::endl;
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    std::string payload_root = temp_root + "/root";
    if (!mkdir_p(payload_root)) {
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    std::string payload_tar;
    std::string materialize_error;
    if (!materialize_deb_payload_tar(data_archive, temp_root, payload_tar, &materialize_error)) {
        std::cerr << Color::RED << "E: Failed to prepare Debian payload for " << meta.name;
        if (!materialize_error.empty()) std::cerr << ": " << materialize_error;
        std::cerr << Color::RESET << std::endl;
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    if (!GpkgArchive::tar_extract_to_directory(payload_tar, payload_root, {}, &archive_error)) {
        std::cerr << Color::RED << "E: Failed to extract the prepared Debian payload tar for "
                  << meta.name;
        if (!archive_error.empty()) std::cerr << ": " << archive_error;
        std::cerr << Color::RESET << std::endl;
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    if (!normalize_imported_payload_layout(payload_root, verbose)) {
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    bool ok = build_imported_gpkg_archive(meta, payload_root, output_path, verbose);
    run_command("rm -rf " + shell_quote(temp_root), false);
    return ok;
}
