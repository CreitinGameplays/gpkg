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

struct DebianImportedIndexCacheState {
    std::string fingerprint;
    size_t package_count = 0;
};

struct DebianCompiledRecordCacheState {
    std::string policy_fingerprint;
    size_t record_count = 0;
};

struct DebianRawContextIndexState {
    std::string compiled_cache_fingerprint;
    size_t raw_package_count = 0;
    size_t import_name_count = 0;
    size_t provider_count = 0;
};

struct DebianCompiledRecordCacheEntry {
    std::string raw_package;
    std::string record_fingerprint;
    std::vector<std::string> provided_symbols;
    bool importable = false;
    std::string skip_reason;
    PackageMetadata meta;
};

struct DebianIncrementalImportResult {
    std::vector<PackageMetadata> entries;
    std::vector<std::string> skipped_policy;
    std::vector<DebianCompiledRecordCacheEntry> compiled_record_entries;
    size_t processed_records = 0;
    bool compiled_record_cache_written = false;
    size_t compiled_record_cache_count = 0;
    size_t reused_records = 0;
    size_t rebuilt_records = 0;
};

struct DebianParsedRecordCacheState {
    std::string schema_fingerprint;
    std::string packages_fingerprint;
    size_t record_count = 0;
};

struct DebianParsedRecordCacheEntry {
    std::string stanza_fingerprint;
    DebianPackageRecord record;
};

struct DebianParsedRecordLoadResult {
    std::vector<DebianPackageRecord> records;
    std::vector<DebianParsedRecordCacheEntry> cache_entries;
    size_t reused_records = 0;
    size_t reparsed_records = 0;
};

struct DebianStanzaSpan {
    std::streamoff offset = 0;
    size_t size = 0;
};

struct ImportedPackageDependencyState {
    std::string name;
    std::string version;
    std::vector<std::string> depends;
    std::vector<std::string> recommends;
    std::vector<std::string> suggests;
    std::vector<std::string> provides;
};

struct DebianDiffIndexEntry {
    std::string hash;
    long size = -1;
    std::string name;
    std::string remote_name;
};

struct DebianPackagesDiffIndex {
    std::string current_hash;
    long current_size = -1;
    std::map<std::string, DebianDiffIndexEntry> history_by_hash;
    std::map<std::string, DebianDiffIndexEntry> patches_by_name;
    std::map<std::string, DebianDiffIndexEntry> downloads_by_name;
};

struct RawDebianAvailabilityResult {
    bool found = false;
    bool installable = false;
    std::string requested_name;
    std::string resolved_name;
    std::string reason;
    PackageMetadata meta;
};

struct RawDebianOffsetIndexEntry {
    std::string key;
    uint64_t offset = 0;
};

struct RawDebianNameIndexEntry {
    std::string key;
    std::vector<std::string> raw_names;
};

struct RawDebianContext {
    bool loaded = false;
    bool available = false;
    std::string problem;
    DebianBackendConfig config;
    ImportPolicy policy;
    std::string compiled_cache_path;
    std::vector<RawDebianOffsetIndexEntry> raw_package_offsets;
    std::vector<RawDebianNameIndexEntry> import_name_to_raw_names;
    std::vector<RawDebianNameIndexEntry> provider_map;
    std::map<std::string, RawDebianAvailabilityResult> raw_exact_cache;
    std::map<std::string, DebianCompiledRecordCacheEntry> raw_compiled_entry_cache;
};

struct DebianSearchPreviewEntry {
    PackageMetadata meta;
    bool installable = false;
    std::string reason;
    std::vector<std::string> raw_names;
};

bool ensure_raw_debian_context_loaded(
    RawDebianContext& context,
    bool verbose,
    std::string* error_out = nullptr
);
bool query_raw_debian_exact_package(
    const std::string& requested_name,
    RawDebianContext& context,
    RawDebianAvailabilityResult& out_result,
    bool verbose,
    std::string* reason_out = nullptr
);
bool query_raw_debian_relation_availability(
    const std::string& requested_name,
    const std::string& op,
    const std::string& version,
    RawDebianContext& context,
    RawDebianAvailabilityResult& out_result,
    bool verbose,
    std::string* reason_out = nullptr
);
void invalidate_debian_search_preview_cache();
template <typename Callback>
bool foreach_debian_search_preview_entry(
    bool verbose,
    Callback callback,
    std::string* error_out = nullptr
);
bool get_debian_search_preview_exact_package(
    const std::string& requested_name,
    DebianSearchPreviewEntry& out_entry,
    bool verbose,
    std::string* error_out = nullptr
);
std::vector<DebianSearchPreviewEntry> build_debian_search_preview_entries(
    const std::string& packages_path,
    const std::vector<PackageMetadata>& installable_entries,
    const std::vector<std::string>& skipped_policy,
    bool verbose
);
std::string raw_debian_effective_import_name(
    const DebianPackageRecord& record,
    const ImportPolicy& policy
);
std::string debian_installed_size_kib_to_bytes_string(const std::string& kib_text);
bool resolve_raw_debian_relation_candidate(
    const std::string& requested_name,
    const std::string& op,
    const std::string& version,
    RawDebianContext& context,
    RawDebianAvailabilityResult& out_result,
    bool verbose,
    std::string* reason_out = nullptr
);
std::string fingerprint_debian_package_record(const DebianPackageRecord& record);
std::string sha256_hex_digest(const std::string& value);
std::string sha256_hex_file(const std::string& path);
std::string debian_cache_fingerprint_component(const std::string& path);
const RawDebianOffsetIndexEntry* raw_debian_offset_index_find(
    const std::vector<RawDebianOffsetIndexEntry>& entries,
    const std::string& key
);
const std::vector<std::string>* raw_debian_name_index_find(
    const std::vector<RawDebianNameIndexEntry>& entries,
    const std::string& key
);
bool load_debian_raw_context_index(
    std::vector<RawDebianOffsetIndexEntry>& raw_package_offsets,
    std::vector<RawDebianNameIndexEntry>& import_name_to_raw_names,
    std::vector<RawDebianNameIndexEntry>& provider_map,
    std::string* error_out = nullptr
);
bool rebuild_debian_raw_context_index(
    bool verbose,
    std::string* error_out = nullptr
);
std::vector<std::string> collect_debian_record_provided_symbols(
    const DebianPackageRecord& record,
    const DebianBackendConfig& config
);
std::vector<std::string> collect_debian_record_dependency_watch_symbols(
    const DebianPackageRecord& record,
    const DebianBackendConfig& config,
    const ImportPolicy& policy
);
bool dependency_watch_symbols_intersect(
    const std::vector<std::string>& watch_symbols,
    const std::set<std::string>& changed_symbols
);
std::string build_debian_compiled_record_cache_policy_fingerprint();
std::string describe_debian_cache_rebuild_reason(
    const std::string& cache_name,
    const std::string& reason
);
std::string build_debian_parsed_record_cache_schema_fingerprint(const DebianBackendConfig& config);
std::string build_debian_parsed_record_packages_fingerprint(const std::string& packages_path);
std::string get_debian_parsed_record_cache_path();
std::string get_debian_parsed_record_state_path();
bool load_current_debian_parsed_record_cache(
    const std::string& packages_path,
    const DebianBackendConfig& config,
    std::vector<DebianPackageRecord>& records,
    std::string* error_out = nullptr
);
bool collect_debian_package_stanza_spans(
    const std::string& packages_path,
    std::vector<DebianStanzaSpan>& spans,
    std::string* error_out = nullptr
);
DebianParsedRecordLoadResult load_debian_package_records_incremental(
    const std::string& packages_path,
    const DebianBackendConfig& config,
    bool verbose
);
bool load_debian_compiled_record_cache(
    const std::string& policy_fingerprint,
    std::map<std::string, DebianCompiledRecordCacheEntry>& entries_by_package,
    std::string* error_out = nullptr
);
bool load_debian_imported_index_cache_state(
    const std::string& path,
    DebianImportedIndexCacheState& state
);
bool load_debian_compiled_record_cache_state(
    const std::string& path,
    DebianCompiledRecordCacheState& state
);
bool debian_imported_index_cache_is_current(
    const std::string& packages_path,
    std::string* fingerprint_out = nullptr
);
bool debian_compiled_record_cache_is_current();
void invalidate_debian_derived_metadata_caches(bool verbose = false);
bool ensure_current_debian_imported_index_cache(
    bool verbose,
    std::string* error_out = nullptr
);

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

std::string sha256_hex_digest(const std::string& value) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    if (!value.empty()) {
        SHA256_Update(&ctx, value.data(), value.size());
    }
    SHA256_Final(hash, &ctx);

    std::ostringstream out;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return out.str();
}

std::string sha256_hex_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";

    SHA256_CTX ctx;
    SHA256_Init(&ctx);

    char buffer[32768];
    while (in.read(buffer, sizeof(buffer)) || in.gcount() > 0) {
        SHA256_Update(&ctx, buffer, static_cast<size_t>(in.gcount()));
    }
    if (in.bad()) return "";

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &ctx);

    std::ostringstream out;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        out << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return out.str();
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

std::string* get_debian_record_field_storage(
    DebianPackageRecord& record,
    std::string& essential_value,
    const std::string& key
) {
    if (key == "Package") return &record.package;
    if (key == "Version") return &record.version;
    if (key == "Architecture") return &record.architecture;
    if (key == "Maintainer") return &record.maintainer;
    if (key == "Section") return &record.section;
    if (key == "Priority") return &record.priority;
    if (key == "Filename") return &record.filename;
    if (key == "SHA256") return &record.sha256;
    if (key == "Size") return &record.size;
    if (key == "Installed-Size") return &record.installed_size;
    if (key == "Depends") return &record.depends_raw;
    if (key == "Pre-Depends") return &record.pre_depends_raw;
    if (key == "Recommends") return &record.recommends_raw;
    if (key == "Suggests") return &record.suggests_raw;
    if (key == "Conflicts") return &record.conflicts_raw;
    if (key == "Provides") return &record.provides_raw;
    if (key == "Replaces") return &record.replaces_raw;
    if (key == "Description") return &record.description;
    if (key == "Essential") return &essential_value;
    return nullptr;
}

bool finalize_debian_package_record(
    DebianPackageRecord current,
    const std::string& essential_value,
    const DebianBackendConfig& config,
    bool verbose,
    DebianPackageRecord& ready
) {
    ready = {};
    if (current.package.empty() || current.version.empty()) return false;

    if (!(current.architecture.empty() ||
          current.architecture == "all" ||
          current.architecture == config.apt_arch)) {
        return false;
    }

    current.description = debian_description_text(current.description);
    current.essential = !essential_value.empty() && trim(essential_value) == "yes";

    if (verbose && current.filename.empty()) {
        std::cout << "[DEBUG] Debian record missing Filename: " << current.package << std::endl;
    }

    ready = std::move(current);
    return true;
}

bool parse_debian_package_record_from_stanza(
    const std::string& stanza_text,
    const DebianBackendConfig& config,
    bool verbose,
    DebianPackageRecord& ready
) {
    DebianPackageRecord current;
    std::string essential_value;
    std::string last_key;
    std::string* last_value = nullptr;

    std::istringstream input(stanza_text);
    std::string raw_line;
    while (std::getline(input, raw_line)) {
        if (!raw_line.empty() && raw_line.back() == '\r') raw_line.pop_back();
        if (trim(raw_line).empty()) continue;

        if (!raw_line.empty() && std::isspace(static_cast<unsigned char>(raw_line[0]))) {
            if (last_value) *last_value += "\n" + raw_line.substr(1);
            continue;
        }

        size_t colon = raw_line.find(':');
        if (colon == std::string::npos) {
            last_key.clear();
            last_value = nullptr;
            continue;
        }

        last_key = raw_line.substr(0, colon);
        last_value = get_debian_record_field_storage(current, essential_value, last_key);
        if (!last_value) continue;
        *last_value = trim(raw_line.substr(colon + 1));
    }

    return finalize_debian_package_record(current, essential_value, config, verbose, ready);
}

template <typename Func>
bool for_each_debian_package_stanza_text(
    const std::string& packages_path,
    Func callback,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    std::ifstream in(packages_path);
    if (!in) {
        if (error_out) *error_out = "failed to open Debian Packages file";
        return false;
    }

    std::string stanza_text;
    std::string raw_line;
    while (std::getline(in, raw_line)) {
        if (!raw_line.empty() && raw_line.back() == '\r') raw_line.pop_back();
        if (trim(raw_line).empty()) {
            if (!stanza_text.empty()) {
                if (!callback(stanza_text)) {
                    if (error_out && error_out->empty()) {
                        *error_out = "Debian stanza iteration stopped early";
                    }
                    return false;
                }
                stanza_text.clear();
            }
            continue;
        }

        if (!stanza_text.empty()) stanza_text += "\n";
        stanza_text += raw_line;
    }

    if (in.bad()) {
        if (error_out) *error_out = "failed while reading Debian Packages file";
        return false;
    }

    if (!stanza_text.empty() && !callback(stanza_text)) {
        if (error_out && error_out->empty()) {
            *error_out = "Debian stanza iteration stopped early";
        }
        return false;
    }
    return true;
}

template <typename Func>
bool for_each_debian_package_record(
    const std::string& packages_path,
    const DebianBackendConfig& config,
    bool verbose,
    Func callback
) {
    std::ifstream f(packages_path);
    if (!f) return false;

    DebianPackageRecord current;
    std::string essential_value;
    std::string last_key;
    std::string* last_value = nullptr;

    auto reset_current = [&]() {
        current = {};
        essential_value.clear();
        last_key.clear();
        last_value = nullptr;
    };

    auto flush_current = [&]() -> bool {
        DebianPackageRecord ready;
        bool valid = finalize_debian_package_record(current, essential_value, config, verbose, ready);
        reset_current();
        if (!valid) return true;
        return callback(ready);
    };

    std::string raw_line;
    while (std::getline(f, raw_line)) {
        if (!raw_line.empty() && raw_line.back() == '\r') raw_line.pop_back();
        if (trim(raw_line).empty()) {
            if (!flush_current()) return false;
            continue;
        }

        if (!raw_line.empty() && std::isspace(static_cast<unsigned char>(raw_line[0]))) {
            if (last_value) *last_value += "\n" + raw_line.substr(1);
            continue;
        }

        size_t colon = raw_line.find(':');
        if (colon == std::string::npos) {
            last_key.clear();
            last_value = nullptr;
            continue;
        }

        last_key = raw_line.substr(0, colon);
        last_value = get_debian_record_field_storage(current, essential_value, last_key);
        if (!last_value) continue;
        *last_value = trim(raw_line.substr(colon + 1));
    }

    return flush_current();
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

void populate_raw_debian_preview_metadata_from_sources(
    const DebianPackageRecord& record,
    const DebianBackendConfig& config,
    const ImportPolicy& policy,
    PackageMetadata& meta
) {
    meta = {};
    meta.name = raw_debian_effective_import_name(record, policy);
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
    meta.section = sanitize_section_name(record.section);
    meta.priority = record.priority;
    meta.size = record.size;
    meta.installed_size_bytes = debian_installed_size_kib_to_bytes_string(record.installed_size);
    meta.installed_from = config.packages_url;
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

std::string debian_search_preview_to_json(const DebianSearchPreviewEntry& entry) {
    std::vector<std::string> fields;
    fields.push_back(json_string_field("package", entry.meta.name));
    fields.push_back(json_string_field("version", entry.meta.version));
    fields.push_back(json_string_field("description", entry.meta.description));
    fields.push_back(json_string_field("source_kind", entry.meta.source_kind));
    fields.push_back(json_string_field("source_url", entry.meta.source_url));
    fields.push_back(json_string_field("debian_package", entry.meta.debian_package));
    fields.push_back(json_string_field("debian_version", entry.meta.debian_version));
    fields.push_back(json_string_field("installable", entry.installable ? "yes" : "no"));
    fields.push_back(json_string_field("reason", entry.reason));
    fields.push_back("\"raw_names\":" + json_array_from_strings(entry.raw_names));

    std::ostringstream out;
    out << "{";
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i > 0) out << ",";
        out << fields[i];
    }
    out << "}";
    return out.str();
}

const uint32_t DEBIAN_COMPILED_CACHE_VERSION = 1;
const char DEBIAN_PARSED_RECORD_CACHE_MAGIC[8] = {'G','P','K','R','A','W','1','\0'};
const char DEBIAN_COMPILED_RECORD_CACHE_MAGIC[8] = {'G','P','K','R','E','C','1','\0'};
const char DEBIAN_RAW_CONTEXT_INDEX_MAGIC[8] = {'G','P','K','R','I','D','1','\0'};
const char DEBIAN_IMPORTED_CACHE_MAGIC[8] = {'G','P','K','I','M','P','1','\0'};
const char DEBIAN_PREVIEW_CACHE_MAGIC[8] = {'G','P','K','P','R','V','1','\0'};

bool should_export_legacy_debian_json_caches() {
    const char* env = getenv("GPKG_EXPORT_LEGACY_DEBIAN_JSON_CACHE");
    if (!env || env[0] == '\0') return false;

    std::string value = ascii_lower_copy(trim(env));
    return value != "0" && value != "false" && value != "no" && value != "off";
}

bool write_binary_exact(std::ostream& out, const void* data, size_t size) {
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

bool read_binary_exact(std::istream& in, void* data, size_t size) {
    in.read(static_cast<char*>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(in);
}

bool write_binary_u8(std::ostream& out, uint8_t value) {
    return write_binary_exact(out, &value, sizeof(value));
}

bool read_binary_u8(std::istream& in, uint8_t& value) {
    return read_binary_exact(in, &value, sizeof(value));
}

bool write_binary_u32(std::ostream& out, uint32_t value) {
    unsigned char bytes[4] = {
        static_cast<unsigned char>(value & 0xffu),
        static_cast<unsigned char>((value >> 8) & 0xffu),
        static_cast<unsigned char>((value >> 16) & 0xffu),
        static_cast<unsigned char>((value >> 24) & 0xffu),
    };
    return write_binary_exact(out, bytes, sizeof(bytes));
}

bool read_binary_u32(std::istream& in, uint32_t& value) {
    unsigned char bytes[4] = {};
    if (!read_binary_exact(in, bytes, sizeof(bytes))) return false;
    value =
        static_cast<uint32_t>(bytes[0]) |
        (static_cast<uint32_t>(bytes[1]) << 8) |
        (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
    return true;
}

bool write_binary_u64(std::ostream& out, uint64_t value) {
    unsigned char bytes[8] = {
        static_cast<unsigned char>(value & 0xffu),
        static_cast<unsigned char>((value >> 8) & 0xffu),
        static_cast<unsigned char>((value >> 16) & 0xffu),
        static_cast<unsigned char>((value >> 24) & 0xffu),
        static_cast<unsigned char>((value >> 32) & 0xffu),
        static_cast<unsigned char>((value >> 40) & 0xffu),
        static_cast<unsigned char>((value >> 48) & 0xffu),
        static_cast<unsigned char>((value >> 56) & 0xffu),
    };
    return write_binary_exact(out, bytes, sizeof(bytes));
}

bool read_binary_u64(std::istream& in, uint64_t& value) {
    unsigned char bytes[8] = {};
    if (!read_binary_exact(in, bytes, sizeof(bytes))) return false;
    value =
        static_cast<uint64_t>(bytes[0]) |
        (static_cast<uint64_t>(bytes[1]) << 8) |
        (static_cast<uint64_t>(bytes[2]) << 16) |
        (static_cast<uint64_t>(bytes[3]) << 24) |
        (static_cast<uint64_t>(bytes[4]) << 32) |
        (static_cast<uint64_t>(bytes[5]) << 40) |
        (static_cast<uint64_t>(bytes[6]) << 48) |
        (static_cast<uint64_t>(bytes[7]) << 56);
    return true;
}

bool write_binary_string(std::ostream& out, const std::string& value) {
    if (value.size() > std::numeric_limits<uint32_t>::max()) return false;
    if (!write_binary_u32(out, static_cast<uint32_t>(value.size()))) return false;
    if (value.empty()) return true;
    return write_binary_exact(out, value.data(), value.size());
}

bool read_binary_string(std::istream& in, std::string& value) {
    value.clear();
    uint32_t size = 0;
    if (!read_binary_u32(in, size)) return false;
    if (size == 0) return true;
    value.resize(size);
    return read_binary_exact(in, &value[0], size);
}

bool write_binary_string_vector(std::ostream& out, const std::vector<std::string>& values) {
    if (values.size() > std::numeric_limits<uint32_t>::max()) return false;
    if (!write_binary_u32(out, static_cast<uint32_t>(values.size()))) return false;
    for (const auto& value : values) {
        if (!write_binary_string(out, value)) return false;
    }
    return true;
}

bool read_binary_string_vector(std::istream& in, std::vector<std::string>& values) {
    values.clear();
    uint32_t count = 0;
    if (!read_binary_u32(in, count)) return false;
    values.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        std::string value;
        if (!read_binary_string(in, value)) return false;
        values.push_back(std::move(value));
    }
    return true;
}

bool write_package_metadata_binary(std::ostream& out, const PackageMetadata& meta) {
    return
        write_binary_string(out, meta.name) &&
        write_binary_string(out, meta.version) &&
        write_binary_string(out, meta.arch) &&
        write_binary_string(out, meta.description) &&
        write_binary_string(out, meta.maintainer) &&
        write_binary_string(out, meta.section) &&
        write_binary_string(out, meta.priority) &&
        write_binary_string(out, meta.filename) &&
        write_binary_string(out, meta.sha256) &&
        write_binary_string(out, meta.sha512) &&
        write_binary_string(out, meta.source_url) &&
        write_binary_string(out, meta.source_kind) &&
        write_binary_string(out, meta.debian_package) &&
        write_binary_string(out, meta.debian_version) &&
        write_binary_string(out, meta.package_scope) &&
        write_binary_string(out, meta.installed_from) &&
        write_binary_string(out, meta.size) &&
        write_binary_string(out, meta.installed_size_bytes) &&
        write_binary_string_vector(out, meta.depends) &&
        write_binary_string_vector(out, meta.recommends) &&
        write_binary_string_vector(out, meta.suggests) &&
        write_binary_string_vector(out, meta.conflicts) &&
        write_binary_string_vector(out, meta.provides) &&
        write_binary_string_vector(out, meta.replaces);
}

bool read_package_metadata_binary(std::istream& in, PackageMetadata& meta) {
    meta = {};
    return
        read_binary_string(in, meta.name) &&
        read_binary_string(in, meta.version) &&
        read_binary_string(in, meta.arch) &&
        read_binary_string(in, meta.description) &&
        read_binary_string(in, meta.maintainer) &&
        read_binary_string(in, meta.section) &&
        read_binary_string(in, meta.priority) &&
        read_binary_string(in, meta.filename) &&
        read_binary_string(in, meta.sha256) &&
        read_binary_string(in, meta.sha512) &&
        read_binary_string(in, meta.source_url) &&
        read_binary_string(in, meta.source_kind) &&
        read_binary_string(in, meta.debian_package) &&
        read_binary_string(in, meta.debian_version) &&
        read_binary_string(in, meta.package_scope) &&
        read_binary_string(in, meta.installed_from) &&
        read_binary_string(in, meta.size) &&
        read_binary_string(in, meta.installed_size_bytes) &&
        read_binary_string_vector(in, meta.depends) &&
        read_binary_string_vector(in, meta.recommends) &&
        read_binary_string_vector(in, meta.suggests) &&
        read_binary_string_vector(in, meta.conflicts) &&
        read_binary_string_vector(in, meta.provides) &&
        read_binary_string_vector(in, meta.replaces);
}

bool write_debian_package_record_binary(std::ostream& out, const DebianPackageRecord& record) {
    return
        write_binary_string(out, record.package) &&
        write_binary_string(out, record.version) &&
        write_binary_string(out, record.architecture) &&
        write_binary_string(out, record.maintainer) &&
        write_binary_string(out, record.section) &&
        write_binary_string(out, record.priority) &&
        write_binary_string(out, record.filename) &&
        write_binary_string(out, record.sha256) &&
        write_binary_string(out, record.size) &&
        write_binary_string(out, record.installed_size) &&
        write_binary_string(out, record.depends_raw) &&
        write_binary_string(out, record.pre_depends_raw) &&
        write_binary_string(out, record.recommends_raw) &&
        write_binary_string(out, record.suggests_raw) &&
        write_binary_string(out, record.conflicts_raw) &&
        write_binary_string(out, record.provides_raw) &&
        write_binary_string(out, record.replaces_raw) &&
        write_binary_string(out, record.description) &&
        write_binary_u8(out, record.essential ? 1 : 0);
}

bool read_debian_package_record_binary(std::istream& in, DebianPackageRecord& record) {
    record = {};
    uint8_t essential = 0;
    return
        read_binary_string(in, record.package) &&
        read_binary_string(in, record.version) &&
        read_binary_string(in, record.architecture) &&
        read_binary_string(in, record.maintainer) &&
        read_binary_string(in, record.section) &&
        read_binary_string(in, record.priority) &&
        read_binary_string(in, record.filename) &&
        read_binary_string(in, record.sha256) &&
        read_binary_string(in, record.size) &&
        read_binary_string(in, record.installed_size) &&
        read_binary_string(in, record.depends_raw) &&
        read_binary_string(in, record.pre_depends_raw) &&
        read_binary_string(in, record.recommends_raw) &&
        read_binary_string(in, record.suggests_raw) &&
        read_binary_string(in, record.conflicts_raw) &&
        read_binary_string(in, record.provides_raw) &&
        read_binary_string(in, record.replaces_raw) &&
        read_binary_string(in, record.description) &&
        read_binary_u8(in, essential) &&
        ((record.essential = essential != 0), true);
}

bool write_debian_parsed_record_cache_entry_binary(
    std::ostream& out,
    const DebianParsedRecordCacheEntry& entry
) {
    return
        write_binary_string(out, entry.stanza_fingerprint) &&
        write_debian_package_record_binary(out, entry.record);
}

bool read_debian_parsed_record_cache_entry_binary(
    std::istream& in,
    DebianParsedRecordCacheEntry& entry
) {
    entry = {};
    return
        read_binary_string(in, entry.stanza_fingerprint) &&
        read_debian_package_record_binary(in, entry.record);
}

bool write_debian_search_preview_entry_binary(
    std::ostream& out,
    const DebianSearchPreviewEntry& entry
) {
    return
        write_package_metadata_binary(out, entry.meta) &&
        write_binary_u8(out, entry.installable ? 1 : 0) &&
        write_binary_string(out, entry.reason) &&
        write_binary_string_vector(out, entry.raw_names);
}

bool read_debian_search_preview_entry_binary(
    std::istream& in,
    DebianSearchPreviewEntry& entry
) {
    entry = {};
    uint8_t installable = 0;
    if (!read_package_metadata_binary(in, entry.meta)) return false;
    if (!read_binary_u8(in, installable)) return false;
    if (!read_binary_string(in, entry.reason)) return false;
    if (!read_binary_string_vector(in, entry.raw_names)) return false;
    entry.installable = installable != 0;
    return true;
}

bool write_debian_compiled_record_cache_entry_binary(
    std::ostream& out,
    const DebianCompiledRecordCacheEntry& entry
) {
    return
        write_binary_string(out, entry.raw_package) &&
        write_binary_string(out, entry.record_fingerprint) &&
        write_binary_string_vector(out, entry.provided_symbols) &&
        write_binary_u8(out, entry.importable ? 1 : 0) &&
        write_binary_string(out, entry.skip_reason) &&
        write_package_metadata_binary(out, entry.meta);
}

bool read_debian_compiled_record_cache_entry_binary(
    std::istream& in,
    DebianCompiledRecordCacheEntry& entry
) {
    entry = {};
    uint8_t importable = 0;
    if (!read_binary_string(in, entry.raw_package)) return false;
    if (!read_binary_string(in, entry.record_fingerprint)) return false;
    if (!read_binary_string_vector(in, entry.provided_symbols)) return false;
    if (!read_binary_u8(in, importable)) return false;
    if (!read_binary_string(in, entry.skip_reason)) return false;
    if (!read_package_metadata_binary(in, entry.meta)) return false;
    entry.importable = importable != 0;
    return true;
}

bool write_raw_debian_offset_index_entry_binary(
    std::ostream& out,
    const RawDebianOffsetIndexEntry& entry
) {
    return
        write_binary_string(out, entry.key) &&
        write_binary_u64(out, entry.offset);
}

bool read_raw_debian_offset_index_entry_binary(
    std::istream& in,
    RawDebianOffsetIndexEntry& entry
) {
    entry = {};
    return
        read_binary_string(in, entry.key) &&
        read_binary_u64(in, entry.offset);
}

bool write_raw_debian_name_index_entry_binary(
    std::ostream& out,
    const RawDebianNameIndexEntry& entry
) {
    return
        write_binary_string(out, entry.key) &&
        write_binary_string_vector(out, entry.raw_names);
}

bool read_raw_debian_name_index_entry_binary(
    std::istream& in,
    RawDebianNameIndexEntry& entry
) {
    entry = {};
    return
        read_binary_string(in, entry.key) &&
        read_binary_string_vector(in, entry.raw_names);
}

bool write_binary_cache_header(
    std::ostream& out,
    const char magic[8],
    uint32_t entry_count
) {
    return
        write_binary_exact(out, magic, 8) &&
        write_binary_u32(out, DEBIAN_COMPILED_CACHE_VERSION) &&
        write_binary_u32(out, entry_count);
}

bool read_binary_cache_header(
    std::istream& in,
    const char expected_magic[8],
    uint32_t& entry_count,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    char magic[8] = {};
    if (!read_binary_exact(in, magic, sizeof(magic))) {
        if (error_out) *error_out = "failed to read cache header";
        return false;
    }
    if (std::memcmp(magic, expected_magic, 8) != 0) {
        if (error_out) *error_out = "cache header magic does not match";
        return false;
    }

    uint32_t version = 0;
    if (!read_binary_u32(in, version)) {
        if (error_out) *error_out = "failed to read cache version";
        return false;
    }
    if (version != DEBIAN_COMPILED_CACHE_VERSION) {
        if (error_out) *error_out = "cache version is unsupported";
        return false;
    }

    if (!read_binary_u32(in, entry_count)) {
        if (error_out) *error_out = "failed to read cache entry count";
        return false;
    }

    return true;
}

template <typename Entry, typename Reader, typename Callback>
bool foreach_debian_binary_cache_entry(
    const std::string& path,
    const char expected_magic[8],
    Reader reader,
    Callback callback,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error_out) *error_out = "failed to open binary cache";
        return false;
    }

    uint32_t entry_count = 0;
    if (!read_binary_cache_header(in, expected_magic, entry_count, error_out)) return false;

    for (uint32_t index = 0; index < entry_count; ++index) {
        Entry entry;
        if (!reader(in, entry)) {
            if (error_out) {
                *error_out = "failed to decode cache entry " + std::to_string(index + 1);
            }
            return false;
        }
        if (!callback(entry)) break;
    }

    return true;
}

