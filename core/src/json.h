#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace kairo::json {

struct Value {
    enum class Type { Null, Boolean, Number, String, Array, Object };
    Type type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Value> array;
    std::map<std::string, Value> object;

    static Value Boolean(bool value);
    static Value Number(double value);
    static Value String(std::string value);
    static Value Array(std::vector<Value> value = {});
    static Value Object(std::map<std::string, Value> value = {});
    const Value& At(const std::string& key) const;
    const Value* Find(const std::string& key) const;
    std::string GetString(const std::string& key, const std::string& fallback = "") const;
};

Value Parse(const std::string& input);
std::string Encode(const Value& value);
std::string Escape(const std::string& value);

}  // namespace kairo::json
