# makefile for audio demo 

# specify compiler
CXX = g++ 

# the path to the lib folder of SDL is C:\mingw_dev_lib\lib
SDL_LIB = -LC:\SDL2-2.0.7\i686-w64-mingw32\lib -lSDL2main -lSDL2
SDL_INCLUDE = -IC:\SDL2-2.0.7\i686-w64-mingw32\include\SDL2

# set up flags 
CFLAGS = -Wall -c -std=c++14 $(SDL_INCLUDE)
LDFLAGS = -lmingw32 -mwindows -static-libstdc++ -static-libgcc $(SDL_LIB)

# object files needed 
OBJ = wavplayer.o

EXE = wavplayer

all: $(EXE)

$(EXE): $(OBJ)
	$(CXX) $(OBJ) $(LDFLAGS) -o $@ 
	
wavplayer.o: wavplayer.cpp resources.h
	$(CXX) $(CFLAGS) $< -o $@
	
clean:
	rm *.o