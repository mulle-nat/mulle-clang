//===--- CGObjCMulleRuntime.cpp - Emit LLVM Code from ASTs for a Module ---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This provides Objective-C code generation targeting the Mulle ObjC runtime.
//
// http://www.mulle-kybernetik.com/software/mulle-objc-runtime
//
// This is a tweaked copy of CGObjCMac.cpp. Because those files are as private
// as possible for some reason, inheritance would have been difficult and not
// very future proof. Then stuff got started thrown out and tweaked to taste.
// - OK, this is cargo cult programming :)
//
// A lot of the code isn't really used. Whenever this reaches
// a state of usefulness, unused stuff should get thrown out eventually.
// It's also certainly not my preferred coding style.
//
// @mulle-objc@ Memo: code places, that I modified elsewhere, should be marked
//              like this followed by a comment.
//===----------------------------------------------------------------------===//
#include "CGObjCRuntime.h"
#include "CGBlocks.h"
#include "CGCleanup.h"
#include "CGRecordLayout.h"
#include "CodeGenFunction.h"
#include "CodeGenModule.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/ParentMap.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/StmtObjC.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Basic/LangOptions.h"
#include "clang/CodeGen/CGFunctionInfo.h"
#include "clang/Frontend/CodeGenOptions.h"
#include "clang/Lex/LiteralSupport.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/Parser.h"
#include "clang/Sema/SemaDiagnostic.h" // ugliness
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>

#include "clang/AST/SelectorLocationsKind.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Compiler.h"

#define COMPATIBLE_MULLE_OBJC_RUNTIME_LOAD_VERSION         4


using namespace clang;
using namespace CodeGen;

namespace {

   // FIXME: We should find a nicer way to make the labels for metadata, string
   // concatenation is lame.

   class ObjCCommonTypesHelper {
   protected:
      llvm::LLVMContext &VMContext;

   public:
      // The types of these functions don't really matter because we
      // should always bitcast before calling them. (??)

      /// id mulle_objc_object_call (id, SEL, void *)
      ///
      /// The messenger, used for all message sends, except super calls
      /// might need another one, when params is null..
      ///

      llvm::Constant *getMessageSendFn( int optLevel) const {
         StringRef    name;
         // Add the non-lazy-bind attribute, since objc_msgSend is likely to
         // be called a lot.
         switch( optLevel)
         {
         default : name = "mulle_objc_object_inline_constant_methodid_call"; break;
         case 1  : name = "mulle_objc_object_constant_methodid_call"; break;
         case -1 :
         case 0  : name = "mulle_objc_object_call"; break;
         }

         llvm::Type *params[] = { ObjectPtrTy, SelectorIDTy, ParamsPtrTy };
         llvm::Constant *C;

         C  =  CGM.CreateRuntimeFunction(llvm::FunctionType::get(ObjectPtrTy,
                                                              params, false),
                                         name,
                                         llvm::AttributeSet::get(CGM.getLLVMContext(),
                                                                 llvm::AttributeSet::FunctionIndex,
                                                                 llvm::Attribute::NonLazyBind));
         return( C);
      }

      // just the same but differently typed for llvm
      llvm::Constant *getMessageSendNoParamFn( int optLevel) const {
         StringRef    name;
         // Add the non-lazy-bind attribute, since objc_msgSend is likely to
         // be called a lot.
         switch( optLevel)
         {
         default : name = "mulle_objc_object_inline_constant_methodid_call"; break;
         case 1  : name = "mulle_objc_object_constant_methodid_call"; break;
         case -1 :
         case 0  : name = "mulle_objc_object_call"; break;
         }

         llvm::Type *params[] = { ObjectPtrTy, SelectorIDTy };
         llvm::Constant *C;

         C  =  CGM.CreateRuntimeFunction(llvm::FunctionType::get(ObjectPtrTy,
                                                              params, false),
                                         name,
                                         llvm::AttributeSet::get(CGM.getLLVMContext(),
                                                                 llvm::AttributeSet::FunctionIndex,
                                                                 llvm::Attribute::NonLazyBind));

         return( C);
      }



      llvm::Constant *getMessageSendRetainFn() const {
         llvm::Type *params[] = { ObjectPtrTy };
         return CGM.CreateRuntimeFunction(llvm::FunctionType::get( ObjectPtrTy,
                                                                  params, false),
                                          "mulle_objc_object_retain");
      }

      llvm::Constant *getMessageSendReleaseFn() const {
         llvm::Type *params[] = { ObjectPtrTy };
         return CGM.CreateRuntimeFunction(llvm::FunctionType::get(CGM.VoidTy,
                                                                  params, false),
                                          "mulle_objc_object_release");
      }

      // improve legacy code to basically a no-op
      llvm::Constant *getMessageSendZoneFn() const {
         llvm::Type *params[] = { ObjectPtrTy };
         return CGM.CreateRuntimeFunction(llvm::FunctionType::get(CGM.VoidPtrTy,
                                                                  params, false),
                                          "mulle_objc_object_zone");
      }

      /// id mulle_objc_object_call_classid (id, SEL, void *, CLASSID)
      ///
      /// The messenger used for super calls
      ///
      llvm::Constant *getMessageSendSuperFn( int optLevel) const
      {
         StringRef    name;
         switch( optLevel)
         {
         default : name = "mulle_objc_object_inline_call_classid"; break;
         case -1 :
         case 0  : name = "mulle_objc_object_call_classid"; break;
         }

         llvm::Type *params[] = { ObjectPtrTy, SelectorIDTy, ParamsPtrTy, ClassIDTy  };
         llvm::Constant *C;

         C  =  CGM.CreateRuntimeFunction(llvm::FunctionType::get(ObjectPtrTy,
                                                              params, false),
                                         name,
                                         llvm::AttributeSet::get(CGM.getLLVMContext(),
                                                                 llvm::AttributeSet::FunctionIndex,
                                                                 llvm::Attribute::NonLazyBind));


         return( C);
      }

      llvm::Constant *getMessageSendMetaSuperFn( int optLevel) const
      {
         StringRef    name;
         switch( optLevel)
         {
         default : name = "mulle_objc_class_inline_metacall_classid"; break;
         case -1 :
         case 0  : name = "mulle_objc_class_metacall_classid"; break;
         }

         llvm::Type *params[] = { ObjectPtrTy, SelectorIDTy, ParamsPtrTy, ClassIDTy  };
         llvm::Constant *C;

         C  =  CGM.CreateRuntimeFunction(llvm::FunctionType::get(ObjectPtrTy,
                                                              params, false),
                                         name,
                                         llvm::AttributeSet::get(CGM.getLLVMContext(),
                                                                 llvm::AttributeSet::FunctionIndex,
                                                                 llvm::Attribute::NonLazyBind));


         return( C);
      }


   protected:
      CodeGen::CodeGenModule &CGM;

   public:
      llvm::Type *ShortTy, *IntTy, *LongTy, *LongLongTy;
      llvm::Type *Int8PtrTy, *Int8PtrPtrTy;
      llvm::Type *IvarOffsetVarTy;

      /// ObjectPtrTy - LLVM type for object handles (typeof(id))
      llvm::Type *ObjectPtrTy;

      /// PtrObjectPtrTy - LLVM type for id *
      llvm::Type *PtrObjectPtrTy;

      // mulle specific stuff for the various hashes
      // no energy for "pe" after "Ty" :D
      llvm::Type *ParamsPtrTy;

      llvm::Type *ClassIDTy;
      llvm::Type *CategoryIDTy;
      llvm::Type *IvarIDTy;
      llvm::Type *SelectorIDTy;
      llvm::Type *PropertyIDTy;
      llvm::Type *ProtocolIDTy;

      llvm::Type *ClassIDPtrTy;
      llvm::Type *IvarIDPtrTy;
      llvm::Type *CategoryIDTyPtrTy;
      llvm::Type *SelectorIDTyPtrTy;
      llvm::Type *PropertyIDTyPtrTy;
      llvm::Type *ProtocolIDPtrTy;

      llvm::StructType *StaticStringTy;
      llvm::StructType *HashNameTy;

      // the structures that get exported
      llvm::Type *ClassListTy;
      llvm::Type *CategoryListTy;
      llvm::Type *StaticStringListTy;
      llvm::Type *HashNameListTy;
      llvm::Type *LoadInfoTy;


   private:
      /// ProtocolPtrTy - LLVM type for external protocol handles
      /// (typeof(Protocol))
      llvm::Type *ExternalProtocolPtrTy;

   public:
      llvm::Type *getExternalProtocolPtrTy() {
         if (!ExternalProtocolPtrTy) {
            // FIXME: It would be nice to unify this with the opaque type, so that the
            // IR comes out a bit cleaner.
            CodeGen::CodeGenTypes &Types = CGM.getTypes();
            ASTContext &Ctx = CGM.getContext();
            llvm::Type *T = Types.ConvertType(Ctx.getObjCProtoType());
            ExternalProtocolPtrTy = llvm::PointerType::getUnqual(T);
         }

         return ExternalProtocolPtrTy;
      }

      /// PropertyTy - LLVM type for struct objc_property (struct _prop_t
      /// in GCC parlance).
      llvm::StructType *PropertyTy;

      /// PropertyListTy - LLVM type for struct objc_property_list
      /// (_prop_list_t in GCC parlance).
      llvm::StructType *PropertyListTy;
      /// PropertyListPtrTy - LLVM type for struct objc_property_list*.
      llvm::Type *PropertyListPtrTy;

      // DefTy - LLVM type for struct objc_method.
      llvm::StructType *MethodTy;

      /// CacheTy - LLVM type for struct objc_cache.
      llvm::Type *CacheTy;
      /// CachePtrTy - LLVM type for struct objc_cache *.
      llvm::Type *CachePtrTy;


      // TODO: use different code for different optlevel, like mulle_objc_uninlined_unfailing_get_or_lookup_class
      llvm::Constant *getGetRuntimeClassFn( int optLevel) {
         llvm::Type *params[] = { ClassIDTy };
         llvm::Constant *fn;
         StringRef    name;
         switch( optLevel)
         {
         default : name = "mulle_objc_inline_unfailing_get_or_lookup_class"; break;
         case -1 :
         case 0  : name = "mulle_objc_unfailing_get_or_lookup_class"; break;
         }

         llvm::AttributeSet   attributes = llvm::AttributeSet::get(CGM.getLLVMContext(),
                                                                  llvm::AttributeSet::FunctionIndex,
                                                                  llvm::Attribute::ReadNone);
         attributes.addAttribute(CGM.getLLVMContext(),
                                                                  llvm::AttributeSet::FunctionIndex,
                                                                  llvm::Attribute::NoUnwind);
         fn = CGM.CreateRuntimeFunction(llvm::FunctionType::get( ObjectPtrTy,

                                                                  params, false),
                                          name,
                                          attributes);
         return( fn);
      }

      llvm::Constant *getGetPropertyFn() {
         CodeGen::CodeGenTypes &Types = CGM.getTypes();
         ASTContext &Ctx = CGM.getContext();

         // id objc_getProperty (id, SEL, ptrdiff_t, bool)
         SmallVector<CanQualType,4> Params;
         CanQualType IdType = Ctx.getCanonicalParamType(Ctx.getObjCIdType());
         CanQualType SelType = Ctx.getCanonicalParamType(Ctx.getObjCSelType());
         Params.push_back(IdType);
         Params.push_back(SelType);
         Params.push_back(Ctx.getPointerDiffType()->getCanonicalTypeUnqualified());
         Params.push_back(Ctx.BoolTy);
         llvm::FunctionType *FTy =
         Types.GetFunctionType(Types.arrangeLLVMFunctionInfo(
                                                             IdType, false, false, Params, FunctionType::ExtInfo(), {},
                                                              RequiredArgs::All));
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_object_get_property_value");
      }

      llvm::Constant *getSetPropertyFn() {
         CodeGen::CodeGenTypes &Types = CGM.getTypes();
         ASTContext &Ctx = CGM.getContext();
         // void objc_setProperty (id, SEL, ptrdiff_t, id, bool, bool)
         SmallVector<CanQualType,6> Params;
         CanQualType IdType = Ctx.getCanonicalParamType(Ctx.getObjCIdType());
         CanQualType SelType = Ctx.getCanonicalParamType(Ctx.getObjCSelType());
         Params.push_back(IdType);
         Params.push_back(SelType);
         Params.push_back(Ctx.getPointerDiffType()->getCanonicalTypeUnqualified());
         Params.push_back(IdType);
         Params.push_back(Ctx.BoolTy);
         Params.push_back(Ctx.BoolTy);
         llvm::FunctionType *FTy =
         Types.GetFunctionType(Types.arrangeLLVMFunctionInfo(
                                                             Ctx.VoidTy, false, false, Params, FunctionType::ExtInfo(), {},
                                                             RequiredArgs::All));
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_object_set_property_value");
      }

      // this is not really used
      llvm::Constant *getOptimizedSetPropertyFn(bool atomic, bool copy) {
         CodeGen::CodeGenTypes &Types = CGM.getTypes();
         ASTContext &Ctx = CGM.getContext();

         // void objc_setProperty_atomic(id self, SEL _cmd,
         //                              id newValue, ptrdiff_t offset);
         // void objc_setProperty_nonatomic(id self, SEL _cmd,
         //                                 id newValue, ptrdiff_t offset);
         // void objc_setProperty_atomic_copy(id self, SEL _cmd,
         //                                   id newValue, ptrdiff_t offset);
         // void objc_setProperty_nonatomic_copy(id self, SEL _cmd,
         //                                      id newValue, ptrdiff_t offset);

         SmallVector<CanQualType,4> Params;
         CanQualType IdType = Ctx.getCanonicalParamType(Ctx.getObjCIdType());
         CanQualType SelType = Ctx.getCanonicalParamType(Ctx.getObjCSelType());
         Params.push_back(IdType);
         Params.push_back(SelType);
         Params.push_back(IdType);
         Params.push_back(Ctx.getPointerDiffType()->getCanonicalTypeUnqualified());
         llvm::FunctionType *FTy =
         Types.GetFunctionType(Types.arrangeLLVMFunctionInfo(
                                                             Ctx.VoidTy, false, false, Params, FunctionType::ExtInfo(), {},
                                                             RequiredArgs::All));
         const char *name;
         if (atomic && copy)
            name = "mulle_objc_object_set_property_atomic_copy";
         else if (atomic && !copy)
            name = "mulle_objc_object_set_property_atomic";
         else if (!atomic && copy)
            name = "mulle_objc_object_set_property_nonatomic_copy";
         else
            name = "mulle_objc_object_set_property_nonatomic";

         return CGM.CreateRuntimeFunction(FTy, name);
      }

      /*
       * Support for all the following Functions is kinda doubtful ATM
       */
      llvm::Constant *getCopyStructFn() {
         CodeGen::CodeGenTypes &Types = CGM.getTypes();
         ASTContext &Ctx = CGM.getContext();
         // void objc_copyStruct (void *, const void *, size_t, bool, bool)
         SmallVector<CanQualType,5> Params;
         Params.push_back(Ctx.VoidPtrTy);
         Params.push_back(Ctx.VoidPtrTy);
         Params.push_back(Ctx.LongTy);
         Params.push_back(Ctx.BoolTy);
         Params.push_back(Ctx.BoolTy);
         llvm::FunctionType *FTy =
         Types.GetFunctionType(Types.arrangeLLVMFunctionInfo(
                                                             Ctx.VoidTy, false, false, Params, FunctionType::ExtInfo(), {},
                                                             RequiredArgs::All));
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_copy_struct");
      }

      /// This routine declares and returns address of:
      /// void objc_copyCppObjectAtomic(
      ///         void *dest, const void *src,
      ///         void (*copyHelper) (void *dest, const void *source));
      llvm::Constant *getCppAtomicObjectFunction() {
         CodeGen::CodeGenTypes &Types = CGM.getTypes();
         ASTContext &Ctx = CGM.getContext();
         /// void objc_copyCppObjectAtomic(void *dest, const void *src, void *helper);
         SmallVector<CanQualType,3> Params;
         Params.push_back(Ctx.VoidPtrTy);
         Params.push_back(Ctx.VoidPtrTy);
         Params.push_back(Ctx.VoidPtrTy);
         llvm::FunctionType *FTy =
         Types.GetFunctionType(Types.arrangeLLVMFunctionInfo(Ctx.VoidTy, false, false,
                                                             Params,
                                                             FunctionType::ExtInfo(), {},
                                                             RequiredArgs::All));
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_copy_cpp_object_atomic");
      }

      llvm::Constant *getEnumerationMutationFn() {
         CodeGen::CodeGenTypes &Types = CGM.getTypes();
         ASTContext &Ctx = CGM.getContext();
         // void objc_enumerationMutation (id)
         SmallVector<CanQualType,1> Params;
         Params.push_back(Ctx.getCanonicalParamType(Ctx.getObjCIdType()));
         llvm::FunctionType *FTy =
         Types.GetFunctionType(Types.arrangeLLVMFunctionInfo(
                                                             Ctx.VoidTy, false, false, Params, FunctionType::ExtInfo(), {},
                                                             RequiredArgs::All));
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_enumeration_mutation");
      }

      /// GcReadWeakFn -- LLVM objc_read_weak (id *src) function.
      llvm::Constant *getGcReadWeakFn() {
         // id objc_read_weak (id *)
         llvm::Type *args[] = { ObjectPtrTy->getPointerTo() };
         llvm::FunctionType *FTy =
         llvm::FunctionType::get(ObjectPtrTy, args, false);
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_read_weak");
      }

      /// GcAssignWeakFn -- LLVM objc_assign_weak function.
      llvm::Constant *getGcAssignWeakFn() {
         // id objc_assign_weak (id, id *)
         llvm::Type *args[] = { ObjectPtrTy, ObjectPtrTy->getPointerTo() };
         llvm::FunctionType *FTy =
         llvm::FunctionType::get(ObjectPtrTy, args, false);
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_assign_weak");
      }

      /// GcAssignGlobalFn -- LLVM objc_assign_global function.
      llvm::Constant *getGcAssignGlobalFn() {
         // id objc_assign_global(id, id *)
         llvm::Type *args[] = { ObjectPtrTy, ObjectPtrTy->getPointerTo() };
         llvm::FunctionType *FTy =
         llvm::FunctionType::get(ObjectPtrTy, args, false);
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_assign_global");
      }

      /// GcAssignThreadLocalFn -- LLVM objc_assign_threadlocal function.
      llvm::Constant *getGcAssignThreadLocalFn() {
         // id objc_assign_threadlocal(id src, id * dest)
         llvm::Type *args[] = { ObjectPtrTy, ObjectPtrTy->getPointerTo() };
         llvm::FunctionType *FTy =
         llvm::FunctionType::get(ObjectPtrTy, args, false);
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_assign_threadlocal");
      }

      /// GcAssignIvarFn -- LLVM objc_assign_ivar function.
      llvm::Constant *getGcAssignIvarFn() {
         // id objc_assign_ivar(id, id *, ptrdiff_t)
         llvm::Type *args[] = { ObjectPtrTy, ObjectPtrTy->getPointerTo(),
            CGM.PtrDiffTy };
         llvm::FunctionType *FTy =
         llvm::FunctionType::get(ObjectPtrTy, args, false);
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_assign_ivar");
      }

      /// GcMemmoveCollectableFn -- LLVM objc_memmove_collectable function.
      llvm::Constant *GcMemmoveCollectableFn() {
         // void *objc_memmove_collectable(void *dst, const void *src, size_t size)
         llvm::Type *args[] = { Int8PtrTy, Int8PtrTy, LongTy };
         llvm::FunctionType *FTy = llvm::FunctionType::get(Int8PtrTy, args, false);
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_memmove_collectable");
      }

      /// GcAssignStrongCastFn -- LLVM objc_assign_strongCast function.
      llvm::Constant *getGcAssignStrongCastFn() {
         // id objc_assign_strongCast(id, id *)
         llvm::Type *args[] = { ObjectPtrTy, ObjectPtrTy->getPointerTo() };
         llvm::FunctionType *FTy =
         llvm::FunctionType::get(ObjectPtrTy, args, false);
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_assign_strong_cast");
      }

      /// ExceptionThrowFn - LLVM objc_exception_throw function.
      llvm::Constant *getExceptionThrowFn() {
         // void objc_exception_throw(id)
         llvm::Type *args[] = { ObjectPtrTy };
         llvm::FunctionType *FTy =
         llvm::FunctionType::get(CGM.VoidTy, args, false);
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_exception_throw");
      }

      /// ExceptionRethrowFn - LLVM objc_exception_rethrow function.
      llvm::Constant *getExceptionRethrowFn() {
         // void objc_exception_rethrow(void)
         llvm::FunctionType *FTy = llvm::FunctionType::get(CGM.VoidTy, false);
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_exception_rethrow");
      }

      /// SyncEnterFn - LLVM object_sync_enter function.
      llvm::Constant *getSyncEnterFn() {
         // int objc_sync_enter (id)
         llvm::Type *args[] = { ObjectPtrTy };
         llvm::FunctionType *FTy =
         llvm::FunctionType::get(CGM.IntTy, args, false);
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_sync_enter");
      }

      /// SyncExitFn - LLVM object_sync_exit function.
      llvm::Constant *getSyncExitFn() {
         // int objc_sync_exit (id)
         llvm::Type *args[] = { ObjectPtrTy };
         llvm::FunctionType *FTy =
         llvm::FunctionType::get(CGM.IntTy, args, false);
         return CGM.CreateRuntimeFunction(FTy, "mulle_objc_sync_exit");
      }

      ObjCCommonTypesHelper(CodeGen::CodeGenModule &cgm);
      ~ObjCCommonTypesHelper(){}
   };

   /// ObjCTypesHelper - Helper class that encapsulates lazy
   /// construction of varies types used during ObjC generation.
   class ObjCTypesHelper : public ObjCCommonTypesHelper {
   public:
      /// SymtabTy - LLVM type for struct objc_symtab.
      llvm::StructType *SymtabTy;
      /// SymtabPtrTy - LLVM type for struct objc_symtab *.
      llvm::Type *SymtabPtrTy;
      /// ModuleTy - LLVM type for struct objc_module.
      llvm::StructType *ModuleTy;

      /// ProtocolTy - LLVM type for struct objc_protocol.
      llvm::StructType *ProtocolTy;
      /// ProtocolPtrTy - LLVM type for struct objc_protocol *.
      llvm::Type *ProtocolPtrTy;
      /// ProtocolExtensionTy - LLVM type for struct
      /// objc_protocol_extension.
      llvm::StructType *ProtocolExtensionTy;
      /// ProtocolExtensionTy - LLVM type for struct
      /// objc_protocol_extension *.
      llvm::Type *ProtocolExtensionPtrTy;
      /// MethodDescriptionTy - LLVM type for struct
      /// objc_method_description.
      llvm::StructType *MethodDescriptionTy;
      /// MethodDescriptionListTy - LLVM type for struct
      /// objc_method_description_list.
      llvm::StructType *MethodDescriptionListTy;
      /// MethodDescriptionListPtrTy - LLVM type for struct
      /// objc_method_description_list *.
      llvm::Type *MethodDescriptionListPtrTy;
      /// ProtocolListTy - LLVM type for struct objc_property_list.
      llvm::StructType *ProtocolListTy;
      /// ProtocolListPtrTy - LLVM type for struct objc_property_list*.
      llvm::Type *ProtocolListPtrTy;
      /// CategoryTy - LLVM type for struct objc_category.
      llvm::StructType *CategoryTy;
      /// ClassTy - LLVM type for struct objc_class.
      llvm::StructType *ClassTy;
      /// ClassPtrTy - LLVM type for struct objc_class *.
      llvm::Type *ClassPtrTy;
      /// ClassExtensionTy - LLVM type for struct objc_class_ext.
      llvm::StructType *ClassExtensionTy;
      /// ClassExtensionPtrTy - LLVM type for struct objc_class_ext *.
      llvm::Type *ClassExtensionPtrTy;
      // IvarTy - LLVM type for struct objc_ivar.
      llvm::StructType *IvarTy;
      /// IvarListTy - LLVM type for struct objc_ivar_list.
      llvm::Type *IvarListTy;
      /// IvarListPtrTy - LLVM type for struct objc_ivar_list *.
      llvm::Type *IvarListPtrTy;
      /// MethodListTy - LLVM type for struct objc_method_list.
      llvm::Type *MethodListTy;
      /// MethodListPtrTy - LLVM type for struct objc_method_list *.
      llvm::Type *MethodListPtrTy;

      /// ExceptionDataTy - LLVM type for struct _objc_exception_data.
      private:  // @mulle-objc@ need to keep this lazy
      llvm::Type *ExceptionDataTy;

      public:
      /// ExceptionTryEnterFn - LLVM objc_exception_try_enter function.
      llvm::Constant *getExceptionTryEnterFn() {
         llvm::Type *params[] = { getExceptionDataTy( CGM)->getPointerTo() };
         return CGM.CreateRuntimeFunction(
                                          llvm::FunctionType::get(CGM.VoidTy, params, false),
                                          "mulle_objc_exception_try_enter");
      }

      /// ExceptionTryExitFn - LLVM objc_exception_try_exit function.
      llvm::Constant *getExceptionTryExitFn() {
         llvm::Type *params[] = { getExceptionDataTy( CGM)->getPointerTo() };
         return CGM.CreateRuntimeFunction(
                                          llvm::FunctionType::get(CGM.VoidTy, params, false),
                                          "mulle_objc_exception_try_exit");
      }

      /// ExceptionExtractFn - LLVM objc_exception_extract function.
      llvm::Constant *getExceptionExtractFn() {
         llvm::Type *params[] = { getExceptionDataTy( CGM)->getPointerTo() };
         return CGM.CreateRuntimeFunction(llvm::FunctionType::get(ObjectPtrTy,
                                                                  params, false),
                                          "mulle_objc_exception_extract");
      }

      /// ExceptionMatchFn - LLVM objc_exception_match function.
      llvm::Constant *getExceptionMatchFn() {
         llvm::Type *params[] = { ClassIDTy, ObjectPtrTy };
         return CGM.CreateRuntimeFunction(
                                          llvm::FunctionType::get(CGM.Int32Ty, params, false),
                                          "mulle_objc_exception_match");

      }

      /// SetJmpFn - LLVM _setjmp function.
      llvm::Constant *getSetJmpFn() {
         // This is specifically the prototype for x86.
         llvm::Type *params[] = { CGM.Int32Ty->getPointerTo() };
         return
         CGM.CreateRuntimeFunction(llvm::FunctionType::get(CGM.Int32Ty,
                                                           params, false),
                                   "_setjmp",
                                   llvm::AttributeSet::get(CGM.getLLVMContext(),
                                                           llvm::AttributeSet::FunctionIndex,
                                                           llvm::Attribute::NonLazyBind));
      }

   public:
      ObjCTypesHelper(CodeGen::CodeGenModule &cgm);
      ~ObjCTypesHelper() {}

      llvm::Type  *getExceptionDataTy( CodeGen::CodeGenModule &cgm);
   };


   /*
    * Biting the hand that feeds, but damn if this files isn't way
    * too long, way to unstructured and all in C++.
    *
    */

   class CGObjCCommonMulleRuntime : public CodeGen::CGObjCRuntime {
   public:
      // FIXME - accessibility
      class GC_IVAR {
      public:
         unsigned ivar_bytepos;
         unsigned ivar_size;
         GC_IVAR(unsigned bytepos = 0, unsigned size = 0)
         : ivar_bytepos(bytepos), ivar_size(size) {}

         // Allow sorting based on byte pos.
         bool operator<(const GC_IVAR &b) const {
            return ivar_bytepos < b.ivar_bytepos;
         }
      };

      class SKIP_SCAN {
      public:
         unsigned skip;
         unsigned scan;
         SKIP_SCAN(unsigned _skip = 0, unsigned _scan = 0)
         : skip(_skip), scan(_scan) {}
      };

      /// opcode for captured block variables layout 'instructions'.
      /// In the following descriptions, 'I' is the value of the immediate field.
      /// (field following the opcode).
      ///
      enum BLOCK_LAYOUT_OPCODE {
         /// An operator which affects how the following layout should be
         /// interpreted.
         ///   I == 0: Halt interpretation and treat everything else as
         ///           a non-pointer.  Note that this instruction is equal
         ///           to '\0'.
         ///   I != 0: Currently unused.
         BLOCK_LAYOUT_OPERATOR            = 0,

         /// The next I+1 bytes do not contain a value of object pointer type.
         /// Note that this can leave the stream unaligned, meaning that
         /// subsequent word-size instructions do not begin at a multiple of
         /// the pointer size.
         BLOCK_LAYOUT_NON_OBJECT_BYTES    = 1,

         /// The next I+1 words do not contain a value of object pointer type.
         /// This is simply an optimized version of BLOCK_LAYOUT_BYTES for
         /// when the required skip quantity is a multiple of the pointer size.
         BLOCK_LAYOUT_NON_OBJECT_WORDS    = 2,

         /// The next I+1 words are __strong pointers to Objective-C
         /// objects or blocks.
         BLOCK_LAYOUT_STRONG              = 3,

         /// The next I+1 words are pointers to __block variables.
         BLOCK_LAYOUT_BYREF               = 4,

         /// The next I+1 words are __weak pointers to Objective-C
         /// objects or blocks.
         BLOCK_LAYOUT_WEAK                = 5,

         /// The next I+1 words are __unsafe_unretained pointers to
         /// Objective-C objects or blocks.
         BLOCK_LAYOUT_UNRETAINED          = 6

         /// The next I+1 words are block or object pointers with some
         /// as-yet-unspecified ownership semantics.  If we add more
         /// flavors of ownership semantics, values will be taken from
         /// this range.
         ///
         /// This is included so that older tools can at least continue
         /// processing the layout past such things.
         //BLOCK_LAYOUT_OWNERSHIP_UNKNOWN = 7..10,

         /// All other opcodes are reserved.  Halt interpretation and
         /// treat everything else as opaque.
      };

      class RUN_SKIP {
      public:
         enum BLOCK_LAYOUT_OPCODE opcode;
         CharUnits block_var_bytepos;
         CharUnits block_var_size;
         RUN_SKIP(enum BLOCK_LAYOUT_OPCODE Opcode = BLOCK_LAYOUT_OPERATOR,
                  CharUnits BytePos = CharUnits::Zero(),
                  CharUnits Size = CharUnits::Zero())
         : opcode(Opcode), block_var_bytepos(BytePos),  block_var_size(Size) {}

         // Allow sorting based on byte pos.
         bool operator<(const RUN_SKIP &b) const {
            return block_var_bytepos < b.block_var_bytepos;
         }
      };

   protected:
      llvm::LLVMContext &VMContext;
      // FIXME! May not be needing this after all.
      unsigned ObjCABI;

      int64_t   no_tagged_pointers;
      int64_t   foundation_version;
      int64_t   load_version;
      int64_t   runtime_version;
      int64_t   thread_local_runtime;
      int64_t   user_version;
      // @mulle-objc@ uniqueid: make it 32 bit here
      uint32_t  fastclassids[ 32];
      bool      fastclassids_defined;
      bool      _trace_fastids;

      // gc ivar layout bitmap calculation helper caches.
      SmallVector<GC_IVAR, 16> SkipIvars;
      SmallVector<GC_IVAR, 16> IvarsInfo;

      // arc/mrr layout of captured block literal variables.
      SmallVector<RUN_SKIP, 16> RunSkipBlockVars;

      /// LazySymbols - Symbols to generate a lazy reference for. See
      /// DefinedSymbols and FinishModule().
      llvm::SetVector<IdentifierInfo*> LazySymbols;

      /// mulle::start
      /// HashNames - uniqued hashes for debugging.
      llvm::StringMap<llvm::ConstantInt*> DefinedHashes;

      /// IvarNames - uniqued ivar names. We have to use
      /// a StringMap here because have no other unique reference.
      llvm::StringMap<llvm::GlobalVariable*> IvarNames;

      /// IvarTypes - uniqued ivar types. We have to use
      /// a StringMap here because have no other unique reference.
      llvm::StringMap<llvm::GlobalVariable*> IvarTypes;
      /// mulle::end


      /// DefinedSymbols - External symbols which are defined by this
      /// module. The symbols in this list and LazySymbols are used to add
      /// special linker symbols which ensure that Objective-C modules are
      /// linked properly.
      llvm::SetVector<IdentifierInfo*> DefinedSymbols;

      /// ClassNames - uniqued class names.
      llvm::StringMap<llvm::GlobalVariable*> ClassNames;

      /// MethodVarNames - uniqued method variable names.
      llvm::DenseMap<Selector, llvm::GlobalVariable*> MethodVarNames;

      /// DefinedCategoryNames - list of category names in form Class_Category.
      llvm::SmallSetVector<std::string, 16>  DefinedCategoryNames;

      /// MethodVarTypes - uniqued method type signatures. We have to use
      /// a StringMap here because have no other unique reference.
      llvm::StringMap<llvm::GlobalVariable*> MethodVarTypes;

      /// MethodDefinitions - map of methods which have been defined in
      /// this translation unit.
      llvm::DenseMap<const ObjCMethodDecl*, llvm::Function*> MethodDefinitions;

      /// PropertyNames - uniqued method variable names.
      llvm::DenseMap<IdentifierInfo*, llvm::GlobalVariable*> PropertyNames;

      /// DeclaredClassNames - all known class names.
      std::set<std::string> DeclaredClassNames;

      /// SelectorReferences - uniqued selector references.
      llvm::DenseMap<Selector, llvm::GlobalVariable*> SelectorReferences;

      /// Protocols - Protocols for which an objc_protocol structure has
      /// been emitted. Forward declarations are handled by creating an
      /// empty structure whose initializer is filled in when/if defined.
      llvm::DenseMap<IdentifierInfo*, llvm::GlobalVariable*> Protocols;

      /// DefinedProtocols - Protocols which have actually been
      /// defined. We should not need this, see FIXME in GenerateProtocol.
      llvm::DenseSet<IdentifierInfo*> DefinedProtocols;

      /// DefinedClasses - List of defined classes.
      SmallVector<llvm::GlobalValue*, 16> DefinedClasses;

      /// ImplementedClasses - List of @implemented classes.
      SmallVector<const ObjCInterfaceDecl*, 16> ImplementedClasses;

      /// DefinedNonLazyClasses - List of defined "non-lazy" classes.
      SmallVector<llvm::GlobalValue*, 16> DefinedNonLazyClasses;

      /// DefinedCategories - List of defined categories.
      SmallVector<llvm::GlobalValue*, 16> DefinedCategories;

      /// DefinedNonLazyCategories - List of defined "non-lazy" categories.
      SmallVector<llvm::GlobalValue*, 16> DefinedNonLazyCategories;

      /// GetNameForMethod - Return a name for the given method.
      /// \param[out] NameOut - The return value.
      void GetNameForMethod(const ObjCMethodDecl *OMD,
                            const ObjCContainerDecl *CD,
                            SmallVectorImpl<char> &NameOut);

      /// GetMethodVarName - Return a unique constant for the given
      /// selector's name. The return value has type char *.
      llvm::Constant *GetMethodVarName(Selector Sel);
      llvm::Constant *GetMethodVarName(IdentifierInfo *Ident);

      /// GetMethodVarType - Return a unique constant for the given
      /// method's type encoding string. The return value has type char *.

      // FIXME: This is a horrible name.
      llvm::Constant *GetMethodVarType(const ObjCMethodDecl *D,
                                       bool Extended = false);
      llvm::Constant *GetMethodVarType(const FieldDecl *D);

      llvm::Constant *GetIvarType(const ObjCIvarDecl *Ivar);
      llvm::Constant *GetIvarName(const ObjCIvarDecl *Ivar);

      /// GetPropertyName - Return a unique constant for the given
      /// name. The return value has type char *.
      llvm::Constant *GetPropertyName(IdentifierInfo *Ident);

      // FIXME: This can be dropped once string functions are unified.
      llvm::Constant *GetPropertyTypeString(const ObjCPropertyDecl *PD,
                                            const Decl *Container);

      /// GetClassName - Return a unique constant for the given selector's
      /// runtime name (which may change via use of objc_runtime_name attribute on
      /// class or protocol definition. The return value has type char *.
      llvm::Constant *GetClassName(StringRef RuntimeName);

      llvm::Function *GetMethodDefinition(const ObjCMethodDecl *MD);

      // dup from CodeGenModule, but easier here
      llvm::StructType *NSConstantStringType;
      llvm::StringMap<llvm::GlobalAlias *> NSConstantStringMap;

      llvm::StringMapEntry<llvm::GlobalAlias *> &GetNSConstantStringMapEntry( const StringLiteral *Literal, unsigned &StringLength);

      ConstantAddress   GenerateConstantString(const StringLiteral *SL) override;

