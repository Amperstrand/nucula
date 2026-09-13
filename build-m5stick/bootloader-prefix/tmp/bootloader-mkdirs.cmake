# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

cmake_minimum_required(VERSION 3.5)

file(MAKE_DIRECTORY
  "/home/ubuntu/esp/esp-idf/components/bootloader/subproject"
  "/home/ubuntu/src/nucula/build-m5stick/bootloader"
  "/home/ubuntu/src/nucula/build-m5stick/bootloader-prefix"
  "/home/ubuntu/src/nucula/build-m5stick/bootloader-prefix/tmp"
  "/home/ubuntu/src/nucula/build-m5stick/bootloader-prefix/src/bootloader-stamp"
  "/home/ubuntu/src/nucula/build-m5stick/bootloader-prefix/src"
  "/home/ubuntu/src/nucula/build-m5stick/bootloader-prefix/src/bootloader-stamp"
)

set(configSubDirs )
foreach(subDir IN LISTS configSubDirs)
    file(MAKE_DIRECTORY "/home/ubuntu/src/nucula/build-m5stick/bootloader-prefix/src/bootloader-stamp/${subDir}")
endforeach()
if(cfgdir)
  file(MAKE_DIRECTORY "/home/ubuntu/src/nucula/build-m5stick/bootloader-prefix/src/bootloader-stamp${cfgdir}") # cfgdir has leading slash
endif()
