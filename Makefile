all: live.cpp
	g++ -g live.cpp -lavformat -lavdevice -lpthread -lavcodec -lavutil -lavfilter -lswscale