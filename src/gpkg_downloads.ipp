// Archive verification and bounded parallel package downloads.

struct ArchiveFetchResult {
    bool success = false;
    bool reused = false;
    size_t bytes_downloaded = 0;
    size_t archive_bytes = 0;
    std::string error;
};

struct DownloadBatchReport {
    std::vector<ArchiveFetchResult> results;
    size_t downloaded_count = 0;
    size_t reused_count = 0;
    size_t downloaded_bytes = 0;
    size_t reused_bytes = 0;
    size_t estimated_bytes = 0;
};

std::string format_batch_speed(double bytes_per_sec) {
    const char* units[] = {"B/s", "KB/s", "MB/s", "GB/s"};
    size_t unit_index = 0;
    while (bytes_per_sec >= 1024.0 && unit_index < 3) {
        bytes_per_sec /= 1024.0;
        ++unit_index;
    }

    std::ostringstream out;
    if (unit_index == 0) {
        out << static_cast<long>(bytes_per_sec) << " " << units[unit_index];
    } else {
        out << std::fixed << std::setprecision(1) << bytes_per_sec << " " << units[unit_index];
    }
    return out.str();
}

std::string format_total_bytes(size_t bytes) {
    static const char* units[] = {"B", "KB", "MB", "GB"};
    double value = static_cast<double>(bytes);
    size_t unit_index = 0;
    while (value >= 1024.0 && unit_index < 3) {
        value /= 1024.0;
        ++unit_index;
    }

    std::ostringstream out;
    if (unit_index == 0) {
        out << bytes << " " << units[unit_index];
    } else {
        out << std::fixed << std::setprecision(1) << value << " " << units[unit_index];
    }
    return out.str();
}

std::string format_data_progress(size_t transferred, size_t estimated, bool estimate_complete = true) {
    if (!estimate_complete) return format_total_bytes(transferred) + "/?";
    if (estimated == 0) return format_total_bytes(transferred);
    return format_total_bytes(transferred) + "/" + format_total_bytes(estimated);
}

bool verify_hash(
    const std::string& file,
    const std::string& expected_hash,
    const std::string& algorithm,
    const std::string& label = "",
    std::string* error_out = nullptr,
    bool quiet = false
) {
    if (error_out) error_out->clear();

    std::string subject = label.empty() ? file : label;
    if (!quiet) {
        std::cout << "Verifying " << subject << "..." << std::endl;
    }

    std::ifstream f(file, std::ios::binary);
    if (!f) {
        if (error_out) *error_out = "could not open file for verification";
        if (!quiet) {
            std::cerr << "E: Could not open " << subject << " for verification: " << file << std::endl;
        }
        return false;
    }

    char buffer[32768];
    std::string calculated;
    if (algorithm == "sha256") {
        SHA256_CTX sha256;
        SHA256_Init(&sha256);
        while (f.read(buffer, sizeof(buffer)) || f.gcount() > 0) {
            SHA256_Update(&sha256, buffer, f.gcount());
        }
        unsigned char hash[SHA256_DIGEST_LENGTH];
        SHA256_Final(hash, &sha256);
        std::stringstream ss;
        for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
        }
        calculated = ss.str();
    } else {
        SHA512_CTX sha512;
        SHA512_Init(&sha512);
        while (f.read(buffer, sizeof(buffer)) || f.gcount() > 0) {
            SHA512_Update(&sha512, buffer, f.gcount());
        }
        unsigned char hash[SHA512_DIGEST_LENGTH];
        SHA512_Final(hash, &sha512);
        std::stringstream ss;
        for (int i = 0; i < SHA512_DIGEST_LENGTH; ++i) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
        }
        calculated = ss.str();
    }
    if (calculated == expected_hash) return true;

    if (error_out) *error_out = "hash mismatch";
    if (!quiet) {
        std::cerr << "E: Hash mismatch for " << subject << "!" << std::endl;
        std::cerr << "   Expected:   " << expected_hash << std::endl;
        std::cerr << "   Calculated: " << calculated << std::endl;
    }
    return false;
}

