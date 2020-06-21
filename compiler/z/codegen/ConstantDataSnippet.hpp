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

#ifndef S390CONSTANTDATASNIPPET_INCL
#define S390CONSTANTDATASNIPPET_INCL

#include "codegen/Snippet.hpp"

#include <stddef.h>
#include <stdint.h>
#include "compile/Compilation.hpp"
#include "env/jittypes.h"
#include "infra/Assert.hpp"
#include "OMRCodeGenerator.hpp"

class TR_Debug;
namespace TR { class CodeGenerator; }
namespace TR { class Instruction; }
namespace TR { class LabelSymbol; }
namespace TR { class Node; }
namespace TR { class Register; }
namespace TR { class Symbol; }
namespace TR { class SymbolReference; }
namespace TR { class UnresolvedDataSnippet; }

namespace TR {

/**
 * ConstantDataSnippet is used to hold shared data
 */
class S390ConstantDataSnippet : public TR::Snippet
   {
   protected:

   uint8_t _value[1<<TR_DEFAULT_DATA_SNIPPET_EXPONENT];

   uint32_t                      _length;
   TR::UnresolvedDataSnippet *_unresolvedDataSnippet;
   TR::SymbolReference*           _symbolReference;
   uint32_t                      _reloType;

   public:

   S390ConstantDataSnippet(TR::CodeGenerator *cg, TR::Node *, void *c, uint16_t size);

   virtual Kind getKind() { return IsConstantData; }

   virtual uint32_t getConstantSize() { return _length; }
   virtual int32_t getDataAs1Byte()  { return *((int8_t *) &_value); }
   virtual int32_t getDataAs2Bytes() { return *((int16_t *) &_value); }
   virtual int32_t getDataAs4Bytes() { return *((int32_t *) &_value); }
   virtual int64_t getDataAs8Bytes() { return *((int64_t *) &_value); }

   virtual uint8_t * getRawData()
      {
      return _value;
      }

   void addMetaDataForCodeAddress(uint8_t *cursor);
   virtual uint8_t *emitSnippetBody();
   virtual uint32_t getLength(int32_t estimatedSnippetStart);
   virtual uint32_t setLength(uint32_t length) { return _length=length; }

   TR::UnresolvedDataSnippet* getUnresolvedDataSnippet() { return _unresolvedDataSnippet;}
   TR::UnresolvedDataSnippet* setUnresolvedDataSnippet(TR::UnresolvedDataSnippet* uds)
      {
      return _unresolvedDataSnippet = uds;
      }
   TR::SymbolReference* getSymbolReference() { return _symbolReference;}
   TR::SymbolReference* setSymbolReference(TR::SymbolReference* sr)
      {
      return _symbolReference = sr;
      }

   uint32_t            getReloType() { return _reloType;}
   uint32_t            setReloType(uint32_t rt)
      {
      return _reloType = rt;
      }
   TR::Compilation* comp() { return TR::comp(); }

   };

class S390ConstantInstructionSnippet : public TR::S390ConstantDataSnippet
   {
   TR::Instruction * _instruction;

   public:

   S390ConstantInstructionSnippet(TR::CodeGenerator *cg, TR::Node *, TR::Instruction *);
   TR::Instruction * getInstruction() { return _instruction; }
   Kind getKind() { return IsConstantInstruction; }
   virtual uint32_t getLength(int32_t estimatedSnippetStart) { return 8; }
   uint32_t getConstantSize() { return 8; }
   virtual int64_t getDataAs8Bytes();
   uint8_t * emitSnippetBody();
   };

/**
 * Create constant data snippet with method ep and complete method signature
 */
class S390EyeCatcherDataSnippet : public TR::S390ConstantDataSnippet
   {
   uint8_t *  _value;

   public:

   S390EyeCatcherDataSnippet(TR::CodeGenerator *cg, TR::Node *);

   uint8_t *emitSnippetBody();
   Kind getKind() { return IsEyeCatcherData; }
   };

/**
 * WritableDataSnippet is used to hold patchable data
 */
class S390WritableDataSnippet : public TR::S390ConstantDataSnippet
   {
   public:

   S390WritableDataSnippet(TR::CodeGenerator *cg, TR::Node *, void *c, uint16_t size);

   virtual Kind getKind() { return IsWritableData; }
   };
}

#endif
