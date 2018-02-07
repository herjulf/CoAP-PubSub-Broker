INCLUDE= -I cantcoap/

#CXX=g++49
CXX=clang++
CXXFLAGS=-Wall -DDEBUG -std=c++11 $(INCLUDE)

#CC=gcc49
CC=clang
CFLAGS=-Wall -std=c99 -DDEBUG

default: broker

broker: cantcoap/libcantcoap.a cantcoap/nethelper.o broker.cpp

clean:
	rm broker;
