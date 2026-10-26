#include "fsm.h"
#include "result.h"

#define RESULT_FSM_OK(fsm) RESULT_OK(FSM, FSM_Error, (fsm))
#define RESULT_FSM_ERR(err) RESULT_ERROR(FSM, FSM_Error, (err))
#define RESULT_FSMERR_ERR(err) RESULT_ERROR(FSM_Error, FSM_Error, (err))

static const FSM_Ops fsm_ops = {
	.update = FSM_Update,
	.transition = FSM_Transition,
};

Result(FSM, FSM_Error) FSM_Init(const FSM_StateOps *states_ops, size_t states_count, size_t initial_state, void *ctx)
{
	if (states_ops == NULL)
	{
		return RESULT_FSM_ERR(FSM_ERROR_INIT_STATES_OPS_NULLPTR);
	}

	FSM self = {
		.states_ops = states_ops,
		.states_count = states_count,
		.current_state = initial_state,
		.ctx = ctx,
		.ops = &fsm_ops,
	};

	FSM_Callback enter = states_ops[initial_state].enter;

	if (enter == NULL)
	{
		return RESULT_FSM_ERR(FSM_ERROR_STATE_ENTER_CALLBACK_NULLPTR);
	}

	enter(&self, ctx);

	return RESULT_FSM_OK(self);
}

Result(FSM_Error, FSM_Error) FSM_Update(FSM *self)
{
	FSM_Callback update = self->states_ops[self->current_state].update;

	if (update == NULL)
	{
		return RESULT_FSMERR_ERR(FSM_ERROR_STATE_UPDATE_CALLBACK_NULLPTR);
	}

	update(self, self->ctx);

	return RESULT_FSMERR_ERR(FSM_ERROR_NONE);
}

Result(FSM_Error, FSM_Error) FSM_Transition(FSM *self, size_t next_state)
{
	if (next_state == self->current_state)
	{
		return RESULT_FSMERR_ERR(FSM_ERROR_NONE);
	}

	const FSM_StateOps *current = &self->states_ops[self->current_state];
	const FSM_StateOps *next = &self->states_ops[next_state];

	if (current->exit == NULL)
	{
		return RESULT_FSMERR_ERR(FSM_ERROR_STATE_EXIT_CALLBACK_NULLPTR);
	}

	current->exit(self, self->ctx);

	self->current_state = next_state;

	if (next->enter == NULL)
	{
		return RESULT_FSMERR_ERR(FSM_ERROR_STATE_ENTER_CALLBACK_NULLPTR);
	}

	next->enter(self, self->ctx);

	return RESULT_FSMERR_ERR(FSM_ERROR_NONE);
}
