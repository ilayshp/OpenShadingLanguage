/*
Copyright (c) 2009-2010 Sony Pictures Imageworks Inc., et al.
All Rights Reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are
met:
* Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.
* Neither the name of Sony Pictures Imageworks nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

//#define OSL_DEV

#include <memory>
#include <OpenImageIO/thread.h>
#include <boost/thread/tss.hpp>   /* for thread_specific_ptr */
#include <unordered_map>

#include "OSL/oslconfig.h"
#include "OSL/llvm_util.h"
#include "OSL/wide.h"
#include "oslexec_pvt.h"

#ifndef OSL_LLVM_VERSION
    #error Must define an OSL_LLVM_VERSION
#endif

#if OSL_LLVM_VERSION < 34
#error "LLVM minimum version required for OSL is 3.4"
#endif

#if OSL_LLVM_VERSION >= 35 && OSL_CPLUSPLUS_VERSION < 11
#error "LLVM >= 3.5 requires C++11 or newer"
#endif

// Use MCJIT for LLVM 3.6 and beyind, old JIT for earlier
#define USE_MCJIT   (OSL_LLVM_VERSION >= 36)
#define USE_OLD_JIT (OSL_LLVM_VERSION <  36)

#include <llvm/CodeGen/CommandFlags.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DataLayout.h>
#if OSL_LLVM_VERSION >= 35
#  include <llvm/Linker/Linker.h>
#  include <llvm/Support/FileSystem.h>
#else
#  include <llvm/Linker.h>
#endif
#include <llvm/Support/ErrorOr.h>
#include <llvm/Support/raw_os_ostream.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/TargetRegistry.h>

#if OSL_LLVM_VERSION < 40
#  include <llvm/Bitcode/ReaderWriter.h>
#else
#  include <llvm/Bitcode/BitcodeReader.h>
#  include <llvm/Bitcode/BitcodeWriter.h>
#endif
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/ExecutionEngine/JITEventListener.h>
#if USE_MCJIT
#  include <llvm/ExecutionEngine/MCJIT.h>
#endif
#if USE_OLD_JIT
#  include <llvm/ExecutionEngine/JIT.h>
#  include <llvm/ExecutionEngine/JITMemoryManager.h>
#endif
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/PrettyStackTrace.h>
#if OSL_LLVM_VERSION >= 35
#  include <llvm/IR/Verifier.h>
#else
#  include <llvm/Analysis/Verifier.h>
#endif
#include <llvm/Target/TargetOptions.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/IPO.h>
#include <llvm/Transforms/Utils/UnifyFunctionExitNodes.h>
#include <llvm/Transforms/IPO/PassManagerBuilder.h>
#if OSL_LLVM_VERSION >= 36
#  include <llvm/ExecutionEngine/SectionMemoryManager.h>
#endif
#if OSL_LLVM_VERSION >= 39
#  include <llvm/Transforms/Scalar/GVN.h>
#endif

OSL_NAMESPACE_ENTER

namespace pvt {

#if USE_OLD_JIT
    typedef llvm::JITMemoryManager LLVMMemoryManager;
#else
    typedef llvm::SectionMemoryManager LLVMMemoryManager;
#endif

#if OSL_LLVM_VERSION >= 35
#if OSL_LLVM_VERSION < 40
    typedef std::error_code LLVMErr;
#else
    typedef llvm::Error LLVMErr;
#endif
#endif


namespace {
static OIIO::spin_mutex llvm_global_mutex;
static bool setup_done = false;
static std::vector<std::shared_ptr<LLVMMemoryManager> > jitmm_hold;

};




// We hold certain things (LLVM context and custom JIT memory manager)
// per thread and retained across LLVM_Util invocations.  We are
// intentionally "leaking" them.
struct LLVM_Util::PerThreadInfo::Impl {
    Impl () : llvm_context(NULL), llvm_jitmm(NULL) {}
    ~Impl () {
        delete llvm_context;
        // N.B. Do NOT delete the jitmm -- another thread may need the
        // code! Don't worry, we stashed a pointer in jitmm_hold.
        // TODO: look into alternative way to manage lifetime of JIT'd code.
        // Once the last ShadingSystem is destructed we could free this llvm_jitmm memory?
    }

    llvm::LLVMContext *llvm_context;
    LLVMMemoryManager *llvm_jitmm;
};

LLVM_Util::PerThreadInfo::PerThreadInfo()
: m_thread_info(nullptr)
{}

LLVM_Util::PerThreadInfo::~PerThreadInfo()
{
    // Make sure destructor to PerThreadInfoImpl is only called here
    // where we know the definition of the owned PerThreadInfoImpl;
    delete m_thread_info;
}

LLVM_Util::PerThreadInfo::Impl *
LLVM_Util::PerThreadInfo::get() const
{
    if (nullptr == m_thread_info) {
        m_thread_info = new Impl();
    }
    return m_thread_info;
}


size_t
LLVM_Util::total_jit_memory_held ()
{
    size_t jitmem = 0;
    OIIO::spin_lock lock (llvm_global_mutex);
#if USE_OLD_JIT
    for (size_t i = 0;  i < jitmm_hold.size();  ++i) {
        LLVMMemoryManager *mm = jitmm_hold[i].get();
        if (mm)
            jitmem += mm->GetDefaultCodeSlabSize() * mm->GetNumCodeSlabs()
                    + mm->GetDefaultDataSlabSize() * mm->GetNumDataSlabs()
                    + mm->GetDefaultStubSlabSize() * mm->GetNumStubSlabs();
    }
#endif
    return jitmem;
}



/// MemoryManager - Create a shell that passes on requests
/// to a real LLVMMemoryManager underneath, but can be retained after the
/// dummy is destroyed.  Also, we don't pass along any deallocations.
class LLVM_Util::MemoryManager : public LLVMMemoryManager {
protected:
    LLVMMemoryManager *mm;  // the real one
public:

#if USE_OLD_JIT // llvm::JITMemoryManager
    MemoryManager(LLVMMemoryManager *realmm) : mm(realmm) { HasGOT = realmm->isManagingGOT(); }

    void setMemoryWritable() override { mm->setMemoryWritable(); }
    void setMemoryExecutable() override { mm->setMemoryExecutable(); }
    void setPoisonMemory(bool poison) override { mm->setPoisonMemory(poison); }
    void AllocateGOT() override { ASSERT(HasGOT == false); ASSERT(HasGOT == mm->isManagingGOT()); mm->AllocateGOT(); HasGOT = true; ASSERT(HasGOT == mm->isManagingGOT()); }
    uint8_t *getGOTBase() override const { return mm->getGOTBase(); }
    uint8_t *startFunctionBody(const llvm::Function *F,
                                       uintptr_t &ActualSize) override {
        return mm->startFunctionBody (F, ActualSize);
    }
    uint8_t *allocateStub(const llvm::GlobalValue* F, unsigned StubSize,
                                  unsigned Alignment) override {
        return mm->allocateStub (F, StubSize, Alignment);
    }
    void endFunctionBody(const llvm::Function *F,
                                 uint8_t *FunctionStart, uint8_t *FunctionEnd) override {
        mm->endFunctionBody (F, FunctionStart, FunctionEnd);
    }
    uint8_t *allocateSpace(intptr_t Size, unsigned Alignment) override {
        return mm->allocateSpace (Size, Alignment);
    }
    uint8_t *allocateGlobal(uintptr_t Size, unsigned Alignment) override {
        return mm->allocateGlobal (Size, Alignment);
    }
    void deallocateFunctionBody(void *Body) override {
        // DON'T DEALLOCATE mm->deallocateFunctionBody (Body);
    }
    bool CheckInvariants(std::string &s) override {
        return mm->CheckInvariants(s);
    }
    size_t GetDefaultCodeSlabSize() override {
        return mm->GetDefaultCodeSlabSize();
    }
    size_t GetDefaultDataSlabSize() override {
        return mm->GetDefaultDataSlabSize();
    }
    size_t GetDefaultStubSlabSize() override {
        return mm->GetDefaultStubSlabSize();
    }
    unsigned GetNumCodeSlabs() override { return mm->GetNumCodeSlabs(); }
    unsigned GetNumDataSlabs() override { return mm->GetNumDataSlabs(); }
    unsigned GetNumStubSlabs() override { return mm->GetNumStubSlabs(); }

    void notifyObjectLoaded(llvm::ExecutionEngine *EE, const llvm::ObjectImage *oi) override {
        mm->notifyObjectLoaded (EE, oi);
    }

#else // MCJITMemoryManager

    MemoryManager(LLVMMemoryManager *realmm) : mm(realmm) {}

    void notifyObjectLoaded(llvm::ExecutionEngine *EE, const llvm::object::ObjectFile &oi) override {
        mm->notifyObjectLoaded (EE, oi);
    }

    void notifyObjectLoaded (RuntimeDyld &RTDyld, const object::ObjectFile &Obj) override {
        mm->notifyObjectLoaded(RTDyld, Obj);
    }

#if OSL_LLVM_VERSION <= 37
    void reserveAllocationSpace(uintptr_t CodeSize, uintptr_t DataSizeRO, uintptr_t DataSizeRW) override {
        return mm->reserveAllocationSpace(CodeSize, DataSizeRO, DataSizeRW);
    }
#else
    void reserveAllocationSpace(uintptr_t CodeSize, uint32_t CodeAlign,
                                uintptr_t RODataSize, uint32_t RODataAlign,
                                uintptr_t RWDataSize, uint32_t RWDataAlign) override {
        return mm->reserveAllocationSpace(CodeSize, CodeAlign, RODataSize, RODataAlign, RWDataSize, RWDataAlign);
    }
#endif

    bool needsToReserveAllocationSpace() override {
        return mm->needsToReserveAllocationSpace();
    }

    void invalidateInstructionCache() override {
        mm->invalidateInstructionCache();
    }
    
    JITSymbol findSymbol(const std::string &Name) override {
        return mm->findSymbol(Name);
    }

    uint64_t getSymbolAddressInLogicalDylib (const std::string &Name) override {
        return mm->getSymbolAddressInLogicalDylib(Name);
    }

    JITSymbol findSymbolInLogicalDylib (const std::string &Name) override {
        return mm->findSymbolInLogicalDylib(Name);
    }
#endif

    // Common
    virtual ~MemoryManager() {}

    void *getPointerToNamedFunction(const std::string &Name,
                                    bool AbortOnFailure) override {
        return mm->getPointerToNamedFunction (Name, AbortOnFailure);
    }
    uint8_t *allocateCodeSection(uintptr_t Size, unsigned Alignment,
                                 unsigned SectionID, llvm::StringRef SectionName) override {
        return mm->allocateCodeSection(Size, Alignment, SectionID, SectionName);
    }
    uint8_t *allocateDataSection(uintptr_t Size, unsigned Alignment,
                                 unsigned SectionID, llvm::StringRef SectionName,
                                 bool IsReadOnly) override {
        return mm->allocateDataSection(Size, Alignment, SectionID,
                                       SectionName, IsReadOnly);
    }
    void registerEHFrames(uint8_t *Addr, uint64_t LoadAddr, size_t Size) override {
        mm->registerEHFrames (Addr, LoadAddr, Size);
    }
    #if OSL_LLVM_VERSION < 50
        void deregisterEHFrames(uint8_t *Addr, uint64_t LoadAddr, size_t Size) override {
            mm->deregisterEHFrames(Addr, LoadAddr, Size);
        }
    #else
        void deregisterEHFrames() override {
            mm->deregisterEHFrames();
        }
    #endif

    uint64_t getSymbolAddress(const std::string &Name) override {
        return mm->getSymbolAddress (Name);
    }

