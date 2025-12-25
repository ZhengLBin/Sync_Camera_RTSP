#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <string>
#include <vector>

struct StreamerConfig {
    std::string name;
    std::string host;
    int port;
};

class ConfigManager {
public:
    static ConfigManager& instance();
    
    bool load_config(const std::string& config_file);
    const std::vector<StreamerConfig>& get_configs() const { return configs_; }
    StreamerConfig* get_config(const std::string& name);
    
    static void generate_default_config(const std::string& config_file);

private:
    ConfigManager() = default;
    std::vector<StreamerConfig> configs_;
    
    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
};

#endif // CONFIG_MANAGER_H