template <typename Entry, typename Writer>
bool write_debian_binary_cache(
    const std::string& path,
    const char magic[8],
    const std::vector<Entry>& entries,
    Writer writer,
    std::string* error_out = nullptr
) {
    if (error_out) *error_out = "";

    if (!mkdir_parent(path)) {
        if (error_out) *error_out = "failed to create binary cache directory";
        return false;
    }

    std::string temp_path = path + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error_out) *error_out = "failed to open binary cache for writing";
        return false;
    }

    if (entries.size() > std::numeric_limits<uint32_t>::max()) {
        out.close();
        remove(temp_path.c_str());
        if (error_out) *error_out = "binary cache is too large";
        return false;
    }

    if (!write_binary_cache_header(out, magic, static_cast<uint32_t>(entries.size()))) {
        out.close();
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to write binary cache header";
        return false;
    }

    for (size_t i = 0; i < entries.size(); ++i) {
        if (!writer(out, entries[i])) {
            out.close();
            remove(temp_path.c_str());
            if (error_out) {
                *error_out = "failed to encode binary cache entry " + std::to_string(i + 1);
            }
            return false;
        }
    }

    out.close();
    if (!out) {
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to flush binary cache";
        return false;
    }

    if (rename(temp_path.c_str(), path.c_str()) != 0) {
        remove(temp_path.c_str());
        if (error_out) *error_out = strerror(errno);
        return false;
    }

    return true;
}

bool remove_optional_cache_export(const std::string& path) {
    if (access(path.c_str(), F_OK) != 0) return true;
    return remove(path.c_str()) == 0;
}