std::string get_cached_package_path(const PackageMetadata& meta) {
    if (package_is_debian_source(meta)) return get_cached_debian_archive_path(meta);

    std::string repo_component = cache_safe_component(meta.source_url.empty() ? "default" : meta.source_url);
    std::string base = REPO_CACHE_PATH + "gpkg/" + repo_component + "/";
    std::string filename = cache_safe_component(meta.name)
        + "_" + safe_repo_filename_component(meta.version)
        + "_" + cache_safe_component(meta.arch.empty() ? std::string(OS_ARCH) : meta.arch)
        + EXTENSION;
    return base + filename;
}

std::string get_partial_package_path(const PackageMetadata& meta) {
    return get_cached_package_path(meta) + ".part";
}

size_t file_size_if_exists(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size > 0) {
        return static_cast<size_t>(st.st_size);
    }
    return 0;
}

size_t get_partial_package_bytes(const PackageMetadata& meta) {
    struct stat st;
    if (stat(get_partial_package_path(meta).c_str(), &st) == 0 && st.st_size > 0) {
        return static_cast<size_t>(st.st_size);
    }
    return 0;
}

size_t get_cached_package_bytes(const PackageMetadata& meta) {
    if (package_is_debian_source(meta)) {
        return file_size_if_exists(get_cached_debian_archive_path(meta));
    }

    return file_size_if_exists(get_cached_package_path(meta));
}

long get_declared_archive_size_bytes(const PackageMetadata& meta) {
    if (meta.size.empty()) return -1;

    char* end = nullptr;
    errno = 0;
    unsigned long parsed = std::strtoul(meta.size.c_str(), &end, 10);
    if (errno != 0 || end == meta.size.c_str()) return -1;
    return parsed > static_cast<unsigned long>(std::numeric_limits<long>::max())
        ? -1
        : static_cast<long>(parsed);
}

