// libapt-pkg-backed Debian transaction planning.

#if defined(GPKG_HAVE_WORKING_LIBAPT_PKG_BACKEND)
#include <apt-pkg/algorithms.h>
#include <apt-pkg/cachefile.h>
#include <apt-pkg/configuration.h>
#include <apt-pkg/init.h>
#include <apt-pkg/packagemanager.h>
#include <apt-pkg/pkgsystem.h>
#include <apt-pkg/pkgrecords.h>
#endif

enum class LibAptOperationType {
    Install,
    Configure,
    Remove,
    Purge,
};

struct LibAptPlannedOperation {
    LibAptOperationType type = LibAptOperationType::Install;
    std::string apt_package_name;
    std::string file_path;
};

struct LibAptPlannedInstallAction {
    PackageMetadata meta;
    std::string apt_package_name;
    std::string current_version;
    bool was_installed = false;
    bool reinstall_only = false;
    bool explicit_target = false;
};

struct LibAptTransactionPlanResult {
    bool success = false;
    std::string error;
    std::vector<LibAptPlannedInstallAction> install_actions;
    std::vector<LibAptPlannedOperation> ordered_operations;
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
bool get_upgrade_target_current_version(
    const Dependency& requested_dep,
    const PackageMetadata& target_meta,
    std::string& current_version,
    bool verbose,
    UpgradeContext* context
);
std::string get_install_archive_path(const PackageMetadata& meta);
std::string get_cached_debian_archive_path(const PackageMetadata& meta);
bool libapt_plan_install_like_transaction(
    const std::vector<std::string>& explicit_targets,
    const std::set<std::string>& reinstall_targets,
    bool fix_broken,
    bool verbose,
    LibAptTransactionPlanResult& out_result,
    std::string* error_out
);
bool libapt_plan_remove_transaction(
    const std::vector<std::string>& explicit_targets,
    bool purge,
    bool autoremove,
    bool verbose,
    LibAptTransactionPlanResult& out_result,
    std::string* error_out
);
bool package_can_use_libapt_native_planner(const PackageMetadata& meta);
bool ensure_native_dpkg_backend_ready(bool verbose, std::string* error_out);

#if defined(GPKG_HAVE_WORKING_LIBAPT_PKG_BACKEND)

using LibAptClock = std::chrono::steady_clock;

std::string libapt_format_elapsed_ms(LibAptClock::time_point started_at) {
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        LibAptClock::now() - started_at
    ).count();
    return std::to_string(elapsed_ms) + " ms";
}

class ScopedLibAptDebugTimer {
    bool verbose_ = false;
    std::string label_;
    LibAptClock::time_point started_at_;

   public:
    ScopedLibAptDebugTimer(bool verbose, std::string label)
        : verbose_(verbose),
          label_(std::move(label)),
          started_at_(LibAptClock::now()) {
        VLOG(verbose_, label_ << "...");
    }

    void checkpoint(const std::string& detail) const {
        VLOG(verbose_, label_ << ": " << detail
                             << " (" << libapt_format_elapsed_ms(started_at_) << ")");
    }

    void finish(const std::string& detail = "done") const {
        VLOG(verbose_, label_ << ": " << detail
                             << " (" << libapt_format_elapsed_ms(started_at_) << ")");
    }
};

struct ScopedLibAptSessionRoot {
    std::string path;
    bool cleanup_on_destroy = false;

    ~ScopedLibAptSessionRoot() {
        if (cleanup_on_destroy && !path.empty()) remove_path_recursive(path);
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
    std::string normalized = repo_dir;
    if (!normalized.empty() && normalized.back() != '/') normalized += "/";
    return libapt_sanitize_cache_key(normalized) + "._Packages";
}

bool libapt_copy_file(const std::string& src, const std::string& dst, std::string* error_out);
bool libapt_write_text_file(const std::string& path, const std::string& content, std::string* error_out);
bool libapt_prime_planner_cache(bool verbose, std::string* error_out);

bool libapt_read_file_contents(
    const std::string& path,
    std::string& out_content,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    out_content.clear();

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error_out) *error_out = "failed to open " + path + " for reading";
        return false;
    }

    std::ostringstream buffer;
    buffer << in.rdbuf();
    if (!in.good() && !in.eof()) {
        if (error_out) *error_out = "failed to read " + path;
        return false;
    }

    out_content = buffer.str();
    return true;
}

