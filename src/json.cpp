#include "json.h"

#include <cstdio>

namespace qfv {

std::string jsonEscape(std::string_view s) {
    std::string r;
    r.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"': r += "\\\""; break;
            case '\\': r += "\\\\"; break;
            case '\n': r += "\\n"; break;
            case '\r': r += "\\r"; break;
            case '\t': r += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    r += buf;
                }
                else {
                    r += c;
                }
        }
    }
    return r;
}

void JsonWriter::separator() {
    if (afterKey) {
        afterKey = false;
        return;
    }
    if (!firstInScope.empty()) {
        if (!firstInScope.back())
            out += ',';
        firstInScope.back() = false;
    }
}

JsonWriter& JsonWriter::beginObject() {
    separator();
    out += '{';
    firstInScope.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::endObject() {
    out += '}';
    firstInScope.pop_back();
    return *this;
}

JsonWriter& JsonWriter::beginArray() {
    separator();
    out += '[';
    firstInScope.push_back(true);
    return *this;
}

JsonWriter& JsonWriter::endArray() {
    out += ']';
    firstInScope.pop_back();
    return *this;
}

JsonWriter& JsonWriter::key(std::string_view k) {
    separator();
    out += '"';
    out += jsonEscape(k);
    out += "\":";
    afterKey = true;
    return *this;
}

JsonWriter& JsonWriter::value(std::string_view v) {
    separator();
    out += '"';
    out += jsonEscape(v);
    out += '"';
    return *this;
}

JsonWriter& JsonWriter::value(int64_t v) {
    separator();
    out += std::to_string(v);
    return *this;
}

JsonWriter& JsonWriter::value(uint64_t v) {
    separator();
    out += std::to_string(v);
    return *this;
}

JsonWriter& JsonWriter::value(double v) {
    separator();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    out += buf;
    return *this;
}

JsonWriter& JsonWriter::value(bool v) {
    separator();
    out += v ? "true" : "false";
    return *this;
}

JsonWriter& JsonWriter::null() {
    separator();
    out += "null";
    return *this;
}

} // namespace qfv
