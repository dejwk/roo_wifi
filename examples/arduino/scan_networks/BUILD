load("//examples:example_build.bzl", "roo_wifi_arduino_example")

roo_wifi_arduino_example(
    name = "scan_networks",
    sketch = "scan_networks.ino",
    deps = [
        "@roo_testing//roo_testing/microcontrollers/esp32",
        "@roo_testing//roo_testing/transducers/wifi",
    ],
)
