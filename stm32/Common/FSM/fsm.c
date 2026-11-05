#include "fsm.h"

/*
	update - обов'язковий для кожного стану.
	enter / exit - опційні (NULL = нічого не робити).
	FSM_Init НЕ викликає enter початкового стану (FSM повертається за значенням,
	тому вказівник self у callback був би на тимчасовий об'єкт).
*/

static const FSM_Ops fsm_ops = {
	.update = FSM_Update,
	.transition = FSM_Transition,
};

Result(FSM, FSM_Error) FSM_Init(const FSM_StateOps *states_ops, size_t states_count, size_t initial_state, void *ctx)
{
	if (states_ops == NULL || states_count == 0U || initial_state >= states_count)
	{
		return RESULT_ERROR(FSM, FSM_Error, FSM_ERROR_INIT_STATES_OPS_NULLPTR);
	}

	for (size_t i = 0; i < states_count; i++)
	{
		if (states_ops[i].update == NULL)
		{
			return RESULT_ERROR(FSM, FSM_Error, FSM_ERROR_STATE_UPDATE_CALLBACK_NULLPTR);
		}
	}

	FSM fsm = {
		.states_ops = states_ops,
		.states_count = states_count,
		.current_state = initial_state,
		.ctx = ctx,
		.ops = &fsm_ops,
	};

	return RESULT_OK(FSM, FSM_Error, fsm);
}

Result(FSM_Error, FSM_Error) FSM_Update(FSM *self)
{
	if (self == NULL || self->states_ops == NULL || self->current_state >= self->states_count)
	{
		return RESULT_ERROR(FSM_Error, FSM_Error, FSM_ERROR_INIT_STATES_OPS_NULLPTR);
	}

	const FSM_Callback update = self->states_ops[self->current_state].update;

	if (update == NULL)
	{
		return RESULT_ERROR(FSM_Error, FSM_Error, FSM_ERROR_STATE_UPDATE_CALLBACK_NULLPTR);
	}

	update(self, self->ctx);

	return RESULT_OK(FSM_Error, FSM_Error, FSM_ERROR_NONE);
}

Result(FSM_Error, FSM_Error) FSM_Transition(FSM *self, size_t next_state)
{
	if (self == NULL || self->states_ops == NULL || next_state >= self->states_count || self->current_state >= self->states_count)
	{
		return RESULT_ERROR(FSM_Error, FSM_Error, FSM_ERROR_INIT_STATES_OPS_NULLPTR);
	}

	const FSM_Callback exit_cb = self->states_ops[self->current_state].exit;
	if (exit_cb != NULL)
	{
		exit_cb(self, self->ctx);
	}

	self->current_state = next_state;

	const FSM_Callback enter_cb = self->states_ops[next_state].enter;
	if (enter_cb != NULL)
	{
		enter_cb(self, self->ctx);
	}

	return RESULT_OK(FSM_Error, FSM_Error, FSM_ERROR_NONE);
}
