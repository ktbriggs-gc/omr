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

#include "EvacuatorBase.hpp"
#include "EvacuatorDelegate.hpp"
#include "EvacuatorWorklist.hpp"
#include "EvacuatorWhitelist.hpp"
#include "ForwardedHeader.hpp"
#include "ObjectScanner.hpp"
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
	/* space for _object scanner variant instantiation */
	typedef MM_EvacuatorDelegate::ScannerState Scanner;

	/* scan stack frame -- workspaces are always pulled into bottom stack frames, and split array segments are never pushed from an inferior frame */
	typedef struct ScanSpace {
		uint8_t *_base;			/* base points to first object contained in frame (or array object if pulled from split array workspace) */
		union {
			omrobjectptr_t _object;	/* pointer to object is automorphic with scan head, advancing in mixed (pulled) and pushed frames */
			uint8_t *_scan;			/* scan head always points to array object while scanning in frames pulled from split array workspaces */
		};
		uint8_t *_end;			/* frame is popped when _scan advances to _end (*_end is void) */
		uintptr_t _flags;		/* ScanFlags */
		Scanner _scanner; 		/* object scanner instance for _object at _scan (GC_LeafObjectScanner if isNull flag set) */
	} StackFrame;

	/* scanspace flags */
	typedef enum ScanFlags {
		  scanTenure = tenure	/* scanning tenure copy (otherwise scanning survivor copy) */
		, isRemembered = 2		/* remembered state of object at scan head */
		, isPulled = 4			/* workspace was pulled from worklist (workspaces are pulled bottom up and have closed endpoints)*/
		, isSplitArray = 8		/* pulled split array workspace */
		, isNull = 16			/* marks the static empty frame in _nil that is used to obviate NULL checks on _sp, _rp */
	} ScanFlags;

	/* copy spaces hold copied material pending scan */
	typedef struct CopySpace {
		uint8_t *_base;			/* base address of scan work laid down by copy since last rebase or whitespace if just refreshed */
		uint8_t *_copy;			/* copy head points to where next object will be copied (*_copy is whitespace unless void ) */
		uint8_t *_end;			/* end marks the end of remaining whitespace (*_end is void) */
		uintptr_t _flags;		/* CopyFlags */
	} CopySpace;

	/* copyspace flags */
	typedef enum CopyFlags {
		  copyTenure = tenure	/* holds copy in tenure space (otherwise holds survivor copy) */
		, isLOA = 2				/* set if whitespace allocated from LOA */
		, isLeaves = 4			/* set if copyspace holds only leaf objects */
	} CopyFlags;

	/* enumeration of _copyspace[] array indices (inside/outside survivor/tenure and overflow copyspaces) */
	typedef enum CopySpaces {
		  insideSurvivor = survivor			/* [0] holds small objects copied inside survivor stack frames */
		, insideTenure = tenure				/* [1] holds small objects copied inside tenure stack frames */
		, outsideSurvivor = 2 + survivor	/* [2] holds larger objects copied to outside survivor copyspace */
		, outsideTenure = 2 + tenure		/* [3] holds larger objects copied to outside tenure copyspace */
		, overflow = 4						/* [4] holds objects that overflow inside and outside copyspaces */
		, copyspaces = 5					/* (5) number of copyspaces in _copyspace[] array */
	} CopySpaces;

	MM_EvacuatorController * const _controller;	/* controller provides collective services and instrumentation */
	const uintptr_t _maxStackDepth;				/* number of frames to use for scanning (1 implies breadth-first operation) */
	const uintptr_t _maxInsideCopyDistance;		/* limit on distance from scan to copy head for copying inside stack frames */
	const uintptr_t _minInsideCopySize;			/* limit on size of object that can be copied inside stack frames */
	const uintptr_t _maxInsideCopySize;			/* limit on size of object that can be copied inside stack frames */
	uintptr_t _workReleaseThreshold;			/* limit for accumulation in outside copyspaces of unscanned copy to worklist */
	uintptr_t _conditionFlags;					/* bitmap of current evacuator operating conditions (including scan options) */
	uintptr_t _insideDistanceMax;				/* effective distance limit may be throttled for some conditions */
	uintptr_t _insideSizeMax;					/* effective size limit may be throttled for some conditions */
	const GC_SlotObject *_slot;					/* pointer to most recently scanned slot (holding reference to evacuation candidate) */
	ScanSpace * const _stack;					/* points to array of scan stack frames */
	ScanSpace * const _nil;						/* points to (nil) last frame in scan stack array (holds const empty frame to obviate NULL checks on _sp) */
	ScanSpace * _sp;							/* points to topmost active stack frame */
	ScanSpace * _rp[sources];					/* point to (may be nil) stack frames scanning inside copyspace per source region (survivor/tenure) */
	uintptr_t _tenureMask;						/* used to determine age threshold for tenuring evacuated objects */
	MM_ScavengerStats *_stats;					/* pointer to MM_EnvironmentBase::_scavengerStats */
	MM_EvacuatorDelegate _delegate;				/* implements methods the evacuator delegates to the language/runtime */

	MM_EvacuatorWorklist _workList;				/* FIFO queue of distributable survivor/tenure workspaces */
	MM_EvacuatorFreelist _freeList;				/* LIFO queue of empty workspaces */
	MM_EvacuatorWhitelist * const _whiteList;	/* array of survivor/tenure whitelists of whitespace fragments */

	uintptr_t _copyspaceOverflow[sources];		/* volume of copy overflowing outside copyspaces, reset when outside copyspace is refreshed */
	CopySpace _copyspace[copyspaces];			/* array of survivor/tenure inside/outside and large copyspaces */
	omrthread_monitor_t	_mutex;					/* controls access to evacuator worklist */
	bool _abortedCycle;							/* set when work is aborted by any evacuator task */

