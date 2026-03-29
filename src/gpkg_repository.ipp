// Repository configuration, index management, and repo-backed commands.

std::map<std::string, PackageMetadata> g_repo_package_cache;
std::map<std::string, std::vector<std::string>> g_repo_provider_cache;
std::set<std::string> g_repo_available_package_cache;
bool g_repo_package_cache_loaded = false;

struct PackageUniverseResult {
    bool found = false;
    bool installable = false;
    bool raw_only = false;
    std::string reason;
    PackageMetadata meta;
};

bool ensure_repo_package_cache_loaded(bool verbose);
bool should_prefer_repo_candidate(const PackageMetadata& candidate, const PackageMetadata& current);
bool query_full_universe_exact_package(
    const std::string& pkg_name,
    PackageUniverseResult& out_result,
    bool verbose,
    RawDebianContext* raw_context = nullptr
);
bool resolve_full_universe_relation_candidate(
    const std::string& pkg_name,
    const std::string& op,
    const std::string& req_version,
    PackageUniverseResult& out_result,
    bool verbose,
    RawDebianContext* raw_context = nullptr,
    const PackageMetadata* installed_meta = nullptr
);

std::string relation_name_from_text(const std::string& relation) {
    size_t open_paren = relation.find('(');
    return trim(open_paren == std::string::npos ? relation : relation.substr(0, open_paren));
}

bool repo_index_file_present() {
    return access((REPO_CACHE_PATH + "Packages.json").c_str(), F_OK) == 0;
}

bool try_ensure_repo_package_cache_loaded(bool verbose) {
    if (g_repo_package_cache_loaded) return true;
    if (!repo_index_file_present()) return false;
    return ensure_repo_package_cache_loaded(verbose);
}

bool catalog_version_satisfies(
    const std::string& current_ver,
    const std::string& op,
    const std::string& req_version
) {
    if (op.empty()) return true;

    int cmp = compare_versions(current_ver, req_version);
    if (op == ">>" || op == ">") return cmp > 0;
    if (op == "<<") return cmp < 0;
    if (op == ">=") return cmp >= 0;
    if (op == "<=") return cmp <= 0;
    if (op == "=" || op == "==") return cmp == 0;
    return false;
}

bool catalog_meta_satisfies_relation(
    const std::string& package_name,
    const PackageMetadata& meta,
    const RelationAtom& relation
) {
    std::string canonical_package = canonicalize_package_name(package_name);
    std::string canonical_relation = canonicalize_package_name(relation.name);
    if (canonical_package == canonical_relation &&
        catalog_version_satisfies(meta.version, relation.op, relation.version)) {
        return true;
    }

    for (const auto& provided_relation : meta.provides) {
        RelationAtom provided = normalize_relation_atom(provided_relation, "any");
        if (!provided.valid) continue;
        if (canonicalize_package_name(provided.name) != canonical_relation) continue;
        if (relation.op.empty()) return true;
        if (!provided.version.empty() &&
            catalog_version_satisfies(provided.version, relation.op, relation.version)) {
            return true;
        }
    }

    return false;
}

bool get_loaded_repo_package_info(const std::string& pkg_name, PackageMetadata& out_meta) {
    auto it = g_repo_package_cache.find(pkg_name);
    if (it == g_repo_package_cache.end()) return false;
    out_meta = it->second;
    return true;
}

std::string upgrade_catalog_file_fingerprint_component(const std::string& path) {
    struct stat st {};
    if (lstat(path.c_str(), &st) != 0) return path + ":missing";

    std::ostringstream out;
    out << path << ":" << static_cast<long long>(st.st_size)
        << ":" << static_cast<long long>(st.st_mtime);
    return out.str();
}

std::string build_upgrade_catalog_fingerprint() {
    return "v1|" +
        upgrade_catalog_file_fingerprint_component(REPO_CACHE_PATH + "Packages.json") + "|" +
        upgrade_catalog_file_fingerprint_component(IMPORT_POLICY_PATH) + "|" +
        upgrade_catalog_file_fingerprint_component(UPGRADE_COMPANIONS_PATH);
}

std::set<std::string> load_present_base_registry_package_names() {
    std::set<std::string> names;
    for (const auto& entry : load_base_system_registry_entries()) {
        if (entry.package.empty()) continue;
        if (!base_system_registry_entry_looks_present(entry)) continue;
        names.insert(canonicalize_package_name(entry.package));
    }
    return names;
}

const std::set<std::string>& get_loaded_repo_package_names() {
    return g_repo_available_package_cache;
}