    bool finalizeMemory(std::string *ErrMsg) override {
        return mm->finalizeMemory (ErrMsg);
    }
};



#if OSL_LLVM_VERSION <= 38
class LLVM_Util::IRBuilder : public llvm::IRBuilder<true,llvm::ConstantFolder,
                                        llvm::IRBuilderDefaultInserter<true> > {
    typedef llvm::IRBuilder<true, llvm::ConstantFolder,
                                  llvm::IRBuilderDefaultInserter<true> > Base;
public:
    IRBuilder(llvm::BasicBlock *TheBB) : Base(TheBB) {}
};
#else
class LLVM_Util::IRBuilder : public llvm::IRBuilder<llvm::ConstantFolder,
                                               llvm::IRBuilderDefaultInserter> {
    typedef llvm::IRBuilder<llvm::ConstantFolder,
                            llvm::IRBuilderDefaultInserter> Base;
public:
    IRBuilder(llvm::BasicBlock *TheBB) : Base(TheBB) {}
};
#endif



LLVM_Util::LLVM_Util (int debuglevel, const LLVM_Util::PerThreadInfo &per_thread_info)
    : m_debug(debuglevel), m_thread(nullptr),
      m_llvm_context(NULL), m_llvm_module(NULL),
      m_builder(NULL), m_llvm_jitmm(NULL),
      m_current_function(NULL),
      m_llvm_module_passes(NULL), m_llvm_func_passes(NULL),
      m_llvm_exec(NULL),
      m_masked_exit_count(0),
      mVTuneNotifier(nullptr),
      m_llvm_debug_builder(nullptr),
      mDebugCU(nullptr),
      mSubTypeForInlinedFunction(nullptr)
{
    SetupLLVM ();
    m_thread = per_thread_info.get();
    ASSERT (m_thread);
    {
        OIIO::spin_lock lock (llvm_global_mutex);
        if (! m_thread->llvm_context)
            m_thread->llvm_context = new llvm::LLVMContext();

        if (! m_thread->llvm_jitmm) {
#if USE_OLD_JIT
            m_thread->llvm_jitmm = llvm::JITMemoryManager::CreateDefaultMemManager();
#else
            m_thread->llvm_jitmm = new LLVMMemoryManager;
#endif
            ASSERT (m_thread->llvm_jitmm);
            jitmm_hold.push_back (std::shared_ptr<LLVMMemoryManager>(m_thread->llvm_jitmm));
        }
#if USE_OLD_JIT
        m_llvm_jitmm = new MemoryManager(m_thread->llvm_jitmm);
#else
        // Hold the REAL manager and use it as an argument later
        m_llvm_jitmm = reinterpret_cast<MemoryManager*>(m_thread->llvm_jitmm);
#endif
    }

    m_llvm_context = m_thread->llvm_context;

    // Set up aliases for types we use over and over
    m_llvm_type_float = (llvm::Type *) llvm::Type::getFloatTy (*m_llvm_context);
    m_llvm_type_double = (llvm::Type *) llvm::Type::getDoubleTy (*m_llvm_context);
    m_llvm_type_int = (llvm::Type *) llvm::Type::getInt32Ty (*m_llvm_context);
    if (sizeof(char *) == 4)
        m_llvm_type_addrint = (llvm::Type *) llvm::Type::getInt32Ty (*m_llvm_context);
    else
        m_llvm_type_addrint = (llvm::Type *) llvm::Type::getInt64Ty (*m_llvm_context);
    m_llvm_type_int_ptr = (llvm::PointerType *) llvm::Type::getInt32PtrTy (*m_llvm_context);
    m_llvm_type_bool = (llvm::Type *) llvm::Type::getInt1Ty (*m_llvm_context);
    m_llvm_type_bool_ptr = (llvm::PointerType *) llvm::Type::getInt1PtrTy (*m_llvm_context);
    m_llvm_type_char = (llvm::Type *) llvm::Type::getInt8Ty (*m_llvm_context);
    m_llvm_type_longlong = (llvm::Type *) llvm::Type::getInt64Ty (*m_llvm_context);
    m_llvm_type_void = (llvm::Type *) llvm::Type::getVoidTy (*m_llvm_context);
    m_llvm_type_char_ptr = (llvm::PointerType *) llvm::Type::getInt8PtrTy (*m_llvm_context);
    m_llvm_type_float_ptr = (llvm::PointerType *) llvm::Type::getFloatPtrTy (*m_llvm_context);
    m_llvm_type_ustring_ptr = (llvm::PointerType *) llvm::PointerType::get (m_llvm_type_char_ptr, 0);
    m_llvm_type_void_ptr = m_llvm_type_char_ptr;

    // A triple is a struct composed of 3 floats
    std::vector<llvm::Type*> triplefields(3, m_llvm_type_float);
    m_llvm_type_triple = type_struct (triplefields, "Vec3");
    m_llvm_type_triple_ptr = (llvm::PointerType *) llvm::PointerType::get (m_llvm_type_triple, 0);

    // A matrix is a struct composed 16 floats
    std::vector<llvm::Type*> matrixfields(16, m_llvm_type_float);
    m_llvm_type_matrix = type_struct (matrixfields, "Matrix4");
    m_llvm_type_matrix_ptr = (llvm::PointerType *) llvm::PointerType::get (m_llvm_type_matrix, 0);

    // Setup up wide aliases
    m_vector_width = SimdLaneCount;
    // TODO:  why are there casts to the base class llvm::Type *?  
    m_llvm_type_wide_float = llvm::VectorType::get(m_llvm_type_float, m_vector_width);
    m_llvm_type_wide_double = llvm::VectorType::get(m_llvm_type_double, m_vector_width);
    m_llvm_type_wide_int = llvm::VectorType::get(m_llvm_type_int, m_vector_width);
    m_llvm_type_wide_bool = llvm::VectorType::get(m_llvm_type_bool, m_vector_width);
    m_llvm_type_wide_char = llvm::VectorType::get(m_llvm_type_char, m_vector_width);
    
    m_llvm_type_wide_char_ptr = llvm::PointerType::get(m_llvm_type_wide_char, 0);    
    m_llvm_type_wide_ustring_ptr = llvm::VectorType::get(m_llvm_type_char_ptr, m_vector_width);
    m_llvm_type_wide_void_ptr = llvm::VectorType::get(m_llvm_type_void_ptr, m_vector_width);
    m_llvm_type_wide_int_ptr = llvm::PointerType::get(m_llvm_type_wide_int, 0);
    m_llvm_type_wide_bool_ptr = llvm::PointerType::get(m_llvm_type_wide_bool, 0);
    m_llvm_type_wide_float_ptr = llvm::PointerType::get(m_llvm_type_wide_float, 0);

    // A triple is a struct composed of 3 floats
    std::vector<llvm::Type*> triple_wide_fields(3, m_llvm_type_wide_float);
    m_llvm_type_wide_triple = type_struct (triple_wide_fields, "WideVec3");
    
    // A matrix is a struct composed 16 floats
    std::vector<llvm::Type*> matrix_wide_fields(16, m_llvm_type_wide_float);
    m_llvm_type_wide_matrix = type_struct (matrix_wide_fields, "WideMatrix4");
}



LLVM_Util::~LLVM_Util ()
{
    execengine (NULL);
    delete m_llvm_module_passes;
    delete m_llvm_func_passes;
    delete m_builder;
    delete m_llvm_debug_builder;
    module (NULL);
    // DO NOT delete m_llvm_jitmm;  // just the dummy wrapper around the real MM
}



void
LLVM_Util::SetupLLVM ()
{
    OIIO::spin_lock lock (llvm_global_mutex);
    if (setup_done)
        return;
    // Some global LLVM initialization for the first thread that
    // gets here.

#if OSL_LLVM_VERSION < 35
    // enable it to be thread-safe
    llvm::llvm_start_multithreaded ();
#endif
// new versions (>=3.5)don't need this anymore


#if USE_MCJIT
    LLVMInitializeNativeTarget();
    LLVMInitializeNativeDisassembler();
    LLVMInitializeNativeAsmPrinter();
    LLVMInitializeNativeAsmParser();
    LLVMLinkInMCJIT();
#else
    llvm::InitializeNativeTarget();
#endif

    if (debug()) {
#if OSL_LLVM_VERSION <= 36
# define OSL_TGT_DEF(t) t->
        for (llvm::TargetRegistry::iterator t = llvm::TargetRegistry::begin();
             t != llvm::TargetRegistry::end();  ++t)
#else
# define OSL_TGT_DEF(t) t.
        for (auto t : llvm::TargetRegistry::targets())
#endif
        {
            std::cout << "Target: '" << OSL_TGT_DEF(t)getName() << "' "
                      << OSL_TGT_DEF(t)getShortDescription() << "\n";
        }
#undef OSL_TGT_DEF
        std::cout << "\n";
    }

    setup_done = true;
}



llvm::Module *
LLVM_Util::new_module (const char *id)
{
    return new llvm::Module(id, context());
}

bool
LLVM_Util::debug_is_enabled() const
{
    return m_llvm_debug_builder != nullptr;
}


void
LLVM_Util::debug_setup_compilation_unit(const char * compile_unit_name) {
    ASSERT(debug_is_enabled());
    ASSERT(mDebugCU == nullptr);

    OSL_DEV_ONLY(std::cout << "debug_setup_compilation_unit"<< std::endl);


	mDebugCU = m_llvm_debug_builder->createCompileUnit(
			/*llvm::dwarf::DW_LANG_C*/
			llvm::dwarf::DW_LANG_C_plus_plus
			,
# if OSL_LLVM_VERSION >= 40
			m_llvm_debug_builder->createFile(compile_unit_name, // filename
					"." // directory
					),
#else
					compile_unit_name, // filename
			".", // directory
#endif
			"OSLv1.9", // Identify the producer of debugging information and code. Usually this is a compiler version string.
			true /*false*/ , // isOptimized
			"", // This string lists command line options. This string is directly embedded in debug info output which may be used by a tool analyzing generated debugging information.
			1900, // This indicates runtime version for languages like Objective-C
			StringRef(), // SplitName = he name of the file that we'll split debug info out into.
			DICompileUnit::DebugEmissionKind::LineTablesOnly, // DICompileUnit::DebugEmissionKind
			0, // The DWOId if this is a split skeleton compile unit.
			false /*true*/, // SplitDebugInlining = Whether to emit inline debug info.
			true // DebugInfoForProfiling (default=false) = Whether to emit extra debug info for profile collection.
			);

	OSL_DEV_ONLY(std::cout << "created debug module for " << compile_unit_name << std::endl);
}

void
LLVM_Util::debug_push_function(
	const std::string & function_name,
	OIIO::ustring file_name,
	unsigned int method_line)
{
    ASSERT(debug_is_enabled());
#ifdef OSL_DEV
	std::cout << "debug_push_function function_name="<< function_name.c_str()
			  << " file_name=" << file_name.c_str()
			  << " method_line=" << method_line << std::endl;
#endif

    llvm::DIFile * file = getOrCreateDebugFileFor(file_name.c_str());
    const unsigned int method_scope_line = 0;

    // Rather than use dummy function parameters, we'll just reuse
    // the inlined subroutine type of void func(void).
    // TODO:  Added DIType * for ShaderGlobalsBatch  And Groupdata to be
    // passed into this function so proper function type can be created.
#if 0
    llvm::DISubroutineType *subType;
    {
        llvm::SmallVector<llvm::Metadata *, 8> EltTys;
        //llvm::DIType *DblTy = KSTheDebugInfo.getDoubleTy();
        llvm::DIType *debug_double_type = m_llvm_debug_builder->createBasicType(
# if OSL_LLVM_VERSION >= 40
                "double", 64, llvm::dwarf::DW_ATE_float);
#else
                "double",
                64, 64, llvm::dwarf::DW_ATE_float);
#endif
        EltTys.push_back(debug_double_type);
        EltTys.push_back(debug_double_type);

        subType = m_llvm_debug_builder->createSubroutineType(
                m_llvm_debug_builder->getOrCreateTypeArray(EltTys));
    }
#endif

    ASSERT(file);
    llvm::DISubprogram *function = m_llvm_debug_builder->createFunction(
            mDebugCU, // Scope
            function_name.c_str(),  // Name
            /*function_name.c_str()*/ llvm::StringRef(), // Linkage Name
            file, // File
            method_line, // Line Number
            mSubTypeForInlinedFunction, // subroutine type
            false, // isLocalToUnit
            true,  // isDefinition
            method_scope_line,  // Scope Line
            llvm::DINode::FlagPrototyped, // Flags
            false // isOptimized
            );

    ASSERT(mLexicalBlocks.empty());
	current_function()->setSubprogram(function);
    mLexicalBlocks.push_back(function);
}


void
LLVM_Util::debug_push_inlined_function(
	OIIO::ustring function_name,
	OIIO::ustring file_name,
	unsigned int method_line)
{
#ifdef OSL_DEV
    std::cout << "debug_push_inlined_function function_name="<< function_name.c_str()
              << " file_name=" << file_name.c_str()
              << " method_line=" << method_line << std::endl;
#endif

    ASSERT(debug_is_enabled());
    ASSERT(m_builder);
    ASSERT(m_builder->getCurrentDebugLocation().get() != NULL);
    mInliningSites.push_back(m_builder->getCurrentDebugLocation().get());

    llvm::DIFile * file = getOrCreateDebugFileFor(file_name.c_str());
    unsigned int method_scope_line = 0;

    ASSERT(getCurrentDebugScope());

    llvm::DINode::DIFlags fnFlags = (llvm::DINode::DIFlags)(llvm::DINode::FlagPrototyped | llvm::DINode::FlagNoReturn);
    llvm::DISubprogram *function = nullptr;
    function = m_llvm_debug_builder->createFunction(
        mDebugCU, // Scope
        function_name.c_str(),  // Name
        // We are inlined function so not sure supplying a linkage name makes sense
        /*function_name.c_str()*/llvm::StringRef(), // Linkage Name
        file, // File
        method_line, // Line Number
        mSubTypeForInlinedFunction, // subroutine type
        true, // isLocalToUnit
        true, // isDefinition
        method_scope_line, // Scope Line
        fnFlags, // Flags
        true /*false*/ //isOptimized
        );

    mLexicalBlocks.push_back(function);
}

void
LLVM_Util::debug_pop_inlined_function()
{
	OSL_DEV_ONLY(std::cout << "debug_pop_inlined_function"<< std::endl);
    ASSERT(debug_is_enabled());

	ASSERT(!mLexicalBlocks.empty());

	llvm::DIScope *scope = mLexicalBlocks.back();
    auto *existingLbf = dyn_cast<llvm::DILexicalBlockFile>(scope);
    if (existingLbf) {
        // Allow nesting of exactly one DILexicalBlockFile
        // Unwrap it to a function
        scope = existingLbf->getScope();
        OSL_DEV_ONLY(std::cout << "DILexicalBlockFile popped"<< std::endl);
    }

    auto *function = dyn_cast<llvm::DISubprogram>(scope);
	ASSERT(function);
	mLexicalBlocks.pop_back();

	m_llvm_debug_builder->finalizeSubprogram(function);

	// Return debug location to where the function was inlined from
	// Necessary to avoid unnecessarily creating DILexicalBlockFile
	// if the source file changed
    llvm::DILocation *location_inlined_at = mInliningSites.back();
    ASSERT(location_inlined_at);
    ASSERT(m_builder);
    m_builder->SetCurrentDebugLocation(llvm::DebugLoc(location_inlined_at));
    mInliningSites.pop_back();


}

void
LLVM_Util::debug_pop_function()
{
    OSL_DEV_ONLY(std::cout << "debug_pop_function"<< std::endl);
    ASSERT(debug_is_enabled());

    ASSERT(!mLexicalBlocks.empty());
    llvm::DIScope *scope = mLexicalBlocks.back();
    auto *existingLbf = dyn_cast<llvm::DILexicalBlockFile>(scope);
    if (existingLbf) {
        // Allow nesting of exactly one DILexicalBlockFile
        // Unwrap it to a function
        scope = existingLbf->getScope();
        OSL_DEV_ONLY(std::cout << "DILexicalBlockFile popped"<< std::endl);
    }

    auto *function = dyn_cast<llvm::DISubprogram>(scope);
    ASSERT(function);

    mLexicalBlocks.pop_back();
    ASSERT(mLexicalBlocks.empty());

    // Make sure our current debug location isn't pointing at a subprogram
    // that has been finalized, point it back to the compilation unit
    ASSERT(m_builder);
    ASSERT(m_builder->getCurrentDebugLocation().get() != nullptr);
    m_builder->SetCurrentDebugLocation(llvm::DebugLoc::get(static_cast<unsigned int>(1),
                static_cast<unsigned int>(0), /* column?  we don't know it, may be worth tracking through osl->oso*/
                getCurrentDebugScope()));

    m_llvm_debug_builder->finalizeSubprogram(function);


}

void
LLVM_Util::debug_set_location(ustring source_file_name, int sourceline)
{
    OSL_DEV_ONLY(std::cout << "LLVM_Util::debug_set_location:" << source_file_name.c_str() << "(" << sourceline << ")" << std::endl);
    ASSERT(debug_is_enabled());
    ASSERT(sourceline > 0 && "GDB doesn't like 0 because its a nonsensical as a line number");

    llvm::DIScope *sp = getCurrentDebugScope();
    llvm::DILocation *inlineSite = getCurrentInliningSite();
    ASSERT(sp != nullptr);

    // If the file changed on us (due to an #include or inlined function that we missed) update the scope
    // As we do model inlined functions, don't expect this code path to be taken
    // unless support for the functioncall_nr has been disabled.
    if(sp->getFilename().compare(StringRef(source_file_name.c_str())))
    {
        llvm::DIFile * file = getOrCreateDebugFileFor(source_file_name.c_str());

        // Don't nest DILexicalBlockFile's (don't allow DILexicalBlockFile's
        // to be a parent to another DILexicalBlockFile's).
        // Instead make the parent of the new DILexicalBlockFile
        // the same as the existing DILexicalBlockFile's parent.
        auto *existingLbf = dyn_cast<llvm::DILexicalBlockFile>(sp);
        bool requiresNewLBF = true;
        llvm::DIScope *parentScope;
        if (existingLbf) {
            parentScope = existingLbf->getScope();
            // Only allow a single LBF, check for any logic bugs here
            ASSERT(!dyn_cast<llvm::DILexicalBlockFile>(parentScope));
            // If the parent scope has the same filename, no need to create a LBF
            // we can directly use the parentScope
            if (!parentScope->getFilename().compare(StringRef(source_file_name.c_str())))
            {
                // The parent scope has the same file name, we can just use it directly
                sp = parentScope;
                requiresNewLBF = false;
            }
        } else {
            parentScope = sp;
        }
        if (requiresNewLBF) {
            ASSERT(parentScope != nullptr);
            llvm::DILexicalBlockFile *lbf = m_llvm_debug_builder->createLexicalBlockFile(parentScope, file);
            OSL_DEV_ONLY(std::cout << "createLexicalBlockFile" << std::endl);
            sp = lbf;
        }

        // Swap out the current scope for a scope to the correct file
        mLexicalBlocks.pop_back();
        mLexicalBlocks.push_back(sp);
    }
    ASSERT(sp != NULL);


    ASSERT(m_builder);
    const llvm::DebugLoc & current_debug_location = m_builder->getCurrentDebugLocation();
    bool newDebugLocation = true;
    if (current_debug_location)
    {
        if(sourceline == current_debug_location.getLine() &&
           sp == current_debug_location.getScope() &&
           inlineSite == current_debug_location.getInlinedAt () ) {
            newDebugLocation = false;
        }
    }

    if (newDebugLocation)
    {
        llvm::DebugLoc debug_location =
                llvm::DebugLoc::get(static_cast<unsigned int>(sourceline),
                        static_cast<unsigned int>(0), /* column?  we don't know it, may be worth tracking through osl->oso*/
                        sp,
                        inlineSite);
        m_builder->SetCurrentDebugLocation(debug_location);
    }
}

#if OSL_LLVM_VERSION >= 35
#if OSL_LLVM_VERSION < 40
inline bool error_string (const std::error_code &err, std::string *str) {
    if (err) {
        if (str) *str = err.message();
        return true;
    }
    return false;
}
#else
inline bool error_string (llvm::Error err, std::string *str) {
    if (err) {
        if (str) {
            llvm::handleAllErrors(std::move(err),
                      [str](llvm::ErrorInfoBase &E) { *str += E.message(); });
        }
        return true;
    }
    return false;
}
#endif
#endif



