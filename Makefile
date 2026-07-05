# L10GL - Lightweight Legacy OpenGL Driver Framework
#
# Backend structure:
#   src/l10gl.c                 - Frontend dispatch layer
#   src/backends/mga1064        - Matrox MGA-1064SG (Mystique) backend
#   src/backends/virge          - S3 ViRGE integrated 3D accelerator backend
#   demos/cube.c                - Gouraud cube demo
#
# Usage:
#   make              - Build with default backend (mga1064)
#   make BACKEND=virge - Build with S3 ViRGE backend

CC       = gcc
CFLAGS   = -Wall -Wextra -O2 -g -D_GNU_SOURCE -Isrc
LDFLAGS  = -lm

# Backend selection (which hardware driver to compile in)
BACKEND  ?= mga1064

# Map backend name to source files
BACKEND_SRCS = src/backends/$(BACKEND)/$(BACKEND).c \
               src/backends/$(BACKEND)/l10gl_$(BACKEND).c

# Core sources (frontend + selected backend)
CORE_SRCS = src/l10gl.c $(BACKEND_SRCS)

# Backend selection define for cube.c
ifeq ($(BACKEND),virge)
BACKEND_DEFINE = -DBACKEND_VIRGE
else
BACKEND_DEFINE = -DBACKEND_MGA1064
endif

# Demos
DEMOS = cube

.PHONY: all clean

all: $(DEMOS)

# Pattern: build demo from demos/X.c + core sources
cube: demos/cube.c $(CORE_SRCS) src/l10gl.h src/backends/$(BACKEND)/$(BACKEND).h
	$(CC) $(CFLAGS) $(BACKEND_DEFINE) -o $@ demos/cube.c $(CORE_SRCS) $(LDFLAGS)

clean:
	rm -f $(DEMOS) *.o src/*.o src/backends/*/*.o
