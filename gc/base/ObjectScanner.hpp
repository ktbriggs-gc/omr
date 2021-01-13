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

#if !defined(OBJECTSCANNER_HPP_)
#define OBJECTSCANNER_HPP_

#include "omrcfg.h"
#include "ModronAssertions.h"
#include "objectdescription.h"

#include "BaseVirtual.hpp"
#include "EnvironmentBase.hpp"
#include "GCExtensionsBase.hpp"
#include "SlotObject.hpp"

/**
 * FOR DEBUG BUILDS ONLY:
 * Define DEBUG_OBJECT_SCANNERS to enable fine-grained validation of scanner state.
 *
 * #define DEBUG_OBJECT_SCANNERS
 *
 * Do not commit this file with DEBUG_OBJECT_SCANNERS defined.
 */
#undef DEBUG_OBJECT_SCANNERS
#if defined(DEBUG_OBJECT_SCANNERS)
#define Debug_OS_true(assertion) Assert_MM_true(assertion)
#else
#define Debug_OS_true(assertion)
#endif /*  */

/**
 * This object scanning model allows an inline getNextSlot() implementation and works best
 * with object representations that contain all object reference slots within one or more
 * contiguous blocks of fomrobject_t-aligned memory. This allows reference slots to be
 * identified by a series of (base-pointer, bit-map) pairs. Subclasses present the initial
 * pair when they instantiate and subsequent pairs, if any, in subsequent calls to
 * getNextSlotMap().
 *
 * Subclasses can set the noMoreSlots bit in _flags, to signal that any future calls to
 * getNextSlotMap() will return NULL, when they instantiate or in their implementation of
 * getNextSlotMap(). This assumes that subclasses can look ahead in their object traverse
 * and allows scanning for most objects (<32/64 slots) to complete inline, without
 * requiring a virtual method call (getNextSlotMap()). Otherwise at least one such call
 * is required before getNextSlot() will return NULL.
 *
 * The object scanner leaf optimization option is enabled by the OMR_GC_LEAF_BITS
 * flag in omrcfg.h.
 *
 * TODO: Get OMR_GC_LEAF_BITS into omrcfg.h.
 *
 * Scanners with leaf optimization are used in marking contexts when parent
 * object class includes information identifying which child slots refer to
 * objects belonging to classes that contain no reference slots. The OMR
 * marking scheme will use this information to optimize the marking work
 * stack.
 *
 * If leaf information is available it should be expressed in the implementation
 * of getNextSlotMap(uintptr_t *, uintptr_t *, bool *). This method is called to
 * obtain a bit map of the contained leaf slots conforming to the reference slot
 * map. An initial leaf map is provided to the GC_ObjectScanner constructor and
 * it is refreshed when required.
 */
class GC_ObjectScanner : public MM_BaseVirtual
{
	/* Data Members */
private:

protected:
	static const intptr_t _bitsPerScanMap = sizeof(uintptr_t) << 3;

	GC_SlotObject _slotObject;				/**< wrapper for most recently scanned slot */
	uintptr_t _scanMap;						/**< bit map of reference slots in object being scanned (32/64-bit window) */
#if defined(OMR_GC_LEAF_BITS)
	uintptr_t _leafMap;						/**< bit map of reference slots in object that reference leaf objects */
#endif /* defined(OMR_GC_LEAF_BITS) */
	fomrobject_t *_mapPtr;					/**< address corresponding to LSB of _scanMap */
	uint32_t _objectSize;					/**< instance size in bytes or 0 if not known or >2^32 */
	uint32_t _flags;						/**< instance flags (scanRoots, scanHeap, ...) + client flags*/
	
public:
	/**
	 *  Instantiation flags used to specialize scanner for specific scavenger operations
	 */
	enum InstanceFlags
	{
		scanRoots = 1					/* scavenge roots phase -- scan & copy/forward root objects */
		, scanHeap = 2					/* scavenge heap phase -- scan  & copy/forward objects reachable from scanned roots */
		, indexableObject = 4			/* this is set for array object scanners where the array elements can be partitioned for multithreaded scanning */
		, indexableObjectNoSplit = 8	/* this is set for array object scanners where the array elements cannot be partitioned for multithreaded scanning */
		, headObjectScanner = 16		/* this is set for array object scanners containing the elements from the first split segment, and for all non-indexable objects */
		, noMoreSlots = 32				/* this is set when object has more no slots to scan past current bitmap */
		, nullObject = 64				/* null object scanner */
		, compressObjectSlots = 128		/* fomrobject_t is compressed */
	};

