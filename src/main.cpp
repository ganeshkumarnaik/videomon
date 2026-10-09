#include <iostream>
#include <filesystem>
#include <string>
#include <cctype>
#include <chrono>
#include <thread>
#include <fstream>
#include <csignal>
#include <future>
#include <curl/curl.h>
#include <toml.hpp>
#include <utility>

/* Global variables */
volatile std::sig_atomic_t checkConfigFile;

/* Signal Handlers */
void sigHandler(int sig)
{
    if(sig == SIGHUP)
        checkConfigFile = true;
}

class videoMon{
public:
    std::filesystem::path configFilePath;
    std::future<void> telegramFuture; //removes warning. keeps async op alive. new alert waits

    enum class DeviceStatus{
        IDLE,
        ACTIVE
    };

    struct MonitoredDevices{
        std::filesystem::path configured_path;
        std::filesystem::path canonical_path;
        bool resolution_status = false;

        DeviceStatus current_device_status{DeviceStatus::IDLE};
        DeviceStatus previous_device_status{DeviceStatus::IDLE};
        int current_pid = -1;
        std::string current_process_name;

        std::chrono::steady_clock::time_point previous_alert = std::chrono::steady_clock::time_point::min();

        std::string updateState(int cooldown_seconds)
        {
            std::string msg;
            if(current_device_status != previous_device_status)
            {
                if(current_device_status == DeviceStatus::ACTIVE)
                {
                    std::cout << "INFO: Process " << current_process_name << " with PID " << current_pid << " has " << canonical_path << " open" << std::endl;
                    msg = "Camera active with " + current_process_name + " accessing " + canonical_path.string();
                    previous_alert = std::chrono::steady_clock::now();
                }
                else
                {
                    std::cout << "INFO: Stream ended for " << canonical_path << std::endl;
                    msg = "Camera stream ended for " + canonical_path.string();
                    previous_alert = std::chrono::steady_clock::time_point::min();
                }
                previous_device_status = current_device_status;
            }
            else
            {
                if(current_device_status == DeviceStatus::ACTIVE)
                {
                    auto now = std::chrono::steady_clock::now();
                    if(now - previous_alert >= std::chrono::seconds(cooldown_seconds))
                    {
                        msg = "Camera active with process " + current_process_name;
                        previous_alert = now;
                    }
                }
                else
                {
                    previous_alert = std::chrono::steady_clock::time_point::min();
                }
            }
            return msg;
        }

    };

    struct AppConfig{
        int cooldown_seconds;
        int check_interval_seconds;
        std::string bot_token;
        std::string chat_id;
        std::vector<MonitoredDevices> monitored_devices{};
    } cfg;

    /* Constructor */
    videoMon()
    {
        checkConfigFile = false;
        configFilePath = VIDEOMON_CONFIG_PATH;
        std::cout << "INFO: Config file is at path: " << configFilePath << std::endl;
    }

    std::string trim_white_spaces(const std::string& str)
    {
        size_t first = str.find_first_not_of(' ');
        if(first == std::string::npos)
            return str;

        size_t last = str.find_last_not_of(' ');
        return str.substr(first, (last-first+1));
    }

    bool parseConfigFile(std::filesystem::path& path, AppConfig& cfg)
    {
        //Parse TOML
        toml::table tbl;
        std::string filePath = path.generic_string();
        
        try
        {
            tbl = toml::parse_file(filePath);
            
            cfg.cooldown_seconds       = tbl["cooldown_seconds"].value_or(1);
            cfg.check_interval_seconds = tbl["check_interval_seconds"].value_or(60);
            cfg.bot_token              = tbl["telegram"]["bot_token"].value_or("");
            cfg.chat_id                = tbl["telegram"]["chat_id"].value_or("");

            /* Iterate over the monitored_devices array */
            if(auto* mon_dev = tbl["devices"]["monitor"].as_array())
            {
                cfg.monitored_devices.clear();

                for(const auto& dev : *mon_dev)
                {
                    if(auto dev_entry = dev.value<std::string>())
                    {
                        /* Create MonitoredDevices objects */
                        MonitoredDevices md;
                        md.configured_path = *dev_entry;

                        std::error_code ec;
                        std::filesystem::path canonical_path = std::filesystem::canonical(*dev_entry, ec);
                        if(ec)
                        {
                            /* Path is not valid */
                            continue;
                        }
                        else
                        {
                            md.canonical_path = canonical_path;
                            md.resolution_status = true;
                            cfg.monitored_devices.push_back(std::move(md));
                        }
                    }
                }
            }

            return true;
        }
        catch (const toml::parse_error& err)
        {
            std::cerr << "ERROR: Parsing failed; Sticking to default values\n" << err << "\n";
            return false;
        }
    }

    static size_t curl_handle_response(void* ptr, size_t size, size_t nmemb, void* userdata)
    {
        (void)ptr;
        (void)userdata;
        return size * nmemb;
    }

