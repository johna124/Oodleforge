# Makefile for OodleForge

CXX = x86_64-w64-mingw32-g++
CXXFLAGS = -std=c++17 -O3 -march=x86-64 -mtune=generic -static -static-libgcc -static-libstdc++ -s -flto
INCLUDES = -I. -Iaes
LIBS = -lwinpthread -lpthread
TARGET = oodleforge.exe
SOURCES = main.cpp common.cpp scan.cpp encode.cpp reconstruct.cpp aes.c

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) $(INCLUDES) $^ -o $@ $(LIBS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
