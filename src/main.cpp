/**
 * ============================================================================
 * THE KIKGEN LABS - PLUGINS MANAGER FOR MPCs AND FORCE DEVICES
 * File: src/main.cpp
 * Description: Standalone DRM/Framebuffer application entry point for
 *              Akai MPC Live / Live II / One / X / Force (Linux ARMv7hf).
 * ============================================================================
 */

#include "tkgl_gfx.hpp"
#include "tkgl_config.hpp"
#include "tkgl_ui_toolkit.hpp"

#include <thread>
#include <mutex>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <alsa/asoundlib.h>
#include <libgen.h>

static std::atomic<bool> g_running{true};
static std::mutex        g_ui_mutex;

static void sig_handler(int) {
    g_running = false;
}

struct AkaiMidiPortCandidate {
    std::string device; // e.g. "hw:1,0,0" or "hw:1,0,1"
    std::string label;  // e.g. "Akai Pro Force Public" or "MPC Private"
};

static std::vector<AkaiMidiPortCandidate> find_mpc_public_private_midi_ports() {
    std::vector<AkaiMidiPortCandidate> ports;

    auto addUniquePort = [&](const std::string& devStr, const std::string& labelStr) {
        for (const auto& p : ports) {
            if (p.device == devStr) return;
        }
        ports.push_back({ devStr, labelStr });
    };

    // Pass 1: Scan all ALSA RawMIDI subdevices whose name explicitly contains "Public" or "Private"
    // (e.g. "Akai Pro Force Public", "Akai Pro Force Private", "MPC Public", "MPC Private", "MPC Studio Live Public/Private")
    int card = -1;
    while (snd_card_next(&card) >= 0 && card >= 0) {
        snd_ctl_t* ctl = nullptr;
        char ctl_name[32];
        std::snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);
        if (snd_ctl_open(&ctl, ctl_name, 0) < 0) continue;

        int dev = -1;
        while (snd_ctl_rawmidi_next_device(ctl, &dev) >= 0 && dev >= 0) {
            snd_rawmidi_info_t* info;
            snd_rawmidi_info_alloca(&info);
            snd_rawmidi_info_set_device(info, dev);
            snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
            snd_rawmidi_info_set_subdevice(info, 0);
            if (snd_ctl_rawmidi_info(ctl, info) < 0) continue;

            int subs = snd_rawmidi_info_get_subdevices_count(info);
            for (int sub = 0; sub < subs; ++sub) {
                snd_rawmidi_info_set_subdevice(info, sub);
                if (snd_ctl_rawmidi_info(ctl, info) == 0) {
                    const char* sub_name = snd_rawmidi_info_get_subdevice_name(info);
                    if (sub_name && (std::strstr(sub_name, "Public") != nullptr || std::strstr(sub_name, "Private") != nullptr)) {
                        char hw_buf[32];
                        std::snprintf(hw_buf, sizeof(hw_buf), "hw:%d,%d,%d", card, dev, sub);
                        addUniquePort(hw_buf, sub_name);
                    }
                }
            }
        }
        snd_ctl_close(ctl);
    }

    // Pass 2: If no subdevice explicitly named "Public" or "Private" was returned by the kernel driver,
    // check any ALSA card/rawmidi device matching "Force", "MPC", or "Akai" and include Subdevice 0 (Public) & Subdevice 1 (Private)
    if (ports.empty()) {
        card = -1;
        while (snd_card_next(&card) >= 0 && card >= 0) {
            snd_ctl_t* ctl = nullptr;
            char ctl_name[32];
            std::snprintf(ctl_name, sizeof(ctl_name), "hw:%d", card);
            if (snd_ctl_open(&ctl, ctl_name, 0) < 0) continue;

            char* card_name_ptr = nullptr;
            snd_card_get_name(card, &card_name_ptr);
            std::string cardName = card_name_ptr ? card_name_ptr : "";
            if (card_name_ptr) std::free(card_name_ptr);

            int dev = -1;
            while (snd_ctl_rawmidi_next_device(ctl, &dev) >= 0 && dev >= 0) {
                snd_rawmidi_info_t* info;
                snd_rawmidi_info_alloca(&info);
                snd_rawmidi_info_set_device(info, dev);
                snd_rawmidi_info_set_stream(info, SND_RAWMIDI_STREAM_INPUT);
                snd_rawmidi_info_set_subdevice(info, 0);
                if (snd_ctl_rawmidi_info(ctl, info) < 0) continue;

                const char* rm_name = snd_rawmidi_info_get_name(info);
                std::string rmName = rm_name ? rm_name : "";
                int subs = snd_rawmidi_info_get_subdevices_count(info);

                bool isAkaiSurface =
                    cardName.find("Force") != std::string::npos ||
                    cardName.find("MPC")   != std::string::npos ||
                    cardName.find("Akai")  != std::string::npos ||
                    rmName.find("Force")   != std::string::npos ||
                    rmName.find("MPC")     != std::string::npos ||
                    rmName.find("Akai")    != std::string::npos;

                if (isAkaiSurface && subs >= 1) {
                    std::string baseLabel = !rmName.empty() ? rmName : cardName;
                    char hw0[32];
                    std::snprintf(hw0, sizeof(hw0), "hw:%d,%d,0", card, dev);
                    addUniquePort(hw0, baseLabel + " Public");
                    if (subs >= 2) {
                        char hw1[32];
                        std::snprintf(hw1, sizeof(hw1), "hw:%d,%d,1", card, dev);
                        addUniquePort(hw1, baseLabel + " Private");
                    }
                }
            }
            snd_ctl_close(ctl);
        }
    }

    return ports;
}