std::vector<DebianPackageRecord> parse_debian_packages_file(
    const std::string& packages_path,
    const DebianBackendConfig& config,
    bool verbose
) {
    std::vector<DebianPackageRecord> parsed;
    std::string cache_error;
    if (load_current_debian_parsed_record_cache(packages_path, config, parsed, &cache_error)) {
        VLOG(verbose, "Loaded " << parsed.size()
             << " Debian package records from the parsed-record cache.");
        return parsed;
    }

    DebianParsedRecordLoadResult result =
        load_debian_package_records_incremental(packages_path, config, verbose);
    if (verbose) {
        std::cout << "[DEBUG] Debian parse cache reuse: "
                  << result.reused_records << " reused, "
                  << result.reparsed_records << " reparsed."
                  << std::endl;
    }
    return result.records;
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

void append_debian_record_providers_to_map(
    const DebianPackageRecord& record,
    const std::string& apt_arch,
    std::map<std::string, std::vector<std::string>>& providers
) {
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

void append_debian_record_to_compact_availability_index_builder(
    const DebianPackageRecord& record,
    const std::string& apt_arch,
    std::vector<std::string>& available_packages,
    std::vector<std::pair<std::string, std::string>>& provider_pairs
) {
    available_packages.push_back(record.package);

    std::vector<std::string> provided = normalize_relation_field_value(record.provides_raw, apt_arch);
    append_debian_t64_legacy_provides(provided, record.package, record.version);
    for (const auto& capability : provided) {
        RelationAtom atom = normalize_relation_atom(capability, apt_arch);
        if (!atom.valid) continue;
        provider_pairs.push_back({atom.name, record.package});
    }
}

CompactPackageAvailabilityIndex finalize_compact_package_availability_index(
    std::vector<std::string> available_packages,
    std::vector<std::pair<std::string, std::string>> provider_pairs
) {
    std::sort(available_packages.begin(), available_packages.end());
    available_packages.erase(
        std::unique(available_packages.begin(), available_packages.end()),
        available_packages.end()
    );

    std::sort(provider_pairs.begin(), provider_pairs.end(),
        [](const std::pair<std::string, std::string>& lhs,
           const std::pair<std::string, std::string>& rhs) {
            if (lhs.first != rhs.first) return lhs.first < rhs.first;
            return lhs.second < rhs.second;
        });

    CompactPackageAvailabilityIndex index;
    index.available_packages = std::move(available_packages);
    for (const auto& entry : provider_pairs) {
        if (index.provider_map.empty() || index.provider_map.back().first != entry.first) {
            index.provider_map.push_back({entry.first, {}});
        }

        auto& providers = index.provider_map.back().second;
        if (providers.empty() || providers.back() != entry.second) {
            providers.push_back(entry.second);
        }
    }

    return index;
}

std::vector<std::string> apply_dependency_removals(
    const std::vector<std::string>& dependencies,
    const PackageOverridePolicy& package_override
) {
    std::vector<std::string> filtered = unique_string_list(dependencies);
    if (package_override.depends_remove.empty()) return filtered;

    std::vector<std::string> result;
    for (const auto& dep : filtered) {
        auto matches_override = [&](const std::string& candidate) {
            std::string trimmed = trim(candidate);
            if (trimmed.empty()) return false;
            return std::find(
                       package_override.depends_remove.begin(),
                       package_override.depends_remove.end(),
                       trimmed
                   ) != package_override.depends_remove.end();
        };

        if (matches_override(dep)) continue;

        RelationAtom parsed = normalize_relation_atom(dep, "any");
        if (parsed.valid &&
            (matches_override(parsed.name) || matches_override(parsed.normalized))) {
            continue;
        }

        result.push_back(dep);
    }
    return result;
}

bool dependency_relation_matches_override_removal(
    const std::string& relation_text,
    const PackageOverridePolicy& package_override,
    const std::string& apt_arch,
    const ImportPolicy* policy = nullptr
) {
    if (package_override.depends_remove.empty()) return false;

    auto matches_override = [&](const std::string& candidate) {
        std::string trimmed = trim(candidate);
        if (trimmed.empty()) return false;
        return std::find(
                   package_override.depends_remove.begin(),
                   package_override.depends_remove.end(),
                   trimmed
               ) != package_override.depends_remove.end();
    };

    std::string trimmed_relation = trim(relation_text);
    if (trimmed_relation.empty()) return false;
    if (matches_override(trimmed_relation)) return true;

    RelationAtom parsed = normalize_relation_atom(trimmed_relation, apt_arch);
    if (!parsed.valid) return false;
    if (matches_override(parsed.name) || matches_override(parsed.normalized)) return true;

    if (!policy) return false;

    std::string rewritten_name = apply_dependency_rewrite_name(
        parsed.name,
        policy->dependency_rewrites,
        &policy->package_aliases
    );
    if (rewritten_name.empty() || rewritten_name == parsed.name) return false;
    if (matches_override(rewritten_name)) return true;

    std::string rewritten_normalized = parsed.op.empty()
        ? rewritten_name
        : rewritten_name + " (" + parsed.op + " " + parsed.version + ")";
    return matches_override(rewritten_normalized);
}

std::string apply_dependency_removals_to_raw_value(
    const std::string& raw_value,
    const PackageOverridePolicy& package_override,
    const std::string& apt_arch,
    const ImportPolicy& policy
) {
    if (raw_value.empty() || package_override.depends_remove.empty()) return raw_value;

    std::vector<std::string> filtered_groups;
    for (const auto& group : split_top_level_text(raw_value, ',')) {
        std::vector<std::string> kept_alternatives;
        for (const auto& alternative : split_top_level_text(group, '|')) {
            if (dependency_relation_matches_override_removal(
                    alternative,
                    package_override,
                    apt_arch,
                    &policy
                )) {
                continue;
            }
            kept_alternatives.push_back(trim(alternative));
        }

        if (kept_alternatives.empty()) continue;
        filtered_groups.push_back(join_strings(kept_alternatives, " | "));
    }

    return join_strings(filtered_groups, ", ");
}

bool debian_dependency_version_satisfies(
    const std::string& current_ver,
    const std::string& op,
    const std::string& req_ver
) {
    if (op.empty()) return true;

    int cmp = compare_versions(current_ver, req_ver);
    if (op == ">>" || op == ">") return cmp > 0;
    if (op == "<<") return cmp < 0;
    if (op == ">=") return cmp >= 0;
    if (op == "<=") return cmp <= 0;
    if (op == "=" || op == "==") return cmp == 0;
    return false;
}

bool debian_meta_satisfies_required_dependency(
    const PackageMetadata& meta,
    const RelationAtom& dep
) {
    if (dep.name.empty()) return false;

    if (meta.name == dep.name &&
        debian_dependency_version_satisfies(meta.version, dep.op, dep.version)) {
        return true;
    }

    for (const auto& provide : meta.provides) {
        RelationAtom provided = normalize_relation_atom(provide, "any");
        if (!provided.valid) continue;
        if (provided.name != dep.name) continue;
        if (dep.op.empty()) return true;
        if (!provided.version.empty() &&
            debian_dependency_version_satisfies(provided.version, dep.op, dep.version)) {
            return true;
        }
        if (provided.version.empty() &&
            debian_dependency_version_satisfies(meta.version, dep.op, dep.version)) {
            return true;
        }
    }

    return false;
}

std::map<std::string, std::vector<std::string>> build_imported_dependency_index(
    const std::map<std::string, PackageMetadata>& selected
) {
    std::map<std::string, std::vector<std::string>> index;

    auto append = [&](const std::string& provided_name, const std::string& package_name) {
        if (provided_name.empty() || package_name.empty()) return;
        auto& entry = index[provided_name];
        if (std::find(entry.begin(), entry.end(), package_name) == entry.end()) {
            entry.push_back(package_name);
        }
    };

    for (const auto& entry : selected) {
        append(entry.first, entry.first);
        for (const auto& provide : entry.second.provides) {
            RelationAtom provided = normalize_relation_atom(provide, "any");
            if (!provided.valid) continue;
            append(provided.name, entry.first);
        }
    }

    return index;
}

ImportedPackageDependencyState build_imported_dependency_state(
    const PackageMetadata& meta
) {
    ImportedPackageDependencyState state;
    state.name = meta.name;
    state.version = meta.version;
    state.depends = meta.depends;
    state.recommends = meta.recommends;
    state.suggests = meta.suggests;
    state.provides = meta.provides;
    return state;
}

std::map<std::string, std::vector<std::string>> build_imported_dependency_index(
    const std::map<std::string, ImportedPackageDependencyState>& selected
) {
    std::map<std::string, std::vector<std::string>> index;

    auto append = [&](const std::string& provided_name, const std::string& package_name) {
        if (provided_name.empty() || package_name.empty()) return;
        auto& entry = index[provided_name];
        if (std::find(entry.begin(), entry.end(), package_name) == entry.end()) {
            entry.push_back(package_name);
        }
    };

    for (const auto& entry : selected) {
        append(entry.first, entry.first);
        for (const auto& provide : entry.second.provides) {
            RelationAtom provided = normalize_relation_atom(provide, "any");
            if (!provided.valid) continue;
            append(provided.name, entry.first);
        }
    }

    return index;
}

bool imported_dependency_relation_is_available(
    const std::string& dep_str,
    const std::map<std::string, PackageMetadata>& selected,
    const std::map<std::string, std::vector<std::string>>& dependency_index
);

std::vector<std::string> find_missing_imported_required_dependencies(
    const PackageMetadata& meta,
    const std::map<std::string, PackageMetadata>& selected,
    const std::map<std::string, std::vector<std::string>>& dependency_index
) {
    std::vector<std::string> missing;

    for (const auto& dep_str : meta.depends) {
        if (!imported_dependency_relation_is_available(dep_str, selected, dependency_index)) {
            missing.push_back(dep_str);
        }
    }

    return missing;
}

bool imported_dependency_state_satisfies_required_dependency(
    const ImportedPackageDependencyState& meta,
    const RelationAtom& dep
) {
    if (dep.name.empty()) return false;

    if (meta.name == dep.name &&
        debian_dependency_version_satisfies(meta.version, dep.op, dep.version)) {
        return true;
    }

    for (const auto& provide : meta.provides) {
        RelationAtom provided = normalize_relation_atom(provide, "any");
        if (!provided.valid) continue;
        if (provided.name != dep.name) continue;
        if (dep.op.empty()) return true;
        if (!provided.version.empty() &&
            debian_dependency_version_satisfies(provided.version, dep.op, dep.version)) {
            return true;
        }
        if (provided.version.empty() &&
            debian_dependency_version_satisfies(meta.version, dep.op, dep.version)) {
            return true;
        }
    }

    return false;
}

bool imported_dependency_relation_is_available(
    const std::string& dep_str,
    const std::map<std::string, PackageMetadata>& selected,
    const std::map<std::string, std::vector<std::string>>& dependency_index
) {
    RelationAtom dep = normalize_relation_atom(dep_str, "any");
    if (!dep.valid) return false;

    if (is_system_provided(dep.name, dep.op, dep.version)) return true;

    auto candidate_it = dependency_index.find(dep.name);
    if (candidate_it == dependency_index.end()) return false;

    for (const auto& candidate_name : candidate_it->second) {
        auto meta_it = selected.find(candidate_name);
        if (meta_it == selected.end()) continue;
        if (debian_meta_satisfies_required_dependency(meta_it->second, dep)) return true;
    }

    return false;
}

bool imported_dependency_relation_is_available(
    const std::string& dep_str,
    const std::map<std::string, ImportedPackageDependencyState>& selected,
    const std::map<std::string, std::vector<std::string>>& dependency_index
) {
    RelationAtom dep = normalize_relation_atom(dep_str, "any");
    if (!dep.valid) return false;

    if (is_system_provided(dep.name, dep.op, dep.version)) return true;

    auto candidate_it = dependency_index.find(dep.name);
    if (candidate_it == dependency_index.end()) return false;

    for (const auto& candidate_name : candidate_it->second) {
        auto meta_it = selected.find(candidate_name);
        if (meta_it == selected.end()) continue;
        if (imported_dependency_state_satisfies_required_dependency(meta_it->second, dep)) return true;
    }

    return false;
}

void prune_imported_packages_with_missing_required_dependencies(
    std::map<std::string, PackageMetadata>& selected,
    std::vector<std::string>* skipped_policy
) {
    while (true) {
        auto dependency_index = build_imported_dependency_index(selected);
        std::vector<std::pair<std::string, std::vector<std::string>>> removals;

        for (const auto& entry : selected) {
            std::vector<std::string> missing = find_missing_imported_required_dependencies(
                entry.second,
                selected,
                dependency_index
            );
            if (!missing.empty()) {
                removals.push_back({entry.first, missing});
            }
        }

        if (removals.empty()) break;

        for (const auto& removal : removals) {
            selected.erase(removal.first);
            if (skipped_policy) {
                skipped_policy->push_back(
                    removal.first + ": required dependency missing from imported set: " +
                    join_strings(removal.second)
                );
            }
        }
    }
}

void prune_imported_optional_dependencies(
    std::map<std::string, PackageMetadata>& selected
) {
    auto dependency_index = build_imported_dependency_index(selected);

    auto filter_relations = [&](std::vector<std::string>& relations) {
        std::vector<std::string> filtered;
        filtered.reserve(relations.size());
        for (const auto& relation : relations) {
            if (!imported_dependency_relation_is_available(relation, selected, dependency_index)) continue;
            filtered.push_back(relation);
        }
        relations.swap(filtered);
    };

    for (auto& entry : selected) {
        filter_relations(entry.second.recommends);
        filter_relations(entry.second.suggests);
    }
}

void prune_imported_packages_with_missing_required_dependencies(
    std::map<std::string, ImportedPackageDependencyState>& selected,
    std::vector<std::string>* skipped_policy
) {
    while (true) {
        auto dependency_index = build_imported_dependency_index(selected);
        std::vector<std::pair<std::string, std::vector<std::string>>> removals;

        for (const auto& entry : selected) {
            std::vector<std::string> missing;
            for (const auto& dep_str : entry.second.depends) {
                if (!imported_dependency_relation_is_available(dep_str, selected, dependency_index)) {
                    missing.push_back(dep_str);
                }
            }
            if (!missing.empty()) removals.push_back({entry.first, missing});
        }

        if (removals.empty()) break;

        for (const auto& removal : removals) {
            selected.erase(removal.first);
            if (skipped_policy) {
                skipped_policy->push_back(
                    removal.first + ": required dependency missing from imported set: " +
                    join_strings(removal.second)
                );
            }
        }
    }
}

void prune_imported_optional_dependencies(
    std::map<std::string, ImportedPackageDependencyState>& selected
) {
    auto dependency_index = build_imported_dependency_index(selected);

    auto filter_relations = [&](std::vector<std::string>& relations) {
        std::vector<std::string> filtered;
        filtered.reserve(relations.size());
        for (const auto& relation : relations) {
            if (!imported_dependency_relation_is_available(relation, selected, dependency_index)) continue;
            filtered.push_back(relation);
        }
        relations.swap(filtered);
    };

    for (auto& entry : selected) {
        filter_relations(entry.second.recommends);
        filter_relations(entry.second.suggests);
    }
}

bool build_debian_package_metadata(
    const DebianPackageRecord& record,
    const DebianBackendConfig& config,
    const ImportPolicy& policy,
    const std::set<std::string>& available_packages,
    const std::map<std::string, std::vector<std::string>>& provider_map,
    const std::vector<std::string>& system_drop_patterns,
    PackageMetadata& meta,
    std::string* skip_reason_out = nullptr
) {
    if (skip_reason_out) skip_reason_out->clear();
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
    required_dependency_text = apply_dependency_removals_to_raw_value(
        required_dependency_text,
        package_override,
        config.apt_arch,
        policy
    );

    std::vector<std::string> unresolved_required_groups;
    std::vector<std::string> depends = normalize_dependency_relation_value(
        required_dependency_text,
        record.package,
        config.apt_arch,
        false,
        policy,
        available_packages,
        provider_map,
        system_drop_patterns,
        &unresolved_required_groups
    );
    if (!unresolved_required_groups.empty()) {
        if (skip_reason_out) {
            std::ostringstream reason;
            reason << "unresolved required dependency group(s): ";
            for (size_t i = 0; i < unresolved_required_groups.size(); ++i) {
                if (i > 0) reason << ", ";
                reason << unresolved_required_groups[i];
            }
            *skip_reason_out = reason.str();
        }
        return false;
    }
    for (const auto& dep : package_override.depends_add) depends.push_back(dep);
    depends = apply_dependency_removals(depends, package_override);
    std::vector<std::string> recommends = apply_dependency_removals(
        normalize_dependency_relation_value(
            apply_dependency_removals_to_raw_value(
                record.recommends_raw,
                package_override,
                config.apt_arch,
                policy
            ),
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
            apply_dependency_removals_to_raw_value(
                record.suggests_raw,
                package_override,
                config.apt_arch,
                policy
            ),
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
    return true;
}

bool build_debian_package_metadata(
    const DebianPackageRecord& record,
    const DebianBackendConfig& config,
    const ImportPolicy& policy,
    const std::vector<std::string>& available_packages,
    const std::vector<std::pair<std::string, std::vector<std::string>>>& provider_map,
    const std::vector<std::string>& system_drop_patterns,
    PackageMetadata& meta,
    std::string* skip_reason_out = nullptr
) {
    if (skip_reason_out) skip_reason_out->clear();
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
    required_dependency_text = apply_dependency_removals_to_raw_value(
        required_dependency_text,
        package_override,
        config.apt_arch,
        policy
    );

    std::vector<std::string> unresolved_required_groups;
    std::vector<std::string> depends = normalize_dependency_relation_value(
        required_dependency_text,
        record.package,
        config.apt_arch,
        false,
        policy,
        available_packages,
        provider_map,
        system_drop_patterns,
        &unresolved_required_groups
    );
    if (!unresolved_required_groups.empty()) {
        if (skip_reason_out) {
            std::ostringstream reason;
            reason << "unresolved required dependency group(s): ";
            for (size_t i = 0; i < unresolved_required_groups.size(); ++i) {
                if (i > 0) reason << ", ";
                reason << unresolved_required_groups[i];
            }
            *skip_reason_out = reason.str();
        }
        return false;
    }
    for (const auto& dep : package_override.depends_add) depends.push_back(dep);
    depends = apply_dependency_removals(depends, package_override);
    std::vector<std::string> recommends = apply_dependency_removals(
        normalize_dependency_relation_value(
            apply_dependency_removals_to_raw_value(
                record.recommends_raw,
                package_override,
                config.apt_arch,
                policy
            ),
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
            apply_dependency_removals_to_raw_value(
                record.suggests_raw,
                package_override,
                config.apt_arch,
                policy
            ),
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
    return true;
}

std::vector<PackageMetadata> load_debian_index_entries_from_records(
    const std::vector<DebianPackageRecord>& records,
    const DebianBackendConfig& config,
    const ImportPolicy& policy,
    bool verbose,
    std::vector<std::string>* skipped_policy = nullptr
) {
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
            if (matches_any_pattern(record.package, policy.skip_packages)) {
                if (skipped_policy) skipped.push_back(record.package + ": blocked by policy");
                continue;
            }

            PackageMetadata meta;
            std::string skip_reason;
            if (!build_debian_package_metadata(
                record,
                config,
                policy,
                available_packages,
                provider_map,
                system_drop_patterns,
                meta,
                &skip_reason
            )) {
                if (skipped_policy) {
                    if (skip_reason.empty()) skip_reason = "unresolved required dependencies";
                    skipped.push_back(record.package + ": " + skip_reason);
                }
                continue;
            }

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

    prune_imported_packages_with_missing_required_dependencies(selected, skipped_policy);
    prune_imported_optional_dependencies(selected);

    std::vector<PackageMetadata> entries;
    for (const auto& entry : selected) entries.push_back(entry.second);
    return entries;
}

bool ensure_current_debian_parsed_record_cache(
    const std::string& packages_path,
    const DebianBackendConfig& config,
    bool verbose,
    DebianParsedRecordCacheState* state_out = nullptr,
    CompactPackageAvailabilityIndex* compact_index_out = nullptr,
    std::string* error_out = nullptr
);

template <typename Func>
bool foreach_current_debian_parsed_record(
    const std::string& packages_path,
    const DebianBackendConfig& config,
    Func callback,
    DebianParsedRecordCacheState* state_out = nullptr,
    std::string* error_out = nullptr
);

struct DebianCompiledRecordCacheStreamWriter {
    std::string temp_path;
    std::ofstream out;
    size_t entry_count = 0;
};

bool begin_debian_compiled_record_cache_stream(
    DebianCompiledRecordCacheStreamWriter& writer,
    std::string* error_out = nullptr
);
bool append_debian_compiled_record_cache_stream_entry(
    DebianCompiledRecordCacheStreamWriter& writer,
    const DebianCompiledRecordCacheEntry& entry,
    std::string* error_out = nullptr
);
void abort_debian_compiled_record_cache_stream(
    DebianCompiledRecordCacheStreamWriter& writer
);
bool finish_debian_compiled_record_cache_stream(
    DebianCompiledRecordCacheStreamWriter& writer,
    const std::string& policy_fingerprint,
    std::string* error_out = nullptr
);
bool materialize_imported_entries_from_compiled_cache(
    const std::map<std::string, ImportedPackageDependencyState>& selected,
    std::vector<PackageMetadata>& entries_out,
    std::string* error_out = nullptr
);

DebianIncrementalImportResult load_debian_index_entries_from_records_incremental(
    const std::vector<DebianPackageRecord>& records,
    const DebianBackendConfig& config,
    const ImportPolicy& policy,
    bool verbose
) {
    DebianIncrementalImportResult result;

    std::set<std::string> available_packages;
    for (const auto& record : records) available_packages.insert(record.package);
    std::vector<std::string> system_drop_patterns = build_system_drop_patterns(policy, available_packages);
    auto provider_map = build_debian_provider_map(records, config.apt_arch);

    std::string policy_fingerprint = build_debian_compiled_record_cache_policy_fingerprint();
    std::map<std::string, DebianCompiledRecordCacheEntry> previous_entries_by_package;
    std::string cache_problem;
    bool have_previous_cache = load_debian_compiled_record_cache(
        policy_fingerprint,
        previous_entries_by_package,
        &cache_problem
    );
    bool full_rebuild = !have_previous_cache;

    std::map<std::string, std::string> current_fingerprints;
    std::map<std::string, std::vector<std::string>> current_provided_symbols;
    std::set<std::string> current_raw_packages;
    std::set<std::string> changed_symbols;
    std::set<std::string> impacted_raw_packages;

    for (const auto& record : records) {
        current_raw_packages.insert(record.package);
        std::string record_fingerprint = fingerprint_debian_package_record(record);
        std::vector<std::string> provided_symbols = collect_debian_record_provided_symbols(record, config);
        current_fingerprints[record.package] = record_fingerprint;
        current_provided_symbols[record.package] = provided_symbols;

        auto previous_it = previous_entries_by_package.find(record.package);
        if (full_rebuild ||
            previous_it == previous_entries_by_package.end() ||
            previous_it->second.record_fingerprint != record_fingerprint) {
            impacted_raw_packages.insert(record.package);
            changed_symbols.insert(provided_symbols.begin(), provided_symbols.end());
            if (previous_it != previous_entries_by_package.end()) {
                changed_symbols.insert(
                    previous_it->second.provided_symbols.begin(),
                    previous_it->second.provided_symbols.end()
                );
            }
        }
    }

    if (!full_rebuild) {
        for (const auto& previous_entry : previous_entries_by_package) {
            if (current_raw_packages.count(previous_entry.first) != 0) continue;
            changed_symbols.insert(
                previous_entry.second.provided_symbols.begin(),
                previous_entry.second.provided_symbols.end()
            );
        }
    }

    if (!full_rebuild && !changed_symbols.empty()) {
        for (const auto& record : records) {
            if (impacted_raw_packages.count(record.package) != 0) continue;
            std::vector<std::string> watch_symbols =
                collect_debian_record_dependency_watch_symbols(record, config, policy);
            if (dependency_watch_symbols_intersect(watch_symbols, changed_symbols)) {
                impacted_raw_packages.insert(record.package);
            }
        }
    }

    const size_t worker_count = recommended_parallel_worker_count(records.size());
    if (verbose) {
        std::cout << "[DEBUG] Importing Debian metadata with "
                  << worker_count << " worker(s)"
                  << " (" << (full_rebuild ? "full rebuild" : "incremental reuse") << ")."
                  << std::endl;
        if (!have_previous_cache && !cache_problem.empty()) {
            std::cout << "[DEBUG] "
                      << describe_debian_cache_rebuild_reason(
                             "Debian compiled record cache",
                             cache_problem
                         )
                      << std::endl;
        }
    }

    std::atomic<size_t> next_record{0};
    std::vector<std::map<std::string, PackageMetadata>> worker_selected(worker_count);
    std::vector<std::vector<std::string>> worker_skipped(worker_count);
    std::vector<std::vector<DebianCompiledRecordCacheEntry>> worker_compiled(worker_count);
    std::vector<size_t> worker_reused(worker_count, 0);
    std::vector<size_t> worker_rebuilt(worker_count, 0);

    auto worker = [&](size_t worker_index) {
        auto& selected = worker_selected[worker_index];
        auto& skipped = worker_skipped[worker_index];
        auto& compiled = worker_compiled[worker_index];

        while (true) {
            size_t record_index = next_record.fetch_add(1);
            if (record_index >= records.size()) return;

            const auto& record = records[record_index];
            const auto fingerprint_it = current_fingerprints.find(record.package);
            const auto provided_it = current_provided_symbols.find(record.package);
            if (fingerprint_it == current_fingerprints.end() ||
                provided_it == current_provided_symbols.end()) {
                continue;
            }

            DebianCompiledRecordCacheEntry cache_entry;
            bool reused = false;
            if (!full_rebuild && impacted_raw_packages.count(record.package) == 0) {
                auto previous_it = previous_entries_by_package.find(record.package);
                if (previous_it != previous_entries_by_package.end()) {
                    cache_entry = previous_it->second;
                    reused = true;
                    ++worker_reused[worker_index];
                }
            }

            if (!reused) {
                cache_entry = {};
                cache_entry.raw_package = record.package;
                cache_entry.record_fingerprint = fingerprint_it->second;
                cache_entry.provided_symbols = provided_it->second;

                    if (!record.filename.empty() && !record.sha256.empty()) {
                    if (matches_any_pattern(record.package, policy.skip_packages)) {
                        cache_entry.skip_reason = "blocked by policy";
                    } else {
                        PackageMetadata meta;
                        std::string skip_reason;
                        if (build_debian_package_metadata(
                                record,
                                config,
                                policy,
                                available_packages,
                                provider_map,
                                system_drop_patterns,
                                meta,
                                &skip_reason
                            )) {
                            cache_entry.importable = true;
                            cache_entry.meta = meta;
                        } else {
                            cache_entry.skip_reason = skip_reason;
                        }
                    }
                }

                ++worker_rebuilt[worker_index];
            }

            compiled.push_back(cache_entry);
            if (cache_entry.importable) {
                auto it = selected.find(cache_entry.meta.name);
                if (it == selected.end() ||
                    compare_versions(cache_entry.meta.version, it->second.version) > 0) {
                    selected[cache_entry.meta.name] = cache_entry.meta;
                }
            } else if (!cache_entry.skip_reason.empty()) {
                skipped.push_back(record.package + ": " + cache_entry.skip_reason);
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
        result.skipped_policy.insert(
            result.skipped_policy.end(),
            worker_skipped[worker_index].begin(),
            worker_skipped[worker_index].end()
        );
        result.compiled_record_entries.insert(
            result.compiled_record_entries.end(),
            worker_compiled[worker_index].begin(),
            worker_compiled[worker_index].end()
        );
        result.reused_records += worker_reused[worker_index];
        result.rebuilt_records += worker_rebuilt[worker_index];
    }

    result.processed_records = result.compiled_record_entries.size();

    prune_imported_packages_with_missing_required_dependencies(selected, &result.skipped_policy);
    prune_imported_optional_dependencies(selected);

    result.entries.reserve(selected.size());
    for (const auto& entry : selected) result.entries.push_back(entry.second);

    if (verbose) {
        std::cout << "[DEBUG] Debian import reuse: "
                  << result.reused_records << " reused, "
                  << result.rebuilt_records << " rebuilt."
                  << std::endl;
    }

    return result;
}

DebianIncrementalImportResult load_debian_index_entries_from_current_parsed_cache_incremental(
    const std::string& packages_path,
    const DebianBackendConfig& config,
    const ImportPolicy& policy,
    const CompactPackageAvailabilityIndex* precomputed_compact_index,
    bool verbose
) {
    DebianIncrementalImportResult result;

    DebianParsedRecordCacheState parsed_state;
    std::string parsed_error;
    if (!ensure_current_debian_parsed_record_cache(
            packages_path,
            config,
            verbose,
            &parsed_state,
            nullptr,
            &parsed_error
        )) {
        if (verbose && !parsed_error.empty()) {
            std::cout << "[DEBUG] Failed to prepare current Debian parsed-record cache: "
                      << parsed_error << std::endl;
        }
        return result;
    }

    std::string policy_fingerprint = build_debian_compiled_record_cache_policy_fingerprint();
    std::map<std::string, DebianCompiledRecordCacheEntry> previous_entries_by_package;
    std::string cache_problem;
    bool have_previous_cache = load_debian_compiled_record_cache(
        policy_fingerprint,
        previous_entries_by_package,
        &cache_problem
    );
    bool full_rebuild = !have_previous_cache;

    std::map<std::string, std::string> current_fingerprints;
    std::map<std::string, std::vector<std::string>> current_provided_symbols;
    std::set<std::string> current_raw_packages;
    std::set<std::string> changed_symbols;
    std::set<std::string> impacted_raw_packages;
    CompactPackageAvailabilityIndex compact_index;
    bool have_compact_index =
        full_rebuild &&
        precomputed_compact_index != nullptr &&
        !precomputed_compact_index->available_packages.empty();
    if (have_compact_index) {
        compact_index = *precomputed_compact_index;
        VLOG(verbose, "Reused the compact Debian availability index collected during parsed-cache build.");
    } else {
        std::vector<std::string> available_package_list;
        std::vector<std::pair<std::string, std::string>> provider_pairs;
        std::string first_pass_error;
        if (!foreach_current_debian_parsed_record(
                packages_path,
                config,
                [&](const DebianPackageRecord& record) {
                    append_debian_record_to_compact_availability_index_builder(
                        record,
                        config.apt_arch,
                        available_package_list,
                        provider_pairs
                    );

                    if (full_rebuild) return true;

                    current_raw_packages.insert(record.package);
                    std::string record_fingerprint = fingerprint_debian_package_record(record);
                    std::vector<std::string> provided_symbols =
                        collect_debian_record_provided_symbols(record, config);
                    current_fingerprints[record.package] = record_fingerprint;
                    current_provided_symbols[record.package] = provided_symbols;

                    auto previous_it = previous_entries_by_package.find(record.package);
                    if (previous_it == previous_entries_by_package.end() ||
                        previous_it->second.record_fingerprint != record_fingerprint) {
                        impacted_raw_packages.insert(record.package);
                        changed_symbols.insert(provided_symbols.begin(), provided_symbols.end());
                        if (previous_it != previous_entries_by_package.end()) {
                            changed_symbols.insert(
                                previous_it->second.provided_symbols.begin(),
                                previous_it->second.provided_symbols.end()
                            );
                        }
                    }

                    return true;
                },
                nullptr,
                &first_pass_error
            )) {
            if (verbose && !first_pass_error.empty()) {
                std::cout << "[DEBUG] Failed while streaming Debian parsed records: "
                          << first_pass_error << std::endl;
            }
            return result;
        }

        compact_index = finalize_compact_package_availability_index(
            std::move(available_package_list),
            std::move(provider_pairs)
        );
        have_compact_index = !compact_index.available_packages.empty();
    }

    if (!full_rebuild) {
        for (const auto& previous_entry : previous_entries_by_package) {
            if (current_raw_packages.count(previous_entry.first) != 0) continue;
            changed_symbols.insert(
                previous_entry.second.provided_symbols.begin(),
                previous_entry.second.provided_symbols.end()
            );
        }
    }
    std::vector<std::string> system_drop_patterns =
        build_system_drop_patterns(policy, compact_index.available_packages);

    if (!full_rebuild && !changed_symbols.empty()) {
        std::string impact_error;
        if (!foreach_current_debian_parsed_record(
                packages_path,
                config,
                [&](const DebianPackageRecord& record) {
                    if (impacted_raw_packages.count(record.package) != 0) return true;
                    std::vector<std::string> watch_symbols =
                        collect_debian_record_dependency_watch_symbols(record, config, policy);
                    if (dependency_watch_symbols_intersect(watch_symbols, changed_symbols)) {
                        impacted_raw_packages.insert(record.package);
                    }
                    return true;
                },
                nullptr,
                &impact_error
            )) {
            if (verbose && !impact_error.empty()) {
                std::cout << "[DEBUG] Failed while scanning Debian dependency watch symbols: "
                          << impact_error << std::endl;
            }
            return result;
        }
    }

    if (verbose) {
        std::cout << "[DEBUG] Importing Debian metadata from the parsed-record cache"
                  << " (" << (full_rebuild ? "full rebuild" : "incremental reuse") << ")."
                  << std::endl;
        if (!have_previous_cache && !cache_problem.empty()) {
            std::cout << "[DEBUG] "
                      << describe_debian_cache_rebuild_reason(
                             "Debian compiled record cache",
                             cache_problem
                         )
                      << std::endl;
        }
    }

    std::map<std::string, ImportedPackageDependencyState> selected;
    DebianCompiledRecordCacheStreamWriter compiled_writer;
    std::string compiled_writer_error;
    if (!begin_debian_compiled_record_cache_stream(compiled_writer, &compiled_writer_error)) {
        if (verbose && !compiled_writer_error.empty()) {
            std::cout << "[DEBUG] Failed to open the compiled-record cache stream: "
                      << compiled_writer_error << std::endl;
        }
        return result;
    }
    std::string second_pass_error;
    if (!foreach_current_debian_parsed_record(
            packages_path,
            config,
            [&](const DebianPackageRecord& record) {
                DebianCompiledRecordCacheEntry cache_entry;
                bool reused = false;
                if (!full_rebuild && impacted_raw_packages.count(record.package) == 0) {
                    auto previous_it = previous_entries_by_package.find(record.package);
                    if (previous_it != previous_entries_by_package.end()) {
                        cache_entry = previous_it->second;
                        reused = true;
                        ++result.reused_records;
                    }
                }

                if (!reused) {
                    cache_entry = {};
                    cache_entry.raw_package = record.package;
                    if (full_rebuild) {
                        cache_entry.record_fingerprint = fingerprint_debian_package_record(record);
                        cache_entry.provided_symbols =
                            collect_debian_record_provided_symbols(record, config);
                    } else {
                        const auto fingerprint_it = current_fingerprints.find(record.package);
                        const auto provided_it = current_provided_symbols.find(record.package);
                        if (fingerprint_it == current_fingerprints.end() ||
                            provided_it == current_provided_symbols.end()) {
                            second_pass_error =
                                "streamed parsed cache lost incremental fingerprint state";
                            return false;
                        }
                        cache_entry.record_fingerprint = fingerprint_it->second;
                        cache_entry.provided_symbols = provided_it->second;
                    }

                    if (!record.filename.empty() && !record.sha256.empty()) {
                        if (matches_any_pattern(record.package, policy.skip_packages)) {
                            cache_entry.skip_reason = "blocked by policy";
                        } else {
                            PackageMetadata meta;
                            std::string skip_reason;
                            if (build_debian_package_metadata(
                                    record,
                                    config,
                                    policy,
                                    compact_index.available_packages,
                                    compact_index.provider_map,
                                    system_drop_patterns,
                                    meta,
                                    &skip_reason
                                )) {
                                cache_entry.importable = true;
                                cache_entry.meta = meta;
                            } else {
                                cache_entry.skip_reason = skip_reason;
                            }
                        }
                    }

                    ++result.rebuilt_records;
                }

                if (!append_debian_compiled_record_cache_stream_entry(
                        compiled_writer,
                        cache_entry,
                        &second_pass_error
                    )) {
                    return false;
                }

                ++result.processed_records;
                if (cache_entry.importable) {
                    ImportedPackageDependencyState dependency_state =
                        build_imported_dependency_state(cache_entry.meta);
                    auto it = selected.find(dependency_state.name);
                    if (it == selected.end() ||
                        compare_versions(dependency_state.version, it->second.version) > 0) {
                        selected[dependency_state.name] = std::move(dependency_state);
                    }
                } else if (!cache_entry.skip_reason.empty()) {
                    result.skipped_policy.push_back(record.package + ": " + cache_entry.skip_reason);
                }

                return true;
            },
            nullptr,
            &second_pass_error
        )) {
        abort_debian_compiled_record_cache_stream(compiled_writer);
        if (verbose && !second_pass_error.empty()) {
            std::cout << "[DEBUG] Failed while compiling Debian metadata from the parsed-record cache: "
                      << second_pass_error << std::endl;
        }
        result = {};
        return result;
    }

    if (!finish_debian_compiled_record_cache_stream(
            compiled_writer,
            policy_fingerprint,
            &second_pass_error
        )) {
        abort_debian_compiled_record_cache_stream(compiled_writer);
        if (verbose && !second_pass_error.empty()) {
            std::cout << "[DEBUG] Failed to finalize the Debian compiled-record cache stream: "
                      << second_pass_error << std::endl;
        }
        result = {};
        return result;
    }
    result.compiled_record_cache_written = true;
    result.compiled_record_cache_count = result.processed_records;

    prune_imported_packages_with_missing_required_dependencies(selected, &result.skipped_policy);
    prune_imported_optional_dependencies(selected);

    if (!materialize_imported_entries_from_compiled_cache(selected, result.entries, &second_pass_error)) {
        if (verbose && !second_pass_error.empty()) {
            std::cout << "[DEBUG] Failed to materialize imported Debian entries from the compiled cache: "
                      << second_pass_error << std::endl;
        }
        result = {};
        return result;
    }

    if (verbose) {
        std::cout << "[DEBUG] Debian import reuse: "
                  << result.reused_records << " reused, "
                  << result.rebuilt_records << " rebuilt."
                  << std::endl;
    }

    return result;
}

std::vector<PackageMetadata> load_debian_index_entries(
    const std::string& packages_path,
    bool verbose,
    std::vector<std::string>* skipped_policy
) {
    DebianBackendConfig config = load_debian_backend_config(verbose);
    ImportPolicy policy = get_import_policy(verbose);
    std::vector<DebianPackageRecord> records = parse_debian_packages_file(packages_path, config, verbose);
    return load_debian_index_entries_from_records(records, config, policy, verbose, skipped_policy);
}

std::string get_debian_packages_gz_cache_path() {
    return REPO_CACHE_PATH + "debian/Packages.gz";
}

std::string get_debian_packages_cache_path() {
    return REPO_CACHE_PATH + "debian/Packages";
}

std::string get_debian_parsed_record_cache_path() {
    return REPO_CACHE_PATH + "debian/Packages.records.bin";
}

std::string get_debian_parsed_record_state_path() {
    return REPO_CACHE_PATH + "debian/Packages.records.state";
}

std::string get_debian_search_preview_path() {
    return REPO_CACHE_PATH + "debian/Packages.preview.json";
}

std::string get_debian_search_preview_binary_cache_path() {
    return REPO_CACHE_PATH + "debian/Packages.preview.bin";
}

std::string get_debian_imported_index_cache_path() {
    return REPO_CACHE_PATH + "debian/Packages.imported.json";
}

std::string get_debian_imported_index_binary_cache_path() {
    return REPO_CACHE_PATH + "debian/Packages.imported.bin";
}

std::string get_debian_imported_index_state_path() {
    return REPO_CACHE_PATH + "debian/Packages.imported.state";
}

std::string get_debian_compiled_record_cache_path() {
    return REPO_CACHE_PATH + "debian/Packages.compiled.bin";
}

std::string get_debian_compiled_record_state_path() {
    return REPO_CACHE_PATH + "debian/Packages.compiled.state";
}

std::string get_debian_raw_context_index_path() {
    return REPO_CACHE_PATH + "debian/Packages.compiled.index.bin";
}

std::string get_debian_raw_context_index_state_path() {
    return REPO_CACHE_PATH + "debian/Packages.compiled.index.state";
}

void invalidate_debian_derived_metadata_caches(bool verbose) {
    const std::vector<std::string> paths = {
        get_debian_search_preview_binary_cache_path(),
        get_debian_search_preview_path(),
        get_debian_imported_index_binary_cache_path(),
        get_debian_imported_index_cache_path(),
        get_debian_imported_index_state_path(),
        get_debian_compiled_record_cache_path(),
        get_debian_compiled_record_state_path(),
        get_debian_raw_context_index_path(),
        get_debian_raw_context_index_state_path(),
    };

    for (const auto& path : paths) {
        if (remove(path.c_str()) != 0 && errno != ENOENT) {
            VLOG(verbose, "Failed to remove stale Debian cache artifact " << path
                 << ": " << strerror(errno));
        }
    }

    invalidate_debian_search_preview_cache();
}

std::string strip_debian_index_compression_suffix(const std::string& path) {
    static const char* kSuffixes[] = {".gz", ".xz", ".bz2", ".lzma", ".zst", ".zstd"};
    for (const char* suffix : kSuffixes) {
        size_t len = std::strlen(suffix);
        if (path.size() >= len && path.compare(path.size() - len, len, suffix) == 0) {
            return path.substr(0, path.size() - len);
        }
    }
    return path;
}

std::string get_debian_packages_diff_index_url(const std::string& packages_url) {
    return strip_debian_index_compression_suffix(packages_url) + ".diff/Index";
}

std::string path_or_url_dirname(const std::string& path) {
    size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return path;
    if (pos == 0) return path.substr(0, 1);
    return path.substr(0, pos);
}

std::string normalize_debian_diff_patch_name(const std::string& value) {
    if (value.size() > 3 && value.compare(value.size() - 3, 3, ".gz") == 0) {
        return value.substr(0, value.size() - 3);
    }
    return value;
}

bool fetch_text_url(
    const std::string& url,
    std::string& text_out,
    bool verbose,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    HttpOptions opts;
    opts.method = "GET";
    opts.follow_location = true;
    opts.allow_connection_reuse = true;
    opts.show_progress = false;
    opts.verbose = verbose;

    std::stringstream response;
    if (!HttpRequest(url, response, opts, error_out)) return false;
    text_out = response.str();
    return true;
}

bool parse_debian_diff_index_entry_line(
    const std::string& line,
    DebianDiffIndexEntry& entry,
    bool normalize_name,
    bool require_name = true
) {
    entry = {};
    std::istringstream iss(line);
    std::string hash;
    std::string size_text;
    std::string name;
    if (!(iss >> hash >> size_text)) return false;
    if (!(iss >> name)) {
        if (require_name) return false;
        name.clear();
    }

    errno = 0;
    char* end = nullptr;
    long parsed_size = std::strtol(size_text.c_str(), &end, 10);
    if (errno != 0 || end == size_text.c_str() || (end && *end != '\0')) return false;

    entry.hash = hash;
    entry.size = parsed_size;
    entry.remote_name = name;
    entry.name = normalize_name ? normalize_debian_diff_patch_name(name) : name;
    return !entry.hash.empty() && (!require_name || !entry.name.empty());
}

bool parse_debian_packages_diff_index(
    const std::string& text,
    DebianPackagesDiffIndex& index,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    index = {};

    std::vector<std::map<std::string, std::string>> stanzas = parse_debian_control_stanzas(text);
    if (stanzas.empty()) {
        if (error_out) *error_out = "PDiff index is empty";
        return false;
    }

    const auto& stanza = stanzas.front();
    auto current_it = stanza.find("SHA256-Current");
    auto history_it = stanza.find("SHA256-History");
    auto patches_it = stanza.find("SHA256-Patches");
    auto downloads_it = stanza.find("SHA256-Download");
    if (current_it == stanza.end() ||
        history_it == stanza.end() ||
        patches_it == stanza.end() ||
        downloads_it == stanza.end()) {
        if (error_out) *error_out = "PDiff index is missing required SHA256 fields";
        return false;
    }

    {
        DebianDiffIndexEntry current_entry;
        if (!parse_debian_diff_index_entry_line(current_it->second, current_entry, false, false)) {
            if (error_out) *error_out = "failed to parse SHA256-Current";
            return false;
        }
        index.current_hash = current_entry.hash;
        index.current_size = current_entry.size;
    }

    auto append_named_entries = [](
        const std::string& raw_value,
        bool normalize_name,
        std::map<std::string, DebianDiffIndexEntry>& out_map
    ) {
        std::istringstream lines(raw_value);
        std::string line;
        while (std::getline(lines, line)) {
            line = trim(line);
            if (line.empty()) continue;

            DebianDiffIndexEntry entry;
            if (!parse_debian_diff_index_entry_line(line, entry, normalize_name)) continue;
            out_map[entry.name] = entry;
        }
    };

    {
        std::istringstream lines(history_it->second);
        std::string line;
        while (std::getline(lines, line)) {
            line = trim(line);
            if (line.empty()) continue;

            DebianDiffIndexEntry entry;
            if (!parse_debian_diff_index_entry_line(line, entry, true)) continue;
            index.history_by_hash[entry.hash] = entry;
        }
    }

    append_named_entries(patches_it->second, true, index.patches_by_name);
    append_named_entries(downloads_it->second, true, index.downloads_by_name);

    if (index.current_hash.empty() ||
        index.history_by_hash.empty() ||
        index.patches_by_name.empty() ||
        index.downloads_by_name.empty()) {
        if (error_out) *error_out = "PDiff index does not contain a usable patch chain";
        return false;
    }

    return true;
}

struct DebianEdPatchCommand {
    long start = 0;
    long end = 0;
    char op = '\0';
    std::vector<std::string> payload;
};

bool parse_debian_ed_patch_commands(
    const std::string& patch_text,
    std::vector<DebianEdPatchCommand>& commands,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    commands.clear();

    std::istringstream input(patch_text);
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        size_t pos = 0;
        while (pos < line.size() && std::isdigit(static_cast<unsigned char>(line[pos]))) ++pos;
        if (pos == 0) {
            if (error_out) *error_out = "invalid ed patch command";
            return false;
        }

        DebianEdPatchCommand command;
        command.start = std::strtol(line.substr(0, pos).c_str(), nullptr, 10);
        command.end = command.start;

        if (pos < line.size() && line[pos] == ',') {
            size_t rhs_start = ++pos;
            while (pos < line.size() && std::isdigit(static_cast<unsigned char>(line[pos]))) ++pos;
            if (rhs_start == pos) {
                if (error_out) *error_out = "invalid ed patch range";
                return false;
            }
            command.end = std::strtol(line.substr(rhs_start, pos - rhs_start).c_str(), nullptr, 10);
        }

        if (pos >= line.size()) {
            if (error_out) *error_out = "missing ed patch opcode";
            return false;
        }
        command.op = line[pos++];
        if (pos != line.size() || (command.op != 'a' && command.op != 'c' && command.op != 'd')) {
            if (error_out) *error_out = "unsupported ed patch opcode";
            return false;
        }

        if (command.op == 'a' || command.op == 'c') {
            bool terminated = false;
            while (std::getline(input, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line == ".") {
                    terminated = true;
                    break;
                }
                command.payload.push_back(line);
            }
            if (!terminated) {
                if (error_out) *error_out = "unterminated ed patch payload";
                return false;
            }
        }

        commands.push_back(std::move(command));
    }

    return true;
}

bool apply_debian_ed_patch_to_file(
    const std::string& target_path,
    const std::string& patch_path,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    std::ifstream target(target_path);
    if (!target) {
        if (error_out) *error_out = "failed to open local Packages file";
        return false;
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(target, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
    }
    if (target.bad()) {
        if (error_out) *error_out = "failed to read local Packages file";
        return false;
    }

    std::ifstream patch_in(patch_path);
    if (!patch_in) {
        if (error_out) *error_out = "failed to open downloaded PDiff patch";
        return false;
    }
    std::string patch_text(
        (std::istreambuf_iterator<char>(patch_in)),
        std::istreambuf_iterator<char>()
    );

    std::vector<DebianEdPatchCommand> commands;
    if (!parse_debian_ed_patch_commands(patch_text, commands, error_out)) return false;

    long previous_start = std::numeric_limits<long>::max();
    for (const auto& command : commands) {
        if (command.start > previous_start) {
            if (error_out) *error_out = "PDiff patch commands are not reverse sorted";
            return false;
        }
        previous_start = command.start;

        if (command.op == 'a') {
            if (command.start < 0 || command.start > static_cast<long>(lines.size())) {
                if (error_out) *error_out = "PDiff append command is out of range";
                return false;
            }
            lines.insert(lines.begin() + command.start, command.payload.begin(), command.payload.end());
            continue;
        }

        if (command.start < 1 ||
            command.end < command.start ||
            command.end > static_cast<long>(lines.size())) {
            if (error_out) *error_out = "PDiff command range is out of bounds";
            return false;
        }

        auto erase_begin = lines.begin() + (command.start - 1);
        auto erase_end = lines.begin() + command.end;
        size_t insert_index = static_cast<size_t>(command.start - 1);
        if (command.op == 'd') {
            lines.erase(erase_begin, erase_end);
            continue;
        }

        lines.erase(erase_begin, erase_end);
        lines.insert(lines.begin() + insert_index, command.payload.begin(), command.payload.end());
    }

    std::string temp_path = target_path + ".pdiff.tmp";
    std::ofstream out(temp_path, std::ios::trunc);
    if (!out) {
        if (error_out) *error_out = "failed to open patched Packages file";
        return false;
    }
    for (const auto& item : lines) {
        out << item << "\n";
    }
    out.close();
    if (!out) {
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to write patched Packages file";
        return false;
    }

    if (rename(temp_path.c_str(), target_path.c_str()) != 0) {
        remove(temp_path.c_str());
        if (error_out) *error_out = strerror(errno);
        return false;
    }

    return true;
}

bool try_update_debian_packages_with_pdiff(
    const DebianBackendConfig& config,
    const std::string& packages_path,
    bool verbose,
    bool& changed_out,
    size_t& patches_applied_out,
    std::string* error_out = nullptr
) {
    changed_out = false;
    patches_applied_out = 0;
    if (error_out) error_out->clear();

    if (access(packages_path.c_str(), F_OK) != 0) {
        if (error_out) *error_out = "local Debian Packages cache is unavailable";
        return false;
    }

    std::string diff_index_url = get_debian_packages_diff_index_url(config.packages_url);
    std::string diff_text;
    if (!fetch_text_url(diff_index_url, diff_text, verbose, error_out)) return false;

    DebianPackagesDiffIndex diff_index;
    if (!parse_debian_packages_diff_index(diff_text, diff_index, error_out)) return false;

    std::string current_hash = sha256_hex_file(packages_path);
    if (current_hash.empty()) {
        if (error_out) *error_out = "failed to hash local Debian Packages cache";
        return false;
    }

    if (current_hash == diff_index.current_hash) {
        return true;
    }

    const size_t kMaxPatchChain = 64;
    std::set<std::string> seen_hashes;
    seen_hashes.insert(current_hash);
    std::string patch_base_url = path_or_url_dirname(diff_index_url);
    std::string temp_dir = REPO_CACHE_PATH + "debian/pdiff/";
    if (!mkdir_p(temp_dir)) {
        if (error_out) *error_out = "failed to create Debian PDiff cache directory";
        return false;
    }

    while (current_hash != diff_index.current_hash) {
        if (patches_applied_out >= kMaxPatchChain) {
            if (error_out) *error_out = "PDiff chain is too long";
            return false;
        }

        auto history_it = diff_index.history_by_hash.find(current_hash);
        if (history_it == diff_index.history_by_hash.end()) {
            if (error_out) *error_out = "no usable PDiff patch chain for the local Packages cache";
            return false;
        }

        const std::string& patch_name = history_it->second.name;
        auto patch_it = diff_index.patches_by_name.find(patch_name);
        auto download_it = diff_index.downloads_by_name.find(patch_name);
        if (patch_it == diff_index.patches_by_name.end() ||
            download_it == diff_index.downloads_by_name.end()) {
            if (error_out) *error_out = "PDiff metadata is incomplete";
            return false;
        }

        std::string patch_gz_path = temp_dir + safe_repo_filename_component(download_it->second.remote_name);
        std::string patch_txt_path = patch_gz_path + ".txt";
        std::string patch_url = join_url_path(patch_base_url, download_it->second.remote_name);

        std::string download_error;
        if (!DownloadFile(patch_url, patch_gz_path, verbose, &download_error, false)) {
            if (error_out) *error_out = download_error.empty()
                ? "failed to download Debian PDiff patch"
                : download_error;
            remove(patch_gz_path.c_str());
            return false;
        }

        std::string downloaded_hash = sha256_hex_file(patch_gz_path);
        if (downloaded_hash.empty() || downloaded_hash != download_it->second.hash) {
            remove(patch_gz_path.c_str());
            if (error_out) *error_out = "downloaded Debian PDiff patch failed SHA256 verification";
            return false;
        }

        std::string unpack_error;
        if (!GpkgArchive::decompress_gzip_file(patch_gz_path, patch_txt_path, &unpack_error)) {
            remove(patch_gz_path.c_str());
            remove(patch_txt_path.c_str());
            if (error_out) *error_out = unpack_error.empty()
                ? "failed to unpack Debian PDiff patch"
                : unpack_error;
            return false;
        }

        std::string patch_hash = sha256_hex_file(patch_txt_path);
        if (patch_hash.empty() || patch_hash != patch_it->second.hash) {
            remove(patch_gz_path.c_str());
            remove(patch_txt_path.c_str());
            if (error_out) *error_out = "unpacked Debian PDiff patch failed SHA256 verification";
            return false;
        }

        std::string apply_error;
        if (!apply_debian_ed_patch_to_file(packages_path, patch_txt_path, &apply_error)) {
            remove(patch_gz_path.c_str());
            remove(patch_txt_path.c_str());
            if (error_out) *error_out = apply_error.empty()
                ? "failed to apply Debian PDiff patch"
                : apply_error;
            return false;
        }

        remove(patch_gz_path.c_str());
        remove(patch_txt_path.c_str());

        current_hash = sha256_hex_file(packages_path);
        if (current_hash.empty()) {
            if (error_out) *error_out = "failed to hash the patched Debian Packages cache";
            return false;
        }
        if (!seen_hashes.insert(current_hash).second) {
            if (error_out) *error_out = "PDiff patch chain loop detected";
            return false;
        }

        changed_out = true;
        ++patches_applied_out;
    }

    return true;
}

bool should_prefer_debian_search_preview_candidate(
    const DebianSearchPreviewEntry& candidate,
    const DebianSearchPreviewEntry& current
) {
    if (candidate.installable != current.installable) return candidate.installable;
    return compare_versions(candidate.meta.version, current.meta.version) > 0;
}

bool parse_debian_search_preview_skip_entry(
    const std::string& entry,
    std::string& package_name,
    std::string& reason
) {
    size_t colon = entry.find(':');
    if (colon == std::string::npos) return false;

    package_name = trim(entry.substr(0, colon));
    reason = trim(entry.substr(colon + 1));
    return !package_name.empty();
}

std::vector<DebianSearchPreviewEntry> build_debian_search_preview_entries_from_records(
    const std::vector<DebianPackageRecord>& records,
    const DebianBackendConfig& config,
    const ImportPolicy& policy,
    const std::vector<PackageMetadata>& installable_entries,
    const std::vector<std::string>& skipped_policy
) {
    std::map<std::string, PackageMetadata> installable_by_name;
    for (const auto& meta : installable_entries) {
        auto it = installable_by_name.find(meta.name);
        if (it == installable_by_name.end() ||
            compare_versions(meta.version, it->second.version) > 0) {
            installable_by_name[meta.name] = meta;
        }
    }

    std::map<std::string, std::string> skipped_reason_by_name;
    for (const auto& entry : skipped_policy) {
        std::string package_name;
        std::string reason;
        if (!parse_debian_search_preview_skip_entry(entry, package_name, reason)) continue;

        auto it = skipped_reason_by_name.find(package_name);
        if (it == skipped_reason_by_name.end() || reason.size() > it->second.size()) {
            skipped_reason_by_name[package_name] = reason;
        }
    }

    std::map<std::string, DebianSearchPreviewEntry> preview_by_name;
    std::map<std::string, std::vector<std::string>> raw_names_by_import;

    for (const auto& record : records) {
        std::string import_name = raw_debian_effective_import_name(record, policy);
        if (import_name.empty()) continue;

        auto& raw_names = raw_names_by_import[import_name];
        if (std::find(raw_names.begin(), raw_names.end(), record.package) == raw_names.end()) {
            raw_names.push_back(record.package);
        }

        DebianSearchPreviewEntry candidate;
        populate_raw_debian_preview_metadata_from_sources(record, config, policy, candidate.meta);
        if (record.filename.empty() || record.sha256.empty()) {
            candidate.reason = "package metadata is incomplete";
        }

        auto it = preview_by_name.find(import_name);
        if (it == preview_by_name.end() ||
            should_prefer_debian_search_preview_candidate(candidate, it->second)) {
            preview_by_name[import_name] = candidate;
        }
    }

    for (auto& entry : preview_by_name) {
        auto raw_names_it = raw_names_by_import.find(entry.first);
        if (raw_names_it != raw_names_by_import.end()) {
            entry.second.raw_names = raw_names_it->second;
        }

        auto installable_it = installable_by_name.find(entry.first);
        if (installable_it != installable_by_name.end()) {
            entry.second.meta = installable_it->second;
            entry.second.installable = true;
            entry.second.reason.clear();
            continue;
        }

        entry.second.installable = false;
        auto skipped_it = skipped_reason_by_name.find(entry.first);
        if (skipped_it != skipped_reason_by_name.end()) {
            entry.second.reason = skipped_it->second;
        } else if (entry.second.reason.empty()) {
            entry.second.reason = "required dependency closure is unsatisfied";
        }
    }

    std::vector<DebianSearchPreviewEntry> entries;
    entries.reserve(preview_by_name.size());
    for (const auto& entry : preview_by_name) entries.push_back(entry.second);
    return entries;
}

std::vector<DebianSearchPreviewEntry> build_debian_search_preview_entries_from_current_parsed_cache(
    const std::string& packages_path,
    const DebianBackendConfig& config,
    const ImportPolicy& policy,
    const std::vector<PackageMetadata>& installable_entries,
    const std::vector<std::string>& skipped_policy,
    bool verbose
) {
    std::map<std::string, PackageMetadata> installable_by_name;
    for (const auto& meta : installable_entries) {
        auto it = installable_by_name.find(meta.name);
        if (it == installable_by_name.end() ||
            compare_versions(meta.version, it->second.version) > 0) {
            installable_by_name[meta.name] = meta;
        }
    }

    std::map<std::string, std::string> skipped_reason_by_name;
    for (const auto& entry : skipped_policy) {
        std::string package_name;
        std::string reason;
        if (!parse_debian_search_preview_skip_entry(entry, package_name, reason)) continue;

        auto it = skipped_reason_by_name.find(package_name);
        if (it == skipped_reason_by_name.end() || reason.size() > it->second.size()) {
            skipped_reason_by_name[package_name] = reason;
        }
    }

    std::map<std::string, DebianSearchPreviewEntry> preview_by_name;
    std::map<std::string, std::vector<std::string>> raw_names_by_import;

    std::string stream_error;
    if (!foreach_current_debian_parsed_record(
            packages_path,
            config,
            [&](const DebianPackageRecord& record) {
                std::string import_name = raw_debian_effective_import_name(record, policy);
                if (import_name.empty()) return true;

                auto& raw_names = raw_names_by_import[import_name];
                if (std::find(raw_names.begin(), raw_names.end(), record.package) == raw_names.end()) {
                    raw_names.push_back(record.package);
                }

                DebianSearchPreviewEntry candidate;
                populate_raw_debian_preview_metadata_from_sources(record, config, policy, candidate.meta);
                if (record.filename.empty() || record.sha256.empty()) {
                    candidate.reason = "package metadata is incomplete";
                }

                auto it = preview_by_name.find(import_name);
                if (it == preview_by_name.end() ||
                    should_prefer_debian_search_preview_candidate(candidate, it->second)) {
                    preview_by_name[import_name] = candidate;
                }

                return true;
            },
            nullptr,
            &stream_error
        ) && verbose && !stream_error.empty()) {
        std::cout << "[DEBUG] Failed while streaming Debian preview candidates: "
                  << stream_error << std::endl;
    }

    for (auto& entry : preview_by_name) {
        auto raw_names_it = raw_names_by_import.find(entry.first);
        if (raw_names_it != raw_names_by_import.end()) {
            entry.second.raw_names = raw_names_it->second;
        }

        auto installable_it = installable_by_name.find(entry.first);
        if (installable_it != installable_by_name.end()) {
            entry.second.meta = installable_it->second;
            entry.second.installable = true;
            entry.second.reason.clear();
            continue;
        }

        entry.second.installable = false;
        auto skipped_it = skipped_reason_by_name.find(entry.first);
        if (skipped_it != skipped_reason_by_name.end()) {
            entry.second.reason = skipped_it->second;
        } else if (entry.second.reason.empty()) {
            entry.second.reason = "required dependency closure is unsatisfied";
        }
    }

    std::vector<DebianSearchPreviewEntry> entries;
    entries.reserve(preview_by_name.size());
    for (const auto& entry : preview_by_name) entries.push_back(entry.second);
    return entries;
}

std::vector<DebianSearchPreviewEntry> build_debian_search_preview_entries(
    const std::string& packages_path,
    const std::vector<PackageMetadata>& installable_entries,
    const std::vector<std::string>& skipped_policy,
    bool verbose
) {
    DebianBackendConfig config = load_debian_backend_config(verbose);
    ImportPolicy policy = get_import_policy(verbose);
    std::vector<DebianPackageRecord> records = parse_debian_packages_file(packages_path, config, verbose);
    return build_debian_search_preview_entries_from_records(
        records,
        config,
        policy,
        installable_entries,
        skipped_policy
    );
}

std::vector<DebianSearchPreviewEntry> build_debian_search_preview_entries_from_compiled_cache(
    const std::vector<PackageMetadata>& installable_entries,
    bool verbose,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    std::map<std::string, PackageMetadata> installable_by_name;
    for (const auto& meta : installable_entries) {
        auto it = installable_by_name.find(meta.name);
        if (it == installable_by_name.end() ||
            compare_versions(meta.version, it->second.version) > 0) {
            installable_by_name[meta.name] = meta;
        }
    }

    std::map<std::string, DebianSearchPreviewEntry> preview_by_name;
    std::string cache_error;
    if (!foreach_debian_binary_cache_entry<DebianCompiledRecordCacheEntry>(
            get_debian_compiled_record_cache_path(),
            DEBIAN_COMPILED_RECORD_CACHE_MAGIC,
            [](std::istream& in, DebianCompiledRecordCacheEntry& entry) {
                return read_debian_compiled_record_cache_entry_binary(in, entry);
            },
            [&](const DebianCompiledRecordCacheEntry& cache_entry) {
                if (cache_entry.raw_package.empty()) return true;

                DebianSearchPreviewEntry candidate;
                candidate.meta = cache_entry.meta;
                if (candidate.meta.name.empty()) candidate.meta.name = cache_entry.raw_package;
                if (candidate.meta.debian_package.empty()) {
                    candidate.meta.debian_package = cache_entry.raw_package;
                }
                if (candidate.meta.source_kind.empty()) candidate.meta.source_kind = "debian";
                candidate.raw_names.push_back(cache_entry.raw_package);
                candidate.installable = false;
                candidate.reason = cache_entry.skip_reason.empty()
                    ? "required dependency closure is unsatisfied"
                    : cache_entry.skip_reason;

                auto it = preview_by_name.find(candidate.meta.name);
                if (it == preview_by_name.end()) {
                    preview_by_name[candidate.meta.name] = candidate;
                    return true;
                }

                auto& current = it->second;
                if (std::find(
                        current.raw_names.begin(),
                        current.raw_names.end(),
                        cache_entry.raw_package
                    ) == current.raw_names.end()) {
                    current.raw_names.push_back(cache_entry.raw_package);
                }
                if (should_prefer_debian_search_preview_candidate(candidate, current)) {
                    candidate.raw_names = current.raw_names;
                    current = candidate;
                }
                return true;
            },
            &cache_error
        )) {
        if (error_out) *error_out = cache_error;
        return {};
    }

    for (auto& entry : preview_by_name) {
        auto installable_it = installable_by_name.find(entry.first);
        if (installable_it != installable_by_name.end()) {
            entry.second.meta = installable_it->second;
            entry.second.installable = true;
            entry.second.reason.clear();
        }
    }

    std::vector<DebianSearchPreviewEntry> entries;
    entries.reserve(preview_by_name.size());
    for (auto& entry : preview_by_name) {
        entry.second.raw_names = unique_string_list(entry.second.raw_names);
        entries.push_back(std::move(entry.second));
    }

    VLOG(verbose, "Built Debian search preview entries from the compiled cache ("
         << entries.size() << " packages).");
    return entries;
}

std::string raw_debian_effective_import_name(
    const DebianPackageRecord& record,
    const ImportPolicy& policy
) {
    auto override_it = policy.package_overrides.find(record.package);
    if (override_it != policy.package_overrides.end() &&
        !override_it->second.rename.empty()) {
        return override_it->second.rename;
    }
    return record.package;
}

void populate_raw_debian_preview_metadata(
    const RawDebianContext& context,
    const DebianPackageRecord& record,
    PackageMetadata& meta
) {
    populate_raw_debian_preview_metadata_from_sources(record, context.config, context.policy, meta);
}

bool raw_debian_record_is_blocked_by_policy(
    const RawDebianContext& context,
    const DebianPackageRecord& record,
    std::string* reason_out = nullptr
) {
    if (reason_out) reason_out->clear();

    if (matches_any_pattern(record.package, context.policy.skip_packages)) {
        if (reason_out) *reason_out = "blocked by GeminiOS import policy";
        return true;
    }

    return false;
}

std::vector<std::string> collect_raw_debian_exact_candidate_names(
    const std::string& requested_name,
    const RawDebianContext& context
) {
    std::vector<std::string> candidates;
    std::set<std::string> seen;

    auto append = [&](const std::string& raw_name) {
        if (raw_name.empty()) return;
        if (!seen.insert(raw_name).second) return;
        candidates.push_back(raw_name);
    };

    const RawDebianOffsetIndexEntry* exact_it =
        raw_debian_offset_index_find(context.raw_package_offsets, requested_name);
    if (exact_it) append(exact_it->key);

    const std::vector<std::string>* import_names =
        raw_debian_name_index_find(context.import_name_to_raw_names, requested_name);
    if (import_names) {
        for (const auto& raw_name : *import_names) append(raw_name);
    }

    return candidates;
}

bool raw_debian_result_satisfies_relation(
    const RawDebianAvailabilityResult& result,
    const RelationAtom& relation
) {
    if (!result.installable) return false;
    return debian_meta_satisfies_required_dependency(result.meta, relation);
}

bool should_prefer_raw_debian_availability(
    const RawDebianAvailabilityResult& candidate,
    const RawDebianAvailabilityResult& current
) {
    if (current.meta.name.empty()) return true;

    bool candidate_exact_requested = candidate.meta.name == candidate.requested_name;
    bool current_exact_requested = current.meta.name == current.requested_name;
    if (candidate_exact_requested != current_exact_requested) {
        return candidate_exact_requested;
    }

    return compare_versions(candidate.meta.version, current.meta.version) > 0;
}

RawDebianAvailabilityResult evaluate_raw_debian_exact_package_recursive(
    RawDebianContext& context,
    const std::string& raw_name,
    std::set<std::string>& walk,
    bool verbose
);

bool resolve_raw_debian_relation_candidate_recursive(
    const RelationAtom& relation,
    RawDebianContext& context,
    RawDebianAvailabilityResult& out_result,
    std::set<std::string>& walk,
    bool verbose,
    std::string* reason_out
);

bool load_raw_debian_compiled_record_cache_entry(
    RawDebianContext& context,
    const std::string& raw_name,
    DebianCompiledRecordCacheEntry& entry_out,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    entry_out = {};

    auto cached_it = context.raw_compiled_entry_cache.find(raw_name);
    if (cached_it != context.raw_compiled_entry_cache.end()) {
        entry_out = cached_it->second;
        return true;
    }

    const RawDebianOffsetIndexEntry* offset_entry =
        raw_debian_offset_index_find(context.raw_package_offsets, raw_name);
    if (!offset_entry) {
        if (error_out) *error_out = "package is absent from cached Debian metadata";
        return false;
    }

    std::ifstream in(context.compiled_cache_path, std::ios::binary);
    if (!in) {
        if (error_out) *error_out = "failed to open compiled record cache";
        return false;
    }
    in.seekg(static_cast<std::streamoff>(offset_entry->offset), std::ios::beg);
    if (!in) {
        if (error_out) *error_out = "failed to seek compiled record cache";
        return false;
    }
    if (!read_debian_compiled_record_cache_entry_binary(in, entry_out)) {
        if (error_out) *error_out = "failed to decode compiled record cache entry";
        return false;
    }
    context.raw_compiled_entry_cache[raw_name] = entry_out;
    return true;
}

RawDebianAvailabilityResult evaluate_raw_debian_exact_package_recursive(
    RawDebianContext& context,
    const std::string& raw_name,
    std::set<std::string>& walk,
    bool verbose
) {
    auto cached_it = context.raw_exact_cache.find(raw_name);
    if (cached_it != context.raw_exact_cache.end()) return cached_it->second;

    RawDebianAvailabilityResult result;
    result.requested_name = raw_name;

    if (!context.available) {
        result.reason = context.problem.empty()
            ? "cached Debian metadata is unavailable; run 'gpkg update'"
            : context.problem;
        return result;
    }

    DebianCompiledRecordCacheEntry cache_entry;
    std::string entry_error;
    if (!load_raw_debian_compiled_record_cache_entry(
            context,
            raw_name,
            cache_entry,
            &entry_error
        )) {
        result.reason = entry_error.empty()
            ? "package is absent from cached Debian metadata"
            : entry_error;
        return result;
    }

    result.found = true;
    result.meta = cache_entry.meta;
    if (result.meta.name.empty()) {
        result.meta.name = raw_name;
        result.meta.debian_package = cache_entry.raw_package;
        result.meta.source_kind = "debian";
        result.meta.source_url = context.config.base_url;
    }
    result.resolved_name = result.meta.name;

    if (!cache_entry.importable) {
        result.reason = cache_entry.skip_reason.empty()
            ? "required dependency closure is unsatisfied"
            : cache_entry.skip_reason;
        context.raw_exact_cache[raw_name] = result;
        return result;
    }

    if (!walk.insert(raw_name).second) {
        result.installable = true;
        return result;
    }

    for (const auto& dep_str : result.meta.depends) {
        RelationAtom dep = normalize_relation_atom(dep_str, "any");
        if (!dep.valid || dep.name.empty()) continue;
        if (is_system_provided(dep.name, dep.op, dep.version)) continue;

        RawDebianAvailabilityResult dep_result;
        std::string dep_reason;
        if (!resolve_raw_debian_relation_candidate_recursive(
                dep,
                context,
                dep_result,
                walk,
                verbose,
                &dep_reason
            )) {
            result.reason = dep_reason.empty()
                ? ("required dependency closure is unsatisfied via " + dep_str)
                : dep_reason;
            walk.erase(raw_name);
            context.raw_exact_cache[raw_name] = result;
            return result;
        }
    }

    walk.erase(raw_name);
    result.installable = true;
    result.reason.clear();
    context.raw_exact_cache[raw_name] = result;
    VLOG(verbose, "Raw Debian package " << raw_name
                 << " is available for on-demand install as " << result.meta.name
                 << " (" << result.meta.version << ")");
    return result;
}

bool resolve_raw_debian_relation_candidate_recursive(
    const RelationAtom& relation,
    RawDebianContext& context,
    RawDebianAvailabilityResult& out_result,
    std::set<std::string>& walk,
    bool verbose,
    std::string* reason_out
) {
    out_result = {};
    if (reason_out) reason_out->clear();

    if (!context.available) {
        if (reason_out) {
            *reason_out = context.problem.empty()
                ? "cached Debian metadata is unavailable; run 'gpkg update'"
                : context.problem;
        }
        return false;
    }

    bool found_any_candidate = false;
    RawDebianAvailabilityResult best_result;
    std::string best_reason;

    for (const auto& raw_name : collect_raw_debian_exact_candidate_names(relation.name, context)) {
        RawDebianAvailabilityResult candidate =
            evaluate_raw_debian_exact_package_recursive(context, raw_name, walk, verbose);
        if (!candidate.found) continue;
        found_any_candidate = true;
        if (!raw_debian_result_satisfies_relation(candidate, relation)) {
            if (best_reason.empty()) {
                best_reason = candidate.reason.empty()
                    ? ("cached Debian candidate does not satisfy " + relation.name)
                    : candidate.reason;
            }
            continue;
        }
        if (!candidate.installable) {
            if (best_reason.empty()) best_reason = candidate.reason;
            continue;
        }
        if (!best_result.installable ||
            should_prefer_raw_debian_availability(candidate, best_result)) {
            best_result = candidate;
        }
    }

    const std::vector<std::string>* provider_names =
        raw_debian_name_index_find(context.provider_map, relation.name);
    if (provider_names) {
        for (const auto& provider_raw_name : *provider_names) {
            RawDebianAvailabilityResult candidate =
                evaluate_raw_debian_exact_package_recursive(context, provider_raw_name, walk, verbose);
            if (!candidate.found) continue;
            found_any_candidate = true;
            if (!raw_debian_result_satisfies_relation(candidate, relation)) {
                if (best_reason.empty()) {
                    best_reason = candidate.reason.empty()
                        ? ("no cached Debian provider satisfies " + relation.name)
                        : candidate.reason;
                }
                continue;
            }
            if (!candidate.installable) {
                if (best_reason.empty()) best_reason = candidate.reason;
                continue;
            }
            if (!best_result.installable ||
                should_prefer_raw_debian_availability(candidate, best_result)) {
                best_result = candidate;
            }
        }
    }

    if (best_result.installable) {
        out_result = best_result;
        return true;
    }

    if (reason_out) {
        if (!best_reason.empty()) *reason_out = best_reason;
        else if (found_any_candidate) *reason_out = "no cached Debian candidate satisfies the required relation";
        else *reason_out = "package is absent from cached Debian metadata";
    }
    return false;
}

bool ensure_raw_debian_context_loaded(
    RawDebianContext& context,
    bool verbose,
    std::string* error_out
) {
    if (error_out) error_out->clear();
    if (context.loaded) {
        if (error_out && !context.available) *error_out = context.problem;
        return context.available;
    }

    context = {};
    context.loaded = true;
    context.config = load_debian_backend_config(verbose);
    context.policy = get_import_policy(verbose);
    context.compiled_cache_path = get_debian_compiled_record_cache_path();

    std::string import_prepare_error;
    if (!ensure_current_debian_imported_index_cache(verbose, &import_prepare_error)) {
        context.problem = import_prepare_error.empty()
            ? "cached Debian metadata is stale or incomplete; run 'gpkg update'"
            : import_prepare_error;
        if (error_out) *error_out = context.problem;
        return false;
    }

    std::string index_error;
    if (!load_debian_raw_context_index(
            context.raw_package_offsets,
            context.import_name_to_raw_names,
            context.provider_map,
            &index_error
        )) {
        VLOG(verbose, "Raw Debian context index unavailable: " << index_error);
        std::string rebuild_error;
        if (!rebuild_debian_raw_context_index(verbose, &rebuild_error) ||
            !load_debian_raw_context_index(
                context.raw_package_offsets,
                context.import_name_to_raw_names,
                context.provider_map,
                &index_error
            )) {
            context.problem = rebuild_error.empty() ? index_error : rebuild_error;
            if (context.problem.empty()) {
                context.problem = "cached Debian metadata is stale or incomplete; run 'gpkg update'";
            }
            if (error_out) *error_out = context.problem;
            return false;
        }
    }

    context.available = true;
    if (error_out) error_out->clear();
    VLOG(verbose, "Loaded compact raw Debian context index with "
                 << context.raw_package_offsets.size() << " raw packages.");
    return true;
}

bool query_raw_debian_exact_package(
    const std::string& requested_name,
    RawDebianContext& context,
    RawDebianAvailabilityResult& out_result,
    bool verbose,
    std::string* reason_out
) {
    out_result = {};
    if (reason_out) reason_out->clear();

    std::string load_error;
    if (!ensure_raw_debian_context_loaded(context, verbose, &load_error)) {
        if (reason_out) *reason_out = load_error;
        return false;
    }

    std::string canonical_requested = canonicalize_package_name(requested_name, verbose);
    RawDebianAvailabilityResult best_result;
    std::string best_reason;
    bool found_any = false;
    std::set<std::string> walk;
    for (const auto& raw_name : collect_raw_debian_exact_candidate_names(canonical_requested, context)) {
        RawDebianAvailabilityResult candidate =
            evaluate_raw_debian_exact_package_recursive(context, raw_name, walk, verbose);
        if (!candidate.found) continue;
        candidate.requested_name = canonical_requested;
        found_any = true;
        if (!candidate.installable) {
            if (best_reason.empty()) best_reason = candidate.reason;
            if (best_result.meta.name.empty()) best_result = candidate;
            continue;
        }
        if (!best_result.installable ||
            should_prefer_raw_debian_availability(candidate, best_result)) {
            best_result = candidate;
        }
    }

    if (!found_any) {
        if (reason_out) *reason_out = "package is absent from cached Debian metadata";
        return false;
    }

    if (best_result.meta.name.empty()) {
        if (reason_out) *reason_out = best_reason;
        return false;
    }

    out_result = best_result;
    if (reason_out) *reason_out = best_result.reason;
    return true;
}

bool query_raw_debian_relation_availability(
    const std::string& requested_name,
    const std::string& op,
    const std::string& version,
    RawDebianContext& context,
    RawDebianAvailabilityResult& out_result,
    bool verbose,
    std::string* reason_out
) {
    out_result = {};
    if (reason_out) reason_out->clear();

    std::string load_error;
    if (!ensure_raw_debian_context_loaded(context, verbose, &load_error)) {
        if (reason_out) *reason_out = load_error;
        return false;
    }

    RelationAtom relation;
    relation.name = canonicalize_package_name(requested_name, verbose);
    relation.op = op;
    relation.version = version;
    relation.valid = !relation.name.empty();
    relation.normalized = relation.op.empty()
        ? relation.name
        : (relation.name + " (" + relation.op + " " + relation.version + ")");
    if (!relation.valid) {
        if (reason_out) *reason_out = "invalid package relation";
        return false;
    }

    bool found_any_candidate = false;
    RawDebianAvailabilityResult best_result;
    std::string best_reason;
    std::set<std::string> walk;

    auto consider_candidate = [&](const RawDebianAvailabilityResult& candidate, const std::string& fallback_reason) {
        if (!candidate.found) return;
        found_any_candidate = true;

        RawDebianAvailabilityResult normalized = candidate;
        normalized.requested_name = relation.name;

        if (raw_debian_result_satisfies_relation(candidate, relation) && candidate.installable) {
            if (!best_result.installable ||
                should_prefer_raw_debian_availability(normalized, best_result)) {
                best_result = normalized;
            }
            return;
        }

        if (best_reason.empty()) {
            best_reason = normalized.reason.empty() ? fallback_reason : normalized.reason;
        }
        if (best_result.meta.name.empty() ||
            (!best_result.installable &&
             should_prefer_raw_debian_availability(normalized, best_result))) {
            best_result = normalized;
        }
    };

    for (const auto& raw_name : collect_raw_debian_exact_candidate_names(relation.name, context)) {
        RawDebianAvailabilityResult candidate =
            evaluate_raw_debian_exact_package_recursive(context, raw_name, walk, verbose);
        consider_candidate(candidate, "cached Debian candidate does not satisfy " + relation.name);
    }

    const std::vector<std::string>* provider_names =
        raw_debian_name_index_find(context.provider_map, relation.name);
    if (provider_names) {
        for (const auto& provider_raw_name : *provider_names) {
            RawDebianAvailabilityResult candidate =
                evaluate_raw_debian_exact_package_recursive(context, provider_raw_name, walk, verbose);
            consider_candidate(candidate, "no cached Debian provider satisfies " + relation.name);
        }
    }

    if (!found_any_candidate) {
        if (reason_out) *reason_out = "package is absent from cached Debian metadata";
        return false;
    }

    if (best_result.meta.name.empty()) {
        if (reason_out) {
            *reason_out = best_reason.empty()
                ? "no cached Debian candidate satisfies the required relation"
                : best_reason;
        }
        return false;
    }

    if (!best_result.installable && best_result.reason.empty()) {
        best_result.reason = best_reason;
    }
    out_result = best_result;
    if (reason_out) *reason_out = best_result.reason.empty() ? best_reason : best_result.reason;
    return true;
}

bool resolve_raw_debian_relation_candidate(
    const std::string& requested_name,
    const std::string& op,
    const std::string& version,
    RawDebianContext& context,
    RawDebianAvailabilityResult& out_result,
    bool verbose,
    std::string* reason_out
) {
    RawDebianAvailabilityResult availability;
    std::string availability_reason;
    if (!query_raw_debian_relation_availability(
            requested_name,
            op,
            version,
            context,
            availability,
            verbose,
            &availability_reason
        )) {
        out_result = {};
        if (reason_out) *reason_out = availability_reason;
        return false;
    }

    out_result = availability;
    if (reason_out) *reason_out = availability_reason;
    return out_result.installable;
}

void invalidate_debian_search_preview_cache() {
    // Search preview metadata is streamed directly from disk now, so there is
    // no in-memory cache to invalidate here.
}

void populate_debian_search_preview_from_json(
    const std::string& obj,
    DebianSearchPreviewEntry& entry
) {
    entry = {};
    get_json_value(obj, "package", entry.meta.name);
    get_json_value(obj, "version", entry.meta.version);
    get_json_value(obj, "description", entry.meta.description);
    get_json_value(obj, "source_kind", entry.meta.source_kind);
    get_json_value(obj, "source_url", entry.meta.source_url);
    get_json_value(obj, "debian_package", entry.meta.debian_package);
    get_json_value(obj, "debian_version", entry.meta.debian_version);
    get_json_value(obj, "reason", entry.reason);
    get_json_array(obj, "raw_names", entry.raw_names);

    std::string installable_value;
    get_json_value(obj, "installable", installable_value);
    entry.installable = installable_value == "yes";
}

bool write_debian_search_preview_cache(
    const std::vector<DebianSearchPreviewEntry>& entries,
    std::string* error_out = nullptr
) {
    if (error_out) *error_out = "";

    std::string preview_binary_path = get_debian_search_preview_binary_cache_path();
    if (!write_debian_binary_cache(
            preview_binary_path,
            DEBIAN_PREVIEW_CACHE_MAGIC,
            entries,
            [](std::ostream& out, const DebianSearchPreviewEntry& entry) {
                return write_debian_search_preview_entry_binary(out, entry);
            },
            error_out
        )) {
        return false;
    }

    std::string preview_path = get_debian_search_preview_path();
    if (should_export_legacy_debian_json_caches()) {
        if (mkdir_parent(preview_path)) {
            std::string tmp_path = preview_path + ".tmp";
            std::ofstream out(tmp_path);
            if (out) {
                out << "[\n";
                for (size_t i = 0; i < entries.size(); ++i) {
                    if (i > 0) out << ",\n";
                    out << debian_search_preview_to_json(entries[i]);
                }
                out << "\n]\n";
                out.close();
                if (out) {
                    if (rename(tmp_path.c_str(), preview_path.c_str()) != 0) {
                        remove(tmp_path.c_str());
                    }
                } else {
                    remove(tmp_path.c_str());
                }
            }
        }
    } else {
        remove_optional_cache_export(preview_path);
    }

    invalidate_debian_search_preview_cache();
    return true;
}

std::string debian_cache_fingerprint_component(const std::string& path) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) return path + ":missing";

    std::ostringstream out;
    out << path << ":" << static_cast<long long>(st.st_size)
        << ":" << static_cast<long long>(st.st_mtime);
    return out.str();
}

std::string build_debian_imported_index_cache_fingerprint(
    const std::string& packages_path
) {
    return "bin1|" +
        debian_cache_fingerprint_component(packages_path) + "|" +
        debian_cache_fingerprint_component(IMPORT_POLICY_PATH) + "|" +
        debian_cache_fingerprint_component(DEBIAN_CONFIG_PATH) + "|" +
        debian_cache_fingerprint_component(SYSTEM_PROVIDES_PATH) + "|" +
        debian_cache_fingerprint_component(UPGRADEABLE_SYSTEM_PATH);
}

bool debian_imported_index_cache_is_current(
    const std::string& packages_path,
    std::string* fingerprint_out
) {
    if (fingerprint_out) fingerprint_out->clear();

    std::string cache_path = get_debian_imported_index_binary_cache_path();
    std::string state_path = get_debian_imported_index_state_path();
    if (access(cache_path.c_str(), F_OK) != 0 || access(state_path.c_str(), F_OK) != 0) {
        return false;
    }

    std::string fingerprint = build_debian_imported_index_cache_fingerprint(packages_path);
    DebianImportedIndexCacheState state;
    if (!load_debian_imported_index_cache_state(state_path, state)) return false;
    if (state.fingerprint != fingerprint) return false;

    if (fingerprint_out) *fingerprint_out = fingerprint;
    return true;
}

uint64_t fnv1a64_update_bytes(uint64_t hash, const void* data, size_t size) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<uint64_t>(bytes[i]);
        hash *= 1099511628211ULL;
    }
    return hash;
}

void fnv1a64_update_string(uint64_t& hash, const std::string& value) {
    hash = fnv1a64_update_bytes(hash, value.data(), value.size());
    const unsigned char separator = 0xff;
    hash = fnv1a64_update_bytes(hash, &separator, sizeof(separator));
}

std::string fnv1a64_hex_digest(const std::vector<std::string>& fields) {
    uint64_t hash = 1469598103934665603ULL;
    for (const auto& field : fields) {
        fnv1a64_update_string(hash, field);
    }

    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

std::string fingerprint_debian_package_record(const DebianPackageRecord& record) {
    return fnv1a64_hex_digest({
        record.package,
        record.version,
        record.architecture,
        record.maintainer,
        record.section,
        record.priority,
        record.filename,
        record.sha256,
        record.size,
        record.installed_size,
        record.depends_raw,
        record.pre_depends_raw,
        record.recommends_raw,
        record.suggests_raw,
        record.conflicts_raw,
        record.provides_raw,
        record.replaces_raw,
        record.description,
        record.essential ? "1" : "0",
    });
}

std::vector<std::string> collect_debian_record_provided_symbols(
    const DebianPackageRecord& record,
    const DebianBackendConfig& config
) {
    std::vector<std::string> provided = normalize_relation_field_value(record.provides_raw, config.apt_arch);
    append_debian_t64_legacy_provides(provided, record.package, record.version);
    provided.push_back(record.package);

    std::vector<std::string> symbols;
    symbols.reserve(provided.size());
    for (const auto& relation : provided) {
        RelationAtom atom = normalize_relation_atom(relation, config.apt_arch);
        if (!atom.valid || atom.name.empty()) continue;
        symbols.push_back(atom.name);
    }

    return unique_string_list(symbols);
}

void append_debian_dependency_watch_symbols_from_raw_value(
    const std::string& raw_value,
    const std::string& apt_arch,
    const ImportPolicy& policy,
    std::vector<std::string>& symbols
) {
    for (const auto& group : split_top_level_text(raw_value, ',')) {
        for (const auto& alternative : split_top_level_text(group, '|')) {
            RelationAtom atom = normalize_relation_atom(alternative, apt_arch);
            if (!atom.valid || atom.name.empty()) continue;

            symbols.push_back(atom.name);
            std::string rewritten = apply_dependency_rewrite_name(
                atom.name,
                policy.dependency_rewrites,
                &policy.package_aliases
            );
            if (!rewritten.empty()) symbols.push_back(rewritten);
        }
    }
}

std::vector<std::string> collect_debian_record_dependency_watch_symbols(
    const DebianPackageRecord& record,
    const DebianBackendConfig& config,
    const ImportPolicy& policy
) {
    PackageOverridePolicy package_override;
    auto override_it = policy.package_overrides.find(record.package);
    if (override_it != policy.package_overrides.end()) {
        package_override = override_it->second;
    }

    std::vector<std::string> symbols;
    append_debian_dependency_watch_symbols_from_raw_value(
        apply_dependency_removals_to_raw_value(
            record.pre_depends_raw,
            package_override,
            config.apt_arch,
            policy
        ),
        config.apt_arch,
        policy,
        symbols
    );
    append_debian_dependency_watch_symbols_from_raw_value(
        apply_dependency_removals_to_raw_value(
            record.depends_raw,
            package_override,
            config.apt_arch,
            policy
        ),
        config.apt_arch,
        policy,
        symbols
    );
    append_debian_dependency_watch_symbols_from_raw_value(
        apply_dependency_removals_to_raw_value(
            record.recommends_raw,
            package_override,
            config.apt_arch,
            policy
        ),
        config.apt_arch,
        policy,
        symbols
    );
    append_debian_dependency_watch_symbols_from_raw_value(
        apply_dependency_removals_to_raw_value(
            record.suggests_raw,
            package_override,
            config.apt_arch,
            policy
        ),
        config.apt_arch,
        policy,
        symbols
    );

    return unique_string_list(symbols);
}

bool dependency_watch_symbols_intersect(
    const std::vector<std::string>& watch_symbols,
    const std::set<std::string>& changed_symbols
) {
    for (const auto& symbol : watch_symbols) {
        if (changed_symbols.count(symbol) != 0) return true;
    }
    return false;
}

std::string build_debian_parsed_record_cache_schema_fingerprint(const DebianBackendConfig& config) {
    return "phase1|" +
        debian_cache_fingerprint_component(DEBIAN_CONFIG_PATH) + "|" +
        config.packages_url + "|" +
        config.apt_arch;
}

std::string build_debian_parsed_record_packages_fingerprint(const std::string& packages_path) {
    return "pkg1|" + debian_cache_fingerprint_component(packages_path);
}

std::string build_debian_compiled_record_cache_policy_fingerprint() {
    return "phase3|" +
        debian_cache_fingerprint_component(IMPORT_POLICY_PATH) + "|" +
        debian_cache_fingerprint_component(DEBIAN_CONFIG_PATH) + "|" +
        debian_cache_fingerprint_component(SYSTEM_PROVIDES_PATH) + "|" +
        debian_cache_fingerprint_component(UPGRADEABLE_SYSTEM_PATH);
}

bool debian_compiled_record_cache_is_current() {
    std::string cache_path = get_debian_compiled_record_cache_path();
    std::string state_path = get_debian_compiled_record_state_path();
    if (access(cache_path.c_str(), F_OK) != 0 || access(state_path.c_str(), F_OK) != 0) {
        return false;
    }

    DebianCompiledRecordCacheState state;
    if (!load_debian_compiled_record_cache_state(state_path, state)) return false;
    return state.policy_fingerprint == build_debian_compiled_record_cache_policy_fingerprint();
}

std::string describe_debian_cache_rebuild_reason(
    const std::string& cache_name,
    const std::string& reason
) {
    if (reason.empty()) return "";

    if (reason.find("is unavailable") != std::string::npos) {
        return cache_name + " not present yet; building it for future updates.";
    }

    if (reason.find("does not match") != std::string::npos ||
        reason.find("fingerprint does not match") != std::string::npos) {
        return cache_name + " no longer matches the current inputs; rebuilding it.";
    }

    return cache_name + " could not be reused (" + reason + "); rebuilding it.";
}

std::string describe_debian_cache_rebuild_reason(
    const std::string& cache_name,
    const std::string& reason
);

bool load_debian_parsed_record_cache_state(
    const std::string& path,
    DebianParsedRecordCacheState& state
) {
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
        if (key == "SCHEMA_FINGERPRINT") state.schema_fingerprint = value;
        else if (key == "PACKAGES_FINGERPRINT") state.packages_fingerprint = value;
        else if (key == "RECORD_COUNT") state.record_count = static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
    }

    return !state.schema_fingerprint.empty();
}

bool save_debian_parsed_record_cache_state(
    const std::string& path,
    const DebianParsedRecordCacheState& state
) {
    if (!mkdir_parent(path)) return false;

    std::string temp_path = path + ".tmp";
    std::ofstream out(temp_path, std::ios::trunc);
    if (!out) return false;

    out << "SCHEMA_FINGERPRINT=" << state.schema_fingerprint << "\n";
    out << "PACKAGES_FINGERPRINT=" << state.packages_fingerprint << "\n";
    out << "RECORD_COUNT=" << state.record_count << "\n";
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

bool load_debian_parsed_record_cache_entries(
    const std::string& schema_fingerprint,
    std::map<std::string, DebianParsedRecordCacheEntry>& entries_by_fingerprint,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    entries_by_fingerprint.clear();

    std::string cache_path = get_debian_parsed_record_cache_path();
    std::string state_path = get_debian_parsed_record_state_path();
    if (access(cache_path.c_str(), F_OK) != 0 || access(state_path.c_str(), F_OK) != 0) {
        if (error_out) *error_out = "parsed record cache is unavailable";
        return false;
    }

    DebianParsedRecordCacheState state;
    if (!load_debian_parsed_record_cache_state(state_path, state)) {
        if (error_out) *error_out = "parsed record cache state could not be read";
        return false;
    }
    if (state.schema_fingerprint != schema_fingerprint) {
        if (error_out) *error_out = "parsed record cache schema does not match";
        return false;
    }

    std::string cache_error;
    if (!foreach_debian_binary_cache_entry<DebianParsedRecordCacheEntry>(
            cache_path,
            DEBIAN_PARSED_RECORD_CACHE_MAGIC,
            [](std::istream& in, DebianParsedRecordCacheEntry& entry) {
                return read_debian_parsed_record_cache_entry_binary(in, entry);
            },
            [&](const DebianParsedRecordCacheEntry& entry) {
                if (entry.stanza_fingerprint.empty()) return true;
                entries_by_fingerprint[entry.stanza_fingerprint] = entry;
                return true;
            },
            &cache_error
        )) {
        entries_by_fingerprint.clear();
        if (error_out) *error_out = cache_error;
        return false;
    }

    return !entries_by_fingerprint.empty();
}

bool load_current_debian_parsed_record_cache(
    const std::string& packages_path,
    const DebianBackendConfig& config,
    std::vector<DebianPackageRecord>& records,
    std::string* error_out
) {
    if (error_out) error_out->clear();
    records.clear();

    std::string cache_path = get_debian_parsed_record_cache_path();
    std::string state_path = get_debian_parsed_record_state_path();
    if (access(cache_path.c_str(), F_OK) != 0 || access(state_path.c_str(), F_OK) != 0) {
        if (error_out) *error_out = "parsed record cache is unavailable";
        return false;
    }

    DebianParsedRecordCacheState state;
    if (!load_debian_parsed_record_cache_state(state_path, state)) {
        if (error_out) *error_out = "parsed record cache state could not be read";
        return false;
    }

    std::string schema_fingerprint = build_debian_parsed_record_cache_schema_fingerprint(config);
    if (state.schema_fingerprint != schema_fingerprint) {
        if (error_out) *error_out = "parsed record cache schema does not match";
        return false;
    }

    std::string packages_fingerprint = build_debian_parsed_record_packages_fingerprint(packages_path);
    if (state.packages_fingerprint != packages_fingerprint) {
        if (error_out) *error_out = "parsed record cache does not match the current Packages file";
        return false;
    }

    std::string cache_error;
    if (!foreach_debian_binary_cache_entry<DebianParsedRecordCacheEntry>(
            cache_path,
            DEBIAN_PARSED_RECORD_CACHE_MAGIC,
            [](std::istream& in, DebianParsedRecordCacheEntry& entry) {
                return read_debian_parsed_record_cache_entry_binary(in, entry);
            },
            [&](const DebianParsedRecordCacheEntry& entry) {
                records.push_back(entry.record);
                return true;
            },
            &cache_error
        )) {
        records.clear();
        if (error_out) *error_out = cache_error;
        return false;
    }

    if (records.empty()) {
        if (error_out) *error_out = "parsed record cache is empty";
        return false;
    }

    return true;
}

bool write_debian_parsed_record_cache(
    const std::vector<DebianParsedRecordCacheEntry>& entries,
    const std::string& schema_fingerprint,
    const std::string& packages_fingerprint,
    std::string* error_out = nullptr
) {
    if (error_out) *error_out = "";

    std::string cache_path = get_debian_parsed_record_cache_path();
    if (!write_debian_binary_cache(
            cache_path,
            DEBIAN_PARSED_RECORD_CACHE_MAGIC,
            entries,
            [](std::ostream& out, const DebianParsedRecordCacheEntry& entry) {
                return write_debian_parsed_record_cache_entry_binary(out, entry);
            },
            error_out
        )) {
        return false;
    }

    DebianParsedRecordCacheState state;
    state.schema_fingerprint = schema_fingerprint;
    state.packages_fingerprint = packages_fingerprint;
    state.record_count = entries.size();
    if (!save_debian_parsed_record_cache_state(get_debian_parsed_record_state_path(), state)) {
        if (error_out) *error_out = "failed to persist parsed record cache state";
        return false;
    }

    return true;
}

bool patch_binary_cache_entry_count(
    const std::string& path,
    uint32_t entry_count,
    std::string* error_out = nullptr
) {
    if (error_out) *error_out = "";

    std::fstream io(path, std::ios::binary | std::ios::in | std::ios::out);
    if (!io) {
        if (error_out) *error_out = "failed to reopen binary cache for patching";
        return false;
    }

    io.seekp(8 + 4, std::ios::beg);
    if (!io) {
        if (error_out) *error_out = "failed to seek to binary cache entry count";
        return false;
    }

    if (!write_binary_u32(io, entry_count)) {
        if (error_out) *error_out = "failed to patch binary cache entry count";
        return false;
    }

    io.flush();
    if (!io) {
        if (error_out) *error_out = "failed to flush patched binary cache";
        return false;
    }

    return true;
}

bool parsed_record_cache_matches_current_packages(
    const std::string& packages_path,
    const DebianBackendConfig& config,
    DebianParsedRecordCacheState* state_out = nullptr,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    if (state_out) *state_out = {};

    std::string cache_path = get_debian_parsed_record_cache_path();
    std::string state_path = get_debian_parsed_record_state_path();
    if (access(cache_path.c_str(), F_OK) != 0 || access(state_path.c_str(), F_OK) != 0) {
        if (error_out) *error_out = "parsed record cache is unavailable";
        return false;
    }

    DebianParsedRecordCacheState state;
    if (!load_debian_parsed_record_cache_state(state_path, state)) {
        if (error_out) *error_out = "parsed record cache state could not be read";
        return false;
    }

    std::string schema_fingerprint = build_debian_parsed_record_cache_schema_fingerprint(config);
    if (state.schema_fingerprint != schema_fingerprint) {
        if (error_out) *error_out = "parsed record cache schema does not match";
        return false;
    }

    std::string packages_fingerprint = build_debian_parsed_record_packages_fingerprint(packages_path);
    if (state.packages_fingerprint != packages_fingerprint) {
        if (error_out) *error_out = "parsed record cache does not match the current Packages file";
        return false;
    }

    if (state_out) *state_out = state;
    return true;
}

bool write_current_debian_parsed_record_cache_streaming(
    const std::string& packages_path,
    const DebianBackendConfig& config,
    bool verbose,
    CompactPackageAvailabilityIndex* compact_index_out = nullptr,
    std::string* error_out = nullptr
) {
    if (error_out) *error_out = "";
    if (compact_index_out) *compact_index_out = {};

    std::string cache_path = get_debian_parsed_record_cache_path();
    if (!mkdir_parent(cache_path)) {
        if (error_out) *error_out = "failed to create parsed record cache directory";
        return false;
    }

    std::string temp_path = cache_path + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error_out) *error_out = "failed to open parsed record cache for writing";
        return false;
    }

    if (!write_binary_cache_header(out, DEBIAN_PARSED_RECORD_CACHE_MAGIC, 0)) {
        out.close();
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to write parsed record cache header";
        return false;
    }

    std::vector<std::string> available_package_list;
    std::vector<std::pair<std::string, std::string>> provider_pairs;
    uint32_t entry_count = 0;
    std::string iterate_error;
    if (!for_each_debian_package_stanza_text(
            packages_path,
            [&](const std::string& stanza_text) {
                DebianPackageRecord record;
                if (!parse_debian_package_record_from_stanza(stanza_text, config, verbose, record)) {
                    return true;
                }

                DebianParsedRecordCacheEntry entry;
                entry.stanza_fingerprint = sha256_hex_digest(stanza_text);
                entry.record = std::move(record);
                if (compact_index_out) {
                    append_debian_record_to_compact_availability_index_builder(
                        entry.record,
                        config.apt_arch,
                        available_package_list,
                        provider_pairs
                    );
                }
                if (!write_debian_parsed_record_cache_entry_binary(out, entry)) {
                    iterate_error = "failed to encode parsed record cache entry " +
                                    std::to_string(static_cast<unsigned long long>(entry_count + 1));
                    return false;
                }
                ++entry_count;
                return true;
            },
            &iterate_error
        )) {
        out.close();
        remove(temp_path.c_str());
        if (error_out) *error_out = iterate_error.empty()
            ? "failed while building the parsed record cache"
            : iterate_error;
        return false;
    }

    out.close();
    if (!out) {
        remove(temp_path.c_str());
        if (error_out) *error_out = iterate_error.empty() ? "failed to flush parsed record cache" : iterate_error;
        return false;
    }

    std::string patch_error;
    if (!patch_binary_cache_entry_count(temp_path, entry_count, &patch_error)) {
        remove(temp_path.c_str());
        if (error_out) *error_out = patch_error;
        return false;
    }

    if (rename(temp_path.c_str(), cache_path.c_str()) != 0) {
        remove(temp_path.c_str());
        if (error_out) *error_out = strerror(errno);
        return false;
    }

    DebianParsedRecordCacheState state;
    state.schema_fingerprint = build_debian_parsed_record_cache_schema_fingerprint(config);
    state.packages_fingerprint = build_debian_parsed_record_packages_fingerprint(packages_path);
    state.record_count = entry_count;
    if (!save_debian_parsed_record_cache_state(get_debian_parsed_record_state_path(), state)) {
        if (error_out) *error_out = "failed to persist parsed record cache state";
        return false;
    }
    if (compact_index_out) {
        *compact_index_out = finalize_compact_package_availability_index(
            std::move(available_package_list),
            std::move(provider_pairs)
        );
    }

    VLOG(verbose, "Built Debian parsed-record cache in a streaming pass (" << entry_count
         << " record" << (entry_count == 1 ? "" : "s") << ").");
    return true;
}

bool ensure_current_debian_parsed_record_cache(
    const std::string& packages_path,
    const DebianBackendConfig& config,
    bool verbose,
    DebianParsedRecordCacheState* state_out,
    CompactPackageAvailabilityIndex* compact_index_out,
    std::string* error_out
) {
    if (compact_index_out) *compact_index_out = {};
    DebianParsedRecordCacheState state;
    std::string cache_problem;
    if (parsed_record_cache_matches_current_packages(packages_path, config, &state, &cache_problem)) {
        if (state_out) *state_out = state;
        if (error_out) error_out->clear();
        return true;
    }

    if (verbose && !cache_problem.empty()) {
        std::cout << "[DEBUG] "
                  << describe_debian_cache_rebuild_reason(
                         "Debian parsed-record cache",
                         cache_problem
                     )
                      << std::endl;
    }

    if (!write_current_debian_parsed_record_cache_streaming(
            packages_path,
            config,
            verbose,
            compact_index_out,
            error_out
        )) {
        return false;
    }

    if (!parsed_record_cache_matches_current_packages(packages_path, config, &state, error_out)) {
        return false;
    }

    if (state_out) *state_out = state;
    if (error_out) error_out->clear();
    return true;
}

template <typename Func>
bool foreach_current_debian_parsed_record(
    const std::string& packages_path,
    const DebianBackendConfig& config,
    Func callback,
    DebianParsedRecordCacheState* state_out,
    std::string* error_out
) {
    DebianParsedRecordCacheState state;
    if (!ensure_current_debian_parsed_record_cache(
            packages_path,
            config,
            false,
            &state,
            nullptr,
            error_out
        )) {
        return false;
    }

    std::string cache_error;
    bool callback_stopped = false;
    if (!foreach_debian_binary_cache_entry<DebianParsedRecordCacheEntry>(
            get_debian_parsed_record_cache_path(),
            DEBIAN_PARSED_RECORD_CACHE_MAGIC,
            [](std::istream& in, DebianParsedRecordCacheEntry& entry) {
                return read_debian_parsed_record_cache_entry_binary(in, entry);
            },
            [&](const DebianParsedRecordCacheEntry& entry) {
                bool keep_going = callback(entry.record);
                if (!keep_going) callback_stopped = true;
                return keep_going;
            },
            &cache_error
        )) {
        if (error_out) *error_out = cache_error;
        return false;
    }

    if (callback_stopped) {
        if (error_out && error_out->empty()) {
            *error_out = "streaming parsed-record iteration stopped early";
        }
        return false;
    }

    if (state_out) *state_out = state;
    if (error_out) error_out->clear();
    return true;
}

bool collect_debian_package_stanza_spans(
    const std::string& packages_path,
    std::vector<DebianStanzaSpan>& spans,
    std::string* error_out
) {
    if (error_out) error_out->clear();
    spans.clear();

    std::ifstream in(packages_path, std::ios::binary);
    if (!in) {
        if (error_out) *error_out = "failed to open Debian Packages file";
        return false;
    }

    bool in_stanza = false;
    std::streamoff stanza_start = 0;
    std::string raw_line;
    while (true) {
        std::streamoff line_start = in.tellg();
        if (!std::getline(in, raw_line)) {
            if (in.bad()) {
                if (error_out) *error_out = "failed while scanning Debian Packages file";
                return false;
            }

            std::streamoff file_end = in.tellg();
            if (file_end < 0) {
                in.clear();
                in.seekg(0, std::ios::end);
                file_end = in.tellg();
            }

            if (in_stanza && file_end >= stanza_start) {
                spans.push_back({stanza_start, static_cast<size_t>(file_end - stanza_start)});
            }
            break;
        }

        std::streamoff next_pos = in.tellg();
        if (next_pos < 0) {
            in.clear();
            in.seekg(0, std::ios::end);
            next_pos = in.tellg();
        }

        if (!raw_line.empty() && raw_line.back() == '\r') raw_line.pop_back();
        if (trim(raw_line).empty()) {
            if (in_stanza && line_start >= stanza_start) {
                spans.push_back({stanza_start, static_cast<size_t>(line_start - stanza_start)});
                in_stanza = false;
            }
            stanza_start = next_pos;
            continue;
        }

        if (!in_stanza) {
            stanza_start = line_start;
            in_stanza = true;
        }
    }

    return true;
}

bool read_debian_stanza_text(
    std::ifstream& in,
    const DebianStanzaSpan& span,
    std::string& text_out
) {
    text_out.clear();
    in.clear();
    in.seekg(span.offset);
    if (!in) return false;
    if (span.size == 0) return true;

    text_out.resize(span.size);
    in.read(&text_out[0], static_cast<std::streamsize>(span.size));
    return in.good() || (in.eof() && static_cast<size_t>(in.gcount()) == span.size);
}

DebianParsedRecordLoadResult load_debian_package_records_incremental(
    const std::string& packages_path,
    const DebianBackendConfig& config,
    bool verbose
) {
    DebianParsedRecordLoadResult result;

    std::string schema_fingerprint = build_debian_parsed_record_cache_schema_fingerprint(config);
    std::map<std::string, DebianParsedRecordCacheEntry> previous_entries_by_fingerprint;
    std::string cache_problem;
    bool have_previous_cache = load_debian_parsed_record_cache_entries(
        schema_fingerprint,
        previous_entries_by_fingerprint,
        &cache_problem
    );

    std::vector<DebianStanzaSpan> spans;
    std::string spans_error;
    if (!collect_debian_package_stanza_spans(packages_path, spans, &spans_error)) {
        if (verbose) {
            std::cout << "[DEBUG] Failed to prepare Debian Packages stanzas for parsing: "
                      << spans_error << std::endl;
        }
        return result;
    }

    if (spans.empty()) {
        if (verbose) {
            std::cout << "[DEBUG] Debian Packages file does not contain any package stanzas."
                      << std::endl;
        }
        return result;
    }

    const size_t worker_count = recommended_parallel_worker_count(spans.size());
    if (verbose) {
        std::cout << "[DEBUG] Parsing Debian Packages with "
                  << worker_count << " worker(s) across "
                  << spans.size() << " stanza(s)." << std::endl;
    }

    struct ParsedStanzaResult {
        bool has_record = false;
        DebianParsedRecordCacheEntry cache_entry;
    };

    std::vector<ParsedStanzaResult> parsed_results(spans.size());
    std::atomic<size_t> next_span{0};
    std::atomic<bool> worker_failed{false};
    std::mutex worker_error_mutex;
    std::string worker_error;
    std::vector<size_t> worker_reused(worker_count, 0);
    std::vector<size_t> worker_reparsed(worker_count, 0);

    auto worker = [&](size_t worker_index) {
        std::ifstream in(packages_path, std::ios::binary);
        if (!in) {
            std::lock_guard<std::mutex> lock(worker_error_mutex);
            if (worker_error.empty()) worker_error = "failed to open Debian Packages file";
            worker_failed.store(true);
            return;
        }

        std::string stanza_text;
        while (true) {
            if (worker_failed.load()) return;

            size_t span_index = next_span.fetch_add(1);
            if (span_index >= spans.size()) return;

            if (!read_debian_stanza_text(in, spans[span_index], stanza_text)) {
                std::lock_guard<std::mutex> lock(worker_error_mutex);
                if (worker_error.empty()) {
                    worker_error = "failed to read Debian Packages stanza " +
                                   std::to_string(span_index + 1);
                }
                worker_failed.store(true);
                return;
            }

            std::string stanza_fingerprint = sha256_hex_digest(stanza_text);
            auto previous_it = previous_entries_by_fingerprint.find(stanza_fingerprint);
            if (previous_it != previous_entries_by_fingerprint.end()) {
                parsed_results[span_index].has_record = true;
                parsed_results[span_index].cache_entry = previous_it->second;
                ++worker_reused[worker_index];
                continue;
            }

            DebianPackageRecord record;
            if (parse_debian_package_record_from_stanza(stanza_text, config, verbose, record)) {
                parsed_results[span_index].has_record = true;
                parsed_results[span_index].cache_entry.stanza_fingerprint = stanza_fingerprint;
                parsed_results[span_index].cache_entry.record = std::move(record);
            }

            ++worker_reparsed[worker_index];
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count > 0 ? worker_count - 1 : 0);
    for (size_t worker_index = 1; worker_index < worker_count; ++worker_index) {
        workers.emplace_back(worker, worker_index);
    }
    worker(0);
    for (auto& thread : workers) thread.join();

    if (worker_failed.load()) {
        if (verbose) {
            std::cout << "[DEBUG] Failed while parsing Debian Packages stanzas: "
                      << worker_error << std::endl;
        }
        return result;
    }

    for (size_t worker_index = 0; worker_index < worker_count; ++worker_index) {
        result.reused_records += worker_reused[worker_index];
        result.reparsed_records += worker_reparsed[worker_index];
    }

    result.cache_entries.reserve(spans.size());
    result.records.reserve(spans.size());
    for (const auto& parsed_result : parsed_results) {
        if (!parsed_result.has_record) continue;
        result.cache_entries.push_back(parsed_result.cache_entry);
        result.records.push_back(parsed_result.cache_entry.record);
    }

    std::string write_error;
    std::string packages_fingerprint = build_debian_parsed_record_packages_fingerprint(packages_path);
    if (!write_debian_parsed_record_cache(
            result.cache_entries,
            schema_fingerprint,
            packages_fingerprint,
            &write_error
        ) && verbose) {
        std::cout << "[DEBUG] Failed to refresh Debian parsed-record cache: "
                  << write_error << std::endl;
    } else if (verbose && !have_previous_cache && !cache_problem.empty()) {
        std::cout << "[DEBUG] "
                  << describe_debian_cache_rebuild_reason(
                         "Debian parsed-record cache",
                         cache_problem
                     )
                  << std::endl;
    }

    return result;
}

bool load_debian_imported_index_cache_state(
    const std::string& path,
    DebianImportedIndexCacheState& state
) {
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
        if (key == "FINGERPRINT") state.fingerprint = value;
        else if (key == "PACKAGE_COUNT") state.package_count = static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
    }

    return !state.fingerprint.empty();
}

bool save_debian_imported_index_cache_state(
    const std::string& path,
    const DebianImportedIndexCacheState& state
) {
    if (!mkdir_parent(path)) return false;

    std::string temp_path = path + ".tmp";
    std::ofstream out(temp_path, std::ios::trunc);
    if (!out) return false;

    out << "FINGERPRINT=" << state.fingerprint << "\n";
    out << "PACKAGE_COUNT=" << state.package_count << "\n";
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

bool load_debian_compiled_record_cache_state(
    const std::string& path,
    DebianCompiledRecordCacheState& state
) {
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
        if (key == "POLICY_FINGERPRINT") state.policy_fingerprint = value;
        else if (key == "RECORD_COUNT") state.record_count = static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
    }

    return !state.policy_fingerprint.empty();
}

bool save_debian_compiled_record_cache_state(
    const std::string& path,
    const DebianCompiledRecordCacheState& state
) {
    if (!mkdir_parent(path)) return false;

    std::string temp_path = path + ".tmp";
    std::ofstream out(temp_path, std::ios::trunc);
    if (!out) return false;

    out << "POLICY_FINGERPRINT=" << state.policy_fingerprint << "\n";
    out << "RECORD_COUNT=" << state.record_count << "\n";
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

bool load_debian_raw_context_index_state(
    const std::string& path,
    DebianRawContextIndexState& state
) {
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
        if (key == "COMPILED_CACHE_FINGERPRINT") state.compiled_cache_fingerprint = value;
        else if (key == "RAW_PACKAGE_COUNT") state.raw_package_count = static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
        else if (key == "IMPORT_NAME_COUNT") state.import_name_count = static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
        else if (key == "PROVIDER_COUNT") state.provider_count = static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
    }

    return !state.compiled_cache_fingerprint.empty();
}

bool save_debian_raw_context_index_state(
    const std::string& path,
    const DebianRawContextIndexState& state
) {
    if (!mkdir_parent(path)) return false;

    std::string temp_path = path + ".tmp";
    std::ofstream out(temp_path, std::ios::trunc);
    if (!out) return false;

    out << "COMPILED_CACHE_FINGERPRINT=" << state.compiled_cache_fingerprint << "\n";
    out << "RAW_PACKAGE_COUNT=" << state.raw_package_count << "\n";
    out << "IMPORT_NAME_COUNT=" << state.import_name_count << "\n";
    out << "PROVIDER_COUNT=" << state.provider_count << "\n";
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

const RawDebianOffsetIndexEntry* raw_debian_offset_index_find(
    const std::vector<RawDebianOffsetIndexEntry>& entries,
    const std::string& key
) {
    auto it = std::lower_bound(
        entries.begin(),
        entries.end(),
        key,
        [](const RawDebianOffsetIndexEntry& entry, const std::string& value) {
            return entry.key < value;
        }
    );
    if (it == entries.end() || it->key != key) return nullptr;
    return &(*it);
}

const std::vector<std::string>* raw_debian_name_index_find(
    const std::vector<RawDebianNameIndexEntry>& entries,
    const std::string& key
) {
    auto it = std::lower_bound(
        entries.begin(),
        entries.end(),
        key,
        [](const RawDebianNameIndexEntry& entry, const std::string& value) {
            return entry.key < value;
        }
    );
    if (it == entries.end() || it->key != key) return nullptr;
    return &it->raw_names;
}

bool append_raw_debian_name_index_candidate(
    std::map<std::string, std::vector<std::string>>& index,
    const std::string& key,
    const std::string& raw_name
) {
    if (key.empty() || raw_name.empty()) return true;
    auto& names = index[key];
    if (std::find(names.begin(), names.end(), raw_name) == names.end()) {
        names.push_back(raw_name);
    }
    return true;
}

std::vector<RawDebianNameIndexEntry> finalize_raw_debian_name_index_entries(
    const std::map<std::string, std::vector<std::string>>& source
) {
    std::vector<RawDebianNameIndexEntry> entries;
    entries.reserve(source.size());
    for (const auto& entry : source) {
        RawDebianNameIndexEntry index_entry;
        index_entry.key = entry.first;
        index_entry.raw_names = unique_string_list(entry.second);
        entries.push_back(std::move(index_entry));
    }
    return entries;
}

bool write_debian_raw_context_index(
    const std::vector<RawDebianOffsetIndexEntry>& raw_package_offsets,
    const std::vector<RawDebianNameIndexEntry>& import_name_to_raw_names,
    const std::vector<RawDebianNameIndexEntry>& provider_map,
    const std::string& compiled_cache_fingerprint,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    if (raw_package_offsets.size() > std::numeric_limits<uint32_t>::max() ||
        import_name_to_raw_names.size() > std::numeric_limits<uint32_t>::max() ||
        provider_map.size() > std::numeric_limits<uint32_t>::max()) {
        if (error_out) *error_out = "raw Debian context index is too large";
        return false;
    }

    std::string path = get_debian_raw_context_index_path();
    if (!mkdir_parent(path)) {
        if (error_out) *error_out = "failed to create raw Debian context index directory";
        return false;
    }

    std::string temp_path = path + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error_out) *error_out = "failed to open raw Debian context index for writing";
        return false;
    }

    if (!write_binary_exact(out, DEBIAN_RAW_CONTEXT_INDEX_MAGIC, 8) ||
        !write_binary_u32(out, DEBIAN_COMPILED_CACHE_VERSION) ||
        !write_binary_u32(out, static_cast<uint32_t>(raw_package_offsets.size())) ||
        !write_binary_u32(out, static_cast<uint32_t>(import_name_to_raw_names.size())) ||
        !write_binary_u32(out, static_cast<uint32_t>(provider_map.size()))) {
        out.close();
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to write raw Debian context index header";
        return false;
    }

    auto write_named_section = [&](const auto& entries, auto writer, const std::string& section_name) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (!writer(out, entries[i])) {
                if (error_out) {
                    *error_out = "failed to encode " + section_name + " entry " +
                        std::to_string(i + 1);
                }
                return false;
            }
        }
        return true;
    };

    if (!write_named_section(
            raw_package_offsets,
            [](std::ostream& stream, const RawDebianOffsetIndexEntry& entry) {
                return write_raw_debian_offset_index_entry_binary(stream, entry);
            },
            "raw-package index"
        ) ||
        !write_named_section(
            import_name_to_raw_names,
            [](std::ostream& stream, const RawDebianNameIndexEntry& entry) {
                return write_raw_debian_name_index_entry_binary(stream, entry);
            },
            "import-name index"
        ) ||
        !write_named_section(
            provider_map,
            [](std::ostream& stream, const RawDebianNameIndexEntry& entry) {
                return write_raw_debian_name_index_entry_binary(stream, entry);
            },
            "provider index"
        )) {
        out.close();
        remove(temp_path.c_str());
        return false;
    }

    out.close();
    if (!out) {
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to flush raw Debian context index";
        return false;
    }

    if (rename(temp_path.c_str(), path.c_str()) != 0) {
        remove(temp_path.c_str());
        if (error_out) *error_out = strerror(errno);
        return false;
    }

    DebianRawContextIndexState state;
    state.compiled_cache_fingerprint = compiled_cache_fingerprint;
    state.raw_package_count = raw_package_offsets.size();
    state.import_name_count = import_name_to_raw_names.size();
    state.provider_count = provider_map.size();
    if (!save_debian_raw_context_index_state(get_debian_raw_context_index_state_path(), state)) {
        if (error_out) *error_out = "failed to persist raw Debian context index state";
        return false;
    }

    return true;
}

