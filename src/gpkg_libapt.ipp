// libapt-pkg-backed Debian transaction planning.

#if defined(GPKG_HAVE_WORKING_LIBAPT_PKG_BACKEND)
#include <apt-pkg/algorithms.h>
#include <apt-pkg/cachefile.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/init.h>
#include <apt-pkg/pkgsystem.h>
#include <apt-pkg/pkgrecords.h>
#endif

struct LibAptPlannedInstallAction {
    PackageMetadata meta;
    std::string current_version;
    bool was_installed = false;
    bool reinstall_only = false;
    bool explicit_target = false;
};

struct LibAptTransactionPlanResult {
    bool success = false;
    std::string error;
    std::vector<LibAptPlannedInstallAction> install_actions;
    std::vector<std::string> remove_packages;
    std::vector<std::string> purge_packages;
    std::map<std::string, bool> auto_state_after;
};

bool package_is_config_files_only(const std::string& pkg_name, std::string* out_version);
bool get_live_package_metadata_for_upgrade_resolution(
    const std::string& pkg_name,
    PackageMetadata& out_meta,
    UpgradeContext* context
);
bool resolve_upgrade_target_metadata(
    const Dependency& requested_dep,
    PackageMetadata& out_meta,
    bool verbose,
    RawDebianContext* raw_context,
    const PackageMetadata* installed_meta,
    std::string* reason_out
);

#if defined(GPKG_HAVE_WORKING_LIBAPT_PKG_BACKEND)

struct ScopedLibAptSessionRoot {
    std::string path;

    ~ScopedLibAptSessionRoot() {
        if (!path.empty()) remove_path_recursive(path);
    }
};

std::string libapt_sanitize_cache_key(const std::string& value) {
    std::string sanitized;
    sanitized.reserve(value.size() + 8);
    for (char ch : value) {
        unsigned char uch = static_cast<unsigned char>(ch);
        if (std::isalnum(uch) || ch == '.' || ch == '-') sanitized += ch;
        else sanitized += '_';
    }
    return sanitized;
}

std::string libapt_seeded_packages_list_name(const std::string& repo_dir) {
    return libapt_sanitize_cache_key(repo_dir) + "._Packages";
}

bool libapt_copy_file(const std::string& src, const std::string& dst, std::string* error_out = nullptr) {
    if (error_out) error_out->clear();

    std::ifstream in(src, std::ios::binary);
    if (!in) {
        if (error_out) *error_out = "failed to open " + src + " for reading";
        return false;
    }

    if (!mkdir_parent(dst)) {
        if (error_out) *error_out = "failed to create parent directory for " + dst;
        return false;
    }

    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error_out) *error_out = "failed to open " + dst + " for writing";
        return false;
    }

    out << in.rdbuf();
    if (!out.good()) {
        if (error_out) *error_out = "failed to copy " + src + " to " + dst;
        return false;
    }

    return true;
}

bool libapt_write_text_file(const std::string& path, const std::string& content, std::string* error_out = nullptr) {
    if (error_out) error_out->clear();
    if (!mkdir_parent(path)) {
        if (error_out) *error_out = "failed to create parent directory for " + path;
        return false;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error_out) *error_out = "failed to open " + path + " for writing";
        return false;
    }

    out << content;
    if (!out.good()) {
        if (error_out) *error_out = "failed to write " + path;
        return false;
    }
    return true;
}

bool libapt_prepare_session_root(
    ScopedLibAptSessionRoot& session_root,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    char temp_template[] = "/tmp/gpkg-libapt-XXXXXX";
    char* created = mkdtemp(temp_template);
    if (!created) {
        if (error_out) *error_out = "failed to create a temporary libapt-pkg workspace";
        return false;
    }

    session_root.path = created;
    std::vector<std::string> needed_dirs = {
        session_root.path + "/etc",
        session_root.path + "/etc/sources.list.d",
        session_root.path + "/state",
        session_root.path + "/state/lists",
        session_root.path + "/state/lists/partial",
        session_root.path + "/cache",
        session_root.path + "/cache/archives",
        session_root.path + "/cache/archives/partial",
    };
    for (const auto& dir : needed_dirs) {
        if (!mkdir_p(dir)) {
            if (error_out) *error_out = "failed to prepare " + dir;
            return false;
        }
    }

    return true;
}

bool libapt_seed_debian_packages_index(
    const ScopedLibAptSessionRoot& session_root,
    const std::string& packages_path,
    bool verbose,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    std::string repo_dir = path_dirname(packages_path);
    std::string list_name = libapt_seeded_packages_list_name(repo_dir);
    std::string list_path = session_root.path + "/state/lists/" + list_name;
    if (!libapt_copy_file(packages_path, list_path, error_out)) return false;

    std::string source_list = "deb [trusted=yes] file:" + repo_dir + " ./\n";
    if (!libapt_write_text_file(session_root.path + "/etc/sources.list", source_list, error_out)) {
        return false;
    }

    if (!libapt_write_text_file(session_root.path + "/state/extended_states", "", error_out)) {
        return false;
    }

    VLOG(verbose, "Seeded libapt-pkg session from " << packages_path
                 << " into " << session_root.path);
    return true;
}

