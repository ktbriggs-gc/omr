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

#ifndef EVACUATOR_HPP_
#define EVACUATOR_HPP_

#include "omr.h"
#include "omrcfg.h"
#include "omrmodroncore.h"
#include "omrthread.h"

#include "EnvironmentStandard.hpp"
#include "EvacuatorBase.hpp"
#include "EvacuatorCopyspace.hpp"
#include "EvacuatorDelegate.hpp"
#include "EvacuatorWorklist.hpp"
#include "EvacuatorScanspace.hpp"
#include "EvacuatorWhitelist.hpp"
#include "ForwardedHeader.hpp"
#include "GCExtensionsBase.hpp"
#include "ObjectModel.hpp"
#include "ParallelTask.hpp"
#include "SlotObject.hpp"

class GC_ObjectScanner;
class GC_SlotObject;
class MM_EvacuatorController;

class MM_Evacuator : public MM_EvacuatorBase
{
/*
 * Data members
 */
private:
	/* at least 3 stack frames are required -- bottom frame and a parking frame per region to hold whitespace when bottom frame is cleared */
	MM_EnvironmentStandard *_env;					/* collecting thread environment (this thread) */
	MM_EvacuatorController * const _controller;		/* controller provides collective services and instrumentation */
	MM_EvacuatorDelegate _delegate;					/* implements methods the evacuator delegates to the language/runtime */
	GC_ObjectModel * const _objectModel;			/* object model for language */
	MM_Forge * const _forge;						/* system memory allocator */
	const uintptr_t _maxStackDepth;					/* number of frames to use for scanning (1 implies breadth-first operation) */
	const uintptr_t _maxInsideCopySize;				/* limit on size of object that can be copied inside stack frames */
	const uintptr_t _maxInsideCopyDistance;			/* limit on distance from scan to copy head for copying inside stack frames */
	const uintptr_t _workerIndex;					/* controller's index of this evacuator */
	omrthread_monitor_t	_mutex;						/* controls access to evacuator worklist */
	uintptr_t _copiedBytesDelta[2];					/* cumulative number of bytes copied out of evacuation semispace since last report */
	uintptr_t _scannedBytesDelta;					/* cumulative number of bytes scanned in survivor semispace or tenure space since last report */
	uintptr_t _copiedBytesReportingDelta;			/* copied bytes increment for reporting copied/scanned byte counts to controller */
	uintptr_t _workspaceReleaseThreshold;			/* threshold for releasing accumulated workspace from outside copyspaces */
	uintptr_t _tenureMask;							/* used to determine age threshold for tenuring evacuated objects */
	MM_ScavengerStats *_stats;						/* pointer to MM_EnvironmentBase::_scavengerStats */

	MM_EvacuatorScanspace * const _stackBottom;		/* bottom (location) of scan stack */
	const MM_EvacuatorScanspace * const _stackTop;	/* physical limit determines number of frames allocated for scan stack */
	const MM_EvacuatorScanspace * const _stackCeiling;	/* normative limit determines maximal depth of scan stack (may be below top) */
	const MM_EvacuatorScanspace * _stackLimit;		/* operational limit is set to ceiling for full stack scanning or bottom to force flushing to outside copyspaces */
	MM_EvacuatorScanspace * _scanStackFrame;		/* points to active stack frame, NULL if scan stack empty */
	MM_EvacuatorScanspace * _whiteStackFrame[2];	/* pointers to stack frames that hold reserved survivor/tenure inside whitespace */

	MM_EvacuatorCopyspace * const _copyspace;		/* pointers to survivor/tenure outside copyspaces that receive outside copy */
	MM_EvacuatorWhitelist * const _whiteList;		/* pointers to survivor/tenure whitelists of whitespace fragments */

	MM_EvacuatorCopyspace _largeCopyspace;			/* copyspace for large objects (solo object allocation and distribution as workspace) */
	MM_EvacuatorFreelist _freeList;					/* LIFO queue of empty workspaces */
	MM_EvacuatorWorklist _workList;					/* FIFO queue of distributable survivor/tenure workspaces */

	uintptr_t _copyspaceOverflow[2];				/* volume of copy overflowing outside copyspaces, reset when outside copyspace is refreshed */
	uint8_t *_heapBounds[3][2];						/* lower and upper bounds for nursery semispaces and tenure space */

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	uintptr_t _conditionCounts[conditions_mask + 1];/* histogram counters for condition flags, indexed by 8-bit pattern of flags */
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

