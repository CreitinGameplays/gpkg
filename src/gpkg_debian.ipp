// Debian sid backend: config loading, metadata import, and .deb to .gpkg conversion.

#include <cstdint>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

struct DebianBackendConfig {
    std::string packages_url = "https://deb.debian.org/debian/dists/sid/main/binary-amd64/Packages.gz";
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
    std::string depends_raw;
    std::string pre_depends_raw;
    std::string recommends_raw;
    std::string suggests_raw;
    std::string conflicts_raw;
    std::string provides_raw;
    std::string description;
    bool essential = false;
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
        record.pre_depends_raw = get_field("Pre-Depends");
        record.depends_raw = get_field("Depends");
        record.recommends_raw = get_field("Recommends");
        record.suggests_raw = get_field("Suggests");
        record.conflicts_raw = get_field("Conflicts");
        record.provides_raw = get_field("Provides");
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

std::map<std::string, std::vector<std::string>> build_debian_provider_map(
    const std::vector<DebianPackageRecord>& records,
    const std::string& apt_arch
) {
    std::map<std::string, std::vector<std::string>> providers;
    for (const auto& record : records) {
        std::vector<std::string> provided = normalize_relation_field_value(record.provides_raw, apt_arch);
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
        false,
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
    meta.depends = depends;
    meta.recommends = recommends;
    meta.suggests = suggests;
    meta.conflicts = normalize_relation_field_value(record.conflicts_raw, config.apt_arch);
    meta.provides = normalize_relation_field_value(record.provides_raw, config.apt_arch);
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
    std::vector<std::string> system_drop_patterns = build_system_drop_patterns(available_packages);
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
            if (record.essential) {
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
    if (!mkdir_parent(packages_gz)) return false;

    std::string download_error;
    if (!DownloadFile(config.packages_url, packages_gz, verbose, &download_error)) {
        std::cerr << Color::YELLOW << "W: Failed to fetch Debian Packages index from "
                  << config.packages_url;
        if (!download_error.empty()) std::cerr << " (" << download_error << ")";
        std::cerr << Color::RESET << std::endl;
        return false;
    }

    std::string unpack_cmd = "gunzip -c " + shell_quote(packages_gz) + " > " + shell_quote(packages_txt);
    if (run_command(unpack_cmd, verbose) != 0) {
        std::cerr << Color::YELLOW << "W: Failed to unpack Debian Packages index." << Color::RESET << std::endl;
        return false;
    }

    std::vector<std::string> skipped_policy;
    std::vector<PackageMetadata> entries = load_debian_index_entries(packages_txt, verbose, &skipped_policy);
    for (const auto& meta : entries) {
        if (!first_object) merged << ",\n";
        merged << package_metadata_to_json(meta);
        first_object = false;
        ++total_packages;
    }

    std::cout << Color::GREEN << "✓ Updated packages index"
              << " (" << entries.size() << " packages)" << Color::RESET << std::endl;
    return true;
}

std::string get_imported_gpkg_path(const PackageMetadata& meta) {
    std::string base = REPO_CACHE_PATH + "imported/"
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
        return decompress_gzip_file(archive_path, tar_path_out, error_out);
    }
    if (path_has_suffix(archive_path, ".tar.xz") || path_has_suffix(archive_path, ".tar.lzma")) {
        return decompress_xz_file(archive_path, tar_path_out, error_out);
    }
    if (path_has_suffix(archive_path, ".tar.zst") || path_has_suffix(archive_path, ".tar.zstd")) {
        return decompress_zstd_file(archive_path, tar_path_out, error_out);
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

    std::string tar_data_cmd = "tar -cf " + shell_quote(data_tar)
        + " -C " + shell_quote(payload_root) + " .";
    std::string failure_log;
    if (!run_quiet_import_command(tar_data_cmd, "gpkg-import-build", &failure_log)) {
        std::cerr << Color::RED << "E: Failed to stage the converted payload for "
                  << meta.name << format_import_failure_hint(failure_log)
                  << Color::RESET << std::endl;
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    std::string zstd_data_cmd = "zstd -T0 -f -10 --quiet "
        + shell_quote(data_tar) + " -o " + shell_quote(data_tar_zst);
    if (!run_quiet_import_command(zstd_data_cmd, "gpkg-import-build", &failure_log)) {
        std::cerr << Color::RED << "E: Failed to compress the converted payload for "
                  << meta.name << format_import_failure_hint(failure_log)
                  << Color::RESET << std::endl;
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    std::string build_tar_cmd = "tar -cf " + shell_quote(final_tar)
        + " -C " + shell_quote(temp_root) + " control.json data.tar.zst";
    if (!run_quiet_import_command(build_tar_cmd, "gpkg-import-build", &failure_log)) {
        std::cerr << Color::RED << "E: Failed to assemble the converted package for "
                  << meta.name << format_import_failure_hint(failure_log)
                  << Color::RESET << std::endl;
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    std::string final_cmd = "zstd -T0 -f -10 --quiet "
        + shell_quote(final_tar) + " -o " + shell_quote(output_path);
    bool ok = run_quiet_import_command(final_cmd, "gpkg-import-build", &failure_log);
    if (!ok) {
        std::cerr << Color::RED << "E: Failed to write the converted package cache entry for "
                  << meta.name << format_import_failure_hint(failure_log)
                  << Color::RESET << std::endl;
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
    std::string unpack_cmd = "cd " + shell_quote(temp_root) + " && ar x " + shell_quote(deb_path);
    std::string failure_log;
    if (!run_quiet_import_command(unpack_cmd, "gpkg-deb-import", &failure_log)) {
        std::cerr << Color::RED << "E: Failed to unpack the Debian archive for "
                  << meta.name << format_import_failure_hint(failure_log)
                  << Color::RESET << std::endl;
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

    std::string extract_cmd = "tar -xf " + shell_quote(payload_tar)
        + " -C " + shell_quote(payload_root);
    if (!run_quiet_import_command(extract_cmd, "gpkg-deb-import", &failure_log)) {
        std::cerr << Color::RED << "E: Failed to extract the prepared Debian payload tar for "
                  << meta.name << format_import_failure_hint(failure_log)
                  << Color::RESET << std::endl;
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    bool ok = build_imported_gpkg_archive(meta, payload_root, output_path, verbose);
    run_command("rm -rf " + shell_quote(temp_root), false);
    return ok;
}
