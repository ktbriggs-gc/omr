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

#include "EvacuatorWhitelist.hpp"

MM_EvacuatorBase::Whitespace *
MM_EvacuatorWhitelist::top(uintptr_t length, MM_EvacuatorBase::Whitespace *swap)
{
	Debug_MM_true((NULL == swap) || (swap->length() < length));

	/* try to take whitespace from the top of the whitelist */
	MM_EvacuatorBase::Whitespace * const whitespace = (top() >= length) ? _whitelist[0] : NULL;
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

void
MM_EvacuatorWhitelist::add(MM_EvacuatorBase::Whitespace *whitespace)
{
	if (NULL == whitespace) {
		return;
	}

	/* presume whitespace too small to add */
	MM_EvacuatorBase::Whitespace *displaced = whitespace;
	uintptr_t const length = (NULL != displaced) ? displaced->length() : 0;

	if (MM_EvacuatorBase::min_reusable_whitespace < length) {
		Debug_MM_true(_region == _evacuator->sourceOf(whitespace));

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

void
MM_EvacuatorWhitelist::flush()
{
	MM_EvacuatorBase::Whitespace *whitespace = top(0);

	while (NULL != whitespace) {
		Debug_MM_true(MM_EvacuatorBase::min_reusable_whitespace < whitespace->length());
		discard(whitespace, true);
		whitespace = top(0);
	}

	Debug_MM_true((0 == _count) && (0 == _volume));
}

void
MM_EvacuatorWhitelist::reset(MM_MemorySubSpace *subspace)
{
#if defined(EVACUATOR_DEBUG)
	if (MM_EvacuatorBase::survivor == _region) {
		for (uintptr_t i = 0; i < max_whitelist; i++) {
			Debug_MM_true(NULL == _whitelist[i]);
		}
	} else if (MM_EvacuatorBase::tenure == _region) {
		/* tenure whitespace is retained between cycles, flushed before global gc and on collector shutdown */
		for (uintptr_t i = 0; i < _count; i += 1) {
			Debug_MM_true((NULL == _whitelist[i]) || _evacuator->isInTenure(_whitelist[i]->getBase()));
			Debug_MM_true((NULL == _whitelist[i]) || (MM_EvacuatorBase::min_reusable_whitespace < _whitelist[i]->length()));
		}
	} else {
		Debug_MM_true(false);
	}
#endif /* defined(EVACUATOR_DEBUG) */

	_subspace = subspace;
	_memoryPool = (MM_MemoryPoolAddressOrderedList *)_subspace->getMemoryPool();
	_stats = &_evacuator->getEnvironment()->_scavengerStats;

	Debug_MM_true((MM_EvacuatorBase::tenure == _region) || ((0 == _count) && (0 == _volume)));
}

/* left and right inferiors and superior of element n -- up(0) is undefined */
uintptr_t MM_EvacuatorWhitelist::left(uintptr_t n) const { return (n << 1) + 1; }
uintptr_t MM_EvacuatorWhitelist::right(uintptr_t n) const { return (n << 1) + 2; }
uintptr_t MM_EvacuatorWhitelist::up(uintptr_t n) const { return (n - 1) >> 1; }

/* comparator for free space pointers in array */
bool MM_EvacuatorWhitelist::lt(uintptr_t a, uintptr_t b) const { return _whitelist[a]->length() < _whitelist[b]->length(); }

/* swap free space pointers in array */
void
MM_EvacuatorWhitelist::swap(uintptr_t a, uintptr_t b)
{
	MM_EvacuatorBase::Whitespace *temp = _whitelist[a];
	_whitelist[a] = _whitelist[b];
	_whitelist[b] = temp;
}

void
MM_EvacuatorWhitelist::siftDown()
{
	uintptr_t pos = 0;
	uintptr_t end = _count >> 1;
	while (pos < end) {
		uintptr_t l = left(pos);
		uintptr_t r = right(pos);
		uintptr_t next = ((r < _count) && lt(l, r)) ? r : l;
		if (lt(pos, next)) {
			swap(pos, next);
			pos = next;
		} else {
			break;
		}
	}
}

void
MM_EvacuatorWhitelist::siftUp(uintptr_t bottom)
{
	uintptr_t pos = bottom;
	while (0 < pos) {
		uintptr_t next = up(pos);
		if (lt(pos, next)) {
			break;
		}
		swap(pos, next);
		pos = next;
	}
}

void
MM_EvacuatorWhitelist::discard(MM_EvacuatorBase::Whitespace *discard, bool flushing)
{
	const uintptr_t length = (NULL != discard) ? discard->length() : 0;
	if (0 < length) {
		Debug_MM_true(_region == _evacuator->sourceOf(discard));

		/* tenure memory pool non-reentrant heap lock is held by master thread during global/generational collection */
		if ((_region == MM_EvacuatorBase::survivor) && (_memoryPool->getMinimumFreeEntrySize() <= length)
		&& _memoryPool->recycleHeapChunk(discard, (void *)discard->getEnd())
		) {
			/* recycled to survivor memory pool */
			_evacuator->getMetrics()->_volumeMetrics[_region + MM_EvacuatorBase::survivor_recycled] += length;
		} else {
			/* abandon and fill with holes all discards not recycled (keep runtime heap walkable) */
			_evacuator->getMetrics()->_volumeMetrics[_region + MM_EvacuatorBase::survivor_discarded] += length;
			_subspace->abandonHeapChunk(discard, (void *)discard->getEnd());
		}
#if defined(EVACUATOR_DEBUG)
		debug(discard, flushing ? "flush" : "discard");
		verify();
#endif /* defined(EVACUATOR_DEBUG) */
	}
}

#if defined(EVACUATOR_DEBUG)
void
MM_EvacuatorWhitelist::debug(MM_EvacuatorBase::Whitespace *whitespace, const char* op)
{
	if (_evacuator->isDebugWhitelists()) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_evacuator->getEnvironment());
		omrtty_printf("%5lu    %2lu:%7s[%c]; base:%llx; length:%llx; volume:%llx; [%lld]{",
				_evacuator->getEnvironment()->_scavengerStats._gcCount, _evacuator->getWorkerIndex(), op,
				((MM_EvacuatorBase::tenure == _region) ? 'T' : 'S'), (uintptr_t)whitespace,
				((NULL !=  whitespace) ? whitespace->length() : 0), _volume, _count);
		uintptr_t sum = 0;
		for (uintptr_t i = 0; i < _count; i++) {
			omrtty_printf(" %llx@%llx", _whitelist[i]->length(), (uintptr_t)_whitelist[i]);
			sum += _whitelist[i]->length();
		}
		omrtty_printf("}\n");
		Debug_MM_true(sum == _volume);
	}
}

bool MM_EvacuatorWhitelist::le(uintptr_t a, uintptr_t b) { return _whitelist[a]->length() <= _whitelist[b]->length(); }

void
MM_EvacuatorWhitelist::verify()
{
	Debug_MM_true(max_whitelist >= _count);
	Debug_MM_true((0 == _count) == (0 == _volume));

	if (_evacuator->isDebugWhitelists()) {
		uintptr_t volume = 0;
		Debug_MM_true(((0 == _count) && (NULL == _whitelist[0])) || (DEFAULT_SCAN_CACHE_MAXIMUM_SIZE >= _whitelist[0]->length()));
		for (uintptr_t i = 0; i < _count; i += 1) {
			Debug_MM_true(NULL != _whitelist[i]);
			Debug_MM_true(0  == ((sizeof(_whitelist[i]->length()) - 1) & _whitelist[i]->length()));
			Debug_MM_true(MM_EvacuatorBase::min_reusable_whitespace < _whitelist[i]->length());
			Debug_MM_true(_region == _evacuator->sourceOf(_whitelist[i]));
			Debug_MM_true((0 == i) || le(i, up(i)));
			volume += _whitelist[i]->length();
		}
		uintptr_t end = _count >> 1;
		for (uintptr_t j = 0; j < end; j += 1) {
			Debug_MM_true(le(left(j), j));
			Debug_MM_true((right(j) >=_count) || le(right(j), j));
		}
		Debug_MM_true(volume == _volume);
	}
}
#endif /* defined(EVACUATOR_DEBUG) */