bool libapt_initialize_globals(
    const ScopedLibAptSessionRoot& session_root,
    const DebianBackendConfig& config,
    bool verbose,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    _config = new Configuration;
    if (!pkgInitConfig(*_config)) {
        if (error_out) *error_out = "pkgInitConfig failed";
        return false;
    }

    _config->Set("Dir", session_root.path + "/");
    _config->Set("Dir::Etc::sourcelist", session_root.path + "/etc/sources.list");
    _config->Set("Dir::Etc::sourceparts", session_root.path + "/etc/sources.list.d");
    _config->Set("Dir::State::status", DPKG_STATUS_FILE);
    _config->Set("Dir::State::lists", session_root.path + "/state/lists/");
    _config->Set("Dir::State::extended_states", session_root.path + "/state/extended_states");
    _config->Set("Dir::Cache::pkgcache", session_root.path + "/cache/pkgcache.bin");
    _config->Set("Dir::Cache::srcpkgcache", session_root.path + "/cache/srcpkgcache.bin");
    _config->Set("Dir::Cache::archives", session_root.path + "/cache/archives/");
    _config->Set("Debug::NoLocking", "true");
    _config->Set("APT::Architecture", config.apt_arch);
    _config->Set("Acquire::Languages", "none");
    _config->Set(
        "APT::Install-Recommends",
        (g_optional_dependency_policy.recommends == OptionalDependencyMode::ForceYes) ? "true" : "false"
    );
    _config->Set(
        "APT::Install-Suggests",
        (g_optional_dependency_policy.suggests == OptionalDependencyMode::ForceYes) ? "true" : "false"
    );

    pkgSystem* sys = nullptr;
    if (!pkgInitSystem(*_config, sys) || sys == nullptr) {
        if (error_out) *error_out = "pkgInitSystem failed";
        return false;
    }
    _system = sys;

    VLOG(verbose, "Initialized libapt-pkg for architecture " << config.apt_arch);
    return true;
}

bool libapt_build_cache_file(
    pkgCacheFile& cache_file,
    bool verbose,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    if (!cache_file.BuildSourceList(nullptr)) {
        if (error_out) *error_out = "failed to build apt source list";
        return false;
    }
    if (!cache_file.BuildCaches(nullptr, false)) {
        if (error_out) *error_out = "failed to build apt package caches";
        return false;
    }
    if (!cache_file.BuildPolicy(nullptr)) {
        if (error_out) *error_out = "failed to build apt policy";
        return false;
    }
    if (!cache_file.BuildDepCache(nullptr)) {
        if (error_out) *error_out = "failed to build apt dependency cache";
        return false;
    }

    VLOG(verbose, "Built libapt-pkg cache with "
                     << cache_file.GetPkgCache()->HeaderP->PackageCount
                     << " package entries.");
    return true;
}

void libapt_seed_auto_install_state(pkgCacheFile& cache_file) {
    pkgDepCache& cache = *cache_file;
    for (pkgCache::PkgIterator pkg = cache.PkgBegin(); pkg.end() == false; ++pkg) {
        bool auto_installed = false;
        if (get_package_auto_installed_state(pkg.Name(), &auto_installed)) {
            cache.MarkAuto(pkg, auto_installed);
        }
    }
    cache.MarkAndSweep();
}

bool libapt_find_package(
    pkgCacheFile& cache_file,
    const std::string& name,
    pkgCache::PkgIterator& out_pkg,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    pkgDepCache& cache = *cache_file;
    out_pkg = cache.FindPkg(name);
    if (!out_pkg.end()) return true;

    if (error_out) *error_out = "package '" + name + "' is not present in the seeded apt cache";
    return false;
}

bool libapt_resolve_metadata_for_candidate(
    const std::string& pkg_name,
    const std::string& version,
    RawDebianContext& raw_context,
    bool verbose,
    PackageMetadata& out_meta,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    RawDebianAvailabilityResult raw_result;
    std::string reason;
    if (resolve_raw_debian_relation_candidate(
            pkg_name,
            version.empty() ? "" : "=",
            version,
            raw_context,
            raw_result,
            verbose,
            &reason
        ) &&
        raw_result.found) {
        out_meta = raw_result.meta;
        return true;
    }

    if (query_raw_debian_exact_package(pkg_name, raw_context, raw_result, verbose, &reason) &&
        raw_result.found) {
        out_meta = raw_result.meta;
        return true;
    }

    if (error_out) {
        *error_out = reason.empty()
            ? "no Debian metadata candidate is available for " + pkg_name
            : reason;
    }
    return false;
}

