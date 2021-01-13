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
 * Linked list of free MM_EvacuatorBase::Workspace is accessed only by the evacuator that owns it. It
 * is backed by chunks of MM_EvacuatorBase::Workspace allocated and freed in system memory (off-heap).
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
		MM_EvacuatorBase::Workspace workspace[workspace_chunksize];
		UnderflowChunk *next;
	} UnderflowChunk;

	MM_EvacuatorBase::Workspace *_head;	/* head of free list (last one in) */
	volatile uintptr_t _count;		/* number of elements on free list */
	UnderflowChunk *_underflow;		/* head of list of underflow chunks allocated from system memory */
	MM_Forge * const _forge;		/* memory allocator for underflow chunks */

protected:

/*
 * Function members
 */
public:
	/**
	 * Return the number of contained free elements
	 */
	uintptr_t getCount() const { return _count; }

	/**
	 * Mark workspace as free and return its successor
	 */
	MM_EvacuatorBase::Workspace *flush(MM_EvacuatorBase::Workspace *workspace);

	/**
	 * Get the next available free workspace. This will assert if it fails.
	 *
	 * @return the next available free workspace, or assert failure
	 */
	MM_EvacuatorBase::Workspace *next();

	/**
	 * Add a free workspace at the head of the list. This may include workspaces taken from
	 * forge memory owned by another evacuator.
	 *
	 * @param free the workspace to add
	 * @return the freed workspace successor
	 */
	MM_EvacuatorBase::Workspace *add(MM_EvacuatorBase::Workspace *free);

	/**
	 * Claim 0 or more free elements from system (forge) memory or C++ stack space
	 *
	 * @return the number of free elements added
	 */
	uintptr_t refresh();

	void reload();

	/**
	 * Releases all underflow chunks, if any are allocated, and resets list to empty state.
	 */
	void reset();

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
		MM_EvacuatorBase::Workspace *head;		/* points to head of list */
		MM_EvacuatorBase::Workspace *tail;		/* points to tail of list */
	} List;

private:
	MM_EvacuatorBase *_evacuator;			/* abridged view of bound MM_Evacuator instance */
	MM_EvacuatorFreelist * const _freeList;	/* backing store for workspace allocation */
	volatile uintptr_t _volume;				/* (bytes) volume of work contained in list */
	List _default;							/* FIFO list of priority workspaces */
	List _deferred;							/* FIFO list of deferred workspaces */

protected:

/*
 * Function members
 */
private:
#if defined(EVACUATOR_DEBUG)
	MMINLINE void assertWorklistInvariant() const;
#else
#define assertWorklistInvariant()
#endif /* defined(EVACUATOR_DEBUG) */

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
	const MM_EvacuatorBase::Workspace *peek(bool deferred) const;

	/**
	 * Append a single workspace to the default or deferred list.
	 *
	 * @param workspace the workspace to add
	 * @param defer whether to add to deferred list (true) or preferred list (false, default)
	 * @return false if not added (0 length or NULL)
	 */
	void add(MM_EvacuatorBase::Workspace *workspace, bool defer = false);

	/**
	 * Append a list of workspaces to the list.
	 *
	 * @param work the workspaces to add
	 * @param defer whether to add to deferred list (true) or preferred list (false, default)
	 */
	void add(List *workspaces, bool defer = false);

	/**
	 * Append a workspace to a FIFO list
	 * @param list the list
	 * @param workspace the workspace
	 */
	void addWorkToList(List *list, MM_EvacuatorBase::Workspace *workspace) const;

	/**
	 * Pull the workspace from the head of the list.
	 *
	 * @param deferred if true drain deferred list before preferred list
	 * @return the next workspace, or NULL
	 */
	MM_EvacuatorBase::Workspace *next(bool deferred);

	/**
	 * Just clear count and list pointers. The freelist owns the forge memory
	 * allocated for one evacuator and this worklist may contain workspaces
	 * allocated and distributed from other evacuator freelists. Each evacuator
	 * reclaims it's allocated forge memory at the beginning of each gc cycle.
	 * No forge memory is reclaimed here.
	 */
	void flush();

	/**
	 * Get the volume of work from a workspace
	 *
	 * @param[in] work pointer to workspace.
	 */
	uintptr_t volume(const MM_EvacuatorBase::Workspace *work) const;

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