RelationAtom apply_catalog_policy_resolution(const RelationAtom& relation) {
    RelationAtom resolved = relation;
    if (!resolved.valid || resolved.name.empty()) return resolved;

    const ImportPolicy& policy = get_import_policy();
    std::string rewritten_name = apply_dependency_rewrite_name(
        resolved.name,
        policy.dependency_rewrites,
        &policy.package_aliases
    );
    if (!rewritten_name.empty()) resolved.name = rewritten_name;

    std::string provider_name = resolve_provider_name(
        resolved.name,
        policy.provider_choices,
        g_repo_provider_cache,
        get_loaded_repo_package_names()
    );
    if (!provider_name.empty()) resolved.name = provider_name;

    resolved.normalized = resolved.op.empty()
        ? resolved.name
        : resolved.name + " (" + resolved.op + " " + resolved.version + ")";
    resolved.valid = !resolved.name.empty();
    return resolved;
}

std::vector<std::string> parse_upgrade_companion_tokens_for_catalog(const std::string& raw_value) {
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

std::map<std::string, std::vector<std::string>> load_raw_upgrade_companions_for_catalog() {
    std::map<std::string, std::vector<std::string>> companions;
    std::ifstream f(UPGRADE_COMPANIONS_PATH);
    if (!f) return companions;

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

        auto parsed = parse_upgrade_companion_tokens_for_catalog(raw_companions);
        auto& entry = companions[trigger];
        std::set<std::string> seen(entry.begin(), entry.end());
        for (const auto& pkg : parsed) {
            if (seen.insert(pkg).second) entry.push_back(pkg);
        }
    }

    return companions;
}

void append_upgrade_catalog_skip_entry(
    ResolvedUpgradeCatalog& catalog,
    const std::string& kind,
    const std::string& configured_name,
    const std::string& reason,
    const std::string& trigger = "",
    const std::string& resolved_name = ""
) {
    UpgradeCatalogSkipEntry entry;
    entry.kind = kind;
    entry.trigger = trigger;
    entry.configured_name = configured_name;
    entry.resolved_name = resolved_name;
    entry.reason = reason;
    catalog.skipped_entries.push_back(entry);
}

bool resolve_catalog_relation(
    const RelationAtom& relation,
    PackageMetadata& out_meta,
    std::string& out_name,
    std::string* reason_out = nullptr
) {
    out_meta = {};
    out_name.clear();
    if (reason_out) reason_out->clear();

    if (!relation.valid || relation.name.empty()) {
        if (reason_out) *reason_out = "invalid package relation";
        return false;
    }

    RelationAtom effective_relation = apply_catalog_policy_resolution(relation);
    std::string requested_name = canonicalize_package_name(effective_relation.name);
    PackageMetadata exact_meta;
    if (get_loaded_repo_package_info(requested_name, exact_meta) &&
        catalog_meta_satisfies_relation(requested_name, exact_meta, effective_relation)) {
        out_meta = exact_meta;
        out_name = exact_meta.name;
        return true;
    }

    auto provider_it = g_repo_provider_cache.find(requested_name);
    if (provider_it == g_repo_provider_cache.end()) {
        if (reason_out) *reason_out = "no repository package or provider candidate";
        return false;
    }

    bool found = false;
    PackageMetadata best_meta;
    std::string best_name;
    for (const auto& provider_name : provider_it->second) {
        PackageMetadata candidate;
        if (!get_loaded_repo_package_info(provider_name, candidate)) continue;
        if (!catalog_meta_satisfies_relation(provider_name, candidate, effective_relation)) continue;

        if (!found || should_prefer_repo_candidate(candidate, best_meta)) {
            best_meta = candidate;
            best_name = candidate.name;
            found = true;
        }
    }

    if (!found) {
        if (reason_out) *reason_out = "no repository candidate satisfies the required relation";
        return false;
    }

    out_meta = best_meta;
    out_name = best_name;
    return true;
}

struct UpgradeCatalogValidationCache {
    std::set<std::string> valid_packages;
    std::map<std::string, std::string> invalid_reasons;
};

bool validate_catalog_package_closure_recursive(
    const std::string& pkg_name,
    UpgradeCatalogValidationCache& cache,
    std::set<std::string>& walk
) {
    if (cache.valid_packages.count(pkg_name) != 0) return true;
    auto invalid_it = cache.invalid_reasons.find(pkg_name);
    if (invalid_it != cache.invalid_reasons.end()) return false;
    if (!walk.insert(pkg_name).second) return true;

    PackageMetadata meta;
    if (!get_loaded_repo_package_info(pkg_name, meta)) {
        cache.invalid_reasons[pkg_name] = "repository metadata is missing";
        walk.erase(pkg_name);
        return false;
    }

    for (const auto& dep_str : meta.depends) {
        RelationAtom dep = normalize_relation_atom(dep_str, "any");
        if (!dep.valid) continue;
        if (is_system_provided(dep.name, dep.op, dep.version)) continue;

        PackageMetadata dep_meta;
        std::string dep_name;
        std::string resolve_reason;
        if (!resolve_catalog_relation(dep, dep_meta, dep_name, &resolve_reason)) {
            cache.invalid_reasons[pkg_name] =
                "missing required dependency " + dep_str + " for " + pkg_name;
            walk.erase(pkg_name);
            return false;
        }

        if (!validate_catalog_package_closure_recursive(dep_name, cache, walk)) {
            auto child_invalid_it = cache.invalid_reasons.find(dep_name);
            if (child_invalid_it != cache.invalid_reasons.end()) {
                cache.invalid_reasons[pkg_name] = child_invalid_it->second;
            } else {
                cache.invalid_reasons[pkg_name] =
                    "required dependency closure failed via " + dep_name;
            }
            walk.erase(pkg_name);
            return false;
        }
    }

    walk.erase(pkg_name);
    cache.valid_packages.insert(pkg_name);
    return true;
}