bool libapt_extract_transaction_result(
    pkgCacheFile& cache_file,
    const std::set<std::string>& explicit_targets,
    bool include_garbage_removals,
    bool purge_garbage,
    bool verbose,
    LibAptTransactionPlanResult& out_result,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    out_result = {};

    RawDebianContext raw_context;
    pkgDepCache& cache = *cache_file;
    pkgCache& pkg_cache = cache.GetCache();

    for (pkgCache::PkgIterator pkg = cache.PkgBegin(); pkg.end() == false; ++pkg) {
        pkgDepCache::StateCache& state = cache[pkg];
        bool currently_present =
            pkg->CurrentVer != 0 ||
            package_is_config_files_only(pkg.Name(), nullptr);

        bool should_remove =
            state.Delete() ||
            (include_garbage_removals && state.Garbage && currently_present);
        if (should_remove && currently_present) {
            std::string pkg_name = pkg.Name();
            out_result.remove_packages.push_back(pkg_name);
            if (state.Purge() || purge_garbage) out_result.purge_packages.push_back(pkg_name);
        }

        if (!state.Install() && !state.ReInstall()) continue;

        pkgCache::VerIterator install_version = state.InstVerIter(pkg_cache);
        if (install_version.end()) continue;

        PackageMetadata meta;
        std::string metadata_error;
        if (!libapt_resolve_metadata_for_candidate(
                pkg.Name(),
                install_version.VerStr(),
                raw_context,
                verbose,
                meta,
                &metadata_error
            )) {
            if (error_out) *error_out = metadata_error;
            return false;
        }

        LibAptPlannedInstallAction action;
        action.meta = meta;
        action.current_version = pkg->CurrentVer == 0 ? "" : pkg.CurrentVer().VerStr();
        action.was_installed = currently_present;
        action.reinstall_only =
            action.was_installed &&
            !action.current_version.empty() &&
            compare_versions(action.current_version, meta.version) == 0;
        action.explicit_target = explicit_targets.count(pkg.Name()) != 0;
        out_result.install_actions.push_back(action);
        out_result.auto_state_after[pkg.Name()] =
            (state.Flags & pkgCache::Flag::Auto) == pkgCache::Flag::Auto;
    }

    auto dedupe_names = [](std::vector<std::string>& names) {
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
    };
    dedupe_names(out_result.remove_packages);
    dedupe_names(out_result.purge_packages);
    out_result.success = true;
    return true;
}

bool libapt_open_seeded_cache(
    const std::string& packages_path,
    bool verbose,
    ScopedLibAptSessionRoot& session_root,
    pkgCacheFile& cache_file,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    DebianBackendConfig config = load_debian_backend_config(verbose);
    if (!libapt_prepare_session_root(session_root, error_out)) return false;
    if (!libapt_seed_debian_packages_index(session_root, packages_path, verbose, error_out)) return false;
    if (!libapt_initialize_globals(session_root, config, verbose, error_out)) return false;
    if (!libapt_build_cache_file(cache_file, verbose, error_out)) return false;
    libapt_seed_auto_install_state(cache_file);
    return true;
}

bool libapt_plan_install_like_transaction(
    const std::vector<std::string>& explicit_targets,
    const std::set<std::string>& reinstall_targets,
    bool fix_broken,
    bool verbose,
    LibAptTransactionPlanResult& out_result,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    out_result = {};

    std::string packages_path = get_debian_packages_cache_path();
    if (access(packages_path.c_str(), F_OK) != 0) {
        if (error_out) *error_out = "Debian Packages cache is missing; run 'gpkg update'";
        return false;
    }

    ScopedLibAptSessionRoot session_root;
    pkgCacheFile cache_file;
    if (!libapt_open_seeded_cache(packages_path, verbose, session_root, cache_file, error_out)) {
        return false;
    }

    pkgDepCache& cache = *cache_file;
    for (const auto& target : explicit_targets) {
        pkgCache::PkgIterator pkg;
        std::string lookup_error;
        if (!libapt_find_package(cache_file, target, pkg, &lookup_error)) {
            if (error_out) *error_out = lookup_error;
            return false;
        }

        {
            pkgDepCache::ActionGroup group(cache);
            if (!cache.MarkInstall(pkg, true, 0, true)) {
                if (error_out) *error_out = "apt refused to mark " + target + " for installation";
                return false;
            }
            if (reinstall_targets.count(target) != 0) cache.SetReInstall(pkg, true);
        }
    }

    if (fix_broken && !pkgFixBroken(cache)) {
        if (error_out) *error_out = "apt could not repair broken dependency state";
        return false;
    }

    pkgProblemResolver resolver(&cache);
    if (!resolver.Resolve(fix_broken)) {
        if (error_out) *error_out = "apt could not solve the requested transaction";
        return false;
    }

    return libapt_extract_transaction_result(
        cache_file,
        std::set<std::string>(explicit_targets.begin(), explicit_targets.end()),
        false,
        false,
        verbose,
        out_result,
        error_out
    );
}

