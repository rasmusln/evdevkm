
build:
	gcc -g evdevkm.c -I/usr/include/libevdev-1.0 -levdev -o evdevkm

run: build
	./evdevkm

debug: build
	gdb evdevkm