bool load_debian_raw_context_index(
    std::vector<RawDebianOffsetIndexEntry>& raw_package_offsets,
    std::vector<RawDebianNameIndexEntry>& import_name_to_raw_names,
    std::vector<RawDebianNameIndexEntry>& provider_map,
    std::string* error_out
) {
    if (error_out) error_out->clear();

    raw_package_offsets.clear();
    import_name_to_raw_names.clear();
    provider_map.clear();

    std::string path = get_debian_raw_context_index_path();
    std::string state_path = get_debian_raw_context_index_state_path();
    std::string compiled_cache_path = get_debian_compiled_record_cache_path();
    if (access(path.c_str(), F_OK) != 0 ||
        access(state_path.c_str(), F_OK) != 0 ||
        access(compiled_cache_path.c_str(), F_OK) != 0) {
        if (error_out) *error_out = "raw Debian context index is unavailable";
        return false;
    }

    DebianRawContextIndexState state;
    if (!load_debian_raw_context_index_state(state_path, state)) {
        if (error_out) *error_out = "raw Debian context index state could not be read";
        return false;
    }

    std::string expected_fingerprint = debian_cache_fingerprint_component(compiled_cache_path);
    if (state.compiled_cache_fingerprint != expected_fingerprint) {
        if (error_out) *error_out = "raw Debian context index fingerprint does not match";
        return false;
    }

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error_out) *error_out = "failed to open raw Debian context index";
        return false;
    }

    char magic[8] = {};
    uint32_t version = 0;
    uint32_t raw_count = 0;
    uint32_t import_count = 0;
    uint32_t provider_count = 0;
    if (!read_binary_exact(in, magic, sizeof(magic)) ||
        std::memcmp(magic, DEBIAN_RAW_CONTEXT_INDEX_MAGIC, sizeof(magic)) != 0 ||
        !read_binary_u32(in, version) ||
        !read_binary_u32(in, raw_count) ||
        !read_binary_u32(in, import_count) ||
        !read_binary_u32(in, provider_count)) {
        if (error_out) *error_out = "failed to decode raw Debian context index header";
        return false;
    }
    if (version != DEBIAN_COMPILED_CACHE_VERSION) {
        if (error_out) *error_out = "raw Debian context index version is unsupported";
        return false;
    }

    auto read_section = [&](auto& entries, uint32_t count, auto reader, const std::string& section_name) {
        entries.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            typename std::decay<decltype(entries)>::type::value_type entry;
            if (!reader(in, entry)) {
                if (error_out) {
                    *error_out = "failed to decode " + section_name + " entry " +
                        std::to_string(i + 1);
                }
                return false;
            }
            entries.push_back(std::move(entry));
        }
        return true;
    };

    if (!read_section(
            raw_package_offsets,
            raw_count,
            [](std::istream& stream, RawDebianOffsetIndexEntry& entry) {
                return read_raw_debian_offset_index_entry_binary(stream, entry);
            },
            "raw-package index"
        ) ||
        !read_section(
            import_name_to_raw_names,
            import_count,
            [](std::istream& stream, RawDebianNameIndexEntry& entry) {
                return read_raw_debian_name_index_entry_binary(stream, entry);
            },
            "import-name index"
        ) ||
        !read_section(
            provider_map,
            provider_count,
            [](std::istream& stream, RawDebianNameIndexEntry& entry) {
                return read_raw_debian_name_index_entry_binary(stream, entry);
            },
            "provider index"
        )) {
        raw_package_offsets.clear();
        import_name_to_raw_names.clear();
        provider_map.clear();
        return false;
    }

    return true;
}

