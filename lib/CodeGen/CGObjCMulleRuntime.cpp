//===------- CGObjCMulleRuntime.cpp - Emit LLVM Code from ASTs for a Module --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This provides Objective-C code generation targeting the Mulle ObjC runtime.  
// The class in this file generates structures used by the Mulle Objective-C 
// runtime library.  These structures are defined elsewhere :)
//
//===----------------------------------------------------------------------===//

#include "CGObjCRuntime.h"
#include "CGCleanup.h"
#include "CodeGenFunction.h"
#include "CodeGenModule.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/StmtObjC.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Compiler.h"
#include <cstdarg>


using namespace clang;
using namespace CodeGen;

#define _MISSING_CODE() abort()
#define MISSING_CODE()  ( _MISSING_CODE(), (void *) 0)


// helper class that stores stuff
class _CGObjCMulleRuntime : public CodeGen::CGObjCRuntime
{
public:
   _CGObjCMulleRuntime(CodeGenModule &cgm) : CGObjCRuntime( cgm)
   {
   }
   
   /// DefinedSymbols - External symbols which are defined by this
   /// module. The symbols in this list and LazySymbols are used to add
   /// special linker symbols which ensure that Objective-C modules are
   /// linked properly.
   llvm::SetVector<IdentifierInfo*> DefinedSymbols;

   
  /// MethodDefinitions - map of methods which have been defined in
  /// this translation unit.
   llvm::DenseMap<const ObjCMethodDecl*, llvm::Function*> MethodDefinitions;
  
   llvm::Function *GetMethodDefinition(const ObjCMethodDecl *MD) 
   {
     llvm::DenseMap<const ObjCMethodDecl*, llvm::Function*>::iterator
      I = MethodDefinitions.find(MD);
      if (I != MethodDefinitions.end())
         return I->second;
      return nullptr;
   }
   
   void GetNameForMethod(const ObjCMethodDecl *D,
                                       const ObjCContainerDecl *CD,
                                       SmallVectorImpl<char> &Name) 
   {
      llvm::raw_svector_ostream OS(Name);
      assert (CD && "Missing container decl in GetNameForMethod");
      OS << '\01' << (D->isInstanceMethod() ? '-' : '+')
         << '[' << CD->getName();
      if (const ObjCCategoryImplDecl *CID =
          dyn_cast<ObjCCategoryImplDecl>(D->getDeclContext()))
     OS << '(' << *CID << ')';
     OS << ' ' << D->getSelector().getAsString() << ']';
   }
};


class CGObjCMulleRuntime : public _CGObjCMulleRuntime
{
protected:

  
public:
   CGObjCMulleRuntime(CodeGenModule &cgm) : _CGObjCMulleRuntime( cgm)
   {
   }

  /// Generate the function required to register all Objective-C components in
  /// this compilation unit with the runtime library.
   virtual llvm::Function *ModuleInitFunction()
   {
       return( (llvm::Function *) MISSING_CODE());
   }

  /// Get a selector for the specified name and type values. The
  /// return value should have the LLVM type for pointer-to
  /// ASTContext::getObjCSelType().
  virtual llvm::Value *GetSelector(CodeGenFunction &CGF,
                                   Selector Sel, bool lval=false)
  {
      return( (llvm::Value *) MISSING_CODE());
  }

  /// Get a typed selector.
  virtual llvm::Value *GetSelector(CodeGenFunction &CGF,
                                   const ObjCMethodDecl *Method) 
  {
      return( (llvm::Value *) MISSING_CODE());
  }

  /// Get the type constant to catch for the given ObjC pointer type.
  /// This is used externally to implement catching ObjC types in C++.
  /// Runtimes which don't support this should add the appropriate
  /// error to Sema.
  virtual llvm::Constant *GetEHType(QualType T) 
  {
      return( (llvm::Constant *) MISSING_CODE());
  }

  /// Generate a constant string object.
  virtual llvm::Constant *GenerateConstantString(const StringLiteral *) 
  {
      return( (llvm::Constant *) MISSING_CODE());
  }
  
  /// Generate a category.  A category contains a list of methods (and
  /// accompanying metadata) and a list of protocols.
  virtual void GenerateCategory(const ObjCCategoryImplDecl *OCD)
  {
      _MISSING_CODE();
  }

  /// Generate a class structure for this class.
/*
  struct _objc_class {
  Class isa;
  Class super_class;
  const char *name;
  long version;
  long info;
  long instance_size;
  struct _objc_ivar_list *ivars;
  struct _objc_method_list *methods;
  struct _objc_cache *cache;
  struct _objc_protocol_list *protocols;
  // Objective-C 1.0 extensions (<rdr://4585769>)
  const char *ivar_layout;
  struct _objc_class_ext *ext;
  };

  See EmitClassExtension();
*/
   virtual void GenerateClass(const ObjCImplementationDecl *ID)
   {
      _MISSING_CODE();
   }

