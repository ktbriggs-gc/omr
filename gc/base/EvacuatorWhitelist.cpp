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

/* left and right inferiors and superior of element n -- up(0) is undefined */
uintptr_t MM_EvacuatorWhitelist::left(uintptr_t n) { return (n << 1) + 1; }
uintptr_t MM_EvacuatorWhitelist::right(uintptr_t n) { return (n << 1) + 2; }
uintptr_t MM_EvacuatorWhitelist::up(uintptr_t n) { return (n - 1) >> 1; }

/* comparator for free space pointers in array */
bool MM_EvacuatorWhitelist::lt(uintptr_t a, uintptr_t b) { return _whitelist[a]->length() < _whitelist[b]->length(); }

/* swap free space pointers in array */
void
MM_EvacuatorWhitelist::swap(uintptr_t a, uintptr_t b)
{
	MM_EvacuatorWhitespace *temp = _whitelist[a];
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
MM_EvacuatorWhitelist::discard(MM_EvacuatorWhitespace *discard, bool flushing)
{
	Debug_MM_true(NULL != discard);
	uintptr_t length = discard->length();
	void *top = (void *)((uintptr_t)discard + length);

	/* recycle discards whenever possible */
	if ((MM_EvacuatorBase::tenure != _region) && (_memoryPool->getMinimumFreeEntrySize() <= length) && _memoryPool->recycleHeapChunk(discard, top)) {
		_recycled += length;
		length = 0;
	}

	/* abandon discards when not recycled and fill with holes to keep runtime heap walkable */
	if (0 < length) {

		_subspace->abandonHeapChunk(discard, top);

		/* local discard counts are aggregate for survivor + tenure regions */
		if (flushing) {
			/* intentional discard */
			_flushed += length;
		} else {
			/* incidental discard  */
			_discarded += length;
		}

		/* regional stats for all discards */
		if (MM_EvacuatorBase::tenure == _region) {
			_stats->_tenureDiscardBytes += length;
		} else {
			_stats->_flipDiscardBytes += length;
		}
	}
#if defined(EVACUATOR_DEBUG)
	verify();
	debug(discard, flushing ? "flush" : "discard");
#endif /* defined(EVACUATOR_DEBUG) */
}

#if defined(EVACUATOR_DEBUG)
void
MM_EvacuatorWhitelist::debug(MM_EvacuatorWhitespace *whitespace, const char* op)
{
	if (_evacuator->isDebugWhitelists()) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_evacuator->getEnvironment());
		omrtty_printf("%5lu    %2lu:%7s[%c]; lx@%lx; volume:%lx; discarded:%lx; flushed:%lx; [%ld]{",
				_evacuator->getEnvironment()->_scavengerStats._gcCount, _evacuator->getWorkerIndex(), op,
				((MM_EvacuatorBase::tenure == _region) ? 'T' : 'S'), ((NULL !=  whitespace) ? whitespace->length() : 0),
				(uintptr_t)whitespace, _volume, _discarded, _flushed, _count);
		uintptr_t sum = 0;
		for (uintptr_t i = 0; i < _count; i++) {
			omrtty_printf(" %lx@%lx", _whitelist[i]->length(), (uintptr_t)_whitelist[i]);
			sum += _whitelist[i]->length();
		}
		omrtty_printf(" }\n");
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
			Debug_MM_true((MM_EvacuatorBase::tenure == _region) == _evacuator->getEnvironment()->getExtensions()->isOld((omrobjectptr_t)_whitelist[i]));
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
