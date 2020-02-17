/** 
 * Lower global variable initializers into explicit initialization
 * code added inserted in the entry block of main. 
 * 
 * The initialization code mostly consists of a sequence of Store
 * instructions. However, ConstantAggregateZero are treated specially.
 *
 * Given a global variable with initializer like @g:
 * 
 *   %struct.gstate = type { i32, i32, [10 x i32] }
 *   @g = internal global %struct.gstate zeroinitializer, align 4
 * 
 * This option inserts in the entry block of main :
 * 
 * define i32 @main() {
 *   %_1 = getelementptr %struct.gstate* @g, i32 0, i32 0
 *   call void @verifier.zero_initializer.1(i32* %_1)
 *   %_2 = getelementptr %struct.gstate* @g, i32 0, i32 1
 *   call void @verifier.zero_initializer.1(i32* %_2)
 *   %_3 = getelementptr %struct.gstate* @g, i32 0, i32 2
 *   call void @verifier.zero_initializer.2([10 x i32]* _3)
 *   ...
 * }
 *
 * The reason to treat specially ConstantAggregateZero is to be able
 * to model it in a concise way via special functions (understood by
 * the analyzer) to avoid having too many Store instructions.
 */

#include "llvm/Pass.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/GlobalStatus.h"

#include "clam/Support/Boost.hh"
#include "boost/range.hpp"
#include "boost/format.hpp"

using namespace llvm;

//#define LOWERGV_LOG(...) __VA_ARGS__
#define LOWERGV_LOG(...)

namespace clam {

  class LowerGvInitializers : public ModulePass {
    
    static char ID;

    // for debugging only
    void printIndices(std::vector<APInt> &Indices) {
      errs() << "[";
      for(unsigned i=0,e=Indices.size();i<e;) {
	errs() << Indices[i];
	++i;
	if (i < e) {
	  errs() << ",";
	}
      }
      errs() << "]";
    }
    
    SmallVector<Value*, 8> CreateGEPIndices(const std::vector<APInt> &NIndices){
      SmallVector<Value*, 8> Indices;
      for(unsigned i=0; i< NIndices.size(); i++){
	const APInt &Idx = NIndices[i];
	IntegerType* ITy = IntegerType::get(*m_ctx, Idx.getBitWidth());	
	Indices.push_back(ConstantInt::get(ITy, Idx.getZExtValue()));
      }
      return Indices;
    }

    void CreateStoreIntValue(Value &Base,
			     const std::vector<APInt> &Indices,
			     const ConstantInt &Val,
			     IRBuilder<> &Builder) {
      Value *Ptr = Builder.CreateInBoundsGEP(&Base, CreateGEPIndices(Indices));
      LOWERGV_LOG(errs() << "Creating GEP " << *Ptr << "\n";);
      Builder.CreateAlignedStore(const_cast<ConstantInt*>(&Val), Ptr,
       				 m_dl->getABITypeAlignment(Val.getType()));
    }
    
    Constant* getInitFn(Type *type, std::vector<Constant*> &LLVMUsed, Module &m) {
      Constant* res = m_initfn[type];
      if (res == NULL) {
	res = m.getOrInsertFunction 
	  (boost::str 
	   (boost::format ("verifier.zero_initializer.%d") % m_initfn.size ()), 
	   m_voidty, type);
	m_initfn[type] = res;

	Type *i8PTy = Type::getInt8PtrTy(m.getContext ());
	LLVMUsed.push_back(ConstantExpr::getBitCast(res, i8PTy));
      }
      return res;
    }
        
    Function* CreateZeroInitializerFunction(Value * V,
					    std::vector<Constant*> &LLVMUsed,
					    Module &M) {
      AttrBuilder AB;
      Function* fun = dyn_cast<Function>(getInitFn(V->getType(), LLVMUsed, M));
      // XXX: do not mark it as ReadNone, otherwise LLVM will optimize
      // it away.
      //fun->addFnAttr(Attribute::ReadNone);
      if (m_cg) m_cg->getOrInsertFunction(fun);
      return fun;
    }

    void CreateZeroInitializerCallSite(Value &base, const std::vector<APInt> &Indices,  
				       IRBuilder<> &Builder,
				       std::vector<Constant*> &LLVMUsed,
				       Module &M) {
      Value *ptr = Builder.CreateInBoundsGEP(&base, CreateGEPIndices(Indices));
      Function *f = CreateZeroInitializerFunction(ptr, LLVMUsed, M);
      Builder.CreateCall(f, ptr);
    }

