// A small JSON reader and the one helper needed to write it, for the TowerTrainer link (Tower mode).
// Objects, arrays, strings (with \uXXXX escapes), numbers, true/false/null; a depth limit; no exceptions.
// Nothing here is clever: the messages are a few kilobytes of plain state a few times a second.
#pragma once

#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace xr {
namespace json {

struct Value {
    enum Type { Null, Bool, Number, String, Array, Object };
    Type        type = Null;
    bool        b = false;
    double      num = 0.0;
    std::string str;
    std::vector<Value> arr;
    std::vector<std::pair<std::string, Value>> obj;

    bool isNull()   const { return type == Null; }
    bool isObject() const { return type == Object; }
    bool isArray()  const { return type == Array; }
    bool isString() const { return type == String; }
    bool isNumber() const { return type == Number; }

    // Object member, or nullptr.
    const Value* at(const char* key) const {
        if (type != Object) return nullptr;
        for (const auto& kv : obj) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    const Value& operator[](size_t i) const {
        static const Value none;
        return (type == Array && i < arr.size()) ? arr[i] : none;
    }
    size_t size() const { return type == Array ? arr.size() : type == Object ? obj.size() : 0; }

    // Typed member access with defaults -- what every message handler wants.
    std::string s(const char* key, const std::string& def = "") const {
        const Value* v = at(key);
        return v && v->type == String ? v->str : def;
    }
    double n(const char* key, double def = 0.0) const {
        const Value* v = at(key);
        if (!v) return def;
        if (v->type == Number) return v->num;
        if (v->type == Bool) return v->b ? 1.0 : 0.0;
        return def;
    }
    bool truthy(const char* key) const {
        const Value* v = at(key);
        if (!v) return false;
        switch (v->type) {
            case Bool:   return v->b;
            case Number: return v->num != 0.0;
            case String: return !v->str.empty();
            case Null:   return false;
            default:     return v->size() > 0;
        }
    }
    const Value& a(const char* key) const {      // array member or an empty value
        static const Value none;
        const Value* v = at(key);
        return v && v->type == Array ? *v : none;
    }
    const Value& o(const char* key) const {      // object member or an empty value
        static const Value none;
        const Value* v = at(key);
        return v && v->type == Object ? *v : none;
    }
};

namespace detail {

struct Parser {
    const char* p;
    const char* end;
    int depth = 0;
    std::string err;

    void ws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    bool fail(const char* what) { if (err.empty()) err = what; return false; }