      /// BuildIvarLayout - Builds ivar layout bitmap for the class
      /// implementation for the __strong or __weak case.
      ///
      llvm::Constant *BuildIvarLayout(const ObjCImplementationDecl *OI,
                                      bool ForStrongLayout);

      llvm::Constant *BuildIvarLayoutBitmap(std::string &BitMap);

      void BuildAggrIvarRecordLayout(const RecordType *RT,
                                     unsigned int BytePos, bool ForStrongLayout,
                                     bool &HasUnion);
      void BuildAggrIvarLayout(const ObjCImplementationDecl *OI,
                               const llvm::StructLayout *Layout,
                               const RecordDecl *RD,
                               ArrayRef<const FieldDecl*> RecFields,
                               unsigned int BytePos, bool ForStrongLayout,
                               bool &HasUnion);

      Qualifiers::ObjCLifetime getBlockCaptureLifetime(QualType QT, bool ByrefLayout);

      void UpdateRunSkipBlockVars(bool IsByref,
                                  Qualifiers::ObjCLifetime LifeTime,
                                  CharUnits FieldOffset,
                                  CharUnits FieldSize);

      void BuildRCBlockVarRecordLayout(const RecordType *RT,
                                       CharUnits BytePos, bool &HasUnion,
                                       bool ByrefLayout=false);

      void BuildRCRecordLayout(const llvm::StructLayout *RecLayout,
                               const RecordDecl *RD,
                               ArrayRef<const FieldDecl*> RecFields,
                               CharUnits BytePos, bool &HasUnion,
                               bool ByrefLayout);

      uint64_t InlineLayoutInstruction(SmallVectorImpl<unsigned char> &Layout);

      llvm::Constant *getBitmapBlockLayout(bool ComputeByrefLayout);


      /// GetIvarLayoutName - Returns a unique constant for the given
      /// ivar layout bitmap.
      llvm::Constant *GetIvarLayoutName(IdentifierInfo *Ident,
                                        const ObjCCommonTypesHelper &ObjCTypes);

      void  SetPropertyInfoToEmit( const ObjCPropertyDecl *PD,
                                   const Decl *Container,
                                   llvm::Constant *Prop[ 6]);

      /// EmitPropertyList - Emit the given property list. The return
      /// value has type PropertyListPtrTy.
      llvm::Constant *EmitPropertyList(Twine Name,
                                       const Decl *Container,
                                       const ObjCContainerDecl *OCD,
                                       const ObjCCommonTypesHelper &ObjCTypes);

      /// EmitProtocolMethodTypes - Generate the array of extended method type
      /// strings. The return value has type Int8PtrPtrTy.
      llvm::Constant *EmitProtocolMethodTypes(Twine Name,
                                              ArrayRef<llvm::Constant*> MethodTypes,
                                              const ObjCCommonTypesHelper &ObjCTypes);

      /// PushProtocolProperties - Push protocol's property on the input stack.
      void PushProtocolProperties(
                                  llvm::SmallPtrSet<const IdentifierInfo*, 16> &PropertySet,
                                  SmallVectorImpl<llvm::Constant*> &Properties,
                                  const Decl *Container,
                                  const ObjCProtocolDecl *Proto,
                                  const ObjCCommonTypesHelper &ObjCTypes);

      /// GetProtocolRef - Return a reference to the internal protocol
      /// description, creating an empty one if it has not been
      /// defined. The return value has type ProtocolPtrTy.
      llvm::Constant *GetProtocolRef(const ObjCProtocolDecl *PD);

      // common helper function, turning names into abbreviated hashes
      uint64_t          UniqueidHashForString( std::string s, uint64_t first_valid, unsigned WordSizeInBytes);
      llvm::ConstantInt *__HashConstantForString( std::string s, uint64_t first_valid);
      llvm::ConstantInt *_HashConstantForString( std::string s, uint64_t first_valid);
      llvm::ConstantInt *HashClassConstantForString( std::string s)
      {
         return( _HashConstantForString( s, 0x1));
      }
      llvm::ConstantInt *HashProtocolConstantForString( std::string s)
      {
         return( HashClassConstantForString( s));
      }
      llvm::ConstantInt *HashCategoryConstantForString( std::string s)
      {
         return( _HashConstantForString( s, 0x1));
      }
      llvm::ConstantInt *HashPropertyConstantForString( std::string s)
      {
         return( _HashConstantForString( s, 0x1));
      }
      llvm::ConstantInt *HashSelConstantForString( std::string s)
      {
         return( _HashConstantForString( s, 0x1));
      }
      llvm::ConstantInt *HashIvarConstantForString( std::string s)
      {
         return( _HashConstantForString( s, 0x0)); // here we don't care
      }

      /// CreateMetadataVar - Create a global variable with internal
      /// linkage for use by the Objective-C runtime.
      ///
      /// This is a convenience wrapper which not only creates the
      /// variable, but also sets the section and alignment and adds the
      /// global to the "llvm.used" list.
      ///
      /// \param Name - The variable name.
      /// \param Init - The variable initializer; this is also used to
      /// define the type of the variable.
      /// \param Section - The section the variable should go into, or empty.
      /// \param Align - The alignment for the variable, or 0.
      /// \param AddToUsed - Whether the variable should be added to
      /// "llvm.used".
      llvm::GlobalVariable *CreateMetadataVar(Twine Name, llvm::Constant *Init,
                                              StringRef Section, unsigned Align,
                                              bool AddToUsed);

      /// EmitImageInfo - Emit the image info marker used to encode some module
      /// level information.
      void EmitImageInfo();

   public:
      CGObjCCommonMulleRuntime(CodeGen::CodeGenModule &cgm) :
      CGObjCRuntime(cgm), VMContext(cgm.getLLVMContext()) { }

      virtual llvm::ConstantStruct *CreateNSConstantStringStruct( StringRef S, unsigned StringLength) = 0;
      llvm::StructType *GetOrCreateNSConstantStringType( void);
      llvm::StructType *CreateNSConstantStringType( void);

      llvm::Function *GenerateMethod(const ObjCMethodDecl *OMD,
                                     const ObjCContainerDecl *CD=nullptr) override;

      virtual void GenerateProtocol(const ObjCProtocolDecl *PD) override;

      /// GetOrEmitProtocol - Get the protocol object for the given
      /// declaration, emitting it if necessary. The return value has type
      /// ProtocolPtrTy.
      virtual llvm::Constant *GetOrEmitProtocol(const ObjCProtocolDecl *PD)=0;

      /// GetOrEmitProtocolRef - Get a forward reference to the protocol
      /// object for the given declaration, emitting it if needed. These
      /// forward references will be filled in with empty bodies if no
      /// definition is seen. The return value has type ProtocolPtrTy.
      virtual llvm::Constant *GetOrEmitProtocolRef(const ObjCProtocolDecl *PD)=0;
      llvm::Constant *BuildGCBlockLayout(CodeGen::CodeGenModule &CGM,
                                         const CGBlockInfo &blockInfo) override;
      llvm::Constant *BuildRCBlockLayout(CodeGen::CodeGenModule &CGM,
                                         const CGBlockInfo &blockInfo) override;

      llvm::Constant *BuildByrefLayout(CodeGen::CodeGenModule &CGM,
                                       QualType T) override;
   };



   /*
    *
    *
    *
    */
   class CGObjCMulleRuntime : public CGObjCCommonMulleRuntime {
   private:
      ObjCTypesHelper ObjCTypes;

      /// EmitModuleInfo - Another marker encoding module level
      /// information.
      void EmitModuleInfo();

      /// EmitModuleSymols - Emit module symbols, the list of defined
      /// classes and categories. The result has type SymtabPtrTy.
      llvm::Constant *EmitModuleSymbols();

      /// FinishModule - Write out global data structures at the end of
      /// processing a translation unit.
      void FinishModule();

      /// EmitClassExtension - Generate the class extension structure used
      /// to store the weak ivar layout and properties. The return value
      /// has type ClassExtensionPtrTy.
      llvm::Constant *EmitClassExtension(const ObjCImplementationDecl *ID);

      /// EmitClassRef - Return a Value*, of type ObjCTypes.ClassPtrTy,
      /// for the given class.
      llvm::Constant *EmitClassRef(CodeGenFunction &CGF,
                                const ObjCInterfaceDecl *ID);

      llvm::Constant *EmitClassRefFromId(CodeGenFunction &CGF,
                                      IdentifierInfo *II);

      llvm::Value *EmitNSAutoreleasePoolClassRef(CodeGenFunction &CGF) override;


      /// EmitIvarList - Emit the ivar list for the given
      /// implementation. If ForClass is true the list of class ivars
      /// (i.e. metaclass ivars) is emitted, otherwise the list of
      /// interface ivars will be emitted. The return value has type
      /// IvarListPtrTy.
      llvm::Constant *EmitIvarList(const ObjCImplementationDecl *ID,
                                   ArrayRef<llvm::Constant*> Ivars,
                                   bool ForClass);

      llvm::Constant *GetIvarConstant( const ObjCInterfaceDecl *OID,
                                       const ObjCIvarDecl *IVD);

      llvm::Constant *GetMethodConstant(const ObjCMethodDecl *MD);

      llvm::Constant *GetMethodDescriptionConstant(const ObjCMethodDecl *MD);

      /// EmitMethodList - Emit the method list for the given
      /// implementation. The return value has type MethodListPtrTy.
      llvm::Constant *EmitMethodList(Twine Name,
                                     const char *Section,
                                     ArrayRef<llvm::Constant*> Methods);

      /// EmitMethodDescList - Emit a method description list for a list of
      /// method declarations.
      ///  - TypeName: The name for the type containing the methods.
      ///  - IsProtocol: True iff these methods are for a protocol.
      ///  - ClassMethds: True iff these are class methods.
      ///  - Required: When true, only "required" methods are
      ///    listed. Similarly, when false only "optional" methods are
      ///    listed. For classes this should always be true.
      ///  - begin, end: The method list to output.
      ///
      /// The return value has type MethodDescriptionListPtrTy.
      llvm::Constant *EmitMethodDescList(Twine Name,
                                         const char *Section,
                                         ArrayRef<llvm::Constant*> Methods);

      /// GetOrEmitProtocol - Get the protocol object for the given
      /// declaration, emitting it if necessary. The return value has type
      /// ProtocolxTy.
      llvm::Constant *GetOrEmitProtocol(const ObjCProtocolDecl *PD) override;

      /// GetOrEmitProtocolRef - Get a forward reference to the protocol
      /// object for the given declaration, emitting it if needed. These
      /// forward references will be filled in with empty bodies if no
      /// definition is seen. The return value has type ProtocolPtrTy.
      llvm::Constant *GetOrEmitProtocolRef(const ObjCProtocolDecl *PD) override;


      /// EmitProtocolIDList - Generate the list of referenced
      /// protocols. The return value has type ProtocolListPtrTy.
      llvm::Constant *EmitProtocolIDList(Twine Name,
                                         ObjCProtocolDecl::protocol_iterator begin,
                                         ObjCProtocolDecl::protocol_iterator end);
      /// EmitProtocolClassIDList - Generate the list of referenced
      /// protocol classes. The return value has type
      llvm::Constant *EmitProtocolClassIDList(Twine Name,
                                         ObjCProtocolDecl::protocol_iterator begin,
                                         ObjCProtocolDecl::protocol_iterator end);


      /// EmitSelector - Return a Value*, of type SelectorIDTy,
      /// for the given selector.
      llvm::Constant *EmitClassID(CodeGenFunction &CGF, const ObjCInterfaceDecl *Class);

      llvm::Constant *EmitSelector(CodeGenFunction &CGF, Selector Sel,
                                bool lval=false);

      llvm::Constant *EmitClassList(Twine Name,
                                    const char *Section,
                                    ArrayRef<llvm::Constant*> Classes);
      llvm::Constant *EmitCategoryList(Twine Name,
                                       const char *Section,
                                       ArrayRef<llvm::Constant*> Categories);
      llvm::Constant *EmitStaticStringList(Twine Name,
                                           const char *Section,
                                           ArrayRef<llvm::Constant*> StaticStrings);
      llvm::Constant *EmitHashNameList(Twine Name,
                                       const char *Section,
                                       ArrayRef<llvm::Constant*> HashNames);

      llvm::Constant *EmitLoadInfoList(Twine Name,
                                          const char *Section,
                                          llvm::Constant *ClassList,
                                          llvm::Constant *CategoryList,
                                          llvm::Constant *StringList,
                                          llvm::Constant *HashNameList);


   public:
      CGObjCMulleRuntime(CodeGen::CodeGenModule &cgm);

      llvm::Function *ModuleInitFunction() override;

      void  ParserDidFinish( clang::Parser *P) override;

      bool  GetMacroDefinitionUnsignedIntegerValue( clang::Preprocessor *PP,
                                                    StringRef name,
                                                    uint64_t *value);

      const CGFunctionInfo   &GenerateFunctionInfo( QualType arg0Ty,
                                                    QualType rvalTy);

      CodeGen::RValue CommonFunctionCall(CodeGen::CodeGenFunction &CGF,
                                         llvm::Constant *Fn,
                                         ReturnValueSlot Return,
                                         QualType ResultType,
                                         QualType FnResultType,
                                         const CGFunctionInfo &FI,
                                         llvm::Value *Receiver,
                                         const CallArgList &CallArgs,
                                         CallArgList   &ActualArgs,
                                         llvm::Value   *Arg0,
                                         const ObjCMethodDecl *Method);

      CodeGen::RValue CommonMessageSend(CodeGen::CodeGenFunction &CGF,
                                        llvm::Constant *Fn,
                                        ReturnValueSlot Return,
                                        QualType ResultType,
                                        llvm::Value *Receiver,
                                        const CallArgList &CallArgs,
                                        CallArgList &ActualArgs,
                                        llvm::Value   *Arg0,
                                        const ObjCMethodDecl *Method,
                                        bool isDispatchFn);

      CodeGen::RValue GenerateMessageSend(CodeGen::CodeGenFunction &CGF,
                                          ReturnValueSlot Return,
                                          QualType ResultType,
                                          Selector Sel, llvm::Value *Receiver,
                                          const CallArgList &CallArgs,
                                          const ObjCInterfaceDecl *Class,
                                          const ObjCMethodDecl *Method) override;

      CodeGen::RValue
      GenerateMessageSendSuper(CodeGen::CodeGenFunction &CGF,
                               ReturnValueSlot Return, QualType ResultType,
                               Selector Sel, const ObjCInterfaceDecl *Class,
                               bool isCategoryImpl, llvm::Value *Receiver,
                               bool IsClassMessage, const CallArgList &CallArgs,
                               const ObjCMethodDecl *Method) override;


      CodeGen::RValue
      EmitFastEnumeratorCall( CodeGen::CodeGenFunction &CGF,
                              ReturnValueSlot ReturnSlot,
                              QualType ResultType,
                              Selector Sel,
                              llvm::Value *Receiver,
                              llvm::Value *StatePtr,
                              QualType StateTy,
                              llvm::Value *ItemsPtr,
                              QualType ItemsTy,
                              llvm::Value *Count,
                              QualType CountTy) override;

      llvm::Value *GetClass(CodeGenFunction &CGF,
                            const ObjCInterfaceDecl *ID) override;
      llvm::Value *GetClass(CodeGenFunction &CGF,
                            llvm::Value  *classID);
      llvm::Value *GetClass(CodeGenFunction &CGF,
                            StringRef className);

      llvm::Constant *GetSelector(CodeGenFunction &CGF, Selector Sel) override;

      /// The NeXT/Apple runtimes do not support typed selectors; just emit an
      /// untyped one.
      llvm::Constant *GetSelector(CodeGenFunction &CGF,
                               const ObjCMethodDecl *Method) override;
      Address GetAddrOfSelector(CodeGenFunction &CGF, Selector Sel) override;

      llvm::Constant *GetEHType(QualType T) override;

      void GenerateCategory(const ObjCCategoryImplDecl *CMD) override;

      void GenerateClass(const ObjCImplementationDecl *ClassDecl) override;

      void GenerateForwardClass(const ObjCInterfaceDecl *OID) override;

      void RegisterAlias(const ObjCCompatibleAliasDecl *OAD) override {}

      RecordDecl  *CreateMetaABIRecordDecl( Selector sel,
                                            ArrayRef<QualType> Types);

      RecordDecl  *CreateOnTheFlyRecordDecl( const ObjCMessageExpr *Expr);
      RecordDecl  *CreateVariadicOnTheFlyRecordDecl( Selector Sel,
                                                     RecordDecl *RD,
                                                     llvm::ArrayRef<const Expr*> &Exprs);

      bool    OptimizeReuseParam( CodeGenFunction &CGF,
                                  CallArgList &Args,
                                  const ObjCMessageExpr *Expr,
                                  RecordDecl   *RD,
                                  Address  RecordAddress,
                                  CGObjCRuntimeLifetimeMarker &Marker);

      LValue  GenerateMetaABIRecordAlloca( CodeGenFunction &CGF,
                                           RecordDecl   *RD,
                                           RecordDecl   *RV,
                                           CGObjCRuntimeLifetimeMarker &Marker);

      void  PushArgumentsIntoRecord( CodeGenFunction &CGF,
                                     RecordDecl *RD,
                                     LValue Record,
                                     const ObjCMessageExpr *expr);

      void   EmitVoidPtrExpression( CodeGenFunction &CGF,
                                    CallArgList &Args,
                                    const Expr  *Arg);

      void  PushCallArgsIntoRecord( CodeGenFunction &CGF,
                                    RecordDecl *RD,
                                    LValue Record,
                                    CallArgList &Arg);

      CGObjCRuntimeLifetimeMarker   ConvertToMetaABIArgsIfNeeded( CodeGenFunction &CGF,
                                                                  const ObjCMethodDecl *method,
                                                                  CallArgList &Args) override;

      CGObjCRuntimeLifetimeMarker   GenerateCallArgs( CodeGenFunction &CGF,
                                                      CallArgList &Args,
                                                      const ObjCMethodDecl *method,
                                                      const ObjCMessageExpr *expr) override;
      QualType   CreateVoid5PtrTy( void);
      LValue     GenerateAllocaedUnion( CodeGenFunction &CGF,
                                        RecordDecl *UD);

      RecordDecl   *CreateMetaABIUnionDecl( CodeGenFunction &CGF,
                                            RecordDecl  *RD,
                                            RecordDecl  *RV);

      RecordDecl   *CreateMetaABIUnionDecl( CodeGenFunction &CGF,
                                            QualType recTy,
                                            uint64_t recSize,
                                            QualType  rvalTy,
                                            bool hasRvalTy);

      llvm::Value *GenerateProtocolRef(CodeGenFunction &CGF,
                                       const ObjCProtocolDecl *PD) override;

      llvm::Constant *GetPropertyGetFunction() override;
      llvm::Constant *GetPropertySetFunction() override;
      llvm::Constant *GetOptimizedPropertySetFunction(bool atomic,
                                                      bool copy) override;
      llvm::Constant *GetGetStructFunction() override;
      llvm::Constant *GetSetStructFunction() override;
      llvm::Constant *GetCppAtomicObjectGetFunction() override;
      llvm::Constant *GetCppAtomicObjectSetFunction() override;
      llvm::Constant *EnumerationMutationFunction() override;

      void EmitTryStmt(CodeGen::CodeGenFunction &CGF,
                       const ObjCAtTryStmt &S) override;
      void EmitSynchronizedStmt(CodeGen::CodeGenFunction &CGF,
                                const ObjCAtSynchronizedStmt &S) override;
      void EmitTryOrSynchronizedStmt(CodeGen::CodeGenFunction &CGF, const Stmt &S);
      void EmitThrowStmt(CodeGen::CodeGenFunction &CGF, const ObjCAtThrowStmt &S,
                         bool ClearInsertionPoint=true) override;
      llvm::Value * EmitObjCWeakRead(CodeGen::CodeGenFunction &CGF,
                                     Address AddrWeakObj) override;
      void EmitObjCWeakAssign(CodeGen::CodeGenFunction &CGF,
                              llvm::Value *src, Address dst) override;
      void EmitObjCGlobalAssign(CodeGen::CodeGenFunction &CGF,
                                llvm::Value *src, Address dest,
                                bool threadlocal = false) override;
      void EmitObjCIvarAssign(CodeGen::CodeGenFunction &CGF,
                              llvm::Value *src, Address dest,
                              llvm::Value *ivarOffset) override;
      void EmitObjCStrongCastAssign(CodeGen::CodeGenFunction &CGF,
                                    llvm::Value *src, Address dest) override;
      void EmitGCMemmoveCollectable(CodeGen::CodeGenFunction &CGF,
                                    Address dest, Address src,
                                    llvm::Value *size) override;

      LValue EmitObjCValueForIvar(CodeGen::CodeGenFunction &CGF, QualType ObjectTy,
                                  llvm::Value *BaseValue, const ObjCIvarDecl *Ivar,
                                  unsigned CVRQualifiers) override;
      llvm::Value *EmitIvarOffset(CodeGen::CodeGenFunction &CGF,
                                  const ObjCInterfaceDecl *Interface,
                                  const ObjCIvarDecl *Ivar) override;


      llvm::ConstantStruct *CreateNSConstantStringStruct( StringRef S, unsigned StringLength) override;
      /// GetClassGlobal - Return the global variable for the Objective-C
      /// class of the given name.
      llvm::GlobalVariable *GetClassGlobal(StringRef Name,
                                           bool Weak = false) override {
         llvm_unreachable("CGObjCMulleRuntime::GetClassGlobal");
      }
   };
}

/// A helper class for performing the null-initialization of a return
/// value.
struct NullReturnState {
   llvm::BasicBlock *NullBB;
   NullReturnState() : NullBB(nullptr) {}

   /// Perform a null-check of the given receiver.
   void init(CodeGenFunction &CGF, llvm::Value *receiver) {
      // Make blocks for the null-receiver and call edges.
      NullBB = CGF.createBasicBlock("msgSend.null-receiver");
      llvm::BasicBlock *callBB = CGF.createBasicBlock("msgSend.call");

      // Check for a null receiver and, if there is one, jump to the
      // null-receiver block.  There's no point in trying to avoid it:
      // we're always going to put *something* there, because otherwise
      // we shouldn't have done this null-check in the first place.
      llvm::Value *isNull = CGF.Builder.CreateIsNull(receiver);
      CGF.Builder.CreateCondBr(isNull, NullBB, callBB);

      // Otherwise, start performing the call.
      CGF.EmitBlock(callBB);
   }

   /// Complete the null-return operation.  It is valid to call this
   /// regardless of whether 'init' has been called.
   RValue complete(CodeGenFunction &CGF, RValue result, QualType resultType,
                   const CallArgList &CallArgs,
                   const ObjCMethodDecl *Method) {
      // If we never had to do a null-check, just use the raw result.
      if (!NullBB) return result;

      // The continuation block.  This will be left null if we don't have an
      // IP, which can happen if the method we're calling is marked noreturn.
      llvm::BasicBlock *contBB = nullptr;

      // Finish the call path.
      llvm::BasicBlock *callBB = CGF.Builder.GetInsertBlock();
      if (callBB) {
         contBB = CGF.createBasicBlock("msgSend.cont");
         CGF.Builder.CreateBr(contBB);
      }

      // Okay, start emitting the null-receiver block.
      CGF.EmitBlock(NullBB);

      // Release any consumed arguments we've got.
      if (Method) {
         CallArgList::const_iterator I = CallArgs.begin();
         for (ObjCMethodDecl::param_const_iterator i = Method->param_begin(),
              e = Method->param_end(); i != e; ++i, ++I) {
            const ParmVarDecl *ParamDecl = (*i);
            if (ParamDecl->hasAttr<NSConsumedAttr>()) {
               RValue RV = I->RV;
               assert(RV.isScalar() &&
                      "NullReturnState::complete - arg not on object");
               CGF.EmitARCRelease(RV.getScalarVal(), ARCImpreciseLifetime);
            }
         }
      }

      // The phi code below assumes that we haven't needed any control flow yet.
      assert(CGF.Builder.GetInsertBlock() == NullBB);

      // If we've got a void return, just jump to the continuation block.
      if (result.isScalar() && resultType->isVoidType()) {
         // No jumps required if the message-send was noreturn.
         if (contBB) CGF.EmitBlock(contBB);
         return result;
      }

      // If we've got a scalar return, build a phi.
      if (result.isScalar()) {
         // Derive the null-initialization value.
         llvm::Constant *null = CGF.CGM.EmitNullConstant(resultType);

         // If no join is necessary, just flow out.
         if (!contBB) return RValue::get(null);

         // Otherwise, build a phi.
         CGF.EmitBlock(contBB);
         llvm::PHINode *phi = CGF.Builder.CreatePHI(null->getType(), 2);
         phi->addIncoming(result.getScalarVal(), callBB);
         phi->addIncoming(null, NullBB);
         return RValue::get(phi);
      }

      // If we've got an aggregate return, null the buffer out.
      // FIXME: maybe we should be doing things differently for all the
      // cases where the ABI has us returning (1) non-agg values in
      // memory or (2) agg values in registers.
      if (result.isAggregate()) {
         assert(result.isAggregate() && "null init of non-aggregate result?");
         CGF.EmitNullInitialization(result.getAggregateAddress(), resultType);
         if (contBB) CGF.EmitBlock(contBB);
         return result;
      }

      // Complex types.
      CGF.EmitBlock(contBB);
      CodeGenFunction::ComplexPairTy callResult = result.getComplexVal();

      // Find the scalar type and its zero value.
      llvm::Type *scalarTy = callResult.first->getType();
      llvm::Constant *scalarZero = llvm::Constant::getNullValue(scalarTy);

      // Build phis for both coordinates.
      llvm::PHINode *real = CGF.Builder.CreatePHI(scalarTy, 2);
      real->addIncoming(callResult.first, callBB);
      real->addIncoming(scalarZero, NullBB);
      llvm::PHINode *imag = CGF.Builder.CreatePHI(scalarTy, 2);
      imag->addIncoming(callResult.second, callBB);
      imag->addIncoming(scalarZero, NullBB);
      return RValue::getComplex(real, imag);
   }
};


/* *** Helper Functions *** */

/// getConstantGEP() - Help routine to construct simple GEPs.
/// TODO: (nat) why not inherit ?
static llvm::Constant *getConstantGEP(llvm::LLVMContext &VMContext,
                                      llvm::GlobalVariable *C, unsigned idx0,
                                      unsigned idx1) {
  llvm::Value *Idxs[] = {
    llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext), idx0),
    llvm::ConstantInt::get(llvm::Type::getInt32Ty(VMContext), idx1)
  };
  return llvm::ConstantExpr::getGetElementPtr(C->getValueType(), C, Idxs);
}


/* *** CGObjCMulleRuntime Public Interface *** */

CGObjCMulleRuntime::CGObjCMulleRuntime(CodeGen::CodeGenModule &cgm) : CGObjCCommonMulleRuntime(cgm),
ObjCTypes(cgm) {
   ObjCABI = 1;

   load_version         = 0;
   foundation_version   = 0;
   runtime_version      = 0;      // MUST be set by header, if we emit loadinfo
   user_version         = 0;
   thread_local_runtime = CGM.getLangOpts().ObjCHasThreadLocalRuntime;
   no_tagged_pointers   = CGM.getLangOpts().ObjCDisableTaggedPointers;

   memset( fastclassids, 0, sizeof( fastclassids));
   fastclassids_defined = false;
   _trace_fastids = getenv( "MULLE_CLANG_TRACE_FASTCLASS") ? 1 : 0;  // need compiler flag

   NSConstantStringType = nullptr;

   EmitImageInfo();
}


bool  CGObjCMulleRuntime::GetMacroDefinitionUnsignedIntegerValue( clang::Preprocessor *PP,
                                                                  StringRef name,
                                                                  uint64_t *value)
{
   IdentifierInfo    *identifier;
   MacroDefinition   definition;
   MacroInfo         *info;
   const Token       *token;
   SmallString<128>  SpellingBuffer;
   llvm::APInt       tmp(64, 0);

   identifier = &CGM.getContext().Idents.get( name);
   definition = PP->getMacroDefinition( identifier);
   if( ! definition)
      return( 0);

   // default: 1 if just defined
   info = definition.getMacroInfo();
   if( ! info->getNumTokens())
   {
      *value = 1;
      return( 1);
   }

   token = &info->getReplacementToken( 0);
   if( token->getKind() != tok::numeric_constant)
   {
      CGM.getDiags().Report( diag::err_mulle_objc_preprocessor_not_integer_value);
      return( 0);
   }

   // How can this be even possibly fail ? It's impossible. More is more!
   bool Invalid = false;
   StringRef TokSpelling = PP->getSpelling( *token, SpellingBuffer, &Invalid);
   if (Invalid)
      return( 0);

   NumericLiteralParser  Literal( TokSpelling, token->getLocation(), *PP);
   if( Literal.hadError)
      return( 0);

   Literal.GetIntegerValue( tmp);
   *value = tmp.getZExtValue();
   return( 1);
}


void   CGObjCMulleRuntime::ParserDidFinish( clang::Parser *P)
{
   clang::Preprocessor  *PP;
   uint64_t  major;
   uint64_t  minor;
   uint64_t  patch;
   uint64_t  value;

   // it's cool if versions aren't defined, when just compiling C stuff
   PP = &P->getPreprocessor();
   if( ! runtime_version)
   {
      if( GetMacroDefinitionUnsignedIntegerValue( PP, "MULLE_OBJC_RUNTIME_VERSION_MAJOR", &major) &&
          GetMacroDefinitionUnsignedIntegerValue( PP, "MULLE_OBJC_RUNTIME_VERSION_MINOR", &minor) &&
          GetMacroDefinitionUnsignedIntegerValue( PP, "MULLE_OBJC_RUNTIME_VERSION_PATCH", &patch))
      {
         runtime_version = (major << 20) | (minor << 8) | patch;
         // fprintf( stderr, "%d.%d.%d -> 0x%x\n", (int) major, (int)  minor, (int) patch, (int)  runtime_version);
      }
   }

   if( ! load_version)
   {
      if( GetMacroDefinitionUnsignedIntegerValue( PP, "MULLE_OBJC_RUNTIME_LOAD_VERSION", &value))
      {
         load_version = value;
         // fprintf( stderr, "load_version -> 0x%x\n", (int)  load_version);
      }
   }
   
   // optional anyway
   if( ! foundation_version)
   {
      if( GetMacroDefinitionUnsignedIntegerValue( PP, "FOUNDATION_VERSION_MAJOR", &major) &&
          GetMacroDefinitionUnsignedIntegerValue( PP, "FOUNDATION_VERSION_MINOR", &minor) &&
          GetMacroDefinitionUnsignedIntegerValue( PP, "FOUNDATION_VERSION_PATCH", &patch))
      {
         foundation_version = (major << 20) | (minor << 8) | patch;
      }
   }

   // optional anyway
   if( ! user_version)
   {
      if( GetMacroDefinitionUnsignedIntegerValue( PP, "USER_VERSION_MAJOR", &major) &&
          GetMacroDefinitionUnsignedIntegerValue( PP, "USER_VERSION_MINOR", &minor) &&
          GetMacroDefinitionUnsignedIntegerValue( PP, "USER_VERSION_PATCH", &patch))
      {
         user_version = (major << 20) | (minor << 8) | patch;
      }
   }


   // possibly make this a #pragma sometime
   if( GetMacroDefinitionUnsignedIntegerValue( PP, "__MULLE_OBJC_TPS__", &value))
   {
      no_tagged_pointers = ! value;
   }

   if( GetMacroDefinitionUnsignedIntegerValue( PP, "__MULLE_OBJC_NO_TPS__", &value))
   {
      no_tagged_pointers = value;
   }

   // possibly make this a #pragma sometime
   if( GetMacroDefinitionUnsignedIntegerValue( PP, "__MULLE_OBJC_TRT__", &value))
   {
      thread_local_runtime = value;
   }

   if( GetMacroDefinitionUnsignedIntegerValue( PP, "__MULLE_OBJC_NO_TRT__", &value))
   {
      thread_local_runtime = ! value;
   }

   /* this runs for every top level declaration.(layme)
      Fastclass must always be compiled regardless of optimization level
    */
   if( ! fastclassids_defined)
   {
      char   buf[ 64];

      for( int i = 0; i < 32; i++)
      {
         // these are always 64 bit regardless if compiling to 32 bit
         sprintf( buf, "MULLE_OBJC_FASTCLASSHASH_%d", i);
         if( GetMacroDefinitionUnsignedIntegerValue( PP, buf, &value))
         {
            fastclassids[ i]     = value; // >> 32;
            fastclassids_defined = true;

            if( _trace_fastids)
               fprintf( stderr, "fastclassid #%d = 0x%llx\n", i, (long long) fastclassids[ i]);
         }
      }
   }
}


/// GetClass - Return a reference to the class for the given interface
/// decl.
llvm::Value *CGObjCMulleRuntime::GetClass(CodeGenFunction &CGF,
                                          llvm::Value  *classID)
{
   llvm::Value *rval;
   llvm::Value *classPtr;

   int optLevel = CGM.getLangOpts().OptimizeSize ? -1 : CGM.getCodeGenOpts().OptimizationLevel;
   classPtr = CGF.EmitNounwindRuntimeCall(ObjCTypes.getGetRuntimeClassFn( optLevel),
                                          classID, "mulle_objc_unfailing_get_or_lookup_class"); // string what for ??
   rval     = CGF.Builder.CreateBitCast( classPtr, ObjCTypes.ObjectPtrTy);
   return rval;
}

/// GetClass - Return a reference to the class for the given interface
/// decl.
llvm::Value *CGObjCMulleRuntime::GetClass(CodeGenFunction &CGF,
                                          StringRef className)
{
   llvm::Value  *classID;

   classID = HashClassConstantForString( className);
   return( GetClass( CGF, classID));
}


/// GetClass - Return a reference to the class for the given interface
/// decl.
llvm::Value *CGObjCMulleRuntime::GetClass(CodeGenFunction &CGF,
                                          const ObjCInterfaceDecl *ID)
{
   llvm::Value  *classID;

   classID  = HashClassConstantForString( ID->getName());
   return( GetClass( CGF, classID));
}

/// GetSelector - Return the pointer to the unique'd string for this selector.
llvm::Constant *CGObjCMulleRuntime::GetSelector(CodeGenFunction &CGF, Selector Sel) {
   return EmitSelector(CGF, Sel, false);
}
llvm::Constant *CGObjCMulleRuntime::GetSelector(CodeGenFunction &CGF, const ObjCMethodDecl
                                    *Method) {
   return EmitSelector(CGF, Method->getSelector());
}

Address CGObjCMulleRuntime::GetAddrOfSelector(CodeGenFunction &CGF, Selector Sel)
{
   llvm::Constant *result;

   result = EmitSelector(CGF, Sel, false);
   return( Address( result, CGM.getPointerAlign()));
}


llvm::Constant *CGObjCMulleRuntime::GetEHType(QualType T) {
   if (T->isObjCIdType() ||
       T->isObjCQualifiedIdType()) {
      return CGM.GetAddrOfRTTIDescriptor(
                                         CGM.getContext().getObjCIdRedefinitionType(), /*ForEH=*/true);
   }
   if (T->isObjCClassType() ||
       T->isObjCQualifiedClassType()) {
      return CGM.GetAddrOfRTTIDescriptor(
                                         CGM.getContext().getObjCClassRedefinitionType(), /*ForEH=*/true);
   }
   if (T->isObjCObjectPointerType())
      return CGM.GetAddrOfRTTIDescriptor(T,  /*ForEH=*/true);

   llvm_unreachable("asking for catch type for ObjC type in fragile runtime");
}

#pragma mark -
#pragma mark ConstantString

///  Generate a constant NSString object.
//
// { INTPTR_MAX, NULL, "VfL Bochum 1848", 15 };
//
llvm::StructType *CGObjCCommonMulleRuntime::CreateNSConstantStringType( void)
{
   ASTContext   &Context = CGM.getContext();

   // Construct the type for a constant NSString.
   RecordDecl *D = Context.buildImplicitRecord("__builtin_NSString");
   D->startDefinition();

   QualType FieldTypes[4];

   // const unsigned long  retainCount;
   FieldTypes[0] = Context.UnsignedLongTy;
   // const void *isa;
   FieldTypes[1] = Context.VoidPtrTy;

   // unsigned char *str;
   FieldTypes[2] = Context.getPointerType(Context.CharTy.withConst());
   // unsigned int length;
   FieldTypes[3] = Context.UnsignedIntTy;

   // Create fields
   for (unsigned i = 0; i < 4; ++i) {
      FieldDecl *Field = FieldDecl::Create(Context, D,
                                           SourceLocation(),
                                           SourceLocation(), nullptr,
                                           FieldTypes[i], /*TInfo=*/nullptr,
                                           /*BitWidth=*/nullptr,
                                           i == 1,
                                           ICIS_NoInit);
      Field->setAccess(AS_public);
      D->addDecl(Field);
   }

   D->completeDefinition();

   QualType NSTy = Context.getTagDeclType(D);
   return( cast<llvm::StructType>( CGM.getTypes().ConvertType(NSTy)));
}


