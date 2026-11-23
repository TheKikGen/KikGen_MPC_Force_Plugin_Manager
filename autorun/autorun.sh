#!/bin/sh
#
# __ __| |           |  /_) |     ___|             |           |
#    |   __ \   _ \  ' /  | |  / |      _ \ __ \   |      _` | __ \   __|
#    |   | | |  __/  . \  |   <  |   |  __/ |   |  |     (   | |   |\__ \
#   _|  _| |_|\___| _|\_\_|_|\_\\____|\___|_|  _| _____|\__,_|_.__/ ____/
#
# autorun static script for MPC device. 
# https://github.com/TheKikGen/MPC-LiveXplore

SCRIPT_NAME=$(readlink -f "$0")
SCRIPT_DIR=$(dirname "$SCRIPT_NAME")

echo "The KikGen Labs- Autorun for MPC family"
echo "[+] Running from $SCRIPT_DIR"

TKGL_SUB_SCRIPT_NAME=$SCRIPT_DIR/autorun/tkgl_sub_script.sh

if [ -f "$TKGL_SUB_SCRIPT_NAME" ]
then
     sh "$TKGL_SUB_SCRIPT_NAME"    
     EXIT_CODE=$?
else
    echo "Looks like there is not $TKGL_SUB_SCRIPT_NAME file here ! - exiting"
    exit 1
fi

echo "autorun.sh has ended with code $EXIT_CODE."
exit $EXIT_CODE