	enum ClientFlags
	{
		/* subclasses can define private flags in this closed range */
		clientFlagBase = (intptr_t)1 << 16
		, clientFlagMax = (intptr_t)1 << 31
	};

	/* Member Functions */
private:

protected:
	/**
	 * Constructor. With leaf optimization.
	 *
	 * @param[in] env The environment for the scanning thread
	 * @param[in] scanPtr The first slot contained in the object to be scanned
	 * @param[in] leafMap Map of slots in slotMap that reference leaf objects (ignored if #if !defined(OMR_GC_LEAF_BITS))
	 * @param[in] scanMap Bit map marking object reference slots, with least significant bit mapped to slot at scanPtr
	 * @param[in] flags A bit mask comprised of InstanceFlags
	 */
	GC_ObjectScanner(MM_EnvironmentBase *env, fomrobject_t *scanPtr, uintptr_t leafMap, uintptr_t scanMap, uintptr_t flags)
		: MM_BaseVirtual()
		, _slotObject(env->getOmrVM(), (fomrobject_t *)(1 | (uintptr_t)scanPtr))
		, _scanMap(scanMap)
#if defined(OMR_GC_LEAF_BITS)
		, _leafMap(leafMap)
#endif /* defined(OMR_GC_LEAF_BITS) */
		, _mapPtr(scanPtr)
		, _objectSize(0)
		, _flags(flags | headObjectScanner | (uint32_t)(env->getExtensions()->compressObjectReferences() ? compressObjectSlots : 0))
	{
#if defined(OMR_GC_LEAF_BITS)
		Debug_OS_true(0 == (_leafMap & ~_scanMap));
#endif /* defined(OMR_GC_LEAF_BITS) */
		_typeId = __FUNCTION__;
	}

	/**
	 * Constructor. Without leaf optimization.
	 *
	 * @param[in] env The environment for the scanning thread
	 * @param[in] scanPtr The first slot contained in the object to be scanned
	 * @param[in] scanMap Bit map marking object reference slots, with least significant bit mapped to slot at scanPtr
	 * @param[in] flags A bit mask comprised of InstanceFlags
	 */
	GC_ObjectScanner(MM_EnvironmentBase *env, fomrobject_t *scanPtr, uintptr_t scanMap, uintptr_t flags)
		: GC_ObjectScanner(env, scanPtr, 0, scanMap, flags )
	{ }

	/**
	 * Set up the scanner. Subclasses should provide a non-virtual implementation
	 * to build next slot map and call it from their constructor or just after
	 * their constructor. This will obviate the need to make an initial call to
	 * the virtual getNextSlotMap() function and, in most cases, avoid the need
	 * to call getNextSlotMap() entirely.
	 *
	 * If all of the object reference fields are mapped in the initial slot map,
	 * the sublcass implementation should call setNoMoreSlots() to indicate that
	 * the getNextSlotMap() method should not be called to refresh the slot map.
	 *
	 * This non-virtual base class implementation should be called directly at the
	 * end of the sublcass implementation, eg
	 *
	 *    MM_ObjectScanner::initialize(env);
	 *
	 * @param[in] env Current environment
	 * @see getNextSlotMap()
	 * @see putNextSlotMapBit()
	 */
	void
	initialize(MM_EnvironmentBase *env)
	{
		Debug_OS_true(_mapPtr == getScanPtr());
		Debug_OS_true(1 == (1 & (uintptr_t)_slotObject.readAddressFromSlot()));
		Debug_OS_true((NULL != _mapPtr) || ((0 == _scanMap) && !hasMoreSlots()));
#if defined(OMR_GC_LEAF_BITS)
		Debug_OS_true(_leafMap == (_leafMap & _scanMap));
#endif /* defined(OMR_GC_LEAF_BITS) */
	}

