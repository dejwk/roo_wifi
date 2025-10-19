cc_library(
    name = "roo_wifi",
    visibility = ["//visibility:public"],
    srcs = glob(
        [
            "src/**/*.cpp",
            "src/**/*.h",
        ],
        exclude = ["test/**"],
    ),
    includes = [
        "src",
    ],
    defines = [
        "ROO_TESTING",
        "ARDUINO=10805",
        "ESP32",
    ],
    deps = [
        "@roo_collections",
        "@roo_prefs",
        "@roo_testing//roo_testing/frameworks/arduino-esp32-2.0.4/libraries/WiFi",
    ],
)
