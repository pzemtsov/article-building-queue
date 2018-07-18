JAVA_HOME=/usr/java/8/x86_64/jdk

    c++ -m64 -std=c++11  -Wall  -fPIC -O3  -falign-functions=32 -falign-loops=32 -funroll-loops -shared -o libNativeDataSource.so \
	-I${JAVA_HOME}/include{,/linux} \
	NativeDataSource.cpp \
	-lpthread

