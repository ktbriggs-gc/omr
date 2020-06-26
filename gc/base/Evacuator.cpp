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

#include "thrtypes.h"

#include "EnvironmentStandard.hpp"
#include "Evacuator.hpp"
#include "EvacuatorController.hpp"
#include "EvacuatorDelegate.hpp"
#include "ForwardedHeader.hpp"
#include "GCExtensionsBase.hpp"
#include "IndexableObjectScanner.hpp"
#include "Math.hpp"
#if defined(OMR_VALGRIND_MEMCHECK)
#include "MemcheckWrapper.hpp"
#endif /* defined(OMR_VALGRIND_MEMCHECK) */
#include "ObjectModel.hpp"
#include "ScavengerStats.hpp"
#include "SlotObject.hpp"
#include "SublistFragment.hpp"

extern "C" {
	uintptr_t allocateMemoryForSublistFragment(void *vmThreadRawPtr, J9VMGC_SublistFragment *fragmentPrimitive);
}

MM_Evacuator *
MM_Evacuator::newInstance(uintptr_t workerIndex, MM_EvacuatorController *controller, MM_GCExtensionsBase *extensions, uint8_t *regionBase[2])
{
	MM_Evacuator *evacuator = (MM_Evacuator *)extensions->getForge()->allocate(sizeof(MM_Evacuator), OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
	if (NULL != evacuator) {
		new(evacuator) MM_Evacuator(workerIndex, controller, extensions, regionBase);
		if (!evacuator->initialize()) {
			evacuator->kill();
			evacuator = NULL;
		}
	}
	return evacuator;
}

void
MM_Evacuator::kill()
{
	tearDown();
	_forge->free(this);
}

bool
MM_Evacuator::initialize()
{
	Debug_MM_true((_maxStackDepth > 1) || isScanOptionSelected(_extensions, breadth_first_always));

	/* enable spinning for monitor-enter own evacuator worklist to put/take work */
	if (0 != omrthread_monitor_init_with_name(&_mutex, 0, "MM_Evacuator::_mutex")) {
		return false;
	}

	/* disable spinning for monitor-try-enter other evacuator worklist mutex to pull work */
	_mutex->flags &= ~J9THREAD_MONITOR_TRY_ENTER_SPIN;

	/* initialize the delegate */
	if (!_delegate.initialize(this, _forge, _controller)) {
		return false;
	}

	return true;
}

void
MM_Evacuator::tearDown()
{
	/* tear down delegate */
	_delegate.tearDown();

	/* tear down mutex */
	if (NULL != _mutex) {
		omrthread_monitor_destroy(_mutex);
		_mutex = NULL;
	}

	/* free forge memory bound to arrays instantiated in constructor */
	_forge->free(_stackBottom);
	_forge->free(_copyspace);
	_forge->free(_whiteList);

	/* free the freelist */
	_freeList.reset();
}

/**
 * Per gc cycle setup. This binds the evacuator instance to a gc worker thread for the duration of the cycle.
 *
 * @param env the environment for the gc worker thread that is bound to this evacuator instance
 */
void
MM_Evacuator::bindWorkerThread(MM_EnvironmentStandard *env, uintptr_t tenureMask, uint8_t *heapBounds[][2], uintptr_t copiedBytesReportingDelta)
{
	Debug_MM_true(0 == _workList.volume());

	omrthread_monitor_enter(_mutex);

	/* bind evacuator and delegate to executing gc thread */
	_env = env;
	_env->setEvacuator(this);
	_tenureMask = tenureMask;
	
	/* make a local copy of heap boundaries */
	for (Region region = survivor; region <= evacuate; region = nextEvacuationRegion(region)) {
		_heapBounds[region][0] = heapBounds[region][0]; /* lower bound for heap region address range */
		_heapBounds[region][1] = heapBounds[region][1]; /* upper bound for heap region address range */
	}

	/* reset the delegate */
	_delegate.cycleStart();

	/* clear worker gc stats */
	_stats = &_env->_scavengerStats;
	_stats->clear(true);
	_stats->_gcCount = _env->getExtensions()->scavengerStats._gcCount;

	/* Reset the local remembered set fragment */
	_env->_scavengerRememberedSet.count = 0;
	_env->_scavengerRememberedSet.fragmentCurrent = NULL;
	_env->_scavengerRememberedSet.fragmentTop = NULL;
	_env->_scavengerRememberedSet.fragmentSize = (uintptr_t)OMR_SCV_REMSET_FRAGMENT_SIZE;
	_env->_scavengerRememberedSet.parentList = &_env->getExtensions()->rememberedSet;

	/* clear cycle stats */
	_slotExpandedVolume = _baseReportingVolumeDelta = 0;
	 _volumeReportingThreshold = copiedBytesReportingDelta;
	for (intptr_t metric = survivor_copy; metric < metrics; metric += 1) {
		_baseMetrics[metric] = _volumeMetrics[metric] = 0;
	}
	_copyspaceOverflow[survivor] = _copyspaceOverflow[tenure] = 0;
	_abortedCycle = false;

	/* set up whitespaces for the cycle */
	_whiteList[survivor].reset(_controller->getMemorySubspace(survivor));
	_whiteList[tenure].reset(_controller->getMemorySubspace(tenure));

	/* load some empty workspaces into the freelist -- each evacuator retains forge memory between gc cycles to back this up */
	_freeList.reload();

	omrthread_monitor_exit(_mutex);
}

/**
 * Per gc cycle cleanup. This unbinds the evacuator instance from its gc worker thread.
 *
 * @param env the environment for the gc worker thread that is bound to this evacuator instance
 */
void
MM_Evacuator::unbindWorkerThread(MM_EnvironmentStandard *env)
{
	omrthread_monitor_enter(_mutex);

	Debug_MM_true((0 == _workList.volume()) || isAbortedCycle());

	/* flush to freelist any abandoned work from the worklist  */
	_workList.flush();

	/* merge GC stats */
	_controller->mergeThreadGCStats(_env);

	/* unbind delegate from gc thread */
	_delegate.cycleEnd();

	/* unbind evacuator from gc thread */
	_env->setEvacuator(NULL);
	_env = NULL;

	omrthread_monitor_exit(_mutex);
}

void
MM_Evacuator::workThreadGarbageCollect(MM_EnvironmentStandard *env)
{
	Debug_MM_true(_env == env);
	Debug_MM_true(_env->getEvacuator() == this);
	Debug_MM_true(_controller->isBoundEvacuator(_workerIndex));
	Debug_MM_true(NULL == _whiteStackFrame[survivor]);
	Debug_MM_true(NULL == _whiteStackFrame[tenure]);

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	memset(_conditionCounts, 0, sizeof(_conditionCounts));
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

	/* initialize copyspaces */
	_largeCopyspace.evacuator(this);
	_largeCopyspace.reset(survivor, getRegionBase(survivor));
	for (Region region = survivor; region <= tenure; region = nextEvacuationRegion(region)) {
		_copyspace[region].evacuator(this);
		_copyspace[region].reset(region, getRegionBase(region));
	}

	/* set the frame below top as empty stack white frame for tenure */
	_whiteStackFrame[tenure] = top(-1);
	_whiteStackFrame[tenure]->evacuator(this);
	_whiteStackFrame[tenure]->setScanspace(tenure, getRegionBase(tenure), getRegionBase(tenure), 0);
	/* initialize remaining stack frames as empty survivor frames */
	_whiteStackFrame[survivor] = top(-2);
	for (_scanStackFrame = _whiteStackFrame[survivor]; _scanStackFrame >= _stackBottom; _scanStackFrame -= 1) {
		_scanStackFrame->evacuator(this);
		_scanStackFrame->setScanspace(survivor, getRegionBase(survivor), getRegionBase(survivor), 0);
	}
	/* set stack limit to top (unreachable) frame and start with empty scan stack */
	_stackLimit = _stackCeiling;
	_scanStackFrame = NULL;

	/* set scan options to initial conditions and start with minimal workspace release threshold */
	_conditionFlags = _evacuatorScanOptions & initial_mask;
	_workspaceReleaseThreshold = _controller->_minimumWorkspaceSize;

	/**
	 * tl;dr
	 *
	 * This is just flattening the remembered referent/root set dependent tail recursion:
	 *
	 * ((remembered | scan) roots) | (evacuate | forward (copy | scan)?)*
	 *
	 * remembered() presents the remembered set to scan() which produces a set of addresses
	 * of slots in tenured objects that may contain heap references in evacuation space.
	 * roots() produces a set of addresses of slots containing heap references (disregarding
	 * slot format). The union of remembered and root slot addresses is presented to evacuate()
	 * which selects slots containing references to evacuation space. The addresses of these
	 * slots are presented to forward() which sets the forwarding address for the evacuating
	 * slot object and updates the slot address. Also, if it laid down a new forwarding pointer,
	 * forward() presents the slot address to copy(). copy() moves the object into survivor or
	 * tenure space and presents the forwarded slot address to scan(). The net result is
	 * presented to evacuate() for continuation.
	 *
	 * The show stops when the net result is empty. At that point, the aggregate volume of
	 * objects copied must equal the aggregate volume of objects scanned or the show was
	 * a flop.
	 */
	/* scan roots and remembered set and objects depending from these */
	scanRemembered();
	scanRoots();
	scanHeap();

	/* trick to obviate a write barrier -- thread root objects tenured this cycle need to be rescanned to set forwarded address */
	if (!isAbortedCycle() && _controller->isEvacuatorFlagSet(MM_EvacuatorController::rescanThreadSlots)) {
		_delegate.rescanThreadSlots();
		flushRememberedSet();
	}

	/* scan clearable objects -- this may involve 0 or more phases, with evacuator threads joining in scanHeap() at end of each phase */
	while (!isAbortedCycle() && _delegate.hasClearable()) {

		/* java root clearer has repeated phases involving *deprecated* evacuateHeap() and its delegated scanClearable() leaves no unscanned work */
		if (scanClearable()) {
			/* runtimes should use evacuateObject() only in delegated scanClearable(), obviate evacuateHeap() call and allow scanHeap() here to complete each delegated phase */
			 scanHeap();
		}
	}

	/* flush thread-local buffers at end of gc cycle */
	_delegate.flushForEndCycle();
	Debug_MM_true(0 == _env->_scavengerRememberedSet.count);

	/* clear whitespace from outside copyspaces and whitespace frames */
	for (Region region = survivor; region <= tenure; region = nextEvacuationRegion(region)) {

		/* clear whitespace from the last active whitespace frame */
		Debug_MM_true(NULL != _whiteStackFrame[region]);
		_whiteList[region].add(_whiteStackFrame[region]->trim());
		_whiteStackFrame[region] = NULL;

		/* release unused whitespace from outside copyspaces */
		Debug_MM_true(isAbortedCycle() || (0 == _copyspace[region].getCopySize()));
		_whiteList[region].add(_copyspace[region].trim());
	}

#if defined(EVACUATOR_DEBUG)
	_whiteList[survivor].verify();
	_whiteList[tenure].verify();
#endif /* defined(EVACUATOR_DEBUG) */

	/* reset large copyspace (it is void of work and whitespace at this point) */
	Debug_MM_true(0 == _largeCopyspace.getCopySize());
	Debug_MM_true(0 == _largeCopyspace.getWhiteSize());

	/* large survivor whitespace fragments on the whitelist may be recycled back into the memory pool */
	flushWhitespace(survivor);

	/* tenure whitespace is retained between completed nursery collections but must be flushed for backout */
	if (!isAbortedCycle()) {

		/* prune remembered set to complete cycle */
		_controller->pruneRememberedSet(_env);
		Debug_MM_true(0 == _workList.volume());

	} else {

		/* flush tenure whitelist before backout */
		flushWhitespace(tenure);

		/* set backout flag to abort cycle */
		_controller->setBackOutFlag(_env, backOutFlagRaised);
		_controller->completeBackOut(_env);
	}
}

omrobjectptr_t
MM_Evacuator::evacuateRootObject(MM_ForwardedHeader *forwardedHeader, bool breadthFirst)
{
	Debug_MM_true(!hasScanWork());
	Debug_MM_true(!isConditionSet(scanning_heap));
	Debug_MM_true(0 == _stackBottom->getWorkSize());
	Debug_MM_true(NULL != _whiteStackFrame[survivor]);
	Debug_MM_true(0 == _whiteStackFrame[survivor]->getWorkSize());
	Debug_MM_true(NULL != _whiteStackFrame[tenure]);
	Debug_MM_true(0 == _whiteStackFrame[tenure]->getWorkSize());

	if (isAbortedCycle()) {
		/* failure to evacuate will return original pointer to evacuate region */
		return forwardedHeader->getObject();
	} else if (forwardedHeader->isForwardedPointer()) {
		/* return pointer to forwarded object if copied and forwarded by other evacuator */
		return forwardedHeader->getForwardedObject();
	}

	omrobjectptr_t forwardedAddress = NULL;

	/* determine before and after object size and which evacuation region should receive this object */
	uintptr_t slotObjectSizeAfterCopy = 0, slotObjectSizeBeforeCopy = 0, hotFieldAlignmentDescriptor = 0;
	_objectModel->calculateObjectDetailsForCopy(_env, forwardedHeader, &slotObjectSizeBeforeCopy, &slotObjectSizeAfterCopy, &hotFieldAlignmentDescriptor);
	Region const region = isNurseryAge(_objectModel->getPreservedAge(forwardedHeader)) ? survivor : tenure;

	/* set breadth first root condition for this object to force outside copy without reflection into stack whitespace */
	bool breadthFirstRoot = isConditionSet(breadth_first_roots);
	setCondition(breadth_first_roots, breadthFirstRoot || breadthFirst);

	/* direct copy to outside copyspace if forcing breadth first or object is very large (eg split array pointer) */
	if (!isBreadthFirstCondition() && !isLargeObject(slotObjectSizeAfterCopy)) {

		/* move stack whitespace to topmost frames */
		clear();

		/* try to copy into region whitespace (refresh it if too small for copy and remnant is disposable) */
		if ((slotObjectSizeAfterCopy <= _stackBottom->getWhiteSize()) || reserveInsideScanspace(region, slotObjectSizeAfterCopy)) {
			Debug_MM_true(_stackBottom == _whiteStackFrame[region]);

			/* copy and forward object to white stack frame (we don't know the referring address so just say NULL) */
			omrobjectptr_t const copyHead = (omrobjectptr_t)_stackBottom->getCopyHead();
			forwardedAddress = copyForward(forwardedHeader, NULL, _stackBottom, slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy);

			/* if copied into white stack frame pull tail (work+white) from receiving frame into bottomh frame for scanning */
			if (forwardedAddress == copyHead) {
				push(_stackBottom);
			}
		}
	}

	/* if not copied try outside or large object copyspaces (or stack scanspace unless breadth_first_roots condition is set) */
	if (NULL == forwardedAddress) {
		/* if copy is redirected to inside whitespace this will push receiving frame to force scanning on stack */
		forwardedAddress = copyOutside(region, forwardedHeader, NULL, slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy, isSplitablePointerArray(forwardedHeader, slotObjectSizeAfterCopy));
	}

	/* check for direct or redirected copy into stack whitespace */
	if (hasScanWork()) {
		Debug_MM_true(!isConditionSet(breadth_first_roots));
		/* scan stack until empty, leaving unscanned work in outside copyspaces and worklist for evacuaction in scanHeap() */
		scan();
	}

	/* restore original breadth first root flush condition after copy is complete */
	setCondition(breadth_first_roots, breadthFirstRoot);

	Debug_MM_true(!hasScanWork());
	Debug_MM_true(0 == _stackBottom->getWorkSize());
	Debug_MM_true(isInSurvivor(forwardedAddress) || isInTenure(forwardedAddress) || (isInEvacuate(forwardedAddress) && isAbortedCycle()));

	return forwardedAddress;
}

bool
MM_Evacuator::evacuateRememberedObject(omrobjectptr_t objectptr)
{
	Debug_MM_true((NULL != objectptr) && isInTenure(objectptr));
	Debug_MM_true(_objectModel->getRememberedBits(objectptr) < (uintptr_t)0xc0);

	bool rememberObject = false;

	GC_ObjectScannerState objectScannerState;
	GC_ObjectScanner *objectScanner = _delegate.getObjectScanner(objectptr, &objectScannerState, GC_ObjectScanner::scanRoots);
	if (NULL != objectScanner) {
		GC_SlotObject *slotPtr;

		/* scan a remembered object - evacuate all referents in evacuate region */
		while (NULL != (slotPtr = objectScanner->getNextSlot())) {
			if (evacuateRootObject(slotPtr)) {
				rememberObject = true;
			}
		}
	}

	/* the remembered state of the object may also depends on its indirectly related objects */
	if (_objectModel->hasIndirectObjectReferents((CLI_THREAD_TYPE*)_env->getLanguageVMThread(), objectptr)) {
		if (_delegate.scanIndirectObjects(objectptr)) {
			rememberObject = true;
		}
	}

	return rememberObject;
}

void
MM_Evacuator::evacuateThreadSlot(volatile omrobjectptr_t *objectPtrIndirect)
{
	Debug_MM_true(!_env->getExtensions()->isConcurrentScavengerEnabled());
	Debug_MM_true(_env->getExtensions()->isEvacuatorEnabled());

	omrobjectptr_t objectPtr = *objectPtrIndirect;
	if (NULL != objectPtr) {
		/* auto-remember stack- and thread-referenced objects */
		if (isInEvacuate(objectPtr)) {

			if (!evacuateRootObject(objectPtrIndirect)) {
				Debug_MM_true(isInTenure(*objectPtrIndirect));
				Trc_MM_ParallelScavenger_copyAndForwardThreadSlot_deferRememberObject(_env->getLanguageVMThread(), *objectPtrIndirect);
				/* the object was tenured while it was referenced from the stack. Undo the forward, and process it in the rescan pass. */
				_controller->setEvacuatorFlag(MM_EvacuatorController::rescanThreadSlots, true);
				*objectPtrIndirect = objectPtr;
			}

		} else if (isInTenure(objectPtr)) {

			Debug_MM_true(_objectModel->getRememberedBits(objectPtr) < (uintptr_t)0xc0);
			if (_objectModel->atomicSwitchReferencedState(objectPtr, OMR_TENURED_STACK_OBJECT_RECENTLY_REFERENCED, OMR_TENURED_STACK_OBJECT_CURRENTLY_REFERENCED)) {
				Trc_MM_ParallelScavenger_copyAndForwardThreadSlot_renewingRememberedObject(_env->getLanguageVMThread(), objectPtr, OMR_TENURED_STACK_OBJECT_RECENTLY_REFERENCED);
			}
			Debug_MM_true(_objectModel->getRememberedBits(objectPtr) < (uintptr_t)0xc0);

		} else {

			Debug_MM_true(isInSurvivor(objectPtr));
		}
	}
}

void
MM_Evacuator::rescanThreadSlot(omrobjectptr_t *objectPtrIndirect)
{
	omrobjectptr_t objectPtr = *objectPtrIndirect;
	if (isInEvacuate(objectPtr)) {

		/* the slot was left unforwarded in the first pass to signal that its remembered state must be updated here */
		MM_ForwardedHeader forwardedHeader(objectPtr, _env->compressObjectReferences());
		omrobjectptr_t tenuredObjectPtr = forwardedHeader.getForwardedObject();

		/* set the slot to point to the forwarded address and update remembered state now */
		*objectPtrIndirect = tenuredObjectPtr;
		setRememberedState(tenuredObjectPtr, OMR_TENURED_STACK_OBJECT_CURRENTLY_REFERENCED);

		Debug_MM_true(NULL != tenuredObjectPtr);
		Debug_MM_true(isInTenure(tenuredObjectPtr));
	}
}

bool
MM_Evacuator::evacuateHeap()
{
	setCondition(scanning_heap, true);
	setCondition(breadth_first_roots, false);

	/* TODO: deprecate this calling pattern, forced by MM_RootScanner; root scanners should not call scanHeap() directly  */
	scanHeap();

	return !isAbortedCycle();
}

void
MM_Evacuator::scanRoots()
{
#if defined(EVACUATOR_DEBUG)
	if (isDebugCycle()) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("%5lu %2lu %2lu:     roots; ", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex);
		_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
		_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
		omrtty_printf("; flags:%lx; vow:%lx\n", _controller->sampleEvacuatorFlags(), _workList.volume());
	}
#endif /* defined(EVACUATOR_DEBUG) */

	setCondition(scanning_heap, false);
	setCondition(breadth_first_roots, isScanOptionSelected(breadth_first_roots));
	setCondition(clearing, false);

	/* this generates calling pattern [ evacuateRootObject()* evacuateThreadSlot()* evacuateRootObject()* ] while iterating root set */
	_delegate.scanRoots();
}