bool rebuild_debian_raw_context_index(
    bool verbose,
    std::string* error_out
) {
    if (error_out) error_out->clear();

    std::string compiled_cache_path = get_debian_compiled_record_cache_path();
    if (access(compiled_cache_path.c_str(), F_OK) != 0) {
        if (error_out) *error_out = "compiled record cache is unavailable";
        return false;
    }

    std::ifstream in(compiled_cache_path, std::ios::binary);
    if (!in) {
        if (error_out) *error_out = "failed to open compiled record cache";
        return false;
    }

    uint32_t entry_count = 0;
    if (!read_binary_cache_header(in, DEBIAN_COMPILED_RECORD_CACHE_MAGIC, entry_count, error_out)) {
        return false;
    }

    std::vector<RawDebianOffsetIndexEntry> raw_package_offsets;
    raw_package_offsets.reserve(entry_count);
    std::map<std::string, std::vector<std::string>> import_name_map;
    std::map<std::string, std::vector<std::string>> provider_map;

    for (uint32_t i = 0; i < entry_count; ++i) {
        std::streamoff offset_stream = in.tellg();
        if (offset_stream < 0) {
            if (error_out) *error_out = "failed to capture compiled record cache offset";
            return false;
        }

        DebianCompiledRecordCacheEntry entry;
        if (!read_debian_compiled_record_cache_entry_binary(in, entry)) {
            if (error_out) {
                *error_out = "failed to decode compiled record cache entry " +
                    std::to_string(i + 1);
            }
            return false;
        }
        if (entry.raw_package.empty()) continue;

        RawDebianOffsetIndexEntry offset_entry;
        offset_entry.key = entry.raw_package;
        offset_entry.offset = static_cast<uint64_t>(offset_stream);
        raw_package_offsets.push_back(std::move(offset_entry));

        const std::string import_name = entry.meta.name.empty() ? entry.raw_package : entry.meta.name;
        append_raw_debian_name_index_candidate(import_name_map, import_name, entry.raw_package);
        for (const auto& provided_symbol : entry.provided_symbols) {
            append_raw_debian_name_index_candidate(provider_map, provided_symbol, entry.raw_package);
        }
    }

    std::sort(
        raw_package_offsets.begin(),
        raw_package_offsets.end(),
        [](const RawDebianOffsetIndexEntry& lhs, const RawDebianOffsetIndexEntry& rhs) {
            return lhs.key < rhs.key;
        }
    );
    raw_package_offsets.erase(
        std::unique(
            raw_package_offsets.begin(),
            raw_package_offsets.end(),
            [](const RawDebianOffsetIndexEntry& lhs, const RawDebianOffsetIndexEntry& rhs) {
                return lhs.key == rhs.key;
            }
        ),
        raw_package_offsets.end()
    );

    std::vector<RawDebianNameIndexEntry> import_name_entries =
        finalize_raw_debian_name_index_entries(import_name_map);
    std::vector<RawDebianNameIndexEntry> provider_entries =
        finalize_raw_debian_name_index_entries(provider_map);
    std::string compiled_cache_fingerprint = debian_cache_fingerprint_component(compiled_cache_path);
    bool ok = write_debian_raw_context_index(
        raw_package_offsets,
        import_name_entries,
        provider_entries,
        compiled_cache_fingerprint,
        error_out
    );
    if (ok) {
        VLOG(verbose, "Rebuilt compact raw Debian context index with "
             << raw_package_offsets.size() << " raw packages.");
    }
    return ok;
}