/// \brief The LLVM type corresponding to NSConstantString.
llvm::StructType *CGObjCCommonMulleRuntime::GetOrCreateNSConstantStringType( void)
{
   if( ! NSConstantStringType)
      NSConstantStringType = CreateNSConstantStringType();
   return( NSConstantStringType);
}


//
// this is basically CodeGenModule::GetAddrOfConstantString copy/pasted
// and slighlty tweaked. Why this code is in CodeGenModule beats me..
//

llvm::ConstantStruct *CGObjCMulleRuntime::CreateNSConstantStringStruct( StringRef S, unsigned StringLength)
{
   llvm::Constant *Fields[4];

   // drop LONG_MAX
   llvm::Type *Ty = CGM.getTypes().ConvertType(CGM.getContext().LongTy);
   Fields[0] = llvm::ConstantInt::get(Ty, LONG_MAX);

   // this is filled in by the runtime later

   Fields[1] = llvm::Constant::getNullValue( CGM.VoidPtrTy);

   llvm::GlobalValue::LinkageTypes Linkage = llvm::GlobalValue::PrivateLinkage;
   llvm::Constant *C                       = llvm::ConstantDataArray::getString(VMContext, S);

   auto *GV = new llvm::GlobalVariable( CGM.getModule(), C->getType(), false,
                                       Linkage, C, ".str");
   // FIXME: release_39 used to be true, now its Global:
   GV->setUnnamedAddr( llvm::GlobalValue::UnnamedAddr::Global);

   // Don't enforce the target's minimum global alignment, since the only use
   // of the string is via this class initializer.

   CharUnits Align = CGM.getContext().getTypeAlignInChars(CGM.getContext().CharTy);
   GV->setAlignment(Align.getQuantity());
   Fields[2] = getConstantGEP( VMContext, GV, 0, 0);

   // String length.
   Ty = CGM.getTypes().ConvertType(CGM.getContext().UnsignedIntTy);
   Fields[3] = llvm::ConstantInt::get(Ty, StringLength);

   llvm::StructType *StructType = GetOrCreateNSConstantStringType();
   return( (llvm::ConstantStruct *) llvm::ConstantStruct::get( StructType, Fields));
}


static llvm::StringMapEntry<llvm::GlobalAlias *> &
GetConstantStringEntry(llvm::StringMap<llvm::GlobalAlias *> &Map,
                       const StringLiteral *Literal, unsigned &StringLength)
{
  StringRef String = Literal->getString();
  StringLength = String.size();
  return *Map.insert(std::make_pair(String, nullptr)).first;
}


llvm::StringMapEntry<llvm::GlobalAlias *> &
CGObjCCommonMulleRuntime::GetNSConstantStringMapEntry( const StringLiteral *Literal, unsigned &StringLength) {
   return( GetConstantStringEntry( NSConstantStringMap, Literal, StringLength));
}

#pragma mark -
#pragma mark mulle_char5

static int   mulle_char5_encode_character( int c)
{
   switch( c)
   {
   case  0  : return( 0);
   case '.' : return( 1);
   case 'A' : return( 2);
   case 'C' : return( 3);
   case 'D' : return( 4);
   case 'E' : return( 5);
   case 'I' : return( 6);
   case 'N' : return( 7);

   case 'O' : return( 8);
   case 'P' : return( 9);
   case 'S' : return( 10);
   case 'T' : return( 11);
   case '_' : return( 12);
   case 'a' : return( 13);
   case 'b' : return( 14);
   case 'c' : return( 15);

   case 'd' : return( 16);
   case 'e' : return( 17);
   case 'f' : return( 18);
   case 'g' : return( 19);
   case 'h' : return( 20);
   case 'i' : return( 21);
   case 'l' : return( 22);
   case 'm' : return( 23);

   case 'n' : return( 24);
   case 'o' : return( 25);
   case 'p' : return( 26);
   case 'r' : return( 27);
   case 's' : return( 28);
   case 't' : return( 29);
   case 'u' : return( 30);
   case 'y' : return( 31);
   }
   return( -1);
}


static int   mulle_char5_is32bit( char *src, size_t len)
{
   char   *sentinel;

   if( len > 6)
      return( 0);

   sentinel = &src[ len];
   while( src < sentinel)
      switch( mulle_char5_encode_character( *src++))
      {
      case 0  : return( 1);   // zero byte, ok fine!
      case -1 : return( 0);   // invalid char
      }

   return( 1);
}


static int   mulle_char5_is64bit( char *src, size_t len)
{
   char   *sentinel;

   if( len > 12)
      return( 0);

   sentinel = &src[ len];
   while( src < sentinel)
      switch( mulle_char5_encode_character( *src++))
      {
      case 0  : return( 1);
      case -1 : return( 0);
      }

   return( 1);
}


uint32_t  mulle_char5_encode32_ascii( char *src, size_t len)
{
   char       *s;
   char       *sentinel;
   char       c;
   int        char5;
   uint32_t   value;

   value    = 0;
   sentinel = src;
   s        = &src[ len];
   while( s > sentinel)
   {
      c = *--s;
      if( ! c)
         continue;

      char5 = mulle_char5_encode_character( c);
      assert( char5 > 0 && char5 < 0x20);
      assert( value << 5 >> 5 == value);  // hope the optimizer doesn't fck up
      value <<= 5;
      value  |= char5;
   }
   return( value);
}


uint64_t  mulle_char5_encode64_ascii( char *src, size_t len)
{
   char       *s;
   char       *sentinel;
   char       c;
   int        char5;
   uint64_t   value;

   value    = 0;
   sentinel = src;
   s        = &src[ len];
   while( s > sentinel)
   {
      c = *--s;
      if( ! c)
         continue;

      char5 = mulle_char5_encode_character( c);
      assert( char5 > 0 && char5 < 0x20);
      assert( value << 5 >> 5 == value);  // hope the optimizer doesn't fck up
      value <<= 5;
      value  |= char5;
   }
   return( value);
}


int   mulle_char7_is32bit( char *src, size_t len)
{
   char   *sentinel;

   if( len > 4)
      return( 0);

   sentinel = &src[ len];
   while( src < sentinel)
      if( *src++ & 0x80)
         return( 0);   // invalid char

   return( 1);
}


int   mulle_char7_is64bit( char *src, size_t len)
{
   char   *sentinel;

   if( len > 8)
      return( 0);

   sentinel = &src[ len];
   while( src < sentinel)
      if( *src++ & 0x80)
         return( 0);   // invalid char

   return( 1);
}


uint32_t  mulle_char7_encode32_ascii( char *src, size_t len)
{
   char       *s;
   char       *sentinel;
   int        char7;
   uint32_t   value;

   value    = 0;
   sentinel = src;
   s        = &src[ len];
   while( s > sentinel)
   {
      char7 = *--s;
      if( ! char7)
         continue;

      assert( ! (char7 & 0x80));
      value <<= 7;
      value  |= char7;
   }
   return( value);
}


uint64_t  mulle_char7_encode64_ascii( char *src, size_t len)
{
   char       *s;
   char       *sentinel;
   int        char7;
   uint64_t   value;

   value    = 0;
   sentinel = src;
   s        = &src[ len];
   while( s > sentinel)
   {
      char7 = *--s;
      if( ! char7)
         continue;

      assert( ! (char7 & 0x80));
      value <<= 7;
      value  |= char7;
   }
   return( value);
}


ConstantAddress CGObjCCommonMulleRuntime::GenerateConstantString( const StringLiteral *SL)
{
   CharUnits Align = CGM.getPointerAlign();
   StringRef str = SL->getString();
   unsigned StringLength = str.size();

   //
   // create a tagged pointer for strings, if the constant matches
   //
   if( ! no_tagged_pointers && SL->getKind() == StringLiteral::Ascii)
   {
      unsigned WordSizeInBits = CGM.getTarget().getPointerWidth(0);

      if( WordSizeInBits == 32)
      {
         uint32_t   value;

         value = 0;
         if( mulle_char7_is32bit( (char *) str.data(), StringLength))
         {
            value = mulle_char7_encode32_ascii( (char *) str.data(), StringLength);
            value <<= 2;
            value |= 0x3;
         }
         else
            if( mulle_char5_is32bit( (char *) str.data(), StringLength))
            {
               value = mulle_char5_encode32_ascii( (char *) str.data(), StringLength);

               // shift up and tag as string
               value <<= 2;
               value |= 0x1;
            }

         if( value)
         {
            llvm::APInt APValue( 32, value);
            llvm::Constant  *pointerValue = llvm::Constant::getIntegerValue( CGM.Int32Ty, APValue);
            llvm::Constant  *pointer = llvm::ConstantExpr::getIntToPtr( pointerValue, CGM.VoidPtrTy);
            //fprintf( stderr, "Created tagged 32 bit pointer for \"%.*s\"\n", (int) StringLength, (char *) str.data());

            return ConstantAddress( pointer, Align);
         }
      }
      else
      {
         uint64_t   value;

         value = 0;
         if( mulle_char7_is64bit( (char *) str.data(), StringLength))
         {
            value = mulle_char7_encode64_ascii( (char *) str.data(), StringLength);
            value <<= 3;
            value |= 0x3;
         }
         else
            if( mulle_char5_is64bit( (char *) str.data(), StringLength))
            {
               value = mulle_char5_encode64_ascii( (char *) str.data(), StringLength);

               // shift up and tag as string
               value <<= 3;
               value |= 0x1;
            }

         if( value)
         {
            llvm::APInt APValue( 64, value);
            llvm::Constant  *pointerValue = llvm::Constant::getIntegerValue( CGM.Int64Ty, APValue);
            llvm::Constant  *pointer = llvm::ConstantExpr::getIntToPtr( pointerValue, CGM.VoidPtrTy);
            //fprintf( stderr, "Created tagged 64 bit pointer for \"%.*s\"\n", (int) StringLength, (char *) str.data());
            return ConstantAddress( pointer, Align);
         }
      }
   }

   llvm::StringMapEntry<llvm::GlobalAlias *> &Entry = GetNSConstantStringMapEntry( SL, StringLength);

   if (auto *C = Entry.second)
      return ConstantAddress( C, Align);

   llvm::GlobalVariable   *GV;
   llvm::ConstantStruct   *NSStringHeader = CreateNSConstantStringStruct( Entry.first(), StringLength);

   GV = new llvm::GlobalVariable( CGM.getModule(), NSStringHeader->getType(), false,
                                 llvm::GlobalVariable::PrivateLinkage, NSStringHeader,
                                 "_unnamed_nsstring_header");
   // FIXME. Fix section.
   GV->setSection( "__DATA,__objc_stringobj,regular,no_dead_strip");
   GV->setConstant( false);

   QualType           CharType =  CGM.getContext().getPointerType( CGM.getContext().CharTy);
   llvm::Type         *CType = CGM.getTypes().ConvertTypeForMem( CharType);

   llvm::Constant     *C = getConstantGEP( VMContext, GV, 0, 2);
   llvm::GlobalAlias  *GA = llvm::GlobalAlias::create( CType,
                                                       0,
                                                       llvm::GlobalVariable::InternalLinkage,
                                                       Twine( "_unnamed_nsstring"),
                                                       C,
                                                       &CGM.getModule());
   Entry.second = GA;
   //   fprintf( stderr, "Created constant string for \"%.*s\"\n", (int) StringLength, (char *) str.data());
   return ConstantAddress( GA, Align);
}


#pragma mark -
#pragma mark message sending

// @mulle-objc@ MetaABI: CommonFunctionCall, send message to self and super
CodeGen::RValue   CGObjCMulleRuntime::CommonFunctionCall(CodeGen::CodeGenFunction &CGF,
                                                         llvm::Constant *Fn,
                                                         ReturnValueSlot Return,
                                                         QualType ResultType,
                                                         QualType FnResultType,
                                                         const CGFunctionInfo &FI,
                                                         llvm::Value *Receiver,
                                                         const CallArgList &CallArgs,
                                                         CallArgList   &ActualArgs,
                                                         llvm::Value   *Arg0,
                                                         const ObjCMethodDecl *Method)
{
   NullReturnState nullReturn;

   if (CGM.ReturnSlotInterferesWithArgs( FI))
      nullReturn.init( CGF, Arg0);

   RValue rvalue = CGF.EmitCall( FI, Fn, Return, ActualArgs, Method);

   RValue param = ActualArgs.size() >= 3 ? ActualArgs[ 2].RV : rvalue; // rvalue is just bogus, wont be used then

   rvalue = CGF.EmitMetaABIReadReturnValue( Method, rvalue, param, Return, ResultType);
   // it would be a good time to end the lifetime of the arg[2] alloca now
   // but this is done elsewhere, as not to disturb the method signatures
   // too much

   return nullReturn.complete( CGF, rvalue, ResultType, CallArgs, nullptr);
}



CodeGen::RValue CGObjCMulleRuntime::CommonMessageSend(CodeGen::CodeGenFunction &CGF,
                                                      llvm::Constant *Fn,
                                                      ReturnValueSlot Return,
                                                      QualType ResultType,
                                                      llvm::Value *Receiver,
                                                      const CallArgList &CallArgs,
                                                      CallArgList   &ActualArgs,
                                                      llvm::Value   *Arg0,
                                                      const ObjCMethodDecl *Method,
                                                      bool isDispatchFn)
{
   /* now common code with super follows */

   if (Method)
      assert(CGM.getContext().getCanonicalType(Method->getReturnType()) ==
             CGM.getContext().getCanonicalType(ResultType) &&
             "Result type mismatch!");
   else
   {
      /* if we have no method, we should construct a default ObjCMethodDecl
         from the selector with all ids
       */
   }
   //
   // this runs through patched code, to produce what we need
   //
   if( ! Fn)
   {
      int optLevel = CGM.getLangOpts().OptimizeSize ? -1 : CGM.getCodeGenOpts().OptimizationLevel;
      
      // tagged pointers bloat the code too much IMO (make decision later)
      // if( optLevel > 1 && ! no_tagged_pointers)
      //   optLevel = 1;
      
      if( ! CallArgs.size())
         Fn = ObjCTypes.getMessageSendNoParamFn( optLevel); // : ObjCTypes.getMessageSendFn0();
      else
         Fn = ObjCTypes.getMessageSendFn( optLevel); // : ObjCTypes.getMessageSendFn0();
      isDispatchFn = true;
   }

   if( isDispatchFn && ! CallArgs.size())
   {
      /*
      llvm::Value   *Arg2;
      // pushing a bogus Arg0 again is probably cheaper than nulling
      // but then maybe not
      // we need this for _params
      Arg2 = CGF.Builder.CreateBitCast( Receiver, ObjCTypes.ParamsPtrTy);
      ActualArgs.add( RValue::get( Arg2), CGF.getContext().VoidPtrTy);
      */
   }

   // fprintf( stderr, "#CallArgs %ld\n", CallArgs.size());
   // fprintf( stderr, "#ActualArgs %ld\n", ActualArgs.size());

   MessageSendInfo MSI = getMessageSendInfo( Method, ResultType, ActualArgs);

   return( CommonFunctionCall( CGF,
                              Fn,
                              Return,
                              ResultType,
                              CGF.getContext().VoidPtrTy,
                              MSI.CallInfo,
                              Receiver,
                              CallArgs,
                              ActualArgs,
                              Arg0,
                              Method));
}


const CGFunctionInfo   &CGObjCMulleRuntime::GenerateFunctionInfo( QualType arg0Ty,
                                                                  QualType rvalTy)
{
   FunctionType::ExtInfo   einfo;
   CallingConv             callConv;

   callConv = CGM.getContext().getDefaultCallingConvention( false, false);
  // @mulle-objc@ MetaABI: message signature: fix call convention
   einfo = einfo.withCallingConv( callConv);

  RequiredArgs required = RequiredArgs::All;
  SmallVector<CanQualType, 16> argTys;

  argTys.push_back( CGM.getContext().getCanonicalParamType( arg0Ty));

  // @mulle-objc@ MetaABI: fix returnType to void * (Part II)
   CodeGen::CodeGenTypes &Types = CGM.getTypes();

   const CGFunctionInfo &CallInfo = Types.arrangeLLVMFunctionInfo(
      rvalTy->getCanonicalTypeUnqualified().getUnqualifiedType(),
      /*instanceMethod=*/false,
      /*chainCall=*/false, argTys, einfo, {}, required);   /* if the method -release hasn't been declared yet, then
      we emit a void function call, but the compiler expects id
      back. (and crashes) Fake it up with a bogus NULL return.
   */
   return( CallInfo);
}


/// Generate code for a message send expression.
CodeGen::RValue CGObjCMulleRuntime::GenerateMessageSend(CodeGen::CodeGenFunction &CGF,
                                                        ReturnValueSlot Return,
                                                        QualType ResultType,
                                                        Selector Sel,
                                                        llvm::Value *Receiver,
                                                        const CallArgList &CallArgs,
                                                        const ObjCInterfaceDecl *Class,
                                                        const ObjCMethodDecl *Method)
{
   CallArgList     ActualArgs;
   QualType        TmpResultType;
   int             nArgs;
   llvm::Constant  *Fn;
   llvm::Value     *Arg0;
   llvm::Value     *selID;
   std::string     selName;

   Arg0 = CGF.Builder.CreateBitCast(Receiver, ObjCTypes.ObjectPtrTy);
   ActualArgs.add(RValue::get(Arg0), CGF.getContext().getObjCInstanceType());

   // figure out where to send the message
   // layme.. should probably use a map for this

   Fn            = nullptr;
   TmpResultType = ResultType;
   nArgs         = CallArgs.size();

   if( ! nArgs)
   {
      int optLevel = CGM.getLangOpts().OptimizeSize ? -1 : CGM.getCodeGenOpts().OptimizationLevel;

      //
      // use shortcuts when optimizing O2 and up
      //
      if( optLevel >= 2)
      {
         selName = Sel.getAsString();
         do
         {
            if( selName == "release")
            {
               Fn            = ObjCTypes.getMessageSendReleaseFn();
               TmpResultType = CGF.getContext().VoidTy;
               break;
            }

            if( selName == "retain")
            {
               Fn            = ObjCTypes.getMessageSendRetainFn();
               TmpResultType = CGF.getContext().getObjCInstanceType();
               break;
            }

            /* could just make this a nullptr */
            if( selName == "zone")
            {
               Fn            = ObjCTypes.getMessageSendZoneFn();
               TmpResultType = CGF.getContext().VoidPtrTy;
               break;
            }
         }
         while( 0);
      }
   }

   if( Fn == nullptr)  // regular method send call
   {
      selID = EmitSelector(CGF, Sel);
      ActualArgs.add(RValue::get( selID), CGF.getContext().getObjCSelType());
      ActualArgs.addFrom( CallArgs);
      return( CommonMessageSend( CGF,
                                 Fn,
                                 Return,
                                 ResultType,
                                 Receiver,
                                 CallArgs,
                                 ActualArgs,
                                 Arg0,
                                 Method,
                                 true));
   }

   CodeGen::RValue   rvalue;

   const CGFunctionInfo &CallInfo = GenerateFunctionInfo( ActualArgs[0].Ty, TmpResultType);
   const CGFunctionInfo &SignatureForCall = CGM.getTypes().arrangeCall( CallInfo, ActualArgs);

   rvalue  = CommonFunctionCall( CGF,
                                 Fn,
                                 Return,
                                 ResultType,
                                 TmpResultType,
                                 SignatureForCall,
                                 Receiver,
                                 CallArgs,
                                 ActualArgs,
                                 Arg0,
                                 nullptr);

   if( ResultType != TmpResultType)
      if( TmpResultType == CGF.getContext().VoidTy)
         rvalue = RValue::get( llvm::Constant::getNullValue( ObjCTypes.ObjectPtrTy));

   return( rvalue);
}



/// Generates a message send where the super is the receiver.  This is
/// a message send to self with special delivery semantics indicating
/// which class's method should be called.
CodeGen::RValue
CGObjCMulleRuntime::GenerateMessageSendSuper(CodeGen::CodeGenFunction &CGF,
                                    ReturnValueSlot Return,
                                    QualType ResultType,
                                    Selector Sel,
                                    const ObjCInterfaceDecl *Class,
                                    bool isCategoryImpl,
                                    llvm::Value *Receiver,
                                    bool IsClassMessage,
                                    const CodeGen::CallArgList &CallArgs,
                                    const ObjCMethodDecl *Method)
{
   llvm::Value   *Arg0;
   llvm::Value   *Arg2;
   CallArgList   ActualArgs;
   llvm::Value   *selID;
   llvm::Value   *classID;


   Arg0 = CGF.Builder.CreateBitCast(Receiver, ObjCTypes.ObjectPtrTy);
   ActualArgs.add(RValue::get( Arg0), CGF.getContext().getObjCInstanceType());

   selID   = EmitSelector(CGF, Sel);
   ActualArgs.add(RValue::get( selID), CGF.getContext().getObjCSelType());

   switch( CallArgs.size())
   {
   case 0 :
      Arg2 = CGF.Builder.CreateBitCast(Receiver, CGF.ConvertType( CGF.getContext().VoidPtrTy));
      ActualArgs.add( RValue::get( Arg2), CGF.getContext().VoidPtrTy);
      break;

   case 1 :
      ActualArgs.addFrom( CallArgs);
      break;

   default :
      llvm_unreachable( "metabi lossage, too many args");
   }

   classID = EmitClassID( CGF, Class->getSuperClass());

   // @mulle-objc@ uniqueid: make it 32 bit here
   ActualArgs.add(RValue::get( classID), CGF.getContext().getIntTypeForBitwidth( 32, false));


   // add classID
   // a class is not really an ObjCSelType but soo similiar, good enough

   // TODO add parameter for Class to call
   //   ActualArgs.add(RValue::get(  selID), CGF.getContext().getObjCSelType());

   llvm::Constant *Fn;

   int optLevel = CGM.getLangOpts().OptimizeSize ? -1 : CGM.getCodeGenOpts().OptimizationLevel;

   Fn = IsClassMessage ? ObjCTypes.getMessageSendMetaSuperFn( optLevel)
                       : ObjCTypes.getMessageSendSuperFn( optLevel);
   return( CommonMessageSend( CGF, Fn, Return, ResultType, Receiver, CallArgs, ActualArgs, Arg0, Method, false));
}



static uint64_t  get_size_of_type( CodeGenModule *CGM, QualType type);


CodeGen::RValue
CGObjCMulleRuntime::EmitFastEnumeratorCall( CodeGen::CodeGenFunction &CGF,
                                            ReturnValueSlot ReturnSlot,
                                            QualType ResultType,
                                            Selector FastEnumSel,
                                            llvm::Value *Receiver,
                                            llvm::Value *StatePtr,
                                            QualType StateTy,
                                            llvm::Value *ItemsPtr,
                                            QualType ItemsTy,
                                            llvm::Value *Count,
                                            QualType CountTy)
{
   llvm::Value    *Values[ 3];
   QualType       Types[ 3];
   unsigned int   i;
   QualType       ItemsPtrTy;

   Values[ 0] = StatePtr;
   Values[ 1] = ItemsPtr;
   Values[ 2] = Count;

   Types[ 0] = CGM.getContext().VoidPtrTy;
   Types[ 1] = CGM.getContext().VoidPtrTy;
   Types[ 2] = CountTy;


   RecordDecl  *RD    = CreateMetaABIRecordDecl( FastEnumSel, ArrayRef<QualType>( Types, 3));
   RecordDecl  *UD    = CreateMetaABIUnionDecl( CGF, RD, nullptr);
   LValue       Union = GenerateAllocaedUnion( CGF, UD);

   uint64_t     size = get_size_of_type( &CGM, Union.getType());
   llvm::Type   *UnionLLVMType = CGF.ConvertTypeForMem( Union.getType());
   llvm::Value  *emitMarker    = CGF.EmitLifetimeStart( size, Union.getPointer());

   llvm::Value   *RecordAddr        = CGF.Builder.CreateConstGEP2_32( UnionLLVMType, Union.getPointer(), 0, 0);
   QualType       RDType            = CGM.getContext().getTagDeclType( RD);
   llvm::Type    *RecordLLVMType    = CGF.ConvertTypeForMem( RDType);
   llvm::Type    *RecordLLVMPtrType = CGF.ConvertTypeForMem( CGM.getContext().getPointerType( RDType));
   llvm::Value   *CastedRecordAddr  = CGF.Builder.CreateBitCast( RecordAddr, RecordLLVMPtrType);

   for( i = 0; i < 3; i++)
   {
      CharUnits     Align        = CGM.getContext().getTypeAlignInChars( Types[ i]);
      llvm::Value   *LoadedValue;

      switch( i)
      {
      case 0 :
         LoadedValue = CGF.Builder.CreateConstGEP2_32( CGF.ConvertTypeForMem( StateTy), Values[ i], 0, 0);
         LoadedValue = CGF.Builder.CreateBitCast( LoadedValue, CGF.ConvertTypeForMem( Types[ i]));
         break;

      case 1  :
         LoadedValue = CGF.Builder.CreateConstGEP2_32( CGF.ConvertTypeForMem( ItemsTy), Values[ i], 0, 0);
         LoadedValue = CGF.Builder.CreateBitCast( LoadedValue, CGF.ConvertTypeForMem( Types[ i]));
         break;

      default :
         LoadedValue = Values[ i];
         break;
      }

      llvm::Value   *ElementAddr = CGF.Builder.CreateConstGEP2_32( RecordLLVMType, CastedRecordAddr, 0, i);
      CGF.Builder.CreateAlignedStore( LoadedValue, ElementAddr, Align.getQuantity(), false);
   }

   CallArgList Args;

   Args.add( RValue::get( Union.getPointer()), CGM.getContext().VoidPtrTy);

   CodeGen::RValue CountRV =
   GenerateMessageSend( CGF, ReturnSlot,
                       ResultType,
                       FastEnumSel,
                       Receiver,
                       Args,
                       nullptr,
                       nullptr);
   if( emitMarker)
      CGF.EmitLifetimeEnd( emitMarker, Union.getPointer());

   return( CountRV);
}

#pragma mark -
#pragma mark AST analysis for _param reuse
/*
 * Analyze AST for parameter usage.
 */

static   Expr  *unparenthesizedAndUncastedExpr( Expr *expr)
{
   CastExpr   *castExpr;

   for(;;)
   {
      expr = expr->IgnoreParens();

      if( (castExpr = dyn_cast< CastExpr>( expr)))
      {
         expr = castExpr->getSubExpr();
         continue;
      }
      return( expr);
   }
}


struct find_info
{
   Stmt   *next;
   bool   insideLoop;
};


bool   findNextStatementInParent( Stmt *stmt, Stmt *parent, struct find_info *info)
{
   bool   found;

   found = false;

   for( Stmt::child_iterator
        I = parent->child_begin(), E = parent->child_end(); I != E; ++I)
   {
      Stmt *child = *I;

      if( ! child)  /* this is a <<<NULL>>> apparently harmless ? */
         continue;

      if( found)
      {
         info->next = child;
         break;
      }

      if( child == stmt)
         found = true;
      else
         found = findNextStatementInParent( stmt, child, info);
   }

   if( found)
   {
      if( dyn_cast< DoStmt>( parent) ||
          dyn_cast< WhileStmt>( parent) ||
          dyn_cast< ForStmt>( parent))
      {
         info->insideLoop = true;
      }
   }

   return( found);
}


struct find_info   findNextStatementInBody( Stmt *s, Stmt *body)
{
   struct find_info  info;

   info.next       = nullptr;
   info.insideLoop = false;

   findNextStatementInParent( s, body, &info);
   return( info);
}


class MulleStatementVisitor : public RecursiveASTVisitor<MulleStatementVisitor>
{
  enum
  {
      IsUntaint    = 0x1,
      IsTaint      = 0x2,
      IsSupertaint = 0x3,
  };
  std::set<Decl *>   taintedLoves;
  std::set<Decl *>   supertaintedLoves;

  ObjCMethodDecl     *Method;
  ObjCMessageExpr    *Call;
  ImplicitParamDecl  *Param;
  Stmt               *NextStatement;
  Stmt               *LastStatement;
  Stmt               *SkipAfterStatement;
  bool               skipThisStatement;
  bool               returnFalseIfAnyTaintReference;
  bool               stopCollectingTaints;
  int                status;
  
public:
  MulleStatementVisitor( ObjCMethodDecl *M, ObjCMessageExpr *C)
  {
     Method = M;
     Call   = C;
     Param  = Method->getParamDecl();
     NextStatement = nullptr;
     LastStatement = nullptr;
     SkipAfterStatement = nullptr;
     returnFalseIfAnyTaintReference = false;
     stopCollectingTaints = false;
     status = 0;
  }
  
  Stmt  *GetLastStatement( void)
  {
     return( LastStatement);
  }
  
  int   GetStatus( void)
  {
      return( status);
  }
   
protected:
  MulleStatementVisitor( ObjCMethodDecl *M,
                        ImplicitParamDecl *P,
                        std::set<Decl *> *taints,
                        std::set<Decl *> *supertaints)
  {
     Method = M;
     Call   = nullptr;
     Param  = P;
     NextStatement = nullptr;
     LastStatement = nullptr;
     SkipAfterStatement = nullptr;
     taintedLoves.insert( taints->begin(), taints->end());
     supertaintedLoves.insert( supertaints->begin(), supertaints->end());
     stopCollectingTaints = false;
     returnFalseIfAnyTaintReference = false;
     status = 0;
  }

public:
   // this is always called and more qualified functions later too with same Stmt
   bool VisitStmt(Stmt *s)
   {
      // fprintf( stderr, "%s %p (%s)\n", s->getStmtClassName(), s, __PRETTY_FUNCTION__);
      
      skipThisStatement = SkipAfterStatement != nullptr;

      if( SkipAfterStatement == s)
      {
         skipThisStatement = true;
         SkipAfterStatement = nullptr;
      }
      
      if( NextStatement == s)
      {
         stopCollectingTaints = true;
      }
      LastStatement = s;
      
      return( true);
   }
   
   bool TraverseStmt(Stmt *s)
   {
      bool  flag;
      
      if( ! s)
         return( true);
      
       // fprintf( stderr, "-> %s %p (%s)\n", s->getStmtClassName(), s, __PRETTY_FUNCTION__);

       flag = RecursiveASTVisitor<MulleStatementVisitor>::TraverseStmt( s);

       // fprintf( stderr, "<- %s %p (%s) (%s) (status=%d)\n", s->getStmtClassName(), s, __PRETTY_FUNCTION__, flag ? "true" : "false", status);
      if( returnFalseIfAnyTaintReference && status)
         return( false);
      return( flag);;
   }

   bool VisitDeclRefExpr(DeclRefExpr *E)
   {
      Decl   *value;
      
      if( skipThisStatement)
         return( true);
      
      if( ! E)
         return( true);

      value = E->getDecl();
      if( value == Param)
      {
         status = IsTaint;
      }
      else
         if( taintedLoves.find( value) != taintedLoves.end())
         {
            status = IsTaint;
         }
         else
            if( supertaintedLoves.find( value) != supertaintedLoves.end())
            {
               status = IsSupertaint;
            }

      // fprintf( stderr, "%s %p (%s) status=%d\n", value->getDeclKindName(), value, __PRETTY_FUNCTION__, status);

      return( true);
   }
   
   bool VisitLabelStmt( LabelStmt *s)
   {
      //
      // we bail if we find a label before before the [call]
      // we could collect labels before the call and check
      // that the goto doesn't hit any known label
      //
      if( skipThisStatement)
         return( true);

      // fprintf( stderr, "%s %p (%s)\n", s->getStmtClassName(), s, __PRETTY_FUNCTION__);
      if( ! NextStatement)
      {
         // fprintf( stderr, "TAINTED BY LABEL %s %p (%s)\n", s->getStmtClassName(), s, __PRETTY_FUNCTION__);
         taintedLoves.clear();
         return( false);
      }
      return( true);
   }

   int  isStatementTainted( Stmt *s)
   {
      MulleStatementVisitor   SubVisitor( Method, Param, &taintedLoves, &supertaintedLoves);
      
      SubVisitor.TraverseStmt( s);
      SkipAfterStatement = SubVisitor.GetLastStatement();
   
      // fprintf( stderr, "%s %p (%s) status=%d\n", s->getStmtClassName(), s, __PRETTY_FUNCTION__, SubVisitor.GetStatus());

      return( SubVisitor.GetStatus());
   }
   
   
   // &<E>
   void  checkReferencingExpr( Expr *E)
   {
      int    exprStatus;
      
      E          = unparenthesizedAndUncastedExpr( E);
      exprStatus = isStatementTainted( E);
      
      // addr turns untaints into taints, and taints into supertaints
      switch( exprStatus)
      {
      case IsUntaint :     status = IsTaint; break;
      case IsTaint :       status = IsSupertaint; break;
      case IsSupertaint :  status = IsSupertaint; break;
      }
   }
   
   // *E, E->x, x[E], E[x]
   void  checkDereferencingExpr( Expr *E)
   {
      int    exprStatus;
      
      E          = unparenthesizedAndUncastedExpr( E);
      exprStatus = isStatementTainted( E);
      
      // * turns taints into untaints, supertaints and untaints stay
      switch( exprStatus)
      {
      case IsUntaint :     status = IsUntaint; break;
      case IsTaint :       status = IsUntaint; break;
      case IsSupertaint :  status = IsSupertaint; break;
      }
   }

   bool VisitMemberExpr( const MemberExpr *E)
   {
      if( skipThisStatement)
         return( true);
      
      // fprintf( stderr, "%s %p (%s)\n", E->getStmtClassName(), E, __PRETTY_FUNCTION__);

      checkDereferencingExpr( E->getBase());
      return( true);
   }
   
   bool VisitArraySubscriptExpr(const ArraySubscriptExpr *E)
   {
      if( skipThisStatement)
         return( true);

      // fprintf( stderr, "%s %p (%s)\n", E->getStmtClassName(), E, __PRETTY_FUNCTION__);
      
      checkDereferencingExpr( (Expr *) E->getLHS());
      checkDereferencingExpr( (Expr *) E->getRHS());
      return( true);
   }

   bool VisitUnaryOperator( UnaryOperator *op)
   {
      if( skipThisStatement)
         return( true);
      
      // fprintf( stderr, "%s %p (%s)\n", op->getStmtClassName(), op, __PRETTY_FUNCTION__);
      switch( op->getOpcode())
      {
      default :
         break;
         
      case UO_AddrOf :
         checkReferencingExpr( op->getSubExpr());
         break;

      case UO_Deref :
         checkDereferencingExpr( op->getSubExpr());
         break;
      }
      return( true);
   }

   //
   // check if left side is a pointer, check if right side
   // contains a taint, if yes, assume left side is tainted
   // downside:
   //   s = &tmp[ _param->width]
   // gets tainted, but its fairly complicated to parse this
   // correctly
   //
   bool VisitBinaryOperator( BinaryOperator *op)
   {
      Expr         *lhs;
      Expr         *rhs;
      DeclRefExpr  *ref;
      int          exprStatus;
      
      if( skipThisStatement)
         return( true);
      
      // fprintf( stderr, "%s %p (%s)\n", op->getStmtClassName(), op, __PRETTY_FUNCTION__);
      if( stopCollectingTaints)
         return( true);

      switch( op->getOpcode())
      {
      case BO_Assign    :
      case BO_MulAssign :
      case BO_DivAssign :
      case BO_RemAssign :
      case BO_AddAssign :
      case BO_SubAssign :
      case BO_ShlAssign :
      case BO_ShrAssign :
      case BO_AndAssign :
      case BO_XorAssign :
      case BO_OrAssign  :
         break;
      default           :
         return( true);
      }

      // ok check if leftside is a pointer
      lhs = op->getLHS();
      lhs = unparenthesizedAndUncastedExpr( lhs);
      if( (ref = dyn_cast< DeclRefExpr>( lhs)))
      {
         ValueDecl   *value;

         value = ref->getDecl();
         if( value->getType()->isPointerType())
         {
            // check if rhs is tainted, we don't check deeply, if any
            // taint is mentioned, we assume this pointer is also now tainted
            // its not too bad though, taints don't necessarily mean, that
            // the optimization can't happen

            rhs = op->getRHS();
            rhs = unparenthesizedAndUncastedExpr( rhs);

            exprStatus = isStatementTainted( rhs);
            // if( exprStatus > IsUntaint)
            //    fprintf( stderr, "TAINT %s %p (%s)\n", rhs->getStmtClassName(), rhs, __PRETTY_FUNCTION__);
            
            switch( exprStatus)
            {
            case IsUntaint    : break;
            case IsTaint      : taintedLoves.insert( value); status = IsTaint; break;
            case IsSupertaint : supertaintedLoves.insert( value); status = IsSupertaint; break;
            }
         }
      }
      return( true);
   }
   