void
MM_Evacuator::scanRemembered()
{
#if defined(EVACUATOR_DEBUG)
	if (isDebugCycle()) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("%5lu %2lu %2lu:remembered; ", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex);
		_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
		_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
		omrtty_printf("; flags:%lx; vow:%lx\n", _controller->sampleEvacuatorFlags(), _workList.volume());
	}
#endif /* defined(EVACUATOR_DEBUG) */

	setCondition(remembered_set, true);

	/* flush locally buffered remembered objects to aggregated list */
	_env->flushRememberedSet();
	/* this generates calling pattern [ evacuateRememberedObject()* ] while iterating remembered set */
	_controller->scavengeRememberedSet(_env);

	setCondition(remembered_set, false);
}

void
MM_Evacuator::scanHeap()
{
#if defined(EVACUATOR_DEBUG)
	/* stack is empty */
	Debug_MM_true(!isConditionSet(breadth_first_roots) || !hasScanWork());
	if (isDebugCycle()) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("%5lu %2lu %2lu:      heap; ", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex);
		_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
		_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
		omrtty_printf("; flags:%lx; vow:%lx\n", _controller->sampleEvacuatorFlags(), _workList.volume());
	}
#endif /* defined(EVACUATOR_DEBUG) */

	/* mark start of scan cycle -- this will be reset after all evacuators complete all scan work or abort */
	setCondition(scanning_heap, true);
	setCondition(breadth_first_roots, false);

	/* pull work into the stack to scan until all evacuators clear scanspace stack, outside copyspaces and worklist or abort */
	while (getWork()) {

		/* scan stack until empty */
		scan();
	}
}

bool
MM_Evacuator::scanClearable()
{
#if defined(EVACUATOR_DEBUG)
	if (isDebugCycle()) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("%5lu %2lu %2lu: clearable; ", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex);
		_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
		_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
		omrtty_printf("; flags:%lx; vow:%lx\n", _controller->sampleEvacuatorFlags(), _workList.volume());
	}
#endif /* defined(EVACUATOR_DEBUG) */

	setCondition(scanning_heap, false);
	setCondition(breadth_first_roots, true);
	setCondition(clearing, true);

	/* if there are more root or other unreachable objects to be evacuated they can be copied and forwarded here */
	_delegate.scanClearable();

	/* run scanHeap() if scanClearable() produced more work to be scanned */
	return !_controller->hasCompletedScan();
}

void
MM_Evacuator::scanComplete()
{
#if defined(EVACUATOR_DEBUG)
	if (!isAbortedCycle()) {
		Debug_MM_true(!hasScanWork());
		Debug_MM_true(isConditionSet(scanning_heap));
		for (MM_EvacuatorScanspace *stackFrame = _stackBottom; stackFrame < _stackTop; stackFrame += 1) {
			Debug_MM_true(0 == stackFrame->getWorkSize());
			Debug_MM_true(0 == stackFrame->getWhiteSize() || (stackFrame == _whiteStackFrame[stackFrame->getEvacuationRegion()]));
		}
		Debug_MM_true(0 == _copyspace[survivor].getCopySize());
		Debug_MM_true(0 == _copyspace[tenure].getCopySize());
		Debug_MM_true(0 == _workList.volume());
	}
#endif /* defined(EVACUATOR_DEBUG) */

	/* report final copied/scanned volumes for this scan cycle */
	_baseReportingVolumeDelta = _controller->reportProgress(this, _baseMetrics, _volumeMetrics);

	/* all done heap scan */
	setCondition(scanning_heap, false);
	setCondition(breadth_first_roots, false);
	setCondition(clearing, false);
}

uintptr_t
MM_Evacuator::adjustWorkReleaseThreshold()
{
	/* copyspaces are rebased to release a workspace when the volume of work in copyspace exceeds work release threshold */
	if (!isDistributeWorkCondition()) {

		uintptr_t volumeOfWork = _workList.volume();
		if (volumeOfWork < _controller->_minimumWorkspaceSize) {
			/* set threshold to controller's minimum threshold to produce a high volume of worklisted roots when heap scan starts */
			return _controller->_minimumWorkspaceSize;
		} else if (volumeOfWork > _controller->_maximumWorkspaceSize) {
			/* clip threshold to controller's minimum threshold when worklist volume is high */
			return _controller->_maximumWorkspaceSize;
		} else {
			/* scale threshold to worklist volume for normal operation */
			return volumeOfWork;
		}
	}

	/* promote work distribution and parallelize (cut and shed) scanning dense/recursive structures */
	return min_workspace_release;
}

void
MM_Evacuator::addWork(MM_EvacuatorWorkspace *workspace, bool defer)
{
	/* append/prepend workspace to worklist */
	omrthread_monitor_enter(_mutex);
	while (NULL != workspace) {
		MM_EvacuatorWorkspace *next = workspace->next;
		workspace->next = NULL;
		_workList.add(workspace, defer);
		workspace = next;
	}
	omrthread_monitor_exit(_mutex);

	/* notify controller */
	gotWork();
}

void
MM_Evacuator::addWork(MM_EvacuatorCopyspace *copyspace)
{
	Debug_MM_true(copyspace->getCopySize());

	/* pull work from copyspace into worklist, deferring if work was copied in stack overflow condition */
	MM_EvacuatorWorkspace *workspace = _freeList.next();
	const bool stackOverflow = copyspace->isStackOverflow();
	workspace->base = (omrobjectptr_t)copyspace->rebase(&workspace->length);
	addWork(workspace, stackOverflow);
	if (stackOverflow) {
		copyspace->setStackOverflow(false);
	}
}

