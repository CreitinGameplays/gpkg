// Install, upgrade, and remove command handlers.

struct InstallCommandResult {
    bool success = false;
    std::string log_path;
};

std::string truncate_install_label(const std::string& value, size_t max_len = 32) {
    if (value.size() <= max_len) return value;
    if (max_len <= 3) return value.substr(0, max_len);
    return value.substr(0, max_len - 3) + "...";
}

void render_install_progress(
    size_t completed,
    size_t total,
    const std::string& current_pkg,
    size_t* last_render_width
) {
    if (total == 0) return;

    const size_t width = 32;
    const double ratio = static_cast<double>(completed) / static_cast<double>(total);
    const size_t filled = static_cast<size_t>(ratio * static_cast<double>(width) + 0.5);

    std::ostringstream line;
    line << Color::CYAN << "[";
    for (size_t i = 0; i < width; ++i) {
        line << (i < filled ? "#" : ".");
    }
    line << "] " << Color::RESET
         << std::setw(3) << static_cast<int>(ratio * 100.0 + 0.5) << "% "
         << "(" << completed << "/" << total << ") "
         << "current:" << truncate_install_label(current_pkg);

    std::string rendered = line.str();
    size_t visible_width = rendered.size();
    if (last_render_width && *last_render_width > visible_width) {
        rendered += std::string(*last_render_width - visible_width, ' ');
    }

    std::cout << "\r" << rendered << std::flush;
    if (last_render_width) *last_render_width = std::max(*last_render_width, visible_width);
}

void finish_install_progress_line(size_t* last_render_width) {
    if (!last_render_width || *last_render_width == 0) return;
    std::cout << "\r" << std::string(*last_render_width, ' ') << "\r" << std::flush;
    *last_render_width = 0;
}

InstallCommandResult install_package_from_file(const std::string& pkg_file, bool verbose) {
    std::string cmd = "gpkg-worker --install " + shell_quote(pkg_file);
    if (verbose) cmd += " --verbose";
    if (!ROOT_PREFIX.empty()) cmd += " --root " + shell_quote(ROOT_PREFIX);
    CommandCaptureResult result = run_command_captured(cmd, verbose, "gpkg-install");
    return {result.exit_code == 0, result.log_path};
}

