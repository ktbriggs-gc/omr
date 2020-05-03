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

#include <stdint.h>
#include "omrport.h"

#include "AllocateDescription.hpp"
#include "AtomicSupport.hpp"
#include "Bits.hpp"
#include "CollectionStatisticsStandard.hpp"
#include "Dispatcher.hpp"
#include "EvacuatorController.hpp"
#include "EvacuatorScanspace.hpp"
#include "Math.hpp"
#include "MemorySubSpace.hpp"
#include "MemorySubSpaceSemiSpace.hpp"
#include "ScavengerCopyScanRatio.hpp"
#include "ScavengerStats.hpp"

bool
MM_EvacuatorController::setEvacuatorFlag(uintptr_t flag, bool value)
{
	uintptr_t oldFlags;

	if (value) {
		oldFlags = VM_AtomicSupport::bitOr(&_evacuatorFlags, flag);
	} else {
		oldFlags = VM_AtomicSupport::bitAnd(&_evacuatorFlags, ~flag);
	}
	VM_AtomicSupport::readBarrier();

	return (flag == (flag & oldFlags));
}

bool
MM_EvacuatorController::setAborting()
{
	/* test & set the aborting flag, return false if not previously set */
	return setEvacuatorFlag(aborting, true);
}

bool
MM_EvacuatorController::initialize(MM_EnvironmentBase *env)
{
	bool result = true;

	/* evacuator model is not instrumented for concurrent scavenger */
	Assert_MM_true(!_extensions->isEvacuatorEnabled() || !_extensions->isConcurrentScavengerEnabled());

	if (_extensions->isEvacuatorEnabled()) {
		/* if jvm is only user process cpu would likely stall if thread yielded to wait on the controller mutex so enable spinning */
		if (0 != omrthread_monitor_init_with_name(&_controllerMutex, 0, "MM_EvacuatorController::_controllerMutex")) {
			_controllerMutex = NULL;
			return false;
		}

		/* evacuator never uses monitor-enter and requires monitor-try-enter semantics to *not* yield cpu for reporter mutex so disable spinning */
		if (0 != omrthread_monitor_init_with_name(&_reporterMutex, J9THREAD_MONITOR_DISABLE_SPINNING, "MM_EvacuatorController::_reporterMutex")) {
			omrthread_monitor_destroy(_controllerMutex);
			_controllerMutex = NULL;
			_reporterMutex = NULL;
			return false;
		}

		for (uintptr_t workerIndex = 0; workerIndex < _maxGCThreads; workerIndex++) {
			_evacuatorTask[workerIndex] = NULL;
		}

		/* initialize heap region bounds, copied here at the start of each gc cycle for ease of access */
		for (MM_Evacuator::Region region = MM_Evacuator::survivor; region <= MM_Evacuator::evacuate; region = MM_Evacuator::nextEvacuationRegion(region)) {
			_heapLayout[region][0] = NULL;
			_heapLayout[region][1] = NULL;
			_memorySubspace[region] = NULL;
		}

		result = (NULL != _controllerMutex);
	}

	return result;
}

void
MM_EvacuatorController::tearDown(MM_EnvironmentBase *env)
{
	if (_extensions->isEvacuatorEnabled()) {
		for (uintptr_t workerIndex = 0; workerIndex < _maxGCThreads; workerIndex++) {
			if (NULL != _evacuatorTask[workerIndex]) {
				Debug_MM_true(NULL == _evacuatorTask[workerIndex]->getEnvironment());
				_evacuatorTask[workerIndex]->kill();
				_evacuatorTask[workerIndex] = NULL;
			}
		}

		MM_Forge *forge = env->getForge();

		/* free the system memory bound to these const (not nullable) pointers */
		forge->free((void *)_boundEvacuatorBitmap);
		forge->free((void *)_stalledEvacuatorBitmap);
		forge->free((void *)_resumingEvacuatorBitmap);
		forge->free((void *)_evacuatorMask);
		forge->free((void *)_evacuatorTask);
#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		forge->free((void *)_stackActivations);
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

		if (NULL != _controllerMutex) {
			omrthread_monitor_destroy(_controllerMutex);
			_controllerMutex = NULL;
		}
		if (NULL != _reporterMutex) {
			omrthread_monitor_destroy(_reporterMutex);
			_reporterMutex = NULL;
		}
	}
}

bool
MM_EvacuatorController::collectorStartup(MM_GCExtensionsBase* extensions)
{
#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	if (extensions->isEvacuatorEnabled()) {
#if defined(EVACUATOR_DEBUG)
		if (MM_EvacuatorBase::isTraceOptionSelected(_extensions, EVACUATOR_DEBUG_END)) {
#endif /* defined(EVACUATOR_DEBUG) */
			OMRPORT_ACCESS_FROM_OMRVM(_omrVM);
			_collectorStartTime = omrtime_hires_clock();
			omrtty_printf("%5lu      :   startup; stack-depth:%lu; object-size:%lx; frame-width:%lx; work-size:%lx; work-quanta:%lx\n", _history.getEpoch()->gc,
				_extensions->evacuatorMaximumStackDepth, _extensions->evacuatorMaximumInsideCopySize, _extensions->evacuatorMaximumInsideCopyDistance,
				_extensions->evacuatorWorkQuantumSize, _extensions->evacuatorWorkQuanta);
#if defined(EVACUATOR_DEBUG)
		}
#endif /* defined(EVACUATOR_DEBUG) */
	}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

	return true;
}

void
MM_EvacuatorController::collectorShutdown(MM_GCExtensionsBase* extensions)
{
	flushTenureWhitespace(true);

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
#if defined(EVACUATOR_DEBUG)
	if (MM_EvacuatorBase::isTraceOptionSelected(_extensions, EVACUATOR_DEBUG_END)) {
#endif /* defined(EVACUATOR_DEBUG) */
		OMRPORT_ACCESS_FROM_OMRVM(_omrVM);
		uint64_t collectorElapsedMicros = omrtime_hires_delta(_collectorStartTime, omrtime_hires_clock(), OMRPORT_TIME_DELTA_IN_MICROSECONDS);
		omrtty_printf("%5lu      :  shutdown; elapsed:%llu\n", _history.getEpoch()->gc, collectorElapsedMicros);
#if defined(EVACUATOR_DEBUG)
	}
#endif /* defined(EVACUATOR_DEBUG) */
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
}

void
MM_EvacuatorController::flushTenureWhitespace(bool shutdown)
{
#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	uintptr_t flushed = 0, discarded = 0, recycled = 0;
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

	if (_extensions->isEvacuatorEnabled()) {
		for (uintptr_t workerIndex = 0; workerIndex < _maxGCThreads; workerIndex += 1) {
			if (NULL != _evacuatorTask[workerIndex]) {
				Debug_MM_true(NULL == _evacuatorTask[workerIndex]->getEnvironment());
				_evacuatorTask[workerIndex]->flushWhitespace(MM_Evacuator::tenure);
#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
				flushed += _evacuatorTask[workerIndex]->getFlushed();
				discarded += _evacuatorTask[workerIndex]->getDiscarded();
				recycled += _evacuatorTask[workerIndex]->getRecycled();
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
			}
		}
	}

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
#if defined(EVACUATOR_DEBUG)
	if (MM_EvacuatorBase::isTraceOptionSelected(_extensions, EVACUATOR_DEBUG_END)) {
#endif /* defined(EVACUATOR_DEBUG) */
		OMRPORT_ACCESS_FROM_OMRVM(_omrVM);
		omrtty_printf("%5lu      :%10s; tenure; discarded:%lx; flushed:%lx; recycled:%lx\n", _history.getEpoch()->gc,
				(shutdown ? "finalize" : "global gc"), flushed, discarded, recycled);
#if defined(EVACUATOR_DEBUG)
	}
#endif /* defined(EVACUATOR_DEBUG) */
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
}

