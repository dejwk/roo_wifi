# Backend migration

Include `roo_wifi.h` for portable construction with caller-owned Interface,
Store and Scheduler dependencies. Include `roo_wifi/esp32.h` explicitly for
ESP32 convenience construction. Bazel consumers of that adapter depend on
`@roo_wifi//:esp32`; `@roo_wifi` contains only the portable core.

SDK-free validation (bypass the workspace's default Arduino wrapper):

```
BAZELISK_SKIP_WRAPPER=true bazel test //:portable_api_compile_test //test:controller_test --copt=-DROO_THREADS_USE_CPPSTD --copt=-fno-exceptions --copt=-fno-rtti
bazel build //:esp32
```

The host graph selects standard C++ threading. Platform constraint labels may
appear in `cquery deps(...)`, but no Arduino/ESP-IDF implementation is linked.
