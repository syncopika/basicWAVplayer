# makefile for audio demo 

# specify compiler
CXX = g++ 

# -LC:\SDL2\lib -lSDL2main -lSDL2
SDL_LIB = -LC:\libraries\SDL2-2.0.10\i686-w64-mingw32\lib -lSDL2main -lSDL2

# -IC:\SDL2\include\SDL2
SDL_INCLUDE = -IC:\libraries\SDL2-2.0.10\i686-w64-mingw32\include\SDL2

SNDTOUCH_DIR = soundtouch

# instruction set stuff for more optimization
MMX = $(SNDTOUCH_DIR)/opts/mmx_optimized.cpp
SSE = $(SNDTOUCH_DIR)/opts/sse_optimized.cpp

# set up flags 
CXXFLAGS = -g -Wall -Wformat -O2 -c -std=c++14 $(SDL_INCLUDE) -I$(SNDTOUCH_DIR)
LDFLAGS = -lmingw32 -mwindows -static-libstdc++ -static-libgcc $(SDL_LIB)

# object files needed 
OBJS = wavplayer.o

# add the soundtouch obj files
SNDTOUCH_SRC = $(wildcard $(SNDTOUCH_DIR)/*.cpp)
OBJS += mmx_optimized.o sse_optimized.o
OBJS += $(patsubst $(SNDTOUCH_DIR)/%.cpp, %.o, $(SNDTOUCH_SRC))

EXE = wavplayer


#$(info OBJS is $(OBJS))
all: $(EXE)

# build instruction set stuff first
mmx_optimized.o: $(MMX)
	$(CXX) -mmmx $(CXXFLAGS) -c -o $@ $<
    
sse_optimized.o: $(SSE)
	$(CXX) -msse $(CXXFLAGS) -c -o $@ $<

# build the rest of the soundtouch stuff
%.o: $(SNDTOUCH_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

#smbPitchShift.o: smbPitchShift.cpp smbPitchShift.h
#	$(CXX) $(CXXFLAGS) $< -o $@

wavplayer.o: wavplayer.cpp resources.h
	$(CXX) $(CXXFLAGS) $< -o $@
    
$(EXE): $(OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)
    
clean:
	rm *.o