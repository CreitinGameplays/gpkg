// Install, upgrade, and remove command handlers.

bool install_package_from_file(const std::string& pkg_file, bool verbose) {
    std::string cmd = "gpkg-worker --install " + pkg_file;
    if (verbose) cmd += " --verbose";
    if (!ROOT_PREFIX.empty()) cmd += " --root " + ROOT_PREFIX;
    return run_command(cmd, verbose) == 0;
}

bool install_package_v2(const std::string& pkg_name, bool verbose) {
    PackageMetadata meta;
    meta.name = pkg_name;
    return install_package_from_file(get_cached_package_path(meta), verbose);
}

bool get_installed_package_metadata(const std::string& pkg_name, PackageMetadata& out_meta) {
    std::ifstream f(INFO_DIR + pkg_name + ".json");
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

bool is_required_by_others(const std::string& pkg, const std::set<std::string>& excluding, bool verbose) {
    auto all_installed = get_installed_packages();
    for (const auto& other : all_installed) {
        if (excluding.count(other)) continue;

        PackageMetadata meta;
        if (!get_installed_package_metadata(other, meta)) continue;

        for (const auto& dep_str : meta.depends) {
            Dependency dep = parse_dependency(dep_str);
            if (dep.name == pkg) {
                VLOG(verbose, pkg << " is still required by " << other);
                return true;
            }

            PackageMetadata pkg_meta;
            if (!get_installed_package_metadata(pkg, pkg_meta)) continue;
            for (const auto& provided : pkg_meta.provides) {
                Dependency prov = parse_dependency(provided);
                if (prov.name == dep.name) {
                    VLOG(verbose, pkg << " provides " << prov.name << " which is required by " << other);
                    return true;
                }
            }
        }
    }

    return false;
}

int handle_upgrade(const std::set<std::string>& installed_cache, bool verbose) {
    if (!ensure_repo_index_available()) return 1;

    std::cout << "Reading package lists..." << std::endl;
    VLOG(verbose, "Checking " << installed_cache.size() << " installed packages for updates.");

    std::vector<PackageMetadata> updates;
    for (const auto& pkg : installed_cache) {
        std::string current_ver;
        if (!is_installed(pkg, &current_ver)) continue;

        PackageMetadata repo_meta;
        if (get_repo_package_info(pkg, repo_meta) && compare_versions(repo_meta.version, current_ver) > 0) {
            VLOG(verbose, "Update found for " << pkg << ": " << current_ver << " -> " << repo_meta.version);
            updates.push_back(repo_meta);
        }
    }

    if (updates.empty()) {
        std::cout << "All packages are up to date." << std::endl;
        return 0;
    }

    std::cout << "The following packages will be upgraded:" << std::endl;
    for (const auto& update : updates) {
        std::cout << "  " << Color::GREEN << update.name << Color::RESET
                  << " (" << update.version << ")" << std::endl;
    }
    if (!ask_confirmation("Do you want to continue?")) return 0;

    std::cout << Color::CYAN << "[*] Downloading "
              << updates.size() << " package(s)..." << Color::RESET << std::endl;
    DownloadBatchReport download_report = download_package_archives(
        updates,
        verbose,
        MAX_PARALLEL_PACKAGE_DOWNLOADS
    );
    std::cout << Color::CYAN << "[*] Download summary: "
              << download_report.downloaded_count << " downloaded, "
              << download_report.reused_count << " reused from cache, "
              << format_total_bytes(download_report.downloaded_bytes) << " transferred."
              << Color::RESET << std::endl;

    size_t upgraded_count = 0;
    std::vector<std::string> failures;
    for (size_t i = 0; i < updates.size(); ++i) {
        if (!download_report.results[i].success) {
            failures.push_back(updates[i].name);
            continue;
        }

        std::cout << "Upgrading (" << (i + 1) << "/" << updates.size() << ") "
                  << updates[i].name << "..." << std::endl;
        if (!install_package_v2(updates[i].name, verbose)) {
            std::cerr << Color::RED << "E: Failed to upgrade " << updates[i].name
                      << Color::RESET << std::endl;
            failures.push_back(updates[i].name);
            continue;
        }

        ++upgraded_count;
    }

    std::cout << Color::CYAN << "Upgrade summary: "
              << upgraded_count << " upgraded, "
              << download_report.downloaded_count << " downloaded, "
              << download_report.reused_count << " reused from cache, "
              << format_total_bytes(download_report.downloaded_bytes) << " transferred."
              << Color::RESET << std::endl;

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

    if (needs_repo_index && !ensure_repo_index_available()) return 1;

    for (int i = 2; i < argc; ++i) {
        std::string arg = trim(argv[i]);
        if (arg == "-v" || arg == "--verbose") continue;
        if (arg.length() > 5 && arg.substr(arg.length() - 5) == ".gpkg" && access(arg.c_str(), F_OK) == 0) {
            continue;
        }

        if (!resolve_dependencies(arg, "", "", install_queue, visited, installed_cache, verbose)) {
            std::cerr << Color::RED << "E: Failed to resolve dependencies for " << arg
                      << Color::RESET << std::endl;
            return 1;
        }
    }

    for (const auto& local_file : local_files) {
        std::cout << "Installing local package: " << local_file << std::endl;
        if (!install_package_from_file(local_file, verbose)) return 1;
    }

    if (install_queue.empty()) {
        if (local_files.empty()) std::cout << "Nothing to do." << std::endl;
        return 0;
    }

    std::cout << "The following NEW packages will be installed:" << std::endl;
    for (const auto& pkg : install_queue) {
        std::cout << "  " << Color::GREEN << pkg.name << Color::RESET
                  << " (" << pkg.version << ")" << std::endl;
    }

    if (!check_conflicts(install_queue, installed_cache, verbose)) return 1;
    if (!ask_confirmation("Do you want to continue?")) return 0;

    std::cout << Color::CYAN << "[*] Downloading "
              << install_queue.size() << " package(s)..." << Color::RESET << std::endl;
    DownloadBatchReport download_report = download_package_archives(
        install_queue,
        verbose,
        MAX_PARALLEL_PACKAGE_DOWNLOADS
    );
    std::cout << Color::CYAN << "[*] Download summary: "
              << download_report.downloaded_count << " downloaded, "
              << download_report.reused_count << " reused from cache, "
              << format_total_bytes(download_report.downloaded_bytes) << " transferred."
              << Color::RESET << std::endl;

    std::vector<std::string> failed_downloads;
    for (size_t i = 0; i < install_queue.size(); ++i) {
        if (!download_report.results[i].success) {
            failed_downloads.push_back(install_queue[i].name);
        }
    }
    if (!failed_downloads.empty()) {
        std::cerr << Color::RED << "E: Aborting install because these packages could not be fetched safely: "
                  << join_strings(failed_downloads) << Color::RESET << std::endl;
        return 1;
    }

    std::cout << Color::CYAN << "[*] Installing " << install_queue.size()
              << " package(s)..." << Color::RESET << std::endl;
    size_t installed_count = 0;
    for (size_t i = 0; i < install_queue.size(); ++i) {
        std::cout << "Installing (" << (i + 1) << "/" << install_queue.size() << ") "
                  << install_queue[i].name << "..." << std::endl;
        if (!install_package_v2(install_queue[i].name, verbose)) {
            std::cerr << Color::RED << "E: Installation stopped at " << install_queue[i].name
                      << ". " << installed_count << " package(s) were installed before the failure."
                      << Color::RESET << std::endl;
            return 1;
        }
        ++installed_count;
    }

    std::cout << Color::GREEN << "✓ Installed " << installed_count << " package(s)." << Color::RESET << std::endl;
    return 0;
}

int handle_remove(int argc, char* argv[], bool verbose, bool purge) {
    if (argc < 3) {
        std::cerr << "Usage: gpkg remove <package_name> [--purge]" << std::endl;
        return 1;
    }

    std::string target_pkg;
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
        std::cerr << Color::RED << "E: Package '" << target_pkg << "' is not installed."
                  << Color::RESET << std::endl;
        return 1;
    }

    std::vector<std::string> to_remove = {target_pkg};
    std::set<std::string> removal_set = {target_pkg};

    if (purge) {
        std::cout << "Calculating dependencies for purge..." << std::endl;
        bool changed = true;
        while (changed) {
            changed = false;
            std::vector<std::string> current_removals = to_remove;
            for (const auto& pkg : current_removals) {
                PackageMetadata meta;
                if (!get_installed_package_metadata(pkg, meta)) continue;

                for (const auto& dep_str : meta.depends) {
                    Dependency dep = parse_dependency(dep_str);
                    if (!is_installed(dep.name) || removal_set.count(dep.name)) continue;
                    if (is_required_by_others(dep.name, removal_set, verbose)) continue;

                    to_remove.push_back(dep.name);
                    removal_set.insert(dep.name);
                    changed = true;
                }
            }
        }
    }

    std::cout << "The following packages will be REMOVED:" << std::endl;
    for (const auto& pkg : to_remove) {
        std::cout << "  " << Color::RED << pkg << Color::RESET << std::endl;
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
