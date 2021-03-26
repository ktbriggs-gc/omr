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

/**
 * TODO:
 */
#ifndef EVACUATORCONTROLLER_HPP_
#define EVACUATORCONTROLLER_HPP_

#include "omr.h"
#include "omrcfg.h"

#include "AtomicSupport.hpp"
#include "Collector.hpp"
#include "EnvironmentStandard.hpp"
#include "Evacuator.hpp"
#include "EvacuatorBase.hpp"
#include "EvacuatorParallelTask.hpp"
#include "GCExtensionsBase.hpp"
#include "Heap.hpp"
#include "ParallelDispatcher.hpp"
#include "ParallelScavengeTask.hpp"

class GC_ObjectScanner;
class MM_MemorySubSpace;
class MM_MemorySubSpaceSemiSpace;

/**
 * The controller periodically sums copied/scanned byte counts over all evacuators to obtain
 * information about progress of evacuation and asserts if the aggregate volume of material
 * copied is not equal to the aggregate volume scanned at the the of each evacuation stage
 * (roots, clearable, ..) and at the end of each generational collection cycle.
 *
 * All pointers and lengths are expressed in bytes (uint8_t) in white/work/copy/scan spaces.
 */
class MM_EvacuatorController : public MM_Collector
{
/**
 * Data members
 */
private:
	/* constants for mapping evacuator index to word and bit offset in bound/stalled/resuming evacuator bitmaps */
	static const uintptr_t index_to_map_word_shift = ((OMR_LOG_POINTER_SIZE == 3) ? 6 : 5);
	static const uintptr_t index_to_map_word_modulus = (((uintptr_t)1 << index_to_map_word_shift) - 1);

	typedef MM_Evacuator *EvacuatorPointer;

	const uintptr_t _maxGCThreads;						/* fixed for life of vm, never <_evacuatorCount */
	MM_Evacuator ** const _evacuatorTask;				/* array of pointers to instantiated evacuators */
	MM_Evacuator::Metrics ** const _evacuatorMetrics;	/* array of pointers to evacuator metrics */
	volatile uintptr_t _evacuatorCount;					/* number of gc threads that will participate in collection */
	volatile uintptr_t * const _boundEvacuatorBitmap;	/* maps evacuator threads that have been dispatched and bound to an evacuator instance */
	volatile uintptr_t * const _stalledEvacuatorBitmap;	/* maps evacuator threads that are stalled (waiting for work) */
	volatile uintptr_t * const _resumingEvacuatorBitmap;/* maps evacuator threads that are resuming or completing scan cycle after stalling */
	volatile uintptr_t * const _evacuatorMask;			/* maps all evacuator threads, bound or unbound */
	volatile uintptr_t _isNotifyOfWorkPending;			/* non-zero if an evacuator is stalled and notification of work is pending */
	volatile uintptr_t _stalledEvacuatorCount;			/* number of stalled evacuator threads */
	volatile uintptr_t _evacuatorFlags;					/* private and public (language defined) evacuation flags shared among evauators */
	volatile uintptr_t _copyspaceAllocationCeiling[2];	/* upper bounds for copyspace allocation in survivor, tenure regions */
	volatile uintptr_t _objectAllocationCeiling[2];		/* upper bounds for object allocation in survivor, tenure regions */

	MM_MemorySubSpace *_memorySubspace[3];				/* pointer to memory subspace per heap region */

protected:

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	uint64_t _collectorStartTime;						/* collector startup time */
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

	MM_Evacuator::Metrics _aggregateMetrics;			/* total aggregate metrics accumulated in current gc cycle */
	const uintptr_t _objectAlignmentInBytes;			/* cached object alignment from GC_ObjectModelBase */

