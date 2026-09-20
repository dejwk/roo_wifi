load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

cc_library(
    name = "roo_wifi",
    srcs = glob(["src/roo_wifi/*.cpp", "src/roo_wifi/hal/*.cpp"]),
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
    deps = [":roo_wifi", "@roo_prefs", "@roo_testing//roo_testing/frameworks/esp-idf:core"],
)

cc_test(
    name = "portable_api_compile_test",
    srcs = ["//test:portable_api_compile_test.cpp"],
    copts = [
        "-fno-exceptions",
        "-fno-rtti",
        "-UARDUINO",
        "-UESP32",
        "-UESP_PLATFORM",
    ],
    deps = [":roo_wifi"],
)

[
    test_suite(name = name, tests = ["//test:" + name])
    for name in [
        "configuration_store_test",
        "configuration_controller_test",
        "interface_lifecycle_test",
        "backend_resource_test",
    ]
]

test_suite(name = "configuration_interface_test", tests = ["//test:esp32_backend_test"])

test_suite(
    name = "interface_conformance_test",
    tests = ["//test:controller_test", "//test:esp32_backend_test"],
)