void
MM_Evacuator::gotWork()
{
	/* check work volume and send notification of work to controller if quota filled and notification pending */
	if ((0 < getDistributableVolumeOfWork(min_workspace_release)) && _controller->isNotififyOfWorkPending()) {
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		uint64_t waitStartTime = startWaitTimer("notify");
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

		_controller->notifyOfWork(this);

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		endWaitTimer(waitStartTime);
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
	}
}

uintptr_t
MM_Evacuator::getDistributableVolumeOfWork(uintptr_t workReleaseThreshold)
{
	/* this method treats _workList as volatile and may be called safely without acquiring the evacuator mutex */
	uintptr_t distributableVolume = _workList.volume() >> 1;
	if (!_controller->hasFulfilledWorkQuota(workReleaseThreshold, distributableVolume)) {
		distributableVolume = 0;
	}
	return distributableVolume;
}

void
MM_Evacuator::findWork()
{
	Debug_MM_true(!isAbortedCycle());

	/* select as prospective donor the evacuator with greatest volume of distributable work */
	uintptr_t maxVolume = 0;
	MM_Evacuator *maxDonor = NULL;
	for (MM_Evacuator *next = _controller->getNextEvacuator(this); next != this; next = _controller->getNextEvacuator(next)) {
		uintptr_t volume = next->getDistributableVolumeOfWork(min_workspace_release);
		if (maxVolume < volume) {
			maxVolume = volume;
			maxDonor = next;
		}
	}

	/*if no prospective donor just return */
	if (NULL == maxDonor) {
		return;
	}

	/* starting with max donor poll evacuators for distributable work */
	MM_Evacuator *donor = maxDonor;
	do {

		/* do not wait if donor is involved with its worklist */
		if (0 < donor->getDistributableVolumeOfWork(min_workspace_release)) {

			/* retest donor volume of work after locking its worklist */
			if (0 == omrthread_monitor_try_enter(donor->_mutex)) {

				/* pull work from donor worklist into stack and worklist until pulled volume levels up with donor remainder */
				if (0 < donor->getDistributableVolumeOfWork(min_workspace_release)) {
					pull(&donor->_workList);
				}

				omrthread_monitor_exit(donor->_mutex);

				 /* break to scan work pulled into the stack */
				if (hasScanWork()) {
					break;
				}
			}
		}

		/* keep polling until donor wraps around to max donor */
		donor = _controller->getNextEvacuator(donor);

	} while (donor != maxDonor);

#if defined(EVACUATOR_DEBUG)
	if (isDebugWork()) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		uintptr_t flags = _controller->sampleEvacuatorFlags();
		omrtty_printf("%5lu %2lu %2lu:%10s; flags:%lx; ", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex, (hasScanWork() ? " pull work" : "   no work"), flags);
		_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
		_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
		if (hasScanWork()) {
			uintptr_t stacked = 0;
			for (MM_EvacuatorScanspace *frame = _scanStackFrame; frame >= _stackBottom; frame -= 1) {
				stacked += frame->getWorkSize();
			}
			omrtty_printf("; stacked: %lx; vow:%lx; donor:%lx; donor-vow:%lx\n", stacked, _workList.volume(), donor->_workerIndex, donor->_workList.volume());
		} else {
			omrtty_printf("; stacked: 0; vow:%lx\n", _workList.volume());
		}
	}
#endif /* defined(EVACUATOR_DEBUG) */
}

bool
MM_Evacuator::getWork()
{
	Debug_MM_true((0 == _copyspace[survivor].getWhiteSize()) || (_copyspace[survivor].getWhiteSize() >= min_workspace_release) || (_copyspace[survivor].getSize() < _workspaceReleaseThreshold) || isConditionSet(copyspaceTailFillCondition(survivor)));
	Debug_MM_true((0 == _copyspace[tenure].getWhiteSize()) || (_copyspace[tenure].getWhiteSize() >= min_workspace_release) || (_copyspace[tenure].getSize() < _workspaceReleaseThreshold) || isConditionSet(copyspaceTailFillCondition(tenure)));
	Debug_MM_true(!hasScanWork());

	/* clear whitespace from bottom stack frame and select it for scanning */
	clear();

	/* evacuator must clear both outside copyspaces and its worklist before polling other evacuators */
	uintptr_t copysize[] = { _copyspace[survivor].getCopySize(), _copyspace[tenure].getCopySize() };

	/* if outside copyspaces are empty and worklist is not work must be pulled from worklist regardless of quota or distribution condition */
	uintptr_t copyspaceVolume = copysize[survivor] + copysize[tenure];

	/* pull work from worklist if its aggregate volume of work exceeds work quota or outside copyspaces are both empty */
	uintptr_t worklistVolumeQuota = (0 == copyspaceVolume) ? 0 : _controller->getWorkNotificationQuota(_controller->_minimumWorkspaceSize);
	if ((worklistVolumeQuota < _workList.volume()) && !isAbortedCycle()) {

		omrthread_monitor_enter(_mutex);

		/* another evacuator may have stalled or worklist volume might have been reduced while waiting on worklist mutex */
		if ((worklistVolumeQuota < _workList.volume()) && !isAbortedCycle()) {
			/* pull workspaces from worklist into stack scanspaces */
			pull(&_workList);
		}

		omrthread_monitor_exit(_mutex);
	}

	if (!hasScanWork() && (0 < copyspaceVolume) && !isAbortedCycle()) {

		Region outside = survivor;
		/* outside copyspace selection depends on copy size as well as tail filling and work distribution conditions */
		if ((0 < copysize[survivor]) && (0 < copysize[tenure])) {
			/* both outside copyspaces hold work -- try not to pull from a tail filling copyspace */
			bool tailfill[] = { isConditionSet(copyspaceTailFillCondition(survivor)), isConditionSet(copyspaceTailFillCondition(tenure)) };
			/* an outside copyspace is tail filling when its remaining whitespace is too small to produce a minimal workspace */
			if (tailfill[survivor] == tailfill[tenure]) {
				/* tail filling or not in both copyspaces -- pull work from copyspace with the most work */
				if (copysize[survivor] < copysize[tenure]) {
					outside = tenure;
				}
			} else if (tailfill[survivor]) {
				/* survivor outside copyspace is tail filling, but tenure is not and has work to pull */
				outside = tenure;
			}
		} else if (0 < copysize[tenure]){
			/* survivor copyspace has no work, but tenure may have work to pull */
			outside = tenure;
		}

		Debug_MM_true((copysize[outside] >= copysize[otherEvacuationRegion(outside)]) || isConditionSet(copyspaceTailFillCondition(otherEvacuationRegion(outside))));

		/* pull work from copyspace into bottom stack frame */
		pull(&_copyspace[outside]);
	}

	/* if no work and not aborting at this point the worklist and both outside copyspaces must be empty */
	Assert_MM_true(hasScanWork() || ((_workList.volume() == 0) && (0 == copysize[survivor]) && (0 == copysize[tenure])) || isAbortedCycle());

	/* aborting or not, if no local work available try to pull work from other evacuator worklists */
	if (!hasScanWork() || isAbortedCycle()) {
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		uint64_t waitStartTime = startWaitTimer("stall");
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

		/* clear the stack in case work was pulled above while another evacuator aborted */
		_scanStackFrame = NULL;

		/* flush local buffers and scanned/copied byte counters before acquiring controller */
		flushForWaitState();
		_baseReportingVolumeDelta = _controller->reportProgress(this, _baseMetrics, _volumeMetrics);
		_controller->acquireController();

		/* poll other evacuators and try to pull workspaces into stack from evacuator with greatest worklist volume */
		do {
			/* throttle work pipeline while aborting */
			if (!isAbortedCycle()) {
				findWork();
			}
			/* if no work pulled wait for another evacuator to notify of work or notify all to complete or abort heap scan */
		} while (_controller->isWaitingForWork(this));

		/* continue scanning received work, or complete heap scan if stack is empty */
		_controller->releaseController();

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		endWaitTimer(waitStartTime);
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

		/* empty stack at this point means all evacuators have completed (or aborted) heap scan */
		if (!hasScanWork()) {
			scanComplete();
		}
	}

	/* it may be that this evacuator has work and controller has received abort from another evacuator -- that's ok :) */
	return hasScanWork();
}

void
MM_Evacuator::moveStackWhiteFrame(Region region, MM_EvacuatorScanspace *toFrame)
{
	Debug_MM_true(toFrame != _whiteStackFrame[region]);
	Debug_MM_true(0 == _whiteStackFrame[region]->getWorkSize());

	MM_EvacuatorWhitespace *whitespace = _whiteStackFrame[region]->trim();
	if (NULL != whitespace) {
		Debug_MM_true(sizeof(MM_EvacuatorWhitespace) <= whitespace->length());
		toFrame->setScanspace(region, whitespace->getBase(), whitespace->getBase(), whitespace->length());
		Debug_MM_true(region == getEvacuationRegion(toFrame->getBase(), region));
	} else {
		toFrame->setScanspace(region, getRegionBase(region), getRegionBase(region), 0);
	}
	_whiteStackFrame[region] = toFrame;

}

void
MM_Evacuator::clear()
{
	Debug_MM_true(((_stackTop - _stackBottom) == (intptr_t)_maxStackDepth) || (((_stackTop - _stackBottom) == 3) && (_maxStackDepth < 3)));
	Debug_MM_true((_stackBottom <= _whiteStackFrame[survivor]) && (_stackTop > _whiteStackFrame[survivor]));
	Debug_MM_true((_stackBottom <= _whiteStackFrame[tenure]) && (_stackTop > _whiteStackFrame[tenure]));
	Debug_MM_true(survivor == _whiteStackFrame[survivor]->getEvacuationRegion());
	Debug_MM_true(tenure == _whiteStackFrame[tenure]->getEvacuationRegion());
	Debug_MM_true(0 == _whiteStackFrame[survivor]->getWorkSize());
	Debug_MM_true(0 == _whiteStackFrame[tenure]->getWorkSize());
	Debug_MM_true(!hasScanWork());

	/* unconditionally move region whitespace to top stack frames (this is why at least 3 frames are needed), in any order .. */
	MM_EvacuatorScanspace *topFrame[] = { top(-1), top(-2) };
	if (topFrame[0] != _whiteStackFrame[tenure]) {
		/* .. (from top) in ascending region order */
		for (Region topRegion = survivor; topRegion <= tenure; topRegion = nextEvacuationRegion(topRegion)) {
			if (topFrame[topRegion] != _whiteStackFrame[topRegion]) {
				moveStackWhiteFrame(topRegion, topFrame[topRegion]);
			}
		}
	} else if (topFrame[1] != _whiteStackFrame[survivor]) {
		/* .. (from top) in descending region order */
		moveStackWhiteFrame(survivor, topFrame[1]);
	}

	/* clear stack scanspace activation counters */
	for (MM_EvacuatorScanspace *stackFrame = _stackBottom; stackFrame < _stackTop; stackFrame += 1) {
		Debug_MM_true(stackFrame->isEmpty() || (stackFrame >= top(-2) && (0 == stackFrame->getWorkSize())));
		stackFrame->clearActivationCount();
	}

	Debug_MM_true(survivor == _whiteStackFrame[survivor]->getEvacuationRegion());
	Debug_MM_true(tenure == _whiteStackFrame[tenure]->getEvacuationRegion());

	/* reset slot volume */
	_slotExpandedVolume = 0;

	/* region inside whitespaces parked in top stack frames and scanspaces below are clear to pull/push work */
	Debug_MM_true((survivor == _whiteStackFrame[survivor]->getEvacuationRegion()) || (0 == _whiteStackFrame[survivor]->getSize()));
	Debug_MM_true((tenure == _whiteStackFrame[tenure]->getEvacuationRegion()) || (0 == _whiteStackFrame[tenure]->getSize()));
	Debug_MM_true((topFrame[survivor] == _whiteStackFrame[survivor]) || (topFrame[survivor] == _whiteStackFrame[tenure]));
	Debug_MM_true((topFrame[tenure] == _whiteStackFrame[tenure]) || (topFrame[tenure] == _whiteStackFrame[survivor]));
	Debug_MM_true(_stackBottom->isEmpty());
}

void
MM_Evacuator::pull(MM_EvacuatorCopyspace *copyspace)
{
	Debug_MM_true(0 < copyspace->getCopySize());

	/* pull work into bottom scanspace and rebase copyspace */
	_scanStackFrame = _stackBottom;
	_scanStackFrame->pullWork(copyspace);
	_scanStackFrame->activated();

	/* trim copyspace if not enough whitespace to build a viable workspace */
	if (copyspace->getWhiteSize() < min_workspace_release) {
		/* trim remainder whitespace to whitelist */
		_whiteList[copyspace->getEvacuationRegion()].add(copyspace->trim());
	}
}