bool fetch_package_archive(
    const PackageMetadata& meta,
    size_t index,
    size_t total,
    bool verbose,
    bool* reused_out = nullptr,
    size_t* transferred_out = nullptr,
    std::string* error_out = nullptr,
    bool quiet = false,
    const std::function<void(size_t, size_t, double)>& progress_callback = nullptr
) {
    if (reused_out) *reused_out = false;
    if (transferred_out) *transferred_out = 0;
    if (error_out) error_out->clear();

    auto fail = [&](const std::string& message) {
        if (error_out) *error_out = message;
        if (!quiet) {
            std::cerr << Color::RED << "E: " << message << Color::RESET << std::endl;
        }
        return false;
    };

    if (package_is_debian_source(meta) &&
        access(get_cached_debian_archive_path(meta).c_str(), F_OK) == 0) {
        if (!quiet) {
            std::cout << "Using cached Debian archive (" << index << "/" << total << ") "
                      << meta.name << "..." << std::endl;
        }
        if (reused_out) *reused_out = true;
        return true;
    }

    if (!mkdir_parent(get_cached_package_path(meta))) {
        return fail("Failed to create cache directory for " + meta.name);
    }

    std::string local_path = get_cached_package_path(meta);
    std::string verify_label = "package archive " + meta.name;
    std::string hash_value = package_is_debian_source(meta) ? meta.sha256 : meta.sha512;
    std::string hash_algorithm = package_is_debian_source(meta) ? "sha256" : "sha512";
    if (hash_value.empty()) {
        return fail("Missing archive checksum for " + meta.name);
    }

    if (access(local_path.c_str(), F_OK) == 0) {
        if (!quiet) {
            std::cout << "Using cached (" << index << "/" << total << ") "
                      << meta.name << "..." << std::endl;
        }

        std::string verify_error;
        if (verify_hash(local_path, hash_value, hash_algorithm, "cached " + verify_label, &verify_error, quiet)) {
            if (reused_out) *reused_out = true;
            return true;
        }

        if (!quiet) {
            std::cerr << Color::YELLOW << "W: Cached archive for " << meta.name
                      << " is invalid. Removing it and downloading a fresh copy."
                      << Color::RESET << std::endl;
        }
        remove(local_path.c_str());
    }

    std::string url;
    if (!resolve_download_url(meta, url)) {
        return fail("Unable to resolve download URL for " + meta.name);
    }

    const int max_fetch_attempts = 2;
    std::string last_error;
    long known_remote_size = get_declared_archive_size_bytes(meta);
    for (int attempt = 1; attempt <= max_fetch_attempts; ++attempt) {
        if (!quiet) {
            std::cout << "Downloading (" << index << "/" << total << ") " << meta.name;
            if (attempt > 1) std::cout << " [retry " << attempt << "/" << max_fetch_attempts << "]";
            std::cout << "..." << std::endl;
        }

        std::string download_error;
        size_t transferred = 0;
        bool network_verbose = quiet ? false : verbose;
        bool network_progress = quiet ? false : true;
        if (!DownloadFile(
                url,
                local_path,
                network_verbose,
                &download_error,
                network_progress,
                progress_callback,
                &transferred,
                known_remote_size
            )) {
            remove(local_path.c_str());
            last_error = "failed to download from " + url;
            if (!download_error.empty()) last_error += " (" + download_error + ")";
            if (!quiet) {
                std::cerr << Color::YELLOW << "W: Failed to download " << meta.name
                          << " from " << url;
                if (!download_error.empty()) std::cerr << " (" << download_error << ")";
                std::cerr << Color::RESET << std::endl;
            }
            continue;
        }

        std::string verify_error;
        if (verify_hash(local_path, hash_value, hash_algorithm, verify_label, &verify_error, quiet)) {
            if (transferred_out) *transferred_out = transferred;
            return true;
        }

        last_error = "downloaded archive failed integrity verification";
        if (!quiet) {
            std::cerr << Color::YELLOW << "W: Downloaded archive for " << meta.name
                      << " failed integrity verification. Re-downloading."
                      << Color::RESET << std::endl;
        }
        remove(local_path.c_str());
    }

    if (last_error.empty()) {
        last_error = "failed to fetch a valid archive from " + url;
    }
    return fail("Failed to fetch a valid archive for " + meta.name + " from " + url + " (" + last_error + ")");
}

size_t estimate_package_archive_bytes(const PackageMetadata& meta) {
    size_t cached_bytes = get_cached_package_bytes(meta);
    if (cached_bytes > 0) return cached_bytes;

    if (!meta.size.empty()) {
        char* end = nullptr;
        unsigned long parsed = std::strtoul(meta.size.c_str(), &end, 10);
        if (end != meta.size.c_str() && parsed > 0) return static_cast<size_t>(parsed);
    }

    std::string url;
    if (!resolve_download_url(meta, url)) return 0;

    long remote_size = GetRemoteFileSize(url);
    if (remote_size <= 0) return 0;
    return static_cast<size_t>(remote_size);
}

