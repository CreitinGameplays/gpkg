// Install, upgrade, and remove command handlers.

struct InstallCommandResult {
    bool success = false;
    std::string log_path;
};

enum class PlannedPackageKind {
    NewInstall,
    Upgrade,
    Reinstall,
    BaseSystemUpgrade,
};

PlannedPackageKind classify_planned_package(const PackageMetadata& meta, std::string* current_version = nullptr) {
    std::string installed_version;
    if (!is_installed(meta.name, &installed_version)) {
        if (package_is_base_system_provided(meta.name)) {
            if (current_version) *current_version = "base system";
            return PlannedPackageKind::BaseSystemUpgrade;
        }
        if (current_version) current_version->clear();
        return PlannedPackageKind::NewInstall;
    }

    if (current_version) *current_version = installed_version;
    return compare_versions(meta.version, installed_version) == 0
        ? PlannedPackageKind::Reinstall
        : PlannedPackageKind::Upgrade;
}

void render_package_progress(
    const std::string& item_label,
    size_t completed,
    size_t total,
    const std::string& current_pkg,
    size_t* last_render_width
) {
    if (total == 0) return;

    const size_t terminal_width = get_terminal_width();
    const size_t width = std::max<size_t>(10, std::min<size_t>(48, terminal_width > 72 ? terminal_width / 3 : 10));
    const double ratio = static_cast<double>(completed) / static_cast<double>(total);
    const size_t filled = static_cast<size_t>(ratio * static_cast<double>(width) + 0.5);
    const size_t base_width = width + item_label.size() + 20;
    const size_t label_width = terminal_width > base_width ? terminal_width - base_width : 12;

    std::ostringstream line;
    line << Color::CYAN << "[";
    for (size_t i = 0; i < width; ++i) {
        line << (i < filled ? "#" : ".");
    }
    line << "] " << Color::RESET
         << std::setw(3) << static_cast<int>(ratio * 100.0 + 0.5) << "% "
         << "(" << completed << "/" << total << ") "
         << item_label << ":" << truncate_progress_label(current_pkg, std::max<size_t>(12, label_width));

    std::string rendered = line.str();
    size_t visible_width = visible_text_width(rendered);
    if (last_render_width && *last_render_width > visible_width) {
        rendered += std::string(*last_render_width - visible_width, ' ');
    }

    std::cout << "\r" << rendered << std::flush;
    if (last_render_width) *last_render_width = std::max(*last_render_width, visible_width);
}

void finish_progress_line(size_t* last_render_width) {
    if (!last_render_width || *last_render_width == 0) return;
    std::cout << "\r" << std::string(*last_render_width, ' ') << "\r" << std::flush;
    *last_render_width = 0;
}

std::vector<std::string> build_worker_command_argv(const std::string& mode, const std::string& value, bool verbose) {
    std::vector<std::string> argv = {"gpkg-worker", mode, value};
    if (verbose) argv.push_back("--verbose");
    if (!ROOT_PREFIX.empty()) {
        argv.push_back("--root");
        argv.push_back(ROOT_PREFIX);
    }
    return argv;
}

InstallCommandResult install_package_from_file(const std::string& pkg_file, bool verbose) {
    CommandCaptureResult result = run_command_captured_argv(
        build_worker_command_argv("--install", pkg_file, verbose),
        verbose,
        "gpkg-install"
    );
    return {result.exit_code == 0, result.log_path};
}

InstallCommandResult retire_package_by_name(const std::string& pkg_name, bool verbose) {
    CommandCaptureResult result = run_command_captured_argv(
        build_worker_command_argv("--retire", pkg_name, verbose),
        verbose,
        "gpkg-retire"
    );
    return {result.exit_code == 0, result.log_path};
}

InstallCommandResult remove_package_by_name(const std::string& pkg_name, bool verbose) {
    CommandCaptureResult result = run_command_captured_argv(
        build_worker_command_argv("--remove", pkg_name, verbose),
        verbose,
        "gpkg-remove"
    );
    return {result.exit_code == 0, result.log_path};
}

InstallCommandResult purge_package_by_name(const std::string& pkg_name, bool verbose) {
    CommandCaptureResult result = run_command_captured_argv(
        build_worker_command_argv("--purge", pkg_name, verbose),
        verbose,
        "gpkg-purge"
    );
    return {result.exit_code == 0, result.log_path};
}

std::string get_install_archive_path(const PackageMetadata& meta) {
    if (package_is_debian_source(meta)) return get_imported_gpkg_path(meta);
    return get_cached_package_path(meta);
}

bool ensure_install_archive_ready(const PackageMetadata& meta, bool verbose, std::string* error_out = nullptr) {
    if (error_out) error_out->clear();

    if (package_is_debian_source(meta)) {
        std::string output_path;
        if (!convert_debian_archive_to_gpkg(meta, verbose, &output_path)) {
            if (error_out) *error_out = "failed to convert Debian archive to gpkg";
            return false;
        }
        return true;
    }

    if (access(get_cached_package_path(meta).c_str(), F_OK) == 0) return true;
    if (error_out) *error_out = "cached gpkg archive is missing";
    return false;
}

void cleanup_converted_debian_archives(const std::vector<PackageMetadata>& packages) {
    for (const auto& meta : packages) {
        if (!package_is_debian_source(meta)) continue;
        if (access(get_imported_gpkg_path(meta).c_str(), F_OK) != 0) continue;

        std::string deb_path = get_cached_debian_archive_path(meta);
        if (access(deb_path.c_str(), F_OK) == 0) {
            unlink(deb_path.c_str());
        }
    }
}

bool prepare_install_archives(
    const std::vector<PackageMetadata>& packages,
    const DownloadBatchReport& download_report,
    bool verbose,
    std::vector<std::string>& failures
) {
    failures.clear();
    if (packages.empty()) return true;

    std::cout << Color::CYAN << "[*] Preparing " << packages.size()
              << " package(s)..." << Color::RESET << std::endl;
    const size_t worker_count = recommended_parallel_worker_count(packages.size());
    if (verbose) {
        std::cout << "[DEBUG] Preparing packages with "
                  << worker_count << " worker(s)." << std::endl;
    }

    std::atomic<size_t> next_index{0};
    std::atomic<size_t> completed_count{0};
    std::mutex state_mutex;
    size_t prepare_progress_width = 0;

    auto worker = [&]() {
        while (true) {
            size_t package_index = next_index.fetch_add(1);
            if (package_index >= packages.size()) return;

            std::string error;
            bool ok = true;
            if (download_report.results[package_index].success) {
                ok = ensure_install_archive_ready(packages[package_index], verbose, &error);
            }

            size_t completed = completed_count.fetch_add(1) + 1;
            std::lock_guard<std::mutex> lock(state_mutex);
            if (!ok) {
                std::string message = packages[package_index].name;
                if (!error.empty()) message += " (" + error + ")";
                failures.push_back(message);
            }
            if (!verbose) {
                render_package_progress("prep", completed, packages.size(), packages[package_index].name, &prepare_progress_width);
            }
        }
    };

    std::vector<std::thread> workers;
    workers.reserve(worker_count > 0 ? worker_count - 1 : 0);
    for (size_t worker_index = 1; worker_index < worker_count; ++worker_index) {
        workers.emplace_back(worker);
    }
    worker();
    for (auto& thread : workers) {
        thread.join();
    }

    if (!verbose) finish_progress_line(&prepare_progress_width);
    if (!failures.empty()) return false;

    cleanup_converted_debian_archives(packages);
    if (!verbose) {
        std::cout << Color::GREEN << "✓ Prepared " << packages.size()
                  << " package(s)." << Color::RESET << std::endl;
    }
    return failures.empty();
}

