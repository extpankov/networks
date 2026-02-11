CXX = g++
CXXFLAGS = -Wall -Wextra -std=c++17
TARGET_SERVER = server
TARGET_CLIENT = client

all: $(TARGET_SERVER) $(TARGET_CLIENT)

$(TARGET_SERVER): server.cpp
	$(CXX) $(CXXFLAGS) server.cpp -o $(TARGET_SERVER)

$(TARGET_CLIENT): client.cpp
	$(CXX) $(CXXFLAGS) client.cpp -o $(TARGET_CLIENT)

clean:
	rm -f $(TARGET_SERVER) $(TARGET_CLIENT)

re: clean all