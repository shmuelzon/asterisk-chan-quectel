#
# chan_quectel - minimal Asterisk channel driver for the Quectel EC25-E
#
CMAKE_MINIMUM_REQUIRED(VERSION 3.16)

#
# Version comes from the most recent git tag, so nothing in the source has to be
# edited for a release - tag it and the version follows.
#
#   on a tag        v0.1.0            -> 0.1.0
#   after a tag     v0.1.0-5-gabc123  -> 0.1.0.5
#   dirty tree      ...-dirty         -> the full describe string is still shown
#   no git / export -> falls back to CHAN_QUECTEL_VERSION below
#
SET(CHAN_QUECTEL_VERSION "0.0.0" CACHE STRING "Version to use when git is unavailable")

FIND_PACKAGE(Git QUIET)
IF(GIT_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
    EXECUTE_PROCESS(
        COMMAND ${GIT_EXECUTABLE} describe --always --tags --dirty
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_DESCRIBE
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    IF(GIT_DESCRIBE)
        SET(CHAN_QUECTEL_VERSION "${GIT_DESCRIBE}")
    ENDIF()
ENDIF()

# Strip the leading "v" and turn "0.1.0-5-gabc123" into a plain 0.1.0.5 for
# PROJECT(), which only accepts digits and dots.
STRING(REGEX REPLACE "^v" "" _ver "${CHAN_QUECTEL_VERSION}")
STRING(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)(-([0-9]+))?" _m "${_ver}")
IF(_m)
    SET(_numeric "${CMAKE_MATCH_1}.${CMAKE_MATCH_2}.${CMAKE_MATCH_3}")
    IF(CMAKE_MATCH_5)
        SET(_numeric "${_numeric}.${CMAKE_MATCH_5}")
    ENDIF()
ELSE()
    SET(_numeric "0.0.0")
ENDIF()

PROJECT(chan-quectel VERSION ${_numeric} LANGUAGES C)

MESSAGE(STATUS "chan_quectel version: ${CHAN_QUECTEL_VERSION} (numeric ${PROJECT_VERSION})")

SET(CMAKE_C_STANDARD 11)
SET(CMAKE_C_STANDARD_REQUIRED ON)

SET(AST_HEADER_DIR "" CACHE PATH "Directory containing asterisk.h and asterisk/")
IF(NOT AST_HEADER_DIR)
    MESSAGE(FATAL_ERROR "Set -DAST_HEADER_DIR to the directory holding asterisk.h")
ENDIF()

FIND_PACKAGE(Threads REQUIRED)
FIND_PACKAGE(ALSA 1.1.0 REQUIRED)

ADD_LIBRARY(chan_quectel MODULE
    src/chan_quectel.c
    src/audio.c
    src/ringbuf.c
    src/at.c
    src/cli.c
    src/sms.c
    src/pdu.c
    src/char_conv.c
    src/error.c
)

TARGET_INCLUDE_DIRECTORIES(chan_quectel PRIVATE ${AST_HEADER_DIR})
TARGET_COMPILE_DEFINITIONS(chan_quectel PRIVATE _GNU_SOURCE AST_MODULE_SELF_SYM=__internal_chan_quectel_self AST_MODULE="chan_quectel"
    CHAN_QUECTEL_VERSION="${CHAN_QUECTEL_VERSION}")
TARGET_COMPILE_OPTIONS(chan_quectel PRIVATE -Wall -Wextra -Wno-unused-parameter -fPIC)
TARGET_LINK_LIBRARIES(chan_quectel PRIVATE ALSA::ALSA Threads::Threads)

SET_TARGET_PROPERTIES(chan_quectel PROPERTIES PREFIX "" OUTPUT_NAME "chan_quectel")

INSTALL(TARGETS chan_quectel DESTINATION lib/asterisk/modules)
# OpenWrt sets CMAKE_INSTALL_PREFIX to /usr, so the config has to climb out of
# it to land in /etc/asterisk.
INSTALL(FILES quectel.conf DESTINATION ${CMAKE_INSTALL_PREFIX}/../etc/asterisk)
