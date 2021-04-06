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
}

bool
MM_Evacuator::initialize()
{
	/* disable spinning for monitor-try-enter access to other evacuator worklist */
	if (0 != omrthread_monitor_init_with_name(&_mutex, 0, "MM_Evacuator::_mutex")) {
		return false;
	}
	((J9ThreadAbstractMonitor *)_mutex)->flags &= ~J9THREAD_MONITOR_TRY_ENTER_SPIN;

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
	_forge->free(_stack);
	_forge->free(_whiteList);

	/* free the freelist */
	_freeList.reset();
}

void
MM_Evacuator::setHeapBounds(volatile uint8_t *heapBounds[][2])
{
	for (Region region = survivor; region <= evacuate; region = nextEvacuationRegion(region)) {
		_heapBounds[region][0] = (uint8_t *)heapBounds[region][0]; /* lower bound for heap region address range */
		_heapBounds[region][1] = (uint8_t *)heapBounds[region][1]; /* upper bound for heap region address range */
	}
}

/**
 * Per gc cycle setup. This binds the evacuator instance to a gc worker thread for the duration of the cycle.
 *
 * @param env the environment for the gc worker thread that is bound to this evacuator instance
 */
void
MM_Evacuator::bindWorkerThread(MM_EnvironmentStandard *env, uintptr_t tenureMask)
{
	Debug_MM_true(0 == _workList.volume());

	/* bind evacuator and delegate to executing gc thread */
	_env = env;
	_tenureMask = tenureMask;
	_metrics = _controller->getEvacuatorMetrics(_workerIndex);
	_env->setEvacuator(this, _metrics);

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

	/* reset the delegate */
	_delegate.cycleStart();
}

/**
 * Per gc cycle cleanup. This unbinds the evacuator instance from its gc worker thread.
 *
 * @param env the environment for the gc worker thread that is bound to this evacuator instance
 */
void
MM_Evacuator::unbindWorkerThread(MM_EnvironmentStandard *env)
{
	/* unbind delegate from gc thread */
	_delegate.cycleEnd();

	/* unbind evacuator from gc thread */
	_env->setEvacuator(NULL, NULL);
	_env = NULL;
}

void
MM_Evacuator::workThreadGarbageCollect(MM_EnvironmentStandard *env)
{
	Debug_MM_true(_env == env);
	Debug_MM_true(_env->getEvacuator() == this);
	Debug_MM_true(_controller->isBoundEvacuator(_workerIndex));
	Debug_MM_true(!_extensions->isConcurrentScavengerEnabled());

	OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
	uint64_t runStartTime = omrtime_hires_clock();
	uint64_t cpuStartTime = omrthread_get_self_cpu_time(omrthread_self());

	/* load some empty workspaces into the freelist -- each evacuator retains forge memory between gc cycles to back this up */
	_freeList.reload();

	/* set up whitespaces for the cycle */
	_whiteList[survivor].reset(_controller->getMemorySubspace(survivor));
	_whiteList[tenure].reset(_controller->getMemorySubspace(tenure));

	/* initialize inside/outside survivor/tenure and overflow copyspaces as empty */
	for (CopySpace *cp = &_copyspace[insideSurvivor]; cp <= &_copyspace[overflow]; cp += 1) {
		cp->_base = cp->_copy = cp->_end = NULL;
		cp->_flags = 0;
	}

	/* initialize stack frames as empty (including _nil) */
	for (ScanSpace *fp = _stack; fp <= _nil; fp += 1) {
		GC_NullObjectScanner::newInstance(_env, &fp->_scanner);
		fp->_base = fp->_scan = fp->_end = NULL;
		fp->_flags = isNull;
	}

	/* set stack pointer _sp and region frame pointers _rp[] to _nil to start with empty scan stack */
	_sp = _rp[insideSurvivor] = _rp[insideTenure] = _nil;

	/* clamp workspace release while scanning roots to maximize number of distributable workspaces */
	_condition = _evacuatorScanOptions & initial_mask;
	_copyspaceOverflow[survivor] = _copyspaceOverflow[tenure] = 0;
	_insideDistanceMax = _maxInsideCopyDistance;
	_insideSizeMax = _maxInsideCopySize;
	_abortedCycle = false;

	/* worklist volume and workspace count will grow monotonically while remembered/root scanning is in progress */
	scanRemembered();
	scanRoots();

	/* worklist volume and workspace count will typically balloon initially and as quickly deflate to low a plateau for most of the remainder of the heap scan */
	scanHeap();

	/* trick to obviate a write barrier -- stack slots with thread root objects tenured this cycle need to be revisited to set remembered state */
	if (!isAbortedCycle() && _controller->isEvacuatorFlagSet(MM_EvacuatorController::rescanThreadSlots)) {
		_delegate.rescanThreadSlots();
		flushRememberedSet();
	}

	/* scan clearable objects -- this may involve 0 or more phases, with evacuator threads joining in scanHeap() at end of each phase */
	while (!isAbortedCycle() && _delegate.hasClearable()) {
		/* java root clearer has repeated phases involving *deprecated* evacuateHeap() and its delegated scanClearable() leaves no unscanned work */
		if (scanClearable()) {
			/* runtimes should use only evacuateObject() in delegated scanClearable() and allow scanHeap() to complete each delegated phase */
			 scanHeap();
		}
	}

	/* evacuation is complete or aborted  */

	/* flush any abandoned workspaces in the worklist back into the freelist  */
	if (isAbortedCycle()) {
		_workList.flush();
	}

	/* flush whitespace from copyspaces to whitelists */
	for (CopySpace *copyspace = &_copyspace[insideSurvivor]; copyspace <= &_copyspace[overflow]; copyspace += 1) {
		if (copyspace->_copy < copyspace->_end) {
			_whiteList[source(copyspace)].add(trim(copyspace));
		}
	}
	/* recycle whitespace in the survivor whitelist back into the memory pool */
	flushWhitespace(survivor);

	/* flush thread-local buffers before completing or aborting generational cycle */
	_delegate.flushForEndCycle();
	Debug_MM_true(0 == _env->_scavengerRememberedSet.count);

	/* prune remembered set to complete, or back out evacuation to abort, generational cycle */
	if (!isAbortedCycle()) {
		Debug_MM_true(0 == _workList.volume());
		/* whitespace is retained on tenure whitelist between completed evacuations and is flushed before each global gc */
		_controller->pruneRememberedSet(_env);
	} else {
		/* set backout flag and abort cycle (this will likely be followed immediately by a global gc, which will force tenure whitelist to flush) */
		_controller->setBackOutFlag(_env, backOutFlagRaised);
		_controller->completeBackOut(_env);
	}

	/* collection is complete or aborted  */

	/* merge GC stats */
	_metrics->_threadMetrics[cpu_ms] = ((omrthread_get_self_cpu_time(omrthread_self()) - cpuStartTime) / 1000);
	_metrics->_threadMetrics[real_ms] = (omrtime_hires_clock() - runStartTime);
	_controller->mergeThreadGCStats(_env);
}

omrobjectptr_t
MM_Evacuator::evacuateRootObject(MM_ForwardedHeader *forwardedHeader, bool breadthFirst)
{
	Debug_MM_true(nil(_sp));
	Debug_MM_true(!isConditionSet(scan_worklist));
	Debug_MM_true(!isConditionSet(scan_clearable) || breadthFirst);

	const bool previously = isConditionSet(breadth_first_roots);
	setCondition(breadth_first_roots, previously || breadthFirst);
	omrobjectptr_t forwardedAddress = forwardedHeader->getObject();
	if (!isAbortedCycle()) {
		/* copy object and check for scan work on the stack */
		forwardedAddress = copy(forwardedHeader);
		Debug_MM_true(!isBreadthFirstCondition() || nil(_sp));
		if (!nil(_sp)) {
			scan();
		}
	}
	setCondition(breadth_first_roots, previously);

	Debug_MM_true(isInSurvivor(forwardedAddress) || isInTenure(forwardedAddress) || (isInEvacuate(forwardedAddress) && isAbortedCycle()));
	Debug_MM_true(nil(_sp));

	return forwardedAddress;
}

