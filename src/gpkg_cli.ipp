// CLI entrypoint and top-level command dispatch.

#ifndef GPKG_VERSION
#define GPKG_VERSION OS_VERSION
#endif

#ifndef GPKG_CODENAME
#define GPKG_CODENAME OS_CODENAME
#endif

void print_version() {
    std::cout << "gpkg " << GPKG_VERSION << " (" << GPKG_CODENAME << ")" << std::endl;
}

void print_help() {
    std::cout << "Usage: gpkg <command> [args] [options]\n"
              << "GeminiOS Package Manager " << GPKG_VERSION << " (" << GPKG_CODENAME << ")\n\n"
              << "Options:\n"
              << "  -v, --verbose   Show detailed logging information\n"
              << "  -y, --yes       Assume yes for confirmation prompts\n"
              << "  -r, --repair    Repair broken dependencies and damaged installs\n"
              << "  --recommended-yes  Force installation of Debian Recommends for this transaction\n"
              << "  --recommended-no   Do not install Debian Recommends for this transaction\n"
              << "  --suggested-yes    Force installation of Debian Suggests for this transaction\n"
              << "  --suggested-no     Do not install Debian Suggests for this transaction\n"
              << "  -V, --version   Show version\n\n"
              << "Commands:\n"
              << "  install <pkg>   Download and install packages (up to 5 archives in parallel)\n"
              << "  remove <pkg>    Remove an installed package (--purge to remove unneeded deps)\n"
              << "  repair          Repair broken dependencies and reinstall damaged packages\n"
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
    bool repair = false;
    bool recommended_yes = false;
    bool recommended_no = false;
    bool suggested_yes = false;
    bool suggested_no = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose") verbose = true;
        else if (arg == "-y" || arg == "--yes") assume_yes = true;
        else if (arg == "--purge") purge = true;
        else if (arg == "-r" || arg == "--repair") repair = true;
        else if (arg == "--recommended-yes") recommended_yes = true;
        else if (arg == "--recommended-no") recommended_no = true;
        else if (arg == "--suggested-yes") suggested_yes = true;
        else if (arg == "--suggested-no") suggested_no = true;
        else if (arg == "-V" || arg == "--version") {
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

    g_assume_yes = assume_yes;
    g_optional_dependency_policy.recommends = recommended_yes ? OptionalDependencyMode::ForceYes
        : (recommended_no ? OptionalDependencyMode::ForceNo : OptionalDependencyMode::Auto);
    g_optional_dependency_policy.suggests = suggested_yes ? OptionalDependencyMode::ForceYes
        : (suggested_no ? OptionalDependencyMode::ForceNo : OptionalDependencyMode::Auto);

#ifndef DEV_MODE
    if (geteuid() != 0 &&
        (action == "install" || action == "remove" || action == "update" ||
         action == "add-repo" || action == "clean" || action == "upgrade" ||
         action == "repair")) {
        std::cerr << Color::RED << "E: This command requires root privileges." << Color::RESET << std::endl;
        return 1;
    }
#endif

    bool needs_trans = (
        action == "install" ||
        action == "remove" ||
        action == "repair" ||
        action == "upgrade" ||
        action == "update" ||
        action == "add-repo" ||
        action == "clean"
    );
    TransactionGuard guard(needs_trans, verbose);

    std::set<std::string> installed_cache;
    for (const auto& pkg : get_installed_packages()) {
        installed_cache.insert(pkg);
    }

    if (action == "version") {
        print_version();
        return 0;
    }
    if (action == "update") return handle_update(verbose);
    if (action == "upgrade") return handle_upgrade(installed_cache, verbose);
    if (action == "repair") return handle_repair(verbose);
    if (action == "install" && argc > 2) return handle_install(argc, argv, installed_cache, verbose);
    if (action == "remove" && argc > 2) return handle_remove(argc, argv, verbose, purge);
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
