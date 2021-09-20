#
# This is a project Makefile. It is assumed the directory this Makefile resides in is a
# project subdirectory.
#

PROJECT_NAME := water

EXCLUDE_COMPONENTS := console coap esp_gdbstub freemodbus jsmn json libsodium esp-mqtt

include $(IDF_PATH)/make/project.mk

tags:
	ctags -R -f tags ../ESP8266_RTOS_SDK/ . main