   // own function
   bool  CheckObjCArguments( ObjCMessageExpr *C)
   {
      unsigned int  i, n;
      
      n = C->getNumArgs();
      for( i = 0; i < n; i++)
         if( isStatementTainted( C->getArg( i)) >= IsTaint)
            return( false);
      return( true);
   }
   
   bool VisitObjCMessageExpr(ObjCMessageExpr *d)
   {
      if( skipThisStatement)
         return( true);

      // fprintf( stderr, "[] %s %p (%s)\n", d->getStmtClassName(), d, __PRETTY_FUNCTION__);

      if( d == Call)
      {
         struct find_info  info;

         if( ! CheckObjCArguments( d))
            return( false);
         
         info = findNextStatementInBody( d, Method->getBody());
         if( info.insideLoop)
            return( false);

         // reset status now
         status = 0;
         returnFalseIfAnyTaintReference = true;
         NextStatement = info.next;
      }
      return( true);
   }
};



/*
 * Analyze AST for parameter usage.
 */
// some general thoughts:
// 1. we know that the contents of _param reside on the stack and are created
//    by the compiler
// 2. we can therefore rule out that anything in _param intentionally points
//    into param
// 3. Run through the whole function until we encounter the MesssageExpr,
//    collect variables that may alias our _param in the meantime. If we hit a
//    label we return false (currently)
// 4. Find the next statement after MesssageExpr
// 5. Continue with the regular callbacks until this statement hits
// 6. Now continue the flow of the statements including this one.
//    If we hit a loop statement going up the parents
//    we return false. If we find a DeclVarExpr hitting the taints we return false.
// 6. We reach the end, we return true
//

// What is a alias or a taint ?
//   anything that is not a 'taint' or an 'untaint' is 'ok'
//   _param on it's own is a  'taint' e.g.   : _param (duh)
//   dereference of  'taint' is an 'untaint' : *_param, _param->x, _param[ 0]
//   address_ot a untaint is a 'taint'       : &_param->x
//   address_of a 'taint' is a 'supertaint'  : &_param
//   dereference of a 'supertaint' is also a 'supertaint' (don't care no more) : *&_param
//   assignment of a 'taint' to a variable makes this variable a 'taint'
//   assignment of a 'supertaint' to a variable makes this variable a 'supertaint'
//

static bool  param_unused_after_expr( ObjCMethodDecl *Method, ObjCMessageExpr *Expr)
{
   MulleStatementVisitor   Visitor( Method, Expr);
   bool                    result;

   if( ! Method->getParamDecl())
      return( false);

   Stmt *Body = Method->getBody();

   result = Visitor.TraverseStmt( Body);
   return( result);
}


/*
 * Argument construction
 */
static size_t  rounded_type_length( ASTContext *context, QualType type1, QualType type2)
{
   struct TypeInfo Type1Info = context->getTypeInfo( type1);
   struct TypeInfo Type2Info = context->getTypeInfo( type2);
   size_t   size;
   size_t   unit;
   size_t   length;

   unit   = (Type2Info.Width + 7) / 8;
   size   = (Type1Info.Width + 7) / 8;

   length = (size + (unit - 1)) / unit;
   return( length);
}



// doppeltgemoppelt vermutlich

struct struct_info
{
   QualType    recTy;
   QualType    ptrTy;
   llvm::Type  *llvmType;
   size_t      size;
};

static uint64_t  get_size_of_type( CodeGenModule *CGM, QualType type)
{
   llvm::Type  *llvmType;

   llvmType = CGM->getTypes().ConvertTypeForMem( type);
   return( CGM->getDataLayout().getTypeAllocSize( llvmType));
}


static void  fill_struct_info_from_recTy( struct struct_info *info, CodeGenModule *CGM)
{
   info->ptrTy    = CGM->getContext().getPointerType( info->recTy);
   info->llvmType = CGM->getTypes().ConvertTypeForMem( info->recTy);
   info->size     = CGM->getDataLayout().getTypeAllocSize( info->llvmType);
}


static void  fill_struct_info( struct struct_info *info, CodeGenModule *CGM, TagDecl *Decl)
{
   info->recTy    = CGM->getContext().getTagDeclType( Decl);
   fill_struct_info_from_recTy( info, CGM);
}


static RecordDecl   *create_union_type( ASTContext *context, QualType paramType, QualType spaceType, QualType rvalType, bool hasRvalType)
{
   QualType       FieldTypes[3];
   const char     *FieldNames[3];
   unsigned int   i, n;

   RecordDecl  *UD = context->buildImplicitRecord( "_u_args");

   UD->setTagKind( TTK_Union);
   UD->startDefinition();

   FieldTypes[0] = paramType;
   FieldNames[0] = "v";

   FieldTypes[1] = spaceType;
   FieldNames[1] = "space";

   n = 2;
   if( hasRvalType)
   {
      FieldTypes[2] = rvalType;
      FieldNames[2] = "rval";
      ++n;
   }
   for( i = 0; i < n; i++)
   {
      FieldDecl *Field = FieldDecl::Create( *context,
                                            UD,
                                            SourceLocation(),
                                            SourceLocation(),
                                            &context->Idents.get( FieldNames[i]),
                                            FieldTypes[i], /*TInfo=*/nullptr,
                                            /*BitWidth=*/nullptr,
                                            /*Mutable=*/false,
                                            ICIS_NoInit);
      Field->setAccess( AS_public);
      UD->addDecl( Field);
   }

   UD->completeDefinition();

   return( UD);
}


#pragma mark
#pragma mark Argument construction
RecordDecl  *CGObjCMulleRuntime::CreateMetaABIRecordDecl( Selector sel,
                                                          ArrayRef< QualType> Types)
{
   ASTContext   *Context;
   StringRef    FieldName;
   StringRef    RecordName;
   char         buf[ 32];

   Context = &CGM.getContext();
   sprintf( buf, "generic.args_%ld", Types.size());
   RecordName = buf;

   IdentifierInfo  *RecordID = &Context->Idents.get( RecordName);

   ObjCMethodDecl *OnTheFlyDeclContext;

   TypeSourceInfo *ReturnTInfo = nullptr;
   OnTheFlyDeclContext =
      ObjCMethodDecl::Create( *Context,
                              SourceLocation(), SourceLocation(),
                              sel,
                              QualType(),
                              ReturnTInfo,
                              Context->getTranslationUnitDecl(),
                          /*isInstance=*/false, /*isVariadic=*/false,
                          /*isPropertyAccessor=*/false,
                          /*isImplicitlyDeclared=*/true,
                          /*isDefined=*/false, ObjCMethodDecl::Required,
                          /*HasRelatedResultType=*/false);

   // (DeclContext *) CGF.CurFuncDecl is a hack
   RecordDecl  *RD = RecordDecl::Create( *Context, TTK_Struct, OnTheFlyDeclContext, SourceLocation(), SourceLocation(), RecordID);

   for (unsigned i = 0, e = Types.size(); i != e; ++i)
   {
      FieldDecl   *FD;

      sprintf( buf, "param_%d", i);
      FieldName = buf;

      IdentifierInfo  *FieldID = &Context->Idents.get( FieldName);
      FD = FieldDecl::Create(  *Context, RD,
                               SourceLocation(), SourceLocation(),
                               FieldID,
                               Types[ i],
                               Context->CreateTypeSourceInfo( Types[ i], 0),
                               NULL,
                               false,  // Mutable... only for C++
                               ICIS_NoInit);
      RD->addDecl( FD);
   }
   RD->completeDefinition();

   return( RD);
}



RecordDecl  *CGObjCMulleRuntime::CreateOnTheFlyRecordDecl( const ObjCMessageExpr *Method)
{
   ASTContext   *Context;
   StringRef    FieldName;
   StringRef    RecordName;
   char         buf[ 32];

   Context = &CGM.getContext();
   sprintf( buf, "generic.args_%d", Method->getNumArgs());
   RecordName = buf;

   IdentifierInfo  *RecordID = &Context->Idents.get( RecordName);

   ObjCMethodDecl *OnTheFlyDeclContext;

   TypeSourceInfo *ReturnTInfo = nullptr;
   OnTheFlyDeclContext =
      ObjCMethodDecl::Create( *Context,
                              SourceLocation(), SourceLocation(),
                              Method->getSelector(),
                              QualType(),
                              ReturnTInfo,
                              Context->getTranslationUnitDecl(),
                          /*isInstance=*/false, /*isVariadic=*/false,
                          /*isPropertyAccessor=*/false,
                          /*isImplicitlyDeclared=*/true,
                          /*isDefined=*/false, ObjCMethodDecl::Required,
                          /*HasRelatedResultType=*/false);

   // (DeclContext *) CGF.CurFuncDecl is a hack
   RecordDecl  *RD = RecordDecl::Create( *Context, TTK_Struct, OnTheFlyDeclContext, SourceLocation(), SourceLocation(), RecordID);

   for (unsigned i = 0, e = Method->getNumArgs(); i != e; ++i)
   {
      FieldDecl   *FD;
      const Expr  *arg;

      sprintf( buf, "param_%d", i);
      FieldName = buf;

      // if we create something on the fly, the default promotion should
      // have already be in affected

      arg = Method->getArg( i);

      IdentifierInfo  *FieldID = &Context->Idents.get( FieldName);
      FD = FieldDecl::Create(  *Context, RD,
                               SourceLocation(), SourceLocation(),
                               FieldID,
                               arg->getType(),
                               Context->CreateTypeSourceInfo( arg->getType(), 0),
                               NULL,
                               false,  // Mutable... only for C++
                               ICIS_NoInit);
      RD->addDecl( FD);
   }
   RD->completeDefinition();

   return( RD);
}


/* variadic alloca, could have size of struct block in
 * front, for interpreters / storage (?)
 */
RecordDecl  *CGObjCMulleRuntime::CreateVariadicOnTheFlyRecordDecl( Selector Sel,
                                                                   RecordDecl *RD,
                                                                   llvm::ArrayRef<const Expr*> &Exprs)
{
   ASTContext   *Context;
   StringRef    FieldName;
   StringRef    RecordName;
   char         buf[ 32];

   Context = &CGM.getContext();

   // try to reuse identifier for all, as we are in codegen I don't think
   // it matters

   IdentifierInfo  *RecordID = &Context->Idents.get( "variadic_expr");

   ObjCMethodDecl *OnTheFlyDeclContext;

   TypeSourceInfo *ReturnTInfo = nullptr;
   OnTheFlyDeclContext =
      ObjCMethodDecl::Create( *Context,
                              SourceLocation(), SourceLocation(),
                              Sel,
                              QualType(),
                              ReturnTInfo,
                              Context->getTranslationUnitDecl(),
                          /*isInstance=*/false, /*isVariadic=*/false,
                          /*isPropertyAccessor=*/false,
                          /*isImplicitlyDeclared=*/true,
                          /*isDefined=*/false, ObjCMethodDecl::Required,
                          /*HasRelatedResultType=*/false);

   // (DeclContext *) CGF.CurFuncDecl is a hack
   RecordDecl  *RD2 = RecordDecl::Create( *Context, TTK_Struct, OnTheFlyDeclContext, SourceLocation(), SourceLocation(), RecordID);


   /* copy over parameters from Record decl first */
   unsigned int FieldNo;

   FieldNo = 0;
   if( RD)
   {
      for (RecordDecl::field_iterator Field = RD->field_begin(),
           FieldEnd = RD->field_end(); Field != FieldEnd; ++Field, ++FieldNo)
      {
         FieldDecl   *FD;

         sprintf( buf, "param_%d", FieldNo);
         FieldName = buf;

         IdentifierInfo  *FieldID = &Context->Idents.get( FieldName);
         FD = FieldDecl::Create(  *Context, RD2,
                                SourceLocation(), SourceLocation(),
                                FieldID,
                                Field->getType(),
                                Field->getTypeSourceInfo(),
                                NULL,
                                false,  // Mutable... only for C++
                                ICIS_NoInit);
         RD2->addDecl( FD);
      }
   }

   /*
    * Now copy over remaining variadic arguments.
    */
   for (unsigned i = FieldNo, e = Exprs.size(); i != e; ++i)
   {
      FieldDecl   *FD;
      const Expr  *arg;
      QualType    type;

      sprintf( buf, "param_%d", i);
      FieldName = buf;

      // variadic expressions should be already promoted!
      arg  = Exprs[ i];
      type = arg->getType();

      IdentifierInfo  *FieldID = &Context->Idents.get( FieldName);
      FD = FieldDecl::Create(  *Context, RD2,
                               SourceLocation(), SourceLocation(),
                               FieldID,
                               type,
                               Context->CreateTypeSourceInfo( type, 0),
                               NULL,
                               false,  // Mutable... only for C++
                               ICIS_NoInit);
      RD2->addDecl( FD);
   }


   RD2->completeDefinition();

   return( RD2);
}

/// Create the CallArgList
/// Here we stuff all arguments into a alloca struct
/// @mulle-objc@ MetaABI: stuff values into alloca
///

QualType   CGObjCMulleRuntime::CreateVoid5PtrTy( void)
{
   llvm::APInt   units( 32, 5);

   // construct  void  *[5];
   return( CGM.getContext().getConstantArrayType( CGM.getContext().VoidPtrTy,
                                                 units,
                                                 ArrayType::Normal,
                                                 0));
}


RecordDecl *CGObjCMulleRuntime::CreateMetaABIUnionDecl( CodeGenFunction &CGF,
                                                        RecordDecl  *RD,
                                                        RecordDecl  *RV)
{
   struct struct_info   record_info;
   struct struct_info   rval_info;

   fill_struct_info( &record_info, &CGM, RD);
   if( RV)
      fill_struct_info( &rval_info, &CGM, RV);

   return( CreateMetaABIUnionDecl( CGF,
                                  record_info.recTy, record_info.size,
                                  rval_info.recTy, RV != NULL));
}


RecordDecl   *CGObjCMulleRuntime::CreateMetaABIUnionDecl( CodeGenFunction &CGF,
                                                          QualType recTy,
                                                          uint64_t recSize,
                                                          QualType  rvalTy,
                                                          bool hasRvalTy)
{
   //
   // at this point, we alloca something and copy all the values
   // into the alloca, at a later time, we check
   // if we can't reuse the input _param block, and if yes substitute
   // values. The allocaed block needs to be big enough to hold the
   // void *[5] pointer units
   //
   // We make a proper union type here (maybe pedantic)
   //
   QualType Void5PtrTy = CreateVoid5PtrTy();

   // determine 'n' in  void  *[5][ n] and construct type
   llvm::APInt   units2( 32, rounded_type_length( &CGM.getContext(), recTy, Void5PtrTy));
   QualType array2Type = CGM.getContext().getConstantArrayType( Void5PtrTy,
                                                                units2,
                                                                ArrayType::Normal,
                                                                0);

   // construct union { struct _param v; void  *[5][ n] space; struct _rval rval;  };

   return( create_union_type( &CGM.getContext(), recTy, array2Type, rvalTy, hasRvalTy));
}


LValue  CGObjCMulleRuntime::GenerateAllocaedUnion( CodeGenFunction &CGF,
                                                   RecordDecl *UD)
{
   QualType     unionTy = CGM.getContext().getTagDeclType( UD);

   // now alloca the union
   Address  allocaed = CGF.CreateMemTemp( unionTy, "_args");  // leak ?
   LValue   Union  = CGF.MakeNaturalAlignAddrLValue( allocaed.getPointer(), unionTy);

   return( Union);
}


bool CGObjCMulleRuntime::OptimizeReuseParam( CodeGenFunction &CGF,
                                            CallArgList &Args,
                                            const ObjCMessageExpr *Expr,
                                            RecordDecl   *RD,
                                            Address  RecordAddress,
                                            CGObjCRuntimeLifetimeMarker &Marker)
{
   const ObjCMethodDecl *parent;

   parent = dyn_cast<ObjCMethodDecl>(CGF.CurFuncDecl);
   if( ! parent)
      return( false);

   // variadic can't reuse, because of varargs...

   if( parent->isVariadic())
      return( false);

   RecordDecl *PRD = parent->getParamRecord();

   if( ! PRD)
      return( false);

   struct struct_info   parent_info;
   struct struct_info   record_info;
   // construct  void  *[5];
   llvm::APInt   units( 32, 5);
   QualType Void5PtrTy = CGM.getContext().getConstantArrayType( CGM.getContext().VoidPtrTy,
                                                               units,
                                                               ArrayType::Normal,
                                                               0);
   fill_struct_info( &record_info, &CGM, RD);
   fill_struct_info( &parent_info, &CGM, PRD);

   size_t sizeRecord       = rounded_type_length( &CGM.getContext(), record_info.recTy, Void5PtrTy);
   size_t sizeParentRecord = rounded_type_length( &CGM.getContext(), parent_info.recTy, Void5PtrTy);

   if( sizeParentRecord < sizeRecord)
      return( false);

   //
   // ok it would fit
   // now we have to figure out, where we are, then follow the flow of
   // the function and be sure, that _param won't be used again (not
   // so easy)
   // _param but not necessarily _rval (which is in a union with _param)
   //

   if( ! param_unused_after_expr( (ObjCMethodDecl *) parent, (ObjCMessageExpr *) Expr))
   {
      // fprintf( stderr, "reuse denied\n");
      return( false);
   }
   // fprintf( stderr, "can reuse\n");

   //
   // nice we can substitute _param, so what we do is
   // memcpy over the contents of alloca into it
   // the optimizer can optimize the alloca away then
   //
   ImplicitParamDecl  *D = parent->getParamDecl();

   DeclarationNameInfo NameInfo( D->getDeclName(), SourceLocation());

   DeclRefExpr *E = DeclRefExpr::Create( CGM.getContext(),
                                        NestedNameSpecifierLoc(),
                                        SourceLocation(),
                                        D,
                                        false,
                                        NameInfo,
                                        parent_info.recTy,
                                        VK_LValue);

   LValue ParentRecord = CGF.EmitLValue( E);
   // this next emit is needed, weird llvmism i dont understand
   RValue loaded       = CGF.EmitLoadOfLValue(ParentRecord, SourceLocation());

   CGF.EmitAggregateCopy( Address( loaded.getScalarVal(), CGM.getPointerAlign()),
                         RecordAddress,
                         record_info.recTy,
                         false);

   // dont need the alloca anymore
   if( Marker.SizeV)
   {
      CGF.EmitLifetimeEnd( Marker.SizeV, Marker.Addr);
      Marker.SizeV = nullptr;
   }

   Args.add( loaded, CGM.getContext().VoidPtrTy);
   return( true);
}


LValue  CGObjCMulleRuntime::GenerateMetaABIRecordAlloca( CodeGenFunction &CGF,
                                                         RecordDecl   *RD,
                                                         RecordDecl   *RV,
                                                         CGObjCRuntimeLifetimeMarker &Marker)
{
   RecordDecl *UD = CreateMetaABIUnionDecl( CGF,
                                           RD,
                                           RV);
   LValue Union = GenerateAllocaedUnion( CGF, UD);

   //
   // specify beginning of life for this alloca
   // do this always, so that the optimizer can get rid of it
   //
   Marker.Addr  = Union.getPointer();
   Marker.SizeV = CGF.EmitLifetimeStart( get_size_of_type( &CGM, Union.getType()), Marker.Addr);

   // now get record out of union again
   LValue Record = CGF.EmitLValueForField( Union, *UD->field_begin());

   return( Record);
}


void  CGObjCMulleRuntime::PushArgumentsIntoRecord( CodeGenFunction &CGF,
                                                   RecordDecl *RD,
                                                   LValue Record,
                                                   const ObjCMessageExpr *Method)
{
   unsigned int i = 0;

   for( RecordDecl::field_iterator CurField = RD->field_begin(), SentinelField = RD->field_end(); CurField != SentinelField; CurField++)
   {
      const Expr  *arg;
      FieldDecl   *Field;

      Field = *CurField;
      arg   = Method->getArg( i);

      LValue LV = CGF.EmitLValueForFieldInitialization( Record, Field);
      CGF.EmitInitializerForField( Field, LV, (Expr *) arg, None);

      ++i;
   }
}


void   CGObjCMulleRuntime::EmitVoidPtrExpression( CodeGenFunction &CGF,
                                                  CallArgList &Args,
                                                  const Expr *Arg)
{

   if( Arg->isIntegerConstantExpr(CGM.getContext()))
   {
      TypeSourceInfo *TInfo = CGM.getContext().getTrivialTypeSourceInfo(CGM.getContext().VoidPtrTy, SourceLocation());
      Arg  = CStyleCastExpr::Create( CGM.getContext(),
                                    CGM.getContext().VoidPtrTy,
                                    VK_RValue,
                                    CK_IntegralToPointer,
                                    (Expr *) Arg,
                                    NULL,
                                    TInfo,
                                    SourceLocation(),
                                    SourceLocation());
   }
   CGF.EmitCallArg( Args, Arg, CGM.getContext().VoidPtrTy);
}



void  CGObjCMulleRuntime::PushCallArgsIntoRecord( CodeGenFunction &CGF,
                                                  RecordDecl *RD,
                                                  LValue Record,
                                                  CallArgList &Args)
{
   unsigned int i = 0;

   for( RecordDecl::field_iterator CurField = RD->field_begin(), SentinelField = RD->field_end(); CurField != SentinelField; CurField++)
   {
      FieldDecl    *Field;

      Field = *CurField;

      RValue   RHS = Args[ i].RV;
      LValue   LHS = CGF.EmitLValueForFieldInitialization( Record, *CurField);
      CGF.EmitStoreThroughLValue( RHS, LHS, true);
      ++i;
   }
}



// this is only used for literals...
//
CGObjCRuntimeLifetimeMarker   CGObjCMulleRuntime::ConvertToMetaABIArgsIfNeeded( CodeGenFunction &CGF,
                                                                                const ObjCMethodDecl *method,
                                                                                CallArgList &Args)
{
   CGObjCRuntimeLifetimeMarker  Marker;
   RecordDecl   *RD;
   RecordDecl   *RV;
   SmallVector< CallArg, 16>  ArgArray;

   Marker.SizeV = nullptr;
   Marker.Addr  = nullptr;

   assert( method);

   RD = method->getParamRecord();

   // there are no variadic literals

   // special case, argument is void * compatible, there is no paramRecord.
   // yet one argument will likely have been passed
   if( ! RD)
      return( Marker);

   RV = method->getRvalRecord();

   LValue   Record = GenerateMetaABIRecordAlloca( CGF, RD, RV, Marker);

   PushCallArgsIntoRecord( CGF, RD, Record, Args);

   // can't optimize this
   // we are always pushing a void through mulle_objc_calls

   Args.clear();
   assert( Args.size() == 0);

   Args.add( RValue::get( Record.getPointer()), CGM.getContext().VoidPtrTy);
   return( Marker);
}


CGObjCRuntimeLifetimeMarker  CGObjCMulleRuntime::GenerateCallArgs( CodeGenFunction &CGF,
                                                                   CallArgList &Args,
                                                                   const ObjCMethodDecl *method,
                                                                   const ObjCMessageExpr *Expr)
{
   RecordDecl   *RD;
   RecordDecl   *RV;
   CGObjCRuntimeLifetimeMarker  Marker;

   Marker.SizeV = nullptr;
   Marker.Addr  = nullptr;

   // Initialize the captured struct.
   RD = NULL;
   RV = NULL;

   if( method)
   {
      RD = method->getParamRecord();
      RV = method->getRvalRecord();
      if( method->isVariadic())
      {
         auto Args = llvm::makeArrayRef( Expr->getArgs(), Expr->getNumArgs());

         RD = CreateVariadicOnTheFlyRecordDecl( method->getSelector(), RD, Args);
      }
   }
   else
   {
      // missing method declaration
      do
      {
      }
      while(0);
   }

   // special case, argument is void * compatible, there is no paramRecord.
   // yet one argument will likely have been passed
   if( ! RD)
   {
      assert( ! RV);

      switch( Expr->getNumArgs())
      {
      case 1 :
         EmitVoidPtrExpression( CGF, Args, Expr->getArg( 0));
         // fall thru
      case 0 :
         return( Marker);
      }

      // undeclared method, with multiple arguments, assume they are all
      // id ...
      RD = CreateOnTheFlyRecordDecl( Expr);
   }

   LValue   Record = GenerateMetaABIRecordAlloca( CGF, RD, RV, Marker);

   PushArgumentsIntoRecord( CGF, RD, Record, Expr);

   //
   // ok lets see if we can't use _params as an argument
   // First see if we even have that (we could be in a C function)
   // only do this when optimizing.
   //
   if( CGM.getCodeGenOpts().OptimizationLevel >= 2)
   {
      if( OptimizeReuseParam( CGF, Args, Expr, RD, Record.getAddress(), Marker))
         return( Marker);
   }
   // we are always pushing a void through mulle_objc_calls
   Args.add( RValue::get( Record.getPointer()), CGM.getContext().VoidPtrTy);
   return( Marker);
}

#pragma mark -
#pragma mark blocks

static Qualifiers::GC GetGCAttrTypeForType(ASTContext &Ctx, QualType FQT) {
   if (FQT.isObjCGCStrong())
      return Qualifiers::Strong;

   if (FQT.isObjCGCWeak() || FQT.getObjCLifetime() == Qualifiers::OCL_Weak)
      return Qualifiers::Weak;

   // check for __unsafe_unretained
   if (FQT.getObjCLifetime() == Qualifiers::OCL_ExplicitNone)
      return Qualifiers::GCNone;

   if (FQT->isObjCObjectPointerType() || FQT->isBlockPointerType())
      return Qualifiers::Strong;

   if (const PointerType *PT = FQT->getAs<PointerType>())
      return GetGCAttrTypeForType(Ctx, PT->getPointeeType());

   return Qualifiers::GCNone;
}

llvm::Constant *CGObjCCommonMulleRuntime::BuildGCBlockLayout(CodeGenModule &CGM,
                                                    const CGBlockInfo &blockInfo) {

   llvm::Constant *nullPtr = llvm::Constant::getNullValue(CGM.Int8PtrTy);
   if (CGM.getLangOpts().getGC() == LangOptions::NonGC &&
       !CGM.getLangOpts().ObjCAutoRefCount)
      return nullPtr;

   bool hasUnion = false;
   SkipIvars.clear();
   IvarsInfo.clear();
   unsigned WordSizeInBits = CGM.getTarget().getPointerWidth(0);
   unsigned ByteSizeInBits = CGM.getTarget().getCharWidth();

   // __isa is the first field in block descriptor and must assume by runtime's
   // convention that it is GC'able.
   IvarsInfo.push_back(GC_IVAR(0, 1));

   const BlockDecl *blockDecl = blockInfo.getBlockDecl();

   // Calculate the basic layout of the block structure.
   const llvm::StructLayout *layout =
   CGM.getDataLayout().getStructLayout(blockInfo.StructureType);

   // Ignore the optional 'this' capture: C++ objects are not assumed
   // to be GC'ed.

   // Walk the captured variables.
   for (const auto &CI : blockDecl->captures()) {
      const VarDecl *variable = CI.getVariable();
      QualType type = variable->getType();

      const CGBlockInfo::Capture &capture = blockInfo.getCapture(variable);

      // Ignore constant captures.
      if (capture.isConstant()) continue;

      uint64_t fieldOffset = layout->getElementOffset(capture.getIndex());

      // __block variables are passed by their descriptor address.
      if (CI.isByRef()) {
         IvarsInfo.push_back(GC_IVAR(fieldOffset, /*size in words*/ 1));
         continue;
      }

      assert(!type->isArrayType() && "array variable should not be caught");
      if (const RecordType *record = type->getAs<RecordType>()) {
         BuildAggrIvarRecordLayout(record, fieldOffset, true, hasUnion);
         continue;
      }

      Qualifiers::GC GCAttr = GetGCAttrTypeForType(CGM.getContext(), type);
      unsigned fieldSize = CGM.getContext().getTypeSize(type);

      if (GCAttr == Qualifiers::Strong)
         IvarsInfo.push_back(GC_IVAR(fieldOffset,
                                     fieldSize / WordSizeInBits));
      else if (GCAttr == Qualifiers::GCNone || GCAttr == Qualifiers::Weak)
         SkipIvars.push_back(GC_IVAR(fieldOffset,
                                     fieldSize / ByteSizeInBits));
   }

   if (IvarsInfo.empty())
      return nullPtr;

   // Sort on byte position; captures might not be allocated in order,
   // and unions can do funny things.
   llvm::array_pod_sort(IvarsInfo.begin(), IvarsInfo.end());
   llvm::array_pod_sort(SkipIvars.begin(), SkipIvars.end());

   std::string BitMap;
   llvm::Constant *C = BuildIvarLayoutBitmap(BitMap);
   if (CGM.getLangOpts().ObjCGCBitmapPrint) {
      printf("\n block variable layout for block: ");
      const unsigned char *s = (const unsigned char*)BitMap.c_str();
      for (unsigned i = 0, e = BitMap.size(); i < e; i++)
         if (!(s[i] & 0xf0))
            printf("0x0%x%s", s[i], s[i] != 0 ? ", " : "");
         else
            printf("0x%x%s",  s[i], s[i] != 0 ? ", " : "");
      printf("\n");
   }

   return C;
}

/// getBlockCaptureLifetime - This routine returns life time of the captured
/// block variable for the purpose of block layout meta-data generation. FQT is
/// the type of the variable captured in the block.
Qualifiers::ObjCLifetime CGObjCCommonMulleRuntime::getBlockCaptureLifetime(QualType FQT,
                                                                  bool ByrefLayout) {
   if (CGM.getLangOpts().ObjCAutoRefCount)
      return FQT.getObjCLifetime();

   // MRR.
   if (FQT->isObjCObjectPointerType() || FQT->isBlockPointerType())
      return ByrefLayout ? Qualifiers::OCL_ExplicitNone : Qualifiers::OCL_Strong;

   return Qualifiers::OCL_None;
}

void CGObjCCommonMulleRuntime::UpdateRunSkipBlockVars(bool IsByref,
                                             Qualifiers::ObjCLifetime LifeTime,
                                             CharUnits FieldOffset,
                                             CharUnits FieldSize) {
   // __block variables are passed by their descriptor address.
   if (IsByref)
      RunSkipBlockVars.push_back(RUN_SKIP(BLOCK_LAYOUT_BYREF, FieldOffset,
                                          FieldSize));
   else if (LifeTime == Qualifiers::OCL_Strong)
      RunSkipBlockVars.push_back(RUN_SKIP(BLOCK_LAYOUT_STRONG, FieldOffset,
                                          FieldSize));
   else if (LifeTime == Qualifiers::OCL_Weak)
      RunSkipBlockVars.push_back(RUN_SKIP(BLOCK_LAYOUT_WEAK, FieldOffset,
                                          FieldSize));
   else if (LifeTime == Qualifiers::OCL_ExplicitNone)
      RunSkipBlockVars.push_back(RUN_SKIP(BLOCK_LAYOUT_UNRETAINED, FieldOffset,
                                          FieldSize));
   else
      RunSkipBlockVars.push_back(RUN_SKIP(BLOCK_LAYOUT_NON_OBJECT_BYTES,
                                          FieldOffset,
                                          FieldSize));
}

void CGObjCCommonMulleRuntime::BuildRCRecordLayout(const llvm::StructLayout *RecLayout,
                                          const RecordDecl *RD,
                                          ArrayRef<const FieldDecl*> RecFields,
                                          CharUnits BytePos, bool &HasUnion,
                                          bool ByrefLayout) {
   bool IsUnion = (RD && RD->isUnion());
   CharUnits MaxUnionSize = CharUnits::Zero();
   const FieldDecl *MaxField = nullptr;
   const FieldDecl *LastFieldBitfieldOrUnnamed = nullptr;
   CharUnits MaxFieldOffset = CharUnits::Zero();
   CharUnits LastBitfieldOrUnnamedOffset = CharUnits::Zero();

   if (RecFields.empty())
      return;
   unsigned ByteSizeInBits = CGM.getTarget().getCharWidth();

   for (unsigned i = 0, e = RecFields.size(); i != e; ++i) {
      const FieldDecl *Field = RecFields[i];
      // Note that 'i' here is actually the field index inside RD of Field,
      // although this dependency is hidden.
      const ASTRecordLayout &RL = CGM.getContext().getASTRecordLayout(RD);
      CharUnits FieldOffset =
      CGM.getContext().toCharUnitsFromBits(RL.getFieldOffset(i));

      // Skip over unnamed or bitfields
      if (!Field->getIdentifier() || Field->isBitField()) {
         LastFieldBitfieldOrUnnamed = Field;
         LastBitfieldOrUnnamedOffset = FieldOffset;
         continue;
      }

      LastFieldBitfieldOrUnnamed = nullptr;
      QualType FQT = Field->getType();
      if (FQT->isRecordType() || FQT->isUnionType()) {
         if (FQT->isUnionType())
            HasUnion = true;

         BuildRCBlockVarRecordLayout(FQT->getAs<RecordType>(),
                                     BytePos + FieldOffset, HasUnion);
         continue;
      }

      if (const ArrayType *Array = CGM.getContext().getAsArrayType(FQT)) {
         const ConstantArrayType *CArray =
         dyn_cast_or_null<ConstantArrayType>(Array);
         uint64_t ElCount = CArray->getSize().getZExtValue();
         assert(CArray && "only array with known element size is supported");
         FQT = CArray->getElementType();
         while (const ArrayType *Array = CGM.getContext().getAsArrayType(FQT)) {
            const ConstantArrayType *CArray =
            dyn_cast_or_null<ConstantArrayType>(Array);
            ElCount *= CArray->getSize().getZExtValue();
            FQT = CArray->getElementType();
         }
         if (FQT->isRecordType() && ElCount) {
            int OldIndex = RunSkipBlockVars.size() - 1;
            const RecordType *RT = FQT->getAs<RecordType>();
            BuildRCBlockVarRecordLayout(RT, BytePos + FieldOffset,
                                        HasUnion);

            // Replicate layout information for each array element. Note that
            // one element is already done.
            uint64_t ElIx = 1;
            for (int FirstIndex = RunSkipBlockVars.size() - 1 ;ElIx < ElCount; ElIx++) {
               CharUnits Size = CGM.getContext().getTypeSizeInChars(RT);
               for (int i = OldIndex+1; i <= FirstIndex; ++i)
                  RunSkipBlockVars.push_back(
                                             RUN_SKIP(RunSkipBlockVars[i].opcode,
                                                      RunSkipBlockVars[i].block_var_bytepos + Size*ElIx,
                                                      RunSkipBlockVars[i].block_var_size));
            }
            continue;
         }
      }
      CharUnits FieldSize = CGM.getContext().getTypeSizeInChars(Field->getType());
      if (IsUnion) {
         CharUnits UnionIvarSize = FieldSize;
         if (UnionIvarSize > MaxUnionSize) {
            MaxUnionSize = UnionIvarSize;
            MaxField = Field;
            MaxFieldOffset = FieldOffset;
         }
      } else {
         UpdateRunSkipBlockVars(false,
                                getBlockCaptureLifetime(FQT, ByrefLayout),
                                BytePos + FieldOffset,
                                FieldSize);
      }
   }

   if (LastFieldBitfieldOrUnnamed) {
      if (LastFieldBitfieldOrUnnamed->isBitField()) {
         // Last field was a bitfield. Must update the info.
         uint64_t BitFieldSize
         = LastFieldBitfieldOrUnnamed->getBitWidthValue(CGM.getContext());
         unsigned UnsSize = (BitFieldSize / ByteSizeInBits) +
         ((BitFieldSize % ByteSizeInBits) != 0);
         CharUnits Size = CharUnits::fromQuantity(UnsSize);
         Size += LastBitfieldOrUnnamedOffset;
         UpdateRunSkipBlockVars(false,
                                getBlockCaptureLifetime(LastFieldBitfieldOrUnnamed->getType(),
                                                        ByrefLayout),
                                BytePos + LastBitfieldOrUnnamedOffset,
                                Size);
      } else {
         assert(!LastFieldBitfieldOrUnnamed->getIdentifier() &&"Expected unnamed");
         // Last field was unnamed. Must update skip info.
         CharUnits FieldSize
         = CGM.getContext().getTypeSizeInChars(LastFieldBitfieldOrUnnamed->getType());
         UpdateRunSkipBlockVars(false,
                                getBlockCaptureLifetime(LastFieldBitfieldOrUnnamed->getType(),
                                                        ByrefLayout),
                                BytePos + LastBitfieldOrUnnamedOffset,
                                FieldSize);
      }
   }

   if (MaxField)
      UpdateRunSkipBlockVars(false,
                             getBlockCaptureLifetime(MaxField->getType(), ByrefLayout),
                             BytePos + MaxFieldOffset,
                             MaxUnionSize);
}

