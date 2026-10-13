# The following four lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-peer)

# Uncomment if using usrsctp.
#idf_build_set_property(COMPILE_OPTIONS "-DHAVE_USRSCTP" APPEND)

# libsrtp mixes unsigned int * and uint32_t * (unsigned long with the ESP toolchains), both are
# 32 bit, but GCC 14 and later (ESP-IDF 6) reject that by default
if(TARGET __idf_sepfy__srtp)
  target_compile_options(__idf_sepfy__srtp PRIVATE -Wno-incompatible-pointer-types)
endif()
