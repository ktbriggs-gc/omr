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
#include "Math.hpp"
#include "MemorySubSpace.hpp"
#include "MemorySubSpaceSemiSpace.hpp"
#include "ScavengerCopyScanRatio.hpp"
#include "ScavengerStats.hpp"

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
#include "HeapRegionManager.hpp"
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

bool
MM_EvacuatorController::setEvacuatorFlag(uintptr_t flag, bool value)
{
	uintptr_t oldFlags;

	VM_AtomicSupport::readBarrier();
	if (value) {
		oldFlags = VM_AtomicSupport::bitOr(&_evacuatorFlags, flag);
	} else {
		oldFlags = VM_AtomicSupport::bitAnd(&_evacuatorFlags, ~flag);
	}

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

	/* evacuator model is not integrated with concurrent scavenger */
	Assert_MM_true(!_extensions->isEvacuatorEnabled() || !_extensions->isConcurrentScavengerEnabled());

	if (_extensions->isEvacuatorEnabled()) {
		/* normalize evacuator scan options selected in extensions post command-line parsing */
		_extensions->evacuatorScanOptions = MM_Evacuator::selectedScanOptions(_extensions);

		/* disable spinning for monitor-try-enter access to controller work distribution mutex */
		if (0 != omrthread_monitor_init_with_name(&_workMutex, 0, "MM_EvacuatorController::_workMutex")) {
			_workMutex = NULL;
			return false;
		}
		((J9ThreadAbstractMonitor *)_workMutex)->flags &= ~J9THREAD_MONITOR_TRY_ENTER_SPIN;

		for (uintptr_t workerIndex = 0; workerIndex < _maxGCThreads; workerIndex++) {
			_evacuatorTask[workerIndex] = NULL;
		}

		/* initialize heap region bounds, copied here at the start of each gc cycle for ease of access */
		for (MM_Evacuator::Region region = MM_Evacuator::survivor; region <= MM_Evacuator::evacuate; region = MM_Evacuator::nextEvacuationRegion(region)) {
			_heapLayout[region][0] = NULL;
			_heapLayout[region][1] = NULL;
			_memorySubspace[region] = NULL;
		}

		result = (NULL != _workMutex);
	}

	return result;
}

void
MM_EvacuatorController::tearDown(MM_EnvironmentBase *env)
{
	if (_extensions->isEvacuatorEnabled()) {
		MM_Forge *forge = env->getForge();
		/* free the evacuator instances in the evacuator control array */
		for (uintptr_t workerIndex = 0; workerIndex < _maxGCThreads; workerIndex++) {
			if (NULL != _evacuatorTask[workerIndex]) {
				Debug_MM_true(NULL == _evacuatorTask[workerIndex]->getEnvironment());
				_evacuatorTask[workerIndex]->kill();
				forge->free((void *)_evacuatorTask[workerIndex]);
				_evacuatorTask[workerIndex] = NULL;
			}
		}
		forge->free((void *)_evacuatorTask);
		/* free the system memory bound to these const (not nullable) pointers */
		forge->free((void *)_boundEvacuatorBitmap);
		forge->free((void *)_stalledEvacuatorBitmap);
		forge->free((void *)_resumingEvacuatorBitmap);
		forge->free((void *)_evacuatorMask);
		forge->free((void *)_evacuatorMetrics[0]);
		forge->free((void *)_evacuatorMetrics);

		if (NULL != _workMutex) {
			omrthread_monitor_destroy(_workMutex);
			_workMutex = NULL;
		}
	}
}

bool
MM_EvacuatorController::collectorStartup(MM_GCExtensionsBase* extensions)
{
#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	if (extensions->isEvacuatorEnabled()) {
		if (MM_EvacuatorBase::isTraceOptionSelected(_extensions, EVACUATOR_DEBUG_END)) {

			OMRPORT_ACCESS_FROM_OMRVM(_omrVM);
			_collectorStartTime = omrtime_hires_clock();
			omrtty_printf("startup; stack-depth:%llu; max-inside-size:%llu; max-inside-distance:%llu; min-copyspace::%llu; min-workspace:%llu; options:",
				_extensions->evacuatorMaximumStackDepth, _extensions->evacuatorMaximumInsideCopySize, _extensions->evacuatorMaximumInsideCopyDistance,
				_minimumCopyspaceSize, _minimumWorkspaceSize);
			for (uintptr_t condition = 0; condition < MM_Evacuator::condition_count; condition += 1) {
				if (MM_Evacuator::isScanOptionSelected(_extensions, (uintptr_t)1 << condition)) {
					omrtty_printf(" %s", MM_Evacuator::conditionName((MM_Evacuator::ConditionFlag)condition));
				}
			}
			omrtty_printf("\nheap;");
			MM_HeapRegionDescriptor *regionDescriptor = extensions->heapRegionManager->getFirstTableRegion();
			while (NULL != regionDescriptor) {
				omrtty_printf(" R{0x%llx, 0x%llx}", regionDescriptor->getLowAddress(), regionDescriptor->getHighAddress());
				regionDescriptor = extensions->heapRegionManager->getNextTableRegion(regionDescriptor);
			}
			regionDescriptor = extensions->heapRegionManager->getFirstAuxiliaryRegion();
			while (NULL != regionDescriptor) {
				omrtty_printf(" A{0x%llx, 0x%llx}", regionDescriptor->getLowAddress(), regionDescriptor->getHighAddress());
				regionDescriptor = extensions->heapRegionManager->getNextAuxiliaryRegion(regionDescriptor);
			}
			omrtty_printf("\n");
		}
	}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

	return true;
}

void
MM_EvacuatorController::collectorShutdown(MM_GCExtensionsBase* extensions)
{
	flushTenureWhitespace(true);

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
	if (MM_EvacuatorBase::isTraceOptionSelected(_extensions, EVACUATOR_DEBUG_END)) {

		OMRPORT_ACCESS_FROM_OMRVM(_omrVM);
		double collectorElapsedMillis = (double)omrtime_hires_delta(_collectorStartTime, omrtime_hires_clock(), OMRPORT_TIME_DELTA_IN_MICROSECONDS) / (double)1000;
		omrtty_printf("shutdown; elapsed-milliseconds:%0.3f\n", collectorElapsedMillis);
	}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
}

void
MM_EvacuatorController::collectorExpanded(MM_EnvironmentBase *env, MM_MemorySubSpace *subSpace, uintptr_t expandSize)
{
	MM_Collector::collectorExpanded(env, subSpace, expandSize);
	setHeapLayout();
}

