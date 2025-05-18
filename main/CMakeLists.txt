set(CMAKE_CXX_STANDARD 17) # 设置 C++ 标准为 C++17
file(GLOB_RECURSE CPP_SOURCES *.cpp) # 查找所有 .cpp 文件
idf_component_register(SRCS ${CPP_SOURCES} INCLUDE_DIRS ".")