	bool _abortedCycle;								/* set when work is aborted by any evacuator task */
protected:
public:

/*
 * Function members
 */
private:
	/* evacuation workflow: evacuate -> survivor | tenure */
	void scanRoots();
	void scanRemembered();
	void scanHeap();
	bool scanClearable();
	void scanComplete();

	/* scan workflow (huff): (copyspace | worklist:workspace* ) -> stack:scanspace* */
	MMINLINE bool getWork();
	MMINLINE void findWork();

	MMINLINE void clear();
	MMINLINE void pull(MM_EvacuatorWorklist *worklist);
	MMINLINE void pull(MM_EvacuatorCopyspace *copyspace);

	void scan();
	MMINLINE GC_ObjectScanner *scanner(const bool advanceScanHead = false);
	MMINLINE void chain(omrobjectptr_t linkedObject, const uintptr_t selfReferencingSlotOffset, const uintptr_t worklistVolumeCeiling);
	MMINLINE MM_EvacuatorScanspace *top();
	MMINLINE MM_EvacuatorScanspace *next(MM_EvacuatorScanspace *nextFrame, const Region region);
	MMINLINE void push(MM_EvacuatorScanspace * const nextFrame);
	MMINLINE void copy();
	MMINLINE void pop();

	MMINLINE bool reserveInsideScanspace(const Region region, const uintptr_t slotObjectSizeAfterCopy);
	MMINLINE MM_EvacuatorWhitespace *refreshInsideWhitespace(const Region region, const uintptr_t slotObjectSizeAfterCopy);
	MMINLINE omrobjectptr_t copyForward(MM_ForwardedHeader *forwardedHeader, fomrobject_t *referringSlotAddress, MM_EvacuatorCopyspace * const copyspace, const uintptr_t originalLength, const uintptr_t forwardedLength);

	omrobjectptr_t copyOutside(Region region, MM_ForwardedHeader *forwardedHeader, fomrobject_t *referringSlotAddress, const uintptr_t slotObjectSizeBeforeCopy, const uintptr_t slotObjectSizeAfterCopy, const bool isSplitableArray);
	MMINLINE MM_EvacuatorCopyspace *reserveOutsideCopyspace(Region *region, const uintptr_t slotObjectSizeAfterCopy, bool useLargeCopyspace);
	MMINLINE bool shouldRefreshCopyspace(const Region region, const uintptr_t slotObjectSizeAfterCopy, const uintptr_t copyspaceRemainder) const;

	/* copy workflow (puff): stack:scanspace* -> copyspace -> worklist:workspace* */
	MMINLINE uintptr_t adjustWorkReleaseThreshold();
	MMINLINE void addWork(MM_EvacuatorCopyspace *copyspace);
	MMINLINE void splitPointerArrayWork(omrobjectptr_t pointerArray);
	MMINLINE bool isSplitArrayWorkspace(const MM_EvacuatorWorkspace *work) const;
	MMINLINE void flushForWaitState();
	MMINLINE void gotWork();

	/* workflow conditions regulate switching copy between inside/outside scan/copyspaces, inside copy distance, workspace release threshold, ... */
	MMINLINE void setCondition(ConditionFlag condition, bool value);
	MMINLINE ConditionFlag copyspaceTailFillCondition(Region region) const;
	MMINLINE bool isForceOutsideCopyCondition(Region region) const;
	MMINLINE bool isForceOutsideCopyCondition() const;
	MMINLINE bool isDistributeWorkCondition() const;
	MMINLINE bool isBreadthFirstCondition() const;

	/* object geometry */
	MMINLINE bool isSplitablePointerArray(MM_ForwardedHeader *forwardedHeader, uintptr_t objectSizeInBytes);
	MMINLINE bool isLargeObject(const uintptr_t objectSizeInBytes) const;
	MMINLINE bool isHugeObject(const uintptr_t objectSizeInBytes) const;

	/* object age */
	MMINLINE void flushRememberedSet();
	MMINLINE bool isNurseryAge(uintptr_t objectAge) const;
	MMINLINE bool setRememberedState(omrobjectptr_t object, uintptr_t rememberedState);

	/* generational cycle */
	MMINLINE void setAbortedCycle();
	MMINLINE bool isAbortedCycle();


#if defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
#if defined(EVACUATOR_DEBUG)
	MMINLINE void debugStack(const char *stackOp, bool treatAsWork = false);
#endif /* defined(EVACUATOR_DEBUG) */
	MMINLINE uint64_t startWaitTimer(const char *tag);
	MMINLINE void endWaitTimer(uint64_t waitStartTime);
	MMINLINE uint64_t cycleMicros() { OMRPORT_ACCESS_FROM_ENVIRONMENT(_env); return omrtime_hires_delta(_env->getExtensions()->incrementScavengerStats._startTime, omrtime_hires_clock(), OMRPORT_TIME_DELTA_IN_MICROSECONDS); }
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

protected:
public:
	virtual UDATA getVMStateID() { return OMRVMSTATE_GC_EVACUATOR; }

