#include <puzzpool/env.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <string>

#ifdef _WIN32
#include <stdlib.h>
#else
#include <unistd.h>
#endif

namespace puzzpool {

std::string ltrim(std::string s) {
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
        return !std::isspace(ch);
    }));
    return s;
}

std::string rtrim(std::string s) {
    s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
        return !std::isspace(ch);
    }).base(), s.end());
    return s;
}

std::string trim(std::string s) {
    return rtrim(ltrim(std::move(s)));
}

void setEnvVar(const std::string& key, const std::string& value, bool overwrite) {
#ifdef _WIN32
    if (overwrite || std::getenv(key.c_str()) == nullptr) {
        _putenv_s(key.c_str(), value.c_str());
    }
#else
    if (overwrite || std::getenv(key.c_str()) == nullptr) {
        setenv(key.c_str(), value.c_str(), overwrite ? 1 : 0);
    }
#endif
}

static void parseDotEnvLine(const std::string& rawLine,
                             const std::function<void(const std::string&, const std::string&)>& cb) {
    std::string line = trim(rawLine);
    if (line.empty() || line[0] == '#') return;
    if (line.rfind("export ", 0) == 0) line = trim(line.substr(7));
    const auto eqPos = line.find('=');
    if (eqPos == std::string::npos) return;
    std::string key   = trim(line.substr(0, eqPos));
    std::string value = trim(line.substr(eqPos + 1));
    if (key.empty()) return;
    if (value.size() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
    }
    cb(key, value);
}

void loadDotEnv(const std::string& path, bool overwrite) {
    std::ifstream in(path);
    if (!in.is_open()) return;
    std::string line;
    while (std::getline(in, line)) {
        parseDotEnvLine(line, [&](const std::string& k, const std::string& v) {
            setEnvVar(k, v, overwrite);
        });
    }
}

std::map<std::string, std::string> parseDotEnvFile(const std::string& path) {
    std::map<std::string, std::string> out;
    std::ifstream in(path);
    if (!in.is_open()) return out;
    std::string line;
    while (std::getline(in, line)) {
        parseDotEnvLine(line, [&](const std::string& k, const std::string& v) {
            out[k] = v;
        });
    }
    return out;
}

std::string getEnvOr(const char* key, const std::string& fallback) {
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : fallback;
}

int getEnvInt(const char* key, int fallback) {
    const char* v = std::getenv(key);
    if (!v || !*v) return fallback;
    try { return std::stoi(v); } catch (...) { return fallback; }
}

double getEnvDouble(const char* key, double fallback) {
    const char* v = std::getenv(key);
    if (!v || !*v) return fallback;
    try { return std::stod(v); } catch (...) { return fallback; }
}

bool getEnvBool01(const char* key, bool fallback) {
    const char* v = std::getenv(key);
    if (!v || !*v) return fallback;
    return std::string(v) == "1";
}

boost::multiprecision::cpp_int getEnvBigInt(const char* key,
                                             const boost::multiprecision::cpp_int& fallback) {
    const char* v = std::getenv(key);
    if (!v || !*v) return fallback;
    try { return boost::multiprecision::cpp_int(std::string(v)); } catch (...) { return fallback; }
}

} // namespace puzzpool