void CGObjCCommonMulleRuntime::BuildRCBlockVarRecordLayout(const RecordType *RT,
                                                  CharUnits BytePos,
                                                  bool &HasUnion,
                                                  bool ByrefLayout) {
   const RecordDecl *RD = RT->getDecl();
   SmallVector<const FieldDecl*, 16> Fields(RD->fields());
   llvm::Type *Ty = CGM.getTypes().ConvertType(QualType(RT, 0));
   const llvm::StructLayout *RecLayout =
   CGM.getDataLayout().getStructLayout(cast<llvm::StructType>(Ty));

   BuildRCRecordLayout(RecLayout, RD, Fields, BytePos, HasUnion, ByrefLayout);
}

/// InlineLayoutInstruction - This routine produce an inline instruction for the
/// block variable layout if it can. If not, it returns 0. Rules are as follow:
/// If ((uintptr_t) layout) < (1 << 12), the layout is inline. In the 64bit world,
/// an inline layout of value 0x0000000000000xyz is interpreted as follows:
/// x captured object pointers of BLOCK_LAYOUT_STRONG. Followed by
/// y captured object of BLOCK_LAYOUT_BYREF. Followed by
/// z captured object of BLOCK_LAYOUT_WEAK. If any of the above is missing, zero
/// replaces it. For example, 0x00000x00 means x BLOCK_LAYOUT_STRONG and no
/// BLOCK_LAYOUT_BYREF and no BLOCK_LAYOUT_WEAK objects are captured.
uint64_t CGObjCCommonMulleRuntime::InlineLayoutInstruction(
                                                  SmallVectorImpl<unsigned char> &Layout) {
   uint64_t Result = 0;
   if (Layout.size() <= 3) {
      unsigned size = Layout.size();
      unsigned strong_word_count = 0, byref_word_count=0, weak_word_count=0;
      unsigned char inst;
      enum BLOCK_LAYOUT_OPCODE opcode ;
      switch (size) {
         case 3:
            inst = Layout[0];
            opcode = (enum BLOCK_LAYOUT_OPCODE) (inst >> 4);
            if (opcode == BLOCK_LAYOUT_STRONG)
               strong_word_count = (inst & 0xF)+1;
            else
               return 0;
            inst = Layout[1];
            opcode = (enum BLOCK_LAYOUT_OPCODE) (inst >> 4);
            if (opcode == BLOCK_LAYOUT_BYREF)
               byref_word_count = (inst & 0xF)+1;
            else
               return 0;
            inst = Layout[2];
            opcode = (enum BLOCK_LAYOUT_OPCODE) (inst >> 4);
            if (opcode == BLOCK_LAYOUT_WEAK)
               weak_word_count = (inst & 0xF)+1;
            else
               return 0;
            break;

         case 2:
            inst = Layout[0];
            opcode = (enum BLOCK_LAYOUT_OPCODE) (inst >> 4);
            if (opcode == BLOCK_LAYOUT_STRONG) {
               strong_word_count = (inst & 0xF)+1;
               inst = Layout[1];
               opcode = (enum BLOCK_LAYOUT_OPCODE) (inst >> 4);
               if (opcode == BLOCK_LAYOUT_BYREF)
                  byref_word_count = (inst & 0xF)+1;
               else if (opcode == BLOCK_LAYOUT_WEAK)
                  weak_word_count = (inst & 0xF)+1;
               else
                  return 0;
            }
            else if (opcode == BLOCK_LAYOUT_BYREF) {
               byref_word_count = (inst & 0xF)+1;
               inst = Layout[1];
               opcode = (enum BLOCK_LAYOUT_OPCODE) (inst >> 4);
               if (opcode == BLOCK_LAYOUT_WEAK)
                  weak_word_count = (inst & 0xF)+1;
               else
                  return 0;
            }
            else
               return 0;
            break;

         case 1:
            inst = Layout[0];
            opcode = (enum BLOCK_LAYOUT_OPCODE) (inst >> 4);
            if (opcode == BLOCK_LAYOUT_STRONG)
               strong_word_count = (inst & 0xF)+1;
            else if (opcode == BLOCK_LAYOUT_BYREF)
               byref_word_count = (inst & 0xF)+1;
            else if (opcode == BLOCK_LAYOUT_WEAK)
               weak_word_count = (inst & 0xF)+1;
            else
               return 0;
            break;

         default:
            return 0;
      }

      // Cannot inline when any of the word counts is 15. Because this is one less
      // than the actual work count (so 15 means 16 actual word counts),
      // and we can only display 0 thru 15 word counts.
      if (strong_word_count == 16 || byref_word_count == 16 || weak_word_count == 16)
         return 0;

      unsigned count =
      (strong_word_count != 0) + (byref_word_count != 0) + (weak_word_count != 0);

      if (size == count) {
         if (strong_word_count)
            Result = strong_word_count;
         Result <<= 4;
         if (byref_word_count)
            Result += byref_word_count;
         Result <<= 4;
         if (weak_word_count)
            Result += weak_word_count;
      }
   }
   return Result;
}

llvm::Constant *CGObjCCommonMulleRuntime::getBitmapBlockLayout(bool ComputeByrefLayout) {
   llvm::Constant *nullPtr = llvm::Constant::getNullValue(CGM.Int8PtrTy);
   if (RunSkipBlockVars.empty())
      return nullPtr;
   unsigned WordSizeInBits = CGM.getTarget().getPointerWidth(0);
   unsigned ByteSizeInBits = CGM.getTarget().getCharWidth();
   unsigned WordSizeInBytes = WordSizeInBits/ByteSizeInBits;

   // Sort on byte position; captures might not be allocated in order,
   // and unions can do funny things.
   llvm::array_pod_sort(RunSkipBlockVars.begin(), RunSkipBlockVars.end());
   SmallVector<unsigned char, 16> Layout;

   unsigned size = RunSkipBlockVars.size();
   for (unsigned i = 0; i < size; i++) {
      enum BLOCK_LAYOUT_OPCODE opcode = RunSkipBlockVars[i].opcode;
      CharUnits start_byte_pos = RunSkipBlockVars[i].block_var_bytepos;
      CharUnits end_byte_pos = start_byte_pos;
      unsigned j = i+1;
      while (j < size) {
         if (opcode == RunSkipBlockVars[j].opcode) {
            end_byte_pos = RunSkipBlockVars[j++].block_var_bytepos;
            i++;
         }
         else
            break;
      }
      CharUnits size_in_bytes =
      end_byte_pos - start_byte_pos + RunSkipBlockVars[j-1].block_var_size;
      if (j < size) {
         CharUnits gap =
         RunSkipBlockVars[j].block_var_bytepos -
         RunSkipBlockVars[j-1].block_var_bytepos - RunSkipBlockVars[j-1].block_var_size;
         size_in_bytes += gap;
      }
      CharUnits residue_in_bytes = CharUnits::Zero();
      if (opcode == BLOCK_LAYOUT_NON_OBJECT_BYTES) {
         residue_in_bytes = size_in_bytes % WordSizeInBytes;
         size_in_bytes -= residue_in_bytes;
         opcode = BLOCK_LAYOUT_NON_OBJECT_WORDS;
      }

      unsigned size_in_words = size_in_bytes.getQuantity() / WordSizeInBytes;
      while (size_in_words >= 16) {
         // Note that value in imm. is one less that the actual
         // value. So, 0xf means 16 words follow!
         unsigned char inst = (opcode << 4) | 0xf;
         Layout.push_back(inst);
         size_in_words -= 16;
      }
      if (size_in_words > 0) {
         // Note that value in imm. is one less that the actual
         // value. So, we subtract 1 away!
         unsigned char inst = (opcode << 4) | (size_in_words-1);
         Layout.push_back(inst);
      }
      if (residue_in_bytes > CharUnits::Zero()) {
         unsigned char inst =
         (BLOCK_LAYOUT_NON_OBJECT_BYTES << 4) | (residue_in_bytes.getQuantity()-1);
         Layout.push_back(inst);
      }
   }

   int e = Layout.size()-1;
   while (e >= 0) {
      unsigned char inst = Layout[e--];
      enum BLOCK_LAYOUT_OPCODE opcode = (enum BLOCK_LAYOUT_OPCODE) (inst >> 4);
      if (opcode == BLOCK_LAYOUT_NON_OBJECT_BYTES || opcode == BLOCK_LAYOUT_NON_OBJECT_WORDS)
         Layout.pop_back();
      else
         break;
   }

   uint64_t Result = InlineLayoutInstruction(Layout);
   if (Result != 0) {
      // Block variable layout instruction has been inlined.
      if (CGM.getLangOpts().ObjCGCBitmapPrint) {
         if (ComputeByrefLayout)
            printf("\n Inline instruction for BYREF variable layout: ");
         else
            printf("\n Inline instruction for block variable layout: ");
         printf("0x0%" PRIx64 "\n", Result);
      }
      if (WordSizeInBytes == 8) {
         const llvm::APInt Instruction(64, Result);
         return llvm::Constant::getIntegerValue(CGM.Int64Ty, Instruction);
      }
      else {
         const llvm::APInt Instruction(32, Result);
         return llvm::Constant::getIntegerValue(CGM.Int32Ty, Instruction);
      }
   }

   unsigned char inst = (BLOCK_LAYOUT_OPERATOR << 4) | 0;
   Layout.push_back(inst);
   std::string BitMap;
   for (unsigned i = 0, e = Layout.size(); i != e; i++)
      BitMap += Layout[i];

   if (CGM.getLangOpts().ObjCGCBitmapPrint) {
      if (ComputeByrefLayout)
         printf("\n BYREF variable layout: ");
      else
         printf("\n block variable layout: ");
      for (unsigned i = 0, e = BitMap.size(); i != e; i++) {
         unsigned char inst = BitMap[i];
         enum BLOCK_LAYOUT_OPCODE opcode = (enum BLOCK_LAYOUT_OPCODE) (inst >> 4);
         unsigned delta = 1;
         switch (opcode) {
            case BLOCK_LAYOUT_OPERATOR:
               printf("BL_OPERATOR:");
               delta = 0;
               break;
            case BLOCK_LAYOUT_NON_OBJECT_BYTES:
               printf("BL_NON_OBJECT_BYTES:");
               break;
            case BLOCK_LAYOUT_NON_OBJECT_WORDS:
               printf("BL_NON_OBJECT_WORD:");
               break;
            case BLOCK_LAYOUT_STRONG:
               printf("BL_STRONG:");
               break;
            case BLOCK_LAYOUT_BYREF:
               printf("BL_BYREF:");
               break;
            case BLOCK_LAYOUT_WEAK:
               printf("BL_WEAK:");
               break;
            case BLOCK_LAYOUT_UNRETAINED:
               printf("BL_UNRETAINED:");
               break;
         }
         // Actual value of word count is one more that what is in the imm.
         // field of the instruction
         printf("%d", (inst & 0xf) + delta);
         if (i < e-1)
            printf(", ");
         else
            printf("\n");
      }
   }

   llvm::GlobalVariable *Entry = CreateMetadataVar(
                                                   "OBJC_CLASS_NAME_",
                                                   llvm::ConstantDataArray::getString(VMContext, BitMap, false),
                                                   "__TEXT,__objc_classname,cstring_literals", 1, true);
   return getConstantGEP(VMContext, Entry, 0, 0);
}

llvm::Constant *CGObjCCommonMulleRuntime::BuildRCBlockLayout(CodeGenModule &CGM,
                                                    const CGBlockInfo &blockInfo) {
   assert(CGM.getLangOpts().getGC() == LangOptions::NonGC);

   RunSkipBlockVars.clear();
   bool hasUnion = false;

   unsigned WordSizeInBits = CGM.getTarget().getPointerWidth(0);
   unsigned ByteSizeInBits = CGM.getTarget().getCharWidth();
   unsigned WordSizeInBytes = WordSizeInBits/ByteSizeInBits;

   const BlockDecl *blockDecl = blockInfo.getBlockDecl();

   // Calculate the basic layout of the block structure.
   const llvm::StructLayout *layout =
   CGM.getDataLayout().getStructLayout(blockInfo.StructureType);

   // Ignore the optional 'this' capture: C++ objects are not assumed
   // to be GC'ed.
   if (blockInfo.BlockHeaderForcedGapSize != CharUnits::Zero())
      UpdateRunSkipBlockVars(false, Qualifiers::OCL_None,
                             blockInfo.BlockHeaderForcedGapOffset,
                             blockInfo.BlockHeaderForcedGapSize);
   // Walk the captured variables.
   for (const auto &CI : blockDecl->captures()) {
      const VarDecl *variable = CI.getVariable();
      QualType type = variable->getType();

      const CGBlockInfo::Capture &capture = blockInfo.getCapture(variable);

      // Ignore constant captures.
      if (capture.isConstant()) continue;

      CharUnits fieldOffset =
      CharUnits::fromQuantity(layout->getElementOffset(capture.getIndex()));

      assert(!type->isArrayType() && "array variable should not be caught");
      if (!CI.isByRef())
         if (const RecordType *record = type->getAs<RecordType>()) {
            BuildRCBlockVarRecordLayout(record, fieldOffset, hasUnion);
            continue;
         }
      CharUnits fieldSize;
      if (CI.isByRef())
         fieldSize = CharUnits::fromQuantity(WordSizeInBytes);
      else
         fieldSize = CGM.getContext().getTypeSizeInChars(type);
      UpdateRunSkipBlockVars(CI.isByRef(), getBlockCaptureLifetime(type, false),
                             fieldOffset, fieldSize);
   }
   return getBitmapBlockLayout(false);
}


llvm::Constant *CGObjCCommonMulleRuntime::BuildByrefLayout(CodeGen::CodeGenModule &CGM,
                                                  QualType T) {
   assert(CGM.getLangOpts().getGC() == LangOptions::NonGC);
   assert(!T->isArrayType() && "__block array variable should not be caught");
   CharUnits fieldOffset;
   RunSkipBlockVars.clear();
   bool hasUnion = false;
   if (const RecordType *record = T->getAs<RecordType>()) {
      BuildRCBlockVarRecordLayout(record, fieldOffset, hasUnion, true /*ByrefLayout */);
      llvm::Constant *Result = getBitmapBlockLayout(true);
      return Result;
   }
   llvm::Constant *nullPtr = llvm::Constant::getNullValue(CGM.Int8PtrTy);
   return nullPtr;
}

# pragma mark -
# pragma mark protocols

llvm::Value *CGObjCMulleRuntime::GenerateProtocolRef(CodeGenFunction &CGF,
                                            const ObjCProtocolDecl *PD) {
   // FIXME: I don't understand why gcc generates this, or where it is
   // resolved. Investigate. Its also wasteful to look this up over and over.
   //LazySymbols.insert(&CGM.getContext().Idents.get("Protocol"));

   return llvm::ConstantExpr::getBitCast(GetOrEmitProtocol(PD),
                                         ObjCTypes.ProtocolIDTy);
}

void CGObjCCommonMulleRuntime::GenerateProtocol(const ObjCProtocolDecl *PD) {
   // FIXME: We shouldn't need this, the protocol decl should contain enough
   // information to tell us whether this was a declaration or a definition.
   DefinedProtocols.insert(PD->getIdentifier());

   // If we have generated a forward reference to this protocol, emit
   // it now. Otherwise do nothing, the protocol objects are lazily
   // emitted.
   if (Protocols.count(PD->getIdentifier()))
      GetOrEmitProtocol(PD);
}

llvm::Constant *CGObjCCommonMulleRuntime::GetProtocolRef(const ObjCProtocolDecl *PD) {
   if (DefinedProtocols.count(PD->getIdentifier()))
      return GetOrEmitProtocol(PD);

   return GetOrEmitProtocolRef(PD);
}



llvm::Constant *CGObjCMulleRuntime::GetOrEmitProtocol(const ObjCProtocolDecl *PD)
{
   return( HashProtocolConstantForString( PD->getIdentifier()->getName()));
}


llvm::Constant *CGObjCMulleRuntime::GetOrEmitProtocolRef(const ObjCProtocolDecl *PD)
{
   return( HashProtocolConstantForString( PD->getIdentifier()->getName()));
}


/*
 * in our uniform structs, the hash is always the first
 * entry in the struct
 */
static int constant_uintptr_comparator( llvm::Constant * const *P1,
                                       llvm::Constant * const *P2)
{
   llvm::APInt   valueA;
   llvm::APInt   valueB;

   valueA = (*P1)->getUniqueInteger();
   valueB = (*P2)->getUniqueInteger();

   if( valueA == valueB)
      return 0;
   return( valueA.ult( valueB) ? -1 : 1);
}


/*
   just a list of protocol IDs,
   which gets stuck on a category or on a class
 */
llvm::Constant *
CGObjCMulleRuntime::EmitProtocolIDList(Twine Name,
                            ObjCProtocolDecl::protocol_iterator begin,
                            ObjCProtocolDecl::protocol_iterator end)
{
   SmallVector<llvm::Constant *, 16> ProtocolRefs;

   for (; begin != end; ++begin)
      ProtocolRefs.push_back(GetProtocolRef(*begin));

   // Just return null for empty protocol lists
   if (ProtocolRefs.empty())
      return llvm::Constant::getNullValue(ObjCTypes.ProtocolIDPtrTy);

   llvm::array_pod_sort( ProtocolRefs.begin(), ProtocolRefs.end(),
                         constant_uintptr_comparator);

   // This list is null terminated.
   ProtocolRefs.push_back(llvm::Constant::getNullValue(ObjCTypes.ProtocolIDTy));

   llvm::Constant *Values[ 1];
   Values[ 0] = llvm::ConstantArray::get(llvm::ArrayType::get(ObjCTypes.ProtocolIDTy,
                                                              ProtocolRefs.size()),
                                         ProtocolRefs);

   llvm::Constant       *Init = llvm::ConstantStruct::getAnon(Values);
   llvm::GlobalVariable *GV =
   CreateMetadataVar(Name, Init, "__DATA,__protocolids,regular,no_dead_strip",
                     4, false);
   return llvm::ConstantExpr::getBitCast( GV, ObjCTypes.ProtocolIDPtrTy);
}




/*
 just a list of class IDs. Get protocols passed in, then determine if that
 protocol has a class of the same name. @class Foo;  is sufficient if we
 have @protocol Foo;
 */
llvm::Constant *
CGObjCMulleRuntime::EmitProtocolClassIDList(Twine Name,
                                            ObjCProtocolDecl::protocol_iterator begin,
                                            ObjCProtocolDecl::protocol_iterator end)
{
   SmallVector<llvm::Constant *, 16> ClassIds;
   llvm::Constant       *classID;
   StringRef            name;
   StringRef            otherName;
   
   for (; begin != end; ++begin)
   {
      name = (*begin)->getName();
      if( DeclaredClassNames.find( name.str()) != DeclaredClassNames.end())
      {
         classID = HashClassConstantForString( name);
         ClassIds.push_back( classID);
         break;
      }
   }

   // Just return null for empty protocol lists
   if (ClassIds.empty())
      return llvm::Constant::getNullValue(ObjCTypes.ClassIDPtrTy);
   
   llvm::array_pod_sort( ClassIds.begin(), ClassIds.end(),
                        constant_uintptr_comparator);
   
   // This list is null terminated.
   ClassIds.push_back(llvm::Constant::getNullValue(ObjCTypes.ClassIDTy));
   
   llvm::Constant *Values[ 1];
   Values[ 0] = llvm::ConstantArray::get(llvm::ArrayType::get(ObjCTypes.ClassIDTy,
                                                              ClassIds.size()),
                                         ClassIds);
   
   llvm::Constant       *Init = llvm::ConstantStruct::getAnon(Values);
   llvm::GlobalVariable *GV =
   CreateMetadataVar(Name, Init, "__DATA,__protoclassids,regular,no_dead_strip",
                     4, false);
   return llvm::ConstantExpr::getBitCast( GV, ObjCTypes.ClassIDPtrTy);
}


void CGObjCCommonMulleRuntime::
PushProtocolProperties(llvm::SmallPtrSet<const IdentifierInfo*,16> &PropertySet,
                       SmallVectorImpl<llvm::Constant *> &Properties,
                       const Decl *Container,
                       const ObjCProtocolDecl *Proto,
                       const ObjCCommonTypesHelper &ObjCTypes) {
   for (const auto *P : Proto->protocols())
      PushProtocolProperties(PropertySet, Properties, Container, P, ObjCTypes);
   for (const auto *PD : Proto->properties()) {
      if (!PropertySet.insert(PD->getIdentifier()).second)
         continue;

      llvm::Constant *Prop[6];
      SetPropertyInfoToEmit( PD, Container, Prop);

      Properties.push_back(llvm::ConstantStruct::get(ObjCTypes.PropertyTy, Prop));
   }
}

/*
 * in our uniform structs, the hash is always the first
 * entry in the struct
 */
static int uniqueid_comparator( llvm::Constant * const *P1,
                                llvm::Constant * const *P2)
{
   llvm::ConstantStruct   *methodA;
   llvm::ConstantStruct   *methodB;
   llvm::ConstantInt      *hashA;
   llvm::ConstantInt      *hashB;
   llvm::APInt            valueA;
   llvm::APInt            valueB;
   bool                   flag;

   methodA = (llvm::ConstantStruct *) *P1;
   methodB = (llvm::ConstantStruct *) *P2;

   hashA = dyn_cast< llvm::ConstantInt>( methodA->getAggregateElement( 0U));
   hashB = dyn_cast< llvm::ConstantInt>( methodB->getAggregateElement( 0U));

   valueA = hashA->getUniqueInteger();
   valueB = hashB->getUniqueInteger();

   if( valueA == valueB)
      return 0;

   flag = valueA.ult( valueB);
   return( flag ? -1 : +1);
}


void  CGObjCCommonMulleRuntime::SetPropertyInfoToEmit( const ObjCPropertyDecl *PD,
                                                       const Decl *Container,
                                                       llvm::Constant *Prop[ 6])
{
   Selector   getter = PD->getGetterName();
   Selector   setter = PD->getSetterName();
   QualType   type;
   int        is_nonnull;
   const llvm::APInt zero(32, 0);
   llvm::Constant  *zeroSel  = llvm::Constant::getIntegerValue(CGM.Int32Ty, zero);

   Prop[ 0] = HashPropertyConstantForString( PD->getIdentifier()->getNameStart());
   Prop[ 1] = GetPropertyName(PD->getIdentifier());
   Prop[ 2] = GetPropertyTypeString(PD, Container);
   Prop[ 3] = ! getter.isNull() ? HashSelConstantForString( getter.getAsString())
                                : zeroSel;
   Prop[ 4] = (! setter.isNull() && ! PD->isReadOnly())
                ? HashSelConstantForString( setter.getAsString())
                : zeroSel;
   Prop[ 5] = zeroSel;

   // if its a pointer and not nonnull, we can clear it
   type       = PD->getType();
   is_nonnull = PD->getPropertyAttributes() & ObjCPropertyDecl::OBJC_PR_nullability;


   Prop[ 5] =  type->hasPointerRepresentation() && ! is_nonnull ? Prop[ 4] : zeroSel;

//   fprintf( stderr, "%s %s has %s clearer\n",  type->hasPointerRepresentation() ? "pointer" : "nonpointer", PD->getIdentifier()->getNameStart(),  Prop[ 5] == zeroSel  ? "no" : "a");
}


llvm::Constant *CGObjCCommonMulleRuntime::EmitPropertyList(Twine Name,
                                                  const Decl *Container,
                                                  const ObjCContainerDecl *OCD,
                                                  const ObjCCommonTypesHelper &ObjCTypes) {
   SmallVector<llvm::Constant *, 16> Properties;
   llvm::SmallPtrSet<const IdentifierInfo*, 16> PropertySet;
   for (const auto *PD : OCD->properties())
   {
      llvm::Constant *Prop[6];

      PropertySet.insert(PD->getIdentifier());

      SetPropertyInfoToEmit( PD, Container, Prop);
      Properties.push_back(llvm::ConstantStruct::get(ObjCTypes.PropertyTy,
                                                     Prop));
   }
   if (const ObjCInterfaceDecl *OID = dyn_cast<ObjCInterfaceDecl>(OCD)) {
      for (const auto *P : OID->all_referenced_protocols())
         PushProtocolProperties(PropertySet, Properties, Container, P, ObjCTypes);
   }
   else if (const ObjCCategoryDecl *CD = dyn_cast<ObjCCategoryDecl>(OCD)) {
      for (const auto *P : CD->protocols())
         PushProtocolProperties(PropertySet, Properties, Container, P, ObjCTypes);
   }

   // Return null for empty list.
   if (Properties.empty())
      return llvm::Constant::getNullValue(ObjCTypes.PropertyListPtrTy);

   // emit properties sorted by id
   llvm::array_pod_sort( Properties.begin(), Properties.end(),
                         uniqueid_comparator);

   llvm::Constant *Values[2];
   Values[0] = llvm::ConstantInt::get(ObjCTypes.IntTy, Properties.size());
   llvm::ArrayType *AT = llvm::ArrayType::get(ObjCTypes.PropertyTy,
                                              Properties.size());
   Values[1] = llvm::ConstantArray::get(AT, Properties);
   llvm::Constant *Init = llvm::ConstantStruct::getAnon(Values);

   llvm::GlobalVariable *GV =
   CreateMetadataVar(Name, Init,
                     (ObjCABI == 2) ? "__DATA, __objc_const" :
                     "__DATA,__property,regular,no_dead_strip",
                     (ObjCABI == 2) ? 8 : 4,
                     true);
   return llvm::ConstantExpr::getBitCast(GV, ObjCTypes.PropertyListPtrTy);
}

llvm::Constant *
CGObjCCommonMulleRuntime::EmitProtocolMethodTypes(Twine Name,
                                         ArrayRef<llvm::Constant*> MethodTypes,
                                         const ObjCCommonTypesHelper &ObjCTypes) {
   // Return null for empty list.
   if (MethodTypes.empty())
      return llvm::Constant::getNullValue(ObjCTypes.Int8PtrPtrTy);

   llvm::ArrayType *AT = llvm::ArrayType::get(ObjCTypes.Int8PtrTy,
                                              MethodTypes.size());
   llvm::Constant *Init = llvm::ConstantArray::get(AT, MethodTypes);

   llvm::GlobalVariable *GV = CreateMetadataVar(
                                                Name, Init, (ObjCABI == 2) ? "__DATA, __objc_const" : StringRef(),
                                                (ObjCABI == 2) ? 8 : 4, true);
   return llvm::ConstantExpr::getBitCast(GV, ObjCTypes.Int8PtrPtrTy);
}

# pragma mark -
# pragma mark method descriptions

/*
 struct objc_method_description_list {
 int count;
 struct objc_method_description list[];
 };
 */
llvm::Constant *
CGObjCMulleRuntime::GetMethodDescriptionConstant(const ObjCMethodDecl *MD) {
   llvm::Constant *Desc[] = {
      llvm::ConstantExpr::getBitCast(GetMethodVarName(MD->getSelector()),
                                     ObjCTypes.SelectorIDTy),
      GetMethodVarType(MD)
   };
   if (!Desc[1])
      return nullptr;

   return llvm::ConstantStruct::get(ObjCTypes.MethodDescriptionTy,
                                    Desc);
}

llvm::Constant *
CGObjCMulleRuntime::EmitMethodDescList(Twine Name, const char *Section,
                              ArrayRef<llvm::Constant*> Methods) {
   // Return null for empty list.
   if (Methods.empty())
      return llvm::Constant::getNullValue(ObjCTypes.MethodDescriptionListPtrTy);

   llvm::Constant *Values[2];
   Values[0] = llvm::ConstantInt::get(ObjCTypes.IntTy, Methods.size());
   llvm::ArrayType *AT = llvm::ArrayType::get(ObjCTypes.MethodDescriptionTy,
                                              Methods.size());
   Values[1] = llvm::ConstantArray::get(AT, Methods);
   llvm::Constant *Init = llvm::ConstantStruct::getAnon(Values);

   llvm::GlobalVariable *GV = CreateMetadataVar(Name, Init, Section, 4, true);
   return llvm::ConstantExpr::getBitCast(GV,
                                         ObjCTypes.MethodDescriptionListPtrTy);
}


//   struct _mulle_objc_loadcategory
//   {
//      char                             *categoryname;
//      mulle_objc_class_id_t            classid;
//      char                             *classname;         // useful ??
//      uintptr_t                        classivarhash;
//
//      struct _mulle_objc_method_list   *classmethods;
//      struct _mulle_objc_method_list   *instancemethods;
//      struct _mulle_objc_propertylist  *properties;
//
//      mulle_objc_protocolid_t          *protocolids;
//      mulle_objc_classid_t             *protocolclassids;
//   };
void CGObjCMulleRuntime::GenerateCategory(const ObjCCategoryImplDecl *OCD) {
//   unsigned Size = CGM.getDataLayout().getTypeAllocSize(ObjCTypes.CategoryTy);

   // FIXME: This is poor design, the OCD should have a pointer to the category
   // decl. Additionally, note that Category can be null for the @implementation
   // w/o an @interface case. Sema should just create one for us as it does for
   // @implementation so everyone else can live life under a clear blue sky.
   const ObjCInterfaceDecl *Interface = OCD->getClassInterface();
   const ObjCCategoryDecl *Category =
   Interface->FindCategoryDeclaration(OCD->getIdentifier());

   SmallString<256> ExtName;
   llvm::raw_svector_ostream(ExtName) << Interface->getName() << '_'
   << OCD->getName();

   SmallVector<llvm::Constant *, 16> InstanceMethods, ClassMethods;
   for (const auto *I : OCD->instance_methods())
      // Instance methods should always be defined.
      InstanceMethods.push_back(GetMethodConstant(I));
   llvm::array_pod_sort( InstanceMethods.begin(), InstanceMethods.end(),
                         uniqueid_comparator);

   for (const auto *I : OCD->class_methods())
      // Class methods should always be defined.
      ClassMethods.push_back(GetMethodConstant(I));
   llvm::array_pod_sort( ClassMethods.begin(), ClassMethods.end(),
                         uniqueid_comparator);

   llvm::Constant *Values[10];

   // category name emitted below

   Values[ 2] = HashClassConstantForString( Interface->getName());
   Values[ 3] = GetClassName(Interface->getObjCRuntimeNameAsString());
   LazySymbols.insert(Interface->getIdentifier());

   Values[ 4] = llvm::ConstantExpr::getBitCast( _HashConstantForString( Interface->getIvarHashString( CGM.getContext()), 0), ObjCTypes.ClassIDTy);;

   Values[ 5] = EmitMethodList("OBJC_CATEGORY_CLASS_METHODS_" + ExtName.str(),
                              "__DATA,__cat_cls_meth,regular,no_dead_strip",
                              ClassMethods);
   Values[ 6] = EmitMethodList("OBJC_CATEGORY_INSTANCE_METHODS_" + ExtName.str(),
                              "__DATA,__cat_inst_meth,regular,no_dead_strip",
                              InstanceMethods);

   // If there is no category @interface then there can be no properties.
   if (Category)
   {
      Values[ 0] = llvm::ConstantExpr::getBitCast( _HashConstantForString( Category->getName(), 0), ObjCTypes.ClassIDTy);
      Values[ 1] = GetClassName(Category->getName());

      Values[ 7] = EmitPropertyList("OBJC_CATEGORY_PROP_LIST_" + ExtName.str(),
                                   OCD, Category, ObjCTypes);
      Values[ 8] =
      EmitProtocolIDList("OBJC_CATEGORY_PROTOCOLS_" + ExtName.str(),
                         Category->protocol_begin(), Category->protocol_end());
      Values[ 9] =
      EmitProtocolClassIDList("OBJC_CATEGORY_PROTOCOLCLASSES_" + ExtName.str(),
                               Category->protocol_begin(), Category->protocol_end());
   }
   else
   {
      Values[ 0] = llvm::ConstantExpr::getBitCast( _HashConstantForString( OCD->getName(), 0), ObjCTypes.ClassIDTy);
      Values[ 1] = GetClassName( OCD->getName());

      Values[ 7] = llvm::Constant::getNullValue(ObjCTypes.PropertyListPtrTy);
      Values[ 8] = llvm::Constant::getNullValue(ObjCTypes.ProtocolListPtrTy);
      Values[ 9] = llvm::Constant::getNullValue(ObjCTypes.ProtocolListPtrTy);
   }

   llvm::Constant *Init = llvm::ConstantStruct::get(ObjCTypes.CategoryTy,
                                                    Values);

   llvm::GlobalVariable *GV =
   CreateMetadataVar("OBJC_CATEGORY_" + ExtName.str(), Init,
                     "__DATA,__category,regular,no_dead_strip", 4, true);
   DefinedCategories.push_back(GV);
   DefinedCategoryNames.insert(ExtName.str());
   // method definition entries must be clear for next implementation.
   MethodDefinitions.clear();
}


# pragma mark -
# pragma mark class


enum FragileClassFlags {
   FragileABI_Class_Factory                 = 0x00001,
   FragileABI_Class_Meta                    = 0x00002,
   FragileABI_Class_HasCXXStructors         = 0x02000,
   FragileABI_Class_Hidden                  = 0x20000
};