bool write_debian_imported_index_cache(
    const std::vector<PackageMetadata>& entries,
    const std::string& fingerprint,
    std::string* error_out = nullptr
) {
    if (error_out) *error_out = "";

    std::string binary_cache_path = get_debian_imported_index_binary_cache_path();
    if (!write_debian_binary_cache(
            binary_cache_path,
            DEBIAN_IMPORTED_CACHE_MAGIC,
            entries,
            [](std::ostream& out, const PackageMetadata& meta) {
                return write_package_metadata_binary(out, meta);
            },
            error_out
        )) {
        return false;
    }

    DebianImportedIndexCacheState state;
    state.fingerprint = fingerprint;
    state.package_count = entries.size();
    if (!save_debian_imported_index_cache_state(get_debian_imported_index_state_path(), state)) {
        if (error_out) *error_out = "failed to persist imported index cache state";
        return false;
    }

    std::string cache_path = get_debian_imported_index_cache_path();
    if (should_export_legacy_debian_json_caches()) {
        if (mkdir_parent(cache_path)) {
            std::string temp_path = cache_path + ".tmp";
            std::ofstream out(temp_path);
            if (out) {
                out << "[\n";
                for (size_t i = 0; i < entries.size(); ++i) {
                    if (i > 0) out << ",\n";
                    out << package_metadata_to_json(entries[i]);
                }
                out << "\n]\n";
                out.close();
                if (out) {
                    if (rename(temp_path.c_str(), cache_path.c_str()) != 0) {
                        remove(temp_path.c_str());
                    }
                } else {
                    remove(temp_path.c_str());
                }
            }
        }
    } else {
        remove_optional_cache_export(cache_path);
    }

    return true;
}

