// Import policy loading and Debian dependency normalization.

#include <fnmatch.h>

struct RelationAtom {
    std::string name;
    std::string op;
    std::string version;
    std::string normalized;
    bool valid = false;
};

struct PackageOverridePolicy {
    std::string section;
    std::string rename;
    bool has_include_recommends = false;
    bool include_recommends = true;
    std::vector<std::string> depends_add;
    std::vector<std::string> depends_remove;
};

struct ImportPolicy {
    std::vector<std::string> skip_dependency_patterns;
    std::vector<std::string> skip_packages;
    std::map<std::string, std::string> dependency_choices;
    std::map<std::string, std::string> dependency_rewrites;
    std::map<std::string, std::string> provider_choices;
    std::map<std::string, PackageOverridePolicy> package_overrides;
};

bool wildcard_match(const std::string& value, const std::string& pattern) {
    return fnmatch(pattern.c_str(), value.c_str(), 0) == 0;
}

bool matches_any_pattern(const std::string& value, const std::vector<std::string>& patterns) {
    for (const auto& pattern : patterns) {
        if (wildcard_match(value, pattern)) return true;
    }
    return false;
}

std::vector<std::string> split_top_level_text(const std::string& value, char separator) {
    std::vector<std::string> parts;
    std::string current;
    int paren_depth = 0;
    int bracket_depth = 0;
    int angle_depth = 0;

    for (char ch : value) {
        if (ch == '(') ++paren_depth;
        else if (ch == ')' && paren_depth > 0) --paren_depth;
        else if (ch == '[') ++bracket_depth;
        else if (ch == ']' && bracket_depth > 0) --bracket_depth;
        else if (ch == '<' && paren_depth == 0 && bracket_depth == 0) ++angle_depth;
        else if (ch == '>' && paren_depth == 0 && bracket_depth == 0 && angle_depth > 0) --angle_depth;

        if (ch == separator && paren_depth == 0 && bracket_depth == 0 && angle_depth == 0) {
            std::string part = trim(current);
            if (!part.empty()) parts.push_back(part);
            current.clear();
            continue;
        }

        current += ch;
    }

    std::string final_part = trim(current);
    if (!final_part.empty()) parts.push_back(final_part);
    return parts;
}

bool dependency_applies_to_arch(const std::string& restriction, const std::string& apt_arch) {
    std::istringstream iss(restriction);
    std::string token;
    std::set<std::string> positives;
    std::set<std::string> negatives;

    while (iss >> token) {
        if (!token.empty() && token[0] == '!') negatives.insert(token.substr(1));
        else positives.insert(token);
    }

    if (!positives.empty() && !positives.count(apt_arch) && !positives.count("any")) return false;
    if (negatives.count(apt_arch) || negatives.count("any")) return false;
    return true;
}

RelationAtom normalize_relation_atom(const std::string& raw_atom, const std::string& apt_arch) {
    RelationAtom parsed;
    std::string cleaned = trim(raw_atom);
    if (cleaned.empty()) return parsed;

    std::regex profile_re("\\s*<[^>]+>");
    cleaned = std::regex_replace(cleaned, profile_re, "");
    cleaned = trim(cleaned);

    size_t bracket_open = cleaned.find('[');
    size_t bracket_close = cleaned.find(']', bracket_open == std::string::npos ? 0 : bracket_open);
    if (bracket_open != std::string::npos && bracket_close != std::string::npos && bracket_close > bracket_open) {
        std::string restriction = cleaned.substr(bracket_open + 1, bracket_close - bracket_open - 1);
        if (!dependency_applies_to_arch(restriction, apt_arch)) return parsed;
        cleaned.erase(bracket_open, bracket_close - bracket_open + 1);
        cleaned = trim(cleaned);
    }

    size_t paren_open = cleaned.find('(');
    std::string base = trim(paren_open == std::string::npos ? cleaned : cleaned.substr(0, paren_open));
    if (base.empty()) return parsed;

    size_t colon = base.find(':');
    if (colon != std::string::npos) {
        std::string suffix = base.substr(colon + 1);
        if (suffix == "any" || suffix == "native" || suffix == apt_arch) {
            base = base.substr(0, colon);
        }
    }

    parsed.name = base;
    if (paren_open != std::string::npos) {
        size_t paren_close = cleaned.find(')', paren_open);
        if (paren_close != std::string::npos) {
            std::string content = trim(cleaned.substr(paren_open + 1, paren_close - paren_open - 1));
            const std::vector<std::string> ops = {">=", "<=", "<<", ">>", "==", "=", ">", "<"};
            for (const auto& op : ops) {
                if (content.compare(0, op.size(), op) == 0) {
                    parsed.op = op;
                    parsed.version = trim(content.substr(op.size()));
                    break;
                }
            }
        }
    }

    parsed.normalized = parsed.op.empty()
        ? parsed.name
        : parsed.name + " (" + parsed.op + " " + parsed.version + ")";
    parsed.valid = !parsed.name.empty();
    return parsed;
}

std::vector<std::string> unique_string_list(const std::vector<std::string>& values) {
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const auto& value : values) {
        if (seen.insert(value).second) result.push_back(value);
    }
    return result;
}

