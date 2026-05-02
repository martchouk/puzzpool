#pragma once

#include <boost/multiprecision/cpp_int.hpp>

#include <map>
#include <string>

namespace puzzpool {

std::string ltrim(std::string s);
std::string rtrim(std::string s);
std::string trim(std::string s);

void setEnvVar(const std::string& key, const std::string& value, bool overwrite = false);
void loadDotEnv(const std::string& path = ".env", bool overwrite = false);
std::map<std::string, std::string> parseDotEnvFile(const std::string& path = ".env");
std::map<std::string, std::string> processEnvMap();

std::string getEnvOr(const char* key, const std::string& fallback);
int         getEnvInt(const char* key, int fallback);
double      getEnvDouble(const char* key, double fallback);
bool        getEnvBool01(const char* key, bool fallback = false);
boost::multiprecision::cpp_int getEnvBigInt(const char* key,
                                             const boost::multiprecision::cpp_int& fallback);

} // namespace puzzpool