InstallCommandResult install_package_v2(const PackageMetadata& meta, bool verbose) {
    return install_package_from_file(get_install_archive_path(meta), verbose);
}

std::string read_package_name_from_archive(const std::string& pkg_file) {
    char temp_template[] = "/tmp/gpkg-inspect-XXXXXX";
    int fd = mkstemp(temp_template);
    if (fd < 0) return "";
    close(fd);

    std::string temp_tar = temp_template;
    std::string decompress_error;
    if (!GpkgArchive::decompress_zstd_file(pkg_file, temp_tar, &decompress_error)) {
        unlink(temp_tar.c_str());
        return "";
    }

    std::string control_json;
    std::string tar_error;
    bool ok = GpkgArchive::tar_read_file(temp_tar, "control.json", control_json, &tar_error);
    unlink(temp_tar.c_str());
    if (!ok || control_json.empty()) return "";

    std::string pkg_name;
    if (!get_json_value(control_json, "package", pkg_name)) return "";
    return pkg_name;
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

void sort_removal_queue_for_operation(std::vector<std::string>& to_remove, bool verbose) {
    if (to_remove.size() < 2) return;

    std::map<std::string, PackageMetadata> meta_by_name;
    std::vector<std::string> filtered;
    filtered.reserve(to_remove.size());
    for (const auto& pkg : to_remove) {
        PackageMetadata meta;
        if (!get_installed_package_metadata(pkg, meta)) {
            filtered.push_back(pkg);
            continue;
        }
        meta_by_name[pkg] = meta;
        filtered.push_back(pkg);
    }
    to_remove.swap(filtered);

    const size_t n = to_remove.size();
    std::vector<std::set<size_t>> outgoing(n);
    std::vector<size_t> indegree(n, 0);

    auto add_edge = [&](size_t from, size_t to) {
        if (from == to) return;
        if (outgoing[from].insert(to).second) ++indegree[to];
    };

    for (size_t i = 0; i < n; ++i) {
        auto meta_it = meta_by_name.find(to_remove[i]);
        if (meta_it == meta_by_name.end()) continue;

        for (const auto& dep_str : collect_transaction_dependency_edges(meta_it->second)) {
            Dependency dep = parse_dependency(dep_str);
            if (dep.name.empty()) continue;

            for (size_t j = 0; j < n; ++j) {
                if (i == j) continue;
                auto provider_it = meta_by_name.find(to_remove[j]);
                if (provider_it == meta_by_name.end()) continue;
                if (!package_metadata_satisfies_dependency(to_remove[j], provider_it->second, dep)) continue;
                add_edge(i, j);
                break;
            }
        }
    }

    std::vector<std::string> ordered;
    ordered.reserve(n);
    std::vector<bool> emitted(n, false);
    for (size_t emitted_count = 0; emitted_count < n; ++emitted_count) {
        size_t best = n;
        for (size_t i = 0; i < n; ++i) {
            if (emitted[i] || indegree[i] != 0) continue;
            best = i;
            break;
        }

        if (best == n) {
            VLOG(verbose, "Falling back to original removal order because dependency ordering contains a cycle or unresolved provider ambiguity.");
            return;
        }

        emitted[best] = true;
        ordered.push_back(to_remove[best]);
        for (size_t succ : outgoing[best]) {
            if (indegree[succ] > 0) --indegree[succ];
        }
    }

    to_remove.swap(ordered);
}

std::vector<std::string> get_registered_package_names() {
    std::set<std::string> package_names;
    std::map<std::string, std::string> status_by_package;
    for (const auto& record : load_package_status_records()) {
        if (record.package.empty()) continue;
        status_by_package[record.package] = record.status;
        if (!package_status_is_installed_like(record.status)) continue;
        package_names.insert(record.package);
    }
    for (const auto& pkg : get_installed_packages(".json")) {
        auto status_it = status_by_package.find(pkg);
        if (status_it != status_by_package.end() &&
            !package_status_is_installed_like(status_it->second)) {
            continue;
        }
        package_names.insert(pkg);
    }
    for (const auto& pkg : get_installed_packages(".list")) {
        auto status_it = status_by_package.find(pkg);
        if (status_it != status_by_package.end() &&
            !package_status_is_installed_like(status_it->second)) {
            continue;
        }
        package_names.insert(pkg);
    }

    return std::vector<std::string>(package_names.begin(), package_names.end());
}

bool package_is_config_files_only(const std::string& pkg_name, std::string* out_version = nullptr) {
    PackageStatusRecord record;
    if (!get_package_status_record(pkg_name, &record)) return false;
    if (record.status != "config-files") return false;
    if (out_version) *out_version = record.version;
    return true;
}

bool package_is_removal_protected(const std::string& pkg_name, std::string* reason_out = nullptr) {
    if (reason_out) reason_out->clear();
    if (pkg_name.empty()) return false;

    const ImportPolicy& policy = get_import_policy(false);
    if (matches_any_pattern(pkg_name, policy.allow_essential_packages)) {
        if (reason_out) *reason_out = "it is marked essential by GeminiOS policy";
        return true;
    }

    if (is_upgradeable_system_package(pkg_name)) {
        if (reason_out) *reason_out = "it is part of the GeminiOS upgradeable base system";
        return true;
    }

    PackageMetadata meta;
    if (get_installed_package_metadata(pkg_name, meta)) {
        std::string priority = meta.priority;
        std::transform(priority.begin(), priority.end(), priority.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (priority == "required") {
            if (reason_out) *reason_out = "its package priority is 'required'";
            return true;
        }
    }

    return false;
}

bool verify_installed_package(const std::string& pkg_name, bool verbose, std::string* log_path = nullptr) {
    CommandCaptureResult result = run_command_captured_argv(
        build_worker_command_argv("--verify", pkg_name, verbose),
        verbose,
        "gpkg-verify"
    );
    if (log_path) *log_path = result.log_path;
    return result.exit_code == 0;
}

struct InstalledKernelPayloadInfo {
    std::string package;
    std::string version;
    std::string release;
};

bool get_installed_kernel_payload_info(const std::string& pkg_name, InstalledKernelPayloadInfo* out = nullptr) {
    PackageMetadata meta;
    if (!get_installed_package_metadata(pkg_name, meta)) return false;

    std::vector<std::string> files = read_installed_file_list(pkg_name);
    if (!installed_file_list_contains_kernel_payload(files)) return false;

    std::string release = extract_kernel_release_from_installed_file_list(files);
    if (release.empty()) return false;

    if (out) {
        out->package = pkg_name;
        out->version = meta.version;
        out->release = release;
    }
    return true;
}

std::set<std::string> get_autoremove_protected_kernel_packages(bool verbose) {
    std::vector<InstalledKernelPayloadInfo> kernels;
    for (const auto& pkg_name : get_registered_package_names()) {
        InstalledKernelPayloadInfo info;
        if (get_installed_kernel_payload_info(pkg_name, &info)) {
            kernels.push_back(info);
        }
    }

    if (kernels.empty()) return {};

    std::sort(kernels.begin(), kernels.end(), [](const InstalledKernelPayloadInfo& left, const InstalledKernelPayloadInfo& right) {
        int version_cmp = compare_versions(left.version, right.version);
        if (version_cmp != 0) return version_cmp > 0;
        if (left.release != right.release) return left.release > right.release;
        return left.package < right.package;
    });

    std::string running_release = read_running_kernel_release();
    std::set<std::string> protected_packages;

    if (!running_release.empty()) {
        for (const auto& info : kernels) {
            if (info.release == running_release) protected_packages.insert(info.package);
        }
    }

    std::string fallback_release;
    for (const auto& info : kernels) {
        if (!running_release.empty() && info.release == running_release) continue;
        protected_packages.insert(info.package);
        fallback_release = info.release;
        break;
    }

    if (protected_packages.empty()) {
        protected_packages.insert(kernels.front().package);
        for (size_t i = 1; i < kernels.size(); ++i) {
            if (kernels[i].release == kernels.front().release) continue;
            protected_packages.insert(kernels[i].package);
            break;
        }
    }

    VLOG(verbose, "Kernel autoremove protection active for "
        << protected_packages.size() << " package(s)"
        << (running_release.empty() ? "" : " (running release: " + running_release + ")")
        << (fallback_release.empty() ? "" : ", fallback release: " + fallback_release));
    return protected_packages;
}

struct RepairInspection {
    std::vector<std::string> detected_issues;
    std::vector<std::string> unresolved_issues;
    std::vector<std::string> missing_repo_packages;
    std::vector<std::string> missing_upgradeable_base_packages;
    std::vector<std::string> missing_provided_base_packages;
    std::vector<PackageMetadata> install_queue;
    std::vector<PackageMetadata> reinstall_queue;
};

void append_unique_name(std::vector<std::string>& names, const std::string& name) {
    if (std::find(names.begin(), names.end(), name) == names.end()) {
        names.push_back(name);
    }
}

std::string describe_missing_repair_candidate(const std::string& pkg_name) {
    if (is_upgradeable_system_package(pkg_name)) {
        return pkg_name + ": automatic reinstall is not possible because no repository package is available"
            " (this is an upgradeable base runtime; make it available from Debian testing or an S2 repo,"
            " then rerun 'gpkg repair' or 'gpkg upgrade')";
    }

    if (is_system_provided(pkg_name)) {
        return pkg_name + ": automatic reinstall is not possible because no repository package is available"
            " (GeminiOS considers this base-provided; recover it from the base image or make it available"
            " from Debian testing or an S2 repo if you want repo-driven repair)";
    }

    return pkg_name + ": automatic reinstall is not possible because no repository package is available"
        " (make it available from Debian testing or an S2 repo, then rerun 'gpkg repair')";
}

struct UpgradePlanEntry {
    PackageMetadata meta;
    std::string current_version;
    bool was_installed = false;
    bool reinstall_only = false;
};

std::vector<std::string> parse_companion_tokens(const std::string& raw_value) {
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

void append_builtin_upgrade_companion(
    std::map<std::string, std::vector<std::string>>& companions,
    const std::string& trigger,
    const std::string& companion
) {
    auto& entry = companions[trigger];
    if (std::find(entry.begin(), entry.end(), companion) == entry.end()) {
        entry.push_back(companion);
    }
}

std::map<std::string, std::vector<std::string>> load_upgrade_companions() {
    std::map<std::string, std::vector<std::string>> companions;
    std::ifstream f(UPGRADE_COMPANIONS_PATH);
    if (f) {
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

            auto parsed = parse_companion_tokens(raw_companions);
            auto& entry = companions[trigger];
            std::set<std::string> seen(entry.begin(), entry.end());
            for (const auto& pkg : parsed) {
                if (seen.insert(pkg).second) entry.push_back(pkg);
            }
        }
    }

    // Keep a small built-in floor for lockstep runtime transitions even if the
    // image policy files are stale or missing.
    append_builtin_upgrade_companion(companions, "libc6", "libc-bin");

    return companions;
}

void append_companion_targets(
    std::vector<std::string>& out,
    const std::map<std::string, std::vector<std::string>>& companion_map,
    const std::string& key
) {
    auto it = companion_map.find(key);
    if (it == companion_map.end()) return;

    std::set<std::string> seen(out.begin(), out.end());
    for (const auto& pkg : it->second) {
        if (seen.insert(pkg).second) out.push_back(pkg);
    }
}

bool runtime_companion_looks_non_runtime(const std::string& pkg_name) {
    static const std::set<std::string> exact_non_runtime = {
        "libc-dev-bin",
        "libc-l10n",
        "linux-libc-dev",
        "locales",
        "rpcsvc-proto",
    };
    if (exact_non_runtime.count(pkg_name) != 0) return true;
    if (pkg_name.rfind("manpages", 0) == 0) return true;
    if (pkg_name.size() >= 4 && pkg_name.substr(pkg_name.size() - 4) == "-dev") return true;
    if (pkg_name.find("-dbg") != std::string::npos) return true;
    if (pkg_name.find("-dbgsym") != std::string::npos) return true;
    if (pkg_name.find("-doc") != std::string::npos) return true;
    return false;
}

bool should_auto_import_base_runtime_companion(
    const std::string& pkg_name,
    bool verbose
) {
    if (pkg_name == "libc-bin") return true;
    if (!is_system_provided(pkg_name)) return false;
    if (runtime_companion_looks_non_runtime(pkg_name)) {
        VLOG(verbose, "Skipping base-provided non-runtime companion " << pkg_name);
        return false;
    }
    return true;
}

bool resolve_upgrade_target_metadata(
    const Dependency& requested_dep,
    PackageMetadata& out_meta,
    bool verbose
) {
    std::string requested_name = canonicalize_package_name(requested_dep.name, verbose);
    PackageMetadata exact_meta;
    if (get_repo_package_info(requested_name, exact_meta) &&
        version_satisfies(exact_meta.version, requested_dep.op, requested_dep.version)) {
        out_meta = exact_meta;
        return true;
    }

    std::string provider = find_provider(
        requested_name,
        requested_dep.op,
        requested_dep.version,
        verbose
    );
    if (provider.empty()) return false;

    return get_repo_package_info(provider, out_meta);
}

bool queue_upgrade_target(
    const Dependency& requested_dep,
    const std::map<std::string, std::vector<std::string>>& companion_map,
    std::vector<PackageMetadata>& install_queue,
    std::vector<UpgradePlanEntry>& explicit_targets,
    std::set<std::string>& queued_packages,
    std::set<std::string>& explicit_target_names,
    std::set<std::string>& target_walk,
    std::set<std::string>& dependency_visited,
    const std::set<std::string>& installed_cache,
    bool verbose,
    bool force_reinstall = false
) {
    PackageMetadata meta;
    if (!resolve_upgrade_target_metadata(requested_dep, meta, verbose)) {
        VLOG(verbose, "No repository candidate available for upgrade target " << requested_dep.name);
        return true;
    }

    std::string current_version;
    bool was_installed = is_installed(meta.name, &current_version);
    if (!was_installed) {
        std::string canonical_requested = canonicalize_package_name(requested_dep.name, verbose);
        if (canonical_requested != requested_dep.name) {
            was_installed = is_installed(requested_dep.name, &current_version);
        }
    }
    bool reinstall_only = was_installed && compare_versions(meta.version, current_version) == 0;
    if (was_installed && compare_versions(meta.version, current_version) <= 0 && !force_reinstall) {
        VLOG(verbose, meta.name << " is already up to date (" << current_version << ").");
        return true;
    }

    if (!target_walk.insert(meta.name).second) {
        VLOG(verbose, "Skipping recursive upgrade companion cycle for " << meta.name);
        return true;
    }

    std::vector<std::string> companions;
    append_companion_targets(companions, companion_map, requested_dep.name);
    if (meta.name != requested_dep.name) {
        append_companion_targets(companions, companion_map, meta.name);
    }
    for (const auto& companion_name : companions) {
        Dependency companion_dep = parse_dependency(companion_name);
        if (!queue_upgrade_target(
                companion_dep,
                companion_map,
                install_queue,
                explicit_targets,
                queued_packages,
                explicit_target_names,
                target_walk,
                dependency_visited,
                installed_cache,
                verbose,
                force_reinstall
            )) {
            target_walk.erase(meta.name);
            return false;
        }
    }

    for (const auto& dep_str : collect_transaction_dependency_edges(meta)) {
        Dependency dep = parse_dependency(dep_str);
        if (!resolve_dependencies(
                dep.name,
                dep.op,
                dep.version,
                install_queue,
                dependency_visited,
                installed_cache,
                verbose
            )) {
            target_walk.erase(meta.name);
            return false;
        }
    }

    if (queued_packages.insert(meta.name).second) {
        install_queue.push_back(meta);
    }

    if (explicit_target_names.insert(meta.name).second) {
        explicit_targets.push_back({meta, current_version, was_installed, reinstall_only});
    }

    target_walk.erase(meta.name);
    return true;
}

bool expand_runtime_upgrade_companions(
    std::vector<PackageMetadata>& install_queue,
    const std::set<std::string>& installed_cache,
    bool verbose
) {
    if (install_queue.empty()) return true;

    auto companion_map = load_upgrade_companions();
    if (companion_map.empty()) return true;

    std::set<std::string> queued_packages;
    std::set<std::string> explicit_target_names;
    std::set<std::string> target_walk;
    std::set<std::string> dependency_visited;
    std::vector<UpgradePlanEntry> ignored_explicit_targets;

    for (const auto& pkg : install_queue) {
        std::string canonical_name = canonicalize_package_name(pkg.name, verbose);
        queued_packages.insert(canonical_name);
        dependency_visited.insert(canonical_name);
    }

    std::set<std::string> expanded_roots;
    for (size_t index = 0; index < install_queue.size(); ++index) {
        std::string root_name = canonicalize_package_name(install_queue[index].name, verbose);
        if (!expanded_roots.insert(root_name).second) continue;

        auto it = companion_map.find(root_name);
        if (it == companion_map.end() || it->second.empty()) continue;

        VLOG(verbose, "Expanding runtime upgrade companions for " << root_name
                     << ": " << join_strings(it->second));
        for (const auto& companion_name : it->second) {
            Dependency companion_dep = parse_dependency(companion_name);
            std::string canonical_companion = canonicalize_package_name(companion_dep.name, verbose);
            bool companion_already_relevant =
                queued_packages.count(canonical_companion) != 0 ||
                installed_cache.count(canonical_companion) != 0 ||
                should_auto_import_base_runtime_companion(canonical_companion, verbose);
            if (!companion_already_relevant) {
                VLOG(verbose, "Skipping dormant runtime upgrade companion "
                             << canonical_companion << " for " << root_name);
                continue;
            }
            if (!queue_upgrade_target(
                    companion_dep,
                    companion_map,
                    install_queue,
                    ignored_explicit_targets,
                    queued_packages,
                    explicit_target_names,
                    target_walk,
                    dependency_visited,
                    installed_cache,
                    verbose,
                    g_force_reinstall
                )) {
                return false;
            }
        }
    }

    return true;
}

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
            for (const auto& dep_str : collect_transaction_dependency_edges(installed_meta)) {
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
            append_unique_message(inspection.unresolved_issues, describe_missing_repair_candidate(pkg));
            append_unique_name(inspection.missing_repo_packages, pkg);
            if (is_upgradeable_system_package(pkg)) {
                append_unique_name(inspection.missing_upgradeable_base_packages, pkg);
            } else if (is_system_provided(pkg)) {
                append_unique_name(inspection.missing_provided_base_packages, pkg);
            }
            continue;
        }

        inspection.reinstall_queue.push_back(repo_meta);
    }

    return inspection;
}