	/**
	 * Instantiate evacuator.
	 *
	 * @param workerIndex the controller's index binding evacuator to controller
	 * @param controller the evacuation controller (collector)
	 * @param extensions gc extensions (base)
	 * @return an evacuator instance
	 */
	static MM_Evacuator *newInstance(uintptr_t workerIndex, MM_EvacuatorController *controller, MM_GCExtensionsBase *extensions);

	/**
	 * Terminate and deallocate evacuator instance
	 */
	void kill();

	/**
	 * Per instance evacuator initialization
	 */
	bool initialize();

	/**
	 * Per instance evacuator finalization
	 */
	void tearDown();

	/**
	 * Per gc, bind evacuator instance to worker thread and set up evacuator environment, clear evacuator gc stats
	 *
	 * @param[in] env worker thread environment to bind to
	 * @param[in] tenureMask a copy of the controller's tenure mask for the cycle
	 * @param[in] heapBounds address bounds for heap regions survivor, tenure, evacuate
	 * @param[in] copiedBytesReportingDelta the number of scanned/copied bytes to process between reporting scan/copy counts to controller
	 */
	void bindWorkerThread(MM_EnvironmentStandard *env, uintptr_t tenureMask, uint8_t *heapBounds[][2], uintptr_t copiedBytesReportingDelta);

	/**
	 * Per gc, unbind evacuator instance from worker thread, merge evacuator gc stats
	 *
	 * @param[in] env worker thread environment to unbind from
	 */
	void unbindWorkerThread(MM_EnvironmentStandard *env);

	/**
	 * Bound artifacts
	 */
	uintptr_t getWorkerIndex() const { return _workerIndex; }
	MM_EnvironmentStandard *getEnvironment() const { return _env; }
	MM_EvacuatorDelegate *getDelegate() { return &_delegate; }

	/**
	 * Work status
	 */
	bool hasScanWork() const { return (NULL != _scanStackFrame); }
	uintptr_t getVolumeOfWork() { return _workList.volume(); }
	uintptr_t getDistributableVolumeOfWork(uintptr_t workReleaseThreshold);

	bool isInEvacuate(void *address) const { return (_heapBounds[evacuate][0] <= (uint8_t *)address) && ((uint8_t *)address < _heapBounds[evacuate][1]); }
	bool isInSurvivor(void *address) const { return (_heapBounds[survivor][0] <= (uint8_t *)address) && ((uint8_t *)address < _heapBounds[survivor][1]); }
	bool isInTenure(void *address) const { return _env->getExtensions()->isOld((omrobjectptr_t)address); }

	/**
	 * Get the heap region (survivor|tenure|evacuate) containing an address
	 *
	 * @param address a putative heap address
	 * @return heap region or unreachable if address not in heap
	 */
	Region
	getEvacuationRegion(void *address) const
	{
		if (isInSurvivor(address)) {
			return survivor;
		}
		if (isInTenure(address)) {
			return tenure;
		}
		if (isInEvacuate(address)) {
			return evacuate;
		}
		return unreachable;
	}

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

	/**
	 * Main evacuation method driven by all gc slave threads during a nursery collection:
	 *
	 *  (scanRemembered scanRoots (scanHeap scanComplete)) (scanClearable (scanHeap scanComplete))*
	 *
	 * Root set scanning is initiated by evacuator, driven by evacuator delegate:
	 *
	 * 	((evacuateRootObject)* (evacuateThreadSlot)*)*
	 *
	 * Remembered set scanning is driven by the controller:
	 *
	 * 	(evacuateRememberedObject)*
	 *
	 * Remembered set pruning is driven by the controller:
	 *
	 * 	(shouldRememberObject (rememberObject)?)*
	 *
	 * Evacuation of clearable objects is driven by the evacuator delegate in stages and each stage
	 * should be conducted as for root objects, ie simply evacuate each clearable object:
	 *
	 * 	(evacuateRootObject)*
	 *
	 * At each stage the evacuator scans the heap and completes after the delegate has evacuated all
	 * clearable roots. The j9 java evacuator delegate uses a deprecated (legacy) calling pattern
	 * dictated by MM_RootScanner protocol:
	 *
	 * 	(evacuateRootObject* evacuateHeap)
	 *
	 * The evacuateHeap method is provided for this context only and its use is deprecated. Evacuator
	 * is designed to deal with a stream of pointers to root objects presented via evacuateRootObject
	 * in the delegate's scanClearable implementation. When scanClearable completes, the evacuator
	 * will recursively scan the objects depending from the roots presented in scanClearable.
	 *
	 * @param[in] env worker thread environment
	 *
	 * @see MM_ParallelScavengeTask::run(MM_EnvironmentBase *)
	 */
	void workThreadGarbageCollect(MM_EnvironmentStandard *env);