  /// Register an class alias.
  virtual void RegisterAlias(const ObjCCompatibleAliasDecl *OAD)
  {
      _MISSING_CODE();
  }

  /// Generate an Objective-C message send operation.
  ///
  /// \param Method - The method being called, this may be null if synthesizing
  /// a property setter or getter.
  virtual CodeGen::RValue
  GenerateMessageSend(CodeGen::CodeGenFunction &CGF,
                      ReturnValueSlot ReturnSlot,
                      QualType ResultType,
                      Selector Sel,
                      llvm::Value *Receiver,
                      const CallArgList &CallArgs,
                      const ObjCInterfaceDecl *Class = nullptr,
                      const ObjCMethodDecl *Method = nullptr)
  {
      _MISSING_CODE();
  }

  /// Generate an Objective-C message send operation to the super
  /// class initiated in a method for Class and with the given Self
  /// object.
  ///
  /// \param Method - The method being called, this may be null if synthesizing
  /// a property setter or getter.
  virtual CodeGen::RValue
  GenerateMessageSendSuper(CodeGen::CodeGenFunction &CGF,
                           ReturnValueSlot ReturnSlot,
                           QualType ResultType,
                           Selector Sel,
                           const ObjCInterfaceDecl *Class,
                           bool isCategoryImpl,
                           llvm::Value *Self,
                           bool IsClassMessage,
                           const CallArgList &CallArgs,
                           const ObjCMethodDecl *Method = nullptr) 
  {
      _MISSING_CODE();
  }

  /// Emit the code to return the named protocol as an object, as in a
  /// \@protocol expression.
  virtual llvm::Value *GenerateProtocolRef(CodeGenFunction &CGF,
                                           const ObjCProtocolDecl *OPD)
  {
      return( (llvm::Value *) MISSING_CODE());
  }

  /// Generate the named protocol.  Protocols contain method metadata but no
  /// implementations.
  virtual void GenerateProtocol(const ObjCProtocolDecl *OPD)
  {
      _MISSING_CODE();
  }

  /// Generate a function preamble for a method with the specified
  /// types.

  // FIXME: Current this just generates the Function definition, but really this
  // should also be generating the loads of the parameters, as the runtime
  // should have full control over how parameters are passed.
  virtual llvm::Function *GenerateMethod(const ObjCMethodDecl *OMD,
    const ObjCContainerDecl *CD)
  {
    SmallString<256> Name;
    
    GetNameForMethod( OMD, CD, Name);

    CodeGenTypes &Types          = CGM.getTypes();
    llvm::FunctionType *MethodTy = Types.GetFunctionType(Types.arrangeObjCMethodDeclaration(OMD));
    llvm::Function     *Method   = llvm::Function::Create( MethodTy, llvm::GlobalValue::InternalLinkage, Name.str(), &CGM.getModule());
    
    MethodDefinitions.insert(std::make_pair(OMD, Method));

    return Method;
   }
   
  
   
  /// Return the runtime function for getting properties.
  virtual llvm::Constant *GetPropertyGetFunction()
  {
      return( (llvm::Constant *) MISSING_CODE());
  }

  /// Return the runtime function for setting properties.
  virtual llvm::Constant *GetPropertySetFunction()
  {
  
      return( (llvm::Constant *) MISSING_CODE());
  }

  /// Return the runtime function for optimized setting properties.
  virtual llvm::Constant *GetOptimizedPropertySetFunction(bool atomic, 
                                                          bool copy) 
  {
      return( (llvm::Constant *) MISSING_CODE());
  }

  // API for atomic copying of qualified aggregates in getter.
  virtual llvm::Constant *GetGetStructFunction() 
  {
      return( (llvm::Constant *) MISSING_CODE());
  }

  // API for atomic copying of qualified aggregates in setter.
  virtual llvm::Constant *GetSetStructFunction() 
  {
      return( (llvm::Constant *) MISSING_CODE());
  }

  /// API for atomic copying of qualified aggregates with non-trivial copy
  /// assignment (c++) in setter.
  virtual llvm::Constant *GetCppAtomicObjectSetFunction() 
  {
      return( (llvm::Constant *) MISSING_CODE());
  }

  /// API for atomic copying of qualified aggregates with non-trivial copy
  /// assignment (c++) in getter.
  virtual llvm::Constant *GetCppAtomicObjectGetFunction() 
  {
      return( (llvm::Constant *) MISSING_CODE());
  }
  
