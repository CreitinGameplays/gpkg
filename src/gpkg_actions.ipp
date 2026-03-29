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

bool get_local_installed_package_version(
    const std::string& pkg_name,
    std::string* version_out,
    UpgradeContext* context
) {
    if (version_out) version_out->clear();
    if (pkg_name.empty()) return false;

    if (context) {
        if (context->registered_package_set.count(pkg_name) != 0) {
            auto status_it = context->registered_status_by_package.find(pkg_name);
            if (status_it != context->registered_status_by_package.end() &&
                !package_status_is_installed_like(status_it->second.status)) {
                return false;
            }
            if (!version_out) return true;

            if (status_it != context->registered_status_by_package.end() &&
                !status_it->second.version.empty()) {
                *version_out = status_it->second.version;
                return true;
            }

            auto cached_it = context->registered_version_cache.find(pkg_name);
            if (cached_it != context->registered_version_cache.end()) {
                *version_out = cached_it->second;
                return true;
            }
            if (context->missing_registered_versions.count(pkg_name) != 0) return false;

            std::string info_path = INFO_DIR + pkg_name + ".json";
            std::ifstream f(info_path);
            if (!f) {
                context->missing_registered_versions.insert(pkg_name);
                return false;
            }

            std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
            std::string parsed_version;
            if (!get_json_value(content, "version", parsed_version) || parsed_version.empty()) {
                context->missing_registered_versions.insert(pkg_name);
                return false;
            }

            context->registered_version_cache[pkg_name] = parsed_version;
            *version_out = parsed_version;
            return true;
        }

        auto dpkg_it = context->dpkg_status_by_package.find(pkg_name);
        if (dpkg_it != context->dpkg_status_by_package.end() &&
            package_status_is_installed_like(dpkg_it->second.status)) {
            if (version_out) *version_out = dpkg_it->second.version;
            return true;
        }

        auto base_it = context->base_status_by_package.find(pkg_name);
        if (base_it != context->base_status_by_package.end() &&
            package_status_is_installed_like(base_it->second.status) &&
            !base_it->second.version.empty()) {
            if (version_out) *version_out = base_it->second.version;
            return true;
        }

        return false;
    }

    if (is_installed(pkg_name, version_out)) return true;

    PackageStatusRecord dpkg_record;
    if (get_dpkg_package_status_record(pkg_name, &dpkg_record) &&
        package_status_is_installed_like(dpkg_record.status)) {
        if (version_out) *version_out = dpkg_record.version;
        return true;
    }

    PackageStatusRecord base_record;
    if (!get_base_system_package_status_record(pkg_name, &base_record)) return false;
    if (!package_status_is_installed_like(base_record.status) || base_record.version.empty()) return false;

    if (version_out) *version_out = base_record.version;
    return true;
}

bool package_has_exact_live_install_state(
    const std::string& pkg_name,
    std::string* version_out,
    UpgradeContext* context
) {
    if (version_out) version_out->clear();
    if (pkg_name.empty()) return false;

    if (context) {
        if (context->registered_package_set.count(pkg_name) != 0) {
            return get_local_installed_package_version(pkg_name, version_out, context);
        }

        auto dpkg_it = context->dpkg_status_by_package.find(pkg_name);
        if (dpkg_it != context->dpkg_status_by_package.end() &&
            package_status_is_installed_like(dpkg_it->second.status)) {
            if (version_out) *version_out = dpkg_it->second.version;
            return true;
        }

        return false;
    }

    if (is_installed(pkg_name, version_out)) return true;

    PackageStatusRecord dpkg_record;
    if (get_dpkg_package_status_record(pkg_name, &dpkg_record) &&
        package_status_is_installed_like(dpkg_record.status)) {
        if (version_out) *version_out = dpkg_record.version;
        return true;
    }

    return false;
}