	/**
	 * Copy and forward root object given address of referring slot
	 *
	 * @param slotPtr address of referring slot
	 * @param breadthFirst copy object without recursing into dependent referents
	 * @return true if the root object was copied to new space (not tenured), false otherwise
	 */
	bool
	evacuateRootObject(volatile omrobjectptr_t *slotPtr, bool breadthFirst = false)
	{
		omrobjectptr_t object = *slotPtr;
		if (isInEvacuate(object)) {
			/* slot object must be evacuated -- determine before and after object size */
			MM_ForwardedHeader forwardedHeader(object, _env->compressObjectReferences());
			object = evacuateRootObject(&forwardedHeader, breadthFirst);
			Debug_MM_true(NULL != object);
			*slotPtr = object;
		}
		/* failure to evacuate must be reported as object in survivor space to maintain remembered set integrity */
		return isInSurvivor(object) || isInEvacuate(object);
	}

	/**
	 * Copy and forward root object given slot object encapsulating address of referring slot
	 *
	 * @param slotObject pointer to slot object encapsulating address of referring slot
	 * @param breadthFirst copy object without recursing into dependent referents
	 * @return true if the root object was copied to new space (not tenured), false otherwise
	 */
	bool
	evacuateRootObject(GC_SlotObject* slotObject, bool breadthFirst = false)
	{
		omrobjectptr_t object = slotObject->readReferenceFromSlot();
		if (isInEvacuate(object)) {
			/* slot object must be evacuated -- determine before and after object size */
			MM_ForwardedHeader forwardedHeader(object, _env->compressObjectReferences());
			object = evacuateRootObject(&forwardedHeader, breadthFirst);
			Debug_MM_true(NULL != object);
			slotObject->writeReferenceToSlot(object);
		}
		/* failure to evacuate must be reported as object in survivor space to maintain remembered set integrity */
		return isInSurvivor(object) || isInEvacuate(object);
	}

	/**
	 * Copy and forward root object given a forwarding header obtained from the object
	 *
	 * @param forwardedHeader pointer to forwarding header obtained from the object
	 * @param breadthFirst copy object without recursing into dependent referents
	 * @return address in survivor or tenure space that object was forwarded to
	 */
	omrobjectptr_t evacuateRootObject(MM_ForwardedHeader *forwardedHeader, bool breadthFirst = false);

	/**
	 * Evacuate all objects in evacuate space referenced by an object in the remembered set
	 *
	 * @param objectptr the remembered object, in tenure space
	 * @return true if the remembered object contained any evacuated referents
	 */
	bool evacuateRememberedObject(omrobjectptr_t objectptr);

	/**
	 * Test tenured object for containment of referents in survivor space. This method should not be
	 * called until after evacuation is complete.
	 */
	bool shouldRememberObject(omrobjectptr_t objectPtr);

	/**
	 * Copy and forward root object from mutator stack slot given address of referring slot.
	 *
	 * NOTE: the object will be copied and forwarded here but the indirect pointer parameter
	 * update may be deferred if forwarded to tenure space. In that case the indirect pointer
	 * will be updated after recursive heap scanning is complete, when the delegate rescans
	 * thread slots.
	 *
	 * @param objectPtrIndirect address of referring slot
	 * @see MM_EvacuatorDelegate::rescanThreadSlots()
	 * @see rescanThreadSlot(omrobjectptr_t)
	 */
	void evacuateThreadSlot(volatile omrobjectptr_t *objectPtrIndirect);

	/**
	 * Update a thread slot holding a pointer to an object that was evacuated into tenure space
	 * in the current nursery collection. These updates are deferred from evacuateThreadSlot()
	 * to obviate the need for an internal write barrier.
	 *
	 * @param objectPtrIndirect address of referring slot
	 */
	void rescanThreadSlot(omrobjectptr_t *objectPtrIndirect);

	/**
	 * Copy and forward all objects in evacuation space depending from clearable objects copied
	 * during a clearing stage.
	 *
	 * @return true unless gc cycle is aborting
	 */
	bool evacuateHeap();

