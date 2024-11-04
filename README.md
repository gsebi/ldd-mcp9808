# MCP9808 Temperature Sensor Driver

## Requirements
- Linux kernel headers installed on your system. Install using:
  ```bash
  sudo apt update
  sudo apt install raspberrypi-kernel-headers

- Add dudev rule e.g.:
 ```bash
 sudo nano /etc/udev/rules.d/99-mcp9808.rules

  ```SUBSYSTEM=="i2c", KERNEL=="mcp9808", MODE="0660", GROUP="i2cgroup"