bool validate_catalog_package_closure(
    const std::string& pkg_name,
    UpgradeCatalogValidationCache& cache,
    std::string* reason_out = nullptr
) {
    std::set<std::string> walk;
    bool ok = validate_catalog_package_closure_recursive(pkg_name, cache, walk);
    if (reason_out) {
        if (ok) reason_out->clear();
        else *reason_out = cache.invalid_reasons[pkg_name];
    }
    return ok;
}

bool build_resolved_upgrade_catalog(
    ResolvedUpgradeCatalog& out_catalog,
    bool verbose,
    std::string* error_out = nullptr
) {
    out_catalog = {};
    if (error_out) error_out->clear();

    if (!ensure_repo_package_cache_loaded(verbose)) {
        if (error_out) *error_out = "repository package index could not be loaded";
        return false;
    }

    out_catalog.fingerprint = build_upgrade_catalog_fingerprint();

    const ImportPolicy& policy = get_import_policy(verbose);
    std::vector<std::string> raw_roots = policy.upgradeable_system.empty()
        ? load_pattern_entries_file(UPGRADEABLE_SYSTEM_PATH)
        : load_pattern_entries(policy.upgradeable_system);
    std::map<std::string, std::vector<std::string>> raw_companions =
        load_raw_upgrade_companions_for_catalog();
    std::set<std::string> present_base_packages = load_present_base_registry_package_names();
    UpgradeCatalogValidationCache validation_cache;
    std::map<std::string, std::string> resolved_root_by_configured;
    std::set<std::string> emitted_roots;

    auto try_add_root = [&](const std::string& raw_root, bool report_skip) {
        RelationAtom relation = normalize_relation_atom(raw_root, "any");
        if (!relation.valid || relation.name.empty()) {
            if (report_skip) {
                append_upgrade_catalog_skip_entry(
                    out_catalog,
                    "root",
                    raw_root,
                    "invalid configured upgrade root"
                );
            }
            return;
        }

        PackageMetadata resolved_meta;
        std::string resolved_name;
        std::string resolve_reason;
        if (!resolve_catalog_relation(relation, resolved_meta, resolved_name, &resolve_reason)) {
            RelationAtom effective_relation = apply_catalog_policy_resolution(relation);
            std::string canonical_root = canonicalize_package_name(effective_relation.name);
            if (report_skip && !canonical_root.empty() && present_base_packages.count(canonical_root) != 0) {
                VLOG(verbose, "Skipping base-only runtime family " << raw_root
                             << " because no repository upgrade candidate is configured.");
                return;
            }
            if (report_skip) {
                append_upgrade_catalog_skip_entry(
                    out_catalog,
                    "root",
                    raw_root,
                    resolve_reason
                );
            } else {
                VLOG(verbose, "Skipping unresolved base-system upgrade root " << raw_root
                             << ": " << resolve_reason);
            }
            return;
        }

        std::string validation_reason;
        if (!validate_catalog_package_closure(resolved_name, validation_cache, &validation_reason)) {
            if (report_skip) {
                append_upgrade_catalog_skip_entry(
                    out_catalog,
                    "root",
                    raw_root,
                    validation_reason,
                    "",
                    resolved_name
                );
            } else {
                VLOG(verbose, "Skipping invalid base-system upgrade root " << raw_root
                             << ": " << validation_reason);
            }
            return;
        }

        if (report_skip) resolved_root_by_configured[raw_root] = resolved_name;
        if (emitted_roots.insert(resolved_name).second) {
            out_catalog.resolved_roots.push_back(resolved_name);
        }
    };

    for (const auto& raw_root : raw_roots) {
        try_add_root(raw_root, true);
    }

    for (const auto& base_root : present_base_packages) {
        if (base_root.empty()) continue;
        if (is_blocked_import_package(base_root, verbose)) continue;
        try_add_root(base_root, false);
    }

    for (const auto& entry : raw_companions) {
        auto resolved_trigger_it = resolved_root_by_configured.find(entry.first);
        if (resolved_trigger_it == resolved_root_by_configured.end()) continue;

        const std::string& resolved_trigger = resolved_trigger_it->second;
        auto& resolved_list = out_catalog.resolved_companions[resolved_trigger];
        std::set<std::string> seen(resolved_list.begin(), resolved_list.end());
        for (const auto& raw_companion : entry.second) {
            RelationAtom relation = normalize_relation_atom(raw_companion, "any");
            if (!relation.valid || relation.name.empty()) {
                append_upgrade_catalog_skip_entry(
                    out_catalog,
                    "companion",
                    raw_companion,
                    "invalid configured runtime companion",
                    resolved_trigger
                );
                continue;
            }

            PackageMetadata resolved_meta;
            std::string resolved_name;
            std::string resolve_reason;
            if (!resolve_catalog_relation(relation, resolved_meta, resolved_name, &resolve_reason)) {
                append_upgrade_catalog_skip_entry(
                    out_catalog,
                    "companion",
                    raw_companion,
                    resolve_reason,
                    resolved_trigger
                );
                continue;
            }

            std::string validation_reason;
            if (!validate_catalog_package_closure(resolved_name, validation_cache, &validation_reason)) {
                append_upgrade_catalog_skip_entry(
                    out_catalog,
                    "companion",
                    raw_companion,
                    validation_reason,
                    resolved_trigger,
                    resolved_name
                );
                continue;
            }

            if (resolved_name == resolved_trigger) continue;
            if (seen.insert(resolved_name).second) resolved_list.push_back(resolved_name);
        }
    }

    return true;
}

