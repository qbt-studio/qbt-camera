#pragma once
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <string_view>
#include <variant>

// Flat JSON objects as exchanged with the phone: {"key": number | true | false | "text" | null, ...}.
class JsonObject {
public:
    static bool Parse(std::string_view text, JsonObject& out) {
        Parser parser{text};
        return parser.Object(out);
    }

    bool Has(const std::string& key) const { return values_.count(key) != 0; }

    double Number(const std::string& key, double fallback = 0) const {
        const auto it = values_.find(key);
        return it != values_.end() && std::holds_alternative<double>(it->second) ? std::get<double>(it->second) : fallback;
    }

    bool Bool(const std::string& key, bool fallback = false) const {
        const auto it = values_.find(key);
        return it != values_.end() && std::holds_alternative<bool>(it->second) ? std::get<bool>(it->second) : fallback;
    }

    std::string String(const std::string& key) const {
        const auto it = values_.find(key);
        return it != values_.end() && std::holds_alternative<std::string>(it->second) ? std::get<std::string>(it->second)
                                                                                     : std::string();
    }

private:
    using Value = std::variant<double, bool, std::string>;

    struct Parser {
        std::string_view text;
        size_t at = 0;

        void Space() {
            while (at < text.size() && (text[at] == ' ' || text[at] == '\n' || text[at] == '\r' || text[at] == '\t')) ++at;
        }

        bool Take(char expected) {
            Space();
            if (at < text.size() && text[at] == expected) {
                ++at;
                return true;
            }
            return false;
        }

        bool Literal(std::string_view word) {
            if (text.substr(at, word.size()) != word) return false;
            at += word.size();
            return true;
        }

        static void AppendUtf8(std::string& out, unsigned codepoint) {
            if (codepoint < 0x80) {
                out += static_cast<char>(codepoint);
            } else if (codepoint < 0x800) {
                out += static_cast<char>(0xC0 | (codepoint >> 6));
                out += static_cast<char>(0x80 | (codepoint & 0x3F));
            } else {
                out += static_cast<char>(0xE0 | (codepoint >> 12));
                out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
        }

        bool String(std::string& out) {
            if (!Take('"')) return false;
            while (at < text.size() && text[at] != '"') {
                char c = text[at++];
                if (c != '\\') {
                    out += c;
                    continue;
                }
                if (at >= text.size()) return false;
                c = text[at++];
                switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (at + 4 > text.size()) return false;
                    const std::string hex(text.substr(at, 4));
                    at += 4;
                    AppendUtf8(out, static_cast<unsigned>(strtoul(hex.c_str(), nullptr, 16)));
                    break;
                }
                default: out += c; break; // \" \\ \/
                }
            }
            return Take('"');
        }

        bool Object(JsonObject& out) {
            if (!Take('{')) return false;
            if (Take('}')) return true;
            do {
                std::string key;
                if (!String(key) || !Take(':')) return false;
                Space();
                if (at >= text.size()) return false;
                const char c = text[at];
                if (c == '"') {
                    std::string value;
                    if (!String(value)) return false;
                    out.values_[key] = std::move(value);
                } else if (Literal("true")) {
                    out.values_[key] = true;
                } else if (Literal("false")) {
                    out.values_[key] = false;
                } else if (Literal("null")) {
                    // skipped
                } else {
                    const std::string rest(text.substr(at, 40));
                    char* end = nullptr;
                    const double number = strtod(rest.c_str(), &end);
                    if (end == rest.c_str()) return false; // nested objects/arrays are not used
                    at += static_cast<size_t>(end - rest.c_str());
                    out.values_[key] = number;
                }
            } while (Take(','));
            return Take('}');
        }
    };

    std::map<std::string, Value> values_;
};
