# Updater cleanup regression test

The test runs the production cleanup and SFO parser on the host using a built
PS4Hub v1.29 PKG as a fixture. It creates a new, disposable directory and rejects
an existing directory. It verifies that a running 1.28 preserves a pending 1.29,
that matching older/current versions are removed, and that malformed packages, other title IDs,
version mismatches, partial downloads, nested files and unrelated files are retained.
Version comparison is numeric, including equivalent 2.0 and 2.00.

From `ps4hub/`, with a Windows C++17 compiler and MSVC/Windows SDK installed:

```powershell
clang++ -std=c++17 -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH -Isrc src/update_cleanup.cpp src/sfo_parser.cpp tests/update_cleanup_test.cpp -o build/update_cleanup_test.exe
./build/update_cleanup_test.exe ./ps4hub.pkg ./build/cleanup-test-fixtures
```

Use a new fixture-directory name for each run. The compiler compatibility define
allows the bundled Clang 18 to use the installed newer MSVC STL for this host test.
It is not used for PS4 builds.

Console flow: run the test build (APP_VER and APP_VERSION 1.28), download 1.29,
verify that installation instructions include GoldHEN Package Source/all drives,
restart 1.28 and confirm the 1.29 PKG is retained, install 1.29 manually, then
launch it and confirm `/data/pkg/PS4Hub-v1.29.pkg` and older matching PS4Hub updater packages have been removed. Automatic
installation APIs must not be invoked by the updater.
