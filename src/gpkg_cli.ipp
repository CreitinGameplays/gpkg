// CLI entrypoint and top-level command dispatch.

#ifndef GPKG_VERSION
#define GPKG_VERSION OS_VERSION
#endif

#ifndef GPKG_CODENAME
#define GPKG_CODENAME OS_CODENAME
#endif

struct GpkgCliVersionInfo {
    std::string version_label = GPKG_VERSION;
    std::string codename = GPKG_CODENAME;
    std::string build_id;
};

std::string strip_matching_quotes(const std::string& value) {
    if (value.size() >= 2) {
        char first = value.front();
        char last = value.back();
        if ((first == '"' && last == '"') || (first == '\'' && last == '\'')) {
            return value.substr(1, value.size() - 2);
        }
    }
    return value;
}

bool read_first_line_from_paths(
    const std::vector<std::string>& paths,
    std::string* out_line
) {
    if (out_line) out_line->clear();
    for (const auto& path : paths) {
        std::ifstream in(path);
        if (!in) continue;
        std::string line;
        if (!std::getline(in, line)) continue;
        if (out_line) *out_line = trim(line);
        return true;
    }
    return false;
}

std::map<std::string, std::string> load_gpkg_runtime_release_fields() {
    std::map<std::string, std::string> fields;
    const std::vector<std::string> release_paths = {
        ROOT_PREFIX + "/etc/os-release",
        ROOT_PREFIX + "/usr/lib/os-release",
    };

    for (const auto& path : release_paths) {
        std::ifstream in(path);
        if (!in) continue;

        std::string line;
        while (std::getline(in, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#') continue;
            size_t eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            std::string key = trim(line.substr(0, eq));
            std::string value = strip_matching_quotes(trim(line.substr(eq + 1)));
            if (!key.empty()) fields[key] = value;
        }

        if (!fields.empty()) return fields;
    }

    return fields;
}

const GpkgCliVersionInfo& get_gpkg_cli_version_info() {
    static const GpkgCliVersionInfo info = []() {
        GpkgCliVersionInfo loaded;
        std::map<std::string, std::string> fields = load_gpkg_runtime_release_fields();
        auto it = fields.find("VERSION");
        if (loaded.version_label == OS_VERSION &&
            it != fields.end() &&
            !it->second.empty()) {
            loaded.version_label = it->second;
        }
        it = fields.find("VERSION_CODENAME");
        if (it != fields.end() && !it->second.empty()) loaded.codename = it->second;
        it = fields.find("BUILD_ID");
        if (it != fields.end() && !it->second.empty()) loaded.build_id = it->second;
        if (loaded.build_id.empty()) {
            it = fields.find("IMAGE_VERSION");
            if (it != fields.end() && !it->second.empty()) loaded.build_id = it->second;
        }

        if (loaded.version_label == GPKG_VERSION && loaded.version_label == OS_VERSION) {
            std::string geminios_version;
            if (read_first_line_from_paths({ROOT_PREFIX + "/etc/geminios-version"}, &geminios_version) &&
                !geminios_version.empty()) {
                loaded.version_label = geminios_version;
            }
        }

        if (loaded.build_id.empty()) {
            std::string geminios_build_id;
            if (read_first_line_from_paths({ROOT_PREFIX + "/etc/geminios-build-id"}, &geminios_build_id) &&
                !geminios_build_id.empty()) {
                loaded.build_id = geminios_build_id;
            }
        }

        return loaded;
    }();
    return info;
}

std::string get_gpkg_version_banner() {
    const GpkgCliVersionInfo& info = get_gpkg_cli_version_info();
    std::ostringstream out;
    out << "gpkg " << info.version_label;
    if (!info.build_id.empty()) out << " [build " << info.build_id << "]";
    out << " (" << info.codename << ")";
    return out.str();
}

std::string get_gpkg_help_banner() {
    const GpkgCliVersionInfo& info = get_gpkg_cli_version_info();
    std::ostringstream out;
    out << "GeminiOS Package Manager " << info.version_label;
    if (!info.build_id.empty()) out << " [build " << info.build_id << "]";
    out << " (" << info.codename << ")";
    return out.str();
}

void print_version() {
    std::cout << get_gpkg_version_banner() << std::endl;
}

void print_help() {
    std::cout << "Usage: gpkg <command> [args] [options]\n"
              << get_gpkg_help_banner() << "\n\n"
              << "Options:\n"
              << "  -h, --help      Show this help text\n"
              << "  -v, --verbose   Show detailed logging information\n"
              << "  -y, --yes       Assume yes for confirmation prompts\n"
              << "  -r, --repair    Repair broken dependencies and damaged installs\n"
              << "  --reinstall     Reinstall requested packages even if the same version is already installed\n"
              << "  --defer-services  Prevent maintainer scripts from starting/restarting services during the transaction\n"
              << "  --unsafe-io    Skip safe file syncs during package writes for faster installs (less crash-safe)\n"
              << "  --recommended-yes, -rec  Force installation of Debian Recommends for this transaction\n"
              << "  --recommended-no, -nrec  Do not install Debian Recommends for this transaction\n"
              << "  --suggested-yes, -sug  Force installation of Debian Suggests for this transaction\n"
              << "  --suggested-no, -nsug  Do not install Debian Suggests for this transaction\n"
              << "  --autoremove    Remove newly unneeded dependency packages during remove\n"
              << "  --purge         Also purge package conffiles during remove/autoremove\n"
              << "  -V, --version   Show version\n\n"
              << "Commands:\n"
              << "  install <pkg>   Download and install packages (up to 5 archives in parallel)\n"
              << "  remove <pkg>    Remove an installed package (--purge to purge conffiles too)\n"
              << "  autoremove      Remove automatically installed packages that are no longer needed\n"
              << "  repair          Repair broken dependencies and reinstall damaged packages\n"
              << "  doctor          Inspect gpkg, repository, and upgrade health\n"
              << "  upgrade         Upgrade all installed packages\n"
              << "  update          Update local package indices\n"
              << "  search <query>  Search for packages\n"
              << "  show <pkg>      Show package metadata and source repository\n"
              << "  add-repo <url>  Add a third-party repository\n"
              << "  list-repos      Show configured repositories\n"
              << "  clean           Clear package cache\n";
}

int main(int argc, char* argv[]) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    if (argc < 2) {
        print_help();
        return 1;
    }

    std::string action;
    bool verbose = false;
    bool assume_yes = false;
    bool purge = false;
    bool autoremove = false;
    bool repair = false;
    bool reinstall = false;
    bool defer_services = false;
    bool unsafe_io = false;
    bool recommended_yes = false;
    bool recommended_no = false;
    bool suggested_yes = false;
    bool suggested_no = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose") verbose = true;
        else if (arg == "-y" || arg == "--yes") assume_yes = true;
        else if (arg == "--purge") purge = true;
        else if (arg == "--autoremove") autoremove = true;
        else if (arg == "-r" || arg == "--repair") repair = true;
        else if (arg == "--reinstall") reinstall = true;
        else if (arg == "--defer-services") defer_services = true;
        else if (arg == "--unsafe-io") unsafe_io = true;
        else if (arg == "--recommended-yes" || arg == "-rec") recommended_yes = true;
        else if (arg == "--recommended-no" || arg == "-nrec") recommended_no = true;
        else if (arg == "--suggested-yes" || arg == "-sug") suggested_yes = true;
        else if (arg == "--suggested-no" || arg == "-nsug") suggested_no = true;
        else if (arg == "-h" || arg == "--help") {
            action = "help";
        } else if (arg == "-V" || arg == "--version") {
            if (action.empty()) action = "version";
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << Color::RED
                      << "E: Unknown option '" << arg << "'."
                      << Color::RESET << std::endl;
            return 1;
        } else if (action.empty()) {
            action = arg;
        }
    }

    if (repair) {
        if (!action.empty() && action != "repair") {
            std::cerr << Color::RED
                      << "E: --repair cannot be combined with the '" << action
                      << "' command. Use 'gpkg repair' or 'gpkg --repair'."
                      << Color::RESET << std::endl;
            return 1;
        }
        action = "repair";
    }

    if (action.empty()) {
        print_help();
        return 1;
    }

    if (recommended_yes && recommended_no) {
        std::cerr << Color::RED
                  << "E: --recommended-yes and --recommended-no cannot be used together."
                  << Color::RESET << std::endl;
        return 1;
    }
    if (suggested_yes && suggested_no) {
        std::cerr << Color::RED
                  << "E: --suggested-yes and --suggested-no cannot be used together."
                  << Color::RESET << std::endl;
        return 1;
    }

    bool optional_flags_requested = recommended_yes || recommended_no || suggested_yes || suggested_no;
    if (optional_flags_requested &&
        action != "install" &&
        action != "upgrade" &&
        action != "repair") {
        std::cerr << Color::RED
                  << "E: optional dependency flags are only valid with install, upgrade, or repair."
                  << Color::RESET << std::endl;
        return 1;
    }

    if (reinstall &&
        action != "install" &&
        action != "upgrade") {
        std::cerr << Color::RED
                  << "E: --reinstall is only valid with install or upgrade."
                  << Color::RESET << std::endl;
        return 1;
    }
    if (purge &&
        action != "remove" &&
        action != "autoremove") {
        std::cerr << Color::RED
                  << "E: --purge is only valid with remove or autoremove."
                  << Color::RESET << std::endl;
        return 1;
    }
    if (autoremove && action != "remove") {
        std::cerr << Color::RED
                  << "E: --autoremove is only valid with remove."
                  << Color::RESET << std::endl;
        return 1;
    }
    if (action == "autoremove") {
        std::vector<std::string> operands = collect_cli_operands(argc, argv, 2);
        if (!operands.empty()) {
            std::cerr << Color::RED
                      << "E: gpkg autoremove does not take package names."
                      << Color::RESET << std::endl;
            return 1;
        }
    }

    bool mutates_package_runtime = (
        action == "install" ||
        action == "remove" ||
        action == "autoremove" ||
        action == "repair" ||
        action == "upgrade"
    );
    if ((defer_services || unsafe_io) && !mutates_package_runtime) {
        std::cerr << Color::RED
                  << "E: --defer-services and --unsafe-io are only valid with install, remove, autoremove, repair, or upgrade."
                  << Color::RESET << std::endl;
        return 1;
    }

    g_assume_yes = assume_yes;
    g_force_reinstall = reinstall;
    g_defer_services = defer_services;
    g_unsafe_io = unsafe_io;
    g_optional_dependency_policy.recommends = recommended_yes ? OptionalDependencyMode::ForceYes
        : (recommended_no ? OptionalDependencyMode::ForceNo : OptionalDependencyMode::Auto);
    g_optional_dependency_policy.suggests = suggested_yes ? OptionalDependencyMode::ForceYes
        : (suggested_no ? OptionalDependencyMode::ForceNo : OptionalDependencyMode::Auto);

