import csv
import random
import string

def generate_hex_string(length):
    return ''.join(random.choice('0123456789ABCDEF') for _ in range(length))

def generate_lorawan_devices(num_devices):
    devices = []
    for _ in range(num_devices):
        device = {
            'Serial number': '',
            'DevEUI': generate_hex_string(16),
            'DevAddr': generate_hex_string(8),
            'AppSKey': generate_hex_string(32),
            'NwkSKey': generate_hex_string(32)
        }
        devices.append(device)
    return devices

def save_to_csv(devices, filename):
    with open(filename, mode='w', newline='') as file:
        writer = csv.DictWriter(file, fieldnames=devices[0].keys())
        writer.writeheader()
        writer.writerows(devices)

def main():
    num_devices = 200
    filename = 'lorawan_devices.csv'
    devices = generate_lorawan_devices(num_devices)
    save_to_csv(devices, filename)
    print(f"{num_devices} LoRaWAN devices have been saved to {filename}")

if __name__ == "__main__":
    main()
