#include "util.h"

#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <sstream>
#include <thread>

namespace recmeet {

static fs::path xdg_dir(const char* env_var, const char* fallback_suffix) {
    if (const char* val = std::getenv(env_var); val && val[0] != '\0')
        return fs::path(val) / "recmeet";
    if (const char* home = std::getenv("HOME"))
        return fs::path(home) / fallback_suffix / "recmeet";
    return fs::path(".") / fallback_suffix / "recmeet";
}

fs::path config_dir() { return xdg_dir("XDG_CONFIG_HOME", ".config"); }
fs::path data_dir()   { return xdg_dir("XDG_DATA_HOME", ".local/share"); }
fs::path models_dir() { return data_dir() / "models"; }

fs::path create_output_dir(const fs::path& base_dir) {
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d_%H-%M");
    std::string timestamp = oss.str();

    fs::path dir = base_dir / timestamp;
    if (!fs::exists(dir)) {
        fs::create_directories(dir);
        return dir;
    }

    // Handle collision
    for (int i = 2; i < 100; ++i) {
        fs::path candidate = base_dir / (timestamp + "_" + std::to_string(i));
        if (!fs::exists(candidate)) {
            fs::create_directories(candidate);
            return candidate;
        }
    }
    throw RecmeetError("Too many sessions in the same minute.");
}

int default_thread_count() {
    unsigned n = std::thread::hardware_concurrency();
    return (n > 1) ? static_cast<int>(n - 1) : 1;
}

} // namespace recmeet
