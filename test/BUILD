load("@rules_cc//cc:cc_test.bzl", "cc_test")
exports_files(["portable_api_compile_test.cpp"])
cc_test(
    name = "controller_test",
    size = "small",
    srcs = ["controller_test.cpp", "backend_fakes.h"],
    deps = ["//:roo_wifi", "@googletest//:gtest_main"],
)
cc_test(
    name = "esp32_backend_test",
    size = "small",
    srcs = ["esp32_backend_test.cpp", "backend_fakes.h"],
    deps = [
        "//:esp32", "@googletest//:gtest",
        "@roo_testing//:arduino_gtest_main",
        "@roo_testing//roo_testing/microcontrollers/esp32",
        "@roo_testing//roo_testing/transducers/wifi",
    ],
)
load("@rules_cc//cc:cc_library.bzl", "cc_library")
cc_library(
    name = "test_support",
    testonly = True,
    hdrs = ["backend_fakes.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = ["//:roo_wifi"],
)
