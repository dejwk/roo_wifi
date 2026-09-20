# Running the examples

Every checked-in example is a native, runnable Bazel target backed by the
`roo_testing` emulator. Run an Arduino sketch from the `roo_wifi` workspace
root using its directory hierarchy and sketch basename:

```sh
bazel run //examples/arduino/scan_networks
```

The ESP-IDF example uses the same `roo_wifi` controller and ESP-IDF radio
adapter without the Arduino frontend:

```sh
bazel run //examples/esp_idf/scan_networks \
    --config=roo_testing_idf_esp32
```

Build every example without opening the interactive emulator with:

```sh
bazel build //examples:all_arduino_example_builds
bazel build //examples:all_esp_idf_example_builds \
    --config=roo_testing_idf_esp32
```

Both examples show the controller lifecycle required to scan safely: register
a listener, call `begin()`, wait for the physical station to be enabled, then
request a scan and read the borrowed snapshot in `onScanChanged()`.
