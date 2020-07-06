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

#ifndef EVACUATORBASE_HPP_
#define EVACUATORBASE_HPP_

#undef EVACUATOR_DEBUG
#undef EVACUATOR_DEBUG_TRACE
#undef EVACUATOR_DEBUG_ALWAYS

#if defined(EVACUATOR_DEBUG) && defined(EVACUATOR_DEBUG_ALWAYS)
#error "EVACUATOR_DEBUG and EVACUATOR_DEBUG_ALWAYS are mutually exclusive"
#endif /* defined(EVACUATOR_DEBUG) && defined(EVACUATOR_DEBUG_ALWAYS) */

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
#define OMR_SCAVENGER_TRACK_COPY_DISTANCE
#if defined(EVACUATOR_DEBUG_TRACE)
#define OMR_SCAVENGER_TRACE
#define OMR_SCAVENGER_TRACE_REMEMBERED_SET
#define OMR_SCAVENGER_TRACE_BACKOUT
#define OMR_SCAVENGER_TRACE_COPY
#endif /* defined(EVACUATOR_DEBUG_TRACE) */
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

#include "ModronAssertions.h"
#include "omr.h"
#if defined(EVACUATOR_DEBUG)
#include "omrgcconsts.h"
#endif /* defined(EVACUATOR_DEBUG) */

#include "BaseNonVirtual.hpp"
#include "GCExtensionsBase.hpp"
#if defined(EVACUATOR_DEBUG)
#include "Math.hpp"
#endif /* defined(EVACUATOR_DEBUG) */

class MM_EnvironmentStandard;

#if defined(EVACUATOR_DEBUG)
#define Debug_MM_true(assertion) Assert_MM_true(assertion)
#define Debug_MM_true1(env, assertion, format, arg) Assert_GC_true_with_message(env, assertion, format, arg)
#define Debug_MM_true2(env, assertion, format, arg1, arg2) Assert_GC_true_with_message2(env, assertion, format, arg1, arg2)
#define Debug_MM_true3(env, assertion, format, arg1, arg2, arg3) Assert_GC_true_with_message3(env, assertion, format, arg1, arg2, arg3)
#define Debug_MM_true4(env, assertion, format, arg1, arg2, arg3, arg4) Assert_GC_true_with_message4(env, assertion, format, arg1, arg2, arg3, arg4)
#else
#define Debug_MM_true(assertion)
#define Debug_MM_true1(env, assertion, format, arg)
#define Debug_MM_true2(env, assertion, format, arg1, arg2)
#define Debug_MM_true3(env, assertion, format, arg1, arg2, arg3)
#define Debug_MM_true4(env, assertion, format, arg1, arg2, arg3, arg4)
#endif /* defined(EVACUATOR_DEBUG) */

/* base debug flags (evacuatorTraceOptions) */
#define EVACUATOR_DEBUG_END 1
#define EVACUATOR_DEBUG_CYCLE 2
#define EVACUATOR_DEBUG_EPOCH 4
#define EVACUATOR_DEBUG_WORK 8
#define EVACUATOR_DEBUG_STACK 16
#define EVACUATOR_DEBUG_COPY 32
#define EVACUATOR_DEBUG_CONDITIONS 32
#define EVACUATOR_DEBUG_REMEMBERED 64
#define EVACUATOR_DEBUG_ALLOCATE 128
#define EVACUATOR_DEBUG_WHITELISTS 256
#define EVACUATOR_DEBUG_POISON_DISCARD 512
#define EVACUATOR_DEBUG_BACKOUT 1024
#define EVACUATOR_DEBUG_DELEGATE 2048
#define EVACUATOR_DEBUG_HEAPCHECK 4096

/* delegate can define additional flags above 0x10000 */
#define EVACUATOR_DEBUG_DELEGATE_BASE 0x10000

class MM_EvacuatorBase : public MM_BaseNonVirtual
{
/**
 * Data members
 */
private:

protected:
	/* Enumeration of stack volume metrics: bytes copied inside, outside, bytes scanned are reset when stack empties */
	typedef enum VolumeMetric {
		  survivor_copy	/* bytes copied to survivor */
		, tenure_copy	/* bytes copied to tenure */
		, scanned		/* bytes scanned in survivor or tenure */
		, metrics		/* upper bound for volume metrics enum */
	} VolumeMetric;

	/* evacuator instance index in controller */
	const uintptr_t _workerIndex;

	/* environment (thread) attached to this evacuator */
	MM_EnvironmentStandard *_env;

	/* Global GC extensions */
	MM_GCExtensionsBase *_extensions;

	/* Reference slot size in bytes */
	const uintptr_t _sizeofObjectReferenceSlot;

	/* Tracing options always in build (for prototyping) */
	const uintptr_t _evacuatorTraceOptions;

