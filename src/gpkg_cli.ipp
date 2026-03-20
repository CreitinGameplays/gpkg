// CLI entrypoint and top-level command dispatch.

void print_help() {
    std::cout << "Usage: gpkg <command> [args] [--verbose]\n"
              << "GeminiOS Package Manager (v2.1 - Genesis)\n\n"
              << "Options:\n"
              << "  -v, --verbose   Show detailed logging information\n\n"
              << "Commands:\n"
              << "  install <pkg>   Download and install packages (up to 5 archives in parallel)\n"
              << "  remove <pkg>    Remove an installed package (--purge to remove unneeded deps)\n"
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

    std::string action = argv[1];
    bool verbose = false;
    bool purge = false;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose") verbose = true;
        if (arg == "--purge") purge = true;
    }

#ifndef DEV_MODE
    if (geteuid() != 0 &&
        (action == "install" || action == "remove" || action == "update" ||
         action == "add-repo" || action == "clean" || action == "upgrade")) {
        std::cerr << Color::RED << "E: This command requires root privileges." << Color::RESET << std::endl;
        return 1;
    }
#endif

    bool needs_trans = (
        action == "install" ||
        action == "remove" ||
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

    if (action == "update") return handle_update(verbose);
    if (action == "upgrade") return handle_upgrade(installed_cache, verbose);
    if (action == "install" && argc > 2) return handle_install(argc, argv, installed_cache, verbose);
    if (action == "remove" && argc > 2) return handle_remove(argc, argv, verbose, purge);
    if (action == "search" && argc > 2) return handle_search(argv[2], verbose);
    if (action == "show" && argc > 2) return handle_show(argv[2], verbose);
    if (action == "clean") return handle_clean(verbose);
    if (action == "add-repo" && argc > 2) return handle_add_repo(argv[2], verbose);
    if (action == "list-repos") return handle_list_repos();

    print_help();
    return 0;
}