bool libapt_files_have_same_contents(
    const std::string& left_path,
    const std::string& right_path,
    bool& same_out,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    same_out = false;

    struct stat left_stat {};
    if (stat(left_path.c_str(), &left_stat) != 0) {
        if (error_out) *error_out = "failed to stat " + left_path;
        return false;
    }

    struct stat right_stat {};
    if (stat(right_path.c_str(), &right_stat) != 0) {
        if (errno == ENOENT) return true;
        if (error_out) *error_out = "failed to stat " + right_path;
        return false;
    }

    if (left_stat.st_size != right_stat.st_size) return true;

    std::ifstream left(left_path, std::ios::binary);
    if (!left) {
        if (error_out) *error_out = "failed to open " + left_path + " for reading";
        return false;
    }
    std::ifstream right(right_path, std::ios::binary);
    if (!right) {
        if (error_out) *error_out = "failed to open " + right_path + " for reading";
        return false;
    }

    char left_buffer[32768];
    char right_buffer[32768];
    while (true) {
        left.read(left_buffer, sizeof(left_buffer));
        right.read(right_buffer, sizeof(right_buffer));

        std::streamsize left_count = left.gcount();
        std::streamsize right_count = right.gcount();
        if (left_count != right_count) return true;
        if (left_count == 0) break;
        if (std::memcmp(left_buffer, right_buffer, static_cast<size_t>(left_count)) != 0) {
            return true;
        }
    }

    if ((!left.good() && !left.eof()) || (!right.good() && !right.eof())) {
        if (error_out) *error_out = "failed while comparing " + left_path + " and " + right_path;
        return false;
    }

    same_out = true;
    return true;
}

void libapt_try_preserve_file_times(const std::string& path, const struct stat& source_stat) {
    utimbuf times {};
    times.actime = source_stat.st_atime;
    times.modtime = source_stat.st_mtime;
    (void)utime(path.c_str(), &times);
}

std::string libapt_make_absolute_path(const std::string& path) {
    if (path.empty() || path.front() == '/') return path;

    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) return path;
    return std::string(cwd) + "/" + path;
}

std::string libapt_normalize_architecture_name(
    const std::string& raw_arch,
    const std::string& fallback_arch
) {
    std::string arch = trim(raw_arch);
    if (arch.empty()) arch = fallback_arch;
    if (arch.empty()) return "amd64";

    std::string lowered = ascii_lower_copy(arch);
    if (lowered == "x86_64" || lowered == "amd64") return "amd64";
    if (lowered == "aarch64" || lowered == "arm64") return "arm64";
    if (lowered == "i386" || lowered == "i486" || lowered == "i586" || lowered == "i686") {
        return "i386";
    }
    if (lowered == "all" || lowered == "noarch") return "all";
    return lowered;
}

bool libapt_append_seeded_packages_source(
    const ScopedLibAptSessionRoot& session_root,
    const std::string& repo_dir,
    const std::string& packages_path,
    std::string& source_list,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    if (repo_dir.empty() || packages_path.empty()) {
        if (error_out) *error_out = "invalid seeded apt source";
        return false;
    }

    std::string normalized_repo_dir = repo_dir;
    if (!normalized_repo_dir.empty() && normalized_repo_dir.back() != '/') {
        normalized_repo_dir += "/";
    }
    normalized_repo_dir = libapt_make_absolute_path(normalized_repo_dir);

    std::string list_name = libapt_seeded_packages_list_name(normalized_repo_dir);
    std::string list_path = session_root.path + "/state/lists/" + list_name;
    if (!libapt_copy_file(packages_path, list_path, error_out)) return false;

    source_list += "deb [trusted=yes] file:" + normalized_repo_dir + " ./\n";
    return true;
}