  /// GetClass - Return a reference to the class for the given
  /// interface decl.
  virtual llvm::Value *GetClass(CodeGenFunction &CGF,
                                const ObjCInterfaceDecl *OID) 
  {
      return( (llvm::Value *) MISSING_CODE());
  }


// Moar
  /// EnumerationMutationFunction - Return the function that's called by the
  /// compiler when a mutation is detected during foreach iteration.
  virtual llvm::Constant *EnumerationMutationFunction() 
  {
      return( (llvm::Constant *) MISSING_CODE());
  }

  virtual void EmitSynchronizedStmt(CodeGen::CodeGenFunction &CGF,
                                    const ObjCAtSynchronizedStmt &S)
  {
    _MISSING_CODE();
  }

  virtual void EmitTryStmt(CodeGen::CodeGenFunction &CGF,
                           const ObjCAtTryStmt &S) 
  {
    _MISSING_CODE();
  }

  virtual void EmitThrowStmt(CodeGen::CodeGenFunction &CGF,
                             const ObjCAtThrowStmt &S,
                             bool ClearInsertionPoint=true)
  {
    _MISSING_CODE();
  }

  virtual llvm::Value *EmitObjCWeakRead(CodeGen::CodeGenFunction &CGF,
                                        llvm::Value *AddrWeakObj)
  {
    _MISSING_CODE();
  }

  virtual void EmitObjCWeakAssign(CodeGen::CodeGenFunction &CGF,
                                  llvm::Value *src, llvm::Value *dest)
  {
    _MISSING_CODE();
  }

  virtual void EmitObjCGlobalAssign(CodeGen::CodeGenFunction &CGF,
                                    llvm::Value *src, llvm::Value *dest,
                                    bool threadlocal=false)
  {
    _MISSING_CODE();
  }

  virtual void EmitObjCIvarAssign(CodeGen::CodeGenFunction &CGF,
                                  llvm::Value *src, llvm::Value *dest,
                                  llvm::Value *ivarOffset)
  {
    _MISSING_CODE();
  }

  virtual void EmitObjCStrongCastAssign(CodeGen::CodeGenFunction &CGF,
                                        llvm::Value *src, llvm::Value *dest)

  {
    _MISSING_CODE();
  }

  virtual LValue EmitObjCValueForIvar(CodeGen::CodeGenFunction &CGF,
                                      QualType ObjectTy,
                                      llvm::Value *BaseValue,
                                      const ObjCIvarDecl *Ivar,
                                      unsigned CVRQualifiers)
  {
    _MISSING_CODE();
  }

  virtual llvm::Value *EmitIvarOffset(CodeGen::CodeGenFunction &CGF,
                                      const ObjCInterfaceDecl *Interface,
                                      const ObjCIvarDecl *Ivar)
  {
    _MISSING_CODE();
  }

  virtual void EmitGCMemmoveCollectable(CodeGen::CodeGenFunction &CGF,
                                        llvm::Value *DestPtr,
                                        llvm::Value *SrcPtr,
                                        llvm::Value *Size)
  {
    _MISSING_CODE();
  }

  virtual llvm::Constant *BuildGCBlockLayout(CodeGen::CodeGenModule &CGM,
                                  const CodeGen::CGBlockInfo &blockInfo)
  {
      return( (llvm::Constant *) MISSING_CODE());
  }

  virtual llvm::Constant *BuildRCBlockLayout(CodeGen::CodeGenModule &CGM,
                                  const CodeGen::CGBlockInfo &blockInfo)
  {
      return( (llvm::Constant *) MISSING_CODE());
  }

  virtual llvm::Constant *BuildByrefLayout(CodeGen::CodeGenModule &CGM,
                                           QualType T)
  {
      return( (llvm::Constant *) MISSING_CODE());
  }
  
  virtual llvm::GlobalVariable *GetClassGlobal(const std::string &Name,
                                               bool Weak = false)  
  {
      return( (llvm::GlobalVariable *) MISSING_CODE());
  }
};



CGObjCRuntime *clang::CodeGen::CreateMulleObjCRuntime(CodeGenModule &CGM) 
{
  switch (CGM.getLangOpts().ObjCRuntime.getKind()) {
  case ObjCRuntime::Mulle:
    return new CGObjCMulleRuntime(CGM);

  case ObjCRuntime::MacOSX :
  case ObjCRuntime::FragileMacOSX :
  case ObjCRuntime::iOS :
  case ObjCRuntime::GCC :
  case ObjCRuntime::GNUstep :
  case ObjCRuntime::ObjFW :
    llvm_unreachable("these runtimes are not Mulle ObjC runtimes");
  }
  llvm_unreachable("bad runtime");
}