std::vector<std::string> collect_registered_package_names_from_status_records(
    const std::vector<PackageStatusRecord>& status_records
) {
    std::set<std::string> package_names;
    std::map<std::string, std::string> status_by_package;
    for (const auto& record : status_records) {
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

UpgradeContext build_upgrade_context(bool verbose) {
    (void)verbose;

    UpgradeContext context;
    context.registered_status_records = load_package_status_records();
    for (const auto& record : context.registered_status_records) {
        if (record.package.empty()) continue;
        context.registered_status_by_package[record.package] = record;
    }

    context.registered_package_names =
        collect_registered_package_names_from_status_records(context.registered_status_records);
    context.registered_package_set.insert(
        context.registered_package_names.begin(),
        context.registered_package_names.end()
    );
    context.exact_live_packages.insert(
        context.registered_package_names.begin(),
        context.registered_package_names.end()
    );

    context.dpkg_status_records = load_dpkg_package_status_records();
    for (const auto& record : context.dpkg_status_records) {
        if (record.package.empty()) continue;
        context.dpkg_status_by_package[record.package] = record;
        if (!package_status_is_installed_like(record.status)) continue;
        context.exact_live_packages.insert(record.package);
    }

    context.base_entries = load_base_system_registry_entries();
    for (const auto& entry : context.base_entries) {
        if (entry.package.empty()) continue;
        bool present = base_system_registry_entry_looks_present(entry);
        auto existing = context.base_presence_by_package.find(entry.package);
        if (existing == context.base_presence_by_package.end()) {
            context.base_presence_by_package[entry.package] = present;
        } else {
            existing->second = existing->second || present;
        }
        if (!present) continue;

        context.present_base_packages.insert(entry.package);
        PackageStatusRecord record;
        record.package = entry.package;
        record.version = entry.version;
        record.want = "install";
        record.flag = "ok";
        record.status = "installed";
        context.base_status_by_package[entry.package] = record;
    }

    std::string catalog_problem;
    if (load_upgrade_catalog(context.upgrade_catalog, &catalog_problem, verbose)) {
        context.upgrade_catalog_available = true;
        context.upgrade_catalog_problem.clear();
    } else {
        context.upgrade_catalog_available = false;
        context.upgrade_catalog_problem = catalog_problem;
    }

    return context;
}

bool get_context_live_installed_package_metadata(
    UpgradeContext& context,
    const std::string& pkg_name,
    PackageMetadata& out_meta
) {
    auto cache_it = context.live_metadata_cache.find(pkg_name);
    if (cache_it != context.live_metadata_cache.end()) {
        out_meta = cache_it->second;
        return true;
    }
    if (context.missing_live_metadata.count(pkg_name) != 0) return false;

    PackageMetadata meta;
    if (get_installed_package_metadata(pkg_name, meta)) {
        context.live_metadata_cache[pkg_name] = meta;
        out_meta = meta;
        return true;
    }

    std::string installed_version;
    if (!get_local_installed_package_version(pkg_name, &installed_version, &context)) {
        context.missing_live_metadata.insert(pkg_name);
        return false;
    }

    if (get_repo_package_info(pkg_name, meta)) {
        if (!installed_version.empty()) meta.version = installed_version;
        context.live_metadata_cache[pkg_name] = meta;
        out_meta = meta;
        return true;
    }

    meta = {};
    meta.name = pkg_name;
    meta.version = installed_version;
    if (meta.version.empty()) {
        context.missing_live_metadata.insert(pkg_name);
        return false;
    }

    context.live_metadata_cache[pkg_name] = meta;
    out_meta = meta;
    return true;
}

bool resolve_local_or_repo_package_metadata(
    const std::string& pkg_name,
    PackageMetadata& out_meta,
    UpgradeContext* context = nullptr
) {
    if (context && get_context_live_installed_package_metadata(*context, pkg_name, out_meta)) {
        return true;
    }
    if (get_installed_package_metadata(pkg_name, out_meta)) return true;
    return get_repo_package_info(pkg_name, out_meta);
}

bool base_registry_entry_is_shadowed_by_live_package(
    const BaseSystemRegistryEntry& entry,
    const PackageMetadata& entry_repo_meta,
    const std::set<std::string>& exact_live_packages,
    bool verbose
) {
    for (const auto& live_name : exact_live_packages) {
        if (live_name.empty() || live_name == entry.package) continue;

        PackageMetadata live_meta;
        if (!resolve_local_or_repo_package_metadata(live_name, live_meta)) continue;

        Dependency entry_dep{entry.package, "", ""};
        bool live_provides_entry =
            package_metadata_satisfies_dependency(live_name, live_meta, entry_dep);
        bool entry_conflicts_live =
            package_conflicts_with_package(entry_repo_meta, live_name, &live_meta);
        bool live_conflicts_entry =
            package_conflicts_with_package(live_meta, entry.package, &entry_repo_meta);
        bool live_replaces_entry =
            package_replaces_package(live_meta, entry.package, &entry_repo_meta);

        if (!live_provides_entry && !entry_conflicts_live &&
            !live_conflicts_entry && !live_replaces_entry) {
            continue;
        }

        VLOG(verbose, "Skipping stale base-system entry " << entry.package
                     << " because live package " << live_name
                     << " supersedes or conflicts with it.");
        return true;
    }

    return false;
}

std::vector<std::string> collect_upgrade_scan_packages(
    const std::set<std::string>& registered_installed,
    bool verbose
) {
    std::set<std::string> upgrade_scan = registered_installed;
    std::set<std::string> exact_live_packages = registered_installed;

    for (const auto& record : load_dpkg_package_status_records()) {
        if (record.package.empty()) continue;
        if (!package_status_is_installed_like(record.status)) continue;
        exact_live_packages.insert(record.package);
        if (upgrade_scan.count(record.package) != 0) continue;

        if (package_is_base_system_provided(record.package) &&
            !is_upgradeable_system_package(record.package)) {
            VLOG(verbose, "Skipping non-upgradeable base package during upgrade scan: " << record.package);
            continue;
        }

        if (is_blocked_import_package(record.package, verbose)) {
            VLOG(verbose, "Skipping policy-blocked package during upgrade scan: " << record.package);
            continue;
        }

        PackageMetadata repo_meta;
        if (!get_repo_package_info(record.package, repo_meta)) continue;
        upgrade_scan.insert(record.package);
    }

    for (const auto& entry : load_base_system_registry_entries()) {
        if (entry.package.empty()) continue;
        if (upgrade_scan.count(entry.package) != 0) continue;
        if (!base_system_registry_entry_looks_present(entry)) {
            VLOG(verbose, "Skipping stale base-system registry package during upgrade scan: "
                         << entry.package);
            continue;
        }

        if (package_is_base_system_provided(entry.package) &&
            !is_upgradeable_system_package(entry.package)) {
            VLOG(verbose, "Skipping non-upgradeable base package from base-system registry: " << entry.package);
            continue;
        }

        if (is_blocked_import_package(entry.package, verbose)) {
            VLOG(verbose, "Skipping policy-blocked base-system registry package during upgrade scan: " << entry.package);
            continue;
        }

        PackageMetadata repo_meta;
        if (!get_repo_package_info(entry.package, repo_meta)) continue;
        if (!package_has_exact_live_install_state(entry.package) &&
            base_registry_entry_is_shadowed_by_live_package(
                entry,
                repo_meta,
                exact_live_packages,
                verbose)) {
            continue;
        }
        upgrade_scan.insert(entry.package);
    }

    return std::vector<std::string>(upgrade_scan.begin(), upgrade_scan.end());
}

bool package_has_present_base_registry_state(const std::string& pkg_name) {
    if (pkg_name.empty()) return false;

    for (const auto& entry : load_base_system_registry_entries()) {
        if (entry.package != pkg_name) continue;
        return base_system_registry_entry_looks_present(entry);
    }

    return false;
}

bool queued_upgrade_candidate_shadows_base_alias(
    const PackageMetadata& candidate,
    const PackageMetadata& shadowed,
    bool verbose
) {
    if (candidate.name.empty() || shadowed.name.empty()) return false;
    if (candidate.name == shadowed.name) return false;
    if (package_has_exact_live_install_state(shadowed.name)) return false;

    bool shadowed_is_base_backed =
        package_is_base_system_provided(shadowed.name) ||
        package_has_present_base_registry_state(shadowed.name);
    if (!shadowed_is_base_backed) return false;

    Dependency shadowed_dep{shadowed.name, "", ""};
    if (!package_metadata_satisfies_dependency(candidate.name, candidate, shadowed_dep)) {
        return false;
    }

    bool candidate_conflicts_shadowed =
        package_conflicts_with_package(candidate, shadowed.name, &shadowed);
    bool shadowed_conflicts_candidate =
        package_conflicts_with_package(shadowed, candidate.name, &candidate);
    bool candidate_replaces_shadowed =
        package_replaces_package(candidate, shadowed.name, &shadowed);
    if (!candidate_conflicts_shadowed &&
        !shadowed_conflicts_candidate &&
        !candidate_replaces_shadowed) {
        return false;
    }

    Dependency candidate_dep{candidate.name, "", ""};
    bool shadowed_satisfies_candidate =
        package_metadata_satisfies_dependency(shadowed.name, shadowed, candidate_dep);
    bool shadowed_replaces_candidate =
        package_replaces_package(shadowed, candidate.name, &candidate);
    if ((shadowed_satisfies_candidate || shadowed_replaces_candidate) &&
        !candidate_replaces_shadowed) {
        return false;
    }

    VLOG(verbose, "Pruning shadowed upgrade target " << shadowed.name
                 << " in favor of " << candidate.name
                 << " because the former only survives as non-exact base-system state.");
    return true;
}

bool context_package_has_present_base_registry_state(
    const UpgradeContext& context,
    const std::string& pkg_name
) {
    auto it = context.base_presence_by_package.find(pkg_name);
    return it != context.base_presence_by_package.end() && it->second;
}

bool candidate_shadows_requested_alias(
    const std::string& candidate_name,
    const PackageMetadata& candidate_meta,
    const std::string& requested_name,
    const PackageMetadata* requested_meta = nullptr
) {
    if (candidate_name.empty() || requested_name.empty()) return false;
    if (candidate_name == requested_name) return false;

    Dependency requested_dep{requested_name, "", ""};
    bool candidate_satisfies_requested =
        package_metadata_satisfies_dependency(candidate_name, candidate_meta, requested_dep);
    bool candidate_replaces_requested =
        package_replaces_package(candidate_meta, requested_name, requested_meta);
    if (!candidate_satisfies_requested && !candidate_replaces_requested) return false;

    bool candidate_conflicts_requested =
        package_conflicts_with_package(candidate_meta, requested_name, requested_meta);
    bool requested_conflicts_candidate =
        requested_meta &&
        package_conflicts_with_package(*requested_meta, candidate_name, &candidate_meta);
    if (!candidate_conflicts_requested &&
        !requested_conflicts_candidate &&
        !candidate_replaces_requested) {
        return false;
    }

    if (requested_meta) {
        Dependency candidate_dep{candidate_name, "", ""};
        bool requested_satisfies_candidate =
            package_metadata_satisfies_dependency(requested_name, *requested_meta, candidate_dep);
        bool requested_replaces_candidate =
            package_replaces_package(*requested_meta, candidate_name, &candidate_meta);
        if ((requested_satisfies_candidate || requested_replaces_candidate) &&
            !candidate_replaces_requested) {
            return false;
        }
    }

    return true;
}

bool should_prefer_family_candidate(
    const std::string& requested_name,
    const PackageMetadata* requested_meta,
    const std::string& candidate_name,
    const PackageMetadata& candidate_meta,
    const std::string& current_name,
    const PackageMetadata* current_meta
) {
    if (current_name.empty() || !current_meta) return true;

    bool candidate_shadows =
        candidate_shadows_requested_alias(candidate_name, candidate_meta, requested_name, requested_meta);
    bool current_shadows =
        candidate_shadows_requested_alias(current_name, *current_meta, requested_name, requested_meta);
    if (candidate_shadows != current_shadows) return candidate_shadows;

    bool candidate_is_requested = candidate_name == requested_name;
    bool current_is_requested = current_name == requested_name;
    if (candidate_is_requested != current_is_requested) {
        return !current_is_requested;
    }

    return should_prefer_repo_candidate(candidate_meta, *current_meta);
}

std::string find_best_exact_live_family(
    const Dependency& requested_dep,
    UpgradeContext& context,
    bool verbose
) {
    if (package_has_exact_live_install_state(requested_dep.name, nullptr, &context)) {
        return requested_dep.name;
    }

    std::string best_name;
    PackageMetadata best_meta;
    for (const auto& live_name : context.exact_live_packages) {
        if (live_name.empty() || live_name == requested_dep.name) continue;

        PackageMetadata live_meta;
        if (!get_context_live_installed_package_metadata(context, live_name, live_meta)) continue;
        bool satisfies_dependency =
            package_metadata_satisfies_dependency(live_name, live_meta, requested_dep);
        bool replaces_requested =
            package_replaces_package(live_meta, requested_dep.name, nullptr);
        bool conflict_only_shadow =
            dependency_matches_conflicting_exact_live_base_alias(
                requested_dep,
                live_name,
                live_meta,
                &context
            );
        if (!satisfies_dependency &&
            !replaces_requested &&
            !conflict_only_shadow) {
            continue;
        }

        if (best_name.empty() || should_prefer_repo_candidate(live_meta, best_meta)) {
            best_name = live_name;
            best_meta = live_meta;
        }
    }

    if (!best_name.empty()) {
        VLOG(verbose, "Normalized " << requested_dep.name
                     << " to exact live package family " << best_name);
    }
    return best_name;
}

std::string find_best_present_base_family(
    const Dependency& requested_dep,
    UpgradeContext& context,
    bool verbose
) {
    PackageMetadata requested_meta;
    PackageMetadata* requested_meta_ptr = nullptr;
    if (resolve_local_or_repo_package_metadata(requested_dep.name, requested_meta, &context)) {
        requested_meta_ptr = &requested_meta;
    }

    std::string best_name;
    PackageMetadata best_meta;
    for (const auto& base_name : context.present_base_packages) {
        PackageMetadata base_meta;
        if (!resolve_local_or_repo_package_metadata(base_name, base_meta, &context)) continue;

        if (!package_metadata_satisfies_dependency(base_name, base_meta, requested_dep) &&
            !package_replaces_package(base_meta, requested_dep.name, requested_meta_ptr)) {
            continue;
        }

        if (best_name.empty() ||
            should_prefer_family_candidate(
                requested_dep.name,
                requested_meta_ptr,
                base_name,
                base_meta,
                best_name,
                &best_meta
            )) {
            best_name = base_name;
            best_meta = base_meta;
        }
    }

    if (!best_name.empty() && best_name != requested_dep.name) {
        VLOG(verbose, "Normalized " << requested_dep.name
                     << " to present base-backed family " << best_name);
    }
    return best_name;
}

std::string normalize_upgrade_root_name(
    const std::string& raw_name,
    UpgradeContext& context,
    bool verbose
) {
    std::string canonical_name = canonicalize_package_name(raw_name, verbose);
    if (canonical_name.empty()) return "";

    Dependency requested_dep{canonical_name, "", ""};
    if (package_has_exact_live_install_state(canonical_name, nullptr, &context)) {
        return canonical_name;
    }

    std::string exact_live_family = find_best_exact_live_family(requested_dep, context, verbose);
    if (!exact_live_family.empty()) return exact_live_family;

    std::string base_family = find_best_present_base_family(requested_dep, context, verbose);
    if (!base_family.empty()) return base_family;

    PackageMetadata repo_meta;
    if (get_repo_package_info(canonical_name, repo_meta)) return repo_meta.name;

    return find_provider(canonical_name, requested_dep.op, requested_dep.version, verbose);
}

std::vector<std::string> collect_normalized_upgrade_roots(
    UpgradeContext& context,
    bool verbose
) {
    context.normalized_root_by_raw.clear();
    context.shadowed_aliases_by_target.clear();
    context.shadowed_base_alias_target.clear();

    std::set<std::string> raw_roots;
    for (const auto& pkg : context.registered_package_names) {
        if (!pkg.empty()) raw_roots.insert(canonicalize_package_name(pkg, verbose));
    }
    for (const auto& pkg : context.upgrade_catalog.resolved_roots) {
        if (!pkg.empty()) raw_roots.insert(canonicalize_package_name(pkg, verbose));
    }

    std::vector<std::string> normalized_roots;
    std::set<std::string> emitted_targets;
    for (const auto& raw_name : raw_roots) {
        if (raw_name.empty()) continue;

        if (is_blocked_import_package(raw_name, verbose)) {
            VLOG(verbose, "Skipping policy-blocked package during normalized upgrade scan: "
                         << raw_name);
            continue;
        }

        std::string target = normalize_upgrade_root_name(raw_name, context, verbose);
        if (target.empty()) {
            VLOG(verbose, "No repository candidate available for normalized upgrade root "
                         << raw_name);
            continue;
        }

        context.normalized_root_by_raw[raw_name] = target;
        if (raw_name != target) {
            context.shadowed_aliases_by_target[target].push_back(raw_name);
            if (context_package_has_present_base_registry_state(context, raw_name)) {
                context.shadowed_base_alias_target[raw_name] = target;
            }
        }

        if (emitted_targets.insert(target).second) {
            normalized_roots.push_back(target);
        }
    }

    return normalized_roots;
}

PlannedPackageKind classify_planned_package(const PackageMetadata& meta, std::string* current_version = nullptr) {
    std::string installed_version;
    if (!get_local_installed_package_version(meta.name, &installed_version)) {
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

std::vector<std::string> build_worker_command_argv(
    const std::string& mode,
    const std::string& value,
    bool verbose,
    bool defer_runtime_refresh = false,
    bool defer_selinux_relabel = false
) {
    std::vector<std::string> argv = {"gpkg-worker", mode, value};
    if (verbose) argv.push_back("--verbose");
    if (defer_runtime_refresh) argv.push_back("--defer-runtime-linker-refresh");
    if (defer_selinux_relabel) argv.push_back("--defer-selinux-relabel");
    if (!ROOT_PREFIX.empty()) {
        argv.push_back("--root");
        argv.push_back(ROOT_PREFIX);
    }
    return argv;
}

InstallCommandResult install_package_from_file(const std::string& pkg_file, bool verbose) {
    CommandCaptureResult result = run_command_captured_argv(
        build_worker_command_argv("--install", pkg_file, verbose, true, true),
        verbose,
        "gpkg-install"
    );
    if (result.exit_code == 0) g_pending_triggers.insert(SELINUX_RELABEL_TRIGGER);
    return {result.exit_code == 0, result.log_path};
}

InstallCommandResult retire_package_by_name(const std::string& pkg_name, bool verbose) {
    CommandCaptureResult result = run_command_captured_argv(
        build_worker_command_argv("--retire", pkg_name, verbose, true, true),
        verbose,
        "gpkg-retire"
    );
    if (result.exit_code == 0) g_pending_triggers.insert(SELINUX_RELABEL_TRIGGER);
    return {result.exit_code == 0, result.log_path};
}

InstallCommandResult remove_package_by_name(const std::string& pkg_name, bool verbose) {
    CommandCaptureResult result = run_command_captured_argv(
        build_worker_command_argv("--remove", pkg_name, verbose, true, true),
        verbose,
        "gpkg-remove"
    );
    if (result.exit_code == 0) g_pending_triggers.insert(SELINUX_RELABEL_TRIGGER);
    return {result.exit_code == 0, result.log_path};
}

InstallCommandResult purge_package_by_name(const std::string& pkg_name, bool verbose) {
    CommandCaptureResult result = run_command_captured_argv(
        build_worker_command_argv("--purge", pkg_name, verbose, true, true),
        verbose,
        "gpkg-purge"
    );
    if (result.exit_code == 0) g_pending_triggers.insert(SELINUX_RELABEL_TRIGGER);
    return {result.exit_code == 0, result.log_path};
}

bool package_is_config_files_only(const std::string& pkg_name, std::string* out_version = nullptr);
bool package_is_removal_protected(const std::string& pkg_name, std::string* reason_out = nullptr);
std::set<std::string> get_autoremove_protected_kernel_packages(bool verbose);

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

bool parse_decimal_u64(const std::string& text, uint64_t* out) {
    if (out) *out = 0;

    std::string trimmed = trim(text);
    if (trimmed.empty()) return false;

    char* end = nullptr;
    errno = 0;
    unsigned long long value = std::strtoull(trimmed.c_str(), &end, 10);
    if (errno != 0 || end == trimmed.c_str()) return false;
    while (end && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (end && *end != '\0') return false;

    if (out) *out = static_cast<uint64_t>(value);
    return true;
}

struct MultiarchLogicalPrefixMapForEstimate {
    const char* path_prefix;
    const char* canonical_prefix;
};

const MultiarchLogicalPrefixMapForEstimate k_multiarch_logical_prefix_maps_for_estimate[] = {
    {"/lib64/x86_64-linux-gnu", "/lib/x86_64-linux-gnu"},
    {"/lib64", "/lib/x86_64-linux-gnu"},
    {"/lib/x86_64-linux-gnu", "/lib/x86_64-linux-gnu"},
    {"/usr/lib64/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu"},
    {"/usr/lib64", "/usr/lib/x86_64-linux-gnu"},
    {"/usr/lib/x86_64-linux-gnu", "/usr/lib/x86_64-linux-gnu"},
};

std::string canonical_multiarch_logical_path_for_estimate(const std::string& path) {
    for (const auto& map : k_multiarch_logical_prefix_maps_for_estimate) {
        std::string prefix = map.path_prefix;
        if (path == prefix) return map.canonical_prefix;
        if (path.rfind(prefix + "/", 0) != 0) continue;
        return std::string(map.canonical_prefix) + path.substr(prefix.size());
    }
    return path;
}

std::string normalize_tar_member_path_for_estimate(const std::string& raw_path, bool strip_data) {
    std::string line = trim(raw_path);
    if (line.empty() || line == "." || line == "./") return "";

    if (line.rfind("./", 0) == 0) line.erase(0, 2);

    if (strip_data) {
        if (line.rfind("data/", 0) == 0) {
            line.erase(0, 5);
        } else {
            return "";
        }
    }

    if (!line.empty() && line.back() == '/') line.pop_back();
    if (line.empty()) return "";
    return canonical_multiarch_logical_path_for_estimate("/" + line);
}

std::string installed_conffile_manifest_path(const std::string& pkg_name) {
    return INFO_DIR + pkg_name + ".conffiles";
}

uint64_t saturating_add_u64(uint64_t left, uint64_t right) {
    if (right > std::numeric_limits<uint64_t>::max() - left) {
        return std::numeric_limits<uint64_t>::max();
    }
    return left + right;
}

int64_t saturating_add_i64(int64_t left, int64_t right) {
    if (right > 0 && left > std::numeric_limits<int64_t>::max() - right) {
        return std::numeric_limits<int64_t>::max();
    }
    if (right < 0 && left < std::numeric_limits<int64_t>::min() - right) {
        return std::numeric_limits<int64_t>::min();
    }
    return left + right;
}

int64_t signed_disk_delta(uint64_t added, uint64_t removed) {
    if (added >= removed) {
        uint64_t delta = added - removed;
        if (delta > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return std::numeric_limits<int64_t>::max();
        }
        return static_cast<int64_t>(delta);
    }

    uint64_t freed = removed - added;
    if (freed > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
        return std::numeric_limits<int64_t>::min() + 1;
    }
    return -static_cast<int64_t>(freed);
}

bool estimate_declared_installed_bytes(const PackageMetadata& meta, uint64_t* bytes_out, bool* approximate_out = nullptr) {
    if (bytes_out) *bytes_out = 0;
    if (approximate_out) *approximate_out = false;

    uint64_t parsed = 0;
    if (parse_decimal_u64(meta.installed_size_bytes, &parsed)) {
        if (bytes_out) *bytes_out = parsed;
        if (approximate_out) *approximate_out = true;
        return true;
    }
    if (parse_decimal_u64(meta.size, &parsed)) {
        if (bytes_out) *bytes_out = parsed;
        if (approximate_out) *approximate_out = true;
        return true;
    }

    if (approximate_out) *approximate_out = true;
    return false;
}

uint64_t measure_live_path_disk_bytes_no_follow(
    const std::string& full_path,
    std::set<std::pair<dev_t, ino_t>>* seen_inodes = nullptr
) {
    struct stat st {};
    if (lstat(full_path.c_str(), &st) != 0) return 0;

    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) return 0;

    if (!S_ISLNK(st.st_mode) && seen_inodes) {
        std::pair<dev_t, ino_t> inode_key{st.st_dev, st.st_ino};
        if (!seen_inodes->insert(inode_key).second) return 0;
    }

    if (st.st_blocks > 0) {
        return static_cast<uint64_t>(st.st_blocks) * 512ULL;
    }

    if (st.st_size <= 0) return 0;
    return static_cast<uint64_t>(st.st_size);
}

uint64_t measure_manifest_payload_bytes(
    const std::vector<std::string>& paths,
    const std::set<std::string>* exclude_paths = nullptr
) {
    std::set<std::string> seen;
    std::set<std::pair<dev_t, ino_t>> seen_inodes;
    uint64_t total = 0;
    for (const auto& path : paths) {
        if (path.empty()) continue;
        std::string canonical_path = canonical_multiarch_logical_path_for_estimate(path);
        if (exclude_paths && exclude_paths->count(canonical_path) != 0) continue;
        if (!seen.insert(canonical_path).second) continue;
        total = saturating_add_u64(
            total,
            measure_live_path_disk_bytes_no_follow(ROOT_PREFIX + canonical_path, &seen_inodes)
        );
    }
    return total;
}

uint64_t estimate_current_installed_payload_bytes(
    const std::string& pkg_name,
    bool include_conffiles,
    bool* approximate_out = nullptr
) {
    if (approximate_out) *approximate_out = false;

    std::vector<std::string> installed_files = read_installed_file_list(pkg_name);
    std::set<std::string> excluded_paths;
    if (!include_conffiles) {
        std::vector<std::string> conffiles = load_dependency_entries(installed_conffile_manifest_path(pkg_name));
        for (const auto& conffile : conffiles) {
            excluded_paths.insert(canonical_multiarch_logical_path_for_estimate(conffile));
        }
    }

    uint64_t measured = measure_manifest_payload_bytes(
        installed_files,
        excluded_paths.empty() ? nullptr : &excluded_paths
    );
    if (measured != 0) return measured;

    PackageMetadata installed_meta;
    if (get_installed_package_metadata(pkg_name, installed_meta)) {
        bool declared_approximate = false;
        uint64_t declared = 0;
        if (estimate_declared_installed_bytes(installed_meta, &declared, &declared_approximate)) {
            if (approximate_out) *approximate_out = declared_approximate;
            return declared;
        }
    }

    if (approximate_out) *approximate_out = true;
    return 0;
}

struct CachedArchivePayloadInfo {
    bool available = false;
    bool approximate = true;
    uint64_t target_bytes = 0;
    std::vector<std::string> installed_paths;
};

CachedArchivePayloadInfo inspect_payload_tar_for_disk_estimate(const std::string& tar_path) {
    CachedArchivePayloadInfo info;
    std::vector<GpkgArchive::TarEntry> entries;
    std::string error;
    if (!GpkgArchive::tar_list_entries(tar_path, entries, &error)) {
        return info;
    }

    bool strip_data = false;
    for (const auto& entry : entries) {
        std::string normalized = trim(entry.path);
        if (normalized.rfind("./", 0) == 0) normalized.erase(0, 2);
        if (normalized.rfind("data/", 0) == 0 || normalized == "data") {
            strip_data = true;
            break;
        }
    }

    std::set<std::string> seen_paths;
    bool has_non_regular_entries = false;
    for (const auto& entry : entries) {
        std::string normalized_path = normalize_tar_member_path_for_estimate(entry.path, strip_data);
        if (normalized_path.empty()) continue;

        if (entry.type == GpkgArchive::TarEntryType::Regular) {
            if (seen_paths.insert(normalized_path).second) {
                info.installed_paths.push_back(normalized_path);
                info.target_bytes = saturating_add_u64(info.target_bytes, entry.size);
            }
            continue;
        }

        if (entry.type == GpkgArchive::TarEntryType::Symlink ||
            entry.type == GpkgArchive::TarEntryType::Hardlink) {
            if (seen_paths.insert(normalized_path).second) {
                info.installed_paths.push_back(normalized_path);
            }
            has_non_regular_entries = true;
        }
    }

    info.available = true;
    info.approximate = has_non_regular_entries;
    return info;
}

bool inspect_gpkg_archive_payload_for_disk_estimate(
    const std::string& archive_path,
    CachedArchivePayloadInfo* out_info
) {
    if (out_info) *out_info = CachedArchivePayloadInfo{};

    char temp_template[] = "/tmp/gpkg-disk-estimate-XXXXXX";
    char* temp_dir = mkdtemp(temp_template);
    if (!temp_dir) return false;

    std::string temp_root = temp_dir;
    bool ok = false;
    std::string archive_error;
    std::string outer_tar = temp_root + "/archive.tar";
    if (!GpkgArchive::decompress_zstd_file(archive_path, outer_tar, &archive_error)) {
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    std::string unpack_root = temp_root + "/unpack";
    if (!mkdir_p(unpack_root) ||
        !GpkgArchive::tar_extract_to_directory(outer_tar, unpack_root, {}, &archive_error)) {
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    std::string data_archive;
    if (!locate_deb_data_archive(unpack_root, data_archive)) {
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    std::string payload_tar;
    if (!materialize_deb_payload_tar(data_archive, temp_root, payload_tar, &archive_error)) {
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    CachedArchivePayloadInfo info = inspect_payload_tar_for_disk_estimate(payload_tar);
    ok = info.available;
    if (out_info) *out_info = info;
    run_command("rm -rf " + shell_quote(temp_root), false);
    return ok;
}

bool inspect_debian_archive_payload_for_disk_estimate(
    const std::string& archive_path,
    CachedArchivePayloadInfo* out_info
) {
    if (out_info) *out_info = CachedArchivePayloadInfo{};

    char temp_template[] = "/tmp/gpkg-deb-disk-estimate-XXXXXX";
    char* temp_dir = mkdtemp(temp_template);
    if (!temp_dir) return false;

    std::string temp_root = temp_dir;
    bool ok = false;
    std::string archive_error;
    if (!GpkgArchive::extract_ar_archive_to_directory(archive_path, temp_root, &archive_error)) {
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    std::string data_archive;
    if (!locate_deb_data_archive(temp_root, data_archive)) {
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    std::string payload_tar;
    if (!materialize_deb_payload_tar(data_archive, temp_root, payload_tar, &archive_error)) {
        run_command("rm -rf " + shell_quote(temp_root), false);
        return false;
    }

    CachedArchivePayloadInfo info = inspect_payload_tar_for_disk_estimate(payload_tar);
    ok = info.available;
    if (out_info) *out_info = info;
    run_command("rm -rf " + shell_quote(temp_root), false);
    return ok;
}

bool get_cached_archive_payload_info(const PackageMetadata& meta, CachedArchivePayloadInfo* out_info) {
    if (out_info) *out_info = CachedArchivePayloadInfo{};

    std::string cache_key;
    enum class ArchiveSourceKind { None, Gpkg, Debian } source_kind = ArchiveSourceKind::None;

    if (package_is_debian_source(meta)) {
        std::string imported_path = get_imported_gpkg_path(meta);
        if (access(imported_path.c_str(), F_OK) == 0) {
            cache_key = "gpkg:" + imported_path;
            source_kind = ArchiveSourceKind::Gpkg;
        } else {
            std::string deb_path = get_cached_debian_archive_path(meta);
            if (access(deb_path.c_str(), F_OK) == 0) {
                cache_key = "deb:" + deb_path;
                source_kind = ArchiveSourceKind::Debian;
            }
        }
    } else {
        std::string gpkg_path = get_cached_package_path(meta);
        if (access(gpkg_path.c_str(), F_OK) == 0) {
            cache_key = "gpkg:" + gpkg_path;
            source_kind = ArchiveSourceKind::Gpkg;
        }
    }

    if (source_kind == ArchiveSourceKind::None) return false;

    static std::map<std::string, CachedArchivePayloadInfo> cache;
    auto cache_it = cache.find(cache_key);
    if (cache_it != cache.end()) {
        if (out_info) *out_info = cache_it->second;
        return cache_it->second.available;
    }

    CachedArchivePayloadInfo info;
    bool ok = false;
    if (source_kind == ArchiveSourceKind::Gpkg) {
        ok = inspect_gpkg_archive_payload_for_disk_estimate(cache_key.substr(5), &info);
    } else {
        ok = inspect_debian_archive_payload_for_disk_estimate(cache_key.substr(4), &info);
    }
    if (!ok) info = CachedArchivePayloadInfo{};
    cache[cache_key] = info;
    if (out_info) *out_info = info;
    return info.available;
}

uint64_t estimate_current_purge_only_bytes(const std::string& pkg_name, bool* approximate_out = nullptr) {
    if (approximate_out) *approximate_out = false;

    std::vector<std::string> conffiles = load_dependency_entries(installed_conffile_manifest_path(pkg_name));
    uint64_t measured = measure_manifest_payload_bytes(conffiles);
    if (measured != 0) return measured;

    if (approximate_out) *approximate_out = true;
    return 0;
}

struct TransactionDiskEstimate {
    int64_t net_bytes = 0;
    bool approximate = false;
};

TransactionDiskEstimate estimate_install_transaction_disk_change(
    const std::vector<PackageMetadata>& packages,
    bool include_retirement_uncertainty
) {
    TransactionDiskEstimate estimate;
    estimate.approximate = include_retirement_uncertainty;

    for (const auto& pkg : packages) {
        uint64_t target_bytes = 0;
        bool target_approximate = false;
        uint64_t current_bytes = 0;
        bool current_approximate = false;
        CachedArchivePayloadInfo archive_info;
        bool have_archive_info = get_cached_archive_payload_info(pkg, &archive_info);
        if (have_archive_info) {
            target_bytes = archive_info.target_bytes;
            target_approximate = archive_info.approximate;
        } else if (!estimate_declared_installed_bytes(pkg, &target_bytes, &target_approximate)) {
            target_approximate = true;
        }

        if (is_installed(pkg.name)) {
            current_bytes = estimate_current_installed_payload_bytes(pkg.name, true, &current_approximate);
        } else if (have_archive_info) {
            current_bytes = measure_manifest_payload_bytes(archive_info.installed_paths);
            current_approximate = archive_info.approximate;
        } else if (package_is_base_system_provided(pkg.name)) {
            current_approximate = true;
        }

        estimate.approximate = estimate.approximate || target_approximate || current_approximate;
        estimate.net_bytes = saturating_add_i64(
            estimate.net_bytes,
            signed_disk_delta(target_bytes, current_bytes)
        );
    }

    return estimate;
}

TransactionDiskEstimate estimate_removal_transaction_disk_change(
    const std::vector<std::string>& to_remove,
    const std::vector<std::string>& to_purge
) {
    TransactionDiskEstimate estimate;
    std::set<std::string> removed_packages(to_remove.begin(), to_remove.end());

    for (const auto& pkg : to_remove) {
        bool approximate = false;
        uint64_t bytes = estimate_current_installed_payload_bytes(pkg, false, &approximate);
        estimate.approximate = estimate.approximate || approximate;
        estimate.net_bytes = saturating_add_i64(estimate.net_bytes, signed_disk_delta(0, bytes));
    }

    for (const auto& pkg : to_purge) {
        bool approximate = false;
        uint64_t bytes = estimate_current_purge_only_bytes(pkg, &approximate);
        if (removed_packages.count(pkg) == 0) {
            estimate.approximate = estimate.approximate || approximate;
            estimate.net_bytes = saturating_add_i64(estimate.net_bytes, signed_disk_delta(0, bytes));
            continue;
        }

        estimate.approximate = estimate.approximate || approximate;
        estimate.net_bytes = saturating_add_i64(estimate.net_bytes, signed_disk_delta(0, bytes));
    }

    return estimate;
}

void print_transaction_disk_change_summary(const TransactionDiskEstimate& estimate) {
    if (estimate.net_bytes == 0) return;

    uint64_t absolute = estimate.net_bytes < 0
        ? static_cast<uint64_t>(-(estimate.net_bytes + 1)) + 1
        : static_cast<uint64_t>(estimate.net_bytes);

    std::cout << "After this operation, ";
    if (estimate.approximate) std::cout << "about ";
    std::cout << format_total_bytes(absolute);
    if (estimate.net_bytes > 0) {
        std::cout << " of additional disk space will be used.";
    } else {
        std::cout << " of disk space will be freed.";
    }
    std::cout << std::endl;
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

        for (const auto& dep_str : collect_required_transaction_dependency_edges(meta_it->second)) {
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
    return collect_registered_package_names_from_status_records(load_package_status_records());
}

std::vector<std::string> get_exact_live_package_names() {
    std::set<std::string> package_names;

    for (const auto& pkg : get_registered_package_names()) {
        if (!pkg.empty()) package_names.insert(pkg);
    }

    for (const auto& record : load_dpkg_package_status_records()) {
        if (record.package.empty()) continue;
        if (!package_status_is_installed_like(record.status)) continue;
        package_names.insert(record.package);
    }

    return std::vector<std::string>(package_names.begin(), package_names.end());
}

std::set<std::string> get_exact_live_installed_package_set() {
    auto names = get_exact_live_package_names();
    return std::set<std::string>(names.begin(), names.end());
}

std::set<std::string> get_registered_installed_package_set(const std::set<std::string>& excluding = {}) {
    std::set<std::string> installed;
    for (const auto& pkg : get_registered_package_names()) {
        if (excluding.count(pkg) != 0) continue;
        if (!is_installed(pkg)) continue;
        installed.insert(pkg);
    }
    return installed;
}

bool update_package_auto_install_state_after_install(
    const std::string& pkg_name,
    bool should_be_manual,
    const std::set<std::string>& previously_registered
) {
    if (pkg_name.empty()) return true;
    if (should_be_manual) return set_package_auto_installed_state(pkg_name, false);
    if (previously_registered.count(pkg_name) != 0) return true;
    return set_package_auto_installed_state(pkg_name, true);
}

std::string resolve_requested_package_for_manual_marking(
    const std::string& requested_name,
    const std::vector<PackageMetadata>& planned_queue,
    const std::set<std::string>& installed_cache,
    bool verbose
) {
    Dependency requested_dep{canonicalize_package_name(requested_name, verbose), "", ""};
    for (const auto& meta : planned_queue) {
        if (package_metadata_satisfies_dependency(meta.name, meta, requested_dep)) {
            return meta.name;
        }
    }

    std::string provider_name;
    if (find_installed_dependency_provider(requested_dep, installed_cache, &provider_name)) {
        return provider_name;
    }

    std::string installed_ver;
    if (is_installed(requested_dep.name, &installed_ver)) return requested_dep.name;
    return "";
}

std::set<std::string> compute_needed_installed_packages(
    const std::set<std::string>& installed_cache,
    const std::set<std::string>& protected_kernel_packages,
    bool verbose
) {
    (void)verbose;
    std::set<std::string> needed;
    std::vector<std::string> queue;

    for (const auto& pkg : installed_cache) {
        std::string protection_reason;
        bool auto_installed = false;
        get_package_auto_installed_state(pkg, &auto_installed);
        if (!auto_installed ||
            package_is_removal_protected(pkg, &protection_reason) ||
            protected_kernel_packages.count(pkg) != 0) {
            if (needed.insert(pkg).second) queue.push_back(pkg);
        }
    }

    for (size_t index = 0; index < queue.size(); ++index) {
        PackageMetadata meta;
        if (!get_installed_package_metadata(queue[index], meta)) continue;

        for (const auto& dep_str : collect_required_transaction_dependency_edges(meta)) {
            Dependency dep = parse_dependency(dep_str);
            if (dep.name.empty()) continue;

            std::string provider_name;
            if (!find_installed_dependency_provider(dep, installed_cache, &provider_name)) continue;
            if (provider_name.empty()) continue;
            if (needed.insert(provider_name).second) queue.push_back(provider_name);
        }
    }

    return needed;
}

std::vector<std::string> collect_autoremove_packages(
    const std::set<std::string>& preplanned_removals,
    bool verbose,
    std::set<std::string>* kept_kernel_packages_out = nullptr,
    std::map<std::string, std::string>* kept_protected_packages_out = nullptr
) {
    std::set<std::string> protected_kernel_packages = get_autoremove_protected_kernel_packages(verbose);
    if (kept_kernel_packages_out) kept_kernel_packages_out->clear();
    if (kept_protected_packages_out) kept_protected_packages_out->clear();

    std::set<std::string> remaining_installed = get_registered_installed_package_set(preplanned_removals);
    std::set<std::string> needed = compute_needed_installed_packages(
        remaining_installed,
        protected_kernel_packages,
        verbose
    );

    std::vector<std::string> removable;
    for (const auto& pkg : remaining_installed) {
        bool auto_installed = false;
        if (!get_package_auto_installed_state(pkg, &auto_installed) || !auto_installed) continue;

        std::string protection_reason;
        if (package_is_removal_protected(pkg, &protection_reason)) {
            if (kept_protected_packages_out) (*kept_protected_packages_out)[pkg] = protection_reason;
            continue;
        }
        if (protected_kernel_packages.count(pkg) != 0) {
            if (kept_kernel_packages_out) kept_kernel_packages_out->insert(pkg);
            continue;
        }
        if (needed.count(pkg) != 0) continue;
        removable.push_back(pkg);
    }

    if (!removable.empty()) sort_removal_queue_for_operation(removable, verbose);
    return removable;
}

std::vector<std::string> collect_autoremove_purge_only_packages(
    const std::set<std::string>& already_selected,
    bool verbose
) {
    std::set<std::string> protected_kernel_packages = get_autoremove_protected_kernel_packages(verbose);
    std::vector<std::string> purge_only;
    for (const auto& record : load_package_auto_state_records()) {
        if (!record.auto_installed || record.package.empty()) continue;
        if (already_selected.count(record.package) != 0) continue;
        if (!package_is_config_files_only(record.package)) continue;

        std::string protection_reason;
        if (package_is_removal_protected(record.package, &protection_reason)) continue;
        if (protected_kernel_packages.count(record.package) != 0) continue;
        purge_only.push_back(record.package);
    }

    std::sort(purge_only.begin(), purge_only.end());
    purge_only.erase(std::unique(purge_only.begin(), purge_only.end()), purge_only.end());
    return purge_only;
}

int execute_removal_plan(
    const std::vector<std::string>& to_remove,
    const std::vector<std::string>& to_purge,
    bool verbose
) {
    if (to_remove.empty() && to_purge.empty()) {
        std::cout << "Nothing to do." << std::endl;
        return 0;
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

    print_transaction_disk_change_summary(
        estimate_removal_transaction_disk_change(to_remove, to_purge)
    );

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
            if (!erase_package_auto_installed_state(pkg)) {
                if (!verbose) finish_progress_line(&purge_progress_width);
                std::cerr << Color::RED << "E: Failed to update gpkg auto-install state for "
                          << pkg << Color::RESET << std::endl;
                return 1;
            }
            if (!verbose) render_package_progress("current", i + 1, to_purge.size(), pkg, &purge_progress_width);
        }

        if (!verbose) finish_progress_line(&purge_progress_width);
    }

    return 0;
}

bool package_is_config_files_only(const std::string& pkg_name, std::string* out_version) {
    PackageStatusRecord record;
    if (!get_package_status_record(pkg_name, &record)) return false;
    if (record.status != "config-files") return false;
    if (out_version) *out_version = record.version;
    return true;
}

bool package_is_removal_protected(const std::string& pkg_name, std::string* reason_out) {
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

struct PreparedUpgradeState {
    std::vector<PackageMetadata> upgrade_queue;
    std::vector<UpgradePlanEntry> explicit_targets;
    TransactionPlan plan;
    std::vector<std::string> skipped_managed_packages;
};

void prune_shadowed_upgrade_targets(
    std::vector<PackageMetadata>& upgrade_queue,
    std::vector<UpgradePlanEntry>& explicit_targets,
    bool verbose
) {
    if (upgrade_queue.size() < 2) return;

    std::vector<bool> keep(upgrade_queue.size(), true);
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < upgrade_queue.size(); ++i) {
            if (!keep[i]) continue;

            for (size_t j = 0; j < upgrade_queue.size(); ++j) {
                if (i == j || !keep[j]) continue;
                if (!queued_upgrade_candidate_shadows_base_alias(
                        upgrade_queue[j],
                        upgrade_queue[i],
                        verbose
                    )) {
                    continue;
                }

                keep[i] = false;
                changed = true;
                break;
            }
        }
    }

    std::vector<PackageMetadata> filtered_queue;
    std::set<std::string> kept_names;
    filtered_queue.reserve(upgrade_queue.size());
    for (size_t i = 0; i < upgrade_queue.size(); ++i) {
        if (!keep[i]) continue;
        filtered_queue.push_back(upgrade_queue[i]);
        kept_names.insert(upgrade_queue[i].name);
    }
    upgrade_queue.swap(filtered_queue);

    std::vector<UpgradePlanEntry> filtered_targets;
    filtered_targets.reserve(explicit_targets.size());
    for (const auto& entry : explicit_targets) {
        if (kept_names.count(entry.meta.name) == 0) continue;
        filtered_targets.push_back(entry);
    }
    explicit_targets.swap(filtered_targets);
}

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

bool package_metadata_is_managed_debian_package(const PackageMetadata& meta) {
    return meta.source_kind == "debian" || !meta.debian_package.empty();
}

bool get_live_package_metadata_for_upgrade_resolution(
    const std::string& pkg_name,
    PackageMetadata& out_meta,
    UpgradeContext* context = nullptr
) {
    if (context && get_context_live_installed_package_metadata(*context, pkg_name, out_meta)) {
        return true;
    }
    return get_installed_package_metadata(pkg_name, out_meta);
}

bool resolve_upgrade_target_metadata(
    const Dependency& requested_dep,
    PackageMetadata& out_meta,
    bool verbose,
    RawDebianContext* raw_context = nullptr,
    const PackageMetadata* installed_meta = nullptr,
    std::string* reason_out = nullptr
) {
    PackageUniverseResult result;
    bool ok = resolve_full_universe_relation_candidate(
        requested_dep.name,
        requested_dep.op,
        requested_dep.version,
        result,
        verbose,
        raw_context,
        installed_meta
    );
    if (reason_out) *reason_out = result.reason;
    if (!ok) return false;
    out_meta = result.meta;
    return true;
}

bool redirect_upgrade_target_to_live_provider(
    const Dependency& requested_dep,
    const std::set<std::string>& installed_cache,
    bool verbose,
    Dependency& redirected_dep,
    UpgradeContext* context = nullptr,
    RawDebianContext* raw_context = nullptr
) {
    redirected_dep = requested_dep;

    std::string canonical_requested = canonicalize_package_name(requested_dep.name, verbose);
    if (canonical_requested.empty()) return false;
    if (package_has_exact_live_install_state(canonical_requested, nullptr, context)) return false;

    std::string provider_name;
    if (!find_installed_dependency_provider(
            requested_dep,
            installed_cache,
            &provider_name,
            context,
            verbose
        )) {
        return false;
    }
    if (provider_name.empty() || provider_name == BASE_SYSTEM_PROVIDER) return false;

    provider_name = canonicalize_package_name(provider_name, verbose);
    if (provider_name.empty() || provider_name == canonical_requested) return false;

    PackageMetadata provider_repo_meta;
    PackageMetadata provider_live_meta;
    PackageMetadata* provider_live_meta_ptr = nullptr;
    if (get_live_package_metadata_for_upgrade_resolution(provider_name, provider_live_meta, context)) {
        provider_live_meta_ptr = &provider_live_meta;
    }
    if (!resolve_upgrade_target_metadata(
            {provider_name, "", ""},
            provider_repo_meta,
            verbose,
            raw_context,
            provider_live_meta_ptr
        )) {
        return false;
    }
    if (!package_metadata_satisfies_dependency(provider_name, provider_repo_meta, requested_dep)) return false;

    VLOG(verbose, "Redirecting upgrade target " << canonical_requested
         << " to installed provider " << provider_name);
    redirected_dep = {provider_name, "", ""};
    return true;
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
    bool force_reinstall = false,
    UpgradeContext* context = nullptr,
    RawDebianContext* raw_context = nullptr,
    std::string* failure_reason_out = nullptr
) {
    if (failure_reason_out) failure_reason_out->clear();
    Dependency effective_dep = requested_dep;
    redirect_upgrade_target_to_live_provider(
        requested_dep,
        installed_cache,
        verbose,
        effective_dep,
        context,
        raw_context
    );

    PackageMetadata meta;
    PackageMetadata live_meta;
    PackageMetadata* live_meta_ptr = nullptr;
    if (get_live_package_metadata_for_upgrade_resolution(effective_dep.name, live_meta, context)) {
        live_meta_ptr = &live_meta;
    }
    if (!resolve_upgrade_target_metadata(
            effective_dep,
            meta,
            verbose,
            raw_context,
            live_meta_ptr,
            failure_reason_out
        )) {
        VLOG(verbose, "No repository candidate available for upgrade target " << requested_dep.name);
        return true;
    }

    std::string current_version;
    bool was_installed = get_local_installed_package_version(meta.name, &current_version, context);
    if (!was_installed) {
        std::string canonical_requested = canonicalize_package_name(requested_dep.name, verbose);
        if (canonical_requested != requested_dep.name) {
            was_installed = get_local_installed_package_version(
                requested_dep.name,
                &current_version,
                context
            );
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
                force_reinstall,
                context,
                raw_context,
                failure_reason_out
            )) {
            target_walk.erase(meta.name);
            return false;
        }
    }

    for (const auto& edge : collect_transaction_dependency_edge_details(meta)) {
        Dependency dep = parse_dependency(edge.relation);
        if (dep.name.empty()) continue;

        if (!transaction_dependency_is_optional(edge.kind)) {
            if (!resolve_dependencies(
                    dep.name,
                    dep.op,
                    dep.version,
                    install_queue,
                    dependency_visited,
                    installed_cache,
                    verbose,
                    false,
                    context,
                    false,
                    raw_context,
                    failure_reason_out
                )) {
                target_walk.erase(meta.name);
                return false;
            }
            continue;
        }

        auto optional_queue = install_queue;
        auto optional_visited = dependency_visited;
        if (!resolve_dependencies(
                dep.name,
                dep.op,
                dep.version,
                optional_queue,
                optional_visited,
                installed_cache,
                verbose,
                false,
                context,
                true,
                raw_context
            )) {
            VLOG(
                verbose,
                "Skipping optional " << transaction_dependency_kind_label(edge.kind)
                    << " dependency " << edge.relation << " for upgrade target "
                    << meta.name << " because no importable candidate is available."
            );
            continue;
        }

        install_queue.swap(optional_queue);
        dependency_visited.swap(optional_visited);
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

bool package_should_use_raw_debian_upgrade_fallback(
    const std::string& pkg_name,
    UpgradeContext& context
) {
    if (context.registered_package_set.count(pkg_name) == 0) return false;
    if (std::find(
            context.upgrade_catalog.resolved_roots.begin(),
            context.upgrade_catalog.resolved_roots.end(),
            pkg_name
        ) != context.upgrade_catalog.resolved_roots.end()) {
        return false;
    }

    PackageMetadata meta;
    if (!get_live_package_metadata_for_upgrade_resolution(pkg_name, meta, &context)) return false;
    return package_metadata_is_managed_debian_package(meta);
}

bool prepare_upgrade_transaction(
    UpgradeContext& context,
    bool verbose,
    PreparedUpgradeState& out_state
) {
    out_state = {};
    if (!context.upgrade_catalog_available) return false;

    std::vector<std::string> normalized_roots = collect_normalized_upgrade_roots(context, verbose);
    VLOG(verbose, "Checking " << normalized_roots.size()
         << " normalized installed or importable package families and upgradeable base runtimes.");

    std::set<std::string> queued_packages;
    std::set<std::string> explicit_target_names;
    std::set<std::string> target_walk;
    std::set<std::string> dependency_visited;
    auto companion_map = get_planner_upgrade_companion_map(&context, verbose);
    RawDebianContext raw_context;

    for (const auto& pkg : normalized_roots) {
        std::string current_ver;
        bool was_installed = get_local_installed_package_version(pkg, &current_ver, &context);
        bool allow_raw_fallback = package_should_use_raw_debian_upgrade_fallback(pkg, context);

        PackageMetadata repo_meta;
        std::string resolve_reason;
        PackageMetadata installed_meta;
        PackageMetadata* installed_meta_ptr = nullptr;
        if (allow_raw_fallback &&
            get_live_package_metadata_for_upgrade_resolution(pkg, installed_meta, &context)) {
            installed_meta_ptr = &installed_meta;
        }
        if (!resolve_upgrade_target_metadata(
                {pkg, "", ""},
                repo_meta,
                verbose,
                allow_raw_fallback ? &raw_context : nullptr,
                installed_meta_ptr,
                &resolve_reason
            )) {
            if (allow_raw_fallback) {
                out_state.skipped_managed_packages.push_back(
                    pkg + " was skipped: " +
                    (resolve_reason.empty()
                        ? "no cached Debian upgrade candidate is currently available"
                        : resolve_reason)
                );
            }
            continue;
        }
        if (was_installed &&
            !g_force_reinstall &&
            compare_versions(repo_meta.version, current_ver) <= 0) {
            continue;
        }

        if (was_installed && compare_versions(repo_meta.version, current_ver) > 0) {
            VLOG(verbose, "Update found for " << pkg << ": " << current_ver << " -> " << repo_meta.version);
        } else if (was_installed && g_force_reinstall) {
            VLOG(verbose, "Reinstall requested for " << pkg << " at " << current_ver);
        } else if (!was_installed) {
            VLOG(verbose, "Import candidate found for normalized live family " << pkg
                         << " at repository version " << repo_meta.version);
        }
        Dependency dep{pkg, "", ""};
        if (allow_raw_fallback) {
            auto queue_copy = out_state.upgrade_queue;
            auto explicit_copy = out_state.explicit_targets;
            auto queued_copy = queued_packages;
            auto explicit_target_copy = explicit_target_names;
            auto target_walk_copy = target_walk;
            auto dependency_copy = dependency_visited;
            std::string queue_failure_reason;
            if (!queue_upgrade_target(
                    dep,
                    companion_map,
                    queue_copy,
                    explicit_copy,
                    queued_copy,
                    explicit_target_copy,
                    target_walk_copy,
                    dependency_copy,
                    context.exact_live_packages,
                    verbose,
                    g_force_reinstall,
                    &context,
                    &raw_context,
                    &queue_failure_reason
                )) {
                out_state.skipped_managed_packages.push_back(
                    pkg + " was skipped: " +
                    (queue_failure_reason.empty()
                        ? "dependency resolution failed under current GeminiOS policy"
                        : queue_failure_reason)
                );
                continue;
            }
            out_state.upgrade_queue.swap(queue_copy);
            out_state.explicit_targets.swap(explicit_copy);
            queued_packages.swap(queued_copy);
            explicit_target_names.swap(explicit_target_copy);
            target_walk.swap(target_walk_copy);
            dependency_visited.swap(dependency_copy);
            continue;
        }

        if (!queue_upgrade_target(
                dep,
                companion_map,
                out_state.upgrade_queue,
                out_state.explicit_targets,
                queued_packages,
                explicit_target_names,
                target_walk,
                dependency_visited,
                context.exact_live_packages,
                verbose,
                g_force_reinstall,
                &context
            )) {
            return false;
        }
    }

    if (out_state.upgrade_queue.empty()) return true;

    if (!build_transaction_plan(
            out_state.upgrade_queue,
            context.exact_live_packages,
            verbose,
            out_state.plan,
            &context
        )) {
        return false;
    }
    out_state.upgrade_queue = out_state.plan.install_queue;
    return true;
}

std::string describe_upgrade_catalog_skip_entry(const UpgradeCatalogSkipEntry& entry) {
    std::ostringstream out;
    if (entry.kind == "companion") {
        out << "runtime companion " << entry.configured_name;
        if (!entry.trigger.empty()) out << " for " << entry.trigger;
    } else {
        out << "runtime family " << entry.configured_name;
    }
    out << " was skipped: " << entry.reason;
    return out.str();
}

RepairInspection inspect_repair_state(
    const std::vector<std::string>& registered_packages,
    bool verbose
) {
    RepairInspection inspection;
    RawDebianContext raw_context;
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
            for (const auto& dep_str : collect_integrity_dependency_edges(installed_meta)) {
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
                        verbose,
                        false,
                        nullptr,
                        false,
                        &raw_context
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

        PackageMetadata installed_meta;
        PackageMetadata* installed_meta_ptr = nullptr;
        if (get_installed_package_metadata(pkg, installed_meta)) installed_meta_ptr = &installed_meta;

        PackageMetadata repo_meta;
        std::string resolve_reason;
        if (!resolve_upgrade_target_metadata(
                {pkg, "", ""},
                repo_meta,
                verbose,
                &raw_context,
                installed_meta_ptr,
                &resolve_reason
            )) {
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

RepairInspection inspect_repair_state(bool verbose) {
    return inspect_repair_state(get_registered_package_names(), verbose);
}

int handle_upgrade(const std::set<std::string>& installed_cache, bool verbose) {
    (void)installed_cache;
    if (!ensure_repo_index_available()) return 1;

    std::cout << "Reading package lists..." << std::endl;
    if (!ensure_repo_package_cache_loaded(verbose)) return 1;
    std::cout << "Optional dependency policy: " << describe_optional_dependency_policy() << std::endl;
    UpgradeContext context = build_upgrade_context(verbose);
    if (!context.upgrade_catalog_available) {
        std::cerr << Color::RED << "E: "
                  << (context.upgrade_catalog_problem.empty()
                          ? "upgrade catalog is unavailable; run 'gpkg update'"
                          : context.upgrade_catalog_problem)
                  << Color::RESET << std::endl;
        return 1;
    }
    PreparedUpgradeState prepared;
    if (!prepare_upgrade_transaction(context, verbose, prepared)) return 1;
    std::vector<PackageMetadata>& upgrade_queue = prepared.upgrade_queue;
    std::vector<UpgradePlanEntry>& explicit_targets = prepared.explicit_targets;
    TransactionPlan& upgrade_plan = prepared.plan;

    if (upgrade_queue.empty()) {
        for (const auto& warning : prepared.skipped_managed_packages) {
            std::cout << Color::YELLOW << "W: " << warning << Color::RESET << std::endl;
        }
        std::cout << "All packages are up to date." << std::endl;
        return 0;
    }

    for (const auto& warning : prepared.skipped_managed_packages) {
        std::cout << Color::YELLOW << "W: " << warning << Color::RESET << std::endl;
    }

    std::set<std::string> planned_names;
    for (const auto& pkg : upgrade_queue) planned_names.insert(pkg.name);
    std::set<std::string> explicit_target_names;
    for (const auto& entry : explicit_targets) explicit_target_names.insert(entry.meta.name);

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

    print_transaction_disk_change_summary(
        estimate_install_transaction_disk_change(
            upgrade_queue,
            !upgrade_plan.retirements.empty()
        )
    );

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
        if (!update_package_auto_install_state_after_install(
                upgrade_queue[i].name,
                explicit_target_names.count(upgrade_queue[i].name) != 0 &&
                    context.exact_live_packages.count(upgrade_queue[i].name) == 0,
                context.exact_live_packages)) {
            if (!verbose) finish_progress_line(&install_progress_width);
            std::cerr << Color::RED << "E: Failed to update gpkg auto-install state for "
                      << upgrade_queue[i].name << Color::RESET << std::endl;
            failures.push_back(upgrade_queue[i].name);
            break;
        }
        std::vector<std::string> retirements;
        if (should_retire_after_install(upgrade_plan, upgrade_queue[i].name, retirements)) {
            for (const auto& retired_pkg : retirements) {
                std::vector<std::string> retired_files = read_installed_file_list(retired_pkg);
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
                check_triggers(retired_files);
                if (!erase_package_auto_installed_state(retired_pkg)) {
                    if (!verbose) finish_progress_line(&install_progress_width);
                    std::cerr << Color::RED << "E: Failed to update gpkg auto-install state for "
                              << retired_pkg << Color::RESET << std::endl;
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

struct DoctorSection {
    std::string title;
    std::vector<std::string> ok;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

void print_doctor_lines(const std::vector<std::string>& lines, const std::string& color, const std::string& prefix) {
    for (const auto& line : lines) {
        std::cout << "  " << color << prefix << " " << line << Color::RESET << std::endl;
    }
}

void print_doctor_section(const DoctorSection& section) {
    std::cout << section.title << ":" << std::endl;
    print_doctor_lines(section.ok, Color::GREEN, "[OK]");
    print_doctor_lines(section.warnings, Color::YELLOW, "[WARN]");
    print_doctor_lines(section.errors, Color::RED, "[ERR]");
    if (section.ok.empty() && section.warnings.empty() && section.errors.empty()) {
        std::cout << "  " << Color::BLUE << "[INFO] no findings" << Color::RESET << std::endl;
    }
}

int handle_doctor(bool verbose) {
    DoctorSection repo_section;
    repo_section.title = "Repository configuration";
    DoctorSection install_section;
    install_section.title = "Installed package state";
    DoctorSection base_section;
    base_section.title = "Base system registry";
    DoctorSection upgrade_section;
    upgrade_section.title = "Upgrade dry-run";

    auto repo_urls = get_repo_urls();
    repo_section.ok.push_back("Debian backend config: " + load_debian_backend_config(false).packages_url);
    repo_section.ok.push_back("Additional repositories configured: " + std::to_string(repo_urls.size()));

    const std::string merged_index = REPO_CACHE_PATH + "Packages.json";
    struct stat index_st {};
    bool repo_index_present = lstat(merged_index.c_str(), &index_st) == 0;
    if (!repo_index_present) {
        repo_section.errors.push_back("local merged package index is missing; run 'gpkg update'");
    } else {
        repo_section.ok.push_back("local merged package index is present");
        if (!ensure_repo_package_cache_loaded(verbose)) {
            repo_section.errors.push_back("local package index exists but could not be loaded");
        } else {
            repo_section.ok.push_back(
                "loaded " + std::to_string(g_repo_package_cache.size()) + " package records from the local index"
            );
        }
    }

    UpgradeContext context = build_upgrade_context(verbose);
    if (!context.upgrade_catalog_available) {
        repo_section.errors.push_back(
            context.upgrade_catalog_problem.empty()
                ? "validated upgrade catalog is unavailable; run 'gpkg update'"
                : context.upgrade_catalog_problem
        );
    } else {
        repo_section.ok.push_back("validated upgrade catalog is present and current");
    }
    std::vector<std::string>& registered_packages = context.registered_package_names;
    std::set<std::string>& exact_live_packages = context.exact_live_packages;
    install_section.ok.push_back("gpkg knows about " + std::to_string(registered_packages.size()) + " registered package(s)");
    if (exact_live_packages.size() != registered_packages.size()) {
        install_section.ok.push_back(
            "detected " + std::to_string(exact_live_packages.size()) + " exact live package(s) from gpkg and dpkg state"
        );
    }

    RepairInspection inspection = inspect_repair_state(registered_packages, verbose);
    if (inspection.detected_issues.empty()) {
        install_section.ok.push_back("installed package metadata and file lists look consistent");
    } else {
        install_section.errors.push_back(
            std::to_string(inspection.detected_issues.size()) + " installed-package issue(s) detected"
        );
        for (const auto& issue : inspection.detected_issues) {
            install_section.errors.push_back(issue);
        }
    }
    for (const auto& issue : inspection.unresolved_issues) {
        install_section.errors.push_back(issue);
    }

    PreparedUpgradeState prepared;
    bool have_valid_upgrade_plan = false;
    if (!repo_index_present || !g_repo_package_cache_loaded) {
        upgrade_section.errors.push_back("cannot build a dry-run upgrade plan until the local package index is available");
    } else if (!context.upgrade_catalog_available) {
        upgrade_section.errors.push_back(
            context.upgrade_catalog_problem.empty()
                ? "validated upgrade catalog is unavailable; run 'gpkg update'"
                : context.upgrade_catalog_problem
        );
    } else if (!prepare_upgrade_transaction(context, verbose, prepared)) {
        upgrade_section.errors.push_back(
            "gpkg could not build a safe upgrade plan; the next 'gpkg upgrade' would likely fail"
        );
    } else {
        have_valid_upgrade_plan = true;
    }

    for (const auto& warning : prepared.skipped_managed_packages) {
        upgrade_section.warnings.push_back(warning);
    }

    if (context.upgrade_catalog_available) {
        if (context.upgrade_catalog.skipped_entries.empty()) {
            upgrade_section.ok.push_back("validated upgrade catalog has no skipped configured runtime families");
        } else {
            for (const auto& entry : context.upgrade_catalog.skipped_entries) {
                upgrade_section.warnings.push_back(describe_upgrade_catalog_skip_entry(entry));
            }
        }
    }

    std::vector<BaseSystemRegistryEntry>& base_entries = context.base_entries;
    if (base_entries.empty()) {
        base_section.errors.push_back("base-system registry is missing or empty: " + BASE_SYSTEM_REGISTRY_PATH);
    } else {
        base_section.ok.push_back("loaded " + std::to_string(base_entries.size()) + " base-system registry entrie(s)");
    }

    size_t stale_base_entries = 0;
    size_t shadowed_base_entries = 0;
    for (const auto& entry : base_entries) {
        if (!base_system_registry_entry_looks_present(entry)) {
            ++stale_base_entries;
            base_section.warnings.push_back(
                entry.package + " is still recorded in the base-system registry, but none of its recorded files are present"
            );
            continue;
        }

        auto shadow_it = context.shadowed_base_alias_target.find(entry.package);
        if (shadow_it != context.shadowed_base_alias_target.end()) {
            ++shadowed_base_entries;
            base_section.warnings.push_back(
                entry.package + " is shadowed by live package family " + shadow_it->second
                + " and will not drive upgrades"
            );
        }
    }
    if (base_entries.empty()) {
        // already reported as an error above
    } else if (stale_base_entries == 0 && shadowed_base_entries == 0) {
        base_section.ok.push_back("base-system registry entries look consistent with the live system");
    }

    if (have_valid_upgrade_plan && prepared.upgrade_queue.empty()) {
        upgrade_section.ok.push_back("all packages are currently up to date");
    } else if (have_valid_upgrade_plan) {
        size_t installed_upgrades = 0;
        size_t installed_reinstalls = 0;
        size_t base_bootstraps = 0;
        for (const auto& entry : prepared.explicit_targets) {
            if (entry.was_installed && entry.reinstall_only) ++installed_reinstalls;
            else if (entry.was_installed) ++installed_upgrades;
            else ++base_bootstraps;
        }
        upgrade_section.ok.push_back(
            "dry-run upgrade plan is valid for " + std::to_string(prepared.upgrade_queue.size()) + " package(s)"
        );
        if (installed_upgrades > 0) {
            upgrade_section.ok.push_back(std::to_string(installed_upgrades) + " installed package(s) would be upgraded");
        }
        if (installed_reinstalls > 0) {
            upgrade_section.warnings.push_back(std::to_string(installed_reinstalls) + " package(s) would be reinstalled");
        }
        if (base_bootstraps > 0) {
            upgrade_section.warnings.push_back(
                std::to_string(base_bootstraps) + " base-system package(s) would be imported into gpkg during upgrade"
            );
        }
        if (!prepared.plan.retirements.empty()) {
            upgrade_section.warnings.push_back(
                std::to_string(prepared.plan.retirements.size()) + " replaced package(s) would be retired during upgrade"
            );
        }
    }

    std::vector<DoctorSection> sections = {
        repo_section,
        install_section,
        base_section,
        upgrade_section,
    };

    size_t warning_count = 0;
    size_t error_count = 0;
    std::cout << "gpkg doctor report:" << std::endl;
    for (const auto& section : sections) {
        print_doctor_section(section);
        warning_count += section.warnings.size();
        error_count += section.errors.size();
    }

    std::cout << "Summary: ";
    if (error_count == 0 && warning_count == 0) {
        std::cout << Color::GREEN << "healthy" << Color::RESET;
    } else if (error_count == 0) {
        std::cout << Color::YELLOW << "warnings detected" << Color::RESET;
    } else {
        std::cout << Color::RED << "problems detected" << Color::RESET;
    }
    std::cout << " (" << error_count << " error(s), " << warning_count << " warning(s))" << std::endl;

    return error_count == 0 ? 0 : 1;
}

int handle_repair(bool verbose) {
    if (!ensure_repo_index_available()) return 1;

    std::cout << "Inspecting installed packages..." << std::endl;
    std::cout << "Optional dependency policy: " << describe_optional_dependency_policy() << std::endl;
    RepairInspection inspection = inspect_repair_state(verbose);

    if (inspection.detected_issues.empty()) {
        std::cout << "No broken packages found." << std::endl;
        g_pending_triggers.insert("ldconfig");
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
        if (!update_package_auto_install_state_after_install(
                repair_queue[i].name,
                false,
                installed_set)) {
            if (!verbose) finish_progress_line(&install_progress_width);
            std::cerr << Color::RED << "E: Failed to update gpkg auto-install state for "
                      << repair_queue[i].name << Color::RESET << std::endl;
            failures.push_back(repair_queue[i].name);
            break;
        }
        std::vector<std::string> retirements;
        if (should_retire_after_install(repair_plan, repair_queue[i].name, retirements)) {
            for (const auto& retired_pkg : retirements) {
                std::vector<std::string> retired_files = read_installed_file_list(retired_pkg);
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
                check_triggers(retired_files);
                if (!erase_package_auto_installed_state(retired_pkg)) {
                    if (!verbose) finish_progress_line(&install_progress_width);
                    std::cerr << Color::RED << "E: Failed to update gpkg auto-install state for "
                              << retired_pkg << Color::RESET << std::endl;
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
        g_pending_triggers.insert("ldconfig");
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

bool maybe_report_unavailable_install_target(
    const std::string& requested_name,
    bool verbose
) {
    std::string canonical_name = canonicalize_package_name(requested_name, verbose);
    if (canonical_name.empty()) return false;

    RawDebianContext raw_context;
    PackageUniverseResult available_result;
    if (resolve_full_universe_relation_candidate(
            canonical_name,
            "",
            "",
            available_result,
            verbose,
            &raw_context
        )) {
        return false;
    }

    PackageUniverseResult exact_result;
    if (query_full_universe_exact_package(canonical_name, exact_result, verbose, &raw_context)) {
        if (exact_result.installable) return false;

        std::cerr << Color::YELLOW
                  << "Package " << requested_name << " is available, but it is not installable."
                  << Color::RESET << std::endl;
        if (exact_result.meta.name != canonical_name && !exact_result.meta.name.empty()) {
            std::cerr << "  Candidate: " << exact_result.meta.name << std::endl;
        }
        std::string origin = format_package_origin(exact_result.meta);
        if (!origin.empty()) {
            std::cerr << "  Source: " << origin << std::endl;
        }
        if (!exact_result.reason.empty()) {
            std::cerr << "  Reason: " << exact_result.reason << std::endl;
        }
        std::cerr << Color::RED
                  << "E: Package '" << requested_name << "' has no installation candidate."
                  << Color::RESET << std::endl;
        return true;
    }

    DebianSearchPreviewEntry preview;
    std::string preview_error;
    if (!get_debian_search_preview_exact_package(canonical_name, preview, verbose, &preview_error)) {
        return false;
    }
    if (preview.installable) return false;

    std::cerr << Color::YELLOW
              << "Package " << requested_name << " is available, but it is not installable."
              << Color::RESET << std::endl;
    if (preview.meta.name != canonical_name && !preview.meta.name.empty()) {
        std::cerr << "  Candidate: " << preview.meta.name << std::endl;
    }
    std::string origin = format_package_origin(preview.meta);
    if (!origin.empty()) {
        std::cerr << "  Source: " << origin << std::endl;
    }
    if (!preview.reason.empty()) {
        std::cerr << "  Reason: " << preview.reason << std::endl;
    }
    std::cerr << Color::RED
              << "E: Package '" << requested_name << "' has no installation candidate."
              << Color::RESET << std::endl;
    return true;
}

int handle_install(int argc, char* argv[], const std::set<std::string>& installed_cache, bool verbose) {
    std::vector<PackageMetadata> install_queue;
    std::vector<std::string> local_files;
    std::vector<std::string> repo_operands;
    std::vector<std::string> operands = collect_cli_operands(argc, argv, 2);
    std::set<std::string> visited;
    bool needs_repo_index = false;
    RawDebianContext raw_context;

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
            repo_operands.push_back(arg);
            needs_repo_index = true;
        }
    }

    if (needs_repo_index && !ensure_repo_index_available()) return 1;

    for (const auto& arg : operands) {
        if (arg.length() > 5 && arg.substr(arg.length() - 5) == ".gpkg" && access(arg.c_str(), F_OK) == 0) {
            continue;
        }

        if (maybe_report_unavailable_install_target(arg, verbose)) {
            return 1;
        }

        std::string failure_reason;
        if (!resolve_dependencies(
                arg,
                "",
                "",
                install_queue,
                visited,
                installed_cache,
                verbose,
                g_force_reinstall,
                nullptr,
                false,
                &raw_context,
                &failure_reason
            )) {
            std::cerr << Color::RED << "E: Failed to resolve dependencies for " << arg;
            if (!failure_reason.empty()) {
                std::cerr << " (" << failure_reason << ")";
            }
            std::cerr << Color::RESET << std::endl;
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
            if (!set_package_auto_installed_state(local_pkg_name, false)) {
                std::cerr << Color::RED << "E: Failed to update gpkg auto-install state for "
                          << local_pkg_name << Color::RESET << std::endl;
                return 1;
            }
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
                        std::vector<std::string> retired_files = read_installed_file_list(retired_pkg);
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
                        check_triggers(retired_files);
                        if (!erase_package_auto_installed_state(retired_pkg)) {
                            std::cerr << Color::RED << "E: Failed to update gpkg auto-install state for "
                                      << retired_pkg << Color::RESET << std::endl;
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

    std::set<std::string> explicit_manual_targets;
    for (const auto& arg : repo_operands) {
        std::string resolved = resolve_requested_package_for_manual_marking(
            arg,
            install_queue,
            installed_cache,
            verbose
        );
        if (!resolved.empty()) explicit_manual_targets.insert(resolved);
    }

    if (install_queue.empty()) {
        for (const auto& pkg_name : explicit_manual_targets) {
            if (!update_package_auto_install_state_after_install(pkg_name, true, installed_cache)) {
                std::cerr << Color::RED << "E: Failed to update gpkg auto-install state for "
                          << pkg_name << Color::RESET << std::endl;
                return 1;
            }
        }
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

    print_transaction_disk_change_summary(
        estimate_install_transaction_disk_change(
            install_queue,
            !install_plan.retirements.empty()
        )
    );

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
        if (!update_package_auto_install_state_after_install(
                install_queue[i].name,
                explicit_manual_targets.count(install_queue[i].name) != 0,
                installed_cache)) {
            if (!verbose) finish_progress_line(&install_progress_width);
            std::cerr << Color::RED << "E: Failed to update gpkg auto-install state for "
                      << install_queue[i].name << Color::RESET << std::endl;
            return 1;
        }
        std::vector<std::string> retirements;
        if (should_retire_after_install(install_plan, install_queue[i].name, retirements)) {
            for (const auto& retired_pkg : retirements) {
                std::vector<std::string> retired_files = read_installed_file_list(retired_pkg);
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
                check_triggers(retired_files);
                if (!erase_package_auto_installed_state(retired_pkg)) {
                    if (!verbose) finish_progress_line(&install_progress_width);
                    std::cerr << Color::RED << "E: Failed to update gpkg auto-install state for "
                              << retired_pkg << Color::RESET << std::endl;
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
        std::set<std::string> skipped_kernel_packages;
        std::map<std::string, std::string> skipped_protected_packages;
        std::vector<std::string> autoremove_packages = collect_autoremove_packages(
            removal_set,
            verbose,
            &skipped_kernel_packages,
            &skipped_protected_packages
        );
        for (const auto& pkg : autoremove_packages) {
            if (removal_set.insert(pkg).second) to_remove.push_back(pkg);
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

    return execute_removal_plan(to_remove, to_purge, verbose);
}

int handle_autoremove(bool verbose, bool purge) {
    std::cout << "Calculating removable packages..." << std::endl;

    std::set<std::string> skipped_kernel_packages;
    std::map<std::string, std::string> skipped_protected_packages;
    std::vector<std::string> to_remove = collect_autoremove_packages(
        {},
        verbose,
        &skipped_kernel_packages,
        &skipped_protected_packages
    );

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

    std::vector<std::string> to_purge;
    std::set<std::string> selected(to_remove.begin(), to_remove.end());
    if (purge) {
        to_purge = to_remove;
        std::vector<std::string> purge_only = collect_autoremove_purge_only_packages(selected, verbose);
        to_purge.insert(to_purge.end(), purge_only.begin(), purge_only.end());
        std::sort(to_purge.begin(), to_purge.end());
        to_purge.erase(std::unique(to_purge.begin(), to_purge.end()), to_purge.end());
    }

    return execute_removal_plan(to_remove, to_purge, verbose);
}