void CGObjCMulleRuntime::GenerateClass(const ObjCImplementationDecl *ID) {
   DefinedSymbols.insert(ID->getIdentifier());

   std::string ClassName = ID->getNameAsString();
   // FIXME: Gross
   ObjCInterfaceDecl *Interface =
   const_cast<ObjCInterfaceDecl*>(ID->getClassInterface());
   llvm::Constant *Protocols =
   EmitProtocolIDList("OBJC_CLASS_PROTOCOLS_" + ID->getName(),
                    Interface->all_referenced_protocol_begin(),
                    Interface->all_referenced_protocol_end());
   llvm::Constant *ProtocolClasses =
   EmitProtocolClassIDList("OBJC_CLASS_PROTOCOLCLASSES_" + ID->getName(),
                      Interface->all_referenced_protocol_begin(),
                      Interface->all_referenced_protocol_end());
   unsigned Flags = FragileABI_Class_Factory;
   if (ID->hasNonZeroConstructors() || ID->hasDestructors())
      Flags |= FragileABI_Class_HasCXXStructors;
   unsigned Size =
   CGM.getContext().getASTObjCImplementationLayout(ID).getSize().getQuantity();

   // FIXME: Set CXX-structors flag.
   if (ID->getClassInterface()->getVisibility() == HiddenVisibility)
      Flags |= FragileABI_Class_Hidden;

   SmallVector<llvm::Constant *, 16> InstanceMethods, ClassMethods, InstanceVariables, Properties;

  const ObjCInterfaceDecl *OID = ID->getClassInterface();

  for (const ObjCIvarDecl *IVD = OID->all_declared_ivar_begin();
       IVD; IVD = IVD->getNextIvar())
  {
    if (!IVD->getDeclName())
      continue;
    InstanceVariables.push_back( GetIvarConstant( OID, IVD));
  }
   llvm::array_pod_sort( InstanceVariables.begin(), InstanceVariables.end(),
                         uniqueid_comparator);

   for (const auto *I : ID->class_methods())
      // Class methods should always be defined.
      ClassMethods.push_back(GetMethodConstant(I));
   llvm::array_pod_sort( ClassMethods.begin(), ClassMethods.end(),
                         uniqueid_comparator);

   for (const auto *I : ID->instance_methods())
      // Instance methods should always be defined.
      InstanceMethods.push_back(GetMethodConstant(I));

   for (const auto *PID : ID->property_impls()) {
      if (PID->getPropertyImplementation() == ObjCPropertyImplDecl::Synthesize) {
         ObjCPropertyDecl *PD = PID->getPropertyDecl();
         if (ObjCMethodDecl *MD = PD->getGetterMethodDecl())
            if (llvm::Constant *C = GetMethodConstant(MD))
               InstanceMethods.push_back(C);
         if (ObjCMethodDecl *MD = PD->getSetterMethodDecl())
            if (llvm::Constant *C = GetMethodConstant(MD))
               InstanceMethods.push_back(C);
      }
   }

   llvm::array_pod_sort( InstanceMethods.begin(), InstanceMethods.end(),
                         uniqueid_comparator);


//   struct _mulle_objc_loadclass
//   {
//      mulle_objc_classid_t              classid;
//      char                              *classname;
//      mulle_objc_hash_t                 classivarhash;
//      
//      mulle_objc_classid_t              superclassid;
//      char                              *superclassname;
//      mulle_objc_hash_t                 superclassivarhash;
//      
//      int                               fastclassindex;
//      int                               instancesize;
//      
//      struct _mulle_objc_ivarlist       *instancevariables;
//      
//      struct _mulle_objc_methodlist     *classmethods;
//      struct _mulle_objc_methodlist     *instancemethods;
//      struct _mulle_objc_propertylist   *properties;
//      
//      mulle_objc_protocolid_t           *protocolids;
//      mulle_objc_classid_t              *protocolclassids;
//   };

   llvm::Constant *Values[14];
   int   i = 0;

   ObjCInterfaceDecl *Super = Interface->getSuperClass();
   llvm::ConstantInt *ClassID = HashClassConstantForString( ID->getObjCRuntimeNameAsString());

   Values[ i++] = llvm::ConstantExpr::getBitCast( ClassID, ObjCTypes.ClassIDTy);
   Values[ i++] = GetClassName( ID->getObjCRuntimeNameAsString());
   Values[ i++] = llvm::ConstantExpr::getBitCast( _HashConstantForString( OID->getIvarHashString( CGM.getContext()), 0), ObjCTypes.ClassIDTy);;

   llvm::ConstantInt   *SuperClassID = nullptr;
   if( Super)
   {
      SuperClassID = HashClassConstantForString( Super->getObjCRuntimeNameAsString());
      Values[ i++] = llvm::ConstantExpr::getBitCast( SuperClassID, ObjCTypes.ClassIDTy);
      Values[ i++] = GetClassName( Super->getObjCRuntimeNameAsString());
      Values[ i++] = llvm::ConstantExpr::getBitCast( _HashConstantForString( Super->getIvarHashString( CGM.getContext()), 0), ObjCTypes.ClassIDTy);;
   }
   else
   {
      Values[ i++] = llvm::Constant::getNullValue(ObjCTypes.ClassIDTy);
      Values[ i++] = llvm::Constant::getNullValue(ObjCTypes.Int8PtrTy);
      Values[ i++] = llvm::Constant::getNullValue(ObjCTypes.ClassIDTy);
   }

   // determine fastclass index
   if( fastclassids_defined)
   {
      int        j;
      // @mulle-objc@ uniqueid: make it 32 bit here
      uint32_t   uniqueid;

      // the ClassID is possibly 32 bit though, so we have to rehash here
      uniqueid = UniqueidHashForString(ID->getObjCRuntimeNameAsString(), 1, 4);
      for( j = 31; j >= 0; j--)
      {
         if( fastclassids[ j] == uniqueid)
            break;
      }
      Values[ i++] = llvm::ConstantInt::get(ObjCTypes.IntTy, j);
      if( _trace_fastids && j >= 0)
         fprintf( stderr, "%s is a fastclass with id 0x%llx\n",
               ID->getNameAsString().c_str(), (long long) uniqueid);
   }
   else
      Values[ i++] = llvm::ConstantInt::get(ObjCTypes.IntTy, -1);

   Values[ i++] = llvm::ConstantInt::get(ObjCTypes.IntTy, Size);

   Values[ i++] = EmitIvarList( ID, InstanceVariables, false);

   Values[ i++] = EmitMethodList("OBJC_CLASS_METHODS_" + ID->getNameAsString(),
                                 "_DATA,__cls_meth,regular,no_dead_strip", ClassMethods);
   Values[ i++] = EmitMethodList("OBJC_CLASS_METHODS_" + ID->getNameAsString(),
                               "_DATA,__inst_meth,regular,no_dead_strip", InstanceMethods);
   Values[ i++] = EmitPropertyList("OBJC_CLASS_METHODS_" + ID->getNameAsString(),
                               ID, OID, ObjCTypes);
   Values[ i++] = Protocols;
   Values[ i++] = ProtocolClasses;

   assert( i == sizeof( Values) / sizeof( llvm::Constant *));

   llvm::Constant *Init = llvm::ConstantStruct::get(ObjCTypes.ClassTy,
                                                    Values);
   std::string Name("OBJC_CLASS_");
   Name += ClassName;

   // cargo cult programming
   const char *Section = "__DATA,__class,regular,no_dead_strip";
   // Check for a forward reference.
   llvm::GlobalVariable *GV = CGM.getModule().getGlobalVariable(Name, true);
   if (GV) {
      assert(GV->getType()->getElementType() == ObjCTypes.ClassTy &&
             "Forward metaclass reference has incorrect type.");
      GV->setInitializer(Init);
      GV->setSection(Section);
      GV->setAlignment(4);
      CGM.addCompilerUsedGlobal(GV);
   } else
      GV = CreateMetadataVar(Name, Init, Section, 4, true);

   DefinedClasses.push_back(GV);
   ImplementedClasses.push_back(Interface);

   // method definition entries must be clear for next implementation.
   MethodDefinitions.clear();
}


void   CGObjCMulleRuntime::GenerateForwardClass(const ObjCInterfaceDecl *OID)
{
   StringRef   name;
   
   name = OID->getName();
   DeclaredClassNames.insert( name.str());
}


/*
struct _mulle_objc_ivar_descriptor
{
   mulle_objc_ivar_id_t   ivar_id;
   char                   *name;
   char                   *type;
};

struct _mulle_objc_ivar
{
   struct _mulle_objc_ivar_descriptor   descriptor;
   size_t                               offset;  // if == -1 : hashed access (future)
};


struct _mulle_objc_ivar_list
{
   unsigned int ivar_count;
   struct _mulle_objc_ivar list[count];
};
*/
llvm::Constant *CGObjCMulleRuntime::EmitIvarList(const ObjCImplementationDecl *ID,
                                                 ArrayRef<llvm::Constant*> Ivars,
                                        bool ForwardClass) {
   // When emitting the root class GCC emits ivar entries for the
   // actual class structure. It is not clear if we need to follow this
   // behavior; for now lets try and get away with not doing it. If so,
   // the cleanest solution would be to make up an ObjCInterfaceDecl
   // for the class.
   if (ForwardClass)
      return llvm::Constant::getNullValue(ObjCTypes.IvarListPtrTy);


   // Return null for empty list.
   if (Ivars.empty())
      return llvm::Constant::getNullValue(ObjCTypes.IvarListPtrTy);

   llvm::Constant *Values[2];
   Values[0] = llvm::ConstantInt::get(ObjCTypes.IntTy, Ivars.size());
   llvm::ArrayType *AT = llvm::ArrayType::get(ObjCTypes.IvarTy,
                                              Ivars.size());
   Values[1] = llvm::ConstantArray::get(AT, Ivars);
   llvm::Constant *Init = llvm::ConstantStruct::getAnon(Values);

   llvm::GlobalVariable *GV;

   GV = CreateMetadataVar("OBJC_INSTANCE_VARIABLES_" + ID->getName(), Init,
                             "__DATA,__instance_vars,regular,no_dead_strip", 4,
                             true);
   return llvm::ConstantExpr::getBitCast(GV, ObjCTypes.IvarListPtrTy);
}



llvm::Constant *CGObjCMulleRuntime::GetIvarConstant( const ObjCInterfaceDecl *OID,
                                                     const ObjCIvarDecl *IVD)
{
   llvm::Constant *Ivar[] = {
      llvm::ConstantExpr::getBitCast( HashIvarConstantForString( IVD->getNameAsString()),
                                      ObjCTypes.SelectorIDTy),
      GetIvarName( IVD),
      GetIvarType(IVD),
      llvm::ConstantInt::get(ObjCTypes.IntTy,
                             ComputeIvarBaseOffset(CGM, OID, IVD))
   };
   return llvm::ConstantStruct::get(ObjCTypes.IvarTy, Ivar);
}

/*
  struct _mulle_objc_method_descriptor
  {
      mulle_objc_method_id_t   method_id;
      char                     *name;
      char                     *signature;
      unsigned int             bits;
};

 struct objc_method_list {
 struct objc_method_list *obsolete;
 int count;
 struct objc_method methods_list[count];
 };
 */

/// GetMethodConstant - Return a struct objc_method constant for the
/// given method if it has been defined. The result is null if the
/// method has not been defined. The return value has type MethodPtrTy.
llvm::Constant *CGObjCMulleRuntime::GetMethodConstant(const ObjCMethodDecl *MD) {
   llvm::Function *Fn = GetMethodDefinition(MD);
   if (!Fn)
      return nullptr;

   int   bits;

   // every method remembers if it's been written in aaomode
   bits  = CGM.getLangOpts().ObjCAllocsAutoreleasedObjects ? 0x4 : 0x0;
   // remember if it is variadic
   bits |= MD->isVariadic() ? 0x8 : 0x0;
   // also remember method family (nice for checking if it's init or something)
   bits |= MD->getMethodFamily() << 16;

   llvm::Constant *Method[] = {
      llvm::ConstantExpr::getBitCast( HashSelConstantForString( MD->getSelector().getAsString()),
                                       ObjCTypes.SelectorIDTy),
      llvm::ConstantExpr::getBitCast(GetMethodVarName(MD->getSelector()),
                                     ObjCTypes.Int8PtrTy),
      GetMethodVarType(MD),
      llvm::ConstantInt::get(ObjCTypes.IntTy, bits),

      llvm::ConstantExpr::getBitCast(Fn, ObjCTypes.Int8PtrTy)
   };
   return llvm::ConstantStruct::get(ObjCTypes.MethodTy, Method);
}


llvm::Constant *CGObjCMulleRuntime::EmitMethodList(Twine Name,
                                          const char *Section,
                                          ArrayRef<llvm::Constant*> Methods) {
   // Return null for empty list.
   if (Methods.empty())
      return llvm::Constant::getNullValue(ObjCTypes.MethodListPtrTy);

   llvm::Constant *Values[3];
   Values[0] = llvm::ConstantInt::get(ObjCTypes.IntTy, Methods.size());
   llvm::ArrayType *AT = llvm::ArrayType::get(ObjCTypes.MethodTy,
                                              Methods.size());

   Values[1] = llvm::Constant::getNullValue( ObjCTypes.Int8PtrTy);

   Values[2] = llvm::ConstantArray::get(AT, Methods);
   llvm::Constant *Init = llvm::ConstantStruct::getAnon(Values);

   llvm::GlobalVariable *GV = CreateMetadataVar(Name, Init, Section, 4, true);
   return llvm::ConstantExpr::getBitCast(GV, ObjCTypes.MethodListPtrTy);
}

llvm::Function *CGObjCCommonMulleRuntime::GenerateMethod(const ObjCMethodDecl *OMD,
                                                const ObjCContainerDecl *CD) {
   SmallString<256> Name;
   GetNameForMethod(OMD, CD, Name);

   CodeGenTypes &Types = CGM.getTypes();
   llvm::FunctionType *MethodTy =
   Types.GetFunctionType(Types.arrangeObjCMethodDeclaration(OMD));
   llvm::Function *Method =
   llvm::Function::Create(MethodTy,
                          llvm::GlobalValue::InternalLinkage,
                          Name.str(),
                          &CGM.getModule());
   MethodDefinitions.insert(std::make_pair(OMD, Method));

   return Method;
}

llvm::GlobalVariable *CGObjCCommonMulleRuntime::CreateMetadataVar(Twine Name,
                                                         llvm::Constant *Init,
                                                         StringRef Section,
                                                         unsigned Align,
                                                         bool AddToUsed) {
   llvm::Type *Ty = Init->getType();
   llvm::GlobalVariable *GV =
   new llvm::GlobalVariable(CGM.getModule(), Ty, false,
                            llvm::GlobalValue::PrivateLinkage, Init, Name);
   if (!Section.empty())
      GV->setSection(Section);
   if (Align)
      GV->setAlignment(Align);
   if (AddToUsed)
      CGM.addCompilerUsedGlobal(GV);
   return GV;
}


llvm::Constant *CGObjCMulleRuntime::EmitClassList(Twine Name,
                                          const char *Section,
                                          ArrayRef<llvm::Constant*> Classes)
{
   // Return null for empty list.
   if (Classes.empty())
      return llvm::Constant::getNullValue( llvm::PointerType::getUnqual( ObjCTypes.ClassListTy));

   llvm::Constant *Values[2];
   Values[0] = llvm::ConstantInt::get(ObjCTypes.IntTy, Classes.size());
   llvm::ArrayType *AT = llvm::ArrayType::get(ObjCTypes.ClassPtrTy,
                                              Classes.size());
   Values[1] = llvm::ConstantArray::get(AT, Classes);
   llvm::Constant *Init = llvm::ConstantStruct::getAnon(Values);

   llvm::GlobalVariable *GV = CreateMetadataVar( Name, Init, Section, 4, true);
   return llvm::ConstantExpr::getBitCast(GV, llvm::PointerType::getUnqual( ObjCTypes.ClassListTy));
}


llvm::Constant *CGObjCMulleRuntime::EmitCategoryList(Twine Name,
                                          const char *Section,
                                          ArrayRef<llvm::Constant*> Categories)
{
   // Return null for empty list.
   if (Categories.empty())
      return llvm::Constant::getNullValue( llvm::PointerType::getUnqual( ObjCTypes.CategoryListTy));

   llvm::Constant *Values[2];
   Values[0] = llvm::ConstantInt::get(ObjCTypes.IntTy, Categories.size());
   llvm::ArrayType *AT = llvm::ArrayType::get( llvm::PointerType::getUnqual( ObjCTypes.CategoryTy),
                                              Categories.size());
   Values[1] = llvm::ConstantArray::get(AT, Categories);
   llvm::Constant *Init = llvm::ConstantStruct::getAnon(Values);

   llvm::GlobalVariable *GV = CreateMetadataVar( Name, Init, Section, 4, true);
   return llvm::ConstantExpr::getBitCast(GV, llvm::PointerType::getUnqual( ObjCTypes.CategoryListTy));
}


llvm::Constant *CGObjCMulleRuntime::EmitStaticStringList(Twine Name,
                                                        const char *Section,
                                                        ArrayRef<llvm::Constant*> StaticStrings)
{
   // Return null for empty list.
   if (StaticStrings.empty())
      return llvm::Constant::getNullValue( llvm::PointerType::getUnqual( ObjCTypes.StaticStringListTy));

   llvm::Constant *Values[2];
   Values[0] = llvm::ConstantInt::get(ObjCTypes.IntTy, StaticStrings.size());
   llvm::ArrayType *AT = llvm::ArrayType::get( CGM.VoidPtrTy,
                                               StaticStrings.size());
   Values[1] = llvm::ConstantArray::get(AT, StaticStrings);
   llvm::Constant *Init = llvm::ConstantStruct::getAnon(Values);

   llvm::GlobalVariable *GV = CreateMetadataVar( Name, Init, Section, 4, true);
   return llvm::ConstantExpr::getBitCast(GV, llvm::PointerType::getUnqual( ObjCTypes.StaticStringListTy));
}


llvm::Constant *CGObjCMulleRuntime::EmitHashNameList(Twine Name,
                                                     const char *Section,
                                                     ArrayRef<llvm::Constant*> HashNames)
{
   // Return null for empty list.
   if (HashNames.empty())
      return llvm::Constant::getNullValue( llvm::PointerType::getUnqual( ObjCTypes.HashNameListTy));

   llvm::Constant *Values[2];
   Values[0] = llvm::ConstantInt::get(ObjCTypes.IntTy, HashNames.size());
   llvm::ArrayType *AT = llvm::ArrayType::get( ObjCTypes.HashNameTy,
                                              HashNames.size());
   Values[1] = llvm::ConstantArray::get(AT, HashNames);
   llvm::Constant *Init = llvm::ConstantStruct::getAnon(Values);

   llvm::GlobalVariable *GV = CreateMetadataVar( Name, Init, Section, 4, true);
   return llvm::ConstantExpr::getBitCast(GV, llvm::PointerType::getUnqual( ObjCTypes.HashNameListTy));
}



llvm::Constant *CGObjCMulleRuntime::EmitLoadInfoList(Twine Name,
                                                     const char *Section,
                                                     llvm::Constant *ClassList,
                                                     llvm::Constant *CategoryList,
                                                     llvm::Constant *StringList,
                                                     llvm::Constant *HashNameList)
{
   llvm::Constant   *Values[9];

   //
   // should get these values from the header
   //
   Values[0] = llvm::ConstantInt::get(ObjCTypes.IntTy, load_version);        // just a number
   Values[1] = llvm::ConstantInt::get(ObjCTypes.IntTy, runtime_version);     // major, minor, patch version
   Values[2] = llvm::ConstantInt::get(ObjCTypes.IntTy, foundation_version);  // foundation
   Values[3] = llvm::ConstantInt::get(ObjCTypes.IntTy, user_version);        // user

   unsigned int   optLevel;

   optLevel  = CGM.getLangOpts().OptimizeSize ? -1 : CGM.getCodeGenOpts().OptimizationLevel;
   optLevel &= 0x7;

   unsigned int   bits;

   bits  = optLevel << 8;
   bits |= no_tagged_pointers ? 0x4 : 0x0;
   bits |= thread_local_runtime  ? 0x8 : 0x0;
   bits |= CGM.getLangOpts().ObjCAllocsAutoreleasedObjects ? 0x2 : 0;
   bits |= 0;         // we are sorted, so unsorted == 0

   //
   // memorize some compilation context
   //
   Values[4] = llvm::ConstantInt::get(ObjCTypes.IntTy, bits);
   
   Values[5] = ClassList;
   Values[6] = CategoryList;
   Values[7] = StringList;
   Values[8] = HashNameList;

   llvm::Constant *Init = llvm::ConstantStruct::getAnon(Values);

   llvm::GlobalVariable *GV = CreateMetadataVar( Name, Init, Section, 4, true);
   return llvm::ConstantExpr::getBitCast(GV, llvm::PointerType::getUnqual( ObjCTypes.LoadInfoTy));
}


llvm::Function *CGObjCMulleRuntime::ModuleInitFunction() {
   // Abuse this interface function as a place to finalize.
   // Although it's called init, it's being called during
   // CodeGenModule::Release so it's certainly not too early (but maybe too
   // late ?)
   FinishModule();

   // build up the necessary info structure now and emit it
   llvm::Constant  *expr;

   SmallVector<llvm::Constant *, 16> LoadClasses, LoadCategories, LoadStrings, EmitHashes;
   for (auto *I : DefinedClasses)
   {
      // Instance methods should always be defined.
      expr = llvm::ConstantExpr::getBitCast( I, llvm::PointerType::getUnqual( ObjCTypes.ClassTy));
      LoadClasses.push_back( expr);
   }

   for (auto *I : DefinedCategories)
   {
      // Instance methods should always be defined.
      expr = llvm::ConstantExpr::getBitCast( I, llvm::PointerType::getUnqual( ObjCTypes.CategoryTy));
      LoadCategories.push_back( expr);
   }

   for( llvm::StringMap<llvm::GlobalAlias *>::const_iterator
        I = NSConstantStringMap.begin(), E = NSConstantStringMap.end();
        I != E; ++I)
   {
      expr = llvm::ConstantExpr::getBitCast( I->getValue(), CGM.VoidPtrTy);
      LoadStrings.push_back( expr);
   }

   if( CGM.getCodeGenOpts().getDebugInfo() >= clang::codegenoptions::LimitedDebugInfo)
   {
      for (llvm::StringMap<llvm::ConstantInt *>::const_iterator
           I = DefinedHashes.begin(), E = DefinedHashes.end();
           I != E; ++I)
      {
         llvm::Constant         *Values[2];
         llvm::GlobalVariable   *String;

         String  = CreateMetadataVar( "OBJC_HASHNAME_" + I->getKey(),
                                      llvm::ConstantDataArray::getString( VMContext, I->getKey()),
                                      "__DATA,__module_info,regular,no_dead_strip", 4, false);
         Values[0] = llvm::ConstantExpr::getBitCast( I->getValue(), ObjCTypes.ClassIDTy);
         Values[1] = llvm::ConstantExpr::getBitCast( String, CGM.VoidPtrTy);

         llvm::Constant *expr = llvm::ConstantStruct::get( ObjCTypes.HashNameTy, Values);

         EmitHashes.push_back( expr);
      }
      llvm::array_pod_sort( EmitHashes.begin(), EmitHashes.end(),
                            uniqueid_comparator);
   }

   // always emit to check for code compatability
   if( ! LoadClasses.size() && ! LoadCategories.size() && \
       ! LoadStrings.size() && ! EmitHashes.size())
   {
      // if nothing is emitted, and no runtime versions has been set emit
      // nothing, it's plain C code
      if( ! runtime_version)
         return( nullptr);
   }

   // if we emit something, then check that our produced loadinfo is compatible
   if( ! runtime_version)
   {
      CGM.getDiags().Report( diag::err_mulle_objc_preprocessor_missing_include);
      return( nullptr);
   }
   
   // since the loadinfo and stuff is hardcoded, the check is also hardcoded
   // not elegant...
  
   if( load_version != COMPATIBLE_MULLE_OBJC_RUNTIME_LOAD_VERSION)
   {
      // fprintf( stderr, "version found: 0x%x\n", (int) runtime_version);
      CGM.getDiags().Report( diag::err_mulle_objc_runtime_version_mismatch);
   }

  llvm::Constant *ClassList = EmitClassList( "OBJC_CLASS_LOADS", "__DATA,_objc_load_info", LoadClasses);
  llvm::Constant *CategoryList = EmitCategoryList( "OBJC_CATEGORY_LOADS", "__DATA,_objc_load_info", LoadCategories);
  llvm::Constant *StringList = EmitStaticStringList( "OBJC_STATICSTRING_LOADS", "__DATA,_objc_load_info", LoadStrings);
  llvm::Constant *HashNameList = EmitHashNameList( "OBJC_HASHNAME_LOADS", "__DATA,_objc_load_info", EmitHashes);

  llvm::Constant *LoadInfo = EmitLoadInfoList( "OBJC_LOAD_INFO", "__DATA,_objc_load_info", ClassList, CategoryList, StringList, HashNameList);

   // take collected initializers and create a __attribute__(constructor)
   // static void   __load() function
   // that does the appropriate calls to setup the runtime
   // Now this I could handily steal from CGObjCGnu.cpp

  llvm::Function * LoadFunction = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(VMContext), false),
      llvm::GlobalValue::InternalLinkage, "__load",
      &CGM.getModule());

  // (nat) i have no idea, what this is for
  llvm::BasicBlock *EntryBB =
      llvm::BasicBlock::Create(VMContext, "entry", LoadFunction);

  CGBuilderTy Builder(CGM, VMContext);
  Builder.SetInsertPoint(EntryBB);

  llvm::FunctionType *FT =
    llvm::FunctionType::get(Builder.getVoidTy(),
                            llvm::PointerType::getUnqual(ObjCTypes.LoadInfoTy), true);
  llvm::Value *Register = CGM.CreateRuntimeFunction( FT, "mulle_objc_loadinfo_unfailing_enqueue");
  Builder.CreateCall( Register, LoadInfo);
  Builder.CreateRetVoid();

  return LoadFunction;
}

llvm::Constant *CGObjCMulleRuntime::GetPropertyGetFunction() {
   return ObjCTypes.getGetPropertyFn();
}

llvm::Constant *CGObjCMulleRuntime::GetPropertySetFunction() {
   return ObjCTypes.getSetPropertyFn();
}

llvm::Constant *CGObjCMulleRuntime::GetOptimizedPropertySetFunction(bool atomic,
                                                           bool copy) {
   return ObjCTypes.getOptimizedSetPropertyFn(atomic, copy);
}

llvm::Constant *CGObjCMulleRuntime::GetGetStructFunction() {
   return ObjCTypes.getCopyStructFn();
}
llvm::Constant *CGObjCMulleRuntime::GetSetStructFunction() {
   return ObjCTypes.getCopyStructFn();
}

llvm::Constant *CGObjCMulleRuntime::GetCppAtomicObjectGetFunction() {
   return ObjCTypes.getCppAtomicObjectFunction();
}
llvm::Constant *CGObjCMulleRuntime::GetCppAtomicObjectSetFunction() {
   return ObjCTypes.getCppAtomicObjectFunction();
}

llvm::Constant *CGObjCMulleRuntime::EnumerationMutationFunction() {
   return ObjCTypes.getEnumerationMutationFn();
}

void CGObjCMulleRuntime::EmitTryStmt(CodeGenFunction &CGF, const ObjCAtTryStmt &S) {
   return EmitTryOrSynchronizedStmt(CGF, S);
}

void CGObjCMulleRuntime::EmitSynchronizedStmt(CodeGenFunction &CGF,
                                     const ObjCAtSynchronizedStmt &S) {
   return EmitTryOrSynchronizedStmt(CGF, S);
}

#pragma mark -
#pragma mark Fragile Exception Helper

namespace {
   struct PerformFragileFinally final : EHScopeStack::Cleanup {
      const Stmt &S;
      Address SyncArgSlot;
      Address CallTryExitVar;
      Address ExceptionData;
      ObjCTypesHelper &ObjCTypes;
      PerformFragileFinally(const Stmt *S,
                            Address SyncArgSlot,
                            Address CallTryExitVar,
                            Address ExceptionData,
                            ObjCTypesHelper *ObjCTypes)
      : S(*S), SyncArgSlot(SyncArgSlot), CallTryExitVar(CallTryExitVar),
      ExceptionData(ExceptionData), ObjCTypes(*ObjCTypes) {}

      void Emit(CodeGenFunction &CGF, Flags flags) override {
         // Check whether we need to call objc_exception_try_exit.
         // In optimized code, this branch will always be folded.
         llvm::BasicBlock *FinallyCallExit =
         CGF.createBasicBlock("finally.call_exit");
         llvm::BasicBlock *FinallyNoCallExit =
         CGF.createBasicBlock("finally.no_call_exit");
         CGF.Builder.CreateCondBr(CGF.Builder.CreateLoad(CallTryExitVar),
                                  FinallyCallExit, FinallyNoCallExit);

         CGF.EmitBlock(FinallyCallExit);
         CGF.EmitNounwindRuntimeCall(ObjCTypes.getExceptionTryExitFn(),
                                     ExceptionData.getPointer());

         CGF.EmitBlock(FinallyNoCallExit);

         if (isa<ObjCAtTryStmt>(S)) {
            if (const ObjCAtFinallyStmt* FinallyStmt =
                cast<ObjCAtTryStmt>(S).getFinallyStmt()) {
               // Don't try to do the @finally if this is an EH cleanup.
               if (flags.isForEHCleanup()) return;

               // Save the current cleanup destination in case there's
               // control flow inside the finally statement.
               llvm::Value *CurCleanupDest =
               CGF.Builder.CreateLoad(CGF.getNormalCleanupDestSlot());

               CGF.EmitStmt(FinallyStmt->getFinallyBody());

               if (CGF.HaveInsertPoint()) {
                  CGF.Builder.CreateStore(CurCleanupDest,
                                          CGF.getNormalCleanupDestSlot());
               } else {
                  // Currently, the end of the cleanup must always exist.
                  CGF.EnsureInsertPoint();
               }
            }
         } else {
            // Emit objc_sync_exit(expr); as finally's sole statement for
            // @synchronized.
            llvm::Value *SyncArg = CGF.Builder.CreateLoad(SyncArgSlot);
            CGF.EmitNounwindRuntimeCall(ObjCTypes.getSyncExitFn(), SyncArg);
         }
      }
   };

   class FragileHazards {
      CodeGenFunction &CGF;
      SmallVector<llvm::Value*, 20> Locals;
      llvm::DenseSet<llvm::BasicBlock*> BlocksBeforeTry;

      llvm::InlineAsm *ReadHazard;
      llvm::InlineAsm *WriteHazard;

      llvm::FunctionType *GetAsmFnType();

      void collectLocals();
      void emitReadHazard(CGBuilderTy &Builder);

   public:
      FragileHazards(CodeGenFunction &CGF);

      void emitWriteHazard();
      void emitHazardsInNewBlocks();
   };
}


/// Create the fragile-ABI read and write hazards based on the current
/// state of the function, which is presumed to be immediately prior
/// to a @try block.  These hazards are used to maintain correct
/// semantics in the face of optimization and the fragile ABI's
/// cavalier use of setjmp/longjmp.
FragileHazards::FragileHazards(CodeGenFunction &CGF) : CGF(CGF) {
   collectLocals();

   if (Locals.empty()) return;

   // Collect all the blocks in the function.
   for (llvm::Function::iterator
        I = CGF.CurFn->begin(), E = CGF.CurFn->end(); I != E; ++I)
      BlocksBeforeTry.insert(&*I);

   llvm::FunctionType *AsmFnTy = GetAsmFnType();

   // Create a read hazard for the allocas.  This inhibits dead-store
   // optimizations and forces the values to memory.  This hazard is
   // inserted before any 'throwing' calls in the protected scope to
   // reflect the possibility that the variables might be read from the
   // catch block if the call throws.
   {
      std::string Constraint;
      for (unsigned I = 0, E = Locals.size(); I != E; ++I) {
         if (I) Constraint += ',';
         Constraint += "*m";
      }

      ReadHazard = llvm::InlineAsm::get(AsmFnTy, "", Constraint, true, false);
   }

   // Create a write hazard for the allocas.  This inhibits folding
   // loads across the hazard.  This hazard is inserted at the
   // beginning of the catch path to reflect the possibility that the
   // variables might have been written within the protected scope.
   {
      std::string Constraint;
      for (unsigned I = 0, E = Locals.size(); I != E; ++I) {
         if (I) Constraint += ',';
         Constraint += "=*m";
      }

      WriteHazard = llvm::InlineAsm::get(AsmFnTy, "", Constraint, true, false);
   }
}

/// Emit a write hazard at the current location.
void FragileHazards::emitWriteHazard() {
   if (Locals.empty()) return;

   CGF.EmitNounwindRuntimeCall(WriteHazard, Locals);
}

void FragileHazards::emitReadHazard(CGBuilderTy &Builder) {
   assert(!Locals.empty());
   llvm::CallInst *call = Builder.CreateCall(ReadHazard, Locals);
   call->setDoesNotThrow();
   call->setCallingConv(CGF.getRuntimeCC());
}

/// Emit read hazards in all the protected blocks, i.e. all the blocks
/// which have been inserted since the beginning of the try.
void FragileHazards::emitHazardsInNewBlocks() {
   if (Locals.empty()) return;

   CGBuilderTy Builder(CGF, CGF.getLLVMContext());

   // Iterate through all blocks, skipping those prior to the try.
   for (llvm::Function::iterator
        FI = CGF.CurFn->begin(), FE = CGF.CurFn->end(); FI != FE; ++FI) {
      llvm::BasicBlock &BB = *FI;
      if (BlocksBeforeTry.count(&BB)) continue;

      // Walk through all the calls in the block.
      for (llvm::BasicBlock::iterator
           BI = BB.begin(), BE = BB.end(); BI != BE; ++BI) {
         llvm::Instruction &I = *BI;

         // Ignore instructions that aren't non-intrinsic calls.
         // These are the only calls that can possibly call longjmp.
         if (!isa<llvm::CallInst>(I) && !isa<llvm::InvokeInst>(I)) continue;
         if (isa<llvm::IntrinsicInst>(I))
            continue;

         // Ignore call sites marked nounwind.  This may be questionable,
         // since 'nounwind' doesn't necessarily mean 'does not call longjmp'.
         llvm::CallSite CS(&I);
         if (CS.doesNotThrow()) continue;

         // Insert a read hazard before the call.  This will ensure that
         // any writes to the locals are performed before making the
         // call.  If the call throws, then this is sufficient to
         // guarantee correctness as long as it doesn't also write to any
         // locals.
         Builder.SetInsertPoint(&BB, BI);
         emitReadHazard(Builder);
      }
   }
}

static void addIfPresent(llvm::DenseSet<llvm::Value*> &S, llvm::Value *V) {
   if (V) S.insert(V);
}

static void addIfPresent(llvm::DenseSet<llvm::Value*> &S, Address V) {
   if (V.isValid()) S.insert(V.getPointer());
}

void FragileHazards::collectLocals() {
   // Compute a set of allocas to ignore.
   llvm::DenseSet<llvm::Value*> AllocasToIgnore;
   addIfPresent(AllocasToIgnore, CGF.ReturnValue);
   addIfPresent(AllocasToIgnore, CGF.NormalCleanupDest);

   // Collect all the allocas currently in the function.  This is
   // probably way too aggressive.
   llvm::BasicBlock &Entry = CGF.CurFn->getEntryBlock();
   for (llvm::BasicBlock::iterator
        I = Entry.begin(), E = Entry.end(); I != E; ++I)
      if (isa<llvm::AllocaInst>(*I) && !AllocasToIgnore.count(&*I))
         Locals.push_back(&*I);
}

llvm::FunctionType *FragileHazards::GetAsmFnType() {
   SmallVector<llvm::Type *, 16> tys(Locals.size());
   for (unsigned i = 0, e = Locals.size(); i != e; ++i)
      tys[i] = Locals[i]->getType();
   return llvm::FunctionType::get(CGF.VoidTy, tys, false);
}


#pragma mark -
#pragma mark Objective-C setjmp-longjmp (sjlj) Exception Handling

/*

 Objective-C setjmp-longjmp (sjlj) Exception Handling
 --

 A catch buffer is a setjmp buffer plus:
 - a pointer to the exception that was caught
 - a pointer to the previous exception data buffer
 - two pointers of reserved storage
 Therefore catch buffers form a stack, with a pointer to the top
 of the stack kept in thread-local storage.

 objc_exception_try_enter pushes a catch buffer onto the EH stack.
 objc_exception_try_exit pops the given catch buffer, which is
 required to be the top of the EH stack.
 objc_exception_throw pops the top of the EH stack, writes the
 thrown exception into the appropriate field, and longjmps
 to the setjmp buffer.  It crashes the process (with a printf
 and an abort()) if there are no catch buffers on the stack.
 objc_exception_extract just reads the exception pointer out of the
 catch buffer.

 There's no reason an implementation couldn't use a light-weight
 setjmp here --- something like __builtin_setjmp, but API-compatible
 with the heavyweight setjmp.  This will be more important if we ever
 want to implement correct ObjC/C++ exception interactions for the
 fragile ABI.

 Note that for this use of setjmp/longjmp to be correct, we may need
 to mark some local variables volatile: if a non-volatile local
 variable is modified between the setjmp and the longjmp, it has
 indeterminate value.  For the purposes of LLVM IR, it may be
 sufficient to make loads and stores within the @try (to variables
 declared outside the @try) volatile.  This is necessary for
 optimized correctness, but is not currently being done; this is
 being tracked as rdar://problem/8160285

 The basic framework for a @try-catch-finally is as follows:
 {
 objc_exception_data d;
 id _rethrow = null;
 bool _call_try_exit = true;

 objc_exception_try_enter(&d);
 if (!setjmp(d.jmp_buf)) {
 ... try body ...
 } else {
 // exception path
 id _caught = objc_exception_extract(&d);

 // enter new try scope for handlers
 if (!setjmp(d.jmp_buf)) {
 ... match exception and execute catch blocks ...

 // fell off end, rethrow.
 _rethrow = _caught;
 ... jump-through-finally to finally_rethrow ...
 } else {
 // exception in catch block
 _rethrow = objc_exception_extract(&d);
 _call_try_exit = false;
 ... jump-through-finally to finally_rethrow ...
 }
 }
 ... jump-through-finally to finally_end ...

 finally:
 if (_call_try_exit)
 objc_exception_try_exit(&d);

 ... finally block ....
 ... dispatch to finally destination ...

 finally_rethrow:
 objc_exception_throw(_rethrow);

 finally_end:
 }

 This framework differs slightly from the one gcc uses, in that gcc
 uses _rethrow to determine if objc_exception_try_exit should be called
 and if the object should be rethrown. This breaks in the face of
 throwing nil and introduces unnecessary branches.

 We specialize this framework for a few particular circumstances:

 - If there are no catch blocks, then we avoid emitting the second
 exception handling context.

 - If there is a catch-all catch block (i.e. @catch(...) or @catch(id
 e)) we avoid emitting the code to rethrow an uncaught exception.

 - FIXME: If there is no @finally block we can do a few more
 simplifications.

 Rethrows and Jumps-Through-Finally
 --

 '@throw;' is supported by pushing the currently-caught exception
 onto ObjCEHStack while the @catch blocks are emitted.

 Branches through the @finally block are handled with an ordinary
 normal cleanup.  We do not register an EH cleanup; fragile-ABI ObjC
 exceptions are not compatible with C++ exceptions, and this is
 hardly the only place where this will go wrong.

 @synchronized(expr) { stmt; } is emitted as if it were:
 id synch_value = expr;
 objc_sync_enter(synch_value);
 @try { stmt; } @finally { objc_sync_exit(synch_value); }
 */