void
MM_Evacuator::pull(MM_EvacuatorWorklist *worklist)
{
	Debug_MM_true(!hasScanWork());
	Debug_MM_true(NULL != worklist);
	Debug_MM_true(0 < worklist->volume());
	Debug_MM_true(NULL != worklist->peek(worklist != &_workList));
	Debug_MM_true((worklist == &_workList) || (0 == _workList.volume()));

	/* drain deferred workspaces first when pulling from other evacuator's worklist */
	const bool pullDeferred = (worklist != &_workList);

	/* make a local LIFO list of workspaces to be loaded onto the stack for scanning in worklist order */
	MM_EvacuatorWorkspace *stacklist = worklist->next(pullDeferred);

	/* make a local FIFO list of workspaces to be loaded onto the stack for scanning in worklist order */
	MM_EvacuatorWorklist::List locallist;
	locallist.head = locallist.tail = NULL;

	/* if pulling from own worklist limit the take to head when other evacuators are stalling */
	uintptr_t pulledVolume = worklist->volume(stacklist);
	if ((0 < pulledVolume) && (!isConditionSet(stall) || pullDeferred)) {

		/* limit the number of workspaces that can be pulled into stack frames (cascading tail-to-head workspaces are merged N->1) */
		uintptr_t limit = ((_stackTop - _stackBottom) - 2) >> 1;

		/* pull workspaces from source worklist until pulled volume levels up with source worklist volume  */
		const MM_EvacuatorWorkspace *peek = worklist->peek(pullDeferred);
		while (NULL != peek) {
			Debug_MM_true(0 < peek->length);

			/* if under stack limit pull next workspace into stack list */
			uintptr_t nextVolume = worklist->volume(peek);
			if ((pulledVolume + nextVolume) <= (worklist->volume() - nextVolume)) {
				if (0 < limit) {
					/* pull next workspace from source worklist and merge or prepend to LIFO stack list (first in list -> first scanned in top stack frame) */
					MM_EvacuatorWorkspace *workspace = worklist->next(pullDeferred);
					if ((0 == workspace->offset) && (0 == stacklist->offset) &&
						((uint8_t*)workspace->base == ((uint8_t*)stacklist->base + stacklist->length)) &&
						((stacklist->length + workspace->length) < _controller->_maximumWorkspaceSize)
					) {
						/* merge workspace into head of stacklist */
						stacklist->length += workspace->length;
						_freeList.add(workspace);
					} else {
						/* prepend workspace to stacklist */
						workspace->next = stacklist;
						stacklist = workspace;
						limit -= 1;
					}
					peek = worklist->peek(pullDeferred);
					pulledVolume += nextVolume;
				} else if (pullDeferred) {
					/* if source worklist belongs to other evacuator add workspace to local list (no need to take own mutex as this evacuator has the controller mutex) */
					MM_EvacuatorWorkspace *workspace = worklist->next(true);
					_workList.addWorkToList(&locallist, workspace);
					peek = worklist->peek(true);
					pulledVolume += nextVolume;
				} else {
					/* break out of loop */
					break;
				}
			} else {
				/* break out of loop */
				break;
			}
		}
	}

	/* pull workspaces from stack list into stack frames (last pulled workspace on the bottom) */
	MM_EvacuatorScanspace *stackFrame = _stackBottom;
	for (MM_EvacuatorWorkspace *workspace = stacklist; NULL != workspace; workspace = _freeList.add(workspace)) {
		Debug_MM_true(0 < workspace->length);

		/* set workspace into scanspace in next stack frame */
		uintptr_t arrayHeaderSize = 0;
		Region region = isInTenure(workspace->base) ? tenure : (isInSurvivor(workspace->base) ? survivor : unreachable);
		Assert_MM_true(region != unreachable);

		if (isSplitArrayWorkspace(workspace)) {

			/* set scanspace: base=array object, scan = copy = end = arrayEnd */
			uint8_t *arrayHead = (uint8_t *)workspace->base;
			uint8_t *arrayEnd = arrayHead + _objectModel->getConsumedSizeInBytesWithHeader(workspace->base);
			/* split array object scanner holds split segment bounds and actual scan head */
			uintptr_t startSlot = workspace->offset - 1;
			uintptr_t stopSlot = startSlot + workspace->length;

			/* split array object scanner is activated here - hence no way to determine whether scanning has started for frame */
			GC_IndexableObjectScanner *scanner = (GC_IndexableObjectScanner *)stackFrame->activateObjectScanner();
			_delegate.getSplitPointerArrayObjectScanner(workspace->base, scanner, startSlot, stopSlot, GC_ObjectScanner::scanHeap);
			stackFrame->setSplitArrayScanspace(region, arrayHead, arrayEnd, startSlot, stopSlot);

			/* first split array segment accounts for volume of object header and alignment padding etc */
			if (scanner->isHeadObjectScanner()) {
				uintptr_t arrayBytes = getReferenceSlotSize() * ((GC_IndexableObjectScanner *)scanner)->getIndexableRange();
				arrayHeaderSize = stackFrame->getSize() - arrayBytes;
			}

		} else {

			/* set base = scan = workspace base, copy = end = base + workspace volume, object scanner is NULL until scanning starts in frame */
			stackFrame->setScanspace(region, (uint8_t *)workspace->base, (uint8_t *)workspace->base + workspace->length, workspace->length);
		}
		_stats->countWorkPacketSize((arrayHeaderSize + _workList.volume(workspace)), _controller->_maximumWorkspaceSize);
		stackFrame += 1;
	}

	if (_stackBottom < stackFrame) {
		_scanStackFrame = stackFrame - 1;
		_scanStackFrame->activated();
	}

	/* pull workspaces from local list into own worklist */
	if (NULL != locallist.head) {
		addWork(locallist.head);
	}

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	Debug_MM_true((0 == pulledVolume) || hasScanWork());
	Debug_MM_true((_stackBottom <= _scanStackFrame) && (_scanStackFrame < top(-2)));
	if (pullDeferred) {
		uintptr_t stackVolume = 0;
		for (MM_EvacuatorScanspace *frame = _stackBottom; frame <= _scanStackFrame; frame += 1) {
			stackVolume += frame->getWorkSize();
			frame->activated();
		}
		Debug_MM_true(pulledVolume == (stackVolume + _workList.volume()));
	}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
}

void
MM_Evacuator::scan()
{
	Debug_MM_true(!isBreadthFirstCondition() || (isConditionSet(breadth_first_roots) && !isConditionSet(scanning_heap)));
	Debug_MM_true(NULL != _whiteStackFrame[survivor]);
	Debug_MM_true(NULL != _whiteStackFrame[tenure]);

	/* scan stack until empty or cycle is aborted */
	while (hasScanWork() && !isAbortedCycle()) {
		Debug_MM_true((_stackLimit == _stackCeiling) || (_stackLimit == (_stackBottom + 1)));
		Debug_MM_true((_scanStackFrame < _stackLimit) || (_stackLimit == (_stackBottom + 1)));
		Debug_MM_true(_scanStackFrame >= _stackBottom);

		/* copy inside current frame until push or pop */
		copy();
	}

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	for (MM_EvacuatorScanspace *frame = _stackBottom; frame < _stackTop; ++frame) {
		Debug_MM_true(0 == frame->getWorkSize());
		Debug_MM_true((0 == frame->getWhiteSize()) || (frame == _whiteStackFrame[frame->getEvacuationRegion()]));
	}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
}

void
MM_Evacuator::copy()
{
	const uintptr_t sizeLimit = _maxInsideCopySize;
	MM_EvacuatorScanspace * const stackFrame = _scanStackFrame;
	uint8_t * const frameLimit = isConditionSet(depth_first) ? stackFrame->getBase() : (stackFrame->getBase() + _maxInsideCopyDistance);
	MM_EvacuatorScanspace *whiteFrame[] = { _whiteStackFrame[survivor], _whiteStackFrame[tenure] };
	const Region scanRegion = stackFrame->getEvacuationRegion();

	/* get active scanner or null if frame has received copy in stack overflow condition and has not yet been activated */
	GC_ObjectScanner *objectScanner = scanner(false);

	/* copy small stack region objects inside frame, large and other region objects outside, until push() or frame completed */
	while (NULL != objectScanner) {
		Debug_MM_true((NULL == stackFrame->getActiveObject()) || (stackFrame->getActiveObject() == (omrobjectptr_t)stackFrame->getScanHead()) || (stackFrame->isSplitArrayScanspace() && (stackFrame->getActiveObject() == (omrobjectptr_t)stackFrame->getBase())));

		/* set inside copy size limit for indexable/mixed object scanner */

		/* loop through reference slots in current object at scan head */
		GC_SlotObject *slotObject = objectScanner->getNextSlot();
		while (NULL != slotObject) {

			const omrobjectptr_t object = slotObject->readReferenceFromSlot();
			omrobjectptr_t forwardedAddress = object;
			if (isInEvacuate(object)) {

				/* copy and forward the slot object */
				MM_ForwardedHeader forwardedHeader(object, _env->compressObjectReferences());
				if (!forwardedHeader.isForwardedPointer()) {
					Debug_MM_true((survivor == whiteFrame[survivor]->getEvacuationRegion() || (0 == whiteFrame[survivor]->getSize())));
					Debug_MM_true((tenure == whiteFrame[tenure]->getEvacuationRegion() || (0 == whiteFrame[tenure]->getSize())));
					Debug_MM_true(whiteFrame[survivor] == _whiteStackFrame[survivor]);
					Debug_MM_true(whiteFrame[tenure] == _whiteStackFrame[tenure]);

					/* slot object must be evacuated -- determine receiving region and before and after object size */
					uintptr_t slotObjectSizeBeforeCopy = 0, slotObjectSizeAfterCopy = 0, hotFieldAlignmentDescriptor = 0;
					_objectModel->calculateObjectDetailsForCopy(_env, &forwardedHeader, &slotObjectSizeBeforeCopy, &slotObjectSizeAfterCopy, &hotFieldAlignmentDescriptor);
					const Region region = isNurseryAge(_objectModel->getPreservedAge(&forwardedHeader)) ? survivor : tenure;

					/* copy small objects inside the stack if possible, otherwise copy to outside copyspace or large object copyspace */
					if ((sizeLimit > slotObjectSizeAfterCopy) && !isForceOutsideCopyCondition(region)
					&& ((slotObjectSizeAfterCopy <= whiteFrame[region]->getWhiteSize()) || reserveInsideScanspace(region, slotObjectSizeAfterCopy))
					) {
						/* sync local white frame pointer here because _whiteStackFrame may have changed with a new reservation */
						if (0 == whiteFrame[region]->getWhiteSize()) {
							whiteFrame[region] = _whiteStackFrame[region];
						}

						/* copy/forward inside white stack frame and test for copy completion at preset copy head */
						uint8_t * const copyHead = whiteFrame[region]->getCopyHead();
						forwardedAddress = copyForward(&forwardedHeader, slotObject->readAddressFromSlot(), whiteFrame[region], slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy);

						/* if object was copied past copy limit in active stack frame or into superior frame it should be pushed if possible */
						if ((uint8_t *)forwardedAddress == copyHead) {
							if (((copyHead > frameLimit) && (stackFrame == whiteFrame[region])) || (stackFrame < whiteFrame[region])) {
								/* try to find nearest superior frame that can receive whitespace for evacuation region (otherwise leave it to scan in current frame) */
								MM_EvacuatorScanspace * const nextFrame = next(region, stackFrame + 1);
								if (NULL != nextFrame) {
									/* ensure copy and remaining whitespace are in next frame */
									if ((nextFrame < whiteFrame[region]) || isTop(nextFrame)) {
										nextFrame->pullTail(whiteFrame[region], copyHead);
										_whiteStackFrame[region] = nextFrame;
									}
									/* push to next frame */
									push(nextFrame);
								}
							}
						}

						Debug_MM_true((survivor == whiteFrame[survivor]->getEvacuationRegion() || (0 == whiteFrame[survivor]->getSize())));
						Debug_MM_true((tenure == whiteFrame[tenure]->getEvacuationRegion() || (0 == whiteFrame[tenure]->getSize())));

					} else {

						/* copy directly into outside copyspace if there is enough whitespace, otherwise refresh whitespace in copyspace and copy */
						bool isSplitableArray = isSplitablePointerArray(&forwardedHeader, slotObjectSizeAfterCopy);
						if (!isSplitableArray && (slotObjectSizeAfterCopy <= _copyspace[region].getWhiteSize())) {
							/*  copy and release a workspace from copyspace when work volume hits threshold unless tailfilling is necessary */
							forwardedAddress = copyForward(&forwardedHeader, slotObject->readAddressFromSlot(), &_copyspace[region], slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy);
							if ((_copyspace[region].getCopySize() >= _workspaceReleaseThreshold) && (_copyspace[region].getWhiteSize() >= min_workspace_release)) {
								addWork(&_copyspace[region]);
							}
						} else {
							/* try to refresh whitespace for copyspace, or redirect copy into other outside copyspace, large object copyspace, or inside whitespace (and push containing stack scanspace) */
							forwardedAddress = copyOutside(region, &forwardedHeader, slotObject->readAddressFromSlot(), slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy, isSplitableArray);
						}
					}

				} else {

					/* just get the forwarding address */
					forwardedAddress = forwardedHeader.getForwardedObject();
				}

				/* update the referring slot with the forwarding address */
				slotObject->writeReferenceToSlot(forwardedAddress);
			}

			Debug_MM_true(isInSurvivor(forwardedAddress) || isInTenure(forwardedAddress) || (isInEvacuate(forwardedAddress) && isAbortedCycle()));

			/* if scanning a tenured object update its remembered state */
			if ((tenure == scanRegion) && (isInSurvivor(forwardedAddress) || isInEvacuate(forwardedAddress))) {
				stackFrame->updateRememberedState(true);
			}

			/* scan stack frame pushed or popped return to continue in next frame */
			if (_scanStackFrame != stackFrame) {
				return;
			}

			/* continue with next scanned slot object */
			slotObject = objectScanner->getNextSlot();
		}

		/* advance object scanner unless scan head is at copy head */
		objectScanner = scanner(true);
	}

	Debug_MM_true(stackFrame == _scanStackFrame);
	Debug_MM_true(0 == stackFrame->getWorkSize());

	/* pop scan stack */
	pop();
}

