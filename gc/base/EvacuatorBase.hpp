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

/**
 * EVACUATOR_DEBUG* should be defined only for debug builds.
 *
 *  Do not commit this file with any of the following macros defined:
 */
#undef EVACUATOR_DEBUG			/* enables fine-grained assertions in evacuator code plus metrics gathering and reporting */
#undef EVACUATOR_DEBUG_TRACE	/* enables scavenger remembered set tracing (if EVACUATOR_DEBUG or EVACUATOR_DEBUG_ALWAYS are defined) */
#undef EVACUATOR_DEBUG_ALWAYS	/* enables metrics gathering and reporting */

#if defined(EVACUATOR_DEBUG) && defined(EVACUATOR_DEBUG_ALWAYS)
#error "EVACUATOR_DEBUG and EVACUATOR_DEBUG_ALWAYS are mutually exclusive"
#endif /* defined(EVACUATOR_DEBUG) && defined(EVACUATOR_DEBUG_ALWAYS) */

#if !defined(OMR_SCAVENGER_TRACK_COPY_DISTANCE)
#define OMR_SCAVENGER_TRACK_COPY_DISTANCE
#endif /* !defined(OMR_SCAVENGER_TRACK_COPY_DISTANCE) */
#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
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
#include "EnvironmentBase.hpp"
#include "GCExtensionsBase.hpp"
#include "HeapLinkedFreeHeader.hpp"
#if defined(EVACUATOR_DEBUG)
#include "Math.hpp"
#endif /* defined(EVACUATOR_DEBUG) */
#include "ObjectModel.hpp"
#include "ScavengerStats.hpp"

#if defined(EVACUATOR_DEBUG)
#define Debug_MM_true(assertion) Assert_MM_true(assertion)
#define Debug_MM_true1(env, assertion, format, arg) Assert_GC_true_with_message(env, assertion, format, arg)
#define Debug_MM_true2(env, assertion, format, arg1, arg2) Assert_GC_true_with_message2(env, assertion, format, arg1, arg2)
#define Debug_MM_true3(env, assertion, format, arg1, arg2, arg3) Assert_GC_true_with_message3(env, assertion, format, arg1, arg2, arg3)
#define Debug_MM_true4(env, assertion, format, arg1, arg2, arg3, arg4) Assert_GC_true_with_message4(env, assertion, format, arg1, arg2, arg3, arg4)
#if defined(OMR_ARCH_X86)
#define Debug_MM_if(condition) if (condition) __asm__("int3")
#define Debug_MM_address(address) Debug_MM_if((uintptr_t)address == (_extensions->evacuatorTraceOptions >> 32))
#else
#define Debug_MM_if(condition)
#define Debug_MM_address(address)
#endif /* defined(OMR_ARCH_X86) */
#else
#define Debug_MM_true(assertion)
#define Debug_MM_true1(env, assertion, format, arg)
#define Debug_MM_true2(env, assertion, format, arg1, arg2)
#define Debug_MM_true3(env, assertion, format, arg1, arg2, arg3)
#define Debug_MM_true4(env, assertion, format, arg1, arg2, arg3, arg4)
#define Debug_MM_if(condition)
#define Debug_MM_address(address)
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

/* delegate can define additional flags in bits [16-31]; bits [32-63] are reserved for a debug heap address */
#define EVACUATOR_DEBUG_DELEGATE_BASE 0x10000
#define EVACUATOR_DEBUG_DELEGATE_TOP 0x80000000

/* from scavenger.cpp (CACHE_LINE_SIZE) */
#if defined(J9ZOS390) || (defined(LINUX) && defined(S390))
#define EVACUATOR_CACHE_LINE 256
#elif defined(AIXPPC) || defined(LINUXPPC)
#define EVACUATOR_CACHE_LINE 128
#else
#define EVACUATOR_CACHE_LINE 64
#endif /* defined(J9ZOS390) || (defined(LINUX) && defined(S390)) */

