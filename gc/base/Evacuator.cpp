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

#include "CollectorLanguageInterface.hpp"
#include "EnvironmentStandard.hpp"
#include "Evacuator.hpp"
#include "EvacuatorController.hpp"
#include "EvacuatorDelegate.hpp"
#include "ForwardedHeader.hpp"
#include "GCExtensionsBase.hpp"
#include "IndexableObjectScanner.hpp"
#include "Math.hpp"
#include "MemcheckWrapper.hpp"
#include "ObjectModel.hpp"
#include "ScavengerStats.hpp"
#include "SlotObject.hpp"
#include "SublistFragment.hpp"

extern "C" {
	uintptr_t allocateMemoryForSublistFragment(void *vmThreadRawPtr, J9VMGC_SublistFragment *fragmentPrimitive);
}

MM_Evacuator *
MM_Evacuator::newInstance(uintptr_t workerIndex, MM_EvacuatorController *controller, MM_GCExtensionsBase *extensions)
{
	MM_Evacuator *evacuator = (MM_Evacuator *)extensions->getForge()->allocate(sizeof(MM_Evacuator), OMR::GC::AllocationCategory::FIXED, OMR_GET_CALLSITE());
	if (NULL != evacuator) {
		new(evacuator) MM_Evacuator(workerIndex, controller, extensions);
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
	Debug_MM_true((_maxStackDepth > 1) || isConditionSet(breadth_first_always));

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
	_stats->_cycleVolumeMetrics[inside] = _stats->_cycleVolumeMetrics[outside] = _stats->_cycleVolumeMetrics[scanned] = 0;
	_stackVolumeMetrics[inside] = _stackVolumeMetrics[outside] = _stackVolumeMetrics[scanned] = 0;
	_copyspaceOverflow[survivor] = _copyspaceOverflow[tenure] = 0;
	_copiedBytesDelta[survivor] = _copiedBytesDelta[tenure] = 0;
	_copiedBytesReportingDelta = copiedBytesReportingDelta;
	_scannedBytesDelta = 0;
	_abortedCycle = false;

	/* set up whitespaces for the cycle */
	_whiteList[survivor].bind(_env, _workerIndex, _controller->getMemorySubspace(survivor), false);
	_whiteList[tenure].bind(_env, _workerIndex, _controller->getMemorySubspace(tenure), true);

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

	/* initialize whitespace frame pointers empty and high in the stack to clear bottom frames for work */
	_scanStackFrame = _stackTop;
	for (Region region = survivor; region <= tenure; region = nextEvacuationRegion(region)) {
		_scanStackFrame -= 1;
		_scanStackFrame->reset(true);
		_scanStackFrame->setScanspace(_heapBounds[region][0], _heapBounds[region][0], 0);
		_whiteStackFrame[region] = _scanStackFrame;
	}

	/* reset remaining stack frames and set scan/limit frame pointers */
	while (_stackBottom <_scanStackFrame) {
		_scanStackFrame -= 1;
		_scanStackFrame->reset(true);
	}
	_stackLimit = _stackCeiling;
	_scanStackFrame = NULL;

	/* set scan options into initial conditions and start with workspace release threshold */
	_conditionFlags = _evacuatorScanOptions & static_mask;
	_workspaceReleaseThreshold = _controller->_minimumWorkspaceSize;

	/* scan roots and remembered set and objects depending from these */
	scanRemembered();
	scanRoots();
	scanHeap();

	/* trick to obviate a write barrier -- objects tenured this cycle are always remembered */
	if (!isAbortedCycle() && _controller->isEvacuatorFlagSet(MM_EvacuatorController::rescanThreadSlots)) {
		_delegate.rescanThreadSlots();
		flushRememberedSet();
	}

	/* scan clearable objects -- this may involve 0 or more phases, with evacuator threads joining in scanHeap() at end of each phase */
	while (!isAbortedCycle() && _delegate.hasClearable()) {

		/* java root clearer has repeated phases involving *deprecated* evacuateHeap() and its delegated scanClearable() leaves no unscanned work */
		if (scanClearable()) {

			/* other language runtimes should use evacuateObject() only in delegated scanClearble() and allow scanHeap() to complete each delegated phase */
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
		_copyspace[region].reset();
	}

#if defined(EVACUATOR_DEBUG)
	_whiteList[survivor].verify();
	_whiteList[tenure].verify();
#endif /* defined(EVACUATOR_DEBUG) */

	/* reset large copyspace (it is void of work and whitespace at this point) */
	Debug_MM_true(0 == _largeCopyspace.getCopySize());
	Debug_MM_true(0 == _largeCopyspace.getWhiteSize());
	_largeCopyspace.reset();

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
	Debug_MM_true(NULL != _whiteStackFrame[tenure]);

	/* failure to evacuate will return original pointer to evacuate region */
	omrobjectptr_t forwardedAddress = forwardedHeader->getObject();

	if (!isAbortedCycle()) {

		/* try to copy and forward object unless already done by other evacuator */
		if (!forwardedHeader->isForwardedPointer()) {

			/* clear stack overflow conditions */
			setCondition(stack_overflow, false);
			setCondition(depth_first, false);

			/* set flush condition for this object to force outside copy with no option to overflow into stack scanspace */
			bool breadthFirstRoot = isConditionSet(breadth_first_roots);
			setCondition(breadth_first_roots, breadthFirstRoot || breadthFirst);

			/* slot object must be evacuated -- determine before and after object size and which evacuation region should receive this object */
			uintptr_t slotObjectSizeAfterCopy = 0, slotObjectSizeBeforeCopy = 0, hotFieldAlignmentDescriptor = 0;
			_objectModel->calculateObjectDetailsForCopy(_env, forwardedHeader, &slotObjectSizeBeforeCopy, &slotObjectSizeAfterCopy, &hotFieldAlignmentDescriptor);
			Region const region = isNurseryAge(_objectModel->getPreservedAge(forwardedHeader)) ? survivor : tenure;

			/* direct copy to outside copyspace if forcing breadth first or object is very large (eg split array pointer) */
			MM_EvacuatorScanspace *rootFrame = NULL;
			if (!isBreadthFirstCondition() && !isLargeObject(slotObjectSizeAfterCopy)) {

				/* select stack frame near bottom of stack to hold whitespace for inside copy */
				rootFrame = next(_stackBottom, region);

				/* if inside copying allowed copy to root frame and push */
				if (NULL != rootFrame) {

					/* ensure capacity in evacuation region stack whitespace */
					MM_EvacuatorScanspace *whiteFrame = _whiteStackFrame[region];
					if (slotObjectSizeAfterCopy <= whiteFrame->getWhiteSize()) {

						/* fits in remaining stack whitespace for evacuation region so pull whitespace into root stack frame */
						if (rootFrame != whiteFrame) {
							rootFrame->pullWhitespace(whiteFrame);
						}

					} else {

						/* does not fit in stack whitespace for evacuation region -- try to refresh */
						MM_EvacuatorWhitespace *whitespace = refreshInsideWhitespace(region, slotObjectSizeAfterCopy);
						if (NULL != whitespace) {
							/* set whitespace into root stack frame */
							rootFrame->setScanspace(whitespace->getBase(), whitespace->getBase(), whitespace->length(), whitespace->isLOA());
						} else {
							/* failover to outside copy */
							rootFrame = NULL;
						}
					}

					/* if receiving frame is holding enough stack whitespace to copy object to evacuation region */
					if (NULL != rootFrame) {
						Debug_MM_true(region == getEvacuationRegion(rootFrame->getBase()));
						Debug_MM_true(slotObjectSizeAfterCopy <= rootFrame->getWhiteSize());

						/* stack whitespace for evacuation region is installed in root stack frame */
						_whiteStackFrame[region] = rootFrame;

						/* copy and forward object to bottom frame (we don't know the referring address so just say NULL) */
						omrobjectptr_t const copyHead = (omrobjectptr_t)rootFrame->getCopyHead();
						forwardedAddress = copyForward(forwardedHeader, NULL, rootFrame, slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy);
						if (forwardedAddress == copyHead) {

							/* object copied into root frame, so push it for scanning on stack */
							push(rootFrame);
						}
					}
				}
			}

			/* if not copied try outside or large object copyspaces (or stack scanspace unless breadth_first_roots condition is set) */
			if (NULL == rootFrame) {

				/* if copy is redirected to inside whitespace this will push receiving frame to force scanning on stack */
				forwardedAddress = copyOutside(region, forwardedHeader, NULL, slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy);
			}

			/* restore original breadth first root flush condition after copy is complete */
			Debug_MM_true(!isConditionSet(breadth_first_roots) || !hasScanWork());
			setCondition(breadth_first_roots, breadthFirstRoot);

			/* scan stack until empty */
			if (hasScanWork()) {
				scan();
			}

			Debug_MM_true(!hasScanWork());
			Debug_MM_true(0 == _stackBottom->getWorkSize());

		} else {
			Debug_MM_true(forwardedHeader->isForwardedPointer());

			/* return pointer to object copied and forwarded by other evacuator */
			forwardedAddress = forwardedHeader->getForwardedObject();
		}
	}

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
	if (!_env->getExtensions()->isConcurrentScavengerEnabled()) {
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
}

void
MM_Evacuator::rescanThreadSlot(omrobjectptr_t *objectPtrIndirect)
{
	omrobjectptr_t objectPtr = *objectPtrIndirect;
	if (isInEvacuate(objectPtr)) {
		/* the slot is still pointing at evacuate memory. This means that it must have been left unforwarded
		 * in the first pass so that we would process it here.
		 */
		MM_ForwardedHeader forwardedHeader(objectPtr, compressObjectReferences());
		omrobjectptr_t tenuredObjectPtr = forwardedHeader.getForwardedObject();
		*objectPtrIndirect = tenuredObjectPtr;

		Debug_MM_true(NULL != tenuredObjectPtr);
		Debug_MM_true(isInTenure(tenuredObjectPtr));

		setRememberedState(tenuredObjectPtr, OMR_TENURED_STACK_OBJECT_CURRENTLY_REFERENCED);
	}
}

bool
MM_Evacuator::evacuateHeap()
{
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

	/* this generates calling pattern ( evacuateRootObject() | evacuateThreadSlot() )* while iterating root set */
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

	/* flush locally buffered remembered objects to aggregated list */
	_env->flushRememberedSet();

	/* this generates calling pattern ( evacuateRememberedObject() )* while iterating remembered set */
	_controller->scavengeRememberedSet(_env);
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
	setCondition(reverse_roots, false);

	/* pull work into the stack to scan until all evacuators clear scanspace stack, outside copyspaces and worklist or abort */
	while (getWork()) {

		/* scan stack until empty */
		scan();
	}

	/* heap scan complete (or cycle aborted) */
	setCondition(breadth_first_roots, isScanOptionSelected(breadth_first_always | breadth_first_roots));
	setCondition(reverse_roots, isScanOptionSelected(reverse_roots));

	Debug_MM_true(!isConditionSet(scanning_heap));
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
			Debug_MM_true(0 == stackFrame->getWhiteSize() || (stackFrame == _whiteStackFrame[getEvacuationRegion(stackFrame->getBase())]));
		}
		Debug_MM_true(0 == _copyspace[survivor].getCopySize());
		Debug_MM_true(0 == _copyspace[tenure].getCopySize());
		Debug_MM_true(0 == _workList.volume());
	}
#endif /* defined(EVACUATOR_DEBUG) */

	/* report final copied/scanned volumes for this scan cycle */
	_controller->reportProgress(this, _copiedBytesDelta, &_scannedBytesDelta);

	/* all done heap scan */
	setCondition(scanning_heap, false);
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
		} else {
			/* scale threshold to worklist volume for normal operation */
			return OMR_MIN(volumeOfWork, _controller->_maximumWorkspaceSize);
		}
	}

	/* promote work distribution and parallelize (cut and shed) scanning dense/recursive structures */
	return min_workspace_release;
}