void
MM_Evacuator::pop()
{
#if defined(EVACUATOR_DEBUG)
	debugStack("pop");
#endif /* defined(EVACUATOR_DEBUG) */
	Debug_MM_true((_stackBottom <= _scanStackFrame) && (_scanStackFrame < _stackCeiling));
	Debug_MM_true(isAbortedCycle() || (0 == _scanStackFrame->getWorkSize()));
	Debug_MM_true(isAbortedCycle() || (0 == _scanStackFrame->getWhiteSize()) || (_scanStackFrame == _whiteStackFrame[_scanStackFrame->getEvacuationRegion()]));
	Debug_MM_true(isAbortedCycle() || (_scanStackFrame->getScanHead() == _scanStackFrame->getCopyHead()));

	/* clear scanned or deferred work and work-related flags from popped frame */
	_scanStackFrame->reset();

	/* check for stack overflow before popping back to bottom (or inactive) frame */
	if (isConditionSet(stack_overflow | depth_first)
	&& ((_stackBottom == (_scanStackFrame - 1)) || (NULL == (_scanStackFrame - 1)->getActiveObject()))
	) {
		/* check whether stack overflowed or excessive volume of copy was expressed from from bottom slot */
		if (isConditionSet(stack_overflow)) {

			/* force depth first scanning for next slot */
			setCondition(depth_first, true);
			/* reset stack overflow -- set it again if stack oveflows on next slot */
			setCondition(stack_overflow, false);

		} else if (isConditionSet(depth_first) && !isConditionSet(stack_overflow)) {

			/* release from outside copyspaces any unscanned copy received while in stack overflow condition */
			flushOutsideCopyspaces();
			/* stack overflow condition can be cleared when stack unwinds from depth-first traversal with no stack overflow */
			setCondition(depth_first, false);
		}

		/* reset tracking of volume pushed from bottom slot */
		_slotExpandedVolume = 0;
	}

	/* pop stack frame leaving trailing whitespace where it is (in _whiteStackFrame[_scanStackFrame->getEvacuationRegion()]) */
	if (_stackBottom < _scanStackFrame) {

		/* pop to previous frame, will continue with whitespace in popped frame if next pushed object does not cross region boundary */
		_scanStackFrame -= 1;

		/* activate frame now if it was pulled up under head of list of workspaces pulled from a worklist */
		if (0 == _scanStackFrame->getActivationCount()) {
			Debug_MM_true((NULL == _scanStackFrame->getActiveObject()) || _scanStackFrame->isSplitArrayScanspace());
			_scanStackFrame->activated();
		}

	} else {

		/* pop to empty stack */
		_scanStackFrame = NULL;
	}
}

bool
MM_Evacuator::isTop(const MM_EvacuatorScanspace *stackFrame)
{
	/* return true if push() is not possible from stack frame */
	if (top(-2) <= stackFrame) {
		if (_stackTop <= (stackFrame + 1)) {
			return true;
		}
		if (top(-1) == _whiteStackFrame[otherEvacuationRegion(top(-1)->getEvacuationRegion())]) {
			return true;
		}
	}

	return false;
}

void
MM_Evacuator::push(MM_EvacuatorScanspace *nextFrame)
{
	Debug_MM_true(nextFrame < _stackTop);

	if (_scanStackFrame <= nextFrame) {

		/* push to next frame */
		_scanStackFrame = nextFrame;
		_scanStackFrame->activated();

		/* set stack overflow condition if not possible to push from next frame or an excessive cumulative volume copied while scanning bottom slot */
		if ((uintptr_t)stack_overflow == (((uintptr_t)stack_overflow & _evacuatorScanOptions) & _conditionFlags)) {
			if ((isTop(nextFrame) || (_slotExpandedVolume > _slotVolumeThreshold))) {
				setCondition(stack_overflow, true);
			}
		}
#if defined(EVACUATOR_DEBUG)
		debugStack("push");
#endif /* defined(EVACUATOR_DEBUG) */
	}
}

MM_EvacuatorScanspace *
MM_Evacuator::next(Region region, MM_EvacuatorScanspace *proposed)
{
	Debug_MM_true(!isAbortedCycle());
	Debug_MM_true(!isBreadthFirstCondition());

	/* find the nearest frame not inferior to frame that holds or can receive whitespace for evacuation region */
	if (proposed == _whiteStackFrame[otherEvacuationRegion(region)]) {
		/* frame holds whitespace for other evacuation region and must be skipped */
		proposed += 1;
	}

	/* if next frame out of stack bounds return NULL */
	if (proposed >= _stackCeiling) {
		return NULL;
	}

	/* next frame is empty or holds whitespace for evacuation region */
	Debug_MM_true(proposed->isEmpty() || (proposed == _whiteStackFrame[region]));
	return proposed;
}

MM_EvacuatorScanspace *
MM_Evacuator::top(intptr_t offset)
{
	Debug_MM_true(_stackBottom <= (_stackTop + offset));
	Debug_MM_true(_stackTop >= (_stackTop + offset));
	
	/* this may return a pointer to _stackTop as non-const -- intended for operations that work relative to top and write only to inferior frames */
	return _stackBottom + OMR_MAX(_maxStackDepth, unreachable) + offset;
}

omrobjectptr_t
MM_Evacuator::copyOutside(Region region, MM_ForwardedHeader *forwardedHeader, fomrobject_t *referringSlotAddress, const uintptr_t slotObjectSizeBeforeCopy, const uintptr_t slotObjectSizeAfterCopy, const bool isSplitableArray)
{
	Debug_MM_true(!forwardedHeader->isForwardedPointer());
	Debug_MM_true(isInEvacuate(forwardedHeader->getObject()));

	/* failure to evacuate must return original address in evacuate space */
	omrobjectptr_t forwardedAddress = forwardedHeader->getObject();

	/* use the preferred region copyspace if it can contain the object */
	MM_EvacuatorCopyspace *effectiveCopyspace = reserveOutsideCopyspace(&region, slotObjectSizeAfterCopy, isSplitableArray);

	/* effective copyspace (outside or large object copyspace or inside scanspace) has enough whitespace reserved for copy */
	if (NULL != effectiveCopyspace) {
		Debug_MM_true(slotObjectSizeAfterCopy <= effectiveCopyspace->getWhiteSize());

		/* copy slot object to effective outside copyspace */
		omrobjectptr_t copyHead = (omrobjectptr_t)effectiveCopyspace->getCopyHead();
		forwardedAddress = copyForward(forwardedHeader, referringSlotAddress, effectiveCopyspace, slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy);
		if (copyHead == forwardedAddress) {

			/* object copied into effective copyspace -- check for sharable work to distribute */
			if (effectiveCopyspace == &_largeCopyspace) {

				/* add large object to the worklist to free large copyspace for reuse */
				if (isSplitableArray) {

					/* record 1-based array offsets to mark split pointer array workspaces */
					splitPointerArrayWork(&_largeCopyspace);

				} else {
					Debug_MM_true(slotObjectSizeAfterCopy == _largeCopyspace.getCopySize());

					/* extract workspace to worklist and rebase large copyspace */
					addWork(&_largeCopyspace);
				}

				/* clear the large object copyspace for next use */
				_whiteList[region].add(_largeCopyspace.trim());
				_largeCopyspace.setCopyspace(survivor, getRegionBase(survivor), getRegionBase(survivor), 0);

			} else if (effectiveCopyspace != _whiteStackFrame[region]->asCopyspace()) {

				/* copied to outside copyspace, release a workspace when work volume hits threshold */
				if ((effectiveCopyspace->getCopySize() >= _workspaceReleaseThreshold) && (effectiveCopyspace->getWhiteSize() >= min_workspace_release)) {
					addWork(effectiveCopyspace);
				}

			} else {

				/* copied inside stack whitespace -- receiving frame must be pushed if above current frame */
				if (_whiteStackFrame[region] > _scanStackFrame) {

					/* there is a next frame to push to because copy went to white frame above current frame */
					MM_EvacuatorScanspace * const nextFrame = next(region, (NULL != _scanStackFrame) ? (_scanStackFrame + 1) : _stackBottom);
					if (nextFrame != _whiteStackFrame[region]) {
						Debug_MM_true(NULL != nextFrame);

						/* pull copy and remaining whitespace up into next frame */
						nextFrame->pullTail(_whiteStackFrame[region], (uint8_t *)copyHead);
						/* set next frame as white frame for evacuation region */
						_whiteStackFrame[region] = nextFrame;
					}

					/* push to next frame (or same frame if no next frame) */
					push(nextFrame);
				}
				
				/* copied inside stack whitespace as a last resort after failing to reserve from copyspace and whitelist */
				Debug_MM_true(((uint8_t *)copyHead + slotObjectSizeAfterCopy) == _whiteStackFrame[region]->getCopyHead());
				Debug_MM_true(isConditionSet((uintptr_t)copyspaceTailFillCondition(region)));
				Debug_MM_true(_scanStackFrame >= _whiteStackFrame[region]);
			}

		} else if (effectiveCopyspace == &_largeCopyspace) {
			Debug_MM_true(0 == _largeCopyspace.getCopySize());

			/* large object copied by other thread so clip whitespace from large object copyspace onto the whitelist */
			_whiteList[region].add(_largeCopyspace.trim());
		}
	}

	Debug_MM_true(isInSurvivor(forwardedAddress) || isInTenure(forwardedAddress) || (isInEvacuate(forwardedAddress) && isAbortedCycle()));

	return forwardedAddress;
}

omrobjectptr_t
MM_Evacuator::copyForward(MM_ForwardedHeader *forwardedHeader, fomrobject_t *referringSlotAddress, MM_EvacuatorCopyspace * const copyspace, const uintptr_t originalLength, const uintptr_t forwardedLength)
{
	/* if not already forwarded object will be copied to copy head in designated copyspace */
	omrobjectptr_t const copyHead = (omrobjectptr_t)copyspace->getCopyHead();
	Debug_MM_true(isInSurvivor(copyHead) || isInTenure(copyHead));

	/* try to set forwarding address to the copy head in copyspace; otherwise do not copy, just return address forwarded by another thread */
	omrobjectptr_t forwardedAddress = forwardedHeader->setForwardedObject(copyHead);
	if (forwardedAddress == copyHead) {
#if defined(OMR_VALGRIND_MEMCHECK)
		valgrindMempoolAlloc(_env->getExtensions(), (uintptr_t)forwardedAddress, forwardedLength);
#endif /* defined(OMR_VALGRIND_MEMCHECK) */
#if defined(EVACUATOR_DEBUG)
		if (isDebugCopy()) {
			_delegate.debugValidateObject(forwardedHeader);
		}
#endif /* defined(EVACUATOR_DEBUG) */
		Debug_MM_true(MM_EvacuatorWhitespace::isWhitespace(forwardedAddress, copyspace->getEnd() - (uint8_t *)forwardedAddress, isTraceOptionSelected(EVACUATOR_DEBUG_POISON_DISCARD)));

		/* forwarding address set by this thread -- object will be evacuated to the copy head in copyspace */
		memcpy(forwardedAddress, forwardedHeader->getObject(), originalLength);

		/* advance the copy head in the receiving copyspace */
		copyspace->advanceCopyHead(forwardedLength);

		/* copy the preserved fields from the forwarded header into the destination object */
		forwardedHeader->fixupForwardedObject(forwardedAddress);

		/* object model updates object age and finalizes copied object header */
		Region region = copyspace->getEvacuationRegion();
		uintptr_t objectAge = _objectModel->getPreservedAge(forwardedHeader);
		uintptr_t nextAge = (survivor == region) ? (objectAge + 1) : STATE_NOT_REMEMBERED;
		_objectModel->fixupForwardedObject(forwardedHeader, forwardedAddress, OMR_MIN(nextAge, OBJECT_HEADER_AGE_MAX));

#if defined(EVACUATOR_DEBUG)
		if (isDebugCopy()) {
			_delegate.debugValidateObject(forwardedAddress, forwardedLength);
		}
#endif /* defined(EVACUATOR_DEBUG) */

		if (copyspace != _whiteStackFrame[region]->asCopyspace()) {

			/* check for stack overflow condition */
			if (isConditionSet(stack_overflow | depth_first)) {
				copyspace->setStackOverflow(true);
			}

			/* check remaining whitespace */
			uintptr_t whiteSize = copyspace->getWhiteSize();
			if ((min_workspace_release > whiteSize) && (0 < whiteSize)) {
				/* signal tail filling for outside region */
				setCondition(copyspaceTailFillCondition(region), true);
			}
		}

		/* synchronize stall condition with controller */
		setCondition(stall, _controller->areAnyEvacuatorsStalled());

		/* update copy volume metrics for reporting scan/copy progress */
		_slotExpandedVolume += forwardedLength;
		_volumeMetrics[region] += forwardedLength;
		_baseReportingVolumeDelta += forwardedLength;
		if (_baseReportingVolumeDelta >= _volumeReportingThreshold) {
			_baseReportingVolumeDelta = _controller->reportProgress(this, _baseMetrics, _volumeMetrics);
		}

		/* update scavenger stats */
		if (tenure == region) {
			_stats->_tenureAggregateCount += 1;
			_stats->_tenureAggregateBytes += originalLength;
			_stats->getFlipHistory(0)->_tenureBytes[objectAge + 1] += forwardedLength;
#if defined(OMR_GC_LARGE_OBJECT_AREA)
			if (copyspace->isLOA()) {
				_stats->_tenureLOACount += 1;
				_stats->_tenureLOABytes += originalLength;
			}
#endif /* OMR_GC_LARGE_OBJECT_AREA */
		} else {
			_stats->_flipCount += 1;
			_stats->_flipBytes += originalLength;
			_stats->getFlipHistory(0)->_flipBytes[objectAge + 1] += forwardedLength;
		}

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		/* update proximity and size metrics in stats */
		_stats->countCopyDistance((uintptr_t)referringSlotAddress, (uintptr_t)forwardedAddress);
		_stats->countObjectSize(forwardedLength);
		/* update condition metrics */
		_conditionCounts[_conditionFlags & (uintptr_t)conditions_mask] += 1;
		Debug_MM_true(!isConditionSet(breadth_first_roots) || !isConditionSet(recursive_object));
		Debug_MM_true(region == getEvacuationRegion(forwardedAddress, region));
#if defined(EVACUATOR_DEBUG)
	} else if (isDebugCopy()) {
		_delegate.debugValidateObject(forwardedHeader);
#endif /* defined(EVACUATOR_DEBUG) */
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
	}

	Debug_MM_true(isInSurvivor(forwardedAddress) || isInTenure(forwardedAddress) || (isInEvacuate(forwardedAddress) && isAbortedCycle()));
	return forwardedAddress;
}

