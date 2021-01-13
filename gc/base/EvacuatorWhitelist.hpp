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

#ifndef EVACUATORWHITELIST_HPP_
#define EVACUATORWHITELIST_HPP_

#if defined(EVACUATOR_DEBUG)
#include <string.h> // for memset
#endif /* defined(EVACUATOR_DEBUG) */

#include "AtomicSupport.hpp"
#include "Base.hpp"
#include "EvacuatorBase.hpp"
#include "EnvironmentStandard.hpp"
#include "GCExtensionsBase.hpp"
#include "HeapLinkedFreeHeader.hpp"
#include "MemoryPoolAddressOrderedList.hpp"
#include "MemorySubSpace.hpp"
#include "ScavengerStats.hpp"

/**
 *  Bounded priority queue of free space, pointer to largest on top at _whitelist[0]. Elements
 *  added at capacity will force a smaller element to be dropped, dropped elements are converted
 *  to holes in the runtime heap.
 */
class MM_EvacuatorWhitelist : public MM_Base
{
/*
 * Data members
 */
private:
	/* number of elements in whitelist backing array must be (2^N)-1 for some N */
	static const uintptr_t max_whitelist = 15;

	MM_EvacuatorBase *_evacuator;						/* the owning evacuator instance base class */
	const MM_EvacuatorBase::Region _region;				/* region containing whitespace */
	MM_EvacuatorBase::Whitespace *_whitelist[max_whitelist];/* array of pointers to free space */
	uintptr_t _count;									/* number of active free space elements in array */
	uintptr_t _volume;									/* current sum of bytes available as whitespace */
	MM_MemorySubSpace *_subspace;						/* memory subspace receives discarded fragments */
	MM_MemoryPoolAddressOrderedList * _memoryPool;		/* this assumes AOL memory pool */
	MM_ScavengerStats * _stats;							/* pointer to _env->_scavengerStats */

protected:
public:

/*
 * Function members
 */
private:
	/* left and right inferiors and superior of element n -- up(0) is undefined */
	MMINLINE uintptr_t left(uintptr_t n) const;
	MMINLINE uintptr_t right(uintptr_t n) const;
	MMINLINE uintptr_t up(uintptr_t n) const;

	/* reorder in tree */
	MMINLINE void siftDown();
	MMINLINE void siftUp(uintptr_t bottom);

	/* comparator for free space pointers in array */
	MMINLINE bool lt(uintptr_t a, uintptr_t b) const;
	/* swap free space pointers in array */
	MMINLINE void swap(uintptr_t a, uintptr_t b);

	/* discard small whitespace from the bottom of the tree */
	MMINLINE void discard(MM_EvacuatorBase::Whitespace *discard, bool flushing = false);

#if defined(EVACUATOR_DEBUG)
	bool le(uintptr_t a, uintptr_t b);
	void debug(MM_EvacuatorBase::Whitespace *whitespace, const char* op);
#endif /* defined(EVACUATOR_DEBUG) */

protected:
public:
	/**
	 * Basic array constructor obviates need for stdlibc++ linkage in gc component libraries. Array
	 * is allocated from forge as contiguous block sized to contain requested number of elements and
	 * must be freed using MM_Forge::free() when no longer needed. See MM_Evacuator::tearDown().
	 *
	 * @param evacuator the evacuator instance the whitelist is bound to
	 * @param forge the system memory allocator
	 * @return a pointer to instantiated array
	 */
	static MM_EvacuatorWhitelist *
	newInstanceArray(MM_Forge *forge)
	{
		MM_EvacuatorWhitelist *whitelist = (MM_EvacuatorWhitelist *)forge->allocate((2 * sizeof(MM_EvacuatorWhitelist)), OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
		if (NULL != whitelist) {
			new(whitelist + MM_EvacuatorBase::survivor) MM_EvacuatorWhitelist(MM_EvacuatorBase::survivor);
			new(whitelist + MM_EvacuatorBase::tenure) MM_EvacuatorWhitelist(MM_EvacuatorBase::tenure);
		}
		return whitelist;
	}

	/* evacuator is effectively const but cannot be set when array inititalizer is called from evacuator constructor*/
	void evacuator(MM_EvacuatorBase *evacuator) { _evacuator = evacuator; }

	/**
	 * Get the length of largest whitespace at top of whitelist
	 */
	uintptr_t top() const { return (0 < _count) ? _whitelist[0]->length() : 0; }

	/**
	 * Takes largest whitespace from top and sifts down a small one from end of list to restore largest on top
	 *
	 * @param length the minimum number of bytes of whitespace required
	 * @param swap optional pointer to whitespace with size < length to swap into whitelist
	 * @return whitespace with required capacity (length) or NULL if nothing available
	 */
	MM_EvacuatorBase::Whitespace *top(uintptr_t length, MM_EvacuatorBase::Whitespace *swap = NULL);

	/**
	 * Tries to add a new free space element and sift it up the queue. It will be discarded
	 * if too small to include in current whitelist.
	 *
	 * @param (nullable) whitespace points to head of free space to add
	 */
	void add(MM_EvacuatorBase::Whitespace *whitespace);

	/**
	 * Discards (recycles or fills with holes) all whitespace in current whitelist.
	 */
	void flush();

	/**
	 * Binds survivor/tenure memory subspace to whitelist
	 */
	void reset(MM_MemorySubSpace *subspace);

	/**
	 * Returns the number of whitespace elements in the list
	 */
	uintptr_t getSize() const { return _count; }

	/**
	 * Constructor
	 */
	MM_EvacuatorWhitelist(MM_EvacuatorBase::Region region)
		: MM_Base()
		, _evacuator(NULL)
		, _region(region)
		, _count(0)
		, _volume(0)
		, _subspace(NULL)
		, _memoryPool(NULL)
		, _stats(NULL)
	{
		for (uintptr_t i = 0; i < max_whitelist; i++) {
			_whitelist[i] = NULL;
		}
	}

#if defined(EVACUATOR_DEBUG)
	void verify();
#endif /* defined(EVACUATOR_DEBUG) */
};

#endif /* EVACUATORWHITELIST_HPP_ */