void
MM_EvacuatorController::flushTenureWhitespace(bool shutdown)
{
	uintptr_t recycled = 0, discarded = 0;

	if (_extensions->isEvacuatorEnabled()) {
		for (uintptr_t workerIndex = 0; workerIndex < _maxGCThreads; workerIndex += 1) {
			if (NULL != _evacuatorTask[workerIndex]) {
				Debug_MM_true(NULL == _evacuatorTask[workerIndex]->getEnvironment());
				/* evacuator is passive (not bound to master/slave thread) here (starting global gc or collector shutting down) */
				uintptr_t * const volumeMetrics = &_evacuatorTask[workerIndex]->_metrics->_volumeMetrics[0];
				if (!shutdown) {
					/* flush tenure whitelist to memory subspace and count volumes recycled and discarded */
					_evacuatorTask[workerIndex]->flushWhitespace(MM_Evacuator::tenure);
					discarded += volumeMetrics[MM_Evacuator::tenure_discarded];
					recycled += volumeMetrics[MM_Evacuator::tenure_recycled];
				} else {
					/* abandon and count as discarded whitespace remaining in tenure whitelist */
					MM_EvacuatorWhitelist * const whiteList = &_evacuatorTask[workerIndex]->_whiteList[MM_Evacuator::tenure];
					while (0 < whiteList->top()) {
						discarded += whiteList->top(0)->length();
					}
				}
				volumeMetrics[MM_Evacuator::tenure_discarded] = 0;
				volumeMetrics[MM_Evacuator::tenure_recycled] = 0;
			}
		}
#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
		if (MM_EvacuatorBase::isTraceOptionSelected(_extensions, EVACUATOR_DEBUG_END)) {
			OMRPORT_ACCESS_FROM_OMRVM(_omrVM);
			omrtty_printf("%10s; tenure; discarded:%llu; recycled:%llu\n", (shutdown ? "finalize" : "global gc"), discarded, recycled);
		}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */
	}
}

void
MM_EvacuatorController::setHeapLayout()
{
	MM_AtomicOperations::readBarrier();
	_heapLayout[MM_Evacuator::survivor][0] = (uint8_t*) (_survivorSpaceBase);
	_heapLayout[MM_Evacuator::survivor][1] = (uint8_t*) (_survivorSpaceTop);
	_heapLayout[MM_Evacuator::tenure][0] = (uint8_t*) (_extensions->_tenureBase);
	_heapLayout[MM_Evacuator::tenure][1] = _heapLayout[MM_Evacuator::tenure][0] + _extensions->_tenureSize;
	_heapLayout[MM_Evacuator::evacuate][0] = (uint8_t*) (_evacuateSpaceBase);
	_heapLayout[MM_Evacuator::evacuate][1] = (uint8_t*) (_evacuateSpaceTop);
	MM_AtomicOperations::readWriteBarrier();
}

void
MM_EvacuatorController::masterSetupForGC(MM_EnvironmentStandard *env)
{
	_evacuatorFlags = 0;
	_isNotifyOfWorkPending = 0;

	/* clear collection metrics */
	memset(&_aggregateMetrics, 0, sizeof(_aggregateMetrics));

	/* reset the evacuator bit maps */
	uintptr_t tailMask = 0;
	uintptr_t bitmapWords = countEvacuatorBitmapWords(&tailMask, _maxGCThreads);
	for (uintptr_t map = 0; map < bitmapWords; map += 1) {
		Debug_MM_true(0 == _boundEvacuatorBitmap[map]);
		Debug_MM_true(0 == _stalledEvacuatorBitmap[map]);
		Debug_MM_true(0 == _resumingEvacuatorBitmap[map]);
		_boundEvacuatorBitmap[map]= 0;
		_stalledEvacuatorBitmap[map]= 0;
		_resumingEvacuatorBitmap[map]= 0;
		_evacuatorMask[map]= 0;
	}
	_stalledEvacuatorCount= 0;
	_evacuatorCount= 0;

	/* set up controller's subspace and layout arrays to match collector subspaces */
	_memorySubspace[MM_Evacuator::evacuate] = _evacuateMemorySubSpace;
	_memorySubspace[MM_Evacuator::survivor] = _survivorMemorySubSpace;
	_memorySubspace[MM_Evacuator::tenure] = _tenureMemorySubSpace;

	setHeapLayout();

	/* reset upper bounds for tlh allocation size, indexed by outside region -- these will be readjusted when thread count is stable */
	_copyspaceAllocationCeiling[MM_Evacuator::survivor] = _copyspaceAllocationCeiling[MM_Evacuator::tenure] = _maximumCopyspaceSize;
	_objectAllocationCeiling[MM_Evacuator::survivor] = _objectAllocationCeiling[MM_Evacuator::tenure] = ~(uintptr_t)0xff;

	/* clear private/public controller flags then set up the evacuator delegate class, which can initialize its public flags for the cycle */
	_evacuatorFlags = MM_EvacuatorDelegate::prepareForEvacuation(env);
}

void
MM_EvacuatorController::bindWorkers(MM_EnvironmentStandard *env)
{
	/* number of dispatched threads must be stable at this point */
	_evacuatorCount = _dispatcher->activeThreadCount();

	/* upper bound for tlh allocation size is indexed by outside region -- reduce these for small region spaces */
	uintptr_t optimalAllocationSize = (_heapLayout[MM_Evacuator::survivor][1] - _heapLayout[MM_Evacuator::survivor][0]) / (MM_Evacuator::copyspaces * _evacuatorCount);
	optimalAllocationSize = OMR_MAX(OMR_MIN(_maximumCopyspaceSize, _extensions->objectModel.adjustSizeInBytes(optimalAllocationSize)), _minimumCopyspaceSize);
	_copyspaceAllocationCeiling[MM_Evacuator::survivor] = _copyspaceAllocationCeiling[MM_Evacuator::tenure] = optimalAllocationSize;

	/* instantiate evacuator instances */
	for (uintptr_t evacuatorIndex = 0; evacuatorIndex < _evacuatorCount; evacuatorIndex += 1) {
		if (NULL == _evacuatorTask[evacuatorIndex]) {
			_evacuatorTask[evacuatorIndex] = MM_Evacuator::newInstance(evacuatorIndex, this, _extensions);
			Assert_MM_true(NULL != _evacuatorTask[evacuatorIndex]);
		}
		memset((void*)_evacuatorMetrics[evacuatorIndex], 0, sizeof(MM_Evacuator::Metrics));
	}

	/* evacuators bind to the gc cycle as they come online */
	fillEvacuatorBitmap(_evacuatorMask);
}

