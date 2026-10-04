#include "utils/services_env.h"

#include <fstream>

namespace utils {

namespace {

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

std::string trim(const std::string& s) {
    std::size_t b = 0;
    std::size_t e = s.size();
    while (b < e && is_space(s[b])) ++b;
    while (e > b && is_space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

}  // namespace

std::string env_value_from_stream(std::istream& in, const std::string& key) {
    const std::string prefix = key + "=";
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        std::string v = trim(line.substr(prefix.size()));
        if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') &&
            v.back() == v.front()) {
            v = v.substr(1, v.size() - 2);
        }
        return v;
    }
    return "";
}

std::string read_env_value(const std::string& path, const std::string& key) {
    std::ifstream f(path);
    if (!f) return "";
    return env_value_from_stream(f, key);
}

}  // namespace utils