	/**
	 * Controller calls this when it allocates a TLH from survivor or tenure region that is too small to hold
	 * the current object. The evacuator adds the unused TLH to the whitelist for the containing region.
	 */
	void receiveWhitespace(MM_EvacuatorWhitespace *whitespace);

	/**
	 * Controller calls this to force evacuator to flush unused whitespace from survivor or tenure whitelist.
	 */
	void flushWhitespace(Region region);

	/**
	 * Get the number of allocated bytes discarded during the gc cycle (micro-fragmentation).
	 */
	uintptr_t getDiscarded() { return _whiteList[survivor].getDiscarded() + _whiteList[tenure].getDiscarded(); }

	/**
	 * Get the number of allocated bytes flushed at the end of the gc cycle (macro-fragmentation).
	 */
	uintptr_t getFlushed() { return _whiteList[survivor].getFlushed() + _whiteList[tenure].getFlushed(); }

	/**
	 * Get the number of allocated bytes flushed at the end of the gc cycle (macro-fragmentation).
	 */
	uintptr_t getRecycled() { return _whiteList[survivor].getRecycled() + _whiteList[tenure].getRecycled(); }

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	uintptr_t getStackActivationCount(uintptr_t depth) { return _stackBottom[depth].getActivationCount(); }
	const uintptr_t *getConditionCounts() { return (const uintptr_t *)_conditionCounts; }
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

#if defined(EVACUATOR_DEBUG)
	void checkSurvivor();
	void checkTenure();
#endif /* defined(EVACUATOR_DEBUG) */

	/**
	 * Constructor. The minimum number of stack frames is three - two to hold whitespace and one or more
	 * for scanning over full stack range up to maxStackDepth. Set maxStackDepth=1 for breadth first scanning
	 * and maxInsideCopyDistance=0 for depth first scanning.
	 *
	 * @param workerIndex worker thread index assigned by controller
	 * @param controller the controller
	 * @param extensions the global GC extensions
	 */
	MM_Evacuator(uintptr_t workerIndex, MM_EvacuatorController *controller, MM_GCExtensionsBase *extensions)
		: MM_EvacuatorBase(extensions)
		, _env(NULL)
		, _controller(controller)
		, _delegate()
		, _objectModel(&_extensions->objectModel)
		, _forge(_extensions->getForge())
		, _maxStackDepth(!isScanOptionSelected(extensions, breadth_first_always) ? extensions->evacuatorMaximumStackDepth : 1)
		, _maxInsideCopySize(_objectModel->adjustSizeInBytes(extensions->evacuatorMaximumInsideCopySize))
		, _maxInsideCopyDistance(_objectModel->adjustSizeInBytes(extensions->evacuatorMaximumInsideCopyDistance))
		, _workerIndex(workerIndex)
		, _mutex(NULL)
		, _scannedBytesDelta(0)
		, _copiedBytesReportingDelta(0)
		, _workspaceReleaseThreshold(0)
		, _tenureMask(0)
		, _stats(NULL)
		, _stackBottom(MM_EvacuatorScanspace::newInstanceArray(this, _forge, OMR_MAX(_maxStackDepth, unreachable)))
		, _stackTop(_stackBottom + OMR_MAX(_maxStackDepth, unreachable))
		, _stackCeiling(_stackBottom + _maxStackDepth)
		, _stackLimit(_stackCeiling)
		, _scanStackFrame(NULL)
		, _copyspace(MM_EvacuatorCopyspace::newInstanceArray((MM_EvacuatorBase *)this, _forge, evacuate))
		, _whiteList(MM_EvacuatorWhitelist::newInstanceArray((MM_EvacuatorBase *)this, _forge, evacuate))
		, _largeCopyspace(this)
		, _freeList(_forge)
		, _workList(this, &_freeList)
		, _abortedCycle(false)
	{
		_typeId = __FUNCTION__;

		_copiedBytesDelta[survivor] = _copiedBytesDelta[tenure] = 0;
		_copyspaceOverflow[survivor] = _copyspaceOverflow[tenure] = 0;
		_whiteStackFrame[survivor] = _whiteStackFrame[tenure] = NULL;

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		memset(_conditionCounts, 0, sizeof(_conditionCounts));
		Debug_MM_true(0 == (_objectModel->getObjectAlignmentInBytes() % sizeof(uintptr_t)));
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

		Assert_MM_true((NULL != _stackBottom) && (NULL != _copyspace) && (NULL != _whiteList));
	}

	friend class MM_EvacuatorController;
	friend class MM_ScavengerRootClearer;
};

#endif /* EVACUATOR_HPP_ */
