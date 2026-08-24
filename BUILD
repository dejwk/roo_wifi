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
    includes = [
        "src",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@roo_collections",
        "@roo_prefs",
        "@roo_backport",
        "@roo_scheduler",
        "@roo_testing//roo_testing/frameworks/arduino-esp32-2.0.4/libraries/WiFi",
    ],
)