    Constant* getIntInitFn(Type *ty,std::vector<Constant*> &LLVMUsed, Module &m) {
      assert(ty->isIntegerTy());
      
      Constant* res = m_initfn[ty];
      if (res == NULL) {
	res = m.getOrInsertFunction 
	  (boost::str 
	   (boost::format ("verifier.int_initializer.%d") % m_initfn.size ()), 
	   m_voidty, ty->getPointerTo(), ty);
	m_initfn[ty] = res;
	Type *i8PTy = Type::getInt8PtrTy(m.getContext ());
	LLVMUsed.push_back(ConstantExpr::getBitCast(res, i8PTy));
      }
      return res;
    }
    
    Function* CreateIntInitializerFunction(GlobalVariable &gv,
					   std::vector<Constant*> &LLVMUsed,
					   Module &M) {      
      AttrBuilder AB;
      Function* fun = dyn_cast<Function>(getIntInitFn(gv.getInitializer()->getType(),
						      LLVMUsed, M));
      // XXX: do not mark it as ReadNone, otherwise LLVM will optimize
      // it away.
      //fun->addFnAttr(Attribute::ReadNone);
      if (m_cg) m_cg->getOrInsertFunction(fun);
      return fun;
    }    
    void CreateIntInitializerCallSite(GlobalVariable &gv,
				      IRBuilder<> &Builder,				      
				      std::vector<Constant*> &LLVMUsed,
				      Module &M) {

      assert(gv.hasInitializer() && "global without initializer");
      assert(gv.getInitializer()->getType()->isIntegerTy());
      
      Function *intfn = CreateIntInitializerFunction(gv, LLVMUsed, M);
      Builder.CreateCall(intfn, {&gv, gv.getInitializer()});
    }

    bool LowerConstantAggregateZero(Type* T, Value &base,
				    IRBuilder<> &Builder, Module &M,
				    std::vector<Constant*> &LLVMUsed,
				    std::vector<APInt> &Indices) {
      bool change = false;
      LOWERGV_LOG(errs () << "GEP " << base.getName() << " indices=";
		  printIndices(Indices);
		  errs () << "\nTYPE=" << *T << "\n";);

      if (IntegerType *ITy = dyn_cast<IntegerType>(T)) {
	// XXX: we choose to add both the special initialization
	// function and the lowered Store instruction.
	CreateZeroInitializerCallSite(base, Indices, Builder, LLVMUsed, M);
	ConstantInt* Zero = ConstantInt::get(ITy, 0);
	CreateStoreIntValue(base, Indices, *Zero, Builder);
	change = true;
      } else if (StructType *STy = dyn_cast<StructType> (T)) {
	for (unsigned i=0; i < STy->getNumElements(); ++i) {
	  Type* ETy = STy->getElementType(i);
	  uint64_t ElementSize = m_dl->getTypeAllocSize(ETy);
	  Indices.push_back(APInt(ElementSize*8, i));
	  change |= LowerConstantAggregateZero(ETy, base, Builder, M, LLVMUsed, Indices);
	  Indices.pop_back();
	}
      } else if (ArrayType *ATy = dyn_cast<ArrayType> (T)) {
	// XXX: we don't lower the array into individual stores.  we
	//      add instead a special initialization function that the
	//      analyzer can understand and hopefully be more
	//      efficient.

	#if 0
	for (unsigned i=0; i < ATy->getNumElements(); ++i){
	  uint64_t ElementSize = m_dl->getTypeAllocSize(ATy->getElementType());
	  Indices.push_back(APInt(ElementSize*8, i));
	  change |= LowerConstantAggregateZero(ATy->getElementType(), base, Builder, M,
					       LLVMUsed, Indices);
	  Indices.pop_back();	      
	}
	#endif
	
	if (ATy->getElementType()->isIntegerTy()) {
	  CreateZeroInitializerCallSite(base, Indices, Builder, LLVMUsed, M);
	  change = true;
	}
      } else {
	// ignore the rest of types
      }

      return change;
    }

    
    bool LowerInitializer(const Constant *C, Value &Base, IRBuilder<> &Builder,
			  Module &M, std::vector<Constant*> &LLVMUsed,
    			  std::vector<APInt> &Indices) {
      LOWERGV_LOG(errs() << "Lowering " << *C << " indices=";
		  printIndices(Indices);
		  errs() << "\n";);
      bool change = false;      
      if (isa<ConstantPointerNull>(C) ||
	  isa<ConstantFP>(C) ||
	  isa<UndefValue>(C)) {
	// ignore these cases
      } else if (const ConstantDataSequential *CDS = dyn_cast<ConstantDataSequential>(C)) {
	// ignore C strings
	if (!(CDS->isString() || CDS->isCString())) {
	  uint64_t ElementSize = CDS->getElementByteSize();
	  LOWERGV_LOG(errs() << "\tCDS element size=" 
		      << ElementSize << " num elements=" << CDS->getNumElements() << "\n";);
	  for (unsigned i=0, e = CDS->getNumElements(); i < e; ++i) {
	    APInt Index(ElementSize*8, i);
	    Indices.push_back(Index);
	    change |= LowerInitializer(CDS->getElementAsConstant(i), Base,
				       Builder, M, LLVMUsed, Indices);
	  }
	}
      } else if (const ConstantVector *CP = dyn_cast<ConstantVector>(C)) { 
    	unsigned ElementSize = m_dl->getTypeAllocSize(CP->getType()->getElementType());
	LOWERGV_LOG(errs() << "\tCV element size=" 
		    << ElementSize << " num elements=" << CP->getNumOperands() << "\n";);
	
    	for (unsigned i = 0, e = CP->getNumOperands(); i < e; ++i) {
	  APInt Index(ElementSize*8, i);
    	  Indices.push_back(Index);
    	  change |= LowerInitializer(CP->getOperand(i), Base, Builder, M, LLVMUsed, Indices);
    	}
      } else if (isa<ConstantAggregateZero>(C)) {
	change |= LowerConstantAggregateZero(C->getType(), Base, Builder, M, LLVMUsed, Indices);
      } else if (const ConstantArray *CPA = dyn_cast<ConstantArray>(C)) {
    	unsigned ElementSize = m_dl->getTypeAllocSize(CPA->getType()->getElementType());
	LOWERGV_LOG(errs() << "\tCA element size=" 
		    << ElementSize << " num elements=" << CPA->getNumOperands() << "\n";);
    	for (unsigned i = 0, e = CPA->getNumOperands(); i < e; ++i) {
	  APInt Index(ElementSize*8, i);
    	  Indices.push_back(Index);	  
    	  change |= LowerInitializer(CPA->getOperand(i), Base, Builder, M, LLVMUsed, Indices);
    	}
      } else if (const ConstantStruct *CPS = dyn_cast<ConstantStruct>(C)) {
    	const StructLayout *SL = m_dl->getStructLayout(cast<StructType>(CPS->getType()));
	LOWERGV_LOG(errs() << "\tCS\n";);	
    	for (unsigned i = 0, e = CPS->getNumOperands(); i < e; ++i) {
	  APInt Index(32, SL->getElementOffset(i));
    	  Indices.push_back(Index);	  
    	  change |= LowerInitializer(CPS->getOperand(i), Base, Builder, M, LLVMUsed, Indices);
    	}
      } else if (const ConstantInt *CI = dyn_cast<ConstantInt>(C)) {

	LOWERGV_LOG(errs() << "Constant Integer " << *CI << " indices=";
		    printIndices(Indices);
		    errs() << "\n";);
	CreateStoreIntValue(Base, Indices, *CI, Builder);
	change = true;
      }

      Indices.pop_back();      
      return change;
    }
    