void
MM_EvacuatorController::bindWorker(MM_EnvironmentStandard *env)
{
	Debug_MM_true(_evacuatorTask[env->getSlaveID()]->getWorkerIndex() == env->getSlaveID());
	/* activate the evacuator instance and bind it to the gc cycle */
	MM_Evacuator *evacuator = _evacuatorTask[env->getSlaveID()];
	evacuator->bindWorkerThread(env, _tenureMask);
	evacuator->setHeapBounds(_heapLayout);
	uintptr_t mapBit = 0, mapWord = mapEvacuatorIndexToMapAndMask(evacuator->getWorkerIndex(), &mapBit);
	VM_AtomicSupport::readBarrier();
	VM_AtomicSupport::bitOr(&_boundEvacuatorBitmap[mapWord], mapBit);
	Debug_MM_true(env->_currentTask->getThreadCount() == _evacuatorCount);
}

void
MM_EvacuatorController::unbindWorker(MM_EnvironmentStandard *env)
{
	/* unbind and passivate the evacuator instance */
	MM_Evacuator *evacuator = env->getEvacuator();
	evacuator->unbindWorkerThread(env);
	uintptr_t mapBit = 0, mapWord = mapEvacuatorIndexToMapAndMask(evacuator->getWorkerIndex(), &mapBit);
	VM_AtomicSupport::readBarrier();
	VM_AtomicSupport::bitAnd(&_boundEvacuatorBitmap[mapWord], ~mapBit);

	Debug_MM_true(!isEvacuatorBitmapEmpty(_boundEvacuatorBitmap)
		|| (isEvacuatorBitmapEmpty(_stalledEvacuatorBitmap) && isEvacuatorBitmapEmpty(_resumingEvacuatorBitmap))
	);
}

uintptr_t
MM_EvacuatorController::getWorkReleaseThreshold() const
{
	uintptr_t workReleaseThreshold = _minimumWorkspaceSize;
	const uintptr_t stalledEvacuatorCount = (uintptr_t)_stalledEvacuatorCount;
	if (0 < stalledEvacuatorCount) {
		if (stalledEvacuatorCount <= (_evacuatorCount >> 1)) {
			workReleaseThreshold = OMR_MAX(MM_Evacuator::min_workspace_release, (_minimumWorkspaceSize >> 1));
		} else {
			workReleaseThreshold = MM_Evacuator::min_workspace_release;
		}
	}
	return workReleaseThreshold;
}

uintptr_t
MM_EvacuatorController::getWorkDistributionQuota() const
{
	return (_minimumCopyspaceSize << 1);
}

void
MM_EvacuatorController::notifyOfWork(MM_Evacuator *evacuator)
{
	/* only one notification is required if >1 fat evacutors have distributable work and >0 evacuators are starving for work */
	if (isNotifyOfWorkPending() && (0 < evacuator->getDistributableVolumeOfWork())) {
		uint64_t time = knock(evacuator->getEnvironment(), _workMutex);
		if (0 < time) {
			if (isNotifyOfWorkPending() && (0 < evacuator->getDistributableVolumeOfWork())) {
				_isNotifyOfWorkPending = 0;
				omrthread_monitor_notify(_workMutex);
			}
			leave(evacuator->getEnvironment(), _workMutex, time, MM_Evacuator::notify_count);
		}
	}
}

bool
MM_EvacuatorController::waitForWork(MM_Evacuator *worker)
{
	uintptr_t workerIndex = worker->getWorkerIndex();

	/* the worker may have pulled some work from another evacuator worklist */
	if (worker->hasScanWork()) {
		Debug_MM_true(!testEvacuatorBit(workerIndex, _resumingEvacuatorBitmap));

		/* worker has work and can resume if it was stalled stall */
		if (testEvacuatorBit(workerIndex, _stalledEvacuatorBitmap)) {
			clearEvacuatorBit(workerIndex, _stalledEvacuatorBitmap);
			_stalledEvacuatorCount -= 1;
		}

		/* prospectively notify other stalled evacuators that there may be more work to be found */
		_isNotifyOfWorkPending = 0;
		if (0 < _stalledEvacuatorCount) {
			worker->_metrics->_threadMetrics[MM_Evacuator::notify_count] += 1;
			omrthread_monitor_notify(_workMutex);
		}

		/* return to exit wait loop and continue heap scan */
		return false;
	}

	/* if this worker is stalling set and hold its stall state until it finds work or heap scan completes */
	Debug_MM_true(!testEvacuatorBit(workerIndex, _resumingEvacuatorBitmap));

	/* the evacuator that sets the last stall bit to fill the stalled bitmap will notify all to complete the heap scan */
	uintptr_t otherStalledEvacuators = setEvacuatorBit(workerIndex, _stalledEvacuatorBitmap);
	if (0 == (otherStalledEvacuators & getEvacuatorBitMask(workerIndex))) {
		_stalledEvacuatorCount += 1;

		/* if all evacuators are stalled and none are resuming (ie, with work) the scan cycle can complete or abort */
		Debug_MM_true((_stalledEvacuatorCount == _evacuatorCount) == (isEvacuatorBitmapFull(_stalledEvacuatorBitmap) && isEvacuatorBitmapEmpty(_resumingEvacuatorBitmap)));
		if (_stalledEvacuatorCount == _evacuatorCount) {
			assertGenerationalInvariant(worker->getEnvironment());
			/* set resuming bits for all evacuators -- now stalled + resuming */
			fillEvacuatorBitmap(_resumingEvacuatorBitmap);
			/* notify stalled evacuators to resume and complete heap scan */
			omrthread_monitor_notify_all(_workMutex);
		}
	}

	/* at this point the worker's resuming bit won't be set unless another evacuator notified of end of heap scan */
	if (!testEvacuatorBit(workerIndex, _resumingEvacuatorBitmap)) {
		Debug_MM_true(testEvacuatorBit(workerIndex, _stalledEvacuatorBitmap));
		Debug_MM_true(0 < _stalledEvacuatorCount);
		Debug_MM_true(!worker->hasScanWork());

		_isNotifyOfWorkPending = 1;
		pause(worker->getEnvironment(), _workMutex, MM_Evacuator::wait_count);
	}

	/* another evacuator may have set resuming bit and notified end of heap scan */
	if (testEvacuatorBit(workerIndex, _resumingEvacuatorBitmap)) {
		Debug_MM_true(testEvacuatorBit(workerIndex, _stalledEvacuatorBitmap));
		Debug_MM_true(0 < _stalledEvacuatorCount);
		Debug_MM_true(!worker->hasScanWork());

		/* remove resuming worker from stall but leave as resuming until all evacuators synchronize and clear resuming state */
		clearEvacuatorBit(workerIndex, _stalledEvacuatorBitmap);
		clearEvacuatorBit(workerIndex, _resumingEvacuatorBitmap);
		_stalledEvacuatorCount -= 1;

		/* return to exit wait loop and complete heap scan */
		return false;
	}

	/* return and try again to find work (another evacuator notified of work) */
	return true;
}

