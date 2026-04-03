// Repository configuration, index management, and repo-backed commands.

std::map<std::string, PackageMetadata> g_repo_package_cache;
struct RepoPackageLocator {
    std::string shard_path;
    uint64_t offset = 0;
};

std::map<std::string, RepoPackageLocator> g_repo_package_offsets;
std::map<std::string, std::vector<std::string>> g_repo_provider_cache;
std::set<std::string> g_repo_available_package_cache;
bool g_repo_package_cache_loaded = false;

const char REPO_RUNTIME_INDEX_MAGIC[8] = {'G','P','K','R','I','D','1','\0'};
const char REPO_CATALOG_SHARD_MAGIC[8] = {'G','P','K','S','H','D','2','\0'};
const char REPO_CATALOG_SHARD_INDEX_MAGIC[8] = {'G','P','K','S','I','D','1','\0'};

struct RepoIndexCacheState {
    std::string index_url;
    std::string etag;
    std::string last_modified;
    long content_length = -1;
    size_t package_count = 0;
};

struct RepoCatalogState {
    std::string fingerprint;
};

struct RepoCatalogShardState {
    std::string fingerprint;
    size_t package_count = 0;
};

struct RepoCatalogShardIndexEntry {
    std::string name;
    std::string version;
    std::string source_kind;
    uint64_t offset = 0;
    std::vector<std::string> provides;
};

struct RepoRuntimePackageEntry {
    std::string name;
    uint32_t shard_id = 0;
    uint64_t offset = 0;
};

struct RepoRuntimeProviderEntry {
    std::string capability;
    std::vector<std::string> package_names;
};

struct PackageUniverseResult {
    bool found = false;
    bool installable = false;
    bool raw_only = false;
    std::string reason;
    PackageMetadata meta;
};

bool ensure_repo_package_cache_loaded(bool verbose);
bool build_current_repo_catalog(bool verbose, std::string* error_out = nullptr);
bool ensure_current_upgrade_catalog(bool verbose, std::string* error_out = nullptr);
bool ensure_repo_index_available();
bool sync_debian_testing_index(
    bool verbose,
    bool* changed_out = nullptr,
    std::string* result_out = nullptr
);
void invalidate_repo_package_cache();
bool should_prefer_repo_candidate(const PackageMetadata& candidate, const PackageMetadata& current);
void populate_package_metadata_from_json(const std::string& obj, PackageMetadata& meta);
bool query_full_universe_exact_package(
    const std::string& pkg_name,
    PackageUniverseResult& out_result,
    bool verbose,
    RawDebianContext* raw_context = nullptr
);
bool resolve_full_universe_relation_candidate(
    const std::string& pkg_name,
    const std::string& op,
    const std::string& req_version,
    PackageUniverseResult& out_result,
    bool verbose,
    RawDebianContext* raw_context = nullptr,
    const PackageMetadata* installed_meta = nullptr,
    bool prefer_native_debian = false
);

std::string relation_name_from_text(const std::string& relation) {
    size_t open_paren = relation.find('(');
    return trim(open_paren == std::string::npos ? relation : relation.substr(0, open_paren));
}

std::string get_repo_catalog_path() {
    return REPO_CACHE_PATH + "Packages.catalog.bin";
}

std::string get_legacy_repo_index_path() {
    return REPO_CACHE_PATH + "Packages.json";
}

std::string get_repo_catalog_state_path() {
    return REPO_CACHE_PATH + "Packages.catalog.state";
}

std::string get_repo_source_cache_root(const std::string& repo_url);

std::string get_repo_source_catalog_shard_path(const std::string& repo_url) {
    return get_repo_source_cache_root(repo_url) + "Packages.catalog.bin";
}

std::string get_repo_source_catalog_shard_state_path(const std::string& repo_url) {
    return get_repo_source_cache_root(repo_url) + "Packages.catalog.state";
}

std::string get_repo_source_catalog_shard_index_path(const std::string& repo_url) {
    return get_repo_source_cache_root(repo_url) + "Packages.runtime.bin";
}

std::string get_debian_catalog_shard_path() {
    return REPO_CACHE_PATH + "debian/Packages.catalog.bin";
}

std::string get_debian_catalog_shard_state_path() {
    return REPO_CACHE_PATH + "debian/Packages.catalog.state";
}

std::string get_debian_catalog_shard_index_path() {
    return REPO_CACHE_PATH + "debian/Packages.runtime.bin";
}

std::string get_repo_source_cache_root(const std::string& repo_url) {
    std::string normalized = normalize_repo_base_url(repo_url);
    std::string suffix = sha256_hex_digest(normalized);
    if (suffix.size() > 16) suffix = suffix.substr(0, 16);
    return REPO_CACHE_PATH + "sources/" + cache_safe_component(normalized) + "_" + suffix + "/";
}

std::string get_repo_source_index_zst_path(const std::string& repo_url) {
    return get_repo_source_cache_root(repo_url) + "Packages.json.zst";
}

std::string get_repo_source_index_json_path(const std::string& repo_url) {
    return get_repo_source_cache_root(repo_url) + "Packages.json";
}

std::string get_repo_source_state_path(const std::string& repo_url) {
    return get_repo_source_cache_root(repo_url) + "Packages.state";
}

bool load_repo_index_cache_state(const std::string& path, RepoIndexCacheState& state) {
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
        if (key == "INDEX_URL") state.index_url = value;
        else if (key == "ETAG") state.etag = value;
        else if (key == "LAST_MODIFIED") state.last_modified = value;
        else if (key == "CONTENT_LENGTH") state.content_length = std::atol(value.c_str());
        else if (key == "PACKAGE_COUNT") {
            state.package_count = static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
        }
    }

    return !state.index_url.empty();
}

bool save_repo_index_cache_state(const std::string& path, const RepoIndexCacheState& state) {
    if (!mkdir_parent(path)) return false;

    std::string temp_path = path + ".tmp";
    std::ofstream out(temp_path, std::ios::trunc);
    if (!out) return false;

    out << "INDEX_URL=" << state.index_url << "\n";
    out << "ETAG=" << state.etag << "\n";
    out << "LAST_MODIFIED=" << state.last_modified << "\n";
    out << "CONTENT_LENGTH=" << state.content_length << "\n";
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

    return false;
}

bool load_repo_catalog_state(const std::string& path, RepoCatalogState& state) {
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
    }

    return !state.fingerprint.empty();
}

bool save_repo_catalog_state(const std::string& path, const RepoCatalogState& state) {
    if (!mkdir_parent(path)) return false;

    std::string temp_path = path + ".tmp";
    std::ofstream out(temp_path, std::ios::trunc);
    if (!out) return false;

    out << "FINGERPRINT=" << state.fingerprint << "\n";
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

bool load_repo_catalog_shard_state(const std::string& path, RepoCatalogShardState& state) {
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
        else if (key == "PACKAGE_COUNT") {
            state.package_count = static_cast<size_t>(std::strtoull(value.c_str(), nullptr, 10));
        }
    }

    return !state.fingerprint.empty();
}

bool save_repo_catalog_shard_state(const std::string& path, const RepoCatalogShardState& state) {
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

bool write_repo_catalog_shard_index_entry_binary(
    std::ostream& out,
    const RepoCatalogShardIndexEntry& entry
) {
    return
        write_binary_string(out, entry.name) &&
        write_binary_string(out, entry.version) &&
        write_binary_string(out, entry.source_kind) &&
        write_binary_u64(out, entry.offset) &&
        write_binary_string_vector(out, entry.provides);
}

bool read_repo_catalog_shard_index_entry_binary(
    std::istream& in,
    RepoCatalogShardIndexEntry& entry
) {
    entry = {};
    return
        read_binary_string(in, entry.name) &&
        read_binary_string(in, entry.version) &&
        read_binary_string(in, entry.source_kind) &&
        read_binary_u64(in, entry.offset) &&
        read_binary_string_vector(in, entry.provides);
}

bool write_repo_runtime_package_entry_binary(
    std::ostream& out,
    const RepoRuntimePackageEntry& entry
) {
    return
        write_binary_string(out, entry.name) &&
        write_binary_u32(out, entry.shard_id) &&
        write_binary_u64(out, entry.offset);
}

bool read_repo_runtime_package_entry_binary(
    std::istream& in,
    RepoRuntimePackageEntry& entry
) {
    entry = {};
    return
        read_binary_string(in, entry.name) &&
        read_binary_u32(in, entry.shard_id) &&
        read_binary_u64(in, entry.offset);
}

bool write_repo_runtime_provider_entry_binary(
    std::ostream& out,
    const RepoRuntimeProviderEntry& entry
) {
    return
        write_binary_string(out, entry.capability) &&
        write_binary_string_vector(out, entry.package_names);
}

bool read_repo_runtime_provider_entry_binary(
    std::istream& in,
    RepoRuntimeProviderEntry& entry
) {
    entry = {};
    return
        read_binary_string(in, entry.capability) &&
        read_binary_string_vector(in, entry.package_names);
}

bool repo_runtime_index_header_is_valid(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    uint32_t entry_count = 0;
    return read_binary_cache_header(in, REPO_RUNTIME_INDEX_MAGIC, entry_count, nullptr);
}

bool fetch_remote_repo_index_state(
    const std::string& url,
    RepoIndexCacheState& state,
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
    state.index_url = url;
    state.etag = extract_http_header_value(headers, lower_headers, "etag: ");
    state.last_modified = extract_http_header_value(headers, lower_headers, "last-modified: ");

    std::string content_length = extract_http_header_value(headers, lower_headers, "content-length: ");
    if (!content_length.empty()) state.content_length = std::atol(content_length.c_str());

    return true;
}

bool remote_repo_index_matches_cache(
    const RepoIndexCacheState& cached,
    const RepoIndexCacheState& remote
) {
    if (cached.index_url.empty() || remote.index_url.empty()) return false;
    if (cached.index_url != remote.index_url) return false;

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

size_t count_repo_index_packages(const std::string& path) {
    size_t count = 0;
    foreach_json_object(path, [&](const std::string& obj) {
        PackageMetadata candidate;
        populate_package_metadata_from_json(obj, candidate);
        candidate.name = trim(candidate.name);
        if (!candidate.name.empty()) ++count;
        return true;
    });
    return count;
}

bool sync_repo_source_index(
    const std::string& repo_url,
    bool verbose,
    bool* changed_out = nullptr,
    size_t* package_count_out = nullptr,
    std::string* result_out = nullptr
) {
    if (changed_out) *changed_out = false;
    if (package_count_out) *package_count_out = 0;
    if (result_out) result_out->clear();

    std::string normalized = normalize_repo_base_url(repo_url);
    std::string index_url = build_repo_index_url(normalized);
    std::string cached_json = get_repo_source_index_json_path(normalized);
    std::string cached_zst = get_repo_source_index_zst_path(normalized);
    std::string state_path = get_repo_source_state_path(normalized);
    if (!mkdir_parent(cached_json)) return false;

    RepoIndexCacheState cached_state;
    bool have_cached_state = load_repo_index_cache_state(state_path, cached_state);
    bool have_cached_json = access(cached_json.c_str(), F_OK) == 0;

    RepoIndexCacheState remote_state;
    std::string probe_error;
    bool have_remote_state = fetch_remote_repo_index_state(index_url, remote_state, verbose, &probe_error);

    if (have_cached_json && have_cached_state && have_remote_state &&
        remote_repo_index_matches_cache(cached_state, remote_state)) {
        if (package_count_out) *package_count_out = cached_state.package_count;
        if (result_out) {
            *result_out = "✓ Reused cached index from " + normalized +
                          " (" + std::to_string(cached_state.package_count) + " packages)";
        }
        return true;
    }

    std::string download_error;
    if (!DownloadFile(index_url, cached_zst, verbose, &download_error)) {
        if (have_cached_json) {
            size_t cached_count = have_cached_state ? cached_state.package_count : count_repo_index_packages(cached_json);
            if (package_count_out) *package_count_out = cached_count;
            if (result_out) {
                *result_out = "✓ Reused cached index from " + normalized +
                              " (" + std::to_string(cached_count) + " packages)";
            }
            return true;
        }
        if (result_out) {
            *result_out = "W: Failed to fetch index from " + normalized +
                          (download_error.empty() ? "" : " (" + download_error + ")");
        }
        return false;
    }

    std::string decompress_error;
    if (!GpkgArchive::decompress_zstd_file(cached_zst, cached_json, &decompress_error)) {
        remove(cached_zst.c_str());
        if (have_cached_json) {
            size_t cached_count = have_cached_state ? cached_state.package_count : count_repo_index_packages(cached_json);
            if (package_count_out) *package_count_out = cached_count;
            if (result_out) {
                *result_out = "✓ Reused cached index from " + normalized +
                              " (" + std::to_string(cached_count) + " packages)";
            }
            return true;
        }
        if (result_out) {
            *result_out = "W: Failed to decompress index from " + normalized +
                          (decompress_error.empty() ? "" : " (" + decompress_error + ")");
        }
        return false;
    }

    size_t package_count = count_repo_index_packages(cached_json);
    RepoIndexCacheState state_to_save = have_remote_state ? remote_state : RepoIndexCacheState{};
    state_to_save.index_url = index_url;
    state_to_save.package_count = package_count;
    if (!save_repo_index_cache_state(state_path, state_to_save)) {
        remove(state_path.c_str());
    }

    remove(cached_zst.c_str());
    if (changed_out) *changed_out = true;
    if (package_count_out) *package_count_out = package_count;
    if (result_out) {
        *result_out = "✓ Updated index from " + normalized +
                      " (" + std::to_string(package_count) + " packages)";
    }
    return true;
}

std::string build_repo_catalog_fingerprint(const std::vector<std::string>& urls) {
    std::vector<std::string> fields = {"repo-catalog-v1"};

    std::string packages_txt = get_debian_packages_cache_path();
    if (access(packages_txt.c_str(), F_OK) == 0) {
        fields.push_back(build_debian_imported_index_cache_fingerprint(packages_txt));
    } else {
        fields.push_back("debian:missing");
    }

    for (const auto& url : urls) {
        std::string normalized = normalize_repo_base_url(url);
        fields.push_back(normalized);
        fields.push_back(debian_cache_fingerprint_component(get_repo_source_index_json_path(normalized)));
    }

    return fnv1a64_hex_digest(fields);
}

std::string build_repo_source_catalog_shard_fingerprint(const std::string& repo_url) {
    std::string normalized = normalize_repo_base_url(repo_url);
    return fnv1a64_hex_digest({
        "repo-shard-v1",
        normalized,
        debian_cache_fingerprint_component(get_repo_source_index_json_path(normalized))
    });
}

std::string build_debian_catalog_shard_fingerprint(const std::string& packages_path) {
    return fnv1a64_hex_digest({
        "debian-shard-v1",
        build_debian_imported_index_cache_fingerprint(packages_path)
    });
}

bool repo_catalog_matches_current_sources(const std::vector<std::string>& urls) {
    std::string catalog_path = get_repo_catalog_path();
    std::string state_path = get_repo_catalog_state_path();
    if (access(catalog_path.c_str(), F_OK) != 0 || access(state_path.c_str(), F_OK) != 0) {
        return false;
    }
    if (!repo_runtime_index_header_is_valid(catalog_path)) return false;

    RepoCatalogState state;
    if (!load_repo_catalog_state(state_path, state)) return false;
    return state.fingerprint == build_repo_catalog_fingerprint(urls);
}

bool read_repo_shard_entry_at_offset(
    const std::string& path,
    uint64_t offset,
    PackageMetadata& meta,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    meta = {};

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error_out) *error_out = "failed to open package catalog shard";
        return false;
    }

    uint32_t entry_count = 0;
    if (!read_binary_cache_header(in, REPO_CATALOG_SHARD_MAGIC, entry_count, error_out)) return false;
    (void)entry_count;

    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in) {
        if (error_out) *error_out = "package catalog offset is out of range";
        return false;
    }

    if (!read_package_metadata_binary(in, meta)) {
        if (error_out) *error_out = "failed to decode package metadata from catalog";
        return false;
    }

    return true;
}