std::string upgrade_catalog_skip_entry_to_json(const UpgradeCatalogSkipEntry& entry) {
    std::vector<std::string> fields;
    fields.push_back(json_string_field("kind", entry.kind));
    fields.push_back(json_string_field("trigger", entry.trigger));
    fields.push_back(json_string_field("configured_name", entry.configured_name));
    fields.push_back(json_string_field("resolved_name", entry.resolved_name));
    fields.push_back(json_string_field("reason", entry.reason));
    return "{" + join_strings(fields, ",") + "}";
}

std::string upgrade_catalog_companion_map_to_json(
    const std::map<std::string, std::vector<std::string>>& companions
) {
    std::vector<std::string> fields;
    for (const auto& entry : companions) {
        fields.push_back(
            "\"" + json_escape(entry.first) + "\":" + json_array_from_strings(entry.second)
        );
    }
    return "{" + join_strings(fields, ",") + "}";
}

bool write_upgrade_catalog(
    const ResolvedUpgradeCatalog& catalog,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    if (!mkdir_parent(UPGRADE_CATALOG_PATH)) {
        if (error_out) *error_out = "failed to create parent directory for " + UPGRADE_CATALOG_PATH;
        return false;
    }

    std::string tmp_path = UPGRADE_CATALOG_PATH + ".tmp";
    std::ofstream out(tmp_path);
    if (!out) {
        if (error_out) *error_out = "failed to open " + tmp_path + " for writing";
        return false;
    }

    out << "{\n";
    out << "  " << json_string_field("fingerprint", catalog.fingerprint) << ",\n";
    out << "  \"resolved_roots\":" << json_array_from_strings(catalog.resolved_roots) << ",\n";
    out << "  \"resolved_companions\":"
        << upgrade_catalog_companion_map_to_json(catalog.resolved_companions) << ",\n";
    out << "  \"skipped_entries\":[";
    for (size_t i = 0; i < catalog.skipped_entries.size(); ++i) {
        if (i > 0) out << ",";
        out << "\n    " << upgrade_catalog_skip_entry_to_json(catalog.skipped_entries[i]);
    }
    if (!catalog.skipped_entries.empty()) out << "\n  ";
    out << "]\n";
    out << "}\n";
    out.close();

    if (rename(tmp_path.c_str(), UPGRADE_CATALOG_PATH.c_str()) != 0) {
        if (error_out) *error_out = "failed to replace " + UPGRADE_CATALOG_PATH;
        remove(tmp_path.c_str());
        return false;
    }

    return true;
}

bool load_upgrade_catalog(
    ResolvedUpgradeCatalog& out_catalog,
    std::string* problem_out,
    bool verbose
) {
    (void)verbose;
    out_catalog = {};
    if (problem_out) problem_out->clear();

    JsonValue root;
    if (!load_json_document(UPGRADE_CATALOG_PATH, root)) {
        if (problem_out) {
            if (access(UPGRADE_CATALOG_PATH.c_str(), F_OK) == 0) {
                *problem_out = "upgrade catalog is unreadable; run 'gpkg update'";
            } else {
                *problem_out = "upgrade catalog is missing; run 'gpkg update'";
            }
        }
        return false;
    }

    out_catalog.fingerprint = json_string_or(json_object_get(root, "fingerprint"));
    if (out_catalog.fingerprint.empty()) {
        if (problem_out) *problem_out = "upgrade catalog is missing its fingerprint; run 'gpkg update'";
        return false;
    }

    std::string expected_fingerprint = build_upgrade_catalog_fingerprint();
    if (out_catalog.fingerprint != expected_fingerprint) {
        if (problem_out) *problem_out = "upgrade catalog is stale; run 'gpkg update'";
        return false;
    }

    out_catalog.resolved_roots = json_string_array(json_object_get(root, "resolved_roots"));

    if (const JsonValue* companions = json_object_get(root, "resolved_companions")) {
        if (companions->is_object()) {
            for (const auto& entry : companions->object_items) {
                out_catalog.resolved_companions[entry.first] = json_string_array(&entry.second);
            }
        }
    }

    if (const JsonValue* skipped_entries = json_object_get(root, "skipped_entries")) {
        if (skipped_entries->is_array()) {
            for (const auto& item : skipped_entries->array_items) {
                if (!item.is_object()) continue;
                UpgradeCatalogSkipEntry entry;
                entry.kind = json_string_or(json_object_get(item, "kind"));
                entry.trigger = json_string_or(json_object_get(item, "trigger"));
                entry.configured_name = json_string_or(json_object_get(item, "configured_name"));
                entry.resolved_name = json_string_or(json_object_get(item, "resolved_name"));
                entry.reason = json_string_or(json_object_get(item, "reason"));
                if (!entry.kind.empty() && !entry.configured_name.empty()) {
                    out_catalog.skipped_entries.push_back(entry);
                }
            }
        }
    }

    return true;
}