std::vector<std::string> load_pattern_entries_file(const std::string& path) {
    std::vector<std::string> entries;
    std::ifstream f(path);
    if (!f) return entries;

    std::string line;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t bracket = line.find('[');
        if (bracket != std::string::npos) line = trim(line.substr(0, bracket));
        size_t angle = line.find('<');
        if (angle != std::string::npos) line = trim(line.substr(0, angle));
        size_t paren = line.find('(');
        if (paren != std::string::npos) line = trim(line.substr(0, paren));
        if (!line.empty()) entries.push_back(line);
    }
    return unique_string_list(entries);
}

bool pattern_has_glob(const std::string& pattern) {
    return pattern.find_first_of("*?[]") != std::string::npos;
}

bool should_keep_upgradeable_pattern(
    const std::string& pattern,
    const std::set<std::string>& available_packages
) {
    if (pattern_has_glob(pattern)) return true;
    return !available_packages.count(pattern);
}

std::vector<std::string> build_system_drop_patterns(const std::set<std::string>& available_packages) {
    std::vector<std::string> base_patterns = load_pattern_entries_file(SYSTEM_PROVIDES_PATH);
    std::vector<std::string> upgradeable_patterns = load_pattern_entries_file(UPGRADEABLE_SYSTEM_PATH);
    std::vector<std::string> filtered;

    for (const auto& pattern : base_patterns) {
        if (matches_any_pattern(pattern, upgradeable_patterns) &&
            !should_keep_upgradeable_pattern(pattern, available_packages)) {
            continue;
        }
        filtered.push_back(pattern);
    }

    for (const auto& pattern : upgradeable_patterns) {
        if (should_keep_upgradeable_pattern(pattern, available_packages) &&
            std::find(filtered.begin(), filtered.end(), pattern) == filtered.end()) {
            filtered.push_back(pattern);
        }
    }

    return unique_string_list(filtered);
}

ImportPolicy load_import_policy(bool verbose = false) {
    JsonValue root;
    if (!load_json_document(IMPORT_POLICY_PATH, root)) {
        if (verbose) {
            std::cout << "[DEBUG] No import policy found at " << IMPORT_POLICY_PATH
                      << "; using built-in defaults." << std::endl;
        }
        return {};
    }

    ImportPolicy policy;
    policy.skip_dependency_patterns = json_string_array(json_object_get(root, "skip_dependency_patterns"));
    policy.skip_packages = json_string_array(json_object_get(root, "skip_packages"));

    const JsonValue* dependency_choices = json_object_get(root, "dependency_choices");
    if (dependency_choices && dependency_choices->is_object()) {
        for (const auto& entry : dependency_choices->object_items) {
            policy.dependency_choices[entry.first] = json_string_or(&entry.second);
        }
    }

    const JsonValue* dependency_rewrites = json_object_get(root, "dependency_rewrites");
    if (dependency_rewrites && dependency_rewrites->is_object()) {
        for (const auto& entry : dependency_rewrites->object_items) {
            policy.dependency_rewrites[entry.first] = json_string_or(&entry.second);
        }
    }

    const JsonValue* provider_choices = json_object_get(root, "provider_choices");
    if (provider_choices && provider_choices->is_object()) {
        for (const auto& entry : provider_choices->object_items) {
            policy.provider_choices[entry.first] = json_string_or(&entry.second);
        }
    }

    const JsonValue* overrides = json_object_get(root, "package_overrides");
    if (overrides && overrides->is_object()) {
        for (const auto& entry : overrides->object_items) {
            if (!entry.second.is_object()) continue;
            PackageOverridePolicy package_override;
            package_override.section = json_string_or(json_object_get(entry.second, "section"));
            package_override.rename = json_string_or(json_object_get(entry.second, "rename"));
            if (const JsonValue* include_recommends = json_object_get(entry.second, "include_recommends")) {
                if (include_recommends->is_bool()) {
                    package_override.has_include_recommends = true;
                    package_override.include_recommends = include_recommends->bool_value;
                }
            }
            package_override.depends_add = json_string_array(json_object_get(entry.second, "depends_add"));
            package_override.depends_remove = json_string_array(json_object_get(entry.second, "depends_remove"));
            policy.package_overrides[entry.first] = package_override;
        }
    }

    return policy;
}

const ImportPolicy& get_import_policy(bool verbose = false) {
    static bool loaded = false;
    static ImportPolicy policy;
    if (!loaded) {
        policy = load_import_policy(verbose);
        loaded = true;
    }
    return policy;
}

bool is_blocked_import_package(const std::string& package_name, bool verbose = false) {
    const ImportPolicy& policy = get_import_policy(verbose);
    return matches_any_pattern(package_name, policy.skip_packages);
}

std::string apply_dependency_rewrite_name(
    const std::string& name,
    const std::map<std::string, std::string>& rewrites
) {
    for (const auto& entry : rewrites) {
        if (wildcard_match(name, entry.first)) return trim(entry.second);
    }
    return name;
}

