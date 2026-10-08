#ifndef PROJ_RESULT_H
#define PROJ_RESULT_H

#include <stdbool.h>

#define Result(T, E) Result_##T##_##E

#define DEFINE_RESULT(T, E) \
typedef struct 							\
{ 													\
	union 										\
	{ 												\
		T value; 								\
		E error; 								\
	}; 												\
	bool is_error; 						\
} Result_##T##_##E

#define RESULT_OK(T, E, _value)	\
((Result(T, E)){ 								\
	.is_error = false,						\
	.value = (_value),						\
})

#define RESULT_ERROR(T, E, _error)	\
((Result(T, E)){ 										\
	.is_error = true,									\
	.error = (_error),								\
})

#endif