	/* fields pulled down from MM_Scavenger ... the memory subspace pointers and nursery semispace bounds are cached above */
	MM_GCExtensionsBase * const _extensions;			/* points to GC extensions */
	MM_ParallelDispatcher * const _dispatcher;			/* dispatches evacuator tasks */
	uintptr_t _tenureMask;								/* tenure mask for selecting whether evacuated object should be tenured */
	MM_MemorySubSpaceSemiSpace *_activeSubSpace; 		/* top level new subspace subject to GC */
	MM_MemorySubSpace *_evacuateMemorySubSpace;			/* cached pointer to memory subspace for evacuate semispace */
	MM_MemorySubSpace *_survivorMemorySubSpace; 		/* cached pointer to memory subspace for survivor semispace */
	MM_MemorySubSpace *_tenureMemorySubSpace;			/* cached pointer to memory subspace for tenure subspace */
	void *_evacuateSpaceBase, *_evacuateSpaceTop;		/* cached base and top heap pointers for evacuate semispace */
	void *_survivorSpaceBase, *_survivorSpaceTop;		/* cached base and top heap pointers for survivor semispace */

public:
	/* boundary between public (delegate, low 16 bits) and private (evacuator, high 16 bits) evacuation flags */
	static const uintptr_t max_evacuator_public_flag = (uintptr_t)1 << 15;
	static const uintptr_t min_evacuator_private_flag = max_evacuator_public_flag << 1;

	/* private evacuation flags */
	enum {
		 rescanThreadSlots = min_evacuator_private_flag << 2	/* rescan threads after first heap scan, before clearing */
		, aborting = min_evacuator_private_flag << 3	/* an evacuator has failed and gc cycle is aborting */
	};

	/* lower and upper address bounds per heap region */
	volatile uint8_t *_heapLayout[3][2];

	/* hard lower and upper bounds for whitespace allocation are bound to tlh size */
	const uintptr_t _minimumCopyspaceSize;
	const uintptr_t _maximumCopyspaceSize;

	/* hard lower and upper bounds for workspace size, evacuator lower bound can be less than controller's while flushing outside */
	const uintptr_t _minimumWorkspaceSize;
	const uintptr_t _maximumWorkspaceSize;

	omrthread_monitor_t	_workMutex;				/* synchronize evacuator work distribution and end of scan cycle */
	OMR_VM *_omrVM;

/**
 * Function members
 */
private:
	/* set or refresh bounds for each heap region from corresponding memory subspaces */
	MMINLINE void setHeapLayout();

	/* calculate whitespace allocation size considering evacuator's production scaling factor */
	MMINLINE uintptr_t calculateOptimalWhitespaceSize(MM_Evacuator::Region region);

	/* calculate the number of active words in the evacuator bitmaps */
	MMINLINE uintptr_t countEvacuatorBitmapWords(uintptr_t *tailMask, uintptr_t evacuatorCount) const;

	/* calculate the number of active words in the evacuator bitmaps */
	MMINLINE uintptr_t countEvacuatorBitmapWords(uintptr_t *tailMask) const;

	/* fill evacuator bitmap with all 1s (reliable only when caller holds controller mutex) */
	MMINLINE void fillEvacuatorBitmap(volatile uintptr_t * const bitmap, uintptr_t evacuatorCount);

	/* fill evacuator bitmap with all 1s (reliable only when caller holds controller mutex) */
	MMINLINE void fillEvacuatorBitmap(volatile uintptr_t * const bitmap);

	/* test evacuator bitmap for all 0s (reliable only when caller holds controller mutex) */
	MMINLINE bool isEvacuatorBitmapEmpty(volatile uintptr_t * const bitmap) const;

	/* test evacuator bitmap for all 1s (reliable only when caller holds controller mutex) */
	MMINLINE bool isEvacuatorBitmapFull(volatile uintptr_t * const bitmap) const;

	/* set evacuator bit in evacuator bitmap */
	MMINLINE uintptr_t setEvacuatorBit(uintptr_t evacuatorIndex, volatile uintptr_t * const bitmap);

	/* clear evacuator bit in evacuator bitmap */
	MMINLINE void clearEvacuatorBit(uintptr_t evacuatorIndex, volatile uintptr_t * const bitmap);

	/* get bit mask for evacuator bit as aligned in evacuator bitmap */
	uintptr_t
	getEvacuatorBitMask(uintptr_t evacuatorIndex) const
	{
		Debug_MM_true(evacuatorIndex < _evacuatorCount);
		return (uintptr_t)1 << (evacuatorIndex & index_to_map_word_modulus);
	}

