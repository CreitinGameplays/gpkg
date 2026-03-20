// Dependency parsing and resolver logic.

struct Dependency {
    std::string name;
    std::string op;
    std::string version;
};

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

std::vector<std::string> load_system_provides() {
    std::vector<std::string> entries;
    std::ifstream f(SYSTEM_PROVIDES_PATH);
    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        entries.push_back(line);
    }
    return entries;
}

bool is_system_provided(const std::string& pkg, const std::string& op = "", const std::string& req_version = "") {
    for (const auto& entry : load_system_provides()) {
        Dependency dep = parse_dependency(entry);
        if (dep.name != pkg) continue;
        if (op.empty() || dep.version.empty() || version_satisfies(dep.version, op, req_version)) {
            return true;
        }
    }
    return false;
}

std::string find_provider(const std::string& capability, const std::string& op, const std::string& req_version, bool verbose) {
    std::string result;
    foreach_json_object(REPO_CACHE_PATH + "Packages.json", [&](const std::string& obj) {
        std::vector<std::string> provides;
        if (!get_json_array(obj, "provides", provides)) return true;

        for (const auto& provided : provides) {
            Dependency prov_dep = parse_dependency(provided);
            if (prov_dep.name != capability) continue;

            bool satisfies = op.empty() ||
                (!prov_dep.version.empty() && version_satisfies(prov_dep.version, op, req_version));
            if (!satisfies) continue;

            get_json_value(obj, "package", result);
            result = trim(result);
            VLOG(verbose, "Found provider for " << capability
                 << (op.empty() ? "" : (" (" + op + " " + req_version + ")"))
                 << ": " << result);
            return false;
        }

        return true;
    });
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

    if (is_system_provided(pkg, op, req_version)) {
        VLOG(verbose, pkg << " is satisfied by " << SYSTEM_PROVIDES_PATH);
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

    for (const auto& installed_name : installed_cache) {
        std::ifstream f(INFO_DIR + installed_name + ".json");
        if (!f) continue;

        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        std::vector<std::string> installed_provides;
        if (!get_json_array(content, "provides", installed_provides)) continue;

        for (const auto& provided : installed_provides) {
            std::string provided_name = provided;
            size_t space = provided.find(' ');
            if (space != std::string::npos) provided_name = provided.substr(0, space);
            if (provided_name == pkg) {
                VLOG(verbose, pkg << " is provided by installed package " << installed_name);
                return true;
            }
        }
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
    if (verbose && !meta.depends.empty()) {
        VLOG(verbose, pkg << " depends on: " << join_strings(meta.depends));
    }

    visited.insert(pkg);
    for (const auto& dep_str : meta.depends) {
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