template <typename Callback>
bool foreach_repo_catalog_entry(
    Callback callback,
    std::string* error_out = nullptr
) {
    if (!ensure_repo_package_cache_loaded(false)) {
        if (error_out) *error_out = "failed to load the local package runtime index";
        return false;
    }

    for (const auto& entry : g_repo_package_offsets) {
        PackageMetadata meta;
        if (!read_repo_shard_entry_at_offset(entry.second.shard_path, entry.second.offset, meta, error_out)) {
            return false;
        }
        if (!callback(meta)) break;
    }

    return true;
}

bool write_repo_runtime_index(
    const std::vector<std::string>& shard_paths,
    const std::vector<RepoRuntimePackageEntry>& packages,
    const std::vector<RepoRuntimeProviderEntry>& providers,
    std::string* error_out = nullptr
) {
    if (error_out) *error_out = "";

    std::string runtime_path = get_repo_catalog_path();
    if (!mkdir_parent(runtime_path)) {
        if (error_out) *error_out = "failed to create package runtime index directory";
        return false;
    }

    if (packages.size() > std::numeric_limits<uint32_t>::max() ||
        providers.size() > std::numeric_limits<uint32_t>::max() ||
        shard_paths.size() > std::numeric_limits<uint32_t>::max()) {
        if (error_out) *error_out = "package runtime index is too large";
        return false;
    }

    std::string temp_path = runtime_path + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error_out) *error_out = "failed to open package runtime index for writing";
        return false;
    }

    if (!write_binary_exact(out, REPO_RUNTIME_INDEX_MAGIC, 8) ||
        !write_binary_u32(out, DEBIAN_COMPILED_CACHE_VERSION) ||
        !write_binary_u32(out, static_cast<uint32_t>(shard_paths.size())) ||
        !write_binary_u32(out, static_cast<uint32_t>(packages.size())) ||
        !write_binary_u32(out, static_cast<uint32_t>(providers.size()))) {
        out.close();
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to write package runtime index header";
        return false;
    }

    for (size_t i = 0; i < shard_paths.size(); ++i) {
        if (!write_binary_string(out, shard_paths[i])) {
            out.close();
            remove(temp_path.c_str());
            if (error_out) *error_out = "failed to encode package runtime shard path " + std::to_string(i + 1);
            return false;
        }
    }

    for (size_t i = 0; i < packages.size(); ++i) {
        if (!write_repo_runtime_package_entry_binary(out, packages[i])) {
            out.close();
            remove(temp_path.c_str());
            if (error_out) *error_out = "failed to encode package runtime package entry " + std::to_string(i + 1);
            return false;
        }
    }

    for (size_t i = 0; i < providers.size(); ++i) {
        if (!write_repo_runtime_provider_entry_binary(out, providers[i])) {
            out.close();
            remove(temp_path.c_str());
            if (error_out) *error_out = "failed to encode package runtime provider entry " + std::to_string(i + 1);
            return false;
        }
    }

    out.close();
    if (!out) {
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to flush package runtime index";
        return false;
    }

    if (rename(temp_path.c_str(), runtime_path.c_str()) != 0) {
        remove(temp_path.c_str());
        if (error_out) *error_out = strerror(errno);
        return false;
    }

    return true;
}

bool write_repo_catalog_shard(
    const std::string& path,
    const std::vector<PackageMetadata>& entries,
    std::string* error_out = nullptr
) {
    return write_debian_binary_cache(
        path,
        REPO_CATALOG_SHARD_MAGIC,
        entries,
        [](std::ostream& out, const PackageMetadata& meta) {
            return write_package_metadata_binary(out, meta);
        },
        error_out
    );
}

bool write_repo_catalog_shard_index(
    const std::string& path,
    const std::vector<RepoCatalogShardIndexEntry>& entries,
    std::string* error_out = nullptr
) {
    return write_debian_binary_cache(
        path,
        REPO_CATALOG_SHARD_INDEX_MAGIC,
        entries,
        [](std::ostream& out, const RepoCatalogShardIndexEntry& entry) {
            return write_repo_catalog_shard_index_entry_binary(out, entry);
        },
        error_out
    );
}

bool read_repo_catalog_shard_index(
    const std::string& path,
    std::vector<RepoCatalogShardIndexEntry>& entries,
    std::string* error_out = nullptr
) {
    entries.clear();
    return foreach_debian_binary_cache_entry<RepoCatalogShardIndexEntry>(
        path,
        REPO_CATALOG_SHARD_INDEX_MAGIC,
        [](std::istream& in, RepoCatalogShardIndexEntry& entry) {
            return read_repo_catalog_shard_index_entry_binary(in, entry);
        },
        [&](const RepoCatalogShardIndexEntry& entry) {
            entries.push_back(entry);
            return true;
        },
        error_out
    );
}

bool build_repo_catalog_shard_index(
    const std::string& shard_path,
    const std::string& index_path,
    std::string* error_out = nullptr
) {
    if (error_out) *error_out = "";

    std::ifstream in(shard_path, std::ios::binary);
    if (!in) {
        if (error_out) *error_out = "failed to open package catalog shard";
        return false;
    }

    uint32_t entry_count = 0;
    if (!read_binary_cache_header(in, REPO_CATALOG_SHARD_MAGIC, entry_count, error_out)) return false;

    std::vector<RepoCatalogShardIndexEntry> entries;
    entries.reserve(entry_count);
    for (uint32_t index = 0; index < entry_count; ++index) {
        std::streamoff offset = in.tellg();
        if (offset < 0) {
            if (error_out) *error_out = "failed to locate package catalog shard entry offset";
            return false;
        }

        PackageMetadata meta;
        if (!read_package_metadata_binary(in, meta)) {
            if (error_out) *error_out = "failed to decode package catalog shard entry " + std::to_string(index + 1);
            return false;
        }
        if (meta.name.empty()) continue;

        RepoCatalogShardIndexEntry entry;
        entry.name = meta.name;
        entry.version = meta.version;
        entry.source_kind = meta.source_kind;
        entry.offset = static_cast<uint64_t>(offset);
        for (const auto& capability : meta.provides) {
            std::string relation_name = relation_name_from_text(capability);
            if (relation_name.empty()) continue;
            if (std::find(entry.provides.begin(), entry.provides.end(), relation_name) == entry.provides.end()) {
                entry.provides.push_back(relation_name);
            }
        }
        entries.push_back(std::move(entry));
    }

    return write_repo_catalog_shard_index(index_path, entries, error_out);
}

bool ensure_repo_catalog_shard_index_ready(
    const std::string& shard_path,
    const std::string& index_path,
    bool verbose,
    const std::string& label,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    if (access(shard_path.c_str(), F_OK) != 0) {
        if (error_out) *error_out = "package catalog shard is unavailable";
        return false;
    }

    if (access(index_path.c_str(), F_OK) == 0) {
        std::string read_error;
        std::vector<RepoCatalogShardIndexEntry> probe_entries;
        if (read_repo_catalog_shard_index(index_path, probe_entries, &read_error)) {
            return true;
        }
        VLOG(verbose, "Rebuilding stale package catalog shard index for "
             << label << ": " << read_error);
    }

    std::string build_error;
    if (!build_repo_catalog_shard_index(shard_path, index_path, &build_error)) {
        if (error_out) {
            *error_out = build_error.empty()
                ? "failed to build package catalog shard index"
                : build_error;
        }
        return false;
    }

    VLOG(verbose, "Rebuilt package catalog shard index for " << label << ".");
    return true;
}

bool migrate_legacy_repo_index_to_catalog(bool verbose, std::string* error_out = nullptr) {
    (void)verbose;
    if (error_out) error_out->clear();
    if (error_out) {
        *error_out = "legacy merged package index migration is no longer supported; run 'gpkg update'";
    }
    return false;
}

bool build_repo_catalog_runtime_index(
    std::map<std::string, RepoPackageLocator>& offsets_out,
    std::map<std::string, std::vector<std::string>>& providers_out,
    std::set<std::string>& package_names_out,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    offsets_out.clear();
    providers_out.clear();
    package_names_out.clear();

    std::ifstream in(get_repo_catalog_path(), std::ios::binary);
    if (!in) {
        if (error_out) *error_out = "failed to open package runtime index";
        return false;
    }

    char magic[8] = {};
    uint32_t version = 0;
    uint32_t shard_count = 0;
    uint32_t package_count = 0;
    uint32_t provider_count = 0;
    if (!read_binary_exact(in, magic, sizeof(magic)) ||
        std::memcmp(magic, REPO_RUNTIME_INDEX_MAGIC, sizeof(magic)) != 0 ||
        !read_binary_u32(in, version) ||
        !read_binary_u32(in, shard_count) ||
        !read_binary_u32(in, package_count) ||
        !read_binary_u32(in, provider_count)) {
        if (error_out) *error_out = "failed to decode package runtime index header";
        return false;
    }
    if (version != DEBIAN_COMPILED_CACHE_VERSION) {
        if (error_out) *error_out = "package runtime index version is unsupported";
        return false;
    }

    std::vector<std::string> shard_paths;
    shard_paths.reserve(shard_count);
    for (uint32_t i = 0; i < shard_count; ++i) {
        std::string shard_path;
        if (!read_binary_string(in, shard_path)) {
            if (error_out) *error_out = "failed to decode package runtime shard path " + std::to_string(i + 1);
            return false;
        }
        if (access(shard_path.c_str(), F_OK) != 0) {
            if (error_out) *error_out = "package runtime shard is missing: " + shard_path;
            return false;
        }
        shard_paths.push_back(std::move(shard_path));
    }

    for (uint32_t i = 0; i < package_count; ++i) {
        RepoRuntimePackageEntry entry;
        if (!read_repo_runtime_package_entry_binary(in, entry)) {
            if (error_out) *error_out = "failed to decode package runtime package entry " + std::to_string(i + 1);
            return false;
        }
        if (entry.name.empty()) continue;
        if (entry.shard_id >= shard_paths.size()) {
            if (error_out) *error_out = "package runtime package entry references an invalid shard id";
            return false;
        }
        offsets_out[entry.name] = {shard_paths[entry.shard_id], entry.offset};
        package_names_out.insert(entry.name);
    }

    for (uint32_t i = 0; i < provider_count; ++i) {
        RepoRuntimeProviderEntry entry;
        if (!read_repo_runtime_provider_entry_binary(in, entry)) {
            if (error_out) *error_out = "failed to decode package runtime provider entry " + std::to_string(i + 1);
            return false;
        }
        if (entry.capability.empty()) continue;
        providers_out[entry.capability] = std::move(entry.package_names);
    }

    return !offsets_out.empty();
}

bool repo_index_file_present() {
    return access(get_repo_catalog_path().c_str(), F_OK) == 0;
}

bool try_ensure_repo_package_cache_loaded(bool verbose) {
    if (g_repo_package_cache_loaded) return true;
    if (!repo_index_file_present()) return false;
    return ensure_repo_package_cache_loaded(verbose);
}

bool catalog_version_satisfies(
    const std::string& current_ver,
    const std::string& op,
    const std::string& req_version
) {
    if (op.empty()) return true;

    int cmp = compare_versions(current_ver, req_version);
    if (op == ">>" || op == ">") return cmp > 0;
    if (op == "<<") return cmp < 0;
    if (op == ">=") return cmp >= 0;
    if (op == "<=") return cmp <= 0;
    if (op == "=" || op == "==") return cmp == 0;
    return false;
}