bool libapt_plan_remove_transaction(
    const std::vector<std::string>& explicit_targets,
    bool purge,
    bool autoremove,
    bool verbose,
    LibAptTransactionPlanResult& out_result,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    out_result = {};

    std::string packages_path = get_debian_packages_cache_path();
    if (access(packages_path.c_str(), F_OK) != 0) {
        if (error_out) *error_out = "Debian Packages cache is missing; run 'gpkg update'";
        return false;
    }

    ScopedLibAptSessionRoot session_root;
    pkgCacheFile cache_file;
    if (!libapt_open_seeded_cache(packages_path, verbose, session_root, cache_file, error_out)) {
        return false;
    }

    pkgDepCache& cache = *cache_file;
    for (const auto& target : explicit_targets) {
        pkgCache::PkgIterator pkg;
        std::string lookup_error;
        if (!libapt_find_package(cache_file, target, pkg, &lookup_error)) {
            if (error_out) *error_out = lookup_error;
            return false;
        }
        {
            pkgDepCache::ActionGroup group(cache);
            if (!cache.MarkDelete(pkg, purge, 0, true)) {
                if (error_out) *error_out = "apt refused to mark " + target + " for removal";
                return false;
            }
        }
    }

    if (autoremove) cache.MarkAndSweep();

    return libapt_extract_transaction_result(
        cache_file,
        std::set<std::string>(explicit_targets.begin(), explicit_targets.end()),
        autoremove,
        purge,
        verbose,
        out_result,
        error_out
    );
}

#endif

bool libapt_can_handle_repo_install_operands(
    const std::vector<std::string>& repo_operands,
    bool verbose,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    if (repo_operands.empty()) return false;

    RawDebianContext raw_context;
    for (const auto& operand : repo_operands) {
        PackageUniverseResult result;
        if (!resolve_full_universe_relation_candidate(
                canonicalize_package_name(operand, verbose),
                "",
                "",
                result,
                verbose,
                &raw_context
            )) {
            if (error_out) *error_out = "no repository candidate is available for " + operand;
            return false;
        }
        if (!package_is_debian_source(result.meta)) {
            if (error_out) {
                *error_out = operand + " resolves to a non-Debian package, so the legacy planner is still required";
            }
            return false;
        }
    }

    return true;
}

bool libapt_can_handle_upgrade_roots(
    UpgradeContext& context,
    const std::vector<std::string>& normalized_roots,
    bool verbose,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    if (normalized_roots.empty()) return false;

    RawDebianContext raw_context;
    for (const auto& pkg : normalized_roots) {
        PackageMetadata repo_meta;
        std::string resolve_reason;
        PackageMetadata installed_meta;
        PackageMetadata* installed_meta_ptr = nullptr;
        if (get_live_package_metadata_for_upgrade_resolution(pkg, installed_meta, &context)) {
            installed_meta_ptr = &installed_meta;
        }
        if (!resolve_upgrade_target_metadata(
                {pkg, "", ""},
                repo_meta,
                verbose,
                &raw_context,
                installed_meta_ptr,
                &resolve_reason
            )) {
            if (error_out) *error_out = resolve_reason.empty() ? ("no upgrade candidate is available for " + pkg) : resolve_reason;
            return false;
        }
        if (!package_is_debian_source(repo_meta)) {
            if (error_out) {
                *error_out = pkg + " resolves to a non-Debian upgrade target, so the legacy planner is still required";
            }
            return false;
        }
    }

    return true;
}

bool libapt_can_handle_repair_queue(
    const std::vector<PackageMetadata>& repair_queue,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    if (repair_queue.empty()) return false;

    for (const auto& meta : repair_queue) {
        if (!package_is_debian_source(meta)) {
            if (error_out) {
                *error_out = meta.name + " is not a Debian-backed package, so the legacy repair path is still required";
            }
            return false;
        }
    }

    return true;
}

bool libapt_can_handle_remove_target(
    const std::string& pkg_name,
    bool purge
) {
    (void)purge;
    if (pkg_name.empty()) return false;
    return package_has_exact_live_install_state(pkg_name);
}

bool libapt_has_non_native_auto_installed_packages() {
    for (const auto& record : load_package_auto_state_records()) {
        if (!record.auto_installed || record.package.empty()) continue;
        if (package_has_exact_live_install_state(record.package) ||
            package_is_config_files_only(record.package, nullptr)) {
            continue;
        }
        return true;
    }
    return false;
}
