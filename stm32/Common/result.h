/*
*	Result type: one value that holds EITHER a good value OR an error.
*	This helps us return errors without special tricks (like -1 or NULL).
*/

#ifndef PROJ_RESULT_H
#define PROJ_RESULT_H

#include <stdbool.h>

/*
*	Makes a type name. Result(int, MyError) becomes Result_int_MyError.
*/
#define Result(T, E) Result_##T##_##E

/*
*	Creates the struct type. "value" and "error" share the same memory (union),
*	and "is_error" tells which one is valid. Always check is_error first!
*/
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

/*
*	Helper: make a Result that contains a good value.
*/
#define RESULT_OK(T, E, _value)	\
((Result(T, E)){ 								\
	.is_error = false,						\
	.value = (_value),						\
})

/*
*	Helper: make a Result that contains an error.
*/
#define RESULT_ERROR(T, E, _error)	\
((Result(T, E)){ 										\
	.is_error = true,									\
	.error = (_error),								\
})

#endif