bool catalog_meta_satisfies_relation(
    const std::string& package_name,
    const PackageMetadata& meta,
    const RelationAtom& relation
) {
    std::string canonical_package = canonicalize_package_name(package_name);
    std::string canonical_relation = canonicalize_package_name(relation.name);
    if (canonical_package == canonical_relation &&
        catalog_version_satisfies(meta.version, relation.op, relation.version)) {
        return true;
    }

    for (const auto& provided_relation : meta.provides) {
        RelationAtom provided = normalize_relation_atom(provided_relation, "any");
        if (!provided.valid) continue;
        if (canonicalize_package_name(provided.name) != canonical_relation) continue;
        if (relation.op.empty()) return true;
        if (!provided.version.empty() &&
            catalog_version_satisfies(provided.version, relation.op, relation.version)) {
            return true;
        }
    }

    return false;
}

bool get_loaded_repo_package_info(const std::string& pkg_name, PackageMetadata& out_meta) {
    auto it = g_repo_package_cache.find(pkg_name);
    if (it != g_repo_package_cache.end()) {
        out_meta = it->second;
        return true;
    }

    auto offset_it = g_repo_package_offsets.find(pkg_name);
    if (offset_it == g_repo_package_offsets.end()) return false;

    PackageMetadata meta;
    if (!read_repo_shard_entry_at_offset(
            offset_it->second.shard_path,
            offset_it->second.offset,
            meta
        )) {
        return false;
    }

    g_repo_package_cache[pkg_name] = meta;
    out_meta = meta;
    return true;
}

bool get_repo_package_info_uncached(const std::string& pkg_name, PackageMetadata& out_meta) {
    auto offset_it = g_repo_package_offsets.find(pkg_name);
    if (offset_it == g_repo_package_offsets.end()) return false;

    return read_repo_shard_entry_at_offset(
        offset_it->second.shard_path,
        offset_it->second.offset,
        out_meta
    );
}

std::string upgrade_catalog_file_fingerprint_component(const std::string& path) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) return path + ":missing";

    std::ostringstream out;
    out << path << ":" << static_cast<long long>(st.st_size)
        << ":" << static_cast<long long>(st.st_mtime);
    return out.str();
}

std::string build_upgrade_catalog_fingerprint() {
    return "v1|" +
        upgrade_catalog_file_fingerprint_component(get_repo_catalog_state_path()) + "|" +
        upgrade_catalog_file_fingerprint_component(IMPORT_POLICY_PATH) + "|" +
        upgrade_catalog_file_fingerprint_component(UPGRADE_COMPANIONS_PATH) + "|" +
        upgrade_catalog_file_fingerprint_component(BASE_DEBIAN_PACKAGE_REGISTRY_PATH) + "|" +
        upgrade_catalog_file_fingerprint_component(BASE_SYSTEM_REGISTRY_PATH) + "|" +
        upgrade_catalog_file_fingerprint_component(SYSTEM_PROVIDES_PATH) + "|" +
        upgrade_catalog_file_fingerprint_component(UPGRADEABLE_SYSTEM_PATH);
}

std::set<std::string> load_present_base_registry_package_names() {
    std::set<std::string> names;
    for (const auto& record : load_base_system_package_status_records()) {
        if (record.package.empty()) continue;
        if (!package_status_is_installed_like(record.status)) continue;

        std::string canonical_name = canonicalize_package_name(record.package);
        if (canonical_name.empty()) continue;

        PackageMetadata exact_meta;
        if (get_repo_package_info_uncached(canonical_name, exact_meta)) {
            names.insert(canonical_name);
            continue;
        }

        auto provider_it = g_repo_provider_cache.find(canonical_name);
        if (provider_it == g_repo_provider_cache.end()) continue;

        bool found_provider = false;
        PackageMetadata best_meta;
        std::string best_name;
        for (const auto& provider_name : provider_it->second) {
            PackageMetadata candidate;
            if (!get_repo_package_info_uncached(provider_name, candidate)) continue;
            if (!found_provider || should_prefer_repo_candidate(candidate, best_meta)) {
                found_provider = true;
                best_meta = candidate;
                best_name = candidate.name;
            }
        }
        if (found_provider && !best_name.empty()) {
            names.insert(best_name);
        }
    }
    return names;
}

const std::set<std::string>& get_loaded_repo_package_names() {
    return g_repo_available_package_cache;
}

RelationAtom apply_catalog_policy_resolution(const RelationAtom& relation) {
    RelationAtom resolved = relation;
    if (!resolved.valid || resolved.name.empty()) return resolved;

    const ImportPolicy& policy = get_import_policy();
    std::string rewritten_name = apply_dependency_rewrite_name(
        resolved.name,
        policy.dependency_rewrites,
        &policy.package_aliases
    );
    if (!rewritten_name.empty()) resolved.name = rewritten_name;

    std::string provider_name = resolve_provider_name(
        resolved.name,
        policy.provider_choices,
        g_repo_provider_cache,
        get_loaded_repo_package_names()
    );
    if (!provider_name.empty()) resolved.name = provider_name;

    resolved.normalized = resolved.op.empty()
        ? resolved.name
        : resolved.name + " (" + resolved.op + " " + resolved.version + ")";
    resolved.valid = !resolved.name.empty();
    return resolved;
}

std::vector<std::string> parse_upgrade_companion_tokens_for_catalog(const std::string& raw_value) {
    std::string normalized = raw_value;
    for (char& ch : normalized) {
        if (ch == ',' || ch == '\t') ch = ' ';
    }

    std::vector<std::string> tokens;
    std::set<std::string> seen;
    std::istringstream iss(normalized);
    std::string token;
    while (iss >> token) {
        if (seen.insert(token).second) tokens.push_back(token);
    }
    return tokens;
}

std::map<std::string, std::vector<std::string>> load_raw_upgrade_companions_for_catalog() {
    std::map<std::string, std::vector<std::string>> companions;
    std::ifstream f(UPGRADE_COMPANIONS_PATH);
    if (!f) return companions;

    std::string line;
    while (std::getline(f, line)) {
        size_t comment = line.find('#');
        if (comment != std::string::npos) line = line.substr(0, comment);
        line = trim(line);
        if (line.empty()) continue;

        size_t sep = line.find(':');
        if (sep == std::string::npos) sep = line.find('=');
        if (sep == std::string::npos) continue;

        std::string trigger = trim(line.substr(0, sep));
        std::string raw_companions = trim(line.substr(sep + 1));
        if (trigger.empty() || raw_companions.empty()) continue;

        auto parsed = parse_upgrade_companion_tokens_for_catalog(raw_companions);
        auto& entry = companions[trigger];
        std::set<std::string> seen(entry.begin(), entry.end());
        for (const auto& pkg : parsed) {
            if (seen.insert(pkg).second) entry.push_back(pkg);
        }
    }

    return companions;
}

void append_upgrade_catalog_skip_entry(
    ResolvedUpgradeCatalog& catalog,
    const std::string& kind,
    const std::string& configured_name,
    const std::string& reason,
    const std::string& trigger = "",
    const std::string& resolved_name = ""
) {
    UpgradeCatalogSkipEntry entry;
    entry.kind = kind;
    entry.trigger = trigger;
    entry.configured_name = configured_name;
    entry.resolved_name = resolved_name;
    entry.reason = reason;
    catalog.skipped_entries.push_back(entry);
}

UpgradeCatalogSkipEntry make_upgrade_catalog_skip_entry(
    const std::string& kind,
    const std::string& configured_name,
    const std::string& reason,
    const std::string& trigger = "",
    const std::string& resolved_name = ""
) {
    UpgradeCatalogSkipEntry entry;
    entry.kind = kind;
    entry.trigger = trigger;
    entry.configured_name = configured_name;
    entry.resolved_name = resolved_name;
    entry.reason = reason;
    return entry;
}

bool resolve_catalog_relation(
    const RelationAtom& relation,
    PackageMetadata& out_meta,
    std::string& out_name,
    std::string* reason_out = nullptr
) {
    out_meta = {};
    out_name.clear();
    if (reason_out) reason_out->clear();

    if (!relation.valid || relation.name.empty()) {
        if (reason_out) *reason_out = "invalid package relation";
        return false;
    }

    RelationAtom effective_relation = apply_catalog_policy_resolution(relation);
    std::string requested_name = canonicalize_package_name(effective_relation.name);
    PackageMetadata exact_meta;
    if (get_repo_package_info_uncached(requested_name, exact_meta) &&
        catalog_meta_satisfies_relation(requested_name, exact_meta, effective_relation)) {
        out_meta = exact_meta;
        out_name = exact_meta.name;
        return true;
    }

    auto provider_it = g_repo_provider_cache.find(requested_name);
    if (provider_it == g_repo_provider_cache.end()) {
        if (reason_out) *reason_out = "no repository package or provider candidate";
        return false;
    }

    bool found = false;
    PackageMetadata best_meta;
    std::string best_name;
    for (const auto& provider_name : provider_it->second) {
        PackageMetadata candidate;
        if (!get_repo_package_info_uncached(provider_name, candidate)) continue;
        if (!catalog_meta_satisfies_relation(provider_name, candidate, effective_relation)) continue;

        if (!found || should_prefer_repo_candidate(candidate, best_meta)) {
            best_meta = candidate;
            best_name = candidate.name;
            found = true;
        }
    }

    if (!found) {
        if (reason_out) *reason_out = "no repository candidate satisfies the required relation";
        return false;
    }

    out_meta = best_meta;
    out_name = best_name;
    return true;
}

struct UpgradeCatalogValidationCache {
    std::set<std::string> valid_packages;
    std::map<std::string, std::string> invalid_reasons;
};

bool validate_catalog_package_closure_recursive(
    const std::string& pkg_name,
    UpgradeCatalogValidationCache& cache,
    std::set<std::string>& walk
) {
    if (cache.valid_packages.count(pkg_name) != 0) return true;
    auto invalid_it = cache.invalid_reasons.find(pkg_name);
    if (invalid_it != cache.invalid_reasons.end()) return false;
    if (!walk.insert(pkg_name).second) return true;

    PackageMetadata meta;
    if (!get_repo_package_info_uncached(pkg_name, meta)) {
        cache.invalid_reasons[pkg_name] = "repository metadata is missing";
        walk.erase(pkg_name);
        return false;
    }

    for (const auto& dep_str : collect_required_transaction_dependency_edges(meta)) {
        RelationAtom dep = normalize_relation_atom(dep_str, "any");
        if (!dep.valid) continue;
        if (is_system_provided(dep.name, dep.op, dep.version)) continue;

        PackageMetadata dep_meta;
        std::string dep_name;
        std::string resolve_reason;
        if (!resolve_catalog_relation(dep, dep_meta, dep_name, &resolve_reason)) {
            cache.invalid_reasons[pkg_name] =
                "missing required dependency " + dep_str + " for " + pkg_name;
            walk.erase(pkg_name);
            return false;
        }

        if (!validate_catalog_package_closure_recursive(dep_name, cache, walk)) {
            auto child_invalid_it = cache.invalid_reasons.find(dep_name);
            if (child_invalid_it != cache.invalid_reasons.end()) {
                cache.invalid_reasons[pkg_name] = child_invalid_it->second;
            } else {
                cache.invalid_reasons[pkg_name] =
                    "required dependency closure failed via " + dep_name;
            }
            walk.erase(pkg_name);
            return false;
        }
    }

    walk.erase(pkg_name);
    cache.valid_packages.insert(pkg_name);
    return true;
}

bool validate_catalog_package_closure(
    const std::string& pkg_name,
    UpgradeCatalogValidationCache& cache,
    std::string* reason_out = nullptr
) {
    std::set<std::string> walk;
    bool ok = validate_catalog_package_closure_recursive(pkg_name, cache, walk);
    if (reason_out) {
        if (ok) reason_out->clear();
        else *reason_out = cache.invalid_reasons[pkg_name];
    }
    return ok;
}

struct EvaluatedUpgradeRootResult {
    bool emit_root = false;
    bool map_configured = false;
    bool has_skip = false;
    std::string raw_root;
    std::string resolved_name;
    UpgradeCatalogSkipEntry skip_entry;
};

EvaluatedUpgradeRootResult evaluate_upgrade_root_candidate(
    const std::string& raw_root,
    bool report_skip,
    const std::set<std::string>& present_base_packages,
    UpgradeCatalogValidationCache& validation_cache,
    bool verbose
) {
    EvaluatedUpgradeRootResult result;
    result.raw_root = raw_root;

    RelationAtom relation = normalize_relation_atom(raw_root, "any");
    if (!relation.valid || relation.name.empty()) {
        if (report_skip) {
            result.has_skip = true;
            result.skip_entry = make_upgrade_catalog_skip_entry(
                "root",
                raw_root,
                "invalid configured upgrade root"
            );
        }
        return result;
    }

    PackageMetadata resolved_meta;
    std::string resolved_name;
    std::string resolve_reason;
    if (!resolve_catalog_relation(relation, resolved_meta, resolved_name, &resolve_reason)) {
        RelationAtom effective_relation = apply_catalog_policy_resolution(relation);
        std::string canonical_root = canonicalize_package_name(effective_relation.name);
        if (report_skip && !canonical_root.empty() && present_base_packages.count(canonical_root) != 0) {
            VLOG(verbose, "Skipping base-only runtime family " << raw_root
                         << " because no repository upgrade candidate is configured.");
            return result;
        }
        if (report_skip) {
            result.has_skip = true;
            result.skip_entry = make_upgrade_catalog_skip_entry(
                "root",
                raw_root,
                resolve_reason
            );
        } else {
            VLOG(verbose, "Skipping unresolved base-system upgrade root " << raw_root
                         << ": " << resolve_reason);
        }
        return result;
    }

    std::string validation_reason;
    if (!validate_catalog_package_closure(resolved_name, validation_cache, &validation_reason)) {
        if (report_skip) {
            result.has_skip = true;
            result.skip_entry = make_upgrade_catalog_skip_entry(
                "root",
                raw_root,
                validation_reason,
                "",
                resolved_name
            );
        } else {
            VLOG(verbose, "Skipping invalid base-system upgrade root " << raw_root
                         << ": " << validation_reason);
        }
        return result;
    }

    result.emit_root = true;
    result.resolved_name = resolved_name;
    result.map_configured = report_skip;
    return result;
}

