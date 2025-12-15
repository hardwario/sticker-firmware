#!/bin/bash

set -eu

###############################################################################
# Configuration
###############################################################################

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="${SCRIPT_DIR}/../app"
DEPLOY_DIR="${APP_DIR}/deploy"
FIRMWARE_HEX="${DEPLOY_DIR}/sticker-debug.hex"
CSV_FILE="${SCRIPT_DIR}/sticker_sensors.csv"
RTT_LOG="/tmp/sticker_provision.log"
RTT_ADDRESS="0x20000800"

# Parse arguments
DO_FLASH=false
while [[ $# -gt 0 ]]; do
    case $1 in
        --flash)
            DO_FLASH=true
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [--flash]"
            echo "  --flash  Flash firmware before provisioning"
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

###############################################################################
# Functions
###############################################################################

cleanup() {
    echo "Cleaning up..."
    if [ -n "${RTT_PID:-}" ]; then
        kill ${RTT_PID} 2>/dev/null || true
    fi
}

trap cleanup EXIT

###############################################################################
# Main
###############################################################################

echo "=========================================="
echo "HARDWARIO Sticker Provisioning Tool"
echo "=========================================="
echo

# Check firmware exists (only if flashing)
if [ "${DO_FLASH}" = true ] && [ ! -f "${FIRMWARE_HEX}" ]; then
    echo "Error: Firmware not found at ${FIRMWARE_HEX}"
    echo "Please run 'make deploy' first"
    exit 1
fi

# Get serial number
echo "Please scan the serial number..."
read -r SERIAL_NUMBER
echo "Serial number: ${SERIAL_NUMBER}"
echo

# Validate serial number
if [ -z "${SERIAL_NUMBER}" ]; then
    echo "Error: Serial number cannot be empty"
    exit 1
fi

###############################################################################
# Flash firmware (optional)
###############################################################################

if [ "${DO_FLASH}" = true ]; then
    echo "Flashing firmware..."

    # Create temporary JLink script
    JLINK_SCRIPT=$(mktemp)
    cat > "${JLINK_SCRIPT}" <<EOF
r
h
loadfile ${FIRMWARE_HEX}
r
g
exit
EOF

    JLinkExe -device STM32WLE5CC -if SWD -speed 4000 -autoconnect 1 -CommandFile "${JLINK_SCRIPT}"
    FLASH_RESULT=$?
    rm -f "${JLINK_SCRIPT}"

    if [ ${FLASH_RESULT} -ne 0 ]; then
        echo "Error: Failed to flash firmware"
        exit 1
    fi

    echo "Firmware flashed successfully"
    echo
else
    echo "Skipping firmware flash (use --flash to enable)"
    echo
fi

###############################################################################
# Start RTT Logger
###############################################################################

echo "Starting RTT logger..."
rm -f "${RTT_LOG}"

# Reset device to ensure firmware is running
echo "Resetting device..."
JLINK_RESET=$(mktemp)
cat > "${JLINK_RESET}" <<EOF
r
g
exit
EOF
JLinkExe -device STM32WLE5CC -if SWD -speed 4000 -autoconnect 1 -CommandFile "${JLINK_RESET}" > /dev/null
rm -f "${JLINK_RESET}"

sleep 2

# Start RTT logger in background
JLinkRTTLogger -Device STM32WLE5CC -If SWD -Speed 4000 -RTTAddress ${RTT_ADDRESS} -RTTChannel 1 "${RTT_LOG}" &
RTT_PID=$!

# Wait for connection
sleep 5

###############################################################################
# Configure device
###############################################################################

echo "Configuring device..."

# Set serial number
echo "serialno ${SERIAL_NUMBER}" | socat - TCP:localhost:19021
sleep 1

# Enable machine probe
echo "config cap-1w-machine-probe true" | socat - TCP:localhost:19021
sleep 1

# Save configuration
echo "config save" | socat - TCP:localhost:19021
sleep 2

###############################################################################
# Read sensor data
###############################################################################

echo "Reading sensor sample..."
echo "tester sensors sample" | socat - TCP:localhost:19021
sleep 3

echo "Reading sensor serial numbers..."
echo "tester sensors serial" | socat - TCP:localhost:19021
sleep 3

###############################################################################
# Parse results
###############################################################################

echo "Parsing results..."

# Wait for log to be written
sleep 2

# Extract sensor sample data
ORIENTATION=$(grep -o 'orientation:[[:space:]]*[0-9]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9]*$' || echo "N/A")
VOLTAGE=$(grep -o 'voltage:[[:space:]]*[0-9.]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9.]*$' || echo "N/A")
TEMPERATURE=$(grep -o 'temperature:[[:space:]]*[0-9.-]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9.-]*$' || echo "N/A")
HUMIDITY=$(grep -o 'humidity:[[:space:]]*[0-9.]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9.]*$' || echo "N/A")
ILLUMINANCE=$(grep -o 'illuminance:[[:space:]]*[0-9.]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9.]*$' || echo "N/A")
T1_TEMPERATURE=$(grep -o 't1_temperature:[[:space:]]*[0-9.-]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9.-]*$' || echo "N/A")
T2_TEMPERATURE=$(grep -o 't2_temperature:[[:space:]]*[0-9.-]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9.-]*$' || echo "N/A")
ALTITUDE=$(grep -o 'altitude:[[:space:]]*[0-9.-]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9.-]*$' || echo "N/A")
PRESSURE=$(grep -o 'pressure:[[:space:]]*[0-9.]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9.]*$' || echo "N/A")
MP1_TEMPERATURE=$(grep -o 'machine_probe_temp_1:[[:space:]]*[0-9.-]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9.-]*$' || echo "N/A")
MP1_HUMIDITY=$(grep -o 'machine_probe_humidity_1:[[:space:]]*[0-9.]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9.]*$' || echo "N/A")

# Extract serial numbers
SHT40_SERIAL=$(grep -o 'SHT40 serial:[[:space:]]*[0-9]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9]*$' || echo "N/A")
DS18B20_COUNT=$(grep -o 'DS18B20 count:[[:space:]]*[0-9]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9]*$' || echo "0")
MP_COUNT=$(grep -o 'Machine Probe count:[[:space:]]*[0-9]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9]*$' || echo "0")
MP0_SERIAL=$(grep -o 'Machine Probe\[0\] serial:[[:space:]]*[0-9]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9]*$' || echo "N/A")
MP0_SHT_SERIAL=$(grep -o 'Machine Probe\[0\] SHT serial:[[:space:]]*[0-9]*' "${RTT_LOG}" | tail -1 | grep -o '[0-9]*$' || echo "N/A")

###############################################################################
# Write to CSV
###############################################################################

# Create CSV header if file doesn't exist
if [ ! -f "${CSV_FILE}" ]; then
    echo "timestamp,serial_number,sht40_serial,mp0_serial,mp0_sht_serial,ds18b20_count,mp_count,temperature,humidity,voltage,illuminance,t1_temperature,t2_temperature,altitude,pressure,mp1_temperature,mp1_humidity,orientation" > "${CSV_FILE}"
fi

# Append data
TIMESTAMP=$(date -Iseconds)
echo "${TIMESTAMP},${SERIAL_NUMBER},${SHT40_SERIAL},${MP0_SERIAL},${MP0_SHT_SERIAL},${DS18B20_COUNT},${MP_COUNT},${TEMPERATURE},${HUMIDITY},${VOLTAGE},${ILLUMINANCE},${T1_TEMPERATURE},${T2_TEMPERATURE},${ALTITUDE},${PRESSURE},${MP1_TEMPERATURE},${MP1_HUMIDITY},${ORIENTATION}" >> "${CSV_FILE}"

###############################################################################
# Summary
###############################################################################

echo
echo "=========================================="
echo "Provisioning Complete"
echo "=========================================="
echo "Serial Number:      ${SERIAL_NUMBER}"
echo "SHT40 Serial:       ${SHT40_SERIAL}"
echo "MP[0] Serial:       ${MP0_SERIAL}"
echo "MP[0] SHT Serial:   ${MP0_SHT_SERIAL}"
echo "Temperature:        ${TEMPERATURE} C"
echo "Humidity:           ${HUMIDITY} %"
echo "Voltage:            ${VOLTAGE} V"
echo
echo "Results saved to: ${CSV_FILE}"
echo "=========================================="
