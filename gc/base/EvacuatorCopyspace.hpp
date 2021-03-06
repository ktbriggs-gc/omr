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

#ifndef EVACUATORCOPYSPACE_HPP_
#define EVACUATORCOPYSPACE_HPP_

#include "Base.hpp"
#include "EvacuatorBase.hpp"
#include "EvacuatorWhitelist.hpp"
#include "SlotObject.hpp"

class MM_EvacuatorCopyspace : public MM_Base
{
/*
 * Data members
 */
private:

protected:
	MM_EvacuatorBase *_evacuator;	/* abridged view of bound MM_Evacuator instance */
	uint8_t *_base;					/* points to start of copyspace */
	uint8_t *_copy;					/* points to current copy head */
	uint8_t *_end;					/* points to end of copyspace */
	uint32_t _flags;				/* extensible bitmap of flags */

	/* enumeration of flag bits may be extended past copyEndFlag by subclasses */
	typedef enum copyspaceFlags {
		  isLOAFlag = 1				/* set if base is in the large object area (LOA) of the heap */
		, copyEndFlag = isLOAFlag	/* marks end of copyspace flags, subclasses can extend flag bit enumeration from here */
	} copyspaceFlags;

public:
/*
 * Function members
 */
private:

protected:

public:
	/**
	 * Basic array constructor obviates need for stdlibc++ linkage in gc component libraries. Array
	 * is allocated from forge as contiguous block sized to contain requested number of elements and
	 * must be freed using MM_Forge::free() when no longer needed. See MM_Evacuator::tearDown().
	 *
	 * @param count the number of aray elements to instantiate
	 * @return a pointer to instantiated array
	 */
	static MM_EvacuatorCopyspace *
	newInstanceArray(MM_EvacuatorBase *evacuator, MM_Forge *forge, uintptr_t count)
	{
		MM_EvacuatorCopyspace *copyspace = (MM_EvacuatorCopyspace *)forge->allocate(sizeof(MM_EvacuatorCopyspace) * count, OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());

		if (NULL != copyspace) {
			for (uintptr_t i = 0; i < count; i += 1) {
				MM_EvacuatorCopyspace *space = new(copyspace + i) MM_EvacuatorCopyspace(evacuator);
				if (NULL == space) {
					return NULL;
				}
			}
		}

		return copyspace;
	}

	/**
	 * Get the location of the base of the copyspace
	 */
	uint8_t *getBase() { return _base; }

	/**
	 * Get the location of the copy head
	 */
	uint8_t *getCopyHead() { return _copy; }

	/**
	 * Get the location of the end of the copyspace
	 */
	uint8_t *getEnd() { return _end; }

	/**
	 * Return the total size of the copyspace
	 */
	uintptr_t getSize() { return (uintptr_t)(_end - _base); }

	/**
	 * Return the number of bytes free to receive copy
	 */
	uintptr_t getWhiteSize() { return (uintptr_t)(_end - _copy); }

	/**
	 * Return the number of bytes that hold copied material
	 */
	uintptr_t getCopySize() { return (uintptr_t)(_copy - _base); }

	/**
	 * Return the total number of bytes spanned by copyspace (copysize + freesize)
	 */
	uintptr_t getTotalSize() { return (uintptr_t)(_end - _base); }

	/**
	 * Set/test copyspace for containment in large object area
	 */
	bool isLOA() { return (0 != ((uintptr_t)isLOAFlag & _flags)); }

	void setLOA(bool isLOA)
	{
		if (isLOA) {
			_flags |= (uintptr_t)isLOAFlag;
		} else {
			_flags &= ~(uintptr_t)isLOAFlag;
		}
	}

	/**
	 * Load free memory into the copyspace. The copyspace must be empty before the call.
	 */
	void
	setCopyspace(uint8_t *base, uint8_t *copy, uintptr_t length, bool isLOA = false)
	{
		Debug_MM_true(0 == getWhiteSize());
		Debug_MM_true(0 == getCopySize());

		_base = base;
		_copy = copy;
		_end = _base + length;

		_flags = 0;
		if (isLOA) {
			setLOA(isLOA);
		}

		Debug_MM_true((_base <= _copy) && (_copy <= _end));
	}

	/**
	 * Advance the copy pointer to end of most recently copied object.
	 *
	 * @param copiedBytes number of bytes copied (consumed size of most recently copied object)
	 * @return pointer to the new copy head (location that will receive next object)
	 */
	void
	advanceCopyHead(uintptr_t copiedBytes)
	{
		_copy += copiedBytes;
	}

	/**
	 * Split current contents, returning a pointer to and length of current copied material. The
	 * copyspace will be rebased to include only the whitespace remaining at the copy head.
	 *
	 * @param volume (NULLable) pointer to location that will receive volume of work released
	 * @return pointer to work split from head, or NULL
	 */
	uint8_t *
	rebase(uintptr_t *volume = NULL)
	{
		uint8_t *work = NULL;

		/* return volume of work between base and copy head */
		if (NULL != volume) {
			*volume = getCopySize();
		}

		/* set workspace base to current base and advance base to copy head to clear work from copyspace */
		if (0 < getCopySize()) {
			work = _base;
			_base = _copy;
		}

		return work;
	}

	/**
	 * Trim remaining free space from end and return it as whitespace.
	 *
	 * @return whitespace from end of copyspace, or NULL if none available
	 */
	MM_EvacuatorWhitespace *
	trim()
	{
		MM_EvacuatorWhitespace *freespace = NULL;

		if (_end > _copy) {
			freespace = MM_EvacuatorWhitespace::whitespace(_copy, getWhiteSize(), _evacuator->compressObjectReferences(), isLOA());
			_end = _copy;
		}

		return freespace;
	}

	/**
	 * Reset this copyspace to an empty state. Copyspace must be full (no trailing whitespace) and
	 * all work contained between base and copy head consumed before the call.
	 */
	void reset()
	{
		Debug_MM_true(_copy == _end);

		/* this effectively makes the copyspace empty but leaves some information for debugging */
		_base = _copy = _end;
		_flags = 0;
	}

	/**
	 * Constructor
	 */
	MM_EvacuatorCopyspace(MM_EvacuatorBase *evacuator)
		: MM_Base()
		, _evacuator(evacuator)
		, _base(NULL)
		, _copy(NULL)
		, _end(NULL)
		, _flags(0)
	{ }
};

#endif /* EVACUATORCOPYSPACE_HPP_ */