void CGObjCMulleRuntime::EmitTryOrSynchronizedStmt(CodeGen::CodeGenFunction &CGF,
                                          const Stmt &S) {
   bool isTry = isa<ObjCAtTryStmt>(S);

   // A destination for the fall-through edges of the catch handlers to
   // jump to.
   CodeGenFunction::JumpDest FinallyEnd =
   CGF.getJumpDestInCurrentScope("finally.end");

   // A destination for the rethrow edge of the catch handlers to jump
   // to.
   CodeGenFunction::JumpDest FinallyRethrow =
   CGF.getJumpDestInCurrentScope("finally.rethrow");

   // For @synchronized, call objc_sync_enter(sync.expr). The
   // evaluation of the expression must occur before we enter the
   // @synchronized.  We can't avoid a temp here because we need the
   // value to be preserved.  If the backend ever does liveness
   // correctly after setjmp, this will be unnecessary.
   Address SyncArgSlot = Address::invalid();
   if (!isTry) {
      llvm::Value *SyncArg =
      CGF.EmitScalarExpr(cast<ObjCAtSynchronizedStmt>(S).getSynchExpr());
      SyncArg = CGF.Builder.CreateBitCast(SyncArg, ObjCTypes.ObjectPtrTy);
      CGF.EmitNounwindRuntimeCall(ObjCTypes.getSyncEnterFn(), SyncArg);

      SyncArgSlot = CGF.CreateTempAlloca(SyncArg->getType(),
                                         CGF.getPointerAlign(), "sync.arg");
      CGF.Builder.CreateStore(SyncArg, SyncArgSlot);
   }

   // Allocate memory for the setjmp buffer.  This needs to be kept
   // live throughout the try and catch blocks.
   Address ExceptionData = CGF.CreateTempAlloca( ObjCTypes.getExceptionDataTy( CGM),
                                                CGF.getPointerAlign(),
                                                "exceptiondata.ptr");

   // Create the fragile hazards.  Note that this will not capture any
   // of the allocas required for exception processing, but will
   // capture the current basic block (which extends all the way to the
   // setjmp call) as "before the @try".
   FragileHazards Hazards(CGF);

   // Create a flag indicating whether the cleanup needs to call
   // objc_exception_try_exit.  This is true except when
   //   - no catches match and we're branching through the cleanup
   //     just to rethrow the exception, or
   //   - a catch matched and we're falling out of the catch handler.
   // The setjmp-safety rule here is that we should always store to this
   // variable in a place that dominates the branch through the cleanup
   // without passing through any setjmps.
   Address CallTryExitVar = CGF.CreateTempAlloca(CGF.Builder.getInt1Ty(),
                                                 CharUnits::One(),
                                                 "_call_try_exit");

   // A slot containing the exception to rethrow.  Only needed when we
   // have both a @catch and a @finally.
   Address PropagatingExnVar = Address::invalid();

   // Push a normal cleanup to leave the try scope.
   CGF.EHStack.pushCleanup<PerformFragileFinally>(NormalAndEHCleanup, &S,
                                                  SyncArgSlot,
                                                  CallTryExitVar,
                                                  ExceptionData,
                                                  &ObjCTypes);

   // Enter a try block:
   //  - Call objc_exception_try_enter to push ExceptionData on top of
   //    the EH stack.
   CGF.EmitNounwindRuntimeCall(ObjCTypes.getExceptionTryEnterFn(),
                               ExceptionData.getPointer());

   //  - Call setjmp on the exception data buffer.
   llvm::Constant *Zero = llvm::ConstantInt::get(CGF.Builder.getInt32Ty(), 0);
   llvm::Constant *One = llvm::ConstantInt::get(CGF.Builder.getInt32Ty(), 1);

   // the setjmp buffer *follows* the pointers in MulleObJC
   llvm::Value *GEPIndexes[] = { Zero, One, Zero};
   llvm::Value *SetJmpBuffer = CGF.Builder.CreateGEP(
                                                     ObjCTypes.getExceptionDataTy( CGM), ExceptionData.getPointer(), GEPIndexes,
                                                     "setjmp_buffer");
   llvm::CallInst *SetJmpResult = CGF.EmitNounwindRuntimeCall(
                                                              ObjCTypes.getSetJmpFn(), SetJmpBuffer, "setjmp_result");
   SetJmpResult->setCanReturnTwice();

   // If setjmp returned 0, enter the protected block; otherwise,
   // branch to the handler.
   llvm::BasicBlock *TryBlock = CGF.createBasicBlock("try");
   llvm::BasicBlock *TryHandler = CGF.createBasicBlock("try.handler");
   llvm::Value *DidCatch =
   CGF.Builder.CreateIsNotNull(SetJmpResult, "did_catch_exception");
   CGF.Builder.CreateCondBr(DidCatch, TryHandler, TryBlock);

   // Emit the protected block.
   CGF.EmitBlock(TryBlock);
   CGF.Builder.CreateStore(CGF.Builder.getTrue(), CallTryExitVar);
   CGF.EmitStmt(isTry ? cast<ObjCAtTryStmt>(S).getTryBody()
                : cast<ObjCAtSynchronizedStmt>(S).getSynchBody());

   CGBuilderTy::InsertPoint TryFallthroughIP = CGF.Builder.saveAndClearIP();

   // Emit the exception handler block.
   CGF.EmitBlock(TryHandler);

   // Don't optimize loads of the in-scope locals across this point.
   Hazards.emitWriteHazard();

   // For a @synchronized (or a @try with no catches), just branch
   // through the cleanup to the rethrow block.
   if (!isTry || !cast<ObjCAtTryStmt>(S).getNumCatchStmts()) {
      // Tell the cleanup not to re-pop the exit.
      CGF.Builder.CreateStore(CGF.Builder.getFalse(), CallTryExitVar);
      CGF.EmitBranchThroughCleanup(FinallyRethrow);

      // Otherwise, we have to match against the caught exceptions.
   } else {
      // Retrieve the exception object.  We may emit multiple blocks but
      // nothing can cross this so the value is already in SSA form.
      llvm::CallInst *Caught =
      CGF.EmitNounwindRuntimeCall(ObjCTypes.getExceptionExtractFn(),
                                  ExceptionData.getPointer(), "caught");

      // Push the exception to rethrow onto the EH value stack for the
      // benefit of any @throws in the handlers.
      CGF.ObjCEHValueStack.push_back(Caught);

      const ObjCAtTryStmt* AtTryStmt = cast<ObjCAtTryStmt>(&S);

      bool HasFinally = (AtTryStmt->getFinallyStmt() != nullptr);

      llvm::BasicBlock *CatchBlock = nullptr;
      llvm::BasicBlock *CatchHandler = nullptr;
      if (HasFinally) {
         // Save the currently-propagating exception before
         // objc_exception_try_enter clears the exception slot.
         PropagatingExnVar = CGF.CreateTempAlloca(Caught->getType(),
                                                  CGF.getPointerAlign(),
                                                  "propagating_exception");
         CGF.Builder.CreateStore(Caught, PropagatingExnVar);

         // Enter a new exception try block (in case a @catch block
         // throws an exception).
         CGF.EmitNounwindRuntimeCall(ObjCTypes.getExceptionTryEnterFn(),
                                     ExceptionData.getPointer());

         llvm::CallInst *SetJmpResult =
         CGF.EmitNounwindRuntimeCall(ObjCTypes.getSetJmpFn(),
                                     SetJmpBuffer, "setjmp.result");
         SetJmpResult->setCanReturnTwice();

         llvm::Value *Threw =
         CGF.Builder.CreateIsNotNull(SetJmpResult, "did_catch_exception");

         CatchBlock = CGF.createBasicBlock("catch");
         CatchHandler = CGF.createBasicBlock("catch_for_catch");
         CGF.Builder.CreateCondBr(Threw, CatchHandler, CatchBlock);

         CGF.EmitBlock(CatchBlock);
      }

      CGF.Builder.CreateStore(CGF.Builder.getInt1(HasFinally), CallTryExitVar);

      // Handle catch list. As a special case we check if everything is
      // matched and avoid generating code for falling off the end if
      // so.
      bool AllMatched = false;
      for (unsigned I = 0, N = AtTryStmt->getNumCatchStmts(); I != N; ++I) {
         const ObjCAtCatchStmt *CatchStmt = AtTryStmt->getCatchStmt(I);

         const VarDecl *CatchParam = CatchStmt->getCatchParamDecl();
         const ObjCObjectPointerType *OPT = nullptr;

         // catch(...) always matches.
         if (!CatchParam) {
            AllMatched = true;
         } else {
            OPT = CatchParam->getType()->getAs<ObjCObjectPointerType>();

            // catch(id e) always matches under this ABI, since only
            // ObjC exceptions end up here in the first place.
            // FIXME: For the time being we also match id<X>; this should
            // be rejected by Sema instead.
            if (OPT && (OPT->isObjCIdType() || OPT->isObjCQualifiedIdType()))
               AllMatched = true;
         }

         // If this is a catch-all, we don't need to test anything.
         if (AllMatched) {
            CodeGenFunction::RunCleanupsScope CatchVarCleanups(CGF);

            if (CatchParam) {
               CGF.EmitAutoVarDecl(*CatchParam);
               assert(CGF.HaveInsertPoint() && "DeclStmt destroyed insert point?");

               // These types work out because ConvertType(id) == i8*.
               EmitInitOfCatchParam(CGF, Caught, CatchParam);
            }

            CGF.EmitStmt(CatchStmt->getCatchBody());

            // The scope of the catch variable ends right here.
            CatchVarCleanups.ForceCleanup();

            CGF.EmitBranchThroughCleanup(FinallyEnd);
            break;
         }

         assert(OPT && "Unexpected non-object pointer type in @catch");
         const ObjCObjectType *ObjTy = OPT->getObjectType();

         // FIXME: @catch (Class c) ?
         ObjCInterfaceDecl *IDecl = ObjTy->getInterface();
         assert(IDecl && "Catch parameter must have Objective-C type!");

         // Check if the @catch block matches the exception object.
         llvm::Value *Class = EmitClassRef(CGF, IDecl);

         llvm::Value *matchArgs[] = { Class, Caught };
         llvm::CallInst *Match =
         CGF.EmitNounwindRuntimeCall(ObjCTypes.getExceptionMatchFn(),
                                     matchArgs, "match");

         llvm::BasicBlock *MatchedBlock = CGF.createBasicBlock("match");
         llvm::BasicBlock *NextCatchBlock = CGF.createBasicBlock("catch.next");

         CGF.Builder.CreateCondBr(CGF.Builder.CreateIsNotNull(Match, "matched"),
                                  MatchedBlock, NextCatchBlock);

         // Emit the @catch block.
         CGF.EmitBlock(MatchedBlock);

         // Collect any cleanups for the catch variable.  The scope lasts until
         // the end of the catch body.
         CodeGenFunction::RunCleanupsScope CatchVarCleanups(CGF);

         CGF.EmitAutoVarDecl(*CatchParam);
         assert(CGF.HaveInsertPoint() && "DeclStmt destroyed insert point?");

         // Initialize the catch variable.
         llvm::Value *Tmp =
         CGF.Builder.CreateBitCast(Caught,
                                   CGF.ConvertType(CatchParam->getType()));
         EmitInitOfCatchParam(CGF, Tmp, CatchParam);

         CGF.EmitStmt(CatchStmt->getCatchBody());

         // We're done with the catch variable.
         CatchVarCleanups.ForceCleanup();

         CGF.EmitBranchThroughCleanup(FinallyEnd);

         CGF.EmitBlock(NextCatchBlock);
      }

      CGF.ObjCEHValueStack.pop_back();

      // If nothing wanted anything to do with the caught exception,
      // kill the extract call.
      if (Caught->use_empty())
         Caught->eraseFromParent();

      if (!AllMatched)
         CGF.EmitBranchThroughCleanup(FinallyRethrow);

      if (HasFinally) {
         // Emit the exception handler for the @catch blocks.
         CGF.EmitBlock(CatchHandler);

         // In theory we might now need a write hazard, but actually it's
         // unnecessary because there's no local-accessing code between
         // the try's write hazard and here.
         //Hazards.emitWriteHazard();

         // Extract the new exception and save it to the
         // propagating-exception slot.
         assert(PropagatingExnVar.isValid());
         llvm::CallInst *NewCaught =
         CGF.EmitNounwindRuntimeCall(ObjCTypes.getExceptionExtractFn(),
                                     ExceptionData.getPointer(), "caught");
         CGF.Builder.CreateStore(NewCaught, PropagatingExnVar);

         // Don't pop the catch handler; the throw already did.
         CGF.Builder.CreateStore(CGF.Builder.getFalse(), CallTryExitVar);
         CGF.EmitBranchThroughCleanup(FinallyRethrow);
      }
   }

   // Insert read hazards as required in the new blocks.
   Hazards.emitHazardsInNewBlocks();

   // Pop the cleanup.
   CGF.Builder.restoreIP(TryFallthroughIP);
   if (CGF.HaveInsertPoint())
      CGF.Builder.CreateStore(CGF.Builder.getTrue(), CallTryExitVar);
   CGF.PopCleanupBlock();
   CGF.EmitBlock(FinallyEnd.getBlock(), true);

   // Emit the rethrow block.
   CGBuilderTy::InsertPoint SavedIP = CGF.Builder.saveAndClearIP();
   CGF.EmitBlock(FinallyRethrow.getBlock(), true);
   if (CGF.HaveInsertPoint()) {
      // If we have a propagating-exception variable, check it.
      llvm::Value *PropagatingExn;
      if (PropagatingExnVar.isValid()) {
         PropagatingExn = CGF.Builder.CreateLoad(PropagatingExnVar);

         // Otherwise, just look in the buffer for the exception to throw.
      } else {
         llvm::CallInst *Caught =
         CGF.EmitNounwindRuntimeCall(ObjCTypes.getExceptionExtractFn(),
                                     ExceptionData.getPointer());
         PropagatingExn = Caught;
      }

      CGF.EmitNounwindRuntimeCall(ObjCTypes.getExceptionThrowFn(),
                                  PropagatingExn);
      CGF.Builder.CreateUnreachable();
   }

   CGF.Builder.restoreIP(SavedIP);
}

void CGObjCMulleRuntime::EmitThrowStmt(CodeGen::CodeGenFunction &CGF,
                                       const ObjCAtThrowStmt &S,
                                       bool ClearInsertionPoint) {
   llvm::Value *ExceptionAsObject;

   if (const Expr *ThrowExpr = S.getThrowExpr()) {
      llvm::Value *Exception = CGF.EmitObjCThrowOperand(ThrowExpr);
      ExceptionAsObject =
      CGF.Builder.CreateBitCast(Exception, ObjCTypes.ObjectPtrTy);
   } else {
      assert((!CGF.ObjCEHValueStack.empty() && CGF.ObjCEHValueStack.back()) &&
             "Unexpected rethrow outside @catch block.");
      ExceptionAsObject = CGF.ObjCEHValueStack.back();
   }

   CGF.EmitRuntimeCall(ObjCTypes.getExceptionThrowFn(), ExceptionAsObject)
   ->setDoesNotReturn();
   CGF.Builder.CreateUnreachable();

   // Clear the insertion point to indicate we are in unreachable code.
   if (ClearInsertionPoint)
      CGF.Builder.ClearInsertionPoint();
}

/// EmitObjCWeakRead - Code gen for loading value of a __weak
/// object: objc_read_weak (id *src)
///
llvm::Value * CGObjCMulleRuntime::EmitObjCWeakRead(CodeGen::CodeGenFunction &CGF,
                                          Address AddrWeakObj) {
   llvm::Type* DestTy = AddrWeakObj.getElementType();
   AddrWeakObj = CGF.Builder.CreateBitCast(AddrWeakObj,
                                           ObjCTypes.PtrObjectPtrTy);
   llvm::Value *read_weak =
   CGF.EmitNounwindRuntimeCall(ObjCTypes.getGcReadWeakFn(),
                               AddrWeakObj.getPointer(), "weakread");
   read_weak = CGF.Builder.CreateBitCast(read_weak, DestTy);
   return read_weak;
}

/// EmitObjCWeakAssign - Code gen for assigning to a __weak object.
/// objc_assign_weak (id src, id *dst)
///
void CGObjCMulleRuntime::EmitObjCWeakAssign(CodeGen::CodeGenFunction &CGF,
                                   llvm::Value *src, Address dst) {
   llvm::Type * SrcTy = src->getType();
   if (!isa<llvm::PointerType>(SrcTy)) {
      unsigned Size = CGM.getDataLayout().getTypeAllocSize(SrcTy);
      assert(Size <= 8 && "does not support size > 8");
      src = (Size == 4) ? CGF.Builder.CreateBitCast(src, ObjCTypes.IntTy)
      : CGF.Builder.CreateBitCast(src, ObjCTypes.LongLongTy);
      src = CGF.Builder.CreateIntToPtr(src, ObjCTypes.Int8PtrTy);
   }
   src = CGF.Builder.CreateBitCast(src, ObjCTypes.ObjectPtrTy);
   dst = CGF.Builder.CreateBitCast(dst, ObjCTypes.PtrObjectPtrTy);
   llvm::Value *args[] = { src, dst.getPointer() };
   CGF.EmitNounwindRuntimeCall(ObjCTypes.getGcAssignWeakFn(),
                               args, "weakassign");
   return;
}

/// EmitObjCGlobalAssign - Code gen for assigning to a __strong object.
/// objc_assign_global (id src, id *dst)
///
void CGObjCMulleRuntime::EmitObjCGlobalAssign(CodeGen::CodeGenFunction &CGF,
                                     llvm::Value *src, Address dst,
                                     bool threadlocal) {
   llvm::Type * SrcTy = src->getType();
   if (!isa<llvm::PointerType>(SrcTy)) {
      unsigned Size = CGM.getDataLayout().getTypeAllocSize(SrcTy);
      assert(Size <= 8 && "does not support size > 8");
      src = (Size == 4) ? CGF.Builder.CreateBitCast(src, ObjCTypes.IntTy)
      : CGF.Builder.CreateBitCast(src, ObjCTypes.LongLongTy);
      src = CGF.Builder.CreateIntToPtr(src, ObjCTypes.Int8PtrTy);
   }
   src = CGF.Builder.CreateBitCast(src, ObjCTypes.ObjectPtrTy);
   dst = CGF.Builder.CreateBitCast(dst, ObjCTypes.PtrObjectPtrTy);
   llvm::Value *args[] = { src, dst.getPointer() };
   if (!threadlocal)
      CGF.EmitNounwindRuntimeCall(ObjCTypes.getGcAssignGlobalFn(),
                                  args, "globalassign");
   else
      CGF.EmitNounwindRuntimeCall(ObjCTypes.getGcAssignThreadLocalFn(),
                                  args, "threadlocalassign");
   return;
}

/// EmitObjCIvarAssign - Code gen for assigning to a __strong object.
/// objc_assign_ivar (id src, id *dst, ptrdiff_t ivaroffset)
///
void CGObjCMulleRuntime::EmitObjCIvarAssign(CodeGen::CodeGenFunction &CGF,
                                   llvm::Value *src, Address dst,
                                   llvm::Value *ivarOffset) {
   assert(ivarOffset && "EmitObjCIvarAssign - ivarOffset is NULL");
   llvm::Type * SrcTy = src->getType();
   if (!isa<llvm::PointerType>(SrcTy)) {
      unsigned Size = CGM.getDataLayout().getTypeAllocSize(SrcTy);
      assert(Size <= 8 && "does not support size > 8");
      src = (Size == 4) ? CGF.Builder.CreateBitCast(src, ObjCTypes.IntTy)
      : CGF.Builder.CreateBitCast(src, ObjCTypes.LongLongTy);
      src = CGF.Builder.CreateIntToPtr(src, ObjCTypes.Int8PtrTy);
   }
   src = CGF.Builder.CreateBitCast(src, ObjCTypes.ObjectPtrTy);
   dst = CGF.Builder.CreateBitCast(dst, ObjCTypes.PtrObjectPtrTy);
   llvm::Value *args[] = { src, dst.getPointer(), ivarOffset };
   CGF.EmitNounwindRuntimeCall(ObjCTypes.getGcAssignIvarFn(), args);
   return;
}

/// EmitObjCStrongCastAssign - Code gen for assigning to a __strong cast object.
/// objc_assign_strongCast (id src, id *dst)
///
void CGObjCMulleRuntime::EmitObjCStrongCastAssign(CodeGen::CodeGenFunction &CGF,
                                         llvm::Value *src, Address dst) {
   llvm::Type * SrcTy = src->getType();
   if (!isa<llvm::PointerType>(SrcTy)) {
      unsigned Size = CGM.getDataLayout().getTypeAllocSize(SrcTy);
      assert(Size <= 8 && "does not support size > 8");
      src = (Size == 4) ? CGF.Builder.CreateBitCast(src, ObjCTypes.IntTy)
      : CGF.Builder.CreateBitCast(src, ObjCTypes.LongLongTy);
      src = CGF.Builder.CreateIntToPtr(src, ObjCTypes.Int8PtrTy);
   }
   src = CGF.Builder.CreateBitCast(src, ObjCTypes.ObjectPtrTy);
   dst = CGF.Builder.CreateBitCast(dst, ObjCTypes.PtrObjectPtrTy);
   llvm::Value *args[] = { src, dst.getPointer() };
   CGF.EmitNounwindRuntimeCall(ObjCTypes.getGcAssignStrongCastFn(),
                               args, "strongassign");
   return;
}

void CGObjCMulleRuntime::EmitGCMemmoveCollectable(CodeGen::CodeGenFunction &CGF,
                                         Address DestPtr,
                                         Address SrcPtr,
                                         llvm::Value *size) {
   SrcPtr = CGF.Builder.CreateBitCast(SrcPtr, ObjCTypes.Int8PtrTy);
   DestPtr = CGF.Builder.CreateBitCast(DestPtr, ObjCTypes.Int8PtrTy);
   llvm::Value *args[] = { DestPtr.getPointer(), SrcPtr.getPointer(), size };
   CGF.EmitNounwindRuntimeCall(ObjCTypes.GcMemmoveCollectableFn(), args);
}

/// EmitObjCValueForIvar - Code Gen for ivar reference.
///
LValue CGObjCMulleRuntime::EmitObjCValueForIvar(CodeGen::CodeGenFunction &CGF,
                                       QualType ObjectTy,
                                       llvm::Value *BaseValue,
                                       const ObjCIvarDecl *Ivar,
                                       unsigned CVRQualifiers) {
   const ObjCInterfaceDecl *ID =
   ObjectTy->getAs<ObjCObjectType>()->getInterface();
   return EmitValueForIvarAtOffset(CGF, ID, BaseValue, Ivar, CVRQualifiers,
                                   EmitIvarOffset(CGF, ID, Ivar));
}

llvm::Value *CGObjCMulleRuntime::EmitIvarOffset(CodeGen::CodeGenFunction &CGF,
                                       const ObjCInterfaceDecl *Interface,
                                       const ObjCIvarDecl *Ivar) {
   uint64_t Offset = ComputeIvarBaseOffset(CGM, Interface, Ivar);
   return llvm::ConstantInt::get(
                                 CGM.getTypes().ConvertType(CGM.getContext().LongTy),
                                 Offset);
}

/* *** Private Interface *** */

/// EmitImageInfo - Emit the image info marker used to encode some module
/// level information.
///
/// See: <rdr://4810609&4810587&4810587>
/// struct IMAGE_INFO {
///   unsigned version;
///   unsigned flags;
/// };
enum ImageInfoFlags {
   eImageInfo_FixAndContinue      = (1 << 0), // This flag is no longer set by clang.
   eImageInfo_GarbageCollected    = (1 << 1),
   eImageInfo_GCOnly              = (1 << 2),
   eImageInfo_OptimizedByDyld     = (1 << 3), // This flag is set by the dyld shared cache.

   // A flag indicating that the module has no instances of a @synthesize of a
   // superclass variable. <rdar://problem/6803242>
   eImageInfo_CorrectedSynthesize = (1 << 4), // This flag is no longer set by clang.
   eImageInfo_ImageIsSimulated    = (1 << 5)
};

void CGObjCCommonMulleRuntime::EmitImageInfo() {
   unsigned version = 0; // Version is unused?
   const char *Section = (ObjCABI == 1) ?
   "__DATA, __image_info,regular" :
   "__DATA, __objc_imageinfo, regular, no_dead_strip";

   // Generate module-level named metadata to convey this information to the
   // linker and code-gen.
   llvm::Module &Mod = CGM.getModule();

   // Add the ObjC ABI version to the module flags.
   Mod.addModuleFlag(llvm::Module::Error, "Objective-C Version", ObjCABI);
   Mod.addModuleFlag(llvm::Module::Error, "Objective-C Image Info Version",
                     version);
   Mod.addModuleFlag(llvm::Module::Error, "Objective-C Image Info Section",
                     llvm::MDString::get(VMContext,Section));

   if (CGM.getLangOpts().getGC() == LangOptions::NonGC) {
      // Non-GC overrides those files which specify GC.
      Mod.addModuleFlag(llvm::Module::Override,
                        "Objective-C Garbage Collection", (uint32_t)0);
   } else {
      // Add the ObjC garbage collection value.
      Mod.addModuleFlag(llvm::Module::Error,
                        "Objective-C Garbage Collection",
                        eImageInfo_GarbageCollected);

      if (CGM.getLangOpts().getGC() == LangOptions::GCOnly) {
         // Add the ObjC GC Only value.
         Mod.addModuleFlag(llvm::Module::Error, "Objective-C GC Only",
                           eImageInfo_GCOnly);

         // Require that GC be specified and set to eImageInfo_GarbageCollected.
         llvm::Metadata *Ops[2] = {
            llvm::MDString::get(VMContext, "Objective-C Garbage Collection"),
            llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
                                                                 llvm::Type::getInt32Ty(VMContext), eImageInfo_GarbageCollected))};
         Mod.addModuleFlag(llvm::Module::Require, "Objective-C GC Only",
                           llvm::MDNode::get(VMContext, Ops));
      }
   }

   // Indicate whether we're compiling this to run on a simulator.
   const llvm::Triple &Triple = CGM.getTarget().getTriple();
   if (Triple.isiOS() &&
       (Triple.getArch() == llvm::Triple::x86 ||
        Triple.getArch() == llvm::Triple::x86_64))
      Mod.addModuleFlag(llvm::Module::Error, "Objective-C Is Simulated",
                        eImageInfo_ImageIsSimulated);
}

// struct objc_module {
//   unsigned long version;
//   unsigned long size;
//   const char *name;
//   Symtab symtab;
// };

// FIXME: Get from somewhere
static const int ModuleVersion = 7;

void CGObjCMulleRuntime::EmitModuleInfo() {
   uint64_t Size = CGM.getDataLayout().getTypeAllocSize(ObjCTypes.ModuleTy);

   llvm::Constant *Values[] = {
      llvm::ConstantInt::get(ObjCTypes.LongTy, ModuleVersion),
      llvm::ConstantInt::get(ObjCTypes.LongTy, Size),
      // This used to be the filename, now it is unused. <rdr://4327263>
      GetClassName(StringRef("")),
      EmitModuleSymbols()
   };
   CreateMetadataVar("OBJC_MODULES",
                     llvm::ConstantStruct::get(ObjCTypes.ModuleTy, Values),
                     "__DATA,__module_info,regular,no_dead_strip", 4, true);
}

llvm::Constant *CGObjCMulleRuntime::EmitModuleSymbols() {
   unsigned NumClasses = DefinedClasses.size();
   unsigned NumCategories = DefinedCategories.size();

   // Return null if no symbols were defined.
   if (!NumClasses && !NumCategories)
      return llvm::Constant::getNullValue(ObjCTypes.SymtabPtrTy);

   llvm::Constant *Values[5];
   Values[0] = llvm::ConstantInt::get(ObjCTypes.LongTy, 0);
   Values[1] = llvm::Constant::getNullValue(ObjCTypes.SelectorIDTy);
   Values[2] = llvm::ConstantInt::get(ObjCTypes.ShortTy, NumClasses);
   Values[3] = llvm::ConstantInt::get(ObjCTypes.ShortTy, NumCategories);

   // The runtime expects exactly the list of defined classes followed
   // by the list of defined categories, in a single array.
   SmallVector<llvm::Constant*, 8> Symbols(NumClasses + NumCategories);
   for (unsigned i=0; i<NumClasses; i++) {
      const ObjCInterfaceDecl *ID = ImplementedClasses[i];
      assert(ID);
      if (ObjCImplementationDecl *IMP = ID->getImplementation())
         // We are implementing a weak imported interface. Give it external linkage
         if (ID->isWeakImported() && !IMP->isWeakImported())
            DefinedClasses[i]->setLinkage(llvm::GlobalVariable::ExternalLinkage);

      Symbols[i] = llvm::ConstantExpr::getBitCast(DefinedClasses[i],
                                                  ObjCTypes.Int8PtrTy);
   }
   for (unsigned i=0; i<NumCategories; i++)
      Symbols[NumClasses + i] =
      llvm::ConstantExpr::getBitCast(DefinedCategories[i],
                                     ObjCTypes.Int8PtrTy);

   Values[4] =
   llvm::ConstantArray::get(llvm::ArrayType::get(ObjCTypes.Int8PtrTy,
                                                 Symbols.size()),
                            Symbols);

   llvm::Constant *Init = llvm::ConstantStruct::getAnon(Values);

   llvm::GlobalVariable *GV = CreateMetadataVar(
                                                "OBJC_SYMBOLS", Init, "__DATA,__symbols,regular,no_dead_strip", 4, true);
   return llvm::ConstantExpr::getBitCast(GV, ObjCTypes.SymtabPtrTy);
}


llvm::Constant *CGObjCMulleRuntime::EmitClassRefFromId(CodeGenFunction &CGF,
                                           IdentifierInfo *II) {
   CallArgList  ActualArgs;
   llvm::Constant  *Hash;
   ReturnValueSlot Return;

   LazySymbols.insert(II);

   Hash = HashClassConstantForString( II->getName());
   return( Hash);
}

llvm::Constant *CGObjCMulleRuntime::EmitClassRef(CodeGenFunction &CGF,
                                     const ObjCInterfaceDecl *ID) {
   return EmitClassRefFromId(CGF, ID->getIdentifier());
}

llvm::Value *CGObjCMulleRuntime::EmitNSAutoreleasePoolClassRef(CodeGenFunction &CGF) {
   // we need to actually get the class here, though
   return( GetClass( CGF, "NSAutoreleasePool"));
}


#pragma mark -
#pragma mark Hash Selectors, ClassIDs and friends



#define FNV1_32_PRIME             0x01000193
#define MULLE_OBJC_FNV1_32_INIT   0x811c9dc5

static uint32_t   mulle_objc_chained_fnv1_32( void *buf, size_t len, uint32_t hash)
{
   unsigned char   *s;
   unsigned char   *sentinel;

   s        = (unsigned char *) buf;
   sentinel = &s[ len];

    /*
     * FNV-1 hash each octet in the buffer
     */
    while( s < sentinel)
    {
	hash *= FNV1_32_PRIME;
	hash ^= (uint32_t) *s++;
    }

    return( hash);
}


static inline uint32_t   mulle_objc_fnv1_32( void *buf, size_t len)
{
   return( mulle_objc_chained_fnv1_32( buf, len, MULLE_OBJC_FNV1_32_INIT));
}


uint64_t   CGObjCCommonMulleRuntime::UniqueidHashForString( std::string s, uint64_t first_valid, unsigned WordSizeInBytes)
{
   uint64_t   value;
   char       *c_str;

   c_str = (char *) s.c_str();
   value = (uint64_t) mulle_objc_fnv1_32( (void *) c_str, s.length());
//   fprintf( stderr, "%s = %08lx\n", c_str, (long) value);

   if( value < first_valid || (first_valid && value == (uint64_t) -1))
   {
      // FAIL! FIX
      std::string  fail;

      fprintf( stderr, "congratulations, your string \"%s\" "
              "hashes badly (rare and precious, please tweet it @mulle_nat, then rename it).", fail.c_str());
      CGM.getDiags().Report( diag::err_mulle_objc_hash_invalid);
   }

   return( value);
}


llvm::ConstantInt *CGObjCCommonMulleRuntime::__HashConstantForString( std::string s, uint64_t first_valid)
{
   //   unsigned WordSizeInBits = CGM.getTarget().getPointerWidth(0);
   //   unsigned ByteSizeInBits = CGM.getTarget().getCharWidth();
   unsigned WordSizeInBytes = 4; // WordSizeInBits/ByteSizeInBits;
   uint64_t   value;

   value = UniqueidHashForString( s, first_valid, WordSizeInBytes);

   if( WordSizeInBytes == 8)
   {
      const llvm::APInt SelConstant(64, value);
      return (llvm::ConstantInt *) llvm::ConstantInt::getIntegerValue(CGM.Int64Ty, SelConstant);
   }

   const llvm::APInt SelConstant(32, value);
   return (llvm::ConstantInt *) llvm::ConstantInt::getIntegerValue(CGM.Int32Ty, SelConstant);
}


llvm::ConstantInt *CGObjCCommonMulleRuntime::_HashConstantForString( std::string s, uint64_t first_valid)
{
   llvm::ConstantInt *&Entry = DefinedHashes[ s];  // how does this work ???

   if( ! Entry)
      Entry = __HashConstantForString( s, first_valid);
   return( Entry);
}


llvm::Constant *CGObjCMulleRuntime::EmitSelector(CodeGenFunction &CGF, Selector Sel,
                                     bool lvalue)
{
   return( HashSelConstantForString( Sel.getAsString()));
}


llvm::Constant *CGObjCMulleRuntime::EmitClassID(CodeGenFunction &CGF, const ObjCInterfaceDecl *Class)
{
   return( HashClassConstantForString( Class->getNameAsString()));
}

#pragma mark -


llvm::Constant *CGObjCCommonMulleRuntime::GetClassName(StringRef RuntimeName) {
    llvm::GlobalVariable *&Entry = ClassNames[RuntimeName];
    if (!Entry)
      Entry = CreateMetadataVar(
          "OBJC_CLASS_NAME_" + RuntimeName.str(),
          llvm::ConstantDataArray::getString(VMContext, RuntimeName),
          ((ObjCABI == 2) ? "__TEXT,__objc_classname,cstring_literals"
                          : "__TEXT,__cstring,cstring_literals"),
          1, true);
    return getConstantGEP(VMContext, Entry, 0, 0);
}

llvm::Function *CGObjCCommonMulleRuntime::GetMethodDefinition(const ObjCMethodDecl *MD) {
   llvm::DenseMap<const ObjCMethodDecl*, llvm::Function*>::iterator
   I = MethodDefinitions.find(MD);
   if (I != MethodDefinitions.end())
      return I->second;

   return nullptr;
}

# pragma mark -
# pragma mark Ivar code stolen from Mac runtime

/// GetIvarLayoutName - Returns a unique constant for the given
/// ivar layout bitmap.
llvm::Constant *CGObjCCommonMulleRuntime::GetIvarLayoutName(IdentifierInfo *Ident,
                                                   const ObjCCommonTypesHelper &ObjCTypes) {
   return llvm::Constant::getNullValue(ObjCTypes.Int8PtrTy);
}

void CGObjCCommonMulleRuntime::BuildAggrIvarRecordLayout(const RecordType *RT,
                                                unsigned int BytePos,
                                                bool ForStrongLayout,
                                                bool &HasUnion) {
   const RecordDecl *RD = RT->getDecl();
   // FIXME - Use iterator.
   SmallVector<const FieldDecl*, 16> Fields(RD->fields());
   llvm::Type *Ty = CGM.getTypes().ConvertType(QualType(RT, 0));
   const llvm::StructLayout *RecLayout =
   CGM.getDataLayout().getStructLayout(cast<llvm::StructType>(Ty));

   BuildAggrIvarLayout(nullptr, RecLayout, RD, Fields, BytePos, ForStrongLayout,
                       HasUnion);
}

