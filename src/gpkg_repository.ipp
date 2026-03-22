// Repository configuration, index management, and repo-backed commands.

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
    bool found = false;
    foreach_json_object(REPO_CACHE_PATH + "Packages.json", [&](const std::string& obj) {
        std::string name;
        if (get_json_value(obj, "package", name) && trim(name) == pkg_name) {
            PackageMetadata candidate;
            populate_package_metadata_from_json(obj, candidate);
            candidate.name = trim(name);

            if (!found || should_prefer_repo_candidate(candidate, out_meta)) {
                out_meta = candidate;
            }
            found = true;
        }
        return true;
    });
    return found;
}

int handle_list_repos() {
    auto urls = get_repo_urls();
    DebianBackendConfig debian = load_debian_backend_config(false);
    std::cout << "Configured package sources:" << std::endl;
    std::cout << "  1. Debian sid (" << debian.packages_url << ")" << std::endl;
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

    std::cout << Color::GREEN << "✓ Merged " << total_packages << " packages from "
              << success_count << " sources." << Color::RESET << std::endl;
    return 0;
}

int handle_search(const std::string& query, bool verbose) {
    if (!ensure_repo_index_available()) return 1;

    VLOG(verbose, "Searching for '" << query << "' in " << REPO_CACHE_PATH << "Packages.json");
    std::map<std::string, PackageMetadata> matches;
    foreach_json_object(REPO_CACHE_PATH + "Packages.json", [&](const std::string& obj) {
        PackageMetadata meta;
        populate_package_metadata_from_json(obj, meta);

        if (meta.name.find(query) != std::string::npos || meta.description.find(query) != std::string::npos) {
            auto it = matches.find(meta.name);
            if (it == matches.end() || should_prefer_repo_candidate(meta, it->second)) {
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
        std::string status_str;
        std::string repo_str = format_package_origin(meta).empty() ? ""
            : (Color::CYAN + " [source: " + format_package_origin(meta) + "]" + Color::RESET);

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

    run_command("zstd -df " + tmp_index + " -o /tmp/gpkg_validation.json", verbose);
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
    run_command("rm -rf " + REPO_CACHE_PATH + "*", verbose);
    return 0;
}
