# ______________________________________________________________________________
#
#  MRE Makefile - Calculator for MediaTek MRE
# ______________________________________________________________________________
# 
GCC = arm-none-eabi-gcc
GPP = arm-none-eabi-g++
PYTHON = python
OBJCOPY = arm-none-eabi-objcopy

SDK = sdk
APP_FILE_NAME = Calculator

INCLUDES = -I "$(SDK)/include" -I "src/include" -I "src/ResID" -I "src" 
DEFINES = -D _MINIGUI_LIB_ -D _USE_MINIGUIENTRY -D _NOUNIX_ -D _FOR_WNC -D __MRE_SDK__ -D __MRE_VENUS_NORMAL__ -D __MMI_MAINLCD_240X320__ -D MRE -D GCC -D __MRE_COMPILER_GCC__

SHARED_PARAM = -c -fpic -march=armv5te -fvisibility=hidden -Os -mlittle-endian $(INCLUDES) $(DEFINES) -fdata-sections -ffunction-sections
GCC_PARAM = $(SHARED_PARAM)
GPP_PARAM = $(SHARED_PARAM) -fno-threadsafe-statics -fno-exceptions -fno-rtti -fno-non-call-exceptions

LINK_LIB = -lstdc++ $(SDK)/lib/*.a 
LINK_PARAM = -fno-threadsafe-statics -Wl,--gc-sections -fpic -fpcc-struct-return --disable-libstdcxx-verbose -pie -lm -T "$(SDK)/scat.ld"

# ______________________________________________________________________________
# 
all: build $(APP_FILE_NAME).vxp

C_SRCS = $(wildcard src/*.c)
CPP_SRCS = $(wildcard src/*.cpp)

C_OBJS = $(patsubst src/%.c,build/%.o,$(C_SRCS))
CPP_OBJS = $(patsubst src/%.cpp,build/%.o,$(CPP_SRCS))

# 1. Compile C
build/%.o: src/%.c
	$(GCC) $(GCC_PARAM) -c $< -o $@

# 1. Compile C++
build/%.o: src/%.cpp
	$(GPP) $(GPP_PARAM) -c $< -o $@

# 2. Link
build/$(APP_FILE_NAME).axf: $(C_OBJS) $(CPP_OBJS)
	$(GCC) -o $@ $^ $(LINK_LIB) $(LINK_PARAM)

# 3. Add resources
build/$(APP_FILE_NAME).vxp: build/$(APP_FILE_NAME).axf
	$(OBJCOPY) -I elf32-little --add-section .vm_res=$(SDK)/resource.bin $^ $@

# 4. Add tags
$(APP_FILE_NAME).vxp: build/$(APP_FILE_NAME).vxp
	$(PYTHON) $(SDK)/build.py $^ $@

build:
	mkdir -p build

clean:
	rm -rf build
	rm -f *.vxp
