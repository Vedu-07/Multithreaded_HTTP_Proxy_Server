CXX = g++
CXXFLAGS = -g -Wall -std=c++17 -pthread

all: proxy

proxy: proxy.cpp
	$(CXX) $(CXXFLAGS) -o proxy proxy.cpp

clean:
	rm -f proxy *.o

tar:
	tar -cvzf ass1.tgz proxy.cpp README Makefile
