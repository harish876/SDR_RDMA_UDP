#pragma once

#include <string>
#include <map>
#include <cstdint>

namespace sdr {

// Simple config file parser
class ConfigParser {
public:
    ConfigParser();
    ~ConfigParser();
    
    // Load config from file
    bool load_from_file(const std::string& filepath);
    
    // Get integer value (returns default if not found)
    uint32_t get_uint32(const std::string& key, uint32_t default_value = 0) const;
    
    // Get string value (returns default if not found)
    std::string get_string(const std::string& key, const std::string& default_value = "") const;
    
    // Check if key exists
    bool has_key(const std::string& key) const;
    
    // Print all config values (for debugging)
    void print_all() const;

private:
    std::map<std::string, std::string> config_map_;
    
    // Helper: trim whitespace
    std::string trim(const std::string& str) const;
    
    // Helper: parse line
    bool parse_line(const std::string& line);
};

} // namespace sdr

