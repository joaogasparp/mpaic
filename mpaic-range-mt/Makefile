
CXX = g++
CXXFLAGS = -std=c++17 -O3 -pthread

COMMON_SRC = src/image_io.cpp

all: encoder decoder

encoder: src/encoder.cpp $(COMMON_SRC) src/range_codec.hpp src/adaptive_model.hpp src/context_model.hpp src/image_io.hpp
	$(CXX) $(CXXFLAGS) -o encoder src/encoder.cpp $(COMMON_SRC)

decoder: src/decoder.cpp $(COMMON_SRC) src/range_codec.hpp src/adaptive_model.hpp src/context_model.hpp src/image_io.hpp
	$(CXX) $(CXXFLAGS) -o decoder src/decoder.cpp $(COMMON_SRC)

clean:
	rm -f encoder decoder

.PHONY: all clean
