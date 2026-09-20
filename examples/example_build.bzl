"""roo_wifi defaults for runnable Arduino and ESP-IDF examples."""

load("@roo_testing//roo_testing/emulation:arduino.bzl", "roo_arduino_example")
load("@roo_testing//roo_testing/emulation:esp_idf.bzl", "roo_esp_idf_example")

_ESP32_WIFI_DEPS = [
    "//:esp32",
]

_ESP_IDF_EXAMPLE_DEPS = _ESP32_WIFI_DEPS + [
    "@roo_testing//roo_testing/frameworks/esp-idf:core",
]

def roo_wifi_arduino_example(
        name,
        sketch,
        deps = [],
        visibility = ["//visibility:public"],
        **kwargs):
    """Creates a runnable Arduino roo_wifi example."""
    roo_arduino_example(
        name = name,
        sketch = sketch,
        deps = _ESP32_WIFI_DEPS + deps,
        visibility = visibility,
        **kwargs
    )

def roo_wifi_esp_idf_example(
        name,
        srcs,
        deps = [],
        visibility = ["//visibility:public"],
        **kwargs):
    """Creates a runnable native ESP-IDF roo_wifi example."""
    roo_esp_idf_example(
        name = name,
        srcs = srcs,
        deps = _ESP_IDF_EXAMPLE_DEPS + deps,
        visibility = visibility,
        **kwargs
    )
