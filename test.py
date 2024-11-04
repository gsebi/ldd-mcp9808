import time

# Path to the MCP9808 device file
DEVICE_PATH = "/dev/mcp9808"

def read_temperature():
    try:
        # Open the device file and read temperature data
        with open(DEVICE_PATH, "r") as device_file:
            # Read the temperature data
            temp_data = device_file.read().strip()
            
            # Convert the read string into a float
            temperature = float(temp_data)
            
            # Print the temperature with four decimal places
            print(f"Temperature: {temperature:.4f} °C")
    except FileNotFoundError:
        print(f"Error: Device file '{DEVICE_PATH}' not found.")
    except PermissionError:
        print(f"Error: Permission denied. Please check permissions for '{DEVICE_PATH}'.")
    except ValueError:
        print("Error: Could not parse temperature data.")
    except Exception as e:
        print(f"An unexpected error occurred: {e}")

# Set the read interval in seconds (e.g., 0.1 for 100ms)
READ_INTERVAL = 0.1

# Test by reading temperature at the specified interval
if __name__ == "__main__":
    while True:
        read_temperature()
        time.sleep(READ_INTERVAL)

