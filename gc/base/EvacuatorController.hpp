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
#include "EvacuatorHistory.hpp"
#include "EvacuatorParallelTask.hpp"
#include "GCExtensionsBase.hpp"
#include "Heap.hpp"
#include "ParallelDispatcher.hpp"

class GC_ObjectScanner;
class MM_EvacuatorWhitespace;
class MM_MemorySubSpace;
class MM_MemorySubSpaceSemiSpace;

/**
 * The controller periodically sums copied/scanned byte counts over all evacuators to obtain epochal
 * information about progress of evacuation and asserts if the aggregate volume of material copied is
 * not equal to the aggregate volume scanned at the the of each evacuation stage (roots, clearable, ..)
 * and at the end of the generational collection cycle.
 *
 * All sizes and lengths are expressed in bytes in controller, evacuator, white/copy/scanspace contexts.
 */
class MM_EvacuatorController : public MM_Collector
{
/**
 * Data members
 */
private:
	/* constants for mapping evacuator index to bitmap word and bit offset */
	static const uintptr_t index_to_map_word_shift = ((OMR_LOG_POINTER_SIZE == 3) ? 6 : 5);
	static const uintptr_t index_to_map_word_modulus = (((uintptr_t)1 << index_to_map_word_shift) - 1);

	typedef MM_Evacuator *EvacuatorPointer;

	const uintptr_t _maxGCThreads;						/* fixed for life of vm, never <_evacuatorCount */
	EvacuatorPointer * const _evacuatorTask;			/* array of pointers to instantiated evacuators */
	omrthread_monitor_t	_controllerMutex;				/* synchronize evacuator work distribution and end of scan cycle */
	omrthread_monitor_t	_reporterMutex;					/* synchronize collection of epochal records within gc cycle */
	volatile uintptr_t _evacuatorCount;					/* number of gc threads that will participate in collection */
	volatile uintptr_t * const _boundEvacuatorBitmap;	/* maps evacuator threads that have been dispatched and bound to an evacuator instance */
	volatile uintptr_t * const _stalledEvacuatorBitmap;	/* maps evacuator threads that are stalled (waiting for work) */
	volatile uintptr_t * const _resumingEvacuatorBitmap;/* maps evacuator threads that are resuming or completing scan cycle after stalling */
	volatile uintptr_t * const _evacuatorMask;			/* maps all evacuator threads, bound or unbound */
	volatile uintptr_t _isNotifyOfWorkPending;			/* non-zero if an evacuator is stalled and notification of work is pending */
	volatile uintptr_t _stalledEvacuatorCount;			/* number of stalled evacuator threads */
	volatile uintptr_t _evacuatorIndex;					/* number of GC threads that have joined the evacuation so far */
	volatile uintptr_t _evacuatorFlags;					/* private and public (language defined) evacuation flags shared among evauators */
	volatile uintptr_t _copyspaceAllocationCeiling[2];	/* upper bounds for copyspace allocation in survivor, tenure regions */
	volatile uintptr_t _objectAllocationCeiling[2];		/* upper bounds for object allocation in survivor, tenure regions */
	uintptr_t _copiedBytesReportingDelta;				/* delta copied/scanned byte count per evacuator for triggering evacuator progress report to controller */
	uintptr_t _bytesPerReportingEpoch;					/* number of bytes copied per reporting epoch */

	MM_MemorySubSpace *_memorySubspace[3];				/* pointer to memory subspace per heap region */

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	MM_EvacuatorHistory _history;						/* epochal record per gc cycle */
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

protected:

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	uint64_t _collectorStartTime;						/* collector startup time */
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

	const uintptr_t _objectAlignmentInBytes;			/* cached object alignment from GC_ObjectModelBase */
	volatile uintptr_t _aggregateVolumeMetrics[MM_Evacuator::metrics];/* running total aggregate volume of copy accumulated in current gc cycle */
	volatile uintptr_t _finalDiscardedBytes;			/* sum of final whitespace bytes discarded during gc cycle for all evacuators */
	volatile uintptr_t _finalFlushedBytes;				/* sum of final whitespace bytes flushed at end of gc cycle for all evacuators */
	volatile uintptr_t _finalRecycledBytes;				/* sum of final whitespace bytes recycled at end of gc cycle for all evacuators */
	uintptr_t _finalEvacuatedBytes;						/* total number of bytes evacuated to survivor/tenure regions during gc cycle */
	uintptr_t _modalSurvivorVolumeMetric;				/* back-weighted running average of agggreagte survivor bytes copied last generation */

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
	uint8_t *_heapLayout[3][2];

