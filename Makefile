CXX      = g++
CC       = gcc
CXXFLAGS = -std=c++17 -O2 -g -Wall -Wno-unused-result -rdynamic -fno-strict-aliasing \
           $(shell pkg-config --cflags gtk+-3.0) \
           -I quickjs
CFLAGS   = -std=gnu11 -O2 -Wall -Wno-unused-result \
           -D_GNU_SOURCE -DCONFIG_VERSION=\"2025-09-13\"
LDFLAGS  = $(shell pkg-config --libs gtk+-3.0) -lcurl -lm -lpthread -ldl

# QuickJS C sources (we compile only what we need)
QJS_SRCS = quickjs/quickjs.c quickjs/libregexp.c quickjs/libunicode.c \
           quickjs/cutils.c quickjs/dtoa.c quickjs/quickjs-libc.c
QJS_OBJS = $(QJS_SRCS:.c=.o)

# Our C++ sources
CXX_SRCS = browser.cpp dom.cpp js_engine.cpp js_bindings.cpp js_event.cpp \
           js_compat.cpp layout.cpp paint.cpp hit_test.cpp
CXX_OBJS = $(CXX_SRCS:.cpp=.o)

TARGET = browser

all: $(TARGET)

$(TARGET): $(CXX_OBJS) $(QJS_OBJS)
	$(CXX) -o $@ $^ $(LDFLAGS)

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

quickjs/%.o: quickjs/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(CXX_OBJS) $(QJS_OBJS) $(TARGET)

.PHONY: all clean
