MICROPYTHON_TOP = ../../../../../build/vendor/micropython-1.26.1
SRC_QSTR += $(MICROPYTHON_TOP)/extmod/modjson.c ../../../python/mpbridge.c
include $(MICROPYTHON_TOP)/ports/embed/embed.mk
