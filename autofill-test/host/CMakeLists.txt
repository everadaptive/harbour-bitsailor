# SPDX-FileCopyrightText: 2026 Zach Dunton
# SPDX-License-Identifier: MIT
#
# Host build of the autofill provider of BitSailor on a stub core, for the
# tests in this directory. It needs Qt6 (Core, DBus) and qdbusxml2cpp. The
# real app builds with qmake for Sailfish OS.
cmake_minimum_required(VERSION 3.16)
project(bitsailor-autofill-host CXX)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 20)
find_package(Qt6 REQUIRED COMPONENTS Core DBus)

# The sources include "../bitsailorcore.h". Stage a copy of them next to the
# stub, so that this include finds the stub and not the real header.
set(REPO ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(STAGE ${CMAKE_BINARY_DIR}/stage/src)
# The app builds with Qt 5.6. Qt 6 spells the split option Qt::SkipEmptyParts.
function(stage from to)
    file(READ ${from} content)
    string(REPLACE "QString::SkipEmptyParts" "Qt::SkipEmptyParts" content "${content}")
    file(WRITE ${to} "${content}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${from})
endfunction()
foreach(f autofillprovider.cpp autofillprovider.h autofillsave.cpp autofillsave.h
          autofillpickerservice.cpp autofillpickerservice.h urimatcher.cpp urimatcher.h
          candidatetypes.h tech.everadaptive.Autofill1.Provider.xml)
    stage(${REPO}/src/autofill/${f} ${STAGE}/autofill/${f})
endforeach()
foreach(f appsettings.cpp appsettings.h consts.h)
    stage(${REPO}/src/${f} ${STAGE}/${f})
endforeach()
stage(${CMAKE_CURRENT_SOURCE_DIR}/stub/bitsailorcore.h ${STAGE}/bitsailorcore.h)

set(ADAPTOR_DIR ${CMAKE_BINARY_DIR}/adaptor)
file(MAKE_DIRECTORY ${ADAPTOR_DIR})
add_custom_command(
    OUTPUT ${ADAPTOR_DIR}/provideradaptor.h ${ADAPTOR_DIR}/provideradaptor.cpp
    COMMAND qdbusxml2cpp -a provideradaptor.h:provideradaptor.cpp -c ProviderAdaptor
            -i candidatetypes.h ${STAGE}/autofill/tech.everadaptive.Autofill1.Provider.xml
    WORKING_DIRECTORY ${ADAPTOR_DIR}
    DEPENDS ${STAGE}/autofill/tech.everadaptive.Autofill1.Provider.xml)

set(A ${STAGE}/autofill)
add_executable(provider_host provider_host.cpp
    ${A}/autofillprovider.cpp ${A}/autofillprovider.h ${A}/autofillsave.cpp ${A}/autofillsave.h
    ${A}/urimatcher.cpp ${STAGE}/appsettings.cpp ${STAGE}/appsettings.h ${STAGE}/bitsailorcore.h
    ${ADAPTOR_DIR}/provideradaptor.cpp ${ADAPTOR_DIR}/provideradaptor.h)
target_include_directories(provider_host PRIVATE ${A} ${ADAPTOR_DIR} ${STAGE})
target_link_libraries(provider_host Qt6::Core Qt6::DBus)

add_executable(ui_host ui_host.cpp ${A}/autofillpickerservice.cpp ${A}/autofillpickerservice.h)
target_include_directories(ui_host PRIVATE ${A})
target_link_libraries(ui_host Qt6::Core Qt6::DBus)

add_executable(autofillsave_test autofillsave_test.cpp ${A}/autofillsave.cpp ${A}/autofillsave.h)
target_include_directories(autofillsave_test PRIVATE ${A})
target_link_libraries(autofillsave_test Qt6::Core)
