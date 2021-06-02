# makefile for audio demo 

# specify compiler
CXX = g++ 

# -LC:\libraries\SDL2-2.0.10\i686-w64-mingw32\lib -lSDL2main -lSDL2
SDL_LIB = -LC:\SDL2\lib -lSDL2main -lSDL2

# -IC:\libraries\SDL2-2.0.10\i686-w64-mingw32\include\SDL2
SDL_INCLUDE = -IC:\SDL2\include\SDL2

# set up flags 
CFLAGS = -Wall -c -std=c++14 $(SDL_INCLUDE)
LDFLAGS = -lmingw32 -mwindows -static-libstdc++ -static-libgcc $(SDL_LIB)

# object files needed 
OBJ = wavplayer.o smbPitchShift.o

EXE = wavplayer

all: $(EXE)

$(EXE): $(OBJ)
	$(CXX) $(OBJ) $(LDFLAGS) -o $@ 

smbPitchShift.o: smbPitchShift.cpp smbPitchShift.h
	$(CXX) $(CFLAGS) $< -o $@

wavplayer.o: wavplayer.cpp smbPitchShift.h resources.h
	$(CXX) $(CFLAGS) $< -o $@
	
clean:
	rm *.o