	/* calculate word/bit coordinates for worker index */
	uintptr_t
	mapEvacuatorIndexToMapAndMask(uintptr_t evacuatorIndex, uintptr_t *evacuatorBitmask) const
	{
		Debug_MM_true(evacuatorIndex < _evacuatorCount);
		*evacuatorBitmask = getEvacuatorBitMask(evacuatorIndex);
		return evacuatorIndex >> index_to_map_word_shift;
	}

	/* set evacuator bit in evacuator bitmap */
	bool
	testEvacuatorBit(uintptr_t evacuatorIndex, volatile uintptr_t * const bitmap) const
	{
		Debug_MM_true(evacuatorIndex < _evacuatorCount);
		uintptr_t evacuatorMask = 0;
		uintptr_t evacuatorMap = mapEvacuatorIndexToMapAndMask(evacuatorIndex, &evacuatorMask);
		return (evacuatorMask == (bitmap[evacuatorMap] & evacuatorMask));
	}

protected:
	virtual bool initialize(MM_EnvironmentBase *env);
	virtual void tearDown(MM_EnvironmentBase *env);

	/**
	 * Evacuator instances hold onto tenure whitelist contents between back-to-back nursery collections. These must
	 * be flushed before each global collection and should be flushed at collector shutdown.
	 *
	 * @param shutdown set false for global gc, true for collector shutdown
	 */
	void flushTenureWhitespace(bool shutdown);

	/**
	 * Called to initiate an evacuation and inform controller of number of GC threads that will participate (as
	 * evacuators) and heap layout.
	 *
	 * @param env environment for calling (master) thread
	 */
	virtual void masterSetupForGC(MM_EnvironmentStandard *env);

	/* Assert that aggregate volumes of work copied and scanned are equal unless aborting */
	void assertGenerationalInvariant(MM_EnvironmentStandard *env);

public:
	MM_GCExtensionsBase * const getExtensions() { return _extensions; }

	/**
	 * Wrappers to enable matriculation of thread wait/notify times
	 */
	static uint64_t
	enter(MM_EnvironmentStandard *env, omrthread_monitor_t monitor, MM_Evacuator::ThreadMetric counter)
	{
		OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
		uint64_t time = omrtime_hires_clock();
		omrthread_monitor_enter(monitor);
		time  = omrtime_hires_clock() - time;
		env->getMetrics()->_threadMetrics[counter] += 1;
		env->getMetrics()->_threadMetrics[counter + 1] += time;
		env->getMetrics()->_lastMonitorMetric = counter;
		env->getMetrics()->_lastMonitorTime = time;
		return time;
	}

	static uint64_t
	knock(MM_EnvironmentStandard *env, omrthread_monitor_t monitor)
	{
		OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
		uint64_t time = omrtime_hires_clock();
		return (0 == omrthread_monitor_try_enter(monitor)) ? time : 0;
	}

	static uint64_t
	pause(MM_EnvironmentStandard *env, omrthread_monitor_t monitor, MM_Evacuator::ThreadMetric counter)
	{
		OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
		uint64_t time = omrtime_hires_clock();
		omrthread_monitor_wait(monitor);
		time = omrtime_hires_clock() - time;
		env->getMetrics()->_threadMetrics[counter] += 1;
		env->getMetrics()->_threadMetrics[counter + 1] += time;
		env->getMetrics()->_lastMonitorMetric = counter;
		env->getMetrics()->_lastMonitorTime = time;
		return time;
	}

	static uint64_t
	leave(MM_EnvironmentStandard *env, omrthread_monitor_t monitor, uint64_t time, MM_Evacuator::ThreadMetric counter)
	{
		OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
		omrthread_monitor_exit(monitor);
		time = omrtime_hires_clock() - time;
		env->getMetrics()->_threadMetrics[counter] += 1;
		env->getMetrics()->_threadMetrics[counter + 1] += time;
		return time;
	}

	static void
	leave(MM_EnvironmentStandard *env, omrthread_monitor_t monitor)
	{
		omrthread_monitor_exit(monitor);
	}

	MM_Evacuator::Metrics *getEvacuatorMetrics(uintptr_t index) { return _evacuatorMetrics[index]; }

	void aggregateEvacuatorMetrics(MM_EnvironmentStandard *env);