    void send_telegram_alert_native(const std::string& msg, const std::string& bot_token, const std::string& chat_id)
    {
        CURL *curl = curl_easy_init();
        if(!curl)
            return;

        if(!bot_token.empty() && !chat_id.empty())
        {
            /* Structure the URL */
            char* encoded_msg = curl_easy_escape(curl, msg.c_str(), msg.length()); //encodes spaces and such
            if(encoded_msg == NULL)
            {
                curl_easy_cleanup(curl);
                return;
            }
            std::string url = "https://api.telegram.org/bot" + bot_token + "/sendMessage?chat_id=" + chat_id + "&text=" + encoded_msg;
            curl_free(encoded_msg);

            /* Configure options */
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_handle_response);

            CURLcode res = curl_easy_perform(curl);
            if(res != CURLE_OK)
                std::cerr << "ERROR: Telegram request failed: " << curl_easy_strerror(res) << '\n';

        }
        curl_easy_cleanup(curl);

    }

    void trigger_async_telegram_alert(std::string msg)
    {
        /* Kepp a copy of cfg to avoid racing with config file reload triggered by SIGHUP */
        std::string bot_token = cfg.bot_token;
        std::string chat_id   = cfg.chat_id;
        telegramFuture = std::async(std::launch::async, [this, msg, bot_token, chat_id]()
        {
            send_telegram_alert_native(msg, bot_token, chat_id);
        });
    }

    std::string getProcessName(int pid)
    {
        std::string path = "/proc/" + std::to_string(pid) + "/comm";
        std::string procName;
        std::ifstream f(path); //open in read mode
        if(!f.is_open())
            return "";
        
        if(!std::getline(f, procName))
            return "";

        f.close();

        return procName;
    }

    void printPid()
    {
        std::cout << "INFO: PID : " << getpid() << std::endl;
    }

    void scan(std::vector<MonitoredDevices>& device_list)
    {
        const std::string path = "/proc/";
        std::string procName;

        /* Set devies to IDLE state before scan */
        for(auto & device : device_list)
        {
            device.current_device_status = DeviceStatus::IDLE;
            device.current_pid = -1;
            device.current_process_name.clear();
        }

        for(const auto& entry : std::filesystem::directory_iterator(path))
        {
            std::string subpath = entry.path();

            /* Check if subpath is a directory and if it doesnt start with a number, skip it */
            std::error_code ec;
            if(!std::filesystem::is_directory(subpath, ec) || 
            !std::filesystem::exists(subpath + "/fd", ec) ||
            !std::isdigit(subpath.substr(6,subpath.size()-1).front()))
            {
                continue; //skip this subpath
            }

            try
            {
                for(const auto& subentry : std::filesystem::directory_iterator(subpath + "/fd"))
                {
                    std::error_code ec;
                    std::filesystem::path canonical_path = std::filesystem::canonical(subentry.path(), ec);
                    if(ec)
                    {
                        continue;
                    }
                    else
                    {
                        for(auto& device : device_list)
                        {
                            if(canonical_path == device.canonical_path)
                            {
                                device.current_device_status = DeviceStatus::ACTIVE;
                                device.current_pid = std::stoi(subpath.substr(6,subpath.size()-1));
                                procName = getProcessName(device.current_pid);
                                if(!procName.empty())
                                {
                                    device.current_process_name = procName;
                                }
                                break; // continue to scan other fd inside the proc
                            }
                        }
                    }
                }
            }
            catch(const std::exception& e)
            {
                continue; //skip this subpath if conversion fails
            }
        }
    }

};


int main()
{
    /* Instantiate */
    videoMon vidMon;
    vidMon.printPid();
    vidMon.parseConfigFile(vidMon.configFilePath, vidMon.cfg);
    vidMon.trigger_async_telegram_alert("Starting Videomon service with PID " + std::to_string(getpid()));

    //Register signal handler
    signal(SIGHUP, sigHandler);
    
    while(true)
    {
        if(checkConfigFile)
        {
            std::cout << "INFO: Reload config file.\n";
            bool retVal = vidMon.parseConfigFile(vidMon.configFilePath, vidMon.cfg);
            if(retVal)
            {
                checkConfigFile = false;
                std::cout << "INFO: Parsing config file successful.\n";
            }
            else
            {
                /* Fallback to default values, read config again next loop */
                vidMon.cfg.cooldown_seconds = 60;
                vidMon.cfg.check_interval_seconds = 1;
                std::cout << "WARN: Parsing failed\n";
            }
        }

        std::this_thread::sleep_for(std::chrono::seconds(vidMon.cfg.check_interval_seconds));

        if(vidMon.cfg.monitored_devices.empty())
        {
            continue;
        }

        auto& device_list = vidMon.cfg.monitored_devices;

        vidMon.scan(device_list);

        std::string alert_concat;
        for(auto& device : device_list)
        {
            std::string alert = device.updateState(vidMon.cfg.cooldown_seconds);
            if(!alert.empty())
            {
                alert_concat = alert_concat + alert + "\n";
            }
        }
        if(!alert_concat.empty())
        {
            vidMon.trigger_async_telegram_alert(alert_concat);
        }
    }
    return 0;
}