std::string libapt_render_repo_native_packages_paragraph(
    const PackageMetadata& meta,
    const DebianBackendConfig& config
) {
    std::vector<std::string> lines;
    std::string package_name = canonicalize_package_name(meta.name);
    if (package_name.empty()) package_name = meta.name;
    std::string description = description_summary(meta.description, 200);
    if (description.empty()) description = "GeminiOS native package";

    lines.push_back("Package: " + package_name);
    lines.push_back("Version: " + trim(meta.version));
    lines.push_back("Architecture: " + libapt_normalize_architecture_name(meta.arch, config.apt_arch));

    std::string priority = trim(meta.priority);
    if (!priority.empty()) lines.push_back("Priority: " + priority);

    std::string section = trim(meta.section);
    if (!section.empty()) lines.push_back("Section: " + section);

    std::string maintainer = trim(meta.maintainer);
    if (!maintainer.empty()) lines.push_back("Maintainer: " + maintainer);

    std::string installed_size_text = trim(meta.installed_size_bytes);
    if (!installed_size_text.empty()) {
        char* end = nullptr;
        errno = 0;
        unsigned long long installed_size_bytes = std::strtoull(installed_size_text.c_str(), &end, 10);
        if (errno == 0 &&
            end != nullptr &&
            *end == '\0' &&
            installed_size_bytes > 0) {
            lines.push_back("Installed-Size: " + std::to_string((installed_size_bytes + 1023ULL) / 1024ULL));
        }
    }

    auto append_relation_field = [&](const std::string& key, const std::vector<std::string>& values) {
        std::vector<std::string> normalized;
        normalized.reserve(values.size());
        for (const auto& value : values) {
            std::string trimmed = trim(value);
            if (!trimmed.empty()) normalized.push_back(trimmed);
        }
        if (!normalized.empty()) lines.push_back(key + ": " + join_strings(normalized));
    };

    append_relation_field("Pre-Depends", meta.pre_depends);
    append_relation_field("Depends", meta.depends);
    append_relation_field("Recommends", meta.recommends);
    append_relation_field("Suggests", meta.suggests);
    append_relation_field("Breaks", meta.breaks);
    append_relation_field("Conflicts", meta.conflicts);
    append_relation_field("Provides", meta.provides);
    append_relation_field("Replaces", meta.replaces);

    if (!meta.filename.empty()) lines.push_back("Filename: " + meta.filename);
    if (!meta.size.empty()) lines.push_back("Size: " + trim(meta.size));
    if (!meta.sha256.empty()) lines.push_back("SHA256: " + trim(meta.sha256));
    lines.push_back("Description: " + description);
    return join_strings(lines, "\n");
}

bool libapt_seed_repo_native_packages_index(
    const ScopedLibAptSessionRoot& session_root,
    const DebianBackendConfig& config,
    std::string& source_list,
    bool verbose,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    if (!try_ensure_repo_package_cache_loaded(verbose)) return true;

    std::ostringstream packages_stream;
    size_t package_count = 0;
    for (const auto& package_name : g_repo_available_package_cache) {
        PackageMetadata meta;
        if (!get_loaded_repo_package_info(package_name, meta)) continue;
        if (meta.source_kind != "gpkg_repo") continue;
        if (trim(meta.name).empty() || trim(meta.version).empty()) continue;

        packages_stream << libapt_render_repo_native_packages_paragraph(meta, config) << "\n\n";
        ++package_count;
    }

    if (package_count == 0) return true;

    std::string repo_dir = session_root.path + "/repo-native";
    if (!mkdir_p(repo_dir)) {
        if (error_out) *error_out = "failed to prepare " + repo_dir;
        return false;
    }

    std::string packages_path = repo_dir + "/Packages";
    if (!libapt_write_text_file(packages_path, packages_stream.str(), error_out)) return false;
    if (!libapt_append_seeded_packages_source(
            session_root,
            repo_dir,
            packages_path,
            source_list,
            error_out
        )) {
        return false;
    }

    VLOG(verbose, "Seeded libapt-pkg with " << package_count
                 << " GeminiOS repository package entr"
                 << (package_count == 1 ? "y." : "ies."));
    return true;
}

bool libapt_copy_file(const std::string& src, const std::string& dst, std::string* error_out = nullptr) {
    if (error_out) error_out->clear();

    struct stat src_stat {};
    if (stat(src.c_str(), &src_stat) != 0) {
        if (error_out) *error_out = "failed to open " + src + " for reading";
        return false;
    }

    struct stat dst_stat {};
    if (stat(dst.c_str(), &dst_stat) == 0) {
        if (src_stat.st_size == dst_stat.st_size &&
            src_stat.st_mtime == dst_stat.st_mtime) {
            return true;
        }

        bool same_contents = false;
        if (!libapt_files_have_same_contents(src, dst, same_contents, error_out)) return false;
        if (same_contents) return true;
    } else if (errno != ENOENT) {
        if (error_out) *error_out = "failed to inspect " + dst;
        return false;
    }

    std::ifstream in(src, std::ios::binary);
    if (!in) {
        if (error_out) *error_out = "failed to open " + src + " for reading";
        return false;
    }

    if (!mkdir_parent(dst)) {
        if (error_out) *error_out = "failed to create parent directory for " + dst;
        return false;
    }

    std::string temp_path = dst + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error_out) *error_out = "failed to open " + temp_path + " for writing";
        return false;
    }

    out << in.rdbuf();
    if (!out.good()) {
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to copy " + src + " to " + dst;
        return false;
    }

    out.close();
    if (!out) {
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to finalize " + temp_path;
        return false;
    }

    if (rename(temp_path.c_str(), dst.c_str()) != 0) {
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to replace " + dst;
        return false;
    }

    libapt_try_preserve_file_times(dst, src_stat);
    return true;
}