	/**
	 * Get the memory subspace backing heap region.
	 *
	 * @return a pointer to the memory subspace
	 */
	MM_MemorySubSpace *getMemorySubspace(MM_Evacuator::Region region) const { return _memorySubspace[region]; }

	/**
	 * Test for object alignment
	 *
	 * @param pointer pointer to test
	 * @return true if pointer is object aligned
	 */
	bool isObjectAligned(void *pointer) const { return 0 == ((uintptr_t)pointer & (_objectAlignmentInBytes - 1)); }

	/**
	 * Adjust to object size
	 *
	 * @param size to be adjusted
	 * @return adjusted size
	 */
	uintptr_t alignToObjectSize(uintptr_t size) const { return _extensions->objectModel.adjustSizeInBytes(size); }

	/**
	 * Controller delegates backout and remembered set to subclass
	 *
	 * TODO: MM_EvacuatorRememberedSet & MM_EvacuatorBackout
	 */
	virtual bool collectorStartup(MM_GCExtensionsBase* extensions);
	virtual void collectorShutdown(MM_GCExtensionsBase* extensions);
	virtual void collectorExpanded(MM_EnvironmentBase *env, MM_MemorySubSpace *subSpace, uintptr_t expandSize);
	virtual void scavengeRememberedSet(MM_EnvironmentStandard *env) = 0;
	virtual void pruneRememberedSet(MM_EnvironmentStandard *env) = 0;
	virtual void setBackOutFlag(MM_EnvironmentBase *env, BackOutState value) = 0;
	virtual void completeBackOut(MM_EnvironmentStandard *env) = 0;
	virtual void mergeThreadGCStats(MM_EnvironmentBase *env) = 0;
	virtual uintptr_t calculateTenureMask() = 0;

	/**
	 * Atomically test & set/reset a (public) evacuator flag.
	 *
	 * This method is also used with private flags used by controller and evacuators.
	 *
	 * @param flag the flag (bit) to set or reset
	 * @param value true to set the flag, false to reset
	 * @return true if the bit was previously set
	 */
	bool setEvacuatorFlag(uintptr_t flag, bool value);

	/**
	 * The controller maintains a bitset of public (defined by delegate) and private (defined by controller)
	 * flags that are used to communicate runtime conditions across all evacuators. The following methods are
	 * used to synchronize multicore views of the flags.
	 *
	 * The flags are all cleared at the start of each gc cycle.
	 */
	bool isEvacuatorFlagSet(uintptr_t flag) const { return (flag == (_evacuatorFlags & flag)); }
	bool isAnyEvacuatorFlagSet(uintptr_t flags) const { return (0 != (_evacuatorFlags & flags)); }
	bool areAllEvacuatorFlagsSet(uintptr_t flags) const { return (flags == (_evacuatorFlags & flags)); }
	void resetEvacuatorFlags() { VM_AtomicSupport::set(&_evacuatorFlags, 0); }

	/**
	 * Get the number of GC threads dispatched for current gc cycle
	 */
	uintptr_t getEvacuatorThreadCount() const { return _evacuatorCount; }

	/**
	 * Get the number of GC threads active (not stalled) as of time of call
	 */
	uintptr_t getActiveThreadCount() const { return _evacuatorCount - _stalledEvacuatorCount; }

	/* these methods return accurate results only when caller holds the controller or evacuator mutex */
	bool isBoundEvacuator(uintptr_t evacuatorIndex) const { return testEvacuatorBit(evacuatorIndex, _boundEvacuatorBitmap); }
	bool isStalledEvacuator(uintptr_t evacuatorIndex) const { return isBoundEvacuator(evacuatorIndex) && testEvacuatorBit(evacuatorIndex, _stalledEvacuatorBitmap); }
	bool areAnyEvacuatorsStalled() const { return (0 < _stalledEvacuatorCount); }

