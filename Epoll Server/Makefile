CXX = g++
CXXFLAGS = -g -Wall -std=c++17 -pthread
TARGET = proxy
SRC = proxy.cpp

all: $(TARGET)

$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) -o $(TARGET) $(SRC)

clean:
	rm -f $(TARGET)