void
MM_EvacuatorController::aggregateEvacuatorMetrics(MM_EnvironmentStandard *env)
{
	/* aggregate evacuator metrics */
	for (uintptr_t taskIndex = 0; taskIndex < _dispatcher->threadCount(); taskIndex += 1) {
		MM_Evacuator::Metrics *metrics = _evacuatorMetrics[taskIndex];
		for (intptr_t metric = 0; metric < MM_Evacuator::volume_metrics; metric += 1) {
			_aggregateMetrics._volumeMetrics[metric] += metrics->_volumeMetrics[metric];
		}
		for (intptr_t counter = 0; counter <= MM_Evacuator::array_counters; counter += 1) {
			_aggregateMetrics._arrayVolumeCounts[counter] += metrics->_arrayVolumeCounts[counter];
		}
		for (intptr_t metric = 0; metric < MM_Evacuator::condition_states; metric += 1) {
			_aggregateMetrics._conditionMetrics[metric] += metrics->_conditionMetrics[metric];
		}
		for (intptr_t metric = 0; metric < MM_Evacuator::thread_metrics; metric += 1) {
			_aggregateMetrics._threadMetrics[metric] += metrics->_threadMetrics[metric];
		}
	}

	/* aggregate volume allocated in survivor space equals aggregate bytes copied or recycled or discarded from survivor space */
	uintptr_t liveVolume = _aggregateMetrics._volumeMetrics[MM_Evacuator::survivor_copy];
	uintptr_t disposedVolume = _aggregateMetrics._volumeMetrics[MM_Evacuator::survivor_recycled] + _aggregateMetrics._volumeMetrics[MM_Evacuator::survivor_discarded];
	uintptr_t allocatedVolume = _aggregateMetrics._volumeMetrics[MM_Evacuator::survivor_alloc];
	Assert_GC_true_with_message4(env, (allocatedVolume == (liveVolume + disposedVolume)),
			"***[evacuator-controller] allocated survivor bytes %lld != %lld (%lld+%lld) (copied+discarded) bytes\n",
			allocatedVolume, (liveVolume + disposedVolume), liveVolume, disposedVolume);
}

void
MM_EvacuatorController::assertGenerationalInvariant(MM_EnvironmentStandard *env) {
	if (!isAborting()) {
		/* aggregate evacuator volume metrics */
		uintptr_t volume[] = {0, 0, 0, 0};
		for (uintptr_t taskIndex = 0; taskIndex < _dispatcher->threadCount(); taskIndex += 1) {
			MM_Evacuator::Metrics *metrics = _evacuatorMetrics[taskIndex];
			for (intptr_t metric = 0; metric <= MM_Evacuator::leaf; metric += 1) {
				volume[metric] += metrics->_volumeMetrics[metric];
			}
		}
		/* the aggregate volume copied equals aggregate volume scanned unless aborting cycle */
		uintptr_t copied = volume[MM_Evacuator::survivor_copy] + volume[MM_Evacuator::tenure_copy];
		uintptr_t scanned = volume[MM_Evacuator::scanned] + volume[MM_Evacuator::leaf];
		Assert_GC_true_with_message4(env, (copied == scanned),
				"***[evacuator-controller] copied bytes (survivor+tenure) (%lld+%lld) != (%lld+%lld) (scanned+leaf) bytes\n",
				volume[MM_Evacuator::survivor_copy], volume[MM_Evacuator::tenure_copy],
				volume[MM_Evacuator::scanned], volume[MM_Evacuator::leaf]);

	}
}

uintptr_t
MM_EvacuatorController::calculateOptimalWhitespaceSize(MM_Evacuator::Region region)
{
	/* scale down tlh allocation size for survivor as available whitespace reserve depletes beyond critical threshold */
	if ((MM_Evacuator::survivor == region) && (0 < _copyspaceAllocationCeiling[region])) {
		uintptr_t allocatedVolume = 0;
		for (uintptr_t taskIndex = 0; taskIndex < _dispatcher->threadCount(); taskIndex += 1) {
			allocatedVolume += _evacuatorMetrics[taskIndex]->_volumeMetrics[MM_Evacuator::survivor_alloc];
		}
		intptr_t survivorWhitespace = (_heapLayout[region][1] - _heapLayout[region][0]) - allocatedVolume;
		intptr_t reserveWhitespace = MM_Evacuator::copyspaces * _maximumCopyspaceSize * _evacuatorCount;
		uintptr_t optimalAllocationSize = _maximumCopyspaceSize;
		if (survivorWhitespace <= reserveWhitespace) {
			optimalAllocationSize = alignToObjectSize((uintptr_t)(survivorWhitespace / (MM_Evacuator::copyspaces * _evacuatorCount)));
			optimalAllocationSize = OMR_MAX(OMR_MIN(_maximumCopyspaceSize, optimalAllocationSize), _minimumCopyspaceSize);
		}
		/* lower the object allocation ceiling for the region if forced to accept suboptimal size*/
		if (optimalAllocationSize < _copyspaceAllocationCeiling[region]) {
			VM_AtomicSupport::readBarrier();
			while (optimalAllocationSize < _copyspaceAllocationCeiling[region]) {
				VM_AtomicSupport::lockCompareExchange(&_copyspaceAllocationCeiling[region], _copyspaceAllocationCeiling[region], optimalAllocationSize);
			}
		}
	}
	/* be as greedy as possible all the time in tenure */
	return alignToObjectSize(_copyspaceAllocationCeiling[region]);
}