	/* hard lower and upper bounds for whitespace allocation are bound to tlh size */
	const uintptr_t _maximumCopyspaceSize;
	const uintptr_t _minimumCopyspaceSize;

	/* hard lower and upper bounds for workspace size, evacuator lower bound can be less than controller's while flushing outside */
	const uintptr_t _minimumWorkspaceSize;
	const uintptr_t _maximumWorkspaceSize;

	/* multiplier for _minimumWorkspaceSize to determine evacuator work quota */
	const uintptr_t _minimumWorkQuanta;

	OMR_VM *_omrVM;

/**
 * Function members
 */
private:
	/* calculate a rough overestimate of the amount of matter that will be evacuated to survivor or tenure in current cycle */
	MMINLINE uintptr_t  calculateProjectedEvacuationBytes() const;

	/* update evacuation history at end of a reporting epoch */
	MMINLINE void reportProgress(MM_Evacuator *worker, uintptr_t baseScannedMetric, uintptr_t *sampledVolumeMetrics);

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
			if (nextIndex >= _evacuatorIndex) {
				nextIndex = 0;
			}

		} while ((nextIndex != evacuator->getWorkerIndex()) && !isBoundEvacuator(nextIndex));

		return _evacuatorTask[nextIndex];
	}

	/**
	 * Parallel task wrapper calls this to bind worker thread to an evacuator instance at the beginning of a gc cycle.
	 *
	 * @param env the environment for the worker thread
	 * @return a pointer to the evacuator that is bound to the worker thread
	 */
	MM_Evacuator *bindWorker(MM_EnvironmentStandard *env);

	/**
	 * Parallel task wrapper calls this to unbind worker thread to an evacuator instance at the end of a gc cycle.
	 *
	 * @param env the environment for the worker thread
	 */
	void unbindWorker(MM_EnvironmentStandard *env);

	/**
	 * Evacuators acquire/release exclusive controller access to notify stalled evacuators of work or to poll/pull work from running evacuators
	 */
	void acquireController() { omrthread_monitor_enter(_controllerMutex); }

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
	bool isWaitingForWork(MM_Evacuator *worker);

	/**
	 * Atomically test pending notification flag.
	 *
	 * @return true if there is a notification request pending.
	 */
	bool isNotififyOfWorkPending() { return 1 == VM_AtomicSupport::lockCompareExchange(&_isNotifyOfWorkPending, 1, 1); }

	/*
	 * Evacuators with distributable work will notify controller at the instant that a stall condition is
	 * raised. If >1 evacuators have distributable work all should call this method but only one will
	 * actually acquire the controller to post the notification.
	 *
	 * Caller must not acquire/release the controller before/after the call
	 *
	 * @param worker the evacuator that is notifying
	 */
	void notifyOfWork(MM_Evacuator *evacuator);

	void releaseController() { omrthread_monitor_exit(_controllerMutex); }

	/**
	 * Evacuator has fulfilled work quota when it's volume of distributable work is greater than the a threshold
	 * number of work quanta. Quantum size is volume of a minimal workspace (default is TLH minimum size). Running
	 * evacuators flush material to outside copyspaces to generate distributable workspaces as long as 1 or more
	 * evacuators are stalled. Flushing continues until the stall condition clears and work quota is fulfilled.
	 *
	 * @param workQuantumVolume the volume of a quantum of work (use 1 to return minimal number of workspaces required to fulfill)
	 * @return volume of work required to fulfill quota
	 */
	uintptr_t getWorkNotificationQuota(uintptr_t workQuantumVolume = 1) const { return (workQuantumVolume * _minimumWorkQuanta); }

	/**
	 * Test evacuator worklist volume against work notification threshold.
	 *
	 * @param workQuantumVolume the volume of a quantum of work
	 * @param volumeOfWork the volume of work on the evacuator's worklist
	 * @return true if evacuator's volume of work is greater than quota
	 */
	bool hasFulfilledWorkQuota(uintptr_t workQuantumVolume, uintptr_t volumeOfWork) const { return (volumeOfWork > getWorkNotificationQuota(workQuantumVolume)); }

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
	bool hasCompletedScan() const { return ((_aggregateVolumeMetrics[MM_Evacuator::survivor] + _aggregateVolumeMetrics[MM_Evacuator::tenure]) == _aggregateVolumeMetrics[MM_Evacuator::scanned]); }

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
	 * Evacuator calls this to get free space for refreshing stack scanspaces and outside copyspaces.
	 *
	 * @param worker the calling evacuator
	 * @param region the region (survivor or tenure) to obtain free space from
	 * @param length the (minimum) number of bytes of free space required, a larger chunk may be allocated at controller discretion
	 * @return a pointer to space allocated, which may be larger that the requested length, or NULL
	 */
	MM_EvacuatorWhitespace *getWhitespace(MM_Evacuator *worker, MM_Evacuator::Region region, uintptr_t length);

	/**
	 * Evacuator calls this to get free space for solo objects.
	 *
	 * @param worker the calling evacuator
	 * @param region the region (survivor or tenure) to obtain free space from
	 * @param length the (exact) number of bytes of free space required
	 * @return a pointer to space allocated, which will be the requested length, or NULL
	 */
	MM_EvacuatorWhitespace *getObjectWhitespace(MM_Evacuator *worker, MM_Evacuator::Region region, uintptr_t length);

	/**
	 * Evacuator periodically reports scanning/copying progress to controller. Period is determined by
	 * bytes scanned delta set by controller. Last running (not stalled) reporting thread may end reporting
	 * epoch if not timesliced while accumulating summary scanned and copied byte counts across all
	 * evacuator threads.
	 *
	 * @param worker the reporting evacuator
	 * @param baseVolumeMetrics evacuator volume metrics as of last report, which will be reset to 0
	 * @param currentVolumeMetrics evacuator volume metrics as of now
	 * @return 0
	 */
	uintptr_t reportProgress(MM_Evacuator *worker,  uintptr_t *baseVolumeMetrics, uintptr_t *currentVolumeMetrics);

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
		, _controllerMutex(NULL)
		, _reporterMutex(NULL)
		, _evacuatorCount(0)
		, _boundEvacuatorBitmap(allocateEvacuatorBitmap(env, _maxGCThreads))
		, _stalledEvacuatorBitmap(allocateEvacuatorBitmap(env, _maxGCThreads))
		, _resumingEvacuatorBitmap(allocateEvacuatorBitmap(env, _maxGCThreads))
		, _evacuatorMask(allocateEvacuatorBitmap(env, _maxGCThreads))
		, _isNotifyOfWorkPending(0)
		, _stalledEvacuatorCount(0)
		, _evacuatorIndex(0)
		, _evacuatorFlags(0)
		, _copiedBytesReportingDelta(0)
		, _bytesPerReportingEpoch(0)
