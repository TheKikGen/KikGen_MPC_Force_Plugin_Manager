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

static bool find_mpc_private_midi(char* out_device, size_t max_len) {
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
            snd_ctl_rawmidi_info(ctl, info);

            int subs = snd_rawmidi_info_get_subdevices_count(info);
            for (int sub = 0; sub < subs; ++sub) {
                snd_rawmidi_info_set_subdevice(info, sub);
                if (snd_ctl_rawmidi_info(ctl, info) == 0) {
                    const char* sub_name = snd_rawmidi_info_get_subdevice_name(info);
                    if (sub_name && std::strstr(sub_name, "Private") != nullptr) {
                        std::snprintf(out_device, max_len, "hw:%d,%d,%d", card, dev, sub);
                        snd_ctl_close(ctl);
                        return true;
                    }
                }
            }
        }
        snd_ctl_close(ctl);
    }
    return false;
}

static void midi_thread_func(tkgl::ui::PluginManagerView* ui) {
    char midi_dev[64] = {0};
    if (!find_mpc_private_midi(midi_dev, sizeof(midi_dev))) {
        std::snprintf(midi_dev, sizeof(midi_dev), "hw:0,0,1");
    }

    snd_rawmidi_t* midi_in = nullptr;
    if (snd_rawmidi_open(&midi_in, nullptr, midi_dev, SND_RAWMIDI_NONBLOCK) < 0) {
        std::snprintf(midi_dev, sizeof(midi_dev), "hw:1,0,1");
        if (snd_rawmidi_open(&midi_in, nullptr, midi_dev, SND_RAWMIDI_NONBLOCK) < 0) {
            std::fprintf(stderr, "[TKGL-MIDI] Could not open ALSA Private MIDI port.\n");
            return;
        }
    }
    std::printf("[TKGL-MIDI] Listening on ALSA Private port: %s\n", midi_dev);

    uint8_t status = 0;
    uint8_t data[2] = {0, 0};
    int data_idx = 0;
    uint8_t raw_buf[32];

    while (g_running) {
        ssize_t n = snd_rawmidi_read(midi_in, raw_buf, sizeof(raw_buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; ++i) {
                uint8_t b = raw_buf[i];
                if (b >= 0xF8) continue;
                if (b & 0x80) {
                    status = b & 0xF0;
                    data_idx = 0;
                } else if (status != 0) {
                    data[data_idx++] = b;
                    int expected = (status == 0xC0 || status == 0xD0) ? 1 : 2;
                    if (data_idx >= expected) {
                        uint8_t d1 = data[0];
                        uint8_t d2 = (expected == 2) ? data[1] : 0;
                        data_idx = 0;

                        std::lock_guard<std::mutex> lock(g_ui_mutex);
                        // Wake from screensaver on ANY MIDI message (pads, Q-Links, buttons, wheel)
                        if (ui->isScreensaverActive()) {
                            ui->onAnyInput();
                            continue;
                        }
                        ui->onAnyInput();
                        if (status == 0xB0 && d1 == 0x64) {
                            // Data Wheel rotation: B0 64 01 (CW) / B0 64 7F (CCW)
                            int delta = (d2 < 64) ? static_cast<int>(d2) : (static_cast<int>(d2) - 128);
                            ui->onWheelDelta(delta);
                        } else if (status == 0x90 && d2 > 0) {
                            // MPC Internal MIDI Controller Note Mapping (TheKikGen MPC-LiveXplore):
                            // - 0x6F (90 6F 7F): Data Wheel Push -> Enter / Install
                            // - 0x37 (90 37 7F): Minus (-) Button -> Back (same as < BACK touch button)
                            // - 0x36 (90 36 7F): Plus (+) Button  -> Next / Enter
                            // - 0x7B (90 7B 7F): MENU Button      -> Back to Menu
                            // - 0x34 (90 34 7F): MAIN Button      -> Back to Menu
                            if (d1 == 0x37 || d1 == 0x7B || d1 == 0x34) {
                                ui->onMenuButton();
                            } else if (d1 == 0x6F || d1 == 0x36) {
                                ui->onWheelPress();
                            }
                            if (ui->shouldExit()) {
                                g_running = false;
                            }
                        }
                    }
                }
            }
        } else {
            usleep(4000);
        }
    }
    snd_rawmidi_close(midi_in);
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
    // Headless helper mode: mpc_plugin_manager_fb --fetch <url> <dest>
    // Uses dynamic libcurl.so.4 / libcurl.so.4.8.0 (dlopen) for background/script downloads.
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
    canvas.clearScreenBlack();
    canvas.shutdown();
    return 0;
}
