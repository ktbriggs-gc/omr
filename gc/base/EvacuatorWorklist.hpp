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
	uintptr_t getCount() { return _count; }

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

		MM_EvacuatorWorkspace *next = free->next;
		free->next = _head;
		free->base = NULL;
		free->length = 0;
		free->offset = 0;
		_head = free;
		_count += 1;

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
private:
	MM_EvacuatorBase *_evacuator;		/* abridged view of bound MM_Evacuator instance */
	MM_EvacuatorFreelist *_freeList;	/* backing store for workspace allocation */
	MM_EvacuatorWorkspace *_head;		/* points to head of worklist */
	MM_EvacuatorWorkspace *_tail;		/* points to tail of worklist */
	volatile uintptr_t _volume;			/* (bytes) volume of work contained in list */

protected:
public:

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
	 */
	const MM_EvacuatorWorkspace *peek() const { return _head; }

	/**
	 * Append a workspace to the list.
	 *
	 * @param work the workspace to add
	 */
	void
	add(MM_EvacuatorWorkspace *work, bool push = false)
	{
		Debug_MM_true((0 == _volume) == (NULL == _head));
		Debug_MM_true((NULL == _head) == (NULL == _tail));
		Debug_MM_true((_head != _tail) || (NULL == _head) || (volume(_head) == _volume));
		Debug_MM_true((NULL != work) && (NULL != work->base) && (0 < work->length));

		VM_AtomicSupport::add(&_volume, volume(work));
		VM_AtomicSupport::readBarrier();

		/* if work not split array workspace and volume is greater than head insert as head else append as tail */
		work->next = NULL;
		push |= (0 == work->offset) && (volume() < MM_EvacuatorBase::min_split_indexable_size) && (volume(work) > volume(_head));
		if (NULL == _head) {
			_head = _tail = work;
		} else if (push) {
			work->next = _head;
			_head = work;
		} else if (_head == _tail) {
			work->next = _head;
			_head = work;
		} else {
			_tail->next = work;
			_tail = _tail->next;
		}

		Debug_MM_true((0 == _volume) == (NULL == _head));
		Debug_MM_true((NULL == _head) == (NULL == _tail));
		Debug_MM_true((_head != _tail) || (NULL == _head) || (volume(_head) == _volume));
	}

	/**
	 * Pull the workspace from the head of the list.
	 *
	 * @return the next workspace, or NULL
	 */
	MM_EvacuatorWorkspace *
	next()
	{
		Debug_MM_true((0 == _volume) == (NULL == _head));
		Debug_MM_true((NULL == _head) == (NULL == _tail));
		Debug_MM_true((_head != _tail) || (NULL == _head) || (volume(_head) == _volume));

		MM_EvacuatorWorkspace *work = _head;
		if (NULL != work) {
			Debug_MM_true((work != _tail) || (NULL == work->next));
			Debug_MM_true((work == _tail) || (NULL != work->next));

			VM_AtomicSupport::subtract(&_volume, volume(work));
			VM_AtomicSupport::readBarrier();

			_head = (work == _tail) ? (_tail = NULL) : work->next;
			work->next = NULL;

			Debug_MM_true((NULL != work->base) && (0 < work->length));
		}

		Debug_MM_true((0 == _volume) == (NULL == _head));
		Debug_MM_true((NULL == _head) == (NULL == _tail));
		Debug_MM_true((_head != _tail) || (NULL == _head) || (volume(_head) == _volume));
		Debug_MM_true((NULL == work) || ((NULL != work->base) && (0 < work->length)));
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
		Debug_MM_true((0 == _volume) == (NULL == _head));
		Debug_MM_true((NULL == _head) == (NULL == _tail));

		VM_AtomicSupport::set(&_volume, 0);
		VM_AtomicSupport::readBarrier();

		while (NULL != _head) {
			_head = _freeList->flush(_head);
		}
		_tail = NULL;

		Debug_MM_true((0 == _volume) == (NULL == _head));
	}

	/**
	 * Get the volume of work from a workspace
	 *
	 * @param[in] work pointer to workspace.
	 */
	uintptr_t volume(const MM_EvacuatorWorkspace *work)
	{
		uintptr_t volume = 0;

		if (NULL != work) {

			/* volume is length in bytes */
			volume = work->length;

			/* for split array workspaces offset is >0 (start slot is array[offset-1]) */
			if (0 < work->offset) {

				/* the length the number of slots to scan */
				volume *= _evacuator->getReferenceSlotSize();
			}
		}

		return volume;
	}

	/**
	 * Constructor
	 */
	MM_EvacuatorWorklist(MM_EvacuatorBase *evacuator, MM_EvacuatorFreelist *freeList)
		: MM_Base()
		, _evacuator(evacuator)
		, _freeList(freeList)
		, _head(NULL)
		, _tail(NULL)
		, _volume(0)
	{ }
};

#endif /* EVACUATORWORKLIST_HPP_ */
