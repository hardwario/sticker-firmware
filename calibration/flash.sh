#!/bin/sh

set -eu

###############################################################################

echo "Please scan the serial number..."
read serno
echo "Serial number read: $serno"

###############################################################################

echo "Please insert the batteries..."
read

###############################################################################

JLinkExe -commanderscript flash.jlink
echo "Recycle power..."
read

###############################################################################

tmux kill-session -t rttlogger || true
tmux new-session -d -s rttlogger "JLinkRTTLogger -Device STM32WLE5CC -If SWD -Speed 1000 -RTTAddress 0x20000800 -RTTChannel 1 ~/.sticker.log"
trap "echo 'Terminating background process'; tmux kill-session -t rttlogger" EXIT

###############################################################################

sleep 2
echo "serialno $serno" | socat - TCP:localhost:19021

###############################################################################

tail -f ~/.sticker.log