class MM_EnvironmentStandard;

/**
 * Evacuator base class exposes evacuator features accessible to worklist and whitelist.
 */
class MM_EvacuatorBase : public MM_BaseNonVirtual
{
/**
 * Data members
 */
private:

protected:
	/* evacuator instance index in controller */
	const uintptr_t _workerIndex;

	/* environment (thread) attached to this evacuator */
	MM_EnvironmentStandard *_env;

	/* Global GC extensions */
	MM_GCExtensionsBase * const _extensions;

	/* GC object model for language */
	GC_ObjectModel * const _objectModel;

	/* System memory allocator */
	MM_Forge * const _forge;

	/* Reference slot size in bytes */
	const uintptr_t _sizeofObjectReferenceSlot;

	/* Tracing options always in build (for prototyping) */
	const uintptr_t _evacuatorTraceOptions;

	/* Scanning options always in build (for prototyping) */
	const uintptr_t _evacuatorScanOptions;

	/* lower and upper bounds for nursery semispaces and tenure space (may be updated from other thread allocating whitespace) */
	uint8_t *_heapBounds[3][2];

public:
	/**
	 * Whitespace represents a contiguous range of reserved and unused heap/RAM as a heap linked free header
	 * extended with a uintptr_t flag bitmap (smaller ranges are filled with heap holes and discarded). All
	 * available whitespace is retained on evacuator whitelists which are priority sources for reserving
	 * survivor/tenure space for object copy.
	 *
	 * (NOTE: Whitespace of size < min_reusable_whitespace are discardable and never used for copy. These fragments are
	 * wrapped with Whitespace so that they can be passed along to the whitelists, for accounting purposes only. LOA
	 * status for these fragments is irrelevant.)
	 */
	class Whitespace : private MM_HeapLinkedFreeHeader
	{
	/*
	 * Data members
	 */
	private:
		uintptr_t _flags;
	protected:
	public:
		enum {
			  dead		= J9_GC_OBJ_HEAP_HOLE
			, hole		= J9_GC_OBJ_HEAP_HOLE_MASK
			, loa 		= hole | 0x4
			, compress	= hole | 0x8
			, poisoned	= hole | EVACUATOR_DEBUG_POISON_DISCARD
		};

	/*
	 * Function members
	 */
	private:
	protected:
	public:
		static Whitespace * whitespace(void *address, uintptr_t length, uintptr_t flags = hole);
		static bool isDead(uintptr_t *slot) { return (Whitespace::dead == (Whitespace::hole & *slot)); }
		bool isLOA() { return (sizeof(MM_HeapLinkedFreeHeader) < length()) && (loa ==  (_flags & loa)); }
		uint8_t *getBase() { return (uint8_t *)this; }
		uint8_t *getEnd() { return getBase() + length(); }
		uintptr_t
		length()
		{
			switch (hole & _next) {
			case J9_GC_SINGLE_SLOT_HOLE:
				return sizeof(uintptr_t);
			case J9_GC_MULTI_SLOT_HOLE:
				return getSize();
			default:
				Assert_MM_unreachable();
			}
			return 0;
		}
	};

	/**
	 * Workspaces encapsulate copied material that needs to be scanned (ie, work released from outside
	 * copyspaces or large object copyspace). They are queued on evacuator worklists until pulled onto
	 * the scan stack for scanning. Workspaces not so queued are reset and freed to evacuator freelists.
	 *
	 * Each workspace represents the location and size of a contiguous range of material copied to survivor
	 * offset is shifted (0-based -> 1-based) and the base points to the array object. For all other
	 * workspaces the base points to the first object in the workspace, the offset field is 0 and the
	 * length field represents the volume of work in the workspace in bytes (respecting heap object alignment).
	 */
	typedef struct Workspace {
		union {
			uint8_t *base;			/* points to base of workspace viewed as array of bytes */
			omrobjectptr_t object;	/* points to array object in pointer array workspace or first object in mixed workspace */
		};
		uintptr_t length;			/* number of array slots in pointer array workspace or number bytes to scan in mixed workspace */
		uintptr_t offset;			/* >0 for split array workspaces only, 1-based array offset to first of <length> slots in segment to scan */
		Workspace *next;			/* points to next workspace in queue, or NULL */
	} Workspace;

