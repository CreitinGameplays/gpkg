// Minimal JSON parser for gpkg policy/config files.

enum class JsonType {
    Null,
    String,
    Bool,
    Array,
    Object,
};

struct JsonValue {
    JsonType type = JsonType::Null;
    std::string string_value;
    bool bool_value = false;
    std::vector<JsonValue> array_items;
    std::map<std::string, JsonValue> object_items;

    bool is_null() const { return type == JsonType::Null; }
    bool is_string() const { return type == JsonType::String; }
    bool is_bool() const { return type == JsonType::Bool; }
    bool is_array() const { return type == JsonType::Array; }
    bool is_object() const { return type == JsonType::Object; }
};

void skip_json_whitespace(const std::string& input, size_t& pos) {
    while (pos < input.size() && std::isspace(static_cast<unsigned char>(input[pos]))) ++pos;
}

bool parse_json_string_token(const std::string& input, size_t& pos, std::string& out) {
    if (pos >= input.size() || input[pos] != '"') return false;
    ++pos;

    std::string raw;
    while (pos < input.size()) {
        char ch = input[pos++];
        if (ch == '"') {
            out = json_unescape(raw);
            return true;
        }
        if (ch == '\\' && pos < input.size()) {
            raw += ch;
            raw += input[pos++];
            continue;
        }
        raw += ch;
    }

    return false;
}

bool parse_json_literal(const std::string& input, size_t& pos, const std::string& literal) {
    if (input.compare(pos, literal.size(), literal) != 0) return false;
    pos += literal.size();
    return true;
}

bool parse_json_value_token(const std::string& input, size_t& pos, JsonValue& out);

bool parse_json_array_token(const std::string& input, size_t& pos, JsonValue& out) {
    if (pos >= input.size() || input[pos] != '[') return false;
    ++pos;
    out = {};
    out.type = JsonType::Array;

    skip_json_whitespace(input, pos);
    if (pos < input.size() && input[pos] == ']') {
        ++pos;
        return true;
    }

    while (pos < input.size()) {
        JsonValue item;
        if (!parse_json_value_token(input, pos, item)) return false;
        out.array_items.push_back(item);

        skip_json_whitespace(input, pos);
        if (pos < input.size() && input[pos] == ',') {
            ++pos;
            skip_json_whitespace(input, pos);
            continue;
        }
        if (pos < input.size() && input[pos] == ']') {
            ++pos;
            return true;
        }
        return false;
    }

    return false;
}

bool parse_json_object_token(const std::string& input, size_t& pos, JsonValue& out) {
    if (pos >= input.size() || input[pos] != '{') return false;
    ++pos;
    out = {};
    out.type = JsonType::Object;

    skip_json_whitespace(input, pos);
    if (pos < input.size() && input[pos] == '}') {
        ++pos;
        return true;
    }

    while (pos < input.size()) {
        std::string key;
        if (!parse_json_string_token(input, pos, key)) return false;

        skip_json_whitespace(input, pos);
        if (pos >= input.size() || input[pos] != ':') return false;
        ++pos;
        skip_json_whitespace(input, pos);

        JsonValue value;
        if (!parse_json_value_token(input, pos, value)) return false;
        out.object_items[key] = value;

        skip_json_whitespace(input, pos);
        if (pos < input.size() && input[pos] == ',') {
            ++pos;
            skip_json_whitespace(input, pos);
            continue;
        }
        if (pos < input.size() && input[pos] == '}') {
            ++pos;
            return true;
        }
        return false;
    }

    return false;
}

bool parse_json_value_token(const std::string& input, size_t& pos, JsonValue& out) {
    skip_json_whitespace(input, pos);
    if (pos >= input.size()) return false;

    if (input[pos] == '"') {
        out = {};
        out.type = JsonType::String;
        return parse_json_string_token(input, pos, out.string_value);
    }
    if (input[pos] == '{') return parse_json_object_token(input, pos, out);
    if (input[pos] == '[') return parse_json_array_token(input, pos, out);
    if (parse_json_literal(input, pos, "true")) {
        out = {};
        out.type = JsonType::Bool;
        out.bool_value = true;
        return true;
    }
    if (parse_json_literal(input, pos, "false")) {
        out = {};
        out.type = JsonType::Bool;
        out.bool_value = false;
        return true;
    }
    if (parse_json_literal(input, pos, "null")) {
        out = {};
        out.type = JsonType::Null;
        return true;
    }

    return false;
}

bool parse_json_document(const std::string& input, JsonValue& out) {
    size_t pos = 0;
    if (!parse_json_value_token(input, pos, out)) return false;
    skip_json_whitespace(input, pos);
    return pos == input.size();
}

bool load_json_document(const std::string& path, JsonValue& out) {
    std::ifstream f(path);
    if (!f) return false;

    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return parse_json_document(content, out);
}

const JsonValue* json_object_get(const JsonValue& value, const std::string& key) {
    if (!value.is_object()) return nullptr;
    auto it = value.object_items.find(key);
    if (it == value.object_items.end()) return nullptr;
    return &it->second;
}

std::string json_string_or(const JsonValue* value, const std::string& fallback = "") {
    if (!value || !value->is_string()) return fallback;
    return value->string_value;
}

bool json_bool_or(const JsonValue* value, bool fallback = false) {
    if (!value || !value->is_bool()) return fallback;
    return value->bool_value;
}

std::vector<std::string> json_string_array(const JsonValue* value) {
    std::vector<std::string> items;
    if (!value || !value->is_array()) return items;
    for (const auto& entry : value->array_items) {
        if (entry.is_string()) items.push_back(entry.string_value);
    }
    return items;
}