DownloadBatchReport download_package_archives(
    const std::vector<PackageMetadata>& packages,
    bool verbose,
    size_t max_parallel_downloads
) {
    DownloadBatchReport report;
    report.results.resize(packages.size());
    if (packages.empty()) return report;

    const size_t worker_count = std::max<size_t>(1, std::min(max_parallel_downloads, packages.size()));
    std::atomic<size_t> next_index{0};
    std::atomic<size_t> next_estimate_index{0};
    std::atomic<bool> stop_estimators{false};
    std::mutex output_mutex;
    struct ActiveDownloadState {
        bool active = false;
        bool reused = false;
        size_t transferred = 0;
        size_t estimated = 0;
        double speed = 0.0;
        std::string name;
    };
    std::vector<ActiveDownloadState> active_downloads(packages.size());
    std::vector<size_t> known_archive_estimates(packages.size(), 0);
    size_t completed_count = 0;
    size_t downloaded_count = 0;
    size_t downloaded_bytes = 0;
    size_t completed_archive_bytes = 0;
    size_t reused_count = 0;
    size_t failed_count = 0;
    size_t last_render_width = 0;

    for (size_t i = 0; i < packages.size(); ++i) {
        size_t cached_bytes = get_cached_package_bytes(packages[i]);
        if (cached_bytes == 0) continue;
        known_archive_estimates[i] = cached_bytes;
        report.estimated_bytes += cached_bytes;
    }

    auto render_progress = [&](const std::string& last_package) {
        const size_t terminal_width = get_terminal_width();
        const int bar_width = static_cast<int>(
            std::max<size_t>(10, std::min<size_t>(48, terminal_width > 72 ? terminal_width / 3 : 10))
        );
        size_t live_bytes = completed_archive_bytes;
        double live_speed = 0.0;
        size_t active_count = 0;
        std::string label = last_package;
        bool all_archive_estimates_known = true;

        for (const auto& state : active_downloads) {
            if (!state.active || state.reused) continue;
            live_bytes += state.transferred;
            live_speed += state.speed;
            ++active_count;
            if (label.empty()) label = state.name;
        }

        for (size_t estimate : known_archive_estimates) {
            if (estimate == 0) {
                all_archive_estimates_known = false;
                break;
            }
        }

        int percent = 0;
        if (all_archive_estimates_known && report.estimated_bytes > 0) {
            percent = static_cast<int>((live_bytes * 100) / report.estimated_bytes);
        } else {
            percent = static_cast<int>((completed_count * 100) / packages.size());
        }
        if (percent > 100) percent = 100;
        int filled = (percent * bar_width) / 100;

        const size_t base_width = static_cast<size_t>(bar_width) + 58;
        const size_t label_width = terminal_width > base_width ? terminal_width - base_width : 12;
        label = truncate_progress_label(label, std::max<size_t>(12, label_width));

        std::ostringstream line;
        line << "\r" << Color::CYAN << "[";
        for (int i = 0; i < bar_width; ++i) {
            line << (i < filled ? "#" : ".");
        }
        line << "]" << Color::RESET
             << " " << std::setw(3) << percent << "% "
             << "(" << completed_count << "/" << packages.size() << ")"
             << "  net:" << downloaded_count
             << "  fail:" << failed_count
             << "  data:" << format_data_progress(live_bytes, report.estimated_bytes, all_archive_estimates_known)
             << "  speed:" << format_batch_speed(live_speed);
        if (!label.empty()) {
            line << "  pkg:" << label;
        }
        if (active_count > 1) {
            line << "  +" << (active_count - 1) << " more";
        }

        std::string rendered = line.str();
        size_t visible_width = visible_text_width(rendered);
        std::cout << rendered;
        if (visible_width < last_render_width) {
            std::cout << std::string(last_render_width - visible_width, ' ');
        }
        last_render_width = visible_width;
        std::cout << std::flush;
    };

    {
        std::lock_guard<std::mutex> lock(output_mutex);
        render_progress("");
    }

    auto estimator = [&]() {
        while (!stop_estimators.load()) {
            size_t idx = next_estimate_index.fetch_add(1);
            if (idx >= packages.size()) return;

            {
                std::lock_guard<std::mutex> lock(output_mutex);
                if (known_archive_estimates[idx] > 0) continue;
            }

            size_t estimate = estimate_package_archive_bytes(packages[idx]);
            if (estimate == 0) continue;

            std::lock_guard<std::mutex> lock(output_mutex);
            if (known_archive_estimates[idx] > 0) continue;
            known_archive_estimates[idx] = estimate;
            report.estimated_bytes += estimate;
            if (active_downloads[idx].active) {
                active_downloads[idx].estimated = estimate;
            }
            render_progress(active_downloads[idx].name.empty() ? packages[idx].name : active_downloads[idx].name);
        }
    };

    auto worker = [&]() {
        while (true) {
            size_t idx = next_index.fetch_add(1);
            if (idx >= packages.size()) return;

            {
                std::lock_guard<std::mutex> lock(output_mutex);
                active_downloads[idx].active = true;
                active_downloads[idx].name = packages[idx].name;
                active_downloads[idx].transferred = get_partial_package_bytes(packages[idx]);
                active_downloads[idx].estimated = known_archive_estimates[idx];
                render_progress(packages[idx].name);
            }

            bool reused = false;
            size_t transferred = 0;
            std::string error;
            bool ok = fetch_package_archive(
                packages[idx],
                idx + 1,
                packages.size(),
                verbose,
                &reused,
                &transferred,
                &error,
                true,
                [&](size_t transferred, size_t estimated, double speed) {
                    std::lock_guard<std::mutex> lock(output_mutex);
                    if (estimated > known_archive_estimates[idx]) {
                        report.estimated_bytes += (estimated - known_archive_estimates[idx]);
                        known_archive_estimates[idx] = estimated;
                    }
                    active_downloads[idx].active = true;
                    active_downloads[idx].reused = false;
                    active_downloads[idx].name = packages[idx].name;
                    active_downloads[idx].transferred = transferred;
                    active_downloads[idx].estimated = known_archive_estimates[idx];
                    active_downloads[idx].speed = speed;
                    render_progress(packages[idx].name);
                }
            );

            report.results[idx].success = ok;
            report.results[idx].reused = reused;
            report.results[idx].error = error;
            if (ok) {
                report.results[idx].archive_bytes = get_cached_package_bytes(packages[idx]);
                if (!reused) {
                    report.results[idx].bytes_downloaded = transferred;
                }
            }

            std::lock_guard<std::mutex> lock(output_mutex);
            active_downloads[idx].active = false;
            active_downloads[idx].reused = reused;
            active_downloads[idx].transferred = 0;
            active_downloads[idx].estimated = 0;
            active_downloads[idx].speed = 0.0;
            ++completed_count;
            if (ok) {
                if (report.results[idx].archive_bytes > 0 && report.results[idx].archive_bytes != known_archive_estimates[idx]) {
                    if (report.results[idx].archive_bytes > known_archive_estimates[idx]) {
                        report.estimated_bytes += (report.results[idx].archive_bytes - known_archive_estimates[idx]);
                    } else {
                        report.estimated_bytes -= (known_archive_estimates[idx] - report.results[idx].archive_bytes);
                    }
                    known_archive_estimates[idx] = report.results[idx].archive_bytes;
                }
                completed_archive_bytes += report.results[idx].archive_bytes;
                if (reused) {
                    ++reused_count;
                } else {
                    ++downloaded_count;
                    downloaded_bytes += report.results[idx].bytes_downloaded;
                }
            } else {
                ++failed_count;
            }
            render_progress(packages[idx].name);
        }
    };

    std::vector<std::thread> estimators;
    estimators.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        estimators.emplace_back(estimator);
    }

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        workers.emplace_back(worker);
    }
    for (auto& thread : workers) {
        thread.join();
    }

    stop_estimators.store(true);
    for (auto& thread : estimators) {
        thread.join();
    }

    {
        std::lock_guard<std::mutex> lock(output_mutex);
        std::cout << std::endl;
    }

    for (const auto& result : report.results) {
        if (!result.success) continue;
        if (result.reused) {
            ++report.reused_count;
            report.reused_bytes += result.archive_bytes;
        } else {
            ++report.downloaded_count;
            report.downloaded_bytes += result.bytes_downloaded;
        }
    }

    return report;
}