void
MM_EvacuatorController::masterSetupForGC(MM_EnvironmentStandard *env)
{
	_evacuatorIndex = 0;
	_evacuatorFlags = 0;
	_evacuatorCount = 0;
	_finalDiscardedBytes = 0;
	_finalFlushedBytes = 0;
	_finalRecycledBytes = 0;
	_copiedBytes[MM_Evacuator::survivor] = 0;
	_copiedBytes[MM_Evacuator::tenure] = 0;
	_isNotifyOfWorkPending = 0;
	_scannedBytes = 0;

	/* reset the evacuator bit maps */
	uintptr_t tailWords = 0;
	uintptr_t bitmapWords = countEvacuatorBitmapWords(&tailWords);
	for (uintptr_t map = 0; map < bitmapWords; map += 1) {
		Debug_MM_true(0 == _boundEvacuatorBitmap[map]);
		_boundEvacuatorBitmap[map] = 0;
		Debug_MM_true(0 == _stalledEvacuatorBitmap[map]);
		_stalledEvacuatorBitmap[map] = 0;
		Debug_MM_true(0 == _resumingEvacuatorBitmap[map]);
		_resumingEvacuatorBitmap[map] = 0;
		_evacuatorMask[map] = 0;
	}
	_stalledEvacuatorCount = 0;

	/* set up controller's subspace and layout arrays to match collector subspaces */
	_memorySubspace[MM_Evacuator::evacuate] = _evacuateMemorySubSpace;
	_memorySubspace[MM_Evacuator::survivor] = _survivorMemorySubSpace;
	_memorySubspace[MM_Evacuator::tenure] = _tenureMemorySubSpace;

	_heapLayout[MM_Evacuator::evacuate][0] = (uint8_t *)_evacuateSpaceBase;
	_heapLayout[MM_Evacuator::evacuate][1] = (uint8_t *)_evacuateSpaceTop;
	_heapLayout[MM_Evacuator::survivor][0] = (uint8_t *)_survivorSpaceBase;
	_heapLayout[MM_Evacuator::survivor][1] = (uint8_t *)_survivorSpaceTop;
	_heapLayout[MM_Evacuator::tenure][0] = (uint8_t *)_extensions->_tenureBase;
	_heapLayout[MM_Evacuator::tenure][1] = _heapLayout[MM_Evacuator::tenure][0] + _extensions->_tenureSize;

	/* reset upper bounds for tlh allocation size, indexed by outside region -- these will be readjusted when thread count is stable */
	_copyspaceAllocationCeiling[MM_Evacuator::survivor] = _copyspaceAllocationCeiling[MM_Evacuator::tenure] = _maximumCopyspaceSize;
	_objectAllocationCeiling[MM_Evacuator::survivor] = _objectAllocationCeiling[MM_Evacuator::tenure] = ~(uintptr_t)0xff;

	/* prepare the evacuator delegate class and enable it to add private flags for the cycle */
	_evacuatorFlags |= MM_EvacuatorDelegate::prepareForEvacuation(env);
}

MM_Evacuator *
MM_EvacuatorController::bindWorker(MM_EnvironmentStandard *env)
{
	/* get an unbound evacuator instance */
	uintptr_t workerIndex = VM_AtomicSupport::add(&_evacuatorIndex, 1) - 1;
	VM_AtomicSupport::readBarrier();

	/* instantiate evacuator task for this worker thread if required (evacuators are instantiated once and persist until vm shuts down) */
	if (NULL == _evacuatorTask[workerIndex]) {
		_evacuatorTask[workerIndex] = MM_Evacuator::newInstance(workerIndex, this, _extensions);
		Assert_MM_true(NULL != _evacuatorTask[workerIndex]);
	}

	/* controller doesn't have final view on evacuator thread count until after first task is dispatched ... */
	if (0 == _evacuatorCount) {

		acquireController();

		/* ... so first evacuator to reach this point must complete thread count dependent initialization */
		if (0 == _evacuatorCount) {

			/* all evacuator threads must have same view of dispatched thread count at this point */
			VM_AtomicSupport::set(&_evacuatorCount, env->_currentTask->getThreadCount());
			fillEvacuatorBitmap(_evacuatorMask);
			VM_AtomicSupport::readBarrier();

			/* set upper bounds for tlh allocation size, indexed by outside region -- reduce these for small survivor spaces */
			uintptr_t copyspaceSize = _maximumCopyspaceSize;
			uintptr_t projectedEvacuationBytes = calculateProjectedEvacuationBytes();
			while ((copyspaceSize > _minimumCopyspaceSize) && ((4 * copyspaceSize * _evacuatorCount) > projectedEvacuationBytes)) {
				/* scale down tlh allocation limit until maximal cache size is small enough to ensure adequate distribution */
				copyspaceSize -= _minimumCopyspaceSize;
			}
			_copyspaceAllocationCeiling[MM_Evacuator::survivor] = _copyspaceAllocationCeiling[MM_Evacuator::tenure] = OMR_MAX(_minimumCopyspaceSize, copyspaceSize);

			/* reporting epochs partition survivor semispace to produce a preset number of epochs per gc cycle */
			_bytesPerReportingEpoch = projectedEvacuationBytes / MM_EvacuatorBase::epochs_per_cycle;

			/* on average each evacuator reports bytes scanned/copied at a preset number of points in each epoch */
			_copiedBytesReportingDelta = _bytesPerReportingEpoch / (_evacuatorCount * MM_EvacuatorBase::reports_per_epoch);

#if defined(EVACUATOR_DEBUG)|| defined(EVACUATOR_DEBUG_ALWAYS)
			OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
			_history.reset(_extensions->scavengerStats._gcCount, omrtime_hires_clock(), _copyspaceAllocationCeiling[MM_Evacuator::survivor], _copyspaceAllocationCeiling[MM_Evacuator::tenure]);
#if defined(EVACUATOR_DEBUG)
			if (MM_EvacuatorBase::isTraceOptionSelected(_extensions, EVACUATOR_DEBUG_END)) {
#endif /* defined(EVACUATOR_DEBUG) */
				omrtty_printf("%5lu      :  gc start; survivor{%lx %lx} tenure{%lx %lx} evacuate{%lx %lx}; threads:%lu; projection:%lx; allocation:%lx\n",
						_extensions->scavengerStats._gcCount,
						(uintptr_t)_heapLayout[0][0], (uintptr_t)_heapLayout[0][1],
						(uintptr_t)_heapLayout[1][0], (uintptr_t)_heapLayout[1][1],
						(uintptr_t)_heapLayout[2][0], (uintptr_t)_heapLayout[2][1],
						_evacuatorCount, projectedEvacuationBytes, copyspaceSize);
				omrtty_printf("%5lu      :   options;", _extensions->scavengerStats._gcCount);
				for (uintptr_t condition = 1; condition < MM_Evacuator::conditions_mask; condition <<= 1) {
					if (MM_Evacuator::isScanOptionSelected(_extensions, condition)) {
						omrtty_printf(" %s", MM_Evacuator::conditionName((MM_Evacuator::ConditionFlag)condition));
					}
				}
				omrtty_printf("\n");
#if defined(EVACUATOR_DEBUG)
			}
#endif /* defined(EVACUATOR_DEBUG) */
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
		}

		releaseController();
	}

	/* bind the evacuator to the gc cycle */
	_evacuatorTask[workerIndex]->bindWorkerThread(env, _tenureMask, _heapLayout, _copiedBytesReportingDelta);
	setEvacuatorBit(workerIndex, _boundEvacuatorBitmap);

#if defined(EVACUATOR_DEBUG)
	Debug_MM_true(_evacuatorCount > workerIndex);
	Debug_MM_true(_evacuatorCount == env->_currentTask->getThreadCount());
	Debug_MM_true(testEvacuatorBit(workerIndex, _evacuatorMask));
	Debug_MM_true(isEvacuatorBitmapFull(_evacuatorMask));
	if (MM_EvacuatorBase::isTraceOptionSelected(_extensions, EVACUATOR_DEBUG_CYCLE)) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
		omrtty_printf("%5lu %2lu %2lu: %cbind[%2lu]; ", getEpoch()->gc, getEpoch()->epoch, workerIndex, env->isMasterThread() ? '*' : ' ', _evacuatorCount);
		printEvacuatorBitmap(env, "bound", _boundEvacuatorBitmap);
		printEvacuatorBitmap(env, "; stalled", _stalledEvacuatorBitmap);
		printEvacuatorBitmap(env, "; resuming", _resumingEvacuatorBitmap);
		printEvacuatorBitmap(env, "; mask", _evacuatorMask);
		omrtty_printf("; flags%lx; threads:%lu; reporting:%lx\n", _evacuatorFlags, env->_currentTask->getThreadCount(), _copiedBytesReportingDelta);
	}