	/**
	 * Get the nearest neighboring bound evacuator, or wrap around and return identity if no other evacuators are bound
	 */
	MM_Evacuator *
	getNextEvacuator(MM_Evacuator *evacuator) const
	{
		/* skip evacuator to start enumeration */
		uintptr_t nextIndex = evacuator->getWorkerIndex();

		/* traverse evacuator bitmask in increasing index order, wrap index to 0 after last bound evacuator */
		do {

			nextIndex += 1;
			if (nextIndex >= _evacuatorCount) {
				nextIndex = 0;
			}

		} while ((nextIndex != evacuator->getWorkerIndex()) && !isBoundEvacuator(nextIndex));

		return _evacuatorTask[nextIndex];
	}

	/**
	 * Parallel task wrapper calls this for *master* thread to instantiate or resurrect evacuator instances at the beginning of a gc cycle.
	 *
	 * @param env the environment for the calling thread
	 */
	void bindWorkers(MM_EnvironmentStandard *env);

	/**
	 * Parallel task wrapper calls this to bind *worker* threads to evacuator instances at the beginning of a gc cycle.
	 *
	 * @param env the environment for the calling thread
	 */
	void bindWorker(MM_EnvironmentStandard *env);

	/**
	 * Parallel task wrapper calls this to unbind worker thread to an evacuator instance at the end of a gc cycle.
	 *
	 * @param env the environment for the worker thread
	 */
	void unbindWorker(MM_EnvironmentStandard *env);

	/**
	 * Acquire the controller to gain access to the work distribution bus.
	 */
	void acquireController() { omrthread_monitor_enter(_workMutex); }

	/**
	 * Release the controller.
	 */
	void releaseController() { omrthread_monitor_exit(_workMutex); }

	/**
	 * Evacuator calls controller when waiting for work or to synchronize with completing evacuators. All
	 * evacuator threads are synchronized when they are all stalled and have empty worklists. This join point
	 * marks the end of a heap scan. At this point the sums of the number of bytes copied and bytes scanned by
	 * each evacuator must be equal unless the scan is aborting. Any found work is discarded if any evacuator
	 * has raised an abort condition.
	 *
	 * @param worker the evacuator that is waiting for work
	 * @return true if worker must wait for work
	 */
	bool waitForWork(MM_Evacuator *worker);

	/**
	 * Test pending notification flag.
	 *
	 * @return true if there is a notification request pending.
	 */
	bool isNotifyOfWorkPending() { return (1 == _isNotifyOfWorkPending); }

	/**
	 * Reduce work release threshold to reduce granularity of workspaces on worklist.
	 *
	 * @return volume of work required to release workspaces from evacuator outside copyspaces
	 */
	uintptr_t getWorkReleaseThreshold() const;

	/**
	 * Evacuator has fulfilled work quota when it's volume of distributable work is greater than the notification
	 * quota. Running evacuators flush larger volumes to outside copyspaces to generate distributable workspaces as
	 * long as 1 or more evacuators are stalled.
	 *
	 * @return volume of work required to fulfill quota
	 */
	uintptr_t getWorkDistributionQuota() const;

	/*
	 * Evacuators with distributable work will notify controller at the instant that a stall condition is
	 * raised. If >1 evacuators have distributable work all should call this method but only one will
	 * actually acquire the controller to post the notification.
	 *
	 * Caller must not own controller at point of call.
	 *
	 * @param worker the evacuator that is notifying
	 */
	void notifyOfWork(MM_Evacuator *evacuator);

	/**
	 * Evacuator calls this to determine whether there is scan work remaining in any evacuator's queue. If
	 * not then the volume of evacuated material must equal the volume scanned unless any evacuator has
	 * raised an abort condition.
	 *
	 * Note that this will falsely return true until an evacuator makes an interim progress report. It
	 * is intended for use after all evacuator threads have completed or aborted a prior scan cycle,
	 * either to assert completeness or test for evacuated material after a clearable root scan.
	 *
	 * @return true if all material evacuated has been scanned (end of successful scan cycle)
	 */
	bool
	hasCompletedScan() const
	{
		uintptr_t copied = _aggregateMetrics._volumeMetrics[MM_Evacuator::survivor] + _aggregateMetrics._volumeMetrics[MM_Evacuator::tenure];
		uintptr_t scanned = _aggregateMetrics._volumeMetrics[MM_Evacuator::scanned];
		return (copied == scanned);
	}

