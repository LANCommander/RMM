#include "util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace rmm {

std::string Trim(const std::string& s) {
    size_t b = 0, e = s.size();

    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;

    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;

    return s.substr(b, e - b);
}

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    return s;
}

bool IEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;

    return true;
}

bool IContains(const std::string& haystack, const std::string& needle) {
    return ToLower(haystack).find(ToLower(needle)) != std::string::npos;
}

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

namespace {
// Iterative glob with single-star backtracking; both inputs lower-case.
bool GlobOne(const std::string& p, const std::string& t) {
    size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;

    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == t[ti])) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            mark = ti;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ti = ++mark;
        } else
            return false;
    }

    while (pi < p.size() && p[pi] == '*')
        ++pi;

    return pi == p.size();
}
}  // namespace

bool GlobMatch(const std::string& patterns, const std::string& text) {
    std::string all = Trim(patterns);

    if (all.empty())
        return true;

    std::string t = ToLower(text);
    size_t start = 0;

    while (start <= all.size()) {
        size_t comma = all.find(',', start);
        std::string one =
            ToLower(Trim(all.substr(start, comma == std::string::npos ? std::string::npos : comma - start)));

        if (!one.empty() && GlobOne(one, t))
            return true;

        if (comma == std::string::npos)
            break;

        start = comma + 1;
    }

    return false;
}

std::string Join(const std::vector<std::string>& v, size_t first, const char* sep) {
    std::string out;

    for (size_t i = first; i < v.size(); ++i) {
        if (i > first)
            out += sep;

        out += v[i];
    }

    return out;
}

std::string VFormat(const char* fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int n = std::vsnprintf(nullptr, 0, fmt, ap2);
    va_end(ap2);

    if (n <= 0)
        return {};

    std::string s(static_cast<size_t>(n), '\0');
    std::vsnprintf(&s[0], s.size() + 1, fmt, ap);

    return s;
}

std::string Format(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::string s = VFormat(fmt, ap);
    va_end(ap);

    return s;
}

std::vector<std::string> SplitArgs(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0, n = line.size();

    while (i < n) {
        while (i < n && std::isspace(static_cast<unsigned char>(line[i])))
            ++i;

        if (i >= n)
            break;

        std::string tok;

        if (line[i] == '"') {
            ++i;

            while (i < n && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < n && line[i + 1] == '"') {
                    tok += '"';
                    i += 2;
                    continue;
                }

                tok += line[i++];
            }

            if (i < n)
                ++i;  // closing quote
        } else {
            while (i < n && !std::isspace(static_cast<unsigned char>(line[i])))
                tok += line[i++];
        }

        out.push_back(std::move(tok));
    }

    return out;
}

std::string QuoteArg(const std::string& v) {
    std::string out = "\"";

    for (char c : v) {
        if (c == '"')
            out += '\'';  // Refractor console has no escape for quotes
        else if (c == '\n' || c == '\r')
            out += ' ';
        else
            out += c;
    }

    out += '"';

    return out;
}

size_t CopyOut(const std::string& src, char* out, size_t cap) {
    if (out && cap > 0) {
        size_t n = std::min(src.size(), cap - 1);
        std::memcpy(out, src.data(), n);
        out[n] = '\0';
    }

    return src.size();
}

bool ParseDouble(const std::string& s, double& out) {
    std::string t = Trim(s);

    if (t.empty())
        return false;

    char* end = nullptr;
    out = std::strtod(t.c_str(), &end);

    return end && *end == '\0';
}

bool Ini::Load(const std::string& path) {
    std::ifstream f(path);

    if (!f)
        return false;

    sections_.clear();
    std::string line, section;

    while (std::getline(f, line)) {
        line = Trim(line);

        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;

        if (line.front() == '[' && line.back() == ']') {
            section = ToLower(Trim(line.substr(1, line.size() - 2)));
            sections_[section];
            continue;
        }

        size_t eq = line.find('=');

        if (eq == std::string::npos)
            continue;

        std::string key = ToLower(Trim(line.substr(0, eq)));
        std::string val = Trim(line.substr(eq + 1));

        // strip trailing inline comment " ; ..." when not quoted
        if (!val.empty() && val.front() != '"') {
            size_t c = val.find(" ;");

            if (c != std::string::npos)
                val = Trim(val.substr(0, c));
        }

        if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
            val = val.substr(1, val.size() - 2);

        Set(section, key, val);
    }

    return true;
}

bool Ini::Has(const std::string& section, const std::string& key) const {
    auto it = sections_.find(ToLower(section));

    if (it == sections_.end())
        return false;

    std::string k = ToLower(key);

    for (auto& kv : it->second)
        if (kv.first == k)
            return true;

    return false;
}

std::string Ini::Get(const std::string& section, const std::string& key, const std::string& def) const {
    auto it = sections_.find(ToLower(section));

    if (it == sections_.end())
        return def;

    std::string k = ToLower(key);

    for (auto& kv : it->second)
        if (kv.first == k)
            return kv.second;

    return def;
}

const std::vector<std::pair<std::string, std::string>>& Ini::Section(const std::string& section) const {
    static const std::vector<std::pair<std::string, std::string>> empty;
    auto it = sections_.find(ToLower(section));

    return it == sections_.end() ? empty : it->second;
}

void Ini::Set(const std::string& section, const std::string& key, const std::string& value) {
    auto& vec = sections_[ToLower(section)];
    std::string k = ToLower(key);

    for (auto& kv : vec)
        if (kv.first == k) {
            kv.second = value;

            return;
        }

    vec.emplace_back(k, value);
}

std::string Substitute(const std::string& tmpl, const std::map<std::string, std::string>& vars) {
    std::string out;
    out.reserve(tmpl.size() + 32);

    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] == '{') {
            size_t close = tmpl.find('}', i);

            if (close != std::string::npos) {
                auto it = vars.find(tmpl.substr(i + 1, close - i - 1));

                if (it != vars.end()) {
                    out += it->second;
                    i = close;
                    continue;
                }
            }
        }

        out += tmpl[i];
    }

    return out;
}

}  // namespace rmm