#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		, _collectorStartTime(0)
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
		, _objectAlignmentInBytes(env->getObjectAlignmentInBytes())
		, _finalDiscardedBytes(0)
		, _finalFlushedBytes(0)
		, _finalRecycledBytes(0)
		, _finalEvacuatedBytes(0)
		, _modalSurvivorVolumeMetric(0)
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
		, _maximumCopyspaceSize(_extensions->tlhMaximumSize)
		, _minimumCopyspaceSize(_maximumCopyspaceSize >> 4)
		, _minimumWorkspaceSize(_extensions->objectModel.adjustSizeInBytes(_extensions->evacuatorWorkQuantumSize))
		, _maximumWorkspaceSize(_minimumWorkspaceSize << 4)
		, _minimumWorkQuanta(_extensions->evacuatorWorkQuanta)
		, _omrVM(env->getOmrVM())
	{
		_typeId = __FUNCTION__;
		for (intptr_t metric = MM_Evacuator::survivor_copy; metric < MM_Evacuator::metrics; metric += 1) {
			_aggregateVolumeMetrics[metric] = 0;
		}
	}

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	void reportConditionCounts(uintptr_t gc, uintptr_t epoch);
	void reportCollectionStats(MM_EnvironmentBase *env);
	uintptr_t sampleEvacuatorFlags() { return _evacuatorFlags; }
	volatile uintptr_t *sampleStalledMap() { return _stalledEvacuatorBitmap; }
	volatile uintptr_t *sampleResumingMap() { return _resumingEvacuatorBitmap; }
	void printEvacuatorBitmap(MM_EnvironmentBase *env, const char *label, volatile uintptr_t *bitmap);
	void waitToSynchronize(MM_Evacuator *worker, const char *id);
	void continueAfterSynchronizing(MM_Evacuator *worker, uint64_t startTime, uint64_t endTime, const char *id);
	const MM_EvacuatorHistory::Epoch *getEpoch() { return _history.getEpoch(); }
	uintptr_t sumStackActivations(uintptr_t *stackActivations, uintptr_t maxFrame);
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
};

#endif /* EVACUATORCONTROLLER_HPP_ */
