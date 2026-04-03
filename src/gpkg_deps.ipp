// Dependency parsing and resolver logic.

struct Dependency {
    std::string name;
    std::string op;
    std::string version;
};

std::string find_provider(const std::string& capability, const std::string& op, const std::string& req_version, bool verbose);

bool get_installed_package_metadata(const std::string& pkg_name, PackageMetadata& out_meta) {
    std::ifstream f(INFO_DIR + pkg_name + ".json");
    if (!f) return false;

    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    out_meta = {};
    out_meta.name = pkg_name;
    get_json_value(content, "version", out_meta.version);
    get_json_value(content, "architecture", out_meta.arch);
    get_json_value(content, "maintainer", out_meta.maintainer);
    get_json_value(content, "description", out_meta.description);
    get_json_value(content, "section", out_meta.section);
    get_json_value(content, "priority", out_meta.priority);
    get_json_value(content, "filename", out_meta.filename);
    get_json_value(content, "sha256", out_meta.sha256);
    get_json_value(content, "sha512", out_meta.sha512);
    get_json_value(content, "source_kind", out_meta.source_kind);
    get_json_value(content, "source_url", out_meta.source_url);
    if (out_meta.source_url.empty()) get_json_value(content, "repo_url", out_meta.source_url);
    get_json_value(content, "debian_package", out_meta.debian_package);
    get_json_value(content, "debian_version", out_meta.debian_version);
    get_json_value(content, "package_scope", out_meta.package_scope);
    get_json_value(content, "installed_from", out_meta.installed_from);
    get_json_value(content, "size", out_meta.size);
    get_json_value(content, "installed_size_bytes", out_meta.installed_size_bytes);
    get_json_array(content, "pre_depends", out_meta.pre_depends);
    get_json_array(content, "depends", out_meta.depends);
    get_json_array(content, "recommends", out_meta.recommends);
    get_json_array(content, "suggests", out_meta.suggests);
    get_json_array(content, "breaks", out_meta.breaks);
    get_json_array(content, "conflicts", out_meta.conflicts);
    get_json_array(content, "provides", out_meta.provides);
    get_json_array(content, "replaces", out_meta.replaces);
    return !out_meta.version.empty() || !content.empty();
}

bool get_live_installed_package_metadata(const std::string& pkg_name, PackageMetadata& out_meta) {
    auto has_comparable_live_version = [](const std::string& version) {
        return native_dpkg_version_is_exact(version);
    };

    PackageStatusRecord status_record;
    bool have_live_status =
        get_dpkg_package_status_record(pkg_name, &status_record) &&
        package_status_is_installed_like(status_record.status);
    if (!have_live_status) {
        have_live_status =
            get_base_system_package_status_record(pkg_name, &status_record) &&
            package_status_is_installed_like(status_record.status);
    }

    PackageMetadata installed_meta;
    bool have_installed_meta = get_installed_package_metadata(pkg_name, installed_meta);
    bool have_exact_live_version = has_comparable_live_version(status_record.version);
    bool installed_relations_exact =
        have_exact_live_version &&
        have_installed_meta &&
        package_metadata_relations_match_version_exactly(installed_meta, status_record.version);

    if (have_live_status) {
        PackageMetadata repo_meta;
        bool have_repo_meta = get_repo_package_info(pkg_name, repo_meta);
        bool repo_relations_exact =
            have_exact_live_version &&
            have_repo_meta &&
            package_metadata_relations_match_version_exactly(repo_meta, status_record.version);

        PackageMetadata meta = build_minimal_live_package_metadata(pkg_name, status_record.version);
        if (repo_relations_exact) {
            meta = repo_meta;
        } else if (installed_relations_exact) {
            meta = installed_meta;
        }

        meta.name = pkg_name;
        meta.version = status_record.version;
        if (meta.version.empty()) return false;

        if (have_installed_meta) {
            overlay_missing_package_metadata_descriptive_fields(meta, installed_meta);
        }
        if (have_repo_meta) {
            overlay_missing_package_metadata_descriptive_fields(meta, repo_meta);
            if (have_exact_live_version &&
                !repo_relations_exact &&
                !installed_relations_exact) {
                overlay_missing_package_metadata_relations(meta, repo_meta, true);
            }
        }

        out_meta = meta;
        return true;
    }

    if (have_installed_meta) {
        out_meta = installed_meta;
        return true;
    }

    return false;
}

Dependency parse_dependency(const std::string& dep_str) {
    Dependency dep;

    RelationAtom normalized = normalize_relation_atom(dep_str, "any");
    if (normalized.valid) {
        dep.name = normalized.name;
        dep.op = normalized.op;
        dep.version = normalized.version;
        return dep;
    }

    size_t open_paren = dep_str.find('(');
    if (open_paren == std::string::npos) {
        dep.name = trim(dep_str);
        return dep;
    }

    dep.name = trim(dep_str.substr(0, open_paren));
    size_t close_paren = dep_str.find(')', open_paren);
    if (close_paren == std::string::npos) return dep;

    std::string content = trim(dep_str.substr(open_paren + 1, close_paren - open_paren - 1));
    const std::vector<std::string> ops = {">=", "<=", "<<", ">>", "==", "=", ">", "<"};
    for (const auto& op : ops) {
        if (content.substr(0, op.length()) == op) {
            dep.op = op;
            dep.version = trim(content.substr(op.length()));
            break;
        }
    }

    return dep;
}

bool package_name_is_runtime_bootstrap_first(const std::string& pkg_name, bool verbose) {
    static const std::set<std::string> bootstrap_first = {
        "libc6",
        "libc-bin",
        "libc-gconv-modules-extra",
        "libgcc-s1",
        "libstdc++6",
        "libcrypt1",
        "zlib1g",
        "libzstd1",
    };

    std::set<std::string> family_names;
    std::string canonical = canonicalize_package_name(pkg_name, verbose);
    family_names.insert(canonical);
    std::string legacy = derive_debian_t64_legacy_alias(canonical);
    if (!legacy.empty()) {
        family_names.insert(legacy);
    } else if (canonical.rfind("lib", 0) == 0) {
        family_names.insert(canonical + "t64");
    }

    for (const auto& family_name : family_names) {
        if (bootstrap_first.count(family_name) != 0) return true;
    }
    return false;
}

