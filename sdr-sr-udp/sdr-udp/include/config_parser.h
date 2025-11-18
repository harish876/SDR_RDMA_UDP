#pragma once

#include <string>
#include <map>
#include <cstdint>

namespace sdr {

class ConfigParser {
public:
    ConfigParser();
    ~ConfigParser();
    
    bool load_from_file(const std::string& filepath);
    
    uint32_t get_uint32(const std::string& key, uint32_t default_value = 0) const;
    
    std::string get_string(const std::string& key, const std::string& default_value = "") const;
    
    bool has_key(const std::string& key) const;
    
    void print_all() const;

private:
    std::map<std::string, std::string> config_map_;
    
    std::string trim(const std::string& str) const;
    
    bool parse_line(const std::string& line);
};

} // namespace sdr