	/* Enumeration of memory spaces that are receiving evacuated material */
	typedef enum Region {
		survivor					/* survivor semispace for current gc */
		, tenure					/* tenure space */
		, evacuate					/* evacuate semispace for current gc */
		, unreachable				/* upper bound for evacuation regions */
		, sources = 2				/* number of regions that source whitespace for evacuation */
		, regions = 3				/* number of heap regions */
	} Region;

	/* Enumeration of stack volume metrics: bytes copied inside, outside, bytes scanned */
	typedef enum VolumeMetric {
		  survivor_copy		/* bytes copied to survivor */
		, tenure_copy		/* bytes copied to tenure */
		, scanned			/* bytes scanned in survivor or tenure */
		, leaf				/* bytes not scanned in primitive objects */
		, objects			/* number of objects copied/scanned */
		, survivor_recycled	/* number of bytes of recycled survivor whitespace */
		, tenure_recycled	/* number of bytes of recycled tenure whitespace */
		, survivor_discarded/* number of bytes discarded as unusable survivor whitespace */
		, tenure_discarded	/* number of bytes discarded as unusable tenure whitespace */
		, volume_metrics	/* upper bound for volume metrics enum */
		, array_counters = OMR_SCAVENGER_OBJECT_BINS
	} VolumeMetric;

	/* Enumeration of thread metrics: wait/sync/end counts/times; 256 counters for 8x8 stall map combinations (up to 8 threads) */
	typedef enum ThreadMetric {
		  cpu_ms			/* cpu time thread spent in workThreadGarbasgeCollect() */
		, real_ms			/* total time thread spent in workThreadGarbasgeCollect() */
		, scan_count		/* identity count incremented by 1 each time evacuator enters collective heap scan */
		, pulled_count		/* number of workspaces pulled from other threads */
		, pulled_volume		/* volume of workspaces pulled from other threads */
		, clearing_count	/* identity count incremented by 1 each time evacuator enters collective clearing scan */
		, stall_count		/* number of times thread ran dry of local work and had to poll others for work */
		, stall_ms			/* time spent with thread waiting controller mutex to enable polling */
		, wait_count		/* number of times thread stopped to wait on the controller mutex for notification of work/end */
		, wait_ms			/* time spent with thread waiting on the controller mutex for notification of work/end */
		, notify_count		/* number of times thread notified stalled threads of work */
		, notify_ms			/* time spent with thread waiting notify stalled threads of work */
		, sync_count		/* number of times thread stopped to sync with other threads */
		, sync_ms			/* time spent with thread waiting to sync with other threads */
		, end_count			/* number of times thread stalled to wait for other threads to end heap scan */
		, end_ms			/* time spent with thread waiting to wait for other threads to end heap scan  */
		, stalled			/* base for 256 counters for 8x8 stall map */
		, stall_map = 256
		, thread_metrics = stalled + stall_map
	} ThreadMetric;