struct EvaluatedUpgradeCompanionResult {
    bool emit_companion = false;
    bool has_skip = false;
    std::string resolved_trigger;
    std::string resolved_name;
    UpgradeCatalogSkipEntry skip_entry;
};

EvaluatedUpgradeCompanionResult evaluate_upgrade_companion_candidate(
    const std::string& raw_companion,
    const std::string& resolved_trigger,
    UpgradeCatalogValidationCache& validation_cache
) {
    EvaluatedUpgradeCompanionResult result;
    result.resolved_trigger = resolved_trigger;

    RelationAtom relation = normalize_relation_atom(raw_companion, "any");
    if (!relation.valid || relation.name.empty()) {
        result.has_skip = true;
        result.skip_entry = make_upgrade_catalog_skip_entry(
            "companion",
            raw_companion,
            "invalid configured runtime companion",
            resolved_trigger
        );
        return result;
    }

    PackageMetadata resolved_meta;
    std::string resolved_name;
    std::string resolve_reason;
    if (!resolve_catalog_relation(relation, resolved_meta, resolved_name, &resolve_reason)) {
        result.has_skip = true;
        result.skip_entry = make_upgrade_catalog_skip_entry(
            "companion",
            raw_companion,
            resolve_reason,
            resolved_trigger
        );
        return result;
    }

    std::string validation_reason;
    if (!validate_catalog_package_closure(resolved_name, validation_cache, &validation_reason)) {
        result.has_skip = true;
        result.skip_entry = make_upgrade_catalog_skip_entry(
            "companion",
            raw_companion,
            validation_reason,
            resolved_trigger,
            resolved_name
        );
        return result;
    }

    if (resolved_name == resolved_trigger) return result;
    result.emit_companion = true;
    result.resolved_name = resolved_name;
    return result;
}

