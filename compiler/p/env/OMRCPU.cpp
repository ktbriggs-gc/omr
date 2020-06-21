/*******************************************************************************
 * Copyright (c) 2000, 2020 IBM Corp. and others
 *
 * This program and the accompanying materials are made available under
 * the terms of the Eclipse Public License 2.0 which accompanies this
 * distribution and is available at http://eclipse.org/legal/epl-2.0
 * or the Apache License, Version 2.0 which accompanies this distribution
 * and is available at https://www.apache.org/licenses/LICENSE-2.0.
 *
 * This Source Code may also be made available under the following Secondary
 * Licenses when the conditions for such availability set forth in the
 * Eclipse Public License, v. 2.0 are satisfied: GNU General Public License,
 * version 2 with the GNU Classpath Exception [1] and GNU General Public
 * License, version 2 with the OpenJDK Assembly Exception [2].
 *
 * [1] https://www.gnu.org/software/classpath/license.html
 * [2] http://openjdk.java.net/legal/assembly-exception.html
 *
 * SPDX-License-Identifier: EPL-2.0 OR Apache-2.0 OR GPL-2.0 WITH Classpath-exception-2.0 OR LicenseRef-GPL-2.0 WITH Assembly-exception
 *******************************************************************************/

#include "env/CPU.hpp"
#include "env/CompilerEnv.hpp"
#include "env/Processors.hpp"
#include "infra/Assert.hpp"

char* feGetEnv(const char*);

bool
OMR::Power::CPU::getPPCSupportsAES()
   {
   return self()->supportsFeature(OMR_FEATURE_PPC_HAS_ALTIVEC) && self()->isAtLeast(OMR_PROCESSOR_PPC_P8) && self()->supportsFeature(OMR_FEATURE_PPC_HAS_VSX);
   }

bool
OMR::Power::CPU::hasPopulationCountInstruction()
   {
#if defined(J9OS_I5)
   return false;
#else
   return self()->isAtLeast(OMR_PROCESSOR_PPC_P7);
#endif
   }

bool
OMR::Power::CPU::supportsDecimalFloatingPoint()
   {
   return self()->supportsFeature(OMR_FEATURE_PPC_HAS_DFP);
   }

bool 
OMR::Power::CPU::getSupportsHardwareSQRT()
   {
   return self()->isAtLeast(OMR_PROCESSOR_PPC_HW_SQRT_FIRST);
   }

bool
OMR::Power::CPU::getSupportsHardwareRound()
   {
   return self()->isAtLeast(OMR_PROCESSOR_PPC_HW_ROUND_FIRST);
   }

bool
OMR::Power::CPU::getSupportsHardwareCopySign()
   {
   return self()->isAtLeast(OMR_PROCESSOR_PPC_HW_COPY_SIGN_FIRST);
   }

bool
OMR::Power::CPU::supportsTransactionalMemoryInstructions()
   {
   return self()->supportsFeature(OMR_FEATURE_PPC_HTM);
   }

bool
OMR::Power::CPU::isTargetWithinIFormBranchRange(intptr_t targetAddress, intptr_t sourceAddress)
   {
   intptr_t range = targetAddress - sourceAddress;
   return range <= self()->maxIFormBranchForwardOffset() &&
          range >= self()->maxIFormBranchBackwardOffset();
   }

bool
OMR::Power::CPU::supportsFeature(uint32_t feature)
   {
   if (TR::Compiler->omrPortLib == NULL)
      {
      return false;
      }

   OMRPORT_ACCESS_FROM_OMRPORT(TR::Compiler->omrPortLib);
   return (TRUE == omrsysinfo_processor_has_feature(&_processorDescription, feature));
   }

bool
OMR::Power::CPU::is(OMRProcessorArchitecture p)
   {
   if (TR::Compiler->omrPortLib == NULL)
      return self()->id() == self()->get_old_processor_type_from_new_processor_type(p);

   TR_ASSERT_FATAL((_processorDescription.processor == p) == (self()->id() == self()->get_old_processor_type_from_new_processor_type(p)), "is test %d failed, id() %d, _processorDescription.processor %d", p, self()->id(), _processorDescription.processor);
   return _processorDescription.processor == p;
   }

