CXX      = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -pthread

all: server client

server: server.cpp common.hpp
	$(CXX) $(CXXFLAGS) -o server server.cpp

client: client.cpp common.hpp
	$(CXX) $(CXXFLAGS) -o client client.cpp

clean:
	rm -f server client chat_history.json net_diag_*.json
