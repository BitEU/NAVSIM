# NAVSIM - Cold War Naval Tactical Engagement Simulator
# ────────────────────────────────────────────────────────
# Linux/macOS:  make
# Windows MSVC: cl /O2 navsim.c /Fe:navsim.exe
# Windows GCC:  gcc -O2 -o navsim.exe navsim.c -lm
CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -pedantic -std=c11
LDFLAGS = -lm
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