	/**
	 * Return base pointer and scan bit map for next block of contiguous slots to be scanned. The
	 * base pointer must be fomrobject_t-aligned. Bits in the bit map are scanned in order of
	 * increasing significance, and the least significant bit maps to the slot at the returned
	 * base pointer.
	 *
	 * When this method is called the scan
	 *
	 * *** NOTE: successive map base address ranges are not presumed to be contiguous. Map base
	 * can point to any fomrobject_t aligned address within the heap region being scanned.
	 *
	 * *** NOTE: if the returned map base pointer is *not* NULL *and* slot scanMap is empty (0)
	 * the next call to getNextSlot() will return NULL *and* setNoMoreSlots(). This will preempt
	 * further scanning of object slots so consider it unwise to return an empty slot map with a
	 * non-NULL map base unless the object is known to contain no more unscanned slots.
	 *
	 * @param[out] scanMap the bit map for the slots contiguous with the returned base pointer
	 * @param[out] hasNextSlotMap set this to true if this method should be called again, false if this map is known to be last
	 * @return a pointer to the first slot mapped by the least significant bit of the map, or NULL if no more slots
	 */
#if defined(OMR_GC_LEAF_BITS)
	virtual fomrobject_t *getNextSlotMap(uintptr_t *scanMap, uintptr_t *leafMap, bool *hasNextSlotMap) = 0;
#else
	virtual fomrobject_t *getNextSlotMap(uintptr_t *scanMap, bool *hasNextSlotMap) = 0;
#endif /* defined(OMR_GC_LEAF_BITS) */

	/**
	 * Helper function can be used to initialize the map base pointer and slot map of
	 * reference fields for a new object to be scanned or to rebase and build slot map
	 * for a large (>32/64 slots) or discontiguous object. Call this method once for
	 * each object slot holding a reference pointer (best to present reference fields
	 * in increasing address order). Iterate until this method returns false, at which
	 * point the map pointer will be rebased and slot map for the new map base will be
	 * complete.
 	 *
 	 * Call this method with nextSlotAddress == NULL in getNextSlotMap() if there are no
 	 * more slots to return. This is not required if noMoreSlots flag was set in the
 	 * object scanner subclass constructor or in initialize(), or the most recent call
 	 * to getNextSlotMap() returned hasNoMoreSlots == true.
	 *
	 * This method may be used in subclass implementations of initialize()/getNextSlotMap().
	 * If the method returns false, the field presented in the call will not be included in
	 * the slot map and must be presented first in the next call to getNextSlotMap().
	 *
	 * Alternatively, subclasses can fully initialize this base class by passing map base
	 * pointer and initial slot map in the constructor. This obviates calls to getNextSlotMap()
	 * for objects with reference slots confined to a <32/64 slot range of contiguous slots
	 * as long as noMoreSlots flag is also set in constructor.
	 */
	bool
#if defined(OMR_GC_LEAF_BITS)
	putNextSlotMapBit(fomrobject_t *nextSlotAddress, bool isLeafSlot, bool hasMoreSlots)
#else
	putNextSlotMapBit(fomrobject_t *nextSlotAddress, bool hasMoreSlots)
#endif /* defined(OMR_GC_LEAF_BITS) */
	{
		if (NULL != nextSlotAddress) {
			if (0 != _scanMap) {
				intptr_t bitOffset = GC_SlotObject::subtractSlotAddresses(nextSlotAddress, _mapPtr, compressObjectReferences());
				if (_bitsPerScanMap < bitOffset) {
					_scanMap |= 1 << bitOffset;
#if defined(OMR_GC_LEAF_BITS)
					if (isLeafSlot) {
						_leafMap |= 1 << bitOffset;
					}
#endif /* defined(OMR_GC_LEAF_BITS) */
					return true;
				} else {
					setFlags(noMoreSlots, !hasMoreSlots);
				}
			} else {
				setMapPtr(nextSlotAddress, hasMoreSlots);
				_scanMap = 1;
				return true;
			}
		} else {
			setMapPtr(NULL, false);
		}
		return false;
	}

