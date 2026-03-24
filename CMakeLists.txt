cmake_minimum_required(VERSION 3.18)
project(immumap LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(pybind11 REQUIRED)
pybind11_add_module(_core src/main.cpp)

install(TARGETS _core DESTINATION immumap)
