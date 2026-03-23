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
    get_json_array(content, "depends", out_meta.depends);
    get_json_array(content, "recommends", out_meta.recommends);
    get_json_array(content, "suggests", out_meta.suggests);
    get_json_array(content, "conflicts", out_meta.conflicts);
    get_json_array(content, "provides", out_meta.provides);
    get_json_array(content, "replaces", out_meta.replaces);
    return !out_meta.version.empty() || !content.empty();
}

Dependency parse_dependency(const std::string& dep_str) {
    Dependency dep;
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

bool is_system_provided(const std::string& pkg, const std::string& op = "", const std::string& req_version = "") {
    if (dependency_list_matches(load_system_provides(), pkg, op, req_version)) return true;
    return dependency_list_matches(load_upgradeable_system_packages(), pkg, op, req_version);
}

bool is_upgradeable_system_package(const std::string& pkg) {
    return dependency_list_matches(load_upgradeable_system_packages(), pkg);
}

bool repo_has_satisfying_dependency(const Dependency& dep, bool verbose) {
    PackageMetadata repo_meta;
    if (get_repo_package_info(dep.name, repo_meta) &&
        version_satisfies(repo_meta.version, dep.op, dep.version)) {
        VLOG(verbose, dep.name << " is available from the repository as "
             << repo_meta.version << " and can replace the base runtime.");
        return true;
    }

    std::string provider = find_provider(dep.name, dep.op, dep.version, verbose);
    if (!provider.empty()) {
        VLOG(verbose, dep.name << " can be satisfied by repository provider " << provider
             << " instead of the base runtime.");
        return true;
    }

    return false;
}

bool package_metadata_satisfies_dependency(
    const std::string& package_name,
    const PackageMetadata& meta,
    const Dependency& dep
) {
    std::string canonical_package = canonicalize_package_name(package_name);
    std::string canonical_dep = canonicalize_package_name(dep.name);
    if (canonical_package == canonical_dep &&
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

bool queued_candidate_satisfies_dependency(
    const PackageMetadata& meta,
    const Dependency& dep
) {
    return package_metadata_satisfies_dependency(meta.name, meta, dep);
}

bool find_installed_dependency_provider(
    const Dependency& dep,
    const std::set<std::string>& installed_cache,
    std::string* provider_out = nullptr
) {
    if (provider_out) provider_out->clear();

    for (const auto& installed_name : installed_cache) {
        PackageMetadata meta;
        if (!get_installed_package_metadata(installed_name, meta)) continue;
        if (!package_metadata_satisfies_dependency(installed_name, meta, dep)) continue;
        if (provider_out) *provider_out = installed_name;
        return true;
    }

    return false;
}

bool is_dependency_satisfied_locally(
    const Dependency& dep,
    const std::set<std::string>& installed_cache,
    bool verbose,
    std::string* provider_out = nullptr
) {
    if (provider_out) provider_out->clear();

    std::string installed_ver;
    if (is_installed(dep.name, &installed_ver) && version_satisfies(installed_ver, dep.op, dep.version)) {
        if (provider_out) *provider_out = dep.name;
        return true;
    }

    std::string provider_name;
    if (find_installed_dependency_provider(dep, installed_cache, &provider_name)) {
        if (provider_out) *provider_out = provider_name;
        return true;
    }

    if (is_system_provided(dep.name, dep.op, dep.version)) {
        if (is_upgradeable_system_package(dep.name) && repo_has_satisfying_dependency(dep, verbose)) {
            VLOG(verbose, dep.name << " is base-provided but marked upgradeable; preferring repository candidate.");
            return false;
        }
        if (provider_out) *provider_out = BASE_SYSTEM_PROVIDER;
        return true;
    }

    if (verbose && !dep.op.empty() && is_installed(dep.name, &installed_ver)) {
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

bool resolve_dependencies(
    const std::string& pkg,
    const std::string& op,
    const std::string& req_version,
    std::vector<PackageMetadata>& install_queue,
    std::set<std::string>& visited,
    const std::set<std::string>& installed_cache,
    bool verbose,
    bool force_queue_requested = false
) {
    std::string canonical_pkg = canonicalize_package_name(pkg, verbose);
    Dependency requested_dep{canonical_pkg, op, req_version};

    if (visited.count(canonical_pkg)) {
        for (const auto& queued : install_queue) {
            if (queued.name != canonical_pkg) continue;
            if (!queued_candidate_satisfies_dependency(queued, requested_dep)) {
                std::cerr << Color::RED << "E: Dependency conflict! " << canonical_pkg << " " << queued.version
                          << " is queued, but " << op << " " << req_version
                          << " is required." << Color::RESET << std::endl;
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
        is_dependency_satisfied_locally(requested_dep, installed_cache, verbose, &provider_name)) {
        if (provider_name == BASE_SYSTEM_PROVIDER) {
            VLOG(verbose, canonical_pkg << " is satisfied by the base-system policy.");
        } else if (provider_name == canonical_pkg) {
            std::string installed_ver;
            is_installed(canonical_pkg, &installed_ver);
            VLOG(verbose, canonical_pkg << " " << installed_ver << " is installed and satisfies constraints.");
        } else {
            VLOG(verbose, canonical_pkg << " is provided by installed package " << provider_name);
        }
        return true;
    }

    std::string installed_ver;
    if (!force_queue_requested && is_installed(canonical_pkg, &installed_ver)) {
        if (version_satisfies(installed_ver, op, req_version)) {
            VLOG(verbose, canonical_pkg << " " << installed_ver << " is installed and satisfies constraints.");
            return true;
        }

        std::cerr << Color::YELLOW << "W: " << canonical_pkg << " " << installed_ver
                  << " is installed but does not meet requirements (" << op
                  << " " << req_version << ")." << Color::RESET << std::endl;
    }

    if (is_blocked_import_package(canonical_pkg, verbose)) {
        std::cerr << Color::RED << "E: Package " << canonical_pkg
                  << " is blocked by GeminiOS import policy." << Color::RESET << std::endl;
        return false;
    }

    PackageMetadata meta;
    bool found_exact = get_repo_package_info(canonical_pkg, meta);
    if (!found_exact) {
        VLOG(verbose, "Exact match for " << canonical_pkg << " not found. Searching for providers...");
        std::string provider = find_provider(canonical_pkg, op, req_version, verbose);
        if (!provider.empty()) {
            VLOG(verbose, "Redirecting " << canonical_pkg << " -> " << provider);
            return resolve_dependencies(provider, "", "", install_queue, visited, installed_cache, verbose, force_queue_requested);
        }

        std::cerr << Color::RED << "E: Unable to locate package " << canonical_pkg << Color::RESET << std::endl;
        return false;
    }

    if (!queued_candidate_satisfies_dependency(meta, requested_dep)) {
        std::cerr << Color::RED << "E: Package " << canonical_pkg << " found (v" << meta.version
                  << ") but does not meet requirements (" << op << " " << req_version
                  << ")" << Color::RESET << std::endl;
        return false;
    }

    VLOG(verbose, "Found " << canonical_pkg << " in repository (version: " << meta.version << ")");
    if (verbose) {
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
    for (const auto& dep_str : collect_transaction_dependency_edges(meta)) {
        Dependency dep = parse_dependency(dep_str);
        if (!resolve_dependencies(dep.name, dep.op, dep.version, install_queue, visited, installed_cache, verbose, false)) {
            return false;
        }
    }

    VLOG(verbose, "Adding " << canonical_pkg << " to installation queue.");
    install_queue.push_back(meta);
    return true;
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

bool package_conflicts_with_package(
    const PackageMetadata& meta,
    const std::string& pkg_name,
    const PackageMetadata* pkg_meta = nullptr
) {
    return relation_list_matches_package(meta.conflicts, pkg_name, pkg_meta);
}

bool package_replaces_package(
    const PackageMetadata& meta,
    const std::string& pkg_name,
    const PackageMetadata* pkg_meta = nullptr
) {
    return relation_list_matches_package(meta.replaces, pkg_name, pkg_meta);
}

bool build_transaction_plan(
    const std::vector<PackageMetadata>& queue,
    const std::set<std::string>& installed,
    bool verbose,
    TransactionPlan& out_plan
) {
    out_plan = {};

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
        if (!get_installed_package_metadata(pkg_name, meta)) {
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

            std::cerr << Color::RED << "E: Conflict detected in transaction! "
                      << left.name << " conflicts with " << right.name
                      << Color::RESET << std::endl;
            return false;
        }
    }

    for (size_t i = 0; i < working_queue.size(); ++i) {
        if (!dropped[i]) out_plan.install_queue.push_back(working_queue[i]);
    }

    std::set<std::string> scheduled_retirements;
    for (const auto& pkg : out_plan.install_queue) {
        for (const auto& installed_name : installed) {
            if (installed_name == pkg.name) continue;

            const PackageMetadata* installed_meta = get_installed_meta(installed_name);
            if (!package_replaces_package(pkg, installed_name, installed_meta)) continue;

            if (scheduled_retirements.insert(installed_name).second) {
                out_plan.retirements.push_back({installed_name, pkg.name});
            }
        }
    }

    for (const auto& pkg : out_plan.install_queue) {
        for (const auto& installed_name : installed) {
            if (installed_name == pkg.name) continue;

            const PackageMetadata* installed_meta = get_installed_meta(installed_name);
            bool queued_conflicts_installed =
                package_conflicts_with_package(pkg, installed_name, installed_meta);
            bool installed_conflicts_queued =
                installed_meta && package_conflicts_with_package(*installed_meta, pkg.name, &pkg);
            if (!queued_conflicts_installed && !installed_conflicts_queued) continue;
            if (package_replaces_package(pkg, installed_name, installed_meta)) continue;

            std::cerr << Color::RED << "E: Conflict detected! " << pkg.name
                      << " conflicts with installed package " << installed_name
                      << Color::RESET << std::endl;
            return false;
        }
    }

    if (verbose) {
        VLOG(verbose, "No unresolved conflicts detected for " << out_plan.install_queue.size()
                     << " queued packages.");
    }
    return true;
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