bool package_name_is_shell_runtime_next(const std::string& pkg_name, bool verbose) {
    static const std::set<std::string> shell_runtime_next = {
        "libtinfo6",
        "libncursesw6",
        "libreadline8t64",
        "bash",
        "dash",
    };

    std::set<std::string> family_names;
    std::string canonical = canonicalize_package_name(pkg_name, verbose);
    family_names.insert(canonical);
    std::string legacy = derive_debian_t64_legacy_alias(canonical);
    if (!legacy.empty()) {
        family_names.insert(legacy);
    } else if (canonical.rfind("lib", 0) == 0) {
        family_names.insert(canonical + "t64");
    }

    for (const auto& family_name : family_names) {
        if (shell_runtime_next.count(family_name) != 0) return true;
    }
    return false;
}

int package_runtime_bootstrap_rank(const std::string& pkg_name, bool verbose) {
    if (package_name_is_runtime_bootstrap_first(pkg_name, verbose)) return 0;
    if (package_name_is_shell_runtime_next(pkg_name, verbose)) return 1;
    return 2;
}

bool version_satisfies(const std::string& current_ver, const std::string& op, const std::string& req_ver) {
    if (op.empty()) return true;

    int cmp = compare_versions(current_ver, req_ver);
    if (op == ">=" && cmp >= 0) return true;
    if (op == "<=" && cmp <= 0) return true;
    if (op == ">"  && cmp > 0) return true;
    if (op == "<"  && cmp < 0) return true;
    if (op == ">>" && cmp > 0) return true;
    if (op == "<<" && cmp < 0) return true;
    if ((op == "=" || op == "==") && cmp == 0) return true;
    return false;
}

std::vector<std::string> load_dependency_entries(const std::string& path) {
    std::vector<std::string> entries;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        entries.push_back(line);
    }
    return entries;
}

const std::vector<std::string>& load_system_provides() {
    const ImportPolicy& policy = get_import_policy();
    if (!policy.system_provides.empty()) return policy.system_provides;

    static const std::vector<std::string> legacy_system_provides =
        load_dependency_entries(SYSTEM_PROVIDES_PATH);
    return legacy_system_provides;
}

const std::vector<std::string>& load_upgradeable_system_packages() {
    const ImportPolicy& policy = get_import_policy();
    if (!policy.upgradeable_system.empty()) return policy.upgradeable_system;

    static const std::vector<std::string> legacy_upgradeable_system =
        load_dependency_entries(UPGRADEABLE_SYSTEM_PATH);
    return legacy_upgradeable_system;
}

bool dependency_list_matches(
    const std::vector<std::string>& entries,
    const std::string& pkg,
    const std::string& op = "",
    const std::string& req_version = ""
) {
    for (const auto& entry : entries) {
        Dependency dep = parse_dependency(entry);
        if (dep.name != pkg) continue;
        if (op.empty()) return true;
        if (dep.version.empty()) continue;
        if (version_satisfies(dep.version, op, req_version)) return true;
    }
    return false;
}

bool is_system_provided(const std::string& pkg, const std::string& op, const std::string& req_version) {
    bool claimed_by_policy =
        dependency_list_matches(load_system_provides(), pkg) ||
        dependency_list_matches(load_upgradeable_system_packages(), pkg);
    if (!claimed_by_policy) return false;
    return system_package_has_live_evidence(pkg, op, req_version);
}

bool is_upgradeable_system_package(const std::string& pkg) {
    if (!dependency_list_matches(load_upgradeable_system_packages(), pkg)) return false;
    return system_package_has_live_evidence(pkg);
}

bool package_is_base_system_provided(const std::string& pkg_name, std::string* reason_out) {
    if (reason_out) reason_out->clear();
    if (pkg_name.empty() || !is_system_provided(pkg_name)) return false;

    if (reason_out) {
        if (is_upgradeable_system_package(pkg_name)) {
            *reason_out = "it is provided by the GeminiOS base system image and managed as an upgradeable system package";
        } else {
            *reason_out = "it is provided by the GeminiOS base system image";
        }
    }
    return true;
}

bool package_metadata_satisfies_dependency(
    const std::string& package_name,
    const PackageMetadata& meta,
    const Dependency& dep
) {
    auto has_comparable_version = [&](const std::string& version) {
        return native_dpkg_version_is_exact(version);
    };

    std::string canonical_package = canonicalize_package_name(package_name);
    std::string canonical_dep = canonicalize_package_name(dep.name);
    if (canonical_package == canonical_dep &&
        (dep.op.empty() || has_comparable_version(meta.version)) &&
        version_satisfies(meta.version, dep.op, dep.version)) {
        return true;
    }

    for (const auto& provided : meta.provides) {
        Dependency provided_dep = parse_dependency(provided);
        if (canonicalize_package_name(provided_dep.name) != canonical_dep) continue;
        if (dep.op.empty()) return true;
        if (!provided_dep.version.empty() &&
            version_satisfies(provided_dep.version, dep.op, dep.version)) {
            return true;
        }
    }

    return false;
}

bool dependency_matches_conflicting_exact_live_base_alias(
    const Dependency& dep,
    const std::string& candidate_name,
    const PackageMetadata& candidate_meta,
    UpgradeContext* context
) {
    if (!context) return false;
    if (dep.name.empty() || !dep.op.empty() || !dep.version.empty()) return false;

    std::string canonical_dep = canonicalize_package_name(dep.name);
    std::string canonical_candidate = canonicalize_package_name(candidate_name);
    if (canonical_dep.empty() || canonical_candidate.empty() || canonical_dep == canonical_candidate) {
        return false;
    }

    if (package_has_exact_live_install_state(canonical_dep, nullptr, context)) return false;

    auto base_it = context->base_presence_by_package.find(canonical_dep);
    if (base_it == context->base_presence_by_package.end() || !base_it->second) return false;

    if (!package_has_exact_live_install_state(canonical_candidate, nullptr, context)) return false;

    for (const auto& relation : candidate_meta.conflicts) {
        Dependency conflict_dep = parse_dependency(relation);
        if (canonicalize_package_name(conflict_dep.name) != canonical_dep) continue;
        if (conflict_dep.op.empty()) return true;
    }

    return false;
}