MM_Evacuator::Whitespace *
MM_EvacuatorController::getWhitespace(MM_Evacuator *evacuator, MM_Evacuator::Region region, uintptr_t length)
{
	MM_Evacuator::Whitespace *whitespace = NULL;
	MM_EnvironmentBase *env = evacuator->getEnvironment();

	/* try to allocate a tlh unless object won't fit in outside copyspace remainder and remainder is still too big to whitelist */
	length = alignToObjectSize(length);
	const uintptr_t optimalSize =  calculateOptimalWhitespaceSize(region);
	uintptr_t actualSize = optimalSize;
	if (length <= optimalSize) {
		/* try to allocate tlh in region to contain at least length bytes */
		uintptr_t limitSize = OMR_MAX(_minimumCopyspaceSize, length);
		while ((NULL == whitespace) && (actualSize >= limitSize)) {
			void *addrBase = NULL, *addrTop = NULL;
			MM_AllocateDescription allocateDescription(0, 0, false, true);
			allocateDescription.setCollectorAllocateExpandOnFailure(MM_Evacuator::tenure == region);
			void *allocation = (MM_Evacuator::Whitespace *)getMemorySubspace(region)->collectorAllocateTLH(env, this, &allocateDescription, actualSize, addrBase, addrTop);
			if (NULL != allocation) {
#if defined(EVACUATOR_DEBUG)
				Debug_MM_true(isObjectAligned(allocation));
				if (evacuator->isDebugPoisonDiscard()) {
					memset((uint8_t*)allocation, J9_GC_SINGLE_SLOT_HOLE, ((uintptr_t)addrTop - (uintptr_t)addrBase));
				}
#endif /* defined(EVACUATOR_DEBUG) */

				/* got a tlh of some size <= optimalSize */
				uintptr_t whitesize = (uintptr_t)addrTop - (uintptr_t)addrBase;
				env->_scavengerStats.countCopyCacheSize(whitesize, _maximumCopyspaceSize);
				if (MM_Evacuator::survivor == region) {
					evacuator->getMetrics()->_volumeMetrics[MM_Evacuator::survivor_alloc] += whitesize;
					env->_scavengerStats._semiSpaceAllocationCountSmall += 1;
				} else {
					evacuator->getMetrics()->_volumeMetrics[MM_Evacuator::tenure_alloc] += whitesize;
					env->_scavengerStats._tenureSpaceAllocationCountSmall += 1;
				}
				if (_extensions->_tenureSize != (uintptr_t)(evacuator->_heapBounds[MM_Evacuator::tenure][1] - evacuator->_heapBounds[MM_Evacuator::tenure][0])) {
					evacuator->setHeapBounds(_heapLayout);
				}

				/* wrap allocation in a Whitespace for evacuator use to end allocation loop */
				uintptr_t flags = MM_Evacuator::Whitespace::hole;
				if (allocateDescription.isLOAAllocation()) {
					flags |= MM_Evacuator::Whitespace::loa;
				}
				if (evacuator->compressObjectReferences()) {
					flags |= MM_Evacuator::Whitespace::compress;
				}
				whitespace = MM_Evacuator::Whitespace::whitespace(allocation, whitesize, flags);
#if defined(EVACUATOR_DEBUG)
				if (evacuator->isDebugPoisonDiscard()) {
					intptr_t offset = OMR_MIN(whitespace->length(), sizeof(MM_Evacuator::Whitespace));
					intptr_t fill = whitespace->length() - offset;
					if (0 < fill) {
						memset(whitespace->getBase() + offset, (uint8_t)MM_Evacuator::Whitespace::hole, fill);
					}
				}
#endif /* defined(EVACUATOR_DEBUG) */
			} else {
				/* try again using a reduced allocation size */
				actualSize = (actualSize > _minimumCopyspaceSize) ? (actualSize >> 1) : 0;
			}
		}

		/* lower the object allocation ceiling for the region if forced to accept suboptimal size*/
		if (actualSize < _copyspaceAllocationCeiling[region]) {
			VM_AtomicSupport::readBarrier();
			while (actualSize < _copyspaceAllocationCeiling[region]) {
				VM_AtomicSupport::lockCompareExchange(&_copyspaceAllocationCeiling[region], _copyspaceAllocationCeiling[region], actualSize);
			}
		}

		Debug_MM_true4(evacuator->getEnvironment(), (NULL == whitespace) || (_extensions->tlhMinimumSize <= whitespace->length()),
				"%s tlh whitespace should not be less than tlhMinimumSize: requested=%llx; whitespace=%llx; limit=%llx\n",
				((MM_Evacuator::survivor == region) ? "survivor" : "tenure"), length, whitespace->length(), _copyspaceAllocationCeiling[region]);

		/* hand off any unused tlh allocation to evacuator to reuse later */
		if ((NULL != whitespace) && (whitespace->length() < length)) {
			evacuator->receiveWhitespace(region, whitespace);
			whitespace = NULL;
		}
	}

#if defined(EVACUATOR_DEBUG)
	if ((NULL != whitespace) && evacuator->isDebugAllocate()) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
		omrtty_printf("allocate[%2lu]; %s; %llx %llx %llx %llx %llx %llx %llx\n",
				evacuator->getWorkerIndex(), ((MM_Evacuator::survivor == region) ? "survivor" : "tenure"),
				(uintptr_t)whitespace, ((NULL != whitespace) ? whitespace->length() : 0), length, optimalSize, actualSize,
				_copyspaceAllocationCeiling[region], _objectAllocationCeiling[region]);
	}
#endif /* defined(EVACUATOR_DEBUG) */

	return whitespace;
}

