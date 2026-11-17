// Small string/INI helpers shared by the core and adapters.
#pragma once

#include <cstdarg>
#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace rmm {

std::string Trim(const std::string& s);
std::string ToLower(std::string s);
bool IEquals(const std::string& a, const std::string& b);
bool IContains(const std::string& haystack, const std::string& needle);
bool StartsWith(const std::string& s, const std::string& prefix);
/// Case-insensitive glob (`*`, `?`). Several patterns may be separated by
/// commas; empty matches everything.
bool GlobMatch(const std::string& patterns, const std::string& text);
std::string Join(const std::vector<std::string>& v, size_t first, const char* sep = " ");
std::string Format(const char* fmt, ...);
std::string VFormat(const char* fmt, va_list ap);

/// Split a console line Refractor-style: whitespace separated, "double quoted"
/// spans kept together (quotes removed).
std::vector<std::string> SplitArgs(const std::string& line);

/// Quote a value for the engine console. Embedded quotes become ' (the console
/// has no escape for them) and newlines become spaces.
std::string QuoteArg(const std::string& v);

/// Copy into a C buffer, always NUL terminating. Returns full source length.
size_t CopyOut(const std::string& src, char* out, size_t cap);

bool ParseDouble(const std::string& s, double& out);

/// Very small INI reader: [section] / key = value / ; and # comments.
/// Keys are case-insensitive (stored lower-case). Order of keys is preserved
/// per section.
class Ini {
public:
    bool Load(const std::string& path);
    bool Has(const std::string& section, const std::string& key) const;
    std::string Get(const std::string& section, const std::string& key, const std::string& def = {}) const;
    const std::vector<std::pair<std::string, std::string>>& Section(const std::string& section) const;
    void Set(const std::string& section, const std::string& key, const std::string& value);

private:
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> sections_;
};

/// Substitute {name} placeholders in a template.
std::string Substitute(const std::string& tmpl, const std::map<std::string, std::string>& vars);

}  // namespace rmm
