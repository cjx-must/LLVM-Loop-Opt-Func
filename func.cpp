#include <llvm/ADT/Any.h>
#include <llvm/Analysis/LazyCallGraph.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/CodeGen/MachineFunction.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalValue.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Cloning.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;

static std::unique_ptr<Module> gBeforeMod;
static const Module *gCurrMod = nullptr;
static std::vector<std::string> gScopeFuncs;
static std::string gIrName;
//static unsigned gPassNumber = 0;

// ---- Per-function diff result ----

struct FuncDiff {
    std::string name;
    int origBBs = 0, origInsts = 0;
    int origNEBBS = 0;
    int origNEInsts = 0;
    int origLoops = 0, newLoops = 0;
    int addedBBs = 0, removedBBs = 0, modifiedBBs = 0;
    int addedInsts = 0, removedInsts = 0, modifiedInsts = 0;
    int neBB_added = 0, neBB_removed = 0, neBB_modified = 0;
    int neInst_added = 0, neInst_removed = 0, neInst_modified = 0;
    int NEaddedBBs = 0, NEremovedBBs = 0;
    bool destroyed = false;
};

// ---- Myers diff ----

enum class DiffOp { Keep, Delete, Insert };

struct EditScript {
    std::vector<DiffOp> ops;
    int added = 0, removed = 0;
};

static EditScript myersDiff(const std::vector<std::string> &a,
                            const std::vector<std::string> &b) {
    int N = (int)a.size(), M = (int)b.size();
    int MAX = N + M, offset = MAX;
    std::vector<int> curV(2 * MAX + 1, 0);
    std::vector<std::map<int, int>> V;

    bool found = false;
    for (int D = 0; D <= MAX && !found; D++) {
        std::map<int, int> row;
        for (int k = -D; k <= D; k += 2) {
            int x;
            if (k == -D || (k != D && curV[offset + k - 1] < curV[offset + k + 1]))
                x = curV[offset + k + 1];
            else
                x = curV[offset + k - 1] + 1;
            int y = x - k;
            while (x < N && y < M && a[x] == b[y]) { x++; y++; }
            curV[offset + k] = x;
            row[k] = x;
            if (x >= N && y >= M) { found = true; break; }
        }
        V.push_back(std::move(row));
    }

    std::vector<DiffOp> ops;
    int x = N, y = M;
    for (int D = (int)V.size() - 1; D > 0; D--) {
        int k = x - y;
        const auto &prev = V[D - 1];
        int prevK;
        bool fromDel;
        if (k == -D || (k != D && prev.at(k - 1) < prev.at(k + 1))) {
            prevK = k + 1; fromDel = false;
        } else {
            prevK = k - 1; fromDel = true;
        }
        int px = prev.at(prevK), py = px - prevK;
        if (fromDel) {
            int sx = px + 1, sy = py;
            while (x > sx && y > sy) { ops.push_back(DiffOp::Keep); x--; y--; }
            ops.push_back(DiffOp::Delete); x--;
        } else {
            int sx = px, sy = py + 1;
            while (x > sx && y > sy) { ops.push_back(DiffOp::Keep); x--; y--; }
            ops.push_back(DiffOp::Insert); y--;
        }
    }
    while (x > 0 && y > 0) { ops.push_back(DiffOp::Keep); x--; y--; }
    std::reverse(ops.begin(), ops.end());

    EditScript es;
    es.ops = std::move(ops);
    for (auto op : es.ops) {
        if (op == DiffOp::Insert) es.added++;
        else if (op == DiffOp::Delete) es.removed++;
    }
    return es;
}

// ---- Instruction helpers ----

static std::string fullInstText(const Instruction &I) {
    std::string s;
    raw_string_ostream OS(s);
    I.print(OS);
    return s;
}

static std::string instText(const Instruction &I) {
    std::string s;
    raw_string_ostream OS(s);
    OS << I.getOpcodeName();
    if (const auto *CI = dyn_cast<ICmpInst>(&I))
        OS << ' ' << CI->getPredicate();
    else if (const auto *CFI = dyn_cast<FCmpInst>(&I))
        OS << ' ' << CFI->getPredicate();
    OS << ' ' << *I.getType();
    for (unsigned i = 0; i < I.getNumOperands(); i++) {
        const Value *Op = I.getOperand(i);
        OS << ' ' << *Op->getType();
        if (const auto *CI = dyn_cast<ConstantInt>(Op))
            OS << ':' << CI->getValue();
        else if (const auto *CFP = dyn_cast<ConstantFP>(Op)) {
            std::string Buf;
            raw_string_ostream BufOS(Buf);
            CFP->getValueAPF().print(BufOS);
            OS << ':' << Buf;
        } else if (isa<ConstantPointerNull>(Op))
            OS << ":null";
        else if (isa<UndefValue>(Op))
            OS << ":undef";
        else if (isa<BlockAddress>(Op))
            OS << ":blockaddr";
        else if (isa<GlobalValue>(Op))
            OS << "@" << Op->getName();
        else if (isa<Constant>(Op))
            OS << ":const";
    }
    return s;
}

