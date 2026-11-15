/**
 * ============================================================================
 * THE KIKGEN LABS - PLUGINS MANAGER FOR MPCs AND FORCE DEVICES
 * File: src/tkgl_config.cpp
 * Description: Block-delimited parser for mpc_plugins.conf, internet reachability
 *              check via ping, and libcurl IPv4 downloader for catalog.json,
 *              plugin thumbnails (thumb.png), and plugin archives.
 * ============================================================================
 */

#include "tkgl_config.hpp"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <csignal>
#include <ctime>
#include <chrono>
#include <dirent.h>
#include <libgen.h>
#include <dlfcn.h>
#include <unordered_map>
#include <set>
#include <algorithm>

namespace tkgl::config {

namespace {

// Reload glibc DNS resolver configuration (handles autorun starting before Wi-Fi DHCP updates /etc/resolv.conf)
static void reloadSystemDnsResolver() {
    using ResInitFn = int(*)(void);
    static ResInitFn fn = nullptr;
    static bool lookedUp = false;
    if (!lookedUp) {
        lookedUp = true;
        fn = reinterpret_cast<ResInitFn>(dlsym(RTLD_DEFAULT, "__res_init"));
        if (!fn) fn = reinterpret_cast<ResInitFn>(dlsym(RTLD_DEFAULT, "res_init"));
    }
    if (fn) fn();
}

static std::string sanitizePingHost(const std::string& rawHost) {
    std::string s = rawHost;
    if (s.rfind("https://", 0) == 0) s = s.substr(8);
    else if (s.rfind("http://", 0) == 0) s = s.substr(7);
    size_t slash = s.find_first_of("/:");
    if (slash != std::string::npos) s = s.substr(0, slash);

    std::string clean;
    for (char c : s) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-') {
            clean += c;
        }
    }
    return clean.empty() ? "github.com" : clean;
}

} // anonymous namespace

bool ConfigParser::checkInternetReachable(
    const std::string& pingHost,
    std::vector<std::string>* outLogs
) {
    reloadSystemDnsResolver();
    std::string host = sanitizePingHost(pingHost);

    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        int devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            dup2(devNull, STDOUT_FILENO);
            dup2(devNull, STDERR_FILENO);
            if (devNull > 2) close(devNull);
        }
        execlp("ping", "ping", "-c", "1", "-W", "2", host.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    } else if (pid > 0) {
        auto startWait = std::chrono::steady_clock::now();
        int status = 0;
        while (true) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) {
                bool ok = (WIFEXITED(status) && WEXITSTATUS(status) == 0);
                if (ok) {
                    std::string msg = "[TKGL-NET] Online check OK (ping " + host + " succeeded).";
                    std::printf("%s\n", msg.c_str());
                    if (outLogs) outLogs->push_back(msg);
                } else {
                    std::string msg = "[TKGL-NET] Offline: ping " + host + " failed (no internet access).";
                    std::fprintf(stderr, "%s\n", msg.c_str());
                    if (outLogs) outLogs->push_back(msg);
                }
                return ok;
            }
            if (w < 0) break;
            usleep(80000);
            auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startWait
            ).count();
            if (elapsedMs >= 3000) {
                kill(-pid, SIGKILL);
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                std::string msg = "[TKGL-NET] Offline: ping " + host + " timed out.";
                std::fprintf(stderr, "%s\n", msg.c_str());
                if (outLogs) outLogs->push_back(msg);
                return false;
            }
        }
    }
    return false;
}

bool ConfigParser::downloadFile(
    const std::string& url,
    const std::string& destPath,
    long timeoutSec,
    std::function<void(const std::string&)> onProgress,
    const std::string& progressPrefix
) {
    if (url.empty() || destPath.empty()) {
        std::fprintf(stderr, "[ERROR] downloadFile: Empty URL or destination path.\n");
        return false;
    }

    reloadSystemDnsResolver();

    // Sanitize URL (trim whitespace and encode spaces)
    std::string cleanUrl;
    cleanUrl.reserve(url.size() + 16);
    for (char c : url) {
        if (c == '\r' || c == '\n' || c == '\t') continue;
        if (c == ' ') cleanUrl += "%20";
        else cleanUrl += c;
    }
    if (cleanUrl.empty()) return false;

    // Ensure parent directory of destPath exists
    size_t lastSlash = destPath.find_last_of('/');
    if (lastSlash != std::string::npos && lastSlash > 0) {
        std::string parentDir = destPath.substr(0, lastSlash);
        mkdir(parentDir.c_str(), 0755);
    }

    std::string tmpPath = destPath + ".part";
    unlink(tmpPath.c_str());

    FILE* fp = fopen(tmpPath.c_str(), "wb");
    if (!fp) {
        char errBuf[256];
        std::snprintf(errBuf, sizeof(errBuf), "[ERROR] Could not open %s for writing: %s", tmpPath.c_str(), strerror(errno));
        std::fprintf(stderr, "%s\n", errBuf);
        if (onProgress) onProgress(errBuf);
        return false;
    }

    // Dynamic loader for libcurl present across all MPC firmware generations
    struct LibCurlLoader {
        void* handle = nullptr;
        int   (*global_init)(long) = nullptr;
        void  (*global_cleanup)() = nullptr;
        void* (*easy_init)() = nullptr;
        void  (*easy_cleanup)(void*) = nullptr;
        int   (*easy_setopt)(void*, int, ...) = nullptr;
        int   (*easy_perform)(void*) = nullptr;
        const char* (*easy_strerror)(int) = nullptr;
        int   (*easy_getinfo)(void*, int, ...) = nullptr;
        bool  loaded = false;
        std::string loadError;

        LibCurlLoader() {
            const char* candidates[] = {
                "libcurl.so.4",
                "libcurl.so.4.8.0",
                "libcurl.so.3",
                "libcurl.so",
                "/usr/lib/libcurl.so.4",
                "/lib/libcurl.so.4",
                "/usr/lib/arm-linux-gnueabihf/libcurl.so.4",
                "/usr/lib/aarch64-linux-gnu/libcurl.so.4",
                "/usr/lib/x86_64-linux-gnu/libcurl.so.4"
            };
            for (const char* path : candidates) {
                handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
                if (handle) break;
            }
            if (!handle) {
                const char* dlErr = dlerror();
                loadError = dlErr ? dlErr : "libcurl.so.4 not found on system";
                return;
            }

            *(void**)(&global_init)   = dlsym(handle, "curl_global_init");
            *(void**)(&global_cleanup)= dlsym(handle, "curl_global_cleanup");
            *(void**)(&easy_init)     = dlsym(handle, "curl_easy_init");
            *(void**)(&easy_cleanup)  = dlsym(handle, "curl_easy_cleanup");
            *(void**)(&easy_setopt)   = dlsym(handle, "curl_easy_setopt");
            *(void**)(&easy_perform)  = dlsym(handle, "curl_easy_perform");
            *(void**)(&easy_strerror) = dlsym(handle, "curl_easy_strerror");
            *(void**)(&easy_getinfo)  = dlsym(handle, "curl_easy_getinfo");

            if (!easy_init || !easy_cleanup || !easy_setopt || !easy_perform) {
                loadError = "Required libcurl function symbols missing";
                dlclose(handle);
                handle = nullptr;
                return;
            }
            if (global_init) global_init(3L /* CURL_GLOBAL_DEFAULT */);
            loaded = true;
        }

        ~LibCurlLoader() {
            if (global_cleanup && loaded) global_cleanup();
            if (handle) dlclose(handle);
        }
    };

    static LibCurlLoader s_curl;
    if (!s_curl.loaded) {
        fclose(fp);
        unlink(tmpPath.c_str());
        std::string errMsg = "[ERROR] libcurl is not available: " + s_curl.loadError + ". Download aborted (no fallback).";
        std::fprintf(stderr, "%s\n", errMsg.c_str());
        if (onProgress) onProgress(errMsg);
        return false;
    }

    void* curl = s_curl.easy_init();
    if (!curl) {
        fclose(fp);
        unlink(tmpPath.c_str());
        std::string errMsg = "[ERROR] curl_easy_init failed. Download aborted.";
        std::fprintf(stderr, "%s\n", errMsg.c_str());
        if (onProgress) onProgress(errMsg);
        return false;
    }

    auto writeCb = [](char* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        FILE* outF = static_cast<FILE*>(userdata);
        return (outF && ptr) ? fwrite(ptr, size, nmemb, outF) : 0;
    };

    struct ProgressData {
        std::function<void(const std::string&)> onProgress;
        std::string prefix;
        std::chrono::steady_clock::time_point lastTick;
        int dotCount = 0;
    };

    ProgressData pdata;
    pdata.onProgress = onProgress;
    pdata.prefix = progressPrefix;
    pdata.lastTick = std::chrono::steady_clock::now();

    auto progCb = [](void* clientp, double dltotal, double dlnow, double /*ultotal*/, double /*ulnow*/) -> int {
        ProgressData* p = static_cast<ProgressData*>(clientp);
        if (!p || !p->onProgress || p->prefix.empty()) return 0;
        auto now = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - p->lastTick).count();
        if (ms >= 250) {
            p->lastTick = now;
            p->dotCount = (p->dotCount % 5) + 1;
            std::string dots(static_cast<size_t>(p->dotCount), '.');
            double mbNow = dlnow / 1048576.0;
            char buf[64];
            if (dltotal > 0) {
                double mbTotal = dltotal / 1048576.0;
                std::snprintf(buf, sizeof(buf), "%.2f / %.2f MB", mbNow, mbTotal);
            } else {
                std::snprintf(buf, sizeof(buf), "%.2f MB", mbNow);
            }
            p->onProgress(p->prefix + " (libcurl IPv4) [ " + dots + " ] " + buf + " received");
        }
        return 0;
    };

    long maxWaitSec = (timeoutSec > 0) ? timeoutSec : 60L;

    // Standard libcurl option constants (RFC & libcurl ABI compliant)
    constexpr int OPT_WRITEDATA        = 10001;
    constexpr int OPT_URL              = 10002;
    constexpr int OPT_TIMEOUT          = 13;
    constexpr int OPT_NOPROGRESS       = 43;
    constexpr int OPT_FAILONERROR      = 45;
    constexpr int OPT_FOLLOWLOCATION   = 52;
    constexpr int OPT_PROGRESSFUNCTION = 20056;
    constexpr int OPT_PROGRESSDATA     = 10057;
    constexpr int OPT_WRITEFUNCTION    = 20011;
    constexpr int OPT_SSL_VERIFYPEER   = 64;
    constexpr int OPT_SSL_VERIFYHOST   = 81;
    constexpr int OPT_CONNECTTIMEOUT   = 78;
    constexpr int OPT_NOSIGNAL         = 99;
    constexpr int OPT_USERAGENT        = 10018;
    constexpr int OPT_IPRESOLVE        = 113;
    constexpr int OPT_MAXREDIRS        = 68;
    constexpr long VAL_IPRESOLVE_V4    = 1L; // Mandatory IPv4 configuration per user requirement

    s_curl.easy_setopt(curl, OPT_URL, cleanUrl.c_str());
    s_curl.easy_setopt(curl, OPT_WRITEFUNCTION, static_cast<size_t(*)(char*,size_t,size_t,void*)>(writeCb));
    s_curl.easy_setopt(curl, OPT_WRITEDATA, fp);
    s_curl.easy_setopt(curl, OPT_IPRESOLVE, VAL_IPRESOLVE_V4); // Strictly IPv4
    s_curl.easy_setopt(curl, OPT_FOLLOWLOCATION, 1L);
    s_curl.easy_setopt(curl, OPT_MAXREDIRS, 10L);
    s_curl.easy_setopt(curl, OPT_TIMEOUT, maxWaitSec);
    s_curl.easy_setopt(curl, OPT_CONNECTTIMEOUT, 15L);
    s_curl.easy_setopt(curl, OPT_FAILONERROR, 1L);
    s_curl.easy_setopt(curl, OPT_SSL_VERIFYPEER, 0L);
    s_curl.easy_setopt(curl, OPT_SSL_VERIFYHOST, 0L);
    s_curl.easy_setopt(curl, OPT_NOSIGNAL, 1L);
    s_curl.easy_setopt(curl, OPT_USERAGENT, "TheKikGenLabs-MPC-PluginManager/1.0");

    if (onProgress && !progressPrefix.empty()) {
        s_curl.easy_setopt(curl, OPT_NOPROGRESS, 0L);
        s_curl.easy_setopt(curl, OPT_PROGRESSFUNCTION, static_cast<int(*)(void*,double,double,double,double)>(progCb));
        s_curl.easy_setopt(curl, OPT_PROGRESSDATA, &pdata);
    } else {
        s_curl.easy_setopt(curl, OPT_NOPROGRESS, 1L);
    }

    int res = s_curl.easy_perform(curl);
    long httpCode = 0;
    if (s_curl.easy_getinfo) {
        s_curl.easy_getinfo(curl, 0x200002 /* CURLINFO_RESPONSE_CODE */, &httpCode);
    }

    fclose(fp);
    s_curl.easy_cleanup(curl);

    // If libcurl does not respond, transfer fails, or HTTP code indicates error: DO NOT FALLBACK!
    if (res != 0 /* CURLE_OK */ || (httpCode >= 400)) {
        unlink(tmpPath.c_str());
        const char* errStr = s_curl.easy_strerror ? s_curl.easy_strerror(res) : "transfer failed";
        char errMsg[256];
        if (httpCode > 0) {
            std::snprintf(errMsg, sizeof(errMsg), "[ERROR] libcurl IPv4 transfer failed (HTTP %ld, res=%d: %s) for %s", httpCode, res, errStr, cleanUrl.c_str());
        } else {
            std::snprintf(errMsg, sizeof(errMsg), "[ERROR] libcurl IPv4 transfer failed (res=%d: %s) for %s", res, errStr, cleanUrl.c_str());
        }
        std::fprintf(stderr, "%s\n", errMsg);
        if (onProgress) onProgress(errMsg);
        return false; // Strictly no fallback
    }

    struct stat st;
    if (stat(tmpPath.c_str(), &st) != 0 || st.st_size <= 0) {
        unlink(tmpPath.c_str());
        std::string errMsg = "[ERROR] libcurl downloaded file is empty: " + cleanUrl;
        std::fprintf(stderr, "%s\n", errMsg.c_str());
        if (onProgress) onProgress(errMsg);
        return false;
    }

    // If destination is a .png file, verify the 8-byte PNG signature so a 404 HTML page is never saved as a thumbnail
    if (destPath.size() >= 4 && destPath.compare(destPath.size() - 4, 4, ".png") == 0) {
        std::ifstream pngCheck(tmpPath, std::ios::binary);
        uint8_t sig[8] = {0};
        pngCheck.read(reinterpret_cast<char*>(sig), 8);
        pngCheck.close();
        static const uint8_t kPngSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
        if (std::memcmp(sig, kPngSig, 8) != 0) {
            unlink(tmpPath.c_str());
            std::string errMsg = "[ERROR] Downloaded thumbnail has invalid PNG signature: " + cleanUrl;
            std::fprintf(stderr, "%s\n", errMsg.c_str());
            if (onProgress) onProgress(errMsg);
            return false;
        }
    }

    unlink(destPath.c_str());
    if (std::rename(tmpPath.c_str(), destPath.c_str()) == 0) {
        return true;
    }

    std::ifstream srcF(tmpPath, std::ios::binary);
    std::ofstream dstF(destPath, std::ios::binary | std::ios::trunc);
    if (srcF.is_open() && dstF.is_open()) {
        dstF << srcF.rdbuf();
        dstF.close();
        unlink(tmpPath.c_str());
        return (stat(destPath.c_str(), &st) == 0 && st.st_size > 0);
    }

    unlink(tmpPath.c_str());
    return false;
}

std::string ConfigParser::getManagerRootDir() {
    // Capture initial working directory before any chdir() happens in main()
    static std::string s_initialCwd;
    if (s_initialCwd.empty()) {
        char cwdBuf[1024];
        if (getcwd(cwdBuf, sizeof(cwdBuf)) != nullptr) {
            s_initialCwd = cwdBuf;
            while (s_initialCwd.size() > 1 && s_initialCwd.back() == '/') s_initialCwd.pop_back();
        }
    }

    // 1. Check PLG_MAN_DIR environment variable first (handles /tmp execution when USB vfat is mounted noexec)
    const char* envDir = std::getenv("PLG_MAN_DIR");
    if (envDir && envDir[0] != '\0') {
        std::string dir(envDir);
        char resolved[1024];
        if (dir[0] != '/' && realpath(dir.c_str(), resolved) != nullptr) {
            dir = resolved;
        } else if (dir == "." && !s_initialCwd.empty()) {
            dir = s_initialCwd;
        }
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        return dir;
    }

    // 2. Fallback to executable directory via /proc/self/exe, UNLESS running from /tmp without config
    char exePath[1024];
    ssize_t exeLen = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (exeLen > 0) {
        exePath[exeLen] = '\0';
        char* exeDir = dirname(exePath);
        if (exeDir && std::strlen(exeDir) > 0) {
            std::string dir(exeDir);
            while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
            bool isTmpExec = (dir == "/tmp" || dir.rfind("/tmp/", 0) == 0 || dir == "/var/tmp");
            struct stat stConf;
            std::string confInExe = dir + "/mpc_plugins.conf";
            if (!isTmpExec || stat(confInExe.c_str(), &stConf) == 0) {
                return dir;
            }
        }
    }

    // 3. Fallback to initial working directory (e.g. when launched as /tmp/mpc_plugin_manager_fb from USB folder)
    if (!s_initialCwd.empty() && s_initialCwd != "/tmp") {
        return s_initialCwd;
    }

    char cwdBuf[1024];
    if (getcwd(cwdBuf, sizeof(cwdBuf)) != nullptr) {
        std::string dir(cwdBuf);
        while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        return dir;
    }

    return "/media/az01-internal-sd/KikGen_MPC_Force_Plugin_Manager";
}

void ConfigParser::initLogFile() {
    std::string root = getManagerRootDir();
    std::string logPath = root + "/plgman_log.txt";
    std::ofstream lf(logPath, std::ios::trunc);
    if (lf.is_open()) {
        char timeBuf[64];
        std::time_t now = std::time(nullptr);
        std::strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        lf << "=================================================================\n";
        lf << "The KikGen Labs - Plugins Manager for Akai MPC and Force Devices\n";
        lf << "Version: " << TKGL_MANAGER_VERSION << " | Compiled: " << __DATE__ << " " << __TIME__ << "\n";
        lf << "Session started: " << timeBuf << "\n";
        lf << "Manager root dir: " << root << "\n";
        lf << "=================================================================\n";
        lf.flush();
    }
}

void ConfigParser::logMessage(const std::string& msg) {
    if (msg.empty()) return;
    std::string root = getManagerRootDir();
    std::string logPath = root + "/plgman_log.txt";
    std::ofstream lf(logPath, std::ios::app);
    if (lf.is_open()) {
        char timeBuf[32];
        std::time_t now = std::time(nullptr);
        std::strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", std::localtime(&now));
        lf << "[" << timeBuf << "] " << msg << "\n";
        lf.flush();
    }
}

void LauncherConfig::loadEmbeddedDefaults() {
    sections.clear();
    {
        Section sec;
        sec.title = "INSTRUMENTS";
        sec.items = {
        {
            "tr-6w6", "6W6", "athousanddetails", "v1.3.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-12",
            "4120", "7.6 MB", "31%",
            "athousanddetails - VST - 6W6", "tr6w6.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/ba525167c18bb0961dfd60d5989a033f737fe517/docs/screenshots/6w6.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/6w6-vst-v1.3.1/6W6-1.3.1-mpc-armv7.zip", "plugins/tr-6w6/thumb.png",
            "MPC/Force port of athousanddetails' schwung-6W6 TR-606 drum machine: voices fitted against real hardware recordings, with measured-partial hats and cymbal. Touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/athousanddetails - VST - 6W6\" && ZIP_PATH=\"./plugins/tr-6w6/tr-6w6.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/6w6-vst-v1.3.1/6W6-1.3.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/tr-6w6\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/6w6-vst-v1.3.1/6W6-1.3.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/athousanddetails - VST - 6W6/\"; elif [ -f \"./plugins/tr-6w6/tr6w6.so\" ]; then cp -rf ./plugins/tr-6w6/* \"$TARGET_SYNTH_DIR/athousanddetails - VST - 6W6/\"; else echo \"[ERROR] No local binary archive found for tr-6w6\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "tr-606" }
        },
        {
            "tr-8w8", "8W8", "athousanddetails", "v1.3.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-12",
            "4445", "5.9 MB", "13%",
            "athousanddetails - VST - 8W8", "tr8w8.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/ba525167c18bb0961dfd60d5989a033f737fe517/docs/screenshots/8w8.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/8w8-vst-v1.3.1/8W8-1.3.1-mpc-armv7.zip", "plugins/tr-8w8/thumb.png",
            "MPC/Force port of athousanddetails' schwung-8W8 TR-808 drum machine: 15 of 16 voices circuit-modelled from the service notes. Touchscreen skin and Q-Links.", "./plugins/tr-8w8/install.sh", 0,
            0,
            false,
            false,
            "",
            { "tr-808" }
        },
        {
            "tr-9w9", "9W9", "athousanddetails", "v1.3.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-12",
            "4245", "5.1 MB", "21%",
            "athousanddetails - VST - 9W9", "tr9w9.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/ba525167c18bb0961dfd60d5989a033f737fe517/docs/screenshots/9w9.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/9w9-vst-v1.3.1/9W9-1.3.1-mpc-armv7.zip", "plugins/tr-9w9/thumb.png",
            "MPC/Force port of athousanddetails' schwung-9W9 TR-909 drum machine: circuit-modelled kick, snare, toms, rim and clap, with sampled hats, ride and crash (ER-99 derived), as on the original. Touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/athousanddetails - VST - 9W9\" && ZIP_PATH=\"./plugins/tr-9w9/tr-9w9.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/9w9-vst-v1.3.1/9W9-1.3.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/tr-9w9\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/9w9-vst-v1.3.1/9W9-1.3.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/athousanddetails - VST - 9W9/\"; elif [ -f \"./plugins/tr-9w9/tr9w9.so\" ]; then cp -rf ./plugins/tr-9w9/* \"$TARGET_SYNTH_DIR/athousanddetails - VST - 9W9/\"; else echo \"[ERROR] No local binary archive found for tr-9w9\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "tr-909" }
        },
        {
            "acid-303", "Acid-303", "sd88me", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "1857", "23.6 MB", "28%",
            "sd88me - VST - Acid-303", "acid303.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-acid303/main/docs/device-303.png", "https://github.com/sd88me/mpc-vst-acid303/releases/download/acid303-vst-v1.0.2/Acid-303-1.0.2-mpc-armv7.zip", "plugins/acid-303/thumb.png",
            "An accurate, circuit-modelled 303 voice, matched to the hardware: the voice (from Lemus) solves the original schematic transistor by transistor and is fitted to 400 notes from a real unit. Switchable Devil Fish mods; Gen1 and Gen2 (WARN for CPU on Gen1: one or two instances, ECO).", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Acid-303\" && ZIP_PATH=\"./plugins/acid-303/acid-303.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-acid303/releases/download/acid303-vst-v1.0.2/Acid-303-1.0.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/acid-303\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-acid303/releases/download/acid303-vst-v1.0.2/Acid-303-1.0.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Acid-303/\"; elif [ -f \"./plugins/acid-303/acid303.so\" ]; then cp -rf ./plugins/acid-303/* \"$TARGET_SYNTH_DIR/sd88me - VST - Acid-303/\"; else echo \"[ERROR] No local binary archive found for acid-303\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "303", "acid", "mono", "bass", "circuit-model", "devil-fish" }
        },
        {
            "aphex", "Aphex", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2429", "7.4 MB", "14%",
            "Filliformes - VST - [SYN] Aphex", "aphex.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-aphex/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-aphex/releases/download/v1.1.1/SYN-Aphex-1.1.1-mpc-armv7.zip", "plugins/aphex/thumb.png",
            "Korg MS-10/MS-20 style mono synth with a patch bay, an ESP section and both MS filters.", "./plugins/aphex/install.sh", 0,
            0,
            false,
            false,
            "",
            { "ms-20", "monophonic", "patch-bay", "analog-modelling" }
        },
        {
            "breaks", "Breaks", "Aaron Quint", "v0.9.7", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-12",
            "861", "5.6 MB", "9%",
            "quirkey - VST - Breaks", "breaks.so",
            "https://raw.githubusercontent.com/quirkey/mpc-plugins/breaks-v0.9.7/docs/breaks.png", "https://github.com/quirkey/mpc-plugins/releases/download/breaks-v0.9.7/Breaks-0.9.7-mpc-armv7.zip", "plugins/breaks/thumb.png",
            "Plays a drum break in time with the song while the transport runs: 16th slices retriggered on the song grid, through the whole break whatever the loop length, with pitch, bit and sample-rate grit, time-stretch grains, half time, seeded variations and pad effects. Bring your own breaks.", "./plugins/breaks/install.sh", 0,
            0,
            false,
            false,
            "",
            { "breakbeat", "loop", "slicer", "tempo-sync", "lo-fi" }
        },
        {
            "chordism", "Chordism", "saustin2010", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2444", "2.3 MB", "14%",
            "Charles Vestal - VST - [SYN] Chordism", "chordism.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-chordism/v1.0.2/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-chordism/releases/download/v1.0.2/SYN-Chordism-1.0.2-mpc-armv7.zip", "plugins/chordism/thumb.png",
            "One key in, a four-voice chord out: morphing oscillators, FM, filter, lo-fi, delay, reverb and an arpeggiator.", "./plugins/chordism/install.sh", 0,
            0,
            false,
            false,
            "",
            { "chords", "arpeggiator", "fm", "lo-fi" }
        },
        {
            "clementine-xt", "Clementine-XT", "sd88me", "v0.9.7", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-11-01",
            "6091", "2.5 MB", "14%",
            "sd88me - VST - Clementine-XT", "clementine_xt.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-clementineXT/main/docs/img/clementine-xt.png", "https://github.com/sd88me/mpc-vst-clementineXT/releases/download/clementine-xt-v0.9.7/Clementine-XT-0.9.7-mpc-armv7.zip", "plugins/clementine-xt/thumb.png",
            "Ten-voice wavetable synthesizer modelled exactly on the Microwave II/XT with a ten-page touchscreen skin and Q-Links. Needs your own ROM files in its ROMS folder.", "./plugins/clementine-xt/install.sh", 0,
            0,
            false,
            false,
            "",
            { "wavetable", "polyphonic", "arpeggiator" }
        },
        {
            "op1fx-cluster", "Cluster", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "823", "3.5 MB", "14%",
            "bombest - VST - Cluster", "cluster.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/cluster.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Cluster-0.4.1-mpc-armv7.zip", "plugins/op1fx-cluster/thumb.png",
            "Eight self-feedback sine oscillators that drift apart over a note. OP-1 inspired.", "./plugins/op1fx-cluster/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "synth", "feedback", "drone" }
        },
        {
            "crate-digger", "Crate Digger", "sd88me", "v2.0.0", "Sample Browser",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-12",
            "13336", "66.7 MB", "14%",
            "sd88me - VST - Crate Digger", "cratedigger.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-cratedigger/c638d19f51220df69ab30a3831b13d5aac896d7e/docs/screenshots/play.png", "https://github.com/sd88me/mpc-vst-cratedigger/releases/download/cratedigger-vst-v2.0.0/Crate-Digger-2.0.0-mpc-armv7.zip", "plugins/crate-digger/thumb.png",
            "Dig for records by genre, style, decade, region and country on Discogs, then stream and play them into an MPC track, with its own screen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Crate Digger\" && ZIP_PATH=\"./plugins/crate-digger/crate-digger.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-cratedigger/releases/download/cratedigger-vst-v2.0.0/Crate-Digger-2.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/crate-digger\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-cratedigger/releases/download/cratedigger-vst-v2.0.0/Crate-Digger-2.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Crate Digger/\"; elif [ -f \"./plugins/crate-digger/cratedigger.so\" ]; then cp -rf ./plugins/crate-digger/* \"$TARGET_SYNTH_DIR/sd88me - VST - Crate Digger/\"; else echo \"[ERROR] No local binary archive found for crate-digger\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "discogs", "streaming" }
        },
        {
            "csxxl", "CSXXL", "L'Cronx", "v1.0.13", "Synth",
            "INST", "Stable", "Force (Gen1) (3.9.1)", "MPC OS 3.x", "2026-11-12",
            "927", "7.2 MB", "14%",
            "L'Cronx - VST - CSXXL", "csxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/CSXXL/v1.0.13/docs/screenshots/hero.jpg", "https://github.com/sunskiefer/CSXXL/releases/download/v1.0.13/CSXXL-1.0.13-mpc-armv7.zip", "plugins/csxxl/thumb.png",
            "A two-layer, 8-voice, polyphonic synth in the spirit of the Yamaha CS-80, written from voice notes and self-built schematics. Slider-only panels over 12 pages, per-key aftertouch, touch ribbon, arpeggiator with an 8-step gate per layer, ring mod, chorus and reverb, 768 factory presets in a named list. MPC OS 3.x.", "./plugins/csxxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "synth", "polysynth", "analog", "cs-80", "layers", "arpeggiator", "ribbon", "presets" }
        },
        {
            "tr-cw78", "CW-78", "athousanddetails", "v1.3.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-12",
            "3949", "6.9 MB", "8%",
            "athousanddetails - VST - CW-78", "trcw78.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/ba525167c18bb0961dfd60d5989a033f737fe517/docs/screenshots/cw78.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/cw78-vst-v1.3.1/CW-78-1.3.1-mpc-armv7.zip", "plugins/tr-cw78/thumb.png",
            "MPC/Force port of athousanddetails' schwung-CW-78 CR-78 drum machine: voices modelled from the 1979 service notes, with the preset rhythms. Touchscreen skin and Q-Links.", "./plugins/tr-cw78/install.sh", 0,
            0,
            false,
            false,
            "",
            { "cr-78" }
        },
        {
            "op1fx-dbox", "DBox", "Bombest", "v0.4.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "686", "8.6 MB", "14%",
            "bombest - VST - DBox", "dbox.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/dbox.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DBox-0.4.1-mpc-armv7.zip", "plugins/op1fx-dbox/thumb.png",
            "A 24-key synthesized drum kit: two oscillators and noise per hit, 12 voices. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - DBox\" && ZIP_PATH=\"./plugins/op1fx-dbox/op1fx-dbox.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DBox-0.4.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-dbox\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DBox-0.4.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/bombest - VST - DBox/\"; elif [ -f \"./plugins/op1fx-dbox/dbox.so\" ]; then cp -rf ./plugins/op1fx-dbox/* \"$TARGET_SYNTH_DIR/bombest - VST - DBox/\"; else echo \"[ERROR] No local binary archive found for op1fx-dbox\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "drums", "synth", "kit" }
        },
        {
            "denis", "Denis", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2262", "5.4 MB", "14%",
            "Filliformes - VST - [SYN] Denis", "denis.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-denis/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-denis/releases/download/v1.1.1/SYN-Denis-1.1.1-mpc-armv7.zip", "plugins/denis/thumb.png",
            "West Coast / Serge-inspired mono synth: complex oscillators, a wavefolder and a modulation matrix.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Denis\" && ZIP_PATH=\"./plugins/denis/denis.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-denis/releases/download/v1.1.1/SYN-Denis-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/denis\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-denis/releases/download/v1.1.1/SYN-Denis-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Denis/\"; elif [ -f \"./plugins/denis/denis.so\" ]; then cp -rf ./plugins/denis/* \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Denis/\"; else echo \"[ERROR] No local binary archive found for denis\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "west-coast", "serge", "wavefolder", "monophonic" }
        },
        {
            "dexed-dx7", "Dexed", "sd88me", "v2.0.3", "Fm Synth",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-13",
            "9958", "6.9 MB", "14%",
            "sd88me - VST - Dexed (DX7)", "dx7_dexed.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-dx7/main/docs/img/dexed-dx7.png", "https://github.com/sd88me/mpc-vst-dx7/releases/download/dx7-v2.0.3/Dexed-DX7-2.0.3-mpc-armv7.zip", "plugins/dexed-dx7/thumb.png",
            "DX7-style 6-operator FM synth (Dexed/MSFA engine) as a native instrument, with .syx bank browsing, all 32 algorithm diagrams and a touchscreen skin with Q-Links.", "./plugins/dexed-dx7/install.sh", 0,
            0,
            false,
            false,
            "",
            { "dx7", "emulation" }
        },
        {
            "op1fx-digital", "Digital", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "693", "5.7 MB", "14%",
            "bombest - VST - Digital", "digital.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/digital.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Digital-0.4.1-mpc-armv7.zip", "plugins/op1fx-digital/thumb.png",
            "Feedback sine oscillator, a second sine with sync, and a bit-flipping XOR stage. Eight voices. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Digital\" && ZIP_PATH=\"./plugins/op1fx-digital/op1fx-digital.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Digital-0.4.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-digital\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Digital-0.4.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/bombest - VST - Digital/\"; elif [ -f \"./plugins/op1fx-digital/digital.so\" ]; then cp -rf ./plugins/op1fx-digital/* \"$TARGET_SYNTH_DIR/bombest - VST - Digital/\"; else echo \"[ERROR] No local binary archive found for op1fx-digital\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "synth", "feedback", "xor" }
        },
        {
            "dinxxl", "DINXXL", "L'Cronx", "v1.0.0", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "695", "4.5 MB", "14%",
            "L'Cronx - VST - DINXXL", "dinxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/dinxxl/v1.0.0/docs/img/keys.png", "https://github.com/sunskiefer/DINXXL/releases/download/v1.0.0/DINXXL-1.0.0-mpc-armv7.zip", "plugins/dinxxl/thumb.png",
            "BlastBeats-style OPL3 FM groovebox: the published v7.1 firmware by Twisted Electrons runs unmodified on a software model of its board (YMF262 via ymfm). 6 drums, 4 four-op synths, 64-step sequencer with p-locks and songs, factory card inside. By L'Cronx; not made or endorsed by Twisted Electrons.", "./plugins/dinxxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "blastbeats", "opl3", "fm", "drum-machine", "groovebox", "step-sequencer", "chiptune", "adlib" }
        },
        {
            "op1fx-dna", "DNA", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "815", "4.7 MB", "14%",
            "bombest - VST - DNA", "dna.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/dna.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DNA-0.4.1-mpc-armv7.zip", "plugins/op1fx-dna/thumb.png",
            "A 128-bit genome played as a one-bit wave through an anti-aliased reader. Eight voices. OP-1 inspired.", "./plugins/op1fx-dna/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "synth", "1-bit", "wavetable" }
        },
        {
            "drloguexxl", "DrLogueXXL", "L'Cronx", "v0.2.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1179", "7.8 MB", "14%",
            "L'Cronx - VST - DrLogueXXL", "drloguexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/DrLogueXXL/v0.2.0/docs/img/perform.png", "https://github.com/sunskiefer/DrLogueXXL/releases/download/v0.2.1/DrLogueXXL-0.2.1-mpc-armv7.zip", "plugins/drloguexxl/thumb.png",
            "Hybrid drum machine modelled on the KORG drumlogue (not affiliated with KORG): analogue-style kick, snare and toms, six sample parts with 192 synthesised sounds and your own WAVs, a MULTI part with VPM, noise and six open logue SDK units, delay, reverb and master effects, choke groups, a mixer and four macros, on a panel in the machine's look.", "./plugins/drloguexxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "drums", "analog", "hybrid", "samples", "fm", "logue", "drumlogue", "mixer" }
        },
        {
            "dronexxl", "DroneXXL", "L'Cronx", "v0.1.0", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-25",
            "1037", "83.7 MB", "14%",
            "L'Cronx - VST - DroneXXL", "dronexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/DroneXXL/v0.1.0/docs/img/overview.png", "https://github.com/sunskiefer/DroneXXL/releases/download/v0.1.0/DroneXXL-0.1.0-mpc-armv7.zip", "plugins/dronexxl/thumb.png",
            "Drone and experimental synth, after the Solar 42, built on Neil Baumberger's AmbientModules: a 15-module rack (4 drones, mixer, filter, pads, 2 PapaSrapa, 2 VCOs, LFOs, joystick, sequencer, detector) patched by touch, with 8 macros, an XY pad, a randomizer that never repeats and a brickwall limiter.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - DroneXXL\" && ZIP_PATH=\"./plugins/dronexxl/dronexxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/DroneXXL/releases/download/v0.1.0/DroneXXL-0.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/dronexxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/DroneXXL/releases/download/v0.1.0/DroneXXL-0.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - DroneXXL/\"; elif [ -f \"./plugins/dronexxl/dronexxl.so\" ]; then cp -rf ./plugins/dronexxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - DroneXXL/\"; else echo \"[ERROR] No local binary archive found for dronexxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "drone", "experimental", "modular", "patch-panel", "ambient", "randomizer", "vcv-rack" }
        },
        {
            "op1fx-drwave", "DrWave", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "718", "6.7 MB", "14%",
            "bombest - VST - DrWave", "drwave.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/drwave.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DrWave-0.4.1-mpc-armv7.zip", "plugins/op1fx-drwave/thumb.png",
            "Wave, blend, motion and fold controls over eight voices. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - DrWave\" && ZIP_PATH=\"./plugins/op1fx-drwave/op1fx-drwave.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DrWave-0.4.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-drwave\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DrWave-0.4.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/bombest - VST - DrWave/\"; elif [ -f \"./plugins/op1fx-drwave/drwave.so\" ]; then cp -rf ./plugins/op1fx-drwave/* \"$TARGET_SYNTH_DIR/bombest - VST - DrWave/\"; else echo \"[ERROR] No local binary archive found for op1fx-drwave\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "synth", "wavetable", "fold" }
        },
        {
            "op1fx-dsynth", "DSynth", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "729", "4.8 MB", "14%",
            "bombest - VST - DSynth", "dsynth.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/dsynth.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/DSynth-0.4.1-mpc-armv7.zip", "plugins/op1fx-dsynth/thumb.png",
            "Two oscillators with FM, a pitch drop for drum thump and an eight-mode filter. OP-1 inspired.", "./plugins/op1fx-dsynth/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "synth", "fm", "drums" }
        },
        {
            "dub-force-siren", "Dub Force Siren", "no3z", "v2.1.0", "Synth",
            "INST", "Stable", "Akai Force (3.9.1.2)", "MPC OS 3.x", "2026-10-26",
            "1861", "117 KB", "14%",
            "Dub Force - VST - Dub Force Siren", "dub_force_siren.so",
            "https://raw.githubusercontent.com/no3z/mpc-dubsiren/v2.0.0/resources/preview.png", "https://github.com/no3z/mpc-dubsiren/releases/download/v2.1.0/Dub-Force-Siren-2.1.0-mpc-armv7.zip", "plugins/dub-force-siren/thumb.png",
            "Dub siren with tape echo: siren and zap modes, LFO, filter and ping-pong delay on one page mirroring two banks of eight Q-Links.", "./plugins/dub-force-siren/install.sh", 0,
            0,
            false,
            false,
            "",
            { "dub", "siren", "echo", "sfx" }
        },
        {
            "edrum-kit", "EDrum Kit", "burgerbarn", "v1.0.0", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-14",
            "64", "903 KB", "14%",
            "burgerbarn - VST - EDrum Kit", "VST/edrum-kit-arm32.so",
            "https://raw.githubusercontent.com/burgerbarn/edrum-kit-mpc/f3886f20a42ea057c3db682d4da8306c667217e2/docs/card.png", "https://github.com/burgerbarn/edrum-kit-mpc/releases/download/v1.0.0/EDrum-Kit-1.0.0-mpc-armv7.zip", "plugins/edrum-kit/thumb.png",
            "Plays the MPC's own drum kits from Roland V-Drums (TD-27): auto-mapped zones, continuous VH-14D hi-hat, positional snare and ride, cymbal chokes, velocity curves, per-drum tune/decay/tone.", "./plugins/edrum-kit/install.sh", 0,
            0,
            false,
            false,
            "",
            { "drums", "e-drums", "td-27", "midi", "sampler" }
        },
        {
            "evil-tuner-xxl", "Evil Tuner XXL", "L'Cronx", "v0.2.0", "Radio Granular",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-22",
            "2293", "12.8 MB", "14%",
            "L'Cronx - VST - Evil Tuner XXL", "evil_tuner_xxl.so",
            "https://raw.githubusercontent.com/sunskiefer/Evil-Tuner-XXL/main/docs/img/RADIO.png", "https://github.com/sunskiefer/Evil-Tuner-XXL/releases/download/v0.2.0/Evil-Tuner-XXL-0.2.0-mpc-armv7.zip", "plugins/evil-tuner-xxl/thumb.png",
            "Internet radio (30,000+ stations), or a phone over Wi-Fi or AirPlay, played into an MPC track through a granular engine, filter, character stages, delay, reverb, trance gate, stutter, tempo-synced looper and three LFOs. Inspired by the Endorphin.es Evil Pet (no affiliation, no code). Needs a network.", "./plugins/evil-tuner-xxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "radio", "internet-radio", "granular", "airplay", "looper", "stutter", "reverb", "sampling" }
        },
        {
            "fizzik", "Fizzik", "saustin2010", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2163", "1.8 MB", "14%",
            "Filliformes - VST - [SYN] Fizzik", "fizzik.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-fizzik/v1.0.2/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-fizzik/releases/download/v1.0.2/SYN-Fizzik-1.0.2-mpc-armv7.zip", "plugins/fizzik/thumb.png",
            "Physical modelling: an exciter and two coupled resonators (string, beam, plate, membrane).", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Fizzik\" && ZIP_PATH=\"./plugins/fizzik/fizzik.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-fizzik/releases/download/v1.0.2/SYN-Fizzik-1.0.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/fizzik\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-fizzik/releases/download/v1.0.2/SYN-Fizzik-1.0.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Fizzik/\"; elif [ -f \"./plugins/fizzik/fizzik.so\" ]; then cp -rf ./plugins/fizzik/* \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Fizzik/\"; else echo \"[ERROR] No local binary archive found for fizzik\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "physical-modelling", "resonator", "percussion" }
        },
        {
            "op1fx-fm", "Fm", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "733", "8.3 MB", "14%",
            "bombest - VST - Fm", "fm.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/fm.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Fm-0.4.1-mpc-armv7.zip", "plugins/op1fx-fm/thumb.png",
            "Four-operator FM synth with eight voices and four screen knobs. OP-1 inspired.", "./plugins/op1fx-fm/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "synth", "fm", "operators" }
        },
        {
            "fretty", "Fretty", "3dthc", "v0.8.6", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-13",
            "2818", "70.2 MB", "14%",
            "3dthc - VST - Fretty", "fretty.so",
            "https://raw.githubusercontent.com/3dthc/fretty-mpc/main/docs/images/play_sunburst.png", "https://github.com/3dthc/fretty-mpc/releases/download/v0.8.6/Fretty-0.8.6-mpc-armv7.zip", "plugins/fretty/thumb.png",
            "A guitar instrument for Akai MPC OS standalone devices (native VST2), featuring refined selector typography, 110 presets, and a synth layer.", "./plugins/fretty/install.sh", 0,
            0,
            false,
            false,
            "",
            {  }
        },
        {
            "gaiaxxl", "GAIAXXL", "L'Cronx", "v0.1.3", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "2027", "7.7 MB", "14%",
            "L'Cronx - VST - GAIAXXL", "gaiaxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/GAIAXXL/v0.1.2/docs/img/tone.png", "https://github.com/sunskiefer/GAIAXXL/releases/download/v0.1.3/GAIAXXL-0.1.3-mpc-armv7.zip", "plugins/gaiaxxl/thumb.png",
            "Three-tone virtual-analog synth modelled on the discontinued Roland GAIA SH-01 (not affiliated with Roland): the editor's parameters over six full-screen pages, 21 oscillator variations (with KORG logue-sdk wave tables), SYNC/RING, the GAIA's effects chain and arpeggiator, two beams, a musical randomizer and 64 patches.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - GAIAXXL\" && ZIP_PATH=\"./plugins/gaiaxxl/gaiaxxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/GAIAXXL/releases/download/v0.1.3/GAIAXXL-0.1.3-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/gaiaxxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/GAIAXXL/releases/download/v0.1.3/GAIAXXL-0.1.3-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - GAIAXXL/\"; elif [ -f \"./plugins/gaiaxxl/gaiaxxl.so\" ]; then cp -rf ./plugins/gaiaxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - GAIAXXL/\"; else echo \"[ERROR] No local binary archive found for gaiaxxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "synth", "virtual-analog", "supersaw", "gaia", "arpeggiator", "pad", "lead", "randomizer" }
        },
        {
            "grainology", "Grainology", "Stephane Schmitz", "v1.0.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-14",
            "40", "4.7 MB", "14%",
            "Stephane Schmitz - VST - Grainology", "VST/grainology-arm32.so",
            "https://raw.githubusercontent.com/eldontyrellcorp/mpc-plugin-grainology/v1.0.0/screenshots/main_page.png", "https://github.com/eldontyrellcorp/mpc-plugin-grainology/releases/download/v1.0.1/Grainology-1.0.1-mpc-armv7.zip", "plugins/grainology/thumb.png",
            "A granular synthesizer: two granular parts that play one of eleven sound generators or your own WAV files, a wavetable drone and a wavetable pad, each with a filter and two LFOs, plus a ping-pong delay and a plate reverb. 16 programs, a grain view on the screen. Built with the Akai MPC Plugin SDK; Gen1 and Gen2.", "./plugins/grainology/install.sh", 0,
            0,
            false,
            false,
            "",
            { "granular", "ambient", "drone", "pad", "wavetable" }
        },
        {
            "hank", "Hank", "saustin2010", "v1.1.1", "Fm Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1832", "2.1 MB", "14%",
            "Charles Vestal - VST - [SYN] Hank", "hank.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-hank/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-hank/releases/download/v1.1.1/SYN-Hank-1.1.1-mpc-armv7.zip", "plugins/hank/thumb.png",
            "Small, immediate two-operator FM synth on one page: ratio, brightness, bite, envelopes, noise; 32 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] Hank\" && ZIP_PATH=\"./plugins/hank/hank.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-hank/releases/download/v1.1.1/SYN-Hank-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/hank\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-hank/releases/download/v1.1.1/SYN-Hank-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] Hank/\"; elif [ -f \"./plugins/hank/hank.so\" ]; then cp -rf ./plugins/hank/* \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] Hank/\"; else echo \"[ERROR] No local binary archive found for hank\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "fm", "two-operator", "polyphonic", "bass" }
        },
        {
            "helm", "Helm", "Lewinator56", "v1.0.0", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-04",
            "1740", "12.3 MB", "14%",
            "Matt Tytel - VST - Helm", "helm.so",
            "https://github.com/user-attachments/assets/b537127a-22e6-4e21-b9f1-48e473c892b8", "https://github.com/Lewinator56/mpc-vst-helm/releases/download/1.0.0/Helm-1.0.0-mpc-armv7.zip", "plugins/helm/thumb.png",
            "32-voice polyphonic synthesizer (Helm engine by Matt Tytel) with 2 cross-mod oscillators, sub, noise, multi-mode filter, 3 LFOs, 3 envelopes, 32-step sequencer & arpeggiator, built-in effects and touchscreen skin with Q-Links.", "./plugins/helm/install.sh", 0,
            0,
            false,
            false,
            "",
            { "polyphonic", "subtractive", "arpeggiator", "step-sequencer", "mod-matrix" }
        },
        {
            "hera", "Hera", "saustin2010", "v1.1.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-24",
            "3603", "3.3 MB", "14%",
            "jpcima - VST - [SYN] Hera", "hera.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-hera/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-hera/releases/download/v1.1.2/SYN-Hera-1.1.2-mpc-armv7.zip", "plugins/hera/thumb.png",
            "Roland Juno-60: jpcima's Hera engine with its 56 presets, the BBD chorus and a touchscreen skin with Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jpcima - VST - [SYN] Hera\" && ZIP_PATH=\"./plugins/hera/hera.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-hera/releases/download/v1.1.2/SYN-Hera-1.1.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/hera\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-hera/releases/download/v1.1.2/SYN-Hera-1.1.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/jpcima - VST - [SYN] Hera/\"; elif [ -f \"./plugins/hera/hera.so\" ]; then cp -rf ./plugins/hera/* \"$TARGET_SYNTH_DIR/jpcima - VST - [SYN] Hera/\"; else echo \"[ERROR] No local binary archive found for hera\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "juno-60", "polyphonic", "chorus", "analog-modelling" }
        },
        {
            "hexachord-73", "HexChord 73", "Eurikon", "v1.1.0", "Synth",
            "INST", "Stable", "MPC Live II Retro (3.10.1)", "MPC OS 3.x", "2026-11-14",
            "1271", "3.0 MB", "14%",
            "Twisted Cable - VST - HexChord 73", "hexachord-73.so",
            "https://raw.githubusercontent.com/Eurikon/mpc-vst-plugins/9eed16e63f97e5ad87621a21cead85b6a7fee35b/media/hexachord-73.png", "https://github.com/Eurikon/mpc-vst-plugins/releases/download/hexachord-73-v1.1.0/HexChord-73-1.1.0-mpc-armv7.zip", "plugins/hexachord-73/thumb.png",
            "Stereo chord synthesizer with four engines, eight chords, four voicings and a touchscreen chord diagram.", "./plugins/hexachord-73/install.sh", 0,
            0,
            false,
            false,
            "",
            { "chord", "polyphonic", "stereo" }
        },
        {
            "incipience", "Incipience", "saustin2010", "v1.1.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1556", "7.0 MB", "14%",
            "Steve A - VST - [DRUM] Incipience", "incipience.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-incipience/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-incipience/releases/download/v1.1.1/DRUM-Incipience-1.1.1-mpc-armv7.zip", "plugins/incipience/thumb.png",
            "Eleven-instrument drum synth laid out like the Jomox AlphaBase Mk II (kick, MBrane, hats, clap, rim, cymbals, two sample players, 4-op FM, reverb, delay), with RANDOMISE per instrument and a mod matrix; each instrument chromatic on its own MIDI channel and on its own track with Incipience OUT; plays the AlphaBase's free samples and your own WAVs.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Steve A - VST - [DRUM] Incipience\" && ZIP_PATH=\"./plugins/incipience/incipience.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-incipience/releases/download/v1.1.1/DRUM-Incipience-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/incipience\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-incipience/releases/download/v1.1.1/DRUM-Incipience-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Steve A - VST - [DRUM] Incipience/\"; elif [ -f \"./plugins/incipience/incipience.so\" ]; then cp -rf ./plugins/incipience/* \"$TARGET_SYNTH_DIR/Steve A - VST - [DRUM] Incipience/\"; else echo \"[ERROR] No local binary archive found for incipience\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "drums", "alphabase", "fm", "samples", "multi-out", "mod-matrix" }
        },
        {
            "interzonexxl", "InterzoneXXL", "L'Cronx", "v0.1.3", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "2580", "13.6 MB", "14%",
            "L'Cronx - VST - InterzoneXXL", "interzonexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/InterzoneXXL/v0.1.0/docs/img/vco.png", "https://github.com/sunskiefer/InterzoneXXL/releases/download/v0.1.3/InterzoneXXL-0.1.3-mpc-armv7.zip", "plugins/interzonexxl/thumb.png",
            "Valley Audio's Interzone synth voice from VCV Rack, played from MIDI in mono or up to 16-voice poly on the module's own panel. Every input jack is patched to a built-in source (LFOs, sequencers, random) on the MPC tempo, with a brickwall limiter and 16 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - InterzoneXXL\" && ZIP_PATH=\"./plugins/interzonexxl/interzonexxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/InterzoneXXL/releases/download/v0.1.3/InterzoneXXL-0.1.3-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/interzonexxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/InterzoneXXL/releases/download/v0.1.3/InterzoneXXL-0.1.3-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - InterzoneXXL/\"; elif [ -f \"./plugins/interzonexxl/interzonexxl.so\" ]; then cp -rf ./plugins/interzonexxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - InterzoneXXL/\"; else echo \"[ERROR] No local binary archive found for interzonexxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "synth", "monosynth", "polysynth", "analog", "vcv", "valley", "lfo", "modulation" }
        },
        {
            "jc303-mpc", "JC303", "303inmyheart", "v0.7.4", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-15",
            "392", "7.3 MB", "35%",
            "midilab - VST - JC303 MPC", "jc303_mpc.so",
            "https://raw.githubusercontent.com/303inmyheart/OpenMPC-JC303/jc303-mpc-v0.7.4/docs/screenshots/synth.png", "https://github.com/303inmyheart/OpenMPC-JC303/releases/download/jc303-mpc-v0.7.4/JC303-MPC-0.7.4-mpc-aarch64.zip", "plugins/jc303-mpc/thumb.png",
            "303-style synth for Gen2 MPCs: eight filters, 64-step touch sequencer, modulation matrix, 37 neural drive models and native MPC presets. Hardware tested on Live III.", "./plugins/jc303-mpc/install.sh", 0,
            0,
            false,
            false,
            "",
            { "acid", "bass", "sequencer", "analog", "gen2" }
        },
        {
            "je8086", "JE-8086", "sd88me", "v1.0.0", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-15",
            "130", "14.1 MB", "14%",
            "sd88me - VST - JE-8086", "je8086.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-je8086/v1.0.0/docs/screenshots/main.png", "https://github.com/sd88me/mpc-vst-je8086/releases/download/v1.0.0/JE-8086-1.0.0-mpc-aarch64.zip", "plugins/je8086/thumb.png",
            "An emulation of the Roland JP-8000's original firmware (gearmulator's JE-8086 core) as a native instrument for Gen2 MPC devices, with a touchscreen skin in the JP-8000's look, envelope graphs, named favourites, RANDOM and INIT, tempo sync and Q-Link control. Gen2 (aarch64) only; you need your own JP-8000 ROM.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - JE-8086\" && ZIP_PATH=\"./plugins/je8086/je8086.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-je8086/releases/download/v1.0.0/JE-8086-1.0.0-mpc-aarch64.zip\" ]; then mkdir -p \"./plugins/je8086\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-je8086/releases/download/v1.0.0/JE-8086-1.0.0-mpc-aarch64.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - JE-8086/\"; elif [ -f \"./plugins/je8086/je8086.so\" ]; then cp -rf ./plugins/je8086/* \"$TARGET_SYNTH_DIR/sd88me - VST - JE-8086/\"; else echo \"[ERROR] No local binary archive found for je8086\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "emulation", "virtual-analog", "rompler-free", "gen2" }
        },
        {
            "j106", "JN-106", "sd88me", "v0.2.0", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), Akai MPC Live III (aarch64) (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-28",
            "1937", "25.3 MB", "14%",
            "sd88me - VST - JN-106", "kr106.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-jn106/9053529f679d2a5f07781f002004e27cc12959e0/docs/img/screenshot-lfo-dco.png", "https://github.com/sd88me/mpc-vst-jn106/releases/download/jn-106-vst-v0.2.0/JN-106-0.2.0-mpc-armv7.zip", "plugins/j106/thumb.png",
            "Faithful, circuit-modelled and hardware-profiled six-voice Juno emulation (Ultramaster KR-106 DSP): IR3109 filter, BA662 VCA and MN3009 BBD chorus modelled from the circuits, the 106 CPU firmware reimplemented from its disassembly, curves fitted to real hardware and checked against the service notes. 256 factory presets, realistic panel skin.", "./plugins/j106/install.sh", 0,
            0,
            false,
            false,
            "",
            { "analog", "poly", "vintage", "chorus" }
        },
        {
            "ju-08", "Ju-08", "Stephane Schmitz", "v1.0.0", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-14",
            "48", "2.2 MB", "14%",
            "Stephane Schmitz - VST - Ju-08", "VST/ju-08-arm32.so",
            "https://raw.githubusercontent.com/eldontyrellcorp/mpc-plugin-ju08/v1.0.0/screenshots/main_page.png", "https://github.com/eldontyrellcorp/mpc-plugin-ju08/releases/download/v1.0.0/Ju-08-1.0.0-mpc-armv7.zip", "plugins/ju-08/thumb.png",
            "A Jupiter-8 style polysynth: two parts of four voices (or eight on one patch), two VCOs with sync and cross mod, HPF, 12/24 dB VCF, two envelopes, LFO, arpeggiator, 17 chorus and multi-effects, delay, reverb with shimmer, 64 patches. Built with the Akai MPC Plugin SDK; Gen1 and Gen2.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Stephane Schmitz - VST - Ju-08\" && ZIP_PATH=\"./plugins/ju-08/ju-08.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/eldontyrellcorp/mpc-plugin-ju08/releases/download/v1.0.0/Ju-08-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/ju-08\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/eldontyrellcorp/mpc-plugin-ju08/releases/download/v1.0.0/Ju-08-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Stephane Schmitz - VST - Ju-08/\"; elif [ -f \"./plugins/ju-08/VST/ju-08-arm32.so\" ]; then cp -rf ./plugins/ju-08/* \"$TARGET_SYNTH_DIR/Stephane Schmitz - VST - Ju-08/\"; else echo \"[ERROR] No local binary archive found for ju-08\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "poly", "analog", "jupiter", "arpeggiator", "chorus" }
        },
        {
            "kutilxxl", "KutilXXL", "L'Cronx", "v0.3.4", "Groovebox",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-09",
            "1330", "68.7 MB", "14%",
            "L'Cronx - VST - KutilXXL", "kutilxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/BUSTEDXXL/v0.3.4/docs/img/screen1.png", "https://github.com/sunskiefer/BUSTEDXXL/releases/download/v0.3.4/KutilXXL-0.3.4-mpc-armv7.zip", "plugins/kutilxxl/thumb.png",
            "Groovebox of Bastl-style modules patched with touch cables across three screens: Wave Bard, Kompas, Crust 2, Pizza, Alchemist, Basil and ABC. Patch with jack-group windows and CONNECT; 36 factory patches plus your own (up to 512) with tag filtering. MPC OS 3.x.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - KutilXXL\" && ZIP_PATH=\"./plugins/kutilxxl/kutilxxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/BUSTEDXXL/releases/download/v0.3.4/KutilXXL-0.3.4-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/kutilxxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/BUSTEDXXL/releases/download/v0.3.4/KutilXXL-0.3.4-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - KutilXXL/\"; elif [ -f \"./plugins/kutilxxl/kutilxxl.so\" ]; then cp -rf ./plugins/kutilxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - KutilXXL/\"; else echo \"[ERROR] No local binary archive found for kutilxxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "bastl", "kastle", "modular", "patch-panel", "touch-cables", "sampler", "fm", "groovebox" }
        },
        {
            "lucky-dip", "Lucky Dip", "sd88me", "v1.0.0", "Sampler",
            "INST", "Stable", "MPC Live III (Gen2) (3.9), Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-11-12",
            "2920", "2.2 MB", "14%",
            "sd88me - VST - Lucky Dip", "lucky_dip.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-luckydip/5d9704104fcad1b1cfa829796ce249d1ace035ac/docs/img/pads.png", "https://github.com/sd88me/mpc-vst-luckydip/releases/download/v1.0.0/Lucky-Dip-1.0.0-mpc-armv7.zip", "plugins/lucky-dip/thumb.png",
            "A random 16-pad drum-kit builder and inspiration machine: dips into your sample library, fills the pads at random, plays the kit and exports it as an MPC drum program.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Lucky Dip\" && ZIP_PATH=\"./plugins/lucky-dip/lucky-dip.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-luckydip/releases/download/v1.0.0/Lucky-Dip-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/lucky-dip\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-luckydip/releases/download/v1.0.0/Lucky-Dip-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Lucky Dip/\"; elif [ -f \"./plugins/lucky-dip/lucky_dip.so\" ]; then cp -rf ./plugins/lucky-dip/* \"$TARGET_SYNTH_DIR/sd88me - VST - Lucky Dip/\"; else echo \"[ERROR] No local binary archive found for lucky-dip\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "sampler", "utility", "kit-builder", "random", "drum-kit" }
        },
        {
            "lxrxxl", "LXRXXL", "L'Cronx", "v0.6.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "5864", "10.2 MB", "14%",
            "L'Cronx - VST - LXRXXL", "lxrxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/LXR02XXL/v0.6.0/docs/img/page.png", "https://github.com/sunskiefer/LXR02XXL/releases/download/v0.6.1/LXRXXL-0.6.1-mpc-armv7.zip", "plugins/lxrxxl/thumb.png",
            "Inspired by the LXR-02: its open-source sound engine (GPL-3.0) with the real FX unit and the plate reverb, the whole panel on one page, 64-step sequencer with songs, rolls and a euclidean generator, Force pads playing the voices, randomizers and the Hrtl LXR-02 DLC presets old and new. Artwork by Suns Kiefer.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL\" && ZIP_PATH=\"./plugins/lxrxxl/lxrxxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/LXR02XXL/releases/download/v0.6.1/LXRXXL-0.6.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/lxrxxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/LXR02XXL/releases/download/v0.6.1/LXRXXL-0.6.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL/\"; elif [ -f \"./plugins/lxrxxl/lxrxxl.so\" ]; then cp -rf ./plugins/lxrxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL/\"; else echo \"[ERROR] No local binary archive found for lxrxxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "drums", "drum-machine", "lxr", "lxr-02", "erica-synths", "sequencer", "reverb", "randomizer" }
        },
        {
            "lxrxxl-bazzline", "LXRXXL BAZZLINE", "L'Cronx", "v0.2.10", "Bass Synth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-22",
            "5706", "16.2 MB", "14%",
            "L'Cronx - VST - LXRXXL BAZZLINE", "lxrxxl_bazzline.so",
            "https://raw.githubusercontent.com/sunskiefer/LXRXXL-BAZZLINE/v0.2.2/docs/img/panel.png", "https://github.com/sunskiefer/LXRXXL-BAZZLINE/releases/download/v0.2.10/LXRXXL-BAZZLINE-0.2.10-mpc-armv7.zip", "plugins/lxrxxl-bazzline/thumb.png",
            "An 8-track bass synth: a variation of LXRXXL with bass voices inspired by the Erica Synths Bassline DB-01 on the LXR's oscillators, the LXR-02 style sequencer with the DB-01's step functions, a tape/digital delay, a MIDI gate/duck, an 8-track mixer and sound / performance presets saved on the device.", "./plugins/lxrxxl-bazzline/install.sh", 0,
            0,
            false,
            false,
            "",
            { "bass", "acid", "303", "bassline", "db-01", "lxr", "sequencer", "delay" }
        },
        {
            "lxrxxl-zynth", "LXRXXL ZYNTH", "L'Cronx", "v0.2.2", "Synth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-22",
            "2375", "22.4 MB", "14%",
            "L'Cronx - VST - LXRXXL ZYNTH", "lxrxxl_zynth.so",
            "https://raw.githubusercontent.com/sunskiefer/LXRXXL-ZYNTH/v0.2.0/docs/img/panel.png", "https://github.com/sunskiefer/LXRXXL-ZYNTH/releases/download/v0.2.2/LXRXXL-ZYNTH-0.2.2-mpc-armv7.zip", "plugins/lxrxxl-zynth/thumb.png",
            "An experimental 8-track synth with 19 voices on unconventional maths (Lorenz chaos, Hilbert frequency shifter, Euclidean rhythms) and voices coded after the DX7, CZ, Minimoog, Hammond, NES chips, Braids and Plaits. LXR-02 style sequencer with random functions, tape/digital delay, mixer and presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL ZYNTH\" && ZIP_PATH=\"./plugins/lxrxxl-zynth/lxrxxl-zynth.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/LXRXXL-ZYNTH/releases/download/v0.2.2/LXRXXL-ZYNTH-0.2.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/lxrxxl-zynth\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/LXRXXL-ZYNTH/releases/download/v0.2.2/LXRXXL-ZYNTH-0.2.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL ZYNTH/\"; elif [ -f \"./plugins/lxrxxl-zynth/lxrxxl_zynth.so\" ]; then cp -rf ./plugins/lxrxxl-zynth/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - LXRXXL ZYNTH/\"; else echo \"[ERROR] No local binary archive found for lxrxxl-zynth\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "synth", "groovebox", "fm", "wavetable", "sequencer", "experimental", "chaos", "lxr" }
        },
        {
            "machinemodule", "Machinemodule", "sd88me", "v1.0.5", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-11",
            "100", "2.0 MB", "14%",
            "sd88me - VST - Machinemodule", "machinemodule.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-machinedrum/main/docs/img/machinemodule.png", "", "plugins/machinemodule/thumb.png",
            "The Elektron Machinedrum SPS-1 UW sound engine as a native instrument: all 16 tracks from one plugin instance, with its own touchscreen skin and Q-Links.", "./plugins/machinemodule/install.sh", 0,
            0,
            false,
            false,
            "",
            { "elektron", "machinedrum", "firmware-derived" }
        },
        {
            "machinemodule-gen2", "Machinemodule (Gen2)", "sd88me", "v1.0.5", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-11",
            "1937", "7.7 MB", "14%",
            "sd88me - VST - Machinemodule", "machinedrum_one.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-machinedrum/main/docs/img/machinemodule-gen2.png", "https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.5/Machinemodule-1.0.5-mpc-aarch64.zip", "plugins/machinemodule-gen2/thumb.png",
            "The Elektron Machinedrum UW sound engine as an instrument for Gen2 MPC devices: 16 tracks in one plugin, touchscreen skin, bank and kit lists, Q-Links. A ready-made download with nothing of Elektron's in it: you add your own OS file (and optionally your flash image) in the plugin folder. Gen2 only.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Machinemodule\" && ZIP_PATH=\"./plugins/machinemodule-gen2/machinemodule-gen2.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.5/Machinemodule-1.0.5-mpc-aarch64.zip\" ]; then mkdir -p \"./plugins/machinemodule-gen2\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.5/Machinemodule-1.0.5-mpc-aarch64.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Machinemodule/\"; elif [ -f \"./plugins/machinemodule-gen2/machinedrum_one.so\" ]; then cp -rf ./plugins/machinemodule-gen2/* \"$TARGET_SYNTH_DIR/sd88me - VST - Machinemodule/\"; else echo \"[ERROR] No local binary archive found for machinemodule-gen2\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "elektron", "machinedrum", "gen2" }
        },
        {
            "braids", "Macro Osc", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2902", "8.0 MB", "14%",
            "Émilie Gillet - VST - [SYN] Macro Osc", "braids.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-macro-osc/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-macro-osc/releases/download/v1.1.1/SYN-Macro-Osc-1.1.1-mpc-armv7.zip", "plugins/braids/thumb.png",
            "Macro Osc: a macro oscillator with 47 synthesis models, from Émilie Gillet's open-source Braids.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Macro Osc\" && ZIP_PATH=\"./plugins/braids/braids.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-macro-osc/releases/download/v1.1.1/SYN-Macro-Osc-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/braids\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-macro-osc/releases/download/v1.1.1/SYN-Macro-Osc-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Macro Osc/\"; elif [ -f \"./plugins/braids/braids.so\" ]; then cp -rf ./plugins/braids/* \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Macro Osc/\"; else echo \"[ERROR] No local binary archive found for braids\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "macro-oscillator", "eurorack" }
        },
        {
            "maschine-group", "Maschine Group", "mmiroshnikov", "v1.0.0", "Sampler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-13",
            "918", "169 KB", "14%",
            "mmiroshnikov - VST - Maschine Group", "maschine_group.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-maschine-group/main/docs/group.png", "https://github.com/mmiroshnikov/mpc-maschine-group/releases/download/maschine-group-v1.0.0/Maschine-Group-1.0.0-mpc-armv7.zip", "plugins/maschine-group/thumb.png",
            "Opens a Native Instruments Maschine 2 drum group (.mxgrp) and plays its pads and stored patterns, with a three-page touchscreen skin and Q-Links.", "./plugins/maschine-group/install.sh", 0,
            0,
            false,
            false,
            "",
            { "drums", "maschine", "sampler", "patterns" }
        },
        {
            "maze-voice", "Maze Voice", "sd88me", "v2.0.0", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-14",
            "5186", "6.4 MB", "14%",
            "sd88me - VST - Maze Voice", "maze_voice.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-maze/main/docs/img/maze-voice.png", "https://github.com/sd88me/mpc-vst-maze/releases/download/maze-voice-vst-v2.0.0/Maze-Voice-2.0.0-mpc-armv7.zip", "plugins/maze-voice/thumb.png",
            "Monophonic Moog Labyrinth-style thru-zero oscillator, wavefolder and state-variable filter voice with two LFOs, a page randomiser and a three-page touchscreen skin with Q-Links.", "./plugins/maze-voice/install.sh", 0,
            0,
            false,
            false,
            "",
            { "monophonic", "wavefolder", "moog", "analog-modelling" }
        },
        {
            "meowsynth", "MeowSynth", "mcardonaserrano", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-22",
            "1954", "4.5 MB", "14%",
            "knobster - VST - MeowSynth", "meowsynth.so",
            "https://raw.githubusercontent.com/mcardonaserrano/meowsynth-mpc/main/previews/meow_page_meowing.png", "https://github.com/mcardonaserrano/meowsynth-mpc/releases/download/v1.0.2/MeowSynth-1.0.2-mpc-armv7.zip", "plugins/meowsynth/thumb.png",
            "knobster's MeowSynth rebuilt natively from the original's SynthEdit patch: a ramp oscillator through an envelope-following resonant filter that meows, five cats, vibrato and chorus, with the original's look and a meowing cat on the touchscreen and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/knobster - VST - MeowSynth\" && ZIP_PATH=\"./plugins/meowsynth/meowsynth.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mcardonaserrano/meowsynth-mpc/releases/download/v1.0.2/MeowSynth-1.0.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/meowsynth\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mcardonaserrano/meowsynth-mpc/releases/download/v1.0.2/MeowSynth-1.0.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/knobster - VST - MeowSynth/\"; elif [ -f \"./plugins/meowsynth/meowsynth.so\" ]; then cp -rf ./plugins/meowsynth/* \"$TARGET_SYNTH_DIR/knobster - VST - MeowSynth/\"; else echo \"[ERROR] No local binary archive found for meowsynth\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "cat", "meow", "novelty", "synthedit" }
        },
        {
            "meta4mpcxxl", "Meta4MPCXXL", "L'Cronx", "v0.1.2", "Modular",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "849", "11.2 MB", "14%",
            "L'Cronx - VST - Meta4MPCXXL", "meta4mpcxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/Meta4MPCXXL/v0.1.1/docs/img/meta4mpcxxl.png", "https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-0.1.2-mpc-armv7.zip", "plugins/meta4mpcxxl/thumb.png",
            "Plays 4ms MetaModule-format patches (VCV Rack patches exported with the MetaModule hub) with 4ms's own open-source patch player and 215 built-in modules: every parameter on Q-Link pages, MIDI and the Force's clock into the patch, pads and keys to the hub's gate/CV inputs, patch outs 3-8 as MIDI for a CV track, and Wi-Fi patch upload from VCV Rack.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - Meta4MPCXXL\" && ZIP_PATH=\"./plugins/meta4mpcxxl/meta4mpcxxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-0.1.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/meta4mpcxxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-0.1.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - Meta4MPCXXL/\"; elif [ -f \"./plugins/meta4mpcxxl/meta4mpcxxl.so\" ]; then cp -rf ./plugins/meta4mpcxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - Meta4MPCXXL/\"; else echo \"[ERROR] No local binary archive found for meta4mpcxxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "modular", "vcv", "metamodule", "patch-player", "eurorack", "generative", "cv" }
        },
        {
            "jv-880", "Mini-JV", "sd88me", "v2.0.3", "Rompler",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "9699", "4.6 MB", "14%",
            "sd88me - VST - JV-880", "jv880.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-jv880/a38d32cbbe90dc8b209a392a0835236acdd8d95f/docs/screenshots/jv880.png", "https://github.com/sd88me/mpc-vst-jv880/releases/download/v2.0.3/JV-880-2.0.3-mpc-armv7.zip", "plugins/jv-880/thumb.png",
            "An emulation of the Roland JV-880 sample-based sound engine as a native instrument, with the original reverb and chorus, a Play page with eight macros and a touchscreen skin.", "./plugins/jv-880/install.sh", 0,
            0,
            false,
            false,
            "",
            { "roland", "emulation" }
        },
        {
            "elements", "Modal Voice", "saustin2010", "v1.2.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2786", "1.2 MB", "14%",
            "Émilie Gillet - VST - [SYN] Modal Voice", "elements.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-modal-voice/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-modal-voice/releases/download/v1.2.1/SYN-Modal-Voice-1.2.1-mpc-armv7.zip", "plugins/elements/thumb.png",
            "Modal Voice: a modal synthesis voice you bow, blow and strike, with 12 presets, from Émilie Gillet's open-source Elements.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Modal Voice\" && ZIP_PATH=\"./plugins/elements/elements.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-modal-voice/releases/download/v1.2.1/SYN-Modal-Voice-1.2.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/elements\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-modal-voice/releases/download/v1.2.1/SYN-Modal-Voice-1.2.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Modal Voice/\"; elif [ -f \"./plugins/elements/elements.so\" ]; then cp -rf ./plugins/elements/* \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Modal Voice/\"; else echo \"[ERROR] No local binary archive found for elements\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "modal", "physical-modelling", "eurorack" }
        },
        {
            "model-72", "Model-72", "sd88me", "v1.0.1", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-10-21",
            "2180", "31.6 MB", "14%",
            "sd88me - VST - Model-72", "model72.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-model72/main/docs/img/mixer_filter.png", "https://github.com/sd88me/mpc-vst-model72/releases/download/model72-v1.0.1/Model-72-1.0.1-mpc-armv7.zip", "plugins/model-72/thumb.png",
            "A port of Idle Foundry's CA-72, a circuit-modelled 1970s Minimoog Model-D: three oscillators, the ladder filter and contours, mono, poly or unison with stereo and 19 presets. LO is the fitted light model; HI runs the circuit's own filter in real time.", "./plugins/model-72/install.sh", 0,
            0,
            false,
            false,
            "",
            { "analog", "mono", "bass", "circuit-model", "stereo", "presets" }
        },
        {
            "monksynth", "MonkSynth", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1994", "1.6 MB", "14%",
            "Jonathan Taylor - VST - [SYN] MonkSynth", "monksynth.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-monksynth/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-monksynth/releases/download/v1.1.1/SYN-MonkSynth-1.1.1-mpc-armv7.zip", "plugins/monksynth/thumb.png",
            "A formant (FOF) singing voice with 12 characters and a choir.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Jonathan Taylor - VST - [SYN] MonkSynth\" && ZIP_PATH=\"./plugins/monksynth/monksynth.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-monksynth/releases/download/v1.1.1/SYN-MonkSynth-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/monksynth\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-monksynth/releases/download/v1.1.1/SYN-MonkSynth-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Jonathan Taylor - VST - [SYN] MonkSynth/\"; elif [ -f \"./plugins/monksynth/monksynth.so\" ]; then cp -rf ./plugins/monksynth/* \"$TARGET_SYNTH_DIR/Jonathan Taylor - VST - [SYN] MonkSynth/\"; else echo \"[ERROR] No local binary archive found for monksynth\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "formant", "voice", "choir" }
        },
        {
            "mono-voice", "Mono Voice", "saustin2010", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2121", "250 KB", "14%",
            "timncox - VST - [SYN] Mono Voice", "monovoice.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-monovoice/v1.0.2/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-monovoice/releases/download/v1.0.2/SYN-Mono-Voice-1.0.2-mpc-armv7.zip", "plugins/mono-voice/thumb.png",
            "Elektron Monomachine-style digital voice: SuperWave, SID, DigiPRO, FM and more machines; 12 factory patches.", "./plugins/mono-voice/install.sh", 0,
            0,
            false,
            false,
            "",
            { "monomachine", "sid", "fm", "digital" }
        },
        {
            "monomodule-one-gen2", "Monomodule One (Gen2)", "sd88me", "v1.0.4", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-04",
            "1435", "4.2 MB", "14%",
            "sd88me - VST - Monomodule One", "monomodule_one.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-monomodule/main/docs/img/monomodule-gen2.png", "https://github.com/sd88me/mpc-vst-monomodule/releases/download/v1.0.4/Monomodule-One-1.0.4-mpc-aarch64.zip", "plugins/monomodule-one-gen2/thumb.png",
            "The Elektron Monomachine's synth engine (15 machines) as an instrument for Gen2 MPC devices, with a touchscreen skin, banks and preset lists, and Q-Links. A ready-made download: nothing of Elektron's is in the zip; you put your own Monomachine OS file in the plugin folder and it makes its skin and sound from that. Gen2 (aarch64) only.", "./plugins/monomodule-one-gen2/install.sh", 0,
            0,
            false,
            false,
            "",
            { "elektron", "monomachine", "gen2" }
        },
        {
            "monomodule", "Monomodule One + FX", "sd88me", "v1.0.4", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-04",
            "100", "2.0 MB", "14%",
            "sd88me - VST - Monomodule One + FX", "monomodule.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-monomodule/main/docs/img/monomodule.png", "", "plugins/monomodule/thumb.png",
            "The Elektron Monomachine's synth engine (15 machines) as an instrument, plus its 7 effect machines as an audio effect, each with a touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Monomodule One + FX\" && ZIP_PATH=\"./plugins/monomodule/monomodule.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"\" ]; then mkdir -p \"./plugins/monomodule\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Monomodule One + FX/\"; elif [ -f \"./plugins/monomodule/monomodule.so\" ]; then cp -rf ./plugins/monomodule/* \"$TARGET_SYNTH_DIR/sd88me - VST - Monomodule One + FX/\"; else echo \"[ERROR] No local binary archive found for monomodule\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "elektron", "monomachine", "firmware-derived" }
        },
        {
            "monoslide", "MonoSlide", "Eurikon", "v1.0.5", "Synth",
            "INST", "Stable", "MPC Live II Retro (3.10.1)", "MPC OS 3.x", "2026-11-13",
            "1061", "4.4 MB", "11%",
            "Twisted Cable - VST - MonoSlide", "monoslide.so",
            "https://raw.githubusercontent.com/Eurikon/mpc-vst-plugins/0b9a58444c7d84a78e38d95818eeb53172195782/media/monoslide.png", "https://github.com/Eurikon/mpc-vst-plugins/releases/download/monoslide-v1.0.5/MonoSlide-1.0.5-mpc-armv7.zip", "plugins/monoslide/thumb.png",
            "Monophonic acid bass voice with overlapping-note slide, velocity accents and an MPC touchscreen skin.", "./plugins/monoslide/install.sh", 0,
            0,
            false,
            false,
            "",
            { "303", "acid", "bass", "monophonic" }
        },
        {
            "morpho-pe", "Morpho-PE", "sd88me", "v1.0.2", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "2565", "2.2 MB", "14%",
            "sd88me - VST - Morpho-PE", "morpho_pe.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-morpho-PE/main/docs/img/morpho-pe.png", "https://github.com/sd88me/mpc-vst-morpho-PE/releases/download/morpho-pe-v1.0.2/Morpho-PE-1.0.2-mpc-armv7.zip", "plugins/morpho-pe/thumb.png",
            "Four-voice synthesizer modelled on the Poly Evolver's voice: analog-style and digital waveshape oscillators, 2/4-pole lowpass, delay, feedback, distortion, 4 x 16 step sequencer, a bank browser and a touchscreen skin with Q-Links. Loads Poly Evolver program dumps and, from your own Prophet VS ROM chip images, the 95 factory waves.", "./plugins/morpho-pe/install.sh", 0,
            0,
            false,
            false,
            "",
            { "analog-modelling", "polyphonic", "sequencer", "evolver" }
        },
        {
            "mpc-plaits", "MPC Plaits", "poloq", "v1.0.1", "Synth",
            "INST", "Stable", "Akai MPC One (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-05",
            "3655", "5.5 MB", "24%",
            "poloq - VST - MPC Plaits", "plaits.so",
            "https://raw.githubusercontent.com/poloq-instruments/mpc-vst-plaits/a30c24b83663a5efead73bbaca674ba4502a3eb5/docs/screenshots/card.png", "https://github.com/poloq-instruments/mpc-vst-plaits/releases/download/v1.0.1/MPC-Plaits-1.0.1-mpc-armv7.zip", "plugins/mpc-plaits/thumb.png",
            "Mutable Instruments Plaits, all 24 models, with LFOs, envelopes, two mod matrices, tempo sync and a touchscreen skin.", "./plugins/mpc-plaits/install.sh", 0,
            0,
            false,
            false,
            "",
            { "mutable-instruments", "plaits", "macro-oscillator", "eurorack" }
        },
        {
            "mr-drums", "Mr Drums", "saustin2010", "v1.1.1", "Sampler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1815", "464 KB", "14%",
            "Move Everything - VST - [DRUM] Mr Drums", "mrdrums.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-mrdrums/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-mrdrums/releases/download/v1.1.1/DRUM-Mr-Drums-1.1.1-mpc-armv7.zip", "plugins/mr-drums/thumb.png",
            "A 16-pad drum sampler: kits of samples on pads 36-51.", "./plugins/mr-drums/install.sh", 0,
            0,
            false,
            false,
            "",
            { "drums", "kits", "pads" }
        },
        {
            "mr-hyde", "Mr Hyde", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2615", "1.9 MB", "14%",
            "Move Everything - VST - [SYN] Mr Hyde", "mrhyde.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-mrhyde/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-mrhyde/releases/download/v1.1.1/SYN-Mr-Hyde-1.1.1-mpc-armv7.zip", "plugins/mr-hyde/thumb.png",
            "MicroFreak-style voice on Plaits' 17 models, with a low-pass gate, filter, cycling envelope and a 6x6 mod matrix; 19 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Move Everything - VST - [SYN] Mr Hyde\" && ZIP_PATH=\"./plugins/mr-hyde/mr-hyde.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-mrhyde/releases/download/v1.1.1/SYN-Mr-Hyde-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/mr-hyde\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-mrhyde/releases/download/v1.1.1/SYN-Mr-Hyde-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Move Everything - VST - [SYN] Mr Hyde/\"; elif [ -f \"./plugins/mr-hyde/mrhyde.so\" ]; then cp -rf ./plugins/mr-hyde/* \"$TARGET_SYNTH_DIR/Move Everything - VST - [SYN] Mr Hyde/\"; else echo \"[ERROR] No local binary archive found for mr-hyde\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "microfreak", "macro-oscillator", "mod-matrix", "low-pass-gate" }
        },
        {
            "ms20xxl", "MS20XXL", "L'Cronx", "v0.1.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-27",
            "1639", "19.3 MB", "14%",
            "L'Cronx - VST - MS20XXL", "ms20xxl.so",
            "https://raw.githubusercontent.com/sunskiefer/MS20XXL/v0.1.1/docs/img/patch.png", "https://github.com/sunskiefer/MS20XXL/releases/download/v0.1.2/MS20XXL-0.1.2-mpc-armv7.zip", "plugins/ms20xxl/thumb.png",
            "Korg MS-20-style semi-modular mono synth, circuit-modelled from the MS-20 schematics: the KLM-307 (MK2) and KORG35 (MK1) filter boards solved from their circuit equations, Hz/V sockets, the panel's voltage laws. Every MS-20 patch jack in its place, normalled like the hardware, patched by touch; an SQ-10-style sequencer; 63 patches with cables.", "./plugins/ms20xxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "ms-20", "semi-modular", "mono", "patch-panel", "sequencer", "sq-10", "korg35", "analog" }
        },
        {
            "ms2kr", "MS2KR", "sd88me", "v0.1.0", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-15",
            "89", "1.8 MB", "14%",
            "sd88me - VST - MS2KR", "ms2kr.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-ms2kr/v0.1.0/docs/screenshots/play.png", "https://github.com/sd88me/mpc-vst-ms2kr/releases/download/v0.1.0/MS2KR-0.1.0-mpc-aarch64.zip", "plugins/ms2kr/thumb.png",
            "An emulation of the MS2000R analog-modeling synth's own firmware (ms2000r-emu) as a native instrument for Gen2 MPC devices, with a six-page touchscreen skin laid out like the front panel, live LCD, favourites and Q-Link control. Gen2 (aarch64) only; you need your own ROM dumps.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - MS2KR\" && ZIP_PATH=\"./plugins/ms2kr/ms2kr.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-ms2kr/releases/download/v0.1.0/MS2KR-0.1.0-mpc-aarch64.zip\" ]; then mkdir -p \"./plugins/ms2kr\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-ms2kr/releases/download/v0.1.0/MS2KR-0.1.0-mpc-aarch64.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - MS2KR/\"; elif [ -f \"./plugins/ms2kr/ms2kr.so\" ]; then cp -rf ./plugins/ms2kr/* \"$TARGET_SYNTH_DIR/sd88me - VST - MS2KR/\"; else echo \"[ERROR] No local binary archive found for ms2kr\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "emulation", "virtual-analog", "vocoder", "gen2" }
        },
        {
            "mutable-vibe", "Mutable Vibe MPC", "nachtaktiv303", "v0.9.3", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3610", "1.1 MB", "14%",
            "nachtaktiv303 - VST - Mutable Vibe MPC", "mutable_vibe_mpc.so",
            "https://raw.githubusercontent.com/nachtaktiv303/mpc-vst-mutable-vibe/main/preview_0.png", "https://github.com/nachtaktiv303/mpc-vst-mutable-vibe/releases/download/mutable-vibe-vst-v0.9.3/Mutable-Vibe-MPC-0.9.3-mpc-armv7.zip", "plugins/mutable-vibe/thumb.png",
            "Mutable Instruments Rings + Plaits in one synth: 6 resonator models and 14 melodic macro-oscillator engines, with filter, FX, 4 envelopes, 4 LFOs, a mod matrix and tempo sync, plus a touchscreen skin and Q-Links.", "./plugins/mutable-vibe/install.sh", 0,
            0,
            false,
            false,
            "",
            { "mutable-instruments", "rings", "plaits", "resonator", "macro-oscillator", "eurorack" }
        },
        {
            "nodalred2x", "NodalRed2x", "sd88me", "v0.1.0", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-15",
            "96", "2.3 MB", "14%",
            "sd88me - VST - NodalRed2x", "nodalred2x.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-nodalred2x/v0.1.0/docs/screenshots/main.png", "https://github.com/sd88me/mpc-vst-nodalred2x/releases/download/v0.1.0/NodalRed2x-0.1.0-mpc-aarch64.zip", "plugins/nodalred2x/thumb.png",
            "An emulation of the Clavia Nord Lead 2x's original firmware (gearmulator) as a native instrument for Gen2 MPC devices, with a touchscreen skin, a starter bank plus your own sound sets, favourites and Q-Link control. Gen2 (aarch64) only; you need your own Nord Lead 2x ROM.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - NodalRed2x\" && ZIP_PATH=\"./plugins/nodalred2x/nodalred2x.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-nodalred2x/releases/download/v0.1.0/NodalRed2x-0.1.0-mpc-aarch64.zip\" ]; then mkdir -p \"./plugins/nodalred2x\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-nodalred2x/releases/download/v0.1.0/NodalRed2x-0.1.0-mpc-aarch64.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - NodalRed2x/\"; elif [ -f \"./plugins/nodalred2x/nodalred2x.so\" ]; then cp -rf ./plugins/nodalred2x/* \"$TARGET_SYNTH_DIR/sd88me - VST - NodalRed2x/\"; else echo \"[ERROR] No local binary archive found for nodalred2x\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "emulation", "virtual-analog", "rompler-free", "gen2" }
        },
        {
            "noisemaker", "Noisemaker", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2014", "5.3 MB", "14%",
            "TAL - VST - [SYN] Noisemaker", "noisemaker.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-noisemaker/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-noisemaker/releases/download/v1.1.1/SYN-Noisemaker-1.1.1-mpc-armv7.zip", "plugins/noisemaker/thumb.png",
            "TAL-NoiseMaker: a virtual-analog polysynth with 12 filter types, three envelopes and effects; all 256 factory presets built in, and folders of .noisemakerpreset files load as banks (the README links a free collection of 442).", "./plugins/noisemaker/install.sh", 0,
            0,
            false,
            false,
            "",
            { "tal", "virtual-analog", "polyphonic", "subtractive" }
        },
        {
            "nova-editor", "Nova Editor", "3dthc", "v0.7.1", "Editor",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-13",
            "413", "1.7 MB", "14%",
            "3dthc - VST - Nova Editor", "nova_editor.so",
            "https://raw.githubusercontent.com/3dthc/nova-editor/main/nova-editor-main.png", "https://github.com/3dthc/Nova-Editor/releases/download/nova-editor-v0.7.1/Nova-Editor-0.7.1-mpc-armv7.zip", "plugins/nova-editor/thumb.png",
            "An editor and librarian for the Novation MiniNova as a native plugin, with its own MPC screen skin: every control maps to a MiniNova CC/NRPN, controls follow the synth, banks A-C and imported .syx files live on the SD card. Also a SysEx librarian for any other synth. With nova-audio loaded, the MiniNova plays on the plugin's own track.", "./plugins/nova-editor/install.sh", 0,
            0,
            false,
            false,
            "",
            { "librarian", "sysex", "mininova", "novation", "midi" }
        },
        {
            "nusaw", "NuSaw", "saustin2010", "v1.0.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2090", "2.2 MB", "14%",
            "Charles Vestal - VST - [SYN] NuSaw", "nusaw.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-nusaw/v1.0.2/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-nusaw/releases/download/v1.0.2/SYN-NuSaw-1.0.2-mpc-armv7.zip", "plugins/nusaw/thumb.png",
            "Detuned supersaw polysynth with a sub, resonant filter, chorus and delay; 27 presets for pads, stabs and leads.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] NuSaw\" && ZIP_PATH=\"./plugins/nusaw/nusaw.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-nusaw/releases/download/v1.0.2/SYN-NuSaw-1.0.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/nusaw\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-nusaw/releases/download/v1.0.2/SYN-NuSaw-1.0.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] NuSaw/\"; elif [ -f \"./plugins/nusaw/nusaw.so\" ]; then cp -rf ./plugins/nusaw/* \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] NuSaw/\"; else echo \"[ERROR] No local binary archive found for nusaw\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "supersaw", "trance", "polyphonic", "pads" }
        },
        {
            "obxd", "OB-Xd", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2652", "1.5 MB", "14%",
            "reales - VST - [SYN] OB-Xd", "obxd.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-obxd/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-obxd/releases/download/v1.1.1/SYN-OB-Xd-1.1.1-mpc-armv7.zip", "plugins/obxd/thumb.png",
            "OB-Xd, the Oberheim OB-X emulation: two oscillators with sync and cross-mod, a 12/24 dB filter, voice variation; 128 presets.", "./plugins/obxd/install.sh", 0,
            0,
            false,
            false,
            "",
            { "ob-x", "analog-modelling", "polyphonic", "subtractive" }
        },
        {
            "obxdmpc", "OB-Xd MPC", "3dthc", "v0.4.9", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-15",
            "3934", "14.9 MB", "14%",
            "sd88me - VST - OB-Xd", "obxd.so",
            "https://raw.githubusercontent.com/3dthc/OB-XD-MPC/c564dfd0c0966d92b29365819d754b8cf75bc616/docs/screenshots/obxd.png", "https://github.com/3dthc/OB-XD-MPC/releases/download/ob-xd-mpc-0.4.9/OB-Xd-0.4.9-mpc-armv7.zip", "plugins/obxdmpc/thumb.png",
            "OB-Xd, the Oberheim OB-X emulation: two oscillators with sync and cross-mod, a 12/24 dB filter, voice variation; 128 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - OB-Xd\" && ZIP_PATH=\"./plugins/obxdmpc/obxdmpc.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/3dthc/OB-XD-MPC/releases/download/ob-xd-mpc-0.4.9/OB-Xd-0.4.9-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/obxdmpc\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/OB-XD-MPC/releases/download/ob-xd-mpc-0.4.9/OB-Xd-0.4.9-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - OB-Xd/\"; elif [ -f \"./plugins/obxdmpc/obxd.so\" ]; then cp -rf ./plugins/obxdmpc/* \"$TARGET_SYNTH_DIR/sd88me - VST - OB-Xd/\"; else echo \"[ERROR] No local binary archive found for obxdmpc\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "ob-x", "analog-modelling", "polyphonic", "subtractive" }
        },
        {
            "odin2", "Odin 2", "3dthc", "v1.4.9", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-13",
            "3561", "23.2 MB", "14%",
            "3dthc - VST - Odin 2", "odin2.so",
            "https://raw.githubusercontent.com/3dthc/Odin2-MPC/main/odin2-mpc-main.png", "https://github.com/3dthc/Odin2-MPC/releases/download/odin2-v1.4.9/Odin-2-1.4.9-mpc-armv7.zip", "plugins/odin2/thumb.png",
            "Odin 2 synthesizer ported to standalone Akai MPC devices as a native VST2 plugin. Features Odin's original engine, 261 factory patches, touchscreen pages, automatic voice management and a CPU meter. Tested on an Akai MPC Live II; unofficial port.", "./plugins/odin2/install.sh", 0,
            0,
            false,
            false,
            "",
            {  }
        },
        {
            "omni-sampler", "Omni Sampler", "MrHighMan", "v1.8.1", "Sampler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-02",
            "1671", "5.0 MB", "20%",
            "MR HighMan - VST - Omni Sampler", "omni_sampler.so",
            "https://raw.githubusercontent.com/B0ss-M/MrHighMan/main/docs/screenshot.png", "https://github.com/B0ss-M/MrHighMan/releases/download/v1.8.1/Omni-Sampler-1.8.1-mpc-armv7.zip", "plugins/omni-sampler/thumb.png",
            "Multi-format sampler: plays Akai, E-mu, Roland, Kontakt/Maschine, SF2 and SFZ instruments straight from their files or sampler disk images, with layers, keyswitching, a mod matrix and its own screen skin.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/MR HighMan - VST - Omni Sampler\" && ZIP_PATH=\"./plugins/omni-sampler/omni-sampler.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/B0ss-M/MrHighMan/releases/download/v1.8.1/Omni-Sampler-1.8.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/omni-sampler\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/B0ss-M/MrHighMan/releases/download/v1.8.1/Omni-Sampler-1.8.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/MR HighMan - VST - Omni Sampler/\"; elif [ -f \"./plugins/omni-sampler/omni_sampler.so\" ]; then cp -rf ./plugins/omni-sampler/* \"$TARGET_SYNTH_DIR/MR HighMan - VST - Omni Sampler/\"; else echo \"[ERROR] No local binary archive found for omni-sampler\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "akai", "emu", "roland", "kontakt", "sf2", "sfz", "disk-images" }
        },
        {
            "plugin-manager-g2", "Open MPC Plugin Manager", "sd88me", "v1.2.0", "Utility",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-14",
            "3891", "25.2 MB", "14%",
            "OpenMPC - VST - Plugin Manager", "openmpc_pm_g2.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/588466d/tools/plugin_manager_g2/docs/screenshots/manager-discover-light.png", "https://github.com/open-mpc/plugin-manager/releases/download/plugin-manager-g2-v1.2.0/Plugin-Manager-G2-1.2.0-mpc-armv7.zip", "plugins/plugin-manager-g2/thumb.png",
            "The official OpenMPC plugin manager for Gen1 and Gen2 MPCs (32-bit build for Force, Live, One, X, Key; 64-bit for the Live III and kin). Browse this catalog and install, update and remove plugins on the MPC itself, with rollback if an install fails. Built on poloq's mpc-vst-manager.", "./plugins/plugin-manager-g2/install.sh", 0,
            0,
            false,
            false,
            "",
            { "installer", "catalog", "manager", "gen1", "gen2", "utility" }
        },
        {
            "open303", "Open303", "saustin2010", "v1.2.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3261", "2.0 MB", "14%",
            "Robin Schmidt - VST - [SYN] Open303", "acid303.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-open303/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-open303/releases/download/v1.2.1/SYN-Open303-1.2.1-mpc-armv7.zip", "plugins/open303/thumb.png",
            "TB-303 bass line: Open303 with the Devilfish mods, a drive stage and 13 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Robin Schmidt - VST - [SYN] Open303\" && ZIP_PATH=\"./plugins/open303/open303.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-open303/releases/download/v1.2.1/SYN-Open303-1.2.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/open303\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-open303/releases/download/v1.2.1/SYN-Open303-1.2.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Robin Schmidt - VST - [SYN] Open303/\"; elif [ -f \"./plugins/open303/acid303.so\" ]; then cp -rf ./plugins/open303/* \"$TARGET_SYNTH_DIR/Robin Schmidt - VST - [SYN] Open303/\"; else echo \"[ERROR] No local binary archive found for open303\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "tb-303", "acid", "bass", "monophonic" }
        },
        {
            "osirus", "Osirus", "sd88me", "v0.2.10", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-03",
            "2425", "4.3 MB", "14%",
            "sd88me - VST - Osirus", "osirus.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-osirus/e39d03c538f4fde79d558cd4c423b18964cc5ffc/docs/screenshots/osirus-osc.png", "https://github.com/sd88me/mpc-vst-osirus/releases/download/v0.2.10/Osirus-0.2.10-mpc-aarch64.zip", "plugins/osirus/thumb.png",
            "An emulation of the Access Virus A, B and C synthesizers' original firmware (gearmulator's DSP56300 emulator) as a native instrument for Gen2 MPC devices, with a touchscreen skin, banks and patch browser, Q-Link control and knob values on every page. Gen2 (aarch64) only; you need your own Virus ROM.", "./plugins/osirus/install.sh", 0,
            0,
            false,
            false,
            "",
            { "emulation", "virtual-analog", "rompler-free", "gen2" }
        },
        {
            "oxzygen", "OxZygen", "3dthc", "v0.7.6", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-15",
            "1234", "158.5 MB", "14%",
            "oxzygen - VST - OxZygen", "oxzygen.so",
            "https://raw.githubusercontent.com/3dthc/OxZygen/b7640ca5971ef1371742bb2fad4ffc74a534b2a2/docs/img/player.png", "https://github.com/3dthc/OxZygen/releases/download/oxzygen-v0.7.6/OxZygen-0.7.6-mpc-armv7.zip", "plugins/oxzygen/thumb.png",
            "A dark, cinematic instrument: choirs, sung doos, flutes, reeds, brass, atmospheric strings, bells, Juno-style synths, wind and textures from CC0 recordings, in two layered slots with filter, lo-fi, delay, reverb and sub. 96 presets; a CRT crystal-ball orb reacts as you play. Gen1 (MPC OS 3.x, plus an OS 2.x zip) and Gen2.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/oxzygen - VST - OxZygen\" && ZIP_PATH=\"./plugins/oxzygen/oxzygen.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/3dthc/OxZygen/releases/download/oxzygen-v0.7.6/OxZygen-0.7.6-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/oxzygen\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/OxZygen/releases/download/oxzygen-v0.7.6/OxZygen-0.7.6-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/oxzygen - VST - OxZygen/\"; elif [ -f \"./plugins/oxzygen/oxzygen.so\" ]; then cp -rf ./plugins/oxzygen/* \"$TARGET_SYNTH_DIR/oxzygen - VST - OxZygen/\"; else echo \"[ERROR] No local binary archive found for oxzygen\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "choir", "vocal", "cinematic", "ambient", "winds", "brass", "bells", "lo-fi" }
        },
        {
            "percolator", "Percolator", "saustin2010", "v2.1.0", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3869", "6.6 MB", "14%",
            "Steve A - VST - [DRUM] Percolator", "percolator.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-percolator/v2.1.0/screenshots/page_1.png", "https://github.com/saustin2010/mpc-vst-percolator/releases/download/v2.1.0/DRUM-Percolator-2.1.0-mpc-armv7.zip", "plugins/percolator/thumb.png",
            "Four-voice percussion synth laid out like the Erica Synths Pērkons HD-01 (three algorithms per voice, BBD delay, LFO to every knob), with envelope displays; each voice on its own MIDI channel with the Pērkons' CCs, and on its own track with Percolator OUT; plays the Pērkons' free kit packs.", "./plugins/percolator/install.sh", 0,
            0,
            false,
            false,
            "",
            { "drums", "percussion", "kits", "drone", "envelope-display" }
        },
        {
            "op1fx-phase", "Phase", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "611", "7.5 MB", "14%",
            "bombest - VST - Phase", "phase.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/phase.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Phase-0.4.1-mpc-armv7.zip", "plugins/op1fx-phase/thumb.png",
            "Eight-voice phase-distortion synth, designed from the OP-1 screen.", "./plugins/op1fx-phase/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "synth", "phase-distortion" }
        },
        {
            "polywerk", "PolyWerk", "Devko", "v0.0.6", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "765", "6.3 MB", "14%",
            "Devko - VST - PolyWerk", "polywerk.so",
            "https://raw.githubusercontent.com/Devko/PolyWerk/main/docs/img/waves.png", "https://github.com/Devko/PolyWerk/releases/download/v0.0.6/PolyWerk-0.0.6-mpc-armv7.zip", "plugins/polywerk/thumb.png",
            "Eight-voice wavetable synth: two wavetable oscillators with 8x unison and subs, 30 built-in tables plus your own Serum-format WAVs, two filters with comb and vowel, two LFOs, a 12-slot mod matrix, arpeggiator and sequencers, 205 presets and a seven-tab touchscreen skin with Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Devko - VST - PolyWerk\" && ZIP_PATH=\"./plugins/polywerk/polywerk.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Devko/PolyWerk/releases/download/v0.0.6/PolyWerk-0.0.6-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/polywerk\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Devko/PolyWerk/releases/download/v0.0.6/PolyWerk-0.0.6-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Devko - VST - PolyWerk/\"; elif [ -f \"./plugins/polywerk/polywerk.so\" ]; then cp -rf ./plugins/polywerk/* \"$TARGET_SYNTH_DIR/Devko - VST - PolyWerk/\"; else echo \"[ERROR] No local binary archive found for polywerk\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "wavetable", "polyphonic", "arpeggiator", "sequencer", "mod-matrix" }
        },
        {
            "potential1200", "Potential1200", "PotentialBreaks", "v1.0.2", "Sampler",
            "INST", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "84", "4.5 MB", "7%",
            "PotentialBreaks - VST - Potential1200", "potential1200.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potential1200/65bd97f44fd315c321e58f92ce368d33636c18a9/docs/images/potential1200.png", "https://github.com/PotentialBreakfast/mpc-vst-potential1200/releases/download/v1.0.2/Potential1200-1.0.2-mpc-armv7.zip", "plugins/potential1200/thumb.png",
            "12-bit drum sampler in the late-80s style: 8 faders and 8 pads, one-shots from your folders or a break cut to 2 bars from a kick on the 1, taken at 26 kHz into 12 bits with drop-sample pitch, the 45 > 33 trick, the classic per-pad filters and MULTI PITCH. Saves an MPC drum program. Four looks, its own Vintage look first.", "./plugins/potential1200/install.sh", 0,
            0,
            false,
            false,
            "",
            { "sampler", "12-bit", "lo-fi", "drums", "breaks", "vintage" }
        },
        {
            "potentialbreaks", "PotentialBreaks", "PotentialBreaks", "v1.8.0", "Sampler",
            "INST", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "1464", "4.1 MB", "2%",
            "PotentialBreaks - VST - PotentialBreaks", "potentialbreaks.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialbreaks/8d94a8c800a149c068b13b056e62369e6449cbc0/docs/images/potentialbreaks.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialbreaks/releases/download/v1.8.0/PotentialBreaks-1.8.0-mpc-armv7.zip", "plugins/potentialbreaks/thumb.png",
            "Drum-break slicer: picks a random break from your own break folders (WAV or FLAC), chops it to 8 or 16 slices or by hits across the pads and matches it to the project tempo, by re-pitching or stretching. Shuffle the slices and save the groove as MIDI.", "./plugins/potentialbreaks/install.sh", 0,
            0,
            false,
            false,
            "",
            { "breaks", "slicer", "drums", "chop" }
        },
        {
            "potentialcrates", "PotentialCrates", "PotentialBreaks", "v1.2.0", "Sample Browser",
            "INST", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "432", "68.6 MB", "1%",
            "PotentialBreaks - VST - PotentialCrates", "potentialcrates.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialcrates/a299c15321ad23c77343baaeb0c25e851e6e74f9/docs/images/potentialcrates.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialcrates/releases/download/v1.2.0/PotentialCrates-1.2.0-mpc-armv7.zip", "plugins/potentialcrates/thumb.png",
            "Record digger: one-tap crates (Drum Breaks, Library, Soundtracks, ...) and ROLL dice dig Discogs and stream a record into your track to sample. A PotentialSuite fork of Crate Digger.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialCrates\" && ZIP_PATH=\"./plugins/potentialcrates/potentialcrates.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/PotentialBreakfast/mpc-vst-potentialcrates/releases/download/v1.2.0/PotentialCrates-1.2.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/potentialcrates\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/PotentialBreakfast/mpc-vst-potentialcrates/releases/download/v1.2.0/PotentialCrates-1.2.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialCrates/\"; elif [ -f \"./plugins/potentialcrates/potentialcrates.so\" ]; then cp -rf ./plugins/potentialcrates/* \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialCrates/\"; else echo \"[ERROR] No local binary archive found for potentialcrates\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "crate-digging", "discogs", "youtube", "sampling" }
        },
        {
            "potentialkits", "PotentialKits", "PotentialBreaks", "v1.2.0", "Sampler",
            "INST", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "133", "4.4 MB", "3%",
            "PotentialBreaks - VST - PotentialKits", "potentialkits.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialkits/aa460ad13569f7f01928d0ed9167c58a665cba26/docs/images/potentialkits.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialkits/releases/download/v1.2.0/PotentialKits-1.2.0-mpc-armv7.zip", "plugins/potentialkits/thumb.png",
            "Random drum-kit builder: 16 sounds from your own sample folders, one-tap KIT STYLES and ROLL dice, pitch, decay and choke per pad, USER kits, exported as an MPC drum program. A PotentialSuite fork of Lucky Dip.", "./plugins/potentialkits/install.sh", 0,
            0,
            false,
            false,
            "",
            { "sampler", "kit-builder", "random", "drum-kit" }
        },
        {
            "potentialnoise", "PotentialNoise", "PotentialBreaks", "v2.5.1", "Utility",
            "INST", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "920", "1.9 MB", "2%",
            "PotentialBreaks - VST - PotentialNoise", "potentialnoise.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialnoise/3c9b81922b9e044af69123dcdfe72f06011ed4e5/docs/images/potentialnoise.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialnoise/releases/download/v2.5.1/PotentialNoise-2.5.1-mpc-armv7.zip", "plugins/potentialnoise/thumb.png",
            "Calibrated pink noise reference for the pink-noise mixing method: one-tap RMS levels from -24 to -12 dB, mono or stereo, full or 40 Hz-16 kHz range, live RMS and peak reading and a level meter.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialNoise\" && ZIP_PATH=\"./plugins/potentialnoise/potentialnoise.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/PotentialBreakfast/mpc-vst-potentialnoise/releases/download/v2.5.1/PotentialNoise-2.5.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/potentialnoise\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/PotentialBreakfast/mpc-vst-potentialnoise/releases/download/v2.5.1/PotentialNoise-2.5.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialNoise/\"; elif [ -f \"./plugins/potentialnoise/potentialnoise.so\" ]; then cp -rf ./plugins/potentialnoise/* \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialNoise/\"; else echo \"[ERROR] No local binary archive found for potentialnoise\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "pink-noise", "mixing", "reference", "meter" }
        },
        {
            "potentialwax", "PotentialWax", "PotentialBreaks", "v1.9.0", "Sampler",
            "INST", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "1412", "4.9 MB", "6%",
            "PotentialBreaks - VST - PotentialWax", "potentialwax.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialwax/7daecab7e3363545c8642fdfbf186ace5ab10366/docs/images/potentialwax.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialwax/releases/download/v1.9.0/PotentialWax-1.9.0-mpc-armv7.zip", "plugins/potentialwax/thumb.png",
            "Melodic sampler in the spirit of Serato Sample: picks or browses songs from your own folders (MP3, WAV or FLAC), detects tempo and key, sets 16 cue points (phrases, bars or hits) on the pads and matches them to your tempo and key, with time-stretch or vinyl-style pitching. Save the chops as WAVs.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialWax\" && ZIP_PATH=\"./plugins/potentialwax/potentialwax.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/PotentialBreakfast/mpc-vst-potentialwax/releases/download/v1.9.0/PotentialWax-1.9.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/potentialwax\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/PotentialBreakfast/mpc-vst-potentialwax/releases/download/v1.9.0/PotentialWax-1.9.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialWax/\"; elif [ -f \"./plugins/potentialwax/potentialwax.so\" ]; then cp -rf ./plugins/potentialwax/* \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialWax/\"; else echo \"[ERROR] No local binary archive found for potentialwax\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "chop", "melodic", "sampler", "time-stretch", "key-detection" }
        },
        {
            "prloguexxl", "PrLogueXXL", "L'Cronx", "v0.1.1", "Synth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-23",
            "1485", "18.0 MB", "14%",
            "L'Cronx - VST - PrLogueXXL", "prloguexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/PrLogueXXL/v0.1.1/docs/img/osc.png", "https://github.com/sunskiefer/PrLogueXXL/releases/download/v0.1.1/PrLogueXXL-0.1.1-mpc-armv7.zip", "plugins/prloguexxl/thumb.png",
            "16-voice poly synth modelled on the KORG prologue-16 (not affiliated with KORG): two VCOs, a multi engine (noise, 16 VPM types, KORG's logue-sdk waves oscillator), Poly/Duo/Mono/Unison/Chord, Main and Sub timbres, arpeggiator on MPC's grid, mod FX, delay/reverb and a low-frequency compressor with a live meter, over four pages.", "./plugins/prloguexxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "synth", "poly", "analog-modelled", "vpm", "fm", "arpeggiator", "pad", "lead" }
        },
        {
            "profit-08", "Profit-08", "sd88me", "v2.0.1", "Synth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-11-13",
            "4291", "4.8 MB", "14%",
            "sd88me - VST - Profit-08", "profit_08.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-profit8/main/docs/img/screenshot-program.png", "https://github.com/sd88me/mpc-vst-profit8/releases/download/profit-08-v2.0.1/Profit-08-2.0.1-mpc-armv7.zip", "plugins/profit-08/thumb.png",
            "Eight-voice, two-layer analog-style synthesizer: stack and split layers, a gated sequencer and arpeggiator, a resonant 2/4-pole filter, and a banks browser with favourites for your own program and bank SysEx dumps. 2.0 adds a native MPC screen with large knobs and Q-Link control of every page.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Profit-08\" && ZIP_PATH=\"./plugins/profit-08/profit-08.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-profit8/releases/download/profit-08-v2.0.1/Profit-08-2.0.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/profit-08\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-profit8/releases/download/profit-08-v2.0.1/Profit-08-2.0.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Profit-08/\"; elif [ -f \"./plugins/profit-08/profit_08.so\" ]; then cp -rf ./plugins/profit-08/* \"$TARGET_SYNTH_DIR/sd88me - VST - Profit-08/\"; else echo \"[ERROR] No local binary archive found for profit-08\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "analog", "poly", "layers", "sequencer", "favourites", "q-links" }
        },
        {
            "profit-5", "Profit-5", "sd88me", "v2.0.0", "Synth",
            "INST", "Stable", "Akai MPC Live III (Gen2) (3.10), Akai Force (Gen1) (3.9.1)", "MPC OS 3.x", "2026-11-13",
            "2412", "17.8 MB", "14%",
            "sd88me - VST - Profit-5", "profit_5.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-profit5/5724d1fe8fdb0aab2c72837228a7fd77f1980ec1/docs/img/screenshot-osc.png", "https://github.com/sd88me/mpc-vst-profit5/releases/download/profit-5-vst-v2.0.0/Profit-5-2.0.0-mpc-armv7.zip", "plugins/profit-5/thumb.png",
            "Five-voice analog polysynth modelled on an early SSM-chip classic: two oscillators, a four-pole resonant filter, Poly-Mod, wheel-mod, glide and unison, 38 factory programs, a Rev 2 / Rev 4 voicing switch, and an importer for Repro-5 .h2p patches.", "./plugins/profit-5/install.sh", 0,
            0,
            false,
            false,
            "",
            { "analog", "poly", "vintage", "poly-mod" }
        },
        {
            "ps3100xxl", "PS3100XXL", "L'Cronx", "v0.1.5", "Synth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-23",
            "1395", "15.9 MB", "14%",
            "L'Cronx - VST - PS3100XXL", "ps3100xxl.so",
            "https://raw.githubusercontent.com/sunskiefer/PS3100XXL/v0.1.5/docs/img/overview.png", "https://github.com/sunskiefer/PS3100XXL/releases/download/v0.1.5/PS3100XXL-0.1.5-mpc-armv7.zip", "plugins/ps3100xxl/thumb.png",
            "Semi-modular poly synth modelled on the 1977 Korg PS-3100 (not affiliated with Korg): one oscillator, filter and envelope per key up to 48 voices, three-peak resonators, ensemble, two modulation generators, sample & hold, twelve temperament knobs and a patch bay you patch by touch, on one zoomable page.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - PS3100XXL\" && ZIP_PATH=\"./plugins/ps3100xxl/ps3100xxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/PS3100XXL/releases/download/v0.1.5/PS3100XXL-0.1.5-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/ps3100xxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/PS3100XXL/releases/download/v0.1.5/PS3100XXL-0.1.5-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - PS3100XXL/\"; elif [ -f \"./plugins/ps3100xxl/ps3100xxl.so\" ]; then cp -rf ./plugins/ps3100xxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - PS3100XXL/\"; else echo \"[ERROR] No local binary archive found for ps3100xxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "synth", "poly", "semi-modular", "analog-modelled", "patch-bay", "ensemble", "resonator" }
        },
        {
            "op1fx-pulse", "Pulse", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "591", "5.1 MB", "14%",
            "bombest - VST - Pulse", "pulse.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/pulse.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Pulse-0.4.1-mpc-armv7.zip", "plugins/op1fx-pulse/thumb.png",
            "Two PWM pulse oscillators per voice, eight voices. OP-1 inspired.", "./plugins/op1fx-pulse/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "synth", "pwm", "pulse" }
        },
        {
            "raffosynth", "RaffoSynth", "saustin2010", "v1.3.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2981", "6.4 MB", "14%",
            "Raffo - VST - [SYN] RaffoSynth", "moog.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-raffosynth/v1.3.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-raffosynth/releases/download/v1.3.1/SYN-RaffoSynth-1.3.1-mpc-armv7.zip", "plugins/raffosynth/thumb.png",
            "RaffoSynth: a Minimoog-style mono synth with four oscillators and a ladder filter.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Raffo - VST - [SYN] RaffoSynth\" && ZIP_PATH=\"./plugins/raffosynth/raffosynth.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-raffosynth/releases/download/v1.3.1/SYN-RaffoSynth-1.3.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/raffosynth\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-raffosynth/releases/download/v1.3.1/SYN-RaffoSynth-1.3.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Raffo - VST - [SYN] RaffoSynth/\"; elif [ -f \"./plugins/raffosynth/moog.so\" ]; then cp -rf ./plugins/raffosynth/* \"$TARGET_SYNTH_DIR/Raffo - VST - [SYN] RaffoSynth/\"; else echo \"[ERROR] No local binary archive found for raffosynth\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "minimoog", "monophonic", "ladder-filter", "analog-modelling" }
        },
        {
            "remixxxl", "reMixXXL", "L'Cronx", "v0.1.25", "Dj",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-08",
            "757", "27.3 MB", "14%",
            "L'Cronx - VST - reMixXXL", "remixxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/reMixXXL/v0.1.25/docs/img/wheel.png", "https://github.com/sunskiefer/reMixXXL/releases/download/v0.1.25/reMixXXL-0.1.25-mpc-armv7.zip", "plugins/remixxxl/thumb.png",
            "Four Mixxx-engine DJ decks for the Force: CDJ-style jog wheels you spin with a finger (VINYL scratches, CDJ nudges the pitch), mixer, two effect units, a track browser, a phone/PC companion page and Mixxx controller maps, with a big-button touchscreen skin and Q-Links.", "./plugins/remixxxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "dj", "decks", "mixer", "cdj", "jog-wheel", "mixxx", "midi-learn", "browser" }
        },
        {
            "rings", "Resonator", "saustin2010", "v1.2.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3107", "4.1 MB", "14%",
            "Émilie Gillet - VST - [SYN] Resonator", "rings.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-resonator/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-resonator/releases/download/v1.2.1/SYN-Resonator-1.2.1-mpc-armv7.zip", "plugins/rings/thumb.png",
            "Resonator: seven resonator models (strings, modal, FM, chords) strummed by the notes you play; 14 presets, from Émilie Gillet's open-source Rings.", "./plugins/rings/install.sh", 0,
            0,
            false,
            false,
            "",
            { "resonator", "physical-modelling", "eurorack" }
        },
        {
            "roads", "Roads", "Aaron Quint", "v1.2.3", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-12",
            "669", "3.0 MB", "15%",
            "quirkey - VST - Roads", "roads.so",
            "https://raw.githubusercontent.com/quirkey/mpc-plugins/roads-v1.2.3/docs/roads.png", "https://github.com/quirkey/mpc-plugins/releases/download/roads-v1.2.3/Roads-1.2.3-mpc-armv7.zip", "plugins/roads/thumb.png",
            "A 16-voice electric piano from mda ePiano's samples and modelling, with tine, reed and clav characters, tremolo / auto-pan, chorus, a note-triggered envelope filter and a 3-band EQ.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/quirkey - VST - Roads\" && ZIP_PATH=\"./plugins/roads/roads.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/quirkey/mpc-plugins/releases/download/roads-v1.2.3/Roads-1.2.3-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/roads\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/quirkey/mpc-plugins/releases/download/roads-v1.2.3/Roads-1.2.3-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/quirkey - VST - Roads/\"; elif [ -f \"./plugins/roads/roads.so\" ]; then cp -rf ./plugins/roads/* \"$TARGET_SYNTH_DIR/quirkey - VST - Roads/\"; else echo \"[ERROR] No local binary archive found for roads\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "electric-piano", "keys", "epiano", "polyphonic" }
        },
        {
            "setbfree-organ", "setBfree Organ", "SamplingHigh", "v0.3.1", "Organ",
            "INST", "Stable", "Akai Force (3.10.1)", "MPC OS 3.x", "2026-11-14",
            "2031", "2.2 MB", "8120%",
            "SamplingHigh - VST - setBfree Organ", "setbfree.so",
            "https://raw.githubusercontent.com/SamplingHigh/mpc-vst-setbfree/v0.3.1/docs/images/setbfree-organ.png", "https://github.com/SamplingHigh/mpc-vst-setbfree/releases/download/v0.3.1/setBfree-Organ-0.3.1-mpc-armv7.zip", "plugins/setbfree-organ/thumb.png",
            "Tonewheel organ with drawbars, percussion, scanner, editable Leslie and keyboard splits.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/SamplingHigh - VST - setBfree Organ\" && ZIP_PATH=\"./plugins/setbfree-organ/setbfree-organ.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/SamplingHigh/mpc-vst-setbfree/releases/download/v0.3.1/setBfree-Organ-0.3.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/setbfree-organ\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/SamplingHigh/mpc-vst-setbfree/releases/download/v0.3.1/setBfree-Organ-0.3.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/SamplingHigh - VST - setBfree Organ/\"; elif [ -f \"./plugins/setbfree-organ/setbfree.so\" ]; then cp -rf ./plugins/setbfree-organ/* \"$TARGET_SYNTH_DIR/SamplingHigh - VST - setBfree Organ/\"; else echo \"[ERROR] No local binary archive found for setbfree-organ\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "tonewheel", "drawbar", "rotary" }
        },
        {
            "sfizz", "sfizz", "gmorb (MPC port); sfizz by sfztools", "v1.1.1", "Sampler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1274", "1.4 MB", "14%",
            "sfztools - VST - sfizz", "sfizz_mpc.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-sfizz/6d9e627281779f087898521cd27caabf9b7a75f2/docs/screen.png", "https://github.com/gmorb/mpc-vst-sfizz/releases/download/v1.1.1/sfizz-1.1.1-mpc-armv7.zip", "plugins/sfizz/thumb.png",
            "The sfizz 1.2.3 sampler: plays SFZ libraries and SoundFont 2 (.sf2) files from SD cards and USB drives, streamed from disk, with key switches, the instrument's own CC controls, Scala tuning, mono/legato with glide and up to Sinc 72 interpolation, on a one-page screen modelled on sfizz's editor. Gen1 and Gen2 builds.", "./plugins/sfizz/install.sh", 0,
            0,
            false,
            false,
            "",
            { "sfz", "sf2", "soundfont", "sampler", "piano", "keyswitch", "streaming", "scala" }
        },
        {
            "sonix", "Sonix", "3dthc", "v0.3.15", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-13",
            "2830", "124.8 MB", "14%",
            "3dthc - VST - Sonix", "sonix.so",
            "https://raw.githubusercontent.com/3dthc/Sonix/main/docs/images/main_layers.jpg", "https://github.com/3dthc/Sonix/releases/download/v0.3.15/Sonix-0.3.15-mpc-armv7.zip", "plugins/sonix/thumb.png",
            "A melodic ROMpler instrument for standalone Akai MPC devices, built as a native ARM VST2 plugin. Includes pianos, keys, pads, strings, brass, choirs, leads, plucks, bass, bells, synth chords and effects, using redistributable sound libraries.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3dthc - VST - Sonix\" && ZIP_PATH=\"./plugins/sonix/sonix.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/3dthc/Sonix/releases/download/v0.3.15/Sonix-0.3.15-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/sonix\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/Sonix/releases/download/v0.3.15/Sonix-0.3.15-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/3dthc - VST - Sonix/\"; elif [ -f \"./plugins/sonix/sonix.so\" ]; then cp -rf ./plugins/sonix/* \"$TARGET_SYNTH_DIR/3dthc - VST - Sonix/\"; else echo \"[ERROR] No local binary archive found for sonix\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            {  }
        },
        {
            "op1fx-string", "String", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "709", "3.7 MB", "14%",
            "bombest - VST - String", "string.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/string.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/String-0.4.1-mpc-armv7.zip", "plugins/op1fx-string/thumb.png",
            "Plucked string with eight voices and an amp envelope. OP-1 inspired.", "./plugins/op1fx-string/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "synth", "pluck", "string" }
        },
        {
            "sturm-tp", "Sturm-TP", "sd88me", "v0.1.2", "Drum Machine",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "2029", "4.5 MB", "14%",
            "sd88me - VST - Sturm-TP", "sturm.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-sturm/sturm-tp-v0.1.1/docs/img/sturm.png", "https://github.com/sd88me/mpc-vst-sturm/releases/download/sturm-tp-v0.1.2/Sturm-TP-0.1.2-mpc-armv7.zip", "plugins/sturm-tp/thumb.png",
            "Six-voice analog-style drum and synth voice modelled on the Tempest drum machine: two analog and two sample oscillators, lowpass and highpass filters, five envelopes, two LFOs, eight mod paths, kit mode, distortion, compressor and delay. Bank browser with Q-Links; loads its SysEx dumps.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Sturm-TP\" && ZIP_PATH=\"./plugins/sturm-tp/sturm-tp.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-sturm/releases/download/sturm-tp-v0.1.2/Sturm-TP-0.1.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/sturm-tp\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-sturm/releases/download/sturm-tp-v0.1.2/Sturm-TP-0.1.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Sturm-TP/\"; elif [ -f \"./plugins/sturm-tp/sturm.so\" ]; then cp -rf ./plugins/sturm-tp/* \"$TARGET_SYNTH_DIR/sd88me - VST - Sturm-TP/\"; else echo \"[ERROR] No local binary archive found for sturm-tp\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "analog-modelling", "drums", "sampler", "kit", "tempest", "dsi" }
        },
        {
            "subwerk", "SubWerk", "Devko", "v0.0.10", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "621", "1.0 MB", "14%",
            "Devko - VST - SubWerk", "subwerk.so",
            "https://raw.githubusercontent.com/Devko/SubWerk/main/docs/img/filter.png", "https://github.com/Devko/SubWerk/releases/download/v0.0.10/SubWerk-0.0.10-mpc-armv7.zip", "plugins/subwerk/thumb.png",
            "Analog-style monosynth in the spirit of a 37-key American paraphonic classic: two morphing oscillators with hard sync, sub and noise, a feedback mixer, a 4-pole ladder filter with Multidrive, two looping envelopes, glide and Duo. 57 presets and a six-tab touchscreen skin with Q-Links.", "./plugins/subwerk/install.sh", 0,
            0,
            false,
            false,
            "",
            { "monosynth", "analog", "bass", "ladder-filter", "paraphonic" }
        },
        {
            "surgexxl", "SurgeXXL", "L'Cronx", "v0.2.0", "Synth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-24",
            "2006", "294.4 MB", "14%",
            "L'Cronx - VST - SurgeXXL", "surgexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/SurgeXXL/08c3c3eed8a3ad6792211fb1dc2351ded2383995/docs/img/osc.png", "https://github.com/sunskiefer/SurgeXXL/releases/download/v0.2.0/SurgeXXL-0.2.0-mpc-armv7.zip", "plugins/surgexxl/thumb.png",
            "The open-source Surge XT synth engine, headless, for Gen1: 2 scenes of 3 oscillators, dual filters, 6 LFOs per scene, mod matrix, 16 FX slots and 8 macros on eight pages. Loads every factory and third-party patch (3008) and wavetable (761), plus your own. Install it on its own (about 450 MB). MPC OS 3.x.", "./plugins/surgexxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "synth", "polysynth", "wavetable", "fm", "surge", "lfo", "presets", "airwindows" }
        },
        {
            "tablor", "Tablor", "saustin2010", "v1.1.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1662", "20.3 MB", "14%",
            "athousanddetails - VST - [SYN] Tablor", "tablor.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-tablor/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-tablor/releases/download/v1.1.1/SYN-Tablor-1.1.1-mpc-armv7.zip", "plugins/tablor/thumb.png",
            "Two-oscillator wavetable synth with unison, filter and mod envelopes; ships its wavetable library and 9 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/athousanddetails - VST - [SYN] Tablor\" && ZIP_PATH=\"./plugins/tablor/tablor.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-tablor/releases/download/v1.1.1/SYN-Tablor-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/tablor\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-tablor/releases/download/v1.1.1/SYN-Tablor-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/athousanddetails - VST - [SYN] Tablor/\"; elif [ -f \"./plugins/tablor/tablor.so\" ]; then cp -rf ./plugins/tablor/* \"$TARGET_SYNTH_DIR/athousanddetails - VST - [SYN] Tablor/\"; else echo \"[ERROR] No local binary archive found for tablor\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "wavetable", "polyphonic", "unison" }
        },
        {
            "tinyfreaky", "TinyFreaky", "MagicStino", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1528", "5.0 MB", "14%",
            "MagicStino - VST - Tinyfreaky", "tinyfreaky.so",
            "https://raw.githubusercontent.com/MagicStino/TinyFreaky/v0.4.0/assets/force-readable-preview.png", "https://github.com/MagicStino/TinyFreaky/releases/download/v0.4.1/Tinyfreaky-0.4.1-mpc-armv7.zip", "plugins/tinyfreaky/thumb.png",
            "Eight-voice MicroFreak-inspired interpretation with 16 synthesis engines, a 5x7 modulation matrix, arpeggiator/swing, 64-step sequencer and sound-informed preset names. 342 playable factory slots; six engine families remain unavailable.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/MagicStino - VST - Tinyfreaky\" && ZIP_PATH=\"./plugins/tinyfreaky/tinyfreaky.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/MagicStino/TinyFreaky/releases/download/v0.4.1/Tinyfreaky-0.4.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/tinyfreaky\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/MagicStino/TinyFreaky/releases/download/v0.4.1/Tinyfreaky-0.4.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/MagicStino - VST - Tinyfreaky/\"; elif [ -f \"./plugins/tinyfreaky/tinyfreaky.so\" ]; then cp -rf ./plugins/tinyfreaky/* \"$TARGET_SYNTH_DIR/MagicStino - VST - Tinyfreaky/\"; else echo \"[ERROR] No local binary archive found for tinyfreaky\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "synth", "polyphonic", "modulation", "sequencer" }
        },
        {
            "tr-mpc", "TR-MPC", "sd88me", "v1.2.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-12",
            "2786", "29.1 MB", "25%",
            "sd88me - VST - TR-MPC", "trmpc.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/7e704c6f3be14b661c887631e203494d8cc3f74f/docs/img/tr-mpc-slots.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-vst-v1.2.1/TR-MPC-1.2.1-mpc-armv7.zip", "plugins/tr-mpc/thumb.png",
            "A modular 16-slot drum kit: each slot plays any of the 49 voices of athousanddetails' 6W6, 8W8, CW-78 and 9W9 (a 606 kick beside an 808 snare and 909 hats), with per-slot level, tune, decay, drive, pan and sends, one shared reverb, delay and master chain, a randomise module, and a touchscreen skin that reskins each slot to its voice's kit.", "./plugins/tr-mpc/install.sh", 0,
            0,
            false,
            false,
            "",
            { "tr-606", "tr-808", "cr-78", "tr-909", "modular" }
        },
        {
            "tributary", "Tributary", "johnny", "v1.2.4", "Utility",
            "INST", "Stable", "MPC One (3.10.1.12)", "MPC OS 3.x", "2026-11-13",
            "821", "1.1 MB", "14%",
            "johnny - VST - Tributary", "tributary.so",
            "https://raw.githubusercontent.com/stampman3000-jpg/mpc-tributary/v1.1.0/screenshots/tributary.png", "https://github.com/stampman3000-jpg/mpc-tributary/releases/download/v1.2.4/Tributary-1.2.4-mpc-armv7.zip", "plugins/tributary/thumb.png",
            "Sums chosen Elektron Overbridge outputs onto one MPC track over USB. The machine is 48 kHz; the track resamples that to the MPC's 44.1. Capture saves the machine's own 48 kHz samples, locked to the bars. Based on Overwitch.", "./plugins/tributary/install.sh", 0,
            0,
            false,
            false,
            "",
            { "utility" }
        },
        {
            "tropix", "Tropix", "3dthc", "v0.1.5", "Sampler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-13",
            "1448", "215.2 MB", "14%",
            "3dthc - VST - Tropix", "tropix.so",
            "https://raw.githubusercontent.com/3dthc/Tropix/1005f9c4dc719f58bd17b02fac750457e6c8cb1e/docs/images/browse.jpg", "https://github.com/3dthc/Tropix/releases/download/v0.1.5/Tropix-0.1.5-mpc-armv7.zip", "plugins/tropix/thumb.png",
            "Island and afro ROMpler: 128 presets and 65 instruments for afrobeat, amapiano, kompa, soca, reggae and Hawaiian music - steel pans, log drums, saxes and trumpets, kompa and slack-key guitars, steels, keys, mallets, basses and percussion, from redistributable recordings and original synthesis. Two layers, glide, horn scoop, vibrato, nine colorways.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3dthc - VST - Tropix\" && ZIP_PATH=\"./plugins/tropix/tropix.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/3dthc/Tropix/releases/download/v0.1.5/Tropix-0.1.5-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/tropix\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/Tropix/releases/download/v0.1.5/Tropix-0.1.5-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/3dthc - VST - Tropix/\"; elif [ -f \"./plugins/tropix/tropix.so\" ]; then cp -rf ./plugins/tropix/* \"$TARGET_SYNTH_DIR/3dthc - VST - Tropix/\"; else echo \"[ERROR] No local binary archive found for tropix\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "rompler", "afrobeat", "amapiano", "kompa", "soca", "reggae", "steel-pan", "horns" }
        },
        {
            "vader", "VADER", "L'Cronx", "v0.10.1", "Synth",
            "INST", "Beta", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-13",
            "4916", "42.3 MB", "14%",
            "L'Cronx - VST - VADER", "vader.so",
            "https://raw.githubusercontent.com/sunskiefer/VADER/v0.10.1/docs/img/main_star.png", "https://github.com/sunskiefer/VADER/releases/download/v0.10.1/VADER-0.10.1-mpc-armv7.zip", "plugins/vader/thumb.png",
            "An orbit-based vector synth: an orbiter travels a shape (15 built in, 35 maths shapes, or your own SVG, OBJ, PNG or JPG) or one of 27 modes across four corners. Each corner plays overtone generators, one of 14 voices, or a WAV sample. Two timbres, arpeggiator, effects. Its screen runs on your phone: scan the QR code, load shapes from the phone too.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - VADER\" && ZIP_PATH=\"./plugins/vader/vader.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/VADER/releases/download/v0.10.1/VADER-0.10.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/vader\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/VADER/releases/download/v0.10.1/VADER-0.10.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - VADER/\"; elif [ -f \"./plugins/vader/vader.so\" ]; then cp -rf ./plugins/vader/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - VADER/\"; else echo \"[ERROR] No local binary archive found for vader\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "synth", "vector", "orbit", "additive", "morphing", "arpeggiator", "svg", "3d" }
        },
        {
            "vavra", "Vavra", "sd88me", "v1.0.0", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "95", "27.3 MB", "14%",
            "sd88me - VST - Vavra", "vavra.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-vavra/v1.0.0/docs/screenshots/main.png", "https://github.com/sd88me/mpc-vst-vavra/releases/download/v1.0.0/Vavra-1.0.0-mpc-aarch64.zip", "plugins/vavra/thumb.png",
            "An emulation of the Waldorf microQ's original OS (gearmulator) as a native instrument for Gen2 MPC devices, with a native screen in the look of its front panel (three skins), 300 ROM sounds plus your own sound sets, RANDOM, named favourites, Q-Link control and MPC tempo sync. Gen2 (aarch64) only; you need your own microQ ROM.", "./plugins/vavra/install.sh", 0,
            0,
            false,
            false,
            "",
            { "emulation", "wavetable", "rompler-free", "gen2" }
        },
        {
            "op1fx-voltage", "Voltage", "Bombest", "v0.4.1", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "583", "4.6 MB", "14%",
            "bombest - VST - Voltage", "voltage.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/voltage.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Voltage-0.4.1-mpc-armv7.zip", "plugins/op1fx-voltage/thumb.png",
            "Four-control synth with eight voices and an amp envelope. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Voltage\" && ZIP_PATH=\"./plugins/op1fx-voltage/op1fx-voltage.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Voltage-0.4.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-voltage\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Voltage-0.4.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/bombest - VST - Voltage/\"; elif [ -f \"./plugins/op1fx-voltage/voltage.so\" ]; then cp -rf ./plugins/op1fx-voltage/* \"$TARGET_SYNTH_DIR/bombest - VST - Voltage/\"; else echo \"[ERROR] No local binary archive found for op1fx-voltage\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "synth", "analog" }
        },
        {
            "welle", "Welle", "sd88me", "v0.2.0", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (3.10)", "MPC OS 3.x", "2026-11-11",
            "419", "2.8 MB", "14%",
            "sd88me - VST - Welle", "welle-arm64.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-welle/v0.2.0/docs/screenshots/main.png", "https://github.com/sd88me/mpc-vst-welle/releases/download/v0.2.0/Welle-0.2.0-mpc-aarch64.zip", "plugins/welle/thumb.png",
            "Waldorf Wave emulation running from the original Wave OS - developed by Mookid. Styled faithfully to the original panel. Needs your own Wave OS 1.700 files, uploaded from the Plugin Manager.", "./plugins/welle/install.sh", 0,
            0,
            false,
            false,
            "",
            { "wavetable", "waldorf", "emulation" }
        },
        {
            "wreck", "Wreck", "L'Cronx", "v0.3.0", "Sampler",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-11-08",
            "721", "3.0 MB", "14%",
            "L'Cronx - VST - Wreck", "wreck.so",
            "https://raw.githubusercontent.com/sunskiefer/Wreck/v0.3.0/docs/screenshots/page_0.png", "https://github.com/sunskiefer/Wreck/releases/download/v0.3.0/Wreck-0.3.0-mpc-armv7.zip", "plugins/wreck/thumb.png",
            "A self-sampling sound design instrument: ten sources (noise, clicks, your own WAV files from any folder or drive, FM, attractors and more) run through five blocks in any order into a 20 second loop that MIDI voices play back, and the voices can feed the chain again. 4-lane polymetric arp, bouncing-ball delay, 22 presets. MPC OS 3.x.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - Wreck\" && ZIP_PATH=\"./plugins/wreck/wreck.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/Wreck/releases/download/v0.3.0/Wreck-0.3.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/wreck\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/Wreck/releases/download/v0.3.0/Wreck-0.3.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - Wreck/\"; elif [ -f \"./plugins/wreck/wreck.so\" ]; then cp -rf ./plugins/wreck/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - Wreck/\"; else echo \"[ERROR] No local binary archive found for wreck\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "sampler", "sound-design", "experimental", "feedback", "generative", "arpeggiator", "chaos", "lo-fi" }
        },
        {
            "wurl", "Wurl", "saustin2010", "v1.1.1", "Electric Piano",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2054", "827 KB", "14%",
            "Filliformes - VST - [SYN] Wurl", "wurl.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-wurl/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-wurl/releases/download/v1.1.1/SYN-Wurl-1.1.1-mpc-armv7.zip", "plugins/wurl/thumb.png",
            "A physically modelled Wurlitzer 200A electric piano (OpenWurli) that barks with velocity, with tremolo and 10 presets.", "./plugins/wurl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "wurlitzer", "physical-modelling", "keys", "tremolo" }
        },
        {
            "xenia", "Xenia", "sd88me", "v1.1.0", "Synth",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "1323", "24.8 MB", "14%",
            "sd88me - VST - Xenia", "xenia.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-xenia/aeb6a25cfe8e5e14fc1df592526b2c2a32eb99be/docs/screenshots/xenia-play.png", "https://github.com/sd88me/mpc-vst-xenia/releases/download/v1.1.0/Xenia-1.1.0-mpc-aarch64.zip", "plugins/xenia/thumb.png",
            "An emulation of the Microwave XT wavetable synthesizer's original firmware (gearmulator's 68k and DSP56300 emulators) as a native instrument for Gen2 MPC devices: ten touchscreen pages, ROM banks and patch browser, assignable play knobs and Q-Link control on every page. Gen2 (aarch64) only; you need your own Microwave XT ROM.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Xenia\" && ZIP_PATH=\"./plugins/xenia/xenia.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-xenia/releases/download/v1.1.0/Xenia-1.1.0-mpc-aarch64.zip\" ]; then mkdir -p \"./plugins/xenia\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-xenia/releases/download/v1.1.0/Xenia-1.1.0-mpc-aarch64.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Xenia/\"; elif [ -f \"./plugins/xenia/xenia.so\" ]; then cp -rf ./plugins/xenia/* \"$TARGET_SYNTH_DIR/sd88me - VST - Xenia/\"; else echo \"[ERROR] No local binary archive found for xenia\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "emulation", "wavetable", "rompler-free", "gen2" }
        },
        {
            "pimorph", "πMorpH", "Timo", "v0.1.2", "Synth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-12",
            "640", "62.3 MB", "14%",
            "Timo - VST - πMorpH", "pimorph.so",
            "https://raw.githubusercontent.com/timomacquis/piMorpH-MPC/v0.1.0/Cover.png", "https://github.com/timomacquis/piMorpH-MPC/releases/download/v0.1.2/piMorpH-0.1.2-mpc-armv7.zip", "plugins/pimorph/thumb.png",
            "Polyphonic XY wavetable synthesizer with paths, orbit, dual envelopes, HP/LP filters, two LFOs and an eight-slot modulation matrix. Includes 100 presets plus INIT and Custom Wavetable import: mono/stereo PCM16, PCM32 or float32 WAV, 512/1024/2048 samples per waveform.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Timo - VST - πMorpH\" && ZIP_PATH=\"./plugins/pimorph/pimorph.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/timomacquis/piMorpH-MPC/releases/download/v0.1.2/piMorpH-0.1.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/pimorph\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/timomacquis/piMorpH-MPC/releases/download/v0.1.2/piMorpH-0.1.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Timo - VST - πMorpH/\"; elif [ -f \"./plugins/pimorph/pimorph.so\" ]; then cp -rf ./plugins/pimorph/* \"$TARGET_SYNTH_DIR/Timo - VST - πMorpH/\"; else echo \"[ERROR] No local binary archive found for pimorph\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "synth", "wavetable", "polyphonic", "modulation" }
        },
        {
            "pitonic", "πToniC", "Timo", "v1.0.3", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-11",
            "932", "2.6 MB", "14%",
            "Timo - VST - πToniC", "pitonic.so",
            "https://raw.githubusercontent.com/timomacquis/piToniC-MPC/423643e60fcf0d5d5634e6d4eae5513f1a4edc76/Cover.jpg", "https://github.com/timomacquis/piToniC-MPC/releases/download/v1.0.3/piToniC-1.0.3-mpc-armv7.zip", "plugins/pitonic/thumb.png",
            "A Microtonic-inspired drum machine. πToniC features 8 independent drum voices, each combining filtered noise with a morphing oscillator or WAV sample playback. Its sound engine includes pitch modulation, amplitude envelopes, drive, EQ and individual voice controls. Shared stereo reverb and tempo-synced delay complete the engine.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Timo - VST - πToniC\" && ZIP_PATH=\"./plugins/pitonic/pitonic.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/timomacquis/piToniC-MPC/releases/download/v1.0.3/piToniC-1.0.3-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/pitonic\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/timomacquis/piToniC-MPC/releases/download/v1.0.3/piToniC-1.0.3-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Timo - VST - πToniC/\"; elif [ -f \"./plugins/pitonic/pitonic.so\" ]; then cp -rf ./plugins/pitonic/* \"$TARGET_SYNTH_DIR/Timo - VST - πToniC/\"; else echo \"[ERROR] No local binary archive found for pitonic\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "drum-machine", "synth", "percussion" }
        }
        };
        if (!sec.items.empty()) sections.push_back(sec);
    }
    {
        Section sec;
        sec.title = "MIDI";
        sec.items = {
        {
            "acid", "Acid", "sd88me", "v3.0.0", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-12",
            "8643", "2.8 MB", "14%",
            "sd88me - VST - Acid", "acid.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-acid/master/docs/img/acid.png", "https://github.com/sd88me/mpc-vst-acid/releases/download/acid-vst-v3.0.0/Acid-3.0.0-mpc-armv7.zip", "plugins/acid/thumb.png",
            "TB-303-style acid bassline sequencer: two blendable lines with generate/mutate, driving a synth track over MIDI, with a touchscreen skin and Q-Links.", "./plugins/acid/install.sh", 0,
            0,
            false,
            false,
            "",
            { "303", "acid", "midi", "bassline" }
        },
        {
            "cadenza", "Cadenza", "Shrekage", "v0.28.1", "Sequencer",
            "MIDI", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-11-14",
            "700", "1.1 MB", "24%",
            "Cadenza - VST - Cadenza", "cadenza.so",
            "https://raw.githubusercontent.com/Shrekage2024/cadenza-force/v0.28.1/docs/screenshots/0260-play.png", "https://github.com/Shrekage2024/cadenza-force/releases/download/v0.28.1/Cadenza-0.28.1-mpc-armv7.zip", "plugins/cadenza/thumb.png",
            "Chord-driven generative MIDI sequencer: pick a key and scale, shape chords, bass and melody with a few knobs per page, and it generates and follows along as you play.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Cadenza - VST - Cadenza\" && ZIP_PATH=\"./plugins/cadenza/cadenza.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Shrekage2024/cadenza-force/releases/download/v0.28.1/Cadenza-0.28.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/cadenza\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Shrekage2024/cadenza-force/releases/download/v0.28.1/Cadenza-0.28.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Cadenza - VST - Cadenza/\"; elif [ -f \"./plugins/cadenza/cadenza.so\" ]; then cp -rf ./plugins/cadenza/* \"$TARGET_SYNTH_DIR/Cadenza - VST - Cadenza/\"; else echo \"[ERROR] No local binary archive found for cadenza\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "chords", "generative", "midi", "bassline" }
        },
        {
            "chordsmith", "Chordsmith", "jacob-sabella", "v1.0.5", "Utility",
            "MIDI", "Stable", "MPC Key 37 (3.10.1.12)", "MPC OS 2.x & 3.x", "2026-11-12",
            "4995", "4.2 MB", "8%",
            "jacob-sabella - VST - Chordsmith", "chordsmith.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-chordsmith/188d34ca0d9482d2d1ba840edb12ccd8c3773f76/docs/screenshots/chordsmith.png", "https://github.com/jacob-sabella/mpc-vst-chordsmith/releases/download/chordsmith-vst-v1.0.5/Chordsmith-1.0.5-mpc-armv7.zip", "plugins/chordsmith/thumb.png",
            "Chord tool: pads, keys or chord buttons play scale, set or custom chords with voicing, strum, arp and harp modes, MIDI out to other tracks and a built-in preview synth.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Chordsmith\" && ZIP_PATH=\"./plugins/chordsmith/chordsmith.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/jacob-sabella/mpc-vst-chordsmith/releases/download/chordsmith-vst-v1.0.5/Chordsmith-1.0.5-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/chordsmith\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-vst-chordsmith/releases/download/chordsmith-vst-v1.0.5/Chordsmith-1.0.5-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Chordsmith/\"; elif [ -f \"./plugins/chordsmith/chordsmith.so\" ]; then cp -rf ./plugins/chordsmith/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Chordsmith/\"; else echo \"[ERROR] No local binary archive found for chordsmith\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "chords", "midi", "arpeggiator", "strum", "chord-builder" }
        },
        {
            "daemon", "Daemon", "sd88me", "v1.0.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-21",
            "1039", "727 KB", "14%",
            "sd88me - VST - Daemon", "daemon.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-daemon/v1.0.0/docs/screenshots/daemon.png", "https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-1.0.1-mpc-armv7.zip", "plugins/daemon/thumb.png",
            "AI generative assistant: type or say what you hear and Daemon writes chords, basslines, melodies and beats in your key and plays them in time on its MIDI port. Agent mode builds the session too (tracks, instruments, presets, effects, clips) as a plan you RUN. Gemini or Claude; needs network and an API key.", "./plugins/daemon/install.sh", 0,
            0,
            false,
            false,
            "",
            { "generative", "ai", "midi", "chords", "melody", "beats", "agent", "voice" }
        },
        {
            "grids", "Drum Map", "saustin2010", "v1.2.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2366", "337 KB", "14%",
            "Émilie Gillet - VST - [SEQ] Drum Map", "grids.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-drum-map/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-drum-map/releases/download/v1.2.1/SEQ-Drum-Map-1.2.1-mpc-armv7.zip", "plugins/grids/thumb.png",
            "Drum Map: a topographic drum sequencer that plays kick, snare and hats from MPC's clock through its own MIDI port, from Émilie Gillet's open-source Grids.", "./plugins/grids/install.sh", 0,
            0,
            false,
            false,
            "",
            { "drum", "midi", "euclidean", "eurorack" }
        },
        {
            "rampage", "Dual Slope", "saustin2010", "v1.2.1", "Modulation",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2058", "3.1 MB", "14%",
            "Andrew Belt - VST - [SEQ] Dual Slope", "rampage.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-dual-slope/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-dual-slope/releases/download/v1.2.1/SEQ-Dual-Slope-1.2.1-mpc-armv7.zip", "plugins/rampage/thumb.png",
            "Dual Slope, based on Befaco's Rampage VCV Rack module: two rise-and-fall generators (envelopes, LFOs, slew) whose outputs go out of its own MIDI port as CCs.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Andrew Belt - VST - [SEQ] Dual Slope\" && ZIP_PATH=\"./plugins/rampage/rampage.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-dual-slope/releases/download/v1.2.1/SEQ-Dual-Slope-1.2.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/rampage\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-dual-slope/releases/download/v1.2.1/SEQ-Dual-Slope-1.2.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Andrew Belt - VST - [SEQ] Dual Slope/\"; elif [ -f \"./plugins/rampage/rampage.so\" ]; then cp -rf ./plugins/rampage/* \"$TARGET_SYNTH_DIR/Andrew Belt - VST - [SEQ] Dual Slope/\"; else echo \"[ERROR] No local binary archive found for rampage\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "vcv", "envelope", "lfo", "slew", "midi-cc", "eurorack" }
        },
        {
            "eucalypso", "Eucalypso", "saustin2010", "v1.0.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1430", "546 KB", "14%",
            "handcraftedcc - VST - [SEQ] Eucalypso", "eucalypso.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-eucalypso/v1.0.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-eucalypso/releases/download/v1.0.1/SEQ-Eucalypso-1.0.1-mpc-armv7.zip", "plugins/eucalypso/thumb.png",
            "Four-lane Euclidean sequencer that turns the notes you hold into interlocking rhythms for other tracks.", "./plugins/eucalypso/install.sh", 0,
            0,
            false,
            false,
            "",
            { "euclidean", "midi", "polyrhythm", "generative" }
        },
        {
            "euclidier", "Euclid", "sd88me", "v2.0.0", "Sequencer",
            "MIDI", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (MPC OS 3.10)", "MPC OS 3.x", "2026-11-11",
            "6430", "425 KB", "14%",
            "sd88me - VST - Euclid", "euclid.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-euclid/master/docs/img/euclidier.png", "https://github.com/sd88me/mpc-vst-euclid/releases/download/euclid-v2.0.0/Euclid-2.0.0-mpc-armv7.zip", "plugins/euclidier/thumb.png",
            "8-lane Euclidean MIDI sequencer: live pattern circle and lane rows with a play-head, per-lane and global randomise, a drum-mode switch, and a touchscreen skin with Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Euclid\" && ZIP_PATH=\"./plugins/euclidier/euclidier.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-euclid/releases/download/euclid-v2.0.0/Euclid-2.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/euclidier\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-euclid/releases/download/euclid-v2.0.0/Euclid-2.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Euclid/\"; elif [ -f \"./plugins/euclidier/euclid.so\" ]; then cp -rf ./plugins/euclidier/* \"$TARGET_SYNTH_DIR/sd88me - VST - Euclid/\"; else echo \"[ERROR] No local binary archive found for euclidier\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "euclidean", "midi", "drum", "polyrhythm" }
        },
        {
            "glass-spheres", "Glass Spheres", "Padbangers", "v1.1.0", "Sequencer",
            "MIDI", "Stable", "Akai MPC X (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-14",
            "1752", "6.6 MB", "14%",
            "Padbangers - VST - Glass Spheres", "VST/glass-spheres-arm32.so",
            "https://raw.githubusercontent.com/padbangers/glass-spheres/glass-spheres-v1.1.0/docs/glass-spheres-main.png", "https://github.com/padbangers/glass-spheres/releases/download/glass-spheres-v1.1.0/Glass-Spheres-1.1.0-mpc-armv7.zip", "plugins/glass-spheres/thumb.png",
            "A random sampler after Mutable Instruments Marbles: random gates, melodies and deja vu loops that play other tracks over MIDI or analog gear over the MPC X's CV jacks, with envelopes, LFOs and a modulation matrix.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Padbangers - VST - Glass Spheres\" && ZIP_PATH=\"./plugins/glass-spheres/glass-spheres.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/padbangers/glass-spheres/releases/download/glass-spheres-v1.1.0/Glass-Spheres-1.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/glass-spheres\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/padbangers/glass-spheres/releases/download/glass-spheres-v1.1.0/Glass-Spheres-1.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Padbangers - VST - Glass Spheres/\"; elif [ -f \"./plugins/glass-spheres/VST/glass-spheres-arm32.so\" ]; then cp -rf ./plugins/glass-spheres/* \"$TARGET_SYNTH_DIR/Padbangers - VST - Glass Spheres/\"; else echo \"[ERROR] No local binary archive found for glass-spheres\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "mutable-instruments", "marbles", "random", "generative", "sequencer", "midi", "cv", "eurorack" }
        },
        {
            "groove-bank", "Groove Bank", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2438", "852 KB", "14%",
            "Mission Minnow - VST - [SEQ] Groove Bank", "groovebank.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-groovebank/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-groovebank/releases/download/v1.1.1/SEQ-Groove-Bank-1.1.1-mpc-armv7.zip", "plugins/groove-bank/thumb.png",
            "Replays the chord you hold in genre grooves from its library, with swing, strum and latch, through its own MIDI port.", "./plugins/groove-bank/install.sh", 0,
            0,
            false,
            false,
            "",
            { "midi", "chords", "rhythm", "grooves" }
        },
        {
            "maze-sequencer", "Maze Sequencer", "sd88me", "v2.0.0", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-14",
            "6066", "4.8 MB", "14%",
            "sd88me - VST - Maze Sequencer", "maze_seq.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-maze/main/docs/img/maze-sequencer.png", "https://github.com/sd88me/mpc-vst-maze/releases/download/maze-sequencer-vst-v2.0.0/Maze-Sequencer-2.0.0-mpc-armv7.zip", "plugins/maze-sequencer/thumb.png",
            "Dual 8-step generative sequencer (Moog Labyrinth style) with corrupt, scale quantising, two LFOs and per-line MIDI channels, driving other tracks over MIDI.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Maze Sequencer\" && ZIP_PATH=\"./plugins/maze-sequencer/maze-sequencer.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-maze/releases/download/maze-sequencer-vst-v2.0.0/Maze-Sequencer-2.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/maze-sequencer\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-maze/releases/download/maze-sequencer-vst-v2.0.0/Maze-Sequencer-2.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Maze Sequencer/\"; elif [ -f \"./plugins/maze-sequencer/maze_seq.so\" ]; then cp -rf ./plugins/maze-sequencer/* \"$TARGET_SYNTH_DIR/sd88me - VST - Maze Sequencer/\"; else echo \"[ERROR] No local binary archive found for maze-sequencer\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "generative", "midi", "moog", "labyrinth" }
        },
        {
            "midi-player", "MIDI Player", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1627", "144 KB", "14%",
            "Charles Vestal - VST - [SEQ] MIDI Player", "midiplayer.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-midiplayer/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-midiplayer/releases/download/v1.1.1/SEQ-MIDI-Player-1.1.1-mpc-armv7.zip", "plugins/midi-player/thumb.png",
            "Plays Standard MIDI Files in time with MPC's tempo and transport, one track or all, through its own MIDI port.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SEQ] MIDI Player\" && ZIP_PATH=\"./plugins/midi-player/midi-player.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-midiplayer/releases/download/v1.1.1/SEQ-MIDI-Player-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/midi-player\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-midiplayer/releases/download/v1.1.1/SEQ-MIDI-Player-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SEQ] MIDI Player/\"; elif [ -f \"./plugins/midi-player/midiplayer.so\" ]; then cp -rf ./plugins/midi-player/* \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SEQ] MIDI Player/\"; else echo \"[ERROR] No local binary archive found for midi-player\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "midi", "midi-file", "player", "transport-sync" }
        },
        {
            "pixel-walkers", "Pixel Walkers", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1632", "375 KB", "14%",
            "mestela - VST - [SEQ] Pixel Walkers", "pixelwalkers.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-pixelwalkers/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-pixelwalkers/releases/download/v1.1.1/SEQ-Pixel-Walkers-1.1.1-mpc-armv7.zip", "plugins/pixel-walkers/thumb.png",
            "Generative gravity sequencer: each note you play becomes a walker that bounces and retriggers its note as it lands.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/mestela - VST - [SEQ] Pixel Walkers\" && ZIP_PATH=\"./plugins/pixel-walkers/pixel-walkers.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-pixelwalkers/releases/download/v1.1.1/SEQ-Pixel-Walkers-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/pixel-walkers\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-pixelwalkers/releases/download/v1.1.1/SEQ-Pixel-Walkers-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/mestela - VST - [SEQ] Pixel Walkers/\"; elif [ -f \"./plugins/pixel-walkers/pixelwalkers.so\" ]; then cp -rf ./plugins/pixel-walkers/* \"$TARGET_SYNTH_DIR/mestela - VST - [SEQ] Pixel Walkers/\"; else echo \"[ERROR] No local binary archive found for pixel-walkers\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "generative", "midi", "physics", "bouncing" }
        },
        {
            "steprunner-8", "StepRunner-8", "djokenzi", "v1.2.0", "Sequencer",
            "MIDI", "Stable", "MPC Live (3.9.1.2)", "MPC OS 3.x", "2026-10-19",
            "2376", "3.4 MB", "14%",
            "djokenzi - VST - StepRunner-8", "steprunner8.so",
            "https://raw.githubusercontent.com/djokenzi/steprunner-8/76285a3aa34dcdc85bf8e72e3aa18baa7f6428cd/docs/screenshots/steprunner-8.png", "https://github.com/djokenzi/steprunner-8/releases/download/v1.2.0/StepRunner-8-1.2.0-mpc-armv7.zip", "plugins/steprunner-8/thumb.png",
            "8-track, 64-step sequencer with step conditions, step and CC locks, patterns, states and performance effects, driving a drum program over MIDI.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/djokenzi - VST - StepRunner-8\" && ZIP_PATH=\"./plugins/steprunner-8/steprunner-8.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/djokenzi/steprunner-8/releases/download/v1.2.0/StepRunner-8-1.2.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/steprunner-8\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/djokenzi/steprunner-8/releases/download/v1.2.0/StepRunner-8-1.2.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/djokenzi - VST - StepRunner-8/\"; elif [ -f \"./plugins/steprunner-8/steprunner8.so\" ]; then cp -rf ./plugins/steprunner-8/* \"$TARGET_SYNTH_DIR/djokenzi - VST - StepRunner-8/\"; else echo \"[ERROR] No local binary archive found for steprunner-8\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "midi", "step-sequencer", "drums", "step-locks" }
        },
        {
            "stevequencer", "Stevequencer", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-24",
            "1846", "1.2 MB", "14%",
            "Steve A - VST - [SEQ] Stevequencer", "stevequencer.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-stevequencer/v1.0.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-stevequencer/releases/download/v1.1.1/SEQ-Stevequencer-1.1.1-mpc-armv7.zip", "plugins/stevequencer/thumb.png",
            "Melodic step sequencer edited from the Q-Links: 16 steps on a 4x4 grid, up to 64, with chance, ratchets and MOD lanes.", "./plugins/stevequencer/install.sh", 0,
            0,
            false,
            false,
            "",
            { "step-sequencer", "midi", "q-links", "melodic" }
        },
        {
            "stevequencer-16", "Stevequencer 16", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-24",
            "1786", "628 KB", "14%",
            "Steve A - VST - [SEQ] Stevequencer 16", "stevequencer16.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-stevequencer16/v1.0.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-stevequencer16/releases/download/v1.1.1/SEQ-Stevequencer-16-1.1.1-mpc-armv7.zip", "plugins/stevequencer-16/thumb.png",
            "16-step sequencer with eight modulation lanes that move any parameter of the instrument (CC or NRPN), on any page.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Steve A - VST - [SEQ] Stevequencer 16\" && ZIP_PATH=\"./plugins/stevequencer-16/stevequencer-16.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-stevequencer16/releases/download/v1.1.1/SEQ-Stevequencer-16-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/stevequencer-16\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-stevequencer16/releases/download/v1.1.1/SEQ-Stevequencer-16-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Steve A - VST - [SEQ] Stevequencer 16/\"; elif [ -f \"./plugins/stevequencer-16/stevequencer16.so\" ]; then cp -rf ./plugins/stevequencer-16/* \"$TARGET_SYNTH_DIR/Steve A - VST - [SEQ] Stevequencer 16/\"; else echo \"[ERROR] No local binary archive found for stevequencer-16\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "step-sequencer", "midi", "modulation", "nrpn", "q-links" }
        },
        {
            "super-arp", "Super Arp", "saustin2010", "v1.1.1", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1819", "2.3 MB", "14%",
            "handcraftedcc - VST - [SEQ] Super Arp", "superarp.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-superarp/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-superarp/releases/download/v1.1.1/SEQ-Super-Arp-1.1.1-mpc-armv7.zip", "plugins/super-arp/thumb.png",
            "Arpeggiator that splits note order from rhythm: 40 patterns, 40 rhythms and seeded random modifiers.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/handcraftedcc - VST - [SEQ] Super Arp\" && ZIP_PATH=\"./plugins/super-arp/super-arp.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-superarp/releases/download/v1.1.1/SEQ-Super-Arp-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/super-arp\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-superarp/releases/download/v1.1.1/SEQ-Super-Arp-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/handcraftedcc - VST - [SEQ] Super Arp/\"; elif [ -f \"./plugins/super-arp/superarp.so\" ]; then cp -rf ./plugins/super-arp/* \"$TARGET_SYNTH_DIR/handcraftedcc - VST - [SEQ] Super Arp/\"; else echo \"[ERROR] No local binary archive found for super-arp\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "arpeggiator", "midi", "patterns", "rhythm" }
        },
        {
            "piclidean-mpc", "πClideaN", "Timo", "v1.3.7", "Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-15",
            "1195", "5.2 MB", "14%",
            "Timo - VST - πClideaN", "piclidean_mpc.so",
            "https://raw.githubusercontent.com/timomacquis/piClideaN-MPC/657e7ca8ac8139eeb1678f8639afa2b40ac7faa0/Cover.png", "https://github.com/timomacquis/piClideaN-MPC/releases/download/v1.3.7/ClideaN-MPC-1.3.7-mpc-armv7.zip", "plugins/piclidean-mpc/thumb.png",
            "Sixteen-lane hybrid MIDI sequencer with Euclidean patterns, per-step expression, scales, selective randomization and dual-LFO modulation.", "./plugins/piclidean-mpc/install.sh", 0,
            0,
            false,
            false,
            "",
            { "midi", "sequencer", "euclidean", "generative" }
        }
        };
        if (!sec.items.empty()) sections.push_back(sec);
    }
    {
        Section sec;
        sec.title = "EFFECTS";
        sec.items = {
        {
            "airwindows-air3", "Airwindows Air3", "mmiroshnikov", "v1.0.0", "Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1750", "745 KB", "14%",
            "Airwindows - VST - Air3", "air3.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Air3.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Air3-1.0.0-mpc-armv7.zip", "plugins/airwindows-air3/thumb.png",
            "Air-band EQ that lifts or tames the very top and the low ground of a mix.", "./plugins/airwindows-air3/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "eq", "air", "treble" }
        },
        {
            "airwindows-buttercomp2", "Airwindows ButterComp2", "mmiroshnikov", "v1.0.0", "Compressor",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1743", "757 KB", "14%",
            "Airwindows - VST - ButterComp2", "buttercomp2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/ButterComp2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ButterComp2-1.0.0-mpc-armv7.zip", "plugins/airwindows-buttercomp2/thumb.png",
            "Smooth, buttery bipolar compressor that stays transparent even when pushed.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - ButterComp2\" && ZIP_PATH=\"./plugins/airwindows-buttercomp2/airwindows-buttercomp2.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ButterComp2-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/airwindows-buttercomp2\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ButterComp2-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - ButterComp2/\"; elif [ -f \"./plugins/airwindows-buttercomp2/buttercomp2.so\" ]; then cp -rf ./plugins/airwindows-buttercomp2/* \"$TARGET_SYNTH_DIR/Airwindows - VST - ButterComp2/\"; else echo \"[ERROR] No local binary archive found for airwindows-buttercomp2\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "compressor", "dynamics" }
        },
        {
            "airwindows-chamber2", "Airwindows Chamber2", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1652", "702 KB", "14%",
            "Airwindows - VST - Chamber2", "chamber2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Chamber2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Chamber2-1.0.0-mpc-armv7.zip", "plugins/airwindows-chamber2/thumb.png",
            "Chamber reverb with delay, regeneration and thickness controls.", "./plugins/airwindows-chamber2/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "reverb", "chamber" }
        },
        {
            "airwindows-chorusensemble", "Airwindows ChorusEnsemble", "mmiroshnikov", "v1.0.0", "Chorus",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1656", "647 KB", "14%",
            "Airwindows - VST - ChorusEnsemble", "chorusensemble.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/ChorusEnsemble.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ChorusEnsemble-1.0.0-mpc-armv7.zip", "plugins/airwindows-chorusensemble/thumb.png",
            "Lush multi-tap ensemble chorus.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - ChorusEnsemble\" && ZIP_PATH=\"./plugins/airwindows-chorusensemble/airwindows-chorusensemble.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ChorusEnsemble-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/airwindows-chorusensemble\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ChorusEnsemble-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - ChorusEnsemble/\"; elif [ -f \"./plugins/airwindows-chorusensemble/chorusensemble.so\" ]; then cp -rf ./plugins/airwindows-chorusensemble/* \"$TARGET_SYNTH_DIR/Airwindows - VST - ChorusEnsemble/\"; else echo \"[ERROR] No local binary archive found for airwindows-chorusensemble\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "chorus", "ensemble", "modulation" }
        },
        {
            "aw-ambience", "Airwindows Collection: Ambience", "sd88me", "v0.1.0", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "726", "10.6 MB", "14%",
            "Airwindows - VST - AW Ambience", "aw_ambience.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/ambience.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Ambience-0.1.0-mpc-armv7.zip", "plugins/aw-ambience/thumb.png",
            "The Airwindows Ambience collection: all 26 ambience, delay, chorus and echo effects in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-ambience/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "delay", "collection" }
        },
        {
            "aw-console-strips", "Airwindows Collection: Console Strips", "sd88me", "v0.1.0", "Mixing",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "723", "15.2 MB", "14%",
            "Airwindows - VST - AW Console Strips", "aw_consolestrips.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/consolestrips.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Console-Strips-0.1.0-mpc-armv7.zip", "plugins/aw-console-strips/thumb.png",
            "The Airwindows Console Strips collection: all 8 big console channel strips (ConsoleX and ConsoleH), spread over four pages in one plugin, each with its own panel, Q-Link badges and built-in manual.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Console Strips\" && ZIP_PATH=\"./plugins/aw-console-strips/aw-console-strips.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Console-Strips-0.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/aw-console-strips\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Console-Strips-0.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Console Strips/\"; elif [ -f \"./plugins/aw-console-strips/aw_consolestrips.so\" ]; then cp -rf ./plugins/aw-console-strips/* \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Console Strips/\"; else echo \"[ERROR] No local binary archive found for aw-console-strips\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "channel-strip", "collection" }
        },
        {
            "aw-consoles", "Airwindows Collection: Consoles", "sd88me", "v0.1.0", "Mixing",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "674", "10.5 MB", "14%",
            "Airwindows - VST - AW Consoles", "aw_consoles.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/consoles.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Consoles-0.1.0-mpc-armv7.zip", "plugins/aw-consoles/thumb.png",
            "The Airwindows Consoles collection: all 50 console and mix-buss systems (Console 0 to 9, ConsoleLA, MC, MD and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-consoles/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "console", "collection" }
        },
        {
            "aw-distortion", "Airwindows Collection: Distortion", "sd88me", "v0.1.0", "Distortion",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "685", "10.0 MB", "14%",
            "Airwindows - VST - AW Distortion", "aw_distortion.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/distortion.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Distortion-0.1.0-mpc-armv7.zip", "plugins/aw-distortion/thumb.png",
            "The Airwindows Distortion collection: all 15 distortions (Density, Drive, Edge, Mackity, HardVacuum and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Distortion\" && ZIP_PATH=\"./plugins/aw-distortion/aw-distortion.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Distortion-0.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/aw-distortion\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Distortion-0.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Distortion/\"; elif [ -f \"./plugins/aw-distortion/aw_distortion.so\" ]; then cp -rf ./plugins/aw-distortion/* \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Distortion/\"; else echo \"[ERROR] No local binary archive found for aw-distortion\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "distortion", "collection" }
        },
        {
            "aw-dynamics", "Airwindows Collection: Dynamics", "sd88me", "v0.1.0", "Dynamics",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "698", "13.8 MB", "14%",
            "Airwindows - VST - AW Dynamics", "aw_dynamics.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/dynamics.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Dynamics-0.1.0-mpc-armv7.zip", "plugins/aw-dynamics/thumb.png",
            "The Airwindows Dynamics collection: all 35 compressors, gates and dynamics tools in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-dynamics/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "compressor", "collection" }
        },
        {
            "aw-effects", "Airwindows Collection: Effects", "sd88me", "v0.1.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "720", "12.2 MB", "14%",
            "Airwindows - VST - AW Effects", "aw_effects.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/effects.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Effects-0.1.0-mpc-armv7.zip", "plugins/aw-effects/thumb.png",
            "The Airwindows Effects collection: all 29 effects (Pafnuty, tremolo, pitch shifters, wavefolders, exciters and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Effects\" && ZIP_PATH=\"./plugins/aw-effects/aw-effects.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Effects-0.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/aw-effects\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Effects-0.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Effects/\"; elif [ -f \"./plugins/aw-effects/aw_effects.so\" ]; then cp -rf ./plugins/aw-effects/* \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Effects/\"; else echo \"[ERROR] No local binary archive found for aw-effects\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "effects", "collection" }
        },
        {
            "aw-filter", "Airwindows Collection: Filter", "sd88me", "v0.1.0", "Filter",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "668", "13.7 MB", "14%",
            "Airwindows - VST - AW Filter", "aw_filter.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/filter.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Filter-0.1.0-mpc-armv7.zip", "plugins/aw-filter/thumb.png",
            "The Airwindows Filter collection: all 54 filters and EQs (Baxandall, Isolator, Holt, SmoothEQ and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-filter/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "eq", "collection" }
        },
        {
            "aw-reverb", "Airwindows Collection: Reverb", "sd88me", "v0.1.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "719", "10.7 MB", "14%",
            "Airwindows - VST - AW Reverb", "aw_reverb.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/reverb.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Reverb-0.1.0-mpc-armv7.zip", "plugins/aw-reverb/thumb.png",
            "The Airwindows Reverb collection: all 42 reverbs (the k-series, plates, Galactic, Verbity and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Reverb\" && ZIP_PATH=\"./plugins/aw-reverb/aw-reverb.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Reverb-0.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/aw-reverb\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Reverb-0.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Reverb/\"; elif [ -f \"./plugins/aw-reverb/aw_reverb.so\" ]; then cp -rf ./plugins/aw-reverb/* \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Reverb/\"; else echo \"[ERROR] No local binary archive found for aw-reverb\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "reverb", "collection" }
        },
        {
            "aw-saturation", "Airwindows Collection: Saturation", "sd88me", "v0.1.0", "Saturation",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "695", "10.9 MB", "14%",
            "Airwindows - VST - AW Saturation", "aw_saturation.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/saturation.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Saturation-0.1.0-mpc-armv7.zip", "plugins/aw-saturation/thumb.png",
            "The Airwindows Saturation collection: all 20 saturation models (Tube, Mojo, Focus, Spiral2 and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-saturation/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "saturation", "collection" }
        },
        {
            "aw-tape", "Airwindows Collection: Tape", "sd88me", "v0.1.0", "Tape",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "701", "10.6 MB", "14%",
            "Airwindows - VST - AW Tape", "aw_tape.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/tape.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Tape-0.1.0-mpc-armv7.zip", "plugins/aw-tape/thumb.png",
            "The Airwindows Tape collection: all 12 tape emulations (ToTape, TapeHack, IronOxide and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Tape\" && ZIP_PATH=\"./plugins/aw-tape/aw-tape.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Tape-0.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/aw-tape\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Tape-0.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Tape/\"; elif [ -f \"./plugins/aw-tape/aw_tape.so\" ]; then cp -rf ./plugins/aw-tape/* \"$TARGET_SYNTH_DIR/Airwindows - VST - AW Tape/\"; else echo \"[ERROR] No local binary archive found for aw-tape\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "tape", "collection" }
        },
        {
            "aw-utility", "Airwindows Collection: Utility", "sd88me", "v0.1.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "635", "13.5 MB", "14%",
            "Airwindows - VST - AW Utility", "aw_utility.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-airwindows/6196324ee776860ce4d3cc46dbd90e331f3ba53d/docs/img/utility.png", "https://github.com/sd88me/mpc-vst-airwindows/releases/download/v0.1.0/AW-Utility-0.1.0-mpc-armv7.zip", "plugins/aw-utility/thumb.png",
            "The Airwindows Utility collection: all 38 utilities (gain, pan, trims, mid/side, monitoring, mix checks and more) in one plugin, each with its own panel, Q-Link badges and built-in manual.", "./plugins/aw-utility/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "utility", "collection" }
        },
        {
            "airwindows-console7channel", "Airwindows Console7Channel", "mmiroshnikov", "v1.0.0", "Saturation",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1618", "737 KB", "14%",
            "Airwindows - VST - Console7Channel", "console7channel.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Console7Channel.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Console7Channel-1.0.0-mpc-armv7.zip", "plugins/airwindows-console7channel/thumb.png",
            "Console7 channel stage: analog-style console saturation behind a fader.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - Console7Channel\" && ZIP_PATH=\"./plugins/airwindows-console7channel/airwindows-console7channel.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Console7Channel-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/airwindows-console7channel\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Console7Channel-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - Console7Channel/\"; elif [ -f \"./plugins/airwindows-console7channel/console7channel.so\" ]; then cp -rf ./plugins/airwindows-console7channel/* \"$TARGET_SYNTH_DIR/Airwindows - VST - Console7Channel/\"; else echo \"[ERROR] No local binary archive found for airwindows-console7channel\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "console", "saturation", "summing" }
        },
        {
            "airwindows-density2", "Airwindows Density2", "mmiroshnikov", "v1.0.0", "Saturation",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1645", "648 KB", "14%",
            "Airwindows - VST - Density2", "density2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Density2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Density2-1.0.0-mpc-armv7.zip", "plugins/airwindows-density2/thumb.png",
            "Density saturation with a highpass, for thickening and fattening sounds.", "./plugins/airwindows-density2/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "saturation", "distortion", "density" }
        },
        {
            "airwindows-galactic", "Airwindows Galactic", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1751", "835 KB", "14%",
            "Airwindows - VST - Galactic", "galactic.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Galactic.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic-1.0.0-mpc-armv7.zip", "plugins/airwindows-galactic/thumb.png",
            "Huge, dark, shimmering ambient space reverb.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic\" && ZIP_PATH=\"./plugins/airwindows-galactic/airwindows-galactic.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/airwindows-galactic\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic/\"; elif [ -f \"./plugins/airwindows-galactic/galactic.so\" ]; then cp -rf ./plugins/airwindows-galactic/* \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic/\"; else echo \"[ERROR] No local binary archive found for airwindows-galactic\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "reverb", "ambient", "shimmer" }
        },
        {
            "airwindows-galactic2", "Airwindows Galactic2", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1636", "816 KB", "14%",
            "Airwindows - VST - Galactic2", "galactic2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Galactic2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic2-1.0.0-mpc-armv7.zip", "plugins/airwindows-galactic2/thumb.png",
            "Galactic reverb with drive, sustain and darkening for vast ambient tails.", "./plugins/airwindows-galactic2/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "reverb", "ambient" }
        },
        {
            "airwindows-galactic3", "Airwindows Galactic3", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1664", "692 KB", "14%",
            "Airwindows - VST - Galactic3", "galactic3.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Galactic3.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic3-1.0.0-mpc-armv7.zip", "plugins/airwindows-galactic3/thumb.png",
            "Galactic reverb with brightness, detune, derez and size controls.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic3\" && ZIP_PATH=\"./plugins/airwindows-galactic3/airwindows-galactic3.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic3-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/airwindows-galactic3\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Galactic3-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic3/\"; elif [ -f \"./plugins/airwindows-galactic3/galactic3.so\" ]; then cp -rf ./plugins/airwindows-galactic3/* \"$TARGET_SYNTH_DIR/Airwindows - VST - Galactic3/\"; else echo \"[ERROR] No local binary archive found for airwindows-galactic3\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "reverb", "ambient" }
        },
        {
            "airwindows-galacticvibe", "Airwindows GalacticVibe", "mmiroshnikov", "v1.0.0", "Chorus",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1567", "702 KB", "14%",
            "Airwindows - VST - GalacticVibe", "galacticvibe.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/GalacticVibe.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/GalacticVibe-1.0.0-mpc-armv7.zip", "plugins/airwindows-galacticvibe/thumb.png",
            "The pitch-wobble vibrato stage of the Galactic reverbs on its own.", "./plugins/airwindows-galacticvibe/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "vibrato", "modulation", "chorus" }
        },
        {
            "airwindows-infinity2", "Airwindows Infinity2", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1638", "622 KB", "14%",
            "Airwindows - VST - Infinity2", "infinity2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Infinity2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Infinity2-1.0.0-mpc-armv7.zip", "plugins/airwindows-infinity2/thumb.png",
            "Infinite-sustain reverb with filtering, damping and allpass diffusion.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - Infinity2\" && ZIP_PATH=\"./plugins/airwindows-infinity2/airwindows-infinity2.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Infinity2-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/airwindows-infinity2\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Infinity2-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - Infinity2/\"; elif [ -f \"./plugins/airwindows-infinity2/infinity2.so\" ]; then cp -rf ./plugins/airwindows-infinity2/* \"$TARGET_SYNTH_DIR/Airwindows - VST - Infinity2/\"; else echo \"[ERROR] No local binary archive found for airwindows-infinity2\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "reverb", "ambient", "freeze" }
        },
        {
            "airwindows-kalienspaceship", "Airwindows kAlienSpaceship", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1614", "721 KB", "14%",
            "Airwindows - VST - kAlienSpaceship", "kalienspaceship.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/kAlienSpaceship.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/kAlienSpaceship-1.0.0-mpc-armv7.zip", "plugins/airwindows-kalienspaceship/thumb.png",
            "Enormous sci-fi space reverb from the kCathedral family.", "./plugins/airwindows-kalienspaceship/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "reverb", "sci-fi", "ambient" }
        },
        {
            "airwindows-kcathedral5", "Airwindows kCathedral5", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1563", "599 KB", "14%",
            "Airwindows - VST - kCathedral5", "kcathedral5.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/kCathedral5.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/kCathedral5-1.0.0-mpc-armv7.zip", "plugins/airwindows-kcathedral5/thumb.png",
            "Cathedral-sized reverb with early reflections and position control.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - kCathedral5\" && ZIP_PATH=\"./plugins/airwindows-kcathedral5/airwindows-kcathedral5.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/kCathedral5-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/airwindows-kcathedral5\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/kCathedral5-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - kCathedral5/\"; elif [ -f \"./plugins/airwindows-kcathedral5/kcathedral5.so\" ]; then cp -rf ./plugins/airwindows-kcathedral5/* \"$TARGET_SYNTH_DIR/Airwindows - VST - kCathedral5/\"; else echo \"[ERROR] No local binary archive found for airwindows-kcathedral5\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "reverb", "cathedral" }
        },
        {
            "airwindows-kcosmos", "Airwindows kCosmos", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1626", "715 KB", "14%",
            "Airwindows - VST - kCosmos", "kcosmos.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/kCosmos.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/kCosmos-1.0.0-mpc-armv7.zip", "plugins/airwindows-kcosmos/thumb.png",
            "Vast cosmic reverb with predelay and early reflections.", "./plugins/airwindows-kcosmos/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "reverb", "ambient", "cosmic" }
        },
        {
            "airwindows-mackity", "Airwindows Mackity", "mmiroshnikov", "v1.0.0", "Saturation",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-05",
            "1433", "587 KB", "14%",
            "Airwindows - VST - Mackity", "mackity.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Mackity.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.1.0/Mackity-1.0.0-mpc-armv7.zip", "plugins/airwindows-mackity/thumb.png",
            "Mackie-style console distortion: drive it with the input trim, tame it with the output pad.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - Mackity\" && ZIP_PATH=\"./plugins/airwindows-mackity/airwindows-mackity.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.1.0/Mackity-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/airwindows-mackity\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.1.0/Mackity-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - Mackity/\"; elif [ -f \"./plugins/airwindows-mackity/mackity.so\" ]; then cp -rf ./plugins/airwindows-mackity/* \"$TARGET_SYNTH_DIR/Airwindows - VST - Mackity/\"; else echo \"[ERROR] No local binary archive found for airwindows-mackity\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "saturation", "distortion", "console" }
        },
        {
            "airwindows-matrixverb", "Airwindows MatrixVerb", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1571", "730 KB", "14%",
            "Airwindows - VST - MatrixVerb", "matrixverb.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/MatrixVerb.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/MatrixVerb-1.0.0-mpc-armv7.zip", "plugins/airwindows-matrixverb/thumb.png",
            "Classic algorithmic reverb with filter, damping, modulation and room size.", "./plugins/airwindows-matrixverb/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "reverb", "algorithmic" }
        },
        {
            "airwindows-pitchdelay", "Airwindows PitchDelay", "mmiroshnikov", "v1.0.0", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1624", "618 KB", "14%",
            "Airwindows - VST - PitchDelay", "pitchdelay.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/PitchDelay.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/PitchDelay-1.0.0-mpc-armv7.zip", "plugins/airwindows-pitchdelay/thumb.png",
            "Delay with pitch shifting in the feedback path and a resonant filter.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - PitchDelay\" && ZIP_PATH=\"./plugins/airwindows-pitchdelay/airwindows-pitchdelay.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/PitchDelay-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/airwindows-pitchdelay\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/PitchDelay-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - PitchDelay/\"; elif [ -f \"./plugins/airwindows-pitchdelay/pitchdelay.so\" ]; then cp -rf ./plugins/airwindows-pitchdelay/* \"$TARGET_SYNTH_DIR/Airwindows - VST - PitchDelay/\"; else echo \"[ERROR] No local binary archive found for airwindows-pitchdelay\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "delay", "pitch", "filter" }
        },
        {
            "airwindows-tapedelay2", "Airwindows TapeDelay2", "mmiroshnikov", "v1.0.0", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1680", "548 KB", "14%",
            "Airwindows - VST - TapeDelay2", "tapedelay2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/TapeDelay2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/TapeDelay2-1.0.0-mpc-armv7.zip", "plugins/airwindows-tapedelay2/thumb.png",
            "Tape-style delay with flutter and a resonant filter in the repeats.", "./plugins/airwindows-tapedelay2/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "delay", "tape", "flutter" }
        },
        {
            "airwindows-tovinyl4", "Airwindows ToVinyl4", "mmiroshnikov", "v1.0.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1557", "738 KB", "14%",
            "Airwindows - VST - ToVinyl4", "tovinyl4.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/ToVinyl4.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ToVinyl4-1.0.0-mpc-armv7.zip", "plugins/airwindows-tovinyl4/thumb.png",
            "Vinyl mastering stage: mid and side highpass, high limiter and groove wear.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - ToVinyl4\" && ZIP_PATH=\"./plugins/airwindows-tovinyl4/airwindows-tovinyl4.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ToVinyl4-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/airwindows-tovinyl4\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ToVinyl4-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - ToVinyl4/\"; elif [ -f \"./plugins/airwindows-tovinyl4/tovinyl4.so\" ]; then cp -rf ./plugins/airwindows-tovinyl4/* \"$TARGET_SYNTH_DIR/Airwindows - VST - ToVinyl4/\"; else echo \"[ERROR] No local binary archive found for airwindows-tovinyl4\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "vinyl", "mastering", "lo-fi" }
        },
        {
            "airwindows-verbity2", "Airwindows Verbity2", "mmiroshnikov", "v1.0.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1535", "779 KB", "14%",
            "Airwindows - VST - Verbity2", "verbity2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/Verbity2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/Verbity2-1.0.0-mpc-armv7.zip", "plugins/airwindows-verbity2/thumb.png",
            "Dense, smooth reverb with size, sustain and mulch controls.", "./plugins/airwindows-verbity2/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "reverb", "dense" }
        },
        {
            "airwindows-zacidlowpass", "Airwindows ZAcidLowpass", "mmiroshnikov", "v1.0.0", "Filter",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1518", "685 KB", "14%",
            "Airwindows - VST - ZAcidLowpass", "zacidlowpass.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/ZAcidLowpass.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ZAcidLowpass-1.0.0-mpc-armv7.zip", "plugins/airwindows-zacidlowpass/thumb.png",
            "Acid-style overdriven resonant lowpass filter.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Airwindows - VST - ZAcidLowpass\" && ZIP_PATH=\"./plugins/airwindows-zacidlowpass/airwindows-zacidlowpass.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ZAcidLowpass-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/airwindows-zacidlowpass\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ZAcidLowpass-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Airwindows - VST - ZAcidLowpass/\"; elif [ -f \"./plugins/airwindows-zacidlowpass/zacidlowpass.so\" ]; then cp -rf ./plugins/airwindows-zacidlowpass/* \"$TARGET_SYNTH_DIR/Airwindows - VST - ZAcidLowpass/\"; else echo \"[ERROR] No local binary archive found for airwindows-zacidlowpass\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "airwindows", "filter", "lowpass", "acid" }
        },
        {
            "airwindows-zlowpass2", "Airwindows ZLowpass2", "mmiroshnikov", "v1.0.0", "Filter",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "1508", "694 KB", "14%",
            "Airwindows - VST - ZLowpass2", "zlowpass2.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-airwindows/main/docs/screenshots/ZLowpass2.jpg", "https://github.com/mmiroshnikov/mpc-airwindows/releases/download/v1.0.0/ZLowpass2-1.0.0-mpc-armv7.zip", "plugins/airwindows-zlowpass2/thumb.png",
            "Smooth lowpass filter with a choice of slopes.", "./plugins/airwindows-zlowpass2/install.sh", 0,
            0,
            false,
            false,
            "",
            { "airwindows", "filter", "lowpass" }
        },
        {
            "alpha", "ALPHA", "L'Cronx", "v1.2.1", "Granular",
            "FX", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-11-11",
            "577", "7.6 MB", "14%",
            "L'Cronx - VST - ALPHA", "alpha.so",
            "https://raw.githubusercontent.com/sunskiefer/ALPHA/v1.2.1/docs/screens/ALPHA_page_01_Pitch.png", "https://github.com/sunskiefer/ALPHA/releases/download/v1.2.1/ALPHA-1.2.1-mpc-armv7.zip", "plugins/alpha/thumb.png",
            "A granular vocal transformer: pitch and formant move separately, with harmonic shifter, alternator, smear, detune, scale/key TUNE, four modulation slots and 492 factory presets. Put it on an input or an insert and sing or play a voice through it, with or without MIDI. MPC OS 3.x.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - ALPHA\" && ZIP_PATH=\"./plugins/alpha/alpha.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/ALPHA/releases/download/v1.2.1/ALPHA-1.2.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/alpha\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/ALPHA/releases/download/v1.2.1/ALPHA-1.2.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - ALPHA/\"; elif [ -f \"./plugins/alpha/alpha.so\" ]; then cp -rf ./plugins/alpha/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - ALPHA/\"; else echo \"[ERROR] No local binary archive found for alpha\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "vocal", "granular", "pitch-shifter", "formant", "harmonizer", "autotune", "modulation", "sound-design" }
        },
        {
            "boris-granular", "Boris Granular", "sd88me", "v1.0.1", "Granular",
            "FX", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "2122", "10.3 MB", "14%",
            "sd88me - VST - Boris Granular", "boris_granular.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-boris/2d7b1360c68ccc91b22b0c5030152e08dd3682b8/docs/screenshot-force.png", "https://github.com/sd88me/mpc-vst-boris/releases/download/v1.0.1/Boris-Granular-1.0.1-mpc-armv7.zip", "plugins/boris-granular/thumb.png",
            "Real-time granular insert effect: incoming audio fills a 10 s buffer and up to 24 overlapping grains replay it with random size, delay, pitch, direction and pan. MPC tempo sync, freeze, feedback, 22 presets and a one-page touchscreen skin with Q-Links. A port of Boris Granular Station by Alessandro Gaiba.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Boris Granular\" && ZIP_PATH=\"./plugins/boris-granular/boris-granular.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-boris/releases/download/v1.0.1/Boris-Granular-1.0.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/boris-granular\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-boris/releases/download/v1.0.1/Boris-Granular-1.0.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Boris Granular/\"; elif [ -f \"./plugins/boris-granular/boris_granular.so\" ]; then cp -rf ./plugins/boris-granular/* \"$TARGET_SYNTH_DIR/sd88me - VST - Boris Granular/\"; else echo \"[ERROR] No local binary archive found for boris-granular\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "granular", "freeze", "tempo-sync", "texture", "pads", "shimmer", "stutter" }
        },
        {
            "clif-tape", "Clif Tape", "MagicStino", "v0.2.0", "Effect",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "731", "1.6 MB", "14%",
            "MagicStino - VST - Clif Tape", "clif_tape.so",
            "https://raw.githubusercontent.com/MagicStino/Clif-Tape/v0.2.0/catalog-thumbnail.png", "https://github.com/MagicStino/Clif-Tape/releases/download/v0.2.0/Clif-Tape-0.2.0-mpc-armv7.zip", "plugins/clif-tape/thumb.png",
            "Stereo tape colour and digital degradation, expressive Wow/Flutter, twelve tone profiles, crackle, stereo spread, independent noise textures and Silence Gate. Fifteen presets and touchscreen Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/MagicStino - VST - Clif Tape\" && ZIP_PATH=\"./plugins/clif-tape/clif-tape.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/MagicStino/Clif-Tape/releases/download/v0.2.0/Clif-Tape-0.2.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/clif-tape\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/MagicStino/Clif-Tape/releases/download/v0.2.0/Clif-Tape-0.2.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/MagicStino - VST - Clif Tape/\"; elif [ -f \"./plugins/clif-tape/clif_tape.so\" ]; then cp -rf ./plugins/clif-tape/* \"$TARGET_SYNTH_DIR/MagicStino - VST - Clif Tape/\"; else echo \"[ERROR] No local binary archive found for clif-tape\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "tape", "lo-fi", "stereo", "effect" }
        },
        {
            "ctrl-mixbus", "CTRL MixBus", "tonyalonsotech", "v2.2.0", "Utility",
            "FX", "Stable", "MPC One (Gen2) (3.10.1), MPC Live III (Gen2) (3.10.1), Force (Gen1) (3.10.1)", "MPC OS 3.x", "2026-11-14",
            "597", "9.8 MB", "14%",
            "CTRL - VST - CTRL MixBus", "ctrl_mixbus.so",
            "https://raw.githubusercontent.com/tonyalonsotech/CTRL-MixBus/v2.1.0/docs/preview.png", "https://github.com/tonyalonsotech/CTRL-MixBus/releases/download/v2.2.0/CTRL-MixBus-2.2.0-mpc-armv7.zip", "plugins/ctrl-mixbus/thumb.png",
            "CTRL Mixbus is a professional stereo mixing and processing effect designed to streamline your workflow across three focused pages. Take control of input gain, stereo width, equalization, precision filtering, dynamic gating, and loudness maximization.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/CTRL - VST - CTRL MixBus\" && ZIP_PATH=\"./plugins/ctrl-mixbus/ctrl-mixbus.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/tonyalonsotech/CTRL-MixBus/releases/download/v2.2.0/CTRL-MixBus-2.2.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/ctrl-mixbus\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/tonyalonsotech/CTRL-MixBus/releases/download/v2.2.0/CTRL-MixBus-2.2.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/CTRL - VST - CTRL MixBus/\"; elif [ -f \"./plugins/ctrl-mixbus/ctrl_mixbus.so\" ]; then cp -rf ./plugins/ctrl-mixbus/* \"$TARGET_SYNTH_DIR/CTRL - VST - CTRL MixBus/\"; else echo \"[ERROR] No local binary archive found for ctrl-mixbus\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "mixing", "dj-eq", "isolator", "filter", "stereo-width", "gate", "maximizer", "metering" }
        },
        {
            "op1fx-cwo", "Cwo", "Bombest", "v0.4.1", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "655", "4.2 MB", "14%",
            "bombest - VST - Cwo", "cwo.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/cwo.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Cwo-0.4.1-mpc-armv7.zip", "plugins/op1fx-cwo/thumb.png",
            "Stereo delay with a frequency shifter in the feedback loop, plus a MIX knob. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Cwo\" && ZIP_PATH=\"./plugins/op1fx-cwo/op1fx-cwo.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Cwo-0.4.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-cwo\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Cwo-0.4.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/bombest - VST - Cwo/\"; elif [ -f \"./plugins/op1fx-cwo/cwo.so\" ]; then cp -rf ./plugins/op1fx-cwo/* \"$TARGET_SYNTH_DIR/bombest - VST - Cwo/\"; else echo \"[ERROR] No local binary archive found for op1fx-cwo\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "delay", "frequency-shifter", "feedback" }
        },
        {
            "daemon-listen", "Daemon Listen", "sd88me", "v1.0.1", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-21",
            "878", "64 KB", "14%",
            "sd88me - VST - Daemon Listen", "daemonlisten.so",
            "", "https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-Listen-1.0.1-mpc-armv7.zip", "plugins/daemon-listen/thumb.png",
            "Companion to Daemon: insert it on the audio track that carries your mic, and Daemon's TALK button hears you. Passes audio through unchanged. Needs Daemon in the same project.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Daemon Listen\" && ZIP_PATH=\"./plugins/daemon-listen/daemon-listen.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-Listen-1.0.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/daemon-listen\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-Listen-1.0.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Daemon Listen/\"; elif [ -f \"./plugins/daemon-listen/daemonlisten.so\" ]; then cp -rf ./plugins/daemon-listen/* \"$TARGET_SYNTH_DIR/sd88me - VST - Daemon Listen/\"; else echo \"[ERROR] No local binary archive found for daemon-listen\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "voice", "mic", "ai" }
        },
        {
            "spectral-sidechain", "DAVIS Spectral Sidechain", "elizmita", "v1.4.2", "Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-14",
            "26", "3.1 MB", "14%",
            "DAVIS - VST - Spectral Sidechain", "spectral_sidechain.so",
            "https://raw.githubusercontent.com/elizmita/davis-spectral-sidechain/spectral-sidechain-vst-v1.4.2/docs/preview.png", "https://github.com/elizmita/davis-spectral-sidechain/releases/download/spectral-sidechain-vst-v1.4.2/Spectral-Sidechain-1.4.2-mpc-armv7.zip", "plugins/spectral-sidechain/thumb.png",
            "One-band sidechain dynamic EQ: the kick ducks the bass frequencies, with a live spectrum display and a touchscreen skin.", "./plugins/spectral-sidechain/install.sh", 0,
            0,
            false,
            false,
            "",
            { "sidechain", "dynamic-eq", "eq" }
        },
        {
            "op1fx-delay", "Delay", "Bombest", "v0.4.1", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "686", "4.2 MB", "14%",
            "bombest - VST - Delay", "delay.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/delay.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Delay-0.4.1-mpc-armv7.zip", "plugins/op1fx-delay/thumb.png",
            "Four-knob delay with a 12-preset bank. OP-1 inspired.", "./plugins/op1fx-delay/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "delay", "echo", "tape" }
        },
        {
            "djfx-looper", "DJFX Looper Native", "Rare Data", "v1.1.3", "Glitch",
            "FX", "Stable", "Akai Force (3.10.0.49)", "MPC OS 3.x", "2026-11-02",
            "367", "1.6 MB", "1%",
            "Rare Data - VST - DJFX Looper Native", "DJFXLooperNative.so",
            "", "https://github.com/macdigi/djfx-looper/releases/download/v1.1.3/DJFX-Looper-Native-1.1.3-mpc-armv7.zip", "plugins/djfx-looper/thumb.png",
            "Three Q-Link knobs for short loops, speed bends and reverse playback, with FX bypass and an on-device quick guide.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Rare Data - VST - DJFX Looper Native\" && ZIP_PATH=\"./plugins/djfx-looper/djfx-looper.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/macdigi/djfx-looper/releases/download/v1.1.3/DJFX-Looper-Native-1.1.3-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/djfx-looper\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/macdigi/djfx-looper/releases/download/v1.1.3/DJFX-Looper-Native-1.1.3-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Rare Data - VST - DJFX Looper Native/\"; elif [ -f \"./plugins/djfx-looper/DJFXLooperNative.so\" ]; then cp -rf ./plugins/djfx-looper/* \"$TARGET_SYNTH_DIR/Rare Data - VST - DJFX Looper Native/\"; else echo \"[ERROR] No local binary archive found for djfx-looper\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "looper", "reverse", "performance" }
        },
        {
            "dragonfly-early-reflections", "Dragonfly Early Reflections", "gmorb", "v1.3.1", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "666", "766 KB", "14%",
            "Dragonfly - VST - Early Reflections", "dragonfly_early.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-early-reflections/2044beed9f510a3963df7b61da6a4c19c6c7e7b3/docs/banners/early.png", "https://github.com/gmorb/mpc-vst-dragonfly-early-reflections/releases/download/v1.3.1/Early-Reflections-1.3.1-mpc-armv7.zip", "plugins/dragonfly-early-reflections/thumb.png",
            "Dragonfly Early Reflections as a native insert effect, with a touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Dragonfly - VST - Early Reflections\" && ZIP_PATH=\"./plugins/dragonfly-early-reflections/dragonfly-early-reflections.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/gmorb/mpc-vst-dragonfly-early-reflections/releases/download/v1.3.1/Early-Reflections-1.3.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/dragonfly-early-reflections\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/gmorb/mpc-vst-dragonfly-early-reflections/releases/download/v1.3.1/Early-Reflections-1.3.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Dragonfly - VST - Early Reflections/\"; elif [ -f \"./plugins/dragonfly-early-reflections/dragonfly_early.so\" ]; then cp -rf ./plugins/dragonfly-early-reflections/* \"$TARGET_SYNTH_DIR/Dragonfly - VST - Early Reflections/\"; else echo \"[ERROR] No local binary archive found for dragonfly-early-reflections\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "reverb", "early-reflections", "dragonfly" }
        },
        {
            "dragonfly-hall", "Dragonfly Hall", "gmorb", "v1.3.0", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "1848", "2.4 MB", "14%",
            "Dragonfly - VST - Hall", "dragonfly_hall.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-hall/eb9f41f22ea93414776db40760c2387b36cf3144/docs/banners/hall.png", "https://github.com/gmorb/mpc-vst-dragonfly-hall/releases/download/v1.3.0/Hall-1.3.0-mpc-armv7.zip", "plugins/dragonfly-hall/thumb.png",
            "Dragonfly Hall reverb as a native insert effect, with a touchscreen skin and Q-Links.", "./plugins/dragonfly-hall/install.sh", 0,
            0,
            false,
            false,
            "",
            { "reverb", "hall", "dragonfly" }
        },
        {
            "dragonfly-plate", "Dragonfly Plate", "gmorb", "v1.3.0", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "1747", "1.2 MB", "14%",
            "Dragonfly - VST - Plate", "dragonfly_plate.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-plate/a9087385948d18ae38739c44ddf42ab826d1a567/docs/banners/plate.png", "https://github.com/gmorb/mpc-vst-dragonfly-plate/releases/download/v1.3.0/Plate-1.3.0-mpc-armv7.zip", "plugins/dragonfly-plate/thumb.png",
            "Dragonfly Plate reverb as a native insert effect, with a touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Dragonfly - VST - Plate\" && ZIP_PATH=\"./plugins/dragonfly-plate/dragonfly-plate.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/gmorb/mpc-vst-dragonfly-plate/releases/download/v1.3.0/Plate-1.3.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/dragonfly-plate\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/gmorb/mpc-vst-dragonfly-plate/releases/download/v1.3.0/Plate-1.3.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Dragonfly - VST - Plate/\"; elif [ -f \"./plugins/dragonfly-plate/dragonfly_plate.so\" ]; then cp -rf ./plugins/dragonfly-plate/* \"$TARGET_SYNTH_DIR/Dragonfly - VST - Plate/\"; else echo \"[ERROR] No local binary archive found for dragonfly-plate\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "reverb", "plate", "dragonfly" }
        },
        {
            "dragonfly-room", "Dragonfly Room", "gmorb", "v1.3.0", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "1699", "2.0 MB", "14%",
            "Dragonfly - VST - Room", "dragonfly_room.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-room/6c62e1a5c9ef57f0c31897afa0faf07d7a446585/docs/banners/room.png", "https://github.com/gmorb/mpc-vst-dragonfly-room/releases/download/v1.3.0/Room-1.3.0-mpc-armv7.zip", "plugins/dragonfly-room/thumb.png",
            "Dragonfly Room reverb as a native insert effect, with a touchscreen skin and Q-Links.", "./plugins/dragonfly-room/install.sh", 0,
            0,
            false,
            false,
            "",
            { "reverb", "room", "dragonfly" }
        },
        {
            "drips", "Drips", "Padbangers", "v1.1.0", "Granular",
            "FX", "Stable", "Akai MPC X (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-14",
            "1550", "3.6 MB", "14%",
            "Padbangers - VST - Drips", "VST/drips-arm32.so",
            "https://raw.githubusercontent.com/padbangers/drips/85683cebf7b4fcf1716ed6f32faa89041768b24e/docs/drips-main.png", "https://github.com/padbangers/drips/releases/download/drips-v1.1.0/Drips-1.1.0-mpc-armv7.zip", "plugins/drips/thumb.png",
            "Drips: a granular texture synthesizer with four recording qualities, a delay and beat slicer, tempo-synced seeding, wavetables and a modulation page, from Émilie Gillet's open-source Beads.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Padbangers - VST - Drips\" && ZIP_PATH=\"./plugins/drips/drips.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/padbangers/drips/releases/download/drips-v1.1.0/Drips-1.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/drips\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/padbangers/drips/releases/download/drips-v1.1.0/Drips-1.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Padbangers - VST - Drips/\"; elif [ -f \"./plugins/drips/VST/drips-arm32.so\" ]; then cp -rf ./plugins/drips/* \"$TARGET_SYNTH_DIR/Padbangers - VST - Drips/\"; else echo \"[ERROR] No local binary archive found for drips\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "granular", "texture", "delay", "beat-slicer", "wavetable", "reverb", "tempo-sync" }
        },
        {
            "dub-station", "Dub Station", "nachtaktiv303", "v1.0.2", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2408", "2.1 MB", "14%",
            "Lukas - VST - Dub Station", "dub_station.so",
            "https://raw.githubusercontent.com/nachtaktiv303/mpc-vst-dub-station/main/preview_0.png", "https://github.com/nachtaktiv303/mpc-vst-dub-station/releases/download/dub-station-vst-v1.0.2/Dub-Station-1.0.2-mpc-armv7.zip", "plugins/dub-station/thumb.png",
            "Dub-techno multi-effect: two independent tape delays, a huge modulated reverb, tape grit, a noise/crackle ambience bed, phaser, ducking and a panic kill.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Lukas - VST - Dub Station\" && ZIP_PATH=\"./plugins/dub-station/dub-station.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/nachtaktiv303/mpc-vst-dub-station/releases/download/dub-station-vst-v1.0.2/Dub-Station-1.0.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/dub-station\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/nachtaktiv303/mpc-vst-dub-station/releases/download/dub-station-vst-v1.0.2/Dub-Station-1.0.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Lukas - VST - Dub Station/\"; elif [ -f \"./plugins/dub-station/dub_station.so\" ]; then cp -rf ./plugins/dub-station/* \"$TARGET_SYNTH_DIR/Lukas - VST - Dub Station/\"; else echo \"[ERROR] No local binary archive found for dub-station\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "dub", "delay", "tape", "reverb", "space-echo", "multi-effect" }
        },
        {
            "effectwerk", "EffectWerk", "Devko", "v0.0.5", "Multi Fx",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "571", "3.0 MB", "14%",
            "Devko - VST - EffectWerk", "effectwerk.so",
            "https://raw.githubusercontent.com/Devko/EffectWerk/v0.0.1/docs/img/looper.png", "https://github.com/Devko/EffectWerk/releases/download/v0.0.5/EffectWerk-0.0.5-mpc-armv7.zip", "plugins/effectwerk/thumb.png",
            "A live looper and performance mixer in one insert slot: record bars on MPC's grid, then roll, slow, reverse and layer them, and crossfade two scenes from 64 one-tap effects. Plus a ten-module effect rack with macros, LFOs and a mod matrix, 62 presets and a 14-page touchscreen skin with Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Devko - VST - EffectWerk\" && ZIP_PATH=\"./plugins/effectwerk/effectwerk.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Devko/EffectWerk/releases/download/v0.0.5/EffectWerk-0.0.5-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/effectwerk\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Devko/EffectWerk/releases/download/v0.0.5/EffectWerk-0.0.5-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Devko - VST - EffectWerk/\"; elif [ -f \"./plugins/effectwerk/effectwerk.so\" ]; then cp -rf ./plugins/effectwerk/* \"$TARGET_SYNTH_DIR/Devko - VST - EffectWerk/\"; else echo \"[ERROR] No local binary archive found for effectwerk\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "looper", "performance-mixer", "octatrack-style", "crossfader", "scenes", "effect-rack", "granular", "shimmer-reverb" }
        },
        {
            "ghost", "GHOST", "MPC-EURO", "v2.4.2", "Glitch",
            "FX", "Beta", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "364", "812 KB", "14%",
            "MPC-EURO - VST - GHOST", "ghost.so",
            "", "https://github.com/carballocba/MPC-EURO-/releases/download/v2.4.2-installer2-rc3/GHOST-2.4.2-installer2-rc3-mpc-armv7.zip", "plugins/ghost/thumb.png",
            "Live buffer manipulation, rhythmic stutter and generative glitch with independent and combined Random Repeat modes.", "./plugins/ghost/install.sh", 0,
            0,
            false,
            false,
            "",
            { "buffer", "stutter", "reverse", "generative", "performance" }
        },
        {
            "op1fx-grid", "Grid", "Bombest", "v0.4.1", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "596", "9.5 MB", "14%",
            "bombest - VST - Grid", "grid.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/grid.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Grid-0.4.1-mpc-armv7.zip", "plugins/op1fx-grid/thumb.png",
            "A 2D waveguide mesh you feed audio and read back. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Grid\" && ZIP_PATH=\"./plugins/op1fx-grid/op1fx-grid.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Grid-0.4.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-grid\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Grid-0.4.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/bombest - VST - Grid/\"; elif [ -f \"./plugins/op1fx-grid/grid.so\" ]; then cp -rf ./plugins/op1fx-grid/* \"$TARGET_SYNTH_DIR/bombest - VST - Grid/\"; else echo \"[ERROR] No local binary archive found for op1fx-grid\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "reverb", "waveguide", "resonator" }
        },
        {
            "incipience-out", "Incipience OUT", "saustin2010", "v1.1.1", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1188", "104 KB", "14%",
            "Steve A - VST - [FX] Incipience OUT", "incipienceout.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-incipience/v1.1.1/incipienceout/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-incipience/releases/download/incipience-out-v1.1.1/FX-Incipience-OUT-1.1.1-mpc-armv7.zip", "plugins/incipience-out/thumb.png",
            "Companion to Incipience: plays one of its OUT 1-11 on its own MPC track (an audio track will do), so an instrument gets that track's fader, inserts and outputs, in time with the kit. Needs Incipience 1.0.0 or later in the same project.", "./plugins/incipience-out/install.sh", 0,
            0,
            false,
            false,
            "",
            { "routing", "multi-out", "drum" }
        },
        {
            "keyscope", "Keyscope", "jacob-sabella", "v1.1.1", "Utility",
            "FX", "Stable", "MPC Key 37 (3.10.1.12), MPC Sample (1.3.0.12)", "MPC OS 2.x & 3.x", "2026-11-12",
            "3874", "3.2 MB", "4%",
            "jacob-sabella - VST - Keyscope", "keyscope.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-keyscope/88cb99ac5e0d682e1e6d9c81836bf4096a9cde3c/docs/screenshots/keyscope.png", "https://github.com/jacob-sabella/mpc-vst-keyscope/releases/download/keyscope-vst-v1.1.1/Keyscope-1.1.1-mpc-armv7.zip", "plugins/keyscope/thumb.png",
            "Listens to any track and names its key, chord, bass note and tuning, from the spectrum of the whole mix; the audio passes through untouched.", "./plugins/keyscope/install.sh", 0,
            0,
            false,
            false,
            "",
            { "key-detection", "chord-detection", "tuner", "analyzer" }
        },
        {
            "liminal-amp", "Liminal Amp", "gmorb", "v1.1.0", "Amp Sim",
            "FX", "Stable", "Akai Force (Gen1) (3.10.1), MPC Live 3 (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-13",
            "2039", "5.5 MB", "14%",
            "Gm0rb - VST - Liminal Amp", "liminal_amp.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-liminal-amp/v1.1.0/docs/images/page.png", "https://github.com/gmorb/mpc-vst-liminal-amp/releases/download/v1.1.0/Liminal-Amp-1.1.0-mpc-armv7.zip", "plugins/liminal-amp/thumb.png",
            "Lightweight, official-sound Neural Amp Modeler: A2 captures with cab IR, gate and tone stack, resampled like the official plugin; browses models/ and irs/ folders on any drive. One MPC OS 3.10.1 folder for every MPC.", "./plugins/liminal-amp/install.sh", 0,
            0,
            false,
            false,
            "",
            { "nam", "guitar", "amp", "cab-ir", "lightweight" }
        },
        {
            "liminal-hz", "Liminal Hz", "gmorb", "v1.4.1", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-13",
            "3387", "24.7 MB", "14%",
            "Gm0rb - VST - Liminal Hz", "liminal_hz.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-liminal-hz/main/docs/images/banner.png", "https://github.com/gmorb/mpc-vst-liminal-hz/releases/download/v1.4.1/Liminal-Hz-1.4.1-mpc-armv7.zip", "plugins/liminal-hz/thumb.png",
            "Real rooms, halls and echoes from impulse responses (up to 5 s, mono or stereo), the space for your NAM amp; ~3.5% of an audio block on a Force; browses reverbs/ folders and TONE3000.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Gm0rb - VST - Liminal Hz\" && ZIP_PATH=\"./plugins/liminal-hz/liminal-hz.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/gmorb/mpc-vst-liminal-hz/releases/download/v1.4.1/Liminal-Hz-1.4.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/liminal-hz\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/gmorb/mpc-vst-liminal-hz/releases/download/v1.4.1/Liminal-Hz-1.4.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Gm0rb - VST - Liminal Hz/\"; elif [ -f \"./plugins/liminal-hz/liminal_hz.so\" ]; then cp -rf ./plugins/liminal-hz/* \"$TARGET_SYNTH_DIR/Gm0rb - VST - Liminal Hz/\"; else echo \"[ERROR] No local binary archive found for liminal-hz\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "reverb", "delay", "echo", "convolution", "ir", "tone3000" }
        },
        {
            "loop-saboteurxxl", "Loop-SaboteurXXL", "L'Cronx", "v0.1.3", "Glitch",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "2024", "8.2 MB", "14%",
            "L'Cronx - VST - Loop-SaboteurXXL", "loop_saboteurxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/Loop-SaboteurXXL/v0.1.0/docs/img/3-acts-seq.png", "https://github.com/sunskiefer/Loop-SaboteurXXL/releases/download/v0.1.3/Loop-SaboteurXXL-0.1.3-mpc-armv7.zip", "plugins/loop-saboteurxxl/thumb.png",
            "A port of Loop Saboteur by Stephen McLeod Blythe (clickysteve): a glitch / chop / beat-slicer that records the last bars and replays them on the MPC tempo as retriggered, pitched, reversed, stretched and crushed slices. Eight Acts, a 64-step sequencer with trig conditions, ratchets and p-locks, and 19 factory presets.", "./plugins/loop-saboteurxxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "glitch", "slicer", "stutter", "beat-repeat", "sequencer", "p-lock", "lo-fi", "tape" }
        },
        {
            "lufest", "LUFest", "L'Cronx", "v0.1.1", "Utility",
            "FX", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-11-09",
            "351", "102 KB", "14%",
            "L'Cronx - VST - LUFest", "lufest.so",
            "https://raw.githubusercontent.com/sunskiefer/LUFest/v0.1.1/images/skin-background-0.1.1.png", "https://github.com/sunskiefer/LUFest/releases/download/v0.1.1/LUFest-0.1.1-mpc-armv7.zip", "plugins/lufest/thumb.png",
            "Loudness meter for the master channel's effects: momentary, short-term and integrated LUFS, LRA, 4x oversampled true peak, held L/R sample peaks, green/yellow/red level bars with a CLIP lamp, and large readouts. Audio passes through untouched. Tested on an Akai Force; numbers not yet checked against a reference meter. MPC OS 3.x.", "./plugins/lufest/install.sh", 0,
            0,
            false,
            false,
            "",
            { "loudness", "lufs", "meter", "true-peak", "master", "analyzer", "ebu-r128", "utility" }
        },
        {
            "machinemodule-tap-gen2", "Machinemodule Tap (Gen2)", "sd88me", "v1.0.5", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-11",
            "1469", "95 KB", "14%",
            "sd88me - VST - Machinemodule Tap", "machinedrum_tapfx.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-machinedrum/main/docs/img/machinemodule-tap-gen2.png", "https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.5/Machinemodule-Tap-1.0.5-mpc-aarch64.zip", "plugins/machinemodule-tap-gen2/thumb.png",
            "Puts any Machinemodule tracks, and its reverb and delay sends, on their own MPC return or FX track, so MPC's mixer and effects can process them. Needs Machinemodule in the same project. Gen2 (aarch64) only.", "./plugins/machinemodule-tap-gen2/install.sh", 0,
            0,
            false,
            false,
            "",
            { "elektron", "machinedrum", "gen2" }
        },
        {
            "magic-tape-echo", "Magic Tape Echo", "MagicStino", "v0.1.2", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1160", "2.6 MB", "14%",
            "MagicStino - VST - Magic Tape Echo", "magic_tape_echo.so",
            "https://raw.githubusercontent.com/MagicStino/Magic-Tape-Echo/a616cc2fad6bd7eb920111627bb1fc81631ff09a/magic-tape-echo-three-heads.png", "https://github.com/MagicStino/Magic-Tape-Echo/releases/download/v0.1.2/Magic-Tape-Echo-0.1.2-mpc-armv7.zip", "plugins/magic-tape-echo/thumb.png",
            "Three-head stereo tape echo with tape saturation, wow/flutter, tempo sync, optional spring reverb and a slow VU meter.", "./plugins/magic-tape-echo/install.sh", 0,
            0,
            false,
            false,
            "",
            { "delay", "tape", "echo", "spring" }
        },
        {
            "mantra", "MANTRA", "L'Cronx", "v0.2.0", "Multi Fx",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-14",
            "63", "16.2 MB", "14%",
            "L'Cronx - VST - MANTRA", "mantra.so",
            "https://raw.githubusercontent.com/sunskiefer/MANTRA/v0.2.0/docs/screenshots/page_0.png", "https://github.com/sunskiefer/MANTRA/releases/download/v0.2.0/MANTRA-0.2.0-mpc-armv7.zip", "plugins/mantra/thumb.png",
            "A rhythmic multi-effect: a pattern chops, filters, crushes, delays and glitches the sound in time with the song. Two layers of six effects (filter, distortion, delay, lo-fi, flanger, glitch) in any order, eight 32-step modulators locked to the song, a 24-slot matrix, per-page randomiser, 419 factory presets plus unlimited own. MPC OS 3.x.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - MANTRA\" && ZIP_PATH=\"./plugins/mantra/mantra.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/MANTRA/releases/download/v0.2.0/MANTRA-0.2.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/mantra\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/MANTRA/releases/download/v0.2.0/MANTRA-0.2.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - MANTRA/\"; elif [ -f \"./plugins/mantra/mantra.so\" ]; then cp -rf ./plugins/mantra/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - MANTRA/\"; else echo \"[ERROR] No local binary archive found for mantra\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "multi-fx", "gate", "filter", "delay", "glitch", "lo-fi", "sequencer", "sound-design" }
        },
        {
            "warps", "Meta Mod", "saustin2010", "v1.2.1", "Modulation",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2548", "1.0 MB", "14%",
            "Émilie Gillet - VST - [FX] Meta Mod", "warps.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-meta-mod/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-meta-mod/releases/download/v1.2.1/FX-Meta-Mod-1.2.1-mpc-armv7.zip", "plugins/warps/thumb.png",
            "Meta Mod: ring mod, folding, XOR, vocoder and frequency shifting as an insert effect; 12 presets, from Émilie Gillet's open-source Warps.", "./plugins/warps/install.sh", 0,
            0,
            false,
            false,
            "",
            { "ring-mod", "vocoder", "frequency-shifter", "eurorack" }
        },
        {
            "meta4mpcxxl-fx", "Meta4MPCXXL FX", "L'Cronx", "v0.1.2", "Modular",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "756", "11.2 MB", "14%",
            "L'Cronx - VST - Meta4MPCXXL FX", "meta4mpcxxl_fx.so",
            "https://raw.githubusercontent.com/sunskiefer/Meta4MPCXXL/v0.1.1/docs/img/meta4mpcxxl.png", "https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-FX-0.1.2-mpc-armv7.zip", "plugins/meta4mpcxxl-fx/thumb.png",
            "Meta4MPCXXL as an effect: the track's audio goes into a 4ms MetaModule-format (VCV Rack) patch's In 1 / In 2, with every parameter on Q-Links, MIDI and clock into the patch, and Wi-Fi patch upload from VCV Rack.", "./plugins/meta4mpcxxl-fx/install.sh", 0,
            0,
            false,
            false,
            "",
            { "modular", "vcv", "metamodule", "patch-player", "eurorack", "multi-fx" }
        },
        {
            "monomodule-fx-gen2", "Monomodule FX (Gen2)", "sd88me", "v1.0.4", "Multi Effect",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-04",
            "1259", "4.2 MB", "14%",
            "sd88me - VST - Monomodule FX", "monomodule_fx.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-monomodule/main/docs/img/monomodule-fx-gen2.png", "https://github.com/sd88me/mpc-vst-monomodule/releases/download/v1.0.4/Monomodule-FX-1.0.4-mpc-aarch64.zip", "plugins/monomodule-fx-gen2/thumb.png",
            "The Elektron Monomachine's seven effect machines (thru, reverb, chorus, dynamix, ring mod, phaser, flanger) as an audio effect for Gen2 MPC devices, with a touchscreen skin and Q-Links. A ready-made download with nothing of Elektron's in it: it reads your own Monomachine OS file from the plugin folder. Gen2 only.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Monomodule FX\" && ZIP_PATH=\"./plugins/monomodule-fx-gen2/monomodule-fx-gen2.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-monomodule/releases/download/v1.0.4/Monomodule-FX-1.0.4-mpc-aarch64.zip\" ]; then mkdir -p \"./plugins/monomodule-fx-gen2\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-monomodule/releases/download/v1.0.4/Monomodule-FX-1.0.4-mpc-aarch64.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Monomodule FX/\"; elif [ -f \"./plugins/monomodule-fx-gen2/monomodule_fx.so\" ]; then cp -rf ./plugins/monomodule-fx-gen2/* \"$TARGET_SYNTH_DIR/sd88me - VST - Monomodule FX/\"; else echo \"[ERROR] No local binary archive found for monomodule-fx-gen2\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "elektron", "monomachine", "gen2" }
        },
        {
            "mori", "MORI", "L'Cronx", "v0.6.4", "Delay",
            "FX", "Stable", "Akai Force (not recorded)", "MPC OS 3.x", "2026-11-14",
            "455", "8.3 MB", "14%",
            "L'Cronx - VST - MORI", "mori.so",
            "https://raw.githubusercontent.com/sunskiefer/MORI/v0.6.4/docs/screenshots/page_0.png", "https://github.com/sunskiefer/MORI/releases/download/v0.6.4/MORI-0.6.4-mpc-armv7.zip", "plugins/mori/thumb.png",
            "A love letter to MEMENTO | Enjoy-Lab, own code. A tempo-synced DUB loop, stereo tempo delay, multi-head, grain and vinyl TEXTURE memory, shimmer reverb, bipolar filter and master FX on a 14-way routing matrix. Two 16-step modulators, a three-zone touch bar, a browser editor, 69 presets plus your own. MPC OS 3.x.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - MORI\" && ZIP_PATH=\"./plugins/mori/mori.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/MORI/releases/download/v0.6.4/MORI-0.6.4-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/mori\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/MORI/releases/download/v0.6.4/MORI-0.6.4-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - MORI/\"; elif [ -f \"./plugins/mori/mori.so\" ]; then cp -rf ./plugins/mori/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - MORI/\"; else echo \"[ERROR] No local binary archive found for mori\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "delay", "reverb", "granular", "looper", "ambient", "dub", "sound-design", "filter" }
        },
        {
            "mpc-dj-fx-2", "MPC DJ FX 2", "mitchoster", "v3.2.0", "Multi Fx",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-09",
            "3247", "12.6 MB", "14%",
            "LarryDaimond - VST - MPC DJ FX 2", "mpc_djfx2.so",
            "https://raw.githubusercontent.com/mitchoster/mpc-vst-djfx2/0f20d92f/docs/ui-preview/page_1.png", "https://github.com/mitchoster/mpc-vst-djfx2/releases/download/v3.2.0/MPC-DJ-FX-2-3.2.0-mpc-armv7.zip", "plugins/mpc-dj-fx-2/thumb.png",
            "One-slot live DJ insert for MPC/Force: 3-band isolator, DJ filter, beat repeat, delay, reverb, through-zero flanger, grain delay, limiter, and eight performance macros measured from Lenny Kiser's DJ-FX rack.", "./plugins/mpc-dj-fx-2/install.sh", 0,
            0,
            false,
            false,
            "",
            { "dj", "isolator", "filter", "beat-repeat", "delay", "reverb", "performance", "macro" }
        },
        {
            "mpc-multihead-echo", "MPC Multihead Echo", "mitchoster", "v1.0.3", "Delay",
            "FX", "Stable", "MPC Live II (MPC OS 3.x (AZ0x))", "MPC OS 3.x", "2026-11-11",
            "982", "1.7 MB", "25%",
            "LarryDaimond - VST - MPC Multihead Echo", "mpc_multihead_echo.so",
            "https://raw.githubusercontent.com/mitchoster/mpc-vst-multihead-echo/c31c3ef479ce87083ab87fbef14c2fe92a6bb83b/docs/companion-panel-echo.png", "https://github.com/mitchoster/mpc-vst-multihead-echo/releases/download/v1.0.3/MPC-Multihead-Echo-1.0.3-mpc-armv7.zip", "plugins/mpc-multihead-echo/thumb.png",
            "Three-head tape echo with three preamp characters, chorus, tape wear, tempo sync with drift, a spring reverb and a companion screen in the browser.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/LarryDaimond - VST - MPC Multihead Echo\" && ZIP_PATH=\"./plugins/mpc-multihead-echo/mpc-multihead-echo.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mitchoster/mpc-vst-multihead-echo/releases/download/v1.0.3/MPC-Multihead-Echo-1.0.3-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/mpc-multihead-echo\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mitchoster/mpc-vst-multihead-echo/releases/download/v1.0.3/MPC-Multihead-Echo-1.0.3-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/LarryDaimond - VST - MPC Multihead Echo/\"; elif [ -f \"./plugins/mpc-multihead-echo/mpc_multihead_echo.so\" ]; then cp -rf ./plugins/mpc-multihead-echo/* \"$TARGET_SYNTH_DIR/LarryDaimond - VST - MPC Multihead Echo/\"; else echo \"[ERROR] No local binary archive found for mpc-multihead-echo\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "tape-echo", "delay", "saturation", "spring-reverb", "tempo-sync", "companion-app" }
        },
        {
            "mpc-tape-echo", "MPC Tape Echo", "mitchoster", "v1.3.0", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-09",
            "1895", "9.7 MB", "14%",
            "LarryDaimond - VST - MPC Tape Echo", "mpc_tape_echo.so",
            "https://raw.githubusercontent.com/mitchoster/mpc-vst-tape-echo/main/docs/img/panel.png", "https://github.com/mitchoster/mpc-vst-tape-echo/releases/download/v1.3.0/MPC-Tape-Echo-1.3.0-mpc-armv7.zip", "plugins/mpc-tape-echo/thumb.png",
            "Three-head tape echo with a spring tank: 12 head and reverb modes, tape saturation, wow and flutter, tempo sync, live VU.", "./plugins/mpc-tape-echo/install.sh", 0,
            0,
            false,
            false,
            "",
            { "delay", "tape", "echo", "spring" }
        },
        {
            "nam", "NAM", "jacob-sabella", "v1.1.0", "Amp Sim",
            "FX", "Stable", "MPC Sample (1.3.0.12)", "MPC OS 2.x & 3.x", "2026-11-13",
            "5632", "2.8 MB", "14%",
            "jacob-sabella - VST - NAM", "nam_vst.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-nam/f2fa28c8f4086606e929732a4824e72579bdcf80/docs/screenshots/nam.png", "https://github.com/jacob-sabella/mpc-vst-nam/releases/download/nam-vst-v1.1.0/NAM-1.1.0-mpc-armv7.zip", "plugins/nam/thumb.png",
            "Neural Amp Modeler captures as an insert effect, with cab IR, tone, gate, pitch, delay, reverb and an on-device TONE3000 downloader.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - NAM\" && ZIP_PATH=\"./plugins/nam/nam.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/jacob-sabella/mpc-vst-nam/releases/download/nam-vst-v1.1.0/NAM-1.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/nam\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-vst-nam/releases/download/nam-vst-v1.1.0/NAM-1.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - NAM/\"; elif [ -f \"./plugins/nam/nam_vst.so\" ]; then cp -rf ./plugins/nam/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - NAM/\"; else echo \"[ERROR] No local binary archive found for nam\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "nam", "guitar", "amp", "cab-ir", "tone3000" }
        },
        {
            "op1fx-nitro", "Nitro", "Bombest", "v0.4.1", "Filter",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "579", "4.9 MB", "14%",
            "bombest - VST - Nitro", "nitro.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/nitro.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Nitro-0.4.1-mpc-armv7.zip", "plugins/op1fx-nitro/thumb.png",
            "Filter with a drive stage and a MIX knob, 12 presets. OP-1 inspired.", "./plugins/op1fx-nitro/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "filter", "drive", "dj" }
        },
        {
            "ottmpc", "OTTmpc", "sd88me", "v1.0.1", "Compressor",
            "FX", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "1948", "1.7 MB", "14%",
            "sd88me - VST - OTTmpc", "ottmpc.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-OTT/claude/friendly-wozniak-m9ak1l/docs/screenshot.png", "https://github.com/sd88me/mpc-vst-OTT/releases/download/ottmpc-vst-v1.0.1/OTTmpc-1.0.1-mpc-armv7.zip", "plugins/ottmpc/thumb.png",
            "Vital's OTT multiband up/down compressor as an insert effect: live band meters with dB numbers, 12 Q-Link controls and 10 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - OTTmpc\" && ZIP_PATH=\"./plugins/ottmpc/ottmpc.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-OTT/releases/download/ottmpc-vst-v1.0.1/OTTmpc-1.0.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/ottmpc\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-OTT/releases/download/ottmpc-vst-v1.0.1/OTTmpc-1.0.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - OTTmpc/\"; elif [ -f \"./plugins/ottmpc/ottmpc.so\" ]; then cp -rf ./plugins/ottmpc/* \"$TARGET_SYNTH_DIR/sd88me - VST - OTTmpc/\"; else echo \"[ERROR] No local binary archive found for ottmpc\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "ott", "multiband", "compressor", "dynamics" }
        },
        {
            "overcast", "Overcast", "Padbangers", "v1.1.0", "Granular",
            "FX", "Stable", "Akai MPC X (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-14",
            "5830", "3.9 MB", "14%",
            "Padbangers - VST - Overcast", "VST/overcast-arm32.so",
            "https://raw.githubusercontent.com/padbangers/overcast/overcast-v1.1.0/docs/overcast-main.png", "https://github.com/padbangers/overcast/releases/download/overcast-v1.1.0/Overcast-1.1.0-mpc-armv7.zip", "plugins/overcast/thumb.png",
            "Mutable Instruments Clouds (Parasites firmware, six modes) with a modulation page: two envelopes, two LFOs and an 8-slot matrix onto Clouds' CV inputs.", "./plugins/overcast/install.sh", 0,
            0,
            false,
            false,
            "",
            { "mutable-instruments", "clouds", "parasites", "granular", "texture", "reverb", "delay", "eurorack" }
        },
        {
            "percolator-out", "Percolator OUT", "saustin2010", "v2.1.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1482", "147 KB", "14%",
            "Steve A - VST - [FX] Percolator OUT", "percolatorout.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-percolator/v2.1.0/percolatorout/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-percolator/releases/download/percolator-out-v2.1.0/FX-Percolator-OUT-2.1.0-mpc-armv7.zip", "plugins/percolator-out/thumb.png",
            "Companion to Percolator: plays one of its OUT 1-4 on its own MPC track (an audio track will do), so a voice gets that track's fader, inserts and outputs, in time with the kit. Needs Percolator 2.0.0 or later in the same project.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Steve A - VST - [FX] Percolator OUT\" && ZIP_PATH=\"./plugins/percolator-out/percolator-out.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-percolator/releases/download/percolator-out-v2.1.0/FX-Percolator-OUT-2.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/percolator-out\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-percolator/releases/download/percolator-out-v2.1.0/FX-Percolator-OUT-2.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Steve A - VST - [FX] Percolator OUT/\"; elif [ -f \"./plugins/percolator-out/percolatorout.so\" ]; then cp -rf ./plugins/percolator-out/* \"$TARGET_SYNTH_DIR/Steve A - VST - [FX] Percolator OUT/\"; else echo \"[ERROR] No local binary archive found for percolator-out\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "routing", "multi-out", "drum" }
        },
        {
            "op1fx-phone", "Phone", "Bombest", "v0.4.1", "Filter",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "544", "5.1 MB", "14%",
            "bombest - VST - Phone", "phone.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/phone.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Phone-0.4.1-mpc-armv7.zip", "plugins/op1fx-phone/thumb.png",
            "Telephone-band squash with a 12-preset bank. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/bombest - VST - Phone\" && ZIP_PATH=\"./plugins/op1fx-phone/op1fx-phone.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Phone-0.4.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-phone\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Phone-0.4.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/bombest - VST - Phone/\"; elif [ -f \"./plugins/op1fx-phone/phone.so\" ]; then cp -rf ./plugins/op1fx-phone/* \"$TARGET_SYNTH_DIR/bombest - VST - Phone/\"; else echo \"[ERROR] No local binary archive found for op1fx-phone\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "telephone", "band-limit", "lo-fi" }
        },
        {
            "phyxxl", "PhyXXL", "L'Cronx", "v0.1.6", "Filter",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-25",
            "1784", "18.3 MB", "14%",
            "L'Cronx - VST - PhyXXL", "phyxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/PhyXXL/v0.1.2/docs/img/overview.png", "https://github.com/sunskiefer/PhyXXL/releases/download/v0.1.6/PhyXXL-0.1.6-mpc-armv7.zip", "plugins/phyxxl/thumb.png",
            "Eight filters in the style of classic synths (MS-20, SEM, Minimoog, Matrix-12, 2600, Wasp, prologue, Polivoks) in one effect. Route audio from any filter into any other; modulate each from ten sources by touch patch bay or matrix. 28 patches. By L'Cronx, GPL-3. No third-party code; not endorsed by the makers of the originals.", "./plugins/phyxxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "filter", "multi-filter", "ms-20", "sem", "moog", "matrix-12", "arp-2600", "polivoks" }
        },
        {
            "plateauxxl", "PlateauXXL", "L'Cronx", "v0.4.3", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "2419", "12.9 MB", "14%",
            "L'Cronx - VST - PlateauXXL", "plateauxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/PlateauXXL/v0.4.1/docs/img/plateau.png", "https://github.com/sunskiefer/PlateauXXL/releases/download/v0.4.3/PlateauXXL-0.4.3-mpc-armv7.zip", "plugins/plateauxxl/thumb.png",
            "Valley Audio's Plateau reverb from VCV Rack, patched: every CV input on a built-in source (four Bogaudio LFOs, Tidal Modulator 2, Random Sampler, step and gate sequencers) synced to the MPC tempo, a brickwall limiter, Panic and 16 presets, each page drawn like its module.", "./plugins/plateauxxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "reverb", "plate", "dattorro", "valley", "vcv", "freeze", "lfo", "modulation" }
        },
        {
            "potentialbleed", "PotentialBleed", "PotentialBreaks", "v1.2.0", "Resonator",
            "FX", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "259", "8.6 MB", "3%",
            "PotentialBreaks - VST - PotentialBleed", "potentialbleed.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialbleed/4d582269764864993c333429c3c3cd57ecd7c8b6/docs/images/potentialbleed.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialbleed/releases/download/v1.2.0/PotentialBleed-1.2.0-mpc-armv7.zip", "plugins/potentialbleed/thumb.png",
            "Ghost bleed: hears each hit and adds quiet synthetic residue behind it (snare wires, cabinet, piano board, tines and more), so chops sound like a real recording.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialBleed\" && ZIP_PATH=\"./plugins/potentialbleed/potentialbleed.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/PotentialBreakfast/mpc-vst-potentialbleed/releases/download/v1.2.0/PotentialBleed-1.2.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/potentialbleed\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/PotentialBreakfast/mpc-vst-potentialbleed/releases/download/v1.2.0/PotentialBleed-1.2.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialBleed/\"; elif [ -f \"./plugins/potentialbleed/potentialbleed.so\" ]; then cp -rf ./plugins/potentialbleed/* \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialBleed/\"; else echo \"[ERROR] No local binary archive found for potentialbleed\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "lo-fi", "texture", "ghost-notes" }
        },
        {
            "potentialgrit", "PotentialGrit", "PotentialBreaks", "v1.2.0", "Multi Fx",
            "FX", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "295", "12.3 MB", "3%",
            "PotentialBreaks - VST - PotentialGrit", "potentialgrit.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialgrit/96b1b4e4f9ef1fddeafa1063be8346740da94518/docs/images/potentialgrit.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialgrit/releases/download/v1.2.0/PotentialGrit-1.2.0-mpc-armv7.zip", "plugins/potentialgrit/thumb.png",
            "Seven lo-fi effects on pads, one at a time: dust (glue, hiss, wobble), crush, resonant filter, three-band kill EQ, tempo echo, chop gate and ring mod, each with its own settings.", "./plugins/potentialgrit/install.sh", 0,
            0,
            false,
            false,
            "",
            { "lo-fi", "bitcrusher", "filter", "delay", "ring-mod" }
        },
        {
            "potentialheat", "PotentialHeat", "PotentialBreaks", "v1.5.1", "Saturation",
            "FX", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "1040", "6.6 MB", "6%",
            "PotentialBreaks - VST - PotentialHeat", "potentialheat.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialheat/6ade776dda00fcd70053168b6b7ede87cd7fa091/docs/images/potentialheat.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialheat/releases/download/v1.5.1/PotentialHeat-1.5.1-mpc-armv7.zip", "plugins/potentialheat/thumb.png",
            "Soft clipper and bus saturator: push drums, breaks or a whole beat into a warm curve (soft, tape or hard) that rounds the peaks off instead of clipping, with tone, ceiling, level match, mix, a live meter and 12 one-tap presets. Pairs with PotentialPunch.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialHeat\" && ZIP_PATH=\"./plugins/potentialheat/potentialheat.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/PotentialBreakfast/mpc-vst-potentialheat/releases/download/v1.5.1/PotentialHeat-1.5.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/potentialheat\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/PotentialBreakfast/mpc-vst-potentialheat/releases/download/v1.5.1/PotentialHeat-1.5.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialHeat/\"; elif [ -f \"./plugins/potentialheat/potentialheat.so\" ]; then cp -rf ./plugins/potentialheat/* \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialHeat/\"; else echo \"[ERROR] No local binary archive found for potentialheat\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "saturation", "soft-clipper", "bus", "drums", "loudness", "warmth" }
        },
        {
            "potentialpatina", "PotentialPatina", "PotentialBreaks", "v1.6.0", "Saturation",
            "FX", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "1197", "12.7 MB", "6%",
            "PotentialBreaks - VST - PotentialPatina", "potentialpatina.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialpatina/b6e1a98e6b836a51d21ea62e0791ba695c237084/docs/images/potentialpatina.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialpatina/releases/download/v1.6.0/PotentialPatina-1.6.0-mpc-armv7.zip", "plugins/potentialpatina/thumb.png",
            "Retro-colour insert effect: vinyl and tape noise, wow and flutter, tape/tube/cassette drive, bit and sample-rate crush, a tone tilt and a small room, with presets from subtle dust to old radio.", "./plugins/potentialpatina/install.sh", 0,
            0,
            false,
            false,
            "",
            { "lo-fi", "vinyl", "tape", "bitcrusher", "wobble" }
        },
        {
            "potentialpunch", "PotentialPunch", "PotentialBreaks", "v1.6.1", "Compressor",
            "FX", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "1124", "5.8 MB", "3%",
            "PotentialBreaks - VST - PotentialPunch", "potentialpunch.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialpunch/2640801fdb40fde4984275819330847fe4761870/docs/images/potentialpunch.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialpunch/releases/download/v1.6.1/PotentialPunch-1.6.1-mpc-armv7.zip", "plugins/potentialpunch/thumb.png",
            "Transient shaper: more or less punch at the start of every hit and more or less tail after it, level-independent with no threshold, speed and focus (full/lows/highs), soft clip, live meter and 12 one-tap presets for drums, breaks and more.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialPunch\" && ZIP_PATH=\"./plugins/potentialpunch/potentialpunch.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/PotentialBreakfast/mpc-vst-potentialpunch/releases/download/v1.6.1/PotentialPunch-1.6.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/potentialpunch\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/PotentialBreakfast/mpc-vst-potentialpunch/releases/download/v1.6.1/PotentialPunch-1.6.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialPunch/\"; elif [ -f \"./plugins/potentialpunch/potentialpunch.so\" ]; then cp -rf ./plugins/potentialpunch/* \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialPunch/\"; else echo \"[ERROR] No local binary archive found for potentialpunch\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "transient-shaper", "drums", "punch", "breaks", "dynamics" }
        },
        {
            "potentialwarp", "PotentialWarp", "PotentialBreaks", "v1.4.0", "Glitch",
            "FX", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "340", "6.6 MB", "5%",
            "PotentialBreaks - VST - PotentialWarp", "potentialwarp.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialwarp/a37d7dee2c87533557f4e898adf8c21b242d127a/docs/images/potentialwarp.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialwarp/releases/download/v1.4.0/PotentialWarp-1.4.0-mpc-armv7.zip", "plugins/potentialwarp/thumb.png",
            "Tempo-locked mangler: 16 patterns of half time, tape stop, stutter, reverse and gates, with swing, triplets, 4-bar patterns and a hold-a-pad mode.", "./plugins/potentialwarp/install.sh", 0,
            0,
            false,
            false,
            "",
            { "stutter", "tape-stop", "gate", "half-time", "tempo-sync" }
        },
        {
            "pulytek", "PulyTek", "MagicStino", "v1.0.2", "Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-10-23",
            "3665", "14.9 MB", "14%",
            "MagicStino - VST - PulyTek", "pulytek.so",
            "https://raw.githubusercontent.com/MagicStino/pulytek/v1.0.1/docs/preview_0.png", "https://github.com/MagicStino/pulytek/releases/download/v1.0.2/PulyTek-1.0.2-mpc-armv7.zip", "plugins/pulytek/thumb.png",
            "Pultec-style WDF EQ with tube warmth, AIR, stereo VU meters and four-band side EQ.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/MagicStino - VST - PulyTek\" && ZIP_PATH=\"./plugins/pulytek/pulytek.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/MagicStino/pulytek/releases/download/v1.0.2/PulyTek-1.0.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/pulytek\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/MagicStino/pulytek/releases/download/v1.0.2/PulyTek-1.0.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/MagicStino - VST - PulyTek/\"; elif [ -f \"./plugins/pulytek/pulytek.so\" ]; then cp -rf ./plugins/pulytek/* \"$TARGET_SYNTH_DIR/MagicStino - VST - PulyTek/\"; else echo \"[ERROR] No local binary archive found for pulytek\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "eq", "pultec", "wdf", "tube", "stereo" }
        },
        {
            "op1fx-punch", "Punch", "Bombest", "v0.4.1", "Distortion",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "635", "6.0 MB", "14%",
            "bombest - VST - Punch", "punch.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/punch.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Punch-0.4.1-mpc-armv7.zip", "plugins/op1fx-punch/thumb.png",
            "Bit-crush style punch for drums and buses, 12 presets, with a Detroit boxer skin.", "./plugins/op1fx-punch/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "compressor", "drive", "drums" }
        },
        {
            "remixxxl-cue", "reMixXXL CUE", "L'Cronx", "v0.1.3", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-08",
            "248", "982 KB", "14%",
            "L'Cronx - VST - reMixXXL CUE", "remixxxl_cue.so",
            "", "https://github.com/sunskiefer/reMixXXL/releases/download/v0.1.25/reMixXXL-CUE-0.1.3-mpc-armv7.zip", "plugins/remixxxl-cue/thumb.png",
            "Companion to reMixXXL: plays reMixXXL's PFL cue (decks 1/3 left, 2/4 right) on its own audio track routed to outputs 3/4, so you can beat match in the headphones while the main mix stays on 1/2. Needs reMixXXL 0.1.25 or later in the same project.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - reMixXXL CUE\" && ZIP_PATH=\"./plugins/remixxxl-cue/remixxxl-cue.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/reMixXXL/releases/download/v0.1.25/reMixXXL-CUE-0.1.3-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/remixxxl-cue\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/reMixXXL/releases/download/v0.1.25/reMixXXL-CUE-0.1.3-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - reMixXXL CUE/\"; elif [ -f \"./plugins/remixxxl-cue/remixxxl_cue.so\" ]; then cp -rf ./plugins/remixxxl-cue/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - reMixXXL CUE/\"; else echo \"[ERROR] No local binary archive found for remixxxl-cue\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "dj", "routing", "cue", "headphones" }
        },
        {
            "rings-fx", "Resonator FX", "saustin2010", "v1.2.1", "Resonator",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2919", "1.2 MB", "14%",
            "Émilie Gillet - VST - [FX] Resonator FX", "ringsfx.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-resonator-fx/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-resonator-fx/releases/download/v1.2.1/FX-Resonator-FX-1.2.1-mpc-armv7.zip", "plugins/rings-fx/thumb.png",
            "Resonator FX: Resonator as an insert effect, the track's audio strums it, turning drums and loops into tuned strings; 12 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Resonator FX\" && ZIP_PATH=\"./plugins/rings-fx/rings-fx.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-resonator-fx/releases/download/v1.2.1/FX-Resonator-FX-1.2.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/rings-fx\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-resonator-fx/releases/download/v1.2.1/FX-Resonator-FX-1.2.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Resonator FX/\"; elif [ -f \"./plugins/rings-fx/ringsfx.so\" ]; then cp -rf ./plugins/rings-fx/* \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Resonator FX/\"; else echo \"[ERROR] No local binary archive found for rings-fx\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "rings", "resonator", "eurorack" }
        },
        {
            "retrolooper-force", "Retrolooper One (Force)", "Retroloop USA / IAMTHEBAND LLC", "v0.2.10", "Looper",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-07",
            "210", "2.8 MB", "14%",
            "RetroLooper - VST - Retrolooper One", "retrolooper_force.so",
            "https://raw.githubusercontent.com/asearan/Retrolooper-One-MPC-Force/force-v0.2.10/docs/images/retrolooper-force-thumbnail.jpg", "https://github.com/asearan/Retrolooper-One-MPC-Force/releases/download/force-v0.2.10/RetroLooper-One-Force-0.2.10-mpc-armv7.zip", "plugins/retrolooper-force/thumb.png",
            "Stereo Audio looper for Akai Force. On MPC OS 3.10.1, copy the Gen1 build into VST2 Plugins. 0.3.0 keeps plugin id RlF1. Native Controls remain pinned to Force 3.10.", "./plugins/retrolooper-force/install.sh", 0,
            0,
            false,
            false,
            "",
            { "looper", "overdub", "utility" }
        },
        {
            "retrolooper-one", "Retrolooper One (MPC)", "Retroloop USA / IAMTHEBAND LLC", "v0.3.9", "Looper",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-07",
            "507", "2.7 MB", "14%",
            "RetroLooper - VST - Retrolooper One", "retrolooper_one.so",
            "https://raw.githubusercontent.com/asearan/Retrolooper-One-MPC-Force/mpc-v0.3.9/docs/images/retrolooper-one-thumbnail.jpg", "https://github.com/asearan/Retrolooper-One-MPC-Force/releases/download/mpc-v0.3.9/RetroLooper-One-0.3.9-mpc-armv7.zip", "plugins/retrolooper-one/thumb.png",
            "Stereo Audio looper for Gen1 and Gen2 MPCs. On MPC OS 3.10.1, copy it into VST2 Plugins. 0.4.0 keeps plugin id RlO1 so saved projects recall it. The separate official SDK build is IH97.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/RetroLooper - VST - Retrolooper One\" && ZIP_PATH=\"./plugins/retrolooper-one/retrolooper-one.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/asearan/Retrolooper-One-MPC-Force/releases/download/mpc-v0.3.9/RetroLooper-One-0.3.9-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/retrolooper-one\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/asearan/Retrolooper-One-MPC-Force/releases/download/mpc-v0.3.9/RetroLooper-One-0.3.9-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/RetroLooper - VST - Retrolooper One/\"; elif [ -f \"./plugins/retrolooper-one/retrolooper_one.so\" ]; then cp -rf ./plugins/retrolooper-one/* \"$TARGET_SYNTH_DIR/RetroLooper - VST - Retrolooper One/\"; else echo \"[ERROR] No local binary archive found for retrolooper-one\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "looper", "overdub", "utility" }
        },
        {
            "rmxxxl", "RMXXXL", "L'Cronx", "v1.3.0", "Multi Fx",
            "FX", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "3807", "3.3 MB", "14%",
            "L'Cronx - VST - RMXXXL", "rmxxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/RMXXXL/0ef9fe195525cada860205f1370a4b9f6896bc60/docs/img/remix.png", "https://github.com/sunskiefer/RMXXXL/releases/download/v1.3.0/RMXXXL-1.3.0-mpc-armv7.zip", "plugins/rmxxxl/thumb.png",
            "RMX-1000-style remix effect: isolator, DJ filter, Build Up / Break Down scenes, synced echo, Release FX and a Release kill switch, EQ Kill buttons, a clean Panic, Clouds, Dragonfly reverbs, sample pads with ADSR, 16 user presets and a brickwall limiter, with a big-button touchscreen skin and Q-Links.", "./plugins/rmxxxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "remix", "dj", "rmx", "isolator", "filter", "clouds", "reverb", "pads" }
        },
        {
            "samplerfilter", "SamplerFilter", "Aaron Quint", "v1.1.4", "Filter",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-12",
            "473", "3.1 MB", "4%",
            "quirkey - VST - SamplerFilter", "samplerfilter.so",
            "https://raw.githubusercontent.com/quirkey/mpc-plugins/samplerfilter-v1.1.4/docs/samplerfilter.png", "https://github.com/quirkey/mpc-plugins/releases/download/samplerfilter-v1.1.4/SamplerFilter-1.1.4-mpc-armv7.zip", "plugins/samplerfilter/thumb.png",
            "A late-80s rack-sampler style 6-pole low-pass (no resonance) with the sampler's bit-depth and sample-rate grit, drive and mix.", "./plugins/samplerfilter/install.sh", 0,
            0,
            false,
            false,
            "",
            { "lo-fi", "low-pass", "bitcrusher", "vintage-sampler" }
        },
        {
            "shapey-5-band-eq", "Shapey 5-Band EQ", "3dthc", "v1.0.2", "Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-13",
            "1229", "3.4 MB", "14%",
            "3DTHC - VST - Shapey 5-Band EQ", "shapey_5band_eq.so",
            "https://raw.githubusercontent.com/3dthc/Shapey-MPC/ddbf9bbc9f66f2af0802070b38bb3d113140131c/docs/shapey-5-band-eq.png", "https://github.com/3dthc/Shapey-MPC/releases/download/shapey-5-band-eq-vst-v1.0.2/Shapey-5-Band-EQ-1.0.2-mpc-armv7.zip", "plugins/shapey-5-band-eq/thumb.png",
            "The 5-band version of Shapey EQ, made for touch: every band has its own lane under a taller curve, so any dot drags directly. Any band can be a bell, shelf, cut, notch or band pass. Live analyzer, zero latency.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3DTHC - VST - Shapey 5-Band EQ\" && ZIP_PATH=\"./plugins/shapey-5-band-eq/shapey-5-band-eq.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/3dthc/Shapey-MPC/releases/download/shapey-5-band-eq-vst-v1.0.2/Shapey-5-Band-EQ-1.0.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/shapey-5-band-eq\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/Shapey-MPC/releases/download/shapey-5-band-eq-vst-v1.0.2/Shapey-5-Band-EQ-1.0.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/3DTHC - VST - Shapey 5-Band EQ/\"; elif [ -f \"./plugins/shapey-5-band-eq/shapey_5band_eq.so\" ]; then cp -rf ./plugins/shapey-5-band-eq/* \"$TARGET_SYNTH_DIR/3DTHC - VST - Shapey 5-Band EQ/\"; else echo \"[ERROR] No local binary archive found for shapey-5-band-eq\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "eq", "parametric-eq", "analyzer", "mixing" }
        },
        {
            "shapey-eq", "Shapey EQ", "3dthc", "v1.6.3", "Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-13",
            "1313", "4.3 MB", "14%",
            "3DTHC - VST - Shapey EQ", "shapey_eq.so",
            "https://raw.githubusercontent.com/3dthc/Shapey-MPC/ddbf9bbc9f66f2af0802070b38bb3d113140131c/docs/shapey-eq.png", "https://github.com/3dthc/Shapey-MPC/releases/download/shapey-eq-vst-v1.6.3/Shapey-EQ-1.6.3-mpc-armv7.zip", "plugins/shapey-eq/thumb.png",
            "An 8-band parametric EQ you shape by touch: tap or drag on the curve, drag numbered dots, and make any band a bell, shelf, cut, notch or band pass. Live analyzer, zero latency, presets and four themes.", "./plugins/shapey-eq/install.sh", 0,
            0,
            false,
            false,
            "",
            { "eq", "parametric-eq", "analyzer", "mixing" }
        },
        {
            "op1fx-spring", "Spring", "Bombest", "v0.4.1", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-01",
            "570", "3.4 MB", "14%",
            "bombest - VST - Spring", "spring.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/spring.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.4.1/Spring-0.4.1-mpc-armv7.zip", "plugins/op1fx-spring/thumb.png",
            "Spring reverb with a 12-preset bank. OP-1 inspired.", "./plugins/op1fx-spring/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "reverb", "spring" }
        },
        {
            "stompbox", "Stompbox", "jacob-sabella", "v1.1.1", "Amp Sim",
            "FX", "Stable", "MPC Sample (1.3.0.12), MPC Key 37 (3.10.1.12)", "MPC OS 3.x", "2026-11-13",
            "967", "6.8 MB", "21%",
            "jacob-sabella - VST - Stompbox", "stompbox.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-stompbox/bc70be3dd01103bc792db0c2b0e54a589fc37d36/docs/screenshots/stompbox.png", "https://github.com/jacob-sabella/mpc-vst-stompbox/releases/download/stompbox-vst-v1.1.1/Stompbox-1.1.1-mpc-armv7.zip", "plugins/stompbox/thumb.png",
            "A whole guitar rig from guitarix's DSP: gate, comp, wah, any number of pedals, amp, tone stack, power amp, cabinet with an IR loader, post FX, mod, delay, reverb and EQ. 219 models and guitarix's 40 factory presets in the PRESET menu. Not affiliated with guitarix.", "./plugins/stompbox/install.sh", 0,
            0,
            false,
            false,
            "",
            { "guitar", "amp", "pedals", "cabinet", "cab-ir", "guitarix", "presets" }
        },
        {
            "stream", "Stream", "johnny", "v1.0.0", "Utility",
            "FX", "Stable", "MPC One (3.9.1.2)", "MPC OS 3.x", "2026-10-03",
            "826", "2.7 MB", "1%",
            "johnny - VST - Stream", "stream.so",
            "https://raw.githubusercontent.com/stampman3000-jpg/mpc-stream/764605ba1a08594e2dc5268e4f12bd6e25edb184/screenshots/stream.png", "https://github.com/stampman3000-jpg/mpc-stream/releases/download/v1.0.0/Stream-1.0.0-mpc-armv7.zip", "plugins/stream/thumb.png",
            "Multichannel USB audio effect send from an MPC to a Mac. Needs BlackHole 64ch and a Mac helper app.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/johnny - VST - Stream\" && ZIP_PATH=\"./plugins/stream/stream.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/stampman3000-jpg/mpc-stream/releases/download/v1.0.0/Stream-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/stream\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/stampman3000-jpg/mpc-stream/releases/download/v1.0.0/Stream-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/johnny - VST - Stream/\"; elif [ -f \"./plugins/stream/stream.so\" ]; then cp -rf ./plugins/stream/* \"$TARGET_SYNTH_DIR/johnny - VST - Stream/\"; else echo \"[ERROR] No local binary archive found for stream\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "utility" }
        },
        {
            "tidal-verb", "Tidal Verb", "Fullban", "v1.6.0", "Reverb",
            "FX", "Stable", "Akai Force (Gen1) (3.10.1), MPC XL (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-15",
            "152", "216 KB", "14%",
            "Fullban - VST - Tidal Verb", "tidal-verb.so",
            "https://raw.githubusercontent.com/fullbann/tidal-verb/v1.0.0/docs/catalog-thumbnail.png", "https://github.com/fullbann/tidal-verb/releases/download/v1.6.0/Tidal-Verb-1.6.0-mpc-armv7.zip", "plugins/tidal-verb/thumb.png",
            "Stereo reverb with ten types, from Room and Hall to a dark swirling Abyss and a cascading-octave shimmer. Decay, Liveliness, Swirl, Warmth and Air controls, eleven presets, deep-sea touchscreen skin.", "./plugins/tidal-verb/install.sh", 0,
            0,
            false,
            false,
            "",
            { "reverb", "shimmer", "ambient", "stereo" }
        },
        {
            "tr-mpc-tap-fx", "TR-MPC Tap FX", "sd88me", "v1.2.1", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-12",
            "1895", "515 KB", "14%",
            "sd88me - VST - TR-MPC Tap FX", "trmpc_tapfx.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/7e704c6f3be14b661c887631e203494d8cc3f74f/docs/img/tr-mpc-tap-fx.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-tap-fx-vst-v1.2.1/TR-MPC-Tap-FX-1.2.1-mpc-armv7.zip", "plugins/tr-mpc-tap-fx/thumb.png",
            "Companion to TR-MPC: sends any slot, or the kit's reverb or delay send, out on its own MPC track, sample-aligned with the kit, so MPC's mixer, submixes and insert effects can process it. Needs TR-MPC in the same project.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - TR-MPC Tap FX\" && ZIP_PATH=\"./plugins/tr-mpc-tap-fx/tr-mpc-tap-fx.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-tap-fx-vst-v1.2.1/TR-MPC-Tap-FX-1.2.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/tr-mpc-tap-fx\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-tap-fx-vst-v1.2.1/TR-MPC-Tap-FX-1.2.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - TR-MPC Tap FX/\"; elif [ -f \"./plugins/tr-mpc-tap-fx/trmpc_tapfx.so\" ]; then cp -rf ./plugins/tr-mpc-tap-fx/* \"$TARGET_SYNTH_DIR/sd88me - VST - TR-MPC Tap FX/\"; else echo \"[ERROR] No local binary archive found for tr-mpc-tap-fx\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "routing", "multi-out", "drum" }
        },
        {
            "tuner", "Tuner", "jacob-sabella", "v1.1.1", "Utility",
            "FX", "Stable", "MPC Key 37 (3.10.1.12), MPC Sample (1.3.0.12)", "MPC OS 3.x", "2026-11-12",
            "817", "670 KB", "1%",
            "jacob-sabella - VST - Tuner", "tuner.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-tuner/a3e4c099eacce3cdd440d54eb3ff059c96baafa5/docs/screenshots/needle.png", "https://github.com/jacob-sabella/mpc-vst-tuner/releases/download/tuner-vst-v1.1.1/Tuner-1.1.1-mpc-armv7.zip", "plugins/tuner/thumb.png",
            "A chromatic tuner insert with a live needle drawn straight onto the MPC's screen at 20-24 fps: note, cents and Hz, A4 from 430 to 450 Hz, and a mute for silent tuning. It writes into MPC's display memory, which MPC OS doesn't offer plugins, so a firmware update could break it. Not affiliated with Akai.", "./plugins/tuner/install.sh", 0,
            0,
            false,
            false,
            "",
            { "tuner", "guitar", "bass", "pitch", "screen-draw" }
        },
        {
            "tuney", "Tuney", "3dthc", "v1.4.4", "Pitch",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-13",
            "952", "9.3 MB", "14%",
            "3DTHC - VST - Tuney", "tuney.so",
            "https://raw.githubusercontent.com/3dthc/Tuney-MPC/55862ef83ab6938b5f1e3e20f5fa1ead7449e9b2/docs/tuney.png", "https://github.com/3dthc/Tuney-MPC/releases/download/tuney-vst-v1.4.4/Tuney-1.4.4-mpc-armv7.zip", "plugins/tuney/thumb.png",
            "Real-time vocal pitch correction: key and scale or your own notes on a touch keyboard, a pitch dial, Retune, Transition, Amount, Vibrato and Humanize, voice ranges and a reference pitch. Presets from hard tune to subtle.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3DTHC - VST - Tuney\" && ZIP_PATH=\"./plugins/tuney/tuney.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/3dthc/Tuney-MPC/releases/download/tuney-vst-v1.4.4/Tuney-1.4.4-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/tuney\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/Tuney-MPC/releases/download/tuney-vst-v1.4.4/Tuney-1.4.4-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/3DTHC - VST - Tuney/\"; elif [ -f \"./plugins/tuney/tuney.so\" ]; then cp -rf ./plugins/tuney/* \"$TARGET_SYNTH_DIR/3DTHC - VST - Tuney/\"; else echo \"[ERROR] No local binary archive found for tuney\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "pitch-correction", "vocal", "tuner", "effect" }
        },
        {
            "undertow", "Undertow", "Fullban", "v1.5.0", "Delay",
            "FX", "Stable", "Akai Force (Gen1) (3.10.1), MPC XL (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-15",
            "119", "1.1 MB", "14%",
            "Fullban - VST - Undertow", "undertow.so",
            "https://raw.githubusercontent.com/fullbann/undertow/v1.0.0/docs/catalog-thumbnail.png", "https://github.com/fullbann/undertow/releases/download/v1.5.0/Undertow-1.5.0-mpc-armv7.zip", "plugins/undertow/thumb.png",
            "Altered stereo delay with Analogue, Tape, Lo-Fi and Reverse modes, degrading repeats, runaway feedback, tempo sync and Tails. Nine presets, sepia touchscreen skin.", "./plugins/undertow/install.sh", 0,
            0,
            false,
            false,
            "",
            { "delay", "tape", "lo-fi", "reverse", "stereo" }
        },
        {
            "verglas", "Verglas", "saustin2010", "v1.2.1", "Granular",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2404", "642 KB", "14%",
            "Émilie Gillet - VST - [FX] Verglas", "verglas.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-verglas/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-verglas/releases/download/v1.2.1/FX-Verglas-1.2.1-mpc-armv7.zip", "plugins/verglas/thumb.png",
            "Verglas: granular, stretch, looper and spectral modes and freeze as an insert effect; 12 presets, from Émilie Gillet's open-source Clouds.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Verglas\" && ZIP_PATH=\"./plugins/verglas/verglas.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-verglas/releases/download/v1.2.1/FX-Verglas-1.2.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/verglas\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-verglas/releases/download/v1.2.1/FX-Verglas-1.2.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Verglas/\"; elif [ -f \"./plugins/verglas/verglas.so\" ]; then cp -rf ./plugins/verglas/* \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Verglas/\"; else echo \"[ERROR] No local binary archive found for verglas\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "clouds", "granular", "texture", "eurorack" }
        },
        {
            "vhs-vibes", "VHS Vibes", "anthonytheinventor", "v1.4.0", "Tape",
            "FX", "Stable", "MPC One+ (3.10.1.12)", "MPC OS 3.x", "2026-11-14",
            "309", "6.2 MB", "5%",
            "anthonytheinventor - VST - VHS Vibes", "VST/vhs-vibes-arm32.so",
            "https://raw.githubusercontent.com/anthonytheinventor/vhs-vibes/v1.3.0/docs/page_1_tape.png", "https://github.com/anthonytheinventor/vhs-vibes/releases/download/v1.4.0/VHS-Vibes-1.4.0-mpc-armv7.zip", "plugins/vhs-vibes/thumb.png",
            "A worn video-tape effect: pitch wobble, saturation, generation loss, hum, hiss and crackle, plus deck faults (level control, drift, tracking drop-outs) and a dry/wet mix. 21 programs, on a hot pink cassette with reels that turn.", "./plugins/vhs-vibes/install.sh", 0,
            0,
            false,
            false,
            "",
            { "tape", "lo-fi", "saturation", "modulation", "noise" }
        },
        {
            "vibe-fx", "Vibe FX", "nachtaktiv303", "v1.0.0", "Multi Fx",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1623", "906 KB", "14%",
            "nachtaktiv303 - VST - Vibe FX", "mpc_fx.so",
            "https://raw.githubusercontent.com/nachtaktiv303/mpc-vst-vibe-fx/main/preview_1.png", "https://github.com/nachtaktiv303/mpc-vst-vibe-fx/releases/download/vibe-fx-vst-v1.0.0/Vibe-FX-1.0.0-mpc-armv7.zip", "plugins/vibe-fx/thumb.png",
            "Drive, chorus, FDN reverb and tape delay in one native MPC OS insert effect, with a touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/nachtaktiv303 - VST - Vibe FX\" && ZIP_PATH=\"./plugins/vibe-fx/vibe-fx.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/nachtaktiv303/mpc-vst-vibe-fx/releases/download/vibe-fx-vst-v1.0.0/Vibe-FX-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/vibe-fx\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/nachtaktiv303/mpc-vst-vibe-fx/releases/download/vibe-fx-vst-v1.0.0/Vibe-FX-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/nachtaktiv303 - VST - Vibe FX/\"; elif [ -f \"./plugins/vibe-fx/mpc_fx.so\" ]; then cp -rf ./plugins/vibe-fx/* \"$TARGET_SYNTH_DIR/nachtaktiv303 - VST - Vibe FX/\"; else echo \"[ERROR] No local binary archive found for vibe-fx\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "drive", "chorus", "reverb", "delay", "multi-fx", "insert" }
        },
        {
            "vintage-military-radio", "Vintage Military Radio", "Tropix R!X Crazy Jungle Sounds", "v1.0.16", "Tape",
            "FX", "Stable", "MPC XL (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-14",
            "106", "5.3 MB", "14%",
            "Tropix R!X Crazy Jungle Sounds - VST - Vintage Military Radio", "VST/vintage-military-radio-arm32.so",
            "https://raw.githubusercontent.com/vulnerablecodes/music-production/v1.0.16/media/vintage-military-radio-current.jpg", "https://github.com/vulnerablecodes/music-production/releases/download/v1.0.16/Vintage-Military-Radio-1.0.16-mpc-armv7.zip", "plugins/vintage-military-radio/thumb.png",
            "Eight stereo tape-performance effects with six procedural noise layers and native chrome controls.", "./plugins/vintage-military-radio/install.sh", 0,
            0,
            false,
            false,
            "",
            { "tape", "lo-fi", "saturation", "performance", "noise" }
        },
        {
            "xxl-pro", "XXL Pro", "L'Cronx", "v0.1.0", "Multi Fx",
            "FX", "Stable", "Akai Force (3.9)", "MPC OS 3.x", "2026-11-14",
            "75", "30.1 MB", "14%",
            "L'Cronx - VST - XXL Pro", "xxlpro.so",
            "https://raw.githubusercontent.com/sunskiefer/XXL-Strips/xxl-v0.1/XXL-Pro/docs/img/view_00_overview.png", "https://github.com/sunskiefer/XXL-Strips/releases/download/xxl-v0.1/XXL-Pro-0.1.0-mpc-armv7.zip", "plugins/xxl-pro/thumb.png",
            "Fifteen process modules in the style of classic studio gear in one effect, joined on a touch patch bay: six compressors, five EQs with a preamp, two tape machines and two digital reverbs. Patch any module into any other, in series or parallel. 59 presets. By L'Cronx, GPL-3. Not endorsed by the makers of the originals.", "./plugins/xxl-pro/install.sh", 0,
            0,
            false,
            false,
            "",
            { "multi-fx", "compressor", "eq", "tape", "reverb", "emulation", "routing", "mixing" }
        },
        {
            "xxl-strip", "XXL STRIP", "L'Cronx", "v0.1.1", "Mixing",
            "FX", "Stable", "Akai Force (3.9)", "MPC OS 3.x", "2026-11-14",
            "75", "11.0 MB", "14%",
            "L'Cronx - VST - XXL STRIP", "xxlstrip.so",
            "https://raw.githubusercontent.com/sunskiefer/XXL-Strips/xxl-v0.1/XXL-STRIP/docs/img/page_0.png", "https://github.com/sunskiefer/XXL-Strips/releases/download/xxl-v0.1/XXL-STRIP-0.1.1-mpc-armv7.zip", "plugins/xxl-strip/thumb.png",
            "Channel strip in the style of a classic large-format British console (4000 E / G style): input drive, high- and low-pass filters, four-band EQ in three models (E Brown, E Black, G), compressor, gate / expander and a VU meter. 56 presets. By L'Cronx, GPL-3; the E-model filters and EQ are Dusk Audio's. Not endorsed by the makers of the originals.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - XXL STRIP\" && ZIP_PATH=\"./plugins/xxl-strip/xxl-strip.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/XXL-Strips/releases/download/xxl-v0.1/XXL-STRIP-0.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/xxl-strip\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/XXL-Strips/releases/download/xxl-v0.1/XXL-STRIP-0.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - XXL STRIP/\"; elif [ -f \"./plugins/xxl-strip/xxlstrip.so\" ]; then cp -rf ./plugins/xxl-strip/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - XXL STRIP/\"; else echo \"[ERROR] No local binary archive found for xxl-strip\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "console", "channel-strip", "eq", "compressor", "gate", "emulation", "mixing" }
        },
        {
            "xxl-strip-bus", "XXL STRIP BUS", "L'Cronx", "v0.1.1", "Compressor",
            "FX", "Stable", "Akai Force (3.9)", "MPC OS 3.x", "2026-11-14",
            "72", "8.9 MB", "14%",
            "L'Cronx - VST - XXL STRIP BUS", "xxlstripbus.so",
            "https://raw.githubusercontent.com/sunskiefer/XXL-Strips/xxl-v0.1/XXL-STRIP-BUS/docs/img/page_0.png", "https://github.com/sunskiefer/XXL-Strips/releases/download/xxl-v0.1/XXL-STRIP-BUS-0.1.1-mpc-armv7.zip", "plugins/xxl-strip-bus/thumb.png",
            "Stereo bus compressor in the style of the classic console bus compressor (G-series style): threshold, ratio, attack, release with auto, make-up, side-chain high-pass, mix, Classic and Modern modes, VU meter. By L'Cronx, GPL-3. Not endorsed by the makers of the original.", "./plugins/xxl-strip-bus/install.sh", 0,
            0,
            false,
            false,
            "",
            { "console", "compressor", "bus-compressor", "emulation", "mixing" }
        },
        {
            "xxl-strip-master", "XXL STRIP MASTER", "L'Cronx", "v0.1.1", "Mixing",
            "FX", "Stable", "Akai Force (3.9)", "MPC OS 3.x", "2026-11-14",
            "79", "12.3 MB", "14%",
            "L'Cronx - VST - XXL STRIP MASTER", "xxlstripmaster.so",
            "https://raw.githubusercontent.com/sunskiefer/XXL-Strips/xxl-v0.1/XXL-STRIP-MASTER/docs/img/page_0.png", "https://github.com/sunskiefer/XXL-Strips/releases/download/xxl-v0.1/XXL-STRIP-MASTER-0.1.1-mpc-armv7.zip", "plugins/xxl-strip-master/thumb.png",
            "Master-bus strip in the same console style as XXL STRIP: drive and density, low and high shelving EQ, high-frequency compressor, stereo width and space, transformer stage, high-pass filter, VU meter. By L'Cronx, GPL-3.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - XXL STRIP MASTER\" && ZIP_PATH=\"./plugins/xxl-strip-master/xxl-strip-master.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/XXL-Strips/releases/download/xxl-v0.1/XXL-STRIP-MASTER-0.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/xxl-strip-master\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/XXL-Strips/releases/download/xxl-v0.1/XXL-STRIP-MASTER-0.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - XXL STRIP MASTER/\"; elif [ -f \"./plugins/xxl-strip-master/xxlstripmaster.so\" ]; then cp -rf ./plugins/xxl-strip-master/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - XXL STRIP MASTER/\"; else echo \"[ERROR] No local binary archive found for xxl-strip-master\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "console", "mastering", "saturation", "eq", "compressor", "stereo", "mixing" }
        }
        };
        if (!sec.items.empty()) sections.push_back(sec);
    }
    {
        Section sec;
        sec.title = "ADDINS";
        sec.items = {
        {
            "force-link-audio", "Force Link Audio", "Mac Digi / Rare Data", "v1.1.0", "Network Audio",
            "ADDIN", "Stable", "Akai Force (3.9.1.2)", "MPC OS 2.x & 3.x", "2026-10-14",
            "632", "257 KB", "14%",
            "Mac Digi / Rare Data - VST - Force Link Audio", "forceLinkAudioAddin.so",
            "https://github.com/macdigi/force-link-audio/releases/download/v1.1.0/force-link-audio-card.png", "https://github.com/macdigi/force-link-audio/releases/download/v1.1.0/Force-Link-Audio-1.1.0-mpc-armv7.zip", "plugins/force-link-audio/thumb.png",
            "Publish the Force stereo main mix to Link Audio receivers on your trusted LAN while staying standalone. Disable the legacy Force Link Audio addon first. Off until enabled in settings; no remote-control server. Other MPC hardware is unverified.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Mac Digi / Rare Data - VST - Force Link Audio\" && ZIP_PATH=\"./plugins/force-link-audio/force-link-audio.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/macdigi/force-link-audio/releases/download/v1.1.0/Force-Link-Audio-1.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/force-link-audio\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/macdigi/force-link-audio/releases/download/v1.1.0/Force-Link-Audio-1.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Mac Digi / Rare Data - VST - Force Link Audio/\"; elif [ -f \"./plugins/force-link-audio/forceLinkAudioAddin.so\" ]; then cp -rf ./plugins/force-link-audio/* \"$TARGET_SYNTH_DIR/Mac Digi / Rare Data - VST - Force Link Audio/\"; else echo \"[ERROR] No local binary archive found for force-link-audio\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "link-audio", "network-audio", "recording", "compa" }
        },
        {
            "commander", "MPC Commander", "jacob-sabella", "v1.3.1", "Utility",
            "ADDIN", "Stable", "MPC Key 37 (Gen1) (3.10.1.12), MPC Sample (1.3.0.12)", "MPC OS 2.x & 3.x", "2026-11-13",
            "4771", "254 KB", "14%",
            "jacob-sabella - VST - MPC Commander", "mpc_commander_addin.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-commander/763c959e1855fd3bcec7dfd6832b4be77a502e4f/docs/tile.png", "https://github.com/jacob-sabella/mpc-addin-commander/releases/download/commander-addin-v1.3.1/MPC-Commander-addin-1.3.1-mpc-armv7.zip", "plugins/commander/thumb.png",
            "The plugins MPC loads, with their own skins, and MPC's transport, on a desktop app. Listens on the device only: reach it through an SSH tunnel (no login; bind=0.0.0.0 opens it to your network).", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Commander\" && ZIP_PATH=\"./plugins/commander/commander.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/jacob-sabella/mpc-addin-commander/releases/download/commander-addin-v1.3.1/MPC-Commander-addin-1.3.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/commander\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-addin-commander/releases/download/commander-addin-v1.3.1/MPC-Commander-addin-1.3.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Commander/\"; elif [ -f \"./plugins/commander/mpc_commander_addin.so\" ]; then cp -rf ./plugins/commander/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Commander/\"; else echo \"[ERROR] No local binary archive found for commander\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "desktop", "plugins", "skins", "transport", "midi" }
        },
        {
            "filebrowser", "MPC File Browser", "jacob-sabella", "v1.1.1", "Utility",
            "ADDIN", "Stable", "MPC Key 37 (Gen1) (3.10.1.12), MPC Sample (1.3.0.12)", "MPC OS 2.x & 3.x", "2026-11-13",
            "1687", "19.2 MB", "14%",
            "jacob-sabella - VST - MPC File Browser", "mpc_filebrowser_addin.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-filebrowser/88b86eb39f907485995497cf0eaeeb5ddd7aa3e4/docs/tile.png", "https://github.com/jacob-sabella/mpc-addin-filebrowser/releases/download/filebrowser-addin-v1.1.1/MPC-File-Browser-1.1.1-mpc-armv7.zip", "plugins/filebrowser/thumb.png",
            "FileBrowser Quantum beside MPC: browse the device's drives in a web browser, upload and download samples and projects, play audio, edit text files. Listens on the device only: reach it through an SSH tunnel (login admin / mpcadmin; change it). bind=0.0.0.0 opens it to your network.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC File Browser\" && ZIP_PATH=\"./plugins/filebrowser/filebrowser.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/jacob-sabella/mpc-addin-filebrowser/releases/download/filebrowser-addin-v1.1.1/MPC-File-Browser-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/filebrowser\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-addin-filebrowser/releases/download/filebrowser-addin-v1.1.1/MPC-File-Browser-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC File Browser/\"; elif [ -f \"./plugins/filebrowser/mpc_filebrowser_addin.so\" ]; then cp -rf ./plugins/filebrowser/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC File Browser/\"; else echo \"[ERROR] No local binary archive found for filebrowser\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "files", "browser", "upload", "samples", "editor" }
        },
        {
            "gateway", "MPC Gateway", "jacob-sabella", "v1.0.2", "Utility",
            "ADDIN", "Stable", "MPC Key 37 (Gen1) (3.10.1.12), MPC Sample (1.3.0.12)", "MPC OS 2.x & 3.x", "2026-11-13",
            "589", "16.2 MB", "14%",
            "jacob-sabella - VST - MPC Gateway", "mpc_gateway_addin.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-gateway/f17f7722faee4805da75955d1b09e572a366d8b3/docs/card.png", "https://github.com/jacob-sabella/mpc-addin-gateway/releases/download/gateway-addin-v1.0.2/MPC-Gateway-1.0.2-mpc-armv7.zip", "plugins/gateway/thumb.png",
            "One HTTPS address and one login (mpc / mpc until you change it on its Settings page) for every addin's web page. A start page lists the services addins declare (ADDIN_WEB_PORT), each routed behind the password. Runs Caddy beside MPC. Device only by default (reach it through an SSH tunnel); bind=0.0.0.0 opens it to your network.", "./plugins/gateway/install.sh", 0,
            0,
            false,
            false,
            "",
            { "network", "https", "login", "web", "gateway" }
        },
        {
            "remote", "MPC Remote", "jacob-sabella", "v1.4.1", "Utility",
            "ADDIN", "Stable", "MPC Key 37 (Gen1) (3.10.1.12), MPC Sample (1.3.0.12)", "MPC OS 2.x & 3.x", "2026-11-13",
            "5614", "77 KB", "14%",
            "jacob-sabella - VST - MPC Remote", "mpc_remote_addin.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-remote/c9cc05422873025693cf5527b485ef44da51c5a0/docs/tile.png", "https://github.com/jacob-sabella/mpc-addin-remote/releases/download/remote-addin-v1.4.1/MPC-Remote-addin-1.4.1-mpc-armv7.zip", "plugins/remote/thumb.png",
            "The MPC screen in a browser, and an MCP server so an AI model can see and touch it, play MIDI and read files. Listens on the device only: reach it through an SSH tunnel (no login; bind=0.0.0.0 opens it to your network).", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Remote\" && ZIP_PATH=\"./plugins/remote/remote.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/jacob-sabella/mpc-addin-remote/releases/download/remote-addin-v1.4.1/MPC-Remote-addin-1.4.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/remote\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-addin-remote/releases/download/remote-addin-v1.4.1/MPC-Remote-addin-1.4.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Remote/\"; elif [ -f \"./plugins/remote/mpc_remote_addin.so\" ]; then cp -rf ./plugins/remote/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Remote/\"; else echo \"[ERROR] No local binary archive found for remote\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "remote", "screen", "touch", "mcp", "midi" }
        },
        {
            "sample-draw-handler", "MPC Sample Draw Handler", "jacob-sabella", "v1.0.1", "Utility",
            "ADDIN", "Stable", "MPC Sample (1.3.0.12)", "MPC OS 2.x & 3.x", "2026-11-06",
            "391", "64 KB", "14%",
            "jacob-sabella - VST - MPC Sample Draw Handler", "mpc_sample_draw_handler.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-sample-draw-handler/3db0a761ac2e8ac74d52fef433e576b6c966fee1/docs/screenshots/stompbox-page.gif", "https://github.com/jacob-sabella/mpc-addin-sample-draw-handler/releases/download/sample-draw-handler-addin-v1.0.1/MPC-Sample-draw-handler-addin-1.0.1-mpc-aarch64.zip", "plugins/sample-draw-handler/thumb.png",
            "Lets plugins and addins draw their own page on the MPC Sample's screen. Needed by MPC Sample Rack.", "./plugins/sample-draw-handler/install.sh", 0,
            0,
            false,
            false,
            "",
            { "screen", "plugin-pages" }
        },
        {
            "sample-rack", "MPC Sample Rack", "jacob-sabella", "v1.0.0", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-06",
            "380", "68 KB", "14%",
            "jacob-sabella - VST - MPC Sample Rack", "mpc_sample_rack.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-sample-rack/c2a988ddc52bf92dc3bf83f5f4283cadc78eeb47/docs/screenshots/rack-page.png", "https://github.com/jacob-sabella/mpc-addin-sample-rack/releases/download/sample-rack-addin-v1.0.0/MPC-Sample-rack-addin-1.0.0-mpc-aarch64.zip", "plugins/sample-rack/thumb.png",
            "Add, remove and turn on or off the plugins in a project's inserts from a RACK page on the Sample. APPLY saves and restarts MPC: save first.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Sample Rack\" && ZIP_PATH=\"./plugins/sample-rack/sample-rack.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/jacob-sabella/mpc-addin-sample-rack/releases/download/sample-rack-addin-v1.0.0/MPC-Sample-rack-addin-1.0.0-mpc-aarch64.zip\" ]; then mkdir -p \"./plugins/sample-rack\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-addin-sample-rack/releases/download/sample-rack-addin-v1.0.0/MPC-Sample-rack-addin-1.0.0-mpc-aarch64.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Sample Rack/\"; elif [ -f \"./plugins/sample-rack/mpc_sample_rack.so\" ]; then cp -rf ./plugins/sample-rack/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Sample Rack/\"; else echo \"[ERROR] No local binary archive found for sample-rack\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "plugins", "inserts" }
        },
        {
            "sample-scalemode", "MPC Sample Scale Mode", "jacob-sabella", "v1.2.0", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-10",
            "471", "321 KB", "14%",
            "jacob-sabella - VST - MPC Sample Scale Mode", "mpc_sample_scalemode.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-sample-scalemode/7f5c90324dbc6f3458eb17466a265d5992d2fade/docs/screenshots/scale-page.png", "https://github.com/jacob-sabella/mpc-addin-sample-scalemode/releases/download/v1.2.0/MPC-Sample-Scale-Mode-1.2.0-mpc-aarch64.zip", "plugins/sample-scalemode/thumb.png",
            "Hold 16 Levels on the MPC Sample to play its 16 slots in a scale (8 scales, any root), starting from the sample's own note (MPC's, or heard in the audio when untagged). A SCALE page shows the notes and lights the one you play. Recorded hits keep their notes. Needs the MPC Sample Draw Handler for the page.", "./plugins/sample-scalemode/install.sh", 0,
            0,
            false,
            false,
            "",
            { "scale", "16-levels", "notes", "pads" }
        },
        {
            "skipback", "MPC Skipback", "sd88me", "v0.6.3", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-12",
            "2159", "64 KB", "14%",
            "sd88me - VST - MPC Skipback", "libmpc_skipback.so",
            "https://raw.githubusercontent.com/sd88me/mpc-addin-skipback/dc82602b2a05354d42445c8a330fb92a864892fb/docs/img/skipback.png", "https://github.com/sd88me/mpc-addin-skipback/releases/download/v0.6.3/MPC-Skipback-0.6.3-mpc-armv7.zip", "plugins/skipback/thumb.png",
            "Records the main output all the time and saves the last 30 to 60 seconds as a WAV when you double-press a hardware button (Rec Arm on a Force; Overdub, or a button you teach it, on other models). Tested on a Gen1 Force; other models are unverified.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - MPC Skipback\" && ZIP_PATH=\"./plugins/skipback/skipback.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-addin-skipback/releases/download/v0.6.3/MPC-Skipback-0.6.3-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/skipback\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-addin-skipback/releases/download/v0.6.3/MPC-Skipback-0.6.3-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - MPC Skipback/\"; elif [ -f \"./plugins/skipback/libmpc_skipback.so\" ]; then cp -rf ./plugins/skipback/* \"$TARGET_SYNTH_DIR/sd88me - VST - MPC Skipback/\"; else echo \"[ERROR] No local binary archive found for skipback\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "skipback", "recording", "sampling", "wav", "buffer" }
        },
        {
            "usb-audio", "MPC USB audio", "jacob-sabella", "v1.2.1", "Audio Interface",
            "ADDIN", "Stable", "MPC Key 37 (Gen1) (3.10.1.12)", "MPC OS 2.x & 3.x", "2026-11-13",
            "5512", "64 KB", "14%",
            "jacob-sabella - VST - MPC USB audio", "libmpc_usb_audio.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-usb-audio/f73cf3a1de3c3d005f30b6d38caddf587376e6d4/docs/tile.png", "https://github.com/jacob-sabella/mpc-addin-usb-audio/releases/download/usb-audio-addin-v1.2.1/MPC-USB-audio-addin-1.2.1-mpc-armv7.zip", "plugins/usb-audio/thumb.png",
            "The MPC as a USB audio interface: its outputs and inputs as channels on the computer, next to its MIDI ports. Works where the device port is wired to the USB-B socket (Key 37 verified; One, One+). Not on the Force, Live II or X: their USB-B port sits behind the internal hub in standalone mode; use force-link-audio there.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC USB audio\" && ZIP_PATH=\"./plugins/usb-audio/usb-audio.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/jacob-sabella/mpc-addin-usb-audio/releases/download/usb-audio-addin-v1.2.1/MPC-USB-audio-addin-1.2.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/usb-audio\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-addin-usb-audio/releases/download/usb-audio-addin-v1.2.1/MPC-USB-audio-addin-1.2.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC USB audio/\"; elif [ -f \"./plugins/usb-audio/libmpc_usb_audio.so\" ]; then cp -rf ./plugins/usb-audio/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC USB audio/\"; else echo \"[ERROR] No local binary archive found for usb-audio\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "usb", "audio-interface", "recording", "inputs" }
        },
        {
            "nova-audio", "Nova Audio", "3dthc", "v0.2.0", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "285", "64 KB", "14%",
            "3dthc - VST - Nova Audio", "libnova_audio.so",
            "", "https://github.com/3dthc/mpc-addin-nova-audio/releases/download/nova-audio-v0.2.0/Nova-Audio-for-Nova-Editor-0.2.0-mpc-armv7.zip", "plugins/nova-audio/thumb.png",
            "Lets a plugin play the MPC's audio inputs as its own sound, so a hardware synth becomes one ordinary MPC track: played, monitored, recorded, mixed and resampled on the plugin's own track, with no separate audio track and no Monitor switching. Written for Nova Editor but not MiniNova-specific; any plugin can read the tap.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3dthc - VST - Nova Audio\" && ZIP_PATH=\"./plugins/nova-audio/nova-audio.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/3dthc/mpc-addin-nova-audio/releases/download/nova-audio-v0.2.0/Nova-Audio-for-Nova-Editor-0.2.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/nova-audio\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/mpc-addin-nova-audio/releases/download/nova-audio-v0.2.0/Nova-Audio-for-Nova-Editor-0.2.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/3dthc - VST - Nova Audio/\"; elif [ -f \"./plugins/nova-audio/libnova_audio.so\" ]; then cp -rf ./plugins/nova-audio/* \"$TARGET_SYNTH_DIR/3dthc - VST - Nova Audio/\"; else echo \"[ERROR] No local binary archive found for nova-audio\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "audio-input", "hardware-synth", "mininova", "ld-preload" }
        },
        {
            "piremote", "πRemote", "Timo", "v0.1.0", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-13",
            "66", "64 KB", "14%",
            "Timo - VST - πRemote", "piremote_addin.so",
            "https://raw.githubusercontent.com/timomacquis/piRemote-Force/main/docs/piremote-panel.jpg", "https://github.com/timomacquis/piRemote-Force/releases/download/v0.1.0/piRemote-0.1.0-mpc-armv7.zip", "plugins/piremote/thumb.png",
            "Compact Windows transport and physical-button navigation companion for the official Remote Screen, on Force OS 3.10.1. Independent addin; localhost only, reached through an SSH tunnel. Windows app requires WSL Ubuntu and SSH key access.", "./plugins/piremote/install.sh", 0,
            0,
            false,
            false,
            "",
            { "remote", "transport", "navigation", "force", "windows" }
        }
        };
        if (!sec.items.empty()) sections.push_back(sec);
    }
    {
        Section sec;
        sec.title = "DEVICE PATCHES";
        sec.items = {
        {
            "patch-auto-exec", "Auto exec: run plugins from any noexec drive", "sd88me (mount logic after timomacquis)", "v1.0.0", "Device Patch",
            "PATCH", "Stable", "MPC OS Gen1 (Akai Force 3.9.1)", "MPC OS 2.x & 3.x (armv7l)", "2026-10-14",
            "412", "18 KB", "1%",
            "patches/auto-exec", "auto-exec-patch.sh",
            "", "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/1a6251e5dc90386a010179cb2173a51831cbbf4b/tools/mpc_patch/auto_exec/auto-exec-patch.sh", "plugins/patch-auto-exec/thumb.png",
            "Recommended for a noexec drive. Makes the VST2 Plugins folder of every noexec drive (SSD, USB stick, SD card) executable through a small boot-time service, with nothing to choose; the drives stay noexec. No MPC restart required and undo is built in.", "(\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/1a6251e5dc90386a010179cb2173a51831cbbf4b/tools/mpc_patch/auto_exec/auto-exec-patch.sh\" /tmp/auto-exec-patch.sh) && sh /tmp/auto-exec-patch.sh", 1,
            0,
            false,
            false,
            "",
            { "device-patch", "noexec", "systemd", "mount", "undo-built-in" }
        },
        {
            "patch-16-pad-drum-layout", "16-pad drum layout for selected plugins", "sd88me", "v1.0.0", "Device Patch",
            "PATCH", "Stable", "MPC OS 3.9.1.2 (armv7l)", "MPC OS 3.9.1.2", "2026-10-12",
            "318", "24 KB", "1%",
            "patches/drum-pad-layout", "mpc-drum-pad-patch.sh",
            "", "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/6ddf68cf92b3f1c0bdd322231ed133c0cc2eed48/tools/mpc_patch/mpc-drum-pad-patch.sh", "plugins/patch-16-pad-drum-layout/thumb.png",
            "Machinedrum Module / Machinemodule, 6W6, 8W8, CW-78, 9W9, TR-MPC and Lucky Dip get MPC's drum layout with all 16 pads lit. Backs up /usr/bin/MPC to /sdcard/MPC-backup with built-in undo.", "(\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/6ddf68cf92b3f1c0bdd322231ed133c0cc2eed48/tools/mpc_patch/mpc-drum-pad-patch.sh\" /tmp/mpc-drum-pad-patch.sh) && sh /tmp/mpc-drum-pad-patch.sh", 1,
            0,
            false,
            false,
            "",
            { "device-patch", "16-pad", "drum-layout", "tr-mpc", "6w6", "8w8", "9w9" }
        },
        {
            "patch-button-remap", "Button remap: open screens from hardware buttons", "mmiroshnikov", "v1.0.0", "Device Patch",
            "PATCH", "Stable", "MPC Live (Hakai 3.9.1), Force (3.9.0)", "MPC OS 3.9 (armv7l)", "2026-10-10",
            "275", "42 KB", "1%",
            "patches/hwremap", "hwremap-patch.sh",
            "", "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/a9bb8c71ba382fa6c3902fbbdead6d5f620222a5/tools/mpc_patch/hwremap/hwremap-patch.sh", "plugins/patch-button-remap/thumb.png",
            "Remap hardware buttons on a standalone MPC or Force via an LD_PRELOAD shim (/usr/lib/hwremap.so): a button can become other buttons, a pad, or a screen tap. Pick which remaps you want at install (Mixer, Edit, Clip, Menu, and Knobs options).", "(\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/a9bb8c71ba382fa6c3902fbbdead6d5f620222a5/tools/mpc_patch/hwremap/hwremap-patch.sh\" /tmp/hwremap-patch.sh) && sh /tmp/hwremap-patch.sh", 1,
            0,
            false,
            false,
            "",
            { "device-patch", "hwremap", "buttons", "ld-preload", "force", "mpc-live" }
        },
        {
            "patch-drive-exec", "Drive exec: run plugins from a noexec drive (SSD)", "timomacquis (adapted by sd88me)", "v1.0.0", "Device Patch",
            "PATCH", "Beta", "MPC OS Gen1 (Akai Force 3.9.1)", "MPC OS Gen1 (armv7l)", "2026-10-08",
            "164", "16 KB", "1%",
            "patches/drive-exec", "drive-exec-patch.sh",
            "", "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/f036a8791daff1ccf426c3953e84a8942c979564/tools/mpc_patch/drive_exec/drive-exec-patch.sh", "plugins/patch-drive-exec/thumb.png",
            "Makes one selected drive VST2 Plugins folder executable through a boot-time systemd bind-mount service while keeping the drive itself noexec. (Superseded by Auto exec, retained for uninstall compatibility.)", "(\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/f036a8791daff1ccf426c3953e84a8942c979564/tools/mpc_patch/drive_exec/drive-exec-patch.sh\" /tmp/drive-exec-patch.sh) && sh /tmp/drive-exec-patch.sh", 1,
            0,
            false,
            false,
            "",
            { "device-patch", "ssd", "noexec", "bind-mount" }
        }
        };
        if (!sec.items.empty()) sections.push_back(sec);
    }
    {
        Section sec;
        sec.title = "COMPANION APPS";
        sec.items = {
        {
            "companion-mpc-link", "MPC Link", "Padbangers", "v1.0.2", "Companion App",
            "APP", "Stable", "MPC X, MPC OS 3.9.1 (macOS & Windows)", "MPC OS 2.x & 3.x (SSH)", "2026-10-15",
            "690", "4.1 MB", "2%",
            "companion/mpc-link", "mpc-link-sshfs",
            "https://raw.githubusercontent.com/padbangers/mpc-link/main/assets/catalog.png", "https://github.com/padbangers/mpc-link/releases/download/v1.0.2/MPC-Link-1.0.2-macOS.zip", "plugins/companion-mpc-link/thumb.png",
            "Menu bar / tray app that puts folders from your computer into the MPC's browser over the network: load samples straight from the computer, save projects back to it. Connects over SSH, copies a static sshfs to the device and mounts the shared folders on a drive MPC already shows.", "echo \"[MPC Link] Companion app runs on macOS/Windows and connects to root@<mpc-ip> via SSHFS.\" && mkdir -p /media/AKAI_FORCE_SSD/MPC_Link_Shares", 1,
            0,
            false,
            false,
            "",
            { "companion-app", "sshfs", "network-mount", "macos", "windows", "samples" }
        }
        };
        if (!sec.items.empty()) sections.push_back(sec);
    }
}

static const char* find_matching_delim(const char* start, char open_c, char close_c) {
    if (!start || *start != open_c) return nullptr;
    int depth = 1;
    bool in_quotes = false;
    const char* p = start + 1;
    while (*p && depth > 0) {
        if (*p == '\\' && in_quotes && *(p + 1)) {
            p += 2;
            continue;
        }
        if (*p == '"') {
            in_quotes = !in_quotes;
        } else if (!in_quotes) {
            if (*p == open_c) depth++;
            else if (*p == close_c) {
                depth--;
                if (depth == 0) return p;
            }
        }
        p++;
    }
    return nullptr;
}

static bool extract_string_field(const char* start, const char* end, const char* key, std::string& out) {
    std::string pattern = std::string("\"") + key + "\"";
    const char* p = start;
    while (p < end) {
        const char* found = std::strstr(p, pattern.c_str());
        if (!found || found >= end) return false;
        const char* colon = found + pattern.size();
        while (colon < end && (*colon == ' ' || *colon == '\t' || *colon == '\r' || *colon == '\n')) colon++;
        if (colon < end && *colon == ':') {
            const char* val = colon + 1;
            while (val < end && (*val == ' ' || *val == '\t' || *val == '\r' || *val == '\n')) val++;
            if (val < end && *val == '"') {
                val++;
                out.clear();
                while (val < end && *val != '"') {
                    if (*val == '\\' && (val + 1) < end) {
                        val++;
                        if (*val == 'n') out += '\n';
                        else if (*val == 't') out += '\t';
                        else out += *val;
                    } else {
                        out += *val;
                    }
                    val++;
                }
                return true;
            }
        }
        p = found + pattern.size();
    }
    return false;
}

static void extract_string_array(const char* start, const char* end, const char* key, std::vector<std::string>& out) {
    std::string pattern = std::string("\"") + key + "\"";
    const char* found = std::strstr(start, pattern.c_str());
    if (!found || found >= end) return;
    const char* arr_open = std::strchr(found + pattern.size(), '[');
    if (!arr_open || arr_open >= end) return;
    const char* arr_close = find_matching_delim(arr_open, '[', ']');
    if (!arr_close || arr_close > end) return;

    out.clear();
    const char* p = arr_open + 1;
    while (p < arr_close) {
        const char* q1 = std::strchr(p, '"');
        if (!q1 || q1 >= arr_close) break;
        const char* q2 = q1 + 1;
        std::string s;
        while (q2 < arr_close && *q2 != '"') {
            if (*q2 == '\\' && (q2 + 1) < arr_close) q2++;
            s += *q2++;
        }
        if (!s.empty()) out.push_back(s);
        p = q2 + 1;
    }
}

static bool extract_int_field(const char* start, const char* end, const char* key, int& out) {
    std::string pattern = std::string("\"") + key + "\"";
    const char* found = std::strstr(start, pattern.c_str());
    if (!found || found >= end) return false;
    const char* colon = found + pattern.size();
    while (colon < end && (*colon == ' ' || *colon == '\t' || *colon == '\r' || *colon == '\n')) colon++;
    if (colon < end && *colon == ':') {
        const char* val = colon + 1;
        while (val < end && (*val == ' ' || *val == '\t' || *val == '\r' || *val == '\n')) val++;
        if (val < end && (*val == '-' || (*val >= '0' && *val <= '9'))) {
            out = std::atoi(val);
            return true;
        }
    }
    return false;
}

static bool extract_raw_code_field(const char* start, const char* end, std::string& out) {
    const char* found = std::strstr(start, "\"code\"");
    if (!found || found >= end) return false;
    const char* p = found + 6;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p >= end || *p != ':') return false;
    p++;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (p >= end || *p != '{') return false;

    const char* close_brace = find_matching_delim(p, '{', '}');
    if (!close_brace || close_brace > end) return false;

    out.assign(p + 1, close_brace - (p + 1));
    while (!out.empty() && (out.front() == '\n' || out.front() == '\r' || out.front() == ' ')) out.erase(out.begin());
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return true;
}

bool ConfigParser::loadFromFile(const std::string& configPath, LauncherConfig& outConfig) {
    outConfig.config_path = configPath;
    std::ifstream file(configPath, std::ios::binary);
    if (!file.is_open()) {
        std::printf("[TKGL-CFG] %s not found, loading embedded catalog.\n", configPath.c_str());
        outConfig.loadEmbeddedDefaults();
        refreshInstallStates(outConfig);
        return false;
    }

    std::stringstream ss;
    ss << file.rdbuf();
    std::string buf = ss.str();
    const char* raw = buf.c_str();

    // 1. Parse "global": { ... }
    const char* global_key = std::strstr(raw, "\"global\"");
    if (global_key) {
        const char* brace = std::strchr(global_key, '{');
        const char* end_brace = find_matching_delim(brace, '{', '}');
        if (brace && end_brace) {
            extract_string_field(brace, end_brace, "title", outConfig.app_title);
            extract_int_field(brace, end_brace, "rotation", outConfig.screen_rotation);
            extract_string_field(brace, end_brace, "catalog_url", outConfig.catalog_url);
            extract_string_field(brace, end_brace, "ping_host", outConfig.ping_host);
        }
    }
    if (outConfig.ping_host.empty()) {
        outConfig.ping_host = "github.com";
    }

    // 2. Parse "sections": [ ... ]
    outConfig.sections.clear();
    const char* sections_key = std::strstr(raw, "\"sections\"");
    if (sections_key) {
        const char* arr_start = std::strchr(sections_key, '[');
        const char* arr_end = find_matching_delim(arr_start, '[', ']');
        if (arr_start && arr_end) {
            const char* p = arr_start + 1;
            while (p < arr_end) {
                const char* sec_open = std::strchr(p, '{');
                if (!sec_open || sec_open >= arr_end) break;
                const char* sec_close = find_matching_delim(sec_open, '{', '}');
                if (!sec_close || sec_close > arr_end) break;

                Section sec;
                extract_string_field(sec_open, sec_close, "section", sec.title);
                if (sec.title.empty()) sec.title = "Plugins";

                const char* items_key = std::strstr(sec_open, "\"items\"");
                if (items_key && items_key < sec_close) {
                    const char* items_arr_open = std::strchr(items_key, '[');
                    const char* items_arr_close = find_matching_delim(items_arr_open, '[', ']');
                    if (items_arr_open && items_arr_close && items_arr_close <= sec_close) {
                        const char* ip = items_arr_open + 1;
                        while (ip < items_arr_close) {
                            const char* item_open = std::strchr(ip, '{');
                            if (!item_open || item_open >= items_arr_close) break;
                            const char* item_close = find_matching_delim(item_open, '{', '}');
                            if (!item_close || item_close > items_arr_close) break;

                            MenuItem item;
                            extract_string_field(item_open, item_close, "id", item.id);
                            extract_string_field(item_open, item_close, "item", item.label);
                            extract_string_field(item_open, item_close, "author", item.author);
                            extract_string_field(item_open, item_close, "version", item.version);
                            extract_string_field(item_open, item_close, "category", item.category);
                            if (item.category.empty()) {
                                extract_string_field(item_open, item_close, "style", item.category);
                            }
                            if (item.category.empty()) {
                                item.category = sec.title;
                            }
                            extract_string_field(item_open, item_close, "size", item.sizeMb);
                            extract_string_field(item_open, item_close, "updated", item.updatedDate);
                            extract_string_field(item_open, item_close, "downloads", item.downloads);
                            extract_string_field(item_open, item_close, "tested", item.testedDevice);
                            extract_string_field(item_open, item_close, "screenshot", item.screenshotUrl);
                            extract_string_field(item_open, item_close, "url", item.downloadUrl);
                            extract_string_field(item_open, item_close, "description", item.description);
                            extract_string_field(item_open, item_close, "target_subdir", item.targetSubdir);
                            extract_string_field(item_open, item_close, "binary", item.targetBinary);
                            extract_string_array(item_open, item_close, "tags", item.tags);

                            int instVal = 0;
                            if (extract_int_field(item_open, item_close, "installed", instVal)) {
                                item.installed = (instVal >= 0 && instVal <= 2) ? instVal : (instVal != 0 ? 1 : 0);
                            }

                            if (item.tags.empty()) {
                                item.tags = {"mpc", "standalone", "vst2", "armv7"};
                            }
                            if (!item.id.empty()) {
                                item.thumbPpmPath = "plugins/" + item.id + "/thumb.png";
                            }

                            if (extract_raw_code_field(item_open, item_close, item.cmd)) {
                                item.is_inline = 1;
                            } else if (extract_string_field(item_open, item_close, "script", item.cmd)) {
                                item.is_inline = 0;
                            }

                            if (item.downloadUrl.empty() && !item.cmd.empty()) {
                                size_t hpos = item.cmd.find("https://");
                                if (hpos != std::string::npos) {
                                    size_t hend = item.cmd.find_first_of(" \"'\t\r\n", hpos);
                                    item.downloadUrl = (hend != std::string::npos)
                                        ? item.cmd.substr(hpos, hend - hpos)
                                        : item.cmd.substr(hpos);
                                }
                            }

                            if (!item.label.empty()) {
                                sec.items.push_back(item);
                            }
                            ip = item_close + 1;
                        }
                    }
                }

                if (!sec.items.empty()) {
                    outConfig.sections.push_back(sec);
                }
                p = sec_close + 1;
            }
        }
    }

    if (outConfig.sections.empty()) {
        outConfig.loadEmbeddedDefaults();
    }
    refreshInstallStates(outConfig);
    return true;
}

static std::string escapeJsonString(const std::string& raw) {
    std::string out;
    out.reserve(raw.size() + 16);
    for (char c : raw) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

bool ConfigParser::saveToFile(const std::string& configPath, const LauncherConfig& config) {
    std::ofstream out(configPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    std::string pingHost = config.ping_host.empty() ? "github.com" : config.ping_host;
    out << "{\n";
    out << "  \"global\": {\n";
    out << "    \"title\": \"" << escapeJsonString(config.app_title) << "\",\n";
    out << "    \"rotation\": " << config.screen_rotation << ",\n";
    out << "    \"catalog_url\": \"" << escapeJsonString(config.catalog_url) << "\",\n";
    out << "    \"ping_host\": \"" << escapeJsonString(pingHost) << "\"\n";
    out << "  },\n";
    out << "  \"sections\": [\n";

    for (size_t s = 0; s < config.sections.size(); ++s) {
        const auto& sec = config.sections[s];
        out << "    {\n";
        out << "      \"section\": \"" << escapeJsonString(sec.title) << "\",\n";
        out << "      \"items\": [\n";
        for (size_t i = 0; i < sec.items.size(); ++i) {
            const auto& it = sec.items[i];
            out << "        {\n";
            out << "          \"id\": \"" << escapeJsonString(it.id) << "\",\n";
            out << "          \"item\": \"" << escapeJsonString(it.label) << "\",\n";
            out << "          \"author\": \"" << escapeJsonString(it.author) << "\",\n";
            out << "          \"version\": \"" << escapeJsonString(it.version) << "\",\n";
            out << "          \"category\": \"" << escapeJsonString(it.category) << "\",\n";
            out << "          \"size\": \"" << escapeJsonString(it.sizeMb) << "\",\n";
            out << "          \"updated\": \"" << escapeJsonString(it.updatedDate) << "\",\n";
            out << "          \"downloads\": \"" << escapeJsonString(it.downloads) << "\",\n";
            out << "          \"tested\": \"" << escapeJsonString(it.testedDevice) << "\",\n";
            out << "          \"screenshot\": \"" << escapeJsonString(it.screenshotUrl) << "\",\n";
            out << "          \"url\": \"" << escapeJsonString(it.downloadUrl) << "\",\n";
            out << "          \"description\": \"" << escapeJsonString(it.description) << "\",\n";
            out << "          \"target_subdir\": \"" << escapeJsonString(it.targetSubdir) << "\",\n";
            out << "          \"binary\": \"" << escapeJsonString(it.targetBinary) << "\",\n";
            out << "          \"installed\": " << it.installed << ",\n";
            if (it.is_inline) {
                out << "          \"code\": {\n            " << it.cmd << "\n          }\n";
            } else {
                out << "          \"script\": \"" << escapeJsonString(it.cmd) << "\"\n";
            }
            out << "        }" << (i + 1 < sec.items.size() ? "," : "") << "\n";
        }
        out << "      ]\n";
        out << "    }" << (s + 1 < config.sections.size() ? "," : "") << "\n";
    }
    out << "  ]\n}\n";
    return true;
}

bool ConfigParser::syncFromOnlineCatalog(
    LauncherConfig& config,
    std::vector<std::string>& outLogs,
    std::function<void(const std::string&)> onProgress
) {
    std::string mgrDir = getManagerRootDir();
    if (!mgrDir.empty()) {
        chdir(mgrDir.c_str());
    }

    // 1. Mandatory internet reachability check before any refresh operation
    std::string pingHost = config.ping_host.empty() ? "github.com" : config.ping_host;
    outLogs.push_back("[TKGL-SYNC] Checking internet access (ping " + pingHost + ")...");
    if (onProgress) onProgress("[TKGL-SYNC] Pinging " + pingHost + "...");
    if (!checkInternetReachable(pingHost, &outLogs)) {
        outLogs.push_back("[ERROR] No internet access (ping " + pingHost + " failed). Refresh aborted.");
        if (onProgress) onProgress("[ERROR] No internet (" + pingHost + " unreachable). Refresh impossible.");
        return false;
    }

    std::string url = config.catalog_url;
    if (url.empty()) url = "https://sd88me.github.io/mpc-vst-plugins/catalog.json";
    if (url.back() == '/') url += "catalog.json";
    std::string fallbackUrl = "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/gh-pages/catalog.json";

    mkdir("plugins", 0755);
    outLogs.push_back("[TKGL-SYNC] Downloading online catalog via libcurl (IPv4): " + url);
    if (onProgress) onProgress("[TKGL-SYNC] Fetching catalog JSON via libcurl (IPv4)...");

    std::string tmpCat = "/tmp/tkgl_catalog_dl.json";
    unlink(tmpCat.c_str());
    bool dlOk = downloadFile(url, tmpCat, 20, onProgress, "[TKGL-SYNC] Downloading catalog");
    if (!dlOk && url != fallbackUrl) {
        outLogs.push_back("[TKGL-SYNC] Trying raw GitHub mirror: " + fallbackUrl);
        dlOk = downloadFile(fallbackUrl, tmpCat, 20, onProgress, "[TKGL-SYNC] Downloading mirror catalog");
    }
    if (!dlOk) {
        outLogs.push_back("[ERROR] Failed to download online catalog via libcurl. Refresh aborted.");
        if (onProgress) onProgress("[ERROR] Catalog download failed via libcurl.");
        return false;
    }

    {
        std::ifstream src(tmpCat, std::ios::binary);
        std::ofstream dst("plugins/tkgl_catalog.json", std::ios::binary | std::ios::trunc);
        if (src.is_open() && dst.is_open()) {
            dst << src.rdbuf();
            dst.close();
        }
        unlink(tmpCat.c_str());
    }

    struct stat stCat;
    if (stat("plugins/tkgl_catalog.json", &stCat) != 0 || stCat.st_size < 64) {
        outLogs.push_back("[ERROR] Invalid or empty plugins/tkgl_catalog.json.");
        return false;
    }

    std::ifstream file("plugins/tkgl_catalog.json", std::ios::binary);
    if (!file.is_open()) {
        outLogs.push_back("[ERROR] Could not open plugins/tkgl_catalog.json.");
        return false;
    }
    outLogs.push_back(std::string("[TKGL-SYNC] Loaded catalog plugins/tkgl_catalog.json (") + std::to_string(stCat.st_size / 1024) + " KB)");
    if (onProgress) onProgress("[TKGL-SYNC] Parsing catalog entries...");

    std::stringstream ss;
    ss << file.rdbuf();
    std::string buf = ss.str();
    const char* raw = buf.c_str();

    const char* plugins_key = std::strstr(raw, "\"plugins\"");
    if (!plugins_key) {
        outLogs.push_back("[ERROR] Invalid catalog.json: missing plugins array.");
        return false;
    }

    const char* arr_open = std::strchr(plugins_key, '[');
    const char* arr_close = find_matching_delim(arr_open, '[', ']');
    if (!arr_open || !arr_close) return false;

    Section secInst;    secInst.title    = "INSTRUMENTS";
    Section secMidi;    secMidi.title    = "MIDI";
    Section secFx;      secFx.title      = "EFFECTS";
    Section secAddins;  secAddins.title  = "ADDINS";
    Section secPatches; secPatches.title = "DEVICE PATCHES";
    Section secApps;    secApps.title    = "COMPANION APPS";

    // Preserve existing DEVICE PATCHES and COMPANION APPS items, and remember previously installed plugins
    std::unordered_map<std::string, bool> prevInstalledMap;
    for (const auto& existingSec : config.sections) {
        if (existingSec.title == "DEVICE PATCHES") secPatches.items = existingSec.items;
        if (existingSec.title == "COMPANION APPS") secApps.items = existingSec.items;
        for (const auto& existingIt : existingSec.items) {
            if (existingIt.installed) {
                if (!existingIt.id.empty()) prevInstalledMap[existingIt.id] = true;
                if (!existingIt.label.empty()) prevInstalledMap[existingIt.label] = true;
            }
        }
    }

    auto ensureThumbDownloaded = [&](MenuItem& it, int itemNumber) {
        if (it.id.empty()) return;
        std::string plugDir = "plugins/" + it.id;
        mkdir("plugins", 0755);
        mkdir(plugDir.c_str(), 0755);
        std::string localPng = plugDir + "/thumb.png";
        std::string localPpm = plugDir + "/thumb.ppm";

        // Normalize relative screenshot URL if needed
        if (!it.screenshotUrl.empty() &&
            it.screenshotUrl.rfind("http://", 0) != 0 &&
            it.screenshotUrl.rfind("https://", 0) != 0) {
            std::string baseUrl = url;
            size_t slashPos = baseUrl.find_last_of('/');
            if (slashPos != std::string::npos) {
                baseUrl = baseUrl.substr(0, slashPos + 1);
            } else {
                baseUrl += "/";
            }
            if (!it.screenshotUrl.empty() && it.screenshotUrl[0] == '/') {
                it.screenshotUrl.erase(0, 1);
            }
            it.screenshotUrl = baseUrl + it.screenshotUrl;
        }

        struct stat stPng;
        if (stat(localPng.c_str(), &stPng) == 0 && stPng.st_size < 64) {
            unlink(localPng.c_str());
        }
        bool hasPng = (stat(localPng.c_str(), &stPng) == 0 && stPng.st_size >= 64);

        // If thumb.png does not exist on disk, download it via libcurl (IPv4)
        if (!hasPng && !it.screenshotUrl.empty()) {
            if (onProgress) {
                int dotCount = (itemNumber % 5) + 1;
                std::string dots;
                for (int d = 0; d < 5; ++d) dots += (d < dotCount) ? " * " : " . ";
                onProgress("[SYNC " + dots + "] Downloading thumb: " + it.label);
            }
            if (downloadFile(it.screenshotUrl, localPng, 15)) {
                hasPng = (stat(localPng.c_str(), &stPng) == 0 && stPng.st_size >= 64);
                if (hasPng) {
                    unlink(localPpm.c_str());
                    outLogs.push_back("[TKGL-SYNC] Downloaded thumb: " + localPng);
                }
            } else {
                outLogs.push_back("[WARNING] Failed to download thumb for " + it.label + ": " + it.screenshotUrl);
            }
        }
        it.thumbPpmPath = localPng;
    };

    const char* p = arr_open + 1;
    int count = 0;
    while (p < arr_close) {
        const char* p_open = std::strchr(p, '{');
        if (!p_open || p_open >= arr_close) break;
        const char* p_close = find_matching_delim(p_open, '{', '}');
        if (!p_close || p_close > arr_close) break;

        MenuItem item;
        extract_string_field(p_open, p_close, "id", item.id);
        extract_string_field(p_open, p_close, "name", item.label);
        extract_string_field(p_open, p_close, "author", item.author);
        extract_string_field(p_open, p_close, "style", item.category);
        if (item.category.empty()) extract_string_field(p_open, p_close, "category", item.category);
        extract_string_field(p_open, p_close, "summary", item.description);
        extract_string_field(p_open, p_close, "screenshot", item.screenshotUrl);
        if (item.screenshotUrl.empty()) extract_string_field(p_open, p_close, "image", item.screenshotUrl);
        if (item.screenshotUrl.empty()) extract_string_field(p_open, p_close, "thumbnail", item.screenshotUrl);
        extract_string_array(p_open, p_close, "tags", item.tags);
        if (item.tags.empty()) item.tags = {"synth", "mpc", "vst2"};

        std::string kind, role;
        extract_string_field(p_open, p_close, "kind", kind);
        extract_string_field(p_open, p_close, "role", role);

        // Extract latest version entry from versions[0]
        const char* vers_key = std::strstr(p_open, "\"versions\"");
        if (vers_key && vers_key < p_close) {
            const char* v_arr = std::strchr(vers_key, '[');
            const char* v_obj = v_arr ? std::strchr(v_arr, '{') : nullptr;
            const char* v_close = v_obj ? find_matching_delim(v_obj, '{', '}') : nullptr;
            if (v_obj && v_close && v_close <= p_close) {
                std::string ver;
                if (extract_string_field(v_obj, v_close, "version", ver)) item.version = "v" + ver;
                extract_string_field(v_obj, v_close, "date", item.updatedDate);
                if (item.screenshotUrl.empty()) {
                    extract_string_field(v_obj, v_close, "screenshot", item.screenshotUrl);
                }

                // Extract download URL: check assets.armv7.url first, then download_url/package_url, then scan all "url" fields for .zip/release asset
                const char* armv7_key = std::strstr(v_obj, "\"armv7\"");
                if (armv7_key && armv7_key < v_close) {
                    const char* arm_open = std::strchr(armv7_key, '{');
                    const char* arm_close = arm_open ? find_matching_delim(arm_open, '{', '}') : nullptr;
                    if (arm_open && arm_close && arm_close <= v_close) {
                        extract_string_field(arm_open, arm_close, "url", item.downloadUrl);
                    }
                }
                if (item.downloadUrl.empty()) {
                    extract_string_field(v_obj, v_close, "download_url", item.downloadUrl);
                }
                if (item.downloadUrl.empty()) {
                    extract_string_field(v_obj, v_close, "package_url", item.downloadUrl);
                }
                if (item.downloadUrl.empty()) {
                    const char* scanP = v_obj;
                    std::string firstUrl;
                    while (scanP < v_close) {
                        const char* uFound = std::strstr(scanP, "\"url\"");
                        if (!uFound || uFound >= v_close) break;
                        std::string candidate;
                        if (extract_string_field(uFound, v_close, "url", candidate) && !candidate.empty()) {
                            if (firstUrl.empty()) firstUrl = candidate;
                            if (candidate.find(".zip") != std::string::npos ||
                                candidate.find("/releases/download/") != std::string::npos ||
                                candidate.find("raw.githubusercontent.com") != std::string::npos) {
                                item.downloadUrl = candidate;
                                break;
                            }
                        }
                        scanP = uFound + 5;
                    }
                    if (item.downloadUrl.empty()) item.downloadUrl = firstUrl;
                }

                int sizeB = 0, dl = 0;
                if (extract_int_field(v_obj, v_close, "size", sizeB) && sizeB > 0) {
                    char mbBuf[32];
                    std::snprintf(mbBuf, sizeof(mbBuf), "%.1f MB", static_cast<double>(sizeB) / (1024.0 * 1024.0));
                    item.sizeMb = mbBuf;
                }
                if (extract_int_field(v_obj, v_close, "downloads", dl)) {
                    item.downloads = std::to_string(dl);
                }
                extract_string_field(v_obj, v_close, "so", item.targetBinary);
                extract_string_field(v_obj, v_close, "skin", item.targetSubdir);
                extract_string_field(v_obj, v_close, "os_compat", item.osCompat);
                std::string dev;
                if (extract_string_field(v_obj, v_close, "device", dev)) item.testedDevice = dev;
            }
        }
        if (item.downloadUrl.empty()) {
            extract_string_field(p_open, p_close, "url", item.downloadUrl);
        }

        if (item.targetSubdir.empty()) item.targetSubdir = item.author + " - VST - " + item.label;
        if (item.targetBinary.empty()) item.targetBinary = item.id + ".so";

        // Preserve previous installed detection state so refreshInstallStates verifies MPC.settings & .so
        if ((!item.id.empty() && prevInstalledMap[item.id]) || (!item.label.empty() && prevInstalledMap[item.label])) {
            item.installed = true;
        }

        // Download thumb.png if it does not exist on disk
        ensureThumbDownloaded(item, count + 1);

        item.is_inline = 1;
        item.cmd = "mkdir -p \"$SYNTH_INSTALL_DIR/" + item.targetSubdir + "\" && (\"$PLG_MAN_DIR/mpc_plugin_manager_fb\" --fetch \"" + item.downloadUrl + "\" /tmp/" + item.id + ".zip) && unzip -o /tmp/" + item.id + ".zip -d \"$SYNTH_INSTALL_DIR/" + item.targetSubdir + "/\" && rm -f /tmp/" + item.id + ".zip && sync";

        if (!item.label.empty()) {
            if (kind == "addin") {
                item.shortType = "ADDIN";
                secAddins.items.push_back(item);
            } else if (role == "midi" || item.category.find("MIDI") != std::string::npos || item.category.find("Sequencer") != std::string::npos) {
                item.shortType = "MIDI";
                secMidi.items.push_back(item);
            } else if (kind == "effect" || item.category.find("Effect") != std::string::npos) {
                item.shortType = "FX";
                secFx.items.push_back(item);
            } else {
                item.shortType = "INST";
                secInst.items.push_back(item);
            }
            count++;
            if (onProgress) {
                int dotCount = (count % 5) + 1;
                std::string dots;
                for (int d = 0; d < 5; ++d) dots += (d < dotCount) ? " * " : " . ";
                onProgress("[SYNC " + dots + "] (" + std::to_string(count) + ") " + item.label + " (" + item.category + ")");
            }
        }
        p = p_close + 1;
    }

    // Also ensure thumbnails are downloaded for any preserved DEVICE PATCHES and COMPANION APPS items
    for (auto& patchIt : secPatches.items) {
        ensureThumbDownloaded(patchIt, ++count);
    }
    for (auto& appIt : secApps.items) {
        ensureThumbDownloaded(appIt, ++count);
    }

    if (count > 0) {
        config.sections.clear();
        if (!secInst.items.empty())    config.sections.push_back(secInst);
        if (!secMidi.items.empty())    config.sections.push_back(secMidi);
        if (!secFx.items.empty())      config.sections.push_back(secFx);
        if (!secAddins.items.empty())  config.sections.push_back(secAddins);
        if (!secPatches.items.empty()) config.sections.push_back(secPatches);
        if (!secApps.items.empty())    config.sections.push_back(secApps);
        refreshInstallStates(config, &outLogs);
        saveToFile(config.config_path, config);
        outLogs.push_back("[TKGL-SYNC] Classified " + std::to_string(count) + " plugins into " + std::to_string(config.sections.size()) + " sections in " + config.config_path);
        return true;
    }
    return false;
}

// ============================================================================
// Safe MPC.settings XML Manipulation & Integrity Engine
// ============================================================================

static std::string getBackupTimestampYYYYMMDDHHMMSS() {
    std::time_t now = std::time(nullptr);
    struct tm tmBuf;
    struct tm* lt = localtime_r(&now, &tmBuf);
    char buf[32];
    if (lt && std::strftime(buf, sizeof(buf), "%Y%m%d%H%M%S", lt) > 0) {
        return std::string(buf);
    }
    return "20260101000000";
}

static std::string xmlUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (s.compare(i, 6, "&apos;") == 0) { out += '\''; i += 5; continue; }
            if (s.compare(i, 6, "&quot;") == 0) { out += '"';  i += 5; continue; }
            if (s.compare(i, 4, "&lt;")   == 0) { out += '<';  i += 3; continue; }
            if (s.compare(i, 4, "&gt;")   == 0) { out += '>';  i += 3; continue; }
            if (s.compare(i, 5, "&amp;")  == 0) { out += '&';  i += 4; continue; }
        }
        out += s[i];
    }
    return out;
}

static std::string xmlEscapeAttr(const std::string& raw) {
    std::string unescaped = xmlUnescape(raw);
    std::string out;
    out.reserve(unescaped.size() + 16);
    for (char c : unescaped) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            default:   out += c;        break;
        }
    }
    return out;
}

static std::string xmlEscapeText(const std::string& raw) {
    std::string unescaped = xmlUnescape(raw);
    std::string out;
    out.reserve(unescaped.size() + 16);
    for (char c : unescaped) {
        switch (c) {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            default:   out += c;        break;
        }
    }
    return out;
}

static std::string resolveMpcSettingsPath() {
    const char* candMpc[] = {
        "/media/az01-internal/Settings/MPC/MPC.settings",
        "/media/internal/Settings/MPC/MPC.settings",
        "/var/lib/MPC/MPC.settings",
        "/root/.config/Akai/MPC/MPC.settings"
    };
    for (const char* cp : candMpc) {
        struct stat st;
        if (stat(cp, &st) == 0 && S_ISREG(st.st_mode)) {
            return std::string(cp);
        }
    }
    return "/media/az01-internal/Settings/MPC/MPC.settings";
}

// Finds the closing '>' of an XML tag starting at tagStart (quote-aware across multiple lines).
// Sets outIsSelfClosing = true if the tag ends with "/>". Returns index right after '>', or npos.
static size_t findXmlTagEnd(const std::string& xml, size_t tagStart, bool& outIsSelfClosing) {
    outIsSelfClosing = false;
    if (tagStart >= xml.size() || xml[tagStart] != '<') return std::string::npos;
    bool inDoubleQuote = false;
    bool inSingleQuote = false;
    char lastNonSpace = 0;
    for (size_t i = tagStart + 1; i < xml.size(); ++i) {
        char c = xml[i];
        if (c == '"' && !inSingleQuote) {
            inDoubleQuote = !inDoubleQuote;
        } else if (c == '\'' && !inDoubleQuote) {
            inSingleQuote = !inSingleQuote;
        } else if (!inDoubleQuote && !inSingleQuote) {
            if (c == '>') {
                outIsSelfClosing = (lastNonSpace == '/');
                return i + 1;
            }
            if (c == '<') {
                // Unexpected '<' before closing '>' -> malformed tag
                return std::string::npos;
            }
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                lastNonSpace = c;
            }
        }
    }
    return std::string::npos;
}

static std::string getXmlTagAttribute(const std::string& tag, const std::string& attrName) {
    std::string needle = attrName + "=";
    size_t pos = 0;
    while ((pos = tag.find(needle, pos)) != std::string::npos) {
        if (pos > 0) {
            char prev = tag[pos - 1];
            if (prev != ' ' && prev != '\t' && prev != '\r' && prev != '\n') {
                pos += needle.size();
                continue;
            }
        }
        size_t valStart = pos + needle.size();
        if (valStart < tag.size() && (tag[valStart] == '"' || tag[valStart] == '\'')) {
            char quote = tag[valStart];
            size_t valEnd = tag.find(quote, valStart + 1);
            if (valEnd != std::string::npos) {
                return xmlUnescape(tag.substr(valStart + 1, valEnd - (valStart + 1)));
            }
        }
        pos += needle.size();
    }
    return "";
}

static std::string setXmlTagAttribute(const std::string& tag, const std::string& attrName, const std::string& newRawValue) {
    std::string escapedVal = xmlEscapeAttr(newRawValue);
    std::string needle = attrName + "=";
    size_t pos = 0;
    while ((pos = tag.find(needle, pos)) != std::string::npos) {
        if (pos > 0) {
            char prev = tag[pos - 1];
            if (prev != ' ' && prev != '\t' && prev != '\r' && prev != '\n') {
                pos += needle.size();
                continue;
            }
        }
        size_t valStart = pos + needle.size();
        if (valStart < tag.size() && (tag[valStart] == '"' || tag[valStart] == '\'')) {
            char quote = tag[valStart];
            size_t valEnd = tag.find(quote, valStart + 1);
            if (valEnd != std::string::npos) {
                std::string updated = tag;
                updated.replace(valStart, (valEnd - valStart) + 1, std::string("\"") + escapedVal + "\"");
                return updated;
            }
        }
        pos += needle.size();
    }

    // If attrName is not present on the tag, insert it!
    // Prefer inserting right after version="..." if present (canonical JUCE <PLUGIN> order),
    // otherwise insert right before "/>" or ">".
    std::string attrInsertion = " " + attrName + "=\"" + escapedVal + "\"";
    size_t verPos = 0;
    const std::string verNeedle = "version=";
    while ((verPos = tag.find(verNeedle, verPos)) != std::string::npos) {
        if (verPos > 0) {
            char prev = tag[verPos - 1];
            if (prev != ' ' && prev != '\t' && prev != '\r' && prev != '\n') {
                verPos += verNeedle.size();
                continue;
            }
        }
        size_t vStart = verPos + verNeedle.size();
        if (vStart < tag.size() && (tag[vStart] == '"' || tag[vStart] == '\'')) {
            char quote = tag[vStart];
            size_t vEnd = tag.find(quote, vStart + 1);
            if (vEnd != std::string::npos) {
                std::string updated = tag;
                updated.insert(vEnd + 1, attrInsertion);
                return updated;
            }
        }
        verPos += verNeedle.size();
    }

    size_t selfClose = tag.rfind("/>");
    if (selfClose != std::string::npos) {
        std::string updated = tag;
        updated.insert(selfClose, attrInsertion);
        return updated;
    }
    size_t endBracket = tag.rfind('>');
    if (endBracket != std::string::npos) {
        std::string updated = tag;
        updated.insert(endBracket, attrInsertion);
        return updated;
    }
    return tag;
}

static void collectXplFilesRecursive(const std::string& dirPath, std::vector<std::string>& outXplPaths, int depth = 0) {
    if (depth > 8) return;
    DIR* d = opendir(dirPath.c_str());
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (std::strcmp(de->d_name, ".") == 0 || std::strcmp(de->d_name, "..") == 0) continue;
        std::string fullPath = dirPath + "/" + de->d_name;
        struct stat st;
        if (stat(fullPath.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            collectXplFilesRecursive(fullPath, outXplPaths, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            size_t len = std::strlen(de->d_name);
            if (len > 4) {
                const char* ext = de->d_name + (len - 4);
                if (ext[0] == '.' &&
                    (ext[1] == 'x' || ext[1] == 'X') &&
                    (ext[2] == 'p' || ext[2] == 'P') &&
                    (ext[3] == 'l' || ext[3] == 'L')) {
                    outXplPaths.push_back(fullPath);
                }
            }
        }
    }
    closedir(d);
}

// Updates all .xpl preset files in pluginTargetDir (e.g. Presets/*.xpl) so that
// the <PLUGIN .../> element has file="<target .so path>" set or updated.
static int updatePluginPresetsXplFiles(
    const std::string& pluginTargetDir,
    const std::string& primarySoPath,
    const std::vector<std::string>& pluginMetaTags
) {
    if (primarySoPath.empty() && pluginMetaTags.empty()) return 0;
    std::vector<std::string> xplFiles;
    collectXplFilesRecursive(pluginTargetDir, xplFiles);
    if (xplFiles.empty()) return 0;

    std::sort(xplFiles.begin(), xplFiles.end());

    int updatedCount = 0;
    for (const auto& xplPath : xplFiles) {
        std::ifstream in(xplPath, std::ios::binary);
        if (!in.is_open()) continue;
        std::stringstream buffer;
        buffer << in.rdbuf();
        std::string content = buffer.str();
        in.close();
        if (content.empty()) continue;

        // Bound search before <state> so we never scan or touch the binary/base64 state payload
        size_t searchLimit = content.find("<state");
        if (searchLimit == std::string::npos) {
            searchLimit = content.size();
        }

        bool foundPluginTag = false;
        size_t pos = 0;
        while (pos < searchLimit && (pos = content.find("<PLUGIN", pos)) != std::string::npos && pos < searchLimit) {
            if (pos + 7 < content.size()) {
                char nextC = content[pos + 7];
                if (nextC != ' ' && nextC != '\t' && nextC != '\r' && nextC != '\n' && nextC != '/' && nextC != '>') {
                    pos += 7;
                    continue;
                }
            }
            bool selfClosing = false;
            size_t tagEnd = findXmlTagEnd(content, pos, selfClosing);
            if (tagEnd == std::string::npos || tagEnd > searchLimit) break;

            std::string rawPlugTag = content.substr(pos, tagEnd - pos);
            std::string plugName = getXmlTagAttribute(rawPlugTag, "name");
            std::string plugUid  = getXmlTagAttribute(rawPlugTag, "uid");

            std::string resolvedSo = primarySoPath;
            for (const auto& mt : pluginMetaTags) {
                std::string mtFile = getXmlTagAttribute(mt, "file");
                if (mtFile.empty()) continue;
                std::string mtName = getXmlTagAttribute(mt, "name");
                std::string mtUid  = getXmlTagAttribute(mt, "uid");
                if ((!plugUid.empty() && plugUid == mtUid) || (!plugName.empty() && plugName == mtName)) {
                    resolvedSo = mtFile;
                    break;
                }
            }

            if (!resolvedSo.empty()) {
                std::string newPlugTag = setXmlTagAttribute(rawPlugTag, "file", resolvedSo);
                if (newPlugTag != rawPlugTag) {
                    std::ptrdiff_t diff = static_cast<std::ptrdiff_t>(newPlugTag.size()) - static_cast<std::ptrdiff_t>(rawPlugTag.size());
                    content.replace(pos, tagEnd - pos, newPlugTag);
                    searchLimit = static_cast<size_t>(static_cast<std::ptrdiff_t>(searchLimit) + diff);
                    tagEnd = pos + newPlugTag.size();
                }
                foundPluginTag = true;
            }
            pos = tagEnd;
        }

        if (foundPluginTag) {
            std::string tmpXpl = "/tmp/tkgl_preset_staged.xpl";
            unlink(tmpXpl.c_str());
            bool stagedOk = false;
            {
                std::ofstream outTmp(tmpXpl, std::ios::binary | std::ios::trunc);
                if (outTmp.is_open()) {
                    outTmp.write(content.data(), static_cast<std::streamsize>(content.size()));
                    outTmp.flush();
                    stagedOk = !outTmp.fail();
                    outTmp.close();
                }
            }
            if (stagedOk) {
                std::ofstream out(xplPath, std::ios::binary | std::ios::trunc);
                if (out.is_open()) {
                    out.write(content.data(), static_cast<std::streamsize>(content.size()));
                    out.flush();
                    if (!out.fail()) {
                        updatedCount++;
                    }
                    out.close();
                }
                unlink(tmpXpl.c_str());
            }
        }
    }
    return updatedCount;
}

// Locates <VALUE name="valueName"...> block in MPC.settings.
// Properly distinguishes self-closing <VALUE name="..."/> from <VALUE name="...">...</VALUE>.
static bool findValueBlock(
    const std::string& xml,
    const std::string& valueName,
    size_t& outBlockStart,
    size_t& outOpenTagEnd,
    size_t& outBlockEnd,
    bool& outIsSelfClosing
) {
    size_t pos = 0;
    while ((pos = xml.find("<VALUE", pos)) != std::string::npos) {
        if (pos + 6 < xml.size()) {
            char nextC = xml[pos + 6];
            if (nextC != ' ' && nextC != '\t' && nextC != '\r' && nextC != '\n' && nextC != '/' && nextC != '>') {
                pos += 6;
                continue;
            }
        }
        bool selfClosing = false;
        size_t openEnd = findXmlTagEnd(xml, pos, selfClosing);
        if (openEnd == std::string::npos) return false;
        std::string openTag = xml.substr(pos, openEnd - pos);
        if (getXmlTagAttribute(openTag, "name") == valueName) {
            outBlockStart = pos;
            outOpenTagEnd = openEnd;
            outIsSelfClosing = selfClosing;
            if (selfClosing) {
                outBlockEnd = openEnd;
                return true;
            }
            size_t closeVal = xml.find("</VALUE>", openEnd);
            if (closeVal == std::string::npos) return false;
            outBlockEnd = closeVal + 8;
            return true;
        }
        pos = openEnd;
    }
    return false;
}

struct MpcPluginXmlEntry {
    size_t startPos = 0;
    size_t endPos   = 0;
    std::string rawTag;
    std::string name;
    std::string descName;
    std::string uid;
    std::string file;
};

// Extracts all <PLUGIN .../> (or <PLUGIN ...>...</PLUGIN>) elements within [rangeStart, rangeEnd),
// including multi-line <PLUGIN> elements wrapped by JUCE XmlElement!
static std::vector<MpcPluginXmlEntry> extractPluginXmlEntries(const std::string& xml, size_t rangeStart, size_t rangeEnd) {
    std::vector<MpcPluginXmlEntry> entries;
    size_t pos = rangeStart;
    while (pos < rangeEnd && (pos = xml.find("<PLUGIN", pos)) != std::string::npos && pos < rangeEnd) {
        if (pos + 7 < xml.size()) {
            char c = xml[pos + 7];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n' && c != '/' && c != '>') {
                pos += 7;
                continue;
            }
        }
        bool selfClosing = false;
        size_t tagEnd = findXmlTagEnd(xml, pos, selfClosing);
        if (tagEnd == std::string::npos || tagEnd > rangeEnd) break;
        size_t elemEnd = tagEnd;
        if (!selfClosing) {
            size_t closeTag = xml.find("</PLUGIN>", tagEnd);
            if (closeTag != std::string::npos && closeTag + 9 <= rangeEnd) {
                elemEnd = closeTag + 9;
            }
        }
        MpcPluginXmlEntry e;
        e.startPos = pos;
        e.endPos   = elemEnd;
        e.rawTag   = xml.substr(pos, elemEnd - pos);
        e.name     = getXmlTagAttribute(e.rawTag, "name");
        e.descName = getXmlTagAttribute(e.rawTag, "descriptiveName");
        e.uid      = getXmlTagAttribute(e.rawTag, "uid");
        e.file     = getXmlTagAttribute(e.rawTag, "file");
        entries.push_back(e);
        pos = elemEnd;
    }
    return entries;
}

// Collapses multi-line whitespace inside an XML tag (outside quotes) into single spaces
[[maybe_unused]] static inline std::string normalizeSingleLineXmlTag(const std::string& tag) {
    std::string out;
    out.reserve(tag.size());
    bool inDoubleQuote = false;
    bool inSingleQuote = false;
    bool lastWasSpace  = false;
    for (size_t i = 0; i < tag.size(); ++i) {
        char c = tag[i];
        if (c == '"' && !inSingleQuote) {
            inDoubleQuote = !inDoubleQuote;
            out += c;
            lastWasSpace = false;
        } else if (c == '\'' && !inDoubleQuote) {
            inSingleQuote = !inSingleQuote;
            out += c;
            lastWasSpace = false;
        } else if (!inDoubleQuote && !inSingleQuote && (c == ' ' || c == '\t' || c == '\r' || c == '\n')) {
            if (!lastWasSpace && !out.empty() && out.back() != '<') {
                out += ' ';
                lastWasSpace = true;
            }
        } else {
            if (!inDoubleQuote && !inSingleQuote && (c == '>' || (c == '/' && i + 1 < tag.size() && tag[i + 1] == '>'))) {
                if (!out.empty() && out.back() == ' ') out.pop_back();
            }
            out += c;
            lastWasSpace = false;
        }
    }
    return out;
}

// Robust structural XML validator for MPC.settings before committing any changes to disk
static bool validateMpcSettingsXml(const std::string& xml, std::string& outErrorReason) {
    if (xml.size() < 32) {
        outErrorReason = "XML content is too short or empty";
        return false;
    }
    size_t propOpen = xml.find("<PROPERTIES");
    if (propOpen == std::string::npos) propOpen = xml.find("<Properties");
    if (propOpen == std::string::npos) propOpen = xml.find("<properties");

    size_t propClose = xml.rfind("</PROPERTIES>");
    if (propClose == std::string::npos) propClose = xml.rfind("</Properties>");
    if (propClose == std::string::npos) propClose = xml.rfind("</properties>");

    if (propOpen == std::string::npos || propClose == std::string::npos || propClose <= propOpen) {
        outErrorReason = "Missing or mismatched <PROPERTIES> root element";
        return false;
    }

    size_t propOpenClose = xml.find('>', propOpen);
    if (propOpenClose == std::string::npos || propOpenClose >= propClose) {
        outErrorReason = "Unclosed <PROPERTIES> opening tag";
        return false;
    }

    // Verify basic bracket balancing across all non-comment non-CDATA sections
    int depth = 0;
    bool inQuote = false;
    char quoteChar = 0;
    for (size_t i = 0; i < xml.size(); ++i) {
        char c = xml[i];
        if (inQuote) {
            if (c == quoteChar) inQuote = false;
            continue;
        }
        if (c == '"' || c == '\'') {
            inQuote = true;
            quoteChar = c;
            continue;
        }
        if (c == '<') {
            depth++;
            if (depth > 200) {
                outErrorReason = "Excessively nested XML brackets";
                return false;
            }
        } else if (c == '>') {
            depth--;
            if (depth < 0) {
                outErrorReason = "Unmatched closing bracket '>' at offset " + std::to_string(i);
                return false;
            }
        }
    }
    if (depth != 0) {
        outErrorReason = "Unclosed opening bracket '<' in XML";
        return false;
    }

    return true;
}

static bool s_mpcSettingsDirty = false;
static const char* SESSION_STAGED_MPC_SETTINGS = "/tmp/MPC.settings.new";

static void ensureDirectoryExists(const std::string& path) {
    if (path.empty()) return;
    std::string tmp = path;
    for (size_t i = 1; i < tmp.size(); ++i) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            mkdir(tmp.c_str(), 0755);
            tmp[i] = '/';
        }
    }
    mkdir(tmp.c_str(), 0755);
}

static std::string resolveBackupDirectory() {
    std::string mgrDir = ConfigParser::getManagerRootDir();
    std::string bakDir;
    if (!mgrDir.empty() && mgrDir != "/") {
        bakDir = mgrDir + "/backups";
    } else {
        bakDir = "./backups";
    }
    while (bakDir.size() > 1 && bakDir.back() == '/') bakDir.pop_back();
    ensureDirectoryExists(bakDir);
    return bakDir;
}

static void enforceMpcSettingsBackupRetention(
    size_t maxKeep = 10,
    std::vector<std::string>* outLogs = nullptr
) {
    std::string dirPath = resolveBackupDirectory();
    DIR* dir = opendir(dirPath.c_str());
    if (!dir) return;

    struct BakEntry {
        std::string filename;
        std::string fullPath;
    };
    std::vector<BakEntry> entries;
    const std::string prefix = "MPC.settings.bak-";

    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        std::string fname = ent->d_name;
        if (fname.rfind(prefix, 0) == 0) {
            std::string fullPath = dirPath + "/" + fname;
            struct stat st;
            if (stat(fullPath.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                // Rule: Immediately remove any corrupted, 0-byte, truncated, or XML-invalid backup
                bool isValidBackup = false;
                std::string valReason = "too small (< 64 bytes)";
                if (st.st_size >= 64) {
                    std::ifstream inBak(fullPath, std::ios::binary);
                    if (inBak.is_open()) {
                        std::stringstream bss;
                        bss << inBak.rdbuf();
                        inBak.close();
                        std::string bContent = bss.str();
                        if (validateMpcSettingsXml(bContent, valReason)) {
                            isValidBackup = true;
                        }
                    } else {
                        valReason = "unreadable file";
                    }
                }
                if (!isValidBackup) {
                    unlink(fullPath.c_str());
                    std::string cleanMsg = "[TKGL-CFG] Removed non-integral/corrupted backup: " + fname + " (" + valReason + ")";
                    ConfigParser::logMessage(cleanMsg);
                    if (outLogs) outLogs->push_back(cleanMsg);
                    continue;
                }
                entries.push_back({fname, fullPath});
            }
        }
    }
    closedir(dir);

    // Sort descending by filename (starts with timestamp: MPC.settings.bak-YYYYMMDDHHMMSS)
    std::sort(entries.begin(), entries.end(), [](const BakEntry& a, const BakEntry& b) {
        return a.filename > b.filename;
    });

    // Retain at most maxKeep integral backups, delete older ones
    if (entries.size() > maxKeep) {
        for (size_t i = maxKeep; i < entries.size(); ++i) {
            unlink(entries[i].fullPath.c_str());
            std::string purgeMsg = "[TKGL-CFG] Purged old backup (retention max " + std::to_string(maxKeep) + "): " + entries[i].filename;
            ConfigParser::logMessage(purgeMsg);
            if (outLogs) outLogs->push_back(purgeMsg);
        }
    }
    sync();
}

// Helper: Copy a file using POSIX open/read/write/fsync with UBIFS pre-truncation sync
static bool posixCopyFileWithSync(const std::string& srcPath, const std::string& dstPath, size_t expectedSize, std::string& outErr) {
    int fdIn = open(srcPath.c_str(), O_RDONLY);
    if (fdIn < 0) {
        outErr = "open(" + srcPath + ") failed: " + std::string(strerror(errno));
        return false;
    }

    // Pre-truncate destination and sync so UBIFS (/media/az01-internal) commits the truncation
    // and reclaims dirty flash LEBs BEFORE allocating blocks for the new write.
    {
        int fdTrunc = open(dstPath.c_str(), O_WRONLY | O_TRUNC);
        if (fdTrunc >= 0) {
            fsync(fdTrunc);
            close(fdTrunc);
        }
        sync();
    }

    int fdOut = open(dstPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fdOut < 0) {
        outErr = "open(" + dstPath + ") for write failed: " + std::string(strerror(errno));
        close(fdIn);
        return false;
    }

    char buf[16384];
    size_t totalWritten = 0;
    while (true) {
        ssize_t r = read(fdIn, buf, sizeof(buf));
        if (r < 0) {
            if (errno == EINTR) continue;
            outErr = "read(" + srcPath + ") failed: " + std::string(strerror(errno));
            close(fdIn);
            close(fdOut);
            return false;
        }
        if (r == 0) break;

        ssize_t offset = 0;
        while (offset < r) {
            ssize_t w = write(fdOut, buf + offset, static_cast<size_t>(r - offset));
            if (w < 0) {
                if (errno == EINTR) continue;
                int savedErrno = errno;
                outErr = "write(" + dstPath + ") failed at byte " + std::to_string(totalWritten) +
                         "/" + std::to_string(expectedSize) + ": " + std::string(strerror(savedErrno));
                close(fdIn);
                close(fdOut);
                errno = savedErrno;
                return false;
            }
            offset += w;
            totalWritten += static_cast<size_t>(w);
        }
    }

    fsync(fdOut);
    close(fdOut);
    close(fdIn);
    sync();

    if (expectedSize > 0 && totalWritten != expectedSize) {
        outErr = "size mismatch (" + std::to_string(totalWritten) + " vs " + std::to_string(expectedSize) + " bytes)";
        return false;
    }
    return true;
}

// Helper: Find latest valid backup content in $PLG_MAN_DIR/backups if active MPC.settings was interrupted/truncated
static std::string loadLatestValidBackupXml(std::string* outRecoveredFrom = nullptr) {
    std::string dirPath = resolveBackupDirectory();
    DIR* dir = opendir(dirPath.c_str());
    if (!dir) return "";

    std::vector<std::string> names;
    const std::string prefix = "MPC.settings.bak-";
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        std::string fname = ent->d_name;
        if (fname.rfind(prefix, 0) == 0) {
            names.push_back(fname);
        }
    }
    closedir(dir);
    std::sort(names.begin(), names.end(), std::greater<std::string>());

    for (const auto& fname : names) {
        std::string fullPath = dirPath + "/" + fname;
        std::ifstream in(fullPath, std::ios::binary);
        if (!in.is_open()) continue;
        std::stringstream ss;
        ss << in.rdbuf();
        in.close();
        std::string content = ss.str();
        std::string errReason;
        if (validateMpcSettingsXml(content, errReason)) {
            if (outRecoveredFrom) *outRecoveredFrom = fullPath;
            return content;
        }
    }
    return "";
}

static std::string loadSessionMpcSettingsXml(bool* outIsStaged = nullptr) {
    if (outIsStaged) *outIsStaged = false;
    struct stat stStaged;
    if (stat(SESSION_STAGED_MPC_SETTINGS, &stStaged) == 0 && S_ISREG(stStaged.st_mode) && stStaged.st_size > 32) {
        std::ifstream inStaged(SESSION_STAGED_MPC_SETTINGS, std::ios::binary);
        if (inStaged.is_open()) {
            std::stringstream ss;
            ss << inStaged.rdbuf();
            inStaged.close();
            std::string stagedStr = ss.str();
            std::string valErr;
            if (!stagedStr.empty() && validateMpcSettingsXml(stagedStr, valErr)) {
                if (outIsStaged) *outIsStaged = true;
                return stagedStr;
            }
        }
    }

    std::string mpcSettingsPath = resolveMpcSettingsPath();
    std::string activeStr;
    {
        std::ifstream inActive(mpcSettingsPath, std::ios::binary);
        if (inActive.is_open()) {
            std::stringstream ss;
            ss << inActive.rdbuf();
            inActive.close();
            activeStr = ss.str();
        }
    }

    std::string valErr;
    if (!activeStr.empty() && validateMpcSettingsXml(activeStr, valErr)) {
        // Also populate /tmp/MPC.settings.new working copy so all session tools & Sanity Checks operate on /tmp staging
        std::ofstream outTmp(SESSION_STAGED_MPC_SETTINGS, std::ios::binary | std::ios::trunc);
        if (outTmp.is_open()) {
            outTmp.write(activeStr.data(), static_cast<std::streamsize>(activeStr.size()));
            outTmp.flush();
            outTmp.close();
            if (outIsStaged) *outIsStaged = true;
        }
        return activeStr;
    }

    // Active MPC.settings is missing or truncated/corrupted -> auto-recover from latest valid backup in $PLG_MAN_DIR/backups
    std::string recPath;
    std::string recXml = loadLatestValidBackupXml(&recPath);
    if (!recXml.empty()) {
        std::string recMsg = "[WARNING] Active MPC.settings was corrupted/truncated (" + valErr +
                             "). Auto-recovered session staging /tmp/MPC.settings.new from backup: " + recPath;
        ConfigParser::logMessage(recMsg);
        std::ofstream outTmp(SESSION_STAGED_MPC_SETTINGS, std::ios::binary | std::ios::trunc);
        if (outTmp.is_open()) {
            outTmp.write(recXml.data(), static_cast<std::streamsize>(recXml.size()));
            outTmp.flush();
            outTmp.close();
            s_mpcSettingsDirty = true;
            if (outIsStaged) *outIsStaged = true;
        }
        return recXml;
    }

    return activeStr;
}

static bool stageSessionMpcSettingsXml(const std::string& newContent, std::vector<std::string>* outLogs = nullptr) {
    std::string errReason;
    if (!validateMpcSettingsXml(newContent, errReason)) {
        std::string errMsg = "[ERROR] MPC.settings integrity check failed (" + errReason + "). Aborting staging to preserve MPC.settings!";
        std::fprintf(stderr, "%s\n", errMsg.c_str());
        ConfigParser::logMessage(errMsg);
        if (outLogs) outLogs->push_back(errMsg);
        return false;
    }

    std::ofstream out(SESSION_STAGED_MPC_SETTINGS, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::string errMsg = std::string("[ERROR] Could not open session staging file: ") + SESSION_STAGED_MPC_SETTINGS;
        ConfigParser::logMessage(errMsg);
        if (outLogs) outLogs->push_back(errMsg);
        return false;
    }
    out.write(newContent.data(), static_cast<std::streamsize>(newContent.size()));
    out.flush();
    if (out.fail()) {
        out.close();
        unlink(SESSION_STAGED_MPC_SETTINGS);
        std::string errMsg = std::string("[ERROR] Failed to write XML to ") + SESSION_STAGED_MPC_SETTINGS;
        ConfigParser::logMessage(errMsg);
        if (outLogs) outLogs->push_back(errMsg);
        return false;
    }
    out.close();

    struct stat stStaged;
    if (stat(SESSION_STAGED_MPC_SETTINGS, &stStaged) != 0 || static_cast<size_t>(stStaged.st_size) != newContent.size()) {
        unlink(SESSION_STAGED_MPC_SETTINGS);
        std::string errMsg = "[ERROR] Staged XML size mismatch in /tmp/MPC.settings.new.";
        ConfigParser::logMessage(errMsg);
        if (outLogs) outLogs->push_back(errMsg);
        return false;
    }

    s_mpcSettingsDirty = true;
    std::string msg = "[TKGL-CFG] Changes staged in session file: /tmp/MPC.settings.new (will be committed on exit)";
    ConfigParser::logMessage(msg);
    if (outLogs) outLogs->push_back(msg);
    return true;
}

static bool backupAndWriteMpcSettings(
    const std::string& mpcSettingsPath,
    const std::string& newContent,
    std::vector<std::string>* outLogs,
    std::string* outBackupPath = nullptr
) {
    std::string errReason;
    if (!validateMpcSettingsXml(newContent, errReason)) {
        std::string errMsg = "[ERROR] MPC.settings integrity check failed (" + errReason + "). Aborting write to preserve MPC.settings!";
        std::fprintf(stderr, "%s\n", errMsg.c_str());
        ConfigParser::logMessage(errMsg);
        if (outLogs) outLogs->push_back(errMsg);
        return false;
    }

    // 1. Gold Rule: ONLY backup active MPC.settings if it is present, complete, and passes full XML integrity validation.
    //    Never backup a truncated/corrupted MPC.settings, and never purge a healthy slot unless a valid backup is created.
    std::string ts = getBackupTimestampYYYYMMDDHHMMSS();
    std::string bakDir = resolveBackupDirectory();
    std::string bakPath = bakDir + "/MPC.settings.bak-" + ts;
    {
        std::ifstream src(mpcSettingsPath, std::ios::binary);
        if (src.is_open()) {
            std::stringstream curSs;
            curSs << src.rdbuf();
            src.close();
            std::string curXml = curSs.str();
            std::string curValErr;
            if (!curXml.empty() && validateMpcSettingsXml(curXml, curValErr)) {
                // Retain max 9 valid backups in Manager backups folder before writing the 10th valid backup
                enforceMpcSettingsBackupRetention(9, outLogs);

                std::ofstream bak(bakPath, std::ios::binary | std::ios::trunc);
                if (bak.is_open()) {
                    bak.write(curXml.data(), static_cast<std::streamsize>(curXml.size()));
                    bak.flush();
                    bool bakWriteOk = !bak.fail();
                    bak.close();

                    struct stat stBak;
                    if (bakWriteOk && stat(bakPath.c_str(), &stBak) == 0 && static_cast<size_t>(stBak.st_size) == curXml.size()) {
                        if (outBackupPath) *outBackupPath = bakPath;
                        std::string bakMsg = "[TKGL-CFG] Safety backup created (verified integral XML): " + bakPath;
                        ConfigParser::logMessage(bakMsg);
                        if (outLogs) outLogs->push_back(bakMsg);
                    } else {
                        unlink(bakPath.c_str());
                        std::string warnMsg = "[WARNING] Incomplete write when creating backup file (removed): " + bakPath;
                        ConfigParser::logMessage(warnMsg);
                        if (outLogs) outLogs->push_back(warnMsg);
                    }
                } else {
                    std::string warnMsg = "[WARNING] Could not create backup file in: " + bakPath;
                    ConfigParser::logMessage(warnMsg);
                    if (outLogs) outLogs->push_back(warnMsg);
                }
            } else {
                // Keep all 10 existing valid backups untouched
                enforceMpcSettingsBackupRetention(10, outLogs);
                std::string warnMsg = "[WARNING] Active MPC.settings is not integral (" + curValErr + "); skipping backup to protect healthy backups.";
                ConfigParser::logMessage(warnMsg);
                if (outLogs) outLogs->push_back(warnMsg);
            }
        } else {
            enforceMpcSettingsBackupRetention(10, outLogs);
        }
    }

    // 2. Ensure staging file /tmp/MPC.settings.new holds the exact validated content
    std::string tmpStagedPath = SESSION_STAGED_MPC_SETTINGS;
    bool needWriteTmp = true;
    struct stat stExistingTmp;
    if (stat(tmpStagedPath.c_str(), &stExistingTmp) == 0 &&
        S_ISREG(stExistingTmp.st_mode) &&
        static_cast<size_t>(stExistingTmp.st_size) == newContent.size()) {
        std::ifstream checkTmp(tmpStagedPath, std::ios::binary);
        if (checkTmp.is_open()) {
            std::stringstream css;
            css << checkTmp.rdbuf();
            checkTmp.close();
            if (css.str() == newContent) {
                needWriteTmp = false;
            }
        }
    }

    if (needWriteTmp) {
        std::ofstream out(tmpStagedPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            std::string errMsg = "[ERROR] Could not open staging file in /tmp: " + tmpStagedPath;
            ConfigParser::logMessage(errMsg);
            if (outLogs) outLogs->push_back(errMsg);
            return false;
        }
        out.write(newContent.data(), static_cast<std::streamsize>(newContent.size()));
        out.flush();
        if (out.fail()) {
            out.close();
            std::string errMsg = "[ERROR] Failed to write XML to staging file " + tmpStagedPath;
            ConfigParser::logMessage(errMsg);
            if (outLogs) outLogs->push_back(errMsg);
            return false;
        }
        out.close();
    }

    // Verify /tmp staged file matches exact buffer length
    struct stat stTmp;
    if (stat(tmpStagedPath.c_str(), &stTmp) != 0 || static_cast<size_t>(stTmp.st_size) != newContent.size()) {
        std::string errMsg = "[ERROR] Staged file size mismatch in /tmp/MPC.settings.new.";
        ConfigParser::logMessage(errMsg);
        if (outLogs) outLogs->push_back(errMsg);
        return false;
    }

    // 3. Overwrite active MPC.settings by copying from /tmp/MPC.settings.new (with UBIFS pre-truncation sync)
    std::string copyErr;
    if (!posixCopyFileWithSync(tmpStagedPath, mpcSettingsPath, newContent.size(), copyErr)) {
        std::string errMsg = "[ERROR] Failed to copy staged " + tmpStagedPath + " into active " + mpcSettingsPath + " (" + copyErr + ")";
        ConfigParser::logMessage(errMsg);
        if (outLogs) outLogs->push_back(errMsg);
        return false;
    }

    // 4. Verify destination file exists and matches exact buffer length
    struct stat stActive;
    if (stat(mpcSettingsPath.c_str(), &stActive) != 0 || static_cast<size_t>(stActive.st_size) != newContent.size()) {
        std::string errMsg = "[ERROR] Size mismatch after copying /tmp/MPC.settings.new to active " + mpcSettingsPath;
        ConfigParser::logMessage(errMsg);
        if (outLogs) outLogs->push_back(errMsg);
        return false;
    }

    sync();

    std::string okMsg = "[TKGL-CFG] Successfully updated active MPC.settings from staged /tmp/MPC.settings.new";
    ConfigParser::logMessage(okMsg);
    if (outLogs) outLogs->push_back(okMsg);

    return true;
}

void ConfigParser::initSession() {
    unlink(SESSION_STAGED_MPC_SETTINGS);
    s_mpcSettingsDirty = false;
    enforceMpcSettingsBackupRetention(10);

    // Initialize /tmp/MPC.settings.new working copy immediately so all session tools & Sanity Checks operate on /tmp staging
    bool isStaged = false;
    loadSessionMpcSettingsXml(&isStaged);
}

bool ConfigParser::hasStagedMpcSettings() {
    return s_mpcSettingsDirty;
}

void ConfigParser::enforceBackupRetention(size_t maxKeep, std::vector<std::string>* outLogs) {
    enforceMpcSettingsBackupRetention(maxKeep, outLogs);
}

std::string ConfigParser::resolveBackupDirectory() {
    return ::tkgl::config::resolveBackupDirectory();
}

bool ConfigParser::commitSessionMpcSettings(std::vector<std::string>* outLogs) {
    struct stat stStaged;
    if (!s_mpcSettingsDirty || stat(SESSION_STAGED_MPC_SETTINGS, &stStaged) != 0 || stStaged.st_size < 32) {
        std::string msg = "[TKGL-EXIT] No MPC.settings changes staged during this session. Active MPC.settings untouched.";
        ConfigParser::logMessage(msg);
        if (outLogs) outLogs->push_back(msg);
        unlink(SESSION_STAGED_MPC_SETTINGS);
        s_mpcSettingsDirty = false;
        return true;
    }

    std::ifstream inStaged(SESSION_STAGED_MPC_SETTINGS, std::ios::binary);
    if (!inStaged.is_open()) {
        std::string errMsg = "[ERROR] Could not read staged file /tmp/MPC.settings.new for commit.";
        ConfigParser::logMessage(errMsg);
        if (outLogs) outLogs->push_back(errMsg);
        return false;
    }
    std::stringstream ss;
    ss << inStaged.rdbuf();
    inStaged.close();
    std::string stagedContent = ss.str();

    std::string mpcSettingsPath = resolveMpcSettingsPath();
    std::string bakPath;
    if (!backupAndWriteMpcSettings(mpcSettingsPath, stagedContent, outLogs, &bakPath)) {
        std::string errMsg = "[ERROR] Failed to commit session MPC.settings changes upon exit! Staged file preserved in /tmp/MPC.settings.new";
        ConfigParser::logMessage(errMsg);
        if (outLogs) outLogs->push_back(errMsg);
        return false;
    }

    unlink(SESSION_STAGED_MPC_SETTINGS);
    s_mpcSettingsDirty = false;
    std::string succMsg = "[TKGL-EXIT] SUCCESS: Staged /tmp/MPC.settings.new committed to active MPC.settings (backup: " + bakPath + ")";
    ConfigParser::logMessage(succMsg);
    if (outLogs) outLogs->push_back(succMsg);
    return true;
}

// Safely ensures that targetSynthDir is present inside <VALUE name="SynthContentLocations"> in an in-memory XML string.
// Helper: Removes a span [elemStart, elemEnd) from an XML string along with its line indentation and trailing newline
static void eraseXmlElementLineCleanly(std::string& xml, size_t elemStart, size_t elemEnd) {
    size_t lineStart = elemStart;
    while (lineStart > 0 && (xml[lineStart - 1] == ' ' || xml[lineStart - 1] == '\t')) {
        lineStart--;
    }
    size_t lineEnd = elemEnd;
    while (lineEnd < xml.size() && (xml[lineEnd] == ' ' || xml[lineEnd] == '\t')) {
        lineEnd++;
    }
    if (lineEnd < xml.size() && xml[lineEnd] == '\r') lineEnd++;
    if (lineEnd < xml.size() && xml[lineEnd] == '\n') lineEnd++;
    xml.erase(lineStart, lineEnd - lineStart);
}

// Returns true if the XML string was modified.
// NOTE: This function NEVER deletes or purges any pre-existing <Location> elements, preserving
// any manual edits or custom paths added by the user. It ONLY verifies and appends targetSynthDir
// if it is located on an external mounted media drive (/media/...).
static bool ensureSynthLocationInXmlString(std::string& content, const std::string& targetSynthDir) {
    if (targetSynthDir.empty() || targetSynthDir[0] != '/') return false;
    std::string cleanDir = targetSynthDir;
    while (cleanDir.size() > 1 && cleanDir.back() == '/') cleanDir.pop_back();

    // RULE: Only add to <SynthContentLocations> if the path is on an external mounted drive (/media/...).
    // Never automatically register paths from /root, /tmp, or internal root filesystem.
    // If not on /media/, do not modify anything and return false.
    if (cleanDir.rfind("/media/", 0) != 0) {
        return false;
    }

    bool modified = false;

    std::string escapedDir = xmlEscapeText(cleanDir);
    std::string expectedTag = "<Location>" + escapedDir + "</Location>";
    std::string rawExpectedTag = "<Location>" + cleanDir + "</Location>";

    size_t valStart = 0, valOpenEnd = 0, valEnd = 0;
    bool valSelfClosing = false;
    if (findValueBlock(content, "SynthContentLocations", valStart, valOpenEnd, valEnd, valSelfClosing)) {
        if (valSelfClosing) {
            std::string replacement =
                std::string("<VALUE name=\"SynthContentLocations\">\n") +
                "    <SynthContentLocations>\n" +
                "      " + expectedTag + "\n" +
                "    </SynthContentLocations>\n" +
                "  </VALUE>";
            content.replace(valStart, valEnd - valStart, replacement);
            modified = true;
        } else {
            std::string block = content.substr(valStart, valEnd - valStart);
            if (block.find(expectedTag) == std::string::npos && block.find(rawExpectedTag) == std::string::npos) {
                size_t closeLocs = block.find("</SynthContentLocations>");
                if (closeLocs != std::string::npos) {
                    size_t absCloseLocs = valStart + closeLocs;
                    size_t lineStart = absCloseLocs;
                    while (lineStart > valStart && (content[lineStart - 1] == ' ' || content[lineStart - 1] == '\t')) {
                        lineStart--;
                    }
                    std::string indent = content.substr(lineStart, absCloseLocs - lineStart);
                    std::string inject = "  " + expectedTag + "\n" + indent;
                    content.insert(absCloseLocs, inject);
                    modified = true;
                } else {
                    // Check for self-closing <SynthContentLocations/>
                    size_t innerPos = block.find("<SynthContentLocations");
                    if (innerPos != std::string::npos) {
                        size_t absInner = valStart + innerPos;
                        bool innerSelf = false;
                        size_t innerEnd = findXmlTagEnd(content, absInner, innerSelf);
                        if (innerEnd != std::string::npos && innerSelf) {
                            std::string replacement =
                                std::string("<SynthContentLocations>\n") +
                                "      " + expectedTag + "\n" +
                                "    </SynthContentLocations>";
                            content.replace(absInner, innerEnd - absInner, replacement);
                            modified = true;
                        }
                    } else {
                        size_t closeValPos = valEnd - 8; // before </VALUE>
                        std::string inject =
                            std::string("  <SynthContentLocations>\n") +
                            "      " + expectedTag + "\n" +
                            "    </SynthContentLocations>\n  ";
                        content.insert(closeValPos, inject);
                        modified = true;
                    }
                }
            }
        }
    } else {
        size_t propEnd = content.rfind("</PROPERTIES>");
        if (propEnd == std::string::npos) propEnd = content.rfind("</Properties>");
        if (propEnd == std::string::npos) propEnd = content.rfind("</properties>");
        if (propEnd != std::string::npos) {
            std::string block =
                std::string("  <VALUE name=\"SynthContentLocations\">\n") +
                "    <SynthContentLocations>\n" +
                "      " + expectedTag + "\n" +
                "    </SynthContentLocations>\n" +
                "  </VALUE>\n";
            content.insert(propEnd, block);
            modified = true;
        }
    }
    return modified;
}

void ConfigParser::ensureManagerSynthsInMpcSettings(std::vector<std::string>* outLogs) {
    std::string mgrDir = getManagerRootDir();
    while (mgrDir.size() > 1 && mgrDir.back() == '/') mgrDir.pop_back();

    std::string content = loadSessionMpcSettingsXml();
    if (content.empty()) return;

    std::string valErr;
    if (!validateMpcSettingsXml(content, valErr)) {
        std::string warnMsg = "[WARNING] MPC.settings has corrupted XML structure (" + valErr + "). Skipping VST2 Plugins auto-registration. Please restore a healthy backup.";
        ConfigParser::logMessage(warnMsg);
        if (outLogs) outLogs->push_back(warnMsg);
        return;
    }

    std::vector<std::string> dirsToEnsure;

    // 1. Manager's own "VST2 Plugins" directory when running from an external/mounted volume (/media/...)
    if (mgrDir.rfind("/media/", 0) == 0 && mgrDir != "/media/az01-internal" && mgrDir.rfind("/media/az01-internal/", 0) != 0) {
        std::string defaultMgrSynths = mgrDir + "/VST2 Plugins";
        mkdir(defaultMgrSynths.c_str(), 0755);
        dirsToEnsure.push_back(defaultMgrSynths);
    }

    // 2. Any existing "/media/<Drive>/VST2 Plugins" directory across mounted drives (USB key, SATA SSD 1, SATA SSD 2, SD card)
    DIR* mediaDir = opendir("/media");
    if (mediaDir) {
        struct dirent* ent;
        while ((ent = readdir(mediaDir)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            if (std::strcmp(ent->d_name, "az01-internal") == 0) continue;
            std::string driveVst2 = std::string("/media/") + ent->d_name + "/VST2 Plugins";
            struct stat st;
            if (stat(driveVst2.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && isSynthDirectoryWritable(driveVst2)) {
                if (std::find(dirsToEnsure.begin(), dirsToEnsure.end(), driveVst2) == dirsToEnsure.end()) {
                    dirsToEnsure.push_back(driveVst2);
                }
            }
        }
        closedir(mediaDir);
    }

    // 3. Any writable ".../VST2 Plugins" parent folder referenced by an existing <PLUGIN file="..."/> in KNOWNPLUGINS
    size_t armStart = 0, armOpenEnd = 0, armEnd = 0;
    bool armSelf = false;
    if (findValueBlock(content, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelf) && !armSelf) {
        auto regPlugins = extractPluginXmlEntries(content, armOpenEnd, armEnd);
        for (const auto& rp : regPlugins) {
            size_t pos = rp.file.find("/VST2 Plugins/");
            if (pos != std::string::npos) {
                std::string vstRoot = rp.file.substr(0, pos + 13); // ".../VST2 Plugins"
                struct stat st;
                if (stat(vstRoot.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && isSynthDirectoryWritable(vstRoot)) {
                    if (std::find(dirsToEnsure.begin(), dirsToEnsure.end(), vstRoot) == dirsToEnsure.end()) {
                        dirsToEnsure.push_back(vstRoot);
                    }
                }
            }
        }
    }

    bool anyAdded = false;
    std::vector<std::string> addedDirs;
    for (const auto& d : dirsToEnsure) {
        if (ensureSynthLocationInXmlString(content, d)) {
            anyAdded = true;
            addedDirs.push_back(d);
        }
    }

    if (anyAdded) {
        if (stageSessionMpcSettingsXml(content, outLogs)) {
            for (const auto& ad : addedDirs) {
                std::string msg = "[TKGL-INIT] Added <Location>" + ad + "</Location> to session staging (/tmp/MPC.settings.new)";
                std::printf("%s\n", msg.c_str());
                if (outLogs) outLogs->push_back(msg);
            }
        }
    }
}

bool ConfigParser::isItemBinaryCached(const MenuItem& item, std::string* outSizeStr, std::string* outZipPath) {
    if (item.id.empty()) return false;
    std::string mgrDir = getManagerRootDir();
    std::vector<std::string> cands = {
        mgrDir + "/plugins/" + item.id + "/" + item.id + ".zip",
        mgrDir + "/plugins/" + item.id + "/package.zip",
        "plugins/" + item.id + "/" + item.id + ".zip",
        "plugins/" + item.id + "/package.zip"
    };
    for (const auto& cp : cands) {
        struct stat st;
        if (stat(cp.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 1024) {
            if (outZipPath) *outZipPath = cp;
            if (outSizeStr) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.1f MB", static_cast<double>(st.st_size) / (1024.0 * 1024.0));
                *outSizeStr = buf;
            }
            return true;
        }
    }
    return false;
}

bool ConfigParser::cacheItemBinary(const MenuItem& item, std::vector<std::string>& outLogs, std::function<void(const std::string&)> onProgress) {
    if (item.downloadUrl.empty()) {
        outLogs.push_back("[ERROR] " + item.label + ": No download URL configured.");
        return false;
    }
    std::string mgrDir = getManagerRootDir();
    mkdir("plugins", 0755);
    mkdir((mgrDir + "/plugins").c_str(), 0755);
    std::string pluginDir = mgrDir + "/plugins/" + item.id;
    mkdir(pluginDir.c_str(), 0755);
    std::string destZip = pluginDir + "/" + item.id + ".zip";

    outLogs.push_back("[TKGL-CACHE] Downloading " + item.label + " to local cache: " + destZip);
    if (onProgress) onProgress("[CACHE] Downloading " + item.label + " via libcurl (IPv4)...");

    bool ok = downloadFile(item.downloadUrl, destZip, 180, onProgress, "[CACHE] " + item.label);
    if (ok) {
        struct stat st;
        if (stat(destZip.c_str(), &st) == 0 && st.st_size > 1024) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "[TKGL-CACHE] SUCCESS: Cached %s (%.2f MB) for offline installation.", item.label.c_str(), static_cast<double>(st.st_size) / 1048576.0);
            outLogs.push_back(buf);
            return true;
        }
    }
    unlink(destZip.c_str());
    outLogs.push_back("[ERROR] Failed to download binary for " + item.label);
    return false;
}

bool ConfigParser::deleteItemBinary(const MenuItem& item, std::vector<std::string>& outLogs) {
    if (item.id.empty()) return false;
    std::string mgrDir = getManagerRootDir();
    std::vector<std::string> cands = {
        mgrDir + "/plugins/" + item.id + "/" + item.id + ".zip",
        mgrDir + "/plugins/" + item.id + "/package.zip",
        "plugins/" + item.id + "/" + item.id + ".zip",
        "plugins/" + item.id + "/package.zip"
    };
    bool removed = false;
    for (const auto& cp : cands) {
        if (unlink(cp.c_str()) == 0) removed = true;
    }
    if (removed) {
        outLogs.push_back("[TKGL-CACHE] Deleted local binary archive for " + item.label);
    } else {
        outLogs.push_back("[TKGL-CACHE] No cached binary archive found to delete for " + item.label);
    }
    return removed;
}

int ConfigParser::cacheAllCatalogBinaries(LauncherConfig& config, std::vector<std::string>& outLogs, std::function<void(const std::string&)> onProgress) {
    int total = 0, alreadyCached = 0, newlyCached = 0, failed = 0;
    for (const auto& sec : config.sections) total += static_cast<int>(sec.items.size());

    outLogs.push_back("[TKGL-CACHE] === Synchronizing all catalog binaries for Offline Use ===");
    int idx = 0;
    for (auto& sec : config.sections) {
        for (auto& it : sec.items) {
            idx++;
            if (it.downloadUrl.empty()) continue;
            std::string zPath;
            if (isItemBinaryCached(it, &it.cachedZipSize, &zPath)) {
                it.isCached = true;
                alreadyCached++;
                outLogs.push_back("[TKGL-CACHE] [" + std::to_string(idx) + "/" + std::to_string(total) + "] " + it.label + " already cached (" + it.cachedZipSize + ")");
                continue;
            }
            if (onProgress) onProgress("[CACHE " + std::to_string(idx) + "/" + std::to_string(total) + "] " + it.label);
            if (cacheItemBinary(it, outLogs, onProgress)) {
                it.isCached = true;
                newlyCached++;
            } else {
                failed++;
            }
        }
    }
    char sumBuf[160];
    std::snprintf(sumBuf, sizeof(sumBuf), "[TKGL-CACHE] Complete: %d in local cache (%d already present, %d downloaded, %d failed).", alreadyCached + newlyCached, alreadyCached, newlyCached, failed);
    outLogs.push_back(sumBuf);
    return alreadyCached + newlyCached;
}

std::string ConfigParser::resolveMpcSettingsPath() {
    return ::tkgl::config::resolveMpcSettingsPath();
}

std::vector<MpcSettingsBackup> ConfigParser::listMpcSettingsBackups() {
    std::vector<MpcSettingsBackup> backups;
    enforceMpcSettingsBackupRetention(10);
    std::string dirPath = resolveBackupDirectory();

    DIR* dir = opendir(dirPath.c_str());
    if (!dir) return backups;

    struct dirent* ent = nullptr;
    const std::string prefix = "MPC.settings.bak-";
    while ((ent = readdir(dir)) != nullptr) {
        std::string fname = ent->d_name;
        if (fname.rfind(prefix, 0) == 0) {
            std::string fullPath = dirPath + "/" + fname;
            struct stat st;
            if (stat(fullPath.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                MpcSettingsBackup b;
                b.filename = fname;
                b.fullPath = fullPath;
                b.fileSize = static_cast<size_t>(st.st_size);

                std::string rawTs = fname.substr(prefix.length());
                if (rawTs.length() >= 14) {
                    b.timestampFormatted = rawTs.substr(0, 4) + "-" +
                                           rawTs.substr(4, 2) + "-" +
                                           rawTs.substr(6, 2) + " " +
                                           rawTs.substr(8, 2) + ":" +
                                           rawTs.substr(10, 2) + ":" +
                                           rawTs.substr(12, 2);
                } else {
                    b.timestampFormatted = rawTs;
                }
                // Count <PLUGIN> tags inside this backup file
                int pCount = 0;
                std::ifstream bakFile(fullPath, std::ios::binary);
                if (bakFile.is_open()) {
                    std::string line;
                    while (std::getline(bakFile, line)) {
                        size_t ppos = 0;
                        while ((ppos = line.find("<PLUGIN ", ppos)) != std::string::npos) {
                            pCount++;
                            ppos += 8;
                        }
                    }
                    bakFile.close();
                }
                b.pluginCount = pCount;
                backups.push_back(b);
            }
        }
    }
    closedir(dir);

    std::sort(backups.begin(), backups.end(), [](const MpcSettingsBackup& a, const MpcSettingsBackup& b) {
        return a.filename > b.filename;
    });

    return backups;
}

bool ConfigParser::restoreMpcSettingsBackup(const std::string& backupPath, std::vector<std::string>& outLogs) {
    if (backupPath.empty()) {
        std::string errMsg = "[ERROR] No backup path provided for restoration.";
        logMessage(errMsg);
        outLogs.push_back(errMsg);
        return false;
    }

    struct stat st;
    if (stat(backupPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 32) {
        std::string errMsg = "[ERROR] Backup file does not exist or is too small: " + backupPath;
        logMessage(errMsg);
        outLogs.push_back(errMsg);
        return false;
    }

    std::string mpcSettingsPath = resolveMpcSettingsPath();

    // 1. Read & validate the backup file's XML integrity before restoring
    std::string bakContent;
    {
        std::ifstream srcBak(backupPath, std::ios::binary);
        if (!srcBak.is_open()) {
            std::string errMsg = "[ERROR] Could not open backup file for reading: " + backupPath;
            logMessage(errMsg);
            outLogs.push_back(errMsg);
            return false;
        }
        std::stringstream bss;
        bss << srcBak.rdbuf();
        srcBak.close();
        bakContent = bss.str();
        std::string valErr;
        if (!validateMpcSettingsXml(bakContent, valErr)) {
            std::string errMsg = "[ERROR] Backup file has corrupted XML (" + valErr + "). Restore aborted.";
            logMessage(errMsg);
            outLogs.push_back(errMsg);
            return false;
        }
    }

    // 2. Restore validated backup XML to active MPC.settings via backupAndWriteMpcSettings
    std::string resBakPath;
    bool ok = backupAndWriteMpcSettings(mpcSettingsPath, bakContent, &outLogs, &resBakPath);
    if (ok) {
        // Keep session staging file /tmp/MPC.settings.new synchronized with the restored backup
        std::ofstream outStaged(SESSION_STAGED_MPC_SETTINGS, std::ios::binary | std::ios::trunc);
        if (outStaged.is_open()) {
            outStaged.write(bakContent.data(), static_cast<std::streamsize>(bakContent.size()));
            outStaged.flush();
            outStaged.close();
        }
        s_mpcSettingsDirty = false;
        std::string succMsg = "[TKGL-RESTORE] SUCCESS: MPC.settings restored & synchronized with /tmp/MPC.settings.new from " + backupPath;
        logMessage(succMsg);
        outLogs.push_back(succMsg);
    }
    return ok;
}

bool ConfigParser::deleteMpcSettingsBackup(const std::string& backupPath, std::vector<std::string>& outLogs) {
    if (backupPath.empty()) {
        std::string errMsg = "[ERROR] No backup path provided for deletion.";
        logMessage(errMsg);
        outLogs.push_back(errMsg);
        return false;
    }
    if (backupPath.find(".bak-") == std::string::npos) {
        std::string errMsg = "[ERROR] Refusing to delete non-backup file: " + backupPath;
        logMessage(errMsg);
        outLogs.push_back(errMsg);
        return false;
    }
    struct stat st;
    if (stat(backupPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        std::string errMsg = "[ERROR] Backup file does not exist: " + backupPath;
        logMessage(errMsg);
        outLogs.push_back(errMsg);
        return false;
    }
    if (unlink(backupPath.c_str()) != 0) {
        std::string errMsg = "[ERROR] Failed to delete backup file: " + backupPath;
        logMessage(errMsg);
        outLogs.push_back(errMsg);
        return false;
    }
    sync();
    std::string msg = "[TKGL-DELETE] Deleted backup file: " + backupPath;
    logMessage(msg);
    outLogs.push_back(msg);
    return true;
}

std::vector<std::string> ConfigParser::readSystemBootLogs(size_t maxLines) {
    std::vector<std::string> lines;
    auto appendFileTail = [&](const char* path, const char* tag, size_t limit) {
        std::ifstream f(path);
        if (!f.is_open()) return false;
        std::vector<std::string> fileLines;
        std::string line;
        while (std::getline(f, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty()) {
                fileLines.push_back(std::string("[") + tag + "] " + line);
            }
        }
        f.close();
        size_t startIdx = fileLines.size() > limit ? (fileLines.size() - limit) : 0;
        for (size_t i = startIdx; i < fileLines.size(); ++i) {
            lines.push_back(fileLines[i]);
        }
        return !fileLines.empty();
    };

    bool foundSys = appendFileTail("/var/log/messages", "SYSLOG", maxLines / 2);
    if (!foundSys) {
        appendFileTail("/var/log/syslog", "SYSLOG", maxLines / 2);
    }
    appendFileTail("/var/log/boot.log", "BOOT", 60);

    // Also capture kernel ring buffer / dmesg tail
    FILE* pipe = popen("dmesg | tail -n 80 2>/dev/null", "r");
    if (pipe) {
        char buf[512];
        while (std::fgets(buf, sizeof(buf), pipe) != nullptr) {
            std::string dl(buf);
            while (!dl.empty() && (dl.back() == '\r' || dl.back() == '\n')) dl.pop_back();
            if (!dl.empty()) {
                lines.push_back("[DMESG] " + dl);
            }
        }
        pclose(pipe);
    }

    // Also include tail of Plugin Manager log for complete control
    std::string plgLog = getManagerRootDir() + "/plgman_log.txt";
    appendFileTail(plgLog.c_str(), "PLGMAN", 40);

    if (lines.empty()) {
        lines.push_back("[INFO] No readable entries in /var/log/messages, /var/log/syslog, or dmesg.");
    }
    if (lines.size() > maxLines) {
        lines.erase(lines.begin(), lines.begin() + static_cast<std::ptrdiff_t>(lines.size() - maxLines));
    }
    return lines;
}

bool ConfigParser::enableSshAccess(bool permanent, std::string& outStatusMsg) {
    if (!permanent) {
        int rc = std::system("(dropbear -R 2>/dev/null || /usr/sbin/dropbear -R 2>/dev/null || /etc/init.d/dropbear start 2>/dev/null || /etc/init.d/S50dropbear start 2>/dev/null || /usr/sbin/sshd 2>/dev/null) &");
        (void)rc;
        logMessage("[TKGL-SSH] Activated temporary SSH daemon (Port 22) for current session.");
        outStatusMsg = "SSH enabled for current session (Port 22 active).";
        return true;
    } else {
        int rc = std::system("(dropbear -R 2>/dev/null || /usr/sbin/dropbear -R 2>/dev/null || /etc/init.d/dropbear start 2>/dev/null || /etc/init.d/S50dropbear start 2>/dev/null) &");
        (void)rc;
        std::string mgrDir = getManagerRootDir();
        std::string flagPath = mgrDir + "/.enable_ssh_permanent";
        FILE* fp = std::fopen(flagPath.c_str(), "w");
        if (fp) {
            std::fprintf(fp, "SSH_PERMANENT=1\n");
            std::fclose(fp);
        }
        int rc2 = std::system("(if [ -f /etc/init.d/dropbear ]; then chmod +x /etc/init.d/dropbear 2>/dev/null; ln -sf /etc/init.d/dropbear /etc/rc.d/S50dropbear 2>/dev/null; fi) &");
        (void)rc2;
        sync();
        logMessage("[TKGL-SSH] Activated SSH daemon & applied persistent startup hook.");
        outStatusMsg = "Permanent SSH enabled & system startup patched.";
        return true;
    }
}

std::vector<SanityCheckIssue> ConfigParser::runSanityChecks(const LauncherConfig& config, std::vector<std::string>* outLogs) {
    (void)config;
    std::vector<SanityCheckIssue> issues;

    std::string mpcSettingsContent = loadSessionMpcSettingsXml();
    std::vector<MpcPluginXmlEntry> regPlugins;
    if (!mpcSettingsContent.empty()) {
        size_t valStart = 0, valOpenEnd = 0, valEnd = 0;
        bool valSelfClosing = false;
        if (findValueBlock(mpcSettingsContent, "pluginList-arm", valStart, valOpenEnd, valEnd, valSelfClosing) && !valSelfClosing) {
            regPlugins = extractPluginXmlEntries(mpcSettingsContent, valOpenEnd, valEnd);
        }
    }

    // 1. Scan all writable (non-RO) SynthContentLocations for orphan synth folders
    //    (present on disk with a .so or plugin-meta.xml, but absent from MPC.settings KNOWNPLUGINS)
    auto synthLocs = getSynthContentLocations();
    for (const auto& loc : synthLocs) {
        if (loc.path.empty() || !isSynthDirectoryWritable(loc.path)) continue;
        DIR* d = opendir(loc.path.c_str());
        if (!d) continue;
        struct dirent* de;
        while ((de = readdir(d)) != nullptr) {
            if (de->d_name[0] == '.') continue;
            std::string subName = de->d_name;
            std::string fullSubDir = loc.path + "/" + subName;
            struct stat stDir;
            if (stat(fullSubDir.c_str(), &stDir) != 0 || !S_ISDIR(stDir.st_mode)) continue;

            // Check if this subdirectory contains any .so binary or plugin-meta.xml
            std::string foundSoInDir;
            DIR* subD = opendir(fullSubDir.c_str());
            if (subD) {
                struct dirent* sde;
                while ((sde = readdir(subD)) != nullptr) {
                    size_t len = std::strlen(sde->d_name);
                    if (len > 3 && std::strcmp(sde->d_name + len - 3, ".so") == 0) {
                        foundSoInDir = fullSubDir + "/" + sde->d_name;
                        break;
                    }
                }
                closedir(subD);
            }
            struct stat stMeta;
            bool hasMeta = (stat((fullSubDir + "/plugin-meta.xml").c_str(), &stMeta) == 0 && S_ISREG(stMeta.st_mode));
            if (foundSoInDir.empty() && !hasMeta) continue;

            // Check if registered in MPC.settings KNOWNPLUGINS
            bool isRegistered = false;
            for (const auto& rp : regPlugins) {
                if (!foundSoInDir.empty() && rp.file == foundSoInDir) {
                    isRegistered = true;
                    break;
                }
                if (!rp.file.empty() && rp.file.find(fullSubDir + "/") == 0) {
                    isRegistered = true;
                    break;
                }
            }

            if (!isRegistered) {
                SanityCheckIssue iss;
                iss.type       = SanityCheckIssue::ORPHAN_SYNTH_DIR;
                iss.pluginName = subName;
                iss.dirPath    = fullSubDir;
                iss.soPath     = foundSoInDir;
                iss.synthRoot  = loc.path;
                issues.push_back(iss);
            }
        }
        closedir(d);
    }

    // 2. Scan all <PLUGIN .../> entries in MPC.settings KNOWNPLUGINS for missing/inaccessible .so files
    for (const auto& rp : regPlugins) {
        struct stat stSo;
        bool soExists = (!rp.file.empty() && stat(rp.file.c_str(), &stSo) == 0 && S_ISREG(stSo.st_mode));
        if (!soExists) {
            SanityCheckIssue iss;
            iss.type       = SanityCheckIssue::MISSING_SO_PATH;
            iss.pluginName = !rp.name.empty() ? rp.name : (!rp.descName.empty() ? rp.descName : "Unknown VST");
            iss.soPath     = rp.file;
            size_t lastSlash = rp.file.find_last_of('/');
            iss.dirPath    = (lastSlash != std::string::npos) ? rp.file.substr(0, lastSlash) : "";
            issues.push_back(iss);
        }
    }

    std::string msg = "[TKGL-SANITY] Scan complete: " + std::to_string(issues.size()) + " issue(s) found.";
    logMessage(msg);
    if (outLogs) outLogs->push_back(msg);
    return issues;
}

bool ConfigParser::repairSanityIssue(LauncherConfig& config, const SanityCheckIssue& issue, std::string& outStatusMsg, std::vector<std::string>* outLogs) {
    if (issue.type == SanityCheckIssue::ORPHAN_SYNTH_DIR) {
        std::string settXml = loadSessionMpcSettingsXml();
        if (settXml.empty()) {
            outStatusMsg = "Cannot repair: MPC.settings not readable.";
            return false;
        }
        std::string valErr;
        if (!validateMpcSettingsXml(settXml, valErr)) {
            outStatusMsg = "MPC.settings XML error: " + valErr;
            return false;
        }
        if (!issue.synthRoot.empty()) {
            ensureSynthLocationInXmlString(settXml, issue.synthRoot);
        }

        std::string pluginDirName = issue.pluginName;
        size_t lastSlash = issue.dirPath.find_last_of('/');
        if (lastSlash != std::string::npos && lastSlash + 1 < issue.dirPath.size()) {
            pluginDirName = issue.dirPath.substr(lastSlash + 1);
        }

        std::vector<std::string> newPluginTags;
        std::string metaXmlPath = issue.dirPath + "/plugin-meta.xml";
        std::ifstream metaFile(metaXmlPath, std::ios::binary);
        if (metaFile.is_open()) {
            std::stringstream mss;
            mss << metaFile.rdbuf();
            std::string metaXml = mss.str();
            metaFile.close();
            auto metaEntries = extractPluginXmlEntries(metaXml, 0, metaXml.size());
            for (const auto& me : metaEntries) {
                std::string singleTag = normalizeSingleLineXmlTag(me.rawTag);
                std::string rawFileAttr = getXmlTagAttribute(singleTag, "file");
                std::string resolvedFile = issue.soPath;
                if (!rawFileAttr.empty()) {
                    const std::string token = "%payload-path%";
                    size_t tpos = rawFileAttr.find(token);
                    if (tpos != std::string::npos) {
                        std::string cand1 = rawFileAttr;
                        cand1.replace(tpos, token.size(), issue.synthRoot);
                        std::string cand2 = rawFileAttr;
                        cand2.replace(tpos, token.size(), issue.dirPath);
                        struct stat stSo;
                        if (stat(cand1.c_str(), &stSo) == 0) resolvedFile = cand1;
                        else if (stat(cand2.c_str(), &stSo) == 0) resolvedFile = cand2;
                    }
                }
                if (!resolvedFile.empty()) {
                    singleTag = setXmlTagAttribute(singleTag, "file", resolvedFile);
                }
                newPluginTags.push_back(singleTag);
            }
        }
        if (newPluginTags.empty() && !issue.soPath.empty()) {
            std::string genTag =
                "<PLUGIN name=\"" + xmlEscapeAttr(issue.pluginName) +
                "\" descriptiveName=\"" + xmlEscapeAttr(issue.pluginName) +
                "\" format=\"VST\" category=\"Synth\" manufacturer=\"TKGL\" version=\"1.0\" file=\"" +
                xmlEscapeAttr(issue.soPath) +
                "\" isInstrument=\"1\" fileTime=\"0\" infoUpdateTime=\"0\" numInputs=\"0\" numOutputs=\"2\" isShell=\"0\"/>";
            newPluginTags.push_back(genTag);
        }

        if (newPluginTags.empty()) {
            outStatusMsg = "No .so binary found in " + issue.dirPath;
            return false;
        }

        // Clean any stale/broken <PLUGIN> entries in pluginList-arm that match this orphan plugin's name, uid, or .so binary
        std::string orphanBinName;
        size_t soSlash = issue.soPath.find_last_of('/');
        if (soSlash != std::string::npos) orphanBinName = issue.soPath.substr(soSlash + 1);

        size_t armStart = 0, armOpenEnd = 0, armEnd = 0;
        bool armSelfClosing = false;
        if (!findValueBlock(settXml, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelfClosing)) {
            size_t propEnd = settXml.rfind("</PROPERTIES>");
            if (propEnd != std::string::npos) {
                settXml.insert(propEnd, "  <VALUE name=\"pluginList-arm\">\n    <KNOWNPLUGINS>\n    </KNOWNPLUGINS>\n  </VALUE>\n");
            }
        }
        while (findValueBlock(settXml, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelfClosing) && !armSelfClosing) {
            auto existingEntries = extractPluginXmlEntries(settXml, armOpenEnd, armEnd);
            bool removedStale = false;
            for (int idx = static_cast<int>(existingEntries.size()) - 1; idx >= 0; --idx) {
                const auto& ex = existingEntries[idx];
                struct stat stEx;
                bool exExists = (!ex.file.empty() && stat(ex.file.c_str(), &stEx) == 0 && S_ISREG(stEx.st_mode));
                if (exExists && ex.file != issue.soPath) continue;

                bool match = false;
                if (!orphanBinName.empty() && ex.file.find("/" + orphanBinName) != std::string::npos) match = true;
                for (const auto& nt : newPluginTags) {
                    std::string ntName = getXmlTagAttribute(nt, "name");
                    std::string ntUid  = getXmlTagAttribute(nt, "uid");
                    if ((!ntName.empty() && ex.name == ntName) || (!ntUid.empty() && ex.uid == ntUid)) {
                        match = true;
                        break;
                    }
                }
                if (match) {
                    eraseXmlElementLineCleanly(settXml, ex.startPos, ex.endPos);
                    removedStale = true;
                    break;
                }
            }
            if (!removedStale) break;
        }

        if (findValueBlock(settXml, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelfClosing) && !armSelfClosing) {
            std::string block = settXml.substr(armStart, armEnd - armStart);
            std::string allNewLines;
            for (const auto& nt : newPluginTags) {
                allNewLines += "      " + nt + "\n";
            }
            size_t closeKp = block.find("</KNOWNPLUGINS>");
            if (closeKp != std::string::npos) {
                size_t absCloseKp = armStart + closeKp;
                size_t lineStart = absCloseKp;
                while (lineStart > armStart && (settXml[lineStart - 1] == ' ' || settXml[lineStart - 1] == '\t')) lineStart--;
                settXml.insert(lineStart, allNewLines);
            }
        }

        if (!stageSessionMpcSettingsXml(settXml, outLogs)) {
            outStatusMsg = "Failed to stage update in /tmp/MPC.settings.new.";
            return false;
        }

        int xplCount = updatePluginPresetsXplFiles(issue.dirPath, issue.soPath, newPluginTags);
        refreshInstallStates(config, outLogs);
        outStatusMsg = "Staged orphan '" + pluginDirName + "' in /tmp/MPC.settings.new (" + std::to_string(xplCount) + " presets updated).";
        std::printf("[TKGL-SANITY] %s\n", outStatusMsg.c_str());
        logMessage("[TKGL-SANITY] " + outStatusMsg);
        return true;
    } else {
        // MISSING_SO_PATH: 1) search all writable SynthLocations for the .so or folder
        std::string soFileName;
        std::string parentFolderName;
        size_t lastSlash = issue.soPath.find_last_of('/');
        if (lastSlash != std::string::npos) {
            soFileName = issue.soPath.substr(lastSlash + 1);
            size_t prevSlash = issue.soPath.find_last_of('/', lastSlash > 0 ? lastSlash - 1 : 0);
            if (prevSlash != std::string::npos) {
                parentFolderName = issue.soPath.substr(prevSlash + 1, lastSlash - prevSlash - 1);
            }
        }

        auto synthLocs = getSynthContentLocations();
        std::string foundRelocatedSo;
        std::string foundRelocatedDir;
        std::string foundSynthRoot;
        for (const auto& loc : synthLocs) {
            if (loc.path.empty()) continue;
            if (!parentFolderName.empty() && !soFileName.empty()) {
                std::string cand = loc.path + "/" + parentFolderName + "/" + soFileName;
                struct stat st;
                if (stat(cand.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                    foundRelocatedSo = cand;
                    foundRelocatedDir = loc.path + "/" + parentFolderName;
                    foundSynthRoot = loc.path;
                    break;
                }
            }
            if (!soFileName.empty()) {
                DIR* d = opendir(loc.path.c_str());
                if (d) {
                    struct dirent* de;
                    while ((de = readdir(d)) != nullptr) {
                        if (de->d_name[0] == '.') continue;
                        std::string cand = loc.path + "/" + de->d_name + "/" + soFileName;
                        struct stat st;
                        if (stat(cand.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                            foundRelocatedSo = cand;
                            foundRelocatedDir = loc.path + "/" + de->d_name;
                            foundSynthRoot = loc.path;
                            break;
                        }
                    }
                    closedir(d);
                }
                if (!foundRelocatedSo.empty()) break;
            }
        }

        if (!foundRelocatedSo.empty()) {
            std::string settXml = loadSessionMpcSettingsXml();
            if (!settXml.empty()) {
                ensureSynthLocationInXmlString(settXml, foundSynthRoot);
                std::vector<std::string> relinkedTags;
                size_t armStart = 0, armOpenEnd = 0, armEnd = 0;
                bool armSelf = false;
                if (findValueBlock(settXml, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelf) && !armSelf) {
                    auto entries = extractPluginXmlEntries(settXml, armOpenEnd, armEnd);
                    for (int idx = static_cast<int>(entries.size()) - 1; idx >= 0; --idx) {
                        if (entries[idx].file == issue.soPath || (!issue.pluginName.empty() && entries[idx].name == issue.pluginName)) {
                            std::string updatedTag = setXmlTagAttribute(normalizeSingleLineXmlTag(entries[idx].rawTag), "file", foundRelocatedSo);
                            settXml.replace(entries[idx].startPos, entries[idx].endPos - entries[idx].startPos, updatedTag);
                            relinkedTags.push_back(updatedTag);
                            break;
                        }
                    }
                }
                if (!stageSessionMpcSettingsXml(settXml, outLogs)) {
                    outStatusMsg = "Failed to stage relinked path in /tmp/MPC.settings.new.";
                    return false;
                }
                int xplCnt = updatePluginPresetsXplFiles(foundRelocatedDir, foundRelocatedSo, relinkedTags);
                refreshInstallStates(config, outLogs);
                outStatusMsg = "Staged relink '" + issue.pluginName + "' in /tmp/MPC.settings.new (" + std::to_string(xplCnt) + " presets updated).";
                std::printf("[TKGL-SANITY] %s\n", outStatusMsg.c_str());
                logMessage("[TKGL-SANITY] " + outStatusMsg);
                return true;
            }
        }

        // Fallback: find matching item in catalog and reinstall it into primary SynthLocation (which stages in /tmp/MPC.settings.new)
        std::string targetLoc = !synthLocs.empty() ? synthLocs[0].path : (getManagerRootDir() + "/VST2 Plugins");
        for (const auto& sec : config.sections) {
            for (const auto& it : sec.items) {
                if (it.label == issue.pluginName || it.id == issue.pluginName ||
                    (!soFileName.empty() && it.targetBinary == soFileName)) {
                    std::vector<std::string> instLogs;
                    int rc = executeItemInstall(config, it, targetLoc, instLogs);
                    if (outLogs) {
                        for (const auto& l : instLogs) outLogs->push_back(l);
                    }
                    if (rc == 0) {
                        refreshInstallStates(config, outLogs);
                        outStatusMsg = "Reinstalled & staged '" + issue.pluginName + "' in /tmp/MPC.settings.new (" + targetLoc + ")";
                        std::printf("[TKGL-SANITY] %s\n", outStatusMsg.c_str());
                        return true;
                    }
                }
            }
        }

        outStatusMsg = "Could not repair '" + issue.pluginName + "': binary not found on drives or cache.";
        return false;
    }
}

bool ConfigParser::deleteSanityIssue(LauncherConfig& config, const SanityCheckIssue& issue, std::string& outStatusMsg, std::vector<std::string>* outLogs) {
    if (issue.type == SanityCheckIssue::ORPHAN_SYNTH_DIR) {
        if (issue.dirPath.empty() || issue.dirPath == "/" || issue.dirPath.size() < 8) {
            outStatusMsg = "Refusing to delete unsafe directory path.";
            return false;
        }
        std::string cmd = "rm -rf \"" + issue.dirPath + "\" && sync";
        int rc = std::system(cmd.c_str());
        (void)rc;
        refreshInstallStates(config, outLogs);
        outStatusMsg = "Deleted orphan synth directory: " + issue.dirPath;
        std::printf("[TKGL-SANITY] %s\n", outStatusMsg.c_str());
        logMessage("[TKGL-SANITY] " + outStatusMsg);
        if (outLogs) outLogs->push_back("[TKGL-SANITY] " + outStatusMsg);
        return true;
    } else {
        std::string settXml = loadSessionMpcSettingsXml();
        if (settXml.empty()) {
            outStatusMsg = "Cannot modify staged /tmp/MPC.settings.new (empty or missing).";
            return false;
        }
        size_t armStart = 0, armOpenEnd = 0, armEnd = 0;
        bool armSelf = false;
        bool removed = false;
        while (findValueBlock(settXml, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelf) && !armSelf) {
            auto entries = extractPluginXmlEntries(settXml, armOpenEnd, armEnd);
            bool erasedOne = false;
            for (int idx = static_cast<int>(entries.size()) - 1; idx >= 0; --idx) {
                if ((!issue.soPath.empty() && entries[idx].file == issue.soPath) ||
                    (issue.soPath.empty() && !issue.pluginName.empty() && entries[idx].name == issue.pluginName)) {
                    eraseXmlElementLineCleanly(settXml, entries[idx].startPos, entries[idx].endPos);
                    erasedOne = true;
                    removed = true;
                    break;
                }
            }
            if (!erasedOne) break;
        }
        if (removed && stageSessionMpcSettingsXml(settXml, outLogs)) {
            refreshInstallStates(config, outLogs);
            outStatusMsg = "Deleted broken reference '" + issue.pluginName + "' in staged /tmp/MPC.settings.new.";
            std::printf("[TKGL-SANITY] %s\n", outStatusMsg.c_str());
            logMessage("[TKGL-SANITY] " + outStatusMsg);
            return true;
        }
        outStatusMsg = "Reference '" + issue.pluginName + "' not found in staged /tmp/MPC.settings.new.";
        return false;
    }
}

int ConfigParser::applyPresetsPathsForAllInstalled(const LauncherConfig& config, int& outPluginsCount, std::string& outStatusMsg, std::vector<std::string>* outLogs) {
    (void)config;
    outPluginsCount = 0;
    int totalPresetsUpdated = 0;

    // Always read from session staged /tmp/MPC.settings.new so any repaired/relocated paths in this session are applied to .xpl presets
    std::string mpcSettingsContent = loadSessionMpcSettingsXml();
    std::vector<MpcPluginXmlEntry> regPlugins;
    if (!mpcSettingsContent.empty()) {
        size_t valStart = 0, valOpenEnd = 0, valEnd = 0;
        bool valSelfClosing = false;
        if (findValueBlock(mpcSettingsContent, "pluginList-arm", valStart, valOpenEnd, valEnd, valSelfClosing) && !valSelfClosing) {
            regPlugins = extractPluginXmlEntries(mpcSettingsContent, valOpenEnd, valEnd);
        }
    }

    std::vector<std::string> visitedDirs;
    for (const auto& rp : regPlugins) {
        if (rp.file.empty()) continue;
        struct stat stSo;
        if (stat(rp.file.c_str(), &stSo) != 0 || !S_ISREG(stSo.st_mode)) continue;
        size_t lastSlash = rp.file.find_last_of('/');
        if (lastSlash == std::string::npos) continue;
        std::string plugDir = rp.file.substr(0, lastSlash);
        if (std::find(visitedDirs.begin(), visitedDirs.end(), plugDir) != visitedDirs.end()) continue;
        visitedDirs.push_back(plugDir);

        int count = updatePluginPresetsXplFiles(plugDir, rp.file, {rp.rawTag});
        outPluginsCount++;
        totalPresetsUpdated += count;
    }

    sync();
    outStatusMsg = "Updated " + std::to_string(totalPresetsUpdated) + " preset (.xpl) file(s) across " + std::to_string(outPluginsCount) + " plugin(s) from staged /tmp/MPC.settings.new.";
    std::printf("[TKGL-SANITY] %s\n", outStatusMsg.c_str());
    logMessage("[TKGL-SANITY] " + outStatusMsg);
    if (outLogs) outLogs->push_back("[TKGL-SANITY] " + outStatusMsg);
    return totalPresetsUpdated;
}

void ConfigParser::refreshInstallStates(LauncherConfig& config, std::vector<std::string>* outLogs) {
    // 0. Ensure the Manager's Synths subdirectory is present in <SynthContentLocations> of MPC.settings
    ensureManagerSynthsInMpcSettings(outLogs);

    // 1. Determine MPC.settings file path
    std::string mpcSettingsPath = resolveMpcSettingsPath();

    // 2. Read session staged or active MPC.settings and extract <PLUGIN .../> entries inside <VALUE name="pluginList-arm">
    std::string mpcSettingsContent = loadSessionMpcSettingsXml();

    std::vector<MpcPluginXmlEntry> regPlugins;
    if (!mpcSettingsContent.empty()) {
        size_t valStart = 0, valOpenEnd = 0, valEnd = 0;
        bool valSelfClosing = false;
        if (findValueBlock(mpcSettingsContent, "pluginList-arm", valStart, valOpenEnd, valEnd, valSelfClosing) && !valSelfClosing) {
            regPlugins = extractPluginXmlEntries(mpcSettingsContent, valOpenEnd, valEnd);
        }
    }

    // 3. Match each catalog item to at most one <PLUGIN> entry in MPC.settings (2-pass priority:
    //    Pass 1 matches exact targetSubdir in file="...", Pass 2 matches exact plugin name/id)
    //    so distinct plugins sharing a binary name (e.g. "OB-Xd" by reales vs "OB-Xd MPC" by sd88me)
    //    never collide on the same <PLUGIN> entry.
    std::vector<bool> regClaimed(regPlugins.size(), false);
    std::unordered_map<std::string, int> itemMatchedRegIdx;

    // Pass 1: Exact subdirectory match in rp.file
    for (const auto& sec : config.sections) {
        for (const auto& item : sec.items) {
            std::string subDir = !item.targetSubdir.empty() ? item.targetSubdir : (item.author + " - VST - " + item.label);
            std::string altSubDir = item.author + " - VST - " + item.label;
            for (size_t r = 0; r < regPlugins.size(); ++r) {
                if (regClaimed[r]) continue;
                const auto& rp = regPlugins[r];
                bool subMatch = (!subDir.empty() && rp.file.find("/" + subDir + "/") != std::string::npos) ||
                                (!altSubDir.empty() && rp.file.find("/" + altSubDir + "/") != std::string::npos);
                if (subMatch) {
                    regClaimed[r] = true;
                    itemMatchedRegIdx[item.id + "::" + item.label] = static_cast<int>(r);
                    break;
                }
            }
        }
    }

    // Pass 2: Name / ID match for items not yet matched in Pass 1
    for (const auto& sec : config.sections) {
        for (const auto& item : sec.items) {
            std::string key = item.id + "::" + item.label;
            if (itemMatchedRegIdx.find(key) != itemMatchedRegIdx.end()) continue;
            for (size_t r = 0; r < regPlugins.size(); ++r) {
                if (regClaimed[r]) continue;
                const auto& rp = regPlugins[r];
                bool nameMatch = (!item.label.empty() && (rp.name == item.label || rp.descName == item.label));
                bool idMatch = (!item.id.empty() && (rp.name == item.id || rp.descName == item.id));
                if (nameMatch || idMatch) {
                    regClaimed[r] = true;
                    itemMatchedRegIdx[key] = static_cast<int>(r);
                    break;
                }
            }
        }
    }

    // Discover known VST2 Plugins / SynthContentLocations for file existence check
    std::string mgrDir = getManagerRootDir();
    std::vector<std::string> knownSynthDirs;
    auto synthLocs = getSynthContentLocations(mpcSettingsPath);
    for (const auto& sl : synthLocs) {
        if (!sl.path.empty()) knownSynthDirs.push_back(sl.path);
    }
    knownSynthDirs.push_back(mgrDir + "/VST2 Plugins");
    knownSynthDirs.push_back("./VST2 Plugins");
    DIR* mDir = opendir("/media");
    if (mDir) {
        struct dirent* dent;
        while ((dent = readdir(mDir)) != nullptr) {
            if (dent->d_name[0] == '.') continue;
            std::string d = std::string("/media/") + dent->d_name + "/VST2 Plugins";
            knownSynthDirs.push_back(d);
        }
        closedir(mDir);
    }
    std::sort(knownSynthDirs.begin(), knownSynthDirs.end());
    knownSynthDirs.erase(std::unique(knownSynthDirs.begin(), knownSynthDirs.end()), knownSynthDirs.end());

    // Verify each plugin's installation state and MPC.settings .so path integrity
    for (auto& sec : config.sections) {
        for (auto& item : sec.items) {
            std::string subDir = !item.targetSubdir.empty() ? item.targetSubdir : (item.author + " - VST - " + item.label);
            std::string binName = !item.targetBinary.empty() ? item.targetBinary : (item.id + ".so");
            std::string key = item.id + "::" + item.label;

            int matchedIdx = -1;
            auto itMatch = itemMatchedRegIdx.find(key);
            if (itMatch != itemMatchedRegIdx.end()) {
                matchedIdx = itMatch->second;
            }

            if (matchedIdx >= 0) {
                // Plugin IS registered in MPC.settings <pluginList-arm><KNOWNPLUGINS>
                std::string regFilePath = regPlugins[matchedIdx].file;
                struct stat stSo;
                if (!regFilePath.empty() && stat(regFilePath.c_str(), &stSo) == 0 && S_ISREG(stSo.st_mode)) {
                    size_t lastSlash = regFilePath.find_last_of('/');
                    if (lastSlash != std::string::npos) {
                        item.targetBinary = regFilePath.substr(lastSlash + 1);
                        item.skinFolderPath = regFilePath.substr(0, lastSlash);
                        size_t prevSlash = regFilePath.find_last_of('/', lastSlash - 1);
                        if (prevSlash != std::string::npos) {
                            item.targetSubdir = regFilePath.substr(prevSlash + 1, lastSlash - prevSlash - 1);
                        }
                    }
                    item.installed = 1; // Properly installed & accessible
                } else {
                    // Registered in MPC.settings but .so path is no longer accessible on storage -> installed = 2 (broken)
                    item.installed = 2;
                    size_t lastSlash = regFilePath.find_last_of('/');
                    if (lastSlash != std::string::npos) {
                        item.skinFolderPath = regFilePath.substr(0, lastSlash);
                    }
                    std::string brkMsg = "[WARNING] " + item.label + ": registered path in MPC.settings '" +
                        (regFilePath.empty() ? binName : regFilePath) +
                        "' is not accessible on storage -> marked installed = 2 (broken).";
                    logMessage(brkMsg);
                    if (outLogs) outLogs->push_back(brkMsg);
                }
            } else {
                // Plugin is NOT registered in MPC.settings -> not installed (installed = 0)
                item.installed = 0;
                if (!knownSynthDirs.empty()) {
                    item.skinFolderPath = knownSynthDirs[0] + "/" + subDir;
                }
            }
            item.isCached = isItemBinaryCached(item, &item.cachedZipSize);
        }
    }

}

bool ConfigParser::isSynthDirectoryWritable(const std::string& path) {
    if (path.empty()) return false;
    // 1. Exclude read-only rootfs, internal system flash (/media/az01-internal), and system directories directly
    if (path == "/" || path.rfind("/usr", 0) == 0 || path.rfind("/etc", 0) == 0 ||
        path.rfind("/bin", 0) == 0 || path.rfind("/sbin", 0) == 0 || path.rfind("/lib", 0) == 0 ||
        path.rfind("/root", 0) == 0 || path.rfind("/sys", 0) == 0 || path.rfind("/proc", 0) == 0 ||
        path == "/media/az01-internal" || path.rfind("/media/az01-internal/", 0) == 0) {
        return false;
    }

    // 2. Stat filesystem flags: check for ST_RDONLY
    struct statvfs sv;
    if (statvfs(path.c_str(), &sv) == 0) {
        if ((sv.f_flag & ST_RDONLY) != 0) {
            return false;
        }
    } else {
        size_t lastSlash = path.find_last_of('/');
        std::string parent = (lastSlash != std::string::npos && lastSlash > 0) ? path.substr(0, lastSlash) : "/";
        if (statvfs(parent.c_str(), &sv) == 0 && (sv.f_flag & ST_RDONLY) != 0) {
            return false;
        }
    }

    // 3. Check POSIX access permissions (W_OK) and probe file creation
    struct stat st;
    if (stat(path.c_str(), &st) == 0) {
        if (!S_ISDIR(st.st_mode)) return false;
        if (access(path.c_str(), W_OK) != 0) return false;
        std::string probeFile = path + "/.tkgl_rw_test";
        FILE* fp = std::fopen(probeFile.c_str(), "w");
        if (!fp) return false;
        std::fclose(fp);
        std::remove(probeFile.c_str());
        return true;
    } else {
        size_t lastSlash = path.find_last_of('/');
        std::string parent = (lastSlash != std::string::npos && lastSlash > 0) ? path.substr(0, lastSlash) : "/";
        if (access(parent.c_str(), W_OK) != 0) return false;
        std::string probeFile = parent + "/.tkgl_rw_test";
        FILE* fp = std::fopen(probeFile.c_str(), "w");
        if (!fp) return false;
        std::fclose(fp);
        std::remove(probeFile.c_str());
        return true;
    }
}

std::vector<SynthLocation> ConfigParser::getSynthContentLocations(const std::string& mpcSettingsPath) {
    (void)mpcSettingsPath;
    std::vector<SynthLocation> locs;

    std::string mgrDir = getManagerRootDir();
    std::string defaultMgrSynths = mgrDir + "/VST2 Plugins";
    mkdir(defaultMgrSynths.c_str(), 0755);

    auto getFreeSpace = [](const std::string& dirPath, std::string& outStr) -> uint64_t {
        struct statvfs sv;
        std::string checkPath = dirPath;
        if (statvfs(checkPath.c_str(), &sv) != 0) {
            size_t lastSlash = checkPath.find_last_of('/');
            std::string parentPath = (lastSlash != std::string::npos && lastSlash > 0) ? checkPath.substr(0, lastSlash) : "/media";
            if (statvfs(parentPath.c_str(), &sv) != 0) {
                checkPath = "/media";
                if (statvfs(checkPath.c_str(), &sv) != 0) {
                    checkPath = "/";
                    if (statvfs(checkPath.c_str(), &sv) != 0) {
                        outStr = "58.4 GB free";
                        return static_cast<uint64_t>(58.4 * 1024.0 * 1024.0 * 1024.0);
                    }
                }
            }
        }
        uint64_t bytes = static_cast<uint64_t>(sv.f_bavail) * static_cast<uint64_t>(sv.f_frsize);
        double freeGb = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
        char buf[32];
        if (freeGb >= 1.0) {
            std::snprintf(buf, sizeof(buf), "%.1f GB free", freeGb);
        } else {
            std::snprintf(buf, sizeof(buf), "%.0f MB free", freeGb * 1024.0);
        }
        outStr = std::string(buf);
        return bytes;
    };

    auto addUniqueVst2Loc = [&](const std::string& candidatePath, bool isMgrDefault, const std::string& customLabel = "") {
        if (candidatePath.empty() || candidatePath[0] != '/') return;
        if (candidatePath == "./VST2 Plugins") return;
        if (!isSynthDirectoryWritable(candidatePath)) return;
        for (const auto& l : locs) {
            if (l.path == candidatePath) return;
        }
        SynthLocation l;
        l.path = candidatePath;
        l.label = customLabel.empty() ? candidatePath : customLabel;
        l.freeBytes = getFreeSpace(candidatePath, l.freeSpace);
        l.isManagerDefault = isMgrDefault;
        locs.push_back(l);
    };

    // 1. Plugin Manager's own VST2 Plugins folder
    if (isSynthDirectoryWritable(defaultMgrSynths)) {
        addUniqueVst2Loc(defaultMgrSynths, true, "Manager VST2 Plugins (" + defaultMgrSynths + ")");
    }

    // 2. Read session staged or active MPC.settings for registered <SynthContentLocations>:
    //    - Keep any writable ".../VST2 Plugins" location already registered.
    //    - If MPC.settings only has Akai's default "/media/<Drive>/Synths" for a drive (e.g. SATA SSD 1 or 2),
    //      derive "/media/<Drive>/VST2 Plugins" as the VST2 installation target for that drive
    //      (without touching Akai's existing <Location> in MPC.settings).
    std::string content = loadSessionMpcSettingsXml();
    if (!content.empty()) {
        size_t valStart = 0, valOpenEnd = 0, valEnd = 0;
        bool valSelfClosing = false;
        if (findValueBlock(content, "SynthContentLocations", valStart, valOpenEnd, valEnd, valSelfClosing) && !valSelfClosing) {
            std::string sub = content.substr(valOpenEnd, valEnd - valOpenEnd);
            size_t tag = 0;
            while ((tag = sub.find("<Location>", tag)) != std::string::npos) {
                size_t closeTag = sub.find("</Location>", tag);
                if (closeTag == std::string::npos) break;
                std::string path = xmlUnescape(sub.substr(tag + 10, closeTag - (tag + 10)));
                tag = closeTag + 11;
                while (path.size() > 1 && path.back() == '/') path.pop_back();
                if (path.empty() || path[0] != '/') continue;

                // If this entry in MPC.settings is a default ".../Synths" path on a writable drive,
                // map our VST2 target to ".../VST2 Plugins" on that same drive
                const std::string synthsSuffix = "/Synths";
                if (path.size() > synthsSuffix.size() &&
                    path.compare(path.size() - synthsSuffix.size(), synthsSuffix.size(), synthsSuffix) == 0) {
                    std::string parentDrive = path.substr(0, path.size() - synthsSuffix.size());
                    if (parentDrive.rfind("/media/", 0) == 0 && parentDrive != "/media/az01-internal") {
                        addUniqueVst2Loc(parentDrive + "/VST2 Plugins", false);
                    }
                    continue;
                }

                // Otherwise, if it's already a writable ".../VST2 Plugins" or custom VST2 location, include it
                addUniqueVst2Loc(path, false);
            }
        }
    }

    // 3. Auto-discover all mounted writable drives in /media/* (USB stick, internal SATA SSD 1, SATA SSD 2, SD card)
    //    and offer "/media/<Drive>/VST2 Plugins" even if the "VST2 Plugins" folder has not been created yet on that drive.
    DIR* mediaDir = opendir("/media");
    if (mediaDir) {
        struct dirent* ent;
        while ((ent = readdir(mediaDir)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            if (std::strcmp(ent->d_name, "az01-internal") == 0) continue;
            std::string drive = std::string("/media/") + ent->d_name;
            struct stat st;
            if (stat(drive.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && isSynthDirectoryWritable(drive)) {
                std::string s1 = drive + "/VST2 Plugins";
                addUniqueVst2Loc(s1, false);
            }
        }
        closedir(mediaDir);
    }

    if (locs.empty()) {
        std::vector<std::string> fallbacks = {
            "/media/MPCLIVE500/VST2 Plugins",
            "/media/az01-internal-sd/VST2 Plugins"
        };
        for (const auto& fb : fallbacks) {
            addUniqueVst2Loc(fb, false);
        }
    }

    // Sort locations by free disk space descending (from largest to smallest)
    std::sort(locs.begin(), locs.end(), [](const SynthLocation& a, const SynthLocation& b) {
        return a.freeBytes > b.freeBytes;
    });

    return locs;
}

int ConfigParser::executeItemInstall(
    const LauncherConfig& cfg,
    const MenuItem& item,
    const std::string& targetSynthDir,
    std::vector<std::string>& outLogs,
    std::function<void(const std::string&)> onProgress
) {
    (void)cfg;
    std::string mgrDir = getManagerRootDir();
    std::string defaultMgrSynths = mgrDir + "/VST2 Plugins";
    std::string synthDir = (targetSynthDir.empty() || targetSynthDir == "./VST2 Plugins") ? defaultMgrSynths : targetSynthDir;
    while (synthDir.size() > 1 && synthDir.back() == '/') synthDir.pop_back();

    std::string tsStr = getBackupTimestampYYYYMMDDHHMMSS();
    std::string mpcSettings = resolveMpcSettingsPath();
    std::string defaultSubdir = !item.targetSubdir.empty() ? item.targetSubdir : (item.author + " - VST - " + item.label);
    std::string defaultBin    = !item.targetBinary.empty() ? item.targetBinary : (item.id + ".so");

    auto emitLog = [&](const std::string& line) {
        outLogs.push_back(line);
        ConfigParser::logMessage(line);
        if (onProgress) onProgress(line);
    };

    emitLog("[TKGL-INSTALL] Step 1/4 (Gold Rule): Preparing MPC.settings backup (" + tsStr + ")...");

    // If Reinstalling or Repairing: clean up any old files from other SynthLocations to allow clean location change
    if (item.installed == 1 || item.installed == 2) {
        auto oldLocs = getSynthContentLocations(mpcSettings);
        std::vector<std::string> allOldDirs;
        for (const auto& ol : oldLocs) {
            if (!ol.path.empty() && ol.path != synthDir) allOldDirs.push_back(ol.path);
        }
        allOldDirs.push_back(mgrDir + "/VST2 Plugins");
        DIR* md = opendir("/media");
        if (md) {
            struct dirent* de;
            while ((de = readdir(md)) != nullptr) {
                if (de->d_name[0] == '.') continue;
                std::string dp = std::string("/media/") + de->d_name + "/VST2 Plugins";
                if (dp != synthDir) allOldDirs.push_back(dp);
            }
            closedir(md);
        }
        std::sort(allOldDirs.begin(), allOldDirs.end());
        allOldDirs.erase(std::unique(allOldDirs.begin(), allOldDirs.end()), allOldDirs.end());

        std::vector<std::string> oldCandidates = {
            defaultSubdir,
            item.targetSubdir,
            item.author + " - VST - " + item.label,
            item.label,
            item.id
        };
        std::sort(oldCandidates.begin(), oldCandidates.end());
        oldCandidates.erase(std::unique(oldCandidates.begin(), oldCandidates.end()), oldCandidates.end());

        std::set<std::string> cleanedFolders;
        for (const auto& op : allOldDirs) {
            if (op.empty() || op == synthDir) continue;
            for (const auto& oc : oldCandidates) {
                if (oc.empty()) continue;
                std::string oldDir = op + "/" + oc;
                if (cleanedFolders.find(oldDir) != cleanedFolders.end()) continue;
                struct stat stOld;
                if (stat(oldDir.c_str(), &stOld) == 0 && S_ISDIR(stOld.st_mode)) {
                    cleanedFolders.insert(oldDir);
                    std::string rmCmd = "rm -rf \"" + oldDir + "\"";
                    int rmRes = system(rmCmd.c_str());
                    (void)rmRes;
                    emitLog("[TKGL-INSTALL] Cleaned old location: " + oldDir);
                }
            }
        }
    }

    // Step 2: Ensure target Synth directory exists on disk
    mkdir(synthDir.c_str(), 0755);
    emitLog("[TKGL-INSTALL] Step 2/4: Target Synth location: " + synthDir);

    // Step 3A: Resolve or download package binary archive
    std::string cachedZipPath;
    bool isCached = isItemBinaryCached(item, nullptr, &cachedZipPath);

    std::string usedZipPath;
    if (isCached && !cachedZipPath.empty()) {
        emitLog("[TKGL-INSTALL] Step 3/4: Using local cached binary archive: " + cachedZipPath + " (offline mode, no download needed)");
        usedZipPath = cachedZipPath;
    } else {
        std::string pkgUrl = item.downloadUrl;
        if (pkgUrl.empty() && !item.cmd.empty()) {
            size_t hpos = item.cmd.find("https://");
            if (hpos != std::string::npos) {
                size_t hend = item.cmd.find_first_of(" \"'\t\r\n", hpos);
                pkgUrl = (hend != std::string::npos)
                    ? item.cmd.substr(hpos, hend - hpos)
                    : item.cmd.substr(hpos);
            }
        }

        if (pkgUrl.empty()) {
            emitLog("[ERROR] No download URL or local binary available for " + item.label + " (" + item.id + ").");
            return -1;
        }

        std::string pingHost = cfg.ping_host.empty() ? "github.com" : cfg.ping_host;
        if (!checkInternetReachable(pingHost, &outLogs)) {
            emitLog("[ERROR] No internet connection (ping " + pingHost + " failed).");
            emitLog("[ERROR] Binary not cached locally. Switch to ONLINE mode to download.");
            return -1;
        }

        std::string pluginCacheDir = mgrDir + "/plugins/" + item.id;
        mkdir("plugins", 0755);
        mkdir((mgrDir + "/plugins").c_str(), 0755);
        mkdir(pluginCacheDir.c_str(), 0755);
        std::string permanentZip = pluginCacheDir + "/" + item.id + ".zip";

        std::string sizeLabel = item.sizeMb.empty() ? "ZIP" : item.sizeMb;
        emitLog("[TKGL-INSTALL] Step 3/4: Downloading " + item.label + " (" + sizeLabel + ") to local cache via libcurl (IPv4)...");
        emitLog("[TKGL-NET] Source: " + pkgUrl);

        bool preDownloaded = downloadFile(pkgUrl, permanentZip, 180, [&](const std::string& progLine) {
            if (onProgress) onProgress(progLine);
        }, "[TKGL-NET] Downloading");

        if (preDownloaded) {
            struct stat stZip;
            double mb = (stat(permanentZip.c_str(), &stZip) == 0) ? (static_cast<double>(stZip.st_size) / 1048576.0) : 0.0;
            char doneBuf[128];
            std::snprintf(doneBuf, sizeof(doneBuf), "[TKGL-NET] Download complete (%.2f MB), saved into local cache.", mb);
            emitLog(doneBuf);
            usedZipPath = permanentZip;
        } else {
            emitLog("[ERROR] libcurl IPv4 download failed for: " + pkgUrl);
            unlink(permanentZip.c_str());
            return -1;
        }
    }

    // Step 3B: Extract package to $SYNTH_DIR and record the extracted directory path to /tmp/tkgl_target_dir_<ts>
    char scriptPath[] = "/tmp/tkgl_inst_XXXXXX";
    int fd = mkstemp(scriptPath);
    if (fd < 0) {
        outLogs.push_back("[ERROR] Could not create temp installer script.");
        return -1;
    }
    std::string outTargetDirFile = std::string(scriptPath) + ".dir";

    std::ostringstream ss;
    ss << "#!/bin/sh\n";
    ss << "export PLG_MAN_DIR=\"" << mgrDir << "\"\n";
    ss << "cd \"$PLG_MAN_DIR\" 2>/dev/null || true\n";
    ss << "SYNTH_DIR=\"" << synthDir << "\"\n";
    ss << "TS=\"" << tsStr << "\"\n";
    ss << "DEFAULT_SUBDIR=\"" << defaultSubdir << "\"\n";
    ss << "mkdir -p \"$SYNTH_DIR\"\n";
    ss << "echo \"[TKGL-INSTALL] Extracting portable/ plugin bundle...\"\n";

    if (!usedZipPath.empty()) {
        ss << "TMP_ZIP=\"" << usedZipPath << "\"\n";
        ss << "TMP_EXT=\"/tmp/" << item.id << "_${TS}_pkg\"\n";
        ss << "rm -rf \"$TMP_EXT\"\n";
        ss << "mkdir -p \"$TMP_EXT\"\n";
        ss << "if [ ! -s \"$TMP_ZIP\" ]; then\n";
        ss << "  echo \"[ERROR] Package archive missing or empty: $TMP_ZIP\"\n";
        ss << "  rm -rf \"$TMP_EXT\"\n";
        ss << "  exit 1\n";
        ss << "fi\n";
        ss << "if ! unzip -q -o \"$TMP_ZIP\" -d \"$TMP_EXT\"; then\n";
        ss << "  echo \"[ERROR] Failed to unzip $TMP_ZIP\"\n";
        ss << "  rm -rf \"$TMP_EXT\"\n";
        ss << "  exit 1\n";
        ss << "fi\n";
        ss << "SRC_DIR=\"\"\n";
        ss << "if [ -d \"$TMP_EXT/portable\" ]; then\n";
        ss << "  SRC_DIR=$(find \"$TMP_EXT/portable\" -mindepth 1 -maxdepth 1 -type d | head -n 1)\n";
        ss << "fi\n";
        ss << "if [ -z \"$SRC_DIR\" ]; then\n";
        ss << "  META_FOUND=$(find \"$TMP_EXT\" -name \"plugin-meta.xml\" | head -n 1)\n";
        ss << "  if [ -n \"$META_FOUND\" ]; then SRC_DIR=$(dirname \"$META_FOUND\"); fi\n";
        ss << "fi\n";
        ss << "if [ -z \"$SRC_DIR\" ]; then\n";
        ss << "  SO_FOUND=$(find \"$TMP_EXT\" -name \"*.so\" | head -n 1)\n";
        ss << "  if [ -n \"$SO_FOUND\" ]; then SRC_DIR=$(dirname \"$SO_FOUND\"); fi\n";
        ss << "fi\n";
        ss << "if [ -z \"$SRC_DIR\" ] || [ \"$SRC_DIR\" = \"$TMP_EXT\" ]; then\n";
        ss << "  PLUGIN_DIR_NAME=\"$DEFAULT_SUBDIR\"\n";
        ss << "  TARGET_DIR=\"$SYNTH_DIR/$PLUGIN_DIR_NAME\"\n";
        ss << "  mkdir -p \"$TARGET_DIR\"\n";
        ss << "  cp -rf \"$TMP_EXT/\"* \"$TARGET_DIR/\"\n";
        ss << "else\n";
        ss << "  PLUGIN_DIR_NAME=$(basename \"$SRC_DIR\")\n";
        ss << "  TARGET_DIR=\"$SYNTH_DIR/$PLUGIN_DIR_NAME\"\n";
        ss << "  mkdir -p \"$SYNTH_DIR\"\n";
        ss << "  cp -rf \"$SRC_DIR\" \"$SYNTH_DIR/\"\n";
        ss << "fi\n";
        ss << "rm -rf \"$TMP_ZIP\" \"$TMP_EXT\"\n";
    } else {
        ss << "PLUGIN_DIR_NAME=\"$DEFAULT_SUBDIR\"\n";
        ss << "TARGET_DIR=\"$SYNTH_DIR/$PLUGIN_DIR_NAME\"\n";
        ss << "mkdir -p \"$TARGET_DIR\"\n";
        ss << "if [ -d \"./plugins/" << item.id << "\" ]; then\n";
        ss << "  cp -rf \"./plugins/" << item.id << "/\"* \"$TARGET_DIR/\"\n";
        ss << "fi\n";
    }
    ss << "chmod -R 755 \"$TARGET_DIR\" 2>/dev/null || true\n";
    ss << "printf '%s' \"$TARGET_DIR\" > \"" << outTargetDirFile << "\"\n";
    ss << "sync\n";

    std::string sBody = ss.str();
    ssize_t written = write(fd, sBody.c_str(), sBody.size());
    (void)written;
    close(fd);
    chmod(scriptPath, 0755);

    std::string runCmd = std::string("/bin/sh ") + scriptPath + " 2>&1; rm -f " + scriptPath;
    FILE* pipe = popen(runCmd.c_str(), "r");
    if (!pipe) {
        unlink(outTargetDirFile.c_str());
        outLogs.push_back("[ERROR] Failed to run extraction script.");
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), pipe)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0) continue;
        emitLog(line);
    }
    int extractRc = pclose(pipe);

    std::string targetDir = synthDir + "/" + defaultSubdir;
    {
        std::ifstream dirFile(outTargetDirFile, std::ios::binary);
        if (dirFile.is_open()) {
            std::stringstream dss;
            dss << dirFile.rdbuf();
            if (!dss.str().empty()) targetDir = dss.str();
            dirFile.close();
        }
        unlink(outTargetDirFile.c_str());
    }
    if (extractRc != 0) {
        return extractRc;
    }

    std::string pluginDirName = defaultSubdir;
    size_t lastSlash = targetDir.find_last_of('/');
    if (lastSlash != std::string::npos && lastSlash + 1 < targetDir.size()) {
        pluginDirName = targetDir.substr(lastSlash + 1);
    }

    // Resolve installed .so binary path in targetDir and build <PLUGIN .../> tag(s)
    std::string foundSo;
    {
        std::string prefSo = targetDir + "/" + defaultBin;
        struct stat stSo;
        if (stat(prefSo.c_str(), &stSo) == 0 && S_ISREG(stSo.st_mode)) {
            foundSo = prefSo;
        } else {
            DIR* d = opendir(targetDir.c_str());
            if (d) {
                struct dirent* de;
                while ((de = readdir(d)) != nullptr) {
                    size_t len = std::strlen(de->d_name);
                    if (len > 3 && std::strcmp(de->d_name + len - 3, ".so") == 0) {
                        foundSo = targetDir + "/" + de->d_name;
                        break;
                    }
                }
                closedir(d);
            }
        }
    }

    std::vector<std::string> newPluginTags;
    std::string metaXmlPath = targetDir + "/plugin-meta.xml";
    std::ifstream metaFile(metaXmlPath, std::ios::binary);
    if (metaFile.is_open()) {
        std::stringstream mss;
        mss << metaFile.rdbuf();
        std::string metaXml = mss.str();
        metaFile.close();
        auto metaEntries = extractPluginXmlEntries(metaXml, 0, metaXml.size());
        for (const auto& me : metaEntries) {
            std::string singleTag = normalizeSingleLineXmlTag(me.rawTag);
            std::string rawFileAttr = getXmlTagAttribute(singleTag, "file");
            std::string resolvedFile = rawFileAttr;
            if (!rawFileAttr.empty()) {
                const std::string token = "%payload-path%";
                size_t tpos = resolvedFile.find(token);
                if (tpos != std::string::npos) {
                    std::string cand1 = resolvedFile;
                    cand1.replace(tpos, token.size(), synthDir);
                    std::string cand2 = resolvedFile;
                    cand2.replace(tpos, token.size(), targetDir);
                    struct stat stSo;
                    if (stat(cand1.c_str(), &stSo) == 0) {
                        resolvedFile = cand1;
                    } else if (stat(cand2.c_str(), &stSo) == 0) {
                        resolvedFile = cand2;
                    } else {
                        resolvedFile = (rawFileAttr.find(token + "/" + pluginDirName) != std::string::npos) ? cand1 : cand2;
                    }
                } else {
                    struct stat stSo;
                    if (stat(resolvedFile.c_str(), &stSo) != 0 && !foundSo.empty()) {
                        resolvedFile = foundSo;
                    }
                }
            } else if (!foundSo.empty()) {
                resolvedFile = foundSo;
            }
            if (!resolvedFile.empty()) {
                singleTag = setXmlTagAttribute(singleTag, "file", resolvedFile);
            }
            newPluginTags.push_back(singleTag);
        }
    }

    if (newPluginTags.empty() && !foundSo.empty()) {
        std::string isInst = (item.shortType == "FX") ? "0" : "1";
        std::string nIn    = (item.shortType == "FX") ? "2" : "0";
        std::string genTag =
            "<PLUGIN name=\"" + xmlEscapeAttr(item.label) +
            "\" descriptiveName=\"" + xmlEscapeAttr(item.label) +
            "\" format=\"VST\" category=\"" + xmlEscapeAttr(item.category) +
            "\" manufacturer=\"" + xmlEscapeAttr(item.author) +
            "\" version=\"" + xmlEscapeAttr(item.version) +
            "\" file=\"" + xmlEscapeAttr(foundSo) +
            "\" isInstrument=\"" + isInst +
            "\" fileTime=\"0\" infoUpdateTime=\"0\" numInputs=\"" + nIn +
            "\" numOutputs=\"2\" isShell=\"0\"/>";
        newPluginTags.push_back(genTag);
    }

    // Determine primary .so target path and update all .xpl preset files in Presets/
    std::string primarySoPath = foundSo;
    if (!newPluginTags.empty()) {
        std::string firstTagFile = getXmlTagAttribute(newPluginTags[0], "file");
        if (!firstTagFile.empty()) primarySoPath = firstTagFile;
    }
    if (primarySoPath.empty()) {
        primarySoPath = targetDir + "/" + defaultBin;
    }

    int updatedPresetsCount = updatePluginPresetsXplFiles(targetDir, primarySoPath, newPluginTags);
    if (updatedPresetsCount > 0) {
        emitLog("[TKGL-PRESETS] Updated " + std::to_string(updatedPresetsCount) + " preset (.xpl) file(s) with file=\"" + primarySoPath + "\"");
        sync();
    }

    // Step 4: Register in MPC.settings using our multi-line-safe C++ XML parser & validator
    emitLog("[TKGL-INSTALL] Step 4/4: Registering in session staging (/tmp/MPC.settings.new)...");
    std::string settXml = loadSessionMpcSettingsXml();
    if (!settXml.empty()) {
        std::string valErr;
        if (!validateMpcSettingsXml(settXml, valErr)) {
            emitLog("[ERROR] Active MPC.settings is corrupted (" + valErr + "). Aborting installation to protect storage!");
            emitLog("[ERROR] Please restore a healthy backup via Advanced Settings -> MPC Settings.");
            return -1;
        }
        // 4A. Ensure target synthDir (e.g. /media/<Drive>/VST2 Plugins) is registered in <SynthContentLocations>
        bool locAdded = ensureSynthLocationInXmlString(settXml, synthDir);
        if (locAdded) {
            emitLog("[TKGL-INSTALL] Added <Location>" + synthDir + "</Location> to <SynthContentLocations>.");
        }

            if (!newPluginTags.empty()) {
                // 4C. Ensure <VALUE name="pluginList-arm"> exists and is not self-closing
                size_t armStart = 0, armOpenEnd = 0, armEnd = 0;
                bool armSelfClosing = false;
                if (!findValueBlock(settXml, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelfClosing)) {
                    size_t propEnd = settXml.rfind("</PROPERTIES>");
                    if (propEnd == std::string::npos) propEnd = settXml.rfind("</Properties>");
                    if (propEnd == std::string::npos) propEnd = settXml.rfind("</properties>");
                    if (propEnd != std::string::npos) {
                        std::string armBlock =
                            "  <VALUE name=\"pluginList-arm\">\n"
                            "    <KNOWNPLUGINS>\n"
                            "    </KNOWNPLUGINS>\n"
                            "  </VALUE>\n";
                        settXml.insert(propEnd, armBlock);
                    }
                } else if (armSelfClosing) {
                    std::string armBlock =
                        "<VALUE name=\"pluginList-arm\">\n"
                        "    <KNOWNPLUGINS>\n"
                        "    </KNOWNPLUGINS>\n"
                        "  </VALUE>";
                    settXml.replace(armStart, armEnd - armStart, armBlock);
                }

                // 4D. Re-locate pluginList-arm block and thoroughly purge all previous/broken entries of this plugin
                auto matchesPluginEntry = [&](const MpcPluginXmlEntry& ep) -> bool {
                    if (!item.label.empty() && (ep.name == item.label || ep.descName == item.label)) return true;
                    if (!item.id.empty() && (ep.name == item.id || ep.descName == item.id)) return true;
                    if (!pluginDirName.empty() && ep.file.find("/" + pluginDirName + "/") != std::string::npos) return true;
                    if (!defaultSubdir.empty() && ep.file.find("/" + defaultSubdir + "/") != std::string::npos) return true;
                    if (!item.targetSubdir.empty() && ep.file.find("/" + item.targetSubdir + "/") != std::string::npos) return true;
                    std::string altSub = item.author + " - VST - " + item.label;
                    if (!altSub.empty() && ep.file.find("/" + altSub + "/") != std::string::npos) return true;
                    if (!defaultBin.empty() && ep.file.find("/" + defaultBin) != std::string::npos) return true;
                    if (!item.targetBinary.empty() && ep.file.find("/" + item.targetBinary) != std::string::npos) return true;
                    if (ep.file.find("/" + item.id + ".so") != std::string::npos) return true;
                    for (const auto& nt : newPluginTags) {
                        std::string ntName = getXmlTagAttribute(nt, "name");
                        std::string ntFile = getXmlTagAttribute(nt, "file");
                        if (!ntName.empty() && ep.name == ntName) return true;
                        if (!ntFile.empty() && ep.file == ntFile) return true;
                    }
                    return false;
                };

                while (true) {
                    if (!findValueBlock(settXml, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelfClosing) || armSelfClosing) {
                        break;
                    }
                    auto existing = extractPluginXmlEntries(settXml, armOpenEnd, armEnd);
                    bool foundAndErased = false;
                    for (int idx = static_cast<int>(existing.size()) - 1; idx >= 0; --idx) {
                        if (matchesPluginEntry(existing[idx])) {
                            eraseXmlElementLineCleanly(settXml, existing[idx].startPos, existing[idx].endPos);
                            foundAndErased = true;
                            break;
                        }
                    }
                    if (!foundAndErased) break;
                }

                // 4E. Insert the new clean <PLUGIN .../> entry/entries into <KNOWNPLUGINS>
                if (findValueBlock(settXml, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelfClosing) && !armSelfClosing) {
                    std::string block = settXml.substr(armStart, armEnd - armStart);
                    std::string allNewLines;
                    for (const auto& nt : newPluginTags) {
                        allNewLines += "      " + nt + "\n";
                    }

                    size_t closeKp = block.find("</KNOWNPLUGINS>");
                    if (closeKp != std::string::npos) {
                        size_t absCloseKp = armStart + closeKp;
                        size_t lineStart = absCloseKp;
                        while (lineStart > armStart && (settXml[lineStart - 1] == ' ' || settXml[lineStart - 1] == '\t')) {
                            lineStart--;
                        }
                        settXml.insert(lineStart, allNewLines);
                    } else {
                        size_t kpPos = block.find("<KNOWNPLUGINS");
                        if (kpPos != std::string::npos) {
                            size_t absKp = armStart + kpPos;
                            bool kpSelf = false;
                            size_t kpEnd = findXmlTagEnd(settXml, absKp, kpSelf);
                            if (kpEnd != std::string::npos && kpSelf) {
                                std::string replacement = "<KNOWNPLUGINS>\n" + allNewLines + "    </KNOWNPLUGINS>";
                                settXml.replace(absKp, kpEnd - absKp, replacement);
                            }
                        } else {
                            size_t closeValPos = armEnd - 8;
                            std::string inject = "  <KNOWNPLUGINS>\n" + allNewLines + "    </KNOWNPLUGINS>\n  ";
                            settXml.insert(closeValPos, inject);
                        }
                    }
                }
            }

            // 4F. Validate XML integrity and stage in session file /tmp/MPC.settings.new (committed upon exit)
            if (!newPluginTags.empty() || locAdded) {
                if (stageSessionMpcSettingsXml(settXml, &outLogs)) {
                    emitLog("[TKGL-INSTALL] Clean plugin entry & SynthContentLocations registered in session staging (/tmp/MPC.settings.new).");
                } else {
                    return -1;
                }
            }
        }

    emitLog("[TKGL-INSTALL] SUCCESS: " + pluginDirName + " " + (item.installed == 2 ? "repaired" : "installed") + " in " + synthDir);
    return 0;
}

int ConfigParser::executeItemUninstall(
    const LauncherConfig& cfg,
    const MenuItem& item,
    std::vector<std::string>& outLogs,
    std::function<void(const std::string&)> onProgress
) {
    (void)cfg;
    std::string mgrDir = getManagerRootDir();
    std::string mpcSettings = resolveMpcSettingsPath();
    std::string defaultSubdir = item.targetSubdir.empty()
        ? (item.author + " - VST - " + item.label)
        : item.targetSubdir;
    std::string defaultBin = item.targetBinary.empty()
        ? (item.id + ".so")
        : item.targetBinary;

    auto emitLog = [&](const std::string& line) {
        outLogs.push_back(line);
        ConfigParser::logMessage(line);
        if (onProgress) onProgress(line);
    };

    emitLog("[TKGL-REMOVE] Step 1/3: Unregistering plugin in session staging (/tmp/MPC.settings.new)...");

    // Step 1 & 2: Safely remove all matching <PLUGIN .../> entries from MPC.settings in session staging
    std::string settXml = loadSessionMpcSettingsXml();
    if (settXml.empty()) {
        emitLog("[ERROR] Could not load active or staged MPC.settings for uninstall.");
        return -1;
    }

    std::string valErr;
    if (!validateMpcSettingsXml(settXml, valErr)) {
        emitLog("[ERROR] Active MPC.settings is corrupted (" + valErr + "). Aborting uninstall to protect storage!");
        emitLog("[ERROR] Please restore a healthy backup via Advanced Settings -> MPC Settings.");
        return -1;
    }

    size_t armStart = 0, armOpenEnd = 0, armEnd = 0;
    bool armSelfClosing = false;
    bool removedAny = false;

    auto matchesPlugin = [&](const MpcPluginXmlEntry& ep) -> bool {
        if (!item.label.empty() && (ep.name == item.label || ep.descName == item.label)) return true;
        if (!item.id.empty() && (ep.name == item.id || ep.descName == item.id)) return true;
        if (!defaultSubdir.empty() && ep.file.find("/" + defaultSubdir + "/") != std::string::npos) return true;
        if (!item.targetSubdir.empty() && ep.file.find("/" + item.targetSubdir + "/") != std::string::npos) return true;
        std::string altSub = item.author + " - VST - " + item.label;
        if (!altSub.empty() && ep.file.find("/" + altSub + "/") != std::string::npos) return true;
        if (!defaultBin.empty() && ep.file.find("/" + defaultBin) != std::string::npos) return true;
        if (!item.targetBinary.empty() && ep.file.find("/" + item.targetBinary) != std::string::npos) return true;
        if (ep.file.find("/" + item.id + ".so") != std::string::npos) return true;
        return false;
    };

    while (true) {
        if (!findValueBlock(settXml, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelfClosing) || armSelfClosing) {
            break;
        }
        auto existing = extractPluginXmlEntries(settXml, armOpenEnd, armEnd);
        bool foundInPass = false;
        for (int idx = static_cast<int>(existing.size()) - 1; idx >= 0; --idx) {
            if (matchesPlugin(existing[idx])) {
                eraseXmlElementLineCleanly(settXml, existing[idx].startPos, existing[idx].endPos);
                removedAny = true;
                foundInPass = true;
                break;
            }
        }
        if (!foundInPass) break;
    }

    if (removedAny) {
        if (stageSessionMpcSettingsXml(settXml, &outLogs)) {
            emitLog("[TKGL-REMOVE] Unregistered all instances of " + item.label + " in session staging (/tmp/MPC.settings.new).");
        } else {
            emitLog("[ERROR] Failed to stage updated MPC.settings during uninstall. Aborting without deleting files on storage!");
            return -1;
        }
    }

    // Step 3: Remove plugin directories and binaries across ALL SynthContentLocations and mounted drives
    emitLog("[TKGL-REMOVE] Step 3/3: Removing plugin files from all SynthContentLocations...");
    char scriptPath[] = "/tmp/tkgl_uninst_XXXXXX";
    int fd = mkstemp(scriptPath);
    if (fd < 0) {
        outLogs.push_back("[ERROR] Failed to create temporary uninstall script in /tmp.");
        return -1;
    }

    std::ostringstream ss;
    ss << "#!/bin/sh\n";

    std::vector<std::string> allLocs;
    auto locs = getSynthContentLocations(mpcSettings);
    for (const auto& loc : locs) {
        if (!loc.path.empty()) allLocs.push_back(loc.path);
    }
    allLocs.push_back(mgrDir + "/VST2 Plugins");
    allLocs.push_back("./VST2 Plugins");

    // Auto-discover /media/*/VST2 Plugins and /media/*/Expansions
    DIR* mediaDir = opendir("/media");
    if (mediaDir) {
        struct dirent* ent;
        while ((ent = readdir(mediaDir)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string drive = std::string("/media/") + ent->d_name;
            struct stat st;
            if (stat(drive.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                allLocs.push_back(drive + "/VST2 Plugins");
                allLocs.push_back(drive + "/Expansions");
            }
        }
        closedir(mediaDir);
    }

    std::sort(allLocs.begin(), allLocs.end());
    allLocs.erase(std::unique(allLocs.begin(), allLocs.end()), allLocs.end());

    std::vector<std::string> candSubdirs = {
        defaultSubdir,
        item.targetSubdir,
        item.author + " - VST - " + item.label,
        item.label,
        item.id
    };
    std::sort(candSubdirs.begin(), candSubdirs.end());
    candSubdirs.erase(std::unique(candSubdirs.begin(), candSubdirs.end()), candSubdirs.end());

    std::vector<std::string> candBins = {
        defaultBin,
        item.targetBinary,
        item.id + ".so"
    };
    std::sort(candBins.begin(), candBins.end());
    candBins.erase(std::unique(candBins.begin(), candBins.end()), candBins.end());

    for (const auto& l : allLocs) {
        if (l.empty()) continue;
        for (const auto& sd : candSubdirs) {
            if (sd.empty()) continue;
            ss << "if [ -d \"" << l << "/" << sd << "\" ]; then\n";
            ss << "  rm -rf \"" << l << "/" << sd << "\"\n";
            ss << "  echo \"[TKGL-REMOVE] Deleted directory: " << l << "/" << sd << "\"\n";
            ss << "fi\n";
        }
        for (const auto& cb : candBins) {
            if (cb.empty()) continue;
            ss << "if [ -f \"" << l << "/" << cb << "\" ]; then\n";
            ss << "  rm -f \"" << l << "/" << cb << "\"\n";
            ss << "  echo \"[TKGL-REMOVE] Deleted binary: " << l << "/" << cb << "\"\n";
            ss << "fi\n";
        }
    }
    ss << "sync\n";
    ss << "echo \"[TKGL-REMOVE] SUCCESS: " << item.label << " cleanly uninstalled.\"\n";

    std::string sBody = ss.str();
    ssize_t written = write(fd, sBody.c_str(), sBody.size());
    (void)written;
    close(fd);
    chmod(scriptPath, 0755);

    std::string runCmd = std::string("/bin/sh ") + scriptPath + " 2>&1; rm -f " + scriptPath;
    FILE* pipe = popen(runCmd.c_str(), "r");
    if (!pipe) {
        outLogs.push_back("[ERROR] Failed to run uninstaller script.");
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), pipe)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 0) continue;
        emitLog(line);
    }
    return pclose(pipe);
}

int ConfigParser::executeItemCommand(
    const LauncherConfig& cfg,
    const MenuItem& item,
    std::vector<std::string>& outLogs
) {
    (void)cfg;
    if (item.cmd.empty()) {
        outLogs.push_back("[ERROR] Empty command.");
        return -1;
    }

    std::string mgrDir = getManagerRootDir();
    std::string finalCmd;
    if (item.is_inline) {
        char tmpPath[] = "/tmp/tkgl_exec_XXXXXX";
        int fd = mkstemp(tmpPath);
        if (fd >= 0) {
            std::string defSynthDir = mgrDir + "/VST2 Plugins";
            std::string scriptBody = "#!/bin/sh\nexport PLG_MAN_DIR=\"" + mgrDir + "\"\ncd \"$PLG_MAN_DIR\" 2>/dev/null || true\nexport SYNTH_INSTALL_DIR=\"${SYNTH_INSTALL_DIR:-" + defSynthDir + "}\"\n" + item.cmd + "\n";
            ssize_t written = write(fd, scriptBody.c_str(), scriptBody.size());
            (void)written;
            close(fd);
            chmod(tmpPath, 0755);
            finalCmd = std::string("/bin/sh ") + tmpPath + " 2>&1; rm -f " + tmpPath;
        } else {
            std::string defSynthDir = mgrDir + "/VST2 Plugins";
            finalCmd = "PLG_MAN_DIR=\"" + mgrDir + "\" SYNTH_INSTALL_DIR=\"" + defSynthDir + "\" /bin/sh -c '" + item.cmd + "' 2>&1";
        }
    } else {
        std::string defSynthDir = mgrDir + "/VST2 Plugins";
        finalCmd = "PLG_MAN_DIR=\"" + mgrDir + "\" SYNTH_INSTALL_DIR=\"" + defSynthDir + "\" /bin/sh " + item.cmd + " 2>&1";
    }

    FILE* pipe = popen(finalCmd.c_str(), "r");
    if (!pipe) {
        outLogs.push_back("[ERROR] Failed to spawn /bin/sh.");
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), pipe)) {
        line[strcspn(line, "\r\n")] = 0;
        outLogs.emplace_back(line);
    }
    return pclose(pipe);
}

} // namespace tkgl::config