bool load_debian_compiled_record_cache(
    const std::string& policy_fingerprint,
    std::map<std::string, DebianCompiledRecordCacheEntry>& entries_by_package,
    std::string* error_out
) {
    if (error_out) error_out->clear();
    entries_by_package.clear();

    std::string cache_path = get_debian_compiled_record_cache_path();
    std::string state_path = get_debian_compiled_record_state_path();
    if (access(cache_path.c_str(), F_OK) != 0 || access(state_path.c_str(), F_OK) != 0) {
        if (error_out) *error_out = "compiled record cache is unavailable";
        return false;
    }

    DebianCompiledRecordCacheState state;
    if (!load_debian_compiled_record_cache_state(state_path, state)) {
        if (error_out) *error_out = "compiled record cache state could not be read";
        return false;
    }
    if (state.policy_fingerprint != policy_fingerprint) {
        if (error_out) *error_out = "compiled record cache fingerprint does not match";
        return false;
    }

    std::string cache_error;
    if (!foreach_debian_binary_cache_entry<DebianCompiledRecordCacheEntry>(
            cache_path,
            DEBIAN_COMPILED_RECORD_CACHE_MAGIC,
            [](std::istream& in, DebianCompiledRecordCacheEntry& entry) {
                return read_debian_compiled_record_cache_entry_binary(in, entry);
            },
            [&](const DebianCompiledRecordCacheEntry& entry) {
                if (entry.raw_package.empty()) return true;
                entries_by_package[entry.raw_package] = entry;
                return true;
            },
            &cache_error
        )) {
        entries_by_package.clear();
        if (error_out) *error_out = cache_error;
        return false;
    }

    return !entries_by_package.empty();
}

bool write_debian_compiled_record_cache(
    const std::vector<DebianCompiledRecordCacheEntry>& entries,
    const std::string& policy_fingerprint,
    std::string* error_out = nullptr
) {
    if (error_out) *error_out = "";

    std::string cache_path = get_debian_compiled_record_cache_path();
    if (!write_debian_binary_cache(
            cache_path,
            DEBIAN_COMPILED_RECORD_CACHE_MAGIC,
            entries,
            [](std::ostream& out, const DebianCompiledRecordCacheEntry& entry) {
                return write_debian_compiled_record_cache_entry_binary(out, entry);
            },
            error_out
        )) {
        return false;
    }

    DebianCompiledRecordCacheState state;
    state.policy_fingerprint = policy_fingerprint;
    state.record_count = entries.size();
    if (!save_debian_compiled_record_cache_state(get_debian_compiled_record_state_path(), state)) {
        if (error_out) *error_out = "failed to persist compiled record cache state";
        return false;
    }

    return true;
}

bool begin_debian_compiled_record_cache_stream(
    DebianCompiledRecordCacheStreamWriter& writer,
    std::string* error_out
) {
    if (error_out) *error_out = "";
    writer = {};

    std::string cache_path = get_debian_compiled_record_cache_path();
    if (!mkdir_parent(cache_path)) {
        if (error_out) *error_out = "failed to create compiled record cache directory";
        return false;
    }

    writer.temp_path = cache_path + ".tmp";
    writer.out.open(writer.temp_path, std::ios::binary | std::ios::trunc);
    if (!writer.out) {
        if (error_out) *error_out = "failed to open compiled record cache for writing";
        return false;
    }

    if (!write_binary_cache_header(writer.out, DEBIAN_COMPILED_RECORD_CACHE_MAGIC, 0)) {
        writer.out.close();
        remove(writer.temp_path.c_str());
        if (error_out) *error_out = "failed to write compiled record cache header";
        return false;
    }

    return true;
}

bool append_debian_compiled_record_cache_stream_entry(
    DebianCompiledRecordCacheStreamWriter& writer,
    const DebianCompiledRecordCacheEntry& entry,
    std::string* error_out
) {
    if (error_out) *error_out = "";

    if (writer.entry_count >= std::numeric_limits<uint32_t>::max()) {
        if (error_out) *error_out = "compiled record cache is too large";
        return false;
    }

    if (!write_debian_compiled_record_cache_entry_binary(writer.out, entry)) {
        if (error_out) {
            *error_out = "failed to encode compiled record cache entry " +
                         std::to_string(static_cast<unsigned long long>(writer.entry_count + 1));
        }
        return false;
    }

    ++writer.entry_count;
    return true;
}

void abort_debian_compiled_record_cache_stream(
    DebianCompiledRecordCacheStreamWriter& writer
) {
    if (writer.out.is_open()) writer.out.close();
    if (!writer.temp_path.empty()) remove(writer.temp_path.c_str());
    writer = {};
}

bool finish_debian_compiled_record_cache_stream(
    DebianCompiledRecordCacheStreamWriter& writer,
    const std::string& policy_fingerprint,
    std::string* error_out
) {
    if (error_out) *error_out = "";

    if (!writer.out.is_open()) {
        if (error_out) *error_out = "compiled record cache stream is not open";
        return false;
    }

    writer.out.close();
    if (!writer.out) {
        remove(writer.temp_path.c_str());
        if (error_out) *error_out = "failed to flush compiled record cache";
        return false;
    }

    std::string patch_error;
    if (!patch_binary_cache_entry_count(
            writer.temp_path,
            static_cast<uint32_t>(writer.entry_count),
            &patch_error
        )) {
        remove(writer.temp_path.c_str());
        if (error_out) *error_out = patch_error;
        return false;
    }

    std::string cache_path = get_debian_compiled_record_cache_path();
    if (rename(writer.temp_path.c_str(), cache_path.c_str()) != 0) {
        remove(writer.temp_path.c_str());
        if (error_out) *error_out = strerror(errno);
        return false;
    }

    DebianCompiledRecordCacheState state;
    state.policy_fingerprint = policy_fingerprint;
    state.record_count = writer.entry_count;
    if (!save_debian_compiled_record_cache_state(get_debian_compiled_record_state_path(), state)) {
        if (error_out) *error_out = "failed to persist compiled record cache state";
        return false;
    }

    writer = {};
    return true;
}

