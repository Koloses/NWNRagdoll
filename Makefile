CXX=x86_64-w64-mingw32-g++
CC=x86_64-w64-mingw32-gcc
MH=third_party/minhook
B3=third_party/box3d
CFLAGS=-O2 -I$(MH)/include -I$(B3)/include -Isrc
CXXFLAGS=$(CFLAGS) -std=c++17 -fno-exceptions -fno-rtti -Wall -Wno-unused-function -Wno-unused-variable
B3FLAGS=-std=c17 -O2 -msse4.1 -I$(B3)/include -I$(B3)/src -DNDEBUG
LDFLAGS=-shared -static -static-libgcc -static-libstdc++ -s -Wl,--enable-stdcall-fixup
LIBS=-luser32 -lkernel32
SRC=$(wildcard src/*.cpp)
B3SRC=$(wildcard $(B3)/src/*.c)
MHSRC=$(MH)/src/buffer.c $(MH)/src/hook.c $(MH)/src/trampoline.c $(MH)/src/hde/hde64.c
OBJ=$(patsubst src/%.cpp,build/%.o,$(SRC)) $(patsubst $(B3)/src/%.c,build/b3_%.o,$(B3SRC)) $(patsubst $(MH)/src/%.c,build/mh_%.o,$(subst hde/,hde_,$(MHSRC)))
all: build/dbghelp.dll
build/%.o: src/%.cpp src/*.h
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -c $< -o $@
build/b3_%.o: $(B3)/src/%.c
	@mkdir -p build
	@$(CC) $(B3FLAGS) -c $< -o $@
build/mh_%.o:
	@mkdir -p build
	$(CC) -O2 -I$(MH)/include -c $(MH)/src/$(subst hde_,hde/,$*).c -o $@
build/dbghelp.dll: $(OBJ) src/dbghelp.def
	$(CXX) $(LDFLAGS) -o $@ $(OBJ) src/dbghelp.def $(LIBS)
release: build/dbghelp.dll
	rm -rf build/release && mkdir -p build/release
	cp build/dbghelp.dll NWNRagdoll.ini README.md build/release/
	cp third_party/box3d/LICENSE build/release/LICENSE-box3d.txt
	mkdir -p build/release/ovr && cp ovr/* build/release/ovr/
	cd build/release && zip -q -9 -r ../NWNRagdoll.zip *
clean:
	rm -rf build