bool queued_candidate_satisfies_dependency(
    const PackageMetadata& meta,
    const Dependency& dep
) {
    return package_metadata_satisfies_dependency(meta.name, meta, dep);
}

std::map<std::string, std::vector<std::string>> load_upgrade_companions();

std::map<std::string, std::vector<std::string>> get_planner_upgrade_companion_map(
    UpgradeContext* context,
    bool verbose
) {
    if (context && context->upgrade_catalog_available) {
        return context->upgrade_catalog.resolved_companions;
    }
    (void)verbose;
    return load_upgrade_companions();
}

bool find_installed_dependency_provider(
    const Dependency& dep,
    const std::set<std::string>& installed_cache,
    std::string* provider_out = nullptr,
    UpgradeContext* context = nullptr,
    bool verbose = false,
    bool prefer_native_dpkg = false
) {
    if (provider_out) provider_out->clear();

    auto provider_matches_preferred_backend = [&](const std::string& provider_name) {
        if (!prefer_native_dpkg) return true;
        if (provider_name.empty() || provider_name == BASE_SYSTEM_PROVIDER) return true;

        PackageStatusRecord dpkg_record;
        if (get_dpkg_package_status_record(provider_name, &dpkg_record) &&
            package_status_is_installed_like(dpkg_record.status)) {
            return true;
        }

        PackageMetadata provider_meta;
        bool have_meta = context
            ? get_context_live_installed_package_metadata(*context, provider_name, provider_meta)
            : get_live_installed_package_metadata(provider_name, provider_meta);
        if (!have_meta) return false;

        return package_is_debian_source(provider_meta) || !provider_meta.debian_package.empty();
    };

    if (context) {
        std::string normalized_name = normalize_upgrade_root_name(dep.name, *context, verbose);
        if (!normalized_name.empty()) {
            PackageMetadata normalized_meta;
            if (get_context_live_installed_package_metadata(*context, normalized_name, normalized_meta) &&
                package_metadata_satisfies_dependency(normalized_name, normalized_meta, dep) &&
                provider_matches_preferred_backend(normalized_name)) {
                if (provider_out) *provider_out = normalized_name;
                return true;
            }
        }

        std::set<std::string> live_candidates = context->exact_live_packages;
        live_candidates.insert(
            context->present_base_packages.begin(),
            context->present_base_packages.end()
        );
        std::string best_name;
        PackageMetadata best_meta;
        for (const auto& candidate_name : live_candidates) {
            if (candidate_name.empty()) continue;

            PackageMetadata candidate_meta;
            if (!get_context_live_installed_package_metadata(*context, candidate_name, candidate_meta)) continue;
            bool satisfies_dependency =
                package_metadata_satisfies_dependency(candidate_name, candidate_meta, dep);
            bool shadows_base_alias =
                dependency_matches_conflicting_exact_live_base_alias(
                    dep,
                    candidate_name,
                    candidate_meta,
                    context
                );
            if (!satisfies_dependency && !shadows_base_alias) continue;
            if (!provider_matches_preferred_backend(candidate_name)) continue;

            if (best_name.empty() || should_prefer_repo_candidate(candidate_meta, best_meta)) {
                best_name = candidate_name;
                best_meta = candidate_meta;
            }
        }
        if (!best_name.empty()) {
            if (provider_out) *provider_out = best_name;
            return true;
        }
    }

    for (const auto& installed_name : installed_cache) {
        PackageMetadata meta;
        if (!get_live_installed_package_metadata(installed_name, meta)) continue;
        if (!package_metadata_satisfies_dependency(installed_name, meta, dep)) continue;
        if (!provider_matches_preferred_backend(installed_name)) continue;
        if (provider_out) *provider_out = installed_name;
        return true;
    }

    return false;
}

bool dependency_requires_exact_live_system_version(const Dependency& dep) {
    if (dep.name.empty() || dep.op.empty()) return false;
    return is_upgradeable_system_package(dep.name);
}

bool is_dependency_satisfied_locally(
    const Dependency& dep,
    const std::set<std::string>& installed_cache,
    bool verbose,
    std::string* provider_out = nullptr,
    UpgradeContext* context = nullptr,
    RawDebianContext* raw_context = nullptr,
    bool prefer_native_dpkg = false
) {
    if (provider_out) provider_out->clear();
    (void)raw_context;

    bool require_exact_live_system_version =
        dependency_requires_exact_live_system_version(dep);
    bool has_exact_live_version = false;
    if (require_exact_live_system_version) {
        has_exact_live_version =
            package_has_exact_live_install_state(dep.name, nullptr, context);
    }

    std::string installed_ver;
    if ((!require_exact_live_system_version || has_exact_live_version) &&
        get_local_installed_package_version(dep.name, &installed_ver, context) &&
        version_satisfies(installed_ver, dep.op, dep.version)) {
        if (!prefer_native_dpkg) {
            if (provider_out) *provider_out = dep.name;
            return true;
        }

        PackageStatusRecord dpkg_record;
        if (get_dpkg_package_status_record(dep.name, &dpkg_record) &&
            package_status_is_installed_like(dpkg_record.status)) {
            if (provider_out) *provider_out = dep.name;
            return true;
        }
    }

    std::string provider_name;
    if (find_installed_dependency_provider(
            dep,
            installed_cache,
            &provider_name,
            context,
            verbose,
            prefer_native_dpkg
        )) {
        if (provider_out) *provider_out = provider_name;
        return true;
    }

    if ((!require_exact_live_system_version || has_exact_live_version) &&
        is_system_provided(dep.name, dep.op, dep.version)) {
        if (is_upgradeable_system_package(dep.name)) {
            VLOG(verbose, dep.name
                 << " is provided by the GeminiOS base image and remains satisfied locally"
                 << " unless an explicit upgrade or versioned dependency requires a repo candidate.");
        }
        if (provider_out) *provider_out = BASE_SYSTEM_PROVIDER;
        return true;
    }

    if (verbose && !dep.op.empty() &&
        get_local_installed_package_version(dep.name, &installed_ver, context)) {
        VLOG(verbose, dep.name << " is installed as " << installed_ver
             << " but does not satisfy " << dep.op << " " << dep.version);
    }

    return false;
}

