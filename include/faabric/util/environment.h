#pragma once

#include <string>

namespace faabric::util {
std::string getEnvVar(const std::string& key, const std::string& deflt);

std::string setEnvVar(const std::string& varName, const std::string& value);

void unsetEnvVar(const std::string& varName);

unsigned int getUsableCores();
}
