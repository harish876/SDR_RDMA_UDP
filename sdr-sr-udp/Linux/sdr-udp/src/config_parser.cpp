#include "config_parser.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>

namespace sdr {

ConfigParser::ConfigParser() {
}

ConfigParser::~ConfigParser() {
}

std::string ConfigParser::trim(const std::string& str) const {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool ConfigParser::parse_line(const std::string& line) {
    std::string trimmed = trim(line);
    
    if (trimmed.empty() || trimmed[0] == '#' || trimmed[0] == ';') {
        return true;
    }
    
    size_t eq_pos = trimmed.find('=');
    if (eq_pos == std::string::npos) {
        return false; 
    }
    
    std::string key = trim(trimmed.substr(0, eq_pos));
    std::string value = trim(trimmed.substr(eq_pos + 1));
    
    if (key.empty()) {
        return false;
    }
    
    config_map_[key] = value;
    return true;
}

bool ConfigParser::load_from_file(const std::string& filepath) {
    config_map_.clear();
    
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "[Config] Failed to open config file: " << filepath << std::endl;
        return false;
    }
    
    std::string line;
    int line_num = 0;
    bool has_errors = false;
    
    while (std::getline(file, line)) {
        line_num++;
        if (!parse_line(line)) {
            std::cerr << "[Config] Warning: Invalid line " << line_num 
                      << " in config file: " << line << std::endl;
            has_errors = true;
        }
    }
    
    file.close();
    
    if (!has_errors) {
        std::cout << "[Config] Loaded " << config_map_.size() 
                  << " configuration entries from " << filepath << std::endl;
    }
    
    return true;
}

uint32_t ConfigParser::get_uint32(const std::string& key, uint32_t default_value) const {
    auto it = config_map_.find(key);
    if (it == config_map_.end()) {
        return default_value;
    }
    
    try {
        return static_cast<uint32_t>(std::stoul(it->second));
    } catch (const std::exception& e) {
        std::cerr << "[Config] Warning: Failed to parse " << key 
                  << " as integer: " << it->second << std::endl;
        return default_value;
    }
}

std::string ConfigParser::get_string(const std::string& key, const std::string& default_value) const {
    auto it = config_map_.find(key);
    if (it == config_map_.end()) {
        return default_value;
    }
    return it->second;
}

bool ConfigParser::has_key(const std::string& key) const {
    return config_map_.find(key) != config_map_.end();
}

void ConfigParser::print_all() const {
    std::cout << "[Config] Current configuration:" << std::endl;
    for (const auto& pair : config_map_) {
        std::cout << "  " << pair.first << " = " << pair.second << std::endl;
    }
}

} // namespace sdr

