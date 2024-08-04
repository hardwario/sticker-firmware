#!/bin/sh

set -eu

echo "*******************************************************************************"
echo "Scan serial number..."
read serial_number
echo "Serial number read: $serial_number"

echo "*******************************************************************************"
echo "Connect J-Link and insert batteries..."
echo "*******************************************************************************"

read

echo "*******************************************************************************"
echo "Flashing the device (debug)..."
echo "*******************************************************************************"

JLinkExe -commanderscript flash-debug.jlink

echo "*******************************************************************************"
echo "Recycle power..."
echo "*******************************************************************************"

read

echo "*******************************************************************************"
echo "Starting RTT session..."
echo "*******************************************************************************"

echo "Start this command in a separate terminal:"
echo "JLinkRTTLogger -Device STM32WLE5CC -If SWD -Speed 1000 -RTTAddress 0x20000800 -RTTChannel 1 ~/.sticker.log"
echo
echo "Then watch logs:"
echo "tail -f ~/.sticker.log"
echo
echo "All good?"
read

sleep 2

echo "*******************************************************************************"
echo "Programming parameters..."
echo "*******************************************************************************"

echo "config serial-number $serial_number" | socat - TCP:localhost:19021

row=$(csvgrep -c "Serial number" -m "$serial_number" devices.csv)

offset_temp=$(echo "$row" | csvcut -c "Offset / Temperature" | sed 1d | sed 's/^""$//')
offset_ext_temp1=$(echo "$row" | csvcut -c "Offset / Ext. temperature 1" | sed 1d | sed 's/^""$//')
offset_ext_temp2=$(echo "$row" | csvcut -c "Offset / Ext. temperature 2" | sed 1d | sed 's/^""$//')
dev_eui=$(echo "$row" | csvcut -c "LoRaWAN / DevEUI" | sed 1d | sed 's/^""$//')
dev_addr=$(echo "$row" | csvcut -c "LoRaWAN / DevAddr" | sed 1d | sed 's/^""$//')
app_skey=$(echo "$row" | csvcut -c "LoRaWAN / AppSKey" | sed 1d | sed 's/^""$//')
nwk_skey=$(echo "$row" | csvcut -c "LoRaWAN / NwkSKey" | sed 1d | sed 's/^""$//')

if [ -n "$offset_temp" ]; then
    echo "Offset / Temperature: $offset_temp"
    echo "config corr-temperature $offset_temp" | socat - TCP:localhost:19021
    sleep 0.2
fi
if [ -n "$offset_ext_temp1" ]; then
    echo "Offset / Ext. temperature 1: $offset_ext_temp1"
    echo "config corr-ext-temperature-1 $offset_ext_temp1" | socat - TCP:localhost:19021
    sleep 0.2
fi
if [ -n "$offset_ext_temp2" ]; then
    echo "Offset / Ext. temperature 2: $offset_ext_temp2"
    echo "config corr-ext-temperature-1 $offset_ext_temp2" | socat - TCP:localhost:19021
    sleep 0.2
fi
if [ -n "$dev_eui" ]; then
    echo "LoRaWAN / DevEUI: $dev_eui"
    echo "config lrw-deveui $dev_eui" | socat - TCP:localhost:19021
    sleep 0.2
fi
if [ -n "$dev_addr" ]; then
    echo "LoRaWAN / DevAddr: $dev_addr"
    echo "config lrw-devaddr $dev_addr" | socat - TCP:localhost:19021
    sleep 0.2
fi
if [ -n "$nwk_skey" ]; then
    echo "LoRaWAN / NwkSKey: $nwk_skey"
    echo "config lrw-nwkskey $nwk_skey" | socat - TCP:localhost:19021
    sleep 0.2
fi
if [ -n "$app_skey" ]; then
    echo "LoRaWAN / AppSKey: $app_skey"
    echo "config lrw-appskey $app_skey" | socat - TCP:localhost:19021
    sleep 0.2
fi

echo "config save" | socat - TCP:localhost:19021
sleep 0.2

echo "*******************************************************************************"
echo "Kill JLinkLogger and hit <ENTER> to continue..."
echo "*******************************************************************************"

read

echo "*******************************************************************************"
echo "Flashing the device (release)..."
echo "*******************************************************************************"

JLinkExe -commanderscript flash-release.jlink

echo "*******************************************************************************"
echo "Serial number read: $serial_number"

if [ -n "$offset_temp" ]; then
    echo "Offset / Temperature: $offset_temp"
fi
if [ -n "$offset_ext_temp1" ]; then
    echo "Offset / Ext. temperature 1: $offset_ext_temp1"
fi
if [ -n "$offset_ext_temp2" ]; then
    echo "Offset / Ext. temperature 2: $offset_ext_temp2"
fi
if [ -n "$dev_eui" ]; then
    echo "LoRaWAN / DevEUI: $dev_eui"
fi
if [ -n "$dev_addr" ]; then
    echo "LoRaWAN / DevAddr: $dev_addr"
fi
if [ -n "$nwk_skey" ]; then
    echo "LoRaWAN / NwkSKey: $nwk_skey"
fi
if [ -n "$app_skey" ]; then
    echo "LoRaWAN / AppSKey: $app_skey"
fi
echo "*******************************************************************************"

echo "Successfully finished"
