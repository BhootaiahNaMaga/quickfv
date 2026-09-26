// Minimal streaming JSON writer for the CLI's machine-readable output.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace qfv {

class JsonWriter {
public:
    JsonWriter& beginObject();
    JsonWriter& endObject();
    JsonWriter& beginArray();
    JsonWriter& endArray();
    JsonWriter& key(std::string_view k);
    JsonWriter& value(std::string_view v);
    JsonWriter& value(const char* v) { return value(std::string_view(v)); }
    JsonWriter& value(int64_t v);
    JsonWriter& value(uint64_t v);
    JsonWriter& value(int v) { return value(int64_t(v)); }
    JsonWriter& value(double v);
    JsonWriter& value(bool v);
    JsonWriter& null();

    template<typename T>
    JsonWriter& field(std::string_view k, const T& v) {
        key(k);
        return value(v);
    }

    const std::string& str() const { return out; }

private:
    void separator();

    std::string out;
    std::vector<bool> firstInScope; // per open object/array: no element emitted yet
    bool afterKey = false;
};

std::string jsonEscape(std::string_view s);

} // namespace qfv