    /// C may have non-instruction users. Can all of those users be turned into
    /// instructions?
    static bool allNonInstructionUsersCanBeMadeInstructions(Constant *C) {
      // We don't do this exhaustively. The most common pattern that we really need
      // to care about is a constant GEP or constant bitcast - so just looking
      // through one single ConstantExpr.
      //
      // The set of constants that this function returns true for must be able to be
      // handled by makeAllConstantUsesInstructions.
      for (auto *U : C->users()) {
	if (isa<Instruction>(U))
	continue;
	if (!isa<ConstantExpr>(U))
	  // Non instruction, non-constantexpr user; cannot convert this.
	  return false;
	for (auto *UU : U->users())
	  if (!isa<Instruction>(UU))
	    // A constantexpr used by another constant. We don't try and recurse any
	    // further but just bail out at this point.
	    return false;
      }    
      return true;
    }
    
    /// C may have non-instruction users, and
    /// allNonInstructionUsersCanBeMadeInstructions has returned true. Convert the
    /// non-instruction users to instructions.
    void makeAllConstantUsesInstructions(Constant *C) {
      SmallVector<ConstantExpr*,4> Users;
      for (auto *U : C->users()) {
	if (isa<ConstantExpr>(U))
	  Users.push_back(cast<ConstantExpr>(U));
	else
	  // We should never get here; allNonInstructionUsersCanBeMadeInstructions
	  // should not have returned true for C.
	  assert(
		 isa<Instruction>(U) &&
		 "Can't transform non-constantexpr non-instruction to instruction!");
      }
      
      SmallVector<Value*,4> UUsers;
      for (auto *U : Users) {
	UUsers.clear();
	for (auto *UU : U->users())
	  UUsers.push_back(UU);
	for (auto *UU : UUsers) {
	  Instruction *UI = cast<Instruction>(UU);
	  Instruction *NewU = U->getAsInstruction();
	  NewU->insertBefore(UI);
	  UI->replaceUsesOfWith(U, NewU);
	}
	U->dropAllReferences();
      }
    }
    
    
    /** map for initializer functions */
    DenseMap<const Type*, Constant*> m_initfn;
    /** void type **/
    Type *m_voidty;
    /** gep index types **/
    IntegerType* m_intptrty;
    IntegerType* m_intty;    
    /** callgraph **/
    CallGraph *m_cg;
    const DataLayout *m_dl;
    LLVMContext *m_ctx;
    
