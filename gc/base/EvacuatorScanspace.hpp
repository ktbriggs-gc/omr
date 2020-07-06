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

#ifndef EVACUATORSCANSPACE_HPP_
#define EVACUATORSCANSPACE_HPP_

#include "Base.hpp"
#include "EvacuatorCopyspace.hpp"
#include "EvacuatorWhitelist.hpp"
#include "EvacuatorWorklist.hpp"
#include "IndexableObjectScanner.hpp"
#include "ObjectScanner.hpp"
#include "ObjectScannerState.hpp"

/**
 * Extends copyspace to allow scanning.
 */
class MM_EvacuatorScanspace : public MM_EvacuatorCopyspace
{
/*
 * Data members
 */
private:
	GC_ObjectScannerState _objectScannerState;	/* space reserved for instantiation of object scanner for current object */
	GC_ObjectScanner *_objectScanner;			/* points to _objectScannerState after scanner is instantiated, NULL if object scanner not instantiated */
	uint8_t *_scan;								/* scan head points to object being scanned */
	uintptr_t _startSlot;						/* (split array) 0-based array index of first slot to scan */
	uintptr_t _stopSlot;						/* (split array) 0-based array index of slot after the last slot to scan */
	uintptr_t _activations;						/* number of times this scanspace has been activated (pushed into) */

protected:
	/* enumeration of flag bits may be extended past scanEndFlag by subclasses */
	typedef enum scanspaceFlags {
		isRememberedFlag = copyEndFlag << 1		/* remembered state of object at scan head */
		, isSplitArrayFlag = copyEndFlag << 2	/* indicates that scanspace contains a slit array segment when set */
		, scanEndFlag = isSplitArrayFlag		/* marks end of scanspace flags */
	} scanspaceFlags;

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
	 * @param count the number of array elements to instantiate
	 * @return a pointer to instantiated array
	 */
	static MM_EvacuatorScanspace *
	newInstanceArray(MM_Forge *forge, uintptr_t count)
	{
		MM_EvacuatorScanspace *scanspace = (MM_EvacuatorScanspace *)forge->allocate(sizeof(MM_EvacuatorScanspace) * count, OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
		if (NULL != scanspace) {
			for (intptr_t index = count - 1; 0 <= index; index -= 1) {
				new(&scanspace[index]) MM_EvacuatorScanspace();
			}
		}
		return scanspace;
	}

	/**
	 * Return a pointer to this scanscape as copyspace
	 */
	MM_EvacuatorCopyspace *asCopyspace() { return static_cast<MM_EvacuatorCopyspace *>(this); }

	/*
	 * Current or latent object scanner is instantiated in space reserved within containing scanspace. This
	 * method should only be called when caller is committed to use the returned pointer to instantiate an
	 * object scanner. To get current pointer to active object scanner, which may be NULL, use
	 * getActiveObjectScanner().
	 */
	GC_ObjectScannerState *activateObjectScanner() { _objectScanner = (GC_ObjectScanner *)&_objectScannerState; return &_objectScannerState; }

	/**
	 * Clear the remembered state (before starting to scan a new object)
	 */
	void clearRememberedState() { _flags &= ~(uintptr_t)isRememberedFlag; }

	/**
	 * Get the remembered state of the most recently scanned object
	 */
	bool getRememberedState() const { return isRememberedFlag == (_flags & (uintptr_t)isRememberedFlag); }

	/**
	 * Set the remembered state of the current object if it is tenured and has a referent in new space
	 *
	 * @param referentInSurvivor set this to true if the referent is in new space
	 */
	void
	updateRememberedState(bool referentInSurvivor)
	{
		if (referentInSurvivor) {
			_flags |= (uintptr_t)isRememberedFlag;
		}
	}

	/**
	 * Reset to an empty state after popping or rebasing to release unscanned work. Scanspace
	 * must be empty of work.
	 */
	void
	reset()
	{
		/* adjust base to scan head to clear work scanned since frame was activated */
		_base = _scan;
		assertScanspaceInvariant();

		/* advance base to scan head */
		if (0 == getWhiteSize()) {
			/* reset scanspace bounds to empty */
			_scan = rebase();
		} else {
			_base = _scan;
		}

		/* reset array-specific flag and fields and clear active object scanner */
		_flags &= ~(uintptr_t)isSplitArrayFlag;
		_startSlot = _stopSlot = 0;
		_objectScanner = NULL;

		assertScanspaceInvariant();
	}

	void
	setScanspace(MM_EvacuatorBase::Region region, uint8_t *base, uint8_t *copy, uintptr_t length, bool isLOA = false)
	{
		Debug_MM_true(isEmpty());

		MM_EvacuatorCopyspace::reset(region, base);
		MM_EvacuatorCopyspace::setCopyspace(region, base, copy, length, isLOA);
		_scan = _base;

		assertScanspaceInvariant();
	}

	/**
	 * Load a split array segment into this scanspace. This will set the base and end points to the
	 * complete array bounds and the scan and copy heads to the end point so split array scanspaces
	 * always appear to be empty; isEmpty() is always true, getWorkSize(), getCopySize(),
	 * getWhiteSize() will always return 0.
	 *
	 * @param base points to indexable object containing the segment
	 * @param end points after end of contiguous indexable object containing the segment
	 * @param start 1-based array index of first element in the segment
	 * @param stop 1-based array index of element after the last element segment
	 */
	void
	setSplitArrayScanspace(MM_EvacuatorBase::Region region, uint8_t *array, uint8_t *end, uintptr_t start, uintptr_t stop)
	{
		Debug_MM_true(isEmpty());

		MM_EvacuatorCopyspace::reset(region, array);
		MM_EvacuatorCopyspace::setCopyspace(region, array, end, (uintptr_t)(end - array));
		_flags |= isSplitArrayFlag;
		_startSlot = start;
		_stopSlot = stop;
		_scan = _end;

		Debug_MM_true((NULL != getActiveObject()) && _objectScanner->isIndexableObject());
		assertScanspaceInvariant();
	}

	/**
	 * Test whether scanspace contains a split array segment
	 */
	bool isSplitArrayScanspace() const { return isSplitArrayFlag == (_flags & (uintptr_t)isSplitArrayFlag); }

	/**
	 * Return the scanned volume of the split array segment (or 0 if not a split array scanscpace).
	 */
	uintptr_t getSplitArrayScanVolume() const { return ((_stopSlot - _startSlot) * _evacuator->getReferenceSlotSize()); }

	/**
	 * Return the start slot offset of the split array segment (or 0 if not a split array scanscpace).
	 */
	uintptr_t getSplitArrayStartOffset() const { return _startSlot; }

	/**
	 * Return the end slot offset of the split array segment (or 0 if not a split array scanscpace).
	 */
	uintptr_t getSplitArrayStopOffset() const { return _stopSlot; }

	/**
	 * Return true if scanspace holds no unscanned work and no whitespace .
	 */
	bool isEmpty() const { return (_scan == _copy) && (_copy == _end); }

	/**
	 * Return the number of bytes remaining to be scanned (this will always be 0 for active split array scanspaces).
	 */
	uintptr_t getWorkSize() const { return isSplitArrayScanspace() ? getSplitArrayScanVolume() : (_copy - _scan); }

	/**
	 * Return pointer to active object scanner, or NULL if object scanner not instantiated.
	 */
	GC_ObjectScanner *getActiveObjectScanner() { return _objectScanner; }

	/**
	 * Return pointer to active object scanner, or NULL if object scanner not instantiated.
	 */
	omrobjectptr_t getActiveObject()
	{
		if (NULL != getActiveObjectScanner()) {
			if (isSplitArrayScanspace()) {
				return (omrobjectptr_t)_base;
			} else {
				return (omrobjectptr_t)_scan;
			}
		}
		return NULL;
	}

	/**
	 * Return the position of the scan head
	 */
	uint8_t *getScanHead() const { return _scan; }

	/**
	 * Advance the scan pointer to next unscanned object and drop active object scanner.
	 *
	 * @param scannedBytes number of bytes scanned (size of scanned object)
	 */
	void
	advanceScanHead(uintptr_t scannedBytes)
	{
		assertScanspaceInvariant();
		Debug_MM_true((_scan < _copy) == !isSplitArrayScanspace());

		/* done scanning current object */
		if (_scan < _copy) {
			/* clear scan work for scalar object */
			_scan += scannedBytes;
		} else {
			/* scan head for split array scanspaces is preset to end -- set start slot to end slot to clear work */
			_startSlot = _stopSlot;
		}

		/* done with active object scanner */
		_objectScanner = NULL;

		assertScanspaceInvariant();
	}

	omrobjectptr_t
	cutWork(uintptr_t *length, uintptr_t *offset)
	{
		assertScanspaceInvariant();
		omrobjectptr_t work = NULL;

		/* only cut inactive frames */
		if (0 == _activations) {

			/* cut unscanned work between scan and copy head into workspace */
			if (isSplitArrayScanspace()) {
				work = (omrobjectptr_t)getBase();
				*length = getSplitArrayStopOffset() - getSplitArrayStartOffset();
				*offset = getSplitArrayStartOffset() + 1;
			} else {
				Debug_MM_true(_base == _scan);
				Debug_MM_true(0 < getWorkSize());
				Debug_MM_true(NULL == _objectScanner);
				work = (omrobjectptr_t)getBase();
				*length = getWorkSize();
				*offset = 0;
			}

			/* clear cut work from scanspace */
			_scan = _copy;
			reset();
		}

		assertScanspaceInvariant();
		return work;
	}

	/**
	 * Pull work and remaining whitespace from a scanspace from point of last copy.
	 *
	 * @param fromspace the source copyspace
	 * @param base points to head of tail to pull
	 */
	void
	pullTail(MM_EvacuatorScanspace *fromspace, uint8_t *base)
	{
		fromspace->assertScanspaceInvariant();
		assertScanspaceInvariant();
		Debug_MM_true(isEmpty() || (this == fromspace));
		Debug_MM_true(!fromspace->isSplitArrayScanspace());
		Debug_MM_true(!isSplitArrayScanspace());

		/* idempotent */
		if (this == fromspace) {
			return;
		}

		/* pull work and whitespace from copyspace into this scanspace and rebase copyspace to copy head */
		setScanspace(fromspace->getEvacuationRegion(), base, fromspace->_copy, fromspace->_end - base, fromspace->isLOA());

		/* truncate fromspace at point of last copy */
		fromspace->_copy = fromspace->_end = base;

		/* reset remembered state and passivate active scanner */
		clearRememberedState();
		_objectScanner = NULL;

		fromspace->assertScanspaceInvariant();
		assertScanspaceInvariant();
	}

	/**
	 * Pull whitespace from another scanspace
	 *
	 * @param fromspace the other scanspace
	 */
	void
	pullWhitespace(MM_EvacuatorScanspace *fromspace)
	{
		Debug_MM_true(isEmpty() || (this == fromspace));
		Debug_MM_true(!fromspace->isSplitArrayScanspace());
		Debug_MM_true(!isSplitArrayScanspace());
		fromspace->assertScanspaceInvariant();
		assertScanspaceInvariant();

		/* idempotent */
		if (this == fromspace) {
			return;
		}

		/* pull whitespace at end of fromspace into this scanspace */
		setScanspace(fromspace->getEvacuationRegion(), fromspace->_copy, fromspace->_copy, fromspace->getWhiteSize(), fromspace->isLOA());

		/* trim tail of fromspace and leave base, scan head and flags as they are */
		fromspace->_end = fromspace->_copy;

		fromspace->assertScanspaceInvariant();
		assertScanspaceInvariant();
	}

	/**
	 * Pull work from an outside copyspace leaving it empty of work and retaining whitespace.
	 *
	 * @param fromspace the source copyspace
	 */
	void
	pullWork(MM_EvacuatorCopyspace *fromspace)
	{
		assertScanspaceInvariant();
		fromspace->assertCopyspaceInvariant();
		Debug_MM_true(isEmpty() || (this == fromspace));
		Debug_MM_true(!isSplitArrayScanspace());

		/* idempotent */
		if (asCopyspace() == fromspace) {
			return;
		}

		/* pull work from copyspace into this scanspace and rebase copyspace to copy head */
		uintptr_t length = 0;
		uint8_t *workspace = fromspace->rebase(&length);
		setScanspace(fromspace->getEvacuationRegion(), workspace, workspace + length, length, fromspace->isLOA());

		/* reset remembered state and passivate active scanner */
		clearRememberedState();
		_objectScanner = NULL;
		fromspace->assertCopyspaceInvariant();
		assertScanspaceInvariant();
	}

	/**
	 * Reset the base to scan head to clear scanned work, retaining region containment.
	 */
		/**
	 * Bump activation count
	 */
	void activated() { _activations += 1; }

	/**
	 * Return the number of times this scanspace has been activated (pushed into)
	 */
	uintptr_t getActivationCount() const { return _activations; }

	/**
	 * Reset scanspace activation count.
	 */
	uintptr_t clearActivationCount() { uintptr_t activations = _activations; _activations = 0; return activations; }

	void
	assertScanspaceInvariant() const
	{
		MM_EvacuatorCopyspace::assertCopyspaceInvariant();
		Debug_MM_true(0 == (_flags & ~(((uintptr_t)scanEndFlag << 1) - 1)));
		Debug_MM_true(_base <= _scan);
		Debug_MM_true(_scan <= _copy);
	}

	/**
	 * Constructor
	 *
	 * @param evacuator pointer to owning evacuator instance, which may not be fully instatiated.
	 */
	MM_EvacuatorScanspace()
		: MM_EvacuatorCopyspace()
		, _objectScanner(NULL)
		, _scan(NULL)
		, _startSlot (0)
		, _stopSlot(0)
		, _activations(0)
	{ }
};

#endif /* EVACUATORSCANSPACE_HPP_ */