bool libapt_write_text_file(const std::string& path, const std::string& content, std::string* error_out = nullptr) {
    if (error_out) error_out->clear();

    struct stat existing_file_stat {};
    if (stat(path.c_str(), &existing_file_stat) == 0) {
        (void)existing_file_stat;
        std::string existing_content;
        if (!libapt_read_file_contents(path, existing_content, error_out)) return false;
        if (existing_content == content) return true;
    } else if (errno != ENOENT) {
        if (error_out) *error_out = "failed to inspect " + path;
        return false;
    }

    if (!mkdir_parent(path)) {
        if (error_out) *error_out = "failed to create parent directory for " + path;
        return false;
    }

    std::string temp_path = path + ".tmp";
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error_out) *error_out = "failed to open " + temp_path + " for writing";
        return false;
    }

    out << content;
    if (!out.good()) {
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to write " + path;
        return false;
    }

    out.close();
    if (!out) {
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to finalize " + temp_path;
        return false;
    }

    if (rename(temp_path.c_str(), path.c_str()) != 0) {
        remove(temp_path.c_str());
        if (error_out) *error_out = "failed to replace " + path;
        return false;
    }
    return true;
}

bool libapt_prepare_session_root(
    ScopedLibAptSessionRoot& session_root,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    session_root.path = libapt_make_absolute_path(ROOT_PREFIX + "/var/lib/gpkg/libapt-session");
    session_root.cleanup_on_destroy = false;
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
    const DebianBackendConfig& config,
    bool verbose,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    std::string repo_dir = path_dirname(packages_path);
    std::string source_list;
    if (!libapt_append_seeded_packages_source(
            session_root,
            repo_dir,
            packages_path,
            source_list,
            error_out
        )) {
        return false;
    }

    if (!libapt_seed_repo_native_packages_index(session_root, config, source_list, verbose, error_out)) {
        return false;
    }

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
    _config->Set("Dir::State::status", libapt_make_absolute_path(DPKG_STATUS_FILE));
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
    ScopedLibAptDebugTimer debug_timer(verbose, "libapt-pkg: building dependency cache state");

    if (!cache_file.BuildSourceList(nullptr)) {
        if (error_out) *error_out = "failed to build apt source list";
        return false;
    }
    debug_timer.checkpoint("source list loaded");
    if (!cache_file.BuildCaches(nullptr, false)) {
        if (error_out) *error_out = "failed to build apt package caches";
        return false;
    }
    debug_timer.checkpoint("package cache mmap/build completed");
    if (!cache_file.BuildPolicy(nullptr)) {
        if (error_out) *error_out = "failed to build apt policy";
        return false;
    }
    debug_timer.checkpoint("policy constructed");
    if (!cache_file.BuildDepCache(nullptr)) {
        if (error_out) *error_out = "failed to build apt dependency cache";
        return false;
    }

    VLOG(verbose, "Built libapt-pkg cache with "
                     << cache_file.GetPkgCache()->HeaderP->PackageCount
                     << " package entries.");
    debug_timer.finish("dependency cache ready");
    return true;
}

void libapt_seed_auto_install_state(pkgCacheFile& cache_file, bool verbose) {
    pkgDepCache& cache = *cache_file;
    size_t marked_packages = 0;
    for (pkgCache::PkgIterator pkg = cache.PkgBegin(); pkg.end() == false; ++pkg) {
        bool auto_installed = false;
        if (get_package_auto_installed_state(pkg.Name(), &auto_installed)) {
            cache.MarkAuto(pkg, auto_installed);
            ++marked_packages;
        }
    }
    cache.MarkAndSweep();
    VLOG(verbose, "libapt-pkg: synchronized auto-installed state for "
                      << marked_packages << " package(s).");
}

bool libapt_find_package(
    pkgCacheFile& cache_file,
    const std::string& name,
    pkgCache::PkgIterator& out_pkg,
    bool verbose,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    ScopedLibAptDebugTimer debug_timer(verbose, "libapt-pkg: locating target '" + name + "'");
    pkgDepCache& cache = *cache_file;
    out_pkg = cache.FindPkg(name);
    if (!out_pkg.end()) {
        std::string current_version = out_pkg->CurrentVer == 0 ? "<not-installed>" : out_pkg.CurrentVer().VerStr();
        debug_timer.finish("found target (current=" + current_version + ")");
        return true;
    }

    if (error_out) *error_out = "package '" + name + "' is not present in the seeded apt cache";
    return false;
}

