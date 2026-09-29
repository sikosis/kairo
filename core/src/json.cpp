#include "json.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace kairo::json {

Value Value::Boolean(bool value) { Value v; v.type = Type::Boolean; v.boolean = value; return v; }
Value Value::Number(double value) { Value v; v.type = Type::Number; v.number = value; return v; }
Value Value::String(std::string value) { Value v; v.type = Type::String; v.string = std::move(value); return v; }
Value Value::Array(std::vector<Value> value) { Value v; v.type = Type::Array; v.array = std::move(value); return v; }
Value Value::Object(std::map<std::string, Value> value) { Value v; v.type = Type::Object; v.object = std::move(value); return v; }

const Value& Value::At(const std::string& key) const {
    auto it = object.find(key);
    if (type != Type::Object || it == object.end()) throw std::runtime_error("missing JSON key: " + key);
    return it->second;
}

const Value* Value::Find(const std::string& key) const {
    if (type != Type::Object) return nullptr;
    auto it = object.find(key);
    return it == object.end() ? nullptr : &it->second;
}

std::string Value::GetString(const std::string& key, const std::string& fallback) const {
    const Value* value = Find(key);
    return value && value->type == Type::String ? value->string : fallback;
}

class Parser {
public:
    explicit Parser(const std::string& input) : input_(input) {}

    Value Run() {
        Value value = ParseValue(0);
        Space();
        if (position_ != input_.size()) Fail("trailing data");
        return value;
    }

private:
    [[noreturn]] void Fail(const std::string& reason) const {
        throw std::runtime_error("invalid JSON at byte " + std::to_string(position_) + ": " + reason);
    }

    void Space() { while (position_ < input_.size() && std::isspace(static_cast<unsigned char>(input_[position_]))) ++position_; }
    bool Take(char expected) {
        Space();
        if (position_ < input_.size() && input_[position_] == expected) { ++position_; return true; }
        return false;
    }

    Value ParseValue(std::size_t depth) {
        if (depth > 128) Fail("nesting exceeds the safety limit");
        Space();
        if (position_ >= input_.size()) Fail("expected value");
        char c = input_[position_];
        if (c == '"') return Value::String(ParseString());
        if (c == '{') return ParseObject(depth + 1);
        if (c == '[') return ParseArray(depth + 1);
        if (c == 't' && input_.compare(position_, 4, "true") == 0) { position_ += 4; return Value::Boolean(true); }
        if (c == 'f' && input_.compare(position_, 5, "false") == 0) { position_ += 5; return Value::Boolean(false); }
        if (c == 'n' && input_.compare(position_, 4, "null") == 0) { position_ += 4; return {}; }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return ParseNumber();
        Fail("unexpected character");
    }

