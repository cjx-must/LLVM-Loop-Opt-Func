#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;

// ---- Per-function diff result (from func.cpp) ----

struct FuncDiff {
    std::string name;
    int origBBs = 0, origInsts = 0;
    int origNEBBS = 0, origNEInsts = 0;
    int origLoops = 0, newLoops = 0;
    int addedBBs = 0, removedBBs = 0, modifiedBBs = 0;
    int addedInsts = 0, removedInsts = 0, modifiedInsts = 0;
    int neBB_added = 0, neBB_removed = 0, neBB_modified = 0;
    int neInst_added = 0, neInst_removed = 0, neInst_modified = 0;
    int NEaddedBBs = 0, NEremovedBBs = 0;
    bool destroyed = false;
};

// ---- Myers diff (from func.cpp) ----

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

// ---- Instruction helpers (from func.cpp) ----

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

// ---- BB / Loop helpers (from func.cpp) ----

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

// ---- nameUnnamedValues (from func.cpp) ----

static void nameUnnamedValues(Module &M) {
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

// ---- diffFunc (from func.cpp) ----

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

// ---- Output ----

static void printFuncDiff(const FuncDiff &d) {
    fprintf(stdout, "  %s  L(%d->%d) BB(%d +%d -%d ~%d) Inst(%d +%d -%d ~%d)"
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

// ---- main ----

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage:\n");
        fprintf(stderr, "  ir-diff <func_name> <old.ll> <new.ll>\n");
        fprintf(stderr, "  ir-diff <func_name>  (reads two IRs from stdin, separated by '---')\n");
        return 1;
    }

    const char *funcName = argv[1];

    LLVMContext Context;
    SMDiagnostic Err;

    std::unique_ptr<Module> oldMod, newMod;

    if (argc >= 4) {
        // 从文件读取
        oldMod = parseIRFile(argv[2], Err, Context);
        if (!oldMod) { Err.print("ir-diff", errs()); return 1; }
        newMod = parseIRFile(argv[3], Err, Context);
        if (!newMod) { Err.print("ir-diff", errs()); return 1; }
    } else {
        // 从 stdin 读取，用 "---" 分隔两段 IR
        std::string input;
        {
            char buf[4096];
            while (fgets(buf, sizeof(buf), stdin))
                input += buf;
        }

        // 查找分隔符 "---"（独占一行）
        std::string delim = "---\n";
        size_t pos = input.find(delim);
        if (pos == std::string::npos) {
            fprintf(stderr, "ir-diff: delimiter '---' not found in stdin\n");
            return 1;
        }

        std::string oldIR = input.substr(0, pos);
        std::string newIR = input.substr(pos + delim.size());

        auto oldBuf = MemoryBuffer::getMemBuffer(oldIR, "old", false);
        oldMod = parseIR(*oldBuf, Err, Context);
        if (!oldMod) { Err.print("ir-diff", errs()); return 1; }

        auto newBuf = MemoryBuffer::getMemBuffer(newIR, "new", false);
        newMod = parseIR(*newBuf, Err, Context);
        if (!newMod) { Err.print("ir-diff", errs()); return 1; }
    }

    // 给匿名值命名，确保 BB 可以按名字匹配
    nameUnnamedValues(*oldMod);
    nameUnnamedValues(*newMod);

    const Function *oldF = oldMod->getFunction(funcName);
    const Function *newF = newMod->getFunction(funcName);

    if (!oldF && !newF) {
        fprintf(stderr, "ir-diff: function '%s' not found in either module\n", funcName);
        return 1;
    }

    FuncDiff d = diffFunc(oldF, newF);
    printFuncDiff(d);

    return 0;
}