	/**
	 * Set a new map pointer and sync scan pointer to base of map with LSB set to indicate that
	 * the slot at the base of the map has not previously been scanned in getNextSlot().
	 */
	void
	setMapPtr(fomrobject_t *mapPtr, bool hasMoreSlots)
	{
		_mapPtr = mapPtr;
		if (NULL != _mapPtr) {
			_slotObject.writeAddressToSlot((fomrobject_t* )(1 | (uintptr_t)_mapPtr));
			setFlags(noMoreSlots, !hasMoreSlots);
		} else {
			/* clear the map and slot object at end of object scan */
			_slotObject.writeAddressToSlot(NULL);
			setFlags(noMoreSlots, true);
			_scanMap = 0;
#if defined(OMR_GC_LEAF_BITS)
			_leafMap = 0;
#endif /* defined(OMR_GC_LEAF_BITS) */
		}
		Debug_OS_true(_mapPtr <= getScanPtr());
		Debug_OS_true(_bitsPerScanMap > GC_SlotObject::subtractSlotAddresses(getScanPtr(), _mapPtr, compressObjectReferences()));
		Debug_OS_true((NULL != _mapPtr) || ((0 == _scanMap) && isFlagSet(noMoreSlots)));
	}

	/**
	 * Get a pointer to the unscanned slot after the current scan head. This returns
	 * position only and does not ensure the validity of the contents of the slot.
	 *
	 * NOTE: returned value may point to a slot not contained in the object if the
	 * scan pointer has past the end of the current slot map but getNextSlotMap()
	 * has not yet been called. The validity of the slot contents can be checked
	 * by ensuring that the returned value is < (_mapPtr + _bitsPerScanMap).
	 *
	 * @return NULL if no more slots (object scan complete)
	 */
	fomrobject_t *
	getScanPtr()
	{
		fomrobject_t *scanPtr = _slotObject.readAddressFromSlot();
		if (NULL != scanPtr) {
			bool slotHasBeenScanned = (0 ==  (1 & (uintptr_t)scanPtr));
			if (slotHasBeenScanned) {
				scanPtr = GC_SlotObject::addToSlotAddress(scanPtr, 1, compressObjectReferences());
			} else {
				scanPtr = (fomrobject_t *)(~(uintptr_t)1 & (uintptr_t)scanPtr);
			}
		}
		return scanPtr;
	}

public:
	/**
	 * Get the address of the slot returned from the most recent call to getNextSlot(), or the
	 * first (unexamined) slot in the object. This is intended to provide positional information
	 * only, caller should not modify slot contents.
	 */
	const fomrobject_t *
	getScanSlotAddress() const
	{
		return (const fomrobject_t *)(~(uintptr_t)1 & (uintptr_t)_slotObject.readAddressFromSlot());
	}