#endif /* defined(EVACUATOR_DEBUG) */

	return _evacuatorTask[workerIndex];
}

void
MM_EvacuatorController::unbindWorker(MM_EnvironmentStandard *env)
{
	MM_Evacuator *evacuator = env->getEvacuator();

	/* passivate the evacuator instance */
	clearEvacuatorBit(evacuator->getWorkerIndex(), _boundEvacuatorBitmap);

	/* pull final remaining metrics from evacuator */
	VM_AtomicSupport::writeBarrier();
	VM_AtomicSupport::add(&_finalDiscardedBytes, evacuator->getDiscarded());
	VM_AtomicSupport::add(&_finalFlushedBytes, evacuator->getFlushed());
	VM_AtomicSupport::add(&_finalRecycledBytes, evacuator->getRecycled());
	VM_AtomicSupport::readBarrier();
	if (isEvacuatorBitmapEmpty(_boundEvacuatorBitmap)) {
		Debug_MM_true(isEvacuatorBitmapEmpty(_stalledEvacuatorBitmap) || isAborting());
		_finalEvacuatedBytes = _copiedBytes[MM_Evacuator::survivor] + _copiedBytes[MM_Evacuator::tenure];
		Debug_MM_true((_finalEvacuatedBytes == _scannedBytes) || isAborting());
	}

#if defined(EVACUATOR_DEBUG)
	if (MM_EvacuatorBase::isTraceOptionSelected(_extensions, EVACUATOR_DEBUG_CYCLE)) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
		omrtty_printf("%5lu %2lu %2lu:    unbind; ", getEpoch()->gc, getEpoch()->epoch, evacuator->getWorkerIndex());
		printEvacuatorBitmap(env, "bound", _boundEvacuatorBitmap);
		printEvacuatorBitmap(env, "; stalled", _stalledEvacuatorBitmap);
		printEvacuatorBitmap(env, "; resuming", _resumingEvacuatorBitmap);
		omrtty_printf("; flags:%lx\n", _evacuatorFlags);
	}
	if ((MM_EvacuatorBase::isTraceOptionSelected(_extensions, EVACUATOR_DEBUG_HEAPCHECK)) && isEvacuatorBitmapEmpty(_boundEvacuatorBitmap)) {
		evacuator->checkSurvivor();
		evacuator->checkTenure();
	}
#endif /* defined(EVACUATOR_DEBUG) */

	evacuator->unbindWorkerThread(env);
}

void MM_EvacuatorController::assertGenerationalInvariant(MM_EnvironmentStandard *env) {
#if defined(EVACUATOR_DEBUG)
	VM_AtomicSupport::writeBarrier();
	OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
	Debug_MM_true(hasCompletedScan() || isAborting());
	Debug_MM_true(hasCompletedScan() != isAborting());
	if (MM_Evacuator::isTraceOptionSelected(_extensions, (EVACUATOR_DEBUG_CYCLE | EVACUATOR_DEBUG_WORK))) {
		omrtty_printf("%5lu %2lu %2lu:  end scan; ", _history.getEpoch()->epoch, env->getEvacuator()->getWorkerIndex());
		printEvacuatorBitmap(env, "stalled", _stalledEvacuatorBitmap);
		printEvacuatorBitmap(env, "; resuming", _resumingEvacuatorBitmap);
		omrtty_printf("; flags:%lx; copied:%lx; scanned:%lx\n",  _evacuatorFlags,
				(_copiedBytes[MM_Evacuator::survivor] + _copiedBytes[MM_Evacuator::tenure]),
				_scannedBytes);
	}
#endif /* defined(EVACUATOR_DEBUG) */

	/* assert that the aggregate volume copied equals volume scanned */
	VM_AtomicSupport::writeBarrier();
	uintptr_t totalCopied = _copiedBytes[MM_Evacuator::survivor] + _copiedBytes[MM_Evacuator::tenure];
	Assert_GC_true_with_message4(env, (totalCopied == _scannedBytes) || isAborting(),
			"copied bytes (survivor+tenure) (%lx+%lx)=%lx != %lx scanned bytes\n",
			_copiedBytes[MM_Evacuator::survivor], _copiedBytes[MM_Evacuator::tenure], totalCopied, _scannedBytes);
}

void
MM_EvacuatorController::notifyOfWork()
{
	/* only one notification is required if >1 fat evacutors have distributable work when a stall condition is raised */
	if (1 == VM_AtomicSupport::lockCompareExchange(&_isNotifyOfWorkPending, 1, 0)) {
		VM_AtomicSupport::readBarrier();

		acquireController();
		if (0 < _stalledEvacuatorCount) {
			omrthread_monitor_notify(_controllerMutex);
		}
		releaseController();
	}
}