protected:
public:

/*
 * Function members
 */
private:
	static ScanSpace *
	newStackFrameArray(MM_Forge *forge, uintptr_t count)
	{
		uintptr_t stackSize = sizeof(ScanSpace) * count;
		ScanSpace *stack = (ScanSpace *)forge->allocate(stackSize, OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
		if (NULL != stack) {
			memset(stack, 0, stackSize);
		}
		return stack;
	}

	/**
	 * controller sets initial bounds and may update during gc if MM_Collector::collectorExpanded() is
	 * called when an allocation fails and there is headroom in the heap to expand tenure.
	 */
	void setHeapBounds(volatile uint8_t *heapBounds[][2]);

	/* evacuation workflow: evacuate -> survivor | tenure */
	void scanRoots();
	void scanRemembered();
	void scanHeap();
	bool scanClearable();
	void scanComplete();

	/* scan workflow: pull and scan work from worklist|copyspace to find referents in the evacuation region */
	void scan();
	MMINLINE bool next();
	MMINLINE void pull(MM_EvacuatorWorklist *worklist);
	MMINLINE void pull(CopySpace *copyspace);
	MMINLINE void pull(ScanSpace *sp, Region region, const Workspace *workspace);
	MMINLINE void push(ScanSpace *sp, Region region, uint8_t *copyhead, uint8_t *copyend);
	MMINLINE bool pop();
	MMINLINE GC_ObjectScanner *scanner(const ScanSpace *stackframe) const;
	MMINLINE Region source(const ScanSpace *stackframe) const;
	MMINLINE bool pulled(const ScanSpace *stackframe) const;
	MMINLINE bool nil(const ScanSpace *stackframe) const;
	MMINLINE void reset(ScanSpace *stackframe) const;

	/* copy workflow: copy referents in evacuation region into whitespace in survivor/tenure copyspaces, filling copyspaces with scan work */
	omrobjectptr_t copy(MM_ForwardedHeader *forwardedHeader);
	MMINLINE bool cached(const uint8_t *copyhead) const;
	MMINLINE CopySpace *selectCopyspace(Region *selected, uintptr_t sizeAfterCopy, const bool isLeaf);
	MMINLINE omrobjectptr_t copyForward(MM_ForwardedHeader *forwardedHeader, CopySpace *copyspace, const uintptr_t sizeBeforeCopy, const uintptr_t sizeAfterCopy);
	MMINLINE void flushOverflow(Region selected = unreachable);
	MMINLINE uint8_t *rebase(CopySpace *copyspace, uintptr_t *volume);
	MMINLINE bool whitesize(CopySpace *copyspace, uintptr_t size);
	MMINLINE uintptr_t whiteFlags(bool isLoa);
	bool refresh(CopySpace *copyspace, Region region, uintptr_t size, bool isLeaf);
	MMINLINE Whitespace *trim(CopySpace *copyspace);
	MMINLINE void reset(CopySpace *copyspace) const;
	MMINLINE Region source(const CopySpace *copyspace) const;
	MMINLINE CopySpaces index(const CopySpace *copyspace) const;
	MMINLINE CopySpace *inside(Region region);
	MMINLINE CopySpace *outside(Region region);

	/* workspace workflow: rebase copyspaces to extract scan work to worklist to be pulled into stack for scanning */
	MMINLINE bool getWork();
	MMINLINE void findWork();
	MMINLINE void addWork(CopySpace *copyspace);
	void splitPointerArrayWork(CopySpace *copyspace, uint8_t *arrayAddress);
	MMINLINE bool isSplitArrayWorkspace(const Workspace *work) const;
	MMINLINE void flushForWaitState();

	/* workflow conditions regulate switching copy between inside/outside scan/copyspaces, inside copy distance, workspace release threshold, ... */
	MMINLINE bool selectCondition(ConditionFlag condition, bool force = false);
	MMINLINE void setCondition(ConditionFlag condition, bool value);
	MMINLINE bool areConditionsSet(uintptr_t conditionFlags) const;
	MMINLINE bool isConditionSet(uintptr_t conditionFlags) const;
	MMINLINE ConditionFlag copyspaceTailFillCondition(Region region) const;
	MMINLINE bool isForceOutsideCopyCondition(Region region) const;
	MMINLINE bool isDistributeWorkCondition() const;
	MMINLINE bool isBreadthFirstCondition() const;

	/* object geometry */
	MMINLINE bool isSplitablePointerArray(omrobjectptr_t object, uintptr_t objectSizeInBytes);
	MMINLINE bool isLargeObject(const uintptr_t objectSizeInBytes) const;

	/* object age */
	MMINLINE void flushRememberedSet();
	MMINLINE bool isNurseryAge(uintptr_t objectAge) const;
	MMINLINE bool setRememberedState(omrobjectptr_t object, uintptr_t rememberedState);

	/* generational cycle */
	MMINLINE void setAbortedCycle();
	MMINLINE bool isAbortedCycle();

#if defined(EVACUATOR_DEBUG)
	void walk(const Workspace *workspace);
#endif /* defined(EVACUATOR_DEBUG) */

protected:

public:

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
	 */
	void bindWorkerThread(MM_EnvironmentStandard *env, uintptr_t tenureMask);

	/**
	 * Per gc, unbind evacuator instance from worker thread, merge evacuator gc stats
	 *
	 * @param[in] env worker thread environment to unbind from
	 */
	void unbindWorkerThread(MM_EnvironmentStandard *env);

	/**
	 * Bound artifacts
	 */
	MM_EvacuatorDelegate *getDelegate() { return &_delegate; }

	/**
	 * Work status
	 */
	bool hasScanWork() const { return _nil != _sp; }
	uintptr_t getVolumeOfWork() const { return _workList.volume(); }
	uintptr_t getDistributableVolumeOfWork() const ;

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
	 * expects to deal with a stream of pointers to root objects presented via evacuateRootObject
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
	omrobjectptr_t
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
		return object;
	}

	/**
	 * Copy and forward root object given slot object encapsulating address of referring slot
	 *
	 * @param slotObject pointer to slot object encapsulating address of referring slot
	 * @param breadthFirst copy object without recursing into dependent referents
	 * @return the forwarded address, or the original address if evacuation failed
	 */
	omrobjectptr_t
	evacuateRootObject(const GC_SlotObject *slotObject, bool breadthFirst = false)
	{
		omrobjectptr_t object = slotObject->readReferenceFromSlot();
		if (isInEvacuate(object)) {
			/* slot object must be evacuated -- determine before and after object size */
			MM_ForwardedHeader forwardedHeader(object, _env->compressObjectReferences());
			object = evacuateRootObject(&forwardedHeader, breadthFirst);
			Debug_MM_true(NULL != object);
			slotObject->writeReferenceToSlot(object);
		}
		/* failure to evacuate must be reported as object in nursery space to maintain remembered set integrity */
		return object;
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
	void receiveWhitespace(Region region, Whitespace *whitespace);

	/**
	 * Controller calls this to force evacuator to flush unused whitespace from survivor or tenure whitelist.
	 */
	void flushWhitespace(Region region);

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
		: MM_EvacuatorBase(workerIndex, extensions)
		, _controller(controller)
		, _maxStackDepth(isScanOptionSelected(_extensions, breadth_first_always) ? 1 : _extensions->evacuatorMaximumStackDepth)
		, _maxInsideCopyDistance(_objectModel->adjustSizeInBytes(OMR_MAX(modal_inside_copy_distance, _extensions->evacuatorMaximumInsideCopyDistance)))
		, _minInsideCopySize(modal_inside_copy_size)
		, _maxInsideCopySize(_objectModel->adjustSizeInBytes(OMR_MAX(_minInsideCopySize, _extensions->evacuatorMaximumInsideCopySize)))
		, _workReleaseThreshold(_extensions->evacuatorMinimumWorkspaceSize)
		, _conditionFlags(0)
		, _insideDistanceMax(_maxInsideCopyDistance)
		, _insideSizeMax(_maxInsideCopySize)
		, _slot(NULL)
		, _stack(newStackFrameArray(_forge, _maxStackDepth + 1))
		, _nil(_stack + _maxStackDepth)
		, _sp(_nil)
		, _tenureMask(0)
		, _stats(NULL)
		, _delegate()
		, _workList(&_freeList)
		, _freeList(_forge)
		, _whiteList(MM_EvacuatorWhitelist::newInstanceArray(_forge))
		, _mutex(NULL)
		, _abortedCycle(false)
	{
		_typeId = __FUNCTION__;

		_rp[insideSurvivor] = _rp[insideTenure] = _nil;
		_whiteList[survivor].evacuator(this);
		_whiteList[tenure].evacuator(this);
		_workList.evacuator(this);

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		Debug_MM_true(0 == (_objectModel->getObjectAlignmentInBytes() % sizeof(uintptr_t)));
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

		Assert_MM_true((NULL != _stack) && (NULL != _whiteList));
	}

	friend class MM_EvacuatorController;
	friend class MM_ScavengerRootClearer;
};

#endif /* EVACUATOR_HPP_ */