struct OpenMidiPortState {
    std::string    device;
    std::string    label;
    snd_rawmidi_t* handle         = nullptr;
    uint8_t        status         = 0;
    uint8_t        data[2]        = {0, 0};
    int            data_idx       = 0;
    bool           activityLogged = false;
};

static void midi_thread_func(tkgl::ui::PluginManagerView* ui) {
    auto candidates = find_mpc_public_private_midi_ports();
    if (candidates.empty()) {
        candidates.push_back({ "hw:1,0,0", "Fallback Public (hw:1,0,0)" });
        candidates.push_back({ "hw:1,0,1", "Fallback Private (hw:1,0,1)" });
        candidates.push_back({ "hw:0,0,0", "Fallback Public (hw:0,0,0)" });
        candidates.push_back({ "hw:0,0,1", "Fallback Private (hw:0,0,1)" });
    }

    std::vector<OpenMidiPortState> openPorts;
    for (const auto& cand : candidates) {
        snd_rawmidi_t* h = nullptr;
        if (snd_rawmidi_open(&h, nullptr, cand.device.c_str(), SND_RAWMIDI_NONBLOCK) >= 0 && h != nullptr) {
            OpenMidiPortState st;
            st.device = cand.device;
            st.label  = cand.label;
            st.handle = h;
            openPorts.push_back(st);

            std::string scanMsg = "[TKGL-MIDI] Opened persistent Public/Private port: " + st.device + " [" + st.label + "]";
            std::printf("%s\n", scanMsg.c_str());
            tkgl::config::ConfigParser::logMessage(scanMsg);
        }
    }

    if (openPorts.empty()) {
        std::string errMsg = "[TKGL-MIDI] ERROR: Could not open any ALSA Public or Private MIDI port (hw:X,0,0 / hw:X,0,1).";
        std::fprintf(stderr, "%s\n", errMsg.c_str());
        tkgl::config::ConfigParser::logMessage(errMsg);
        return;
    }

    {
        std::string sessMsg = "[TKGL-MIDI] Dual-port session scan active across " +
            std::to_string(openPorts.size()) + " port(s) for entire session (Public + Private sleep-safe monitoring).";
        std::printf("%s\n", sessMsg.c_str());
        tkgl::config::ConfigParser::logMessage(sessMsg);
    }

    uint8_t raw_buf[64];

    while (g_running) {
        bool anyRead = false;

        // Maintain scan across ALL opened Public & Private ports for the entire session
        // (either port may enter controller sleep unexpectedly on MPC/Force firmware).
        for (size_t p = 0; p < openPorts.size(); ++p) {
            auto& port = openPorts[p];
            if (!port.handle) continue;

            ssize_t n = snd_rawmidi_read(port.handle, raw_buf, sizeof(raw_buf));
            if (n <= 0) continue;
            anyRead = true;

            for (ssize_t i = 0; i < n; ++i) {
                uint8_t b = raw_buf[i];
                // Ignore MIDI Real-Time bytes (0xF8..0xFF) without disrupting running status
                if (b >= 0xF8) continue;
                // Ignore System Common / SysEx framing (0xF0..0xF7) and clear running status so SysEx payload bytes are skipped
                if (b >= 0xF0) {
                    port.status = 0;
                    port.data_idx = 0;
                    continue;
                }
                if (b & 0x80) {
                    port.status = b & 0xF0;
                    port.data_idx = 0;
                } else if (port.status != 0) {
                    port.data[port.data_idx++] = b;
                    int expected = (port.status == 0xC0 || port.status == 0xD0) ? 1 : 2;
                    if (port.data_idx >= expected) {
                        uint8_t d1 = port.data[0];
                        uint8_t d2 = (expected == 2) ? port.data[1] : 0;
                        port.data_idx = 0;

                        if (!port.activityLogged) {
                            port.activityLogged = true;
                            std::string actMsg = "[TKGL-MIDI] Active MIDI traffic received on persistent port: " +
                                port.device + " [" + port.label + "]";
                            std::printf("%s\n", actMsg.c_str());
                            tkgl::config::ConfigParser::logMessage(actMsg);
                        }

                        std::lock_guard<std::mutex> lock(g_ui_mutex);
                        // Wake from screensaver on ANY MIDI message (pads, Q-Links, buttons, wheel)
                        if (ui->isScreensaverActive()) {
                            ui->onAnyInput();
                            continue;
                        }
                        ui->onAnyInput();

                        if (port.status == 0xB0 && d1 == 0x64) {
                            // Hardware-confirmed JOG WHEEL rotation across all MPC & Force devices:
                            //   CW  Turn: B0 64 01 (0x01..0x3F)
                            //   CCW Turn: B0 64 7F (0x7F..0x40)
                            int delta = (d2 < 64) ? static_cast<int>(d2) : (static_cast<int>(d2) - 128);
                            ui->onWheelDelta(delta);
                        } else if (port.status == 0x90 && d2 > 0) {
                            // Hardware-confirmed JOG WHEEL Push & Buttons:
                            // - 0x6F (90 6F 7F): JOG WHEEL APPUI (Release = 90 6F 00) -> Enter / Confirm
                            // - 0x36 (90 36 7F): PLUS (+) Button  -> Enter / Confirm
                            // - 0x20 (90 20 7F): ENCODER PUSH alt -> Enter / Confirm
                            // - 0x02 (90 02 7F): MENU Button (Akai Force) -> Back / Menu
                            // - 0x00 (90 00 7F): NAVIGATE Button (Akai Force) -> Back / Menu
                            // - 0x7B (90 7B 7F): MENU Button (MPC) -> Back / Menu
                            // - 0x34 (90 34 7F): MAIN Button (MPC) -> Back / Menu
                            // - 0x37 (90 37 7F): MINUS (-) Button (MPC) -> Back / Menu
                            if (d1 == 0x02 || d1 == 0x00 || d1 == 0x37 || d1 == 0x7B || d1 == 0x34) {
                                ui->onMenuButton();
                            } else if (d1 == 0x6F || d1 == 0x36 || d1 == 0x20) {
                                ui->onWheelPress();
                            } else if (d1 == 0x21 || d1 == 0x3D) {
                                ui->onWheelDelta(-1);
                            } else if (d1 == 0x22 || d1 == 0x3E) {
                                ui->onWheelDelta(1);
                            }
                            if (ui->shouldExit()) {
                                g_running = false;
                            }
                        }
                    }
                }
            }
        }

        if (!anyRead) {
            usleep(4000);
        }
    }

    for (auto& port : openPorts) {
        if (port.handle) {
            snd_rawmidi_close(port.handle);
            port.handle = nullptr;
            std::string closeMsg = "[TKGL-MIDI] Closed persistent MIDI port cleanly at session end: " +
                port.device + " [" + port.label + "]";
            std::printf("%s\n", closeMsg.c_str());
            tkgl::config::ConfigParser::logMessage(closeMsg);
        }
    }
}