	/* Enumeration of conditions that relate to evacuator operation (superset of evacuatorScanOptions */
	typedef enum ConditionFlag {
		  stack_overflow = 1			/* forcing outside copy and minimal work release threshold while winding down stack after stack overflow */
		, survivor_tail_fill = 2		/* forcing outside copy to fill survivor outside copyspace remainder */
		, tenure_tail_fill = 4			/* forcing outside copy to fill tenure outside copyspace remainder */
		, stall = 8						/* forcing minimal work release threshold while distributing outside copy to stalled evacuators */
		, breadth_first_roots = 16		/* forcing outside copy for root objects */
		, scan_remembered = 32			/* this is raised while scanning the remembered set */
		, scan_roots = 64				/* this is raised while scanning the root set */
		, scan_worklist = 128			/* this is raised while scanning the worklist */
		, scan_clearable = 256			/* this is raised while delegating to clearing stages */
		, indexable_object = 512		/* forcing outside copy for a pointer array object */
		, breadth_first_always = 1024	/* forcing outside copy for all objects all the time */
		, conditions_mask = 2047		/* bit mask covering above flags */
		, condition_states = 2048		/* number of possible condition combinations */
		, condition_count = 11			/* number of conditions */
		, initial_mask = (0)
		, options_mask = (breadth_first_roots | breadth_first_always)
		, outside_mask = (options_mask)
	} ConditionFlag;

	/* Evacuator metrics store. All metrics are additive across threads and are aggregated when evacuators exit rendezvous with controller. */
	typedef struct Metrics {
		uintptr_t _volumeMetrics[volume_metrics];			/* cumulative numbers of bytes copied out of evacuation semispace and scanned in survivor or tenure space */
		uintptr_t _arrayVolumeCounts[array_counters + 1];	/* array volume histogram indexed by log2(array-byte-size) + total array volume */
		uintptr_t _conditionMetrics[condition_states]; 		/* counters for condition state per evacuated object */
		uint64_t _threadMetrics[thread_metrics]; 			/* counters for thread wait counts and times and stalled thread map */
		ThreadMetric _lastMonitorMetric;					/* stall/wait_count metric recorded with last omr_monitor_enter/wait() call */
		uint64_t _lastMonitorTime;							/* holds clock time delta for last omr_monitor_enter/wait() call */
	} Metrics;

	/* minimum size of scan stack -- a value of 1 forces breadth first copying */
	static const uintptr_t min_scan_stack_depth = 1;

	/* modal upper bound for allowing object copy to inside copyspace */
	static const uintptr_t modal_inside_copy_size = OMR_MINIMUM_OBJECT_SIZE << 1;

	/* modal upper bound for copy distance from scanspace base */
	static const uintptr_t modal_inside_copy_distance = 0;

	/* modal upper bound for copy distance from scanspace base */
	static const uintptr_t cache_line_size = EVACUATOR_CACHE_LINE;

	/* indexable pointer array upper bound for copy distance from scanspace base */
	static const uintptr_t max_cached_indexable_size = cache_line_size << 1;

	/* minimum size of whitespace that can be retained on a whitelist must not exceed host L1 cache line size */
	static const uintptr_t min_reusable_whitespace = cache_line_size;

	/* largest amount of whitespace that can be trimmed and discarded from stack whitespaces */
	static const uintptr_t max_scanspace_remainder = 32;

	/* largest amount of whitespace that can be trimmed and discarded from outside copyspaces */
	static const uintptr_t max_copyspace_remainder = 256;

	/* smallest allowable setting for workspace size */
	static const uintptr_t min_workspace_size = 2048;

	/* smallest allowable setting for copyspace size */
	static const uintptr_t min_copyspace_size = 4096;

	/* smallest allowable setting for work release threshold overrides controller's minimum workspace size */
	static const uintptr_t min_workspace_release = min_workspace_size;

	/* number of bits to shift object classification bits into condition flags */
	static const uintptr_t classification_shift = 9;

	/* per thread volume/thread/condition metrics */
	Metrics *_metrics;

/**
 * Function members
 */
private:

protected:

public:
	/**
	 * Index of evacuator instance on controller bus
	 */
	uintptr_t getWorkerIndex() const { return _workerIndex; }

	/**
	 * Evacuator is bound to a GC thread for the duration of each GC, this is the thread's environment
	 */
	MM_EnvironmentStandard *getEnvironment() const { return _env; }

	/**
	 * Get a pointer to evacuator metrics
	 */
	Metrics *getMetrics() { return _metrics; }

