# Builds the interactive simulator: the firmware's own sources (src/main.cpp,
# the vpet game module, Arduino_GFX) against the fake board in fakes/ and
# sim_hal.cpp. Usually run through ./run.sh. See ../README.md.
#
#   make -C tools/sim/live            both binaries (the window one needs SDL2)
#   make -C tools/sim/live headless   just the windowless one

ROOT := $(abspath ../../..)
BUILD := $(ROOT)/.pio/sim-live
GFX := lib/GFX_Library_for_Arduino/src

VERSION := $(shell sed -n 's/.*DIGIPET_VERSION=\\"\([^\\]*\)\\".*/\1/p' $(ROOT)/platformio.ini)

CXX ?= g++
CXXFLAGS ?= -O1 -g
CXXFLAGS += -std=gnu++17 -MMD -MP -Wno-attributes -Wno-format-truncation -Wno-format
CPPFLAGS := -I$(ROOT)/tools/sim/live/fakes -I$(ROOT)/tools/sim/fakes -I$(ROOT)/$(GFX) \
            -I$(ROOT)/include -I$(ROOT)/src \
            -DBOARD_AMOLED_216 -DDIGIPET_VERSION='"$(VERSION)-sim"' \
            -DDIGIPET_OTA_REPO='"sim/none"'

# Firmware sources, exactly what [env:waveshare_amoled_216] builds minus the
# radio/OTA code the sim replaces (familiar_battle_service.cpp, ota_updater.cpp).
FIRMWARE := src/main.cpp src/power_telemetry.cpp src/color_utils.cpp \
            $(wildcard $(addprefix $(ROOT)/,src/game/vpet/*.cpp))
FIRMWARE := $(patsubst $(ROOT)/%,%,$(FIRMWARE))
LIBS := $(GFX)/Arduino_G.cpp $(GFX)/Arduino_GFX.cpp $(GFX)/Arduino_DataBus.cpp \
        $(GFX)/canvas/Arduino_Canvas.cpp
SIM := tools/sim/live/sim_hal.cpp tools/sim/live/sim_battle_service.cpp \
       tools/sim/live/sim_ota.cpp
ASSETS := $(sort $(wildcard $(ROOT)/src/assets/*.pcm $(ROOT)/src/assets/*.rgb565))
ASSETS := $(patsubst $(ROOT)/%,%,$(ASSETS))

COMMON_OBJS := $(addprefix $(BUILD)/,$(FIRMWARE:.cpp=.o) $(LIBS:.cpp=.o) $(SIM:.cpp=.o)) \
               $(BUILD)/assets.o

SDL_CFLAGS := $(shell sdl2-config --cflags 2>/dev/null || pkg-config --cflags sdl2 2>/dev/null)
SDL_LIBS := $(shell sdl2-config --libs 2>/dev/null || pkg-config --libs sdl2 2>/dev/null)

TARGETS := $(BUILD)/digipet-sim-headless
ifneq ($(SDL_LIBS),)
TARGETS += $(BUILD)/digipet-sim
endif

.PHONY: all headless window clean
all: $(TARGETS)
ifeq ($(SDL_LIBS),)
	@echo "note: SDL2 not found, built only the headless sim (sudo apt install libsdl2-dev)"
endif
headless: $(BUILD)/digipet-sim-headless
window: $(BUILD)/digipet-sim

$(BUILD)/digipet-sim: $(COMMON_OBJS) $(BUILD)/tools/sim/live/frontend_sdl.o
	$(CXX) -o $@ $^ $(SDL_LIBS) -pthread

$(BUILD)/digipet-sim-headless: $(COMMON_OBJS) $(BUILD)/tools/sim/live/frontend_headless.o
	$(CXX) -o $@ $^ -pthread

$(BUILD)/tools/sim/live/frontend_sdl.o: CPPFLAGS += $(SDL_CFLAGS)

$(BUILD)/%.o: $(ROOT)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -c $< -o $@

# The same _binary_src_assets_*_start/_end symbols board_build.embed_files
# gives the firmware; ld derives them from the path, so run it from ROOT.
$(BUILD)/assets.o: $(addprefix $(ROOT)/,$(ASSETS))
	@mkdir -p $(dir $@)
	cd $(ROOT) && ld -r -b binary -z noexecstack -o $@ $(ASSETS)

clean:
	rm -rf $(BUILD)

-include $(COMMON_OBJS:.o=.d) $(BUILD)/tools/sim/live/frontend_sdl.d \
         $(BUILD)/tools/sim/live/frontend_headless.d