bool
MM_EvacuatorController::isWaitingForWork(MM_Evacuator *worker)
{
	Debug_MM_true(!isAborting());

	uintptr_t workerIndex = worker->getWorkerIndex();

	/* the worker may have pulled some work from another evacuator worklist */
	if (worker->hasScanWork()) {
		Debug_MM_true(!testEvacuatorBit(workerIndex, _resumingEvacuatorBitmap));

		/* worker has work and can resume if it was stalled stall */
		if (testEvacuatorBit(workerIndex, _stalledEvacuatorBitmap)) {
			clearEvacuatorBit(workerIndex, _stalledEvacuatorBitmap);
			VM_AtomicSupport::subtract(&_stalledEvacuatorCount, 1);
		}

		/* prospectively notify other stalled evacuators that there may be more work to be found */
		if (0 < _stalledEvacuatorCount) {
			VM_AtomicSupport::lockCompareExchange(&_isNotifyOfWorkPending, 1, 0);
			omrthread_monitor_notify(_controllerMutex);
		}

		/* return to continue heap scan */
		VM_AtomicSupport::readBarrier();
		return false;
	}

	/* this worker is stalled or stalling -- set its stall until it finds work or heap scan completes */
	uintptr_t otherStalledEvacuators = setEvacuatorBit(workerIndex, _stalledEvacuatorBitmap);

	/* the evacuator that sets the last stall bit to fill the stalled bitmap will notify all to complete the heap scan */
	if (0 == (otherStalledEvacuators & getEvacuatorBitMask(workerIndex))) {
		Debug_MM_true(!testEvacuatorBit(workerIndex, _resumingEvacuatorBitmap));

		VM_AtomicSupport::add(&_stalledEvacuatorCount, 1);
		VM_AtomicSupport::readBarrier();

		/* if all evacuators are stalled and none are resuming (ie, with work) the scan cycle can complete or abort */
		if (isEvacuatorBitmapFull(_stalledEvacuatorBitmap) && isEvacuatorBitmapEmpty(_resumingEvacuatorBitmap)) {

			/* assert generational invariant: aggregate bytes copied == bytes scanned xor cycle aborted */
			assertGenerationalInvariant(worker->getEnvironment());

			/* set resuming bits for all evacuators and notify stalled evacuators to resume to complete heap scan */
			fillEvacuatorBitmap(_resumingEvacuatorBitmap);
			omrthread_monitor_notify_all(_controllerMutex);
		}
	}

	Debug_MM_true(testEvacuatorBit(workerIndex, _stalledEvacuatorBitmap));

	/* at this point the worker's resuming bit won't be set unless another evacuator notified of end of heap scan */
	if (!testEvacuatorBit(workerIndex, _resumingEvacuatorBitmap)) {

		/* set pending notification flag to request notification when an evacuator has distributable work */
		VM_AtomicSupport::lockCompareExchange(&_isNotifyOfWorkPending, 0, 1);
		VM_AtomicSupport::readBarrier();

		/* wait for another evacuator to notify of work or end of heap scan*/
		worker->getEnvironment()->_scavengerStats._acquireScanListCount += 1;
		omrthread_monitor_wait(_controllerMutex);
	}

	/* another evacuator may have set resuming bit and notified end of heap scan */
	if (testEvacuatorBit(workerIndex, _resumingEvacuatorBitmap)) {

		/* remove resuming worker from stall */
		clearEvacuatorBit(workerIndex, _resumingEvacuatorBitmap);
		clearEvacuatorBit(workerIndex, _stalledEvacuatorBitmap);
		VM_AtomicSupport::subtract(&_stalledEvacuatorCount, 1);
		VM_AtomicSupport::readBarrier();

		/* return to complete heap scan */
		return false;
	}

	/* return and try again to find work (another evacuator notified of work) */
	return true;
}

void
MM_EvacuatorController::reportProgress(MM_Evacuator *worker,  uintptr_t *copied, uintptr_t *scanned)
{
	/* any or all of these counters may be updated while this thread is sampling them, but epoch boundaries are metered by scanned volume */
	VM_AtomicSupport::writeBarrier();
	uintptr_t sampledScannedBytes = _scannedBytes;
	uintptr_t aggregateScannedBytes = *scanned + VM_AtomicSupport::lockCompareExchange(&_scannedBytes, sampledScannedBytes, sampledScannedBytes + *scanned);
	if (aggregateScannedBytes != (sampledScannedBytes + *scanned)) {
		VM_AtomicSupport::add(&_scannedBytes, *scanned);
	}
	VM_AtomicSupport::readBarrier();

	/* if above block succeeds these might be sampled in same timeslice, otherwise they may be skewed in the epochal record */
	uintptr_t sampledCopiedBytes[] = {
		VM_AtomicSupport::add(&_copiedBytes[MM_Evacuator::survivor], copied[MM_Evacuator::survivor]),
		VM_AtomicSupport::add(&_copiedBytes[MM_Evacuator::tenure], copied[MM_Evacuator::tenure])
	};

	/* some epochs may be skipped due to noise in non-atomic block above (stats will merge into next recorded epoch) */
	if (aggregateScannedBytes == (sampledScannedBytes + *scanned)) {
		/* ... but at most one evacuator will cross any given epoch boundary, and only once ... */
		reportProgress(worker, (aggregateScannedBytes - *scanned), aggregateScannedBytes, sampledCopiedBytes);
	}

	copied[MM_Evacuator::survivor] = 0;
	copied[MM_Evacuator::tenure] = 0;
	*scanned = 0;
}

void
MM_EvacuatorController::reportProgress(MM_Evacuator *worker, uintptr_t oldScannedValue, uintptr_t newScannedValue, uintptr_t *sampledCopiedBytes)
{
#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	/* these are default values in case _bytesPerReportingEpoch has not yet been set (see thread count knot in bindWorker()) */
	uintptr_t oldEpoch = 0, newEpoch = 1;
	if (0 < _bytesPerReportingEpoch) {
		oldEpoch = oldScannedValue / _bytesPerReportingEpoch;
		newEpoch = newScannedValue / _bytesPerReportingEpoch;
	}

	/* trigger end of epoch when scanned bytes counter crosses a reporting threshold or generational invariant is satisfied */
	bool lastEpoch = (0 < newScannedValue) && ((sampledCopiedBytes[0] + sampledCopiedBytes[1]) == newScannedValue);
	if ((oldEpoch < newEpoch) || lastEpoch) {

		/* get current timestamp to mark start of next epoch */
		OMRPORT_ACCESS_FROM_ENVIRONMENT(worker->getEnvironment());
		uint64_t currentTimestamp = omrtime_hires_clock();

		/* run around the bus sampling worklist volumes for all evacuators */
		uintptr_t clears = 0, stalls = 0;
		uintptr_t histogram[] = {0, 0, 0};
		uintptr_t maxVolume = 0, minVolume = UINTPTR_MAX, totalVolume = 0;
		uintptr_t volumeQuota = getWorkNotificationQuota(_minimumWorkspaceSize);
		for (uintptr_t index = 0; index < _evacuatorCount; index += 1) {
			MM_Evacuator *next = isBoundEvacuator(index) ? _evacuatorTask[index] : NULL;
			if (NULL != next) {
				uintptr_t volume = next->getVolumeOfWork();
				totalVolume += volume;
				if (volume > maxVolume) {
					maxVolume = volume;
				}
				if (volume < minVolume) {
					minVolume = volume;
				}
				if (0 == volume) {
					histogram[0] += 1;
				} else if (volume < volumeQuota) {
					histogram[1] += 1;
				} else {
					histogram[2] += 1;
				}
				clears += next->_stats->_acquireScanListCount;
				stalls += next->_stats->_workStallCount;
			}
		}

		omrthread_monitor_enter(_reporterMutex);

		/* get next epoch record and fill it in */
		MM_EvacuatorHistory::Epoch *epoch = _history.nextEpoch(newEpoch, lastEpoch);
		epoch->gc = worker->getEnvironment()->_scavengerStats._gcCount;
		epoch->epoch = newEpoch;
		epoch->duration = omrtime_hires_delta(_history.epochStartTime(currentTimestamp), currentTimestamp, OMRPORT_TIME_DELTA_IN_MICROSECONDS);
		epoch->survivorAllocationCeiling = _copyspaceAllocationCeiling[MM_Evacuator::survivor];
		epoch->tenureAllocationCeiling = _copyspaceAllocationCeiling[MM_Evacuator::tenure];
		epoch->cleared = clears;
		epoch->stalled = stalls;
		epoch->survivorCopied = sampledCopiedBytes[0];
		epoch->tenureCopied = sampledCopiedBytes[1];
		epoch->scanned = newScannedValue;
		epoch->sumVolumeOfWork = totalVolume;
		epoch->minVolumeOfWork = minVolume;
		epoch->maxVolumeOfWork = maxVolume;
		for (uintptr_t i = 0; i < 3; i += 1) {
			epoch->volumeHistogram[i] = histogram[i];
		}
		epoch->volumeQuota = volumeQuota;

		omrthread_monitor_exit(_reporterMutex);
	}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
}