	/* Scanning options always in build (for prototyping) */
	const uintptr_t _evacuatorScanOptions;

	/* bitmap of current evacuator operating conditions (including scan options) */
	uintptr_t _conditionFlags;

	/* lower and upper bounds for nursery semispaces and tenure space */
	uint8_t *_heapBounds[3][2];

public:
	/* Enumeration of memory spaces that are receiving evacuated material */
	typedef enum Region {
		survivor					/* survivor semispace for current gc */
		, tenure					/* tenure space */
		, evacuate					/* evacuate semispace for current gc */
		, unreachable				/* upper bound for evacuation regions */
	} Region;

	/* Enumeration of conditions that relate to evacuator operation (superset of evacuatorScanOptions */
	typedef enum ConditionFlag {
		  stack_overflow = 1			/* forcing outside copy and minimal work release threshold while winding down stack after stack overflow */
		, depth_first = 2				/* forcing depth-first scanning while in stack overflow condition */
		, survivor_tail_fill = 4		/* forcing outside copy to fill survivor outside copyspace remainder */
		, tenure_tail_fill = 8			/* forcing outside copy to fill tenure outside copyspace remainder */
		, remembered_set = 16			/* this is raised while scanning the remembered set */
		, recursive_object = 32			/* forcing outside copy for a chain of mixed self referencing objects rooted in object to be scanned */
		, indexable_object = 64			/* forcing array elements outside for close copying */
		, stall = 128					/* forcing minimal work release threshold while distributing outside copy to stalled evacuators */
		, breadth_first_roots = 256		/* forcing outside copy for root objects */
		, scanning_heap = 512			/* this is raised while evacuator is in collective heap scan */
		, clearing = 1024				/* this is raised while evacuator is in clearing stages */
		, breadth_first_always = 2048	/* forcing outside copy for all objects all the time */
		, conditions_mask = 4095		/* bit mask covering above flags */
		, initial_mask = (breadth_first_always)
		, options_mask = (breadth_first_always + breadth_first_roots + recursive_object + indexable_object + stack_overflow)
		, outside_mask = (options_mask + stall)
	} ConditionFlag;

	/* Minimal size of scan stack -- a value of 1 forces breadth first scanning */
	static const uintptr_t min_scan_stack_depth = 1;

	/* Object size threshold for copying inside -cannot be set to a value lower than this */
	static const uintptr_t min_inside_object_size = OMR_MINIMUM_OBJECT_SIZE;

	/* minimum size of whitespace that can be retained on a whitelist */
	static const uintptr_t min_reusable_whitespace = 256;

	/* largest amount of whitespace that can be trimmed and discarded from stack whitespaces */
	static const uintptr_t max_scanspace_remainder = 32;

	/* largest amount of whitespace that can be trimmed and discarded from outside copyspaces */
	static const uintptr_t max_copyspace_remainder = 256;

	/* smallest allowable setting for work release threshold overrides controller's minimum workspace size */
	static const uintptr_t min_workspace_release = 512;

	/* multiplier for minimum workspace release determines threshold byte count for objects overflowing copyspace whitespace remainder */
	static const uintptr_t max_copyspace_overflow_quanta = 6;

	/* soft upper bound on evacuator worklist volume in units of maximum workspace size to scale with workspace size */
	static const uintptr_t worklist_volume_ceiling = 64;

	/* controller selects evacuator sampling rate to produce a fixed number of epochs per gc cycle (sometimes this works out :) */
	static const uintptr_t epochs_per_cycle = 64;

	/* each evacuator reports progress at a preset number of points in each epoch */
	static const uintptr_t reports_per_epoch = 64;

/**
 * Function members
 */
private:

protected:

public:
	/**
	 * Index of vacuator instance on controller bus
	 */
	uintptr_t getWorkerIndex() const { return _workerIndex; }

	/**
	 * Evacuator is bound to a GC thread for the duration of each GC, this is the thread's environment
	 */
	MM_EnvironmentStandard *getEnvironment() const { return _env; }

	/**
	 * Tests for inclusion in heap regions
	 */
	bool isInEvacuate(void *address) const { return (_heapBounds[evacuate][0] <= (uint8_t *)address) && ((uint8_t *)address < _heapBounds[evacuate][1]); }
	bool isInSurvivor(void *address) const { return (_heapBounds[survivor][0] <= (uint8_t *)address) && ((uint8_t *)address < _heapBounds[survivor][1]); }
	bool isInTenure(void *address) const { return getExtensions()->isOld((omrobjectptr_t)address); }