bool libapt_lookup_compiled_debian_metadata_for_candidate(
    const std::string& pkg_name,
    const std::string& version,
    PackageMetadata& out_meta,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();

    static std::string cached_policy_fingerprint;
    static std::map<std::string, DebianCompiledRecordCacheEntry> cached_entries;
    static bool cache_loaded = false;
    static bool cache_available = false;
    static std::string cached_error;

    std::string policy_fingerprint = build_debian_compiled_record_cache_policy_fingerprint();
    if (!cache_loaded || cached_policy_fingerprint != policy_fingerprint) {
        cached_policy_fingerprint = policy_fingerprint;
        cached_entries.clear();
        cached_error.clear();
        cache_available = load_debian_compiled_record_cache(
            policy_fingerprint,
            cached_entries,
            &cached_error
        );
        cache_loaded = true;
    }

    if (!cache_available) {
        if (error_out) {
            *error_out = cached_error.empty()
                ? "compiled Debian metadata cache is unavailable"
                : cached_error;
        }
        return false;
    }

    auto it = cached_entries.find(pkg_name);
    if (it == cached_entries.end()) {
        if (error_out) *error_out = "package is absent from cached Debian compiled metadata";
        return false;
    }

    const DebianCompiledRecordCacheEntry& entry = it->second;
    if (!entry.importable || entry.meta.name.empty()) {
        if (error_out) {
            *error_out = entry.skip_reason.empty()
                ? ("package " + pkg_name + " is not importable under current GeminiOS policy")
                : entry.skip_reason;
        }
        return false;
    }

    if (!version.empty() &&
        !entry.meta.version.empty() &&
        compare_versions(entry.meta.version, version) != 0) {
        if (error_out) {
            *error_out = "compiled Debian metadata version mismatch for " + pkg_name +
                " (expected " + version + ", cached " + entry.meta.version + ")";
        }
        return false;
    }

    out_meta = entry.meta;
    if (out_meta.debian_package.empty()) out_meta.debian_package = entry.raw_package;
    return true;
}

class pkgGeminiPMPlanBuilder : public pkgPackageManager {
    std::map<std::string, std::string> archive_paths_by_package_;
    std::vector<LibAptPlannedOperation> operations_;

   public:
    explicit pkgGeminiPMPlanBuilder(
        pkgDepCache* cache,
        const std::map<std::string, std::string>& archive_paths_by_package
    ) : pkgPackageManager(cache),
        archive_paths_by_package_(archive_paths_by_package) {
        for (PkgIterator pkg = Cache.PkgBegin(); pkg.end() == false; ++pkg) {
            FileNames[pkg->ID].clear();
        }

        for (const auto& entry : archive_paths_by_package_) {
            PkgIterator pkg = Cache.FindPkg(entry.first);
            if (pkg.end()) continue;
            FileNames[pkg->ID] = entry.second;
        }
    }

    bool Install(PkgIterator pkg, std::string file) override {
        operations_.push_back({LibAptOperationType::Install, pkg.Name(), file});
        return true;
    }

    bool Configure(PkgIterator pkg) override {
        operations_.push_back({LibAptOperationType::Configure, pkg.Name(), ""});
        return true;
    }

    bool Remove(PkgIterator pkg, bool purge = false) override {
        operations_.push_back({
            purge ? LibAptOperationType::Purge : LibAptOperationType::Remove,
            pkg.Name(),
            ""
        });
        return true;
    }

    bool Go(APT::Progress::PackageManager* /*progress*/) override {
        return true;
    }

    void Reset() override {
        operations_.clear();
    }

    const std::vector<LibAptPlannedOperation>& operations() const {
        return operations_;
    }
};

