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

#include <stdlib.h>
#include <string.h>

#include "Evacuator.hpp"
#include "ParallelScavengeTask.hpp"

MM_EvacuatorBase::Whitespace *
MM_EvacuatorBase::Whitespace::whitespace(void *address, uintptr_t length, uintptr_t flags)
{
	Assert_MM_true(sizeof(MM_HeapLinkedFreeHeader) == (2 * sizeof(uintptr_t)));
	Assert_MM_true((0 == (((uintptr_t)address) % sizeof(uintptr_t))) && (0 == (length % sizeof(uintptr_t))));
#if defined(EVACUATOR_DEBUG)
	if ((0 < length) && (poisoned == (flags & poisoned))) {
		if (compress == (flags & compress)) {
			Debug_MM_true(0 != (*((uint32_t *)address) & J9_GC_OBJ_HEAP_HOLE_MASK));
		} else {
			Debug_MM_true(0 != (*((uintptr_t *)address) & J9_GC_OBJ_HEAP_HOLE_MASK));
		}
		Whitespace *w = (Whitespace *)address;
		uint8_t *q = (uint8_t *)w + w->length();
		for (uint8_t *p = (uint8_t *)w + OMR_MIN(w->length(), sizeof(Whitespace)); p < q; p += 1) {
			Debug_MM_true(*p == (uint8_t)J9_GC_SINGLE_SLOT_HOLE);
		}
	}
#endif /* defined(EVACUATOR_DEBUG) */
	Whitespace *whitespace = NULL;
	if (0 < length) {
		whitespace = (Whitespace *)MM_HeapLinkedFreeHeader::fillWithHoles(address, length, compress == (flags & compress));
		if ((NULL != whitespace) && (sizeof(Whitespace) <= length)) {
			/* there is room after MM_HeapLinkedFreeHeader for the _flags field ... */
			whitespace->_flags = flags & (uintptr_t)loa;
			if (sizeof(uint32_t) < sizeof(uintptr_t)) {
				/* compressed or not, ensure that both halves of the flags field are marked as heap holes */
				whitespace->_flags = (flags << 32) | flags;
			}
		} else if (NULL == whitespace) {
			/* whitespace is a single-slot hole */
			whitespace = (Whitespace *)address;
		}
		Debug_MM_true(whitespace->length() == length);
	}
	return whitespace;
}

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
const char *
MM_EvacuatorBase::conditionName(ConditionFlag condition)
{
	static const char *conditionNames[] = {"so","stf","ttf","st","bfr","rs","sr","sw","sc","io","bfa"};
	Debug_MM_true((condition_count * sizeof(const char *)) == sizeof(conditionNames));
	return (condition < condition_count) ? conditionNames[condition] : "";
}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBG_ALWAYS) */

const char *
MM_EvacuatorBase::callsite(const char *id) {
	const char *callsite = strrchr(id, '/');
	if (NULL == callsite) {
		callsite = strrchr(id, '\\');
		if (NULL == callsite) {
			callsite = id;
		}
	}
	if ((NULL != callsite) && (('/' == *callsite) || ('\\' == *callsite))) {
		callsite += 1;
	}
	return callsite;
}