    std::string ParseString() {
        if (!Take('"')) Fail("expected string");
        std::string output;
        while (position_ < input_.size()) {
            char c = input_[position_++];
            if (c == '"') return output;
            if (static_cast<unsigned char>(c) < 0x20) Fail("unescaped control character in string");
            if (c != '\\') { output += c; continue; }
            if (position_ >= input_.size()) Fail("unfinished escape");
            c = input_[position_++];
            switch (c) {
                case '"': output += '"'; break;
                case '\\': output += '\\'; break;
                case '/': output += '/'; break;
                case 'b': output += '\b'; break;
                case 'f': output += '\f'; break;
                case 'n': output += '\n'; break;
                case 'r': output += '\r'; break;
                case 't': output += '\t'; break;
                case 'u': {
                    auto hex4 = [&]() {
                        if (position_ + 4 > input_.size()) Fail("short unicode escape");
                        unsigned value = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = input_[position_++];
                            value *= 16;
                            if (h >= '0' && h <= '9') value += h - '0';
                            else if (h >= 'a' && h <= 'f') value += h - 'a' + 10;
                            else if (h >= 'A' && h <= 'F') value += h - 'A' + 10;
                            else Fail("invalid unicode escape");
                        }
                        return value;
                    };
                    unsigned code = hex4();
                    if (code >= 0xd800 && code <= 0xdbff) {
                        if (position_ + 2 > input_.size() || input_[position_] != '\\' || input_[position_ + 1] != 'u')
                            Fail("missing low surrogate");
                        position_ += 2;
                        unsigned low = hex4();
                        if (low < 0xdc00 || low > 0xdfff) Fail("invalid low surrogate");
                        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    } else if (code >= 0xdc00 && code <= 0xdfff) {
                        Fail("unexpected low surrogate");
                    }
                    if (code <= 0x7f) output += static_cast<char>(code);
                    else if (code <= 0x7ff) { output += static_cast<char>(0xc0 | (code >> 6)); output += static_cast<char>(0x80 | (code & 0x3f)); }
                    else if (code <= 0xffff) { output += static_cast<char>(0xe0 | (code >> 12)); output += static_cast<char>(0x80 | ((code >> 6) & 0x3f)); output += static_cast<char>(0x80 | (code & 0x3f)); }
                    else { output += static_cast<char>(0xf0 | (code >> 18)); output += static_cast<char>(0x80 | ((code >> 12) & 0x3f)); output += static_cast<char>(0x80 | ((code >> 6) & 0x3f)); output += static_cast<char>(0x80 | (code & 0x3f)); }
                    break;
                }
                default: Fail("invalid escape");
            }
        }
        Fail("unterminated string");
    }

    Value ParseNumber() {
        Space();
        const char* start = input_.c_str() + position_;
        char* end = nullptr;
        double number = std::strtod(start, &end);
        if (end == start) Fail("invalid number");
        if (!std::isfinite(number)) Fail("number is outside the supported range");
        position_ += static_cast<std::size_t>(end - start);
        return Value::Number(number);
    }

    Value ParseArray(std::size_t depth) {
        Take('[');
        std::vector<Value> values;
        if (Take(']')) return Value::Array(std::move(values));
        do { values.push_back(ParseValue(depth)); } while (Take(','));
        if (!Take(']')) Fail("expected ]");
        return Value::Array(std::move(values));
    }

    Value ParseObject(std::size_t depth) {
        Take('{');
        std::map<std::string, Value> values;
        if (Take('}')) return Value::Object(std::move(values));
        do {
            Space();
            if (position_ >= input_.size() || input_[position_] != '"') Fail("expected object key");
            std::string key = ParseString();
            if (!Take(':')) Fail("expected colon");
            values.emplace(std::move(key), ParseValue(depth));
        } while (Take(','));
        if (!Take('}')) Fail("expected }");
        return Value::Object(std::move(values));
    }

    const std::string& input_;
    std::size_t position_ = 0;
};

Value Parse(const std::string& input) { return Parser(input).Run(); }

std::string Escape(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
                else out << c;
        }
    }
    out << '"';
    return out.str();
}

std::string Encode(const Value& value) {
    std::ostringstream out;
    switch (value.type) {
        case Value::Type::Null: return "null";
        case Value::Type::Boolean: return value.boolean ? "true" : "false";
        case Value::Type::Number:
            if (std::floor(value.number) == value.number) out << static_cast<long long>(value.number);
            else out << std::setprecision(15) << value.number;
            return out.str();
        case Value::Type::String: return Escape(value.string);
        case Value::Type::Array:
            out << '[';
            for (std::size_t i = 0; i < value.array.size(); ++i) { if (i) out << ','; out << Encode(value.array[i]); }
            out << ']'; return out.str();
        case Value::Type::Object:
            out << '{';
            { bool first = true; for (const auto& [key, item] : value.object) { if (!first) out << ','; first = false; out << Escape(key) << ':' << Encode(item); } }
            out << '}'; return out.str();
    }
    return "null";
}

}  // namespace kairo::json
