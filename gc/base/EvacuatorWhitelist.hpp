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
 * A chunk of reserved or unused whitespace. The first two uintptr_t slots are co-opted to
 * record whitespace length in bytes and LOA flag. Instances of this object are invalidated
 * of course when the contained whitespace is overwritten after acquisition. Until then the
 * property functions return valid results.
 */
class MM_EvacuatorWhitespace {
	/*
	 * Data members
	 */
private:
	enum { tenure = 1, loa = 2, tenure_loa = 3, poisoned = 4 };
	uintptr_t _lengthAndFlags;
protected:
public:

	/*
	 * Function members
	 */
private:
protected:
public:
	static MM_EvacuatorWhitespace *
	whitespace(MM_EvacuatorBase::Region region, void *address, uintptr_t length, bool compressed, bool poison = false, bool isLOA = false)
	{
		Debug_MM_true((MM_EvacuatorBase::tenure == region) || (MM_EvacuatorBase::survivor == region));
		Debug_MM_true((sizeof(MM_EvacuatorWhitespace) <= length) || (0 == length));
		Debug_MM_true(0 == (((uintptr_t)address) % sizeof(uintptr_t)));
		Debug_MM_true(0 == (length % sizeof(uintptr_t)));

		MM_EvacuatorWhitespace *space = NULL;
		if (sizeof(MM_EvacuatorWhitespace) <= length) {
			space = (MM_EvacuatorWhitespace *)address;
			if (region == MM_EvacuatorBase::tenure) {
				space->_lengthAndFlags = (length << tenure_loa) | (uintptr_t)(isLOA ? tenure_loa : tenure);
			} else {
				space->_lengthAndFlags = (length << tenure_loa) | (uintptr_t)(isLOA ? loa : 0);
			}
			if (poison) {
				space-> _lengthAndFlags |= poisoned;
			}
		}
		Debug_MM_true(space->isWhitespace((uint8_t *)space + sizeof(MM_EvacuatorWhitespace), length - sizeof(MM_EvacuatorWhitespace), poison));
		return space;
	}

	static bool
	isWhitespace(void *space, uintptr_t length, bool poisoned = false)
	{
		Debug_MM_true(0 == (((uintptr_t)space) % sizeof(uintptr_t)));
		const uint32_t hole32 = (uint32_t)J9_GC_SINGLE_SLOT_HOLE;
		const uint32_t holes32 = (hole32 << 24) | (hole32 << 16) | (hole32 << 8) | hole32;
		const uint32_t *space32 = (uint32_t *)((uintptr_t)space + sizeof(MM_EvacuatorWhitespace));
		if ((NULL == space) || ((length < 0) && (length < sizeof(MM_EvacuatorWhitespace)))) {
			return false;
		} else if ((0 == length) || (length == sizeof(MM_EvacuatorWhitespace))) {
			return true;
		} else if (!poisoned || ((*space32 == holes32) && (*(space32 + 1) == holes32))) {
			return true;
		}
		return false;
	}

	MM_EvacuatorBase::Region getEvacuationRegion() const { return isTenure() ? MM_EvacuatorBase::tenure : MM_EvacuatorBase::survivor; }

	bool isWhitespace() { return isWhitespace((uint8_t *)this, length() - sizeof(MM_EvacuatorWhitespace), isPoisoned()); }

	bool isTenure() const { return ((uintptr_t)tenure == (_lengthAndFlags & (uintptr_t)tenure)); }

	bool isLOA() const { return ((uintptr_t)loa == (_lengthAndFlags & (uintptr_t)loa)); }

	bool isPoisoned() const { return ((uintptr_t)poisoned == (_lengthAndFlags & (uintptr_t)poisoned)); }

	uintptr_t length() const { return (_lengthAndFlags >> tenure_loa); }

	uint8_t *getBase() { return (uint8_t *)this; }

