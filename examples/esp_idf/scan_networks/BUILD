load("//examples:example_build.bzl", "roo_wifi_esp_idf_example")

roo_wifi_esp_idf_example(
    name = "scan_networks",
    srcs = ["scan_networks.cpp"],
    deps = [
        "@roo_testing//roo_testing/microcontrollers/esp32",
        "@roo_testing//roo_testing/transducers/wifi",
    ],
)
