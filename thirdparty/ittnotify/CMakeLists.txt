#
# Copyright (C) 2019 Intel Corporation
#
# SPDX-License-Identifier: MIT
#

add_library(libittprofiler INTERFACE)
if (X86_ENV)
    if (NOT WIN32)
        target_include_directories(libittprofiler INTERFACE
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ittnotify_x86/include>
                $<INSTALL_INTERFACE:ittnotify_x86/include>)
        target_link_libraries(libittprofiler INTERFACE  ${CMAKE_CURRENT_SOURCE_DIR}/ittnotify_x86/lib/libittnotify.a dl pthread)
        #install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/ittnotify_x86/lib/libittnotify_collector.so DESTINATION lib COMPONENT HddlUnite)
    else ()
        target_include_directories(libittprofiler INTERFACE
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ittnotify_win/include>
                $<INSTALL_INTERFACE:ittnotify_win/include>)
        target_link_libraries(libittprofiler INTERFACE  ${CMAKE_CURRENT_SOURCE_DIR}/ittnotify_win/lib/libittnotify.lib)
        #install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/ittnotify_win/lib/ittnotify_collector.dll DESTINATION lib)
    endif ()
else ()
    #target_include_directories(libittprofiler INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/ittnotify_arm/include)
    #target_link_libraries(libittprofiler INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/ittnotify_arm/lib/libittnotify.a dl pthread)
    #install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/ittnotify_arm/lib/libittnotify_collector.so DESTINATION lib COMPONENT device_service)
endif ()