InstallCommandResult install_package_v2(const std::string& pkg_name, bool verbose) {
    PackageMetadata meta;
    meta.name = pkg_name;
    return install_package_from_file(get_cached_package_path(meta), verbose);
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

void append_unique_message(std::vector<std::string>& messages, const std::string& message) {
    if (std::find(messages.begin(), messages.end(), message) == messages.end()) {
        messages.push_back(message);
    }
}

std::vector<std::string> get_registered_package_names() {
    std::set<std::string> package_names;
    for (const auto& pkg : get_installed_packages(".json")) {
        package_names.insert(pkg);
    }
    for (const auto& pkg : get_installed_packages(".list")) {
        package_names.insert(pkg);
    }

    return std::vector<std::string>(package_names.begin(), package_names.end());
}

bool verify_installed_package(const std::string& pkg_name, bool verbose, std::string* log_path = nullptr) {
    std::string cmd = "gpkg-worker --verify " + shell_quote(pkg_name);
    if (verbose) cmd += " --verbose";
    if (!ROOT_PREFIX.empty()) cmd += " --root " + shell_quote(ROOT_PREFIX);
    CommandCaptureResult result = run_command_captured(cmd, verbose, "gpkg-verify");
    if (log_path) *log_path = result.log_path;
    return result.exit_code == 0;
}

struct RepairInspection {
    std::vector<std::string> detected_issues;
    std::vector<std::string> unresolved_issues;
    std::vector<PackageMetadata> install_queue;
    std::vector<PackageMetadata> reinstall_queue;
};

RepairInspection inspect_repair_state(bool verbose) {
    RepairInspection inspection;
    std::vector<std::string> registered_packages = get_registered_package_names();
    std::set<std::string> installed_cache(registered_packages.begin(), registered_packages.end());
    std::set<std::string> reinstall_targets;
    std::set<std::string> visited;

    for (const auto& pkg : registered_packages) {
        const bool has_json = access((INFO_DIR + pkg + ".json").c_str(), F_OK) == 0;
        const bool has_list = access((INFO_DIR + pkg + ".list").c_str(), F_OK) == 0;
        bool needs_reinstall = false;

        if (!has_json || !has_list) {
            std::string missing_parts;
            if (!has_json) missing_parts += ".json";
            if (!has_json && !has_list) missing_parts += " and ";
            if (!has_list) missing_parts += ".list";
            append_unique_message(
                inspection.detected_issues,
                pkg + ": incomplete local package metadata (" + missing_parts + " missing)"
            );
            needs_reinstall = true;
        }

        PackageMetadata installed_meta;
        if (!has_json || !get_installed_package_metadata(pkg, installed_meta) || installed_meta.version.empty()) {
            if (has_json) {
                append_unique_message(
                    inspection.detected_issues,
                    pkg + ": installed metadata is unreadable or missing a version"
                );
            }
            needs_reinstall = true;
        } else {
            for (const auto& dep_str : installed_meta.depends) {
                Dependency dep = parse_dependency(dep_str);
                std::string provider_name;
                if (is_dependency_satisfied_locally(dep, installed_cache, verbose, &provider_name)) continue;

                append_unique_message(
                    inspection.detected_issues,
                    pkg + ": unsatisfied dependency " + dep_str
                );

                if (!resolve_dependencies(
                        dep.name,
                        dep.op,
                        dep.version,
                        inspection.install_queue,
                        visited,
                        installed_cache,
                        verbose
                    )) {
                    append_unique_message(
                        inspection.unresolved_issues,
                        pkg + ": unable to resolve dependency " + dep_str
                    );
                }
            }
        }

        if (!needs_reinstall) {
            std::string verify_log_path;
            if (!verify_installed_package(pkg, verbose, &verify_log_path)) {
                std::string issue = pkg + ": installed files are missing or inconsistent";
                if (!verbose && !verify_log_path.empty()) {
                    issue += " (see " + verify_log_path + ")";
                }
                append_unique_message(inspection.detected_issues, issue);
                needs_reinstall = true;
            }
        }

        if (needs_reinstall) {
            reinstall_targets.insert(pkg);
        }
    }

    std::set<std::string> queued_names;
    for (const auto& meta : inspection.install_queue) {
        queued_names.insert(meta.name);
    }

    for (const auto& pkg : reinstall_targets) {
        if (queued_names.count(pkg)) continue;

        PackageMetadata repo_meta;
        if (!get_repo_package_info(pkg, repo_meta)) {
            append_unique_message(
                inspection.unresolved_issues,
                pkg + ": automatic reinstall is not possible because no repository package is available"
            );
            continue;
        }

        inspection.reinstall_queue.push_back(repo_meta);
    }

    return inspection;
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
    size_t install_progress_width = 0;
    std::cout << Color::CYAN << "[*] Installing " << updates.size()
              << " package(s)..." << Color::RESET << std::endl;
    for (size_t i = 0; i < updates.size(); ++i) {
        if (!download_report.results[i].success) {
            failures.push_back(updates[i].name);
            continue;
        }

        if (!verbose) render_install_progress(i, updates.size(), updates[i].name, &install_progress_width);
        InstallCommandResult result = install_package_v2(updates[i].name, verbose);
        if (!result.success) {
            if (!verbose) finish_install_progress_line(&install_progress_width);
            std::cerr << Color::RED << "E: Failed to upgrade " << updates[i].name
                      << Color::RESET;
            if (!verbose && !result.log_path.empty()) {
                std::cerr << " (see " << result.log_path << ")";
            }
            std::cerr << std::endl;
            failures.push_back(updates[i].name);
            continue;
        }

        ++upgraded_count;
        if (!verbose) render_install_progress(i + 1, updates.size(), updates[i].name, &install_progress_width);
    }
    if (!verbose) {
        finish_install_progress_line(&install_progress_width);
        std::cout << Color::GREEN << "✓ Installed " << upgraded_count << "/" << updates.size()
                  << " package(s)." << Color::RESET << std::endl;
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

int handle_repair(bool verbose) {
    if (!ensure_repo_index_available()) return 1;

    std::cout << "Inspecting installed packages..." << std::endl;
    RepairInspection inspection = inspect_repair_state(verbose);

    if (inspection.detected_issues.empty()) {
        std::cout << "No broken packages found." << std::endl;
        return 0;
    }

    std::cout << "Detected issues:" << std::endl;
    for (const auto& issue : inspection.detected_issues) {
        std::cout << "  " << Color::YELLOW << issue << Color::RESET << std::endl;
    }

    std::vector<PackageMetadata> repair_queue = inspection.install_queue;
    repair_queue.insert(
        repair_queue.end(),
        inspection.reinstall_queue.begin(),
        inspection.reinstall_queue.end()
    );

    if (repair_queue.empty()) {
        std::cerr << Color::RED
                  << "E: Broken packages were detected, but gpkg could not build an automatic repair plan."
                  << Color::RESET << std::endl;
        for (const auto& issue : inspection.unresolved_issues) {
            std::cerr << Color::RED << "  " << issue << Color::RESET << std::endl;
        }
        return 1;
    }

    if (!inspection.install_queue.empty()) {
        std::cout << "The following packages will be installed to satisfy dependencies:" << std::endl;
        for (const auto& pkg : inspection.install_queue) {
            std::cout << "  " << Color::GREEN << pkg.name << Color::RESET
                      << " (" << pkg.version << ")" << std::endl;
        }
    }

    if (!inspection.reinstall_queue.empty()) {
        std::cout << "The following installed packages will be reinstalled:" << std::endl;
        for (const auto& pkg : inspection.reinstall_queue) {
            std::cout << "  " << Color::BLUE << pkg.name << Color::RESET
                      << " (" << pkg.version << ")" << std::endl;
        }
    }

    if (!inspection.unresolved_issues.empty()) {
        std::cout << Color::YELLOW
                  << "W: Some issues may remain after this repair attempt:"
                  << Color::RESET << std::endl;
        for (const auto& issue : inspection.unresolved_issues) {
            std::cout << "  " << Color::YELLOW << issue << Color::RESET << std::endl;
        }
    }

    std::vector<std::string> registered_packages = get_registered_package_names();
    std::set<std::string> installed_set(registered_packages.begin(), registered_packages.end());
    if (!check_conflicts(repair_queue, installed_set, verbose)) {
        return 1;
    }
    if (!ask_confirmation("Do you want to continue with the repair?")) return 0;

    std::cout << Color::CYAN << "[*] Downloading "
              << repair_queue.size() << " package(s)..." << Color::RESET << std::endl;
    DownloadBatchReport download_report = download_package_archives(
        repair_queue,
        verbose,
        MAX_PARALLEL_PACKAGE_DOWNLOADS
    );
    std::cout << Color::CYAN << "[*] Download summary: "
              << download_report.downloaded_count << " downloaded, "
              << download_report.reused_count << " reused from cache, "
              << format_total_bytes(download_report.downloaded_bytes) << " transferred."
              << Color::RESET << std::endl;

    std::vector<std::string> failed_downloads;
    for (size_t i = 0; i < repair_queue.size(); ++i) {
        if (!download_report.results[i].success) {
            failed_downloads.push_back(repair_queue[i].name);
        }
    }
    if (!failed_downloads.empty()) {
        std::cerr << Color::RED << "E: Aborting repair because these packages could not be fetched safely: "
                  << join_strings(failed_downloads) << Color::RESET << std::endl;
        return 1;
    }

    std::cout << Color::CYAN << "[*] Applying repair plan..." << Color::RESET << std::endl;
    size_t repaired_count = 0;
    size_t install_progress_width = 0;
    std::vector<std::string> failures;
    for (size_t i = 0; i < repair_queue.size(); ++i) {
        if (!verbose) render_install_progress(i, repair_queue.size(), repair_queue[i].name, &install_progress_width);
        InstallCommandResult result = install_package_v2(repair_queue[i].name, verbose);
        if (!result.success) {
            if (!verbose) finish_install_progress_line(&install_progress_width);
            std::cerr << Color::RED << "E: Repair stopped at " << repair_queue[i].name
                      << Color::RESET;
            if (!verbose && !result.log_path.empty()) {
                std::cerr << " (see " << result.log_path << ")";
            }
            std::cerr << std::endl;
            failures.push_back(repair_queue[i].name);
            break;
        }
        ++repaired_count;
        if (!verbose) render_install_progress(i + 1, repair_queue.size(), repair_queue[i].name, &install_progress_width);
    }
    if (!verbose) finish_install_progress_line(&install_progress_width);

    if (!failures.empty()) {
        return 1;
    }

    std::cout << Color::GREEN << "✓ Applied repair plan to " << repaired_count
              << " package(s)." << Color::RESET << std::endl;

    std::cout << "Rechecking package state..." << std::endl;
    RepairInspection after_repair = inspect_repair_state(false);
    if (after_repair.detected_issues.empty()) {
        std::cout << Color::GREEN << "✓ Repair completed successfully." << Color::RESET << std::endl;
        return 0;
    }

    std::cerr << Color::YELLOW
              << "W: Repair completed, but some issues remain:"
              << Color::RESET << std::endl;
    for (const auto& issue : after_repair.detected_issues) {
        std::cerr << Color::YELLOW << "  " << issue << Color::RESET << std::endl;
    }
    for (const auto& issue : after_repair.unresolved_issues) {
        std::cerr << Color::RED << "  " << issue << Color::RESET << std::endl;
    }
    return 1;
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
        InstallCommandResult result = install_package_from_file(local_file, verbose);
        if (!result.success) {
            std::cerr << Color::RED << "E: Failed to install local package " << local_file
                      << Color::RESET;
            if (!verbose && !result.log_path.empty()) {
                std::cerr << " See " << result.log_path << " for details.";
            }
            std::cerr << std::endl;
            return 1;
        }
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
    size_t install_progress_width = 0;
    for (size_t i = 0; i < install_queue.size(); ++i) {
        if (!verbose) render_install_progress(i, install_queue.size(), install_queue[i].name, &install_progress_width);
        InstallCommandResult result = install_package_v2(install_queue[i].name, verbose);
        if (!result.success) {
            if (!verbose) finish_install_progress_line(&install_progress_width);
            std::cerr << Color::RED << "E: Installation stopped at " << install_queue[i].name
                      << ". " << installed_count << " package(s) were installed before the failure."
                      << Color::RESET;
            if (!verbose && !result.log_path.empty()) {
                std::cerr << " See " << result.log_path << " for details.";
            }
            std::cerr << std::endl;
            return 1;
        }
        ++installed_count;
        if (!verbose) render_install_progress(i + 1, install_queue.size(), install_queue[i].name, &install_progress_width);
    }
    if (!verbose) finish_install_progress_line(&install_progress_width);

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
