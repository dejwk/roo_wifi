load("@rules_cc//cc:cc_library.bzl", "cc_library")

cc_library(
    name = "roo_wifi",
    srcs = glob(
        [
            "src/**/*.cpp",
            "src/**/*.h",
        ],
        exclude = ["test/**"],
    ),
    defines = [
        "ROO_TESTING",
        "ARDUINO=10805",
        "ESP32",
    ],
    includes = [
        "src",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@roo_collections",
        "@roo_prefs",
        "@roo_testing//roo_testing/frameworks/arduino-esp32-2.0.4/libraries/WiFi",
    ],
)