// ---- Module / IR helpers ----

static const Module *getModule(const Any &IR) {
    if (auto *M = any_cast<const Module *>(&IR))
        return *M;
    if (auto *F = any_cast<const Function *>(&IR))
        return (*F)->getParent();
    if (auto *L = any_cast<const Loop *>(&IR))
        return (*L)->getHeader()->getModule();
    if (auto *SCC = any_cast<const LazyCallGraph::SCC *>(&IR))
        for (const auto &N : **SCC)
            return N.getFunction().getParent();
    if (auto *MF = any_cast<const MachineFunction *>(&IR))
        return (*MF)->getFunction().getParent();
    return nullptr;
}

// ---- Compare two versions of a function ----

static bool isEmptyInst(const Instruction &I) {
    if (isa<PHINode>(&I)) return true;
    if (isa<UnreachableInst>(&I)) return true;
    if (const auto *BI = dyn_cast<BranchInst>(&I))
        return BI->isUnconditional();
    return false;
}

static bool isEmptyBB(const BasicBlock &BB) {
    for (const auto &I : BB)
        if (!isEmptyInst(I))
            return false;
    return true;
}

static int countNonEmptyInsts(const BasicBlock &BB) {
    int n = 0;
    for (const auto &I : BB)
        if (!isEmptyInst(I)) n++;
    return n;
}

static int countLoops(Function &F) {
    if (F.isDeclaration()) return 0;
    DominatorTree DT(F);
    LoopInfo LI(DT);
    return (int)LI.getLoopsInPreorder().size();
}