	/**
	 * Tests for inclusion in heap regions
	 */
	bool isInEvacuate(void *address) const { return (_heapBounds[evacuate][0] <= (uint8_t *)address) && ((uint8_t *)address < _heapBounds[evacuate][1]); }
	bool isInSurvivor(void *address) const { return (_heapBounds[survivor][0] <= (uint8_t *)address) && ((uint8_t *)address < _heapBounds[survivor][1]); }
	bool isInTenure(void *address) const { return getExtensions()->isOld((omrobjectptr_t)address); }

	Region
	sourceOf(void *address)  const
	{
		if ((isInSurvivor(address) || ((uint8_t *)address == _heapBounds[survivor][1]))) {
			return survivor;
		} else if ((isInTenure(address) || ((uint8_t *)address == _heapBounds[tenure][1]))) {
			return tenure;
		}
		Assert_MM_true(NULL == address);
		return unreachable;
	}

	/**
	 * Get the complementary evacuation region (survivor|tenure)
	 *
	 * @param region the evacuation region to complement
	 * @return complementary evacuation region
	 */
	Region
	other(Region region) const
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
	static bool isScanOptionSelected(MM_GCExtensionsBase *extensions, uintptr_t scanOptions) { return (0 != (selectedScanOptions(extensions) & scanOptions)); }
	static bool isTraceOptionSelected(MM_GCExtensionsBase *extensions, uintptr_t traceOptions) { return (0 != (extensions->evacuatorTraceOptions & traceOptions)); }

	bool isScanOptionSelected(uintptr_t scanOptions) const { return (0 != (_evacuatorScanOptions & scanOptions)); }
	bool isTraceOptionSelected(uintptr_t traceOptions) const { return (0 != (_evacuatorTraceOptions & traceOptions)); }

	MM_GCExtensionsBase *getExtensions() const { return _extensions; }
	uintptr_t getReferenceSlotSize() const { return _sizeofObjectReferenceSlot; }
	bool compressObjectReferences() const { return _extensions->compressObjectReferences(); }
	bool addressOverflow(uint8_t *address, uintptr_t size) const { return (0 > size) && (((uintptr_t)address + size) < size) && ((address + size) != 0); }

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	static const char *conditionName(ConditionFlag condition);
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
		/* conditions that are not optional or optional and not selected are censored; any other condition may be set at runtime */
		extensions->evacuatorScanOptions &= options_mask;

		/* minimum stack depth forces breadth-first always and breadth-first always forces breadth-first roots */
		if ((extensions->evacuatorMaximumStackDepth <= min_scan_stack_depth) || (0 != (extensions->evacuatorScanOptions & breadth_first_always))) {
			extensions->evacuatorScanOptions |= (breadth_first_always | breadth_first_roots);
			extensions->evacuatorMaximumStackDepth = min_scan_stack_depth;
		}

		Debug_MM_true((extensions->evacuatorMaximumStackDepth <= min_scan_stack_depth) == (0 != (extensions->evacuatorScanOptions & breadth_first_always)));
		return extensions->evacuatorScanOptions;
	}

	MM_EvacuatorBase(uintptr_t workerIndex, MM_GCExtensionsBase *extensions)
	: MM_BaseNonVirtual()
	, _workerIndex(workerIndex)
	, _env(NULL)
	, _extensions(extensions)
	, _objectModel(&_extensions->objectModel)
	, _forge(_extensions->getForge())
	, _sizeofObjectReferenceSlot(extensions->compressObjectReferences() ? sizeof(uint32_t) : sizeof(uintptr_t))
	, _evacuatorTraceOptions(_extensions->evacuatorTraceOptions)
	, _evacuatorScanOptions(selectedScanOptions(_extensions) | ((uintptr_t)conditions_mask & ~(uintptr_t)options_mask))
	, _metrics(NULL)
	{
		Assert_MM_true(indexable_object == (1 << classification_shift));
	}
};

#endif /* EVACUATORBASE_HPP_ */