uintptr_t
MM_EvacuatorController::calculateProjectedEvacuationBytes()
{
	return _heapLayout[MM_Evacuator::survivor][1] - _heapLayout[MM_Evacuator::survivor][0];
}

uintptr_t
MM_EvacuatorController::calculateOptimalWhitespaceSize(MM_Evacuator::Region region)
{
	/* monitor aggregate survivor copy volume and reduce tlh allocation size when running low */
	if (MM_Evacuator::survivor == region) {
		Debug_MM_true(calculateProjectedEvacuationBytes() >= _copiedBytes[MM_Evacuator::survivor]);
		Debug_MM_true((_maximumCopyspaceSize >> 3) >= _minimumCopyspaceSize);

		/* calculate approximate total survivor whitespace remaining given current aggregate reported survivor bytes copied */
		uintptr_t approximateTotalSurvivorRemaining = (calculateProjectedEvacuationBytes() - _copiedBytes[MM_Evacuator::survivor]);

		/* adjust down assuming each evacuator on average is halfway through reporting delta */
		uintptr_t approximateUnreportedSurvivorCopied = _evacuatorCount * (_copiedBytesReportingDelta >> 1);
		if (approximateTotalSurvivorRemaining >= approximateUnreportedSurvivorCopied) {
			approximateTotalSurvivorRemaining -= approximateUnreportedSurvivorCopied;
		}

		/* scale down allocation ceiling if survivor copy flows into the last 3 reporting epochs */
		uintptr_t allocationCeiling = _copyspaceAllocationCeiling[MM_Evacuator::survivor];

		/* if each epoch consumes 1/64 of survivor whitespace this kicks in when survivor semispace is >95% full */
		if (approximateTotalSurvivorRemaining < _bytesPerReportingEpoch) {
			allocationCeiling = _maximumCopyspaceSize >> 3;
		} else if (approximateTotalSurvivorRemaining < (2 * _bytesPerReportingEpoch)) {
			allocationCeiling = _maximumCopyspaceSize >> 2;
		} else if (approximateTotalSurvivorRemaining < (3 * _bytesPerReportingEpoch)) {
			allocationCeiling = _maximumCopyspaceSize >> 1;
		}

		/* set survivor copyspace allocation ceiling for all evacuators */
		volatile uintptr_t *survivorAllocationCeiling = &_copyspaceAllocationCeiling[MM_Evacuator::survivor];
		while (allocationCeiling < *survivorAllocationCeiling) {
			VM_AtomicSupport::lockCompareExchange(survivorAllocationCeiling, *survivorAllocationCeiling, allocationCeiling);
		}
		VM_AtomicSupport::readBarrier();
	}

	/* be as greedy as possible all the time in tenure */
	return alignToObjectSize(_copyspaceAllocationCeiling[region]);
}

uintptr_t
MM_EvacuatorController::calculateOptimalWorkspaceSize(uintptr_t evacuatorVolumeOfWork)
{
	/* scale down workspace size to minimum if any other evacuators are stalled */
	uintptr_t worksize = _minimumWorkspaceSize;

	/* allow worklist volume to double if no other evacuators are stalled */
	if (0 == _stalledEvacuatorCount) {
		worksize = alignToObjectSize(evacuatorVolumeOfWork);
		if (_minimumWorkspaceSize > worksize) {
			worksize = _minimumWorkspaceSize;
		} else if (_maximumWorkspaceSize < worksize)
			worksize = _maximumWorkspaceSize;
	}

	return worksize;
}

MM_EvacuatorWhitespace *
MM_EvacuatorController::getWhitespace(MM_Evacuator *evacuator, MM_Evacuator::Region region, uintptr_t length)
{
	MM_EvacuatorWhitespace *whitespace = NULL;
	MM_EnvironmentBase *env = evacuator->getEnvironment();

	/* try to allocate a tlh unless object won't fit in outside copyspace remainder and remainder is still too big to whitelist */
	uintptr_t optimalSize =  (0 < length) ? calculateOptimalWhitespaceSize(region) : _minimumCopyspaceSize;
	uintptr_t maximumLength = optimalSize;
	if (length <= maximumLength) {

		/* try to allocate tlh in region to contain at least length bytes */
		uintptr_t limitSize = OMR_MAX(_minimumCopyspaceSize, length);
		while ((NULL == whitespace) && (optimalSize >= limitSize)) {

			void *addrBase = NULL, *addrTop = NULL;
			MM_AllocateDescription allocateDescription(0, 0, false, true);
			allocateDescription.setCollectorAllocateExpandOnFailure(MM_Evacuator::tenure == region);
			void *allocation = (MM_EvacuatorWhitespace *)getMemorySubspace(region)->collectorAllocateTLH(env, this, &allocateDescription, optimalSize, addrBase, addrTop);
			if (NULL != allocation) {

				/* got a tlh of some size <= optimalSize */
				uintptr_t whitesize = (uintptr_t)addrTop - (uintptr_t)addrBase;
				whitespace = MM_EvacuatorWhitespace::whitespace(allocation, whitesize, env->compressObjectReferences(),  allocateDescription.isLOAAllocation());

				env->_scavengerStats.countCopyCacheSize(whitesize, _maximumCopyspaceSize);
				if (MM_Evacuator::survivor == region) {
					env->_scavengerStats._semiSpaceAllocationCountSmall += 1;
				} else {
					env->_scavengerStats._tenureSpaceAllocationCountSmall += 1;
				}

			} else {

				/* try again using a reduced allocation size */
				optimalSize >>= 1;
			}
		}

		Debug_MM_true4(evacuator->getEnvironment(), (NULL == whitespace) || (_extensions->tlhMinimumSize <= whitespace->length()),
				"%s tlh whitespace should not be less than tlhMinimumSize: requested=%lx; whitespace=%lx; limit=%lx\n",
				((MM_Evacuator::survivor == region) ? "survivor" : "tenure"), length, whitespace->length(), _copyspaceAllocationCeiling[region]);

		/* hand off any unused tlh allocation to evacuator to reuse later */
		if ((NULL != whitespace) && (whitespace->length() < length)) {
			evacuator->receiveWhitespace(whitespace);
			whitespace = NULL;
		}
	}

#if defined(EVACUATOR_DEBUG)
	if ((NULL != whitespace) && (MM_EvacuatorBase::isTraceOptionSelected(_extensions, EVACUATOR_DEBUG_ALLOCATE))) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
		omrtty_printf("%5lu %2lu %2lu:  allocate; %s; %lx %lx %lx %lx %lx %lx %lx\n",
				getEpoch()->gc, getEpoch()->epoch, evacuator->getWorkerIndex(), ((MM_Evacuator::survivor == region) ? "survivor" : "tenure"),
				(uintptr_t)whitespace, ((NULL != whitespace) ? whitespace->length() : 0), length, maximumLength, optimalSize,
				_copyspaceAllocationCeiling[region], _objectAllocationCeiling[region]);
	}
	if ((NULL != whitespace) && (MM_EvacuatorBase::isTraceOptionSelected(_extensions, EVACUATOR_DEBUG_POISON_DISCARD))) {
		MM_EvacuatorWhitespace::poison(whitespace);
	}