bool ensure_current_debian_imported_index_cache(
    bool verbose,
    std::string* error_out
) {
    if (error_out) error_out->clear();

    DebianBackendConfig config = load_debian_backend_config(verbose);
    std::string packages_txt = get_debian_packages_cache_path();
    if (access(packages_txt.c_str(), F_OK) != 0) {
        if (error_out) *error_out = "cached Debian Packages index is unavailable; run 'gpkg update'";
        return false;
    }

    std::string import_cache_fingerprint;
    if (debian_imported_index_cache_is_current(packages_txt, &import_cache_fingerprint) &&
        debian_compiled_record_cache_is_current()) {
        return true;
    }
    if (import_cache_fingerprint.empty()) {
        import_cache_fingerprint = build_debian_imported_index_cache_fingerprint(packages_txt);
    }

    ImportPolicy policy = get_import_policy(verbose);
    DebianParsedRecordCacheState parsed_cache_state;
    CompactPackageAvailabilityIndex prepared_compact_index;
    const CompactPackageAvailabilityIndex* prepared_compact_index_ptr = nullptr;
    std::string parsed_cache_error;
    if (!ensure_current_debian_parsed_record_cache(
            packages_txt,
            config,
            verbose,
            &parsed_cache_state,
            &prepared_compact_index,
            &parsed_cache_error
        )) {
        VLOG(verbose, "Failed to prepare the Debian parsed-record cache: " << parsed_cache_error);
    } else if (!prepared_compact_index.available_packages.empty()) {
        prepared_compact_index_ptr = &prepared_compact_index;
    }

    DebianIncrementalImportResult incremental_import =
        parsed_cache_error.empty()
            ? load_debian_index_entries_from_current_parsed_cache_incremental(
                  packages_txt,
                  config,
                  policy,
                  prepared_compact_index_ptr,
                  verbose
              )
            : DebianIncrementalImportResult{};
    if (parsed_cache_error.empty() && incremental_import.processed_records == 0 &&
        parsed_cache_state.record_count > 0) {
        VLOG(verbose, "Streaming Debian import produced no compiled entries; falling back to the in-memory path.");
        std::vector<DebianPackageRecord> records = parse_debian_packages_file(packages_txt, config, verbose);
        incremental_import =
            load_debian_index_entries_from_records_incremental(records, config, policy, verbose);
    } else if (!parsed_cache_error.empty()) {
        std::vector<DebianPackageRecord> records = parse_debian_packages_file(packages_txt, config, verbose);
        incremental_import =
            load_debian_index_entries_from_records_incremental(records, config, policy, verbose);
    }

    std::vector<PackageMetadata>& entries = incremental_import.entries;
    std::string compiled_cache_error;
    bool compiled_cache_available = incremental_import.compiled_record_cache_written;
    if (!compiled_cache_available) {
        compiled_cache_available = write_debian_compiled_record_cache(
            incremental_import.compiled_record_entries,
            build_debian_compiled_record_cache_policy_fingerprint(),
            &compiled_cache_error
        );
        if (!compiled_cache_available) {
            std::cerr << Color::YELLOW
                      << "W: Failed to write Debian compiled record cache";
            if (!compiled_cache_error.empty()) std::cerr << " (" << compiled_cache_error << ")";
            std::cerr << ". On-demand Debian lookups may be slower."
                      << Color::RESET << std::endl;
        }
    }

    std::string import_cache_error;
    if (!write_debian_imported_index_cache(entries, import_cache_fingerprint, &import_cache_error)) {
        if (error_out) {
            *error_out = import_cache_error.empty()
                ? "failed to write imported Debian index cache"
                : import_cache_error;
        }
        return false;
    }

    return true;
}

bool materialize_imported_entries_from_compiled_cache(
    const std::map<std::string, ImportedPackageDependencyState>& selected,
    std::vector<PackageMetadata>& entries_out,
    std::string* error_out
) {
    if (error_out) error_out->clear();
    entries_out.clear();

    if (selected.empty()) return true;

    std::map<std::string, PackageMetadata> selected_entries;
    std::string cache_error;
    if (!foreach_debian_binary_cache_entry<DebianCompiledRecordCacheEntry>(
            get_debian_compiled_record_cache_path(),
            DEBIAN_COMPILED_RECORD_CACHE_MAGIC,
            [](std::istream& in, DebianCompiledRecordCacheEntry& entry) {
                return read_debian_compiled_record_cache_entry_binary(in, entry);
            },
            [&](const DebianCompiledRecordCacheEntry& entry) {
                if (!entry.importable || entry.meta.name.empty()) return true;

                auto selected_it = selected.find(entry.meta.name);
                if (selected_it == selected.end()) return true;
                if (entry.meta.version != selected_it->second.version) return true;

                PackageMetadata meta = entry.meta;
                meta.depends = selected_it->second.depends;
                meta.recommends = selected_it->second.recommends;
                meta.suggests = selected_it->second.suggests;
                meta.provides = selected_it->second.provides;

                auto existing_it = selected_entries.find(meta.name);
                if (existing_it == selected_entries.end() ||
                    compare_versions(meta.version, existing_it->second.version) > 0) {
                    selected_entries[meta.name] = std::move(meta);
                }
                return true;
            },
            &cache_error
        )) {
        if (error_out) *error_out = cache_error;
        return false;
    }

    entries_out.reserve(selected_entries.size());
    for (const auto& entry : selected_entries) {
        entries_out.push_back(entry.second);
    }

    if (entries_out.size() != selected.size()) {
        if (error_out) {
            *error_out = "compiled record cache did not contain every selected imported package";
        }
        return false;
    }

    return true;
}

bool append_cached_debian_imported_index(
    std::ofstream& merged,
    bool& first_object,
    int& total_packages,
    const std::string& fingerprint,
    bool verbose
) {
    std::string cache_path = get_debian_imported_index_binary_cache_path();
    std::string state_path = get_debian_imported_index_state_path();
    if (access(cache_path.c_str(), F_OK) != 0 ||
        access(state_path.c_str(), F_OK) != 0) {
        return false;
    }

    DebianImportedIndexCacheState state;
    if (!load_debian_imported_index_cache_state(state_path, state)) return false;
    if (state.fingerprint != fingerprint) return false;

    int appended_count = 0;
    std::string cache_error;
    if (!foreach_debian_binary_cache_entry<PackageMetadata>(
            cache_path,
            DEBIAN_IMPORTED_CACHE_MAGIC,
            [](std::istream& in, PackageMetadata& meta) {
                return read_package_metadata_binary(in, meta);
            },
            [&](const PackageMetadata& meta) {
                if (!first_object) merged << ",\n";
                merged << package_metadata_to_json(meta);
                first_object = false;
                ++appended_count;
                ++total_packages;
                return true;
            },
            &cache_error
        )) {
        VLOG(verbose, "Discarded cached imported Debian index: " << cache_error);
        return false;
    }

    if (appended_count <= 0) return false;

    VLOG(verbose, "Reused cached imported Debian compiled index (" << appended_count << " packages).");
    return true;
}

void canonicalize_debian_search_preview_entry(
    DebianSearchPreviewEntry& candidate,
    bool verbose
) {
    candidate.meta.name = canonicalize_package_name(candidate.meta.name, verbose);
    candidate.meta.debian_package = canonicalize_package_name(candidate.meta.debian_package, verbose);
    for (auto& raw_name : candidate.raw_names) {
        raw_name = canonicalize_package_name(raw_name, verbose);
    }
}

template <typename Callback>
bool foreach_debian_search_preview_entry(
    bool verbose,
    Callback callback,
    std::string* error_out
) {
    if (error_out) error_out->clear();

    std::string preview_binary_path = get_debian_search_preview_binary_cache_path();
    std::string preview_json_path = get_debian_search_preview_path();
    bool saw_any = false;

    if (access(preview_binary_path.c_str(), F_OK) == 0) {
        std::string binary_error;
        bool binary_ok = foreach_debian_binary_cache_entry<DebianSearchPreviewEntry>(
            preview_binary_path,
            DEBIAN_PREVIEW_CACHE_MAGIC,
            [](std::istream& in, DebianSearchPreviewEntry& candidate) {
                return read_debian_search_preview_entry_binary(in, candidate);
            },
            [&](DebianSearchPreviewEntry candidate) {
                canonicalize_debian_search_preview_entry(candidate, verbose);
                if (!candidate.meta.name.empty()) saw_any = true;
                return callback(candidate);
            },
            &binary_error
        );
        if (binary_ok && saw_any) return true;
        if (!binary_ok) {
            VLOG(verbose, "Discarded cached Debian search preview binary cache: "
                         << binary_error);
        }
        saw_any = false;
    }

    if (access(preview_json_path.c_str(), F_OK) == 0) {
        foreach_json_object(preview_json_path, [&](const std::string& obj) {
            DebianSearchPreviewEntry candidate;
            populate_debian_search_preview_from_json(obj, candidate);
            canonicalize_debian_search_preview_entry(candidate, verbose);
            if (!candidate.meta.name.empty()) saw_any = true;
            return callback(candidate);
        });
        if (saw_any) return true;
    }

    if (error_out) {
        if (access(preview_binary_path.c_str(), F_OK) != 0 &&
            access(preview_json_path.c_str(), F_OK) != 0) {
            *error_out = "cached Debian search preview is unavailable; run 'gpkg update'";
        } else {
            *error_out = "cached Debian search preview could not be parsed; run 'gpkg update'";
        }
    }
    return false;
}

bool get_debian_search_preview_exact_package(
    const std::string& requested_name,
    DebianSearchPreviewEntry& out_entry,
    bool verbose,
    std::string* error_out
) {
    out_entry = {};
    if (error_out) error_out->clear();

    std::string canonical_requested = canonicalize_package_name(requested_name, verbose);
    if (canonical_requested.empty()) {
        if (error_out) *error_out = "invalid package name";
        return false;
    }

    bool found = false;
    std::string preview_error;
    if (!foreach_debian_search_preview_entry(
            verbose,
            [&](const DebianSearchPreviewEntry& candidate) {
                bool matched = candidate.meta.name == canonical_requested;
                if (!matched) {
                    matched = std::find(
                        candidate.raw_names.begin(),
                        candidate.raw_names.end(),
                        canonical_requested
                    ) != candidate.raw_names.end();
                }
                if (!matched) return true;

                if (!found ||
                    should_prefer_debian_search_preview_candidate(candidate, out_entry)) {
                    out_entry = candidate;
                    found = true;
                }
                return true;
            },
            &preview_error
        )) {
        if (error_out) *error_out = preview_error;
        return false;
    }

    if (!found) {
        if (error_out) *error_out = "package is absent from cached Debian preview metadata";
        return false;
    }

    return true;
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
        bool pdiff_changed = false;
        size_t pdiff_patches_applied = 0;
        std::string pdiff_error;
        if (have_packages_txt) {
            if (try_update_debian_packages_with_pdiff(
                    config,
                    packages_txt,
                    verbose,
                    pdiff_changed,
                    pdiff_patches_applied,
                    &pdiff_error
                )) {
                needs_download = false;
                if (pdiff_changed) {
                    std::cout << Color::GREEN << "✓ Updated Debian Packages index via PDiff"
                              << " (" << pdiff_patches_applied << " patch"
                              << (pdiff_patches_applied == 1 ? "" : "es") << ")"
                              << Color::RESET << std::endl;
                } else {
                    VLOG(verbose, "Debian PDiff metadata confirmed that the cached Packages file is already current.");
                }
            } else if (verbose && !pdiff_error.empty()) {
                VLOG(verbose, "Unable to apply Debian PDiffs; falling back to full Packages download: "
                             << pdiff_error);
            }
        }
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

    std::string import_cache_fingerprint =
        build_debian_imported_index_cache_fingerprint(packages_txt);
    if (!needs_download) {
        int cached_total_before = total_packages;
        if (append_cached_debian_imported_index(
                merged,
                first_object,
                total_packages,
                import_cache_fingerprint,
                verbose
            )) {
            std::cout << Color::GREEN << "✓ Reused cached packages index"
                      << " (" << (total_packages - cached_total_before) << " packages)"
                      << Color::RESET << std::endl;
            return true;
        }
    }

    ImportPolicy policy = get_import_policy(verbose);
    std::vector<DebianPackageRecord> records = parse_debian_packages_file(packages_txt, config, verbose);
    DebianIncrementalImportResult incremental_import =
        load_debian_index_entries_from_records_incremental(records, config, policy, verbose);
    std::vector<PackageMetadata>& entries = incremental_import.entries;
    std::string compiled_cache_error;
    bool compiled_cache_available = write_debian_compiled_record_cache(
        incremental_import.compiled_record_entries,
        build_debian_compiled_record_cache_policy_fingerprint(),
        &compiled_cache_error
    );
    if (!compiled_cache_available) {
        std::cerr << Color::YELLOW
                  << "W: Failed to write Debian compiled record cache";
        if (!compiled_cache_error.empty()) std::cerr << " (" << compiled_cache_error << ")";
        std::cerr << ". Fine-grained Debian cache reuse will be unavailable."
                  << Color::RESET << std::endl;
    }
    std::vector<DebianSearchPreviewEntry> preview_entries;
    std::string preview_error;
    if (compiled_cache_available) {
        preview_entries = build_debian_search_preview_entries_from_compiled_cache(
            entries,
            verbose,
            &preview_error
        );
    } else {
        preview_entries = build_debian_search_preview_entries_from_records(
            records,
            config,
            policy,
            entries,
            incremental_import.skipped_policy
        );
    }
    if (!write_debian_search_preview_cache(preview_entries, &preview_error)) {
        std::cerr << Color::YELLOW
                  << "W: Failed to write Debian search preview cache";
        if (!preview_error.empty()) std::cerr << " (" << preview_error << ")";
        std::cerr << ". Search and install diagnostics may be slower."
                  << Color::RESET << std::endl;
    }
    std::string import_cache_error;
    if (!write_debian_imported_index_cache(entries, import_cache_fingerprint, &import_cache_error)) {
        std::cerr << Color::YELLOW
                  << "W: Failed to write imported Debian index cache";
        if (!import_cache_error.empty()) std::cerr << " (" << import_cache_error << ")";
        std::cerr << ". Future 'gpkg update' runs may be slower."
                  << Color::RESET << std::endl;
    }
    std::string raw_context_index_error;
    if (compiled_cache_available &&
        !rebuild_debian_raw_context_index(verbose, &raw_context_index_error)) {
        std::cerr << Color::YELLOW
                  << "W: Failed to write raw Debian context index";
        if (!raw_context_index_error.empty()) std::cerr << " (" << raw_context_index_error << ")";
        std::cerr << ". On-demand Debian lookups may be slower."
                  << Color::RESET << std::endl;
    }

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
    std::string base = REPO_CACHE_PATH + "imported/v8/"
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

bool locate_deb_member_archive(
    const std::string& directory,
    const std::string& member_prefix,
    std::string& out_path
) {
    out_path.clear();
    DIR* dir = opendir(directory.c_str());
    if (!dir) return false;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.rfind(member_prefix, 0) != 0) continue;
        out_path = directory + "/" + name;
        closedir(dir);
        return true;
    }

    closedir(dir);
    return false;
}

bool locate_deb_data_archive(const std::string& directory, std::string& out_path) {
    return locate_deb_member_archive(directory, "data.tar", out_path);
}

bool locate_deb_control_archive(const std::string& directory, std::string& out_path) {
    return locate_deb_member_archive(directory, "control.tar", out_path);
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

bool materialize_deb_tar_archive(
    const std::string& archive_path,
    const std::string& output_path,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    if (path_has_suffix(archive_path, ".tar")) {
        std::ifstream input(archive_path, std::ios::binary);
        std::ofstream output(output_path, std::ios::binary);
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
        return GpkgArchive::decompress_gzip_file(archive_path, output_path, error_out);
    }
    if (path_has_suffix(archive_path, ".tar.xz") || path_has_suffix(archive_path, ".tar.lzma")) {
        return GpkgArchive::decompress_xz_file(archive_path, output_path, error_out);
    }
    if (path_has_suffix(archive_path, ".tar.zst") || path_has_suffix(archive_path, ".tar.zstd")) {
        return GpkgArchive::decompress_zstd_file(archive_path, output_path, error_out);
    }

    if (error_out) *error_out = "unsupported Debian payload compression";
    return false;
}

bool materialize_deb_payload_tar(
    const std::string& archive_path,
    const std::string& temp_root,
    std::string& tar_path_out,
    std::string* error_out = nullptr
) {
    tar_path_out = temp_root + "/data.tar";
    return materialize_deb_tar_archive(archive_path, tar_path_out, error_out);
}

bool materialize_deb_control_tar(
    const std::string& archive_path,
    const std::string& temp_root,
    std::string& tar_path_out,
    std::string* error_out = nullptr
) {
    tar_path_out = temp_root + "/control.tar";
    return materialize_deb_tar_archive(archive_path, tar_path_out, error_out);
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
    const ImportPolicy& policy,
    const std::string& payload_root,
    const std::string& control_root,
    const std::string& output_path,
    bool verbose
) {
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
        remove_path_recursive(temp_root);
        return false;
    }

    std::string archive_error;
    if (!GpkgArchive::tar_create_from_directory(payload_root, data_tar, &archive_error)) {
        std::cerr << Color::RED << "E: Failed to stage the converted payload for "
                  << meta.name;
        if (!archive_error.empty()) std::cerr << ": " << archive_error;
        std::cerr << Color::RESET << std::endl;
        remove_path_recursive(temp_root);
        return false;
    }

    if (!GpkgArchive::compress_zstd_file(data_tar, data_tar_zst, 10, &archive_error)) {
        std::cerr << Color::RED << "E: Failed to compress the converted payload for "
                  << meta.name;
        if (!archive_error.empty()) std::cerr << ": " << archive_error;
        std::cerr << Color::RESET << std::endl;
        remove_path_recursive(temp_root);
        return false;
    }

    std::vector<GpkgArchive::TarSource> top_level_sources = {
        {"control.json", control_json, GpkgArchive::TarEntryType::Regular, 0644, ""},
        {"data.tar.zst", data_tar_zst, GpkgArchive::TarEntryType::Regular, 0644, ""},
    };
    std::string policy_name = meta.debian_package.empty() ? meta.name : meta.debian_package;
    auto override_it = policy.package_overrides.find(policy_name);
    const PackageOverridePolicy* package_override =
        override_it == policy.package_overrides.end() ? nullptr : &override_it->second;
    const std::vector<std::string> maintainer_scripts = {"preinst", "postinst", "prerm", "postrm"};
    std::set<std::string> maintainer_script_names(
        maintainer_scripts.begin(),
        maintainer_scripts.end()
    );
    for (const auto& script_name : maintainer_scripts) {
        if (package_override &&
            std::find(
                package_override->drop_scripts.begin(),
                package_override->drop_scripts.end(),
                script_name
            ) != package_override->drop_scripts.end()) {
            VLOG(verbose, "Dropping imported Debian maintainer script "
                 << policy_name << ":" << script_name << " due to import policy.");
            continue;
        }

        std::string script_path = control_root + "/" + script_name;
        struct stat st {};
        if (stat(script_path.c_str(), &st) != 0) continue;
        if (!S_ISREG(st.st_mode)) {
            std::cerr << Color::YELLOW
                      << "W: Ignoring non-regular Debian maintainer script "
                      << policy_name << ":" << script_name << Color::RESET << std::endl;
            continue;
        }

        top_level_sources.push_back({
            "scripts/" + script_name,
            script_path,
            GpkgArchive::TarEntryType::Regular,
            st.st_mode & 07777,
            ""
        });
        VLOG(verbose, "Included Debian maintainer script "
             << policy_name << ":" << script_name << " in converted package.");
    }

    DIR* control_dir = opendir(control_root.c_str());
    if (!control_dir) {
        std::cerr << Color::RED << "E: Failed to inspect converted Debian control data for "
                  << meta.name << Color::RESET << std::endl;
        remove_path_recursive(temp_root);
        return false;
    }
    while (true) {
        errno = 0;
        dirent* entry = readdir(control_dir);
        if (!entry) break;

        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (maintainer_script_names.count(name) != 0) continue;

        std::string full_path = control_root + "/" + name;
        struct stat st {};
        if (stat(full_path.c_str(), &st) != 0) {
            closedir(control_dir);
            std::cerr << Color::RED
                      << "E: Failed to inspect Debian control sidecar "
                      << policy_name << ":" << name << Color::RESET << std::endl;
            remove_path_recursive(temp_root);
            return false;
        }
        if (!S_ISREG(st.st_mode)) {
            VLOG(verbose, "Skipping non-regular Debian control sidecar "
                 << policy_name << ":" << name);
            continue;
        }

        top_level_sources.push_back({
            "control/" + name,
            full_path,
            GpkgArchive::TarEntryType::Regular,
            st.st_mode & 07777,
            ""
        });
        VLOG(verbose, "Included Debian control sidecar "
             << policy_name << ":" << name << " in converted package.");
    }
    int control_errno = errno;
    closedir(control_dir);
    if (control_errno != 0) {
        std::cerr << Color::RED << "E: Failed while reading converted Debian control data for "
                  << meta.name << Color::RESET << std::endl;
        remove_path_recursive(temp_root);
        return false;
    }

    if (!GpkgArchive::tar_create_from_sources(top_level_sources, final_tar, &archive_error)) {
        std::cerr << Color::RED << "E: Failed to assemble the converted package for "
                  << meta.name;
        if (!archive_error.empty()) std::cerr << ": " << archive_error;
        std::cerr << Color::RESET << std::endl;
        remove_path_recursive(temp_root);
        return false;
    }

    bool ok = GpkgArchive::compress_zstd_file(final_tar, output_path, 10, &archive_error);
    if (!ok) {
        std::cerr << Color::RED << "E: Failed to write the converted package cache entry for "
                  << meta.name;
        if (!archive_error.empty()) std::cerr << ": " << archive_error;
        std::cerr << Color::RESET << std::endl;
    }
    remove_path_recursive(temp_root);
    return ok;
}

bool convert_debian_archive_to_gpkg(const PackageMetadata& meta, bool verbose, std::string* out_path = nullptr) {
    const ImportPolicy& policy = get_import_policy(verbose);
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
        remove_path_recursive(temp_root);
        return false;
    }

    std::string data_archive;
    if (!locate_deb_data_archive(temp_root, data_archive)) {
        std::cerr << Color::RED << "E: Could not locate data.tar.* inside " << deb_path << Color::RESET << std::endl;
        remove_path_recursive(temp_root);
        return false;
    }
    std::string control_archive;
    if (!locate_deb_control_archive(temp_root, control_archive)) {
        std::cerr << Color::RED << "E: Could not locate control.tar.* inside " << deb_path << Color::RESET << std::endl;
        remove_path_recursive(temp_root);
        return false;
    }

    std::string payload_root = temp_root + "/root";
    std::string control_root = temp_root + "/control";
    if (!mkdir_p(payload_root)) {
        remove_path_recursive(temp_root);
        return false;
    }
    if (!mkdir_p(control_root)) {
        remove_path_recursive(temp_root);
        return false;
    }

    std::string payload_tar;
    std::string materialize_error;
    if (!materialize_deb_payload_tar(data_archive, temp_root, payload_tar, &materialize_error)) {
        std::cerr << Color::RED << "E: Failed to prepare Debian payload for " << meta.name;
        if (!materialize_error.empty()) std::cerr << ": " << materialize_error;
        std::cerr << Color::RESET << std::endl;
        remove_path_recursive(temp_root);
        return false;
    }
    std::string control_tar;
    if (!materialize_deb_control_tar(control_archive, temp_root, control_tar, &materialize_error)) {
        std::cerr << Color::RED << "E: Failed to prepare Debian control archive for " << meta.name;
        if (!materialize_error.empty()) std::cerr << ": " << materialize_error;
        std::cerr << Color::RESET << std::endl;
        remove_path_recursive(temp_root);
        return false;
    }

    if (!GpkgArchive::tar_extract_to_directory(payload_tar, payload_root, {}, &archive_error)) {
        std::cerr << Color::RED << "E: Failed to extract the prepared Debian payload tar for "
                  << meta.name;
        if (!archive_error.empty()) std::cerr << ": " << archive_error;
        std::cerr << Color::RESET << std::endl;
        remove_path_recursive(temp_root);
        return false;
    }
    if (!GpkgArchive::tar_extract_to_directory(control_tar, control_root, {}, &archive_error)) {
        std::cerr << Color::RED << "E: Failed to extract the prepared Debian control tar for "
                  << meta.name;
        if (!archive_error.empty()) std::cerr << ": " << archive_error;
        std::cerr << Color::RESET << std::endl;
        remove_path_recursive(temp_root);
        return false;
    }

    if (!normalize_imported_payload_layout(payload_root, verbose)) {
        remove_path_recursive(temp_root);
        return false;
    }

    bool ok = build_imported_gpkg_archive(meta, policy, payload_root, control_root, output_path, verbose);
    remove_path_recursive(temp_root);
    return ok;
}
