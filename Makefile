all: cow_fuse

cow_fuse: cow.cpp sql.h sql.cpp openat_sqlite_vfs.cpp
	c++ -g3 -Wall -W -o cow_fuse -std=c++11 cow.cpp sql.cpp openat_sqlite_vfs.cpp \
	-lsqlite3 $(shell pkg-config fuse --cflags --libs)