#endif /* defined(EVACUATOR_DEBUG) */

	return whitespace;
}

MM_EvacuatorWhitespace *
MM_EvacuatorController::getObjectWhitespace(MM_Evacuator *evacuator, MM_Evacuator::Region region, uintptr_t length)
{
	MM_EvacuatorWhitespace *whitespace = NULL;
	MM_EnvironmentBase *env = evacuator->getEnvironment();

	/* allocate minimal (this object's exact) size */
	MM_AllocateDescription allocateDescription(length, 0, false, true);
	allocateDescription.setCollectorAllocateExpandOnFailure(MM_Evacuator::tenure == region);
	void *allocation = getMemorySubspace(region)->collectorAllocate(env, this, &allocateDescription);
	if (NULL != allocation) {
		Debug_MM_true(isObjectAligned(allocation));

		whitespace = MM_EvacuatorWhitespace::whitespace(allocation, length, env->compressObjectReferences(), allocateDescription.isLOAAllocation());
		env->_scavengerStats.countCopyCacheSize(length, _maximumCopyspaceSize);
		if (MM_Evacuator::survivor == region) {
			env->_scavengerStats._semiSpaceAllocationCountLarge += 1;
		} else {
			env->_scavengerStats._tenureSpaceAllocationCountLarge += 1;
		}

	} else {

		/* lower the object allocation ceiling for the region */
		while (length < _objectAllocationCeiling[region]) {
			VM_AtomicSupport::lockCompareExchange(&_objectAllocationCeiling[region], _objectAllocationCeiling[region], length);
		}
		VM_AtomicSupport::readBarrier();
	}

	return whitespace;
}

/* calculate the number of active words in the evacuator bitmaps */
uintptr_t
MM_EvacuatorController::countEvacuatorBitmapWords(uintptr_t *tailWords)
{
	*tailWords = (0 != (_evacuatorCount & index_to_map_word_modulus)) ? 1 : 0;
	return (_evacuatorCount >> index_to_map_word_shift) + *tailWords;
}

/* test evacuator bitmap for all 0s (reliable only when caller holds controller mutex) */
bool
MM_EvacuatorController::isEvacuatorBitmapEmpty(volatile uintptr_t * const bitmap)
{
	uintptr_t tailWords = 0;
	uintptr_t bitmapWords = countEvacuatorBitmapWords(&tailWords);
	for (uintptr_t map = 0; map < bitmapWords; map += 1) {
		if (0 != bitmap[map]) {
			return false;
		}
	}
	return true;
}

/* test evacuator bitmap for all 1s (reliable only when caller holds controller mutex) */
bool
MM_EvacuatorController::isEvacuatorBitmapFull(volatile uintptr_t * const bitmap)
{
	uintptr_t tailWords = 0;
	uintptr_t bitmapWords = countEvacuatorBitmapWords(&tailWords);
	uintptr_t fullWords = bitmapWords - tailWords;
	for (uintptr_t map = 0; map < fullWords; map += 1) {
		if (~(uintptr_t)0 != bitmap[map]) {
			return false;
		}
	}
	if (0 < tailWords) {
		uintptr_t tailBits = ((uintptr_t)1 << (_evacuatorCount & index_to_map_word_modulus)) - 1;
		if (tailBits != bitmap[fullWords]) {
			return false;
		}
	}
	return true;
}

/* fill evacuator bitmap with all 1s (reliable only when caller holds controller mutex) */
void
MM_EvacuatorController::fillEvacuatorBitmap(volatile uintptr_t * bitmap)
{
	uintptr_t tailWords = 0;
	uintptr_t bitmapWords = countEvacuatorBitmapWords(&tailWords);
	uintptr_t fullWords = bitmapWords - tailWords;
	for (uintptr_t map = 0; map < fullWords; map += 1) {
		bitmap[map] = ~(uintptr_t)0;
	}
	if (0 < tailWords) {
		bitmap[fullWords] = ((uintptr_t)1 << (_evacuatorCount & index_to_map_word_modulus)) - 1;
	}
}

/* set evacuator bit in evacuator bitmap */
uintptr_t
MM_EvacuatorController::setEvacuatorBit(uintptr_t evacuatorIndex, volatile uintptr_t * const bitmap)
{
	uintptr_t evacuatorMask = 0;
	uintptr_t evacuatorMap = mapEvacuatorIndexToMapAndMask(evacuatorIndex, &evacuatorMask);

	uintptr_t previousBit =  VM_AtomicSupport::bitOr(&bitmap[evacuatorMap], evacuatorMask);
	VM_AtomicSupport::readBarrier();

	return previousBit;
}