	/**
	 * Get the next object slot if one is available in the range of the current _slotMap. Otherwise,
	 * if client has indicated that there are more slots in the object, call getNextSlotMap() to
	 * obtain the next _slotMap and_mapPtr (which may not be contiguous with completed range of slots).
	 * This continues until client indicates no more slots by signaling noMoreSlots when returning the
	 * last _mapPtr/_slotMap from getNextSlotMap().
	 *
	 * @see getNextSlotMap()
	 * @return a pointer to a slot object encapsulating the next object slot, or NULL if no next object slot
	 */
	const GC_SlotObject *
	getNextSlot()
	{
		Debug_OS_true(_mapPtr <= getScanPtr());
		Debug_OS_true((0 == _scanMap) || (_bitsPerScanMap >= GC_SlotObject::subtractSlotAddresses(getScanPtr(), _mapPtr, compressObjectReferences())));
		while (0 != _scanMap) {
			/* skip over most recently returned slot or start with fresh map base and slot bit map */
			if (0 == (1 & (uintptr_t)_slotObject.readAddressFromSlot())) {
				/* shift slot bit out of scan map and advance slot address */
				_scanMap >>= 1;
#if defined(OMR_GC_LEAF_BITS)
				_leafMap >>= 1;
#endif /* defined(OMR_GC_LEAF_BITS) */
				_slotObject.addToSlotAddress(1);
			} else {
				/*  slot bit and slot address are aligned with map base but slot not yet scanned so just remove the tag */
				_slotObject.writeAddressToSlot((fomrobject_t*)getScanPtr());
			}

			/* while there is at least one bit-mapped slot, advance scan pointer to a non-NULL slot or end of map */
			while ((0 != _scanMap) && ((0 == (1 & _scanMap)) || (NULL == _slotObject.readReferenceFromSlot()))) {
				_scanMap >>= 1;
#if defined(OMR_GC_LEAF_BITS)
				_leafMap >>= 1;
#endif /* defined(OMR_GC_LEAF_BITS) */
				_slotObject.addToSlotAddress(1);
			}

			/* if the scan bit for the slot is set return the slot object, otherwise look for a next slot map */
			if (0 != _scanMap) {
#if defined(DEBUG_OBJBECT_SCANNER)
				uintptr_t slot = (uintptr_t)(*(uint32_t*)(uintptr_t)(*(uint32_t*)((uintptr_t)_slotObject.readAddressFromSlot() & ~(uintptr_t)0x1)));
				Debug_OS_true((0x4 & slot) || (0x99669966 == *(uint32_t*)(~(uintptr_t)0xff & slot)));
#endif /* defined(DEBUG_OBJBECT_SCANNER) */
				return &_slotObject;
			} else if (hasMoreSlots()) {
				bool hasNextSlotMap = false;
				/* advance map and scan slot past trailing 0s in current scan bit map and tag slot address to indicate not scanned */
				_mapPtr = GC_SlotObject::addToSlotAddress(_mapPtr, _bitsPerScanMap, compressObjectReferences());
				_slotObject.writeAddressToSlot((fomrobject_t* )(1 | (uintptr_t)_mapPtr));
				/* rebase map pointer and refresh the scan bit map */
#if defined(OMR_GC_LEAF_BITS)
				fomrobject_t *mapPtr = getNextSlotMap(&_scanMap, &_leafMap, &hasNextSlotMap);
				Debug_OS_true(_leafMap == (_leafMap & _scanMap));
#else
				fomrobject_t *mapPtr = getNextSlotMap(&_scanMap, &hasNextSlotMap);
#endif /* defined(OMR_GC_LEAF_BITS) */
				setMapPtr(mapPtr, hasNextSlotMap);
			} else {
				break;
			}
		}

		_slotObject.writeAddressToSlot(NULL);
		setMapPtr(NULL, false);
		return NULL;
	}

#if defined(OMR_GC_LEAF_BITS)
	const GC_SlotObject *
	getNextSlot(bool* isLeafSlot)
	{
		const GC_SlotObject *nextSlot = getNextSlot();
		*isLeafSlot = (NULL == nextSlot) || (0 != (1 & _leafMap));
		return nextSlot;
	}
#endif /* defined(OMR_GC_LEAF_BITS) */

	/**
	 * Convenience method tests for object slot compression 64->32 bits.
	 */
	bool compressObjectReferences() const { return (0 != ((uintptr_t)compressObjectSlots & _flags)); }

	/**
	 * Convenience method gets fomrobject_t size in bytes.
	 */
	uintptr_t
	slotSizeInBytes() const
	{
		fomrobject_t *oneSlot = GC_SlotObject::addToSlotAddress((fomrobject_t *)0, 1, compressObjectReferences());
		return (uintptr_t)oneSlot;
	}

	/**
	 * Convenience method sets/gets instance size in bytes, or 0 if not known. Setting this
	 * >0 when the scanner is initialized obviates querying the object model for size at the
	 * end of object scan and will override the object size default (0) preset in this base
	 * class constructor.
	 */
	void objectSizeInBytes(uint32_t totalInstanceSize) { _objectSize = totalInstanceSize; }
	uint32_t objectSizeInBytes() const { return _objectSize; }