	/**
	 * Get global abort flag value. This is set if any evacuator raises an abort condition
	 *
	 * @return true if the evacuation has been aborted
	 */
	bool isAborting() const { return isEvacuatorFlagSet(aborting); }

	/**
	 * Atomically test and set global abort flag to true. This is set if any evacuator raises an abort condition.
	 *
	 * @return true if abort flag was previously set, false if caller is first to set it
	 */
	bool setAborting();

	/**
	 * Region memory subspace is exhausted when any evacuator fails to allocate a minimal copyspace
	 *
	 * @param region the heap region (survivor/tenure)
	 * @param size allocation size in bytes
	 * @return true if an allocation of size bytes might (not necessarily) succeed
	 */
	bool isAllocatable(MM_Evacuator::Region region, uintptr_t sizeInBytes);

	/**
	 * Evacuator calls this to get free space for refreshing inside and outside copyspaces.
	 *
	 * @param worker the calling evacuator
	 * @param region the region (survivor or tenure) to obtain free space from
	 * @param length the (minimum) number of bytes of free space required, a larger chunk may be allocated at controller discretion
	 * @return a pointer to space allocated, which may be larger that the requested length, or NULL
	 */
	MM_Evacuator::Whitespace *getWhitespace(MM_Evacuator *worker, MM_Evacuator::Region region, uintptr_t length);

	/**
	 * Get the maximal size for allocation of whitespace (TLH) to refresh a copyspace
	 *
	 * @param region survivor or tenure
	 * @return maximal size for allocation of whitespace (TLH) to refresh a copyspace
	 */
	uintptr_t maxWhitespaceAllocation(MM_Evacuator::Region region) { return _copyspaceAllocationCeiling[region]; }

	/**
	 * Evacuator calls this to get free space for solo objects.
	 *
	 * @param worker the calling evacuator
	 * @param region the region (survivor or tenure) to obtain free space from
	 * @param length the (exact) number of bytes of free space required
	 * @return a pointer to space allocated, which will be the requested length, or NULL
	 */
	MM_Evacuator::Whitespace *getObjectWhitespace(MM_Evacuator *worker, MM_Evacuator::Region region, uintptr_t length);

	/**
	 * Get the maximal size for allocation of whitespace (TLH) to refresh a copyspace
	 *
	 * @param region survivor or tenure
	 * @return maximal size for allocation of whitespace (TLH) to refresh a copyspace
	 */
	uintptr_t maxObjectAllocation(MM_Evacuator::Region region) { return _objectAllocationCeiling[region]; }