	/**
	 * Get the heap region (survivor|tenure|evacuate) containing an address
	 *
	 * Note: this will assert hard if address is not in heap or cannot be disambiguated
	 * with a verifiable presumption.
	 *
	 * @param address a putative heap address
	 * @param presumed the region presumed in case the address is ambiguous (lies on upper bound for region)
	 * @return heap region or assert hard if address not in heap or ambiguous without presumption
	 */
	Region
	getEvacuationRegion(void *address, Region presumed = unreachable) const
	{
		Debug_MM_true((address >= _heapBounds[tenure][0]) && (address <= OMR_MAX(_heapBounds[survivor][1], _heapBounds[evacuate][1])));

		Region region = unreachable;
		if ((address > _heapBounds[survivor][0]) && (address < _heapBounds[survivor][1])) {
			region = survivor;
		} else if ((address >= _heapBounds[tenure][0]) && (address < OMR_MIN(_heapBounds[survivor][0], _heapBounds[evacuate][0]))) {
			region = tenure;
		} else if (address == _heapBounds[survivor][0]) {
			if ((address == _heapBounds[tenure][1]) && ((presumed == survivor) || (presumed == tenure))) {
				region = presumed;
			} else if ((address == _heapBounds[evacuate][1]) && ((presumed == survivor) || (presumed == evacuate))) {
				region = presumed;
			} else {
				region = survivor;
			}
		} else if ((address > _heapBounds[evacuate][0]) && (address < _heapBounds[evacuate][1])) {
			region = evacuate;
		} else if (address == _heapBounds[evacuate][0]) {
			if ((address == _heapBounds[tenure][1]) && ((presumed == evacuate) || (presumed == tenure))) {
				region = presumed;
			} else if ((address == _heapBounds[survivor][1]) && ((presumed == survivor) || (presumed == evacuate))) {
				region = presumed;
			} else {
				region = evacuate;
			}
		} else if ((presumed < unreachable) && (address == _heapBounds[presumed][1])) {
			region = presumed;
		}
#if defined(EVACUATOR_DEBUG)
		if ((region != presumed) || (region == unreachable)) {
			return unreachable;
		}
#endif /* defined(EVACUATOR_DEBUG) */
		Assert_MM_true(region == presumed);
		Assert_MM_true(region != unreachable);
		return region;
	}

	uint8_t *
	disambiguate(Region containingRegion, uint8_t *endpoint) const
	{
		Debug_MM_true((containingRegion == survivor) || (containingRegion == tenure));
		if (endpoint == _heapBounds[tenure][1]) {
			endpoint = getRegionBase(containingRegion);
		} else if (endpoint == _heapBounds[survivor][1]) {
			Debug_MM_true(containingRegion == survivor);
			endpoint = getRegionBase(survivor);
		} else if (NULL == endpoint) {
			endpoint = getRegionBase(containingRegion);
		}
		Debug_MM_true(containingRegion == getEvacuationRegion(endpoint, containingRegion));
		return endpoint;
	}

	/**
	 * Get the low bound of an evacuation region
	 *
	 * @param region the region of interest
	 * @return the base (least) address included in the region addess range
	 */
	uint8_t *getRegionBase(Region region) const { return _heapBounds[region][0]; }

	/**
	 * Get the complementary evacuation region (survivor|tenure)
	 *
	 * @param region the evacuation region to complement
	 * @return complementary evacuation region
	 */
	Region
	otherEvacuationRegion(Region region) const
	{
		Debug_MM_true((survivor == region) || (tenure == region));

		return (Region)(1 - (intptr_t)region);
	}

	/**
	 * Get the next evacuation region in Region enumeration order
	 *
	 * @param region the evacuation region to incremment
	 * @return next evacuation region, or unreachable
	 */
	static Region
	nextEvacuationRegion(Region region)
	{
		switch(region) {
		case survivor:
			return tenure;
		case tenure:
			return evacuate;
		default:
			return unreachable;
		}
	}

	static const char *callsite(const char *id);

	static bool
	isScanOptionSelected(MM_GCExtensionsBase *extensions, uintptr_t scanOptions)
	{
		return (0 != (selectedScanOptions(extensions) & scanOptions));
	}
	bool isScanOptionSelected(uintptr_t scanOptions) const { return (0 != (_evacuatorScanOptions & scanOptions)); }

	static bool
	isTraceOptionSelected(MM_GCExtensionsBase *extensions, uintptr_t traceOptions)
	{
		return (0 != (extensions->evacuatorTraceOptions & traceOptions));
	}
	bool isTraceOptionSelected(uintptr_t traceOptions) const { return (0 != (_evacuatorTraceOptions & traceOptions)); }

	bool isConditionSet(uintptr_t conditionFlags) const {return (0 != (_conditionFlags & conditionFlags)); }

	MM_GCExtensionsBase *getExtensions() const { return _extensions; }

	bool compressObjectReferences() const { return _extensions->compressObjectReferences(); }

