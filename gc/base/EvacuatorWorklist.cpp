/*******************************************************************************
 * Copyright (c) 2018, 2018 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at https://www.eclipse.org/legal/epl-2.0/
 * or the Apache License, Version 2.0 which accompanies this distribution and
 * is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following
 * Secondary Licenses when the conditions for such availability set
 * forth in the Eclipse Public License, v. 2.0 are satisfied: GNU
 * General Public License, version 2 with the GNU Classpath
 * Exception [1] and GNU General Public License, version 2 with the
 * OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "EvacuatorWorklist.hpp"

/* Worklist methods */

#if defined(EVACUATOR_DEBUG)
void
MM_EvacuatorWorklist::assertWorklistInvariant() const
{
	Debug_MM_true((NULL == _default.head) == (NULL == _default.tail));
	Debug_MM_true((NULL == _default.head) || (NULL == _default.tail->next));
	Debug_MM_true((NULL == _deferred.head) == (NULL == _deferred.tail));
	Debug_MM_true((NULL == _deferred.head) || (NULL == _deferred.tail->next));
	uintptr_t v = 0;
	for (const MM_EvacuatorBase::Workspace *work = _default.head; work != NULL; work = work->next) {
		v += volume(work);
	}
	for (const MM_EvacuatorBase::Workspace *work = _deferred.head; work != NULL; work = work->next) {
		v += volume(work);
	}
	Debug_MM_true(v == _volume);

}
#endif /* defined(EVACUATOR_DEBUG) */

const MM_EvacuatorBase::Workspace *
MM_EvacuatorWorklist::peek(bool deferred) const
{
	assertWorklistInvariant();

	/* select a list in preference order */
	const List *list = deferred ? &_deferred : &_default;
	const MM_EvacuatorBase::Workspace *work = list->head;
	if (NULL == work) {
		list = deferred ? &_default : &_deferred;
		work = list->head;
	}

	assertWorklistInvariant();
	return work;
}

void
MM_EvacuatorWorklist::addWorkToList(List *list, MM_EvacuatorBase::Workspace *workspace) const
{
	/* expect singleton workspace reject null or empty workspace */
	Debug_MM_true((NULL != workspace) && (0 < workspace->length) && (NULL == workspace->next));
	if (NULL != workspace) {
		if (0 < workspace->length) {
			if (NULL == list->head) {
				list->head = list->tail = workspace;
			} else if ((MM_EvacuatorBase::min_copyspace_size > list->tail->length)
			&& (workspace->base == (list->tail->base + list->tail->length))
			&& (0 == workspace->offset) && (0 == list->tail->offset)
			) {
				list->tail->length += workspace->length;
				_freeList->add(workspace);
			} else {
				list->tail->next = workspace;
				list->tail = list->tail->next;
			}
		} else {
			_freeList->add(workspace);
		}
	}
}

void
MM_EvacuatorWorklist::add(MM_EvacuatorBase::Workspace *workspace, bool defer)
{
	/* expect singleton workspace reject null or empty workspace */
	Debug_MM_true((NULL != workspace) && (0 < workspace->length) && (NULL == workspace->next));
	assertWorklistInvariant();

	uintptr_t work = 0;
	if (NULL != workspace) {
		if (0 < workspace->length) {
			work += volume(workspace);
			List *list = defer ? &_deferred : &_default;
			addWorkToList(list, workspace);
		} else {
			_freeList->add(workspace);
		}
	}
	_volume += work;

	assertWorklistInvariant();
}

void
MM_EvacuatorWorklist::add(List *workspaces, bool defer)
{
	Debug_MM_true(NULL != workspaces);
	Debug_MM_true((NULL != workspaces->head) && (NULL != workspaces->tail));
	Debug_MM_true(NULL == workspaces->tail->next);
	assertWorklistInvariant();

	if ((NULL != workspaces) && (NULL != workspaces->head)) {
		/* append workspaces from input list to default or deferred worklist */
		List *list = defer ? &_deferred : &_default;
		if (NULL != list->head) {
			list->tail->next = workspaces->head;
		} else {
			list->head = workspaces->head;
		}
		list->tail = workspaces->tail;
		/* sum volumes of added workspaces into worklist volume */
		uintptr_t v = 0;
		for (const MM_EvacuatorBase::Workspace *w = workspaces->head; w != NULL; w = w->next) {
			v += volume(w);
		}
		_volume += v;
		/* cut appended workspaces from input list */
		workspaces->head = workspaces->tail = NULL;
	}

	assertWorklistInvariant();
}