std::string resolve_provider_name(
    const std::string& name,
    const std::map<std::string, std::string>& explicit_choices,
    const std::map<std::string, std::vector<std::string>>& provider_map,
    const std::set<std::string>& available_packages
) {
    auto explicit_it = explicit_choices.find(name);
    if (explicit_it != explicit_choices.end()) return explicit_it->second;

    auto it = provider_map.find(name);
    if (it == provider_map.end()) return "";

    std::vector<std::string> candidates;
    for (const auto& provider : it->second) {
        if (provider == name) continue;
        if (available_packages.count(provider)) candidates.push_back(provider);
    }
    if (candidates.size() == 1) return candidates[0];
    return "";
}

std::vector<std::string> normalize_relation_field_value(const std::string& value, const std::string& apt_arch) {
    std::vector<std::string> relations;
    for (const auto& group : split_top_level_text(value, ',')) {
        for (const auto& alternative : split_top_level_text(group, '|')) {
            RelationAtom parsed = normalize_relation_atom(alternative, apt_arch);
            if (parsed.valid) {
                relations.push_back(parsed.normalized);
                break;
            }
        }
    }
    return unique_string_list(relations);
}

std::vector<std::string> normalize_dependency_relation_value(
    const std::string& value,
    const std::string& package_name,
    const std::string& apt_arch,
    bool include_recommends,
    const ImportPolicy& policy,
    const std::set<std::string>& available_packages,
    const std::map<std::string, std::vector<std::string>>& provider_map,
    const std::vector<std::string>& system_drop_patterns
) {
    std::vector<std::string> normalized;
    std::vector<std::string> dependency_skip_patterns = policy.skip_packages;
    dependency_skip_patterns.insert(
        dependency_skip_patterns.end(),
        policy.skip_dependency_patterns.begin(),
        policy.skip_dependency_patterns.end()
    );

    for (const auto& group : split_top_level_text(value, ',')) {
        if (group.empty()) continue;

        std::vector<std::string> alternatives = split_top_level_text(group, '|');
        std::string override_key = package_name + "::" + group;
        auto explicit_choice_it = policy.dependency_choices.find(override_key);
        if (explicit_choice_it == policy.dependency_choices.end()) {
            explicit_choice_it = policy.dependency_choices.find(group);
        }

        std::string selected;
        bool dropped_as_system = false;

        auto consider_relation = [&](const std::string& raw_choice, bool require_exists) {
            RelationAtom parsed = normalize_relation_atom(raw_choice, apt_arch);
            if (!parsed.valid) return std::string();

            std::string original_name = parsed.name;
            std::string rewritten_name = apply_dependency_rewrite_name(parsed.name, policy.dependency_rewrites);
            if (rewritten_name.empty()) {
                dropped_as_system = true;
                return std::string();
            }
            parsed.name = rewritten_name;
            if (parsed.name != original_name) {
                parsed.op.clear();
                parsed.version.clear();
            }
            parsed.normalized = parsed.op.empty()
                ? parsed.name
                : parsed.name + " (" + parsed.op + " " + parsed.version + ")";

            std::string provider_name = resolve_provider_name(
                parsed.name,
                policy.provider_choices,
                provider_map,
                available_packages
            );
            bool keep_virtual_relation = !provider_name.empty() && !parsed.op.empty();
            std::string effective_name = keep_virtual_relation
                ? parsed.name
                : (provider_name.empty() ? parsed.name : provider_name);
            std::string effective_normalized = parsed.op.empty()
                ? effective_name
                : effective_name + " (" + parsed.op + " " + parsed.version + ")";

            if (matches_any_pattern(original_name, system_drop_patterns) ||
                matches_any_pattern(parsed.name, system_drop_patterns) ||
                matches_any_pattern(provider_name, system_drop_patterns) ||
                matches_any_pattern(effective_name, system_drop_patterns)) {
                dropped_as_system = true;
                return std::string();
            }

            if (matches_any_pattern(original_name, dependency_skip_patterns) ||
                matches_any_pattern(parsed.name, dependency_skip_patterns) ||
                matches_any_pattern(provider_name, dependency_skip_patterns) ||
                matches_any_pattern(effective_name, dependency_skip_patterns)) {
                return std::string();
            }

            bool relation_exists = available_packages.count(effective_name) > 0 ||
                (!provider_name.empty() && available_packages.count(provider_name) > 0);
            if (require_exists && !relation_exists) {
                return std::string();
            }

            return effective_normalized;
        };

        if (explicit_choice_it != policy.dependency_choices.end()) {
            selected = consider_relation(explicit_choice_it->second, false);
        } else {
            for (const auto& alternative : alternatives) {
                selected = consider_relation(alternative, true);
                if (!selected.empty()) break;
            }
        }

        if (selected.empty() && dropped_as_system) continue;
        if (selected.empty()) {
            for (const auto& alternative : alternatives) {
                selected = consider_relation(alternative, false);
                if (!selected.empty()) break;
            }
        }
        if (selected.empty() && dropped_as_system) continue;
        if (!selected.empty()) normalized.push_back(selected);
    }

    (void)include_recommends;
    return unique_string_list(normalized);
}