int handle_upgrade(const std::set<std::string>& installed_cache, bool verbose) {
    if (!ensure_repo_index_available()) return 1;

    std::cout << "Reading package lists..." << std::endl;
    if (!ensure_repo_package_cache_loaded(verbose)) return 1;
    std::cout << "Optional dependency policy: " << describe_optional_dependency_policy() << std::endl;
    VLOG(verbose, "Checking " << installed_cache.size() << " installed packages and upgradeable base runtimes.");

    std::vector<PackageMetadata> upgrade_queue;
    std::vector<UpgradePlanEntry> explicit_targets;
    std::set<std::string> queued_packages;
    std::set<std::string> explicit_target_names;
    std::set<std::string> target_walk;
    std::set<std::string> dependency_visited;
    auto upgradeable_system = load_upgradeable_system_packages();
    auto companion_map = load_upgrade_companions();

    for (const auto& entry : upgradeable_system) {
        Dependency dep = parse_dependency(entry);
        if (!queue_upgrade_target(
                dep,
                companion_map,
                upgrade_queue,
                explicit_targets,
                queued_packages,
                explicit_target_names,
                target_walk,
                dependency_visited,
                installed_cache,
                verbose,
                g_force_reinstall
            )) {
            return 1;
        }
    }

    for (const auto& pkg : installed_cache) {
        std::string current_ver;
        if (!is_installed(pkg, &current_ver)) continue;

        PackageMetadata repo_meta;
        if (!get_repo_package_info(pkg, repo_meta)) continue;
        if (!g_force_reinstall && compare_versions(repo_meta.version, current_ver) <= 0) continue;

        if (compare_versions(repo_meta.version, current_ver) > 0) {
            VLOG(verbose, "Update found for " << pkg << ": " << current_ver << " -> " << repo_meta.version);
        } else if (g_force_reinstall) {
            VLOG(verbose, "Reinstall requested for " << pkg << " at " << current_ver);
        }
        Dependency dep{pkg, "", ""};
        if (!queue_upgrade_target(
                dep,
                companion_map,
                upgrade_queue,
                explicit_targets,
                queued_packages,
                explicit_target_names,
                target_walk,
                dependency_visited,
                installed_cache,
                verbose,
                g_force_reinstall
            )) {
            return 1;
        }
    }

    if (upgrade_queue.empty()) {
        std::cout << "All packages are up to date." << std::endl;
        return 0;
    }

    TransactionPlan upgrade_plan;
    if (!build_transaction_plan(upgrade_queue, installed_cache, verbose, upgrade_plan)) return 1;
    upgrade_queue = upgrade_plan.install_queue;

    std::set<std::string> planned_names;
    for (const auto& pkg : upgrade_queue) planned_names.insert(pkg.name);

    std::vector<UpgradePlanEntry> installed_upgrades;
    std::vector<UpgradePlanEntry> installed_reinstalls;
    std::vector<UpgradePlanEntry> base_bootstraps;
    for (const auto& entry : explicit_targets) {
        if (!planned_names.count(entry.meta.name)) continue;
        if (entry.was_installed && entry.reinstall_only) installed_reinstalls.push_back(entry);
        else if (entry.was_installed) installed_upgrades.push_back(entry);
        else base_bootstraps.push_back(entry);
    }

    if (!installed_upgrades.empty()) {
        std::cout << "The following packages will be upgraded:" << std::endl;
        for (const auto& entry : installed_upgrades) {
            std::cout << "  " << Color::GREEN << entry.meta.name << Color::RESET
                      << " (" << entry.current_version << " -> " << entry.meta.version << ")" << std::endl;
        }
    }

    if (!installed_reinstalls.empty()) {
        std::cout << "The following packages will be reinstalled:" << std::endl;
        for (const auto& entry : installed_reinstalls) {
            std::cout << "  " << Color::BLUE << entry.meta.name << Color::RESET
                      << " (" << entry.meta.version << ")" << std::endl;
        }
    }

    if (!base_bootstraps.empty()) {
        std::cout << "The following base packages will be imported into gpkg and upgraded:" << std::endl;
        for (const auto& entry : base_bootstraps) {
            std::cout << "  " << Color::GREEN << entry.meta.name << Color::RESET
                      << " (" << entry.meta.version << ")" << std::endl;
        }
    }

    std::vector<PackageMetadata> dependency_installs;
    for (const auto& pkg : upgrade_queue) {
        if (!explicit_target_names.count(pkg.name)) dependency_installs.push_back(pkg);
    }
    if (!dependency_installs.empty()) {
        std::cout << "Additional dependency packages will be installed:" << std::endl;
        for (const auto& pkg : dependency_installs) {
            std::cout << "  " << Color::GREEN << pkg.name << Color::RESET
                      << " (" << pkg.version << ")" << std::endl;
        }
    }

    if (!upgrade_plan.retirements.empty()) {
        std::cout << "The following installed packages will be retired as replacements:" << std::endl;
        for (const auto& entry : upgrade_plan.retirements) {
            std::cout << "  " << Color::YELLOW << entry.installed_name << Color::RESET
                      << " -> " << Color::GREEN << entry.replacement_name << Color::RESET << std::endl;
        }
    }

    if (!ask_confirmation("Do you want to continue?")) return 0;

    std::cout << Color::CYAN << "[*] Downloading "
              << upgrade_queue.size() << " package(s)..." << Color::RESET << std::endl;
    DownloadBatchReport download_report = download_package_archives(
        upgrade_queue,
        verbose,
        MAX_PARALLEL_PACKAGE_DOWNLOADS
    );
    std::cout << Color::CYAN << "[*] Download summary: "
              << download_report.downloaded_count << " downloaded, "
              << download_report.reused_count << " reused from cache, "
              << format_total_bytes(download_report.downloaded_bytes) << " transferred."
              << Color::RESET << std::endl;

    std::vector<std::string> failed_preparation;
    if (!prepare_install_archives(upgrade_queue, download_report, verbose, failed_preparation)) {
        std::cerr << Color::RED << "E: Aborting upgrade because these packages could not be prepared safely: "
                  << join_strings(failed_preparation) << Color::RESET << std::endl;
        return 1;
    }

    size_t installed_count = 0;
    std::vector<std::string> failures;
    size_t install_progress_width = 0;
    std::cout << Color::CYAN << "[*] Installing " << upgrade_queue.size()
              << " package(s)..." << Color::RESET << std::endl;
    for (size_t i = 0; i < upgrade_queue.size(); ++i) {
        if (!download_report.results[i].success) {
            failures.push_back(upgrade_queue[i].name);
            continue;
        }

        if (!verbose) render_package_progress("current", i, upgrade_queue.size(), upgrade_queue[i].name, &install_progress_width);
        InstallCommandResult result = install_package_v2(upgrade_queue[i], verbose);
        if (!result.success) {
            if (!verbose) finish_progress_line(&install_progress_width);
            std::cerr << Color::RED << "E: Failed to install " << upgrade_queue[i].name
                      << Color::RESET;
            if (!verbose && !result.log_path.empty()) {
                std::cerr << " (see " << result.log_path << ")";
            }
            std::cerr << std::endl;
            failures.push_back(upgrade_queue[i].name);
            break;
        }

        queue_triggers_for_package(upgrade_queue[i].name);
        std::vector<std::string> retirements;
        if (should_retire_after_install(upgrade_plan, upgrade_queue[i].name, retirements)) {
            for (const auto& retired_pkg : retirements) {
                InstallCommandResult retire_result = retire_package_by_name(retired_pkg, verbose);
                if (!retire_result.success) {
                    if (!verbose) finish_progress_line(&install_progress_width);
                    std::cerr << Color::RED << "E: Failed to retire replaced package " << retired_pkg
                              << Color::RESET;
                    if (!verbose && !retire_result.log_path.empty()) {
                        std::cerr << " (see " << retire_result.log_path << ")";
                    }
                    std::cerr << std::endl;
                    failures.push_back(retired_pkg);
                    break;
                }
            }
            if (!failures.empty()) break;
        }
        ++installed_count;
        if (!verbose) render_package_progress("current", i + 1, upgrade_queue.size(), upgrade_queue[i].name, &install_progress_width);
    }
    if (!verbose) {
        finish_progress_line(&install_progress_width);
        std::cout << Color::GREEN << "✓ Installed " << installed_count << "/" << upgrade_queue.size()
                  << " package(s)." << Color::RESET << std::endl;
    }

    if (!failures.empty()) {
        std::cout << Color::CYAN << "Upgrade summary: "
                  << installed_upgrades.size() << " upgraded, "
                  << installed_reinstalls.size() << " reinstalled, "
                  << base_bootstraps.size() << " imported from base image, "
                  << dependency_installs.size() << " dependency installs, "
                  << download_report.downloaded_count << " downloaded, "
                  << download_report.reused_count << " reused from cache, "
                  << format_total_bytes(download_report.downloaded_bytes) << " transferred."
                  << Color::RESET << std::endl;
        std::cerr << Color::RED << "E: Upgrade completed with failures: "
                  << join_strings(failures) << Color::RESET << std::endl;
        return 1;
    }

    std::cout << Color::CYAN << "Upgrade summary: "
              << installed_upgrades.size() << " upgraded, "
              << installed_reinstalls.size() << " reinstalled, "
              << base_bootstraps.size() << " imported from base image, "
              << dependency_installs.size() << " dependency installs, "
              << download_report.downloaded_count << " downloaded, "
              << download_report.reused_count << " reused from cache, "
              << format_total_bytes(download_report.downloaded_bytes) << " transferred."
              << Color::RESET << std::endl;

    return 0;
}

