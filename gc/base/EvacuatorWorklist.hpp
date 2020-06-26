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

#ifndef EVACUATORWORKLIST_HPP_
#define EVACUATORWORKLIST_HPP_

#include "omrmutex.h"

#include "Base.hpp"
#include "EvacuatorBase.hpp"
#include "Forge.hpp"
#include "ForwardedHeader.hpp"

/**
 * Workspaces encapsulate copied material that needs to be scanned (ie, work released from outside
 * copyspaces or large object copyspace). They are queued on evacuator worklists until pulled onto
 * the scan stack for scanning. Workspaces not so queued are reset and freed to evacuator freelists.
 *
 * Location and size of a contiguous range of material copied to survivor or tenure space. For
 * split array workspaces the length and offset fields represent array slots, the offset is shifted
 * (0-based -> 1-based) and the base points to the array object. For all other workspaces the base
 * points to the first object in the workspace, the offset field is 0 and the length field represents
 * the volume of work in the workspace in bytes (respecting heap object alignment).
 */
typedef struct MM_EvacuatorWorkspace {
	omrobjectptr_t base;			/* points to pointer array or work released from a copyspace */
	uintptr_t length;				/* number of array slots or copied workspace bytes to be scanned */
	uintptr_t offset;				/* >0 for split arrays only, offset from base to first slot to scan */
	MM_EvacuatorWorkspace *next;	/* points to next workspace in queue, or NULL */
} MM_EvacuatorWorkspace;

/**
 * Linked list of free MM_EvacuatorWorkspace is accessed only by the evacuator that owns it. It
 * is backed by chunks of MM_EvacuatorWorkspace allocated and freed in system memory (off-heap).
 */
class MM_EvacuatorFreelist : public MM_Base
{
/*
 * Data members
 */
public:

private:
	/* if starved for free elements, allocate a chunk of this size from system memory and refresh */
	static const uintptr_t workspace_chunksize = 128;

	/* underflow chunk obtained from system memory to seed or refresh free list */
	typedef struct UnderflowChunk {
		MM_EvacuatorWorkspace workspace[workspace_chunksize];
		UnderflowChunk *next;
	} UnderflowChunk;

	MM_EvacuatorWorkspace *_head;	/* head of free list (last one in) */
	volatile uintptr_t _count;		/* number of elements on free list */
	UnderflowChunk *_underflow;		/* head of list of underflow chunks allocated from system memory */
	MM_Forge *_forge;				/* memory allocator for underflow chunks */

protected:

/*
 * Function members
 */
public:
	/**
	 * Mark workspace as free and return its successor
	 */
	MM_EvacuatorWorkspace *flush(MM_EvacuatorWorkspace *workspace)
	{
		MM_EvacuatorWorkspace *next = workspace->next;

		workspace->base = NULL;
		workspace->length = 0;
		workspace->offset = 0;
		workspace->next = NULL;

		return next;
	}

	/**
	 * Return the number of contained free elements
	 */
	uintptr_t getCount() const { return _count; }

	/**
	 * Get the next available free workspace. This will assert if it fails.
	 *
	 * @return the next available free workspace, or assert failure
	 */
	MM_EvacuatorWorkspace *
	next()
	{
		Debug_MM_true((0 == _count) == (NULL == _head));

		/* refresh backing underflow chunk list from system (non-heap) memory */
		if (0 == _count) {
			refresh();
		}

		/* assert and bail if out of system memory */
		Assert_MM_true((0 < _count) && (NULL != _head));

		MM_EvacuatorWorkspace *freespace = _head;
		_head = freespace->next;
		_count -= 1;

		freespace->next = NULL;
		freespace->base = NULL;
		freespace->length = 0;
		freespace->offset = 0;

		return freespace;
	}