/* clear evacuator bit in evacuator bitmap */
void
MM_EvacuatorController::clearEvacuatorBit(uintptr_t evacuatorIndex, volatile uintptr_t * const bitmap)
{
	uintptr_t evacuatorMask = 0;
	uintptr_t evacuatorMap = mapEvacuatorIndexToMapAndMask(evacuatorIndex, &evacuatorMask);
	VM_AtomicSupport::bitAnd(&bitmap[evacuatorMap], ~evacuatorMask);
	VM_AtomicSupport::readBarrier();
}

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
void
MM_EvacuatorController::reportCollectionStats(MM_EnvironmentBase *env)
{
#if defined(EVACUATOR_DEBUG)
	if ((MM_EnvironmentStandard *)env)->getEvacuator()->isDebugEnd()) {
#endif /* defined(EVACUATOR_DEBUG) */
		OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
		MM_ScavengerStats *stats = &_extensions->scavengerStats;

		omrtty_printf("%5lu      : contained;", stats->_gcCount);
		uint64_t contained = 0;
		for (uintptr_t insideCache = 0; insideCache < 7; insideCache += 1) {
			contained +=  stats->_copy_distance_counts[insideCache];
		}
		omrtty_printf(" %llu %llu %llu", contained, stats->_copy_distance_counts[7], stats->_copy_distance_counts[8]);
		for (uintptr_t outsideCache = 7; outsideCache < OMR_SCAVENGER_DISTANCE_BINS; outsideCache += 1) {
			contained +=  stats->_copy_distance_counts[outsideCache];
		}
		omrtty_printf(" %llu\n", contained);

		omrtty_printf("%5lu      : cachesize;", stats->_gcCount);
		for (uintptr_t cachesize = 0; cachesize < OMR_SCAVENGER_CACHESIZE_BINS; cachesize += 1) {
			omrtty_printf(" %llu", stats->_copy_cachesize_counts[cachesize]);
		}
		omrtty_printf(" %lx\n", stats->_copy_cachesize_sum);
		omrtty_printf("%5lu      :  worksize;", stats->_gcCount);
		for (uintptr_t worksize = 0; worksize < OMR_SCAVENGER_DISTANCE_BINS; worksize += 1) {
			omrtty_printf(" %llu", stats->_work_packetsize_counts[worksize]);
		}

		omrtty_printf(" %lx\n", stats->_work_packetsize_sum);
		omrtty_printf("%5lu      :     small;", stats->_gcCount);
		for (uintptr_t smallsize = 0; smallsize <= OMR_SCAVENGER_DISTANCE_BINS; smallsize += 1) {
			omrtty_printf(" %lu", stats->_small_object_counts[smallsize]);
		}
		omrtty_printf("\n");
		omrtty_printf("%5lu      :     large;", stats->_gcCount);
		for (uintptr_t largesize = 0; largesize <= OMR_SCAVENGER_DISTANCE_BINS; largesize += 1) {
			omrtty_printf(" %lu", stats->_large_object_counts[largesize]);
		}

		omrtty_printf("\n");
		if (_extensions->isEvacuatorEnabled()) {
			uintptr_t maxFrame = OMR_MAX(MM_Evacuator::unreachable, _extensions->evacuatorMaximumStackDepth);
			uintptr_t sumActivations = sumStackActivations(_stackActivations, maxFrame);
			omrtty_printf("%5lu      :     stack;", stats->_gcCount);
			for (uintptr_t depth = 0; depth < maxFrame; depth += 1) {
				omrtty_printf(" %lu", _stackActivations[depth]);
			}
			omrtty_printf(" %lu\n", sumActivations);
		}

		/* present time-related collection metrics in milliseconds */
		MM_CollectionStatisticsStandard *collectionStats = (MM_CollectionStatisticsStandard *)env->_cycleState->_collectionStatistics;
		uint64_t startUserTime = (uint64_t)collectionStats->_startProcessTimes._userTime;
		uint64_t startSystemTime = (uint64_t)collectionStats->_startProcessTimes._systemTime;
		uint64_t endUserTime = (uint64_t)collectionStats->_endProcessTimes._userTime;
		uint64_t endSystemTime = (uint64_t)collectionStats->_endProcessTimes._systemTime;
		double userMillis = (double)(endUserTime - startUserTime) / 1000000.0;
		double systemMillis = (double)(endSystemTime - startSystemTime) / 1000000.0;
		double scavengeMillis = (double)omrtime_hires_delta(collectionStats->_startTime, collectionStats->_endTime, OMRPORT_TIME_DELTA_IN_MICROSECONDS) / 1000.0;
		omrtty_printf("%5lu      : idle time; %lu %lu %lu %lu %7.3f %7.3f %7.3f %7.3f %7.3f %7.3f\n", stats->_gcCount,
			stats->_workStallCount, stats->_acquireScanListCount, stats->_syncStallCount, stats->_completeStallCount,
			(double)omrtime_hires_delta(0, stats->_workStallTime, OMRPORT_TIME_DELTA_IN_MICROSECONDS) / 1000.0,
			(double)omrtime_hires_delta(0, stats->_syncStallTime, OMRPORT_TIME_DELTA_IN_MICROSECONDS) / 1000.0,
			(double)omrtime_hires_delta(0, stats->_completeStallTime, OMRPORT_TIME_DELTA_IN_MICROSECONDS) / 1000.0,
			userMillis, systemMillis, scavengeMillis);

		if (!_extensions->isEvacuatorEnabled()) {

			uint64_t scavengeBytes = stats->_flipBytes + stats->_hashBytes + stats->_tenureAggregateBytes;
			uint64_t insideBytes = (scavengeBytes > stats->_work_packetsize_sum) ? (scavengeBytes - stats->_work_packetsize_sum) : 0;
			omrtty_printf("%5lu      :%10s; %lx 0 %lx %lx %lx %lx 0\n", stats->_gcCount, !isAborting() ? "end cycle" : "backout",
					scavengeBytes, insideBytes, stats->_tenureAggregateBytes, stats->_flipDiscardBytes, stats->_tenureDiscardBytes);

		} else {

			/* count bytes copied inside stack frames */
			uint64_t totalCopiedBytes = stats->_cycleVolumeMetrics[0] + stats->_cycleVolumeMetrics[1];
			double insideBytes = (0 < totalCopiedBytes) ? ((double)stats->_cycleVolumeMetrics[0] / (double)totalCopiedBytes) : 0;
			/* total volume of material evacuated to survivor or tenure spaces */
			uint64_t copiedBytes = _copiedBytes[MM_Evacuator::survivor] + _copiedBytes[MM_Evacuator::tenure];
			/* evacuator copied/scanned byte counts may include bytes added on 1st generational copy and not included in scavenger byte counts */
			omrtty_printf("%5lu %2lu   :%10s; %lx %lx %0.3f %lx %lx %lx %lx\n", getEpoch()->gc, getEpoch()->epoch, isAborting() ? "backout" : "end cycle",
					copiedBytes, _scannedBytes, insideBytes, _copiedBytes[MM_Evacuator::tenure], _finalDiscardedBytes, _finalFlushedBytes, _finalRecycledBytes);
			reportConditionCounts(getEpoch()->gc, getEpoch()->epoch);

			Debug_MM_true((_finalDiscardedBytes + _finalFlushedBytes) == (stats->_flipDiscardBytes + stats->_tenureDiscardBytes));
			if (MM_EvacuatorBase::isTraceOptionSelected(_extensions, EVACUATOR_DEBUG_EPOCH)) {
				for (uintptr_t epochIndex =  0; epochIndex <= _history.maxEpoch; epochIndex += 1) {
					MM_EvacuatorHistory::Epoch *epoch = _history.getEpoch(epochIndex);
					if (_history.isRecorded(epoch)) {
						uintptr_t totalCopied = epoch->survivorCopied + epoch->tenureCopied;
						MM_EvacuatorHistory::Epoch *previous = _history.priorEpoch(epoch);
						uintptr_t deltaScanned = epoch->scanned;
						uintptr_t deltaCopied = totalCopied;
						if (epoch > previous) {
							deltaCopied -= (previous->survivorCopied + previous->tenureCopied);
							deltaScanned -= previous->scanned;
						}
						uintptr_t unscanned = (totalCopied > epoch->scanned) ? (totalCopied - epoch->scanned) : 0;
						double copyScanRatio = (0 < epoch->scanned) ? ((double)totalCopied / (double)epoch->scanned) : 0.0;
						double deltaCopyScanRatio = (0 < deltaScanned) ? ((double)deltaCopied / (double)deltaScanned) : 0.0;
						omrtty_printf("%5lu %2lu  0:     epoch; %0.3f %0.3f %8lx %8lx %8lx %8lx %8lx %8lx %8.3f ", epoch->gc, epoch->epoch,
								copyScanRatio, deltaCopyScanRatio, epoch->survivorCopied, epoch->tenureCopied,	epoch->scanned, unscanned,
								epoch->survivorAllocationCeiling, epoch->tenureAllocationCeiling,
								((double)(epoch->duration) / 1000.0));
						omrtty_printf("%8lx %8lx %8lx %6lx %3lx %3lx %3lx %4lu %3lu\n", epoch->sumVolumeOfWork, epoch->minVolumeOfWork, epoch->maxVolumeOfWork,
								epoch->volumeQuota, epoch->volumeHistogram[0], epoch->volumeHistogram[1], epoch->volumeHistogram[2], epoch->cleared, epoch->stalled);
					}
				}
			}

			/* total copied/scanned byte counts should be equal unless we are aborting */
			Assert_GC_true_with_message4(env, (isAborting() || hasCompletedScan()), "survived+tenured (%lx+%lx)=%lx != %lx scanned\n",
					_copiedBytes[MM_Evacuator::survivor], _copiedBytes[MM_Evacuator::tenure], copiedBytes, _scannedBytes);
		}
#if defined(EVACUATOR_DEBUG)
	}
