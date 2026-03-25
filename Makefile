# NAVSIM - Cold War Naval Tactical Engagement Simulator v4.0
# ────────────────────────────────────────────────────────────
# Requires: ncurses (ncursesw + panelw)
# Linux/macOS:  make
# MSYS2/MinGW:  make  (install: pacman -S mingw-w64-ucrt-x86_64-ncurses)
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -pedantic -std=c11 $(shell pkg-config --cflags ncursesw 2>/dev/null || echo -DNCURSES_WIDECHAR)
LDFLAGS = -lm $(shell pkg-config --libs ncursesw panelw 2>/dev/null || echo -lncursesw -lpanelw)
TARGET  = navsim
SRC     = navsim.c

.PHONY: all clean run analyze monte-carlo

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

run: $(TARGET)
	./$(TARGET)

analyze: run
	python3 navsim_analysis.py

# Run 100 Monte Carlo simulations and analyze aggregate results
monte-carlo: $(TARGET)
	python3 navsim_analysis.py --monte-carlo 100

clean:
	rm -f $(TARGET) battle_log.csv ship_status.csv