	uintptr_t getReferenceSlotSize() const { return _sizeofObjectReferenceSlot; }

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	static uintptr_t
	conditionCount()
	{
		return MM_Math::floorLog2((uintptr_t)conditions_mask + 1);
	}
	static const char *
	conditionName(ConditionFlag condition)
	{
		static const char *conditionNames[] = {"so","df","stf","ttf","rso","ro","io","stall","bfr","sh","clr","bfa"};
		uintptr_t flag = MM_Math::floorLog2((uintptr_t)condition);

		Debug_MM_true((conditionCount() * sizeof(const char *)) == sizeof(conditionNames));
		Debug_MM_true((flag < conditionCount()) && (((uintptr_t)1 << flag) == condition));

		return ((flag < conditionCount()) && (((uintptr_t)1 << flag) == condition)) ? conditionNames[flag] : "";
	}
	bool isAnyDebugFlagSet(uintptr_t flags) const { return isTraceOptionSelected(flags); }
	bool isDebugEnd() const { return isAnyDebugFlagSet(EVACUATOR_DEBUG_END); }
	bool isDebugCycle() const { return isAnyDebugFlagSet(EVACUATOR_DEBUG_CYCLE); }
	bool isDebugEpoch() const { return isAnyDebugFlagSet(EVACUATOR_DEBUG_EPOCH); }
	bool isDebugStack() const { return isAnyDebugFlagSet(EVACUATOR_DEBUG_STACK); }
	bool isDebugWork() const { return isAnyDebugFlagSet(EVACUATOR_DEBUG_WORK); }
	bool isDebugCopy() const { return isAnyDebugFlagSet(EVACUATOR_DEBUG_COPY); }
	bool isDebugConditions() const { return isAnyDebugFlagSet(EVACUATOR_DEBUG_CONDITIONS); }
	bool isDebugRemembered() const { return isAnyDebugFlagSet(EVACUATOR_DEBUG_REMEMBERED); }
	bool isDebugWhitelists() const { return isAnyDebugFlagSet(EVACUATOR_DEBUG_WHITELISTS); }
	bool isDebugPoisonDiscard() const { return isAnyDebugFlagSet(EVACUATOR_DEBUG_POISON_DISCARD); }
	bool isDebugAllocate() const { return isAnyDebugFlagSet(EVACUATOR_DEBUG_ALLOCATE); }
	bool isDebugBackout() const { return isAnyDebugFlagSet(EVACUATOR_DEBUG_BACKOUT); }
	bool isDebugDelegate() const { return isAnyDebugFlagSet(EVACUATOR_DEBUG_DELEGATE); }
	bool isDebugHeapCheck() const { return isAnyDebugFlagSet(EVACUATOR_DEBUG_HEAPCHECK); }
#else
	static uintptr_t conditionCount() { return 0; }
	static const char *conditionName(ConditionFlag condition) { return ""; }
	bool isAnyDebugFlagSet(uintptr_t traceOptions) const { return false; }
	bool isDebugEnd() const { return false; }
	bool isDebugCycle() const { return false; }
	bool isDebugEpoch() const { return false; }
	bool isDebugWork() const { return false; }
	bool isDebugStack() const { return false; }
	bool isDebugCopy() const { return false; }
	bool isDebugConditions() const { return false; }
	bool isDebugRemembered() const { return false; }
	bool isDebugWhitelists() const { return false; }
	bool isDebugPoisonDiscard() const { return false; }
	bool isDebugAllocate() const { return false; }
	bool isDebugBackout() const { return false; }
	bool isDebugDelegate() const { return false; }
	bool isDebugHeapCheck() const { return false; }
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

	static uintptr_t
	selectedScanOptions(MM_GCExtensionsBase *extensions)
	{
		uintptr_t scanOptions = extensions->evacuatorScanOptions & (uintptr_t)options_mask;

		/* minimum stack depth forces breadth-first always and breadth-first always forces breadth-first roots */
		if ((extensions->evacuatorMaximumStackDepth <= MM_EvacuatorBase::min_scan_stack_depth) || (0 != (scanOptions & breadth_first_always))) {
			scanOptions |= (breadth_first_always | breadth_first_roots);
		}

		/* conditions that are optional and not selected are censored; any other condition may be at runtime */
		return scanOptions;
	}

	/* Note: any address in process space will do for reference slot size calculation */
	MM_EvacuatorBase(uintptr_t workerIndex, MM_GCExtensionsBase *extensions)
	: _workerIndex(workerIndex)
	, _env(NULL)
	, _extensions(extensions)
	, _sizeofObjectReferenceSlot(extensions->compressObjectReferences() ? sizeof(uint32_t) : sizeof(uintptr_t))
	, _evacuatorTraceOptions(_extensions->evacuatorTraceOptions)
	, _evacuatorScanOptions(selectedScanOptions(_extensions) | ((uintptr_t)conditions_mask & ~(uintptr_t)options_mask))
	, _conditionFlags(0)
	{ }
};

#endif /* EVACUATORBASE_HPP_ */