std::string find_provider(const std::string& capability, const std::string& op, const std::string& req_version, bool verbose) {
    std::string result;
    PackageMetadata best_meta;
    bool found = false;
    const auto* providers = get_repo_provider_candidates(capability, verbose);
    if (!providers) return result;

    Dependency requested_dep{capability, op, req_version};
    for (const auto& provider_name : *providers) {
        PackageMetadata candidate;
        if (!get_repo_package_info(provider_name, candidate)) continue;
        if (!package_metadata_satisfies_dependency(provider_name, candidate, requested_dep)) continue;

        if (!found || should_prefer_repo_candidate(candidate, best_meta)) {
            best_meta = candidate;
            result = candidate.name;
            found = true;
        }
    }
    if (found) {
        VLOG(verbose, "Found provider for " << capability
             << (op.empty() ? "" : (" (" + op + " " + req_version + ")"))
             << ": " << result);
    }
    return result;
}

bool resolve_dependencies_legacy(
    const std::string& pkg,
    const std::string& op,
    const std::string& req_version,
    std::vector<PackageMetadata>& install_queue,
    std::set<std::string>& visited,
    const std::set<std::string>& installed_cache,
    bool verbose,
    bool force_queue_requested = false,
    UpgradeContext* context = nullptr,
    bool suppress_errors = false,
    RawDebianContext* raw_context = nullptr,
    std::string* failure_reason_out = nullptr,
    bool prefer_native_dpkg = false
) {
    if (failure_reason_out) failure_reason_out->clear();
    std::string canonical_pkg = canonicalize_package_name(pkg, verbose);
    Dependency requested_dep{canonical_pkg, op, req_version};

    if (visited.count(canonical_pkg)) {
        for (const auto& queued : install_queue) {
            if (queued.name != canonical_pkg) continue;
            if (!queued_candidate_satisfies_dependency(queued, requested_dep)) {
                if (!suppress_errors) {
                    std::cerr << Color::RED << "E: Dependency conflict! " << canonical_pkg << " " << queued.version
                              << " is queued, but " << op << " " << req_version
                              << " is required." << Color::RESET << std::endl;
                }
                return false;
            }
            return true;
        }
        return true;
    }

    VLOG(verbose, "Resolving dependencies for: " << canonical_pkg
         << (op.empty() ? "" : (" (" + op + " " + req_version + ")")));

    std::string provider_name;
    if (!force_queue_requested &&
        is_dependency_satisfied_locally(
            requested_dep,
            installed_cache,
            verbose,
            &provider_name,
            context,
            raw_context,
            prefer_native_dpkg
        )) {
        if (provider_name == BASE_SYSTEM_PROVIDER) {
            VLOG(verbose, canonical_pkg << " is satisfied by the base-system policy.");
        } else if (provider_name == canonical_pkg) {
            std::string installed_ver;
            get_local_installed_package_version(canonical_pkg, &installed_ver, context);
            VLOG(verbose, canonical_pkg << " " << installed_ver << " is installed and satisfies constraints.");
        } else {
            VLOG(verbose, canonical_pkg << " is provided by installed package " << provider_name);
        }
        return true;
    }

    std::string installed_ver;
    if (!force_queue_requested &&
        package_has_exact_live_install_state(canonical_pkg, &installed_ver, context)) {
        bool acceptable_exact_live_state = true;
        if (prefer_native_dpkg) {
            PackageStatusRecord dpkg_record;
            acceptable_exact_live_state =
                get_dpkg_package_status_record(canonical_pkg, &dpkg_record) &&
                package_status_is_installed_like(dpkg_record.status);
        }

        if (acceptable_exact_live_state &&
            version_satisfies(installed_ver, op, req_version)) {
            VLOG(verbose, canonical_pkg << " " << installed_ver << " is installed and satisfies constraints.");
            return true;
        }

        if (!suppress_errors && acceptable_exact_live_state) {
            std::cerr << Color::YELLOW << "W: " << canonical_pkg << " " << installed_ver
                      << " is installed but does not meet requirements (" << op
                      << " " << req_version << ")." << Color::RESET << std::endl;
        }
    }

    if (is_blocked_import_package(canonical_pkg, verbose)) {
        if (failure_reason_out) {
            *failure_reason_out = "package is blocked by GeminiOS import policy";
        }
        if (!suppress_errors) {
            std::cerr << Color::RED << "E: Package " << canonical_pkg
                      << " is blocked by GeminiOS import policy." << Color::RESET << std::endl;
        }
        return false;
    }

    PackageMetadata meta;
    PackageUniverseResult universe_result;
    if (!resolve_full_universe_relation_candidate(
            canonical_pkg,
            op,
            req_version,
            universe_result,
            verbose,
            raw_context,
            nullptr,
            prefer_native_dpkg
        )) {
        if (failure_reason_out && failure_reason_out->empty()) {
            *failure_reason_out = universe_result.reason.empty()
                ? "package was not found in the local package universe"
                : universe_result.reason;
        }

        if (!suppress_errors) {
            std::string message = "Unable to locate package " + canonical_pkg;
            if (failure_reason_out && !failure_reason_out->empty()) {
                message += " (" + *failure_reason_out + ")";
            }
            std::cerr << Color::RED << "E: " << message << Color::RESET << std::endl;
        }
        return false;
    }

    meta = universe_result.meta;
    if (meta.name != canonical_pkg &&
        package_metadata_satisfies_dependency(meta.name, meta, requested_dep)) {
        VLOG(verbose, "Redirecting " << canonical_pkg << " -> " << meta.name
                     << " from the "
                     << (universe_result.raw_only ? "full cached Debian" : "repository")
                     << " universe");
        return resolve_dependencies_legacy(
            meta.name,
            "",
            "",
            install_queue,
            visited,
            installed_cache,
            verbose,
            force_queue_requested,
            context,
            suppress_errors,
            raw_context,
            failure_reason_out,
            prefer_native_dpkg
        );
    }

    if (!queued_candidate_satisfies_dependency(meta, requested_dep)) {
        if (failure_reason_out) {
            *failure_reason_out = "candidate version " + meta.version +
                " does not satisfy " + op + " " + req_version;
        }
        if (!suppress_errors) {
            std::cerr << Color::RED << "E: Package " << canonical_pkg << " found (v" << meta.version
                      << ") but does not meet requirements (" << op << " " << req_version
                      << ")" << Color::RESET << std::endl;
        }
        return false;
    }

    VLOG(verbose, "Found " << canonical_pkg << " in repository (version: " << meta.version << ")");
    if (verbose) {
        if (!meta.pre_depends.empty()) {
            VLOG(verbose, canonical_pkg << " pre-depends on: " << join_strings(meta.pre_depends));
        }
        if (!meta.depends.empty()) {
            VLOG(verbose, canonical_pkg << " depends on: " << join_strings(meta.depends));
        }
        if (!meta.recommends.empty()) {
            VLOG(verbose, canonical_pkg
                 << (should_include_recommends_for_transaction(meta)
                        ? " includes recommends: "
                        : " skips recommends: ")
                 << join_strings(meta.recommends));
        }
        if (!meta.suggests.empty()) {
            VLOG(verbose, canonical_pkg
                 << (should_include_suggests_for_transaction(meta)
                        ? " includes suggests: "
                        : " skips suggests: ")
                 << join_strings(meta.suggests));
        }
    }

    visited.insert(canonical_pkg);
    bool child_prefer_native_dpkg = prefer_native_dpkg || package_is_debian_source(meta);
    for (const auto& edge : collect_transaction_dependency_edge_details(meta)) {
        Dependency dep = parse_dependency(edge.relation);
        if (dep.name.empty()) continue;

        if (!transaction_dependency_is_optional(edge.kind)) {
            if (!resolve_dependencies_legacy(
                    dep.name,
                    dep.op,
                    dep.version,
                    install_queue,
                    visited,
                    installed_cache,
                    verbose,
                    false,
                    context,
                    suppress_errors,
                    raw_context,
                    failure_reason_out,
                    child_prefer_native_dpkg
                )) {
                return false;
            }
            continue;
        }

        auto optional_queue = install_queue;
        auto optional_visited = visited;
        if (!resolve_dependencies_legacy(
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
                raw_context,
                nullptr,
                child_prefer_native_dpkg
            )) {
            VLOG(
                verbose,
                "Skipping optional " << transaction_dependency_kind_label(edge.kind)
                    << " dependency " << edge.relation << " for " << canonical_pkg
                    << " because no importable candidate is available."
            );
            continue;
        }

        install_queue.swap(optional_queue);
        visited.swap(optional_visited);
    }

    VLOG(verbose, "Adding " << canonical_pkg << " to installation queue.");
    install_queue.push_back(meta);
    return true;
}