#endif /* defined(EVACUATOR_DEBUG) */
}

void
MM_EvacuatorController::reportConditionCounts(uintptr_t gc, uintptr_t epoch)
{
	uintptr_t conditionCount = MM_Math::floorLog2(MM_Evacuator::conditions_mask + 1);
	uintptr_t conditionCountTotals[MM_Evacuator::conditions_mask + 1];
	uintptr_t conditionCountSummary[conditionCount];

	memset(conditionCountSummary, 0, sizeof(conditionCountSummary));
	memset(conditionCountTotals, 0, sizeof(conditionCountTotals));

	uintptr_t objectCount = 0;
	for (uintptr_t index = 0; index < _evacuatorCount; index++) {
		const uintptr_t *conditionCounts = _evacuatorTask[index]->getConditionCounts();
		for (uintptr_t flags = 0; flags <= MM_Evacuator::conditions_mask; flags += 1) {
			for (uintptr_t condition = 1; condition < conditionCount; condition += 1) {
				if (0 != (flags & (1 << condition))) {
					conditionCountSummary[condition] += conditionCounts[flags];
				}
			}
			conditionCountTotals[flags] += conditionCounts[flags];
			objectCount += conditionCounts[flags];
		}
	}
	conditionCountSummary[0] = conditionCountTotals[0];

	OMRPORT_ACCESS_FROM_OMRVM(_omrVM);
	omrtty_printf("%5lu %2lu   :conditions;", gc, epoch);
	for (uintptr_t condition = 0; condition < conditionCount; condition += 1) {
		omrtty_printf(" %lu", conditionCountSummary[condition]);
	}
	omrtty_printf(" %lu\n", objectCount);
	double percent = (100.0 * (double)conditionCountTotals[0]) / (double)objectCount;
	omrtty_printf("%10lu :%10.3f| <none>\n", conditionCountTotals[0], percent);
	for (uintptr_t flags = 1; flags <= MM_Evacuator::conditions_mask; flags += 1) {
		if (0 != conditionCountTotals[flags]) {
			double percent = (100.0 * (double)conditionCountTotals[flags]) / (double)objectCount;
			omrtty_printf("%10lu :%10.3f", conditionCountTotals[flags], percent);
			for (uintptr_t condition = 1; condition < MM_Evacuator::conditions_mask; condition <<= 1) {
				if (0 != (flags & condition)) {
					omrtty_printf("| %s", MM_Evacuator::conditionName((MM_Evacuator::ConditionFlag)condition));
				}
			}
			omrtty_printf("\n");
		}
	}
}

void
MM_EvacuatorController::printEvacuatorBitmap(MM_EnvironmentBase *env, const char *label, volatile uintptr_t * const bitmap)
{
	OMRPORT_ACCESS_FROM_ENVIRONMENT(env);

	uintptr_t tailWords = 0;
	uintptr_t bitmapWords = countEvacuatorBitmapWords(&tailWords);
	omrtty_printf("%s:%lx", label, bitmap[0]);
	for (uintptr_t map = 1; map < bitmapWords; map += 1) {
		omrtty_printf(" %lx", bitmap[map]);
	}
}

uintptr_t
MM_EvacuatorController::sumStackActivations(uintptr_t *stackActivations, uintptr_t maxFrame)
{
	uintptr_t sum = 0;
	for (uintptr_t depth = 0; depth < maxFrame; depth += 1) {
		stackActivations[depth] = 0;
		for (uintptr_t evacuator = 0; evacuator < _evacuatorCount; evacuator += 1) {
			stackActivations[depth] += _evacuatorTask[evacuator]->getStackActivationCount(depth);
		}
		sum += stackActivations[depth];
	}
	return sum;
}

void
MM_EvacuatorController::waitToSynchronize(MM_Evacuator *worker, const char *id)
{
#if defined(EVACUATOR_DEBUG)
	Debug_MM_true(!testEvacuatorBit(worker->getWorkerIndex(), _stalledEvacuatorBitmap));
	Debug_MM_true(!testEvacuatorBit(worker->getWorkerIndex(), _resumingEvacuatorBitmap));
	if (MM_EvacuatorBase::isTraceOptionSelected(_extensions, EVACUATOR_DEBUG_CYCLE)) {
		MM_EnvironmentBase *env = worker->getEnvironment();
		OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
		omrtty_printf("%5lu %2lu %2lu:      sync; ", getEpoch()->gc, getEpoch()->epoch, worker->getWorkerIndex());
		printEvacuatorBitmap(env, "stalled", _stalledEvacuatorBitmap);
		printEvacuatorBitmap(env, "; resuming", _resumingEvacuatorBitmap);
		omrtty_printf("; flags:%lx; workunit:%lx; %s\n", _evacuatorFlags, worker->getEnvironment()->getWorkUnitIndex(), MM_EvacuatorBase::callsite(id));
	}
#endif /* defined(EVACUATOR_DEBUG) */
}

void
MM_EvacuatorController::continueAfterSynchronizing(MM_Evacuator *worker, uint64_t startTime, uint64_t endTime, const char *id)
{
#if defined(EVACUATOR_DEBUG)
	Debug_MM_true(!testEvacuatorBit(worker->getWorkerIndex(), _stalledEvacuatorBitmap));
	Debug_MM_true(!testEvacuatorBit(worker->getWorkerIndex(), _resumingEvacuatorBitmap));
	if (MM_EvacuatorBase::isTraceOptionSelected(_extensions, EVACUATOR_DEBUG_CYCLE)) {
		MM_EnvironmentBase *env = worker->getEnvironment();
		OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
		uint64_t waitMicros = omrtime_hires_delta(startTime, endTime, OMRPORT_TIME_DELTA_IN_MICROSECONDS);
		omrtty_printf("%5lu %2lu %2lu:  continue; ", getEpoch()->gc, getEpoch()->epoch, worker->getWorkerIndex());
		printEvacuatorBitmap(env, "stalled", _stalledEvacuatorBitmap);
		printEvacuatorBitmap(env, "; resuming", _resumingEvacuatorBitmap);
		omrtty_printf("; flags:%lx; micros:%%lx; %s\n", _evacuatorFlags, waitMicros, MM_EvacuatorBase::callsite(id));
	}
#endif /* defined(EVACUATOR_DEBUG) */
}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