	uint8_t *getEnd() { return getBase() + length(); }
};

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

	MM_EvacuatorBase * _evacuator;						/* the owning evacuator instance base class */
	const MM_EvacuatorBase::Region _region;				/* region containing whitespace */
	MM_EvacuatorWhitespace *_whitelist[max_whitelist];	/* array of pointers to free space */
	uintptr_t _count;									/* number of active free space elements in array */
	uintptr_t _volume;									/* current sum of bytes available as whitespace */
	uintptr_t _discarded;								/* cumulative sum of bytes discarded as heap holes */
	uintptr_t _flushed;									/* cumulative sum of bytes flushed as heap holes */
	uintptr_t _recycled;								/* cumulative sum of bytes recycled into memory pool */
	MM_MemorySubSpace *_subspace;						/* memory subspace receives discarded fragments */
	MM_MemoryPoolAddressOrderedList *_memoryPool;		/* HACK: this assumes AOL memory pool */
	MM_ScavengerStats *_stats;							/* pointer to _env->_scavengerStats */

protected:
public:

/*
 * Function members
 */
private:
	/* left and right inferiors and superior of element n -- up(0) is undefined */
	uintptr_t left(uintptr_t n);
	uintptr_t right(uintptr_t n);
	uintptr_t up(uintptr_t n);

	/* reorder in tree */
	void siftDown();
	void siftUp(uintptr_t bottom);

	/* comparator for free space pointers in array */
	bool lt(uintptr_t a, uintptr_t b);
	/* swap free space pointers in array */
	void swap(uintptr_t a, uintptr_t b);

	/* discard small whitespace from the bottom of the tree */
	void discard(MM_EvacuatorWhitespace *discard, bool flushing = false);