bool build_resolved_upgrade_catalog(
    ResolvedUpgradeCatalog& out_catalog,
    bool verbose,
    std::string* error_out = nullptr
) {
    out_catalog = {};
    if (error_out) error_out->clear();

    if (!ensure_repo_package_cache_loaded(verbose)) {
        if (error_out) *error_out = "repository package index could not be loaded";
        return false;
    }

    out_catalog.fingerprint = build_upgrade_catalog_fingerprint();

    const ImportPolicy& policy = get_import_policy(verbose);
    std::vector<std::string> raw_roots = policy.upgradeable_system.empty()
        ? load_pattern_entries_file(UPGRADEABLE_SYSTEM_PATH)
        : load_pattern_entries(policy.upgradeable_system);
    std::map<std::string, std::vector<std::string>> raw_companions =
        load_raw_upgrade_companions_for_catalog();
    std::set<std::string> present_base_packages = load_present_base_registry_package_names();
    std::map<std::string, std::string> resolved_root_by_configured;
    std::set<std::string> emitted_roots;
    struct RootTask {
        std::string raw_root;
        bool report_skip = false;
    };
    std::vector<RootTask> root_tasks;
    root_tasks.reserve(raw_roots.size() + present_base_packages.size());
    for (const auto& raw_root : raw_roots) {
        root_tasks.push_back({raw_root, true});
    }
    for (const auto& base_root : present_base_packages) {
        if (base_root.empty()) continue;
        if (is_blocked_import_package(base_root, verbose)) continue;
        root_tasks.push_back({base_root, false});
    }

    const size_t root_worker_count = recommended_parallel_worker_count(root_tasks.size());
    if (verbose && !root_tasks.empty()) {
        std::cout << "[DEBUG] Rebuilding upgrade catalog roots with "
                  << root_worker_count << " worker(s) across "
                  << root_tasks.size() << " candidate(s)." << std::endl;
    }

    std::vector<EvaluatedUpgradeRootResult> root_results(root_tasks.size());
    if (!root_tasks.empty()) {
        std::atomic<size_t> next_root{0};
        auto root_worker = [&](size_t) {
            UpgradeCatalogValidationCache validation_cache;
            while (true) {
                size_t task_index = next_root.fetch_add(1);
                if (task_index >= root_tasks.size()) return;
                const auto& task = root_tasks[task_index];
                root_results[task_index] = evaluate_upgrade_root_candidate(
                    task.raw_root,
                    task.report_skip,
                    present_base_packages,
                    validation_cache,
                    verbose
                );
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(root_worker_count > 0 ? root_worker_count - 1 : 0);
        for (size_t worker_index = 1; worker_index < root_worker_count; ++worker_index) {
            workers.emplace_back(root_worker, worker_index);
        }
        root_worker(0);
        for (auto& thread : workers) thread.join();
    }

    for (size_t i = 0; i < root_results.size(); ++i) {
        const auto& result = root_results[i];
        if (result.has_skip) out_catalog.skipped_entries.push_back(result.skip_entry);
        if (!result.emit_root) continue;
        if (result.map_configured) resolved_root_by_configured[result.raw_root] = result.resolved_name;
        if (emitted_roots.insert(result.resolved_name).second) {
            out_catalog.resolved_roots.push_back(result.resolved_name);
        }
    }

    struct CompanionTask {
        std::string resolved_trigger;
        std::string raw_companion;
    };
    std::vector<CompanionTask> companion_tasks;
    for (const auto& entry : raw_companions) {
        auto resolved_trigger_it = resolved_root_by_configured.find(entry.first);
        if (resolved_trigger_it == resolved_root_by_configured.end()) continue;
        const std::string& resolved_trigger = resolved_trigger_it->second;
        for (const auto& raw_companion : entry.second) {
            companion_tasks.push_back({resolved_trigger, raw_companion});
        }
    }

    const size_t companion_worker_count = recommended_parallel_worker_count(companion_tasks.size());
    if (verbose && !companion_tasks.empty()) {
        std::cout << "[DEBUG] Rebuilding upgrade catalog companions with "
                  << companion_worker_count << " worker(s) across "
                  << companion_tasks.size() << " candidate(s)." << std::endl;
    }

    std::vector<EvaluatedUpgradeCompanionResult> companion_results(companion_tasks.size());
    if (!companion_tasks.empty()) {
        std::atomic<size_t> next_companion{0};
        auto companion_worker = [&](size_t) {
            UpgradeCatalogValidationCache validation_cache;
            while (true) {
                size_t task_index = next_companion.fetch_add(1);
                if (task_index >= companion_tasks.size()) return;
                const auto& task = companion_tasks[task_index];
                companion_results[task_index] = evaluate_upgrade_companion_candidate(
                    task.raw_companion,
                    task.resolved_trigger,
                    validation_cache
                );
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(companion_worker_count > 0 ? companion_worker_count - 1 : 0);
        for (size_t worker_index = 1; worker_index < companion_worker_count; ++worker_index) {
            workers.emplace_back(companion_worker, worker_index);
        }
        companion_worker(0);
        for (auto& thread : workers) thread.join();
    }

    for (size_t i = 0; i < companion_results.size(); ++i) {
        const auto& result = companion_results[i];
        if (result.has_skip) out_catalog.skipped_entries.push_back(result.skip_entry);
        if (!result.emit_companion) continue;
        auto& resolved_list = out_catalog.resolved_companions[result.resolved_trigger];
        if (std::find(resolved_list.begin(), resolved_list.end(), result.resolved_name) == resolved_list.end()) {
            resolved_list.push_back(result.resolved_name);
        }
    }

    return true;
}

std::string upgrade_catalog_skip_entry_to_json(const UpgradeCatalogSkipEntry& entry) {
    std::vector<std::string> fields;
    fields.push_back(json_string_field("kind", entry.kind));
    fields.push_back(json_string_field("trigger", entry.trigger));
    fields.push_back(json_string_field("configured_name", entry.configured_name));
    fields.push_back(json_string_field("resolved_name", entry.resolved_name));
    fields.push_back(json_string_field("reason", entry.reason));
    return "{" + join_strings(fields, ",") + "}";
}

std::string upgrade_catalog_companion_map_to_json(
    const std::map<std::string, std::vector<std::string>>& companions
) {
    std::vector<std::string> fields;
    for (const auto& entry : companions) {
        fields.push_back(
            "\"" + json_escape(entry.first) + "\":" + json_array_from_strings(entry.second)
        );
    }
    return "{" + join_strings(fields, ",") + "}";
}

bool write_upgrade_catalog(
    const ResolvedUpgradeCatalog& catalog,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    if (!mkdir_parent(UPGRADE_CATALOG_PATH)) {
        if (error_out) *error_out = "failed to create parent directory for " + UPGRADE_CATALOG_PATH;
        return false;
    }

    std::string tmp_path = UPGRADE_CATALOG_PATH + ".tmp";
    std::ofstream out(tmp_path);
    if (!out) {
        if (error_out) *error_out = "failed to open " + tmp_path + " for writing";
        return false;
    }

    out << "{\n";
    out << "  " << json_string_field("fingerprint", catalog.fingerprint) << ",\n";
    out << "  \"resolved_roots\":" << json_array_from_strings(catalog.resolved_roots) << ",\n";
    out << "  \"resolved_companions\":"
        << upgrade_catalog_companion_map_to_json(catalog.resolved_companions) << ",\n";
    out << "  \"skipped_entries\":[";
    for (size_t i = 0; i < catalog.skipped_entries.size(); ++i) {
        if (i > 0) out << ",";
        out << "\n    " << upgrade_catalog_skip_entry_to_json(catalog.skipped_entries[i]);
    }
    if (!catalog.skipped_entries.empty()) out << "\n  ";
    out << "]\n";
    out << "}\n";
    out.close();

    if (rename(tmp_path.c_str(), UPGRADE_CATALOG_PATH.c_str()) != 0) {
        if (error_out) *error_out = "failed to replace " + UPGRADE_CATALOG_PATH;
        remove(tmp_path.c_str());
        return false;
    }

    return true;
}

bool ensure_current_upgrade_catalog(bool verbose, std::string* error_out) {
    if (error_out) error_out->clear();

    if (!ensure_repo_index_available()) {
        if (error_out) *error_out = "repository package index could not be loaded";
        return false;
    }

    std::string expected_fingerprint = build_upgrade_catalog_fingerprint();
    JsonValue root;
    if (load_json_document(UPGRADE_CATALOG_PATH, root)) {
        std::string current_fingerprint = json_string_or(json_object_get(root, "fingerprint"));
        if (!current_fingerprint.empty() && current_fingerprint == expected_fingerprint) {
            return true;
        }
    }

    ResolvedUpgradeCatalog catalog;
    std::string catalog_error;
    if (!build_resolved_upgrade_catalog(catalog, verbose, &catalog_error)) {
        if (error_out) *error_out = catalog_error.empty()
            ? "failed to build the runtime upgrade catalog"
            : catalog_error;
        return false;
    }
    if (!write_upgrade_catalog(catalog, &catalog_error)) {
        if (error_out) *error_out = catalog_error.empty()
            ? "failed to write the runtime upgrade catalog"
            : catalog_error;
        return false;
    }

    return true;
}

bool load_upgrade_catalog(
    ResolvedUpgradeCatalog& out_catalog,
    std::string* problem_out,
    bool verbose
) {
    (void)verbose;
    out_catalog = {};
    if (problem_out) problem_out->clear();

    JsonValue root;
    if (!load_json_document(UPGRADE_CATALOG_PATH, root)) {
        if (problem_out) {
            if (access(UPGRADE_CATALOG_PATH.c_str(), F_OK) == 0) {
                *problem_out = "upgrade catalog is unreadable; run 'gpkg update'";
            } else {
                *problem_out = "upgrade catalog is missing; run 'gpkg update'";
            }
        }
        return false;
    }

    out_catalog.fingerprint = json_string_or(json_object_get(root, "fingerprint"));
    if (out_catalog.fingerprint.empty()) {
        if (problem_out) *problem_out = "upgrade catalog is missing its fingerprint; run 'gpkg update'";
        return false;
    }

    std::string expected_fingerprint = build_upgrade_catalog_fingerprint();
    if (out_catalog.fingerprint != expected_fingerprint) {
        if (problem_out) *problem_out = "upgrade catalog is stale; run 'gpkg update'";
        return false;
    }

    out_catalog.resolved_roots = json_string_array(json_object_get(root, "resolved_roots"));

    if (const JsonValue* companions = json_object_get(root, "resolved_companions")) {
        if (companions->is_object()) {
            for (const auto& entry : companions->object_items) {
                out_catalog.resolved_companions[entry.first] = json_string_array(&entry.second);
            }
        }
    }

    if (const JsonValue* skipped_entries = json_object_get(root, "skipped_entries")) {
        if (skipped_entries->is_array()) {
            for (const auto& item : skipped_entries->array_items) {
                if (!item.is_object()) continue;
                UpgradeCatalogSkipEntry entry;
                entry.kind = json_string_or(json_object_get(item, "kind"));
                entry.trigger = json_string_or(json_object_get(item, "trigger"));
                entry.configured_name = json_string_or(json_object_get(item, "configured_name"));
                entry.resolved_name = json_string_or(json_object_get(item, "resolved_name"));
                entry.reason = json_string_or(json_object_get(item, "reason"));
                if (!entry.kind.empty() && !entry.configured_name.empty()) {
                    out_catalog.skipped_entries.push_back(entry);
                }
            }
        }
    }

    return true;
}

void invalidate_repo_package_cache() {
    g_repo_package_cache.clear();
    g_repo_package_offsets.clear();
    g_repo_provider_cache.clear();
    g_repo_available_package_cache.clear();
    g_repo_package_cache_loaded = false;
}

std::vector<std::string> get_repo_urls() {
    std::vector<std::string> urls;
    std::set<std::string> seen_urls;

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

    DIR* dir = opendir(SOURCES_DIR.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (!strstr(entry->d_name, ".list")) continue;
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
    auto urls = get_repo_urls();
    if (repo_catalog_matches_current_sources(urls)) return true;

    std::cerr << Color::RED
              << "E: Local package index is missing or stale. Run 'gpkg update' first."
              << Color::RESET << std::endl;
    return false;
}

bool remove_cache_tree_no_follow(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT;
    }

    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR* dir = opendir(path.c_str());
        if (!dir) return false;

        bool ok = true;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            if (!remove_cache_tree_no_follow(path + "/" + entry->d_name)) ok = false;
        }
        closedir(dir);
        if (rmdir(path.c_str()) != 0 && errno != ENOENT) ok = false;
        return ok;
    }

    return unlink(path.c_str()) == 0 || errno == ENOENT;
}

bool clear_repo_cache_contents(bool verbose) {
    struct stat st;
    if (lstat(REPO_CACHE_PATH.c_str(), &st) != 0) {
        if (errno == ENOENT) return mkdir_p(REPO_CACHE_PATH);
        std::cerr << Color::RED << "E: Failed to inspect repo cache directory "
                  << REPO_CACHE_PATH << ": " << strerror(errno) << Color::RESET << std::endl;
        return false;
    }

    if (!S_ISDIR(st.st_mode)) {
        std::cerr << Color::RED << "E: Repo cache path is not a directory: "
                  << REPO_CACHE_PATH << Color::RESET << std::endl;
        return false;
    }

    if (!mkdir_p(REPO_CACHE_PATH + "debian/")) {
        std::cerr << Color::RED << "E: Failed to ensure Debian cache directory exists inside "
                  << REPO_CACHE_PATH << Color::RESET << std::endl;
        return false;
    }

    bool ok = true;

    const std::vector<std::string> removable_entries = {
        REPO_CACHE_PATH + "gpkg",
        REPO_CACHE_PATH + "imported",
        REPO_CACHE_PATH + "debian/pool",
        REPO_CACHE_PATH + "Packages.json.tmp",
        get_repo_catalog_path() + ".tmp",
    };

    for (const auto& child : removable_entries) {
        if (lstat(child.c_str(), &st) != 0) {
            if (errno == ENOENT) continue;
            ok = false;
            std::cerr << Color::YELLOW << "W: Failed to remove cache entry "
                      << child << ": " << strerror(errno) << Color::RESET << std::endl;
            continue;
        }

        VLOG(verbose, "Removing cache entry " << child);
        if (!remove_cache_tree_no_follow(child)) {
            ok = false;
            std::cerr << Color::YELLOW << "W: Failed to remove cache entry "
                      << child << ": " << strerror(errno) << Color::RESET << std::endl;
        }
    }

    DIR* dir = opendir(REPO_CACHE_PATH.c_str());
    if (!dir) {
        std::cerr << Color::RED << "E: Failed to open repo cache directory "
                  << REPO_CACHE_PATH << ": " << strerror(errno) << Color::RESET << std::endl;
        return false;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        std::string name = entry->d_name;
        if (name.rfind("repo_index_", 0) != 0) continue;
        std::string child = REPO_CACHE_PATH + name;
        VLOG(verbose, "Removing stale repository index fragment " << child);
        if (!remove_cache_tree_no_follow(child)) {
            ok = false;
            std::cerr << Color::YELLOW << "W: Failed to remove cache entry "
                      << child << ": " << strerror(errno) << Color::RESET << std::endl;
        }
    }
    closedir(dir);

    if (!mkdir_p(REPO_CACHE_PATH)) {
        std::cerr << Color::RED << "E: Failed to ensure repo cache directory exists after cleaning."
                  << Color::RESET << std::endl;
        return false;
    }

    return ok;
}

int package_source_rank(const PackageMetadata& meta) {
    if (meta.source_kind == "debian") return 0;
    if (meta.source_kind == "gpkg_repo") return 1;
    return 2;
}

bool should_prefer_repo_candidate(const PackageMetadata& candidate, const PackageMetadata& current) {
    int candidate_rank = package_source_rank(candidate);
    int current_rank = package_source_rank(current);
    if (candidate_rank != current_rank) return candidate_rank < current_rank;
    return compare_versions(candidate.version, current.version) > 0;
}

void populate_package_metadata_from_json(const std::string& obj, PackageMetadata& meta) {
    get_json_value(obj, "package", meta.name);
    get_json_value(obj, "version", meta.version);
    get_json_value(obj, "architecture", meta.arch);
    get_json_value(obj, "maintainer", meta.maintainer);
    get_json_value(obj, "description", meta.description);
    get_json_value(obj, "section", meta.section);
    get_json_value(obj, "priority", meta.priority);
    get_json_value(obj, "filename", meta.filename);
    get_json_value(obj, "sha256", meta.sha256);
    get_json_value(obj, "sha512", meta.sha512);
    get_json_value(obj, "repo_url", meta.source_url);
    if (meta.source_url.empty()) get_json_value(obj, "source_url", meta.source_url);
    get_json_value(obj, "source_kind", meta.source_kind);
    get_json_value(obj, "debian_package", meta.debian_package);
    get_json_value(obj, "debian_version", meta.debian_version);
    get_json_value(obj, "package_scope", meta.package_scope);
    get_json_value(obj, "installed_from", meta.installed_from);
    get_json_value(obj, "size", meta.size);
    get_json_value(obj, "installed_size_bytes", meta.installed_size_bytes);
    get_json_array(obj, "pre_depends", meta.pre_depends);
    get_json_array(obj, "depends", meta.depends);
    get_json_array(obj, "recommends", meta.recommends);
    get_json_array(obj, "suggests", meta.suggests);
    get_json_array(obj, "breaks", meta.breaks);
    get_json_array(obj, "conflicts", meta.conflicts);
    get_json_array(obj, "provides", meta.provides);
    get_json_array(obj, "replaces", meta.replaces);
}

bool ensure_repo_package_cache_loaded(bool verbose) {
    if (g_repo_package_cache_loaded) {
        if (repo_catalog_matches_current_sources(get_repo_urls())) return true;
        invalidate_repo_package_cache();
    }
    if (!ensure_repo_index_available()) return false;

    std::map<std::string, RepoPackageLocator> offsets;
    std::map<std::string, std::vector<std::string>> providers;
    std::set<std::string> package_names;
    std::string load_error;
    if (!build_repo_catalog_runtime_index(offsets, providers, package_names, &load_error)) {
        VLOG(verbose, "Failed to load repository catalog index: " << load_error);
        return false;
    }

    g_repo_package_cache.clear();
    g_repo_package_offsets = std::move(offsets);
    g_repo_provider_cache = std::move(providers);
    g_repo_available_package_cache = std::move(package_names);
    g_repo_package_cache_loaded = true;
    VLOG(verbose, "Loaded repository catalog index for "
         << g_repo_available_package_cache.size() << " package records.");
    return true;
}

const std::vector<std::string>* get_repo_provider_candidates(const std::string& capability, bool verbose) {
    if (!ensure_repo_package_cache_loaded(verbose)) return nullptr;
    auto it = g_repo_provider_cache.find(capability);
    if (it == g_repo_provider_cache.end()) return nullptr;
    return &it->second;
}

std::string format_package_origin(const PackageMetadata& meta) {
    std::string label = meta.source_kind.empty() ? "unknown" : meta.source_kind;
    if (!meta.source_url.empty()) label += ": " + meta.source_url;
    return label;
}

bool resolve_download_url(const PackageMetadata& meta, std::string& out_url) {
    if (!meta.source_url.empty()) {
        if (meta.source_kind == "debian") {
            out_url = get_debian_package_url(meta);
            return true;
        }
        out_url = build_repo_package_url(meta.source_url, meta.filename);
        return true;
    }

    auto urls = get_repo_urls();
    if (!ensure_repo_urls(urls)) return false;
    out_url = build_repo_package_url(urls[0], meta.filename);
    return true;
}

bool get_repo_package_info(const std::string& pkg_name, PackageMetadata& out_meta) {
    if (!ensure_repo_package_cache_loaded(false)) return false;
    return get_loaded_repo_package_info(pkg_name, out_meta);
}

bool query_full_universe_exact_package(
    const std::string& pkg_name,
    PackageUniverseResult& out_result,
    bool verbose,
    RawDebianContext* raw_context
) {
    out_result = {};
    std::string canonical_name = canonicalize_package_name(pkg_name, verbose);
    if (canonical_name.empty()) {
        out_result.reason = "invalid package name";
        return false;
    }

    if (try_ensure_repo_package_cache_loaded(verbose)) {
        PackageMetadata repo_meta;
        if (get_loaded_repo_package_info(canonical_name, repo_meta)) {
            out_result.found = true;
            out_result.installable = true;
            out_result.raw_only = false;
            out_result.meta = repo_meta;
            return true;
        }

        auto provider_it = g_repo_provider_cache.find(canonical_name);
        if (provider_it != g_repo_provider_cache.end()) {
            bool found = false;
            PackageMetadata best_meta;
            RelationAtom relation;
            relation.name = canonical_name;
            relation.valid = true;
            relation.normalized = canonical_name;
            for (const auto& provider_name : provider_it->second) {
                PackageMetadata candidate;
                if (!get_loaded_repo_package_info(provider_name, candidate)) continue;
                if (!catalog_meta_satisfies_relation(provider_name, candidate, relation)) continue;
                if (!found || should_prefer_repo_candidate(candidate, best_meta)) {
                    best_meta = candidate;
                    found = true;
                }
            }
            if (found) {
                out_result.found = true;
                out_result.installable = true;
                out_result.raw_only = false;
                out_result.meta = best_meta;
                return true;
            }
        }
    }

    if (!raw_context) {
        out_result.reason = "package is absent from the curated local package universe";
        return false;
    }

    RawDebianAvailabilityResult raw_result;
    std::string raw_reason;
    if (query_raw_debian_exact_package(
            canonical_name,
            *raw_context,
            raw_result,
            verbose,
            &raw_reason
        )) {
        out_result.found = raw_result.found;
        out_result.installable = raw_result.installable;
        out_result.raw_only = true;
        out_result.reason = raw_result.reason;
        out_result.meta = raw_result.meta;
        return true;
    }

    RawDebianAvailabilityResult raw_relation_result;
    std::string raw_relation_reason;
    if (query_raw_debian_relation_availability(
            canonical_name,
            "",
            "",
            *raw_context,
            raw_relation_result,
            verbose,
            &raw_relation_reason
        )) {
        out_result.found = raw_relation_result.found;
        out_result.installable = raw_relation_result.installable;
        out_result.raw_only = true;
        out_result.reason = raw_relation_result.reason.empty() ? raw_relation_reason : raw_relation_result.reason;
        out_result.meta = raw_relation_result.meta;
        return true;
    }

    out_result.reason = !raw_reason.empty()
        ? raw_reason
        : (raw_relation_reason.empty() ? "package is absent from cached Debian metadata" : raw_relation_reason);
    return false;
}

bool resolve_full_universe_relation_candidate(
    const std::string& pkg_name,
    const std::string& op,
    const std::string& req_version,
    PackageUniverseResult& out_result,
    bool verbose,
    RawDebianContext* raw_context,
    const PackageMetadata* installed_meta,
    bool prefer_native_debian
) {
    out_result = {};
    std::string canonical_name = canonicalize_package_name(pkg_name, verbose);
    if (canonical_name.empty()) {
        out_result.reason = "invalid package relation";
        return false;
    }

    bool effective_prefer_native_debian = prefer_native_debian;
    if (!effective_prefer_native_debian && installed_meta) {
        effective_prefer_native_debian =
            package_is_debian_source(*installed_meta) ||
            !installed_meta->debian_package.empty();
    }

    std::vector<std::string> raw_queries;
    auto append_query = [&](const std::string& value) {
        if (value.empty()) return;
        std::string canonical = canonicalize_package_name(value, verbose);
        if (canonical.empty()) return;
        if (std::find(raw_queries.begin(), raw_queries.end(), canonical) == raw_queries.end()) {
            raw_queries.push_back(canonical);
        }
    };
    if (installed_meta && !installed_meta->debian_package.empty()) {
        append_query(installed_meta->debian_package);
    }
    append_query(canonical_name);

    auto try_raw_debian_candidate = [&](std::string* reason_out) {
        if (reason_out) reason_out->clear();
        if (!raw_context) return false;

        std::string best_reason;
        for (const auto& query_name : raw_queries) {
            RawDebianAvailabilityResult raw_result;
            std::string raw_reason;
            if (resolve_raw_debian_relation_candidate(
                    query_name,
                    op,
                    req_version,
                    *raw_context,
                    raw_result,
                    verbose,
                    &raw_reason
                )) {
                out_result.found = true;
                out_result.installable = true;
                out_result.raw_only = true;
                out_result.reason.clear();
                out_result.meta = raw_result.meta;
                return true;
            }
            if (best_reason.empty() && !raw_reason.empty()) best_reason = raw_reason;
        }

        if (reason_out) *reason_out = best_reason;
        return false;
    };

    auto try_repo_candidate = [&]() {
        if (!try_ensure_repo_package_cache_loaded(verbose)) return false;

        PackageMetadata repo_meta;
        if (get_loaded_repo_package_info(canonical_name, repo_meta) &&
            catalog_version_satisfies(repo_meta.version, op, req_version)) {
            out_result.found = true;
            out_result.installable = true;
            out_result.meta = repo_meta;
            return true;
        }

        RelationAtom relation;
        relation.name = canonical_name;
        relation.op = op;
        relation.version = req_version;
        relation.valid = !relation.name.empty();
        relation.normalized = relation.op.empty()
            ? relation.name
            : (relation.name + " (" + relation.op + " " + relation.version + ")");
        auto provider_it = g_repo_provider_cache.find(canonical_name);
        if (provider_it == g_repo_provider_cache.end()) return false;

        bool found = false;
        PackageMetadata best_meta;
        for (const auto& provider_name : provider_it->second) {
            PackageMetadata candidate;
            if (!get_loaded_repo_package_info(provider_name, candidate)) continue;
            if (!catalog_meta_satisfies_relation(provider_name, candidate, relation)) continue;
            if (!found || should_prefer_repo_candidate(candidate, best_meta)) {
                best_meta = candidate;
                found = true;
            }
        }
        if (!found) return false;

        out_result.found = true;
        out_result.installable = true;
        out_result.meta = best_meta;
        return true;
    };

    std::string raw_reason;
    if (effective_prefer_native_debian && try_raw_debian_candidate(&raw_reason)) {
        return true;
    }

    if (try_repo_candidate()) return true;

    if (!raw_context) {
        out_result.reason = "no repository package or provider candidate";
        return false;
    }

    if (!effective_prefer_native_debian && try_raw_debian_candidate(&raw_reason)) {
        return true;
    }

    out_result.reason = raw_reason.empty()
        ? "no cached Debian candidate satisfies the required relation"
        : raw_reason;
    return false;
}

int handle_list_repos() {
    auto urls = get_repo_urls();
    DebianBackendConfig debian = load_debian_backend_config(false);
    std::cout << "Configured package sources:" << std::endl;
    std::cout << "  1. Debian testing (" << debian.packages_url << ")" << std::endl;
    if (urls.empty()) {
        std::cout << "  2. No additional S2 repositories configured." << std::endl;
        return 0;
    }

    for (size_t i = 0; i < urls.size(); ++i) {
        std::cout << "  " << (i + 2) << ". " << normalize_repo_base_url(urls[i]) << std::endl;
    }
    return 0;
}

void merge_repo_catalog_candidate(
    std::map<std::string, PackageMetadata>& packages,
    const PackageMetadata& candidate
) {
    if (candidate.name.empty()) return;

    auto it = packages.find(candidate.name);
    if (it == packages.end() || should_prefer_repo_candidate(candidate, it->second)) {
        packages[candidate.name] = candidate;
    }
}

bool append_cached_debian_imported_catalog(
    std::map<std::string, PackageMetadata>& packages,
    const std::string& fingerprint,
    bool verbose,
    int* appended_count_out = nullptr
) {
    if (appended_count_out) *appended_count_out = 0;

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
                merge_repo_catalog_candidate(packages, meta);
                ++appended_count;
                return true;
            },
            &cache_error
        )) {
        VLOG(verbose, "Discarded cached imported Debian catalog: " << cache_error);
        return false;
    }

    if (appended_count_out) *appended_count_out = appended_count;
    return appended_count > 0;
}

