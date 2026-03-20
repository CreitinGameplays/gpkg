// Archive verification and bounded parallel package downloads.

struct ArchiveFetchResult {
    bool success = false;
    bool reused = false;
    size_t bytes_downloaded = 0;
    std::string error;
};

struct DownloadBatchReport {
    std::vector<ArchiveFetchResult> results;
    size_t downloaded_count = 0;
    size_t reused_count = 0;
    size_t downloaded_bytes = 0;
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

bool verify_hash(
    const std::string& file,
    const std::string& expected_hash,
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

    SHA512_CTX sha512;
    SHA512_Init(&sha512);
    char buffer[32768];
    while (f.read(buffer, sizeof(buffer)) || f.gcount() > 0) {
        SHA512_Update(&sha512, buffer, f.gcount());
    }

    unsigned char hash[SHA512_DIGEST_LENGTH];
    SHA512_Final(hash, &sha512);

    std::stringstream ss;
    for (int i = 0; i < SHA512_DIGEST_LENGTH; ++i) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }

    std::string calculated = ss.str();
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
    return REPO_CACHE_PATH + meta.name + EXTENSION;
}

bool fetch_package_archive(
    const PackageMetadata& meta,
    size_t index,
    size_t total,
    bool verbose,
    bool* reused_out = nullptr,
    std::string* error_out = nullptr,
    bool quiet = false
) {
    if (reused_out) *reused_out = false;
    if (error_out) error_out->clear();

    auto fail = [&](const std::string& message) {
        if (error_out) *error_out = message;
        if (!quiet) {
            std::cerr << Color::RED << "E: " << message << Color::RESET << std::endl;
        }
        return false;
    };

    if (!mkdir_p(REPO_CACHE_PATH)) {
        return fail("Failed to create cache directory " + REPO_CACHE_PATH);
    }

    std::string local_path = get_cached_package_path(meta);
    std::string verify_label = "package archive " + meta.name;

    if (access(local_path.c_str(), F_OK) == 0) {
        if (!quiet) {
            std::cout << "Using cached (" << index << "/" << total << ") "
                      << meta.name << "..." << std::endl;
        }

        std::string verify_error;
        if (verify_hash(local_path, meta.sha512, "cached " + verify_label, &verify_error, quiet)) {
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
    for (int attempt = 1; attempt <= max_fetch_attempts; ++attempt) {
        if (!quiet) {
            std::cout << "Downloading (" << index << "/" << total << ") " << meta.name;
            if (attempt > 1) std::cout << " [retry " << attempt << "/" << max_fetch_attempts << "]";
            std::cout << "..." << std::endl;
        }

        std::string download_error;
        bool network_verbose = quiet ? false : verbose;
        bool network_progress = quiet ? false : true;
        if (!DownloadFile(url, local_path, network_verbose, &download_error, network_progress)) {
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
        if (verify_hash(local_path, meta.sha512, verify_label, &verify_error, quiet)) {
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
    std::mutex output_mutex;
    size_t completed_count = 0;
    size_t downloaded_count = 0;
    size_t downloaded_bytes = 0;
    size_t reused_count = 0;
    size_t failed_count = 0;
    auto batch_start = std::chrono::steady_clock::now();

    auto render_progress = [&](const std::string& last_package) {
        const int bar_width = 32;
        int percent = static_cast<int>((completed_count * 100) / packages.size());
        int filled = static_cast<int>((completed_count * bar_width) / packages.size());
        std::string label = last_package;
        if (label.size() > 24) label = label.substr(0, 21) + "...";
        auto now = std::chrono::steady_clock::now();
        double elapsed_seconds = std::chrono::duration_cast<std::chrono::milliseconds>(now - batch_start).count() / 1000.0;
        double bytes_per_sec = elapsed_seconds > 0.0 ? static_cast<double>(downloaded_bytes) / elapsed_seconds : 0.0;

        std::cout << "\r" << Color::CYAN << "[";
        for (int i = 0; i < bar_width; ++i) {
            std::cout << (i < filled ? "#" : ".");
        }
        std::cout << "]" << Color::RESET
                  << " " << std::setw(3) << percent << "% "
                  << "(" << completed_count << "/" << packages.size() << ")"
                  << "  net:" << downloaded_count
                  << "  cache:" << reused_count
                  << "  fail:" << failed_count
                  << "  data:" << format_total_bytes(downloaded_bytes)
                  << "  speed:" << format_batch_speed(bytes_per_sec);
        if (!label.empty()) {
            std::cout << "  last:" << label;
        }
        std::cout << std::flush;
    };

    {
        std::lock_guard<std::mutex> lock(output_mutex);
        render_progress("");
    }

    auto worker = [&]() {
        while (true) {
            size_t idx = next_index.fetch_add(1);
            if (idx >= packages.size()) return;

            bool reused = false;
            std::string error;
            bool ok = fetch_package_archive(
                packages[idx],
                idx + 1,
                packages.size(),
                verbose,
                &reused,
                &error,
                true
            );

            report.results[idx].success = ok;
            report.results[idx].reused = reused;
            report.results[idx].error = error;
            if (ok && !reused) {
                struct stat st;
                if (stat(get_cached_package_path(packages[idx]).c_str(), &st) == 0 && st.st_size > 0) {
                    report.results[idx].bytes_downloaded = static_cast<size_t>(st.st_size);
                }
            }

            std::lock_guard<std::mutex> lock(output_mutex);
            ++completed_count;
            if (ok) {
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

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    for (size_t i = 0; i < worker_count; ++i) {
        workers.emplace_back(worker);
    }
    for (auto& thread : workers) {
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
        } else {
            ++report.downloaded_count;
            report.downloaded_bytes += result.bytes_downloaded;
        }
    }

    return report;
}
