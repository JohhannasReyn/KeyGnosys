cmake_minimum_required(VERSION 3.20)
project(mousetrapkeys VERSION 0.1.0 LANGUAGES CXX)

# enable_testing() must be called in the *top-level* list file, not only in the
# subdirectory that registers the tests. Without it here, CTest generates no
# root CTestTestfile.cmake and reports "No tests were found" even though the
# test executables built and pass when run by hand.
enable_testing()

# The Python overlay is installed with pip and is not part of this build; see
# pyproject.toml. CMake covers the native core only.
add_subdirectory(core)
