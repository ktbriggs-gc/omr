/*******************************************************************************
 * Copyright (c) 2015, 2020 IBM Corp. and others
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

#if !defined(INDEXABLEOBJECTSCANNER_HPP_)
#define INDEXABLEOBJECTSCANNER_HPP_

#include "ObjectScanner.hpp"

class GC_IndexableObjectScanner : public GC_ObjectScanner
{
	/* Data Members */
private:

protected:
	omrobjectptr_t _arrayPtr; /**< pointer to array */
	fomrobject_t *_basePtr; /**< pointer to base array element */
	fomrobject_t *_limitPtr; /**< pointer to end of last array element */
	fomrobject_t *_endPtr; /**< pointer to end of last array element in scan segment */

public:

	/* Member Functions */
private:

protected:

	MMINLINE virtual fomrobject_t *
#if defined(OMR_GC_LEAF_BITS)
	getNextSlotMap(uintptr_t *scanMap, uintptr_t *leafMap, bool *hasNextSlotMap)
#else
	getNextSlotMap(uintptr_t *scanMap, bool *hasNextSlotMap)
#endif /* OMR_GC_LEAF_BITS */
	{
		Assert_MM_unreachable();
		return NULL;
	}

public:
	/**
	 * @param env The scanning thread environment
	 * @param[in] arrayPtr pointer to the array to be processed
	 * @param[in] basePtr pointer to the first contiguous array cell
	 * @param[in] limitPtr pointer to end of last contiguous array cell
	 * @param[in] scanPtr pointer to the array cell where scanning will start
	 * @param[in] endPtr pointer to the array cell where scanning will stop
	 * @param[in] scanMap first portion of bitmap for slots to scan
	 * @param[in] elementSize array element size must be aligned to the size of an object to object reference
	 * @param[in] flags scanning context flags
	 */
	GC_IndexableObjectScanner(
		MM_EnvironmentBase *env
		, omrobjectptr_t arrayPtr
		, fomrobject_t *basePtr
		, fomrobject_t *limitPtr
		, fomrobject_t *scanPtr
		, fomrobject_t *endPtr
		, uintptr_t scanMap
		, uintptr_t flags
	)
#if defined(OMR_GC_LEAF_BITS)
		: GC_ObjectScanner(env, scanPtr, 0, scanMap, flags | GC_ObjectScanner::indexableObject)
#else
		: GC_ObjectScanner(env, scanPtr, scanMap, flags | GC_ObjectScanner::indexableObject)
#endif /* defined(OMR_GC_LEAF_BITS) */
		, _arrayPtr(arrayPtr)
		, _basePtr(basePtr)
		, _limitPtr(limitPtr)
		, _endPtr(endPtr)
	{
		_typeId = __FUNCTION__;
	}

	/**
	 * Set up the scanner.
	 * @param[in] env current environment (per thread)
	 */
	MMINLINE void
	initialize(MM_EnvironmentBase *env)
	{
		Debug_OS_true(_basePtr <= getScanPtr());
		Debug_OS_true(getScanPtr() <= _endPtr);
		Debug_OS_true(_endPtr <= _limitPtr);
		GC_ObjectScanner::initialize(env);
	}

	/**
	 * Get the address of the next element in the array.
	 * This is used in flattened contexts only -- should stride be dependent on slot compression?
	 * @see GC_FlattenedArrayObjectScanner::getNextSlotMap()
	 * @return NULL if there are no more elements
	 */
	MMINLINE fomrobject_t *
	nextIndexableElement(uintptr_t stride)
	{
		fomrobject_t *result = GC_SlotObject::addToSlotAddress(getScanPtr(), (intptr_t)stride, compressObjectReferences());
		if (result >= _endPtr) {
			result =  NULL;
		}
		return result;
	}

	/**
	 * Get the maximal slot index for the array. Array indices are assumed to be zero-based.
	 */
	MMINLINE uintptr_t getIndexableRange() { return ((uintptr_t)_limitPtr - (uintptr_t)_basePtr) / slotSizeInBytes(); }

	/**
	 * Reset truncated end pointer to force scanning to limit pointer (scan to end of indexable object). This
	 * must be called if this scanner cannot be split to hive off the tail
	 */
	MMINLINE void scanToLimit() { _endPtr = _limitPtr; }

	/**
	* Return pointer to array object
	*/
	MMINLINE omrobjectptr_t const getArrayObject() { return _arrayPtr; }

	/**
	 * Split this instance and set split scan/end pointers to indicate split scan range.
	 *
	 * @param env The scanning thread environment
	 * @param allocSpace Pointer to memory where split scanner will be instantiated (in-place)
	 * @param splitAmount The maximum number of array elements to include
	 * @return Pointer to split scanner in allocSpace
	 */
	virtual GC_IndexableObjectScanner *splitTo(MM_EnvironmentBase *env, void *allocSpace, uintptr_t splitAmount)
	{
		Assert_MM_unreachable();
		return NULL;
	}
};

#endif /* INDEXABLEOBJECTSCANNER_HPP_ */
