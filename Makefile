all:
	$(CC) -std=c99 -O3 -o pinselflut `pkg-config --cflags sdl2` pinselflut.c -lm `pkg-config --libs sdl2`
