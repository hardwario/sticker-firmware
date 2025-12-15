#!/bin/bash

set -eu

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CSV_FILE="${SCRIPT_DIR}/sticker_sensors.csv"

echo "=========================================="
echo "HARDWARIO Sticker Sensor Logger"
echo "=========================================="
echo

# Get serial number
echo "Please enter the serial number:"
read -r SERIAL_NUMBER

if [ -z "${SERIAL_NUMBER}" ]; then
    echo "Error: Serial number cannot be empty"
    exit 1
fi

echo
echo "Paste sensor sample data (end with empty line):"
SAMPLE_DATA=""
while IFS= read -r line; do
    [ -z "$line" ] && break
    SAMPLE_DATA="${SAMPLE_DATA}${line}"$'\n'
done

echo
echo "Paste sensor serial data (end with empty line):"
SERIAL_DATA=""
while IFS= read -r line; do
    [ -z "$line" ] && break
    SERIAL_DATA="${SERIAL_DATA}${line}"$'\n'
done

# Parse sensor sample data
TEMPERATURE=$(echo "$SAMPLE_DATA" | grep -o 'temperature:[[:space:]]*[0-9.-]*' | head -1 | grep -o '[0-9.-]*$' || echo "N/A")
HUMIDITY=$(echo "$SAMPLE_DATA" | grep -o 'humidity:[[:space:]]*[0-9.]*' | head -1 | grep -o '[0-9.]*$' || echo "N/A")
MP1_TEMPERATURE=$(echo "$SAMPLE_DATA" | grep -o 'machine_probe_temp_1:[[:space:]]*[0-9.-]*' | grep -o '[0-9.-]*$' || echo "N/A")
MP1_HUMIDITY=$(echo "$SAMPLE_DATA" | grep -o 'machine_probe_humidity_1:[[:space:]]*[0-9.]*' | grep -o '[0-9.]*$' || echo "N/A")

# Parse serial numbers
SHT40_SERIAL=$(echo "$SERIAL_DATA" | grep -o 'SHT40 serial:[[:space:]]*[0-9]*' | grep -o '[0-9]*$' || echo "N/A")
MP0_SERIAL=$(echo "$SERIAL_DATA" | grep -o 'Machine Probe\[0\] serial:[[:space:]]*[0-9]*' | grep -o '[0-9]*$' || echo "N/A")
MP0_SHT_SERIAL=$(echo "$SERIAL_DATA" | grep -o 'Machine Probe\[0\] SHT serial:[[:space:]]*[0-9]*' | grep -o '[0-9]*$' || echo "N/A")

# Create CSV header if file doesn't exist
if [ ! -f "${CSV_FILE}" ]; then
    echo "timestamp,serial_number,sht40_serial,mp0_serial,mp0_sht_serial,temperature,humidity,mp1_temperature,mp1_humidity" > "${CSV_FILE}"
fi

# Append data
TIMESTAMP=$(date -Iseconds)
echo "${TIMESTAMP},${SERIAL_NUMBER},${SHT40_SERIAL},${MP0_SERIAL},${MP0_SHT_SERIAL},${TEMPERATURE},${HUMIDITY},${MP1_TEMPERATURE},${MP1_HUMIDITY}" >> "${CSV_FILE}"

# Summary
echo
echo "=========================================="
echo "Data Logged"
echo "=========================================="
echo "Serial Number:      ${SERIAL_NUMBER}"
echo "SHT40 Serial:       ${SHT40_SERIAL}"
echo "MP[0] Serial:       ${MP0_SERIAL}"
echo "MP[0] SHT Serial:   ${MP0_SHT_SERIAL}"
echo "Temperature:        ${TEMPERATURE} C"
echo "Humidity:           ${HUMIDITY} %"
echo "MP1 Temperature:    ${MP1_TEMPERATURE} C"
echo "MP1 Humidity:       ${MP1_HUMIDITY} %"
echo
echo "Results saved to: ${CSV_FILE}"
echo "=========================================="