bool
MM_Evacuator::scanRememberedObject(omrobjectptr_t objectptr)
{
	Debug_MM_true(isConditionSet(scan_remembered));
	Debug_MM_true((NULL != objectptr) && isInTenure(objectptr));
	Debug_MM_true(_objectModel->getRememberedBits(objectptr) < (uintptr_t)0xc0);
	Debug_MM_address(objectptr);

	bool rememberObject = false;

	/* scan a remembered object - evacuate all referents in evacuate region */
	MM_EvacuatorDelegate::ScannerState scanState;
	GC_ObjectScanner *objectScanner = _delegate.getObjectScanner(objectptr, &scanState, GC_ObjectScanner::scanRoots);
	if (NULL != objectScanner) {
		const GC_SlotObject *slotPtr = objectScanner->getNextSlot();
		while ( NULL != slotPtr) {
			omrobjectptr_t forwardedAddress = evacuateRootObject(slotPtr);
			if (isInSurvivor(forwardedAddress) || isInEvacuate(forwardedAddress)) {
				rememberObject = true;
			}
			slotPtr = objectScanner->getNextSlot();
		}
	}

	/* the remembered state of the object may also depend on indirectly related objects */
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
	Debug_MM_true(isConditionSet(scan_roots));
	Debug_MM_true(!_env->getExtensions()->isConcurrentScavengerEnabled());
	Debug_MM_true(_env->getExtensions()->isEvacuatorEnabled());

	omrobjectptr_t objectPtr = *objectPtrIndirect;
	if (NULL != objectPtr) {
		/* auto-remember stack- and thread-referenced objects */
		if (isInEvacuate(objectPtr)) {
			omrobjectptr_t forwardedAddress = evacuateRootObject(objectPtrIndirect);
			if (isInTenure(forwardedAddress)) {
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
		Debug_MM_true(isInTenure(tenuredObjectPtr));

		/* set the slot to point to the forwarded address and update remembered state now */
		*objectPtrIndirect = tenuredObjectPtr;
		remember(tenuredObjectPtr, OMR_TENURED_STACK_OBJECT_CURRENTLY_REFERENCED);
	}
}

bool
MM_Evacuator::evacuateHeap()
{
	Debug_MM_true(isConditionSet(scan_clearable));

	/* TODO: deprecate this calling pattern, forced by MM_RootScanner; root scanners should not call scanHeap() directly  */
	scanHeap();

	return !isAbortedCycle();
}

void
MM_Evacuator::scanRoots()
{
	/* this generates calling pattern [ evacuateRootObject()* evacuateThreadSlot()* evacuateRootObject()* ] while iterating root set */
	const bool previously = selectCondition(breadth_first_roots);
	_workReleaseThreshold = isConditionSet(breadth_first_roots) ? min_workspace_release : _controller->_minimumWorkspaceSize;
	setCondition(scan_roots, true);
	_delegate.scanRoots();
	setCondition(scan_roots, false);
	setCondition(breadth_first_roots, previously);

}

void
MM_Evacuator::scanRemembered()
{
	/* flush locally buffered remembered objects to aggregated list */
	_env->flushRememberedSet();

	/* this generates calling pattern [ (scanRememberedObject)* := (evacuateRootObject*)* ] while iterating remembered set */
	const bool previously = selectCondition(breadth_first_roots);
	_workReleaseThreshold = isConditionSet(breadth_first_roots) ? min_workspace_release : _controller->_minimumWorkspaceSize;
	setCondition(scan_remembered, true);
	_controller->scavengeRememberedSet(_env);
	setCondition(scan_remembered, false);
	setCondition(breadth_first_roots, previously);
}

void
MM_Evacuator::scanHeap()
{
#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	_metrics->_threadMetrics[!isConditionSet(scan_clearable) ? scan_count : clearing_count] += 1;
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

	/* allow recursive scanning */
	setCondition(breadth_first_roots, false);
	_workReleaseThreshold = _controller->getWorkReleaseThreshold();

	/* pull scan work into the stack from outside copyspaces and worklist until all are cleared or gc abort */
	setCondition(scan_worklist, true);
	while (getWork()) {
		/* scan stack until empty */
		scan();
	}
	setCondition(scan_worklist, false);

	/* restore root scanning conditions */
	selectCondition(breadth_first_roots, isConditionSet(scan_clearable));
	_workReleaseThreshold = isConditionSet(breadth_first_roots) ? min_workspace_release : _controller->_minimumWorkspaceSize;
}

bool
MM_Evacuator::scanClearable()
{
	Debug_MM_true(isConditionSet(breadth_first_roots) == isScanOptionSelected(breadth_first_roots));

	/* suppress recursive scanning of clearable roots */
	setCondition(breadth_first_roots, true);
	_workReleaseThreshold = min_workspace_release;

	/* if there are more root or other unreachable objects to be evacuated they can be copied and forwarded here */
	setCondition(scan_clearable, true);
	_delegate.scanClearable();
	setCondition(scan_clearable, false);

	/* restore root scanning conditions */
	selectCondition(breadth_first_roots);
	_workReleaseThreshold = isConditionSet(breadth_first_roots) ? min_workspace_release : _controller->_minimumWorkspaceSize;

	/* run scanHeap() if scanClearable() produced more work to be scanned */
	return !_controller->hasCompletedScan();
}

void
MM_Evacuator::scanComplete()
{
	/* the last wait is first charged as a pause (monitor wait) and must be discounted there and charged as an end */
	uint64_t endTime = _metrics->_lastMonitorTime;
	_metrics->_threadMetrics[end_count] += 1;
	_metrics->_threadMetrics[end_ms] += endTime;
	_metrics->_threadMetrics[_metrics->_lastMonitorMetric] -= 1;
	_metrics->_threadMetrics[_metrics->_lastMonitorMetric + 1] -= endTime;
	Debug_MM_true((stall_count == _metrics->_lastMonitorMetric) || (wait_count == _metrics->_lastMonitorMetric));
}

void
MM_Evacuator::push(ScanSpace *sp, Region region, uint8_t *copyhead, uint8_t *copyend)
{
	Debug_MM_true(copyhead < copyend);
	Debug_MM_true(sourceOf(copyhead) == region);
	Debug_MM_true(sourceOf(copyend) == region);
	Debug_MM_true(_rp[other(region)] != sp);
	Debug_MM_true((_stack <= sp) && (sp < _nil));
	Debug_MM_true(nil(_sp) || (_sp + 1) == sp);
	Debug_MM_true((_stack < sp) || nil(_sp));
	Debug_MM_true(flagged(sp, isPulled) || !isBreadthFirstCondition());
	Debug_MM_true(flagged(sp, isNull));
	Debug_MM_address(copyhead);

	_sp = sp;
	_sp->_base = _sp->_scan = copyhead;
	_sp->_end = copyend;
	_sp->_flags |= region;
	_sp->_flags &= ~(uintptr_t)isNull;
	if (!flagged(_sp, isPulled)) {
		_rp[region] = _sp;
	}
	_delegate.getObjectScanner(_sp->_object, &_sp->_scanner, GC_ObjectScanner::scanHeap);
}

void
MM_Evacuator::scan()
{
	Debug_MM_true(!isBreadthFirstCondition() || nil(_sp) || flagged(_sp, isPulled));
	while (next()) {
		ScanSpace * const sp = _sp;
		omrobjectptr_t object = _slot->readReferenceFromSlot();
		if (isInEvacuate(object)) {
			MM_ForwardedHeader forwardedHeader(object, _env->compressObjectReferences());
			object = copy(&forwardedHeader);
			_slot->writeReferenceToSlot(object);
		}
		if (isInTenure(sp->_scan) && (isInSurvivor(object) || isInEvacuate(object))) {
			sp->_flags |= (uintptr_t)isRemembered;
		}
		_slot = NULL;
	}
}

bool
MM_Evacuator::next()
{
	while (!nil(_sp)) {
		/* pop the next slot from object at scan head */
		Debug_MM_true((outside(source(_sp))->_base > _sp->_scan) || (_sp->_scan >= outside(source(_sp))->_end));
		Debug_MM_true((_sp >= _stack) && (_sp < _nil));
		_slot = scanner(_sp)->getNextSlot();
		if (NULL != _slot) {
			return true;
		}

		/* done with scanned object -- reset associated conditions and advance scan head to next non-leaf object */
		do {
			/* update scanned object remembered state and reset object-related flags in stack frame */
			if (flagged(_sp, isRemembered)) {
				Debug_MM_true(isInTenure(_sp->_base));
				remember(_sp->_object, STATE_REMEMBERED);
				_sp->_flags &= ~(uintptr_t)isRemembered;
			}

			/* advance scan head and set up scanner for next object in frame */
			if (!flagged(_sp, isSplitArray | isNull)) {
				Debug_MM_true((_sp->_base <= _sp->_scan) && (_sp->_scan < _sp->_end));
				/* advance scan head in current frame (scanner might hold a size hint) */
				const uint32_t size = scanner(_sp)->objectSizeInBytes();
				Debug_MM_true((0 == size) || (size == _objectModel->getConsumedSizeInBytesWithHeader(_sp->_object)));
				_sp->_scan += (0 < size) ? size : _objectModel->getConsumedSizeInBytesWithHeader(_sp->_object);
				if (_sp->_scan < _sp->_end) {
					Debug_MM_address(_sp->_scan);
					Debug_MM_true((outside(source(_sp))->_base > _sp->_scan) || (_sp->_scan >= outside(source(_sp))->_end));
					/* get object scanner for object at scan head */
					_delegate.getObjectScanner(_sp->_object, &_sp->_scanner, GC_ObjectScanner::scanHeap);
				}
			} else {
				Debug_MM_true((_sp->_scan <= _sp->_base) && (_sp->_base <= _sp->_end));
				/* advance scan head (pointing to array head) to end of frame to force pop for scanned split array */
				_sp->_scan = _sp->_end;
			}

			/* pop frame when scan head reaches end of frame */
			Debug_MM_true(_sp->_scan <= _sp->_end);
			if ((_sp->_scan >= _sp->_end) && !pop()) {
				/* break and return NULL on  empty stack */
				break;
			}
		} while (scanner(_sp)->isLeafObject());
	}
	return NULL;
}

bool
MM_Evacuator::pop()
{
	Debug_MM_true(!flagged(_sp, isNull));
	Debug_MM_true((_sp == _rp[source(_sp)]) || flagged(_sp, isPulled));
	Debug_MM_true(_sp->_scan == _sp->_end);

	/* update scanned volume metrics */
	_metrics->_volumeMetrics[scanned] += (_sp->_scan - _sp->_base);

	/* reset and pop frame and adjust stack pointers */
	const Region region = source(_sp);
	reset( _sp -- );
	if (_stack <= _sp) {
		/* relocate region frame pointer for region being scanned in popped frame */
		_rp[region] = _nil;
		for (ScanSpace *fp = _sp; (fp >= _stack) && !flagged(fp, isPulled); fp -= 1) {
			if (region == source(fp)) {
				Debug_MM_true(_rp[other(region)] != fp);
				_rp[region] = fp;
				break;
			}
		}
		Debug_MM_true(inside(region)->_base == inside(region)->_copy);
		Debug_MM_true((_rp[region] != _nil) || (region != source(_sp)) || flagged(_sp, isPulled));
	} else {
		Debug_MM_true(inside(survivor)->_base == inside(survivor)->_copy);
		Debug_MM_true(inside(tenure)->_base == inside(tenure)->_copy);
		Debug_MM_true(_rp[survivor]->_base == _rp[survivor]->_end);
		Debug_MM_true(_rp[tenure]->_base == _rp[tenure]->_end);
		/* empty stack */
		_sp = _rp[survivor] = _rp[tenure] = _nil;
	}

	/* hold stack overflow condition if scanning above floor or scanning indexable object in floor */
	if (nil(_sp) || (flagged(_sp, isPulled) && !scanner(_sp)->isIndexableObject())) {
		/* clear stack overflow condition if popping to lower floor or empty stack */
		setCondition(stack_overflow, false);
	}

	/* sync stall condition with controller view of evacuator status and if it changes adjust work release threshold */
	if (isConditionSet(stall) != (_controller->isNotifyOfWorkPending() || (0 == getDistributableVolumeOfWork()))) {
		if (isConditionSet(scan_worklist)) {
			_workReleaseThreshold = _controller->getWorkReleaseThreshold();
		}
		setCondition(stall, !isConditionSet(stall));
	}

	/* regulate inside copy constraints to suit current conditions */
	if (isConditionSet(stall | stack_overflow)) {
		/* limit inside copy size and maintain depth-first scanning while any modal conditions are set */
		_insideDistanceMax = OMR_MIN(modal_inside_copy_distance, _maxInsideCopyDistance);
		_insideSizeMax = OMR_MIN(modal_inside_copy_size, _maxInsideCopySize);
	} else {
		/* continue or resume with normal inside copy size and distance limits */
		_insideSizeMax = _maxInsideCopySize;
		_insideDistanceMax = _maxInsideCopyDistance;
	}

	Debug_MM_true(nil(_sp) || (outside(region)->_base > _sp->_scan) || (_sp->_scan >= outside(region)->_end));
	Debug_MM_true(nil(_sp) || (outside(other(region))->_base > _sp->_scan) || (_sp->_scan >= outside(other(region))->_end));
	Debug_MM_true(nil(_sp) || (_copyspace[overflow]._base > _sp->_scan) || (_sp->_scan >= _copyspace[overflow]._end));
	return !nil(_sp);
}

omrobjectptr_t
MM_Evacuator::copy(MM_ForwardedHeader *forwardedHeader)
{
	omrobjectptr_t forwardedAddress = forwardedHeader->getObject();
	if (!forwardedHeader->isForwardedPointer()) {
		/* marshal object metadata, clear and set indexable object condition for pointer array */
		uintptr_t sizeBeforeCopy = 0, sizeAfterCopy = 0, hotFieldAlignmentDescriptor = 0;
		Region region = isNurseryAge(_objectModel->getPreservedAge(forwardedHeader)) ? survivor : tenure;
		const uintptr_t classificationBits = _objectModel->calculateObjectDetailsForCopy(_env, forwardedHeader, &sizeBeforeCopy, &sizeAfterCopy, &hotFieldAlignmentDescriptor);
		_condition = (_condition & ~(uintptr_t)classification_mask) | (classificationBits << classification_shift);

		/* primitive (leaf) objects are just bubbles in the scan pipeline so are flushed to overflow copyspace unless collocated or needed to pump up worklist volume */
		CopySpace * const cp = selectCopyspace(&region, sizeAfterCopy);
		if (NULL != cp) {
			Debug_MM_true((cp->_base <= cp->_copy) && (cp->_copy <= (cp->_end - sizeAfterCopy)));
			Debug_MM_true((region == source(cp)) && (region == sourceOf(cp->_copy)));
			/* copy and forward */
			uint8_t *copyhead = cp->_copy;
			forwardedAddress = copyForward(forwardedHeader, cp, sizeBeforeCopy, sizeAfterCopy);
			if (forwardedAddress == (omrobjectptr_t)copyhead) {
				Debug_MM_true((cp->_base < cp->_copy) && (cp->_copy <= cp->_end));
				switch (index(cp)) {
				case insideSurvivor:
				case insideTenure:
					/* copied to inside survivor/tenure copyspace */
					if (_rp[region]->_end == copyhead) {
						if (!nil(_sp + 1) && (copyhead > (_rp[region]->_base + _insideDistanceMax))) {
							/* clip scanspace endpoint and push copied object up to next frame */
							_rp[region]->_end = copyhead;
							push((_sp + 1), region,  copyhead, cp->_copy);
						} else {
							/* extend end of current scanspace to end of copied object */
							_rp[region]->_end = cp->_copy;
						}
					} else {
						Debug_MM_true(!nil(_sp + 1));
						Debug_MM_true(nil(_rp[region]) || (_rp[region] <= _sp));
						/* first push to stack or refreshed inside copyspace for region -- just push copied object to lowest empty frame */
						push((nil(_sp) ? _stack : (_sp + 1)), region, copyhead, cp->_copy);
					}
					/* rebase inside copyspace to copy head */
					Debug_MM_true(_rp[region]->_end == cp->_copy);
					cp->_base = cp->_copy;
					break;
				case outsideSurvivor:
				case outsideTenure:
					Debug_MM_true(isConditionSet(copyspaceTailFillCondition(region)) == (0 < _copyspaceOverflow[region]));
					/* copied to outside survivor/tenure copyspace, which may be able to release work */
					if (isSplitablePointerArray(sizeAfterCopy)) {
						/* split into multiple workspaces and add to worklist, rebase copyspace */
						splitPointerArrayWork(cp, copyhead);
					} else if (worksize(cp, _workReleaseThreshold) && whitesize(cp, _controller->_minimumWorkspaceSize)) {
						/* rebase and release work only if copyspace can fill a workspace leaving enough whitespace for a minimal workspace */
						addWork(cp);
					}
					break;
				case overflow:
					/* copied to overflow copyspace for distribution to worklist (non-leaf) or disposal (leaf) */
					Debug_MM_true(isLeafCondition() == flagged(cp, isLeaves));
					if (isSplitablePointerArray(sizeAfterCopy)) {
						/* split into multiple workspaces and add to worklist, rebase copyspace */
						splitPointerArrayWork(cp, copyhead);
					} else if (!flagged(cp, isLeaves)) {
						/* release unscanned work if enough for a minimal workspace */
						if (worksize(cp, _workReleaseThreshold) && whitesize(cp, _controller->_minimumWorkspaceSize)) {
							/* rebase and release work only if copyspace can fill a workspace leaving enough whitespace for a minimal workspace */
							addWork(cp);
						}
					} else {
						/* no need to scan accumulated leaf objects, just count volume as scanned and rebase */
						_metrics->_volumeMetrics[leaf] += cp->_copy - cp->_base;
						cp->_base = cp->_copy;
					}
					/* trim and reset copyspace if remaining whitespace too small to hold a workspace */
					if ((cp->_base == cp->_copy) && (cp->_base + _controller->_minimumWorkspaceSize) > cp->_end) {
						_whiteList[region].add(trim(cp));
						reset(cp);
					}
					break;
				default:
					Assert_MM_unreachable();
					break;
				}
			}
		} else {
			setAbortedCycle();
		}
	} else {
		forwardedAddress = forwardedHeader->getForwardedObject();
	}
	return forwardedAddress;
}

MM_Evacuator::CopySpace *
MM_Evacuator::selectCopyspace(Region *region, uintptr_t sizeAfterCopy)
{
	Region selected = *region;
	do {
		const Region selection = selected;
		Debug_MM_true(!nil(_sp) || nil(_rp[selection]));

		/* try inside copyspace if there is an open stack frame and not forcing outside */
		if (!isForceOutsideCopyCondition(selection, sizeAfterCopy)) {
			CopySpace * const cp = inside(selection);
			Debug_MM_true((cp->_copy == _rp[selection]->_end) || nil(_rp[selection]) || (_rp[selection]->_scan < cp->_base) || (cp->_end <= _rp[selection]->_scan));
			Debug_MM_true((source(cp) == selection) || (NULL == cp->_base));
			if (whitesize(cp, sizeAfterCopy)
			&& ((sizeAfterCopy <= _insideSizeMax) || (!isConditionSet(pointer_array) && cached(cp->_copy)))
			&& (!nil(_sp + 1) || (selection == source(_sp)))
			) {
				/* select inside copyspace for small objects and objects that stick to referring object (cached) */
				*region = selection;
				return cp;
			} else if (!whitesize(cp, max_scanspace_remainder)
				&& (sizeAfterCopy <= _insideSizeMax) && !isLeafCondition() && !nil(_sp + 1)
				&& refresh(cp, selection, sizeAfterCopy)
			) {
				/* refresh & select inside copyspace if remaining whitespace is disposable (all remaining scan work is mapped in the stack) */
				Debug_MM_true(!nil(_sp + 1) || !nil(_rp[selection]));
				*region = selection;
				return cp;
			} else if (nil(_sp + 1) && !isConditionSet(leaf_object | indexed_primitive | stack_overflow)) {
				/* set stack overflow condition on first copy thrown to outside copyspace from scanspace in topmost frame */
				setCondition(stack_overflow, true);
			}
		} else if (nil(_sp + 1) && !isConditionSet(leaf_object | indexed_primitive | stack_overflow)) {
			/* set stack overflow condition on first copy thrown to outside copyspace from scanspace in topmost frame */
			setCondition(stack_overflow, true);
		}

		/* select outside copyspace if object fits in remaining whitespace, unless forcing primitive objects to overflow */
		Debug_MM_true((0 < _copyspaceOverflow[selection]) == isConditionSet(copyspaceTailFillCondition(selection)));
		if (!isForceOverflowCopyCondition(selection, sizeAfterCopy)) {
			CopySpace * const outsideCp = outside(selection);
			Debug_MM_true((NULL == outsideCp->_base) || source(outsideCp) == selection);
			if (whitesize(outsideCp, sizeAfterCopy)) {
				/* outside copyspace for source region can receive copy */
				*region = selection;
				return outsideCp;
			}
			/* refresh outside copyspace when remaining whitespace is disposable or scanning an array or overflow is excessive */
			if (!whitesize(outsideCp, max_copyspace_remainder) || scanner(_sp)->isIndexableObject()
			|| (_copyspaceOverflow[selection] > _controller->getWorkDistributionQuota())
			) {
				/* dispose of any unscanned work in overflow copyspace */
				flushOverflow();
				/* rebase copyspace and release unscanned work to worklist */
				if (outsideCp->_base < outsideCp->_copy) {
					addWork(outsideCp);
				}
				/* refresh outside copyspace with a new whitespace allocation */
				if (refresh(outsideCp, selection, sizeAfterCopy)) {
					/* clear tail filling condition and select refreshed copyspace */
					setCondition(copyspaceTailFillCondition(selection), false);
					_copyspaceOverflow[selection] = 0;
					*region = selection;
					return outsideCp;
				}
				Debug_MM_true((sizeAfterCopy >= OMR_MAX(_controller->maxWhitespaceAllocation(selection), _controller->maxObjectAllocation(selection)))
						|| (_copyspaceOverflow[selection] <= _controller->getWorkDistributionQuota()));
			}
			/* start/continue tracking outside overflow and redirecting object copy for source region to outside copyspace to consume remaining whitespace */
			Debug_MM_true((0 < _copyspaceOverflow[selection]) == isConditionSet(copyspaceTailFillCondition(selection)));
			setCondition(copyspaceTailFillCondition(selection), true);
			_copyspaceOverflow[selection] += sizeAfterCopy;
			/* if no refresh try to deflect copy into the inside copyspace for the source region */
			if (!isForceOutsideCopyCondition(selection, sizeAfterCopy)) {
				CopySpace * const insideCp = inside(selection);
				if (whitesize(insideCp, sizeAfterCopy)) {
					/* copy into inside copyspace for scanning inside the stack */
					*region = selection;
					return insideCp;
				}
			}
		}

		Debug_MM_true((0 < _copyspaceOverflow[selection]) == isConditionSet(copyspaceTailFillCondition(selection)));
		Debug_MM_true(isForceOverflowCopyCondition(selection, sizeAfterCopy)
				|| (whitesize(outside(selection), max_copyspace_remainder) && !scanner(_sp)->isIndexableObject() && ((_copyspaceOverflow[selection] - sizeAfterCopy) <= _controller->getWorkDistributionQuota()))
				|| (sizeAfterCopy >= OMR_MAX(_controller->maxWhitespaceAllocation(selection), _controller->maxObjectAllocation(selection)))
		);

		/* select overflow copyspace as a last resort */
		CopySpace * const overflowCp = &_copyspace[overflow];
		/* flush if content type (leaf/non-leaf) or region is different from incoming object */
		if (!whitesize(overflowCp, sizeAfterCopy)
		|| (selection != source(overflowCp)) || (isLeafCondition() != flagged(overflowCp, isLeaves))
		) {
			/* flush unscanned work to worklist, if region not selected flush whitespace to whitelist */
			flushOverflow(selection);
		}
		/* select large copyspace if it has enough whitespace in selected region for copy or can be refreshed */
		if (whitesize(overflowCp, sizeAfterCopy)
		|| refresh(overflowCp, selection, sizeAfterCopy)
		) {
			overflowCp->_flags |= isLeafCondition() ? isLeaves : 0;
			*region = selection;
			return overflowCp;
		}

		/* record failure stats if no whitespace in selected region */
		if (survivor == selection) {
			_stats->_failedFlipCount += 1;
			_stats->_failedFlipBytes += sizeAfterCopy;
		} else {
			_stats->_failedTenureCount += 1;
			_stats->_failedTenureBytes += sizeAfterCopy;
			if (sizeAfterCopy > _stats->_failedTenureLargest) {
				_stats->_failedTenureLargest = sizeAfterCopy;
			}
		}

		/* try the other evacation region */
		selected = other(selection);
	} while (selected != *region);

	return NULL;
}

void
MM_Evacuator::flushOverflow(Region selected)
{
	/* dispose of any unscanned work in overflow copyspace */
	CopySpace *const overflowCp = &_copyspace[overflow];
	if (overflowCp->_base < overflowCp->_copy) {
		if (flagged(overflowCp, isLeaves)) {
			/* no need to scan leaf objects, just count accumulated volume and rebase copyspace */
			_metrics->_volumeMetrics[leaf] += overflowCp->_copy - overflowCp->_base;
			overflowCp->_base = overflowCp->_copy;
		} else {
			/* flush non-leaf objects to worklist */
			addWork(overflowCp);
		}
	}
	overflowCp->_flags &= ~(uintptr_t) (isLeaves);
	/* if regions are disparate clip remaining whitespace into whitelist for source region */
	if ((selected < unreachable) && (selected != source(overflowCp))) {
		_whiteList[source(overflowCp)].add(trim(overflowCp));
		reset(overflowCp);
	} else if (overflowCp->_base >= overflowCp->_end) {
		reset(overflowCp);
	}
}

omrobjectptr_t
MM_Evacuator::copyForward(MM_ForwardedHeader *forwardedHeader, CopySpace *copyspace, const uintptr_t sizeBeforeCopy, const uintptr_t sizeAfterCopy)
{
	Debug_MM_true(whitesize(copyspace, sizeAfterCopy));
	Debug_MM_true(_controller->isObjectAligned(copyspace->_copy));
	Debug_MM_true(_controller->isObjectAligned(copyspace->_copy + sizeAfterCopy));
	Debug_MM_true(isInSurvivor(copyspace->_copy) || isInTenure(copyspace->_copy));
	Debug_MM_true(!isBreadthFirstCondition() || (index(copyspace) >= outsideSurvivor));
	/* if not already forwarded object will be copied to copy head in designated copyspace */
	omrobjectptr_t const prospectiveAddress = (omrobjectptr_t)copyspace->_copy;
	/* try to set forwarding address to the copy head in copyspace */
	omrobjectptr_t const forwardedAddress = forwardedHeader->setForwardedObject(prospectiveAddress);
	/* if forwarding address set copy object, otherwise just return address forwarded by another thread */
	if (forwardedAddress == prospectiveAddress) {
#if defined(OMR_VALGRIND_MEMCHECK)
		valgrindMempoolAlloc(_env->getExtensions(), (uintptr_t)forwardedAddress, sizeAfterCopy);
#endif /* defined(OMR_VALGRIND_MEMCHECK) */
#if defined(EVACUATOR_DEBUG)
		if (isDebugPoisonDiscard()) {
			Debug_MM_true(_objectModel->isDeadObject(forwardedAddress));
			Whitespace *w = (Whitespace *)forwardedAddress;
			uint8_t *q = (uint8_t *)w + sizeAfterCopy;
			for (uint8_t *p = (uint8_t *)w + OMR_MIN(w->length(), sizeof(Whitespace)); p < q; p += 1) {
				Debug_MM_true(*p == (uint8_t)J9_GC_SINGLE_SLOT_HOLE);
			}
		}
		if (isDebugCopy()) {
			_delegate.debugValidateObject(forwardedHeader);
		}
#endif /* defined(EVACUATOR_DEBUG) */

		/* forwarding address set by this thread -- object will be evacuated to the copy head in copyspace */
		memcpy(forwardedAddress, forwardedHeader->getObject(), sizeBeforeCopy);
		/* advance the copy head in the receiving copyspace */
		copyspace->_copy += sizeAfterCopy;
		/* advance survivor copy age up to max young age or for tenure copy reset age and remembered bits */
		const uintptr_t thisAge = _objectModel->getPreservedAge(forwardedHeader);
		uintptr_t nextAge = thisAge + 1;

		/* update scavenger stats and evacuator metrics */
		Region region = sourceOf(forwardedAddress);
		if (survivor == region) {
			_stats->_flipCount += 1;
			_stats->_flipBytes += sizeBeforeCopy;
			_stats->getFlipHistory(0)->_flipBytes[nextAge] += sizeAfterCopy;
			_metrics->_volumeMetrics[survivor] += sizeAfterCopy;
			nextAge = OMR_MIN(nextAge, OBJECT_HEADER_AGE_MAX);
		} else {
			_stats->_tenureAggregateCount += 1;
			_stats->_tenureAggregateBytes += sizeBeforeCopy;
			_stats->getFlipHistory(0)->_tenureBytes[nextAge] += sizeAfterCopy;
#if defined(OMR_GC_LARGE_OBJECT_AREA)
			if (flagged(copyspace, isLOA)) {
				_stats->_tenureLOACount += 1;
				_stats->_tenureLOABytes += sizeBeforeCopy;
			}
#endif /* OMR_GC_LARGE_OBJECT_AREA */
			_metrics->_volumeMetrics[tenure] += sizeAfterCopy;
			nextAge = STATE_NOT_REMEMBERED;
		}
#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
#if defined(OMR_SCAVENGER_TRACK_COPY_DISTANCE)
		_stats->countCopyDistance(region, (NULL != _slot) ? (uintptr_t)_slot->readAddressFromSlot() : 0, (uintptr_t)forwardedAddress);
#endif /* defined(OMR_SCAVENGER_TRACK_COPY_DISTANCE) */
		_metrics->_volumeMetrics[objects] += 1;
		_metrics->_threadMetrics[stalled + (*_controller->sampleStalledMap() & (uintptr_t)0xff)] += 1;
		Debug_MM_true(_condition < condition_states);
		_metrics->_conditionMetrics[_condition] += 1;
		if (isConditionSet(pointer_array)) {
			_metrics->_arrayVolumeCounts[array_counters] += sizeAfterCopy;
			intptr_t bin = MM_Math::floorLog2(sizeAfterCopy);
			if (array_counters <= bin) {
				bin = array_counters - 1;
			}
			_metrics->_arrayVolumeCounts[bin] += 1;
		}
		_stats->countObjectSize(sizeAfterCopy);
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
		_stats->_hashBytes += (sizeAfterCopy - sizeBeforeCopy);

		/* copy the preserved fields from the forwarded header into the destination object */
		forwardedHeader->fixupForwardedObject(forwardedAddress);
		/* object model updates object age and finalizes copied object header */
		_objectModel->fixupForwardedObject(forwardedHeader, forwardedAddress, nextAge);

#if defined(EVACUATOR_DEBUG)
		Debug_MM_address(forwardedAddress);
		if (isDebugCopy()) {
			_delegate.debugValidateObject(forwardedAddress, sizeAfterCopy);
		}
#endif /* defined(EVACUATOR_DEBUG) */
	}

	Debug_MM_true(isInSurvivor(forwardedAddress) || isInTenure(forwardedAddress) || (isInEvacuate(forwardedAddress) && isAbortedCycle()));
	return forwardedAddress;
}

bool
MM_Evacuator::refresh(CopySpace *copyspace, Region region, uintptr_t size)
{
	Debug_MM_true(!whitesize(copyspace, size));
	Debug_MM_true(copyspace->_base == copyspace->_copy);
	Debug_MM_true((NULL == copyspace->_base) || (region == source(copyspace)));

	Whitespace *whitespace = NULL;

	/* try whitelist first for leaf or large object */
	if (isLeafCondition() || isLargeObject(size)) {
		whitespace = _whiteList[region].top(size);
	}

	/* if object size not greater than tlh minimum size try tlh allocation */
	if ((NULL == whitespace) && (size <= _controller->_maximumCopyspaceSize)) {
		if (_whiteList[region].top() >= _controller->_minimumCopyspaceSize) {
			whitespace = _whiteList[region].top(size);
		} else {
			whitespace = _controller->getWhitespace(this, region, size);
		}
	}

	/* for failed tlh or object size greater than tlh minimum size allocate for object size */
	if (NULL == whitespace) {
		whitespace = _controller->getObjectWhitespace(this, region, size);
	}

	/* try to steal whitespace from another copyspace as a last resort */
	if (NULL == whitespace) {
		if (_whiteList[region].top() >= size) {
			/* refresh from whitelist */
			whitespace = _whiteList[region].top(size);
		} else if (index(copyspace) < overflow) {
			/* try to refresh inside/outside copyspace from overflow copyspace */
			CopySpace * const overflowCp = &_copyspace[overflow];
			if ((region == source(overflowCp)) && whitesize(overflowCp, size)) {
				whitespace = trim(overflowCp);
			} else if (index(copyspace) < outsideSurvivor) {
				/* try to refresh inside copyspace with whitespace from outside copyspace */
				CopySpace * const outsideCp = outside(region);
				if (whitesize(outsideCp, size)) {
					/* trim whitespace from outside copyspace to refresh inside copyspace */
					whitespace = Whitespace::whitespace(outsideCp->_end - size, size,  whiteFlags(flagged(outsideCp, isLOA)));
					outsideCp->_end -= size;
				}
			} else {
				/* inside deflection may have been inhibited when considering outside copy */
				CopySpace * const insideCp = inside(region);
				if ((region == source(insideCp)) && whitesize(insideCp, size)) {
					/* trim whitespace from inside copyspace to refresh outside copyspace */
					whitespace = Whitespace::whitespace(insideCp->_end - size, size,  whiteFlags(flagged(insideCp, isLOA)));
					insideCp->_end -= size;
				}
			}
		} else {
			/* refresh overflow copyspace from inside/outside copyspace for region */
			CopySpace * const in = inside(region);
			CopySpace * const out = outside(region);
			CopySpace * const maxCp = ((in->_end - in->_copy) > (out->_end - out->_copy)) ? in : out;
			if (whitesize(maxCp, size)) {
				/* trim whitespace from inside copyspace to refresh outside copyspace */
				whitespace = Whitespace::whitespace(maxCp->_end - size, size,  whiteFlags(flagged(maxCp, isLOA)));
				maxCp->_end -= size;
			}
		}
	}

	/* dispose of remaining whitespace in copyspace and install new whitespace */
	if (NULL != whitespace) {
		_whiteList[region].add(trim(copyspace));
		copyspace->_base = copyspace->_copy = whitespace->getBase();
		copyspace->_end = copyspace->_base + whitespace->length();
		copyspace->_flags = (tenure == region) ? (uintptr_t)copyTenure : 0;
		copyspace->_flags |= (isLeafCondition() && (index(copyspace) == overflow)) ? isLeaves : 0;
		copyspace->_flags |= whitespace->isLOA() ? isLOA : 0;
		Debug_MM_true(copyspace->_end >= copyspace->_base);
		Debug_MM_true(size <= (uintptr_t)(copyspace->_end - copyspace->_base));
		Debug_MM_true((uintptr_t)whitespace == (uintptr_t)whitespace->getBase());
		Debug_MM_true(nil(_rp[region]) || ((outside(region)->_base > _rp[region]->_scan) || (_rp[region]->_scan >= outside(region)->_end)));
	}

	return (NULL != whitespace);
}

uint8_t *
MM_Evacuator::rebase(CopySpace *copyspace, uintptr_t *length)
{
	uint8_t *work = NULL;
	Debug_MM_true(source(copyspace) == sourceOf(copyspace->_base));
	Debug_MM_true(copyspace->_base < copyspace->_copy);
	Debug_MM_true(copyspace->_copy <= copyspace->_end);

	*length = (uintptr_t)(copyspace->_copy - copyspace->_base);
	if (0 < *length) {
		/* cut work from copyspace and advance base to copy head */
		work = copyspace->_base;
		copyspace->_base = copyspace->_copy;
	}

	Debug_MM_true(copyspace->_base <= copyspace->_copy);
	Debug_MM_true(copyspace->_copy <= copyspace->_end);
	return work;
}

MM_Evacuator::Whitespace *
MM_Evacuator::trim(CopySpace *copyspace)
{
	Debug_MM_true(copyspace->_base <= copyspace->_copy);
	Debug_MM_true(copyspace->_copy <= copyspace->_end);

	/* trim clipped whitespace from copyspace and reset empty copyspace */
	uintptr_t size = (uintptr_t)(copyspace->_end - copyspace->_copy);
	uintptr_t flags = whiteFlags(flagged(copyspace, isLOA));
	Whitespace *whitespace = Whitespace::whitespace(copyspace->_copy, size, flags);
	copyspace->_end = copyspace->_copy;

	/* reset copyspace if empty */
	if (copyspace->_base == copyspace->_end) {
		reset(copyspace);
	}

	/* wrap tailing whitespace in a Whitespace frame to return */
	return whitespace;
}

void
MM_Evacuator::reset(CopySpace *copyspace) const
{
	Debug_MM_true((copyspace->_base == copyspace->_copy) && (copyspace->_copy == copyspace->_end));
	copyspace->_base = copyspace->_copy = copyspace->_end = NULL;
	copyspace->_flags = 0;
}

void
MM_Evacuator::pull(CopySpace *copyspace)
{
	Debug_MM_true(isConditionSet(scan_worklist));
	Debug_MM_true(copyspace->_base < copyspace->_copy);
	Debug_MM_true(source(copyspace) == sourceOf(copyspace->_base));

	/* pull work into bottom stack frame and rebase copyspace */
	uintptr_t length = 0;
	uint8_t *work = rebase(copyspace, &length);
	_stack->_flags |= isPulled;
	push(_stack, sourceOf(work), work, work + length);

	Debug_MM_true((_sp->_base <= _sp->_scan) && (_sp->_scan < _sp->_end));
}

void
MM_Evacuator::pull(ScanSpace *sp, Region region, const Workspace *workspace)
{
	Debug_MM_address(workspace->base);

	sp->_flags |= isPulled;
	uintptr_t arrayHeaderSize = 0;
	if (isSplitArrayWorkspace(workspace)) {
		Debug_MM_true(flagged(sp, isNull));
		sp->_flags &= ~(uintptr_t)isNull;
		const uintptr_t startSlot = workspace->offset - 1;
		const uintptr_t slotCount = workspace->length;
		/* actual scan head is within the delegated split array object scanner -- scanspace bounds and scan head remain fixed until popped */ 
		_delegate.getSplitPointerArrayObjectScanner(workspace->object, &sp->_scanner, startSlot, slotCount, GC_ObjectScanner::scanHeap);
		/* scanspace _scan points to array object, _base & _end point to range of slots to scan */
		_sp = sp;
		_sp->_object = workspace->object;
		_sp->_base = (uint8_t *)scanner(_sp)->getScanSlotAddress();
		_sp->_end = (uint8_t *)GC_SlotObject::addToSlotAddress((fomrobject_t *)_sp->_base, slotCount, compressObjectReferences());
		_sp->_flags |= (region | isSplitArray);
		/* first split array segment accounts for volume of object header and alignment padding etc */
		if (0 == startSlot) {
			const uintptr_t arrayBytes = ((GC_IndexableObjectScanner *)scanner(_sp))->getIndexableRange() * getReferenceSlotSize();
			arrayHeaderSize = _objectModel->getConsumedSizeInBytesWithHeader(workspace->object) - arrayBytes;
			_metrics->_volumeMetrics[scanned] += arrayHeaderSize;
		}
		Debug_MM_true((outside(region)->_base > _sp->_scan) || (_sp->_scan >= outside(region)->_end));
	} else {
		/* push work from workspace into next stack frame */
		push(sp, region, workspace->base, workspace->base + workspace->length);
	}
}

void
MM_Evacuator::pull(MM_EvacuatorWorklist *worklist)
{
	Debug_MM_true(nil(_sp));
	Debug_MM_true(NULL != worklist);
	Debug_MM_true((worklist == &_workList) || (0 == _workList.volume()));

	/* drain deferred workspaces first when pulling from other evacuator's worklist */
	const bool pullDeferred = (worklist != &_workList);

	/* make a local LIFO list of workspaces to be loaded bottom up onto the stack for scanning in worklist order */
	Workspace *workStack = worklist->next(pullDeferred);
	uintptr_t pulledVolume = worklist->volume(workStack);
	if (0 == pulledVolume) {
		return;
	}

	/* make local FIFO lists of workspaces to be pulled into local worklist scanning in worklist order */
	MM_EvacuatorWorklist::List defaultList;
	defaultList.head = defaultList.tail = NULL;
	MM_EvacuatorWorklist::List deferList;
	deferList.head = deferList.tail = NULL;

	/* if pulling from own worklist limit the take to head when other evacuators are stalling */
	if (pullDeferred || !isConditionSet(stall)) {
		/* limit the number of workspaces that can be pulled into stack frames */
		uintptr_t limit = (_nil - _stack) >> 1;
		/* pull workspaces from source worklist until pulled volume levels up with source worklist volume  */
		const Workspace *peek = worklist->peek(pullDeferred);
		while (NULL != peek) {
			Debug_MM_true(0 < peek->length);
			/* if under stack limit pull next workspace into stack list */
			uintptr_t nextVolume = worklist->volume(peek);
			if ((pulledVolume + nextVolume) <= (worklist->volume() - nextVolume)) {
				if (0 < limit) {
					/* pull next workspace from source worklist and prepend to LIFO stack list (first in list -> first scanned in top stack frame) */
					Workspace *workspace = worklist->next(pullDeferred);
#if defined(EVACUATOR_DEBUG)
					walk(workspace);
#endif /* defined(EVACUATOR_DEBUG) */
					if ((workspace->base == (workStack->base + workStack->length))
					&& (_controller->_maximumWorkspaceSize >= (workStack->length + workspace->length))
					&& ((0 == workStack->offset) && (0 == workspace->offset))
					){
						/* allow coalescing of mixed workspaces if pulling from own worklist and worklist is fat with work */
						workStack->length += workspace->length;
						_freeList.add(workspace);
					} else {
						workspace->next = workStack;
						workStack = workspace;
					}
					peek = worklist->peek(pullDeferred);
					limit -= 1;
				} else if (pullDeferred) {
					/* work stack list is full, own worklist is dry and source worklist belongs to other evacuator -- add to local list */
					Workspace *workspace = worklist->next(true);
					_workList.addWorkToList(isSplitArrayWorkspace(workspace) ? &deferList : &defaultList, workspace);
					peek = worklist->peek(true);
				} else {
					break;
				}
				pulledVolume += nextVolume;
			} else {
				break;
			}
		}
	}

	/* pull workspaces from local lists into own worklist */
	if (NULL != defaultList.head) {
		_workList.add(&defaultList);
	}
	if (NULL != deferList.head) {
		_workList.add(&deferList);
	}

	/* pull workspaces from stack list into stack frames (last pulled workspace on the bottom) */
	ScanSpace *fp = _stack;
	uintptr_t pulled = 0;
	for (Workspace *workspace = workStack; NULL != workspace; fp += 1) {
#if defined(EVACUATOR_DEBUG)
		Debug_MM_true(0 < workspace->length);
		Debug_MM_true((fp->_base == fp->_scan) && (fp->_scan == fp->_end));
		walk(workspace);
#endif /* defined(EVACUATOR_DEBUG) */

		/* push workspace into stack frame */
		uintptr_t volume = _workList.volume(workspace);
		pull(fp, sourceOf(workspace->base), workspace);
		_env->_scavengerStats.countWorkPacketSize(volume, OMR_SCAVENGER_WORKSIZE_BINS * min_workspace_size);
		workspace = _freeList.add(workspace);
		pulled += volume;

		Debug_MM_true(flagged(fp, isSplitArray) || ((fp->_base == fp->_scan) && (fp->_scan < fp->_end)));
		Debug_MM_true(!flagged(fp, isSplitArray) || ((fp->_scan < fp->_base) && (fp->_base < fp->_end)));
	}
	_metrics->_threadMetrics[pulled_volume] += pulled;
	_metrics->_threadMetrics[pulled_count] += 1;

#if defined(EVACUATOR_DEBUG)
	Debug_MM_true((0 == pulledVolume) || !nil(_sp));
	Debug_MM_true((_stack <= _sp) && (_sp < _nil));
	if (pullDeferred) {
		uintptr_t stackVolume = 0;
		for (ScanSpace *frame = _stack; frame <= _sp; frame += 1) {
			if (!flagged(frame, isSplitArray)) {
				stackVolume += (frame->_end - frame->_scan);
			} else {
				stackVolume += (frame->_end - frame->_base);
			}
		}
		Debug_MM_true(pulledVolume == (stackVolume + _workList.volume()));
	}
#endif /* defined(EVACUATOR_DEBUG) */
}

bool
MM_Evacuator::getWork()
{
	/* scan stack is empty */
	Debug_MM_true(nil(_sp));
	if (!nil(_sp)) {
		return true;
	}

	/* ** evacuator must clear (or abandon if aborting) outside & overflow copyspaces and worklist before waiting for work/completion/abort ** */

	/* first try to pull work from worklist */
	if (0 < _workList.volume()) {
		omrthread_monitor_enter(_mutex);
		if (0 < _workList.volume()) {
			/* load up the bottom half of the stack with work pulled from local worklist */
			pull(&_workList);
		}
		omrthread_monitor_exit(_mutex);
		if (!nil(_sp)) {
			/* there may be some work left in worklist for distribution */
			_controller->notifyOfWork(this);
			return true;
		}
	}

	/* if no work from worklist look for work between base and copy head in outside or large copyspace */
	CopySpaces cs = copyspaces;
	const intptr_t outsideWork[] = {
		(outside(survivor)->_copy - outside(survivor)->_base)
	  , (outside(tenure)->_copy - outside(tenure)->_base)
	};
	if (0 < (outsideWork[survivor] + outsideWork[tenure])) {
		/* outside copyspace selection depends on volume of work and tail filling conditions */
		cs = outsideSurvivor;
		if ((0 < outsideWork[survivor]) && (0 < outsideWork[tenure])) {
			const bool tailfilling[] = { isConditionSet(copyspaceTailFillCondition(survivor)), isConditionSet(copyspaceTailFillCondition(tenure)) };
			if (tailfilling[survivor] == tailfilling[tenure]) {
				if (outsideWork[survivor] < outsideWork[tenure]) {
					cs = outsideTenure;
				}
			} else if (tailfilling[survivor]) {
				cs = outsideTenure;
			}
		} else if (0 < outsideWork[tenure]) {
			cs = outsideTenure;
		}
		Debug_MM_true((outsideWork[(Region)(cs - 2)] >= outsideWork[(Region)(3 - cs)]) || isConditionSet(copyspaceTailFillCondition((Region)(3 - cs)))); // cs in {2,3} ie outside{Survivor,Tenure}
	} else {
		/* if overflow copyspace has accumulated leaf objects it has no scan work and must be flushed at this point */
		CopySpace * const overflowCp = &_copyspace[overflow];
		const intptr_t overflowWork = overflowCp->_copy - overflowCp->_base;
		if (0 < overflowWork) {
			if (flagged(overflowCp, isLeaves)) {
				/* overflow copyspace has nothing to scan, flush leaves and rebase but do not select */
				_metrics->_volumeMetrics[leaf] += overflowWork;
				overflowCp->_flags &= ~(uintptr_t)isLeaves;
				overflowCp->_base = overflowCp->_copy;
			} else {
				/* select overflow copyspace to pull non-leaf contents */
				cs = overflow;
			}
		}
	}

	/* if a copyspace is selected pull work and rebase */ 
	if (cs < copyspaces) {
		pull(&_copyspace[cs]);
		Debug_MM_true(!nil(_sp));
		return true;
	}

	/* aborting or not, if take the controller mutex and wait for work or to sync and complete or abort scan */
	if (nil(_sp) || isAbortedCycle()) {
		/* if no work and not aborting at this point the worklist and both outside copyspaces must be empty */
		Assert_MM_true(isAbortedCycle() || ((0 == (_workList.volume() + outsideWork[survivor] + outsideWork[tenure] + _copyspace[overflow]._copy - _copyspace[overflow]._base))));
		/* if another evacuator aborted while work was pulled above clear the stack and abandon pulled work */
		_sp = _rp[survivor] = _rp[tenure] = _nil;
		/* flush local buffers and metrics before getting on the bus to poll other evacuators for scan work */
		flushForWaitState();

		/* loop until work pulled from other evacuator or all sync on complete/abort condition */
		_controller->enter(_env, _controller->_workMutex, stall_count);
		do {
			/* stop the bus if aborting, otherwise iterate starting with evacuator with greatest worklist volume */
			if (!isAbortedCycle()) {
				/* if any polled evacuator offers work it is pulled from worklist until stack volume levels up with worklist */
				findWork();
			}
			/* if no work pulled wait for another evacuator to notify of work or end of heap scan */
		} while (_controller->waitForWork(this));
		/* get off the bus and continue scanning stack, or complete/abort heap scan if stack is empty */
		_controller->leave(_env, _controller->_workMutex);

		/* empty stack at this point means all evacuators have completed or aborted heap scan */
		if (nil(_sp)) {
			scanComplete();
		}
	}

	/* it may be that this evacuator has work and controller has received abort from another evacuator -- that's ok :) */
	return !nil(_sp);
}

void
MM_Evacuator::findWork()
{
	Debug_MM_true(!isAbortedCycle());

	/* select as prospective donor the evacuator with greatest volume of distributable work */
	uintptr_t maxVolume = 0;
	MM_Evacuator *maxDonor = NULL;
	for (MM_Evacuator *next = _controller->getNextEvacuator(this); next != this; next = _controller->getNextEvacuator(next)) {
		uintptr_t volume = next->getDistributableVolumeOfWork();
		if (maxVolume < volume) {
			maxVolume = volume;
			maxDonor = next;
		}
	}

	/*if no prospective donor just return */
	if (NULL != maxDonor) {
		/* starting with max donor poll evacuators for distributable work */
		MM_Evacuator *donor = maxDonor;
		do {
			/* do not wait if donor is involved with its worklist */
			if (0 < donor->getDistributableVolumeOfWork()) {
				/* retest donor volume of work after locking its worklist */
				uint64_t time = _controller->knock(_env, donor->_mutex);
				if (0 < time) {
					/* pull work from donor worklist into stack and worklist until pulled volume levels up with donor remainder */
					if (0 < donor->getDistributableVolumeOfWork()) {
						pull(&donor->_workList);
					}
					_controller->leave(_env, donor->_mutex, time, stall_count);
					 /* break to scan work pulled into the stack */
					if (!nil(_sp)) {
						break;
					}
				}
			}
			/* keep polling until donor wraps around to max donor */
			donor = _controller->getNextEvacuator(donor);
		} while (donor != maxDonor);
	}
}

uintptr_t
MM_Evacuator::getDistributableVolumeOfWork() const
{
	/* this method samples volatile _workList._volume and may return 0 < result <= quota when called without evacuator mutex */
	return (_workList.volume() > _controller->getWorkDistributionQuota()) ? _workList.volume() : 0;
}

void
MM_Evacuator::addWork(CopySpace *copyspace)
{
	if (copyspace->_base < copyspace->_copy) {
		/* pull work from copyspace into worklist, deferring if work was copied in stack overflow condition */
		Workspace *workspace = _freeList.next();
		workspace->base = rebase(copyspace, &workspace->length);

#if defined(EVACUATOR_DEBUG)
		walk(workspace);
#endif /* defined(EVACUATOR_DEBUG) */

		omrthread_monitor_enter(_mutex);
		_workList.add(workspace, isConditionSet(stack_overflow));
		omrthread_monitor_exit(_mutex);

		/* notify controller */
		_controller->notifyOfWork(this);
	}
}

void
MM_Evacuator::splitPointerArrayWork(CopySpace *copyspace, uint8_t *arrayAddress)
{
	Debug_MM_true(!flagged(copyspace, isLeaves));
	Debug_MM_true(arrayAddress < copyspace->_end);

	/* flush any unscanned work preceding array in copyspace */
	Workspace *workspace = NULL;
	if (copyspace->_base < arrayAddress) {
		workspace = _freeList.next();
		workspace->base = copyspace->_base;
		workspace->length = (uintptr_t)(arrayAddress - copyspace->_base);
#if defined(EVACUATOR_DEBUG)
		walk(workspace);
#endif /* defined(EVACUATOR_DEBUG) */
		copyspace->_base = arrayAddress;
	}

	/* rebase copyspace to extract pointer array */
	uintptr_t volume = 0, elements = 0;
	omrobjectptr_t pointerArray = (omrobjectptr_t)rebase(copyspace, &volume);
	Debug_MM_true((uint8_t*)pointerArray == arrayAddress);

	/* distribute elements to segments as evenly as possible and take largest segment first */
	_delegate.getIndexableDataBounds(pointerArray, &elements);
	uintptr_t elementsPerSegment = (_controller->_minimumWorkspaceSize >> 1) / _sizeofObjectReferenceSlot;
	uintptr_t segments = elements / elementsPerSegment;
	elementsPerSegment = elements / segments;
	uintptr_t elementsThisSegment = elementsPerSegment + (elements - (segments * elementsPerSegment));

	/* record 1-based array offsets to mark split array workspaces */
	MM_EvacuatorWorklist::List splitlist;
	splitlist.head = splitlist.tail = NULL;
	uintptr_t offset = 1;
	while (0 < segments) {
		/* wrap each segment in a split array workspace */
		Workspace* segment = _freeList.next();
		segment->object = pointerArray;
		segment->offset = offset;
		segment->length = elementsThisSegment;
#if defined(EVACUATOR_DEBUG)
		walk(segment);
#endif /* defined(EVACUATOR_DEBUG) */
		/* append split array workspace to splitlist */
		_workList.addWorkToList(&splitlist, segment);
		/* all split array workspaces but the first have equal volumes of work */
		offset += elementsThisSegment;
		elementsThisSegment = elementsPerSegment;
		segments -= 1;
	}

	/* lock worklist to add flushed and segmented workspaces */
	omrthread_monitor_enter(_mutex);
	/* add flushed work to default queue */
	if (NULL != workspace) {
		_workList.add(workspace, false);
	}
	/* defer segmented workspaces */
	_workList.add(&splitlist, true);
	omrthread_monitor_exit(_mutex);

	/* notify controller */
	_controller->notifyOfWork(this);
}

bool
MM_Evacuator::selectCondition(ConditionFlag condition, bool force)
{
	const bool previously = isConditionSet(condition);
	setCondition(condition, force || isScanOptionSelected(condition));
	return previously;
}

void
MM_Evacuator::setCondition(ConditionFlag condition, bool value)
{
	Debug_MM_true(((uintptr_t)1 << MM_Math::floorLog2((uintptr_t)condition)) == (uintptr_t)condition);

	if (value != isConditionSet(condition)) {
		if (value) {
			_condition |= (uintptr_t)condition;
		} else {
			_condition &= ~(uintptr_t)condition;
		}
	}
}

bool
MM_Evacuator::isConditionSet(uintptr_t conditions) const
{
	/* true if any condition in conditions is set */
	return (0 != (_condition & conditions));
}

bool
MM_Evacuator::areConditionsSet(uintptr_t conditions) const
{
	/* true if all conditions in conditions are set */
	return (conditions == (_condition & conditions));
}

bool
MM_Evacuator::isLeafCondition() const
{
	/* this tests conditions that force breadth first stack operation  */
	return isConditionSet(leaf_object | indexed_primitive);
}

bool
MM_Evacuator::isBreadthFirstCondition() const
{
	/* this tests conditions that force breadth first stack operation  */
	return isConditionSet(breadth_first_always | breadth_first_roots);
}

bool
MM_Evacuator::isForceOutsideCopyCondition(Region region, uintptr_t size) const
{
	/* force outside pointer arrays if splitable or pumping worklist volume, everything for breadth-first copy or tail filling */
	return (isConditionSet(pointer_array) && (isConditionSet(stall | stack_overflow)))
		|| isConditionSet(copyspaceTailFillCondition(region) | breadth_first_always | breadth_first_roots)
		|| (nil(_sp + 1) && (nil(_rp[region]) || (_sp->_base < _sp->_scan))
	);
}

bool
MM_Evacuator::isForceOverflowCopyCondition(Region region, uintptr_t size) const
{
	/* direct leaf objects to overflow copyspace unless tail filling to clear whitespace or stall/stack overflow condition set to accelerate work release */
	return isLeafCondition()
		&& !isConditionSet(stall | stack_overflow | copyspaceTailFillCondition(region));
}

MM_Evacuator::ConditionFlag
MM_Evacuator::copyspaceTailFillCondition(Region region) const
{
	/* return copyspace overflow condition for the specific region */
	return (ConditionFlag)((uintptr_t)survivor_tail_fill << region);
}

MM_Evacuator::CopySpace *
MM_Evacuator::inside(Region region)
{
	return &_copyspace[insideSurvivor + region];
}

MM_Evacuator::CopySpace *
MM_Evacuator::outside(Region region)
{
	return &_copyspace[outsideSurvivor + region];
}

GC_ObjectScanner *
MM_Evacuator::scanner(const ScanSpace *frame) const
{
	return (GC_ObjectScanner *)&frame->_scanner;
}

bool
MM_Evacuator::cached(const uint8_t *copyhead) const
{
	/* return true only if there is a stack frame scanning inside copyspace for scan region */
	const uintptr_t delta = (uintptr_t)scanner(_sp)->getScanSlotAddress() ^ (uintptr_t)copyhead;
	Debug_MM_true((cache_line_size <= delta) || (_sp->_end <= copyhead));
	return (cache_line_size > delta);
}

void
MM_Evacuator::reset(ScanSpace *frame) const
{
	Debug_MM_true((frame->_base <= frame->_scan) && (frame->_scan == frame->_end));
	GC_NullObjectScanner::newInstance(_env, &frame->_scanner);
	frame->_base = frame->_scan = frame->_end = NULL;
	frame->_flags = isNull;
}

bool
MM_Evacuator::nil(const ScanSpace *frame) const
{
	return (_nil == frame);
}

MM_Evacuator::Region
MM_Evacuator::source(const ScanSpace *frame) const
{
	Debug_MM_true((frame->_base == frame->_end) || flagged(frame, scanTenure) == isInTenure(frame->_base));
	Debug_MM_true((frame->_base == frame->_end) || !flagged(frame, scanTenure) == isInSurvivor(frame->_base));
	return (flagged(frame, scanTenure)) ? tenure : survivor;
}

bool
MM_Evacuator::flagged(const ScanSpace *frame, uintptr_t flags) const
{
	return (0 != (frame->_flags & flags));
}

bool
MM_Evacuator::flagged(const CopySpace *copyspace, uintptr_t flags) const
{
	return (0 != (copyspace->_flags & flags));
}

MM_Evacuator::Region
MM_Evacuator::source(const CopySpace *copyspace) const
{
	Debug_MM_true((copyspace->_base == copyspace->_end) || flagged(copyspace, scanTenure)  == isInTenure(copyspace->_base));
	Debug_MM_true((copyspace->_base == copyspace->_end) || !flagged(copyspace, scanTenure) == isInSurvivor(copyspace->_base));
	return (flagged(copyspace, copyTenure)) ? tenure : survivor;
}

MM_Evacuator::CopySpaces
MM_Evacuator::index(const CopySpace *copyspace) const
{
	intptr_t offset = copyspace - &_copyspace[0];
	Debug_MM_true(offset < copyspaces);
	return (CopySpaces)offset;
}

bool
MM_Evacuator::worksize(const CopySpace *copyspace, uintptr_t size) const
{
	Debug_MM_true((copyspace->_base <= copyspace->_copy) && (copyspace->_copy <= copyspace->_end));
	return (((copyspace->_base + size) <= copyspace->_copy) && !addressOverflow(copyspace->_base, size));
}

bool
MM_Evacuator::whitesize(const CopySpace *copyspace, uintptr_t size) const
{
	Debug_MM_true((copyspace->_base <= copyspace->_copy) && (copyspace->_copy <= copyspace->_end));
	return (((copyspace->_copy + size) <= copyspace->_end) && !addressOverflow(copyspace->_copy, size));
}

bool
MM_Evacuator::isLargeObject(const uintptr_t objectSizeInBytes) const
{
	/* any object that might be a splitable pointer array is a very large object */
	return (_controller->_minimumCopyspaceSize <= objectSizeInBytes);
}

bool
MM_Evacuator::isSplitablePointerArray(const uintptr_t objectSizeInBytes) const
{
	/* this will not work if pointer_array becomes a selectable scan option */
	Debug_MM_true(0 == (pointer_array & options_mask));
	
	/* only large pointer arrays can be split into segments */
	return isConditionSet(pointer_array) && (_controller->_minimumWorkspaceSize <= objectSizeInBytes);
}

bool
MM_Evacuator::isSplitArrayWorkspace(const Workspace *work) const
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
	Debug_MM_address(objectPtr);

	/* scan object for referents in the nursery */
	MM_EvacuatorDelegate::ScannerState scanState;
	const uintptr_t scanFlags = GC_ObjectScanner::scanRoots | GC_ObjectScanner::indexableObjectNoSplit;
	GC_ObjectScanner *objectScanner = _delegate.getObjectScanner(objectPtr, &scanState, scanFlags);
	if (NULL != objectScanner) {
		const GC_SlotObject *slotPtr;
		while (NULL != (slotPtr = objectScanner->getNextSlot())) {
			omrobjectptr_t slotObjectPtr = slotPtr->readReferenceFromSlot();
			if (NULL != slotObjectPtr) {
				if (isInSurvivor(slotObjectPtr)) {
					return true;
				}
				Debug_MM_true(isInTenure(slotObjectPtr) || isAbortedCycle() || !isInEvacuate(slotObjectPtr));
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
MM_Evacuator::remember(omrobjectptr_t object, uintptr_t rememberedState)
{
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
		if ((_env->_scavengerRememberedSet.fragmentCurrent < _env->_scavengerRememberedSet.fragmentTop)
		|| (0 == allocateMemoryForSublistFragment(_env->getOmrVMThread(), (J9VMGC_SublistFragment*)&_env->_scavengerRememberedSet))
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

uintptr_t
MM_Evacuator::whiteFlags(bool isLoa) const
{
	uintptr_t flags = Whitespace::hole;
	if (isLoa) {
		flags |= Whitespace::loa;
	}
	if (compressObjectReferences()) {
		flags |= Whitespace::compress;
	}
	if (isDebugPoisonDiscard()) {
		flags |= Whitespace::poisoned;
	}
	return flags;
}

void
MM_Evacuator::receiveWhitespace(Region region, Whitespace *whitespace)
{
	/* this is tlh whitespace reserved but not used for copy */
	_whiteList[region].add(whitespace);
}

void
MM_Evacuator::flushWhitespace(Region region)
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
	_controller->acquireController();
	if (!_controller->setAborting()) {
#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		/* report only if this evacuator is first to set the abort condition */
		OMRPORT_ACCESS_FROM_ENVIRONMENT(_env);
		omrtty_printf("abort[%2lu]; flags:%llx", _workerIndex, _controller->sampleEvacuatorFlags());
		if (!nil(_sp)) {
			omrtty_printf("; scanning:0x%llx", (uintptr_t)_sp->_scan);
		}
		omrtty_printf("\n");
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
	}
	_controller->releaseController();

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

#if defined(EVACUATOR_DEBUG)
void
MM_Evacuator::walk(const Workspace *workspace)
{
	if (isDebugPoisonDiscard()) {
		Debug_MM_true((NULL != workspace) && (0 < workspace->length));
		Debug_MM_true((0 == ((uintptr_t )workspace % sizeof(uintptr_t))));
		uintptr_t *a = (uintptr_t*)(workspace->base), *z = NULL, e = 0;
		Debug_MM_true(!Whitespace::isDead(a));
		if (0 == workspace->offset) {
			z = a + (workspace->length / sizeof(uintptr_t));
			a += _objectModel->getConsumedSizeInBytesWithHeader((omrobjectptr_t) (a)) / sizeof(uintptr_t);
			e = 0;
		} else {
			a = (uintptr_t*) (_delegate.getIndexableDataBounds(workspace->object, &e));
			a += (((workspace->offset - 1) * getReferenceSlotSize()) / sizeof(uintptr_t));
			z = a + ((workspace->length * getReferenceSlotSize()) / sizeof(uintptr_t));
			e = 1;
		}
		while (a < z) {
			Debug_MM_true(!Whitespace::isDead(a));
			a += (e == 0) ? (_objectModel->getConsumedSizeInBytesWithHeader((omrobjectptr_t)a) / sizeof(uintptr_t)) : 1;
		}
		Debug_MM_true(a == z);
	}
}
#endif /* defined(EVACUATOR_DEBUG) */

