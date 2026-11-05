add_library(proto STATIC
	src/protocol.c
	src/msg.c
)
target_include_directories(proto PUBLIC include)
target_compile_features(proto PUBLIC c_std_11)