	/* allocate and NULL-fill evacuator pointer array (evacuators are instantiated at gc start as required) */
	static MM_Evacuator**
	allocateEvacuatorArray(MM_EnvironmentBase *env, uintptr_t maxGCThreads)
	{
		MM_Evacuator **evacuatorArray = NULL;

		if (env->getExtensions()->isEvacuatorEnabled()) {
			Debug_MM_true(0 < maxGCThreads);
			uintptr_t evacuatorPointerArraySize = sizeof(uintptr_t) * maxGCThreads;
			evacuatorArray = (MM_Evacuator**)env->getForge()->allocate(evacuatorPointerArraySize, OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
			Debug_MM_true(NULL != evacuatorArray);
			for (uintptr_t evacuator = 0; evacuator < maxGCThreads; evacuator += 1) {
				evacuatorArray[evacuator] = NULL;
			}
		}

		return evacuatorArray;
	}

	/* allocate and NULL-fill evacuator pointer array (evacuators are instantiated at gc start as required) */
	static MM_Evacuator::Metrics**
	allocateEvacuatorMetrics(MM_EnvironmentBase *env, uintptr_t maxGCThreads)
	{
		MM_Evacuator::Metrics *metricsArray = NULL;
		MM_Evacuator::Metrics **evacuatorMetrics = NULL;

		Debug_MM_true(0 < maxGCThreads);
		uintptr_t metricsPointerArraySize = sizeof(uintptr_t) * maxGCThreads;
		uintptr_t metricsArraySize = sizeof(MM_Evacuator::Metrics) * maxGCThreads;
		evacuatorMetrics = (MM_Evacuator::Metrics **)env->getForge()->allocate(metricsPointerArraySize, OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
		metricsArray = (MM_Evacuator::Metrics *)env->getForge()->allocate(metricsArraySize, OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
		for (uintptr_t evacuator = 0; evacuator < maxGCThreads; evacuator += 1) {
			evacuatorMetrics[evacuator] = &metricsArray[evacuator];
		}

		Assert_MM_true(NULL != evacuatorMetrics);
		Assert_MM_true(NULL != metricsArray);
		return evacuatorMetrics;
	}

	/* allocate and 0-fill evacuator thread bitmap */
	static volatile uintptr_t *
	allocateEvacuatorBitmap(MM_EnvironmentBase *env, uintptr_t maxGCThreads)
	{
		volatile uintptr_t *map = NULL;

		if (env->getExtensions()->isEvacuatorEnabled()) {
			Debug_MM_true(0 < maxGCThreads);
			uintptr_t mapWords = (maxGCThreads >> index_to_map_word_shift) + ((0 != (maxGCThreads & index_to_map_word_modulus)) ? 1 : 0);
			map = (volatile uintptr_t *)env->getForge()->allocate(sizeof(uintptr_t) * mapWords, OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
			Debug_MM_true(NULL != map);
			for (uintptr_t word = 0; word < mapWords; word += 1) {
				map[word] = 0;
			}
		}

		return map;
	}

	/**
	 * Constructor
	 */
	MM_EvacuatorController(MM_EnvironmentBase *env)
		: MM_Collector()
		, _maxGCThreads(((MM_ParallelDispatcher *)env->getExtensions()->dispatcher)->threadCountMaximum())
		, _evacuatorTask(allocateEvacuatorArray(env, _maxGCThreads))
		, _evacuatorMetrics(allocateEvacuatorMetrics(env, _maxGCThreads))
		, _evacuatorCount(0)
		, _boundEvacuatorBitmap(allocateEvacuatorBitmap(env, _maxGCThreads))
		, _stalledEvacuatorBitmap(allocateEvacuatorBitmap(env, _maxGCThreads))
		, _resumingEvacuatorBitmap(allocateEvacuatorBitmap(env, _maxGCThreads))
		, _evacuatorMask(allocateEvacuatorBitmap(env, _maxGCThreads))
		, _isNotifyOfWorkPending(0)
		, _stalledEvacuatorCount(0)
		, _evacuatorFlags(0)
#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		, _collectorStartTime(0)
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
		, _objectAlignmentInBytes(env->getObjectAlignmentInBytes())
		, _extensions(env->getExtensions())
		, _dispatcher((MM_ParallelDispatcher *)_extensions->dispatcher)
		, _tenureMask(0)
		, _activeSubSpace(NULL)
		, _evacuateMemorySubSpace(NULL)
		, _survivorMemorySubSpace(NULL)
		, _tenureMemorySubSpace(NULL)
		, _evacuateSpaceBase(NULL)
		, _evacuateSpaceTop(NULL)
		, _survivorSpaceBase(NULL)
		, _survivorSpaceTop(NULL)
		, _minimumCopyspaceSize(OMR_MAX(_extensions->tlhMinimumSize, _extensions->evacuatorMinimumCopyspaceSize))
		, _maximumCopyspaceSize(OMR_MIN(_extensions->tlhMaximumSize, (_minimumCopyspaceSize << 4)))
		, _minimumWorkspaceSize(OMR_MIN((_maximumCopyspaceSize << 1), _extensions->evacuatorMinimumWorkspaceSize))
		, _maximumWorkspaceSize(_maximumCopyspaceSize)
		, _workMutex(NULL)
		, _omrVM(env->getOmrVM())
	{
		_typeId = __FUNCTION__;
		memset(&_aggregateMetrics, 0, sizeof(_aggregateMetrics));
	}

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	volatile uintptr_t *sampleStalledMap() { return _stalledEvacuatorBitmap; }
	volatile uintptr_t *sampleResumingMap() { return _resumingEvacuatorBitmap; }
	uintptr_t sampleEvacuatorFlags() { return _evacuatorFlags; }
	void printMetrics(MM_EnvironmentBase *env);
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
};

#endif /* EVACUATORCONTROLLER_HPP_ */