void
MM_Evacuator::chain(omrobjectptr_t linkedObject, const uintptr_t selfReferencingSlotOffset, const uintptr_t worklistVolumeCeiling)
{
	while ((NULL != linkedObject) && (_workList.volume() < worklistVolumeCeiling)) {

		/* pull out the slot pointer and test for evacuation */
		GC_SlotObject selfReferencingSlot(_env->getOmrVM(), (fomrobject_t*)(((uintptr_t)linkedObject) + selfReferencingSlotOffset));
		omrobjectptr_t forwardedAddress = selfReferencingSlot.readReferenceFromSlot();
		omrobjectptr_t lastLinkedObject = linkedObject;
		linkedObject = NULL;

		/* break away from chain if it crosses into survivor/tenure space */
		if (isInEvacuate(forwardedAddress)) {

			/* try to copy and forward the slot object */
			MM_ForwardedHeader forwardedHeader(forwardedAddress, _env->compressObjectReferences());
			if (!forwardedHeader.isForwardedPointer()) {

				/* determine receiving region and before and after object size and copy to an outside copyspace and continue chain */
				uintptr_t slotObjectSizeBeforeCopy = 0, slotObjectSizeAfterCopy = 0, hotFieldAlignmentDescriptor = 0;
				_objectModel->calculateObjectDetailsForCopy(_env, &forwardedHeader, &slotObjectSizeBeforeCopy, &slotObjectSizeAfterCopy, &hotFieldAlignmentDescriptor);
				const Region region = isNurseryAge(_objectModel->getPreservedAge(&forwardedHeader)) ? survivor : tenure;
				forwardedAddress = copyOutside(region, &forwardedHeader, selfReferencingSlot.readAddressFromSlot(), slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy, false);
				linkedObject = forwardedAddress;

				Debug_MM_true(lastLinkedObject != linkedObject);

			} else {

				/* just get the forwarding address and break away from chain */
				forwardedAddress = forwardedHeader.getForwardedObject();
			}

			/* update the referring slot with the forwarding address */
			selfReferencingSlot.writeReferenceToSlot(forwardedAddress);
		}

		/* if scanning a tenured object update its remembered state */
		if (isInTenure(lastLinkedObject) && (isInSurvivor(forwardedAddress) || isInEvacuate(forwardedAddress))) {
			setRememberedState(lastLinkedObject, STATE_REMEMBERED);
		}
	}
}

GC_ObjectScanner *
MM_Evacuator::scanner(const bool advanceScanHead)
{
	Debug_MM_true(!isBreadthFirstCondition());
	GC_ObjectScanner *objectScanner = NULL;

	if (!isAbortedCycle()) {

		/* get active object scanner, or NULL if no active scanner for scanspace */
		MM_EvacuatorScanspace* const scanspace = _scanStackFrame;
		objectScanner = scanspace->getActiveObjectScanner();
		uintptr_t scannedBytes = 0;

		/* current object scanner must be finalized before advancing scan head to next object */
		if (advanceScanHead && (NULL != objectScanner)) {

			omrobjectptr_t const scannedObject = scanspace->getActiveObject();

			/* for split array scanspaces the scanned volume always includes a fixed number of elements */
			if (scanspace->isSplitArrayScanspace()) {
				/* extra volume in first segment accounts for header and non-array data in object and may include more elements than subsequent segments */
				if (objectScanner->isHeadObjectScanner()) {
					uintptr_t arrayBytes = getReferenceSlotSize() * ((GC_IndexableObjectScanner *)objectScanner)->getIndexableRange();
					scannedBytes = _objectModel->getConsumedSizeInBytesWithHeader(scannedObject) - arrayBytes;
				}
				/* scan volume for split array data segment is preset when split array workspace is set into scanspace */
				scannedBytes += scanspace->getSplitArrayScanVolume();
			} else {
				/* object size is needed for mixed objects scanners */
				scannedBytes = _objectModel->getConsumedSizeInBytesWithHeader(scannedObject);
			}

			/* update remembered state for parent object */
			if (isInTenure(scannedObject) && scanspace->getRememberedState()) {
				setRememberedState(scannedObject, STATE_REMEMBERED);
			}
			scanspace->clearRememberedState();

			/* advance scan head over scanned object */
			scanspace->advanceScanHead(scannedBytes);

			/* done with scanned object scanner */
			objectScanner = NULL;

			Debug_MM_true(NULL == scanspace->getActiveObjectScanner());
		}

		/* advance scan head over skipped and leaf objects to set up next object scanner, halt copy head */
		while ((NULL == objectScanner) && (scanspace->getScanHead() < scanspace->getCopyHead())) {

			omrobjectptr_t const objectPtr = (omrobjectptr_t)scanspace->getScanHead();
			objectScanner = _delegate.getObjectScanner(objectPtr, scanspace->activateObjectScanner(), GC_ObjectScanner::scanHeap);
			if ((NULL == objectScanner) || objectScanner->isLeafObject()) {

				/* nothing to scan for object at scan head, advance scan head to next object in scanspace and drop object scanner */
				const uintptr_t objectVolume = _objectModel->getConsumedSizeInBytesWithHeader(objectPtr);
				scanspace->advanceScanHead(objectVolume);
				scannedBytes += objectVolume;
				objectScanner = NULL;

			} else if (isScanOptionSelected(recursive_object) && objectScanner->isLinkedObjectScanner()) {

				/* got a linked object scanner -- set recursive object condition to inhibit reflection of outside copy into stack */
				setCondition(recursive_object, true);

				/* chain (copy-forward) its self referencing fields into outside copyspaces */
				uintptr_t selfReferencingSlotCount = 0;
				uintptr_t worklistVolumeCeiling = worklist_volume_ceiling * _controller->_maximumWorkspaceSize;
				const uintptr_t *selfReferencingSlotOffsets = objectScanner->getSelfReferencingSlotOffsets(selfReferencingSlotCount);
				for (uintptr_t slot = 0; (slot < selfReferencingSlotCount) && (_workList.volume() < worklistVolumeCeiling); slot += 1) {
					/* chain this slot until NULL or forwarded linked object or worklist volume explodes */
					chain(objectPtr, selfReferencingSlotOffsets[slot], worklistVolumeCeiling);
				}

				/* clear recursive object condition */
				setCondition(recursive_object, false);

			}
		}

		/* sync indexable object condition flag with scanner */
		if (isScanOptionSelected(indexable_object)) {
			setCondition(indexable_object, (NULL != objectScanner) && objectScanner->isIndexableObject());
		}

		/* update evacuator progress for epoch reporting */
		if (0 < scannedBytes) {
			_volumeMetrics[scanned] += scannedBytes;
			_baseReportingVolumeDelta += scannedBytes;
			if (_baseReportingVolumeDelta >= _volumeReportingThreshold) {
				_baseReportingVolumeDelta = _controller->reportProgress(this, _baseMetrics, _volumeMetrics);
			}
		}
	}

	return objectScanner;
}

bool
MM_Evacuator::reserveInsideScanspace(const Region region, const uintptr_t slotObjectSizeAfterCopy)
{
	Debug_MM_true(evacuate > region);

	MM_EvacuatorWhitespace *whitespace = NULL;

	/* force outside copy while in stack overflow condition */
	if (!isConditionSet(stack_overflow)) {
		
		/* find next frame that can receive whitespace for evacuation region */
		MM_EvacuatorScanspace *nextFrame = next(region, (NULL != _scanStackFrame) ? (_scanStackFrame + 1) : _stackBottom);
		if (NULL != nextFrame) {
		
			/* refresh stack whitespace only if remainder is discardable */
			MM_EvacuatorScanspace *whiteFrame = _whiteStackFrame[region];
			if ((NULL == whiteFrame) || (MM_EvacuatorBase::max_scanspace_remainder >= whiteFrame->getWhiteSize())) {

				/* trim white frame remainder to whitelist (it will be discarded and set as heap hole) */
				if (NULL != whiteFrame) {
					_whiteList[region].add(whiteFrame->trim());
				}

				/* try to allocate from whitelist if top is large enough to bother with and will hold object */
				if (_whiteList[region].top() >= OMR_MAX(slotObjectSizeAfterCopy, min_workspace_release)) {
					/* this is whitespace from unused large object reservations or the tails of trimmed copyspaces*/
					whitespace = _whiteList[region].top(slotObjectSizeAfterCopy);
					Debug_MM_true((NULL != whitespace) && (slotObjectSizeAfterCopy <= whitespace->length()));

				} else {
					/* update controller view of aggregate copied volume for slightly more accurate estimation of remaining volume */
					_baseReportingVolumeDelta = _controller->reportProgress(this, _baseMetrics, _volumeMetrics);
					/* try to allocate whitespace (tlh) from the evacuation region */
					whitespace = _controller->getWhitespace(this, region, slotObjectSizeAfterCopy);
					Debug_MM_true((NULL == whitespace) || (slotObjectSizeAfterCopy <= whitespace->length()));
				}
			}

			if (NULL != whitespace) {

				/* set whitespace into next stack frame */
				nextFrame->setScanspace(region, whitespace->getBase(), whitespace->getBase(), whitespace->length(), whitespace->isLOA());
				/* stack whitespace for evacuation region is installed in next stack frame */
				_whiteStackFrame[region] = nextFrame;
		
				/* receiving frame is holding enough stack whitespace to copy object to evacuation region */
				Debug_MM_true((survivor == _whiteStackFrame[survivor]->getEvacuationRegion()) || (0 == _whiteStackFrame[survivor]->getSize()));
				Debug_MM_true((tenure == _whiteStackFrame[tenure]->getEvacuationRegion()) || (0 == _whiteStackFrame[tenure]->getSize()));
				Debug_MM_true(region == nextFrame->getEvacuationRegion());
				Debug_MM_true(slotObjectSizeAfterCopy <= nextFrame->getWhiteSize());
			}
		}
	}

	/* failover to outside copy if no whitespace */
	return (NULL != whitespace);
}