llvm::Module *
LLVM_Util::module_from_bitcode (const char *bitcode, size_t size,
                                const std::string &name, std::string *err)
{
    if (err)
        err->clear();

#if OSL_LLVM_VERSION <= 35 /* Old JIT vvvvvvvvvvvvvvvvvvvvvvvvvvvv */
    llvm::MemoryBuffer* buf =
        llvm::MemoryBuffer::getMemBuffer (llvm::StringRef(bitcode, size), name);

    // Create a lazily deserialized IR module
    // This can only be done for old JIT
# if OSL_LLVM_VERSION >= 35
    llvm::Module *m = llvm::getLazyBitcodeModule (buf, context()).get();
# else
    llvm::Module *m = llvm::getLazyBitcodeModule (buf, context(), err);
# endif
    // don't delete buf, the module has taken ownership of it

#if 0
    // Debugging: print all functions in the module
    for (llvm::Module::iterator i = m->begin(); i != m->end(); ++i)
        std::cout << "  found " << i->getName().data() << "\n";
#endif
    return m;
#endif /* End of LLVM <= 3.5 Old JIT section ^^^^^^^^^^^^^^^^^^^^^ */


#if OSL_LLVM_VERSION >= 36  /* MCJIT vvvvvvvvvvvvvvvvvvvvvvvvvvvvv */
# if OSL_LLVM_VERSION >= 40
    typedef llvm::Expected<std::unique_ptr<llvm::Module> > ErrorOrModule;
# else
    typedef llvm::ErrorOr<std::unique_ptr<llvm::Module> > ErrorOrModule;
# endif

# if OSL_LLVM_VERSION >= 40 || defined(OSL_FORCE_BITCODE_PARSE)
    llvm::MemoryBufferRef buf =
        llvm::MemoryBufferRef(llvm::StringRef(bitcode, size), name);
#  ifdef OSL_FORCE_BITCODE_PARSE
    //
    // None of the below seems to be an issue for 3.9 and above.
    // In other JIT code I've seen a related issue, though only on OS X.
    // So if it is still is broken somewhere between 3.6 and 3.8: instead of
    // defining OSL_FORCE_BITCODE_PARSE (which is slower), you may want to
    // try prepending a "_" in two methods above:
    //   LLVM_Util::MemoryManager::getPointerToNamedFunction
    //   LLVM_Util::MemoryManager::getSymbolAddress.
    //
    // Using MCJIT should not require unconditionally parsing
    // the bitcode. But for now, when using getLazyBitcodeModule to
    // lazily deserialize the bitcode, MCJIT is unable to find the
    // called functions due to disagreement about whether a leading "_"
    // is part of the symbol name.
    ErrorOrModule ModuleOrErr = llvm::parseBitcodeFile (buf, context());
#  else
    ErrorOrModule ModuleOrErr = llvm::getLazyBitcodeModule(buf, context());
#  endif

# else /* !OSL_FORCE_BITCODE_PARSE */
    std::unique_ptr<llvm::MemoryBuffer> buf (
        llvm::MemoryBuffer::getMemBuffer (llvm::StringRef(bitcode, size), name, false));
    ErrorOrModule ModuleOrErr = llvm::getLazyBitcodeModule(std::move(buf), context());
# endif

    if (err) {
# if OSL_LLVM_VERSION >= 40
        error_string(ModuleOrErr.takeError(), err);
# else
        error_string(ModuleOrErr.getError(), err);
# endif
    }
    llvm::Module *m = ModuleOrErr ? ModuleOrErr->release() : nullptr;
# if 0
    // Debugging: print all functions in the module
    for (llvm::Module::iterator i = m->begin(); i != m->end(); ++i)
        std::cout << "  found " << i->getName().data() << "\n";
# endif
    
    return m;

#endif /* MCJIT ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^ */
}


void
LLVM_Util::push_function_mask(llvm::Value * startMaskValue)
{
	// TODO:  refactor combining 	m_alloca_for_modified_mask_stack & m_masked_return_count_stack
	// into a single stack of FunctionInfo, and change "modified mask" to "function mask"
	// while at it change "exit mask" to "shader mask"

	// As each nested function (that is inlined) will have different control flow,
	// as some lanes of nested function may return early, but that would not affect
	// the lanes of the calling function, we mush have a modified mask stack for each
	// function
    m_alloca_for_modified_mask_stack.push_back(op_alloca(type_wide_bool(), 1, "modified_mask"));
	llvm::Value * loc_of_modified_mask = m_alloca_for_modified_mask_stack.back();
	push_masking_enabled(false);
	op_store(startMaskValue, loc_of_modified_mask);
	pop_masking_enabled();

	m_masked_return_count_stack.push_back(0);

	// Give the new function its own mask so that it may be swapped out
	// to mask out lanes that have returned early,
	// and we can just pop that mask off when the function exits
	push_mask(startMaskValue,  /*negate=*/ false, /*absolute = */ true);
}

int
LLVM_Util::masked_return_count() const
{
	ASSERT(!m_masked_return_count_stack.empty());
	return m_masked_return_count_stack.back();
}

int
LLVM_Util::masked_exit_count() const
{
	OSL_DEV_ONLY(std::cout << "masked_exit_count = " << m_masked_exit_count << std::endl);

	return m_masked_exit_count;
}

void
LLVM_Util::pop_function_mask()
{
	pop_mask();

	ASSERT(!m_alloca_for_modified_mask_stack.empty());
	m_alloca_for_modified_mask_stack.pop_back();

	ASSERT(!m_masked_return_count_stack.empty());
	m_masked_return_count_stack.pop_back();
}

void
LLVM_Util::push_masked_loop(llvm::Value* location_of_condition_mask, llvm::Value* location_of_continue_mask)
{
	// As each nested loop will have different control flow,
	// as some lanes of nested function may 'break' out early, but that would not affect
	// the lanes outside the loop, and we could have nested loops,
	// we mush have a break count for each loop
	m_masked_loop_stack.push_back(LoopInfo{location_of_condition_mask, location_of_continue_mask, 0, 0});
}

bool
LLVM_Util::is_innermost_loop_masked() const
{
	if(m_masked_loop_stack.empty())
		return false;
	return (m_masked_loop_stack.back().location_of_condition_mask != nullptr);
}

int
LLVM_Util::masked_break_count() const
{
	if(m_masked_loop_stack.empty()) {
		return 0;
	} else {
		return m_masked_loop_stack.back().break_count;
	}
}

int
LLVM_Util::masked_continue_count() const
{
	if(m_masked_loop_stack.empty()) {
		return 0;
	} else {
		return m_masked_loop_stack.back().continue_count;
	}
}


void
LLVM_Util::pop_masked_loop()
{
	m_masked_loop_stack.pop_back();
}



void
LLVM_Util::push_shader_instance(llvm::Value * startMaskValue)
{
	push_function_mask(startMaskValue);
}

void
LLVM_Util::pop_shader_instance()
{
	m_masked_exit_count = 0;
	pop_function_mask();
}

void
LLVM_Util::new_builder (llvm::BasicBlock *block)
{
    end_builder();
    if (! block)
        block = new_basic_block ();
    m_builder = new IRBuilder (block);
    if (this->debug_is_enabled()) {
        ASSERT(getCurrentDebugScope());
        m_builder->SetCurrentDebugLocation(llvm::DebugLoc::get(static_cast<unsigned int>(1),
                static_cast<unsigned int>(0), /* column?  we don't know it, may be worth tracking through osl->oso*/
                getCurrentDebugScope()));
    }

    ASSERT(m_masked_exit_count == 0);
	ASSERT(m_alloca_for_modified_mask_stack.empty());
	ASSERT(m_mask_stack.empty());
}


/// Return the current IR builder, create a new one (for the current
/// function) if necessary.
LLVM_Util::IRBuilder &
LLVM_Util::builder () {
    if (! m_builder)
        new_builder ();
    ASSERT(m_builder);
    return *m_builder;
}


void
LLVM_Util::end_builder ()
{
    if (m_builder) {
        delete m_builder;
        m_builder = nullptr;
    }
}



llvm::ExecutionEngine *
LLVM_Util::make_jit_execengine (std::string *err, bool debugging_symbols, bool profiling_events)
{

    OSL_DEV_ONLY(std::cout << "LLVM_Util::make_jit_execengine" << std::endl);

    execengine (NULL);   // delete and clear any existing engine
    if (err)
        err->clear ();
# if OSL_LLVM_VERSION >= 36
    llvm::EngineBuilder engine_builder ((std::unique_ptr<llvm::Module>(module())));
# else /* < 36: */
    llvm::EngineBuilder engine_builder (module());
# endif

    engine_builder.setEngineKind (llvm::EngineKind::JIT);
    engine_builder.setErrorStr (err);
    //engine_builder.setRelocationModel(llvm::Reloc::PIC_);
    //engine_builder.setCodeModel(llvm::CodeModel::Default);
    engine_builder.setVerifyModules(true);

#if USE_OLD_JIT
    engine_builder.setJITMemoryManager (m_llvm_jitmm);
    // N.B. createJIT will take ownership of the the JITMemoryManager!
    engine_builder.setUseMCJIT (0);
#else
    // We are actually holding a LLVMMemoryManager
    engine_builder.setMCJITMemoryManager (std::unique_ptr<llvm::RTDyldMemoryManager>
        (new MemoryManager(reinterpret_cast<LLVMMemoryManager*>(m_llvm_jitmm))));
#endif /* USE_OLD_JIT */

    
    //engine_builder.setOptLevel (llvm::CodeGenOpt::None);
    //engine_builder.setOptLevel (llvm::CodeGenOpt::Default);
    engine_builder.setOptLevel (llvm::CodeGenOpt::Aggressive);
    

    const char * oslDumpAsmString = std::getenv("OSL_DUMP_ASM");
    bool dumpAsm = (oslDumpAsmString != NULL);

#if 1
    llvm::TargetOptions options;
    options.AllowFPOpFusion = llvm::FPOpFusion::Fast;
    options.UnsafeFPMath = true;

    #if OSL_LLVM_VERSION < 40
    // Turn off approximate reciprocals for division. It's too
    // inaccurate even for us. In LLVM 4.0+ this moved to be a
    // function attribute.
    options.Reciprocals.setDefaults("all", false, 0);
    #endif

    options.NoInfsFPMath = true;
    options.NoNaNsFPMath = true;
    options.HonorSignDependentRoundingFPMathOption = false;
    options.NoZerosInBSS = false;
    options.GuaranteedTailCallOpt = false;
    options.StackAlignmentOverride = 0;
    options.FunctionSections = true;
    options.UseInitArray = false;
    bool use_soft_float_abi = false;
    options.FloatABIType =
        use_soft_float_abi ? llvm::FloatABI::Soft : llvm::FloatABI::Hard;
    #if OSL_LLVM_VERSION >= 39
    // Not supported by older linkers
    options.RelaxELFRelocations = false;    
    #endif    
    
    //options.DebuggerTuning = llvm::DebuggerKind::GDB;

    if (dumpAsm) {
        options.PrintMachineCode = true;
    }
    engine_builder.setTargetOptions(options);
    
    
#endif
    
#if 0
//    llvm::TargetOptions options = InitTargetOptionsFromCodeGenFlags();
    llvm::TargetOptions options;
    options.LessPreciseFPMADOption = EnableFPMAD;
   options.AllowFPOpFusion = FuseFPOps;
   options.Reciprocals = TargetRecip(ReciprocalOps);
   options.UnsafeFPMath = EnableUnsafeFPMath;
   options.NoInfsFPMath = EnableNoInfsFPMath;
   options.NoNaNsFPMath = EnableNoNaNsFPMath;
   options.HonorSignDependentRoundingFPMathOption =
		   EnableHonorSignDependentRoundingFPMath;
   if (FloatABIForCalls != FloatABI::Default)
      options.FloatABIType = FloatABIForCalls;
   options.NoZerosInBSS = DontPlaceZerosInBSS;
   options.GuaranteedTailCallOpt = EnableGuaranteedTailCallOpt;
   //options.StackAlignmentOverride = OverrideStackAlignment;
   options.StackAlignmentOverride = 32;
   //options.PositionIndependentExecutable = EnablePIE;
   options.UseInitArray = !UseCtors;
   options.DataSections = DataSections;
   options.FunctionSections = FunctionSections;
   options.UniqueSectionNames = UniqueSectionNames;
   options.EmulatedTLS = EmulatedTLS;
   
   options.MCOptions = InitMCTargetOptionsFromFlags();
   options.JTType = JTableType;
   
   options.ThreadModel = llvm::ThreadModel::Single;
   options.EABIVersion = EABIVersion;
   //options.EABIVersion = EABI::EABI4;
   options.DebuggerTuning = DebuggerTuningOpt;   
   options.RelaxELFRelocations = false;
   //options.PrintMachineCode = true;
   engine_builder.setTargetOptions(options);
#endif
    
   
   enum TargetISA
   {
	   TargetISA_UNLIMITTED,
	   TargetISA_SSE4_2,
	   TargetISA_AVX,
	   TargetISA_AVX2,
	   TargetISA_AVX512
   };
   
   TargetISA oslIsa = TargetISA_UNLIMITTED;
   const char * oslIsaString = std::getenv("OSL_ISA");
   if (oslIsaString != NULL) {
	   if (strcmp(oslIsaString, "SSE4.2") == 0)
	   {
		   oslIsa = TargetISA_SSE4_2;
	   } else if (strcmp(oslIsaString, "AVX") == 0)
	   {
		   oslIsa = TargetISA_AVX;
	   } else if (strcmp(oslIsaString, "AVX2") == 0)
	   {
		   oslIsa = TargetISA_AVX2;
	   } else if (strcmp(oslIsaString, "AVX512") == 0)
	   {
		   oslIsa = TargetISA_AVX512;
	   }
   }
   
    //engine_builder.setMArch("core-avx2");
    OSL_DEV_ONLY(std::cout << std::endl<< "llvm::sys::getHostCPUName()>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>" << llvm::sys::getHostCPUName().str() << std::endl);
    //engine_builder.setMCPU(llvm::sys::getHostCPUName());
    //engine_builder.setMCPU("skylake-avx512");
    //engine_builder.setMCPU("broadwell");
    engine_builder.setMArch("x86-64");    

//    bool disableFMA = true;
    bool disableFMA = false;
    const char * oslNoFmaString = std::getenv("OSL_NO_FMA");
    if (oslNoFmaString != NULL) {
 	   if ((strcmp(oslNoFmaString, "1") == 0) || 
		   (strcmp(oslNoFmaString, "y") == 0) ||
		   (strcmp(oslNoFmaString, "Y") == 0) ||
		   (strcmp(oslNoFmaString, "yes") == 0) ||
		   (strcmp(oslNoFmaString, "t") == 0) ||
		   (strcmp(oslNoFmaString, "true") == 0) ||
		   (strcmp(oslNoFmaString, "T") == 0) ||
		   (strcmp(oslNoFmaString, "TRUE") == 0))
 	   {
 		  disableFMA = true;
 	   } 
    }
    
    llvm::StringMap< bool > cpuFeatures;
    if (llvm::sys::getHostCPUFeatures(cpuFeatures)) {
		m_supports_masked_stores = false;
		m_supports_native_bit_masks = false;

    	OSL_DEV_ONLY(std::cout << std::endl<< "llvm::sys::getHostCPUFeatures()>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>" << std::endl);
		std::vector<std::string> attrvec;
		for (auto &cpuFeature : cpuFeatures) 
		{
			//auto enabled = (cpuFeature.second && (cpuFeature.first().str().find("512") == std::string::npos)) ? "+" : "-";
			auto enabled = (cpuFeature.second) ? "+" : "-";
			//OSL_DEV_ONLY(std::cout << cpuFeature.first().str()  << " is " << enabled << std::endl);
			
			if (oslIsa == TargetISA_UNLIMITTED) {				
				if (!disableFMA || std::string("fma") != cpuFeature.first().str()) {
					attrvec.push_back(enabled + cpuFeature.first().str());
				}

				if(cpuFeature.first().str().find("512") != std::string::npos) {
					m_supports_masked_stores = true;
					m_supports_native_bit_masks = true;
				}
			}
		}
		//The particular format of the names are target dependent, and suitable for passing as -mattr to the target which matches the host.
	//    const char *mattr[] = {"avx"};
	//    std::vector<std::string> attrvec (mattr, mattr+1);

		
		// TODO: consider extending adding all CPU features for different target platforms
		// and also intersecting with supported features vs. blindly adding them
		switch(oslIsa) {
		case TargetISA_SSE4_2:
			attrvec.push_back("+sse4.2");
			OSL_DEV_ONLY(std::cout << "Intended OSL ISA: SSE4.2" << std::endl);
			break;
		case TargetISA_AVX:
			attrvec.push_back("+avx");
			OSL_DEV_ONLY(std::cout << "Intended OSL ISA: AVX" << std::endl);
			break;		
		case TargetISA_AVX2:
			attrvec.push_back("+sse4.2");
			attrvec.push_back("+avx");
			attrvec.push_back("+avx2");
			OSL_DEV_ONLY(std::cout << "Intended OSL ISA: AVX2" << std::endl);
			break;		
		case TargetISA_AVX512:
			m_supports_masked_stores = true;
			m_supports_native_bit_masks = true;
			attrvec.push_back("+avx512f");
			attrvec.push_back("+avx512dq");
			attrvec.push_back("+avx512bw");
			attrvec.push_back("+avx512vl");
			attrvec.push_back("+avx512cd");
			attrvec.push_back("+avx512f");
			
			OSL_DEV_ONLY(std::cout << "Intended OSL ISA: AVX512" << std::endl);
			break;		
		case TargetISA_UNLIMITTED:		
		default:
			break;
		};
		
	    if (disableFMA) {
			attrvec.push_back("-fma");
	    }
		engine_builder.setMAttrs(attrvec);
		
    }
    

    m_llvm_exec = engine_builder.create();        
    if (! m_llvm_exec)
        return NULL;
    
    //const llvm::DataLayout & data_layout = m_llvm_exec->getDataLayout();
    //OSL_DEV_ONLY(std::cout << "data_layout.getStringRepresentation()=" << data_layout.getStringRepresentation() << std::endl);
    		
    
    OSL_DEV_ONLY(TargetMachine * target_machine = m_llvm_exec->getTargetMachine());
    //OSL_DEV_ONLY(std::cout << "target_machine.getTargetCPU()=" << target_machine->getTargetCPU().str() << std::endl);
    OSL_DEV_ONLY(std::cout << "target_machine.getTargetFeatureString ()=" << target_machine->getTargetFeatureString ().str() << std::endl);
	//OSL_DEV_ONLY(std::cout << "target_machine.getTargetTriple ()=" << target_machine->getTargetTriple().str() << std::endl);

#if USE_MCJIT
    // For unknown reasons the MCJIT when constructed registers the GDB listener (which is static)
    // The following is an attempt to unregister it, and pretend it was never registered in the 1st place
    // The underlying GDBRegistrationListener is static, so we are leaking it
    m_llvm_exec->UnregisterJITEventListener(llvm::JITEventListener::createGDBRegistrationListener());
#endif

    if (debugging_symbols) {
        ASSERT(m_llvm_module != nullptr);
        OSL_DEV_ONLY(std::cout << "debugging symbols"<< std::endl);

        module()->addModuleFlag(llvm::Module::Error, "Debug Info Version",
                llvm::DEBUG_METADATA_VERSION);

        unsigned int modulesDebugInfoVersion = 0;
        if (auto *Val = llvm::mdconst::dyn_extract_or_null < llvm::ConstantInt
                > (module()->getModuleFlag("Debug Info Version"))) {
            modulesDebugInfoVersion = Val->getZExtValue();
        }

        ASSERT(m_llvm_debug_builder == nullptr && "Only handle creating the debug builder once");
        m_llvm_debug_builder = new llvm::DIBuilder(*m_llvm_module);

        llvm::SmallVector<llvm::Metadata *, 8> EltTys;
        mSubTypeForInlinedFunction = m_llvm_debug_builder->createSubroutineType(
                        m_llvm_debug_builder->getOrCreateTypeArray(EltTys));

        //  OSL_DEV_ONLY(std::cout)
        //  OSL_DEV_ONLY(       << "------------------>enable_debug_info<-----------------------------module flag['Debug Info Version']= ")
        //  OSL_DEV_ONLY(       << modulesDebugInfoVersion << std::endl);

        // The underlying GDBRegistrationListener is static, so we are leaking it
        m_llvm_exec->RegisterJITEventListener(llvm::JITEventListener::createGDBRegistrationListener());
    }

    if (profiling_events) {
        // TODO:  Create better VTune listener that can handle inline fuctions
        //        https://software.intel.com/en-us/node/544211
        mVTuneNotifier = llvm::JITEventListener::createIntelJITEventListener();
        assert (mVTuneNotifier != NULL);
        m_llvm_exec->RegisterJITEventListener(mVTuneNotifier);
    }

    // Force it to JIT as soon as we ask it for the code pointer,
    // don't take any chances that it might JIT lazily, since we
    // will be stealing the JIT code memory from under its nose and
    // destroying the Module & ExecutionEngine.
    m_llvm_exec->DisableLazyCompilation ();
    return m_llvm_exec;
}


