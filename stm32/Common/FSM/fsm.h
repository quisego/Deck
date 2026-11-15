#ifndef PROJ_FSM_H
#define PROJ_FSM_H

#include "result.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct FSM FSM;
typedef struct FSM_Ops FSM_Ops;
typedef struct FSM_StateOps FSM_StateOps;
typedef enum FSM_Error FSM_Error;

typedef void (*FSM_Callback)(FSM *self, void *ctx);

struct FSM_StateOps
{
	FSM_Callback enter;
	FSM_Callback update;
	FSM_Callback exit;
};

struct FSM {
	const FSM_StateOps *states_ops;
	size_t states_count;
	size_t current_state;
	void *ctx;

	const FSM_Ops *ops;
};

enum FSM_Error {
	FSM_ERROR_NONE = 0,
	FSM_ERROR_INIT_STATES_OPS_NULLPTR,
	FSM_ERROR_STATE_ENTER_CALLBACK_NULLPTR,
	FSM_ERROR_STATE_UPDATE_CALLBACK_NULLPTR,
	FSM_ERROR_STATE_EXIT_CALLBACK_NULLPTR,
};

DEFINE_RESULT(FSM, FSM_Error);
DEFINE_RESULT(FSM_Error, FSM_Error);

struct FSM_Ops
{
	Result(FSM_Error, FSM_Error) (*update)(FSM *self);
	Result(FSM_Error, FSM_Error) (*transition)(FSM *self, size_t next_state);
};

Result(FSM, FSM_Error) FSM_Init(const FSM_StateOps *states_ops, size_t states_count, size_t initial_state, void *ctx);

Result(FSM_Error, FSM_Error) FSM_Update(FSM *self);
Result(FSM_Error, FSM_Error) FSM_Transition(FSM *self, size_t next_state);

#ifdef __cplusplus
}
#endif

#endif
