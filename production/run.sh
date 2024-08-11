#!/bin/sh

set -eu

# Text Colors
BLACK='\e[30m'
RED='\e[31m'
GREEN='\e[32m'
YELLOW='\e[33m'
BLUE='\e[34m'
MAGENTA='\e[35m'
CYAN='\e[36m'
WHITE='\e[37m'

# Bright (Bold) Text Colors
BRIGHT_BLACK='\e[90m'
BRIGHT_RED='\e[91m'
BRIGHT_GREEN='\e[92m'
BRIGHT_YELLOW='\e[93m'
BRIGHT_BLUE='\e[94m'
BRIGHT_MAGENTA='\e[95m'
BRIGHT_CYAN='\e[96m'
BRIGHT_WHITE='\e[97m'

# Background Colors
BG_BLACK='\e[40m'
BG_RED='\e[41m'
BG_GREEN='\e[42m'
BG_YELLOW='\e[43m'
BG_BLUE='\e[44m'
BG_MAGENTA='\e[45m'
BG_CYAN='\e[46m'
BG_WHITE='\e[47m'

# Bright Background Colors
BG_BRIGHT_BLACK='\e[100m'
BG_BRIGHT_RED='\e[101m'
BG_BRIGHT_GREEN='\e[102m'
BG_BRIGHT_YELLOW='\e[103m'
BG_BRIGHT_BLUE='\e[104m'
BG_BRIGHT_MAGENTA='\e[105m'
BG_BRIGHT_CYAN='\e[106m'
BG_BRIGHT_WHITE='\e[107m'

# Text Styles
RESET='\e[0m'
BOLD='\e[1m'
DIM='\e[2m'
ITALIC='\e[3m'
UNDERLINE='\e[4m'
BLINK='\e[5m'
REVERSE='\e[7m'
HIDDEN='\e[8m'
STRIKE='\e[9m'

echo -e "${CYAN}*******************************************************************************${RESET}"
echo -e "${CYAN}Scan serial number...${RESET}"
read serial_number
echo -e "${GREEN}Serial number read:${RESET} $serial_number"

echo
echo -e "${CYAN}*******************************************************************************${RESET}"
echo -e "${CYAN}Connect J-Link and insert batteries...${RESET}"
echo -e "${CYAN}*******************************************************************************${RESET}"

read

echo -e "${YELLOW}*******************************************************************************${RESET}"
echo -e "${YELLOW}Flashing the device (debug)...${RESET}"
echo -e "${YELLOW}*******************************************************************************${RESET}"

JLinkExe -commanderscript flash-debug.jlink

echo -e "${CYAN}*******************************************************************************${RESET}"
echo -e "${CYAN}Recycle power...${RESET}"
echo -e "${CYAN}*******************************************************************************${RESET}"

read

echo -e "${YELLOW}*******************************************************************************${RESET}"
echo -e "${YELLOW}Starting RTT session...${RESET}"
echo -e "${YELLOW}*******************************************************************************${RESET}"

echo -e "${CYAN}Start this command in another terminal:${RESET}"
echo -e "${BLUE}JLinkRTTLogger -Device STM32WLE5CC -If SWD -Speed 1000 -RTTAddress 0x20000800 -RTTChannel 1 ~/.sticker.log${RESET}"

sleep 2

zsh -c "source ~/.zshrc && setsid stdbuf -oL JLinkRTTLogger -Device STM32WLE5CC -If SWD -Speed 1000 -RTTAddress 0x20000800 -RTTChannel 1 ~/.sticker.log >~/.JLink.log 2>&1 &"

echo
echo -e "${CYAN}Watch the log:${RESET}"
echo -e "${BLUE}tail -f ~/.sticker.log${RESET}"
echo
echo -e "${CYAN}All good?${RESET}"
read

echo -e "${GREEN}Great!${RESET}"
echo

sleep 2

echo -e "${YELLOW}*******************************************************************************${RESET}"
echo -e "${YELLOW}Programming parameters...${RESET}"
echo -e "${YELLOW}*******************************************************************************${RESET}"

echo -e "config serial-number $serial_number" | socat - TCP:localhost:19021

row=$(csvgrep -c "Serial number" -m "$serial_number" devices.csv)

offset_temp=$(echo -e "$row" | csvcut -c "Offset / Temperature" | sed 1d | sed 's/^""$//')
offset_ext_temp1=$(echo -e "$row" | csvcut -c "Offset / Ext. temperature 1" | sed 1d | sed 's/^""$//')
offset_ext_temp2=$(echo -e "$row" | csvcut -c "Offset / Ext. temperature 2" | sed 1d | sed 's/^""$//')
dev_eui=$(echo -e "$row" | csvcut -c "LoRaWAN / DevEUI" | sed 1d | sed 's/^""$//')
dev_addr=$(echo -e "$row" | csvcut -c "LoRaWAN / DevAddr" | sed 1d | sed 's/^""$//')
app_skey=$(echo -e "$row" | csvcut -c "LoRaWAN / AppSKey" | sed 1d | sed 's/^""$//')
nwk_skey=$(echo -e "$row" | csvcut -c "LoRaWAN / NwkSKey" | sed 1d | sed 's/^""$//')

