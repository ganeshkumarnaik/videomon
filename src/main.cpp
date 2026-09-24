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

    struct AppConfig{
        int cooldown_seconds = 60;
        int check_interval_seconds = 1;
        std::string bot_token;
        std::string chat_id;
        std::vector<std::string> monitored_devices;
    } cfg;

    /* Constructor */
    videoMon()
    {
        checkConfigFile = false;
        configFilePath = VIDEOMON_CONFIG_PATH;
        std::cout << "Config file is at path: " << configFilePath << std::endl;
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
                        cfg.monitored_devices.push_back(*dev_entry);
                    }
                }
            }

            return true;
        }
        catch (const toml::parse_error& err)
        {
            std::cerr << "Parsing failed; Sticking to default values\n" << err << "\n";
            return false;
        }
    }

    static size_t curl_handle_response(void* ptr, size_t size, size_t nmemb, void* userdata)
    {
        (void)ptr;
        (void)userdata;
        return size * nmemb;
    }

    void send_telegram_alert_native(const std::string& msg)
    {
        CURL *curl = curl_easy_init();
        if(!curl)
            return;

        if(!cfg.bot_token.empty() && !cfg.chat_id.empty())
        {
            /* Structure the URL */
            char* encoded_msg = curl_easy_escape(curl, msg.c_str(), msg.length()); //encodes spaces and such
            if(encoded_msg == NULL)
            {
                curl_easy_cleanup(curl);
                return;
            }
            std::string url = "https://api.telegram.org/bot" + cfg.bot_token + "/sendMessage?chat_id=" + cfg.chat_id + "&text=" + encoded_msg;
            curl_free(encoded_msg);

            /* Configure options */
            curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_handle_response);

            CURLcode res = curl_easy_perform(curl);
            if(res != CURLE_OK)
                std::cerr << "Telegram request failed: " << curl_easy_strerror(res) << '\n';

        }
        curl_easy_cleanup(curl);

    }

    void trigger_async_telegram_alert(std::string msg)
    {
        telegramFuture = std::async(std::launch::async, [this, msg]()
        {
            send_telegram_alert_native(msg);
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
        std::cout << "PID : " << getpid() << std::endl;
    }
};


int main()
{
    const std::string path = "/proc/";
    const std::string inactive = "INACTIVE";
    const std::string active = "ACTIVE";
    std::string currentState = inactive;
    std::string previousState = inactive;
    int pid;
    std::string procName;
    auto previousAlert = std::chrono::steady_clock::time_point::min();

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
            vidMon.parseConfigFile(vidMon.configFilePath, vidMon.cfg);
            checkConfigFile = false;
            std::cout << "cooldown_seconds = " << vidMon.cfg.cooldown_seconds << std::endl;
            std::cout << "check_interval_seconds = " << vidMon.cfg.check_interval_seconds << std::endl;
        }

        std::this_thread::sleep_for(std::chrono::seconds(vidMon.cfg.check_interval_seconds));
        currentState = inactive;

        if(vidMon.cfg.monitored_devices.empty())
        {
            continue;
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
                pid = std::stoi(subpath.substr(6,subpath.size()-1));

                for(const auto& subentry : std::filesystem::directory_iterator(subpath + "/fd"))
                {
                    if(std::filesystem::is_symlink(subentry.path()) && 
                       std::filesystem::read_symlink(subentry.path()) == vidMon.cfg.monitored_devices[0])
                    {
                        procName = vidMon.getProcessName(pid);
                        if(!procName.empty())
                        {
                            currentState = active;
                            break;
                        }
                    }
                }

            }
            catch(const std::exception& e)
            {
                continue; //skip this subpath if conversion fails
            }
        }
        if(currentState != previousState)
        {
            if(currentState == active)
            {
                std::cout << "Process " << procName << " with PID " << pid << " has " << vidMon.cfg.monitored_devices[0] << " open" << std::endl;
                std::string msg = "Camera active with process " + procName;
                vidMon.trigger_async_telegram_alert(msg);
                previousAlert = std::chrono::steady_clock::now();
            }
            else
            {
                std::cout << "Stream ended." << std::endl;
                std::string msg = "Camera stream ended";
                vidMon.trigger_async_telegram_alert(msg);
                previousAlert = std::chrono::steady_clock::time_point::min();
            }
            previousState = currentState;
        }
        else
        {
            if(currentState == active)
            {
                auto now = std::chrono::steady_clock::now();
                if(now - previousAlert >= std::chrono::seconds(vidMon.cfg.cooldown_seconds))
                {
                    std::string msg = "Camera active with process " + procName;
                    vidMon.trigger_async_telegram_alert(msg);
                    previousAlert = now;
                }
            }
            else
            {
                previousAlert = std::chrono::steady_clock::time_point::min();
            }
        }
    }
    return 0;
}