void
MM_Evacuator::addWork(MM_EvacuatorWorkspace *work)
{
	omrthread_monitor_enter(_mutex);

	/* append workspace to worklist */
	_workList.add(work, isConditionSet(reverse_roots));

	omrthread_monitor_exit(_mutex);

	/* notify controller */
	gotWork();
}

bool MM_Evacuator::hasDistributableWork(uintptr_t workReleaseThreshold, uintptr_t volumeOfWork)
{
	uintptr_t volumeAfterHead = (_workList.volume() - _workList.volume(_workList.peek()));
	return ((workReleaseThreshold < volumeAfterHead) && _controller->hasFulfilledWorkQuota(workReleaseThreshold, volumeOfWork));
}

void
MM_Evacuator::gotWork()
{
	/* check work volume and send notification of work to controller if quota filled and notification pending */
	if (hasDistributableWork(min_workspace_release, _workList.volume())) {
#if defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		uint64_t waitStartTime = startWaitTimer("notify");
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

		_controller->notifyOfWork();

#if defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		endWaitTimer(waitStartTime);
#endif /* defined(J9MODRON_TGC_PARALLEL_STATISTICS) || defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
	}
}

void
MM_Evacuator::findWork()
{
	Debug_MM_true(!isAbortedCycle());

	/* select as prospective donor the evacuator with greatest volume of distributable work */
	uintptr_t maxVolume = 0;
	MM_Evacuator *maxDonor = NULL;
	for (MM_Evacuator *next = _controller->getNextEvacuator(this); next != this; next = _controller->getNextEvacuator(next)) {
		uintptr_t volume = next->_workList.volume();
		if ((volume > maxVolume) && next->hasDistributableWork(min_workspace_release, volume)) {
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
		if (donor->hasDistributableWork(min_workspace_release, donor->_workList.volume())) {

			/* retest donor volume of work after locking its worklist */
			if (0 == omrthread_monitor_try_enter(donor->_mutex)) {

				/* pull work from donor worklist into stack and worklist until pulled volume levels up with donor remainder */
				if (donor->hasDistributableWork(min_workspace_release, donor->_workList.volume())) {
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
	Debug_MM_true((0 == _copyspace[survivor].getWhiteSize()) || (_copyspace[survivor].getWhiteSize() >= min_workspace_release) || isConditionSet(copyspaceTailFillCondition(survivor)));
	Debug_MM_true((0 == _copyspace[tenure].getWhiteSize()) || (_copyspace[tenure].getWhiteSize() >= min_workspace_release) || isConditionSet(copyspaceTailFillCondition(tenure)));
	Debug_MM_true(!hasScanWork());

	/* evacuator must clear both outside copyspaces and its worklist before polling other evacuators */
	/* if outside copyspaces are empty and worklist is not work must be pulled from worklist regardless of quota or distribution condition */
	uintptr_t copysize[] = { _copyspace[survivor].getCopySize(), _copyspace[tenure].getCopySize() };

	/* pull work from worklist if its aggregate volume of work exceeds work quota and volume of work in outside copyspace  */
	uintptr_t worklistVolumeQuota = (0 == (copysize[0] + copysize[1])) ? 0 : _controller->getWorkNotificationQuota(_controller->_minimumWorkspaceSize);
	if ((worklistVolumeQuota <= _workList.volume()) && !isAbortedCycle()) {

		omrthread_monitor_enter(_mutex);

		/* another evacuator may have stalled or worklist volume might have been reduced while waiting on worklist mutex */
		if ((worklistVolumeQuota <= _workList.volume()) && !isAbortedCycle()) {
			/* pull workspaces from worklist into stack scanspaces */
			pull(&_workList);
		}

		omrthread_monitor_exit(_mutex);
	}

	if (!hasScanWork() && !isAbortedCycle()) {

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
			} else if ((tailfill[survivor]) && (0 < copysize[tenure])) {
				/* survivor outside copyspace is tail filling, but tenure is not and has work to pull */
				outside = tenure;
			}
		} else if (0 < copysize[tenure]) {
			/* survivor copyspace has no work, but tenure may have work to pull */
			outside = tenure;
		}

		Debug_MM_true((copysize[outside] >= copysize[otherEvacuationRegion(outside)]) || isConditionSet(copyspaceTailFillCondition(otherEvacuationRegion(outside))));

		/* if no work pulled from worklist try to pull work from selected outside copyspace */
		if (0 < copysize[outside]) {
			/* pull work from copyspace into bottom stack frame */
			pull(&_copyspace[outside]);
		}
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
		_controller->reportProgress(this, _copiedBytesDelta, &_scannedBytesDelta);
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
MM_Evacuator::clear()
{
	Debug_MM_true(((_stackTop - _stackBottom) == _maxStackDepth) || (((_stackTop - _stackBottom) == _3) && (_maxStackDepth < 3)));
	Debug_MM_true((_stackBottom <= _whiteStackFrame[survivor]) && (_stackTop > _whiteStackFrame[survivor]));
	Debug_MM_true((_stackBottom <= _whiteStackFrame[tenure]) && (_stackTop > _whiteStackFrame[tenure]));
	Debug_MM_true(_whiteStackFrame[survivor] != _whiteStackFrame[tenure]);
	Debug_MM_true(!hasScanWork());

	/* unconditionally move region whitespace to top stack frames (this is why stack needs at least 3 frames) */
	MM_EvacuatorScanspace * const topFrame[] = { (_stackTop - 1),  (_stackTop - 2) };

	/* if both top frames hold region inside whitespace (in any order) then its all good */
	const uintptr_t topWhitesize[] = { topFrame[survivor]->getWhiteSize(), topFrame[tenure]->getWhiteSize() };
	if ((0 == topWhitesize[survivor]) && (0 == topWhitesize[tenure])) {

		/* both top frames are clear to pull region whitespaces */
		topFrame[survivor]->pullWhitespace(_whiteStackFrame[survivor]);
		topFrame[tenure]->pullWhitespace(_whiteStackFrame[tenure]);
		_whiteStackFrame[survivor] = topFrame[survivor];
		_whiteStackFrame[tenure] = topFrame[tenure];

	} else if ((0 < topWhitesize[survivor]) ^ (0 < topWhitesize[tenure])) {

		/* one top frame holds whitespace for one region, other top frame is clear to pull other region whitespace */
		Region region = (0 == topWhitesize[survivor]) ? survivor : tenure;
		topFrame[region]->pullWhitespace(_whiteStackFrame[region]);
		_whiteStackFrame[region] = topFrame[region];
	}

	/* region inside whitespaces parked in top stack frames and scanspaces below are clear to pull/push work */
	Debug_MM_true(_whiteStackFrame[survivor] != _whiteStackFrame[tenure]);
	Debug_MM_true(2 >= (_stackTop - _whiteStackFrame[survivor]));
	Debug_MM_true(2 >= (_stackTop - _whiteStackFrame[tenure]));
	Debug_MM_true(_stackBottom->isEmpty());
}

void
MM_Evacuator::pull(MM_EvacuatorCopyspace *copyspace)
{
	Debug_MM_true(0 < copyspace->getCopySize());

	/* clear whitespace from bottom stack frame and select it for scanning */
	clear();
	/* select bottom stack frame for scanning */
	_scanStackFrame = _stackBottom;
	/* pull work into selected frame scanspace and rebase copyspace */
	_scanStackFrame->pullWork(copyspace);

	/* trim copyspace if not enough whitespace to build a viable workspace */
	if (copyspace->getWhiteSize() < min_workspace_release) {

		/* trim remainder whitespace to whitelist */
		_whiteList[getEvacuationRegion(copyspace->getBase())].add(copyspace->trim());
	}

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	_scanStackFrame->activated();
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
}

void
MM_Evacuator::pull(MM_EvacuatorWorklist *worklist)
{
	Debug_MM_true(NULL != worklist);
	Debug_MM_true(0 < worklist->volume());
	Debug_MM_true(NULL != worklist->peek());

	/* move inside whitespace for survivor and tenure regions to top stack frames */
	clear();

	/* make a local LIFO list of workspaces to be loaded onto the stack and scanned in worklist order */
	uintptr_t limit = ((_stackTop - _stackBottom) - 2) >> 1;
	MM_EvacuatorWorkspace *stacklist = worklist->next();
	uintptr_t pulledVolume = worklist->volume(stacklist);
	const MM_EvacuatorWorkspace *next = worklist->peek();
	uintptr_t nextVolume = worklist->volume(next);

	/* pull workspaces from source worklist into stack list until pulled volume levels up with donor worklist volume  */
	while ((NULL != next) && ((pulledVolume + nextVolume) <= (worklist->volume() - nextVolume))) {
		MM_EvacuatorWorkspace *workspace = NULL;

		/* if under stack limit pull next workspace into stack list */
		if (0 < limit) {
			/* pull next workspace from source worklist and append to LIFO stack list (first in list -> first scanned in top stack frame) */
			workspace = worklist->next();
			workspace->next = stacklist;
			stacklist = workspace;
			limit -= 1;
		} else if (&_workList != worklist) {
			/* if source worklist belongs to other evacuator add workspace to own worklist */
			workspace = worklist->next();
			_workList.add(workspace);
		} else {
			/* break out of worklist iteration */
			break;
		}

		/* peek at next workspace in source worklist for next iteration */
		pulledVolume += nextVolume;
		next = worklist->peek();
		nextVolume = worklist->volume(next);
	}

	/* pull workspaces from stack list into stack frames (last pulled workspace on the bottom) */
	for (MM_EvacuatorWorkspace *workspace = stacklist; NULL != workspace; workspace = _freeList.add(workspace)) {

		/* set workspace into scanspace in next stack frame */
		uintptr_t arrayHeaderSize = 0;
		_scanStackFrame = hasScanWork() ? (_scanStackFrame + 1) : _stackBottom;
		if (isSplitArrayWorkspace(workspace)) {

			/* set scanspace: base=array object, scan = copy = limit = end = arrayEnd */
			uint8_t *arrayHead = (uint8_t *)workspace->base;
			uint8_t *arrayEnd = arrayHead + _objectModel->getConsumedSizeInBytesWithHeader(workspace->base);

			/* split array object scanner holds split segment bounds and actual scan head */
			uintptr_t startSlot = workspace->offset - 1;
			uintptr_t stopSlot = startSlot + workspace->length;

			/* split array object scanner is instantiated in scanspace but not activated until scanning starts */
			GC_IndexableObjectScanner *scanner = (GC_IndexableObjectScanner *)_scanStackFrame->activateObjectScanner();
			_delegate.getSplitPointerArrayObjectScanner(workspace->base, scanner, startSlot, stopSlot, GC_ObjectScanner::scanHeap);
			_scanStackFrame->setSplitArrayScanspace(arrayHead, arrayEnd, startSlot, stopSlot);

			if (scanner->isHeadObjectScanner()) {
				uintptr_t arrayBytes = getReferenceSlotSize() * ((GC_IndexableObjectScanner *)scanner)->getIndexableRange();
				arrayHeaderSize = _scanStackFrame->getSize() - arrayBytes;
			}
		} else {

			/* set scanscape: base = scan = workspace base, copy = limit = end = base + workspace volume */
			_scanStackFrame->setScanspace((uint8_t *)workspace->base, (uint8_t *)workspace->base + workspace->length, workspace->length);
		}
		_stats->countWorkPacketSize((arrayHeaderSize + _workList.volume(workspace)), _controller->_maximumWorkspaceSize);
	}

	/* At this point the stack must not be empty, total work volume pulled from worklist
	 * must not exceed remainder volume unless only head workspace was pulled and it
	 * accounted for more the half of the original worklist volume and the source worklist
	 * will be empty if it had only one workspace and it was pulled to stack or worklist. */

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	Debug_MM_true((pulledVolume <= worklist->volume() || (_scanStackFrame == _stackBottom)));
	Debug_MM_true((_stackBottom <= _scanStackFrame) && (_scanStackFrame < (_stackTop - 2)));
	uintptr_t stackVolume = 0;
	for (MM_EvacuatorScanspace *frame = _stackBottom; frame <= _scanStackFrame; frame += 1) {
		stackVolume += frame->getWorkSize();
		frame->activated();
	}
	Debug_MM_true(stackVolume <= pulledVolume);
	Debug_MM_true((pulledVolume - stackVolume) <= _workList.volume());
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
		Debug_MM_true((_stackLimit == _stackCeiling) || (_stackLimit == _stackBottom));
		Debug_MM_true((_scanStackFrame < _stackLimit) || (_stackLimit == _stackBottom));
		Debug_MM_true(_scanStackFrame >= _stackBottom);

		/* copy inside current frame until push or pop */
		copy();
	}

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	for (MM_EvacuatorScanspace *frame = _stackBottom; frame < _stackTop; ++frame) {
		Debug_MM_true(0 == frame->getWorkSize());
		Debug_MM_true((0 == frame->getWhiteSize()) || (frame == _whiteStackFrame[getEvacuationRegion(frame->getBase())]));
	}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
}

MM_EvacuatorScanspace *
MM_Evacuator::next(MM_EvacuatorScanspace *nextFrame, const Region region)
{
	Debug_MM_true(!isAbortedCycle());

	/* find the nearest frame not inferior to frame that holds or can receive whitespace for evacuation region */
	if (nextFrame == _whiteStackFrame[otherEvacuationRegion(region)]) {

		/* frame holds whitespace for other evacuation region and must be skipped */
		nextFrame += 1;
	}

	/* frame is empty or holds whitespace for evacuation region or stack is blown */
	if (nextFrame >= _stackCeiling) {

		/* set stack overflow condition and inhibit push */
		setCondition(stack_overflow, true);
		return NULL;
	}

	Debug_MM_true((0 == nextFrame->getWorkSize()) || (nextFrame == _whiteStackFrame[region]));
	Debug_MM_true((0 == nextFrame->getWhiteSize()) || (nextFrame == _whiteStackFrame[region]));

	return nextFrame;
}

void
MM_Evacuator::push(MM_EvacuatorScanspace * const nextFrame)
{
	if (_scanStackFrame < nextFrame) {

		/* push to next frame */
		_scanStackFrame = nextFrame;

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		_scanStackFrame->activated();
		debugStack("push");
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
	}
}

void
MM_Evacuator::pop()
{
	debugStack("pop");
	Debug_MM_true((_stackBottom <= _scanStackFrame) && (_scanStackFrame < _stackCeiling));
	Debug_MM_true((0 == _scanStackFrame->getWhiteSize()) || (_scanStackFrame == _whiteStackFrame[getEvacuationRegion(_scanStackFrame->getBase())]));
	Debug_MM_true((0 == _scanStackFrame->getWorkSize()) || isAbortedCycle());

	/* clear scanned work and work-related flags from scanspace */
	_scanStackFrame->reset(false);

	/* pop stack frame leaving trailing whitespace where it is (in _whiteStackFrame[getEvacuationRegion(_scanStackFrame->getBase())]) */
	if (_stackBottom < _scanStackFrame) {

		/* pop to previous frame, will continue with whitespace in popped frame if next pushed object does not cross region boundary */
		_scanStackFrame -= 1;

	} else {

		/* pop to empty stack */
		_scanStackFrame = NULL;
	}

	/* update stack overflow flags when popping back into bottom frame and clear them on empty stack */
	if ((NULL == _scanStackFrame) || (_scanStackFrame == _stackBottom)) {

		/* at bottom frame force depth first scanning if stack overflowed and flushed as it unwound */
		setCondition(depth_first, ((_scanStackFrame == _stackBottom) && isConditionSet(stack_overflow)));
		/* in either case clear stack overflow condition */
		setCondition(stack_overflow, false);

		/* update cycle volume metrics and clear stack volume metrics */
		if (NULL == _scanStackFrame) {
			_stats->_cycleVolumeMetrics[outside] += _stackVolumeMetrics[outside];
			_stats->_cycleVolumeMetrics[inside] += _stackVolumeMetrics[inside];
			_stats->_cycleVolumeMetrics[scanned] += _stackVolumeMetrics[scanned];
			_stackVolumeMetrics[inside] = _stackVolumeMetrics[outside] = 0;
			_stackVolumeMetrics[scanned] = 0;
		}
	}
}

void
MM_Evacuator::copy()
{
	const uintptr_t sizeLimit = _maxInsideCopySize;
	const bool compressed = _compressObjectReferences;
	MM_EvacuatorScanspace * const stackFrame = _scanStackFrame;
	const bool isTenureFrame = (tenure == getEvacuationRegion(stackFrame->getBase()));
	uint8_t * const frameLimit = stackFrame->getBase() + (isConditionSet(depth_first) ? 0: _maxInsideCopyDistance);

	/* get the active object scanner without advancing scan head */
	GC_ObjectScanner *objectScanner = scanner();

	/* copy small stack region objects inside frame, large and other region objects outside, until push() or frame completed */
	while (NULL != objectScanner) {
		Debug_MM_true((NULL == stackFrame->getActiveObject()) || (stackFrame->getActiveObject() == (omrobjectptr_t)stackFrame->getScanHead()) || (stackFrame->isSplitArrayScanspace() && (stackFrame->getActiveObject() == (omrobjectptr_t)stackFrame->getBase())));

		/* loop through reference slots in current object at scan head */
		GC_SlotObject *slotObject = objectScanner->getNextSlot();
		while (NULL != slotObject) {

			const omrobjectptr_t object = slotObject->readReferenceFromSlot();
			omrobjectptr_t forwardedAddress = object;
			if (isInEvacuate(object)) {

				/* copy and forward the slot object */
				MM_ForwardedHeader forwardedHeader(object, compressed);
				if (!forwardedHeader.isForwardedPointer()) {

					/* slot object must be evacuated -- determine receiving region and before and after object size */
					uintptr_t slotObjectSizeBeforeCopy = 0, slotObjectSizeAfterCopy = 0, hotFieldAlignmentDescriptor = 0;
					_objectModel->calculateObjectDetailsForCopy(_env, &forwardedHeader, &slotObjectSizeBeforeCopy, &slotObjectSizeAfterCopy, &hotFieldAlignmentDescriptor);
					const Region region = isNurseryAge(_objectModel->getPreservedAge(&forwardedHeader)) ? survivor : tenure;
					MM_EvacuatorScanspace * const whiteFrame = _whiteStackFrame[region];

					/* copy flush overflow and large objects outside,  copy small objects inside the stack ... */
					if ((sizeLimit > slotObjectSizeAfterCopy) && !isForceOutsideCopyCondition(region)) {

						/* ... if sufficient whitespace remaining for region or can be refreshed in a superior (next) stack frame */
						if ((whiteFrame->getWhiteSize() < slotObjectSizeAfterCopy) && !reserveInsideScanspace(region, slotObjectSizeAfterCopy)) {
							goto outside;
						}

						/* copy/forward inside white stack frame and test for copy completion at preset copy head */
						uint8_t * const copyHead = _whiteStackFrame[region]->getCopyHead();
						forwardedAddress = copyForward(&forwardedHeader, slotObject->readAddressFromSlot(), _whiteStackFrame[region], slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy);
						if ((uint8_t *)forwardedAddress == copyHead) {

							if (_whiteStackFrame[region] == stackFrame) {

								/* object was copied past copy limit in active stack frame and should be pushed if possible */
								if (copyHead > frameLimit) {

									/* try to find nearest superior frame that can receive whitespace for evacuation region */
									MM_EvacuatorScanspace * const nextFrame =  next(stackFrame + 1, region);
									if (NULL != nextFrame) {

										/* pull copy and remaining whitespace up into next frame */
										nextFrame->pullTail(stackFrame, copyHead);
										/* set next frame as white frame for evacuation region */
										_whiteStackFrame[region] = nextFrame;

										/* push to next frame */
										push(nextFrame);
									}
								}

							} else if (_whiteStackFrame[region] > stackFrame) {

								/* object was copied into a superior frame and must be pushed */
								if (_whiteStackFrame[region] == whiteFrame) {

									/* if white frame is hanging some distance up the stack try to pull it closer to active frame */
									MM_EvacuatorScanspace * const nextFrame =  next(stackFrame + 1, region);
									if ((NULL != nextFrame) && (nextFrame < whiteFrame)) {

										/* pull copy and remaining whitespace down into next frame */
										nextFrame->pullTail(_whiteStackFrame[region], copyHead);
										/* set next frame as white frame for evacuation region */
										_whiteStackFrame[region] = nextFrame;
									}
								}

								/* push to next frame */
								push(_whiteStackFrame[region]);
							}
						}

					} else {

						/* try outside copyspace if not copied inside stack -- it may redirect into a stack frame and push */
outside:				forwardedAddress = copyOutside(region, &forwardedHeader, slotObject->readAddressFromSlot(), slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy);
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
			if (isTenureFrame && (isInSurvivor(forwardedAddress) || isInEvacuate(forwardedAddress))) {
				stackFrame->updateRememberedState(true);
			}

			/* if copy was pushed up the stack return to continue in pushed frame */
			if (_scanStackFrame != stackFrame) {
				return;
			}

			/* continue with next scanned slot object */
			slotObject = objectScanner->getNextSlot();
		}

		/* advance scan head and set scanner for next object or set NULL scanner to pop at end of scanspace */
		objectScanner = scanner(true);
	}

	Debug_MM_true(stackFrame->getScanHead() == stackFrame->getCopyHead());

	/* pop scan stack */
	pop();
}

omrobjectptr_t
MM_Evacuator::copyOutside(Region region, MM_ForwardedHeader *forwardedHeader, fomrobject_t *referringSlotAddress, const uintptr_t slotObjectSizeBeforeCopy, const uintptr_t slotObjectSizeAfterCopy)
{
	Debug_MM_true(!forwardedHeader->isForwardedPointer());
	Debug_MM_true(isInEvacuate(forwardedHeader->getObject()));

	/* failure to evacuate must return original address in evacuate space */
	omrobjectptr_t forwardedAddress = forwardedHeader->getObject();

	/* use the preferred region copyspace if it can contain the object */
	MM_EvacuatorCopyspace *effectiveCopyspace = &_copyspace[region];

	/* splitable pointer arrays are directed to the large object copyspace and split into distributable workspaces on the worklist */
	const bool isSplitableArray = isSplitablePointerArray(forwardedHeader, slotObjectSizeAfterCopy);
	if (isSplitableArray || (slotObjectSizeAfterCopy > effectiveCopyspace->getWhiteSize())) {

		/* reserve whitespace in a copyspace (or scanspace) -- this may be from other evacuation region if none available in preferred region */
		effectiveCopyspace = reserveOutsideCopyspace(&region, slotObjectSizeAfterCopy, isSplitableArray);
	}

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
					splitPointerArrayWork(copyHead);
					/* array scanning work has been distributed to worklist so clear it from large copyspace */
					_largeCopyspace.rebase();

				} else {

					/* extract workspace to worklist and rebase large copyspace */
					MM_EvacuatorWorkspace *workspace = _freeList.next();
					workspace->base = (omrobjectptr_t)_largeCopyspace.rebase(&workspace->length);
					addWork(workspace);

					Debug_MM_true(slotObjectSizeAfterCopy == workspace->length);
				}

				/* clear the large object copyspace for next use */
				_whiteList[region].add(_largeCopyspace.trim());

			} else if (effectiveCopyspace != _whiteStackFrame[region]->asCopyspace()) {

				/* copied to outside copyspace, release work only if remaining whitespace can hold a minimal workspace */
				if (effectiveCopyspace->getWhiteSize() >= min_workspace_release) {

					/*  release a workspace when work volume hits threshold */
					if (effectiveCopyspace->getCopySize() >= _workspaceReleaseThreshold) {

						/* rebase copyspace to pull workspace leaving only whitespace in copyspace, add to worklist */
						MM_EvacuatorWorkspace *workspace = _freeList.next();
						workspace->base = (omrobjectptr_t)effectiveCopyspace->rebase(&workspace->length);
						addWork(workspace);
					}

				} else {

					/* force flushing of small objects to this copyspace to fill tail and reduce whitelist traffic  */
					setCondition(copyspaceTailFillCondition(region), true);
				}

			} else {
				
				/* copied inside stack whitespace -- receiving frame was pushed if above current frame */
				Debug_MM_true(((uint8_t *)copyHead + slotObjectSizeAfterCopy) == _whiteStackFrame[region]->getCopyHead());
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
		Region region = getEvacuationRegion(forwardedAddress);

#if defined(OMR_VALGRIND_MEMCHECK)
		valgrindMempoolAlloc(_env->getExtensions(), (uintptr_t)forwardedAddress, forwardedLength);
#endif /* defined(OMR_VALGRIND_MEMCHECK) */
#if defined(EVACUATOR_DEBUG)
		_delegate.debugValidateObject(forwardedHeader);
#endif /* defined(EVACUATOR_DEBUG) */

		/* forwarding address set by this thread -- object will be evacuated to the copy head in copyspace */
		memcpy(forwardedAddress, forwardedHeader->getObject(), originalLength);

		/* advance the copy head in the receiving copyspace and track scan/copy progress */
		copyspace->advanceCopyHead(forwardedLength);

		/* synchronize stall condition with controller */
		if (isConditionSet(stall) ^ _controller->areAnyEvacuatorsStalled()) {
			setCondition(stall, !isConditionSet(stall));
		}

		/* update copy volume metrics for tracking distribution of work and reporting scan/copy progress */
		_stackVolumeMetrics[(copyspace == _whiteStackFrame[region]->asCopyspace()) ? inside : outside] += forwardedLength;
		if ((_copiedBytesDelta[survivor] + _copiedBytesDelta[tenure]) >= _copiedBytesReportingDelta) {
			_controller->reportProgress(this, _copiedBytesDelta, &_scannedBytesDelta);
		}

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		Debug_MM_true(!isConditionSet(breadth_first_roots) || !isConditionSet(recursive_object));
		/* update proximity and size metrics */
		_stats->countCopyDistance((uintptr_t)referringSlotAddress, (uintptr_t)forwardedAddress);
		_stats->countObjectSize(forwardedLength, _maxInsideCopySize);
		_conditionCounts[_conditionFlags & (uintptr_t)conditions_mask] += 1;
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

		/* update scavenger stats */
		uintptr_t objectAge = _objectModel->getPreservedAge(forwardedHeader);
		if (isInTenure(forwardedAddress)) {
			_stats->_tenureAggregateCount += 1;
			_stats->_tenureAggregateBytes += originalLength;
			_stats->getFlipHistory(0)->_tenureBytes[objectAge + 1] += forwardedLength;
#if defined(OMR_GC_LARGE_OBJECT_AREA)
			if (copyspace->isLOA()) {
				_stats->_tenureLOACount += 1;
				_stats->_tenureLOABytes += originalLength;
			}
#endif /* OMR_GC_LARGE_OBJECT_AREA */
			_copiedBytesDelta[tenure] += forwardedLength;
		} else {
			Debug_MM_true(isInSurvivor(forwardedAddress));
			_stats->_flipCount += 1;
			_stats->_flipBytes += originalLength;
			_stats->getFlipHistory(0)->_flipBytes[objectAge + 1] += forwardedLength;
			_copiedBytesDelta[survivor] += forwardedLength;
		}

		/* update object age and finalize copied object header */
		if (tenure == region) {
			objectAge = STATE_NOT_REMEMBERED;
		} else if (objectAge < OBJECT_HEADER_AGE_MAX) {
			objectAge += 1;
		}

		/* copy the preserved fields from the forwarded header into the destination object */
		forwardedHeader->fixupForwardedObject(forwardedAddress);
		/* object model fixes the flags in the destination object */
		_objectModel->fixupForwardedObject(forwardedHeader, forwardedAddress, objectAge);

#if defined(EVACUATOR_DEBUG)
		if (isDebugCopy()) {
			OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
			char className[32];
			omrobjectptr_t parent = hasScanWork() ? _scanStackFrame->getActiveObject() : NULL;
			omrtty_printf("%5lu %2lu %2lu:%c copy %3s; base:%lx; copy:%lx; end:%lx; free:%lx; %lx %s %lx -> %lx %lx\n", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex,
					hasScanWork() && ((uint8_t *)forwardedAddress >= _scanStackFrame->getBase()) && ((uint8_t *)forwardedAddress < _scanStackFrame->getEnd()) ? 'I' : 'O',
					isInSurvivor(forwardedAddress) ? "new" : "old",	(uintptr_t)copyspace->getBase(), (uintptr_t)copyspace->getCopyHead(), (uintptr_t)copyspace->getEnd(), copyspace->getWhiteSize(),
					(uintptr_t)parent, _delegate.debugGetClassname(forwardedAddress, className, 32), (uintptr_t)forwardedHeader->getObject(), (uintptr_t)forwardedAddress, forwardedLength);
		}
		_delegate.debugValidateObject(forwardedAddress);
#endif /* defined(EVACUATOR_DEBUG) */
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
			MM_ForwardedHeader forwardedHeader(forwardedAddress, compressObjectReferences());
			if (!forwardedHeader.isForwardedPointer()) {

				/* determine receiving region and before and after object size and copy to an outside copyspace and continue chain */
				uintptr_t slotObjectSizeBeforeCopy = 0, slotObjectSizeAfterCopy = 0, hotFieldAlignmentDescriptor = 0;
				_objectModel->calculateObjectDetailsForCopy(_env, &forwardedHeader, &slotObjectSizeBeforeCopy, &slotObjectSizeAfterCopy, &hotFieldAlignmentDescriptor);
				const Region region = isNurseryAge(_objectModel->getPreservedAge(&forwardedHeader)) ? survivor : tenure;
				forwardedAddress = copyOutside(region, &forwardedHeader, selfReferencingSlot.readAddressFromSlot(), slotObjectSizeBeforeCopy, slotObjectSizeAfterCopy);
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

	/* get active object scanner, or NULL if no active scanner for scanspace */
	MM_EvacuatorScanspace* const scanspace = _scanStackFrame;
	GC_ObjectScanner *objectScanner = scanspace->getActiveObjectScanner();
	uintptr_t scannedBytes = 0;

	/* current object scanner must be finalized before advancing scan head to next object */
	if (advanceScanHead && (NULL != objectScanner)) {

		omrobjectptr_t const scannedObject = scanspace->getActiveObject();

		/* for split array scanspaces the scanned volume always includes a fixed number of elements */
		if (scanspace->isSplitArrayScanspace()) {
			/* extra volume in first segment accounts for header and non-array data in object and may include more elements than subsequent segments */
			if (objectScanner->isHeadObjectScanner()) {
				uintptr_t arrayBytes = getReferenceSlotSize() * ((GC_IndexableObjectScanner *)objectScanner)->getIndexableRange();
				scannedBytes = scanspace->getSize() - arrayBytes;
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

	/* advance scan head over skipped and leaf objects to set up next object scanner */
	while ((NULL == objectScanner) && (scanspace->getScanHead() < scanspace->getCopyHead())) {

		omrobjectptr_t const objectPtr = (omrobjectptr_t)scanspace->getScanHead();
		objectScanner = _delegate.getObjectScanner(objectPtr, scanspace->activateObjectScanner(), GC_ObjectScanner::scanHeap);
		if ((NULL == objectScanner) || objectScanner->isLeafObject()) {

			/* nothing to scan for object at scan head, advance scan head to next object in scanspace and drop object scanner */
			const uintptr_t objectVolume = _objectModel->getConsumedSizeInBytesWithHeader(objectPtr);
			scanspace->advanceScanHead(objectVolume);
			scannedBytes += objectVolume;
			objectScanner = NULL;

		} else if (objectScanner->isLinkedObjectScanner()) {

			/* got a linked object scanner -- set recursive object condition to inhibit reflection of outside copy into stack */
			setCondition(recursive_object, isScanOptionSelected(recursive_object));
			if (isConditionSet(recursive_object)) {

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
	}

	/* update evacuator progress for epoch reporting */
	if (0 < scannedBytes) {
		_scannedBytesDelta += scannedBytes;
		if (_scannedBytesDelta >= _copiedBytesReportingDelta) {
			_controller->reportProgress(this, _copiedBytesDelta, &_scannedBytesDelta);
		}
		_stackVolumeMetrics[scanned] += scannedBytes;
	}

	return objectScanner;
}

MM_EvacuatorWhitespace *
MM_Evacuator::refreshInsideWhitespace(const Region region, const uintptr_t slotObjectSizeAfterCopy)
{
	MM_EvacuatorWhitespace *whitespace = NULL;

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
			Debug_MM_true(NULL != whitespace);

		} else {

			/* update controller view of aggregate confirmed volume of survivor copied and approximate remaining survivor volume */
			_controller->reportProgress(this, _copiedBytesDelta, &_scannedBytesDelta);

			/* try to allocate whitespace (tlh) from the evacuation region */
			whitespace = _controller->getWhitespace(this, region, slotObjectSizeAfterCopy);
		}
	}

	return whitespace;
}

bool
MM_Evacuator::reserveInsideScanspace(const Region region, const uintptr_t slotObjectSizeAfterCopy)
{
	Debug_MM_true(hasScanWork());
	Debug_MM_true(evacuate > region);

	/* find next frame that can receive whitespace for evacuation region */
	MM_EvacuatorScanspace *nextFrame = next(_scanStackFrame + 1, region);
	if (NULL != nextFrame) {

		/* try to refresh from whitelist or new tlh allocation */
		MM_EvacuatorWhitespace *whitespace = refreshInsideWhitespace(region, slotObjectSizeAfterCopy);
		if (NULL != whitespace) {

			/* set whitespace into next stack frame */
			nextFrame->setScanspace(whitespace->getBase(), whitespace->getBase(), whitespace->length(), whitespace->isLOA());

			/* stack whitespace for evacuation region is installed in next stack frame */
			_whiteStackFrame[region] = nextFrame;

			/* receiving frame is holding enough stack whitespace to copy object to evacuation region */
			Debug_MM_true(region == getEvacuationRegion(nextFrame->getBase()));
			Debug_MM_true(slotObjectSizeAfterCopy <= nextFrame->getWhiteSize());

			return true;
		}
	}

	/* failover to outside copy */
	return false;
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

				} else {

					/* update controller view of aggregate copied volume for slightly more accurate estimation of remaining volume */
					_controller->reportProgress(this, _copiedBytesDelta, &_scannedBytesDelta);
					/* try to allocate whitespace from the evacuation region */
					whitespace = _controller->getWhitespace(this, preferred, slotObjectSizeAfterCopy);
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
				if (!isBreadthFirstCondition() && !isConditionSet(recursive_object)) {
					Debug_MM_true(NULL != _whiteStackFrame[preferred]);

					/* copy into preferred region whitespace reserved for copying inside stack scanspaces */
					MM_EvacuatorScanspace *whiteFrame = _whiteStackFrame[preferred];
					if (slotObjectSizeAfterCopy <= whiteFrame->getWhiteSize()) {

						/* stack may be empty in root evacuation contexts if no breadth-first condition is set */
						if (!hasScanWork() || (whiteFrame > _scanStackFrame)) {

							/* select stack frame to hold region whitespace receiving copy */
							MM_EvacuatorScanspace *nextFrame = hasScanWork() ? (_scanStackFrame + 1) : _stackBottom;
							if (nextFrame != whiteFrame) {

								/* find lowest frame above selected frame that can pull region inside whitespace */
								if (nextFrame == _whiteStackFrame[otherEvacuationRegion(preferred)]) {
									nextFrame += 1;
								}

								/* pull whitespace into selected stack frame and set it as white stack frame for region */
								if (nextFrame != whiteFrame) {
									nextFrame->pullWhitespace(whiteFrame);
									_whiteStackFrame[preferred] = nextFrame;
									whiteFrame = nextFrame;
								}
							}
						}

						/* if white frame is at current frame object will be scanned immediately after copy without push(), if below it will be scanned when stack pops */
						if (!hasScanWork() || (whiteFrame > _scanStackFrame)) {
							/* if stack empty or selected frame is above current frame push() to scan it immediately after copy is complete */
							push(whiteFrame);
						}

						return whiteFrame->asCopyspace();
					}
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
			if (NULL == whitespace) {

				/* this will allocate just enough whitespace to receive object copy */
				whitespace = _controller->getObjectWhitespace(this, preferred, slotObjectSizeAfterCopy);
			}
		}
	}

	/* at this point no whitespace to refresh copyspace means abort collection */
	if (NULL != whitespace) {
		Debug_MM_true(slotObjectSizeAfterCopy <= whitespace->length());
		Debug_MM_true(*region == getEvacuationRegion(whitespace));

		/* pull whitespace from evacuation region into outside or large copyspace */
		if (useLargeCopyspace) {

			/* select large copyspace to receive whitespace */
			copyspace = &_largeCopyspace;

		} else {

			/* select and clear outside copyspace to receive whitespace */
			copyspace = &_copyspace[*region];

			/* distribute any work in copyspace to worklist */
			if (0 < copyspace->getCopySize()) {

				/* pull remaining work in copyspace into a workspace and put it on the worklist */
				MM_EvacuatorWorkspace *work = _freeList.next();
				work->base = (omrobjectptr_t)copyspace->rebase(&work->length);
				addWork(work);
			}

			/* trim remainder whitespace from copyspace to whitelist */
			_whiteList[*region].add(copyspace->trim());
		}

		/* set up selected copyspace with reserved whitespace to receive object copy */
		copyspace->setCopyspace((uint8_t*)whitespace, (uint8_t*)whitespace, whitespace->length(), whitespace->isLOA());

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
	return (MM_EvacuatorBase::min_split_indexable_size <= objectSizeInBytes);
}

bool
MM_Evacuator::isHugeObject(const uintptr_t objectSizeInBytes) const
{
	/* any object that can't fit in a maximal copyspace is a huge object */
	return (_controller->_maximumCopyspaceSize < objectSizeInBytes);
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
MM_Evacuator::splitPointerArrayWork(omrobjectptr_t pointerArray)
{
	uintptr_t elements = 0;
	_delegate.getIndexableDataBounds(pointerArray, &elements);

	/* distribute elements to segments as evenly as possible and take largest segment first */
	uintptr_t segments = elements / MM_EvacuatorBase::max_split_segment_elements;
	if (0 != (elements % MM_EvacuatorBase::max_split_segment_elements)) {
		segments += 1;
	}
	uintptr_t elementsPerSegment = elements / segments;
	uintptr_t elementsThisSegment = elementsPerSegment + (elements % segments);

	omrthread_monitor_enter(_mutex);

	/* record 1-based array offsets to mark split array workspaces */
	uintptr_t offset = 1;
	while (0 < segments) {

		/* wrap each segment in a split array workspace */
		MM_EvacuatorWorkspace* work = _freeList.next();
		work->base = pointerArray;
		work->offset = offset;
		work->length = elementsThisSegment;
		work->next = NULL;

		/* append split array workspace to worklist */
		_workList.add(work, isConditionSet(reverse_roots));

		/* all split array workspaces but the first have equal volumes of work */
		offset += elementsThisSegment;
		elementsThisSegment = elementsPerSegment;
		segments -= 1;
	}

	omrthread_monitor_exit(_mutex);

	/* notify controller if this evacuator has distributable work */
	gotWork();
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
	/* return true if age selected by collector's tenure mask */
	return (0 == (((uintptr_t)1 << objectAge) & _tenureMask));
}

void
MM_Evacuator::setCondition(MM_Evacuator::ConditionFlag condition, bool value)
{
	/* set or reset specific flush condition(s) */
	if (value) {
		_conditionFlags |= (uintptr_t)condition;
	} else {
		_conditionFlags &= ~(uintptr_t)condition;
	}

	/* synchronize stack limit with outside copy conditions */
	if (isForceOutsideCopyCondition() ^ (_stackLimit == _stackBottom)) {
		_stackLimit = isForceOutsideCopyCondition() ? _stackBottom : _stackCeiling;
	}

	/* synchronize workspace release threshold with scanning stage and work distribution conditions */
	if (isDistributeWorkCondition() ^ (min_workspace_release == _workspaceReleaseThreshold)) {
		_workspaceReleaseThreshold = adjustWorkReleaseThreshold();
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
	return isConditionSet(stack_overflow | depth_first | recursive_object | stall | breadth_first_roots);
}

bool
MM_Evacuator::isForceOutsideCopyCondition() const
{
	/* test stack overflow and copyspace tail filling conditions, which force all objects to be copied outside */
	return (isConditionSet(breadth_first_always | breadth_first_roots | stack_overflow | recursive_object | stall));
}

bool
MM_Evacuator::isForceOutsideCopyCondition(MM_Evacuator::Region region) const
{
	/* test stack overflow and copyspace tail filling conditions, which force all objects to be copied outside */
	return (isConditionSet(copyspaceTailFillCondition(region) | breadth_first_always | breadth_first_roots | stack_overflow | recursive_object | stall));
}

MM_Evacuator::ConditionFlag
MM_Evacuator::copyspaceTailFillCondition(MM_Evacuator::Region region) const
{
	/* return copyspace overflow condition for the specific region */
	return (ConditionFlag)((uintptr_t)survivor_tail_fill << region);
}

void
MM_Evacuator::receiveWhitespace(MM_EvacuatorWhitespace *whitespace)
{
	/* this is tlh whitespace reserved but not used for copy */
	_whiteList[getEvacuationRegion(whitespace)].add(whitespace);
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
	OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
#if defined(EVACUATOR_DEBUG)
	if (isDebugWork()) {
		uintptr_t flags = _controller->sampleEvacuatorFlags();
		omrtty_printf("%5lu %2lu %2lu:%10s; flags:%lx; ", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex, tag, flags);
		_controller->printEvacuatorBitmap(_env, "stalled", _controller->sampleStalledMap());
		_controller->printEvacuatorBitmap(_env, "; resuming", _controller->sampleResumingMap());
		omrtty_printf("; stacked: %lx; vow:%lx\n", (hasScanWork() ? _scanStackFrame->getWorkSize() : 0), _workList.volume());
	}
#endif /* defined(EVACUATOR_DEBUG) */
	return omrtime_hires_clock();
}

void
MM_Evacuator::endWaitTimer(uint64_t waitStartTime)
{
	OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
	uint64_t waitEndTime = omrtime_hires_clock();

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
		Region region = getEvacuationRegion(scanspace->getBase());
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
			_delegate.debugValidateObject(object);
			object = (omrobjectptr_t)((uintptr_t)object + _objectModel->getConsumedSizeInBytesWithHeader(object));
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
			_delegate.debugValidateObject(object);
			Debug_MM_true(_objectModel->getRememberedBits(object) < (uintptr_t)0xc0);
			if (_objectModel->isRemembered(object)) {
				if (!shouldRememberObject(object)) {
					omrtty_printf("%5lu %2lu %2lu:downgraded; object:%lx; flags:%lx\n", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex, (uintptr_t)object, _objectModel->getObjectFlags(object));
				}
			} else if (shouldRememberObject(object)) {
				omrtty_printf("%5lu %2lu %2lu: !remember; object:%lx; flags:%lx\n", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex, (uintptr_t)object, _objectModel->getObjectFlags(object));
				Debug_MM_true(isAbortedCycle());
			}
			object = (omrobjectptr_t)((uintptr_t)object + _objectModel->getConsumedSizeInBytesWithHeader(object));
		}
	}
	omrtty_printf("%5lu %2lu %2lu:    tenure; end:%lx\n", _controller->getEpoch()->gc, _controller->getEpoch()->epoch, _workerIndex, (uintptr_t)object);
	Debug_MM_true(object == end);
}
#else
void MM_Evacuator::debugStack(const char *stackOp, bool treatAsWork) { }
#endif /* defined(EVACUATOR_DEBUG) */

/*
Thread 2 "main" hit Breakpoint 4, MM_Evacuator::copy (this=0x7ffff009fef0) at Evacuator.cpp:1184
1184				Debug_MM_true(isInSurvivor(forwardedAddress) || isInTenure(forwardedAddress) || (isInEvacuate(forwardedAddress) && isAbortedCycle()));
copied: 0xffe00060:4
base: 0xffe00000:5c
scan: 0xffe00000:5c
copy: 0xffe00070
(gdb) c
Continuing.

Thread 2 "main" hit Breakpoint 4, MM_Evacuator::copy (this=0x7ffff009fef0) at Evacuator.cpp:1184
1184				Debug_MM_true(isInSurvivor(forwardedAddress) || isInTenure(forwardedAddress) || (isInEvacuate(forwardedAddress) && isAbortedCycle()));
copied: 0xffe00070:24
base: 0xffe00000:5c
scan: 0xffe00000:5c
copy: 0xffe00098
(gdb) c
Continuing.

Thread 2 "main" hit Breakpoint 4, MM_Evacuator::copy (this=0x7ffff009fef0) at Evacuator.cpp:1184
1184				Debug_MM_true(isInSurvivor(forwardedAddress) || isInTenure(forwardedAddress) || (isInEvacuate(forwardedAddress) && isAbortedCycle()));
copied: 0xffe00098:4
base: 0xffe00000:5c
scan: 0xffe00000:5c
copy: 0xffe000a8
(gdb) c
Continuing.

Thread 2 "main" hit Breakpoint 4, MM_Evacuator::copy (this=0x7ffff009fef0) at Evacuator.cpp:1184
1184				Debug_MM_true(isInSurvivor(forwardedAddress) || isInTenure(forwardedAddress) || (isInEvacuate(forwardedAddress) && isAbortedCycle()));
copied: 0xffe000a8:46500
base: 0xffe00000:5c
scan: 0xffe00000:5c
copy: 0xffe000d0
(gdb) c
Continuing.

Thread 2 "main" hit Breakpoint 4, MM_Evacuator::copy (this=0x7ffff009fef0) at Evacuator.cpp:1184
1184				Debug_MM_true(isInSurvivor(forwardedAddress) || isInTenure(forwardedAddress) || (isInEvacuate(forwardedAddress) && isAbortedCycle()));
copied: 0xffe000d0:43200
base: 0xffe00000:5c
scan: 0xffe00000:5c
copy: 0xffe000f8
(gdb) c
Continuing.

Thread 2 "main" hit Breakpoint 4, MM_Evacuator::copy (this=0x7ffff009fef0) at Evacuator.cpp:1184
1184				Debug_MM_true(isInSurvivor(forwardedAddress) || isInTenure(forwardedAddress) || (isInEvacuate(forwardedAddress) && isAbortedCycle()));
copied: 0xffe000f8:3a400
base: 0xffe00000:5c
scan: 0xffe00000:5c
copy: 0xffe00120
(gdb) c
Continuing.

Thread 2 "main" hit Breakpoint 4, MM_Evacuator::copy (this=0x7ffff009fef0) at Evacuator.cpp:1184
1184				Debug_MM_true(isInSurvivor(forwardedAddress) || isInTenure(forwardedAddress) || (isInEvacuate(forwardedAddress) && isAbortedCycle()));
copied: 0xffe00120:57d00
base: 0xffe00000:5c
scan: 0xffe00070:24
copy: 0xffe00158
(gdb) c
Continuing.

Thread 2 "main" hit Breakpoint 4, MM_Evacuator::copy (this=0x7ffff009fef0) at Evacuator.cpp:1184
1184				Debug_MM_true(isInSurvivor(forwardedAddress) || isInTenure(forwardedAddress) || (isInEvacuate(forwardedAddress) && isAbortedCycle()));
copied: 0xffe001a0:eb600
base: 0xffe00158:44
scan: 0xffe00158:44
copy: 0xffe001e8
(gdb) c
*/