#if defined(EVACUATOR_DEBUG)
	bool le(uintptr_t a, uintptr_t b);
	void debug(MM_EvacuatorWhitespace *whitespace, const char* op);
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

	/**
	 * Get the length of largest whitespace at top of whitelist
	 */
	uintptr_t top() { return (0 < _count) ? _whitelist[0]->length() : 0; }

	/**
	 * Takes largest whitespace from top and sifts down a small one from end of list to restore largest on top
	 *
	 * @param length the minimum number of bytes of whitespace required
	 * @param swap optional pointer to whitespace with size < length to swap into whitelist
	 * @return whitespace with required capacity (length) or NULL if nothing available
	 */
	MM_EvacuatorWhitespace *
	top(uintptr_t length, MM_EvacuatorWhitespace *swap = NULL)
	{
		Debug_MM_true((NULL == swap) || (swap->length() < length));

		/* try to take whitespace from the top of the whitelist */
		MM_EvacuatorWhitespace * const whitespace = ((0 < _count) && (_whitelist[0]->length() >= length)) ? _whitelist[0] : NULL;
		if (NULL != whitespace) {

			/* discard unviable residual whitespace */
			if ((NULL != swap) && (MM_EvacuatorBase::min_reusable_whitespace >= swap->length())) {
				discard(swap);
				swap = NULL;
			}

			/* detach whitespace from whitelist */
			_volume -= whitespace->length();

			/* swap residual whitespace into top and sift it down */
			if (NULL != swap) {
				_volume += swap->length();
				_whitelist[0] = swap;
			} else {
				_count -= 1;
				_whitelist[0] = _whitelist[_count];
				_whitelist[_count] = NULL;
			}
			siftDown();

#if defined(EVACUATOR_DEBUG)
			verify();
			debug(whitespace, "-white");
#endif /* defined(EVACUATOR_DEBUG) */
		} else {

			/* add residual */
			add(swap);
		}

		return whitespace;
	}

	/**
	 * Tries to add a new free space element and sift it up the queue. It will be discarded
	 * if too small to include in current whitelist.
	 *
	 * @param (nullable) whitespace points to head of free space to add
	 */
	void
	add(MM_EvacuatorWhitespace *whitespace)
	{
		if (NULL == whitespace) {
			return;
		}

		/* presume whitespace too small to add */
		MM_EvacuatorWhitespace *displaced = whitespace;
		uintptr_t const length = (NULL != displaced) ? displaced->length() : 0;

		if (MM_EvacuatorBase::min_reusable_whitespace < length) {
			Debug_MM_true((MM_EvacuatorBase::tenure == _region) == _evacuator->getEnvironment()->getExtensions()->isOld((omrobjectptr_t)whitespace));

			/* append whitespace or displace a leaf from the bottom of the pile */
			uintptr_t leaf = _count;
			if (max_whitelist == leaf) {

				/* whitelist is full -- walk whitelist array from midpoint to end, displace smallest leaf smaller than whitespace (or not) */
				uintptr_t minLeaf = 0, minLength = ~(uintptr_t)0;
				for (leaf -= 1; (max_whitelist >> 1) < leaf; leaf -= 1) {
					if (_whitelist[leaf]->length() < minLength) {
						minLength = _whitelist[leaf]->length();
						minLeaf = leaf;
					}
				}
				leaf = (0 != minLeaf) ? minLeaf : max_whitelist;
			}

			/* if new whitespace was not too small to add, append and sift it up the heap */
			if (leaf < max_whitelist) {

				 if (leaf < _count) {
				 	displaced = _whitelist[leaf];
					_volume -= displaced->length();
				} else {
					displaced = NULL;
					_count += 1;
				}
				_whitelist[leaf] = whitespace;
				_volume += length;
				siftUp(leaf);

#if defined(EVACUATOR_DEBUG)
				debug(whitespace, "+white");
#endif /* defined(EVACUATOR_DEBUG) */
			}
		}

		/* discard any displacement */
		if (NULL != displaced) {
			discard(displaced);
		}

#if defined(EVACUATOR_DEBUG)
		verify();
#endif /* defined(EVACUATOR_DEBUG) */
	}

	/**
	 * Discards (recycles or fills with holes) all whitespace in current whitelist.
	 */
	void
	flush()
	{
		MM_EvacuatorWhitespace *whitespace = top(0);

		while (NULL != whitespace) {
			Debug_MM_true(MM_EvacuatorBase::min_reusable_whitespace < whitespace->length());
			discard(whitespace, true);
			whitespace = top(0);
		}

		Debug_MM_true((0 == _count) && (0 == _volume));
	}

	/**
	 * Returns the number of whitespace elements in the list
	 */
	uintptr_t getSize() const { return _count; }

	/**
	 * Returns the number of whitespace bytes discarded (filled with holes)
	 */
	uintptr_t getDiscarded() { uintptr_t  discarded = _discarded; _discarded = 0; return discarded; }

	/**
	 * Returns the number of whitespace bytes discarded (filled with holes)
	 */
	uintptr_t getFlushed() { uintptr_t  flushed = _flushed; _flushed = 0; return flushed; }

	/**
	 * Returns the number of whitespace bytes discarded (filled with holes)
	 */
	uintptr_t getRecycled() { uintptr_t  recycled = _recycled; _recycled = 0; return recycled; }

	/**
	 * Binds memory subspace and region to whitelist (region is effectively const but is unknow
	 */
	void
	reset(MM_MemorySubSpace *subspace)
	{
#if defined(EVACUATOR_DEBUG)
		if (MM_EvacuatorBase::tenure != _region) {
			Debug_MM_true(0 == (_discarded + _flushed + _recycled));
			for (uintptr_t i = 0; i < max_whitelist; i++) {
				Debug_MM_true(NULL == _whitelist[i]);
			}
		} else {
			/* tenure whitespace is retained between cycles, flushed before global gc and on collector shutdown */
			Debug_MM_true((0 < _count) || (0 == _flushed));
			for (uintptr_t i = 0; i < _count; i += 1) {
				Debug_MM_true(MM_EvacuatorBase::min_reusable_whitespace < _whitelist[i]->length());
			}
		}
#endif /* defined(EVACUATOR_DEBUG) */

		/* discarded/flushed/recycled counters are reset at end of cycle */
		_subspace = subspace;
		_memoryPool = (MM_MemoryPoolAddressOrderedList *)_subspace->getMemoryPool();
		_stats = &_evacuator->getEnvironment()->_scavengerStats;

		Debug_MM_true((MM_EvacuatorBase::survivor != _region) || ((0 == _count) && (0 == _volume)));
		Debug_MM_true(sizeof(MM_EvacuatorWhitespace) <= _evacuator->getEnvironment()->getObjectAlignmentInBytes());
	}

	/* evacuator is effectively const but cannot be set in array constructor */
	void evacuator(MM_EvacuatorBase *evacuator) { _evacuator = evacuator; }

	/**
	 * Constructor
	 */
	MM_EvacuatorWhitelist(MM_EvacuatorBase::Region region)
		: MM_Base()
		, _evacuator(NULL)
		, _region(region)
		, _count(0)
		, _volume(0)
		, _discarded(0)
		, _flushed(0)
		, _recycled(0)
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