	/**
	 * Add a free workspace at the head of the list. This may include workspaces taken from
	 * forge memory owned by another evacuator.
	 *
	 * @param free the workspace to add
	 * @return the freed workspace successor
	 */
	MM_EvacuatorWorkspace *
	add(MM_EvacuatorWorkspace *free)
	{
		Debug_MM_true((0 == _count) == (NULL == _head));

		MM_EvacuatorWorkspace *next = NULL;
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

	/**
	 * Claim 0 or more free elements from system (forge) memory or C++ stack space
	 *
	 * @return the number of free elements added
	 */
	uintptr_t
	refresh()
	{
		if ((NULL == _underflow) || (0 == _count)) {
			/* allocate a new underflow chunk and link it in at head of underflow list*/
			UnderflowChunk *underflowChunk = (UnderflowChunk *)_forge->allocate(sizeof(UnderflowChunk), OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
			underflowChunk->next = _underflow;
			_underflow = underflowChunk;

			/* add free elements from underflow chunk to free list */
			MM_EvacuatorWorkspace *workspace = underflowChunk->workspace;
			MM_EvacuatorWorkspace *end = workspace + workspace_chunksize;
			while (workspace < end) {
				add(workspace);
				workspace += 1;
			}
		}

		Debug_MM_true(0 < _count);
		return _count;
	}

	void
	reload()
	{
		if (NULL != _underflow) {
			_count = 0;
			_head = NULL;
			for (UnderflowChunk *underflowChunk = _underflow; NULL != underflowChunk; underflowChunk = underflowChunk->next) {
				/* add free elements from underflow chunk to free list */
				MM_EvacuatorWorkspace *workspace = underflowChunk->workspace;
				MM_EvacuatorWorkspace *end = workspace + workspace_chunksize;
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

	/**
	 * Releases all underflow chunks, if any are allocated, and resets list to empty state.
	 */
	void
	reset()
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

	/**
	 * Constructor.
	 */
	MM_EvacuatorFreelist(MM_Forge *forge)
		: MM_Base()
		, _head(NULL)
		, _count(0)
		, _underflow(NULL)
		, _forge(forge)
	{ }
};

/**
 * FIFO queue of workspaces. Other evacuator threads may access this list if their own queues run dry. The
 * queue volume is exposed to other threads as a volatile value to enable them to check queue volume without
 * taking the controller and evacuator mutexes. Stalled evacuators must acquire both the controller mutex
 * and the running evacuator mutex (in that order) to pull work from a running evacuator's worklist.
 */
class MM_EvacuatorWorklist : public MM_Base
{
/*
 * Data members
 */
public:
	typedef struct List {
		MM_EvacuatorWorkspace *head;	/* points to head of list */
		MM_EvacuatorWorkspace *tail;	/* points to tail of list */
	} List;

private:
	MM_EvacuatorBase *_evacuator;		/* abridged view of bound MM_Evacuator instance */
	MM_EvacuatorFreelist *_freeList;	/* backing store for workspace allocation */
	volatile uintptr_t _volume;			/* (bytes) volume of work contained in list */
	List _default;						/* FIFO list of priority workspaces */
	List _deferred;						/* FIFO list of deferred workspaces */

protected:

/*
 * Function members
 */
private:
protected:
public:
	/**
	 * Returns sum of work volumes of workspaces in the list (volatile).
	 */
	uintptr_t volume() const { return _volume; }

	/**
	 * Peek at the workspace at the head of the list, which may be NULL
	 *
	 * @param deferred if true drain deferred list before preferred list
	 * @return pointer to const workspace at head of deferred or preferred list
	 */
	const MM_EvacuatorWorkspace *
	peek(bool deferred) const
	{
		assertWorklistInvariant();

		/* select a list in preference order */
		const List *list = deferred ? &_deferred : &_default;
		const MM_EvacuatorWorkspace *work = list->head;
		if (NULL == work) {
			list = deferred ? &_default : &_deferred;
			work = list->head;
		}

		assertWorklistInvariant();
		return work;
	}

	void
	addWorkToList(List *list, MM_EvacuatorWorkspace *workspace) const
	{
		/* expect singleton workspace reject null or empty workspace */
		Debug_MM_true((NULL != workspace) && (0 < workspace->length) && (NULL == workspace->next));
		if (NULL != workspace) {
			if (0 < workspace->length) {
				if (NULL == list->head) {
					list->head = list->tail = workspace;
				} else if (list->head == list->tail) {
					workspace->next = list->head;
					list->head = workspace;
				} else {
					list->tail->next = workspace;
					list->tail = list->tail->next;
				}
			} else {
				_freeList->add(workspace);
			}
		}
	}

	/**
	 * Append a single workspace to the default or deferred list.
	 *
	 * @param workspace the workspace to add
	 * @param defer whether to add to deferred list (true) or preferred list (false, default)
	 * @return false if not added (0 length or NULL)
	 */
	void
	add(MM_EvacuatorWorkspace *workspace, bool defer = false)
	{
		/* expect singleton workspace reject null or empty workspace */
		Debug_MM_true((NULL != workspace) && (0 < workspace->length) && (NULL == workspace->next));
		assertWorklistInvariant();

		if (NULL != workspace) {

			/* append work to worklist */
			workspace->next = NULL;
			List *list = defer ? &_deferred : &_default;
			addWorkToList(list, workspace);

			VM_AtomicSupport::add(&_volume, volume(workspace));
		}

		assertWorklistInvariant();
	}

	/**
	 * Append a list of workspaces to the list.
	 *
	 * @param work the workspace to add
	 * @param defer whether to add to deferred list (true) or preferred list (false, default)
	 */
	void
	add(List *workspaces, bool defer = false)
	{
		assertWorklistInvariant();

		/* drain workspaces into worklist */
		while (NULL != workspaces->head) {
			MM_EvacuatorWorkspace *work = workspaces->head;
			Debug_MM_true(0 < work->length);
			workspaces->head = work->next;
			work->next = NULL;
			add(work, defer);
		}
		workspaces->tail = NULL;

		assertWorklistInvariant();
	}

	/**
	 * Pull the workspace from the head of the list.
	 *
	 * @param deferred if true drain deferred list before preferred list
	 * @return the next workspace, or NULL
	 */
	MM_EvacuatorWorkspace *
	next(bool deferred)
	{
		assertWorklistInvariant();

		/* select a list in preference order */
		List *list = deferred ? &_deferred : &_default;
		MM_EvacuatorWorkspace *work = list->head;
		if (NULL == work) {
			list = deferred ? &_default : &_deferred;
			work = list->head;
		}

		/* if a head workspace is available from selected list pull and return it */
		if (NULL != work) {
			Debug_MM_true((work != list->tail) || (NULL == work->next));
			Debug_MM_true((work == list->tail) || (NULL != work->next));

			VM_AtomicSupport::subtract(&_volume, volume(work));
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

	/**
	 * Just clear count and list pointers. The freelist owns the forge memory
	 * allocated for one evacuator and this worklist may contain workspaces
	 * allocated and distributed from other evacuator freelists. Each evacuator
	 * reclaims it's allocated forge memory at the beginning of each gc cycle.
	 * No forge memory is reclaimed here.
	 */
	void
	flush()
	{
		assertWorklistInvariant();

		VM_AtomicSupport::set(&_volume, 0);
		VM_AtomicSupport::readBarrier();

		while (NULL != _default.head) {
			_default.head = _freeList->flush(_default.head);
		}
		while (NULL != _deferred.head) {
			_deferred.head = _freeList->flush(_deferred.head);
		}
		_default.tail = _deferred.tail = NULL;

		assertWorklistInvariant();
	}

	/**
	 * Get the volume of work from a workspace
	 *
	 * @param[in] work pointer to workspace.
	 */
	uintptr_t volume(const MM_EvacuatorWorkspace *work) const
	{
		/* volume is length in bytes */
		if (NULL != work) {
			return (0 == work->offset) ? work->length : (work->length * _evacuator->getReferenceSlotSize());
		}
		return 0;
	}


	void
	assertWorklistInvariant() const
	{
		Debug_MM_true((0 == _volume) == ((NULL == _default.head) && (NULL == _deferred.head)));
		Debug_MM_true((NULL == _default.head) == (NULL == _default.tail));
		Debug_MM_true((NULL == _default.head) || (NULL == _default.tail->next));
		Debug_MM_true((NULL == _deferred.head) == (NULL == _deferred.tail));
		Debug_MM_true((NULL == _deferred.head) || (NULL == _deferred.tail->next));
	}

	/* evacuator is effectively const but cannot be set in evacuator constructor */
	void evacuator(MM_EvacuatorBase *evacuator) { _evacuator = evacuator; }

	/**
	 * Constructor
	 */
	MM_EvacuatorWorklist(MM_EvacuatorFreelist *freeList)
		: MM_Base()
		, _evacuator(NULL)
		, _freeList(freeList)
		, _volume(0)
	{
		_default.head = _default.tail = NULL;
		_deferred.head = _deferred.tail = NULL;
	}
};

#endif /* EVACUATORWORKLIST_HPP_ */