MM_Evacuator::Whitespace *
MM_EvacuatorController::getObjectWhitespace(MM_Evacuator *evacuator, MM_Evacuator::Region region, uintptr_t length)
{
	MM_Evacuator::Whitespace *whitespace = NULL;
	MM_EnvironmentBase *env = evacuator->getEnvironment();

	length = alignToObjectSize(length);
	if (length <= _objectAllocationCeiling[region]) {
		/* allocate minimal (this object's exact) size */
		MM_AllocateDescription allocateDescription(length, 0, false, true);
		allocateDescription.setCollectorAllocateExpandOnFailure(MM_Evacuator::tenure == region);
		void *allocation = getMemorySubspace(region)->collectorAllocate(env, this, &allocateDescription);
		if (NULL != allocation) {
			Debug_MM_true(length == allocateDescription.getContiguousBytes());
#if defined(EVACUATOR_DEBUG)
			Debug_MM_true(isObjectAligned(allocation));
			if (evacuator->isDebugPoisonDiscard()) {
				memset((uint8_t*)allocation, J9_GC_SINGLE_SLOT_HOLE, length);
			}
#endif /* defined(EVACUATOR_DEBUG) */

			/* allocated object whitespace */
			env->_scavengerStats.countCopyCacheSize(length, _maximumCopyspaceSize);
			if (MM_Evacuator::survivor == region) {
				evacuator->getMetrics()->_volumeMetrics[MM_Evacuator::survivor_alloc] += length;
				env->_scavengerStats._semiSpaceAllocationCountLarge += 1;
			} else {
				evacuator->getMetrics()->_volumeMetrics[MM_Evacuator::tenure_alloc] += length;
				env->_scavengerStats._tenureSpaceAllocationCountLarge += 1;
			}
			if (_extensions->_tenureSize != (uintptr_t)(evacuator->_heapBounds[MM_Evacuator::tenure][1] - evacuator->_heapBounds[MM_Evacuator::tenure][0])) {
				evacuator->setHeapBounds(_heapLayout);
			}

			/* wrap allocation in a Whitespace for evacuator use */
			uintptr_t flags = MM_Evacuator::Whitespace::hole;
			if (allocateDescription.isLOAAllocation()) {
				flags |= MM_Evacuator::Whitespace::loa;
			}
			if (evacuator->compressObjectReferences()) {
				flags |= MM_Evacuator::Whitespace::compress;
			}
			whitespace = MM_Evacuator::Whitespace::whitespace(allocation, length, flags);
#if defined(EVACUATOR_DEBUG)
			if (evacuator->isDebugPoisonDiscard()) {
				intptr_t offset = OMR_MIN(whitespace->length(), sizeof(MM_Evacuator::Whitespace));
				intptr_t fill = whitespace->length() - offset;
				if (0 < fill) {
					memset(whitespace->getBase() + offset, (uint8_t)MM_Evacuator::Whitespace::hole, fill);
				}
			}
#endif /* defined(EVACUATOR_DEBUG)  */
		} else if (length < _objectAllocationCeiling[region]) {
			VM_AtomicSupport::readBarrier();
			while (length < _objectAllocationCeiling[region]) {
				VM_AtomicSupport::lockCompareExchange(&_objectAllocationCeiling[region], _objectAllocationCeiling[region], length);
			}
		}
	}

#if defined(EVACUATOR_DEBUG)
	if ((NULL != whitespace) && evacuator->isDebugAllocate()) {
		OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
		omrtty_printf("allocate[%2lu]; %s; %llx %llx %llx %llx %llx\n",
				evacuator->getWorkerIndex(), ((MM_Evacuator::survivor == region) ? "survivor" : "tenure"),
				(uintptr_t)whitespace, ((NULL != whitespace) ? whitespace->length() : 0), length,
				_copyspaceAllocationCeiling[region], _objectAllocationCeiling[region]);
	}
#endif /* defined(EVACUATOR_DEBUG) */

	return whitespace;
}

bool
MM_EvacuatorController::isAllocatable(MM_Evacuator::Region region, uintptr_t sizeInBytes)
{
	return (sizeInBytes < OMR_MAX(_copyspaceAllocationCeiling[region], _objectAllocationCeiling[region]));
}

/* calculate the number of active words in the evacuator bitmaps */
uintptr_t
MM_EvacuatorController::countEvacuatorBitmapWords(uintptr_t *tailMask, uintptr_t evacuatorCount) const
{
	*tailMask = ((uintptr_t)1 << (evacuatorCount & index_to_map_word_modulus)) - 1;
	return (evacuatorCount >> index_to_map_word_shift) + ((0 == *tailMask) ? 0 : 1);
}

/* calculate the number of active words in the evacuator bitmaps */
uintptr_t
MM_EvacuatorController::countEvacuatorBitmapWords(uintptr_t *tailMask) const
{
	return countEvacuatorBitmapWords(tailMask, _evacuatorCount);
}

/* test evacuator bitmap for all 0s (reliable only when caller holds controller mutex) */
bool
MM_EvacuatorController::isEvacuatorBitmapEmpty(volatile uintptr_t * const bitmap) const
{
	uintptr_t tailMask = 0;
	uintptr_t bitmapWords = countEvacuatorBitmapWords(&tailMask);
	for (uintptr_t map = 0; map < bitmapWords; map += 1) {
		if (0 != bitmap[map]) {
			return false;
		}
	}
	return true;
}

/* test evacuator bitmap for all 1s (reliable only when caller holds controller mutex) */
bool
MM_EvacuatorController::isEvacuatorBitmapFull(volatile uintptr_t * const bitmap) const
{
	uintptr_t tailMask = 0;
	uintptr_t bitmapWords = countEvacuatorBitmapWords(&tailMask);
	uintptr_t fullWords = bitmapWords - ((0 != tailMask) ? 1 : 0);
	for (uintptr_t map = 0; map < fullWords; map += 1) {
		if (~(uintptr_t)0 != bitmap[map]) {
			return false;
		}
	}
	return (0 == tailMask) || (tailMask == bitmap[fullWords]);
}

/* fill evacuator bitmap with all 1s (reliable only when caller holds controller mutex) */
void
MM_EvacuatorController::fillEvacuatorBitmap(volatile uintptr_t * const bitmap, uintptr_t evacuatorCount)
{
	uintptr_t tailMask = 0;
	uintptr_t bitmapWords = countEvacuatorBitmapWords(&tailMask, evacuatorCount);
	uintptr_t fullWords = bitmapWords - ((0 != tailMask) ? 1 : 0);
	for (uintptr_t map = 0; map < fullWords; map += 1) {
		bitmap[map] = ~(uintptr_t)0;
	}
	if (0 < tailMask) {
		bitmap[fullWords] = tailMask;
	}
}

/* fill evacuator bitmap with all 1s (reliable only when caller holds controller mutex) */
void
MM_EvacuatorController::fillEvacuatorBitmap(volatile uintptr_t * const bitmap) {
	fillEvacuatorBitmap(bitmap, _evacuatorCount);
}

/* set evacuator bit in evacuator bitmap (reliable only when caller holds controller mutex) */
uintptr_t
MM_EvacuatorController::setEvacuatorBit(uintptr_t evacuatorIndex, volatile uintptr_t * const bitmap)
{
	uintptr_t evacuatorMask = 0, evacuatorMap = mapEvacuatorIndexToMapAndMask(evacuatorIndex, &evacuatorMask);
	uintptr_t evacuatorBits = bitmap[evacuatorMap];
	bitmap[evacuatorMap] |= evacuatorMask;
	return evacuatorBits;
}

/* clear evacuator bit in evacuator bitmap (reliable only when caller holds controller mutex) */
void
MM_EvacuatorController::clearEvacuatorBit(uintptr_t evacuatorIndex, volatile uintptr_t * const bitmap)
{
	uintptr_t evacuatorMask = 0, evacuatorMap = mapEvacuatorIndexToMapAndMask(evacuatorIndex, &evacuatorMask);
	bitmap[evacuatorMap] &= ~evacuatorMask;
}

