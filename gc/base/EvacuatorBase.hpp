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
#define EVACUATOR_DEBUG_ALWAYS

#if defined(EVACUATOR_DEBUG) && defined(EVACUATOR_DEBUG_ALWAYS)
#error "EVACUATOR_DEBUG and EVACUATOR_DEBUG_ALWAYS are mutually exclusive"
#endif /* defined(EVACUATOR_DEBUG) && defined(EVACUATOR_DEBUG_ALWAYS) */

#include "omr.h"

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
#define OMR_SCAVENGER_TRACK_COPY_DISTANCE
#if defined(EVACUATOR_DEBUG_TRACE)
#define OMR_SCAVENGER_TRACE
#define OMR_SCAVENGER_TRACE_REMEMBERED_SET
#define OMR_SCAVENGER_TRACE_BACKOUT
#define OMR_SCAVENGER_TRACE_COPY
#endif /* defined(EVACUATOR_DEBUG_TRACE) */
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

#if defined(EVACUATOR_DEBUG)
#include "omrgcconsts.h"
#include "ModronAssertions.h"

#include "Math.hpp"

/* default debug flags */
#if defined(EVACUATOR_DEBUG)
#define EVACUATOR_DEBUG_DEFAULT_FLAGS 0
#else
#define EVACUATOR_DEBUG_DEFAULT_FLAGS 0
#endif /* defined(EVACUATOR_DEBUG) */

/* delegate can define additional flags above 0x10000 */
#define EVACUATOR_DEBUG_DELEGATE_BASE 0x10000

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

#include "BaseNonVirtual.hpp"
#include "GCExtensionsBase.hpp"

#define NOINLINE

/* base debug flags (evacuatorTraceOptions) */
#define EVACUATOR_DEBUG_END 1
#define EVACUATOR_DEBUG_CYCLE 2
#define EVACUATOR_DEBUG_EPOCH 4
#define EVACUATOR_DEBUG_WORK 8
#define EVACUATOR_DEBUG_STACK 16
#define EVACUATOR_DEBUG_COPY 32
#define EVACUATOR_DEBUG_REMEMBERED 64
#define EVACUATOR_DEBUG_ALLOCATE 128
#define EVACUATOR_DEBUG_WHITELISTS 256
#define EVACUATOR_DEBUG_POISON_DISCARD 512
#define EVACUATOR_DEBUG_BACKOUT 1024
#define EVACUATOR_DEBUG_DELEGATE 2048
#define EVACUATOR_DEBUG_HEAPCHECK 4096

class MM_EvacuatorBase : public MM_BaseNonVirtual
{
/**
 * Data members
 */
private:

protected:
	/* Enumeration of stack volume metrics: bytes copied inside, outside, bytes scanned are reset when stack empties */
	typedef enum StackVolumeMetric {
		  inside
		, outside
		, scanned
	} StackVolumeMetric;

	/* Global GC extensions */
	MM_GCExtensionsBase *_extensions;

	/* bitmap of current evacuator operating conditions */
	uintptr_t _conditionFlags;

	/* Reference slot size in bytes */
	const uintptr_t _sizeofObjectReferenceSlot;

	/* Tracing options always in build (for prototyping) */
	const uintptr_t _evacuatorTraceOptions;

	/* Scanning options always in build (for prototyping) */
	const uintptr_t _evacuatorScanOptions;

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
		  breadth_first_always = 1	/* forcing outside copy for all objects all the time */
		, breadth_first_roots = 2	/* forcing outside copy for root objects */
		, scanning_heap = 4			/* this is raised while evacuator is in collective heap scan */
		, stall = 8					/* forcing minimal work release threshold while distributing outside copy to stalled evacuators */
		, recursive_object = 16		/* forcing outside copy for a chain of mixed self referencing objects rooted in object to be scanned */
		, indexable_object = 32		/* forcing array elements outside for close copying */
		, survivor_tail_fill = 64	/* forcing outside copy to fill survivor outside copyspace remainder */
		, tenure_tail_fill = 128	/* forcing outside copy to fill tenure outside copyspace remainder */
		, stack_overflow = 256		/* forcing outside copy and minimal work release threshold while winding down stack after stack overflow */
		, depth_first = 512			/* forcing depth-first scanning up the stack until popped to bottom frame without stack_overflow */
		, conditions_mask = 1023	/* bit mask covering above flags */
		, outside_mask = (breadth_first_always + breadth_first_roots + recursive_object + indexable_object + stall + stack_overflow)
		, static_mask = (breadth_first_always + breadth_first_roots + indexable_object)
		, options_mask = (static_mask + recursive_object)
		, dynamic_mask = (conditions_mask - static_mask)
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
	static const char *callsite(const char *id);

	static bool
	isScanOptionSelected(MM_GCExtensionsBase *extensions, uintptr_t scanOptions)
	{
		return (0 != (extensions->evacuatorScanOptions & scanOptions));
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
	static uintptr_t conditionCount() { return MM_Math::floorLog2((uintptr_t)conditions_mask + 1); }
	static const char *
	conditionName(ConditionFlag condition)
	{
		static const char *conditionNames[] = {"bfa","bfr","sh","stall","ro","io","stf","ttf","so","df"};
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
	bool isDebugRemembered() const { return false; }
	bool isDebugWhitelists() const { return false; }
	bool isDebugPoisonDiscard() const { return false; }
	bool isDebugAllocate() const { return false; }
	bool isDebugBackout() const { return false; }
	bool isDebugDelegate() const { return false; }
	bool isDebugHeapCheck() const { return false; }
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

	static uintptr_t
	staticScanOptions(MM_GCExtensionsBase *extensions)
	{
		uintptr_t scanOptions = extensions->evacuatorScanOptions;

		/* minimum stack depth forces breadth-first always and breadth-first always forces breadth-first roots */
		if ((extensions->evacuatorMaximumStackDepth <= MM_EvacuatorBase::min_scan_stack_depth) || isScanOptionSelected(extensions, breadth_first_always)) {
			scanOptions |= (breadth_first_always | breadth_first_roots);
		}

		return scanOptions;
	}

	/* Note: any address in process space will do for reference slot size calculation */
	MM_EvacuatorBase(MM_GCExtensionsBase *extensions)
	: _extensions(extensions)
	, _conditionFlags(0)
	, _sizeofObjectReferenceSlot(extensions->compressObjectReferences() ? sizeof(uint32_t) : sizeof(uint64_t))
	, _evacuatorTraceOptions(_extensions->evacuatorTraceOptions)
	, _evacuatorScanOptions(staticScanOptions(_extensions))
	{ }
};

#endif /* EVACUATORBASE_HPP_ */