bool resolve_dependencies(
    const std::string& pkg,
    const std::string& op,
    const std::string& req_version,
    std::vector<PackageMetadata>& install_queue,
    std::set<std::string>& visited,
    const std::set<std::string>& installed_cache,
    bool verbose,
    bool force_queue_requested = false,
    UpgradeContext* context = nullptr,
    bool suppress_errors = false,
    RawDebianContext* raw_context = nullptr,
    std::string* failure_reason_out = nullptr,
    bool prefer_native_dpkg = false
) {
    DebianBackendSelection backend = select_debian_backend(
        DebianBackendOperation::ResolveDependencies,
        verbose
    );
    maybe_log_debian_backend_selection(backend, DebianBackendOperation::ResolveDependencies, verbose);
    return resolve_dependencies_legacy(
        pkg,
        op,
        req_version,
        install_queue,
        visited,
        installed_cache,
        verbose,
        force_queue_requested,
        context,
        suppress_errors,
        raw_context,
        failure_reason_out,
        prefer_native_dpkg
    );
}

struct TransactionRetirement {
    std::string installed_name;
    std::string replacement_name;
};

struct TransactionPlan {
    std::vector<PackageMetadata> install_queue;
    std::vector<TransactionRetirement> retirements;
};

Dependency parse_relation_constraint(const std::string& relation) {
    Dependency dep = parse_dependency(relation);
    dep.name = canonicalize_package_name(dep.name);
    return dep;
}

bool relation_matches_package(
    const std::string& relation,
    const std::string& pkg_name,
    const PackageMetadata* pkg_meta = nullptr
) {
    Dependency dep = parse_relation_constraint(relation);
    if (dep.name.empty()) return false;

    if (pkg_meta) {
        return package_metadata_satisfies_dependency(pkg_name, *pkg_meta, dep);
    }

    if (!dep.op.empty()) return false;
    return canonicalize_package_name(pkg_name) == dep.name;
}

bool relation_matches_concrete_package(
    const std::string& relation,
    const std::string& pkg_name,
    const PackageMetadata* pkg_meta = nullptr
) {
    Dependency dep = parse_relation_constraint(relation);
    if (dep.name.empty()) return false;
    if (canonicalize_package_name(pkg_name) != dep.name) return false;
    if (!pkg_meta) return dep.op.empty();
    if (!dep.op.empty()) {
        std::string normalized_version = trim(pkg_meta->version);
        if (!native_dpkg_version_is_exact(normalized_version)) {
            return false;
        }
    }
    return version_satisfies(pkg_meta->version, dep.op, dep.version);
}

