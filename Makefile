# CMake also works directly with Ninja, Visual Studio, and other generators.
CMAKE ?= cmake
JOBS ?= 4
CMAKE_ARGS ?=
BUILD_ROOT ?= build

.PHONY: all dynamic static
all: dynamic

dynamic:
	$(CMAKE) -S . -B $(BUILD_ROOT)-dynamic -DCMAKE_BUILD_TYPE=Release -DDPDFCOMP_STATIC=OFF $(CMAKE_ARGS)
	$(CMAKE) --build $(BUILD_ROOT)-dynamic --config Release --parallel $(JOBS)

static:
	$(CMAKE) -S . -B $(BUILD_ROOT)-static -DCMAKE_BUILD_TYPE=Release -DDPDFCOMP_STATIC=ON $(CMAKE_ARGS)
	$(CMAKE) --build $(BUILD_ROOT)-static --config Release --parallel $(JOBS)