void
LLVM_Util::dump_struct_data_layout(llvm::Type *Ty)
{
	ASSERT(Ty);
	ASSERT(Ty->isStructTy());
			
	llvm::StructType *structTy = static_cast<llvm::StructType *>(Ty);
    const llvm::DataLayout & data_layout = m_llvm_exec->getDataLayout();
    
    int number_of_elements = structTy->getNumElements();


	const StructLayout * layout = data_layout.getStructLayout (structTy);
	std::cout << "dump_struct_data_layout: getSizeInBytes(" << layout->getSizeInBytes() << ") "
		<< " getAlignment(" << layout->getAlignment() << ")"		
		<< " hasPadding(" << layout->hasPadding() << ")" << std::endl;
	for(int index=0; index < number_of_elements; ++index) {
		llvm::Type * et = structTy->getElementType(index);
		std::cout << "   element[" << index << "] offset in bytes = " << layout->getElementOffset(index) << 
				" type is ";
		{
			llvm::raw_os_ostream os_cout(std::cout);
			et->print(os_cout);
		}
		std::cout << std::endl;
	}
		
}

void
LLVM_Util::validate_struct_data_layout(llvm::Type *Ty, const std::vector<unsigned int> & expected_offset_by_index)
{
	ASSERT(Ty);
	ASSERT(Ty->isStructTy());
			
	llvm::StructType *structTy = static_cast<llvm::StructType *>(Ty);
    const llvm::DataLayout & data_layout = m_llvm_exec->getDataLayout();
    
    int number_of_elements = structTy->getNumElements();


	const StructLayout * layout = data_layout.getStructLayout (structTy);
	OSL_DEV_ONLY(std::cout << "dump_struct_data_layout: getSizeInBytes(" << layout->getSizeInBytes() << ") ")
	OSL_DEV_ONLY(	<< " getAlignment(" << layout->getAlignment() << ")")
	OSL_DEV_ONLY(	<< " hasPadding(" << layout->hasPadding() << ")" << std::endl);
	
	for(int index=0; index < number_of_elements; ++index) {
	    OSL_DEV_ONLY(llvm::Type * et = structTy->getElementType(index));
		
		auto actual_offset = layout->getElementOffset(index);

		ASSERT(index < expected_offset_by_index.size());
		

		
		OSL_DEV_ONLY(std::cout << "   element[" << index << "] offset in bytes = " << actual_offset << " expect offset = " << expected_offset_by_index[index] <<)
		OSL_DEV_ONLY(		" type is ");
		{
			llvm::raw_os_ostream os_cout(std::cout);
			OSL_DEV_ONLY(		et->print(os_cout));
		}
				
				
		ASSERT(expected_offset_by_index[index] == actual_offset);
		OSL_DEV_ONLY(std::cout << std::endl);
	}		
	if (expected_offset_by_index.size() != number_of_elements)
	{
		std::cout << "   expected " << expected_offset_by_index.size() << " members but actual member count is = " << number_of_elements << std::endl;
		ASSERT(expected_offset_by_index.size() == number_of_elements);
	}
}


void
LLVM_Util::execengine (llvm::ExecutionEngine *exec)
{
    if (nullptr != m_llvm_exec) {
        if (nullptr != mVTuneNotifier) {
            // We explicitly remove the VTune listener, so it can't be notified of the object's release.
            // As we are holding onto the memory backing the object, this should be fine.
            // It is necessary because a profiler could try and lookup info from an object that otherwise
            // would have been unregistered.
            m_llvm_exec->UnregisterJITEventListener(mVTuneNotifier);

            delete mVTuneNotifier;
            mVTuneNotifier = nullptr;
        }

        if (debug_is_enabled()) {
            // We explicitly remove the GDB listener, so it can't be notified of the object's release.
            // As we are holding onto the memory backing the object, this should be fine.
            // It is necessary because a debugger could try and lookup info from an object that otherwise
            // would have been unregistered.

            // The GDB listener is a static object, we really aren't creating one here
            m_llvm_exec->UnregisterJITEventListener(llvm::JITEventListener::createGDBRegistrationListener());
        }
        delete m_llvm_exec;
    }
    m_llvm_exec = exec;
}



void *
LLVM_Util::getPointerToFunction (llvm::Function *func)
{
    DASSERT (func && "passed NULL to getPointerToFunction");

    if (debug_is_enabled()) {
        // We have to finalize debug info before jit happens
        m_llvm_debug_builder->finalize();
    }

    llvm::ExecutionEngine *exec = execengine();
    if (USE_MCJIT)
        exec->finalizeObject ();

    void *f = exec->getPointerToFunction (func);
    ASSERT (f && "could not getPointerToFunction");
    return f;
}



void
LLVM_Util::InstallLazyFunctionCreator (void* (*P)(const std::string &))
{
    llvm::ExecutionEngine *exec = execengine();
    exec->InstallLazyFunctionCreator (P);
}



void
LLVM_Util::setup_optimization_passes (int optlevel)
{
    ASSERT (m_llvm_module_passes == NULL && m_llvm_func_passes == NULL);
    OSL_DEV_ONLY(std::cout << "setup_optimization_passes " << optlevel);

    // Construct the per-function passes and module-wide (interprocedural
    // optimization) passes.
    //
    // LLVM keeps changing names and call sequence. This part is easier to
    // understand if we explicitly break it into individual LLVM versions.
#if OSL_LLVM_VERSION >= 37

    m_llvm_func_passes = new llvm::legacy::FunctionPassManager(module());
    llvm::legacy::FunctionPassManager &fpm = (*m_llvm_func_passes);

    m_llvm_module_passes = new llvm::legacy::PassManager;
    llvm::legacy::PassManager &mpm = (*m_llvm_module_passes);

#elif OSL_LLVM_VERSION >= 36

    m_llvm_func_passes = new llvm::legacy::FunctionPassManager(module());
    llvm::legacy::FunctionPassManager &fpm (*m_llvm_func_passes);
    fpm.add (new llvm::DataLayoutPass());

    m_llvm_module_passes = new llvm::legacy::PassManager;
    llvm::legacy::PassManager &mpm (*m_llvm_module_passes);
    mpm.add (new llvm::DataLayoutPass());

#elif OSL_LLVM_VERSION == 35

    m_llvm_func_passes = new llvm::legacy::FunctionPassManager(module());
    llvm::legacy::FunctionPassManager &fpm (*m_llvm_func_passes);
    fpm.add (new llvm::DataLayoutPass(module()));

    m_llvm_module_passes = new llvm::legacy::PassManager;
    llvm::legacy::PassManager &mpm (*m_llvm_module_passes);
    mpm.add (new llvm::DataLayoutPass(module()));

#elif OSL_LLVM_VERSION == 34

    m_llvm_func_passes = new llvm::legacy::FunctionPassManager(module());
    llvm::legacy::FunctionPassManager &fpm (*m_llvm_func_passes);
    fpm.add (new llvm::DataLayout(module()));

    m_llvm_module_passes = new llvm::legacy::PassManager;
    llvm::legacy::PassManager &mpm (*m_llvm_module_passes);
    mpm.add (new llvm::DataLayout(module()));

#endif

    if (optlevel >= 1 && optlevel <= 3) {
        // For LLVM 3.0 and higher, llvm_optimize 1-3 means to use the
        // same set of optimizations as clang -O1, -O2, -O3
        llvm::PassManagerBuilder builder;
        builder.OptLevel = optlevel;
        builder.Inliner = llvm::createFunctionInliningPass();
        // builder.DisableUnrollLoops = true;
        builder.populateFunctionPassManager (fpm);
        builder.populateModulePassManager (mpm);
    } else {
        // Unknown choices for llvm_optimize: use the same basic
        // set of passes that we always have.

        // Always add verifier?
        mpm.add (llvm::createVerifierPass());
        // Simplify the call graph if possible (deleting unreachable blocks, etc.)
        mpm.add (llvm::createCFGSimplificationPass());
        // Change memory references to registers
        //  mpm.add (llvm::createPromoteMemoryToRegisterPass());
#if OSL_LLVM_VERSION <= 36
        // Is there a replacement for this in newer LLVM?
        mpm.add (llvm::createScalarReplAggregatesPass());
#endif
        // Combine instructions where possible -- peephole opts & bit-twiddling
        mpm.add (llvm::createInstructionCombiningPass());
        // Inline small functions
        mpm.add (llvm::createFunctionInliningPass());  // 250?
        // Eliminate early returns
        mpm.add (llvm::createUnifyFunctionExitNodesPass());
        // resassociate exprssions (a = x + (3 + y) -> a = x + y + 3)
        mpm.add (llvm::createReassociatePass());
        // Eliminate common sub-expressions
        mpm.add (llvm::createGVNPass());
        // Constant propagation with SCCP
        mpm.add (llvm::createSCCPPass());
        // More dead code elimination
        mpm.add (llvm::createAggressiveDCEPass());
        // Combine instructions where possible -- peephole opts & bit-twiddling
        mpm.add (llvm::createInstructionCombiningPass());
        // Simplify the call graph if possible (deleting unreachable blocks, etc.)
        mpm.add (llvm::createCFGSimplificationPass());
        // Try to make stuff into registers one last time.
        mpm.add (llvm::createPromoteMemoryToRegisterPass());
    }
}



void
LLVM_Util::do_optimize (std::string *out_err)
{
    ASSERT(m_llvm_module && "No module to optimize!");

#if OSL_LLVM_VERSION > 35 && !defined(OSL_FORCE_BITCODE_PARSE)
    LLVMErr err = m_llvm_module->materializeAll();
    if (error_string(std::move(err), out_err))
        return;
#endif

    m_llvm_func_passes->doInitialization();
    m_llvm_module_passes->run (*m_llvm_module);
    m_llvm_func_passes->doFinalization();
}