bool relation_list_matches_package(
    const std::vector<std::string>& relations,
    const std::string& pkg_name,
    const PackageMetadata* pkg_meta = nullptr
) {
    for (const auto& relation : relations) {
        if (relation_matches_package(relation, pkg_name, pkg_meta)) return true;
    }
    return false;
}

bool relation_list_matches_concrete_package(
    const std::vector<std::string>& relations,
    const std::string& pkg_name,
    const PackageMetadata* pkg_meta = nullptr
) {
    for (const auto& relation : relations) {
        if (relation_matches_concrete_package(relation, pkg_name, pkg_meta)) return true;
    }
    return false;
}

bool package_conflicts_with_package(
    const PackageMetadata& meta,
    const std::string& pkg_name,
    const PackageMetadata* pkg_meta = nullptr
) {
    return relation_list_matches_package(meta.conflicts, pkg_name, pkg_meta);
}

bool package_breaks_package(
    const PackageMetadata& meta,
    const std::string& pkg_name,
    const PackageMetadata* pkg_meta = nullptr
) {
    return relation_list_matches_concrete_package(meta.breaks, pkg_name, pkg_meta);
}

bool package_replaces_package(
    const PackageMetadata& meta,
    const std::string& pkg_name,
    const PackageMetadata* pkg_meta = nullptr
) {
    return relation_list_matches_concrete_package(meta.replaces, pkg_name, pkg_meta);
}

