cmake_minimum_required(VERSION 3.5)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

project(VitaRecorder C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

# No libc / heap: everything uses the system's built-in sceClib* functions and memblocks
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wl,-q -Wall -O2 -std=gnu11 -fno-builtin-printf -nostdlib")

add_executable(VitaRecorder
  main.c
  renderer.c
  encoder.c
  rescaler.c
  mp4.c
  libc_shim.c
)

target_link_libraries(VitaRecorder
  taihen_stub
  SceLibKernel_stub
  SceKernelThreadMgr_stub
  SceIofilemgr_stub
  SceSysmem_stub
  SceDisplay_stub
  SceCtrl_stub
  SceRtc_stub
  SceAppMgr_stub
  SceJpegEnc_stub_weak
  ScePower_stub_weak
  gcc
)

set_target_properties(VitaRecorder PROPERTIES LINK_FLAGS "-nostdlib")

vita_create_self(VitaRecorder.suprx VitaRecorder
  CONFIG ${CMAKE_SOURCE_DIR}/VitaRecorder.yml
)