if [ -n "$offset_temp" ]; then
    echo -e "Offset / Temperature: $offset_temp"
    echo -e "config corr-temperature $offset_temp" | socat - TCP:localhost:19021
    sleep 0.2
fi
if [ -n "$offset_ext_temp1" ]; then
    echo -e "Offset / Ext. temperature 1: $offset_ext_temp1"
    echo -e "config corr-ext-temperature-1 $offset_ext_temp1" | socat - TCP:localhost:19021
    sleep 0.2
fi
if [ -n "$offset_ext_temp2" ]; then
    echo -e "Offset / Ext. temperature 2: $offset_ext_temp2"
    echo -e "config corr-ext-temperature-1 $offset_ext_temp2" | socat - TCP:localhost:19021
    sleep 0.2
fi
if [ -n "$dev_eui" ]; then
    echo -e "LoRaWAN / DevEUI: $dev_eui"
    echo -e "config lrw-deveui $dev_eui" | socat - TCP:localhost:19021
    sleep 0.2
fi
if [ -n "$dev_addr" ]; then
    echo -e "LoRaWAN / DevAddr: $dev_addr"
    echo -e "config lrw-devaddr $dev_addr" | socat - TCP:localhost:19021
    sleep 0.2
fi
if [ -n "$nwk_skey" ]; then
    echo -e "LoRaWAN / NwkSKey: $nwk_skey"
    echo -e "config lrw-nwkskey $nwk_skey" | socat - TCP:localhost:19021
    sleep 0.2
fi
if [ -n "$app_skey" ]; then
    echo -e "LoRaWAN / AppSKey: $app_skey"
    echo -e "config lrw-appskey $app_skey" | socat - TCP:localhost:19021
    sleep 0.2
fi

echo -e "config save" | socat - TCP:localhost:19021
sleep 0.2

echo
echo
echo -e "${YELLOW}*******************************************************************************${RESET}"
echo -e "${YELLOW}Killing JlinkRTTLogger...${RESET}"
echo -e "${YELLOW}*******************************************************************************${RESET}"

pkill JLinkRTTLogger

echo
echo -e "${GREEN}JLinkRTTLogger is dead! ${RESET}"
echo

echo -e "${YELLOW}*******************************************************************************${RESET}"
echo -e "${YELLOW}Flashing the device (release)...${RESET}"
echo -e "${YELLOW}*******************************************************************************${RESET}"

JLinkExe -commanderscript flash-release.jlink

echo -e "${YELLOW}*******************************************************************************${RESET}"
echo -e "${YELLOW}Serial number read:${RESET} $serial_number"

if [ -n "$offset_temp" ]; then
    echo -e "${GREEN}Offset / Temperature:${RESET} $offset_temp"
else
    echo -e "${RED}Offset / Temperature Nonexistent!    ${RESET}"
fi
if [ -n "$offset_ext_temp1" ]; then
    echo -e "${GREEN}Offset / Ext. temperature 1:${RESET} $offset_ext_temp1"
else
    echo -e "${RED}Offset / Ext. temperature 1 Nonexistent! ${RESET}"
fi
if [ -n "$offset_ext_temp2" ]; then
    echo -e "${GREEN}Offset / Ext. temperature 2:${RESET} $offset_ext_temp2"
else
    echo -e "${RED}Offset / Ext. temperature 2 Nonexistent! ${RESET}"
fi
if [ -n "$dev_eui" ]; then
    echo -e "${GREEN}LoRaWAN / DevEUI:${RESET} $dev_eui"
else
    echo -e "${RED}LoRaWAN / DevEUI Nonexistent! ${RESET}"
fi
if [ -n "$dev_addr" ]; then
    echo -e "${GREEN}LoRaWAN / DevAddr:${RESET} $dev_addr"
else
    echo -e "${RED}LoRaWAN / DevAddr Nonextsitent! ${RESET}"
fi
if [ -n "$nwk_skey" ]; then
    echo -e "${GREEN}LoRaWAN / NwkSKey:${RESET} $nwk_skey"
else
    echo -e "${RED}LoRaWAN / NwkSKey Nonexistent! ${RESET}"
fi
if [ -n "$app_skey" ]; then
    echo -e "${GREEN}LoRaWAN / AppSKey:${RESET} $app_skey"
else
    echo -e "${RED}LoRaWAN / AppSKey Nonexistent! ${RESET}"
fi
echo -e "${YELLOW}*******************************************************************************${RESET}"

echo -e "${GREEN}Successfully finished${RESET}"
