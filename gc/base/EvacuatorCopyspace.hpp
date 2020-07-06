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
	uintptr_t _flags;				/* extensible bitmap of flags */

	/* enumeration of flag bits may be extended past copyEndFlag by subclasses */
	typedef enum copyspaceFlags {
		  isTenureFlag = 1			/* set if original base was in tenure (otherwise it was in survivor) */
		, isLOAFlag = 2				/* set if base is in the large object area (LOA) of the heap */
		, isStackOverflowFlag = 4	/* set if copy head advanced while evacuator was in stack overflow condition */
		, copyEndFlag = isStackOverflowFlag	/* marks end of copyspace flags, subclasses can extend flag bit enumeration from here */
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
	 * @param forge the system memory allocator
	 * @return a pointer to instantiated array indexed by [survivor,tenure]
	 */
	static MM_EvacuatorCopyspace *
	newInstanceArray(MM_Forge *forge)
	{
		MM_EvacuatorCopyspace *copyspace = (MM_EvacuatorCopyspace *)forge->allocate((2 * sizeof(MM_EvacuatorCopyspace)), OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
		if (NULL != copyspace) {
			new(copyspace + MM_EvacuatorBase::survivor) MM_EvacuatorCopyspace();
			new(copyspace + MM_EvacuatorBase::tenure) MM_EvacuatorCopyspace();
		}
		return copyspace;
	}

	/**
	 * Get the evacuation region containing the original base address of this scanspace.
	 *
	 * Note that all ranges are inclusive of base and exclusive of end. This method assumes
	 * that tenure space is always below nursery space and that the copyspace contains
	 * tenure or survivor space.
	 *
	 * Also note that when copy head advances to end (copy == end) the evacuation region
	 * becomes ambiguous. Copyspace sets the isTenureFlag whenever whitespace is set into
	 * into it and is presumes to retain respective survivor or tenure containment when
	 * copy head is at end.
	 */
	MM_EvacuatorBase::Region
	getEvacuationRegion() const
	{
		assertCopyspaceInvariant();

		MM_EvacuatorBase::Region presumed = MM_EvacuatorBase::survivor;
		if (isTenureFlag == (_flags & isTenureFlag)) {
			presumed = MM_EvacuatorBase::tenure;
		}

		Debug_MM_true(presumed == _evacuator->getEvacuationRegion(_base, presumed));
		return presumed;
	}

	/**
	 * Get the location of the base of the copyspace
	 */
	uint8_t *getBase() const { return _base; }

	/**
	 * Get the location of the copy head
	 */
	uint8_t *getCopyHead() const { return _copy; }

	/**
	 * Get the location offset relative to copyspace base
	 *
	 * @param baseOffset number of bytes to offset base pointer (must be object aligned)
	 */
	uint8_t *getLimit(uintptr_t baseOffset) const { return _base + baseOffset; }

	/**
	 * Get the location of the end of the copyspace
	 */
	uint8_t *getEnd() const { return _end; }

	/**
	 * Return the total size of the copyspace
	 */
	uintptr_t getSize() const { return (uintptr_t)(_end - _base); }

	/**
	 * Return the number of bytes free to receive copy
	 */
	uintptr_t getWhiteSize() const { return (uintptr_t)(_end - _copy); }

	/**
	 * Return the number of bytes that hold copied material
	 */
	uintptr_t getCopySize() const { return (uintptr_t)(_copy - _base); }

	/**
	 * Return the total number of bytes spanned by copyspace (copysize + freesize)
	 */
	uintptr_t getTotalSize() const { return (uintptr_t)(_end - _base); }

	/**
	 * Test copyspace for containment in tenure space
	 */
	bool isTenure() const { return (0 != ((uintptr_t)isTenureFlag & _flags)); }

	/**
	 * Test copyspace for containment in large object area
	 */
	bool isLOA() const { return (0 != ((uintptr_t)isLOAFlag & _flags)); }

	/**
	 * Set/test copyspace for containment of objects copied while evacuator wqas in stack overflow condition.
	 */
	bool isStackOverflow() const { return (0 != ((uintptr_t)isStackOverflowFlag & _flags)); }
	void setStackOverflow(bool isStackOverflow)
	{
		Debug_MM_true(!isStackOverflow || (0 < getCopySize()));
		if (isStackOverflow) {
			_flags |= (uintptr_t)isStackOverflowFlag;
		} else {
			_flags &= ~(uintptr_t)isStackOverflowFlag;
		}
	}

	/**
	 * Load free memory into the copyspace. The copyspace must be empty before the call.
	 */
	void
	setCopyspace(MM_EvacuatorBase::Region region, uint8_t *base, uint8_t *copy, uintptr_t length, bool isLOA = false)
	{
		Debug_MM_true(region == _evacuator->getEvacuationRegion(base, region));
		Debug_MM_true((region >= 0) && (region < MM_EvacuatorBase::evacuate));
		Debug_MM_true(0 == getWhiteSize());
		Debug_MM_true(0 == getCopySize());

		_base = base;
		_copy = copy;
		_end = _base + length;

		_flags = (region == MM_EvacuatorBase::tenure) ? isTenureFlag : 0;
		if (isLOA) {
			_flags |= isLOA;
		}

		assertCopyspaceInvariant();
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
		assertCopyspaceInvariant();
	}

	/**
	 * Split current contents, returning a pointer to and length of current copied material. The
	 * copyspace will be rebased to include only the whitespace remaining at the copy head.
	 *
	 * @param volume (NULLable) pointer to location that will receive volume of work released
	 * @return pointer to work split from base to copy head (the base pointer is at the head of an object), or NULL
	 */
	uint8_t *
	rebase(uintptr_t *volume)
	{
		assertCopyspaceInvariant();
		uint8_t *work = NULL;

		/* if copyspace holds unscanned work return current base and volume of work between base and copy head */
		*volume = getCopySize();
		if (0 < *volume) {
			work = _base;
		}

		/* advance base to copy head to clear work from copyspace */
		if (_copy == _end) {
			_base = _copy = _end = _evacuator->disambiguate(getEvacuationRegion(), _end);
		} else {
			_base = _copy;
		}

		/* clear stack overflow flag */
		setStackOverflow(false);

		assertCopyspaceInvariant();
		return work;
	}

	/**
	 * Reset copyspace to empty at an unambiguous base point.
	 *
	 * @return pointer to copyspace base
	 */
	uint8_t *
	rebase()
	{
		assertCopyspaceInvariant();
		Debug_MM_true(0 == getWhiteSize());
		Debug_MM_true(0 == getCopySize());

		/* reset scanspace bounds to empty (after resetting end pointer to low edge of copyspace base region if ambiguous) and clear flags */
		_base = _copy = _end = _evacuator->disambiguate(getEvacuationRegion(), _end);
		_flags &= (uintptr_t)isTenureFlag;

		assertCopyspaceInvariant();
		return _base;
	}

	/**
	 * Reset the base to region base and set scanspace bounds to empty at an address in the region.
	 */
	uint8_t *
	reset(MM_EvacuatorBase::Region region, uint8_t *address)
	{
		Debug_MM_true(0 == getWhiteSize());
		Debug_MM_true(0 == getCopySize());

		/* reset copyspace bounds to empty at address */
		_base = _copy = _end = address;

		/* reset region flag and and fields */
		_flags = (MM_EvacuatorBase::tenure == region) ? isTenureFlag : 0;

		assertCopyspaceInvariant();
		return _base;
	}

	/**
	 * Trim remaining free space from end and return it as whitespace.
	 *
	 * @return whitespace from end of copyspace, or NULL if none available
	 */
	MM_EvacuatorWhitespace *
	trim()
	{
		assertCopyspaceInvariant();
		MM_EvacuatorWhitespace *whitespace = NULL;

		whitespace = MM_EvacuatorWhitespace::whitespace(_copy, (_end - _copy), _evacuator->compressObjectReferences(), isLOA());
		_end = _copy;

		assertCopyspaceInvariant();
		return whitespace;
	}

	void
	assertCopyspaceInvariant() const
	{
		Debug_MM_true(NULL != _base);
		Debug_MM_true(NULL != _copy);
		Debug_MM_true(NULL != _end);
		Debug_MM_true(_base <= _copy);
		Debug_MM_true(_copy <= _end);
		Debug_MM_true((uintptr_t)(_end - _base) <= ((3 *_evacuator->getExtensions()->tlhMaximumSize) << 1));
#if defined(EVACUATOR_DEBUG)
		MM_EvacuatorBase::Region region = isTenure() ? MM_EvacuatorBase::tenure: MM_EvacuatorBase::survivor;
		Debug_MM_true(region == _evacuator->getEvacuationRegion(_end, region));
		Debug_MM_true(region == _evacuator->getEvacuationRegion(_base, region));
#endif /* defined(EVACUATOR_DEBUG) */
	}

	/* evacuator is effectively const but cannot be set in array constructor */
	void evacuator(MM_EvacuatorBase *evacuator) { _evacuator = evacuator; }

	/**
	 * Constructor.
	 */
	MM_EvacuatorCopyspace()
		: MM_Base()
		, _evacuator(NULL)
		, _base(NULL)
		, _copy(NULL)
		, _end(NULL)
		, _flags(0)
	{ }
};

#endif /* EVACUATORCOPYSPACE_HPP_ */