static void touch_thread_func(tkgl::gfx::DrmFramebufferCanvas* canvas, tkgl::ui::PluginManagerView* ui) {
    int touch_fd = -1;
    int max_x = 0, max_y = 0;
    std::string opened_dev;

    DIR* dir = opendir("/dev/input");
    if (!dir) return;

    struct dirent* ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (std::strncmp(ent->d_name, "event", 5) == 0) {
            std::string devPath = std::string("/dev/input/") + ent->d_name;
            int fd = open(devPath.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                unsigned long evbit = 0;
                ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), &evbit);
                if (evbit & (1 << EV_ABS)) {
                    struct input_absinfo abs_x, abs_y;
                    std::memset(&abs_x, 0, sizeof(abs_x));
                    std::memset(&abs_y, 0, sizeof(abs_y));
                    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &abs_x) == 0 && abs_x.maximum > 0) {
                        max_x = abs_x.maximum;
                    } else if (ioctl(fd, EVIOCGABS(ABS_X), &abs_x) == 0) {
                        max_x = abs_x.maximum;
                    }
                    if (ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &abs_y) == 0 && abs_y.maximum > 0) {
                        max_y = abs_y.maximum;
                    } else if (ioctl(fd, EVIOCGABS(ABS_Y), &abs_y) == 0) {
                        max_y = abs_y.maximum;
                    }
                    if (max_x > 0 && max_y > 0) {
                        touch_fd = fd;
                        opened_dev = devPath;
                        break;
                    }
                }
                close(fd);
            }
        }
    }
    closedir(dir);

    if (touch_fd < 0) return;
    std::printf("[TKGL-TOUCH] Opened %s (max_x=%d, max_y=%d)\n", opened_dev.c_str(), max_x, max_y);

    int cur_x = 0, cur_y = 0;
    bool down = false;
    struct input_event ev;

    while (g_running) {
        ssize_t r = read(touch_fd, &ev, sizeof(ev));
        if (r == sizeof(ev)) {
            if (ev.type == EV_ABS) {
                if (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X) cur_x = ev.value;
                else if (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y) cur_y = ev.value;
                else if (ev.code == ABS_MT_TRACKING_ID) {
                    if (ev.value >= 0 && !down) {
                        down = true;
                    } else if (ev.value == -1 && down) {
                        down = false;
                        int lx = 0, ly = 0;
                        canvas->mapTouchToLogical(cur_x, cur_y, max_x, max_y, lx, ly);
                        std::lock_guard<std::mutex> lock(g_ui_mutex);
                        ui->onTouchDown(lx, ly);
                        if (ui->shouldExit()) g_running = false;
                    }
                }
            } else if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                if (ev.value == 1 && !down) {
                    down = true;
                } else if (ev.value == 0 && down) {
                    down = false;
                    int lx = 0, ly = 0;
                    canvas->mapTouchToLogical(cur_x, cur_y, max_x, max_y, lx, ly);
                    std::lock_guard<std::mutex> lock(g_ui_mutex);
                    ui->onTouchDown(lx, ly);
                    if (ui->shouldExit()) g_running = false;
                }
            }
        } else {
            usleep(4000);
        }
    }
    close(touch_fd);
}