	/**
	 * Leaf objects contain no reference slots (eg plain value object or empty array).
	 *
	 * @return true if the object to be scanned is a leaf object
	 */
	bool isLeafObject() const { return (0 == _scanMap) && !hasMoreSlots(); }
	
	/**
	 * Null object scanners (GC_NullObjectScanner) are placeholders to allow RAM reserved for
	 * object sacanner instantiation to be safely initialized.
	 */
	static bool isNullObject(uintptr_t flags) { return (0 != (nullObject & flags)); }
	bool isNullObject() const { return isFlagSet(nullObject); }

	/**
	 * Informational, relating to scanning and client context (_flags)
	 */
	void setFlags(uint32_t flags, bool value) { if (value) { _flags |= flags; } else { _flags &= ~flags; } }
	void clearFlags(uint32_t flags) { _flags &= ~flags; }
	bool isFlagSet(uint32_t flag) const { return (0 != (_flags & flag)); }

	void setNoMoreSlots() { setFlags(noMoreSlots, true); }
	void setMoreSlots() { setFlags(noMoreSlots, false); }
	bool hasMoreSlots() const { return !isFlagSet(noMoreSlots); }

	static bool isRootScan(uintptr_t flags) { return (0 != (scanRoots & flags)); }
	bool isRootScan() const { return isFlagSet(scanRoots); }

	static bool isHeapScan(uintptr_t flags){ return (0 != (scanHeap & flags)); }
	bool isHeapScan() const { return isFlagSet(scanHeap); }

	static bool isIndexableObject(uintptr_t flags) { return (0 != (indexableObject & flags)); }
	bool const isIndexableObject() const { return isFlagSet(indexableObject); }
	static bool isIndexableObjectNoSplit(uintptr_t flags) { return (0 != (indexableObjectNoSplit & flags)); }
	bool const isIndexableObjectNoSplit() const { return isFlagSet(indexableObjectNoSplit); }

	bool const isHeadObjectScanner() const { return isFlagSet(headObjectScanner); }
	void clearHeadObjectScanner() { clearFlags(headObjectScanner); }
};

/**
 * This subclass is a simple wrapper for an object that is known to contain no heap references. It is useful
 * for initializing static RAM used to instantiate successive scanners in place when object type is not
 * known. The resulting scanner instance will always return NULL from getNextSlot().
 *
 * Use GC_ObjectScanner::isLeafObject() to test for leaf object without calling getNextSlot().
 */
class GC_NullObjectScanner : public GC_ObjectScanner
{
	/* Data Members */
private:
protected:
public:

	/* Function Members */
private:
protected:
public:
	GC_NullObjectScanner(MM_EnvironmentBase *env)
#if defined(OMR_GC_LEAF_BITS)
	: GC_ObjectScanner(env, NULL, 0, 0, noMoreSlots)
#else
	: GC_ObjectScanner(env, NULL, 0, noMoreSlots)
#endif /* defined(OMR_GC_LEAF_BITS) */
	{
		_typeId = __FUNCTION__;
	}

	static GC_NullObjectScanner *
	newInstance(MM_EnvironmentBase *env, void *allocSpace)
	{
		GC_NullObjectScanner *objectScanner = (GC_NullObjectScanner *)allocSpace;
		if (NULL != objectScanner) {
			new(objectScanner) GC_NullObjectScanner(env);
			objectScanner->initialize(env);
		}
		return objectScanner;
	}

	virtual fomrobject_t *
#if defined(OMR_GC_LEAF_BITS)
	getNextSlotMap(uintptr_t *scanMap, uintptr_t *leafMap, bool *hasNextSlotMap)
#else
	getNextSlotMap(uintptr_t *scanMap, bool *hasNextSlotMap)
#endif /* defined(OMR_GC_LEAF_BITS) */
	{
		Assert_MM_unreachable();
		return NULL;
	}
};
#endif /* OBJECTSCANNER_HPP_ */