  public:
    
    LowerGvInitializers () : ModulePass (ID) {}
    
    virtual bool runOnModule (Module &M) {
      m_dl = &M.getDataLayout();
      CallGraphWrapperPass *cgwp = getAnalysisIfAvailable<CallGraphWrapperPass> ();
      m_cg = cgwp ? &cgwp->getCallGraph () : nullptr;
      m_voidty = Type::getVoidTy(M.getContext());
      m_intptrty = cast<IntegerType>(m_dl->getIntPtrType(M.getContext(), 0));
      m_intty = IntegerType::get(M.getContext(), 32);      
      m_ctx = &M.getContext();
      
      Function *f = M.getFunction ("main");
      if (!f) return false;

      std::vector<GlobalVariable*> gvs;
      for (GlobalVariable &gv: llvm::make_range(M.global_begin(), M.global_end())) {
        if (gv.hasInitializer () && gv.getName() != "llvm.used")
	  gvs.push_back(&gv);
      }
      
      if (gvs.empty()) return false;

      /* add our verifier.zero_initializer and
	 verifier.int_initializer functions to llvm used to avoid them
	 to be optimized away by LLVM.
      */
      GlobalVariable *LLVMUsed = M.getGlobalVariable("llvm.used");
      std::vector<Constant*> MergedVars;
      if (LLVMUsed && LLVMUsed->hasInitializer()) {
      	ConstantArray *Inits = cast<ConstantArray>(LLVMUsed->getInitializer());
      	for (unsigned I = 0, E = Inits->getNumOperands(); I != E; ++I) {
	  MergedVars.push_back(Inits->getOperand(I));
      	}
      	LLVMUsed->eraseFromParent();
      }
      
      IRBuilder<> Builder(M.getContext());
      Builder.SetInsertPoint(&f->getEntryBlock(), f->getEntryBlock().begin ());
      bool change=false;
      for (GlobalVariable *gv : gvs) {
	assert(gv->hasInitializer());

	// Trivial case first
	if (isa<ConstantInt>(gv->getInitializer())) {
	  GlobalStatus GS;
	  bool AddressTaken = GlobalStatus::analyzeGlobal(gv, GS);
	  if (!AddressTaken &&
	      !GS.HasMultipleAccessingFunctions &&
	      GS.AccessingFunction &&
	      GS.AccessingFunction->getName() == "main" &&
	      allNonInstructionUsersCanBeMadeInstructions(gv)) {
	    Type *ElemTy = gv->getType()->getElementType();
	    AllocaInst* Alloca = Builder.CreateAlloca(ElemTy, nullptr, gv->getName());
	    Builder.CreateAlignedStore(gv->getInitializer(), Alloca,
				       m_dl->getABITypeAlignment(ElemTy));
	    makeAllConstantUsesInstructions(gv);
	    gv->replaceAllUsesWith(Alloca);
	    gv->eraseFromParent();
	    change = true;
	    continue;
	  }
	}

	// XXX: we choose to add both the special initialization
	// function and the lowered Store instruction.
	if (isa<ConstantInt>(gv->getInitializer())) {
	  CreateIntInitializerCallSite(*gv, Builder, MergedVars, M);
	  change = true;
	}	

	APInt ZeroIdx = APInt(m_intptrty->getBitWidth(), 0);
	std::vector<APInt> Indices = {ZeroIdx};
	change |=
	  LowerInitializer(gv->getInitializer(), *gv, Builder, M, MergedVars, Indices);
      }


      // re-create llvm.used
      if (!MergedVars.empty()) {
	Type *i8PTy = Type::getInt8PtrTy(M.getContext ());      
	ArrayType *ATy = ArrayType::get(i8PTy, MergedVars.size());
	LLVMUsed = new llvm::GlobalVariable(M, ATy, false, llvm::GlobalValue::AppendingLinkage,
					    llvm::ConstantArray::get(ATy, MergedVars),
					    "llvm.used");
	LLVMUsed->setSection("llvm.metadata");
      }
      
      return change;
    }
    
    void getAnalysisUsage (AnalysisUsage &AU) const {
      AU.setPreservesAll ();
      AU.addRequired<llvm::CallGraphWrapperPass>();      
    }

    virtual StringRef getPassName() const {
      return "Clam: Lower global initializers";
    }
    
  };

  char LowerGvInitializers::ID = 0;
  Pass* createLowerGvInitializersPass () { return new LowerGvInitializers (); }  

} 



