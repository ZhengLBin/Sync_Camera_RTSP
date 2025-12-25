#include "../includes/config_manager.h"
#include <fstream>
#include <sstream>
#include <iostream>

ConfigManager& ConfigManager::instance() {
    static ConfigManager instance;
    return instance;
}

bool ConfigManager::load_config(const std::string& config_file) {
    try {
        std::ifstream file(config_file);
        if (!file.is_open()) {
            std::cerr << "Failed to open config file: " << config_file << std::endl;
            return false;
        }

        configs_.clear();
        std::string line;
        
        while (std::getline(file, line)) {
            // 跳过空行和注释行
            if (line.empty() || line[0] == '#') {
                continue;
            }
            
            std::istringstream iss(line);
            std::string name, host;
            int port;
            
            if (iss >> name >> host >> port) {
                StreamerConfig config;
                config.name = name;
                config.host = host;
                config.port = port;
                
                configs_.push_back(config);
                std::cout << "Loaded streamer config: " << name 
                          << " -> " << host << ":" << port << std::endl;
            }
        }
        
        file.close();
        return !configs_.empty();
    }
    catch (const std::exception& e) {
        std::cerr << "Error loading config: " << e.what() << std::endl;
        return false;
    }
}

StreamerConfig* ConfigManager::get_config(const std::string& name) {
    for (auto& config : configs_) {
        if (config.name == name) {
            return &config;
        }
    }
    return nullptr;
}

void ConfigManager::generate_default_config(const std::string& config_file) {
    try {
        std::ofstream file(config_file);
        file << "# Streamer Configuration\n";
        file << "# Format: name host port\n";
        file << "# Example:\n";
        file << "# front 127.0.0.1 5010\n";
        file << "\n";
        file << "front 127.0.0.1 5010\n";
        file << "back 127.0.0.1 5011\n";
        file.close();
        std::cout << "Generated default config: " << config_file << std::endl;
    }
    catch (const std::exception& e) {
        std::cerr << "Error generating default config: " << e.what() << std::endl;
    }
}