MM_EvacuatorCopyspace *
MM_Evacuator::reserveOutsideCopyspace(Region *region, const uintptr_t slotObjectSizeAfterCopy, bool useLargeCopyspace)
{
	Debug_MM_true(evacuate > *region);

	MM_EvacuatorCopyspace *copyspace = NULL;
	MM_EvacuatorWhitespace *whitespace = NULL;

	/* preference order of potential evacuation regions */
	Region regionPreference[] = { *region, otherEvacuationRegion(*region) };

	/* force solo whitespace allocation for large objects (including all splitable pointer arrays) */
	if (!useLargeCopyspace) {

		/* select outside copyspaces to try in order of region preference */
		for (uintptr_t preference = 0; preference < 2; preference += 1) {

			const Region preferred = *region = regionPreference[preference];

			/* try to reserve whitespace in copyspace to receive object, refreshing if possible */
			const uintptr_t copyspaceRemainder = _copyspace[preferred].getWhiteSize();
			if (copyspaceRemainder >= slotObjectSizeAfterCopy) {

				/* use outside copyspace for evacuation region if object will fit */
				return &_copyspace[preferred];
			}

			/* outside copyspaces can be refreshed only if the remainder whitespace size is below threshold for trimming */
			if (shouldRefreshCopyspace(preferred, slotObjectSizeAfterCopy, copyspaceRemainder)) {

				/* try to refresh whitespace for copyspace */
				if (_whiteList[preferred].top() >= OMR_MAX(slotObjectSizeAfterCopy, _controller->_minimumCopyspaceSize)) {

					/* take top of whitelist and swap remainder whitespace from copyspace into whitelist */
					whitespace = _whiteList[preferred].top(slotObjectSizeAfterCopy, _copyspace[preferred].trim());
					Debug_MM_true((NULL == whitespace) || (sizeof(MM_EvacuatorWhitespace) <= whitespace->length()));

				} else {

					/* update controller view of aggregate copied volume for slightly more accurate estimation of remaining volume */
					_baseReportingVolumeDelta = _controller->reportProgress(this, _baseMetrics, _volumeMetrics);
					/* try to allocate whitespace from the evacuation region */
					whitespace = _controller->getWhitespace(this, preferred, slotObjectSizeAfterCopy);
					Debug_MM_true((NULL == whitespace) || (sizeof(MM_EvacuatorWhitespace) <= whitespace->length()));
				}

				if (NULL != whitespace) {

					/* clear copyspace overflow flush condition for evacuation region */
					setCondition(copyspaceTailFillCondition(preferred), false);
					_copyspaceOverflow[preferred] = 0;

					/* break to set whitespace into copyspace */
					break;
				}
			}

			/* whitespace in outside copyspace is too small for object and too large to trim */
			if (!isLargeObject(slotObjectSizeAfterCopy)) {

				/* copyspace tail filling condition forces small objects to be copied to copyspace until it can be refreshed */
				setCondition(copyspaceTailFillCondition(preferred), true);
				/* force copyspace trim and refresh later if an excessive volume overflows before tail filling completes */
				_copyspaceOverflow[preferred] += slotObjectSizeAfterCopy;

				/* if inside copying not inhibited try redirecting copy into stack whitespace */
				if (!isBreadthFirstCondition() && !isConditionSet(recursive_object) && (slotObjectSizeAfterCopy <= _whiteStackFrame[preferred]->getWhiteSize())) {
					/* copy into preferred region whitespace reserved for copying inside stack scanspaces */
					return _whiteStackFrame[preferred]->asCopyspace();
				}

				/* loop to try other evacuation region */
				continue;

			} else {

				/* break to force whitespace allocation for solo object copy */
				break;
			}
		}
	}

	Debug_MM_true(NULL == copyspace);

	/* if no whitespace try to allocate for solo object copy */
	if (NULL == whitespace) {

		/* force use of large object copyspace */
		useLargeCopyspace = true;

		for (uintptr_t preference = 0; (NULL == whitespace) && (preference < 2); preference += 1) {

			/* if whitelist top is taken object excess whitespace will be trimmed back to whitelist after object copy is complete */
			const Region preferred = *region = regionPreference[preference];
			whitespace = _whiteList[preferred].top(slotObjectSizeAfterCopy);
			Debug_MM_true((NULL == whitespace) || (sizeof(MM_EvacuatorWhitespace) <= whitespace->length()));
			if (NULL == whitespace) {

				/* this will allocate just enough whitespace to receive object copy */
				whitespace = _controller->getObjectWhitespace(this, preferred, slotObjectSizeAfterCopy);
				Debug_MM_true((NULL == whitespace) || (sizeof(MM_EvacuatorWhitespace) <= whitespace->length()));
			}
		}
	}

	/* at this point no whitespace to refresh copyspace means abort collection */
	if (NULL != whitespace) {
		Debug_MM_true(slotObjectSizeAfterCopy <= whitespace->length());
		Debug_MM_true(*region == getEvacuationRegion(whitespace->getBase(), *region));

		/* pull whitespace from evacuation region into outside or large copyspace */
		if (useLargeCopyspace) {

			/* select large copyspace to receive whitespace */
			copyspace = &_largeCopyspace;

		} else {

			/* select and clear outside copyspace to receive whitespace */
			copyspace = &_copyspace[*region];

			/* trim remainder whitespace from copyspace to whitelist */
			_whiteList[*region].add(copyspace->trim());

			/* distribute any work in copyspace to worklist */
			if (0 < copyspace->getCopySize()) {
				addWork(copyspace);
			}
		}

		/* set up selected copyspace with reserved whitespace to receive object copy */
		copyspace->setCopyspace(*region, whitespace->getBase(), whitespace->getBase(), whitespace->length(), whitespace->isLOA());

	} else {

		/* broadcast abort condition to other evacuators */
		setAbortedCycle();

		/* record every allocation failure after abort condition raised */
		if (survivor == regionPreference[0]) {
			_stats->_failedFlipCount += 1;
			_stats->_failedFlipBytes += slotObjectSizeAfterCopy;
		} else {
			_stats->_failedTenureCount += 1;
			_stats->_failedTenureBytes += slotObjectSizeAfterCopy;
			_stats->_failedTenureLargest = OMR_MAX(slotObjectSizeAfterCopy, _stats->_failedTenureLargest);
		}

		/* abort object evacuation */
		*region = unreachable;
		copyspace = NULL;
	}

	return copyspace;
}

bool
MM_Evacuator::shouldRefreshCopyspace(const Region region, const uintptr_t slotObjectSizeAfterCopy, const uintptr_t copyspaceRemainder) const
{
	/* do not refresh outside copyspace for huge objects -- these are always directed to large object copyspace */
	if (!isHugeObject(slotObjectSizeAfterCopy)) {

		/* refresh when copyspace has a discardable whitespace remainder */
		if (MM_EvacuatorBase::max_copyspace_remainder >= copyspaceRemainder) {
			return true;
		}

		/* refresh when scanning a pointer array */
		if (hasScanWork() && _scanStackFrame->getActiveObjectScanner()->isIndexableObject()) {
			/* remaining array elements will likewise overflow copyspace remainder */
			return true;
		}

		/* refresh when a large volume of copy has overflowed the copyspace */
		if (_copyspaceOverflow[region] > (min_workspace_release * MM_EvacuatorBase::max_copyspace_overflow_quanta)) {
			/* this will turn outside copyspace overflow flushing off */
			return true;
		}
	}

	return false;
}

bool
MM_Evacuator::isLargeObject(const uintptr_t objectSizeInBytes) const
{
	/* any object that might be a splitable pointer array is a very large object */
	return (_controller->_minimumCopyspaceSize <= objectSizeInBytes);
}

bool
MM_Evacuator::isHugeObject(const uintptr_t objectSizeInBytes) const
{
	/* any object that can't fit in a maximal copyspace is a huge object */
	return (_controller->_maximumCopyspaceSize <= objectSizeInBytes);
}

bool
MM_Evacuator::isSplitablePointerArray(MM_ForwardedHeader *forwardedHeader, const uintptr_t objectSizeInBytes)
{
	/* only very large (or huge) pointer arrays can be split into segments */
	if (isLargeObject(objectSizeInBytes)) {
		/* delegate may veto splitting here */
		return _delegate.isIndexablePointerArray(forwardedHeader);
	}

	return false;
}

void
MM_Evacuator::splitPointerArrayWork(MM_EvacuatorCopyspace *copyspace)
{
	uintptr_t elements = 0;
	omrobjectptr_t pointerArray = (omrobjectptr_t)copyspace->getBase();
	_delegate.getIndexableDataBounds(pointerArray, &elements);

	/* distribute elements to segments as evenly as possible and take largest segment first */
	uintptr_t elementsPerSegment = _controller->_minimumCopyspaceSize / getReferenceSlotSize();
	uintptr_t segments = elements / elementsPerSegment;
	if (0 != (elements % elementsPerSegment)) {
		segments += 1;
	}
	elementsPerSegment = elements / segments;
	uintptr_t elementsThisSegment = elementsPerSegment + (elements % segments);

	/* record 1-based array offsets to mark split array workspaces */
	MM_EvacuatorWorklist::List worklist;
	worklist.head = worklist.tail = NULL;
	uintptr_t offset = 1;
	while (0 < segments) {

		/* wrap each segment in a split array workspace */
		MM_EvacuatorWorkspace* work = _freeList.next();
		work->base = pointerArray;
		work->offset = offset;
		work->length = elementsThisSegment;
		work->next = NULL;

		/* append split array workspace to worklist */
		_workList.addWorkToList(&worklist, work);

		/* all split array workspaces but the first have equal volumes of work */
		offset += elementsThisSegment;
		elementsThisSegment = elementsPerSegment;
		segments -= 1;
	}

	/* rebase copyspace base to copy head to clear copied array */
	uintptr_t volume;
	copyspace->rebase(&volume);

	/* defer segmented workspaces in worklist */
	addWork(worklist.head, true);
}

bool
MM_Evacuator::isSplitArrayWorkspace(const MM_EvacuatorWorkspace *work) const
{
	/* offset in workspace is >0 only for split array workspaces (segment starts at array[offset-1]) */
	return (0 < work->offset);
}

bool
MM_Evacuator::shouldRememberObject(omrobjectptr_t objectPtr)
{
	/* this is a clone of MM_Scavenger::shouldRememberObject(), inaccessible here */
	Debug_MM_true((NULL != objectPtr) && isInTenure(objectPtr));
	Debug_MM_true(_objectModel->getRememberedBits(objectPtr) < (uintptr_t)0xc0);

	/* scan object for referents in the nursery */
	GC_ObjectScannerState objectScannerState;
	GC_ObjectScanner *objectScanner = _delegate.getObjectScanner(objectPtr, &objectScannerState, GC_ObjectScanner::scanRoots);
	if (NULL != objectScanner) {

		GC_SlotObject *slotPtr;
		while (NULL != (slotPtr = objectScanner->getNextSlot())) {

			omrobjectptr_t slotObjectPtr = slotPtr->readReferenceFromSlot();
			if (NULL != slotObjectPtr) {

				if (isInSurvivor(slotObjectPtr)) {
					return true;
				}

				Debug_MM_true(isInTenure(slotObjectPtr) || (isInEvacuate(slotObjectPtr) && isAbortedCycle()));
			}
		}
	}

	/* the remembered state of a class object also depends on the class statics */
	if (_objectModel->hasIndirectObjectReferents((CLI_THREAD_TYPE*)_env->getLanguageVMThread(), objectPtr)) {

		return _delegate.objectHasIndirectObjectsInNursery(objectPtr);
	}

	return false;
}

bool
MM_Evacuator::setRememberedState(omrobjectptr_t object, uintptr_t rememberedState)
{
	/* this is a clone of MM_Scavenger::rememberObject(), inaccessible here */
	Debug_MM_true(isInTenure(object));
	Debug_MM_true(_objectModel->getRememberedBits(object) < (uintptr_t)0xc0);

	/* atomically test-set the referenced/remembered bits in the flags field of the object header slot */
	bool setByThisEvacuator = false;
	switch (rememberedState) {
	case OMR_TENURED_STACK_OBJECT_CURRENTLY_REFERENCED:
		setByThisEvacuator = _objectModel->atomicSwitchReferencedState(object, OMR_TENURED_STACK_OBJECT_CURRENTLY_REFERENCED);
		break;
	case STATE_REMEMBERED:
		setByThisEvacuator = _objectModel->atomicSetRememberedState(object, STATE_REMEMBERED);
		break;
	default:
		Assert_MM_unreachable();
		break;
	}

	/* if referenced/remembered bits were not identically set before test-set add object to remembered set */
	if (setByThisEvacuator) {
		Debug_MM_true(_objectModel->isRemembered(object));
		/* allocate an entry in the remembered set */
		if ((_env->_scavengerRememberedSet.fragmentCurrent < _env->_scavengerRememberedSet.fragmentTop) ||
				(0 == allocateMemoryForSublistFragment(_env->getOmrVMThread(), (J9VMGC_SublistFragment*)&_env->_scavengerRememberedSet))
		) {
			/* there is at least 1 free entry in the fragment - use it */
			uintptr_t *rememberedSetEntry = _env->_scavengerRememberedSet.fragmentCurrent++;
			*rememberedSetEntry = (uintptr_t)object;
			_env->_scavengerRememberedSet.count++;
		} else {
			/* failed to allocate a fragment - set the remembered set overflow state */
			if (!_env->getExtensions()->isRememberedSetInOverflowState()) {
				_stats->_causedRememberedSetOverflow = 1;
			}
			_env->getExtensions()->setRememberedSetOverflowState();
		}
	}

	Debug_MM_true(_objectModel->getRememberedBits(object) < (uintptr_t)0xc0);
	return setByThisEvacuator;
}

void
MM_Evacuator::flushRememberedSet()
{
	/* flush thread-local remembered set cache to collector remembered set */
	if (0 != _env->_scavengerRememberedSet.count) {
		_env->flushRememberedSet();
	}
}