bool append_repo_catalog_shard(
    const std::string& path,
    std::map<std::string, PackageMetadata>& packages,
    bool verbose,
    int* appended_count_out = nullptr
) {
    if (appended_count_out) *appended_count_out = 0;

    int appended_count = 0;
    std::string shard_error;
    if (!foreach_debian_binary_cache_entry<PackageMetadata>(
            path,
            REPO_CATALOG_SHARD_MAGIC,
            [](std::istream& in, PackageMetadata& meta) {
                return read_package_metadata_binary(in, meta);
            },
            [&](const PackageMetadata& meta) {
                merge_repo_catalog_candidate(packages, meta);
                ++appended_count;
                return true;
            },
            &shard_error
        )) {
        VLOG(verbose, "Discarded stale repository catalog shard " << path
                     << ": " << shard_error);
        return false;
    }

    if (appended_count_out) *appended_count_out = appended_count;
    return true;
}

bool ensure_current_repo_source_catalog_shard(
    const std::string& repo_url,
    bool verbose,
    int* package_count_out = nullptr,
    std::string* error_out = nullptr
) {
    if (package_count_out) *package_count_out = 0;
    if (error_out) error_out->clear();

    std::string normalized = normalize_repo_base_url(repo_url);
    std::string source_json = get_repo_source_index_json_path(normalized);
    if (access(source_json.c_str(), F_OK) != 0) {
        if (error_out) *error_out = "cached source index is unavailable for " + normalized;
        return false;
    }

    std::string shard_path = get_repo_source_catalog_shard_path(normalized);
    std::string shard_state_path = get_repo_source_catalog_shard_state_path(normalized);
    std::string fingerprint = build_repo_source_catalog_shard_fingerprint(normalized);

    RepoCatalogShardState shard_state;
    if (access(shard_path.c_str(), F_OK) == 0 &&
        load_repo_catalog_shard_state(shard_state_path, shard_state) &&
        shard_state.fingerprint == fingerprint) {
        if (package_count_out) *package_count_out = static_cast<int>(shard_state.package_count);
        return true;
    }

    std::map<std::string, PackageMetadata> source_packages;
    foreach_json_object(source_json, [&](const std::string& obj) {
        PackageMetadata candidate;
        populate_package_metadata_from_json(obj, candidate);
        candidate.name = trim(candidate.name);
        if (candidate.name.empty()) return true;
        if (candidate.source_url.empty()) candidate.source_url = normalized;
        if (candidate.source_kind.empty()) candidate.source_kind = "gpkg_repo";
        merge_repo_catalog_candidate(source_packages, candidate);
        return true;
    });

    std::vector<PackageMetadata> entries;
    entries.reserve(source_packages.size());
    for (const auto& entry : source_packages) {
        entries.push_back(entry.second);
    }

    std::string shard_error;
    if (!write_repo_catalog_shard(shard_path, entries, &shard_error)) {
        if (error_out) {
            *error_out = shard_error.empty()
                ? "failed to write repository catalog shard for " + normalized
                : shard_error;
        }
        return false;
    }

    RepoCatalogShardState new_state;
    new_state.fingerprint = fingerprint;
    new_state.package_count = entries.size();
    if (!save_repo_catalog_shard_state(shard_state_path, new_state)) {
        if (error_out) *error_out = "failed to persist repository catalog shard state for " + normalized;
        return false;
    }

    if (package_count_out) *package_count_out = static_cast<int>(entries.size());
    VLOG(verbose, "Rebuilt repository catalog shard for " << normalized
         << " (" << entries.size() << " package records).");
    return true;
}

bool copy_debian_imported_cache_to_repo_shard(
    const std::string& shard_path,
    size_t expected_entries,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    std::string imported_cache_path = get_debian_imported_index_binary_cache_path();
    std::ifstream in(imported_cache_path, std::ios::binary);
    if (!in) {
        if (error_out) *error_out = "failed to open imported Debian cache";
        return false;
    }

    uint32_t entry_count = 0;
    if (!read_binary_cache_header(in, DEBIAN_IMPORTED_CACHE_MAGIC, entry_count, error_out)) {
        return false;
    }

    if (expected_entries <= std::numeric_limits<uint32_t>::max() &&
        entry_count != static_cast<uint32_t>(expected_entries)) {
        if (error_out) *error_out = "imported Debian cache entry count does not match its state";
        return false;
    }

    if (!mkdir_parent(shard_path)) {
        if (error_out) *error_out = "failed to create Debian catalog shard directory";
        return false;
    }

    std::string temp_path = shard_path + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error_out) *error_out = "failed to open Debian catalog shard for writing";
        return false;
    }

    if (!write_binary_cache_header(out, REPO_CATALOG_SHARD_MAGIC, entry_count)) {
        out.close();
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to write Debian catalog shard header";
        return false;
    }

    for (uint32_t i = 0; i < entry_count; ++i) {
        PackageMetadata meta;
        if (!read_package_metadata_binary(in, meta)) {
            out.close();
            remove(temp_path.c_str());
            if (error_out) *error_out = "failed to decode imported Debian cache entry " + std::to_string(i + 1);
            return false;
        }
        if (!write_package_metadata_binary(out, meta)) {
            out.close();
            remove(temp_path.c_str());
            if (error_out) *error_out = "failed to encode Debian catalog shard entry " + std::to_string(i + 1);
            return false;
        }
    }

    out.close();
    if (!out) {
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to flush Debian catalog shard";
        return false;
    }

    if (rename(temp_path.c_str(), shard_path.c_str()) != 0) {
        remove(temp_path.c_str());
        if (error_out) *error_out = strerror(errno);
        return false;
    }

    return true;
}

bool ensure_current_debian_catalog_shard(
    bool verbose,
    int* package_count_out = nullptr,
    std::string* error_out = nullptr
) {
    if (package_count_out) *package_count_out = 0;
    if (error_out) error_out->clear();

    std::string packages_txt = get_debian_packages_cache_path();
    if (access(packages_txt.c_str(), F_OK) != 0) {
        if (error_out) *error_out = "cached Debian Packages index is unavailable";
        return false;
    }

    std::string ensure_error;
    if (!ensure_current_debian_imported_index_cache(verbose, &ensure_error)) {
        if (error_out) {
            *error_out = ensure_error.empty()
                ? "failed to prepare the imported Debian metadata cache"
                : ensure_error;
        }
        return false;
    }

    std::string imported_state_path = get_debian_imported_index_state_path();
    DebianImportedIndexCacheState imported_state;
    if (!load_debian_imported_index_cache_state(imported_state_path, imported_state)) {
        if (error_out) *error_out = "failed to read imported Debian cache state";
        return false;
    }

    std::string shard_path = get_debian_catalog_shard_path();
    std::string shard_state_path = get_debian_catalog_shard_state_path();
    std::string fingerprint = build_debian_catalog_shard_fingerprint(packages_txt);

    RepoCatalogShardState shard_state;
    if (access(shard_path.c_str(), F_OK) == 0 &&
        load_repo_catalog_shard_state(shard_state_path, shard_state) &&
        shard_state.fingerprint == fingerprint) {
        if (package_count_out) *package_count_out = static_cast<int>(shard_state.package_count);
        return true;
    }

    std::string shard_error;
    if (!copy_debian_imported_cache_to_repo_shard(
            shard_path,
            imported_state.package_count,
            &shard_error
        )) {
        if (error_out) {
            *error_out = shard_error.empty()
                ? "failed to write the Debian repository catalog shard"
                : shard_error;
        }
        return false;
    }

    RepoCatalogShardState new_state;
    new_state.fingerprint = fingerprint;
    new_state.package_count = imported_state.package_count;
    if (!save_repo_catalog_shard_state(shard_state_path, new_state)) {
        if (error_out) *error_out = "failed to persist Debian catalog shard state";
        return false;
    }

    if (package_count_out) *package_count_out = static_cast<int>(imported_state.package_count);
    VLOG(verbose, "Rebuilt Debian catalog shard with "
         << imported_state.package_count << " package records.");
    return true;
}

bool build_current_repo_catalog(bool verbose, std::string* error_out) {
    if (error_out) error_out->clear();

    auto urls = get_repo_urls();
    if (repo_catalog_matches_current_sources(urls)) return true;

    std::string fingerprint = build_repo_catalog_fingerprint(urls);
    struct SelectedPackageEntry {
        PackageMetadata meta;
        RepoPackageLocator locator;
        std::vector<std::string> provides;
    };

    std::map<std::string, SelectedPackageEntry> selected_packages;
    std::vector<std::string> shard_paths;
    std::set<std::string> seen_shards;
    int success_count = 0;

    for (const auto& url : urls) {
        std::string shard_error;
        if (!ensure_current_repo_source_catalog_shard(
                url,
                verbose,
                nullptr,
                &shard_error
            )) {
            if (verbose && !shard_error.empty()) {
                VLOG(verbose, "Skipping repository source " << normalize_repo_base_url(url)
                             << " while rebuilding the merged package catalog: "
                             << shard_error);
            }
            continue;
        }

        const std::string shard_path = get_repo_source_catalog_shard_path(url);
        const std::string index_path = get_repo_source_catalog_shard_index_path(url);
        if (!ensure_repo_catalog_shard_index_ready(
                shard_path,
                index_path,
                verbose,
                normalize_repo_base_url(url),
                &shard_error
            )) {
            if (verbose && !shard_error.empty()) {
                VLOG(verbose, "Skipping repository shard index for "
                             << normalize_repo_base_url(url) << ": " << shard_error);
            }
            continue;
        }

        std::vector<RepoCatalogShardIndexEntry> shard_entries;
        if (!read_repo_catalog_shard_index(index_path, shard_entries, &shard_error)) {
            if (verbose && !shard_error.empty()) {
                VLOG(verbose, "Skipping unreadable repository shard index for "
                             << normalize_repo_base_url(url) << ": " << shard_error);
            }
            continue;
        }

        if (seen_shards.insert(shard_path).second) shard_paths.push_back(shard_path);
        for (const auto& entry : shard_entries) {
            if (entry.name.empty()) continue;

            PackageMetadata candidate_meta;
            candidate_meta.name = entry.name;
            candidate_meta.version = entry.version;
            candidate_meta.source_kind = entry.source_kind;

            auto it = selected_packages.find(entry.name);
            if (it == selected_packages.end() ||
                should_prefer_repo_candidate(candidate_meta, it->second.meta)) {
                SelectedPackageEntry selected;
                selected.meta = candidate_meta;
                selected.locator = {shard_path, entry.offset};
                selected.provides = entry.provides;
                selected_packages[entry.name] = std::move(selected);
            }
        }
        ++success_count;
    }

    std::string debian_error;
    std::string packages_txt = get_debian_packages_cache_path();
    if (access(packages_txt.c_str(), F_OK) == 0 &&
        ensure_current_debian_catalog_shard(verbose, nullptr, &debian_error)) {
        const std::string shard_path = get_debian_catalog_shard_path();
        const std::string index_path = get_debian_catalog_shard_index_path();
        if (ensure_repo_catalog_shard_index_ready(
                shard_path,
                index_path,
                verbose,
                "Debian testing",
                &debian_error
            )) {
            std::vector<RepoCatalogShardIndexEntry> shard_entries;
            if (read_repo_catalog_shard_index(index_path, shard_entries, &debian_error)) {
                if (seen_shards.insert(shard_path).second) shard_paths.push_back(shard_path);
                for (const auto& entry : shard_entries) {
                    if (entry.name.empty()) continue;

                    PackageMetadata candidate_meta;
                    candidate_meta.name = entry.name;
                    candidate_meta.version = entry.version;
                    candidate_meta.source_kind = entry.source_kind;

                    auto it = selected_packages.find(entry.name);
                    if (it == selected_packages.end() ||
                        should_prefer_repo_candidate(candidate_meta, it->second.meta)) {
                        SelectedPackageEntry selected;
                        selected.meta = candidate_meta;
                        selected.locator = {shard_path, entry.offset};
                        selected.provides = entry.provides;
                        selected_packages[entry.name] = std::move(selected);
                    }
                }
                ++success_count;
            } else if (verbose && !debian_error.empty()) {
                VLOG(verbose, "Skipping unreadable Debian shard index while rebuilding the runtime package index: "
                             << debian_error);
            }
        }
    } else if (verbose && !debian_error.empty()) {
        VLOG(verbose, "Skipping the Debian catalog shard while rebuilding the repo catalog: "
                     << debian_error);
    }

    if (success_count == 0) {
        if (error_out) *error_out = "no cached package sources are available; run 'gpkg update'";
        return false;
    }

    std::map<std::string, uint32_t> shard_ids;
    for (size_t i = 0; i < shard_paths.size(); ++i) {
        shard_ids[shard_paths[i]] = static_cast<uint32_t>(i);
    }

    std::vector<RepoRuntimePackageEntry> runtime_packages;
    runtime_packages.reserve(selected_packages.size());
    std::vector<RepoRuntimeProviderEntry> runtime_providers;
    std::map<std::string, std::vector<std::string>> provider_map;
    for (const auto& entry : selected_packages) {
        RepoRuntimePackageEntry runtime_entry;
        runtime_entry.name = entry.first;
        runtime_entry.shard_id = shard_ids[entry.second.locator.shard_path];
        runtime_entry.offset = entry.second.locator.offset;
        runtime_packages.push_back(std::move(runtime_entry));

        for (const auto& capability : entry.second.provides) {
            if (capability.empty()) continue;
            auto& provider_names = provider_map[capability];
            if (std::find(provider_names.begin(), provider_names.end(), entry.first) == provider_names.end()) {
                provider_names.push_back(entry.first);
            }
        }
    }

    runtime_providers.reserve(provider_map.size());
    for (auto& entry : provider_map) {
        RepoRuntimeProviderEntry provider_entry;
        provider_entry.capability = entry.first;
        provider_entry.package_names = std::move(entry.second);
        runtime_providers.push_back(std::move(provider_entry));
    }

    if (!write_repo_runtime_index(shard_paths, runtime_packages, runtime_providers, error_out)) {
        return false;
    }

    RepoCatalogState state;
    state.fingerprint = fingerprint;
    if (!save_repo_catalog_state(get_repo_catalog_state_path(), state)) {
        if (error_out) *error_out = "failed to persist repo catalog state";
        return false;
    }
    remove_optional_cache_export(get_legacy_repo_index_path());
    return true;
}

