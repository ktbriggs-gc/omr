/*******************************************************************************
 * Copyright (c) 2019, 2020 IBM Corp. and others
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

// On zOS XLC linker can't handle files with same name at link time.
// This workaround with pragma is needed. What this does is essentially
// give a different name to the codesection (csect) for this file. So it
// doesn't conflict with another file with the same name.
#pragma csect(CODE,"OMRZCPUBase#C")
#pragma csect(STATIC,"OMRZCPUBase#S")
#pragma csect(TEST,"OMRZCPUBase#T")

#include "control/Options.hpp"
#include "control/Options_inlines.hpp"
#include "env/CPU.hpp"

TR::CPU
OMR::Z::CPU::detect(OMRPortLibrary * const omrPortLib)
   {
   if (omrPortLib == NULL)
      return TR::CPU();

   OMRPORT_ACCESS_FROM_OMRPORT(omrPortLib);
   OMRProcessorDesc processorDescription;
   omrsysinfo_get_processor_description(&processorDescription);

   if (processorDescription.processor < OMR_PROCESSOR_S390_Z10)
      {
      omrsysinfo_processor_set_feature(&processorDescription, OMR_FEATURE_S390_DFP, FALSE);
      }

   if (processorDescription.processor < OMR_PROCESSOR_S390_Z196)
      {
      omrsysinfo_processor_set_feature(&processorDescription, OMR_FEATURE_S390_HIGH_WORD, FALSE);
      }

   if (processorDescription.processor < OMR_PROCESSOR_S390_ZEC12)
      {
      omrsysinfo_processor_set_feature(&processorDescription, OMR_FEATURE_S390_TE, FALSE);
      omrsysinfo_processor_set_feature(&processorDescription, OMR_FEATURE_S390_RI, FALSE);
      }

   if (processorDescription.processor < OMR_PROCESSOR_S390_Z13)
      {
      omrsysinfo_processor_set_feature(&processorDescription, OMR_FEATURE_S390_VECTOR_FACILITY, FALSE);
      }

   if (processorDescription.processor < OMR_PROCESSOR_S390_Z14)
      {
      omrsysinfo_processor_set_feature(&processorDescription, OMR_FEATURE_S390_VECTOR_PACKED_DECIMAL, FALSE);
      omrsysinfo_processor_set_feature(&processorDescription, OMR_FEATURE_S390_GUARDED_STORAGE, FALSE);
      }

   if (processorDescription.processor < OMR_PROCESSOR_S390_Z15)
      {
      omrsysinfo_processor_set_feature(&processorDescription, OMR_FEATURE_S390_MISCELLANEOUS_INSTRUCTION_EXTENSION_3, FALSE);
      omrsysinfo_processor_set_feature(&processorDescription, OMR_FEATURE_S390_VECTOR_FACILITY_ENHANCEMENT_2, FALSE);
      omrsysinfo_processor_set_feature(&processorDescription, OMR_FEATURE_S390_VECTOR_PACKED_DECIMAL_ENHANCEMENT_FACILITY, FALSE);
      }

   return TR::CPU(processorDescription);
   }

bool
OMR::Z::CPU::isAtLeast(OMRProcessorArchitecture p)
   {
#if defined(TR_HOST_S390) && (defined(J9ZOS390) || defined(LINUX))
   if (TR::Compiler->omrPortLib == NULL)
      return self()->is_at_least_old_api(p);
   TR_ASSERT_FATAL(self()->is_at_least_test(p), "processor %d failed, _supportedArch %d, _processorDescription.processor %d", p, _supportedArch, _processorDescription.processor);

   return _processorDescription.processor >= p;
#endif
   return false;
   }

bool
OMR::Z::CPU::supportsFeature(uint32_t feature)
   {
   if (TR::Compiler->omrPortLib == NULL)
      return self()->supports_feature_old_api(feature);
   TR_ASSERT_FATAL(self()->supports_feature_test(feature), "feature test %d failed, _supportedArch %d, _processorDescription.processor %d", feature, _supportedArch, _processorDescription.processor);

   OMRPORT_ACCESS_FROM_OMRPORT(TR::Compiler->omrPortLib);
   return TRUE == omrsysinfo_processor_has_feature(&_processorDescription, feature);
   }

bool
OMR::Z::CPU::is_at_least_test(OMRProcessorArchitecture p)
   {
   switch(p)
      {
      case OMR_PROCESSOR_S390_Z10:
         return (self()->getSupportsArch(TR::CPU::z10) == (_processorDescription.processor >= p));
      case OMR_PROCESSOR_S390_Z196:
         return (self()->getSupportsArch(TR::CPU::z196) == (_processorDescription.processor >= p));
      case OMR_PROCESSOR_S390_ZEC12:
         return (self()->getSupportsArch(TR::CPU::zEC12) == (_processorDescription.processor >= p));
      case OMR_PROCESSOR_S390_Z13:
         return (self()->getSupportsArch(TR::CPU::z13) == (_processorDescription.processor >= p));
      case OMR_PROCESSOR_S390_Z14:
         return (self()->getSupportsArch(TR::CPU::z14) == (_processorDescription.processor >= p));
      case OMR_PROCESSOR_S390_Z15:
         return (self()->getSupportsArch(TR::CPU::z15) == (_processorDescription.processor >= p));
      case OMR_PROCESSOR_S390_ZNEXT:
         return (self()->getSupportsArch(TR::CPU::zNext) == (_processorDescription.processor >= p));
      default:
         return false;
      }
   return false;
   }

bool
OMR::Z::CPU::supports_feature_test(uint32_t feature)
   {
   OMRPORT_ACCESS_FROM_OMRPORT(TR::Compiler->omrPortLib);
   bool ans = (TRUE == omrsysinfo_processor_has_feature(&_processorDescription, feature));

   switch(feature)
      {
      case OMR_FEATURE_S390_HIGH_WORD:
         return (self()->getSupportsHighWordFacility() == ans);
      case OMR_FEATURE_S390_DFP:
         return (self()->getSupportsDecimalFloatingPointFacility() == ans);
      case OMR_FEATURE_S390_FPE:
         return (self()->getSupportsFloatingPointExtensionFacility() == ans);
      case OMR_FEATURE_S390_TE:
         return (self()->getSupportsTransactionalMemoryFacility() == ans);
      case OMR_FEATURE_S390_RI:
         return (self()->getSupportsRuntimeInstrumentationFacility() == ans);
      case OMR_FEATURE_S390_VECTOR_FACILITY:
         return (self()->getSupportsVectorFacility() == ans);
      case OMR_FEATURE_S390_VECTOR_PACKED_DECIMAL:
         return (self()->getSupportsVectorPackedDecimalFacility() == ans);
      case OMR_FEATURE_S390_MISCELLANEOUS_INSTRUCTION_EXTENSION_3:
         return (self()->getSupportsMiscellaneousInstructionExtensions3Facility() == ans);
      case OMR_FEATURE_S390_VECTOR_FACILITY_ENHANCEMENT_2:
         return (self()->getSupportsVectorFacilityEnhancement2() == ans);
      case OMR_FEATURE_S390_VECTOR_PACKED_DECIMAL_ENHANCEMENT_FACILITY:
         return (self()->getSupportsVectorPackedDecimalEnhancementFacility() == ans);
      case OMR_FEATURE_S390_GUARDED_STORAGE:
         return (self()->getSupportsGuardedStorageFacility() == ans);
      case OMR_FEATURE_S390_MISCELLANEOUS_INSTRUCTION_EXTENSION_2:
         return (self()->getSupportsMiscellaneousInstructionExtensions2Facility() == ans);
      case OMR_FEATURE_S390_VECTOR_FACILITY_ENHANCEMENT_1:
         return (self()->getSupportsVectorFacilityEnhancement1() == ans);
      default:
         return false;
      }
   return false;
   }


bool
OMR::Z::CPU::is_at_least_old_api(OMRProcessorArchitecture p)
   {
   bool ans = false;
   switch(p)
      {
      case OMR_PROCESSOR_S390_Z10:
         ans = self()->getSupportsArch(TR::CPU::z10);
         break;
      case OMR_PROCESSOR_S390_Z196:
         ans = self()->getSupportsArch(TR::CPU::z196);
         break;
      case OMR_PROCESSOR_S390_ZEC12:
         ans = self()->getSupportsArch(TR::CPU::zEC12);
         break;
      case OMR_PROCESSOR_S390_Z13:
         ans = self()->getSupportsArch(TR::CPU::z13);
         break;
      case OMR_PROCESSOR_S390_Z14:
         ans = self()->getSupportsArch(TR::CPU::z14);
         break;
      case OMR_PROCESSOR_S390_Z15:
         ans = self()->getSupportsArch(TR::CPU::z15);
         break;
      case OMR_PROCESSOR_S390_ZNEXT:
         ans = self()->getSupportsArch(TR::CPU::zNext);
         break;
      default:
         TR_ASSERT_FATAL(false, "Unknown processor!\n");
      }
   return ans;
   }

bool
OMR::Z::CPU::supports_feature_old_api(uint32_t feature)
   {
   bool supported = false;
   switch(feature)
      {
      case OMR_FEATURE_S390_HIGH_WORD:
         supported = self()->getSupportsHighWordFacility();
         break;
      case OMR_FEATURE_S390_DFP:
         supported = self()->getSupportsDecimalFloatingPointFacility();
         break;
      case OMR_FEATURE_S390_FPE:
         supported = self()->getSupportsFloatingPointExtensionFacility();
         break;
      case OMR_FEATURE_S390_TE:
         supported = self()->getSupportsTransactionalMemoryFacility();
         break;
      case OMR_FEATURE_S390_RI:
         supported = self()->getSupportsRuntimeInstrumentationFacility();
         break;
      case OMR_FEATURE_S390_VECTOR_FACILITY:
         supported = self()->getSupportsVectorFacility();
         break;
      case OMR_FEATURE_S390_VECTOR_PACKED_DECIMAL:
         supported = self()->getSupportsVectorPackedDecimalFacility();
         break;
      case OMR_FEATURE_S390_MISCELLANEOUS_INSTRUCTION_EXTENSION_3:
         supported = self()->getSupportsMiscellaneousInstructionExtensions3Facility();
         break;
      case OMR_FEATURE_S390_VECTOR_FACILITY_ENHANCEMENT_2:
         supported = self()->getSupportsVectorFacilityEnhancement2();
         break;
      case OMR_FEATURE_S390_VECTOR_PACKED_DECIMAL_ENHANCEMENT_FACILITY:
         supported = self()->getSupportsVectorPackedDecimalEnhancementFacility();
         break;
      case OMR_FEATURE_S390_GUARDED_STORAGE:
         supported = self()->getSupportsGuardedStorageFacility();
         break;
      case OMR_FEATURE_S390_MISCELLANEOUS_INSTRUCTION_EXTENSION_2:
         supported = self()->getSupportsMiscellaneousInstructionExtensions2Facility();
         break;
      case OMR_FEATURE_S390_VECTOR_FACILITY_ENHANCEMENT_1:
         supported = self()->getSupportsVectorFacilityEnhancement1();
         break;
      default:
         TR_ASSERT_FATAL(false, "Unknown processor feature!\n");
      }
   return supported;
   }

const char*
OMR::Z::CPU::getProcessorName(int32_t machineId)
   {
   const char* result = "";

   switch (machineId)
      {
      case 1090:
      case 1091:
         result = "zPDT";
      break;

      case 2066:
         result = "z800";
      break;

      case 2084:
         result = "z990";
      break;

      case 2086:
         result = "z890";
      break;

      case 2064:
         result = "z900";
      break;

      case 2094:
      case 2096:
         result = "z9";
      break;

      case 2097:
      case 2098:
         result = "z10";
      break;

      case 2817:
         result = "z196";
      break;
      
      case 2818:
         result = "z114";
      break;

      case 2827:
      case 2828:
         result = "zEC12";
      break;

      case 2964:
      case 2965:
         result = "z13";
      break;

      case 3906:
      case 3907:
         result = "z14";
      break;

      case 8561:
      case 8562:
         result = "z15";
      break;

      case 9998:
      case 9999:
         result = "zNext";
      break;

      default:
         result = "Unknown";
      break;
      }

   return result;
   }

bool
OMR::Z::CPU::getSupportsArch(Architecture arch)
   {
   return _supportedArch >= arch;
   }

bool
OMR::Z::CPU::setSupportsArch(Architecture arch)
   {
   return _supportedArch = _supportedArch >= arch ? _supportedArch : arch;
   }

bool
OMR::Z::CPU::getSupportsHardwareSQRT()
   {
   return true;
   }

bool
OMR::Z::CPU::hasPopulationCountInstruction()
   {
   return self()->supportsFeature(OMR_FEATURE_S390_MISCELLANEOUS_INSTRUCTION_EXTENSION_3);
   }

bool
OMR::Z::CPU::getSupportsHighWordFacility()
   {
   return _flags.testAny(S390SupportsHPR);
   }

bool
OMR::Z::CPU::setSupportsHighWordFacility(bool value)
   {
   if (value)
      {
      _flags.set(S390SupportsHPR);
      }
   else
      {
      _flags.reset(S390SupportsHPR);
      }

   return value;
   }

bool
OMR::Z::CPU::getSupportsDecimalFloatingPointFacility()
   {
   return _flags.testAny(S390SupportsDFP);
   }

bool
OMR::Z::CPU::setSupportsDecimalFloatingPointFacility(bool value)
   {
   if (value)
      {
      _flags.set(S390SupportsDFP);
      }
   else
      {
      _flags.reset(S390SupportsDFP);
      }

   return value;
   }

bool
OMR::Z::CPU::getSupportsFloatingPointExtensionFacility()
   {
   return _flags.testAny(S390SupportsFPE);
   }

bool
OMR::Z::CPU::setSupportsFloatingPointExtensionFacility(bool value)
   {
   if (value)
      {
      _flags.set(S390SupportsFPE);
      }
   else
      {
      _flags.reset(S390SupportsFPE);
      }

   return value;
   }

bool
OMR::Z::CPU::getSupportsTransactionalMemoryFacility()
   {
   return _flags.testAny(S390SupportsTM);
   }

bool
OMR::Z::CPU::supportsTransactionalMemoryInstructions()
   {
   return self()->supportsFeature(OMR_FEATURE_S390_TE);
   }


bool
OMR::Z::CPU::setSupportsTransactionalMemoryFacility(bool value)
   {
   if (value)
      {
      _flags.set(S390SupportsTM);
      }
   else
      {
      _flags.reset(S390SupportsTM);
      }

   return value;
   }

bool
OMR::Z::CPU::getSupportsRuntimeInstrumentationFacility()
   {
   return _flags.testAny(S390SupportsRI);
   }

bool
OMR::Z::CPU::setSupportsRuntimeInstrumentationFacility(bool value)
   {
   if (value)
      {
      _flags.set(S390SupportsRI);
      }
   else
      {
      _flags.reset(S390SupportsRI);
      }

   return value;
   }

bool
OMR::Z::CPU::getSupportsVectorFacility()
   {
   return _flags.testAny(S390SupportsVectorFacility);
   }

bool
OMR::Z::CPU::setSupportsVectorFacility(bool value)
   {
   if (value)
      {
      _flags.set(S390SupportsVectorFacility);
      }
   else
      {
      _flags.reset(S390SupportsVectorFacility);
      }

   return value;
   }

bool
OMR::Z::CPU::getSupportsVectorPackedDecimalFacility()
   {
   return _flags.testAny(S390SupportsVectorPackedDecimalFacility);
   }

bool
OMR::Z::CPU::setSupportsVectorPackedDecimalFacility(bool value)
   {
   if (value)
      {
      _flags.set(S390SupportsVectorPackedDecimalFacility);
      }
   else
      {
      _flags.reset(S390SupportsVectorPackedDecimalFacility);
      }

   return value;
   }

bool
OMR::Z::CPU::getSupportsGuardedStorageFacility()
   {
   return _flags.testAny(S390SupportsGuardedStorageFacility);
   }

bool
OMR::Z::CPU::setSupportsGuardedStorageFacility(bool value)
   {
   if (value)
      {
      _flags.set(S390SupportsGuardedStorageFacility);
      }
   else
      {
      _flags.reset(S390SupportsGuardedStorageFacility);
      }

   return value;
   }

bool
OMR::Z::CPU::getSupportsMiscellaneousInstructionExtensions2Facility()
   {
   return _flags.testAny(S390SupportsMIE2);
   }

bool
OMR::Z::CPU::setSupportsMiscellaneousInstructionExtensions2Facility(bool value)
   {
   if (value)
      {
      _flags.set(S390SupportsMIE2);
      }
   else
      {
      _flags.reset(S390SupportsMIE2);
      }
   }

bool
OMR::Z::CPU::getSupportsMiscellaneousInstructionExtensions3Facility()
   {
   return _flags.testAny(S390SupportsMIE3);
   }

bool
OMR::Z::CPU::setSupportsMiscellaneousInstructionExtensions3Facility(bool value)
   {
   if (value)
      {
      _flags.set(S390SupportsMIE3);
      }
   else
      {
      _flags.reset(S390SupportsMIE3);
      }

   return value;
   }

bool
OMR::Z::CPU::getSupportsVectorFacilityEnhancement2()
   {
   return _flags.testAny(S390SupportsVectorFacilityEnhancement2);
   }

bool
OMR::Z::CPU::setSupportsVectorFacilityEnhancement2(bool value)
   {
   if (value)
      {
      _flags.set(S390SupportsVectorFacilityEnhancement2);
      }
   else
      {
      _flags.reset(S390SupportsVectorFacilityEnhancement2);
      }

   return value;
   }

bool
OMR::Z::CPU::getSupportsVectorFacilityEnhancement1()
   {
   return _flags.testAny(S390SupportsVectorFacilityEnhancement1);
   }

bool
OMR::Z::CPU::setSupportsVectorFacilityEnhancement1(bool value)
   {
   if (value)
      {
      _flags.set(S390SupportsVectorFacilityEnhancement1);
      }
   else
      {
      _flags.reset(S390SupportsVectorFacilityEnhancement1);
      }

   return value;
   }

bool
OMR::Z::CPU::getSupportsVectorPackedDecimalEnhancementFacility()
   {
   return _flags.testAny(S390SupportsVectorPDEnhancementFacility);
   }

bool
OMR::Z::CPU::setSupportsVectorPackedDecimalEnhancementFacility(bool value)
   {
   if (value)
      {
      _flags.set(S390SupportsVectorPDEnhancementFacility);
      }
   else
      {
      _flags.reset(S390SupportsVectorPDEnhancementFacility);
      }

   return value;
   }

bool
OMR::Z::CPU::isTargetWithinBranchRelativeRILRange(intptr_t targetAddress, intptr_t sourceAddress)
   {
   return (targetAddress == sourceAddress + ((intptr_t)((int32_t)((targetAddress - sourceAddress) / 2))) * 2) &&
            (targetAddress % 2 == 0);
   }