static FuncDiff diffFunc(const Function *oldF, const Function *newF) {
    FuncDiff d;
    if (newF) d.name = newF->getName().str();
    else if (oldF) { d.name = oldF->getName().str(); d.destroyed = true; }

    if (!oldF && newF) {
        for (const auto &BB : *newF) {
            d.addedBBs++; d.addedInsts += BB.size();
            if (!isEmptyBB(BB)) { d.neBB_added++; d.NEaddedBBs++; }
            d.neInst_added += countNonEmptyInsts(BB);
        }
        d.newLoops = countLoops(const_cast<Function &>(*newF));
        return d;
    }
    if (oldF && !newF) {
        d.origBBs = oldF->size();
        for (const auto &BB : *oldF) {
            d.removedBBs++; d.removedInsts += BB.size(); d.origInsts += BB.size();
            if (!isEmptyBB(BB)) { d.neBB_removed++; d.origNEBBS++; d.NEremovedBBs++; }
            int ne = countNonEmptyInsts(BB);
            d.neInst_removed += ne; d.origNEInsts += ne;
        }
        d.origLoops = countLoops(const_cast<Function &>(*oldF));
        return d;
    }
    if (!oldF || !newF) return d;

    d.origBBs = oldF->size();
    for (const auto &BB : *oldF) {
        d.origInsts += BB.size();
        if (!isEmptyBB(BB)) d.origNEBBS++;
        d.origNEInsts += countNonEmptyInsts(BB);
    }
    d.origLoops = countLoops(const_cast<Function &>(*oldF));
    d.newLoops = countLoops(const_cast<Function &>(*newF));

    std::map<std::string, const BasicBlock *> oldMap, newMap;
    for (const auto &BB : *oldF) oldMap[BB.getName().str()] = &BB;
    for (const auto &BB : *newF) newMap[BB.getName().str()] = &BB;

    for (auto &p : newMap)
        if (!oldMap.count(p.first)) {
            d.addedBBs++; d.addedInsts += p.second->size();
            if (!isEmptyBB(*p.second)) { d.neBB_added++; d.NEaddedBBs++; }
            d.neInst_added += countNonEmptyInsts(*p.second);
        }
    for (auto &p : oldMap)
        if (!newMap.count(p.first)) {
            d.removedBBs++; d.removedInsts += p.second->size();
            if (!isEmptyBB(*p.second)) { d.neBB_removed++; d.NEremovedBBs++; }
            d.neInst_removed += countNonEmptyInsts(*p.second);
        }

    for (auto &p : oldMap) {
        auto it = newMap.find(p.first);
        if (it == newMap.end()) continue;
        const BasicBlock *oldBB = p.second, *newBB = it->second;

        std::vector<std::string> oldSigs, newSigs;
        for (const auto &I : *oldBB) oldSigs.push_back(instText(I));
        for (const auto &I : *newBB) newSigs.push_back(instText(I));

        EditScript es = myersDiff(oldSigs, newSigs);
        d.addedInsts += es.added;
        d.removedInsts += es.removed;

        bool bbChanged = (es.added > 0 || es.removed > 0);
        bool neBBChanged = false;
        auto oit = oldBB->begin(), nit = newBB->begin();
        for (auto op : es.ops) {
            if (op == DiffOp::Keep) {
                if (fullInstText(*oit) != fullInstText(*nit)) {
                    d.modifiedInsts++; bbChanged = true;
                    bool oldInstNonEmpty = !isEmptyInst(*oit), newInstNonEmpty = !isEmptyInst(*nit);
                    if (oldInstNonEmpty && newInstNonEmpty) { d.neInst_modified++; neBBChanged = true; }
                    if (!oldInstNonEmpty && newInstNonEmpty) d.neInst_added++;
                    if (oldInstNonEmpty && !newInstNonEmpty) d.neInst_removed++;
                }
                ++oit; ++nit;
            } else if (op == DiffOp::Delete) {
                if (!isEmptyInst(*oit)) { d.neInst_removed++; neBBChanged = true; }
                ++oit;
            } else {
                if (!isEmptyInst(*nit)) { d.neInst_added++; neBBChanged = true; }
                ++nit;
            }
        }
        
        if (bbChanged) d.modifiedBBs++;
        
        bool oldBBNonEmpty = !isEmptyBB(*oldBB), newBBNonEmpty = !isEmptyBB(*newBB);
        if (!oldBBNonEmpty && newBBNonEmpty) d.neBB_added++;
        if (oldBBNonEmpty && !newBBNonEmpty) d.neBB_removed++;
        if (oldBBNonEmpty && newBBNonEmpty && neBBChanged) d.neBB_modified++;

    }
    return d;
}

// ---- Output helpers ----

static void printFuncLine(const char *PassName, const FuncDiff &d) {
    fprintf(stderr, "  %s  L(%d->%d) BB(%d +%d -%d ~%d) Inst(%d +%d -%d ~%d)"
                    " NBB(%d +%d -%d ~%d) NI(%d +%d -%d ~%d) NEBB(+%d,-%d)%s\n",
            d.name.c_str(),
            d.origLoops, d.newLoops,
            d.origBBs, d.addedBBs, d.removedBBs, d.modifiedBBs,
            d.origInsts, d.addedInsts, d.removedInsts, d.modifiedInsts,
            d.origNEBBS,
            d.neBB_added, d.neBB_removed, d.neBB_modified,
            d.origNEInsts,
            d.neInst_added, d.neInst_removed, d.neInst_modified,
            d.NEaddedBBs, d.NEremovedBBs,
            d.destroyed ? " [DESTROYED]" : "");
}

// ---- Public interface ----

static bool isAdaptorPass(StringRef PassName) {
    return PassName.contains("PassManager<") ||
           PassName.contains("Adaptor") ||
           PassName.contains("Wrapper") ||
           PassName == "DevirtSCCRepeatedPass" ;
}

void nameUnnamedValues(Module &M) {
    static unsigned long long bbCnt = 0, instCnt = 0;
    for (Function &F : M) {
        if (F.isDeclaration()) continue;
        for (BasicBlock &BB : F)
            if (!BB.hasName()) BB.setName("bb_" + Twine(bbCnt++));
        for (BasicBlock &BB : F)
            for (Instruction &I : BB)
                if (!I.hasName() && !I.getType()->isVoidTy())
                    I.setName("v_" + Twine(instCnt++));
    }
}