bool update_debian_backend_catalog(
    std::map<std::string, PackageMetadata>& packages,
    int& package_count,
    bool verbose
) {
    package_count = 0;

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
        if (append_cached_debian_imported_catalog(
                packages,
                import_cache_fingerprint,
                verbose,
                &package_count
            )) {
            std::cout << Color::GREEN << "✓ Reused cached packages index"
                      << " (" << package_count << " packages)"
                      << Color::RESET << std::endl;
            return true;
        }
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
        std::cerr << Color::YELLOW
                  << "W: Failed to prepare the Debian parsed-record cache";
        if (!parsed_cache_error.empty()) std::cerr << " (" << parsed_cache_error << ")";
        std::cerr << ". Falling back to the in-memory import path."
                  << Color::RESET << std::endl;
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
            std::cerr << ". Fine-grained Debian cache reuse will be unavailable."
                      << Color::RESET << std::endl;
        }
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
        preview_entries =
            parsed_cache_error.empty()
                ? build_debian_search_preview_entries_from_current_parsed_cache(
                      packages_txt,
                      config,
                      policy,
                      entries,
                      incremental_import.skipped_policy,
                      verbose
                  )
                : build_debian_search_preview_entries(
                      packages_txt,
                      entries,
                      incremental_import.skipped_policy,
                      verbose
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

    package_count = static_cast<int>(entries.size());
    for (const auto& meta : entries) {
        merge_repo_catalog_candidate(packages, meta);
    }

    return true;
}

bool sync_debian_testing_index_legacy(
    bool verbose,
    bool* changed_out = nullptr,
    std::string* result_out = nullptr
) {
    if (changed_out) *changed_out = false;
    if (result_out) result_out->clear();

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

    if (have_packages_txt && have_cached_state && have_remote_state &&
        remote_packages_index_matches_cache(cached_state, remote_state)) {
        if (result_out) *result_out = "✓ Reused cached Debian testing Packages index";
        return true;
    } else if (verbose && !have_remote_state && !probe_error.empty()) {
        VLOG(verbose, "Unable to probe Debian Packages metadata; falling back to cached state: "
                     << probe_error);
    }

    if (have_packages_txt) {
        bool pdiff_changed = false;
        size_t pdiff_patches_applied = 0;
        std::string pdiff_error;
        if (try_update_debian_packages_with_pdiff(
                config,
                packages_txt,
                verbose,
                pdiff_changed,
                pdiff_patches_applied,
                &pdiff_error
            )) {
            if (have_remote_state) save_debian_packages_cache_state(packages_state, remote_state);
            else remove(packages_state.c_str());

            if (changed_out) *changed_out = pdiff_changed;
            if (result_out) {
                if (pdiff_changed) {
                    *result_out = "✓ Updated Debian testing Packages index via PDiff (" +
                                  std::to_string(pdiff_patches_applied) + " patch" +
                                  (pdiff_patches_applied == 1 ? "" : "es") + ")";
                } else {
                    *result_out = "✓ Reused cached Debian testing Packages index";
                }
            }
            return true;
        } else if (verbose && !pdiff_error.empty()) {
            VLOG(verbose, "Unable to apply Debian PDiffs; falling back to full Packages download: "
                         << pdiff_error);
        }
    }

    std::string download_error;
    if (!DownloadFile(config.packages_url, packages_gz, verbose, &download_error)) {
        if (have_packages_txt) {
            if (result_out) *result_out = "✓ Reused cached Debian testing Packages index";
            return true;
        }
        if (result_out) {
            *result_out = "W: Failed to fetch Debian Packages index from " + config.packages_url +
                          (download_error.empty() ? "" : " (" + download_error + ")");
        }
        return false;
    }

    std::string unpack_error;
    if (!GpkgArchive::decompress_gzip_file(packages_gz, packages_txt, &unpack_error)) {
        if (have_packages_txt) {
            if (result_out) *result_out = "✓ Reused cached Debian testing Packages index";
            return true;
        }
        if (result_out) {
            *result_out = "W: Failed to unpack Debian Packages index" +
                          (unpack_error.empty() ? "" : " (" + unpack_error + ")");
        }
        return false;
    }

    if (have_remote_state) save_debian_packages_cache_state(packages_state, remote_state);
    else remove(packages_state.c_str());

    if (changed_out) *changed_out = true;
    if (result_out) *result_out = "✓ Updated Debian testing Packages index";
    return true;
}

bool sync_debian_testing_index(
    bool verbose,
    bool* changed_out,
    std::string* result_out
) {
    DebianBackendSelection backend = select_debian_backend(
        DebianBackendOperation::SyncIndex,
        verbose
    );
    maybe_log_debian_backend_selection(backend, DebianBackendOperation::SyncIndex, verbose);
    return sync_debian_testing_index_legacy(verbose, changed_out, result_out);
}

int handle_update(bool verbose) {
    auto urls = get_repo_urls();
    VLOG(verbose, "Found " << urls.size() << " repository URLs.");
    std::cout << Color::BLUE << "Updating package indices (this may take a while)..." << Color::RESET << std::endl;
    run_command("mkdir -p " + REPO_CACHE_PATH, verbose);

    int success_count = 0;
    bool repo_sources_changed = false;

    for (const auto& url : urls) {
        std::string full_url = build_repo_index_url(url);
        VLOG(verbose, "Refreshing cached index from: " << full_url);
        std::cout << "Get: " << full_url << std::endl;

        bool source_changed = false;
        size_t package_count = 0;
        std::string result;
        if (!sync_repo_source_index(url, verbose, &source_changed, &package_count, &result)) {
            if (!result.empty()) {
                std::cerr << Color::YELLOW << result << Color::RESET << std::endl;
            }
            continue;
        }

        repo_sources_changed = repo_sources_changed || source_changed;
        ++success_count;
        if (!result.empty()) {
            std::cout << Color::GREEN << result << Color::RESET << std::endl;
        }
    }

    bool debian_changed = false;
    std::string debian_result;
    if (sync_debian_testing_index(verbose, &debian_changed, &debian_result)) {
        ++success_count;
        if (!debian_result.empty()) {
            std::cout << Color::GREEN << debian_result << Color::RESET << std::endl;
        }
    } else if (!debian_result.empty()) {
        std::cerr << Color::YELLOW << debian_result << Color::RESET << std::endl;
    }

    if (success_count == 0) {
        std::cerr << Color::RED << "E: Failed to update any package indices." << Color::RESET << std::endl;
        return 1;
    }

    bool debian_views_stale = false;
    std::string packages_txt = get_debian_packages_cache_path();
    if (access(packages_txt.c_str(), F_OK) == 0) {
        debian_views_stale =
            !debian_imported_index_cache_is_current(packages_txt) ||
            !debian_compiled_record_cache_is_current();
    }

    if (debian_changed || debian_views_stale) {
        invalidate_debian_derived_metadata_caches(verbose);
    }

    if (repo_sources_changed || debian_changed || debian_views_stale) {
        remove(get_repo_catalog_path().c_str());
        remove(get_repo_catalog_state_path().c_str());
        remove(get_legacy_repo_index_path().c_str());
        remove(UPGRADE_CATALOG_PATH.c_str());
        invalidate_repo_package_cache();
    }

    try {
        std::string catalog_error;
        if (!build_current_repo_catalog(verbose, &catalog_error)) {
            std::cerr << Color::RED
                      << "E: Failed to rebuild the compiled package catalog";
            if (!catalog_error.empty()) std::cerr << " (" << catalog_error << ")";
            std::cerr << Color::RESET << std::endl;
            return 1;
        }

        g_repo_package_cache.clear();

        std::string upgrade_error;
        if (!ensure_current_upgrade_catalog(verbose, &upgrade_error)) {
            std::cerr << Color::RED
                      << "E: Failed to rebuild the runtime upgrade catalog";
            if (!upgrade_error.empty()) std::cerr << " (" << upgrade_error << ")";
            std::cerr << Color::RESET << std::endl;
            return 1;
        }

        g_repo_package_cache.clear();
    } catch (const std::bad_alloc&) {
        invalidate_repo_package_cache();
        std::cerr << Color::RED
                  << "E: gpkg update ran out of memory while rebuilding compiled package metadata. "
                  << "The package indices were updated, but the compiled catalogs need to be rebuilt "
                  << "with a lower-memory pass."
                  << Color::RESET << std::endl;
        return 1;
    }

    bool libapt_cache_primed = false;
    if (access(packages_txt.c_str(), F_OK) == 0) {
        std::string libapt_prime_error;
        if (!libapt_prime_planner_cache(verbose, &libapt_prime_error)) {
            std::cerr << Color::YELLOW
                      << "W: Failed to prime the libapt-pkg planner cache";
            if (!libapt_prime_error.empty()) std::cerr << " (" << libapt_prime_error << ")";
            std::cerr << ". Debian-backed installs may be slower until the cache is rebuilt."
                      << Color::RESET << std::endl;
        } else {
            libapt_cache_primed = true;
        }
    }

    std::cout << Color::GREEN << "✓ Synced raw package indices from "
              << success_count << " source"
              << (success_count == 1 ? "" : "s")
              << Color::RESET << std::endl;
    std::cout << Color::GREEN
              << "✓ Rebuilt compiled package catalogs for fast queries."
              << Color::RESET << std::endl;
    if (libapt_cache_primed) {
        std::cout << Color::GREEN
                  << "✓ Primed the libapt-pkg planner cache for faster installs."
                  << Color::RESET << std::endl;
    }

    return 0;
}

struct SearchResultDisplay {
    PackageMetadata meta;
    bool on_demand = false;
    bool installable = true;
    std::string reason;
};

std::string search_result_channel_label(const SearchResultDisplay& result) {
    if (result.meta.source_kind == "debian") return "debian";
    if (result.meta.source_kind == "gpkg_repo") return "repo";
    return result.meta.source_kind.empty() ? "unknown" : result.meta.source_kind;
}

size_t find_case_insensitive_substring(const std::string& haystack, const std::string& normalized_needle) {
    if (normalized_needle.empty()) return 0;
    return ascii_lower_copy(haystack).find(normalized_needle);
}

struct SearchResultSortKey {
    int bucket = 100;
    size_t position = std::numeric_limits<size_t>::max();
};

SearchResultSortKey compute_search_result_sort_key(const SearchResultDisplay& result, const std::string& normalized_query) {
    const auto& meta = result.meta;

    size_t pos = find_case_insensitive_substring(meta.name, normalized_query);
    if (pos == 0 && ascii_lower_copy(meta.name) == normalized_query) return {0, 0};
    if (!meta.debian_package.empty()) {
        size_t debian_pos = find_case_insensitive_substring(meta.debian_package, normalized_query);
        if (debian_pos == 0 && ascii_lower_copy(meta.debian_package) == normalized_query) return {1, 0};
    }
    if (pos == 0) return {2, 0};
    if (!meta.debian_package.empty()) {
        size_t debian_pos = find_case_insensitive_substring(meta.debian_package, normalized_query);
        if (debian_pos == 0) return {3, 0};
    }
    if (pos != std::string::npos) return {4, pos};
    if (!meta.debian_package.empty()) {
        size_t debian_pos = find_case_insensitive_substring(meta.debian_package, normalized_query);
        if (debian_pos != std::string::npos) return {5, debian_pos};
    }

    pos = find_case_insensitive_substring(meta.description, normalized_query);
    if (pos == 0) return {6, 0};
    if (pos != std::string::npos) return {7, pos};

    return {};
}