void invalidate_repo_package_cache() {
    g_repo_package_cache.clear();
    g_repo_provider_cache.clear();
    g_repo_available_package_cache.clear();
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
    get_json_value(obj, "installed_size_bytes", meta.installed_size_bytes);
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

    g_repo_available_package_cache.clear();
    for (const auto& entry : packages) {
        if (!entry.first.empty()) g_repo_available_package_cache.insert(entry.first);
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

bool query_full_universe_exact_package(
    const std::string& pkg_name,
    PackageUniverseResult& out_result,
    bool verbose,
    RawDebianContext* raw_context
) {
    out_result = {};
    std::string canonical_name = canonicalize_package_name(pkg_name, verbose);
    if (canonical_name.empty()) {
        out_result.reason = "invalid package name";
        return false;
    }

    if (try_ensure_repo_package_cache_loaded(verbose)) {
        PackageMetadata repo_meta;
        if (get_loaded_repo_package_info(canonical_name, repo_meta)) {
            out_result.found = true;
            out_result.installable = true;
            out_result.raw_only = false;
            out_result.meta = repo_meta;
            return true;
        }

        auto provider_it = g_repo_provider_cache.find(canonical_name);
        if (provider_it != g_repo_provider_cache.end()) {
            bool found = false;
            PackageMetadata best_meta;
            RelationAtom relation;
            relation.name = canonical_name;
            relation.valid = true;
            relation.normalized = canonical_name;
            for (const auto& provider_name : provider_it->second) {
                PackageMetadata candidate;
                if (!get_loaded_repo_package_info(provider_name, candidate)) continue;
                if (!catalog_meta_satisfies_relation(provider_name, candidate, relation)) continue;
                if (!found || should_prefer_repo_candidate(candidate, best_meta)) {
                    best_meta = candidate;
                    found = true;
                }
            }
            if (found) {
                out_result.found = true;
                out_result.installable = true;
                out_result.raw_only = false;
                out_result.meta = best_meta;
                return true;
            }
        }
    }

    if (!raw_context) {
        out_result.reason = "package is absent from the curated local package universe";
        return false;
    }

    RawDebianAvailabilityResult raw_result;
    std::string raw_reason;
    if (query_raw_debian_exact_package(
            canonical_name,
            *raw_context,
            raw_result,
            verbose,
            &raw_reason
        )) {
        out_result.found = raw_result.found;
        out_result.installable = raw_result.installable;
        out_result.raw_only = true;
        out_result.reason = raw_result.reason;
        out_result.meta = raw_result.meta;
        return true;
    }

    RawDebianAvailabilityResult raw_relation_result;
    std::string raw_relation_reason;
    if (query_raw_debian_relation_availability(
            canonical_name,
            "",
            "",
            *raw_context,
            raw_relation_result,
            verbose,
            &raw_relation_reason
        )) {
        out_result.found = raw_relation_result.found;
        out_result.installable = raw_relation_result.installable;
        out_result.raw_only = true;
        out_result.reason = raw_relation_result.reason.empty() ? raw_relation_reason : raw_relation_result.reason;
        out_result.meta = raw_relation_result.meta;
        return true;
    }

    out_result.reason = !raw_reason.empty()
        ? raw_reason
        : (raw_relation_reason.empty() ? "package is absent from cached Debian metadata" : raw_relation_reason);
    return false;
}

bool resolve_full_universe_relation_candidate(
    const std::string& pkg_name,
    const std::string& op,
    const std::string& req_version,
    PackageUniverseResult& out_result,
    bool verbose,
    RawDebianContext* raw_context,
    const PackageMetadata* installed_meta
) {
    out_result = {};
    std::string canonical_name = canonicalize_package_name(pkg_name, verbose);
    if (canonical_name.empty()) {
        out_result.reason = "invalid package relation";
        return false;
    }

    if (try_ensure_repo_package_cache_loaded(verbose)) {
        PackageMetadata repo_meta;
        if (get_loaded_repo_package_info(canonical_name, repo_meta) &&
            catalog_version_satisfies(repo_meta.version, op, req_version)) {
            out_result.found = true;
            out_result.installable = true;
            out_result.meta = repo_meta;
            return true;
        }

        RelationAtom relation;
        relation.name = canonical_name;
        relation.op = op;
        relation.version = req_version;
        relation.valid = !relation.name.empty();
        relation.normalized = relation.op.empty()
            ? relation.name
            : (relation.name + " (" + relation.op + " " + relation.version + ")");
        auto provider_it = g_repo_provider_cache.find(canonical_name);
        if (provider_it != g_repo_provider_cache.end()) {
            bool found = false;
            PackageMetadata best_meta;
            for (const auto& provider_name : provider_it->second) {
                PackageMetadata candidate;
                if (!get_loaded_repo_package_info(provider_name, candidate)) continue;
                if (!catalog_meta_satisfies_relation(provider_name, candidate, relation)) continue;
                if (!found || should_prefer_repo_candidate(candidate, best_meta)) {
                    best_meta = candidate;
                    found = true;
                }
            }
            if (found) {
                out_result.found = true;
                out_result.installable = true;
                out_result.meta = best_meta;
                return true;
            }
        }
    }

    if (!raw_context) {
        out_result.reason = "no repository package or provider candidate";
        return false;
    }

    std::vector<std::string> raw_queries;
    auto append_query = [&](const std::string& value) {
        if (value.empty()) return;
        std::string canonical = canonicalize_package_name(value, verbose);
        if (canonical.empty()) return;
        if (std::find(raw_queries.begin(), raw_queries.end(), canonical) == raw_queries.end()) {
            raw_queries.push_back(canonical);
        }
    };
    if (installed_meta && !installed_meta->debian_package.empty()) {
        append_query(installed_meta->debian_package);
    }
    append_query(canonical_name);

    std::string best_reason;
    for (const auto& query_name : raw_queries) {
        RawDebianAvailabilityResult raw_result;
        std::string raw_reason;
        if (resolve_raw_debian_relation_candidate(
                query_name,
                op,
                req_version,
                *raw_context,
                raw_result,
                verbose,
                &raw_reason
            )) {
            out_result.found = true;
            out_result.installable = true;
            out_result.raw_only = true;
            out_result.reason.clear();
            out_result.meta = raw_result.meta;
            return true;
        }
        if (best_reason.empty() && !raw_reason.empty()) best_reason = raw_reason;
    }

    out_result.reason = best_reason.empty()
        ? "no cached Debian candidate satisfies the required relation"
        : best_reason;
    return false;
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
    if (!ensure_repo_package_cache_loaded(verbose)) {
        std::cerr << Color::RED << "E: Failed to reload the merged package index after update."
                  << Color::RESET << std::endl;
        return 1;
    }

    ResolvedUpgradeCatalog upgrade_catalog;
    std::string catalog_error;
    if (!build_resolved_upgrade_catalog(upgrade_catalog, verbose, &catalog_error)) {
        std::cerr << Color::RED << "E: Failed to build the runtime upgrade catalog";
        if (!catalog_error.empty()) std::cerr << ": " << catalog_error;
        std::cerr << Color::RESET << std::endl;
        return 1;
    }
    if (!write_upgrade_catalog(upgrade_catalog, &catalog_error)) {
        std::cerr << Color::RED << "E: Failed to write the runtime upgrade catalog";
        if (!catalog_error.empty()) std::cerr << ": " << catalog_error;
        std::cerr << Color::RESET << std::endl;
        return 1;
    }

    std::cout << Color::GREEN << "✓ Merged " << total_packages << " packages from "
              << success_count << " sources." << Color::RESET << std::endl;
    std::cout << Color::GREEN << "✓ Wrote validated upgrade catalog for "
              << upgrade_catalog.resolved_roots.size() << " upgrade "
              << (upgrade_catalog.resolved_roots.size() == 1 ? "root" : "roots")
              << Color::RESET << std::endl;
    if (!upgrade_catalog.skipped_entries.empty()) {
        std::cout << Color::YELLOW << "W: Skipped "
                  << upgrade_catalog.skipped_entries.size()
                  << " configured runtime upgrade entr"
                  << (upgrade_catalog.skipped_entries.size() == 1 ? "y" : "ies")
                  << " during catalog validation:" << Color::RESET << std::endl;
        for (const auto& entry : upgrade_catalog.skipped_entries) {
            std::cout << "  " << Color::YELLOW << entry.kind << " "
                      << entry.configured_name;
            if (!entry.trigger.empty()) std::cout << " (trigger " << entry.trigger << ")";
            std::cout << ": " << entry.reason << Color::RESET << std::endl;
        }
    }
    return 0;
}

struct SearchResultDisplay {
    PackageMetadata meta;
    bool on_demand = false;
    bool installable = true;
    std::string reason;
};

std::string search_result_channel_label(const SearchResultDisplay& result) {
    if (result.meta.source_kind == "debian") return "debian";
    if (result.meta.source_kind == "gpkg_repo") return "repo";
    return result.meta.source_kind.empty() ? "unknown" : result.meta.source_kind;
}

size_t find_case_insensitive_substring(const std::string& haystack, const std::string& normalized_needle) {
    if (normalized_needle.empty()) return 0;
    return ascii_lower_copy(haystack).find(normalized_needle);
}

struct SearchResultSortKey {
    int bucket = 100;
    size_t position = std::numeric_limits<size_t>::max();
};

SearchResultSortKey compute_search_result_sort_key(const SearchResultDisplay& result, const std::string& normalized_query) {
    const auto& meta = result.meta;

    size_t pos = find_case_insensitive_substring(meta.name, normalized_query);
    if (pos == 0 && ascii_lower_copy(meta.name) == normalized_query) return {0, 0};
    if (!meta.debian_package.empty()) {
        size_t debian_pos = find_case_insensitive_substring(meta.debian_package, normalized_query);
        if (debian_pos == 0 && ascii_lower_copy(meta.debian_package) == normalized_query) return {1, 0};
    }
    if (pos == 0) return {2, 0};
    if (!meta.debian_package.empty()) {
        size_t debian_pos = find_case_insensitive_substring(meta.debian_package, normalized_query);
        if (debian_pos == 0) return {3, 0};
    }
    if (pos != std::string::npos) return {4, pos};
    if (!meta.debian_package.empty()) {
        size_t debian_pos = find_case_insensitive_substring(meta.debian_package, normalized_query);
        if (debian_pos != std::string::npos) return {5, debian_pos};
    }

    pos = find_case_insensitive_substring(meta.description, normalized_query);
    if (pos == 0) return {6, 0};
    if (pos != std::string::npos) return {7, pos};

    return {};
}

std::string render_search_result_display(const SearchResultDisplay& result) {
    const auto& meta = result.meta;

    std::string installed_ver;
    std::vector<std::string> flags;
    if (is_installed(meta.name, &installed_ver)) {
        if (compare_versions(installed_ver, meta.version) == 0) {
            flags.push_back(Color::BLUE + "[installed]" + Color::RESET);
        } else {
            flags.push_back(Color::BLUE + "[installed: " + installed_ver + "]" + Color::RESET);
        }
    } else if (package_is_base_system_provided(meta.name)) {
        flags.push_back(Color::BLUE + "[base system]" + Color::RESET);
    }

    std::ostringstream out;
    out << Color::GREEN << meta.name << Color::RESET
        << "/" << Color::CYAN << search_result_channel_label(result) << Color::RESET
        << " " << meta.version;
    for (const auto& flag : flags) {
        out << " " << flag;
    }
    out << std::endl;

    std::string summary = description_summary(meta.description);
    if (!summary.empty()) out << "  " << summary << std::endl;
    return out.str();
}

int handle_search(const std::string& query, bool verbose) {
    bool have_repo_cache = try_ensure_repo_package_cache_loaded(verbose);
    VLOG(verbose, "Searching for '" << query << "' in the local package universe");
    const std::string normalized_query = ascii_lower_copy(query);
    std::map<std::string, SearchResultDisplay> matches;
    if (have_repo_cache) {
        for (const auto& entry : g_repo_package_cache) {
            const PackageMetadata& meta = entry.second;
            if (find_case_insensitive_substring(meta.name, normalized_query) != std::string::npos ||
                find_case_insensitive_substring(meta.description, normalized_query) != std::string::npos) {
                auto it = matches.find(meta.name);
                if (it == matches.end() || should_prefer_repo_candidate(meta, it->second.meta)) {
                    SearchResultDisplay display;
                    display.meta = meta;
                    matches[meta.name] = display;
                }
            }
        }
    }

    std::string preview_error;
    const auto* preview_cache = get_debian_search_preview_cache(verbose, &preview_error);
    bool preview_available = preview_cache != nullptr;
    if (preview_available) {
        for (const auto& entry : *preview_cache) {
            const auto& preview = entry.second;
            if (matches.count(preview.meta.name) != 0) continue;

            bool matched = find_case_insensitive_substring(preview.meta.name, normalized_query) != std::string::npos ||
                find_case_insensitive_substring(preview.meta.description, normalized_query) != std::string::npos;
            if (!matched &&
                find_case_insensitive_substring(preview.meta.debian_package, normalized_query) != std::string::npos) {
                matched = true;
            }
            if (!matched) {
                for (const auto& raw_name : preview.raw_names) {
                    if (find_case_insensitive_substring(raw_name, normalized_query) != std::string::npos) {
                        matched = true;
                        break;
                    }
                }
            }
            if (!matched) continue;

            SearchResultDisplay display;
            display.meta = preview.meta;
            display.on_demand = true;
            display.installable = preview.installable;
            display.reason = preview.reason;
            matches[display.meta.name] = display;
        }
    }

    RawDebianContext raw_context;
    std::string raw_load_error;
    bool raw_available = false;
    if (!preview_available) {
        raw_available = ensure_raw_debian_context_loaded(raw_context, verbose, &raw_load_error);
        std::map<std::string, RawDebianAvailabilityResult> raw_matches;
        auto should_prefer_raw_search_result = [&](const RawDebianAvailabilityResult& candidate,
                                                   const RawDebianAvailabilityResult& current) {
            if (candidate.installable != current.installable) return candidate.installable;
            return compare_versions(candidate.meta.version, current.meta.version) > 0;
        };
        if (raw_available) {
            for (const auto& entry : raw_context.import_name_to_raw_names) {
                bool matched = find_case_insensitive_substring(entry.first, normalized_query) != std::string::npos;
                if (!matched) {
                    for (const auto& raw_name : entry.second) {
                        if (find_case_insensitive_substring(raw_name, normalized_query) != std::string::npos) {
                            matched = true;
                            break;
                        }
                    }
                }

                RawDebianAvailabilityResult result;
                std::string raw_reason;
                if (!query_raw_debian_exact_package(entry.first, raw_context, result, verbose, &raw_reason)) {
                    continue;
                }
                if (!matched &&
                    find_case_insensitive_substring(result.meta.description, normalized_query) == std::string::npos) {
                    continue;
                }
                if (g_repo_package_cache.count(result.meta.name) != 0) continue;

                auto existing = raw_matches.find(result.meta.name);
                if (existing == raw_matches.end() ||
                    should_prefer_raw_search_result(result, existing->second)) {
                    raw_matches[result.meta.name] = result;
                }
            }
        }

        for (const auto& entry : raw_matches) {
            SearchResultDisplay display;
            display.meta = entry.second.meta;
            display.on_demand = true;
            display.installable = entry.second.installable;
            display.reason = entry.second.reason;
            matches[display.meta.name] = display;
        }
    }

    if (matches.empty()) {
        if (!preview_available && !raw_available) {
            std::cerr << Color::RED << "E: "
                      << (!preview_error.empty()
                              ? preview_error
                              : (raw_load_error.empty()
                                      ? "cached Debian metadata is unavailable; run 'gpkg update'"
                                      : raw_load_error))
                      << Color::RESET << std::endl;
            return 1;
        }
        std::cout << "No matches found for '" << query << "'" << std::endl;
        return 0;
    }

    std::vector<SearchResultDisplay> ordered_matches;
    ordered_matches.reserve(matches.size());
    for (const auto& entry : matches) {
        ordered_matches.push_back(entry.second);
    }
    std::stable_sort(ordered_matches.begin(), ordered_matches.end(),
        [&](const SearchResultDisplay& lhs, const SearchResultDisplay& rhs) {
            SearchResultSortKey lhs_key = compute_search_result_sort_key(lhs, normalized_query);
            SearchResultSortKey rhs_key = compute_search_result_sort_key(rhs, normalized_query);
            if (lhs_key.bucket != rhs_key.bucket) return lhs_key.bucket < rhs_key.bucket;
            if (lhs_key.position != rhs_key.position) return lhs_key.position < rhs_key.position;

            std::string lhs_name = ascii_lower_copy(lhs.meta.name);
            std::string rhs_name = ascii_lower_copy(rhs.meta.name);
            if (lhs_name != rhs_name) return lhs_name < rhs_name;
            return compare_versions(lhs.meta.version, rhs.meta.version) > 0;
        });

    std::ostringstream rendered;
    for (const auto& result : ordered_matches) {
        rendered << render_search_result_display(result);
    }

    const std::string output = rendered.str();
    if (!write_text_via_pager(output, verbose)) {
        std::cout << output;
    }

    return 0;
}

int handle_show(const std::string& pkg_name, bool verbose) {
    VLOG(verbose, "Showing package metadata for '" << pkg_name << "'");
    RawDebianContext raw_context;
    PackageUniverseResult result;
    if (!query_full_universe_exact_package(pkg_name, result, verbose, &raw_context)) {
        DebianSearchPreviewEntry preview;
        std::string preview_error;
        if (get_debian_search_preview_exact_package(pkg_name, preview, verbose, &preview_error)) {
            result.found = true;
            result.installable = preview.installable;
            result.raw_only = true;
            result.reason = preview.reason;
            result.meta = preview.meta;
        } else {
            std::cerr << Color::RED << "E: Package '" << pkg_name << "' was not found in the local package universe";
            if (!result.reason.empty()) std::cerr << " (" << result.reason << ")";
            std::cerr << "." << Color::RESET << std::endl;
            return 1;
        }
    }
    PackageMetadata meta = result.meta;

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
    if (result.raw_only) {
        std::cout << "  Availability: "
                  << (result.installable
                          ? "available via on-demand Debian install"
                          : ("unavailable (" + result.reason + ")"))
                  << std::endl;
    }

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