void saveBeforeIR(const char *PassName, const Any &IR) {
    if (isAdaptorPass(PassName)) {
        // fprintf(stderr, "[%s] Adaptor\n", PassName);
        return;
    }

    const Module *M = getModule(IR);
    if (!M) return;

    gCurrMod = M;
    gScopeFuncs.clear();
    std::string irName;

    if (any_cast<const Module *>(&IR)) {
        irName = "Module";
        for (const auto &F : *M)
            if (!F.isDeclaration()) gScopeFuncs.push_back(F.getName().str());
    } else if (auto *FA = any_cast<const Function *>(&IR)) {
        irName = "function:" + (*FA)->getName().str();
        if (!(*FA)->isDeclaration()) gScopeFuncs.push_back((*FA)->getName().str());
    } else if (auto *LA = any_cast<const Loop *>(&IR)) {
        const Function *F = (*LA)->getHeader()->getParent();
        std::string hdr;
        raw_string_ostream OS(hdr);
        (*LA)->getHeader()->printAsOperand(OS, false);
        irName = "loop:" + hdr + " function:" + F->getName().str();
        gScopeFuncs.push_back(F->getName().str());
    } else if (auto *SA = any_cast<const LazyCallGraph::SCC *>(&IR)) {
        irName = "scc:";
        bool first = true;
        for (const auto &N : **SA) {
            if (!N.getFunction().isDeclaration()) {
                if (!first) irName += ",";
                irName += N.getFunction().getName().str();
                first = false;
                gScopeFuncs.push_back(N.getFunction().getName().str());
            }
        }
    } else if (auto *MF = any_cast<const MachineFunction *>(&IR)) {
        const Function &F = (*MF)->getFunction();
        irName = "machine:" + F.getName().str();
        if (!F.isDeclaration()) gScopeFuncs.push_back(F.getName().str());
    }

    gIrName = std::move(irName);

    nameUnnamedValues(const_cast<Module &>(*M));
    gBeforeMod = CloneModule(*M);
}

void diffAfterIR(const char *PassName, const Any &IR) {
    if (isAdaptorPass(PassName)) {
        // fprintf(stderr, "[%s] Adaptor done\n", PassName);
        return;
    }

    fprintf(stderr, "[%s]", PassName);
    if (!gIrName.empty()) fprintf(stderr, " %s", gIrName.c_str());
    //fprintf(stderr, " #%u", gPassNumber++);
    if (!gBeforeMod) {
        fprintf(stderr, " Error No before IR at non-Adaptor pass\n");
        return;
    }
    if (!gCurrMod) {
        fprintf(stderr, " Error No current IR at non-Adaptor pass\n");
        return;
    }
    if (StringRef(gIrName).starts_with("machine:")) {
        fprintf(stderr, " skip MachineFunction pass\n");
        return;
    }
    fprintf(stderr, "\n");

    nameUnnamedValues(const_cast<Module &>(*gCurrMod));

    for (auto &fn : gScopeFuncs) {
        const Function *oldF = gBeforeMod->getFunction(fn);
        const Function *newF = gCurrMod->getFunction(fn);
        if (!oldF) {
            fprintf(stderr, "  %s  Error Function not found in before\n", fn.c_str());
            continue;
        }
        printFuncLine(PassName, diffFunc(oldF, newF));
    }

    gBeforeMod.reset();
    gScopeFuncs.clear();
}

void diffAfterIRInvalidated(const char *PassName) {
    if (isAdaptorPass(PassName)) {
        // fprintf(stderr, "[%s] Adaptor done\n", PassName);
        return;
    }

    fprintf(stderr, "[%s]", PassName);
    if (!gIrName.empty()) fprintf(stderr, " %s", gIrName.c_str());
    fprintf(stderr, " [INVALIDATED]");
    //fprintf(stderr, " #%u", gPassNumber++);
    if (!gBeforeMod) {
        fprintf(stderr, " Error No before IR at non-Adaptor pass\n");
        return;
    }
    if (!gCurrMod) {
        fprintf(stderr, " Error No current IR at non-Adaptor pass\n");
        return;
    }
    if (StringRef(gIrName).starts_with("machine:")) {
        fprintf(stderr, " skip MachineFunction pass\n");
        return;
    }
    fprintf(stderr, "\n");

    nameUnnamedValues(const_cast<Module &>(*gCurrMod));

    for (auto &fn : gScopeFuncs) {
        const Function *oldF = gBeforeMod->getFunction(fn);
        const Function *newF = gCurrMod->getFunction(fn);
        if (!oldF) {
            fprintf(stderr, "  %s  Error Function not found in before\n", fn.c_str());
            continue;
        }
        printFuncLine(PassName, diffFunc(oldF, newF));
    }

    gBeforeMod.reset();
    gScopeFuncs.clear();
}