std::string render_search_result_display(const SearchResultDisplay& result) {
    const auto& meta = result.meta;

    auto get_live_display_version = [](const std::string& pkg_name, std::string* version_out = nullptr) {
        if (version_out) version_out->clear();

        PackageStatusRecord status_record;
        if (get_dpkg_package_status_record(pkg_name, &status_record) &&
            package_status_is_installed_like(status_record.status)) {
            if (version_out) *version_out = status_record.version;
            return true;
        }

        if (get_base_system_package_status_record(pkg_name, &status_record) &&
            package_status_is_installed_like(status_record.status)) {
            if (version_out) *version_out = status_record.version;
            return true;
        }

        return false;
    };

    std::string installed_ver;
    std::vector<std::string> flags;
    if (is_installed(meta.name, &installed_ver)) {
        if (compare_versions(installed_ver, meta.version) == 0) {
            flags.push_back(Color::BLUE + "[installed]" + Color::RESET);
        } else {
            flags.push_back(Color::BLUE + "[installed: " + installed_ver + "]" + Color::RESET);
        }
    } else {
        std::string live_version;
        if (get_live_display_version(meta.name, &live_version)) {
            if (package_is_base_system_provided(meta.name)) {
                flags.push_back(
                    Color::BLUE + "[base system: " + live_version + "]" + Color::RESET
                );
            } else {
                flags.push_back(
                    Color::BLUE + "[live: " + live_version + "]" + Color::RESET
                );
            }
        } else if (package_is_base_system_provided(meta.name)) {
            flags.push_back(Color::BLUE + "[base system]" + Color::RESET);
        }
    }

    std::ostringstream out;
    out << Color::GREEN << meta.name << Color::RESET
        << "/" << Color::CYAN << search_result_channel_label(result) << Color::RESET
        << " " << meta.version;
    for (const auto& flag : flags) {
        out << " " << flag;
    }
    out << std::endl;

    std::string summary = description_summary(meta.description);
    if (!summary.empty()) out << "  " << summary << std::endl;
    return out.str();
}

int handle_search(const std::string& query, bool verbose) {
    bool have_repo_cache = try_ensure_repo_package_cache_loaded(verbose);
    VLOG(verbose, "Searching for '" << query << "' in the local package universe");
    const std::string normalized_query = ascii_lower_copy(query);
    std::map<std::string, SearchResultDisplay> matches;
    if (have_repo_cache) {
        std::string catalog_error;
        if (!foreach_repo_catalog_entry([&](const PackageMetadata& meta) {
            if (find_case_insensitive_substring(meta.name, normalized_query) != std::string::npos ||
                find_case_insensitive_substring(meta.description, normalized_query) != std::string::npos) {
                auto it = matches.find(meta.name);
                if (it == matches.end() || should_prefer_repo_candidate(meta, it->second.meta)) {
                    SearchResultDisplay display;
                    display.meta = meta;
                    matches[meta.name] = display;
                }
            }
            return true;
        }, &catalog_error) && verbose) {
            std::cout << "[DEBUG] Failed while scanning the binary package catalog for search: "
                      << catalog_error << std::endl;
        }
    }

    std::string preview_error;
    bool preview_available = foreach_debian_search_preview_entry(
        verbose,
        [&](const DebianSearchPreviewEntry& preview) {
            if (matches.count(preview.meta.name) != 0) return true;

            bool matched = find_case_insensitive_substring(preview.meta.name, normalized_query) != std::string::npos ||
                find_case_insensitive_substring(preview.meta.description, normalized_query) != std::string::npos;
            if (!matched &&
                find_case_insensitive_substring(preview.meta.debian_package, normalized_query) != std::string::npos) {
                matched = true;
            }
            if (!matched) {
                for (const auto& raw_name : preview.raw_names) {
                    if (find_case_insensitive_substring(raw_name, normalized_query) != std::string::npos) {
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) return true;

            SearchResultDisplay display;
            display.meta = preview.meta;
            display.on_demand = true;
            display.installable = preview.installable;
            display.reason = preview.reason;
            matches[display.meta.name] = display;
            return true;
        },
        &preview_error
    );

    RawDebianContext raw_context;
    std::string raw_load_error;
    bool raw_available = false;
    if (!preview_available) {
        raw_available = ensure_raw_debian_context_loaded(raw_context, verbose, &raw_load_error);
        std::map<std::string, RawDebianAvailabilityResult> raw_matches;
        auto should_prefer_raw_search_result = [&](const RawDebianAvailabilityResult& candidate,
                                                   const RawDebianAvailabilityResult& current) {
            if (candidate.installable != current.installable) return candidate.installable;
            return compare_versions(candidate.meta.version, current.meta.version) > 0;
        };
        if (raw_available) {
            for (const auto& entry : raw_context.import_name_to_raw_names) {
                bool matched = find_case_insensitive_substring(entry.key, normalized_query) != std::string::npos;
                if (!matched) {
                    for (const auto& raw_name : entry.raw_names) {
                        if (find_case_insensitive_substring(raw_name, normalized_query) != std::string::npos) {
                            matched = true;
                            break;
                        }
                    }
                }

                RawDebianAvailabilityResult result;
                std::string raw_reason;
                if (!query_raw_debian_exact_package(entry.key, raw_context, result, verbose, &raw_reason)) {
                    continue;
                }
                if (!matched &&
                    find_case_insensitive_substring(result.meta.description, normalized_query) == std::string::npos) {
                    continue;
                }
                if (g_repo_available_package_cache.count(result.meta.name) != 0) continue;

                auto existing = raw_matches.find(result.meta.name);
                if (existing == raw_matches.end() ||
                    should_prefer_raw_search_result(result, existing->second)) {
                    raw_matches[result.meta.name] = result;
                }
            }
        }

        for (const auto& entry : raw_matches) {
            SearchResultDisplay display;
            display.meta = entry.second.meta;
            display.on_demand = true;
            display.installable = entry.second.installable;
            display.reason = entry.second.reason;
            matches[display.meta.name] = display;
        }
    }

    if (matches.empty()) {
        if (!preview_available && !raw_available) {
            std::cerr << Color::RED << "E: "
                      << (!preview_error.empty()
                              ? preview_error
                              : (raw_load_error.empty()
                                      ? "cached Debian metadata is unavailable; run 'gpkg update'"
                                      : raw_load_error))
                      << Color::RESET << std::endl;
            return 1;
        }
        std::cout << "No matches found for '" << query << "'" << std::endl;
        return 0;
    }

    std::vector<SearchResultDisplay> ordered_matches;
    ordered_matches.reserve(matches.size());
    for (const auto& entry : matches) {
        ordered_matches.push_back(entry.second);
    }
    std::stable_sort(ordered_matches.begin(), ordered_matches.end(),
        [&](const SearchResultDisplay& lhs, const SearchResultDisplay& rhs) {
            SearchResultSortKey lhs_key = compute_search_result_sort_key(lhs, normalized_query);
            SearchResultSortKey rhs_key = compute_search_result_sort_key(rhs, normalized_query);
            if (lhs_key.bucket != rhs_key.bucket) return lhs_key.bucket < rhs_key.bucket;
            if (lhs_key.position != rhs_key.position) return lhs_key.position < rhs_key.position;

            std::string lhs_name = ascii_lower_copy(lhs.meta.name);
            std::string rhs_name = ascii_lower_copy(rhs.meta.name);
            if (lhs_name != rhs_name) return lhs_name < rhs_name;
            return compare_versions(lhs.meta.version, rhs.meta.version) > 0;
        });

    std::ostringstream rendered;
    for (const auto& result : ordered_matches) {
        rendered << render_search_result_display(result);
    }

    const std::string output = rendered.str();
    if (!write_text_via_pager(output, verbose)) {
        std::cout << output;
    }

    return 0;
}

int handle_show(const std::string& pkg_name, bool verbose) {
    VLOG(verbose, "Showing package metadata for '" << pkg_name << "'");
    RawDebianContext raw_context;
    PackageUniverseResult result;
    if (!query_full_universe_exact_package(pkg_name, result, verbose, &raw_context)) {
        DebianSearchPreviewEntry preview;
        std::string preview_error;
        if (get_debian_search_preview_exact_package(pkg_name, preview, verbose, &preview_error)) {
            result.found = true;
            result.installable = preview.installable;
            result.raw_only = true;
            result.reason = preview.reason;
            result.meta = preview.meta;
        } else {
            std::cerr << Color::RED << "E: Package '" << pkg_name << "' was not found in the local package universe";
            if (!result.reason.empty()) std::cerr << " (" << result.reason << ")";
            std::cerr << "." << Color::RESET << std::endl;
            return 1;
        }
    }
    PackageMetadata meta = result.meta;

    std::cout << Color::GREEN << meta.name << Color::RESET << std::endl;
    std::cout << "  Version:     " << meta.version << std::endl;
    std::cout << "  Source:      " << format_package_origin(meta) << std::endl;
    std::cout << "  Filename:    " << meta.filename << std::endl;
    if (!meta.debian_package.empty()) std::cout << "  Debian Pkg:  " << meta.debian_package << std::endl;
    if (!meta.debian_version.empty()) std::cout << "  Debian Ver:  " << meta.debian_version << std::endl;
    if (!meta.description.empty()) print_description_block("Description", meta.description);
    if (!meta.pre_depends.empty()) print_wrapped_block("  Pre-Depends: ", join_strings(meta.pre_depends));
    if (!meta.depends.empty()) print_wrapped_block("  Depends:     ", join_strings(meta.depends));
    if (!meta.recommends.empty()) print_wrapped_block("  Recommends:  ", join_strings(meta.recommends));
    if (!meta.suggests.empty()) print_wrapped_block("  Suggests:    ", join_strings(meta.suggests));
    if (!meta.conflicts.empty()) print_wrapped_block("  Conflicts:   ", join_strings(meta.conflicts));
    if (!meta.provides.empty()) print_wrapped_block("  Provides:    ", join_strings(meta.provides));
    if (!meta.replaces.empty()) print_wrapped_block("  Replaces:    ", join_strings(meta.replaces));
    if (result.raw_only) {
        std::cout << "  Availability: "
                  << (result.installable
                          ? "available via on-demand Debian install"
                          : ("unavailable (" + result.reason + ")"))
                  << std::endl;
    }

    std::string installed_ver;
    if (is_installed(meta.name, &installed_ver)) {
        std::cout << "  Installed:   yes (" << installed_ver << ")" << std::endl;
    } else {
        PackageStatusRecord live_status;
        bool have_live_status =
            get_dpkg_package_status_record(meta.name, &live_status) &&
            package_status_is_installed_like(live_status.status);
        if (!have_live_status) {
            have_live_status =
                get_base_system_package_status_record(meta.name, &live_status) &&
                package_status_is_installed_like(live_status.status);
        }

        if (have_live_status) {
            if (package_is_base_system_provided(meta.name)) {
                std::cout << "  Installed:   base system";
                if (!live_status.version.empty()) {
                    std::cout << " (" << live_status.version << ")";
                }
                std::cout << std::endl;
            } else {
                std::cout << "  Installed:   yes";
                if (!live_status.version.empty()) {
                    std::cout << " (" << live_status.version << "; unmanaged live system)";
                } else {
                    std::cout << " (unmanaged live system)";
                }
                std::cout << std::endl;
            }
        } else if (package_is_base_system_provided(meta.name)) {
            std::cout << "  Installed:   base system" << std::endl;
        } else {
            std::cout << "  Installed:   no" << std::endl;
        }
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
            std::cout << Color::YELLOW << "W: Repository already configured: "
                      << normalized << Color::RESET << std::endl;
            return 0;
        }
    }

    std::string check_url = build_repo_index_url(normalized);
    std::cout << "Validating repository " << normalized << "..." << std::endl;

    std::string tmp_index = "/tmp/gpkg_validation_index.zst";
    std::string download_error;
    if (!DownloadFile(check_url, tmp_index, verbose, &download_error)) {
        std::cerr << Color::RED << "E: Validation failed.";
        if (!download_error.empty()) std::cerr << " " << download_error;
        std::cerr << Color::RESET << std::endl;
        return 1;
    }

    std::string validation_error;
    if (!GpkgArchive::decompress_zstd_file(tmp_index, "/tmp/gpkg_validation.json", &validation_error)) {
        std::cerr << Color::RED << "E: Failed to decompress repository index.";
        if (!validation_error.empty()) std::cerr << " " << validation_error;
        std::cerr << Color::RESET << std::endl;
        return 1;
    }
    std::ifstream f_check("/tmp/gpkg_validation.json");
    std::string content((std::istreambuf_iterator<char>(f_check)), std::istreambuf_iterator<char>());
    if (content.find("\"package\":") == std::string::npos) {
        std::cerr << Color::RED << "E: Invalid repository index." << Color::RESET << std::endl;
        return 1;
    }

    std::cout << Color::GREEN << "✓ Repository validated." << Color::RESET << std::endl;
    std::string name = "repo_" + std::to_string(time(nullptr)) + ".list";
    run_command("mkdir -p " + SOURCES_DIR, verbose);
    std::ofstream f(SOURCES_DIR + name);
    if (f) {
        f << normalized << std::endl;
    } else {
        std::cerr << "E: Failed to write to " << SOURCES_DIR << name << std::endl;
    }

    return 0;
}

int handle_clean(bool verbose) {
    std::cout << "Cleaning package cache..." << std::endl;
    invalidate_repo_package_cache();
    if (!clear_repo_cache_contents(verbose)) return 1;
    std::cout << Color::GREEN
              << "✓ Removed cached package archives, converted imports, and partial downloads. Kept package indices."
              << Color::RESET << std::endl;
    return 0;
}
