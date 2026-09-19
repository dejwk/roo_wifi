load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

cc_library(
    name = "roo_wifi",
    srcs = ["src/roo_wifi/controller.cpp"],
    hdrs = glob(["src/**/*.h"], exclude = ["src/roo_wifi/hal/esp32/**", "src/roo_wifi/esp32.h"]),
    includes = ["src"],
    visibility = ["//visibility:public"],
    deps = ["@roo_collections", "@roo_backport", "@roo_scheduler"],
)

cc_library(
    name = "esp32",
    srcs = glob(["src/roo_wifi/hal/esp32/*.cpp"]),
    hdrs = glob(["src/roo_wifi/hal/esp32/*.h"]) + ["src/roo_wifi/esp32.h"],
    includes = ["src"],
    visibility = ["//visibility:public"],
    deps = [":roo_wifi", "@roo_prefs", "@roo_testing//roo_testing/frameworks/arduino-esp32-2.0.4/libraries/WiFi"],
)

cc_test(
    name = "portable_api_compile_test",
    srcs = ["//test:portable_api_compile_test.cpp"],
    copts = ["-fno-exceptions", "-fno-rtti"],
    deps = [":roo_wifi"],
)
