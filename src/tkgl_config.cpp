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
#include <sys/utsname.h>
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

static void findAllSoFilesRecursive(const std::string& dirPath, std::vector<std::string>& outSoFiles) {
    DIR* d = opendir(dirPath.c_str());
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (std::strcmp(de->d_name, ".") == 0 || std::strcmp(de->d_name, "..") == 0) continue;
        std::string fullPath = dirPath + "/" + de->d_name;
        struct stat st;
        if (lstat(fullPath.c_str(), &st) != 0) continue;
        if (S_ISLNK(st.st_mode)) continue;
        if (S_ISDIR(st.st_mode)) {
            findAllSoFilesRecursive(fullPath, outSoFiles);
        } else if (S_ISREG(st.st_mode)) {
            size_t len = std::strlen(de->d_name);
            if (len > 3 && std::strcmp(de->d_name + len - 3, ".so") == 0) {
                outSoFiles.push_back(fullPath);
            }
        }
    }
    closedir(d);
}

static bool verifyDirectorySoCompatibility(
    const std::string& dirPath,
    const std::function<void(const std::string&)>& emitLog
) {
    std::string sysDesc;
    int sysBits = ConfigParser::getSystemElfBitness(&sysDesc);

    std::vector<std::string> soFiles;
    findAllSoFilesRecursive(dirPath, soFiles);
    std::sort(soFiles.begin(), soFiles.end());

    if (soFiles.empty()) {
        emitLog("[TKGL-ARCH] System is " + std::to_string(sysBits) + "-bit (" + sysDesc + "); no .so binary in package.");
        return true;
    }

    for (const auto& soPath : soFiles) {
        std::string fileName = soPath;
        size_t sl = soPath.find_last_of('/');
        if (sl != std::string::npos && sl + 1 < soPath.size()) {
            fileName = soPath.substr(sl + 1);
        }
        std::string soDesc;
        int soBits = ConfigParser::getElfFileBitness(soPath, &soDesc);
        if (soBits != sysBits) {
            std::string foundBitsStr = (soBits > 0)
                ? (std::to_string(soBits) + "-bit (" + soDesc + ")")
                : ("invalid/non-ELF (" + soDesc + ")");
            emitLog("[ERROR] Architecture mismatch: " + fileName + " is " + foundBitsStr + ", current OS is " + std::to_string(sysBits) + "-bit (" + sysDesc + ").");
            emitLog("[ERROR] Rule: 32-bit plugins require a 32-bit OS; 64-bit plugins require a 64-bit OS.");
            emitLog("[ERROR] Installation ABORTED. No files installed and MPC.settings untouched.");
            return false;
        }
        emitLog("[TKGL-ARCH] Verified " + fileName + ": " + std::to_string(soBits) + "-bit (" + soDesc + ") compatible with OS " + std::to_string(sysBits) + "-bit.");
    }
    return true;
}

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

    return "/tmp/KikGen_MPC_Force_Plugin_Manager";
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
            "5730", "7.6 MB", "31%",
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
            "8bit-schmitter", "8Bit Schmitter Studio", "Tinythingz", "v1.0.14", "Experimental",
            "INST", "Stable", "Akai Force (3.10.1)", "MPC OS 3.x", "2026-11-15",
            "729", "5.3 MB", "14%",
            "TINYTHINGZ - VST - 8Bit Schmitter Studio", "VST/schmitter-arm32.so",
            "https://raw.githubusercontent.com/Tinythingz/The-8bit-Schmitter/b582294a2cccff88be86b8fec4a6aedce750c0f8/full-schmitt.png", "https://github.com/Tinythingz/The-8bit-Schmitter/releases/download/v1.0.14/8Bit-Schmitter-Studio-1.0.14-mpc-armv7.zip", "plugins/8bit-schmitter/thumb.png",
            "CMOS-inspired drone synth with four voices, two six-step sequencers, a modulation matrix and Clouds-derived effects.", "./plugins/8bit-schmitter/install.sh", 0,
            0,
            false,
            false,
            "",
            { "drone", "sequencer", "modulation" }
        },
        {
            "tr-8w8", "8W8", "athousanddetails", "v1.3.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-12",
            "6369", "5.9 MB", "13%",
            "athousanddetails - VST - 8W8", "tr8w8.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/ba525167c18bb0961dfd60d5989a033f737fe517/docs/screenshots/8w8.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/8w8-vst-v1.3.1/8W8-1.3.1-mpc-armv7.zip", "plugins/tr-8w8/thumb.png",
            "MPC/Force port of athousanddetails' schwung-8W8: 808 voices modelled by hand from original schematics and knowledge bases for low CPU; runs on Gen1 and Gen2 MPCs. Touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/athousanddetails - VST - 8W8\" && ZIP_PATH=\"./plugins/tr-8w8/tr-8w8.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/8w8-vst-v1.3.1/8W8-1.3.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/tr-8w8\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/8w8-vst-v1.3.1/8W8-1.3.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/athousanddetails - VST - 8W8/\"; elif [ -f \"./plugins/tr-8w8/tr8w8.so\" ]; then cp -rf ./plugins/tr-8w8/* \"$TARGET_SYNTH_DIR/athousanddetails - VST - 8W8/\"; else echo \"[ERROR] No local binary archive found for tr-8w8\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "tr-808" }
        },
        {
            "tr-8x8", "8X8", "sd88me", "v1.0.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-21",
            "243", "6.2 MB", "14%",
            "sd88me - VST - 8X8", "tr8x8.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/0a64f7de209368d98b848d850bede77bdbe721d5/docs/screenshots/8x8.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/8x8-vst-v1.0.1/8X8-1.0.1-mpc-aarch64.zip", "plugins/tr-8x8/thumb.png",
            "A circuit-solved TR-808 for Gen2 MPC devices: the voices are the 808's own circuits, solved from the schematics with Lemus, not modelled by ear. Same panel as 8W8, plus drive and distortion per voice, reverb and delay sends, a master bus, extra controls and a Latency setting. CPU heavy, Gen2 only.", "./plugins/tr-8x8/install.sh", 0,
            0,
            false,
            false,
            "",
            { "tr-808", "gen2" }
        },
        {
            "tr-9w9", "9W9", "athousanddetails", "v1.3.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-12",
            "6036", "5.1 MB", "21%",
            "athousanddetails - VST - 9W9", "tr9w9.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/ba525167c18bb0961dfd60d5989a033f737fe517/docs/screenshots/9w9.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/9w9-vst-v1.3.1/9W9-1.3.1-mpc-armv7.zip", "plugins/tr-9w9/thumb.png",
            "MPC/Force port of athousanddetails' schwung-9W9: 909 modelled after ER-99's open-source emulation, with its cymbal samples; runs on Gen1 and Gen2 MPCs. Touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/athousanddetails - VST - 9W9\" && ZIP_PATH=\"./plugins/tr-9w9/tr-9w9.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/9w9-vst-v1.3.1/9W9-1.3.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/tr-9w9\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/9w9-vst-v1.3.1/9W9-1.3.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/athousanddetails - VST - 9W9/\"; elif [ -f \"./plugins/tr-9w9/tr9w9.so\" ]; then cp -rf ./plugins/tr-9w9/* \"$TARGET_SYNTH_DIR/athousanddetails - VST - 9W9/\"; else echo \"[ERROR] No local binary archive found for tr-9w9\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "tr-909" }
        },
        {
            "tr-9x9", "9X9", "sd88me", "v1.0.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-21",
            "226", "5.4 MB", "14%",
            "sd88me - VST - 9X9", "tr9x9.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/0a64f7de209368d98b848d850bede77bdbe721d5/docs/screenshots/9x9.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/9x9-vst-v1.0.1/9X9-1.0.1-mpc-aarch64.zip", "plugins/tr-9x9/thumb.png",
            "A circuit-solved TR-909 for Gen2 MPC devices: kick, snare, toms, rim and clap are the 909's own circuits, solved with Lemus. Hats, ride and crash play the 909's ROMs, which you supply; without them 9W9's samples play. Drive, sends, master bus, extra controls, Latency. CPU heavy, Gen2 only.", "./plugins/tr-9x9/install.sh", 0,
            0,
            false,
            false,
            "",
            { "tr-909", "gen2" }
        },
        {
            "acid-303", "Acid-303", "sd88me", "v1.0.2", "Mono Bass",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "3123", "23.6 MB", "28%",
            "sd88me - VST - Acid-303", "acid303.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-acid303/main/docs/device-303.png", "https://github.com/sd88me/mpc-vst-acid303/releases/download/acid303-vst-v1.0.2/Acid-303-1.0.2-mpc-armv7.zip", "plugins/acid-303/thumb.png",
            "An accurate, circuit-modelled 303 voice, matched to the hardware: the voice (from Lemus) solves the original schematic transistor by transistor and is fitted to 400 notes from a real unit. Switchable Devil Fish mods; Gen1 and Gen2 (WARN for CPU on Gen1: one or two instances, ECO).", "./plugins/acid-303/install.sh", 0,
            0,
            false,
            false,
            "",
            { "303", "acid", "mono", "bass", "circuit-model", "devil-fish" }
        },
        {
            "noxed-acidline", "Acidline", "NOXED", "v0.3.0", "Mono Bass",
            "INST", "Stable", "MPC Live III (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-20",
            "277", "1.6 MB", "14%",
            "NOXED - VST - Acidline", "VST/acidline-arm32.so",
            "https://raw.githubusercontent.com/molgino/noxend-mpc-plugins/40832b26596b1c0228eb97e0dc81c257547a2d1a/docs/screenshots/acidline-sequencer.png", "https://github.com/molgino/noxend-mpc-plugins/releases/download/v0.3.0/noxed-acidline-0.3.0-mpc-armv7.zip", "plugins/noxed-acidline/thumb.png",
            "Monophonic acid bass synth with accent, slide, a transport-synced 16-step pitch/time sequencer and an optional Open303 sound model. Includes six presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/NOXED - VST - Acidline\" && ZIP_PATH=\"./plugins/noxed-acidline/noxed-acidline.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/molgino/noxend-mpc-plugins/releases/download/v0.3.0/noxed-acidline-0.3.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/noxed-acidline\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/molgino/noxend-mpc-plugins/releases/download/v0.3.0/noxed-acidline-0.3.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/NOXED - VST - Acidline/\"; elif [ -f \"./plugins/noxed-acidline/VST/acidline-arm32.so\" ]; then cp -rf ./plugins/noxed-acidline/* \"$TARGET_SYNTH_DIR/NOXED - VST - Acidline/\"; else echo \"[ERROR] No local binary archive found for noxed-acidline\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "synth", "acid", "sequencer" }
        },
        {
            "aphex", "Aphex", "saustin2010", "v1.1.1", "Mono Bass",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3443", "7.4 MB", "14%",
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
            "audity-78", "Audity '78", "Lumina Aerospace LLC", "v0.2.0", "Wavetable",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-23",
            "45", "649 KB", "14%",
            "Lumina Aerospace LLC - VST - Audity 78", "VST/audity-78-arm32.so",
            "https://raw.githubusercontent.com/Luminous-Works/mpc-vst-audity-78/v0.1.0/docs/screenshots/audity-78.png", "https://github.com/Luminous-Works/mpc-vst-audity-78/releases/download/v0.2.0/Audity-78-0.2.0-mpc-armv7.zip", "plugins/audity-78/thumb.png",
            "Wavetable synth: two morphing wavetable oscillators, SSM2040-style filters, chorus, delay and reverb. 16 sounds.", "./plugins/audity-78/install.sh", 0,
            0,
            false,
            false,
            "",
            { "wavetable", "polyphonic", "analog", "chorus", "reverb" }
        },
        {
            "breaks", "Breaks", "Aaron Quint", "v0.9.7", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-12",
            "1782", "5.6 MB", "9%",
            "quirkey - VST - Breaks", "breaks.so",
            "https://raw.githubusercontent.com/quirkey/mpc-plugins/breaks-v0.9.7/docs/breaks.png", "https://github.com/quirkey/mpc-plugins/releases/download/breaks-v0.9.7/Breaks-0.9.7-mpc-armv7.zip", "plugins/breaks/thumb.png",
            "Plays a drum break in time with the song while the transport runs: 16th slices retriggered on the song grid, through the whole break whatever the loop length, with pitch, bit and sample-rate grit, time-stretch grains, half time, seeded variations and pad effects. Bring your own breaks.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/quirkey - VST - Breaks\" && ZIP_PATH=\"./plugins/breaks/breaks.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/quirkey/mpc-plugins/releases/download/breaks-v0.9.7/Breaks-0.9.7-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/breaks\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/quirkey/mpc-plugins/releases/download/breaks-v0.9.7/Breaks-0.9.7-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/quirkey - VST - Breaks/\"; elif [ -f \"./plugins/breaks/breaks.so\" ]; then cp -rf ./plugins/breaks/* \"$TARGET_SYNTH_DIR/quirkey - VST - Breaks/\"; else echo \"[ERROR] No local binary archive found for breaks\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "breakbeat", "loop", "slicer", "tempo-sync", "lo-fi" }
        },
        {
            "chordism", "Chordism", "saustin2010", "v1.0.2", "Fm",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3498", "2.3 MB", "14%",
            "Charles Vestal - VST - [SYN] Chordism", "chordism.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-chordism/v1.0.2/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-chordism/releases/download/v1.0.2/SYN-Chordism-1.0.2-mpc-armv7.zip", "plugins/chordism/thumb.png",
            "One key in, a four-voice chord out: morphing oscillators, FM, filter, lo-fi, delay, reverb and an arpeggiator.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] Chordism\" && ZIP_PATH=\"./plugins/chordism/chordism.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-chordism/releases/download/v1.0.2/SYN-Chordism-1.0.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/chordism\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-chordism/releases/download/v1.0.2/SYN-Chordism-1.0.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] Chordism/\"; elif [ -f \"./plugins/chordism/chordism.so\" ]; then cp -rf ./plugins/chordism/* \"$TARGET_SYNTH_DIR/Charles Vestal - VST - [SYN] Chordism/\"; else echo \"[ERROR] No local binary archive found for chordism\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "chords", "arpeggiator", "fm", "lo-fi" }
        },
        {
            "clementine-xt", "Clementine-XT", "sd88me", "v0.9.7", "Wavetable",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-11-01",
            "8165", "2.5 MB", "14%",
            "sd88me - VST - Clementine-XT", "clementine_xt.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-clementineXT/main/docs/img/clementine-xt.png", "https://github.com/sd88me/mpc-vst-clementineXT/releases/download/clementine-xt-v0.9.7/Clementine-XT-0.9.7-mpc-armv7.zip", "plugins/clementine-xt/thumb.png",
            "Ten-voice wavetable synthesizer modelled exactly on the Microwave II/XT with a ten-page touchscreen skin and Q-Links. Needs your own ROM files in its ROMS folder.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Clementine-XT\" && ZIP_PATH=\"./plugins/clementine-xt/clementine-xt.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-clementineXT/releases/download/clementine-xt-v0.9.7/Clementine-XT-0.9.7-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/clementine-xt\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-clementineXT/releases/download/clementine-xt-v0.9.7/Clementine-XT-0.9.7-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Clementine-XT/\"; elif [ -f \"./plugins/clementine-xt/clementine_xt.so\" ]; then cp -rf ./plugins/clementine-xt/* \"$TARGET_SYNTH_DIR/sd88me - VST - Clementine-XT/\"; else echo \"[ERROR] No local binary archive found for clementine-xt\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "wavetable", "polyphonic", "arpeggiator" }
        },
        {
            "op1fx-cluster", "Cluster", "Bombest Audio", "v0.6.0", "Experimental",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "2089", "3.5 MB", "14%",
            "Bombest Audio - VST - Cluster", "cluster.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/cluster.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Cluster-0.6.0-mpc-armv7.zip", "plugins/op1fx-cluster/thumb.png",
            "Eight self-feedback sine oscillators that drift apart over a note. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Cluster\" && ZIP_PATH=\"./plugins/op1fx-cluster/op1fx-cluster.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Cluster-0.6.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-cluster\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Cluster-0.6.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Cluster/\"; elif [ -f \"./plugins/op1fx-cluster/cluster.so\" ]; then cp -rf ./plugins/op1fx-cluster/* \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Cluster/\"; else echo \"[ERROR] No local binary archive found for op1fx-cluster\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "synth", "feedback", "drone" }
        },
        {
            "crate-digger", "Crate Digger", "sd88me", "v2.2.0", "Dj Players",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-19",
            "18764", "73.4 MB", "14%",
            "sd88me - VST - Crate Digger", "cratedigger.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-cratedigger/cratedigger-vst-v2.2.0/docs/screenshots/deck-play.png", "https://github.com/sd88me/mpc-vst-cratedigger/releases/download/cratedigger-vst-v2.2.0/Crate-Digger-2.2.0-mpc-armv7.zip", "plugins/crate-digger/thumb.png",
            "Dig for records by genre, style, decade, region and country on Discogs, then stream and play them into an MPC track, with its own screen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Crate Digger\" && ZIP_PATH=\"./plugins/crate-digger/crate-digger.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-cratedigger/releases/download/cratedigger-vst-v2.2.0/Crate-Digger-2.2.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/crate-digger\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-cratedigger/releases/download/cratedigger-vst-v2.2.0/Crate-Digger-2.2.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Crate Digger/\"; elif [ -f \"./plugins/crate-digger/cratedigger.so\" ]; then cp -rf ./plugins/crate-digger/* \"$TARGET_SYNTH_DIR/sd88me - VST - Crate Digger/\"; else echo \"[ERROR] No local binary archive found for crate-digger\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "discogs", "streaming" }
        },
        {
            "csxxl", "CSXXL", "L'Cronx", "v1.0.13", "Polysynth",
            "INST", "Stable", "Force (Gen1) (3.9.1)", "MPC OS 3.x", "2026-11-12",
            "2130", "7.2 MB", "14%",
            "L'Cronx - VST - CSXXL", "csxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/CSXXL/v1.0.13/docs/screenshots/hero.jpg", "https://github.com/sunskiefer/CSXXL/releases/download/v1.0.13/CSXXL-1.0.13-mpc-armv7.zip", "plugins/csxxl/thumb.png",
            "A two-layer, 8-voice, polyphonic synth in the spirit of the Yamaha CS-80, written from voice notes and self-built schematics. Slider-only panels over 12 pages, per-key aftertouch, touch ribbon, arpeggiator with an 8-step gate per layer, ring mod, chorus and reverb, 768 factory presets in a named list. MPC OS 3.x.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - CSXXL\" && ZIP_PATH=\"./plugins/csxxl/csxxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/CSXXL/releases/download/v1.0.13/CSXXL-1.0.13-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/csxxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/CSXXL/releases/download/v1.0.13/CSXXL-1.0.13-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - CSXXL/\"; elif [ -f \"./plugins/csxxl/csxxl.so\" ]; then cp -rf ./plugins/csxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - CSXXL/\"; else echo \"[ERROR] No local binary archive found for csxxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "synth", "polysynth", "analog", "cs-80", "layers", "arpeggiator", "ribbon", "presets" }
        },
        {
            "tr-cw78", "CW-78", "athousanddetails", "v1.3.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-12",
            "5550", "6.9 MB", "8%",
            "athousanddetails - VST - CW-78", "trcw78.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/ba525167c18bb0961dfd60d5989a033f737fe517/docs/screenshots/cw78.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/cw78-vst-v1.3.1/CW-78-1.3.1-mpc-armv7.zip", "plugins/tr-cw78/thumb.png",
            "MPC/Force port of athousanddetails' schwung-CW-78 CR-78 drum machine: voices modelled from the 1979 service notes, with the preset rhythms. Touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/athousanddetails - VST - CW-78\" && ZIP_PATH=\"./plugins/tr-cw78/tr-cw78.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/cw78-vst-v1.3.1/CW-78-1.3.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/tr-cw78\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/cw78-vst-v1.3.1/CW-78-1.3.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/athousanddetails - VST - CW-78/\"; elif [ -f \"./plugins/tr-cw78/trcw78.so\" ]; then cp -rf ./plugins/tr-cw78/* \"$TARGET_SYNTH_DIR/athousanddetails - VST - CW-78/\"; else echo \"[ERROR] No local binary archive found for tr-cw78\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "cr-78" }
        },
        {
            "op1fx-dbox", "DBox", "Bombest Audio", "v0.6.0", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1808", "8.6 MB", "14%",
            "Bombest Audio - VST - DBox", "dbox.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/dbox.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/DBox-0.6.0-mpc-armv7.zip", "plugins/op1fx-dbox/thumb.png",
            "A 24-key synthesized drum kit: two oscillators and noise per hit, 12 voices. OP-1 inspired.", "./plugins/op1fx-dbox/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "drums", "synth", "kit" }
        },
        {
            "denis", "Denis", "saustin2010", "v1.1.1", "Mono Bass",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3177", "5.4 MB", "14%",
            "Filliformes - VST - [SYN] Denis", "denis.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-denis/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-denis/releases/download/v1.1.1/SYN-Denis-1.1.1-mpc-armv7.zip", "plugins/denis/thumb.png",
            "West Coast / Serge-inspired mono synth: complex oscillators, a wavefolder and a modulation matrix.", "./plugins/denis/install.sh", 0,
            0,
            false,
            false,
            "",
            { "west-coast", "serge", "wavefolder", "monophonic" }
        },
        {
            "dexed-dx7", "Dexed", "sd88me", "v2.0.5", "Fm",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-18",
            "14827", "7.1 MB", "14%",
            "sd88me - VST - Dexed (DX7)", "dx7_dexed.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-dx7/main/docs/img/dexed-dx7.png", "https://github.com/sd88me/mpc-vst-dx7/releases/download/dx7-v2.0.5/Dexed-DX7-2.0.5-mpc-armv7.zip", "plugins/dexed-dx7/thumb.png",
            "DX7-style 6-operator FM synth (Dexed/MSFA engine) as a native instrument, with .syx bank browsing, all 32 algorithm diagrams and a touchscreen skin with Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Dexed (DX7)\" && ZIP_PATH=\"./plugins/dexed-dx7/dexed-dx7.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-dx7/releases/download/dx7-v2.0.5/Dexed-DX7-2.0.5-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/dexed-dx7\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-dx7/releases/download/dx7-v2.0.5/Dexed-DX7-2.0.5-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Dexed (DX7)/\"; elif [ -f \"./plugins/dexed-dx7/dx7_dexed.so\" ]; then cp -rf ./plugins/dexed-dx7/* \"$TARGET_SYNTH_DIR/sd88me - VST - Dexed (DX7)/\"; else echo \"[ERROR] No local binary archive found for dexed-dx7\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "dx7", "emulation" }
        },
        {
            "op1fx-digital", "Digital", "Bombest Audio", "v0.6.0", "Macro Hybrid",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1739", "5.7 MB", "14%",
            "Bombest Audio - VST - Digital", "digital.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/digital.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Digital-0.6.0-mpc-armv7.zip", "plugins/op1fx-digital/thumb.png",
            "Feedback sine oscillator, a second sine with sync, and a bit-flipping XOR stage. Eight voices. OP-1 inspired.", "./plugins/op1fx-digital/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "synth", "feedback", "xor" }
        },
        {
            "dinxxl", "DINXXL", "L'Cronx", "v1.0.0", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "1267", "4.5 MB", "14%",
            "L'Cronx - VST - DINXXL", "dinxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/dinxxl/v1.0.0/docs/img/keys.png", "https://github.com/sunskiefer/DINXXL/releases/download/v1.0.0/DINXXL-1.0.0-mpc-armv7.zip", "plugins/dinxxl/thumb.png",
            "BlastBeats-style OPL3 FM groovebox: the published v7.1 firmware by Twisted Electrons runs unmodified on a software model of its board (YMF262 via ymfm). 6 drums, 4 four-op synths, 64-step sequencer with p-locks and songs, factory card inside. By L'Cronx; not made or endorsed by Twisted Electrons.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - DINXXL\" && ZIP_PATH=\"./plugins/dinxxl/dinxxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/DINXXL/releases/download/v1.0.0/DINXXL-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/dinxxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/DINXXL/releases/download/v1.0.0/DINXXL-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - DINXXL/\"; elif [ -f \"./plugins/dinxxl/dinxxl.so\" ]; then cp -rf ./plugins/dinxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - DINXXL/\"; else echo \"[ERROR] No local binary archive found for dinxxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "blastbeats", "opl3", "fm", "drum-machine", "groovebox", "step-sequencer", "chiptune", "adlib" }
        },
        {
            "op1fx-dna", "DNA", "Bombest Audio", "v0.6.0", "Wavetable",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "2002", "4.7 MB", "14%",
            "Bombest Audio - VST - DNA", "dna.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/dna.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/DNA-0.6.0-mpc-armv7.zip", "plugins/op1fx-dna/thumb.png",
            "A 128-bit genome played as a one-bit wave through an anti-aliased reader. Eight voices. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Bombest Audio - VST - DNA\" && ZIP_PATH=\"./plugins/op1fx-dna/op1fx-dna.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/DNA-0.6.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-dna\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/DNA-0.6.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Bombest Audio - VST - DNA/\"; elif [ -f \"./plugins/op1fx-dna/dna.so\" ]; then cp -rf ./plugins/op1fx-dna/* \"$TARGET_SYNTH_DIR/Bombest Audio - VST - DNA/\"; else echo \"[ERROR] No local binary archive found for op1fx-dna\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "synth", "1-bit", "wavetable" }
        },
        {
            "donut-theory", "Donut Theory", "kim", "v1.15.4", "Polysynth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-21",
            "801", "5.3 MB", "14%",
            "kim - VST - Donut Theory", "VST/donut-theory-arm32.so",
            "https://raw.githubusercontent.com/Sunghyun23/kimswork/donut-theory/v1.15.4/mpc-software/plugins/public/DonutTheory/design/device-1.15.1/actual-final.png", "https://github.com/Sunghyun23/kimswork/releases/download/donut-theory/v1.15.4/donut-theory-1.15.4-mpc-armv7.zip", "plugins/donut-theory/thumb.png",
            "Analog-inspired bass, keys, leads and pads with dual oscillators/LFOs and 16 chord slots. UI and current pad policy checked on MPC XL OS3.10.1.12; other models not tested for this release. Requires OS3.10.1+.", "./plugins/donut-theory/install.sh", 0,
            0,
            false,
            false,
            "",
            { "synth", "analog", "chords" }
        },
        {
            "downrush", "Downrush", "Jamie Fenton", "v0.1.0", "Polysynth",
            "INST", "Stable", "Force (3.10.1.12)", "MPC OS 3.x", "2026-11-20",
            "214", "2.6 MB", "14%",
            "Jamie Fenton - VST - Downrush", "VST/downrush-arm32.so",
            "https://raw.githubusercontent.com/jamiefaye/mpc-deluge/v0.1.0/screenshots/grid-editor.png", "https://github.com/jamiefaye/mpc-deluge/releases/download/v0.1.0/Downrush-0.1.0-mpc-armv7.zip", "plugins/downrush/thumb.png",
            "A polyphonic synth running the synth engine of the open-source Deluge community firmware 1.3.0: subtractive, FM and ring mod, sample oscillators, a 24-cable mod matrix, arpeggiator, and it loads and saves Deluge SYNTH preset files. Needs MPC OS 3.10.1 or later. Tested on a Force.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Jamie Fenton - VST - Downrush\" && ZIP_PATH=\"./plugins/downrush/downrush.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/jamiefaye/mpc-deluge/releases/download/v0.1.0/Downrush-0.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/downrush\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jamiefaye/mpc-deluge/releases/download/v0.1.0/Downrush-0.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Jamie Fenton - VST - Downrush/\"; elif [ -f \"./plugins/downrush/VST/downrush-arm32.so\" ]; then cp -rf ./plugins/downrush/* \"$TARGET_SYNTH_DIR/Jamie Fenton - VST - Downrush/\"; else echo \"[ERROR] No local binary archive found for downrush\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "fm", "polyphonic", "subtractive", "deluge" }
        },
        {
            "drloguexxl", "DrLogueXXL", "L'Cronx", "v0.2.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1972", "7.8 MB", "14%",
            "L'Cronx - VST - DrLogueXXL", "drloguexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/DrLogueXXL/v0.2.0/docs/img/perform.png", "https://github.com/sunskiefer/DrLogueXXL/releases/download/v0.2.1/DrLogueXXL-0.2.1-mpc-armv7.zip", "plugins/drloguexxl/thumb.png",
            "Hybrid drum machine modelled on the KORG drumlogue (not affiliated with KORG): analogue-style kick, snare and toms, six sample parts with 192 synthesised sounds and your own WAVs, a MULTI part with VPM, noise and six open logue SDK units, delay, reverb and master effects, choke groups, a mixer and four macros, on a panel in the machine's look.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - DrLogueXXL\" && ZIP_PATH=\"./plugins/drloguexxl/drloguexxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/DrLogueXXL/releases/download/v0.2.1/DrLogueXXL-0.2.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/drloguexxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/DrLogueXXL/releases/download/v0.2.1/DrLogueXXL-0.2.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - DrLogueXXL/\"; elif [ -f \"./plugins/drloguexxl/drloguexxl.so\" ]; then cp -rf ./plugins/drloguexxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - DrLogueXXL/\"; else echo \"[ERROR] No local binary archive found for drloguexxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "drums", "analog", "hybrid", "samples", "fm", "logue", "drumlogue", "mixer" }
        },
        {
            "dronexxl", "DroneXXL", "L'Cronx", "v0.1.0", "Experimental",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-25",
            "1780", "83.7 MB", "14%",
            "L'Cronx - VST - DroneXXL", "dronexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/DroneXXL/v0.1.0/docs/img/overview.png", "https://github.com/sunskiefer/DroneXXL/releases/download/v0.1.0/DroneXXL-0.1.0-mpc-armv7.zip", "plugins/dronexxl/thumb.png",
            "Drone and experimental synth, after the Solar 42, built on Neil Baumberger's AmbientModules: a 15-module rack (4 drones, mixer, filter, pads, 2 PapaSrapa, 2 VCOs, LFOs, joystick, sequencer, detector) patched by touch, with 8 macros, an XY pad, a randomizer that never repeats and a brickwall limiter.", "./plugins/dronexxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "drone", "experimental", "modular", "patch-panel", "ambient", "randomizer", "vcv-rack" }
        },
        {
            "op1fx-drwave", "DrWave", "Bombest Audio", "v0.6.0", "Wavetable",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1735", "6.7 MB", "14%",
            "Bombest Audio - VST - DrWave", "drwave.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/drwave.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/DrWave-0.6.0-mpc-armv7.zip", "plugins/op1fx-drwave/thumb.png",
            "Wave, blend, motion and fold controls over eight voices. OP-1 inspired.", "./plugins/op1fx-drwave/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "synth", "wavetable", "fold" }
        },
        {
            "op1fx-dsynth", "DSynth", "Bombest Audio", "v0.6.0", "Fm",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1809", "4.8 MB", "14%",
            "Bombest Audio - VST - DSynth", "dsynth.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/dsynth.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/DSynth-0.6.0-mpc-armv7.zip", "plugins/op1fx-dsynth/thumb.png",
            "Two oscillators with FM, a pitch drop for drum thump and an eight-mode filter. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Bombest Audio - VST - DSynth\" && ZIP_PATH=\"./plugins/op1fx-dsynth/op1fx-dsynth.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/DSynth-0.6.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-dsynth\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/DSynth-0.6.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Bombest Audio - VST - DSynth/\"; elif [ -f \"./plugins/op1fx-dsynth/dsynth.so\" ]; then cp -rf ./plugins/op1fx-dsynth/* \"$TARGET_SYNTH_DIR/Bombest Audio - VST - DSynth/\"; else echo \"[ERROR] No local binary archive found for op1fx-dsynth\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "synth", "fm", "drums" }
        },
        {
            "dub-force-siren", "Dub Force Siren", "no3z", "v2.1.0", "Experimental",
            "INST", "Stable", "Akai Force (3.9.1.2)", "MPC OS 3.x", "2026-10-26",
            "2700", "117 KB", "14%",
            "Dub Force - VST - Dub Force Siren", "dub_force_siren.so",
            "https://raw.githubusercontent.com/no3z/mpc-dubsiren/v2.0.0/resources/preview.png", "https://github.com/no3z/mpc-dubsiren/releases/download/v2.1.0/Dub-Force-Siren-2.1.0-mpc-armv7.zip", "plugins/dub-force-siren/thumb.png",
            "Dub siren with tape echo: siren and zap modes, LFO, filter and ping-pong delay on one page mirroring two banks of eight Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Dub Force - VST - Dub Force Siren\" && ZIP_PATH=\"./plugins/dub-force-siren/dub-force-siren.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/no3z/mpc-dubsiren/releases/download/v2.1.0/Dub-Force-Siren-2.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/dub-force-siren\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/no3z/mpc-dubsiren/releases/download/v2.1.0/Dub-Force-Siren-2.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Dub Force - VST - Dub Force Siren/\"; elif [ -f \"./plugins/dub-force-siren/dub_force_siren.so\" ]; then cp -rf ./plugins/dub-force-siren/* \"$TARGET_SYNTH_DIR/Dub Force - VST - Dub Force Siren/\"; else echo \"[ERROR] No local binary archive found for dub-force-siren\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "dub", "siren", "echo", "sfx" }
        },
        {
            "edrum-kit", "EDrum Kit", "burgerbarn", "v1.0.0", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-14",
            "674", "903 KB", "14%",
            "burgerbarn - VST - EDrum Kit", "VST/edrum-kit-arm32.so",
            "https://raw.githubusercontent.com/burgerbarn/edrum-kit-mpc/f3886f20a42ea057c3db682d4da8306c667217e2/docs/card.png", "https://github.com/burgerbarn/edrum-kit-mpc/releases/download/v1.0.0/EDrum-Kit-1.0.0-mpc-armv7.zip", "plugins/edrum-kit/thumb.png",
            "Plays the MPC's own drum kits from Roland V-Drums (TD-27): auto-mapped zones, continuous VH-14D hi-hat, positional snare and ride, cymbal chokes, velocity curves, per-drum tune/decay/tone.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/burgerbarn - VST - EDrum Kit\" && ZIP_PATH=\"./plugins/edrum-kit/edrum-kit.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/burgerbarn/edrum-kit-mpc/releases/download/v1.0.0/EDrum-Kit-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/edrum-kit\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/burgerbarn/edrum-kit-mpc/releases/download/v1.0.0/EDrum-Kit-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/burgerbarn - VST - EDrum Kit/\"; elif [ -f \"./plugins/edrum-kit/VST/edrum-kit-arm32.so\" ]; then cp -rf ./plugins/edrum-kit/* \"$TARGET_SYNTH_DIR/burgerbarn - VST - EDrum Kit/\"; else echo \"[ERROR] No local binary archive found for edrum-kit\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "drums", "e-drums", "td-27", "midi", "sampler" }
        },
        {
            "esque80", "ESQUE80", "Lichthaus", "v0.9.1", "Wavetable",
            "INST", "Stable", "MPC Live II (3.10.1), MPC Key 37 (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-16",
            "659", "512 KB", "14%",
            "Lichthaus - VST - ESQUE80", "VST/esque80-arm32.so",
            "https://raw.githubusercontent.com/lichthaus/mpc-vst-esque80/v0.9.1/docs/screenshots/esque80-card.png", "https://github.com/lichthaus/mpc-vst-esque80/releases/download/v0.9.1/ESQUE80-0.9.1-mpc-armv7.zip", "plugins/esque80/thumb.png",
            "An 8-voice cross-wave synthesizer modelled on the Ensoniq SQ-80: three wavetable oscillators per voice, a 4-pole low-pass filter, four envelopes with touch-editable displays, three LFOs and a 15-source mod matrix, on an eleven-page skin with Q-Link banks per panel. Plays on built-in stand-in waves; add your own wave ROM for the original sound.", "./plugins/esque80/install.sh", 0,
            0,
            false,
            false,
            "",
            { "emulation", "wavetable", "sq-80" }
        },
        {
            "evil-tuner-xxl", "Evil Tuner XXL", "L'Cronx", "v0.3.0", "Experimental",
            "INST", "Beta", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-16",
            "3889", "14.9 MB", "14%",
            "L'Cronx - VST - Evil Tuner XXL", "evil_tuner_xxl.so",
            "https://raw.githubusercontent.com/sunskiefer/Evil-Tuner-XXL/v0.3.0/docs/img/RADIO.png", "https://github.com/sunskiefer/Evil-Tuner-XXL/releases/download/v0.3.0/Evil-Tuner-XXL-0.3.0-mpc-armv7.zip", "plugins/evil-tuner-xxl/thumb.png",
            "Internet radio (30,000+ stations), or a phone over Wi-Fi or AirPlay, played into an MPC track through a granular engine, filter, character stages, delay, reverb, trance gate, stutter, tempo-synced looper and three LFOs. Inspired by the Endorphin.es Evil Pet (no affiliation, no code). Needs a network. 32-bit and 64-bit MPCs.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - Evil Tuner XXL\" && ZIP_PATH=\"./plugins/evil-tuner-xxl/evil-tuner-xxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/Evil-Tuner-XXL/releases/download/v0.3.0/Evil-Tuner-XXL-0.3.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/evil-tuner-xxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/Evil-Tuner-XXL/releases/download/v0.3.0/Evil-Tuner-XXL-0.3.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - Evil Tuner XXL/\"; elif [ -f \"./plugins/evil-tuner-xxl/evil_tuner_xxl.so\" ]; then cp -rf ./plugins/evil-tuner-xxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - Evil Tuner XXL/\"; else echo \"[ERROR] No local binary archive found for evil-tuner-xxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "radio", "internet-radio", "granular", "airplay", "looper", "stutter", "reverb", "sampling" }
        },
        {
            "f4-phantom", "F4 Phantom", "Lumina Aerospace LLC", "v1.2.0", "Polysynth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-18",
            "624", "1.9 MB", "14%",
            "Lumina Aerospace LLC - VST - F4 Phantom", "VST/f4-phantom-arm32.so",
            "https://raw.githubusercontent.com/Luminous-Works/mpc-vst-f4-phantom/v1.1.0/docs/screenshots/f4-phantom.png", "https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/f4-phantom-1.2.0-763286342-mpc-armv7.zip", "plugins/f4-phantom/thumb.png",
            "8-voice subtractive synth: two oscillators plus sub, SVF filter, drift, stereo chorus and a six-barrel rotary gate. 14 sounds.", "./plugins/f4-phantom/install.sh", 0,
            0,
            false,
            false,
            "",
            { "subtractive", "polyphonic", "analog", "chorus", "rotary" }
        },
        {
            "felt-club", "Felt Club", "kim", "v0.3.2", "Physical Modeling",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-21",
            "186", "10.4 MB", "14%",
            "kim - VST - Felt Club", "VST/felt-club-arm32.so",
            "https://raw.githubusercontent.com/Sunghyun23/kimswork/felt-club/v0.3.2/mpc-software/plugins/public/FeltClub/design/device-0.3.2/main.png", "https://github.com/Sunghyun23/kimswork/releases/download/felt-club/v0.3.2/felt-club-0.3.2-mpc-armv7.zip", "plugins/felt-club/thumb.png",
            "Seven sample-free physical keys and plucked engines with musical Dice and 57 sounds. UI checked on MPC XL OS3.10.1.12. Owner reports CPU peaks above100%; heavy playing can overload. Requires MPC OS3.10.1+.", "./plugins/felt-club/install.sh", 0,
            0,
            false,
            false,
            "",
            { "keys", "physical-modeling", "neo-soul" }
        },
        {
            "fizzik", "Fizzik", "saustin2010", "v1.0.2", "Physical Modeling",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2948", "1.8 MB", "14%",
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
            "op1fx-fm", "Fm", "Bombest Audio", "v0.6.0", "Fm",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1749", "8.3 MB", "14%",
            "Bombest Audio - VST - Fm", "fm.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/fm.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Fm-0.6.0-mpc-armv7.zip", "plugins/op1fx-fm/thumb.png",
            "Four-operator FM synth with eight voices and four screen knobs. OP-1 inspired.", "./plugins/op1fx-fm/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "synth", "fm", "operators" }
        },
        {
            "fretty", "Fretty", "3dthc", "v0.8.7", "Sampler Rompler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-17",
            "5392", "70.4 MB", "14%",
            "3dthc - VST - Fretty", "fretty.so",
            "https://raw.githubusercontent.com/3dthc/fretty-mpc/main/docs/images/play_sunburst.png", "https://github.com/3dthc/fretty-mpc/releases/download/v0.8.7/Fretty-0.8.7-mpc-armv7.zip", "plugins/fretty/thumb.png",
            "A guitar instrument with an animated fretboard: acoustic, electric, bass, harp, violin, zither and folk sounds with a synth layer, chorus and distortion. 110 presets with a browser page by category. Also a VST3 for Mac and Windows and an Audio Unit for Mac.", "./plugins/fretty/install.sh", 0,
            0,
            false,
            false,
            "",
            {  }
        },
        {
            "frodhes", "Frodhes", "leonardoMirabal", "v0.9.13", "Keys Organ",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-17",
            "1024", "9.0 MB", "14%",
            "Frodhes - VST - Frodhes", "frodhes-arm32.so",
            "https://raw.githubusercontent.com/leonardoMirabal/frodhes/main/docs/manual/img/mpc/mpc-catalog.png", "https://github.com/leonardoMirabal/frodhes/releases/download/v0.9.13/Frodhes-0.9.13-mpc-armv7.zip", "plugins/frodhes/thumb.png",
            "A physically modelled electric piano, vibraphone and marimba, with no samples: 16 voices, 57 presets, drive, chorus, phaser, tremolo, wah, delay, reverb, EQ and a compressor.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Frodhes - VST - Frodhes\" && ZIP_PATH=\"./plugins/frodhes/frodhes.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/leonardoMirabal/frodhes/releases/download/v0.9.13/Frodhes-0.9.13-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/frodhes\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/leonardoMirabal/frodhes/releases/download/v0.9.13/Frodhes-0.9.13-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Frodhes - VST - Frodhes/\"; elif [ -f \"./plugins/frodhes/frodhes-arm32.so\" ]; then cp -rf ./plugins/frodhes/* \"$TARGET_SYNTH_DIR/Frodhes - VST - Frodhes/\"; else echo \"[ERROR] No local binary archive found for frodhes\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "physical-modelling", "electric-piano", "keys", "vibraphone", "marimba", "polyphonic", "delay", "reverb" }
        },
        {
            "gaiaxxl", "GAIAXXL", "L'Cronx", "v0.1.3", "Polysynth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "3302", "7.7 MB", "14%",
            "L'Cronx - VST - GAIAXXL", "gaiaxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/GAIAXXL/v0.1.2/docs/img/tone.png", "https://github.com/sunskiefer/GAIAXXL/releases/download/v0.1.3/GAIAXXL-0.1.3-mpc-armv7.zip", "plugins/gaiaxxl/thumb.png",
            "Three-tone virtual-analog synth modelled on the discontinued Roland GAIA SH-01 (not affiliated with Roland): the editor's parameters over six full-screen pages, 21 oscillator variations (with KORG logue-sdk wave tables), SYNC/RING, the GAIA's effects chain and arpeggiator, two beams, a musical randomizer and 64 patches.", "./plugins/gaiaxxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "synth", "virtual-analog", "supersaw", "gaia", "arpeggiator", "pad", "lead", "randomizer" }
        },
        {
            "geonkick-mpc", "GeonKick", "303inmyheart", "v1.0.0", "Drum Machine",
            "INST", "Stable", "MPC Live III (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-22",
            "55", "9.3 MB", "1%",
            "OpenMPC - VST - Geonkick MPC", "geonkick_mpc.so",
            "https://raw.githubusercontent.com/303inmyheart/OpenMPC-Geonkick/geonkick-mpc-v1.0.0/docs/screenshots/synth.png", "https://github.com/303inmyheart/OpenMPC-Geonkick/releases/download/geonkick-mpc-v1.0.0/Geonkick-MPC-1.0.0-mpc-aarch64.zip", "plugins/geonkick-mpc/thumb.png",
            "Gen2 MPC percussion synth with 16 pad sounds, nine oscillators per sound, touch-editable envelopes, WAV import and preview, and 118 native presets/kits. Hardware tested on Live III; MPC OS 3.10.1 or newer.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/OpenMPC - VST - Geonkick MPC\" && ZIP_PATH=\"./plugins/geonkick-mpc/geonkick-mpc.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/303inmyheart/OpenMPC-Geonkick/releases/download/geonkick-mpc-v1.0.0/Geonkick-MPC-1.0.0-mpc-aarch64.zip\" ]; then mkdir -p \"./plugins/geonkick-mpc\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/303inmyheart/OpenMPC-Geonkick/releases/download/geonkick-mpc-v1.0.0/Geonkick-MPC-1.0.0-mpc-aarch64.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/OpenMPC - VST - Geonkick MPC/\"; elif [ -f \"./plugins/geonkick-mpc/geonkick_mpc.so\" ]; then cp -rf ./plugins/geonkick-mpc/* \"$TARGET_SYNTH_DIR/OpenMPC - VST - Geonkick MPC/\"; else echo \"[ERROR] No local binary archive found for geonkick-mpc\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "drums", "percussion", "synth", "sampler", "gen2" }
        },
        {
            "grainology", "Grainology", "Stephane Schmitz", "v1.0.1", "Experimental",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-14",
            "996", "4.7 MB", "14%",
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
            "groove-dice", "Groove Dice", "kim", "v1.11.0", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-21",
            "551", "17.6 MB", "14%",
            "kim - VST - Groove Dice", "VST/groove-dice-arm32.so",
            "https://raw.githubusercontent.com/Sunghyun23/kimswork/groove-dice/v1.11.0/mpc-software/plugins/public/GrooveDice/design/device-1.9.0/public-main.png", "https://github.com/Sunghyun23/kimswork/releases/download/groove-dice/v1.11.0/groove-dice-1.11.0-mpc-armv7.zip", "plugins/groove-dice/thumb.png",
            "Neo-soul/hip-hop drums with one-bar grooves, six fill/roll pads, original sounds and expansion packs. UI/pad policy checked on MPC XL OS3.10.1.12; other models untested for this release. Requires OS3.10.1+.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/kim - VST - Groove Dice\" && ZIP_PATH=\"./plugins/groove-dice/groove-dice.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Sunghyun23/kimswork/releases/download/groove-dice/v1.11.0/groove-dice-1.11.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/groove-dice\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Sunghyun23/kimswork/releases/download/groove-dice/v1.11.0/groove-dice-1.11.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/kim - VST - Groove Dice/\"; elif [ -f \"./plugins/groove-dice/VST/groove-dice-arm32.so\" ]; then cp -rf ./plugins/groove-dice/* \"$TARGET_SYNTH_DIR/kim - VST - Groove Dice/\"; else echo \"[ERROR] No local binary archive found for groove-dice\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "drums", "groove", "sampler" }
        },
        {
            "hank", "Hank", "saustin2010", "v1.1.1", "Fm",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2528", "2.1 MB", "14%",
            "Charles Vestal - VST - [SYN] Hank", "hank.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-hank/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-hank/releases/download/v1.1.1/SYN-Hank-1.1.1-mpc-armv7.zip", "plugins/hank/thumb.png",
            "Small, immediate two-operator FM synth on one page: ratio, brightness, bite, envelopes, noise; 32 presets.", "./plugins/hank/install.sh", 0,
            0,
            false,
            false,
            "",
            { "fm", "two-operator", "polyphonic", "bass" }
        },
        {
            "helm", "Helm", "Lewinator56", "v1.0.0", "Polysynth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-04",
            "2451", "12.3 MB", "14%",
            "Matt Tytel - VST - Helm", "helm.so",
            "https://github.com/user-attachments/assets/b537127a-22e6-4e21-b9f1-48e473c892b8", "https://github.com/Lewinator56/mpc-vst-helm/releases/download/1.0.0/Helm-1.0.0-mpc-armv7.zip", "plugins/helm/thumb.png",
            "32-voice polyphonic synthesizer (Helm engine by Matt Tytel) with 2 cross-mod oscillators, sub, noise, multi-mode filter, 3 LFOs, 3 envelopes, 32-step sequencer & arpeggiator, built-in effects and touchscreen skin with Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Matt Tytel - VST - Helm\" && ZIP_PATH=\"./plugins/helm/helm.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Lewinator56/mpc-vst-helm/releases/download/1.0.0/Helm-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/helm\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Lewinator56/mpc-vst-helm/releases/download/1.0.0/Helm-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Matt Tytel - VST - Helm/\"; elif [ -f \"./plugins/helm/helm.so\" ]; then cp -rf ./plugins/helm/* \"$TARGET_SYNTH_DIR/Matt Tytel - VST - Helm/\"; else echo \"[ERROR] No local binary archive found for helm\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "polyphonic", "subtractive", "arpeggiator", "step-sequencer", "mod-matrix" }
        },
        {
            "hera", "Hera", "saustin2010", "v1.1.2", "Polysynth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-24",
            "5035", "3.3 MB", "14%",
            "jpcima - VST - [SYN] Hera", "hera.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-hera/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-hera/releases/download/v1.1.2/SYN-Hera-1.1.2-mpc-armv7.zip", "plugins/hera/thumb.png",
            "Roland Juno-60: jpcima's Hera engine with its 56 presets, the BBD chorus and a touchscreen skin with Q-Links.", "./plugins/hera/install.sh", 0,
            0,
            false,
            false,
            "",
            { "juno-60", "polyphonic", "chorus", "analog-modelling" }
        },
        {
            "hexachord-73", "HexChord 73", "Eurikon", "v1.1.0", "Polysynth",
            "INST", "Stable", "MPC Live II Retro (3.10.1)", "MPC OS 3.x", "2026-11-14",
            "2613", "3.0 MB", "14%",
            "Twisted Cable - VST - HexChord 73", "hexachord-73.so",
            "https://raw.githubusercontent.com/Eurikon/mpc-vst-plugins/9eed16e63f97e5ad87621a21cead85b6a7fee35b/media/hexachord-73.png", "https://github.com/Eurikon/mpc-vst-plugins/releases/download/hexachord-73-v1.1.0/HexChord-73-1.1.0-mpc-armv7.zip", "plugins/hexachord-73/thumb.png",
            "Stereo chord synthesizer with four engines, eight chords, four voicings and a touchscreen chord diagram.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Twisted Cable - VST - HexChord 73\" && ZIP_PATH=\"./plugins/hexachord-73/hexachord-73.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Eurikon/mpc-vst-plugins/releases/download/hexachord-73-v1.1.0/HexChord-73-1.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/hexachord-73\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Eurikon/mpc-vst-plugins/releases/download/hexachord-73-v1.1.0/HexChord-73-1.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Twisted Cable - VST - HexChord 73/\"; elif [ -f \"./plugins/hexachord-73/hexachord-73.so\" ]; then cp -rf ./plugins/hexachord-73/* \"$TARGET_SYNTH_DIR/Twisted Cable - VST - HexChord 73/\"; else echo \"[ERROR] No local binary archive found for hexachord-73\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "chord", "polyphonic", "stereo" }
        },
        {
            "incipience", "Incipience", "saustin2010", "v1.1.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2347", "7.0 MB", "14%",
            "Steve A - VST - [DRUM] Incipience", "incipience.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-incipience/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-incipience/releases/download/v1.1.1/DRUM-Incipience-1.1.1-mpc-armv7.zip", "plugins/incipience/thumb.png",
            "Eleven-instrument drum synth laid out like the Jomox AlphaBase Mk II (kick, MBrane, hats, clap, rim, cymbals, two sample players, 4-op FM, reverb, delay), with RANDOMISE per instrument and a mod matrix; each instrument chromatic on its own MIDI channel and on its own track with Incipience OUT; plays the AlphaBase's free samples and your own WAVs.", "./plugins/incipience/install.sh", 0,
            0,
            false,
            false,
            "",
            { "drums", "alphabase", "fm", "samples", "multi-out", "mod-matrix" }
        },
        {
            "interzonexxl", "InterzoneXXL", "L'Cronx", "v0.1.3", "Macro Hybrid",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "3560", "13.6 MB", "14%",
            "L'Cronx - VST - InterzoneXXL", "interzonexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/InterzoneXXL/v0.1.0/docs/img/vco.png", "https://github.com/sunskiefer/InterzoneXXL/releases/download/v0.1.3/InterzoneXXL-0.1.3-mpc-armv7.zip", "plugins/interzonexxl/thumb.png",
            "Valley Audio's Interzone synth voice from VCV Rack, played from MIDI in mono or up to 16-voice poly on the module's own panel. Every input jack is patched to a built-in source (LFOs, sequencers, random) on the MPC tempo, with a brickwall limiter and 16 presets.", "./plugins/interzonexxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "synth", "monosynth", "polysynth", "analog", "vcv", "valley", "lfo", "modulation" }
        },
        {
            "jamiro", "Jamiro", "Yann", "v1.0.8", "Polysynth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1441", "699 KB", "14%",
            "Yann - VST - Jamiro", "VST/jamiro-arm32.so",
            "https://raw.githubusercontent.com/Yan-1977/jamiro-mpc/2ce0f3aa6da8e7364c89151e95d3afe4a3c146ad/docs/jamiro-screen.png", "https://github.com/Yan-1977/jamiro-mpc/releases/download/jamiro-v1.0.8/Jamiro-1.0.8-mpc-armv7.zip", "plugins/jamiro/thumb.png",
            "Novation X-Station-style polysynth (KS engine): 8 voices, 3 oscillators with sync, FM and ring mod, multimode filter, 2 envelopes, 2 LFOs, arpeggiator and effects. Loads your own X-Station/KS .syx banks from Content/Banks. Independent project, not affiliated with Novation.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Yann - VST - Jamiro\" && ZIP_PATH=\"./plugins/jamiro/jamiro.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Yan-1977/jamiro-mpc/releases/download/jamiro-v1.0.8/Jamiro-1.0.8-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/jamiro\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Yan-1977/jamiro-mpc/releases/download/jamiro-v1.0.8/Jamiro-1.0.8-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Yann - VST - Jamiro/\"; elif [ -f \"./plugins/jamiro/VST/jamiro-arm32.so\" ]; then cp -rf ./plugins/jamiro/* \"$TARGET_SYNTH_DIR/Yann - VST - Jamiro/\"; else echo \"[ERROR] No local binary archive found for jamiro\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "virtual-analog", "polysynth", "arpeggiator", "x-station" }
        },
        {
            "jc303-mpc", "JC303", "303inmyheart", "v0.7.4", "Mono Bass",
            "INST", "Stable", "MPC Live III (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-15",
            "1117", "7.3 MB", "35%",
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
            "je8086", "JE-8086", "sd88me", "v1.0.0", "Polysynth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-15",
            "959", "14.1 MB", "14%",
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
            "j106", "JN-106", "sd88me", "v0.2.0", "Polysynth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1), Akai MPC Live III (aarch64) (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-28",
            "3347", "25.3 MB", "14%",
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
            "ju-08", "Ju-08", "Stephane Schmitz", "v1.0.0", "Polysynth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-14",
            "1332", "2.2 MB", "14%",
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
            "2447", "68.7 MB", "14%",
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
            "lucky-dip", "Lucky Dip", "sd88me", "v1.0.0", "Sampler Rompler",
            "INST", "Stable", "MPC Live III (Gen2) (3.9), Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-11-12",
            "4198", "2.2 MB", "14%",
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
            "lx-800", "LX-800", "Lightlike", "v1.1.0", "Polysynth",
            "INST", "Stable", "MPC One (3.10.1)", "MPC OS 3.x", "2026-11-18",
            "464", "1.1 MB", "14%",
            "Lightlike - VST - LX-800", "VST/lx-800-arm32.so",
            "https://raw.githubusercontent.com/lightlikemusic/lx-800/v1.1.0/docs/screenshot.png", "https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/lx-800-1.1.0-765462063-mpc-armv7.zip", "plugins/lx-800/thumb.png",
            "An 8-voice paraphonic synth in the spirit of early-80s budget polysynths: divide-down staircase oscillators, one shared resonant 24/12 dB filter with its own envelope, six-stage rate envelopes, delayed LFO, stereo chorus and a chord step sequencer synced to the MPC. 63 presets and a programmer-chart screen with live LED readouts.", "./plugins/lx-800/install.sh", 0,
            0,
            false,
            false,
            "",
            { "paraphonic", "polysynth", "vintage", "analog-modelling", "chorus", "step-sequencer" }
        },
        {
            "lxrxxl", "LXRXXL", "L'Cronx", "v0.6.1", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "8374", "10.2 MB", "14%",
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
            "lxrxxl-bazzline", "LXRXXL BAZZLINE", "L'Cronx", "v0.2.10", "Groovebox",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-22",
            "8328", "16.2 MB", "14%",
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
            "lxrxxl-zynth", "LXRXXL ZYNTH", "L'Cronx", "v0.2.2", "Groovebox",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-22",
            "3590", "22.4 MB", "14%",
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
            "m-one", "M-One", "Fullban", "v1.3.0", "Sampler Rompler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-15",
            "923", "5.4 MB", "14%",
            "Fullban - VST - M-One", "m-one.so",
            "https://raw.githubusercontent.com/fullbann/mpc-vst-m-one/3671aa20119840df2ed7d3ac492705adf6932c6f/docs/catalog-thumbnail.png", "https://github.com/fullbann/mpc-vst-m-one/releases/download/v1.3.0/M-One-1.3.0-mpc-armv7.zip", "plugins/m-one/thumb.png",
            "Korg M1-style ROM workstation built on the PicoFaceM1 engine: 100 programs, 100 combinations and the M1's 33 effects from your own M1 ROM set (none included), quick-edit knobs, an FX page for both effects and a Library of SysEx banks with ten original Fullban sounds.", "./plugins/m-one/install.sh", 0,
            0,
            false,
            false,
            "",
            { "rompler", "workstation", "m1", "korg", "90s", "house" }
        },
        {
            "machinemodule", "Machinemodule", "sd88me", "v1.0.5", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-11",
            "100", "2.0 MB", "14%",
            "sd88me - VST - Machinemodule", "machinemodule.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-machinedrum/main/docs/img/machinemodule.png", "", "plugins/machinemodule/thumb.png",
            "The Elektron Machinedrum SPS-1 UW sound engine as a native instrument: all 16 tracks from one plugin instance, with its own touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Machinemodule\" && ZIP_PATH=\"./plugins/machinemodule/machinemodule.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"\" ]; then mkdir -p \"./plugins/machinemodule\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Machinemodule/\"; elif [ -f \"./plugins/machinemodule/machinemodule.so\" ]; then cp -rf ./plugins/machinemodule/* \"$TARGET_SYNTH_DIR/sd88me - VST - Machinemodule/\"; else echo \"[ERROR] No local binary archive found for machinemodule\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "elektron", "machinedrum", "firmware-derived" }
        },
        {
            "machinemodule-gen2", "Machinemodule (Gen2)", "sd88me", "v1.0.5", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-11",
            "3469", "7.7 MB", "14%",
            "sd88me - VST - Machinemodule", "machinedrum_one.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-machinedrum/main/docs/img/machinemodule-gen2.png", "https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.5/Machinemodule-1.0.5-mpc-aarch64.zip", "plugins/machinemodule-gen2/thumb.png",
            "The Elektron Machinedrum UW sound engine as an instrument for Gen2 MPC devices: 16 tracks in one plugin, touchscreen skin, bank and kit lists, Q-Links. A ready-made download with nothing of Elektron's in it: you add your own OS file (and optionally your flash image) in the plugin folder. Gen2 only.", "./plugins/machinemodule-gen2/install.sh", 0,
            0,
            false,
            false,
            "",
            { "elektron", "machinedrum", "gen2" }
        },
        {
            "braids", "Macro Osc", "saustin2010", "v1.1.1", "Macro Hybrid",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "4104", "8.0 MB", "14%",
            "Émilie Gillet - VST - [SYN] Macro Osc", "braids.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-macro-osc/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-macro-osc/releases/download/v1.1.1/SYN-Macro-Osc-1.1.1-mpc-armv7.zip", "plugins/braids/thumb.png",
            "Macro Osc: a macro oscillator with 47 synthesis models, from Émilie Gillet's open-source Braids.", "./plugins/braids/install.sh", 0,
            0,
            false,
            false,
            "",
            { "macro-oscillator", "eurorack" }
        },
        {
            "maschine-group", "Maschine Group", "mmiroshnikov", "v1.0.0", "Sampler Rompler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-13",
            "1404", "169 KB", "14%",
            "mmiroshnikov - VST - Maschine Group", "maschine_group.so",
            "https://raw.githubusercontent.com/mmiroshnikov/mpc-maschine-group/main/docs/group.png", "https://github.com/mmiroshnikov/mpc-maschine-group/releases/download/maschine-group-v1.0.0/Maschine-Group-1.0.0-mpc-armv7.zip", "plugins/maschine-group/thumb.png",
            "Opens a Native Instruments Maschine 2 drum group (.mxgrp) and plays its pads and stored patterns, with a three-page touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/mmiroshnikov - VST - Maschine Group\" && ZIP_PATH=\"./plugins/maschine-group/maschine-group.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mmiroshnikov/mpc-maschine-group/releases/download/maschine-group-v1.0.0/Maschine-Group-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/maschine-group\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mmiroshnikov/mpc-maschine-group/releases/download/maschine-group-v1.0.0/Maschine-Group-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/mmiroshnikov - VST - Maschine Group/\"; elif [ -f \"./plugins/maschine-group/maschine_group.so\" ]; then cp -rf ./plugins/maschine-group/* \"$TARGET_SYNTH_DIR/mmiroshnikov - VST - Maschine Group/\"; else echo \"[ERROR] No local binary archive found for maschine-group\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "drums", "maschine", "sampler", "patterns" }
        },
        {
            "maze-voice", "Maze Voice", "sd88me", "v2.0.0", "Mono Bass",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-14",
            "6888", "6.4 MB", "14%",
            "sd88me - VST - Maze Voice", "maze_voice.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-maze/main/docs/img/maze-voice.png", "https://github.com/sd88me/mpc-vst-maze/releases/download/maze-voice-vst-v2.0.0/Maze-Voice-2.0.0-mpc-armv7.zip", "plugins/maze-voice/thumb.png",
            "Monophonic Moog Labyrinth-style thru-zero oscillator, wavefolder and state-variable filter voice with two LFOs, a page randomiser and a three-page touchscreen skin with Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Maze Voice\" && ZIP_PATH=\"./plugins/maze-voice/maze-voice.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-maze/releases/download/maze-voice-vst-v2.0.0/Maze-Voice-2.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/maze-voice\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-maze/releases/download/maze-voice-vst-v2.0.0/Maze-Voice-2.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Maze Voice/\"; elif [ -f \"./plugins/maze-voice/maze_voice.so\" ]; then cp -rf ./plugins/maze-voice/* \"$TARGET_SYNTH_DIR/sd88me - VST - Maze Voice/\"; else echo \"[ERROR] No local binary archive found for maze-voice\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "monophonic", "wavefolder", "moog", "analog-modelling" }
        },
        {
            "meowsynth", "MeowSynth", "mcardonaserrano", "v1.0.2", "Experimental",
            "INST", "Stable", "MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-22",
            "2715", "4.5 MB", "14%",
            "knobster - VST - MeowSynth", "meowsynth.so",
            "https://raw.githubusercontent.com/mcardonaserrano/meowsynth-mpc/main/previews/meow_page_meowing.png", "https://github.com/mcardonaserrano/meowsynth-mpc/releases/download/v1.0.2/MeowSynth-1.0.2-mpc-armv7.zip", "plugins/meowsynth/thumb.png",
            "knobster's MeowSynth rebuilt natively from the original's SynthEdit patch: a ramp oscillator through an envelope-following resonant filter that meows, five cats, vibrato and chorus, with the original's look and a meowing cat on the touchscreen and Q-Links.", "./plugins/meowsynth/install.sh", 0,
            0,
            false,
            false,
            "",
            { "cat", "meow", "novelty", "synthedit" }
        },
        {
            "meta4mpcxxl", "Meta4MPCXXL", "L'Cronx", "v0.1.2", "Experimental",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1325", "11.2 MB", "14%",
            "L'Cronx - VST - Meta4MPCXXL", "meta4mpcxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/Meta4MPCXXL/v0.1.1/docs/img/meta4mpcxxl.png", "https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-0.1.2-mpc-armv7.zip", "plugins/meta4mpcxxl/thumb.png",
            "Plays 4ms MetaModule-format patches (VCV Rack patches exported with the MetaModule hub) with 4ms's own open-source patch player and 215 built-in modules: every parameter on Q-Link pages, MIDI and the Force's clock into the patch, pads and keys to the hub's gate/CV inputs, patch outs 3-8 as MIDI for a CV track, and Wi-Fi patch upload from VCV Rack.", "./plugins/meta4mpcxxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "modular", "vcv", "metamodule", "patch-player", "eurorack", "generative", "cv" }
        },
        {
            "midi-transport", "MIDI Transport", "Elsewave", "v1.0.1", "Utility",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-21",
            "78", "253 KB", "14%",
            "Elsewave - VST - MIDI Transport", "VST/midi-transport-arm32.so",
            "https://raw.githubusercontent.com/shhQuiet/MPCMidiTransport/v1.0.0/docs/screenshots/control.png", "https://github.com/shhQuiet/MPCMidiTransport/releases/download/v1.0.1/MIDI-Transport-1.0.1-mpc-armv7.zip", "plugins/midi-transport/thumb.png",
            "Map MIDI notes or CCs to MPC transport and hardware buttons with per-project MIDI Learn and Momentary or Toggle modes. Requires Open MPC Remote. Tested on an original MPC X.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Elsewave - VST - MIDI Transport\" && ZIP_PATH=\"./plugins/midi-transport/midi-transport.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/shhQuiet/MPCMidiTransport/releases/download/v1.0.1/MIDI-Transport-1.0.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/midi-transport\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/shhQuiet/MPCMidiTransport/releases/download/v1.0.1/MIDI-Transport-1.0.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Elsewave - VST - MIDI Transport/\"; elif [ -f \"./plugins/midi-transport/VST/midi-transport-arm32.so\" ]; then cp -rf ./plugins/midi-transport/* \"$TARGET_SYNTH_DIR/Elsewave - VST - MIDI Transport/\"; else echo \"[ERROR] No local binary archive found for midi-transport\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "midi", "transport", "controller" }
        },
        {
            "jv-880", "Mini-JV", "sd88me", "v2.0.4", "Sampler Rompler",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-22",
            "14059", "4.6 MB", "14%",
            "sd88me - VST - JV-880", "jv880.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-jv880/a38d32cbbe90dc8b209a392a0835236acdd8d95f/docs/screenshots/jv880.png", "https://github.com/sd88me/mpc-vst-jv880/releases/download/v2.0.4/JV-880-2.0.4-mpc-armv7.zip", "plugins/jv-880/thumb.png",
            "An emulation of the Roland JV-880 sample-based sound engine as a native instrument, with the original reverb and chorus, a Play page with eight macros and a touchscreen skin.", "./plugins/jv-880/install.sh", 0,
            0,
            false,
            false,
            "",
            { "roland", "emulation" }
        },
        {
            "elements", "Modal Voice", "saustin2010", "v1.2.1", "Physical Modeling",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3993", "1.2 MB", "14%",
            "Émilie Gillet - VST - [SYN] Modal Voice", "elements.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-modal-voice/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-modal-voice/releases/download/v1.2.1/SYN-Modal-Voice-1.2.1-mpc-armv7.zip", "plugins/elements/thumb.png",
            "Modal Voice: a modal synthesis voice you bow, blow and strike, with 12 presets, from Émilie Gillet's open-source Elements.", "./plugins/elements/install.sh", 0,
            0,
            false,
            false,
            "",
            { "modal", "physical-modelling", "eurorack" }
        },
        {
            "model-72", "Model-72", "sd88me", "v2.0.0", "Mono Bass",
            "INST", "Stable", "Akai MPC Live III (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-18",
            "4232", "20.1 MB", "14%",
            "sd88me - VST - Model-72", "model72.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-model72/main/docs/img/mixer_filter.png", "https://github.com/sd88me/mpc-vst-model72/releases/download/model72-v2.0.0/Model-72-2.0.0-mpc-armv7.zip", "plugins/model-72/thumb.png",
            "A port of Idle Foundry's CA-72, a circuit-modelled 1970s Minimoog Model-D: three oscillators, the ladder filter and contours, mono, poly or unison with stereo and 19 presets. LO is the fitted light model; HI runs the circuit's own filter in real time.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Model-72\" && ZIP_PATH=\"./plugins/model-72/model-72.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-model72/releases/download/model72-v2.0.0/Model-72-2.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/model-72\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-model72/releases/download/model72-v2.0.0/Model-72-2.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Model-72/\"; elif [ -f \"./plugins/model-72/model72.so\" ]; then cp -rf ./plugins/model-72/* \"$TARGET_SYNTH_DIR/sd88me - VST - Model-72/\"; else echo \"[ERROR] No local binary archive found for model-72\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "analog", "mono", "bass", "circuit-model", "stereo", "presets" }
        },
        {
            "monksynth", "MonkSynth", "saustin2010", "v1.1.1", "Experimental",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2853", "1.6 MB", "14%",
            "Jonathan Taylor - VST - [SYN] MonkSynth", "monksynth.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-monksynth/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-monksynth/releases/download/v1.1.1/SYN-MonkSynth-1.1.1-mpc-armv7.zip", "plugins/monksynth/thumb.png",
            "A formant (FOF) singing voice with 12 characters and a choir.", "./plugins/monksynth/install.sh", 0,
            0,
            false,
            false,
            "",
            { "formant", "voice", "choir" }
        },
        {
            "mono-voice", "Mono Voice", "saustin2010", "v1.0.2", "Macro Hybrid",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2868", "250 KB", "14%",
            "timncox - VST - [SYN] Mono Voice", "monovoice.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-monovoice/v1.0.2/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-monovoice/releases/download/v1.0.2/SYN-Mono-Voice-1.0.2-mpc-armv7.zip", "plugins/mono-voice/thumb.png",
            "Elektron Monomachine-style digital voice: SuperWave, SID, DigiPRO, FM and more machines; 12 factory patches.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/timncox - VST - [SYN] Mono Voice\" && ZIP_PATH=\"./plugins/mono-voice/mono-voice.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-monovoice/releases/download/v1.0.2/SYN-Mono-Voice-1.0.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/mono-voice\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-monovoice/releases/download/v1.0.2/SYN-Mono-Voice-1.0.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/timncox - VST - [SYN] Mono Voice/\"; elif [ -f \"./plugins/mono-voice/monovoice.so\" ]; then cp -rf ./plugins/mono-voice/* \"$TARGET_SYNTH_DIR/timncox - VST - [SYN] Mono Voice/\"; else echo \"[ERROR] No local binary archive found for mono-voice\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "monomachine", "sid", "fm", "digital" }
        },
        {
            "mono-101", "Mono-101", "sd88me", "v0.5.1", "Mono Bass",
            "INST", "Stable", "Akai MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-21",
            "289", "3.8 MB", "14%",
            "sd88me - VST - Mono-101", "mono101.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-mono101/v0.5.0/docs/screenshots/vco-tab.png", "https://github.com/sd88me/mpc-vst-mono101/releases/download/v0.5.1/Mono-101-0.5.1-mpc-aarch64.zip", "plugins/mono-101/thumb.png",
            "A circuit-modelled SH-101 mono synth: a port of the Lemus engine, which solves the original schematic part by part, modified for the MPC. The manual's 40 sample sounds, user presets named on screen, and a Circuit Bent page based on Nova-mod and more (PWM, triangle VCO, overdrive, VCF FM, LFO range, key trigger, tempo sync). Gen2 only, CPU heavy.", "./plugins/mono-101/install.sh", 0,
            0,
            false,
            false,
            "",
            { "101", "mono", "bass", "lead", "circuit-model", "circuit-bent", "gen2" }
        },
        {
            "monomodule-one-gen2", "Monomodule One (Gen2)", "sd88me", "v1.0.4", "Macro Hybrid",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-04",
            "2457", "4.2 MB", "14%",
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
            "monomodule", "Monomodule One + FX", "sd88me", "v1.0.4", "Macro Hybrid",
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
            "monoslide", "MonoSlide", "Eurikon", "v1.0.5", "Mono Bass",
            "INST", "Stable", "MPC Live II Retro (3.10.1)", "MPC OS 3.x", "2026-11-13",
            "1851", "4.4 MB", "11%",
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
            "morpho-pe", "Morpho-PE", "sd88me", "v1.0.2", "Polysynth",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "3643", "2.2 MB", "14%",
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
            "mpcairlink", "MPC AirLink", "sttm", "v1.3.0", "Utility",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-21",
            "221", "6.0 MB", "14%",
            "sttm - VST - MPC AirLink", "VST/mpcairlink-arm32.so",
            "https://raw.githubusercontent.com/sttm/mpcairlink/v1.3.0/docs/ui-preview.png", "https://github.com/sttm/mpcairlink/releases/download/v1.3.0/mpcairlink-1.3.0-mpc-armv7.zip", "plugins/mpcairlink/thumb.png",
            "Receive classic AirPlay or VBAN audio on an MPC plugin track. Physical-style UI, gain on Q-Link 1, fixed 24 Hz meters and a 120 ms default VBAN buffer. ARMv7/AArch64; MPC OS 3.10.1+.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sttm - VST - MPC AirLink\" && ZIP_PATH=\"./plugins/mpcairlink/mpcairlink.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sttm/mpcairlink/releases/download/v1.3.0/mpcairlink-1.3.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/mpcairlink\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sttm/mpcairlink/releases/download/v1.3.0/mpcairlink-1.3.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sttm - VST - MPC AirLink/\"; elif [ -f \"./plugins/mpcairlink/VST/mpcairlink-arm32.so\" ]; then cp -rf ./plugins/mpcairlink/* \"$TARGET_SYNTH_DIR/sttm - VST - MPC AirLink/\"; else echo \"[ERROR] No local binary archive found for mpcairlink\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "network", "audio", "vban", "airplay" }
        },
        {
            "mpc-midi-converter", "MPC MIDI Converter", "3dthc", "v1.0.0", "Utility",
            "INST", "Stable", "MPC Live II (Gen1) (3.10)", "MPC OS 2.x & 3.x", "2026-11-18",
            "414", "419 KB", "14%",
            "3dthc - VST - MPC MIDI Converter", "mpc_midi_converter.so",
            "https://raw.githubusercontent.com/3dthc/MPC-MIDI-Converter/c9f4849a0fea9e11b28933987aeb5dda27c7159d/docs/images/main.png", "https://github.com/3dthc/MPC-MIDI-Converter/releases/download/v1.0.0/MPC-MIDI-Converter-1.0.0-mpc-armv7.zip", "plugins/mpc-midi-converter/thumb.png",
            "Converts MIDI files to MPC patterns right on the MPC, no computer needed. Browse your SD card and drives, convert one file or a whole pack with its subfolders, then load the patterns onto any track. Split tracks, map GM drums onto pads, quantize; each pattern's name carries the file's tempo. Gen1 (MPC OS 2.x and 3.x) and Gen2.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3dthc - VST - MPC MIDI Converter\" && ZIP_PATH=\"./plugins/mpc-midi-converter/mpc-midi-converter.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/3dthc/MPC-MIDI-Converter/releases/download/v1.0.0/MPC-MIDI-Converter-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/mpc-midi-converter\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/MPC-MIDI-Converter/releases/download/v1.0.0/MPC-MIDI-Converter-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/3dthc - VST - MPC MIDI Converter/\"; elif [ -f \"./plugins/mpc-midi-converter/mpc_midi_converter.so\" ]; then cp -rf ./plugins/mpc-midi-converter/* \"$TARGET_SYNTH_DIR/3dthc - VST - MPC MIDI Converter/\"; else echo \"[ERROR] No local binary archive found for mpc-midi-converter\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "midi", "converter", "mpcpattern", "file-browser", "batch", "drum-map" }
        },
        {
            "noxed-mpc-overwitch", "MPC Overwitch", "NOXED", "v0.6.0", "Utility",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "385", "793 KB", "14%",
            "NOXED - VST - MPC Overwitch", "VST/mpc-overwitch-arm32.so",
            "https://raw.githubusercontent.com/molgino/noxend-mpc-plugins/40832b26596b1c0228eb97e0dc81c257547a2d1a/docs/screenshots/mpc-overwitch.png", "https://github.com/molgino/noxend-mpc-plugins/releases/download/v0.3.0/noxed-mpc-overwitch-0.6.0-mpc-armv7.zip", "plugins/noxed-mpc-overwitch/thumb.png",
            "Receives individual or grouped outputs from an Elektron Overbridge device over USB, with MIDI forwarding and adaptive resampling. Based on Overwitch; requires an Overbridge-capable device.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/NOXED - VST - MPC Overwitch\" && ZIP_PATH=\"./plugins/noxed-mpc-overwitch/noxed-mpc-overwitch.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/molgino/noxend-mpc-plugins/releases/download/v0.3.0/noxed-mpc-overwitch-0.6.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/noxed-mpc-overwitch\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/molgino/noxend-mpc-plugins/releases/download/v0.3.0/noxed-mpc-overwitch-0.6.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/NOXED - VST - MPC Overwitch/\"; elif [ -f \"./plugins/noxed-mpc-overwitch/VST/mpc-overwitch-arm32.so\" ]; then cp -rf ./plugins/noxed-mpc-overwitch/* \"$TARGET_SYNTH_DIR/NOXED - VST - MPC Overwitch/\"; else echo \"[ERROR] No local binary archive found for noxed-mpc-overwitch\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "utility", "usb" }
        },
        {
            "mpc-plaits", "MPC Plaits", "poloq", "v1.0.1", "Macro Hybrid",
            "INST", "Stable", "Akai MPC One (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-05",
            "5090", "5.5 MB", "24%",
            "poloq - VST - MPC Plaits", "plaits.so",
            "https://raw.githubusercontent.com/poloq-instruments/mpc-vst-plaits/a30c24b83663a5efead73bbaca674ba4502a3eb5/docs/screenshots/card.png", "https://github.com/poloq-instruments/mpc-vst-plaits/releases/download/v1.0.1/MPC-Plaits-1.0.1-mpc-armv7.zip", "plugins/mpc-plaits/thumb.png",
            "Mutable Instruments Plaits, all 24 models, with LFOs, envelopes, two mod matrices, tempo sync and a touchscreen skin.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/poloq - VST - MPC Plaits\" && ZIP_PATH=\"./plugins/mpc-plaits/mpc-plaits.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/poloq-instruments/mpc-vst-plaits/releases/download/v1.0.1/MPC-Plaits-1.0.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/mpc-plaits\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/poloq-instruments/mpc-vst-plaits/releases/download/v1.0.1/MPC-Plaits-1.0.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/poloq - VST - MPC Plaits/\"; elif [ -f \"./plugins/mpc-plaits/plaits.so\" ]; then cp -rf ./plugins/mpc-plaits/* \"$TARGET_SYNTH_DIR/poloq - VST - MPC Plaits/\"; else echo \"[ERROR] No local binary archive found for mpc-plaits\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "mutable-instruments", "plaits", "macro-oscillator", "eurorack" }
        },
        {
            "noxed-mpc-usb-bridge", "MPC USB Bridge", "NOXED", "v0.2.3", "Utility",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "221", "409 KB", "14%",
            "NOXED - VST - MPC USB Bridge", "VST/mpc-usb-bridge-arm32.so",
            "https://raw.githubusercontent.com/molgino/noxend-mpc-plugins/40832b26596b1c0228eb97e0dc81c257547a2d1a/docs/screenshots/mpc-usb-bridge.png", "https://github.com/molgino/noxend-mpc-plugins/releases/download/v0.3.0/noxed-mpc-usb-bridge-0.2.3-mpc-armv7.zip", "plugins/noxed-mpc-usb-bridge/thumb.png",
            "Receives stereo audio from a class-compliant USB device as an MPC plugin track, with MIDI forwarding and drift-correcting resampling. Requires a compatible external USB audio device.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/NOXED - VST - MPC USB Bridge\" && ZIP_PATH=\"./plugins/noxed-mpc-usb-bridge/noxed-mpc-usb-bridge.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/molgino/noxend-mpc-plugins/releases/download/v0.3.0/noxed-mpc-usb-bridge-0.2.3-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/noxed-mpc-usb-bridge\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/molgino/noxend-mpc-plugins/releases/download/v0.3.0/noxed-mpc-usb-bridge-0.2.3-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/NOXED - VST - MPC USB Bridge/\"; elif [ -f \"./plugins/noxed-mpc-usb-bridge/VST/mpc-usb-bridge-arm32.so\" ]; then cp -rf ./plugins/noxed-mpc-usb-bridge/* \"$TARGET_SYNTH_DIR/NOXED - VST - MPC USB Bridge/\"; else echo \"[ERROR] No local binary archive found for noxed-mpc-usb-bridge\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "utility", "usb" }
        },
        {
            "mpseejay", "mpSEEJAY", "noahsoconnor", "v1.3.0", "Dj Players",
            "INST", "Stable", "MPC Live II (3.10.1.12)", "MPC OS 3.x", "2026-11-20",
            "244", "7.6 MB", "14%",
            "DJ - VST - mpSEEJAY", "mpseejay.so",
            "https://raw.githubusercontent.com/noahsoconnor/mpseejay/v1.1.0/docs/screenshot.png", "https://github.com/noahsoconnor/mpseejay/releases/download/v1.3.0/mpSEEJAY-1.3.0-mpc-armv7.zip", "plugins/mpseejay/thumb.png",
            "Two DJ decks for an external DJ mixer: one plugin per deck, each on its own output. Waveforms, tempo and beat-grid detection, sync, pitch fader (MIDI-learnable), cue, hot cues, loops and rolls, a browser with rekordbox playlists, sorting and star ratings, and the 16 pads lit on the MPC. WAV and MP3. Tested on an MPC Live II, MPC OS 3.10.1.", "./plugins/mpseejay/install.sh", 0,
            0,
            false,
            false,
            "",
            { "dj", "player", "decks", "sync", "rekordbox" }
        },
        {
            "mr-drums", "Mr Drums", "saustin2010", "v1.1.1", "Sampler Rompler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2528", "464 KB", "14%",
            "Move Everything - VST - [DRUM] Mr Drums", "mrdrums.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-mrdrums/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-mrdrums/releases/download/v1.1.1/DRUM-Mr-Drums-1.1.1-mpc-armv7.zip", "plugins/mr-drums/thumb.png",
            "A 16-pad drum sampler: kits of samples on pads 36-51.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Move Everything - VST - [DRUM] Mr Drums\" && ZIP_PATH=\"./plugins/mr-drums/mr-drums.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-mrdrums/releases/download/v1.1.1/DRUM-Mr-Drums-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/mr-drums\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-mrdrums/releases/download/v1.1.1/DRUM-Mr-Drums-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Move Everything - VST - [DRUM] Mr Drums/\"; elif [ -f \"./plugins/mr-drums/mrdrums.so\" ]; then cp -rf ./plugins/mr-drums/* \"$TARGET_SYNTH_DIR/Move Everything - VST - [DRUM] Mr Drums/\"; else echo \"[ERROR] No local binary archive found for mr-drums\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "drums", "kits", "pads" }
        },
        {
            "mr-hyde", "Mr Hyde", "saustin2010", "v1.1.1", "Macro Hybrid",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3616", "1.9 MB", "14%",
            "Move Everything - VST - [SYN] Mr Hyde", "mrhyde.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-mrhyde/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-mrhyde/releases/download/v1.1.1/SYN-Mr-Hyde-1.1.1-mpc-armv7.zip", "plugins/mr-hyde/thumb.png",
            "MicroFreak-style voice on Plaits' 17 models, with a low-pass gate, filter, cycling envelope and a 6x6 mod matrix; 19 presets.", "./plugins/mr-hyde/install.sh", 0,
            0,
            false,
            false,
            "",
            { "microfreak", "macro-oscillator", "mod-matrix", "low-pass-gate" }
        },
        {
            "ms20xxl", "MS20XXL", "L'Cronx", "v0.2.0", "Mono Bass",
            "INST", "Stable", "Force (3.9.1), MPC Live III (3.10.1)", "MPC OS 3.x", "2026-11-21",
            "2902", "19.3 MB", "14%",
            "L'Cronx - VST - MS20XXL", "ms20xxl.so",
            "https://raw.githubusercontent.com/sunskiefer/MS20XXL/v0.2.0/docs/img/patch.png", "https://github.com/sunskiefer/MS20XXL/releases/download/v0.2.0/MS20XXL-0.2.0-mpc-armv7.zip", "plugins/ms20xxl/thumb.png",
            "Korg MS-20-style semi-modular mono synth, circuit-modelled from the MS-20 schematics: the KLM-307 (MK2) and KORG35 (MK1) filter boards solved from their circuit equations, Hz/V sockets. Every MS-20 patch jack in its place, normalled like the hardware, patched by touch; an SQ-10-style sequencer; 63 patches with cables. 32-bit and 64-bit MPCs.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - MS20XXL\" && ZIP_PATH=\"./plugins/ms20xxl/ms20xxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/MS20XXL/releases/download/v0.2.0/MS20XXL-0.2.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/ms20xxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/MS20XXL/releases/download/v0.2.0/MS20XXL-0.2.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - MS20XXL/\"; elif [ -f \"./plugins/ms20xxl/ms20xxl.so\" ]; then cp -rf ./plugins/ms20xxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - MS20XXL/\"; else echo \"[ERROR] No local binary archive found for ms20xxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "ms-20", "semi-modular", "mono", "patch-panel", "sequencer", "sq-10", "korg35", "analog" }
        },
        {
            "ms2kr", "MS2KR", "sd88me", "v0.2.1", "Polysynth",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-18",
            "1115", "4.7 MB", "14%",
            "sd88me - VST - MS2KR", "ms2kr.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-ms2kr/v0.1.0/docs/screenshots/play.png", "https://github.com/sd88me/mpc-vst-ms2kr/releases/download/v0.2.1/MS2KR-0.2.1-mpc-aarch64.zip", "plugins/ms2kr/thumb.png",
            "An emulation of the MS2000R analog-modeling synth's own firmware (ms2000r-emu) as a native instrument for Gen2 MPC devices, with a six-page touchscreen skin laid out like the front panel, live LCD, favourites and Q-Link control. Gen2 (aarch64) only; you need your own ROM dumps.", "./plugins/ms2kr/install.sh", 0,
            0,
            false,
            false,
            "",
            { "emulation", "virtual-analog", "vocoder", "gen2" }
        },
        {
            "mutable-vibe", "Mutable Vibe MPC", "nachtaktiv303", "v0.9.3", "Macro Hybrid",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "4782", "1.1 MB", "14%",
            "nachtaktiv303 - VST - Mutable Vibe MPC", "mutable_vibe_mpc.so",
            "https://raw.githubusercontent.com/nachtaktiv303/mpc-vst-mutable-vibe/main/preview_0.png", "https://github.com/nachtaktiv303/mpc-vst-mutable-vibe/releases/download/mutable-vibe-vst-v0.9.3/Mutable-Vibe-MPC-0.9.3-mpc-armv7.zip", "plugins/mutable-vibe/thumb.png",
            "Mutable Instruments Rings + Plaits in one synth: 6 resonator models and 14 melodic macro-oscillator engines, with filter, FX, 4 envelopes, 4 LFOs, a mod matrix and tempo sync, plus a touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/nachtaktiv303 - VST - Mutable Vibe MPC\" && ZIP_PATH=\"./plugins/mutable-vibe/mutable-vibe.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/nachtaktiv303/mpc-vst-mutable-vibe/releases/download/mutable-vibe-vst-v0.9.3/Mutable-Vibe-MPC-0.9.3-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/mutable-vibe\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/nachtaktiv303/mpc-vst-mutable-vibe/releases/download/mutable-vibe-vst-v0.9.3/Mutable-Vibe-MPC-0.9.3-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/nachtaktiv303 - VST - Mutable Vibe MPC/\"; elif [ -f \"./plugins/mutable-vibe/mutable_vibe_mpc.so\" ]; then cp -rf ./plugins/mutable-vibe/* \"$TARGET_SYNTH_DIR/nachtaktiv303 - VST - Mutable Vibe MPC/\"; else echo \"[ERROR] No local binary archive found for mutable-vibe\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "mutable-instruments", "rings", "plaits", "resonator", "macro-oscillator", "eurorack" }
        },
        {
            "nodalred2x", "NodalRed2x", "sd88me", "v0.2.0", "Polysynth",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-16",
            "1035", "3.2 MB", "14%",
            "sd88me - VST - NodalRed2x", "nodalred2x.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-nodalred2x/v0.2.0/docs/screenshots/main.png", "https://github.com/sd88me/mpc-vst-nodalred2x/releases/download/v0.2.0/NodalRed2x-0.2.0-mpc-aarch64.zip", "plugins/nodalred2x/thumb.png",
            "An emulation of the Clavia Nord Lead 2x's original firmware (gearmulator) as a native instrument for Gen2 MPC devices: a native MPC screen laid out like the Nord's panel, a starter bank plus your own sound sets, RANDOM, named favourites in the PRESET menu and Q-Link control. Gen2 (aarch64) only; you need your own Nord Lead 2x ROM.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - NodalRed2x\" && ZIP_PATH=\"./plugins/nodalred2x/nodalred2x.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-nodalred2x/releases/download/v0.2.0/NodalRed2x-0.2.0-mpc-aarch64.zip\" ]; then mkdir -p \"./plugins/nodalred2x\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-nodalred2x/releases/download/v0.2.0/NodalRed2x-0.2.0-mpc-aarch64.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - NodalRed2x/\"; elif [ -f \"./plugins/nodalred2x/nodalred2x.so\" ]; then cp -rf ./plugins/nodalred2x/* \"$TARGET_SYNTH_DIR/sd88me - VST - NodalRed2x/\"; else echo \"[ERROR] No local binary archive found for nodalred2x\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "emulation", "virtual-analog", "rompler-free", "gen2" }
        },
        {
            "noisemaker", "Noisemaker", "saustin2010", "v1.1.1", "Polysynth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2838", "5.3 MB", "14%",
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
            "nova-editor", "Nova Editor", "3dthc", "v0.7.2", "Utility",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-17",
            "1216", "1.7 MB", "14%",
            "3dthc - VST - Nova Editor", "nova_editor.so",
            "https://raw.githubusercontent.com/3dthc/nova-editor/main/nova-editor-main.png", "https://github.com/3dthc/Nova-Editor/releases/download/nova-editor-v0.7.2/Nova-Editor-0.7.2-mpc-armv7.zip", "plugins/nova-editor/thumb.png",
            "An editor and librarian for the Novation MiniNova as a native plugin, with its own MPC screen skin: every control maps to a MiniNova CC/NRPN, controls follow the synth, banks A-C and imported .syx files live on the SD card. Also a SysEx librarian for any other synth. With nova-audio loaded, the MiniNova plays on the plugin's own track.", "./plugins/nova-editor/install.sh", 0,
            0,
            false,
            false,
            "",
            { "librarian", "sysex", "mininova", "novation", "midi" }
        },
        {
            "nusaw", "NuSaw", "saustin2010", "v1.0.2", "Polysynth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2858", "2.2 MB", "14%",
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
            "obxd", "OB-Xd", "saustin2010", "v1.1.1", "Polysynth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3923", "1.5 MB", "14%",
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
            "obxdmpc", "OB-Xd MPC", "3dthc", "v0.4.10", "Polysynth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-17",
            "7768", "15.7 MB", "14%",
            "sd88me - VST - OB-Xd", "obxd.so",
            "https://raw.githubusercontent.com/3dthc/OB-XD-MPC/c564dfd0c0966d92b29365819d754b8cf75bc616/docs/screenshots/obxd.png", "https://github.com/3dthc/OB-XD-MPC/releases/download/ob-xd-mpc-0.4.10/OB-Xd-0.4.10-mpc-armv7.zip", "plugins/obxdmpc/thumb.png",
            "OB-Xd, the Oberheim OB-X emulation: two oscillators with sync and cross-mod, a 12/24 dB filter, voice variation; 517 patches in 10 banks with a browser page, and your own .fxb banks. Also a VST3 for Mac and Windows and an Audio Unit for Mac.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - OB-Xd\" && ZIP_PATH=\"./plugins/obxdmpc/obxdmpc.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/3dthc/OB-XD-MPC/releases/download/ob-xd-mpc-0.4.10/OB-Xd-0.4.10-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/obxdmpc\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/OB-XD-MPC/releases/download/ob-xd-mpc-0.4.10/OB-Xd-0.4.10-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - OB-Xd/\"; elif [ -f \"./plugins/obxdmpc/obxd.so\" ]; then cp -rf ./plugins/obxdmpc/* \"$TARGET_SYNTH_DIR/sd88me - VST - OB-Xd/\"; else echo \"[ERROR] No local binary archive found for obxdmpc\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "ob-x", "analog-modelling", "polyphonic", "subtractive" }
        },
        {
            "odin2", "Odin 2", "3dthc", "v1.4.11", "Macro Hybrid",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-18",
            "6731", "23.4 MB", "14%",
            "3dthc - VST - Odin 2", "odin2.so",
            "https://raw.githubusercontent.com/3dthc/Odin2-MPC/main/odin2-mpc-main.png", "https://github.com/3dthc/Odin2-MPC/releases/download/odin2-v1.4.11/Odin-2-1.4.11-mpc-armv7.zip", "plugins/odin2/thumb.png",
            "Odin 2 synthesizer ported to standalone Akai MPC devices as a native VST2 plugin. Features Odin's original engine, 261 factory patches, touchscreen pages, automatic voice management and a CPU meter. Tested on an Akai MPC Live II; unofficial port. Also a VST3 for Mac and Windows and an Audio Unit for Mac.", "./plugins/odin2/install.sh", 0,
            0,
            false,
            false,
            "",
            {  }
        },
        {
            "omni-sampler", "Omni Sampler", "MrHighMan", "v1.8.1", "Sampler Rompler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-02",
            "2484", "5.0 MB", "20%",
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
            "op3000", "OP3000", "Stampman3000", "v1.0.1", "Fm",
            "INST", "Stable", "MPC One (3.10.1)", "MPC OS 3.x", "2026-11-21",
            "355", "3.5 MB", "14%",
            "Stampman3000 - VST - OP3000", "VST/op3000-arm32.so",
            "https://raw.githubusercontent.com/stampman3000-jpg/op3000/v1.0.0/screenshots/op3000.png", "https://github.com/stampman3000-jpg/op3000/releases/download/v1.0.1/OP3000-1.0.1-mpc-armv7.zip", "plugins/op3000/thumb.png",
            "Dual-chip FM synth in the style of the YM2151 and YM2612, with a PSG sub-chip, filter, chorus and delay.", "./plugins/op3000/install.sh", 0,
            0,
            false,
            false,
            "",
            { "synth", "fm" }
        },
        {
            "plugin-manager-g2", "Open MPC Plugin Manager", "sd88me", "v2.0.0", "Utility",
            "INST", "Stable", "MPC Live III (Gen2) (3.10.1.12)", "MPC OS 3.x", "2026-11-22",
            "8976", "34.7 MB", "14%",
            "OpenMPC - VST - Plugin Manager", "openmpc_pm_g2.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-plugins/588466d/tools/plugin_manager_g2/docs/screenshots/manager-discover-light.png", "https://github.com/open-mpc/plugin-manager/releases/download/plugin-manager-g2-v2.0.0/Plugin-Manager-G2-2.0.0-mpc-armv7.zip", "plugins/plugin-manager-g2/thumb.png",
            "The official OpenMPC plugin manager for Gen1 and Gen2 MPCs (32-bit build for Force, Live, One, X, Key; 64-bit for the Live III and kin). Browse this catalog and install, update and remove plugins on the MPC itself, with rollback if an install fails. Built on poloq's mpc-vst-manager.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/OpenMPC - VST - Plugin Manager\" && ZIP_PATH=\"./plugins/plugin-manager-g2/plugin-manager-g2.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/open-mpc/plugin-manager/releases/download/plugin-manager-g2-v2.0.0/Plugin-Manager-G2-2.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/plugin-manager-g2\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/open-mpc/plugin-manager/releases/download/plugin-manager-g2-v2.0.0/Plugin-Manager-G2-2.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/OpenMPC - VST - Plugin Manager/\"; elif [ -f \"./plugins/plugin-manager-g2/openmpc_pm_g2.so\" ]; then cp -rf ./plugins/plugin-manager-g2/* \"$TARGET_SYNTH_DIR/OpenMPC - VST - Plugin Manager/\"; else echo \"[ERROR] No local binary archive found for plugin-manager-g2\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "installer", "catalog", "manager", "gen1", "gen2", "utility" }
        },
        {
            "open303", "Open303", "saustin2010", "v1.2.1", "Mono Bass",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "4491", "2.0 MB", "14%",
            "Robin Schmidt - VST - [SYN] Open303", "acid303.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-open303/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-open303/releases/download/v1.2.1/SYN-Open303-1.2.1-mpc-armv7.zip", "plugins/open303/thumb.png",
            "TB-303 bass line: Open303 with the Devilfish mods, a drive stage and 13 presets.", "./plugins/open303/install.sh", 0,
            0,
            false,
            false,
            "",
            { "tb-303", "acid", "bass", "monophonic" }
        },
        {
            "osc-to-midi", "OSC to MIDI", "Gio Music", "v2.5.0", "Utility",
            "INST", "Stable", "MPC Live III (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-18",
            "67", "918 KB", "14%",
            "Gio Music - VST - OSC to MIDI", "VST/osc-to-midi-arm32.so",
            "https://raw.githubusercontent.com/RAS-G/OSC-to-MIDI-for-MPC/54b91a1f9a47472c583462b76b17bd92e8ff8b8b/docs/screenshot.png", "https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/osc-to-midi-2.5.0-767613110-mpc-armv7.zip", "plugins/osc-to-midi/thumb.png",
            "Turns OSC from TouchOSC, Lemur or Open Stage Control over Wi-Fi into MIDI notes and CCs for other MPC tracks. Listens for OSC on one UDP network port. Tested on an MPC Live III (MPC OS 3.10.1).", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Gio Music - VST - OSC to MIDI\" && ZIP_PATH=\"./plugins/osc-to-midi/osc-to-midi.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/osc-to-midi-2.5.0-767613110-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/osc-to-midi\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/osc-to-midi-2.5.0-767613110-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Gio Music - VST - OSC to MIDI/\"; elif [ -f \"./plugins/osc-to-midi/VST/osc-to-midi-arm32.so\" ]; then cp -rf ./plugins/osc-to-midi/* \"$TARGET_SYNTH_DIR/Gio Music - VST - OSC to MIDI/\"; else echo \"[ERROR] No local binary archive found for osc-to-midi\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "osc", "midi", "controller" }
        },
        {
            "osirus", "Osirus", "sd88me", "v1.0.0", "Polysynth",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-22",
            "4699", "3.8 MB", "14%",
            "sd88me - VST - Osirus", "osirus.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-osirus/v1.0.0/docs/screenshots/osirus-play.png", "https://github.com/sd88me/mpc-vst-osirus/releases/download/v1.0.0/Osirus-1.0.0-mpc-aarch64.zip", "plugins/osirus/thumb.png",
            "An emulation of the Access Virus A, B and C synthesizers' original firmware (gearmulator's DSP56300 emulator) as a native instrument for Gen2 MPC devices, with a touchscreen skin, your own SysEx banks (sub-folders too), named favourites, RANDOM, Q-Link control and knob values on every page. Gen2 (aarch64) only; you need your own Virus ROM.", "./plugins/osirus/install.sh", 0,
            0,
            false,
            false,
            "",
            { "emulation", "virtual-analog", "rompler-free", "gen2" }
        },
        {
            "ostirusmpc", "OsTirusMPC", "Pryonix", "v1.0.0", "Polysynth",
            "INST", "Stable", "MPC Live III (Gen2) (3.9), MPC XL (Gen2) (3.9)", "MPC OS 3.x", "2026-11-15",
            "626", "16.9 MB", "14%",
            "Pryonix - VST - OsTirusMPC", "ostirus_mpc.so",
            "https://raw.githubusercontent.com/dnkkit/OsTirusMPC/4312af0feddf8f0d2fc69963d2b08c4585243ead/MAIN-1.0.png", "https://github.com/dnkkit/OsTirusMPC/releases/download/v1.0.0/OsTirusMPC-1.0.0-mpc-aarch64.zip", "plugins/ostirusmpc/thumb.png",
            "Virus TI Snow hardware emulation for Gen2 MPC standalone, running the user's own compatible firmware. Eight touch pages, Q-Link maps, USER A-D banks, and Single-patch SysEx/MIDI file exchange. No firmware is included.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Pryonix - VST - OsTirusMPC\" && ZIP_PATH=\"./plugins/ostirusmpc/ostirusmpc.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/dnkkit/OsTirusMPC/releases/download/v1.0.0/OsTirusMPC-1.0.0-mpc-aarch64.zip\" ]; then mkdir -p \"./plugins/ostirusmpc\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/dnkkit/OsTirusMPC/releases/download/v1.0.0/OsTirusMPC-1.0.0-mpc-aarch64.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Pryonix - VST - OsTirusMPC/\"; elif [ -f \"./plugins/ostirusmpc/ostirus_mpc.so\" ]; then cp -rf ./plugins/ostirusmpc/* \"$TARGET_SYNTH_DIR/Pryonix - VST - OsTirusMPC/\"; else echo \"[ERROR] No local binary archive found for ostirusmpc\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "synth", "virtual-analog", "emulation" }
        },
        {
            "oxzygen", "OxZygen", "3dthc", "v0.7.7", "Sampler Rompler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-17",
            "3757", "158.7 MB", "14%",
            "oxzygen - VST - OxZygen", "oxzygen.so",
            "https://raw.githubusercontent.com/3dthc/OxZygen/b7640ca5971ef1371742bb2fad4ffc74a534b2a2/docs/img/player.png", "https://github.com/3dthc/OxZygen/releases/download/oxzygen-v0.7.7/OxZygen-0.7.7-mpc-armv7.zip", "plugins/oxzygen/thumb.png",
            "A dark, cinematic instrument: choirs, winds, brass, strings, bells, piano, e-piano, harp, mallets, synths and textures from CC0 recordings, in two layered slots with filter, lo-fi, tempo-synced delay and motion, reverb and sub. 200 presets with a browser page; a CRT orb reacts as you play. Also a VST3 for Mac and Windows and an Audio Unit for Mac.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/oxzygen - VST - OxZygen\" && ZIP_PATH=\"./plugins/oxzygen/oxzygen.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/3dthc/OxZygen/releases/download/oxzygen-v0.7.7/OxZygen-0.7.7-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/oxzygen\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/OxZygen/releases/download/oxzygen-v0.7.7/OxZygen-0.7.7-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/oxzygen - VST - OxZygen/\"; elif [ -f \"./plugins/oxzygen/oxzygen.so\" ]; then cp -rf ./plugins/oxzygen/* \"$TARGET_SYNTH_DIR/oxzygen - VST - OxZygen/\"; else echo \"[ERROR] No local binary archive found for oxzygen\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "choir", "vocal", "cinematic", "ambient", "winds", "brass", "bells", "lo-fi" }
        },
        {
            "percolator", "Percolator", "saustin2010", "v2.1.0", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "5296", "6.6 MB", "14%",
            "Steve A - VST - [DRUM] Percolator", "percolator.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-percolator/v2.1.0/screenshots/page_1.png", "https://github.com/saustin2010/mpc-vst-percolator/releases/download/v2.1.0/DRUM-Percolator-2.1.0-mpc-armv7.zip", "plugins/percolator/thumb.png",
            "Four-voice percussion synth laid out like the Erica Synths Pērkons HD-01 (three algorithms per voice, BBD delay, LFO to every knob), with envelope displays; each voice on its own MIDI channel with the Pērkons' CCs, and on its own track with Percolator OUT; plays the Pērkons' free kit packs.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Steve A - VST - [DRUM] Percolator\" && ZIP_PATH=\"./plugins/percolator/percolator.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-percolator/releases/download/v2.1.0/DRUM-Percolator-2.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/percolator\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-percolator/releases/download/v2.1.0/DRUM-Percolator-2.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Steve A - VST - [DRUM] Percolator/\"; elif [ -f \"./plugins/percolator/percolator.so\" ]; then cp -rf ./plugins/percolator/* \"$TARGET_SYNTH_DIR/Steve A - VST - [DRUM] Percolator/\"; else echo \"[ERROR] No local binary archive found for percolator\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "drums", "percussion", "kits", "drone", "envelope-display" }
        },
        {
            "op1fx-phase", "Phase", "Bombest Audio", "v0.6.0", "Macro Hybrid",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1470", "7.5 MB", "14%",
            "Bombest Audio - VST - Phase", "phase.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/phase.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Phase-0.6.0-mpc-armv7.zip", "plugins/op1fx-phase/thumb.png",
            "Eight-voice phase-distortion synth, designed from the OP-1 screen.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Phase\" && ZIP_PATH=\"./plugins/op1fx-phase/op1fx-phase.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Phase-0.6.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-phase\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Phase-0.6.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Phase/\"; elif [ -f \"./plugins/op1fx-phase/phase.so\" ]; then cp -rf ./plugins/op1fx-phase/* \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Phase/\"; else echo \"[ERROR] No local binary archive found for op1fx-phase\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "synth", "phase-distortion" }
        },
        {
            "polywerk", "PolyWerk", "Devko", "v0.0.6", "Wavetable",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "1387", "6.3 MB", "14%",
            "Devko - VST - PolyWerk", "polywerk.so",
            "https://raw.githubusercontent.com/Devko/PolyWerk/main/docs/img/waves.png", "https://github.com/Devko/PolyWerk/releases/download/v0.0.6/PolyWerk-0.0.6-mpc-armv7.zip", "plugins/polywerk/thumb.png",
            "Eight-voice wavetable synth: two wavetable oscillators with 8x unison and subs, 30 built-in tables plus your own Serum-format WAVs, two filters with comb and vowel, two LFOs, a 12-slot mod matrix, arpeggiator and sequencers, 205 presets and a seven-tab touchscreen skin with Q-Links.", "./plugins/polywerk/install.sh", 0,
            0,
            false,
            false,
            "",
            { "wavetable", "polyphonic", "arpeggiator", "sequencer", "mod-matrix" }
        },
        {
            "potential1200", "Potential1200", "PotentialBreaks", "v1.1.0", "Sampler Rompler",
            "INST", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-19",
            "1371", "9.4 MB", "7%",
            "PotentialBreaks - VST - Potential1200", "potential1200.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potential1200/65bd97f44fd315c321e58f92ce368d33636c18a9/docs/images/potential1200.png", "https://github.com/PotentialBreakfast/mpc-vst-potential1200/releases/download/v1.1.0/Potential1200-1.1.0-mpc-armv7.zip", "plugins/potential1200/thumb.png",
            "12-bit drum sampler in the late-80s style: 8 faders and 8 pads, one-shots from your folders or a break cut to 2 bars from a kick on the 1, taken at 26 kHz into 12 bits with drop-sample pitch, the 45 > 33 trick, the classic per-pad filters and MULTI PITCH. Saves an MPC drum program. Four looks, its own Vintage look first.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - Potential1200\" && ZIP_PATH=\"./plugins/potential1200/potential1200.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/PotentialBreakfast/mpc-vst-potential1200/releases/download/v1.1.0/Potential1200-1.1.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/potential1200\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/PotentialBreakfast/mpc-vst-potential1200/releases/download/v1.1.0/Potential1200-1.1.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - Potential1200/\"; elif [ -f \"./plugins/potential1200/potential1200.so\" ]; then cp -rf ./plugins/potential1200/* \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - Potential1200/\"; else echo \"[ERROR] No local binary archive found for potential1200\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "sampler", "12-bit", "lo-fi", "drums", "breaks", "vintage" }
        },
        {
            "potentialbreaks", "PotentialBreaks", "PotentialBreaks", "v1.9.0", "Sampler Rompler",
            "INST", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-19",
            "4051", "6.9 MB", "2%",
            "PotentialBreaks - VST - PotentialBreaks", "potentialbreaks.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialbreaks/8d94a8c800a149c068b13b056e62369e6449cbc0/docs/images/potentialbreaks.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialbreaks/releases/download/v1.9.0/PotentialBreaks-1.9.0-mpc-armv7.zip", "plugins/potentialbreaks/thumb.png",
            "Drum-break slicer: picks a random break from your own break folders (WAV or FLAC), chops it to 8 or 16 slices or by hits across the pads and matches it to the project tempo, by re-pitching or stretching. Shuffle the slices and save the groove as MIDI.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialBreaks\" && ZIP_PATH=\"./plugins/potentialbreaks/potentialbreaks.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/PotentialBreakfast/mpc-vst-potentialbreaks/releases/download/v1.9.0/PotentialBreaks-1.9.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/potentialbreaks\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/PotentialBreakfast/mpc-vst-potentialbreaks/releases/download/v1.9.0/PotentialBreaks-1.9.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialBreaks/\"; elif [ -f \"./plugins/potentialbreaks/potentialbreaks.so\" ]; then cp -rf ./plugins/potentialbreaks/* \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialBreaks/\"; else echo \"[ERROR] No local binary archive found for potentialbreaks\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "breaks", "slicer", "drums", "chop" }
        },
        {
            "potentialcrates", "PotentialCrates", "PotentialBreaks", "v1.4.0", "Dj Players",
            "INST", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-19",
            "2204", "70.5 MB", "1%",
            "PotentialBreaks - VST - PotentialCrates", "potentialcrates.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialcrates/a299c15321ad23c77343baaeb0c25e851e6e74f9/docs/images/potentialcrates.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialcrates/releases/download/v1.4.0/PotentialCrates-1.4.0-mpc-armv7.zip", "plugins/potentialcrates/thumb.png",
            "Record digger: one-tap crates (Drum Breaks, Library, Soundtracks, ...) and ROLL dice dig Discogs and stream a record into your track to sample. A PotentialSuite fork of Crate Digger.", "./plugins/potentialcrates/install.sh", 0,
            0,
            false,
            false,
            "",
            { "crate-digging", "discogs", "youtube", "sampling" }
        },
        {
            "potentialkits", "PotentialKits", "PotentialBreaks", "v1.3.0", "Sampler Rompler",
            "INST", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-19",
            "1173", "7.5 MB", "3%",
            "PotentialBreaks - VST - PotentialKits", "potentialkits.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialkits/aa460ad13569f7f01928d0ed9167c58a665cba26/docs/images/potentialkits.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialkits/releases/download/v1.3.0/PotentialKits-1.3.0-mpc-armv7.zip", "plugins/potentialkits/thumb.png",
            "Random drum-kit builder: 16 sounds from your own sample folders, one-tap KIT STYLES and ROLL dice, pitch, decay and choke per pad, USER kits, exported as an MPC drum program. A PotentialSuite fork of Lucky Dip.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialKits\" && ZIP_PATH=\"./plugins/potentialkits/potentialkits.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/PotentialBreakfast/mpc-vst-potentialkits/releases/download/v1.3.0/PotentialKits-1.3.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/potentialkits\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/PotentialBreakfast/mpc-vst-potentialkits/releases/download/v1.3.0/PotentialKits-1.3.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialKits/\"; elif [ -f \"./plugins/potentialkits/potentialkits.so\" ]; then cp -rf ./plugins/potentialkits/* \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialKits/\"; else echo \"[ERROR] No local binary archive found for potentialkits\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "sampler", "kit-builder", "random", "drum-kit" }
        },
        {
            "potentialnoise", "PotentialNoise", "PotentialBreaks", "v2.6.0", "Utility",
            "INST", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-19",
            "2765", "3.6 MB", "2%",
            "PotentialBreaks - VST - PotentialNoise", "potentialnoise.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialnoise/3c9b81922b9e044af69123dcdfe72f06011ed4e5/docs/images/potentialnoise.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialnoise/releases/download/v2.6.0/PotentialNoise-2.6.0-mpc-armv7.zip", "plugins/potentialnoise/thumb.png",
            "Calibrated pink noise reference for the pink-noise mixing method: one-tap RMS levels from -24 to -12 dB, mono or stereo, full or 40 Hz-16 kHz range, live RMS and peak reading and a level meter.", "./plugins/potentialnoise/install.sh", 0,
            0,
            false,
            false,
            "",
            { "pink-noise", "mixing", "reference", "meter" }
        },
        {
            "potentialwax", "PotentialWax", "PotentialBreaks", "v1.9.1", "Sampler Rompler",
            "INST", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-18",
            "3918", "8.3 MB", "10%",
            "PotentialBreaks - VST - PotentialWax", "potentialwax.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialwax/7daecab7e3363545c8642fdfbf186ace5ab10366/docs/images/potentialwax.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialwax/releases/download/v1.9.1/PotentialWax-1.9.1-mpc-armv7.zip", "plugins/potentialwax/thumb.png",
            "Melodic sampler in the spirit of Serato Sample: picks or browses songs from your own folders (MP3, WAV or FLAC), detects tempo and key, sets 16 cue points (phrases, bars or hits) on the pads and matches them to your tempo and key, with time-stretch or vinyl-style pitching. Save the chops as WAVs.", "./plugins/potentialwax/install.sh", 0,
            0,
            false,
            false,
            "",
            { "chop", "melodic", "sampler", "time-stretch", "key-detection" }
        },
        {
            "noxed-prism-wave", "Prism Wave", "NOXED", "v0.1.4", "Wavetable",
            "INST", "Stable", "MPC Live III (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-20",
            "462", "13.7 MB", "14%",
            "NOXED - VST - Prism Wave", "VST/prism-wave-arm32.so",
            "https://raw.githubusercontent.com/molgino/noxend-mpc-plugins/40832b26596b1c0228eb97e0dc81c257547a2d1a/docs/screenshots/prism-wave-osc.png", "https://github.com/molgino/noxend-mpc-plugins/releases/download/v0.3.0/noxed-prism-wave-0.1.4-mpc-armv7.zip", "plugins/noxed-prism-wave/thumb.png",
            "Eight-voice wavetable synthesizer with two oscillators, unison, sub/noise, envelopes, LFOs, a modulation matrix, chorus, delay and WAV wavetable import. Includes eight presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/NOXED - VST - Prism Wave\" && ZIP_PATH=\"./plugins/noxed-prism-wave/noxed-prism-wave.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/molgino/noxend-mpc-plugins/releases/download/v0.3.0/noxed-prism-wave-0.1.4-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/noxed-prism-wave\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/molgino/noxend-mpc-plugins/releases/download/v0.3.0/noxed-prism-wave-0.1.4-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/NOXED - VST - Prism Wave/\"; elif [ -f \"./plugins/noxed-prism-wave/VST/prism-wave-arm32.so\" ]; then cp -rf ./plugins/noxed-prism-wave/* \"$TARGET_SYNTH_DIR/NOXED - VST - Prism Wave/\"; else echo \"[ERROR] No local binary archive found for noxed-prism-wave\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "synth", "wavetable" }
        },
        {
            "prloguexxl", "PrLogueXXL", "L'Cronx", "v0.1.1", "Fm",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-23",
            "2490", "18.0 MB", "14%",
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
            "profit-08", "Profit-08", "sd88me", "v2.0.2", "Polysynth",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "6966", "5.0 MB", "14%",
            "sd88me - VST - Profit-08", "profit_08.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-profit8/main/docs/img/screenshot-program.png", "https://github.com/sd88me/mpc-vst-profit8/releases/download/profit-08-v2.0.2/Profit-08-2.0.2-mpc-armv7.zip", "plugins/profit-08/thumb.png",
            "Eight-voice, two-layer analog-style synthesizer: stack and split layers, a gated sequencer and arpeggiator, a resonant 2/4-pole filter, and a banks browser with favourites for your own program and bank SysEx dumps. 2.0 adds a native MPC screen with large knobs and Q-Link control of every page.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Profit-08\" && ZIP_PATH=\"./plugins/profit-08/profit-08.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-profit8/releases/download/profit-08-v2.0.2/Profit-08-2.0.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/profit-08\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-profit8/releases/download/profit-08-v2.0.2/Profit-08-2.0.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Profit-08/\"; elif [ -f \"./plugins/profit-08/profit_08.so\" ]; then cp -rf ./plugins/profit-08/* \"$TARGET_SYNTH_DIR/sd88me - VST - Profit-08/\"; else echo \"[ERROR] No local binary archive found for profit-08\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "analog", "poly", "layers", "sequencer", "favourites", "q-links" }
        },
        {
            "profit-5", "Profit-5", "sd88me", "v2.0.0", "Polysynth",
            "INST", "Stable", "Akai MPC Live III (Gen2) (3.10), Akai Force (Gen1) (3.9.1)", "MPC OS 3.x", "2026-11-13",
            "4121", "17.8 MB", "14%",
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
            "kp-sim-prophetic", "Prophetic", "ratbuddy", "v0.1.0", "Physical Modeling",
            "INST", "Stable", "MPC Live III (3.10.1)", "MPC OS 2.x & 3.x", "2026-11-19",
            "100", "2.0 MB", "14%",
            "ratbuddy - VST - Prophetic", "kp_sim_prophetic.so",
            "", "", "plugins/kp-sim-prophetic/thumb.png",
            "The Korg Prophecy's synthesis, built on your computer from your own Prophecy v2.0 ROMs: their DSP programs and sound-engine firmware, translated to C++. 128 programs, every parameter, Performance Editor, arpeggiator, 1-8 voices. Gen2; tested on an MPC Live III.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/ratbuddy - VST - Prophetic\" && ZIP_PATH=\"./plugins/kp-sim-prophetic/kp-sim-prophetic.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"\" ]; then mkdir -p \"./plugins/kp-sim-prophetic\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/ratbuddy - VST - Prophetic/\"; elif [ -f \"./plugins/kp-sim-prophetic/kp_sim_prophetic.so\" ]; then cp -rf ./plugins/kp-sim-prophetic/* \"$TARGET_SYNTH_DIR/ratbuddy - VST - Prophetic/\"; else echo \"[ERROR] No local binary archive found for kp-sim-prophetic\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "emulation", "firmware-derived", "physical-modelling", "arpeggiator", "polyphonic", "gen2", "korg" }
        },
        {
            "ps3100xxl", "PS3100XXL", "L'Cronx", "v0.1.5", "Polysynth",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-23",
            "2301", "15.9 MB", "14%",
            "L'Cronx - VST - PS3100XXL", "ps3100xxl.so",
            "https://raw.githubusercontent.com/sunskiefer/PS3100XXL/v0.1.5/docs/img/overview.png", "https://github.com/sunskiefer/PS3100XXL/releases/download/v0.1.5/PS3100XXL-0.1.5-mpc-armv7.zip", "plugins/ps3100xxl/thumb.png",
            "Semi-modular poly synth modelled on the 1977 Korg PS-3100 (not affiliated with Korg): one oscillator, filter and envelope per key up to 48 voices, three-peak resonators, ensemble, two modulation generators, sample & hold, twelve temperament knobs and a patch bay you patch by touch, on one zoomable page.", "./plugins/ps3100xxl/install.sh", 0,
            0,
            false,
            false,
            "",
            { "synth", "poly", "semi-modular", "analog-modelled", "patch-bay", "ensemble", "resonator" }
        },
        {
            "op1fx-pulse", "Pulse", "Bombest Audio", "v0.6.0", "Macro Hybrid",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1457", "5.1 MB", "14%",
            "Bombest Audio - VST - Pulse", "pulse.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/pulse.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Pulse-0.6.0-mpc-armv7.zip", "plugins/op1fx-pulse/thumb.png",
            "Two PWM pulse oscillators per voice, eight voices. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Pulse\" && ZIP_PATH=\"./plugins/op1fx-pulse/op1fx-pulse.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Pulse-0.6.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-pulse\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Pulse-0.6.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Pulse/\"; elif [ -f \"./plugins/op1fx-pulse/pulse.so\" ]; then cp -rf ./plugins/op1fx-pulse/* \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Pulse/\"; else echo \"[ERROR] No local binary archive found for op1fx-pulse\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "synth", "pwm", "pulse" }
        },
        {
            "raffosynth", "RaffoSynth", "saustin2010", "v1.3.1", "Mono Bass",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "4043", "6.4 MB", "14%",
            "Raffo - VST - [SYN] RaffoSynth", "moog.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-raffosynth/v1.3.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-raffosynth/releases/download/v1.3.1/SYN-RaffoSynth-1.3.1-mpc-armv7.zip", "plugins/raffosynth/thumb.png",
            "RaffoSynth: a Minimoog-style mono synth with four oscillators and a ladder filter.", "./plugins/raffosynth/install.sh", 0,
            0,
            false,
            false,
            "",
            { "minimoog", "monophonic", "ladder-filter", "analog-modelling" }
        },
        {
            "remixxxl", "reMixXXL", "L'Cronx", "v0.2.0", "Dj Players",
            "INST", "Beta", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-17",
            "1925", "27.3 MB", "14%",
            "L'Cronx - VST - reMixXXL", "remixxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/reMixXXL/v0.1.25/docs/img/wheel.png", "https://github.com/sunskiefer/reMixXXL/releases/download/v0.2.0/reMixXXL-0.2.0-mpc-armv7.zip", "plugins/remixxxl/thumb.png",
            "Four Mixxx-engine DJ decks for the Force: CDJ-style jog wheels you spin with a finger (VINYL scratches, CDJ nudges the pitch), mixer, two effect units, a track browser, a phone/PC companion page and Mixxx controller maps, with a big-button touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - reMixXXL\" && ZIP_PATH=\"./plugins/remixxxl/remixxxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/reMixXXL/releases/download/v0.2.0/reMixXXL-0.2.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/remixxxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/reMixXXL/releases/download/v0.2.0/reMixXXL-0.2.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - reMixXXL/\"; elif [ -f \"./plugins/remixxxl/remixxxl.so\" ]; then cp -rf ./plugins/remixxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - reMixXXL/\"; else echo \"[ERROR] No local binary archive found for remixxxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "dj", "decks", "mixer", "cdj", "jog-wheel", "mixxx", "midi-learn", "browser" }
        },
        {
            "rings", "Resonator", "saustin2010", "v1.2.1", "Physical Modeling",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "4355", "4.1 MB", "14%",
            "Émilie Gillet - VST - [SYN] Resonator", "rings.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-resonator/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-resonator/releases/download/v1.2.1/SYN-Resonator-1.2.1-mpc-armv7.zip", "plugins/rings/thumb.png",
            "Resonator: seven resonator models (strings, modal, FM, chords) strummed by the notes you play; 14 presets, from Émilie Gillet's open-source Rings.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Resonator\" && ZIP_PATH=\"./plugins/rings/rings.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-resonator/releases/download/v1.2.1/SYN-Resonator-1.2.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/rings\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-resonator/releases/download/v1.2.1/SYN-Resonator-1.2.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Resonator/\"; elif [ -f \"./plugins/rings/rings.so\" ]; then cp -rf ./plugins/rings/* \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SYN] Resonator/\"; else echo \"[ERROR] No local binary archive found for rings\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "resonator", "physical-modelling", "eurorack" }
        },
        {
            "roads", "Roads", "Aaron Quint", "v1.2.3", "Keys Organ",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-12",
            "1351", "3.0 MB", "15%",
            "quirkey - VST - Roads", "roads.so",
            "https://raw.githubusercontent.com/quirkey/mpc-plugins/roads-v1.2.3/docs/roads.png", "https://github.com/quirkey/mpc-plugins/releases/download/roads-v1.2.3/Roads-1.2.3-mpc-armv7.zip", "plugins/roads/thumb.png",
            "A 16-voice electric piano from mda ePiano's samples and modelling, with tine, reed and clav characters, tremolo / auto-pan, chorus, a note-triggered envelope filter and a 3-band EQ.", "./plugins/roads/install.sh", 0,
            0,
            false,
            false,
            "",
            { "electric-piano", "keys", "epiano", "polyphonic" }
        },
        {
            "setbfree-organ", "SetBfree Organ", "SamplingHigh", "v0.4.0", "Keys Organ",
            "INST", "Stable", "Akai Force (3.10.1)", "MPC OS 3.x", "2026-11-15",
            "3530", "1.6 MB", "8120%",
            "SamplingHigh - VST - SetBfree Organ", "setbfree.so",
            "https://raw.githubusercontent.com/SamplingHigh/mpc-vst-setbfree/v0.4.0/docs/images/setbfree-organ.png", "https://github.com/SamplingHigh/mpc-vst-setbfree/releases/download/v0.4.0/SetBfree-Organ-0.4.0-mpc-armv7.zip", "plugins/setbfree-organ/thumb.png",
            "Tonewheel organ with drawbars, selectable generator foldback, percussion, scanner, editable Leslie and keyboard splits.", "./plugins/setbfree-organ/install.sh", 0,
            0,
            false,
            false,
            "",
            { "tonewheel", "drawbar", "rotary" }
        },
        {
            "sfizz", "sfizz", "gmorb (MPC port); sfizz by sfztools", "v1.1.1", "Sampler Rompler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1903", "1.4 MB", "14%",
            "sfztools - VST - sfizz", "sfizz_mpc.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-sfizz/6d9e627281779f087898521cd27caabf9b7a75f2/docs/screen.png", "https://github.com/gmorb/mpc-vst-sfizz/releases/download/v1.1.1/sfizz-1.1.1-mpc-armv7.zip", "plugins/sfizz/thumb.png",
            "The sfizz 1.2.3 sampler: plays SFZ libraries and SoundFont 2 (.sf2) files from SD cards and USB drives, streamed from disk, with key switches, the instrument's own CC controls, Scala tuning, mono/legato with glide and up to Sinc 72 interpolation, on a one-page screen modelled on sfizz's editor. Gen1 and Gen2 builds.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sfztools - VST - sfizz\" && ZIP_PATH=\"./plugins/sfizz/sfizz.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/gmorb/mpc-vst-sfizz/releases/download/v1.1.1/sfizz-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/sfizz\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/gmorb/mpc-vst-sfizz/releases/download/v1.1.1/sfizz-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sfztools - VST - sfizz/\"; elif [ -f \"./plugins/sfizz/sfizz_mpc.so\" ]; then cp -rf ./plugins/sfizz/* \"$TARGET_SYNTH_DIR/sfztools - VST - sfizz/\"; else echo \"[ERROR] No local binary archive found for sfizz\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "sfz", "sf2", "soundfont", "sampler", "piano", "keyswitch", "streaming", "scala" }
        },
        {
            "sonix", "Sonix", "3dthc", "v0.3.16", "Sampler Rompler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-17",
            "5081", "125.0 MB", "14%",
            "3dthc - VST - Sonix", "sonix.so",
            "https://raw.githubusercontent.com/3dthc/Sonix/main/docs/images/main_layers.jpg", "https://github.com/3dthc/Sonix/releases/download/v0.3.16/Sonix-0.3.16-mpc-armv7.zip", "plugins/sonix/thumb.png",
            "A melodic ROMpler instrument for standalone Akai MPC devices, built as a native ARM VST2 plugin. Includes pianos, keys, pads, strings, brass, choirs, leads, plucks, bass, bells, synth chords and effects, using redistributable sound libraries. Also a VST3 for Mac and Windows and an Audio Unit for Mac.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3dthc - VST - Sonix\" && ZIP_PATH=\"./plugins/sonix/sonix.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/3dthc/Sonix/releases/download/v0.3.16/Sonix-0.3.16-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/sonix\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/Sonix/releases/download/v0.3.16/Sonix-0.3.16-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/3dthc - VST - Sonix/\"; elif [ -f \"./plugins/sonix/sonix.so\" ]; then cp -rf ./plugins/sonix/* \"$TARGET_SYNTH_DIR/3dthc - VST - Sonix/\"; else echo \"[ERROR] No local binary archive found for sonix\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            {  }
        },
        {
            "sq8lmpc", "SQ8LMPC", "L'Cronx", "v0.2.0", "Wavetable",
            "INST", "Stable", "Force (3.9.1)", "MPC OS 3.x", "2026-11-16",
            "743", "1.4 MB", "14%",
            "L'Cronx - VST - SQ8LMPC", "sq8lmpc.so",
            "https://raw.githubusercontent.com/sunskiefer/SQ8LMPC/0.2.0/docs/img/panel.png", "https://github.com/sunskiefer/SQ8LMPC/releases/download/0.2.0/SQ8LMPC-0.2.0-mpc-armv7.zip", "plugins/sq8lmpc/thumb.png",
            "SQ8L 0.91b (Ensoniq SQ-80 emulation by Siegfried Kullmann, based on sq8l-revival) on the Akai Force: same engine and sound, the SQ8L panel on one screen, 128 factory sounds, your own banks with SAVE, drop-in banks. Bring your own SQ-80 wave ROM (not included). Unofficial, free, non-commercial.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - SQ8LMPC\" && ZIP_PATH=\"./plugins/sq8lmpc/sq8lmpc.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/SQ8LMPC/releases/download/0.2.0/SQ8LMPC-0.2.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/sq8lmpc\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/SQ8LMPC/releases/download/0.2.0/SQ8LMPC-0.2.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - SQ8LMPC/\"; elif [ -f \"./plugins/sq8lmpc/sq8lmpc.so\" ]; then cp -rf ./plugins/sq8lmpc/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - SQ8LMPC/\"; else echo \"[ERROR] No local binary archive found for sq8lmpc\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "ensoniq", "sq-80", "esq-1", "emulation", "sq8l", "digital-wavetable", "preservation" }
        },
        {
            "op1fx-string", "String", "Bombest Audio", "v0.6.0", "Physical Modeling",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1740", "3.7 MB", "14%",
            "Bombest Audio - VST - String", "string.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/string.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/String-0.6.0-mpc-armv7.zip", "plugins/op1fx-string/thumb.png",
            "Plucked string with eight voices and an amp envelope. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Bombest Audio - VST - String\" && ZIP_PATH=\"./plugins/op1fx-string/op1fx-string.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/String-0.6.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-string\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/String-0.6.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Bombest Audio - VST - String/\"; elif [ -f \"./plugins/op1fx-string/string.so\" ]; then cp -rf ./plugins/op1fx-string/* \"$TARGET_SYNTH_DIR/Bombest Audio - VST - String/\"; else echo \"[ERROR] No local binary archive found for op1fx-string\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "synth", "pluck", "string" }
        },
        {
            "sturm-tp", "Sturm-TP", "sd88me", "v0.1.2", "Drum Machine",
            "INST", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "2855", "4.5 MB", "14%",
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
            "subwerk", "SubWerk", "Devko", "v0.0.10", "Mono Bass",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "1084", "1.0 MB", "14%",
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
            "sunflower", "Sunflower", "Lumina Aerospace LLC", "v1.0.0", "Experimental",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-17",
            "121", "5.2 MB", "14%",
            "Lumina Aerospace LLC - VST - Sunflower", "VST/sunflower-arm32.so",
            "https://raw.githubusercontent.com/Luminous-Works/mpc-vst-sunflower/v1.0.0/docs/screenshots/spiral.png", "https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/sunflower-1.0.0-771530594-mpc-armv7.zip", "plugins/sunflower/thumb.png",
            "Fibonacci sequencer: two arms step a sunflower lattice at the golden angle over a self-sounding percussion bed. 14 modes, live playhead.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Lumina Aerospace LLC - VST - Sunflower\" && ZIP_PATH=\"./plugins/sunflower/sunflower.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/sunflower-1.0.0-771530594-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/sunflower\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/sunflower-1.0.0-771530594-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Lumina Aerospace LLC - VST - Sunflower/\"; elif [ -f \"./plugins/sunflower/VST/sunflower-arm32.so\" ]; then cp -rf ./plugins/sunflower/* \"$TARGET_SYNTH_DIR/Lumina Aerospace LLC - VST - Sunflower/\"; else echo \"[ERROR] No local binary archive found for sunflower\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "fibonacci", "generative", "sequencer", "golden-angle", "percussion" }
        },
        {
            "surgexxl", "SurgeXXL", "L'Cronx", "v0.2.0", "Macro Hybrid",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-24",
            "3105", "294.4 MB", "14%",
            "L'Cronx - VST - SurgeXXL", "surgexxl.so",
            "https://raw.githubusercontent.com/sunskiefer/SurgeXXL/08c3c3eed8a3ad6792211fb1dc2351ded2383995/docs/img/osc.png", "https://github.com/sunskiefer/SurgeXXL/releases/download/v0.2.0/SurgeXXL-0.2.0-mpc-armv7.zip", "plugins/surgexxl/thumb.png",
            "The open-source Surge XT synth engine, headless, for Gen1: 2 scenes of 3 oscillators, dual filters, 6 LFOs per scene, mod matrix, 16 FX slots and 8 macros on eight pages. Loads every factory and third-party patch (3008) and wavetable (761), plus your own. Install it on its own (about 450 MB). MPC OS 3.x.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - SurgeXXL\" && ZIP_PATH=\"./plugins/surgexxl/surgexxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/SurgeXXL/releases/download/v0.2.0/SurgeXXL-0.2.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/surgexxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/SurgeXXL/releases/download/v0.2.0/SurgeXXL-0.2.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - SurgeXXL/\"; elif [ -f \"./plugins/surgexxl/surgexxl.so\" ]; then cp -rf ./plugins/surgexxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - SurgeXXL/\"; else echo \"[ERROR] No local binary archive found for surgexxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "synth", "polysynth", "wavetable", "fm", "surge", "lfo", "presets", "airwindows" }
        },
        {
            "sysex-bandit", "Sysex Bandit", "Dream Static Sounds", "v1.0.1", "Utility",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-17",
            "412", "2.5 MB", "14%",
            "Dream Static Sounds - VST - Sysex Bandit", "sysex_bandit.so",
            "https://raw.githubusercontent.com/dreamstaticsounds/sysex-bandit/main/docs/img/sysex-bandit-send.png", "https://github.com/dreamstaticsounds/sysex-bandit/releases/download/v1.0.1/Sysex-Bandit-1.0.1-mpc-armv7.zip", "plugins/sysex-bandit/thumb.png",
            "Send and receive MIDI System Exclusive from a track: pick or hand-edit a .syx message, aim it at one output device, listen on one input and save what arrives. Makes no sound.", "./plugins/sysex-bandit/install.sh", 0,
            0,
            false,
            false,
            "",
            { "midi", "sysex", "utility" }
        },
        {
            "tablor", "Tablor", "saustin2010", "v1.1.1", "Wavetable",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2300", "20.3 MB", "14%",
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
            "tinyfreaky", "TinyFreaky", "MagicStino", "v0.4.1", "Macro Hybrid",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2290", "5.0 MB", "14%",
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
            "tr-mpc", "TR-MPC", "sd88me", "v1.3.0", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-19",
            "4340", "79.1 MB", "25%",
            "sd88me - VST - TR-MPC", "trmpc.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/e92b2de018455a1ca4451d598e628e9ddbeb99ca/docs/screenshots/tr-mpc.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-vst-v1.3.0/TR-MPC-1.3.0-mpc-armv7.zip", "plugins/tr-mpc/thumb.png",
            "A modular 16-slot drum kit: each slot plays any of the 49 voices of athousanddetails' 6W6, 8W8, CW-78 and 9W9 (a 606 kick beside an 808 snare and 909 hats), with per-slot level, tune, decay, drive, pan and sends, one shared reverb, delay and master chain, a randomise module, and a touchscreen skin that reskins each slot to its voice's kit.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - TR-MPC\" && ZIP_PATH=\"./plugins/tr-mpc/tr-mpc.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-vst-v1.3.0/TR-MPC-1.3.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/tr-mpc\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-vst-v1.3.0/TR-MPC-1.3.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - TR-MPC/\"; elif [ -f \"./plugins/tr-mpc/trmpc.so\" ]; then cp -rf ./plugins/tr-mpc/* \"$TARGET_SYNTH_DIR/sd88me - VST - TR-MPC/\"; else echo \"[ERROR] No local binary archive found for tr-mpc\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "tr-606", "tr-808", "cr-78", "tr-909", "modular" }
        },
        {
            "tributary", "Tributary", "Stampman3000", "v1.2.6", "Utility",
            "INST", "Stable", "MPC One (3.10.1)", "MPC OS 3.x", "2026-11-20",
            "1842", "1.1 MB", "14%",
            "Stampman3000 - VST - Tributary", "tributary.so",
            "https://raw.githubusercontent.com/stampman3000-jpg/mpc-tributary/v1.2.6/screenshots/tributary.png", "https://github.com/stampman3000-jpg/mpc-tributary/releases/download/v1.2.6/Tributary-1.2.6-mpc-armv7.zip", "plugins/tributary/thumb.png",
            "Sums chosen Elektron Overbridge outputs onto one MPC track over USB. The machine is 48 kHz; the track resamples that to the MPC's 44.1. Capture saves the machine's own 48 kHz samples, locked to the bars. Based on Overwitch.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Stampman3000 - VST - Tributary\" && ZIP_PATH=\"./plugins/tributary/tributary.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/stampman3000-jpg/mpc-tributary/releases/download/v1.2.6/Tributary-1.2.6-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/tributary\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/stampman3000-jpg/mpc-tributary/releases/download/v1.2.6/Tributary-1.2.6-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Stampman3000 - VST - Tributary/\"; elif [ -f \"./plugins/tributary/tributary.so\" ]; then cp -rf ./plugins/tributary/* \"$TARGET_SYNTH_DIR/Stampman3000 - VST - Tributary/\"; else echo \"[ERROR] No local binary archive found for tributary\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "utility" }
        },
        {
            "tropix", "Tropix", "3dthc", "v0.1.6", "Sampler Rompler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-17",
            "2875", "215.4 MB", "14%",
            "3dthc - VST - Tropix", "tropix.so",
            "https://raw.githubusercontent.com/3dthc/Tropix/1005f9c4dc719f58bd17b02fac750457e6c8cb1e/docs/images/browse.jpg", "https://github.com/3dthc/Tropix/releases/download/v0.1.6/Tropix-0.1.6-mpc-armv7.zip", "plugins/tropix/thumb.png",
            "Island and afro ROMpler: 128 presets and 65 instruments for afrobeat, amapiano, kompa, soca, reggae and Hawaiian music - steel pans, log drums, saxes and trumpets, kompa and slack-key guitars, steels, keys, mallets, basses and percussion. Two layers, glide, horn scoop, vibrato, nine colorways. Also a VST3 (Mac, Windows) and Mac AU.", "./plugins/tropix/install.sh", 0,
            0,
            false,
            false,
            "",
            { "rompler", "afrobeat", "amapiano", "kompa", "soca", "reggae", "steel-pan", "horns" }
        },
        {
            "twenty", "Twenty", "Stampman3000", "v1.0.0", "Mono Bass",
            "INST", "Stable", "MPC One (3.10.1)", "MPC OS 3.x", "2026-11-20",
            "235", "1.8 MB", "14%",
            "Stampman3000 - VST - Twenty", "VST/twenty-arm32.so",
            "https://raw.githubusercontent.com/stampman3000-jpg/twenty/v1.0.0/screenshots/twenty.png", "https://github.com/stampman3000-jpg/twenty/releases/download/v1.0.0/Twenty-1.0.0-mpc-armv7.zip", "plugins/twenty/thumb.png",
            "Semi-modular MS-20-style synth with a 35-jack patchbay. Ported to MPC by Stampman3000; all synthesis DSP is Audio Tools' AT20V, unchanged.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Stampman3000 - VST - Twenty\" && ZIP_PATH=\"./plugins/twenty/twenty.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/stampman3000-jpg/twenty/releases/download/v1.0.0/Twenty-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/twenty\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/stampman3000-jpg/twenty/releases/download/v1.0.0/Twenty-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Stampman3000 - VST - Twenty/\"; elif [ -f \"./plugins/twenty/VST/twenty-arm32.so\" ]; then cp -rf ./plugins/twenty/* \"$TARGET_SYNTH_DIR/Stampman3000 - VST - Twenty/\"; else echo \"[ERROR] No local binary archive found for twenty\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "synth" }
        },
        {
            "vader", "VADER", "L'Cronx", "v0.10.7", "Macro Hybrid",
            "INST", "Stable", "Akai Force (3.9.1), MPC Live III (3.10.1)", "MPC OS 3.x", "2026-11-20",
            "8729", "42.3 MB", "14%",
            "L'Cronx - VST - VADER", "vader.so",
            "https://raw.githubusercontent.com/sunskiefer/VADER/v0.10.7/docs/img/main_star.png", "https://github.com/sunskiefer/VADER/releases/download/v0.10.7/VADER-0.10.7-mpc-armv7.zip", "plugins/vader/thumb.png",
            "An orbit-based vector synth: an orbiter travels a shape (built-in, maths, or your own SVG or OBJ) or one of 27 modes across four corners; a picture (PNG, JPG) under it plays the sound. Each corner plays overtone generators, one of 14 voices or a WAV sample, with its own envelope. Two timbres, arpeggiator, effects; its screen runs on your phone.", "./plugins/vader/install.sh", 0,
            0,
            false,
            false,
            "",
            { "synth", "vector", "orbit", "additive", "morphing", "arpeggiator", "svg", "3d" }
        },
        {
            "vavra", "Vavra", "sd88me", "v1.0.0", "Wavetable",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "820", "27.3 MB", "14%",
            "sd88me - VST - Vavra", "vavra.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-vavra/v1.0.0/docs/screenshots/main.png", "https://github.com/sd88me/mpc-vst-vavra/releases/download/v1.0.0/Vavra-1.0.0-mpc-aarch64.zip", "plugins/vavra/thumb.png",
            "An emulation of the Waldorf microQ's original OS (gearmulator) as a native instrument for Gen2 MPC devices, with a native screen in the look of its front panel (three skins), 300 ROM sounds plus your own sound sets, RANDOM, named favourites, Q-Link control and MPC tempo sync. Gen2 (aarch64) only; you need your own microQ ROM.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Vavra\" && ZIP_PATH=\"./plugins/vavra/vavra.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-vavra/releases/download/v1.0.0/Vavra-1.0.0-mpc-aarch64.zip\" ]; then mkdir -p \"./plugins/vavra\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-vavra/releases/download/v1.0.0/Vavra-1.0.0-mpc-aarch64.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Vavra/\"; elif [ -f \"./plugins/vavra/vavra.so\" ]; then cp -rf ./plugins/vavra/* \"$TARGET_SYNTH_DIR/sd88me - VST - Vavra/\"; else echo \"[ERROR] No local binary archive found for vavra\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "emulation", "wavetable", "rompler-free", "gen2" }
        },
        {
            "op1fx-voltage", "Voltage", "Bombest Audio", "v0.6.0", "Macro Hybrid",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1338", "4.6 MB", "14%",
            "Bombest Audio - VST - Voltage", "voltage.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/voltage.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Voltage-0.6.0-mpc-armv7.zip", "plugins/op1fx-voltage/thumb.png",
            "Four-control synth with eight voices and an amp envelope. OP-1 inspired.", "./plugins/op1fx-voltage/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "synth", "analog" }
        },
        {
            "warpline", "Warpline", "CLVRKE", "v1.2.0", "Wavetable",
            "INST", "Stable", "MPC Live III (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-22",
            "759", "168.8 MB", "14%",
            "CLVRKE - VST - Warpline", "VST/warpline-arm32.so",
            "https://raw.githubusercontent.com/CLVRK3/warpline-mpc/v1.1.0/docs/screenshots/warpline.png", "https://github.com/CLVRK3/warpline-mpc/releases/download/v1.2.0/Warpline-1.2.0-mpc-armv7.zip", "plugins/warpline/thumb.png",
            "Wavetable synth for standalone MPC built on the open-source Vital engine: plays .vital patches (add your own in <drive>/Warpline/Patches), 360 factory patches with genres, search and favourites, a wavetable picker, a generative 16-step arpeggiator, GRAINZ granular FX and a 16-slot mod matrix with random sources.", "./plugins/warpline/install.sh", 0,
            0,
            false,
            false,
            "",
            { "wavetable", "vital", "arpeggiator", "granular" }
        },
        {
            "welle", "Welle", "sd88me", "v0.3.1", "Wavetable",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-18",
            "1305", "2.9 MB", "14%",
            "sd88me - VST - Welle", "welle-arm64.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-welle/v0.2.0/docs/screenshots/main.png", "https://github.com/sd88me/mpc-vst-welle/releases/download/v0.3.1/Welle-0.3.1-mpc-aarch64.zip", "plugins/welle/thumb.png",
            "Waldorf Wave emulation running from the original Wave OS - developed by Mookid. Styled faithfully to the original panel. Needs your own Wave OS 1.700 files, uploaded from the Plugin Manager.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Welle\" && ZIP_PATH=\"./plugins/welle/welle.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-welle/releases/download/v0.3.1/Welle-0.3.1-mpc-aarch64.zip\" ]; then mkdir -p \"./plugins/welle\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-welle/releases/download/v0.3.1/Welle-0.3.1-mpc-aarch64.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Welle/\"; elif [ -f \"./plugins/welle/welle-arm64.so\" ]; then cp -rf ./plugins/welle/* \"$TARGET_SYNTH_DIR/sd88me - VST - Welle/\"; else echo \"[ERROR] No local binary archive found for welle\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "wavetable", "waldorf", "emulation" }
        },
        {
            "wreck", "Wreck", "L'Cronx", "v0.3.0", "Sampler Rompler",
            "INST", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-11-08",
            "1478", "3.0 MB", "14%",
            "L'Cronx - VST - Wreck", "wreck.so",
            "https://raw.githubusercontent.com/sunskiefer/Wreck/v0.3.0/docs/screenshots/page_0.png", "https://github.com/sunskiefer/Wreck/releases/download/v0.3.0/Wreck-0.3.0-mpc-armv7.zip", "plugins/wreck/thumb.png",
            "A self-sampling sound design instrument: ten sources (noise, clicks, your own WAV files from any folder or drive, FM, attractors and more) run through five blocks in any order into a 20 second loop that MIDI voices play back, and the voices can feed the chain again. 4-lane polymetric arp, bouncing-ball delay, 22 presets. MPC OS 3.x.", "./plugins/wreck/install.sh", 0,
            0,
            false,
            false,
            "",
            { "sampler", "sound-design", "experimental", "feedback", "generative", "arpeggiator", "chaos", "lo-fi" }
        },
        {
            "wurl", "Wurl", "saustin2010", "v1.1.1", "Keys Organ",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2795", "827 KB", "14%",
            "Filliformes - VST - [SYN] Wurl", "wurl.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-wurl/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-wurl/releases/download/v1.1.1/SYN-Wurl-1.1.1-mpc-armv7.zip", "plugins/wurl/thumb.png",
            "A physically modelled Wurlitzer 200A electric piano (OpenWurli) that barks with velocity, with tremolo and 10 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Wurl\" && ZIP_PATH=\"./plugins/wurl/wurl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-wurl/releases/download/v1.1.1/SYN-Wurl-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/wurl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-wurl/releases/download/v1.1.1/SYN-Wurl-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Wurl/\"; elif [ -f \"./plugins/wurl/wurl.so\" ]; then cp -rf ./plugins/wurl/* \"$TARGET_SYNTH_DIR/Filliformes - VST - [SYN] Wurl/\"; else echo \"[ERROR] No local binary archive found for wurl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "wurlitzer", "physical-modelling", "keys", "tremolo" }
        },
        {
            "xenia", "Xenia", "sd88me", "v1.1.0", "Wavetable",
            "INST", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-15",
            "2785", "24.8 MB", "14%",
            "sd88me - VST - Xenia", "xenia.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-xenia/v1.1.0/docs/screenshots/xenia-play.png", "https://github.com/sd88me/mpc-vst-xenia/releases/download/v1.1.0/Xenia-1.1.0-mpc-aarch64.zip", "plugins/xenia/thumb.png",
            "An emulation of the Microwave XT wavetable synthesizer's original firmware (gearmulator's 68k and DSP56300 emulators) for Gen2 MPC devices, with a PLAY page laid out like the XT's front panel, two skins, ROM banks, a patch browser, named favourites and Q-Link control on every page. Gen2 (aarch64) only; you need your own Microwave XT ROM.", "./plugins/xenia/install.sh", 0,
            0,
            false,
            false,
            "",
            { "emulation", "wavetable", "rompler-free", "gen2" }
        },
        {
            "xp-08", "XP-08", "Stephane Schmitz", "v1.0.0", "Sampler Rompler",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-18",
            "186", "1.1 MB", "14%",
            "Stephane Schmitz - VST - XP-08", "VST/xp-08-arm32.so",
            "https://raw.githubusercontent.com/eldontyrellcorp/mpc-plugin-xp08/v1.0.0/preview/01_PLAY.png", "https://github.com/eldontyrellcorp/mpc-plugin-xp08/releases/download/v1.0.0/XP-08-1.0.0-mpc-armv7.zip", "plugins/xp-08/thumb.png",
            "An XP-80-style PCM synth that plays Roland SR-JV80 boards from your own dumps: four board slots, their patches, four tones with pitch, filter and amp envelopes, LFOs, mod matrix, 12/24 dB and ladder filters, all 40 EFX types, chorus, reverb. Needs your own board (and optional JV-1080/JV-880) dumps. Akai MPC Plugin SDK; Gen1 and Gen2.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Stephane Schmitz - VST - XP-08\" && ZIP_PATH=\"./plugins/xp-08/xp-08.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/eldontyrellcorp/mpc-plugin-xp08/releases/download/v1.0.0/XP-08-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/xp-08\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/eldontyrellcorp/mpc-plugin-xp08/releases/download/v1.0.0/XP-08-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Stephane Schmitz - VST - XP-08/\"; elif [ -f \"./plugins/xp-08/VST/xp-08-arm32.so\" ]; then cp -rf ./plugins/xp-08/* \"$TARGET_SYNTH_DIR/Stephane Schmitz - VST - XP-08/\"; else echo \"[ERROR] No local binary archive found for xp-08\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "rompler", "polyphonic", "mod-matrix", "multi-fx" }
        },
        {
            "piamb", "πAMB", "Timo", "v0.1.0", "Experimental",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-17",
            "412", "595 KB", "14%",
            "Timo - VST - πAMB", "piamb.so",
            "https://raw.githubusercontent.com/timomacquis/piAMB-MPC/v0.1.0/docs/images/garden-force.png", "https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/piamb-0.1.0-762558519-mpc-armv7.zip", "plugins/piamb/thumb.png",
            "A generative ambient garden after Music for Airports: pad, bells on tape loops, bass and a soft pulse on one slowly drifting chord. Port of AMB-1 by Charles Vestal (charlesvestal/fm1-amb).", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Timo - VST - πAMB\" && ZIP_PATH=\"./plugins/piamb/piamb.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/piamb-0.1.0-762558519-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/piamb\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/piamb-0.1.0-762558519-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Timo - VST - πAMB/\"; elif [ -f \"./plugins/piamb/piamb.so\" ]; then cp -rf ./plugins/piamb/* \"$TARGET_SYNTH_DIR/Timo - VST - πAMB/\"; else echo \"[ERROR] No local binary archive found for piamb\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "generative", "ambient", "synth", "port" }
        },
        {
            "pimorph", "πMorpH", "Timo", "v0.2.0", "Wavetable",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-17",
            "1763", "50.8 MB", "14%",
            "Timo - VST - πMorpH", "pimorph.so",
            "https://raw.githubusercontent.com/timomacquis/piMorpH-MPC/v0.2.0/Cover.png", "https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/pimorph-0.2.0-762538867-mpc-armv7.zip", "plugins/pimorph/thumb.png",
            "Polyphonic XY wavetable synthesizer with paths, orbit, dual envelopes, HP/LP filters, two LFOs and an eight-slot modulation matrix. Includes 100 presets plus INIT and Custom Wavetable import: mono/stereo PCM16, PCM32 or float32 WAV, 512/1024/2048 samples per waveform.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Timo - VST - πMorpH\" && ZIP_PATH=\"./plugins/pimorph/pimorph.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/pimorph-0.2.0-762538867-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/pimorph\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/pimorph-0.2.0-762538867-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Timo - VST - πMorpH/\"; elif [ -f \"./plugins/pimorph/pimorph.so\" ]; then cp -rf ./plugins/pimorph/* \"$TARGET_SYNTH_DIR/Timo - VST - πMorpH/\"; else echo \"[ERROR] No local binary archive found for pimorph\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "synth", "wavetable", "polyphonic", "modulation" }
        },
        {
            "pitonic", "πToniC", "Timo", "v1.1.0", "Drum Machine",
            "INST", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "2030", "1.2 MB", "14%",
            "Timo - VST - πToniC", "pitonic.so",
            "https://raw.githubusercontent.com/timomacquis/piToniC-MPC/423643e60fcf0d5d5634e6d4eae5513f1a4edc76/Cover.jpg", "https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/pitonic-1.1.0-773197488-mpc-armv7.zip", "plugins/pitonic/thumb.png",
            "A Microtonic-inspired drum machine. πToniC features 8 independent drum voices, each combining filtered noise with a morphing oscillator or WAV sample playback. Its sound engine includes pitch modulation, amplitude envelopes, drive, EQ and individual voice controls. Shared stereo reverb and tempo-synced delay complete the engine.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Timo - VST - πToniC\" && ZIP_PATH=\"./plugins/pitonic/pitonic.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/pitonic-1.1.0-773197488-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/pitonic\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/pitonic-1.1.0-773197488-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Timo - VST - πToniC/\"; elif [ -f \"./plugins/pitonic/pitonic.so\" ]; then cp -rf ./plugins/pitonic/* \"$TARGET_SYNTH_DIR/Timo - VST - πToniC/\"; else echo \"[ERROR] No local binary archive found for pitonic\" && exit 1; fi && sync", 1,
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
            "acid", "Acid", "sd88me", "v3.0.1", "Generative",
            "MIDI", "Stable", "MPC Live III (Gen2) (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-16",
            "12341", "2.8 MB", "14%",
            "sd88me - VST - Acid", "acid.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-acid/master/docs/img/acid.png", "https://github.com/sd88me/mpc-vst-acid/releases/download/acid-vst-v3.0.1/Acid-3.0.1-mpc-armv7.zip", "plugins/acid/thumb.png",
            "TB-303-style acid bassline sequencer: two blendable lines with generate/mutate, driving a synth track over MIDI, with a touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Acid\" && ZIP_PATH=\"./plugins/acid/acid.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-acid/releases/download/acid-vst-v3.0.1/Acid-3.0.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/acid\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-acid/releases/download/acid-vst-v3.0.1/Acid-3.0.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Acid/\"; elif [ -f \"./plugins/acid/acid.so\" ]; then cp -rf ./plugins/acid/* \"$TARGET_SYNTH_DIR/sd88me - VST - Acid/\"; else echo \"[ERROR] No local binary archive found for acid\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "303", "acid", "midi", "bassline" }
        },
        {
            "cadenza", "Cadenza", "Shrekage", "v0.28.1", "Generative",
            "MIDI", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-11-14",
            "1448", "1.1 MB", "24%",
            "Cadenza - VST - Cadenza", "cadenza.so",
            "https://raw.githubusercontent.com/Shrekage2024/cadenza-force/v0.28.1/docs/screenshots/0260-play.png", "https://github.com/Shrekage2024/cadenza-force/releases/download/v0.28.1/Cadenza-0.28.1-mpc-armv7.zip", "plugins/cadenza/thumb.png",
            "Chord-driven generative MIDI sequencer: pick a key and scale, shape chords, bass and melody with a few knobs per page, and it generates and follows along as you play.", "./plugins/cadenza/install.sh", 0,
            0,
            false,
            false,
            "",
            { "chords", "generative", "midi", "bassline" }
        },
        {
            "chordsmith", "Chordsmith", "jacob-sabella", "v1.0.6", "Arp Chord",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-16",
            "7555", "4.2 MB", "8%",
            "jacob-sabella - VST - Chordsmith", "chordsmith.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-chordsmith/188d34ca0d9482d2d1ba840edb12ccd8c3773f76/docs/screenshots/chordsmith.png", "https://github.com/jacob-sabella/mpc-vst-chordsmith/releases/download/chordsmith-vst-v1.0.6/Chordsmith-1.0.6-mpc-armv7.zip", "plugins/chordsmith/thumb.png",
            "Chord tool: pads, keys or chord buttons play scale, set or custom chords with voicing, strum, arp and harp modes, MIDI out to other tracks and a built-in preview synth.", "./plugins/chordsmith/install.sh", 0,
            0,
            false,
            false,
            "",
            { "chords", "midi", "arpeggiator", "strum", "chord-builder" }
        },
        {
            "daemon", "Daemon", "sd88me", "v1.0.1", "Generative",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-21",
            "1685", "727 KB", "14%",
            "sd88me - VST - Daemon", "daemon.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-daemon/v1.0.0/docs/screenshots/daemon.png", "https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-1.0.1-mpc-armv7.zip", "plugins/daemon/thumb.png",
            "AI generative assistant: type or say what you hear and Daemon writes chords, basslines, melodies and beats in your key and plays them in time on its MIDI port. Agent mode builds the session too (tracks, instruments, presets, effects, clips) as a plan you RUN. Gemini or Claude; needs network and an API key.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Daemon\" && ZIP_PATH=\"./plugins/daemon/daemon.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-1.0.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/daemon\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-1.0.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Daemon/\"; elif [ -f \"./plugins/daemon/daemon.so\" ]; then cp -rf ./plugins/daemon/* \"$TARGET_SYNTH_DIR/sd88me - VST - Daemon/\"; else echo \"[ERROR] No local binary archive found for daemon\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "generative", "ai", "midi", "chords", "melody", "beats", "agent", "voice" }
        },
        {
            "grids", "Drum Map", "saustin2010", "v1.2.1", "Generative",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3413", "337 KB", "14%",
            "Émilie Gillet - VST - [SEQ] Drum Map", "grids.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-drum-map/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-drum-map/releases/download/v1.2.1/SEQ-Drum-Map-1.2.1-mpc-armv7.zip", "plugins/grids/thumb.png",
            "Drum Map: a topographic drum sequencer that plays kick, snare and hats from MPC's clock through its own MIDI port, from Émilie Gillet's open-source Grids.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SEQ] Drum Map\" && ZIP_PATH=\"./plugins/grids/grids.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-drum-map/releases/download/v1.2.1/SEQ-Drum-Map-1.2.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/grids\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-drum-map/releases/download/v1.2.1/SEQ-Drum-Map-1.2.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SEQ] Drum Map/\"; elif [ -f \"./plugins/grids/grids.so\" ]; then cp -rf ./plugins/grids/* \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [SEQ] Drum Map/\"; else echo \"[ERROR] No local binary archive found for grids\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "drum", "midi", "euclidean", "eurorack" }
        },
        {
            "rampage", "Dual Slope", "saustin2010", "v1.2.1", "Utility",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2865", "3.1 MB", "14%",
            "Andrew Belt - VST - [SEQ] Dual Slope", "rampage.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-dual-slope/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-dual-slope/releases/download/v1.2.1/SEQ-Dual-Slope-1.2.1-mpc-armv7.zip", "plugins/rampage/thumb.png",
            "Dual Slope, based on Befaco's Rampage VCV Rack module: two rise-and-fall generators (envelopes, LFOs, slew) whose outputs go out of its own MIDI port as CCs.", "./plugins/rampage/install.sh", 0,
            0,
            false,
            false,
            "",
            { "vcv", "envelope", "lfo", "slew", "midi-cc", "eurorack" }
        },
        {
            "eucalypso", "Eucalypso", "saustin2010", "v1.0.1", "Generative",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2085", "546 KB", "14%",
            "handcraftedcc - VST - [SEQ] Eucalypso", "eucalypso.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-eucalypso/v1.0.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-eucalypso/releases/download/v1.0.1/SEQ-Eucalypso-1.0.1-mpc-armv7.zip", "plugins/eucalypso/thumb.png",
            "Four-lane Euclidean sequencer that turns the notes you hold into interlocking rhythms for other tracks.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/handcraftedcc - VST - [SEQ] Eucalypso\" && ZIP_PATH=\"./plugins/eucalypso/eucalypso.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-eucalypso/releases/download/v1.0.1/SEQ-Eucalypso-1.0.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/eucalypso\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-eucalypso/releases/download/v1.0.1/SEQ-Eucalypso-1.0.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/handcraftedcc - VST - [SEQ] Eucalypso/\"; elif [ -f \"./plugins/eucalypso/eucalypso.so\" ]; then cp -rf ./plugins/eucalypso/* \"$TARGET_SYNTH_DIR/handcraftedcc - VST - [SEQ] Eucalypso/\"; else echo \"[ERROR] No local binary archive found for eucalypso\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "euclidean", "midi", "polyrhythm", "generative" }
        },
        {
            "euclidier", "Euclid", "sd88me", "v2.0.0", "Generative",
            "MIDI", "Stable", "Akai Force (MPC OS 3.9.1), MPC Live III (MPC OS 3.10)", "MPC OS 3.x", "2026-11-11",
            "8578", "425 KB", "14%",
            "sd88me - VST - Euclid", "euclid.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-euclid/master/docs/img/euclidier.png", "https://github.com/sd88me/mpc-vst-euclid/releases/download/euclid-v2.0.0/Euclid-2.0.0-mpc-armv7.zip", "plugins/euclidier/thumb.png",
            "8-lane Euclidean MIDI sequencer: live pattern circle and lane rows with a play-head, per-lane and global randomise, a drum-mode switch, and a touchscreen skin with Q-Links.", "./plugins/euclidier/install.sh", 0,
            0,
            false,
            false,
            "",
            { "euclidean", "midi", "drum", "polyrhythm" }
        },
        {
            "glass-spheres", "Glass Spheres", "Padbangers", "v1.1.0", "Generative",
            "MIDI", "Stable", "Akai MPC X (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-14",
            "2900", "6.6 MB", "14%",
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
            "groove-bank", "Groove Bank", "saustin2010", "v1.1.1", "Arp Chord",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3399", "852 KB", "14%",
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
            "maze-sequencer", "Maze Sequencer", "sd88me", "v2.0.0", "Generative",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-14",
            "8257", "4.8 MB", "14%",
            "sd88me - VST - Maze Sequencer", "maze_seq.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-maze/main/docs/img/maze-sequencer.png", "https://github.com/sd88me/mpc-vst-maze/releases/download/maze-sequencer-vst-v2.0.0/Maze-Sequencer-2.0.0-mpc-armv7.zip", "plugins/maze-sequencer/thumb.png",
            "Dual 8-step generative sequencer (Moog Labyrinth style) with corrupt, scale quantising, two LFOs and per-line MIDI channels, driving other tracks over MIDI.", "./plugins/maze-sequencer/install.sh", 0,
            0,
            false,
            false,
            "",
            { "generative", "midi", "moog", "labyrinth" }
        },
        {
            "midi-player", "MIDI Player", "saustin2010", "v1.1.1", "Utility",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2334", "144 KB", "14%",
            "Charles Vestal - VST - [SEQ] MIDI Player", "midiplayer.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-midiplayer/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-midiplayer/releases/download/v1.1.1/SEQ-MIDI-Player-1.1.1-mpc-armv7.zip", "plugins/midi-player/thumb.png",
            "Plays Standard MIDI Files in time with MPC's tempo and transport, one track or all, through its own MIDI port.", "./plugins/midi-player/install.sh", 0,
            0,
            false,
            false,
            "",
            { "midi", "midi-file", "player", "transport-sync" }
        },
        {
            "padstep-16", "PadStep 16", "Benoit", "v0.6.0", "Step Sequencer",
            "MIDI", "Stable", "MPC One G2 (3.10.1)", "MPC OS 3.x", "2026-11-15",
            "270", "3.9 MB", "14%",
            "Benoit - VST - PadStep 16", "padstep16.so",
            "https://raw.githubusercontent.com/frere12/padstep-16-mpc/v0.6.0/docs/screenshots/padstep-16.png", "https://github.com/frere12/padstep-16-mpc/releases/download/v0.6.0/PadStep-16-0.6.0-mpc-armv7.zip", "plugins/padstep-16/thumb.png",
            "16-step mono MIDI step sequencer played from the MPC pads: per-step tie, slide, ratchet and chance, velocity shapes, 16 slots and song-locked chains.", "./plugins/padstep-16/install.sh", 0,
            0,
            false,
            false,
            "",
            { "step-sequencer", "midi", "pads", "303", "ratchet" }
        },
        {
            "pixel-walkers", "Pixel Walkers", "saustin2010", "v1.1.1", "Generative",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2304", "375 KB", "14%",
            "mestela - VST - [SEQ] Pixel Walkers", "pixelwalkers.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-pixelwalkers/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-pixelwalkers/releases/download/v1.1.1/SEQ-Pixel-Walkers-1.1.1-mpc-armv7.zip", "plugins/pixel-walkers/thumb.png",
            "Generative gravity sequencer: each note you play becomes a walker that bounces and retriggers its note as it lands.", "./plugins/pixel-walkers/install.sh", 0,
            0,
            false,
            false,
            "",
            { "generative", "midi", "physics", "bouncing" }
        },
        {
            "steprunner-8", "StepRunner-8", "djokenzi", "v1.2.0", "Step Sequencer",
            "MIDI", "Stable", "MPC Live (3.9.1.2)", "MPC OS 3.x", "2026-10-19",
            "3347", "3.4 MB", "14%",
            "djokenzi - VST - StepRunner-8", "steprunner8.so",
            "https://raw.githubusercontent.com/djokenzi/steprunner-8/76285a3aa34dcdc85bf8e72e3aa18baa7f6428cd/docs/screenshots/steprunner-8.png", "https://github.com/djokenzi/steprunner-8/releases/download/v1.2.0/StepRunner-8-1.2.0-mpc-armv7.zip", "plugins/steprunner-8/thumb.png",
            "8-track, 64-step sequencer with step conditions, step and CC locks, patterns, states and performance effects, driving a drum program over MIDI.", "./plugins/steprunner-8/install.sh", 0,
            0,
            false,
            false,
            "",
            { "midi", "step-sequencer", "drums", "step-locks" }
        },
        {
            "stevequencer", "Stevequencer", "saustin2010", "v1.1.1", "Step Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-24",
            "2655", "1.2 MB", "14%",
            "Steve A - VST - [SEQ] Stevequencer", "stevequencer.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-stevequencer/v1.0.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-stevequencer/releases/download/v1.1.1/SEQ-Stevequencer-1.1.1-mpc-armv7.zip", "plugins/stevequencer/thumb.png",
            "Melodic step sequencer edited from the Q-Links: 16 steps on a 4x4 grid, up to 64, with chance, ratchets and MOD lanes.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Steve A - VST - [SEQ] Stevequencer\" && ZIP_PATH=\"./plugins/stevequencer/stevequencer.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-stevequencer/releases/download/v1.1.1/SEQ-Stevequencer-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/stevequencer\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-stevequencer/releases/download/v1.1.1/SEQ-Stevequencer-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Steve A - VST - [SEQ] Stevequencer/\"; elif [ -f \"./plugins/stevequencer/stevequencer.so\" ]; then cp -rf ./plugins/stevequencer/* \"$TARGET_SYNTH_DIR/Steve A - VST - [SEQ] Stevequencer/\"; else echo \"[ERROR] No local binary archive found for stevequencer\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "step-sequencer", "midi", "q-links", "melodic" }
        },
        {
            "stevequencer-16", "Stevequencer 16", "saustin2010", "v1.1.1", "Step Sequencer",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-24",
            "2554", "628 KB", "14%",
            "Steve A - VST - [SEQ] Stevequencer 16", "stevequencer16.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-stevequencer16/v1.0.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-stevequencer16/releases/download/v1.1.1/SEQ-Stevequencer-16-1.1.1-mpc-armv7.zip", "plugins/stevequencer-16/thumb.png",
            "16-step sequencer with eight modulation lanes that move any parameter of the instrument (CC or NRPN), on any page.", "./plugins/stevequencer-16/install.sh", 0,
            0,
            false,
            false,
            "",
            { "step-sequencer", "midi", "modulation", "nrpn", "q-links" }
        },
        {
            "super-arp", "Super Arp", "saustin2010", "v1.1.1", "Arp Chord",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2622", "2.3 MB", "14%",
            "handcraftedcc - VST - [SEQ] Super Arp", "superarp.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-superarp/v1.1.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-superarp/releases/download/v1.1.1/SEQ-Super-Arp-1.1.1-mpc-armv7.zip", "plugins/super-arp/thumb.png",
            "Arpeggiator that splits note order from rhythm: 40 patterns, 40 rhythms and seeded random modifiers.", "./plugins/super-arp/install.sh", 0,
            0,
            false,
            false,
            "",
            { "arpeggiator", "midi", "patterns", "rhythm" }
        },
        {
            "piclidean-mpc", "πClideaN", "Timo", "v1.3.8", "Generative",
            "MIDI", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-17",
            "2659", "1.9 MB", "14%",
            "Timo - VST - πClideaN", "piclidean_mpc.so",
            "https://raw.githubusercontent.com/timomacquis/piClideaN-MPC/657e7ca8ac8139eeb1678f8639afa2b40ac7faa0/Cover.png", "https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/piclidean-mpc-1.3.8-762530960-mpc-armv7.zip", "plugins/piclidean-mpc/thumb.png",
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
            "airwindows-air3", "Airwindows Air3", "mmiroshnikov", "v1.0.0", "Filter Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "2352", "745 KB", "14%",
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
            "airwindows-buttercomp2", "Airwindows ButterComp2", "mmiroshnikov", "v1.0.0", "Dynamics",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "2322", "757 KB", "14%",
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
            "2213", "702 KB", "14%",
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
            "airwindows-chorusensemble", "Airwindows ChorusEnsemble", "mmiroshnikov", "v1.0.0", "Modulation",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "2226", "647 KB", "14%",
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
            "1340", "10.6 MB", "14%",
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
            "1321", "15.2 MB", "14%",
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
            "1241", "10.5 MB", "14%",
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
            "1248", "10.0 MB", "14%",
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
            "1271", "13.8 MB", "14%",
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
            "aw-effects", "Airwindows Collection: Effects", "sd88me", "v0.1.0", "Multi Fx",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "1304", "12.2 MB", "14%",
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
            "aw-filter", "Airwindows Collection: Filter", "sd88me", "v0.1.0", "Filter Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "1219", "13.7 MB", "14%",
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
            "1357", "10.7 MB", "14%",
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
            "aw-saturation", "Airwindows Collection: Saturation", "sd88me", "v0.1.0", "Distortion",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "1300", "10.9 MB", "14%",
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
            "aw-tape", "Airwindows Collection: Tape", "sd88me", "v0.1.0", "Lofi Tape",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "1292", "10.6 MB", "14%",
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
            "1158", "13.5 MB", "14%",
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
            "airwindows-console7channel", "Airwindows Console7Channel", "mmiroshnikov", "v1.0.0", "Distortion",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "2132", "737 KB", "14%",
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
            "airwindows-density2", "Airwindows Density2", "mmiroshnikov", "v1.0.0", "Distortion",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "2211", "648 KB", "14%",
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
            "2337", "835 KB", "14%",
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
            "2181", "816 KB", "14%",
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
            "2226", "692 KB", "14%",
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
            "airwindows-galacticvibe", "Airwindows GalacticVibe", "mmiroshnikov", "v1.0.0", "Modulation",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "2065", "702 KB", "14%",
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
            "2167", "622 KB", "14%",
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
            "2138", "721 KB", "14%",
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
            "2088", "599 KB", "14%",
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
            "2176", "715 KB", "14%",
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
            "airwindows-mackity", "Airwindows Mackity", "mmiroshnikov", "v1.0.0", "Distortion",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-05",
            "1964", "587 KB", "14%",
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
            "2104", "730 KB", "14%",
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
            "2169", "618 KB", "14%",
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
            "2244", "548 KB", "14%",
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
            "airwindows-tovinyl4", "Airwindows ToVinyl4", "mmiroshnikov", "v1.0.0", "Lofi Tape",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "2092", "738 KB", "14%",
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
            "2034", "779 KB", "14%",
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
            "airwindows-zacidlowpass", "Airwindows ZAcidLowpass", "mmiroshnikov", "v1.0.0", "Filter Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "2015", "685 KB", "14%",
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
            "airwindows-zlowpass2", "Airwindows ZLowpass2", "mmiroshnikov", "v1.0.0", "Filter Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-03",
            "2011", "694 KB", "14%",
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
            "alpha", "ALPHA", "L'Cronx", "v1.2.1", "Pitch Vocal",
            "FX", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-11-11",
            "1460", "7.6 MB", "14%",
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
            "argevox-geraet", "Argevox Gerät", "Argerax", "v0.2.1", "Pitch Vocal",
            "FX", "Beta", "MPC One+ (3.10.1)", "MPC OS 3.x", "2026-11-16",
            "125", "525 KB", "14%",
            "Argerax - VST - Argevox Gerät", "VST/argevox-ger-te-arm32.so",
            "https://raw.githubusercontent.com/ymn27nhnwd-hue/argevox-geraet/v0.2.1/docs/interfaz.png", "https://github.com/ymn27nhnwd-hue/argevox-geraet/releases/download/v0.2.1/Argevox-Ger-t-0.2.1-mpc-armv7.zip", "plugins/argevox-geraet/thumb.png",
            "20-band live vocoder inspired by the Sennheiser VSM-201, with a voice gate, de-esser, and touch-adjustable band levels. Routes the carrier and microphone into opposite channels of a stereo submix.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Argerax - VST - Argevox Gerät\" && ZIP_PATH=\"./plugins/argevox-geraet/argevox-geraet.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/ymn27nhnwd-hue/argevox-geraet/releases/download/v0.2.1/Argevox-Ger-t-0.2.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/argevox-geraet\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/ymn27nhnwd-hue/argevox-geraet/releases/download/v0.2.1/Argevox-Ger-t-0.2.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Argerax - VST - Argevox Gerät/\"; elif [ -f \"./plugins/argevox-geraet/VST/argevox-ger-te-arm32.so\" ]; then cp -rf ./plugins/argevox-geraet/* \"$TARGET_SYNTH_DIR/Argerax - VST - Argevox Gerät/\"; else echo \"[ERROR] No local binary archive found for argevox-geraet\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "vocoder", "voice" }
        },
        {
            "binaural-echo", "Binaural Echo", "Lightlike", "v1.0.0", "Delay",
            "FX", "Stable", "MPC One (3.10.1)", "MPC OS 3.x", "2026-11-19",
            "172", "10.5 MB", "14%",
            "Lightlike - VST - Binaural Echo", "VST/binaural-echo-arm32.so",
            "https://raw.githubusercontent.com/lightlikemusic/binaural-echo/v1.0.0/docs/screenshot.png", "https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/binaural-echo-1.0.0-769711994-mpc-armv7.zip", "plugins/binaural-echo/thumb.png",
            "Binaural echoes and 3D panning. Every repeat turns and tilts through the sound field, so delays circle you, bounce from front to back or trail behind a moving source, free or tempo-synced. Place the sound with Q-Links for azimuth, elevation and distance, or let it orbit, swing or fly by. Made for headphones.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Lightlike - VST - Binaural Echo\" && ZIP_PATH=\"./plugins/binaural-echo/binaural-echo.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/binaural-echo-1.0.0-769711994-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/binaural-echo\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/binaural-echo-1.0.0-769711994-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Lightlike - VST - Binaural Echo/\"; elif [ -f \"./plugins/binaural-echo/VST/binaural-echo-arm32.so\" ]; then cp -rf ./plugins/binaural-echo/* \"$TARGET_SYNTH_DIR/Lightlike - VST - Binaural Echo/\"; else echo \"[ERROR] No local binary archive found for binaural-echo\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "echo", "delay", "binaural", "spatial", "tempo-sync", "headphones", "ambisonics", "panner" }
        },
        {
            "boris-granular", "Boris Granular", "sd88me", "v1.0.1", "Granular",
            "FX", "Stable", "Akai Force (MPC OS 3.9.1)", "MPC OS 3.x", "2026-10-21",
            "2994", "10.3 MB", "14%",
            "sd88me - VST - Boris Granular", "boris_granular.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-boris/2d7b1360c68ccc91b22b0c5030152e08dd3682b8/docs/screenshot-force.png", "https://github.com/sd88me/mpc-vst-boris/releases/download/v1.0.1/Boris-Granular-1.0.1-mpc-armv7.zip", "plugins/boris-granular/thumb.png",
            "Real-time granular insert effect: incoming audio fills a 10 s buffer and up to 24 overlapping grains replay it with random size, delay, pitch, direction and pan. MPC tempo sync, freeze, feedback, 22 presets and a one-page touchscreen skin with Q-Links. A port of Boris Granular Station by Alessandro Gaiba.", "./plugins/boris-granular/install.sh", 0,
            0,
            false,
            false,
            "",
            { "granular", "freeze", "tempo-sync", "texture", "pads", "shimmer", "stutter" }
        },
        {
            "clif-tape", "Clif Tape", "MagicStino", "v0.2.0", "Lofi Tape",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "1291", "1.6 MB", "14%",
            "MagicStino - VST - Clif Tape", "clif_tape.so",
            "https://raw.githubusercontent.com/MagicStino/Clif-Tape/v0.2.0/catalog-thumbnail.png", "https://github.com/MagicStino/Clif-Tape/releases/download/v0.2.0/Clif-Tape-0.2.0-mpc-armv7.zip", "plugins/clif-tape/thumb.png",
            "Stereo tape colour and digital degradation, expressive Wow/Flutter, twelve tone profiles, crackle, stereo spread, independent noise textures and Silence Gate. Fifteen presets and touchscreen Q-Links.", "./plugins/clif-tape/install.sh", 0,
            0,
            false,
            false,
            "",
            { "tape", "lo-fi", "stereo", "effect" }
        },
        {
            "costello-reverb", "Costello Reverb", "ikloX", "v1.0.2", "Reverb",
            "FX", "Stable", "MPC Live (3.10.1)", "MPC OS 3.x", "2026-11-23",
            "66", "905 KB", "5%",
            "ikloX - VST - Costello Reverb", "costello_reverb.so",
            "https://raw.githubusercontent.com/biloo77-cell/mpc-vst-costello-reverb/d7ed7c4da2f092a25fe248ff71ad27defb066c5b/docs/img/costello-reverb.png", "https://github.com/biloo77-cell/mpc-vst-costello-reverb/releases/download/costello-reverb-v1.0.2/Costello-Reverb-1.0.2-mpc-armv7.zip", "plugins/costello-reverb/thumb.png",
            "Sean Costello's 8-line FDN reverb (Soundpipe revsc, as in AudioKit's CostelloReverb), bit-identical to the original, with low cut, high cut, verb size, mix and output.", "./plugins/costello-reverb/install.sh", 0,
            0,
            false,
            false,
            "",
            { "reverb", "fdn", "stereo", "soundpipe" }
        },
        {
            "crate-press", "Crate Press", "kim", "v0.2.6", "Dynamics",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "239", "1.5 MB", "14%",
            "kim - VST - Crate Press", "VST/crate-press-arm32.so",
            "https://raw.githubusercontent.com/Sunghyun23/kimswork/crate-press/v0.2.6/mpc-software/plugins/public/CratePress/design/device-0.2.6/footer-crate-final.png", "https://github.com/Sunghyun23/kimswork/releases/download/crate-press/v0.2.6/crate-press-0.2.6-mpc-armv7.zip", "plugins/crate-press/thumb.png",
            "Stereo character and three-band ducking compressor with eight latched/momentary effects and sound Dice. UI checked on MPC XL OS3.10.1.12; current physical controls/audio not certified. Requires MPC OS3.10.1+.", "./plugins/crate-press/install.sh", 0,
            0,
            false,
            false,
            "",
            { "compressor", "multiband", "lo-fi", "performance" }
        },
        {
            "ctrl-mixbus", "CTRL MixBus", "tonyalonsotech", "v2.2.0", "Mixing",
            "FX", "Stable", "MPC One (Gen2) (3.10.1), MPC Live III (Gen2) (3.10.1), Force (Gen1) (3.10.1)", "MPC OS 3.x", "2026-11-14",
            "1614", "9.8 MB", "14%",
            "CTRL - VST - CTRL MixBus", "ctrl_mixbus.so",
            "https://raw.githubusercontent.com/tonyalonsotech/CTRL-MixBus/v2.1.0/docs/preview.png", "https://github.com/tonyalonsotech/CTRL-MixBus/releases/download/v2.2.0/CTRL-MixBus-2.2.0-mpc-armv7.zip", "plugins/ctrl-mixbus/thumb.png",
            "CTRL Mixbus is a professional stereo mixing and processing effect designed to streamline your workflow across three focused pages. Take control of input gain, stereo width, equalization, precision filtering, dynamic gating, and loudness maximization.", "./plugins/ctrl-mixbus/install.sh", 0,
            0,
            false,
            false,
            "",
            { "mixing", "dj-eq", "isolator", "filter", "stereo-width", "gate", "maximizer", "metering" }
        },
        {
            "op1fx-cwo", "Cwo", "Bombest Audio", "v0.6.0", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1623", "4.2 MB", "14%",
            "Bombest Audio - VST - Cwo", "cwo.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/cwo.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Cwo-0.6.0-mpc-armv7.zip", "plugins/op1fx-cwo/thumb.png",
            "Stereo delay with a frequency shifter in the feedback loop, plus a MIX knob. OP-1 inspired.", "./plugins/op1fx-cwo/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "delay", "frequency-shifter", "feedback" }
        },
        {
            "daemon-listen", "Daemon Listen", "sd88me", "v1.0.1", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-21",
            "1360", "64 KB", "14%",
            "sd88me - VST - Daemon Listen", "daemonlisten.so",
            "", "https://github.com/sd88me/mpc-vst-daemon/releases/download/v1.0.1/Daemon-Listen-1.0.1-mpc-armv7.zip", "plugins/daemon-listen/thumb.png",
            "Companion to Daemon: insert it on the audio track that carries your mic, and Daemon's TALK button hears you. Passes audio through unchanged. Needs Daemon in the same project.", "./plugins/daemon-listen/install.sh", 0,
            0,
            false,
            false,
            "",
            { "voice", "mic", "ai" }
        },
        {
            "spectral-sidechain", "DAVIS Spectral Sidechain", "elizmita", "v1.6.7", "Filter Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-17",
            "1092", "2.9 MB", "14%",
            "DAVIS - VST - Spectral Sidechain", "spectral_sidechain.so",
            "https://raw.githubusercontent.com/elizmita/davis-spectral-sidechain/spectral-sidechain-vst-v1.4.2/docs/preview.png", "https://github.com/elizmita/davis-spectral-sidechain/releases/download/spectral-sidechain-vst-v1.6.7/Spectral-Sidechain-1.6.7-mpc-armv7.zip", "plugins/spectral-sidechain/thumb.png",
            "One-band sidechain dynamic EQ: the kick ducks the bass frequencies, with a live spectrum display and a touchscreen skin.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/DAVIS - VST - Spectral Sidechain\" && ZIP_PATH=\"./plugins/spectral-sidechain/spectral-sidechain.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/elizmita/davis-spectral-sidechain/releases/download/spectral-sidechain-vst-v1.6.7/Spectral-Sidechain-1.6.7-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/spectral-sidechain\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/elizmita/davis-spectral-sidechain/releases/download/spectral-sidechain-vst-v1.6.7/Spectral-Sidechain-1.6.7-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/DAVIS - VST - Spectral Sidechain/\"; elif [ -f \"./plugins/spectral-sidechain/spectral_sidechain.so\" ]; then cp -rf ./plugins/spectral-sidechain/* \"$TARGET_SYNTH_DIR/DAVIS - VST - Spectral Sidechain/\"; else echo \"[ERROR] No local binary archive found for spectral-sidechain\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "sidechain", "dynamic-eq", "eq" }
        },
        {
            "op1fx-delay", "Delay", "Bombest Audio", "v0.6.0", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1723", "4.2 MB", "14%",
            "Bombest Audio - VST - Delay", "delay.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/delay.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Delay-0.6.0-mpc-armv7.zip", "plugins/op1fx-delay/thumb.png",
            "Four-knob delay with a 12-preset bank. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Delay\" && ZIP_PATH=\"./plugins/op1fx-delay/op1fx-delay.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Delay-0.6.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-delay\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Delay-0.6.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Delay/\"; elif [ -f \"./plugins/op1fx-delay/delay.so\" ]; then cp -rf ./plugins/op1fx-delay/* \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Delay/\"; else echo \"[ERROR] No local binary archive found for op1fx-delay\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "delay", "echo", "tape" }
        },
        {
            "djfx-looper", "DJFX Looper Native", "Rare Data", "v1.1.3", "Glitch Performance",
            "FX", "Stable", "Akai Force (3.10.0.49)", "MPC OS 3.x", "2026-11-02",
            "830", "1.6 MB", "1%",
            "Rare Data - VST - DJFX Looper Native", "DJFXLooperNative.so",
            "", "https://github.com/macdigi/djfx-looper/releases/download/v1.1.3/DJFX-Looper-Native-1.1.3-mpc-armv7.zip", "plugins/djfx-looper/thumb.png",
            "Three Q-Link knobs for short loops, speed bends and reverse playback, with FX bypass and an on-device quick guide.", "./plugins/djfx-looper/install.sh", 0,
            0,
            false,
            false,
            "",
            { "looper", "reverse", "performance" }
        },
        {
            "dragonfly-early-reflections", "Dragonfly Early Reflections", "gmorb", "v1.3.1", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "1163", "766 KB", "14%",
            "Dragonfly - VST - Early Reflections", "dragonfly_early.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-early-reflections/2044beed9f510a3963df7b61da6a4c19c6c7e7b3/docs/banners/early.png", "https://github.com/gmorb/mpc-vst-dragonfly-early-reflections/releases/download/v1.3.1/Early-Reflections-1.3.1-mpc-armv7.zip", "plugins/dragonfly-early-reflections/thumb.png",
            "Dragonfly Early Reflections as a native insert effect, with a touchscreen skin and Q-Links.", "./plugins/dragonfly-early-reflections/install.sh", 0,
            0,
            false,
            false,
            "",
            { "reverb", "early-reflections", "dragonfly" }
        },
        {
            "dragonfly-hall", "Dragonfly Hall", "gmorb", "v1.3.0", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "2609", "2.4 MB", "14%",
            "Dragonfly - VST - Hall", "dragonfly_hall.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-hall/eb9f41f22ea93414776db40760c2387b36cf3144/docs/banners/hall.png", "https://github.com/gmorb/mpc-vst-dragonfly-hall/releases/download/v1.3.0/Hall-1.3.0-mpc-armv7.zip", "plugins/dragonfly-hall/thumb.png",
            "Dragonfly Hall reverb as a native insert effect, with a touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Dragonfly - VST - Hall\" && ZIP_PATH=\"./plugins/dragonfly-hall/dragonfly-hall.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/gmorb/mpc-vst-dragonfly-hall/releases/download/v1.3.0/Hall-1.3.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/dragonfly-hall\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/gmorb/mpc-vst-dragonfly-hall/releases/download/v1.3.0/Hall-1.3.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Dragonfly - VST - Hall/\"; elif [ -f \"./plugins/dragonfly-hall/dragonfly_hall.so\" ]; then cp -rf ./plugins/dragonfly-hall/* \"$TARGET_SYNTH_DIR/Dragonfly - VST - Hall/\"; else echo \"[ERROR] No local binary archive found for dragonfly-hall\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "reverb", "hall", "dragonfly" }
        },
        {
            "dragonfly-plate", "Dragonfly Plate", "gmorb", "v1.3.0", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "2464", "1.2 MB", "14%",
            "Dragonfly - VST - Plate", "dragonfly_plate.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-plate/a9087385948d18ae38739c44ddf42ab826d1a567/docs/banners/plate.png", "https://github.com/gmorb/mpc-vst-dragonfly-plate/releases/download/v1.3.0/Plate-1.3.0-mpc-armv7.zip", "plugins/dragonfly-plate/thumb.png",
            "Dragonfly Plate reverb as a native insert effect, with a touchscreen skin and Q-Links.", "./plugins/dragonfly-plate/install.sh", 0,
            0,
            false,
            false,
            "",
            { "reverb", "plate", "dragonfly" }
        },
        {
            "dragonfly-room", "Dragonfly Room", "gmorb", "v1.3.0", "Reverb",
            "FX", "Stable", "Akai Force (3.9.1), MPC Live III (Gen2) (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "2395", "2.0 MB", "14%",
            "Dragonfly - VST - Room", "dragonfly_room.so",
            "https://raw.githubusercontent.com/gmorb/mpc-vst-dragonfly-room/6c62e1a5c9ef57f0c31897afa0faf07d7a446585/docs/banners/room.png", "https://github.com/gmorb/mpc-vst-dragonfly-room/releases/download/v1.3.0/Room-1.3.0-mpc-armv7.zip", "plugins/dragonfly-room/thumb.png",
            "Dragonfly Room reverb as a native insert effect, with a touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Dragonfly - VST - Room\" && ZIP_PATH=\"./plugins/dragonfly-room/dragonfly-room.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/gmorb/mpc-vst-dragonfly-room/releases/download/v1.3.0/Room-1.3.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/dragonfly-room\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/gmorb/mpc-vst-dragonfly-room/releases/download/v1.3.0/Room-1.3.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Dragonfly - VST - Room/\"; elif [ -f \"./plugins/dragonfly-room/dragonfly_room.so\" ]; then cp -rf ./plugins/dragonfly-room/* \"$TARGET_SYNTH_DIR/Dragonfly - VST - Room/\"; else echo \"[ERROR] No local binary archive found for dragonfly-room\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "reverb", "room", "dragonfly" }
        },
        {
            "drips", "Drips", "Padbangers", "v1.1.0", "Granular",
            "FX", "Stable", "Akai MPC X (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-14",
            "2825", "3.6 MB", "14%",
            "Padbangers - VST - Drips", "VST/drips-arm32.so",
            "https://raw.githubusercontent.com/padbangers/drips/85683cebf7b4fcf1716ed6f32faa89041768b24e/docs/drips-main.png", "https://github.com/padbangers/drips/releases/download/drips-v1.1.0/Drips-1.1.0-mpc-armv7.zip", "plugins/drips/thumb.png",
            "Drips: a granular texture synthesizer with four recording qualities, a delay and beat slicer, tempo-synced seeding, wavetables and a modulation page, from Émilie Gillet's open-source Beads.", "./plugins/drips/install.sh", 0,
            0,
            false,
            false,
            "",
            { "granular", "texture", "delay", "beat-slicer", "wavetable", "reverb", "tempo-sync" }
        },
        {
            "dub-station", "Dub Station", "nachtaktiv303", "v1.0.2", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3648", "2.1 MB", "14%",
            "Lukas - VST - Dub Station", "dub_station.so",
            "https://raw.githubusercontent.com/nachtaktiv303/mpc-vst-dub-station/main/preview_0.png", "https://github.com/nachtaktiv303/mpc-vst-dub-station/releases/download/dub-station-vst-v1.0.2/Dub-Station-1.0.2-mpc-armv7.zip", "plugins/dub-station/thumb.png",
            "Dub-techno multi-effect: two independent tape delays, a huge modulated reverb, tape grit, a noise/crackle ambience bed, phaser, ducking and a panic kill.", "./plugins/dub-station/install.sh", 0,
            0,
            false,
            false,
            "",
            { "dub", "delay", "tape", "reverb", "space-echo", "multi-effect" }
        },
        {
            "effectwerk", "EffectWerk", "Devko", "v0.0.5", "Glitch Performance",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "1025", "3.0 MB", "14%",
            "Devko - VST - EffectWerk", "effectwerk.so",
            "https://raw.githubusercontent.com/Devko/EffectWerk/v0.0.1/docs/img/looper.png", "https://github.com/Devko/EffectWerk/releases/download/v0.0.5/EffectWerk-0.0.5-mpc-armv7.zip", "plugins/effectwerk/thumb.png",
            "A live looper and performance mixer in one insert slot: record bars on MPC's grid, then roll, slow, reverse and layer them, and crossfade two scenes from 64 one-tap effects. Plus a ten-module effect rack with macros, LFOs and a mod matrix, 62 presets and a 14-page touchscreen skin with Q-Links.", "./plugins/effectwerk/install.sh", 0,
            0,
            false,
            false,
            "",
            { "looper", "performance-mixer", "octatrack-style", "crossfader", "scenes", "effect-rack", "granular", "shimmer-reverb" }
        },
        {
            "eq-artist", "EQ Artist", "Slow Ops Music", "v1.0.0", "Filter Eq",
            "FX", "Stable", "MPC Live III (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-15",
            "678", "503 KB", "14%",
            "Slow Ops - VST - EQ Artist Dots", "VST/eqartist-dots-arm32.so",
            "https://raw.githubusercontent.com/drj-io/SlowOps.MPC.EQ.Artist/v1.0.0/docs/images/para-page.png", "https://github.com/drj-io/SlowOps.MPC.EQ.Artist/releases/download/v1.0.0/EQ-Artist-1.0.0-mpc-armv7.zip", "plugins/eq-artist/thumb.png",
            "8-band EQ you edit two ways over a live spectrum: drag colour-coded band nodes, or paint the curve with a finger and it becomes real bands. Bell, shelves, 6 to 48 dB/oct cuts and notch, with zero-latency filters that keep their analog shape near 20 kHz, undo/redo, and a pre/post analyser.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Slow Ops - VST - EQ Artist Dots\" && ZIP_PATH=\"./plugins/eq-artist/eq-artist.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/drj-io/SlowOps.MPC.EQ.Artist/releases/download/v1.0.0/EQ-Artist-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/eq-artist\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/drj-io/SlowOps.MPC.EQ.Artist/releases/download/v1.0.0/EQ-Artist-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Slow Ops - VST - EQ Artist Dots/\"; elif [ -f \"./plugins/eq-artist/VST/eqartist-dots-arm32.so\" ]; then cp -rf ./plugins/eq-artist/* \"$TARGET_SYNTH_DIR/Slow Ops - VST - EQ Artist Dots/\"; else echo \"[ERROR] No local binary archive found for eq-artist\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "eq", "parametric-eq", "analyzer", "mixing", "touch" }
        },
        {
            "fastg-bitcrusher", "FAST G BITCRUSHER – VICTORY EDITION", "FAST G STUDIOS", "v1.0.0", "Lofi Tape",
            "FX", "Stable", "Akai MPC One (Gen1) (3.10.1)", "MPC OS 3.x", "2026-11-17",
            "324", "1.4 MB", "14%",
            "FAST G STUDIOS - VST - FAST G BITCRUSHER", "VST/fastg-bitcrusher-arm32.so",
            "https://raw.githubusercontent.com/fastgbeats-cmd/FASTG-BITCRUSHER/f4ae3501af90416a649019968b4e9bca5f04acdc/NAHLED.png", "https://github.com/fastgbeats-cmd/FASTG-BITCRUSHER/releases/download/v1.0.0/FAST-G-BITCRUSHER-1.0.0-mpc-armv7.zip", "plugins/fastg-bitcrusher/thumb.png",
            "Free vintage bitcrusher audio effect for Akai MPC.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/FAST G STUDIOS - VST - FAST G BITCRUSHER\" && ZIP_PATH=\"./plugins/fastg-bitcrusher/fastg-bitcrusher.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/fastgbeats-cmd/FASTG-BITCRUSHER/releases/download/v1.0.0/FAST-G-BITCRUSHER-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/fastg-bitcrusher\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/fastgbeats-cmd/FASTG-BITCRUSHER/releases/download/v1.0.0/FAST-G-BITCRUSHER-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/FAST G STUDIOS - VST - FAST G BITCRUSHER/\"; elif [ -f \"./plugins/fastg-bitcrusher/VST/fastg-bitcrusher-arm32.so\" ]; then cp -rf ./plugins/fastg-bitcrusher/* \"$TARGET_SYNTH_DIR/FAST G STUDIOS - VST - FAST G BITCRUSHER/\"; else echo \"[ERROR] No local binary archive found for fastg-bitcrusher\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "bitcrusher", "lofi", "distortion", "vintage" }
        },
        {
            "ghost", "GHOST", "MPC-EURO", "v2.4.2", "Glitch Performance",
            "FX", "Beta", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-29",
            "822", "812 KB", "14%",
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
            "gpp-link", "GPP Link", "BeatsbyLonza", "v1.2.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-21",
            "427", "712 KB", "14%",
            "BeatsbyLonza - VST - GPP Link", "VST/gpp-link-arm32.so",
            "https://raw.githubusercontent.com/zay2001-beatsbylonza/GPP-Link/9b75e9a72bb4c2fc4dd1fcff16b85fd4c2b02c95/docs/screenshot-light.png", "https://github.com/zay2001-beatsbylonza/GPP-Link/releases/download/v1.2.0/GPP-Link-1.2.0-mpc-armv7.zip", "plugins/gpp-link/thumb.png",
            "Send beats over Wi-Fi between your MPC or Force and a folder on your computer (macOS or Windows hub included), with a cassette deck to preview them. Opens a local-network HTTP hub with no password: home or studio networks only.", "./plugins/gpp-link/install.sh", 0,
            0,
            false,
            false,
            "",
            { "utility", "files", "network", "player" }
        },
        {
            "grainbloom", "Grainbloom", "Lightlike", "v1.0.0", "Granular",
            "FX", "Stable", "MPC One (3.10.1)", "MPC OS 3.x", "2026-11-15",
            "916", "17.6 MB", "14%",
            "Lightlike - VST - Grainbloom", "VST/grainbloom-arm32.so",
            "https://raw.githubusercontent.com/lightlikemusic/grainbloom/v1.0.0/docs/screenshot.png", "https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/grainbloom-1.0.0-752444086-mpc-armv7.zip", "plugins/grainbloom/thumb.png",
            "Granular texture and micro-loop effect. Seven modes (Collage, Cloud, Stutter, Shimmer, Shatter, Arpeggio, Cascade) slice, scatter and pitch the recent past into rhythmic loops or ambient washes, with tempo sync, freeze, feedback and a lush plate/hall reverb. Grain, Scatter, Space and Mix macros on a touch skin.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Lightlike - VST - Grainbloom\" && ZIP_PATH=\"./plugins/grainbloom/grainbloom.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/grainbloom-1.0.0-752444086-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/grainbloom\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/grainbloom-1.0.0-752444086-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Lightlike - VST - Grainbloom/\"; elif [ -f \"./plugins/grainbloom/VST/grainbloom-arm32.so\" ]; then cp -rf ./plugins/grainbloom/* \"$TARGET_SYNTH_DIR/Lightlike - VST - Grainbloom/\"; else echo \"[ERROR] No local binary archive found for grainbloom\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "granular", "ambient", "looper", "stutter", "shimmer", "reverb", "tempo-sync", "freeze" }
        },
        {
            "op1fx-grid", "Grid", "Bombest Audio", "v0.6.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1434", "9.5 MB", "14%",
            "Bombest Audio - VST - Grid", "grid.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/grid.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Grid-0.6.0-mpc-armv7.zip", "plugins/op1fx-grid/thumb.png",
            "A 2D waveguide mesh you feed audio and read back. OP-1 inspired.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Grid\" && ZIP_PATH=\"./plugins/op1fx-grid/op1fx-grid.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Grid-0.6.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-grid\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Grid-0.6.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Grid/\"; elif [ -f \"./plugins/op1fx-grid/grid.so\" ]; then cp -rf ./plugins/op1fx-grid/* \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Grid/\"; else echo \"[ERROR] No local binary archive found for op1fx-grid\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "reverb", "waveguide", "resonator" }
        },
        {
            "incipience-out", "Incipience OUT", "saustin2010", "v1.1.1", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1744", "104 KB", "14%",
            "Steve A - VST - [FX] Incipience OUT", "incipienceout.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-incipience/v1.1.1/incipienceout/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-incipience/releases/download/incipience-out-v1.1.1/FX-Incipience-OUT-1.1.1-mpc-armv7.zip", "plugins/incipience-out/thumb.png",
            "Companion to Incipience: plays one of its OUT 1-11 on its own MPC track (an audio track will do), so an instrument gets that track's fader, inserts and outputs, in time with the kit. Needs Incipience 1.0.0 or later in the same project.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Steve A - VST - [FX] Incipience OUT\" && ZIP_PATH=\"./plugins/incipience-out/incipience-out.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-incipience/releases/download/incipience-out-v1.1.1/FX-Incipience-OUT-1.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/incipience-out\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-incipience/releases/download/incipience-out-v1.1.1/FX-Incipience-OUT-1.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Steve A - VST - [FX] Incipience OUT/\"; elif [ -f \"./plugins/incipience-out/incipienceout.so\" ]; then cp -rf ./plugins/incipience-out/* \"$TARGET_SYNTH_DIR/Steve A - VST - [FX] Incipience OUT/\"; else echo \"[ERROR] No local binary archive found for incipience-out\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "routing", "multi-out", "drum" }
        },
        {
            "keyscope", "Keyscope", "jacob-sabella", "v1.1.2", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-16",
            "6188", "3.2 MB", "4%",
            "jacob-sabella - VST - Keyscope", "keyscope.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-keyscope/88cb99ac5e0d682e1e6d9c81836bf4096a9cde3c/docs/screenshots/keyscope.png", "https://github.com/jacob-sabella/mpc-vst-keyscope/releases/download/keyscope-vst-v1.1.2/Keyscope-1.1.2-mpc-armv7.zip", "plugins/keyscope/thumb.png",
            "Listens to any track and names its key, chord, bass note and tuning, from the spectrum of the whole mix; the audio passes through untouched.", "./plugins/keyscope/install.sh", 0,
            0,
            false,
            false,
            "",
            { "key-detection", "chord-detection", "tuner", "analyzer" }
        },
        {
            "liminal-amp", "Liminal Amp", "gmorb", "v1.1.0", "Distortion",
            "FX", "Stable", "Akai Force (Gen1) (3.10.1), MPC Live 3 (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-13",
            "3112", "5.5 MB", "14%",
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
            "4880", "24.7 MB", "14%",
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
            "loop-saboteurxxl", "Loop-SaboteurXXL", "L'Cronx", "v0.1.3", "Glitch Performance",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-22",
            "2958", "8.2 MB", "14%",
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
            "759", "102 KB", "14%",
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
            "lufs-vu", "LUFS VU", "Slow Ops Music", "v1.0.1", "Utility",
            "FX", "Stable", "MPC Live III (Gen2) (3.10.1), MPC XL (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-16",
            "676", "6.3 MB", "14%",
            "Slow Ops - VST - LUFS VU", "VST/slow-ops-lufs-vu-arm32.so",
            "https://raw.githubusercontent.com/drj-io/SlowOps.MPC.LufsVu/v1.0.1/docs/images/mpc-lufs.png", "https://github.com/drj-io/SlowOps.MPC.LufsVu/releases/download/v1.0.1/LUFS-VU-1.0.1-mpc-armv7.zip", "plugins/lufs-vu/thumb.png",
            "Stereo analog VU meters plus an EBU R 128 loudness tab: integrated, short-term and momentary LUFS, true peak and gain reduction. Optional AUTO loudness steers towards a target (-23 to -9 LUFS) and an optional true-peak limiter holds a ceiling. Selectable calibration, clip lamps, phase meter. With processing off the audio passes through bit-exact.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Slow Ops - VST - LUFS VU\" && ZIP_PATH=\"./plugins/lufs-vu/lufs-vu.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/drj-io/SlowOps.MPC.LufsVu/releases/download/v1.0.1/LUFS-VU-1.0.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/lufs-vu\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/drj-io/SlowOps.MPC.LufsVu/releases/download/v1.0.1/LUFS-VU-1.0.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Slow Ops - VST - LUFS VU/\"; elif [ -f \"./plugins/lufs-vu/VST/slow-ops-lufs-vu-arm32.so\" ]; then cp -rf ./plugins/lufs-vu/* \"$TARGET_SYNTH_DIR/Slow Ops - VST - LUFS VU/\"; else echo \"[ERROR] No local binary archive found for lufs-vu\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "metering", "loudness", "lufs", "vu-meter", "limiter" }
        },
        {
            "machinemodule-tap-gen2", "Machinemodule Tap (Gen2)", "sd88me", "v1.0.5", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-11",
            "2711", "95 KB", "14%",
            "sd88me - VST - Machinemodule Tap", "machinedrum_tapfx.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-machinedrum/main/docs/img/machinemodule-tap-gen2.png", "https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.5/Machinemodule-Tap-1.0.5-mpc-aarch64.zip", "plugins/machinemodule-tap-gen2/thumb.png",
            "Puts any Machinemodule tracks, and its reverb and delay sends, on their own MPC return or FX track, so MPC's mixer and effects can process them. Needs Machinemodule in the same project. Gen2 (aarch64) only.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - Machinemodule Tap\" && ZIP_PATH=\"./plugins/machinemodule-tap-gen2/machinemodule-tap-gen2.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.5/Machinemodule-Tap-1.0.5-mpc-aarch64.zip\" ]; then mkdir -p \"./plugins/machinemodule-tap-gen2\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-machinedrum/releases/download/v1.0.5/Machinemodule-Tap-1.0.5-mpc-aarch64.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - Machinemodule Tap/\"; elif [ -f \"./plugins/machinemodule-tap-gen2/machinedrum_tapfx.so\" ]; then cp -rf ./plugins/machinemodule-tap-gen2/* \"$TARGET_SYNTH_DIR/sd88me - VST - Machinemodule Tap/\"; else echo \"[ERROR] No local binary archive found for machinemodule-tap-gen2\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "elektron", "machinedrum", "gen2" }
        },
        {
            "magic-tape-echo", "Magic Tape Echo", "MagicStino", "v0.1.2", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1904", "2.6 MB", "14%",
            "MagicStino - VST - Magic Tape Echo", "magic_tape_echo.so",
            "https://raw.githubusercontent.com/MagicStino/Magic-Tape-Echo/a616cc2fad6bd7eb920111627bb1fc81631ff09a/magic-tape-echo-three-heads.png", "https://github.com/MagicStino/Magic-Tape-Echo/releases/download/v0.1.2/Magic-Tape-Echo-0.1.2-mpc-armv7.zip", "plugins/magic-tape-echo/thumb.png",
            "Three-head stereo tape echo with tape saturation, wow/flutter, tempo sync, optional spring reverb and a slow VU meter.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/MagicStino - VST - Magic Tape Echo\" && ZIP_PATH=\"./plugins/magic-tape-echo/magic-tape-echo.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/MagicStino/Magic-Tape-Echo/releases/download/v0.1.2/Magic-Tape-Echo-0.1.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/magic-tape-echo\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/MagicStino/Magic-Tape-Echo/releases/download/v0.1.2/Magic-Tape-Echo-0.1.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/MagicStino - VST - Magic Tape Echo/\"; elif [ -f \"./plugins/magic-tape-echo/magic_tape_echo.so\" ]; then cp -rf ./plugins/magic-tape-echo/* \"$TARGET_SYNTH_DIR/MagicStino - VST - Magic Tape Echo/\"; else echo \"[ERROR] No local binary archive found for magic-tape-echo\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "delay", "tape", "echo", "spring" }
        },
        {
            "mantra", "MANTRA", "L'Cronx", "v0.2.0", "Glitch Performance",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-14",
            "644", "16.2 MB", "14%",
            "L'Cronx - VST - MANTRA", "mantra.so",
            "https://raw.githubusercontent.com/sunskiefer/MANTRA/v0.2.0/docs/screenshots/page_0.png", "https://github.com/sunskiefer/MANTRA/releases/download/v0.2.0/MANTRA-0.2.0-mpc-armv7.zip", "plugins/mantra/thumb.png",
            "A rhythmic multi-effect: a pattern chops, filters, crushes, delays and glitches the sound in time with the song. Two layers of six effects (filter, distortion, delay, lo-fi, flanger, glitch) in any order, eight 32-step modulators locked to the song, a 24-slot matrix, per-page randomiser, 419 factory presets plus unlimited own. MPC OS 3.x.", "./plugins/mantra/install.sh", 0,
            0,
            false,
            false,
            "",
            { "multi-fx", "gate", "filter", "delay", "glitch", "lo-fi", "sequencer", "sound-design" }
        },
        {
            "warps", "Meta Mod", "saustin2010", "v1.2.1", "Modulation",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3605", "1.0 MB", "14%",
            "Émilie Gillet - VST - [FX] Meta Mod", "warps.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-meta-mod/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-meta-mod/releases/download/v1.2.1/FX-Meta-Mod-1.2.1-mpc-armv7.zip", "plugins/warps/thumb.png",
            "Meta Mod: ring mod, folding, XOR, vocoder and frequency shifting as an insert effect; 12 presets, from Émilie Gillet's open-source Warps.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Meta Mod\" && ZIP_PATH=\"./plugins/warps/warps.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/saustin2010/mpc-vst-meta-mod/releases/download/v1.2.1/FX-Meta-Mod-1.2.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/warps\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/saustin2010/mpc-vst-meta-mod/releases/download/v1.2.1/FX-Meta-Mod-1.2.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Meta Mod/\"; elif [ -f \"./plugins/warps/warps.so\" ]; then cp -rf ./plugins/warps/* \"$TARGET_SYNTH_DIR/Émilie Gillet - VST - [FX] Meta Mod/\"; else echo \"[ERROR] No local binary archive found for warps\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "ring-mod", "vocoder", "frequency-shifter", "eurorack" }
        },
        {
            "meta4mpcxxl-fx", "Meta4MPCXXL FX", "L'Cronx", "v0.1.2", "Multi Fx",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "1188", "11.2 MB", "14%",
            "L'Cronx - VST - Meta4MPCXXL FX", "meta4mpcxxl_fx.so",
            "https://raw.githubusercontent.com/sunskiefer/Meta4MPCXXL/v0.1.1/docs/img/meta4mpcxxl.png", "https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-FX-0.1.2-mpc-armv7.zip", "plugins/meta4mpcxxl-fx/thumb.png",
            "Meta4MPCXXL as an effect: the track's audio goes into a 4ms MetaModule-format (VCV Rack) patch's In 1 / In 2, with every parameter on Q-Links, MIDI and clock into the patch, and Wi-Fi patch upload from VCV Rack.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - Meta4MPCXXL FX\" && ZIP_PATH=\"./plugins/meta4mpcxxl-fx/meta4mpcxxl-fx.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-FX-0.1.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/meta4mpcxxl-fx\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/Meta4MPCXXL/releases/download/v0.1.2/Meta4MPCXXL-FX-0.1.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - Meta4MPCXXL FX/\"; elif [ -f \"./plugins/meta4mpcxxl-fx/meta4mpcxxl_fx.so\" ]; then cp -rf ./plugins/meta4mpcxxl-fx/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - Meta4MPCXXL FX/\"; else echo \"[ERROR] No local binary archive found for meta4mpcxxl-fx\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "modular", "vcv", "metamodule", "patch-player", "eurorack", "multi-fx" }
        },
        {
            "monomodule-fx-gen2", "Monomodule FX (Gen2)", "sd88me", "v1.0.4", "Multi Fx",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-04",
            "2216", "4.2 MB", "14%",
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
            "1133", "8.3 MB", "14%",
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
            "mpc-dj-fx-2", "MPC DJ FX 2", "mitchoster", "v3.2.0", "Glitch Performance",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-09",
            "5781", "12.6 MB", "14%",
            "LarryDaimond - VST - MPC DJ FX 2", "mpc_djfx2.so",
            "https://raw.githubusercontent.com/mitchoster/mpc-vst-djfx2/0f20d92f/docs/ui-preview/page_1.png", "https://github.com/mitchoster/mpc-vst-djfx2/releases/download/v3.2.0/MPC-DJ-FX-2-3.2.0-mpc-armv7.zip", "plugins/mpc-dj-fx-2/thumb.png",
            "One-slot live DJ insert for MPC/Force: 3-band isolator, DJ filter, beat repeat, delay, reverb, through-zero flanger, grain delay, limiter, and eight performance macros measured from Lenny Kiser's DJ-FX rack.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/LarryDaimond - VST - MPC DJ FX 2\" && ZIP_PATH=\"./plugins/mpc-dj-fx-2/mpc-dj-fx-2.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mitchoster/mpc-vst-djfx2/releases/download/v3.2.0/MPC-DJ-FX-2-3.2.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/mpc-dj-fx-2\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mitchoster/mpc-vst-djfx2/releases/download/v3.2.0/MPC-DJ-FX-2-3.2.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/LarryDaimond - VST - MPC DJ FX 2/\"; elif [ -f \"./plugins/mpc-dj-fx-2/mpc_djfx2.so\" ]; then cp -rf ./plugins/mpc-dj-fx-2/* \"$TARGET_SYNTH_DIR/LarryDaimond - VST - MPC DJ FX 2/\"; else echo \"[ERROR] No local binary archive found for mpc-dj-fx-2\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "dj", "isolator", "filter", "beat-repeat", "delay", "reverb", "performance", "macro" }
        },
        {
            "mpc-looper", "MPC Looper", "sttm", "v0.6.1", "Glitch Performance",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-21",
            "205", "8.5 MB", "14%",
            "sttm - VST - MPC Looper", "VST/mpc-looper-arm32.so",
            "https://raw.githubusercontent.com/sttm/mpc-looper/v0.6.1/docs/ui-preview.png", "https://github.com/sttm/mpc-looper/releases/download/v0.6.1/MPC-Looper-0.6.1-mpc-armv7.zip", "plugins/mpc-looper/thumb.png",
            "Experimental four-loop stereo insert for MPC OS 3.10.1+. Disk-backed recording up to 32 bars, overdub, undo, tempo sync and Warp, per-loop stop timing, and separate WAV exports. Fixed audio caches; final revision and Gen2 are offline-only.", "./plugins/mpc-looper/install.sh", 0,
            0,
            false,
            false,
            "",
            { "looper", "overdub", "tempo-sync", "performance", "warp" }
        },
        {
            "mpc-multihead-echo", "MPC Multihead Echo", "mitchoster", "v1.0.3", "Delay",
            "FX", "Stable", "MPC Live II (MPC OS 3.x (AZ0x))", "MPC OS 3.x", "2026-11-11",
            "1806", "1.7 MB", "25%",
            "LarryDaimond - VST - MPC Multihead Echo", "mpc_multihead_echo.so",
            "https://raw.githubusercontent.com/mitchoster/mpc-vst-multihead-echo/c31c3ef479ce87083ab87fbef14c2fe92a6bb83b/docs/companion-panel-echo.png", "https://github.com/mitchoster/mpc-vst-multihead-echo/releases/download/v1.0.3/MPC-Multihead-Echo-1.0.3-mpc-armv7.zip", "plugins/mpc-multihead-echo/thumb.png",
            "Three-head tape echo with three preamp characters, chorus, tape wear, tempo sync with drift, a spring reverb and a companion screen in the browser.", "./plugins/mpc-multihead-echo/install.sh", 0,
            0,
            false,
            false,
            "",
            { "tape-echo", "delay", "saturation", "spring-reverb", "tempo-sync", "companion-app" }
        },
        {
            "noxed-mpc-overwitch-send", "MPC Overwitch Send", "NOXED", "v0.6.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "282", "362 KB", "14%",
            "NOXED - VST - MPC Overwitch Send", "VST/mpc-overwitch-send-arm32.so",
            "https://raw.githubusercontent.com/molgino/noxend-mpc-plugins/40832b26596b1c0228eb97e0dc81c257547a2d1a/docs/screenshots/mpc-overwitch-send.png", "https://github.com/molgino/noxend-mpc-plugins/releases/download/v0.3.0/noxed-mpc-overwitch-send-0.6.0-mpc-armv7.zip", "plugins/noxed-mpc-overwitch-send/thumb.png",
            "Sends MPC track audio to the inputs of an Elektron Overbridge device over the shared Overwitch USB connection, with send and dry levels. Requires an Overbridge-capable device.", "./plugins/noxed-mpc-overwitch-send/install.sh", 0,
            0,
            false,
            false,
            "",
            { "utility", "usb" }
        },
        {
            "mpc-radio", "MPC Radio", "Steph", "v1.0.0", "Lofi Tape",
            "FX", "Stable", "MPC Key 37 (3.10.1.12)", "MPC OS 3.x", "2026-11-18",
            "470", "2.2 MB", "14%",
            "Steph - VST - MPC Radio", "VST/mpc-radio-arm32.so",
            "https://raw.githubusercontent.com/Forge-Audio/mpc-radio/v1.0.0/docs/screen-1.png", "https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/mpc-radio-1.0.0-765134829-mpc-armv7.zip", "plugins/mpc-radio/thumb.png",
            "MPC Radio is a radio dial that plays live internet stations, with hiss, crackle, sliding whistles and half-heard ghosts between them and a perfectly clean signal once a station locks in.", "./plugins/mpc-radio/install.sh", 0,
            0,
            false,
            false,
            "",
            {  }
        },
        {
            "mpc-tape-echo", "MPC Tape Echo", "mitchoster", "v1.3.0", "Delay",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-09",
            "3276", "9.7 MB", "14%",
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
            "mpc-template-chooser", "MPC Template Chooser", "Elsewave", "v1.0.1", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-21",
            "95", "459 KB", "14%",
            "Elsewave - VST - MPC Template Chooser", "VST/mpc-template-chooser-arm32.so",
            "https://raw.githubusercontent.com/shhQuiet/MPCTemplateChooser/v1.0.0/docs/screenshots/chooser-help.png", "https://github.com/shhQuiet/MPCTemplateChooser/releases/download/v1.0.1/MPC-Template-Chooser-1.0.1-mpc-armv7.zip", "plugins/mpc-template-chooser/thumb.png",
            "Replaces Demos on the New Project screen with templates from the /Templates folder on each drive. Installs a startup hook that rewrites the system file /sdcard/Demos/NewProjectDialog.json. Requires the Open MPC boot-reload service and MPC OS 3.10.1 or later. Tested on an original MPC X.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Elsewave - VST - MPC Template Chooser\" && ZIP_PATH=\"./plugins/mpc-template-chooser/mpc-template-chooser.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/shhQuiet/MPCTemplateChooser/releases/download/v1.0.1/MPC-Template-Chooser-1.0.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/mpc-template-chooser\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/shhQuiet/MPCTemplateChooser/releases/download/v1.0.1/MPC-Template-Chooser-1.0.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Elsewave - VST - MPC Template Chooser/\"; elif [ -f \"./plugins/mpc-template-chooser/VST/mpc-template-chooser-arm32.so\" ]; then cp -rf ./plugins/mpc-template-chooser/* \"$TARGET_SYNTH_DIR/Elsewave - VST - MPC Template Chooser/\"; else echo \"[ERROR] No local binary archive found for mpc-template-chooser\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "templates", "projects" }
        },
        {
            "nam", "NAM", "jacob-sabella", "v1.1.2", "Distortion",
            "FX", "Stable", "MPC Key 37 (3.10.1.12)", "MPC OS 2.x & 3.x", "2026-11-16",
            "8241", "2.8 MB", "14%",
            "jacob-sabella - VST - NAM", "nam_vst.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-nam/f2fa28c8f4086606e929732a4824e72579bdcf80/docs/screenshots/nam.png", "https://github.com/jacob-sabella/mpc-vst-nam/releases/download/nam-vst-v1.1.2/NAM-1.1.2-mpc-armv7.zip", "plugins/nam/thumb.png",
            "Neural Amp Modeler captures as an insert effect, with cab IR, tone, gate, pitch, delay, reverb and an on-device TONE3000 downloader.", "./plugins/nam/install.sh", 0,
            0,
            false,
            false,
            "",
            { "nam", "guitar", "amp", "cab-ir", "tone3000" }
        },
        {
            "nepenthe", "Nepenthe", "ikloX", "v1.0.2", "Reverb",
            "FX", "Stable", "MPC Live (3.10.1)", "MPC OS 3.x", "2026-11-23",
            "41", "384 KB", "17%",
            "ikloX - VST - Nepenthe", "nepenthe.so",
            "https://raw.githubusercontent.com/biloo77-cell/mpc-vst-nepenthe/67bccc8467bc12bdc57bfab1b66bf5a6563de8ae/docs/img/nepenthe.png", "https://github.com/biloo77-cell/mpc-vst-nepenthe/releases/download/nepenthe-v1.0.2/Nepenthe-1.0.2-mpc-armv7.zip", "plugins/nepenthe/thumb.png",
            "Amalgamated Signals' smooth velvet-noise reverb, bit-identical to the original, with time, pre-delay, bass, treble, width and mix on upstream's sliders.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/ikloX - VST - Nepenthe\" && ZIP_PATH=\"./plugins/nepenthe/nepenthe.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/biloo77-cell/mpc-vst-nepenthe/releases/download/nepenthe-v1.0.2/Nepenthe-1.0.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/nepenthe\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/biloo77-cell/mpc-vst-nepenthe/releases/download/nepenthe-v1.0.2/Nepenthe-1.0.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/ikloX - VST - Nepenthe/\"; elif [ -f \"./plugins/nepenthe/nepenthe.so\" ]; then cp -rf ./plugins/nepenthe/* \"$TARGET_SYNTH_DIR/ikloX - VST - Nepenthe/\"; else echo \"[ERROR] No local binary archive found for nepenthe\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "reverb", "velvet-noise", "stereo", "algorithmic" }
        },
        {
            "op1fx-nitro", "Nitro", "Bombest Audio", "v0.6.0", "Filter Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1433", "4.9 MB", "14%",
            "Bombest Audio - VST - Nitro", "nitro.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/nitro.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Nitro-0.6.0-mpc-armv7.zip", "plugins/op1fx-nitro/thumb.png",
            "Filter with a drive stage and a MIX knob, 12 presets. OP-1 inspired.", "./plugins/op1fx-nitro/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "filter", "drive", "dj" }
        },
        {
            "ostirusmpc-audio-send", "OsTirusMPC Audio Send", "Pryonix", "v1.0.1", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-15",
            "194", "83 KB", "14%",
            "Pryonix - VST - OsTirusMPC Audio Send", "ostirus_audio_send.so",
            "", "https://github.com/dnkkit/OsTirusMPC/releases/download/v1.0.0/OsTirusMPC-Audio-Send-1.0.1-mpc-aarch64.zip", "plugins/ostirusmpc-audio-send/thumb.png",
            "Optional companion effect for OsTirusMPC on Gen2 MPC standalone. Routes audio from any track or individual pad to the OsTirusMPC virtual audio input.", "./plugins/ostirusmpc-audio-send/install.sh", 0,
            0,
            false,
            false,
            "",
            { "utility", "audio-routing" }
        },
        {
            "ottmpc", "OTTmpc", "sd88me", "v2.0.0", "Dynamics",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-18",
            "3239", "4.4 MB", "14%",
            "sd88me - VST - OTTmpc", "ottmpc.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-OTT/claude/friendly-wozniak-m9ak1l/docs/screenshot.png", "https://github.com/sd88me/mpc-vst-OTT/releases/download/ottmpc-vst-v2.0.0/OTTmpc-2.0.0-mpc-armv7.zip", "plugins/ottmpc/thumb.png",
            "Vital's OTT multiband up/down compressor as an insert effect: live band meters with dB numbers, 12 Q-Link controls and 10 presets.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/sd88me - VST - OTTmpc\" && ZIP_PATH=\"./plugins/ottmpc/ottmpc.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sd88me/mpc-vst-OTT/releases/download/ottmpc-vst-v2.0.0/OTTmpc-2.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/ottmpc\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sd88me/mpc-vst-OTT/releases/download/ottmpc-vst-v2.0.0/OTTmpc-2.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/sd88me - VST - OTTmpc/\"; elif [ -f \"./plugins/ottmpc/ottmpc.so\" ]; then cp -rf ./plugins/ottmpc/* \"$TARGET_SYNTH_DIR/sd88me - VST - OTTmpc/\"; else echo \"[ERROR] No local binary archive found for ottmpc\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "ott", "multiband", "compressor", "dynamics" }
        },
        {
            "overcast", "Overcast", "Padbangers", "v1.1.0", "Granular",
            "FX", "Stable", "Akai MPC X (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-14",
            "7946", "3.9 MB", "14%",
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
            "2036", "147 KB", "14%",
            "Steve A - VST - [FX] Percolator OUT", "percolatorout.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-percolator/v2.1.0/percolatorout/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-percolator/releases/download/percolator-out-v2.1.0/FX-Percolator-OUT-2.1.0-mpc-armv7.zip", "plugins/percolator-out/thumb.png",
            "Companion to Percolator: plays one of its OUT 1-4 on its own MPC track (an audio track will do), so a voice gets that track's fader, inserts and outputs, in time with the kit. Needs Percolator 2.0.0 or later in the same project.", "./plugins/percolator-out/install.sh", 0,
            0,
            false,
            false,
            "",
            { "routing", "multi-out", "drum" }
        },
        {
            "op1fx-phone", "Phone", "Bombest Audio", "v0.6.0", "Filter Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1361", "5.1 MB", "14%",
            "Bombest Audio - VST - Phone", "phone.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/phone.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Phone-0.6.0-mpc-armv7.zip", "plugins/op1fx-phone/thumb.png",
            "Telephone-band squash with a 12-preset bank. OP-1 inspired.", "./plugins/op1fx-phone/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "telephone", "band-limit", "lo-fi" }
        },
        {
            "phyxxl", "PhyXXL", "L'Cronx", "v0.1.6", "Filter Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-25",
            "2960", "18.3 MB", "14%",
            "L'Cronx - VST - PhyXXL", "phyxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/PhyXXL/v0.1.2/docs/img/overview.png", "https://github.com/sunskiefer/PhyXXL/releases/download/v0.1.6/PhyXXL-0.1.6-mpc-armv7.zip", "plugins/phyxxl/thumb.png",
            "Eight filters in the style of classic synths (MS-20, SEM, Minimoog, Matrix-12, 2600, Wasp, prologue, Polivoks) in one effect. Route audio from any filter into any other; modulate each from ten sources by touch patch bay or matrix. 28 patches. By L'Cronx, GPL-3. No third-party code; not endorsed by the makers of the originals.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - PhyXXL\" && ZIP_PATH=\"./plugins/phyxxl/phyxxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/PhyXXL/releases/download/v0.1.6/PhyXXL-0.1.6-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/phyxxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/PhyXXL/releases/download/v0.1.6/PhyXXL-0.1.6-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - PhyXXL/\"; elif [ -f \"./plugins/phyxxl/phyxxl.so\" ]; then cp -rf ./plugins/phyxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - PhyXXL/\"; else echo \"[ERROR] No local binary archive found for phyxxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "filter", "multi-filter", "ms-20", "sem", "moog", "matrix-12", "arp-2600", "polivoks" }
        },
        {
            "plateauxxl", "PlateauXXL", "L'Cronx", "v0.5.0", "Reverb",
            "FX", "Beta", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-22",
            "3327", "12.9 MB", "14%",
            "L'Cronx - VST - PlateauXXL", "plateauxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/PlateauXXL/v0.5.0/docs/img/plateau.png", "https://github.com/sunskiefer/PlateauXXL/releases/download/v0.5.0/PlateauXXL-0.5.0-mpc-armv7.zip", "plugins/plateauxxl/thumb.png",
            "Valley Audio's Plateau reverb from VCV Rack, patched: every CV input on a built-in source (four Bogaudio LFOs, Tidal Modulator 2, Random Sampler, step and gate sequencers) synced to the MPC tempo, a brickwall limiter, Panic and 16 presets, each page drawn like its module. 32-bit and 64-bit MPCs.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - PlateauXXL\" && ZIP_PATH=\"./plugins/plateauxxl/plateauxxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/PlateauXXL/releases/download/v0.5.0/PlateauXXL-0.5.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/plateauxxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/PlateauXXL/releases/download/v0.5.0/PlateauXXL-0.5.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - PlateauXXL/\"; elif [ -f \"./plugins/plateauxxl/plateauxxl.so\" ]; then cp -rf ./plugins/plateauxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - PlateauXXL/\"; else echo \"[ERROR] No local binary archive found for plateauxxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "reverb", "plate", "dattorro", "valley", "vcv", "freeze", "lfo", "modulation" }
        },
        {
            "potentialbleed", "PotentialBleed", "PotentialBreaks", "v1.3.0", "Lofi Tape",
            "FX", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-19",
            "1377", "14.2 MB", "3%",
            "PotentialBreaks - VST - PotentialBleed", "potentialbleed.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialbleed/4d582269764864993c333429c3c3cd57ecd7c8b6/docs/images/potentialbleed.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialbleed/releases/download/v1.3.0/PotentialBleed-1.3.0-mpc-armv7.zip", "plugins/potentialbleed/thumb.png",
            "Ghost bleed: hears each hit and adds quiet synthetic residue behind it (snare wires, cabinet, piano board, tines and more), so chops sound like a real recording.", "./plugins/potentialbleed/install.sh", 0,
            0,
            false,
            false,
            "",
            { "lo-fi", "texture", "ghost-notes" }
        },
        {
            "potentialgrit", "PotentialGrit", "PotentialBreaks", "v1.3.0", "Lofi Tape",
            "FX", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-19",
            "1625", "21.0 MB", "3%",
            "PotentialBreaks - VST - PotentialGrit", "potentialgrit.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialgrit/96b1b4e4f9ef1fddeafa1063be8346740da94518/docs/images/potentialgrit.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialgrit/releases/download/v1.3.0/PotentialGrit-1.3.0-mpc-armv7.zip", "plugins/potentialgrit/thumb.png",
            "Seven lo-fi effects on pads, one at a time: dust (glue, hiss, wobble), crush, resonant filter, three-band kill EQ, tempo echo, chop gate and ring mod, each with its own settings.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialGrit\" && ZIP_PATH=\"./plugins/potentialgrit/potentialgrit.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/PotentialBreakfast/mpc-vst-potentialgrit/releases/download/v1.3.0/PotentialGrit-1.3.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/potentialgrit\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/PotentialBreakfast/mpc-vst-potentialgrit/releases/download/v1.3.0/PotentialGrit-1.3.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialGrit/\"; elif [ -f \"./plugins/potentialgrit/potentialgrit.so\" ]; then cp -rf ./plugins/potentialgrit/* \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialGrit/\"; else echo \"[ERROR] No local binary archive found for potentialgrit\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "lo-fi", "bitcrusher", "filter", "delay", "ring-mod" }
        },
        {
            "potentialheat", "PotentialHeat", "PotentialBreaks", "v1.6.0", "Distortion",
            "FX", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-18",
            "3253", "10.2 MB", "5%",
            "PotentialBreaks - VST - PotentialHeat", "potentialheat.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialheat/6ade776dda00fcd70053168b6b7ede87cd7fa091/docs/images/potentialheat.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialheat/releases/download/v1.6.0/PotentialHeat-1.6.0-mpc-armv7.zip", "plugins/potentialheat/thumb.png",
            "Soft clipper and bus saturator: push drums, breaks or a whole beat into a warm curve (soft, tape or hard) that rounds the peaks off instead of clipping, with tone, ceiling, level match, mix, a live meter and 12 one-tap presets. Pairs with PotentialPunch.", "./plugins/potentialheat/install.sh", 0,
            0,
            false,
            false,
            "",
            { "saturation", "soft-clipper", "bus", "drums", "loudness", "warmth" }
        },
        {
            "potentialpatina", "PotentialPatina", "PotentialBreaks", "v1.7.0", "Distortion",
            "FX", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-18",
            "3415", "20.1 MB", "7%",
            "PotentialBreaks - VST - PotentialPatina", "potentialpatina.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialpatina/b6e1a98e6b836a51d21ea62e0791ba695c237084/docs/images/potentialpatina.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialpatina/releases/download/v1.7.0/PotentialPatina-1.7.0-mpc-armv7.zip", "plugins/potentialpatina/thumb.png",
            "Retro-colour insert effect: vinyl and tape noise, wow and flutter, tape/tube/cassette drive, bit and sample-rate crush, a tone tilt and a small room, with presets from subtle dust to old radio.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialPatina\" && ZIP_PATH=\"./plugins/potentialpatina/potentialpatina.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/PotentialBreakfast/mpc-vst-potentialpatina/releases/download/v1.7.0/PotentialPatina-1.7.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/potentialpatina\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/PotentialBreakfast/mpc-vst-potentialpatina/releases/download/v1.7.0/PotentialPatina-1.7.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialPatina/\"; elif [ -f \"./plugins/potentialpatina/potentialpatina.so\" ]; then cp -rf ./plugins/potentialpatina/* \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialPatina/\"; else echo \"[ERROR] No local binary archive found for potentialpatina\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "lo-fi", "vinyl", "tape", "bitcrusher", "wobble" }
        },
        {
            "potentialpunch", "PotentialPunch", "PotentialBreaks", "v1.7.0", "Dynamics",
            "FX", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-19",
            "3527", "9.3 MB", "3%",
            "PotentialBreaks - VST - PotentialPunch", "potentialpunch.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialpunch/2640801fdb40fde4984275819330847fe4761870/docs/images/potentialpunch.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialpunch/releases/download/v1.7.0/PotentialPunch-1.7.0-mpc-armv7.zip", "plugins/potentialpunch/thumb.png",
            "Transient shaper: more or less punch at the start of every hit and more or less tail after it, level-independent with no threshold, speed and focus (full/lows/highs), soft clip, live meter and 12 one-tap presets for drums, breaks and more.", "./plugins/potentialpunch/install.sh", 0,
            0,
            false,
            false,
            "",
            { "transient-shaper", "drums", "punch", "breaks", "dynamics" }
        },
        {
            "potentialwarp", "PotentialWarp", "PotentialBreaks", "v1.5.0", "Glitch Performance",
            "FX", "Stable", "MPC One (MPC OS 3.10.1)", "MPC OS 3.x", "2026-11-19",
            "1826", "9.7 MB", "4%",
            "PotentialBreaks - VST - PotentialWarp", "potentialwarp.so",
            "https://raw.githubusercontent.com/PotentialBreakfast/mpc-vst-potentialwarp/a37d7dee2c87533557f4e898adf8c21b242d127a/docs/images/potentialwarp.png", "https://github.com/PotentialBreakfast/mpc-vst-potentialwarp/releases/download/v1.5.0/PotentialWarp-1.5.0-mpc-armv7.zip", "plugins/potentialwarp/thumb.png",
            "Tempo-locked mangler: 16 patterns of half time, tape stop, stutter, reverse and gates, with swing, triplets, 4-bar patterns and a hold-a-pad mode.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialWarp\" && ZIP_PATH=\"./plugins/potentialwarp/potentialwarp.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/PotentialBreakfast/mpc-vst-potentialwarp/releases/download/v1.5.0/PotentialWarp-1.5.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/potentialwarp\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/PotentialBreakfast/mpc-vst-potentialwarp/releases/download/v1.5.0/PotentialWarp-1.5.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialWarp/\"; elif [ -f \"./plugins/potentialwarp/potentialwarp.so\" ]; then cp -rf ./plugins/potentialwarp/* \"$TARGET_SYNTH_DIR/PotentialBreaks - VST - PotentialWarp/\"; else echo \"[ERROR] No local binary archive found for potentialwarp\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "stutter", "tape-stop", "gate", "half-time", "tempo-sync" }
        },
        {
            "pulytek", "PulyTek", "MagicStino", "v1.0.2", "Filter Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-10-23",
            "5263", "14.9 MB", "14%",
            "MagicStino - VST - PulyTek", "pulytek.so",
            "https://raw.githubusercontent.com/MagicStino/pulytek/v1.0.1/docs/preview_0.png", "https://github.com/MagicStino/pulytek/releases/download/v1.0.2/PulyTek-1.0.2-mpc-armv7.zip", "plugins/pulytek/thumb.png",
            "Pultec-style WDF EQ with tube warmth, AIR, stereo VU meters and four-band side EQ.", "./plugins/pulytek/install.sh", 0,
            0,
            false,
            false,
            "",
            { "eq", "pultec", "wdf", "tube", "stereo" }
        },
        {
            "op1fx-punch", "Punch", "Bombest Audio", "v0.6.0", "Distortion",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1663", "6.0 MB", "14%",
            "Bombest Audio - VST - Punch", "punch.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/punch.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Punch-0.6.0-mpc-armv7.zip", "plugins/op1fx-punch/thumb.png",
            "Bit-crush style punch for drums and buses, 12 presets, with a Detroit boxer skin.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Punch\" && ZIP_PATH=\"./plugins/op1fx-punch/op1fx-punch.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Punch-0.6.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/op1fx-punch\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Punch-0.6.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Punch/\"; elif [ -f \"./plugins/op1fx-punch/punch.so\" ]; then cp -rf ./plugins/op1fx-punch/* \"$TARGET_SYNTH_DIR/Bombest Audio - VST - Punch/\"; else echo \"[ERROR] No local binary archive found for op1fx-punch\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "op1", "compressor", "drive", "drums" }
        },
        {
            "remixxxl-cue", "reMixXXL CUE", "L'Cronx", "v0.2.0", "Utility",
            "FX", "Beta", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-17",
            "694", "973 KB", "14%",
            "L'Cronx - VST - reMixXXL CUE", "remixxxl_cue.so",
            "", "https://github.com/sunskiefer/reMixXXL/releases/download/v0.2.0/reMixXXL-CUE-0.2.0-mpc-armv7.zip", "plugins/remixxxl-cue/thumb.png",
            "Companion to reMixXXL: plays reMixXXL's PFL cue (decks 1/3 left, 2/4 right) on its own audio track routed to outputs 3/4, so you can beat match in the headphones while the main mix stays on 1/2. Needs reMixXXL 0.1.25 or later in the same project.", "./plugins/remixxxl-cue/install.sh", 0,
            0,
            false,
            false,
            "",
            { "dj", "routing", "cue", "headphones" }
        },
        {
            "rings-fx", "Resonator FX", "saustin2010", "v1.2.1", "Granular",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "4045", "1.2 MB", "14%",
            "Émilie Gillet - VST - [FX] Resonator FX", "ringsfx.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-resonator-fx/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-resonator-fx/releases/download/v1.2.1/FX-Resonator-FX-1.2.1-mpc-armv7.zip", "plugins/rings-fx/thumb.png",
            "Resonator FX: Resonator as an insert effect, the track's audio strums it, turning drums and loops into tuned strings; 12 presets.", "./plugins/rings-fx/install.sh", 0,
            0,
            false,
            false,
            "",
            { "rings", "resonator", "eurorack" }
        },
        {
            "retrolooper-force", "Retrolooper One (Force)", "Retroloop USA / IAMTHEBAND LLC", "v0.2.10", "Glitch Performance",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-07",
            "643", "2.8 MB", "14%",
            "RetroLooper - VST - Retrolooper One", "retrolooper_force.so",
            "https://raw.githubusercontent.com/asearan/Retrolooper-One-MPC-Force/force-v0.2.10/docs/images/retrolooper-force-thumbnail.jpg", "https://github.com/asearan/Retrolooper-One-MPC-Force/releases/download/force-v0.2.10/RetroLooper-One-Force-0.2.10-mpc-armv7.zip", "plugins/retrolooper-force/thumb.png",
            "Stereo Audio looper for Akai Force. On MPC OS 3.10.1, copy the Gen1 build into VST2 Plugins. 0.3.0 keeps plugin id RlF1. Native Controls remain pinned to Force 3.10.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/RetroLooper - VST - Retrolooper One\" && ZIP_PATH=\"./plugins/retrolooper-force/retrolooper-force.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/asearan/Retrolooper-One-MPC-Force/releases/download/force-v0.2.10/RetroLooper-One-Force-0.2.10-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/retrolooper-force\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/asearan/Retrolooper-One-MPC-Force/releases/download/force-v0.2.10/RetroLooper-One-Force-0.2.10-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/RetroLooper - VST - Retrolooper One/\"; elif [ -f \"./plugins/retrolooper-force/retrolooper_force.so\" ]; then cp -rf ./plugins/retrolooper-force/* \"$TARGET_SYNTH_DIR/RetroLooper - VST - Retrolooper One/\"; else echo \"[ERROR] No local binary archive found for retrolooper-force\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "looper", "overdub", "utility" }
        },
        {
            "retrolooper-one", "Retrolooper One (MPC)", "Retroloop USA / IAMTHEBAND LLC", "v0.3.9", "Glitch Performance",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-07",
            "1281", "2.7 MB", "14%",
            "RetroLooper - VST - Retrolooper One", "retrolooper_one.so",
            "https://raw.githubusercontent.com/asearan/Retrolooper-One-MPC-Force/mpc-v0.3.9/docs/images/retrolooper-one-thumbnail.jpg", "https://github.com/asearan/Retrolooper-One-MPC-Force/releases/download/mpc-v0.3.9/RetroLooper-One-0.3.9-mpc-armv7.zip", "plugins/retrolooper-one/thumb.png",
            "Stereo Audio looper for Gen1 and Gen2 MPCs. On MPC OS 3.10.1, copy it into VST2 Plugins. 0.4.0 keeps plugin id RlO1 so saved projects recall it. The separate official SDK build is IH97.", "./plugins/retrolooper-one/install.sh", 0,
            0,
            false,
            false,
            "",
            { "looper", "overdub", "utility" }
        },
        {
            "rev-246", "REV-246", "ikloX", "v1.0.1", "Reverb",
            "FX", "Stable", "MPC Live (3.10.1)", "MPC OS 3.x", "2026-11-23",
            "56", "20.8 MB", "29%",
            "ikloX - VST - REV-246", "rev246.so",
            "https://raw.githubusercontent.com/biloo77-cell/mpc-vst-rev246/5c3ecbbd3719a756dee9172c25d3afb3f7b45a18/docs/img/rev246-reverb.png", "https://github.com/biloo77-cell/mpc-vst-rev246/releases/download/rev-246-v1.0.1/REV-246-1.0.1-mpc-armv7.zip", "plugins/rev-246/thumb.png",
            "tehguitarist's REV-246, a vintage mid-80s style digital reverb: five programmes, 16-bit storage, transformer, ducker, true stereo or the original mono-in routing.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/ikloX - VST - REV-246\" && ZIP_PATH=\"./plugins/rev-246/rev-246.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/biloo77-cell/mpc-vst-rev246/releases/download/rev-246-v1.0.1/REV-246-1.0.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/rev-246\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/biloo77-cell/mpc-vst-rev246/releases/download/rev-246-v1.0.1/REV-246-1.0.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/ikloX - VST - REV-246/\"; elif [ -f \"./plugins/rev-246/rev246.so\" ]; then cp -rf ./plugins/rev-246/* \"$TARGET_SYNTH_DIR/ikloX - VST - REV-246/\"; else echo \"[ERROR] No local binary archive found for rev-246\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "reverb", "vintage", "digital", "stereo" }
        },
        {
            "reverbicon-224", "Reverbicon 224", "sd88me", "v1.0.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-17",
            "509", "483 KB", "14%",
            "sd88me - VST - Reverbicon 224", "reverbicon224.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-reverbicon224/7d68c952f5133e80f50fb256e0e088232cd761e0/docs/img/reverbicon224.png", "https://github.com/sd88me/mpc-vst-reverbicon224/releases/download/v1.0.0/Reverbicon-224-1.0.0-mpc-armv7.zip", "plugins/reverbicon-224/thumb.png",
            "A Lexicon 224 running its own v4.4 firmware on an emulated machine (8080, DSP, converters and analog boards): six programs, the original sliders and LED readout. Needs your own ROMs.", "./plugins/reverbicon-224/install.sh", 0,
            0,
            false,
            false,
            "",
            { "lexicon", "emulation", "reverb" }
        },
        {
            "rmxxxl", "RMXXXL", "L'Cronx", "v1.3.0", "Glitch Performance",
            "FX", "Stable", "Akai Force (3.9.1)", "MPC OS 3.x", "2026-10-29",
            "5194", "3.3 MB", "14%",
            "L'Cronx - VST - RMXXXL", "rmxxxl.so",
            "https://raw.githubusercontent.com/sunskiefer/RMXXXL/0ef9fe195525cada860205f1370a4b9f6896bc60/docs/img/remix.png", "https://github.com/sunskiefer/RMXXXL/releases/download/v1.3.0/RMXXXL-1.3.0-mpc-armv7.zip", "plugins/rmxxxl/thumb.png",
            "RMX-1000-style remix effect: isolator, DJ filter, Build Up / Break Down scenes, synced echo, Release FX and a Release kill switch, EQ Kill buttons, a clean Panic, Clouds, Dragonfly reverbs, sample pads with ADSR, 16 user presets and a brickwall limiter, with a big-button touchscreen skin and Q-Links.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - RMXXXL\" && ZIP_PATH=\"./plugins/rmxxxl/rmxxxl.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/RMXXXL/releases/download/v1.3.0/RMXXXL-1.3.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/rmxxxl\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/RMXXXL/releases/download/v1.3.0/RMXXXL-1.3.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - RMXXXL/\"; elif [ -f \"./plugins/rmxxxl/rmxxxl.so\" ]; then cp -rf ./plugins/rmxxxl/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - RMXXXL/\"; else echo \"[ERROR] No local binary archive found for rmxxxl\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "remix", "dj", "rmx", "isolator", "filter", "clouds", "reverb", "pads" }
        },
        {
            "samplerfilter", "SamplerFilter", "Aaron Quint", "v1.1.4", "Filter Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-12",
            "967", "3.1 MB", "4%",
            "quirkey - VST - SamplerFilter", "samplerfilter.so",
            "https://raw.githubusercontent.com/quirkey/mpc-plugins/samplerfilter-v1.1.4/docs/samplerfilter.png", "https://github.com/quirkey/mpc-plugins/releases/download/samplerfilter-v1.1.4/SamplerFilter-1.1.4-mpc-armv7.zip", "plugins/samplerfilter/thumb.png",
            "A late-80s rack-sampler style 6-pole low-pass (no resonance) with the sampler's bit-depth and sample-rate grit, drive and mix.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/quirkey - VST - SamplerFilter\" && ZIP_PATH=\"./plugins/samplerfilter/samplerfilter.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/quirkey/mpc-plugins/releases/download/samplerfilter-v1.1.4/SamplerFilter-1.1.4-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/samplerfilter\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/quirkey/mpc-plugins/releases/download/samplerfilter-v1.1.4/SamplerFilter-1.1.4-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/quirkey - VST - SamplerFilter/\"; elif [ -f \"./plugins/samplerfilter/samplerfilter.so\" ]; then cp -rf ./plugins/samplerfilter/* \"$TARGET_SYNTH_DIR/quirkey - VST - SamplerFilter/\"; else echo \"[ERROR] No local binary archive found for samplerfilter\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "lo-fi", "low-pass", "bitcrusher", "vintage-sampler" }
        },
        {
            "scatterloom", "Scatterloom", "Elsewave", "v1.0.0", "Glitch Performance",
            "FX", "Stable", "MPC X (3.10.1.12)", "MPC OS 3.x", "2026-11-18",
            "346", "7.9 MB", "14%",
            "Elsewave - VST - Scatterloom", "VST/scatterloom-arm32.so",
            "https://raw.githubusercontent.com/shhQuiet/ScatterloomMPC/b222a69cb86ec125c0b3226d1c90d2132cd8c4bf/docs/screenshots/scatterloom-main.png", "https://github.com/shhQuiet/ScatterloomMPC/releases/download/v1.0.0/Scatterloom-1.0.0-mpc-armv7.zip", "plugins/scatterloom/thumb.png",
            "Captures audio grains and weaves them into Euclidean rhythms with Markov-based grain selection.", "./plugins/scatterloom/install.sh", 0,
            0,
            false,
            false,
            "",
            { "granular", "sequencer", "euclidean" }
        },
        {
            "scratchivision", "Scratchivision Digital Vinyl System", "Slow Ops Music", "v1.0.0", "Glitch Performance",
            "FX", "Stable", "MPC Live III (Gen2) (3.10.1), MPC XL (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-14",
            "460", "822 KB", "14%",
            "Slow Ops - VST - Scratchivision", "VST/scratchivision-arm32.so",
            "https://raw.githubusercontent.com/drj-io/SlowOps.Scratchivision/v1.0.0/images/screenshot.png", "https://github.com/drj-io/SlowOps.Scratchivision/releases/download/v1.0.0/Scratchivision-1.0.0-mpc-armv7.zip", "plugins/scratchivision/thumb.png",
            "Digital vinyl system: scratch samples with a turntable using Serato-style timecode vinyl or a Phase-style wireless controller. Plays your sample at the record's speed and direction, with a live waveform and 5 hot cues.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Slow Ops - VST - Scratchivision\" && ZIP_PATH=\"./plugins/scratchivision/scratchivision.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/drj-io/SlowOps.Scratchivision/releases/download/v1.0.0/Scratchivision-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/scratchivision\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/drj-io/SlowOps.Scratchivision/releases/download/v1.0.0/Scratchivision-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Slow Ops - VST - Scratchivision/\"; elif [ -f \"./plugins/scratchivision/VST/scratchivision-arm32.so\" ]; then cp -rf ./plugins/scratchivision/* \"$TARGET_SYNTH_DIR/Slow Ops - VST - Scratchivision/\"; else echo \"[ERROR] No local binary archive found for scratchivision\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "dvs", "scratch", "turntable", "timecode", "vinyl", "dj" }
        },
        {
            "shapey-5-band-eq", "Shapey 5-Band EQ", "3dthc", "v1.0.2", "Filter Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-13",
            "2195", "3.4 MB", "14%",
            "3DTHC - VST - Shapey 5-Band EQ", "shapey_5band_eq.so",
            "https://raw.githubusercontent.com/3dthc/Shapey-MPC/ddbf9bbc9f66f2af0802070b38bb3d113140131c/docs/shapey-5-band-eq.png", "https://github.com/3dthc/Shapey-MPC/releases/download/shapey-5-band-eq-vst-v1.0.2/Shapey-5-Band-EQ-1.0.2-mpc-armv7.zip", "plugins/shapey-5-band-eq/thumb.png",
            "The 5-band version of Shapey EQ, made for touch: every band has its own lane under a taller curve, so any dot drags directly. Any band can be a bell, shelf, cut, notch or band pass. Live analyzer, zero latency. Also a VST3 for Mac and Windows and an Audio Unit for Mac.", "./plugins/shapey-5-band-eq/install.sh", 0,
            0,
            false,
            false,
            "",
            { "eq", "parametric-eq", "analyzer", "mixing" }
        },
        {
            "shapey-eq", "Shapey EQ", "3dthc", "v1.6.3", "Filter Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-13",
            "2379", "4.3 MB", "14%",
            "3DTHC - VST - Shapey EQ", "shapey_eq.so",
            "https://raw.githubusercontent.com/3dthc/Shapey-MPC/ddbf9bbc9f66f2af0802070b38bb3d113140131c/docs/shapey-eq.png", "https://github.com/3dthc/Shapey-MPC/releases/download/shapey-eq-vst-v1.6.3/Shapey-EQ-1.6.3-mpc-armv7.zip", "plugins/shapey-eq/thumb.png",
            "An 8-band parametric EQ you shape by touch: tap or drag on the curve, drag numbered dots, and make any band a bell, shelf, cut, notch or band pass. Live analyzer, zero latency, presets and four themes. Also a VST3 for Mac and Windows and an Audio Unit for Mac.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/3DTHC - VST - Shapey EQ\" && ZIP_PATH=\"./plugins/shapey-eq/shapey-eq.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/3dthc/Shapey-MPC/releases/download/shapey-eq-vst-v1.6.3/Shapey-EQ-1.6.3-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/shapey-eq\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/3dthc/Shapey-MPC/releases/download/shapey-eq-vst-v1.6.3/Shapey-EQ-1.6.3-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/3DTHC - VST - Shapey EQ/\"; elif [ -f \"./plugins/shapey-eq/shapey_eq.so\" ]; then cp -rf ./plugins/shapey-eq/* \"$TARGET_SYNTH_DIR/3DTHC - VST - Shapey EQ/\"; else echo \"[ERROR] No local binary archive found for shapey-eq\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "eq", "parametric-eq", "analyzer", "mixing" }
        },
        {
            "slow-ops-looper", "Slow Ops Looper", "Slow Ops Music", "v1.1.0", "Glitch Performance",
            "FX", "Stable", "MPC Live III (Gen2) (3.10.1), MPC XL (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-13",
            "266", "1.1 MB", "14%",
            "Slow Ops - VST - Looper", "VST/slow-ops-looper-arm32.so",
            "https://raw.githubusercontent.com/drj-io/SlowOps.MPC.Looper/v1.1.0/docs/images/loops-page.png", "https://github.com/drj-io/SlowOps.MPC.Looper/releases/download/v1.1.0/Looper-1.1.0-mpc-armv7.zip", "plugins/slow-ops-looper/thumb.png",
            "Three-bank stereo looper insert. Captures 1, 2, 4 or 8 bars from the track in time with the MPC, with overdub, per-bank and master play/stop on the beat, solo, mute and volume per bank, a metronome with auto standby, and latency compensation.", "./plugins/slow-ops-looper/install.sh", 0,
            0,
            false,
            false,
            "",
            { "looper", "overdub", "tempo-sync", "metronome", "performance" }
        },
        {
            "op1fx-spring", "Spring", "Bombest Audio", "v0.6.0", "Reverb",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "1363", "3.4 MB", "14%",
            "Bombest Audio - VST - Spring", "spring.so",
            "https://raw.githubusercontent.com/Bombest-Audio/op1-fx/34f1396d82a4ae72baa761a50f1d9bfe834c444f/screenshots/mpc/spring.png", "https://github.com/Bombest-Audio/op1-fx/releases/download/v0.6.0/Spring-0.6.0-mpc-armv7.zip", "plugins/op1fx-spring/thumb.png",
            "Spring reverb with a 12-preset bank. OP-1 inspired.", "./plugins/op1fx-spring/install.sh", 0,
            0,
            false,
            false,
            "",
            { "op1", "reverb", "spring" }
        },
        {
            "stompbox", "Stompbox", "jacob-sabella", "v1.1.2", "Distortion",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-16",
            "2099", "6.8 MB", "21%",
            "jacob-sabella - VST - Stompbox", "stompbox.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-stompbox/bc70be3dd01103bc792db0c2b0e54a589fc37d36/docs/screenshots/stompbox.png", "https://github.com/jacob-sabella/mpc-vst-stompbox/releases/download/stompbox-vst-v1.1.2/Stompbox-1.1.2-mpc-armv7.zip", "plugins/stompbox/thumb.png",
            "A whole guitar rig from guitarix's DSP: gate, comp, wah, any number of pedals, amp, tone stack, power amp, cabinet with an IR loader, post FX, mod, delay, reverb and EQ. 219 models and guitarix's 40 factory presets in the PRESET menu. Not affiliated with guitarix.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Stompbox\" && ZIP_PATH=\"./plugins/stompbox/stompbox.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/jacob-sabella/mpc-vst-stompbox/releases/download/stompbox-vst-v1.1.2/Stompbox-1.1.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/stompbox\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-vst-stompbox/releases/download/stompbox-vst-v1.1.2/Stompbox-1.1.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Stompbox/\"; elif [ -f \"./plugins/stompbox/stompbox.so\" ]; then cp -rf ./plugins/stompbox/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Stompbox/\"; else echo \"[ERROR] No local binary archive found for stompbox\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "guitar", "amp", "pedals", "cabinet", "cab-ir", "guitarix", "presets" }
        },
        {
            "stream", "Stream", "Stampman3000", "v1.0.1", "Utility",
            "FX", "Stable", "MPC One (3.10.1)", "MPC OS 3.x", "2026-11-20",
            "1236", "2.7 MB", "14%",
            "Stampman3000 - VST - Stream", "stream.so",
            "https://raw.githubusercontent.com/stampman3000-jpg/mpc-stream/v1.0.1/screenshots/stream.png", "https://github.com/stampman3000-jpg/mpc-stream/releases/download/v1.0.1/Stream-1.0.1-mpc-armv7.zip", "plugins/stream/thumb.png",
            "Multichannel USB audio effect send from an MPC to a Mac. Needs BlackHole 64ch and a Mac helper app.", "./plugins/stream/install.sh", 0,
            0,
            false,
            false,
            "",
            { "utility" }
        },
        {
            "studio424", "STUDIO424", "Lightlike", "v1.0.0", "Mixing",
            "FX", "Stable", "MPC One (3.10.1)", "MPC OS 3.x", "2026-11-16",
            "402", "984 KB", "14%",
            "Lightlike - VST - STUDIO424", "VST/studio424-arm32.so",
            "https://raw.githubusercontent.com/lightlikemusic/studio424/v1.0.0/docs/screenshot.png", "https://github.com/open-mpc/mpc-vst-plugins/releases/download/sdk-converted/studio424-1.0.0-756184946-mpc-armv7.zip", "plugins/studio424/thumb.png",
            "Brings the warm, slightly rough sound of an old cassette four-track mixer to your MPC. Push it for crunch, or leave it clean for a little colour.", "./plugins/studio424/install.sh", 0,
            0,
            false,
            false,
            "",
            { "channel-strip", "console", "eq", "saturation", "lo-fi", "mixing" }
        },
        {
            "tidal-verb", "Tidal Verb", "Fullban", "v1.6.0", "Reverb",
            "FX", "Stable", "Akai Force (Gen1) (3.10.1), MPC XL (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-15",
            "898", "216 KB", "14%",
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
            "toucheq-8", "TouchEQ 8", "GreasyCratez", "v0.1.3", "Filter Eq",
            "FX", "Stable", "MPC Key 61 (3.10.1.12)", "MPC OS 3.x", "2026-11-20",
            "248", "1.8 MB", "14%",
            "GreasyCratez - VST - TouchEQ 8", "VST/toucheq-8-arm32.so",
            "https://raw.githubusercontent.com/GreasySavage/TouchEQ-8/v0.1.3/docs/screenshots/toucheq-8.png", "https://github.com/GreasyCratez/TouchEQ-8/releases/download/v0.1.3/TouchEQ-8-0.1.3-mpc-armv7.zip", "plugins/toucheq-8/thumb.png",
            "8-band parametric EQ with a live filled curve and spectrum analyser: pick a band and drag its point with a finger. Each band is one row of four Q-Links (Freq, Gain, Q, Type), and QLink Follow moves the knobs to the band you tap. 48/12 dB cuts, shelves, bells and notch, Adaptive Q, Scale and Mix.", "./plugins/toucheq-8/install.sh", 0,
            0,
            false,
            false,
            "",
            { "eq", "parametric-eq", "analyzer", "mixing", "touch" }
        },
        {
            "tr-mpc-tap-fx", "TR-MPC Tap FX", "sd88me", "v1.3.0", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-19",
            "2987", "516 KB", "14%",
            "sd88me - VST - TR-MPC Tap FX", "trmpc_tapfx.so",
            "https://raw.githubusercontent.com/sd88me/mpc-vst-TR-MPC/7e704c6f3be14b661c887631e203494d8cc3f74f/docs/img/tr-mpc-tap-fx.png", "https://github.com/sd88me/mpc-vst-TR-MPC/releases/download/trmpc-tap-fx-vst-v1.3.0/TR-MPC-Tap-FX-1.3.0-mpc-armv7.zip", "plugins/tr-mpc-tap-fx/thumb.png",
            "Companion to TR-MPC: sends any slot, or the kit's reverb or delay send, out on its own MPC track, sample-aligned with the kit, so MPC's mixer, submixes and insert effects can process it. Needs TR-MPC in the same project.", "./plugins/tr-mpc-tap-fx/install.sh", 0,
            0,
            false,
            false,
            "",
            { "routing", "multi-out", "drum" }
        },
        {
            "tuner", "Tuner", "jacob-sabella", "v1.1.2", "Utility",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-16",
            "1771", "670 KB", "1%",
            "jacob-sabella - VST - Tuner", "tuner.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-vst-tuner/a3e4c099eacce3cdd440d54eb3ff059c96baafa5/docs/screenshots/needle.png", "https://github.com/jacob-sabella/mpc-vst-tuner/releases/download/tuner-vst-v1.1.2/Tuner-1.1.2-mpc-armv7.zip", "plugins/tuner/thumb.png",
            "A chromatic tuner insert with a live needle drawn straight onto the MPC's screen at 20-24 fps: note, cents and Hz, A4 from 430 to 450 Hz, and a mute for silent tuning. It writes into MPC's display memory, which MPC OS doesn't offer plugins, so a firmware update could break it. Not affiliated with Akai.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Tuner\" && ZIP_PATH=\"./plugins/tuner/tuner.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/jacob-sabella/mpc-vst-tuner/releases/download/tuner-vst-v1.1.2/Tuner-1.1.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/tuner\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-vst-tuner/releases/download/tuner-vst-v1.1.2/Tuner-1.1.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Tuner/\"; elif [ -f \"./plugins/tuner/tuner.so\" ]; then cp -rf ./plugins/tuner/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - Tuner/\"; else echo \"[ERROR] No local binary archive found for tuner\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "tuner", "guitar", "bass", "pitch", "screen-draw" }
        },
        {
            "tuney", "Tuney", "3dthc", "v1.4.4", "Pitch Vocal",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-13",
            "1711", "9.3 MB", "14%",
            "3DTHC - VST - Tuney", "tuney.so",
            "https://raw.githubusercontent.com/3dthc/Tuney-MPC/55862ef83ab6938b5f1e3e20f5fa1ead7449e9b2/docs/tuney.png", "https://github.com/3dthc/Tuney-MPC/releases/download/tuney-vst-v1.4.4/Tuney-1.4.4-mpc-armv7.zip", "plugins/tuney/thumb.png",
            "Real-time vocal pitch correction: key and scale or your own notes on a touch keyboard, a pitch dial, Retune, Transition, Amount, Vibrato and Humanize, voice ranges and a reference pitch. Presets from hard tune to subtle. Also a VST3 for Mac and Windows and an Audio Unit for Mac.", "./plugins/tuney/install.sh", 0,
            0,
            false,
            false,
            "",
            { "pitch-correction", "vocal", "tuner", "effect" }
        },
        {
            "twenty-fx", "Twenty FX", "Stampman3000", "v1.0.0", "Filter Eq",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-20",
            "151", "1.8 MB", "14%",
            "Stampman3000 - VST - Twenty FX", "VST/twenty-fx-arm32.so",
            "https://raw.githubusercontent.com/stampman3000-jpg/twenty/v1.0.0/screenshots/twenty-fx.png", "https://github.com/stampman3000-jpg/twenty/releases/download/v1.0.0/Twenty-FX-1.0.0-mpc-armv7.zip", "plugins/twenty-fx/thumb.png",
            "Twenty's filters, ESP, vocoder and patchbay as an insert effect for any track. DSP by Audio Tools' AT20V.", "./plugins/twenty-fx/install.sh", 0,
            0,
            false,
            false,
            "",
            { "effect" }
        },
        {
            "undertow", "Undertow", "Fullban", "v1.5.0", "Delay",
            "FX", "Stable", "Akai Force (Gen1) (3.10.1), MPC XL (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-15",
            "711", "1.1 MB", "14%",
            "Fullban - VST - Undertow", "undertow.so",
            "https://raw.githubusercontent.com/fullbann/undertow/e357bf20fe7c4fc1e68b73097c514bce38043b8d/docs/catalog-thumbnail.png", "https://github.com/fullbann/undertow/releases/download/v1.5.0/Undertow-1.5.0-mpc-armv7.zip", "plugins/undertow/thumb.png",
            "Altered stereo delay with Analogue, Tape, Lo-Fi and Reverse modes, degrading repeats, runaway feedback, tempo sync and Tails. Nine presets, sepia touchscreen skin.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Fullban - VST - Undertow\" && ZIP_PATH=\"./plugins/undertow/undertow.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/fullbann/undertow/releases/download/v1.5.0/Undertow-1.5.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/undertow\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/fullbann/undertow/releases/download/v1.5.0/Undertow-1.5.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Fullban - VST - Undertow/\"; elif [ -f \"./plugins/undertow/undertow.so\" ]; then cp -rf ./plugins/undertow/* \"$TARGET_SYNTH_DIR/Fullban - VST - Undertow/\"; else echo \"[ERROR] No local binary archive found for undertow\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "delay", "tape", "lo-fi", "reverse", "stereo" }
        },
        {
            "verglas", "Verglas", "saustin2010", "v1.2.1", "Granular",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "3410", "642 KB", "14%",
            "Émilie Gillet - VST - [FX] Verglas", "verglas.so",
            "https://raw.githubusercontent.com/saustin2010/mpc-vst-verglas/v1.2.1/screenshots/page_0.png", "https://github.com/saustin2010/mpc-vst-verglas/releases/download/v1.2.1/FX-Verglas-1.2.1-mpc-armv7.zip", "plugins/verglas/thumb.png",
            "Verglas: granular, stretch, looper and spectral modes and freeze as an insert effect; 12 presets, from Émilie Gillet's open-source Clouds.", "./plugins/verglas/install.sh", 0,
            0,
            false,
            false,
            "",
            { "clouds", "granular", "texture", "eurorack" }
        },
        {
            "vhs-vibes", "VHS Vibes", "anthonytheinventor", "v1.8.0", "Lofi Tape",
            "FX", "Stable", "MPC One+ (3.10.1.12)", "MPC OS 3.x", "2026-11-22",
            "2259", "15.2 MB", "14%",
            "anthonytheinventor - VST - VHS Vibes", "VST/vhs-vibes-arm32.so",
            "https://raw.githubusercontent.com/anthonytheinventor/vhs-vibes/v1.8.0/docs/page_1_tape_hifi.png", "https://github.com/anthonytheinventor/vhs-vibes/releases/download/v1.8.0/VHS-Vibes-1.8.0-mpc-armv7.zip", "plugins/vhs-vibes/thumb.png",
            "A worn video-tape effect: pitch wobble, tape saturation with tape hysteresis, the Hi-Fi track, generation loss, B-type noise reduction, hum, hiss and crackle, plus deck faults (level control, drift, tracking dropouts, a tilted head) and a dry/wet mix. 54 programs, on a hot pink cassette with reels that turn and a picture that shows the sound.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/anthonytheinventor - VST - VHS Vibes\" && ZIP_PATH=\"./plugins/vhs-vibes/vhs-vibes.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/anthonytheinventor/vhs-vibes/releases/download/v1.8.0/VHS-Vibes-1.8.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/vhs-vibes\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/anthonytheinventor/vhs-vibes/releases/download/v1.8.0/VHS-Vibes-1.8.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/anthonytheinventor - VST - VHS Vibes/\"; elif [ -f \"./plugins/vhs-vibes/VST/vhs-vibes-arm32.so\" ]; then cp -rf ./plugins/vhs-vibes/* \"$TARGET_SYNTH_DIR/anthonytheinventor - VST - VHS Vibes/\"; else echo \"[ERROR] No local binary archive found for vhs-vibes\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "tape", "lo-fi", "saturation", "modulation", "noise" }
        },
        {
            "vibe-fx", "Vibe FX", "nachtaktiv303", "v1.0.0", "Multi Fx",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-10-23",
            "2112", "906 KB", "14%",
            "nachtaktiv303 - VST - Vibe FX", "mpc_fx.so",
            "https://raw.githubusercontent.com/nachtaktiv303/mpc-vst-vibe-fx/main/preview_1.png", "https://github.com/nachtaktiv303/mpc-vst-vibe-fx/releases/download/vibe-fx-vst-v1.0.0/Vibe-FX-1.0.0-mpc-armv7.zip", "plugins/vibe-fx/thumb.png",
            "Drive, chorus, FDN reverb and tape delay in one native MPC OS insert effect, with a touchscreen skin and Q-Links.", "./plugins/vibe-fx/install.sh", 0,
            0,
            false,
            false,
            "",
            { "drive", "chorus", "reverb", "delay", "multi-fx", "insert" }
        },
        {
            "vintage-military-radio", "Vintage Military Radio", "Tropix R!X Crazy Jungle Sounds", "v1.0.16", "Lofi Tape",
            "FX", "Stable", "MPC XL (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-14",
            "749", "5.3 MB", "14%",
            "Tropix R!X Crazy Jungle Sounds - VST - Vintage Military Radio", "VST/vintage-military-radio-arm32.so",
            "https://raw.githubusercontent.com/vulnerablecodes/music-production/v1.0.16/media/vintage-military-radio-current.jpg", "https://github.com/vulnerablecodes/music-production/releases/download/v1.0.16/Vintage-Military-Radio-1.0.16-mpc-armv7.zip", "plugins/vintage-military-radio/thumb.png",
            "Eight stereo tape-performance effects with six procedural noise layers and native chrome controls.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Tropix R!X Crazy Jungle Sounds - VST - Vintage Military Radio\" && ZIP_PATH=\"./plugins/vintage-military-radio/vintage-military-radio.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/vulnerablecodes/music-production/releases/download/v1.0.16/Vintage-Military-Radio-1.0.16-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/vintage-military-radio\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/vulnerablecodes/music-production/releases/download/v1.0.16/Vintage-Military-Radio-1.0.16-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Tropix R!X Crazy Jungle Sounds - VST - Vintage Military Radio/\"; elif [ -f \"./plugins/vintage-military-radio/VST/vintage-military-radio-arm32.so\" ]; then cp -rf ./plugins/vintage-military-radio/* \"$TARGET_SYNTH_DIR/Tropix R!X Crazy Jungle Sounds - VST - Vintage Military Radio/\"; else echo \"[ERROR] No local binary archive found for vintage-military-radio\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "tape", "lo-fi", "saturation", "performance", "noise" }
        },
        {
            "voxboy", "Voxboy", "Stampman3000", "v1.0.0", "Pitch Vocal",
            "FX", "Stable", "MPC One (3.10.1)", "MPC OS 3.x", "2026-11-20",
            "178", "475 KB", "14%",
            "Stampman3000 - VST - Voxboy", "VST/voxboy-arm32.so",
            "https://raw.githubusercontent.com/stampman3000-jpg/voxman/v1.0.0/screenshots/voxboy.png", "https://github.com/stampman3000-jpg/voxman/releases/download/v1.0.0/Voxboy-1.0.0-mpc-armv7.zip", "plugins/voxboy/thumb.png",
            "Carrier send for Voxman: put it on a bass or synth track to make it the vocoder's carrier.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Stampman3000 - VST - Voxboy\" && ZIP_PATH=\"./plugins/voxboy/voxboy.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/stampman3000-jpg/voxman/releases/download/v1.0.0/Voxboy-1.0.0-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/voxboy\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/stampman3000-jpg/voxman/releases/download/v1.0.0/Voxboy-1.0.0-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Stampman3000 - VST - Voxboy/\"; elif [ -f \"./plugins/voxboy/VST/voxboy-arm32.so\" ]; then cp -rf ./plugins/voxboy/* \"$TARGET_SYNTH_DIR/Stampman3000 - VST - Voxboy/\"; else echo \"[ERROR] No local binary archive found for voxboy\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "vocoder" }
        },
        {
            "voxman", "Voxman", "Stampman3000", "v1.0.0", "Pitch Vocal",
            "FX", "Stable", "MPC One (3.10.1)", "MPC OS 3.x", "2026-11-20",
            "182", "1.2 MB", "14%",
            "Stampman3000 - VST - Voxman", "VST/voxman-arm32.so",
            "https://raw.githubusercontent.com/stampman3000-jpg/voxman/v1.0.0/screenshots/voxman.png", "https://github.com/stampman3000-jpg/voxman/releases/download/v1.0.0/Voxman-1.0.0-mpc-armv7.zip", "plugins/voxman/thumb.png",
            "8 to 20 band vocoder with an internal string carrier, or play it from any track with Voxboy.", "./plugins/voxman/install.sh", 0,
            0,
            false,
            false,
            "",
            { "vocoder" }
        },
        {
            "vulcan-rotary", "Vulcan Rotary", "Lumina Aerospace LLC", "v1.0.1", "Glitch Performance",
            "FX", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 3.x", "2026-11-19",
            "188", "3.3 MB", "14%",
            "Lumina Aerospace LLC - VST - Vulcan Rotary", "VST/vulcan-rotary-arm32.so",
            "https://raw.githubusercontent.com/Luminous-Works/mpc-vst-vulcan-rotary/v1.0.0/docs/screenshots/vulcan-rotary.png", "https://github.com/Luminous-Works/mpc-vst-vulcan-rotary/releases/download/v1.0.1/Vulcan-Rotary-1.0.1-mpc-armv7.zip", "plugins/vulcan-rotary/thumb.png",
            "Six-barrel rotary gate and stutter: the barrels spool up from rest, rates in real gun rounds per minute (2,500 / 4,000 / 6,000 / 7,200).", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Lumina Aerospace LLC - VST - Vulcan Rotary\" && ZIP_PATH=\"./plugins/vulcan-rotary/vulcan-rotary.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/Luminous-Works/mpc-vst-vulcan-rotary/releases/download/v1.0.1/Vulcan-Rotary-1.0.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/vulcan-rotary\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/Luminous-Works/mpc-vst-vulcan-rotary/releases/download/v1.0.1/Vulcan-Rotary-1.0.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Lumina Aerospace LLC - VST - Vulcan Rotary/\"; elif [ -f \"./plugins/vulcan-rotary/VST/vulcan-rotary-arm32.so\" ]; then cp -rf ./plugins/vulcan-rotary/* \"$TARGET_SYNTH_DIR/Lumina Aerospace LLC - VST - Vulcan Rotary/\"; else echo \"[ERROR] No local binary archive found for vulcan-rotary\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "gate", "stutter", "rotary", "rhythm", "scatter" }
        },
        {
            "xxl-pro", "XXL Pro", "L'Cronx", "v0.1.0", "Multi Fx",
            "FX", "Stable", "Akai Force (3.9)", "MPC OS 3.x", "2026-11-14",
            "514", "30.1 MB", "14%",
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
            "562", "11.0 MB", "14%",
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
            "xxl-strip-bus", "XXL STRIP BUS", "L'Cronx", "v0.1.1", "Dynamics",
            "FX", "Stable", "Akai Force (3.9)", "MPC OS 3.x", "2026-11-14",
            "545", "8.9 MB", "14%",
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
            "534", "12.3 MB", "14%",
            "L'Cronx - VST - XXL STRIP MASTER", "xxlstripmaster.so",
            "https://raw.githubusercontent.com/sunskiefer/XXL-Strips/xxl-v0.1/XXL-STRIP-MASTER/docs/img/page_0.png", "https://github.com/sunskiefer/XXL-Strips/releases/download/xxl-v0.1/XXL-STRIP-MASTER-0.1.1-mpc-armv7.zip", "plugins/xxl-strip-master/thumb.png",
            "Master-bus strip in the same console style as XXL STRIP: drive and density, low and high shelving EQ, high-frequency compressor, stereo width and space, transformer stage, high-pass filter, VU meter. By L'Cronx, GPL-3.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/L'Cronx - VST - XXL STRIP MASTER\" && ZIP_PATH=\"./plugins/xxl-strip-master/xxl-strip-master.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/sunskiefer/XXL-Strips/releases/download/xxl-v0.1/XXL-STRIP-MASTER-0.1.1-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/xxl-strip-master\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/sunskiefer/XXL-Strips/releases/download/xxl-v0.1/XXL-STRIP-MASTER-0.1.1-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/L'Cronx - VST - XXL STRIP MASTER/\"; elif [ -f \"./plugins/xxl-strip-master/xxlstripmaster.so\" ]; then cp -rf ./plugins/xxl-strip-master/* \"$TARGET_SYNTH_DIR/L'Cronx - VST - XXL STRIP MASTER/\"; else echo \"[ERROR] No local binary archive found for xxl-strip-master\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "console", "mastering", "saturation", "eq", "compressor", "stereo", "mixing" }
        },
        {
            "z950", "Z950", "Keebz", "v1.0.1", "Lofi Tape",
            "FX", "Stable", "MPC Live III (Gen2) (3.10.1)", "MPC OS 3.x", "2026-11-18",
            "362", "4.9 MB", "14%",
            "Keebz - VST - Z950", "VST/z950-arm32.so",
            "https://raw.githubusercontent.com/keebz/Z950/v1.0.1/docs/readme/hero.png", "https://github.com/keebz/Z950/releases/download/v1.0.1/Z950-1.0.1-mpc-armv7.zip", "plugins/z950/thumb.png",
            "Live Akai S-series style time stretch and pitch shift with an S950 filter, a CD-deck cue/scrub and a looper with adjustable sample rate.", "./plugins/z950/install.sh", 0,
            0,
            false,
            false,
            "",
            { "time-stretch", "sampler", "lo-fi", "looper", "sseries", "s950", "s900", "filter" }
        }
        };
        if (!sec.items.empty()) sections.push_back(sec);
    }
    {
        Section sec;
        sec.title = "ADDINS";
        sec.items = {
        {
            "force-link-audio", "Force Link Audio", "Mac Digi / Rare Data", "v1.1.0", "Audio",
            "ADDIN", "Stable", "Akai Force (3.9.1.2)", "MPC OS 2.x & 3.x", "2026-10-14",
            "970", "257 KB", "14%",
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
            "mjx-mpc-twister", "MJX MPC Twister", "Michał Jasikowski", "v1.0.1", "Control",
            "ADDIN", "Stable", "MPC Live III (Gen2) (3.10.1.12)", "MPC OS 2.x & 3.x", "2026-11-19",
            "89", "218 KB", "14%",
            "Michał Jasikowski - VST - MJX MPC Twister", "twister-control.so",
            "", "https://github.com/mjasikowski/mjx-mpc-twister/releases/download/v1.0.1/MJX-MPC-Twister-1.0.1-mpc-aarch64.zip", "plugins/mjx-mpc-twister/thumb.png",
            "MPC Live III on MPC OS 3.10.1.12 only. Turn your Twister knobs to reach all 16 Q-Links without changing banks. Press a knob to jump to a track or open a shortcut. The lights follow your settings and track selection. Installation needs root access, writes to internal storage, and restarts the MPC app.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/Michał Jasikowski - VST - MJX MPC Twister\" && ZIP_PATH=\"./plugins/mjx-mpc-twister/mjx-mpc-twister.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/mjasikowski/mjx-mpc-twister/releases/download/v1.0.1/MJX-MPC-Twister-1.0.1-mpc-aarch64.zip\" ]; then mkdir -p \"./plugins/mjx-mpc-twister\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/mjasikowski/mjx-mpc-twister/releases/download/v1.0.1/MJX-MPC-Twister-1.0.1-mpc-aarch64.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/Michał Jasikowski - VST - MJX MPC Twister/\"; elif [ -f \"./plugins/mjx-mpc-twister/twister-control.so\" ]; then cp -rf ./plugins/mjx-mpc-twister/* \"$TARGET_SYNTH_DIR/Michał Jasikowski - VST - MJX MPC Twister/\"; else echo \"[ERROR] No local binary archive found for mjx-mpc-twister\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "midi", "controller", "qlink" }
        },
        {
            "commander", "MPC Commander", "jacob-sabella", "v1.3.2", "Control",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-16",
            "6925", "254 KB", "14%",
            "jacob-sabella - VST - MPC Commander", "mpc_commander_addin.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-commander/763c959e1855fd3bcec7dfd6832b4be77a502e4f/docs/tile.png", "https://github.com/jacob-sabella/mpc-addin-commander/releases/download/commander-addin-v1.3.2/MPC-Commander-addin-1.3.2-mpc-armv7.zip", "plugins/commander/thumb.png",
            "The plugins MPC loads, with their own skins, and MPC's transport, on a desktop app. Listens on the device only: reach it through an SSH tunnel (no login; bind=0.0.0.0 opens it to your network).", "./plugins/commander/install.sh", 0,
            0,
            false,
            false,
            "",
            { "desktop", "plugins", "skins", "transport", "midi" }
        },
        {
            "filebrowser", "MPC File Browser", "jacob-sabella", "v1.1.1", "Utility",
            "ADDIN", "Stable", "MPC Key 37 (Gen1) (3.10.1.12), MPC Sample (1.3.0.12)", "MPC OS 2.x & 3.x", "2026-11-13",
            "2983", "19.2 MB", "14%",
            "jacob-sabella - VST - MPC File Browser", "mpc_filebrowser_addin.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-filebrowser/88b86eb39f907485995497cf0eaeeb5ddd7aa3e4/docs/tile.png", "https://github.com/jacob-sabella/mpc-addin-filebrowser/releases/download/filebrowser-addin-v1.1.1/MPC-File-Browser-1.1.1-mpc-armv7.zip", "plugins/filebrowser/thumb.png",
            "FileBrowser Quantum beside MPC: browse the device's drives in a web browser, upload and download samples and projects, play audio, edit text files. Listens on the device only: reach it through an SSH tunnel (login admin / mpcadmin; change it). bind=0.0.0.0 opens it to your network.", "./plugins/filebrowser/install.sh", 0,
            0,
            false,
            false,
            "",
            { "files", "browser", "upload", "samples", "editor" }
        },
        {
            "gateway", "MPC Gateway", "jacob-sabella", "v1.0.2", "Utility",
            "ADDIN", "Stable", "MPC Key 37 (Gen1) (3.10.1.12), MPC Sample (1.3.0.12)", "MPC OS 2.x & 3.x", "2026-11-13",
            "1425", "16.2 MB", "14%",
            "jacob-sabella - VST - MPC Gateway", "mpc_gateway_addin.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-gateway/f17f7722faee4805da75955d1b09e572a366d8b3/docs/card.png", "https://github.com/jacob-sabella/mpc-addin-gateway/releases/download/gateway-addin-v1.0.2/MPC-Gateway-1.0.2-mpc-armv7.zip", "plugins/gateway/thumb.png",
            "One HTTPS address and one login (mpc / mpc until you change it on its Settings page) for every addin's web page. A start page lists the services addins declare (ADDIN_WEB_PORT), each routed behind the password. Runs Caddy beside MPC. Device only by default (reach it through an SSH tunnel); bind=0.0.0.0 opens it to your network.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Gateway\" && ZIP_PATH=\"./plugins/gateway/gateway.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/jacob-sabella/mpc-addin-gateway/releases/download/gateway-addin-v1.0.2/MPC-Gateway-1.0.2-mpc-armv7.zip\" ]; then mkdir -p \"./plugins/gateway\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-addin-gateway/releases/download/gateway-addin-v1.0.2/MPC-Gateway-1.0.2-mpc-armv7.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Gateway/\"; elif [ -f \"./plugins/gateway/mpc_gateway_addin.so\" ]; then cp -rf ./plugins/gateway/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Gateway/\"; else echo \"[ERROR] No local binary archive found for gateway\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "network", "https", "login", "web", "gateway" }
        },
        {
            "remote", "MPC Remote", "jacob-sabella", "v1.4.1", "Control",
            "ADDIN", "Stable", "MPC Key 37 (Gen1) (3.10.1.12), MPC Sample (1.3.0.12)", "MPC OS 2.x & 3.x", "2026-11-13",
            "7920", "77 KB", "14%",
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
            "811", "64 KB", "14%",
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
            "sample-rack", "MPC Sample Rack", "jacob-sabella", "v1.0.1", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-21",
            "851", "71 KB", "14%",
            "jacob-sabella - VST - MPC Sample Rack", "mpc_sample_rack.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-sample-rack/c2a988ddc52bf92dc3bf83f5f4283cadc78eeb47/docs/screenshots/rack-page.png", "https://github.com/jacob-sabella/mpc-addin-sample-rack/releases/download/sample-rack-addin-v1.0.1/MPC-Sample-rack-addin-1.0.1-mpc-aarch64.zip", "plugins/sample-rack/thumb.png",
            "Add, remove and turn on or off the plugins in a project's inserts from a RACK page on the Sample. APPLY saves and restarts MPC: save first.", "export TARGET_SYNTH_DIR=\"${TARGET_SYNTH_DIR:-${SYNTH_INSTALL_DIR:-./VST2 Plugins}}\" && mkdir -p \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Sample Rack\" && ZIP_PATH=\"./plugins/sample-rack/sample-rack.zip\" && if [ ! -f \"$ZIP_PATH\" ] && [ -n \"https://github.com/jacob-sabella/mpc-addin-sample-rack/releases/download/sample-rack-addin-v1.0.1/MPC-Sample-rack-addin-1.0.1-mpc-aarch64.zip\" ]; then mkdir -p \"./plugins/sample-rack\" && (\"${PLG_MAN_DIR:-.}/mpc_plugin_manager_fb\" --fetch \"https://github.com/jacob-sabella/mpc-addin-sample-rack/releases/download/sample-rack-addin-v1.0.1/MPC-Sample-rack-addin-1.0.1-mpc-aarch64.zip\" \"$ZIP_PATH\"); fi && if [ -f \"$ZIP_PATH\" ]; then unzip -o \"$ZIP_PATH\" -d \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Sample Rack/\"; elif [ -f \"./plugins/sample-rack/mpc_sample_rack.so\" ]; then cp -rf ./plugins/sample-rack/* \"$TARGET_SYNTH_DIR/jacob-sabella - VST - MPC Sample Rack/\"; else echo \"[ERROR] No local binary archive found for sample-rack\" && exit 1; fi && sync", 1,
            0,
            false,
            false,
            "",
            { "plugins", "inserts" }
        },
        {
            "sample-scalemode", "MPC Sample Scale Mode", "jacob-sabella", "v1.2.0", "Utility",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-10",
            "1031", "321 KB", "14%",
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
            "skipback", "MPC Skipback", "sd88me", "v0.6.3", "Audio",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-12",
            "3264", "64 KB", "14%",
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
            "usb-audio", "MPC USB audio", "jacob-sabella", "v1.2.1", "Audio",
            "ADDIN", "Stable", "MPC Key 37 (Gen1) (3.10.1.12)", "MPC OS 2.x & 3.x", "2026-11-13",
            "7612", "64 KB", "14%",
            "jacob-sabella - VST - MPC USB audio", "libmpc_usb_audio.so",
            "https://raw.githubusercontent.com/jacob-sabella/mpc-addin-usb-audio/f73cf3a1de3c3d005f30b6d38caddf587376e6d4/docs/tile.png", "https://github.com/jacob-sabella/mpc-addin-usb-audio/releases/download/usb-audio-addin-v1.2.1/MPC-USB-audio-addin-1.2.1-mpc-armv7.zip", "plugins/usb-audio/thumb.png",
            "The MPC as a USB audio interface: its outputs and inputs as channels on the computer, next to its MIDI ports. Works where the device port is wired to the USB-B socket (Key 37 verified; One, One+). Not on the Force, Live II or X: their USB-B port sits behind the internal hub in standalone mode; use force-link-audio there.", "./plugins/usb-audio/install.sh", 0,
            0,
            false,
            false,
            "",
            { "usb", "audio-interface", "recording", "inputs" }
        },
        {
            "nova-audio", "Nova Audio", "3dthc", "v0.2.0", "Audio",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-03",
            "660", "64 KB", "14%",
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
            "piremote", "πRemote", "Timo", "v0.1.0", "Control",
            "ADDIN", "Stable", "MPC Live II / Force (Gen1)", "MPC OS 2.x & 3.x", "2026-11-13",
            "273", "64 KB", "14%",
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
    std::string targetPath = configPath.empty() ? "mpc_plugins.conf" : configPath;
    if (targetPath[0] != '/') {
        std::string mgrDir = getManagerRootDir();
        if (!mgrDir.empty() && mgrDir != "/") {
            targetPath = mgrDir + "/" + targetPath;
        }
    }
    std::ofstream out(targetPath, std::ios::binary | std::ios::trunc);
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
            if (!it.tags.empty()) {
                out << "          \"tags\": [";
                for (size_t t = 0; t < it.tags.size(); ++t) {
                    out << "\"" << escapeJsonString(it.tags[t]) << "\"" << (t + 1 < it.tags.size() ? ", " : "");
                }
                out << "],\n";
            }
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
    out.flush();
    bool ok = !out.fail();
    out.close();
    sync();
    return ok;
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

static bool s_mpcBinVersionCached = false;
static MpcBinaryVersionInfo s_mpcBinVersionInfo;

static bool tryParseMpcVersionAt(
    const unsigned char* data,
    size_t len,
    size_t mpcPos,
    MpcBinaryVersionInfo& outInfo
) {
    // Look right after "MPC" for zero/whitespace/punctuation separators (ELF .rodata NUL-separated or spaced strings)
    size_t i = mpcPos + 3;
    size_t sepLimit = std::min(len, i + 24);
    while (i < sepLimit && (data[i] == 0 || data[i] == ' ' || data[i] == '\t' || data[i] == ':' || data[i] == '-')) {
        ++i;
    }
    if (i >= len) return false;
    if (data[i] == 'v' || data[i] == 'V') {
        ++i;
    }
    if (i >= len || !std::isdigit(data[i])) return false;

    auto readInt = [&](int& val, size_t& cursor) -> bool {
        if (cursor >= len || !std::isdigit(data[cursor])) return false;
        long acc = 0;
        int digits = 0;
        while (cursor < len && std::isdigit(data[cursor]) && digits < 5) {
            acc = acc * 10 + (data[cursor] - '0');
            ++cursor;
            ++digits;
        }
        if (digits == 0) return false;
        val = static_cast<int>(acc);
        return true;
    };

    size_t cur = i;
    int maj = 0, min = 0, minIdx = 0, pat = 0;
    bool hasY = false, hasZ = false;
    if (!readInt(maj, cur)) return false;
    if (cur >= len || data[cur] != '.') return false;
    ++cur;
    if (!readInt(min, cur)) return false;

    if (cur < len && data[cur] == '.') {
        size_t save = cur;
        ++cur;
        if (readInt(minIdx, cur)) {
            hasY = true;
            if (cur < len && data[cur] == '.') {
                size_t save2 = cur;
                ++cur;
                if (readInt(pat, cur)) {
                    hasZ = true;
                } else {
                    cur = save2;
                }
            }
        } else {
            cur = save;
        }
    }

    // Must end cleanly (NUL, whitespace, or "(xxx)") and not continue with letters/dots/slashes
    if (cur < len) {
        unsigned char nxt = data[cur];
        if (nxt != 0 && nxt != ' ' && nxt != '\t' && nxt != '\r' && nxt != '\n' && nxt != '(') {
            return false;
        }
    }

    // Sanity check plausible MPC OS major version (1..99)
    if (maj < 1 || maj > 99) return false;

    std::string verNum(reinterpret_cast<const char*>(data + i), cur - i);
    std::string fullVer = verNum;

    // Capture optional " (xxx)" immediately following V.X[.Y[.Z]]
    size_t tagCur = cur;
    while (tagCur < len && (data[tagCur] == ' ' || data[tagCur] == '\t')) ++tagCur;
    if (tagCur < len && data[tagCur] == '(') {
        size_t closeParen = tagCur + 1;
        size_t limitParen = std::min(len, tagCur + 64);
        while (closeParen < limitParen && data[closeParen] != ')' && data[closeParen] >= 0x20 && data[closeParen] <= 0x7E) {
            ++closeParen;
        }
        if (closeParen < limitParen && data[closeParen] == ')' && closeParen > tagCur + 1) {
            std::string tagStr(reinterpret_cast<const char*>(data + tagCur), (closeParen + 1) - tagCur);
            fullVer = verNum + " " + tagStr;
        }
    }

    outInfo.found            = true;
    outInfo.major            = maj;
    outInfo.minor            = min;
    outInfo.minorIndex       = minIdx;
    outInfo.patch            = pat;
    outInfo.hasMinorIndex    = hasY;
    outInfo.hasPatch         = hasZ;
    outInfo.versionNumberStr = verNum;
    outInfo.fullVersionStr   = fullVer;
    outInfo.isNativeVst2     = (maj > 3) || (maj == 3 && min >= 10);
    return true;
}

MpcBinaryVersionInfo ConfigParser::detectMpcBinaryVersion(bool forceRefresh) {
    if (s_mpcBinVersionCached && !forceRefresh) {
        return s_mpcBinVersionInfo;
    }

    MpcBinaryVersionInfo info;
    const char* envBin = std::getenv("TKGL_MPC_BIN");
    std::string binPath = (envBin && envBin[0] != '\0') ? envBin : "/usr/bin/MPC";
    info.binaryPath = binPath;

    FILE* fp = std::fopen(binPath.c_str(), "rb");
    if (fp) {
        constexpr size_t CHUNK_SIZE = 262144;
        constexpr size_t OVERLAP    = 128;
        std::vector<unsigned char> buf(CHUNK_SIZE + OVERLAP);
        size_t carry = 0;

        bool foundBestWithParen = false;
        while (!foundBestWithParen) {
            size_t r = std::fread(buf.data() + carry, 1, CHUNK_SIZE, fp);
            if (r == 0) break;
            size_t total = carry + r;

            for (size_t pos = 0; pos + 6 < total; ++pos) {
                if (buf[pos] == 'M' && buf[pos + 1] == 'P' && buf[pos + 2] == 'C') {
                    // Require word boundary or NUL before "MPC"
                    if (pos > 0 && std::isalnum(buf[pos - 1])) continue;
                    MpcBinaryVersionInfo cand;
                    cand.binaryPath = binPath;
                    if (tryParseMpcVersionAt(buf.data(), total, pos, cand)) {
                        bool hasParen = (cand.fullVersionStr.find('(') != std::string::npos);
                        if (hasParen) {
                            info = cand;
                            foundBestWithParen = true;
                            break;
                        }
                        if (!info.found) {
                            info = cand;
                        }
                    }
                }
            }

            if (r < CHUNK_SIZE) break;
            carry = std::min(OVERLAP, total);
            std::memmove(buf.data(), buf.data() + (total - carry), carry);
        }
        std::fclose(fp);
    }

    s_mpcBinVersionInfo   = info;
    s_mpcBinVersionCached = true;

    if (info.found) {
        std::string msg = "[TKGL-MPC] Detected " + info.binaryPath + " version: " + info.fullVersionStr +
            " -> " + (info.isNativeVst2 ? ">= 3.10 Native VST2 (MPC.settings sync disabled)"
                                        : "< 3.10 Legacy mode (MPC.settings sync enabled)");
        logMessage(msg);
    } else {
        logMessage("[TKGL-MPC] WARNING: Could not detect MPC version in " + info.binaryPath +
                   " (defaulting to legacy < 3.10 MPC.settings handling).");
    }

    return info;
}

bool ConfigParser::isMpcNativeVst2Supported() {
    MpcBinaryVersionInfo info = detectMpcBinaryVersion(false);
    return info.found && info.isNativeVst2;
}

void ConfigParser::initSession() {
    unlink(SESSION_STAGED_MPC_SETTINGS);
    s_mpcSettingsDirty = false;
    enforceMpcSettingsBackupRetention(10);
    detectMpcBinaryVersion(true);

    // Initialize /tmp/MPC.settings.new working copy so Sanity Checks & Rollback operate safely in RAM
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

static std::string unescapeProcMountsField(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() &&
            s[i + 1] >= '0' && s[i + 1] <= '7' &&
            s[i + 2] >= '0' && s[i + 2] <= '7' &&
            s[i + 3] >= '0' && s[i + 3] <= '7') {
            int val = ((s[i + 1] - '0') << 6) | ((s[i + 2] - '0') << 3) | (s[i + 3] - '0');
            out.push_back(static_cast<char>(val));
            i += 3;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static bool isHotplugBlockDevice(const std::string& rawDev) {
    if (rawDev.empty() || rawDev[0] != '/') return false;
    std::string dev = rawDev;
    char resolvedDev[1024];
    if (realpath(rawDev.c_str(), resolvedDev) != nullptr) {
        dev = resolvedDev;
    }
    if (dev.rfind("/dev/", 0) != 0) return false;
    std::string name = dev.substr(5);
    if (name.empty() || name.find('/') != std::string::npos) return false;

    if (name == "root" ||
        name.rfind("loop", 0) == 0 ||
        name.rfind("ram", 0) == 0 ||
        name.rfind("zram", 0) == 0 ||
        name.rfind("ubi", 0) == 0 ||
        name.rfind("mtd", 0) == 0 ||
        name.rfind("dm-", 0) == 0 ||
        name.rfind("nbd", 0) == 0) {
        return false;
    }

    std::string baseDev = name;
    if (baseDev.rfind("mmcblk", 0) == 0 || baseDev.rfind("nvme", 0) == 0) {
        size_t pPos = baseDev.find_last_of('p');
        if (pPos != std::string::npos && pPos + 1 < baseDev.size()) {
            bool allDigits = true;
            for (size_t i = pPos + 1; i < baseDev.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(baseDev[i]))) {
                    allDigits = false;
                    break;
                }
            }
            if (allDigits) baseDev = baseDev.substr(0, pPos);
        }
    } else {
        while (!baseDev.empty() && std::isdigit(static_cast<unsigned char>(baseDev.back()))) {
            baseDev.pop_back();
        }
    }
    if (baseDev.empty() || baseDev == "mmcblk0") return false;

    struct stat stSys;
    if (stat("/sys/block", &stSys) != 0 || !S_ISDIR(stSys.st_mode)) {
        return (baseDev.rfind("sd", 0) == 0) ||
               (baseDev.rfind("mmcblk", 0) == 0 && baseDev != "mmcblk0");
    }

    std::string sysBlock = "/sys/block/" + baseDev;
    struct stat stBlk;
    if (stat(sysBlock.c_str(), &stBlk) != 0) return false;

    bool isRemovable = false;
    {
        std::ifstream remFile(sysBlock + "/removable");
        std::string remVal;
        if (remFile.is_open() && std::getline(remFile, remVal)) {
            if (!remVal.empty() && remVal[0] == '1') isRemovable = true;
        }
    }

    bool isUsbBus = false;
    char sysReal[1024];
    if (realpath(sysBlock.c_str(), sysReal) != nullptr) {
        std::string sysPath(sysReal);
        if (sysPath.find("/usb") != std::string::npos) {
            isUsbBus = true;
        }
    }

    if (baseDev.rfind("mmcblk", 0) == 0) {
        std::ifstream typeFile(sysBlock + "/device/type");
        std::string cardType;
        if (typeFile.is_open() && std::getline(typeFile, cardType)) {
            while (!cardType.empty() && std::isspace(static_cast<unsigned char>(cardType.back()))) cardType.pop_back();
            std::string upperType;
            for (char c : cardType) upperType += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            if (upperType == "MMC" || upperType == "EMMC") return false; // Internal soldered eMMC
            if (upperType.rfind("SD", 0) == 0) return true; // External hotplug SD/SDHC/SDXC card
        }
        return isRemovable;
    }

    if (baseDev.rfind("sd", 0) == 0) {
        return isUsbBus || isRemovable;
    }

    return isRemovable || isUsbBus;
}

static bool isVirtualOrInternalFsType(const std::string& fsType) {
    return fsType == "tmpfs" || fsType == "devtmpfs" || fsType == "proc" ||
           fsType == "sysfs" || fsType == "squashfs" || fsType == "overlay" ||
           fsType == "ramfs" || fsType == "ubifs" || fsType == "jffs2" ||
           fsType == "cramfs" || fsType == "nfs" || fsType == "nfs4" ||
           fsType == "cifs" || fsType == "fuse.sshfs" || fsType == "autofs" ||
           fsType == "rootfs";
}

static bool mountOptsHasReadOnly(const std::string& opts) {
    return opts == "ro" || opts.rfind("ro,", 0) == 0 ||
           opts.find(",ro,") != std::string::npos ||
           (opts.size() >= 3 && opts.compare(opts.size() - 3, 3, ",ro") == 0);
}

static std::vector<std::string> getExternalHotplugMounts() {
    std::vector<std::string> order;
    struct MountEntryInfo { std::string dev; std::string fsType; std::string opts; };
    std::unordered_map<std::string, MountEntryInfo> activeByMount;

    std::ifstream pm("/proc/mounts");
    if (pm.is_open()) {
        std::string line;
        while (std::getline(pm, line)) {
            std::istringstream iss(line);
            std::string dev, mpRaw, fsType, opts;
            if (!(iss >> dev >> mpRaw >> fsType >> opts)) continue;
            std::string mp = unescapeProcMountsField(mpRaw);
            while (mp.size() > 1 && mp.back() == '/') mp.pop_back();
            if (mp.rfind("/media/", 0) != 0) continue;
            std::string rel = mp.substr(7);
            if (rel.empty() || rel.find('/') != std::string::npos) continue;
            if (activeByMount.find(mp) == activeByMount.end()) {
                order.push_back(mp);
            }
            activeByMount[mp] = { dev, fsType, opts };
        }
        pm.close();
    }

    std::vector<std::string> result;
    for (const auto& mp : order) {
        if (mp == "/media/az01-internal" || mp.rfind("/media/az01-internal", 0) == 0) continue;
        const auto& info = activeByMount[mp];
        if (mountOptsHasReadOnly(info.opts)) continue;
        if (isVirtualOrInternalFsType(info.fsType)) continue;
        if (!isHotplugBlockDevice(info.dev)) continue;
        if (!ConfigParser::isSynthDirectoryWritable(mp)) continue;
        result.push_back(mp);
    }
    return result;
}

static bool isExternalHotplugMountRoot(const std::string& rawMountRoot) {
    std::string mp = rawMountRoot;
    while (mp.size() > 1 && mp.back() == '/') mp.pop_back();
    if (mp.empty()) return false;
    auto hotplugs = getExternalHotplugMounts();
    return std::find(hotplugs.begin(), hotplugs.end(), mp) != hotplugs.end();
}

std::string ConfigParser::getManagerMountRoot() {
    std::string mgrDir = getManagerRootDir();
    while (mgrDir.size() > 1 && mgrDir.back() == '/') mgrDir.pop_back();
    if (mgrDir.empty() || mgrDir[0] != '/') return "";

    std::string bestMount;
    std::ifstream pm("/proc/mounts");
    if (pm.is_open()) {
        std::string line;
        while (std::getline(pm, line)) {
            std::istringstream iss(line);
            std::string dev, mpRaw, fsType, opts;
            if (!(iss >> dev >> mpRaw >> fsType >> opts)) continue;
            std::string mp = unescapeProcMountsField(mpRaw);
            while (mp.size() > 1 && mp.back() == '/') mp.pop_back();
            if (mp.empty() || mp == "/") continue;
            if (mgrDir == mp || mgrDir.rfind(mp + "/", 0) == 0) {
                if (mp.size() > bestMount.size()) {
                    bestMount = mp;
                }
            }
        }
        pm.close();
    }

    if (!bestMount.empty()) {
        if (isExternalHotplugMountRoot(bestMount)) {
            return bestMount;
        }
        return "";
    }

    if (mgrDir.rfind("/media/", 0) == 0) {
        size_t thirdSlash = mgrDir.find('/', 7);
        std::string driveRoot = (thirdSlash != std::string::npos) ? mgrDir.substr(0, thirdSlash) : mgrDir;
        if (isExternalHotplugMountRoot(driveRoot)) {
            return driveRoot;
        }
    }
    return "";
}

std::string ConfigParser::getManagerVst2Dir() {
    std::string mgrDir = getManagerRootDir();
    while (mgrDir.size() > 1 && mgrDir.back() == '/') mgrDir.pop_back();

    // Since MPC OS 3.10, MPC natively scans "<MountPoint>/VST2 Plugins" at the root of writable external hotplug mounts
    if (isMpcNativeVst2Supported()) {
        std::string mountRoot = getManagerMountRoot();
        if (!mountRoot.empty()) {
            return mountRoot + "/VST2 Plugins";
        }
        auto hotplugs = getExternalHotplugMounts();
        if (!hotplugs.empty()) {
            return hotplugs[0] + "/VST2 Plugins";
        }
    }
    return mgrDir + "/VST2 Plugins";
}

void ConfigParser::ensureManagerSynthsInMpcSettings(std::vector<std::string>* outLogs) {
    std::string mgrDir = getManagerRootDir();
    while (mgrDir.size() > 1 && mgrDir.back() == '/') mgrDir.pop_back();

    // Starting with MPC OS 3.10, VST2 plugins are natively scanned at the root of external hotplug mount points ("<Mount>/VST2 Plugins")
    // and MPC.settings must NOT be modified automatically.
    if (isMpcNativeVst2Supported()) {
        std::string mountRoot = getManagerMountRoot();
        std::string vst2Dir = getManagerVst2Dir();
        if (!mountRoot.empty() && isExternalHotplugMountRoot(mountRoot)) {
            mkdir(vst2Dir.c_str(), 0755);
        }
        static bool s_loggedNativeSkip = false;
        if (!s_loggedNativeSkip) {
            s_loggedNativeSkip = true;
            std::string skipMsg = "[TKGL-INIT] MPC OS >= 3.10 native VST2 active: manager hotplug VST2 root is '" +
                vst2Dir + "' (MPC.settings untouched).";
            ConfigParser::logMessage(skipMsg);
            if (outLogs) outLogs->push_back(skipMsg);
        }
        return;
    }

    // Legacy (< 3.10): Ensure Manager's subfolder "VST2 Plugins" exists ONLY if running on an external hotplug volume
    if (!getManagerMountRoot().empty()) {
        std::string defaultMgrSynths = mgrDir + "/VST2 Plugins";
        mkdir(defaultMgrSynths.c_str(), 0755);
    }

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

    // 1. Manager's own "VST2 Plugins" directory when running from an external hotplug drive
    if (!getManagerMountRoot().empty()) {
        std::string defaultMgrSynths = mgrDir + "/VST2 Plugins";
        mkdir(defaultMgrSynths.c_str(), 0755);
        dirsToEnsure.push_back(defaultMgrSynths);
    }

    // 2. Any existing "<HotplugMount>/VST2 Plugins" directory across external hotplug drives (USB / SD card)
    auto hotplugMounts = getExternalHotplugMounts();
    for (const auto& mp : hotplugMounts) {
        std::string driveVst2 = mp + "/VST2 Plugins";
        struct stat st;
        if (stat(driveVst2.c_str(), &st) == 0 && S_ISDIR(st.st_mode) && isSynthDirectoryWritable(driveVst2)) {
            if (std::find(dirsToEnsure.begin(), dirsToEnsure.end(), driveVst2) == dirsToEnsure.end()) {
                dirsToEnsure.push_back(driveVst2);
            }
        }
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

static bool isVst2PluginsPath(const std::string& rawPath) {
    std::string p = rawPath;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    if (p.empty()) return false;
    if (p == "VST2 Plugins") return true;
    const std::string suffix = "/VST2 Plugins";
    if (p.size() >= suffix.size() && p.compare(p.size() - suffix.size(), suffix.size(), suffix) == 0) {
        return true;
    }
    if (p.find("/VST2 Plugins/") != std::string::npos) return true;
    return false;
}

static bool isMountRootVst2Path(const std::string& rawPath) {
    std::string p = rawPath;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    if (p.rfind("/media/", 0) == 0) {
        std::string rest = p.substr(7); // <Drive>/VST2 Plugins
        size_t sl = rest.find('/');
        if (sl != std::string::npos && rest.substr(sl) == "/VST2 Plugins") {
            std::string drive = rest.substr(0, sl);
            if (!drive.empty() && drive.rfind("az01-internal", 0) != 0) {
                return isExternalHotplugMountRoot("/media/" + drive);
            }
        }
    }
    return false;
}

static void createPreEditSafetyBackup(std::vector<std::string>* outLogs = nullptr) {
    std::string mpcSettingsPath = ConfigParser::resolveMpcSettingsPath();
    std::ifstream src(mpcSettingsPath, std::ios::binary);
    if (!src.is_open()) return;
    std::stringstream ss;
    ss << src.rdbuf();
    src.close();
    std::string curXml = ss.str();
    std::string valErr;
    if (curXml.empty() || !validateMpcSettingsXml(curXml, valErr)) return;

    enforceMpcSettingsBackupRetention(9, outLogs);
    std::string bakDir = resolveBackupDirectory();
    std::string bakPath = bakDir + "/MPC.settings.bak-" + getBackupTimestampYYYYMMDDHHMMSS();
    std::ofstream bak(bakPath, std::ios::binary | std::ios::trunc);
    if (!bak.is_open()) return;
    bak.write(curXml.data(), static_cast<std::streamsize>(curXml.size()));
    bak.flush();
    bool ok = !bak.fail();
    bak.close();
    struct stat stBak;
    if (ok && stat(bakPath.c_str(), &stBak) == 0 && static_cast<size_t>(stBak.st_size) == curXml.size()) {
        std::string msg = "[TKGL-SANITY] Pre-purge safety backup created: " + bakPath;
        ConfigParser::logMessage(msg);
        if (outLogs) outLogs->push_back(msg);
    } else {
        unlink(bakPath.c_str());
    }
}

MpcV310StartupWarnings ConfigParser::checkMpcV310StartupWarnings() {
    MpcV310StartupWarnings warn;
    if (!isMpcNativeVst2Supported()) {
        return warn;
    }

    std::string mpcSettingsContent = loadSessionMpcSettingsXml();
    if (mpcSettingsContent.empty()) {
        return warn;
    }

    // 1. Check <SynthContentLocations> for any <Location> pointing to VST2 Plugins folders
    size_t valStart = 0, valOpenEnd = 0, valEnd = 0;
    bool valSelf = false;
    if (findValueBlock(mpcSettingsContent, "SynthContentLocations", valStart, valOpenEnd, valEnd, valSelf) && !valSelf) {
        std::string sub = mpcSettingsContent.substr(valOpenEnd, valEnd - valOpenEnd);
        size_t tag = 0;
        while ((tag = sub.find("<Location>", tag)) != std::string::npos) {
            size_t closeTag = sub.find("</Location>", tag);
            if (closeTag == std::string::npos) break;
            std::string locPath = xmlUnescape(sub.substr(tag + 10, closeTag - (tag + 10)));
            tag = closeTag + 11;
            if (isVst2PluginsPath(locPath)) {
                warn.hasSynthVst2Locations = true;
                warn.synthVst2Locations.push_back(locPath);
            }
        }
    }

    // 2. Check <VALUE name="pluginList-arm"><KNOWNPLUGINS> for declared VST2 plugins
    size_t armStart = 0, armOpenEnd = 0, armEnd = 0;
    bool armSelf = false;
    if (findValueBlock(mpcSettingsContent, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelf) && !armSelf) {
        auto entries = extractPluginXmlEntries(mpcSettingsContent, armOpenEnd, armEnd);
        if (!entries.empty()) {
            warn.hasLegacyKnownPlugins = true;
            warn.knownPluginsCount     = static_cast<int>(entries.size());
        }
    }

    if (warn.needsAttention()) {
        if (warn.hasSynthVst2Locations) {
            logMessage("[TKGL-WARN] MPC >= 3.10: MPC.settings <SynthContentLocations> still contains " +
                       std::to_string(warn.synthVst2Locations.size()) + " VST2 Plugins entry(ies). Open MPC Settings -> Sanity Checks.");
        }
        if (warn.hasLegacyKnownPlugins) {
            logMessage("[TKGL-WARN] MPC >= 3.10: MPC.settings <KNOWNPLUGINS> still declares " +
                       std::to_string(warn.knownPluginsCount) + " VST2 plugin(s). Open MPC Settings -> Sanity Checks.");
        }
    }

    return warn;
}

bool ConfigParser::purgeKnownPluginsInMpcSettings(std::string& outStatusMsg, std::vector<std::string>* outLogs) {
    std::string settXml = loadSessionMpcSettingsXml();
    if (settXml.empty()) {
        outStatusMsg = "MPC.settings is empty or unreadable.";
        return false;
    }
    std::string valErr;
    if (!validateMpcSettingsXml(settXml, valErr)) {
        outStatusMsg = "MPC.settings XML error: " + valErr;
        return false;
    }

    size_t armStart = 0, armOpenEnd = 0, armEnd = 0;
    bool armSelf = false;
    int purgedCount = 0;
    while (findValueBlock(settXml, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelf) && !armSelf) {
        auto entries = extractPluginXmlEntries(settXml, armOpenEnd, armEnd);
        if (entries.empty()) break;
        createPreEditSafetyBackup(outLogs);
        for (int idx = static_cast<int>(entries.size()) - 1; idx >= 0; --idx) {
            eraseXmlElementLineCleanly(settXml, entries[idx].startPos, entries[idx].endPos);
            purgedCount++;
        }
        break;
    }
    while (purgedCount > 0 && findValueBlock(settXml, "pluginList-arm", armStart, armOpenEnd, armEnd, armSelf) && !armSelf) {
        auto entries = extractPluginXmlEntries(settXml, armOpenEnd, armEnd);
        if (entries.empty()) break;
        for (int idx = static_cast<int>(entries.size()) - 1; idx >= 0; --idx) {
            eraseXmlElementLineCleanly(settXml, entries[idx].startPos, entries[idx].endPos);
            purgedCount++;
        }
    }

    if (purgedCount == 0) {
        outStatusMsg = "No KNOWNPLUGINS entries found to purge in MPC.settings.";
        return true;
    }

    if (!stageSessionMpcSettingsXml(settXml, outLogs)) {
        outStatusMsg = "Failed to stage purged KNOWNPLUGINS in /tmp/MPC.settings.new.";
        return false;
    }

    outStatusMsg = "Purged " + std::to_string(purgedCount) + " legacy KNOWNPLUGINS entry(ies) (safety backup created, staged for exit commit).";
    std::printf("[TKGL-SANITY] %s\n", outStatusMsg.c_str());
    logMessage("[TKGL-SANITY] " + outStatusMsg);
    if (outLogs) outLogs->push_back("[TKGL-SANITY] " + outStatusMsg);
    return true;
}

bool ConfigParser::purgeVst2LocationsInSynthContentLocations(std::string& outStatusMsg, std::vector<std::string>* outLogs) {
    std::string settXml = loadSessionMpcSettingsXml();
    if (settXml.empty()) {
        outStatusMsg = "MPC.settings is empty or unreadable.";
        return false;
    }
    std::string valErr;
    if (!validateMpcSettingsXml(settXml, valErr)) {
        outStatusMsg = "MPC.settings XML error: " + valErr;
        return false;
    }

    int purgedCount = 0;
    bool backupCreated = false;
    while (true) {
        size_t valStart = 0, valOpenEnd = 0, valEnd = 0;
        bool valSelf = false;
        if (!findValueBlock(settXml, "SynthContentLocations", valStart, valOpenEnd, valEnd, valSelf) || valSelf) {
            break;
        }
        bool erasedOne = false;
        size_t scanPos = valOpenEnd;
        while (scanPos < valEnd) {
            size_t locOpen = settXml.find("<Location>", scanPos);
            if (locOpen == std::string::npos || locOpen >= valEnd) break;
            size_t locClose = settXml.find("</Location>", locOpen);
            if (locClose == std::string::npos || locClose >= valEnd) break;
            std::string locPath = xmlUnescape(settXml.substr(locOpen + 10, locClose - (locOpen + 10)));
            if (isVst2PluginsPath(locPath)) {
                if (!backupCreated) {
                    createPreEditSafetyBackup(outLogs);
                    backupCreated = true;
                }
                eraseXmlElementLineCleanly(settXml, locOpen, locClose + 11);
                purgedCount++;
                erasedOne = true;
                break;
            }
            scanPos = locClose + 11;
        }
        if (!erasedOne) break;
    }

    if (purgedCount == 0) {
        outStatusMsg = "No VST2 Plugins entries found to purge in SynthContentLocations.";
        return true;
    }

    if (!stageSessionMpcSettingsXml(settXml, outLogs)) {
        outStatusMsg = "Failed to stage purged SynthContentLocations in /tmp/MPC.settings.new.";
        return false;
    }

    outStatusMsg = "Purged " + std::to_string(purgedCount) + " VST2 Plugins <Location> entry(ies) from SynthContentLocations (backup created).";
    std::printf("[TKGL-SANITY] %s\n", outStatusMsg.c_str());
    logMessage("[TKGL-SANITY] " + outStatusMsg);
    if (outLogs) outLogs->push_back("[TKGL-SANITY] " + outStatusMsg);
    return true;
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

    if (isMpcNativeVst2Supported()) {
        // Check 1 (>= 3.10): Legacy KNOWNPLUGINS entries still present in MPC.settings -> offer to purge
        if (!regPlugins.empty()) {
            SanityCheckIssue iss;
            iss.type       = SanityCheckIssue::LEGACY_KNOWNPLUGINS;
            iss.pluginName = "Legacy KNOWNPLUGINS (" + std::to_string(regPlugins.size()) + " entries in MPC.settings)";
            iss.dirPath    = "MPC.settings -> <VALUE name=\"pluginList-arm\"><KNOWNPLUGINS>";
            iss.synthRoot  = std::to_string(regPlugins.size());
            issues.push_back(iss);
        }

        // Check 2 (>= 3.10): Collect SynthContentLocations (+ /media/*/Synths + legacy subfolder <ManagerDir>/VST2 Plugins)
        // that are NOT a mount-point root "<Mount>/VST2 Plugins" scanned natively by MPC >= 3.10
        std::vector<std::string> nonVst2Roots;
        std::vector<std::string> vst2SynthLocs;
        auto addNonVst2Root = [&](const std::string& rawPath) {
            std::string p = rawPath;
            while (p.size() > 1 && p.back() == '/') p.pop_back();
            if (p.empty() || p[0] != '/') return;
            if (isMountRootVst2Path(p)) return;
            if (!isSynthDirectoryWritable(p)) return;
            if (std::find(nonVst2Roots.begin(), nonVst2Roots.end(), p) == nonVst2Roots.end()) {
                nonVst2Roots.push_back(p);
            }
        };

        if (!mpcSettingsContent.empty()) {
            size_t valStart = 0, valOpenEnd = 0, valEnd = 0;
            bool valSelfClosing = false;
            if (findValueBlock(mpcSettingsContent, "SynthContentLocations", valStart, valOpenEnd, valEnd, valSelfClosing) && !valSelfClosing) {
                std::string sub = mpcSettingsContent.substr(valOpenEnd, valEnd - valOpenEnd);
                size_t tag = 0;
                while ((tag = sub.find("<Location>", tag)) != std::string::npos) {
                    size_t closeTag = sub.find("</Location>", tag);
                    if (closeTag == std::string::npos) break;
                    std::string locPath = xmlUnescape(sub.substr(tag + 10, closeTag - (tag + 10)));
                    tag = closeTag + 11;
                    if (isVst2PluginsPath(locPath)) {
                        vst2SynthLocs.push_back(locPath);
                    }
                    addNonVst2Root(locPath);
                }
            }
        }

        // Also check legacy subfolder <ManagerRootDir>/VST2 Plugins if distinct from mount-root <MountPoint>/VST2 Plugins
        std::string legacyMgrSubVst2 = getManagerRootDir() + "/VST2 Plugins";
        while (legacyMgrSubVst2.size() > 1 && legacyMgrSubVst2.back() == '/') legacyMgrSubVst2.pop_back();
        if (legacyMgrSubVst2 != getManagerVst2Dir()) {
            struct stat stLegacySub;
            if (stat(legacyMgrSubVst2.c_str(), &stLegacySub) == 0 && S_ISDIR(stLegacySub.st_mode)) {
                addNonVst2Root(legacyMgrSubVst2);
            }
        }

        for (const auto& mp : getExternalHotplugMounts()) {
            addNonVst2Root(mp + "/Synths");
        }

        for (const auto& locRoot : nonVst2Roots) {
            DIR* d = opendir(locRoot.c_str());
            if (!d) continue;
            struct dirent* de;
            while ((de = readdir(d)) != nullptr) {
                if (de->d_name[0] == '.') continue;
                std::string subName = de->d_name;
                std::string fullSubDir = locRoot + "/" + subName;
                struct stat stDir;
                if (stat(fullSubDir.c_str(), &stDir) != 0 || !S_ISDIR(stDir.st_mode)) continue;

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

                SanityCheckIssue iss;
                iss.type       = SanityCheckIssue::OUTSIDE_VST2_LOCATION;
                iss.pluginName = subName;
                iss.dirPath    = fullSubDir;
                iss.soPath     = foundSoInDir;
                iss.synthRoot  = locRoot;
                issues.push_back(iss);
            }
            closedir(d);
        }

        // Check 3 (>= 3.10, Sequenced): Once KNOWNPLUGINS and outside-VST2 relocations are resolved (0 remaining),
        // propose purging any obsolete VST2 Plugins <Location> entries inside <SynthContentLocations>
        if (issues.empty() && !vst2SynthLocs.empty()) {
            SanityCheckIssue iss;
            iss.type       = SanityCheckIssue::LEGACY_SYNTH_VST2_LOCATIONS;
            iss.pluginName = "SynthContentLocations: " + std::to_string(vst2SynthLocs.size()) + " VST2 Plugins entry(ies)";
            std::string joined = vst2SynthLocs[0];
            if (vst2SynthLocs.size() > 1) {
                joined += " (+ " + std::to_string(vst2SynthLocs.size() - 1) + " more)";
            }
            iss.dirPath    = joined;
            iss.synthRoot  = std::to_string(vst2SynthLocs.size());
            issues.push_back(iss);
        }

        std::string msg = "[TKGL-SANITY] (>= 3.10 Native VST2 scan) complete: " + std::to_string(issues.size()) + " issue(s) found.";
        logMessage(msg);
        if (outLogs) outLogs->push_back(msg);
        return issues;
    }

    // Legacy (< 3.10) Sanity Check 1: Scan all writable (non-RO) SynthContentLocations for orphan synth folders
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

    // Legacy (< 3.10) Sanity Check 2: Scan all <PLUGIN .../> entries in MPC.settings KNOWNPLUGINS for missing .so files
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

static bool reconstitutePluginJsonIfMissing(
    const std::string& targetDir,
    const std::string& primarySoPath,
    const MenuItem* optItem,
    const std::function<void(const std::string&)>& emitLog
);

bool ConfigParser::repairSanityIssue(LauncherConfig& config, const SanityCheckIssue& issue, std::string& outStatusMsg, std::vector<std::string>* outLogs) {
    if (issue.type == SanityCheckIssue::LEGACY_KNOWNPLUGINS) {
        return purgeKnownPluginsInMpcSettings(outStatusMsg, outLogs);
    } else if (issue.type == SanityCheckIssue::LEGACY_SYNTH_VST2_LOCATIONS) {
        return purgeVst2LocationsInSynthContentLocations(outStatusMsg, outLogs);
    } else if (issue.type == SanityCheckIssue::OUTSIDE_VST2_LOCATION) {
        // Reinstall/migrate plugin found outside mount-root VST2 Plugins onto <Drive>/VST2 Plugins
        std::string subName = issue.pluginName;
        size_t sl = issue.dirPath.find_last_of('/');
        if (sl != std::string::npos && sl + 1 < issue.dirPath.size()) {
            subName = issue.dirPath.substr(sl + 1);
        }

        std::string targetVst2 = getManagerVst2Dir();
        if (issue.dirPath.rfind("/media/", 0) == 0) {
            size_t thirdSlash = issue.dirPath.find('/', 7);
            if (thirdSlash != std::string::npos) {
                std::string driveRoot = issue.dirPath.substr(0, thirdSlash);
                if (isExternalHotplugMountRoot(driveRoot)) {
                    targetVst2 = driveRoot + "/VST2 Plugins";
                }
            }
        }
        if (targetVst2.empty() || (!isExternalHotplugMountRoot(targetVst2.substr(0, targetVst2.rfind("/VST2 Plugins"))))) {
            auto hotplugs = getExternalHotplugMounts();
            if (!hotplugs.empty()) targetVst2 = hotplugs[0] + "/VST2 Plugins";
        }
        mkdir(targetVst2.c_str(), 0755);

        std::string soFileName;
        if (!issue.soPath.empty()) {
            size_t s = issue.soPath.find_last_of('/');
            soFileName = (s != std::string::npos) ? issue.soPath.substr(s + 1) : issue.soPath;
        }

        // First try clean catalog reinstall if matched & cached or online
        for (const auto& sec : config.sections) {
            for (const auto& it : sec.items) {
                std::string itSub = !it.targetSubdir.empty() ? it.targetSubdir : (it.author + " - VST - " + it.label);
                bool matches = (itSub == subName || it.label == subName || it.id == subName ||
                                (!soFileName.empty() && it.targetBinary == soFileName));
                if (matches && (isItemBinaryCached(it) || checkInternetReachable(config.ping_host.empty() ? "github.com" : config.ping_host))) {
                    std::vector<std::string> instLogs;
                    int rc = executeItemInstall(config, it, targetVst2, instLogs);
                    if (outLogs) {
                        for (const auto& l : instLogs) outLogs->push_back(l);
                    }
                    if (rc == 0) {
                        std::string newDir = targetVst2 + "/" + subName;
                        if (issue.dirPath != newDir && issue.dirPath.size() >= 8) {
                            std::string rmCmd = "rm -rf \"" + issue.dirPath + "\" && sync";
                            int rrc = std::system(rmCmd.c_str());
                            (void)rrc;
                        }
                        refreshInstallStates(config, outLogs);
                        outStatusMsg = "Reinstalled '" + subName + "' to " + targetVst2;
                        logMessage("[TKGL-SANITY] " + outStatusMsg);
                        return true;
                    }
                }
            }
        }

        // Fallback: check architecture compatibility before migrating directory to <Drive>/VST2 Plugins/<subName>
        auto sanityEmit = [&](const std::string& l) {
            std::printf("%s\n", l.c_str());
            logMessage(l);
            if (outLogs) outLogs->push_back(l);
        };
        if (!verifyDirectorySoCompatibility(issue.dirPath, sanityEmit)) {
            outStatusMsg = "Aborted: '" + subName + "' .so architecture is incompatible with current OS.";
            return false;
        }

        std::string destDir = targetVst2 + "/" + subName;
        if (destDir != issue.dirPath) {
            mkdir(destDir.c_str(), 0755);
            std::string cpCmd = "cp -rf \"" + issue.dirPath + "/.\" \"" + destDir + "/\" && sync";
            int cpRc = std::system(cpCmd.c_str());
            if (cpRc != 0) {
                outStatusMsg = "Failed to copy '" + subName + "' to " + targetVst2;
                return false;
            }
            std::string rmCmd = "rm -rf \"" + issue.dirPath + "\" && sync";
            int rmRc = std::system(rmCmd.c_str());
            (void)rmRc;
        }

        std::string newSoPath;
        if (!soFileName.empty()) {
            std::string cand = destDir + "/" + soFileName;
            struct stat st;
            if (stat(cand.c_str(), &st) == 0) newSoPath = cand;
        }
        if (newSoPath.empty()) {
            DIR* d = opendir(destDir.c_str());
            if (d) {
                struct dirent* de;
                while ((de = readdir(d)) != nullptr) {
                    size_t len = std::strlen(de->d_name);
                    if (len > 3 && std::strcmp(de->d_name + len - 3, ".so") == 0) {
                        newSoPath = destDir + "/" + de->d_name;
                        break;
                    }
                }
                closedir(d);
            }
        }
        int xplCnt = newSoPath.empty() ? 0 : updatePluginPresetsXplFiles(destDir, newSoPath, {});
        if (!newSoPath.empty()) {
            reconstitutePluginJsonIfMissing(destDir, newSoPath, nullptr, sanityEmit);
        }
        sync();
        refreshInstallStates(config, outLogs);
        outStatusMsg = "Reinstalled '" + subName + "' to " + destDir + " (" + std::to_string(xplCnt) + " presets updated).";
        std::printf("[TKGL-SANITY] %s\n", outStatusMsg.c_str());
        logMessage("[TKGL-SANITY] " + outStatusMsg);
        if (outLogs) outLogs->push_back("[TKGL-SANITY] " + outStatusMsg);
        return true;
    } else if (issue.type == SanityCheckIssue::ORPHAN_SYNTH_DIR) {
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
        reconstitutePluginJsonIfMissing(issue.dirPath, issue.soPath, nullptr, [&](const std::string& l) {
            std::printf("%s\n", l.c_str());
            logMessage(l);
            if (outLogs) outLogs->push_back(l);
        });
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
                reconstitutePluginJsonIfMissing(foundRelocatedDir, foundRelocatedSo, nullptr, [&](const std::string& l) {
                    std::printf("%s\n", l.c_str());
                    logMessage(l);
                    if (outLogs) outLogs->push_back(l);
                });
                refreshInstallStates(config, outLogs);
                outStatusMsg = "Staged relink '" + issue.pluginName + "' in /tmp/MPC.settings.new (" + std::to_string(xplCnt) + " presets updated).";
                std::printf("[TKGL-SANITY] %s\n", outStatusMsg.c_str());
                logMessage("[TKGL-SANITY] " + outStatusMsg);
                return true;
            }
        }

        // Fallback: find matching item in catalog and reinstall it into primary SynthLocation (which stages in /tmp/MPC.settings.new)
        std::string targetLoc = !synthLocs.empty() ? synthLocs[0].path : getManagerVst2Dir();
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
    if (issue.type == SanityCheckIssue::LEGACY_KNOWNPLUGINS) {
        return purgeKnownPluginsInMpcSettings(outStatusMsg, outLogs);
    } else if (issue.type == SanityCheckIssue::LEGACY_SYNTH_VST2_LOCATIONS) {
        return purgeVst2LocationsInSynthContentLocations(outStatusMsg, outLogs);
    } else if (issue.type == SanityCheckIssue::ORPHAN_SYNTH_DIR || issue.type == SanityCheckIssue::OUTSIDE_VST2_LOCATION) {
        if (issue.dirPath.empty() || issue.dirPath == "/" || issue.dirPath.size() < 8) {
            outStatusMsg = "Refusing to delete unsafe directory path.";
            return false;
        }
        std::string cmd = "rm -rf \"" + issue.dirPath + "\" && sync";
        int rc = std::system(cmd.c_str());
        (void)rc;
        refreshInstallStates(config, outLogs);
        outStatusMsg = "Deleted plugin directory: " + issue.dirPath;
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
    outPluginsCount = 0;
    int totalPresetsUpdated = 0;
    int reconstitutedJsonCount = 0;

    auto emitFixLog = [&](const std::string& l) {
        std::printf("%s\n", l.c_str());
        logMessage(l);
        if (outLogs) outLogs->push_back(l);
    };

    auto matchCatalogItemForDir = [&](const std::string& plugDir, const std::string& soPath) -> const MenuItem* {
        std::string dirName = plugDir;
        size_t sl = plugDir.find_last_of('/');
        if (sl != std::string::npos && sl + 1 < plugDir.size()) dirName = plugDir.substr(sl + 1);
        std::string soBase = soPath;
        size_t ssl = soPath.find_last_of('/');
        if (ssl != std::string::npos && ssl + 1 < soPath.size()) soBase = soPath.substr(ssl + 1);
        for (const auto& sec : config.sections) {
            for (const auto& it : sec.items) {
                std::string sub = !it.targetSubdir.empty() ? it.targetSubdir : (it.author + " - VST - " + it.label);
                if (sub == dirName || it.label == dirName || it.id == dirName ||
                    (!it.targetBinary.empty() && it.targetBinary == soBase) ||
                    (it.id + ".so" == soBase)) {
                    return &it;
                }
            }
        }
        return nullptr;
    };

    std::vector<std::string> visitedDirs;

    // 1. Check MPC.settings KNOWNPLUGINS when present (legacy < 3.10)
    std::string mpcSettingsContent = loadSessionMpcSettingsXml();
    std::vector<MpcPluginXmlEntry> regPlugins;
    if (!mpcSettingsContent.empty()) {
        size_t valStart = 0, valOpenEnd = 0, valEnd = 0;
        bool valSelfClosing = false;
        if (findValueBlock(mpcSettingsContent, "pluginList-arm", valStart, valOpenEnd, valEnd, valSelfClosing) && !valSelfClosing) {
            regPlugins = extractPluginXmlEntries(mpcSettingsContent, valOpenEnd, valEnd);
        }
    }

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
        if (reconstitutePluginJsonIfMissing(plugDir, rp.file, matchCatalogItemForDir(plugDir, rp.file), emitFixLog)) {
            reconstitutedJsonCount++;
        }
        outPluginsCount++;
        totalPresetsUpdated += count;
    }

    // 2. Also scan all writable VST2 Plugins directories directly on disk (required for >= 3.10 native VST2)
    auto synthLocs = getSynthContentLocations();
    for (const auto& loc : synthLocs) {
        if (loc.path.empty() || !isSynthDirectoryWritable(loc.path)) continue;
        DIR* d = opendir(loc.path.c_str());
        if (!d) continue;
        struct dirent* de;
        while ((de = readdir(d)) != nullptr) {
            if (de->d_name[0] == '.') continue;
            std::string plugDir = loc.path + "/" + de->d_name;
            struct stat stDir;
            if (stat(plugDir.c_str(), &stDir) != 0 || !S_ISDIR(stDir.st_mode)) continue;
            if (std::find(visitedDirs.begin(), visitedDirs.end(), plugDir) != visitedDirs.end()) continue;

            std::string foundSo;
            DIR* subD = opendir(plugDir.c_str());
            if (subD) {
                struct dirent* sde;
                while ((sde = readdir(subD)) != nullptr) {
                    size_t len = std::strlen(sde->d_name);
                    if (len > 3 && std::strcmp(sde->d_name + len - 3, ".so") == 0) {
                        foundSo = plugDir + "/" + sde->d_name;
                        break;
                    }
                }
                closedir(subD);
            }
            if (foundSo.empty()) continue;

            visitedDirs.push_back(plugDir);
            int count = updatePluginPresetsXplFiles(plugDir, foundSo, {});
            if (reconstitutePluginJsonIfMissing(plugDir, foundSo, matchCatalogItemForDir(plugDir, foundSo), emitFixLog)) {
                reconstitutedJsonCount++;
            }
            outPluginsCount++;
            totalPresetsUpdated += count;
        }
        closedir(d);
    }

    sync();
    outStatusMsg = "Updated " + std::to_string(totalPresetsUpdated) + " preset (.xpl) file(s) across " + std::to_string(outPluginsCount) + " installed plugin(s)";
    if (reconstitutedJsonCount > 0) {
        outStatusMsg += "; reconstituted " + std::to_string(reconstitutedJsonCount) + " missing plugin.json";
    }
    outStatusMsg += ".";
    emitFixLog("[TKGL-SANITY] " + outStatusMsg);
    return totalPresetsUpdated;
}

void ConfigParser::refreshInstallStates(LauncherConfig& config, std::vector<std::string>* outLogs) {
    // 0. Ensure Manager VST2 Plugins folder exists (and update MPC.settings only if OS < 3.10)
    ensureManagerSynthsInMpcSettings(outLogs);

    // 1. Discover VST2 Plugins locations strictly across mounted external hotplug drives
    std::string mpcSettingsPath = resolveMpcSettingsPath();
    std::string mgrDir = getManagerRootDir();
    std::vector<std::string> knownSynthDirs;
    auto synthLocs = getSynthContentLocations(mpcSettingsPath);
    for (const auto& sl : synthLocs) {
        if (!sl.path.empty()) knownSynthDirs.push_back(sl.path);
    }
    if (!getManagerMountRoot().empty()) {
        knownSynthDirs.push_back(getManagerVst2Dir());
    }
    for (const auto& mp : getExternalHotplugMounts()) {
        knownSynthDirs.push_back(mp + "/VST2 Plugins");
    }
    std::sort(knownSynthDirs.begin(), knownSynthDirs.end());
    knownSynthDirs.erase(std::unique(knownSynthDirs.begin(), knownSynthDirs.end()), knownSynthDirs.end());

    // 2. Native VST2 mode (MPC OS >= 3.10): detect installation directly from VST2 Plugins folders on disk
    if (isMpcNativeVst2Supported()) {
        for (auto& sec : config.sections) {
            for (auto& item : sec.items) {
                std::string subDir    = !item.targetSubdir.empty() ? item.targetSubdir : (item.author + " - VST - " + item.label);
                std::string altSubDir = item.author + " - VST - " + item.label;
                std::string binName   = !item.targetBinary.empty() ? item.targetBinary : (item.id + ".so");

                std::vector<std::string> candSubdirs = { subDir, altSubDir, item.label, item.id };
                std::string foundDir;
                std::string foundSo;

                for (const auto& root : knownSynthDirs) {
                    if (root.empty()) continue;
                    for (const auto& sd : candSubdirs) {
                        if (sd.empty()) continue;
                        std::string dirPath = root + "/" + sd;
                        struct stat stDir;
                        if (stat(dirPath.c_str(), &stDir) != 0 || !S_ISDIR(stDir.st_mode)) continue;

                        if (foundDir.empty()) foundDir = dirPath;
                        std::string prefSo = dirPath + "/" + binName;
                        struct stat stSo;
                        if (!binName.empty() && stat(prefSo.c_str(), &stSo) == 0 && S_ISREG(stSo.st_mode)) {
                            foundDir = dirPath;
                            foundSo = prefSo;
                            break;
                        }
                        DIR* dSub = opendir(dirPath.c_str());
                        if (dSub) {
                            struct dirent* sde;
                            while ((sde = readdir(dSub)) != nullptr) {
                                size_t len = std::strlen(sde->d_name);
                                if (len > 3 && std::strcmp(sde->d_name + len - 3, ".so") == 0) {
                                    foundDir = dirPath;
                                    foundSo = dirPath + "/" + sde->d_name;
                                    break;
                                }
                            }
                            closedir(dSub);
                        }
                        if (!foundSo.empty()) break;
                    }
                    if (!foundSo.empty()) break;
                }

                if (!foundSo.empty()) {
                    size_t lastSlash = foundSo.find_last_of('/');
                    item.targetBinary = foundSo.substr(lastSlash + 1);
                    item.skinFolderPath = foundDir;
                    size_t dirSlash = foundDir.find_last_of('/');
                    if (dirSlash != std::string::npos) {
                        item.targetSubdir = foundDir.substr(dirSlash + 1);
                    }
                    item.installed = 1;
                } else if (!foundDir.empty()) {
                    item.skinFolderPath = foundDir;
                    item.installed = 2;
                } else {
                    item.installed = 0;
                    if (!knownSynthDirs.empty()) {
                        item.skinFolderPath = knownSynthDirs[0] + "/" + subDir;
                    }
                }
                item.isCached = isItemBinaryCached(item, &item.cachedZipSize);
            }
        }
        return;
    }

    // 3. Legacy (< 3.10) mode: read staged or active MPC.settings <VALUE name="pluginList-arm">
    std::string mpcSettingsContent = loadSessionMpcSettingsXml();

    std::vector<MpcPluginXmlEntry> regPlugins;
    if (!mpcSettingsContent.empty()) {
        size_t valStart = 0, valOpenEnd = 0, valEnd = 0;
        bool valSelfClosing = false;
        if (findValueBlock(mpcSettingsContent, "pluginList-arm", valStart, valOpenEnd, valEnd, valSelfClosing) && !valSelfClosing) {
            regPlugins = extractPluginXmlEntries(mpcSettingsContent, valOpenEnd, valEnd);
        }
    }

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
                    item.installed = 1;
                } else {
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

    bool nativeVst2 = isMpcNativeVst2Supported();
    std::string mgrMountRoot = getManagerMountRoot();
    std::string defaultMgrSynths = getManagerVst2Dir();
    if (!mgrMountRoot.empty() && isSynthDirectoryWritable(defaultMgrSynths)) {
        mkdir(defaultMgrSynths.c_str(), 0755);
    }

    auto getFreeSpace = [](const std::string& dirPath, std::string& outStr) -> uint64_t {
        struct statvfs sv;
        std::string checkPath = dirPath;
        if (statvfs(checkPath.c_str(), &sv) != 0) {
            size_t lastSlash = checkPath.find_last_of('/');
            std::string parentPath = (lastSlash != std::string::npos && lastSlash > 0) ? checkPath.substr(0, lastSlash) : "/media";
            if (statvfs(parentPath.c_str(), &sv) != 0) {
                outStr = "0 MB free";
                return 0;
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

    auto hotplugMounts = getExternalHotplugMounts();

    auto pathOnHotplugMount = [&](const std::string& p) -> bool {
        for (const auto& mp : hotplugMounts) {
            if (p == mp || p.rfind(mp + "/", 0) == 0) return true;
        }
        return false;
    };

    auto addUniqueVst2Loc = [&](const std::string& candidatePath, bool isMgrDefault, const std::string& customLabel = "") {
        if (candidatePath.empty() || candidatePath[0] != '/') return;
        if (candidatePath == "./VST2 Plugins") return;
        if (!pathOnHotplugMount(candidatePath)) return;
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

    // 1. Plugin Manager's own VST2 Plugins folder (only when hosted on an external hotplug volume)
    if (!mgrMountRoot.empty() && isSynthDirectoryWritable(defaultMgrSynths)) {
        addUniqueVst2Loc(defaultMgrSynths, true, "Manager VST2 Plugins (" + defaultMgrSynths + ")");
    }

    // 2. Read session staged or active MPC.settings <SynthContentLocations> ONLY for active external hotplug drives
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

                const std::string synthsSuffix = "/Synths";
                if (path.size() > synthsSuffix.size() &&
                    path.compare(path.size() - synthsSuffix.size(), synthsSuffix.size(), synthsSuffix) == 0) {
                    std::string parentDrive = path.substr(0, path.size() - synthsSuffix.size());
                    if (isExternalHotplugMountRoot(parentDrive)) {
                        addUniqueVst2Loc(parentDrive + "/VST2 Plugins", false);
                    }
                    continue;
                }

                if (!nativeVst2 || isMountRootVst2Path(path)) {
                    addUniqueVst2Loc(path, false);
                }
            }
        }
    }

    // 3. Auto-discover strictly mounted external hotplug drives (USB flash/SSD, external SD card)
    //    verified via /proc/mounts + /sys/block, offering "<HotplugMount>/VST2 Plugins"
    for (const auto& mp : hotplugMounts) {
        addUniqueVst2Loc(mp + "/VST2 Plugins", false);
    }

    // Sort locations by free disk space descending (from largest to smallest)
    std::sort(locs.begin(), locs.end(), [](const SynthLocation& a, const SynthLocation& b) {
        return a.freeBytes > b.freeBytes;
    });

    return locs;
}

int ConfigParser::getElfFileBitness(const std::string& filePath, std::string* outMachineDesc) {
    FILE* f = std::fopen(filePath.c_str(), "rb");
    if (!f) {
        if (outMachineDesc) *outMachineDesc = "unreadable";
        return 0;
    }
    unsigned char hdr[20];
    size_t n = std::fread(hdr, 1, sizeof(hdr), f);
    std::fclose(f);
    if (n < 20) {
        if (outMachineDesc) *outMachineDesc = "truncated";
        return 0;
    }
    if (hdr[0] != 0x7F || hdr[1] != 'E' || hdr[2] != 'L' || hdr[3] != 'F') {
        if (outMachineDesc) *outMachineDesc = "not-ELF";
        return 0;
    }

    unsigned char eiClass = hdr[4];
    unsigned char eiData  = hdr[5];
    uint16_t eMachine = 0;
    if (eiData == 2) {
        eMachine = static_cast<uint16_t>((static_cast<uint16_t>(hdr[18]) << 8) | hdr[19]);
    } else {
        eMachine = static_cast<uint16_t>(hdr[18] | (static_cast<uint16_t>(hdr[19]) << 8));
    }

    std::string machStr;
    switch (eMachine) {
        case 40:  machStr = "ARM32"; break;
        case 183: machStr = "AArch64"; break;
        case 3:   machStr = "x86"; break;
        case 62:  machStr = "x86_64"; break;
        default: {
            char mbuf[32];
            std::snprintf(mbuf, sizeof(mbuf), "machine=0x%04X", static_cast<unsigned>(eMachine));
            machStr = mbuf;
            break;
        }
    }

    if (eiClass == 1) {
        if (outMachineDesc) *outMachineDesc = "ELF32 " + machStr;
        return 32;
    }
    if (eiClass == 2) {
        if (outMachineDesc) *outMachineDesc = "ELF64 " + machStr;
        return 64;
    }
    if (outMachineDesc) *outMachineDesc = "unknown-ELF-class";
    return 0;
}

int ConfigParser::getSystemElfBitness(std::string* outDesc) {
    std::string mpcDesc;
    int mpcBits = getElfFileBitness("/usr/bin/MPC", &mpcDesc);
    if (mpcBits == 32 || mpcBits == 64) {
        if (outDesc) *outDesc = "/usr/bin/MPC " + mpcDesc;
        return mpcBits;
    }

    std::string selfDesc;
    int selfBits = getElfFileBitness("/proc/self/exe", &selfDesc);
    if (selfBits == 32 || selfBits == 64) {
        if (outDesc) *outDesc = "/proc/self/exe " + selfDesc;
        return selfBits;
    }

    struct utsname uts;
    std::string mach = "unknown";
    if (uname(&uts) == 0) {
        mach = uts.machine;
    }
    int ptrBits = static_cast<int>(sizeof(void*) * 8);
    if (outDesc) *outDesc = mach + " ABI " + std::to_string(ptrBits) + "-bit";
    return ptrBits;
}

static std::string normalizePluginUidHex(
    const std::string& rawUid,
    const std::string& fallbackSeed
) {
    std::string trimmed = rawUid;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();

    if (!trimmed.empty()) {
        std::string hexPart = trimmed;
        if (hexPart.rfind("0x", 0) == 0 || hexPart.rfind("0X", 0) == 0) {
            hexPart = hexPart.substr(2);
        }
        bool allHex = !hexPart.empty() && hexPart.size() <= 8;
        for (char c : hexPart) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) {
                allHex = false;
                break;
            }
        }
        if (allHex) {
            std::string lowerHex;
            while (lowerHex.size() + hexPart.size() < 8) lowerHex += '0';
            for (char c : hexPart) {
                lowerHex += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            return "0x" + lowerHex;
        }
    }

    uint32_t h = 2166136261u;
    for (unsigned char c : fallbackSeed) {
        h ^= static_cast<uint32_t>(c);
        h *= 16777619u;
    }
    char ubuf[16];
    std::snprintf(ubuf, sizeof(ubuf), "0x%08x", static_cast<unsigned>(h));
    return std::string(ubuf);
}

static bool reconstitutePluginJsonIfMissing(
    const std::string& targetDir,
    const std::string& primarySoPath,
    const MenuItem* optItem,
    const std::function<void(const std::string&)>& emitLog
) {
    if (targetDir.empty()) return false;

    std::string pluginJsonPath = targetDir + "/plugin.json";
    struct stat stJson;
    if (stat(pluginJsonPath.c_str(), &stJson) == 0 && S_ISREG(stJson.st_mode) && stJson.st_size > 2) {
        return false; // plugin.json already present
    }

    // Locate .so file inside targetDir (skip pure addins with no VST2 .so)
    std::string resolvedSo = primarySoPath;
    struct stat stSo;
    if (resolvedSo.empty() || stat(resolvedSo.c_str(), &stSo) != 0 || !S_ISREG(stSo.st_mode)) {
        resolvedSo.clear();
        DIR* d = opendir(targetDir.c_str());
        if (d) {
            struct dirent* de;
            while ((de = readdir(d)) != nullptr) {
                size_t len = std::strlen(de->d_name);
                if (len > 3 && std::strcmp(de->d_name + len - 3, ".so") == 0) {
                    resolvedSo = targetDir + "/" + de->d_name;
                    break;
                }
            }
            closedir(d);
        }
    }
    if (resolvedSo.empty()) return false;

    std::string soBase = resolvedSo;
    size_t sSlash = resolvedSo.find_last_of('/');
    if (sSlash != std::string::npos && sSlash + 1 < resolvedSo.size()) {
        soBase = resolvedSo.substr(sSlash + 1);
    }
    int soBits = ConfigParser::getElfFileBitness(resolvedSo, nullptr);
    std::string fileKey = (soBits == 64) ? "file64" : "file32";

    std::string name;
    std::string manufacturer;
    std::string version;
    std::string kind;
    std::string uid;
    std::string category;
    int numInputs = -1;
    int numOutputs = -1;
    std::string binaryFile;
    std::string description;
    std::string license;
    std::string url;
    std::vector<std::string> sourcesUsed;

    auto extractBoolStrField = [](const char* start, const char* end, const char* key, bool& out) -> bool {
        std::string pattern = std::string("\"") + key + "\"";
        const char* found = std::strstr(start, pattern.c_str());
        if (!found || found >= end) return false;
        const char* colon = found + pattern.size();
        while (colon < end && (*colon == ' ' || *colon == '\t' || *colon == '\r' || *colon == '\n')) colon++;
        if (colon < end && *colon == ':') {
            const char* val = colon + 1;
            while (val < end && (*val == ' ' || *val == '\t' || *val == '\r' || *val == '\n')) val++;
            if (val + 4 <= end && std::strncmp(val, "true", 4) == 0) { out = true; return true; }
            if (val + 5 <= end && std::strncmp(val, "false", 5) == 0) { out = false; return true; }
        }
        return false;
    };

    auto basenameOnly = [](const std::string& p) -> std::string {
        size_t sl = p.find_last_of('/');
        return (sl != std::string::npos && sl + 1 < p.size()) ? p.substr(sl + 1) : p;
    };

    // Source 1: mpc-plugin.json
    std::string mpcPluginJsonPath = targetDir + "/mpc-plugin.json";
    {
        std::ifstream mpf(mpcPluginJsonPath, std::ios::binary);
        if (mpf.is_open()) {
            std::stringstream mss;
            mss << mpf.rdbuf();
            std::string mj = mss.str();
            mpf.close();
            if (!mj.empty()) {
                const char* bs = mj.c_str();
                const char* be = bs + mj.size();
                std::string tmp;
                if (extract_string_field(bs, be, "name", tmp) && !tmp.empty()) name = tmp;
                else if (extract_string_field(bs, be, "title", tmp) && !tmp.empty()) name = tmp;
                else if (extract_string_field(bs, be, "pluginName", tmp) && !tmp.empty()) name = tmp;

                if (extract_string_field(bs, be, "manufacturer", tmp) && !tmp.empty()) manufacturer = tmp;
                else if (extract_string_field(bs, be, "author", tmp) && !tmp.empty()) manufacturer = tmp;
                else if (extract_string_field(bs, be, "vendor", tmp) && !tmp.empty()) manufacturer = tmp;

                if (extract_string_field(bs, be, "version", tmp) && !tmp.empty()) version = tmp;

                if (extract_string_field(bs, be, "kind", tmp) && !tmp.empty()) kind = tmp;
                else if (extract_string_field(bs, be, "type", tmp) && !tmp.empty()) kind = tmp;
                else {
                    bool isInstB = true;
                    if (extractBoolStrField(bs, be, "isInstrument", isInstB)) {
                        kind = isInstB ? "instrument" : "effect";
                    } else if (extract_string_field(bs, be, "isInstrument", tmp) && !tmp.empty()) {
                        kind = (tmp == "0" || tmp == "false") ? "effect" : "instrument";
                    }
                }

                if (extract_string_field(bs, be, "uid", tmp) && !tmp.empty()) uid = tmp;
                if (extract_string_field(bs, be, "category", tmp) && !tmp.empty()) category = tmp;

                int nVal = -1;
                if (extract_int_field(bs, be, "numInputs", nVal) && nVal >= 0) numInputs = nVal;
                else if (extract_int_field(bs, be, "inputs", nVal) && nVal >= 0) numInputs = nVal;

                if (extract_int_field(bs, be, "numOutputs", nVal) && nVal >= 0) numOutputs = nVal;
                else if (extract_int_field(bs, be, "outputs", nVal) && nVal >= 0) numOutputs = nVal;

                if (soBits == 64 && extract_string_field(bs, be, "file64", tmp) && !tmp.empty()) binaryFile = basenameOnly(tmp);
                else if (extract_string_field(bs, be, "file32", tmp) && !tmp.empty()) binaryFile = basenameOnly(tmp);
                else if (extract_string_field(bs, be, "file64", tmp) && !tmp.empty()) binaryFile = basenameOnly(tmp);
                else if (extract_string_field(bs, be, "file", tmp) && !tmp.empty()) binaryFile = basenameOnly(tmp);
                else if (extract_string_field(bs, be, "binary", tmp) && !tmp.empty()) binaryFile = basenameOnly(tmp);

                if (extract_string_field(bs, be, "description", tmp) && !tmp.empty()) description = tmp;
                if (extract_string_field(bs, be, "license", tmp) && !tmp.empty()) license = tmp;

                if (extract_string_field(bs, be, "url", tmp) && !tmp.empty()) url = tmp;
                else if (extract_string_field(bs, be, "homepage", tmp) && !tmp.empty()) url = tmp;
                else if (extract_string_field(bs, be, "repo", tmp) && !tmp.empty()) url = tmp;
                else if (extract_string_field(bs, be, "repository", tmp) && !tmp.empty()) url = tmp;

                sourcesUsed.push_back("mpc-plugin.json");
            }
        }
    }

    // Source 2: plugin-meta.xml
    std::string metaXmlPath = targetDir + "/plugin-meta.xml";
    {
        std::ifstream mxf(metaXmlPath, std::ios::binary);
        if (mxf.is_open()) {
            std::stringstream xss;
            xss << mxf.rdbuf();
            std::string mxml = xss.str();
            mxf.close();
            auto entries = extractPluginXmlEntries(mxml, 0, mxml.size());
            if (!entries.empty()) {
                const std::string& tag = entries[0].rawTag;
                if (name.empty()) {
                    name = getXmlTagAttribute(tag, "name");
                    if (name.empty()) name = getXmlTagAttribute(tag, "descriptiveName");
                }
                if (manufacturer.empty()) manufacturer = getXmlTagAttribute(tag, "manufacturer");
                if (version.empty()) version = getXmlTagAttribute(tag, "version");
                if (kind.empty()) {
                    std::string isInst = getXmlTagAttribute(tag, "isInstrument");
                    if (isInst == "0" || isInst == "false") kind = "effect";
                    else if (isInst == "1" || isInst == "true") kind = "instrument";
                }
                if (uid.empty()) {
                    uid = getXmlTagAttribute(tag, "uid");
                    if (uid.empty()) uid = getXmlTagAttribute(tag, "uniqueId");
                }
                if (category.empty()) category = getXmlTagAttribute(tag, "category");
                if (numInputs < 0) {
                    std::string ni = getXmlTagAttribute(tag, "numInputs");
                    if (!ni.empty()) numInputs = std::atoi(ni.c_str());
                }
                if (numOutputs < 0) {
                    std::string no = getXmlTagAttribute(tag, "numOutputs");
                    if (!no.empty()) numOutputs = std::atoi(no.c_str());
                }
                if (binaryFile.empty()) {
                    std::string fAttr = getXmlTagAttribute(tag, "file");
                    if (!fAttr.empty()) {
                        std::string b = basenameOnly(fAttr);
                        if (b.size() > 3 && b.rfind(".so") == b.size() - 3) binaryFile = b;
                    }
                }
                if (description.empty()) description = getXmlTagAttribute(tag, "description");
                if (license.empty()) license = getXmlTagAttribute(tag, "license");
                if (url.empty()) url = getXmlTagAttribute(tag, "url");

                sourcesUsed.push_back("plugin-meta.xml");
            }
        }
    }

    // Source 3: Catalog MenuItem metadata & folder fallbacks
    if (optItem) {
        bool usedCatalog = false;
        if (name.empty() && !optItem->label.empty()) { name = optItem->label; usedCatalog = true; }
        if (manufacturer.empty() && !optItem->author.empty()) { manufacturer = optItem->author; usedCatalog = true; }
        if (version.empty() && !optItem->version.empty()) { version = optItem->version; usedCatalog = true; }
        if (kind.empty()) {
            kind = (optItem->shortType == "FX") ? "effect" : "instrument";
            usedCatalog = true;
        }
        if (category.empty() && !optItem->category.empty()) { category = optItem->category; usedCatalog = true; }
        if (description.empty() && !optItem->description.empty()) { description = optItem->description; usedCatalog = true; }
        if (url.empty() && !optItem->downloadUrl.empty()) {
            std::string u = optItem->downloadUrl;
            size_t rpos = u.find("/releases/");
            if (rpos != std::string::npos) u = u.substr(0, rpos);
            url = u;
            usedCatalog = true;
        }
        if (usedCatalog) sourcesUsed.push_back("catalog metadata");
    }

    // Normalize & fill remaining required fields
    if (name.empty()) {
        name = basenameOnly(targetDir);
        size_t vstPos = name.find(" - VST - ");
        if (vstPos != std::string::npos && vstPos + 9 < name.size()) {
            if (manufacturer.empty()) manufacturer = name.substr(0, vstPos);
            name = name.substr(vstPos + 9);
        }
    }
    if (manufacturer.empty()) manufacturer = "The KikGen Labs";
    if (version.empty()) version = "1.0.0";
    if (version.size() > 1 && (version[0] == 'v' || version[0] == 'V') && std::isdigit(static_cast<unsigned char>(version[1]))) {
        version = version.substr(1);
    }

    {
        std::string lk;
        for (char c : kind) lk += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lk == "fx" || lk == "effect") {
            kind = "effect";
        } else if (lk == "inst" || lk == "instrument" || lk == "synth" || lk == "synthesizer") {
            kind = "instrument";
        } else if (lk.empty()) {
            std::string lc;
            for (char c : category) lc += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            kind = (lc == "fx" || lc == "effect" || lc == "effects") ? "effect" : "instrument";
        }
    }

    if (category.empty()) category = (kind == "effect") ? "Effect" : "Synth";
    if (numInputs < 0) numInputs = (kind == "effect") ? 2 : 0;
    if (numOutputs < 0) numOutputs = 2;

    if (binaryFile.empty()) binaryFile = soBase;
    {
        std::string diskCand = targetDir + "/" + binaryFile;
        struct stat stBin;
        if (stat(diskCand.c_str(), &stBin) != 0 || !S_ISREG(stBin.st_mode)) {
            binaryFile = soBase;
        }
    }

    uid = normalizePluginUidHex(uid, name + "|" + manufacturer + "|" + binaryFile);

    if (description.empty()) {
        description = name + " VST2 " + kind + " plugin for Akai MPC / Force standalone.";
    }
    if (license.empty()) {
        const char* licFiles[] = { "LICENSE", "LICENSE.txt", "LICENSE.md", "license.txt" };
        for (const char* lf : licFiles) {
            struct stat stL;
            if (stat((targetDir + "/" + lf).c_str(), &stL) == 0 && S_ISREG(stL.st_mode)) {
                license = "See LICENSE";
                break;
            }
        }
        if (license.empty()) license = "Open Source";
    }

    std::ofstream out(pluginJsonPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        if (emitLog) emitLog("[WARN] Could not write reconstituted plugin.json at " + pluginJsonPath);
        return false;
    }

    out << "{\n";
    out << "  \"name\": \"" << escapeJsonString(name) << "\",\n";
    out << "  \"manufacturer\": \"" << escapeJsonString(manufacturer) << "\",\n";
    out << "  \"version\": \"" << escapeJsonString(version) << "\",\n";
    out << "  \"kind\": \"" << escapeJsonString(kind) << "\",\n";
    out << "  \"uid\": \"" << escapeJsonString(uid) << "\",\n";
    out << "  \"category\": \"" << escapeJsonString(category) << "\",\n";
    out << "  \"numInputs\": " << numInputs << ",\n";
    out << "  \"numOutputs\": " << numOutputs << ",\n";
    out << "  \"" << fileKey << "\": \"" << escapeJsonString(binaryFile) << "\",\n";
    out << "  \"description\": \"" << escapeJsonString(description) << "\",\n";
    out << "  \"license\": \"" << escapeJsonString(license) << "\",\n";
    out << "  \"url\": \"" << escapeJsonString(url) << "\"\n";
    out << "}\n";
    out.flush();
    out.close();

    std::string srcSummary;
    for (size_t i = 0; i < sourcesUsed.size(); ++i) {
        if (i > 0) srcSummary += " + ";
        srcSummary += sourcesUsed[i];
    }
    if (srcSummary.empty()) srcSummary = "bundle inspection";

    if (emitLog) {
        emitLog("[TKGL-MANIFEST] Reconstituted missing plugin.json in " + targetDir + " (from " + srcSummary + ")");
    }
    return true;
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
    std::string defaultMgrSynths = getManagerVst2Dir();
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

    std::string sysArchDesc;
    int sysBitness = getSystemElfBitness(&sysArchDesc);
    emitLog("[TKGL-ARCH] Current OS / MPC runtime: " + std::to_string(sysBitness) + "-bit (" + sysArchDesc + ")");

    if (isMpcNativeVst2Supported()) {
        emitLog("[TKGL-INSTALL] Step 1/4: MPC OS >= 3.10 native VST2 active (MPC.settings will not be modified).");
    } else {
        emitLog("[TKGL-INSTALL] Step 1/4 (Gold Rule): Preparing MPC.settings backup (" + tsStr + ")...");
    }

    // Step 3A: Resolve or download package binary archive FIRST (before modifying any existing install or drive)
    std::string cachedZipPath;
    bool isCached = isItemBinaryCached(item, nullptr, &cachedZipPath);

    std::string usedZipPath;
    if (isCached && !cachedZipPath.empty()) {
        emitLog("[TKGL-INSTALL] Step 2/4: Using local cached binary archive: " + cachedZipPath + " (offline mode, no download needed)");
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
        emitLog("[TKGL-INSTALL] Step 2/4: Downloading " + item.label + " (" + sizeLabel + ") to local cache via libcurl (IPv4)...");
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

    // Step 3B: RAM-Safe Two-Phase Check - extract ONLY .so files into /tmp for 32/64-bit ELF verification
    std::string localBundleDir;
    if (!usedZipPath.empty()) {
        std::string tmpSoDir = "/tmp/" + item.id + "_" + tsStr + "_so";
        char soStageScriptPath[] = "/tmp/tkgl_sochk_XXXXXX";
        int sfd = mkstemp(soStageScriptPath);
        if (sfd < 0) {
            emitLog("[ERROR] Could not create temp .so extraction script.");
            return -1;
        }

        std::ostringstream ss;
        ss << "#!/bin/sh\n";
        ss << "TMP_ZIP=\"" << usedZipPath << "\"\n";
        ss << "TMP_SO=\"" << tmpSoDir << "\"\n";
        ss << "SO_LIST=\"/tmp/tkgl_sozip_" << tsStr << ".list\"\n";
        ss << "rm -rf \"$TMP_SO\" \"$SO_LIST\"\n";
        ss << "mkdir -p \"$TMP_SO\"\n";
        ss << "if [ ! -s \"$TMP_ZIP\" ]; then\n";
        ss << "  echo \"[ERROR] Package archive missing or empty: $TMP_ZIP\"\n";
        ss << "  rm -rf \"$TMP_SO\"\n";
        ss << "  exit 1\n";
        ss << "fi\n";
        ss << "echo \"[TKGL-ARCH] Extracting only .so binaries to /tmp for 32/64-bit ELF check (RAM-safe)...\"\n";
        ss << "unzip -l \"$TMP_ZIP\" 2>/dev/null | sed -n 's/^[[:space:]]*[0-9][0-9]*[[:space:]]\\{1,\\}[^[:space:]]\\{1,\\}[[:space:]]\\{1,\\}[^[:space:]]\\{1,\\}[[:space:]]\\{1,\\}//p' | grep '\\.so$' > \"$SO_LIST\" || true\n";
        ss << "while IFS= read -r _entry; do\n";
        ss << "  [ -n \"$_entry\" ] || continue\n";
        ss << "  unzip -q -o \"$TMP_ZIP\" \"$_entry\" -d \"$TMP_SO\" 2>/dev/null || true\n";
        ss << "done < \"$SO_LIST\"\n";
        ss << "rm -f \"$SO_LIST\"\n";
        ss << "if [ -z \"$(find \"$TMP_SO\" -type f -name '*.so' 2>/dev/null | head -n 1)\" ]; then\n";
        ss << "  unzip -q -o \"$TMP_ZIP\" '*.so' -d \"$TMP_SO\" 2>/dev/null || true\n";
        ss << "fi\n";

        std::string sBody = ss.str();
        ssize_t written = write(sfd, sBody.c_str(), sBody.size());
        (void)written;
        close(sfd);
        chmod(soStageScriptPath, 0755);

        std::string runStageCmd = std::string("/bin/sh ") + soStageScriptPath + " 2>&1; rm -f " + soStageScriptPath;
        FILE* spipe = popen(runStageCmd.c_str(), "r");
        if (!spipe) {
            emitLog("[ERROR] Failed to run .so staging script.");
            return -1;
        }
        char sline[256];
        while (fgets(sline, sizeof(sline), spipe)) {
            sline[strcspn(sline, "\r\n")] = 0;
            if (sline[0] == 0) continue;
            emitLog(sline);
        }
        int stageRc = pclose(spipe);
        if (stageRc != 0) {
            std::string rmTmp = "rm -rf \"" + tmpSoDir + "\"";
            int rrc = std::system(rmTmp.c_str());
            (void)rrc;
            return stageRc;
        }

        bool archOk = verifyDirectorySoCompatibility(tmpSoDir, emitLog);
        {
            std::string rmTmp = "rm -rf \"" + tmpSoDir + "\"";
            int rrc = std::system(rmTmp.c_str());
            (void)rrc;
        }
        if (!archOk) {
            return -1;
        }
    } else {
        std::string cand1 = mgrDir + "/plugins/" + item.id;
        std::string cand2 = "./plugins/" + item.id;
        struct stat stLocal;
        if (stat(cand1.c_str(), &stLocal) == 0 && S_ISDIR(stLocal.st_mode)) {
            localBundleDir = cand1;
        } else if (stat(cand2.c_str(), &stLocal) == 0 && S_ISDIR(stLocal.st_mode)) {
            localBundleDir = cand2;
        } else {
            emitLog("[ERROR] Local plugin bundle directory missing for " + item.id);
            return -1;
        }
        if (!verifyDirectorySoCompatibility(localBundleDir, emitLog)) {
            return -1;
        }
    }

    // Compatible binary verified: if Reinstalling or Repairing, clean up old files from other SynthLocations
    if (item.installed == 1 || item.installed == 2) {
        auto oldLocs = getSynthContentLocations(mpcSettings);
        std::vector<std::string> allOldDirs;
        for (const auto& ol : oldLocs) {
            if (!ol.path.empty() && ol.path != synthDir) allOldDirs.push_back(ol.path);
        }
        if (!getManagerMountRoot().empty() && defaultMgrSynths != synthDir) {
            allOldDirs.push_back(defaultMgrSynths);
        }
        for (const auto& mp : getExternalHotplugMounts()) {
            std::string dp = mp + "/VST2 Plugins";
            if (dp != synthDir) allOldDirs.push_back(dp);
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
                    int rmRes = std::system(rmCmd.c_str());
                    (void)rmRes;
                    emitLog("[TKGL-INSTALL] Cleaned old location: " + oldDir);
                }
            }
        }
    }

    // Step 3C: Extract full archive DIRECTLY onto target drive (avoiding /tmp RAMFS exhaustion)
    mkdir(synthDir.c_str(), 0755);
    std::string pluginDirName = defaultSubdir;
    std::string targetDir = synthDir + "/" + defaultSubdir;

    if (!usedZipPath.empty()) {
        std::string unpackDir = synthDir + "/.tkgl_unpack_" + tsStr;
        emitLog("[TKGL-INSTALL] Step 3/4: Extracting full archive directly onto target storage " + synthDir + "...");

        char deployScriptPath[] = "/tmp/tkgl_inst_XXXXXX";
        int fd = mkstemp(deployScriptPath);
        if (fd < 0) {
            emitLog("[ERROR] Could not create temp deploy script.");
            return -1;
        }
        std::string outTargetDirFile = std::string(deployScriptPath) + ".tdir";
        std::string outNameFile      = std::string(deployScriptPath) + ".name";

        std::ostringstream ds;
        ds << "#!/bin/sh\n";
        ds << "TMP_ZIP=\"" << usedZipPath << "\"\n";
        ds << "SYNTH_DIR=\"" << synthDir << "\"\n";
        ds << "UNPACK_DIR=\"" << unpackDir << "\"\n";
        ds << "DEFAULT_SUBDIR=\"" << defaultSubdir << "\"\n";
        ds << "rm -rf \"$UNPACK_DIR\"\n";
        ds << "mkdir -p \"$SYNTH_DIR\" \"$UNPACK_DIR\"\n";
        ds << "if ! unzip -q -o \"$TMP_ZIP\" -d \"$UNPACK_DIR\"; then\n";
        ds << "  echo \"[ERROR] Failed to unzip $TMP_ZIP directly into $UNPACK_DIR\"\n";
        ds << "  rm -rf \"$UNPACK_DIR\"\n";
        ds << "  exit 1\n";
        ds << "fi\n";
        ds << "SRC_DIR=\"\"\n";
        ds << "if [ -d \"$UNPACK_DIR/portable\" ]; then\n";
        ds << "  SRC_DIR=$(find \"$UNPACK_DIR/portable\" -mindepth 1 -maxdepth 1 -type d | head -n 1)\n";
        ds << "fi\n";
        ds << "if [ -z \"$SRC_DIR\" ]; then\n";
        ds << "  META_FOUND=$(find \"$UNPACK_DIR\" -name \"plugin-meta.xml\" | head -n 1)\n";
        ds << "  if [ -n \"$META_FOUND\" ]; then SRC_DIR=$(dirname \"$META_FOUND\"); fi\n";
        ds << "fi\n";
        ds << "if [ -z \"$SRC_DIR\" ]; then\n";
        ds << "  JSON_FOUND=$(find \"$UNPACK_DIR\" \\( -name \"plugin.json\" -o -name \"mpc-plugin.json\" \\) | head -n 1)\n";
        ds << "  if [ -n \"$JSON_FOUND\" ]; then SRC_DIR=$(dirname \"$JSON_FOUND\"); fi\n";
        ds << "fi\n";
        ds << "if [ -z \"$SRC_DIR\" ]; then\n";
        ds << "  SO_FOUND=$(find \"$UNPACK_DIR\" -name \"*.so\" | head -n 1)\n";
        ds << "  if [ -n \"$SO_FOUND\" ]; then SRC_DIR=$(dirname \"$SO_FOUND\"); fi\n";
        ds << "fi\n";
        ds << "if [ -z \"$SRC_DIR\" ] || [ \"$SRC_DIR\" = \"$UNPACK_DIR\" ]; then\n";
        ds << "  SRC_DIR=\"$UNPACK_DIR\"\n";
        ds << "  PLUGIN_DIR_NAME=\"$DEFAULT_SUBDIR\"\n";
        ds << "else\n";
        ds << "  PLUGIN_DIR_NAME=$(basename \"$SRC_DIR\")\n";
        ds << "  if [ -z \"$PLUGIN_DIR_NAME\" ] || [ \"$PLUGIN_DIR_NAME\" = \"portable\" ]; then\n";
        ds << "    PLUGIN_DIR_NAME=\"$DEFAULT_SUBDIR\"\n";
        ds << "  fi\n";
        ds << "fi\n";
        ds << "TARGET_DIR=\"$SYNTH_DIR/$PLUGIN_DIR_NAME\"\n";
        ds << "if [ \"$SRC_DIR\" != \"$UNPACK_DIR\" ]; then\n";
        ds << "  rm -rf \"$TARGET_DIR\"\n";
        ds << "  if ! mv \"$SRC_DIR\" \"$TARGET_DIR\" 2>/dev/null; then\n";
        ds << "    mkdir -p \"$TARGET_DIR\"\n";
        ds << "    if ! cp -rf \"$SRC_DIR/\". \"$TARGET_DIR/\"; then\n";
        ds << "      echo \"[ERROR] Failed to promote plugin files to $TARGET_DIR\"\n";
        ds << "      rm -rf \"$UNPACK_DIR\"\n";
        ds << "      exit 1\n";
        ds << "    fi\n";
        ds << "  fi\n";
        ds << "else\n";
        ds << "  rm -rf \"$TARGET_DIR\"\n";
        ds << "  mkdir -p \"$TARGET_DIR\"\n";
        ds << "  if ! cp -rf \"$UNPACK_DIR/\". \"$TARGET_DIR/\"; then\n";
        ds << "    echo \"[ERROR] Failed to copy plugin files to $TARGET_DIR\"\n";
        ds << "    rm -rf \"$UNPACK_DIR\"\n";
        ds << "    exit 1\n";
        ds << "  fi\n";
        ds << "fi\n";
        ds << "rm -rf \"$UNPACK_DIR\"\n";
        ds << "chmod -R 755 \"$TARGET_DIR\" 2>/dev/null || true\n";
        ds << "sync\n";
        ds << "printf '%s' \"$TARGET_DIR\" > \"" << outTargetDirFile << "\"\n";
        ds << "printf '%s' \"$PLUGIN_DIR_NAME\" > \"" << outNameFile << "\"\n";

        std::string dBody = ds.str();
        ssize_t written = write(fd, dBody.c_str(), dBody.size());
        (void)written;
        close(fd);
        chmod(deployScriptPath, 0755);

        std::string runCmd = std::string("/bin/sh ") + deployScriptPath + " 2>&1; rm -f " + deployScriptPath;
        FILE* pipe = popen(runCmd.c_str(), "r");
        if (!pipe) {
            unlink(outTargetDirFile.c_str());
            unlink(outNameFile.c_str());
            emitLog("[ERROR] Failed to run direct-to-drive deployment script.");
            return -1;
        }
        char line[256];
        while (fgets(line, sizeof(line), pipe)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == 0) continue;
            emitLog(line);
        }
        int extractRc = pclose(pipe);

        {
            std::ifstream fTdir(outTargetDirFile, std::ios::binary);
            if (fTdir.is_open()) {
                std::stringstream fss;
                fss << fTdir.rdbuf();
                if (!fss.str().empty()) targetDir = fss.str();
                fTdir.close();
            }
            std::ifstream fName(outNameFile, std::ios::binary);
            if (fName.is_open()) {
                std::stringstream fss;
                fss << fName.rdbuf();
                if (!fss.str().empty()) pluginDirName = fss.str();
                fName.close();
            }
            unlink(outTargetDirFile.c_str());
            unlink(outNameFile.c_str());
        }

        if (extractRc != 0) {
            return extractRc;
        }
        emitLog("[TKGL-INSTALL] Deployed verified plugin directly to " + targetDir);
    } else {
        emitLog("[TKGL-INSTALL] Step 3/4: Deploying verified plugin to " + targetDir);
        char deployScriptPath[] = "/tmp/tkgl_inst_XXXXXX";
        int fd = mkstemp(deployScriptPath);
        if (fd < 0) {
            emitLog("[ERROR] Could not create temp deploy script.");
            return -1;
        }

        std::ostringstream ds;
        ds << "#!/bin/sh\n";
        ds << "SRC_DIR=\"" << localBundleDir << "\"\n";
        ds << "TARGET_DIR=\"" << targetDir << "\"\n";
        ds << "mkdir -p \"$TARGET_DIR\"\n";
        ds << "if ! cp -rf \"$SRC_DIR/\". \"$TARGET_DIR/\"; then\n";
        ds << "  echo \"[ERROR] Failed to copy plugin files to $TARGET_DIR\"\n";
        ds << "  exit 1\n";
        ds << "fi\n";
        ds << "chmod -R 755 \"$TARGET_DIR\" 2>/dev/null || true\n";
        ds << "sync\n";

        std::string dBody = ds.str();
        ssize_t written = write(fd, dBody.c_str(), dBody.size());
        (void)written;
        close(fd);
        chmod(deployScriptPath, 0755);

        std::string runCmd = std::string("/bin/sh ") + deployScriptPath + " 2>&1; rm -f " + deployScriptPath;
        FILE* pipe = popen(runCmd.c_str(), "r");
        if (!pipe) {
            emitLog("[ERROR] Failed to run deployment script.");
            return -1;
        }
        char line[256];
        while (fgets(line, sizeof(line), pipe)) {
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == 0) continue;
            emitLog(line);
        }
        int extractRc = pclose(pipe);
        if (extractRc != 0) {
            return extractRc;
        }
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

    // Reconstitute missing plugin.json from mpc-plugin.json / plugin-meta.xml / catalog if absent
    reconstitutePluginJsonIfMissing(targetDir, primarySoPath, &item, emitLog);

    int updatedPresetsCount = updatePluginPresetsXplFiles(targetDir, primarySoPath, newPluginTags);
    if (updatedPresetsCount > 0) {
        emitLog("[TKGL-PRESETS] Updated " + std::to_string(updatedPresetsCount) + " preset (.xpl) file(s) with file=\"" + primarySoPath + "\"");
    }
    sync();

    // Step 4: Register in MPC.settings ONLY when MPC OS < 3.10 (3.10+ supports VST2 natively)
    if (isMpcNativeVst2Supported()) {
        emitLog("[TKGL-INSTALL] Step 4/4: Skipped MPC.settings update (native VST2 support on MPC OS >= 3.10).");
    } else {
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

    if (isMpcNativeVst2Supported()) {
        emitLog("[TKGL-REMOVE] Step 1/3: Skipped MPC.settings update (native VST2 support on MPC OS >= 3.10).");
    } else {
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
            if (!item.skinFolderPath.empty() && ep.file.find(item.skinFolderPath + "/") != std::string::npos) return true;
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
    if (!getManagerMountRoot().empty()) {
        allLocs.push_back(getManagerVst2Dir());
        allLocs.push_back(mgrDir + "/VST2 Plugins");
    }

    // Auto-discover external hotplug mounts ONLY (<HotplugMount>/VST2 Plugins and <HotplugMount>/Expansions)
    for (const auto& drive : getExternalHotplugMounts()) {
        allLocs.push_back(drive + "/VST2 Plugins");
        allLocs.push_back(drive + "/Expansions");
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

    if (!item.skinFolderPath.empty()) {
        std::string sf = item.skinFolderPath;
        while (sf.size() > 1 && sf.back() == '/') sf.pop_back();
        size_t slashCount = static_cast<size_t>(std::count(sf.begin(), sf.end(), '/'));
        bool isProtectedRoot = (sf == "/" || sf == "/media" ||
                                sf.size() < 12 ||
                                (sf.rfind("/media/", 0) == 0 && slashCount < 4) ||
                                (sf.size() >= 13 && sf.compare(sf.size() - 13, 13, "/VST2 Plugins") == 0) ||
                                (sf.size() >= 11 && sf.compare(sf.size() - 11, 11, "/Expansions") == 0) ||
                                (sf.size() >= 7 && sf.compare(sf.size() - 7, 7, "/Synths") == 0));
        if (!isProtectedRoot) {
            ss << "if [ -d \"" << sf << "\" ]; then\n";
            ss << "  rm -rf \"" << sf << "\"\n";
            ss << "  echo \"[TKGL-REMOVE] Deleted broken/installed folder: " << sf << "\"\n";
            ss << "fi\n";
        }
    }

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
    std::string defSynthDir = getManagerVst2Dir();
    std::string finalCmd;
    if (item.is_inline) {
        char tmpPath[] = "/tmp/tkgl_exec_XXXXXX";
        int fd = mkstemp(tmpPath);
        if (fd >= 0) {
            std::string scriptBody = "#!/bin/sh\nexport PLG_MAN_DIR=\"" + mgrDir + "\"\ncd \"$PLG_MAN_DIR\" 2>/dev/null || true\nexport SYNTH_INSTALL_DIR=\"${SYNTH_INSTALL_DIR:-" + defSynthDir + "}\"\n" + item.cmd + "\n";
            ssize_t written = write(fd, scriptBody.c_str(), scriptBody.size());
            (void)written;
            close(fd);
            chmod(tmpPath, 0755);
            finalCmd = std::string("/bin/sh ") + tmpPath + " 2>&1; rm -f " + tmpPath;
        } else {
            finalCmd = "PLG_MAN_DIR=\"" + mgrDir + "\" SYNTH_INSTALL_DIR=\"" + defSynthDir + "\" /bin/sh -c '" + item.cmd + "' 2>&1";
        }
    } else {
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
