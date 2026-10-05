# The alchemy-lab emulator (branch emu-custom):
#
#   make -C <alchemy-lab>/emu FW=custom FW_DIR=<this repo>
#   make -C <alchemy-lab>/emu test-custom FW_DIR=<this repo>   (emu-tests/*.emu, card = emu-card/)
#
# Described here rather than read from the Makefile because picker.cpp must
# stay out: the emulator stubs the SD picker itself (duplicate symbols).
# A worktree with empty lib/ submodules: export EMU_ALCHEMY_DIR and
# EMU_LIBDAISY_DIR as absolute paths to a checkout that has them.
EMU_TARGET       := break_alchemy
EMU_CPP_SOURCES  := src/break_alchemy.cpp src/card_loader.cpp
EMU_C_SOURCES    := src/clock_adapter.c src/versio_alloc.c src/break_engine.c src/ecto_card.c
EMU_C_INCLUDES   := -Isrc
EMU_C_DEFS       := -DBREAK_VERSION=\"emu\" -DBREAK_GIT_HASH=\"emu\"
EMU_ALCHEMY_DIR  ?= lib/alchemy-sdk
EMU_LIBDAISY_DIR ?= lib/libDaisy
EMU_LABELS       := BREAK|AMEN|SAMPLE|RANDOM|RUNE|DJ FILTER
EMU_LABELS_ALT   := VOLUME|SEED|BANK|PITCH RANGE|CLOCK RATIO|TEMPO
