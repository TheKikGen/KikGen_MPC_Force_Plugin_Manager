#!/bin/sh
#
# autorun sub script for MPC device.
# Runs mpc_plugin_manager_fb directly from /tmp to bypass noexec.
#

SCRIPT_NAME=$(readlink -f "$0")
SCRIPT_DIR=$(dirname "$SCRIPT_NAME")

PLG_MAN_NAME="mpc_plugin_manager_fb"
PLG_MAN_DIR="$SCRIPT_DIR/mpc_plugin_manager"


# --- Service Management Functions ---

purge_mpc_runtime_state() {
    rm -f "/media/az01-internal/Settings/MPC/MPC.crashinfo" \
          "/media/az01-internal/Settings/MPC/MPC.running" \
          2>/dev/null || true
}

stop_mpc() {
    echo "[*] Detecting and stopping active MPC hardware/audio services..."
    if command -v systemctl >/dev/null 2>&1; then
        if systemctl is-active --quiet acvs.service 2>/dev/null || [ -f /lib/systemd/system/acvs.service ]; then
            echo "  -> Stopping systemd unit: acvs.service..."
            systemctl stop acvs.service 2>/dev/null || true
        fi
        if systemctl is-active --quiet inmusic-mpc 2>/dev/null || [ -f /lib/systemd/system/inmusic-mpc.service ]; then
            echo "  -> Stopping systemd unit: inmusic-mpc..."
            systemctl stop inmusic-mpc 2>/dev/null || true
        fi
    fi

    if [ -x /etc/init.d/S90acvs ]; then
        echo "  -> Stopping SysVinit script: /etc/init.d/S90acvs..."
        /etc/init.d/S90acvs stop 2>/dev/null || true
    fi
    if [ -x /etc/init.d/S99mpc ]; then
        echo "  -> Stopping SysVinit script: /etc/init.d/S99mpc..."
        /etc/init.d/S99mpc stop 2>/dev/null || true
    fi

    purge_mpc_runtime_state
}

start_mpc() {
    purge_mpc_runtime_state
    echo "[*] Autodetecting and restoring MPC services..."

    if command -v systemctl >/dev/null 2>&1; then
        if [ -f /lib/systemd/system/acvs.service ]; then
            echo "  -> Starting systemd unit: acvs.service"
            systemctl start acvs.service 2>/dev/null || true
            return 0
        elif [ -f /lib/systemd/system/inmusic-mpc.service ]; then
            echo "  -> Starting systemd unit: inmusic-mpc"
            systemctl start inmusic-mpc 2>/dev/null || true
            return 0
        fi
    fi

    if [ -x /etc/init.d/S90acvs ]; then
        echo "  -> Starting SysVinit script: /etc/init.d/S90acvs"
        /etc/init.d/S90acvs start 2>/dev/null || true
    elif [ -x /etc/init.d/S99mpc ]; then
        echo "  -> Starting SysVinit script: /etc/init.d/S99mpc"
        /etc/init.d/S99mpc start 2>/dev/null || true
    fi
}


# --- CLI Mode ---
if [ -n "$1" ]; then
    case "$1" in
        stop_mpc|kill_manual_mpc_processes|start_mpc|purge_mpc_runtime_state)
            echo "[*] CLI Mode: Executing function '$1'..."
            "$1"
            exit 0
            ;;
        *)
            echo "[!] Unknown function: $1" >&2
            echo "    Available: stop_mpc, start_mpc, purge_mpc_runtime_state" >&2
            exit 1
            ;;
    esac
fi

# --- Main Logic ---

# 1. Check mount point and filesystem type. Only EXFAT allowed.

MNT_POINT=$(df -P "$SCRIPT_DIR" 2>/dev/null | awk 'NR==2 {print $6}')
FS_TYPE=$(awk -v mnt="$MNT_POINT" '$2 == mnt {print $3}' /proc/mounts | head -n 1)

# Fallback if no /proc/mounts sign
if [ -z "$FS_TYPE" ]; then
    FS_TYPE=$(df -T "$SCRIPT_DIR" 2>/dev/null | awk 'NR==2 {print $2}')
fi

echo "[*] Mounting point detected : $MNT_POINT ($FS_TYPE)"

# Remount the volume to grant exec permission

if awk -v mnt="$MNT_POINT" '$2 == mnt {print $4}' /proc/mounts | grep -qw "noexec"; then
    echo "[*] Flag 'noexec' detected at $MNT_POINT. Remount with exec permission..."
    mount -o remount,exec "$MNT_POINT" 2>/dev/null || true
    
    # Sanity check
    if awk -v mnt="$MNT_POINT" '$2 == mnt {print $4}' /proc/mounts | grep -qw "noexec"; then
        echo "[!] Fail to remount with exec permission. Refused by Kernel." >&2
        exit 1
    else
        echo "[+] Remount successfull : exec granted at $MNT_POINT"
    fi
fi


# Exit if it is not an EXFAT FS
if [ "$FS_TYPE" != "exfat" ]; then
    echo "[!] Critical : the volume isn't a exFAT format (ddetected was : '$FS_TYPE')." >&2
    echo "[!] This script needs an exFAT volume to run." >&2
    exit 1
fi


# 2. Check subfolder first, then script root directory
if [ ! -d "$PLG_MAN_DIR" ]; then
    echo "[!] Error: no $PLG_MAN_DIR folder found" >&2
    exit 1
fi
echo "[+] Plugin manager folder found at $PLG_MAN_DIR..."

# 3. Verify source binary exists

TARGET_BIN="$PLG_MAN_DIR/$PLG_MAN_NAME"

if [ ! -f "$TARGET_BIN" ]; then
    echo "[!] Error: Source binary not found at $TARGET_BIN" >&2
    exit 1
fi

chmod +x "$TARGET_BIN"
export PLG_MAN_DIR="$PLG_MAN_DIR"

# 4. Stop native MPC services
stop_mpc

# 5. Launch binary directly in main shell context
echo "[+] Launching $PLG_MAN_NAME (PLG_MAN_DIR=$PLG_MAN_DIR)..."
ORIG_PWD="$PWD"

if cd "$PLG_MAN_DIR" 2>/dev/null; then
    "$TARGET_BIN" || true
    cd "$ORIG_PWD" 2>/dev/null || true
else
    echo "[!] Warning: Failed to cd into $PLG_MAN_DIR. Running directly..."
    "$TARGET_BIN" || true
fi

# 5. Grace period and restore native MPC services
echo "[*] Plugin manager closed. Waiting 5 seconds before restarting MPC services..."
sleep 5

start_mpc
exit 0