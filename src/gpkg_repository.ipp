// Repository configuration, index management, and repo-backed commands.

std::map<std::string, PackageMetadata> g_repo_package_cache;
std::map<std::string, std::vector<std::string>> g_repo_provider_cache;
bool g_repo_package_cache_loaded = false;

std::string relation_name_from_text(const std::string& relation) {
    size_t open_paren = relation.find('(');
    return trim(open_paren == std::string::npos ? relation : relation.substr(0, open_paren));
}

void invalidate_repo_package_cache() {
    g_repo_package_cache.clear();
    g_repo_provider_cache.clear();
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
    if (access((REPO_CACHE_PATH + "Packages.json").c_str(), F_OK) == 0) return true;

    std::cerr << Color::RED
              << "E: No package index available. Run 'gpkg update' first."
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
    get_json_array(obj, "depends", meta.depends);
    get_json_array(obj, "recommends", meta.recommends);
    get_json_array(obj, "suggests", meta.suggests);
    get_json_array(obj, "conflicts", meta.conflicts);
    get_json_array(obj, "provides", meta.provides);
    get_json_array(obj, "replaces", meta.replaces);
}

bool ensure_repo_package_cache_loaded(bool verbose) {
    if (g_repo_package_cache_loaded) return true;
    if (!ensure_repo_index_available()) return false;

    std::string index_path = REPO_CACHE_PATH + "Packages.json";
    std::map<std::string, PackageMetadata> packages;
    foreach_json_object(index_path, [&](const std::string& obj) {
        PackageMetadata candidate;
        populate_package_metadata_from_json(obj, candidate);
        candidate.name = trim(candidate.name);
        if (candidate.name.empty()) return true;

        auto it = packages.find(candidate.name);
        if (it == packages.end() || should_prefer_repo_candidate(candidate, it->second)) {
            packages[candidate.name] = candidate;
        }
        return true;
    });

    std::map<std::string, std::vector<std::string>> providers;
    for (const auto& entry : packages) {
        for (const auto& capability : entry.second.provides) {
            std::string relation_name = relation_name_from_text(capability);
            if (relation_name.empty()) continue;

            auto& provider_names = providers[relation_name];
            if (std::find(provider_names.begin(), provider_names.end(), entry.first) == provider_names.end()) {
                provider_names.push_back(entry.first);
            }
        }
    }

    g_repo_package_cache = std::move(packages);
    g_repo_provider_cache = std::move(providers);
    g_repo_package_cache_loaded = true;
    VLOG(verbose, "Loaded " << g_repo_package_cache.size() << " repository package records into memory.");
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
    auto it = g_repo_package_cache.find(pkg_name);
    if (it == g_repo_package_cache.end()) return false;
    out_meta = it->second;
    return true;
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

int handle_update(bool verbose) {
    auto urls = get_repo_urls();
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
        const std::string& url = urls[i];
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

        std::string decompress_error;
        if (!GpkgArchive::decompress_zstd_file(dest_zst, dest_json, &decompress_error)) {
            std::cerr << Color::YELLOW << "W: Failed to decompress index from " << url << Color::RESET << std::endl;
            if (verbose && !decompress_error.empty()) {
                std::cerr << "[DEBUG] Repo index decompress error: " << decompress_error << std::endl;
            }
            remove(dest_zst.c_str());
            continue;
        }

        int repo_package_count = 0;
        foreach_json_object(dest_json, [&](const std::string& obj) {
            if (!first_object) merged << ",\n";
            merged << inject_repo_url(obj, url);
            first_object = false;
            ++repo_package_count;
            ++total_packages;
            return true;
        });

        remove(dest_zst.c_str());
        remove(dest_json.c_str());

        ++success_count;
        std::cout << Color::GREEN << "✓ Updated index from " << url
                  << " (" << repo_package_count << " packages)" << Color::RESET << std::endl;
    }

    if (update_debian_backend_index(merged, first_object, total_packages, verbose)) {
        ++success_count;
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

    invalidate_repo_package_cache();
    std::cout << Color::GREEN << "✓ Merged " << total_packages << " packages from "
              << success_count << " sources." << Color::RESET << std::endl;
    return 0;
}

int handle_search(const std::string& query, bool verbose) {
    if (!ensure_repo_package_cache_loaded(verbose)) return 1;

    VLOG(verbose, "Searching for '" << query << "' in " << REPO_CACHE_PATH << "Packages.json");
    std::map<std::string, PackageMetadata> matches;
    for (const auto& entry : g_repo_package_cache) {
        const PackageMetadata& meta = entry.second;
        if (meta.name.find(query) != std::string::npos || meta.description.find(query) != std::string::npos) {
            auto it = matches.find(meta.name);
            if (it == matches.end() || should_prefer_repo_candidate(meta, it->second)) {
                matches[meta.name] = meta;
            }
        }
    }

    if (matches.empty()) {
        std::cout << "No matches found for '" << query << "'" << std::endl;
        return 0;
    }

    for (const auto& entry : matches) {
        const auto& meta = entry.second;
        std::string installed_ver;
        std::string status_str;
        std::string repo_str = format_package_origin(meta).empty() ? ""
            : (Color::CYAN + " [source: " + format_package_origin(meta) + "]" + Color::RESET);

        if (is_installed(meta.name, &installed_ver)) {
            if (compare_versions(installed_ver, meta.version) == 0) {
                status_str = Color::BLUE + " [installed]" + Color::RESET;
            } else {
                status_str = Color::BLUE + " [installed: " + installed_ver + "]" + Color::RESET;
            }
        } else if (package_is_base_system_provided(meta.name)) {
            status_str = Color::BLUE + " [base system]" + Color::RESET;
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
        std::cerr << Color::RED << "E: Package '" << pkg_name
                  << "' was not found in the local index. Run 'gpkg update' first."
                  << Color::RESET << std::endl;
        return 1;
    }

    std::cout << Color::GREEN << meta.name << Color::RESET << std::endl;
    std::cout << "  Version:     " << meta.version << std::endl;
    std::cout << "  Source:      " << format_package_origin(meta) << std::endl;
    std::cout << "  Filename:    " << meta.filename << std::endl;
    if (!meta.debian_package.empty()) std::cout << "  Debian Pkg:  " << meta.debian_package << std::endl;
    if (!meta.debian_version.empty()) std::cout << "  Debian Ver:  " << meta.debian_version << std::endl;
    if (!meta.description.empty()) print_description_block("Description", meta.description);
    if (!meta.depends.empty()) print_wrapped_block("  Depends:     ", join_strings(meta.depends));
    if (!meta.recommends.empty()) print_wrapped_block("  Recommends:  ", join_strings(meta.recommends));
    if (!meta.suggests.empty()) print_wrapped_block("  Suggests:    ", join_strings(meta.suggests));
    if (!meta.conflicts.empty()) print_wrapped_block("  Conflicts:   ", join_strings(meta.conflicts));
    if (!meta.provides.empty()) print_wrapped_block("  Provides:    ", join_strings(meta.provides));
    if (!meta.replaces.empty()) print_wrapped_block("  Replaces:    ", join_strings(meta.replaces));

    std::string installed_ver;
    if (is_installed(meta.name, &installed_ver)) {
        std::cout << "  Installed:   yes (" << installed_ver << ")" << std::endl;
    } else if (package_is_base_system_provided(meta.name)) {
        std::cout << "  Installed:   base system" << std::endl;
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
