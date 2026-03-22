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

std::vector<std::string> load_system_provides() {
    return load_dependency_entries(SYSTEM_PROVIDES_PATH);
}

std::vector<std::string> load_upgradeable_system_packages() {
    return load_dependency_entries(UPGRADEABLE_SYSTEM_PATH);
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
        if (op.empty() || dep.version.empty() || version_satisfies(dep.version, op, req_version)) {
            return true;
        }
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
    if (package_name == dep.name && version_satisfies(meta.version, dep.op, dep.version)) {
        return true;
    }

    for (const auto& provided : meta.provides) {
        Dependency provided_dep = parse_dependency(provided);
        if (provided_dep.name != dep.name) continue;
        if (dep.op.empty()) return true;
        if (!provided_dep.version.empty() &&
            version_satisfies(provided_dep.version, dep.op, dep.version)) {
            return true;
        }
    }

    return false;
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
        if (provider_out) *provider_out = SYSTEM_PROVIDES_PATH;
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
    foreach_json_object(REPO_CACHE_PATH + "Packages.json", [&](const std::string& obj) {
        std::vector<std::string> provides;
        if (!get_json_array(obj, "provides", provides)) return true;

        for (const auto& provided : provides) {
            Dependency prov_dep = parse_dependency(provided);
            if (prov_dep.name != capability) continue;

            bool satisfies = op.empty() ||
                (!prov_dep.version.empty() && version_satisfies(prov_dep.version, op, req_version));
            if (!satisfies) continue;

            PackageMetadata candidate;
            populate_package_metadata_from_json(obj, candidate);
            candidate.name = trim(candidate.name);
            if (!found || should_prefer_repo_candidate(candidate, best_meta)) {
                best_meta = candidate;
                result = candidate.name;
                found = true;
            }
            break;
        }

        return true;
    });
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
    bool verbose
) {
    if (visited.count(pkg)) {
        for (const auto& queued : install_queue) {
            if (queued.name != pkg) continue;
            if (!version_satisfies(queued.version, op, req_version)) {
                std::cerr << Color::RED << "E: Dependency conflict! " << pkg << " " << queued.version
                          << " is queued, but " << op << " " << req_version
                          << " is required." << Color::RESET << std::endl;
                return false;
            }
            return true;
        }
        return true;
    }

    VLOG(verbose, "Resolving dependencies for: " << pkg
         << (op.empty() ? "" : (" (" + op + " " + req_version + ")")));

    Dependency requested_dep{pkg, op, req_version};
    std::string provider_name;
    if (is_dependency_satisfied_locally(requested_dep, installed_cache, verbose, &provider_name)) {
        if (provider_name == SYSTEM_PROVIDES_PATH) {
            VLOG(verbose, pkg << " is satisfied by " << SYSTEM_PROVIDES_PATH);
        } else if (provider_name == pkg) {
            std::string installed_ver;
            is_installed(pkg, &installed_ver);
            VLOG(verbose, pkg << " " << installed_ver << " is installed and satisfies constraints.");
        } else {
            VLOG(verbose, pkg << " is provided by installed package " << provider_name);
        }
        return true;
    }

    std::string installed_ver;
    if (is_installed(pkg, &installed_ver)) {
        if (version_satisfies(installed_ver, op, req_version)) {
            VLOG(verbose, pkg << " " << installed_ver << " is installed and satisfies constraints.");
            return true;
        }

        std::cerr << Color::YELLOW << "W: " << pkg << " " << installed_ver
                  << " is installed but does not meet requirements (" << op
                  << " " << req_version << ")." << Color::RESET << std::endl;
    }

    if (is_blocked_import_package(pkg, verbose)) {
        std::cerr << Color::RED << "E: Package " << pkg
                  << " is blocked by GeminiOS import policy." << Color::RESET << std::endl;
        return false;
    }

    PackageMetadata meta;
    bool found_exact = get_repo_package_info(pkg, meta);
    if (!found_exact) {
        VLOG(verbose, "Exact match for " << pkg << " not found. Searching for providers...");
        std::string provider = find_provider(pkg, op, req_version, verbose);
        if (!provider.empty()) {
            VLOG(verbose, "Redirecting " << pkg << " -> " << provider);
            return resolve_dependencies(provider, "", "", install_queue, visited, installed_cache, verbose);
        }

        std::cerr << Color::RED << "E: Unable to locate package " << pkg << Color::RESET << std::endl;
        return false;
    }

    if (!version_satisfies(meta.version, op, req_version)) {
        std::cerr << Color::RED << "E: Package " << pkg << " found (v" << meta.version
                  << ") but does not meet requirements (" << op << " " << req_version
                  << ")" << Color::RESET << std::endl;
        return false;
    }

    VLOG(verbose, "Found " << pkg << " in repository (version: " << meta.version << ")");
    if (verbose) {
        if (!meta.depends.empty()) {
            VLOG(verbose, pkg << " depends on: " << join_strings(meta.depends));
        }
        if (!meta.recommends.empty()) {
            VLOG(verbose, pkg
                 << (should_include_recommends_for_transaction(meta)
                        ? " includes recommends: "
                        : " skips recommends: ")
                 << join_strings(meta.recommends));
        }
        if (!meta.suggests.empty()) {
            VLOG(verbose, pkg
                 << (should_include_suggests_for_transaction(meta)
                        ? " includes suggests: "
                        : " skips suggests: ")
                 << join_strings(meta.suggests));
        }
    }

    visited.insert(pkg);
    for (const auto& dep_str : collect_transaction_dependency_edges(meta)) {
        Dependency dep = parse_dependency(dep_str);
        if (!resolve_dependencies(dep.name, dep.op, dep.version, install_queue, visited, installed_cache, verbose)) {
            return false;
        }
    }

    VLOG(verbose, "Adding " << pkg << " to installation queue.");
    install_queue.push_back(meta);
    return true;
}

bool check_conflicts(const std::vector<PackageMetadata>& queue, const std::set<std::string>& installed, bool verbose) {
    bool has_conflict = false;
    for (const auto& pkg : queue) {
        for (const auto& conflict : pkg.conflicts) {
            if (installed.count(conflict)) {
                std::cerr << Color::RED << "E: Conflict detected! " << pkg.name
                          << " conflicts with installed package " << conflict
                          << Color::RESET << std::endl;
                has_conflict = true;
            }

            for (const auto& other : queue) {
                if (other.name != conflict) continue;
                std::cerr << Color::RED << "E: Conflict detected in transaction! "
                          << pkg.name << " conflicts with " << other.name
                          << Color::RESET << std::endl;
                has_conflict = true;
            }
        }
    }

    if (verbose && !has_conflict) {
        VLOG(verbose, "No conflicts detected for " << queue.size() << " queued packages.");
    }
    return !has_conflict;
}