int handle_repair(bool verbose) {
    if (!ensure_repo_index_available()) return 1;

    std::cout << "Inspecting installed packages..." << std::endl;
    std::cout << "Optional dependency policy: " << describe_optional_dependency_policy() << std::endl;
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
        if (!inspection.missing_repo_packages.empty()) {
            std::cerr << Color::YELLOW
                      << "  Missing repo candidates: " << join_strings(inspection.missing_repo_packages)
                      << Color::RESET << std::endl;
        }
        if (!inspection.missing_upgradeable_base_packages.empty()) {
            std::cerr << Color::YELLOW
                      << "  Upgradeable base runtimes to republish: "
                      << join_strings(inspection.missing_upgradeable_base_packages)
                      << Color::RESET << std::endl;
        }
        if (!inspection.missing_provided_base_packages.empty()) {
            std::cerr << Color::YELLOW
                      << "  Base-provided runtimes needing manual/base recovery: "
                      << join_strings(inspection.missing_provided_base_packages)
                      << Color::RESET << std::endl;
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
    if (!expand_runtime_upgrade_companions(repair_queue, installed_set, verbose)) return 1;
    TransactionPlan repair_plan;
    if (!build_transaction_plan(repair_queue, installed_set, verbose, repair_plan)) return 1;
    repair_queue = repair_plan.install_queue;

    if (!repair_plan.retirements.empty()) {
        std::cout << "The following installed packages will be retired as replacements:" << std::endl;
        for (const auto& entry : repair_plan.retirements) {
            std::cout << "  " << Color::YELLOW << entry.installed_name << Color::RESET
                      << " -> " << Color::GREEN << entry.replacement_name << Color::RESET << std::endl;
        }
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

    std::vector<std::string> failed_preparation;
    if (!prepare_install_archives(repair_queue, download_report, verbose, failed_preparation)) {
        std::cerr << Color::RED << "E: Aborting repair because these packages could not be prepared safely: "
                  << join_strings(failed_preparation) << Color::RESET << std::endl;
        return 1;
    }

    std::cout << Color::CYAN << "[*] Applying repair plan..." << Color::RESET << std::endl;
    size_t repaired_count = 0;
    size_t install_progress_width = 0;
    std::vector<std::string> failures;
    for (size_t i = 0; i < repair_queue.size(); ++i) {
        if (!verbose) render_package_progress("current", i, repair_queue.size(), repair_queue[i].name, &install_progress_width);
        InstallCommandResult result = install_package_v2(repair_queue[i], verbose);
        if (!result.success) {
            if (!verbose) finish_progress_line(&install_progress_width);
            std::cerr << Color::RED << "E: Repair stopped at " << repair_queue[i].name
                      << Color::RESET;
            if (!verbose && !result.log_path.empty()) {
                std::cerr << " (see " << result.log_path << ")";
            }
            std::cerr << std::endl;
            failures.push_back(repair_queue[i].name);
            break;
        }
        queue_triggers_for_package(repair_queue[i].name);
        std::vector<std::string> retirements;
        if (should_retire_after_install(repair_plan, repair_queue[i].name, retirements)) {
            for (const auto& retired_pkg : retirements) {
                InstallCommandResult retire_result = retire_package_by_name(retired_pkg, verbose);
                if (!retire_result.success) {
                    if (!verbose) finish_progress_line(&install_progress_width);
                    std::cerr << Color::RED << "E: Failed to retire replaced package " << retired_pkg
                              << Color::RESET;
                    if (!verbose && !retire_result.log_path.empty()) {
                        std::cerr << " (see " << retire_result.log_path << ")";
                    }
                    std::cerr << std::endl;
                    failures.push_back(retired_pkg);
                    break;
                }
            }
            if (!failures.empty()) break;
        }
        ++repaired_count;
        if (!verbose) render_package_progress("current", i + 1, repair_queue.size(), repair_queue[i].name, &install_progress_width);
    }
    if (!verbose) finish_progress_line(&install_progress_width);

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
    std::vector<std::string> operands = collect_cli_operands(argc, argv, 2);
    std::set<std::string> visited;
    bool needs_repo_index = false;

    if (operands.empty()) {
        std::cerr << "Usage: gpkg install <package_name> [options]" << std::endl;
        return 1;
    }

    std::cout << "Resolving dependencies..." << std::endl;
    std::cout << "Optional dependency policy: " << describe_optional_dependency_policy() << std::endl;
    for (const auto& arg : operands) {
        if (arg.length() > 5 && arg.substr(arg.length() - 5) == ".gpkg" && access(arg.c_str(), F_OK) == 0) {
            local_files.push_back(arg);
        } else {
            needs_repo_index = true;
        }
    }

    if (needs_repo_index && !ensure_repo_index_available()) return 1;

    for (const auto& arg : operands) {
        if (arg.length() > 5 && arg.substr(arg.length() - 5) == ".gpkg" && access(arg.c_str(), F_OK) == 0) {
            continue;
        }

        if (!resolve_dependencies(arg, "", "", install_queue, visited, installed_cache, verbose, g_force_reinstall)) {
            std::cerr << Color::RED << "E: Failed to resolve dependencies for " << arg
                      << Color::RESET << std::endl;
            return 1;
        }
    }

    if (!expand_runtime_upgrade_companions(install_queue, installed_cache, verbose)) {
        std::cerr << Color::RED << "E: Failed to expand runtime upgrade companion packages."
                  << Color::RESET << std::endl;
        return 1;
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

        std::string local_pkg_name = read_package_name_from_archive(local_file);
        if (!local_pkg_name.empty()) {
            queue_triggers_for_package(local_pkg_name);

            PackageMetadata local_meta;
            if (get_installed_package_metadata(local_pkg_name, local_meta)) {
                std::vector<PackageMetadata> local_queue = {local_meta};
                std::vector<std::string> registered = get_registered_package_names();
                std::set<std::string> installed_now(registered.begin(), registered.end());
                TransactionPlan local_plan;
                if (!build_transaction_plan(local_queue, installed_now, verbose, local_plan)) return 1;

                std::vector<std::string> retirements;
                if (should_retire_after_install(local_plan, local_pkg_name, retirements)) {
                    for (const auto& retired_pkg : retirements) {
                        InstallCommandResult retire_result = retire_package_by_name(retired_pkg, verbose);
                        if (!retire_result.success) {
                            std::cerr << Color::RED << "E: Failed to retire replaced package "
                                      << retired_pkg << Color::RESET;
                            if (!verbose && !retire_result.log_path.empty()) {
                                std::cerr << " See " << retire_result.log_path << " for details.";
                            }
                            std::cerr << std::endl;
                            return 1;
                        }
                    }
                }
            }
        }
    }

    TransactionPlan install_plan;
    if (!build_transaction_plan(install_queue, installed_cache, verbose, install_plan)) return 1;
    install_queue = install_plan.install_queue;

    if (install_queue.empty()) {
        if (local_files.empty()) std::cout << "Nothing to do." << std::endl;
        return 0;
    }

    std::vector<PackageMetadata> new_installs;
    std::vector<std::pair<PackageMetadata, std::string>> reinstalls;
    std::vector<std::pair<PackageMetadata, std::string>> upgrades;
    for (const auto& pkg : install_queue) {
        std::string current_version;
        PlannedPackageKind kind = classify_planned_package(pkg, &current_version);
        if (kind == PlannedPackageKind::NewInstall) new_installs.push_back(pkg);
        else if (kind == PlannedPackageKind::Reinstall) reinstalls.push_back({pkg, current_version});
        else upgrades.push_back({pkg, current_version});
    }

    if (!new_installs.empty()) {
        std::cout << "The following packages will be installed:" << std::endl;
        for (const auto& pkg : new_installs) {
            std::cout << "  " << Color::GREEN << pkg.name << Color::RESET
                      << " (" << pkg.version << ")" << std::endl;
        }
    }
    if (!upgrades.empty()) {
        std::cout << "The following packages will be upgraded:" << std::endl;
        for (const auto& entry : upgrades) {
            std::cout << "  " << Color::GREEN << entry.first.name << Color::RESET
                      << " (" << entry.second << " -> " << entry.first.version << ")" << std::endl;
        }
    }
    if (!reinstalls.empty()) {
        std::cout << "The following packages will be reinstalled:" << std::endl;
        for (const auto& entry : reinstalls) {
            std::cout << "  " << Color::BLUE << entry.first.name << Color::RESET
                      << " (" << entry.first.version << ")" << std::endl;
        }
    }

    if (!install_plan.retirements.empty()) {
        std::cout << "The following installed packages will be retired as replacements:" << std::endl;
        for (const auto& entry : install_plan.retirements) {
            std::cout << "  " << Color::YELLOW << entry.installed_name << Color::RESET
                      << " -> " << Color::GREEN << entry.replacement_name << Color::RESET << std::endl;
        }
    }

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

    std::vector<std::string> failed_preparation;
    if (!prepare_install_archives(install_queue, download_report, verbose, failed_preparation)) {
        std::cerr << Color::RED << "E: Aborting install because these packages could not be prepared safely: "
                  << join_strings(failed_preparation) << Color::RESET << std::endl;
        return 1;
    }

    std::cout << Color::CYAN << "[*] Installing " << install_queue.size()
              << " package(s)..." << Color::RESET << std::endl;
    size_t installed_count = 0;
    size_t install_progress_width = 0;
    for (size_t i = 0; i < install_queue.size(); ++i) {
        if (!verbose) render_package_progress("current", i, install_queue.size(), install_queue[i].name, &install_progress_width);
        InstallCommandResult result = install_package_v2(install_queue[i], verbose);
        if (!result.success) {
            if (!verbose) finish_progress_line(&install_progress_width);
            std::cerr << Color::RED << "E: Installation stopped at " << install_queue[i].name
                      << ". " << installed_count << " package(s) were installed before the failure."
                      << Color::RESET;
            if (!verbose && !result.log_path.empty()) {
                std::cerr << " See " << result.log_path << " for details.";
            }
            std::cerr << std::endl;
            return 1;
        }
        queue_triggers_for_package(install_queue[i].name);
        std::vector<std::string> retirements;
        if (should_retire_after_install(install_plan, install_queue[i].name, retirements)) {
            for (const auto& retired_pkg : retirements) {
                InstallCommandResult retire_result = retire_package_by_name(retired_pkg, verbose);
                if (!retire_result.success) {
                    if (!verbose) finish_progress_line(&install_progress_width);
                    std::cerr << Color::RED << "E: Failed to retire replaced package " << retired_pkg
                              << Color::RESET;
                    if (!verbose && !retire_result.log_path.empty()) {
                        std::cerr << " See " << retire_result.log_path << " for details.";
                    }
                    std::cerr << std::endl;
                    return 1;
                }
            }
        }
        ++installed_count;
        if (!verbose) render_package_progress("current", i + 1, install_queue.size(), install_queue[i].name, &install_progress_width);
    }
    if (!verbose) finish_progress_line(&install_progress_width);

    std::cout << Color::GREEN << "✓ Installed " << installed_count << " package(s)." << Color::RESET << std::endl;
    return 0;
}

int handle_remove(int argc, char* argv[], bool verbose, bool purge, bool autoremove) {
    if (argc < 3) {
        std::cerr << "Usage: gpkg remove <package_name> [--purge] [--autoremove]" << std::endl;
        return 1;
    }

    std::vector<std::string> operands = collect_cli_operands(argc, argv, 2);
    if (operands.empty()) {
        std::cerr << "E: No package specified for removal." << std::endl;
        return 1;
    }
    if (operands.size() > 1) {
        std::cerr << "E: gpkg remove currently supports one package at a time." << std::endl;
        return 1;
    }
    std::string target_pkg = operands.front();

    bool target_installed = is_installed(target_pkg);
    bool target_config_files = package_is_config_files_only(target_pkg);

    std::string protection_reason;
    if (!target_installed && !(purge && target_config_files) &&
        package_is_base_system_provided(target_pkg, &protection_reason)) {
        std::cerr << Color::RED << "E: Refusing to remove '" << target_pkg << "' because "
                  << protection_reason << "."
                  << Color::RESET << std::endl;
        return 1;
    }

    if (!target_installed && !(purge && target_config_files)) {
        std::cerr << Color::RED << "E: Package '" << target_pkg << "' is not installed."
                  << Color::RESET << std::endl;
        return 1;
    }

    if (package_is_removal_protected(target_pkg, &protection_reason)) {
        std::cerr << Color::RED << "E: Refusing to remove '" << target_pkg << "' because "
                  << protection_reason << "."
                  << Color::RESET << std::endl;
        return 1;
    }

    std::vector<std::string> to_remove;
    std::set<std::string> removal_set;
    if (target_installed) {
        to_remove.push_back(target_pkg);
        removal_set.insert(target_pkg);
    }

    if (autoremove && target_installed) {
        std::cout << "Calculating newly unneeded dependencies..." << std::endl;
        std::set<std::string> protected_kernel_packages = get_autoremove_protected_kernel_packages(verbose);
        std::set<std::string> skipped_kernel_packages;
        std::map<std::string, std::string> skipped_protected_packages;
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
                    std::string dep_protection_reason;
                    if (package_is_removal_protected(dep.name, &dep_protection_reason)) {
                        skipped_protected_packages[dep.name] = dep_protection_reason;
                        VLOG(verbose, "Keeping protected system package out of autoremove: " << dep.name);
                        continue;
                    }
                    if (protected_kernel_packages.count(dep.name) != 0) {
                        skipped_kernel_packages.insert(dep.name);
                        VLOG(verbose, "Keeping protected kernel package out of autoremove: " << dep.name);
                        continue;
                    }
                    if (is_required_by_others(dep.name, removal_set, verbose)) continue;

                    to_remove.push_back(dep.name);
                    removal_set.insert(dep.name);
                    changed = true;
                }
            }
        }

        if (!skipped_kernel_packages.empty()) {
            std::cout << "Keeping protected kernel package(s):";
            for (const auto& pkg : skipped_kernel_packages) {
                std::cout << " " << pkg;
            }
            std::cout << std::endl;
        }
        if (!skipped_protected_packages.empty()) {
            std::cout << "Keeping protected system package(s):" << std::endl;
            for (const auto& entry : skipped_protected_packages) {
                std::cout << "  " << entry.first << " (" << entry.second << ")" << std::endl;
            }
        }
    }

    if (!to_remove.empty()) sort_removal_queue_for_operation(to_remove, verbose);

    std::vector<std::string> to_purge;
    std::set<std::string> purge_set;
    if (purge) {
        if (target_config_files && purge_set.insert(target_pkg).second) {
            to_purge.push_back(target_pkg);
        }
        for (const auto& pkg : to_remove) {
            if (purge_set.insert(pkg).second) to_purge.push_back(pkg);
        }
    }

    if (!to_remove.empty()) {
        std::cout << "The following packages will be REMOVED:" << std::endl;
        for (const auto& pkg : to_remove) {
            std::cout << "  " << Color::RED << pkg << Color::RESET << std::endl;
        }
    }
    if (!to_purge.empty()) {
        std::cout << "The following packages will be PURGED:" << std::endl;
        for (const auto& pkg : to_purge) {
            std::cout << "  " << Color::MAGENTA << pkg << Color::RESET << std::endl;
        }
    }

    if (!ask_confirmation("Do you want to continue?")) {
        std::cout << "Abort." << std::endl;
        return 0;
    }

    if (!to_remove.empty()) {
        std::cout << Color::CYAN << "[*] Removing " << to_remove.size()
                  << " package(s)..." << Color::RESET << std::endl;
        size_t remove_progress_width = 0;

        for (size_t i = 0; i < to_remove.size(); ++i) {
            const auto& pkg = to_remove[i];
            if (!verbose) render_package_progress("current", i, to_remove.size(), pkg, &remove_progress_width);
            std::vector<std::string> removed_files = read_installed_file_list(pkg);
            InstallCommandResult result = remove_package_by_name(pkg, verbose);
            if (!result.success) {
                if (!verbose) finish_progress_line(&remove_progress_width);
                std::cerr << Color::RED << "E: Failed to remove " << pkg << Color::RESET << std::endl;
                if (!verbose && !result.log_path.empty()) {
                    std::cerr << " See " << result.log_path << " for details.";
                }
                std::cerr << std::endl;
                return 1;
            }
            check_triggers(removed_files);
            if (!verbose) render_package_progress("current", i + 1, to_remove.size(), pkg, &remove_progress_width);
        }

        if (!verbose) finish_progress_line(&remove_progress_width);
    }

    if (!to_purge.empty()) {
        std::cout << Color::CYAN << "[*] Purging " << to_purge.size()
                  << " package(s)..." << Color::RESET << std::endl;
        size_t purge_progress_width = 0;

        for (size_t i = 0; i < to_purge.size(); ++i) {
            const auto& pkg = to_purge[i];
            if (!verbose) render_package_progress("current", i, to_purge.size(), pkg, &purge_progress_width);
            InstallCommandResult result = purge_package_by_name(pkg, verbose);
            if (!result.success) {
                if (!verbose) finish_progress_line(&purge_progress_width);
                std::cerr << Color::RED << "E: Failed to purge " << pkg << Color::RESET << std::endl;
                if (!verbose && !result.log_path.empty()) {
                    std::cerr << " See " << result.log_path << " for details.";
                }
                std::cerr << std::endl;
                return 1;
            }
            if (!verbose) render_package_progress("current", i + 1, to_purge.size(), pkg, &purge_progress_width);
        }

        if (!verbose) finish_progress_line(&purge_progress_width);
    }

    return 0;
}