#ifndef DEV_MODE
    if (geteuid() != 0 &&
        (action == "install" || action == "remove" || action == "autoremove" || action == "update" ||
         action == "add-repo" || action == "clean" || action == "upgrade" ||
         action == "repair")) {
        std::cerr << Color::RED << "E: This command requires root privileges." << Color::RESET << std::endl;
        return 1;
    }
#endif

    bool needs_trans = (
        action == "install" ||
        action == "remove" ||
        action == "autoremove" ||
        action == "repair" ||
        action == "upgrade" ||
        action == "update" ||
        action == "add-repo" ||
        action == "clean"
    );
    if (unsafe_io && mutates_package_runtime) {
        std::cout << Color::YELLOW
                  << "W: Unsafe I/O enabled. Package writes will skip some safety syncs for speed."
                  << Color::RESET << std::endl;
    }
    if (defer_services && mutates_package_runtime && verbose) {
        std::cout << "[DEBUG] Service start/restart suppression is enabled for this transaction."
                  << std::endl;
    }
    TransactionGuard guard(needs_trans, verbose, defer_services && mutates_package_runtime);

    std::set<std::string> installed_cache = get_exact_live_installed_package_set();

    if (action == "help") {
        print_help();
        return 0;
    }
    if (action == "version") {
        print_version();
        return 0;
    }
    if (action == "update") return handle_update(verbose);
    if (action == "upgrade") return handle_upgrade(installed_cache, verbose);
    if (action == "repair") return handle_repair(verbose);
    if (action == "doctor") return handle_doctor(verbose);
    if (action == "install" && argc > 2) return handle_install(argc, argv, installed_cache, verbose);
    if (action == "remove" && argc > 2) return handle_remove(argc, argv, verbose, purge, autoremove);
    if (action == "autoremove") return handle_autoremove(verbose, purge);
    if (action == "search") {
        std::string operand = first_cli_operand(argc, argv, 2);
        if (!operand.empty()) return handle_search(operand, verbose);
    }
    if (action == "show") {
        std::string operand = first_cli_operand(argc, argv, 2);
        if (!operand.empty()) return handle_show(operand, verbose);
    }
    if (action == "clean") return handle_clean(verbose);
    if (action == "add-repo") {
        std::string operand = first_cli_operand(argc, argv, 2);
        if (!operand.empty()) return handle_add_repo(operand, verbose);
    }
    if (action == "list-repos") return handle_list_repos();

    print_help();
    return 0;
}