void
MM_Evacuator::flushForWaitState()
{
	/* flushed cached artifacts before acquiring controller mutex to find work */
	flushRememberedSet();
	_delegate.flushForWaitState();
}

bool
MM_Evacuator::isNurseryAge(uintptr_t objectAge) const
{
	/* return true if age NOT selected by collector's tenure mask */
	return (0 == (((uintptr_t)1 << objectAge) & _tenureMask));
}

bool
MM_Evacuator::flushInactiveFrames()
{
	/* use a local list to minimize the number of time the evacuator mutex must be taken */
	MM_EvacuatorWorklist::List worklist;
	worklist.head = worklist.tail = NULL;

	/* cut work from inactive scanspaces below first inactive scanspace to the deferred work list */
	uintptr_t inactive = 0;
	for (MM_EvacuatorScanspace *frame = _scanStackFrame - 1; frame >= _stackBottom; frame -= 1) {
		/* split array scanspaces appear to be active always so take them only after a superior inactive mixed object scanspace has been located */
		Debug_MM_true(((0 < frame->getActivationCount()) || ((frame != _whiteStackFrame[survivor]) && (frame != _whiteStackFrame[tenure]))) || (0 == frame->getWorkSize()));
		if ((0 == frame->getActivationCount()) && ((0 < frame->getWorkSize()) || frame->isSplitArrayScanspace())) {
			if (0 < inactive) {
				MM_EvacuatorWorkspace *workspace = _freeList.next();
				workspace->base = frame->cutWork(&workspace->length, &workspace->offset);
				_workList.addWorkToList(&worklist, workspace);
			}
			if (!frame->isSplitArrayScanspace()) {
				inactive += 1;
			}
		}
	}

	/* add workspaces to deferred worklist */
	bool flushedWork = (NULL != worklist.head);
	if (flushedWork) {
		addWork(worklist.head, true);
	}
	return flushedWork;
}

bool
MM_Evacuator::flushOutsideCopyspaces()
{
	/* use a local list to minimize the number of time the evacuator mutex must be taken */
	MM_EvacuatorWorklist::List worklist;
	worklist.head = worklist.tail = NULL;

	/* cut extant stack overflow work from outside copyspaces to workspaces and add (defer) to worklist */
	for (Region region = survivor; region <= tenure; region = nextEvacuationRegion(region)) {
		if (_copyspace[region].isStackOverflow()) {
			if (0 < _copyspace[region].getCopySize()) {
				MM_EvacuatorWorkspace *workspace = _freeList.next();
				workspace->base = (omrobjectptr_t)_copyspace[region].rebase(&workspace->length);
				_workList.addWorkToList(&worklist, workspace);
			}
			_copyspace[region].setStackOverflow(false);
		}
	}

	/* add workspaces to default or deferred worklist */
	bool flushedWork = (NULL != worklist.head);
	if (flushedWork) {
		addWork(worklist.head, true);
	}
	return flushedWork;
}

void
MM_Evacuator::setCondition(MM_Evacuator::ConditionFlag condition, bool value)
{
	Debug_MM_true(((uintptr_t)1 << MM_Math::floorLog2((uintptr_t)condition)) == (uintptr_t)condition);

	/* censor idempotent conditions and optional conditions not selected for runtime */
	if ((0 == (_evacuatorScanOptions & condition)) || (value == isConditionSet(condition))) {
		return;
	}

	/* set or reset specific condition flag(s) */
	if (value) {
		_conditionFlags |= (uintptr_t)condition;
	} else {
		_conditionFlags &= ~(uintptr_t)condition;
	}

	bool notify = false;

	/* synchronize stack limit with outside copy conditions */
	const MM_EvacuatorScanspace * const stackFloor = _stackBottom + 1;
	if (isConditionSet(stall) ^ (_stackLimit == stackFloor)) {

		/* maintain stack limit just above stack bottom while other evacuators are stalling */
		_stackLimit = (_stackLimit == stackFloor) ? _stackCeiling : stackFloor;

		/* when stall condition is first raised send a one-time notification if this evacuator has distributable work */
		if ((_stackLimit == stackFloor) && (NULL != _scanStackFrame)) {
			notify |= flushInactiveFrames();
		}
	}

	/* synchronize workspace release threshold with scanning stage and work distribution conditions */
	if (isDistributeWorkCondition() ^ (min_workspace_release == _workspaceReleaseThreshold)) {

		/* adjust threshold for releasing work from outside copyspaces */
		_workspaceReleaseThreshold = adjustWorkReleaseThreshold();

		/* cut small workspaces and defer work copied to copyspace in stack overflow condition */
		if ((min_workspace_release == _workspaceReleaseThreshold) &&
			((min_workspace_release <= _copyspace[survivor].getCopySize()) || (min_workspace_release <= _copyspace[tenure].getCopySize()))
		) {
			notify |= flushOutsideCopyspaces();
		}
	}

	if (notify) {
		gotWork();
	}
}

bool
MM_Evacuator::isBreadthFirstCondition() const
{
	/* this tests conditions that force breadth first stack operation  */
	return (isConditionSet(breadth_first_always | breadth_first_roots));
}

bool
MM_Evacuator::isDistributeWorkCondition() const
{
	/* test conditions that force small workspace release and promote distribution of work to other evacuators */
	return isConditionSet(breadth_first_roots | stall | stack_overflow);
}

bool
MM_Evacuator::isForceOutsideCopyCondition() const
{
	/* test stack overflow and copyspace tail filling conditions, which force all objects to be copied outside */
	return (isConditionSet(outside_mask));
}

bool
MM_Evacuator::isForceOutsideCopyCondition(MM_Evacuator::Region region) const
{
	/* test stack overflow and copyspace tail filling conditions, which force all objects to be copied outside */
	return (isConditionSet(copyspaceTailFillCondition(region) | outside_mask));
}

MM_Evacuator::ConditionFlag
MM_Evacuator::copyspaceTailFillCondition(MM_Evacuator::Region region) const
{
	/* return copyspace overflow condition for the specific region */
	return (ConditionFlag)((uintptr_t)survivor_tail_fill << region);
}

void
MM_Evacuator::receiveWhitespace(Region region, MM_EvacuatorWhitespace *whitespace)
{
	/* this is tlh whitespace reserved but not used for copy */
	_whiteList[region].add(whitespace);
}

void
MM_Evacuator::flushWhitespace(MM_Evacuator::Region region)
{
	/* recycle survivor whitespace back into the memory pool at end of gc cycle, recycle tenure at start of global gc */
	_whiteList[region].flush();

	/* whitelist should be empty, top() should be NULL */
	Debug_MM_true(NULL == _whiteList[region].top(0));
}

void
MM_Evacuator::setAbortedCycle()
{
	/* test and set controller abort condition for all evacuators to see */
	if (!_controller->setAborting()) {

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		/* report only if this evacuator is first to set the abort condition */
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("%5lu %2lu %2lu:     abort; flags:%lx", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex, _controller->sampleEvacuatorFlags());
		if (hasScanWork()) {
			omrtty_printf("; scanning:0x%lx", (uintptr_t)_scanStackFrame->getScanHead());
		}
		omrtty_printf("\n");
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

	}

	_abortedCycle = true;
}

bool
MM_Evacuator::isAbortedCycle()
{
	/* also check with controller in case other evacuator has signaled abort */
	if (!_abortedCycle && _controller->isAborting()) {
		_abortedCycle = true;
	}

	return _abortedCycle;
}

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
uint64_t
MM_Evacuator::startWaitTimer(const char *tag)
{
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
	OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) */
#if defined(EVACUATOR_DEBUG)
	if (isDebugWork()) {
		uintptr_t flags = _controller->sampleEvacuatorFlags();
		omrtty_printf("%5lu %2lu %2lu:%10s; flags:%lx; ", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex, tag, flags);
		_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
		_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
		omrtty_printf("; stacked: %lx; vow:%lx\n", (hasScanWork() ? _scanStackFrame->getWorkSize() : 0), _workList.volume());
	}
#endif /* defined(EVACUATOR_DEBUG) */
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
	return omrtime_hires_clock();
#else
	return 0;
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) */
}

void
MM_Evacuator::endWaitTimer(uint64_t waitStartTime)
{
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
	OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
	uint64_t waitEndTime = omrtime_hires_clock();
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) */

#if defined(EVACUATOR_DEBUG)
	if (isDebugWork()) {
		uintptr_t stackVolume = 0;
		if (hasScanWork()) {
			for (MM_EvacuatorScanspace *frame = _scanStackFrame; frame <= _stackBottom; frame -= 1) {
				stackVolume += frame->getWorkSize();
			}
		}
		uintptr_t flags = _controller->sampleEvacuatorFlags();
		uint64_t waitMicros = omrtime_hires_delta(waitStartTime, waitEndTime, OMRPORT_TIME_DELTA_IN_MICROSECONDS);
		omrtty_printf("%5lu %2lu %2lu:     resume; flags:%lx; ", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex, flags);
		_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
		_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
		omrtty_printf("; stacked: %lx; vow:%lx; micros: %llu\n", stackVolume, _workList.volume(), waitMicros);
	}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS)
	if (hasScanWork()) {
		_stats->addToWorkStallTime(waitStartTime, waitEndTime);
	} else {
		_stats->addToCompleteStallTime(waitStartTime, waitEndTime);
	}
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) */
}
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

#if defined(EVACUATOR_DEBUG)
void
MM_Evacuator::debugStack(const char *stackOp, bool treatAsWork)
{
	if (isDebugStack() || (treatAsWork && (isDebugWork() || isDebugBackout()))) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		MM_EvacuatorScanspace *scanspace = hasScanWork() ? _scanStackFrame : _stackBottom;
		Region region = scanspace->getEvacuationRegion();
		char isWhiteFrame = (evacuate > region) && (_whiteStackFrame[region] == scanspace) ? '*' : ' ';
		const char *whiteRegion = (survivor == region) ? "survivor" : "tenure";
		omrtty_printf("%5lu %2lu %2lu:%6s[%2d];%cbase:%lx; copy:%lx; end:%lx; %s:%lx; scan:%lx; unscanned:%lx; sow:%lx; tow:%lx\n",
				_controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex, stackOp, (scanspace - _stackBottom),
				isWhiteFrame, (uintptr_t)scanspace->getBase(), (uintptr_t)scanspace->getCopyHead(), (uintptr_t)scanspace->getEnd(),
				whiteRegion, scanspace->getWhiteSize(), (uintptr_t)scanspace->getScanHead(), scanspace->getWorkSize(),
				_copyspace[survivor].getCopySize(), _copyspace[tenure].getCopySize());
	}
}

void
MM_Evacuator::checkSurvivor()
{
	OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
	omrobjectptr_t object = (omrobjectptr_t)(_heapBounds[survivor][0]);
	omrobjectptr_t end = (omrobjectptr_t)(_heapBounds[survivor][1]);
	while (isInSurvivor(object)) {
		while (isInSurvivor(object) && _objectModel->isDeadObject(object)) {
			object = (omrobjectptr_t)((uintptr_t)object + _objectModel->getSizeInBytesDeadObject(object));
		}
		if (isInSurvivor(object)) {
			uintptr_t size = _objectModel->getConsumedSizeInBytesWithHeader(object);
			_delegate.debugValidateObject(object, size);
			object = (omrobjectptr_t)((uintptr_t)object + size);
		}
	}
	omrtty_printf("%5lu %2lu %2lu:  survivor; end:%lx\n", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex, (uintptr_t)object);
	Debug_MM_true(object == end);
}

void
MM_Evacuator::checkTenure()
{
	OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
	MM_GCExtensionsBase *extensions = _env->getExtensions();
	omrobjectptr_t object = (omrobjectptr_t)(_heapBounds[tenure][0]);
	omrobjectptr_t end = (omrobjectptr_t)(_heapBounds[tenure][1]);
	while (extensions->isOld(object)) {
		while (extensions->isOld(object) && _objectModel->isDeadObject(object)) {
			object = (omrobjectptr_t)((uintptr_t)object + _objectModel->getSizeInBytesDeadObject(object));
		}
		if (extensions->isOld(object)) {
			uintptr_t size = _objectModel->getConsumedSizeInBytesWithHeader(object);
			_delegate.debugValidateObject(object, size);
			Debug_MM_true(_objectModel->getRememberedBits(object) < (uintptr_t)0xc0);
			if (_objectModel->isRemembered(object)) {
				if (!shouldRememberObject(object)) {
					omrtty_printf("%5lu %2lu %2lu:downgraded; object:%lx; flags:%lx\n", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex, (uintptr_t)object, _objectModel->getObjectFlags(object));
				}
			} else if (shouldRememberObject(object)) {
				omrtty_printf("%5lu %2lu %2lu: !remember; object:%lx; flags:%lx\n", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex, (uintptr_t)object, _objectModel->getObjectFlags(object));
				Debug_MM_true(isAbortedCycle());
			}
			object = (omrobjectptr_t)((uintptr_t)object + size);
		}
	}
	omrtty_printf("%5lu %2lu %2lu:    tenure; end:%lx\n", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex, (uintptr_t)object);
	Debug_MM_true(object == end);
}
#endif /* defined(EVACUATOR_DEBUG) */
