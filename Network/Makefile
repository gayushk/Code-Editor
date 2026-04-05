CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2
LDFLAGS  := -lpthread

OBJS_SERVER := server.o socket.o
OBJS_CLIENT := client.o socket.o

all: server client

server: $(OBJS_SERVER)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

client: $(OBJS_CLIENT)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS)

server.o: server.cpp socket.hpp shared_doc.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

client.o: client.cpp socket.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

socket.o: socket.cpp socket.hpp
	$(CXX) $(CXXFLAGS) -c -o $@ $<

clean:
	rm -f server client *.o document.txt document.txt.tmp

.PHONY: all clean
