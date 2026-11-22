#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace logx {

static std::mutex g_mutex;
static FILE* g_file = nullptr;
static std::string g_path;
static std::string g_prev_path;  // the previous session's log, kept at startup so a crash survives a relaunch
static auto g_start = std::chrono::steady_clock::now();

constexpr int kKeep = 30;

// the run number of prefix_NNN.log, 0 for another name
static int RunNumber(const fs::path& file, const std::string& prefix) {
    std::string name = file.filename().string();
    if (file.extension() != ".log" || name.size() <= prefix.size() + 1 || name.compare(0, prefix.size() + 1, prefix + "_"))
        return 0;
    return std::atoi(name.c_str() + prefix.size() + 1);
}

void Init(const std::string& dir, const std::string& prefix, bool append) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (!kVerbose) {
        g_path = (fs::path(dir) / (prefix + ".log")).string();
        if (!append) {
            // keep the previous session before truncating: the crash handler appends to this file, and a
            // tester who reopens the app to export it would otherwise find it truncated away. Best effort:
            // no previous log on first launch, or an unwritable dir, just means there is nothing to keep
            fs::path prev = fs::path(dir) / (prefix + "-prev.log");
            std::error_code rec;
            fs::remove(prev, rec);
            rec.clear();
            fs::rename(fs::path(g_path), prev, rec);
            if (!rec) g_prev_path = prev.string();
        }
        g_file = std::fopen(g_path.c_str(), append ? "a" : "w");
        return;
    }
    std::vector<std::pair<int, fs::path>> runs;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (int n = RunNumber(e.path(), prefix)) runs.emplace_back(n, e.path());
    std::sort(runs.begin(), runs.end());
    for (size_t i = 0; i + kKeep <= runs.size(); i++) fs::remove(runs[i].second, ec);
    char name[256];
    std::snprintf(name, sizeof(name), "%s_%03d.log", prefix.c_str(), runs.empty() ? 1 : runs.back().first + 1);
    g_path = (fs::path(dir) / name).string();
    g_file = std::fopen(g_path.c_str(), "w");
}

const std::string& Path() { return g_path; }

const std::string& PrevPath() { return g_prev_path; }

void Write(Level lv, const char* fmt, ...) {
    if (!kVerbose && lv < Level::Error) return;
    char msg[4096];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
    static const char* names[] = {"debug", "info", "warn", "error", "info"};
    auto tid = std::hash<std::thread::id>()(std::this_thread::get_id()) & 0xFFFF;
    std::lock_guard<std::mutex> lock(g_mutex);
    char line[4300];
    int n = kVerbose ? std::snprintf(line, sizeof(line), "[%9.3f] [%s] [t%04zx] %s\n", t, names[int(lv)], size_t(tid), msg)
                     : std::snprintf(line, sizeof(line), "[%9.3f] %s%s\n", t, lv == Level::Error ? "error: " : "", msg);
    n = std::min(n, int(sizeof(line)) - 1);
    if (g_file) {
        std::fwrite(line, 1, size_t(n), g_file);
        if (lv >= Level::Warn) std::fflush(g_file);
    }
    std::fwrite(line, 1, size_t(n), stderr);
}

void Flush() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) std::fflush(g_file);
}

}  // namespace logx