void
LLVM_Util::internalize_module_functions (const std::string &prefix,
                                         const std::vector<std::string> &exceptions,
                                         const std::vector<std::string> &moreexceptions)
{
#if OSL_LLVM_VERSION < 40
    for (llvm::Module::iterator iter = module()->begin(); iter != module()->end(); iter++) {
        llvm::Function *sym = static_cast<llvm::Function*>(iter);
#else
    for (llvm::Function& func : module()->getFunctionList()) {
        llvm::Function *sym = &func;
#endif
        std::string symname = sym->getName();
        if (prefix.size() && ! OIIO::Strutil::starts_with(symname, prefix))
            continue;
        bool needed = false;
        for (size_t i = 0, e = exceptions.size(); i < e; ++i)
            if (sym->getName() == exceptions[i]) {
                needed = true;
                // OSL_DEV_ONLY(std::cout << "    necessary LLVM module function ")
                // OSL_DEV_ONLY(          << sym->getName().str() << "\n");
                break;
            }
        for (size_t i = 0, e = moreexceptions.size(); i < e; ++i)
            if (sym->getName() == moreexceptions[i]) {
                needed = true;
                // OSL_DEV_ONLY(std::cout << "    necessary LLVM module function ")
                // OSL_DEV_ONLY(          << sym->getName().str() << "\n");
                break;
            }
        if (!needed) {
            llvm::GlobalValue::LinkageTypes linkage = sym->getLinkage();
            // OSL_DEV_ONLY(std::cout << "    unnecessary LLVM module function ")
            // OSL_DEV_ONLY(          << sym->getName().str() << " linkage " << int(linkage) << "\n");
            if (linkage == llvm::GlobalValue::ExternalLinkage)
                sym->setLinkage (llvm::GlobalValue::LinkOnceODRLinkage);
            // ExternalLinkage means it's potentially externally callable,
            // and so will definitely have code generated.
            // LinkOnceODRLinkage keeps one copy so it can be inlined or
            // called internally to the module, but allows it to be
            // discarded otherwise.
        }
    }
#if 0
    // I don't think we need to worry about linkage of global symbols, but
    // here is an example of how to iterate over the globals anyway.
    for (llvm::Module::global_iterator iter = module()->global_begin(); iter != module()->global_end(); iter++) {
        llvm::GlobalValue *sym = llvm::dyn_cast<llvm::GlobalValue>(iter);
        if (!sym)
            continue;
        std::string symname = sym->getName();
        if (prefix.size() && ! OIIO::Strutil::starts_with(symname, prefix))
            continue;
        bool needed = false;
        for (size_t i = 0, e = exceptions.size(); i < e; ++i)
            if (sym->getName() == exceptions[i]) {
                needed = true;
                break;
            }
        if (! needed) {
            llvm::GlobalValue::LinkageTypes linkage = sym->getLinkage();
            // OSL_DEV_ONLY(std::cout << "    unnecessary LLVM global " << sym->getName().str())
            // OSL_DEV_ONLY(          << " linkage " << int(linkage) << "\n");
            if (linkage == llvm::GlobalValue::ExternalLinkage)
                f->setLinkage (llvm::GlobalValue::LinkOnceODRLinkage);
        }
    }
#endif
}


#if OSL_LLVM_VERSION < 50
llvm::Function *
LLVM_Util::make_function (const std::string &name, bool fastcall,
                          llvm::Type *rettype,
                          llvm::Type *arg1,
                          llvm::Type *arg2,
                          llvm::Type *arg3,
                          llvm::Type *arg4)
{
    llvm::Function *func = llvm::cast<llvm::Function>(
        module()->getOrInsertFunction (name, rettype,
                                       arg1, arg2, arg3, arg4, NULL));

    if (fastcall)
        func->setCallingConv(llvm::CallingConv::Fast);
    return func;
}
#else
llvm::Function *
LLVM_Util::make_function (const std::string &name, bool fastcall,
                          llvm::Type *rettype,
                          llvm::Type *arg1,
                          llvm::Type *arg2)
{
    llvm::Function *func = llvm::cast<llvm::Function>(
    // TODO: verify this is correct for LLVM 5.0
    module()->getOrInsertFunction (name, rettype,
                                   arg1, arg2));

    if (fastcall)
        func->setCallingConv(llvm::CallingConv::Fast);
    return func;
}

llvm::Function *
LLVM_Util::make_function (const std::string &name, bool fastcall,
                          llvm::Type *rettype,
                          llvm::Type *arg1,
                          llvm::Type *arg2,
						  llvm::Type *arg3)
{
    llvm::Function *func = llvm::cast<llvm::Function>(
    // TODO: verify this is correct for LLVM 5.0
    module()->getOrInsertFunction (name, rettype,
                                   arg1, arg2, arg3));

    if (fastcall)
        func->setCallingConv(llvm::CallingConv::Fast);
    return func;
}
#endif


llvm::Function *
LLVM_Util::make_function (const std::string &name, bool fastcall,
                          llvm::Type *rettype,
                          const std::vector<llvm::Type*> &params,
                          bool varargs)
{
    llvm::FunctionType *functype = type_function (rettype, params, varargs);
    llvm::Constant *c = module()->getOrInsertFunction (name, functype);
    ASSERT (c && "getOrInsertFunction returned NULL");
    ASSERT_MSG (llvm::isa<llvm::Function>(c),
                "Declaration for %s is wrong, LLVM had to make a cast", name.c_str());
    llvm::Function *func = llvm::cast<llvm::Function>(c);
    if (fastcall) {
    	
    	OSL_DEV_ONLY(std::cout << ">>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>FAST_CALL MAKE FUNCTION=" << name << std::endl);
        func->setCallingConv(llvm::CallingConv::Fast);
    }
    return func;
}



llvm::Value *
LLVM_Util::current_function_arg (int a)
{
    llvm::Function::arg_iterator arg_it = current_function()->arg_begin();
    for (int i = 0;  i < a;  ++i)
        ++arg_it;
#if OSL_LLVM_VERSION <= 36
    return arg_it;
#else
    return &(*arg_it);
#endif
}



llvm::BasicBlock *
LLVM_Util::new_basic_block (const std::string &name)
{
    return llvm::BasicBlock::Create (context(), name, current_function());
}



llvm::BasicBlock *
LLVM_Util::push_function (llvm::BasicBlock *after)
{
	OSL_DEV_ONLY(std::cout << "push_function" << std::endl);

    if (! after)
        after = new_basic_block ("after_function");
    m_return_block.push_back (after);

    return after;
}

bool
LLVM_Util::inside_function() const
{
	return (false == m_return_block.empty());
}

void
LLVM_Util::pop_function ()
{
	OSL_DEV_ONLY(std::cout << "pop_function" << std::endl);

    ASSERT (! m_return_block.empty());
    builder().SetInsertPoint (m_return_block.back());
    m_return_block.pop_back ();
}


void LLVM_Util::push_masked_return_block(llvm::BasicBlock *test_return)
{
	OSL_DEV_ONLY(std::cout << "push_masked_return_block" << std::endl);

	m_masked_return_block_stack.push_back (test_return);
}

void LLVM_Util::pop_masked_return_block()
{
	OSL_DEV_ONLY(std::cout << "pop_masked_return_block" << std::endl);
	ASSERT(!m_masked_return_block_stack.empty());
	m_masked_return_block_stack.pop_back();
}

bool
LLVM_Util::has_masked_return_block() const
{
    return (! m_masked_return_block_stack.empty());
}

llvm::BasicBlock *
LLVM_Util::masked_return_block() const
{
    ASSERT (! m_masked_return_block_stack.empty());
    return m_masked_return_block_stack.back();
}


llvm::BasicBlock *
LLVM_Util::return_block () const
{
    ASSERT (! m_return_block.empty());
    return m_return_block.back();
}



void 
LLVM_Util::push_loop (llvm::BasicBlock *step, llvm::BasicBlock *after)
{
    m_loop_step_block.push_back (step);
    m_loop_after_block.push_back (after);
}



void 
LLVM_Util::pop_loop ()
{
    ASSERT (! m_loop_step_block.empty() && ! m_loop_after_block.empty());
    m_loop_step_block.pop_back ();
    m_loop_after_block.pop_back ();
}



llvm::BasicBlock *
LLVM_Util::loop_step_block () const
{
    ASSERT (! m_loop_step_block.empty());
    return m_loop_step_block.back();
}



llvm::BasicBlock *
LLVM_Util::loop_after_block () const
{
    ASSERT (! m_loop_after_block.empty());
    return m_loop_after_block.back();
}




llvm::Type *
LLVM_Util::type_union(const std::vector<llvm::Type *> &types)
{
    llvm::DataLayout target(module());
    size_t max_size = 0;
    size_t max_align = 1;
    for (size_t i = 0; i < types.size(); ++i) {
        size_t size = target.getTypeStoreSize(types[i]);
        size_t align = target.getABITypeAlignment(types[i]);
        max_size  = size  > max_size  ? size  : max_size;
        max_align = align > max_align ? align : max_align;
    }
    size_t padding = (max_size % max_align) ? max_align - (max_size % max_align) : 0;
    size_t union_size = max_size + padding;

    llvm::Type * base_type = NULL;
    // to ensure the alignment when included in a struct use
    // an appropiate type for the array
    if (max_align == sizeof(void*))
        base_type = type_void_ptr();
    else if (max_align == 4)
        base_type = (llvm::Type *) llvm::Type::getInt32Ty (context());
    else if (max_align == 2)
        base_type = (llvm::Type *) llvm::Type::getInt16Ty (context());
    else
        base_type = (llvm::Type *) llvm::Type::getInt8Ty (context());

    size_t array_len = union_size / target.getTypeStoreSize(base_type);
    return (llvm::Type *) llvm::ArrayType::get (base_type, array_len);
}



llvm::Type *
LLVM_Util::type_struct (const std::vector<llvm::Type *> &types,
                        const std::string &name, bool is_packed)
{
	llvm::StructType * st = llvm::StructType::create(context(), types, name, is_packed);
	ASSERT(st->isStructTy());
	llvm::Type * t= st;
	ASSERT(t->isStructTy());
	return t;
    //return llvm::StructType::create(context(), types, name, is_packed);
}



llvm::Type *
LLVM_Util::type_ptr (llvm::Type *type)
{
    return llvm::PointerType::get (type, 0);
}



llvm::Type *
LLVM_Util::type_array (llvm::Type *type, int n)
{
    return llvm::ArrayType::get (type, n);
}



llvm::FunctionType *
LLVM_Util::type_function (llvm::Type *rettype,
                          const std::vector<llvm::Type*> &params,
                          bool varargs)
{
    return llvm::FunctionType::get (rettype, params, varargs);
}



llvm::PointerType *
LLVM_Util::type_function_ptr (llvm::Type *rettype,
                              const std::vector<llvm::Type*> &params,
                              bool varargs)
{
    llvm::FunctionType *functype = type_function (rettype, params, varargs);
    return llvm::PointerType::getUnqual (functype);
}



std::string
LLVM_Util::llvm_typename (llvm::Type *type) const
{
	ASSERT(type != nullptr);

    std::string s;
    llvm::raw_string_ostream stream (s);
    stream << (*type);
    return stream.str();
}



llvm::Type *
LLVM_Util::llvm_typeof (llvm::Value *val) const
{
    return val->getType();
}



std::string
LLVM_Util::llvm_typenameof (llvm::Value *val) const
{
    return llvm_typename (llvm_typeof (val));
}

llvm::Value *
LLVM_Util::wide_constant (llvm::Value * constant_val)
{
	llvm::Constant *cv = llvm::dyn_cast<llvm::Constant>(constant_val);
	ASSERT(cv  != nullptr);
	return llvm::ConstantVector::getSplat(m_vector_width, cv); 
}


llvm::Value *
LLVM_Util::constant (float f)
{
    return llvm::ConstantFP::get (context(), llvm::APFloat(f));
}

llvm::Value *
LLVM_Util::wide_constant (float f)
{
	return llvm::ConstantVector::getSplat(m_vector_width, llvm::ConstantFP::get (context(), llvm::APFloat(f))); 
}

llvm::Value *
LLVM_Util::constant (int i)
{
    return llvm::ConstantInt::get (context(), llvm::APInt(32,i));
}


llvm::Value *
LLVM_Util::constant8 (int i)
{
    return llvm::ConstantInt::get (context(), llvm::APInt(8,i));
}

llvm::Value *
LLVM_Util::constant16 (uint16_t i)
{
    return llvm::ConstantInt::get (context(), llvm::APInt(16,i));
}

llvm::Value *
LLVM_Util::constant64 (uint64_t i)
{
    return llvm::ConstantInt::get (context(), llvm::APInt(64,i));
}

llvm::Value *
LLVM_Util::constant128 (uint64_t i)
{
    return llvm::ConstantInt::get (context(), llvm::APInt(128,i));
}

llvm::Value *
LLVM_Util::constant128 (uint64_t left, uint64_t right)
{

	uint64_t bigNum[2];
	bigNum[0] = left;
	bigNum[1] = right;

	ArrayRef< uint64_t > refBigNum(&bigNum[0], 2);

    return llvm::ConstantInt::get (context(), llvm::APInt(128,refBigNum));
}


llvm::Value *
LLVM_Util::wide_constant (int i)
{
	return llvm::ConstantVector::getSplat(m_vector_width, llvm::ConstantInt::get (context(), llvm::APInt(32,i))); 
}

llvm::Value *
LLVM_Util::constant (size_t i)
{
    int bits = sizeof(size_t)*8;
    return llvm::ConstantInt::get (context(), llvm::APInt(bits,i));
}

llvm::Value *
LLVM_Util::wide_constant (size_t i)
{
    int bits = sizeof(size_t)*8;
	return llvm::ConstantVector::getSplat(m_vector_width, llvm::ConstantInt::get (context(), llvm::APInt(bits,i))); 
}

llvm::Value *
LLVM_Util::constant_bool (bool i)
{
    return llvm::ConstantInt::get (context(), llvm::APInt(1,i));
}

llvm::Value *
LLVM_Util::wide_constant_bool (bool i)
{
	return llvm::ConstantVector::getSplat(m_vector_width, llvm::ConstantInt::get (context(), llvm::APInt(1,i))); 
}

llvm::Value *
LLVM_Util::constant_ptr (void *p, llvm::PointerType *type)
{
    if (! type)
        type = type_void_ptr();
    return builder().CreateIntToPtr (constant (size_t (p)), type, "const pointer");
}

llvm::Value *
LLVM_Util::constant (ustring s)
{
    // Create a const size_t with the ustring contents
    size_t bits = sizeof(size_t)*8;
    llvm::Value *str = llvm::ConstantInt::get (context(),
                               llvm::APInt(bits,size_t(s.c_str()), true));
    // Then cast the int to a char*.
    return builder().CreateIntToPtr (str, type_string(), "ustring constant");
}


llvm::Value *
LLVM_Util::wide_constant (ustring s)
{
    // Create a const size_t with the ustring contents
    size_t bits = sizeof(size_t)*8;
    llvm::Value *str = llvm::ConstantInt::get (context(),
                               llvm::APInt(bits,size_t(s.c_str()), true));
    // Then cast the int to a char*.
    //return builder().CreateIntToPtr (str, type_string(), "ustring constant");
    
//    Value* emptyVec = UndefValue::get(type_wide_void_ptr());
    
    llvm::Value * constant_value = builder().CreateIntToPtr (str, type_string(), "ustring constant");
//    llvm::InsertElementInstr::Create(emptyVec, constant_value, llvm::ConstantInt::get (context(), llvm::APInt(32,i)));
    
    return builder().CreateVectorSplat(m_vector_width, constant_value);
//    
//    
//    return llvm::ConstantVector::getSplat(m_vector_width, llvm::ConstantInt::get (context(), llvm::APInt(32,i)));
}


llvm::Value *
LLVM_Util::mask_as_int(llvm::Value *mask)
{
    ASSERT(mask->getType() == type_wide_bool());

    llvm::Value* result;
    llvm::Type * int_reinterpret_cast_vector_type;
#if 0
    switch(m_vector_width)
    {
    case 4:
    	ASSERT(0 && "incomplete, should do something similar to case for 8 below if no mask support");
    	int_reinterpret_cast_vector_type = (llvm::Type *) llvm::Type::getInt32Ty (*m_llvm_context);
    	result = builder().CreateBitCast (mask, int_reinterpret_cast_vector_type);
    	break;
    case 8:
    	// Since we know vectorized comparisons for AVX&AVX2 end up setting 8 32 bit integers
    	// to 0xFFFFFFFF or 0x00000000,
    	// We need to do more than a simple cast to an int.
    	// We need to convert our native <VecWidth x i1> to match the expected native representation.
    	// Since the native impl already has it in in that format, it should end up being a noop
    	llvm::Type * extended_int_vector_type =
		    (llvm::Type *) llvm::VectorType::get(llvm::Type::getInt32Ty (*m_llvm_context), m_vector_width);
		llvm::Value * wide_int_mask = builder().CreateSExt(mask, extended_int_vector_type);
		
		// Now we will use the horizontal sign extraction intrinsic to build a 32 bit mask value.
		// However the only 256bit version works on floats, so we will cast from int32 to float beforehand
    	llvm::Type * extended_float_vector_type =
		    (llvm::Type *) llvm::VectorType::get(llvm::Type::getFloatTy (*m_llvm_context), m_vector_width);
    	llvm::Value * wide_float_mask = builder().CreateBitCast (wide_int_mask, extended_float_vector_type);

	    llvm::Function* func = llvm::Intrinsic::getDeclaration (module(),
	        llvm::Intrinsic::x86_avx_movmsk_ps_256); 

	    llvm::Value *args[1] = {
	    		wide_float_mask
	    };
	    result = builder().CreateCall (func, llvm::ArrayRef<llvm::Value*>(args, 1));
	   
   
    	break;
    case 16:    	
    	int_reinterpret_cast_vector_type = (llvm::Type *) llvm::Type::getInt16Ty (*m_llvm_context);
    	result = builder().CreateBitCast (mask, int_reinterpret_cast_vector_type);
    	break;
    }
#else
	int_reinterpret_cast_vector_type = (llvm::Type *) llvm::Type::getInt16Ty (*m_llvm_context);
	result = builder().CreateBitCast (mask, int_reinterpret_cast_vector_type);
#endif
    

    return builder().CreateZExt(result, (llvm::Type *) llvm::Type::getInt32Ty (*m_llvm_context));
}

llvm::Value *
LLVM_Util::int_as_mask(llvm::Value *value)
{
    ASSERT(value->getType() == type_int());

    llvm::Value* result;
#if 0
    switch(m_vector_width)
    {
    case 4:
    {
    	ASSERT(0 && "incomplete, should do something similar to case for 8 below if no mask support");
    	llvm::Type * intMaskType = (llvm::Type *) llvm::Type::getInt32Ty (*m_llvm_context);
        llvm::Value* intMask = builder().CreateTrunc(value, intMaskType);

        result = builder().CreateBitCast (intMask, type_wide_bool());
    	break;
    }
    case 8:
    {
    	// Since we know vectorized comparisons for AVX&AVX2 end up setting 8 32 bit integers
    	// to 0xFFFFFFFF or 0x00000000,
    	// We need to do more than a simple cast to an int.
    	
        // Broadcast out the int32 mask to all data lanes
        llvm::Value * wide_int_mask = widen_value(value);
        
        // Create a filter for each lane to 0 out the other lane's bits
        std::vector<Constant *> lane_masks(m_vector_width);
		for (int lane_index = 0; lane_index < m_vector_width; ++lane_index) {
		   lane_masks[lane_index] = ConstantInt::get(type_int(), (1<<lane_index));
		}                    
		llvm::Value * lane_filter = ConstantVector::get(lane_masks);        
        
		// Bitwise AND the wide_mask and the lane filter
        llvm::Value * filtered_mask = op_and(wide_int_mask, lane_filter);
		//llvm::Value * filtered_mask = wide_int_mask;
        
		// We get better code gen using the floating point comparison ops
        // as the integer based ones at 256 seem to be work on the XMM only and OR
        // the results together
    	//llvm::Type * extended_float_vector_type =
//		    (llvm::Type *) llvm::VectorType::get(llvm::Type::getFloatTy (*m_llvm_context), m_vector_width);
  //  	llvm::Value * filtered_float_mask = builder().CreateBitCast (filtered_mask, extended_float_vector_type);
  
	    result = op_ne(filtered_mask, wide_constant(0));
    //    result = op_ne(filtered_float_mask, wide_constant(0.0f));
        //result = op_gt(filtered_float_mask, wide_constant(0.0f));
    	break;
    }
    case 16:
    {
    	llvm::Type * intMaskType = (llvm::Type *) llvm::Type::getInt16Ty (*m_llvm_context);
        llvm::Value* intMask = builder().CreateTrunc(value, intMaskType);

        result = builder().CreateBitCast (intMask, type_wide_bool());
    	break;
    }
    }
#endif

    if (m_supports_native_bit_masks) {

    	// We can just reinterpret cast a 16 bit integer to a 16 bit mask
    	// and all types are happy
    	llvm::Type * intMaskType = (llvm::Type *) llvm::Type::getInt16Ty (*m_llvm_context);
		llvm::Value* intMask = builder().CreateTrunc(value, intMaskType);

		result = builder().CreateBitCast (intMask, type_wide_bool());
    } else
    {
    	// Since we know vectorized comparisons for AVX&AVX2 end up setting 8 32 bit integers
    	// to 0xFFFFFFFF or 0x00000000,
    	// We need to do more than a simple cast to an int.

        // Broadcast out the int32 mask to all data lanes
        llvm::Value * wide_int_mask = widen_value(value);

        // Create a filter for each lane to 0 out the other lane's bits
        std::vector<Constant *> lane_masks(m_vector_width);
		for (int lane_index = 0; lane_index < m_vector_width; ++lane_index) {
		   lane_masks[lane_index] = ConstantInt::get(type_int(), (1<<lane_index));
		}
		llvm::Value * lane_filter = ConstantVector::get(lane_masks);

		// Bitwise AND the wide_mask and the lane filter
        llvm::Value * filtered_mask = op_and(wide_int_mask, lane_filter);

	    result = op_ne(filtered_mask, wide_constant(0));
    }

    ASSERT(result->getType() == type_wide_bool());

    return result;
}

llvm::Value *
LLVM_Util::test_if_mask_is_non_zero(llvm::Value *mask)
{
	ASSERT(mask->getType() == type_wide_bool());

	llvm::Type * extended_int_vector_type;
	llvm::Type * int_reinterpret_cast_vector_type;
	llvm::Value * zeroConstant;
	switch(m_vector_width) {
	case 4:
		extended_int_vector_type = (llvm::Type *) llvm::VectorType::get(llvm::Type::getInt32Ty (*m_llvm_context), m_vector_width);
		int_reinterpret_cast_vector_type = (llvm::Type *) llvm::Type::getInt128Ty (*m_llvm_context);
		zeroConstant = constant128(0);
		break;
	case 8:
		extended_int_vector_type = (llvm::Type *) llvm::VectorType::get(llvm::Type::getInt32Ty (*m_llvm_context), m_vector_width);
		int_reinterpret_cast_vector_type = (llvm::Type *) llvm::IntegerType::get(*m_llvm_context,256);
		zeroConstant = llvm::ConstantInt::get (context(), llvm::APInt(256,0));
		break;
	case 16:
		// TODO:  Think better way to represent for AVX512
		// also might need something other than number of vector lanes to detect AVX512
		extended_int_vector_type = (llvm::Type *) llvm::VectorType::get(llvm::Type::getInt8Ty (*m_llvm_context), m_vector_width);
		int_reinterpret_cast_vector_type = (llvm::Type *) llvm::Type::getInt128Ty (*m_llvm_context);
		zeroConstant = constant128(0);
		break;
	default:
		ASSERT(0 && "Unhandled vector width");
		break;
	};		

	llvm::Value * wide_int_mask = builder().CreateSExt(mask, extended_int_vector_type);
	llvm::Value * mask_as_int =  builder().CreateBitCast (wide_int_mask, int_reinterpret_cast_vector_type);
    
    return op_ne (mask_as_int, zeroConstant);
}

void
LLVM_Util::test_if_mask_has_any_on_or_off(llvm::Value *mask, llvm::Value* & any_on, llvm::Value* & any_off)
{
	ASSERT(mask->getType() == type_wide_bool());

	llvm::Type * extended_int_vector_type;
	llvm::Type * int_reinterpret_cast_vector_type;
	llvm::Value * allOffConstant;
	llvm::Value * allOnConstant;
	llvm::Value * mask_as_int;
	switch(m_vector_width) {
	case 4:
		{
			extended_int_vector_type = (llvm::Type *) llvm::VectorType::get(llvm::Type::getInt32Ty (*m_llvm_context), m_vector_width);
			int_reinterpret_cast_vector_type = (llvm::Type *) llvm::Type::getInt128Ty (*m_llvm_context);
			allOffConstant = constant128(0);
			ASSERT(0 && "incomplete the allOnConstant is wrong");
			allOnConstant = constant128(0xF);
			llvm::Value * wide_int_mask = builder().CreateSExt(mask, extended_int_vector_type);
			mask_as_int =  builder().CreateBitCast (wide_int_mask, int_reinterpret_cast_vector_type);
		}
		break;

	case 8:
		{
			extended_int_vector_type = (llvm::Type *) llvm::VectorType::get(llvm::Type::getInt32Ty (*m_llvm_context), m_vector_width);
			int_reinterpret_cast_vector_type = (llvm::Type *) llvm::IntegerType::get(*m_llvm_context,256);
			allOffConstant = llvm::ConstantInt::get (context(), llvm::APInt(256,0));
			ASSERT(0 && "incomplete the allOnConstant is wrong");
			allOnConstant = llvm::ConstantInt::get (context(), llvm::APInt(256,0xFF));
			llvm::Value * wide_int_mask = builder().CreateSExt(mask, extended_int_vector_type);
			mask_as_int =  builder().CreateBitCast (wide_int_mask, int_reinterpret_cast_vector_type);

		}
		break;
	case 16:
		{

			allOffConstant = constant16(0);
			allOnConstant = constant16(0xFFFF);

			int_reinterpret_cast_vector_type = (llvm::Type *) llvm::Type::getInt16Ty (*m_llvm_context);
			mask_as_int = builder().CreateBitCast (mask, int_reinterpret_cast_vector_type);
		}
		break;
	default:
		ASSERT(0 && "Unhandled vector width");
		break;
	};

	any_on = op_ne (mask_as_int, allOffConstant);
	any_off = op_ne (mask_as_int, allOnConstant);
}

llvm::Value *
LLVM_Util::test_mask_lane(llvm::Value *mask, int lane_index)
{
	ASSERT(mask->getType() == type_wide_bool());
	
	return builder().CreateExtractElement (mask, lane_index);
}


llvm::Value *
LLVM_Util::op_1st_active_lane_of(llvm::Value * mask)
{
    ASSERT(mask->getType() == type_wide_bool());
    // Assumes mask is not empty

    ASSERT(m_vector_width == 16); // may be incomplete for other widths

    // Count trailing zeros, least significant
    llvm::Type * int16_type = (llvm::Type *) llvm::Type::getInt16Ty(context());
    llvm::Type* types[] = {
            int16_type
    };
    llvm::Function* func_cttz = llvm::Intrinsic::getDeclaration (module(),
        llvm::Intrinsic::cttz,
        llvm::ArrayRef<llvm::Type *>(types, sizeof(types)/sizeof(llvm::Type*)));


    llvm::Value * int16_mask = builder().CreateBitCast (mask, int16_type);
    llvm::Value *args[2] = {
            int16_mask,
            constant_bool(true)
    };

    llvm::Value * firstNonZeroIndex = builder().CreateCall (func_cttz, llvm::ArrayRef<llvm::Value*>(args, 2));
    return firstNonZeroIndex;
}

llvm::Value *
LLVM_Util::op_lanes_that_match_masked(
    llvm::Value * scalar_value,
    llvm::Value * wide_value,
    llvm::Value * mask
    )
{
    ASSERT(scalar_value->getType()->isVectorTy() == false);
    ASSERT(wide_value->getType()->isVectorTy() == true);

    llvm::Value * uniformWideValue = widen_value(scalar_value);
    llvm::Value * lanes_matching = op_eq(uniformWideValue, wide_value);
    llvm::Value * masked_lanes_matching = op_and(lanes_matching, mask);
    return masked_lanes_matching;
}

llvm::Value *
LLVM_Util::widen_value (llvm::Value *val)
{
    return builder().CreateVectorSplat(m_vector_width, val);
}
 
llvm::Value * 
LLVM_Util::negate_mask(llvm::Value *mask)
{
	ASSERT(mask->getType() == type_wide_bool());
	return builder().CreateNot(mask);
}

llvm::Value *
LLVM_Util::constant (const TypeDesc &type)
{
    long long *i = (long long *)&type;
    return llvm::ConstantInt::get (context(), llvm::APInt(64,*i));
}



llvm::Value *
LLVM_Util::void_ptr_null ()
{
    return llvm::ConstantPointerNull::get (type_void_ptr());
}



llvm::Value *
LLVM_Util::ptr_to_cast (llvm::Value* val, llvm::Type *type)
{
    return builder().CreatePointerCast(val,llvm::PointerType::get(type, 0));
}



llvm::Value *
LLVM_Util::ptr_cast (llvm::Value* val, llvm::Type *type)
{
    return builder().CreatePointerCast(val,type);
}



llvm::Value *
LLVM_Util::ptr_cast (llvm::Value* val, const TypeDesc &type)
{
    return ptr_cast (val, llvm::PointerType::get (llvm_type(type), 0));
}


llvm::Value *
LLVM_Util::wide_ptr_cast (llvm::Value* val, const TypeDesc &type)
{
    return ptr_cast (val, llvm::PointerType::get (llvm_vector_type(type), 0));
}
 

llvm::Value *
LLVM_Util::void_ptr (llvm::Value* val)
{
    return builder().CreatePointerCast(val,type_void_ptr());
}




llvm::Type *
LLVM_Util::llvm_type (const TypeDesc &typedesc)
{
    TypeDesc t = typedesc.elementtype();
    llvm::Type *lt = NULL;
    if (t == TypeDesc::FLOAT)
        lt = type_float();
    else if (t == TypeDesc::INT)
        lt = type_int();
    else if (t == TypeDesc::STRING)
        lt = type_string();
    else if (t.aggregate == TypeDesc::VEC3)
        lt = type_triple();
    else if (t.aggregate == TypeDesc::MATRIX44)
        lt = type_matrix();
    else if (t == TypeDesc::NONE)
        lt = type_void();
    else if (t == TypeDesc::UINT8)
        lt = type_char();
    else if (t == TypeDesc::PTR)
        lt = type_void_ptr();
    else {
        std::cerr << "Bad llvm_type(" << typedesc << ")\n";
        ASSERT (0 && "not handling this type yet");
    }
    if (typedesc.arraylen)
        lt = llvm::ArrayType::get (lt, typedesc.arraylen);
    DASSERT (lt);
    return lt;
}

llvm::Type *
LLVM_Util::llvm_vector_type (const TypeDesc &typedesc)
{
    TypeDesc t = typedesc.elementtype();
    llvm::Type *lt = NULL;
    if (t == TypeDesc::FLOAT)
        lt = type_wide_float();
    else if (t == TypeDesc::INT)
        lt = type_wide_int();
    else if (t == TypeDesc::STRING)
        lt = type_wide_string();
    else if (t.aggregate == TypeDesc::VEC3)
        lt = type_wide_triple();
    else if (t.aggregate == TypeDesc::MATRIX44)
        lt = type_wide_matrix();
    // TODO:  No such thing as a wide void?
    // so let this fall through to error below
    // see if we ever run into it
//    else if (t == TypeDesc::NONE)
//        lt = type_wide_void();
    else if (t == TypeDesc::UINT8)
        lt = type_wide_char();
    else if (t == TypeDesc::PTR)
        lt = type_wide_void_ptr();
    else {
        std::cerr << "Bad llvm_vector_type(" << typedesc << ")\n";
        ASSERT (0 && "not handling this type yet");
    }
    if (typedesc.arraylen)
    {
    	
    	OSL_DEV_ONLY(std::cout << "llvm_vector_type typedesc.arraylen = " << typedesc.arraylen << std::endl);
        lt = llvm::ArrayType::get (lt, typedesc.arraylen);
    }
    DASSERT (lt);
    return lt;
}


llvm::Value *
LLVM_Util::offset_ptr (llvm::Value *ptr, int offset, llvm::Type *ptrtype)
{
    llvm::Value *i = builder().CreatePtrToInt (ptr, type_addrint());
    i = builder().CreateAdd (i, constant ((size_t)offset));
    ptr = builder().CreateIntToPtr (i, type_void_ptr());
    if (ptrtype)
        ptr = ptr_cast (ptr, ptrtype);
    return ptr;
}

void
LLVM_Util::assume_ptr_is_aligned(llvm::Value *ptr, unsigned alignment)
{
    const llvm::DataLayout & data_layout = m_llvm_exec->getDataLayout();

    builder().CreateAlignmentAssumption(data_layout, ptr, alignment);
}



llvm::Value *
LLVM_Util::op_alloca (llvm::Type *llvmtype, int n, const std::string &name)
{
    llvm::ConstantInt* numalloc = (llvm::ConstantInt*)constant(n);
    return builder().CreateAlloca (llvmtype, numalloc, name);
}


llvm::Value *
LLVM_Util::op_alloca_aligned (unsigned alignment, llvm::Type *llvmtype, int n, const std::string &name)
{
    llvm::ConstantInt* numalloc = (llvm::ConstantInt*)constant(n);
    llvm::AllocaInst * inst = builder().CreateAlloca (llvmtype, numalloc, name);
    inst->setAlignment(alignment);
    return inst;
}


llvm::Value *
LLVM_Util::op_alloca (const TypeDesc &type, int n, const std::string &name)
{
    return op_alloca (llvm_type(type.elementtype()), n*type.numelements(), name);
}


llvm::Value *
LLVM_Util::wide_op_alloca (const TypeDesc &type, int n, const std::string &name)
{
    return op_alloca (llvm_vector_type(type.elementtype()), n*type.numelements(), name);
}


llvm::Value *
LLVM_Util::call_function (llvm::Value *func, llvm::Value **args, int nargs)
{
    ASSERT (func);
#if 0
    llvm::outs() << "llvm_call_function " << *func << "\n";
    llvm::outs() << nargs << " args:\n";
    for (int i = 0;  i < nargs;  ++i)
        llvm::outs() << "\t" << *(args[i]) << "\n";
#endif
    //llvm_gen_debug_printf (std::string("start ") + std::string(name));
    llvm::Value *r = builder().CreateCall (func, llvm::ArrayRef<llvm::Value *>(args, nargs));
    //llvm_gen_debug_printf (std::string(" end  ") + std::string(name));
    return r;
}



void
LLVM_Util::mark_structure_return_value(llvm::Value *funccall)
{
    llvm::CallInst* call = llvm::cast<llvm::CallInst>(funccall);
    
#if OSL_LLVM_VERSION < 50
    auto attrs = llvm::AttributeSet::get(
    		call->getContext(),
        llvm::AttributeSet::FunctionIndex,
        llvm::Attribute::NoUnwind);

    //attrs = attrs.addAttribute(call->getContext(), 1, llvm::Attribute::NoAlias);

    attrs = attrs.addAttribute(call->getContext(), 1,
                               llvm::Attribute::StructRet);
#else
    // TODO: verify this is correct for LLVM 5.0
    AttributeList attrs = llvm::AttributeList::get(call->getContext(), llvm::AttributeList::FunctionIndex, llvm::Attribute::NoUnwind);
    attrs.addAttribute(call->getContext(), llvm::AttributeList::ReturnIndex, llvm::Attribute::StructRet);
#endif


    call->setAttributes(attrs);
}



llvm::Value *
LLVM_Util::call_function (const char *name, llvm::Value **args, int nargs)
{
    llvm::Function *func = module()->getFunction (name);
    if (! func)
        std::cerr << "Couldn't find function " << name << "\n";
    return call_function (func, args, nargs);
}



void
LLVM_Util::mark_fast_func_call (llvm::Value *funccall)
{
    llvm::CallInst* call_inst = llvm::cast<llvm::CallInst>(funccall);
    call_inst->setCallingConv (llvm::CallingConv::Fast);
}



void
LLVM_Util::op_branch (llvm::BasicBlock *block)
{
    builder().CreateBr (block);
    set_insert_point (block);
}



void
LLVM_Util::op_branch (llvm::Value *cond, llvm::BasicBlock *trueblock,
                      llvm::BasicBlock *falseblock)
{
    builder().CreateCondBr (cond, trueblock, falseblock);
    set_insert_point (trueblock);
}



void
LLVM_Util::set_insert_point (llvm::BasicBlock *block)
{
    builder().SetInsertPoint (block);
}



void
LLVM_Util::op_return (llvm::Value *retval)
{
    if (retval)
        builder().CreateRet (retval);
    else
        builder().CreateRetVoid ();
}



void
LLVM_Util::op_memset (llvm::Value *ptr, int val, int len, int align)
{
    op_memset(ptr, val, constant(len), align);
}



void
LLVM_Util::op_memset (llvm::Value *ptr, int val, llvm::Value *len, int align)
{
    // memset with i32 len
    // and with an i8 pointer (dst) for LLVM-2.8
    llvm::Type* types[] = {
        (llvm::Type *) llvm::PointerType::get(llvm::Type::getInt8Ty(context()), 0),
        (llvm::Type *) llvm::Type::getInt32Ty(context())
    };

    llvm::Function* func = llvm::Intrinsic::getDeclaration (module(),
        llvm::Intrinsic::memset,
        llvm::ArrayRef<llvm::Type *>(types, sizeof(types)/sizeof(llvm::Type*)));

    // NOTE(boulos): constant(0) would return an i32
    // version of 0, but we need the i8 version. If we make an
    // ::constant(char val) though then we'll get ambiguity
    // everywhere.
    llvm::Value* fill_val = llvm::ConstantInt::get (context(),
                                                    llvm::APInt(8, val));
    // Non-volatile (allow optimizer to move it around as it wishes
    // and even remove it if it can prove it's useless)
#if OSL_LLVM_VERSION <= 36
    builder().CreateCall5 (func, ptr, fill_val, len, constant(align),
                           constant_bool(false));
#else
    llvm::Value *args[5] = {
        ptr, fill_val, len, constant(align), constant_bool(false)
    };
    builder().CreateCall (func, llvm::ArrayRef<llvm::Value*>(args, 5));

#endif
}



void
LLVM_Util::op_memcpy (llvm::Value *dst, llvm::Value *src, int len, int align)
{
    // i32 len
    // and with i8 pointers (dst and src) for LLVM-2.8
    llvm::Type* types[] = {
        (llvm::Type *) llvm::PointerType::get(llvm::Type::getInt8Ty(context()), 0),
        (llvm::Type *) llvm::PointerType::get(llvm::Type::getInt8Ty(context()), 0),
        (llvm::Type *) llvm::Type::getInt32Ty(context())
    };

    llvm::Function* func = llvm::Intrinsic::getDeclaration (module(),
        llvm::Intrinsic::memcpy,
        llvm::ArrayRef<llvm::Type *>(types, sizeof(types)/sizeof(llvm::Type*)));

    // Non-volatile (allow optimizer to move it around as it wishes
    // and even remove it if it can prove it's useless)
#if OSL_LLVM_VERSION <= 36
    builder().CreateCall5 (func, dst, src,
                           constant(len), constant(align), constant_bool(false));
#else
    llvm::Value *args[5] = {
        dst, src, constant(len), constant(align), constant_bool(false)
    };
    builder().CreateCall (func, llvm::ArrayRef<llvm::Value*>(args, 5));
#endif
}



llvm::Value *
LLVM_Util::op_load (llvm::Value *ptr)
{
    return builder().CreateLoad (ptr);
}


void
LLVM_Util::push_mask(llvm::Value *mask, bool negate, bool absolute)
{	
	ASSERT(mask->getType() == type_wide_bool());
	if(m_mask_stack.empty()) {
		m_mask_stack.push_back(MaskInfo{mask, negate, 0});
	} else {
		
		MaskInfo & mi = m_mask_stack.back();
		llvm::Value *prev_mask = mi.mask;
		bool prev_negate = mi.negate;

		int applied_return_mask_count = absolute? 0 : mi.applied_return_mask_count;

		if (false == prev_negate) {
			if (false == negate)
			{
				llvm::Value *blended_mask;
				if (absolute) {
					blended_mask = mask;
				} else {
					blended_mask = builder().CreateSelect(prev_mask, mask, prev_mask);
				}
				m_mask_stack.push_back(MaskInfo{blended_mask, false, applied_return_mask_count});
			} else {				
				ASSERT(false == absolute);
				llvm::Value *blended_mask = builder().CreateSelect(mask, wide_constant_bool(false), prev_mask);
				m_mask_stack.push_back(MaskInfo{blended_mask, false, applied_return_mask_count});
			}
		} else {
			if (false == negate)
			{
				llvm::Value *blended_mask;
				if (absolute) {
					blended_mask = mask;
				} else {
					blended_mask = builder().CreateSelect(prev_mask, wide_constant_bool(false), mask);
				}
				m_mask_stack.push_back(MaskInfo{blended_mask, false, applied_return_mask_count});
			} else {
				ASSERT(false == absolute);
				llvm::Value *blended_mask = builder().CreateSelect(prev_mask, prev_mask, mask);
				m_mask_stack.push_back(MaskInfo{blended_mask, true, applied_return_mask_count});
			}			
		}
	}
}


llvm::Value *
LLVM_Util::shader_mask()
{
	llvm::Value * loc_of_shader_mask = m_alloca_for_modified_mask_stack.front();
	return op_load(loc_of_shader_mask);
}

void
LLVM_Util::apply_exit_to_mask_stack()
{
	ASSERT (false == m_mask_stack.empty());
	ASSERT(false == m_alloca_for_modified_mask_stack.empty());


	llvm::Value * loc_of_shader_mask = m_alloca_for_modified_mask_stack.front();
	llvm::Value * shader_mask = op_load(loc_of_shader_mask);

	llvm::Value * loc_of_function_mask = m_alloca_for_modified_mask_stack.back();
	llvm::Value * function_mask = op_load(loc_of_function_mask);

	// For any inactive lanes of the exit mask
	// set the function_mask to 0.
	llvm::Value * modified_function_mask = builder().CreateSelect(shader_mask, function_mask, shader_mask);

	push_masking_enabled(false);
	op_store(modified_function_mask, loc_of_function_mask);
	pop_masking_enabled();

	// Apply the modified_function_mask to the current conditional mask stack
	// By bumping the return count, the modified_return_mask will get applied
	// to the conditional mask stack as it unwinds.
	++m_masked_return_count_stack.back();

	// We could just call apply_return_to_mask_stack(), but will repeat the work
	// here to take advantage of the already loaded return mask
	auto & mi = m_mask_stack.back();

	int masked_return_count = m_masked_return_count_stack.back();
	ASSERT(masked_return_count > mi.applied_return_mask_count);
	llvm::Value * existing_mask = mi.mask;

	ASSERT(false == m_alloca_for_modified_mask_stack.empty());
	if(mi.negate) {
		mi.mask = builder().CreateSelect(modified_function_mask, existing_mask, wide_constant_bool(true));
	} else {
		mi.mask = builder().CreateSelect(modified_function_mask, existing_mask, modified_function_mask);
	}
	mi.applied_return_mask_count = masked_return_count;
}

void
LLVM_Util::apply_return_to_mask_stack()
{
	ASSERT (false == m_mask_stack.empty());

	auto & mi = m_mask_stack.back();
	int masked_return_count = m_masked_return_count_stack.back();
	// TODO: might be impossible for this conditional to be false, could change to assert
	// or remove applied_return_mask_count entirely
	if (masked_return_count > mi.applied_return_mask_count) {
		llvm::Value * existing_mask = mi.mask;

		ASSERT(false == m_alloca_for_modified_mask_stack.empty());
		llvm::Value * loc_of_return_mask = m_alloca_for_modified_mask_stack.back();
		llvm::Value * rs_mask = op_load(loc_of_return_mask);
		if(mi.negate) {
			mi.mask = builder().CreateSelect(rs_mask, existing_mask, wide_constant_bool(true));
		} else {
			mi.mask = builder().CreateSelect(rs_mask, existing_mask, rs_mask);
		}
		mi.applied_return_mask_count = masked_return_count;
	}
	
}

void
LLVM_Util::apply_break_to_mask_stack()
{
	ASSERT (false == m_mask_stack.empty());

	auto & mi = m_mask_stack.back();

	llvm::Value * existing_mask = mi.mask;

	// TODO: do we need to track if a break was applied or not?
	ASSERT(false == m_masked_loop_stack.empty());
	llvm::Value * loc_of_cond_mask = m_masked_loop_stack.back().location_of_condition_mask;
	llvm::Value * cond_mask = op_load(loc_of_cond_mask);
	if(mi.negate) {
		mi.mask = builder().CreateSelect(cond_mask, existing_mask, wide_constant_bool(true));
	} else {
		mi.mask = builder().CreateSelect(cond_mask, existing_mask, cond_mask);
	}
}

void
LLVM_Util::apply_continue_to_mask_stack()
{
	ASSERT (false == m_mask_stack.empty());

	auto & mi = m_mask_stack.back();

	llvm::Value * existing_mask = mi.mask;

	// TODO: do we need to track if a break was applied or not?
	ASSERT(false == m_masked_loop_stack.empty());
	llvm::Value * loc_of_continue_mask = m_masked_loop_stack.back().location_of_continue_mask;
	llvm::Value * continue_mask = op_load(loc_of_continue_mask);
	if(mi.negate) {
		mi.mask = builder().CreateSelect(continue_mask, wide_constant_bool(true), existing_mask);
	} else {
		mi.mask = builder().CreateSelect(continue_mask, wide_constant_bool(false), existing_mask);
	}
}

llvm::Value *
LLVM_Util::apply_return_to(llvm::Value *existing_mask)
{
	// caller should have checked masked_return_count() beforehand
	ASSERT (m_masked_return_count_stack.back() > 0);
	ASSERT(false == m_alloca_for_modified_mask_stack.empty());

	llvm::Value * loc_of_return_mask = m_alloca_for_modified_mask_stack.back();
	llvm::Value * rs_mask = op_load(loc_of_return_mask);
	llvm::Value *result = builder().CreateSelect(rs_mask, existing_mask, rs_mask);
	return result;
}


void
LLVM_Util::pop_mask()
{
	ASSERT(false == m_mask_stack.empty());

	m_mask_stack.pop_back();
}

llvm::Value *
LLVM_Util::current_mask()
{
	ASSERT(!m_mask_stack.empty());
	auto & mi = m_mask_stack.back();
	if (mi.negate) {
		llvm::Value *negated_mask = builder().CreateSelect(mi.mask, wide_constant_bool(false), wide_constant_bool(true));
		return negated_mask;
	} else {
		return mi.mask;
	}
}

void
LLVM_Util::op_masked_break()
{
	OSL_DEV_ONLY(std::cout << "op_masked_break" << std::endl);

	ASSERT(false == m_mask_stack.empty());

	const MaskInfo & mi = m_mask_stack.back();
	// Because we are inside a conditional branch
	// we can't let our local modified mask be directly used
	// by other scopes, instead we must store the result
	// of to the stack for the outer scope to pickup and
	// use
	ASSERT(!m_masked_loop_stack.empty());
	LoopInfo & loop = m_masked_loop_stack.back();
	llvm::Value * loc_of_cond_mask = loop.location_of_condition_mask;

	llvm::Value * cond_mask = op_load(loc_of_cond_mask);

	llvm::Value * break_from_mask = mi.mask;
	llvm::Value * new_cond_mask;

	// For any active lanes of the mask we are returning from
	// set the after_if_mask to 0.
	if (mi.negate) {
		new_cond_mask = builder().CreateSelect(break_from_mask, cond_mask, break_from_mask);
	} else {
		new_cond_mask = builder().CreateSelect(break_from_mask, wide_constant_bool(false), cond_mask);
	}

	push_masking_enabled(false);
	op_store(new_cond_mask, loc_of_cond_mask);
	pop_masking_enabled();

	// Track that a break was called in the current masked loop
	loop.break_count++;
}

void
LLVM_Util::op_masked_continue()
{
	OSL_DEV_ONLY(std::cout << "op_masked_break" << std::endl);

	ASSERT(false == m_mask_stack.empty());

	const MaskInfo & mi = m_mask_stack.back();
	// Because we are inside a conditional branch
	// we can't let our local modified mask be directly used
	// by other scopes, instead we must store the result
	// of to the stack for the outer scope to pickup and
	// use
	ASSERT(!m_masked_loop_stack.empty());
	LoopInfo & loop = m_masked_loop_stack.back();
	llvm::Value * loc_of_continue_mask = loop.location_of_continue_mask;

	llvm::Value * continue_mask = op_load(loc_of_continue_mask);

	llvm::Value * continue_from_mask = mi.mask;
	llvm::Value * new_abs_continue_mask;

	// For any active lanes of the mask we are returning from
	// set the after_if_mask to 0.
	if (mi.negate) {
		new_abs_continue_mask = builder().CreateSelect(continue_from_mask, continue_mask, this->wide_constant_bool(true));
	} else {
		new_abs_continue_mask = builder().CreateSelect(continue_from_mask, continue_from_mask, continue_mask);
	}

	push_masking_enabled(false);
	op_store(new_abs_continue_mask, loc_of_continue_mask);
	pop_masking_enabled();

	// Track that a break was called in the current masked loop
	loop.continue_count++;
}

void
LLVM_Util::op_masked_exit()
{
	OSL_DEV_ONLY(std::cout << "push_mask_exit" << std::endl);

	ASSERT(false == m_mask_stack.empty());

	const MaskInfo & mi = m_mask_stack.back();
	llvm::Value * exit_from_mask = mi.mask;

	// Because we are inside a conditional branch
	// we can't let our local modified mask be directly used
	// by other scopes, instead we must store the result
	// of to the stack for the outer scope to pickup and
	// use
	ASSERT(false == m_alloca_for_modified_mask_stack.empty());
	{
		llvm::Value * loc_of_shader_mask = m_alloca_for_modified_mask_stack.front();
		llvm::Value * shader_mask = op_load(loc_of_shader_mask);

		llvm::Value * modifiedMask;
		// For any active lanes of the mask we are returning from
		// set the shader scope mask to 0.
		if (mi.negate) {
			modifiedMask = builder().CreateSelect(exit_from_mask, shader_mask, exit_from_mask);
		} else {
			modifiedMask = builder().CreateSelect(exit_from_mask, wide_constant_bool(false), shader_mask);
		}

		push_masking_enabled(false);
		op_store(modifiedMask, loc_of_shader_mask);
		pop_masking_enabled();
	}

	// Are we inside a function scope, then we will need to modify its active lane mask
	// functions higher up in the stack will apply the current exit mask when functions are popped
	if (m_alloca_for_modified_mask_stack.size() > 1) {
		llvm::Value * loc_of_function_mask = m_alloca_for_modified_mask_stack.back();
		llvm::Value * function_mask = op_load(loc_of_function_mask);


		llvm::Value * modifiedMask;

		// For any active lanes of the mask we are returning from
		// set the after_if_mask to 0.
		if (mi.negate) {
			modifiedMask = builder().CreateSelect(exit_from_mask, function_mask, exit_from_mask);
		} else {
			modifiedMask = builder().CreateSelect(exit_from_mask, wide_constant_bool(false), function_mask);
		}

		push_masking_enabled(false);
		op_store(modifiedMask, loc_of_function_mask);
		pop_masking_enabled();
	}

	// Bumping the masked exit count will cause the exit mask to be applied to the return mask
	// of the calling function when the current function is popped
	++m_masked_exit_count;

	// Bumping the masked return count will cause the return mask(which is a subset of the shader_mask)
	// to be applied to the mask stack when leaving if/else block
	ASSERT(!m_masked_return_count_stack.empty());
	m_masked_return_count_stack.back()++;
}

void
LLVM_Util::op_masked_return()
{
	OSL_DEV_ONLY(std::cout << "push_mask_return" << std::endl);

	ASSERT(false == m_mask_stack.empty());

	const MaskInfo & mi = m_mask_stack.back();
	// Because we are inside a conditional branch
	// we can't let our local modified mask be directly used
	// by other scopes, instead we must store the result
	// of to the stack for the outer scope to pickup and
	// use
	ASSERT(false == m_alloca_for_modified_mask_stack.empty());
	llvm::Value * loc_of_function_mask = m_alloca_for_modified_mask_stack.back();
	llvm::Value * function_mask = op_load(loc_of_function_mask);


	llvm::Value * return_from_mask = mi.mask;
	llvm::Value * modifiedMask;

	// For any active lanes of the mask we are returning from
	// set the function scope mask to 0.
	if (mi.negate) {
		modifiedMask = builder().CreateSelect(return_from_mask, function_mask, return_from_mask);
	} else {
		modifiedMask = builder().CreateSelect(return_from_mask, wide_constant_bool(false), function_mask);
	}

	push_masking_enabled(false);
	op_store(modifiedMask, loc_of_function_mask);
	pop_masking_enabled();

	ASSERT(!m_masked_return_count_stack.empty());
	m_masked_return_count_stack.back()++;

}


void
LLVM_Util::push_masking_enabled(bool enabled)
{
	m_enable_masking_stack.push_back(enabled);	
}

void
LLVM_Util::pop_masking_enabled()
{
	ASSERT(false == m_enable_masking_stack.empty());
	m_enable_masking_stack.pop_back();	
}



void
LLVM_Util::op_store (llvm::Value *val, llvm::Value *ptr)
{	
	if(m_mask_stack.empty() || val->getType()->isVectorTy() == false || m_enable_masking_stack.empty() || m_enable_masking_stack.back() == false) {
		
		//OSL_DEV_ONLY(std::cout << "unmasked op_store" << std::endl);
		// We may not be in a non-uniform code block
		// or the value being stored may be uniform, which case it shouldn't
		// be a vector type
	    builder().CreateStore (val, ptr);		
	} else {				
		//OSL_DEV_ONLY(std::cout << "MASKED op_store" << std::endl);
		// TODO: could probably make these DASSERT as  the conditional above "should" be checking all of this
		ASSERT(m_enable_masking_stack.back());
		ASSERT(val->getType()->isVectorTy());
		ASSERT(false == m_mask_stack.empty());
		
		MaskInfo & mi = m_mask_stack.back();
		// TODO: add assert for ptr alignment in debug builds	
#if 0
		if (m_supports_masked_stores) {
			builder().CreateMaskedStore(val, ptr, 64, mi.mask);
		} else 
#endif
		{
			// Transform the masted store to a load+blend+store
			// Technically, the behavior is different than a masked store
			// as different thread could technically have modified the masked off
			// data lane values inbetween the read+store
			// As this language sits below the threading level that could
			// never happen and a read+store
			llvm::Value *previous_value = builder().CreateLoad (ptr);
			if (false == mi.negate) {
				llvm::Value *blended_value = builder().CreateSelect(mi.mask, val, previous_value);
				builder().CreateStore(blended_value, ptr);
			} else {
				llvm::Value *blended_value = builder().CreateSelect(mi.mask, previous_value, val);
				builder().CreateStore(blended_value, ptr);				
			}
		}
	}
	
}



llvm::Value *
LLVM_Util::GEP (llvm::Value *ptr, llvm::Value *elem)
{
    return builder().CreateGEP (ptr, elem);
}



llvm::Value *
LLVM_Util::GEP (llvm::Value *ptr, int elem)
{
    return builder().CreateConstGEP1_32 (ptr, elem);
}



llvm::Value *
LLVM_Util::GEP (llvm::Value *ptr, int elem1, int elem2)
{
#if OSL_LLVM_VERSION <= 36
    return builder().CreateConstGEP2_32 (ptr, elem1, elem2);
#else
    return builder().CreateConstGEP2_32 (nullptr, ptr, elem1, elem2);
#endif
}



llvm::Value *
LLVM_Util::op_add (llvm::Value *a, llvm::Value *b)
{
    if ((a->getType() == type_float() && b->getType() == type_float()) ||
		(a->getType() == type_wide_float() && b->getType() == type_wide_float()))
        return builder().CreateFAdd (a, b);
    if ((a->getType() == type_int() && b->getType() == type_int()) ||
		(a->getType() == type_wide_int() && b->getType() == type_wide_int()))
        return builder().CreateAdd (a, b);
        
    ASSERT (0 && "Op has bad value type combination");
}



llvm::Value *
LLVM_Util::op_sub (llvm::Value *a, llvm::Value *b)
{
    if ((a->getType() == type_float() && b->getType() == type_float()) ||
		(a->getType() == type_wide_float() && b->getType() == type_wide_float()))
        return builder().CreateFSub (a, b);
    if ((a->getType() == type_int() && b->getType() == type_int()) ||
		(a->getType() == type_wide_int() && b->getType() == type_wide_int()))
        return builder().CreateSub (a, b);
        
    ASSERT (0 && "Op has bad value type combination");
}


llvm::Value *
LLVM_Util::op_neg (llvm::Value *a)
{
    if ((a->getType() == type_float()) ||
		(a->getType() == type_wide_float()))
        return builder().CreateFNeg (a);
    if ((a->getType() == type_int()) ||
		(a->getType() == type_wide_int()))
        return builder().CreateNeg (a);
    
    ASSERT (0 && "Op has bad value type combination");
}


llvm::Value *
LLVM_Util::op_mul (llvm::Value *a, llvm::Value *b)
{
    if ((a->getType() == type_float() && b->getType() == type_float()) ||
		(a->getType() == type_wide_float() && b->getType() == type_wide_float()))
        return builder().CreateFMul (a, b);
    if ((a->getType() == type_int() && b->getType() == type_int()) ||
		(a->getType() == type_wide_int() && b->getType() == type_wide_int()))
        return builder().CreateMul (a, b);
           
    ASSERT (0 && "Op has bad value type combination");
}

llvm::Value *
LLVM_Util::op_div (llvm::Value *a, llvm::Value *b)
{
    if ((a->getType() == type_float() && b->getType() == type_float()) ||
		(a->getType() == type_wide_float() && b->getType() == type_wide_float()))
        return builder().CreateFDiv (a, b);
    if ((a->getType() == type_int() && b->getType() == type_int()) ||
		(a->getType() == type_wide_int() && b->getType() == type_wide_int()))
        return builder().CreateSDiv (a, b);
        
    ASSERT (0 && "Op has bad value type combination");
}


llvm::Value *
LLVM_Util::op_mod (llvm::Value *a, llvm::Value *b)
{
    if ((a->getType() == type_float() && b->getType() == type_float()) ||
		(a->getType() == type_wide_float() && b->getType() == type_wide_float()))
        return builder().CreateFRem (a, b);
    if ((a->getType() == type_int() && b->getType() == type_int()) ||
		(a->getType() == type_wide_int() && b->getType() == type_wide_int()))
        return builder().CreateSRem (a, b);

    ASSERT (0 && "Op has bad value type combination");
}

llvm::Value *
LLVM_Util::op_float_to_int (llvm::Value* a)
{
    if (a->getType() == type_float())
        return builder().CreateFPToSI(a, type_int());
    if (a->getType() == type_wide_float())
        return builder().CreateFPToSI(a, type_wide_int());
    if ((a->getType() == type_int()) || (a->getType() == type_wide_int()))
        return a;
    ASSERT (0 && "Op has bad value type combination");
}


llvm::Value *
LLVM_Util::op_float_to_double (llvm::Value* a)
{
    if(a->getType() == type_float())
    	return builder().CreateFPExt(a, type_double());
    if(a->getType() == type_wide_float())
    	return builder().CreateFPExt(a, type_wide_double());
    // TODO: unclear why this is inconsistent vs. the other conversion ops
    // which become no-ops if the type is already the target
    
    ASSERT (0 && "Op has bad value type combination");
}


llvm::Value *
LLVM_Util::op_int_to_float (llvm::Value* a)
{
    if (a->getType() == type_int())
        return builder().CreateSIToFP(a, type_float());
    if (a->getType() == type_wide_int())
        return builder().CreateSIToFP(a, type_wide_float());
    if ((a->getType() == type_float()) || (a->getType() == type_wide_float()))
        return a;
    ASSERT (0 && "Op has bad value type combination");
}


llvm::Value *
LLVM_Util::op_bool_to_int (llvm::Value* a)
{
    if (a->getType() == type_bool())
        return builder().CreateZExt (a, type_int());
    if (a->getType() == type_wide_bool()) 
    	return builder().CreateZExt (a, type_wide_int());
    if ((a->getType() == type_int()) || (a->getType() == type_wide_int()))
        return a;
    ASSERT (0 && "Op has bad value type combination");
}

llvm::Value *
LLVM_Util::op_bool_to_float (llvm::Value* a)
{
    if (a->getType() == type_bool())
        return builder().CreateSIToFP(a, type_float());
    if (a->getType() == type_wide_bool())
        return builder().CreateSIToFP(a, type_wide_float());
    if ((a->getType() == type_float()) || (a->getType() == type_wide_float()))
        return a;
    ASSERT (0 && "Op has bad value type combination");
}


llvm::Value *
LLVM_Util::op_int_to_bool(llvm::Value* a)
{
    if (a->getType() == type_int()) 
        return op_ne (a, constant(static_cast<int>(0)));
    if (a->getType() == type_wide_int()) 
        return op_ne (a, wide_constant(static_cast<int>(0)));
    if ((a->getType() == type_bool()) || (a->getType() == type_wide_bool()))
        return a;
    ASSERT (0 && "Op has bad value type combination");
	return NULL;
}


llvm::Value *
LLVM_Util::op_and (llvm::Value *a, llvm::Value *b)
{
	// TODO: unlclear why inconsistent and not checking for operand types 
	// with final ASSERT for "bad value type combination"
    return builder().CreateAnd (a, b);
}


llvm::Value *
LLVM_Util::op_or (llvm::Value *a, llvm::Value *b)
{
	// TODO: unlclear why inconsistent and not checking for operand types 
	// with final ASSERT for "bad value type combination"
    return builder().CreateOr (a, b);
}


llvm::Value *
LLVM_Util::op_xor (llvm::Value *a, llvm::Value *b)
{
	// TODO: unlclear why inconsistent and not checking for operand types 
	// with final ASSERT for "bad value type combination"
    return builder().CreateXor (a, b);
}


llvm::Value *
LLVM_Util::op_shl (llvm::Value *a, llvm::Value *b)
{
	// TODO: unlclear why inconsistent and not checking for operand types 
	// with final ASSERT for "bad value type combination"
    return builder().CreateShl (a, b);
}


llvm::Value *
LLVM_Util::op_shr (llvm::Value *a, llvm::Value *b)
{
    if ((a->getType() == type_int() && b->getType() == type_int()) ||
		(a->getType() == type_wide_int() && b->getType() == type_wide_int()))
        return builder().CreateAShr (a, b);  // signed int -> arithmetic shift
    
    ASSERT (0 && "Op has bad value type combination");
}


llvm::Value *
LLVM_Util::op_not (llvm::Value *a)
{
	// TODO: unlclear why inconsistent and not checking for operand types 
	// with final ASSERT for "bad value type combination"
    return builder().CreateNot (a);
}



llvm::Value *
LLVM_Util::op_select (llvm::Value *cond, llvm::Value *a, llvm::Value *b)
{
	// TODO: unlclear why inconsistent and not checking for operand types 
	// with final ASSERT for "bad value type combination"
    return builder().CreateSelect (cond, a, b);
}

llvm::Value *
LLVM_Util::op_extract (llvm::Value *a, int index)
{
    return builder().CreateExtractElement (a, index);
}

llvm::Value *
LLVM_Util::op_extract (llvm::Value *a, llvm::Value *index)
{
    return builder().CreateExtractElement (a, index);
}


llvm::Value *
LLVM_Util::op_eq (llvm::Value *a, llvm::Value *b, bool ordered)
{
    if (a->getType() != b->getType()) {
    	std::cout << "a type=" << llvm_typenameof(a) << " b type=" << llvm_typenameof(b) << std::endl;
    }
    ASSERT (a->getType() == b->getType());
    if ((a->getType() == type_float()) || (a->getType() == type_wide_float()))
        return ordered ? builder().CreateFCmpOEQ (a, b) : builder().CreateFCmpUEQ (a, b);
    else
        return builder().CreateICmpEQ (a, b);
}



llvm::Value *
LLVM_Util::op_ne (llvm::Value *a, llvm::Value *b, bool ordered)
{
    if (a->getType() != b->getType()) {
    	std::cout << "a type=" << llvm_typenameof(a) << " b type=" << llvm_typenameof(b) << std::endl;
    }
    ASSERT (a->getType() == b->getType());
    if ((a->getType() == type_float()) || (a->getType() == type_wide_float()))
        return ordered ? builder().CreateFCmpONE (a, b) : builder().CreateFCmpUNE (a, b);
    else
        return builder().CreateICmpNE (a, b);
}



llvm::Value *
LLVM_Util::op_gt (llvm::Value *a, llvm::Value *b, bool ordered)
{
    ASSERT (a->getType() == b->getType());
    if ((a->getType() == type_float()) || (a->getType() == type_wide_float()))
        return ordered ? builder().CreateFCmpOGT (a, b) : builder().CreateFCmpUGT (a, b);
    else
        return builder().CreateICmpSGT (a, b);
}



llvm::Value *
LLVM_Util::op_lt (llvm::Value *a, llvm::Value *b, bool ordered)
{
    ASSERT (a->getType() == b->getType());
    if ((a->getType() == type_float()) || (a->getType() == type_wide_float()))
        return ordered ? builder().CreateFCmpOLT (a, b) : builder().CreateFCmpULT (a, b);
    else
        return builder().CreateICmpSLT (a, b);
}



llvm::Value *
LLVM_Util::op_ge (llvm::Value *a, llvm::Value *b, bool ordered)
{
    ASSERT (a->getType() == b->getType());
    if ((a->getType() == type_float()) || (a->getType() == type_wide_float()))
        return ordered ? builder().CreateFCmpOGE (a, b) : builder().CreateFCmpUGE (a, b);
    else
        return builder().CreateICmpSGE (a, b);
}



llvm::Value *
LLVM_Util::op_le (llvm::Value *a, llvm::Value *b, bool ordered)
{
    ASSERT (a->getType() == b->getType());
    if ((a->getType() == type_float()) || (a->getType() == type_wide_float()))
        return ordered ? builder().CreateFCmpOLE (a, b) : builder().CreateFCmpULE (a, b);
    else
        return builder().CreateICmpSLE (a, b);
}



void
LLVM_Util::write_bitcode_file (const char *filename, std::string *err)
{
#if OSL_LLVM_VERSION >= 36
    std::error_code local_error;
    llvm::raw_fd_ostream out (filename, local_error, llvm::sys::fs::F_None);
#elif OSL_LLVM_VERSION >= 35
    std::string local_error;
    llvm::raw_fd_ostream out (filename, err ? *err : local_error, llvm::sys::fs::F_None);
#else
    std::string local_error;
    llvm::raw_fd_ostream out (filename, err ? *err : local_error);
#endif
    llvm::WriteBitcodeToFile (module(), out);

#if OSL_LLVM_VERSION >= 36
    if (err && local_error)
        *err = local_error.message ();
#endif
}



std::string
LLVM_Util::bitcode_string (llvm::Function *func)
{
    std::string s;
    llvm::raw_string_ostream stream (s);
    stream << (*func);
    return stream.str();
}

std::string
LLVM_Util::module_string ()
{
   std::string s;
   llvm::raw_string_ostream stream (s);
   m_llvm_module->print(stream,nullptr);
   return s;
}

void
LLVM_Util::delete_func_body (llvm::Function *func)
{
    func->deleteBody ();
}



bool
LLVM_Util::func_is_empty (llvm::Function *func)
{
    return func->size() == 1 // func has just one basic block
        && func->front().size() == 1;  // the block has one instruction,
                                       ///   presumably the ret
}


std::string
LLVM_Util::func_name (llvm::Function *func)
{
    return func->getName().str();
}

llvm::DIFile *
LLVM_Util::getOrCreateDebugFileFor(const std::string &file_name) {
    auto iter = mDebugFileByName.find(file_name);
    if(iter == mDebugFileByName.end()) {
        //OSL_DEV_ONLY(std::cout << ">>>>>>>>>>>>>>>>>>>>>>>>CREATING FILE<<<<<<<<<<<<<<<<<<<<<<<<< " << file_name << std::endl);
        ASSERT(m_llvm_debug_builder != nullptr);
        llvm::DIFile *file = m_llvm_debug_builder->createFile(
                file_name, ".\\");
        mDebugFileByName.insert(std::make_pair(file_name,file));
        return file;
    }
    return iter->second;
}

llvm::DIScope *
LLVM_Util::getCurrentDebugScope() const
{
    ASSERT(mDebugCU != nullptr);

    if (mLexicalBlocks.empty()) {
        return mDebugCU;
    } else {
        return mLexicalBlocks.back();
    }
}

llvm::DILocation *
LLVM_Util::getCurrentInliningSite() const
{
    if (mInliningSites.empty()) {
        return nullptr;
    } else {
        return mInliningSites.back();
    }
}

}; // namespace pvt
OSL_NAMESPACE_EXIT