    static void putUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out.push_back((char)cp);
        else if (cp < 0x800) { out.push_back((char)(0xC0 | (cp >> 6))); out.push_back((char)(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back((char)(0xE0 | (cp >> 12))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
        else { out.push_back((char)(0xF0 | (cp >> 18))); out.push_back((char)(0x80 | ((cp >> 12) & 0x3F))); out.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); out.push_back((char)(0x80 | (cp & 0x3F))); }
    }
    static int hex(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
    bool string(std::string& out) {
        if (p >= end || *p != '"') return fail("string expected");
        ++p;
        while (p < end) {
            const char c = *p++;
            if (c == '"') return true;
            if (c != '\\') { out.push_back(c); continue; }
            if (p >= end) return fail("bad escape");
            const char e = *p++;
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    if (end - p < 4) return fail("bad \\u escape");
                    unsigned cp = 0;
                    for (int i = 0; i < 4; ++i) { const int h = hex(p[i]); if (h < 0) return fail("bad \\u escape"); cp = cp * 16 + (unsigned)h; }
                    p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {   // surrogate pair
                        unsigned lo = 0; bool ok = true;
                        for (int i = 0; i < 4; ++i) { const int h = hex(p[2 + i]); if (h < 0) { ok = false; break; } lo = lo * 16 + (unsigned)h; }
                        if (ok && lo >= 0xDC00 && lo <= 0xDFFF) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
                    }
                    putUtf8(out, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool value(Value& v) {
        ws();
        if (p >= end) return fail("unexpected end");
        if (++depth > 64) return fail("too deep");
        bool ok = false;
        const char c = *p;
        if (c == '{') {
            v.type = Value::Object; ++p; ws();
            if (p < end && *p == '}') { ++p; ok = true; }
            else {
                for (;;) {
                    ws();
                    std::string key;
                    if (!string(key)) break;
                    ws();
                    if (p >= end || *p != ':') { fail("':' expected"); break; }
                    ++p;
                    Value child;
                    if (!value(child)) break;
                    v.obj.emplace_back(std::move(key), std::move(child));
                    ws();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == '}') { ++p; ok = true; }
                    else fail("',' or '}' expected");
                    break;
                }
            }
        } else if (c == '[') {
            v.type = Value::Array; ++p; ws();
            if (p < end && *p == ']') { ++p; ok = true; }
            else {
                for (;;) {
                    Value child;
                    if (!value(child)) break;
                    v.arr.push_back(std::move(child));
                    ws();
                    if (p < end && *p == ',') { ++p; continue; }
                    if (p < end && *p == ']') { ++p; ok = true; }
                    else fail("',' or ']' expected");
                    break;
                }
            }
        } else if (c == '"') {
            v.type = Value::String; ok = string(v.str);
        } else if (c == 't' && end - p >= 4 && strncmp(p, "true", 4) == 0) { v.type = Value::Bool; v.b = true; p += 4; ok = true; }
        else if (c == 'f' && end - p >= 5 && strncmp(p, "false", 5) == 0) { v.type = Value::Bool; v.b = false; p += 5; ok = true; }
        else if (c == 'n' && end - p >= 4 && strncmp(p, "null", 4) == 0) { v.type = Value::Null; p += 4; ok = true; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            char* q = nullptr;
            std::string tmp(p, (size_t)(end - p) > 64 ? 64 : (size_t)(end - p));
            v.type = Value::Number; v.num = strtod(tmp.c_str(), &q);
            if (q == tmp.c_str()) ok = fail("bad number");
            else { p += (q - tmp.c_str()); ok = true; }
        } else ok = fail("unexpected character");
        --depth;
        return ok;
    }
};

}  // namespace detail

// Parses `text` into `out`. Trailing whitespace is fine, trailing garbage is not.
inline bool parse(const std::string& text, Value* out, std::string* err = nullptr) {
    detail::Parser ps{text.data(), text.data() + text.size(), {}};
    Value v;
    bool ok = ps.value(v);
    if (ok) { ps.ws(); if (ps.p != ps.end) ok = ps.fail("trailing characters"); }
    if (!ok) { if (err) *err = ps.err.empty() ? "parse error" : ps.err; return false; }
    *out = std::move(v);
    return true;
}

// A JSON string literal (with the quotes) for building outgoing lines.
inline std::string quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
                else out.push_back((char)c);
        }
    }
    out += "\"";
    return out;
}

// Text with HTML tags removed and the common entities decoded -- the trainer's log and ATIS lines
// arrive as the HTML its own page shows.
inline std::string stripHtml(const std::string& h) {
    std::string out;
    bool tag = false;
    for (size_t i = 0; i < h.size(); ++i) {
        const char c = h[i];
        if (c == '<') { tag = true; continue; }
        if (c == '>') { tag = false; out.push_back(' '); continue; }
        if (tag) continue;
        if (c == '&') {
            if (h.compare(i, 5, "&amp;") == 0) { out.push_back('&'); i += 4; continue; }
            if (h.compare(i, 4, "&lt;") == 0) { out.push_back('<'); i += 3; continue; }
            if (h.compare(i, 4, "&gt;") == 0) { out.push_back('>'); i += 3; continue; }
            if (h.compare(i, 6, "&quot;") == 0) { out.push_back('"'); i += 5; continue; }
            if (h.compare(i, 6, "&nbsp;") == 0) { out.push_back(' '); i += 5; continue; }
        }
        out.push_back(c);
    }
    // collapse runs of blanks
    std::string t;
    bool blank = false;
    for (char c : out) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { if (!blank) t.push_back(' '); blank = true; }
        else { t.push_back(c); blank = false; }
    }
    while (!t.empty() && t.back() == ' ') t.pop_back();
    size_t a = 0;
    while (a < t.size() && t[a] == ' ') ++a;
    return t.substr(a);
}

}  // namespace json
}  // namespace xr