int main(int argc, char* argv[]) {
    // Check command line option -v / --version anywhere in arguments
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--version") == 0) {
            std::printf("The KikGen Labs - Plugins Manager for Akai MPC and Force Devices\n");
            std::printf("Version: %s - Compiled on %s %s\n", tkgl::config::TKGL_MANAGER_VERSION, __DATE__, __TIME__);
            return 0;
        }
    }

    // 1. Startup Prompt & Version identification per specification
    std::printf("The KikGen Labs - Plugins Manager for Akai MPC and Force Devices\n");
    std::printf("Version: %s - Compiled on %s %s\n", tkgl::config::TKGL_MANAGER_VERSION, __DATE__, __TIME__);

    // Initialize clean persistent log file at manager startup (cleared on every launch)
    tkgl::config::ConfigParser::initLogFile();

    // Initialize session staging in /tmp/MPC.settings.new and clean up obsolete tmp files
    tkgl::config::ConfigParser::initSession();

    // Headless helper mode: mpc_plugin_manager_fb --fetch <url> <dest> (uses libcurl IPv4)
    if (argc >= 4 && std::strcmp(argv[1], "--fetch") == 0) {
        return tkgl::config::ConfigParser::downloadFile(argv[2], argv[3], 60) ? 0 : 1;
    }

    std::signal(SIGINT, sig_handler);
    std::signal(SIGTERM, sig_handler);

    // Resolve root working directory:
    // Priority 1: PLG_MAN_DIR environment variable (when binary is executed from /tmp due to USB vfat noexec)
    // Priority 2: Executable directory resolved from /proc/self/exe (unless /tmp without config)
    std::string rootDir = tkgl::config::ConfigParser::getManagerRootDir();
    if (!rootDir.empty()) {
        if (chdir(rootDir.c_str()) == 0) {
            std::printf("[TKGL-MAIN] Working directory set to: %s\n", rootDir.c_str());
        } else {
            std::fprintf(stderr, "[TKGL-MAIN] Warning: Could not chdir to %s\n", rootDir.c_str());
        }
    }

    std::string cfgPath = (argc > 1) ? argv[1] : "mpc_plugins.conf";
    const char* fbDev   = (argc > 2) ? argv[2] : "/dev/fb0";

    tkgl::config::LauncherConfig config;
    tkgl::config::ConfigParser::loadFromFile(cfgPath, config);

    tkgl::gfx::DrmFramebufferCanvas canvas;
    if (!canvas.init(fbDev, config.screen_rotation)) {
        std::fprintf(stderr, "[TKGL-MAIN] Warning: /dev/fb0 unavailable, running in offscreen mode.\n");
    }

    tkgl::ui::PluginManagerView ui(canvas, config);
    {
        std::lock_guard<std::mutex> lock(g_ui_mutex);
        ui.render();
    }

    std::thread midiThread(midi_thread_func, &ui);
    std::thread touchThread(touch_thread_func, &canvas, &ui);

    int idleTickCounter = 0;
    while (g_running) {
        usleep(50000);
        {
            std::lock_guard<std::mutex> lock(g_ui_mutex);
            if (ui.shouldExit()) {
                g_running = false;
                break;
            }
            if (++idleTickCounter >= 4) { // Every 200ms
                idleTickCounter = 0;
                ui.checkIdleTick();
            }
        }
    }

    if (midiThread.joinable()) midiThread.join();
    if (touchThread.joinable()) touchThread.join();

    // Commit session staged changes in /tmp/MPC.settings.new if dirty
    if (tkgl::config::ConfigParser::hasStagedMpcSettings()) {
        std::vector<std::string> exitLogs;
        std::printf("[TKGL-MAIN] Committing session staged MPC.settings changes...\n");
        tkgl::config::ConfigParser::commitSessionMpcSettings(&exitLogs);
        for (const auto& log : exitLogs) {
            std::printf("%s\n", log.c_str());
        }
    }

    canvas.clearScreenBlack();
    canvas.shutdown();
    return 0;
}
