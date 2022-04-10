# makefile for audio demo 

# specify compiler
CXX = g++ 

# -LC:\SDL2\lib -lSDL2main -lSDL2
SDL_LIB = -LC:\libraries\SDL2-2.0.10\i686-w64-mingw32\lib -lSDL2main -lSDL2

# -IC:\SDL2\include\SDL2
SDL_INCLUDE = -IC:\libraries\SDL2-2.0.10\i686-w64-mingw32\include\SDL2

SNDTOUCH_DIR = soundtouch

# set up flags 
CXXFLAGS = -g -Wall -Wformat -O2 -c -std=c++14 $(SDL_INCLUDE) -I$(SNDTOUCH_DIR)
LDFLAGS = -lmingw32 -mwindows -static-libstdc++ -static-libgcc $(SDL_LIB)

# object files needed 
OBJS = wavplayer.o smbPitchShift.o

# add the soundtouch files
SNDTOUCH_SRC = $(wildcard $(SNDTOUCH_DIR)/*.cpp)
OBJS += $(patsubst $(SNDTOUCH_DIR)/%.cpp, %.o, $(SNDTOUCH_SRC))

EXE = wavplayer

#$(info OBJS is $(OBJS))
all: $(EXE)
    
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