MM_EvacuatorBase::Workspace *
MM_EvacuatorWorklist::next(bool deferred)
{
	assertWorklistInvariant();

	/* select a list in preference order */
	List *list = deferred ? &_deferred : &_default;
	MM_EvacuatorBase::Workspace *work = list->head;
	if (NULL == work) {
		list = deferred ? &_default : &_deferred;
		work = list->head;
	}

	/* if a head workspace is available from selected list pull and return it */
	if (NULL != work) {
		Debug_MM_true((work != list->tail) || (NULL == work->next));
		Debug_MM_true((work == list->tail) || (NULL != work->next));

		_volume -= volume(work);
		list->head = work->next;
		if (NULL == list->head) {
			list->tail = NULL;
		}
		work->next = NULL;

		Debug_MM_true((NULL != work->base) && (0 < work->length));
	}

	assertWorklistInvariant();
	return work;
}

void
MM_EvacuatorWorklist::flush()
{
	assertWorklistInvariant();

	_volume = 0;

	while (NULL != _default.head) {
		_default.head = _freeList->flush(_default.head);
	}
	while (NULL != _deferred.head) {
		_deferred.head = _freeList->flush(_deferred.head);
	}
	_default.tail = _deferred.tail = NULL;

	assertWorklistInvariant();
}

uintptr_t
MM_EvacuatorWorklist::volume(const MM_EvacuatorBase::Workspace *work) const
{
	/* volume is length in bytes */
	if (NULL != work) {
		return (0 == work->offset) ? work->length : (work->length * _evacuator->getReferenceSlotSize());
	}
	return 0;
}

MM_EvacuatorBase::Workspace *
MM_EvacuatorFreelist::flush(MM_EvacuatorBase::Workspace *workspace)
{
	MM_EvacuatorBase::Workspace *next = workspace->next;

	workspace->base = NULL;
	workspace->length = 0;
	workspace->offset = 0;
	workspace->next = NULL;

	return next;
}

/* Freelist methods */

MM_EvacuatorBase::Workspace *
MM_EvacuatorFreelist::next()
{
	Debug_MM_true((0 == _count) == (NULL == _head));

	/* refresh backing underflow chunk list from system (non-heap) memory */
	if (0 == _count) {
		refresh();
	}

	/* assert and bail if out of system memory */
	Assert_MM_true((0 < _count) && (NULL != _head));

	MM_EvacuatorBase::Workspace *freespace = _head;
	_head = freespace->next;
	_count -= 1;

	freespace->next = NULL;
	freespace->base = NULL;
	freespace->length = 0;
	freespace->offset = 0;

	return freespace;
}

MM_EvacuatorBase::Workspace *
MM_EvacuatorFreelist::add(MM_EvacuatorBase::Workspace *free)
{
	Debug_MM_true((0 == _count) == (NULL == _head));

	MM_EvacuatorBase::Workspace *next = NULL;
	if (NULL != free) {
		next = free->next;
		free->next = _head;
		free->base = NULL;
		free->length = 0;
		free->offset = 0;
		_head = free;
		_count += 1;
	}

	Debug_MM_true((0 == _count) == (NULL == _head));

	return next;
}

uintptr_t
MM_EvacuatorFreelist::refresh()
{
	if ((NULL == _underflow) || (0 == _count)) {
		/* allocate a new underflow chunk and link it in at head of underflow list*/
		UnderflowChunk *underflowChunk = (UnderflowChunk *)_forge->allocate(sizeof(UnderflowChunk), OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
		underflowChunk->next = _underflow;
		_underflow = underflowChunk;

		/* add free elements from underflow chunk to free list */
		MM_EvacuatorBase::Workspace *workspace = underflowChunk->workspace;
		MM_EvacuatorBase::Workspace *end = workspace + workspace_chunksize;
		while (workspace < end) {
			add(workspace);
			workspace += 1;
		}
	}

	Debug_MM_true(0 < _count);
	return _count;
}

void
MM_EvacuatorFreelist::reload()
{
	if (NULL != _underflow) {
		_count = 0;
		_head = NULL;
		for (UnderflowChunk *underflowChunk = _underflow; NULL != underflowChunk; underflowChunk = underflowChunk->next) {
			/* add free elements from underflow chunk to free list */
			MM_EvacuatorBase::Workspace *workspace = underflowChunk->workspace;
			MM_EvacuatorBase::Workspace *end = workspace + workspace_chunksize;
			while (workspace < end) {
				Debug_MM_true(NULL == workspace->base);
				add(workspace);
				workspace += 1;
			}
		}
	} else {
		refresh();
	}
}

void
MM_EvacuatorFreelist::reset()
{
	/* deallocate all underflow chunks allocated from system memory (forged) */
	while (NULL != _underflow) {
		UnderflowChunk *next = _underflow->next;
		_forge->free(_underflow);
		_underflow = next;
	}
	_head = NULL;
	_count = 0;
}



