load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

package(default_visibility = ["//:__pkg__"])

TEST_MAIN_DEPS = select({
    "@roo_testing//roo_testing/platforms:is_arduino": ["@roo_testing//:arduino_gtest_main"],
    "//conditions:default": ["@googletest//:gtest_main"],
})

exports_files(["portable_api_compile_test.cpp"])

cc_test(
    name = "controller_test",
    size = "small",
    srcs = [
        "backend_fakes.h",
        "controller_test.cpp",
    ],
    deps = [
        "//:roo_wifi",
        "@googletest//:gtest",
    ] + TEST_MAIN_DEPS,
)

cc_test(
    name = "esp32_backend_test",
    size = "small",
    srcs = [
        "backend_fakes.h",
        "esp32_backend_test.cpp",
    ],
    target_compatible_with = [
        "@roo_testing//roo_testing/platforms:arduino",
    ],
    deps = [
        "//:esp32",
        "@googletest//:gtest",
        "@roo_testing//:arduino_gtest_main",
        "@roo_testing//roo_testing/microcontrollers/esp32",
        "@roo_testing//roo_testing/transducers/wifi",
    ],
)

cc_test(
    name = "backend_resource_test",
    size = "small",
    srcs = [
        "backend_fakes.h",
        "backend_resource_test.cpp",
    ],
    deps = [
        "//:roo_wifi",
        "@googletest//:gtest",
    ] + TEST_MAIN_DEPS,
)

cc_library(
    name = "test_support",
    testonly = True,
    hdrs = ["backend_fakes.h"],
    includes = ["."],
    visibility = ["//visibility:public"],
    deps = ["//:roo_wifi"],
)

[
    cc_test(
        name = name,
        size = "small",
        srcs = [
            "backend_fakes.h",
            "controller_test.cpp",
        ],
        args = ["--gtest_filter=" + tests],
        deps = [
            "//:roo_wifi",
            "@googletest//:gtest",
        ] + TEST_MAIN_DEPS,
    )
    for name, tests in [
        ("configuration_store_test", "StoreTest.*"),
        ("configuration_controller_test", "StartupTest.*:TimeoutTest.*:OwnershipTest.*:ConfigurationTest.*:BackendTest.IndependentSlotsAndCancelledSave:BackendTest.EnablePersistenceFailure:BackendTest.TemporaryConnectionDoesNotPersist:BackendTest.SavedProfileSurvivesNativeRejection"),
        ("interface_lifecycle_test", "BackendTest.OwnedInputAndAddressReadiness:BackendTest.OrderedSwitchWithDelayedDispatch:BackendTest.CancelBeforeAssociationAndSameSsidRetry:BackendTest.SnapshotLifetimeAndMetadata:BackendTest.ShutdownNeutralizesQueuedEvents:BackendTest.CancelBeforeNativeStart:BackendTest.NativeHandoffOverflowFailsClosed"),
    ]
]
