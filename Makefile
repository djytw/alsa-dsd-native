ifeq ($(origin CC),default)
CC = clang
endif

all:
	$(CC) main.c -lasound -Wall -g -o alsa-dsd-native

clean:
	rm alsa-dsd-native