void CGObjCCommonMulleRuntime::BuildAggrIvarLayout(const ObjCImplementationDecl *OI,
                                          const llvm::StructLayout *Layout,
                                          const RecordDecl *RD,
                                          ArrayRef<const FieldDecl*> RecFields,
                                          unsigned int BytePos, bool ForStrongLayout,
                                          bool &HasUnion) {
   bool IsUnion = (RD && RD->isUnion());
   uint64_t MaxUnionIvarSize = 0;
   uint64_t MaxSkippedUnionIvarSize = 0;
   const FieldDecl *MaxField = nullptr;
   const FieldDecl *MaxSkippedField = nullptr;
   const FieldDecl *LastFieldBitfieldOrUnnamed = nullptr;
   uint64_t MaxFieldOffset = 0;
   uint64_t MaxSkippedFieldOffset = 0;
   uint64_t LastBitfieldOrUnnamedOffset = 0;
   uint64_t FirstFieldDelta = 0;

   if (RecFields.empty())
      return;
   unsigned WordSizeInBits = CGM.getTarget().getPointerWidth(0);
   unsigned ByteSizeInBits = CGM.getTarget().getCharWidth();
   if (!RD && CGM.getLangOpts().ObjCAutoRefCount) {
      const FieldDecl *FirstField = RecFields[0];
      FirstFieldDelta =
      ComputeIvarBaseOffset(CGM, OI, cast<ObjCIvarDecl>(FirstField));
   }

   for (unsigned i = 0, e = RecFields.size(); i != e; ++i) {
      const FieldDecl *Field = RecFields[i];
      uint64_t FieldOffset;
      if (RD) {
         // Note that 'i' here is actually the field index inside RD of Field,
         // although this dependency is hidden.
         const ASTRecordLayout &RL = CGM.getContext().getASTRecordLayout(RD);
         FieldOffset = (RL.getFieldOffset(i) / ByteSizeInBits) - FirstFieldDelta;
      } else
         FieldOffset =
         ComputeIvarBaseOffset(CGM, OI, cast<ObjCIvarDecl>(Field)) - FirstFieldDelta;

      // Skip over unnamed or bitfields
      if (!Field->getIdentifier() || Field->isBitField()) {
         LastFieldBitfieldOrUnnamed = Field;
         LastBitfieldOrUnnamedOffset = FieldOffset;
         continue;
      }

      LastFieldBitfieldOrUnnamed = nullptr;
      QualType FQT = Field->getType();
      if (FQT->isRecordType() || FQT->isUnionType()) {
         if (FQT->isUnionType())
            HasUnion = true;

         BuildAggrIvarRecordLayout(FQT->getAs<RecordType>(),
                                   BytePos + FieldOffset,
                                   ForStrongLayout, HasUnion);
         continue;
      }

      if (const ArrayType *Array = CGM.getContext().getAsArrayType(FQT)) {
         const ConstantArrayType *CArray =
         dyn_cast_or_null<ConstantArrayType>(Array);
         uint64_t ElCount = CArray->getSize().getZExtValue();
         assert(CArray && "only array with known element size is supported");
         FQT = CArray->getElementType();
         while (const ArrayType *Array = CGM.getContext().getAsArrayType(FQT)) {
            const ConstantArrayType *CArray =
            dyn_cast_or_null<ConstantArrayType>(Array);
            ElCount *= CArray->getSize().getZExtValue();
            FQT = CArray->getElementType();
         }
         if (FQT->isRecordType() && ElCount) {
            int OldIndex = IvarsInfo.size() - 1;
            int OldSkIndex = SkipIvars.size() -1;

            const RecordType *RT = FQT->getAs<RecordType>();
            BuildAggrIvarRecordLayout(RT, BytePos + FieldOffset,
                                      ForStrongLayout, HasUnion);

            // Replicate layout information for each array element. Note that
            // one element is already done.
            uint64_t ElIx = 1;
            for (int FirstIndex = IvarsInfo.size() - 1,
                 FirstSkIndex = SkipIvars.size() - 1 ;ElIx < ElCount; ElIx++) {
               uint64_t Size = CGM.getContext().getTypeSize(RT)/ByteSizeInBits;
               for (int i = OldIndex+1; i <= FirstIndex; ++i)
                  IvarsInfo.push_back(GC_IVAR(IvarsInfo[i].ivar_bytepos + Size*ElIx,
                                              IvarsInfo[i].ivar_size));
               for (int i = OldSkIndex+1; i <= FirstSkIndex; ++i)
                  SkipIvars.push_back(GC_IVAR(SkipIvars[i].ivar_bytepos + Size*ElIx,
                                              SkipIvars[i].ivar_size));
            }
            continue;
         }
      }
      // At this point, we are done with Record/Union and array there of.
      // For other arrays we are down to its element type.
      Qualifiers::GC GCAttr = GetGCAttrTypeForType(CGM.getContext(), FQT);

      unsigned FieldSize = CGM.getContext().getTypeSize(Field->getType());
      if ((ForStrongLayout && GCAttr == Qualifiers::Strong)
          || (!ForStrongLayout && GCAttr == Qualifiers::Weak)) {
         if (IsUnion) {
            uint64_t UnionIvarSize = FieldSize / WordSizeInBits;
            if (UnionIvarSize > MaxUnionIvarSize) {
               MaxUnionIvarSize = UnionIvarSize;
               MaxField = Field;
               MaxFieldOffset = FieldOffset;
            }
         } else {
            IvarsInfo.push_back(GC_IVAR(BytePos + FieldOffset,
                                        FieldSize / WordSizeInBits));
         }
      } else if ((ForStrongLayout &&
                  (GCAttr == Qualifiers::GCNone || GCAttr == Qualifiers::Weak))
                 || (!ForStrongLayout && GCAttr != Qualifiers::Weak)) {
         if (IsUnion) {
            // FIXME: Why the asymmetry? We divide by word size in bits on other
            // side.
            uint64_t UnionIvarSize = FieldSize / ByteSizeInBits;
            if (UnionIvarSize > MaxSkippedUnionIvarSize) {
               MaxSkippedUnionIvarSize = UnionIvarSize;
               MaxSkippedField = Field;
               MaxSkippedFieldOffset = FieldOffset;
            }
         } else {
            // FIXME: Why the asymmetry, we divide by byte size in bits here?
            SkipIvars.push_back(GC_IVAR(BytePos + FieldOffset,
                                        FieldSize / ByteSizeInBits));
         }
      }
   }

   if (LastFieldBitfieldOrUnnamed) {
      if (LastFieldBitfieldOrUnnamed->isBitField()) {
         // Last field was a bitfield. Must update skip info.
         uint64_t BitFieldSize
         = LastFieldBitfieldOrUnnamed->getBitWidthValue(CGM.getContext());
         GC_IVAR skivar;
         skivar.ivar_bytepos = BytePos + LastBitfieldOrUnnamedOffset;
         skivar.ivar_size = (BitFieldSize / ByteSizeInBits)
         + ((BitFieldSize % ByteSizeInBits) != 0);
         SkipIvars.push_back(skivar);
      } else {
         assert(!LastFieldBitfieldOrUnnamed->getIdentifier() &&"Expected unnamed");
         // Last field was unnamed. Must update skip info.
         unsigned FieldSize
         = CGM.getContext().getTypeSize(LastFieldBitfieldOrUnnamed->getType());
         SkipIvars.push_back(GC_IVAR(BytePos + LastBitfieldOrUnnamedOffset,
                                     FieldSize / ByteSizeInBits));
      }
   }

   if (MaxField)
      IvarsInfo.push_back(GC_IVAR(BytePos + MaxFieldOffset,
                                  MaxUnionIvarSize));
   if (MaxSkippedField)
      SkipIvars.push_back(GC_IVAR(BytePos + MaxSkippedFieldOffset,
                                  MaxSkippedUnionIvarSize));
}

/// BuildIvarLayoutBitmap - This routine is the horsework for doing all
/// the computations and returning the layout bitmap (for ivar or blocks) in
/// the given argument BitMap string container. Routine reads
/// two containers, IvarsInfo and SkipIvars which are assumed to be
/// filled already by the caller.
llvm::Constant *CGObjCCommonMulleRuntime::BuildIvarLayoutBitmap(std::string &BitMap) {
   unsigned int WordsToScan, WordsToSkip;
   llvm::Type *PtrTy = CGM.Int8PtrTy;

   // Build the string of skip/scan nibbles
   SmallVector<SKIP_SCAN, 32> SkipScanIvars;
   unsigned int WordSize =
   CGM.getTypes().getDataLayout().getTypeAllocSize(PtrTy);
   if (IvarsInfo[0].ivar_bytepos == 0) {
      WordsToSkip = 0;
      WordsToScan = IvarsInfo[0].ivar_size;
   } else {
      WordsToSkip = IvarsInfo[0].ivar_bytepos/WordSize;
      WordsToScan = IvarsInfo[0].ivar_size;
   }
   for (unsigned int i=1, Last=IvarsInfo.size(); i != Last; i++) {
      unsigned int TailPrevGCObjC =
      IvarsInfo[i-1].ivar_bytepos + IvarsInfo[i-1].ivar_size * WordSize;
      if (IvarsInfo[i].ivar_bytepos == TailPrevGCObjC) {
         // consecutive 'scanned' object pointers.
         WordsToScan += IvarsInfo[i].ivar_size;
      } else {
         // Skip over 'gc'able object pointer which lay over each other.
         if (TailPrevGCObjC > IvarsInfo[i].ivar_bytepos)
            continue;
         // Must skip over 1 or more words. We save current skip/scan values
         //  and start a new pair.
         SKIP_SCAN SkScan;
         SkScan.skip = WordsToSkip;
         SkScan.scan = WordsToScan;
         SkipScanIvars.push_back(SkScan);

         // Skip the hole.
         SkScan.skip = (IvarsInfo[i].ivar_bytepos - TailPrevGCObjC) / WordSize;
         SkScan.scan = 0;
         SkipScanIvars.push_back(SkScan);
         WordsToSkip = 0;
         WordsToScan = IvarsInfo[i].ivar_size;
      }
   }
   if (WordsToScan > 0) {
      SKIP_SCAN SkScan;
      SkScan.skip = WordsToSkip;
      SkScan.scan = WordsToScan;
      SkipScanIvars.push_back(SkScan);
   }

   if (!SkipIvars.empty()) {
      unsigned int LastIndex = SkipIvars.size()-1;
      int LastByteSkipped =
      SkipIvars[LastIndex].ivar_bytepos + SkipIvars[LastIndex].ivar_size;
      LastIndex = IvarsInfo.size()-1;
      int LastByteScanned =
      IvarsInfo[LastIndex].ivar_bytepos +
      IvarsInfo[LastIndex].ivar_size * WordSize;
      // Compute number of bytes to skip at the tail end of the last ivar scanned.
      if (LastByteSkipped > LastByteScanned) {
         unsigned int TotalWords = (LastByteSkipped + (WordSize -1)) / WordSize;
         SKIP_SCAN SkScan;
         SkScan.skip = TotalWords - (LastByteScanned/WordSize);
         SkScan.scan = 0;
         SkipScanIvars.push_back(SkScan);
      }
   }
   // Mini optimization of nibbles such that an 0xM0 followed by 0x0N is produced
   // as 0xMN.
   int SkipScan = SkipScanIvars.size()-1;
   for (int i = 0; i <= SkipScan; i++) {
      if ((i < SkipScan) && SkipScanIvars[i].skip && SkipScanIvars[i].scan == 0
          && SkipScanIvars[i+1].skip == 0 && SkipScanIvars[i+1].scan) {
         // 0xM0 followed by 0x0N detected.
         SkipScanIvars[i].scan = SkipScanIvars[i+1].scan;
         for (int j = i+1; j < SkipScan; j++)
            SkipScanIvars[j] = SkipScanIvars[j+1];
         --SkipScan;
      }
   }

   // Generate the string.
   for (int i = 0; i <= SkipScan; i++) {
      unsigned char byte;
      unsigned int skip_small = SkipScanIvars[i].skip % 0xf;
      unsigned int scan_small = SkipScanIvars[i].scan % 0xf;
      unsigned int skip_big  = SkipScanIvars[i].skip / 0xf;
      unsigned int scan_big  = SkipScanIvars[i].scan / 0xf;

      // first skip big.
      for (unsigned int ix = 0; ix < skip_big; ix++)
         BitMap += (unsigned char)(0xf0);

      // next (skip small, scan)
      if (skip_small) {
         byte = skip_small << 4;
         if (scan_big > 0) {
            byte |= 0xf;
            --scan_big;
         } else if (scan_small) {
            byte |= scan_small;
            scan_small = 0;
         }
         BitMap += byte;
      }
      // next scan big
      for (unsigned int ix = 0; ix < scan_big; ix++)
         BitMap += (unsigned char)(0x0f);
      // last scan small
      if (scan_small) {
         byte = scan_small;
         BitMap += byte;
      }
   }
   // null terminate string.
   unsigned char zero = 0;
   BitMap += zero;

   llvm::GlobalVariable *Entry = CreateMetadataVar(
                                                   "OBJC_CLASS_NAME_",
                                                   llvm::ConstantDataArray::getString(VMContext, BitMap, false),
                                                   ((ObjCABI == 2) ? "__TEXT,__objc_classname,cstring_literals"
                                                    : "__TEXT,__cstring,cstring_literals"),
                                                   1, true);
   return getConstantGEP(VMContext, Entry, 0, 0);
}

/// BuildIvarLayout - Builds ivar layout bitmap for the class
/// implementation for the __strong or __weak case.
/// The layout map displays which words in ivar list must be skipped
/// and which must be scanned by GC (see below). String is built of bytes.
/// Each byte is divided up in two nibbles (4-bit each). Left nibble is count
/// of words to skip and right nibble is count of words to scan. So, each
/// nibble represents up to 15 workds to skip or scan. Skipping the rest is
/// represented by a 0x00 byte which also ends the string.
/// 1. when ForStrongLayout is true, following ivars are scanned:
/// - id, Class
/// - object *
/// - __strong anything
///
/// 2. When ForStrongLayout is false, following ivars are scanned:
/// - __weak anything
///
llvm::Constant *CGObjCCommonMulleRuntime::BuildIvarLayout(
                                                 const ObjCImplementationDecl *OMD,
                                                 bool ForStrongLayout) {
   bool hasUnion = false;

   llvm::Type *PtrTy = CGM.Int8PtrTy;
   if (CGM.getLangOpts().getGC() == LangOptions::NonGC &&
       !CGM.getLangOpts().ObjCAutoRefCount)
      return llvm::Constant::getNullValue(PtrTy);

   const ObjCInterfaceDecl *OI = OMD->getClassInterface();
   SmallVector<const FieldDecl*, 32> RecFields;
   if (CGM.getLangOpts().ObjCAutoRefCount) {
      for (const ObjCIvarDecl *IVD = OI->all_declared_ivar_begin();
           IVD; IVD = IVD->getNextIvar())
         RecFields.push_back(cast<FieldDecl>(IVD));
   }
   else {
      SmallVector<const ObjCIvarDecl*, 32> Ivars;
      CGM.getContext().DeepCollectObjCIvars(OI, true, Ivars);

      // FIXME: This is not ideal; we shouldn't have to do this copy.
      RecFields.append(Ivars.begin(), Ivars.end());
   }

   if (RecFields.empty())
      return llvm::Constant::getNullValue(PtrTy);

   SkipIvars.clear();
   IvarsInfo.clear();

   BuildAggrIvarLayout(OMD, nullptr, nullptr, RecFields, 0, ForStrongLayout,
                       hasUnion);
   if (IvarsInfo.empty())
      return llvm::Constant::getNullValue(PtrTy);
   // Sort on byte position in case we encounterred a union nested in
   // the ivar list.
   if (hasUnion && !IvarsInfo.empty())
      std::sort(IvarsInfo.begin(), IvarsInfo.end());
   if (hasUnion && !SkipIvars.empty())
      std::sort(SkipIvars.begin(), SkipIvars.end());

   std::string BitMap;
   llvm::Constant *C = BuildIvarLayoutBitmap(BitMap);

   if (CGM.getLangOpts().ObjCGCBitmapPrint) {
      printf("\n%s ivar layout for class '%s': ",
             ForStrongLayout ? "strong" : "weak",
             OMD->getClassInterface()->getName().str().c_str());
      const unsigned char *s = (const unsigned char*)BitMap.c_str();
      for (unsigned i = 0, e = BitMap.size(); i < e; i++)
         if (!(s[i] & 0xf0))
            printf("0x0%x%s", s[i], s[i] != 0 ? ", " : "");
         else
            printf("0x%x%s",  s[i], s[i] != 0 ? ", " : "");
      printf("\n");
   }
   return C;
}

# pragma mark -
# pragma mark Aux code

llvm::Constant *CGObjCCommonMulleRuntime::GetMethodVarName(Selector Sel)
{
   llvm::GlobalVariable *&Entry = MethodVarNames[Sel];

   // FIXME: Avoid std::string in "Sel.getAsString()"
   if (!Entry)
      Entry = CreateMetadataVar(
                                "OBJC_METH_VAR_NAME_",
                                llvm::ConstantDataArray::getString(VMContext, Sel.getAsString()),
                                ((ObjCABI == 2) ? "__TEXT,__objc_methname,cstring_literals"
                                 : "__TEXT,__cstring,cstring_literals"),
                                1, true);

   return getConstantGEP(VMContext, Entry, 0, 0);
}

// FIXME: Merge into a single cstring creation function.
llvm::Constant *CGObjCCommonMulleRuntime::GetMethodVarName(IdentifierInfo *ID) {
   return GetMethodVarName(CGM.getContext().Selectors.getNullarySelector(ID));
}

llvm::Constant *CGObjCCommonMulleRuntime::GetMethodVarType(const FieldDecl *Field) {
   std::string TypeStr;
   CGM.getContext().getObjCEncodingForType(Field->getType(), TypeStr, Field);

   llvm::GlobalVariable *&Entry = MethodVarTypes[TypeStr];

   if (!Entry)
      Entry = CreateMetadataVar(
                                "OBJC_METH_VAR_TYPE_",
                                llvm::ConstantDataArray::getString(VMContext, TypeStr),
                                ((ObjCABI == 2) ? "__TEXT,__objc_methtype,cstring_literals"
                                 : "__TEXT,__cstring,cstring_literals"),
                                1, true);

   return getConstantGEP(VMContext, Entry, 0, 0);
}

llvm::Constant *CGObjCCommonMulleRuntime::GetMethodVarType(const ObjCMethodDecl *D,
                                                           bool Extended) {
   std::string TypeStr;
   if (CGM.getContext().getObjCEncodingForMethodDecl(D, TypeStr, Extended))
      return nullptr;

   llvm::GlobalVariable *&Entry = MethodVarTypes[TypeStr];

   if (!Entry)
      Entry = CreateMetadataVar(
                                "OBJC_METH_VAR_TYPE_",
                                llvm::ConstantDataArray::getString(VMContext, TypeStr),
                                ((ObjCABI == 2) ? "__TEXT,__objc_methtype,cstring_literals"
                                 : "__TEXT,__cstring,cstring_literals"),
                                1, true);

   return getConstantGEP(VMContext, Entry, 0, 0);
}


llvm::Constant *CGObjCCommonMulleRuntime::GetIvarName(const ObjCIvarDecl *Ivar)
{
   llvm::GlobalVariable *&Entry = IvarNames[ Ivar->getNameAsString()];

   // FIXME: Avoid std::string in "Sel.getAsString()"
   if (!Entry)
      Entry = CreateMetadataVar(
                                "OBJC_IVAR_NAME_",
                                llvm::ConstantDataArray::getString(VMContext, Ivar->getNameAsString()),
                                ((ObjCABI == 2) ? "__TEXT,__objc_ivarname,cstring_literals"
                                 : "__TEXT,__cstring,cstring_literals"),
                                1, true);

   return getConstantGEP(VMContext, Entry, 0, 0);
}


llvm::Constant *CGObjCCommonMulleRuntime::GetIvarType(const ObjCIvarDecl *Ivar)
{
   std::string TypeStr;
   QualType PType = Ivar->getType();

   //
   // find property of same name (how?), if yes, encode
   // '@' differently as '@', '=' or '~' depending
   // on assignment
   //
   CGM.getContext().getObjCEncodingForTypeImpl(PType, TypeStr, true, true, nullptr,
                              true     /*OutermostType*/,
                              false    /*EncodingProperty*/,
                              false    /*StructField*/,
                              false    /*EncodeBlockParameters*/,
                              false    /*EncodeClassNames*/);

   // remember these crazy containers, insert on what looks like a read
   llvm::GlobalVariable *&Entry = IvarTypes[TypeStr];

   if (!Entry)
      Entry = CreateMetadataVar(
                                "OBJC_IVAR_TYPE_",
                                llvm::ConstantDataArray::getString(VMContext, TypeStr),
                                ((ObjCABI == 2) ? "__TEXT,__objc_methtype,cstring_literals"
                                 : "__TEXT,__cstring,cstring_literals"),
                                1, true);

   return getConstantGEP(VMContext, Entry, 0, 0);
}


// FIXME: Merge into a single cstring creation function.
llvm::Constant *CGObjCCommonMulleRuntime::GetPropertyName(IdentifierInfo *Ident) {
   llvm::GlobalVariable *&Entry = PropertyNames[Ident];

   if (!Entry)
      Entry = CreateMetadataVar(
                                "OBJC_PROP_NAME_ATTR_",
                                llvm::ConstantDataArray::getString(VMContext, Ident->getName()),
                                "__TEXT,__cstring,cstring_literals", 1, true);

   return getConstantGEP(VMContext, Entry, 0, 0);
}

// FIXME: Merge into a single cstring creation function.
// FIXME: This Decl should be more precise.
llvm::Constant *
CGObjCCommonMulleRuntime::GetPropertyTypeString(const ObjCPropertyDecl *PD,
                                       const Decl *Container) {
   std::string TypeStr;
   CGM.getContext().getObjCEncodingForPropertyDecl(PD, Container, TypeStr);
   return GetPropertyName(&CGM.getContext().Idents.get(TypeStr));
}

void CGObjCCommonMulleRuntime::GetNameForMethod(const ObjCMethodDecl *D,
                                       const ObjCContainerDecl *CD,
                                       SmallVectorImpl<char> &Name) {
   llvm::raw_svector_ostream OS(Name);
   assert (CD && "Missing container decl in GetNameForMethod");
   OS << /* '\01' << */ (D->isInstanceMethod() ? '-' : '+')
   << '[' << CD->getName();
   if (const ObjCCategoryImplDecl *CID =
       dyn_cast<ObjCCategoryImplDecl>(D->getDeclContext()))
      OS << '(' << *CID << ')';
   OS << ' ' << D->getSelector().getAsString() << ']';
}

void CGObjCMulleRuntime::FinishModule() {
}


/* *** */

ObjCCommonTypesHelper::ObjCCommonTypesHelper(CodeGen::CodeGenModule &cgm)
: VMContext(cgm.getLLVMContext()), CGM(cgm), ExternalProtocolPtrTy(nullptr)
{
   CodeGen::CodeGenTypes &Types = CGM.getTypes();
   ASTContext &Ctx = CGM.getContext();

   ShortTy = Types.ConvertType(Ctx.ShortTy);
   IntTy = Types.ConvertType(Ctx.IntTy);
   LongTy = Types.ConvertType(Ctx.LongTy);
   LongLongTy = Types.ConvertType(Ctx.LongLongTy);
   Int8PtrTy = CGM.Int8PtrTy;
   Int8PtrPtrTy = CGM.Int8PtrPtrTy;

   // @mulle-objc@ uniqueid: make it 32 bit here
   ClassIDTy    =
   CategoryIDTy =
   SelectorIDTy =
   PropertyIDTy =
   IvarIDTy     =
   ProtocolIDTy = Types.ConvertType( Ctx.getIntTypeForBitwidth(32, false));

   ClassIDPtrTy      = llvm::PointerType::getUnqual(ClassIDTy);
   CategoryIDTyPtrTy = llvm::PointerType::getUnqual(CategoryIDTy);
   SelectorIDTyPtrTy = llvm::PointerType::getUnqual(SelectorIDTy);
   PropertyIDTyPtrTy = llvm::PointerType::getUnqual(PropertyIDTy);
   IvarIDPtrTy       = llvm::PointerType::getUnqual(IvarIDTy);
   ProtocolIDPtrTy   = llvm::PointerType::getUnqual(ProtocolIDTy);

   // arm64 targets use "int" ivar offset variables. All others,
   // including OS X x86_64 and Windows x86_64, use "long" ivar offsets.
   if (CGM.getTarget().getTriple().getArch() == llvm::Triple::aarch64)
      IvarOffsetVarTy = IntTy;
   else
      IvarOffsetVarTy = LongTy;

   ObjectPtrTy    = Types.ConvertType(Ctx.getObjCIdType());
   PtrObjectPtrTy = llvm::PointerType::getUnqual(ObjectPtrTy);
   ParamsPtrTy    = llvm::PointerType::getUnqual(Types.ConvertType(Ctx.VoidTy));

   // I'm not sure I like this. The implicit coordination is a bit
   // gross. We should solve this in a reasonable fashion because this
   // is a pretty common task (match some runtime data structure with
   // an LLVM data structure).

   // FIXME: This is leaked.
   // FIXME: Merge with rewriter code?


//   struct _mulle_objc_property
//   {
//      mulle_objc_propertyid_t    propertyid;
//      char                       *name;
//      char                       *signature;  // hmmm...
//      mulle_objc_methodid_t      getter;
//      mulle_objc_methodid_t      setter;
//      mulle_objc_methodid_t      clearer;
//   };

   PropertyTy = llvm::StructType::create("struct._mulle_objc_property",
                                         PropertyIDTy,
                                         Int8PtrTy,
                                         Int8PtrTy,
                                         SelectorIDTy,
                                         SelectorIDTy,
                                         SelectorIDTy,
                                         nullptr);

   //struct _mulle_objc_propertylist
   //{
   //   unsigned int                 n_properties;
   //   struct _mulle_objc_property  properties[ 1];
   //};
   PropertyListTy =
   llvm::StructType::create("struct._mulle_objc_propertylist", IntTy,
                            llvm::ArrayType::get(PropertyTy, 0), nullptr);
   // struct _prop_list_t *
   PropertyListPtrTy = llvm::PointerType::getUnqual(PropertyListTy);

   //struct _mulle_objc_method_descriptor
   //{
   //   mulle_objc_method_id_t   method_id;
   //   char                     *name;
   //   char                     *signature;
   //   unsigned int             bits;
   //};
   //struct _mulle_objc_method
   //{
   //   struct _mulle_objc_method_descriptor  descriptor;
   //   mulle_objc_method_implementation_t    implementation;
   //};

    MethodTy = llvm::StructType::create("struct._mulle_objc_method",
                                       SelectorIDTy, Int8PtrTy, Int8PtrTy, IntTy, Int8PtrTy,
                                       nullptr);

   // struct _objc_cache *
   CacheTy = llvm::StructType::create(VMContext, "struct._mulle_objc_cache");
   CachePtrTy = llvm::PointerType::getUnqual(CacheTy);

}

ObjCTypesHelper::ObjCTypesHelper(CodeGen::CodeGenModule &cgm)
: ObjCCommonTypesHelper(cgm) {

   // struct _objc_method_description {
   //   SEL name;
   //   char *types;
   // }
   MethodDescriptionTy =
   llvm::StructType::create("struct._mulle_objc_methoddescriptor",
                            SelectorIDTy, Int8PtrTy, nullptr);

   // struct _objc_method_description_list {
   //   int count;
   //   struct _objc_method_description[1];
   // }
   MethodDescriptionListTy = llvm::StructType::create(
                                                      "struct._mulle_objc_methoddescriptorlist", IntTy,
                                                      llvm::ArrayType::get(MethodDescriptionTy, 0), nullptr);

   // struct _objc_method_description_list *
   MethodDescriptionListPtrTy =
   llvm::PointerType::getUnqual(MethodDescriptionListTy);

   // Protocol description structures

   // struct _objc_protocol_extension {
   //   uint32_t size;  // sizeof(struct _objc_protocol_extension)
   //   struct _objc_method_description_list *optional_instance_methods;
   //   struct _objc_method_description_list *optional_class_methods;
   //   struct _objc_property_list *instance_properties;
   //   const char ** extendedMethodTypes;
   // }
   ProtocolExtensionTy =
   llvm::StructType::create("struct._mulle_objc_protocolextension",
                            IntTy, MethodDescriptionListPtrTy,
                            MethodDescriptionListPtrTy, PropertyListPtrTy,
                            Int8PtrPtrTy, nullptr);

   // struct _objc_protocol_extension *
   ProtocolExtensionPtrTy = llvm::PointerType::getUnqual(ProtocolExtensionTy);

   // Handle recursive construction of Protocol and ProtocolList types

   ProtocolTy =
   llvm::StructType::create(VMContext, "struct._mulle_objc_protocol");

   ProtocolListTy =
   llvm::StructType::create(VMContext, "struct._mulle_objc_protocollist");
   ProtocolListTy->setBody(llvm::PointerType::getUnqual(ProtocolListTy),
                           LongTy,
                           llvm::ArrayType::get(ProtocolTy, 0),
                           nullptr);

   // struct _objc_protocol {
   //   struct _objc_protocol_extension *isa;
   //   char *protocol_name;
   //   struct _objc_protocol **_objc_protocol_list;
   //   struct _objc_method_description_list *instance_methods;
   //   struct _objc_method_description_list *class_methods;
   // }
   ProtocolTy->setBody(ProtocolExtensionPtrTy, Int8PtrTy,
                       llvm::PointerType::getUnqual(ProtocolListTy),
                       MethodDescriptionListPtrTy,
                       MethodDescriptionListPtrTy,
                       nullptr);

   // struct _objc_protocol_list *
   ProtocolListPtrTy = llvm::PointerType::getUnqual(ProtocolListTy);

   ProtocolPtrTy = llvm::PointerType::getUnqual(ProtocolTy);

   // Class description structures

   // struct _objc_ivar {
   //   char *ivar_name;
   //   char *ivar_type;
   //   int  ivar_offset;
   // }
   IvarTy = llvm::StructType::create(VMContext, "struct._mulle_objc_ivar");
   IvarTy->setBody(
                    IvarIDTy,    // ivar_id
                    Int8PtrTy,   // name
                    Int8PtrTy,   // signature
                    IntTy,       // offset
                    nullptr);

   // struct _objc_ivar_list *
   IvarListTy =
   llvm::StructType::create(VMContext, "struct._mulle_objc_ivarlist");
   IvarListPtrTy = llvm::PointerType::getUnqual(IvarListTy);

   // struct _objc_method_list *
   MethodListTy =
   llvm::StructType::create(VMContext, "struct._mulle_objc_methodlist");
   MethodListPtrTy = llvm::PointerType::getUnqual(MethodListTy);

   // struct _objc_class_extension *
   ClassExtensionTy =
   llvm::StructType::create("struct._objc_classextension",
                            IntTy, Int8PtrTy, PropertyListPtrTy, nullptr);
   ClassExtensionPtrTy = llvm::PointerType::getUnqual(ClassExtensionTy);

   ClassTy = llvm::StructType::create(VMContext, "struct._mulle_objc_loadclass");


//   struct _mulle_objc_loadclass
//   {
//      mulle_objc_classid_t              classid;
//      char                              *classname;
//      mulle_objc_hash_t                 classivarhash;
//
//      mulle_objc_classid_t              superclassuniqueid;
//      char                              *superclassname;
//      mulle_objc_hash_t                 superclassivarhash;
//
//      int                               fastclassindex;
//      int                               instancesize;
//
//      struct _mulle_objc_ivarlist       *instancevariables;
//
//      struct _mulle_objc_methodlist     *classmethods;
//      struct _mulle_objc_methodlist     *instancemethods;
//      struct _mulle_objc_propertylist   *properties;
//
//      mulle_objc_protocolid_t           *protocolids;
//      mulle_objc_classid_t              *protocolclassids;
//   };

   ClassTy->setBody(
                    ClassIDTy,         // classid
                    Int8PtrTy,         // class_name
                    ClassIDTy,         // ivarhash

                    ClassIDTy,         // superclass_classid
                    Int8PtrTy,         // superclass_name,
                    ClassIDTy,         // superclass_ivarhash

                    IntTy,             // fastclassindex
                    IntTy,             // instance_size

                    IvarListPtrTy,     // instance_variables
                    MethodListPtrTy,   // class_methods
                    MethodListPtrTy,   // instance_methods
                    PropertyListPtrTy, // properties

                    ProtocolIDPtrTy,
                    ClassIDPtrTy,
                    nullptr);

   ClassPtrTy = llvm::PointerType::getUnqual(ClassTy);


//   struct _mulle_objc_loadcategory
//   {
//      mulle_objc_hash_t                 categoryid;
//      char                              *categoryname;
//
//      mulle_objc_classid_t              classid;
//      char                              *classname;         // useful ??
//      mulle_objc_hash_t                 classivarhash;
//
//      struct _mulle_objc_methodlist     *classmethods;
//      struct _mulle_objc_methodlist     *instancemethods;
//      struct _mulle_objc_propertylist   *properties;
//
//      mulle_objc_protocolid_t           *protocolids;
//      mulle_objc_classid_t              *protocolclassids;
//   };

   CategoryTy =
   llvm::StructType::create("struct._mulle_objc_loadcategory",
                            ClassIDTy, Int8PtrTy, ClassIDTy, Int8PtrTy, ClassIDTy, MethodListPtrTy,
                            MethodListPtrTy, PropertyListPtrTy,
                            ProtocolIDPtrTy, ClassIDPtrTy, nullptr);

   //   struct _mulle_objc_loadstaticstring
   //   {
   //      struct _mulle_objc_object *address;  //
   //   };

   StaticStringTy =
   llvm::StructType::create("struct._mulle_objc_loadstaticstring",
                            Int8PtrTy, nullptr);

   //   struct _mulle_objc_loadhashname
   //   {
   //      mulle_objc_uniqueid_t   uniqueid;
   //      char                    *name;
   //   };

   HashNameTy =
   llvm::StructType::create("struct._mulle_objc_loadhashname",
                            ClassIDTy, Int8PtrTy, nullptr);

   // Global metadata structures

   // struct _objc_symtab {
   //   long sel_ref_cnt;
   //   SEL *refs;
   //   short cls_def_cnt;
   //   short cat_def_cnt;
   //   char *defs[cls_def_cnt + cat_def_cnt];
   // }
   SymtabTy =
   llvm::StructType::create("struct._objc_symtab",
                            LongTy, SelectorIDTy, ShortTy, ShortTy,
                            llvm::ArrayType::get(Int8PtrTy, 0), nullptr);
   SymtabPtrTy = llvm::PointerType::getUnqual(SymtabTy);

   // struct _objc_module {
   //   long version;
   //   long size;   // sizeof(struct _objc_module)
   //   char *name;
   //   struct _objc_symtab* symtab;
   //  }
   ModuleTy =
   llvm::StructType::create("struct._objc_module",
                            LongTy, LongTy, Int8PtrTy, SymtabPtrTy, nullptr);


   //
   // some stuff we only use to emit the structure as input to load
   //
   ClassListTy = llvm::StructType::create("struct._mulle_objc_loadclasslist",
                            IntTy,
                            llvm::PointerType::getUnqual( ClassPtrTy),
                            nullptr);
   CategoryListTy = llvm::StructType::create("struct._mulle_objc_loadcategorylist",
                            IntTy,
                            llvm::PointerType::getUnqual( llvm::PointerType::getUnqual( CategoryTy)),
                            nullptr);
   StaticStringListTy = llvm::StructType::create("struct._mulle_objc_loadcategorylist",
                                             IntTy,
                                             llvm::PointerType::getUnqual( llvm::PointerType::getUnqual( StaticStringTy)),
                                             nullptr);
   HashNameListTy = llvm::StructType::create("struct._mulle_objc_loadhashnamelist",
                                             IntTy,
                                             llvm::PointerType::getUnqual( llvm::PointerType::getUnqual( HashNameTy)),
                                             nullptr);

   LoadInfoTy = llvm::StructType::create("struct._mulle_objc_loadinfo",
                            IntTy,
                            IntTy,
                            IntTy,
                            IntTy,
                            IntTy,
                            llvm::PointerType::getUnqual( ClassListTy),
                            llvm::PointerType::getUnqual( CategoryListTy),
                            llvm::PointerType::getUnqual( StaticStringListTy),
                            llvm::PointerType::getUnqual( HashNameListTy),
                            nullptr);

   ExceptionDataTy = nullptr; // later on demand
}


 llvm::Type  *ObjCTypesHelper::getExceptionDataTy( CodeGen::CodeGenModule &cgm)
 {
   if( ExceptionDataTy)
      return( ExceptionDataTy);

   QualType   jmpbuf_type;
   uint64_t   SetJmpBufferSize = 0;
    
   // this type is only available if <setjmp.h> has been included already

   jmpbuf_type = cgm.getContext().getjmp_bufType();
   if( jmpbuf_type == QualType())
   {
      CGM.getDiags().Report( diag::err_mulle_objc_setjmp_not_included);
   }
   else
   {
      SetJmpBufferSize = cgm.getContext().getTypeSizeInChars( jmpbuf_type).getQuantity();
      if( ! SetJmpBufferSize)
         CGM.getDiags().Report( diag::err_mulle_objc_jmpbuf_size_unknown);
   }
    
   uint64_t SetJmpBufferInts = (SetJmpBufferSize + SetJmpBufferSize - 1)/ (CGM.Int32Ty->getBitWidth() / 8);

   // fprintf( stderr, "sizeof( jmpbuf) : %lld, %lld\n", SetJmpBufferSize, SetJmpBufferInts);
   // Exceptions (4 void *) ???
   llvm::Type *StackPtrTy = llvm::ArrayType::get(CGM.Int8PtrTy, 4);

   //
   // this is different in MulleObjC, to fix alignment problems
   // the setjmp buffer *follows* the pointers
   //
   ExceptionDataTy =
   llvm::StructType::create("struct._mulle_objc_exception_data",
                            StackPtrTy,
                            llvm::ArrayType::get(CGM.Int32Ty,SetJmpBufferInts),
                            nullptr);
   return( ExceptionDataTy);
}

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
  case ObjCRuntime::WatchOS :
    llvm_unreachable("these runtimes are not Mulle ObjC runtimes");
  }
  llvm_unreachable("bad runtime");
}

