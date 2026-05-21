CXX      := g++
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS  := -lncurses -lpthread

SRCS   := main.cpp simulators.cpp intersection.cpp display.cpp
OBJS   := $(SRCS:.cpp=.o)
TARGET := traffic_sim
FI_TARGET := fault_injector

all: $(TARGET) $(FI_TARGET)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

$(FI_TARGET): fault_injector.o
	$(CXX) $(CXXFLAGS) -o $@ $^ -lncurses

fault_injector.o: fault_injector.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

%.o: %.cpp simulators.h intersection.h display.h
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET) fault_injector.o $(FI_TARGET)

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run