bool libapt_build_operation_plan(
    pkgCacheFile& cache_file,
    LibAptTransactionPlanResult& out_result,
    bool verbose,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    ScopedLibAptDebugTimer debug_timer(
        verbose,
        "libapt-pkg: building package-manager operation plan"
    );

    std::map<std::string, std::string> archive_paths_by_package;
    for (const auto& action : out_result.install_actions) {
        std::string apt_package_name = trim(action.apt_package_name);
        if (apt_package_name.empty()) continue;
        archive_paths_by_package[apt_package_name] = package_is_debian_source(action.meta)
            ? get_cached_debian_archive_path(action.meta)
            : get_install_archive_path(action.meta);
    }

    pkgGeminiPMPlanBuilder plan_builder(&*cache_file, archive_paths_by_package);
    pkgPackageManager::OrderResult order_result = plan_builder.DoInstallPreFork();
    if (order_result != pkgPackageManager::Completed) {
        if (error_out) {
            *error_out = order_result == pkgPackageManager::Incomplete
                ? "apt produced an incomplete package-manager order"
                : "apt could not produce a GeminiOS package-manager order";
        }
        return false;
    }

    out_result.ordered_operations = plan_builder.operations();
    debug_timer.finish(
        "built " + std::to_string(out_result.ordered_operations.size()) + " ordered operation(s)"
    );
    return true;
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

    if (try_ensure_repo_package_cache_loaded(verbose)) {
        PackageMetadata repo_meta;
        std::string canonical_name = canonicalize_package_name(pkg_name, verbose);
        if (get_loaded_repo_package_info(canonical_name, repo_meta) &&
            package_can_use_libapt_native_planner(repo_meta) &&
            compare_versions(repo_meta.version, version) == 0) {
            out_meta = repo_meta;
            return true;
        }
    }

    std::string compiled_error;
    if (libapt_lookup_compiled_debian_metadata_for_candidate(
            pkg_name,
            version,
            out_meta,
            &compiled_error
        )) {
        return true;
    }

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
        if (!compiled_error.empty() &&
            (reason.empty() || reason == "raw Debian context index is unavailable")) {
            *error_out = compiled_error;
        } else {
            *error_out = reason.empty()
                ? "no Debian metadata candidate is available for " + pkg_name
                : reason;
        }
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
    ScopedLibAptDebugTimer debug_timer(verbose, "libapt-pkg: extracting planned transaction result");

    RawDebianContext raw_context;
    pkgDepCache& cache = *cache_file;
    pkgCache& pkg_cache = cache.GetCache();
    size_t scanned_packages = 0;
    size_t planned_install_actions = 0;
    size_t planned_removals = 0;

    for (pkgCache::PkgIterator pkg = cache.PkgBegin(); pkg.end() == false; ++pkg) {
        ++scanned_packages;
        pkgDepCache::StateCache& state = cache[pkg];
        bool currently_present =
            pkg->CurrentState != pkgCache::State::NotInstalled;

        bool should_remove =
            state.Delete() ||
            (include_garbage_removals && state.Garbage && currently_present);
        if (should_remove && currently_present) {
            std::string pkg_name = pkg.Name();
            out_result.remove_packages.push_back(pkg_name);
            if (state.Purge() || purge_garbage) out_result.purge_packages.push_back(pkg_name);
            ++planned_removals;
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
        action.apt_package_name = pkg.Name();
        action.current_version = pkg->CurrentVer == 0 ? "" : pkg.CurrentVer().VerStr();
        action.was_installed = currently_present;
        action.reinstall_only =
            action.was_installed &&
            !action.current_version.empty() &&
            compare_versions(action.current_version, meta.version) == 0;
        action.explicit_target = explicit_targets.count(pkg.Name()) != 0;
        out_result.install_actions.push_back(action);
        ++planned_install_actions;
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
    debug_timer.finish(
        "scanned " + std::to_string(scanned_packages) + " package(s), planned " +
        std::to_string(planned_install_actions) + " install/reinstall action(s), " +
        std::to_string(planned_removals) + " removal(s)"
    );
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
    ScopedLibAptDebugTimer debug_timer(verbose, "libapt-pkg: opening seeded planner cache");

    std::string dpkg_bootstrap_error;
    {
        ScopedLibAptDebugTimer step_timer(verbose, "libapt-pkg: ensuring native dpkg backend state");
    if (!ensure_native_dpkg_backend_ready(verbose, &dpkg_bootstrap_error)) {
        if (error_out) {
            *error_out = dpkg_bootstrap_error.empty()
                ? "native dpkg state is unavailable"
                : dpkg_bootstrap_error;
        }
        return false;
    }
        step_timer.finish("native dpkg backend state ready");
    }

    ScopedLibAptDebugTimer config_timer(verbose, "libapt-pkg: loading Debian backend config");
    DebianBackendConfig config = load_debian_backend_config(verbose);
    config_timer.finish("using architecture " + config.apt_arch);

    {
        ScopedLibAptDebugTimer step_timer(verbose, "libapt-pkg: preparing planner session root");
        if (!libapt_prepare_session_root(session_root, error_out)) return false;
        step_timer.finish("session root at " + session_root.path);
    }
    {
        ScopedLibAptDebugTimer step_timer(verbose, "libapt-pkg: seeding planner session inputs");
        if (!libapt_seed_debian_packages_index(session_root, packages_path, config, verbose, error_out)) {
            return false;
        }
        step_timer.finish("session inputs ready");
    }
    {
        ScopedLibAptDebugTimer step_timer(verbose, "libapt-pkg: initializing libapt globals");
        if (!libapt_initialize_globals(session_root, config, verbose, error_out)) return false;
        step_timer.finish("libapt globals initialized");
    }
    {
        ScopedLibAptDebugTimer step_timer(verbose, "libapt-pkg: building/opening planner cache");
        if (!libapt_build_cache_file(cache_file, verbose, error_out)) return false;
        step_timer.finish("planner cache ready");
    }
    {
        ScopedLibAptDebugTimer step_timer(verbose, "libapt-pkg: seeding auto-installed state");
        libapt_seed_auto_install_state(cache_file, verbose);
        step_timer.finish("auto-installed state synchronized");
    }
    debug_timer.finish("seeded planner cache is ready");
    return true;
}

bool libapt_prime_planner_cache(bool verbose, std::string* error_out) {
    if (error_out) error_out->clear();

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

    VLOG(verbose, "Primed libapt-pkg planner cache at " << session_root.path);
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
    ScopedLibAptDebugTimer debug_timer(verbose, "libapt-pkg: planning install-like transaction");

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
    debug_timer.checkpoint("seeded planner cache opened at " + session_root.path);

    pkgDepCache& cache = *cache_file;
    for (const auto& target : explicit_targets) {
        pkgCache::PkgIterator pkg;
        std::string lookup_error;
        if (!libapt_find_package(cache_file, target, pkg, verbose, &lookup_error)) {
            if (error_out) *error_out = lookup_error;
            return false;
        }

        {
            ScopedLibAptDebugTimer mark_timer(
                verbose,
                "libapt-pkg: marking '" + target + "' for installation"
            );
            pkgDepCache::ActionGroup group(cache);
            pkgCache::VerIterator candidate_version = cache.GetCandidateVersion(pkg);
            bool same_version_candidate =
                pkg->CurrentVer != 0 &&
                !candidate_version.end() &&
                compare_versions(candidate_version.VerStr(), pkg.CurrentVer().VerStr()) == 0;
            std::string candidate_version_str =
                candidate_version.end() ? "<none>" : candidate_version.VerStr();

            bool marked_install = cache.MarkInstall(pkg, true, 0, true);
            if (!marked_install &&
                reinstall_targets.count(target) != 0 &&
                same_version_candidate) {
                cache.SetReInstall(pkg, true);
                marked_install = true;
            }

            if (!marked_install) {
                if (error_out) *error_out = "apt refused to mark " + target + " for installation";
                return false;
            }
            if (reinstall_targets.count(target) != 0) cache.SetReInstall(pkg, true);
            mark_timer.finish(
                "candidate=" + candidate_version_str +
                ", reinstall=" +
                std::string(reinstall_targets.count(target) != 0 ? "yes" : "no")
            );
        }
    }

    if (fix_broken) {
        ScopedLibAptDebugTimer fix_timer(verbose, "libapt-pkg: fixing broken dependency state");
        if (!pkgFixBroken(cache)) {
            if (error_out) *error_out = "apt could not repair broken dependency state";
            return false;
        }
        fix_timer.finish("broken dependency repair completed");
    }

    ScopedLibAptDebugTimer resolver_timer(verbose, "libapt-pkg: running problem resolver");
    pkgProblemResolver resolver(&cache);
    if (!resolver.Resolve(fix_broken)) {
        if (error_out) *error_out = "apt could not solve the requested transaction";
        return false;
    }
    resolver_timer.finish("problem resolver converged");

    if (!libapt_extract_transaction_result(
        cache_file,
        std::set<std::string>(explicit_targets.begin(), explicit_targets.end()),
        false,
        false,
        verbose,
        out_result,
        error_out
    )) {
        return false;
    }

    debug_timer.checkpoint(
        "extracted " + std::to_string(out_result.install_actions.size()) +
        " install action(s) and " + std::to_string(out_result.remove_packages.size()) +
        " removal(s)"
    );
    if (!libapt_build_operation_plan(cache_file, out_result, verbose, error_out)) {
        return false;
    }
    debug_timer.finish("install-like transaction plan ready");
    return true;
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
    ScopedLibAptDebugTimer debug_timer(verbose, "libapt-pkg: planning remove transaction");

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
    debug_timer.checkpoint("seeded planner cache opened at " + session_root.path);

    pkgDepCache& cache = *cache_file;
    for (const auto& target : explicit_targets) {
        pkgCache::PkgIterator pkg;
        std::string lookup_error;
        if (!libapt_find_package(cache_file, target, pkg, verbose, &lookup_error)) {
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
        VLOG(verbose, "libapt-pkg: marked '" << target << "' for "
                                             << (purge ? "purge" : "removal") << ".");
    }

    if (autoremove) {
        ScopedLibAptDebugTimer autoremove_timer(verbose, "libapt-pkg: running autoremove sweep");
        cache.MarkAndSweep();
        autoremove_timer.finish("autoremove sweep completed");
    }

    if (!libapt_extract_transaction_result(
        cache_file,
        std::set<std::string>(explicit_targets.begin(), explicit_targets.end()),
        autoremove,
        purge,
        verbose,
        out_result,
        error_out
    )) {
        return false;
    }

    debug_timer.finish(
        "remove transaction planned with " + std::to_string(out_result.remove_packages.size()) +
        " removal(s)"
    );
    return true;
}

#endif

#if !defined(GPKG_HAVE_WORKING_LIBAPT_PKG_BACKEND)
bool libapt_plan_install_like_transaction(
    const std::vector<std::string>& explicit_targets,
    const std::set<std::string>& reinstall_targets,
    bool fix_broken,
    bool verbose,
    LibAptTransactionPlanResult& out_result,
    std::string* error_out
) {
    (void)explicit_targets;
    (void)reinstall_targets;
    (void)fix_broken;
    (void)verbose;
    out_result = {};
    if (error_out) *error_out = "libapt-pkg backend is not available in this gpkg build";
    return false;
}

bool libapt_plan_remove_transaction(
    const std::vector<std::string>& explicit_targets,
    bool purge,
    bool autoremove,
    bool verbose,
    LibAptTransactionPlanResult& out_result,
    std::string* error_out
) {
    (void)explicit_targets;
    (void)purge;
    (void)autoremove;
    (void)verbose;
    out_result = {};
    if (error_out) *error_out = "libapt-pkg backend is not available in this gpkg build";
    return false;
}

bool libapt_prime_planner_cache(bool verbose, std::string* error_out) {
    (void)verbose;
    if (error_out) error_out->clear();
    return true;
}
#endif

bool package_can_use_libapt_native_planner(const PackageMetadata& meta) {
    return package_is_debian_source(meta) || meta.source_kind == "gpkg_repo";
}

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
        if (!package_can_use_libapt_native_planner(result.meta)) {
            if (error_out) {
                *error_out = operand + " resolves to a package source that is not available in the native planner cache yet";
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
    std::vector<std::string>* apt_targets_out = nullptr,
    std::set<std::string>* reinstall_targets_out = nullptr,
    std::string* error_out = nullptr
) {
    if (error_out) error_out->clear();
    if (apt_targets_out) apt_targets_out->clear();
    if (reinstall_targets_out) reinstall_targets_out->clear();
    if (normalized_roots.empty()) return false;

    RawDebianContext raw_context;
    std::set<std::string> seen_targets;
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
        if (!package_can_use_libapt_native_planner(repo_meta)) {
            if (error_out) {
                *error_out = pkg + " resolves to an upgrade target that is not available in the native planner cache yet";
            }
            return false;
        }

        std::string apt_target = trim(repo_meta.debian_package);
        if (apt_target.empty()) apt_target = trim(repo_meta.name);
        if (apt_target.empty()) apt_target = canonicalize_package_name(pkg, verbose);
        if (apt_target.empty()) {
            if (error_out) *error_out = "no native planner operand is available for " + pkg;
            return false;
        }

        bool needs_apt_action = true;
        bool needs_reinstall = g_force_reinstall;
        if (!needs_reinstall) {
            std::string current_version;
            bool was_installed = get_upgrade_target_current_version(
                {pkg, "", ""},
                repo_meta,
                current_version,
                verbose,
                &context
            );
            if (was_installed) {
                int version_cmp = compare_versions(repo_meta.version, current_version);
                if (version_cmp <= 0) {
                    bool managed_locally =
                        get_local_installed_package_version(repo_meta.name, nullptr, &context);
                    if (!managed_locally && repo_meta.name != pkg) {
                        managed_locally = get_local_installed_package_version(pkg, nullptr, &context);
                    }

                    if (version_cmp == 0 && !managed_locally) {
                        needs_reinstall = true;
                    } else {
                        needs_apt_action = false;
                    }
                }
            }
        }

        if (!needs_apt_action) continue;
        if (seen_targets.insert(apt_target).second && apt_targets_out) {
            apt_targets_out->push_back(apt_target);
        }
        if (needs_reinstall && reinstall_targets_out) {
            reinstall_targets_out->insert(apt_target);
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
        if (!package_can_use_libapt_native_planner(meta)) {
            if (error_out) {
                *error_out = meta.name + " is not available in the native planner cache yet";
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
