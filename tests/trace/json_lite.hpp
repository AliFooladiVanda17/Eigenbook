//
// Minimal JSON reader and TLA+ value writer for the trace tools (Phase B2.2).
//
// Reads the subset of JSON the trace format uses (objects, arrays, strings without escapes other
// than \" and \\, integers, true, false) and writes a parsed value as a TLA+ expression the way
// the CommunityModules Json module maps JSON to TLA+: object -> record, array -> tuple,
// string -> string, integer -> integer, boolean -> BOOLEAN. Test tooling only; not part of the
// library.
//

#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace eigenbook::trace {

struct JsonValue {
    enum class Kind : std::uint8_t {
        NUL,
        BOOL,
        INT,
        STRING,
        ARRAY,
        OBJECT
    };

    Kind kind{Kind::NUL};
    bool boolean{false};
    std::int64_t integer{0};
    std::string string;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::string, JsonValue>> object; // keeps the input order

    [[nodiscard]] const JsonValue* find(const std::string_view key) const {
        for (const auto& [k, v] : object) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }

    [[nodiscard]] JsonValue* find(const std::string_view key) {
        for (auto& [k, v] : object) {
            if (k == key) {
                return &v;
            }
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string_view text) : in(text) {}

    // The whole input must be exactly one value (surrounding white space allowed).
    [[nodiscard]] std::optional<JsonValue> parse() {
        JsonValue v;
        if (!value(v)) {
            return std::nullopt;
        }
        skipSpace();
        if (pos != in.size()) {
            return std::nullopt;
        }
        return v;
    }

private:
    void skipSpace() {
        while (pos < in.size() && std::isspace(static_cast<unsigned char>(in[pos])) != 0) {
            ++pos;
        }
    }

    bool consume(const char c) {
        skipSpace();
        if (pos < in.size() && in[pos] == c) {
            ++pos;
            return true;
        }
        return false;
    }

    bool literal(const std::string_view word) {
        if (in.substr(pos, word.size()) == word) {
            pos += word.size();
            return true;
        }
        return false;
    }

    bool str(std::string& out) {
        if (!consume('"')) {
            return false;
        }
        while (pos < in.size() && in[pos] != '"') {
            if (in[pos] == '\\') {
                if (pos + 1 >= in.size() || (in[pos + 1] != '"' && in[pos + 1] != '\\')) {
                    return false; // the trace format needs no other escapes
                }
                ++pos;
            }
            out.push_back(in[pos++]);
        }
        if (pos >= in.size()) {
            return false;
        }
        ++pos; // closing quote
        return true;
    }

    bool value(JsonValue& v) {
        skipSpace();
        if (pos >= in.size()) {
            return false;
        }
        const char c = in[pos];
        if (c == '{') {
            ++pos;
            v.kind = JsonValue::Kind::OBJECT;
            if (consume('}')) {
                return true;
            }
            do {
                std::string key;
                JsonValue member;
                if (!str(key) || !consume(':') || !value(member)) {
                    return false;
                }
                v.object.emplace_back(std::move(key), std::move(member));
            } while (consume(','));
            return consume('}');
        }
        if (c == '[') {
            ++pos;
            v.kind = JsonValue::Kind::ARRAY;
            if (consume(']')) {
                return true;
            }
            do {
                JsonValue element;
                if (!value(element)) {
                    return false;
                }
                v.array.push_back(std::move(element));
            } while (consume(','));
            return consume(']');
        }
        if (c == '"') {
            v.kind = JsonValue::Kind::STRING;
            return str(v.string);
        }
        if (literal("true")) {
            v.kind = JsonValue::Kind::BOOL;
            v.boolean = true;
            return true;
        }
        if (literal("false")) {
            v.kind = JsonValue::Kind::BOOL;
            return true;
        }
        if (literal("null")) {
            return true;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)) != 0) {
            v.kind = JsonValue::Kind::INT;
            bool negative = false;
            if (c == '-') {
                negative = true;
                ++pos;
            }
            const std::size_t start = pos;
            std::int64_t n = 0;
            while (pos < in.size() && std::isdigit(static_cast<unsigned char>(in[pos])) != 0) {
                n = n * 10 + (in[pos] - '0');
                ++pos;
            }
            if (pos == start) {
                return false;
            }
            v.integer = negative ? -n : n;
            return true;
        }
        return false;
    }

    std::string_view in;
    std::size_t pos{0};
};

// The TLA+ expression for a parsed JSON value. An empty object has no TLA+ record form; the trace
// format never produces one, so it is written as the empty tuple.
inline void writeTla(const JsonValue& v, std::string& out) {
    switch (v.kind) {
        case JsonValue::Kind::NUL:
            out += "<<>>";
            return;
        case JsonValue::Kind::BOOL:
            out += v.boolean ? "TRUE" : "FALSE";
            return;
        case JsonValue::Kind::INT:
            out += std::to_string(v.integer);
            return;
        case JsonValue::Kind::STRING:
            out += '"';
            out += v.string;
            out += '"';
            return;
        case JsonValue::Kind::ARRAY:
            {
                out += "<<";
                for (std::size_t i = 0; i < v.array.size(); ++i) {
                    out += i == 0 ? "" : ", ";
                    writeTla(v.array[i], out);
                }
                out += ">>";
                return;
            }
        case JsonValue::Kind::OBJECT:
            {
                if (v.object.empty()) {
                    out += "<<>>";
                    return;
                }
                out += '[';
                for (std::size_t i = 0; i < v.object.size(); ++i) {
                    out += i == 0 ? "" : ", ";
                    out += v.object[i].first;
                    out += " |-> ";
                    writeTla(v.object[i].second, out);
                }
                out += ']';
                return;
            }
    }
}

[[nodiscard]] inline std::string toTla(const JsonValue& v) {
    std::string out;
    writeTla(v, out);
    return out;
}

} // namespace eigenbook::trace