void sort_transaction_queue_for_install(
    std::vector<PackageMetadata>& queue,
    bool verbose,
    UpgradeContext* context = nullptr
) {
    if (queue.size() < 2) return;

    const size_t n = queue.size();
    std::vector<std::set<size_t>> outgoing(n);
    std::vector<std::set<size_t>> incoming(n);
    std::vector<size_t> indegree(n, 0);

    auto add_edge = [&](size_t from, size_t to) {
        if (from == to) return;
        if (outgoing[from].insert(to).second) {
            incoming[to].insert(from);
            ++indegree[to];
        }
    };

    for (size_t dependent = 0; dependent < n; ++dependent) {
        for (const auto& dep_str : collect_required_transaction_dependency_edges(queue[dependent])) {
            Dependency dep = parse_dependency(dep_str);
            if (dep.name.empty()) continue;

            int provider_index = -1;
            for (size_t candidate = 0; candidate < n; ++candidate) {
                if (candidate == dependent) continue;
                if (!queued_candidate_satisfies_dependency(queue[candidate], dep)) continue;
                provider_index = static_cast<int>(candidate);
                break;
            }

            if (provider_index >= 0) {
                add_edge(static_cast<size_t>(provider_index), dependent);
            }
        }
    }

    std::set<size_t> bootstrap_closure;
    std::vector<size_t> bootstrap_stack;
    for (size_t i = 0; i < n; ++i) {
        if (!package_name_is_runtime_bootstrap_first(queue[i].name, verbose)) continue;
        if (bootstrap_closure.insert(i).second) bootstrap_stack.push_back(i);
    }
    while (!bootstrap_stack.empty()) {
        size_t current = bootstrap_stack.back();
        bootstrap_stack.pop_back();
        for (size_t prerequisite : incoming[current]) {
            if (bootstrap_closure.insert(prerequisite).second) {
                bootstrap_stack.push_back(prerequisite);
            }
        }
    }

    std::map<std::string, std::vector<std::string>> companion_map =
        get_planner_upgrade_companion_map(context, verbose);
    std::map<std::string, std::set<std::string>> reverse_companions;
    for (const auto& entry : companion_map) {
        std::string trigger = canonicalize_package_name(entry.first, verbose);
        for (const auto& companion_token : entry.second) {
            Dependency dep = parse_dependency(companion_token);
            if (dep.name.empty()) continue;
            reverse_companions[canonicalize_package_name(dep.name, verbose)].insert(trigger);
        }
    }

    std::vector<PackageMetadata> ordered;
    ordered.reserve(n);
    std::vector<bool> emitted(n, false);
    std::set<std::string> completed_names;

    auto bootstrap_rank = [&](size_t index) {
        int runtime_rank = package_runtime_bootstrap_rank(queue[index].name, verbose);
        if (runtime_rank < 2) return runtime_rank;

        std::string priority = trim(queue[index].priority);
        std::transform(priority.begin(), priority.end(), priority.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (priority == "required") return 2;
        if (priority == "important") return 3;
        return 4;
    };

    auto candidate_priority = [&](size_t index) {
        std::string canonical_name = canonicalize_package_name(queue[index].name, verbose);
        auto it = reverse_companions.find(canonical_name);
        if (it == reverse_companions.end()) return 1;
        for (const auto& trigger : it->second) {
            if (completed_names.count(trigger) != 0) return 0;
        }
        return 1;
    };

    auto candidate_phase = [&](size_t index) {
        if (bootstrap_closure.count(index) != 0) return 0;

        if (package_name_is_shell_runtime_next(queue[index].name, verbose) &&
            completed_names.count("libc6") == 0) {
            // Do not let shell-linked runtime libraries run ahead of libc.
            return 2;
        }
        return 1;
    };

    for (size_t emitted_count = 0; emitted_count < n; ++emitted_count) {
        size_t best_index = n;
        int best_phase = 3;
        int best_priority = 2;
        int best_bootstrap_rank = 5;
        for (size_t i = 0; i < n; ++i) {
            if (emitted[i] || indegree[i] != 0) continue;
            int phase = candidate_phase(i);
            int priority = candidate_priority(i);
            int rank = bootstrap_rank(i);
            bool better = best_index == n ||
                phase < best_phase ||
                (phase == best_phase &&
                 (priority < best_priority ||
                  (priority == best_priority &&
                   (rank < best_bootstrap_rank ||
                    (rank == best_bootstrap_rank && i < best_index)))));
            if (better) {
                best_index = i;
                best_phase = phase;
                best_priority = priority;
                best_bootstrap_rank = rank;
            }
        }

        if (best_index == n) {
            size_t best_cycle_index = n;
            int best_cycle_phase = 3;
            int best_cycle_priority = 2;
            int best_cycle_rank = 5;
            size_t best_cycle_indegree = std::numeric_limits<size_t>::max();

            for (size_t i = 0; i < n; ++i) {
                if (emitted[i]) continue;
                int phase = candidate_phase(i);
                int priority = candidate_priority(i);
                int rank = bootstrap_rank(i);
                size_t current_indegree = indegree[i];
                bool better_cycle = best_cycle_index == n ||
                    phase < best_cycle_phase ||
                    (phase == best_cycle_phase &&
                     (priority < best_cycle_priority ||
                      (priority == best_cycle_priority &&
                       (rank < best_cycle_rank ||
                        (rank == best_cycle_rank &&
                         (current_indegree < best_cycle_indegree ||
                          (current_indegree == best_cycle_indegree && i < best_cycle_index)))))));
                if (better_cycle) {
                    best_cycle_index = i;
                    best_cycle_phase = phase;
                    best_cycle_priority = priority;
                    best_cycle_rank = rank;
                    best_cycle_indegree = current_indegree;
                }
            }

            if (best_cycle_index == n) {
                VLOG(verbose, "Dependency ordering aborted because no install candidate remained.");
                return;
            }

            VLOG(verbose,
                 "Breaking dependency-order ambiguity by scheduling "
                 << queue[best_cycle_index].name
                 << " next (phase=" << best_cycle_phase
                 << ", priority=" << best_cycle_priority
                 << ", bootstrap-rank=" << best_cycle_rank
                 << ", indegree=" << best_cycle_indegree << ").");
            best_index = best_cycle_index;
        }

        emitted[best_index] = true;
        ordered.push_back(queue[best_index]);
        completed_names.insert(canonicalize_package_name(queue[best_index].name, verbose));
        for (size_t successor : outgoing[best_index]) {
            if (indegree[successor] > 0) --indegree[successor];
        }
    }

    queue.swap(ordered);
}

bool build_transaction_plan_legacy(
    const std::vector<PackageMetadata>& queue,
    const std::set<std::string>& installed,
    bool verbose,
    TransactionPlan& out_plan,
    UpgradeContext* context = nullptr,
    std::string* error_out = nullptr
) {
    out_plan = {};
    if (error_out) error_out->clear();

    auto fail_plan = [&](const std::string& message) {
        if (error_out) {
            *error_out = message;
        } else {
            std::cerr << Color::RED << "E: " << message << Color::RESET << std::endl;
        }
        return false;
    };

    std::vector<PackageMetadata> working_queue;
    std::set<std::string> queued_names;
    for (const auto& pkg : queue) {
        std::string canonical_name = canonicalize_package_name(pkg.name);
        if (!queued_names.insert(canonical_name).second) continue;
        working_queue.push_back(pkg);
    }

    std::map<std::string, PackageMetadata> installed_meta_cache;
    std::set<std::string> missing_installed_meta;
    auto get_installed_meta = [&](const std::string& pkg_name) -> const PackageMetadata* {
        auto cache_it = installed_meta_cache.find(pkg_name);
        if (cache_it != installed_meta_cache.end()) return &cache_it->second;
        if (missing_installed_meta.count(pkg_name)) return nullptr;

        PackageMetadata meta;
        bool found = context
            ? get_context_live_installed_package_metadata(*context, pkg_name, meta)
            : get_live_installed_package_metadata(pkg_name, meta);
        if (!found) {
            missing_installed_meta.insert(pkg_name);
            return nullptr;
        }

        auto inserted = installed_meta_cache.emplace(pkg_name, std::move(meta));
        return &inserted.first->second;
    };

    std::vector<bool> dropped(working_queue.size(), false);
    for (size_t i = 0; i < working_queue.size(); ++i) {
        if (dropped[i]) continue;

        for (size_t j = i + 1; j < working_queue.size(); ++j) {
            if (dropped[j]) continue;

            const PackageMetadata& left = working_queue[i];
            const PackageMetadata& right = working_queue[j];
            bool left_conflicts = package_conflicts_with_package(left, right.name, &right);
            bool right_conflicts = package_conflicts_with_package(right, left.name, &left);
            if (!left_conflicts && !right_conflicts) continue;

            auto queue_entry_is_exact_live_shadowing_base_alias =
                [&](const PackageMetadata& candidate, const PackageMetadata& shadowed) {
                    if (!context) return false;
                    if (candidate.name.empty() || shadowed.name.empty()) return false;
                    if (canonicalize_package_name(candidate.name) ==
                        canonicalize_package_name(shadowed.name)) {
                        return false;
                    }

                    Dependency shadowed_dep{shadowed.name, "", ""};
                    bool exact_live_conflict_shadow =
                        dependency_matches_conflicting_exact_live_base_alias(
                            shadowed_dep,
                            candidate.name,
                            candidate,
                            context
                        );
                    bool direct_shadow =
                        package_metadata_satisfies_dependency(candidate.name, candidate, shadowed_dep) ||
                        package_replaces_package(candidate, shadowed.name, &shadowed);
                    if (!exact_live_conflict_shadow && !direct_shadow) return false;

                    auto existing = context->shadowed_base_alias_target.find(shadowed.name);
                    if (existing == context->shadowed_base_alias_target.end()) {
                        context->shadowed_base_alias_target[shadowed.name] = candidate.name;
                    }
                    auto& aliases = context->shadowed_aliases_by_target[candidate.name];
                    if (std::find(aliases.begin(), aliases.end(), shadowed.name) == aliases.end()) {
                        aliases.push_back(shadowed.name);
                    }

                    VLOG(verbose, "Dropping queued base alias " << shadowed.name
                                 << " in favor of exact live package family " << candidate.name
                                 << " because the alias only survives as present base-system state.");
                    return true;
                };

            if (queue_entry_is_exact_live_shadowing_base_alias(left, right)) {
                dropped[j] = true;
                continue;
            }
            if (queue_entry_is_exact_live_shadowing_base_alias(right, left)) {
                dropped[i] = true;
                break;
            }

            bool left_replaces = package_replaces_package(left, right.name, &right);
            bool right_replaces = package_replaces_package(right, left.name, &left);
            if (left_replaces && !right_replaces) {
                dropped[j] = true;
                continue;
            }
            if (right_replaces && !left_replaces) {
                dropped[i] = true;
                break;
            }

            return fail_plan(
                "Conflict detected in transaction! " + left.name +
                " conflicts with " + right.name
            );
        }
    }

    for (size_t i = 0; i < working_queue.size(); ++i) {
        if (!dropped[i]) out_plan.install_queue.push_back(working_queue[i]);
    }

    sort_transaction_queue_for_install(out_plan.install_queue, verbose, context);

    std::set<std::string> scheduled_retirements;
    for (const auto& pkg : out_plan.install_queue) {
        for (const auto& installed_name : installed) {
            if (installed_name == pkg.name) continue;

            const PackageMetadata* installed_meta = get_installed_meta(installed_name);
            if (!package_replaces_package(pkg, installed_name, installed_meta)) continue;
            bool queued_conflicts_installed =
                package_conflicts_with_package(pkg, installed_name, installed_meta);
            bool installed_conflicts_queued =
                installed_meta && package_conflicts_with_package(*installed_meta, pkg.name, &pkg);
            if (!queued_conflicts_installed && !installed_conflicts_queued) continue;

            if (scheduled_retirements.insert(installed_name).second) {
                out_plan.retirements.push_back({installed_name, pkg.name});
            }
        }
    }

    auto transaction_retires_or_upgrades_installed_package =
        [&](const std::string& installed_name, const PackageMetadata* installed_meta) {
            if (scheduled_retirements.count(installed_name) != 0) return true;

            std::string canonical_installed = canonicalize_package_name(installed_name);
            for (const auto& candidate : out_plan.install_queue) {
                if (canonicalize_package_name(candidate.name) == canonical_installed) {
                    return true;
                }
                if (installed_meta &&
                    package_replaces_package(candidate, installed_name, installed_meta)) {
                    return true;
                }
            }

            return false;
        };

    for (const auto& pkg : out_plan.install_queue) {
        for (const auto& installed_name : installed) {
            if (installed_name == pkg.name) continue;

            const PackageMetadata* installed_meta = get_installed_meta(installed_name);
            bool queued_breaks_installed =
                package_breaks_package(pkg, installed_name, installed_meta);
            bool queued_conflicts_installed =
                package_conflicts_with_package(pkg, installed_name, installed_meta);
            bool installed_conflicts_queued =
                installed_meta && package_conflicts_with_package(*installed_meta, pkg.name, &pkg);
            if (!queued_breaks_installed &&
                !queued_conflicts_installed &&
                !installed_conflicts_queued) {
                continue;
            }
            if (transaction_retires_or_upgrades_installed_package(installed_name, installed_meta)) {
                continue;
            }
            if (package_replaces_package(pkg, installed_name, installed_meta)) continue;

            if (queued_breaks_installed) {
                bool installed_upgrade_queued = false;
                for (const auto& candidate : out_plan.install_queue) {
                    bool same_family =
                        canonicalize_package_name(candidate.name) ==
                        canonicalize_package_name(installed_name);
                    bool replacement_upgrade =
                        installed_meta &&
                        package_replaces_package(candidate, installed_name, installed_meta);
                    if (!same_family && !replacement_upgrade) continue;
                    installed_upgrade_queued = true;
                    if (!package_breaks_package(pkg, candidate.name, &candidate) ||
                        replacement_upgrade) {
                        queued_breaks_installed = false;
                    }
                    break;
                }
                if (!queued_breaks_installed && installed_upgrade_queued) continue;
            }

            return fail_plan(
                "Conflict detected! " + pkg.name +
                (queued_breaks_installed ? " breaks " : " conflicts with installed package ") +
                installed_name
            );
        }
    }

    if (verbose) {
        VLOG(verbose, "No unresolved conflicts detected for " << out_plan.install_queue.size()
                     << " queued packages.");
    }
    return true;
}

bool build_transaction_plan(
    const std::vector<PackageMetadata>& queue,
    const std::set<std::string>& installed,
    bool verbose,
    TransactionPlan& out_plan,
    UpgradeContext* context = nullptr,
    std::string* error_out = nullptr
) {
    DebianBackendSelection backend = select_debian_backend(
        DebianBackendOperation::BuildTransactionPlan,
        verbose
    );
    maybe_log_debian_backend_selection(backend, DebianBackendOperation::BuildTransactionPlan, verbose);
    return build_transaction_plan_legacy(
        queue,
        installed,
        verbose,
        out_plan,
        context,
        error_out
    );
}

bool check_conflicts(const std::vector<PackageMetadata>& queue, const std::set<std::string>& installed, bool verbose) {
    TransactionPlan ignored_plan;
    return build_transaction_plan(queue, installed, verbose, ignored_plan);
}

bool should_retire_after_install(const TransactionPlan& plan, const std::string& pkg_name, std::vector<std::string>& retired_names) {
    retired_names.clear();
    for (const auto& entry : plan.retirements) {
        if (entry.replacement_name == pkg_name) retired_names.push_back(entry.installed_name);
    }
    return !retired_names.empty();
}

bool check_conflicts_legacy(const std::vector<PackageMetadata>& queue, const std::set<std::string>& installed, bool verbose) {
    return check_conflicts(queue, installed, verbose);
}