bool
OMR::Power::CPU::isAtLeast(OMRProcessorArchitecture p)
   {
   if (TR::Compiler->omrPortLib == NULL)
      return self()->id() >= self()->get_old_processor_type_from_new_processor_type(p);

   TR_ASSERT_FATAL((_processorDescription.processor >= p) == (self()->id() >= self()->get_old_processor_type_from_new_processor_type(p)), "is at least test %d failed, id() %d, _processorDescription.processor %d", p, self()->id(), _processorDescription.processor);
   return _processorDescription.processor >= p;
   }

bool
OMR::Power::CPU::isAtMost(OMRProcessorArchitecture p)
   {
   if (TR::Compiler->omrPortLib == NULL)
      return self()->id() <= self()->get_old_processor_type_from_new_processor_type(p);

   TR_ASSERT_FATAL((_processorDescription.processor <= p) == (self()->id() <= self()->get_old_processor_type_from_new_processor_type(p)), "is at most test %d failed, id() %d, _processorDescription.processor %d", p, self()->id(), _processorDescription.processor);
   return _processorDescription.processor <= p;
   }

TR_Processor
OMR::Power::CPU::get_old_processor_type_from_new_processor_type(OMRProcessorArchitecture p)
   {
   switch(p)
      {
      case OMR_PROCESSOR_PPC_FIRST:
         return TR_FirstPPCProcessor;
      case OMR_PROCESSOR_PPC_RIOS1:
         return TR_PPCrios1;
      case OMR_PROCESSOR_PPC_PWR403:
         return TR_PPCpwr403;
      case OMR_PROCESSOR_PPC_PWR405:
         return TR_PPCpwr405;
      case OMR_PROCESSOR_PPC_PWR440:
         return TR_PPCpwr440;
      case OMR_PROCESSOR_PPC_PWR601:
         return TR_PPCpwr601;
      case OMR_PROCESSOR_PPC_PWR602:
         return TR_PPCpwr602;
      case OMR_PROCESSOR_PPC_PWR603:
         return TR_PPCpwr603;
      case OMR_PROCESSOR_PPC_82XX:
         return TR_PPC82xx;
      case OMR_PROCESSOR_PPC_7XX:
         return TR_PPC7xx;
      case OMR_PROCESSOR_PPC_PWR604:
         return TR_PPCpwr604;
      case OMR_PROCESSOR_PPC_RIOS2:
         return TR_PPCrios2;
      case OMR_PROCESSOR_PPC_PWR2S:
         return TR_PPCpwr2s;
      case OMR_PROCESSOR_PPC_PWR620:
         return TR_PPCpwr620;
      case OMR_PROCESSOR_PPC_PWR630:
         return TR_PPCpwr630;
      case OMR_PROCESSOR_PPC_NSTAR:
         return TR_PPCnstar;
      case OMR_PROCESSOR_PPC_PULSAR:
         return TR_PPCpulsar;
      case OMR_PROCESSOR_PPC_GP:
         return TR_PPCgp;
      case OMR_PROCESSOR_PPC_GR:
         return TR_PPCgr;
      case OMR_PROCESSOR_PPC_GPUL:
         return TR_PPCgpul;
      case OMR_PROCESSOR_PPC_P6:
         return TR_PPCp6;
      case OMR_PROCESSOR_PPC_ATLAS:
         return TR_PPCatlas;
      case OMR_PROCESSOR_PPC_BALANCED:
         return TR_PPCbalanced;
      case OMR_PROCESSOR_PPC_CELLPX:
         return TR_PPCcellpx;
      case OMR_PROCESSOR_PPC_P7:
         return TR_PPCp7;
      case OMR_PROCESSOR_PPC_P8:
         return TR_PPCp8;
      case OMR_PROCESSOR_PPC_P9:
         return TR_PPCp9;
      case OMR_PROCESSOR_PPC_P10:
         return TR_PPCp10;
      default:
         TR_ASSERT_FATAL(false, "Unknown processor!");
      }
   return TR_FirstPPCProcessor;
   }

void
OMR::Power::CPU::applyUserOptions()
   {
   // P10 support is not yet well-tested, so it's currently gated behind an environment
   // variable to prevent it from being used by accident by users who use old versions of
   // OMR once P10 chips become available.
   if (_processorDescription.processor == OMR_PROCESSOR_PPC_P10)
      {
      static bool enableP10 = feGetEnv("TR_EnableExperimentalPower10Support");
      if (!enableP10)
         {
         _processorDescription.processor = OMR_PROCESSOR_PPC_P9;
         _processorDescription.physicalProcessor = OMR_PROCESSOR_PPC_P9;
         }
      }
   }