#if defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS)
void
MM_EvacuatorController::printMetrics(MM_EnvironmentBase *env)
{
	/* NOTE: the cpu_ms metric is currently used only for reporting (here) and aggregate cpu_ms will always be 0 in production builds */
	OMRPORT_ACCESS_FROM_ENVIRONMENT(env);
	MM_ScavengerStats *stats = &_extensions->incrementScavengerStats;
	MM_CollectionStatisticsStandard *collectionStats = (MM_CollectionStatisticsStandard *)env->_cycleState->_collectionStatistics;
	uintptr_t volume[] = {0, 0, 0, 0, 0};
	if (_extensions->isEvacuatorEnabled()) {
		volume[0] = _aggregateMetrics._volumeMetrics[MM_Evacuator::survivor_copy];
		volume[1] = _aggregateMetrics._volumeMetrics[MM_Evacuator::tenure_copy];
		volume[2] = _aggregateMetrics._volumeMetrics[MM_Evacuator::scanned];
		volume[3] = _aggregateMetrics._volumeMetrics[MM_Evacuator::leaf];
		volume[4] = _aggregateMetrics._volumeMetrics[MM_Evacuator::survivor_discarded] + _aggregateMetrics._volumeMetrics[MM_Evacuator::tenure_discarded];
	} else {
		volume[0] = stats->_hashBytes + stats->_flipBytes;
		volume[1] = stats->_tenureAggregateBytes;
		volume[2] = volume[0] + volume[1];
		volume[3] = 0;
		volume[4] = stats->_flipDiscardBytes + stats->_tenureDiscardBytes;
	}
	uint64_t elapsedMicros = omrtime_hires_delta(collectionStats->_startTime, collectionStats->_endTime, OMRPORT_TIME_DELTA_IN_MICROSECONDS);
	uint64_t userNanos = collectionStats->_endProcessTimes._userTime - collectionStats->_startProcessTimes._userTime;
	uint64_t sysNanos = collectionStats->_endProcessTimes._systemTime - collectionStats->_startProcessTimes._systemTime;
	double cachePct = 100.0 * ((double)(stats->_copy_distance_counts[0]) / (double)(stats->_copy_distance_counts[0] + stats->_copy_distance_counts[1]));
	double elapsedMs = (double)omrtime_hires_delta(0, elapsedMicros, OMRPORT_TIME_DELTA_IN_MICROSECONDS) / (double)1000;
	double aggregateCpuPct = 100.0 * ((double)(userNanos + sysNanos) / (double)(1000 * elapsedMicros));
	double kbPerMs = (double)(volume[0] + volume[1]) / ((double)1024 * elapsedMs);
	omrtty_printf("%8lu:    gc end; survivor:%llu tenure:%llu scanned:%llu; pulled:%llu; leaf:%llu; frag:%llu; realms:%0.3f; kb/ms:%0.3f; cache%%:%0.3f; cpu%%:%0.3f; yielded:%llu; switched:%llu\n", _extensions->scavengerStats._gcCount,
			volume[0], volume[1], volume[2], _aggregateMetrics._threadMetrics[MM_Evacuator::pulled_volume], volume[3], volume[4], elapsedMs, kbPerMs, cachePct, aggregateCpuPct,
			collectionStats->_endProcessStats._switched - collectionStats->_startProcessStats._switched,
			collectionStats->_endProcessStats._yielded - collectionStats->_startProcessStats._yielded);
	omrtty_printf("%8lu:   objects:", _extensions->scavengerStats._gcCount);
	uintptr_t objectCount = 0;
	for (uintptr_t bin = 0; bin < OMR_SCAVENGER_OBJECT_BINS; bin += 1) {
		omrtty_printf(" %lld", stats->_object_volume_counts[bin]);
		objectCount += stats->_object_volume_counts[bin];
	}
	omrtty_printf(" %lld %lld\n", objectCount, stats->_object_volume_counts[OMR_SCAVENGER_OBJECT_BINS]);
	omrtty_printf("%8lu:    arrays:", _extensions->scavengerStats._gcCount);
	uintptr_t arrayCount = 0;
	for (uintptr_t bin = 0; bin < MM_Evacuator::array_counters; bin += 1) {
		omrtty_printf(" %lld", _aggregateMetrics._arrayVolumeCounts[bin]);
		arrayCount += _aggregateMetrics._arrayVolumeCounts[bin];
	}
	omrtty_printf(" %lld %lld\n", arrayCount, _aggregateMetrics._arrayVolumeCounts[MM_Evacuator::array_counters]);
	omrtty_printf("%8lu:copyspaces:", _extensions->scavengerStats._gcCount);
	for (uintptr_t bin = 0; bin < OMR_SCAVENGER_CACHESIZE_BINS; bin += 1) {
		omrtty_printf(" %lld", stats->_copy_cachesize_counts[bin]);
	}
	omrtty_printf(" %llu\n", stats->_copy_cachesize_sum);
	omrtty_printf("%8lu:workspaces:", _extensions->scavengerStats._gcCount);
	for (uintptr_t bin = 0; bin < OMR_SCAVENGER_WORKSIZE_BINS; bin += 1) {
		omrtty_printf(" %lld", stats->_work_packetsize_counts[bin]);
	}
	omrtty_printf(" %llu %llu\n", stats->_work_packetsize_sum, _aggregateMetrics._threadMetrics[MM_Evacuator::pulled_count]);
	double cpuMs = (double)_aggregateMetrics._threadMetrics[MM_Evacuator::cpu_ms] / (double)1000;
	double runMs = (double)omrtime_hires_delta(0, _aggregateMetrics._threadMetrics[MM_Evacuator::real_ms], OMRPORT_TIME_DELTA_IN_MICROSECONDS) / (double)1000;
	double stallMs = (double)omrtime_hires_delta(0, _aggregateMetrics._threadMetrics[MM_Evacuator::stall_ms], OMRPORT_TIME_DELTA_IN_MICROSECONDS) / (double)1000;
	double waitMs = (double)omrtime_hires_delta(0, _aggregateMetrics._threadMetrics[MM_Evacuator::wait_ms], OMRPORT_TIME_DELTA_IN_MICROSECONDS) / (double)1000;
	double notifyMs = (double)omrtime_hires_delta(0, _aggregateMetrics._threadMetrics[MM_Evacuator::notify_ms], OMRPORT_TIME_DELTA_IN_MICROSECONDS) / (double)1000;
	double syncMs = (double)omrtime_hires_delta(0, _aggregateMetrics._threadMetrics[MM_Evacuator::sync_ms], OMRPORT_TIME_DELTA_IN_MICROSECONDS) / (double)1000;
	double endMs = (double)omrtime_hires_delta(0, _aggregateMetrics._threadMetrics[MM_Evacuator::end_ms], OMRPORT_TIME_DELTA_IN_MICROSECONDS) / (double)1000;
	double runPct = (100.0 * runMs) / elapsedMs;
	double cpuPct = (100.0 * cpuMs) / elapsedMs;
	double stallPct = (100.0 * stallMs) / elapsedMs;
	double waitPct = (100.0 * waitMs) / elapsedMs;
	double endPct = (100.0 * (syncMs + endMs)) / elapsedMs;
	omrtty_printf("%8lu: work time; stall:%llu %0.3f; wait:%llu %0.3f; notify:%llu %0.3f; sync:%llu %0.3f; end:%llu %0.3f; run%%:%0.3f; cpu%%:%0.3f; stall%%:%0.3f; wait%%:%0.3f; end%%:%0.3f\n", _extensions->scavengerStats._gcCount,
		_aggregateMetrics._threadMetrics[MM_Evacuator::stall_count], stallMs,
		_aggregateMetrics._threadMetrics[MM_Evacuator::wait_count], waitMs,
		_aggregateMetrics._threadMetrics[MM_Evacuator::notify_count], notifyMs,
		_aggregateMetrics._threadMetrics[MM_Evacuator::sync_count], syncMs,
		_aggregateMetrics._threadMetrics[MM_Evacuator::end_count], endMs,
		runPct, cpuPct, stallPct, waitPct, endPct);
	const uintptr_t threads = _dispatcher->threadCount();
	for (uintptr_t thread = 0; thread < threads; thread += 1) {
		MM_Evacuator::Metrics *metrics = _evacuatorMetrics[thread];
		cpuMs = (double)metrics->_threadMetrics[MM_Evacuator::cpu_ms] / (double)1000;
		runMs = (double)omrtime_hires_delta(0, metrics->_threadMetrics[MM_Evacuator::real_ms], OMRPORT_TIME_DELTA_IN_MICROSECONDS) / (double)1000;
		stallMs = (double)omrtime_hires_delta(0, metrics->_threadMetrics[MM_Evacuator::stall_ms], OMRPORT_TIME_DELTA_IN_MICROSECONDS) / (double)1000;
		waitMs = (double)omrtime_hires_delta(0, metrics->_threadMetrics[MM_Evacuator::wait_ms], OMRPORT_TIME_DELTA_IN_MICROSECONDS) / (double)1000;
		notifyMs = (double)omrtime_hires_delta(0, metrics->_threadMetrics[MM_Evacuator::notify_ms], OMRPORT_TIME_DELTA_IN_MICROSECONDS) / (double)1000;
		syncMs = (double)omrtime_hires_delta(0, metrics->_threadMetrics[MM_Evacuator::sync_ms], OMRPORT_TIME_DELTA_IN_MICROSECONDS) / (double)1000;
		endMs = (double)omrtime_hires_delta(0, metrics->_threadMetrics[MM_Evacuator::end_ms], OMRPORT_TIME_DELTA_IN_MICROSECONDS) / (double)1000;
		runPct = (100.0 * runMs) / elapsedMs;
		cpuPct = (100.0 * cpuMs) / elapsedMs;
		stallPct = (100.0 * stallMs) / elapsedMs;
		waitPct = (100.0 * waitMs) / elapsedMs;
		endPct = (100.0 * (syncMs + endMs)) / elapsedMs;
		omrtty_printf("%8lu: thread %2lu; scan:%llu; clear:%llu; stall:%llu; wait:%llu; run%%:%0.3f; cpu%%:%0.3f; stall%%:%0.3f; wait%%:%0.3f; end%%:%0.3f\n", _extensions->scavengerStats._gcCount, thread,
				metrics->_threadMetrics[MM_Evacuator::scan_count], metrics->_threadMetrics[MM_Evacuator::clearing_count],
				metrics->_threadMetrics[MM_Evacuator::stall_count], metrics->_threadMetrics[MM_Evacuator::wait_count],
				runPct, cpuPct, stallPct, waitPct, endPct);
	}
	if (_extensions->isEvacuatorEnabled()) {
		const uintptr_t groups = threads * threads;
		const uintptr_t *stalls = &_aggregateMetrics._threadMetrics[MM_Evacuator::stalled];
		for (uintptr_t group = 0; group < groups; group += 1) {
			if (0 < stalls[group]) {
				omrtty_printf("%8lu:%10llu;", _extensions->scavengerStats._gcCount, stalls[group]);
				uintptr_t bits = group;
				for (uintptr_t thread = 0; thread < threads; thread += 1) {
					omrtty_printf(" %c", (1 == (1 & bits)) ? '1' : '0');
					bits >>= 1;
				}
				omrtty_printf("\n");
			}
		}
		uintptr_t counts[MM_Evacuator::condition_count];
		memset(counts, 0, sizeof(counts));
		omrtty_printf("%8lu:conditions;", _extensions->scavengerStats._gcCount);
		for (uintptr_t metric = 0; metric < MM_Evacuator::condition_states; metric += 1) {
			uintptr_t conditions = metric;
			for (uintptr_t condition = 0; (0 < conditions) && (condition < MM_Evacuator::condition_count); condition += 1) {
				if (1 == (1 & conditions)) {
					counts[condition] += _aggregateMetrics._conditionMetrics[metric];
				}
				conditions >>= 1;
			}
		}
		for (uintptr_t condition = 0; condition < MM_Evacuator::condition_count; condition += 1) {
			if (0 < counts[condition]) {
				omrtty_printf(" %s:%llu", MM_Evacuator::conditionName((MM_Evacuator::ConditionFlag)condition), counts[condition]);
			}
		}
		omrtty_printf(" objects:%llu\n", _aggregateMetrics._volumeMetrics[MM_Evacuator::objects]);
		for (uintptr_t metric = 0; metric < MM_Evacuator::condition_states; metric += 1) {
			if (0 < _aggregateMetrics._conditionMetrics[metric]) {
				omrtty_printf("%8lu:%10llu;", _extensions->scavengerStats._gcCount, _aggregateMetrics._conditionMetrics[metric]);
				uintptr_t conditions = metric;
				for (uintptr_t condition = 0; (0 < conditions) && (condition < MM_Evacuator::condition_count); condition += 1) {
					if (1 == (1 & conditions)) {
						omrtty_printf(" %s", MM_Evacuator::conditionName((MM_Evacuator::ConditionFlag)condition));
					}
					conditions >>= 1;
				}
				omrtty_printf("\n");
			}
		}
	}
}
#endif /* defined(EVACUATOR_DEBUG) || defined(EVACUATOR_DEBUG_ALWAYS) */

